/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_events.c — bottom strip: ring buffer of the last EVENTS_MAX events.
 *
 * Each event is one short label like
 *   "14:32:55 CAM2 Motion"
 * Painted left-to-right, newest first, separated by faint vertical
 * dividers. When a new event arrives, the strip is repainted.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "ui_events.h"

#define EVENTS_MAX     6
#define EVENT_TEXT_MAX 48

typedef struct {
    char text[EVENT_TEXT_MAX];
} event_t;

static event_t   g_ring[EVENTS_MAX];
static int       g_count;            /* number of valid entries (0..EVENTS_MAX) */
static lv_obj_t *g_strip;
static lv_obj_t *g_entry_labels[EVENTS_MAX];

#define COL_BG_STRIP   lv_color_hex(0x0d0d0d)
#define COL_DIV        lv_color_hex(0x333333)
#define COL_TEXT       lv_color_hex(0xffffff)

static void repaint(void)
{
    /* Hide all labels first, then fill back-to-front from the ring. */
    for (int i = 0; i < EVENTS_MAX; ++i) {
        lv_label_set_text(g_entry_labels[i], "");
    }
    for (int i = 0; i < g_count; ++i) {
        lv_label_set_text(g_entry_labels[i], g_ring[i].text);
    }
}

void ui_events_push(int channel, const char *kind)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    /* Shift right — newest at index 0. */
    for (int i = EVENTS_MAX - 1; i > 0; --i) g_ring[i] = g_ring[i - 1];
    char who[16] = "SYS";
    if (channel > 0) snprintf(who, sizeof who, "CAM%d", channel);
    snprintf(g_ring[0].text, EVENT_TEXT_MAX,
             "%02d:%02d:%02d  %s  %s",
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             who, kind ? kind : "?");
    if (g_count < EVENTS_MAX) g_count++;
    if (g_strip) repaint();
}

void ui_events_create(lv_obj_t *parent, int h)
{
    g_strip = lv_obj_create(parent);
    lv_obj_set_size(g_strip, LV_PCT(100), h);
    lv_obj_align(g_strip, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(g_strip, 0, 0);
    lv_obj_set_style_margin_all(g_strip, 0, 0);
    lv_obj_set_style_border_color(g_strip, COL_DIV, 0);
    lv_obj_set_style_border_width(g_strip, 0, 0);
    lv_obj_set_style_border_side(g_strip, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(g_strip, 1, 0);
    lv_obj_set_style_radius(g_strip, 0, 0);
    lv_obj_set_style_bg_color(g_strip, COL_BG_STRIP, 0);
    lv_obj_set_style_bg_opa(g_strip, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_strip, LV_OBJ_FLAG_SCROLLABLE);

    /* Flex row with even spacing. */
    lv_obj_set_layout(g_strip, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(g_strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_strip, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(g_strip, 16, 0);
    lv_obj_set_style_pad_right(g_strip, 16, 0);
    lv_obj_set_style_pad_column(g_strip, 24, 0);

    for (int i = 0; i < EVENTS_MAX; ++i) {
        g_entry_labels[i] = lv_label_create(g_strip);
        lv_obj_set_flex_grow(g_entry_labels[i], 1);
        lv_label_set_text(g_entry_labels[i], "");
        lv_label_set_long_mode(g_entry_labels[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(g_entry_labels[i], COL_TEXT, 0);
        lv_obj_set_style_text_font(g_entry_labels[i],
                                   &lv_font_montserrat_14, 0);
    }

    /* Seed with a "Boot" event so the strip isn't blank on first paint. */
    ui_events_push(0, "Boot");
}
