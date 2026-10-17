/* SPDX-License-Identifier: GPL-3.0-or-later */
/* au_norm.h — normalize access units from any source.
 *
 * Sources (RTSP, NETIP, local capture, ...) hand over Annex-B access units
 * as they come. This stage makes them uniform for the decoder and the
 * recorder:
 *
 *   - parameter sets (H.264 SPS/PPS, H.265 VPS/SPS/PPS) are cached from
 *     the stream and from out-of-band sources (SDP), and put in front of a
 *     keyframe that arrives without them, so every keyframe can start a
 *     decoder or a recorded file on its own;
 *   - units without a picture (parameter sets or SEI that some cameras send
 *     as their own "frame") are carried into the next picture;
 *   - nothing is emitted before the first keyframe, nor after a
 *     discontinuity until the next one;
 *   - the SPS gives the stream's size and reference count (au_norm_info),
 *     and `info_gen` changes whenever they do.
 */
#ifndef AU_NORM_H
#define AU_NORM_H

#include "vg_types.h"

#define AU_NORM_PS_MAX    3
#define AU_NORM_PS_BYTES  256

typedef void (*au_emit_fn)(void *user, const vg_au_t *au);

typedef struct {
    vg_codec_t codec;
    uint8_t    ps[AU_NORM_PS_MAX][AU_NORM_PS_BYTES];
    size_t     ps_len[AU_NORM_PS_MAX];
    uint8_t   *pend;                    /* picture-less units awaiting a picture */
    size_t     pend_len, pend_cap;
    int        pend_sps;
    uint8_t   *out;
    size_t     out_cap;
    int        need_key;
    vg_stream_info_t info;
    unsigned   info_gen;                /* 0 = no SPS seen yet */
} au_norm_t;

int  au_norm_init(au_norm_t *n, vg_codec_t codec);
void au_norm_free(au_norm_t *n);

/* After a reconnect: wait for a keyframe again (the cache is kept). */
void au_norm_reset(au_norm_t *n);

/* Seed the cache with one parameter-set NAL unit (no start code). */
void au_norm_add_param_set(au_norm_t *n, const uint8_t *nal, size_t len);

/* Feed one access unit; emits zero or one normalized AU. */
void au_norm_push(au_norm_t *n, const vg_au_t *in, au_emit_fn emit, void *user);

/* Walk the NAL units of an Annex-B buffer: returns the next unit (without
 * its start code) at or after *pos, or NULL at the end. */
const uint8_t *au_next_nal(const uint8_t *buf, size_t len, size_t *pos, size_t *nal_len);

#endif /* AU_NORM_H */
