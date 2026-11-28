/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_set_storage.c — Storage: Basic, Schedule, Disk Manager, Disk
 * Maintenance (ui_set_maint.c), Disk Quota, Network Storage
 * (docs/management.md).
 *
 *   Basic            record.on_full, the segment length in minutes (the
 *                    file keeps seconds), the recording directory with the
 *                    mounted disks as shortcuts, allow_rootfs
 *   Schedule         a week per camera, half hours, painted on one grid
 *                    object (drawn, not 336 widgets: the LVGL heap is 1 MiB)
 *   Disk Manager     the disks and partitions, Format (the name typed to
 *                    confirm), Mount, Unmount, a surface scan under way;
 *                    refreshed every 2 s
 *   Disk Quota       per camera: the most gigabytes and days
 *   Network Storage  what the kernel can mount, the nas form, Test, and
 *                    "Record here"
 *
 * Settings go through model->save like every form: validated, saved
 * atomically, applied by the main loop. Disk work is a job of the
 * management layer, followed on a timer. */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "icons.h"
#include "ui_i18n.h"
#include "ui_settings.h"
#include "ui_theme.h"

#define VAL_MAX   512
#define FIELDS    16
#define CAMS      16
#define DISKS     24
#define REFRESH_MS 2000

static const ui_mgmt_t *MG(void) { return ui_model()->mgmt; }
static int managed(void) { return MG() && MG()->managed && MG()->managed(); }

/* ---- the model's settings, as text ------------------------------------------------ */

static int field_of(int sec, const char *key)
{
    const ui_section_t *s = &ui_model()->sections[sec];
    for (int i = 0; i < s->n_fields; ++i)
        if (!strcmp(s->fields[i].key, key)) return i;
    return -1;
}

static unsigned generation(void)
{
    return ui_model()->config_generation ? ui_model()->config_generation() : 0;
}

/* An array or object section's items as text: n_items x n_fields. */
typedef struct {
    int    sec, n_fields, n_items;
    char (*v)[VAL_MAX];             /* n_items * n_fields */
} rows_t;

static int rows_load(rows_t *r, const char *key)
{
    memset(r, 0, sizeof *r);
    r->sec = ui_set_section_index(key);
    if (r->sec < 0) return -1;
    const ui_section_t *s = &ui_model()->sections[r->sec];
    r->n_fields = s->n_fields;
    r->n_items = s->is_array ? ui_model()->item_count(r->sec) : 1;
    r->v = calloc((size_t)(r->n_items ? r->n_items : 1) * (size_t)r->n_fields, VAL_MAX);
    if (!r->v) return -1;
    for (int k = 0; k < r->n_items; ++k)
        for (int f = 0; f < r->n_fields; ++f)
            ui_model()->get_value(r->sec, s->is_array ? k : -1, f, r->v[k * r->n_fields + f], VAL_MAX);
    return 0;
}

static void rows_free(rows_t *r)
{
    free(r->v);
    r->v = NULL;
}

static char *cell(rows_t *r, int item, const char *key)
{
    int f = field_of(r->sec, key);
    return f < 0 ? NULL : r->v[item * r->n_fields + f];
}

/* Save `n` items of `vals` (n x n_fields strings) into section `sec`;
 * a toast says how it went. 0 when saved. */
static int save_rows(int sec, int n, char (*vals)[VAL_MAX], unsigned gen, ui_errors_t *errs)
{
    const ui_section_t *s = &ui_model()->sections[sec];
    const char **p = calloc((size_t)(n ? n : 1) * (size_t)s->n_fields, sizeof *p);
    if (!p) return -1;
    for (int i = 0; i < n * s->n_fields; ++i) p[i] = vals[i];
    memset(errs, 0, sizeof *errs);
    ui_save_rc_t rc = ui_model()->save(sec, n, p, NULL, gen, errs);
    free(p);
    switch (rc) {
    case UI_SAVE_OK:
        ui_toast(UI_TOAST_OK, "%s", TR(S_SAVED));
        return 0;
    case UI_SAVE_UNSYNCED:
        ui_toast(UI_TOAST_WARN, "%s", errs->n ? errs->e[0].msg : "Saved, but may not survive a power cut");
        return 0;
    case UI_SAVE_STALE:
        ui_toast(UI_TOAST_WARN, "Not saved: the settings were changed elsewhere meanwhile. Showing them now.");
        return -1;
    default:
        ui_toast(UI_TOAST_BAD, "Not saved: %s", errs->n ? errs->e[0].msg : "error");
        return -1;
    }
}

/* The channels that have a camera, 1-based, with their names. */
static int cameras(int *ch, char names[][32], int max)
{
    rows_t r;
    if (rows_load(&r, "cameras") < 0) return 0;
    int n = 0;
    for (int i = 0; i < r.n_items && n < max; ++i) {
        const char *url = cell(&r, i, "url"), *name = cell(&r, i, "name");
        if (!url || !url[0]) continue;
        ch[n] = i + 1;
        if (name && name[0]) snprintf(names[n], 32, "%s", name);
        else snprintf(names[n], 32, "CAM %d", i + 1);
        ++n;
    }
    rows_free(&r);
    return n;
}

/* ---- Basic ------------------------------------------------------------------------ */

typedef struct {
    ui_set_t *s;
    rows_t    rec;
    unsigned  gen;
    int       seg_s, seg_min0, onfull0, rootfs0;
    char      path0[VAL_MAX];
    lv_obj_t *onfull, *seg, *path, *rootfs, *state, *pool;
    lv_timer_t *timer;
} basic_t;

static void rec_state_text(lv_obj_t *l);

/* The pool (docs/recording.md): size, chunks, oldest video, a rebuild. */
static void pool_text(basic_t *b)
{
    char text[512] = "";
    int n = MG() && MG()->rec_pool ? MG()->rec_pool(text, sizeof text) : 0;
    if (n) lv_label_set_text(b->pool, text);
    if (n) lv_obj_remove_flag(b->pool, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(b->pool, LV_OBJ_FLAG_HIDDEN);
}

static void basic_tick(lv_timer_t *t)
{
    basic_t *b = lv_timer_get_user_data(t);
    rec_state_text(b->state);
    pool_text(b);
}

static void rec_state_text(lv_obj_t *l)
{
    const char *states[UI_MAX_CHANNELS];
    int counts[UI_MAX_CHANNELS], n = 0;
    for (int ch = 0; ch < ui_model()->channels && ch < UI_MAX_CHANNELS; ++ch) {
        const ui_chan_t *st = ui_channel(ch);
        if (!st->configured || !st->rec_state[0]) continue;
        int k = 0;
        while (k < n && strcmp(states[k], st->rec_state)) ++k;
        if (k == n) {
            states[n] = st->rec_state;
            counts[n++] = 0;
        }
        ++counts[k];
    }
    char text[256] = "";
    for (int k = 0; k < n; ++k) {
        size_t len = strlen(text);
        snprintf(text + len, sizeof text - len, "%s%c%s: %d camera%s.", k ? "  " : "",
                 toupper((unsigned char)states[k][0]), states[k] + 1, counts[k], counts[k] == 1 ? "" : "s");
    }
    lv_label_set_text_fmt(l, LV_SYMBOL_DRIVE "  %s", n ? text : "Not recording: no directory set.");
}

static void path_pick_cb(lv_event_t *e)
{
    basic_t *b = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);
    lv_textarea_set_text(b->path, lv_label_get_text(ui_btn_label(btn)));
}

static void basic_fill(basic_t *b)
{
    lv_dropdown_set_selected(b->onfull, (uint32_t)b->onfull0);
    char m[16];
    snprintf(m, sizeof m, "%d", b->seg_min0);
    lv_textarea_set_text(b->seg, m);
    lv_textarea_set_text(b->path, b->path0);
    lv_obj_remove_state(b->path, LV_STATE_USER_1);
    if (b->rootfs0) lv_obj_add_state(b->rootfs, LV_STATE_CHECKED);
    else lv_obj_remove_state(b->rootfs, LV_STATE_CHECKED);
}

static int basic_load(basic_t *b)
{
    rows_free(&b->rec);
    b->gen = generation();
    if (rows_load(&b->rec, "record") < 0) return -1;
    const char *of = cell(&b->rec, 0, "on_full"), *seg = cell(&b->rec, 0, "segment");
    const char *path = cell(&b->rec, 0, "path"), *rf = cell(&b->rec, 0, "allow_rootfs");
    b->onfull0 = of && !strcmp(of, "stop");
    b->seg_s = seg ? atoi(seg) : 60;
    b->seg_min0 = (b->seg_s + 30) / 60;
    if (b->seg_min0 < 1) b->seg_min0 = 1;
    if (b->seg_min0 > 60) b->seg_min0 = 60;
    snprintf(b->path0, sizeof b->path0, "%s", path ? path : "");
    b->rootfs0 = rf && !strcmp(rf, "1");
    return 0;
}

static void *basic_open(ui_set_t *s, lv_obj_t *pane)
{
    basic_t *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->s = s;
    if (basic_load(b) < 0) {
        free(b);
        ui_set_hide_actions(s);
        ui_set_note(pane, "This build has no recording settings.", UI_C(WARN));
        return NULL;
    }
    b->state = ui_label(pane, "", VG_FONT_MD_LV, UI_C(TEXT));
    lv_obj_set_width(b->state, LV_PCT(100));
    lv_label_set_long_mode(b->state, LV_LABEL_LONG_WRAP);
    rec_state_text(b->state);
    b->pool = ui_label(pane, "", VG_FONT_SM_LV, UI_C(TEXT2));
    lv_obj_set_width(b->pool, LV_PCT(100));
    lv_label_set_long_mode(b->pool, LV_LABEL_LONG_WRAP);
    pool_text(b);
    b->timer = lv_timer_create(basic_tick, REFRESH_MS, b);

    lv_obj_t *sec = ui_set_section(pane, NULL);
    b->onfull = ui_set_dropdown(s, ui_set_row(sec, "When the disk is full",
                                              "Overwrite: the oldest recordings, whichever camera made them, "
                                              "make room. Stop: nothing is deleted; recording stops with the "
                                              "event \"REC disk full\" until there is room again."),
                                "Overwrite\nStop", 0, 240);
    char help[160] = "Local inputs only (cameras go into the pool): a new file starts at the first keyframe "
                     "after this long.";
    if (b->seg_s % 60)
        snprintf(help, sizeof help, "Local inputs only (cameras go into the pool): a new file starts at the first "
                                    "keyframe after this long. Stored: %d s.",
                 b->seg_s);
    b->seg = ui_set_number(s, ui_set_row(sec, "Segment length (min)", help), b->seg_min0, 1, 60);

    lv_obj_t *pbox = ui_set_row(sec, "Recording directory",
                                "Recording goes only onto a disk: with none mounted where the path leads, it "
                                "waits. Empty: no recording.");
    b->path = ui_set_text(s, pbox, b->path0, 191, 0);
    /* The mounted disks and the network storage as shortcuts. */
    ui_disk_t *d = calloc(DISKS, sizeof *d);
    int nd = d && MG() && MG()->disks ? MG()->disks(d, DISKS) : 0;
    for (int i = 0; i < nd; ++i) {
        if (!d[i].mnt[0] || d[i].system || d[i].ro) continue;
        char p[160];
        snprintf(p, sizeof p, "%.120s/rec", d[i].mnt);
        lv_obj_t *bt = ui_set_button(s, pbox, NULL, p);
        lv_obj_add_event_cb(bt, path_pick_cb, LV_EVENT_CLICKED, b);
    }
    free(d);
    if (MG() && MG()->nas_mounted && MG()->nas_mounted()) {
        lv_obj_t *bt = ui_set_button(s, pbox, NULL, "/mnt/nas/rec");
        lv_obj_add_event_cb(bt, path_pick_cb, LV_EVENT_CLICKED, b);
    }
    b->rootfs = ui_set_switch(s, ui_set_row(sec, "Record onto the root filesystem",
                                            "Off: never onto the root filesystem (on a board, its flash). On "
                                            "only where the root filesystem is itself a disk."),
                              0);
    basic_fill(b);
    return b;
}

static int basic_dirty(void *st)
{
    basic_t *b = st;
    if (!b) return 0;
    return (int)lv_dropdown_get_selected(b->onfull) != b->onfull0 || ui_set_number_get(b->seg) != b->seg_min0 ||
           strcmp(lv_textarea_get_text(b->path), b->path0) ||
           lv_obj_has_state(b->rootfs, LV_STATE_CHECKED) != b->rootfs0;
}

static int basic_save(void *st)
{
    basic_t *b = st;
    if (!b) return 0;
    char(*v)[VAL_MAX] = calloc((size_t)b->rec.n_fields, VAL_MAX);
    if (!v) return -1;
    memcpy(v, b->rec.v, (size_t)b->rec.n_fields * VAL_MAX);
    int f;
    if ((f = field_of(b->rec.sec, "on_full")) >= 0)
        snprintf(v[f], VAL_MAX, "%s", lv_dropdown_get_selected(b->onfull) ? "stop" : "overwrite");
    /* Minutes only when changed: a stored 90 s is not rounded by a save
     * of something else. */
    int min = ui_set_number_get(b->seg);
    if ((f = field_of(b->rec.sec, "segment")) >= 0 && min != b->seg_min0) snprintf(v[f], VAL_MAX, "%d", min * 60);
    if ((f = field_of(b->rec.sec, "path")) >= 0) snprintf(v[f], VAL_MAX, "%s", lv_textarea_get_text(b->path));
    if ((f = field_of(b->rec.sec, "allow_rootfs")) >= 0)
        snprintf(v[f], VAL_MAX, "%d", lv_obj_has_state(b->rootfs, LV_STATE_CHECKED) ? 1 : 0);
    ui_errors_t errs;
    int rc = save_rows(b->rec.sec, 1, v, b->gen, &errs);
    free(v);
    if (rc < 0) {
        int pf = field_of(b->rec.sec, "path");
        for (int i = 0; i < errs.n; ++i)
            if (errs.e[i].field == pf) lv_obj_add_state(b->path, LV_STATE_USER_1);
        if (errs.n) return -1;
    }
    /* Saved, or stale: what is stored now. */
    basic_load(b);
    basic_fill(b);
    rec_state_text(b->state);
    return rc;
}

static void basic_cancel(void *st)
{
    basic_t *b = st;
    if (!b) return;
    basic_load(b);
    basic_fill(b);
}

static void basic_close(void *st)
{
    basic_t *b = st;
    if (!b) return;
    if (b->timer) lv_timer_delete(b->timer);
    rows_free(&b->rec);
    free(b);
}

/* ---- Schedule ---------------------------------------------------------------------- */

#define SLOTS   48
#define CELL_W  18
#define CELL_H  26
#define DAY_W   52
#define HEAD_H  22

static const char *const k_days[7] = { "mon", "tue", "wed", "thu", "fri", "sat", "sun" };
static const char *const k_day_names[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };

typedef struct {
    ui_set_t *s;
    int       sec;
    unsigned  gen;
    int       n, cur;
    int       ch[CAMS];
    char      names[CAMS][32];
    char      week[CAMS][7][SLOTS + 1];
    char      stored[CAMS][7][SLOTS + 1];
    char      brush;
    int       cx, cy;               /* the keyboard's cell */
    lv_obj_t *cam, *grid, *brush_btn[3];
} sched_t;

static const char k_brushes[3] = { 'C', 'M', '-' };

static lv_color_t slot_color(char c)
{
    return c == 'C' ? UI_C(OK) : c == 'M' ? UI_C(WARN) : UI_C(PANEL2);
}

static void grid_draw_cb(lv_event_t *e)
{
    sched_t *sc = lv_event_get_user_data(e);
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = VG_FONT_SM_LV;
    ld.color = UI_C(TEXT2);
    char t[8];
    for (int h = 0; h <= 24; h += 2) {
        snprintf(t, sizeof t, "%d", h);
        ld.text = t;
        int x = a.x1 + DAY_W + h * 2 * CELL_W;
        lv_area_t la = { x - 12, a.y1, x + 12, a.y1 + HEAD_H - 4 };
        ld.align = LV_TEXT_ALIGN_CENTER;
        lv_draw_label(layer, &ld, &la);
    }
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.radius = 0;
    for (int d = 0; d < 7; ++d) {
        int y = a.y1 + HEAD_H + d * CELL_H;
        ld.text = k_day_names[d];
        ld.align = LV_TEXT_ALIGN_LEFT;
        ld.color = d == sc->cy ? UI_C(TEXT) : UI_C(TEXT2);
        lv_area_t la = { a.x1, y + 4, a.x1 + DAY_W - 4, y + CELL_H - 2 };
        lv_draw_label(layer, &ld, &la);
        for (int k = 0; k < SLOTS; ++k) {
            int x = a.x1 + DAY_W + k * CELL_W;
            /* A gap between hours, a thinner one between half hours. */
            lv_area_t ca = { x + (k % 2 ? 0 : 1), y + 1, x + CELL_W - 1, y + CELL_H - 2 };
            rd.bg_color = slot_color(sc->week[sc->cur][d][k]);
            rd.bg_opa = LV_OPA_COVER;
            rd.border_width = 0;
            lv_draw_rect(layer, &rd, &ca);
        }
    }
    /* The keyboard's cell, while the grid has the focus. */
    if (lv_obj_has_state(o, LV_STATE_FOCUSED) && ui_keyboard_in_use()) {
        int x = a.x1 + DAY_W + sc->cx * CELL_W, y = a.y1 + HEAD_H + sc->cy * CELL_H;
        lv_area_t ca = { x - 1, y - 1, x + CELL_W, y + CELL_H - 1 };
        rd.bg_opa = LV_OPA_TRANSP;
        rd.border_color = UI_C(FOCUS);
        rd.border_width = 2;
        rd.border_opa = LV_OPA_COVER;
        lv_draw_rect(layer, &rd, &ca);
    }
}

static void paint(sched_t *sc, int d, int k)
{
    if (d < 0 || d > 6 || k < 0 || k >= SLOTS) return;
    sc->cx = k;
    sc->cy = d;
    sc->week[sc->cur][d][k] = sc->brush;
    lv_obj_invalidate(sc->grid);
}

static void grid_press_cb(lv_event_t *e)
{
    sched_t *sc = lv_event_get_user_data(e);
    lv_indev_t *in = lv_indev_active();
    if (!in || lv_indev_get_type(in) != LV_INDEV_TYPE_POINTER) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    lv_area_t a;
    lv_obj_get_coords(sc->grid, &a);
    int x = p.x - a.x1 - DAY_W, y = p.y - a.y1 - HEAD_H;
    if (x < 0 || y < 0) return;
    paint(sc, y / CELL_H, x / CELL_W);
}

static void grid_key_cb(lv_event_t *e)
{
    sched_t *sc = lv_event_get_user_data(e);
    uint32_t k = lv_event_get_key(e);
    switch (k) {
    case LV_KEY_LEFT: sc->cx = sc->cx > 0 ? sc->cx - 1 : SLOTS - 1; break;
    case LV_KEY_RIGHT: sc->cx = (sc->cx + 1) % SLOTS; break;
    case LV_KEY_UP: sc->cy = sc->cy > 0 ? sc->cy - 1 : 6; break;
    case LV_KEY_DOWN: sc->cy = (sc->cy + 1) % 7; break;
    case LV_KEY_ENTER:
    case ' ':
        paint(sc, sc->cy, sc->cx);
        break;
    default:
        return;
    }
    lv_obj_invalidate(sc->grid);
    lv_event_stop_processing(e);
}

static void grid_focus_cb(lv_event_t *e)
{
    sched_t *sc = lv_event_get_user_data(e);
    lv_obj_invalidate(sc->grid);
}

static void brush_set(sched_t *sc, int i)
{
    sc->brush = k_brushes[i];
    for (int k = 0; k < 3; ++k) {
        if (k == i) lv_obj_add_state(sc->brush_btn[k], LV_STATE_CHECKED);
        else lv_obj_remove_state(sc->brush_btn[k], LV_STATE_CHECKED);
    }
}

static void brush_cb(lv_event_t *e)
{
    sched_t *sc = lv_event_get_user_data(e);
    lv_obj_t *b = lv_event_get_target(e);
    for (int i = 0; i < 3; ++i)
        if (sc->brush_btn[i] == b) brush_set(sc, i);
}

static void cam_cb(lv_event_t *e)
{
    sched_t *sc = lv_event_get_user_data(e);
    sc->cur = (int)lv_dropdown_get_selected(sc->cam);
    lv_obj_invalidate(sc->grid);
}

enum { ACT_COPY_DAY, ACT_COPY_CAMS, ACT_ALWAYS, ACT_NEVER };

static void act_cb(lv_event_t *e)
{
    sched_t *sc = lv_event_get_user_data(e);
    int act = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    char(*w)[SLOTS + 1] = sc->week[sc->cur];
    switch (act) {
    case ACT_COPY_DAY:
        for (int d = 0; d < 7; ++d)
            if (d != sc->cy) memcpy(w[d], w[sc->cy], SLOTS + 1);
        ui_toast(UI_TOAST_OK, "%s copied to every day", k_day_names[sc->cy]);
        break;
    case ACT_COPY_CAMS:
        for (int c = 0; c < sc->n; ++c)
            if (c != sc->cur) memcpy(sc->week[c], w, sizeof sc->week[c]);
        ui_toast(UI_TOAST_OK, "This week copied to every camera");
        break;
    case ACT_ALWAYS:
    case ACT_NEVER:
        for (int d = 0; d < 7; ++d) {
            memset(w[d], act == ACT_ALWAYS ? 'C' : '-', SLOTS);
            w[d][SLOTS] = 0;
        }
        break;
    }
    lv_obj_invalidate(sc->grid);
}

static lv_obj_t *act_btn(sched_t *sc, lv_obj_t *row, const char *sym, const char *text, int act)
{
    lv_obj_t *b = ui_set_button(sc->s, row, sym, text);
    lv_obj_set_user_data(b, (void *)(intptr_t)act);
    lv_obj_add_event_cb(b, act_cb, LV_EVENT_CLICKED, sc);
    return b;
}

static void sched_load(sched_t *sc)
{
    sc->gen = generation();
    rows_t r;
    for (int c = 0; c < sc->n; ++c)
        for (int d = 0; d < 7; ++d) {
            memset(sc->week[c][d], 'C', SLOTS);
            sc->week[c][d][SLOTS] = 0;
        }
    if (rows_load(&r, "schedule") == 0) {
        for (int i = 0; i < r.n_items; ++i) {
            const char *cs = cell(&r, i, "camera");
            int ch = cs ? atoi(cs) : 0;
            for (int c = 0; c < sc->n; ++c) {
                if (sc->ch[c] != ch) continue;
                for (int d = 0; d < 7; ++d) {
                    const char *v = cell(&r, i, k_days[d]);
                    if (v && strlen(v) == SLOTS) memcpy(sc->week[c][d], v, SLOTS + 1);
                }
            }
        }
        rows_free(&r);
    }
    memcpy(sc->stored, sc->week, sizeof sc->stored);
}

static void legend(lv_obj_t *row, lv_color_t c, const char *text)
{
    lv_obj_t *sw = lv_obj_create(row);
    ui_plain(sw);
    lv_obj_set_size(sw, 16, 16);
    lv_obj_set_style_bg_color(sw, c, 0);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
    ui_label(row, text, VG_FONT_SM_LV, UI_C(TEXT2));
}

static void *sched_open(ui_set_t *s, lv_obj_t *pane)
{
    sched_t *sc = calloc(1, sizeof *sc);
    if (!sc) return NULL;
    sc->s = s;
    sc->sec = ui_set_section_index("schedule");
    sc->n = cameras(sc->ch, sc->names, CAMS);
    if (sc->sec < 0 || !sc->n) {
        ui_set_hide_actions(s);
        ui_set_note(pane, sc->sec < 0 ? "This build has no recording schedule."
                                      : "No cameras yet: a schedule is per camera.", UI_C(TEXT2));
        free(sc);
        return NULL;
    }
    sched_load(sc);
    sc->brush = 'C';

    lv_obj_t *top = ui_set_hbox(pane);
    ui_label(top, "Camera", VG_FONT_MD_LV, UI_C(TEXT));
    char opts[CAMS * 40] = "";
    for (int c = 0; c < sc->n; ++c) {
        size_t l = strlen(opts);
        snprintf(opts + l, sizeof opts - l, "%s%s", c ? "\n" : "", sc->names[c]);
    }
    sc->cam = ui_set_dropdown(s, top, opts, 0, 260);
    lv_obj_add_event_cb(sc->cam, cam_cb, LV_EVENT_VALUE_CHANGED, sc);
    lv_obj_t *sp = ui_label(top, "", VG_FONT_SM_LV, UI_C(TEXT2));
    lv_obj_set_width(sp, VG_SPACE_XL);
    static const char *const brush_names[3] = { "Continuous", "Motion only", "Off" };
    for (int i = 0; i < 3; ++i) {
        sc->brush_btn[i] = ui_set_button(s, top, NULL, brush_names[i]);
        lv_obj_add_event_cb(sc->brush_btn[i], brush_cb, LV_EVENT_CLICKED, sc);
    }
    brush_set(sc, 0);

    lv_obj_t *panel = ui_set_section(pane, NULL);
    sc->grid = lv_obj_create(panel);
    ui_plain(sc->grid);
    lv_obj_remove_flag(sc->grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sc->grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(sc->grid, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    /* Room right of the last half hour for the "24" above it. */
    lv_obj_set_size(sc->grid, DAY_W + SLOTS * CELL_W + 14, HEAD_H + 7 * CELL_H);
    lv_obj_add_style(sc->grid, &ui_st_focus, LV_STATE_FOCUSED);
    lv_obj_add_flag(sc->grid, UI_FLAG_ARROWS);
    lv_obj_add_event_cb(sc->grid, grid_draw_cb, LV_EVENT_DRAW_MAIN, sc);
    lv_obj_add_event_cb(sc->grid, grid_press_cb, LV_EVENT_PRESSED, sc);
    lv_obj_add_event_cb(sc->grid, grid_press_cb, LV_EVENT_PRESSING, sc);
    lv_obj_add_event_cb(sc->grid, grid_key_cb, LV_EVENT_KEY | LV_EVENT_PREPROCESS, sc);
    lv_obj_add_event_cb(sc->grid, grid_focus_cb, LV_EVENT_FOCUSED, sc);
    lv_obj_add_event_cb(sc->grid, grid_focus_cb, LV_EVENT_DEFOCUSED, sc);
    lv_group_add_obj(ui_set_group(s), sc->grid);
    lv_obj_t *leg = ui_set_hbox(panel);
    lv_obj_set_style_pad_column(leg, VG_SPACE_SM, 0);
    legend(leg, UI_C(OK), "Continuous   ");
    legend(leg, UI_C(WARN), "Motion only   ");
    legend(leg, UI_C(PANEL2), "Off");

    lv_obj_t *acts = ui_set_hbox(pane);
    act_btn(sc, acts, LV_SYMBOL_COPY, "Copy the day to all days", ACT_COPY_DAY);
    act_btn(sc, acts, LV_SYMBOL_COPY, "Copy to all cameras", ACT_COPY_CAMS);
    act_btn(sc, acts, NULL, "Always", ACT_ALWAYS);
    act_btn(sc, acts, NULL, "Never", ACT_NEVER);
    ui_set_note(pane, "Click or drag over the half hours to paint them with the chosen kind; with a "
                      "keyboard, the arrows move over the grid and Space paints. \"Copy the day\" copies the "
                      "day last painted.", UI_C(TEXT2));
    ui_set_note(pane, "Motion only records while the camera reports motion; no camera source reports motion "
                      "yet, so these hours are not recorded for now.", UI_C(WARN));
    return sc;
}

static int sched_dirty(void *st)
{
    sched_t *sc = st;
    return sc && memcmp(sc->week, sc->stored, sizeof sc->week) != 0;
}

static int all_on(char w[7][SLOTS + 1])
{
    for (int d = 0; d < 7; ++d)
        if (strspn(w[d], "C") != SLOTS) return 0;
    return 1;
}

static int sched_save(void *st)
{
    sched_t *sc = st;
    if (!sc) return 0;
    rows_t r;
    if (rows_load(&r, "schedule") < 0) return -1;
    int nf = r.n_fields, max = ui_model()->sections[sc->sec].max_items;
    char(*v)[VAL_MAX] = calloc((size_t)(max ? max : 1) * (size_t)nf, VAL_MAX);
    if (!v) {
        rows_free(&r);
        return -1;
    }
    int n = 0;
    /* Entries of channels not on this page (a camera removed since) stay. */
    for (int i = 0; i < r.n_items && n < max; ++i) {
        const char *cs = cell(&r, i, "camera");
        int ch = cs ? atoi(cs) : 0, shown = 0;
        for (int c = 0; c < sc->n; ++c) shown |= sc->ch[c] == ch;
        if (shown) continue;
        memcpy(v[n * nf], r.v[i * nf], (size_t)nf * VAL_MAX);
        ++n;
    }
    for (int c = 0; c < sc->n && n < max; ++c) {
        if (all_on(sc->week[c])) continue;          /* no entry: always on */
        for (int f = 0; f < nf; ++f) {
            const char *key = ui_model()->sections[sc->sec].fields[f].key;
            if (!strcmp(key, "camera")) snprintf(v[n * nf + f], VAL_MAX, "%d", sc->ch[c]);
            for (int d = 0; d < 7; ++d)
                if (!strcmp(key, k_days[d])) snprintf(v[n * nf + f], VAL_MAX, "%s", sc->week[c][d]);
        }
        ++n;
    }
    rows_free(&r);
    ui_errors_t errs;
    int rc = save_rows(sc->sec, n, v, sc->gen, &errs);
    free(v);
    if (rc == 0 || !errs.n) {
        int cur = sc->cur;
        sched_load(sc);
        sc->cur = cur;
        lv_obj_invalidate(sc->grid);
    }
    return rc;
}

static void sched_cancel(void *st)
{
    sched_t *sc = st;
    if (!sc) return;
    memcpy(sc->week, sc->stored, sizeof sc->week);
    lv_obj_invalidate(sc->grid);
}

static void free_st(void *st) { free(st); }

/* ---- Disk Manager ------------------------------------------------------------------- */

typedef struct {
    ui_set_t   *s;
    ui_disk_t   d[DISKS];
    int         n;
    lv_obj_t   *table, *job, *scan;
    lv_obj_t   *fmt, *fmt_title, *fmt_fs, *fmt_name, *fmt_go;
    char        target[32], scan_disk[32];
    lv_timer_t *timer;
} disks_t;

static void disks_build(disks_t *dk);

static int has_system_part(const disks_t *dk, const char *disk)
{
    for (int i = 0; i < dk->n; ++i)
        if (!strcmp(dk->d[i].parent, disk) && dk->d[i].system) return 1;
    return 0;
}

static int has_parts(const disks_t *dk, const char *disk)
{
    for (int i = 0; i < dk->n; ++i)
        if (!strcmp(dk->d[i].parent, disk)) return 1;
    return 0;
}

static void refresh(disks_t *dk, int force)
{
    ui_disk_t now[DISKS];
    memset(now, 0, sizeof now);
    int n = MG() && MG()->disks ? MG()->disks(now, DISKS) : 0;
    /* A surface scan under way: a line above the table, and its disk's
     * state says so. */
    char line[200], sd[32];
    ui_maint_scan_line(line, sizeof line, sd, sizeof sd);
    lv_label_set_text(dk->scan, line);
    if (line[0]) lv_obj_remove_flag(dk->scan, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(dk->scan, LV_OBJ_FLAG_HIDDEN);
    if (strcmp(sd, dk->scan_disk)) {
        snprintf(dk->scan_disk, sizeof dk->scan_disk, "%s", sd);
        force = 1;
    }
    if (!force && n == dk->n && !memcmp(now, dk->d, sizeof now[0] * (size_t)n)) return;
    memcpy(dk->d, now, sizeof now);
    dk->n = n;
    disks_build(dk);
}

static void disks_tick(lv_timer_t *t) { refresh(lv_timer_get_user_data(t), 0); }

static void disk_job_done(void *user, int ok, const char *msg)
{
    disks_t *dk = user;
    ui_toast(ok ? UI_TOAST_OK : UI_TOAST_BAD, "%s", msg && *msg ? msg : ok ? "Done" : "Failed");
    refresh(dk, 1);
}

static void fmt_hide(disks_t *dk)
{
    lv_obj_add_flag(dk->fmt, LV_OBJ_FLAG_HIDDEN);
    dk->target[0] = 0;
}

static void fmt_check(disks_t *dk)
{
    if (!strcmp(lv_textarea_get_text(dk->fmt_name), dk->target)) lv_obj_remove_state(dk->fmt_go, LV_STATE_DISABLED);
    else lv_obj_add_state(dk->fmt_go, LV_STATE_DISABLED);
}

static void fmt_name_cb(lv_event_t *e) { fmt_check(lv_event_get_user_data(e)); }

static void fmt_cancel_cb(lv_event_t *e) { fmt_hide(lv_event_get_user_data(e)); }

static void fmt_go_cb(lv_event_t *e)
{
    disks_t *dk = lv_event_get_user_data(e);
    char fs[16], err[200] = "";
    lv_dropdown_get_selected_str(dk->fmt_fs, fs, sizeof fs);
    int rc = MG()->disk_format(dk->target, fs, lv_textarea_get_text(dk->fmt_name), err, sizeof err);
    if (rc <= 0) {
        ui_toast(UI_TOAST_BAD, "Not formatted: %s", rc == UI_MGMT_EUNMANAGED ? "vigild does not manage this system"
                                                                           : err);
        return;
    }
    char t[64];
    snprintf(t, sizeof t, "%s", dk->target);
    fmt_hide(dk);
    lv_label_set_text_fmt(dk->job, LV_SYMBOL_REFRESH "  Formatting %s...", t);
    ui_set_follow_job(dk->s, rc, dk->job, disk_job_done, dk);
}

static void fmt_show(disks_t *dk, const char *name)
{
    snprintf(dk->target, sizeof dk->target, "%s", name);
    lv_label_set_text_fmt(dk->fmt_title, LV_SYMBOL_WARNING "  Format %s", name);
    lv_textarea_set_text(dk->fmt_name, "");
    lv_textarea_set_placeholder_text(dk->fmt_name, name);
    fmt_check(dk);
    lv_obj_remove_flag(dk->fmt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_view_recursive(dk->fmt, LV_ANIM_OFF);
    lv_group_focus_obj(dk->fmt_fs);
}

enum { DA_FORMAT, DA_MOUNT, DA_UNMOUNT };

static void disk_act_cb(lv_event_t *e)
{
    disks_t *dk = lv_event_get_user_data(e);
    lv_obj_t *b = lv_event_get_target(e);
    intptr_t code = (intptr_t)lv_obj_get_user_data(b);
    int i = (int)(code / 4), act = (int)(code % 4);
    if (i < 0 || i >= dk->n) return;
    const char *name = dk->d[i].name;
    if (act == DA_FORMAT) {
        fmt_show(dk, name);
        return;
    }
    char err[200] = "";
    int rc = act == DA_MOUNT ? MG()->disk_mount(name, err, sizeof err) : MG()->disk_unmount(name, err, sizeof err);
    if (rc <= 0) {
        ui_toast(UI_TOAST_BAD, "%s", rc == UI_MGMT_EUNMANAGED ? "vigild does not manage this system" : err);
        return;
    }
    lv_label_set_text_fmt(dk->job, LV_SYMBOL_REFRESH "  %s %s...", act == DA_MOUNT ? "Mounting" : "Unmounting",
                          name);
    ui_set_follow_job(dk->s, rc, dk->job, disk_job_done, dk);
}

static lv_obj_t *col(lv_obj_t *row, const char *text, int w, lv_color_t c)
{
    lv_obj_t *l = ui_label(row, text, VG_FONT_SM_LV, c);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    return l;
}

static lv_obj_t *trow(lv_obj_t *table)
{
    lv_obj_t *r = lv_obj_create(table);
    ui_plain(r);
    ui_ring_room(r);
    lv_obj_set_size(r, LV_PCT(100), 40);
    lv_obj_set_style_pad_hor(r, VG_SPACE_SM, 0);
    lv_obj_set_style_pad_column(r, VG_SPACE_SM, 0);
    lv_obj_set_style_border_color(r, UI_C(LINE), 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_layout(r, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return r;
}

static lv_color_t state_color(const char *st)
{
    if (!strcmp(st, "recording") || !strcmp(st, "mounted")) return UI_C(OK);
    if (!strcmp(st, "error") || !strcmp(st, "read-only")) return UI_C(BAD);
    if (!strcmp(st, "system")) return UI_C(TEXT2);
    return UI_C(WARN);
}

static void disks_build(disks_t *dk)
{
    /* The focused button, to give the focus back to its successor. */
    lv_obj_t *foc = lv_group_get_focused(ui_set_group(dk->s));
    intptr_t foc_code = -1;
    if (foc && lv_obj_get_parent(foc) && lv_obj_get_parent(lv_obj_get_parent(foc)) == dk->table)
        foc_code = (intptr_t)lv_obj_get_user_data(foc);
    lv_obj_clean(dk->table);
    lv_obj_t *h = trow(dk->table);
    lv_obj_set_height(h, 30);
    col(h, "Device", 110, UI_C(TEXT2));
    col(h, "Model / filesystem", 180, UI_C(TEXT2));
    col(h, "Size", 80, UI_C(TEXT2));
    col(h, "Free", 80, UI_C(TEXT2));
    col(h, "Mounted at", 110, UI_C(TEXT2));
    col(h, "State", 100, UI_C(TEXT2));
    if (!dk->n) {
        ui_set_note(dk->table, "No disk: plug in a USB disk, or connect a SATA one.", UI_C(TEXT2));
        return;
    }
    lv_obj_t *refocus = NULL;
    int ok = managed();
    for (int i = 0; i < dk->n; ++i) {
        const ui_disk_t *d = &dk->d[i];
        lv_obj_t *r = trow(dk->table);
        char name[48], what[96], size[24], avail[24];
        snprintf(name, sizeof name, "%s%.31s", d->is_part ? "    " : "", d->name);
        if (d->is_part || !d->model[0]) snprintf(what, sizeof what, "%s", d->fs[0] ? d->fs : "-");
        else snprintf(what, sizeof what, "%s%s", d->model, d->removable ? " (USB)" : "");
        ui_set_size_text(d->size, size, sizeof size);
        if (d->mnt[0]) ui_set_size_text(d->avail, avail, sizeof avail);
        else snprintf(avail, sizeof avail, "-");
        col(r, name, 110, UI_C(TEXT));
        col(r, what, 180, UI_C(TEXT));
        col(r, size, 80, UI_C(TEXT));
        col(r, avail, 80, UI_C(TEXT));
        col(r, d->mnt[0] ? d->mnt : "-", 110, UI_C(TEXT));
        const char *st = d->state[0] ? d->state : (has_parts(dk, d->name) ? "" : "-");
        if (!d->is_part && dk->scan_disk[0] && !strcmp(d->name, dk->scan_disk)) st = "scanning";
        col(r, st, 100, state_color(st));
        int whole_parts = !d->is_part && has_parts(dk, d->name);
        int can_format = !d->system && !(!d->is_part && has_system_part(dk, d->name));
        int can_mount = !d->system && !whole_parts && d->fs[0] && !d->mnt[0];
        int can_umount = !d->system && d->mnt[0];
        struct { int on, act; const char *sym, *text; } b[] = {
            { can_format, DA_FORMAT, NULL, TR(S_FORMAT) },
            { can_mount, DA_MOUNT, NULL, TR(S_MOUNT) },
            { can_umount, DA_UNMOUNT, NULL, TR(S_UNMOUNT) },
        };
        for (size_t k = 0; k < sizeof b / sizeof b[0]; ++k) {
            if (!b[k].on) continue;
            lv_obj_t *bt = ui_set_button(dk->s, r, b[k].sym, b[k].text);
            lv_obj_set_height(bt, 32);
            intptr_t code = (intptr_t)i * 4 + b[k].act;
            lv_obj_set_user_data(bt, (void *)code);
            lv_obj_add_event_cb(bt, disk_act_cb, LV_EVENT_CLICKED, dk);
            if (!ok) lv_obj_add_state(bt, LV_STATE_DISABLED);
            if (code == foc_code) refocus = bt;
        }
    }
    if (refocus) lv_group_focus_obj(refocus);
}

static void *disks_open(ui_set_t *s, lv_obj_t *pane)
{
    ui_set_hide_actions(s);
    if (!MG() || !MG()->disks) {
        ui_set_note(pane, "This build has no disk manager.", UI_C(WARN));
        return NULL;
    }
    disks_t *dk = calloc(1, sizeof *dk);
    if (!dk) return NULL;
    dk->s = s;
    if (!managed())
        ui_set_note(pane, LV_SYMBOL_WARNING "  vigild does not manage this system (it is not OpenIPC "
                                            "firmware): the disks are shown, not changed.", UI_C(WARN));
    dk->scan = ui_set_note(pane, "", UI_C(WARN));
    lv_obj_add_flag(dk->scan, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *panel = ui_set_section(pane, NULL);
    lv_obj_set_style_pad_hor(panel, VG_SPACE_SM, 0);
    dk->table = lv_obj_create(panel);
    ui_plain(dk->table);
    ui_ring_room(dk->table);
    lv_obj_set_size(dk->table, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(dk->table, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(dk->table, LV_FLEX_FLOW_COLUMN);
    dk->job = ui_set_note(pane, "", UI_C(TEXT2));

    /* The format question: in the pane, so the on-screen keyboard and
     * the keyboard's Tab reach its fields. */
    dk->fmt = ui_set_section(pane, NULL);
    lv_obj_set_style_border_color(dk->fmt, UI_C(BAD), 0);
    dk->fmt_title = ui_label(dk->fmt, "", VG_FONT_LG_LV, UI_C(BAD));
    ui_set_note(dk->fmt, "Everything on it is erased. Recording onto it pauses while it is formatted.",
                UI_C(TEXT));
    char fss[4][8], opts[48] = "";
    int nf = MG()->disk_formats ? MG()->disk_formats(fss, 4) : 0;
    for (int i = 0; i < nf; ++i) {
        size_t l = strlen(opts);
        snprintf(opts + l, sizeof opts - l, "%s%s", i ? "\n" : "", fss[i]);
    }
    dk->fmt_fs = ui_set_dropdown(s, ui_set_row(dk->fmt, "Filesystem",
                                               "ext4 (the default where the firmware can make it) keeps a "
                                               "journal: a power cut costs the open segment, not the disk. "
                                               "exFAT where a computer must read the disk; FAT32 for old "
                                               "devices."),
                                 nf ? opts : "exfat", 0, 200);
    lv_obj_t *nbox = ui_set_row(dk->fmt, "Type the name to confirm", NULL);
    dk->fmt_name = ui_set_text(s, nbox, "", 31, 200);
    lv_obj_add_event_cb(dk->fmt_name, fmt_name_cb, LV_EVENT_VALUE_CHANGED, dk);
    lv_obj_t *btns = ui_set_hbox(dk->fmt);
    lv_obj_t *cancel = ui_set_button(s, btns, LV_SYMBOL_CLOSE, TR(S_CANCEL));
    lv_obj_add_event_cb(cancel, fmt_cancel_cb, LV_EVENT_CLICKED, dk);
    dk->fmt_go = ui_set_button(s, btns, LV_SYMBOL_TRASH, TR(S_FORMAT));
    lv_obj_set_style_bg_color(dk->fmt_go, UI_C(BAD), 0);
    lv_obj_set_style_bg_opa(dk->fmt_go, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(dk->fmt_go, fmt_go_cb, LV_EVENT_CLICKED, dk);
    fmt_hide(dk);

    refresh(dk, 1);
    dk->timer = lv_timer_create(disks_tick, REFRESH_MS, dk);
    return dk;
}

static void disks_close(void *st)
{
    disks_t *dk = st;
    if (!dk) return;
    lv_timer_delete(dk->timer);
    free(dk);
}

/* ---- Disk Quota -------------------------------------------------------------------- */

typedef struct {
    ui_set_t *s;
    int       sec;
    unsigned  gen;
    int       n;
    int       ch[CAMS];
    char      names[CAMS][32];
    int       gb0[CAMS], days0[CAMS];
    lv_obj_t *gb[CAMS], *days[CAMS];
} quota_t;

static void quota_load(quota_t *q)
{
    q->gen = generation();
    memset(q->gb0, 0, sizeof q->gb0);
    memset(q->days0, 0, sizeof q->days0);
    rows_t r;
    if (rows_load(&r, "quota") < 0) return;
    for (int i = 0; i < r.n_items; ++i) {
        const char *cs = cell(&r, i, "camera"), *g = cell(&r, i, "max_gb"), *d = cell(&r, i, "max_days");
        int ch = cs ? atoi(cs) : 0;
        for (int c = 0; c < q->n; ++c)
            if (q->ch[c] == ch) {
                q->gb0[c] = g ? atoi(g) : 0;
                q->days0[c] = d ? atoi(d) : 0;
            }
    }
    rows_free(&r);
}

static void quota_fill(quota_t *q)
{
    for (int c = 0; c < q->n; ++c) {
        char b[16];
        snprintf(b, sizeof b, "%d", q->gb0[c]);
        lv_textarea_set_text(q->gb[c], b);
        snprintf(b, sizeof b, "%d", q->days0[c]);
        lv_textarea_set_text(q->days[c], b);
    }
}

static void *quota_open(ui_set_t *s, lv_obj_t *pane)
{
    quota_t *q = calloc(1, sizeof *q);
    if (!q) return NULL;
    q->s = s;
    q->sec = ui_set_section_index("quota");
    q->n = cameras(q->ch, q->names, CAMS);
    if (q->sec < 0 || !q->n) {
        ui_set_hide_actions(s);
        ui_set_note(pane, q->sec < 0 ? "This build has no disk quotas." : "No cameras yet: a quota is per camera.",
                    UI_C(TEXT2));
        free(q);
        return NULL;
    }
    quota_load(q);
    ui_set_note(pane, "The most space and the most days of recordings each camera may keep; beyond them its "
                      "oldest recordings are deleted (checked once a minute). 0: no limit; the disk's own "
                      "rule (Basic: when the disk is full) applies either way.", UI_C(TEXT2));
    lv_obj_t *sec = ui_set_section(pane, NULL);
    for (int c = 0; c < q->n; ++c) {
        lv_obj_t *box = ui_set_row(sec, q->names[c], NULL);
        /* A unit and its number wrap together. */
        lv_obj_t *h = ui_set_hbox(box);
        lv_obj_t *l = ui_label(h, "Max size (GB)", VG_FONT_SM_LV, UI_C(TEXT2));
        lv_obj_set_width(l, 100);
        q->gb[c] = ui_set_number(s, h, q->gb0[c], 0, 100000);
        h = ui_set_hbox(box);
        l = ui_label(h, "Max days", VG_FONT_SM_LV, UI_C(TEXT2));
        lv_obj_set_width(l, 100);
        q->days[c] = ui_set_number(s, h, q->days0[c], 0, 3650);
    }
    return q;
}

static int quota_dirty(void *st)
{
    quota_t *q = st;
    if (!q) return 0;
    for (int c = 0; c < q->n; ++c)
        if (ui_set_number_get(q->gb[c]) != q->gb0[c] || ui_set_number_get(q->days[c]) != q->days0[c]) return 1;
    return 0;
}

static int quota_save(void *st)
{
    quota_t *q = st;
    if (!q) return 0;
    rows_t r;
    if (rows_load(&r, "quota") < 0) return -1;
    int nf = r.n_fields, max = ui_model()->sections[q->sec].max_items;
    char(*v)[VAL_MAX] = calloc((size_t)(max ? max : 1) * (size_t)nf, VAL_MAX);
    if (!v) {
        rows_free(&r);
        return -1;
    }
    int n = 0;
    for (int i = 0; i < r.n_items && n < max; ++i) {
        const char *cs = cell(&r, i, "camera");
        int ch = cs ? atoi(cs) : 0, shown = 0;
        for (int c = 0; c < q->n; ++c) shown |= q->ch[c] == ch;
        if (shown) continue;
        memcpy(v[n * nf], r.v[i * nf], (size_t)nf * VAL_MAX);
        ++n;
    }
    for (int c = 0; c < q->n && n < max; ++c) {
        int g = ui_set_number_get(q->gb[c]), d = ui_set_number_get(q->days[c]);
        if (!g && !d) continue;
        for (int f = 0; f < nf; ++f) {
            const char *key = ui_model()->sections[q->sec].fields[f].key;
            if (!strcmp(key, "camera")) snprintf(v[n * nf + f], VAL_MAX, "%d", q->ch[c]);
            else if (!strcmp(key, "max_gb")) snprintf(v[n * nf + f], VAL_MAX, "%d", g);
            else if (!strcmp(key, "max_days")) snprintf(v[n * nf + f], VAL_MAX, "%d", d);
        }
        ++n;
    }
    rows_free(&r);
    ui_errors_t errs;
    int rc = save_rows(q->sec, n, v, q->gen, &errs);
    free(v);
    quota_load(q);
    quota_fill(q);
    return rc;
}

static void quota_cancel(void *st)
{
    quota_t *q = st;
    if (!q) return;
    quota_load(q);
    quota_fill(q);
}

/* ---- Network Storage --------------------------------------------------------------- */

typedef struct {
    ui_set_t  *s;
    ui_form_t *form;
    lv_obj_t  *job, *mounted;
} nas_t;

static void nas_mounted_text(nas_t *n)
{
    int m = MG() && MG()->nas_mounted && MG()->nas_mounted();
    lv_label_set_text(n->mounted, m ? "Mounted at /mnt/nas" : "Not mounted (/mnt/nas)");
    lv_obj_set_style_text_color(n->mounted, m ? UI_C(OK) : UI_C(TEXT2), 0);
}

static void nas_done(void *user, int ok, const char *msg)
{
    nas_t *n = user;
    ui_toast(ok ? UI_TOAST_OK : UI_TOAST_BAD, "%s", msg && *msg ? msg : ok ? "The share works" : "Test failed");
    nas_mounted_text(n);
}

static void nas_test_cb(lv_event_t *e)
{
    nas_t *n = lv_event_get_user_data(e);
    if (ui_form_embed_dirty(n->form)) {
        ui_toast(UI_TOAST_WARN, "Save the settings first: the test uses the saved ones");
        return;
    }
    int id = MG() && MG()->nas_test ? MG()->nas_test() : -1;
    if (id <= 0) {
        ui_toast(UI_TOAST_BAD, "%s", id == UI_MGMT_EUNMANAGED ? "vigild does not manage this system"
                                                              : "Cannot test now");
        return;
    }
    ui_set_follow_job(n->s, id, n->job, nas_done, n);
}

static void nas_record_cb(lv_event_t *e)
{
    (void)e;
    rows_t r;
    if (rows_load(&r, "record") < 0) return;
    char *p = cell(&r, 0, "path");
    if (p) snprintf(p, VAL_MAX, "/mnt/nas/rec");
    ui_errors_t errs;
    if (save_rows(r.sec, 1, r.v, generation(), &errs) == 0)
        ui_toast(UI_TOAST_OK, "Recording into /mnt/nas/rec");
    rows_free(&r);
}

static void support_line(lv_obj_t *sec, const char *label, const char *type)
{
    char why[256] = "";
    int ok = MG() && MG()->nas_support ? MG()->nas_support(type, why, sizeof why) : 0;
    lv_obj_t *v = ui_set_info(sec, label, ok ? "can be mounted" : why[0] ? why : "not available");
    lv_obj_set_style_text_color(v, ok ? UI_C(OK) : UI_C(WARN), 0);
    lv_obj_set_width(v, LV_PCT(100));
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
}

static void *nas_open(ui_set_t *s, lv_obj_t *pane)
{
    nas_t *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->s = s;
    lv_obj_t *sup = ui_set_section(pane, NULL);
    support_line(sup, "SMB (CIFS)", "cifs");
    support_line(sup, "NFS", "nfs");
    lv_obj_t *row = ui_set_hbox(pane);
    n->mounted = ui_label(row, "", VG_FONT_MD_LV, UI_C(TEXT2));
    nas_mounted_text(n);
    lv_obj_t *test = ui_btn(row, LV_SYMBOL_UPLOAD, TR(S_TEST));
    lv_obj_t *rec = ui_btn(row, LV_SYMBOL_DRIVE, "Record here");
    n->job = ui_set_note(pane, "", UI_C(TEXT2));
    n->form = ui_set_form(s, pane, "nas");
    /* After the form in the keyboard's order: the settings come first. */
    lv_obj_add_event_cb(test, nas_test_cb, LV_EVENT_CLICKED, n);
    lv_obj_add_event_cb(rec, nas_record_cb, LV_EVENT_CLICKED, n);
    lv_group_add_obj(ui_set_group(s), test);
    lv_group_add_obj(ui_set_group(s), rec);
    return n;
}

static int nas_dirty(void *st) { return st && ui_form_embed_dirty(((nas_t *)st)->form); }
static int nas_save(void *st) { return st ? ui_form_embed_save(((nas_t *)st)->form) : 0; }
static void nas_cancel(void *st)
{
    if (st) ui_form_embed_reset(((nas_t *)st)->form);
}
static void nas_close(void *st)
{
    if (!st) return;
    ui_form_embed_free(((nas_t *)st)->form);
    free(st);
}

/* ---- the screen ------------------------------------------------------------------------ */

static ui_set_item_t g_items[6];

void ui_storage_open(int first)
{
    const ui_set_item_t items[] = {
        { TR(S_BASIC), &ui_icon_nav_basic, basic_open, basic_dirty, basic_save, basic_cancel, basic_close },
        { TR(S_SCHEDULE), &ui_icon_nav_schedule, sched_open, sched_dirty, sched_save, sched_cancel, free_st },
        { TR(S_DISK_MANAGER), &ui_icon_nav_disks, disks_open, NULL, NULL, NULL, disks_close },
        { TR(S_DISK_MAINT), &ui_icon_nav_maint, ui_maint_open, NULL, NULL, NULL, ui_maint_close },
        { TR(S_DISK_QUOTA), &ui_icon_nav_quota, quota_open, quota_dirty, quota_save, quota_cancel, free_st },
        { TR(S_NET_STORAGE), &ui_icon_nav_nas, nas_open, nas_dirty, nas_save, nas_cancel, nas_close },
    };
    memcpy(g_items, items, sizeof items);
    ui_settings_open(TR(S_STORAGE), g_items, 6, first);
}
