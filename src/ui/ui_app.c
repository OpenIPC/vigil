/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_app.c — the screen stack, the model's snapshots as subjects, popups,
 * toasts and questions (see ui_app.h). */
#define _POSIX_C_SOURCE 200809L
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ui_app.h"
#include "ui_i18n.h"
#include "ui_theme.h"

#define REFRESH_MS  250
#define STACK_MAX   8

static const ui_model_t *g_model;
static lv_obj_t         *g_live;
static ui_page_t        *g_stack[STACK_MAX];
static int               g_depth;
static lv_indev_t       *g_pointer, *g_keypad;
static int               g_kbd_used;    /* a key since the last keyboard came */

static ui_chan_t    g_status[UI_MAX_CHANNELS];
static lv_subject_t g_subj_ch[UI_MAX_CHANNELS];
lv_subject_t        ui_subj_config;
lv_subject_t        ui_subj_clock;
lv_subject_t        ui_subj_nav_pinned;
static char         g_clock[48], g_clock_prev[48];

static lv_obj_t   *g_popup;         /* the full-screen catcher; the panel is its child */
static lv_group_t *g_popup_group;
static lv_obj_t   *g_popup_panel, *g_sub, *g_sub_item;
static int         g_help;          /* the popup is the help */
static lv_obj_t   *g_toast;
static lv_timer_t *g_toast_timer;

const ui_model_t *ui_model(void) { return g_model; }
lv_subject_t *ui_subject_channel(int ch) { return &g_subj_ch[ch]; }
const ui_chan_t *ui_channel(int ch) { return &g_status[ch]; }
void ui_app_set_pointer(lv_indev_t *indev) { g_pointer = indev; }
int ui_keyboard_in_use(void) { return g_kbd_used; }

/* Over the live view, a panel's rounded corners would blend with the
 * tiles' key colour (a fringe on the video): square there. */
static void over_video(lv_obj_t *o)
{
    if (!ui_page_top()) lv_obj_set_style_radius(o, 0, 0);
}

/* ---- focus ------------------------------------------------------------------------ */

static lv_group_t *focus_group(void)
{
    if (g_popup) return g_popup_group;
    ui_page_t *p = ui_page_top();
    return p ? p->group : ui_live_group();
}

/* The keypad works on the topmost thing: a popup, the page, the live view. */
static void focus_sync(void)
{
    if (g_keypad) lv_indev_set_group(g_keypad, focus_group());
}

void ui_app_set_keypad(lv_indev_t *indev)
{
    g_keypad = indev;
    focus_sync();
}

/* A press with the pointer on an item: it becomes the keyboard's item,
 * so Enter after a click means what was clicked and only one thing has
 * the ring. */
static void item_pressed_cb(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (!in || lv_indev_get_type(in) != LV_INDEV_TYPE_POINTER) return;
    lv_obj_t *item = lv_event_get_current_target(e);
    lv_obj_t *cont = lv_obj_get_parent(item);
    if (cont && lv_obj_has_flag(item, LV_OBJ_FLAG_CLICKABLE)) ui_gridnav_focus(cont, item);
}

static void item_hook(lv_obj_t *item)
{
    /* LVGL marks a clicked object outside a group focused by itself; the
     * gridnav's item must move there, or two would have the ring. */
    lv_obj_add_event_cb(item, item_pressed_cb, LV_EVENT_PRESSED, NULL);
}

static void child_created_cb(lv_event_t *e)
{
    lv_obj_t *child = lv_event_get_target(e);
    if (lv_obj_get_parent(child) == lv_event_get_current_target(e)) item_hook(child);
}

void ui_gridnav(lv_obj_t *o, lv_gridnav_ctrl_t ctrl)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) item_hook(lv_obj_get_child(o, (int32_t)i));
    lv_obj_add_event_cb(o, child_created_cb, LV_EVENT_CHILD_CREATED, NULL);
    /* Last: on LV_EVENT_DELETE gridnav removes its own callback while
     * LVGL 9.2 walks the list, which skips the next callback and runs the
     * last one twice; nothing after it may handle DELETE (an observer's
     * unsubscribe would be run twice). */
    lv_gridnav_add(o, ctrl);
    lv_obj_add_flag(o, UI_FLAG_ARROWS);
}

void ui_gridnav_focus(lv_obj_t *cont, lv_obj_t *item)
{
    if (!lv_obj_has_flag(cont, UI_FLAG_ARROWS)) return;    /* not (yet) a gridnav container */
    lv_gridnav_set_focused(cont, item, LV_ANIM_OFF);
    if (!lv_obj_has_state(cont, LV_STATE_FOCUSED)) lv_obj_remove_state(item, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
}

void ui_inert(lv_obj_t *o)
{
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICK_FOCUSABLE);
}

int ui_pointer(lv_point_t *p)
{
    if (!g_pointer) return 0;
    lv_indev_get_point(g_pointer, p);
    return 1;
}

void ui_chan_title(int ch, char *buf, size_t cap)
{
    if (ch >= 0 && ch < UI_MAX_CHANNELS && g_status[ch].name[0])
        snprintf(buf, cap, "%s", g_status[ch].name);
    else
        snprintf(buf, cap, "CAM %d", ch + 1);
}

/* ---- snapshots ------------------------------------------------------------------ */

/* ui.date_format, ui.time_format and ui.language, read again when the
 * configuration's generation moves. */
static char     g_datefmt[16] = "YYYY-MM-DD", g_timefmt[8] = "24h";
static unsigned g_fmt_gen;

static void formats_sync(void)
{
    unsigned gen = g_model->config_generation ? g_model->config_generation() : 0;
    if (gen == g_fmt_gen && gen) return;
    g_fmt_gen = gen;
    for (int i = 0; i < g_model->n_sections; ++i) {
        const ui_section_t *sec = &g_model->sections[i];
        if (strcmp(sec->key, "ui") != 0) continue;
        for (int f = 0; f < sec->n_fields; ++f) {
            char v[32];
            g_model->get_value(i, -1, f, v, sizeof v);
            if (!strcmp(sec->fields[f].key, "date_format")) snprintf(g_datefmt, sizeof g_datefmt, "%.15s", v);
            else if (!strcmp(sec->fields[f].key, "time_format")) snprintf(g_timefmt, sizeof g_timefmt, "%.7s", v);
            else if (!strcmp(sec->fields[f].key, "language")) ui_i18n_set(v);
        }
    }
}

void ui_format_datetime(const struct tm *tm, int seconds, char *buf, size_t cap)
{
    char d[48], t[48];
    int y = (tm->tm_year + 1900) % 10000, m = (tm->tm_mon + 1) % 100, dd = tm->tm_mday % 100;
    if (!strcmp(g_datefmt, "DD.MM.YYYY")) snprintf(d, sizeof d, "%02d.%02d.%04d", dd, m, y);
    else if (!strcmp(g_datefmt, "MM/DD/YYYY")) snprintf(d, sizeof d, "%02d/%02d/%04d", m, dd, y);
    else snprintf(d, sizeof d, "%04d-%02d-%02d", y, m, dd);
    if (!strcmp(g_timefmt, "12h")) {
        int h = tm->tm_hour % 12 ? tm->tm_hour % 12 : 12;
        if (seconds) snprintf(t, sizeof t, "%d:%02d:%02d %s", h, tm->tm_min, tm->tm_sec, tm->tm_hour < 12 ? "AM" : "PM");
        else snprintf(t, sizeof t, "%d:%02d %s", h, tm->tm_min, tm->tm_hour < 12 ? "AM" : "PM");
    } else if (seconds) {
        snprintf(t, sizeof t, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
    } else {
        snprintf(t, sizeof t, "%02d:%02d", tm->tm_hour, tm->tm_min);
    }
    snprintf(buf, cap, "%s  %s", d, t);
}

static void clock_text(char *buf, size_t cap)
{
    time_t now = g_model->now ? g_model->now() : time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    formats_sync();
    ui_format_datetime(&tm, 1, buf, cap);
}

void ui_app_refresh(void)
{
    int n = g_model->channels < UI_MAX_CHANNELS ? g_model->channels : UI_MAX_CHANNELS;
    for (int i = 0; i < n; ++i) {
        ui_chan_t st;
        memset(&st, 0, sizeof st);
        g_model->chan_status(i, &st);
        if (memcmp(&st, &g_status[i], sizeof st)) {
            g_status[i] = st;
            lv_subject_notify(&g_subj_ch[i]);
        }
    }
    ui_event_t ev;
    while (g_model->next_event && g_model->next_event(&ev)) ui_live_event(&ev);
    if (g_model->config_generation) {
        int gen = (int)g_model->config_generation();
        if (gen != lv_subject_get_int(&ui_subj_config)) lv_subject_set_int(&ui_subj_config, gen);
    }
    char clk[48];
    clock_text(clk, sizeof clk);
    if (strcmp(clk, lv_subject_get_string(&ui_subj_clock))) lv_subject_copy_string(&ui_subj_clock, clk);
}

static void refresh_cb(lv_timer_t *t)
{
    (void)t;
    ui_app_refresh();
}

/* ---- popups ------------------------------------------------------------------------ */

static void popup_outside_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) ui_popup_close();
}

lv_obj_t *ui_popup_open(const lv_point_t *at, int w)
{
    ui_popup_close();
    /* A transparent catcher over the whole screen: a click outside the
     * panel closes it. Transparent objects draw nothing, so the video
     * holes stay holes. */
    g_popup = lv_obj_create(lv_layer_top());
    ui_plain(g_popup);
    lv_obj_set_size(g_popup, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(g_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_popup, popup_outside_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(g_popup);
    ui_plain(panel);
    lv_obj_add_style(panel, &ui_st_panel, 0);
    lv_obj_set_style_pad_all(panel, VG_SPACE_SM, 0);
    lv_obj_set_style_pad_row(panel, VG_SPACE_XS, 0);
    lv_obj_set_width(panel, w);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_layout(panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);      /* clicks on it are not "outside" */
    if (at) lv_obj_set_pos(panel, at->x, at->y);
    else lv_obj_center(panel);
    g_popup_panel = panel;
    over_video(panel);
    g_popup_group = lv_group_create();
    focus_sync();
    return panel;
}

void ui_popup_focus(lv_obj_t *cont, lv_obj_t *first, lv_gridnav_ctrl_t ctrl)
{
    if (!g_popup_group) return;
    ui_gridnav(cont, ctrl);
    lv_group_add_obj(g_popup_group, cont);
    if (first) ui_gridnav_focus(cont, first);
}

void ui_popup_place(const lv_point_t *at)
{
    if (!g_popup) return;
    lv_obj_t *panel = lv_obj_get_child(g_popup, 0);
    lv_obj_update_layout(panel);
    if (!at) {
        lv_obj_center(panel);
        return;
    }
    int sw = lv_display_get_horizontal_resolution(NULL), sh = lv_display_get_vertical_resolution(NULL);
    int w = lv_obj_get_width(panel), h = lv_obj_get_height(panel);
    int x = at->x, y = at->y;
    if (x + w > sw) x = sw - w;
    if (y + h > sh) y = sh - h;
    lv_obj_set_pos(panel, x < 0 ? 0 : x, y < 0 ? 0 : y);
}

lv_obj_t *ui_popup_sub(lv_obj_t *item, int w)
{
    if (!g_popup) return NULL;
    ui_popup_sub_close();
    g_sub_item = item;
    g_sub = lv_obj_create(g_popup);
    ui_plain(g_sub);
    lv_obj_add_style(g_sub, &ui_st_panel, 0);
    lv_obj_set_style_pad_all(g_sub, VG_SPACE_SM, 0);
    lv_obj_set_style_pad_row(g_sub, VG_SPACE_XS, 0);
    lv_obj_set_style_border_color(g_sub, UI_C(ACCENT), 0);
    lv_obj_set_width(g_sub, w);
    lv_obj_set_height(g_sub, LV_SIZE_CONTENT);
    lv_obj_set_layout(g_sub, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(g_sub, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(g_sub, LV_OBJ_FLAG_CLICKABLE);      /* clicks on it are not "outside" */
    over_video(g_sub);
    return g_sub;
}

/* Left in a submenu: back to the menu (after the key's processing: the
 * submenu goes away). */
static void sub_close_async(void *u)
{
    (void)u;
    ui_popup_sub_close();
}

static void sub_key_cb(lv_event_t *e)
{
    if (lv_event_get_key(e) == LV_KEY_LEFT) lv_async_call(sub_close_async, NULL);
}

/* Beside the item, to the right of the menu, or to its left when there is
 * no room; kept on the screen. */
void ui_popup_sub_place(lv_obj_t *first)
{
    if (!g_sub) return;
    lv_obj_update_layout(g_sub);
    lv_area_t item, menu;
    lv_obj_get_coords(g_sub_item, &item);
    lv_obj_get_coords(g_popup_panel, &menu);
    int sw = lv_display_get_horizontal_resolution(NULL), sh = lv_display_get_vertical_resolution(NULL);
    int w = lv_obj_get_width(g_sub), h = lv_obj_get_height(g_sub);
    int x = menu.x2 + VG_SPACE_XS, y = item.y1 - VG_SPACE_SM;
    if (x + w > sw) x = menu.x1 - VG_SPACE_XS - w;
    if (y + h > sh) y = sh - h;
    lv_obj_set_pos(g_sub, x < 0 ? 0 : x, y < 0 ? 0 : y);
    ui_gridnav(g_sub, LV_GRIDNAV_CTRL_ROLLOVER | LV_GRIDNAV_CTRL_VERTICAL_MOVE_ONLY);
    lv_obj_add_event_cb(g_sub, sub_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(g_popup_group, g_sub);
    lv_group_focus_obj(g_sub);
    if (first) ui_gridnav_focus(g_sub, first);
}

void ui_popup_sub_close(void)
{
    if (!g_sub) return;
    lv_group_remove_obj(g_sub);
    /* Async: this usually runs inside an event of one of its items. */
    lv_obj_delete_async(g_sub);
    g_sub = NULL;
    if (g_popup_group && lv_obj_get_group(g_popup_panel) == g_popup_group) lv_group_focus_obj(g_popup_panel);
}

int ui_popup_sub_is_open(void) { return g_sub != NULL; }

void ui_popup_close(void)
{
    if (!g_popup) return;
    lv_obj_delete_async(g_popup);
    g_popup = NULL;
    g_popup_panel = g_sub = g_sub_item = NULL;
    g_help = 0;
    lv_group_delete(g_popup_group);
    g_popup_group = NULL;
    focus_sync();
}

int ui_popup_is_open(void) { return g_popup != NULL; }

/* ---- toasts and questions ----------------------------------------------------------------- */

/* Toasts from events wait their turn (a later one must not hide an
 * earlier failure); a toast for what the user just did shows at once. */
#define TOASTS_WAITING 4

typedef struct {
    ui_toast_t kind;
    char       key[48];
    char       text[200];
} toast_t;

static toast_t g_waiting[TOASTS_WAITING];
static int     g_n_waiting;
static char    g_toast_key[48];          /* the shown toast's, "" for none or a user's */

static void toast_show(ui_toast_t kind, const char *key, const char *text);

static void toast_done_cb(lv_timer_t *t)
{
    (void)t;
    if (g_toast) lv_obj_delete(g_toast);
    g_toast = NULL;
    g_toast_timer = NULL;
    g_toast_key[0] = 0;
    if (g_n_waiting) {
        toast_t next = g_waiting[0];
        memmove(&g_waiting[0], &g_waiting[1], sizeof g_waiting[0] * (size_t)--g_n_waiting);
        toast_show(next.kind, next.key, next.text);
    }
}

static void toast_show(ui_toast_t kind, const char *key, const char *text)
{
    if (g_toast) lv_obj_delete(g_toast);
    if (g_toast_timer) lv_timer_delete(g_toast_timer);
    snprintf(g_toast_key, sizeof g_toast_key, "%s", key);

    lv_color_t c = kind == UI_TOAST_OK ? UI_C(OK) : kind == UI_TOAST_WARN ? UI_C(WARN) : UI_C(BAD);
    g_toast = lv_obj_create(lv_layer_top());
    ui_plain(g_toast);
    lv_obj_add_style(g_toast, &ui_st_panel, 0);
    lv_obj_set_style_border_color(g_toast, c, 0);
    lv_obj_set_style_border_width(g_toast, 2, 0);
    lv_obj_set_style_pad_hor(g_toast, VG_SPACE_XL, 0);
    lv_obj_set_style_pad_ver(g_toast, VG_SPACE_MD, 0);
    lv_obj_set_size(g_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(g_toast, 900, 0);
    lv_obj_remove_flag(g_toast, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = ui_label(g_toast, "", VG_FONT_MD_LV, UI_C(TEXT));
    const char *sym = kind == UI_TOAST_OK ? LV_SYMBOL_OK : LV_SYMBOL_WARNING;
    lv_label_set_text_fmt(l, "%s  %s", sym, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(l, 850, 0);
    lv_obj_align(g_toast, LV_ALIGN_TOP_MID, 0, VG_SIZE_PAGE_HEADER + VG_SPACE_LG);
    over_video(g_toast);
    g_toast_timer = lv_timer_create(toast_done_cb, VG_DURATION_TOAST, NULL);
    lv_timer_set_repeat_count(g_toast_timer, 1);
}

void ui_toast(ui_toast_t kind, const char *fmt, ...)
{
    char text[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    toast_show(kind, "", text);
}

void ui_toast_event(ui_toast_t kind, const char *key, const char *text)
{
    if (g_toast && !strcmp(g_toast_key, key)) {
        toast_show(kind, key, text);                    /* the same trouble, more cameras */
        return;
    }
    for (int i = 0; i < g_n_waiting; ++i)
        if (!strcmp(g_waiting[i].key, key)) {
            snprintf(g_waiting[i].text, sizeof g_waiting[i].text, "%s", text);
            return;
        }
    if (!g_toast) {
        toast_show(kind, key, text);
        return;
    }
    if (g_n_waiting == TOASTS_WAITING)                  /* keep the newest */
        memmove(&g_waiting[0], &g_waiting[1], sizeof g_waiting[0] * (size_t)--g_n_waiting);
    toast_t *w = &g_waiting[g_n_waiting++];
    w->kind = kind;
    snprintf(w->key, sizeof w->key, "%s", key);
    snprintf(w->text, sizeof w->text, "%s", text);
}

typedef struct {
    void (*on_yes)(void *);
    void *user;
} confirm_t;

static void confirm_cb(lv_event_t *e)
{
    confirm_t *c = lv_event_get_user_data(e);
    int yes = lv_obj_get_user_data(lv_event_get_target(e)) != NULL;
    void (*fn)(void *) = c->on_yes;
    void *user = c->user;
    ui_popup_close();
    if (yes && fn) fn(user);
}

static void confirm_free_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

void ui_confirm(const char *text, const char *yes, void (*on_yes)(void *), void *user)
{
    confirm_t *c = malloc(sizeof *c);
    if (!c) return;
    c->on_yes = on_yes;
    c->user = user;
    lv_obj_t *panel = ui_popup_open(NULL, 520);
    lv_obj_set_style_pad_all(panel, VG_SPACE_XL, 0);
    lv_obj_set_style_pad_row(panel, VG_SPACE_XL, 0);
    lv_obj_set_style_border_color(panel, UI_C(ACCENT), 0);
    lv_obj_add_event_cb(panel, confirm_free_cb, LV_EVENT_DELETE, c);
    lv_obj_t *l = ui_label(panel, text, VG_FONT_LG_LV, UI_C(TEXT));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_t *row = lv_obj_create(panel);
    ui_plain(row);
    ui_ring_room(row);                  /* the focus ring */
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, VG_SPACE_MD, 0);
    lv_obj_t *no = ui_btn(row, NULL, "Cancel");
    lv_obj_add_event_cb(no, confirm_cb, LV_EVENT_CLICKED, c);
    lv_obj_t *ok = ui_btn(row, NULL, yes);
    ui_btn_accent(ok);
    lv_obj_set_user_data(ok, (void *)1);
    lv_obj_add_event_cb(ok, confirm_cb, LV_EVENT_CLICKED, c);
    /* Enter right away keeps things as they are: a removal or a discard
     * takes a deliberate move to its button. */
    ui_popup_focus(row, no, LV_GRIDNAV_CTRL_ROLLOVER);
    ui_popup_place(NULL);
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    ui_popup_close();
}

void ui_not_yet(const char *what)
{
    lv_obj_t *panel = ui_popup_open(NULL, 520);
    lv_obj_set_style_pad_all(panel, VG_SPACE_XL, 0);
    lv_obj_set_style_pad_row(panel, VG_SPACE_XL, 0);
    lv_obj_set_style_border_color(panel, UI_C(ACCENT), 0);
    lv_obj_t *l = ui_label(panel, "", VG_FONT_LG_LV, UI_C(TEXT));
    lv_label_set_text_fmt(l, LV_SYMBOL_WARNING "  %s is not available yet.", what);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    ui_label(panel, "It is planned; this version of vigil does not have it.", VG_FONT_SM_LV, UI_C(TEXT2));
    lv_obj_t *row = lv_obj_create(panel);
    ui_plain(row);
    ui_ring_room(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_t *ok = ui_btn(row, NULL, "Close");
    lv_obj_align(ok, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(ok, close_cb, LV_EVENT_CLICKED, NULL);
    ui_popup_focus(row, ok, LV_GRIDNAV_CTRL_ROLLOVER);
    ui_popup_place(NULL);
}

/* ---- help ------------------------------------------------------------------------------- */

static const char *const k_help[][2] = {
    { NULL, "Live view" },
    { "Arrows", "Select a camera" },
    { "Enter", "Selected camera full screen, and back" },
    { "1  4  8  9  0", "Split: 1, 4, 8, 9, 16 windows" },
    { "PgUp PgDn   [ ]", "Previous, next page" },
    { "M   F2", "Main menu" },
    { "Menu   Shift+F10", "Context menu (Right: Split)" },
    { NULL, "Everywhere" },
    { "Tab   Shift+Tab", "Next, previous item" },
    { "Arrows", "Move; change a list or a number" },
    { "Enter   Space", "Activate; turn a switch on or off" },
    { "Enter in a text field", "Next field" },
    { "Ctrl+S", "Save the page" },
    { "Esc", "Back, close (asks before losing changes)" },
    { "F1   ?", "This help" },
};

void ui_help_toggle(void)
{
    if (g_help) {
        ui_popup_close();
        return;
    }
    lv_obj_t *panel = ui_popup_open(NULL, 620);
    g_help = 1;
    lv_obj_set_style_pad_all(panel, VG_SPACE_XL, 0);
    lv_obj_set_style_border_color(panel, UI_C(ACCENT), 0);
    ui_label(panel, LV_SYMBOL_KEYBOARD "  Keyboard shortcuts", VG_FONT_LG_LV, UI_C(TEXT));
    for (size_t i = 0; i < sizeof k_help / sizeof k_help[0]; ++i) {
        if (!k_help[i][0]) {
            lv_obj_t *h = ui_label(panel, k_help[i][1], VG_FONT_SM_LV, UI_C(ACCENT));
            lv_obj_set_style_pad_top(h, VG_SPACE_SM, 0);
            continue;
        }
        lv_obj_t *r = lv_obj_create(panel);
        ui_plain(r);
        ui_inert(r);
        lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_t *k = ui_label(r, k_help[i][0], VG_FONT_MD_LV, UI_C(TEXT));
        lv_obj_set_width(k, 220);
        lv_obj_t *v = ui_label(r, k_help[i][1], VG_FONT_MD_LV, UI_C(TEXT2));
        lv_obj_set_pos(v, 230, 0);
    }
    lv_obj_t *row = lv_obj_create(panel);
    ui_plain(row);
    ui_ring_room(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(row, VG_SPACE_MD, 0);
    lv_obj_t *close = ui_btn(row, NULL, "Close");
    lv_obj_align(close, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(close, close_cb, LV_EVENT_CLICKED, NULL);
    ui_popup_focus(row, close, LV_GRIDNAV_CTRL_ROLLOVER);
    ui_popup_place(NULL);
}

/* ---- the page stack --------------------------------------------------------------------- */

ui_page_t *ui_page_top(void) { return g_depth ? g_stack[g_depth - 1] : NULL; }

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_app_back();
}

static void page_deleted_cb(lv_event_t *e)
{
    ui_page_t *p = lv_event_get_user_data(e);
    if (p->on_close) p->on_close(p);
    lv_group_delete(p->group);
    free(p);
}

ui_page_t *ui_page_push(const char *title)
{
    if (g_depth == STACK_MAX) return NULL;
    ui_page_t *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    ui_popup_close();
    p->group = lv_group_create();
    p->scr = lv_obj_create(NULL);
    ui_plain(p->scr);
    lv_obj_add_style(p->scr, &ui_st_screen, 0);
    lv_obj_add_event_cb(p->scr, page_deleted_cb, LV_EVENT_DELETE, p);

    p->header = lv_obj_create(p->scr);
    ui_plain(p->header);
    lv_obj_set_size(p->header, LV_PCT(100), VG_SIZE_PAGE_HEADER);
    lv_obj_set_style_bg_color(p->header, UI_C(PANEL), 0);
    lv_obj_set_style_bg_opa(p->header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p->header, UI_C(LINE), 0);
    lv_obj_set_style_border_width(p->header, 1, 0);
    lv_obj_set_style_border_side(p->header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_hor(p->header, VG_SPACE_LG, 0);
    lv_obj_set_layout(p->header, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(p->header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(p->header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(p->header, VG_SPACE_LG, 0);

    lv_obj_t *back = ui_btn(p->header, LV_SYMBOL_LEFT, "Back");
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);
    /* First in the order: what a mouse user's page has focused (shown as
     * nothing), and Shift+Tab from the first field. */
    lv_group_add_obj(p->group, back);
    p->title = ui_label(p->header, title, VG_FONT_LG_LV, UI_C(TEXT));
    lv_obj_set_flex_grow(p->title, 1);
    p->actions = lv_obj_create(p->header);
    ui_plain(p->actions);
    ui_ring_room(p->actions);            /* the focus ring */
    lv_obj_set_size(p->actions, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_layout(p->actions, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(p->actions, LV_FLEX_FLOW_ROW);
    /* START: a content-sized row aligned to its end is measured wrongly
     * (the overflow to the left is not counted). The title's grow pushes
     * it to the right anyway. */
    lv_obj_set_flex_align(p->actions, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(p->actions, VG_SPACE_SM, 0);
    lv_obj_t *clk = ui_label(p->header, "", VG_FONT_SM_LV, UI_C(TEXT2));
    lv_label_bind_text(clk, &ui_subj_clock, NULL);

    p->body = lv_obj_create(p->scr);
    ui_plain(p->body);
    lv_obj_add_flag(p->body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(p->body, LV_DIR_VER);
    lv_obj_set_pos(p->body, 0, VG_SIZE_PAGE_HEADER);
    lv_obj_set_size(p->body, LV_PCT(100), lv_display_get_vertical_resolution(NULL) - VG_SIZE_PAGE_HEADER);
    lv_obj_set_style_pad_all(p->body, VG_SPACE_XL, 0);
    lv_obj_set_style_pad_row(p->body, VG_SPACE_LG, 0);
    lv_obj_set_layout(p->body, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(p->body, LV_FLEX_FLOW_COLUMN);

    g_stack[g_depth++] = p;
    lv_screen_load(p->scr);
    focus_sync();
    return p;
}

ui_page_t *ui_page_push_plain(void)
{
    if (g_depth == STACK_MAX) return NULL;
    ui_page_t *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    ui_popup_close();
    p->group = lv_group_create();
    p->scr = lv_obj_create(NULL);
    ui_plain(p->scr);
    lv_obj_add_style(p->scr, &ui_st_screen, 0);
    lv_obj_add_event_cb(p->scr, page_deleted_cb, LV_EVENT_DELETE, p);
    p->body = lv_obj_create(p->scr);
    ui_plain(p->body);
    lv_obj_set_size(p->body, LV_PCT(100), LV_PCT(100));
    g_stack[g_depth++] = p;
    lv_screen_load(p->scr);
    focus_sync();
    return p;
}

void ui_page_ready(ui_page_t *p)
{
    uint32_t n = p->actions ? lv_obj_get_child_count(p->actions) : 0;
    for (uint32_t i = 0; i < n; ++i) lv_group_add_obj(p->group, lv_obj_get_child(p->actions, (int32_t)i));
    if (g_kbd_used && p->header) lv_group_focus_next(p->group);   /* from Back to the first */
}

void ui_page_close(ui_page_t *p)
{
    if (!g_depth || g_stack[g_depth - 1] != p) return;
    ui_popup_close();
    --g_depth;
    lv_screen_load(g_depth ? g_stack[g_depth - 1]->scr : g_live);
    focus_sync();
    /* Async: this usually runs inside an event of one of its widgets. */
    lv_obj_delete_async(p->scr);
}

void ui_app_back(void)
{
    if (g_sub) {
        ui_popup_sub_close();
        return;
    }
    if (g_popup) {
        ui_popup_close();
        return;
    }
    ui_page_t *p = ui_page_top();
    if (p) {
        if (p->on_back && p->on_back(p)) return;
        ui_page_close(p);
        return;
    }
    lv_point_t at;
    if (ui_pointer(&at))
        ui_live_context_menu(&at);
    else
        ui_live_context_menu(NULL);
}

/* ---- keyboard ------------------------------------------------------------------------- */

static void help_async(void *u)
{
    (void)u;
    ui_help_toggle();
}

static void save_async(void *u)
{
    (void)u;
    ui_page_t *p = ui_page_top();
    if (p && p->save && !g_popup) lv_obj_send_event(p->save, LV_EVENT_CLICKED, NULL);
}

/* Esc: back, as the right button, but on the live view it only closes
 * (a menu that opens on Esc would be a surprise). */
static void esc_async(void *u)
{
    (void)u;
    if (g_popup || ui_page_top()) ui_app_back();
    else ui_live_key(LV_KEY_ESC, 0);
}

static int is_type(lv_obj_t *o, const lv_obj_class_t *c) { return o && lv_obj_check_type(o, c); }

uint32_t ui_app_key(uint32_t key, unsigned mods)
{
    if (!g_kbd_used) {
        g_kbd_used = 1;
        ui_theme_focus_visible(1);
    }
    lv_group_t *grp = focus_group();
    lv_obj_t *foc = grp ? lv_group_get_focused(grp) : NULL;
    int typing = is_type(foc, &lv_textarea_class);
    int dd = is_type(foc, &lv_dropdown_class), dd_open = dd && lv_dropdown_is_open(foc);
    int up = key == LV_KEY_UP || key == LV_KEY_LEFT;

    if (key == UI_KEY_F(1) || (key == '?' && !typing && !(mods & (UI_MOD_CTRL | UI_MOD_ALT)))) {
        lv_async_call(help_async, NULL);
        return 0;
    }
    if (mods & (UI_MOD_CTRL | UI_MOD_ALT)) {
        if (mods == UI_MOD_CTRL && key == 's') lv_async_call(save_async, NULL);
        return 0;
    }
    if (key == UI_KEY_F(10) && (mods & UI_MOD_SHIFT)) key = UI_KEY_MENU;
    if (key == LV_KEY_ESC) {
        if (dd_open) return key;                /* the list closes */
        lv_async_call(esc_async, NULL);
        return 0;
    }
    if (!g_popup && !ui_page_top()) {
        if (ui_live_key(key, mods)) return 0;
        return key >= UI_KEY_BASE ? 0 : key;
    }
    if (key >= UI_KEY_BASE) return 0;
    if (key == ' ' && !typing) return LV_KEY_ENTER;
    if (dd && !dd_open && (key == LV_KEY_UP || key == LV_KEY_DOWN || key == LV_KEY_LEFT || key == LV_KEY_RIGHT)) {
        /* A closed list changes in place, as a <select> does. */
        uint32_t sel = lv_dropdown_get_selected(foc), n = lv_dropdown_get_option_count(foc);
        if (up ? sel > 0 : sel + 1 < n) {
            lv_dropdown_set_selected(foc, up ? sel - 1 : sel + 1);
            lv_obj_send_event(foc, LV_EVENT_VALUE_CHANGED, NULL);
        }
        return 0;
    }
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        if (!foc || dd_open || lv_obj_has_flag(foc, UI_FLAG_ARROWS)) return key;
        if (up) lv_group_focus_prev(grp);
        else lv_group_focus_next(grp);
        return 0;
    }
    if ((key == LV_KEY_LEFT || key == LV_KEY_RIGHT) && is_type(foc, &lv_button_class)) {
        if (up) lv_group_focus_prev(grp);
        else lv_group_focus_next(grp);
        return 0;
    }
    return key;
}

void ui_app_keyboards(int n)
{
    if (n) return;
    g_kbd_used = 0;
    ui_theme_focus_visible(0);
}

void ui_app_create(const ui_model_t *m)
{
    g_model = m;
    for (int i = 0; i < UI_MAX_CHANNELS; ++i) lv_subject_init_pointer(&g_subj_ch[i], &g_status[i]);
    lv_subject_init_int(&ui_subj_config, m->config_generation ? (int)m->config_generation() : 0);
    clock_text(g_clock, sizeof g_clock);
    lv_subject_init_string(&ui_subj_clock, g_clock, g_clock_prev, sizeof g_clock, g_clock);
    lv_subject_init_int(&ui_subj_nav_pinned, 0);

    ui_theme_init(lv_display_get_default());
    g_live = ui_live_create();
    lv_screen_load(g_live);
    focus_sync();
    ui_app_refresh();
    lv_timer_create(refresh_cb, REFRESH_MS, NULL);
}
