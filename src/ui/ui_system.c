/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_system.c — System: what the box is and how it is doing, read-only.
 * Refreshed every second while shown (uptime, memory). */
#include "ui_app.h"
#include "ui_theme.h"

#define INFO_MAX 16

typedef struct {
    lv_obj_t *value[INFO_MAX];
    int       n;
} sys_t;

static void sys_fill(sys_t *s)
{
    ui_info_t info[INFO_MAX];
    int n = ui_model()->system_info ? ui_model()->system_info(info, INFO_MAX) : 0;
    for (int i = 0; i < n && i < s->n; ++i) lv_label_set_text(s->value[i], info[i].value);
}

static void sys_tick_cb(lv_timer_t *t)
{
    sys_fill(lv_timer_get_user_data(t));
}

static void sys_close(ui_page_t *p)
{
    lv_timer_t *t = lv_obj_get_user_data(p->body);
    if (t) lv_timer_delete(t);
    lv_free(p->user);
}

void ui_system_open(void)
{
    sys_t *s = lv_malloc_zeroed(sizeof *s);
    if (!s) return;
    ui_page_t *p = ui_page_push("System");
    if (!p) {
        lv_free(s);
        return;
    }
    p->user = s;
    p->on_close = sys_close;

    lv_obj_t *panel = lv_obj_create(p->body);
    ui_plain(panel);
    lv_obj_add_style(panel, &ui_st_panel, 0);
    lv_obj_set_size(panel, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, 0, 0);

    ui_info_t info[INFO_MAX];
    int n = ui_model()->system_info ? ui_model()->system_info(info, INFO_MAX) : 0;
    for (int i = 0; i < n; ++i) {
        lv_obj_t *r = lv_obj_create(panel);
        ui_plain(r);
        lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_ver(r, VG_SPACE_MD, 0);
        lv_obj_set_style_border_color(r, UI_C(LINE), 0);
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(r, i ? 1 : 0, 0);
        lv_obj_set_layout(r, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_t *k = ui_label(r, info[i].label, VG_FONT_MD_LV, UI_C(TEXT2));
        lv_obj_set_width(k, 320);
        s->value[i] = ui_label(r, info[i].value, VG_FONT_MD_LV, UI_C(TEXT));
        lv_obj_set_flex_grow(s->value[i], 1);
        lv_label_set_long_mode(s->value[i], LV_LABEL_LONG_WRAP);
    }
    s->n = n;
    lv_obj_set_user_data(p->body, lv_timer_create(sys_tick_cb, 1000, s));
}
