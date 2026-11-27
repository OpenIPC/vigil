/* SPDX-License-Identifier: GPL-3.0-or-later */
/* stream_tap.c — the cameras' streams, for vigild's own servers (see
 * stream_tap.h).
 *
 * One lock per stream: its session thread holds it while it hands an AU
 * to the stream's taps, so a tap closed under it is never called again.
 * A stream without taps costs its session an atomic load per AU, and a
 * look at the keyframes' parameter sets.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "channel.h"
#include "stream_tap.h"

struct stream_tap {
    stream_tap_t *next;
    int           ch, sub;
    stream_tap_fn fn;
    void         *user;
    int           started;              /* past its first keyframe (or the GOP so far) */
};

typedef struct {
    pthread_mutex_t  lock;
    atomic_int       n_taps;
    atomic_uint      gen;               /* the feeder's info_gen last seen (0: none) */
    stream_tap_t    *taps;
    int              present;
    vg_stream_info_t si;
    vg_codec_t       codec;
    stream_tap_ps_t  ps;
} tap_stream_t;

static tap_stream_t   g_st[CHANNEL_MAX][2];
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static atomic_int     g_standby;

static void init_once(void)
{
    for (int i = 0; i < CHANNEL_MAX; ++i)
        for (int s = 0; s < 2; ++s) pthread_mutex_init(&g_st[i][s].lock, NULL);
}

static tap_stream_t *stream_of(int ch, int sub)
{
    if (ch < 0 || ch >= CHANNEL_MAX || (sub != 0 && sub != 1)) return NULL;
    pthread_once(&g_once, init_once);
    return &g_st[ch][sub];
}

stream_tap_t *stream_tap_open(int ch, int sub, stream_tap_fn fn, void *user)
{
    tap_stream_t *st = stream_of(ch, sub);
    if (!st || !fn) return NULL;
    stream_tap_t *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->ch = ch;
    t->sub = sub;
    t->fn = fn;
    t->user = user;
    pthread_mutex_lock(&st->lock);
    if (!st->present) {
        pthread_mutex_unlock(&st->lock);
        free(t);
        return NULL;
    }
    t->next = st->taps;
    st->taps = t;
    atomic_fetch_add(&st->n_taps, 1);
    pthread_mutex_unlock(&st->lock);
    return t;
}

void stream_tap_close(stream_tap_t *t)
{
    if (!t) return;
    tap_stream_t *st = stream_of(t->ch, t->sub);
    pthread_mutex_lock(&st->lock);
    for (stream_tap_t **p = &st->taps; *p; p = &(*p)->next)
        if (*p == t) {
            *p = t->next;
            atomic_fetch_sub(&st->n_taps, 1);
            break;
        }
    pthread_mutex_unlock(&st->lock);
    free(t);
}

int stream_tap_info(int ch, int sub, vg_stream_info_t *si, vg_codec_t *codec)
{
    tap_stream_t *st = stream_of(ch, sub);
    if (!st) return -1;
    pthread_mutex_lock(&st->lock);
    int known = st->present && atomic_load(&st->gen) != 0 && st->codec != VG_CODEC_NONE;
    if (known) {
        if (si) *si = st->si;
        if (codec) *codec = st->codec;
    }
    pthread_mutex_unlock(&st->lock);
    return known ? 0 : -1;
}

int stream_tap_params(int ch, int sub, stream_tap_ps_t *ps)
{
    memset(ps, 0, sizeof *ps);
    tap_stream_t *st = stream_of(ch, sub);
    if (!st) return 0;
    pthread_mutex_lock(&st->lock);
    if (st->present) *ps = st->ps;
    pthread_mutex_unlock(&st->lock);
    return ps->n;
}

void stream_tap_standby(int on)
{
    if (on) atomic_fetch_add(&g_standby, 1);
    else if (atomic_fetch_sub(&g_standby, 1) <= 0) atomic_fetch_add(&g_standby, 1);
}

int stream_tap_keep(int ch, int sub)
{
    tap_stream_t *st = stream_of(ch, sub);
    if (!st) return TAP_KEEP_NONE;
    if (atomic_load(&st->n_taps) > 0) return TAP_KEEP_MUST;
    return atomic_load(&g_standby) > 0 ? TAP_KEEP_SPARE : TAP_KEEP_NONE;
}

void stream_tap_present(int ch, int sub, int present)
{
    tap_stream_t *st = stream_of(ch, sub);
    if (!st) return;
    pthread_mutex_lock(&st->lock);
    st->present = !!present;
    atomic_store(&st->gen, 0);
    st->codec = VG_CODEC_NONE;
    memset(&st->si, 0, sizeof st->si);
    memset(&st->ps, 0, sizeof st->ps);
    /* Taps stay open across a restart of the session (a new URL, a
     * reconnect): the stream goes on at its next keyframe. */
    for (stream_tap_t *t = st->taps; t; t = t->next) t->started = 0;
    pthread_mutex_unlock(&st->lock);
}

/* The byte after the next start code (00 00 01) at or after `i`, or len. */
static size_t after_start_code(const uint8_t *b, size_t len, size_t i)
{
    while (i + 3 <= len) {
        if (b[i + 2] > 1) i += 3;
        else if (b[i + 2] == 1 && b[i] == 0 && b[i + 1] == 0) return i + 3;
        else ++i;
    }
    return len;
}

/* The parameter sets at the head of keyframe `au` (au_norm puts them
 * before the picture), in SDP order. */
static void params_of(const vg_au_t *au, stream_tap_ps_t *ps)
{
    memset(ps, 0, sizeof *ps);
    ps->codec = au->codec;
    int h265 = au->codec == VG_CODEC_H265;
    size_t i = after_start_code(au->data, au->len, 0);
    while (i < au->len) {
        size_t next = after_start_code(au->data, au->len, i);
        size_t end = next < au->len ? next - 3 : au->len;
        while (end > i && au->data[end - 1] == 0) --end;     /* a 4-byte start code's zero */
        const uint8_t *nal = au->data + i;
        size_t len = end - i;
        if (len >= 2) {
            int t = h265 ? (nal[0] >> 1) & 0x3f : nal[0] & 0x1f;
            int slot = h265 ? (t == 32 ? 0 : t == 33 ? 1 : t == 34 ? 2 : -1)
                            : (t == 7 ? 0 : t == 8 ? 1 : -1);
            int vcl = h265 ? t < 32 : t >= 1 && t <= 5;
            if (vcl) break;
            if (slot >= 0 && len <= AU_NORM_PS_BYTES && !ps->len[slot]) {
                memcpy(ps->ps[slot], nal, len);
                ps->len[slot] = len;
            }
        }
        i = next;
    }
    /* Only a complete set counts, packed in order. */
    int want = h265 ? 3 : 2;
    for (int s = 0; s < want; ++s)
        if (!ps->len[s]) {
            memset(ps, 0, sizeof *ps);
            return;
        }
    ps->n = want;
}

void stream_tap_feed(int ch, int sub, const vg_au_t *au, const vg_stream_info_t *si, unsigned gen,
                     const gop_cache_t *gop)
{
    tap_stream_t *st = stream_of(ch, sub);
    if (!st) return;
    stream_tap_ps_t ps;
    int new_ps = 0;
    if (au->key) {
        params_of(au, &ps);
        new_ps = ps.n > 0;
    }
    int n = atomic_load(&st->n_taps);
    if (!n && !new_ps && atomic_load(&st->gen) == gen) return;

    pthread_mutex_lock(&st->lock);
    atomic_store(&st->gen, gen);
    st->si = *si;
    st->codec = au->codec;
    if (new_ps && (ps.n != st->ps.n || memcmp(&ps, &st->ps, sizeof ps))) st->ps = ps;
    for (stream_tap_t *t = st->taps; t; t = t->next) {
        if (!t->started) {
            /* The GOP so far, this AU its last: the tap starts at its keyframe. */
            if (gop && gop_cache_ready(gop) && gop->gen == gen && gop->n > 0) {
                for (unsigned i = 0; i < gop->n; ++i) {
                    vg_au_t g;
                    gop_cache_get(gop, i, &g);
                    t->fn(t->user, &g, si, g.codec);
                }
                t->started = 1;
                continue;
            }
            if (!au->key) continue;
            t->started = 1;
        }
        t->fn(t->user, au, si, au->codec);
    }
    pthread_mutex_unlock(&st->lock);
}
