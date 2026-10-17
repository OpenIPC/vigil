/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rtp_depay.h — RTP -> Annex-B access units for H.264 (RFC 6184) and
 * H.265 (RFC 7798).
 *
 * Handles single NAL units, STAP-A / AP aggregation and FU-A / FU
 * fragmentation (no DONL, no interleaving — what IP cameras send). An
 * access unit ends on the RTP marker bit or on a timestamp change. An AU
 * that lost packets is dropped, and the next one is flagged `discont`
 * (au_norm.c then waits for a keyframe). Parameter sets and keyframe
 * handling are au_norm's job.
 */
#ifndef RTP_DEPAY_H
#define RTP_DEPAY_H

#include "vg_types.h"

/* Called once per complete access unit. */
typedef void (*depay_au_cb)(void *user, const vg_au_t *au);

typedef struct {
    vg_codec_t codec;
    uint8_t   *au;
    size_t     au_len, au_cap;
    uint32_t   ts;
    uint16_t   seq;
    int        have_ts, have_seq;
    int        fu_active;
    int        au_broken;               /* loss or overflow inside this AU */
    int        discont;                 /* flag the next emitted AU */
    uint64_t   ts_hi;                   /* RTP timestamp unwrap */
    uint32_t   last_ts;
    uint64_t   lost;                    /* packets lost (sequence gaps) */
} depay_t;

int  depay_init(depay_t *d, vg_codec_t codec);
void depay_free(depay_t *d);

/* Feed one RTP packet (header included). Returns 0, or -1 if malformed. */
int depay_push(depay_t *d, const uint8_t *pkt, size_t len, depay_au_cb cb, void *user);

#endif /* RTP_DEPAY_H */
