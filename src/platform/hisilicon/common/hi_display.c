/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hi_display.c — per-SoC MPP display bring-up (see hi_display.h).
 *
 * The sequence is the vendor one on both SoCs:
 *   SYS/VB init -> VO_SetPubAttr + VO_Enable -> video layer -> HDMI.
 *
 * SoC differences handled here:
 *   - VO_PUB_ATTR_S.bDoubleFrame exists on hi3520dv200 only; hi3536dv100
 *     moved it to VO_VIDEO_LAYER_ATTR_S (next to bClusterMode).
 *   - HI_MPI_HDMI_Init takes HI_HDMI_INIT_PARA_S on hi3520dv200 and no
 *     argument on hi3536dv100 (the force mode moved to
 *     HI_HDMI_ATTR_S.enDefaultMode).
 *   - hifb: hi3520dv200 powers up in RGB565 and is left alone; hi3536dv100
 *     is switched to ARGB8888, as the SDK's own sample_hifb does.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

#include "hi_type.h"
#include "hi_comm_sys.h"
#include "hi_comm_vb.h"
#include "hi_comm_vo.h"
#include "hi_comm_hdmi.h"
#include "mpi_sys.h"
#include "mpi_vb.h"
#include "mpi_vo.h"
#include "mpi_hdmi.h"

#include "hi_display.h"

static int g_hdmi_up;

/* Stride alignment of picture buffers. The vendor samples use 64 on these
 * SoCs; hi3520dv200's VENC group setup was seen to fault with 16. */
#define SYS_ALIGN_WIDTH 64

/* SYS + VB bring-up, as the vendor documents it: tear down whatever a
 * previous process left, configure and start VB, set the stride alignment
 * (without it HI_MPI_SYS_Init fails 0xa0028010 and /dev/fb0 refuses open
 * with EPERM), start SYS. */
static HI_S32 sys_init(VB_CONF_S *vb)
{
    HI_MPI_SYS_Exit();
    HI_MPI_VB_Exit();
    HI_S32 rc = HI_MPI_VB_SetConf(vb);
    if (rc != HI_SUCCESS) return rc;
    if ((rc = HI_MPI_VB_Init()) != HI_SUCCESS) return rc;
    MPP_SYS_CONF_S sc;
    memset(&sc, 0, sizeof sc);
    sc.u32AlignWidth = SYS_ALIGN_WIDTH;
    if ((rc = HI_MPI_SYS_SetConf(&sc)) != HI_SUCCESS) return rc;
    return HI_MPI_SYS_Init();
}

static void sys_exit(void)
{
    HI_MPI_SYS_Exit();
    HI_MPI_VB_Exit();
}

static int hdmi_fmt(VO_INTF_SYNC_E sync, HI_HDMI_VIDEO_FMT_E *fmt)
{
    switch (sync) {
    case VO_OUTPUT_720P60:  *fmt = HI_HDMI_VIDEO_FMT_720P_60;  return 0;
#if defined(VG_HI3536DV100)
    case VO_OUTPUT_1080P60: *fmt = HI_HDMI_VIDEO_FMT_1080P_60; return 0;
#endif
    default: return -1;
    }
}

/* libmpi.so on hi3520dv200 does NOT export HI_MPI_HDMI_*; that API lives
 * in libhdmi.a on both SoCs. Without these calls /proc/umap/hdmi reports
 * "do not Open" and the monitor sees no signal — VO drives the VOU plane
 * but the HDMI PHY clocks stay dark. Sequence mirrors the vendor
 * sample_comm_vo.c::SAMPLE_COMM_VO_HdmiStart. */
static void hdmi_start(HI_HDMI_VIDEO_FMT_E fmt)
{
#if defined(VG_HI3536DV100)
    HI_MPI_HDMI_Init();
#else
    HI_HDMI_INIT_PARA_S hp = { .enForceMode = HI_HDMI_FORCE_HDMI };
    HI_MPI_HDMI_Init(&hp);
#endif
    HI_MPI_HDMI_Open(HI_HDMI_ID_0);
    HI_HDMI_ATTR_S ha;
    HI_MPI_HDMI_GetAttr(HI_HDMI_ID_0, &ha);
    ha.bEnableHdmi          = HI_TRUE;
    ha.bEnableVideo         = HI_TRUE;
    ha.enVideoFmt           = fmt;
    ha.enVidOutMode         = HI_HDMI_VIDEO_MODE_YCBCR444;
    ha.enDeepColorMode      = HI_HDMI_DEEP_COLOR_OFF;
    ha.bxvYCCMode           = HI_FALSE;
    ha.bEnableAudio         = HI_FALSE;
    ha.bIsMultiChannel      = HI_FALSE;
    ha.enBitDepth           = HI_HDMI_BIT_DEPTH_16;
    ha.bEnableAviInfoFrame  = HI_TRUE;
    ha.bEnableAudInfoFrame  = HI_TRUE;
    ha.bEnableSpdInfoFrame  = HI_FALSE;
    ha.bEnableMpegInfoFrame = HI_FALSE;
    ha.bDebugFlag           = HI_FALSE;
    ha.bHDCPEnable          = HI_FALSE;
    ha.b3DEnable            = HI_FALSE;
#if defined(VG_HI3536DV100)
    ha.enSoundIntf          = HI_HDMI_SND_INTERFACE_I2S;
    ha.enDefaultMode        = HI_HDMI_FORCE_HDMI;
#endif
    HI_MPI_HDMI_SetAttr(HI_HDMI_ID_0, &ha);
    HI_MPI_HDMI_Start(HI_HDMI_ID_0);
    g_hdmi_up = 1;
}

int hi_display_start(VB_CONF_S *vb, VO_INTF_TYPE_E intf, VO_INTF_SYNC_E sync,
                     unsigned w, unsigned h)
{
    HI_S32 rc;

    /* The critical step inside is HI_MPI_SYS_SetConf(u32AlignWidth) before
     * HI_MPI_SYS_Init — skipping it returns 0xa0028010 (SYS_NOTREADY) and
     * /dev/fb0 then refuses open() with EPERM. */
    rc = sys_init(vb);
    if (rc != HI_SUCCESS) {
        fprintf(stderr, "MPP SYS/VB init: 0x%x\n", rc);
        return -1;
    }

    VO_PUB_ATTR_S pub;
    memset(&pub, 0, sizeof pub);
    pub.u32BgColor = 0x000000;
    pub.enIntfType = intf;
    pub.enIntfSync = sync;
#if !defined(VG_HI3536DV100)
    pub.bDoubleFrame = HI_FALSE;
#endif
    if ((rc = HI_MPI_VO_SetPubAttr(HI_DISPLAY_VO_DEV, &pub)) != HI_SUCCESS ||
        (rc = HI_MPI_VO_Enable(HI_DISPLAY_VO_DEV)) != HI_SUCCESS) {
        fprintf(stderr, "VO device: 0x%x\n", rc);
        return -1;
    }

    /* The VOU video layer MUST be configured + enabled before hifb's
     * graphics layer is composited onto the output. Without
     * SetVideoLayerAttr first, EnableVideoLayer fails with 0xa00f8047
     * (attrs not set) and the fb0 pixels never reach the output. */
    VO_VIDEO_LAYER_ATTR_S la;
    memset(&la, 0, sizeof la);
    la.stDispRect.u32Width   = w;
    la.stDispRect.u32Height  = h;
    la.stImageSize.u32Width  = w;
    la.stImageSize.u32Height = h;
    la.u32DispFrmRt          = 30;
    la.enPixFormat           = PIXEL_FORMAT_YUV_SEMIPLANAR_420;
    rc = HI_MPI_VO_SetVideoLayerAttr(HI_DISPLAY_VO_LAYER, &la);
    if (rc != HI_SUCCESS) {
        fprintf(stderr, "HI_MPI_VO_SetVideoLayerAttr: 0x%x (non-fatal)\n", rc);
    }
    rc = HI_MPI_VO_EnableVideoLayer(HI_DISPLAY_VO_LAYER);
    if (rc != HI_SUCCESS) {
        fprintf(stderr, "HI_MPI_VO_EnableVideoLayer: 0x%x (non-fatal)\n", rc);
    }

    if (intf == VO_INTF_HDMI) {
        HI_HDMI_VIDEO_FMT_E fmt;
        if (hdmi_fmt(sync, &fmt) == 0) {
            hdmi_start(fmt);
        } else {
            fprintf(stderr, "HDMI: no format for VO sync %d, HDMI left off\n", (int)sync);
        }
    }
    return 0;
}

void hi_display_stop(void)
{
    if (g_hdmi_up) {
        HI_MPI_HDMI_Stop(HI_HDMI_ID_0);
        HI_MPI_HDMI_Close(HI_HDMI_ID_0);
        HI_MPI_HDMI_DeInit();
        g_hdmi_up = 0;
    }
    HI_MPI_VO_DisableVideoLayer(HI_DISPLAY_VO_LAYER);
    HI_MPI_VO_Disable(HI_DISPLAY_VO_DEV);
    sys_exit();
}

int hi_display_fb_set_mode(int fd, unsigned w, unsigned h)
{
    struct fb_var_screeninfo v;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) < 0) {
        perror("FBIOGET_VSCREENINFO");
        return -1;
    }
#if HI_DISPLAY_FB_BPP == 32
    static const struct fb_bitfield a32 = { 24, 8, 0 }, r32 = { 16, 8, 0 },
                                    g32 = {  8, 8, 0 }, b32 = {  0, 8, 0 };
    v.transp = a32;
    v.red    = r32;
    v.green  = g32;
    v.blue   = b32;
    v.bits_per_pixel = 32;
    v.xres = v.xres_virtual = w;
    v.yres = v.yres_virtual = h;
    v.xoffset = v.yoffset = 0;
    v.activate = FB_ACTIVATE_NOW;
    if (ioctl(fd, FBIOPUT_VSCREENINFO, &v) < 0) {
        perror("FBIOPUT_VSCREENINFO (ARGB8888)");
        return -1;
    }
    return 0;
#else
    if (v.bits_per_pixel != HI_DISPLAY_FB_BPP || v.xres != w || v.yres != h) {
        fprintf(stderr, "fb: %ux%u %ubpp, expected %ux%u %dbpp\n",
                v.xres, v.yres, v.bits_per_pixel, w, h, HI_DISPLAY_FB_BPP);
    }
    return 0;
#endif
}
