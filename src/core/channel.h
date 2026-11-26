/* SPDX-License-Identifier: GPL-3.0-or-later */
/* channel.h — a camera channel: source -> au_norm -> decoder + recorder.
 *
 * A thread per channel opens the camera's main stream (any protocol,
 * source.h), normalizes its access units, records them as-is
 * (seg_writer.h) and decodes them into the channel's tile when the
 * platform has a decoder (hal_decoder.h, sized from the stream's SPS;
 * rebuilt when the stream changes). While the tile is small (a quarter of
 * the screen or less) a second thread brings the camera's sub-stream and
 * decodes that instead; the main stream is still recorded, through its
 * own connection, whatever the view.
 * Sources the platform displays itself (local capture) are only recorded.
 * Recording does not depend on decoding. A decoder that cannot be had (the
 * platform out of memory or decoder channels) is asked for again after a
 * while, or at once when another channel frees one. Lost connections
 * reconnect with a 1 -> 10 s back-off.
 */
#ifndef CHANNEL_H
#define CHANNEL_H

#include <stdint.h>

#include "vg_types.h"

#define CHANNEL_MAX  16

typedef struct {
    const char *urls[CHANNEL_MAX];  /* source URLs, in tile order; NULL: an empty tile */
    const char *names[CHANNEL_MAX]; /* optional display names (NULL or "") */
    /* Optional sub-streams, shown in small tiles (NULL or "": derived from
     * the URL where the protocol knows how, source_sub_url; else none) */
    const char *sub_urls[CHANNEL_MAX];
    int         n;
    const char *rec_dir;            /* NULL: do not record */
    int         seg_secs;           /* segment length */
    int         keep;               /* segments kept per camera, 0 = all */
    int         allow_rootfs;       /* record onto the root filesystem too (rec_place.h) */
} channels_config_t;

typedef struct {
    int        configured;          /* a source is set for this tile */
    int        live;                /* video arrived within the last 3 s */
    int        decoding;            /* a decoder is running */
    char       view[8];             /* the stream on screen (or about to be): "main",
                                       "sub", or "" (off screen, or none to decode);
                                       codec, size and fps are that stream's */
    int        recording;           /* a segment is open */
    int        motion;              /* the camera reports motion; always 0 for now:
                                       no source reports it yet */
    char       rec_state[24];       /* "" (no recording asked for); set up: "recording",
                                       "ready" (no segment open yet: waiting for a keyframe
                                       or the camera), "failed" (the last write failed); not
                                       set up: "waiting for disk", "not a disk", "read-only",
                                       "error" */
    uint64_t   rec_bytes;
    vg_codec_t codec;
    unsigned   width, height, fps;  /* fps averaged over 2 s */
    char       host[64];
    char       name[32];
    char       state[48];           /* "CONNECTING", the last error, ... */
} channel_status_t;

/* Start every configured channel. `tiles` (may be NULL) are the screen
 * rectangles for the decoders' output, CHANNEL_MAX of them. Returns 0, or
 * -1 on a bad URL (nothing is left running). */
int  channels_start(const channels_config_t *cfg, const vg_rect_t *tiles);

/* Make the running channels match `cfg`, from the thread that called
 * channels_start: a channel whose URL and recording settings are unchanged
 * keeps running (a new name is just taken over), the others are stopped
 * and started again, together. Events say what happened. Returns the
 * number of channels that could not be started. */
int  channels_apply(const channels_config_t *cfg);
void channels_stop(void);

/* Show channel i's picture in tiles[i] (CHANNEL_MAX of them; w == 0: not
 * shown), e.g. for another layout or one channel full screen. From the
 * thread that calls channels_apply. Running channels move at once, the
 * others start in their tile; sources the platform shows itself (local
 * capture) stay where the platform put them. Where the decoders do not
 * stretch to every channel (hal_caps()->dec_shown_only), a channel that
 * leaves the screen frees its decoder and gets one again at the first
 * keyframe after it comes back; recording is not affected. */
void channels_set_tiles(const vg_rect_t *tiles);

/* The screen's size, which tile sizes are judged by: a tile of a quarter
 * of it or less shows a camera's sub-stream, a larger one its main stream.
 * Unset (0), the main stream everywhere. Before channels_start. */
void channels_set_screen(unsigned w, unsigned h);

/* Snapshot of channel `idx` (0-based). Any thread, at any time. */
void channels_status(int idx, channel_status_t *st);
/* Tiles in use: the last configured channel + 1. */
int  channels_count(void);

#endif /* CHANNEL_H */
