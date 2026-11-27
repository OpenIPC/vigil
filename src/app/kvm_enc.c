/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kvm_enc.c — changed parts of the screen as JPEGs, one message (kvm_enc.h). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "kvm_enc.h"

#define MCU      16
#define BAND     4                  /* block rows per band */
#define GAP      2                  /* clean blocks a part bridges rather than split */
#define FULL_PCT 50                 /* past this many changed, the whole picture */
#define PARTS_MAX 256

int kvm_enc_init(kvm_enc_t *e, unsigned w, unsigned h)
{
    memset(e, 0, sizeof *e);
    if (!w || !h || (w | h) & 1) return -1;
    e->w = w;
    e->h = h;
    e->prev = malloc((size_t)w * h * 3 / 2);
    e->dirty = malloc((size_t)((w + MCU - 1) / MCU) * ((h + MCU - 1) / MCU));
    if (!e->prev || !e->dirty) {
        kvm_enc_free(e);
        return -1;
    }
    return 0;
}

void kvm_enc_free(kvm_enc_t *e)
{
    free(e->prev);
    free(e->dirty);
    memset(e, 0, sizeof *e);
}

typedef struct {
    unsigned x, y, w, h;
} part_t;

static unsigned umin(unsigned a, unsigned b) { return a < b ? a : b; }

/* The planes of e->prev as a picture. */
static jpeg_planes_t prev_planes(const kvm_enc_t *e)
{
    unsigned char *y = e->prev, *u = y + (size_t)e->w * e->h, *v = u + (size_t)(e->w / 2) * (e->h / 2);
    return (jpeg_planes_t){ y, u, v, e->w, e->h, e->w, e->w / 2 };
}

static int block_changed(const kvm_enc_t *e, const jpeg_planes_t *p, unsigned bx, unsigned by)
{
    jpeg_planes_t q = prev_planes(e);
    unsigned x0 = bx * MCU, y0 = by * MCU, w = umin(MCU, e->w - x0), h = umin(MCU, e->h - y0);
    for (unsigned r = 0; r < h; ++r)
        if (memcmp(p->y + (size_t)(y0 + r) * p->ys + x0, q.y + (size_t)(y0 + r) * q.ys + x0, w)) return 1;
    for (unsigned r = 0; r < h / 2; ++r) {
        size_t a = (size_t)(y0 / 2 + r) * p->cs + x0 / 2, b = (size_t)(y0 / 2 + r) * q.cs + x0 / 2;
        if (memcmp(p->u + a, q.u + b, w / 2) || memcmp(p->v + a, q.v + b, w / 2)) return 1;
    }
    return 0;
}

static void copy_part(const kvm_enc_t *e, const jpeg_planes_t *p, const part_t *r)
{
    jpeg_planes_t q = prev_planes(e);
    for (unsigned i = 0; i < r->h; ++i)
        memcpy((unsigned char *)q.y + (size_t)(r->y + i) * q.ys + r->x, p->y + (size_t)(r->y + i) * p->ys + r->x, r->w);
    for (unsigned i = 0; i < r->h / 2; ++i) {
        size_t a = (size_t)(r->y / 2 + i) * q.cs + r->x / 2, b = (size_t)(r->y / 2 + i) * p->cs + r->x / 2;
        memcpy((unsigned char *)q.u + a, p->u + b, r->w / 2);
        memcpy((unsigned char *)q.v + a, p->v + b, r->w / 2);
    }
}

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, uint32_t v)
{
    put16(p, v & 0xffff);
    put16(p + 2, v >> 16);
}

/* The changed blocks as parts: per band, runs of columns with a change
 * (bridging short gaps), each as tall as its changed rows. */
static int gather(const kvm_enc_t *e, unsigned bw, unsigned bh, part_t *parts)
{
    int n = 0;
    for (unsigned b0 = 0; b0 < bh; b0 += BAND) {
        unsigned rows = umin(BAND, bh - b0);
        int s = -1, last = -1;
        for (unsigned bx = 0; bx <= bw; ++bx) {
            int col = 0;
            for (unsigned r = 0; bx < bw && r < rows; ++r) col |= e->dirty[(b0 + r) * bw + bx];
            if (col && s >= 0 && (int)bx - last - 1 <= GAP) {
                last = (int)bx;
                continue;
            }
            if (s >= 0 && (col || bx == bw || (int)bx - last - 1 > GAP)) {
                /* close the run s..last */
                unsigned rmin = rows, rmax = 0;
                for (unsigned r = 0; r < rows; ++r)
                    for (int x = s; x <= last; ++x)
                        if (e->dirty[(b0 + r) * bw + (unsigned)x]) {
                            if (r < rmin) rmin = r;
                            if (r > rmax) rmax = r;
                        }
                unsigned px = (unsigned)s * MCU, py = (b0 + rmin) * MCU;
                if (n < PARTS_MAX)
                    parts[n++] = (part_t){ px, py, umin((unsigned)(last + 1) * MCU, e->w) - px,
                                           umin((b0 + rmax + 1) * MCU, e->h) - py };
                s = -1;
            }
            if (col) {
                s = last = (int)bx;
            }
        }
    }
    return n;
}

int kvm_enc_frame(kvm_enc_t *e, const jpeg_planes_t *pic, int quality, unsigned char **msg, size_t *len)
{
    *msg = NULL;
    *len = 0;
    unsigned bw = (e->w + MCU - 1) / MCU, bh = (e->h + MCU - 1) / MCU, changed = 0;
    for (unsigned by = 0; by < bh; ++by)
        for (unsigned bx = 0; bx < bw; ++bx) {
            unsigned char d = !e->have || block_changed(e, pic, bx, by);
            e->dirty[by * bw + bx] = d;
            changed += d;
        }
    if (!changed) return 0;

    static __thread part_t parts[PARTS_MAX];
    int n, full = !e->have || changed * 100 > bw * bh * FULL_PCT;
    if (full) {
        parts[0] = (part_t){ 0, 0, e->w, e->h };
        n = 1;
    } else {
        n = gather(e, bw, bh, parts);
    }

    jpeg_buf_t b = { 0 };
    if (jpeg_buf_reserve(&b, KVM_MSG_HEADER + (size_t)n * KVM_PART_HEADER) < 0) return -1;
    b.len = KVM_MSG_HEADER;
    for (int i = 0; i < n; ++i) {
        size_t at = b.len;
        if (jpeg_buf_reserve(&b, KVM_PART_HEADER) < 0) goto oom;
        b.len += KVM_PART_HEADER;
        long jl = jpeg_encode(pic, parts[i].x, parts[i].y, parts[i].w, parts[i].h, quality, &b);
        if (jl < 0) goto oom;
        put16(b.p + at, parts[i].x);
        put16(b.p + at + 2, parts[i].y);
        put16(b.p + at + 4, parts[i].w);
        put16(b.p + at + 6, parts[i].h);
        put32(b.p + at + 8, (uint32_t)jl);
    }
    for (int i = 0; i < n; ++i) copy_part(e, pic, &parts[i]);
    e->have = 1;
    b.p[0] = 'F';
    b.p[1] = (unsigned char)full;
    put16(b.p + 2, e->w);
    put16(b.p + 4, e->h);
    put16(b.p + 6, (unsigned)n);
    put32(b.p + 8, ++e->seq);
    *msg = b.p;
    *len = b.len;
    return 1;
oom:
    free(b.p);
    return -1;
}
