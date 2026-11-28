/* SPDX-License-Identifier: GPL-3.0-or-later */
/* onvif_events.c — ONVIF events from a camera (see onvif_events.h). */
#define _DEFAULT_SOURCE
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "cam_events.h"
#include "channel.h"
#include "disc_ops.h"
#include "disc_proto.h"
#include "discovery.h"
#include "log.h"
#include "onvif_events.h"
#include "onvif_proto.h"
#include "source.h"

#define PULL_SECS      5            /* PullMessages' Timeout: the camera holds the call this long */
#define LIFE_SECS      120          /* the subscription's life, asked for and renewed */
#define RENEW_BEFORE   30           /* s before it runs out */
#define RETRY_MIN      5            /* events "onvif": first retry, s */
#define RETRY_MAX      120
#define CALL_MS        5000         /* any other call */
#define ACTIVE_MAX     32           /* (type, source) pairs followed at once */
#define TOLD_MAX       16           /* topics said once */

/* ---- XML: start tags, their attributes ------------------------------------ */

/* The next start tag at or after *pos: its local name and the range of its
 * attributes [*a0, *a1); *pos past it. 0, or -1 at the end. */
static int next_tag(const char *x, size_t len, size_t *pos, char *local, size_t lcap, size_t *a0, size_t *a1)
{
    for (size_t i = *pos; i < len; ++i) {
        if (x[i] != '<' || i + 1 >= len || x[i + 1] == '/' || x[i + 1] == '?' || x[i + 1] == '!') continue;
        size_t n0 = i + 1, n1 = n0;
        while (n1 < len && !strchr(" \t\r\n/>", x[n1])) ++n1;
        size_t l0 = n0;
        for (size_t k = n0; k < n1; ++k)
            if (x[k] == ':') l0 = k + 1;
        const char *gt = memchr(x + n1, '>', len - n1);
        if (!gt) return -1;
        size_t ll = n1 - l0 < lcap - 1 ? n1 - l0 : lcap - 1;
        memcpy(local, x + l0, ll);
        local[ll] = 0;
        *a0 = n1;
        *a1 = (size_t)(gt - x);
        *pos = *a1 + 1;
        return 0;
    }
    return -1;
}

static void unescape_to(const char *s, size_t n, char *out, size_t cap)
{
    static const struct { const char *e; char c; } E[] = {
        { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' },
    };
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap;) {
        char c = s[i];
        size_t step = 1;
        if (c == '&')
            for (size_t k = 0; k < sizeof E / sizeof E[0]; ++k) {
                size_t el = strlen(E[k].e);
                if (n - i >= el && !memcmp(s + i, E[k].e, el)) {
                    c = E[k].c;
                    step = el;
                    break;
                }
            }
        out[o++] = (unsigned char)c < 0x20 ? ' ' : c;
        i += step;
    }
    out[o] = 0;
}

/* Attribute `name` (its local name) in x[a0, a1). 0, or -1. */
static int attr(const char *x, size_t a0, size_t a1, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    if (cap) out[0] = 0;
    for (size_t i = a0; i + nl + 2 <= a1; ++i) {
        char b = x[i - 1];
        if ((b == ' ' || b == '\t' || b == '\n' || b == '\r' || b == ':') && !memcmp(x + i, name, nl) &&
            x[i + nl] == '=' && (x[i + nl + 1] == '"' || x[i + nl + 1] == '\'')) {
            const char *v = x + i + nl + 2;
            const char *q = memchr(v, x[i + nl + 1], (size_t)(x + a1 - v));
            if (!q) return -1;
            unescape_to(v, (size_t)(q - v), out, cap);
            return 0;
        }
    }
    return -1;
}

static void lower(const char *in, char *out, size_t cap)
{
    size_t k = 0;
    for (; in[k] && k + 1 < cap; ++k) out[k] = (char)tolower((unsigned char)in[k]);
    out[k] = 0;
}

/* ---- topics ------------------------------------------------------------------ */

/* The topic without its prefixes, lower case: "ruleengine/cellmotiondetector/motion". */
static void topic_norm(const char *topic, char *out, size_t cap)
{
    size_t o = 0;
    const char *seg = topic;
    while (*seg && o + 1 < cap) {
        while (*seg == ' ' || *seg == '\t' || *seg == '\n' || *seg == '\r') ++seg;
        size_t n = strcspn(seg, "/");
        const char *colon = memchr(seg, ':', n);
        const char *p = colon ? colon + 1 : seg;
        size_t pl = (size_t)(seg + n - p);
        if (o && o + 1 < cap) out[o++] = '/';
        for (size_t i = 0; i < pl && o + 1 < cap; ++i) out[o++] = (char)tolower((unsigned char)p[i]);
        seg += n;
        if (*seg == '/') ++seg;
    }
    out[o] = 0;
}

unsigned onvif_ev_topic_type(const char *topic, const char *items)
{
    char t[160], it[256] = "";
    if (!topic || !*topic) return 0;
    topic_norm(topic, t, sizeof t);
    if (items) lower(items, it, sizeof it);
    /* Not alarms: the device's health and settings, outputs, the
     * recordings' and the media's own bookkeeping. */
    static const char *const skip[] = { "monitoring/", "relay", "daynight", "configuration", "recordingconfig",
                                        "media/", "ptzcontroller", "profilechanged", "/io/output",
                                        "networkconfig", "operatingtime", "trigger/relay" };
    for (size_t i = 0; i < sizeof skip / sizeof skip[0]; ++i)
        if (strstr(t, skip[i])) return 0;
    /* In this order: a line detector watching for people is a line
     * crossing, a field detector's object inside an intrusion. */
    static const struct { const char *part; unsigned type; } map[] = {
        { "digitalinput", CH_EV_ALARM_IN }, { "/io/port", CH_EV_ALARM_IN }, { "alarmin", CH_EV_ALARM_IN },
        { "virtualinput", CH_EV_ALARM_IN },
        { "signalloss", CH_EV_VIDEO_LOSS }, { "videoloss", CH_EV_VIDEO_LOSS },
        { "tamper", CH_EV_TAMPER }, { "globalscenechange", CH_EV_TAMPER }, { "imagetoo", CH_EV_TAMPER },
        { "blind", CH_EV_TAMPER }, { "scenechange", CH_EV_TAMPER }, { "defocus", CH_EV_TAMPER },
        { "linedetector", CH_EV_LINE }, { "linecross", CH_EV_LINE }, { "crossline", CH_EV_LINE },
        { "/crossed", CH_EV_LINE }, { "tripwire", CH_EV_LINE },
        { "fielddetector", CH_EV_INTRUSION }, { "objectsinside", CH_EV_INTRUSION }, { "intrusion", CH_EV_INTRUSION },
        { "perimeter", CH_EV_INTRUSION }, { "regionentr", CH_EV_INTRUSION },
        { "face", CH_EV_FACE },
        { "people", CH_EV_HUMAN }, { "human", CH_EV_HUMAN }, { "person", CH_EV_HUMAN }, { "pedestrian", CH_EV_HUMAN },
        { "vehicle", CH_EV_VEHICLE }, { "/car", CH_EV_VEHICLE }, { "carshape", CH_EV_VEHICLE },
        { "motion", CH_EV_MOTION }, { "/vmd", CH_EV_MOTION }, { "vmd/", CH_EV_MOTION },
        { "dogcat", CH_EV_OTHER }, { "animal", CH_EV_OTHER }, { "visitor", CH_EV_OTHER },
        { "hardwarefailure", CH_EV_OTHER }, { "storagefailure", CH_EV_OTHER }, { "detectedsound", CH_EV_OTHER },
        { "fire", CH_EV_OTHER }, { "smoke", CH_EV_OTHER }, { "loitering", CH_EV_OTHER },
    };
    unsigned type = 0;
    for (size_t i = 0; i < sizeof map / sizeof map[0] && !type; ++i)
        if (strstr(t, map[i].part)) type = map[i].type;
    /* A generic object detector (Profile M's ObjectDetection, a field or
     * motion rule with a class filter) says what it saw in its items. */
    if (!type || type == CH_EV_OTHER || (type == CH_EV_MOTION && strstr(t, "object"))) {
        if (strstr(it, "=human") || strstr(it, "=person") || strstr(it, "=people")) return CH_EV_HUMAN;
        if (strstr(it, "=vehicle") || strstr(it, "=car") || strstr(it, "=motorvehicle")) return CH_EV_VEHICLE;
        if (strstr(it, "=face")) return CH_EV_FACE;
        if (!type && strstr(t, "objectdetect")) return CH_EV_OTHER;
    }
    return type;
}

/* ---- notifications ---------------------------------------------------------- */

static int bool_of(const char *v, int *b)
{
    if (!strcasecmp(v, "true") || !strcmp(v, "1") || !strcasecmp(v, "active") || !strcasecmp(v, "on")) {
        *b = 1;
        return 0;
    }
    if (!strcasecmp(v, "false") || !strcmp(v, "0") || !strcasecmp(v, "inactive") || !strcasecmp(v, "off")) {
        *b = 0;
        return 0;
    }
    return -1;
}

static void cat(char *dst, size_t cap, const char *a, const char *b)
{
    size_t l = strlen(dst);
    if (l < cap) snprintf(dst + l, cap - l, "%s=%s;", a, b);
}

/* One NotificationMessage, xml[0, len). */
static int note_parse(const char *x, size_t len, onvif_note_t *n)
{
    memset(n, 0, sizeof *n);
    if (disc_xml_text(x, len, "Topic", n->topic, sizeof n->topic, NULL) < 0 || !n->topic[0]) return -1;
    enum { NONE, SRC, KEY, DATA } sec = NONE;
    char local[48], name[48], value[64], items[256] = "", op[24] = "";
    int state = -1, named_state = 0;
    size_t pos = 0, a0, a1;
    while (next_tag(x, len, &pos, local, sizeof local, &a0, &a1) == 0) {
        if (!strcmp(local, "Message") && !op[0]) {
            attr(x, a0, a1, "PropertyOperation", op, sizeof op);
        } else if (!strcmp(local, "Source")) {
            sec = SRC;
        } else if (!strcmp(local, "Key")) {
            sec = KEY;
        } else if (!strcmp(local, "Data")) {
            sec = DATA;
        } else if (!strcmp(local, "SimpleItem") && attr(x, a0, a1, "Name", name, sizeof name) == 0) {
            attr(x, a0, a1, "Value", value, sizeof value);
            cat(items, sizeof items, name, value);
            if (sec == SRC || sec == KEY) {
                cat(n->source, sizeof n->source, name, value);
            } else if (sec == DATA) {
                /* The item that says the state: one of the names the
                 * specification and the vendors use, else the first
                 * boolean (a count or an id is not a state). */
                static const char *const names[] = { "IsMotion", "State", "IsInside", "IsTamper", "LogicalState",
                                                     "IsActive", "Active", "Alarm", "IsAlarm" };
                int b, known = 0;
                for (size_t k = 0; k < sizeof names / sizeof names[0]; ++k)
                    if (!strcasecmp(name, names[k])) known = 1;
                if (bool_of(value, &b) == 0 && (known ? !named_state : state < 0)) {
                    state = b;
                    named_state |= known;
                    snprintf(n->state, sizeof n->state, "%.15s=%.15s", name, value);
                }
            }
        }
    }
    n->initialized = !strcasecmp(op, "Initialized");
    n->type = onvif_ev_topic_type(n->topic, items);
    if (!strcasecmp(op, "Deleted")) n->what = CAM_EV_STOP;
    else n->what = state < 0 ? CAM_EV_PULSE : state ? CAM_EV_START : CAM_EV_STOP;
    /* The state at subscribing, with no state item: nothing happened. */
    if (n->initialized && state < 0) n->what = CAM_EV_STOP;
    return 0;
}

int onvif_ev_parse(const char *xml, size_t len, onvif_note_fn fn, void *user)
{
    size_t c0, c1, after, from = 0;
    if (disc_xml_elem(xml, len, "PullMessagesResponse", 0, NULL, NULL, NULL, NULL) < 0 &&
        disc_xml_elem(xml, len, "Notify", 0, NULL, NULL, NULL, NULL) < 0)
        return -1;
    int count = 0;
    while (disc_xml_elem(xml, len, "NotificationMessage", from, NULL, &c0, &c1, &after) == 0) {
        onvif_note_t n;
        if (note_parse(xml + c0, c1 - c0, &n) == 0) {
            ++count;
            if (fn) fn(user, &n);
        }
        from = after;
    }
    return count;
}

int onvif_ev_subscription(const char *xml, size_t len, char *addr, size_t acap, char *refs, size_t rcap, long *life)
{
    size_t c0, c1, r0, r1;
    if (acap) addr[0] = 0;
    if (rcap) refs[0] = 0;
    *life = 0;
    if (disc_xml_elem(xml, len, "SubscriptionReference", 0, NULL, &c0, &c1, NULL) < 0) return -1;
    if (disc_xml_text(xml + c0, c1 - c0, "Address", addr, acap, NULL) < 0 || !addr[0]) return -1;
    if (disc_xml_elem(xml + c0, c1 - c0, "ReferenceParameters", 0, NULL, &r0, &r1, NULL) == 0) {
        const char *p = xml + c0 + r0;
        size_t n = r1 - r0;
        while (n && isspace((unsigned char)*p)) ++p, --n;
        while (n && isspace((unsigned char)p[n - 1])) --n;
        if (n < rcap) {
            memcpy(refs, p, n);
            refs[n] = 0;
        }
    }
    long l = onvif_ev_life(xml, len);
    *life = l > 0 ? l : 0;
    return 0;
}

long onvif_ev_life(const char *xml, size_t len)
{
    char cur[48], term[48];
    time_t c, t;
    if (disc_xml_text(xml, len, "TerminationTime", term, sizeof term, NULL) < 0 || onvif_time_parse(term, &t) < 0)
        return -1;
    if (disc_xml_text(xml, len, "CurrentTime", cur, sizeof cur, NULL) < 0 || onvif_time_parse(cur, &c) < 0)
        return -1;
    return (long)(t - c);
}

int onvif_ev_service(const char *xml, size_t len, char *out, size_t cap)
{
    size_t c0, c1, after, from = 0;
    if (cap) out[0] = 0;
    /* GetCapabilities: Capabilities/Events/XAddr. */
    if (disc_xml_elem(xml, len, "Events", 0, NULL, &c0, &c1, NULL) == 0 &&
        disc_xml_text(xml + c0, c1 - c0, "XAddr", out, cap, NULL) == 0 && !strncasecmp(out, "http://", 7))
        return 0;
    /* GetServices: the Service whose Namespace is the events'. */
    while (disc_xml_elem(xml, len, "Service", from, NULL, &c0, &c1, &after) == 0) {
        char ns[96];
        if (disc_xml_text(xml + c0, c1 - c0, "Namespace", ns, sizeof ns, NULL) == 0 &&
            !strcmp(ns, "http://www.onvif.org/ver10/events/wsdl") &&
            disc_xml_text(xml + c0, c1 - c0, "XAddr", out, cap, NULL) == 0 && !strncasecmp(out, "http://", 7))
            return 0;
        from = after;
    }
    if (cap) out[0] = 0;
    return -1;
}

/* ---- the watcher ------------------------------------------------------------ */

struct onvif_ev {
    int           ch;
    int           quiet;
    char          host[128];
    char          user[64], pass[64];
    char          onvif_url[160];
    volatile int  stop;
    pthread_t     thread;
    /* What is going on, per type and source (the thread's own). */
    struct { unsigned type; char source[192]; } active[ACTIVE_MAX];
    int           n_active;
    char          told[TOLD_MAX][128];
    int           n_told;
    char          last_err[160];    /* the last failure said */
};

static atomic_int g_self_port;

void onvif_ev_set_self_port(int port) { atomic_store(&g_self_port, port); }

static int debug_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("VIGIL_DEBUG") != NULL;
    return on;
}

static void sleep_stop(onvif_ev_t *w, int secs)
{
    for (int i = 0; i < secs * 10 && !w->stop; ++i) usleep(100 * 1000);
}

static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

/* Say a failure, unless it is the one said last (and, quiet, any after the first). */
static void say(onvif_ev_t *w, const char *msg)
{
    if (!strcmp(w->last_err, msg)) return;
    int first = !w->last_err[0];
    snprintf(w->last_err, sizeof w->last_err, "%s", msg);
    if (w->quiet && !first) return;
    vg_log("CAM%d: ONVIF events: %s%s", w->ch + 1, msg, w->quiet ? " (tried again now and then, not said)" : "");
}

/* The topic as the event's detail: without its prefixes, which say
 * nothing and would not leave room for the rest
 * ("VideoSource/GlobalSceneChange/ImagingService"). */
static void topic_short(const char *topic, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *seg = topic; *seg && o + 1 < cap;) {
        size_t n = strcspn(seg, "/");
        const char *colon = memchr(seg, ':', n);
        const char *p = colon ? colon + 1 : seg;
        if (o && o + 1 < cap) out[o++] = '/';
        for (; p < seg + n && o + 1 < cap; ++p) out[o++] = *p;
        seg += n;
        if (*seg == '/') ++seg;
    }
    out[o] = 0;
}

static void note_cb(void *user, const onvif_note_t *n)
{
    onvif_ev_t *w = user;
    char detail[CAM_EV_DETAIL];
    topic_short(n->topic, detail, sizeof detail);
    if (!n->type) {
        int told = 0;
        for (int i = 0; i < w->n_told; ++i)
            if (!strcmp(w->told[i], n->topic)) told = 1;
        if (!told && w->n_told < TOLD_MAX) snprintf(w->told[w->n_told++], sizeof w->told[0], "%s", n->topic);
        if (!told && debug_on())
            vg_log("CAM%d: ONVIF event %s (%s): not an alarm vigil shows", w->ch + 1, n->topic, n->state);
        return;
    }
    if (n->what == CAM_EV_PULSE) {
        cam_event_report(w->ch, n->type, CAM_EV_PULSE, CAM_EV_SRC_ONVIF, detail);
        return;
    }
    int at = -1, of_type = 0;
    for (int i = 0; i < w->n_active; ++i) {
        if (w->active[i].type != n->type) continue;
        ++of_type;
        if (!strcmp(w->active[i].source, n->source)) at = i;
    }
    if (n->what == CAM_EV_START) {
        if (at >= 0) return;                            /* already on */
        if (w->n_active < ACTIVE_MAX) {
            w->active[w->n_active].type = n->type;
            snprintf(w->active[w->n_active].source, sizeof w->active[0].source, "%s", n->source);
            ++w->n_active;
        }
        if (!of_type) cam_event_report(w->ch, n->type, CAM_EV_START, CAM_EV_SRC_ONVIF, detail);
        return;
    }
    if (at < 0) return;                                 /* was not on */
    w->active[at] = w->active[--w->n_active];
    if (of_type == 1) cam_event_report(w->ch, n->type, CAM_EV_STOP, CAM_EV_SRC_ONVIF, detail);
}

static void active_clear(onvif_ev_t *w)
{
    if (w->n_active) cam_events_clear(w->ch, CAM_EV_SRC_ONVIF);
    w->n_active = 0;
}

/* WS-Addressing for the subscription's own calls. */
static void wsa_header(char *out, size_t cap, const char *action, const char *addr, const char *refs)
{
    char a[320];
    onvif_xml_escape(addr, a, sizeof a);
    snprintf(out, cap, "<wsa:Action>%s</wsa:Action><wsa:To>%s</wsa:To>%s", action, a, refs);
}

/* Find the events service: 0 with `evx` set and `dev` the device service
 * client; -1 with the reason said. */
static int find_events(onvif_ev_t *w, disc_onvif_t *dev, char *evx, size_t cap)
{
    char cand[4][192];
    int n = 0;
    if (w->onvif_url[0]) {
        snprintf(cand[n++], sizeof cand[0], "%s", w->onvif_url);
    } else {
        char x[160];
        if (disc_xaddr_of(w->host, x, sizeof x) == 0 && !strncasecmp(x, "http://", 7))
            snprintf(cand[n++], sizeof cand[0], "%s", x);
        int self = atomic_load(&g_self_port);
        int local = !strncmp(w->host, "127.", 4) || !strcmp(w->host, "localhost");
        if (!(local && self == 80)) snprintf(cand[n++], sizeof cand[0], "http://%s/onvif/device_service", w->host);
        if (!(local && self == 8899))
            snprintf(cand[n++], sizeof cand[0], "http://%s:8899/onvif/device_service", w->host);
    }
    char why[160] = "no address to try";
    for (int i = 0; i < n && !w->stop; ++i) {
        /* An address of its own (onvif_url) is used as given; one the
         * camera gave is reached at the camera's address. */
        if (disc_onvif_init(dev, cand[i], w->onvif_url[0] ? NULL : w->host, w->user, w->pass) < 0) {
            snprintf(why, sizeof why, "%.60s: not an http:// address", cand[i]);
            continue;
        }
        dev->stop = &w->stop;
        disc_onvif_clock(dev);
        disc_http_t r;
        char err[128];
        disc_rc_t rc = disc_onvif_call(dev, NULL, NULL,
                                       "<tds:GetCapabilities><tds:Category>Events</tds:Category></tds:GetCapabilities>",
                                       CALL_MS, &r, "GetCapabilities", err, sizeof err);
        if (rc == DISC_EUNSUPPORTED || rc == DISC_EPROTO) {
            char e2[128];
            disc_rc_t rc2 = disc_onvif_call(dev, NULL, NULL,
                                            "<tds:GetServices><tds:IncludeCapability>false</tds:IncludeCapability>"
                                            "</tds:GetServices>",
                                            CALL_MS, &r, "GetServices", e2, sizeof e2);
            if (rc2 == DISC_OK) rc = DISC_OK;
        }
        if (rc != DISC_OK) {
            snprintf(why, sizeof why, "%.70s: %.80s", cand[i], err);
            /* A refused password is the answer for every address. */
            if (rc == DISC_EAUTH) break;
            continue;
        }
        int ok = onvif_ev_service(r.body, r.len, evx, cap) == 0;
        free(r.body);
        if (ok) return 0;
        snprintf(why, sizeof why, "%.70s: no events service", cand[i]);
    }
    if (!w->stop) say(w, why);
    return -1;
}

/* One subscription, from creating it to losing it (or the stop). */
static void subscription(onvif_ev_t *w, const disc_onvif_t *dev, const char *evx)
{
    disc_onvif_t ev, sub;
    char err[160], addr[256], refs[512], hdr[1024];
    disc_http_t r;
    if (disc_onvif_init(&ev, evx, dev->host, w->user, w->pass) < 0) {
        say(w, "the events service's address is not http://");
        return;
    }
    ev.clock_offset = dev->clock_offset;
    ev.stop = &w->stop;
    char body[256];
    snprintf(body, sizeof body,
             "<tev:CreatePullPointSubscription><tev:InitialTerminationTime>PT%dS</tev:InitialTerminationTime>"
             "</tev:CreatePullPointSubscription>", LIFE_SECS);
    wsa_header(hdr, sizeof hdr, "http://www.onvif.org/ver10/events/wsdl/EventPortType/CreatePullPointSubscriptionRequest",
               evx, "");
    if (disc_onvif_call(&ev, NULL, hdr, body, CALL_MS, &r, "CreatePullPointSubscription", err, sizeof err) != DISC_OK) {
        if (!w->stop) say(w, err);
        return;
    }
    long life = 0;
    int got = onvif_ev_subscription(r.body, r.len, addr, sizeof addr, refs, sizeof refs, &life);
    free(r.body);
    if (got < 0 || disc_onvif_init(&sub, addr, dev->host, w->user, w->pass) < 0) {
        say(w, "CreatePullPointSubscription: no subscription address");
        return;
    }
    sub.clock_offset = dev->clock_offset;
    sub.stop = &w->stop;
    if (life <= 0) life = LIFE_SECS;
    time_t ends = time(NULL) + life;
    w->last_err[0] = 0;
    vg_log("CAM%d: ONVIF events from %s", w->ch + 1, evx);

    int fails = 0, answered = 1;           /* the camera answered the last call */
    while (!w->stop) {
        time_t now = time(NULL);
        if (ends - now <= RENEW_BEFORE) {
            snprintf(body, sizeof body, "<wsnt:Renew><wsnt:TerminationTime>PT%dS</wsnt:TerminationTime></wsnt:Renew>",
                     LIFE_SECS);
            wsa_header(hdr, sizeof hdr, "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/RenewRequest", addr,
                       refs);
            disc_rc_t rc = disc_onvif_call(&sub, NULL, hdr, body, CALL_MS, &r, "Renew", err, sizeof err);
            if (rc != DISC_OK) {
                /* Some cameras renew only by PullMessages, or not at all:
                 * a new subscription then. */
                answered = rc != DISC_ETIMEOUT && rc != DISC_EUNREACHABLE;
                if (!w->stop) say(w, err);
                break;
            }
            long l = onvif_ev_life(r.body, r.len);
            free(r.body);
            ends = time(NULL) + (l > 0 ? l : LIFE_SECS);
        }
        snprintf(body, sizeof body,
                 "<tev:PullMessages><tev:Timeout>PT%dS</tev:Timeout><tev:MessageLimit>64</tev:MessageLimit>"
                 "</tev:PullMessages>", PULL_SECS);
        wsa_header(hdr, sizeof hdr,
                   "http://www.onvif.org/ver10/events/wsdl/PullPointSubscription/PullMessagesRequest", addr, refs);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        disc_rc_t rc = disc_onvif_call(&sub, NULL, hdr, body, (PULL_SECS + 5) * 1000, &r, "PullMessages", err,
                                       sizeof err);
        if (w->stop) {
            if (rc == DISC_OK) free(r.body);
            break;
        }
        if (rc != DISC_OK) {
            answered = rc != DISC_ETIMEOUT && rc != DISC_EUNREACHABLE;
            if (++fails >= 2) {
                say(w, err);
                break;
            }
            sleep_stop(w, 1);
            continue;
        }
        fails = 0;
        answered = 1;
        int n = onvif_ev_parse(r.body, r.len, note_cb, w);
        long l = onvif_ev_life(r.body, r.len);
        free(r.body);
        if (l > 0) ends = time(NULL) + l;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        /* A camera that ignores Timeout answers at once: not a busy loop. */
        if (n <= 0 && t1.tv_sec - t0.tv_sec < 1) sleep_stop(w, 1);
    }
    /* Done with it, politely and quickly (it ends by itself anyway): not
     * given up at the stop, but within 1.5 s, and not tried on a camera
     * that stopped answering (the channel's stop would wait it out). */
    if (answered) {
        sub.stop = NULL;
        wsa_header(hdr, sizeof hdr, "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/UnsubscribeRequest",
                   addr, refs);
        if (disc_onvif_call(&sub, NULL, hdr, "<wsnt:Unsubscribe/>", 1500, &r, "Unsubscribe", err, sizeof err) ==
            DISC_OK)
            free(r.body);
    }
    active_clear(w);
}

static void *watch(void *arg)
{
    onvif_ev_t *w = arg;
    int backoff = RETRY_MIN;
    while (!w->stop) {
        disc_onvif_t dev;
        char evx[192];
        if (find_events(w, &dev, evx, sizeof evx) == 0) {
            subscription(w, &dev, evx);
            if (w->stop) break;
            sleep_stop(w, 2);
            backoff = RETRY_MIN;
            continue;
        }
        if (w->quiet) {
            sleep_stop(w, ONVIF_EV_RETRY_AUTO);
        } else {
            sleep_stop(w, backoff);
            if (backoff < RETRY_MAX) backoff *= 2;
        }
    }
    active_clear(w);
    return NULL;
}

onvif_ev_t *onvif_ev_start(int ch, const char *cam_url, const char *onvif_url, int quiet)
{
    char scheme[16];
    const char *sep = cam_url ? strstr(cam_url, "://") : NULL;
    if (!sep || (size_t)(sep - cam_url) >= sizeof scheme) return NULL;
    memcpy(scheme, cam_url, (size_t)(sep - cam_url));
    scheme[sep - cam_url] = 0;
    onvif_ev_t *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    const char *path;
    int port = 0;
    if (url_split(cam_url, scheme, w->user, sizeof w->user, w->pass, sizeof w->pass, w->host, sizeof w->host, &port,
                  &path) < 0 ||
        !w->host[0]) {
        free(w);
        return NULL;
    }
    if (!w->user[0]) snprintf(w->user, sizeof w->user, "admin");
    w->ch = ch;
    w->quiet = quiet;
    snprintf(w->onvif_url, sizeof w->onvif_url, "%s", onvif_url ? onvif_url : "");
    if (pthread_create(&w->thread, NULL, watch, w) != 0) {
        wipe(w, sizeof *w);
        free(w);
        return NULL;
    }
    return w;
}

void onvif_ev_signal(onvif_ev_t *w)
{
    if (w) w->stop = 1;
}

void onvif_ev_join(onvif_ev_t *w)
{
    if (!w) return;
    w->stop = 1;
    pthread_join(w->thread, NULL);
    wipe(w, sizeof *w);
    free(w);
}
