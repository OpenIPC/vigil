/* SPDX-License-Identifier: GPL-3.0-or-later */
/* channel.c — camera channels (see channel.h).
 *
 * A channel has up to two sessions with its camera, each a thread with its
 * own source and au_norm, both connected for as long as the channel runs,
 * whatever the view:
 *
 *   main   the camera's `url`: the connection the events follow, the
 *          recording, and the picture when the tile is large
 *   sub    the sub-stream (`sub_url`, or derived from a NETIP main URL):
 *          the picture while the tile is small (multi-window)
 *
 * The channel's one decoder (hal_decoder.h) belongs to one session at a
 * time. Each session keeps the GOP it is in (gop_cache.h: the sub-stream
 * always, the main stream while the tile is on screen), so the one the
 * tile wants (`view`) takes the decoder over at its next AU, not its next
 * keyframe: the decoder opens at the cached keyframe and is fed the GOP so
 * far, then the live stream. Without a GOP (dropped, too big, just
 * connected) it waits for the keyframe, and the other session keeps the
 * picture up until then; a channel coming on screen with no picture to
 * keep shows its sub-stream's GOP meanwhile. A view change never touches
 * the main session, so recording goes on unbroken. Each session keeps its
 * own state; the channel's status is that of the one whose picture is on
 * screen.
 *
 * Each session also hands its AUs to the taps open on its stream
 * (stream_tap.h: vigild's RTSP and NETIP servers), with its GOP so far
 * for a tap that has just opened; a tap keeps its stream's GOP too.
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

#include "aspect.h"
#include "au_norm.h"
#include "cam_events.h"
#include "channel.h"
#include "events.h"
#include "gop_cache.h"
#include "hal.h"
#include "hal_decoder.h"
#include "hal_display.h"
#include "log.h"
#include "rec_place.h"
#include "rec_sched.h"
#include "schema.h"
#include "seg_writer.h"
#include "source.h"
#include "stream_tap.h"

#define NO_DATA_SECS       5        /* reconnect after this long without video */
#define LIVE_SECS          3        /* "live" while video arrived this recently */
#define RECONNECT_MAX_SECS 10
#define DEC_RETRY_SECS     10       /* retry a failed decoder setup */
#define REC_RETRY_SECS     10       /* retry a recording directory that could not be set up */
#define PATH_MAX_WHY       400      /* a reason, with a path in it, for the log */
/* The most a session's GOP cache may hold: a sub-stream's GOP is far
 * smaller, a 4 MP main stream's at 4 Mbit/s and 4 s about 2 MiB. All of
 * them together: GOP_CACHE_BUDGET. */
#define GOP_SUB_MAX        (1u << 20)
#define GOP_MAIN_MAX       (4u << 20)
/* A sub-stream waits this long for its main stream's first SPS before its
 * decoder opens at its own shape (aspect_of_camera). */
#define MAIN_SPS_WAIT_SECS 3

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
    int             dec_ok;         /* feeding the decoder since the last keyframe */
    /* The GOP so far, the session thread's own; whether it holds one is
     * also read by the other session (gop_ok). */
    gop_cache_t     gop;
    atomic_int      gop_ok;
    int             gop_told;       /* a dropped GOP was logged */
    int             was_viewed;
    unsigned        frames;
    unsigned        frames_total;   /* video AUs on this connection, for the log */
    int             title_taken;    /* the source's title went to the channel */
    time_t          last_video;     /* last access unit from the source */
    time_t          info_at;        /* sub: its first SPS came then */
    /* Status, under c->lock. */
    ch_state_t      status;
    char            state[48];
    int             live;
    unsigned        fps, width, height;
    vg_codec_t      codec;
    uint64_t        bytes;          /* video received since the last rate (its thread's) */
    unsigned        kbps;           /* ... over 2 s, under c->lock */
} sess_t;

struct channel {
    int             idx;
    int             active;         /* set up and running (status readers: under lock) */
    /* What it was started with, to tell whether a new configuration
     * changes it. */
    char            url[512], sub_url[512], rec_dir[192];
    int             seg_secs, keep, allow_rootfs, stop_when_full;
    uint64_t        quota_bytes;
    time_t          quota_age;
    int             self_displayed; /* the platform shows its source (local capture) */
    char            title[32];      /* the camera's name for the channel (under lock) */
    /* Where its events come from (channels_config_t.events, onvif_urls):
     * the main session's NETIP connection (its source's `events`). */
    ch_events_mode_t ev_mode;
    char            onvif_url[160];

    sess_t          s[S_N];
    seg_writer_t    seg;            /* the main session's */
    int             rec_on, rec_fail_told;
    time_t          rec_retry_at;   /* not set up then: try again later */
    time_t          rec_checked_at; /* the directory was last looked at then (rec_check) */
    rec_place_t     rec_place;      /* the last answer, to say each change once */
    int             rec_told;
    /* Set up, but not writing now and why: "disk full", "schedule off",
     * "waiting for motion", "paused" (status, under lock); "" writing. */
    char            rec_why[24];
    time_t          rec_why_at;     /* the schedule and the hold were last looked at */
    int             full_told;

    /* The session whose picture the tile wants: S_MAIN, S_SUB or
     * VIEW_NONE (off screen where decoders are scarce). Set by the thread
     * that calls channels_set_tiles. */
    atomic_int      view;
    /* The tile is on the screen: only then is the main stream's GOP kept
     * (only such a tile can be double-clicked). Set with `view`. */
    atomic_int      on_screen;
    /* The decoder, under *dec_lock: which session's stream it was opened
     * for (VIEW_NONE: none), and for what. */
    pthread_mutex_t *dec_lock;      /* g_dec_locks[idx] */
    atomic_int      dec_holder;
    vg_codec_t      dec_codec;
    unsigned        dec_gen;        /* the holder's au_norm info_gen */
    vg_stream_info_t dec_info;      /* the stream it was opened for */
    /* The main stream's SPS, for the picture's shape (aspect_of_camera):
     * copied by the main session's thread, its only writer, when it
     * changes. */
    vg_stream_info_t main_info;
    int             main_known;
    vg_ratio_t      aspect;         /* what the platform was told ... */
    vg_aspect_t     aspect_how;
    int             aspect_told;    /* ... if anything yet */
    time_t          dec_failed_at;
    unsigned        dec_failed_frees; /* g_dec_frees when it failed */
    atomic_int      dec_told;       /* the failure was reported (status: no_decoder) */

    /* Status (read by the UI and the API under `lock`). */
    pthread_mutex_t *lock;          /* g_locks[idx]: outlives every restart */
    channel_status_t st;
};

static channel_t       g_ch[CHANNEL_MAX];
static pthread_mutex_t g_locks[CHANNEL_MAX], g_dec_locks[CHANNEL_MAX];
static pthread_once_t  g_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_n_lock = PTHREAD_MUTEX_INITIALIZER;
/* Per slot, under g_locks: switched off (with its name). Kept whether a
 * channel runs or not, as its events are (cam_events.c). */
static char            g_disabled[CHANNEL_MAX];
static char            g_off_name[CHANNEL_MAX][32];
static int             g_n;         /* tiles in use */
static int             g_can_decode;
static int             g_running;   /* past the first apply: changes are news */
static vg_rect_t       g_tiles[CHANNEL_MAX];
static int             g_have_tiles;
static int             g_shown_only;   /* hal_caps()->dec_shown_only with a display */
static unsigned long long g_screen_area; /* 0: unknown, the main stream everywhere */
static atomic_int      g_aspect;    /* display.video_aspect: a vg_aspect_t */
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

const char *ch_state_name(ch_state_t s)
{
    static const char *const names[] = { "empty", "disabled", "connecting", "live", "no_video",
                                         "offline", "auth", "codec", "error" };
    return (unsigned)s < sizeof names / sizeof names[0] ? names[s] : "error";
}

/* What a failed source means for the tile. */
static ch_state_t fail_state(src_fail_t f)
{
    switch (f) {
    case SRC_FAIL_UNREACHABLE: return CH_OFFLINE;
    case SRC_FAIL_AUTH:        return CH_AUTH;
    case SRC_FAIL_CODEC:       return CH_CODEC;
    default:                   return CH_ERROR;
    }
}

/* A session's state. */
static void set_state(sess_t *s, int live, ch_state_t status, const char *fmt, ...)
{
    pthread_mutex_lock(s->c->lock);
    s->live = live;
    s->status = status;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->state, sizeof s->state, fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(s->c->lock);
}

/* A session's state as the status says it: connected without video for
 * LIVE_SECS is "no video". */
static ch_state_t sess_status(const sess_t *s)
{
    return s->status == CH_LIVE && !s->live ? CH_NO_VIDEO : s->status;
}

static const char *view_name(int v) { return v == S_SUB ? "sub" : v == S_MAIN ? "main" : ""; }

void channels_status(int idx, channel_status_t *st)
{
    memset(st, 0, sizeof *st);
    if (idx < 0 || idx >= CHANNEL_MAX) return;
    pthread_once(&g_once, locks_init);
    channel_t *c = &g_ch[idx];
    pthread_mutex_lock(&g_locks[idx]);
    if (!c->active) {
        if (g_disabled[idx]) {
            st->status = CH_DISABLED;
            snprintf(st->name, sizeof st->name, "%s", g_off_name[idx]);
        }
    } else {
        *st = c->st;
        /* The stream on screen: the decoder's, else the one the tile waits
         * for, else the main one. The main stream failing while a small
         * tile shows the sub-stream is not "no video" there: that is the
         * main state's to say (and the recording's). */
        int holder = atomic_load(&c->dec_holder), view = atomic_load(&c->view);
        const sess_t *sub = &c->s[S_SUB];
        int sub_up = sub->thread_up && !sub->done;
        int shown = holder != VIEW_NONE ? holder : view == S_SUB && sub_up ? S_SUB : S_MAIN;
        const sess_t *v = &c->s[shown];
        st->status = sess_status(v);
        st->live = v->live;
        snprintf(st->state, sizeof st->state, "%s", v->state);
        st->main_status = sess_status(&c->s[S_MAIN]);
        snprintf(st->main_state, sizeof st->main_state, "%s", c->s[S_MAIN].state);
        st->sub_status = sub_up ? sess_status(sub) : CH_EMPTY;
        snprintf(st->sub_state, sizeof st->sub_state, "%s", sub_up ? sub->state : "");
        snprintf(st->title, sizeof st->title, "%s", c->title);
        if (!st->name[0]) snprintf(st->name, sizeof st->name, "%s", c->title);
        st->events = cam_events_held(idx, time(NULL));
        st->decoding = holder != VIEW_NONE;
        st->no_decoder = !st->decoding && view != VIEW_NONE && atomic_load(&c->dec_told);
        st->codec = v->codec ? v->codec : c->s[S_MAIN].codec;
        st->width = v->width;
        st->height = v->height;
        st->fps = v->fps;
        st->main_kbps = c->s[S_MAIN].kbps;
        st->sub_kbps = sub_up ? sub->kbps : 0;
        for (int i = 0; i < S_N; ++i) {
            const sess_t *si = &c->s[i];
            if (i == S_SUB && !sub_up) continue;
            st->streams[i].codec = si->codec;
            st->streams[i].width = si->width;
            st->streams[i].height = si->height;
            st->streams[i].fps = si->fps;
        }
        snprintf(st->view, sizeof st->view, "%s", view_name(holder != VIEW_NONE ? holder : view));
        st->recording = c->rec_on && c->seg.f != NULL;
        st->rec_bytes = c->seg.bytes;
        /* Set up: say how it goes. Not set up: the reason, kept by rec_setup. */
        if (c->rec_on)
            snprintf(st->rec_state, sizeof st->rec_state, "%s",
                     c->rec_why[0] ? c->rec_why
                     : c->rec_fail_told ? "failed" : st->recording ? "recording" : "ready");
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

/* Tell the platform the shape of the picture for `si` (`id` the session
 * whose stream it is) and how it fills the tile, if that changed: the
 * camera's display aspect ratio (0:0 when stretched over the tile). */
static void aspect_tell(channel_t *c, const vg_stream_info_t *si, int id)
{
    if (!hal_caps()->display || c->self_displayed) return;
    vg_aspect_t how = (vg_aspect_t)atomic_load(&g_aspect);
    vg_ratio_t a = { 0, 0 };
    if (how != VG_ASPECT_STRETCH)
        a = aspect_of_camera(si, id == S_SUB, c->main_known ? &c->main_info : NULL);
    if (c->aspect_told && a.w == c->aspect.w && a.h == c->aspect.h && how == c->aspect_how) return;
    /* With what it came from: a wrong shape on a board is a misread SPS
     * (size, VUI) or a camera that says something odd, and only this tells
     * which. */
    char main[64] = "main unknown";
    if (c->main_known)
        snprintf(main, sizeof main, "main %ux%u sar %u:%u", c->main_info.width, c->main_info.height,
                 c->main_info.sar_w, c->main_info.sar_h);
    if (a.w)
        vg_log("CAM%d: picture %u:%u%s (%s, shown %s %ux%u sar %u:%u)", c->idx + 1, a.w, a.h,
               how == VG_ASPECT_FILL ? " filled" : "", main, id == S_SUB ? "sub" : "main", si->width, si->height, si->sar_w, si->sar_h);
    else
        vg_log("CAM%d: picture stretched over the tile (%s, shown %s %ux%u)", c->idx + 1, main,
               id == S_SUB ? "sub" : "main", si->width, si->height);
    hal_display_set_aspect(c->idx, a.w, a.h, how);
    c->aspect = a;
    c->aspect_how = how;
    c->aspect_told = 1;
}

/* The decoder's picture, as it is now: again after the main stream's SPS
 * or the setting changed. */
static void aspect_update(channel_t *c)
{
    int holder = atomic_load(&c->dec_holder);
    if (holder != VIEW_NONE) aspect_tell(c, &c->dec_info, holder);
}

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
    /* A sub-stream is shown at its main stream's shape, which the main
     * stream's SPS tells: at the start give it a moment, rather than show
     * the (often anamorphic) picture at its own shape first and make the
     * decoder again for the right one a second later. */
    if (s->id == S_SUB && !c->main_known && atomic_load(&g_aspect) != VG_ASPECT_STRETCH) {
        time_t now = time(NULL);
        if (!s->info_at) s->info_at = now;
        if (now - s->info_at < MAIN_SPS_WAIT_SECS) return 0;
    }
    if (c->dec_failed_at && time(NULL) - c->dec_failed_at < DEC_RETRY_SECS &&
        atomic_load(&g_dec_frees) == c->dec_failed_frees)
        return 0;

    const vg_stream_info_t *si = &s->norm.info;
    const char *tag = s->id == S_SUB ? " sub" : "";
    /* Before the attempt: a decoder freed while it runs counts as news. */
    unsigned frees = atomic_load(&g_dec_frees);
    /* The shape first: the platform sizes the decoder's output for it. */
    aspect_tell(c, si, s->id);
    /* hal_dec_open replaces a decoder the channel has (the other session's,
     * or this one's for an older stream) without a close first: the
     * platform may keep its picture up until the new one has one. */
    if (hal_dec_open(c->idx, codec, si) < 0) {
        decoder_close(c);                   /* whatever the platform left */
        /* Said once until it works: with more cameras than decoders it
         * fails again at every retry. */
        if (!atomic_load(&c->dec_told)) {
            vg_log("CAM%d%s: no decoder for %s %ux%u", c->idx + 1, tag, vg_codec_name(codec),
                   si->width, si->height);
            events_push(c->idx + 1, "no decoder");
            atomic_store(&c->dec_told, 1);
        }
        c->dec_failed_at = time(NULL);
        c->dec_failed_frees = frees;
        return 0;
    }
    vg_log("CAM%d%s: decoder %s %ux%u, %u refs", c->idx + 1, tag, vg_codec_name(codec), si->width,
           si->height, si->refs);
    atomic_store(&c->dec_holder, s->id);
    c->dec_info = *si;
    c->dec_codec = codec;
    c->dec_gen = s->norm.info_gen;
    c->dec_failed_at = 0;
    atomic_store(&c->dec_told, 0);
    return 1;
}

static long ms_since(const struct timespec *t)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t->tv_sec) * 1000L + (now.tv_nsec - t->tv_nsec) / 1000000L;
}

/* Session s has just been given the decoder in the middle of a GOP: feed
 * it the GOP so far, the current AU being its last. Returns 0, or -1 if
 * the platform dropped one (then the next keyframe, as after any drop).
 * The platform decodes the pictures as fast as it can, so the picture
 * runs fast up to the present for a moment. It takes a GOP in a few ms
 * where its queue has room; one that makes each picture wait for room
 * (Rockchip: up to 100 ms) is given up after REPLAY_MS, under dec_lock
 * as it runs, and at a stop. */
#define REPLAY_MS 500
static int dec_replay(channel_t *c, sess_t *s)
{
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (unsigned i = 0; i < s->gop.n; ++i) {
        vg_au_t au;
        gop_cache_get(&s->gop, i, &au);
        int slow = i && ms_since(&t0) > REPLAY_MS;
        if (s->stop || slow || hal_dec_send(c->idx, &au) < 0) {
            if (!s->stop)
                vg_log("CAM%d%s: the GOP so far was not taken (%u of %u pictures%s); waiting for a keyframe",
                       c->idx + 1, s->id == S_SUB ? " sub" : "", i, s->gop.n, slow ? " in time" : "");
            return -1;
        }
    }
    vg_log("CAM%d%s: decoding from the GOP so far: %u pictures, %zu KiB in %ld ms", c->idx + 1,
           s->id == S_SUB ? " sub" : "", s->gop.n, s->gop.len >> 10, ms_since(&t0));
    return 0;
}

/* One AU of session s, for the decoder. The session the tile wants takes
 * the decoder at once from its GOP so far, else at its keyframe; the other
 * one keeps feeding a decoder it holds until then, so the picture stays up
 * through a switch; off screen (VIEW_NONE) the holder gives it back. A
 * channel with no picture at all whose wanted stream has no GOP to start
 * from shows its sub-stream's meanwhile (back on screen with a full-size
 * tile, the main stream's GOP was not kept). */
static void dec_feed(sess_t *s, const vg_au_t *au)
{
    channel_t *c = s->c;
    /* The main stream's size is the camera's shape, also while the
     * sub-stream is on screen. */
    if (s->id == S_MAIN && s->norm.info_gen &&
        (!c->main_known || memcmp(&c->main_info, &s->norm.info, sizeof c->main_info))) {
        pthread_mutex_lock(c->dec_lock);
        c->main_info = s->norm.info;
        c->main_known = 1;
        aspect_update(c);
        pthread_mutex_unlock(c->dec_lock);
    }
    int view = atomic_load(&c->view), viewed = view == s->id;
    int gop = gop_cache_ready(&s->gop) && s->gop.gen == s->norm.info_gen;
    int stand_in = !viewed && view != VIEW_NONE && s->id == S_SUB && gop &&
                   !atomic_load(&c->s[view].gop_ok);
    if (!viewed && !stand_in && atomic_load(&c->dec_holder) != s->id) {
        s->dec_ok = 0;
        s->was_viewed = 0;
        return;
    }
    pthread_mutex_lock(c->dec_lock);
    int holder = atomic_load(&c->dec_holder);
    stand_in = stand_in && holder == VIEW_NONE;
    if (viewed && !s->was_viewed) c->dec_failed_at = 0;  /* back on screen: try at once */
    s->was_viewed = viewed;
    if (view == VIEW_NONE) {
        decoder_close(c);
        s->dec_ok = 0;
    } else if ((viewed || stand_in) && holder != s->id && !au->key && gop) {
        /* The GOP holds this AU too. */
        s->dec_ok = decoder_ensure(c, s, s->gop.codec) && dec_replay(c, s) == 0;
    } else {
        if (au->key)
            s->dec_ok = viewed || stand_in ? decoder_ensure(c, s, au->codec)
                                           : c->dec_codec == au->codec && c->dec_gen == s->norm.info_gen;
        /* A dropped AU breaks the reference chain: wait for the next
         * keyframe instead of feeding pictures that cannot decode. */
        if (s->dec_ok && atomic_load(&c->dec_holder) == s->id && hal_dec_send(c->idx, au) < 0)
            s->dec_ok = 0;
    }
    pthread_mutex_unlock(c->dec_lock);
}

/* Keep AU au in session s's GOP, if its GOP is kept: for the decoder
 * (`dec`), the sub-stream's always and the main stream's while the tile
 * is on screen; for a server's taps (stream_tap.h), while one is open, or
 * while one stands by and the cache has room to spare. Returns whether
 * the GOP holds au now, as its last AU. */
static int gop_keep(sess_t *s, const vg_au_t *au, int dec)
{
    channel_t *c = s->c;
    int need = dec && (s->id == S_SUB || atomic_load(&c->on_screen));
    int tap = stream_tap_keep(c->idx, s->id == S_SUB);
    /* Spare room: a GOP only a server might want waits while the caches
     * hold half their budget, so it never takes what a decoder needs. */
    if (!need && tap == TAP_KEEP_SPARE && au->key && gop_cache_used() > gop_cache_budget() / 2) tap = 0;
    if (!need && !tap) {
        if (s->gop.cap) gop_cache_free(&s->gop);
        atomic_store(&s->gop_ok, 0);
        return 0;
    }
    int whole = gop_cache_ready(&s->gop);          /* the last GOP, until this keyframe */
    int rc = gop_cache_add(&s->gop, au, s->norm.info_gen);
    atomic_store(&s->gop_ok, gop_cache_ready(&s->gop));
    if (rc == GOP_KEPT || rc == GOP_IDLE) {
        if (au->key && whole) s->gop_told = 0;     /* said again once one fitted */
        return rc == GOP_KEPT;
    }
    if (s->gop_told || (!need && tap != TAP_KEEP_MUST)) return 0;
    s->gop_told = 1;
    if (rc == GOP_OVER_CAP)
        vg_log("CAM%d%s: its GOP is over %zu KiB, not kept: a switch to it waits for a keyframe",
               c->idx + 1, s->id == S_SUB ? " sub" : "", s->gop.limit >> 10);
    else
        vg_log("CAM%d%s: the %zu KiB for every channel's GOP are taken: a switch to it waits for a keyframe",
               c->idx + 1, s->id == S_SUB ? " sub" : "", gop_cache_budget() >> 10);
    return 0;
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
            seg_set_overwrite(&c->seg, !c->stop_when_full);
            seg_set_quota(&c->seg, c->quota_bytes, c->quota_age);
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


/* ---- what keeps a set-up recording from writing ------------------------------ */

/* channels_rec_hold: a directory no channel may record into now. */
static pthread_mutex_t g_hold_lock = PTHREAD_MUTEX_INITIALIZER;
static char            g_hold[192];

static int under(const char *dir, const char *top)
{
    size_t n = strlen(top);
    while (n > 1 && top[n - 1] == '/') --n;
    return n && !strncmp(dir, top, n) && (dir[n] == 0 || dir[n] == '/');
}

static int held(const char *dir)
{
    pthread_mutex_lock(&g_hold_lock);
    int h = g_hold[0] && under(dir, g_hold);
    pthread_mutex_unlock(&g_hold_lock);
    return h;
}

/* Why the main session must not write now ("" when it may): the hold,
 * the schedule. Looked at once a second. */
static const char *rec_blocked(channel_t *c, time_t now)
{
    if (now == c->rec_why_at) return NULL;              /* unchanged */
    c->rec_why_at = now;
    if (held(c->rec_dir)) return "paused";
    char mode = rec_sched_mode(c->idx, now);
    if (mode == REC_SCHED_OFF) return "schedule off";
    if (mode == REC_SCHED_MOTION) {
        if (!(cam_events_held(c->idx, now) & CH_EV_DETECTIONS)) return "waiting for motion";
    }
    return "";
}

int channels_rec_hold(const char *dir, int timeout_ms)
{
    pthread_once(&g_once, locks_init);
    pthread_mutex_lock(&g_hold_lock);
    snprintf(g_hold, sizeof g_hold, "%s", dir);
    pthread_mutex_unlock(&g_hold_lock);
    int open = 0;
    for (int waited = 0;; waited += 100) {
        open = 0;
        for (int i = 0; i < CHANNEL_MAX; ++i) {
            channel_t *c = &g_ch[i];
            pthread_mutex_lock(&g_locks[i]);
            open += c->active && c->rec_on && c->seg.f && under(c->rec_dir, dir);
            pthread_mutex_unlock(&g_locks[i]);
        }
        if (!open || waited >= timeout_ms) break;
        usleep(100 * 1000);
    }
    return open;
}

void channels_rec_release(void)
{
    pthread_mutex_lock(&g_hold_lock);
    g_hold[0] = 0;
    pthread_mutex_unlock(&g_hold_lock);
}

/* ---- per-session thread --------------------------------------------------- */

static void on_au(void *user, const vg_au_t *au)
{
    sess_t *s = user;
    channel_t *c = s->c;

    int dec = g_can_decode && !s->src->ops->self_displayed;
    int in_gop = gop_keep(s, au, dec);
    if (dec) dec_feed(s, au);
    stream_tap_feed(c->idx, s->id == S_SUB, au, &s->norm.info, s->norm.info_gen, in_gop ? &s->gop : NULL);
    if (au->key && s->norm.info_gen) {
        pthread_mutex_lock(c->lock);
        s->width = s->norm.info.width;
        s->height = s->norm.info.height;
        pthread_mutex_unlock(c->lock);
    }
    ++s->frames;
    if (s->id != S_MAIN) return;            /* recording is the main stream's */

    if (!c->rec_on && c->rec_dir[0] && au->key) rec_retry(c, au->codec);
    if (c->rec_on) {
        const char *why = rec_blocked(c, time(NULL));
        if (why) {
            pthread_mutex_lock(c->lock);
            int was = c->rec_why[0] != 0 && strcmp(c->rec_why, "disk full") != 0;
            if (*why) {
                if (c->seg.f) rec_close(c);
                snprintf(c->rec_why, sizeof c->rec_why, "%s", why);
            } else if (was) {
                c->rec_why[0] = 0;          /* "disk full" is seg_write's to clear */
            }
            pthread_mutex_unlock(c->lock);
            if (*why && !was) vg_log("CAM%d: not recording: %s", c->idx + 1, why);
        }
        if (c->rec_why[0] && strcmp(c->rec_why, "disk full") != 0) return;
    }
    /* A keyframe that will open a new segment (seg_write's own test). */
    if (c->rec_on && au->key && (!c->seg.f || time(NULL) - c->seg.seg_start >= c->seg.seg_secs))
        rec_check(c);
    if (c->rec_on) {
        pthread_mutex_lock(c->lock);
        int was_open = c->seg.f != NULL;
        int rc = seg_write(&c->seg, au->data, au->len, au->key);
        int is_open = c->seg.f != NULL;
        if (rc == SEG_FULL) snprintf(c->rec_why, sizeof c->rec_why, "disk full");
        else if (!strcmp(c->rec_why, "disk full")) c->rec_why[0] = 0;
        pthread_mutex_unlock(c->lock);
        if (rc == SEG_FULL) {
            if (!c->full_told) {
                vg_log("CAM%d: the disk is full; recording stops until there is room (record.on_full: stop)",
                       c->idx + 1);
                events_push(c->idx + 1, "REC disk full");
                c->full_told = 1;
            }
            return;
        }
        if (c->full_told) {
            vg_log("CAM%d: room on the disk again; recording", c->idx + 1);
            events_push(c->idx + 1, "REC on");
            c->full_told = 0;
        }
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
    ++s->frames_total;
    s->bytes += au->len;
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

/* The camera named the channel (source.h, title): the name the tile shows
 * while the configuration gives none. Either session may hear it first. */
static void take_title(sess_t *s)
{
    channel_t *c = s->c;
    s->title_taken = 1;
    pthread_mutex_lock(c->lock);
    int news = strcmp(c->title, s->src->title) != 0;
    if (news) snprintf(c->title, sizeof c->title, "%s", s->src->title);
    pthread_mutex_unlock(c->lock);
    if (news) vg_log("CAM%d: the camera calls it \"%s\"", c->idx + 1, s->src->title);
}

static void sleep_interruptible(volatile int *stop, int secs)
{
    for (int i = 0; i < secs * 10 && !*stop; ++i) usleep(100 * 1000);
}

static void *sess_thread(void *arg)
{
    sess_t *s = arg;
    channel_t *c = s->c;
    int ch = c->idx + 1, backoff = 1, told_fail = 0, main = s->id == S_MAIN;
    const char *tag = main ? "" : " sub";

    while (!s->stop) {
        set_state(s, 0, CH_CONNECTING, "CONNECTING");
        if (s->src->ops->open(s->src) < 0) {
            set_state(s, 0, fail_state(s->src->fail), "%.40s", s->src->err);
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
        if (main) events_push(ch, "online");
        /* Live at its first frame, not at the login: a camera that takes
         * the claim and then sends nothing is not live meanwhile. */
        set_state(s, 0, CH_CONNECTING, "waiting for video");

        time_t sec = time(NULL);
        s->last_video = sec;
        s->frames_total = 0;
        s->bytes = 0;
        while (!s->stop) {
            int rc = s->src->ops->poll(s->src, on_raw_au, s);
            time_t now = time(NULL);
            if (s->src->title[0] && !s->title_taken) take_title(s);
            if (rc < 0) {
                vg_log("CAM%d%s %s: %s", ch, tag, s->src->host, s->src->err);
                if (main) events_push(ch, "offline");
                set_state(s, 0, fail_state(s->src->fail), "%.40s", s->src->err);
                break;
            }
            if (now - s->last_video >= NO_DATA_SECS) {
                /* Reconnect. No frame at all since connecting: the camera
                 * sends nothing the source reads as H.264 or H.265. */
                vg_log("CAM%d%s %s: no video for %d s, %u frames since connecting", ch, tag, s->src->host,
                       NO_DATA_SECS, s->frames_total);
                if (main) events_push(ch, "offline");
                set_state(s, 0, CH_NO_VIDEO, "no video for %d s", NO_DATA_SECS);
                break;
            }
            /* Rate over 2 s: cameras deliver in bursts over TCP, so a
             * one-second sample reads 0 or half the rate now and then. */
            if (now - sec >= 2) {
                pthread_mutex_lock(c->lock);
                s->fps = (s->frames + (unsigned)(now - sec) / 2) / (unsigned)(now - sec);
                s->frames = 0;
                s->kbps = (unsigned)(s->bytes * 8 / 1000 / (uint64_t)(now - sec));
                s->bytes = 0;
                pthread_mutex_unlock(c->lock);
                sec = now;
            }
            /* Live = video within the last LIVE_SECS, not "fps > 0". */
            int live = s->frames_total && now - s->last_video < LIVE_SECS;
            if (s->frames_total && s->status == CH_CONNECTING) {
                set_state(s, live, CH_LIVE, "");
            } else if (live != s->live) {
                pthread_mutex_lock(c->lock);
                s->live = live;
                pthread_mutex_unlock(c->lock);
            }
        }
        s->src->ops->close(s->src);
        pthread_mutex_lock(c->lock);
        if (main) rec_close(c);
        s->fps = 0;
        s->kbps = 0;
        s->live = 0;
        pthread_mutex_unlock(c->lock);
        /* The next connection starts at a keyframe: nothing to keep. */
        atomic_store(&s->gop_ok, 0);
        gop_cache_free(&s->gop);
        if (!s->stop) sleep_interruptible(&s->stop, 1);
    }
    /* A decoder this stream still holds goes with it. */
    pthread_mutex_lock(c->dec_lock);
    if (atomic_load(&c->dec_holder) == s->id) decoder_close(c);
    pthread_mutex_unlock(c->dec_lock);
    atomic_store(&s->gop_ok, 0);
    gop_cache_free(&s->gop);
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
    s->dec_ok = s->was_viewed = s->title_taken = 0;
    s->frames = 0;
    s->info_at = 0;
    au_norm_init(&s->norm, VG_CODEC_NONE);
    gop_cache_init(&s->gop, id == S_SUB ? GOP_SUB_MAX : GOP_MAIN_MAX);
    atomic_store(&s->gop_ok, 0);
    s->gop_told = 0;
    pthread_mutex_lock(c->lock);
    s->done = 0;
    s->status = CH_CONNECTING;
    snprintf(s->state, sizeof s->state, "CONNECTING");
    s->live = 0;
    s->fps = s->width = s->height = 0;
    s->kbps = 0;
    s->bytes = 0;
    s->codec = VG_CODEC_NONE;
    pthread_mutex_unlock(c->lock);
    stream_tap_present(c->idx, id == S_SUB, 1);
    if (pthread_create(&s->thread, NULL, sess_thread, s) != 0) {
        stream_tap_present(c->idx, id == S_SUB, 0);
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
    stream_tap_present(c->idx, id == S_SUB, 0);
    pthread_mutex_lock(c->lock);
    s->thread_up = 0;
    s->live = 0;
    s->fps = 0;
    s->kbps = 0;
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

static void view_set(channel_t *c, int i)
{
    atomic_store(&c->on_screen, g_have_tiles && g_tiles[i].w > 0 && g_tiles[i].h > 0);
    atomic_store(&c->view, view_for(c, i));
}

/* Set every channel's view: the sessions see it at their next AU. */
static void views_update(void)
{
    for (int i = 0; i < CHANNEL_MAX; ++i)
        if (g_ch[i].active) view_set(&g_ch[i], i);
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
           c->seg_secs == cfg->seg_secs && c->keep == cfg->keep && c->allow_rootfs == cfg->allow_rootfs &&
           c->stop_when_full == cfg->stop_when_full && c->quota_bytes == cfg->quota_bytes[i] &&
           c->quota_age == cfg->quota_age[i];
}

static void slot_stop(channel_t *c)
{
    for (int i = 0; i < S_N; ++i) c->s[i].stop = 1;
}

/* Tear down a channel whose sessions have been told to stop. */
static void slot_teardown(channel_t *c)
{
    for (int i = 0; i < S_N; ++i) sess_reap(c, i);
    /* The camera is gone: nothing it said goes on. */
    cam_events_clear(c->idx, CAM_EV_SRC_NETIP);
    cam_events_clear(c->idx, CAM_EV_SRC_ONVIF);
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

/* ---- events ------------------------------------------------------------------ */

static int is_netip(const char *url) { return !strncmp(url, "netip://", 8); }

/* Where channel c's camera events come from, from the thread that calls
 * channels_apply: its NETIP connection's alarms (auto or netip, a NETIP
 * camera), or none. A change applies at once, the streams untouched. */
static void events_apply(channel_t *c, ch_events_mode_t mode, const char *onvif_url)
{
    int changed = !c->s[S_MAIN].src || c->ev_mode != mode || strcmp(c->onvif_url, onvif_url ? onvif_url : "");
    c->ev_mode = mode;
    snprintf(c->onvif_url, sizeof c->onvif_url, "%s", onvif_url ? onvif_url : "");
    int netip = is_netip(c->url) && (mode == CH_EVENTS_AUTO || mode == CH_EVENTS_NETIP);
    if (c->s[S_MAIN].src) c->s[S_MAIN].src->events = netip;
    if (!changed) return;
    if (mode == CH_EVENTS_NETIP && !is_netip(c->url))
        vg_log("CAM%d: events netip: not a netip:// camera, no events", c->idx + 1);
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
    c->stop_when_full = cfg->stop_when_full;
    c->quota_bytes = cfg->quota_bytes[i];
    c->quota_age = cfg->quota_age[i];
    c->st.configured = 1;
    snprintf(c->st.name, sizeof c->st.name, "%s", name);
    if (cfg->rec_dir) {
        rec_setup(c, VG_CODEC_NONE);
        if (!c->rec_on) c->rec_retry_at = time(NULL);
    }
    if (g_have_tiles && hal_caps()->display && !c->self_displayed && g_tiles[i].w > 0)
        hal_display_set_tile(i, &g_tiles[i]);
    view_set(c, i);
    if (sess_start(c, S_MAIN, url) < 0) {
        vg_log("CAM%d: cannot start its thread", i + 1);
        slot_teardown(c);
        return -1;
    }
    events_apply(c, cfg->events[i], cfg->onvif_urls[i]);
    /* Both streams for as long as the channel runs: the sub-stream's
     * picture is there the moment a tile wants it. */
    if (c->sub_url[0] && sess_start(c, S_SUB, c->sub_url) < 0)
        vg_log("CAM%d: cannot start its sub-stream", i + 1);
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

    for (int i = 0; i < CHANNEL_MAX; ++i) {
        pthread_mutex_lock(&g_locks[i]);
        g_disabled[i] = i < cfg->n && i < limit && cfg->disabled[i];
        snprintf(g_off_name[i], sizeof g_off_name[i], "%s", g_disabled[i] && cfg->names[i] ? cfg->names[i] : "");
        pthread_mutex_unlock(&g_locks[i]);
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
                events_apply(c, cfg->events[i], cfg->onvif_urls[i]);
                /* A new sub-stream: only that session starts again. */
                char sub[sizeof c->sub_url];
                if (!c->self_displayed) sub_url_of(cfg, i, sub, sizeof sub);
                else sub[0] = 0;
                if (strcmp(sub, c->sub_url)) {
                    c->s[S_SUB].stop = 1;
                    sess_reap(c, S_SUB);
                    snprintf(c->sub_url, sizeof c->sub_url, "%s", sub);
                    view_set(c, i);
                    if (sub[0] && sess_start(c, S_SUB, sub) < 0)
                        vg_log("CAM%d: cannot start its sub-stream", i + 1);
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

void channels_set_aspect(vg_aspect_t how)
{
    pthread_once(&g_once, locks_init);
    if (atomic_exchange(&g_aspect, (int)how) == (int)how) return;
    for (int i = 0; i < CHANNEL_MAX; ++i) {
        channel_t *c = &g_ch[i];
        /* Only this thread starts and stops channels: active is stable. */
        if (!c->active) continue;
        pthread_mutex_lock(c->dec_lock);
        aspect_update(c);
        pthread_mutex_unlock(c->dec_lock);
    }
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
