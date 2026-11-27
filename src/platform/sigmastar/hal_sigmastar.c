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
 * The output mode (k_modes) can change while running. The screen (the
 * UI's coordinates, what hal_init was given) stays: the GOP stretches the
 * UI layer to the output (sstar_fb.h), and the video layer runs at the
 * output's own size, each tile scaled to it, so the video is not scaled
 * twice.
 *
 * The picture goes into a window fitted inside the tile at the display
 * aspect ratio the core gives (hal_display_set_aspect, aspect_fit; the rest
 * of the tile shows the layer's black background), or the whole tile
 * (stretch, fill). The decoder outputs it at about the window's size and
 * follows the tile when it changes: the display enlarges pictures but does
 * not shrink them (out_size). In fill the decoder outputs the picture at
 * about the size that covers the tile, and the display's zoom-in window
 * shows the middle of it (port_zoom).
 *
 * mi_disp needs the panel timings, PQ tables and memory map from /config,
 * and mi_vdec its firmware from /config/vdec_fw (paths built into the
 * modules); OpenIPC's sigmastar-osdrv-infinity2m installs them.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mi_common_datatype.h"
#include "mi_sys.h"
#include "mi_vdec.h"
#include "mi_disp.h"

#include "aspect.h"
#include "hal.h"
#include "hal_capture.h"
#include "hal_decoder.h"
#include "hal_display.h"
#include "sstar_fb.h"

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
    /* 64 Mbit/s of camera streams coming in. SigmaStar publishes no
     * SSR621Q datasheet or product brief; this is the figure every
     * SSR621Q recorder's spec sheet gives (and so, most likely, the
     * vendor's reference design): "Mstar SSR621Q platform ... Network
     * video Input/Output Bandwidth: 64Mbps/32Mbps" (Uvision NVR6116),
     * "64Mbps input" (DVC DRN-0861RN), "Incoming bandwidth 64M"
     * (Secureye ST-NVR-011). The HAL does not tell infinity2m chips
     * apart; the SSR621Q is the NVR one. */
    .max_in_kbps = 64000,
};

static struct {
    vg_codec_t       codec;         /* VG_CODEC_NONE: no decoder */
    int              vdec;          /* its MI_VDEC channel */
    unsigned long long mem;         /* its share of the heap (dec_footprint) */
    vg_stream_info_t si;            /* the stream it was opened for */
    unsigned         buf_size;      /* its bitstream buffer */
    vg_rect_t        scr;           /* the tile as given, on the screen; w == 0: not placed */
    vg_rect_t        tile;          /* ... on the output (the video layer); w == 0: not placed */
    int              asp_set;       /* hal_display_set_aspect was called: */
    vg_ratio_t       asp;           /* the picture's shape (0:0 the decoded size's) */
    vg_aspect_t      how;           /* ... and how it fills the tile */
    /* The part of the port's picture shown (port_zoom), in the pixels of a
     * crop_bw x crop_bh picture; w == 0: all of it. */
    vg_rect_t        crop;
    unsigned         crop_bw, crop_bh;
    unsigned         src_w, src_h;  /* what the decoder outputs */
    vg_rect_t        win;           /* the port's window on the output (port_place) */
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
static unsigned g_sw, g_sh;         /* the screen: the UI's and the tiles' coordinates */
static unsigned g_w, g_h;           /* the output: the video layer */
static int g_mode = -1;             /* k_modes[], in force */
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

static unsigned align_up(unsigned v, unsigned a) { return (v + a - 1) / a * a; }

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

/*
 * The modes both the display (MI_DISP) and open_hdmi have; VGA follows
 * the same timing. The display's own list goes further (1366x768,
 * 1680x1050, 2560x1440, 4K), open_hdmi's does not. None narrower or lower
 * than the screen: the GOP enlarges the UI layer but does not shrink it
 * (fbdev.ko refuses a stretch destination smaller than its buffer, seen
 * in qemu for 1024x768: EINVAL).
 */
static const struct {
    vg_mode_t              m;
    MI_DISP_OutputTiming_e sync;
    const char            *hdmi;        /* open_hdmi's name */
} k_modes[] = {
    { { 1280, 720, 60 }, E_MI_DISP_OUTPUT_720P60, "720p60" },
    { { 1280, 720, 50 }, E_MI_DISP_OUTPUT_720P50, "720p50" },
    { { 1920, 1080, 60 }, E_MI_DISP_OUTPUT_1080P60, "1080p60" },
    { { 1920, 1080, 50 }, E_MI_DISP_OUTPUT_1080P50, "1080p50" },
    { { 1280, 1024, 60 }, E_MI_DISP_OUTPUT_1280x1024_60, "1280x1024p60" },
    { { 1440, 900, 60 }, E_MI_DISP_OUTPUT_1440x900_60, "1440x900p60" },
};
#define N_MODES (int)(sizeof k_modes / sizeof k_modes[0])

static int mode_index(const vg_mode_t *m)
{
    for (int i = 0; i < N_MODES; ++i)
        if (k_modes[i].m.w == m->w && k_modes[i].m.h == m->h && k_modes[i].m.hz == m->hz) return i;
    return -1;
}

/* The device's timing on both interfaces. HDMI first: without its panel
 * ini files in /config it fails, and nothing is shown. */
static MI_S32 disp_timing(int mode)
{
    MI_DISP_PubAttr_t pub;
    memset(&pub, 0, sizeof pub);
    pub.u32BgColor = 0x800080;                  /* YUYV black */
    pub.eIntfSync = k_modes[mode].sync;
    pub.eIntfType = E_MI_DISP_INTF_HDMI;
    MI_S32 rc = MI_DISP_SetPubAttr(DISP_DEV, &pub);
    if (rc != MI_SUCCESS) {
        fprintf(stderr, "sigmastar: MI_DISP_SetPubAttr(HDMI, %s) 0x%x\n", k_modes[mode].hdmi, rc);
        return rc;
    }
    pub.eIntfType = E_MI_DISP_INTF_VGA;
    if ((rc = MI_DISP_SetPubAttr(DISP_DEV, &pub)) != MI_SUCCESS)
        fprintf(stderr, "sigmastar: MI_DISP_SetPubAttr(VGA, %s) 0x%x\n", k_modes[mode].hdmi, rc);
    return MI_SUCCESS;
}

/* The video layer over the whole output. */
static MI_S32 layer_size(int mode)
{
    MI_DISP_VideoLayerAttr_t lay;
    memset(&lay, 0, sizeof lay);
    lay.stVidLayerSize.u16Width = k_modes[mode].m.w;
    lay.stVidLayerSize.u16Height = k_modes[mode].m.h;
    lay.stVidLayerDispWin.u16Width = k_modes[mode].m.w;
    lay.stVidLayerDispWin.u16Height = k_modes[mode].m.h;
    lay.ePixFormat = E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420;
    return MI_DISP_SetVideoLayerAttr(DISP_LAYER, &lay);
}

int hal_init(unsigned w, unsigned h, const vg_mode_t *out)
{
    vg_mode_t want = out ? *out : (vg_mode_t){ w, h, 60 };
    int mode = mode_index(&want);
    if (mode < 0) {
        vg_mode_t screen = { w, h, 60 };
        mode = mode_index(&screen);
        if (mode < 0) {
            fprintf(stderr, "sigmastar: no output mode for a %ux%u screen\n", w, h);
            return -1;
        }
        fprintf(stderr, "sigmastar: no output mode %ux%u@%u, using %s\n", want.w, want.h, want.hz,
                k_modes[mode].hdmi);
    }
    MI_S32 rc;
    if ((rc = MI_SYS_Init()) != MI_SUCCESS) {
        fprintf(stderr, "sigmastar: MI_SYS_Init 0x%x (media modules loaded?)\n", rc);
        return -1;
    }

    if (disp_timing(mode) != MI_SUCCESS) goto fail_sys;      /* the panel ini files, typically */
    if ((rc = MI_DISP_Enable(DISP_DEV)) != MI_SUCCESS) {
        fprintf(stderr, "sigmastar: MI_DISP_Enable 0x%x\n", rc);
        goto fail_sys;
    }
    if ((rc = MI_DISP_BindVideoLayer(DISP_LAYER, DISP_DEV)) != MI_SUCCESS ||
        (rc = layer_size(mode)) != MI_SUCCESS ||
        (rc = MI_DISP_EnableVideoLayer(DISP_LAYER)) != MI_SUCCESS) {
        fprintf(stderr, "sigmastar: video layer 0x%x\n", rc);
        goto fail_disp;
    }
    g_sw = w;
    g_sh = h;
    g_w = k_modes[mode].m.w;
    g_h = k_modes[mode].m.h;
    g_mode = mode;
    /* The UI layer to the output, also when that is the screen's size: a
     * stretch set by a vigild that ran before stays in fbdev.ko. */
    if (sstar_fb_screen(g_w, g_h) < 0 && (g_w != w || g_h != h))
        fprintf(stderr, "sigmastar: the UI layer is not scaled to %s\n", k_modes[mode].hdmi);
    hdmi_output(k_modes[mode].hdmi);
    fprintf(stderr, "sigmastar: display %ux%u (%s) on HDMI and VGA, screen %ux%u\n", g_w, g_h,
            k_modes[mode].hdmi, w, h);
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

/* The shape the picture is shown at: the core's, or before the core says
 * (or when it does not know), the decoded picture's own. */
static vg_ratio_t shape_of(int ch, const vg_stream_info_t *si)
{
    if (g_ch[ch].asp_set && g_ch[ch].asp.w) return g_ch[ch].asp;
    return (vg_ratio_t){ si->width, si->height };
}

static vg_aspect_t how_of(int ch) { return g_ch[ch].asp_set ? g_ch[ch].how : VG_ASPECT_FIT; }

/* The window for a picture on the output: fitted inside the tile at its
 * shape, centred (aspect_fit), or the whole tile (stretch, fill). Sizes
 * and offsets even, as the SDK's display sample has them. */
static vg_rect_t win_of(int ch, const vg_stream_info_t *si)
{
    vg_rect_t t = tile_or_default(ch);
    vg_ratio_t a = how_of(ch) == VG_ASPECT_FIT ? shape_of(ch, si) : (vg_ratio_t){ 0, 0 };
    return aspect_fit(&t, a, 2);
}

/*
 * The size the decoder outputs: about the window's. mi_disp does not shrink
 * a picture: where the window is smaller than the picture, it shows the
 * picture's top left part (open_mhal's _HalDispIfSetInputPortAttr sets the
 * MOP ratio to 1:1 unless the window is larger), so the decoder shrinks it,
 * as the SDK's decoder sample does on every layout change, and the display
 * enlarges what is left to the window. The decoder's scaler shrinks at most
 * 8 times (the sample puts DIVP behind it beyond that), and mi_vdec rounds
 * a shrunk size up to a multiple of 8 (CalcScaleDown, which sizes the
 * scaler's frames): a multiple of 8 is asked for, rounded down, so it
 * never comes out larger than the window, and the display enlarges it by
 * the few pixels left. mi_vdec itself only checks that the size is even,
 * 8..8192 and no larger than the picture (_MI_VDEC_ChkOutputPortAttr).
 * In fill the size wanted is the whole picture's covering the window
 * (aspect_cover), of which the display shows the window's part.
 */
#define VDEC_SCALE_ALIGN  8
#define VDEC_SCALE_MAX    8
static void out_size(int ch, const vg_stream_info_t *si, unsigned *ow, unsigned *oh)
{
    vg_rect_t w = win_of(ch, si);
    if (how_of(ch) == VG_ASPECT_FILL) w = aspect_cover(&w, shape_of(ch, si));
    aspect_scaled_size(si->width, si->height, (unsigned)w.w, (unsigned)w.h, VDEC_SCALE_ALIGN, VDEC_SCALE_MAX,
                       ow, oh);
}

/*
 * In fill, the part of the port's sw x sh picture that shows in window
 * `win`: the middle of it at the window's shape (aspect_crop), given to the
 * display as the port's zoom-in window, in the port's source pixels (the
 * SDK's NVR demo zooms that way, against MI_DISP_GetInputPortAttr's
 * u16SrcWidth). mi_disp keeps it and crops every picture to it on its way
 * to the MOP (MHAL_DISP_InputPortSetCropAttr: the plane's address moved to
 * the corner, the source size and the scaling ratios to the part), which
 * wants every edge even (open_mhal's _HalDispIfGetInfoInputPortCrop). The
 * display enlarges only, so the part is never larger than the window: the
 * decoder's 8x limit can leave more picture than that, and then a little
 * more is cut.
 *
 * A zoom-in window, once given, stays on the port: mi_disp crops every
 * later picture to it, whatever its size, so the top left of a larger
 * picture showed, enlarged, after fill went back to fit and the tile went
 * full screen (emulation). No call takes it away, so every placement of
 * the port sets it, outside fill to all of the picture (as the SDK's NVR
 * demo ends a zoom). Locked.
 */
static void port_zoom(int ch, unsigned sw, unsigned sh, const vg_rect_t *win)
{
    vg_rect_t c = { 0, 0, (int)sw, (int)sh };
    if (how_of(ch) == VG_ASPECT_FILL) {
        c = aspect_crop(sw, sh, shape_of(ch, &g_ch[ch].si), (unsigned)win->w, (unsigned)win->h, 2);
        if (c.w > win->w) {
            c.x += (c.w - win->w) / 2 & ~1;
            c.w = win->w;
        }
        if (c.h > win->h) {
            c.y += (c.h - win->h) / 2 & ~1;
            c.h = win->h;
        }
    }
    int all = c.x == 0 && c.y == 0 && c.w == (int)sw && c.h == (int)sh;
    g_ch[ch].crop = all ? (vg_rect_t){ 0, 0, 0, 0 } : c;
    g_ch[ch].crop_bw = sw;
    g_ch[ch].crop_bh = sh;
    MI_DISP_VidWinRect_t z = { (MI_U16)c.x, (MI_U16)c.y, (MI_U16)c.w, (MI_U16)c.h };
    MI_S32 rc = MI_DISP_SetZoomInWindow(DISP_LAYER, ch, &z);
    if (rc != MI_SUCCESS) {
        fprintf(stderr, "sigmastar: MI_DISP_SetZoomInWindow(%d) %d,%d %dx%d of %ux%u 0x%x\n", ch, c.x, c.y, c.w,
                c.h, sw, sh, rc);
        g_ch[ch].crop = (vg_rect_t){ 0, 0, 0, 0 };
    }
}

/* Channel ch's input port: the decoder's output in the window win_of,
 * which the display enlarges it (in fill, its middle: port_zoom) to.
 * Locked. */
static int port_place(int ch)
{
    vg_rect_t w = win_of(ch, &g_ch[ch].si);
    /* the picture on the port: the old decoder's, until the new one's */
    unsigned sw = g_ch[ch].port_wait ? g_ch[ch].port_w : g_ch[ch].src_w;
    unsigned sh = g_ch[ch].port_wait ? g_ch[ch].port_h : g_ch[ch].src_h;

    MI_DISP_InputPortAttr_t pa;
    memset(&pa, 0, sizeof pa);
    pa.u16SrcWidth = sw;
    pa.u16SrcHeight = sh;
    pa.stDispWin.u16X = w.x;
    pa.stDispWin.u16Y = w.y;
    pa.stDispWin.u16Width = w.w;
    pa.stDispWin.u16Height = w.h;
    MI_S32 rc = MI_DISP_SetInputPortAttr(DISP_LAYER, ch, &pa);
    if (rc != MI_SUCCESS) {
        fprintf(stderr, "sigmastar: MI_DISP_SetInputPortAttr(%d) 0x%x\n", ch, rc);
        return -1;
    }
    g_ch[ch].win = (vg_rect_t){ pa.stDispWin.u16X, pa.stDispWin.u16Y, pa.stDispWin.u16Width,
                                pa.stDispWin.u16Height };
    port_zoom(ch, sw, sh, &w);
    return 0;
}

static unsigned long long dec_footprint(const vg_stream_info_t *si, unsigned buf, unsigned ow, unsigned oh);
static void grab_depth(int vdec);

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

/* A tile on the screen (the UI's coordinates) as a tile on the output,
 * the video layer: each edge scaled, even. On a 5:4 or 16:10 output the
 * UI is stretched, the tile with it; the picture is fitted inside the
 * stretched tile at its own shape (win_of), so it is not. */
static vg_rect_t to_output(const vg_rect_t *r)
{
    return aspect_map(r, g_sw, g_sh, g_w, g_h, 2);
}

int hal_display_set_tile(int ch, const vg_rect_t *r)
{
    if (ch < 0 || ch >= MAX_CH || r->w <= 0 || r->h <= 0) return -1;
    pthread_mutex_lock(&g_lock);
    vg_rect_t old = g_ch[ch].tile, old_scr = g_ch[ch].scr;
    g_ch[ch].scr = *r;
    g_ch[ch].tile = to_output(r);
    int rc = 0;
    if (g_ch[ch].codec != VG_CODEC_NONE) {      /* a decoder feeds the port */
        out_follow(ch);
        rc = port_place(ch);
        if (rc == 0) {
            MI_DISP_ShowInputPort(DISP_LAYER, ch);
        } else {
            g_ch[ch].tile = old;                /* back to the old window */
            g_ch[ch].scr = old_scr;
            out_follow(ch);
            port_place(ch);
        }
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

void hal_display_set_aspect(int ch, unsigned w, unsigned h, vg_aspect_t how)
{
    if (ch < 0 || ch >= MAX_CH) return;
    pthread_mutex_lock(&g_lock);
    vg_ratio_t a = w && h ? (vg_ratio_t){ w, h } : (vg_ratio_t){ 0, 0 };
    if (!g_ch[ch].asp_set || a.w != g_ch[ch].asp.w || a.h != g_ch[ch].asp.h || how != g_ch[ch].how) {
        g_ch[ch].asp_set = 1;
        g_ch[ch].asp = a;
        g_ch[ch].how = how;
        /* a decoder feeding the port: its output and window follow (a
         * larger output from its next keyframe, as for a larger tile) */
        if (g_ch[ch].codec != VG_CODEC_NONE && g_ch[ch].tile.w) {
            out_follow(ch);
            port_place(ch);
        }
    }
    pthread_mutex_unlock(&g_lock);
}

void hal_display_clear_tile(int ch)
{
    if (ch < 0 || ch >= MAX_CH) return;
    pthread_mutex_lock(&g_lock);
    if (g_ch[ch].codec != VG_CODEC_NONE)
        MI_DISP_HideInputPort(DISP_LAYER, ch);
    g_ch[ch].tile.w = 0;
    g_ch[ch].scr.w = 0;
    pthread_mutex_unlock(&g_lock);
}

/* ---- output mode ------------------------------------------------------------ */

int hal_display_modes(vg_mode_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < N_MODES && n < max; ++i) out[n++] = k_modes[i].m;
    return n;
}

void hal_display_mode(vg_mode_t *cur)
{
    *cur = g_mode >= 0 ? k_modes[g_mode].m : (vg_mode_t){ 0, 0, 0 };
}

/*
 * The device off, its timing changed, on again, as the SDK's NVR demo
 * changes the timing (st_main_vdec.c, HDMI and VGA): the video layer is
 * resized, not rebuilt, and the input ports and their bindings stay, each
 * window moved to its tile on the new output. Locked.
 */
static int switch_to(int mode)
{
    MI_S32 rc;
    hdmi_output("off");
    MI_DISP_DisableVideoLayer(DISP_LAYER);
    if ((rc = MI_DISP_Disable(DISP_DEV)) != MI_SUCCESS)
        fprintf(stderr, "sigmastar: MI_DISP_Disable 0x%x\n", rc);
    int ok = disp_timing(mode) == MI_SUCCESS;
    if ((rc = MI_DISP_Enable(DISP_DEV)) != MI_SUCCESS) {
        fprintf(stderr, "sigmastar: MI_DISP_Enable 0x%x\n", rc);
        ok = 0;
    }
    if (ok && (rc = layer_size(mode)) != MI_SUCCESS) {
        fprintf(stderr, "sigmastar: video layer %ux%u 0x%x\n", k_modes[mode].m.w, k_modes[mode].m.h, rc);
        ok = 0;
    }
    if ((rc = MI_DISP_EnableVideoLayer(DISP_LAYER)) != MI_SUCCESS) {
        fprintf(stderr, "sigmastar: MI_DISP_EnableVideoLayer 0x%x\n", rc);
        ok = 0;
    }
    if (!ok) return -1;
    g_w = k_modes[mode].m.w;
    g_h = k_modes[mode].m.h;
    g_mode = mode;
    for (int ch = 0; ch < MAX_CH; ++ch) {
        if (!g_ch[ch].scr.w) continue;
        g_ch[ch].tile = to_output(&g_ch[ch].scr);
        if (g_ch[ch].codec == VG_CODEC_NONE) continue;
        out_follow(ch);                         /* larger: a new decoder at a keyframe */
        if (port_place(ch) == 0) MI_DISP_ShowInputPort(DISP_LAYER, ch);
    }
    /* Without the UI scaled to it the mode is no use: the old one again. */
    if (sstar_fb_screen(g_w, g_h) < 0) {
        fprintf(stderr, "sigmastar: the UI layer is not scaled to %s\n", k_modes[mode].hdmi);
        return -1;
    }
    hdmi_output(k_modes[mode].hdmi);
    return 0;
}

int hal_display_set_mode(const vg_mode_t *m)
{
    int mode = mode_index(m);
    if (mode < 0 || g_mode < 0) return -1;
    if (mode == g_mode) return 0;
    pthread_mutex_lock(&g_lock);
    int old = g_mode, rc = switch_to(mode);
    if (rc < 0 && switch_to(old) < 0)
        fprintf(stderr, "sigmastar: could not go back to %s either\n", k_modes[old].hdmi);
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "sigmastar: display %s%s\n", k_modes[rc < 0 ? old : mode].hdmi,
            rc < 0 ? " (the new mode failed)" : "");
    return rc;
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
    /* Live view: a short queue, the newest picture wins (and one for the
     * zero channel while it reads them: grab_depth). */
    grab_depth(id);
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
    /* Another channel's takeover holds two numbers until its new decoder
     * has pictures: with 4 of them, a 2x2 page coming back from full
     * screen ran out, and the last channel waited for the core's retry
     * (10 s). Finish that takeover now; its port keeps the old picture
     * until the new decoder's first. */
    for (int o = 0; o < MAX_CH && !ids; ++o) {
        if (o == ch || !g_ch[o].nx.on) continue;
        nx_swap(o);
        for (int i = 0; i < g_vdec_ids && !ids; ++i) ids = !g_vdec_busy[i];
    }
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

/* ---- the zero channel: the video layer read back -------------------------------- */

/*
 * The i2m display has no writeback: MI_DISP_GetScreenFrame is in the API
 * but the SDK's mi_disp.ko answers it with an error without looking (its
 * MI_DISP_IMPL_GetScreenFrame is "return 31"), and the display HAL the
 * boot release has the source of reads nothing back. So each tile is read
 * from its decoder's output, the picture the display shows, through a
 * user queue of one on the decoder's output port (MI_SYS keeps the newest
 * picture there while hal_display_grab_keep is on), and drawn where the
 * port's window is (in fill, the part of it the display's zoom-in window
 * shows). A copy of each tile's last picture stays here, at the
 * window's size on the screen, for a decoder with nothing new (a still
 * camera) and as the place the scaling happens in cached memory: the
 * decoder's buffers are read a whole row at a time.
 */
static int g_grab;                  /* grab_keep: on */
static struct {
    unsigned char *pix;             /* y (w x h), then u and v ((w/2) x (h/2)) */
    unsigned       w, h;            /* 0: no picture yet */
    int            vdec;
    vg_rect_t      crop;            /* the part of the decoder's picture it is */
} g_gc[MAX_CH];
static pthread_mutex_t g_grab_lock = PTHREAD_MUTEX_INITIALIZER;     /* g_gc, the grab itself */

/* Locked (g_lock). */
static void grab_depth(int vdec)
{
    MI_SYS_ChnPort_t p = vdec_port(vdec);
    MI_SYS_SetChnOutputPortDepth(&p, g_grab ? 1 : 0, 4);
}

void hal_display_grab_keep(int on)
{
    pthread_mutex_lock(&g_grab_lock);
    pthread_mutex_lock(&g_lock);
    g_grab = on != 0;
    for (int ch = 0; ch < MAX_CH; ++ch) {
        if (g_ch[ch].codec != VG_CODEC_NONE) grab_depth(g_ch[ch].vdec);
        if (g_ch[ch].nx.on) grab_depth(g_ch[ch].nx.vdec);
    }
    pthread_mutex_unlock(&g_lock);
    if (!on)
        for (int ch = 0; ch < MAX_CH; ++ch) {
            free(g_gc[ch].pix);
            memset(&g_gc[ch], 0, sizeof g_gc[ch]);
        }
    pthread_mutex_unlock(&g_grab_lock);
}

/* The decoder's newest picture into the channel's copy, scaled to w x h
 * (nearest pixel): the part `crop` of it (port_zoom's, in the pixels of a
 * bw x bh picture; w == 0: all of it). 1 copied, 0 nothing new. */
static int grab_read(int ch, int vdec, unsigned w, unsigned h, const vg_rect_t *crop, unsigned bw, unsigned bh)
{
    MI_SYS_ChnPort_t port = vdec_port(vdec);
    MI_SYS_BufInfo_t bi, last;
    MI_SYS_BUF_HANDLE hb, held = 0;
    int have = 0;
    /* The queue is one deep; whatever else came, the newest. */
    for (int i = 0; i < 4; ++i) {
        memset(&bi, 0, sizeof bi);
        if (MI_SYS_ChnOutputPortGetBuf(&port, &bi, &hb) != MI_SUCCESS) break;
        if (have) MI_SYS_ChnOutputPortPutBuf(held);
        held = hb;
        last = bi;
        have = 1;
    }
    if (!have) return 0;
    const MI_SYS_FrameData_t *f = &last.stFrameData;
    int rc = 0;
    static int told;
    if (last.eBufType != E_MI_SYS_BUFDATA_FRAME || f->ePixelFormat != E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420 ||
        f->eTileMode != E_MI_SYS_FRAME_TILE_MODE_NONE || f->eCompressMode != E_MI_SYS_COMPRESS_MODE_NONE ||
        !f->pVirAddr[0] || !f->pVirAddr[1] || f->u16Width < 2 || f->u16Height < 2) {
        if (!told++)
            fprintf(stderr, "sigmastar: zero channel: decoder %d gives format %d tile %d compress %d, "
                    "not linear NV12: its tile stays black\n", ch, f->ePixelFormat, f->eTileMode, f->eCompressMode);
        MI_SYS_ChnOutputPortPutBuf(held);
        return 0;
    }
    if (g_gc[ch].w != w || g_gc[ch].h != h || !g_gc[ch].pix) {
        free(g_gc[ch].pix);
        g_gc[ch].pix = malloc((size_t)w * h * 3 / 2);
        g_gc[ch].w = g_gc[ch].pix ? w : 0;
        g_gc[ch].h = g_gc[ch].pix ? h : 0;
    }
    unsigned sw = f->u16Width & ~1u, sh = f->u16Height & ~1u;
    /* the part shown, even, in this picture's pixels (the size it was
     * worked out for, unless a picture of another size is still on its
     * way) */
    unsigned cx = 0, cy = 0, cw = sw, chh = sh;
    if (crop->w > 0 && crop->h > 0 && bw && bh) {
        cx = (unsigned)((unsigned long long)crop->x * sw / bw) & ~1u;
        cy = (unsigned)((unsigned long long)crop->y * sh / bh) & ~1u;
        cw = (unsigned)((unsigned long long)crop->w * sw / bw) & ~1u;
        chh = (unsigned)((unsigned long long)crop->h * sh / bh) & ~1u;
        if (cw < 2 || chh < 2 || cx + cw > sw || cy + chh > sh) cx = cy = 0, cw = sw, chh = sh;
    }
    unsigned char *row = g_gc[ch].pix ? malloc(f->u32Stride[0] > f->u32Stride[1] ? f->u32Stride[0] : f->u32Stride[1]) : NULL;
    unsigned *xm = row ? malloc(sizeof *xm * w) : NULL;
    if (xm) {
        /* The VPU writes these buffers behind the CPU's cache. */
        MI_SYS_FlushInvCache(f->pVirAddr[0], f->u32Stride[0] * sh);
        MI_SYS_FlushInvCache(f->pVirAddr[1], f->u32Stride[1] * (sh / 2));
        for (unsigned x = 0; x < w; ++x) xm[x] = cx + (unsigned)((unsigned long long)x * cw / w);
        unsigned char *dy = g_gc[ch].pix, *du = dy + (size_t)w * h, *dv = du + (size_t)(w / 2) * (h / 2);
        unsigned last_sy = ~0u;
        for (unsigned y = 0; y < h; ++y) {
            unsigned sy = cy + (unsigned)((unsigned long long)y * chh / h);
            if (sy != last_sy) memcpy(row, (unsigned char *)f->pVirAddr[0] + (size_t)sy * f->u32Stride[0], sw);
            last_sy = sy;
            for (unsigned x = 0; x < w; ++x) dy[(size_t)y * w + x] = row[xm[x]];
        }
        last_sy = ~0u;
        for (unsigned y = 0; y < h / 2; ++y) {
            unsigned sy = cy / 2 + (unsigned)((unsigned long long)y * (chh / 2) / (h / 2));
            if (sy != last_sy) memcpy(row, (unsigned char *)f->pVirAddr[1] + (size_t)sy * f->u32Stride[1], sw);
            last_sy = sy;
            for (unsigned x = 0; x < w / 2; ++x) {
                unsigned sx = xm[2 * x] & ~1u;
                du[(size_t)y * (w / 2) + x] = row[sx];
                dv[(size_t)y * (w / 2) + x] = row[sx + 1];
            }
        }
        g_gc[ch].vdec = vdec;
        g_gc[ch].crop = *crop;
        rc = 1;
    }
    free(xm);
    free(row);
    MI_SYS_ChnOutputPortPutBuf(held);
    return rc;
}

int hal_display_grab(const hal_pic_t *pic)
{
    struct { int on, vdec; vg_rect_t r, crop; unsigned bw, bh; } t[MAX_CH];
    pthread_mutex_lock(&g_lock);
    int grab = g_grab;
    for (int ch = 0; ch < MAX_CH; ++ch) {
        vg_rect_t w = g_ch[ch].win;
        t[ch].on = g_ch[ch].codec != VG_CODEC_NONE && g_ch[ch].scr.w && w.w > 0 && w.h > 0 && g_w && g_h;
        t[ch].vdec = g_ch[ch].vdec;
        t[ch].crop = g_ch[ch].crop;
        t[ch].bw = g_ch[ch].crop_bw;
        t[ch].bh = g_ch[ch].crop_bh;
        if (!t[ch].on) continue;
        /* the window on the output, as screen pixels of pic, even */
        int x0 = (int)((long long)w.x * pic->w / g_w), x1 = (int)((long long)(w.x + w.w) * pic->w / g_w);
        int y0 = (int)((long long)w.y * pic->h / g_h), y1 = (int)((long long)(w.y + w.h) * pic->h / g_h);
        x0 &= ~1;
        y0 &= ~1;
        x1 = x1 > (int)pic->w ? (int)pic->w : x1 & ~1;
        y1 = y1 > (int)pic->h ? (int)pic->h : y1 & ~1;
        t[ch].r = (vg_rect_t){ x0, y0, x1 - x0, y1 - y0 };
        t[ch].on = t[ch].r.w >= 2 && t[ch].r.h >= 2;
    }
    pthread_mutex_unlock(&g_lock);
    if (!grab) return -1;

    pthread_mutex_lock(&g_grab_lock);
    for (int ch = 0; ch < MAX_CH; ++ch) {
        if (!t[ch].on) continue;
        unsigned w = (unsigned)t[ch].r.w, h = (unsigned)t[ch].r.h;
        grab_read(ch, t[ch].vdec, w, h, &t[ch].crop, t[ch].bw, t[ch].bh);
        /* A picture of another decoder, of another size or another part of
         * it: not this one's. */
        const vg_rect_t *gc = &g_gc[ch].crop, *tc = &t[ch].crop;
        if (!g_gc[ch].pix || g_gc[ch].w != w || g_gc[ch].h != h || g_gc[ch].vdec != t[ch].vdec ||
            gc->x != tc->x || gc->y != tc->y || gc->w != tc->w || gc->h != tc->h)
            continue;
        const unsigned char *sy = g_gc[ch].pix, *su = sy + (size_t)w * h, *sv = su + (size_t)(w / 2) * (h / 2);
        unsigned x = (unsigned)t[ch].r.x, y = (unsigned)t[ch].r.y;
        for (unsigned r = 0; r < h; ++r) memcpy(pic->y + (size_t)(y + r) * pic->ys + x, sy + (size_t)r * w, w);
        for (unsigned r = 0; r < h / 2; ++r) {
            memcpy(pic->u + (size_t)(y / 2 + r) * pic->cs + x / 2, su + (size_t)r * (w / 2), w / 2);
            memcpy(pic->v + (size_t)(y / 2 + r) * pic->cs + x / 2, sv + (size_t)r * (w / 2), w / 2);
        }
    }
    pthread_mutex_unlock(&g_grab_lock);
    return 0;
}
