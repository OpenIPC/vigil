/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hal_ui.h — the graphics layer LVGL draws on, above the video layer. */
#ifndef HAL_UI_H
#define HAL_UI_H

#include "lvgl.h"

/* Create the LVGL display on the platform's UI layer, sized w x h.
 * NULL if the platform has none. */
lv_display_t *hal_ui_create(unsigned w, unsigned h);

/* How the UI paints a tile so the video below shows through: a colorkey
 * colour (HiSilicon hifb) or full transparency (planes with alpha). */
void hal_ui_hole(lv_color_t *color, lv_opa_t *opa);

/*
 * The mouse pointer drawn by the display hardware, on a plane of its own
 * above the UI: it moves without LVGL redrawing anything, so it follows the
 * mouse at the output's refresh rate whatever the UI is busy with.
 *
 * hal_ui_cursor_create: the arrow, w x h ARGB8888 pixels in UI pixels
 * (alpha 0 or 255; the hot spot is the top-left pixel), hidden at the
 * UI's centre. 0, or -1 when the platform has no such plane (LVGL draws
 * the pointer then, and the other calls are not made).
 * hal_ui_cursor_move: to x, y in UI coordinates (the platform scales them
 * to the output). Safe from any thread and cheap: it never waits for the
 * display, the platform applies the newest position at its next frame.
 * hal_ui_cursor_show: shown (1) or hidden (0), the same way.
 * hal_ui_cursor_delete: hidden, and whatever create started stopped.
 */
int  hal_ui_cursor_create(const uint32_t *argb, int w, int h);
void hal_ui_cursor_move(int x, int y);
void hal_ui_cursor_show(int on);
void hal_ui_cursor_delete(void);

#endif /* HAL_UI_H */
