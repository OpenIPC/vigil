/* SPDX-License-Identifier: GPL-3.0-or-later */
/* onvif_events.h — a camera's ONVIF events, as a client: a thread per
 * camera that finds the Events service, holds a pull-point subscription
 * and reports what the notifications say to cam_events.h (origin ONVIF).
 *
 * ONVIF Core Specification, Event handling (WS-BaseNotification pull
 * point): device service GetCapabilities (Category Events: XAddr and
 * WSPullPointSupport; GetServices when a device has no GetCapabilities),
 * then on the events service CreatePullPointSubscription
 * (InitialTerminationTime PT120S) -> SubscriptionReference (an Address and
 * maybe ReferenceParameters, sent back in every later call's header with
 * wsa:To and wsa:Action) and TerminationTime; PullMessages (Timeout PT5S,
 * MessageLimit 64) in a loop; Renew (PT120S) before the subscription runs
 * out; Unsubscribe when done. A call that fails twice in a row, or a Renew
 * the camera refuses, starts over with a new subscription; what the old
 * one said goes on is ended first (cam_events_clear). Every call carries
 * WS-UsernameToken (and HTTP Digest when the camera asks), with the user
 * and password of the camera's URL, in the camera's own clock
 * (disc_ops.h's client).
 *
 * Where the device service is: cameras[].onvif_url, else the address
 * discovery heard (disc_xaddr_of), else http://host/onvif/device_service
 * on port 80, then 8899 (XM's). With events "auto" a camera that has none
 * of them is said once and tried again every ONVIF_EV_RETRY_AUTO s; with
 * events "onvif" it is tried again sooner, each new failure said.
 *
 * A notification (tt:Message in wsnt:NotificationMessage) is a topic, a
 * PropertyOperation (Initialized, Changed, Deleted), Source items (which
 * rule, input or video source) and Data items (its state). The topic says
 * the type (onvif_ev_topic_type); the first boolean Data item (IsMotion,
 * State, IsInside, IsTamper, LogicalState, ...) says start or stop, and
 * without one it is a moment (a line crossed: ObjectId). Each (type,
 * Source) is followed on its own, so two rules of one type hold it until
 * both are over. Topics that are not an alarm vigil shows are said once,
 * when VIGIL_DEBUG is set in the environment.
 */
#ifndef ONVIF_EVENTS_H
#define ONVIF_EVENTS_H

#include <stddef.h>

#define ONVIF_EV_RETRY_AUTO 600     /* s between tries at a camera that had no events */

/* ---- the wire (pure, for tests) ------------------------------------------- */

typedef struct {
    char     topic[128];            /* as written, prefixes and all */
    unsigned type;                  /* CH_EV_*; 0: not an alarm vigil shows */
    int      what;                  /* CAM_EV_START, CAM_EV_STOP, CAM_EV_PULSE */
    int      initialized;           /* PropertyOperation "Initialized": the state at subscribing */
    char     source[192];           /* the Source items, "Name=Value;..." */
    char     state[32];             /* "IsMotion=true", the item that said start/stop; "" none */
} onvif_note_t;

typedef void (*onvif_note_fn)(void *user, const onvif_note_t *n);

/* Every NotificationMessage in a PullMessagesResponse (or a Notify), in
 * order, to `fn`. The number of them, or -1 when xml is neither. */
int onvif_ev_parse(const char *xml, size_t len, onvif_note_fn fn, void *user);

/* The CH_EV_* type of a topic ("tns1:RuleEngine/CellMotionDetector/Motion",
 * any prefixes, any vendor's) with what its Source and Data items say
 * (`items`: their names and values, may be NULL); 0 for one that is not an
 * alarm vigil shows (monitoring, configuration changes, relays). */
unsigned onvif_ev_topic_type(const char *topic, const char *items);

/* A CreatePullPointSubscriptionResponse: the subscription's address, its
 * ReferenceParameters' content as written ("" none) and its life in
 * seconds (TerminationTime - CurrentTime; 0 when it does not say). 0, or
 * -1 when there is no address. */
int onvif_ev_subscription(const char *xml, size_t len, char *addr, size_t acap, char *refs, size_t rcap,
                          long *life);

/* The life a PullMessages or Renew answer gives the subscription, s; -1
 * when it does not say. */
long onvif_ev_life(const char *xml, size_t len);

/* The events service address in a GetCapabilities or GetServices answer.
 * 0, or -1. */
int onvif_ev_service(const char *xml, size_t len, char *out, size_t cap);

/* ---- the watcher ------------------------------------------------------------ */

typedef struct onvif_ev onvif_ev_t;

/* Watch the events of channel `ch`'s (0-based) camera at `cam_url` (its
 * host, user and password), its device service at `onvif_url` (NULL or
 * "": found as above). `quiet`: events "auto" (a camera without them is
 * no news). NULL when the URL has no host or the thread cannot start. */
onvif_ev_t *onvif_ev_start(int ch, const char *cam_url, const char *onvif_url, int quiet);
/* Tell it to stop (it unsubscribes on its way out), without waiting. */
void onvif_ev_signal(onvif_ev_t *w);
/* Wait for it to end and free it (signalled first if it was not). */
void onvif_ev_join(onvif_ev_t *w);

/* vigild's own ONVIF server port: derived addresses on this host with it
 * are not tried (a camera on 127.0.0.1 would find vigild itself). */
void onvif_ev_set_self_port(int port);

#endif /* ONVIF_EVENTS_H */
