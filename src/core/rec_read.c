/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rec_read.c — queries and video out of a recording pool (rec_read.h). */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rec_read.h"

#define SPAN_JOIN_US 500000             /* fragments this close are one span */
#define MOOF_MAX     (64u << 10)
#define COPY_BLOCK   (256u << 10)

static int pread_all(int fd, void *buf, size_t n, off_t off)
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

static int has_video(const rec_chunk_t *c) { return (c->state == RC_OPEN || c->state == RC_CLOSED) && c->nfrag; }

/* ---- days ------------------------------------------------------------------- */

int rec_q_days(rec_pool_t *p, int cam, int stream, int year, int month, uint32_t *mask)
{
    *mask = 0;
    if (month < 1 || month > 12) return -1;
    /* Local midnights of the month's days and the next month's first. */
    uint64_t mid[33];
    int days = 0;
    for (int d = 1; d <= 32; ++d) {
        struct tm tm = { .tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = d, .tm_isdst = -1 };
        time_t t = mktime(&tm);
        if (t == (time_t)-1) return -1;
        mid[d - 1] = (uint64_t)t * 1000000;
        if (tm.tm_mon != month - 1) {
            days = d - 1;
            break;
        }
    }
    if (!days) return -1;
    pthread_mutex_lock(&p->mu);
    for (uint32_t i = 0; i < p->n; ++i) {
        const rec_chunk_t *c = &p->tab[i];
        if (!p->known[i] || !has_video(c) || c->cam != cam || c->stream != stream) continue;
        if (c->end_us <= mid[0] || c->start_us >= mid[days]) continue;
        /* the first day whose end is after the start, the last whose start is before the end */
        int lo = 0, hi = days - 1;
        while (lo < hi) {
            int m = (lo + hi) / 2;
            if (mid[m + 1] > c->start_us) hi = m; else lo = m + 1;
        }
        int a = lo;
        lo = a;
        hi = days - 1;
        while (lo < hi) {
            int m = (lo + hi + 1) / 2;
            if (mid[m] < c->end_us) lo = m; else hi = m - 1;
        }
        for (int d = a; d <= lo; ++d) *mask |= 1u << d;
    }
    pthread_mutex_unlock(&p->mu);
    return 0;
}

/* ---- the chunks of a stream ---------------------------------------------------- */

typedef struct {
    uint32_t idx;
    uint64_t start;
} cref_t;

static int cmp_cref(const void *a, const void *b)
{
    const cref_t *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

/* The chunks of (cam, stream) with video overlapping [from, to), by time. */
static int chunks_of(rec_pool_t *p, int cam, int stream, uint64_t from, uint64_t to, cref_t **out)
{
    *out = NULL;
    int n = 0, cap = 0;
    pthread_mutex_lock(&p->mu);
    for (uint32_t i = 0; i < p->n; ++i) {
        const rec_chunk_t *c = &p->tab[i];
        if (!p->known[i] || !has_video(c) || c->cam != cam || c->stream != stream) continue;
        if (c->start_us >= to || c->end_us <= from) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            cref_t *r = realloc(*out, (size_t)cap * sizeof *r);
            if (!r) break;
            *out = r;
        }
        (*out)[n++] = (cref_t){ i, c->start_us };
    }
    pthread_mutex_unlock(&p->mu);
    if (n > 1) qsort(*out, (size_t)n, sizeof **out, cmp_cref);
    return n;
}

static void span_add(rec_span_t **sp, int *n, int *cap, uint64_t a, uint64_t b)
{
    if (b <= a) return;
    if (*n && a <= (*sp)[*n - 1].to_us + SPAN_JOIN_US) {
        if (b > (*sp)[*n - 1].to_us) (*sp)[*n - 1].to_us = b;
        return;
    }
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        rec_span_t *r = realloc(*sp, (size_t)*cap * sizeof *r);
        if (!r) return;
        *sp = r;
    }
    (*sp)[(*n)++] = (rec_span_t){ a, b };
}

int rec_q_spans(rec_pool_t *p, int cam, int stream, uint64_t from, uint64_t to, rec_span_t **out)
{
    *out = NULL;
    cref_t *cs;
    int nc = chunks_of(p, cam, stream, from, to, &cs);
    int n = 0, cap = 0;
    for (int k = 0; k < nc; ++k) {
        pthread_mutex_lock(&p->mu);
        rec_chunk_t c = p->tab[cs[k].idx];
        pthread_mutex_unlock(&p->mu);
        if (!(c.flags & RCF_GAPS)) {
            uint64_t a = c.start_us < from ? from : c.start_us, b = c.end_us > to ? to : c.end_us;
            span_add(out, &n, &cap, a, b);
            continue;
        }
        rec_frag_t *fr;
        int nf = rec_pool_frags(p, cs[k].idx, &c, &fr);
        uint64_t a = 0, b = 0;
        for (int i = 0; i < nf; ++i) {
            uint64_t fs = c.start_us + (uint64_t)fr[i].t_ms * 1000, fe = fs + (uint64_t)fr[i].dur_ms * 1000;
            if (i && (fr[i].size & REC_FRAG_GAP)) {
                span_add(out, &n, &cap, a < from ? from : a, b > to ? to : b);
                a = fs;
            } else if (!i) {
                a = fs;
            }
            b = fe;
        }
        if (nf > 0) span_add(out, &n, &cap, a < from ? from : a, b > to ? to : b);
        free(fr);
    }
    free(cs);
    return n;
}

/* ---- events -------------------------------------------------------------------------- */

typedef struct {
    rec_pool_t  *p;
    rec_event_t *pend;
    uint32_t     npend;
} evsrc_t;

static int ev_get(evsrc_t *s, uint32_t seq, rec_event_t *e)
{
    for (uint32_t i = s->npend; i-- > 0;)
        if (s->pend[i].seq == seq) {
            *e = s->pend[i];
            return 0;
        }
    uint8_t b[REC_EVREC];
    off_t off = REC_EVLOG_HDR + (off_t)(seq % s->p->ev_cap) * REC_EVREC;
    if (s->p->events_fd < 0 || pread_all(s->p->events_fd, b, sizeof b, off) < 0 ||
        rec_event_decode(e, s->p->pool_id, b) < 0 || e->seq != seq)
        return -1;
    return 0;
}

int rec_q_events(rec_pool_t *p, int cam, int type, uint64_t from, uint64_t to, int newest_first, int limit,
                 rec_event_t **out)
{
    *out = NULL;
    evsrc_t s = { .p = p };
    pthread_mutex_lock(&p->mu);
    if (!p->mounted || !p->ev_cap) {
        pthread_mutex_unlock(&p->mu);
        return 0;
    }
    uint32_t head = p->ev_head, lo = head > p->ev_cap ? head - p->ev_cap : 0;
    s.npend = p->n_ev_pend;
    s.pend = s.npend ? malloc(s.npend * sizeof *s.pend) : NULL;
    if (s.pend) memcpy(s.pend, p->ev_pend, s.npend * sizeof *s.pend);
    else s.npend = 0;
    /* Events still being reported have their duration so far. */
    rec_active_t act[REC_CAMS][REC_EV_TYPES];
    memcpy(act, p->active, sizeof act);
    pthread_mutex_unlock(&p->mu);
    if (limit <= 0 || limit > 100000) limit = 100000;
    /* The log is in time order: the first at or after `from`, the first at or after `to`. */
    uint32_t a = lo, b = head;
    while (a < b) {
        uint32_t m = a + (b - a) / 2;
        rec_event_t e;
        if (ev_get(&s, m, &e) < 0 || e.time_us < from) a = m + 1; else b = m;
    }
    uint32_t first = a;
    a = first;
    b = head;
    while (a < b) {
        uint32_t m = a + (b - a) / 2;
        rec_event_t e;
        if (ev_get(&s, m, &e) == 0 && e.time_us >= to) b = m; else a = m + 1;
    }
    uint32_t end = a;
    int n = 0, cap = 0;
    for (uint32_t k = 0; k < end - first && n < limit; ++k) {
        uint32_t seq = newest_first ? end - 1 - k : first + k;
        rec_event_t e;
        if (ev_get(&s, seq, &e) < 0) continue;
        if ((cam && e.cam != cam) || (type && e.type != type)) continue;
        if (e.cam >= 1 && e.cam <= REC_CAMS && e.type < REC_EV_TYPES) {
            const rec_active_t *ac = &act[e.cam - 1][e.type];
            if (ac->on && ac->seq == e.seq) e.dur_ms = (uint32_t)((ac->last - ac->start) / 1000);
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            rec_event_t *r = realloc(*out, (size_t)cap * sizeof *r);
            if (!r) break;
            *out = r;
        }
        (*out)[n++] = e;
    }
    free(s.pend);
    return n;
}

/* ---- plans ------------------------------------------------------------------------------ */

void rec_plan_free(rec_plan_t *pl)
{
    free(pl->p);
    free(pl->init);
    memset(pl, 0, sizeof *pl);
}

static int piece_add(rec_plan_t *pl, int *cap, const rec_chunk_t *c, const rec_frag_t *f)
{
    if (pl->n == *cap) {
        *cap = *cap ? *cap * 2 : 256;
        rec_piece_t *r = realloc(pl->p, (size_t)*cap * sizeof *r);
        if (!r) return -1;
        pl->p = r;
    }
    rec_piece_t *x = &pl->p[pl->n++];
    x->chunk = c->no;
    x->use = c->use;
    x->off = f->off;
    x->size = f->size & REC_FRAG_SIZE_MASK;
    x->chunk_start_us = c->start_us;
    x->t_ms = f->t_ms;
    x->dur_ms = f->dur_ms;
    x->key = !!(f->size & REC_FRAG_KEY);
    x->gap = !!(f->size & REC_FRAG_GAP);
    pl->bytes += x->size;
    return 0;
}

static uint8_t *read_init(rec_pool_t *p, const rec_chunk_t *c)
{
    if (!c->init_len || c->init_len > (1u << 16)) return NULL;
    char path[300];
    rec_pool_chunk_path(p, c->no, path, sizeof path);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    uint8_t *b = malloc(c->init_len);
    if (b && pread_all(fd, b, c->init_len, REC_VGCH_SIZE) < 0) {
        free(b);
        b = NULL;
    }
    close(fd);
    return b;
}

/* A fragment's moof (malloc'ed, *len its size), parsed. */
static uint8_t *read_moof(int fd, uint32_t off, mp4_moof_t *m, uint32_t *len)
{
    uint8_t h[8];
    if (pread_all(fd, h, 8, off) < 0 || memcmp(h + 4, "moof", 4)) return NULL;
    uint32_t sz = rec_be32(h);
    if (sz < 8 || sz > MOOF_MAX) return NULL;
    uint8_t *b = malloc(sz);
    if (!b) return NULL;
    if (pread_all(fd, b, sz, off) < 0 || mp4_moof_parse(b, sz, m) < 0) {
        free(b);
        return NULL;
    }
    *len = sz;
    return b;
}

int rec_q_plan(rec_pool_t *p, int cam, int stream, uint64_t from, uint64_t to, int max, int cont, rec_plan_t *pl)
{
    memset(pl, 0, sizeof *pl);
    cref_t *cs;
    int nc = chunks_of(p, cam, stream, from, to, &cs), cap = 0, done = 0;
    for (int k = 0; k < nc && !done; ++k) {
        rec_chunk_t c;
        rec_frag_t *fr;
        int nf = rec_pool_frags(p, cs[k].idx, &c, &fr);
        if (nf <= 0) {
            free(fr);
            continue;
        }
        uint8_t *init = read_init(p, &c);
        if (!init) {
            free(fr);
            continue;
        }
        if (pl->init && !pl->n) {
            /* Nothing taken from the chunks before: this one's format counts. */
            free(pl->init);
            pl->init = NULL;
        }
        if (!pl->init) {
            pl->init = init;
            pl->init_len = c.init_len;
            pl->width = c.width;
            pl->height = c.height;
            if (mp4_init_codecs(init, c.init_len, pl->codecs, sizeof pl->codecs) < 0) pl->codecs[0] = 0;
        } else {
            int same = c.init_len == pl->init_len && !memcmp(init, pl->init, c.init_len);
            free(init);
            if (!same) {
                pl->format_change = 1;
                pl->next_us = c.start_us + (uint64_t)fr[0].t_ms * 1000;
                free(fr);
                break;
            }
        }
        for (int i = 0; i < nf; ++i) {
            uint64_t fs = c.start_us + (uint64_t)fr[i].t_ms * 1000, fe = fs + (uint64_t)fr[i].dur_ms * 1000;
            if (cont ? fs < from : fe <= from) continue;
            if (fs >= to) {
                done = 1;
                break;
            }
            if (max && pl->n >= max) {
                pl->next_us = fs;
                done = 1;
                break;
            }
            if (!pl->n && !cont && !(fr[i].size & REC_FRAG_KEY)) {
                /* Start at the keyframe before it. */
                int j = i;
                while (j > 0 && !(fr[j].size & REC_FRAG_KEY)) --j;
                for (; j < i; ++j) piece_add(pl, &cap, &c, &fr[j]);
            }
            if (piece_add(pl, &cap, &c, &fr[i]) < 0) {
                done = 1;
                break;
            }
        }
        free(fr);
    }
    free(cs);
    if (pl->n) {
        char path[300];
        rec_pool_chunk_path(p, pl->p[0].chunk, path, sizeof path);
        int fd = open(path, O_RDONLY);
        mp4_moof_t m;
        uint32_t len;
        uint8_t *mf = fd >= 0 ? read_moof(fd, pl->p[0].off, &m, &len) : NULL;
        pl->first_tick = (int64_t)(pl->p[0].chunk_start_us * 9 / 100) +
                         (mf ? (int64_t)m.tfdt : (int64_t)pl->p[0].t_ms * 90);
        free(mf);
        if (fd >= 0) close(fd);
    }
    return 0;
}

static int still_there(rec_pool_t *p, const rec_piece_t *x)
{
    pthread_mutex_lock(&p->mu);
    int ok = p->mounted && x->chunk < p->n && p->tab[x->chunk].use == x->use && p->tab[x->chunk].state != RC_FREE;
    pthread_mutex_unlock(&p->mu);
    return ok;
}

int rec_q_send(rec_pool_t *p, const rec_plan_t *pl, int64_t base_tick, int gaps, uint32_t seq0, rec_sink_fn fn,
               void *user)
{
    int fd = -1, rc = 0;
    uint32_t fd_chunk = REC_NONE;
    uint64_t run = 0;
    uint8_t *buf = malloc(COPY_BLOCK);
    if (!buf) return -1;
    for (int i = 0; i < pl->n && !rc; ++i) {
        const rec_piece_t *x = &pl->p[i];
        if (!still_there(p, x)) {
            rc = -1;
            break;
        }
        if (fd_chunk != x->chunk) {
            if (fd >= 0) close(fd);
            char path[300];
            rec_pool_chunk_path(p, x->chunk, path, sizeof path);
            fd = open(path, O_RDONLY);
            fd_chunk = x->chunk;
            if (fd < 0) {
                rc = -1;
                break;
            }
        }
        mp4_moof_t m;
        uint32_t msz;
        uint8_t *mf = read_moof(fd, x->off, &m, &msz);
        if (!mf || msz >= x->size) {
            free(mf);
            rc = -1;
            break;
        }
        int64_t abs = (int64_t)(x->chunk_start_us * 9 / 100) + (int64_t)m.tfdt;
        uint64_t t;
        if (gaps == REC_GAPS_REMOVE) {
            t = run;
            run += m.dur;
        } else {
            t = abs > base_tick ? (uint64_t)(abs - base_tick) : 0;
        }
        mp4_moof_patch(mf, &m, seq0 + (uint32_t)i, t);
        rc = fn(user, mf, msz);
        free(mf);
        for (uint32_t done = msz; !rc && done < x->size;) {
            uint32_t k = x->size - done < COPY_BLOCK ? x->size - done : COPY_BLOCK;
            if (pread_all(fd, buf, k, (off_t)x->off + done) < 0) {
                rc = -1;
                break;
            }
            rc = fn(user, buf, k);
            done += k;
        }
        /* The chunk may have been reused while it was read. */
        if (!rc && !still_there(p, x)) rc = -1;
    }
    if (fd >= 0) close(fd);
    free(buf);
    return rc;
}

int rec_q_export_init(const rec_plan_t *pl, uint64_t from, uint64_t to, int gaps, mp4_buf_t *out)
{
    if (!pl->n || !pl->init) return -1;
    uint64_t first_us = (uint64_t)pl->first_tick * 100 / 9;
    int64_t media = (int64_t)(from * 9 / 100) - pl->first_tick;
    if (media < 0) media = 0;
    const rec_piece_t *l = &pl->p[pl->n - 1];
    uint64_t last_end = l->chunk_start_us + ((uint64_t)l->t_ms + l->dur_ms) * 1000;
    uint64_t start = from > first_us ? from : first_us, end = to < last_end ? to : last_end, dur_us = 0;
    if (gaps == REC_GAPS_REMOVE) {
        uint64_t all = 0;
        for (int i = 0; i < pl->n; ++i) all += (uint64_t)pl->p[i].dur_ms * 1000;
        uint64_t cut = (start - first_us) + (last_end > end ? last_end - end : 0);
        dur_us = all > cut ? all - cut : 0;
    } else if (end > start) {
        dur_us = end - start;
    }
    return mp4_init_edit(pl->init, pl->init_len, media, dur_us / 1000, out);
}
