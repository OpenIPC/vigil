/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_events.h — the live view's event strip: the last few events. */
#ifndef UI_EVENTS_H
#define UI_EVENTS_H

#include <time.h>

#include "lvgl.h"

/* Build the strip into `parent`, anchored to the bottom, height `h`. */
void ui_events_create(lv_obj_t *parent, int h);

/* Put an event in front: "14:32:55  CAM2  offline"; channel 0 is "SYS".
 * `who` names the channel (NULL: "CAMn"). */
void ui_events_push(int channel, const char *who, time_t when, const char *text);

#endif /* UI_EVENTS_H */
