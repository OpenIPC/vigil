/* SPDX-License-Identifier: GPL-3.0-or-later */
/* frontend_ui.c — the LVGL UI on platforms with a display. All LVGL calls
 * happen on this (the main) thread: channel threads reach the UI only
 * through channels_status() snapshots and the events ring (ui_bridge.c);
 * a new configuration, from the API or from the UI's own settings pages,
 * is applied here too, between frames (tick). */
#define _DEFAULT_SOURCE
#include <time.h>
#include <unistd.h>

#include "lvgl.h"

#include "frontend.h"
#include "hal_ui.h"
#include "input_evdev.h"
#include "kvm_app.h"
#include "log.h"
#include "snapshot.h"
#include "ui_app.h"
#include "ui_bridge.h"

static uint32_t tick_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

int frontend_run(int run_secs, volatile sig_atomic_t *stop, void (*tick)(void))
{
    static ui_model_t model;
    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *disp = hal_ui_create(SCREEN_W, SCREEN_H);
    if (!disp || ui_bridge_init(&model) < 0) {
        vg_log("no UI layer");
        lv_deinit();
        return -1;
    }
    model.snapshot = snapshot_request;
    model.snapshot_result = snapshot_result;
    ui_app_create(&model);
    static const input_hooks_t hooks = { ui_app_back, ui_app_key, ui_app_keyboards, ui_app_wheel, ui_app_key_up };
    ui_app_set_pointer(input_evdev_create(disp, &hooks, 1));
    ui_app_set_keypad(input_evdev_keypad());
    kvm_attach(disp);                   /* the zero channel's picture and input (web.c) */

    time_t t0 = time(NULL);
    while (!*stop) {
        tick();
        /* Asleep until LVGL's next timer is due (its next frame at the
         * latest), not longer: a frame late is a pointer late. */
        uint32_t ms = lv_timer_handler();
        usleep((ms > LV_DEF_REFR_PERIOD ? LV_DEF_REFR_PERIOD : ms) * 1000u);
        if (run_secs > 0 && time(NULL) - t0 >= run_secs) break;
    }
    ui_app_flush();                     /* a layout chosen just before the stop */
    kvm_detach();
    input_evdev_delete();
    lv_deinit();
    return 0;
}
