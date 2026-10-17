/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hi_video_pipe.c — VI (NVP6114 BT656 2MUX 720p25) → VPSS → VO_CHN.
 *
 * Wires the vendor MPP video pipeline up to the 4 tile windows of the
 * LVGL home screen so live (or no-signal) camera output appears under
 * the LVGL graphics overlay via VOU hardware compositing.
 *
 * Written for vigil against the MPP V2.0.5.1 API. It handles the one case
 * this board uses: 4 cameras x 720p25 through the NVP6114 in BT656 2-mux
 * mode, channels 0/2/4/6 across VI devices 0 and 1. The VI device values
 * are the NVP6114's hardware parameters for that mode; the vendor's
 * SAMPLE_VI_MODE_8_720P is where they are documented.
 *
 * Status: builds; not run on hardware since the move into vigil, and VENC
 * channel creation is known to fail on the lab DVR (see venc_bringup).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hi_type.h"
#include "hi_common.h"
#include "hi_comm_sys.h"
#include "hi_comm_vb.h"
#include "hi_comm_vi.h"
#include "hi_comm_vpss.h"
#include "hi_comm_vo.h"
#include "hi_comm_venc.h"
#include "hi_comm_rc.h"
#include "mpi_sys.h"
#include "mpi_vi.h"
#include "mpi_vpss.h"
#include "mpi_vo.h"
#include "mpi_venc.h"

#include "hi_video_pipe.h"

/* PAYLOAD_TYPE_E is referenced by VENC_ATTR_S.enType in the V2.0.5.1
 * headers but the enum definition itself isn't shipped. We only need the
 * H.264 constant; values match Hisilicon's standard (== RTP payload types
 * per RFC 3551, with PT_H264=96). */
#ifndef PT_H264
#define PT_H264  96
#endif

#define VO_DEV_DHD0     0
#define VO_LAYER_VHD0   0

/* Channels selected by the BT656 2MUX layout: dev 0 -> ch 0,2; dev 1 -> ch 4,6 */
static const int g_vi_chn[HI_VIDEO_PIPE_CHANNELS] = { 0, 2, 4, 6 };
static const int g_vpss_grp[HI_VIDEO_PIPE_CHANNELS] = { 0, 1, 2, 3 };
#define VPSS_CHN_OUT    0      /* one output channel per group */

/* NVP6114 720p BT656 2-mux device attributes in the V2.0.5.1
 * VI_DEV_ATTR_S layout (no enClkEdge, single s32AdChnId[4]). */
static const VI_DEV_ATTR_S DEV_ATTR_NVP6114_720P_2MUX = {
    .enIntfMode    = VI_MODE_BT656,
    .enWorkMode    = VI_WORK_MODE_2Multiplex,
    .au32CompMask  = { 0xFF000000, 0x0 },
    .enScanMode    = VI_SCAN_PROGRESSIVE,
    .s32AdChnId    = { -1, -1, -1, -1 },
    .enDataSeq     = VI_INPUT_DATA_YVYU,
    .stSynCfg      = {
        .enVsync         = VI_VSYNC_FIELD,
        .enVsyncNeg      = VI_VSYNC_NEG_HIGH,
        .enHsync         = VI_HSYNC_VALID_SINGNAL,
        .enHsyncNeg      = VI_HSYNC_NEG_HIGH,
        .enVsyncValid    = VI_VSYNC_VALID_SINGAL,
        .enVsyncValidNeg = VI_VSYNC_VALID_NEG_HIGH,
        .stTimingBlank   = {0},
    },
    .enDataPath    = VI_PATH_BYPASS,
    .enInputDataType = VI_DATA_TYPE_YUV,
    .bDataRev      = HI_FALSE,
};

/* Track what we successfully brought up so teardown can be selective. */
static struct {
    int vi_devs_up;        /* bitmask of VI devs enabled (bit i for dev i) */
    int vi_chns_up;        /* bitmask of VI chns enabled (4 bits, idx into g_vi_chn) */
    int vpss_grps_up;      /* bitmask of VPSS grps created+started */
    int vpss_chns_up;      /* bitmask of VPSS display chns (chn 0) enabled */
    int vpss_enc_chns_up;  /* bitmask of VPSS encode chns (chn 1) enabled */
    int vo_chns_up;        /* bitmask of VO chns enabled */
    int venc_chns_up;      /* bitmask of VENC chns created+receiving */
    int bound_vi_vpss;     /* bitmask of VI->VPSS binds done */
    int bound_vpss_vo;     /* bitmask of VPSS->VO binds done */
    int bound_vpss_venc;   /* bitmask of VPSS->VENC binds done */
} g_state;

#define VPSS_CHN_ENC    1      /* encode-output channel per group */
/* One VPSS channel feeds both the VO tile and VENC (SYS_Bind fan-out), and
 * V2 VENC has no scaler: both receivers must take the same size. H.264
 * needs 16-pixel alignment, so the encode size is the tile rounded down
 * to 16 (640x288 for a 640x300 slot), and the VO tile is set to exactly
 * that, centred in its slot (compute_tile_rect). */
#define ENCODE_W        640
#define ENCODE_H        288
#define ENCODE_FPS      25
#define ENCODE_BITRATE  256    /* kbps per channel */
#define ENCODE_GOP      50

static int vi_bringup(void)
{
    HI_S32 rc;
    int got_any = 0;

    /* 2 VI devs (0, 1) — each handles 2 NVP6114 channels in BT656 2MUX. */
    for (int dev = 0; dev < 2; ++dev) {
        VI_DEV_ATTR_S a = DEV_ATTR_NVP6114_720P_2MUX;
        rc = HI_MPI_VI_SetDevAttr(dev, &a);
        if (rc != HI_SUCCESS) {
            fprintf(stderr, "VI: HI_MPI_VI_SetDevAttr(%d) 0x%x — skipping dev\n", dev, rc);
            continue;
        }
        rc = HI_MPI_VI_EnableDev(dev);
        if (rc != HI_SUCCESS) {
            fprintf(stderr, "VI: HI_MPI_VI_EnableDev(%d) 0x%x — skipping dev\n", dev, rc);
            continue;
        }
        g_state.vi_devs_up |= (1 << dev);
        got_any = 1;
    }

    if (!got_any) {
        fprintf(stderr, "VI: no VI devs came up (NVP6114 likely reports 0 cameras).\n");
        return -1;
    }

    /* 4 channels at 720p25, full-frame capture, output 720p (let VPSS scale).
     * For SAMPLE_VI_MODE_8_720P, the 2MUX layout puts dev[0]: chn 0+2 and
     * dev[1]: chn 4+6. The secondary chn of each dev (i.e. chn % 4 != 0)
     * must be ChnBind'd to that dev's ViWay 1 before SetChnAttr — otherwise
     * SetChnAttr returns 0xa0108047 (channel not supported on this way). */
    for (int i = 0; i < HI_VIDEO_PIPE_CHANNELS; ++i) {
        int chn = g_vi_chn[i];
        int dev = i / 2;
        if (!(g_state.vi_devs_up & (1 << dev))) continue;

        if (chn % 4 != 0) {
            VI_CHN_BIND_ATTR_S bind = { .ViDev = dev, .ViWay = 1 };
            rc = HI_MPI_VI_ChnBind(chn, &bind);
            if (rc != HI_SUCCESS && rc != (HI_S32)0xa0108029 /* already-bound */) {
                fprintf(stderr, "VI: ChnBind(%d -> dev%d/way1) 0x%x\n", chn, dev, rc);
            }
        }

        VI_CHN_ATTR_S a = {0};
        a.stCapRect.s32X      = 0;
        a.stCapRect.s32Y      = 0;
        a.stCapRect.u32Width  = 1280;
        a.stCapRect.u32Height = 720;
        a.stDestSize.u32Width  = 1280;
        a.stDestSize.u32Height = 720;
        a.enCapSel    = VI_CAPSEL_BOTH;
        a.enPixFormat = PIXEL_FORMAT_YUV_SEMIPLANAR_420;
        a.bMirror     = HI_FALSE;
        a.bFlip       = HI_FALSE;
        a.s32SrcFrameRate = -1;
        a.s32FrameRate    = -1;
        a.bChromaResample = HI_FALSE;

        rc = HI_MPI_VI_SetChnAttr(chn, &a);
        if (rc != HI_SUCCESS) {
            fprintf(stderr, "VI: SetChnAttr(%d) 0x%x\n", chn, rc);
            continue;
        }
        rc = HI_MPI_VI_EnableChn(chn);
        if (rc != HI_SUCCESS) {
            fprintf(stderr, "VI: EnableChn(%d) 0x%x\n", chn, rc);
            continue;
        }
        g_state.vi_chns_up |= (1 << i);
    }

    return g_state.vi_chns_up ? 0 : -1;
}

static int vpss_bringup(int tile_w, int tile_h)
{
    HI_S32 rc;
    int got_any = 0;

    for (int i = 0; i < HI_VIDEO_PIPE_CHANNELS; ++i) {
        int grp = g_vpss_grp[i];

        VPSS_GRP_ATTR_S ga = {0};
        ga.u32MaxW   = 1280;
        ga.u32MaxH   = 720;
        ga.enPixFmt  = PIXEL_FORMAT_YUV_SEMIPLANAR_420;
        ga.bDrEn     = HI_FALSE;
        ga.bDbEn     = HI_FALSE;
        ga.bIeEn     = HI_FALSE;
        ga.bNrEn     = HI_TRUE;
        ga.bHistEn   = HI_FALSE;
        ga.enDieMode = VPSS_DIE_MODE_NODIE;

        rc = HI_MPI_VPSS_CreateGrp(grp, &ga);
        if (rc != HI_SUCCESS) {
            fprintf(stderr, "VPSS: CreateGrp(%d) 0x%x\n", grp, rc);
            continue;
        }
        g_state.vpss_grps_up |= (1 << i);

        /* SetChnAttr + EnableChn only: SetChnMode on V2 was seen to corrupt
         * libmpi state, and a later VENC_CreateGroup then segfaulted. */
        VPSS_CHN_ATTR_S ca = {0};
        ca.bSpEn      = HI_FALSE;
        ca.bFrameEn   = HI_FALSE;
        rc = HI_MPI_VPSS_SetChnAttr(grp, VPSS_CHN_OUT, &ca);
        if (rc != HI_SUCCESS) {
            fprintf(stderr, "VPSS: SetChnAttr(%d,%d) 0x%x\n", grp, VPSS_CHN_OUT, rc);
            continue;
        }

        rc = HI_MPI_VPSS_EnableChn(grp, VPSS_CHN_OUT);
        if (rc != HI_SUCCESS) {
            fprintf(stderr, "VPSS: EnableChn(%d,%d) 0x%x\n", grp, VPSS_CHN_OUT, rc);
            continue;
        }

        /* hi3520dv200 V2 VPSS has exactly one physical channel per group
         * (chn 0) and no extend channels — SetChnAttr / SetExtChnAttr on
         * any other chn id returns INVALID_CHNID (0xa0088002) and the
         * subsequent bind to VENC fails (0xa0028003). For the encoder
         * path we instead fan VPSS_CHN_OUT out to both VO and VENC via
         * HI_MPI_SYS_Bind — V2 SYS supports one source → many
         * destinations. bind_vpss_venc() picks up VPSS_CHN_OUT below. */
        g_state.vpss_enc_chns_up |= (1 << i);

        rc = HI_MPI_VPSS_StartGrp(grp);
        if (rc != HI_SUCCESS) {
            fprintf(stderr, "VPSS: StartGrp(%d) 0x%x\n", grp, rc);
            continue;
        }
        g_state.vpss_chns_up |= (1 << i);
        got_any = 1;
    }

    return got_any ? 0 : -1;
}

static int venc_bringup(void)
{
    HI_S32 rc;
    int got_any = 0;

    for (int i = 0; i < HI_VIDEO_PIPE_CHANNELS; ++i) {
        VENC_CHN_ATTR_S a = {0};
        a.stVeAttr.enType = (PAYLOAD_TYPE_E)PT_H264;
        a.stVeAttr.stAttrH264e = (VENC_ATTR_H264_S){
            /* MaxPic must match PicWidth/Height for this kmod or CreateChn
             * returns EN_ERR_ILLEGAL_PARAM.
             * KNOWN BLOCKER on the lab DVR: CreateChn fails 0xa007800c
             * (NOMEM) for every u32BufSize tried (86 KB .. 1 MiB) — libmpi
             * mmaps a buffer the h264e module allocated without the
             * user-mappable flag. Buffer size is not the cause. */
            .u32MaxPicWidth  = ENCODE_W,
            .u32MaxPicHeight = ENCODE_H,
            .u32BufSize      = ENCODE_W * ENCODE_H * 2,
            .u32Profile      = 0,                         /* baseline */
            .bByFrame        = HI_TRUE,
            .bField          = HI_FALSE,
            .bMainStream     = HI_TRUE,
            .u32Priority     = 0,
            .bVIField        = HI_FALSE,
            .u32PicWidth     = ENCODE_W,
            .u32PicHeight    = ENCODE_H,
        };
        a.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
        a.stRcAttr.stAttrH264Cbr = (VENC_ATTR_H264_CBR_S){
            .u32Gop            = ENCODE_GOP,
            .u32StatTime       = 1,
            .u32ViFrmRate      = ENCODE_FPS,
            .fr32TargetFrmRate = ENCODE_FPS,
            .u32BitRate        = ENCODE_BITRATE,
            .u32FluctuateLevel = 0,
        };

        /* V2 hi3520dv200 gates VENC behind an intermediate HI_ID_GROUP
         * dispatcher; the SYS_Bind destination is HI_ID_GROUP[grp], not
         * HI_ID_VENC[chn]. The order the MPP API requires is:
         *   VENC_CreateGroup → CreateChn → RegisterChn(grp,chn) →
         *   SYS_Bind(VPSS, HI_ID_GROUP[grp]) → StartRecvPic
         * VENC_CreateGroup requires VB_CONF.u32MaxPoolCnt to be high
         * (vendor uses 128) — small caps OOB-segfault inside libmpi. */
        #define DVR_VENC_EXIST 0xA0078004
        rc = HI_MPI_VENC_CreateGroup(i);
        if (rc != HI_SUCCESS && rc != DVR_VENC_EXIST) fprintf(stderr, "VENC_CreateGroup(%d) 0x%x\n", i, rc);
        if (rc != HI_SUCCESS && rc != DVR_VENC_EXIST) continue;
        rc = HI_MPI_VENC_CreateChn(i, &a);
        if (rc != HI_SUCCESS && rc != DVR_VENC_EXIST) fprintf(stderr, "VENC_CreateChn(%d) 0x%x\n", i, rc);
        if (rc != HI_SUCCESS && rc != DVR_VENC_EXIST) continue;
        rc = HI_MPI_VENC_RegisterChn(i, i);
        if (rc != HI_SUCCESS && rc != DVR_VENC_EXIST) fprintf(stderr, "VENC_RegisterChn(%d) 0x%x\n", i, rc);
        if (rc != HI_SUCCESS && rc != DVR_VENC_EXIST) continue;
        /* StartRecvPic is deferred to venc_start_recv() after VPSS->VENC bind. */
        g_state.venc_chns_up |= (1 << i);
        got_any = 1;
    }
    return got_any ? 0 : -1;
}

static void venc_start_recv(void)
{
    HI_S32 rc;
    for (int i = 0; i < HI_VIDEO_PIPE_CHANNELS; ++i) {
        if (!(g_state.venc_chns_up & (1 << i))) continue;
        rc = HI_MPI_VENC_StartRecvPic(i);
        if (rc != HI_SUCCESS) {
            fprintf(stderr, "VENC: StartRecvPic(%d) 0x%x\n", i, rc);
            HI_MPI_VENC_DestroyChn(i);
            g_state.venc_chns_up &= ~(1 << i);
        }
    }
}

static void compute_tile_rect(int tile_idx, int screen_w, int header_h, int strip_h,
                              int *x, int *y, int *w, int *h)
{
    int avail_h = 720 - header_h - strip_h;
    int half_w  = screen_w / 2;
    int half_h  = avail_h / 2;
    int gx = tile_idx % 2;
    int gy = tile_idx / 2;
    /* The encoder's size (16-aligned), centred in the slot, so VPSS sends
     * the same picture size to VO and VENC. */
    *w = half_w & ~15;
    *h = half_h & ~15;
    *x = gx * half_w + (half_w - *w) / 2;
    *y = header_h + gy * half_h + (half_h - *h) / 2;
}

static int vo_chn_bringup(int screen_w, int header_h, int strip_h)
{
    HI_S32 rc;
    int got_any = 0;

    for (int i = 0; i < HI_VIDEO_PIPE_CHANNELS; ++i) {
        int x, y, w, h;
        compute_tile_rect(i, screen_w, header_h, strip_h, &x, &y, &w, &h);

        VO_CHN_ATTR_S a = {0};
        a.stRect.s32X     = x;
        a.stRect.s32Y     = y;
        a.stRect.u32Width = (HI_U32)w;
        a.stRect.u32Height = (HI_U32)h;
        a.u32Priority     = 0;
        a.bDeflicker      = HI_FALSE;
        rc = HI_MPI_VO_SetChnAttr(VO_LAYER_VHD0, i, &a);
        if (rc != HI_SUCCESS) {
            fprintf(stderr, "VO_CHN: SetChnAttr(%d) 0x%x\n", i, rc);
            continue;
        }
        rc = HI_MPI_VO_EnableChn(VO_LAYER_VHD0, i);
        if (rc != HI_SUCCESS) {
            fprintf(stderr, "VO_CHN: EnableChn(%d) 0x%x\n", i, rc);
            continue;
        }
        g_state.vo_chns_up |= (1 << i);
        got_any = 1;
    }

    return got_any ? 0 : -1;
}

static void bind_vi_vpss(void)
{
    for (int i = 0; i < HI_VIDEO_PIPE_CHANNELS; ++i) {
        if (!(g_state.vi_chns_up & (1 << i))) continue;
        if (!(g_state.vpss_grps_up & (1 << i))) continue;

        MPP_CHN_S src = { .enModId = HI_ID_VIU, .s32DevId = i / 2, .s32ChnId = g_vi_chn[i] };
        MPP_CHN_S dst = { .enModId = HI_ID_VPSS, .s32DevId = g_vpss_grp[i], .s32ChnId = 0 };
        HI_S32 rc = HI_MPI_SYS_Bind(&src, &dst);
        if (rc != HI_SUCCESS) {
            fprintf(stderr, "Bind VI(%d/%d) -> VPSS(%d): 0x%x\n",
                    i / 2, g_vi_chn[i], g_vpss_grp[i], rc);
            continue;
        }
        g_state.bound_vi_vpss |= (1 << i);
    }
}

static void bind_vpss_vo(void)
{
    for (int i = 0; i < HI_VIDEO_PIPE_CHANNELS; ++i) {
        if (!(g_state.vpss_chns_up & (1 << i))) continue;
        if (!(g_state.vo_chns_up & (1 << i))) continue;

        MPP_CHN_S src = { .enModId = HI_ID_VPSS, .s32DevId = g_vpss_grp[i], .s32ChnId = VPSS_CHN_OUT };
        MPP_CHN_S dst = { .enModId = HI_ID_VOU,  .s32DevId = VO_LAYER_VHD0,  .s32ChnId = i };
        HI_S32 rc = HI_MPI_SYS_Bind(&src, &dst);
        if (rc != HI_SUCCESS) {
            fprintf(stderr, "Bind VPSS(%d) -> VO_CHN(%d): 0x%x\n", g_vpss_grp[i], i, rc);
            continue;
        }
        g_state.bound_vpss_vo |= (1 << i);
    }
}

static void bind_vpss_venc(void)
{
    for (int i = 0; i < HI_VIDEO_PIPE_CHANNELS; ++i) {
        if (!(g_state.vpss_enc_chns_up & (1 << i))) continue;
        if (!(g_state.venc_chns_up & (1 << i))) continue;

        /* V2 hi3520dv200 binds VPSS to the VENC GROUP, not to the VENC
         * chn directly. The GROUP module routes frames to its
         * registered chns (set up in venc_bringup() via
         * HI_MPI_VENC_RegisterChn). */
        MPP_CHN_S src = { .enModId = HI_ID_VPSS,  .s32DevId = g_vpss_grp[i], .s32ChnId = VPSS_CHN_OUT };
        MPP_CHN_S dst = { .enModId = HI_ID_GROUP, .s32DevId = i,             .s32ChnId = 0 };
        HI_S32 rc = HI_MPI_SYS_Bind(&src, &dst);
        if (rc != HI_SUCCESS) {
            fprintf(stderr, "Bind VPSS(%d,enc) -> VENC(%d): 0x%x\n", g_vpss_grp[i], i, rc);
            continue;
        }
        g_state.bound_vpss_venc |= (1 << i);
    }
}

int hi_video_pipe_start(int screen_w, int screen_h,
                        int header_h, int strip_h)
{
    int tile_w = (screen_w / 2) & ~15;            /* 16-px aligned for H.264 */
    int tile_h = ((screen_h - header_h - strip_h) / 2) & ~15;

    fprintf(stderr, "video_pipe: VENC bring-up (H.264 baseline %dx%d CBR %dkbps)\n",
            ENCODE_W, ENCODE_H, ENCODE_BITRATE);
    if (venc_bringup() < 0) {
        fprintf(stderr, "video_pipe: VENC bring-up failed — recording disabled\n");
    }

    fprintf(stderr, "video_pipe: VI bring-up\n");
    if (vi_bringup() < 0) {
        fprintf(stderr, "video_pipe: VI bring-up returned no channels — continuing with VPSS/VO anyway.\n");
    }

    fprintf(stderr, "video_pipe: VPSS bring-up (%d×%d per chn)\n", tile_w, tile_h);
    if (vpss_bringup(tile_w, tile_h) < 0) {
        fprintf(stderr, "video_pipe: VPSS bring-up failed — bailing out.\n");
        return -1;
    }

    fprintf(stderr, "video_pipe: VO_CHN bring-up (4 tiles, %d×%d each)\n", tile_w, tile_h);
    if (vo_chn_bringup(screen_w, header_h, strip_h) < 0) {
        fprintf(stderr, "video_pipe: VO_CHN bring-up failed — bailing out.\n");
        return -1;
    }

    fprintf(stderr, "video_pipe: bind VI -> VPSS\n");
    bind_vi_vpss();
    fprintf(stderr, "video_pipe: bind VPSS -> VO_CHN\n");
    bind_vpss_vo();
    fprintf(stderr, "video_pipe: bind VPSS -> VENC GROUP\n");
    bind_vpss_venc();

    venc_start_recv();
    return 0;
}

void hi_video_pipe_stop(void)
{
    /* Unbind VPSS -> VENC GROUP */
    for (int i = 0; i < HI_VIDEO_PIPE_CHANNELS; ++i) {
        if (!(g_state.bound_vpss_venc & (1 << i))) continue;
        MPP_CHN_S src = { .enModId = HI_ID_VPSS,  .s32DevId = g_vpss_grp[i], .s32ChnId = VPSS_CHN_OUT };
        MPP_CHN_S dst = { .enModId = HI_ID_GROUP, .s32DevId = i,             .s32ChnId = 0 };
        HI_MPI_SYS_UnBind(&src, &dst);
    }
    /* Stop / unregister / destroy VENC chns + their groups */
    for (int i = 0; i < HI_VIDEO_PIPE_CHANNELS; ++i) {
        if (!(g_state.venc_chns_up & (1 << i))) continue;
        HI_MPI_VENC_StopRecvPic(i);
        HI_MPI_VENC_UnRegisterChn(i);
        HI_MPI_VENC_DestroyChn(i);
        HI_MPI_VENC_DestroyGroup(i);
    }
    /* Unbind VPSS -> VO */
    for (int i = 0; i < HI_VIDEO_PIPE_CHANNELS; ++i) {
        if (!(g_state.bound_vpss_vo & (1 << i))) continue;
        MPP_CHN_S src = { .enModId = HI_ID_VPSS, .s32DevId = g_vpss_grp[i], .s32ChnId = VPSS_CHN_OUT };
        MPP_CHN_S dst = { .enModId = HI_ID_VOU,  .s32DevId = VO_LAYER_VHD0,  .s32ChnId = i };
        HI_MPI_SYS_UnBind(&src, &dst);
    }
    /* Unbind VI -> VPSS */
    for (int i = 0; i < HI_VIDEO_PIPE_CHANNELS; ++i) {
        if (!(g_state.bound_vi_vpss & (1 << i))) continue;
        MPP_CHN_S src = { .enModId = HI_ID_VIU,  .s32DevId = i / 2, .s32ChnId = g_vi_chn[i] };
        MPP_CHN_S dst = { .enModId = HI_ID_VPSS, .s32DevId = g_vpss_grp[i], .s32ChnId = 0 };
        HI_MPI_SYS_UnBind(&src, &dst);
    }
    /* Disable VO chns */
    for (int i = 0; i < HI_VIDEO_PIPE_CHANNELS; ++i) {
        if (g_state.vo_chns_up & (1 << i)) HI_MPI_VO_DisableChn(VO_LAYER_VHD0, i);
    }
    /* Stop / destroy VPSS grps. VPSS chn 0 is the only physical chn on
     * hi3520dv200 V2; vpss_enc_chns_up here only flags that the chn was
     * fanned out to VENC too, no separate chn to disable. */
    for (int i = 0; i < HI_VIDEO_PIPE_CHANNELS; ++i) {
        if (g_state.vpss_chns_up & (1 << i)) HI_MPI_VPSS_DisableChn(g_vpss_grp[i], VPSS_CHN_OUT);
        if (g_state.vpss_grps_up & (1 << i)) {
            HI_MPI_VPSS_StopGrp(g_vpss_grp[i]);
            HI_MPI_VPSS_DestroyGrp(g_vpss_grp[i]);
        }
    }
    /* Disable VI */
    for (int i = 0; i < HI_VIDEO_PIPE_CHANNELS; ++i) {
        if (g_state.vi_chns_up & (1 << i)) HI_MPI_VI_DisableChn(g_vi_chn[i]);
    }
    for (int dev = 0; dev < 2; ++dev) {
        if (g_state.vi_devs_up & (1 << dev)) HI_MPI_VI_DisableDev(dev);
    }

    memset(&g_state, 0, sizeof g_state);
}
