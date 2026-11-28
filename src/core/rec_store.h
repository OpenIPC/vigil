/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rec_store.h — the recording store: where the cameras' streams are
 * recorded (docs/recording.md).
 *
 * One target (record.path) holds a pool of pre-allocated fragmented-MP4
 * chunks used as a ring, with a chunk table, a journal and an event log
 * of its own (rec_pool.h). A writer thread does every write: the camera
 * sessions hand it their access units through a bounded queue (a copy,
 * never waiting on the disk; ~10 s at the measured rate, at most
 * REC_QUEUE_MAX), and a stream whose AUs do not fit drops to its next
 * keyframe, the gap marked. A background thread rebuilds a lost index
 * and creates the chunk files still wanted. The target is used only
 * where rec_place.h allows, checked again every few seconds; it is set
 * up by itself once its disk is there.
 *
 * Cameras are 0-based here (as channel.c), streams 0 main, 1 sub.
 */
#ifndef REC_STORE_H
#define REC_STORE_H

#include <stddef.h>
#include <stdint.h>

#include "rec_pool.h"
#include "vg_types.h"

#define REC_QUEUE_MIN  (4u << 20)
#define REC_QUEUE_MAX  (24u << 20)

typedef struct rec_store_cfg {
    rec_pool_cfg_t pool;                /* pool.path "": no recording */
    int            allow_rootfs;
} rec_store_cfg_t;

/* Apply settings (from the main thread): a new path closes the pool and
 * opens the new one; the rest applies at once. */
void rec_store_configure(const rec_store_cfg_t *cfg);
/* Close every chunk, save the index, stop the threads. */
void rec_store_shutdown(void);

/* An AU of a stream, as au_norm gave it (Annex-B, a keyframe with its
 * parameter sets). 0 queued, 1 not recording (no pool), -1 dropped (the
 * queue is full: the stream waits for its next keyframe). Any thread. */
int  rec_store_put(int cam, int stream, const vg_au_t *au);
/* The writer's copy of an AU: `head` bytes of the caller's, then the
 * rec_au_t, then the data it points to; free() the start. NULL when the
 * AU holds nothing to record. */
rec_au_t *rec_au_from(int cam, int stream, const vg_au_t *au, uint64_t wall_us, size_t head);

/* The stream stops (a reconnect, a pause): what comes next follows a gap. */
void rec_store_break(int cam, int stream);
/* An event (REC_EV_*) of camera `cam` reported now. Any thread. */
void rec_store_event(int cam, int type);

/* Disk work under `dir` (an unmount, a format): the pool there closes
 * its files and opens none until rec_store_release. 0 when it is closed
 * within `timeout_ms` (or was not there), 1 still open. */
int  rec_store_hold(const char *dir, int timeout_ms);
void rec_store_release(void);

typedef struct {
    uint64_t oldest_us, newest_us, bytes;
    uint32_t chunks;
} rec_store_cam_t;

typedef struct {
    rec_pstate_t state;
    char         why[160];              /* waiting, error, read-only: why */
    char         place[24];             /* RP_WAITING: rec_place_name */
    char         path[192];
    uint64_t     pool_id;
    uint32_t     chunk_mb, chunks, want, free, open, closed, bad;
    uint64_t     size_bytes, used_bytes;
    int          rebuilding;
    uint32_t     rebuild_done;
    unsigned     rate;                  /* bytes/s written, over 10 s */
    unsigned     flush_s;
    char         media[8];
    uint32_t     queue_bytes, queue_max;
    uint64_t     dropped;               /* AUs dropped for a full queue */
    uint64_t     oldest_us, newest_us;
    rec_store_cam_t cam[REC_CAMS];
} rec_store_status_t;

void rec_store_status(rec_store_status_t *st);

typedef struct {
    int      on;                        /* a pool to record into */
    int      open;                      /* a chunk open */
    int      writing;                   /* written in the last 3 s */
    int      failed, full, dropping;
    uint64_t bytes;
} rec_stream_status_t;

void rec_store_stream(int cam, int stream, rec_stream_status_t *s);

/* The pool, for rec_read.h's queries (they check it is mounted). */
rec_pool_t *rec_store_pool(void);

/* Take over another recorder's pool; free the chunks of `cam` (1-based,
 * 0 all) wholly inside [from, to). Done by the writer thread. */
int  rec_store_takeover(void);
int  rec_store_delete(int cam, uint64_t from_us, uint64_t to_us);

#endif /* REC_STORE_H */
