/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_events.c — the event strip: the last EVENTS_MAX events, newest
 * first, left to right. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ui_events.h"
#include "ui_theme.h"

#define EVENTS_MAX 5

typedef struct {
    char time[12];
    char text[64];
} entry_t;

static entry_t   g_ring[EVENTS_MAX];
static int       g_count;
static lv_obj_t *g_strip;
static lv_obj_t *g_time[EVENTS_MAX], *g_text[EVENTS_MAX], *g_cell[EVENTS_MAX];

static void repaint(void)
{
    for (int i = 0; i < EVENTS_MAX; ++i) {
        if (i >= g_count) {
            lv_obj_add_flag(g_cell[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(g_cell[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_time[i], g_ring[i].time);
        lv_label_set_text(g_text[i], g_ring[i].text);
        lv_obj_set_style_text_color(g_text[i], i == 0 ? UI_C(TEXT) : UI_C(TEXT2), 0);
    }
}

void ui_events_push(int channel, const char *who, time_t when, const char *text)
{
    struct tm tm;
    localtime_r(&when, &tm);
    memmove(&g_ring[1], &g_ring[0], sizeof g_ring[0] * (EVENTS_MAX - 1));
    entry_t *e = &g_ring[0];
    snprintf(e->time, sizeof e->time, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    char w[40];
    if (who && *who) snprintf(w, sizeof w, "%s", who);
    else if (channel > 0) snprintf(w, sizeof w, "CAM%d", channel);
    else snprintf(w, sizeof w, "SYS");
    snprintf(e->text, sizeof e->text, "%.31s  %.30s", w, text ? text : "?");
    if (g_count < EVENTS_MAX) g_count++;
    if (g_strip) repaint();
}

void ui_events_create(lv_obj_t *parent, int h)
{
    g_strip = lv_obj_create(parent);
    ui_plain(g_strip);
    lv_obj_set_size(g_strip, LV_PCT(100), h);
    lv_obj_align(g_strip, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(g_strip, UI_C(PANEL), 0);
    lv_obj_set_style_bg_opa(g_strip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_strip, UI_C(LINE), 0);
    lv_obj_set_style_border_side(g_strip, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(g_strip, 1, 0);
    lv_obj_set_layout(g_strip, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(g_strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(g_strip, VG_SPACE_LG, 0);
    lv_obj_set_style_pad_column(g_strip, VG_SPACE_SM, 0);

    for (int i = 0; i < EVENTS_MAX; ++i) {
        lv_obj_t *c = lv_obj_create(g_strip);
        ui_plain(c);
        lv_obj_set_height(c, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(c, 1);
        lv_obj_set_style_pad_hor(c, VG_SPACE_MD, 0);
        lv_obj_set_style_border_color(c, UI_C(LINE), 0);
        lv_obj_set_style_border_side(c, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(c, i ? 1 : 0, 0);
        lv_obj_set_layout(c, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(c, 2, 0);
        g_time[i] = ui_label(c, "", VG_FONT_SM_LV, UI_C(TEXT2));
        g_text[i] = ui_label(c, "", VG_FONT_SM_LV, UI_C(TEXT));
        lv_obj_set_width(g_text[i], LV_PCT(100));
        lv_label_set_long_mode(g_text[i], LV_LABEL_LONG_DOT);
        g_cell[i] = c;
    }
    repaint();
}
