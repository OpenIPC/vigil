/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hal_ui_fbdev.c — the UI layer on SigmaStar: LVGL's fbdev driver on the
 * GOP framebuffer /dev/fb0 (fbdev.ko), above the display's video layer.
 * Tiles are holes keyed out by the GOP's colour key. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

#include "lvgl.h"
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#include "mi_common_datatype.h"
#include "sstarFb.h"

#include "hal_ui.h"

#define FB_DEV        "/dev/fb0"
/* The GOP compares its colour key with the pixel's RGB. */
#define COLORKEY_R    0xFF
#define COLORKEY_G    0x00
#define COLORKEY_B    0xFF

static int g_fd = -1;

lv_display_t *hal_ui_create(unsigned w, unsigned h)
{
    /* fbdev.ko sets the layer up once, at load, from /config/fbdev.ini
     * (FB_WIDTH/FB_HEIGHT, FB_HWWIN_FORMAT 5 = ARGB8888). Changing it later
     * with FBIOSET_DISPLAYLAYER_ATTRIBUTES left the scan-out reading the
     * old geometry (seen in qemu), so the layer is only checked here. The
     * fd stays open for the life of the process, as on hifb. */
    if (g_fd < 0) {
        g_fd = open(FB_DEV, O_RDWR);
        if (g_fd < 0) {
            fprintf(stderr, "open %s: %s\n", FB_DEV, strerror(errno));
            return NULL;
        }
    }
    struct fb_var_screeninfo var;
    if (ioctl(g_fd, FBIOGET_VSCREENINFO, &var) < 0) {
        fprintf(stderr, "FBIOGET_VSCREENINFO: %s\n", strerror(errno));
        return NULL;
    }
    if (var.xres != w || var.yres != h || var.bits_per_pixel != LV_COLOR_DEPTH) {
        fprintf(stderr, "%s is %ux%u at %u bpp, the UI needs %ux%u at %d: "
                "set FB_WIDTH/FB_HEIGHT and FB_HWWIN_FORMAT in fbdev.ini\n",
                FB_DEV, var.xres, var.yres, var.bits_per_pixel, w, h, LV_COLOR_DEPTH);
        return NULL;
    }

    lv_display_t *disp = lv_linux_fbdev_create();
    if (!disp) return NULL;
    lv_linux_fbdev_set_file(disp, FB_DEV);

    /* Without the key the UI's tiles would cover the video, and a hidden
     * layer shows no UI: either way there is no usable display. */
    MI_FB_ColorKey_t ck = { .bKeyEnable = 1, .u8Red = COLORKEY_R,
                            .u8Green = COLORKEY_G, .u8Blue = COLORKEY_B };
    if (ioctl(g_fd, FBIOSET_COLORKEY, &ck) < 0) {
        fprintf(stderr, "FBIOSET_COLORKEY: %s\n", strerror(errno));
        lv_display_delete(disp);
        return NULL;
    }
    MI_BOOL show = 1;
    if (ioctl(g_fd, FBIOSET_SHOW, &show) < 0) {
        fprintf(stderr, "FBIOSET_SHOW: %s\n", strerror(errno));
        lv_display_delete(disp);
        return NULL;
    }
    return disp;
}

void hal_ui_hole(lv_color_t *color, lv_opa_t *opa)
{
    *color = lv_color_make(COLORKEY_R, COLORKEY_G, COLORKEY_B);
    *opa = LV_OPA_COVER;
}
