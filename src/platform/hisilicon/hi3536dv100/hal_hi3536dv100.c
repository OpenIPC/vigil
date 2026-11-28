/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hal_hi3536dv100.c — Hi3536DV100 (NVR SoC: decode + HDMI, no VI, no H.264
 * encoder), Hi3536DV100_SDK_V2.0.2.0.
 *
 * Per channel i:   VDEC chn i -> VPSS grp i -> VO chn i (tile i)
 *
 * The decoder is created for the stream's own size and reference count
 * (hal_dec_open, from the SPS), with its pictures in a private pool:
 * vdec.ko must be loaded with VBSource=1 (scripts/hisilicon/
 * load_mpp_hi3536dv100.sh), and HI_MPI_VDEC_SetChnVBCnt sizes the pool
 * before the channel exists. One shared pool sized for the largest camera
 * does not fit three cameras in the board's 128 MiB MMZ when two are 5 MP;
 * sized per camera, 1080p + 2 x 5 MP + 2 x H.264 took 87 MiB on the lab NVR.
 *
 * VPSS runs one channel in auto mode, which scales to the size of the VO
 * channel it is bound to; the VO channel places the picture inside its
 * rectangle at the display aspect ratio the core gives
 * (hal_display_set_aspect: ASPECT_RATIO_MANUAL with aspect_fit's
 * rectangle, black around it; NONE to stretch), or, before the core says,
 * at the picture's own (ASPECT_RATIO_AUTO). In fill the VPSS group crops
 * the decoded picture to its middle at the tile's shape (aspect_crop) and
 * the VO channel stretches that over the tile.
 *
 * Revision-2 Hi3536DV100 chips (SC_SYSID0 0x12050EE0 bits 31:24 = 2) need a
 * vdec.ko whose chip-revision check is bypassed: the stock SDK module reads
 * 0x12060024 and silently refuses frames when the value is above 1.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "hi_type.h"
#include "hi_common.h"
#include "hi_comm_sys.h"
#include "hi_comm_vb.h"
#include "hi_comm_vdec.h"
#include "hi_comm_vpss.h"
#include "hi_comm_vo.h"
#include "mpi_sys.h"
#include "mpi_vb.h"
#include "mpi_vdec.h"
#include "mpi_vpss.h"
#include "mpi_vo.h"

#include "aspect.h"
#include "hal.h"
#include "hal_capture.h"
#include "hal_decoder.h"
#include "hal_display.h"
#include "hal_audio.h"
#include "hi_display.h"

#define MAX_CH             4        /* quad view */
/* Pictures per decoder beyond its reference frames: the one being decoded
 * plus the one VPSS holds. */
#define PIC_BLKS_EXTRA     2
#define SEND_TIMEOUT_MS    20

static const hal_caps_t g_caps = {
    .name = "hi3536dv100", .display = 1, .decoder = 1,
    .dec_max_w = 3072, .dec_max_h = 3072,           /* VDH_H26xD_MAX_* */
    .max_channels = MAX_CH,
};

static struct {
    int        vo_up, vpss_up;
    vg_codec_t codec;                               /* VG_CODEC_NONE: no VDEC */
    vg_rect_t  tile;                                /* the VO channel's rectangle */
    int        asp_set;                             /* hal_display_set_aspect said: */
    vg_ratio_t asp;                                 /* 0:0 the picture's own size */
    vg_aspect_t how;
    unsigned   pic_w, pic_h;                        /* the decoder's picture (0: none yet) */
} g_ch[MAX_CH];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

const hal_caps_t *hal_caps(void) { return &g_caps; }
static vg_mode_t g_mode;                            /* what hal_init started */

int hal_init(unsigned w, unsigned h, const vg_mode_t *out)
{
    (void)out;                                      /* 720P60 / 1080P60 by the screen's size */
    /* The common pool only carries VPSS output at tile size (640x300
     * sp420); decoded pictures live in each decoder's private pool. */
    VB_CONF_S vb = {
        .u32MaxPoolCnt = 16,
        .astCommPool   = { { .u32BlkSize = 640 * 320 * 3 / 2, .u32BlkCnt = 24, .acMmzName = "" } },
    };
    if (hi_display_start(&vb, VO_INTF_HDMI, h >= 1080 ? VO_OUTPUT_1080P60 : VO_OUTPUT_720P60, w, h) < 0) return -1;
    g_mode = (vg_mode_t){ w, h, 60 };
    return 0;
}

void hal_shutdown(void)
{
    for (int i = 0; i < MAX_CH; ++i) {
        hal_dec_close(i);
        hal_display_clear_tile(i);
    }
    hi_display_stop();
}

/* ---- display ------------------------------------------------------------- */

/* The picture's place in VO channel ch: stVideoRect is relative to the
 * channel's rectangle. */
static void chn_aspect(int ch)
{
    VO_CHN_PARAM_S vp;
    if (HI_MPI_VO_GetChnParam(HI_DISPLAY_VO_LAYER, ch, &vp) != HI_SUCCESS) return;
    vp.stAspectRatio.u32BgColor = 0x000000;
    vg_ratio_t a = g_ch[ch].asp.w ? g_ch[ch].asp : (vg_ratio_t){ g_ch[ch].pic_w, g_ch[ch].pic_h };
    if (!g_ch[ch].asp_set || (g_ch[ch].how == VG_ASPECT_FIT && !a.w)) {
        vp.stAspectRatio.enMode = ASPECT_RATIO_AUTO;
    } else if (g_ch[ch].how != VG_ASPECT_FIT) {
        vp.stAspectRatio.enMode = ASPECT_RATIO_NONE;  /* stretched, or the crop over the tile */
    } else {
        vg_rect_t w = aspect_fit(&g_ch[ch].tile, a, 2);
        vp.stAspectRatio.enMode = ASPECT_RATIO_MANUAL;
        vp.stAspectRatio.stVideoRect.s32X = w.x;
        vp.stAspectRatio.stVideoRect.s32Y = w.y;
        vp.stAspectRatio.stVideoRect.u32Width = (HI_U32)w.w;
        vp.stAspectRatio.stVideoRect.u32Height = (HI_U32)w.h;
    }
    HI_S32 rc = HI_MPI_VO_SetChnParam(HI_DISPLAY_VO_LAYER, ch, &vp);
    if (rc != HI_SUCCESS) fprintf(stderr, "hi3536dv100: VO_SetChnParam(%d) aspect 0x%x\n", ch, rc);
}

/* In fill, the VPSS group's crop: the middle of the decoded picture at the
 * tile's shape, in the picture's pixels, multiples of 4 (the coarser of
 * what the SDK's VPSS takes for an offset); off otherwise. Locked. */
static void grp_crop(int ch)
{
    if (!g_ch[ch].vpss_up || !g_ch[ch].pic_w) return;
    VPSS_CROP_INFO_S ci;
    memset(&ci, 0, sizeof ci);
    ci.enCropCoordinate = VPSS_CROP_ABS_COOR;
    if (g_ch[ch].asp_set && g_ch[ch].how == VG_ASPECT_FILL && g_ch[ch].tile.w) {
        vg_rect_t c = aspect_crop(g_ch[ch].pic_w, g_ch[ch].pic_h, g_ch[ch].asp, (unsigned)g_ch[ch].tile.w,
                                  (unsigned)g_ch[ch].tile.h, 4);
        ci.bEnable = c.w != (int)g_ch[ch].pic_w || c.h != (int)g_ch[ch].pic_h ? HI_TRUE : HI_FALSE;
        ci.stCropRect = (RECT_S){ c.x, c.y, (HI_U32)c.w, (HI_U32)c.h };
    }
    HI_S32 rc = HI_MPI_VPSS_SetGrpCrop(ch, &ci);
    if (rc != HI_SUCCESS) fprintf(stderr, "hi3536dv100: VPSS_SetGrpCrop(%d) 0x%x\n", ch, rc);
}

void hal_display_set_aspect(int ch, unsigned w, unsigned h, vg_aspect_t how)
{
    if (ch < 0 || ch >= MAX_CH) return;
    pthread_mutex_lock(&g_lock);                    /* a channel's thread, while tiles move */
    g_ch[ch].asp_set = 1;
    g_ch[ch].asp = w && h ? (vg_ratio_t){ w, h } : (vg_ratio_t){ 0, 0 };
    g_ch[ch].how = how;
    if (g_ch[ch].tile.w) chn_aspect(ch);
    grp_crop(ch);
    pthread_mutex_unlock(&g_lock);
}

int hal_display_set_tile(int ch, const vg_rect_t *r)
{
    if (ch < 0 || ch >= MAX_CH) return -1;
    VO_CHN_ATTR_S va;
    memset(&va, 0, sizeof va);
    va.stRect.s32X = r->x;
    va.stRect.s32Y = r->y;
    va.stRect.u32Width = (HI_U32)r->w & ~1u;
    va.stRect.u32Height = (HI_U32)r->h & ~1u;
    HI_S32 rc = HI_MPI_VO_SetChnAttr(HI_DISPLAY_VO_LAYER, ch, &va);
    if (rc != HI_SUCCESS) {
        fprintf(stderr, "hi3536dv100: VO_SetChnAttr(%d) 0x%x\n", ch, rc);
        return -1;
    }
    pthread_mutex_lock(&g_lock);
    g_ch[ch].tile = (vg_rect_t){ 0, 0, (int)va.stRect.u32Width, (int)va.stRect.u32Height };
    chn_aspect(ch);
    grp_crop(ch);
    pthread_mutex_unlock(&g_lock);
    /* A move (another layout, full screen) changes the attributes of a
     * channel that is already enabled. */
    if (!g_ch[ch].vo_up && (rc = HI_MPI_VO_EnableChn(HI_DISPLAY_VO_LAYER, ch)) != HI_SUCCESS) {
        fprintf(stderr, "hi3536dv100: VO_EnableChn(%d) 0x%x\n", ch, rc);
        return -1;
    }
    g_ch[ch].vo_up = 1;
    return 0;
}

void hal_display_clear_tile(int ch)
{
    if (ch < 0 || ch >= MAX_CH || !g_ch[ch].vo_up) return;
    HI_MPI_VO_DisableChn(HI_DISPLAY_VO_LAYER, ch);
    g_ch[ch].vo_up = 0;
}

/* ---- decoder --------------------------------------------------------------- */

static void dec_teardown(int ch)
{
    if (g_ch[ch].codec != VG_CODEC_NONE) {
        MPP_CHN_S vdec = { .enModId = HI_ID_VDEC, .s32DevId = 0, .s32ChnId = ch };
        MPP_CHN_S vpss = { .enModId = HI_ID_VPSS, .s32DevId = ch, .s32ChnId = 0 };
        HI_MPI_SYS_UnBind(&vdec, &vpss);
        HI_MPI_VDEC_StopRecvStream(ch);
        HI_MPI_VDEC_DestroyChn(ch);
        g_ch[ch].codec = VG_CODEC_NONE;
    }
    if (g_ch[ch].vpss_up) {
        MPP_CHN_S vpss = { .enModId = HI_ID_VPSS, .s32DevId = ch, .s32ChnId = 0 };
        MPP_CHN_S vo   = { .enModId = HI_ID_VOU, .s32DevId = HI_DISPLAY_VO_LAYER, .s32ChnId = ch };
        HI_MPI_SYS_UnBind(&vpss, &vo);
        HI_MPI_VPSS_StopGrp(ch);
        HI_MPI_VPSS_DisableChn(ch, 0);
        HI_MPI_VPSS_DestroyGrp(ch);
        g_ch[ch].vpss_up = 0;
    }
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
    HI_S32 rc;
    /* Create/destroy of MPP objects is serialized across channel threads. */
    pthread_mutex_lock(&g_lock);
    dec_teardown(ch);

    VPSS_GRP_ATTR_S ga;
    memset(&ga, 0, sizeof ga);
    ga.u32MaxW   = (si->width + 15) & ~15u;
    ga.u32MaxH   = (si->height + 15) & ~15u;
    ga.enPixFmt  = PIXEL_FORMAT_YUV_SEMIPLANAR_420;
    ga.enDieMode = VPSS_DIE_MODE_NODIE;
    VPSS_CHN_ATTR_S ca;
    memset(&ca, 0, sizeof ca);
    if ((rc = HI_MPI_VPSS_CreateGrp(ch, &ga)) != HI_SUCCESS) {
        fprintf(stderr, "hi3536dv100: VPSS_CreateGrp(%d) 0x%x\n", ch, rc);
        goto fail;
    }
    g_ch[ch].vpss_up = 1;
    g_ch[ch].pic_w = si->width;
    g_ch[ch].pic_h = si->height;
    grp_crop(ch);
    if (g_ch[ch].tile.w) chn_aspect(ch);            /* 0:0 is the new picture's own shape */
    MPP_CHN_S vpss = { .enModId = HI_ID_VPSS, .s32DevId = ch, .s32ChnId = 0 };
    MPP_CHN_S vo   = { .enModId = HI_ID_VOU, .s32DevId = HI_DISPLAY_VO_LAYER, .s32ChnId = ch };
    if ((rc = HI_MPI_VPSS_SetChnAttr(ch, 0, &ca)) != HI_SUCCESS ||
        (rc = HI_MPI_VPSS_EnableChn(ch, 0)) != HI_SUCCESS ||
        (rc = HI_MPI_VPSS_StartGrp(ch)) != HI_SUCCESS ||
        (rc = HI_MPI_SYS_Bind(&vpss, &vo)) != HI_SUCCESS) {
        fprintf(stderr, "hi3536dv100: VPSS %d start 0x%x\n", ch, rc);
        goto fail;
    }

    PAYLOAD_TYPE_E pt = codec == VG_CODEC_H264 ? PT_H264 : PT_H265;
    VDEC_CHN_ATTR_S a;
    memset(&a, 0, sizeof a);
    a.enType       = pt;
    /* The smallest stream buffer vdec.ko takes is half a raw picture
     * ("Buffer size ... is not enough" below that), whatever the bitrate;
     * it also sizes the decoder's SCD buffer, so stay at the minimum. */
    a.u32BufSize   = si->width * si->height * 3 / 4;
    a.u32Priority  = 5;
    a.u32PicWidth  = si->width;
    a.u32PicHeight = si->height;
    a.stVdecVideoAttr.enMode             = VIDEO_MODE_FRAME;
    a.stVdecVideoAttr.u32RefFrameNum     = si->refs;
    a.stVdecVideoAttr.bTemporalMvpEnable = codec == VG_CODEC_H265;

    HI_U32 blk;
    VB_PIC_BLK_SIZE(si->width, si->height, pt, blk);
    unsigned n_blk = si->refs + PIC_BLKS_EXTRA;

    HI_MPI_VDEC_SetChnVBCnt(ch, n_blk);
    if ((rc = HI_MPI_VDEC_CreateChn(ch, &a)) != HI_SUCCESS) {
        fprintf(stderr, "hi3536dv100: VDEC_CreateChn(%d) %ux%u: 0x%x (needs ~%u MiB of MMZ)\n",
                ch, si->width, si->height, rc, (blk * n_blk + a.u32BufSize) >> 20);
        goto fail;
    }
    g_ch[ch].codec = codec;                         /* from here dec_teardown destroys it */
    VDEC_PRTCL_PARAM_S pp;
    if (HI_MPI_VDEC_GetProtocolParam(ch, &pp) == HI_SUCCESS) {
        pp.enType = pt;
        if (codec == VG_CODEC_H264) {
            pp.stH264PrtclParam.s32MaxSpsNum   = 32;
            pp.stH264PrtclParam.s32MaxPpsNum   = 256;
            pp.stH264PrtclParam.s32MaxSliceNum = 100;
        } else {
            pp.stH265PrtclParam.s32MaxVpsNum          = 16;
            pp.stH265PrtclParam.s32MaxSpsNum          = 16;
            pp.stH265PrtclParam.s32MaxPpsNum          = 64;
            pp.stH265PrtclParam.s32MaxSliceSegmentNum = 100;
        }
        HI_MPI_VDEC_SetProtocolParam(ch, &pp);
    }
    /* Live view: show the newest picture, drop late ones. */
    HI_MPI_VDEC_SetDisplayMode(ch, VIDEO_DISPLAY_MODE_PREVIEW);

    MPP_CHN_S vdec = { .enModId = HI_ID_VDEC, .s32DevId = 0, .s32ChnId = ch };
    if ((rc = HI_MPI_SYS_Bind(&vdec, &vpss)) != HI_SUCCESS ||
        (rc = HI_MPI_VDEC_StartRecvStream(ch)) != HI_SUCCESS) {
        fprintf(stderr, "hi3536dv100: VDEC %d start 0x%x\n", ch, rc);
        goto fail;
    }
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "hi3536dv100: decoder %d: %ux%u, %u pictures, ~%u MiB\n",
            ch, si->width, si->height, n_blk, (blk * n_blk + a.u32BufSize) >> 20);
    return 0;
fail:
    dec_teardown(ch);
    pthread_mutex_unlock(&g_lock);
    return -1;
}

int hal_dec_send(int ch, const vg_au_t *au)
{
    if (ch < 0 || ch >= MAX_CH || g_ch[ch].codec == VG_CODEC_NONE) return -1;
    VDEC_STREAM_S s = {
        .pu8Addr      = (HI_U8 *)au->data,
        .u32Len       = (HI_U32)au->len,
        .u64PTS       = au->pts_us,
        .bEndOfFrame  = HI_TRUE,
        .bEndOfStream = HI_FALSE,
    };
    return HI_MPI_VDEC_SendStream(ch, &s, SEND_TIMEOUT_MS) == HI_SUCCESS ? 0 : -1;
}

void hal_dec_reset(int ch)
{
    if (ch < 0 || ch >= MAX_CH || g_ch[ch].codec == VG_CODEC_NONE) return;
    HI_MPI_VDEC_StopRecvStream(ch);
    HI_MPI_VDEC_ResetChn(ch);
    HI_MPI_VDEC_StartRecvStream(ch);
}

/* ---- capture: none on this SoC ------------------------------------------------ */

int  hal_cap_start(int in, int ch, hal_cap_cb cb, void *user) { (void)in; (void)ch; (void)cb; (void)user; return -1; }
void hal_cap_stop(int in) { (void)in; }
int  hal_cap_signal(int in) { (void)in; return 0; }

/* ---- output mode: the one hal_init started; it does not change ---------------- */

int hal_display_modes(vg_mode_t *out, int max)
{
    if (max < 1 || !g_mode.w) return 0;
    *out = g_mode;
    return 1;
}

void hal_display_mode(vg_mode_t *cur) { *cur = g_mode; }

int hal_display_set_mode(const vg_mode_t *m)
{
    return m->w == g_mode.w && m->h == g_mode.h && m->hz == g_mode.hz ? 0 : -1;
}

/* No picture to read back: the zero channel shows the UI over black. */
void hal_display_grab_keep(int on) { (void)on; }
int  hal_display_grab(const hal_pic_t *pic) { (void)pic; return -1; }
int  hal_display_still(int ch, hal_pic_t *pic, int timeout_ms) { (void)ch; (void)pic; (void)timeout_ms; return -1; }

/* No margins, picture controls or HDMI sound here yet (hal_caps: 0). */
int hal_display_set_margins(const vg_margins_t *m) { (void)m; return -1; }
int hal_display_set_picture(const vg_picture_t *p) { (void)p; return -1; }
int hal_audio_set_volume(unsigned volume, int mute) { (void)volume; (void)mute; return -1; }
