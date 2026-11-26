/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_menu.c — the main menu.
 *
 *   top bar      vigil and the role; the clock, LIVE (back to the live
 *                view), the account (not there yet: shown muted)
 *   cards        the functions, 3 x 2 on 1280x720: a large icon, an
 *                upper-case title, a line or two of what it is for
 *   MANAGEMENT   the settings: a button per section of the schema (a new
 *                section shows up here by itself), System, Account
 *
 * Functions vigil does not have yet (playback, AI, alarms, backup, accounts)
 * are there and say so (ui_not_yet), so the menu has its final shape. The
 * cards and the management row are gridnav containers for the keyboard;
 * Tab goes from one to the other and to LIVE.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "icons.h"
#include "ui_app.h"
#include "ui_theme.h"

#define MARGIN   52             /* (1280 - 3 cards - 2 gaps) / 2 */
#define GAP      VG_SPACE_XL
#define BAR_H    VG_SIZE_PAGE_HEADER

enum { FN_PLAYBACK, FN_AI, FN_ALARM, FN_OPERATION, FN_BACKUP, FN_DISPLAY, FN_SYSTEM, FN_ACCOUNT, FN_SECTION = 100 };

typedef struct {
    const lv_image_dsc_t *icon;
    uint32_t              color;
    const char           *title, *text, *later;     /* later: not there yet, its name */
    int                   fn;
} card_t;

static const card_t k_cards[] = {
    { &ui_icon_playback, VG_COLOR_ICON_PLAYBACK, "PLAYBACK", "View, search and play recorded video.", "Playback",
      FN_PLAYBACK },
    { &ui_icon_ai, VG_COLOR_ICON_AI, "AI", "Motion and smart detection: what the cameras see.", "AI detection",
      FN_AI },
    { &ui_icon_alarm, VG_COLOR_ICON_ALARM, "ALARM", "View live alarms and configure alarm actions.", "Alarms",
      FN_ALARM },
    { &ui_icon_operation, VG_COLOR_ICON_OPERATION, "OPERATION",
      "System information; update, configuration import and export.", NULL, FN_OPERATION },
    { &ui_icon_backup, VG_COLOR_ICON_BACKUP, "BACKUP", "Search and back up video files.", "Backup", FN_BACKUP },
    { &ui_icon_display, VG_COLOR_ICON_DISPLAY, "DISPLAY", "Output resolution and display settings.", NULL,
      FN_DISPLAY },
};

static void open_fn(int fn, const char *later)
{
    if (later) {
        ui_not_yet(later);
        return;
    }
    if (fn >= FN_SECTION) {
        int sec = fn - FN_SECTION;
        if (ui_model()->sections[sec].is_array) ui_list_open(sec);
        else ui_form_open(sec, -1);
    } else if (fn == FN_OPERATION || fn == FN_SYSTEM) {
        ui_system_open();
    } else if (fn == FN_DISPLAY) {
        ui_display_open();
    }
}

static void card_cb(lv_event_t *e)
{
    const card_t *c = lv_event_get_user_data(e);
    open_fn(c->fn, c->later);
}

static void live_cb(lv_event_t *e)
{
    (void)e;
    ui_app_back();
}

static lv_obj_t *icon(lv_obj_t *parent, const lv_image_dsc_t *img, uint32_t color)
{
    lv_obj_t *i = lv_image_create(parent);
    lv_image_set_src(i, img);
    /* A8: the colour is the recolour */
    lv_obj_set_style_image_recolor(i, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(i, LV_OPA_COVER, 0);
    ui_inert(i);
    return i;
}

/* A clickable surface with the card look: hover and focus per the tokens. */
static lv_obj_t *surface(lv_obj_t *parent)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_style_bg_color(b, UI_C(CARD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, VG_RADIUS_MD, 0);
    lv_obj_set_style_border_color(b, UI_C(LINE), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_bg_color(b, UI_C(CARD_HOVER), LV_STATE_HOVERED);
    lv_obj_set_style_border_color(b, UI_C(ACCENT), LV_STATE_HOVERED);
    lv_obj_add_style(b, &ui_st_btn_pressed, LV_STATE_PRESSED);
    lv_obj_add_style(b, &ui_st_focus, LV_STATE_FOCUSED);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    return b;
}

static void card(lv_obj_t *grid, const card_t *c)
{
    lv_obj_t *b = surface(grid);
    lv_obj_set_size(b, VG_SIZE_CARD_W, VG_SIZE_CARD_H);
    lv_obj_set_style_pad_all(b, VG_SPACE_XL, 0);
    lv_obj_set_style_pad_column(b, VG_SPACE_XL, 0);
    lv_obj_set_layout(b, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(b, card_cb, LV_EVENT_CLICKED, (void *)c);
    icon(b, c->icon, c->color);
    lv_obj_t *col = lv_obj_create(b);
    ui_plain(col);
    ui_inert(col);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_layout(col, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, VG_SPACE_SM, 0);
    ui_label(col, c->title, VG_FONT_LG_LV, UI_C(TEXT));
    lv_obj_t *t = ui_label(col, c->text, VG_FONT_SM_LV, UI_C(TEXT2));
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
}

/* ---- the management row ------------------------------------------------------------------- */

typedef struct {
    int         fn;
    const char *later;
} mgmt_t;

static mgmt_t g_mgmt[UI_MAX_ITEMS + 4];

static void mgmt_cb(lv_event_t *e)
{
    const mgmt_t *m = lv_event_get_user_data(e);
    open_fn(m->fn, m->later);
}

static void mgmt_btn(lv_obj_t *row, const lv_image_dsc_t *img, const char *label, mgmt_t *m)
{
    lv_obj_t *b = surface(row);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_HOVERED);
    lv_obj_set_size(b, LV_SIZE_CONTENT, VG_SIZE_BUTTON + VG_SPACE_SM);
    lv_obj_set_style_pad_hor(b, VG_SPACE_LG, 0);
    lv_obj_set_style_pad_column(b, VG_SPACE_SM, 0);
    lv_obj_set_layout(b, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(b, mgmt_cb, LV_EVENT_CLICKED, m);
    icon(b, img, VG_COLOR_TEXT);
    char up[48];
    size_t i = 0;
    for (; label[i] && i < sizeof up - 1; ++i) up[i] = (char)toupper((unsigned char)label[i]);
    up[i] = 0;
    ui_label(b, up, VG_FONT_MD_LV, UI_C(TEXT));
}

/* The settings sections vigild has get their names and icons; another
 * one shows up with its own label and the settings icon. */
static void section_look(const char *key, const char *label, const lv_image_dsc_t **img, const char **name)
{
    *img = &ui_icon_system;
    *name = label;
    if (!strcmp(key, "cameras")) *img = &ui_icon_camera, *name = "Camera";
    else if (!strcmp(key, "api")) *img = &ui_icon_network, *name = "Network";
    else if (!strcmp(key, "record")) *img = &ui_icon_storage, *name = "Storage";
}

/* ---- the screen ------------------------------------------------------------------------------- */

static void top_bar(ui_page_t *p)
{
    lv_obj_t *bar = lv_obj_create(p->body);
    ui_plain(bar);
    ui_inert(bar);
    ui_ring_room(bar);
    lv_obj_set_size(bar, LV_PCT(100), BAR_H);
    lv_obj_set_style_border_color(bar, UI_C(LINE), 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_pad_hor(bar, MARGIN, 0);
    lv_obj_set_layout(bar, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, VG_SPACE_MD, 0);

    ui_label(bar, LV_SYMBOL_VIDEO, VG_FONT_LG_LV, UI_C(ACCENT));
    ui_label(bar, "vigil", VG_FONT_XL_LV, UI_C(TEXT));
    ui_label(bar, ui_model()->role ? ui_model()->role : "", VG_FONT_SM_LV, UI_C(TEXT2));
    lv_obj_t *title = ui_label(bar, "Main menu", VG_FONT_MD_LV, UI_C(TEXT2));
    lv_obj_set_style_pad_left(title, VG_SPACE_LG, 0);
    lv_obj_set_flex_grow(title, 1);
    lv_obj_t *clk = ui_label(bar, "", VG_FONT_SM_LV, UI_C(TEXT2));
    lv_label_bind_text(clk, &ui_subj_clock, NULL);
    lv_obj_t *live = ui_btn(bar, LV_SYMBOL_VIDEO, "LIVE");
    lv_obj_add_event_cb(live, live_cb, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(p->group, live);
    /* Accounts do not exist yet: the icon is there, muted, and does
     * nothing; there is no log-out until there is a log-in. */
    icon(bar, &ui_icon_account, VG_COLOR_TEXT2);
}

void ui_menu_open(void)
{
    ui_page_t *p = ui_page_push_plain();
    if (!p) return;
    lv_obj_set_style_bg_color(p->body, UI_C(MENU_TOP), 0);
    lv_obj_set_style_bg_grad_color(p->body, UI_C(MENU_BOTTOM), 0);
    lv_obj_set_style_bg_grad_dir(p->body, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(p->body, LV_OPA_COVER, 0);

    /* The keyboard's order: the cards, the management row, LIVE. */
    lv_obj_t *grid = lv_obj_create(p->body);
    ui_plain(grid);
    ui_ring_room(grid);
    lv_obj_set_pos(grid, MARGIN, BAR_H + GAP);
    lv_obj_set_size(grid, 3 * VG_SIZE_CARD_W + 2 * GAP, 2 * VG_SIZE_CARD_H + GAP);
    lv_obj_set_layout(grid, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, GAP, 0);
    lv_obj_set_style_pad_column(grid, GAP, 0);
    /* Six cards: one page at 1280x720, so no page dots. */
    for (size_t i = 0; i < sizeof k_cards / sizeof k_cards[0]; ++i) card(grid, &k_cards[i]);
    ui_gridnav(grid, LV_GRIDNAV_CTRL_ROLLOVER);
    lv_group_add_obj(p->group, grid);

    int y = BAR_H + GAP + 2 * VG_SIZE_CARD_H + GAP + VG_SPACE_XL;
    lv_obj_t *l = ui_label(p->body, "MANAGEMENT", VG_FONT_LG_LV, UI_C(TEXT2));
    lv_obj_set_pos(l, MARGIN, y);
    lv_obj_t *row = lv_obj_create(p->body);
    ui_plain(row);
    ui_ring_room(row);
    lv_obj_set_pos(row, MARGIN, y + 36);
    lv_obj_set_size(row, 3 * VG_SIZE_CARD_W + 2 * GAP, VG_SIZE_BUTTON + VG_SPACE_SM);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    const ui_model_t *m = ui_model();
    int k = 0;
    /* Camera, Network, Storage, then any other section in schema order. */
    int order[UI_MAX_ITEMS + 4], n = 0;
    static const char *const first[] = { "cameras", "api", "record" };
    for (int f = 0; f < 3; ++f)
        for (int i = 0; i < m->n_sections; ++i)
            if (!strcmp(m->sections[i].key, first[f])) order[n++] = i;
    for (int i = 0; i < m->n_sections && n < (int)(sizeof order / sizeof order[0]); ++i)
        if (strcmp(m->sections[i].key, "cameras") && strcmp(m->sections[i].key, "api") &&
            strcmp(m->sections[i].key, "record"))
            order[n++] = i;
    for (int j = 0; j < n && k < (int)(sizeof g_mgmt / sizeof g_mgmt[0]) - 2; ++j) {
        int i = order[j];
        const lv_image_dsc_t *img;
        const char *name;
        section_look(m->sections[i].key, m->sections[i].label, &img, &name);
        g_mgmt[k] = (mgmt_t){ FN_SECTION + i, NULL };
        mgmt_btn(row, img, name, &g_mgmt[k++]);
    }
    g_mgmt[k] = (mgmt_t){ FN_SYSTEM, NULL };
    mgmt_btn(row, &ui_icon_system, "System", &g_mgmt[k++]);
    g_mgmt[k] = (mgmt_t){ FN_ACCOUNT, "Accounts" };
    mgmt_btn(row, &ui_icon_account, "Account", &g_mgmt[k++]);
    ui_gridnav(row, LV_GRIDNAV_CTRL_ROLLOVER);
    lv_group_add_obj(p->group, row);

    top_bar(p);
    ui_page_ready(p);
}
