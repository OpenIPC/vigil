/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rec_pool.h — one recording target's pool of chunks (docs/recording.md).
 *
 * The part of the store that touches the disk, without threads of its
 * own: rec_store.c runs it on the writer thread (and the rebuild and the
 * pre-allocation on a background thread), the tests drive it directly.
 * Everything a reader needs (the chunk table, the open chunks' fragment
 * tables) is guarded by `mu`; the writer's own state is the writer's.
 *
 *   <path>/vigil-rec.id   pool id, owner, host, chunk size, chunks wanted
 *   <path>/pool/c<6 digits>.mp4
 *   <path>/index/chunks.a, chunks.b, journal, events.log
 */
#ifndef REC_POOL_H
#define REC_POOL_H

#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include "rec_fmt.h"
#include "vg_types.h"

#define REC_CAMS        16
#define REC_STREAMS     2
#define REC_PS_BYTES    256
#define REC_EV_HOLD_US  5000000ull       /* an event lasts until it was last reported this long ago */
#define REC_IDLE_CLOSE  60              /* a stream silent this long closes its chunk (s) */

/* The pool's file operations, replaceable by the power-cut tests. */
typedef struct {
    ssize_t (*pwrite)(int fd, const void *buf, size_t len, off_t off);
    int     (*fdatasync)(int fd);
    void    *user;
} rec_io_t;

typedef struct {
    char     path[192];
    unsigned chunk_mb;                  /* for a new pool */
    unsigned pool_mb;                   /* the most the pool may take (0: the target less the reserve) */
    unsigned fragment_s;
    unsigned flush_s;                   /* 0: by the medium (rec_media_flush) */
    unsigned index_save_s;
    int      stop_when_full;
    uint64_t quota_bytes[REC_CAMS];
    time_t   quota_age[REC_CAMS];
    char     owner[64], host[64];
} rec_pool_cfg_t;

/* An access unit handed to the writer: NAL units with 4-byte big-endian
 * lengths (no start codes), the stream's parameter sets left out of it
 * and carried beside it (a keyframe's). */
typedef struct {
    uint8_t        cam, stream;         /* 0-based camera */
    uint8_t        key, gap;            /* gap: data was dropped or the stream stopped before it */
    vg_codec_t     codec;
    uint64_t       pts_us, wall_us;
    const uint8_t *data;
    uint32_t       len;
    int            n_ps;
    const uint8_t *ps[3];
    uint16_t       ps_len[3];
} rec_au_t;

typedef struct {
    uint32_t size;
    int64_t  p;                         /* presentation, absolute 90 kHz ticks */
    uint8_t  key;
} rec_wsamp_t;

/* A stream's writer state. */
typedef struct {
    /* time: P = anchor_tick + (pts - anchor_pts) at 90 kHz */
    int        clock;
    uint64_t   anchor_pts;
    int64_t    anchor_tick;
    int64_t    last_p, last_dur, last_end;
    int64_t    last_raw, avg, sum_int;      /* the camera's last time; the average interval */
    int        n_int;                       /* intervals measured for the first average */
    int        bframes;                     /* seen: its times are taken as they are */
    /* the open chunk */
    int        open;
    uint32_t   idx;
    int        fd;
    rec_chunk_t rec;                    /* also in the table, under mu */
    int64_t    start_tick;
    vg_codec_t codec;
    unsigned   width, height;
    int        n_ps;
    uint8_t    ps[3][REC_PS_BYTES];
    uint16_t   ps_len[3];
    /* the fragment being written */
    int        frag_open, frag_key, frag_gap;
    uint32_t   frag_at, data_at, cursor, reserve_n;
    rec_wsamp_t *smp;
    uint32_t   n_smp, cap_smp;
    uint32_t   max_frag_bytes, max_frag_smp;
    /* the chunk's fragments (under mu: readers copy them) */
    rec_frag_t *frags;
    uint32_t   nfrag, cap_frag, journaled;
    rec_event_t *ev;                    /* events for vgix (under mu) */
    uint32_t   nev, cap_ev;
    int        dirty, need_key, gap_next;
    time_t     last_au;
    /* status (under mu) */
    uint64_t   bytes_total;
    time_t     last_write;
    int        failed, full;
} rec_wstream_t;

typedef enum {
    RP_OFF = 0,                         /* no path */
    RP_WAITING,                         /* the place is not usable: `why` */
    RP_READY,                           /* mounted, writing */
    RP_READONLY,                        /* another recorder's pool: playback only */
    RP_ERROR,                           /* cannot be set up: `why` */
} rec_pstate_t;

/* An event being reported: its record in the log, when it began and was
 * last reported. */
typedef struct {
    uint32_t seq;
    uint64_t start, last;
    int      on;
} rec_active_t;

typedef struct {
    pthread_mutex_t mu;
    rec_pool_cfg_t  cfg;
    rec_io_t        io;
    rec_pstate_t    state;
    char            why[160];
    int             mounted;            /* files open */
    char            dir[256];           /* cfg.path */
    uint64_t        pool_id;
    uint32_t        chunk_bytes;
    uint32_t        want;               /* chunks the pool is to have */
    /* the chunk table (under mu) */
    rec_chunk_t    *tab;
    uint32_t        n, cap;             /* n: chunk files ready */
    uint8_t        *known;              /* per chunk: its state is known (a rebuild has not reached it: 0) */
    uint64_t        generation;
    int             table_fd, journal_fd, events_fd;
    uint32_t        journal_len;        /* bytes, header included */
    int             save_due;
    /* the event log (under mu) */
    uint32_t        ev_cap, ev_head;    /* the next event's seq */
    uint64_t        ev_gen;
    rec_active_t    active[REC_CAMS][REC_EV_TYPES];
    rec_event_t    *ev_pend;            /* records to write at the next flush */
    uint32_t        n_ev_pend, cap_ev_pend;
    /* writing */
    rec_wstream_t   ws[REC_CAMS][REC_STREAMS];
    uint8_t        *jbuf;               /* journal records to append at the next flush */
    uint32_t        jlen, jcap;
    unsigned        flush_s;
    char            media[8];           /* "disk", "flash", "share" */
    time_t          next_flush, next_save, next_quota;
    /* background work (under mu) */
    int             rebuilding;
    uint32_t        rebuild_done;
    int             prealloc_running;
    int             events_rebuild;     /* the event log was lost: refill it from the chunks' vgix */
    /* rate (bytes/s over the last 10 s) */
    uint64_t        rate_bytes;
    time_t          rate_at;
    unsigned        rate;
    /* flush hook for the tests: what the disk now holds for sure */
    void          (*on_flush)(void *user);
    void           *on_flush_user;
} rec_pool_t;

void rec_pool_init(rec_pool_t *p);
void rec_pool_free(rec_pool_t *p);

/* Set up or open the pool in cfg->path (which rec_place.h has allowed).
 * 0 mounted (state RP_READY or RP_READONLY), -1 (state, why). Recovers
 * open chunks; with no valid index, sets `rebuilding` (rec_pool_rebuild
 * does the rest). */
int  rec_pool_mount(rec_pool_t *p, const rec_pool_cfg_t *cfg, time_t now);
/* Close every chunk, save, close the files. */
void rec_pool_unmount(rec_pool_t *p);
/* Like a power cut: close the files, write nothing more. */
void rec_pool_abandon(rec_pool_t *p);
/* Settings that change while mounted: quotas, on_full, fragment, flush. */
void rec_pool_update(rec_pool_t *p, const rec_pool_cfg_t *cfg);

/* The writer: one AU of a stream; the stream stops (a gap follows);
 * timers (flush, save, idle chunks, events, age quota). */
void rec_pool_put(rec_pool_t *p, const rec_au_t *au);
void rec_pool_break(rec_pool_t *p, int cam, int stream);
void rec_pool_tick(rec_pool_t *p, time_t now, uint64_t now_us);
/* fdatasync the open chunks, append the journal, write the events. */
int  rec_pool_flush(rec_pool_t *p);
/* Flush and save the chunk table to its older copy, empty the journal. */
int  rec_pool_save(rec_pool_t *p);
/* Close a stream's chunk now. */
void rec_pool_close_stream(rec_pool_t *p, int cam, int stream);

/* Background: read every chunk's header the table does not know yet
 * (rebuild), recover open ones; then refill a lost event log. Runs until
 * done; `stop` is watched. */
void rec_pool_rebuild(rec_pool_t *p, volatile int *stop);
/* Background: create the chunk files still wanted. */
void rec_pool_prealloc(rec_pool_t *p, volatile int *stop);
/* Is there background work for the two above? */
int  rec_pool_rebuild_due(rec_pool_t *p);
int  rec_pool_prealloc_due(rec_pool_t *p);

/* An event of camera `cam` (0-based) of `type` (REC_EV_*) reported now. */
void rec_pool_event(rec_pool_t *p, int cam, int type, uint64_t now_us);

/* Free the chunks of `cam` (1-based; 0 every camera) wholly inside
 * [from, to); returns how many. */
int  rec_pool_delete(rec_pool_t *p, int cam, uint64_t from_us, uint64_t to_us);

/* Take over another recorder's pool (RP_READONLY): it becomes ours. */
int  rec_pool_takeover(rec_pool_t *p, time_t now);

/* ---- reading (any thread; rec_read.c) ---- */

/* A chunk's fragment table: from the writer's RAM while it is open, from
 * its vgix when closed. *rec gets the chunk record it belongs to. Returns
 * the count (*out malloc'ed), or -1. */
int  rec_pool_frags(rec_pool_t *p, uint32_t idx, rec_chunk_t *rec, rec_frag_t **out);
/* The chunk's events from its vgix (closed) or RAM (open). */
int  rec_pool_chunk_events(rec_pool_t *p, uint32_t idx, rec_event_t **out);
/* The chunk file's path. */
void rec_pool_chunk_path(const rec_pool_t *p, uint32_t idx, char *out, size_t cap);

/* What the medium under `dir` is ("disk", "flash", "share") and its
 * default flush interval, from the mount and /sys/block's rotational flag. */
unsigned rec_media_flush(const char *dir, char *kind, size_t cap);

#endif /* REC_POOL_H */
