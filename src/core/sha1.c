/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sha1.c — SHA-1 (RFC 3174) and base64 (see sha1.h). */
#include <string.h>

#include "sha1.h"

static uint32_t rol(uint32_t v, int n) { return v << n | v >> (32 - n); }

static void block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else             { f = b ^ c ^ d;                   k = 0xca62c1d6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1_raw(const void *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    const uint8_t *p = data;
    size_t left = len;
    for (; left >= 64; left -= 64, p += 64) block(h, p);
    uint8_t tail[128] = { 0 };
    memcpy(tail, p, left);
    tail[left] = 0x80;
    size_t tl = left < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; ++i) tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
    block(h, tail);
    if (tl == 128) block(h, tail + 64);
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) out[4 * i + j] = (uint8_t)(h[i] >> (24 - 8 * j));
}

int base64_encode(const void *data, size_t len, char *out, size_t cap)
{
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t *p = data;
    if (cap < (len + 2) / 3 * 4 + 1) return -1;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < len ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < len ? p[i + 2] : 0);
        out[o++] = tab[v >> 18 & 63];
        out[o++] = tab[v >> 12 & 63];
        out[o++] = i + 1 < len ? tab[v >> 6 & 63] : '=';
        out[o++] = i + 2 < len ? tab[v & 63] : '=';
    }
    out[o] = 0;
    return 0;
}
