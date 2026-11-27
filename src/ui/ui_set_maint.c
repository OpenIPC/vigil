/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_set_maint.c — Storage > Disk Maintenance (docs/management.md): a
 * disk's SMART health and a Victoria/MHDD-like surface scan.
 *
 *   SMART         the verdict (Good / Warning / Failing) and why, the
 *                 drive's identity, the attribute table with the counts
 *                 that matter highlighted (amber: not zero; red: at or
 *                 below the threshold), the self-tests and their log.
 *                 Read by a job of the management layer: a sleeping disk
 *                 spins up first.
 *   Surface scan  Read / Verify / Write over a range of LBAs in blocks;
 *                 the sector map (drawn on one object, as the schedule
 *                 grid is: 2000 cells as widgets would not fit the LVGL
 *                 heap), the counts per latency class, the bad blocks.
 *                 The scan runs in vigild, not in the page: leaving the
 *                 page leaves it running, and the page shows it again.
 *                 Write erases the disk: the disk's model, serial and
 *                 size are named, and its name must be typed.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ui_i18n.h"
#include "ui_settings.h"
#include "ui_theme.h"

#define DISKS      24
#define TICK_MS    1000
#define REREAD_S   30               /* SMART again while a self-test runs */
#define MAP_H      250
#define NOTE_W     260

static const ui_mgmt_t *MG(void) { return ui_model()->mgmt; }
static int managed(void) { return MG() && MG()->managed && MG()->managed(); }

static const unsigned BLOCKS[] = { 64, 128, 256, 512, 1024, 2048 };
#define BLOCK_DEFAULT 2

enum { TAB_SMART, TAB_SCAN };

typedef struct {
    ui_set_t   *s;
    int         tab, n;
    ui_disk_t   d[DISKS];           /* whole disks */
    char        cur[32];
    lv_obj_t   *pane, *disk_dd, *tab_btn[2], *job, *body;
    lv_timer_t *timer;
    ui_smart_t *smart;              /* of cur, when smart_ok */
    int         smart_ok, reading;
    ui_scan_t  *scan;
    int         built_active;       /* the scan tab was built for a scan under way */
    char        built_disk[32];
    unsigned long long built_done;
    int         built_bad;
    lv_obj_t   *mode_dd, *block_dd, *start_ta, *end_ta, *go, *pause, *stop, *mode_help;
    lv_obj_t   *title, *fill, *stats, *msg, *map, *count[UI_SCAN_CLASSES], *bad;
    lv_obj_t   *confirm, *confirm_text, *confirm_name, *confirm_go;
} maint_t;

static void build(maint_t *m);

/* ---- text ----------------------------------------------------------------------------- */

/* 1953525168 -> "1 953 525 168" */
static void grouped(unsigned long long v, char *out, size_t cap)
{
    char t[32];
    int n = snprintf(t, sizeof t, "%llu", v), o = 0;
    for (int i = 0; i < n && (size_t)o + 2 < cap; ++i) {
        if (i && (n - i) % 3 == 0) out[o++] = ' ';
        out[o++] = t[i];
    }
    out[o] = 0;
}

static void hms(long s, char *out, size_t cap)
{
    if (s < 0) snprintf(out, cap, "-");
    else snprintf(out, cap, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
}

static lv_color_t verdict_color(int v)
{
    return v == UI_SMART_GOOD ? UI_C(OK) : v == UI_SMART_WARN ? UI_C(WARN) : v == UI_SMART_FAIL ? UI_C(BAD)
                                                                                                : UI_C(TEXT2);
}

static lv_color_t class_color(int c)
{
    static const uint32_t C[] = { VG_COLOR_SCAN_5,   VG_COLOR_SCAN_20,   VG_COLOR_SCAN_50, VG_COLOR_SCAN_150,
                                  VG_COLOR_SCAN_500, VG_COLOR_SCAN_SLOW, VG_COLOR_SCAN_ERR };
    return c >= 0 && c < UI_SCAN_CLASSES ? lv_color_hex(C[c]) : UI_C(PANEL2);
}

static const char *const CLASS_NAMES[UI_SCAN_CLASSES] = { "< 5 ms", "< 20 ms", "< 50 ms", "< 150 ms",
                                                          "< 500 ms", "500+ ms", "Error" };

static const char *mode_name(int mode)
{
    return mode == UI_SCAN_VERIFY ? "Verify" : mode == UI_SCAN_WRITE ? "Write" : "Read";
}

static int scan_active(const ui_scan_t *sc)
{
    return sc->state == UI_SCAN_RUNNING || sc->state == UI_SCAN_PAUSED;
}

int ui_maint_scan_line(char *out, size_t cap, char *disk, size_t dcap)
{
    out[0] = 0;
    if (disk && dcap) disk[0] = 0;
    if (!MG() || !MG()->scan_get) return 0;
    ui_scan_t *sc = malloc(sizeof *sc);
    if (!sc) return 0;
    MG()->scan_get(sc);
    int on = scan_active(sc);
    if (on) {
        unsigned long long ranges = (unsigned long long)sc->n_bad + sc->bad_more;
        snprintf(out, cap, LV_SYMBOL_REFRESH "  Surface scan (%s) of %s %s: %.1f%%, %llu bad range%s. Disk "
                 "Maintenance shows it.", mode_name(sc->mode), sc->disk,
                 sc->state == UI_SCAN_PAUSED ? "paused" : "under way",
                 sc->blocks_total ? (double)sc->blocks_done * 100.0 / (double)sc->blocks_total : 0.0, ranges,
                 ranges == 1 ? "" : "s");
        if (disk && dcap) snprintf(disk, dcap, "%s", sc->disk);
    }
    free(sc);
    return on;
}

/* ---- small widgets --------------------------------------------------------------------- */

static lv_obj_t *column(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    ui_plain(c);
    ui_ring_room(c);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(c, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, VG_SPACE_SM, 0);
    return c;
}

static lv_obj_t *cell(lv_obj_t *row, const char *text, int w, lv_color_t c)
{
    lv_obj_t *l = ui_label(row, text, VG_FONT_SM_LV, c);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    } else {
        lv_obj_set_width(l, 0);
        lv_obj_set_flex_grow(l, 1);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    return l;
}

static lv_obj_t *trow(lv_obj_t *table, int header)
{
    lv_obj_t *r = lv_obj_create(table);
    ui_plain(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(r, header ? 26 : 30, 0);
    lv_obj_set_style_pad_hor(r, VG_SPACE_SM, 0);
    lv_obj_set_style_pad_ver(r, VG_SPACE_XS, 0);
    lv_obj_set_style_pad_column(r, VG_SPACE_SM, 0);
    lv_obj_set_style_border_color(r, UI_C(LINE), 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_layout(r, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return r;
}

/* "Label  value" in half the width of `grid`. */
static lv_obj_t *kv(lv_obj_t *grid, const char *key, const char *value, lv_color_t c)
{
    lv_obj_t *r = lv_obj_create(grid);
    ui_plain(r);
    lv_obj_set_size(r, LV_PCT(49), LV_SIZE_CONTENT);
    lv_obj_set_layout(r, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r, VG_SPACE_SM, 0);
    lv_obj_t *k = ui_label(r, key, VG_FONT_SM_LV, UI_C(TEXT2));
    lv_obj_set_width(k, 130);
    lv_obj_t *v = ui_label(r, value, VG_FONT_SM_LV, c);
    lv_obj_set_width(v, 0);
    lv_obj_set_flex_grow(v, 1);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    return v;
}

/* A table the keyboard can reach: focused, Up and Down scroll the page. */
static void table_key_cb(lv_event_t *e)
{
    maint_t *m = lv_event_get_user_data(e);
    uint32_t k = lv_event_get_key(e);
    if (k != LV_KEY_UP && k != LV_KEY_DOWN) return;
    lv_obj_scroll_by_bounded(m->pane, 0, k == LV_KEY_UP ? 90 : -90, LV_ANIM_OFF);
    lv_event_stop_processing(e);
}

static lv_obj_t *table(maint_t *m, lv_obj_t *parent)
{
    lv_obj_t *t = column(parent);
    lv_obj_set_style_pad_row(t, 0, 0);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(t, &ui_st_focus, LV_STATE_FOCUSED);
    lv_obj_add_flag(t, UI_FLAG_ARROWS);
    lv_obj_add_event_cb(t, table_key_cb, LV_EVENT_KEY | LV_EVENT_PREPROCESS, m);
    lv_group_add_obj(ui_set_group(m->s), t);
    return t;
}

/* ---- SMART ---------------------------------------------------------------------------- */

static void smart_done(void *user, int ok, const char *msg)
{
    maint_t *m = user;
    (void)msg;
    m->reading = 0;
    m->smart_ok = MG()->smart_get(m->cur, m->smart) == 0;
    if (!ok) ui_toast(UI_TOAST_BAD, "SMART of %s: %s", m->cur, msg && *msg ? msg : "not read");
    if (m->tab == TAB_SMART) build(m);
}

static void smart_read(maint_t *m, int quiet)
{
    if (!MG()->smart_read || m->reading || !m->cur[0]) return;
    char err[200] = "";
    int id = MG()->smart_read(m->cur, err, sizeof err);
    if (id <= 0) {
        if (!quiet)
            ui_toast(UI_TOAST_BAD, "SMART: %s", id == UI_MGMT_EUNMANAGED ? "vigild does not manage this system" : err);
        return;
    }
    m->reading = 1;
    lv_obj_remove_flag(m->job, LV_OBJ_FLAG_HIDDEN);
    if (!quiet) lv_label_set_text_fmt(m->job, LV_SYMBOL_REFRESH "  Reading SMART of %s...", m->cur);
    ui_set_follow_job(m->s, id, m->job, smart_done, m);
}

static void again_cb(lv_event_t *e) { smart_read(lv_event_get_user_data(e), 0); }

static void test_cb(lv_event_t *e)
{
    maint_t *m = lv_event_get_user_data(e);
    int type = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    char err[200] = "";
    int rc = MG()->selftest ? MG()->selftest(m->cur, type, err, sizeof err) : UI_MGMT_EFAIL;
    if (rc < 0) {
        ui_toast(UI_TOAST_BAD, "%s", rc == UI_MGMT_EUNMANAGED ? "vigild does not manage this system" : err);
        return;
    }
    ui_toast(UI_TOAST_OK, type ? "%s self-test of %s started: the disk keeps working meanwhile"
                               : "Self-test of %s aborted", type == 1 ? "Short" : type == 2 ? "Extended" : m->cur,
             m->cur);
    smart_read(m, 1);
}

static lv_obj_t *test_btn(maint_t *m, lv_obj_t *row, const char *sym, const char *text, int type, int on)
{
    lv_obj_t *b = ui_set_button(m->s, row, sym, text);
    lv_obj_set_user_data(b, (void *)(intptr_t)type);
    lv_obj_add_event_cb(b, test_cb, LV_EVENT_CLICKED, m);
    if (!on) lv_obj_add_state(b, LV_STATE_DISABLED);
    return b;
}

static void smart_build(maint_t *m)
{
    const ui_smart_t *x = m->smart;
    if (!m->smart_ok) {
        ui_set_note(m->body, m->reading ? "Reading..." : "No SMART read yet.", UI_C(TEXT2));
        lv_obj_t *row = ui_set_hbox(m->body);
        lv_obj_t *b = ui_set_button(m->s, row, LV_SYMBOL_REFRESH, "Read SMART");
        lv_obj_add_event_cb(b, again_cb, LV_EVENT_CLICKED, m);
        return;
    }
    static const char *const WORD[] = { "Good", "Warning", "Failing", "Unknown" };
    static const char *const SYM[] = { LV_SYMBOL_OK, LV_SYMBOL_WARNING, LV_SYMBOL_CLOSE, LV_SYMBOL_DRIVE };
    int v = x->verdict >= 0 && x->verdict <= 3 ? x->verdict : 3;
    lv_obj_t *vp = ui_set_section(m->body, NULL);
    lv_obj_set_style_border_color(vp, verdict_color(v), 0);
    lv_obj_set_style_border_width(vp, 2, 0);
    lv_obj_t *vl = ui_label(vp, "", VG_FONT_XL_LV, verdict_color(v));
    lv_label_set_text_fmt(vl, "%s  %s", SYM[v], WORD[v]);
    ui_set_note(vp, x->reason, UI_C(TEXT));

    char t[96], u[64];
    lv_obj_t *row;
    if (!x->available) {
        ui_set_note(m->body, "A USB stick or a memory card has no SMART; a USB enclosure passes it only when its "
                             "bridge translates ATA commands (SAT). The surface scan works on any disk.",
                    UI_C(TEXT2));
        row = ui_set_hbox(m->body);
        lv_obj_t *b = ui_set_button(m->s, row, LV_SYMBOL_REFRESH, "Read again");
        lv_obj_add_event_cb(b, again_cb, LV_EVENT_CLICKED, m);
        return;
    }

    lv_obj_t *idp = ui_set_section(m->body, NULL);
    lv_obj_t *grid = lv_obj_create(idp);
    ui_plain(grid);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(grid, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, VG_SPACE_XS, 0);
    lv_obj_set_style_pad_column(grid, VG_SPACE_SM, 0);
    kv(grid, "Model", x->model, UI_C(TEXT));
    kv(grid, "Serial", x->serial[0] ? x->serial : "-", UI_C(TEXT));
    kv(grid, "Firmware", x->firmware[0] ? x->firmware : "-", UI_C(TEXT));
    ui_set_size_text(x->bytes, u, sizeof u);
    kv(grid, "Capacity", u, UI_C(TEXT));
    if (x->rpm == 1) snprintf(t, sizeof t, "Solid state (SSD)");
    else if (x->rpm > 1) snprintf(t, sizeof t, "Hard disk, %d rpm", x->rpm);
    else snprintf(t, sizeof t, "Hard disk (speed not given)");
    kv(grid, "Type", t, UI_C(TEXT));
    static const char *const GEN[] = { "", "1.5 Gb/s", "3.0 Gb/s", "6.0 Gb/s" };
    int usb = !strcmp(x->transport, "USB");
    if (x->sata_max && x->sata_cur)
        snprintf(t, sizeof t, "%sSATA %s (the drive: up to %s)", usb ? "USB; behind the bridge " : "",
                 GEN[x->sata_cur], GEN[x->sata_max]);
    else if (x->sata_max) snprintf(t, sizeof t, "%sSATA, up to %s", usb ? "USB; " : "", GEN[x->sata_max]);
    else snprintf(t, sizeof t, "%s", x->transport[0] ? x->transport : "-");
    kv(grid, "Interface", t, UI_C(TEXT));
    int hot = x->temp > (x->rpm == 1 ? 70 : 55);
    if (x->temp >= 0) snprintf(t, sizeof t, "%d C%s", x->temp, hot ? ": too hot" : "");
    else snprintf(t, sizeof t, "-");
    kv(grid, "Temperature", t, hot ? UI_C(WARN) : UI_C(TEXT));
    if (x->hours >= 8760) snprintf(t, sizeof t, "%ld h (%.1f years)", x->hours, (double)x->hours / 8760.0);
    else if (x->hours >= 0) snprintf(t, sizeof t, "%ld h (%ld days)", x->hours, x->hours / 24);
    else snprintf(t, sizeof t, "-");
    kv(grid, "Powered on", t, UI_C(TEXT));
    kv(grid, "SMART status", !x->smart_enabled ? "SMART is off" : x->health == 1 ? "Passed"
                                  : x->health == 0 ? "FAILING" : "Not reported",
       x->health == 0 ? UI_C(BAD) : UI_C(TEXT));
    if (x->test_running) snprintf(t, sizeof t, "Running, %d%% left", x->test_remaining);
    else if (x->n_tests) snprintf(t, sizeof t, "Last: %s (%s)", x->tests[0].status, x->tests[0].type);
    else snprintf(t, sizeof t, "%s", x->test_supported ? "None in the log" : "Not supported");
    kv(grid, "Self-test", t, x->n_tests && x->tests[0].failed && !x->test_running ? UI_C(BAD) : UI_C(TEXT));

    row = ui_set_hbox(idp);
    int ok = managed() && x->test_supported;
    snprintf(t, sizeof t, "Short test (%d min)", x->short_min ? x->short_min : 2);
    test_btn(m, row, LV_SYMBOL_PLAY, t, 1, ok && !x->test_running);
    snprintf(t, sizeof t, "Extended test (%d min)", x->ext_min);
    test_btn(m, row, LV_SYMBOL_PLAY, t, 2, ok && !x->test_running);
    if (x->test_running) test_btn(m, row, LV_SYMBOL_STOP, "Abort the test", 0, ok);
    lv_obj_t *b = ui_set_button(m->s, row, LV_SYMBOL_REFRESH, "Read again");
    lv_obj_add_event_cb(b, again_cb, LV_EVENT_CLICKED, m);
    ui_set_time_text(x->read_at, u, sizeof u);
    snprintf(t, sizeof t, "Read %s", u);
    ui_label(row, t, VG_FONT_SM_LV, UI_C(TEXT2));

    if (x->n_attrs) {
        ui_label(m->body, "Attributes", VG_FONT_LG_LV, UI_C(TEXT));
        lv_obj_t *p = ui_set_section(m->body, NULL);
        lv_obj_set_style_pad_hor(p, VG_SPACE_SM, 0);
        lv_obj_t *tb = table(m, p);
        lv_obj_t *h = trow(tb, 1);
        cell(h, "ID", 34, UI_C(TEXT2));
        cell(h, "Attribute", 196, UI_C(TEXT2));
        cell(h, "Value", 48, UI_C(TEXT2));
        cell(h, "Worst", 48, UI_C(TEXT2));
        cell(h, "Thresh", 52, UI_C(TEXT2));
        cell(h, "Raw", 170, UI_C(TEXT2));
        cell(h, "", 0, UI_C(TEXT2));
        for (int i = 0; i < x->n_attrs; ++i) {
            const ui_smart_attr_t *a = &x->attrs[i];
            lv_obj_t *r = trow(tb, 0);
            lv_color_t c = a->level == UI_SMART_FAIL ? UI_C(BAD) : a->level == UI_SMART_WARN ? UI_C(WARN)
                                                                                               : UI_C(TEXT);
            if (a->level != UI_SMART_GOOD) {
                lv_obj_set_style_bg_color(r, c, 0);
                lv_obj_set_style_bg_opa(r, LV_OPA_20, 0);
            }
            snprintf(t, sizeof t, "%d", a->id);
            cell(r, t, 34, a->critical ? UI_C(TEXT) : UI_C(TEXT2));
            cell(r, a->name, 196, c);
            snprintf(t, sizeof t, "%d", a->value);
            cell(r, t, 48, c);
            snprintf(t, sizeof t, "%d", a->worst);
            cell(r, t, 48, UI_C(TEXT2));
            snprintf(t, sizeof t, "%d", a->thresh);
            cell(r, t, 52, UI_C(TEXT2));
            cell(r, a->raw, 170, a->critical && a->level != UI_SMART_GOOD ? c : UI_C(TEXT));
            cell(r, a->note, 0, c);
        }
        ui_set_note(m->body, "Value and worst are the drive's own scores (higher is better); at or below the "
                             "threshold the drive counts as failing. Amber rows: a count that only grows on a "
                             "sick disk is not zero. Interface CRC errors are usually the cable or the "
                             "connection.", UI_C(TEXT2));
    }
    if (x->n_tests) {
        ui_label(m->body, "Self-test log", VG_FONT_LG_LV, UI_C(TEXT));
        lv_obj_t *p = ui_set_section(m->body, NULL);
        lv_obj_set_style_pad_hor(p, VG_SPACE_SM, 0);
        lv_obj_t *tb = column(p);
        lv_obj_set_style_pad_row(tb, 0, 0);
        lv_obj_t *h = trow(tb, 1);
        cell(h, "Type", 160, UI_C(TEXT2));
        cell(h, "Result", 280, UI_C(TEXT2));
        cell(h, "At hour", 90, UI_C(TEXT2));
        cell(h, "First error at LBA", 0, UI_C(TEXT2));
        for (int i = 0; i < x->n_tests; ++i) {
            const ui_smart_test_t *e = &x->tests[i];
            lv_obj_t *r = trow(tb, 0);
            lv_color_t c = e->failed ? UI_C(BAD) : UI_C(TEXT);
            cell(r, e->type, 160, UI_C(TEXT));
            cell(r, e->status, 280, c);
            snprintf(t, sizeof t, "%d", e->hours);
            cell(r, t, 90, UI_C(TEXT2));
            if (e->failed) grouped(e->lba, t, sizeof t);
            else snprintf(t, sizeof t, "-");
            cell(r, t, 0, c);
        }
    }
}

/* ---- the surface scan ------------------------------------------------------------------ */

static const ui_disk_t *disk(maint_t *m, const char *name)
{
    for (int i = 0; i < m->n; ++i)
        if (!strcmp(m->d[i].name, name)) return &m->d[i];
    return NULL;
}

/* A partition of `name` mounted (1) or recorded onto (2); 0 neither. */
static int in_use(const char *name, char *mnt, size_t cap)
{
    ui_disk_t all[DISKS];
    int n = MG()->disks ? MG()->disks(all, DISKS) : 0, use = 0;
    mnt[0] = 0;
    for (int i = 0; i < n; ++i) {
        if (strcmp(all[i].name, name) && strcmp(all[i].parent, name)) continue;
        if (!all[i].mnt[0]) continue;
        int u = !strcmp(all[i].state, "recording") ? 2 : 1;
        if (u > use) {
            use = u;
            snprintf(mnt, cap, "%s", all[i].mnt);
        }
    }
    return use;
}

static void map_draw_cb(lv_event_t *e)
{
    maint_t *m = lv_event_get_user_data(e);
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const ui_scan_t *sc = m->scan;
    int cells = sc->cells > 0 ? sc->cells : UI_SCAN_MAP;
    int w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    /* The largest square cells that fit them all. */
    int px = 24, cols = 1;
    for (; px > 3; --px) {
        cols = w / px;
        if (cols > 0 && ((cells + cols - 1) / cols) * px <= h) break;
    }
    cols = w / px;
    if (cols < 1) return;
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.radius = 0;
    rd.bg_opa = LV_OPA_COVER;
    int gap = px >= 6 ? 1 : 0;
    long cur = -1;
    if (scan_active(sc) && sc->end > sc->start && sc->block) {
        unsigned long long per = (sc->end - sc->start + sc->block - 1) / sc->block;
        unsigned long long bi = sc->lba > sc->start ? (sc->lba - sc->start) / sc->block : 0;
        if (per) cur = (long)(bi * (unsigned long long)cells / per);
    }
    for (int i = 0; i < cells; ++i) {
        int x = a.x1 + (i % cols) * px, y = a.y1 + (i / cols) * px;
        lv_area_t ca = { x, y, x + px - 1 - gap, y + px - 1 - gap };
        int v = sc->cells > 0 ? sc->map[i] : 0;
        rd.bg_color = v ? class_color(v - 1) : UI_C(PANEL2);
        rd.border_width = 0;
        lv_draw_rect(layer, &rd, &ca);
    }
    if (cur >= 0 && cur < cells) {
        int x = a.x1 + (int)(cur % cols) * px, y = a.y1 + (int)(cur / cols) * px;
        lv_area_t ca = { x - 1, y - 1, x + px - gap, y + px - gap };
        rd.bg_opa = LV_OPA_TRANSP;
        rd.border_color = UI_C(FOCUS);
        rd.border_width = 2;
        rd.border_opa = LV_OPA_COVER;
        lv_draw_rect(layer, &rd, &ca);
    }
}

static void scan_text(maint_t *m)
{
    const ui_scan_t *sc = m->scan;
    char a[32], b[32], c[24], d[24], t[256];
    if (sc->state == UI_SCAN_IDLE) {
        lv_label_set_text(m->title, "No scan yet");
        lv_label_set_text(m->stats, "");
        lv_label_set_text(m->msg, "");
        lv_obj_set_width(m->fill, 0);
    } else {
        double pct = sc->blocks_total ? (double)sc->blocks_done * 100.0 / (double)sc->blocks_total : 0;
        static const char *const ST[] = { "", "under way", "paused", "done", "stopped", "failed" };
        snprintf(t, sizeof t, "%s of %s%s%s%s: %s, %.1f%%", mode_name(sc->mode), sc->disk, sc->model[0] ? " (" : "",
                 sc->model, sc->model[0] ? ")" : "", ST[sc->state >= 0 && sc->state <= 5 ? sc->state : 0], pct);
        lv_label_set_text(m->title, t);
        lv_obj_set_style_text_color(m->title, sc->state == UI_SCAN_FAILED ? UI_C(BAD) : UI_C(TEXT), 0);
        lv_obj_set_width(m->fill, LV_PCT((int)(pct + 0.5)));
        grouped(sc->lba, a, sizeof a);
        grouped(sc->end, b, sizeof b);
        hms(sc->elapsed, c, sizeof c);
        hms(scan_active(sc) ? sc->eta : 0, d, sizeof d);
        snprintf(t, sizeof t, "LBA %s of %s   %.1f MB/s   %u sectors a block%s   elapsed %s   left %s", a, b,
                 sc->mbps, sc->block,
                 sc->passes > 1 ? (sc->pass > 1 ? "   pass 2 of 2 (read back)" : "   pass 1 of 2 (write)") : "",
                 c, d);
        lv_label_set_text(m->stats, t);
        lv_label_set_text(m->msg, sc->msg);
        lv_obj_set_style_text_color(m->msg, sc->read_instead || sc->state == UI_SCAN_FAILED ? UI_C(WARN)
                                                                                           : UI_C(TEXT2), 0);
    }
    for (int i = 0; i < UI_SCAN_CLASSES; ++i) {
        grouped(sc->counts[i], a, sizeof a);
        lv_label_set_text(m->count[i], a);
    }
    if (m->built_bad != sc->n_bad + (int)sc->bad_more) {
        m->built_bad = sc->n_bad + (int)sc->bad_more;
        if (!sc->n_bad) {
            lv_label_set_text(m->bad, sc->state == UI_SCAN_IDLE ? "" : scan_active(sc) ? "Bad blocks: none so far."
                                                                                     : "Bad blocks: none.");
            lv_obj_set_style_text_color(m->bad, UI_C(TEXT2), 0);
        } else {
            char list[UI_SCAN_BAD * 64 + 64];
            int o = snprintf(list, sizeof list, "Bad blocks (LBA ranges):");
            for (int i = 0; i < sc->n_bad && o < (int)sizeof list - 64; ++i) {
                grouped(sc->bad_lba[i], a, sizeof a);
                grouped(sc->bad_lba[i] + sc->bad_count[i] - 1, b, sizeof b);
                o += snprintf(list + o, sizeof list - (size_t)o, "%s  %s - %s (%llu)", i % 2 ? "" : "\n", a, b,
                              sc->bad_count[i]);
            }
            if (sc->bad_more)
                snprintf(list + o, sizeof list - (size_t)o, "\nand %llu more ranges", sc->bad_more);
            lv_label_set_text(m->bad, list);
            lv_obj_set_style_text_color(m->bad, UI_C(BAD), 0);
        }
    }
    if (m->pause)
        lv_label_set_text(ui_btn_label(m->pause), sc->state == UI_SCAN_PAUSED ? LV_SYMBOL_PLAY "  Resume"
                                                                              : LV_SYMBOL_PAUSE "  Pause");
    lv_obj_invalidate(m->map);
}

static void mode_help(maint_t *m)
{
    static const char *const H[] = {
        "Read: every block is read (no cache) and timed. Safe on a disk in use.",
        "Verify: the drive reads each block itself (READ VERIFY), nothing crosses the cable, so the times are "
        "the surface's alone. A USB bridge may not pass it: the scan then reads, and says so.",
        "Write: a pattern is written over every block, then read back and compared. EVERYTHING ON THE DISK IS "
        "ERASED. Only for a disk that is not mounted and not recorded onto.",
    };
    int mode = (int)lv_dropdown_get_selected(m->mode_dd);
    char mnt[128], t[400];
    int use = in_use(m->cur, mnt, sizeof mnt);
    snprintf(t, sizeof t, "%s", H[mode < 3 ? mode : 0]);
    if (use && mode != UI_SCAN_WRITE) {
        size_t l = strlen(t);
        snprintf(t + l, sizeof t - l, "\n%s is mounted at %s%s: the scan reads at the idle I/O priority so the disk "
                 "serves recording first, but recording may still slow down.", m->cur, mnt,
                 use == 2 ? " and recorded onto" : "");
    } else if (use) {
        size_t l = strlen(t);
        snprintf(t + l, sizeof t - l, "\n%s is mounted at %s: unmount it in Disk Manager first.", m->cur, mnt);
    }
    lv_label_set_text(m->mode_help, t);
    lv_obj_set_style_text_color(m->mode_help, mode == UI_SCAN_WRITE || (use == 2) ? UI_C(WARN) : UI_C(TEXT2), 0);
}

static void mode_cb(lv_event_t *e) { mode_help(lv_event_get_user_data(e)); }

static unsigned long long ta_ull(lv_obj_t *ta) { return strtoull(lv_textarea_get_text(ta), NULL, 10); }

static void start_scan(maint_t *m, int mode, const char *confirm)
{
    unsigned block = BLOCKS[lv_dropdown_get_selected(m->block_dd)];
    char err[256] = "";
    int rc = MG()->scan_start(m->cur, mode, ta_ull(m->start_ta), ta_ull(m->end_ta), block, confirm, err, sizeof err);
    if (rc < 0) {
        ui_toast(UI_TOAST_BAD, "Not started: %s", rc == UI_MGMT_EUNMANAGED ? "vigild does not manage this system"
                                                                         : err);
        return;
    }
    ui_toast(UI_TOAST_OK, "%s scan of %s started", mode_name(mode), m->cur);
    build(m);
}

static void confirm_check(maint_t *m)
{
    if (!strcmp(lv_textarea_get_text(m->confirm_name), m->cur)) lv_obj_remove_state(m->confirm_go, LV_STATE_DISABLED);
    else lv_obj_add_state(m->confirm_go, LV_STATE_DISABLED);
}

static void confirm_name_cb(lv_event_t *e) { confirm_check(lv_event_get_user_data(e)); }

static void confirm_cancel_cb(lv_event_t *e)
{
    maint_t *m = lv_event_get_user_data(e);
    lv_obj_add_flag(m->confirm, LV_OBJ_FLAG_HIDDEN);
    lv_group_focus_obj(m->go);
}

static void confirm_go_cb(lv_event_t *e)
{
    maint_t *m = lv_event_get_user_data(e);
    start_scan(m, UI_SCAN_WRITE, lv_textarea_get_text(m->confirm_name));
}

static void confirm_show(maint_t *m)
{
    const ui_disk_t *d = disk(m, m->cur);
    char size[24], t[512];
    ui_set_size_text(d ? d->size : 0, size, sizeof size);
    const char *model = m->smart_ok && m->smart->available ? m->smart->model : d ? d->model : "";
    const char *serial = m->smart_ok && m->smart->available && m->smart->serial[0] ? m->smart->serial : NULL;
    snprintf(t, sizeof t, "%s: %s, %s%s, %s.\nEvery sector is written over and then read back: everything on "
             "this disk is erased - its partitions, files and recordings. It cannot be undone.", m->cur,
             model[0] ? model : "model unknown", serial ? "serial " : "serial unknown", serial ? serial : "", size);
    lv_label_set_text(m->confirm_text, t);
    lv_textarea_set_text(m->confirm_name, "");
    lv_textarea_set_placeholder_text(m->confirm_name, m->cur);
    confirm_check(m);
    lv_obj_remove_flag(m->confirm, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_view_recursive(m->confirm, LV_ANIM_OFF);
    lv_group_focus_obj(m->confirm_name);
}

static void go_cb(lv_event_t *e)
{
    maint_t *m = lv_event_get_user_data(e);
    int mode = (int)lv_dropdown_get_selected(m->mode_dd);
    if (mode == UI_SCAN_WRITE) confirm_show(m);
    else start_scan(m, mode, NULL);
}

static void pause_cb(lv_event_t *e)
{
    maint_t *m = lv_event_get_user_data(e);
    MG()->scan_get(m->scan);
    MG()->scan_pause(m->scan->state != UI_SCAN_PAUSED);
    MG()->scan_get(m->scan);
    scan_text(m);
}

static void stop_cb(lv_event_t *e)
{
    (void)e;
    MG()->scan_stop();
    ui_toast(UI_TOAST_OK, "The scan stops after the block under way");
}

static void scan_build(maint_t *m)
{
    ui_scan_t *sc = m->scan;
    MG()->scan_get(sc);
    int active = scan_active(sc), here = !strcmp(sc->disk, m->cur);
    m->built_active = active;
    snprintf(m->built_disk, sizeof m->built_disk, "%s", sc->disk);
    m->built_bad = -1;
    m->pause = m->stop = NULL;

    lv_obj_t *p = ui_set_section(m->body, NULL);
    lv_obj_t *row = ui_set_hbox(p);
    ui_label(row, "Mode", VG_FONT_MD_LV, UI_C(TEXT));
    m->mode_dd = ui_set_dropdown(m->s, row, "Read\nVerify (no data transfer)\nWrite (erases the disk)",
                                 active && here ? sc->mode : 0, 290);
    lv_obj_add_event_cb(m->mode_dd, mode_cb, LV_EVENT_VALUE_CHANGED, m);
    ui_label(row, "  Block", VG_FONT_MD_LV, UI_C(TEXT));
    /* The block size of the last scan of this disk; the mode is Read
     * again once one ended, so Write is never a click away. */
    int bsel = BLOCK_DEFAULT;
    for (int i = 0; i < (int)(sizeof BLOCKS / sizeof BLOCKS[0]); ++i)
        if (here && sc->state != UI_SCAN_IDLE && BLOCKS[i] == sc->block) bsel = i;
    m->block_dd = ui_set_dropdown(m->s, row, "64 sectors\n128 sectors\n256 sectors\n512 sectors\n1024 sectors\n"
                                             "2048 sectors", bsel, 190);
    row = ui_set_hbox(p);
    ui_label(row, "Start LBA", VG_FONT_MD_LV, UI_C(TEXT));
    char v[32];
    snprintf(v, sizeof v, "%llu", active && here ? sc->start : 0ull);
    m->start_ta = ui_set_text(m->s, row, v, 15, 170);
    lv_textarea_set_accepted_chars(m->start_ta, "0123456789");
    ui_label(row, "  End LBA", VG_FONT_MD_LV, UI_C(TEXT));
    if (active && here) snprintf(v, sizeof v, "%llu", sc->end);
    else v[0] = 0;
    m->end_ta = ui_set_text(m->s, row, v, 15, 170);
    lv_textarea_set_placeholder_text(m->end_ta, "the end");
    lv_textarea_set_accepted_chars(m->end_ta, "0123456789");
    ui_label(row, " ", VG_FONT_MD_LV, UI_C(TEXT));
    if (active) {
        m->pause = ui_set_button(m->s, row, LV_SYMBOL_PAUSE, "Pause");
        lv_obj_add_event_cb(m->pause, pause_cb, LV_EVENT_CLICKED, m);
        m->stop = ui_set_button(m->s, row, LV_SYMBOL_STOP, "Stop");
        lv_obj_add_event_cb(m->stop, stop_cb, LV_EVENT_CLICKED, m);
        lv_obj_add_state(m->mode_dd, LV_STATE_DISABLED);
        lv_obj_add_state(m->block_dd, LV_STATE_DISABLED);
        lv_obj_add_state(m->start_ta, LV_STATE_DISABLED);
        lv_obj_add_state(m->end_ta, LV_STATE_DISABLED);
        m->go = m->pause;
    } else {
        m->go = ui_set_button(m->s, row, LV_SYMBOL_PLAY, "Start");
        ui_btn_accent(m->go);
        lv_obj_add_event_cb(m->go, go_cb, LV_EVENT_CLICKED, m);
        if (!managed() || !m->cur[0]) lv_obj_add_state(m->go, LV_STATE_DISABLED);
    }
    m->mode_help = ui_set_note(p, "", UI_C(TEXT2));
    if (active && !here)
        ui_set_note(p, "A scan of another disk is under way (below): one scan at a time.", UI_C(WARN));
    mode_help(m);

    /* The write question, in the pane so Tab and the on-screen keyboard
     * reach its field. */
    m->confirm = ui_set_section(m->body, NULL);
    lv_obj_set_style_border_color(m->confirm, UI_C(BAD), 0);
    lv_obj_set_style_border_width(m->confirm, 2, 0);
    lv_obj_t *ct = ui_label(m->confirm, LV_SYMBOL_WARNING "  Erase this disk and test it?", VG_FONT_LG_LV,
                            UI_C(BAD));
    (void)ct;
    m->confirm_text = ui_set_note(m->confirm, "", UI_C(TEXT));
    lv_obj_t *nb = ui_set_row(m->confirm, "Type the disk's name to confirm", NULL);
    m->confirm_name = ui_set_text(m->s, nb, "", 31, 200);
    lv_obj_add_event_cb(m->confirm_name, confirm_name_cb, LV_EVENT_VALUE_CHANGED, m);
    lv_obj_t *btns = ui_set_hbox(m->confirm);
    lv_obj_t *cancel = ui_set_button(m->s, btns, LV_SYMBOL_CLOSE, TR(S_CANCEL));
    lv_obj_add_event_cb(cancel, confirm_cancel_cb, LV_EVENT_CLICKED, m);
    m->confirm_go = ui_set_button(m->s, btns, LV_SYMBOL_TRASH, "Erase and test");
    lv_obj_set_style_bg_color(m->confirm_go, UI_C(BAD), 0);
    lv_obj_set_style_bg_opa(m->confirm_go, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(m->confirm_go, confirm_go_cb, LV_EVENT_CLICKED, m);
    lv_obj_add_flag(m->confirm, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *st = ui_set_section(m->body, NULL);
    m->title = ui_label(st, "", VG_FONT_MD_LV, UI_C(TEXT));
    lv_obj_t *track = lv_obj_create(st);
    ui_plain(track);
    lv_obj_set_size(track, LV_PCT(100), 8);
    lv_obj_set_style_bg_color(track, UI_C(PANEL2), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, VG_RADIUS_SM, 0);
    m->fill = lv_obj_create(track);
    ui_plain(m->fill);
    lv_obj_set_size(m->fill, 0, LV_PCT(100));
    lv_obj_set_style_bg_color(m->fill, UI_C(ACCENT), 0);
    lv_obj_set_style_bg_opa(m->fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(m->fill, VG_RADIUS_SM, 0);
    m->stats = ui_set_note(st, "", UI_C(TEXT2));
    m->msg = ui_set_note(st, "", UI_C(TEXT2));
    m->map = lv_obj_create(st);
    ui_plain(m->map);
    lv_obj_remove_flag(m->map, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(m->map, LV_PCT(100), MAP_H);
    lv_obj_add_event_cb(m->map, map_draw_cb, LV_EVENT_DRAW_MAIN, m);
    lv_obj_t *leg = lv_obj_create(st);
    ui_plain(leg);
    lv_obj_set_size(leg, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(leg, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(leg, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(leg, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(leg, VG_SPACE_SM, 0);
    lv_obj_set_style_pad_row(leg, VG_SPACE_XS, 0);
    for (int i = 0; i < UI_SCAN_CLASSES; ++i) {
        lv_obj_t *it = ui_set_hbox(leg);
        lv_obj_set_style_pad_column(it, VG_SPACE_XS, 0);
        lv_obj_t *sw = lv_obj_create(it);
        ui_plain(sw);
        lv_obj_set_size(sw, 14, 14);
        lv_obj_set_style_bg_color(sw, class_color(i), 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        ui_label(it, CLASS_NAMES[i], VG_FONT_SM_LV, UI_C(TEXT2));
        m->count[i] = ui_label(it, "0", VG_FONT_SM_LV, i >= 4 ? class_color(i) : UI_C(TEXT));
        lv_obj_set_width(m->count[i], 92);
    }
    m->bad = ui_set_note(st, "", UI_C(TEXT2));
    scan_text(m);
    m->built_done = sc->blocks_done;
}

static void scan_tick(maint_t *m)
{
    MG()->scan_get(m->scan);
    const ui_scan_t *sc = m->scan;
    /* Started or ended (here, by the API, or on its own): the controls change. */
    if (scan_active(sc) != m->built_active || strcmp(sc->disk, m->built_disk)) {
        build(m);
        return;
    }
    if (sc->blocks_done == m->built_done && !scan_active(sc)) return;
    m->built_done = sc->blocks_done;
    scan_text(m);
}

/* ---- the page ---------------------------------------------------------------------------- */

static void build(maint_t *m)
{
    lv_obj_clean(m->body);
    for (int i = 0; i < 2; ++i) {
        if (i == m->tab) lv_obj_add_state(m->tab_btn[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(m->tab_btn[i], LV_STATE_CHECKED);
    }
    if (!m->n) {
        ui_set_note(m->body, "No disk: plug in a USB disk, or connect a SATA one.", UI_C(TEXT2));
        return;
    }
    if (m->tab == TAB_SMART) smart_build(m);
    else scan_build(m);
}

static void tab_cb(lv_event_t *e)
{
    maint_t *m = lv_event_get_user_data(e);
    int t = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (t == m->tab) {
        lv_obj_add_state(m->tab_btn[t], LV_STATE_CHECKED);
        return;
    }
    m->tab = t;
    build(m);
}

static void select_disk(maint_t *m, int i)
{
    snprintf(m->cur, sizeof m->cur, "%s", i >= 0 && i < m->n ? m->d[i].name : "");
    /* The last read's line is about the disk it read. */
    if (!m->reading) {
        lv_label_set_text(m->job, "");
        lv_obj_add_flag(m->job, LV_OBJ_FLAG_HIDDEN);
    }
    m->smart_ok = m->cur[0] && MG()->smart_get && MG()->smart_get(m->cur, m->smart) == 0;
    if (!m->smart_ok && m->cur[0] && managed()) smart_read(m, 0);
    build(m);
}

static void disk_cb(lv_event_t *e)
{
    maint_t *m = lv_event_get_user_data(e);
    select_disk(m, (int)lv_dropdown_get_selected(m->disk_dd));
}

static void tick(lv_timer_t *t)
{
    maint_t *m = lv_timer_get_user_data(t);
    if (m->tab == TAB_SCAN && m->n) {
        scan_tick(m);
        return;
    }
    /* A self-test under way: its progress, now and then. */
    if (m->tab == TAB_SMART && m->smart_ok && m->smart->test_running && !m->reading &&
        (long long)time(NULL) - m->smart->read_at >= REREAD_S)
        smart_read(m, 1);
}

void *ui_maint_open(ui_set_t *s, lv_obj_t *pane)
{
    ui_set_hide_actions(s);
    if (!MG() || !MG()->disks || !MG()->scan_get || !MG()->smart_get) {
        ui_set_note(pane, "This build has no disk maintenance.", UI_C(WARN));
        return NULL;
    }
    maint_t *m = calloc(1, sizeof *m);
    if (m) m->smart = calloc(1, sizeof *m->smart);
    if (m && m->smart) m->scan = calloc(1, sizeof *m->scan);
    if (!m || !m->smart || !m->scan) {
        if (m) free(m->smart);
        free(m);
        return NULL;
    }
    m->s = s;
    m->pane = pane;
    ui_disk_t all[DISKS];
    int na = MG()->disks(all, DISKS);
    for (int i = 0; i < na && m->n < DISKS; ++i)
        if (!all[i].is_part && all[i].size) m->d[m->n++] = all[i];

    if (!managed())
        ui_set_note(pane, LV_SYMBOL_WARNING "  vigild does not manage this system (it is not OpenIPC firmware): "
                                            "its disks are not read or tested.", UI_C(WARN));
    lv_obj_t *top = ui_set_hbox(pane);
    ui_label(top, "Disk", VG_FONT_MD_LV, UI_C(TEXT));
    char opts[DISKS * 96] = "";
    int sel = 0;
    MG()->scan_get(m->scan);
    for (int i = 0; i < m->n; ++i) {
        char size[24];
        ui_set_size_text(m->d[i].size, size, sizeof size);
        size_t l = strlen(opts);
        snprintf(opts + l, sizeof opts - l, "%s%s   %s%s(%s)", i ? "\n" : "", m->d[i].name, m->d[i].model,
                 m->d[i].model[0] ? "  " : "", size);
        if (scan_active(m->scan) && !strcmp(m->scan->disk, m->d[i].name)) sel = i;
    }
    m->disk_dd = ui_set_dropdown(s, top, m->n ? opts : "-", sel, 400);
    lv_obj_add_event_cb(m->disk_dd, disk_cb, LV_EVENT_VALUE_CHANGED, m);
    if (!m->n) lv_obj_add_state(m->disk_dd, LV_STATE_DISABLED);
    ui_label(top, "   ", VG_FONT_MD_LV, UI_C(TEXT));
    static const char *const TABS[2] = { "SMART", "Surface scan" };
    for (int i = 0; i < 2; ++i) {
        m->tab_btn[i] = ui_set_button(s, top, NULL, TABS[i]);
        lv_obj_set_user_data(m->tab_btn[i], (void *)(intptr_t)i);
        lv_obj_add_event_cb(m->tab_btn[i], tab_cb, LV_EVENT_CLICKED, m);
    }
    m->job = ui_set_note(pane, "", UI_C(TEXT2));
    lv_obj_add_flag(m->job, LV_OBJ_FLAG_HIDDEN);
    m->body = column(pane);
    lv_obj_set_style_pad_row(m->body, VG_SPACE_LG, 0);
    /* A scan under way opens on its map. */
    m->tab = scan_active(m->scan) ? TAB_SCAN : TAB_SMART;
    select_disk(m, m->n ? sel : -1);
    m->timer = lv_timer_create(tick, TICK_MS, m);
    return m;
}

void ui_maint_close(void *st)
{
    maint_t *m = st;
    if (!m) return;
    lv_timer_delete(m->timer);
    free(m->smart);
    free(m->scan);
    free(m);
}
