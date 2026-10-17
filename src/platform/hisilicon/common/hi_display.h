/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hi_display.h — per-SoC MPP display bring-up shared by hifb_demo and
 * dvr_home: SYS/VB init, VO device + video layer, HDMI, and the hifb
 * pixel format.
 *
 * Everything that differs between the supported SoCs lives behind this
 * interface (hi_display.c), selected at build time by the Makefile's
 * PLATFORM= (-DVG_HI3520DV200 / -DVG_HI3536DV100).
 */
#ifndef HI_DISPLAY_H
#define HI_DISPLAY_H

#include "hi_type.h"
#include "hi_comm_vb.h"
#include "hi_comm_vo.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HI_DISPLAY_VO_DEV    0      /* DHD0 */
#define HI_DISPLAY_VO_LAYER  0      /* VHD0, sits under the hifb layer */

/* hifb bits per pixel this SoC is driven at:
 *   hi3520dv200 — 16 (RGB565, the layer's power-on format)
 *   hi3536dv100 — 32 (ARGB8888; its hifb has no RGB565 mode, only
 *                 ARGB1555 / ARGB8888) */
#if defined(VG_HI3536DV100)
#define HI_DISPLAY_FB_BPP  32
#else
#define HI_DISPLAY_FB_BPP  16
#endif

/* SYS/VB init, then VO device `intf`/`sync`, the w x h video layer, and
 * HDMI when intf is VO_INTF_HDMI. Returns 0 on success, -1 if SYS or the
 * VO device fails (video-layer and HDMI failures are logged only). */
int hi_display_start(VB_CONF_S *vb, VO_INTF_TYPE_E intf, VO_INTF_SYNC_E sync,
                     unsigned w, unsigned h);

/* Reverse of hi_display_start. */
void hi_display_stop(void);

/* Put the framebuffer behind `fd` into this SoC's pixel format
 * (HI_DISPLAY_FB_BPP) at w x h. On hi3520dv200 this only checks the
 * current mode, which is RGB565 at power-on. Returns 0 on success. */
int hi_display_fb_set_mode(int fd, unsigned w, unsigned h);

#ifdef __cplusplus
}
#endif

#endif /* HI_DISPLAY_H */
