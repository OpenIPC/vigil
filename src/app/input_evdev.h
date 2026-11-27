/* SPDX-License-Identifier: GPL-3.0-or-later */
/* input_evdev.h — mice, touch screens and keyboards (Linux evdev) as one
 * LVGL pointer and one LVGL keypad (see input_evdev.c). */
#ifndef INPUT_EVDEV_H
#define INPUT_EVDEV_H

#include <stdint.h>

#include "lvgl.h"

typedef struct {
    /* After each right-button press. */
    void (*right)(void);
    /* A key pressed (or repeated by the kernel): `key` and `mods` as in
     * ui_keys.h. Returns the key LVGL's keypad gets, 0 for none (the hook
     * handled it; the keys after it wait for the next input poll, so what
     * it started runs first). NULL: every key goes to LVGL. */
    uint32_t (*key)(uint32_t key, unsigned mods);
    /* The number of keyboards changed (plugged in, unplugged). */
    void (*keyboards)(int n);
    /* The mouse wheel turned `steps` notches (> 0: away from the user,
     * up), after LVGL's input processing; LVGL's pointer has no wheel. */
    void (*wheel)(int steps);
} input_hooks_t;

/* The pointer for `disp` with its cursor, the keypad (input_evdev_keypad),
 * and (scan != 0) the devices under /dev/input, looked for again every
 * 2 s. The hooks run on the LVGL thread; `hooks` is copied. */
lv_indev_t *input_evdev_create(lv_display_t *disp, const input_hooks_t *hooks, int scan);
lv_indev_t *input_evdev_keypad(void);
void        input_evdev_delete(void);

/* Read evdev events from `fd` (non-blocking) as a pointer device named
 * `name`: relative, or absolute with the given ranges; or as a keyboard.
 * The reader closes it when it is gone. Used by the scan, and by the tests
 * with a pipe. */
int input_evdev_attach(int fd, const char *name, int abs, int min_x, int max_x, int min_y, int max_y);
int input_evdev_attach_keyboard(int fd, const char *name);

#endif /* INPUT_EVDEV_H */
