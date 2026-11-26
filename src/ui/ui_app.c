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
#include "ui_events.h"
#include "ui_theme.h"

#define REFRESH_MS  250
#define STACK_MAX   8

static const ui_model_t *g_model;
static lv_obj_t         *g_live;
static ui_page_t        *g_stack[STACK_MAX];
static int               g_depth;
static lv_indev_t       *g_pointer;

static ui_chan_t    g_status[UI_MAX_CHANNELS];
static lv_subject_t g_subj_ch[UI_MAX_CHANNELS];
lv_subject_t        ui_subj_config;
lv_subject_t        ui_subj_clock;
lv_subject_t        ui_subj_nav_pinned;
static char         g_clock[48], g_clock_prev[48];

static lv_obj_t   *g_popup;         /* the full-screen catcher; the panel is its child */
static lv_obj_t   *g_toast;
static lv_timer_t *g_toast_timer;

const ui_model_t *ui_model(void) { return g_model; }
lv_subject_t *ui_subject_channel(int ch) { return &g_subj_ch[ch]; }
const ui_chan_t *ui_channel(int ch) { return &g_status[ch]; }
void ui_app_set_pointer(lv_indev_t *indev) { g_pointer = indev; }

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

static void clock_text(char *buf, size_t cap)
{
    time_t now = g_model->now ? g_model->now() : time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(buf, cap, "%04d-%02d-%02d  %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
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
    return panel;
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

void ui_popup_close(void)
{
    if (!g_popup) return;
    lv_obj_delete_async(g_popup);
    g_popup = NULL;
}

int ui_popup_is_open(void) { return g_popup != NULL; }

/* ---- toasts and questions ----------------------------------------------------------------- */

static void toast_done_cb(lv_timer_t *t)
{
    (void)t;
    if (g_toast) lv_obj_delete(g_toast);
    g_toast = NULL;
    g_toast_timer = NULL;
}

void ui_toast(ui_toast_t kind, const char *fmt, ...)
{
    char text[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (g_toast) lv_obj_delete(g_toast);
    if (g_toast_timer) lv_timer_delete(g_toast_timer);

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
    g_toast_timer = lv_timer_create(toast_done_cb, VG_DURATION_TOAST, NULL);
    lv_timer_set_repeat_count(g_toast_timer, 1);
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
    free(p);
}

ui_page_t *ui_page_push(const char *title)
{
    if (g_depth == STACK_MAX) return NULL;
    ui_page_t *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    ui_popup_close();
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
    p->title = ui_label(p->header, title, VG_FONT_LG_LV, UI_C(TEXT));
    lv_obj_set_flex_grow(p->title, 1);
    p->actions = lv_obj_create(p->header);
    ui_plain(p->actions);
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
    return p;
}

void ui_page_close(ui_page_t *p)
{
    if (!g_depth || g_stack[g_depth - 1] != p) return;
    ui_popup_close();
    --g_depth;
    lv_screen_load(g_depth ? g_stack[g_depth - 1]->scr : g_live);
    /* Async: this usually runs inside an event of one of its widgets. */
    lv_obj_delete_async(p->scr);
}

void ui_app_back(void)
{
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
    ui_app_refresh();
    lv_timer_create(refresh_cb, REFRESH_MS, NULL);
}
