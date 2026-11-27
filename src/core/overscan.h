/* SPDX-License-Identifier: GPL-3.0-or-later */
/* overscan.h — margins around the picture on the output, for a TV that
 * overscans (shows the middle of an HDMI picture enlarged, its edges off
 * the screen): everything on the output, the video, the UI and the mouse
 * pointer, is drawn inside the inner rectangle the margins leave, the rest
 * black. The UI's own coordinates (1280x720) do not change: it is scaled
 * into the inner rectangle as it is scaled to the output.
 *
 * The margins are percentages of the output (top and bottom of its
 * height, left and right of its width), so they keep their place when the
 * output's resolution changes. */
#ifndef OVERSCAN_H
#define OVERSCAN_H

#include "vg_types.h"

typedef struct { unsigned top, bottom, left, right; } vg_margins_t;

/* The most a margin may be (vigil.yaml's display.margin_*). */
#define OVERSCAN_MAX_PCT 10

/* No margin at all. */
int overscan_none(const vg_margins_t *m);

/* The inner rectangle of a w x h output: each margin `pct` % of the
 * output, rounded down to a multiple of `align` (a margin over
 * OVERSCAN_MAX_PCT counts as that), the rectangle what is left. The whole
 * output with no margins. */
vg_rect_t overscan_inner(unsigned w, unsigned h, const vg_margins_t *m, unsigned align);

/* Rectangle `r` on a from_w x from_h screen (the UI's coordinates) placed
 * in `inner` on the output: aspect_map to the inner rectangle's size (each
 * edge scaled, rounded down to `align`, so neighbouring tiles still meet),
 * then moved to its place. */
vg_rect_t overscan_map(const vg_rect_t *r, unsigned from_w, unsigned from_h, const vg_rect_t *inner,
                       unsigned align);

/* Point x,y of a from_w x from_h screen on the output: in `inner`, its
 * edges scaled (rounded down). */
void overscan_point(int x, int y, unsigned from_w, unsigned from_h, const vg_rect_t *inner, int *ox, int *oy);

/* The other way: rectangle `r` on the output (in `inner`) as a rectangle
 * of a to_w x to_h screen, each edge scaled back (rounded down) and kept
 * on the screen; what lies outside `inner` is cut off. */
vg_rect_t overscan_unmap(const vg_rect_t *r, const vg_rect_t *inner, unsigned to_w, unsigned to_h);

#endif /* OVERSCAN_H */
