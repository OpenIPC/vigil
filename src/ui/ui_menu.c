/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_menu.c — the main menu: a tile per settings section of the schema
 * (so a new section shows up here by itself) and System. */
#include <string.h>

#include "ui_app.h"
#include "ui_theme.h"

#define TILE_W 360
#define TILE_H 150
#define SYSTEM (-1)

/* A symbol for the sections vigild has; others get a generic one. */
static const char *section_symbol(const char *key)
{
    if (!strcmp(key, "cameras")) return LV_SYMBOL_VIDEO;
    if (!strcmp(key, "record")) return LV_SYMBOL_SAVE;
    if (!strcmp(key, "api")) return LV_SYMBOL_WIFI;
    return LV_SYMBOL_SETTINGS;
}

static void tile_cb(lv_event_t *e)
{
    int sec = (int)(intptr_t)lv_event_get_user_data(e);
    if (sec == SYSTEM) ui_system_open();
    else if (ui_model()->sections[sec].is_array) ui_list_open(sec);
    else ui_form_open(sec, -1);
}

static void tile(lv_obj_t *parent, const char *sym, const char *title, const char *help, int sec)
{
    lv_obj_t *t = lv_button_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_add_style(t, &ui_st_btn, 0);
    lv_obj_add_style(t, &ui_st_btn_hover, LV_STATE_HOVERED);
    lv_obj_add_style(t, &ui_st_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_style_radius(t, VG_RADIUS_MD, 0);
    lv_obj_set_style_pad_all(t, VG_SPACE_LG, 0);
    lv_obj_set_style_pad_row(t, VG_SPACE_SM, 0);
    lv_obj_set_size(t, TILE_W, TILE_H);
    lv_obj_set_layout(t, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_event_cb(t, tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)sec);

    ui_label(t, sym, VG_FONT_XL_LV, UI_C(ACCENT));
    ui_label(t, title, VG_FONT_LG_LV, UI_C(TEXT));
    lv_obj_t *h = ui_label(t, help, VG_FONT_SM_LV, UI_C(TEXT2));
    lv_obj_set_width(h, LV_PCT(100));
    lv_label_set_long_mode(h, LV_LABEL_LONG_DOT);
}

void ui_menu_open(void)
{
    ui_page_t *p = ui_page_push("Main menu");
    if (!p) return;
    lv_obj_t *grid = lv_obj_create(p->body);
    ui_plain(grid);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(grid, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(grid, VG_SPACE_XL, 0);
    lv_obj_set_style_pad_row(grid, VG_SPACE_XL, 0);
    lv_obj_set_style_pad_column(grid, VG_SPACE_XL, 0);

    const ui_model_t *m = ui_model();
    for (int i = 0; i < m->n_sections; ++i) {
        const ui_section_t *s = &m->sections[i];
        const char *help = s->help && *s->help ? s->help
                         : s->is_array ? "Add, change and remove" : "Settings";
        tile(grid, section_symbol(s->key), s->label, help, i);
    }
    tile(grid, LV_SYMBOL_HOME, "System", "Platform, version, uptime, memory", SYSTEM);
}
