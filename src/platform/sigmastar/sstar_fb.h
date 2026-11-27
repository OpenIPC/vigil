/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sstar_fb.h — between the SigmaStar display (hal_sigmastar.c) and its UI
 * layer (hal_ui_fbdev.c). */
#ifndef SSTAR_FB_H
#define SSTAR_FB_H

/* Scale the UI layer (/dev/fb0, its buffer at the size fbdev.ini gave it)
 * to a w x h output: the GOP's stretch window over the whole screen. The
 * buffer, and so the UI's coordinates, stay. 0, or -1. */
int sstar_fb_screen(unsigned w, unsigned h);

#endif /* SSTAR_FB_H */
