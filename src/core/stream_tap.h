/* SPDX-License-Identifier: GPL-3.0-or-later */
/* stream_tap.h — the cameras' streams, for vigild's own servers.
 *
 * A server that passes a camera's video on (RTSP, NETIP) opens a tap on
 * one of its streams and gets every access unit the channel's session
 * gets, as au_norm hands them out: Annex-B, nothing before a keyframe,
 * and each keyframe carrying its parameter sets, so it can start a
 * decoder on its own. No re-encoding, no second connection to the
 * camera: the AU a tap gets is the one being recorded and decoded.
 *
 * A tap that opens mid-GOP starts with the stream's GOP so far
 * (gop_cache.h) when the channel keeps one, so a client sees a picture at
 * once instead of at the camera's next keyframe. An open tap makes its
 * stream's GOP kept, and while a server stands by (stream_tap_standby)
 * every stream's GOP is kept as long as the cache has room to spare
 * (half of GOP_CACHE_BUDGET), after what the decoders need.
 *
 * Channels are 0-based here (as channels_status), streams 0 main, 1 sub.
 */
#ifndef STREAM_TAP_H
#define STREAM_TAP_H

#include <stddef.h>
#include <stdint.h>

#include "au_norm.h"
#include "gop_cache.h"
#include "vg_types.h"

typedef struct stream_tap stream_tap_t;

/* Called on the channel's session thread for each video AU (au_norm's output:
 * a keyframe carries its parameter sets); must not block (copy and return). */
typedef void (*stream_tap_fn)(void *user, const vg_au_t *au, const vg_stream_info_t *si, vg_codec_t codec);

/* Subscribe to channel ch's main (sub=0) or sub (sub=1) stream. The first
 * calls replay the stream's cached GOP (gop_cache.h) when there is one, so a
 * client starts at once at a keyframe; else from the next keyframe.
 * Returns NULL if the channel/stream does not exist. Any thread. */
stream_tap_t *stream_tap_open(int ch, int sub, stream_tap_fn fn, void *user);

/* After it returns, fn is never called again for this tap. Any thread (not from fn). */
void stream_tap_close(stream_tap_t *t);

/* Whether the stream exists (its channel runs, with that stream), known
 * or not yet. */
int stream_tap_exists(int ch, int sub);

/* What the stream is now: 0 and *si, *codec filled when known (SPS seen), -1 else. */
int stream_tap_info(int ch, int sub, vg_stream_info_t *si, vg_codec_t *codec);

/* The stream's parameter sets, from its last keyframe, for an SDP
 * (sprop-parameter-sets, sprop-vps/sps/pps): H.264 SPS, PPS; H.265 VPS,
 * SPS, PPS, each without a start code. Returns how many (0: none seen
 * yet, or no such stream). */
typedef struct {
    vg_codec_t codec;
    int        n;
    uint8_t    ps[AU_NORM_PS_MAX][AU_NORM_PS_BYTES];
    size_t     len[AU_NORM_PS_MAX];
} stream_tap_ps_t;
int stream_tap_params(int ch, int sub, stream_tap_ps_t *ps);

/* A server listening for clients that may open taps any time: while any
 * stands by (counted; on 1, off 0), every stream's GOP is kept when the
 * cache has room to spare, so even the first client of a stream that is
 * not on screen starts at once. */
void stream_tap_standby(int on);

/* ---- for channel.c ------------------------------------------------------- */

enum { TAP_KEEP_NONE = 0, TAP_KEEP_SPARE = 1, TAP_KEEP_MUST = 2 };
/* Whether the stream's GOP should be kept for the taps: TAP_KEEP_MUST
 * while one is open, TAP_KEEP_SPARE while a server stands by. */
int stream_tap_keep(int ch, int sub);

/* The stream exists (its session runs) or not any more. */
void stream_tap_present(int ch, int sub, int present);

/* One AU of the stream, from its session thread; `si` and `gen` are its
 * au_norm's info and info_gen, `gop` its GOP so far when that holds this
 * AU as its last (NULL otherwise). */
void stream_tap_feed(int ch, int sub, const vg_au_t *au, const vg_stream_info_t *si, unsigned gen,
                     const gop_cache_t *gop);

#endif /* STREAM_TAP_H */
