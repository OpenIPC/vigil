/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hal_display.h — the video layer: one window (tile) per channel, and
 * the output mode.
 *
 * Tiles are given on the screen hal_init was sized to (the UI's
 * coordinates). The output may run at another mode: the platform then
 * scales the screen, the UI layer and the tiles alike, to it. */
#ifndef HAL_DISPLAY_H
#define HAL_DISPLAY_H

#include "vg_types.h"

/* Place channel `ch`'s video at `r` (screen pixels): the picture fitted
 * inside it at the ratio hal_display_set_aspect gave (aspect_fit), on the
 * output. Returns 0 or -1. */
int  hal_display_set_tile(int ch, const vg_rect_t *r);
void hal_display_clear_tile(int ch);
/* The display aspect ratio channel `ch`'s picture is shown at, w:h (the
 * core's choice, aspect_of_camera: a sub-stream at its main stream's), the
 * rest of the tile black; 0:0: the picture fills the tile (stretched).
 * Until it is first called, the decoded picture's own size. Applies at
 * once, to the picture on screen and the decoders that follow. From any
 * thread (a channel's, before hal_dec_open; the main thread). */
void hal_display_set_aspect(int ch, unsigned w, unsigned h);

/* The modes the output can run at, at most `max` (none: no display);
 * a platform that cannot change its output lists the one it runs. */
int  hal_display_modes(vg_mode_t *out, int max);
/* The mode the output runs at now. */
void hal_display_mode(vg_mode_t *cur);
/* Switch the output to `m` (one of hal_display_modes) now; the same mode
 * again changes nothing. 0, or -1 with the output as it was (as far as
 * the platform can tell). */
int  hal_display_set_mode(const vg_mode_t *m);

#endif /* HAL_DISPLAY_H */
