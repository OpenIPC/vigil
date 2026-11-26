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
/* Keyboard focus, for LV_STATE_FOCUSED: a ring around the widget
 * (outline), or inside its edge (border) where the outside belongs to a
 * neighbour (video tiles, list rows). Both draw nothing until
 * ui_theme_focus_visible(1): a mouse user sees no focus. */
extern lv_style_t ui_st_focus;
extern lv_style_t ui_st_focus_in;

/* Once, after the display exists: the tokens' styles and LVGL's default
 * theme (used by the stock widgets: switch, dropdown, keyboard...) in the
 * same colours. */
void ui_theme_init(lv_display_t *disp);

/* Show (a keyboard is in use) or hide the focus styles. */
void ui_theme_focus_visible(int on);
/* Add ui_st_focus to a stock widget (text area, switch, drop-down). */
void ui_focus_style(lv_obj_t *o);

/* Let the focus ring of `o`'s children show outside `o` (a tight row of
 * buttons, a field's box): overflow visible, and room for it in what LVGL
 * redraws when one of them changes. */
void ui_ring_room(lv_obj_t *o);

/* No padding, border, radius, background or scrolling. */
void ui_plain(lv_obj_t *o);

/* A flat button with an optional symbol (LV_SYMBOL_...) and text. */
lv_obj_t *ui_btn(lv_obj_t *parent, const char *symbol, const char *text);
/* Make a ui_btn the page's main action (accent colour, also hovered). */
void ui_btn_accent(lv_obj_t *btn);
/* The label inside a ui_btn. */
lv_obj_t *ui_btn_label(lv_obj_t *btn);

/* An A8 icon (icons.h) in colour `rgb` (0xRRGGBB); not clickable. */
lv_obj_t *ui_icon(lv_obj_t *parent, const lv_image_dsc_t *img, uint32_t rgb);

/* A round button `d` pixels across for over the video: a disc with hard
 * edges (no blend with a key colour), accent while hovered. Add its
 * content as children, centred. */
lv_obj_t *ui_disc_button(lv_obj_t *parent, int d);

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color);

#endif /* UI_THEME_H */
