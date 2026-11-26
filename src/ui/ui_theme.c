/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_theme.c — styles from the design tokens (see ui_theme.h). */
#include <stdio.h>

#include "ui_theme.h"

lv_style_t ui_st_screen, ui_st_panel, ui_st_btn, ui_st_btn_hover, ui_st_btn_pressed,
    ui_st_btn_checked, ui_st_btn_disabled, ui_st_chip, ui_st_focus, ui_st_focus_in;
static lv_style_t st_accent, st_accent_hover;

static int g_init, g_focus_visible;

void ui_theme_focus_visible(int on)
{
    on = !!on;
    if (g_init && on == g_focus_visible) return;
    g_focus_visible = on;
    lv_style_reset(&ui_st_focus);
    lv_style_reset(&ui_st_focus_in);
    if (on) {
        /* Opaque: over video a blended pixel is a fringe in the key colour. */
        lv_style_set_outline_color(&ui_st_focus, UI_C(FOCUS));
        lv_style_set_outline_opa(&ui_st_focus, LV_OPA_COVER);
        lv_style_set_outline_width(&ui_st_focus, VG_SIZE_FOCUS_RING);
        lv_style_set_outline_pad(&ui_st_focus, VG_SIZE_FOCUS_GAP);
        lv_style_set_border_color(&ui_st_focus_in, UI_C(FOCUS));
        lv_style_set_border_opa(&ui_st_focus_in, LV_OPA_COVER);
        lv_style_set_border_width(&ui_st_focus_in, VG_SIZE_FOCUS_RING);
        lv_style_set_border_side(&ui_st_focus_in, LV_BORDER_SIDE_FULL);
    } else {
        /* LVGL's own theme marks a focused widget too: nothing, as here. */
        lv_style_set_outline_width(&ui_st_focus, 0);
    }
    if (g_init) {
        lv_obj_report_style_change(&ui_st_focus);
        lv_obj_report_style_change(&ui_st_focus_in);
    }
}

void ui_focus_style(lv_obj_t *o)
{
    lv_obj_add_style(o, &ui_st_focus, LV_STATE_FOCUSED);
    lv_obj_add_style(o, &ui_st_focus, LV_STATE_FOCUS_KEY);
}

void ui_theme_init(lv_display_t *disp)
{
    lv_theme_t *th = lv_theme_default_init(disp, UI_C(ACCENT), UI_C(OK), 1, VG_FONT_MD_LV);
    lv_display_set_theme(disp, th);
    if (g_init) return;
    lv_style_init(&ui_st_focus);
    lv_style_init(&ui_st_focus_in);
    ui_theme_focus_visible(0);
    g_init = 1;

    lv_style_init(&ui_st_screen);
    lv_style_set_bg_color(&ui_st_screen, UI_C(BG));
    lv_style_set_bg_opa(&ui_st_screen, LV_OPA_COVER);
    lv_style_set_text_color(&ui_st_screen, UI_C(TEXT));
    lv_style_set_text_font(&ui_st_screen, VG_FONT_MD_LV);

    lv_style_init(&ui_st_panel);
    lv_style_set_bg_color(&ui_st_panel, UI_C(PANEL));
    lv_style_set_bg_opa(&ui_st_panel, LV_OPA_COVER);
    lv_style_set_border_color(&ui_st_panel, UI_C(LINE));
    lv_style_set_border_width(&ui_st_panel, 1);
    lv_style_set_radius(&ui_st_panel, VG_RADIUS_MD);
    lv_style_set_pad_all(&ui_st_panel, VG_SPACE_LG);
    lv_style_set_text_color(&ui_st_panel, UI_C(TEXT));

    lv_style_init(&ui_st_btn);
    lv_style_set_bg_color(&ui_st_btn, UI_C(PANEL2));
    lv_style_set_bg_opa(&ui_st_btn, LV_OPA_COVER);
    lv_style_set_radius(&ui_st_btn, VG_RADIUS_SM);
    lv_style_set_border_color(&ui_st_btn, UI_C(LINE));
    lv_style_set_border_width(&ui_st_btn, 1);
    lv_style_set_pad_hor(&ui_st_btn, VG_SPACE_MD);
    lv_style_set_pad_ver(&ui_st_btn, VG_SPACE_SM);
    lv_style_set_pad_column(&ui_st_btn, VG_SPACE_SM);
    lv_style_set_min_height(&ui_st_btn, VG_SIZE_BUTTON);
    lv_style_set_text_color(&ui_st_btn, UI_C(TEXT));
    lv_style_set_text_font(&ui_st_btn, VG_FONT_MD_LV);

    lv_style_init(&ui_st_btn_hover);
    lv_style_set_bg_color(&ui_st_btn_hover, UI_C(HOVER));
    lv_style_set_border_color(&ui_st_btn_hover, UI_C(ACCENT));

    lv_style_init(&ui_st_btn_pressed);
    lv_style_set_bg_color(&ui_st_btn_pressed, lv_color_darken(UI_C(ACCENT), LV_OPA_30));
    lv_style_set_border_color(&ui_st_btn_pressed, UI_C(ACCENT));

    lv_style_init(&ui_st_btn_checked);
    lv_style_set_bg_color(&ui_st_btn_checked, UI_C(ACCENT));
    lv_style_set_border_color(&ui_st_btn_checked, UI_C(ACCENT));
    lv_style_set_text_color(&ui_st_btn_checked, lv_color_white());

    lv_style_init(&st_accent);
    lv_style_set_bg_color(&st_accent, UI_C(ACCENT));
    lv_style_set_border_color(&st_accent, UI_C(ACCENT));
    lv_style_set_text_color(&st_accent, lv_color_white());
    lv_style_init(&st_accent_hover);
    lv_style_set_bg_color(&st_accent_hover, lv_color_lighten(UI_C(ACCENT), LV_OPA_20));
    lv_style_set_border_color(&st_accent_hover, lv_color_white());

    lv_style_init(&ui_st_btn_disabled);
    lv_style_set_bg_color(&ui_st_btn_disabled, UI_C(PANEL));
    lv_style_set_border_color(&ui_st_btn_disabled, UI_C(LINE));
    lv_style_set_text_color(&ui_st_btn_disabled, UI_C(TEXT2));

    lv_style_init(&ui_st_chip);
    lv_style_set_bg_color(&ui_st_chip, UI_C(CHIP));
    lv_style_set_bg_opa(&ui_st_chip, LV_OPA_COVER);
    lv_style_set_radius(&ui_st_chip, VG_RADIUS_SM);
    lv_style_set_pad_hor(&ui_st_chip, VG_SPACE_SM);
    lv_style_set_pad_ver(&ui_st_chip, VG_SPACE_XS);
    lv_style_set_pad_column(&ui_st_chip, VG_SPACE_SM);
    lv_style_set_text_color(&ui_st_chip, UI_C(TEXT));
    lv_style_set_text_font(&ui_st_chip, VG_FONT_SM_LV);
}

static void ring_room_cb(lv_event_t *e)
{
    lv_event_set_ext_draw_size(e, VG_SIZE_FOCUS_RING + VG_SIZE_FOCUS_GAP);
}

void ui_ring_room(lv_obj_t *o)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_event_cb(o, ring_room_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    lv_obj_refresh_ext_draw_size(o);
}

void ui_plain(lv_obj_t *o)
{
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *ui_btn(lv_obj_t *parent, const char *symbol, const char *text)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, &ui_st_btn, 0);
    lv_obj_add_style(b, &ui_st_btn_hover, LV_STATE_HOVERED);
    lv_obj_add_style(b, &ui_st_btn_checked, LV_STATE_CHECKED);
    lv_obj_add_style(b, &ui_st_btn_pressed, LV_STATE_PRESSED);
    lv_obj_add_style(b, &ui_st_btn_disabled, LV_STATE_DISABLED);
    lv_obj_add_style(b, &ui_st_focus, LV_STATE_FOCUSED);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(b, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    char buf[96];
    if (symbol && text && *text)
        snprintf(buf, sizeof buf, "%s  %s", symbol, text);
    else
        snprintf(buf, sizeof buf, "%s", symbol ? symbol : text ? text : "");
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, buf);
    return b;
}

void ui_btn_accent(lv_obj_t *btn)
{
    lv_obj_add_style(btn, &st_accent, 0);
    lv_obj_add_style(btn, &st_accent_hover, LV_STATE_HOVERED);
    lv_obj_add_style(btn, &ui_st_btn_pressed, LV_STATE_PRESSED);
}

lv_obj_t *ui_btn_label(lv_obj_t *btn) { return lv_obj_get_child(btn, 0); }

lv_obj_t *ui_icon(lv_obj_t *parent, const lv_image_dsc_t *img, uint32_t rgb)
{
    lv_obj_t *i = lv_image_create(parent);
    lv_image_set_src(i, img);
    /* A8: the colour is the recolour */
    lv_obj_set_style_image_recolor(i, lv_color_hex(rgb), 0);
    lv_obj_set_style_image_recolor_opa(i, LV_OPA_COVER, 0);
    lv_obj_remove_flag(i, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(i, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    return i;
}

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    if (font) lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}
