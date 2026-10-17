/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hal_ui_drm.c — the UI layer on Rockchip: LVGL renders ARGB8888 into the
 * DRM UI plane's buffer (rk_display.c), which VOP2 blends over the video
 * mosaic with per-pixel alpha. Tiles are fully transparent holes. */
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

#include "hal_ui.h"
#include "rk_display.h"

#define DRAW_LINES 60

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    rk_buf_t *ui = rk_display_ui();
    int32_t w = lv_area_get_width(area);
    size_t row = (size_t)w * 4;
    for (int32_t y = area->y1; y <= area->y2; ++y) {
        memcpy(ui->map + (size_t)y * ui->pitch + (size_t)area->x1 * 4, px, row);
        px += row;
    }
    lv_display_flush_ready(disp);
}

lv_display_t *hal_ui_create(unsigned w, unsigned h)
{
    rk_buf_t *ui = rk_display_ui();
    if (!ui || !ui->map || ui->width != w || ui->height != h) return NULL;
    lv_display_t *disp = lv_display_create((int32_t)w, (int32_t)h);
    if (!disp) return NULL;
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_ARGB8888);
    size_t sz = (size_t)w * DRAW_LINES * 4;
    void *b1 = malloc(sz), *b2 = malloc(sz);
    if (!b1 || !b2) {
        free(b1);
        free(b2);
        return NULL;
    }
    lv_display_set_buffers(disp, b1, b2, (uint32_t)sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    return disp;
}

void hal_ui_hole(lv_color_t *color, lv_opa_t *opa)
{
    *color = lv_color_black();
    *opa = LV_OPA_TRANSP;
}
