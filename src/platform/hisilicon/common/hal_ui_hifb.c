/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hal_ui_hifb.c — the UI layer on HiSilicon: LVGL's fbdev driver on hifb
 * /dev/fb0 (RGB565 on hi3520dv200, ARGB8888 on hi3536dv100), above the VO
 * video layer. Tiles are holes keyed out by hifb's colorkey. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "lvgl.h"
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#include "hifb.h"

#include "hal_ui.h"
#include "hi_display.h"

#define FB_DEV        "/dev/fb0"
/* hifb compares its colorkey against the pixel's RGB888 value. */
#define COLORKEY_RGB  0xFF00FFu

static int g_mode_fd = -1;

lv_display_t *hal_ui_create(unsigned w, unsigned h)
{
    /* The fbdev driver takes the colour format from the bpp the kernel
     * reports, so the SoC's format is set first. That fd stays open for
     * the life of the process: hifb resets the layer to its default format
     * (ARGB1555 on hi3536dv100) when the last fd closes, which would
     * otherwise happen before lv_linux_fbdev opens its own. */
    if (g_mode_fd < 0) {
        g_mode_fd = open(FB_DEV, O_RDWR);
        if (g_mode_fd < 0) {
            fprintf(stderr, "open %s: %s\n", FB_DEV, strerror(errno));
            return NULL;
        }
    }
    if (hi_display_fb_set_mode(g_mode_fd, w, h) < 0) return NULL;

    lv_display_t *disp = lv_linux_fbdev_create();
    if (!disp) return NULL;
    lv_linux_fbdev_set_file(disp, FB_DEV);
    lv_display_set_resolution(disp, (int32_t)w, (int32_t)h);

    /* Make the tile colour transparent onto the video layer, and make sure
     * the layer is shown (some hifb builds default to hidden). */
    HIFB_COLORKEY_S ck = { .bKeyEnable = HI_TRUE, .u32Key = COLORKEY_RGB };
    if (ioctl(g_mode_fd, FBIOPUT_COLORKEY_HIFB, &ck) < 0)
        fprintf(stderr, "FBIOPUT_COLORKEY_HIFB: %s\n", strerror(errno));
    HI_BOOL show = HI_TRUE;
    ioctl(g_mode_fd, FBIOPUT_SHOW_HIFB, &show);
    return disp;
}

void hal_ui_hole(lv_color_t *color, lv_opa_t *opa)
{
    *color = lv_color_hex(COLORKEY_RGB);
    *opa = LV_OPA_COVER;
}
