/* SPDX-License-Identifier: GPL-3.0-or-later */
/* cam_events.h — what the cameras report (motion, people, a line crossed,
 * video loss, ...: channel.h's CH_EV_* types), from wherever it comes
 * (NETIP alarms, ONVIF events, the API) to whatever wants it: the
 * channels' status (the tiles' icons and alarm frame, the API's status),
 * the events ring (the API's WebSocket), the log, and subscribers (the
 * recorder's event log and motion recording, the Alarm page).
 *
 * Each channel's type is held while any origin says it goes on (a start
 * without its stop yet) or for CHANNEL_EVENT_HOLD s after a momentary
 * report (a pulse: a line crossed, the API's POST). What everyone sees
 * are the held state's edges: a start when a type begins to be held, a
 * stop when it no longer is, whatever number of reports came between.
 * An origin that loses its camera (a connection dropped, a subscription
 * gone) clears what it started: no event stays on for want of its stop.
 * A pulse's stop comes from the ticker (cam_events_start), within a
 * second of its hold running out.
 */
#ifndef CAM_EVENTS_H
#define CAM_EVENTS_H

#include <time.h>

#define CAM_EV_DETAIL 48
#define CAM_EV_SUBS   8

typedef enum { CAM_EV_SRC_API = 0, CAM_EV_SRC_NETIP, CAM_EV_SRC_ONVIF, CAM_EV_SRC_N } cam_ev_src_t;
/* "api", "netip", "onvif". */
const char *cam_ev_src_name(cam_ev_src_t s);

/* What a report says. */
enum { CAM_EV_STOP = 0, CAM_EV_START = 1, CAM_EV_PULSE = 2 };

typedef struct {
    int          channel;           /* 0-based */
    unsigned     type;              /* one CH_EV_* bit */
    int          active;            /* 1: it began; 0: it ended */
    time_t       time;              /* wall clock */
    cam_ev_src_t source;            /* the origin whose report began it */
    char         detail[CAM_EV_DETAIL]; /* the camera's own name for it ("VideoMotion",
                                       "RuleEngine/LineDetector/Crossed"), "" none */
} cam_event_t;

/* Called on the reporting thread (a channel's, the ONVIF watcher's, the
 * API's, the ticker's), one call at a time and in order. It must be
 * quick and must not report events itself; it may read the channels'
 * status and cam_events_held. */
typedef void (*cam_event_fn)(void *user, const cam_event_t *ev);

/* Channel `ch` (0-based) reports `types` (CH_EV_* bits) as `what`
 * (CAM_EV_START, CAM_EV_STOP, CAM_EV_PULSE) from origin `src`. A stop
 * ends only that origin's start (and its pulse). `detail` may be NULL.
 * Any thread. */
void cam_event_report(int ch, unsigned types, int what, cam_ev_src_t src, const char *detail);
/* The same at time `now` (tests). */
void cam_event_report_at(int ch, unsigned types, int what, cam_ev_src_t src, const char *detail, time_t now);

/* Everything origin `src` holds on channel `ch` ends (its starts and its
 * pulses): its camera was lost, or it no longer listens. */
void cam_events_clear(int ch, cam_ev_src_t src);
void cam_events_clear_at(int ch, cam_ev_src_t src, time_t now);

/* The CH_EV_* bits channel `ch` holds at `now`. */
unsigned cam_events_held(int ch, time_t now);

/* Ends the pulses whose hold ran out by `now` (their stop edges). The
 * ticker calls it twice a second; tests call it themselves. */
void cam_events_tick(time_t now);

/* The ticker thread. 0, or -1. */
int  cam_events_start(void);
void cam_events_stop(void);

/* A subscriber: its id (>= 0), or -1 when CAM_EV_SUBS are taken. After
 * unsubscribe returns, `fn` is not running and is not called again. */
int  cam_events_subscribe(cam_event_fn fn, void *user);
void cam_events_unsubscribe(int id);

/* Forget every channel's events, without telling anyone (tests). */
void cam_events_reset(void);

#endif /* CAM_EVENTS_H */
