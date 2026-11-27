/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kvm_enc.h — the zero channel's pictures on the wire: only the parts of
 * the screen that changed since the last picture sent, each a JPEG.
 *
 * One binary WebSocket message per picture, little-endian:
 *
 *   0  u8   'F'
 *   1  u8   flags: 1 = the whole picture (the first, or most of it changed)
 *   2  u16  width, 4 u16 height (the picture's; the screen's or half)
 *   6  u16  n, the parts that follow
 *   8  u32  sequence number (the browser acks it, web.c)
 *   12 n x { u16 x, y, w, h; u32 len; len bytes of JPEG }
 *
 * Changes are found per 16x16 block (a JPEG MCU) against the picture
 * last sent; changed blocks are gathered into a few rectangles per band
 * of 64 rows, so a pointer moving or a menu opening sends only that
 * corner, and moving video sends the whole picture as one JPEG. */
#ifndef KVM_ENC_H
#define KVM_ENC_H

#include <stddef.h>

#include "jpeg_enc.h"

#define KVM_MSG_HEADER  12
#define KVM_PART_HEADER 12

typedef struct {
    unsigned       w, h;
    unsigned char *prev;            /* y, u, v of the picture last sent (w x h 4:2:0) */
    unsigned char *dirty;           /* per block */
    int            have;            /* prev holds one */
    unsigned       seq;
} kvm_enc_t;

int  kvm_enc_init(kvm_enc_t *e, unsigned w, unsigned h);
void kvm_enc_free(kvm_enc_t *e);

/* What of `pic` (e->w x e->h) differs from the picture last sent, as one
 * message (malloc'd, *msg; *len bytes). 1, 0 when nothing changed, -1
 * out of memory. */
int kvm_enc_frame(kvm_enc_t *e, const jpeg_planes_t *pic, int quality, unsigned char **msg, size_t *len);

#endif /* KVM_ENC_H */
