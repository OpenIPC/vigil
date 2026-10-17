/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_home.h — DVR home screen: quad-view + chrome + spotlight. */
#ifndef UI_HOME_H
#define UI_HOME_H

#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_HOME_CHANNELS  4

/* What a tile shows about its channel; filled by the role's status source
 * once a second (on the LVGL thread). */
typedef struct {
    int      live;          /* video is arriving */
    int      recording;     /* the channel is being written to storage */
    uint64_t rec_bytes;     /* written this session */
    char     info[48];      /* one line under the label, e.g. "H.265  25 fps" */
} ui_chan_status_t;

/* Status source: fill `st` for tile `idx` (0..UI_HOME_CHANNELS-1). */
typedef void (*ui_status_fn)(int idx, ui_chan_status_t *st);

/* Build the home screen on the active display. `role` is shown in the
 * header ("DVR" / "NVR"); `status` is polled once a second. */
void ui_home_create(const char *role, ui_status_fn status);

/* Trigger a 3-second yellow border flash on the given channel (1..4) to
 * indicate a motion / alarm event on that tile. */
void ui_home_flash_channel(int ch);

#ifdef __cplusplus
}
#endif

#endif /* UI_HOME_H */
