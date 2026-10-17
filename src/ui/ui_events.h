/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_events.h — bottom event strip: ring buffer of last N events. */
#ifndef UI_EVENTS_H
#define UI_EVENTS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build the strip into `parent`, anchored to the bottom, height `h`. */
void ui_events_create(lv_obj_t *parent, int h);

/* Push a new event onto the front of the strip. `kind` is a short text
 * like "Motion" / "Alarm" / "Boot"; channel 0 is shown as "SYS".
 * LVGL thread only. */
void ui_events_push(int channel, const char *kind);

#ifdef __cplusplus
}
#endif

#endif /* UI_EVENTS_H */
