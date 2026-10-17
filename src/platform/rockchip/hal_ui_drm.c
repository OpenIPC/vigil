/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hal_ui_drm.c — the UI layer on Rockchip: LVGL renders ARGB8888 into the
 * DRM UI plane (rk_display.c), which VOP2 blends over the video mosaic with
 * per-pixel alpha. Tiles are fully transparent holes.
 *
 * The plane is double-buffered so a frame is never scanned out half drawn:
 * LVGL renders (partial mode) into a full-screen shadow copy in normal RAM;
 * at the end of each refresh the changed area is copied into the buffer not
 * on screen, which is then flipped in. That buffer is two refreshes old, so
 * it gets this refresh's and the previous refresh's changes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

#include "hal_ui.h"
#include "rk_display.h"

#define DRAW_LINES 60

static struct {
    uint8_t  *shadow;               /* w*h*4, what the UI looks like */
    unsigned  w, h;
    int       front;                /* UI buffer on screen */
    lv_area_t dirty, prev_dirty;    /* changed this / last refresh */
    int       have_dirty, have_prev;
} u;

static void area_join(lv_area_t *acc, int *have, const lv_area_t *a)
{
    if (!*have) {
        *acc = *a;
        *have = 1;
        return;
    }
    if (a->x1 < acc->x1) acc->x1 = a->x1;
    if (a->y1 < acc->y1) acc->y1 = a->y1;
    if (a->x2 > acc->x2) acc->x2 = a->x2;
    if (a->y2 > acc->y2) acc->y2 = a->y2;
}

static void copy_out(rk_buf_t *dst, const lv_area_t *a)
{
    size_t row = (size_t)lv_area_get_width(a) * 4;
    for (int32_t y = a->y1; y <= a->y2; ++y)
        memcpy(dst->map + (size_t)y * dst->pitch + (size_t)a->x1 * 4,
               u.shadow + ((size_t)y * u.w + (size_t)a->x1) * 4, row);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    int32_t w = lv_area_get_width(area);
    size_t row = (size_t)w * 4;
    for (int32_t y = area->y1; y <= area->y2; ++y) {
        memcpy(u.shadow + ((size_t)y * u.w + (size_t)area->x1) * 4, px, row);
        px += row;
    }
    area_join(&u.dirty, &u.have_dirty, area);

    if (lv_display_flush_is_last(disp) && u.have_dirty) {
        int back = !u.front;
        rk_buf_t *b = rk_display_ui_buf(back);
        lv_area_t all = u.dirty;
        int have = 1;
        if (u.have_prev) area_join(&all, &have, &u.prev_dirty);
        copy_out(b, &all);
        if (rk_display_ui_show(back) == 0) {
            u.front = back;
            u.prev_dirty = u.dirty;
            u.have_prev = 1;
        } else {
            /* Not shown: the back buffer stays behind by both areas. */
            u.prev_dirty = all;
            u.have_prev = 1;
        }
        u.have_dirty = 0;
    }
    lv_display_flush_ready(disp);
}

lv_display_t *hal_ui_create(unsigned w, unsigned h)
{
    for (int i = 0; i < 2; ++i) {
        rk_buf_t *b = rk_display_ui_buf(i);
        if (!b || !b->map || b->width != w || b->height != h) return NULL;
    }
    u.w = w;
    u.h = h;
    u.front = 0;
    u.shadow = calloc((size_t)w * h, 4);                    /* transparent */
    lv_display_t *disp = u.shadow ? lv_display_create((int32_t)w, (int32_t)h) : NULL;
    if (!disp) {
        free(u.shadow);
        u.shadow = NULL;
        return NULL;
    }
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_ARGB8888);
    size_t sz = (size_t)w * DRAW_LINES * 4;
    void *b1 = malloc(sz), *b2 = malloc(sz);
    if (!b1 || !b2) {
        free(b1);
        free(b2);
        lv_display_delete(disp);
        free(u.shadow);
        u.shadow = NULL;
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
