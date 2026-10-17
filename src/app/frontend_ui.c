/* SPDX-License-Identifier: GPL-3.0-or-later */
/* frontend_ui.c — the LVGL quad view on platforms with a display. All LVGL
 * calls happen on this (the main) thread: channel threads reach the UI only
 * through channels_status() snapshots and the events queue. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "lvgl.h"

#include "channel.h"
#include "events.h"
#include "frontend.h"
#include "hal.h"
#include "hal_ui.h"
#include "log.h"
#include "ui_events.h"
#include "ui_home.h"

static uint32_t tick_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* channel_status_t -> what a tile shows. */
static void status_cb(int idx, ui_chan_status_t *st)
{
    channel_status_t cs;
    channels_status(idx, &cs);
    st->live = cs.live && cs.fps > 0;
    st->recording = cs.recording;
    st->rec_bytes = cs.rec_bytes;
    if (!cs.configured)
        st->info[0] = 0;
    else if (cs.live && cs.width)
        snprintf(st->info, sizeof st->info, "%.20s  %s %ux%u  %u fps", cs.host,
                 vg_codec_name(cs.codec), cs.width, cs.height, cs.fps);
    else if (cs.live)
        snprintf(st->info, sizeof st->info, "%.20s  %u fps", cs.host, cs.fps);
    else
        snprintf(st->info, sizeof st->info, "%.20s  %.24s", cs.host, cs.state);
}

static void events_cb(lv_timer_t *t)
{
    (void)t;
    int ch;
    char text[40];
    while (events_pop(&ch, text, sizeof text)) {
        ui_events_push(ch, text);
        if (ch > 0) ui_home_flash_channel(ch);
    }
}

int frontend_run(int run_secs, volatile sig_atomic_t *stop)
{
    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *disp = hal_ui_create(SCREEN_W, SCREEN_H);
    if (!disp) {
        vg_log("no UI layer");
        lv_deinit();
        return -1;
    }
    const hal_caps_t *caps = hal_caps();
    ui_home_create(caps->capture ? "DVR" : "NVR", status_cb);
    lv_timer_create(events_cb, 200, NULL);

    time_t t0 = time(NULL);
    while (!*stop) {
        uint32_t ms = lv_timer_handler();
        usleep((ms > 50 ? 50 : ms) * 1000u);
        if (run_secs > 0 && time(NULL) - t0 >= run_secs) break;
    }
    lv_deinit();
    return 0;
}
