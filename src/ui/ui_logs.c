/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_logs.c — Operation -> Logs (see ui_logs.h). */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "icons.h"
#include "ui_app.h"
#include "ui_logs.h"
#include "ui_settings.h"
#include "ui_theme.h"

#define PAGE       50               /* rows a page: made once, refilled */
#define COLS       5
#define TYPES_MAX  48
#define TICK_MS    2000             /* the first page follows what comes in */
#define SEARCH_MS  500              /* the search applies this long after the last key */
#define TARGETS    6

static const ui_logs_src_t *g_src;

void ui_logs_set_source(const ui_logs_src_t *src) { g_src = src; }
const ui_logs_src_t *ui_logs_source(void) { return g_src; }

void ui_logs_audit(const char *action, const char *detail)
{
    if (g_src && g_src->audit) g_src->audit(action, detail);
}

/* ---- the columns ------------------------------------------------------------------------- */

typedef struct {
    const char *head[COLS];
    int         w[COLS];            /* the last one: what is left (0: none) */
} cols_t;

static const cols_t k_cols[UI_LOG_KINDS] = {
    [UI_LOG_SYSTEM] = { { "Time", "Level", "From", "Message", NULL }, { 178, 76, 120, -1, 0 } },
    [UI_LOG_ALARMS] = { { "Time", "Channel", "Type", "State", "Detail" }, { 178, 120, 150, 64, -1 } },
    [UI_LOG_OPS] = { { "Time", "User", "From", "Action", "Detail" }, { 178, 110, 76, 150, -1 } },
};

static const char *const k_ranges = "Today\nLast 24 hours\nLast 7 days\nAll\nFrom ... to ...";
enum { R_TODAY, R_24H, R_7D, R_ALL, R_CUSTOM };
static const char *const k_sources[] = { "screen", "web", "api", "onvif", "netip", "rtsp", "system" };
static const int k_levels[] = { -1, 3, 4, 5, 6 };

typedef struct {
    ui_set_t     *s;
    int           kind;
    lv_obj_t     *range, *type, *third, *search, *custom, *from, *to;
    lv_obj_t     *newer, *older, *pos, *follow, *where, *err;
    lv_obj_t     *table, *row[PAGE], *cell[PAGE][COLS];
    ui_log_row_t  rows[PAGE];
    int           n, offset, more;
    unsigned long seen;
    const char   *types[TYPES_MAX];
    int           ntypes;
    lv_timer_t   *tick, *debounce;
    char          targets[TARGETS][96];
    int           ntargets;
    int           exporting;
} logs_t;

static logs_t *g_open;              /* the page shown (one at a time) */

/* ---- filters -------------------------------------------------------------------------------- */

/* "YYYY-MM-DD[ HH:MM]" local; 0 when empty, -1 when not a time. */
static long long parse_when(const char *s, int end)
{
    while (*s == ' ') ++s;
    if (!*s) return 0;
    int y, mo, d, h = 0, mi = 0;
    int got = sscanf(s, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi);
    if (got < 3 || mo < 1 || mo > 12 || d < 1 || d > 31) return -1;
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = got >= 4 ? h : end ? 23 : 0;
    tm.tm_min = got >= 5 ? mi : end ? 59 : 0;
    tm.tm_sec = end ? 59 : 0;
    tm.tm_isdst = -1;
    return (long long)mktime(&tm);
}

static int filter_of(logs_t *l, ui_log_filter_t *f)
{
    memset(f, 0, sizeof *f);
    f->ch = -1;
    f->level = -1;
    time_t now = ui_model()->now ? ui_model()->now() : time(NULL);
    int r = (int)lv_dropdown_get_selected(l->range);
    if (r == R_TODAY) {
        struct tm tm;
        localtime_r(&now, &tm);
        tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
        tm.tm_isdst = -1;
        f->from = (long long)mktime(&tm);
    } else if (r == R_24H) {
        f->from = (long long)now - 86400;
    } else if (r == R_7D) {
        f->from = (long long)now - 7 * 86400;
    } else if (r == R_CUSTOM) {
        f->from = parse_when(lv_textarea_get_text(l->from), 0);
        f->to = parse_when(lv_textarea_get_text(l->to), 1);
        if (f->from < 0 || f->to < 0) return -1;
    }
    int t = (int)lv_dropdown_get_selected(l->type), third = (int)lv_dropdown_get_selected(l->third);
    if (l->kind == UI_LOG_SYSTEM) {
        f->kernel = t == 1;
        f->level = third >= 0 && third < (int)(sizeof k_levels / sizeof k_levels[0]) ? k_levels[third] : -1;
    } else {
        if (t > 0 && t <= l->ntypes) snprintf(f->type, sizeof f->type, "%s", l->types[t - 1]);
        if (l->kind == UI_LOG_ALARMS) f->ch = third > 0 ? third : -1;
        else if (third > 0 && third <= (int)(sizeof k_sources / sizeof k_sources[0]))
            snprintf(f->source, sizeof f->source, "%s", k_sources[third - 1]);
    }
    snprintf(f->text, sizeof f->text, "%s", lv_textarea_get_text(l->search));
    return 0;
}

/* ---- the table ------------------------------------------------------------------------------- */

static const char *level_word(int lv)
{
    static const char *const w[8] = { "emerg", "alert", "crit", "error", "warning", "notice", "info", "debug" };
    return lv >= 0 && lv < 8 ? w[lv] : "-";
}

static void when_text(long long t, char *out, size_t cap)
{
    if (!t) {
        snprintf(out, cap, "-");
        return;
    }
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    ui_format_datetime(&tm, 1, out, cap);
}

static void set_cell(lv_obj_t *c, const char *text, lv_color_t color)
{
    if (strcmp(lv_label_get_text(c), text)) lv_label_set_text(c, text);
    lv_obj_set_style_text_color(c, color, 0);
}

static void fill_row(logs_t *l, int i)
{
    const ui_log_row_t *r = &l->rows[i];
    char v[COLS][300];
    lv_color_t col[COLS];
    for (int c = 0; c < COLS; ++c) {
        v[c][0] = 0;
        col[c] = UI_C(TEXT);
    }
    when_text(r->time, v[0], sizeof v[0]);
    col[0] = UI_C(TEXT2);
    if (l->kind == UI_LOG_SYSTEM) {
        snprintf(v[1], sizeof v[1], "%s", level_word(r->level));
        col[1] = r->level >= 0 && r->level <= 3 ? UI_C(BAD) : r->level == 4 ? UI_C(WARN) : UI_C(TEXT2);
        snprintf(v[2], sizeof v[2], "%s", r->who[0] ? r->who : "-");
        snprintf(v[3], sizeof v[3], "%s", r->detail);
    } else if (l->kind == UI_LOG_ALARMS) {
        if (r->ch > 0) ui_chan_title(r->ch - 1, v[1], sizeof v[1]);
        else snprintf(v[1], sizeof v[1], "System");
        snprintf(v[2], sizeof v[2], "%s", r->type);
        snprintf(v[3], sizeof v[3], "%s", r->active == 1 ? "start" : r->active == 0 ? "stop" : "-");
        col[3] = r->active == 1 ? UI_C(BAD) : r->active == 0 ? UI_C(OK) : UI_C(TEXT2);
        col[2] = r->active == 0 ? UI_C(TEXT2) : UI_C(TEXT);
        snprintf(v[4], sizeof v[4], "%s", r->detail);
    } else {
        snprintf(v[1], sizeof v[1], "%s", r->who[0] ? r->who : "-");
        snprintf(v[2], sizeof v[2], "%s", r->source);
        col[2] = UI_C(TEXT2);
        snprintf(v[3], sizeof v[3], "%s", r->type);
        col[3] = !strcmp(r->type, "login_failed") || !strcmp(r->type, "lockout") ? UI_C(WARN) : UI_C(TEXT);
        snprintf(v[4], sizeof v[4], "%s", r->detail);
    }
    for (int c = 0; c < COLS; ++c)
        if (l->cell[i][c]) set_cell(l->cell[i][c], v[c], col[c]);
}

static void fill(logs_t *l)
{
    const ui_logs_src_t *src = g_src;
    ui_log_filter_t f;
    char why[160] = "";
    int n = -1;
    l->more = 0;
    if (filter_of(l, &f) < 0) {
        snprintf(why, sizeof why, "From and to are dates: YYYY-MM-DD, with a time if you like (YYYY-MM-DD HH:MM).");
    } else if (src && src->query) {
        n = src->query(l->kind, &f, l->offset, PAGE, l->rows, &l->more, why, sizeof why);
    } else {
        snprintf(why, sizeof why, "No logs in this build.");
    }
    if (n < 0) n = 0;
    else if (src && src->count && l->offset == 0) l->seen = src->count(l->kind);
    l->n = n;
    for (int i = 0; i < PAGE; ++i) {
        if (i < n) {
            fill_row(l, i);
            lv_obj_remove_flag(l->row[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(l->row[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (why[0]) lv_label_set_text(l->err, why);
    else if (!n) lv_label_set_text(l->err, l->offset ? "No older entries." : "No entries match.");
    if (why[0] || !n) lv_obj_remove_flag(l->err, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(l->err, LV_OBJ_FLAG_HIDDEN);
    char pos[64];
    if (n) snprintf(pos, sizeof pos, "%d - %d%s", l->offset + 1, l->offset + n, l->more ? "" : " (the oldest)");
    else snprintf(pos, sizeof pos, "-");
    lv_label_set_text(l->pos, pos);
    if (l->offset > 0) lv_obj_remove_state(l->newer, LV_STATE_DISABLED);
    else lv_obj_add_state(l->newer, LV_STATE_DISABLED);
    if (l->more) lv_obj_remove_state(l->older, LV_STATE_DISABLED);
    else lv_obj_add_state(l->older, LV_STATE_DISABLED);
    if (src && src->where) {
        char w[200];
        src->where(l->kind, w, sizeof w);
        lv_label_set_text(l->where, w);
    }
}

static void refill_first(logs_t *l)
{
    l->offset = 0;
    fill(l);
    lv_obj_scroll_to_y(lv_obj_get_parent(lv_obj_get_parent(l->table)), 0, LV_ANIM_OFF);
}

/* ---- events --------------------------------------------------------------------------------- */

static void filter_cb(lv_event_t *e)
{
    logs_t *l = lv_event_get_user_data(e);
    if (lv_event_get_target(e) == l->range) {
        if (lv_dropdown_get_selected(l->range) == R_CUSTOM) lv_obj_remove_flag(l->custom, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(l->custom, LV_OBJ_FLAG_HIDDEN);
    }
    refill_first(l);
}

static void debounce_cb(lv_timer_t *t)
{
    logs_t *l = lv_timer_get_user_data(t);
    lv_timer_pause(t);
    refill_first(l);
}

static void search_cb(lv_event_t *e)
{
    logs_t *l = lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_READY) {
        lv_timer_pause(l->debounce);
        refill_first(l);
        return;
    }
    lv_timer_reset(l->debounce);
    lv_timer_resume(l->debounce);
}

static void page_cb(lv_event_t *e)
{
    logs_t *l = lv_event_get_user_data(e);
    int older = lv_event_get_target(e) == l->older;
    if (older && !l->more) return;
    if (!older && l->offset == 0) return;
    l->offset = older ? l->offset + PAGE : l->offset > PAGE ? l->offset - PAGE : 0;
    fill(l);
    lv_obj_scroll_to_y(lv_obj_get_parent(lv_obj_get_parent(l->table)), 0, LV_ANIM_OFF);
}

static void refresh_cb(lv_event_t *e) { fill(lv_event_get_user_data(e)); }

static void tick_cb(lv_timer_t *t)
{
    logs_t *l = lv_timer_get_user_data(t);
    if (l->exporting && g_src && g_src->export_poll) {
        int done = 0;
        long count = 0;
        char msg[200] = "";
        if (!g_src->export_poll(&done, &count, msg, sizeof msg) && done) {
            l->exporting = 0;
            if (count >= 0) ui_toast(UI_TOAST_OK, "%ld entr%s exported to %s", count, count == 1 ? "y" : "ies", msg);
            else ui_toast(UI_TOAST_BAD, "Export failed: %s", msg);
        }
    }
    if (l->offset != 0) return;
    if (l->kind == UI_LOG_SYSTEM) {
        if (lv_obj_has_state(l->follow, LV_STATE_CHECKED)) fill(l);
    } else if (g_src && g_src->count && g_src->count(l->kind) != l->seen) {
        fill(l);
    }
}

/* ---- an entry, whole ------------------------------------------------------------------------- */

static void close_cb(lv_event_t *e)
{
    (void)e;
    ui_popup_close();
}

static void info_line(lv_obj_t *panel, const char *k, const char *v)
{
    lv_obj_t *r = lv_obj_create(panel);
    ui_plain(r);
    ui_inert(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(r, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r, VG_SPACE_MD, 0);
    lv_obj_t *a = ui_label(r, k, VG_FONT_SM_LV, UI_C(TEXT2));
    lv_obj_set_width(a, 110);
    lv_obj_t *b = ui_label(r, v, VG_FONT_MD_LV, UI_C(TEXT));
    lv_obj_set_flex_grow(b, 1);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
}

static void row_cb(lv_event_t *e)
{
    logs_t *l = lv_event_get_user_data(e);
    lv_obj_t *row = lv_event_get_current_target(e);
    int i = 0;
    while (i < PAGE && l->row[i] != row) ++i;
    if (i >= l->n) return;
    const ui_log_row_t *r = &l->rows[i];
    lv_obj_t *panel = ui_popup_open(NULL, 720);
    lv_obj_set_style_pad_all(panel, VG_SPACE_XL, 0);
    lv_obj_set_style_pad_row(panel, VG_SPACE_SM, 0);
    lv_obj_set_style_border_color(panel, UI_C(ACCENT), 0);
    char t[64], v[300];
    when_text(r->time, t, sizeof t);
    ui_label(panel, l->kind == UI_LOG_SYSTEM ? "System log" : l->kind == UI_LOG_ALARMS ? "Alarm" : "Operation",
             VG_FONT_LG_LV, UI_C(TEXT));
    info_line(panel, "Time", t);
    if (l->kind == UI_LOG_SYSTEM) {
        info_line(panel, "Level", level_word(r->level));
        info_line(panel, "From", r->who[0] ? r->who : "-");
        info_line(panel, "Message", r->detail);
    } else if (l->kind == UI_LOG_ALARMS) {
        if (r->ch > 0) ui_chan_title(r->ch - 1, v, sizeof v);
        else snprintf(v, sizeof v, "System");
        info_line(panel, "Channel", v);
        snprintf(v, sizeof v, "%s%s", r->type, r->active == 1 ? ", start" : r->active == 0 ? ", stop" : "");
        info_line(panel, "Type", v);
        info_line(panel, "Detail", r->detail[0] ? r->detail : "-");
    } else {
        snprintf(v, sizeof v, "%s (%s)", r->who[0] ? r->who : "-", r->source);
        info_line(panel, "User", v);
        info_line(panel, "Action", r->type);
        info_line(panel, "Detail", r->detail[0] ? r->detail : "-");
    }
    lv_obj_t *btns = lv_obj_create(panel);
    ui_plain(btns);
    ui_ring_room(btns);
    lv_obj_set_size(btns, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(btns, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *ok = ui_btn(btns, LV_SYMBOL_CLOSE, "Close");
    lv_obj_add_event_cb(ok, close_cb, LV_EVENT_CLICKED, NULL);
    ui_popup_focus(btns, ok, LV_GRIDNAV_CTRL_NONE);
    ui_popup_place(NULL);
}

/* ---- export ---------------------------------------------------------------------------------- */

static void export_go_cb(lv_event_t *e)
{
    logs_t *l = g_open;
    intptr_t code = (intptr_t)lv_event_get_user_data(e);
    ui_popup_close();
    if (!l || !g_src || !g_src->export_start) return;
    int t = (int)(code / 2), csv = !(code % 2);
    if (t < 0 || t >= l->ntargets) return;
    ui_log_filter_t f;
    if (filter_of(l, &f) < 0) {
        ui_toast(UI_TOAST_BAD, "From and to are not dates (YYYY-MM-DD)");
        return;
    }
    char err[200] = "";
    if (g_src->export_start(l->kind, &f, csv, l->targets[t], err, sizeof err) < 0) {
        ui_toast(UI_TOAST_BAD, "Export: %s", err);
        return;
    }
    l->exporting = 1;
    ui_toast(UI_TOAST_OK, "Exporting to %s ...", l->targets[t]);
}

static void export_cb(lv_event_t *e)
{
    logs_t *l = lv_event_get_user_data(e);
    if (l->exporting) {
        ui_toast(UI_TOAST_WARN, "An export is under way");
        return;
    }
    l->ntargets = g_src && g_src->export_targets ? g_src->export_targets(l->targets, TARGETS) : 0;
    if (!l->ntargets) {
        ui_toast(UI_TOAST_WARN, "No disk or USB stick to export to: plug one in");
        return;
    }
    lv_obj_t *panel = ui_popup_open(NULL, 640);
    lv_obj_set_style_pad_all(panel, VG_SPACE_XL, 0);
    lv_obj_set_style_pad_row(panel, VG_SPACE_MD, 0);
    lv_obj_set_style_border_color(panel, UI_C(ACCENT), 0);
    ui_label(panel, "Export", VG_FONT_LG_LV, UI_C(TEXT));
    lv_obj_t *note = ui_label(panel, "The entries the filters show (every page), oldest last, into a new file "
                                     "on the disk chosen: CSV for a spreadsheet, or text.",
                              VG_FONT_SM_LV, UI_C(TEXT2));
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_t *items = lv_obj_create(panel);
    ui_plain(items);
    ui_ring_room(items);
    lv_obj_set_size(items, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(items, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(items, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(items, VG_SPACE_SM, 0);
    lv_obj_set_style_pad_column(items, VG_SPACE_SM, 0);
    lv_obj_t *first = NULL;
    for (int t = 0; t < l->ntargets; ++t)
        for (int csv = 1; csv >= 0; --csv) {
            char text[140];
            snprintf(text, sizeof text, "%s  %s", csv ? "CSV" : "Text", l->targets[t]);
            lv_obj_t *b = ui_btn(items, LV_SYMBOL_SAVE, text);
            lv_obj_set_width(b, LV_PCT(48));
            lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_label_set_long_mode(ui_btn_label(b), LV_LABEL_LONG_DOT);
            lv_obj_set_flex_grow(ui_btn_label(b), 1);
            lv_obj_add_event_cb(b, export_go_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(t * 2 + !csv));
            if (!first) first = b;
        }
    lv_obj_t *cancel = ui_btn(items, LV_SYMBOL_CLOSE, "Cancel");
    lv_obj_add_event_cb(cancel, close_cb, LV_EVENT_CLICKED, NULL);
    ui_popup_focus(items, first, LV_GRIDNAV_CTRL_ROLLOVER);
    ui_popup_place(NULL);
}

/* ---- the page -------------------------------------------------------------------------------- */

static lv_obj_t *table_row(lv_obj_t *table, int kind, int header)
{
    lv_obj_t *r = header ? lv_obj_create(table) : lv_button_create(table);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(r, header ? VG_SPACE_SM : 3, 0);
    lv_obj_set_style_pad_hor(r, VG_SPACE_SM, 0);
    lv_obj_set_style_pad_column(r, VG_SPACE_MD, 0);
    lv_obj_set_style_border_color(r, UI_C(LINE), 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_layout(r, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (header) {
        ui_inert(r);
    } else {
        lv_obj_set_style_bg_color(r, UI_C(HOVER), LV_STATE_HOVERED);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_HOVERED);
        lv_obj_add_style(r, &ui_st_focus, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(r, UI_C(HOVER), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_FOCUSED);
    }
    (void)kind;
    return r;
}

static lv_obj_t *cell(lv_obj_t *row, const char *text, int w, int header)
{
    lv_obj_t *c = ui_label(row, text, VG_FONT_SM_LV, header ? UI_C(TEXT2) : UI_C(TEXT));
    if (w > 0) lv_obj_set_width(c, w);
    else {
        lv_obj_set_width(c, 0);
        lv_obj_set_flex_grow(c, 1);
    }
    /* One line, cut with "...": a label wraps unless its height is set. */
    lv_obj_set_height(c, lv_font_get_line_height(VG_FONT_SM_LV));
    lv_label_set_long_mode(c, LV_LABEL_LONG_DOT);
    return c;
}

static lv_obj_t *labelled(lv_obj_t *bar, const char *label)
{
    lv_obj_t *box = ui_set_hbox(bar);
    ui_label(box, label, VG_FONT_SM_LV, UI_C(TEXT2));
    return box;
}

static void *logs_open(ui_set_t *s, lv_obj_t *pane, int kind)
{
    ui_set_hide_actions(s);
    logs_t *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->s = s;
    l->kind = kind;
    g_open = l;
    lv_obj_set_style_pad_row(pane, VG_SPACE_MD, 0);

    /* Filters. */
    lv_obj_t *bar = lv_obj_create(pane);
    ui_plain(bar);
    ui_ring_room(bar);
    lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(bar, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, VG_SPACE_LG, 0);
    lv_obj_set_style_pad_row(bar, VG_SPACE_SM, 0);
    l->range = ui_set_dropdown(s, labelled(bar, "Time"), k_ranges, kind == UI_LOG_SYSTEM ? R_ALL : R_7D, 170);
    char opts[TYPES_MAX * 33] = "";
    if (kind == UI_LOG_SYSTEM) {
        snprintf(opts, sizeof opts, "System (syslog)\nKernel (dmesg)");
    } else {
        l->ntypes = g_src && g_src->types ? g_src->types(kind, l->types, TYPES_MAX) : 0;
        snprintf(opts, sizeof opts, "%s", kind == UI_LOG_ALARMS ? "All types" : "All actions");
        for (int i = 0; i < l->ntypes; ++i) {
            size_t n = strlen(opts);
            snprintf(opts + n, sizeof opts - n, "\n%s", l->types[i]);
        }
    }
    l->type = ui_set_dropdown(s, labelled(bar, kind == UI_LOG_SYSTEM ? "Log" : kind == UI_LOG_ALARMS ? "Type" : "Action"),
                              opts, 0, kind == UI_LOG_SYSTEM ? 170 : 190);
    if (kind == UI_LOG_ALARMS) {
        snprintf(opts, sizeof opts, "All");
        int nch = ui_model()->channels;
        for (int c = 0; c < nch && c < 64; ++c) {
            char t[40];
            ui_chan_title(c, t, sizeof t);
            size_t n = strlen(opts);
            snprintf(opts + n, sizeof opts - n, "\n%d  %s", c + 1, t);
        }
    } else if (kind == UI_LOG_OPS) {
        snprintf(opts, sizeof opts, "All");
        for (size_t i = 0; i < sizeof k_sources / sizeof k_sources[0]; ++i) {
            size_t n = strlen(opts);
            snprintf(opts + n, sizeof opts - n, "\n%s", k_sources[i]);
        }
    } else {
        snprintf(opts, sizeof opts, "All\nErrors\nWarnings and worse\nNotices and worse\nInfo and worse");
    }
    l->third = ui_set_dropdown(s, labelled(bar, kind == UI_LOG_ALARMS ? "Channel" : kind == UI_LOG_OPS ? "From" : "Level"),
                               opts, 0, kind == UI_LOG_SYSTEM ? 190 : 150);
    l->search = ui_set_text(s, labelled(bar, "Search"), "", 60, 180);
    lv_textarea_set_placeholder_text(l->search, kind == UI_LOG_SYSTEM ? "text or from" : "text, user, address");
    lv_obj_add_event_cb(l->range, filter_cb, LV_EVENT_VALUE_CHANGED, l);
    lv_obj_add_event_cb(l->type, filter_cb, LV_EVENT_VALUE_CHANGED, l);
    lv_obj_add_event_cb(l->third, filter_cb, LV_EVENT_VALUE_CHANGED, l);
    lv_obj_add_event_cb(l->search, search_cb, LV_EVENT_VALUE_CHANGED, l);
    lv_obj_add_event_cb(l->search, search_cb, LV_EVENT_READY, l);
    l->debounce = lv_timer_create(debounce_cb, SEARCH_MS, l);
    lv_timer_pause(l->debounce);

    /* From ... to ..., shown with that range. */
    l->custom = ui_set_hbox(pane);
    ui_label(l->custom, "From", VG_FONT_SM_LV, UI_C(TEXT2));
    l->from = ui_set_text(s, l->custom, "", 16, 190);
    lv_textarea_set_placeholder_text(l->from, "YYYY-MM-DD HH:MM");
    ui_label(l->custom, "to", VG_FONT_SM_LV, UI_C(TEXT2));
    l->to = ui_set_text(s, l->custom, "", 16, 190);
    lv_textarea_set_placeholder_text(l->to, "YYYY-MM-DD HH:MM");
    lv_obj_t *apply = ui_set_button(s, l->custom, LV_SYMBOL_OK, "Show");
    lv_obj_add_event_cb(apply, refresh_cb, LV_EVENT_CLICKED, l);
    lv_obj_add_flag(l->custom, LV_OBJ_FLAG_HIDDEN);

    /* The pager and the actions. */
    lv_obj_t *nav = lv_obj_create(pane);
    ui_plain(nav);
    ui_ring_room(nav);
    lv_obj_set_size(nav, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(nav, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(nav, VG_SPACE_SM, 0);
    l->newer = ui_set_button(s, nav, LV_SYMBOL_LEFT, "Newer");
    lv_obj_add_event_cb(l->newer, page_cb, LV_EVENT_CLICKED, l);
    l->pos = ui_label(nav, "", VG_FONT_SM_LV, UI_C(TEXT2));
    ui_tnum(l->pos);
    lv_obj_set_style_min_width(l->pos, 150, 0);
    lv_obj_set_style_text_align(l->pos, LV_TEXT_ALIGN_CENTER, 0);
    l->older = ui_set_button(s, nav, NULL, "Older " LV_SYMBOL_RIGHT);
    lv_obj_add_event_cb(l->older, page_cb, LV_EVENT_CLICKED, l);
    lv_obj_t *gap = lv_obj_create(nav);
    ui_plain(gap);
    ui_inert(gap);
    lv_obj_set_size(gap, 1, 1);
    lv_obj_set_flex_grow(gap, 1);
    if (kind == UI_LOG_SYSTEM) {
        lv_obj_t *fb = ui_set_hbox(nav);
        ui_label(fb, "Follow", VG_FONT_SM_LV, UI_C(TEXT2));
        l->follow = ui_set_switch(s, fb, 0);
    }
    lv_obj_t *refresh = ui_set_button(s, nav, LV_SYMBOL_REFRESH, "Refresh");
    lv_obj_add_event_cb(refresh, refresh_cb, LV_EVENT_CLICKED, l);
    lv_obj_t *exp = ui_set_button(s, nav, LV_SYMBOL_SAVE, "Export");
    lv_obj_add_event_cb(exp, export_cb, LV_EVENT_CLICKED, l);

    /* The table: a header, PAGE rows made once. */
    lv_obj_t *panel = ui_set_section(pane, NULL);
    lv_obj_set_style_pad_hor(panel, VG_SPACE_SM, 0);
    lv_obj_set_style_pad_row(panel, 0, 0);
    const cols_t *cols = &k_cols[kind];
    lv_obj_t *head = table_row(panel, kind, 1);
    for (int c = 0; c < COLS && cols->head[c]; ++c) cell(head, cols->head[c], cols->w[c], 1);
    l->table = lv_obj_create(panel);
    ui_plain(l->table);
    ui_ring_room(l->table);
    lv_obj_set_size(l->table, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(l->table, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(l->table, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < PAGE; ++i) {
        lv_obj_t *r = table_row(l->table, kind, 0);
        for (int c = 0; c < COLS && cols->head[c]; ++c) l->cell[i][c] = cell(r, "", cols->w[c], 0);
        lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, l);
        lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
        l->row[i] = r;
    }
    ui_gridnav(l->table, LV_GRIDNAV_CTRL_SCROLL_FIRST);
    lv_group_add_obj(ui_set_group(s), l->table);
    l->err = ui_set_note(panel, "", UI_C(TEXT2));
    lv_obj_set_style_pad_ver(l->err, VG_SPACE_MD, 0);
    l->where = ui_set_note(pane, "", UI_C(TEXT2));

    fill(l);
    l->tick = lv_timer_create(tick_cb, TICK_MS, l);
    return l;
}

static void *sys_open(ui_set_t *s, lv_obj_t *pane) { return logs_open(s, pane, UI_LOG_SYSTEM); }
static void *alarms_open(ui_set_t *s, lv_obj_t *pane) { return logs_open(s, pane, UI_LOG_ALARMS); }
static void *ops_open(ui_set_t *s, lv_obj_t *pane) { return logs_open(s, pane, UI_LOG_OPS); }

static void logs_close(void *st)
{
    logs_t *l = st;
    if (!l) return;
    if (g_open == l) g_open = NULL;
    lv_timer_delete(l->tick);
    lv_timer_delete(l->debounce);
    free(l);
}

static const ui_set_item_t k_items[UI_LOG_KINDS] = {
    [UI_LOG_SYSTEM] = { "System log", &ui_icon_nav_disks, sys_open, NULL, NULL, NULL, logs_close },
    [UI_LOG_ALARMS] = { "Alarm log", &ui_icon_nav_alarm, alarms_open, NULL, NULL, NULL, logs_close },
    [UI_LOG_OPS] = { "Operation log", &ui_icon_nav_users, ops_open, NULL, NULL, NULL, logs_close },
};

void ui_logs_open(int first)
{
    ui_settings_open("Logs", k_items, UI_LOG_KINDS, first);
}
