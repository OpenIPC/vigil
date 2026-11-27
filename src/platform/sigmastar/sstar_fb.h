/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sstar_fb.h — between the SigmaStar display (hal_sigmastar.c) and its UI
 * layer (hal_ui_fbdev.c). */
#ifndef SSTAR_FB_H
#define SSTAR_FB_H

#include "vg_types.h"

/* Scale the UI layer (/dev/fb0, its buffer at the size fbdev.ini gave it)
 * to a w x h output (the GOP's stretch window over all of it), the UI in
 * `inner`, the rectangle of the output the margins leave (overscan.h; the
 * whole output without them), black around it: with margins the UI is
 * shrunk into the buffer as it is drawn. The hardware pointer follows.
 * The buffer, and so the UI's coordinates, stay. 0, or -1. The main
 * thread (it may redraw the UI). */
int sstar_fb_screen(unsigned w, unsigned h, const vg_rect_t *inner);

#endif /* SSTAR_FB_H */
