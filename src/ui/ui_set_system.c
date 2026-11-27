/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_set_system.c — System: General (device name, language, date and
 * time formats, the clock, the time zone with DST, NTP) and the HTTP
 * API's settings (docs/management.md).
 *
 * General is one page of several parts kept where they belong: the host
 * name and the zone in the system (mgmt calls), the language and the
 * formats in vigil.yaml's `ui` section, NTP in both. Save writes the
 * parts that changed, each on its own, and says which did not go. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "icons.h"
#include "ui_i18n.h"
#include "ui_settings.h"
#include "ui_theme.h"

static const ui_mgmt_t *MG(void) { return ui_model()->mgmt; }

/* Offsets that zones use, in minutes east of UTC. */
static const int k_offsets[] = { -720, -660, -600, -570, -540, -480, -420, -360, -300, -240, -210, -180, -120,
                                 -60,  0,    60,   120,  180,  210,  240,  270,  300,  330,  345,  360,  390,
                                 420,  480,  525,  540,  570,  600,  630,  660,  720,  765,  780,  840 };
#define N_OFFSETS (int)(sizeof k_offsets / sizeof k_offsets[0])
static const int k_dst_deltas[] = { 30, 60, 120 };
static const char *const k_months[] = { "January", "February", "March", "April", "May", "June", "July",
                                        "August", "September", "October", "November", "December" };
static const char *const k_weeks[] = { "1st", "2nd", "3rd", "4th", "last" };
static const char *const k_wdays[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
                                       "Saturday" };
static const char *const k_datefmts[] = { "YYYY-MM-DD", "DD.MM.YYYY", "MM/DD/YYYY" };
static const char *const k_timefmts[] = { "24h", "12h" };
static const char *const k_idle_values[] = { "0", "5", "10", "15", "30", "60" };
static const char *const k_idle_names[] = { "Never", "5 minutes", "10 minutes", "15 minutes", "30 minutes", "1 hour" };

static void off_text(int east, char *out, size_t cap)
{
    int a = east < 0 ? -east : east;
    snprintf(out, cap, "UTC%c%02d:%02d", east < 0 ? '-' : '+', a / 60, a % 60);
}

static int offset_index(int east)
{
    for (int i = 0; i < N_OFFSETS; ++i)
        if (k_offsets[i] == east) return i;
    return 14;                              /* UTC */
}

/* "the last Sunday of March 02:00" */
static void rule_text(int month, int week, int wday, int min, char *out, size_t cap)
{
    snprintf(out, cap, "the %s %s of %s %02d:%02d", k_weeks[(week < 1 ? 1 : week > 5 ? 5 : week) - 1],
             k_wdays[wday < 0 || wday > 6 ? 0 : wday], k_months[(month < 1 ? 1 : month > 12 ? 12 : month) - 1],
             min / 60, min % 60);
}

static void zone_text(const ui_tz_rule_t *r, char *out, size_t cap)
{
    char o[16], d[16], a[64], b[64];
    off_text(r->std_offset, o, sizeof o);
    if (!r->dst) {
        snprintf(out, cap, "%s, no daylight saving time", o);
        return;
    }
    off_text(r->dst_offset, d, sizeof d);
    rule_text(r->start_month, r->start_week, r->start_wday, r->start_min, a, sizeof a);
    rule_text(r->end_month, r->end_week, r->end_wday, r->end_min, b, sizeof b);
    snprintf(out, cap, "%s; daylight saving time %s from %s to %s", o, d, a, b);
}

/* "\n"-joined `n` strings. */
static void opts(const char *const *v, int n, char *out, size_t cap)
{
    out[0] = 0;
    for (int i = 0; i < n; ++i) {
        size_t l = strlen(out);
        snprintf(out + l, cap - l, "%s%s", i ? "\n" : "", v[i]);
    }
}

/* ---- General ----------------------------------------------------------------------- */

typedef struct {
    ui_set_t    *s;
    ui_time_t    t;
    char         host[64];
    int          n_zones, zone0;
    ui_tz_rule_t rule0;
    char         servers0[256];
    int          ui_sec;
    char         lang0[16], date0[16], time0[8], login0[4], idle0[8];
    lv_obj_t    *host_ta, *host_err;
    lv_obj_t    *lang_dd, *datefmt_dd, *timefmt_dd, *login_sw, *idle_dd;
    lv_obj_t    *now, *date_ta, *clock_ta;
    lv_obj_t    *zone_dd, *zone_desc, *custom, *off_dd, *dst_sw, *dst_box, *dstoff_dd;
    lv_obj_t    *rule_dd[2][3], *rule_ta[2], *preview;
    lv_obj_t    *ntp_sw, *srv_ta, *iv_ta, *sync_lbl, *ntp_job;
    lv_timer_t  *timer;
} gen_t;

static int dd_sel(lv_obj_t *d) { return (int)lv_dropdown_get_selected(d); }

static int hhmm(const char *s, int *min)
{
    int h, m;
    if (sscanf(s, "%d:%d", &h, &m) != 2 || h < 0 || h > 167 || m < 0 || m > 59) return -1;
    *min = h * 60 + m;
    return 0;
}

/* The custom zone the widgets describe; -1 when a time does not read. */
static int custom_rule(gen_t *g, ui_tz_rule_t *r)
{
    memset(r, 0, sizeof *r);
    r->std_offset = k_offsets[dd_sel(g->off_dd)];
    r->dst = lv_obj_has_state(g->dst_sw, LV_STATE_CHECKED);
    if (!r->dst) return 0;
    r->dst_offset = r->std_offset + k_dst_deltas[dd_sel(g->dstoff_dd)];
    r->start_month = dd_sel(g->rule_dd[0][0]) + 1;
    r->start_week = dd_sel(g->rule_dd[0][1]) + 1;
    r->start_wday = dd_sel(g->rule_dd[0][2]);
    r->end_month = dd_sel(g->rule_dd[1][0]) + 1;
    r->end_week = dd_sel(g->rule_dd[1][1]) + 1;
    r->end_wday = dd_sel(g->rule_dd[1][2]);
    if (hhmm(lv_textarea_get_text(g->rule_ta[0]), &r->start_min) < 0) return -1;
    if (hhmm(lv_textarea_get_text(g->rule_ta[1]), &r->end_min) < 0) return -1;
    return 0;
}

static int is_custom(gen_t *g) { return dd_sel(g->zone_dd) >= g->n_zones; }

/* The zone line, the custom part shown or not, the preview. */
static void zone_sync(gen_t *g)
{
    int custom = is_custom(g);
    if (custom) lv_obj_remove_flag(g->custom, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(g->custom, LV_OBJ_FLAG_HIDDEN);
    if (lv_obj_has_state(g->dst_sw, LV_STATE_CHECKED)) lv_obj_remove_flag(g->dst_box, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(g->dst_box, LV_OBJ_FLAG_HIDDEN);
    char text[256] = "";
    ui_tz_rule_t r;
    if (!custom) {
        const char *tz = MG()->zone_tz ? MG()->zone_tz(dd_sel(g->zone_dd)) : NULL;
        if (tz && MG()->tz_parse && MG()->tz_parse(tz, &r) == 0) zone_text(&r, text, sizeof text);
        lv_label_set_text(g->zone_desc, text);
        lv_obj_remove_flag(g->zone_desc, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(g->zone_desc, LV_OBJ_FLAG_HIDDEN);
    char tz[96], err[160];
    if (custom_rule(g, &r) < 0) {
        lv_label_set_text(g->preview, LV_SYMBOL_WARNING "  a time is HH:MM");
        lv_obj_set_style_text_color(g->preview, UI_C(BAD), 0);
    } else if (MG()->tz_build(&r, tz, sizeof tz, err, sizeof err) < 0) {
        lv_label_set_text_fmt(g->preview, LV_SYMBOL_WARNING "  %s", err);
        lv_obj_set_style_text_color(g->preview, UI_C(BAD), 0);
    } else {
        zone_text(&r, text, sizeof text);
        lv_label_set_text_fmt(g->preview, "%s\nPOSIX: %s", text, tz);
        lv_obj_set_style_text_color(g->preview, UI_C(TEXT2), 0);
    }
}

static void zone_cb(lv_event_t *e) { zone_sync(lv_event_get_user_data(e)); }

static void now_text(gen_t *g)
{
    time_t now = ui_model()->now ? ui_model()->now() : time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char b[64];
    ui_format_datetime(&tm, 1, b, sizeof b);
    lv_label_set_text(g->now, b);
}

static void tick(lv_timer_t *t) { now_text(lv_timer_get_user_data(t)); }

static void servers_text(const ui_time_t *t, char *out, size_t cap)
{
    out[0] = 0;
    for (int i = 0; i < t->n_servers; ++i) {
        size_t l = strlen(out);
        snprintf(out + l, cap - l, "%s%s", i ? ", " : "", t->servers[i]);
    }
}

static void sync_line(gen_t *g)
{
    MG()->time_get(&g->t);
    char when[48];
    ui_set_time_text(g->t.ntp_last_ok, when, sizeof when);
    if (g->t.ntp_last_try)
        lv_label_set_text_fmt(g->sync_lbl, "Last synchronised: %s (%s)", when, g->t.ntp_result);
    else lv_label_set_text(g->sync_lbl, "Last synchronised: never");
}

static void sync_done(void *user, int ok, const char *msg)
{
    gen_t *g = user;
    (void)msg;
    ui_toast(ok ? UI_TOAST_OK : UI_TOAST_BAD, ok ? "Clock synchronised" : "NTP: %s", msg);
    sync_line(g);
}

static void sync_cb(lv_event_t *e)
{
    gen_t *g = lv_event_get_user_data(e);
    int id = MG()->ntp_sync ? MG()->ntp_sync() : -1;
    if (id <= 0) {
        ui_toast(UI_TOAST_BAD, id == UI_MGMT_EUNMANAGED ? "vigild does not manage this system" : "Cannot sync now");
        return;
    }
    ui_set_follow_job(g->s, id, g->ntp_job, sync_done, g);
}

static void set_clock_cb(lv_event_t *e)
{
    gen_t *g = lv_event_get_user_data(e);
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    if (sscanf(lv_textarea_get_text(g->date_ta), "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3 ||
        sscanf(lv_textarea_get_text(g->clock_ta), "%d:%d:%d", &tm.tm_hour, &tm.tm_min, &tm.tm_sec) < 2 ||
        tm.tm_mon < 1 || tm.tm_mon > 12 || tm.tm_mday < 1 || tm.tm_mday > 31 || tm.tm_hour > 23 || tm.tm_min > 59 ||
        tm.tm_sec > 60) {
        ui_toast(UI_TOAST_BAD, "The date is YYYY-MM-DD, the time HH:MM:SS");
        return;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    char err[160] = "";
    int rc = MG()->time_set(NULL, NULL, 1, (long long)t, err, sizeof err);
    if (rc < 0) ui_toast(UI_TOAST_BAD, "Clock not set: %s", rc == UI_MGMT_EUNMANAGED ? "vigild does not manage this system" : err);
    else ui_toast(UI_TOAST_OK, "Clock set");
}

static lv_obj_t *dd_of(ui_set_t *s, lv_obj_t *box, const char *const *v, int n, int sel, int w)
{
    char o[512];
    opts(v, n, o, sizeof o);
    return ui_set_dropdown(s, box, o, sel, w);
}

/* The `ui` section's field `key`, as stored. */
static void ui_value(int sec, const char *key, char *out, size_t cap)
{
    out[0] = 0;
    if (sec < 0) return;
    const ui_section_t *u = &ui_model()->sections[sec];
    for (int f = 0; f < u->n_fields; ++f)
        if (!strcmp(u->fields[f].key, key)) ui_model()->get_value(sec, -1, f, out, cap);
}

static int index_of(const char *const *v, int n, const char *x)
{
    for (int i = 0; i < n; ++i)
        if (!strcmp(v[i], x)) return i;
    return 0;
}

static void load(gen_t *g)
{
    MG()->time_get(&g->t);
    MG()->hostname_get(g->host, sizeof g->host);
    lv_textarea_set_text(g->host_ta, g->host);
    lv_obj_add_flag(g->host_err, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_state(g->host_ta, LV_STATE_USER_1);

    ui_value(g->ui_sec, "language", g->lang0, sizeof g->lang0);
    ui_value(g->ui_sec, "date_format", g->date0, sizeof g->date0);
    ui_value(g->ui_sec, "time_format", g->time0, sizeof g->time0);
    ui_value(g->ui_sec, "login", g->login0, sizeof g->login0);
    ui_value(g->ui_sec, "idle_logout", g->idle0, sizeof g->idle0);
    if (!strcmp(g->login0, "1")) lv_obj_add_state(g->login_sw, LV_STATE_CHECKED);
    else lv_obj_remove_state(g->login_sw, LV_STATE_CHECKED);
    lv_dropdown_set_selected(g->idle_dd, (uint32_t)index_of(k_idle_values, 6, g->idle0));
    int li = 0;
    for (int i = 0; ui_i18n_code(i); ++i)
        if (!strcmp(ui_i18n_code(i), g->lang0)) li = i;
    lv_dropdown_set_selected(g->lang_dd, (uint32_t)li);
    lv_dropdown_set_selected(g->datefmt_dd, (uint32_t)index_of(k_datefmts, 3, g->date0));
    lv_dropdown_set_selected(g->timefmt_dd, (uint32_t)index_of(k_timefmts, 2, g->time0));

    /* The zone: a named one, else Custom from the stored string. */
    g->zone0 = g->n_zones;
    for (int i = 0; i < g->n_zones; ++i)
        if (!strcmp(MG()->zone_name(i), g->t.zone)) g->zone0 = i;
    memset(&g->rule0, 0, sizeof g->rule0);
    g->rule0 = (ui_tz_rule_t){ 0, 0, 60, 3, 5, 0, 120, 10, 5, 0, 180 };
    ui_tz_rule_t r;
    if (g->t.tz[0] && MG()->tz_parse(g->t.tz, &r) == 0) {
        g->rule0 = r;
        if (!r.dst) {                           /* the DST part's defaults stay */
            g->rule0.start_month = 3, g->rule0.start_week = 5, g->rule0.start_min = 120;
            g->rule0.end_month = 10, g->rule0.end_week = 5, g->rule0.end_min = 180;
        }
    }
    lv_dropdown_set_selected(g->zone_dd, (uint32_t)g->zone0);
    lv_dropdown_set_selected(g->off_dd, (uint32_t)offset_index(g->rule0.std_offset));
    if (g->rule0.dst) lv_obj_add_state(g->dst_sw, LV_STATE_CHECKED);
    else lv_obj_remove_state(g->dst_sw, LV_STATE_CHECKED);
    int delta = g->rule0.dst ? g->rule0.dst_offset - g->rule0.std_offset : 60;
    lv_dropdown_set_selected(g->dstoff_dd, delta == 30 ? 0 : delta == 120 ? 2 : 1);
    int rr[2][4] = { { g->rule0.start_month, g->rule0.start_week, g->rule0.start_wday, g->rule0.start_min },
                     { g->rule0.end_month, g->rule0.end_week, g->rule0.end_wday, g->rule0.end_min } };
    for (int k = 0; k < 2; ++k) {
        lv_dropdown_set_selected(g->rule_dd[k][0], (uint32_t)(rr[k][0] > 0 ? rr[k][0] - 1 : 0));
        lv_dropdown_set_selected(g->rule_dd[k][1], (uint32_t)(rr[k][1] > 0 ? rr[k][1] - 1 : 0));
        lv_dropdown_set_selected(g->rule_dd[k][2], (uint32_t)rr[k][2]);
        char hm[8];
        snprintf(hm, sizeof hm, "%02d:%02d", rr[k][3] / 60 % 100, rr[k][3] % 60);
        lv_textarea_set_text(g->rule_ta[k], hm);
    }
    /* Remember what the widgets show now: dirty compares against them. */
    if (g->zone0 == g->n_zones) custom_rule(g, &g->rule0);

    if (g->t.ntp) lv_obj_add_state(g->ntp_sw, LV_STATE_CHECKED);
    else lv_obj_remove_state(g->ntp_sw, LV_STATE_CHECKED);
    servers_text(&g->t, g->servers0, sizeof g->servers0);
    lv_textarea_set_text(g->srv_ta, g->servers0);
    char iv[16];
    snprintf(iv, sizeof iv, "%d", g->t.ntp_interval);
    lv_textarea_set_text(g->iv_ta, iv);
    lv_obj_remove_state(g->srv_ta, LV_STATE_USER_1);
    sync_line(g);
    zone_sync(g);
}

static void *gen_open(ui_set_t *s, lv_obj_t *pane)
{
    const ui_mgmt_t *m = MG();
    if (!m || !m->time_get || !m->hostname_get || !m->zones) {
        ui_set_hide_actions(s);
        ui_set_note(pane, "Not available in this build.", UI_C(WARN));
        return NULL;
    }
    gen_t *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->s = s;
    g->n_zones = m->zones();
    g->ui_sec = ui_set_section_index("ui");
    if (m->managed && !m->managed())
        ui_set_note(pane, LV_SYMBOL_WARNING "  vigild does not manage this system (it is not OpenIPC firmware): "
                          "the device name, the clock, the time zone and NTP are shown, not changed.", UI_C(WARN));

    lv_obj_t *sec = ui_set_section(pane, "Device");
    lv_obj_t *box = ui_set_row(sec, TR(S_DEVICE_NAME), "The host name: on the network, in the HTTP API's status.");
    g->host_ta = ui_set_text(s, box, "", 63, 320);
    g->host_err = ui_set_note(lv_obj_get_parent(box), "", UI_C(BAD));

    sec = ui_set_section(pane, "Language and formats");
    char o[256] = "";
    for (int i = 0; ui_i18n_name(i); ++i) {
        size_t l = strlen(o);
        snprintf(o + l, sizeof o - l, "%s%s", i ? "\n" : "", ui_i18n_name(i));
    }
    g->lang_dd = ui_set_dropdown(s, ui_set_row(sec, TR(S_LANGUAGE), "The languages the UI has been translated to."),
                                 o, 0, 240);
    g->datefmt_dd = dd_of(s, ui_set_row(sec, "Date format", NULL), k_datefmts, 3, 0, 240);
    g->timefmt_dd = ui_set_dropdown(s, ui_set_row(sec, "Time format", NULL), "24-hour\n12-hour", 0, 240);

    sec = ui_set_section(pane, TR(S_DATE_TIME));
    g->now = ui_set_info(sec, "Now", "");
    box = ui_set_row(sec, "Set the clock", NULL);
    time_t now = ui_model()->now ? ui_model()->now() : time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char d[16], t[16];
    snprintf(d, sizeof d, "%04d-%02d-%02d", (tm.tm_year + 1900) % 10000, (tm.tm_mon + 1) % 100, tm.tm_mday % 100);
    snprintf(t, sizeof t, "%02d:%02d:%02d", tm.tm_hour % 100, tm.tm_min % 100, tm.tm_sec % 100);
    g->date_ta = ui_set_text(s, box, d, 10, 150);
    g->clock_ta = ui_set_text(s, box, t, 8, 120);
    lv_obj_t *set = ui_set_button(s, box, LV_SYMBOL_OK, "Set");
    lv_obj_add_event_cb(set, set_clock_cb, LV_EVENT_CLICKED, g);
    ui_set_note(lv_obj_get_parent(box), "Set at once, written to the RTC. With NTP on, the next synchronisation "
                                        "corrects it.", UI_C(TEXT2));

    sec = ui_set_section(pane, TR(S_TIME_ZONE));
    char *zo = malloc(8192);
    if (zo) {
        zo[0] = 0;
        for (int i = 0; i < g->n_zones; ++i) {
            size_t l = strlen(zo);
            snprintf(zo + l, 8192 - l, "%s\n", m->zone_name(i));
        }
        size_t l = strlen(zo);
        snprintf(zo + l, 8192 - l, "Custom (offset and DST)");
    }
    box = ui_set_row(sec, TR(S_TIME_ZONE), NULL);
    g->zone_dd = ui_set_dropdown(s, box, zo ? zo : "", 0, 360);
    free(zo);
    lv_obj_add_event_cb(g->zone_dd, zone_cb, LV_EVENT_VALUE_CHANGED, g);
    g->zone_desc = ui_set_note(lv_obj_get_parent(box), "", UI_C(TEXT2));

    g->custom = lv_obj_create(sec);
    ui_plain(g->custom);
    ui_ring_room(g->custom);
    lv_obj_set_size(g->custom, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(g->custom, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(g->custom, LV_FLEX_FLOW_COLUMN);
    char offs[1024] = "";
    for (int i = 0; i < N_OFFSETS; ++i) {
        char x[16];
        off_text(k_offsets[i], x, sizeof x);
        size_t l = strlen(offs);
        snprintf(offs + l, sizeof offs - l, "%s%s", i ? "\n" : "", x);
    }
    g->off_dd = ui_set_dropdown(s, ui_set_row(g->custom, "UTC offset", NULL), offs, 14, 200);
    lv_obj_add_event_cb(g->off_dd, zone_cb, LV_EVENT_VALUE_CHANGED, g);
    g->dst_sw = ui_set_switch(s, ui_set_row(g->custom, TR(S_DST), NULL), 0);
    lv_obj_add_event_cb(g->dst_sw, zone_cb, LV_EVENT_VALUE_CHANGED, g);
    g->dst_box = lv_obj_create(g->custom);
    ui_plain(g->dst_box);
    ui_ring_room(g->dst_box);
    lv_obj_set_size(g->dst_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(g->dst_box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(g->dst_box, LV_FLEX_FLOW_COLUMN);
    g->dstoff_dd = ui_set_dropdown(s, ui_set_row(g->dst_box, "Clocks go forward", NULL), "30 minutes\n1 hour\n2 hours",
                                   1, 200);
    lv_obj_add_event_cb(g->dstoff_dd, zone_cb, LV_EVENT_VALUE_CHANGED, g);
    static const char *const which[] = { "Starts", "Ends" };
    for (int k = 0; k < 2; ++k) {
        box = ui_set_row(g->dst_box, which[k], k ? "Local time, as the clock shows it then." : NULL);
        g->rule_dd[k][1] = dd_of(s, box, k_weeks, 5, 4, 100);
        g->rule_dd[k][2] = dd_of(s, box, k_wdays, 7, 0, 150);
        ui_label(box, "of", VG_FONT_MD_LV, UI_C(TEXT2));
        g->rule_dd[k][0] = dd_of(s, box, k_months, 12, 0, 150);
        ui_label(box, "at", VG_FONT_MD_LV, UI_C(TEXT2));
        g->rule_ta[k] = ui_set_text(s, box, "02:00", 5, 90);
        for (int j = 0; j < 3; ++j) lv_obj_add_event_cb(g->rule_dd[k][j], zone_cb, LV_EVENT_VALUE_CHANGED, g);
        lv_obj_add_event_cb(g->rule_ta[k], zone_cb, LV_EVENT_VALUE_CHANGED, g);
    }
    g->preview = ui_set_note(g->custom, "", UI_C(TEXT2));

    sec = ui_set_section(pane, TR(S_NTP));
    g->ntp_sw = ui_set_switch(s, ui_set_row(sec, "Synchronise with NTP",
                                            "The firmware's ntpd keeps the clock from these servers."), 1);
    g->srv_ta = ui_set_text(s, ui_set_row(sec, "Servers", "Up to four, separated by commas."), "", 255, 0);
    g->iv_ta = ui_set_number(s, ui_set_row(sec, "Check every (min)", "How often vigild checks the clock itself."),
                             60, 10, 1440);
    box = ui_set_row(sec, TR(S_SYNC_NOW), NULL);
    lv_obj_t *sync = ui_set_button(s, box, LV_SYMBOL_REFRESH, TR(S_SYNC_NOW));
    lv_obj_add_event_cb(sync, sync_cb, LV_EVENT_CLICKED, g);
    g->ntp_job = ui_label(box, "", VG_FONT_SM_LV, UI_C(TEXT2));
    g->sync_lbl = ui_set_note(lv_obj_get_parent(box), "", UI_C(TEXT2));

    sec = ui_set_section(pane, TR(S_LOG_IN));
    g->login_sw = ui_set_switch(s, ui_set_row(sec, "Log in on screen",
                                              "The menus and settings ask for a user and its password "
                                              "(Account); the live view stays. The system account always "
                                              "logs in, so nobody is locked out."), 0);
    g->idle_dd = dd_of(s, ui_set_row(sec, "Log out after", "Without a key or the mouse for this long."),
                       k_idle_names, 6, 0, 240);

    load(g);
    now_text(g);
    g->timer = lv_timer_create(tick, 1000, g);
    return g;
}

/* What each part would save, and whether it changed. */
static int host_dirty(gen_t *g) { return strcmp(lv_textarea_get_text(g->host_ta), g->host) != 0; }

static int ui_dirty(gen_t *g)
{
    const char *lang = ui_i18n_code(dd_sel(g->lang_dd));
    return strcmp(lang ? lang : "en", g->lang0) || strcmp(k_datefmts[dd_sel(g->datefmt_dd)], g->date0) ||
           strcmp(k_timefmts[dd_sel(g->timefmt_dd)], g->time0) ||
           strcmp(lv_obj_has_state(g->login_sw, LV_STATE_CHECKED) ? "1" : "0", g->login0) ||
           strcmp(k_idle_values[dd_sel(g->idle_dd)], g->idle0);
}

static int zone_dirty(gen_t *g)
{
    if (dd_sel(g->zone_dd) != g->zone0) return 1;
    if (!is_custom(g)) return 0;
    ui_tz_rule_t r;
    if (custom_rule(g, &r) < 0) return 1;
    return memcmp(&r, &g->rule0, sizeof r) != 0;
}

static int ntp_dirty(gen_t *g)
{
    return lv_obj_has_state(g->ntp_sw, LV_STATE_CHECKED) != !!g->t.ntp ||
           strcmp(lv_textarea_get_text(g->srv_ta), g->servers0) || ui_set_number_get(g->iv_ta) != g->t.ntp_interval;
}

static int gen_dirty(void *st)
{
    gen_t *g = st;
    return g && (host_dirty(g) || ui_dirty(g) || zone_dirty(g) || ntp_dirty(g));
}

static const char *why(int rc, const char *err)
{
    return rc == UI_MGMT_EUNMANAGED ? "vigild does not manage this system" : err;
}

static int gen_save(void *st)
{
    gen_t *g = st;
    if (!g) return 0;
    int failed = 0;
    char err[200];
    lv_obj_add_flag(g->host_err, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_state(g->host_ta, LV_STATE_USER_1);
    lv_obj_remove_state(g->srv_ta, LV_STATE_USER_1);

    if (host_dirty(g)) {
        err[0] = 0;
        int rc = MG()->hostname_set(lv_textarea_get_text(g->host_ta), err, sizeof err);
        if (rc < 0) {
            lv_label_set_text_fmt(g->host_err, LV_SYMBOL_CLOSE "  %s", why(rc, err));
            lv_obj_remove_flag(g->host_err, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_state(g->host_ta, LV_STATE_USER_1);
            ui_toast(UI_TOAST_BAD, "Device name not saved: %s", why(rc, err));
            failed = 1;
        }
    }
    if (ui_dirty(g) && g->ui_sec >= 0) {
        const ui_section_t *u = &ui_model()->sections[g->ui_sec];
        char vals[8][32];
        const char *v[8];
        unsigned gen = ui_model()->config_generation ? ui_model()->config_generation() : 0;
        for (int f = 0; f < u->n_fields && f < 8; ++f) {
            ui_model()->get_value(g->ui_sec, -1, f, vals[f], sizeof vals[f]);
            const char *k = u->fields[f].key;
            if (!strcmp(k, "language")) snprintf(vals[f], sizeof vals[f], "%s", ui_i18n_code(dd_sel(g->lang_dd)));
            else if (!strcmp(k, "date_format")) snprintf(vals[f], sizeof vals[f], "%s", k_datefmts[dd_sel(g->datefmt_dd)]);
            else if (!strcmp(k, "time_format")) snprintf(vals[f], sizeof vals[f], "%s", k_timefmts[dd_sel(g->timefmt_dd)]);
            else if (!strcmp(k, "login"))
                snprintf(vals[f], sizeof vals[f], "%s", lv_obj_has_state(g->login_sw, LV_STATE_CHECKED) ? "1" : "0");
            else if (!strcmp(k, "idle_logout")) snprintf(vals[f], sizeof vals[f], "%s", k_idle_values[dd_sel(g->idle_dd)]);
            v[f] = vals[f];
        }
        ui_errors_t errs;
        memset(&errs, 0, sizeof errs);
        ui_save_rc_t rc = ui_model()->save(g->ui_sec, 1, v, NULL, gen, &errs);
        if (rc != UI_SAVE_OK && rc != UI_SAVE_UNSYNCED) {
            ui_toast(UI_TOAST_BAD, "Language and formats not saved: %s", errs.n ? errs.e[0].msg : "error");
            failed = 1;
        }
    }
    if (zone_dirty(g)) {
        err[0] = 0;
        int rc;
        if (is_custom(g)) {
            ui_tz_rule_t r;
            char tz[96];
            if (custom_rule(g, &r) < 0) {
                snprintf(err, sizeof err, "a time is HH:MM");
                rc = UI_MGMT_EINVAL;
            } else if (MG()->tz_build(&r, tz, sizeof tz, err, sizeof err) < 0) {
                rc = UI_MGMT_EINVAL;
            } else {
                rc = MG()->time_set("", tz, 0, 0, err, sizeof err);
            }
        } else {
            rc = MG()->time_set(MG()->zone_name(dd_sel(g->zone_dd)), NULL, 0, 0, err, sizeof err);
        }
        if (rc < 0) {
            ui_toast(UI_TOAST_BAD, "Time zone not saved: %s", why(rc, err));
            failed = 1;
        }
    }
    if (ntp_dirty(g)) {
        err[0] = 0;
        int rc = MG()->ntp_set(lv_obj_has_state(g->ntp_sw, LV_STATE_CHECKED), lv_textarea_get_text(g->srv_ta),
                               ui_set_number_get(g->iv_ta), err, sizeof err);
        if (rc < 0) {
            lv_obj_add_state(g->srv_ta, LV_STATE_USER_1);
            ui_toast(UI_TOAST_BAD, "NTP not saved: %s", why(rc, err));
            failed = 1;
        }
    }
    if (failed) return -1;
    ui_toast(UI_TOAST_OK, "%s", TR(S_SAVED));
    load(g);
    return 0;
}

static void gen_cancel(void *st)
{
    if (st) load(st);
}

static void gen_close(void *st)
{
    gen_t *g = st;
    if (!g) return;
    lv_timer_delete(g->timer);
    free(g);
}

/* ---- HTTP API ------------------------------------------------------------------------ */

typedef struct {
    ui_form_t *form;
} api_t;

static void *api_open(ui_set_t *s, lv_obj_t *pane)
{
    api_t *a = calloc(1, sizeof *a);
    if (a) a->form = ui_set_form(s, pane, "api");
    return a;
}
static int api_dirty(void *st) { return ui_form_embed_dirty(((api_t *)st)->form); }
static int api_save(void *st) { return ui_form_embed_save(((api_t *)st)->form); }
static void api_cancel(void *st) { ui_form_embed_reset(((api_t *)st)->form); }
static void api_close(void *st)
{
    ui_form_embed_free(((api_t *)st)->form);
    free(st);
}

static ui_set_item_t g_items[2];

void ui_sysset_open(int first)
{
    const ui_set_item_t items[] = {
        { TR(S_GENERAL), &ui_icon_nav_general, gen_open, gen_dirty, gen_save, gen_cancel, gen_close },
        { "HTTP API", &ui_icon_nav_ethernet, api_open, api_dirty, api_save, api_cancel, api_close },
    };
    memcpy(g_items, items, sizeof items);
    ui_settings_open(TR(S_SYSTEM), g_items, 2, first);
}
