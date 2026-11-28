/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rec_store.c — the recording store's threads and queue (rec_store.h). */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "au_norm.h"
#include "cam_events.h"
#include "channel.h"
#include "events.h"
#include "log.h"
#include "rec_place.h"
#include "rec_store.h"

#define RETRY_SECS  10                  /* a target not there yet is looked at again */
#define CHECK_SECS  5                   /* a mounted target: is its disk still there? */

/* rec_au_from puts the AU's data right after `au`: au must stay last. */
typedef struct qitem {
    struct qitem *next;
    int           brk;                  /* a break, not an AU */
    rec_au_t      au;
} qitem_t;

enum { CMD_NONE, CMD_TAKEOVER, CMD_DELETE };

/* A stream as its session thread sees it: dropping to a keyframe, a gap to mark. */
typedef struct {
    int drop, gap;
} sess_t;

static struct {
    pthread_once_t  once;
    pthread_mutex_t qmu;                /* the queue, the settings, the hold, the commands */
    pthread_cond_t  qcond, done;
    qitem_t        *head, *tail;
    uint32_t        qbytes, qmax;
    uint64_t        dropped, in_bytes;
    sess_t          sess[REC_CAMS][REC_STREAMS];
    rec_store_cfg_t cfg;
    unsigned        cfg_gen, cfg_applied;
    char            hold[192];
    int             hold_on, hold_ack;
    int             cmd, cmd_cam, cmd_rc, cmd_seq, cmd_done;
    uint64_t        cmd_from, cmd_to;
    pthread_t       writer, bg;
    int             running;
    volatile int    stop;
    atomic_int      accept;             /* the writer takes AUs now */
    rec_pool_t      pool;
    char            place[24];          /* why it is not mounted (RP_WAITING) */
    time_t          retry_at, checked_at;
    int             told;
    rec_place_t     told_place;
    int             bg_kick;
    int             ev_sub;             /* cam_events subscription, -1 none */
} g = { .once = PTHREAD_ONCE_INIT, .ev_sub = -1 };

static void init_once(void)
{
    pthread_mutex_init(&g.qmu, NULL);
    pthread_cond_init(&g.qcond, NULL);
    pthread_cond_init(&g.done, NULL);
    rec_pool_init(&g.pool);
    g.qmax = REC_QUEUE_MIN;
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

static int under(const char *dir, const char *top)
{
    size_t n = strlen(top);
    while (n > 1 && top[n - 1] == '/') --n;
    return n && !strncmp(dir, top, n) && (dir[n] == 0 || dir[n] == '/');
}

/* ---- the session side ------------------------------------------------------- */

static int ps_slot(vg_codec_t c, int type)
{
    if (c == VG_CODEC_H264) return type == 7 ? 0 : type == 8 ? 1 : -1;
    return type >= 32 && type <= 34 ? type - 32 : -1;
}

rec_au_t *rec_au_from(int cam, int stream, const vg_au_t *au, uint64_t wall_us, size_t head)
{
    /* Length-prefixed NAL units, the parameter sets beside them, access
     * unit delimiters left out: one pass to size it, one to copy. */
    size_t pos = 0, nl, out = 0, ps = 0;
    const uint8_t *nal;
    int h265 = au->codec == VG_CODEC_H265;
    while ((nal = au_next_nal(au->data, au->len, &pos, &nl))) {
        if (!nl) continue;
        int type = h265 ? (nal[0] >> 1) & 0x3F : nal[0] & 0x1F;
        if (type == (h265 ? 35 : 9)) continue;
        if (ps_slot(au->codec, type) >= 0) ps += nl;
        else out += 4 + nl;
    }
    if (!out || out > 0xFFFFFFFFu) return NULL;
    uint8_t *mem = malloc(head + sizeof(rec_au_t) + out + ps);
    if (!mem) return NULL;
    memset(mem, 0, head + sizeof(rec_au_t));
    rec_au_t *a = (rec_au_t *)(mem + head);
    uint8_t *d = (uint8_t *)(a + 1), *pd = d + out;
    a->data = d;
    pos = 0;
    while ((nal = au_next_nal(au->data, au->len, &pos, &nl))) {
        if (!nl) continue;
        int type = h265 ? (nal[0] >> 1) & 0x3F : nal[0] & 0x1F;
        if (type == (h265 ? 35 : 9)) continue;
        int s = ps_slot(au->codec, type);
        if (s >= 0) {
            if (!a->ps_len[s] && nl <= 0xFFFF) {
                a->ps[s] = pd;
                a->ps_len[s] = (uint16_t)nl;
            }
            memcpy(pd, nal, nl);
            pd += nl;
            continue;
        }
        d[0] = (uint8_t)(nl >> 24);
        d[1] = (uint8_t)(nl >> 16);
        d[2] = (uint8_t)(nl >> 8);
        d[3] = (uint8_t)nl;
        memcpy(d + 4, nal, nl);
        d += 4 + nl;
    }
    int want = h265 ? 3 : 2, have = 0;
    for (int i = 0; i < want; ++i) have += a->ps_len[i] != 0;
    a->n_ps = have == want ? want : 0;
    a->cam = (uint8_t)cam;
    a->stream = (uint8_t)stream;
    a->key = (uint8_t)!!au->key;
    a->gap = (uint8_t)!!au->discont;
    a->codec = au->codec;
    a->pts_us = au->pts_us;
    a->wall_us = wall_us;
    a->len = (uint32_t)out;
    return a;
}

int rec_store_put(int cam, int stream, const vg_au_t *au)
{
    pthread_once(&g.once, init_once);
    if (cam < 0 || cam >= REC_CAMS || stream < 0 || stream >= REC_STREAMS) return 1;
    if (!atomic_load(&g.accept)) return 1;
    rec_au_t *a = rec_au_from(cam, stream, au, now_us(), offsetof(qitem_t, au));
    if (!a) return 0;
    qitem_t *it = (qitem_t *)((uint8_t *)a - offsetof(qitem_t, au));
    uint32_t out = a->len;

    pthread_mutex_lock(&g.qmu);
    sess_t *ss = &g.sess[cam][stream];
    g.in_bytes += out;
    if (ss->drop && !au->key) {
        pthread_mutex_unlock(&g.qmu);
        free(it);
        return -1;
    }
    if (g.qbytes + out > g.qmax) {
        if (!ss->drop) vg_log("rec: CAM%d: the disk does not keep up; dropping to the next keyframe", cam + 1);
        if (!ss->drop) events_push(cam + 1, "REC dropped");
        ss->drop = 1;
        ss->gap = 1;
        g.dropped++;
        pthread_mutex_unlock(&g.qmu);
        free(it);
        return -1;
    }
    ss->drop = 0;
    it->au.gap = (uint8_t)(ss->gap || au->discont);
    ss->gap = 0;
    if (g.tail) g.tail->next = it;
    else g.head = it;
    g.tail = it;
    g.qbytes += out;
    pthread_cond_signal(&g.qcond);
    pthread_mutex_unlock(&g.qmu);
    return 0;
}

void rec_store_break(int cam, int stream)
{
    pthread_once(&g.once, init_once);
    if (cam < 0 || cam >= REC_CAMS || stream < 0 || stream >= REC_STREAMS || !atomic_load(&g.accept)) return;
    qitem_t *it = calloc(1, sizeof *it);
    if (!it) return;
    it->brk = 1;
    it->au.cam = (uint8_t)cam;
    it->au.stream = (uint8_t)stream;
    pthread_mutex_lock(&g.qmu);
    if (g.tail) g.tail->next = it;
    else g.head = it;
    g.tail = it;
    pthread_cond_signal(&g.qcond);
    pthread_mutex_unlock(&g.qmu);
}

/* The cameras' events (cam_events.h), as they begin and end: into the
 * pool's event log. Quick: a record queued under the pool's lock. */
_Static_assert((int)REC_EV_OTHER == (int)CH_EV_TYPES && CH_EV_OTHER == 1u << (REC_EV_OTHER - 1),
               "REC_EV_* type b + 1 is CH_EV_* bit b");

static void on_cam_event(void *user, const cam_event_t *ev)
{
    (void)user;
    for (int b = 0; b < CH_EV_TYPES; ++b)
        if (ev->type & (1u << b)) rec_pool_event(&g.pool, ev->channel, b + 1, ev->active, now_us());
}

/* Events going on when the pool is mounted: they begin now in its log. */
static void events_late_start(void)
{
    time_t now = time(NULL);
    uint64_t us = now_us();
    for (int ch = 0; ch < REC_CAMS; ++ch) {
        unsigned bits = cam_events_held(ch, now);
        for (int b = 0; b < CH_EV_TYPES; ++b)
            if (bits & (1u << b)) rec_pool_event(&g.pool, ch, b + 1, 1, us);
    }
}

/* ---- the writer --------------------------------------------------------------- */

static void drain(qitem_t *it)
{
    while (it) {
        qitem_t *n = it->next;
        free(it);
        it = n;
    }
}

static void set_accept(void)
{
    pthread_mutex_lock(&g.pool.mu);
    int ok = g.pool.mounted && g.pool.state == RP_READY;
    pthread_mutex_unlock(&g.pool.mu);
    atomic_store(&g.accept, ok && !g.hold_on);
}

static void unmount(void)
{
    atomic_store(&g.accept, 0);
    if (g.pool.mounted) {
        rec_pool_unmount(&g.pool);
        vg_log("rec: %s closed", g.pool.dir);
    }
}

/* Is the target where a recording may go? Mount it when it is. */
static void try_mount(const rec_store_cfg_t *cfg, time_t now)
{
    char why[400];
    rec_place_t pl = rec_place_check(cfg->pool.path, cfg->allow_rootfs, NULL, why, sizeof why);
    if (pl != REC_PLACE_OK) {
        pthread_mutex_lock(&g.pool.mu);
        g.pool.state = RP_WAITING;
        snprintf(g.pool.why, sizeof g.pool.why, "%.159s", why);
        snprintf(g.pool.dir, sizeof g.pool.dir, "%s", cfg->pool.path);
        pthread_mutex_unlock(&g.pool.mu);
        snprintf(g.place, sizeof g.place, "%s", rec_place_name(pl));
        if (!g.told || g.told_place != pl) {
            vg_log("rec: not recording into %s: %s; trying again every %d s", cfg->pool.path, why, RETRY_SECS);
            events_push(0, "REC %s", rec_place_name(pl));
        }
        g.told = 1;
        g.told_place = pl;
        return;
    }
    g.place[0] = 0;
    if (rec_pool_mount(&g.pool, &cfg->pool, now) < 0) {
        if (!g.told || g.told_place != REC_PLACE_ERROR) {
            vg_log("rec: cannot record into %s: %s", cfg->pool.path, g.pool.why);
            events_push(0, "REC error");
        }
        g.told = 1;
        g.told_place = REC_PLACE_ERROR;
        return;
    }
    if (!g.told || g.told_place != REC_PLACE_OK) events_push(0, "REC on");
    g.told = 1;
    g.told_place = REC_PLACE_OK;
    g.checked_at = now;
    g.bg_kick = 1;
    events_late_start();
}

static void commands(void)
{
    if (!g.cmd || g.cmd_done) return;
    int cmd = g.cmd, cam = g.cmd_cam;
    uint64_t from = g.cmd_from, to = g.cmd_to;
    pthread_mutex_unlock(&g.qmu);
    int rc = -1;
    if (cmd == CMD_TAKEOVER) {
        rc = rec_pool_takeover(&g.pool, time(NULL));
        g.bg_kick = 1;
    } else if (cmd == CMD_DELETE) {
        rc = rec_pool_delete(&g.pool, cam, from, to);
    }
    pthread_mutex_lock(&g.qmu);
    g.cmd_rc = rc;
    g.cmd_done = 1;
    pthread_cond_broadcast(&g.done);
}

static void *writer_main(void *arg)
{
    (void)arg;
    rec_store_cfg_t cfg = { 0 };
    time_t rate_at = 0;
    uint64_t rate_in = 0;
    for (;;) {
        pthread_mutex_lock(&g.qmu);
        if (!g.head && !g.stop && g.cfg_gen == g.cfg_applied && !(g.hold_on && !g.hold_ack) &&
            !(g.cmd && !g.cmd_done)) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 200 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&g.qcond, &g.qmu, &ts);
        }
        qitem_t *list = g.head;
        g.head = g.tail = NULL;
        g.qbytes = 0;
        int stop = g.stop, newcfg = g.cfg_gen != g.cfg_applied;
        rec_store_cfg_t want = g.cfg;
        g.cfg_applied = g.cfg_gen;
        int hold = g.hold_on;
        char hold_dir[192];
        snprintf(hold_dir, sizeof hold_dir, "%s", g.hold);
        uint64_t in = g.in_bytes;
        pthread_mutex_unlock(&g.qmu);

        time_t now = time(NULL);
        if (newcfg) {
            if (strcmp(want.pool.path, cfg.pool.path) || want.allow_rootfs != cfg.allow_rootfs) {
                unmount();
                g.told = 0;
                g.retry_at = 0;
                if (!want.pool.path[0]) {
                    pthread_mutex_lock(&g.pool.mu);
                    g.pool.state = RP_OFF;
                    g.pool.why[0] = 0;
                    pthread_mutex_unlock(&g.pool.mu);
                }
            } else if (g.pool.mounted) {
                rec_pool_update(&g.pool, &want.pool);
            }
            cfg = want;
        }
        if (hold && g.pool.mounted && under(g.pool.dir, hold_dir)) {
            drain(list);
            list = NULL;
            unmount();
        }
        if (hold) {
            pthread_mutex_lock(&g.qmu);
            if (!g.hold_ack) {
                g.hold_ack = 1;
                pthread_cond_broadcast(&g.done);
            }
            pthread_mutex_unlock(&g.qmu);
        }
        int held = hold && under(cfg.pool.path, hold_dir);
        if (stop) {
            drain(list);
            unmount();
            break;
        }
        if (!g.pool.mounted && cfg.pool.path[0] && !held && now >= g.retry_at) {
            g.retry_at = now + RETRY_SECS;
            try_mount(&cfg, now);
        }
        if (g.pool.mounted && now - g.checked_at >= CHECK_SECS) {
            /* A disk pulled out leaves its mount point on the root
             * filesystem: stop before anything goes there. */
            g.checked_at = now;
            char why[400];
            rec_place_t pl = rec_place_check(cfg.pool.path, cfg.allow_rootfs, NULL, why, sizeof why);
            if (pl != REC_PLACE_OK) {
                vg_log("rec: recording stopped: %s", why);
                events_push(0, "REC %s", rec_place_name(pl));
                atomic_store(&g.accept, 0);
                rec_pool_abandon(&g.pool);
                g.retry_at = now + RETRY_SECS;
                g.told = 1;
                g.told_place = pl;
                snprintf(g.place, sizeof g.place, "%s", rec_place_name(pl));
                pthread_mutex_lock(&g.pool.mu);
                g.pool.state = RP_WAITING;
                snprintf(g.pool.why, sizeof g.pool.why, "%.159s", why);
                pthread_mutex_unlock(&g.pool.mu);
            }
        }
        set_accept();
        for (qitem_t *it = list; it;) {
            qitem_t *n = it->next;
            if (g.pool.mounted) {
                if (it->brk) rec_pool_break(&g.pool, it->au.cam, it->au.stream);
                else rec_pool_put(&g.pool, &it->au);
            }
            free(it);
            it = n;
        }
        if (g.pool.mounted) rec_pool_tick(&g.pool, now, now_us());
        pthread_mutex_lock(&g.qmu);
        commands();
        if (now - rate_at >= 10) {
            /* ~10 s of what comes in, within bounds */
            if (rate_at) {
                uint64_t q = (in - rate_in) / (uint64_t)(now - rate_at) * 10;
                g.qmax = q < REC_QUEUE_MIN ? REC_QUEUE_MIN : q > REC_QUEUE_MAX ? REC_QUEUE_MAX : (uint32_t)q;
            }
            rate_at = now;
            rate_in = in;
        }
        pthread_mutex_unlock(&g.qmu);
    }
    return NULL;
}

/* Rebuild and pre-allocation, out of the writer's way. */
static void *bg_main(void *arg)
{
    (void)arg;
    while (!g.stop) {
        if (rec_pool_rebuild_due(&g.pool)) rec_pool_rebuild(&g.pool, &g.stop);
        else if (rec_pool_prealloc_due(&g.pool)) rec_pool_prealloc(&g.pool, &g.stop);
        for (int i = 0; i < 10 && !g.stop && !g.bg_kick; ++i) {
            struct timespec ts = { 0, 100 * 1000000L };
            nanosleep(&ts, NULL);
        }
        g.bg_kick = 0;
    }
    return NULL;
}

void rec_store_configure(const rec_store_cfg_t *cfg)
{
    pthread_once(&g.once, init_once);
    pthread_mutex_lock(&g.qmu);
    g.cfg = *cfg;
    g.cfg_gen++;
    int start = !g.running && cfg->pool.path[0];
    if (start) {
        g.stop = 0;
        g.running = 1;
        if (g.ev_sub < 0) g.ev_sub = cam_events_subscribe(on_cam_event, NULL);
    }
    pthread_cond_signal(&g.qcond);
    pthread_mutex_unlock(&g.qmu);
    if (!start) return;
    if (pthread_create(&g.writer, NULL, writer_main, NULL) != 0) {
        vg_log("rec: cannot start the writer thread");
        g.running = 0;
        return;
    }
    if (pthread_create(&g.bg, NULL, bg_main, NULL) != 0) vg_log("rec: cannot start the background thread");
}

void rec_store_shutdown(void)
{
    pthread_once(&g.once, init_once);
    pthread_mutex_lock(&g.qmu);
    int running = g.running;
    g.stop = 1;
    pthread_cond_broadcast(&g.qcond);
    pthread_mutex_unlock(&g.qmu);
    if (!running) return;
    if (g.ev_sub >= 0) cam_events_unsubscribe(g.ev_sub);
    g.ev_sub = -1;
    pthread_join(g.bg, NULL);
    pthread_join(g.writer, NULL);
    g.running = 0;
    atomic_store(&g.accept, 0);
}

int rec_store_hold(const char *dir, int timeout_ms)
{
    pthread_once(&g.once, init_once);
    pthread_mutex_lock(&g.qmu);
    snprintf(g.hold, sizeof g.hold, "%s", dir);
    g.hold_on = 1;
    g.hold_ack = 0;
    atomic_store(&g.accept, 0);
    pthread_cond_signal(&g.qcond);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    while (g.running && !g.hold_ack)
        if (pthread_cond_timedwait(&g.done, &g.qmu, &ts) == ETIMEDOUT) break;
    int still = g.running && !g.hold_ack;
    pthread_mutex_unlock(&g.qmu);
    return still;
}

void rec_store_release(void)
{
    pthread_once(&g.once, init_once);
    pthread_mutex_lock(&g.qmu);
    g.hold_on = 0;
    g.hold[0] = 0;
    g.retry_at = 0;
    pthread_cond_signal(&g.qcond);
    pthread_mutex_unlock(&g.qmu);
}

static int command(int cmd, int cam, uint64_t from, uint64_t to)
{
    pthread_once(&g.once, init_once);
    pthread_mutex_lock(&g.qmu);
    while (g.cmd && !g.cmd_done && g.running) pthread_cond_wait(&g.done, &g.qmu);
    if (!g.running) {
        pthread_mutex_unlock(&g.qmu);
        return -1;
    }
    g.cmd = cmd;
    g.cmd_cam = cam;
    g.cmd_from = from;
    g.cmd_to = to;
    g.cmd_done = 0;
    pthread_cond_signal(&g.qcond);
    while (!g.cmd_done && g.running) pthread_cond_wait(&g.done, &g.qmu);
    int rc = g.cmd_done ? g.cmd_rc : -1;
    g.cmd = CMD_NONE;
    pthread_cond_broadcast(&g.done);
    pthread_mutex_unlock(&g.qmu);
    return rc;
}

int rec_store_takeover(void) { return command(CMD_TAKEOVER, 0, 0, 0); }

int rec_store_delete(int cam, uint64_t from_us, uint64_t to_us) { return command(CMD_DELETE, cam, from_us, to_us); }

rec_pool_t *rec_store_pool(void)
{
    pthread_once(&g.once, init_once);
    return &g.pool;
}

/* ---- status ------------------------------------------------------------------- */

void rec_store_status(rec_store_status_t *st)
{
    pthread_once(&g.once, init_once);
    memset(st, 0, sizeof *st);
    pthread_mutex_lock(&g.qmu);
    st->queue_bytes = g.qbytes;
    st->queue_max = g.qmax;
    st->dropped = g.dropped;
    snprintf(st->place, sizeof st->place, "%s", g.place);
    snprintf(st->path, sizeof st->path, "%s", g.cfg.pool.path);
    pthread_mutex_unlock(&g.qmu);
    rec_pool_t *p = &g.pool;
    pthread_mutex_lock(&p->mu);
    st->state = st->path[0] ? p->state : RP_OFF;
    snprintf(st->why, sizeof st->why, "%s", p->why);
    if (p->mounted) {
        st->pool_id = p->pool_id;
        st->chunk_mb = p->chunk_bytes >> 20;
        st->chunks = p->n;
        st->want = p->want;
        st->rebuilding = p->rebuilding;
        st->rebuild_done = p->rebuild_done;
        st->rate = p->rate;
        st->flush_s = p->flush_s;
        snprintf(st->media, sizeof st->media, "%s", p->media);
        st->size_bytes = (uint64_t)p->n * p->chunk_bytes;
        for (uint32_t i = 0; i < p->n; ++i) {
            const rec_chunk_t *c = &p->tab[i];
            switch (c->state) {
            case RC_FREE: st->free++; break;
            case RC_OPEN: st->open++; break;
            case RC_CLOSED: st->closed++; break;
            default: st->bad++; break;
            }
            if ((c->state != RC_OPEN && c->state != RC_CLOSED) || !c->nfrag) continue;
            st->used_bytes += c->bytes;
            if (!st->oldest_us || c->start_us < st->oldest_us) st->oldest_us = c->start_us;
            if (c->end_us > st->newest_us) st->newest_us = c->end_us;
            if (c->cam < 1 || c->cam > REC_CAMS) continue;
            rec_store_cam_t *k = &st->cam[c->cam - 1];
            k->chunks++;
            k->bytes += c->bytes;
            if (!k->oldest_us || c->start_us < k->oldest_us) k->oldest_us = c->start_us;
            if (c->end_us > k->newest_us) k->newest_us = c->end_us;
        }
    }
    pthread_mutex_unlock(&p->mu);
}

void rec_store_stream(int cam, int stream, rec_stream_status_t *s)
{
    pthread_once(&g.once, init_once);
    memset(s, 0, sizeof *s);
    if (cam < 0 || cam >= REC_CAMS || stream < 0 || stream >= REC_STREAMS) return;
    rec_pool_t *p = &g.pool;
    s->on = atomic_load(&g.accept);
    pthread_mutex_lock(&p->mu);
    const rec_wstream_t *w = &p->ws[cam][stream];
    if (p->mounted) {
        s->open = w->open;
        s->writing = w->open && time(NULL) - w->last_write < 3;
        s->failed = w->failed;
        s->full = w->full;
        s->bytes = w->bytes_total;
    }
    pthread_mutex_unlock(&p->mu);
    pthread_mutex_lock(&g.qmu);
    s->dropping = g.sess[cam][stream].drop;
    pthread_mutex_unlock(&g.qmu);
}
