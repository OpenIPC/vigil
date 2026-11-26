/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hal_sigmastar.c — SigmaStar infinity2m NVRs (SSR621Q), on the i2m NVR
 * SDK's MI libraries (2020-03).
 *
 * Per channel i:   MI_VDEC chn k -> MI_DISP layer 0, input port i (tile i)
 *
 * with k the lowest MI_VDEC channel free when the decoder opens. The core
 * decodes only the channels on screen (dec_shown_only): 16 tiles, but the
 * media heap does not hold 16 decoders of every size, and mi_vdec may take
 * only 4 (below).
 *
 * The display drives HDMI and VGA from the same timing. The SDK has no
 * mi_hdmi: the HDMI transmitter is opensigmastar's open_hdmitx.ko, switched
 * on through open_hdmi.ko's sysfs file once the display runs.
 *
 * The picture goes into a window fitted inside the tile at its aspect ratio
 * (the rest of the tile shows the layer's black background). The decoder
 * outputs it at the window's size and follows the tile when it changes:
 * the display enlarges pictures but does not shrink them (out_size).
 *
 * mi_disp needs the panel timings, PQ tables and memory map from /config,
 * and mi_vdec its firmware from /config/vdec_fw (paths built into the
 * modules); OpenIPC's sigmastar-osdrv-infinity2m installs them.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "mi_common_datatype.h"
#include "mi_sys.h"
#include "mi_vdec.h"
#include "mi_disp.h"

#include "hal.h"
#include "hal_capture.h"
#include "hal_decoder.h"
#include "hal_display.h"

/*
 * Channels: video layer 0 has 16 input ports (MI_DISP_INPUTPORT_MAX; the
 * SDK's display test binds layer 0's port n as DISP channel n, 0..15).
 * MI_VDEC channel numbers are handed out separately, as decoders open: the
 * SDK's mi_vdec takes 32 when the chip's efuse UUID reads zero and only 4
 * when it does not (_MI_VDEC_CreateDevice; its /proc file's MaxChnNum), so
 * with 16 cameras some may have to wait for a number (docs/platforms).
 */
#define MAX_CH          16
#define VDEC_IDS        32          /* the most mi_vdec ever takes */
_Static_assert(MAX_CH <= MI_DISP_INPUTPORT_MAX, "one input port per channel");
#define DISP_DEV        0
#define DISP_LAYER      0
/*
 * The bitstream buffer holds one access unit, which MI_VDEC takes whole.
 * Sized as the SDK's decoder samples do it, by the picture: 512 KiB up to
 * D1, 1 MiB up to 1080p, 2 MiB above. A keyframe that does not fit makes
 * the decoder again with a buffer that does, up to the 4 MiB the RTSP and
 * NETIP sources pass.
 */
#define VDEC_BUF_MIN    (512u << 10)
#define VDEC_BUF_MAX    (4u << 20)
/*
 * Decoders take their buffers from the media heap (mma_heap= in the
 * bootargs, 160 MiB on 256 MiB boards), and MI_VDEC allocates the frame
 * buffers only when the first picture decodes, after MI_VDEC_CreateChn
 * has succeeded. So vigild keeps its own account: what was free with no
 * decoder open, less a reserve (the VPU's common buffer, 1.7 MiB, comes
 * with the first decoder), and an estimate per decoder (dec_footprint).
 */
#define MMA_PROC        "/proc/mi_modules/mi_sys_mma/mma_heap_name0"
#define MMA_RESERVE     (4ull << 20)
#define SEND_TIMEOUT_MS 20
#define SEND_TRIES      5           /* ~100 ms while the decoder catches up */
#define HDMI_SYSFS      "/sys/kernel/open_hdmi/output"
#define REOPEN_SECS     10          /* retry a decoder lost in a reset */
/* A decoder taking over from another goes on screen after this many
 * pictures, or after this long: in emulation an MI_VDEC channel bound to
 * nothing decodes nothing, so it waits for the display; the old decoder's
 * last picture stays on the port until the new one's first. */
#define TAKEOVER_PICS   2
#define TAKEOVER_MS     500
#define PORT_WAIT_MS    10000       /* the old picture at most this long */
#define PORT_CHECK_MS   20
#define DISP_PROC       "/proc/mi_modules/mi_disp/mi_disp0"

static const hal_caps_t g_caps = {
    .name = "sigmastar", .display = 1, .decoder = 1,
    /* the i2m VPU: up to 5 MP (2592x1944) H.264 and H.265 */
    .dec_max_w = 2592, .dec_max_h = 2592,
    .max_channels = MAX_CH,
    /* the heap holds 16 decoders of 720p, fewer of larger pictures, and
     * mi_vdec may take only 4 (above) */
    .dec_shown_only = 1,
};

static struct {
    vg_codec_t       codec;         /* VG_CODEC_NONE: no decoder */
    int              vdec;          /* its MI_VDEC channel */
    unsigned long long mem;         /* its share of the heap (dec_footprint) */
    vg_stream_info_t si;            /* the stream it was opened for */
    unsigned         buf_size;      /* its bitstream buffer */
    vg_rect_t        tile;          /* w == 0: not placed */
    unsigned         src_w, src_h;  /* what the decoder outputs */
    int              out_small;     /* the tile wants more: a new decoder at a keyframe */
    /* After a takeover the port keeps the old decoder's picture, and its
     * size (port_w x port_h), until it has a picture of the new one's. */
    int              port_wait;
    unsigned         port_w, port_h;
    struct timespec  port_checked, switched;
    struct {                        /* a decoder taking over, not on screen yet */
        int              on;
        int              vdec;
        vg_codec_t       codec;
        vg_stream_info_t si;
        unsigned         buf, src_w, src_h;
        unsigned long long mem;
        struct timespec  since;
    } nx;
    int              too_big;       /* an AU over buf_size was reported */
    int              refused;       /* a refusal was reported (until one opens) */
    int              lost;          /* gone in a reset; reopened at a keyframe */
    vg_codec_t       lost_codec;
    unsigned         lost_buf;
    time_t           lost_at;
} g_ch[MAX_CH];
static unsigned g_w, g_h;
static unsigned char g_vdec_busy[VDEC_IDS];
static unsigned g_vdec_freed[VDEC_IDS], g_vdec_frees;   /* when each was last freed */
static int g_vdec_ids = VDEC_IDS;   /* lowered to what mi_vdec takes */
static int g_open;                  /* decoders open */
static int g_mem_known;             /* MMA_PROC could be read */
static unsigned long long g_mem_budget, g_mem_used;
/* MI object create/destroy and port changes, across channel threads */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

const hal_caps_t *hal_caps(void) { return &g_caps; }
static void mem_baseline(void);

static unsigned align_down(unsigned v, unsigned a) { return v / a * a; }
static unsigned align_up(unsigned v, unsigned a) { return (v + a - 1) / a * a; }
static unsigned umin(unsigned a, unsigned b) { return a < b ? a : b; }
static unsigned umax(unsigned a, unsigned b) { return a > b ? a : b; }

/* The transmitter at the timing the display scans out. Without open_hdmi
 * only VGA has a picture, so this warns but does not fail. */
static void hdmi_output(const char *timing)
{
    FILE *f = fopen(HDMI_SYSFS, "w");
    if (!f) {
        fprintf(stderr, "sigmastar: %s missing: no HDMI output (VGA only)\n", HDMI_SYSFS);
        return;
    }
    if (fputs(timing, f) < 0 || fclose(f) != 0)
        fprintf(stderr, "sigmastar: HDMI %s failed\n", timing);
}

int hal_init(unsigned w, unsigned h, unsigned fps)
{
    (void)fps;                                  /* 60 Hz */
    MI_DISP_OutputTiming_e sync;
    const char *timing;
    if (w == 1920 && h == 1080) {
        sync = E_MI_DISP_OUTPUT_1080P60;
        timing = "1080p60";
    } else if (w == 1280 && h == 720) {
        sync = E_MI_DISP_OUTPUT_720P60;
        timing = "720p60";
    } else {
        fprintf(stderr, "sigmastar: no output timing for %ux%u\n", w, h);
        return -1;
    }
    MI_S32 rc;
    if ((rc = MI_SYS_Init()) != MI_SUCCESS) {
        fprintf(stderr, "sigmastar: MI_SYS_Init 0x%x (media modules loaded?)\n", rc);
        return -1;
    }

    MI_DISP_PubAttr_t pub;
    memset(&pub, 0, sizeof pub);
    pub.u32BgColor = 0x800080;                  /* YUYV black */
    pub.eIntfSync = sync;
    pub.eIntfType = E_MI_DISP_INTF_HDMI;
    if ((rc = MI_DISP_SetPubAttr(DISP_DEV, &pub)) != MI_SUCCESS) {
        /* the panel ini files are missing from /config, typically */
        fprintf(stderr, "sigmastar: MI_DISP_SetPubAttr(HDMI) 0x%x\n", rc);
        goto fail_sys;
    }
    pub.eIntfType = E_MI_DISP_INTF_VGA;
    if ((rc = MI_DISP_SetPubAttr(DISP_DEV, &pub)) != MI_SUCCESS)
        fprintf(stderr, "sigmastar: MI_DISP_SetPubAttr(VGA) 0x%x\n", rc);
    if ((rc = MI_DISP_Enable(DISP_DEV)) != MI_SUCCESS) {
        fprintf(stderr, "sigmastar: MI_DISP_Enable 0x%x\n", rc);
        goto fail_sys;
    }

    MI_DISP_VideoLayerAttr_t lay;
    memset(&lay, 0, sizeof lay);
    lay.stVidLayerSize.u16Width = w;
    lay.stVidLayerSize.u16Height = h;
    lay.stVidLayerDispWin.u16Width = w;
    lay.stVidLayerDispWin.u16Height = h;
    lay.ePixFormat = E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420;
    if ((rc = MI_DISP_BindVideoLayer(DISP_LAYER, DISP_DEV)) != MI_SUCCESS ||
        (rc = MI_DISP_SetVideoLayerAttr(DISP_LAYER, &lay)) != MI_SUCCESS ||
        (rc = MI_DISP_EnableVideoLayer(DISP_LAYER)) != MI_SUCCESS) {
        fprintf(stderr, "sigmastar: video layer 0x%x\n", rc);
        goto fail_disp;
    }
    g_w = w;
    g_h = h;
    hdmi_output(timing);
    fprintf(stderr, "sigmastar: display %ux%u (%s) on HDMI and VGA\n", w, h, timing);
    mem_baseline();
    if (g_mem_known)
        fprintf(stderr, "sigmastar: %llu MiB of the media heap for decoders\n", g_mem_budget >> 20);
    else
        fprintf(stderr, "sigmastar: %s unreadable: decoders not counted against the heap\n", MMA_PROC);
    return 0;

fail_disp:
    MI_DISP_UnBindVideoLayer(DISP_LAYER, DISP_DEV);
    MI_DISP_Disable(DISP_DEV);
fail_sys:
    MI_SYS_Exit();
    return -1;
}

void hal_shutdown(void)
{
    for (int i = 0; i < MAX_CH; ++i) {
        hal_dec_close(i);
        hal_display_clear_tile(i);
    }
    MI_DISP_DisableVideoLayer(DISP_LAYER);
    MI_DISP_UnBindVideoLayer(DISP_LAYER, DISP_DEV);
    MI_DISP_Disable(DISP_DEV);
    MI_SYS_Exit();
}

/* ---- display ------------------------------------------------------------- */

/* The tile a channel is shown in, or, while it is not placed, a quarter of
 * the screen to size the decoder for. */
static vg_rect_t tile_or_default(int ch)
{
    if (g_ch[ch].tile.w) return g_ch[ch].tile;
    return (vg_rect_t){ 0, 0, (int)g_w / 2, (int)g_h / 2 };
}

/* The window for a picture: fitted inside the tile at its aspect ratio. */
static void fit(int ch, const vg_stream_info_t *si, unsigned *ww, unsigned *wh)
{
    vg_rect_t t = tile_or_default(ch);
    unsigned pw = si->width, ph = si->height;
    *ww = (unsigned)t.w;
    *wh = (unsigned)t.h;
    if (pw && ph) {
        if ((unsigned long long)pw * t.h > (unsigned long long)ph * t.w)
            *wh = (unsigned)((unsigned long long)t.w * ph / pw);   /* bars above and below */
        else
            *ww = (unsigned)((unsigned long long)t.h * pw / ph);   /* bars at the sides */
    }
    *ww = umax(align_down(*ww, 2), 2);
    *wh = umax(align_down(*wh, 2), 2);
}

/*
 * The size the decoder outputs: the window's. mi_disp does not shrink a
 * picture across: a source wider than its window lost its right part (in
 * emulation the driver set the window's ratio to 1:1), so the decoder
 * shrinks it, as the SDK's decoder sample does on every layout change, and
 * the display only enlarges a picture smaller than its window. The
 * decoder's scaler shrinks at most 8 times (the sample puts DIVP behind it
 * beyond that).
 */
static void out_size(int ch, const vg_stream_info_t *si, unsigned *ow, unsigned *oh)
{
    unsigned ww, wh;
    fit(ch, si, &ww, &wh);
    unsigned minw = align_up((si->width + 7) / 8, 16), minh = align_up((si->height + 7) / 8, 2);
    *ow = umin(umax(align_down(umin(ww, si->width), 16), minw), si->width);
    *oh = umin(umax(align_down(umin(wh, si->height), 2), minh), si->height);
}

/* Channel ch's input port: the decoder's output in a window fitted inside
 * the tile and centred; where the decoder shrank the picture to the
 * window, the window is the output's size (rounded to 16 pixels across).
 * Locked. */
static int port_place(int ch)
{
    vg_rect_t t = tile_or_default(ch);
    unsigned ww, wh, ow, oh;
    fit(ch, &g_ch[ch].si, &ww, &wh);
    out_size(ch, &g_ch[ch].si, &ow, &oh);
    /* the picture on the port: the old decoder's, until the new one's */
    unsigned sw = g_ch[ch].port_wait ? g_ch[ch].port_w : g_ch[ch].src_w;
    unsigned sh = g_ch[ch].port_wait ? g_ch[ch].port_h : g_ch[ch].src_h;
    if (sw == ow && sh == oh) {
        if (g_ch[ch].si.width >= ww) ww = umin(ww, ow);
        if (g_ch[ch].si.height >= wh) wh = umin(wh, oh);
    }

    MI_DISP_InputPortAttr_t pa;
    memset(&pa, 0, sizeof pa);
    pa.u16SrcWidth = sw;
    pa.u16SrcHeight = sh;
    pa.stDispWin.u16X = t.x + (int)align_down(((unsigned)t.w - ww) / 2, 2);
    pa.stDispWin.u16Y = t.y + (int)align_down(((unsigned)t.h - wh) / 2, 2);
    pa.stDispWin.u16Width = ww;
    pa.stDispWin.u16Height = wh;
    MI_S32 rc = MI_DISP_SetInputPortAttr(DISP_LAYER, ch, &pa);
    if (rc != MI_SUCCESS) {
        fprintf(stderr, "sigmastar: MI_DISP_SetInputPortAttr(%d) 0x%x\n", ch, rc);
        return -1;
    }
    return 0;
}

static unsigned long long dec_footprint(const vg_stream_info_t *si, unsigned buf, unsigned ow, unsigned oh);

/*
 * A running decoder's output to the size its tile now wants. Smaller, at
 * once. Larger, the pictures came out scrambled (in emulation: mi_vdec
 * output buffers of the new size, the picture laid out wrong in them),
 * even back up to the size the decoder started with; so the output stays
 * as it is, the display enlarges it, and a new decoder opens at the next
 * keyframe (hal_dec_send). Locked.
 */
static void out_follow(int ch)
{
    unsigned ow, oh;
    out_size(ch, &g_ch[ch].si, &ow, &oh);
    g_ch[ch].out_small = ow > g_ch[ch].src_w || oh > g_ch[ch].src_h;
    if (g_ch[ch].out_small || (ow == g_ch[ch].src_w && oh == g_ch[ch].src_h)) return;
    MI_VDEC_OutputPortAttr_t oa;
    memset(&oa, 0, sizeof oa);
    oa.u16Width = ow;
    oa.u16Height = oh;
    MI_S32 rc = MI_VDEC_SetOutputPortAttr(g_ch[ch].vdec, &oa);
    if (rc != MI_SUCCESS) {
        fprintf(stderr, "sigmastar: decoder %d: output %ux%u 0x%x\n", ch, ow, oh, rc);
        return;
    }
    unsigned long long need = dec_footprint(&g_ch[ch].si, g_ch[ch].buf_size, ow, oh);
    g_mem_used = g_mem_used - g_ch[ch].mem + need;     /* less */
    g_ch[ch].mem = need;
    g_ch[ch].src_w = ow;
    g_ch[ch].src_h = oh;
}

int hal_display_set_tile(int ch, const vg_rect_t *r)
{
    if (ch < 0 || ch >= MAX_CH || r->w <= 0 || r->h <= 0) return -1;
    pthread_mutex_lock(&g_lock);
    vg_rect_t old = g_ch[ch].tile;
    /* the display's windows: x on 16 pixels, sizes even */
    g_ch[ch].tile.x = r->x & ~15;
    g_ch[ch].tile.y = r->y & ~1;
    g_ch[ch].tile.w = (r->w + (r->x & 15)) & ~15;
    g_ch[ch].tile.h = r->h & ~1;
    int rc = 0;
    if (g_ch[ch].codec != VG_CODEC_NONE) {      /* a decoder feeds the port */
        out_follow(ch);
        rc = port_place(ch);
        if (rc == 0) {
            MI_DISP_ShowInputPort(DISP_LAYER, ch);
        } else {
            g_ch[ch].tile = old;                /* back to the old window */
            out_follow(ch);
            port_place(ch);
        }
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

void hal_display_clear_tile(int ch)
{
    if (ch < 0 || ch >= MAX_CH) return;
    pthread_mutex_lock(&g_lock);
    if (g_ch[ch].codec != VG_CODEC_NONE)
        MI_DISP_HideInputPort(DISP_LAYER, ch);
    g_ch[ch].tile.w = 0;
    pthread_mutex_unlock(&g_lock);
}

/* ---- decoder --------------------------------------------------------------- */

static MI_SYS_ChnPort_t vdec_port(int vdec)
{
    MI_SYS_ChnPort_t p;
    memset(&p, 0, sizeof p);
    p.eModId = E_MI_MODULE_ID_VDEC;
    p.u32ChnId = vdec;
    return p;
}

/* MI_SYS addresses layer 0's input port n as DISP channel n, port 0: the
 * SDK's NVR samples bind that way, and so do all the tiles here. */
static MI_SYS_ChnPort_t disp_port(int ch)
{
    MI_SYS_ChnPort_t p;
    memset(&p, 0, sizeof p);
    p.eModId = E_MI_MODULE_ID_DISP;
    p.u32DevId = DISP_DEV;
    p.u32ChnId = ch;
    return p;
}

/* What is free in the media heap, or -1. The line after the header:
 * "mma_heap_name0  <base> <length> <chunk_mgr_avail>", in hex. */
static long long mma_avail(void)
{
    FILE *f = fopen(MMA_PROC, "r");
    if (!f) return -1;
    char line[256], name[64];
    unsigned long long base, len, avail;
    long long r = -1;
    while (r < 0 && fgets(line, sizeof line, f))
        if (sscanf(line, " %63s %llx %llx %llx", name, &base, &len, &avail) == 4 &&
            !strncmp(name, "mma_heap", 8))
            r = (long long)avail;
    fclose(f);
    return r;
}

/* With no decoder open: what the decoders may have between them. */
static void mem_baseline(void)
{
    long long avail = mma_avail();
    g_mem_known = avail >= 0;
    g_mem_budget = avail > (long long)MMA_RESERVE ? (unsigned long long)avail - MMA_RESERVE : 0;
}

/*
 * What a decoder takes from the heap, rounded up from what
 * /proc/mi_modules/mi_sys_mma lists for 720p and 640x360, H.264 and H.265
 * (in emulation, the SDK's modules): per reference picture plus one, the
 * picture at 1.5 bytes a pixel (rounded to 32 lines), its motion vectors
 * (80 bytes a macroblock or less) and compression tables (w x h / 15 or
 * less); the bitstream buffer; a work buffer (104 KiB, 172 for H.265); and
 * output pictures, one to two in use per channel, here four.
 */
static unsigned long long dec_footprint(const vg_stream_info_t *si, unsigned buf, unsigned ow, unsigned oh)
{
    unsigned long long w = align_up(si->width, 32), h = align_up(si->height, 32);
    unsigned long long mbs = (w / 16) * (h / 16);
    unsigned long long pic = w * h * 3 / 2 + mbs * 96 + w * h / 12 + (32u << 10);
    unsigned long long out = (unsigned long long)ow * align_up(oh, 16) * 3 / 2 + (8u << 10);
    unsigned refs = si->refs ? si->refs : 1;
    return (refs + 1) * pic + buf + (192u << 10) + 4 * out;
}

static unsigned buf_for(const vg_stream_info_t *si)
{
    unsigned long long px = (unsigned long long)si->width * si->height;
    return px > 1920 * 1088 ? 2u << 20 : px > 720 * 576 ? 1u << 20 : VDEC_BUF_MIN;
}

/* A refusal, said once until the channel gets a decoder again: the core
 * asks again and again while the heap or the channel numbers are taken. */
static void refuse(int ch, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void refuse(int ch, const char *fmt, ...)
{
    if (g_ch[ch].refused) return;
    g_ch[ch].refused = 1;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "sigmastar: decoder %d: ", ch);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* An MI_VDEC channel for `si`, outputting ow x oh (or the picture's own
 * size if the decoder refuses that): its number, or -1 (said through
 * refuse). Not bound to anything. Locked. */
static int vdec_make(int ch, vg_codec_t codec, const vg_stream_info_t *si, unsigned buf,
                     unsigned *ow, unsigned *oh)
{
    MI_S32 rc;
    MI_VDEC_ChnAttr_t ca;
    memset(&ca, 0, sizeof ca);
    ca.eCodecType = codec == VG_CODEC_H265 ? E_MI_VDEC_CODEC_TYPE_H265 : E_MI_VDEC_CODEC_TYPE_H264;
    ca.u32BufSize = buf;
    ca.u32PicWidth = si->width;
    ca.u32PicHeight = si->height;
    ca.eVideoMode = E_MI_VDEC_VIDEO_MODE_FRAME;
    ca.eDpbBufMode = E_MI_VDEC_DPB_MODE_NORMAL;
    ca.stVdecVideoAttr.u32RefFrameNum = si->refs ? si->refs : 1;
    int id;
    for (;;) {
        /* The free number freed longest ago (never used ones first, the
         * lowest first): a decoder made again in place on the number it
         * had just given up showed nothing for 20-40 s in emulation, the
         * emulated VPU still busy with the old one's pictures. */
        id = -1;
        for (int i = 0; i < g_vdec_ids; ++i)
            if (!g_vdec_busy[i] && (id < 0 || g_vdec_freed[i] < g_vdec_freed[id])) id = i;
        if (id < 0) {
            refuse(ch, "mi_vdec's %d channels are all in use", g_vdec_ids);
            return -1;
        }
        if ((rc = MI_VDEC_CreateChn(id, &ca)) == MI_SUCCESS) break;
        if (rc != MI_ERR_VDEC_INVALID_CHNID || id == 0 || id >= g_vdec_ids) {
            refuse(ch, "MI_VDEC_CreateChn(%d) %ux%u 0x%x", id, si->width, si->height, rc);
            return -1;
        }
        /* The device's MaxChnNum: every lower number has worked (never
         * used numbers are tried lowest first), none from here up will. */
        fprintf(stderr, "sigmastar: mi_vdec takes %d channels (MaxChnNum)\n", id);
        g_vdec_ids = id;
    }
    /* A channel number used before keeps a display mode past the enum
     * (/proc's DisplayMode 2) and its pictures never show: set it. */
    if ((rc = MI_VDEC_SetDisplayMode(id, E_MI_VDEC_DISPLAY_MODE_PREVIEW)) != MI_SUCCESS)
        fprintf(stderr, "sigmastar: MI_VDEC_SetDisplayMode(%d) 0x%x\n", id, rc);
    if ((rc = MI_VDEC_StartChn(id)) != MI_SUCCESS) {
        refuse(ch, "MI_VDEC_StartChn(%d) 0x%x", id, rc);
        MI_VDEC_DestroyChn(id);
        return -1;
    }
    MI_VDEC_OutputPortAttr_t oa;
    memset(&oa, 0, sizeof oa);
    oa.u16Width = *ow;
    oa.u16Height = *oh;
    if (MI_VDEC_SetOutputPortAttr(id, &oa) != MI_SUCCESS) {
        *ow = si->width;                    /* the picture as decoded */
        *oh = si->height;
    }
    MI_SYS_ChnPort_t src = vdec_port(id);
    /* Live view: a short queue, the newest picture wins. */
    MI_SYS_SetChnOutputPortDepth(&src, 0, 4);
    g_vdec_busy[id] = 1;
    ++g_open;
    return id;
}

/* Locked. */
static void vdec_free(int id)
{
    MI_VDEC_StopChn(id);
    MI_VDEC_DestroyChn(id);
    g_vdec_busy[id] = 0;
    g_vdec_freed[id] = ++g_vdec_frees;
    if (--g_open == 0) mem_baseline();      /* the account starts afresh */
}

/* Drop a decoder that was taking over. Locked. */
static void nx_drop(int ch)
{
    if (!g_ch[ch].nx.on) return;
    g_ch[ch].nx.on = 0;
    g_mem_used -= g_ch[ch].nx.mem;
    vdec_free(g_ch[ch].nx.vdec);
}

/* The decoder goes; the port stays as it is, showing its last picture.
 * Locked. */
static void dec_release(int ch)
{
    nx_drop(ch);
    if (g_ch[ch].codec == VG_CODEC_NONE) return;
    int id = g_ch[ch].vdec;
    MI_SYS_ChnPort_t src = vdec_port(id), dst = disp_port(ch);
    MI_SYS_UnBindChnPort(&src, &dst);
    g_mem_used -= g_ch[ch].mem;
    g_ch[ch].mem = 0;
    g_ch[ch].codec = VG_CODEC_NONE;
    g_ch[ch].port_wait = 0;
    vdec_free(id);
}

/* Locked. */
static void dec_teardown(int ch)
{
    int had = g_ch[ch].codec != VG_CODEC_NONE;
    dec_release(ch);
    if (had) MI_DISP_DisableInputPort(DISP_LAYER, ch);
}

void hal_dec_close(int ch)
{
    if (ch < 0 || ch >= MAX_CH) return;
    pthread_mutex_lock(&g_lock);
    dec_teardown(ch);
    g_ch[ch].lost = 0;
    pthread_mutex_unlock(&g_lock);
}

static long ms_since(const struct timespec *t)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t->tv_sec) * 1000L + (now.tv_nsec - t->tv_nsec) / 1000000L;
}

/* The size of the last picture input port ch received, from mi_disp's
 * /proc file (the table with RecvBufCnt: port, count, width, height).
 * 0, or -1 if it cannot be read. */
static int disp_recv_size(int ch, unsigned *w, unsigned *h)
{
    FILE *f = fopen(DISP_PROC, "r");
    if (!f) return -1;
    char line[256];
    int in = 0, rc = -1;
    while (rc < 0 && fgets(line, sizeof line, f)) {
        if (strstr(line, "RecvBufCnt")) { in = 1; continue; }
        if (!in) continue;
        int port;
        unsigned cnt, bw, bh;
        if (sscanf(line, "%d %u %u %u", &port, &cnt, &bw, &bh) != 4) break;
        if (port == ch) {
            *w = bw;
            *h = bh;
            rc = 0;
        }
    }
    fclose(f);
    return rc;
}

/* The decoder taking over goes on screen: the port, which kept showing the
 * old decoder's last picture, is bound to it, and the old one goes. Locked. */
static void nx_swap(int ch)
{
    MI_SYS_ChnPort_t dst = disp_port(ch);
    MI_SYS_ChnPort_t old = vdec_port(g_ch[ch].vdec);
    MI_SYS_UnBindChnPort(&old, &dst);
    g_ch[ch].port_w = g_ch[ch].port_wait ? g_ch[ch].port_w : g_ch[ch].src_w;
    g_ch[ch].port_h = g_ch[ch].port_wait ? g_ch[ch].port_h : g_ch[ch].src_h;
    vdec_free(g_ch[ch].vdec);
    g_mem_used -= g_ch[ch].mem;
    g_ch[ch].vdec = g_ch[ch].nx.vdec;
    g_ch[ch].codec = g_ch[ch].nx.codec;
    g_ch[ch].si = g_ch[ch].nx.si;
    g_ch[ch].buf_size = g_ch[ch].nx.buf;
    g_ch[ch].src_w = g_ch[ch].nx.src_w;
    g_ch[ch].src_h = g_ch[ch].nx.src_h;
    g_ch[ch].mem = g_ch[ch].nx.mem;
    g_ch[ch].too_big = 0;
    g_ch[ch].nx.on = 0;
    out_follow(ch);                         /* the tile may have changed meanwhile */
    /* The port keeps the old picture until the new decoder's first one
     * reaches it: with the new size set at once, the display read the old
     * picture at the wrong size (garbled for seconds in emulation, where
     * the first picture comes late). hal_dec_send sets it then. */
    g_ch[ch].port_wait = g_ch[ch].port_w != g_ch[ch].src_w || g_ch[ch].port_h != g_ch[ch].src_h;
    MI_SYS_ChnPort_t src = vdec_port(g_ch[ch].vdec);
    MI_S32 rc = MI_SYS_BindChnPort(&src, &dst, 30, 30);
    if (rc != MI_SUCCESS) {
        fprintf(stderr, "sigmastar: decoder %d to the display 0x%x\n", ch, rc);
        dec_teardown(ch);
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &g_ch[ch].port_checked);
    fprintf(stderr, "sigmastar: decoder %d: vdec %d bound %ld ms after the switch\n", ch, g_ch[ch].vdec,
            ms_since(&g_ch[ch].nx.since));
}

/* (Re)create channel ch's decoder with a `buf`-byte bitstream buffer.
 * With a decoder already there, the new one is made beside it and fed
 * while the port keeps the old one's last picture, the display enlarging
 * it if the tile grew; it goes on screen once it has pictures (nx_swap).
 * Without room for both, the old one goes first, and stays if the new one
 * would not fit. Locked. */
static int dec_open(int ch, vg_codec_t codec, const vg_stream_info_t *si, unsigned buf)
{
    MI_S32 rc;
    int own = g_ch[ch].codec != VG_CODEC_NONE;
    nx_drop(ch);
    int ids = own;
    for (int i = 0; i < g_vdec_ids && !ids; ++i) ids = !g_vdec_busy[i];
    if (!ids) {
        refuse(ch, "mi_vdec's %d channels are all in use", g_vdec_ids);
        return -1;
    }
    unsigned ow, oh;
    out_size(ch, si, &ow, &oh);
    unsigned long long need = dec_footprint(si, buf, ow, oh);

    int spare = 0;
    for (int i = 0; i < g_vdec_ids && !spare; ++i) spare = !g_vdec_busy[i];
    if (own && spare && (!g_mem_known || g_mem_used + need <= g_mem_budget)) {
        int id = vdec_make(ch, codec, si, buf, &ow, &oh);
        if (id >= 0) {
            g_ch[ch].nx.on = 1;
            g_ch[ch].nx.vdec = id;
            g_ch[ch].nx.codec = codec;
            g_ch[ch].nx.si = *si;
            g_ch[ch].nx.buf = buf;
            g_ch[ch].nx.src_w = ow;
            g_ch[ch].nx.src_h = oh;
            g_ch[ch].nx.mem = need;
            clock_gettime(CLOCK_MONOTONIC, &g_ch[ch].nx.since);
            g_ch[ch].switched = g_ch[ch].nx.since;
            g_mem_used += need;
            g_ch[ch].out_small = 0;
            g_ch[ch].lost = 0;
            g_ch[ch].refused = 0;
            fprintf(stderr, "sigmastar: decoder %d (vdec %d): %s %ux%u -> %ux%u, %u KiB buffer, "
                    "~%llu KiB of the heap, taking over from vdec %d\n", ch, id, vg_codec_name(codec),
                    si->width, si->height, ow, oh, buf >> 10, need >> 10, g_ch[ch].vdec);
            return 0;
        }
    }

    unsigned long long others = g_mem_used - (own ? g_ch[ch].mem : 0);
    if (g_mem_known && others + need > g_mem_budget) {
        refuse(ch, "%s %ux%u needs ~%llu MiB of the media heap, %llu MiB left of %llu",
               vg_codec_name(codec), si->width, si->height, need >> 20,
               g_mem_budget > others ? (g_mem_budget - others) >> 20 : 0, g_mem_budget >> 20);
        return -1;
    }
    /* Replacing a decoder on screen without room for both: the port keeps
     * the old one's last picture (and its size) until the new one's. */
    int keep = own && g_ch[ch].tile.w;
    unsigned pw = g_ch[ch].port_wait ? g_ch[ch].port_w : g_ch[ch].src_w;
    unsigned ph = g_ch[ch].port_wait ? g_ch[ch].port_h : g_ch[ch].src_h;
    if (keep) dec_release(ch);
    else dec_teardown(ch);
    int id = vdec_make(ch, codec, si, buf, &ow, &oh);
    if (id < 0) {
        if (keep) MI_DISP_DisableInputPort(DISP_LAYER, ch);
        return -1;
    }
    /* from here dec_teardown undoes it */
    g_ch[ch].codec = codec;
    g_ch[ch].vdec = id;
    g_ch[ch].mem = need;
    g_mem_used += need;
    g_ch[ch].si = *si;
    g_ch[ch].buf_size = buf;
    g_ch[ch].too_big = 0;
    g_ch[ch].lost = 0;
    g_ch[ch].src_w = ow;
    g_ch[ch].src_h = oh;
    g_ch[ch].out_small = 0;
    if (keep) {
        g_ch[ch].port_w = pw;
        g_ch[ch].port_h = ph;
        g_ch[ch].port_wait = pw != ow || ph != oh;
        clock_gettime(CLOCK_MONOTONIC, &g_ch[ch].switched);
        g_ch[ch].port_checked = g_ch[ch].switched;
    }

    MI_SYS_ChnPort_t src = vdec_port(id), dst = disp_port(ch);
    if (port_place(ch) < 0) {
        dec_teardown(ch);
        return -1;
    }
    if ((!keep && ((rc = MI_DISP_EnableInputPort(DISP_LAYER, ch)) != MI_SUCCESS ||
                   (rc = MI_DISP_SetInputPortSyncMode(DISP_LAYER, ch, E_MI_DISP_SYNC_MODE_FREE_RUN)) != MI_SUCCESS)) ||
        (rc = MI_SYS_BindChnPort(&src, &dst, 30, 30)) != MI_SUCCESS) {
        refuse(ch, "to the display 0x%x", rc);
        dec_teardown(ch);
        return -1;
    }
    /* nothing of an earlier decoder on this port until this one has a
     * picture, unless it is a replacement */
    if (!keep) MI_DISP_ClearInputPortBuffer(DISP_LAYER, ch, 1);
    if (!g_ch[ch].tile.w)
        MI_DISP_HideInputPort(DISP_LAYER, ch);
    g_ch[ch].refused = 0;
    fprintf(stderr, "sigmastar: decoder %d (vdec %d): %s %ux%u -> %ux%u, %u KiB buffer, ~%llu KiB of the heap\n",
            ch, id, vg_codec_name(codec), si->width, si->height, g_ch[ch].src_w, g_ch[ch].src_h,
            buf >> 10, need >> 10);
    return 0;
}

/* Make channel ch's decoder again (reset, a larger buffer); if that tore
 * down the old one and failed, reopen it at keyframes later. Locked. */
static int dec_recreate(int ch, unsigned buf)
{
    /* the newest decoder's stream: a decoder taking over is for it */
    vg_codec_t codec = g_ch[ch].nx.on ? g_ch[ch].nx.codec : g_ch[ch].codec;
    vg_stream_info_t si = g_ch[ch].nx.on ? g_ch[ch].nx.si : g_ch[ch].si;
    if (dec_open(ch, codec, &si, buf) == 0) return 0;
    if (g_ch[ch].codec == VG_CODEC_NONE) {
        fprintf(stderr, "sigmastar: decoder %d lost, retried at keyframes\n", ch);
        g_ch[ch].lost = 1;
        g_ch[ch].lost_codec = codec;
        g_ch[ch].lost_buf = buf;
        g_ch[ch].lost_at = time(NULL);
    }
    return -1;
}

int hal_dec_open(int ch, vg_codec_t codec, const vg_stream_info_t *si)
{
    if (ch < 0 || ch >= MAX_CH || codec == VG_CODEC_NONE) return -1;
    if (si->width > g_caps.dec_max_w || si->height > g_caps.dec_max_h) return -1;
    pthread_mutex_lock(&g_lock);
    int rc = dec_open(ch, codec, si, buf_for(si));
    pthread_mutex_unlock(&g_lock);
    return rc;
}

/* A decoder that a reset could not bring back: the core still counts it as
 * open, so make it again here, at a keyframe, now and then. */
static void dec_reopen_lost(int ch)
{
    pthread_mutex_lock(&g_lock);
    if (g_ch[ch].lost && g_ch[ch].codec == VG_CODEC_NONE &&
        time(NULL) - g_ch[ch].lost_at >= REOPEN_SECS) {
        vg_stream_info_t si = g_ch[ch].si;
        if (dec_open(ch, g_ch[ch].lost_codec, &si, g_ch[ch].lost_buf) == 0)
            fprintf(stderr, "sigmastar: decoder %d back\n", ch);
        else
            g_ch[ch].lost_at = time(NULL);
    }
    pthread_mutex_unlock(&g_lock);
}

/* A keyframe larger than the bitstream buffer: a buffer that holds it. */
static int dec_regrow(int ch, size_t len)
{
    unsigned buf = VDEC_BUF_MIN;
    while (buf < len) buf *= 2;
    pthread_mutex_lock(&g_lock);
    int rc = -1;
    if (g_ch[ch].codec != VG_CODEC_NONE && (rc = dec_recreate(ch, buf)) == 0)
        fprintf(stderr, "sigmastar: decoder %d: %zu-byte keyframe, buffer now %u KiB\n", ch, len, buf >> 10);
    pthread_mutex_unlock(&g_lock);
    return rc;
}

/* The tile outgrew the decoder (out_follow): a new one at its size, from
 * this keyframe on. If the heap has no room, the old one stays. */
static void dec_reopen_larger(int ch)
{
    pthread_mutex_lock(&g_lock);
    if (g_ch[ch].codec != VG_CODEC_NONE && g_ch[ch].out_small) {
        g_ch[ch].out_small = 0;             /* once per tile change */
        dec_recreate(ch, g_ch[ch].buf_size);
    }
    pthread_mutex_unlock(&g_lock);
}

int hal_dec_send(int ch, const vg_au_t *au)
{
    if (ch < 0 || ch >= MAX_CH) return -1;
    if (g_ch[ch].lost && au->key) dec_reopen_lost(ch);
    if (g_ch[ch].out_small && au->key) dec_reopen_larger(ch);
    if (g_ch[ch].codec == VG_CODEC_NONE) return -1;
    /* MI_VDEC takes an AU whole: one larger than its buffer never goes in.
     * At a keyframe the decoder can start over with a larger one. The
     * stream goes to a decoder taking over, if there is one: the old one
     * keeps its last picture on screen. */
    unsigned buf = g_ch[ch].nx.on ? g_ch[ch].nx.buf : g_ch[ch].buf_size;
    if (au->len > buf &&
        !(au->key && au->len <= VDEC_BUF_MAX && dec_regrow(ch, au->len) == 0)) {
        if (!g_ch[ch].too_big)
            fprintf(stderr, "sigmastar: decoder %d: %zu-byte AU over its %u-byte buffer, dropped\n",
                    ch, au->len, buf);
        g_ch[ch].too_big = 1;
        return -1;
    }
    int nx = g_ch[ch].nx.on, id = nx ? g_ch[ch].nx.vdec : g_ch[ch].vdec;
    MI_VDEC_VideoStream_t s;
    memset(&s, 0, sizeof s);
    s.pu8Addr = (MI_U8 *)au->data;
    s.u32Len = (MI_U32)au->len;
    s.u64PTS = au->pts_us;
    s.bEndOfFrame = 1;
    /* The bitstream buffer fills while the decoder lags: wait a little. */
    MI_S32 rc = MI_ERR_VDEC_BUF_FULL;
    for (int i = 0; i < SEND_TRIES && rc == MI_ERR_VDEC_BUF_FULL; ++i)
        rc = MI_VDEC_SendStream(id, &s, SEND_TIMEOUT_MS);
    /* Looked at with every picture sent while it waits: the new picture
     * shows at the old size until then. */
    if (g_ch[ch].port_wait && !nx && ms_since(&g_ch[ch].port_checked) >= PORT_CHECK_MS) {
        clock_gettime(CLOCK_MONOTONIC, &g_ch[ch].port_checked);
        unsigned w = 0, h = 0;
        int arrived = disp_recv_size(ch, &w, &h) == 0 && w == g_ch[ch].src_w && h == g_ch[ch].src_h;
        if (arrived || ms_since(&g_ch[ch].switched) >= PORT_WAIT_MS) {
            pthread_mutex_lock(&g_lock);
            if (g_ch[ch].port_wait && g_ch[ch].vdec == id) {
                g_ch[ch].port_wait = 0;
                port_place(ch);
                fprintf(stderr, "sigmastar: decoder %d: its picture on screen %ld ms after the switch\n",
                        ch, ms_since(&g_ch[ch].switched));
            }
            pthread_mutex_unlock(&g_lock);
        }
    }
    if (nx) {
        MI_VDEC_ChnStat_t st;
        memset(&st, 0, sizeof st);
        if ((MI_VDEC_GetChnStat(id, &st) == MI_SUCCESS && st.u32DecodeStreamFrames >= TAKEOVER_PICS) ||
            ms_since(&g_ch[ch].nx.since) >= TAKEOVER_MS) {
            pthread_mutex_lock(&g_lock);
            if (g_ch[ch].nx.on && g_ch[ch].nx.vdec == id) nx_swap(ch);
            pthread_mutex_unlock(&g_lock);
        }
    }
    return rc == MI_SUCCESS ? 0 : -1;
}

void hal_dec_reset(int ch)
{
    if (ch < 0 || ch >= MAX_CH) return;
    pthread_mutex_lock(&g_lock);
    /* A new decoder for the same stream, not MI_VDEC_StopChn/ResetChn/
     * StartChn: in emulation MI_VDEC_ResetChn never returned after a camera
     * dropped mid-stream (D state, msleep), and every other decoder stopped
     * with it. Destroying and creating a channel is what changing pages
     * does all the time. */
    if (g_ch[ch].codec != VG_CODEC_NONE) dec_recreate(ch, g_ch[ch].buf_size);
    pthread_mutex_unlock(&g_lock);
}

/* ---- capture: none on this SoC --------------------------------------------- */

int  hal_cap_start(int in, int ch, hal_cap_cb cb, void *user) { (void)in; (void)ch; (void)cb; (void)user; return -1; }
void hal_cap_stop(int in) { (void)in; }
int  hal_cap_signal(int in) { (void)in; return 0; }
