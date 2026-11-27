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
#include <time.h>

#include "vg_types.h"

#define CHANNEL_MAX  16

typedef struct {
    const char *urls[CHANNEL_MAX];  /* source URLs, in tile order; NULL: an empty tile */
    const char *names[CHANNEL_MAX]; /* optional display names (NULL or "") */
    /* Optional sub-streams, shown in small tiles (NULL or "": derived from
     * the URL where the protocol knows how, source_sub_url; else none) */
    const char *sub_urls[CHANNEL_MAX];
    /* A camera in the slot but switched off (its url is NULL): the tile
     * says "Disabled" rather than offering to add one. */
    char        disabled[CHANNEL_MAX];
    int         n;
    const char *rec_dir;            /* NULL: do not record */
    int         seg_secs;           /* segment length */
    int         keep;               /* segments kept per camera, 0 = all */
    int         allow_rootfs;       /* record onto the root filesystem too (rec_place.h) */
    int         stop_when_full;     /* record.on_full "stop": nothing deleted for space */
    uint64_t    quota_bytes[CHANNEL_MAX];   /* per camera, 0: none (seg_set_quota) */
    time_t      quota_age[CHANNEL_MAX];     /* seconds, 0: none */
} channels_config_t;

/* What a channel is doing, for the tile and the API (ch_state_name). */
typedef enum {
    CH_EMPTY = 0,                   /* no camera in this slot */
    CH_DISABLED,                    /* a camera, switched off */
    CH_CONNECTING,
    CH_LIVE,                        /* connected; video arrives (or `live` says not) */
    CH_NO_VIDEO,                    /* connected, but no video for a while */
    CH_OFFLINE,                     /* cannot be reached: refused, timed out, no route, lost */
    CH_AUTH,                        /* the camera refused the user or the password */
    CH_CODEC,                       /* its video is not H.264 or H.265 */
    CH_ERROR,                       /* anything else: `state` has the text */
} ch_state_t;

/* "empty", "disabled", "connecting", "live", "no_video", "offline",
 * "auth", "codec", "error" */
const char *ch_state_name(ch_state_t s);

/* Events a camera reports, as bits of channel_status_t.events. Each stays
 * set for CHANNEL_EVENT_HOLD seconds after the camera last reported it.
 * Room for more (line crossing, intrusion) after these. */
enum {
    CH_EV_MOTION  = 1u << 0,
    CH_EV_HUMAN   = 1u << 1,
    CH_EV_FACE    = 1u << 2,
    CH_EV_VEHICLE = 1u << 3,
    CH_EV_TYPES   = 4,
};
#define CHANNEL_EVENT_HOLD 5
/* "motion", "human", "face", "vehicle"; NULL past the last. */
const char *ch_event_name(int bit);

/* A camera of channel `idx` reports events `bits` now (no source does
 * yet: NETIP alarms and ONVIF events are to come). Any thread. */
void channels_report_events(int idx, unsigned bits);
/* The bits still held at `now`, given when each type was last reported
 * (0: never). */
unsigned channel_events_held(const time_t last[CH_EV_TYPES], time_t now);

typedef struct {
    int        configured;          /* a source is set for this tile */
    /* What the stream on screen (`view`; the main one when none is) is
     * doing, `state` has the text: a tile showing the sub-stream is live
     * whatever the main stream does, and the other way round. */
    ch_state_t status;
    unsigned   events;              /* CH_EV_* the camera reports now */
    int        live;                /* video of that stream arrived within the last 3 s */
    /* Each stream on its own: the main one (recorded; the picture in a
     * large tile) and the sub-stream (small tiles; CH_EMPTY and "" while
     * none runs). */
    ch_state_t main_status, sub_status;
    char       main_state[48], sub_state[48];
    int        decoding;            /* a decoder is running */
    int        no_decoder;          /* the tile wants a picture but the platform has no
                                       decoder for it now (memory, decoder channels);
                                       asked for again */
    char       view[8];             /* the stream on screen (or about to be): "main",
                                       "sub", or "" (off screen, or none to decode);
                                       codec, size and fps are that stream's */
    int        recording;           /* a segment is open */
    char       rec_state[24];       /* "" (no recording asked for); set up: "recording",
                                       "ready" (no segment open yet: waiting for a keyframe
                                       or the camera), "failed" (the last write failed),
                                       "disk full" (record.on_full stop), "schedule off",
                                       "waiting for motion" (rec_sched.h), "paused" (held
                                       for disk work, channels_rec_hold); not
                                       set up: "waiting for disk", "not a disk", "read-only",
                                       "error" */
    uint64_t   rec_bytes;
    vg_codec_t codec;
    unsigned   width, height, fps;  /* fps averaged over 2 s */
    char       host[64];
    char       name[32];            /* the configured name, else `title` */
    char       title[32];           /* what the camera calls the channel (NETIP
                                       ChannelTitle); "" when it does not say */
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

/* How pictures fill their tiles (display.video_aspect): 0 fitted at the
 * camera's display aspect ratio, black bars around (aspect.h), 1 stretched
 * over the whole tile. From the thread that calls channels_apply; applies
 * at once. */
void channels_set_stretch(int stretch);

/* Stop recording into `dir` and everything under it (a disk about to be
 * unmounted or formatted): the channels recording there close their
 * segments and open none until channels_rec_release. Waits up to
 * `timeout_ms` for the open segments to close; returns how many are still
 * open (0: the directory is free). Any thread; one hold at a time. */
int  channels_rec_hold(const char *dir, int timeout_ms);
void channels_rec_release(void);

/* Snapshot of channel `idx` (0-based). Any thread, at any time. */
void channels_status(int idx, channel_status_t *st);
/* Tiles in use: the last configured channel + 1. */
int  channels_count(void);

#endif /* CHANNEL_H */
