/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_cursor.h — the mouse pointer's arrow, drawn by LVGL
 * (lv_indev_set_cursor) or by a cursor plane (hal_ui_cursor_create). */
#ifndef UI_CURSOR_H
#define UI_CURSOR_H

#include "lvgl.h"

#define UI_CURSOR_WIDTH 12
#define UI_CURSOR_HEIGHT 19

/* An arrow image on the system layer; its top-left is the hot spot. */
lv_obj_t *ui_cursor_create(void);

/* The same arrow as UI_CURSOR_WIDTH x UI_CURSOR_HEIGHT ARGB8888 pixels,
 * alpha 0 or 255. */
void ui_cursor_argb(uint32_t *px);

#endif /* UI_CURSOR_H */
