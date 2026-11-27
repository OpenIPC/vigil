/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_set_display.c — Display: the output resolution, and what the live
 * view draws over the video (the clock, the camera names, the status
 * icons), from vigil.yaml's `display` section (docs/ui.md).
 *
 * Output: the board's modes (ui_model_t.out_modes). Save switches to the
 * one chosen at once and asks "Keep this resolution?"; without an answer
 * within KEEP_SECS, or with Undo, Esc or a click outside, the old mode
 * comes back, so a monitor that cannot show the new one gets its picture
 * back by itself. Only a kept mode is saved (display.output).
 *
 * Clock, Camera names, Status icons, Video: drop-downs of display.* (and
 * the clock's format, ui.date_format and ui.time_format, which System ->
 * General also sets); the live view follows a save at once. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "icons.h"
#include "ui_i18n.h"
#include "ui_settings.h"
#include "ui_theme.h"

#define KEEP_SECS 15

/* ---- saving fields of an object section ------------------------------------------- */

#define SEC_FIELDS_MAX 16

/* Section `key` with the fields in `keys` set to `vals` (n of them), the
 * others as stored. 0, or -1 with a toast. */
static int save_fields(const char *key, const char *const *keys, const char *const *vals, int n)
{
    int sec = ui_set_section_index(key);
    if (sec < 0) return -1;
    const ui_section_t *u = &ui_model()->sections[sec];
    char buf[SEC_FIELDS_MAX][64];
    const char *v[SEC_FIELDS_MAX];
    unsigned gen = ui_model()->config_generation ? ui_model()->config_generation() : 0;
    for (int f = 0; f < u->n_fields && f < SEC_FIELDS_MAX; ++f) {
        ui_model()->get_value(sec, -1, f, buf[f], sizeof buf[f]);
        for (int k = 0; k < n; ++k)
            if (!strcmp(u->fields[f].key, keys[k])) snprintf(buf[f], sizeof buf[f], "%s", vals[k]);
        v[f] = buf[f];
    }
    ui_errors_t errs;
    memset(&errs, 0, sizeof errs);
    ui_save_rc_t rc = ui_model()->save(sec, 1, v, NULL, gen, &errs);
    if (rc == UI_SAVE_OK || rc == UI_SAVE_UNSYNCED) return 0;
    ui_toast(UI_TOAST_BAD, "Not saved: %s", errs.n ? errs.e[0].msg : "the settings changed meanwhile");
    return -1;
}

static void stored(const char *sec_key, const char *key, char *out, size_t cap)
{
    out[0] = 0;
    int sec = ui_set_section_index(sec_key);
    if (sec < 0) return;
    const ui_section_t *u = &ui_model()->sections[sec];
    for (int f = 0; f < u->n_fields; ++f)
        if (!strcmp(u->fields[f].key, key)) ui_model()->get_value(sec, -1, f, out, cap);
}

static int dd_sel(lv_obj_t *d) { return (int)lv_dropdown_get_selected(d); }

/* ---- Output --------------------------------------------------------------------------- */

typedef struct {
    ui_set_t *s;
    ui_mode_t modes[UI_MODES_MAX];
    int       n, cur;                   /* modes[cur] is in force (-1: none of them) */
    lv_obj_t *dd, *now;
} out_t;

static out_t *g_out;                    /* the sub-page while it is open */

/* The question outlives the sub-page: the user may not get to see either. */
static struct {
    ui_mode_t   old, now;
    uint32_t    since;
    lv_timer_t *timer;
    lv_obj_t   *label;
} g_keep;

static void mode_text(const ui_mode_t *m, char *out, size_t cap)
{
    snprintf(out, cap, "%d x %d, %d Hz", m->w, m->h, m->hz);
}

static int same_mode(const ui_mode_t *a, const ui_mode_t *b)
{
    return a->w == b->w && a->h == b->h && a->hz == b->hz;
}

static void out_load(out_t *o)
{
    ui_mode_t cur;
    ui_model()->out_mode(&cur);
    o->cur = -1;
    for (int i = 0; i < o->n; ++i)
        if (same_mode(&o->modes[i], &cur)) o->cur = i;
    if (o->dd && o->cur >= 0) lv_dropdown_set_selected(o->dd, (uint32_t)o->cur);
    if (o->now) {
        char t[48];
        mode_text(&cur, t, sizeof t);
        lv_label_set_text(o->now, cur.w ? t : "-");
    }
}

static void keep_end(int keep)
{
    if (!g_keep.timer) return;
    lv_timer_delete(g_keep.timer);
    g_keep.timer = NULL;
    lv_obj_t *label = g_keep.label;
    g_keep.label = NULL;
    char t[48];
    if (keep) {
        char key[24];
        snprintf(key, sizeof key, "%dx%d@%d", g_keep.now.w, g_keep.now.h, g_keep.now.hz);
        const char *k = "output", *v = key;
        mode_text(&g_keep.now, t, sizeof t);
        if (save_fields("display", &k, &v, 1) == 0) ui_toast(UI_TOAST_OK, "Resolution %s kept", t);
    } else {
        mode_text(&g_keep.old, t, sizeof t);
        if (ui_model()->out_set(&g_keep.old) < 0) ui_toast(UI_TOAST_BAD, "Could not go back to %s", t);
        else ui_toast(UI_TOAST_WARN, "Back to %s", t);
    }
    if (label) ui_popup_close();
    if (g_out) out_load(g_out);
}

static void keep_tick(lv_timer_t *t)
{
    (void)t;
    int left = KEEP_SECS - (int)(lv_tick_elaps(g_keep.since) / 1000);
    if (left <= 0) {
        keep_end(0);
        return;
    }
    if (g_keep.label) lv_label_set_text_fmt(g_keep.label, "Going back to the old resolution in %d s.", left);
}

static void keep_cb(lv_event_t *e) { keep_end((int)(intptr_t)lv_event_get_user_data(e)); }

/* The popup went without an answer (Esc, the right button, a click
 * outside it, a logout): the old mode, now. */
static void keep_gone_cb(lv_event_t *e)
{
    (void)e;
    g_keep.label = NULL;
    if (g_keep.timer) keep_end(0);
}

static void keep_ask(void)
{
    lv_obj_t *p = ui_popup_open(NULL, 560);
    if (!p) return;
    lv_obj_set_style_pad_all(p, VG_SPACE_LG, 0);
    lv_obj_set_style_pad_row(p, VG_SPACE_MD, 0);
    lv_obj_set_style_border_color(p, UI_C(ACCENT), 0);
    char t[48], q[80];
    mode_text(&g_keep.now, t, sizeof t);
    snprintf(q, sizeof q, "Keep this resolution (%s)?", t);
    ui_label(p, q, VG_FONT_LG_LV, UI_C(TEXT));
    g_keep.label = ui_label(p, "", VG_FONT_MD_LV, UI_C(TEXT2));
    lv_obj_t *row = ui_set_hbox(p);
    lv_obj_t *undo = ui_btn(row, LV_SYMBOL_REFRESH, TR(S_UNDO));
    lv_obj_add_event_cb(undo, keep_cb, LV_EVENT_CLICKED, (void *)0);
    lv_obj_t *keep = ui_btn(row, LV_SYMBOL_OK, TR(S_KEEP));
    ui_btn_accent(keep);
    lv_obj_add_event_cb(keep, keep_cb, LV_EVENT_CLICKED, (void *)1);
    lv_obj_add_event_cb(p, keep_gone_cb, LV_EVENT_DELETE, NULL);
    ui_popup_place(NULL);
    /* Enter at once keeps nothing: the user has to see the picture to keep it. */
    ui_popup_focus(row, undo, LV_GRIDNAV_CTRL_NONE);
    keep_tick(NULL);
}

static void *out_open(ui_set_t *s, lv_obj_t *pane)
{
    const ui_model_t *m = ui_model();
    out_t *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->s = s;
    o->n = m->out_modes && m->out_mode && m->out_set ? m->out_modes(o->modes, UI_MODES_MAX) : 0;
    lv_obj_t *sect = ui_set_section(pane, "Output");
    ui_mode_t cur = { 0, 0, 0 };
    if (m->out_mode) m->out_mode(&cur);
    char t[48];
    mode_text(&cur, t, sizeof t);
    o->now = ui_set_info(sect, "In use", cur.w ? t : "-");
    if (o->n > 1) {
        char opts[UI_MODES_MAX * 24] = "";
        for (int i = 0; i < o->n; ++i) {
            mode_text(&o->modes[i], t, sizeof t);
            size_t l = strlen(opts);
            snprintf(opts + l, sizeof opts - l, "%s%s", i ? "\n" : "", t);
        }
        lv_obj_t *box = ui_set_row(sect, "Resolution",
                                   "HDMI and VGA. After Save the new one has to be kept within 15 s, or the "
                                   "old one comes back by itself.");
        o->dd = ui_set_dropdown(s, box, opts, 0, 320);
        ui_set_note(pane,
                    "The menus keep their layout and are scaled to the output, stretched on a 4:3 or "
                    "5:4 monitor; the video is shown at the output's own resolution.",
                    UI_C(TEXT2));
    } else {
        ui_set_hide_actions(s);
        ui_set_note(pane, o->n ? "This board's output has one resolution." : "This board has no display output.",
                    UI_C(TEXT2));
    }
    g_out = o;
    out_load(o);
    return o;
}

static int out_dirty(void *st)
{
    out_t *o = st;
    return o && o->dd && dd_sel(o->dd) != o->cur;
}

static int out_save(void *st)
{
    out_t *o = st;
    if (!o || !o->dd || g_keep.timer) return -1;
    int sel = dd_sel(o->dd);
    if (sel == o->cur || sel < 0 || sel >= o->n) return 0;
    ui_model()->out_mode(&g_keep.old);
    g_keep.now = o->modes[sel];
    char t[48];
    mode_text(&g_keep.now, t, sizeof t);
    if (ui_model()->out_set(&g_keep.now) < 0) {
        ui_toast(UI_TOAST_BAD, "The output did not take %s", t);
        out_load(o);
        return -1;
    }
    g_keep.since = lv_tick_get();
    g_keep.timer = lv_timer_create(keep_tick, 250, NULL);
    out_load(o);
    keep_ask();
    return 0;
}

static void out_cancel(void *st)
{
    if (st) out_load(st);
}

static void out_close(void *st)
{
    if (g_out == st) g_out = NULL;
    free(st);
}

/* ---- Clock, Camera names, Status icons: drop-downs of display.* and ui.* ------------- */

typedef struct {
    const char        *sec, *key;
    const char        *label, *help;
    const char *const *values;          /* as stored */
    const char *const *names;           /* as shown */
    int                n;
} drow_t;

#define DROWS_MAX 6

typedef struct {
    const drow_t *rows;
    int           n;
    lv_obj_t     *dd[DROWS_MAX];
    int           sel0[DROWS_MAX];      /* what is stored */
} dpage_t;

static const char *const k_pos_names[UI_N_POS] = { "Off", "Top left", "Top centre", "Top right", "Bottom left",
                                                   "Bottom centre", "Bottom right" };
static const char *const k_corner_keys[] = { "off", "top-left", "top-right", "bottom-left", "bottom-right" };
static const char *const k_corner_names[] = { "Off", "Top left", "Top right", "Bottom left", "Bottom right" };
static const char *const k_size_names[UI_N_SIZES] = { "Small", "Medium", "Large", "Extra large" };
static const char *const k_bg_names[UI_N_BGS] = { "Outline", "None", "Box" };
static const char *const k_datefmts[] = { "YYYY-MM-DD", "DD.MM.YYYY", "MM/DD/YYYY" };
static const char *const k_timefmts[] = { "24h", "12h" };
static const char *const k_timefmt_names[] = { "24-hour", "12-hour (AM/PM)" };

#define BG_HELP "Outline: a thin dark edge around the text, readable on any picture. None: the text alone. " \
                "Box: on a dark box."
#define SIZE_HELP "In the UI's font, Montserrat (the only one built in)."

static const drow_t k_clock_rows[] = {
    { "display", "clock_position", "Position", "Off hides it. The navigation bar has its own.", ui_pos_keys,
      k_pos_names, UI_N_POS },
    { "display", "clock_size", "Size", SIZE_HELP, ui_size_keys, k_size_names, UI_N_SIZES },
    { "display", "clock_background", "Background", BG_HELP, ui_bg_keys, k_bg_names, UI_N_BGS },
    { "ui", "date_format", "Date", "Also in System, General: the UI's dates everywhere.", k_datefmts, k_datefmts, 3 },
    { "ui", "time_format", "Time", NULL, k_timefmts, k_timefmt_names, 2 },
};
static const drow_t k_name_rows[] = {
    { "display", "name_position", "Position", "The corner of each tile; Off: no names.", k_corner_keys,
      k_corner_names, 5 },
    { "display", "name_size", "Size", SIZE_HELP " A size smaller in small tiles.", ui_size_keys, k_size_names,
      UI_N_SIZES },
    { "display", "name_background", "Background", BG_HELP, ui_bg_keys, k_bg_names, UI_N_BGS },
};
/* REC and the events are always shown: a corner, no Off. */
static const drow_t k_icon_rows[] = {
    { "display", "icons_position", "Position",
      "The corner of each tile with REC and the camera's events; in the names' corner they go just above "
      "the name (below it at the top).",
      k_corner_keys + 1, k_corner_names + 1, 4 },
    { "display", "icons_background", "Background", BG_HELP, ui_bg_keys, k_bg_names, UI_N_BGS },
};

/* Two values: the drop-down needs no third. */
static const char *const k_aspect_keys[] = { "fit", "stretch" };
static const char *const k_aspect_names[] = { "Fit (keep its shape)", "Stretch to the tile" };
static const drow_t k_video_rows[] = {
    { "display", "video_aspect", "Video aspect",
      "Fit: each camera's picture keeps its shape in its tile and full screen, with black bars where "
      "the shapes differ; a sub-stream is shown at its main stream's shape. Stretch: the picture fills "
      "the tile, whatever its shape.",
      k_aspect_keys, k_aspect_names, 2 },
};

static void dpage_load(dpage_t *d)
{
    for (int i = 0; i < d->n; ++i) {
        char v[32];
        stored(d->rows[i].sec, d->rows[i].key, v, sizeof v);
        d->sel0[i] = 0;
        for (int k = 0; k < d->rows[i].n; ++k)
            if (!strcmp(d->rows[i].values[k], v)) d->sel0[i] = k;
        lv_dropdown_set_selected(d->dd[i], (uint32_t)d->sel0[i]);
    }
}

static void *dpage_open(ui_set_t *s, lv_obj_t *pane, const char *title, const drow_t *rows, int n)
{
    dpage_t *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->rows = rows;
    d->n = n < DROWS_MAX ? n : DROWS_MAX;
    lv_obj_t *sect = ui_set_section(pane, title);
    for (int i = 0; i < d->n; ++i) {
        char opts[256] = "";
        for (int k = 0; k < rows[i].n; ++k) {
            size_t l = strlen(opts);
            snprintf(opts + l, sizeof opts - l, "%s%s", k ? "\n" : "", rows[i].names[k]);
        }
        if (i && strcmp(rows[i].sec, rows[i - 1].sec)) sect = ui_set_section(pane, "Format");
        lv_obj_t *box = ui_set_row(sect, rows[i].label, rows[i].help);
        d->dd[i] = ui_set_dropdown(s, box, opts, 0, 320);
    }
    dpage_load(d);
    return d;
}

static int dpage_dirty(void *st)
{
    dpage_t *d = st;
    for (int i = 0; d && i < d->n; ++i)
        if (dd_sel(d->dd[i]) != d->sel0[i]) return 1;
    return 0;
}

/* Each section the page shows, on its own: the display's rows, then the
 * formats. */
static int dpage_save(void *st)
{
    dpage_t *d = st;
    if (!d) return 0;
    int failed = 0;
    for (int i = 0; i < d->n; ++i) {
        if (i && !strcmp(d->rows[i].sec, d->rows[i - 1].sec)) continue;
        const char *keys[DROWS_MAX], *vals[DROWS_MAX];
        int k = 0, changed = 0;
        for (int j = i; j < d->n && !strcmp(d->rows[j].sec, d->rows[i].sec); ++j) {
            keys[k] = d->rows[j].key;
            vals[k++] = d->rows[j].values[dd_sel(d->dd[j])];
            changed |= dd_sel(d->dd[j]) != d->sel0[j];
        }
        if (changed && save_fields(d->rows[i].sec, keys, vals, k) < 0) failed = 1;
    }
    if (failed) return -1;
    ui_toast(UI_TOAST_OK, "%s", TR(S_SAVED));
    dpage_load(d);
    return 0;
}

static void dpage_cancel(void *st)
{
    if (st) dpage_load(st);
}

static void dpage_close(void *st) { free(st); }

#define N(a) (int)(sizeof(a) / sizeof((a)[0]))
static void *clock_open(ui_set_t *s, lv_obj_t *pane)
{
    return dpage_open(s, pane, "Clock on the live view", k_clock_rows, N(k_clock_rows));
}
static void *names_open(ui_set_t *s, lv_obj_t *pane)
{
    return dpage_open(s, pane, "Camera names on the tiles", k_name_rows, N(k_name_rows));
}
static void *icons_open(ui_set_t *s, lv_obj_t *pane)
{
    return dpage_open(s, pane, "Status icons on the tiles", k_icon_rows, N(k_icon_rows));
}
static void *video_open(ui_set_t *s, lv_obj_t *pane)
{
    return dpage_open(s, pane, "Camera pictures", k_video_rows, N(k_video_rows));
}

static const ui_set_item_t g_items[] = {
    { "Output", &ui_icon_nav_output, out_open, out_dirty, out_save, out_cancel, out_close },
    { "Clock", &ui_icon_nav_clock, clock_open, dpage_dirty, dpage_save, dpage_cancel, dpage_close },
    { "Camera names", &ui_icon_nav_names, names_open, dpage_dirty, dpage_save, dpage_cancel, dpage_close },
    { "Status icons", &ui_icon_nav_icons, icons_open, dpage_dirty, dpage_save, dpage_cancel, dpage_close },
    { "Video", &ui_icon_nav_aspect, video_open, dpage_dirty, dpage_save, dpage_cancel, dpage_close },
};

void ui_display_open(void) { ui_display_open_at(UI_DISPLAY_OUTPUT); }

void ui_display_open_at(int first)
{
    ui_settings_open("Display", g_items, N(g_items), first);
}
