/* SPDX-License-Identifier: GPL-3.0-or-later */
/* channel.c — camera channels (see channel.h). */
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

typedef struct {
    int             idx;
    volatile int    stop;           /* this channel's; its source watches it too */
    int             active;         /* set up and running (status readers: under lock) */
    /* What it was started with, to tell whether a new configuration
     * changes it. */
    char            url[512], rec_dir[192];
    int             seg_secs, keep, allow_rootfs;

    source_t       *src;
    au_norm_t       norm;
    seg_writer_t    seg;
    int             rec_on, rec_fail_told;
    time_t          rec_retry_at;   /* not set up then: try again later */
    time_t          rec_checked_at; /* the directory was last looked at then (rec_check) */
    rec_place_t     rec_place;      /* the last answer, to say each change once */
    int             rec_told;
    pthread_t       thread;
    int             thread_up;

    /* Decoder state (channel thread only). */
    int             dec_up;
    vg_codec_t      dec_codec;
    unsigned        dec_gen;        /* au_norm info_gen it was built for */
    time_t          dec_failed_at;
    unsigned        dec_failed_frees; /* g_dec_frees when it failed */
    int             dec_told;       /* the failure was reported */
    int             dec_ok;         /* feeding the decoder since the last keyframe */
    int             was_shown;
    /* Its tile is on screen (set by channels_set_tiles' thread). */
    atomic_int      shown;

    /* Status (read by the UI and the API under `lock`). */
    pthread_mutex_t *lock;          /* g_locks[idx]: outlives every restart */
    channel_status_t st;
    unsigned        frames;
    time_t          last_video;     /* last access unit from the source */
} channel_t;

static channel_t       g_ch[CHANNEL_MAX];
static pthread_mutex_t g_locks[CHANNEL_MAX];
static pthread_once_t  g_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_n_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_n;         /* tiles in use */
static int             g_can_decode;
static int             g_running;   /* past the first apply: changes are news */
static vg_rect_t       g_tiles[CHANNEL_MAX];
static int             g_have_tiles;
static int             g_shown_only;   /* hal_caps()->dec_shown_only with a display */
/* Decoders closed so far: a channel that could not get one (memory, the
 * decoder's channels) tries again as soon as another lets its go. */
static atomic_uint     g_dec_frees;

static void locks_init(void)
{
    for (int i = 0; i < CHANNEL_MAX; ++i) pthread_mutex_init(&g_locks[i], NULL);
}

static void set_state(channel_t *c, int live, const char *fmt, ...)
{
    pthread_mutex_lock(c->lock);
    c->st.live = live;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->st.state, sizeof c->st.state, fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(c->lock);
}

void channels_status(int idx, channel_status_t *st)
{
    memset(st, 0, sizeof *st);
    if (idx < 0 || idx >= CHANNEL_MAX) return;
    pthread_once(&g_once, locks_init);
    channel_t *c = &g_ch[idx];
    pthread_mutex_lock(&g_locks[idx]);
    if (c->active) {
        *st = c->st;
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

/* ---- decoding ------------------------------------------------------------- */

static void decoder_close(channel_t *c)
{
    if (!c->dec_up) return;
    hal_dec_close(c->idx);
    c->dec_up = 0;
    atomic_fetch_add(&g_dec_frees, 1);
    pthread_mutex_lock(c->lock);
    c->st.decoding = 0;
    pthread_mutex_unlock(c->lock);
}

/* At a keyframe: make sure the decoder matches the stream. Returns 1 if
 * the AU can go to the decoder. */
static int decoder_ensure(channel_t *c, vg_codec_t codec)
{
    if (!c->norm.info_gen) return c->dec_up;            /* no SPS seen yet */
    if (c->dec_up && c->dec_codec == codec && c->dec_gen == c->norm.info_gen) return 1;
    if (c->dec_failed_at && time(NULL) - c->dec_failed_at < DEC_RETRY_SECS &&
        atomic_load(&g_dec_frees) == c->dec_failed_frees)
        return 0;

    decoder_close(c);
    const vg_stream_info_t *si = &c->norm.info;
    /* Before the attempt: a decoder freed while it runs counts as news. */
    unsigned frees = atomic_load(&g_dec_frees);
    if (hal_dec_open(c->idx, codec, si) < 0) {
        /* Said once until it works: with more cameras than decoders it
         * fails again at every retry. */
        if (!c->dec_told) {
            vg_log("CAM%d: no decoder for %s %ux%u", c->idx + 1, vg_codec_name(codec), si->width, si->height);
            events_push(c->idx + 1, "no decoder");
            c->dec_told = 1;
        }
        c->dec_failed_at = time(NULL);
        c->dec_failed_frees = frees;
        return 0;
    }
    vg_log("CAM%d: decoder %s %ux%u, %u refs", c->idx + 1, vg_codec_name(codec), si->width, si->height, si->refs);
    c->dec_up = 1;
    c->dec_codec = codec;
    c->dec_gen = c->norm.info_gen;
    c->dec_failed_at = 0;
    c->dec_told = 0;
    pthread_mutex_lock(c->lock);
    c->st.decoding = 1;
    c->st.width = si->width;
    c->st.height = si->height;
    pthread_mutex_unlock(c->lock);
    return 1;
}

/* Close the open segment (caller holds c->lock); report a lost tail. */
static void rec_close(channel_t *c)
{
    if (seg_close(&c->seg) < 0) {
        vg_log("CAM%d: closing a recording failed; its last data is lost", c->idx + 1);
        events_push(c->idx + 1, "REC failed");
    }
}

/* ---- per-channel thread --------------------------------------------------- */

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

static void on_au(void *user, const vg_au_t *au)
{
    channel_t *c = user;

    int shown = !g_shown_only || atomic_load(&c->shown);
    if (shown && !c->was_shown) c->dec_failed_at = 0;   /* back on screen: try at once */
    c->was_shown = shown;
    if (g_can_decode && !c->src->ops->self_displayed && shown) {
        if (au->key) c->dec_ok = decoder_ensure(c, au->codec);
        /* A dropped AU breaks the reference chain: wait for the next
         * keyframe instead of feeding pictures that cannot decode. */
        if (c->dec_ok && hal_dec_send(c->idx, au) < 0) c->dec_ok = 0;
    } else if (c->dec_up) {
        /* Off screen on a platform whose decoders do not stretch to every
         * channel: give the decoder to the channels shown. */
        decoder_close(c);
        c->dec_ok = 0;
    }
    if (!c->dec_up && au->key && c->norm.info_gen) {
        pthread_mutex_lock(c->lock);
        c->st.width = c->norm.info.width;
        c->st.height = c->norm.info.height;
        pthread_mutex_unlock(c->lock);
    }

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
    ++c->frames;
}

/* Raw AU from the source: (re)start normalization on a codec change. */
static void on_raw_au(void *user, const vg_au_t *au)
{
    channel_t *c = user;
    if (au->codec == VG_CODEC_NONE) return;
    /* Only video counts as "the camera is alive": RTCP, audio or info
     * packets must not hold off the no-data reconnect. */
    c->last_video = time(NULL);
    if (au->codec != c->norm.codec) {
        au_norm_free(&c->norm);
        au_norm_init(&c->norm, au->codec);
        const char *ext = au->codec == VG_CODEC_H264 ? "h264" : "h265";
        pthread_mutex_lock(c->lock);
        rec_close(c);
        seg_set_ext(&c->seg, ext);
        c->st.codec = au->codec;
        pthread_mutex_unlock(c->lock);
    }
    au_norm_push(&c->norm, au, on_au, c);
}

static void sleep_interruptible(volatile int *stop, int secs)
{
    for (int i = 0; i < secs * 10 && !*stop; ++i) usleep(100 * 1000);
}

static void *channel_thread(void *arg)
{
    channel_t *c = arg;
    int ch = c->idx + 1, backoff = 1, told_fail = 0;

    while (!c->stop) {
        set_state(c, 0, "CONNECTING");
        if (c->src->ops->open(c->src) < 0) {
            set_state(c, 0, "%.40s", c->src->err);
            if (!told_fail) {
                vg_log("CAM%d %s: %s", ch, c->src->host, c->src->err);
                events_push(ch, "down");
            }
            told_fail = 1;
            sleep_interruptible(&c->stop, backoff);
            if (backoff < RECONNECT_MAX_SECS) backoff *= 2;
            continue;
        }
        backoff = 1;
        told_fail = 0;

        /* A new stream: same codec keeps the cache but waits for a keyframe. */
        if (c->src->codec != VG_CODEC_NONE && c->src->codec != c->norm.codec) {
            au_norm_free(&c->norm);
            au_norm_init(&c->norm, c->src->codec);
        } else {
            au_norm_reset(&c->norm);
        }
        for (int i = 0; i < c->src->ps_n; ++i) au_norm_add_param_set(&c->norm, c->src->ps[i], c->src->ps_len[i]);
        if (c->norm.codec != VG_CODEC_NONE) {
            pthread_mutex_lock(c->lock);
            seg_set_ext(&c->seg, c->norm.codec == VG_CODEC_H264 ? "h264" : "h265");
            c->st.codec = c->norm.codec;
            pthread_mutex_unlock(c->lock);
        }
        if (c->dec_up) hal_dec_reset(c->idx);
        c->dec_ok = 0;
        c->dec_failed_at = 0;

        vg_log("CAM%d %s: online", ch, c->src->host);
        events_push(ch, "online");
        set_state(c, 1, "");

        time_t sec = time(NULL);
        c->last_video = sec;
        while (!c->stop) {
            int rc = c->src->ops->poll(c->src, on_raw_au, c);
            time_t now = time(NULL);
            if (rc < 0) {
                vg_log("CAM%d %s: %s", ch, c->src->host, c->src->err);
                events_push(ch, "offline");
                break;
            }
            if (now - c->last_video >= NO_DATA_SECS) {
                vg_log("CAM%d %s: no data for %d s", ch, c->src->host, NO_DATA_SECS);
                events_push(ch, "offline");
                break;
            }
            /* Rate over 2 s: cameras deliver in bursts over TCP, so a
             * one-second sample reads 0 or half the rate now and then. */
            if (now - sec >= 2) {
                pthread_mutex_lock(c->lock);
                c->st.fps = (c->frames + (unsigned)(now - sec) / 2) / (unsigned)(now - sec);
                c->frames = 0;
                pthread_mutex_unlock(c->lock);
                sec = now;
            }
            /* Live = video within the last LIVE_SECS, not "fps > 0". */
            int live = now - c->last_video < LIVE_SECS;
            if (live != c->st.live) {
                pthread_mutex_lock(c->lock);
                c->st.live = live;
                pthread_mutex_unlock(c->lock);
            }
        }
        c->src->ops->close(c->src);
        pthread_mutex_lock(c->lock);
        rec_close(c);
        c->st.fps = 0;
        c->st.live = 0;
        pthread_mutex_unlock(c->lock);
        if (!c->stop) sleep_interruptible(&c->stop, 1);
    }
    return NULL;
}

/* ---- public ---------------------------------------------------------------- */

static const char *str_or_empty(const char *s) { return s ? s : ""; }

/* Does channel i run what `cfg` asks for? (The name does not count: it
 * changes without a restart.) */
static int slot_matches(const channel_t *c, const channels_config_t *cfg, int i)
{
    const char *url = i < cfg->n ? cfg->urls[i] : NULL;
    if (!c->active) return url == NULL;
    return url && !strcmp(c->url, url) && !strcmp(c->rec_dir, str_or_empty(cfg->rec_dir)) &&
           c->seg_secs == cfg->seg_secs && c->keep == cfg->keep && c->allow_rootfs == cfg->allow_rootfs;
}

/* Tear down a channel whose thread has been told to stop. */
static void slot_teardown(channel_t *c)
{
    if (c->thread_up) pthread_join(c->thread, NULL);
    c->thread_up = 0;
    pthread_mutex_lock(c->lock);
    c->active = 0;
    pthread_mutex_unlock(c->lock);
    decoder_close(c);
    if (c->src && !c->src->ops->self_displayed && hal_caps()->display) hal_display_clear_tile(c->idx);
    pthread_mutex_lock(c->lock);
    rec_close(c);
    pthread_mutex_unlock(c->lock);
    source_destroy(c->src);
    c->src = NULL;
    au_norm_free(&c->norm);
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
    pthread_mutex_unlock(&g_locks[i]);

    c->src = source_create(url, i);
    if (!c->src) {
        char red[512];
        schema_redact_url(url, red, sizeof red);
        vg_log("CAM%d: cannot use '%s' (schemes: %s)", i + 1, red, source_schemes());
        events_push(i + 1, "bad URL");
        return -1;
    }
    c->src->stop = &c->stop;
    snprintf(c->url, sizeof c->url, "%s", url);
    snprintf(c->rec_dir, sizeof c->rec_dir, "%s", str_or_empty(cfg->rec_dir));
    c->seg_secs = cfg->seg_secs;
    c->keep = cfg->keep;
    c->allow_rootfs = cfg->allow_rootfs;
    au_norm_init(&c->norm, VG_CODEC_NONE);
    c->st.configured = 1;
    snprintf(c->st.host, sizeof c->st.host, "%.63s", c->src->host);
    snprintf(c->st.name, sizeof c->st.name, "%s", name);
    if (cfg->rec_dir) {
        rec_setup(c, VG_CODEC_NONE);
        if (!c->rec_on) c->rec_retry_at = time(NULL);
    }
    if (g_have_tiles && hal_caps()->display && !c->src->ops->self_displayed && g_tiles[i].w > 0)
        hal_display_set_tile(i, &g_tiles[i]);
    atomic_store(&c->shown, !g_have_tiles || g_tiles[i].w > 0);
    if (pthread_create(&c->thread, NULL, channel_thread, c) != 0) {
        vg_log("CAM%d: cannot start its thread", i + 1);
        slot_teardown(c);
        return -1;
    }
    c->thread_up = 1;
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
            }
            continue;
        }
        change[i] = 1;
        was[i] = c->active;
        if (c->active) c->stop = 1;
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

void channels_set_tiles(const vg_rect_t *tiles)
{
    if (!hal_caps()->display) return;
    memcpy(g_tiles, tiles, sizeof g_tiles);
    g_have_tiles = 1;
    for (int i = 0; i < CHANNEL_MAX; ++i) {
        channel_t *c = &g_ch[i];
        /* Only this thread starts and stops channels: active and src are
         * stable here. */
        if (!c->active || !c->src || c->src->ops->self_displayed) continue;
        atomic_store(&c->shown, g_tiles[i].w > 0 && g_tiles[i].h > 0);
        if (g_tiles[i].w > 0 && g_tiles[i].h > 0) {
            if (hal_display_set_tile(i, &g_tiles[i]) < 0)
                vg_log("CAM%d: cannot move its picture to %dx%d at %d,%d", i + 1, g_tiles[i].w,
                       g_tiles[i].h, g_tiles[i].x, g_tiles[i].y);
        } else
            hal_display_clear_tile(i);
    }
}

void channels_stop(void)
{
    for (int i = 0; i < CHANNEL_MAX; ++i)
        if (g_ch[i].active) g_ch[i].stop = 1;
    for (int i = 0; i < CHANNEL_MAX; ++i)
        if (g_ch[i].active) slot_teardown(&g_ch[i]);
    pthread_mutex_lock(&g_n_lock);
    g_n = 0;
    pthread_mutex_unlock(&g_n_lock);
    g_running = 0;
}
