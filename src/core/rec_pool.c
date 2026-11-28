/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rec_pool.c — a recording target's pool of chunks (rec_pool.h, docs/recording.md).
 *
 * Order of writes, which is what a power cut can interrupt:
 *
 *   open a chunk   journal "open" (synced) -> vgch copy A + init segment
 *   a fragment     its samples as they come -> at the next keyframe, the
 *                  moof and mdat header in the room left before them
 *   every flush_s  fdatasync the open chunks -> journal "fragment" records
 *                  (synced): the journal vouches only for synced data
 *   close a chunk  vgix + free box -> vgch copy B -> fdatasync -> journal
 *                  "close" -> the table saved soon after
 *   save           flush -> the whole table to its older copy (synced) ->
 *                  the journal's header with the new generation (synced)
 *
 * A fragment's samples are written before its moof, into room reserved
 * for it (a 'free' box pads what the moof did not need), so nothing of a
 * GOP is held in RAM: 16 cameras' GOPs would not fit in the board's.
 */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "mp4.h"
#include "rec_place.h"
#include "rec_pool.h"
#include "sps.h"

#define TICKS_PER_S   90000
#define FRAG_PAD_MIN  20                /* [size]['free']["vgfr"][use][flags] */
#define JOURNAL_SAVE  (1u << 20)        /* save (and so empty the journal) beyond this */
#define FRAG_MAX      65534
#define EV_PER_CHUNK  1024              /* events kept in a chunk's vgix */
#define READ_MOOF_MAX (64u << 10)

/* ---- small helpers ------------------------------------------------------- */

static ssize_t io_pwrite(rec_pool_t *p, int fd, const void *b, size_t n, off_t off)
{
    return p->io.pwrite ? p->io.pwrite(fd, b, n, off) : pwrite(fd, b, n, off);
}

static int write_all(rec_pool_t *p, int fd, const void *buf, size_t n, off_t off)
{
    const uint8_t *b = buf;
    while (n) {
        ssize_t w = io_pwrite(p, fd, b, n, off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        b += w;
        n -= (size_t)w;
        off += w;
    }
    return 0;
}

static int io_sync(rec_pool_t *p, int fd)
{
    return p->io.fdatasync ? p->io.fdatasync(fd) : fdatasync(fd);
}

static int read_all(int fd, void *buf, size_t n, off_t off)
{
    uint8_t *b = buf;
    while (n) {
        ssize_t r = pread(fd, b, n, off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        b += r;
        n -= (size_t)r;
        off += r;
    }
    return 0;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void path_of(const rec_pool_t *p, const char *rel, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", p->dir, rel);
}

void rec_pool_chunk_path(const rec_pool_t *p, uint32_t idx, char *out, size_t cap)
{
    snprintf(out, cap, "%s/pool/c%06u.mp4", p->dir, idx);
}

static int mkdirs(const char *dir)
{
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *s = tmp + 1; *s; ++s) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
        *s = '/';
    }
    return mkdir(tmp, 0755) < 0 && errno != EEXIST ? -1 : 0;
}

/* The journal's CRCs depend on the generation too: records left behind
 * by an earlier one (the journal is rewritten from its start, never
 * truncated) never check. */
static uint64_t jseed(const rec_pool_t *p) { return p->pool_id ^ (p->generation * 0x9E3779B97F4A7C15ull); }

/* ---- vigil-rec.id ---------------------------------------------------------- */

typedef struct {
    uint64_t pool_id;
    unsigned chunk_mb;
    uint32_t want;
    char     owner[64], host[64];
} id_t_;

static int id_read(const char *dir, id_t_ *id)
{
    char path[300], line[160];
    snprintf(path, sizeof path, "%s/vigil-rec.id", dir);
    memset(id, 0, sizeof *id);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char *v = strchr(line, ' ');
        if (!v) continue;
        *v++ = 0;
        v[strcspn(v, "\r\n")] = 0;
        if (!strcmp(line, "pool")) id->pool_id = strtoull(v, NULL, 16);
        else if (!strcmp(line, "chunk_mb")) id->chunk_mb = (unsigned)strtoul(v, NULL, 10);
        else if (!strcmp(line, "chunks")) id->want = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(line, "owner")) snprintf(id->owner, sizeof id->owner, "%s", v);
        else if (!strcmp(line, "host")) snprintf(id->host, sizeof id->host, "%s", v);
    }
    fclose(f);
    return id->pool_id && id->chunk_mb ? 0 : -1;
}

static int id_write(const char *dir, const id_t_ *id)
{
    char path[300], tmp[310];
    snprintf(path, sizeof path, "%s/vigil-rec.id", dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "# vigil recording pool: the recorder that writes here (docs/recording.md)\n"
               "pool %016llx\nowner %s\nhost %s\nchunk_mb %u\nchunks %u\n",
            (unsigned long long)id->pool_id, id->owner, id->host, id->chunk_mb, id->want);
    int bad = fflush(f) != 0 || fsync(fileno(f)) != 0;
    bad |= fclose(f) != 0;
    if (bad || rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }
    int d = open(dir, O_RDONLY);
    if (d >= 0) {
        fsync(d);
        close(d);
    }
    return 0;
}

static uint64_t random_id(void)
{
    uint64_t v = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, &v, sizeof v) != (ssize_t)sizeof v) v = 0;
        close(fd);
    }
    if (!v) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        v = (uint64_t)ts.tv_sec * 1000000007ull ^ (uint64_t)ts.tv_nsec << 20 ^ (uint64_t)getpid();
    }
    return v ? v : 1;
}

/* ---- the table ------------------------------------------------------------- */

static int tab_reserve(rec_pool_t *p, uint32_t n)
{
    if (n <= p->cap) return 0;
    uint32_t cap = p->cap ? p->cap : 64;
    while (cap < n) cap *= 2;
    rec_chunk_t *t = realloc(p->tab, (size_t)cap * sizeof *t);
    if (!t) return -1;
    p->tab = t;
    uint8_t *k = realloc(p->known, cap);
    if (!k) return -1;
    p->known = k;
    memset(p->tab + p->cap, 0, (size_t)(cap - p->cap) * sizeof *t);
    memset(p->known + p->cap, 0, cap - p->cap);
    p->cap = cap;
    return 0;
}

/* A table copy: 0 with the header and the records, or -1. */
static int table_read(rec_pool_t *p, const char *name, rec_table_hdr_t *h, rec_chunk_t **out)
{
    char path[300];
    uint8_t hb[REC_TABLE_HDR];
    path_of(p, name, path, sizeof path);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int rc = -1;
    uint8_t *buf = NULL;
    if (read_all(fd, hb, sizeof hb, 0) < 0 || rec_table_hdr_decode(h, hb) < 0 || h->pool_id != p->pool_id ||
        h->chunk_bytes != p->chunk_bytes || h->n > (1u << 22))
        goto out;
    size_t len = (size_t)h->n * REC_CHUNK_REC;
    buf = malloc(len ? len : 1);
    if (!buf || (len && read_all(fd, buf, len, REC_TABLE_HDR) < 0)) goto out;
    if (rec_crc32c(0, buf, len) != h->records_crc) goto out;
    rec_chunk_t *t = calloc(h->n ? h->n : 1, sizeof *t);
    if (!t) goto out;
    for (uint32_t i = 0; i < h->n; ++i) {
        if (rec_chunk_decode(&t[i], p->pool_id, buf + (size_t)i * REC_CHUNK_REC) < 0) {
            free(t);
            goto out;
        }
    }
    *out = t;
    rc = 0;
out:
    free(buf);
    close(fd);
    return rc;
}

static int table_write(rec_pool_t *p)
{
    pthread_mutex_lock(&p->mu);
    uint32_t n = p->n;
    size_t len = REC_TABLE_HDR + (size_t)n * REC_CHUNK_REC;
    uint8_t *buf = malloc(len);
    if (!buf) {
        pthread_mutex_unlock(&p->mu);
        return -1;
    }
    for (uint32_t i = 0; i < n; ++i)
        rec_chunk_encode(&p->tab[i], p->pool_id, buf + REC_TABLE_HDR + (size_t)i * REC_CHUNK_REC);
    pthread_mutex_unlock(&p->mu);
    rec_table_hdr_t h = { .pool_id = p->pool_id, .generation = p->generation + 1, .chunk_bytes = p->chunk_bytes,
                          .n = n, .records_crc = rec_crc32c(0, buf + REC_TABLE_HDR, len - REC_TABLE_HDR) };
    rec_table_hdr_encode(&h, buf);
    char path[300];
    path_of(p, h.generation & 1 ? "index/chunks.a" : "index/chunks.b", path, sizeof path);
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    int rc = fd < 0 || write_all(p, fd, buf, len, 0) < 0 || io_sync(p, fd) < 0 ? -1 : 0;
    if (fd >= 0) {
        /* A smaller table than the file held: the rest is past the
         * header's count and never read. */
        close(fd);
    }
    free(buf);
    if (rc == 0) p->generation = h.generation;
    return rc;
}

/* ---- the journal ---------------------------------------------------------- */

static void jappend(rec_pool_t *p, const rec_jrec_t *j)
{
    if (p->jlen + REC_JREC > p->jcap) {
        uint32_t cap = p->jcap ? p->jcap * 2 : 4096;
        uint8_t *b = realloc(p->jbuf, cap);
        if (!b) return;                 /* the next save covers it */
        p->jbuf = b;
        p->jcap = cap;
    }
    rec_jrec_encode(j, jseed(p), p->jbuf + p->jlen);
    p->jlen += REC_JREC;
}

static int journal_reset(rec_pool_t *p)
{
    uint8_t h[REC_JOURNAL_HDR];
    rec_journal_hdr_t jh = { .pool_id = p->pool_id, .generation = p->generation };
    rec_journal_hdr_encode(&jh, h);
    if (write_all(p, p->journal_fd, h, sizeof h, 0) < 0 || io_sync(p, p->journal_fd) < 0) return -1;
    p->journal_len = REC_JOURNAL_HDR;
    return 0;
}

static void j_apply(rec_pool_t *p, const rec_jrec_t *j)
{
    if (j->chunk >= p->n) return;
    rec_chunk_t *c = &p->tab[j->chunk];
    switch (j->type) {
    case RJ_OPEN:
        memset(c, 0, sizeof *c);
        c->state = RC_OPEN;
        c->cam = j->a;
        c->stream = j->b;
        c->codec = j->c;
        c->use = j->use;
        c->start_us = c->end_us = j->u64;
        c->init_len = j->w0;
        c->width = (uint16_t)j->w1;
        c->height = (uint16_t)(j->w1 >> 16);
        c->bytes = REC_VGCH_SIZE + c->init_len;
        c->no = j->chunk;
        break;
    case RJ_FRAG:
        if (c->state != RC_OPEN || c->use != j->use || (c->nfrag & 0xFFFF) != (uint32_t)(j->b | j->c << 8)) break;
        c->nfrag++;
        c->bytes = j->w2 + (j->w3 & REC_FRAG_SIZE_MASK);
        c->last_off = j->w2;
        c->end_us = c->start_us + ((uint64_t)j->w0 + j->w1) * 1000;
        if (j->w3 & REC_FRAG_GAP) c->flags |= RCF_GAPS;
        break;
    case RJ_CLOSE:
        if (c->use != j->use) break;
        c->state = RC_CLOSED;
        c->end_us = j->u64;
        c->bytes = j->w0;
        c->nfrag = j->w1 & 0xFFFF;
        c->vgix_len = (uint32_t)rec_vgix_size(c->nfrag, j->w1 >> 16);
        c->flags = j->a;
        break;
    case RJ_FREE:
        if (c->use != j->use) break;
        c->state = RC_FREE;
        break;
    }
}

static void journal_replay(rec_pool_t *p)
{
    char path[300];
    path_of(p, "index/journal", path, sizeof path);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    uint8_t h[REC_JOURNAL_HDR], r[REC_JREC];
    rec_journal_hdr_t jh;
    int n = 0;
    if (read_all(fd, h, sizeof h, 0) == 0 && rec_journal_hdr_decode(&jh, h) == 0 && jh.pool_id == p->pool_id &&
        jh.generation == p->generation) {
        for (off_t off = REC_JOURNAL_HDR; read_all(fd, r, sizeof r, off) == 0; off += REC_JREC) {
            rec_jrec_t j;
            if (rec_jrec_decode(&j, jseed(p), r) < 0) break;      /* torn, or an earlier generation's */
            j_apply(p, &j);
            ++n;
        }
    }
    close(fd);
    if (n) vg_log("rec: %d journal records replayed", n);
}

/* ---- the event log --------------------------------------------------------- */

static off_t ev_off(const rec_pool_t *p, uint32_t seq) { return REC_EVLOG_HDR + (off_t)(seq % p->ev_cap) * REC_EVREC; }

static int ev_read(const rec_pool_t *p, int fd, uint32_t seq, rec_event_t *e)
{
    uint8_t b[REC_EVREC];
    if (read_all(fd, b, sizeof b, ev_off(p, seq)) < 0 || rec_event_decode(e, p->pool_id, b) < 0) return -1;
    return e->seq == seq ? 0 : -1;
}

static int evlog_open(rec_pool_t *p, int fresh_pool)
{
    char path[300];
    path_of(p, "index/events.log", path, sizeof path);
    p->events_fd = open(path, O_RDWR | O_CREAT, 0644);
    if (p->events_fd < 0) return -1;
    uint8_t b[64];
    rec_evlog_hdr_t best = { 0 }, h;
    int have = 0;
    for (int k = 0; k < 2; ++k)
        if (read_all(p->events_fd, b, sizeof b, k * 512) == 0 && rec_evlog_hdr_decode(&h, b) == 0 &&
            h.pool_id == p->pool_id && h.cap && (!have || h.generation > best.generation)) {
            best = h;
            have = 1;
        }
    if (have) {
        p->ev_cap = best.cap;
        p->ev_head = best.head;
        p->ev_gen = best.generation;
        /* Records appended after the header was last written. */
        rec_event_t e;
        for (uint32_t k = 0; k < p->ev_cap && ev_read(p, p->events_fd, p->ev_head, &e) == 0; ++k) ++p->ev_head;
        return 0;
    }
    /* A year of a busy recorder on a large disk, less on small media. */
    uint64_t cap = (uint64_t)(p->want > p->n ? p->want : p->n) * 256;
    p->ev_cap = (uint32_t)(cap < 4096 ? 4096 : cap > (4u << 20) ? (4u << 20) : cap);
    p->ev_head = 0;
    p->ev_gen = 0;
    if (ftruncate(p->events_fd, REC_EVLOG_HDR + (off_t)p->ev_cap * REC_EVREC) < 0) return -1;
    rec_evlog_hdr_t nh = { .pool_id = p->pool_id, .generation = 1, .cap = p->ev_cap, .head = 0 };
    rec_evlog_hdr_encode(&nh, b);
    if (write_all(p, p->events_fd, b, sizeof b, 0) < 0) return -1;
    p->ev_gen = 1;
    p->events_rebuild = !fresh_pool;
    return 0;
}

static int evlog_hdr_save(rec_pool_t *p)
{
    uint8_t b[64];
    pthread_mutex_lock(&p->mu);
    rec_evlog_hdr_t h = { .pool_id = p->pool_id, .generation = p->ev_gen + 1, .cap = p->ev_cap, .head = p->ev_head };
    pthread_mutex_unlock(&p->mu);
    rec_evlog_hdr_encode(&h, b);
    if (write_all(p, p->events_fd, b, sizeof b, (off_t)(h.generation & 1) * 512) < 0 || io_sync(p, p->events_fd) < 0)
        return -1;
    p->ev_gen = h.generation;
    return 0;
}

/* Under mu. */
static void ev_pend(rec_pool_t *p, const rec_event_t *e)
{
    if (p->n_ev_pend == p->cap_ev_pend) {
        uint32_t cap = p->cap_ev_pend ? p->cap_ev_pend * 2 : 32;
        rec_event_t *n = realloc(p->ev_pend, cap * sizeof *n);
        if (!n) return;
        p->ev_pend = n;
        p->cap_ev_pend = cap;
    }
    p->ev_pend[p->n_ev_pend++] = *e;
}

/* ---- chunk headers --------------------------------------------------------- */

static int vgch_write(rec_pool_t *p, int fd, const rec_chunk_t *a)
{
    uint8_t box[REC_VGCH_SIZE];
    rec_vgch_build(box, p->pool_id, a, NULL);
    return write_all(p, fd, box, sizeof box, 0);
}

static int vgch_write_b(rec_pool_t *p, int fd, const rec_chunk_t *b)
{
    uint8_t r[REC_CHUNK_REC];
    rec_chunk_encode(b, p->pool_id, r);
    return write_all(p, fd, r, sizeof r, REC_VGCH_B);
}

/* What a chunk file's header says (FREE when it says nothing of this pool). */
static void vgch_read(rec_pool_t *p, uint32_t idx, rec_chunk_t *out)
{
    char path[300];
    uint8_t box[REC_VGCH_SIZE];
    rec_pool_chunk_path(p, idx, path, sizeof path);
    memset(out, 0, sizeof *out);
    out->no = idx;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        out->state = RC_BAD;
        return;
    }
    if (read_all(fd, box, sizeof box, 0) < 0 || rec_vgch_parse(box, p->pool_id, out) < 0) memset(out, 0, sizeof *out);
    out->no = idx;
    close(fd);
}

/* ---- recovery of an open chunk -------------------------------------------- */

/* Does every sample of a fragment parse as length-prefixed NAL units? The
 * data behind a moof that was written, when its own pages were not yet. */
static int samples_ok(int fd, const uint8_t *moof, const mp4_moof_t *m, off_t data, vg_codec_t codec)
{
    if (m->data_size > (64u << 20)) return 0;
    uint8_t *buf = malloc(m->data_size ? m->data_size : 1);
    if (!buf) return 0;
    int ok = read_all(fd, buf, m->data_size, data) == 0;
    size_t at = 0;
    for (uint32_t i = 0; ok && i < m->n; ++i) {
        mp4_sample_t s;
        mp4_moof_sample(moof, m, i, &s);
        size_t end = at + s.size;
        if (!s.size) ok = 0;
        while (ok && at < end) {
            if (end - at < 5) { ok = 0; break; }
            uint32_t len = rec_be32(buf + at);
            if (!len || len > end - at - 4) { ok = 0; break; }
            uint8_t h = buf[at + 4];
            if (h & 0x80) ok = 0;                                   /* forbidden_zero_bit */
            else if (codec == VG_CODEC_H265) ok = len >= 2 && (buf[at + 5] & 7) != 0;    /* TemporalId + 1 */
            else ok = (h & 0x1F) != 0;
            at += 4 + len;
        }
    }
    free(buf);
    return ok;
}

/* Walk the fragments of open chunk `c` (its table record), keep those
 * that parse and follow on, and close it there. `vouched`: fragments the
 * journal had (checked less: their data was synced first). Returns the
 * fragments kept; the chunk is FREE when there were none. Takes mu only
 * to write the record. */
static int recover_chunk(rec_pool_t *p, rec_chunk_t *c, uint32_t vouched)
{
    char path[300];
    rec_pool_chunk_path(p, c->no, path, sizeof path);
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        c->state = RC_BAD;
        return 0;
    }
    uint8_t box[REC_VGCH_SIZE];
    rec_chunk_t v;
    int got = read_all(fd, box, sizeof box, 0) == 0 && rec_vgch_parse(box, p->pool_id, &v) == 0 && v.use == c->use;
    if (got && v.state == RC_CLOSED) {
        /* Closed, its close not yet in the journal or the table. */
        close(fd);
        v.no = c->no;
        *c = v;
        return (int)v.nfrag;
    }
    if (!got || v.state != RC_OPEN) {
        /* Its header for this use never reached the disk: nothing of it did. */
        rec_chunk_t f = { .state = RC_FREE, .use = c->use, .no = c->no };
        vgch_write(p, fd, &f);
        io_sync(p, fd);
        close(fd);
        *c = f;
        return 0;
    }
    rec_frag_t *fr = NULL;
    uint32_t nf = 0, cap = 0, pad_flags = 0;
    uint64_t prev_end = 0;
    off_t pos = REC_VGCH_SIZE + v.init_len;
    uint8_t *moof = malloc(READ_MOOF_MAX);
    while (moof && pos + 16 <= (off_t)p->chunk_bytes) {
        uint8_t h[FRAG_PAD_MIN];
        if (read_all(fd, h, sizeof h, pos) < 0) break;
        uint32_t size = rec_be32(h);
        if (!memcmp(h + 4, "free", 4) && size >= FRAG_PAD_MIN && !memcmp(h + 8, "vgfr", 4) &&
            rec_get32(h + 12) == c->use && pos + size < (off_t)p->chunk_bytes) {
            pad_flags = rec_get32(h + 16);
            pos += size;
            continue;
        }
        mp4_moof_t m;
        uint8_t mh[8];
        if (memcmp(h + 4, "moof", 4) || size < 8 || size > READ_MOOF_MAX || read_all(fd, moof, size, pos) < 0 ||
            mp4_moof_parse(moof, size, &m) < 0 || m.seq != nf + 1 || m.tfdt < prev_end ||
            read_all(fd, mh, 8, pos + size) < 0 || memcmp(mh + 4, "mdat", 4) || rec_be32(mh) != 8 + m.data_size ||
            m.data_offset != size + 8)
            break;
        uint64_t end = (uint64_t)pos + size + 8 + m.data_size;
        if (end + 64 > p->chunk_bytes) break;
        if (nf >= vouched && !samples_ok(fd, moof, &m, pos + size + 8, (vg_codec_t)c->codec)) break;
        if (nf == cap) {
            cap = cap ? cap * 2 : 256;
            rec_frag_t *n = realloc(fr, cap * sizeof *n);
            if (!n) break;
            fr = n;
        }
        fr[nf].t_ms = (uint32_t)(m.tfdt / 90);
        fr[nf].dur_ms = (uint32_t)(m.dur / 90);
        fr[nf].off = (uint32_t)pos;
        fr[nf].size = (uint32_t)(end - (uint64_t)pos) | (pad_flags & (REC_FRAG_KEY | REC_FRAG_GAP));
        if (pad_flags & REC_FRAG_GAP) v.flags |= RCF_GAPS;
        ++nf;
        prev_end = m.tfdt + m.dur;
        v.bytes = (uint32_t)end;
        v.last_off = (uint32_t)pos;
        v.end_us = v.start_us + prev_end * 100 / 9;
        pad_flags = 0;
        pos = (off_t)end;
    }
    free(moof);
    if (!nf) {
        rec_chunk_t f = { .state = RC_FREE, .use = c->use, .no = c->no };
        vgch_write(p, fd, &f);
        io_sync(p, fd);
        close(fd);
        free(fr);
        *c = f;
        return 0;
    }
    /* The events the log has for it. */
    rec_event_t *ev = NULL;
    uint32_t nev = 0;
    if (p->events_fd >= 0 && p->ev_cap) {
        uint32_t lo = p->ev_head > p->ev_cap ? p->ev_head - p->ev_cap : 0;
        for (uint32_t s = p->ev_head; s-- > lo && nev < EV_PER_CHUNK;) {
            rec_event_t e;
            if (ev_read(p, p->events_fd, s, &e) < 0) break;
            if (e.time_us < v.start_us) break;
            if (e.chunk != c->no || e.time_us > v.end_us) continue;
            rec_event_t *n = realloc(ev, (nev + 1) * sizeof *n);
            if (!n) break;
            ev = n;
            ev[nev++] = e;
        }
        for (uint32_t i = 0; i < nev / 2; ++i) {   /* oldest first */
            rec_event_t t = ev[i];
            ev[i] = ev[nev - 1 - i];
            ev[nev - 1 - i] = t;
        }
    }
    size_t z = rec_vgix_size(nf, nev);
    while (nev && v.bytes + z + 8 > p->chunk_bytes) z = rec_vgix_size(nf, --nev);
    uint8_t *ix = malloc(z + 8);
    if (ix) {
        rec_vgix_build(ix, p->pool_id, c->use, fr, nf, ev, nev);
        put_be32(ix + z, p->chunk_bytes - v.bytes - (uint32_t)z);
        memcpy(ix + z + 4, "free", 4);
        write_all(p, fd, ix, z + 8, v.bytes);
        free(ix);
    }
    v.state = RC_CLOSED;
    v.nfrag = nf;
    v.vgix_len = (uint32_t)z;
    v.flags |= RCF_RECOVERED | (nev ? RCF_EVENTS : 0);
    v.no = c->no;
    vgch_write_b(p, fd, &v);
    io_sync(p, fd);
    close(fd);
    free(fr);
    free(ev);
    vg_log("rec: chunk %u recovered: %u fragments (%u in the journal), %u bytes", c->no, nf, vouched, v.bytes);
    *c = v;
    return (int)nf;
}

/* ---- space ------------------------------------------------------------------ */

static uint64_t reserve_of(uint64_t total)
{
    uint64_t r = total / 50;
    if (r < (1ull << 30)) r = total / 10 < (1ull << 30) ? total / 10 : (1ull << 30);
    return r;
}

/* How many chunks the pool should have: what fits beside the reserve,
 * no more than pool_mb. */
static uint32_t want_of(const rec_pool_t *p, uint32_t have)
{
    struct statvfs fs;
    if (statvfs(p->dir, &fs) < 0) return have;
    uint64_t total = (uint64_t)fs.f_blocks * fs.f_frsize, avail = (uint64_t)fs.f_bavail * fs.f_frsize;
    uint64_t res = reserve_of(total);
    uint64_t more = avail > res ? (avail - res) / p->chunk_bytes : 0;
    uint64_t want = have + more;
    if (p->cfg.pool_mb) {
        uint64_t lim = ((uint64_t)p->cfg.pool_mb << 20) / p->chunk_bytes;
        if (want > lim) want = lim > have ? lim : have;
    }
    /* Test runs and development hosts: never the whole disk, whatever a
     * test's configuration says (tests/CMakeLists.txt sets it). */
    const char *cap = getenv("VIGIL_REC_POOL_MB_MAX");
    if (cap && atoi(cap) > 0) {
        uint64_t lim = ((uint64_t)atoi(cap) << 20) / p->chunk_bytes;
        if (want > lim) want = lim > have ? lim : have;
    }
    return want > 1000000 ? 1000000 : (uint32_t)want;
}

/* ---- mount ------------------------------------------------------------------- */

void rec_pool_init(rec_pool_t *p)
{
    memset(p, 0, sizeof *p);
    pthread_mutex_init(&p->mu, NULL);
    p->table_fd = p->journal_fd = p->events_fd = -1;
}

static void ws_reset(rec_wstream_t *w)
{
    free(w->smp);
    free(w->frags);
    free(w->ev);
    memset(w, 0, sizeof *w);
    w->fd = -1;
}

static void release(rec_pool_t *p)
{
    for (int c = 0; c < REC_CAMS; ++c)
        for (int s = 0; s < REC_STREAMS; ++s) {
            if (p->ws[c][s].fd >= 0 && p->ws[c][s].open) close(p->ws[c][s].fd);
            ws_reset(&p->ws[c][s]);
        }
    if (p->journal_fd >= 0) close(p->journal_fd);
    if (p->events_fd >= 0) close(p->events_fd);
    p->journal_fd = p->events_fd = -1;
    pthread_mutex_lock(&p->mu);
    free(p->tab);
    free(p->known);
    free(p->ev_pend);
    p->tab = NULL;
    p->known = NULL;
    p->ev_pend = NULL;
    p->n = p->cap = 0;
    p->n_ev_pend = p->cap_ev_pend = 0;
    p->mounted = 0;
    p->rebuilding = 0;
    memset(p->active, 0, sizeof p->active);
    pthread_mutex_unlock(&p->mu);
    free(p->jbuf);
    p->jbuf = NULL;
    p->jlen = p->jcap = 0;
}

void rec_pool_free(rec_pool_t *p)
{
    release(p);
    pthread_mutex_destroy(&p->mu);
}

static int fail(rec_pool_t *p, rec_pstate_t st, const char *fmt, const char *arg)
{
    pthread_mutex_lock(&p->mu);
    p->state = st;
    snprintf(p->why, sizeof p->why, fmt, arg, strerror(errno));
    pthread_mutex_unlock(&p->mu);
    return -1;
}

/* Chunk files present, in a row from c000000, of the pool's size. */
static uint32_t count_files(const rec_pool_t *p)
{
    char path[300];
    struct stat st;
    uint32_t n = 0;
    for (;; ++n) {
        rec_pool_chunk_path(p, n, path, sizeof path);
        if (stat(path, &st) < 0 || (uint64_t)st.st_size < p->chunk_bytes) break;
    }
    return n;
}

int rec_pool_mount(rec_pool_t *p, const rec_pool_cfg_t *cfg, time_t now)
{
    release(p);
    pthread_mutex_lock(&p->mu);
    p->cfg = *cfg;
    snprintf(p->dir, sizeof p->dir, "%s", cfg->path);
    pthread_mutex_unlock(&p->mu);
    char sub[300];
    if (mkdirs(p->dir) < 0) return fail(p, RP_ERROR, "cannot create %.100s: %s", p->dir);
    path_of(p, "pool", sub, sizeof sub);
    if (mkdir(sub, 0755) < 0 && errno != EEXIST) return fail(p, RP_ERROR, "cannot create %.100s: %s", sub);
    path_of(p, "index", sub, sizeof sub);
    if (mkdir(sub, 0755) < 0 && errno != EEXIST) return fail(p, RP_ERROR, "cannot create %.100s: %s", sub);

    id_t_ id;
    int fresh = 0, readonly = 0;
    if (id_read(p->dir, &id) == 0) {
        p->pool_id = id.pool_id;
        p->chunk_bytes = id.chunk_mb << 20;
        readonly = cfg->owner[0] && id.owner[0] && strcmp(cfg->owner, id.owner);
    } else {
        /* No id: chunks of a pool whose id file is gone take theirs. */
        char c0[300];
        uint8_t box[REC_VGCH_SIZE];
        struct stat st;
        memset(&id, 0, sizeof id);
        rec_pool_chunk_path(p, 0, c0, sizeof c0);
        int fd = open(c0, O_RDONLY);
        if (fd >= 0 && fstat(fd, &st) == 0 && read_all(fd, box, sizeof box, 0) == 0 && rec_vgch_pool(box) &&
            st.st_size >= (1 << 20)) {
            id.pool_id = rec_vgch_pool(box);
            id.chunk_mb = (unsigned)(st.st_size >> 20);
            vg_log("rec: %s has chunks but no vigil-rec.id: taking pool %016llx", p->dir,
                   (unsigned long long)id.pool_id);
        } else {
            id.pool_id = random_id();
            id.chunk_mb = cfg->chunk_mb;
            fresh = 1;
        }
        if (fd >= 0) close(fd);
        p->pool_id = id.pool_id;
        p->chunk_bytes = id.chunk_mb << 20;
    }
    if (cfg->chunk_mb != id.chunk_mb && !fresh)
        vg_log("rec: the pool in %s has %u MiB chunks (record.chunk_mb %u applies to a new pool)", p->dir,
               id.chunk_mb, cfg->chunk_mb);
    uint32_t files = count_files(p);
    if (!readonly) {
        uint32_t want = want_of(p, files);
        if (want < id.want) want = id.want;         /* never smaller than it was set up */
        if (cfg->pool_mb && want > files) {
            uint64_t lim = ((uint64_t)cfg->pool_mb << 20) / p->chunk_bytes;
            if (want > lim) want = (uint32_t)(lim > files ? lim : files);
        }
        if (want < 2 && files < 2) {
            errno = ENOSPC;
            return fail(p, RP_ERROR, "%.100s: no room for a pool of two chunks: %s", p->dir);
        }
        if (fresh || want != id.want || strcmp(id.owner, cfg->owner) || strcmp(id.host, cfg->host)) {
            id.want = want;
            snprintf(id.owner, sizeof id.owner, "%s", cfg->owner);
            snprintf(id.host, sizeof id.host, "%s", cfg->host);
            if (id_write(p->dir, &id) < 0) return fail(p, RP_ERROR, "cannot write %.100s/vigil-rec.id: %s", p->dir);
        }
    }
    p->want = id.want;

    /* The table: the newest copy that checks, the journal on top. */
    rec_table_hdr_t ha, hb;
    rec_chunk_t *ta = NULL, *tb = NULL;
    int va = table_read(p, "index/chunks.a", &ha, &ta) == 0, vb = table_read(p, "index/chunks.b", &hb, &tb) == 0;
    pthread_mutex_lock(&p->mu);
    uint32_t n = files;
    int have_table = va || vb;
    rec_table_hdr_t *h = va && (!vb || ha.generation > hb.generation) ? &ha : &hb;
    rec_chunk_t *t = h == &ha ? ta : tb;
    if (have_table && h->n > n) n = h->n;
    if (tab_reserve(p, n ? n : 1) < 0) {
        pthread_mutex_unlock(&p->mu);
        free(ta);
        free(tb);
        errno = ENOMEM;
        return fail(p, RP_ERROR, "%.100s: %s", p->dir);
    }
    p->n = n;
    if (have_table) {
        p->generation = h->generation;
        memcpy(p->tab, t, (size_t)h->n * sizeof *t);
        for (uint32_t i = 0; i < h->n; ++i) p->known[i] = 1;
        for (uint32_t i = h->n; i < n; ++i) p->tab[i] = (rec_chunk_t){ .no = i };
        /* Files gone since the save: their chunks cannot be read. */
        for (uint32_t i = files; i < h->n; ++i) p->tab[i].state = RC_BAD;
    } else {
        p->generation = 0;
        for (uint32_t i = 0; i < n; ++i) p->tab[i] = (rec_chunk_t){ .no = i };
        p->rebuilding = n > 0 && !fresh;
        if (!p->rebuilding) memset(p->known, 1, n);
        p->rebuild_done = 0;
    }
    for (uint32_t i = 0; i < n; ++i) p->tab[i].no = i;
    pthread_mutex_unlock(&p->mu);
    uint32_t tn = have_table ? h->n : 0;
    free(ta);
    free(tb);
    if (have_table) {
        /* Chunks made after the last save. */
        for (uint32_t i = tn; i < n && i < files; ++i) {
            vgch_read(p, i, &p->tab[i]);
            p->known[i] = 1;
        }
        journal_replay(p);
    }
    if (p->rebuilding) vg_log("rec: %s: no valid chunk table; rebuilding it from the %u chunks", p->dir, n);

    char path[300];
    path_of(p, "index/journal", path, sizeof path);
    p->journal_fd = open(path, readonly ? O_RDONLY : O_RDWR | O_CREAT, 0644);
    if (p->journal_fd < 0 && !readonly) return fail(p, RP_ERROR, "cannot open %.100s: %s", path);
    if (readonly) {
        path_of(p, "index/events.log", path, sizeof path);
        p->events_fd = open(path, O_RDONLY);
        uint8_t b[64];
        rec_evlog_hdr_t eh;
        if (p->events_fd >= 0 && read_all(p->events_fd, b, sizeof b, 0) == 0 && rec_evlog_hdr_decode(&eh, b) == 0) {
            p->ev_cap = eh.cap;
            p->ev_head = eh.head;
        }
    } else if (evlog_open(p, fresh || !n) < 0) {
        return fail(p, RP_ERROR, "cannot open %.100s/index/events.log: %s", p->dir);
    }

    if (!readonly) {
        /* Open chunks were being written when the recorder stopped. */
        for (uint32_t i = 0; i < p->n; ++i) {
            if (!p->known[i] || p->tab[i].state != RC_OPEN) continue;
            rec_chunk_t c = p->tab[i];
            recover_chunk(p, &c, c.nfrag);
            pthread_mutex_lock(&p->mu);
            p->tab[i] = c;
            pthread_mutex_unlock(&p->mu);
        }
        /* While a rebuild runs the chunks' headers are the index: a
         * table written now would hide those not read yet. */
        if ((!p->rebuilding && table_write(p) < 0) || journal_reset(p) < 0)
            return fail(p, RP_ERROR, "cannot write the index in %.100s: %s", p->dir);
    }
    char kind[8];
    unsigned fl = rec_media_flush(p->dir, kind, sizeof kind);
    pthread_mutex_lock(&p->mu);
    snprintf(p->media, sizeof p->media, "%s", kind);
    p->flush_s = cfg->flush_s ? cfg->flush_s : fl;
    p->state = readonly ? RP_READONLY : RP_READY;
    if (readonly)
        snprintf(p->why, sizeof p->why, "owned by %s (%s)", id.host[0] ? id.host : "another recorder", id.owner);
    else p->why[0] = 0;
    p->mounted = 1;
    p->next_flush = now + p->flush_s;
    p->next_save = now + (cfg->index_save_s ? cfg->index_save_s : 300);
    p->next_quota = now;
    pthread_mutex_unlock(&p->mu);
    for (int c = 0; c < REC_CAMS; ++c)
        for (int s = 0; s < REC_STREAMS; ++s) p->ws[c][s].fd = -1;
    vg_log("rec: pool %016llx in %s: %u of %u chunks of %u MiB, on %s (flush every %u s)%s",
           (unsigned long long)p->pool_id, p->dir, p->n, p->want, p->chunk_bytes >> 20, p->media, p->flush_s,
           readonly ? ", read-only: another recorder's" : "");
    return 0;
}

void rec_pool_update(rec_pool_t *p, const rec_pool_cfg_t *cfg)
{
    pthread_mutex_lock(&p->mu);
    memcpy(p->cfg.quota_bytes, cfg->quota_bytes, sizeof p->cfg.quota_bytes);
    memcpy(p->cfg.quota_age, cfg->quota_age, sizeof p->cfg.quota_age);
    p->cfg.stop_when_full = cfg->stop_when_full;
    p->cfg.fragment_s = cfg->fragment_s;
    p->cfg.index_save_s = cfg->index_save_s;
    if (cfg->flush_s) p->flush_s = cfg->flush_s;
    else if (p->cfg.flush_s) p->flush_s = rec_media_flush(p->dir, p->media, sizeof p->media);
    p->cfg.flush_s = cfg->flush_s;
    pthread_mutex_unlock(&p->mu);
}

/* ---- flush and save ----------------------------------------------------------- */

int rec_pool_flush(rec_pool_t *p)
{
    if (!p->mounted || p->state != RP_READY) return 0;
    int rc = 0;
    for (int c = 0; c < REC_CAMS; ++c)
        for (int s = 0; s < REC_STREAMS; ++s) {
            rec_wstream_t *w = &p->ws[c][s];
            if (!w->open) continue;
            if (w->dirty && io_sync(p, w->fd) < 0) {
                rc = -1;
                continue;               /* not vouched for: nothing journaled */
            }
            w->dirty = 0;
            for (uint32_t k = w->journaled; k < w->nfrag; ++k) {
                const rec_frag_t *f = &w->frags[k];
                rec_jrec_t j = { .type = RJ_FRAG, .a = (uint8_t)(f->size >> 24 & 0xC0), .b = (uint8_t)k,
                                 .c = (uint8_t)(k >> 8), .chunk = w->idx, .use = w->rec.use,
                                 .w0 = f->t_ms, .w1 = f->dur_ms, .w2 = f->off, .w3 = f->size };
                jappend(p, &j);
            }
            w->journaled = w->nfrag;
        }
    if (p->jlen) {
        if (write_all(p, p->journal_fd, p->jbuf, p->jlen, p->journal_len) < 0 || io_sync(p, p->journal_fd) < 0) {
            rc = -1;
        } else {
            p->journal_len += p->jlen;
        }
        p->jlen = 0;
    }
    pthread_mutex_lock(&p->mu);
    uint32_t n = p->n_ev_pend;
    rec_event_t *ev = n ? malloc(n * sizeof *ev) : NULL;
    if (ev) memcpy(ev, p->ev_pend, n * sizeof *ev);
    p->n_ev_pend = 0;
    pthread_mutex_unlock(&p->mu);
    for (uint32_t i = 0; ev && i < n; ++i) {
        uint8_t b[REC_EVREC];
        rec_event_encode(&ev[i], p->pool_id, b);
        if (write_all(p, p->events_fd, b, sizeof b, ev_off(p, ev[i].seq)) < 0) rc = -1;
    }
    if (ev) {
        if (io_sync(p, p->events_fd) < 0) rc = -1;
        free(ev);
    }
    if (p->on_flush) p->on_flush(p->on_flush_user);
    return rc;
}

int rec_pool_save(rec_pool_t *p)
{
    if (!p->mounted || p->state != RP_READY) return 0;
    rec_pool_flush(p);
    pthread_mutex_lock(&p->mu);
    int rebuilding = p->rebuilding;
    pthread_mutex_unlock(&p->mu);
    if (rebuilding) return 0;
    if (table_write(p) < 0) {
        vg_log("rec: cannot save the chunk table in %s: %s", p->dir, strerror(errno));
        return -1;
    }
    int rc = journal_reset(p);
    if (evlog_hdr_save(p) < 0) rc = -1;
    p->save_due = 0;
    if (p->on_flush) p->on_flush(p->on_flush_user);
    return rc;
}

/* ---- the writer ------------------------------------------------------------------ */

/* The chunk a stream of camera `cam` (1-based) takes next (under mu): a
 * camera over its size quota gives up its own oldest; else a free one;
 * else the oldest closed one of any camera, unless on_full is stop. */
static int64_t pick_chunk(rec_pool_t *p, int cam)
{
    if (p->cfg.quota_bytes[cam - 1]) {
        uint64_t used = 0;
        int64_t oldest = -1;
        for (uint32_t i = 0; i < p->n; ++i) {
            const rec_chunk_t *c = &p->tab[i];
            if (!p->known[i] || c->cam != cam || (c->state != RC_OPEN && c->state != RC_CLOSED)) continue;
            used += p->chunk_bytes;
            if (c->state == RC_CLOSED && (oldest < 0 || c->start_us < p->tab[oldest].start_us)) oldest = i;
        }
        if (used + p->chunk_bytes > p->cfg.quota_bytes[cam - 1] && oldest >= 0) return oldest;
    }
    for (uint32_t i = 0; i < p->n; ++i)
        if (p->known[i] && p->tab[i].state == RC_FREE) return i;
    if (p->cfg.stop_when_full) return -1;
    int64_t oldest = -1;
    for (uint32_t i = 0; i < p->n; ++i) {
        const rec_chunk_t *c = &p->tab[i];
        if (p->known[i] && c->state == RC_CLOSED && (oldest < 0 || c->start_us < p->tab[oldest].start_us)) oldest = i;
    }
    return oldest;
}

static int chunk_open(rec_pool_t *p, rec_wstream_t *w, int cam, int stream, int64_t first_p)
{
    pthread_mutex_lock(&p->mu);
    int64_t idx = pick_chunk(p, cam + 1);
    if (idx < 0) {
        /* Full, or no chunk ready yet (being made, or not read back by
         * the rebuild yet): only the first is "disk full". */
        w->full = !(p->n < p->want || p->rebuilding);
        pthread_mutex_unlock(&p->mu);
        return -1;
    }
    w->full = 0;
    rec_chunk_t old = p->tab[idx];
    p->tab[idx].state = RC_OPEN;        /* taken: no other stream picks it */
    p->tab[idx].use = old.use + 1;
    p->tab[idx].cam = (uint8_t)(cam + 1);
    p->tab[idx].stream = (uint8_t)stream;
    p->tab[idx].nfrag = 0;
    p->tab[idx].flags = 0;
    pthread_mutex_unlock(&p->mu);

    mp4_buf_t b = { 0 };
    mp4_track_t t = { .codec = w->codec, .width = w->width, .height = w->height, .n_ps = w->n_ps };
    for (int i = 0; i < w->n_ps; ++i) {
        t.ps[i] = w->ps[i];
        t.ps_len[i] = w->ps_len[i];
    }
    uint8_t *head = calloc(1, REC_VGCH_SIZE);
    if (!head) goto fail;
    mp4_put(&b, head, REC_VGCH_SIZE);
    free(head);
    if (mp4_init(&b, &t, -1, 0) < 0 || b.len - REC_VGCH_SIZE > 0xFFFF) {
        vg_log("rec: CAM%d: its parameter sets do not make an init segment", cam + 1);
        goto fail;
    }
    rec_chunk_t r = { .state = RC_OPEN, .cam = (uint8_t)(cam + 1), .stream = (uint8_t)stream,
                      .codec = (uint8_t)w->codec, .use = old.use + 1, .no = (uint32_t)idx,
                      .init_len = (uint32_t)(b.len - REC_VGCH_SIZE),
                      .width = (uint16_t)w->width, .height = (uint16_t)w->height };
    int64_t st = first_p < 0 ? 0 : first_p - first_p % 9;
    r.start_us = (uint64_t)st / 9 * 100;
    r.end_us = r.start_us;
    r.bytes = (uint32_t)b.len;
    /* Write-ahead: the journal says the chunk is reused before its old
     * contents are overwritten. */
    rec_jrec_t j = { .type = RJ_OPEN, .a = r.cam, .b = r.stream, .c = r.codec, .chunk = r.no, .use = r.use,
                     .u64 = r.start_us, .w0 = r.init_len, .w1 = (uint32_t)r.width | (uint32_t)r.height << 16 };
    jappend(p, &j);
    if (rec_pool_flush(p) < 0) goto fail;
    char path[300];
    rec_pool_chunk_path(p, (uint32_t)idx, path, sizeof path);
    int fd = open(path, O_RDWR);
    if (fd < 0) goto fail;
    rec_vgch_build(b.p, p->pool_id, &r, NULL);
    if (write_all(p, fd, b.p, b.len, 0) < 0) {
        close(fd);
        goto fail;
    }
    mp4_buf_free(&b);
    pthread_mutex_lock(&p->mu);
    p->tab[idx] = r;
    w->open = 1;
    w->idx = (uint32_t)idx;
    w->fd = fd;
    w->rec = r;
    w->start_tick = st;
    w->nfrag = w->journaled = 0;
    w->nev = 0;
    w->cursor = r.bytes;
    w->frag_open = 0;
    w->dirty = 1;
    w->failed = 0;
    pthread_mutex_unlock(&p->mu);
    return 0;
fail:
    mp4_buf_free(&b);
    pthread_mutex_lock(&p->mu);
    w->failed = 1;
    p->tab[idx] = (rec_chunk_t){ .state = RC_FREE, .use = old.use + 1, .no = (uint32_t)idx };
    pthread_mutex_unlock(&p->mu);
    return -1;
}

static uint32_t reserve_for(uint32_t n) { return FRAG_PAD_MIN + (uint32_t)mp4_moof_size(n, 1) + 8; }

static void frag_begin(rec_pool_t *p, rec_wstream_t *w, int key, int gap)
{
    uint32_t n = w->max_frag_smp ? w->max_frag_smp * 3 / 2 + 8 : (p->cfg.fragment_s ? p->cfg.fragment_s : 4) * 30 + 8;
    if (n < 32) n = 32;
    if (n > 1024) n = 1024;
    w->reserve_n = n;
    w->frag_at = w->cursor;
    w->data_at = w->cursor + reserve_for(n);
    w->cursor = w->data_at;
    w->n_smp = 0;
    w->frag_key = key;
    w->frag_gap = gap;
    w->frag_open = 1;
}

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

/* Write the fragment's moof and mdat header in the room before its
 * samples; `next_p` is the presentation time of the AU after it (-1: none,
 * the last sample lasts as long as the one before). */
static void frag_finish(rec_pool_t *p, rec_wstream_t *w, int64_t next_p)
{
    if (!w->frag_open) return;
    w->frag_open = 0;
    uint32_t n = w->n_smp;
    if (!n) {
        w->cursor = w->frag_at;
        return;
    }
    int64_t *s = malloc(n * sizeof *s);
    mp4_sample_t *ms = malloc(n * sizeof *ms);
    uint8_t *mb = malloc(mp4_moof_size(n, 1) + 8);
    if (!s || !ms || !mb) {
        free(s);
        free(ms);
        free(mb);
        w->cursor = w->frag_at;     /* lost: the next fragment overwrites it */
        return;
    }
    for (uint32_t i = 0; i < n; ++i) s[i] = w->smp[i].p;
    qsort(s, n, sizeof *s, cmp_i64);
    int64_t minp = s[0];
    /* A B-frame of an open GOP shown before the chunk began: shift. */
    int64_t shift = minp < w->start_tick ? w->start_tick - minp : 0;
    int with_cts = 0;
    uint64_t sum = 0;
    for (uint32_t i = 0; i < n; ++i) {
        int64_t d = i + 1 < n ? s[i + 1] - s[i] : next_p > s[i] ? next_p - s[i] : w->last_dur > 0 ? w->last_dur : 3600;
        if (d < 1) d = 1;
        if (d > 0x7FFFFFFF) d = 0x7FFFFFFF;
        ms[i].dur = (uint32_t)d;
        ms[i].size = w->smp[i].size;
        ms[i].flags = w->smp[i].key ? MP4_SYNC : MP4_NON_SYNC;
        ms[i].cts = (int32_t)(w->smp[i].p - s[i]);
        with_cts |= ms[i].cts != 0;
        sum += (uint64_t)d;
    }
    uint64_t tfdt = (uint64_t)(minp + shift - w->start_tick);
    size_t msz = mp4_moof_size(n, with_cts);
    uint32_t moof_at = w->data_at - 8 - (uint32_t)msz, pad = moof_at - w->frag_at;
    uint32_t data_len = w->cursor - w->data_at;
    uint32_t fl = (w->frag_key ? REC_FRAG_KEY : 0) | (w->frag_gap ? REC_FRAG_GAP : 0);
    uint8_t ph[FRAG_PAD_MIN];
    put_be32(ph, pad);
    memcpy(ph + 4, "free", 4);
    memcpy(ph + 8, "vgfr", 4);
    rec_put32(ph + 12, w->rec.use);
    rec_put32(ph + 16, fl);
    mp4_moof_build(mb, w->nfrag + 1, tfdt, ms, n, with_cts);
    put_be32(mb + msz, 8 + data_len);
    memcpy(mb + msz + 4, "mdat", 4);
    int bad = write_all(p, w->fd, ph, sizeof ph, w->frag_at) < 0 || write_all(p, w->fd, mb, msz + 8, moof_at) < 0;
    free(s);
    free(ms);
    free(mb);
    if (bad) {
        pthread_mutex_lock(&p->mu);
        w->failed = 1;
        pthread_mutex_unlock(&p->mu);
        w->cursor = w->frag_at;
        return;
    }
    rec_frag_t f = { .t_ms = (uint32_t)(tfdt / 90), .dur_ms = (uint32_t)(sum / 90), .off = moof_at,
                     .size = (uint32_t)(msz + 8 + data_len) | fl };
    pthread_mutex_lock(&p->mu);
    if (w->nfrag == w->cap_frag) {
        uint32_t cap = w->cap_frag ? w->cap_frag * 2 : 256;
        rec_frag_t *nf = realloc(w->frags, cap * sizeof *nf);
        if (nf) {
            w->frags = nf;
            w->cap_frag = cap;
        }
    }
    if (w->nfrag < w->cap_frag) {
        w->frags[w->nfrag++] = f;
        w->rec.nfrag = w->nfrag;
        w->rec.bytes = w->cursor;
        w->rec.last_off = moof_at;
        w->rec.end_us = w->rec.start_us + (tfdt + sum) * 100 / 9;
        if (w->frag_gap) w->rec.flags |= RCF_GAPS;
        p->tab[w->idx] = w->rec;
    }
    w->bytes_total += msz + 8 + data_len;
    p->rate_bytes += msz + 8 + data_len;
    pthread_mutex_unlock(&p->mu);
    w->last_end = w->start_tick + (int64_t)(tfdt + sum);
    if (data_len + msz > w->max_frag_bytes) w->max_frag_bytes = data_len + (uint32_t)msz;
    if (n > w->max_frag_smp) w->max_frag_smp = n;
    w->dirty = 1;
}

static void chunk_close(rec_pool_t *p, rec_wstream_t *w)
{
    if (!w->open) return;
    frag_finish(p, w, -1);
    rec_chunk_t r = w->rec;
    uint32_t nev = w->nev;
    size_t z = rec_vgix_size(w->nfrag, nev);
    while (nev && r.bytes + z + 8 > p->chunk_bytes) z = rec_vgix_size(w->nfrag, --nev);
    uint8_t *ix = malloc(z + 8);
    int bad = !ix || r.bytes + z + 8 > p->chunk_bytes;
    if (!bad) {
        pthread_mutex_lock(&p->mu);         /* the events' durations change under mu */
        rec_vgix_build(ix, p->pool_id, r.use, w->frags, w->nfrag, w->ev, nev);
        pthread_mutex_unlock(&p->mu);
        put_be32(ix + z, p->chunk_bytes - r.bytes - (uint32_t)z);
        memcpy(ix + z + 4, "free", 4);
        bad = write_all(p, w->fd, ix, z + 8, r.bytes) < 0;
    }
    free(ix);
    r.state = RC_CLOSED;
    r.vgix_len = (uint32_t)z;
    if (nev) r.flags |= RCF_EVENTS;
    if (!bad) bad = vgch_write_b(p, w->fd, &r) < 0;
    if (!bad) bad = io_sync(p, w->fd) < 0;
    close(w->fd);
    w->fd = -1;
    if (bad) {
        /* What the journal holds stands; the recovery closes it at the
         * next mount. */
        vg_log("rec: CAM%d: closing chunk %u failed: %s", r.cam, r.no, strerror(errno));
        pthread_mutex_lock(&p->mu);
        w->open = 0;
        w->failed = 1;
        w->nfrag = 0;
        pthread_mutex_unlock(&p->mu);
        return;
    }
    rec_jrec_t j = { .type = RJ_CLOSE, .a = (uint8_t)r.flags, .chunk = r.no, .use = r.use, .u64 = r.end_us,
                     .w0 = r.bytes, .w1 = (r.nfrag & 0xFFFF) | nev << 16 };
    w->journaled = w->nfrag;            /* the close record covers them */
    jappend(p, &j);
    pthread_mutex_lock(&p->mu);
    p->tab[r.no] = r;
    w->open = 0;
    w->nfrag = 0;
    w->nev = 0;
    p->save_due = 1;
    pthread_mutex_unlock(&p->mu);
}

void rec_pool_close_stream(rec_pool_t *p, int cam, int stream)
{
    if (cam < 0 || cam >= REC_CAMS || stream < 0 || stream >= REC_STREAMS) return;
    chunk_close(p, &p->ws[cam][stream]);
}

/* The AU's presentation time on the stream's clock: the camera's own
 * timestamps from where they were anchored to the wall clock, anchored
 * again when they are more than 2 s off it (a reconnect, a camera's clock
 * reset, drift), never going back. Timestamps that are only the arrival
 * time (NETIP) jitter with the network: a frame late, the next one right
 * behind it. Without B-frames the time is smoothed: each frame one
 * average interval (measured over the first eight, then followed) after
 * the one before, corrected by a sixteenth of what the camera's time
 * says, at most an eighth of an interval at a time, so the stream keeps
 * its clock and players see even frame durations; a step of a second or
 * more is taken as it is. */
static int64_t map_time(rec_wstream_t *w, const rec_au_t *au)
{
    int64_t wall = (int64_t)(au->wall_us * 9 / 100);
    uint64_t pts = au->pts_us ? au->pts_us : au->wall_us;
    int64_t raw = w->anchor_tick + (int64_t)(pts - w->anchor_pts) * 9 / 100;
    if (!w->clock || raw > wall + 2 * TICKS_PER_S || raw < wall - 2 * TICKS_PER_S) {
        w->clock = 1;
        w->anchor_pts = pts;
        w->anchor_tick = raw = wall;
        w->last_raw = 0;
        w->n_int = 0;
    }
    if (w->last_raw && raw < w->last_raw - (w->avg > 2 ? w->avg / 2 : 1) && w->frag_open && w->n_smp && !au->key &&
        raw > w->smp[0].p) {
        /* Earlier than the frame before, later than its keyframe: a B-frame. */
        w->bframes = 1;
        return raw;
    }
    int64_t p = raw;
    if (w->last_raw && !w->bframes) {
        int64_t d = raw - w->last_raw;
        if (w->n_int < 8) {
            if (d > 0 && d < TICKS_PER_S) {
                w->sum_int += d;
                if (++w->n_int == 8) w->avg = w->sum_int / 8;
            }
        } else if (d > 0 && d < 4 * w->avg) {
            w->avg += (d - w->avg) / 16;
        }
        int64_t exp = w->last_p + w->avg, err = raw - exp;
        if (w->n_int >= 8 && w->avg > 0 && err > -TICKS_PER_S && err < TICKS_PER_S) {
            int64_t c = err / 16, lim = w->avg / 8;
            p = exp + (c > lim ? lim : c < -lim ? -lim : c);
        }
    }
    if (!w->last_raw) w->sum_int = 0, w->n_int = 0;
    int64_t min = w->last_p + (w->avg > 4 ? w->avg / 4 : 1);
    if (w->last_p && p < min) p = min;
    if (p < w->last_end) p = w->last_end;           /* after the fragment before */
    if (w->last_p && p - w->last_p < TICKS_PER_S) w->last_dur = p - w->last_p;
    w->last_p = p;
    w->last_raw = raw;
    return p;
}

static int same_format(const rec_wstream_t *w, const rec_au_t *au, unsigned wd, unsigned ht)
{
    if (w->codec != au->codec || w->width != wd || w->height != ht || w->n_ps != au->n_ps) return 0;
    for (int i = 0; i < au->n_ps; ++i)
        if (w->ps_len[i] != au->ps_len[i] || memcmp(w->ps[i], au->ps[i], au->ps_len[i])) return 0;
    return 1;
}

void rec_pool_put(rec_pool_t *p, const rec_au_t *au)
{
    if (!p->mounted || p->state != RP_READY || au->cam >= REC_CAMS || au->stream >= REC_STREAMS) return;
    rec_wstream_t *w = &p->ws[au->cam][au->stream];
    w->last_au = (time_t)(au->wall_us / 1000000);
    if (au->gap) w->gap_next = 1;
    if (w->need_key && !au->key) return;
    if (au->key) {
        w->need_key = 0;
        unsigned wd = w->width, ht = w->height;
        int sps = au->codec == VG_CODEC_H265 ? 1 : 0;
        if (au->n_ps > sps) {
            vg_stream_info_t si;
            if (sps_parse(au->codec, au->ps[sps], au->ps_len[sps], &si) == 0) {
                wd = si.width;
                ht = si.height;
            }
        }
        if (au->n_ps && !same_format(w, au, wd, ht)) {
            if (w->open) chunk_close(p, w);
            w->codec = au->codec;
            w->width = wd;
            w->height = ht;
            w->n_ps = au->n_ps;
            for (int i = 0; i < au->n_ps; ++i) {
                size_t l = au->ps_len[i] < REC_PS_BYTES ? au->ps_len[i] : REC_PS_BYTES;
                memcpy(w->ps[i], au->ps[i], l);
                w->ps_len[i] = (uint16_t)l;
            }
        }
    }
    if (!w->n_ps) {                     /* no parameter sets seen yet */
        w->need_key = 1;
        return;
    }
    int64_t pt = map_time(w, au);
    if (!w->open) {
        if (!au->key || chunk_open(p, w, au->cam, au->stream, pt) < 0) {
            w->need_key = 1;
            return;
        }
    }
    uint32_t vgix = (uint32_t)rec_vgix_size(w->nfrag + 2, w->nev) + 64;
    int64_t span = w->frag_open && w->n_smp ? pt - w->smp[0].p : 0;
    unsigned fs = p->cfg.fragment_s ? p->cfg.fragment_s : 4;
    int start = !w->frag_open || (au->key && w->n_smp) || span >= (int64_t)fs * TICKS_PER_S || w->n_smp >= w->reserve_n;
    if (start) {
        frag_finish(p, w, pt);
        uint32_t est = w->max_frag_bytes * 2 > au->len * 2 ? w->max_frag_bytes * 2 : au->len * 2;
        if (est < (64u << 10)) est = 64u << 10;
        uint64_t need = (uint64_t)w->cursor + reserve_for(1024) + est + vgix;
        if (need > p->chunk_bytes || w->nfrag >= FRAG_MAX) {
            if (!au->key && (uint64_t)w->cursor + reserve_for(1024) + au->len + vgix <= p->chunk_bytes &&
                w->nfrag < FRAG_MAX) {
                /* no room for a whole GOP, but for this AU: on to the next keyframe */
            } else {
                chunk_close(p, w);
                if (!au->key || chunk_open(p, w, au->cam, au->stream, pt) < 0) {
                    w->need_key = 1;
                    w->gap_next = 1;
                    return;
                }
                vgix = (uint32_t)rec_vgix_size(2, 0) + 64;
            }
        }
        int gap = w->gap_next;
        w->gap_next = 0;
        frag_begin(p, w, au->key, gap);
    }
    if ((uint64_t)w->cursor + au->len + vgix > p->chunk_bytes) {
        /* A GOP larger than the room left. */
        chunk_close(p, w);
        w->need_key = 1;
        w->gap_next = 1;
        if (REC_VGCH_SIZE + 4096u + reserve_for(1024) + au->len + vgix > p->chunk_bytes) {
            pthread_mutex_lock(&p->mu);
            w->failed = 1;              /* an AU larger than a chunk */
            pthread_mutex_unlock(&p->mu);
        }
        return;
    }
    if (write_all(p, w->fd, au->data, au->len, w->cursor) < 0) {
        vg_log("rec: CAM%d: writing chunk %u failed: %s", au->cam + 1, w->idx, strerror(errno));
        w->frag_open = 0;
        w->cursor = w->frag_at;
        chunk_close(p, w);
        pthread_mutex_lock(&p->mu);
        w->failed = 1;
        pthread_mutex_unlock(&p->mu);
        w->need_key = 1;
        w->gap_next = 1;
        return;
    }
    if (w->n_smp == w->cap_smp) {
        uint32_t cap = w->cap_smp ? w->cap_smp * 2 : 64;
        rec_wsamp_t *n = realloc(w->smp, cap * sizeof *n);
        if (!n) return;
        w->smp = n;
        w->cap_smp = cap;
    }
    w->smp[w->n_smp++] = (rec_wsamp_t){ .size = au->len, .p = pt, .key = (uint8_t)au->key };
    w->cursor += au->len;
    w->dirty = 1;
    pthread_mutex_lock(&p->mu);
    w->last_write = w->last_au;
    w->failed = 0;
    pthread_mutex_unlock(&p->mu);
}

void rec_pool_break(rec_pool_t *p, int cam, int stream)
{
    if (cam < 0 || cam >= REC_CAMS || stream < 0 || stream >= REC_STREAMS || !p->mounted) return;
    rec_wstream_t *w = &p->ws[cam][stream];
    if (w->open) frag_finish(p, w, -1);
    w->clock = 0;
    w->gap_next = 1;
    w->need_key = 1;
}

/* ---- events ------------------------------------------------------------------- */

/* End event (c, t) at its `last` time: its record gets the duration,
 * in the log and in the vgix of the chunk it began in while that is still
 * open. Under mu. */
static void event_end(rec_pool_t *p, int c, int t)
{
    rec_active_t *a = &p->active[c][t];
    a->on = 0;
    rec_wstream_t *w = &p->ws[c][0];
    uint32_t dur = (uint32_t)((a->last - a->start) / 1000);
    for (uint32_t i = w->nev; i-- > 0;)
        if (w->ev[i].seq == a->seq) {
            w->ev[i].dur_ms = dur;
            ev_pend(p, &w->ev[i]);
            return;
        }
    /* Not in an open chunk any more: rewrite it from what it was. */
    rec_event_t e = { .time_us = a->start, .dur_ms = dur, .cam = (uint8_t)(c + 1), .type = (uint8_t)t,
                      .seq = a->seq, .chunk = REC_NONE, .frag = REC_NONE };
    int found = 0;
    for (uint32_t i = p->n_ev_pend; i-- > 0;)
        if (p->ev_pend[i].seq == a->seq) {
            e = p->ev_pend[i];
            found = 1;
            break;
        }
    rec_event_t d;
    if (!found && p->events_fd >= 0 && ev_read(p, p->events_fd, a->seq, &d) == 0) e = d;
    e.dur_ms = dur;
    ev_pend(p, &e);
}

void rec_pool_event(rec_pool_t *p, int cam, int type, int active, uint64_t now_us)
{
    if (cam < 0 || cam >= REC_CAMS || type <= 0 || type >= REC_EV_TYPES) return;
    pthread_mutex_lock(&p->mu);
    rec_active_t *a = &p->active[cam][type];
    if (!p->mounted || p->state != RP_READY || !p->ev_cap || a->on == !!active) {
        pthread_mutex_unlock(&p->mu);
        return;
    }
    if (!active) {
        a->last = now_us;
        event_end(p, cam, type);
        pthread_mutex_unlock(&p->mu);
        return;
    }
    rec_wstream_t *w = &p->ws[cam][0];
    /* In the chunk while its stream is being written, not merely open
     * (the camera gone for a moment). */
    int rec = w->open && (uint64_t)w->last_write * 1000000 + 3000000 >= now_us;
    rec_event_t e = { .time_us = now_us, .cam = (uint8_t)(cam + 1), .type = (uint8_t)type, .seq = p->ev_head++,
                      .chunk = rec ? w->idx : REC_NONE, .frag = rec ? w->nfrag : REC_NONE };
    a->seq = e.seq;
    a->start = a->last = now_us;
    a->on = 1;
    ev_pend(p, &e);
    if (rec && w->nev < EV_PER_CHUNK) {
        if (w->nev == w->cap_ev) {
            uint32_t cap = w->cap_ev ? w->cap_ev * 2 : 16;
            rec_event_t *n = realloc(w->ev, cap * sizeof *n);
            if (n) {
                w->ev = n;
                w->cap_ev = cap;
            }
        }
        if (w->nev < w->cap_ev) {
            w->ev[w->nev++] = e;
            w->rec.flags |= RCF_EVENTS;
            p->tab[w->idx].flags |= RCF_EVENTS;
        }
    }
    pthread_mutex_unlock(&p->mu);
}

/* Every event still on ends now (a clean stop: the next start begins them again). */
static void events_end_all(rec_pool_t *p, uint64_t now_us)
{
    pthread_mutex_lock(&p->mu);
    for (int c = 0; c < REC_CAMS; ++c)
        for (int t = 1; t < REC_EV_TYPES; ++t)
            if (p->active[c][t].on) {
                p->active[c][t].last = now_us;
                event_end(p, c, t);
            }
    pthread_mutex_unlock(&p->mu);
}

/* ---- freeing chunks --------------------------------------------------------------- */

static int chunk_free(rec_pool_t *p, uint32_t idx)
{
    pthread_mutex_lock(&p->mu);
    rec_chunk_t c = p->tab[idx];
    pthread_mutex_unlock(&p->mu);
    if (c.state != RC_CLOSED) return -1;
    char path[300];
    rec_pool_chunk_path(p, idx, path, sizeof path);
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    rec_chunk_t f = { .state = RC_FREE, .use = c.use, .no = idx };
    int rc = vgch_write(p, fd, &f) < 0 || io_sync(p, fd) < 0 ? -1 : 0;
    close(fd);
    if (rc < 0) return -1;
    rec_jrec_t j = { .type = RJ_FREE, .chunk = idx, .use = c.use };
    jappend(p, &j);
    pthread_mutex_lock(&p->mu);
    p->tab[idx] = f;
    p->save_due = 1;
    pthread_mutex_unlock(&p->mu);
    return 0;
}

int rec_pool_delete(rec_pool_t *p, int cam, uint64_t from_us, uint64_t to_us)
{
    if (!p->mounted || p->state != RP_READY) return 0;
    int n = 0;
    for (uint32_t i = 0; i < p->n; ++i) {
        pthread_mutex_lock(&p->mu);
        rec_chunk_t c = p->tab[i];
        int k = p->known[i];
        pthread_mutex_unlock(&p->mu);
        if (!k || c.state != RC_CLOSED || (cam && c.cam != cam) || c.start_us < from_us || c.end_us > to_us) continue;
        n += chunk_free(p, i) == 0;
    }
    if (n) rec_pool_save(p);
    return n;
}

static void age_quota(rec_pool_t *p, uint64_t now_us)
{
    for (uint32_t i = 0; i < p->n; ++i) {
        pthread_mutex_lock(&p->mu);
        rec_chunk_t c = p->tab[i];
        time_t age = c.cam >= 1 && c.cam <= REC_CAMS ? p->cfg.quota_age[c.cam - 1] : 0;
        int k = p->known[i];
        pthread_mutex_unlock(&p->mu);
        if (k && c.state == RC_CLOSED && age && c.end_us + (uint64_t)age * 1000000 < now_us) chunk_free(p, i);
    }
}

void rec_pool_tick(rec_pool_t *p, time_t now, uint64_t now_us)
{
    if (!p->mounted || p->state != RP_READY) return;
    for (int c = 0; c < REC_CAMS; ++c)
        for (int s = 0; s < REC_STREAMS; ++s) {
            rec_wstream_t *w = &p->ws[c][s];
            if (w->open && now - w->last_au > REC_IDLE_CLOSE) chunk_close(p, w);
        }
    if (now >= p->next_quota) {
        p->next_quota = now + 60;
        age_quota(p, now_us);
    }
    if (now >= p->next_flush) {
        rec_pool_flush(p);
        p->next_flush = now + p->flush_s;
    }
    if (p->save_due || now >= p->next_save || p->journal_len > JOURNAL_SAVE) {
        rec_pool_save(p);
        p->next_save = now + (p->cfg.index_save_s ? p->cfg.index_save_s : 300);
    }
    pthread_mutex_lock(&p->mu);
    if (now - p->rate_at >= 10) {
        if (p->rate_at) p->rate = (unsigned)(p->rate_bytes / (uint64_t)(now - p->rate_at));
        p->rate_bytes = 0;
        p->rate_at = now;
    }
    pthread_mutex_unlock(&p->mu);
}

void rec_pool_unmount(rec_pool_t *p)
{
    if (p->mounted && p->state == RP_READY) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        events_end_all(p, (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000);
        for (int c = 0; c < REC_CAMS; ++c)
            for (int s = 0; s < REC_STREAMS; ++s) chunk_close(p, &p->ws[c][s]);
        rec_pool_save(p);
    }
    release(p);
    pthread_mutex_lock(&p->mu);
    p->state = RP_OFF;
    pthread_mutex_unlock(&p->mu);
}

void rec_pool_abandon(rec_pool_t *p)
{
    release(p);
    pthread_mutex_lock(&p->mu);
    p->state = RP_OFF;
    pthread_mutex_unlock(&p->mu);
}

int rec_pool_takeover(rec_pool_t *p, time_t now)
{
    if (!p->mounted || p->state != RP_READONLY) return -1;
    id_t_ id;
    if (id_read(p->dir, &id) < 0) return -1;
    snprintf(id.owner, sizeof id.owner, "%s", p->cfg.owner);
    snprintf(id.host, sizeof id.host, "%s", p->cfg.host);
    if (id_write(p->dir, &id) < 0) return -1;
    vg_log("rec: taking over the pool in %s", p->dir);
    rec_pool_cfg_t cfg = p->cfg;
    return rec_pool_mount(p, &cfg, now);
}

/* ---- background: rebuild, pre-allocation -------------------------------------------- */

int rec_pool_rebuild_due(rec_pool_t *p)
{
    pthread_mutex_lock(&p->mu);
    int due = p->mounted && (p->rebuilding || p->events_rebuild) && p->state == RP_READY;
    pthread_mutex_unlock(&p->mu);
    return due;
}

static int cmp_ev(const void *a, const void *b)
{
    const rec_event_t *x = a, *y = b;
    return x->time_us < y->time_us ? -1 : x->time_us > y->time_us;
}

/* The event log was lost: what the chunks' vgix keep, with what was
 * logged since, in time order. */
static void events_refill(rec_pool_t *p, volatile int *stop)
{
    rec_event_t *all = NULL;
    uint32_t n = 0, cap = 0;
    for (uint32_t i = 0; i < p->n && !*stop; ++i) {
        rec_event_t *ev = NULL;
        int k = rec_pool_chunk_events(p, i, &ev);
        pthread_mutex_lock(&p->mu);
        int closed = p->tab[i].state == RC_CLOSED;
        pthread_mutex_unlock(&p->mu);
        for (int j = 0; closed && j < k; ++j) {
            if (n == cap) {
                cap = cap ? cap * 2 : 256;
                rec_event_t *x = realloc(all, cap * sizeof *x);
                if (!x) break;
                all = x;
            }
            all[n++] = ev[j];
        }
        free(ev);
    }
    if (*stop) {
        free(all);
        return;
    }
    pthread_mutex_lock(&p->mu);
    /* The events logged since the mount (seq 0 up). */
    for (uint32_t s = 0; s < p->ev_head; ++s) {
        rec_event_t e;
        int found = 0;
        for (uint32_t k = p->n_ev_pend; k-- > 0;)
            if (p->ev_pend[k].seq == s) { e = p->ev_pend[k]; found = 1; break; }
        if (!found && ev_read(p, p->events_fd, s, &e) < 0) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 256;
            rec_event_t *x = realloc(all, cap * sizeof *x);
            if (!x) break;
            all = x;
        }
        all[n++] = e;
    }
    qsort(all, n, sizeof *all, cmp_ev);
    if (n > p->ev_cap) {
        memmove(all, all + (n - p->ev_cap), p->ev_cap * sizeof *all);
        n = p->ev_cap;
    }
    p->n_ev_pend = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t old = all[i].seq;
        all[i].seq = i;
        for (int c = 0; c < REC_CAMS; ++c)
            for (int t = 0; t < REC_EV_TYPES; ++t)
                if (p->active[c][t].on && p->active[c][t].seq == old && all[i].cam == c + 1 && all[i].type == t)
                    p->active[c][t].seq = i;
        ev_pend(p, &all[i]);
    }
    p->ev_head = n;
    p->events_rebuild = 0;
    p->save_due = 1;
    pthread_mutex_unlock(&p->mu);
    free(all);
    vg_log("rec: event log rebuilt from the chunks: %u events", n);
}

void rec_pool_rebuild(rec_pool_t *p, volatile int *stop)
{
    pthread_mutex_lock(&p->mu);
    uint32_t n = p->n;
    int rebuilding = p->rebuilding;
    pthread_mutex_unlock(&p->mu);
    if (rebuilding) {
        for (uint32_t i = 0; i < n && !*stop; ++i) {
            pthread_mutex_lock(&p->mu);
            int k = p->known[i];
            pthread_mutex_unlock(&p->mu);
            if (k) continue;
            rec_chunk_t c;
            vgch_read(p, i, &c);
            if (c.state == RC_OPEN) recover_chunk(p, &c, 0);
            pthread_mutex_lock(&p->mu);
            p->tab[i] = c;
            p->known[i] = 1;
            p->rebuild_done++;
            pthread_mutex_unlock(&p->mu);
        }
        if (*stop) return;
        pthread_mutex_lock(&p->mu);
        p->rebuilding = 0;
        p->save_due = 1;
        pthread_mutex_unlock(&p->mu);
        vg_log("rec: chunk table rebuilt: %u chunks", n);
    }
    pthread_mutex_lock(&p->mu);
    int refill = p->events_rebuild;
    pthread_mutex_unlock(&p->mu);
    if (refill) events_refill(p, stop);
}

int rec_pool_prealloc_due(rec_pool_t *p)
{
    pthread_mutex_lock(&p->mu);
    int due = p->mounted && p->state == RP_READY && p->n < p->want;
    pthread_mutex_unlock(&p->mu);
    return due;
}

/* Zeros where the filesystem cannot allocate (NFSv3, vfat on older
 * kernels): 1 MiB at a time, resting three times as long as each took,
 * so the recording's writes keep most of the disk. */
static int zero_fill(int fd, off_t from, off_t to, volatile int *stop)
{
    static uint8_t z[1 << 20];
    while (from < to && !*stop) {
        size_t n = (size_t)(to - from) < sizeof z ? (size_t)(to - from) : sizeof z;
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        ssize_t w = pwrite(fd, z, n, from);
        if (w <= 0) return -1;
        from += w;
        clock_gettime(CLOCK_MONOTONIC, &b);
        long ns = (b.tv_sec - a.tv_sec) * 1000000000L + (b.tv_nsec - a.tv_nsec);
        struct timespec rest = { .tv_sec = ns * 3 / 1000000000L, .tv_nsec = ns * 3 % 1000000000L };
        nanosleep(&rest, NULL);
    }
    return *stop ? -1 : fdatasync(fd);
}

void rec_pool_prealloc(rec_pool_t *p, volatile int *stop)
{
    for (;;) {
        pthread_mutex_lock(&p->mu);
        uint32_t i = p->n, want = p->want;
        int ok = p->mounted && p->state == RP_READY;
        pthread_mutex_unlock(&p->mu);
        if (!ok || i >= want || *stop) return;
        char path[300];
        rec_pool_chunk_path(p, i, path, sizeof path);
        struct statvfs fs;
        struct stat st;
        int fd = open(path, O_RDWR | O_CREAT, 0644);
        if (fd < 0 || fstat(fd, &st) < 0) {
            vg_log("rec: cannot create %s: %s", path, strerror(errno));
            if (fd >= 0) close(fd);
            pthread_mutex_lock(&p->mu);
            p->want = i;
            pthread_mutex_unlock(&p->mu);
            return;
        }
        uint64_t need = st.st_size < (off_t)p->chunk_bytes ? p->chunk_bytes - (uint64_t)st.st_size : 0;
        if (need && (statvfs(p->dir, &fs) < 0 ||
                     (uint64_t)fs.f_bavail * fs.f_frsize < need + reserve_of((uint64_t)fs.f_blocks * fs.f_frsize))) {
            close(fd);
            vg_log("rec: %s: room for %u chunks, not %u", p->dir, i, want);
            pthread_mutex_lock(&p->mu);
            p->want = i;
            pthread_mutex_unlock(&p->mu);
            return;
        }
        int rc = need ? posix_fallocate(fd, 0, p->chunk_bytes) : 0;
        if (rc == EOPNOTSUPP || rc == EINVAL || rc == ENOSYS)
            rc = zero_fill(fd, st.st_size, p->chunk_bytes, stop) < 0 ? errno : 0;
        else if (!rc && need)
            rc = fdatasync(fd) < 0 ? errno : 0;
        close(fd);
        if (rc) {
            if (!*stop) vg_log("rec: cannot allocate %s: %s", path, strerror(rc));
            pthread_mutex_lock(&p->mu);
            if (!*stop) p->want = i;
            pthread_mutex_unlock(&p->mu);
            return;
        }
        pthread_mutex_lock(&p->mu);
        if (p->mounted && p->n == i && tab_reserve(p, i + 1) == 0) {
            p->tab[i] = (rec_chunk_t){ .state = RC_FREE, .no = i };
            p->known[i] = 1;
            p->n = i + 1;
            p->save_due = 1;
        }
        pthread_mutex_unlock(&p->mu);
    }
}

/* ---- reading -------------------------------------------------------------------- */

static int open_ws(rec_pool_t *p, uint32_t idx, rec_wstream_t **out)
{
    for (int c = 0; c < REC_CAMS; ++c)
        for (int s = 0; s < REC_STREAMS; ++s)
            if (p->ws[c][s].open && p->ws[c][s].idx == idx) {
                *out = &p->ws[c][s];
                return 0;
            }
    return -1;
}

/* A closed chunk's vgix, read (malloc'ed). */
static uint8_t *vgix_load(rec_pool_t *p, const rec_chunk_t *c, uint32_t *nf, const uint8_t **fr, uint32_t *ne,
                          const uint8_t **ev)
{
    if (!c->vgix_len || c->bytes + (uint64_t)c->vgix_len > p->chunk_bytes) return NULL;
    char path[300];
    rec_pool_chunk_path(p, c->no, path, sizeof path);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    uint8_t *b = malloc(c->vgix_len);
    if (b && (read_all(fd, b, c->vgix_len, c->bytes) < 0 ||
              rec_vgix_parse(b, c->vgix_len, p->pool_id, c->use, nf, fr, ne, ev) < 0)) {
        free(b);
        b = NULL;
    }
    close(fd);
    return b;
}

int rec_pool_frags(rec_pool_t *p, uint32_t idx, rec_chunk_t *rec, rec_frag_t **out)
{
    *out = NULL;
    pthread_mutex_lock(&p->mu);
    if (!p->mounted || idx >= p->n) {
        pthread_mutex_unlock(&p->mu);
        return -1;
    }
    rec_chunk_t c = p->tab[idx];
    if (rec) *rec = c;
    rec_wstream_t *w;
    if (c.state == RC_OPEN && open_ws(p, idx, &w) == 0) {
        int n = (int)w->nfrag;
        *out = malloc((n ? n : 1) * sizeof **out);
        if (*out) memcpy(*out, w->frags, (size_t)n * sizeof **out);
        pthread_mutex_unlock(&p->mu);
        return *out ? n : -1;
    }
    pthread_mutex_unlock(&p->mu);
    if (c.state != RC_CLOSED) return c.state == RC_OPEN ? 0 : -1;    /* another recorder's open chunk: none known */
    uint32_t nf, ne;
    const uint8_t *fr, *ev;
    uint8_t *b = vgix_load(p, &c, &nf, &fr, &ne, &ev);
    if (!b) return -1;
    *out = malloc((nf ? nf : 1) * sizeof **out);
    if (*out)
        for (uint32_t i = 0; i < nf; ++i) rec_frag_decode(&(*out)[i], fr + (size_t)i * REC_FRAG);
    free(b);
    return *out ? (int)nf : -1;
}

int rec_pool_chunk_events(rec_pool_t *p, uint32_t idx, rec_event_t **out)
{
    *out = NULL;
    pthread_mutex_lock(&p->mu);
    if (!p->mounted || idx >= p->n) {
        pthread_mutex_unlock(&p->mu);
        return -1;
    }
    rec_chunk_t c = p->tab[idx];
    rec_wstream_t *w;
    if (c.state == RC_OPEN && open_ws(p, idx, &w) == 0) {
        int n = (int)w->nev;
        *out = malloc((n ? n : 1) * sizeof **out);
        if (*out) memcpy(*out, w->ev, (size_t)n * sizeof **out);
        pthread_mutex_unlock(&p->mu);
        return *out ? n : -1;
    }
    pthread_mutex_unlock(&p->mu);
    if (c.state != RC_CLOSED || !(c.flags & RCF_EVENTS)) return 0;
    uint32_t nf, ne;
    const uint8_t *fr, *ev;
    uint8_t *b = vgix_load(p, &c, &nf, &fr, &ne, &ev);
    if (!b) return -1;
    *out = malloc((ne ? ne : 1) * sizeof **out);
    int n = 0;
    for (uint32_t i = 0; *out && i < ne; ++i)
        if (rec_event_decode(&(*out)[n], p->pool_id, ev + (size_t)i * REC_EVREC) == 0) ++n;
    free(b);
    return *out ? n : -1;
}

/* ---- the medium -------------------------------------------------------------------- */

static int read_flag(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int c = fgetc(f);
    fclose(f);
    return c == '0' ? 0 : c == '1' ? 1 : -1;
}

unsigned rec_media_flush(const char *dir, char *kind, size_t cap)
{
    char fs[64], src[256];
    snprintf(kind, cap, "disk");
    if (rec_place_mount(dir, NULL, fs, sizeof fs, src, sizeof src) < 0) return 2;
    static const char *const shares[] = { "nfs", "nfs4", "cifs", "smb3", "smbfs", "fuse.sshfs", "9p" };
    for (size_t i = 0; i < sizeof shares / sizeof shares[0]; ++i)
        if (!strcmp(fs, shares[i])) {
            snprintf(kind, cap, "share");
            return 2;
        }
    const char *name = strrchr(src, '/');
    name = name ? name + 1 : src;
    if (!strncmp(name, "mmcblk", 6)) {
        snprintf(kind, cap, "flash");
        return 5;
    }
    char path[400];
    snprintf(path, sizeof path, "/sys/class/block/%s/queue/rotational", name);
    int r = read_flag(path);
    if (r < 0) {
        snprintf(path, sizeof path, "/sys/class/block/%s/../queue/rotational", name);
        r = read_flag(path);
    }
    if (r == 0) {
        snprintf(kind, cap, "flash");
        return 5;
    }
    return 2;
}
