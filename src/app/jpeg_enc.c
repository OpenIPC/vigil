/* SPDX-License-Identifier: GPL-3.0-or-later */
/* jpeg_enc.c — baseline JPEG (ITU T.81), 4:2:0, the standard tables of its
 * annex K, the floating-point AAN forward DCT (as IJG's jfdctflt.c): one
 * MCU of 16x16 pixels at a time straight from the planes, so nothing is
 * converted or copied first. See jpeg_enc.h. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "jpeg_enc.h"

static const unsigned char k_zigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* Annex K.1, natural order. */
static const unsigned char k_qluma[64] = {
    16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99,
};
static const unsigned char k_qchroma[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
    99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
};

/* Annex K.3: code lengths (count per length 1..16) and values. */
static const unsigned char k_dc_luma_bits[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const unsigned char k_dc_chroma_bits[16] = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const unsigned char k_dc_vals[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const unsigned char k_ac_luma_bits[16] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const unsigned char k_ac_luma_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
    0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};
static const unsigned char k_ac_chroma_bits[16] = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const unsigned char k_ac_chroma_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
    0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
    0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
    0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

typedef struct {
    uint16_t code[256];
    uint8_t  len[256];
} huff_t;

/* Annex C: the codes of a table given by its lengths. */
static void huff_build(huff_t *h, const unsigned char bits[16], const unsigned char *vals)
{
    memset(h, 0, sizeof *h);
    unsigned code = 0, k = 0;
    for (int l = 1; l <= 16; ++l) {
        for (int i = 0; i < bits[l - 1]; ++i, ++k) {
            h->code[vals[k]] = (uint16_t)code++;
            h->len[vals[k]] = (uint8_t)l;
        }
        code <<= 1;
    }
}

typedef struct {
    huff_t dc[2], ac[2];
    float  fdiv[2][64];             /* 1 / (quantizer x AAN scale x 8), natural order */
    unsigned char q[2][64];         /* zigzag order, as DQT has them */
} tables_t;

static void tables_make(tables_t *t, int quality)
{
    static const float aan[8] = { 1.0f, 1.387039845f, 1.306562965f, 1.175875602f,
                                  1.0f, 0.785694958f, 0.541196100f, 0.275899379f };
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    for (int c = 0; c < 2; ++c) {
        const unsigned char *base = c ? k_qchroma : k_qluma;
        for (int i = 0; i < 64; ++i) {
            int v = (base[i] * scale + 50) / 100;
            v = v < 1 ? 1 : v > 255 ? 255 : v;
            t->fdiv[c][i] = 1.0f / ((float)v * aan[i / 8] * aan[i % 8] * 8.0f);
        }
        for (int k = 0; k < 64; ++k) {
            int v = (base[k_zigzag[k]] * scale + 50) / 100;
            t->q[c][k] = (unsigned char)(v < 1 ? 1 : v > 255 ? 255 : v);
        }
    }
    huff_build(&t->dc[0], k_dc_luma_bits, k_dc_vals);
    huff_build(&t->dc[1], k_dc_chroma_bits, k_dc_vals);
    huff_build(&t->ac[0], k_ac_luma_bits, k_ac_luma_vals);
    huff_build(&t->ac[1], k_ac_chroma_bits, k_ac_chroma_vals);
}

int jpeg_buf_reserve(jpeg_buf_t *b, size_t n)
{
    if (b->cap - b->len >= n) return 0;
    size_t cap = b->cap ? b->cap : 16384;
    while (cap - b->len < n) cap *= 2;
    unsigned char *p = realloc(b->p, cap);
    if (!p) return -1;
    b->p = p;
    b->cap = cap;
    return 0;
}

/* ---- bits ---------------------------------------------------------------------------- */

typedef struct {
    jpeg_buf_t *b;
    uint32_t    acc;                /* bits waiting, at the top */
    int         n;
    int         oom;
} bits_t;

static void put_bits(bits_t *w, uint32_t code, int len)
{
    w->acc |= code << (32 - w->n - len);
    w->n += len;
    while (w->n >= 8) {
        unsigned char c = (unsigned char)(w->acc >> 24);
        /* reserved per MCU: never short here */
        w->b->p[w->b->len++] = c;
        if (c == 0xff) w->b->p[w->b->len++] = 0;        /* byte stuffing */
        w->acc <<= 8;
        w->n -= 8;
    }
}

static void put_coef(bits_t *w, const huff_t *h, int sym_run, int v)
{
    int a = v < 0 ? -v : v, nb = 0;
    while (a) {
        ++nb;
        a >>= 1;
    }
    int sym = (sym_run << 4) | nb;
    put_bits(w, h->code[sym], h->len[sym]);
    if (nb) put_bits(w, (uint32_t)(v < 0 ? v - 1 : v) & ((1u << nb) - 1), nb);
}

/* ---- one block ----------------------------------------------------------------------- */

static void fdct_quant(float *d, const float *fdiv, int *out)
{
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < 8; ++i) {
            /* rows, then columns */
            float *p = pass ? d + i : d + i * 8;
            int s = pass ? 8 : 1;
            float t0 = p[0] + p[7 * s], t7 = p[0] - p[7 * s];
            float t1 = p[s] + p[6 * s], t6 = p[s] - p[6 * s];
            float t2 = p[2 * s] + p[5 * s], t5 = p[2 * s] - p[5 * s];
            float t3 = p[3 * s] + p[4 * s], t4 = p[3 * s] - p[4 * s];
            float t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
            p[0] = t10 + t11;
            p[4 * s] = t10 - t11;
            float z1 = (t12 + t13) * 0.707106781f;
            p[2 * s] = t13 + z1;
            p[6 * s] = t13 - z1;
            t10 = t4 + t5;
            t11 = t5 + t6;
            t12 = t6 + t7;
            float z5 = (t10 - t12) * 0.382683433f;
            float z2 = 0.541196100f * t10 + z5;
            float z4 = 1.306562965f * t12 + z5;
            float z3 = t11 * 0.707106781f;
            float z11 = t7 + z3, z13 = t7 - z3;
            p[5 * s] = z13 + z2;
            p[3 * s] = z13 - z2;
            p[s] = z11 + z4;
            p[7 * s] = z11 - z4;
        }
    }
    for (int k = 0; k < 64; ++k) {
        int n = k_zigzag[k];
        float v = d[n] * fdiv[n];
        out[k] = (int)(v < 0 ? v - 0.5f : v + 0.5f);
    }
}

static void block(bits_t *w, const tables_t *t, int c, float *d, int *dc_prev)
{
    int q[64];
    fdct_quant(d, t->fdiv[c], q);
    int diff = q[0] - *dc_prev;
    *dc_prev = q[0];
    put_coef(w, &t->dc[c], 0, diff);
    int run = 0;
    for (int k = 1; k < 64; ++k) {
        if (!q[k]) {
            ++run;
            continue;
        }
        while (run > 15) {
            put_bits(w, t->ac[c].code[0xf0], t->ac[c].len[0xf0]);     /* ZRL */
            run -= 16;
        }
        put_coef(w, &t->ac[c], run, q[k]);
        run = 0;
    }
    if (run) put_bits(w, t->ac[c].code[0], t->ac[c].len[0]);           /* EOB */
}

/* An 8x8 block of a plane at (x, y), clamped to [x0, x1) x [y0, y1):
 * the edge pixels repeat past the region. */
static void fetch(const unsigned char *pl, unsigned stride, unsigned x, unsigned y, unsigned x0, unsigned x1,
                  unsigned y0, unsigned y1, float *d)
{
    unsigned cx[8];
    for (int i = 0; i < 8; ++i) cx[i] = x + i < x1 ? x + i : x1 - 1;
    (void)x0;
    (void)y0;
    for (int r = 0; r < 8; ++r) {
        const unsigned char *row = pl + (size_t)(y + r < y1 ? y + r : y1 - 1) * stride;
        if (x + 8 <= x1) {
            for (int i = 0; i < 8; ++i) d[r * 8 + i] = (float)row[x + i] - 128.0f;
        } else {
            for (int i = 0; i < 8; ++i) d[r * 8 + i] = (float)row[cx[i]] - 128.0f;
        }
    }
}

/* ---- the file ------------------------------------------------------------------------ */

static void put_bytes(jpeg_buf_t *b, const void *p, size_t n)
{
    memcpy(b->p + b->len, p, n);
    b->len += n;
}

static void put16(jpeg_buf_t *b, unsigned v)
{
    b->p[b->len++] = (unsigned char)(v >> 8);
    b->p[b->len++] = (unsigned char)v;
}

static void put_dht(jpeg_buf_t *b, int cls_id, const unsigned char bits[16], const unsigned char *vals)
{
    int n = 0;
    for (int i = 0; i < 16; ++i) n += bits[i];
    put16(b, 0xffc4);
    put16(b, (unsigned)(2 + 1 + 16 + n));
    b->p[b->len++] = (unsigned char)cls_id;
    put_bytes(b, bits, 16);
    put_bytes(b, vals, (size_t)n);
}

long jpeg_encode(const jpeg_planes_t *pic, unsigned rx, unsigned ry, unsigned rw, unsigned rh, int quality,
                 jpeg_buf_t *out)
{
    if (!rw || !rh || rx + rw > pic->w || ry + rh > pic->h) return -1;
    tables_t t;
    tables_make(&t, quality);
    size_t start = out->len;
    if (jpeg_buf_reserve(out, 1024) < 0) return -1;
    jpeg_buf_t *b = out;
    static const unsigned char soi_app0[] = { 0xff, 0xd8, 0xff, 0xe0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0,
                                              0, 1, 0, 1, 0, 0 };
    put_bytes(b, soi_app0, sizeof soi_app0);
    for (int c = 0; c < 2; ++c) {
        put16(b, 0xffdb);
        put16(b, 67);
        b->p[b->len++] = (unsigned char)c;
        put_bytes(b, t.q[c], 64);
    }
    static const unsigned char comps[] = { 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1 };
    put16(b, 0xffc0);
    put16(b, 17);
    b->p[b->len++] = 8;
    put16(b, rh);
    put16(b, rw);
    b->p[b->len++] = 3;
    put_bytes(b, comps, sizeof comps);
    put_dht(b, 0x00, k_dc_luma_bits, k_dc_vals);
    put_dht(b, 0x10, k_ac_luma_bits, k_ac_luma_vals);
    put_dht(b, 0x01, k_dc_chroma_bits, k_dc_vals);
    put_dht(b, 0x11, k_ac_chroma_bits, k_ac_chroma_vals);
    static const unsigned char sos[] = { 0xff, 0xda, 0, 12, 3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0 };
    put_bytes(b, sos, sizeof sos);

    bits_t w = { .b = b };
    int dc[3] = { 0, 0, 0 };
    float d[64];
    unsigned x1 = rx + rw, y1 = ry + rh, cx0 = rx / 2, cy0 = ry / 2, cx1 = (rx + rw + 1) / 2, cy1 = (ry + rh + 1) / 2;
    for (unsigned my = 0; my < rh; my += 16) {
        for (unsigned mx = 0; mx < rw; mx += 16) {
            /* six blocks of at most 64 coefficients of 16 + 11 bits, stuffed */
            if (jpeg_buf_reserve(b, 6 * 64 * 4 * 2 + 16) < 0) {
                out->len = start;
                return -1;
            }
            for (int k = 0; k < 4; ++k) {
                /* past the region's edge, fetch repeats its last pixels */
                unsigned bx = rx + mx + (k & 1) * 8, by = ry + my + (k >> 1) * 8;
                fetch(pic->y, pic->ys, bx, by, rx, x1, ry, y1, d);
                block(&w, &t, 0, d, &dc[0]);
            }
            fetch(pic->u, pic->cs, (rx + mx) / 2, (ry + my) / 2, cx0, cx1, cy0, cy1, d);
            block(&w, &t, 1, d, &dc[1]);
            fetch(pic->v, pic->cs, (rx + mx) / 2, (ry + my) / 2, cx0, cx1, cy0, cy1, d);
            block(&w, &t, 1, d, &dc[2]);
        }
    }
    if (jpeg_buf_reserve(b, 8) < 0) {
        out->len = start;
        return -1;
    }
    if (w.n) put_bits(&w, (1u << (8 - w.n)) - 1, 8 - w.n);     /* fill the last byte with ones */
    put16(b, 0xffd9);
    return (long)(b->len - start);
}
