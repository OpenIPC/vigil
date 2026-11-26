/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hal_sigmastar.c — SigmaStar infinity2m NVRs (SSR621Q), on the i2m NVR
 * SDK's MI libraries (2020-03).
 *
 * Per channel i:   MI_VDEC chn i -> MI_DISP layer 0, input port i (tile i)
 *
 * The display drives HDMI and VGA from the same timing. The SDK has no
 * mi_hdmi: the HDMI transmitter is opensigmastar's open_hdmitx.ko, switched
 * on through open_hdmi.ko's sysfs file once the display runs.
 *
 * The decoder's own scaler only shrinks, so it outputs the picture at the
 * tile size (or the picture's, if smaller) and the display scales it into
 * the tile. The display does not keep the aspect ratio: the tile is filled.
 *
 * mi_disp needs the panel timings, PQ tables and memory map from /config,
 * and mi_vdec its firmware from /config/vdec_fw (paths built into the
 * modules); OpenIPC's sigmastar-osdrv-infinity2m installs them.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "mi_common_datatype.h"
#include "mi_sys.h"
#include "mi_vdec.h"
#include "mi_disp.h"

#include "hal.h"
#include "hal_capture.h"
#include "hal_decoder.h"
#include "hal_display.h"

#define MAX_CH          4           /* quad view */
#define DISP_DEV        0
#define DISP_LAYER      0
/* The bitstream buffer: a 5 MP keyframe fits. */
#define VDEC_BUF_SIZE   (1024 * 1024)
#define SEND_TIMEOUT_MS 20
#define SEND_TRIES      5           /* ~100 ms while the decoder catches up */
#define HDMI_SYSFS      "/sys/kernel/open_hdmi/output"

static const hal_caps_t g_caps = {
    .name = "sigmastar", .display = 1, .decoder = 1,
    /* the i2m VPU: up to 5 MP (2592x1944) H.264 and H.265 */
    .dec_max_w = 2592, .dec_max_h = 2592,
    .max_channels = MAX_CH,
};

static struct {
    vg_codec_t codec;               /* VG_CODEC_NONE: no decoder */
    vg_rect_t  tile;                /* w == 0: not placed */
    unsigned   src_w, src_h;        /* what the decoder outputs */
} g_ch[MAX_CH];
static unsigned g_w, g_h;
/* MI object create/destroy and port changes, across channel threads */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

const hal_caps_t *hal_caps(void) { return &g_caps; }

static unsigned align_down(unsigned v, unsigned a) { return v / a * a; }
static unsigned umin(unsigned a, unsigned b) { return a < b ? a : b; }

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

/* Channel ch's input port at its tile, fed src_w x src_h. Locked. */
static int port_place(int ch)
{
    MI_DISP_InputPortAttr_t pa;
    memset(&pa, 0, sizeof pa);
    pa.u16SrcWidth = g_ch[ch].src_w;
    pa.u16SrcHeight = g_ch[ch].src_h;
    pa.stDispWin.u16X = g_ch[ch].tile.x;
    pa.stDispWin.u16Y = g_ch[ch].tile.y;
    pa.stDispWin.u16Width = g_ch[ch].tile.w;
    pa.stDispWin.u16Height = g_ch[ch].tile.h;
    MI_S32 rc = MI_DISP_SetInputPortAttr(DISP_LAYER, ch, &pa);
    if (rc != MI_SUCCESS) {
        fprintf(stderr, "sigmastar: MI_DISP_SetInputPortAttr(%d) 0x%x\n", ch, rc);
        return -1;
    }
    return 0;
}

int hal_display_set_tile(int ch, const vg_rect_t *r)
{
    if (ch < 0 || ch >= MAX_CH || r->w <= 0 || r->h <= 0) return -1;
    pthread_mutex_lock(&g_lock);
    /* the display's windows: x on 16 pixels, sizes even */
    g_ch[ch].tile.x = r->x & ~15;
    g_ch[ch].tile.y = r->y & ~1;
    g_ch[ch].tile.w = (r->w + (r->x & 15)) & ~15;
    g_ch[ch].tile.h = r->h & ~1;
    int rc = 0;
    if (g_ch[ch].codec != VG_CODEC_NONE) {      /* a decoder feeds the port */
        rc = port_place(ch);
        MI_DISP_ShowInputPort(DISP_LAYER, ch);
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

static MI_SYS_ChnPort_t vdec_port(int ch)
{
    MI_SYS_ChnPort_t p;
    memset(&p, 0, sizeof p);
    p.eModId = E_MI_MODULE_ID_VDEC;
    p.u32ChnId = ch;
    return p;
}

static MI_SYS_ChnPort_t disp_port(int ch)
{
    MI_SYS_ChnPort_t p;
    memset(&p, 0, sizeof p);
    p.eModId = E_MI_MODULE_ID_DISP;
    p.u32DevId = DISP_DEV;
    p.u32ChnId = ch;                            /* layer 0's input port ch */
    return p;
}

/* Locked. */
static void dec_teardown(int ch)
{
    if (g_ch[ch].codec == VG_CODEC_NONE) return;
    MI_SYS_ChnPort_t src = vdec_port(ch), dst = disp_port(ch);
    MI_SYS_UnBindChnPort(&src, &dst);
    MI_DISP_DisableInputPort(DISP_LAYER, ch);
    MI_VDEC_StopChn(ch);
    MI_VDEC_DestroyChn(ch);
    g_ch[ch].codec = VG_CODEC_NONE;
}

void hal_dec_close(int ch)
{
    if (ch < 0 || ch >= MAX_CH) return;
    pthread_mutex_lock(&g_lock);
    dec_teardown(ch);
    pthread_mutex_unlock(&g_lock);
}

int hal_dec_open(int ch, vg_codec_t codec, const vg_stream_info_t *si)
{
    if (ch < 0 || ch >= MAX_CH || codec == VG_CODEC_NONE) return -1;
    if (si->width > g_caps.dec_max_w || si->height > g_caps.dec_max_h) return -1;
    MI_S32 rc;
    pthread_mutex_lock(&g_lock);
    dec_teardown(ch);

    MI_VDEC_ChnAttr_t ca;
    memset(&ca, 0, sizeof ca);
    ca.eCodecType = codec == VG_CODEC_H265 ? E_MI_VDEC_CODEC_TYPE_H265 : E_MI_VDEC_CODEC_TYPE_H264;
    ca.u32BufSize = VDEC_BUF_SIZE;
    ca.u32PicWidth = si->width;
    ca.u32PicHeight = si->height;
    ca.eVideoMode = E_MI_VDEC_VIDEO_MODE_FRAME;
    ca.eDpbBufMode = E_MI_VDEC_DPB_MODE_NORMAL;
    ca.stVdecVideoAttr.u32RefFrameNum = si->refs ? si->refs : 1;
    if ((rc = MI_VDEC_CreateChn(ch, &ca)) != MI_SUCCESS) {
        fprintf(stderr, "sigmastar: MI_VDEC_CreateChn(%d) %ux%u 0x%x\n", ch, si->width, si->height, rc);
        goto fail;
    }
    if ((rc = MI_VDEC_StartChn(ch)) != MI_SUCCESS) {
        fprintf(stderr, "sigmastar: MI_VDEC_StartChn(%d) 0x%x\n", ch, rc);
        MI_VDEC_DestroyChn(ch);
        goto fail;
    }
    g_ch[ch].codec = codec;                     /* from here dec_teardown undoes it */

    /* Decode straight to the tile's size: the decoder only shrinks, the
     * display scales up whatever is left. Not placed yet: a quarter. */
    unsigned tw = g_ch[ch].tile.w ? (unsigned)g_ch[ch].tile.w : g_w / 2;
    unsigned th = g_ch[ch].tile.h ? (unsigned)g_ch[ch].tile.h : g_h / 2;
    MI_VDEC_OutputPortAttr_t oa;
    memset(&oa, 0, sizeof oa);
    oa.u16Width = align_down(umin(tw, si->width), 16);
    oa.u16Height = align_down(umin(th, si->height), 2);
    if (MI_VDEC_SetOutputPortAttr(ch, &oa) != MI_SUCCESS) {
        oa.u16Width = si->width;                /* the picture as decoded */
        oa.u16Height = si->height;
    }
    g_ch[ch].src_w = oa.u16Width;
    g_ch[ch].src_h = oa.u16Height;

    MI_SYS_ChnPort_t src = vdec_port(ch), dst = disp_port(ch);
    /* Live view: a short queue, the newest picture wins. */
    MI_SYS_SetChnOutputPortDepth(&src, 0, 4);

    if (!g_ch[ch].tile.w) {
        g_ch[ch].tile.w = (int)tw;
        g_ch[ch].tile.h = (int)th;
        port_place(ch);
        g_ch[ch].tile.w = 0;
    } else {
        port_place(ch);
    }
    if ((rc = MI_DISP_EnableInputPort(DISP_LAYER, ch)) != MI_SUCCESS ||
        (rc = MI_DISP_SetInputPortSyncMode(DISP_LAYER, ch, E_MI_DISP_SYNC_MODE_FREE_RUN)) != MI_SUCCESS ||
        (rc = MI_SYS_BindChnPort(&src, &dst, 30, 30)) != MI_SUCCESS) {
        fprintf(stderr, "sigmastar: channel %d to the display 0x%x\n", ch, rc);
        goto fail;
    }
    if (!g_ch[ch].tile.w)
        MI_DISP_HideInputPort(DISP_LAYER, ch);
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "sigmastar: decoder %d: %s %ux%u -> %ux%u\n", ch, vg_codec_name(codec),
            si->width, si->height, g_ch[ch].src_w, g_ch[ch].src_h);
    return 0;
fail:
    dec_teardown(ch);
    pthread_mutex_unlock(&g_lock);
    return -1;
}

int hal_dec_send(int ch, const vg_au_t *au)
{
    if (ch < 0 || ch >= MAX_CH || g_ch[ch].codec == VG_CODEC_NONE) return -1;
    MI_VDEC_VideoStream_t s;
    memset(&s, 0, sizeof s);
    s.pu8Addr = (MI_U8 *)au->data;
    s.u32Len = (MI_U32)au->len;
    s.u64PTS = au->pts_us;
    s.bEndOfFrame = 1;
    /* The bitstream buffer fills while the decoder lags: wait a little. */
    for (int i = 0; i < SEND_TRIES; ++i) {
        MI_S32 rc = MI_VDEC_SendStream(ch, &s, SEND_TIMEOUT_MS);
        if (rc == MI_SUCCESS) return 0;
        if (rc != MI_ERR_VDEC_BUF_FULL) break;
    }
    return -1;
}

void hal_dec_reset(int ch)
{
    if (ch < 0 || ch >= MAX_CH) return;
    pthread_mutex_lock(&g_lock);
    if (g_ch[ch].codec != VG_CODEC_NONE) {
        MI_VDEC_StopChn(ch);
        MI_VDEC_ResetChn(ch);
        MI_VDEC_StartChn(ch);
        /* the old picture must not stay up */
        MI_DISP_ClearInputPortBuffer(DISP_LAYER, ch, 1);
    }
    pthread_mutex_unlock(&g_lock);
}

/* ---- capture: none on this SoC --------------------------------------------- */

int  hal_cap_start(int in, int ch, hal_cap_cb cb, void *user) { (void)in; (void)ch; (void)cb; (void)user; return -1; }
void hal_cap_stop(int in) { (void)in; }
int  hal_cap_signal(int in) { (void)in; return 0; }
