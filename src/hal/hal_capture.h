/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hal_capture.h — local video inputs (DVR): the platform captures input
 * `in`, shows it in tile `ch` itself, and encodes it; encoded access units
 * come back through the callback (from a platform thread) for recording. */
#ifndef HAL_CAPTURE_H
#define HAL_CAPTURE_H

#include "vg_types.h"

typedef void (*hal_cap_cb)(void *user, const vg_au_t *au);

/* Returns 0, or -1 if the input does not exist / cannot start. */
int  hal_cap_start(int in, int ch, hal_cap_cb cb, void *user);
void hal_cap_stop(int in);
/* 1 while the input has a signal. */
int  hal_cap_signal(int in);

#endif /* HAL_CAPTURE_H */
