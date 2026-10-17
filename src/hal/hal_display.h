/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hal_display.h — the video layer: one window (tile) per channel. */
#ifndef HAL_DISPLAY_H
#define HAL_DISPLAY_H

#include "vg_types.h"

/* Place channel `ch`'s video at `r` (screen pixels). The platform keeps
 * the source's aspect ratio inside it. Returns 0 or -1. */
int  hal_display_set_tile(int ch, const vg_rect_t *r);
void hal_display_clear_tile(int ch);

#endif /* HAL_DISPLAY_H */
