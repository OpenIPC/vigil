/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rec_read.h — questions to a recording pool and the video it gives back
 * (docs/recording.md, "Reading"): from the chunk table in RAM, the event
 * log and, for one chunk at a time, its fragment table. Any thread.
 *
 * Times are UTC microseconds; cameras are 1-based, streams 0 main, 1 sub.
 */
#ifndef REC_READ_H
#define REC_READ_H

#include <stddef.h>
#include <stdint.h>

#include "mp4.h"
#include "rec_pool.h"

/* Days of `year`-`month` (local time) with video: bit d-1 for day d. */
int rec_q_days(rec_pool_t *p, int cam, int stream, int year, int month, uint32_t *mask);

typedef struct {
    uint64_t from_us, to_us;
} rec_span_t;

/* The recorded spans inside [from, to), in time order (*out malloc'ed);
 * returns how many, or -1. */
int rec_q_spans(rec_pool_t *p, int cam, int stream, uint64_t from_us, uint64_t to_us, rec_span_t **out);

/* Events (camera 0 / type 0: any) starting in [from, to), oldest first or
 * newest first, at most `limit` (*out malloc'ed). */
int rec_q_events(rec_pool_t *p, int cam, int type, uint64_t from_us, uint64_t to_us, int newest_first, int limit,
                 rec_event_t **out);

/* A fragment to send. */
typedef struct {
    uint32_t chunk, use, off, size;     /* size: moof + mdat */
    uint64_t chunk_start_us;
    uint32_t t_ms, dur_ms;
    uint8_t  key, gap;
} rec_piece_t;

typedef struct {
    rec_piece_t *p;
    int          n;
    uint8_t     *init;                  /* the first chunk's init segment */
    uint32_t     init_len;
    char         codecs[64];
    uint16_t     width, height;
    int          format_change;         /* stopped at a chunk of another format: at next_us */
    uint64_t     next_us;               /* where the fragments after these start (0: none known) */
    int64_t      first_tick;            /* the first fragment's decode time, absolute 90 kHz */
    uint64_t     bytes;                 /* the fragments' bytes */
} rec_plan_t;

/* The fragments of a stream for [from, to): from the one holding `from`
 * (back to its keyframe), or with `cont` from the first starting at or
 * after `from` (a player asking for what follows); at most `max` (0: no
 * limit). Stops at a change of format. 0 (pl->n may be 0), or -1. */
int  rec_q_plan(rec_pool_t *p, int cam, int stream, uint64_t from_us, uint64_t to_us, int max, int cont,
                rec_plan_t *pl);
void rec_plan_free(rec_plan_t *pl);

/* Where a plan's fragments go: 0 to go on, -1 to stop. */
typedef int (*rec_sink_fn)(void *user, const void *data, size_t len);

enum { REC_GAPS_KEEP = 0, REC_GAPS_REMOVE = 1 };

/* The plan's fragments, their decode times re-based: KEEP, absolute time
 * less `base_tick` (gaps stay on the clock); REMOVE, one after the other.
 * Sequence numbers from `seq0`. 0, or -1 (a read failed, a chunk was
 * reused meanwhile, or the sink stopped). */
int rec_q_send(rec_pool_t *p, const rec_plan_t *pl, int64_t base_tick, int gaps, uint32_t seq0, rec_sink_fn fn,
               void *user);

/* An export's init segment: the plan's, with an edit list that starts
 * the picture at `from` and ends it at `to` (on the plan's clock after
 * REMOVE when `gaps` says so). */
int rec_q_export_init(const rec_plan_t *pl, uint64_t from_us, uint64_t to_us, int gaps, mp4_buf_t *out);

#endif /* REC_READ_H */
