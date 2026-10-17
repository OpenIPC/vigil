/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rtp_depay.c — RTP -> Annex-B access units (see rtp_depay.h). */
#include <stdlib.h>
#include <string.h>

#include "rtp_depay.h"

#define AU_MAX  (4u << 20)      /* largest access unit accepted: 4 MiB */

static const uint8_t START_CODE[4] = { 0, 0, 0, 1 };

int depay_init(depay_t *d, vg_codec_t codec)
{
    memset(d, 0, sizeof *d);
    d->codec  = codec;
    d->au_cap = 512 * 1024;
    d->au     = malloc(d->au_cap);
    return d->au ? 0 : -1;
}

void depay_free(depay_t *d)
{
    free(d->au);
    memset(d, 0, sizeof *d);
}

static void au_append(depay_t *d, const uint8_t *p, size_t n)
{
    if (d->au_broken) return;
    size_t need = d->au_len + n;
    if (need > d->au_cap) {
        size_t cap = d->au_cap;
        while (cap < need) cap *= 2;
        uint8_t *q = need <= AU_MAX ? realloc(d->au, cap > AU_MAX ? AU_MAX : cap) : NULL;
        if (!q) { d->au_broken = 1; return; }
        d->au = q;
        d->au_cap = cap > AU_MAX ? AU_MAX : cap;
    }
    memcpy(d->au + d->au_len, p, n);
    d->au_len += n;
}

static void nal_begin(depay_t *d, const uint8_t *hdr, size_t hdr_len)
{
    au_append(d, START_CODE, sizeof START_CODE);
    au_append(d, hdr, hdr_len);
}

static void nal_whole(depay_t *d, const uint8_t *nal, size_t len)
{
    au_append(d, START_CODE, sizeof START_CODE);
    au_append(d, nal, len);
}

static void au_flush(depay_t *d, depay_au_cb cb, void *user)
{
    if (d->au_broken) {
        d->discont = 1;
    } else if (d->au_len) {
        /* 90 kHz RTP timestamp -> 64-bit microseconds. */
        if (d->ts < d->last_ts && d->last_ts - d->ts > 0x80000000u) d->ts_hi += 1ull << 32;
        d->last_ts = d->ts;
        vg_au_t au = {
            .codec   = d->codec,
            .data    = d->au,
            .len     = d->au_len,
            .pts_us  = (d->ts_hi + d->ts) * 100 / 9,
            .discont = d->discont,
        };
        cb(user, &au);
        d->discont = 0;
    }
    d->au_len = 0;
    d->au_broken = 0;
    d->fu_active = 0;
}

static void push_h264(depay_t *d, const uint8_t *p, size_t n)
{
    int type = p[0] & 0x1f;
    if (type >= 1 && type <= 23) {
        d->fu_active = 0;
        nal_whole(d, p, n);
    } else if (type == 24) {                            /* STAP-A */
        d->fu_active = 0;
        size_t o = 1;
        while (o + 2 <= n) {
            size_t sz = ((size_t)p[o] << 8) | p[o + 1];
            o += 2;
            if (sz == 0 || o + sz > n) { d->au_broken = 1; return; }
            nal_whole(d, p + o, sz);
            o += sz;
        }
    } else if (type == 28) {                            /* FU-A */
        if (n < 3) { d->au_broken = 1; return; }
        uint8_t fu = p[1];
        if (fu & 0x80) {
            uint8_t hdr = (p[0] & 0xe0) | (fu & 0x1f);
            nal_begin(d, &hdr, 1);
            d->fu_active = 1;
        } else if (!d->fu_active) {
            d->au_broken = 1;                           /* missed the start */
            return;
        }
        au_append(d, p + 2, n - 2);
        if (fu & 0x40) d->fu_active = 0;
    }
    /* Other types (STAP-B, MTAP, FU-B) are not sent by cameras; ignore. */
}

static void push_h265(depay_t *d, const uint8_t *p, size_t n)
{
    if (n < 3) { d->au_broken = 1; return; }
    int type = (p[0] >> 1) & 0x3f;
    if (type < 48) {
        d->fu_active = 0;
        nal_whole(d, p, n);
    } else if (type == 48) {                            /* AP */
        d->fu_active = 0;
        size_t o = 2;
        while (o + 2 <= n) {
            size_t sz = ((size_t)p[o] << 8) | p[o + 1];
            o += 2;
            if (sz < 2 || o + sz > n) { d->au_broken = 1; return; }
            nal_whole(d, p + o, sz);
            o += sz;
        }
    } else if (type == 49) {                            /* FU */
        uint8_t fu = p[2];
        if (fu & 0x80) {
            uint8_t hdr[2] = { (uint8_t)((p[0] & 0x81) | ((fu & 0x3f) << 1)), p[1] };
            nal_begin(d, hdr, 2);
            d->fu_active = 1;
        } else if (!d->fu_active) {
            d->au_broken = 1;
            return;
        }
        au_append(d, p + 3, n - 3);
        if (fu & 0x40) d->fu_active = 0;
    }
    /* 50 (PACI) and reserved types: ignore. */
}

int depay_push(depay_t *d, const uint8_t *pkt, size_t len, depay_au_cb cb, void *user)
{
    if (len < 12 || (pkt[0] >> 6) != 2) return -1;
    size_t hl = 12 + 4 * (pkt[0] & 0x0f);
    if (pkt[0] & 0x10) {                                /* header extension */
        if (len < hl + 4) return -1;
        hl += 4 + 4 * (((size_t)pkt[hl + 2] << 8) | pkt[hl + 3]);
    }
    size_t end = len;
    if (pkt[0] & 0x20) {                                /* padding */
        if (pkt[len - 1] > len) return -1;
        end -= pkt[len - 1];
    }
    if (hl >= end) return -1;

    int      marker = pkt[1] & 0x80;
    uint16_t seq    = (uint16_t)((pkt[2] << 8) | pkt[3]);
    uint32_t ts     = ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16) |
                      ((uint32_t)pkt[6] << 8) | pkt[7];

    int gap = d->have_seq && seq != (uint16_t)(d->seq + 1);
    if (gap) d->lost += (uint16_t)(seq - d->seq - 1);
    d->seq = seq;
    d->have_seq = 1;

    /* A new timestamp ends the previous AU when its marker bit was lost.
     * If packets went missing too, they were its tail or this AU's head —
     * there is no telling which, so neither is trusted. */
    if (d->have_ts && ts != d->ts && (d->au_len || d->au_broken)) {
        if (gap) d->au_broken = 1;
        au_flush(d, cb, user);
    }
    /* After any flush: the gap breaks the AU this packet belongs to (e.g.
     * the first slice of a multi-slice IDR went missing). */
    if (gap) d->au_broken = 1;
    d->ts = ts;
    d->have_ts = 1;

    if (d->codec == VG_CODEC_H264) push_h264(d, pkt + hl, end - hl);
    else                           push_h265(d, pkt + hl, end - hl);

    if (marker) au_flush(d, cb, user);
    return 0;
}
