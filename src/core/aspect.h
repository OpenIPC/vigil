/* SPDX-License-Identifier: GPL-3.0-or-later */
/* aspect.h — what shape a camera's picture is, and where it goes in its
 * tile: the geometry every platform's display uses.
 *
 * A picture is shown at its display aspect ratio (DAR), not at the ratio
 * of its coded size: D1 and CIF sub-streams (704x576, 352x288, 704x480)
 * are anamorphic, their pixels not square. The camera's DAR is its main
 * stream's, whose pixels are square in practice, so a sub-stream is shown
 * at the main stream's DAR once that is known (as commercial NVRs do), and
 * switching between the two does not change the picture's shape. */
#ifndef ASPECT_H
#define ASPECT_H

#include "vg_types.h"

/* A ratio w:h, reduced; 0:0 for none. */
typedef struct { unsigned w, h; } vg_ratio_t;

/* A stream's DAR: its displayed size times its sample aspect ratio (1:1
 * when the SPS gives none), reduced; within 2 % of a common ratio (16:9,
 * 4:3, 16:10, 5:4, 3:2, 1:1, 2:1, 32:9) it is that ratio, so 800x448 or
 * 1920x1088 is 16:9. 0:0 when the size is unknown. */
vg_ratio_t aspect_of_stream(const vg_stream_info_t *si);

/* The DAR to show a camera's picture at: `shown` is the stream decoded,
 * `is_sub` whether it is the sub-stream, `main` the main stream's info
 * (NULL while its SPS has not been seen). A sub-stream takes the main
 * stream's DAR; otherwise the stream's own. */
vg_ratio_t aspect_of_camera(const vg_stream_info_t *shown, int is_sub, const vg_stream_info_t *main);

/* The rectangle a picture of DAR `a` takes in tile `t`: the largest one of
 * that ratio inside it, centred, the bars (above and below, or at the
 * sides) black (VG_ASPECT_FIT); a 0:0 ratio fills the tile (stretch). Offsets and sizes
 * are multiples of `align` (at least `align`; the tile's own x/y kept).
 * Never larger than the tile. */
vg_rect_t aspect_fit(const vg_rect_t *t, vg_ratio_t a, unsigned align);

/* The rectangle a picture of DAR `a` takes when it covers tile `t`
 * (VG_ASPECT_FILL): the smallest one of that ratio the tile fits inside,
 * centred on the tile, so it overflows it one way (above and below, or at
 * the sides). A 0:0 ratio is the tile. */
vg_rect_t aspect_cover(const vg_rect_t *t, vg_ratio_t a);

/* The part of a src_w x src_h picture of DAR `a` (0:0: the ratio of its
 * size, square pixels) that shows when it covers a tile_w x tile_h tile
 * (aspect_cover), in the picture's pixels: all of it one way, the middle
 * of it the other. Offsets and sizes are multiples of `align` where the
 * picture is cut; never outside the picture, never empty. */
vg_rect_t aspect_crop(unsigned src_w, unsigned src_h, vg_ratio_t a, unsigned tile_w, unsigned tile_h,
                      unsigned align);

/* Rectangle `r` on a from_w x from_h screen as a rectangle on a to_w x
 * to_h one (the UI's coordinates on the output's video layer, which may
 * have another aspect: the UI is stretched there, the video is not): each
 * edge scaled, then rounded down to a multiple of `align`, so neighbouring
 * tiles still meet. */
vg_rect_t aspect_map(const vg_rect_t *r, unsigned from_w, unsigned from_h, unsigned to_w, unsigned to_h,
                     unsigned align);

/* The size a scaler that only shrinks should make a src_w x src_h picture
 * for a want_w x want_h window, per dimension: the window's, where it is
 * smaller than the picture, rounded down to a multiple of `align`, and no
 * smaller than the picture shrunk `max_shrink` times (rounded up to
 * `align`); the picture's own where the window is as large. The display
 * enlarges the result into the window. */
void aspect_scaled_size(unsigned src_w, unsigned src_h, unsigned want_w, unsigned want_h, unsigned align,
                        unsigned max_shrink, unsigned *out_w, unsigned *out_h);

#endif /* ASPECT_H */
