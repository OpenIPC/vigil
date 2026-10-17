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

#endif /* HAL_UI_H */
