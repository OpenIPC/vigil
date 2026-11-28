/* SPDX-License-Identifier: GPL-3.0-or-later */
/* cam_events.c — the cameras' events, held and handed on (see cam_events.h). */
#define _DEFAULT_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cam_events.h"
#include "channel.h"
#include "events.h"
#include "log.h"

typedef struct {
    unsigned     active;            /* origins (1 << cam_ev_src_t) with a start and no stop yet */
    time_t       pulse;             /* the last momentary report; 0 none */
    cam_ev_src_t pulse_src;
    int          held;              /* what everyone was last told */
    cam_ev_src_t src;               /* the origin that began it */
    char         detail[CAM_EV_DETAIL];
} slot_t;

typedef struct {
    cam_event_fn fn;
    void        *user;
} sub_t;

/* g_lock guards the slots and is held only briefly; g_tell_lock orders
 * the edges and is held while subscribers run, so a subscriber may read
 * the state (g_lock) but not report. */
static slot_t          g_slot[CHANNEL_MAX][CH_EV_TYPES];
static sub_t           g_sub[CAM_EV_SUBS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_tell_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       g_ticker;
static volatile int    g_ticker_stop;
static int             g_ticker_up;

/* ---- channel.h's names and helpers (here: they need nothing of the
 * channels' threads or the platform) ------------------------------------ */

static const char *const g_ev_names[CH_EV_TYPES] = { "motion", "human", "face", "vehicle", "line_crossing",
                                                     "intrusion", "video_loss", "tamper", "alarm_input", "other" };

const char *ch_event_name(int bit)
{
    return bit >= 0 && bit < CH_EV_TYPES ? g_ev_names[bit] : NULL;
}

int ch_event_bit(const char *name)
{
    for (int b = 0; name && b < CH_EV_TYPES; ++b)
        if (!strcmp(name, g_ev_names[b])) return b;
    return -1;
}

int ch_events_mode(const char *s)
{
    static const char *const names[] = { "auto", "netip", "onvif", "off" };
    for (int i = 0; s && i < 4; ++i)
        if (!strcmp(s, names[i])) return i;
    return -1;
}

unsigned channel_events_held(const time_t last[CH_EV_TYPES], time_t now)
{
    unsigned bits = 0;
    for (int b = 0; b < CH_EV_TYPES; ++b)
        if (last[b] && now >= last[b] && now - last[b] < CHANNEL_EVENT_HOLD) bits |= 1u << b;
    return bits;
}

void channels_report_events(int idx, unsigned bits)
{
    cam_event_report(idx, bits, CAM_EV_PULSE, CAM_EV_SRC_API, NULL);
}

/* ---- the hub ------------------------------------------------------------- */

const char *cam_ev_src_name(cam_ev_src_t s)
{
    static const char *const names[CAM_EV_SRC_N] = { "api", "netip", "onvif" };
    return (unsigned)s < CAM_EV_SRC_N ? names[s] : "?";
}

static int slot_held(const slot_t *s, time_t now)
{
    return s->active || (s->pulse && now >= s->pulse && now - s->pulse < CHANNEL_EVENT_HOLD);
}

/* The edges found under g_lock, told after it is let go. */
typedef struct {
    int         n;
    cam_event_t ev[CH_EV_TYPES * CHANNEL_MAX];
} edges_t;

/* Slot (ch, b) after a change: an edge into `e` if its held state moved. */
static void slot_edge(int ch, int b, time_t now, edges_t *e)
{
    slot_t *s = &g_slot[ch][b];
    int h = slot_held(s, now);
    if (h == s->held) return;
    s->held = h;
    if (!h) s->pulse = 0;
    cam_event_t *ev = &e->ev[e->n++];
    ev->channel = ch;
    ev->type = 1u << b;
    ev->active = h;
    ev->time = now;
    ev->source = s->src;
    snprintf(ev->detail, sizeof ev->detail, "%s", s->detail);
}

/* Tell the log, the events ring and the subscribers; g_tell_lock held. */
static void tell(const edges_t *e)
{
    for (int i = 0; i < e->n; ++i) {
        const cam_event_t *ev = &e->ev[i];
        int b = __builtin_ctz(ev->type);
        const char *name = ch_event_name(b);
        if (ev->detail[0])
            vg_log("CAM%d: %s %s (%s: %s)", ev->channel + 1, name, ev->active ? "start" : "stop",
                   cam_ev_src_name(ev->source), ev->detail);
        else
            vg_log("CAM%d: %s %s (%s)", ev->channel + 1, name, ev->active ? "start" : "stop",
                   cam_ev_src_name(ev->source));
        events_push_camera(ev->channel + 1, ev->type, ev->active, name, ev->detail);
        sub_t subs[CAM_EV_SUBS];
        pthread_mutex_lock(&g_lock);
        memcpy(subs, g_sub, sizeof subs);
        pthread_mutex_unlock(&g_lock);
        for (int k = 0; k < CAM_EV_SUBS; ++k)
            if (subs[k].fn) subs[k].fn(subs[k].user, ev);
    }
}

void cam_event_report_at(int ch, unsigned types, int what, cam_ev_src_t src, const char *detail, time_t now)
{
    if (ch < 0 || ch >= CHANNEL_MAX || (unsigned)src >= CAM_EV_SRC_N) return;
    static edges_t e;                       /* under g_tell_lock */
    pthread_mutex_lock(&g_tell_lock);
    e.n = 0;
    pthread_mutex_lock(&g_lock);
    for (int b = 0; b < CH_EV_TYPES; ++b) {
        if (!(types & (1u << b))) continue;
        slot_t *s = &g_slot[ch][b];
        if (!slot_held(s, now)) {
            /* A new one: who began it and what the camera calls it. */
            s->src = src;
            snprintf(s->detail, sizeof s->detail, "%s", detail ? detail : "");
        }
        if (what == CAM_EV_START) {
            s->active |= 1u << src;
        } else if (what == CAM_EV_STOP) {
            s->active &= ~(1u << src);
            if (s->pulse_src == src) s->pulse = 0;
        } else {
            s->pulse = now;
            s->pulse_src = src;
        }
        slot_edge(ch, b, now, &e);
    }
    pthread_mutex_unlock(&g_lock);
    tell(&e);
    pthread_mutex_unlock(&g_tell_lock);
}

void cam_event_report(int ch, unsigned types, int what, cam_ev_src_t src, const char *detail)
{
    cam_event_report_at(ch, types, what, src, detail, time(NULL));
}

void cam_events_clear_at(int ch, cam_ev_src_t src, time_t now)
{
    if (ch < 0 || ch >= CHANNEL_MAX) return;
    unsigned types = 0;
    pthread_mutex_lock(&g_lock);
    for (int b = 0; b < CH_EV_TYPES; ++b) {
        const slot_t *s = &g_slot[ch][b];
        if ((s->active & (1u << src)) || (s->pulse && s->pulse_src == src)) types |= 1u << b;
    }
    pthread_mutex_unlock(&g_lock);
    if (types) cam_event_report_at(ch, types, CAM_EV_STOP, src, NULL, now);
}

void cam_events_clear(int ch, cam_ev_src_t src)
{
    cam_events_clear_at(ch, src, time(NULL));
}

unsigned cam_events_held(int ch, time_t now)
{
    if (ch < 0 || ch >= CHANNEL_MAX) return 0;
    unsigned bits = 0;
    pthread_mutex_lock(&g_lock);
    for (int b = 0; b < CH_EV_TYPES; ++b)
        if (slot_held(&g_slot[ch][b], now)) bits |= 1u << b;
    pthread_mutex_unlock(&g_lock);
    return bits;
}

void cam_events_tick(time_t now)
{
    static edges_t e;                       /* under g_tell_lock */
    pthread_mutex_lock(&g_tell_lock);
    e.n = 0;
    pthread_mutex_lock(&g_lock);
    for (int ch = 0; ch < CHANNEL_MAX; ++ch)
        for (int b = 0; b < CH_EV_TYPES; ++b)
            if (g_slot[ch][b].held) slot_edge(ch, b, now, &e);
    pthread_mutex_unlock(&g_lock);
    tell(&e);
    pthread_mutex_unlock(&g_tell_lock);
}

static void *ticker(void *arg)
{
    (void)arg;
    while (!g_ticker_stop) {
        usleep(500 * 1000);
        cam_events_tick(time(NULL));
    }
    return NULL;
}

int cam_events_start(void)
{
    if (g_ticker_up) return 0;
    g_ticker_stop = 0;
    if (pthread_create(&g_ticker, NULL, ticker, NULL) != 0) return -1;
    g_ticker_up = 1;
    return 0;
}

void cam_events_stop(void)
{
    if (!g_ticker_up) return;
    g_ticker_stop = 1;
    pthread_join(g_ticker, NULL);
    g_ticker_up = 0;
}

int cam_events_subscribe(cam_event_fn fn, void *user)
{
    int id = -1;
    pthread_mutex_lock(&g_lock);
    for (int k = 0; k < CAM_EV_SUBS && id < 0; ++k)
        if (!g_sub[k].fn) {
            g_sub[k].fn = fn;
            g_sub[k].user = user;
            id = k;
        }
    pthread_mutex_unlock(&g_lock);
    return id;
}

void cam_events_unsubscribe(int id)
{
    if (id < 0 || id >= CAM_EV_SUBS) return;
    /* Not while an edge is being told: after this, `fn` is not running. */
    pthread_mutex_lock(&g_tell_lock);
    pthread_mutex_lock(&g_lock);
    g_sub[id].fn = NULL;
    g_sub[id].user = NULL;
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_unlock(&g_tell_lock);
}

void cam_events_reset(void)
{
    pthread_mutex_lock(&g_tell_lock);
    pthread_mutex_lock(&g_lock);
    memset(g_slot, 0, sizeof g_slot);
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_unlock(&g_tell_lock);
}
