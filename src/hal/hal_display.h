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
#include "overscan.h"
#include "picture.h"

/* Place channel `ch`'s video at `r` (screen pixels): the picture fitted
 * inside it, stretched over it or covering it, as hal_display_set_aspect
 * said, on the output. Returns 0 or -1. */
int  hal_display_set_tile(int ch, const vg_rect_t *r);
void hal_display_clear_tile(int ch);
/* How channel `ch`'s picture fills its tile. w:h is the display aspect
 * ratio to show it at (the core's choice, aspect_of_camera: a sub-stream
 * at its main stream's; 0:0 the decoded picture's own size):
 *   VG_ASPECT_FIT      the largest rectangle of that ratio inside the tile,
 *                      the rest black (aspect_fit)
 *   VG_ASPECT_STRETCH  the whole tile, whatever the ratio
 *   VG_ASPECT_FILL     the whole tile at that ratio, the part of the
 *                      picture that overflows it cut off, centred
 *                      (aspect_crop); a platform that cannot crop fits
 * Until it is first called, fitted at the decoded picture's own size.
 * Applies at once, to the picture on screen and the decoders that follow.
 * From any thread (a channel's, before hal_dec_open; the main thread). */
void hal_display_set_aspect(int ch, unsigned w, unsigned h, vg_aspect_t how);

/* The modes the output can run at, at most `max` (none: no display);
 * a platform that cannot change its output lists the one it runs. */
int  hal_display_modes(vg_mode_t *out, int max);
/* The mode the output runs at now. */
void hal_display_mode(vg_mode_t *cur);
/* Switch the output to `m` (one of hal_display_modes) now; the same mode
 * again changes nothing. 0, or -1 with the output as it was (as far as
 * the platform can tell). */
int  hal_display_set_mode(const vg_mode_t *m);

/* ---- the picture, for the zero channel (the web server's view of the screen) ---- */

/* Three planes of 4:2:0 YUV: y is w x h, u and v (w/2) x (h/2). */
typedef struct {
    unsigned char *y, *u, *v;
    unsigned       w, h;            /* the screen's size (hal_init) */
    unsigned       ys, cs;          /* bytes per row of y, and of u and v */
} hal_pic_t;

/* From on to off: the platform keeps each decoder's newest picture where
 * hal_display_grab can read it (which may hold a buffer per channel), or
 * stops. Off costs the video nothing. */
void hal_display_grab_keep(int on);
/* Draw what the video layer shows now, each tile's picture where the
 * output has it, into `pic` (screen coordinates; the rest left as it
 * is), as the decoders put it out: BT.601 video range (Y 16..235).
 * 0, or -1 where the platform cannot (nothing drawn). Any thread. */
int  hal_display_grab(const hal_pic_t *pic);

/* ---- the output's adjustments (Display -> Output) ------------------------------------ */

/* Margins around everything the output shows (overscan.h): the tiles, the
 * UI layer and the pointer drawn inside the inner rectangle
 * (overscan_inner of the output, kept as the output mode changes), black
 * around it. The screen's coordinates do not change; the zero channel's
 * picture (hal_display_grab) stays the whole screen. 0, or -1 where the
 * platform cannot (hal_caps()->margins 0). The main thread (it may redraw
 * the UI). */
int  hal_display_set_margins(const vg_margins_t *m);

/* The colour controls and gamma of what goes out on HDMI and VGA
 * (picture.h); kept across output modes. 0, or -1 where the platform has
 * none (hal_caps()->picture 0). */
int  hal_display_set_picture(const vg_picture_t *p);

#endif /* HAL_DISPLAY_H */
