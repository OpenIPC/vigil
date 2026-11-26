/* SPDX-License-Identifier: GPL-3.0-or-later */
/* input_evdev.h — mice and touch screens (Linux evdev) as one LVGL
 * pointer, with the right button reported separately (see input_evdev.c). */
#ifndef INPUT_EVDEV_H
#define INPUT_EVDEV_H

#include "lvgl.h"

/* The pointer for `disp`, its cursor, and (scan != 0) the devices under
 * /dev/input, looked for again every 2 s. `on_right` runs on the LVGL
 * thread after each right-button press. */
lv_indev_t *input_evdev_create(lv_display_t *disp, void (*on_right)(void), int scan);
void        input_evdev_delete(void);

/* Read evdev events from `fd` (non-blocking) as a pointer device named
 * `name`: relative, or absolute with the given ranges. The reader closes
 * it when it is gone. Used by the scan, and by the tests with a pipe. */
int input_evdev_attach(int fd, const char *name, int abs, int min_x, int max_x, int min_y, int max_y);

#endif /* INPUT_EVDEV_H */
