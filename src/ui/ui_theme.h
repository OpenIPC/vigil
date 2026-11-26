/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_theme.h — LVGL styles built from the design tokens (theme_tokens.h,
 * generated from theme.json), and small builders that use them.
 *
 * Everything drawn over a video tile is opaque and has no shadow: on a
 * colour-keyed layer (HiSilicon, SigmaStar) a half-transparent pixel is a
 * blend with the key colour, a pink fringe over the video. */
#ifndef UI_THEME_H
#define UI_THEME_H

#include "lvgl.h"
#include "theme_tokens.h"

#define UI_C(tok) lv_color_hex(VG_COLOR_##tok)

extern lv_style_t ui_st_screen;     /* page background, text, font */
extern lv_style_t ui_st_panel;      /* raised panel: panel colour, line border */
extern lv_style_t ui_st_btn;        /* flat button */
extern lv_style_t ui_st_btn_hover;
extern lv_style_t ui_st_btn_pressed;
extern lv_style_t ui_st_btn_checked;
extern lv_style_t ui_st_btn_disabled;
extern lv_style_t ui_st_chip;       /* small opaque label background on video */

/* Once, after the display exists: the tokens' styles and LVGL's default
 * theme (used by the stock widgets: switch, dropdown, keyboard...) in the
 * same colours. */
void ui_theme_init(lv_display_t *disp);

/* No padding, border, radius, background or scrolling. */
void ui_plain(lv_obj_t *o);

/* A flat button with an optional symbol (LV_SYMBOL_...) and text. */
lv_obj_t *ui_btn(lv_obj_t *parent, const char *symbol, const char *text);
/* Make a ui_btn the page's main action (accent colour, also hovered). */
void ui_btn_accent(lv_obj_t *btn);
/* The label inside a ui_btn. */
lv_obj_t *ui_btn_label(lv_obj_t *btn);

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color);

#endif /* UI_THEME_H */
