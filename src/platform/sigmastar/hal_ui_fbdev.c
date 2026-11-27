/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hal_ui_fbdev.c — the UI layer on SigmaStar: LVGL's fbdev driver on the
 * GOP framebuffer /dev/fb0 (fbdev.ko), above the display's video layer.
 * Tiles are holes keyed out by the GOP's colour key. The GOP stretches
 * the buffer to the output mode (sstar_fb_screen). */
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
#include "sstar_fb.h"

#define FB_DEV        "/dev/fb0"
/* The GOP compares its colour key with the pixel's RGB. */
#define COLORKEY_R    0xFF
#define COLORKEY_G    0x00
#define COLORKEY_B    0xFF

static int g_fd = -1;

/* The fd stays open for the life of the process, as on hifb. */
static int fb_open(void)
{
    if (g_fd < 0) {
        g_fd = open(FB_DEV, O_RDWR);
        if (g_fd < 0) fprintf(stderr, "open %s: %s\n", FB_DEV, strerror(errno));
    }
    return g_fd;
}

/*
 * The buffer stays at the size fbdev.ko gave it at load (fbdev.ini's
 * FB_WIDTH/FB_HEIGHT): changing that with FBIOSET_DISPLAYLAYER_ATTRIBUTES
 * left the scan-out reading the old geometry (seen in qemu). The screen
 * (the timing's size, fbdev.ini's FB_TIMMING_*) and the stretch window's
 * destination are what follows the output: the GOP scales the buffer up
 * or down to it, in hardware, and fbdev.ko refuses a destination that
 * does not fit the screen, so both go in one call.
 */
int sstar_fb_screen(unsigned w, unsigned h)
{
    if (fb_open() < 0) return -1;
    MI_FB_DisplayLayerAttr_t a;
    memset(&a, 0, sizeof a);
    if (ioctl(g_fd, FBIOGET_DISPLAYLAYER_ATTRIBUTES, &a) < 0) {
        fprintf(stderr, "FBIOGET_DISPLAYLAYER_ATTRIBUTES: %s\n", strerror(errno));
        return -1;
    }
    if (a.u32ScreenWidth == w && a.u32ScreenHeight == h && a.u32dstWidth == w && a.u32dstHeight == h &&
        !a.u32Xpos && !a.u32YPos)
        return 0;
    a.u32Xpos = 0;
    a.u32YPos = 0;
    a.u32dstWidth = w;
    a.u32dstHeight = h;
    a.u32ScreenWidth = w;
    a.u32ScreenHeight = h;
    a.u32SetAttrMask = E_MI_FB_DISPLAYLAYER_ATTR_MASK_SCREEN_SIZE | E_MI_FB_DISPLAYLAYER_ATTR_MASK_DISP_POS |
                       E_MI_FB_DISPLAYLAYER_ATTR_MASK_DISP_SIZE;
    if (ioctl(g_fd, FBIOSET_DISPLAYLAYER_ATTRIBUTES, &a) < 0) {
        fprintf(stderr, "FBIOSET_DISPLAYLAYER_ATTRIBUTES %ux%u: %s\n", w, h, strerror(errno));
        return -1;
    }
    return 0;
}

lv_display_t *hal_ui_create(unsigned w, unsigned h)
{
    /* fbdev.ko sets the layer up once, at load, from fbdev.ini (FB_WIDTH,
     * FB_HEIGHT, FB_HWWIN_FORMAT 5 = ARGB8888): checked here. */
    if (fb_open() < 0) return NULL;
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
