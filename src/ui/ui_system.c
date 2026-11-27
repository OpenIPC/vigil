/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_system.c — System: what the box is and how it is doing, read-only.
 * Refreshed every second while shown (uptime, memory). */
#include <stdio.h>
#include <string.h>

#include "ui_app.h"
#include "ui_layout.h"
#include "ui_theme.h"

#define INFO_MAX 16

typedef struct {
    lv_obj_t *value[INFO_MAX];
    int       n;
} sys_t;

/* Label and value rows in `panel`; the value labels into `value` (may be NULL). */
static void info_rows(lv_obj_t *panel, const ui_info_t *info, int n, lv_obj_t **value)
{
    for (int i = 0; i < n; ++i) {
        lv_obj_t *r = lv_obj_create(panel);
        ui_plain(r);
        ui_inert(r);
        lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_ver(r, VG_SPACE_MD, 0);
        lv_obj_set_style_border_color(r, UI_C(LINE), 0);
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(r, i ? 1 : 0, 0);
        lv_obj_set_layout(r, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_t *k = ui_label(r, info[i].label, VG_FONT_MD_LV, UI_C(TEXT2));
        lv_obj_set_width(k, 320);
        lv_obj_t *v = ui_label(r, info[i].value, VG_FONT_MD_LV, UI_C(TEXT));
        lv_obj_set_flex_grow(v, 1);
        lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
        if (value) value[i] = v;
    }
}

static lv_obj_t *info_panel(ui_page_t *p)
{
    lv_obj_t *panel = lv_obj_create(p->body);
    ui_plain(panel);
    lv_obj_add_style(panel, &ui_st_panel, 0);
    lv_obj_set_size(panel, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, 0, 0);
    return panel;
}

static void sys_fill(sys_t *s)
{
    ui_info_t info[INFO_MAX];
    int n = ui_model()->system_info ? ui_model()->system_info(info, INFO_MAX) : 0;
    for (int i = 0; i < n && i < s->n; ++i) lv_label_set_text(s->value[i], info[i].value);
}

static void sys_tick_cb(lv_timer_t *t)
{
    sys_fill(lv_timer_get_user_data(t));
}

static void sys_close(ui_page_t *p)
{
    lv_timer_t *t = lv_obj_get_user_data(p->body);
    if (t) lv_timer_delete(t);
    lv_free(p->user);
}

/* ---- Channel info: every channel's state and stream --------------------------------------- */

#define CI_COLS 9
static const char *const k_ci_head[CI_COLS] = { "#", "Name", "State", "Stream", "Codec", "Resolution",
                                                "FPS", "Recording", "Events" };
static const int k_ci_w[CI_COLS] = { 40, 190, 190, 80, 80, 120, 60, 170, 150 };

typedef struct {
    lv_obj_t *cell[UI_MAX_CHANNELS][CI_COLS];
    int       n;
} ci_t;

const char *ui_chan_status_word(const ui_chan_t *st)
{
    switch (st->status) {
    case UI_CH_EMPTY:      return "-";
    case UI_CH_DISABLED:   return "Disabled";
    case UI_CH_CONNECTING: return "Connecting";
    case UI_CH_LIVE:       return st->no_decoder ? "Live, no decoder" : "Live";
    case UI_CH_NO_VIDEO:   return "No video";
    case UI_CH_OFFLINE:    return "Offline";
    case UI_CH_AUTH:       return "Authentication error";
    case UI_CH_CODEC:      return "Unsupported codec";
    default:               return st->state[0] ? st->state : "Error";
    }
}

static void ci_fill(ci_t *ci)
{
    static const char *const ev[] = { "motion", "human", "face", "vehicle" };
    for (int ch = 0; ch < ci->n; ++ch) {
        const ui_chan_t *st = ui_channel(ch);
        char v[CI_COLS][64];
        int live = st->status == UI_CH_LIVE;
        snprintf(v[0], sizeof v[0], "%d", ch + 1);
        if (st->status == UI_CH_EMPTY) snprintf(v[1], sizeof v[1], "-");
        else ui_chan_title(ch, v[1], sizeof v[1]);
        snprintf(v[2], sizeof v[2], "%s", ui_chan_status_word(st));
        snprintf(v[3], sizeof v[3], "%s", live && st->view[0] ? st->view : "-");
        snprintf(v[4], sizeof v[4], "%s", live && st->codec[0] ? st->codec : "-");
        if (live && st->width) snprintf(v[5], sizeof v[5], "%ux%u", st->width, st->height);
        else snprintf(v[5], sizeof v[5], "-");
        if (live) snprintf(v[6], sizeof v[6], "%u", st->fps);
        else snprintf(v[6], sizeof v[6], "-");
        snprintf(v[7], sizeof v[7], "%s", st->rec_state[0] ? st->rec_state : "off");
        v[8][0] = 0;
        for (int b = 0; b < 4; ++b)
            if (st->events & (1u << b)) {
                size_t n = strlen(v[8]);
                snprintf(v[8] + n, sizeof v[8] - n, "%s%s", n ? ", " : "", ev[b]);
            }
        if (!v[8][0]) snprintf(v[8], sizeof v[8], "-");
        for (int c = 0; c < CI_COLS; ++c)
            if (strcmp(lv_label_get_text(ci->cell[ch][c]), v[c])) lv_label_set_text(ci->cell[ch][c], v[c]);
        lv_color_t col = st->status == UI_CH_LIVE ? UI_C(OK)
                       : st->status == UI_CH_EMPTY || st->status == UI_CH_DISABLED ? UI_C(TEXT2)
                       : st->status == UI_CH_CONNECTING ? UI_C(WARN) : UI_C(BAD);
        lv_obj_set_style_text_color(ci->cell[ch][2], col, 0);
    }
}

static void ci_tick_cb(lv_timer_t *t) { ci_fill(lv_timer_get_user_data(t)); }

static void ci_close(ui_page_t *p)
{
    lv_timer_t *t = lv_obj_get_user_data(p->body);
    if (t) lv_timer_delete(t);
    lv_free(p->user);
}

static lv_obj_t *ci_row(lv_obj_t *table, int header)
{
    lv_obj_t *r = lv_obj_create(table);
    ui_plain(r);
    ui_inert(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(r, header ? VG_SPACE_SM : VG_SPACE_XS, 0);
    lv_obj_set_style_border_color(r, UI_C(LINE), 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_layout(r, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    return r;
}

void ui_channel_info_open(void)
{
    ci_t *ci = lv_malloc_zeroed(sizeof *ci);
    if (!ci) return;
    ui_page_t *p = ui_page_push("Channel info");
    if (!p) {
        lv_free(ci);
        return;
    }
    p->user = ci;
    p->on_close = ci_close;
    ci->n = ui_model()->channels < UI_MAX_CHANNELS ? ui_model()->channels : UI_MAX_CHANNELS;
    lv_obj_t *table = info_panel(p);
    lv_obj_set_style_pad_ver(table, VG_SPACE_SM, 0);
    lv_obj_t *r = ci_row(table, 1);
    for (int c = 0; c < CI_COLS; ++c) {
        lv_obj_t *l = ui_label(r, k_ci_head[c], VG_FONT_SM_LV, UI_C(TEXT2));
        lv_obj_set_width(l, k_ci_w[c]);
    }
    for (int ch = 0; ch < ci->n; ++ch) {
        r = ci_row(table, 0);
        for (int c = 0; c < CI_COLS; ++c) {
            lv_obj_t *l = ui_label(r, "", VG_FONT_SM_LV, UI_C(TEXT));
            lv_obj_set_width(l, k_ci_w[c]);
            lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
            ci->cell[ch][c] = l;
        }
    }
    ui_label(p->body, "Stream: the camera's stream on the screen (main, or sub in a small tile); codec, "
                      "resolution and FPS are that stream's. Events are held a few seconds.",
             VG_FONT_SM_LV, UI_C(TEXT2));
    ci_fill(ci);
    lv_obj_set_user_data(p->body, lv_timer_create(ci_tick_cb, 1000, ci));
    ui_page_ready(p);
}

static void channel_info_cb(lv_event_t *e)
{
    (void)e;
    ui_channel_info_open();
}

void ui_system_open(void)
{
    sys_t *s = lv_malloc_zeroed(sizeof *s);
    if (!s) return;
    ui_page_t *p = ui_page_push("System");
    if (!p) {
        lv_free(s);
        return;
    }
    p->user = s;
    p->on_close = sys_close;

    lv_obj_t *panel = info_panel(p);
    ui_info_t info[INFO_MAX];
    int n = ui_model()->system_info ? ui_model()->system_info(info, INFO_MAX) : 0;
    info_rows(panel, info, n, s->value);
    s->n = n;
    lv_obj_set_user_data(p->body, lv_timer_create(sys_tick_cb, 1000, s));
    lv_obj_t *ci_btn = ui_btn(p->actions, LV_SYMBOL_LIST, "Channel info");
    lv_obj_add_event_cb(ci_btn, channel_info_cb, LV_EVENT_CLICKED, NULL);
    ui_page_ready(p);
}
