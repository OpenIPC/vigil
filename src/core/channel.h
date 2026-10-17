/* SPDX-License-Identifier: GPL-3.0-or-later */
/* channel.h — a camera channel: source -> au_norm -> decoder + recorder.
 *
 * One thread per channel. It opens the source (any protocol, source.h),
 * normalizes its access units, decodes them into the channel's tile when
 * the platform has a decoder (hal_decoder.h, sized from the stream's SPS;
 * rebuilt when the stream changes) and records them as-is (seg_writer.h).
 * Sources the platform displays itself (local capture) are only recorded.
 * Recording does not depend on decoding. Lost connections reconnect with a
 * 1 -> 10 s back-off.
 */
#ifndef CHANNEL_H
#define CHANNEL_H

#include <stdint.h>

#include "vg_types.h"

#define CHANNEL_MAX  16

typedef struct {
    const char *urls[CHANNEL_MAX];  /* source URLs, in tile order */
    int         n;
    const char *rec_dir;            /* NULL: do not record */
    int         seg_secs;           /* segment length */
    int         keep;               /* segments kept per camera, 0 = all */
} channels_config_t;

typedef struct {
    int        configured;          /* a source is set for this tile */
    int        live;                /* frames are arriving */
    int        decoding;            /* a decoder is running */
    int        recording;           /* a segment is open */
    uint64_t   rec_bytes;
    vg_codec_t codec;
    unsigned   width, height, fps;
    char       host[64];
    char       state[48];           /* "CONNECTING", the last error, ... */
} channel_status_t;

/* Start every configured channel. `tiles` (may be NULL) are the screen
 * rectangles for the decoders' output, n of them. Returns 0, or -1 on a
 * bad URL. */
int  channels_start(const channels_config_t *cfg, const vg_rect_t *tiles);
void channels_stop(void);

/* Snapshot of channel `idx` (0-based). */
void channels_status(int idx, channel_status_t *st);
int  channels_count(void);

#endif /* CHANNEL_H */
