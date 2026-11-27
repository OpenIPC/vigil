/* SPDX-License-Identifier: GPL-3.0-or-later */
/* jpeg_enc.h — a small baseline JPEG encoder for the zero channel: 4:2:0
 * YUV planes in, a JFIF file out (what browsers decode with
 * createImageBitmap). No dependency: libjpeg is not on the firmware, and
 * the zero channel needs only this one kind of picture. */
#ifndef JPEG_ENC_H
#define JPEG_ENC_H

#include <stddef.h>

/* A growing output buffer. */
typedef struct {
    unsigned char *p;
    size_t         len, cap;
} jpeg_buf_t;

/* Make room for `n` more bytes. 0, or -1 (out of memory). */
int jpeg_buf_reserve(jpeg_buf_t *b, size_t n);

/* Planes of a 4:2:0 picture, full-range (JFIF) YCbCr: y is w x h, u and v
 * (w/2) x (h/2) (w and h even). */
typedef struct {
    const unsigned char *y, *u, *v;
    unsigned             w, h;
    unsigned             ys, cs;        /* bytes per row of y, of u and v */
} jpeg_planes_t;

/* Encode the rw x rh part at (rx, ry) (even, inside the picture) at
 * `quality` (1..100, IJG's scale), appended to `out`. Returns the bytes
 * appended, or -1 (out of memory; `out` keeps what it had). */
long jpeg_encode(const jpeg_planes_t *pic, unsigned rx, unsigned ry, unsigned rw, unsigned rh, int quality,
                 jpeg_buf_t *out);

#endif /* JPEG_ENC_H */
