/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rec_fmt.h — the recording store's records on the disk (docs/recording.md).
 *
 * Every record is a fixed little-endian layout with a CRC32C, encoded and
 * decoded here byte by byte (never by casting a struct onto the disk), so
 * a disk written on one recorder reads on any other and on a PC:
 *
 *   chunk record   64 B   the chunk table (index/chunks.a, .b) and, twice,
 *                         the chunk's own header (vgch): what a chunk holds
 *   journal record 32 B   index/journal: what changed since the table was saved
 *   event record   32 B   index/events.log, and the events in a chunk's vgix
 *   fragment entry 16 B   a chunk's fragment table (vgix, and RAM while open)
 *
 * Each CRC starts from the pool id, so a record of another pool (a disk
 * moved over, a chunk file copied in) never passes for one of this pool.
 */
#ifndef REC_FMT_H
#define REC_FMT_H

#include <stddef.h>
#include <stdint.h>

#define REC_CHUNK_REC   64
#define REC_JREC        32
#define REC_EVREC       32
#define REC_FRAG        16
#define REC_VGCH_SIZE   4096            /* the chunk header box; the init segment follows */
#define REC_VGCH_A      64              /* the copy written when the chunk opens */
#define REC_VGCH_B      2048            /* the copy rewritten when it closes (another sector) */
#define REC_TABLE_HDR   64
#define REC_JOURNAL_HDR 32
#define REC_EVLOG_HDR   4096            /* two header copies, at 0 and 512, then the records */
#define REC_NONE        0xFFFFFFFFu

/* 'uuid' box user types: vigil's chunk header and fragment table. */
extern const uint8_t REC_UUID_VGCH[16], REC_UUID_VGIX[16];

uint32_t rec_crc32c(uint32_t crc, const void *data, size_t len);
/* The CRC every record starts from. */
uint32_t rec_crc_seed(uint64_t pool_id);

/* ---- little-endian helpers ---- */
static inline void rec_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void rec_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void rec_put64(uint8_t *p, uint64_t v) { rec_put32(p, (uint32_t)v); rec_put32(p + 4, (uint32_t)(v >> 32)); }
static inline uint16_t rec_get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t rec_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint64_t rec_get64(const uint8_t *p) { return rec_get32(p) | (uint64_t)rec_get32(p + 4) << 32; }
static inline uint32_t rec_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* ---- chunk record ---- */
typedef enum { RC_FREE = 0, RC_OPEN = 1, RC_CLOSED = 2, RC_BAD = 3 } rec_cstate_t;
enum {
    RCF_EVENTS    = 1u << 0,            /* events fall inside it */
    RCF_GAPS      = 1u << 1,            /* a fragment follows a gap (spans need its fragment table) */
    RCF_RECOVERED = 1u << 2,            /* closed by the recovery after a power cut */
};

typedef struct {
    uint8_t  state;                     /* rec_cstate_t */
    uint8_t  cam;                       /* 1-based camera; 0 none */
    uint8_t  stream;                    /* 0 main, 1 sub */
    uint8_t  codec;                     /* vg_codec_t */
    uint32_t flags;                     /* RCF_* */
    uint32_t use;                       /* this use of the chunk: +1 at every reuse */
    uint32_t nfrag;
    uint64_t start_us, end_us;          /* UTC; start is a multiple of 100 us (9 ticks of 90 kHz) */
    uint32_t bytes;                     /* end of the last fragment (where vgix goes at close) */
    uint32_t init_len;                  /* ftyp+moov, at REC_VGCH_SIZE */
    uint16_t width, height;
    uint32_t last_off;                  /* the last fragment's moof (0: none) */
    uint32_t vgix_len;                  /* closed: vgix's size, at `bytes` */
    uint32_t no;                        /* the chunk's number (its file c<no>.mp4) */
} rec_chunk_t;

void rec_chunk_encode(const rec_chunk_t *c, uint64_t pool_id, uint8_t out[REC_CHUNK_REC]);
/* 0, or -1 when the CRC does not check. */
int  rec_chunk_decode(rec_chunk_t *c, uint64_t pool_id, const uint8_t in[REC_CHUNK_REC]);

/* The vgch box: REC_VGCH_SIZE bytes, a 'uuid' box with the pool id and the
 * record twice (A at open, B at close). */
void rec_vgch_build(uint8_t box[REC_VGCH_SIZE], uint64_t pool_id, const rec_chunk_t *a, const rec_chunk_t *b);
/* The pool id a vgch names (0 when it is not one). */
uint64_t rec_vgch_pool(const uint8_t box[REC_VGCH_SIZE]);
/* The chunk as its header says: copy B when it is valid and of the same
 * use as A, else A. 0, or -1 when neither copy checks. */
int rec_vgch_parse(const uint8_t box[REC_VGCH_SIZE], uint64_t pool_id, rec_chunk_t *out);

/* ---- journal ---- */
enum { RJ_OPEN = 1, RJ_FRAG = 2, RJ_CLOSE = 3, RJ_FREE = 4 };
typedef struct {
    uint8_t  type;
    uint8_t  a, b, c;                   /* OPEN: cam, stream, codec; FRAG: flags, frag index (16 bits);
                                           CLOSE: chunk flags (low 8 bits) */
    uint32_t chunk, use;
    /* OPEN: start_us, init_len, width, height, flags(16)
     * FRAG: t_ms, dur_ms, off, size_key (as rec_frag_t)
     * CLOSE: end_us, bytes, nfrag */
    uint64_t u64;
    uint32_t w0, w1, w2, w3;
} rec_jrec_t;

void rec_jrec_encode(const rec_jrec_t *j, uint64_t pool_id, uint8_t out[REC_JREC]);
int  rec_jrec_decode(rec_jrec_t *j, uint64_t pool_id, const uint8_t in[REC_JREC]);

/* ---- fragment entry ---- */
#define REC_FRAG_KEY 0x80000000u        /* starts with a keyframe */
#define REC_FRAG_GAP 0x40000000u        /* follows a gap (a reconnect, a drop, a pause) */
#define REC_FRAG_SIZE_MASK 0x3FFFFFFFu
typedef struct {
    uint32_t t_ms;                      /* from the chunk's start */
    uint32_t dur_ms;
    uint32_t off;                       /* the moof */
    uint32_t size;                      /* moof + mdat, with REC_FRAG_KEY / REC_FRAG_GAP */
} rec_frag_t;

void rec_frag_encode(const rec_frag_t *f, uint8_t out[REC_FRAG]);
void rec_frag_decode(rec_frag_t *f, const uint8_t in[REC_FRAG]);

/* ---- event record ---- */
typedef struct {
    uint64_t time_us;
    uint32_t dur_ms;
    uint8_t  cam;                       /* 1-based; 0 the recorder */
    uint8_t  type;                      /* REC_EV_* */
    uint16_t detail;                    /* zone, rule */
    uint32_t chunk, frag;               /* where it falls (REC_NONE: not recorded then) */
    uint32_t seq;                       /* its number in the log, from 0 */
} rec_event_t;

/* Types: the camera events (channel.h's CH_EV_* bit b is type b + 1, the
 * same names), then the recorder's own. */
enum {
    REC_EV_MOTION = 1, REC_EV_HUMAN, REC_EV_FACE, REC_EV_VEHICLE,
    REC_EV_LINE, REC_EV_INTRUSION, REC_EV_VIDEO_LOSS, REC_EV_TAMPER, REC_EV_ALARM_IN, REC_EV_OTHER, REC_EV_DISK,
    REC_EV_TYPES
};
/* "motion", ...; NULL for an unknown type. */
const char *rec_event_name(int type);
int rec_event_type(const char *name);   /* 0 when unknown */

void rec_event_encode(const rec_event_t *e, uint64_t pool_id, uint8_t out[REC_EVREC]);
int  rec_event_decode(rec_event_t *e, uint64_t pool_id, const uint8_t in[REC_EVREC]);

/* ---- file headers ---- */
typedef struct {
    uint64_t pool_id, generation;
    uint32_t chunk_bytes, n;
    uint32_t records_crc;               /* over the n records */
} rec_table_hdr_t;
void rec_table_hdr_encode(const rec_table_hdr_t *h, uint8_t out[REC_TABLE_HDR]);
int  rec_table_hdr_decode(rec_table_hdr_t *h, const uint8_t in[REC_TABLE_HDR]);

typedef struct { uint64_t pool_id, generation; } rec_journal_hdr_t;
void rec_journal_hdr_encode(const rec_journal_hdr_t *h, uint8_t out[REC_JOURNAL_HDR]);
int  rec_journal_hdr_decode(rec_journal_hdr_t *h, const uint8_t in[REC_JOURNAL_HDR]);

typedef struct {
    uint64_t pool_id, generation;
    uint32_t cap;                       /* records */
    uint32_t head;                      /* the next record's seq */
} rec_evlog_hdr_t;
void rec_evlog_hdr_encode(const rec_evlog_hdr_t *h, uint8_t out[64]);
int  rec_evlog_hdr_decode(rec_evlog_hdr_t *h, const uint8_t in[64]);

/* The vgix box's fixed part (after it: nfrag fragment entries, then nev
 * event records), and the whole box's size. */
#define REC_VGIX_HDR 56
size_t rec_vgix_size(uint32_t nfrag, uint32_t nev);
/* Build the box into `out` (rec_vgix_size bytes). */
void rec_vgix_build(uint8_t *out, uint64_t pool_id, uint32_t use, const rec_frag_t *f, uint32_t nfrag,
                    const rec_event_t *ev, uint32_t nev);
/* Check a box read from the disk (`len` bytes): 0 with *nfrag, *nev and
 * pointers to the entries inside `box`, -1 when it is not this use's. */
int rec_vgix_parse(const uint8_t *box, size_t len, uint64_t pool_id, uint32_t use, uint32_t *nfrag,
                   const uint8_t **frags, uint32_t *nev, const uint8_t **events);

#endif /* REC_FMT_H */
