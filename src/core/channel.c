/* SPDX-License-Identifier: GPL-3.0-or-later */
/* channel.c — camera channels (see channel.h). */
#define _DEFAULT_SOURCE
#include <pthread.h>
#include <stdarg.h>
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
#include "seg_writer.h"
#include "source.h"

#define NO_DATA_SECS       5        /* reconnect after this long without video */
#define LIVE_SECS          3        /* "live" while video arrived this recently */
#define RECONNECT_MAX_SECS 10
#define DEC_RETRY_SECS     10       /* retry a failed decoder setup */

typedef struct {
    int             idx;
    source_t       *src;
    au_norm_t       norm;
    seg_writer_t    seg;
    int             rec_on, rec_fail_told;
    pthread_t       thread;
    int             thread_up;

    /* Decoder state (channel thread only). */
    int             dec_up;
    vg_codec_t      dec_codec;
    unsigned        dec_gen;        /* au_norm info_gen it was built for */
    time_t          dec_failed_at;
    int             dec_ok;         /* feeding the decoder since the last keyframe */

    /* Status (read by the UI under `lock`). */
    pthread_mutex_t lock;
    channel_status_t st;
    unsigned        frames;
    time_t          last_video;     /* last access unit from the source */
} channel_t;

static channel_t     g_ch[CHANNEL_MAX];
static int           g_n;
static volatile int  g_stop;        /* sources watch it too (source_t.stop) */
static int           g_can_decode;

static void set_state(channel_t *c, int live, const char *fmt, ...)
{
    pthread_mutex_lock(&c->lock);
    c->st.live = live;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->st.state, sizeof c->st.state, fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&c->lock);
}

void channels_status(int idx, channel_status_t *st)
{
    memset(st, 0, sizeof *st);
    if (idx < 0 || idx >= g_n) return;
    channel_t *c = &g_ch[idx];
    pthread_mutex_lock(&c->lock);
    *st = c->st;
    st->recording = c->rec_on && c->seg.f != NULL;
    st->rec_bytes = c->seg.bytes;
    pthread_mutex_unlock(&c->lock);
}

int channels_count(void) { return g_n; }

/* ---- decoding ------------------------------------------------------------- */

static void decoder_close(channel_t *c)
{
    if (!c->dec_up) return;
    hal_dec_close(c->idx);
    c->dec_up = 0;
    pthread_mutex_lock(&c->lock);
    c->st.decoding = 0;
    pthread_mutex_unlock(&c->lock);
}

/* At a keyframe: make sure the decoder matches the stream. Returns 1 if
 * the AU can go to the decoder. */
static int decoder_ensure(channel_t *c, vg_codec_t codec)
{
    if (!c->norm.info_gen) return c->dec_up;            /* no SPS seen yet */
    if (c->dec_up && c->dec_codec == codec && c->dec_gen == c->norm.info_gen) return 1;
    if (c->dec_failed_at && time(NULL) - c->dec_failed_at < DEC_RETRY_SECS) return 0;

    decoder_close(c);
    const vg_stream_info_t *si = &c->norm.info;
    if (hal_dec_open(c->idx, codec, si) < 0) {
        vg_log("CAM%d: no decoder for %s %ux%u", c->idx + 1, vg_codec_name(codec), si->width, si->height);
        events_push(c->idx + 1, "no decoder");
        c->dec_failed_at = time(NULL);
        return 0;
    }
    vg_log("CAM%d: decoder %s %ux%u, %u refs", c->idx + 1, vg_codec_name(codec), si->width, si->height, si->refs);
    c->dec_up = 1;
    c->dec_codec = codec;
    c->dec_gen = c->norm.info_gen;
    c->dec_failed_at = 0;
    pthread_mutex_lock(&c->lock);
    c->st.decoding = 1;
    c->st.width = si->width;
    c->st.height = si->height;
    pthread_mutex_unlock(&c->lock);
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

static void on_au(void *user, const vg_au_t *au)
{
    channel_t *c = user;

    if (g_can_decode && !c->src->ops->self_displayed) {
        if (au->key) c->dec_ok = decoder_ensure(c, au->codec);
        if (c->dec_ok) hal_dec_send(c->idx, au);
    } else if (au->key && c->norm.info_gen) {
        pthread_mutex_lock(&c->lock);
        c->st.width = c->norm.info.width;
        c->st.height = c->norm.info.height;
        pthread_mutex_unlock(&c->lock);
    }

    if (c->rec_on) {
        pthread_mutex_lock(&c->lock);
        int was_open = c->seg.f != NULL;
        int rc = seg_write(&c->seg, au->data, au->len, au->key);
        int is_open = c->seg.f != NULL;
        pthread_mutex_unlock(&c->lock);
        if (rc < 0 && !c->rec_fail_told) {
            vg_log("CAM%d: recording failed", c->idx + 1);
            events_push(c->idx + 1, "REC failed");
            c->rec_fail_told = 1;
        } else if (rc == 0 && is_open && !was_open) {
            c->rec_fail_told = 0;
        }
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
        pthread_mutex_lock(&c->lock);
        rec_close(c);
        seg_set_ext(&c->seg, ext);
        c->st.codec = au->codec;
        pthread_mutex_unlock(&c->lock);
    }
    au_norm_push(&c->norm, au, on_au, c);
}

static void sleep_interruptible(int secs)
{
    for (int i = 0; i < secs * 10 && !g_stop; ++i) usleep(100 * 1000);
}

static void *channel_thread(void *arg)
{
    channel_t *c = arg;
    int ch = c->idx + 1, backoff = 1, told_fail = 0;

    while (!g_stop) {
        set_state(c, 0, "CONNECTING");
        if (c->src->ops->open(c->src) < 0) {
            set_state(c, 0, "%.40s", c->src->err);
            if (!told_fail) {
                vg_log("CAM%d %s: %s", ch, c->src->host, c->src->err);
                events_push(ch, "down");
            }
            told_fail = 1;
            sleep_interruptible(backoff);
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
            pthread_mutex_lock(&c->lock);
            seg_set_ext(&c->seg, c->norm.codec == VG_CODEC_H264 ? "h264" : "h265");
            c->st.codec = c->norm.codec;
            pthread_mutex_unlock(&c->lock);
        }
        if (c->dec_up) hal_dec_reset(c->idx);
        c->dec_ok = 0;
        c->dec_failed_at = 0;

        vg_log("CAM%d %s: online", ch, c->src->host);
        events_push(ch, "online");
        set_state(c, 1, "");

        time_t sec = time(NULL);
        c->last_video = sec;
        while (!g_stop) {
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
                pthread_mutex_lock(&c->lock);
                c->st.fps = (c->frames + (unsigned)(now - sec) / 2) / (unsigned)(now - sec);
                c->frames = 0;
                pthread_mutex_unlock(&c->lock);
                sec = now;
            }
            /* Live = video within the last LIVE_SECS, not "fps > 0". */
            int live = now - c->last_video < LIVE_SECS;
            if (live != c->st.live) {
                pthread_mutex_lock(&c->lock);
                c->st.live = live;
                pthread_mutex_unlock(&c->lock);
            }
        }
        c->src->ops->close(c->src);
        pthread_mutex_lock(&c->lock);
        rec_close(c);
        c->st.fps = 0;
        c->st.live = 0;
        pthread_mutex_unlock(&c->lock);
        if (!g_stop) sleep_interruptible(1);
    }
    return NULL;
}

/* ---- public ---------------------------------------------------------------- */

int channels_start(const channels_config_t *cfg, const vg_rect_t *tiles)
{
    const hal_caps_t *caps = hal_caps();
    g_stop = 0;
    g_can_decode = caps->decoder;
    g_n = cfg->n > CHANNEL_MAX ? CHANNEL_MAX : cfg->n;
    if (caps->max_channels && g_n > caps->max_channels) {
        vg_log("%s shows at most %d channels; using the first %d", caps->name, caps->max_channels, caps->max_channels);
        g_n = caps->max_channels;
    }

    for (int i = 0; i < g_n; ++i) {
        channel_t *c = &g_ch[i];
        memset(c, 0, sizeof *c);
        c->idx = i;
        pthread_mutex_init(&c->lock, NULL);
        c->src = source_create(cfg->urls[i], i);
        if (c->src) c->src->stop = &g_stop;
        if (!c->src) {
            vg_log("CAM%d: cannot use '%s' (schemes: %s)", i + 1, cfg->urls[i], source_schemes());
            g_n = i;
            channels_stop();
            return -1;
        }
        au_norm_init(&c->norm, VG_CODEC_NONE);
        c->st.configured = 1;
        snprintf(c->st.host, sizeof c->st.host, "%.63s", c->src->host);
        if (cfg->rec_dir) {
            char prefix[8];
            snprintf(prefix, sizeof prefix, "CAM%d", i + 1);
            if (seg_init(&c->seg, cfg->rec_dir, prefix, cfg->seg_secs, cfg->keep) == 0)
                c->rec_on = 1;
            else
                vg_log("cannot create %s, CAM%d not recorded", cfg->rec_dir, i + 1);
        }
        if (tiles && caps->display && !c->src->ops->self_displayed)
            hal_display_set_tile(i, &tiles[i]);
    }
    for (int i = 0; i < g_n; ++i) {
        if (pthread_create(&g_ch[i].thread, NULL, channel_thread, &g_ch[i]) != 0) {
            vg_log("CAM%d: cannot start its thread", i + 1);
            channels_stop();
            return -1;
        }
        g_ch[i].thread_up = 1;
    }
    return 0;
}

void channels_stop(void)
{
    g_stop = 1;
    for (int i = 0; i < g_n; ++i) {
        channel_t *c = &g_ch[i];
        if (c->thread_up) pthread_join(c->thread, NULL);
        c->thread_up = 0;
        decoder_close(c);
        if (c->src && !c->src->ops->self_displayed && hal_caps()->display) hal_display_clear_tile(i);
        rec_close(c);
        source_destroy(c->src);
        c->src = NULL;
        au_norm_free(&c->norm);
        pthread_mutex_destroy(&c->lock);
    }
    g_n = 0;
}
