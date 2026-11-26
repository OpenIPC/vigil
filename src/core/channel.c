/* SPDX-License-Identifier: GPL-3.0-or-later */
/* channel.c — camera channels (see channel.h).
 *
 * A channel has up to two sessions with its camera, each a thread with its
 * own source and au_norm:
 *
 *   main   the camera's `url`, always: the connection the status and events
 *          follow, the recording, and the picture when the tile is large
 *   sub    the sub-stream (`sub_url`, or derived from a NETIP main URL),
 *          only while the tile is small (multi-window): its picture there
 *
 * The channel's one decoder (hal_decoder.h) belongs to one session at a
 * time; the one the tile wants (`view`) takes it over at its first
 * keyframe, and the other keeps the picture up until then. A view change
 * never touches the main session, so recording goes on unbroken.
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "au_norm.h"
#include "channel.h"
#include "events.h"
#include "hal.h"
#include "hal_decoder.h"
#include "hal_display.h"
#include "log.h"
#include "rec_place.h"
#include "schema.h"
#include "seg_writer.h"
#include "source.h"

#define NO_DATA_SECS       5        /* reconnect after this long without video */
#define LIVE_SECS          3        /* "live" while video arrived this recently */
#define RECONNECT_MAX_SECS 10
#define DEC_RETRY_SECS     10       /* retry a failed decoder setup */
#define REC_RETRY_SECS     10       /* retry a recording directory that could not be set up */
#define PATH_MAX_WHY       400      /* a reason, with a path in it, for the log */
/* A sub-stream no longer shown keeps its connection this long: layouts
 * switched back and forth do not reconnect. */
#define SUB_LINGER_SECS    5

enum { S_MAIN, S_SUB, S_N };
#define VIEW_NONE (-1)

typedef struct channel channel_t;

typedef struct {
    channel_t      *c;
    int             id;             /* S_MAIN or S_SUB */
    volatile int    stop;           /* its source watches it too */
    source_t       *src;
    au_norm_t       norm;
    pthread_t       thread;
    int             thread_up;      /* under c->lock (the sub's is read by the status) */
    int             done;           /* the thread is ending (under c->lock) */
    time_t          unwanted_since; /* sub: the tile stopped wanting it (under c->lock) */
    int             dec_ok;         /* feeding the decoder since the last keyframe */
    int             was_viewed;
    unsigned        frames;
    time_t          last_video;     /* last access unit from the source */
    /* Status, under c->lock. */
    int             live;
    unsigned        fps, width, height;
    vg_codec_t      codec;
} sess_t;

struct channel {
    int             idx;
    int             active;         /* set up and running (status readers: under lock) */
    /* What it was started with, to tell whether a new configuration
     * changes it. */
    char            url[512], sub_url[512], rec_dir[192];
    int             seg_secs, keep, allow_rootfs;
    int             self_displayed; /* the platform shows its source (local capture) */

    sess_t          s[S_N];
    seg_writer_t    seg;            /* the main session's */
    int             rec_on, rec_fail_told;
    time_t          rec_retry_at;   /* not set up then: try again later */
    time_t          rec_checked_at; /* the directory was last looked at then (rec_check) */
    rec_place_t     rec_place;      /* the last answer, to say each change once */
    int             rec_told;

    /* The session whose picture the tile wants: S_MAIN, S_SUB or
     * VIEW_NONE (off screen where decoders are scarce). Set by the thread
     * that calls channels_set_tiles. */
    atomic_int      view;
    /* The decoder, under *dec_lock: which session's stream it was opened
     * for (VIEW_NONE: none), and for what. */
    pthread_mutex_t *dec_lock;      /* g_dec_locks[idx] */
    atomic_int      dec_holder;
    vg_codec_t      dec_codec;
    unsigned        dec_gen;        /* the holder's au_norm info_gen */
    time_t          dec_failed_at;
    unsigned        dec_failed_frees; /* g_dec_frees when it failed */
    int             dec_told;       /* the failure was reported */

    /* Status (read by the UI and the API under `lock`). */
    pthread_mutex_t *lock;          /* g_locks[idx]: outlives every restart */
    channel_status_t st;
};

static channel_t       g_ch[CHANNEL_MAX];
static pthread_mutex_t g_locks[CHANNEL_MAX], g_dec_locks[CHANNEL_MAX];
static pthread_once_t  g_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_n_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_n;         /* tiles in use */
static int             g_can_decode;
static int             g_running;   /* past the first apply: changes are news */
static vg_rect_t       g_tiles[CHANNEL_MAX];
static int             g_have_tiles;
static int             g_shown_only;   /* hal_caps()->dec_shown_only with a display */
static unsigned long long g_screen_area; /* 0: unknown, the main stream everywhere */
/* Decoders closed so far: a channel that could not get one (memory, the
 * decoder's channels) tries again as soon as another lets its go. */
static atomic_uint     g_dec_frees;

static void locks_init(void)
{
    for (int i = 0; i < CHANNEL_MAX; ++i) {
        pthread_mutex_init(&g_locks[i], NULL);
        pthread_mutex_init(&g_dec_locks[i], NULL);
    }
}

/* The main session's state: what the status says about the camera. */
static void set_state(channel_t *c, int live, const char *fmt, ...)
{
    pthread_mutex_lock(c->lock);
    c->s[S_MAIN].live = live;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->st.state, sizeof c->st.state, fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(c->lock);
}

static const char *view_name(int v) { return v == S_SUB ? "sub" : v == S_MAIN ? "main" : ""; }

void channels_status(int idx, channel_status_t *st)
{
    memset(st, 0, sizeof *st);
    if (idx < 0 || idx >= CHANNEL_MAX) return;
    pthread_once(&g_once, locks_init);
    channel_t *c = &g_ch[idx];
    pthread_mutex_lock(&g_locks[idx]);
    if (c->active) {
        *st = c->st;
        /* The stream on screen: the decoder's, else the one the tile waits
         * for once it is up, else the main one. */
        int holder = atomic_load(&c->dec_holder), view = atomic_load(&c->view);
        int shown = holder != VIEW_NONE ? holder
                  : view == S_SUB && c->s[S_SUB].thread_up && c->s[S_SUB].live ? S_SUB : S_MAIN;
        const sess_t *v = &c->s[shown];
        st->live = c->s[S_MAIN].live;
        st->decoding = holder != VIEW_NONE;
        st->codec = v->codec ? v->codec : c->s[S_MAIN].codec;
        st->width = v->width;
        st->height = v->height;
        st->fps = v->fps;
        snprintf(st->view, sizeof st->view, "%s", view_name(holder != VIEW_NONE ? holder : view));
        st->recording = c->rec_on && c->seg.f != NULL;
        st->rec_bytes = c->seg.bytes;
        /* Set up: say how it goes. Not set up: the reason, kept by rec_setup. */
        if (c->rec_on)
            snprintf(st->rec_state, sizeof st->rec_state, "%s",
                     c->rec_fail_told ? "failed" : st->recording ? "recording" : "ready");
    }
    pthread_mutex_unlock(&g_locks[idx]);
}

int channels_count(void)
{
    pthread_mutex_lock(&g_n_lock);
    int n = g_n;
    pthread_mutex_unlock(&g_n_lock);
    return n;
}

/* ---- decoding (under c->dec_lock) ------------------------------------------- */

static void decoder_close(channel_t *c)
{
    if (atomic_load(&c->dec_holder) == VIEW_NONE) return;
    hal_dec_close(c->idx);
    atomic_store(&c->dec_holder, VIEW_NONE);
    atomic_fetch_add(&g_dec_frees, 1);
}

/* At session s's keyframe, s being the view: make sure the decoder is open
 * for its stream, taking it from the other session if that one has it.
 * Returns 1 if the AU can go to the decoder. */
static int decoder_ensure(channel_t *c, sess_t *s, vg_codec_t codec)
{
    int mine = atomic_load(&c->dec_holder) == s->id;
    if (!s->norm.info_gen) return mine;                 /* no SPS seen yet */
    if (mine && c->dec_codec == codec && c->dec_gen == s->norm.info_gen) return 1;
    if (c->dec_failed_at && time(NULL) - c->dec_failed_at < DEC_RETRY_SECS &&
        atomic_load(&g_dec_frees) == c->dec_failed_frees)
        return 0;

    decoder_close(c);
    const vg_stream_info_t *si = &s->norm.info;
    const char *tag = s->id == S_SUB ? " sub" : "";
    /* Before the attempt: a decoder freed while it runs counts as news. */
    unsigned frees = atomic_load(&g_dec_frees);
    if (hal_dec_open(c->idx, codec, si) < 0) {
        /* Said once until it works: with more cameras than decoders it
         * fails again at every retry. */
        if (!c->dec_told) {
            vg_log("CAM%d%s: no decoder for %s %ux%u", c->idx + 1, tag, vg_codec_name(codec),
                   si->width, si->height);
            events_push(c->idx + 1, "no decoder");
            c->dec_told = 1;
        }
        c->dec_failed_at = time(NULL);
        c->dec_failed_frees = frees;
        return 0;
    }
    vg_log("CAM%d%s: decoder %s %ux%u, %u refs", c->idx + 1, tag, vg_codec_name(codec), si->width,
           si->height, si->refs);
    atomic_store(&c->dec_holder, s->id);
    c->dec_codec = codec;
    c->dec_gen = s->norm.info_gen;
    c->dec_failed_at = 0;
    c->dec_told = 0;
    return 1;
}

/* One AU of session s, for the decoder. The session the tile wants opens
 * the decoder at its keyframe; the other one keeps feeding a decoder it
 * holds until then, so the picture stays up through a switch; off screen
 * (VIEW_NONE) the holder gives it back. */
static void dec_feed(sess_t *s, const vg_au_t *au)
{
    channel_t *c = s->c;
    int view = atomic_load(&c->view), viewed = view == s->id;
    if (!viewed && atomic_load(&c->dec_holder) != s->id) {
        s->dec_ok = 0;
        s->was_viewed = 0;
        return;
    }
    pthread_mutex_lock(c->dec_lock);
    if (viewed && !s->was_viewed) c->dec_failed_at = 0;  /* back on screen: try at once */
    s->was_viewed = viewed;
    if (view == VIEW_NONE) {
        decoder_close(c);
        s->dec_ok = 0;
    } else {
        if (au->key)
            s->dec_ok = viewed ? decoder_ensure(c, s, au->codec)
                               : c->dec_codec == au->codec && c->dec_gen == s->norm.info_gen;
        /* A dropped AU breaks the reference chain: wait for the next
         * keyframe instead of feeding pictures that cannot decode. */
        if (s->dec_ok && atomic_load(&c->dec_holder) == s->id && hal_dec_send(c->idx, au) < 0)
            s->dec_ok = 0;
    }
    pthread_mutex_unlock(c->dec_lock);
}

/* Close the open segment (caller holds c->lock); report a lost tail. */
static void rec_close(channel_t *c)
{
    if (seg_close(&c->seg) < 0) {
        vg_log("CAM%d: closing a recording failed; its last data is lost", c->idx + 1);
        events_push(c->idx + 1, "REC failed");
    }
}

/* ---- recording (the main session) ----------------------------------------- */

/* Set up recording into c->rec_dir, if it may go there (rec_place.h): never
 * into a directory that would land on the root filesystem or in RAM, so a
 * disk that is not mounted (yet, or any more) leaves recording off and
 * reported, not written to the flash. Called when the channel starts and
 * again every REC_RETRY_SECS at a keyframe while recording is off, so it
 * starts by itself once the disk is there. Takes c->lock. */
static void rec_setup(channel_t *c, vg_codec_t codec)
{
    char why[PATH_MAX_WHY], prefix[16];
    int ch = c->idx + 1;
    rec_place_t p = rec_place_check(c->rec_dir, c->allow_rootfs, NULL, why, sizeof why);
    snprintf(prefix, sizeof prefix, "CAM%d", ch);
    pthread_mutex_lock(c->lock);
    if (p == REC_PLACE_OK) {
        if (seg_init(&c->seg, c->rec_dir, prefix, c->seg_secs, c->keep) == 0) {
            if (codec != VG_CODEC_NONE) seg_set_ext(&c->seg, codec == VG_CODEC_H264 ? "h264" : "h265");
            c->rec_on = 1;
            c->rec_fail_told = 0;
            c->st.rec_state[0] = 0;         /* set up: channels_status says how it goes */
        } else {
            snprintf(why, sizeof why, "cannot create %s: %s", c->rec_dir, strerror(errno));
            p = REC_PLACE_ERROR;
        }
    }
    if (p != REC_PLACE_OK) snprintf(c->st.rec_state, sizeof c->st.rec_state, "%s", rec_place_name(p));
    pthread_mutex_unlock(c->lock);
    if (c->rec_told && p == c->rec_place) return;
    c->rec_place = p;
    c->rec_told = 1;
    if (p == REC_PLACE_OK) {
        vg_log("CAM%d: recording into %s, on %s", ch, c->rec_dir, why);
        events_push(ch, "REC on");
    } else {
        vg_log("CAM%d: not recording into %s: %s; trying again every %d s", ch, c->rec_dir, why,
               REC_RETRY_SECS);
        events_push(ch, "REC %s", rec_place_name(p));
    }
}

/* Recording is off: try again at a keyframe every REC_RETRY_SECS. */
static void rec_retry(channel_t *c, vg_codec_t codec)
{
    time_t now = time(NULL);
    if (now - c->rec_retry_at < REC_RETRY_SECS) return;
    c->rec_retry_at = now;
    rec_setup(c, codec);
}

/* While recording: is the directory still where it may record? Asked
 * before every new segment and after a failed write, at most once a second
 * (each is a stat and a read of mountinfo). A disk that was pulled or
 * unmounted leaves the mount point behind on the root filesystem; a new
 * segment opened there would go onto the flash. So recording goes off and
 * waits for the disk as at the start, rather than trying the files again. */
static void rec_check(channel_t *c)
{
    time_t now = time(NULL);
    if (now == c->rec_checked_at) return;
    c->rec_checked_at = now;
    char why[PATH_MAX_WHY];
    rec_place_t p = rec_place_check(c->rec_dir, c->allow_rootfs, NULL, why, sizeof why);
    if (p == REC_PLACE_OK) return;
    pthread_mutex_lock(c->lock);
    seg_close(&c->seg);                 /* its tail is lost with the disk */
    c->rec_on = 0;
    snprintf(c->st.rec_state, sizeof c->st.rec_state, "%s", rec_place_name(p));
    pthread_mutex_unlock(c->lock);
    c->rec_retry_at = now;
    c->rec_place = p;
    vg_log("CAM%d: recording stopped: %s; trying again every %d s", c->idx + 1, why, REC_RETRY_SECS);
    events_push(c->idx + 1, "REC %s", rec_place_name(p));
}


/* ---- per-session thread --------------------------------------------------- */

static void on_au(void *user, const vg_au_t *au)
{
    sess_t *s = user;
    channel_t *c = s->c;

    if (g_can_decode && !s->src->ops->self_displayed) dec_feed(s, au);
    if (au->key && s->norm.info_gen) {
        pthread_mutex_lock(c->lock);
        s->width = s->norm.info.width;
        s->height = s->norm.info.height;
        pthread_mutex_unlock(c->lock);
    }
    ++s->frames;
    if (s->id != S_MAIN) return;            /* recording is the main stream's */

    if (!c->rec_on && c->rec_dir[0] && au->key) rec_retry(c, au->codec);
    /* A keyframe that will open a new segment (seg_write's own test). */
    if (c->rec_on && au->key && (!c->seg.f || time(NULL) - c->seg.seg_start >= c->seg.seg_secs))
        rec_check(c);
    if (c->rec_on) {
        pthread_mutex_lock(c->lock);
        int was_open = c->seg.f != NULL;
        int rc = seg_write(&c->seg, au->data, au->len, au->key);
        int is_open = c->seg.f != NULL;
        pthread_mutex_unlock(c->lock);
        if (rc < 0 && !c->rec_fail_told) {
            vg_log("CAM%d: recording failed", c->idx + 1);
            events_push(c->idx + 1, "REC failed");
            c->rec_fail_told = 1;
        } else if (rc == 0 && is_open && !was_open) {
            c->rec_fail_told = 0;
        }
        if (rc < 0) rec_check(c);
    }
}

/* Raw AU from the source: (re)start normalization on a codec change. */
static void on_raw_au(void *user, const vg_au_t *au)
{
    sess_t *s = user;
    channel_t *c = s->c;
    if (au->codec == VG_CODEC_NONE) return;
    /* Only video counts as "the camera is alive": RTCP, audio or info
     * packets must not hold off the no-data reconnect. */
    s->last_video = time(NULL);
    if (au->codec != s->norm.codec) {
        au_norm_free(&s->norm);
        au_norm_init(&s->norm, au->codec);
        pthread_mutex_lock(c->lock);
        if (s->id == S_MAIN) {
            rec_close(c);
            seg_set_ext(&c->seg, au->codec == VG_CODEC_H264 ? "h264" : "h265");
        }
        s->codec = au->codec;
        pthread_mutex_unlock(c->lock);
    }
    au_norm_push(&s->norm, au, on_au, s);
}

static void sleep_interruptible(volatile int *stop, int secs)
{
    for (int i = 0; i < secs * 10 && !*stop; ++i) usleep(100 * 1000);
}

/* Whether session s should go on: the main one always; the sub-stream
 * while the tile wants it and SUB_LINGER_SECS after. Deciding to end, it
 * says so (done) under the lock views_update looks at it with, so a view
 * that comes back at that moment starts a new session. */
static int sess_wanted(sess_t *s)
{
    if (s->id == S_MAIN) return 1;
    channel_t *c = s->c;
    time_t now = time(NULL);
    pthread_mutex_lock(c->lock);
    int want = 1;
    if (atomic_load(&c->view) == S_SUB) {
        s->unwanted_since = 0;
    } else {
        if (!s->unwanted_since) s->unwanted_since = now;
        want = now - s->unwanted_since < SUB_LINGER_SECS;
        if (!want) s->done = 1;
    }
    pthread_mutex_unlock(c->lock);
    return want;
}

static void *sess_thread(void *arg)
{
    sess_t *s = arg;
    channel_t *c = s->c;
    int ch = c->idx + 1, backoff = 1, told_fail = 0, main = s->id == S_MAIN;
    const char *tag = main ? "" : " sub";

    while (!s->stop && sess_wanted(s)) {
        if (main) set_state(c, 0, "CONNECTING");
        if (s->src->ops->open(s->src) < 0) {
            if (main) set_state(c, 0, "%.40s", s->src->err);
            if (!told_fail) {
                vg_log("CAM%d%s %s: %s", ch, tag, s->src->host, s->src->err);
                if (main) events_push(ch, "down");
            }
            told_fail = 1;
            sleep_interruptible(&s->stop, backoff);
            if (backoff < RECONNECT_MAX_SECS) backoff *= 2;
            continue;
        }
        backoff = 1;
        told_fail = 0;

        /* A new stream: same codec keeps the cache but waits for a keyframe. */
        if (s->src->codec != VG_CODEC_NONE && s->src->codec != s->norm.codec) {
            au_norm_free(&s->norm);
            au_norm_init(&s->norm, s->src->codec);
        } else {
            au_norm_reset(&s->norm);
        }
        for (int i = 0; i < s->src->ps_n; ++i) au_norm_add_param_set(&s->norm, s->src->ps[i], s->src->ps_len[i]);
        if (s->norm.codec != VG_CODEC_NONE) {
            pthread_mutex_lock(c->lock);
            if (main) seg_set_ext(&c->seg, s->norm.codec == VG_CODEC_H264 ? "h264" : "h265");
            s->codec = s->norm.codec;
            pthread_mutex_unlock(c->lock);
        }
        pthread_mutex_lock(c->dec_lock);
        if (atomic_load(&c->dec_holder) == s->id) hal_dec_reset(c->idx);
        if (atomic_load(&c->view) == s->id) c->dec_failed_at = 0;
        pthread_mutex_unlock(c->dec_lock);
        s->dec_ok = 0;

        vg_log("CAM%d%s %s: online", ch, tag, s->src->host);
        if (main) {
            events_push(ch, "online");
            set_state(c, 1, "");
        } else {
            pthread_mutex_lock(c->lock);
            s->live = 1;
            pthread_mutex_unlock(c->lock);
        }

        time_t sec = time(NULL);
        s->last_video = sec;
        while (!s->stop && sess_wanted(s)) {
            int rc = s->src->ops->poll(s->src, on_raw_au, s);
            time_t now = time(NULL);
            if (rc < 0) {
                vg_log("CAM%d%s %s: %s", ch, tag, s->src->host, s->src->err);
                if (main) events_push(ch, "offline");
                break;
            }
            if (now - s->last_video >= NO_DATA_SECS) {
                vg_log("CAM%d%s %s: no data for %d s", ch, tag, s->src->host, NO_DATA_SECS);
                if (main) events_push(ch, "offline");
                break;
            }
            /* Rate over 2 s: cameras deliver in bursts over TCP, so a
             * one-second sample reads 0 or half the rate now and then. */
            if (now - sec >= 2) {
                pthread_mutex_lock(c->lock);
                s->fps = (s->frames + (unsigned)(now - sec) / 2) / (unsigned)(now - sec);
                s->frames = 0;
                pthread_mutex_unlock(c->lock);
                sec = now;
            }
            /* Live = video within the last LIVE_SECS, not "fps > 0". */
            int live = now - s->last_video < LIVE_SECS;
            if (live != s->live) {
                pthread_mutex_lock(c->lock);
                s->live = live;
                pthread_mutex_unlock(c->lock);
            }
        }
        s->src->ops->close(s->src);
        pthread_mutex_lock(c->lock);
        if (main) rec_close(c);
        s->fps = 0;
        s->live = 0;
        pthread_mutex_unlock(c->lock);
        if (!s->stop && sess_wanted(s)) sleep_interruptible(&s->stop, 1);
    }
    /* A decoder this stream still holds goes with it. */
    pthread_mutex_lock(c->dec_lock);
    if (atomic_load(&c->dec_holder) == s->id) decoder_close(c);
    pthread_mutex_unlock(c->dec_lock);
    pthread_mutex_lock(c->lock);
    s->done = 1;
    pthread_mutex_unlock(c->lock);
    return NULL;
}

/* ---- sessions (from the thread that calls channels_apply) --------------- */

static int sess_start(channel_t *c, int id, const char *url)
{
    sess_t *s = &c->s[id];
    s->src = source_create(url, c->idx);
    if (!s->src) return -1;
    s->c = c;
    s->id = id;
    s->stop = 0;
    s->src->stop = &s->stop;
    s->dec_ok = s->was_viewed = 0;
    s->frames = 0;
    au_norm_init(&s->norm, VG_CODEC_NONE);
    pthread_mutex_lock(c->lock);
    s->done = 0;
    s->unwanted_since = 0;
    s->live = 0;
    s->fps = s->width = s->height = 0;
    s->codec = VG_CODEC_NONE;
    pthread_mutex_unlock(c->lock);
    if (pthread_create(&s->thread, NULL, sess_thread, s) != 0) {
        source_destroy(s->src);
        s->src = NULL;
        au_norm_free(&s->norm);
        return -1;
    }
    pthread_mutex_lock(c->lock);
    s->thread_up = 1;
    pthread_mutex_unlock(c->lock);
    return 0;
}

/* Wait for a session told to stop (or ending by itself) and free it. */
static void sess_reap(channel_t *c, int id)
{
    sess_t *s = &c->s[id];
    if (!s->thread_up) return;
    pthread_join(s->thread, NULL);
    pthread_mutex_lock(c->lock);
    s->thread_up = 0;
    s->live = 0;
    s->fps = 0;
    pthread_mutex_unlock(c->lock);
    source_destroy(s->src);
    s->src = NULL;
    au_norm_free(&s->norm);
}

/* The session whose picture tile i wants: the sub-stream in a tile of a
 * quarter of the screen or less, if the camera has one; the main stream
 * in a larger one; none off screen where decoders are scarce. */
static int view_for(const channel_t *c, int i)
{
    if (!g_can_decode || c->self_displayed) return VIEW_NONE;
    if (!g_have_tiles) return S_MAIN;
    const vg_rect_t *t = &g_tiles[i];
    if (t->w <= 0 || t->h <= 0) return g_shown_only ? VIEW_NONE : S_MAIN;
    if (c->sub_url[0] && g_screen_area && (unsigned long long)t->w * (unsigned)t->h * 4 <= g_screen_area)
        return S_SUB;
    return S_MAIN;
}

/* Set every channel's view and start the sub-streams now wanted; reap
 * the ones that ended (a sub-stream no longer wanted ends by itself after
 * SUB_LINGER_SECS). */
static void views_update(void)
{
    for (int i = 0; i < CHANNEL_MAX; ++i) {
        channel_t *c = &g_ch[i];
        if (!c->active) continue;
        int v = view_for(c, i);
        atomic_store(&c->view, v);
        sess_t *sub = &c->s[S_SUB];
        pthread_mutex_lock(c->lock);
        int ended = sub->thread_up && (sub->done || sub->stop);
        int running = sub->thread_up && !ended;
        pthread_mutex_unlock(c->lock);
        if (ended) sess_reap(c, S_SUB);
        if (v == S_SUB && !running && sess_start(c, S_SUB, c->sub_url) < 0)
            vg_log("CAM%d: cannot start its sub-stream", i + 1);
    }
}

/* ---- public ---------------------------------------------------------------- */

static const char *str_or_empty(const char *s) { return s ? s : ""; }

/* The sub-stream of camera i in `cfg`: its sub_url, or one derived from
 * the main URL (source.h); "" for none. */
static void sub_url_of(const channels_config_t *cfg, int i, char *out, size_t cap)
{
    out[0] = 0;
    if (cfg->sub_urls[i] && cfg->sub_urls[i][0]) snprintf(out, cap, "%s", cfg->sub_urls[i]);
    else if (cfg->urls[i] && source_sub_url(cfg->urls[i], out, cap) < 0) out[0] = 0;
}

/* Does channel i run what `cfg` asks for? (The name and the sub-stream do
 * not count: they change without a restart.) */
static int slot_matches(const channel_t *c, const channels_config_t *cfg, int i)
{
    const char *url = i < cfg->n ? cfg->urls[i] : NULL;
    if (!c->active) return url == NULL;
    return url && !strcmp(c->url, url) && !strcmp(c->rec_dir, str_or_empty(cfg->rec_dir)) &&
           c->seg_secs == cfg->seg_secs && c->keep == cfg->keep && c->allow_rootfs == cfg->allow_rootfs;
}

static void slot_stop(channel_t *c)
{
    for (int i = 0; i < S_N; ++i) c->s[i].stop = 1;
}

/* Tear down a channel whose sessions have been told to stop. */
static void slot_teardown(channel_t *c)
{
    for (int i = 0; i < S_N; ++i) sess_reap(c, i);
    pthread_mutex_lock(c->lock);
    c->active = 0;
    pthread_mutex_unlock(c->lock);
    pthread_mutex_lock(c->dec_lock);
    decoder_close(c);
    pthread_mutex_unlock(c->dec_lock);
    if (!c->self_displayed && hal_caps()->display) hal_display_clear_tile(c->idx);
    pthread_mutex_lock(c->lock);
    rec_close(c);
    pthread_mutex_unlock(c->lock);
}

static int slot_start(int i, const channels_config_t *cfg)
{
    channel_t *c = &g_ch[i];
    const char *url = cfg->urls[i];
    const char *name = cfg->names[i] ? cfg->names[i] : "";
    pthread_mutex_lock(&g_locks[i]);
    memset(c, 0, sizeof *c);
    c->idx = i;
    c->lock = &g_locks[i];
    c->dec_lock = &g_dec_locks[i];
    atomic_store(&c->dec_holder, VIEW_NONE);
    atomic_store(&c->view, VIEW_NONE);
    pthread_mutex_unlock(&g_locks[i]);

    source_t *probe = source_create(url, i);
    if (!probe) {
        char red[512];
        schema_redact_url(url, red, sizeof red);
        vg_log("CAM%d: cannot use '%s' (schemes: %s)", i + 1, red, source_schemes());
        events_push(i + 1, "bad URL");
        return -1;
    }
    c->self_displayed = probe->ops->self_displayed;
    snprintf(c->st.host, sizeof c->st.host, "%.63s", probe->host);
    source_destroy(probe);

    snprintf(c->url, sizeof c->url, "%s", url);
    if (!c->self_displayed) sub_url_of(cfg, i, c->sub_url, sizeof c->sub_url);
    snprintf(c->rec_dir, sizeof c->rec_dir, "%s", str_or_empty(cfg->rec_dir));
    c->seg_secs = cfg->seg_secs;
    c->keep = cfg->keep;
    c->allow_rootfs = cfg->allow_rootfs;
    c->st.configured = 1;
    snprintf(c->st.name, sizeof c->st.name, "%s", name);
    if (cfg->rec_dir) {
        rec_setup(c, VG_CODEC_NONE);
        if (!c->rec_on) c->rec_retry_at = time(NULL);
    }
    if (g_have_tiles && hal_caps()->display && !c->self_displayed && g_tiles[i].w > 0)
        hal_display_set_tile(i, &g_tiles[i]);
    atomic_store(&c->view, view_for(c, i));
    if (sess_start(c, S_MAIN, url) < 0) {
        vg_log("CAM%d: cannot start its thread", i + 1);
        slot_teardown(c);
        return -1;
    }
    pthread_mutex_lock(c->lock);
    c->active = 1;
    pthread_mutex_unlock(c->lock);
    return 0;
}

int channels_apply(const channels_config_t *cfg)
{
    const hal_caps_t *caps = hal_caps();
    int limit = CHANNEL_MAX;
    pthread_once(&g_once, locks_init);
    if (caps->max_channels && caps->max_channels < limit) limit = caps->max_channels;
    if (cfg->n > limit) {
        int used = 0;
        for (int i = limit; i < cfg->n; ++i) used |= cfg->urls[i] != NULL;
        if (used) vg_log("%s shows at most %d channels; using the first %d", caps->name, limit, limit);
    }

    /* Stop every channel that changes first, all at once: each stop may
     * wait for a source's timeout, and they should overlap. */
    int change[CHANNEL_MAX] = { 0 }, was[CHANNEL_MAX] = { 0 };
    for (int i = 0; i < CHANNEL_MAX; ++i) {
        channel_t *c = &g_ch[i];
        if (i < limit && slot_matches(c, cfg, i)) {
            if (c->active) {
                const char *name = cfg->names[i] ? cfg->names[i] : "";
                pthread_mutex_lock(c->lock);
                snprintf(c->st.name, sizeof c->st.name, "%s", name);
                pthread_mutex_unlock(c->lock);
                /* A new sub-stream: only that session starts again. */
                char sub[sizeof c->sub_url];
                if (!c->self_displayed) sub_url_of(cfg, i, sub, sizeof sub);
                else sub[0] = 0;
                if (strcmp(sub, c->sub_url)) {
                    c->s[S_SUB].stop = 1;
                    sess_reap(c, S_SUB);
                    snprintf(c->sub_url, sizeof c->sub_url, "%s", sub);
                }
            }
            continue;
        }
        change[i] = 1;
        was[i] = c->active;
        if (c->active) slot_stop(c);
    }
    for (int i = 0; i < CHANNEL_MAX; ++i) {
        if (!change[i] || !was[i]) continue;
        slot_teardown(&g_ch[i]);
        if (i >= cfg->n || !cfg->urls[i]) events_push(i + 1, "removed");
    }

    int failed = 0, n = 0;
    for (int i = 0; i < limit && i < cfg->n; ++i) {
        if (change[i] && cfg->urls[i]) {
            if (slot_start(i, cfg) < 0) ++failed;
            else if (g_running) events_push(i + 1, was[i] ? "restarted" : "added");
        }
        if (cfg->urls[i]) n = i + 1;
    }
    views_update();
    pthread_mutex_lock(&g_n_lock);
    g_n = n;
    pthread_mutex_unlock(&g_n_lock);
    g_running = 1;
    return failed;
}

int channels_start(const channels_config_t *cfg, const vg_rect_t *tiles)
{
    g_can_decode = hal_caps()->decoder;
    g_shown_only = hal_caps()->dec_shown_only && hal_caps()->display;
    g_running = 0;
    g_have_tiles = tiles != NULL;
    if (tiles) memcpy(g_tiles, tiles, sizeof g_tiles);
    if (channels_apply(cfg) > 0) {
        channels_stop();
        return -1;
    }
    return 0;
}

void channels_set_screen(unsigned w, unsigned h)
{
    g_screen_area = (unsigned long long)w * h;
}

void channels_set_tiles(const vg_rect_t *tiles)
{
    if (!hal_caps()->display) return;
    memcpy(g_tiles, tiles, sizeof g_tiles);
    g_have_tiles = 1;
    for (int i = 0; i < CHANNEL_MAX; ++i) {
        channel_t *c = &g_ch[i];
        /* Only this thread starts and stops channels: active and
         * self_displayed are stable here. */
        if (!c->active || c->self_displayed) continue;
        if (g_tiles[i].w > 0 && g_tiles[i].h > 0) {
            if (hal_display_set_tile(i, &g_tiles[i]) < 0)
                vg_log("CAM%d: cannot move its picture to %dx%d at %d,%d", i + 1, g_tiles[i].w,
                       g_tiles[i].h, g_tiles[i].x, g_tiles[i].y);
        } else
            hal_display_clear_tile(i);
    }
    views_update();
}

void channels_stop(void)
{
    for (int i = 0; i < CHANNEL_MAX; ++i)
        if (g_ch[i].active) slot_stop(&g_ch[i]);
    for (int i = 0; i < CHANNEL_MAX; ++i)
        if (g_ch[i].active) slot_teardown(&g_ch[i]);
    pthread_mutex_lock(&g_n_lock);
    g_n = 0;
    pthread_mutex_unlock(&g_n_lock);
    g_running = 0;
}
