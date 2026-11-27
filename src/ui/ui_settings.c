/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_settings.c — the two-pane settings screen and its building blocks
 * (see ui_settings.h). */
#define _DEFAULT_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ui_i18n.h"
#include "ui_osk.h"
#include "ui_settings.h"
#include "ui_theme.h"

#define NAV_W     260
#define LABEL_W   280
#define ITEMS_MAX 12
#define JOB_MS    300

struct ui_set {
    ui_page_t           *page;
    const ui_set_item_t *items;
    int                  n, cur;
    void                *st;
    lv_obj_t            *nav, *nav_btn[ITEMS_MAX], *pane;
    lv_obj_t            *save, *cancel, *kb;
    int                  want;          /* after "discard?": the item to show, -1 close */
    int                  no_actions;    /* set by the sub-page's open() */
};

lv_group_t *ui_set_group(ui_set_t *s) { return s->page->group; }
lv_obj_t   *ui_set_save_btn(ui_set_t *s) { return s->save; }
lv_obj_t   *ui_set_kb(ui_set_t *s) { return s->kb; }

static const ui_set_item_t *item(ui_set_t *s) { return &s->items[s->cur]; }

static int dirty(ui_set_t *s)
{
    return s->st && item(s)->dirty && item(s)->dirty(s->st);
}

/* Save and Cancel: there for a sub-page that saves, last in the order. */
static void actions_sync(ui_set_t *s)
{
    int has = item(s)->save != NULL && !s->no_actions;
    lv_group_remove_obj(s->cancel);
    lv_group_remove_obj(s->save);
    if (has) {
        lv_obj_remove_flag(s->cancel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s->save, LV_OBJ_FLAG_HIDDEN);
        lv_group_add_obj(s->page->group, s->cancel);
        lv_group_add_obj(s->page->group, s->save);
    } else {
        lv_obj_add_flag(s->cancel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s->save, LV_OBJ_FLAG_HIDDEN);
    }
    s->page->save = has ? s->save : NULL;
}

void ui_set_hide_actions(ui_set_t *s) { s->no_actions = 1; }

int ui_set_section_index(const char *key)
{
    for (int i = 0; i < ui_model()->n_sections; ++i)
        if (!strcmp(ui_model()->sections[i].key, key)) return i;
    return -1;
}

ui_form_t *ui_set_form(ui_set_t *s, lv_obj_t *parent, const char *key)
{
    int sec = ui_set_section_index(key);
    return sec < 0 ? NULL : ui_form_embed(parent, s->page->group, s->save, s->kb, sec);
}

static void kb_hide(ui_set_t *s)
{
    if (s->kb && ui_osk_shown(s->kb)) ui_osk_hide(s->kb);
}

static void show(ui_set_t *s, int i)
{
    kb_hide(s);
    if (s->st && item(s)->close) item(s)->close(s->st);
    s->st = NULL;
    lv_obj_clean(s->pane);
    lv_obj_scroll_to_y(s->pane, 0, LV_ANIM_OFF);
    if (s->nav_btn[s->cur]) lv_obj_remove_state(s->nav_btn[s->cur], LV_STATE_CHECKED);
    s->cur = i;
    lv_obj_add_state(s->nav_btn[i], LV_STATE_CHECKED);
    ui_gridnav_focus(s->nav, s->nav_btn[i]);
    char title[96];
    snprintf(title, sizeof title, "%s  " LV_SYMBOL_RIGHT "  %s", lv_obj_get_user_data(s->page->title)
                                                                     ? (const char *)lv_obj_get_user_data(s->page->title)
                                                                     : "",
             item(s)->label);
    lv_label_set_text(s->page->title, title);
    s->no_actions = 0;
    s->st = item(s)->open ? item(s)->open(s, s->pane) : NULL;
    actions_sync(s);
}

void ui_set_reopen(ui_set_t *s) { show(s, s->cur); }

static void discard_cb(void *user)
{
    ui_set_t *s = user;
    if (s->st && item(s)->cancel) item(s)->cancel(s->st);
    if (s->want < 0) ui_page_close(s->page);
    else show(s, s->want);
}

/* Go to item `i` (or close, -1), asking first when there are changes. */
static void leave(ui_set_t *s, int i)
{
    if (i == s->cur) return;
    if (dirty(s)) {
        s->want = i;
        ui_confirm(TR(S_DISCARD_Q), TR(S_DISCARD), discard_cb, s);
        return;
    }
    if (i < 0) ui_page_close(s->page);
    else show(s, i);
}

static void nav_cb(lv_event_t *e)
{
    ui_set_t *s = lv_event_get_user_data(e);
    lv_obj_t *b = lv_event_get_target(e);
    for (int i = 0; i < s->n; ++i)
        if (s->nav_btn[i] == b) leave(s, i);
}

static void save_cb(lv_event_t *e)
{
    ui_set_t *s = lv_event_get_user_data(e);
    kb_hide(s);
    if (s->st && item(s)->save) item(s)->save(s->st);
}

static void cancel_cb(lv_event_t *e)
{
    ui_set_t *s = lv_event_get_user_data(e);
    kb_hide(s);
    if (s->st && item(s)->cancel) item(s)->cancel(s->st);
}

static int on_back(ui_page_t *p)
{
    ui_set_t *s = p->user;
    if (s->kb && ui_osk_shown(s->kb)) {
        kb_hide(s);
        return 1;
    }
    if (dirty(s)) {
        leave(s, -1);
        return 1;
    }
    return 0;
}

static void on_close(ui_page_t *p)
{
    ui_set_t *s = p->user;
    if (s->st && item(s)->close) item(s)->close(s->st);
    free(s);
}

static void kb_cb(lv_event_t *e) { kb_hide(lv_event_get_user_data(e)); }

void ui_settings_open(const char *title, const ui_set_item_t *items, int n, int first)
{
    if (n < 1 || n > ITEMS_MAX) return;
    ui_set_t *s = calloc(1, sizeof *s);
    if (!s) return;
    ui_page_t *p = ui_page_push(title);
    if (!p) {
        free(s);
        return;
    }
    s->page = p;
    s->items = items;
    s->n = n;
    p->user = s;
    p->on_back = on_back;
    p->on_close = on_close;
    lv_obj_set_user_data(p->title, (void *)title);

    s->cancel = ui_btn(p->actions, LV_SYMBOL_CLOSE, TR(S_CANCEL));
    lv_obj_add_event_cb(s->cancel, cancel_cb, LV_EVENT_CLICKED, s);
    s->save = ui_btn(p->actions, LV_SYMBOL_SAVE, TR(S_SAVE));
    ui_btn_accent(s->save);
    lv_obj_add_event_cb(s->save, save_cb, LV_EVENT_CLICKED, s);

    /* The body is the two panes side by side, each scrolling alone. */
    lv_obj_t *b = p->body;
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(b, 0, 0);

    s->nav = lv_obj_create(b);
    ui_plain(s->nav);
    lv_obj_set_size(s->nav, NAV_W, LV_PCT(100));
    lv_obj_set_style_bg_color(s->nav, UI_C(PANEL), 0);
    lv_obj_set_style_bg_opa(s->nav, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s->nav, UI_C(LINE), 0);
    lv_obj_set_style_border_side(s->nav, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_width(s->nav, 1, 0);
    lv_obj_set_style_pad_all(s->nav, VG_SPACE_MD, 0);
    lv_obj_set_style_pad_row(s->nav, VG_SPACE_XS, 0);
    lv_obj_set_layout(s->nav, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s->nav, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s->nav, LV_OBJ_FLAG_SCROLLABLE);
    ui_ring_room(s->nav);
    for (int i = 0; i < n; ++i) {
        lv_obj_t *bt = ui_btn(s->nav, NULL, NULL);
        lv_obj_set_size(bt, LV_PCT(100), VG_SIZE_BUTTON + VG_SPACE_SM);
        lv_obj_set_layout(bt, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(bt, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(bt, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(bt, VG_SPACE_MD, 0);
        lv_obj_set_style_pad_hor(bt, VG_SPACE_MD, 0);
        /* The sub-page shown: the accent, as a pressed key stays lit. */
        lv_obj_set_style_bg_color(bt, UI_C(ACCENT), LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(bt, LV_OPA_COVER, LV_STATE_CHECKED);
        if (items[i].icon) ui_icon(bt, items[i].icon, VG_COLOR_TEXT);
        ui_label(bt, items[i].label, VG_FONT_MD_LV, UI_C(TEXT));
        lv_obj_add_event_cb(bt, nav_cb, LV_EVENT_CLICKED, s);
        s->nav_btn[i] = bt;
    }
    ui_gridnav(s->nav, LV_GRIDNAV_CTRL_ROLLOVER | LV_GRIDNAV_CTRL_SCROLL_FIRST);
    lv_group_add_obj(p->group, s->nav);

    s->pane = lv_obj_create(b);
    ui_plain(s->pane);
    lv_obj_set_height(s->pane, LV_PCT(100));
    lv_obj_set_flex_grow(s->pane, 1);
    lv_obj_add_flag(s->pane, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s->pane, LV_DIR_VER);
    lv_obj_set_style_pad_all(s->pane, VG_SPACE_XL, 0);
    lv_obj_set_style_pad_row(s->pane, VG_SPACE_LG, 0);
    lv_obj_set_layout(s->pane, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s->pane, LV_FLEX_FLOW_COLUMN);
    ui_ring_room(s->pane);

    s->kb = ui_osk_create(p->scr);
    if (s->kb) {
        lv_obj_add_event_cb(s->kb, kb_cb, LV_EVENT_READY, s);
        lv_obj_add_event_cb(s->kb, kb_cb, LV_EVENT_CANCEL, s);
    }
    ui_page_ready(p);
    s->cur = 0;
    show(s, first >= 0 && first < n ? first : 0);
}

/* ---- building blocks ---------------------------------------------------------------- */

static lv_obj_t *column(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    ui_plain(c);
    ui_ring_room(c);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(c, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    return c;
}

lv_obj_t *ui_set_section(lv_obj_t *pane, const char *title)
{
    if (title) ui_label(pane, title, VG_FONT_LG_LV, UI_C(TEXT));
    lv_obj_t *p = column(pane);
    lv_obj_add_style(p, &ui_st_panel, 0);
    lv_obj_set_style_pad_ver(p, VG_SPACE_SM, 0);
    lv_obj_set_style_pad_hor(p, VG_SPACE_LG, 0);
    return p;
}

lv_obj_t *ui_set_row(lv_obj_t *sect, const char *label, const char *help)
{
    lv_obj_t *row = lv_obj_create(sect);
    ui_plain(row);
    ui_ring_room(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(row, VG_SPACE_SM, 0);
    lv_obj_set_style_border_color(row, UI_C(LINE), 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(row, lv_obj_get_child_count(sect) > 1 ? 1 : 0, 0);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, VG_SPACE_XL, 0);
    lv_obj_t *l = ui_label(row, label, VG_FONT_MD_LV, UI_C(TEXT));
    lv_obj_set_width(l, LABEL_W);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_top(l, VG_SPACE_SM, 0);
    lv_obj_t *right = column(row);
    lv_obj_set_width(right, 0);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_style_pad_row(right, VG_SPACE_XS, 0);
    lv_obj_t *box = lv_obj_create(right);
    ui_plain(box);
    ui_ring_room(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box, VG_SPACE_SM, 0);
    lv_obj_set_style_pad_row(box, VG_SPACE_SM, 0);
    if (help && *help) ui_set_note(right, help, UI_C(TEXT2));
    return box;
}

lv_obj_t *ui_set_note(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *l = ui_label(parent, text, VG_FONT_SM_LV, color);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

lv_obj_t *ui_set_info(lv_obj_t *sect, const char *label, const char *value)
{
    lv_obj_t *box = ui_set_row(sect, label, NULL);
    lv_obj_t *v = ui_label(box, value, VG_FONT_MD_LV, UI_C(TEXT2));
    lv_obj_set_style_pad_top(v, VG_SPACE_SM, 0);
    return v;
}

lv_obj_t *ui_set_hbox(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    ui_plain(r);
    ui_ring_room(r);
    lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(r, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, VG_SPACE_SM, 0);
    return r;
}

/* ---- widgets ------------------------------------------------------------------------- */

static void ta_click_cb(lv_event_t *e)
{
    ui_set_t *s = lv_event_get_user_data(e);
    lv_obj_t *ta = lv_event_get_target(e);
    if (ui_keyboard_in_use() || !s->kb) return;
    ui_osk_show(s->kb, ta, lv_obj_has_flag(ta, LV_OBJ_FLAG_USER_2));
}

/* Enter from a keyboard: the next widget (the on-screen keyboard's OK
 * sends READY too, and only closes it). */
static void ta_ready_cb(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (!in || lv_indev_get_type(in) != LV_INDEV_TYPE_KEYPAD) return;
    lv_indev_wait_release(in);
    lv_group_focus_next(lv_obj_get_group(lv_event_get_target(e)));
}

static lv_obj_t *textarea(ui_set_t *s, lv_obj_t *box, const char *value, int max_len, int w)
{
    lv_obj_t *ta = lv_textarea_create(box);
    lv_textarea_set_one_line(ta, 1);
    if (max_len > 0) lv_textarea_set_max_length(ta, (uint32_t)max_len);
    lv_textarea_set_text(ta, value ? value : "");
    lv_obj_set_width(ta, w > 0 ? w : LV_PCT(100));
    lv_obj_add_event_cb(ta, ta_click_cb, LV_EVENT_CLICKED, s);
    lv_obj_add_event_cb(ta, ta_ready_cb, LV_EVENT_READY, s);
    lv_obj_set_style_border_color(ta, UI_C(BAD), LV_PART_MAIN | LV_STATE_USER_1);
    lv_obj_set_style_border_width(ta, 2, LV_PART_MAIN | LV_STATE_USER_1);
    ui_focus_style(ta);
    lv_group_add_obj(s->page->group, ta);
    return ta;
}

lv_obj_t *ui_set_text(ui_set_t *s, lv_obj_t *box, const char *value, int max_len, int w)
{
    return textarea(s, box, value, max_len, w);
}

lv_obj_t *ui_set_password(ui_set_t *s, lv_obj_t *box, int max_len, int w)
{
    lv_obj_t *ta = textarea(s, box, "", max_len, w);
    lv_textarea_set_password_mode(ta, 1);
    return ta;
}

typedef struct {
    int min, max;
} range_t;

static void range_free_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

static void num_step(lv_obj_t *ta, int d)
{
    range_t *r = lv_obj_get_user_data(ta);
    long v = strtol(lv_textarea_get_text(ta), NULL, 10) + d;
    if (r && v < r->min) v = r->min;
    if (r && v > r->max) v = r->max;
    char b[16];
    snprintf(b, sizeof b, "%ld", v);
    lv_textarea_set_text(ta, b);
}

static void num_btn_cb(lv_event_t *e)
{
    lv_obj_t *b = lv_event_get_target(e);
    num_step(lv_event_get_user_data(e), (int)(intptr_t)lv_obj_get_user_data(b));
}

static void num_key_cb(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    if (k != LV_KEY_UP && k != LV_KEY_DOWN) return;
    num_step(lv_event_get_target(e), k == LV_KEY_UP ? 1 : -1);
    lv_event_stop_processing(e);
}

lv_obj_t *ui_set_number(ui_set_t *s, lv_obj_t *box, int value, int min, int max)
{
    lv_obj_t *h = ui_set_hbox(box);
    lv_obj_t *minus = ui_btn(h, LV_SYMBOL_MINUS, NULL);
    lv_obj_set_width(minus, VG_SIZE_BUTTON);
    char v[16];
    snprintf(v, sizeof v, "%d", value);
    lv_obj_t *ta = textarea(s, h, v, 11, 110);
    lv_obj_add_flag(ta, LV_OBJ_FLAG_USER_2);                    /* numeric keyboard */
    lv_textarea_set_accepted_chars(ta, min < 0 ? "-0123456789" : "0123456789");
    lv_obj_set_style_text_align(ta, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(ta, UI_FLAG_ARROWS);
    lv_obj_add_event_cb(ta, num_key_cb, LV_EVENT_KEY | LV_EVENT_PREPROCESS, NULL);
    range_t *r = malloc(sizeof *r);
    if (r) {
        r->min = min;
        r->max = max;
        lv_obj_set_user_data(ta, r);
        lv_obj_add_event_cb(ta, range_free_cb, LV_EVENT_DELETE, r);
    }
    lv_obj_t *plus = ui_btn(h, LV_SYMBOL_PLUS, NULL);
    lv_obj_set_width(plus, VG_SIZE_BUTTON);
    lv_obj_set_user_data(minus, (void *)(intptr_t)-1);
    lv_obj_set_user_data(plus, (void *)(intptr_t)1);
    lv_obj_add_event_cb(minus, num_btn_cb, LV_EVENT_CLICKED, ta);
    lv_obj_add_event_cb(minus, num_btn_cb, LV_EVENT_LONG_PRESSED_REPEAT, ta);
    lv_obj_add_event_cb(plus, num_btn_cb, LV_EVENT_CLICKED, ta);
    lv_obj_add_event_cb(plus, num_btn_cb, LV_EVENT_LONG_PRESSED_REPEAT, ta);
    char rg[40];
    snprintf(rg, sizeof rg, "%d ... %d", min, max);
    ui_label(h, rg, VG_FONT_SM_LV, UI_C(TEXT2));
    return ta;
}

int ui_set_number_get(lv_obj_t *ta)
{
    range_t *r = lv_obj_get_user_data(ta);
    long v = strtol(lv_textarea_get_text(ta), NULL, 10);
    if (r && v < r->min) v = r->min;
    if (r && v > r->max) v = r->max;
    return (int)v;
}

lv_obj_t *ui_set_switch(ui_set_t *s, lv_obj_t *box, int on)
{
    lv_obj_t *sw = lv_switch_create(box);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    ui_focus_style(sw);
    lv_group_add_obj(s->page->group, sw);
    return sw;
}

lv_obj_t *ui_set_dropdown(ui_set_t *s, lv_obj_t *box, const char *options, int sel, int w)
{
    lv_obj_t *d = lv_dropdown_create(box);
    lv_dropdown_set_options(d, options);
    if (sel >= 0) lv_dropdown_set_selected(d, (uint32_t)sel);
    lv_obj_set_width(d, w > 0 ? w : 320);
    ui_focus_style(d);
    lv_group_add_obj(s->page->group, d);
    return d;
}

lv_obj_t *ui_set_button(ui_set_t *s, lv_obj_t *box, const char *symbol, const char *text)
{
    lv_obj_t *b = ui_btn(box, symbol, text);
    lv_group_add_obj(s->page->group, b);
    return b;
}

/* ---- jobs ---------------------------------------------------------------------------- */

typedef struct {
    int         id;
    lv_obj_t   *label;
    lv_timer_t *timer;
    void (*done)(void *user, int ok, const char *msg);
    void       *user;
} follow_t;

static void follow_label_deleted(lv_event_t *e);

static void follow_end(follow_t *f)
{
    if (f->label) lv_obj_remove_event_cb_with_user_data(f->label, follow_label_deleted, f);
    lv_timer_delete(f->timer);
    free(f);
}

static void follow_tick(lv_timer_t *t)
{
    follow_t *f = lv_timer_get_user_data(t);
    const ui_mgmt_t *m = ui_model()->mgmt;
    ui_mjob_t j;
    if (!m || !m->job || m->job(f->id, &j) < 0) {
        if (f->done) f->done(f->user, 0, "the job is no longer known");
        follow_end(f);
        return;
    }
    if (j.state <= UI_JOBST_RUNNING) {
        if (f->label)
            lv_label_set_text(f->label, j.state == UI_JOBST_QUEUED ? LV_SYMBOL_REFRESH "  Waiting..."
                                                                   : LV_SYMBOL_REFRESH "  Working...");
        return;
    }
    int ok = j.state == UI_JOBST_DONE;
    if (f->label) {
        lv_label_set_text_fmt(f->label, "%s  %s", ok ? LV_SYMBOL_OK : LV_SYMBOL_WARNING, j.msg);
        lv_obj_set_style_text_color(f->label, ok ? UI_C(OK) : UI_C(BAD), 0);
    }
    void (*done)(void *, int, const char *) = f->done;
    void *user = f->user;
    char msg[256];
    snprintf(msg, sizeof msg, "%s", j.msg);
    follow_end(f);
    if (done) done(user, ok, msg);
}

static void follow_label_deleted(lv_event_t *e)
{
    follow_t *f = lv_event_get_user_data(e);
    f->label = NULL;
    lv_timer_delete(f->timer);
    free(f);
}

void ui_set_follow_job(ui_set_t *s, int id, lv_obj_t *label, void (*done)(void *user, int ok, const char *msg),
                       void *user)
{
    (void)s;
    follow_t *f = calloc(1, sizeof *f);
    if (!f) return;
    f->id = id;
    f->label = label;
    f->done = done;
    f->user = user;
    f->timer = lv_timer_create(follow_tick, JOB_MS, f);
    if (label) {
        lv_obj_set_style_text_color(label, UI_C(TEXT2), 0);
        lv_label_set_text(label, LV_SYMBOL_REFRESH "  Working...");
        lv_obj_add_event_cb(label, follow_label_deleted, LV_EVENT_DELETE, f);
    }
}

/* ---- text ---------------------------------------------------------------------------- */

void ui_set_size_text(unsigned long long b, char *out, size_t cap)
{
    if (b >= 1000000000000ull) snprintf(out, cap, "%.1f TB", (double)b / 1e12);
    else if (b >= 1000000000ull) snprintf(out, cap, "%.1f GB", (double)b / 1e9);
    else if (b >= 1000000ull) snprintf(out, cap, "%.0f MB", (double)b / 1e6);
    else snprintf(out, cap, "%llu KB", b / 1000);
}

void ui_set_time_text(long long t, char *out, size_t cap)
{
    if (!t) {
        snprintf(out, cap, "never");
        return;
    }
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    ui_format_datetime(&tm, 0, out, cap);
}
