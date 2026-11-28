/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ptz.c — the channels' PTZ: their state, the worker threads that talk
 * to the cameras, the moves' safety stops and the preset names (ptz.h).
 *
 * Each channel is a slot: what the callers asked for (the move wanted and
 * until when, commands waiting) and what the camera was told, under the
 * slot's lock. Its worker does the network work with the lock released.
 * A camera that changes (ptz_apply) gets a new slot; the old one's worker
 * stops what it had going and ends on its own, and is joined later, so
 * the main thread never waits for a camera. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "config.h"
#include "log.h"
#include "logs.h"
#include "ptz.h"
#include "source.h"

#define OPS_MAX        8
#define RETRY_ERROR_MS 5000         /* an unreachable camera is probed again after */
#define RETRY_NONE_MS  300000       /* ... one without PTZ */
#define STOP_GAP_MS    300          /* between the tries of a stop */
#define EPS            0.02

static const char *const k_state[] = { "off", "probing", "ready", "none", "error" };
static const char *const k_caps[PTZ_N_CAPS] = { "pan_tilt", "zoom", "focus", "focus_auto",
                                                "iris", "presets", "tours", "home" };

const char *ptz_state_name(ptz_state_t s) { return (unsigned)s < 5 ? k_state[s] : "off"; }
const char *ptz_cap_name(int bit) { return bit >= 0 && bit < PTZ_N_CAPS ? k_caps[bit] : ""; }
const char *ptz_backend_name(ptz_backend_t b)
{
    return b == PTZ_BACKEND_NETIP ? "netip" : b == PTZ_BACKEND_ONVIF ? "onvif" : "";
}

const char *ptz_rc_text(int rc)
{
    switch (rc) {
    case PTZ_OK:      return "ok";
    case PTZ_EINVAL:  return "invalid request";
    case PTZ_ENOTSUP: return "the camera cannot do that";
    case PTZ_EBUSY:   return "too many commands waiting";
    case PTZ_ENOPTZ:  return "no PTZ on this channel";
    }
    return "failed";
}

static long long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- the backend for a camera --------------------------------------------------------- */

static const ptz_drv_t *g_drv_netip = &ptz_drv_netip, *g_drv_onvif = &ptz_drv_onvif;

void ptz_set_drivers(const ptz_drv_t *netip, const ptz_drv_t *onvif)
{
    g_drv_netip = netip ? netip : &ptz_drv_netip;
    g_drv_onvif = onvif ? onvif : &ptz_drv_onvif;
}

ptz_backend_t ptz_backend_for(const ptz_camera_t *cam)
{
    if (!cam->url[0] || !cam->enabled || !strcmp(cam->mode, "off")) return PTZ_BACKEND_NONE;
    if (!strcmp(cam->mode, "netip")) return PTZ_BACKEND_NETIP;
    if (!strcmp(cam->mode, "onvif")) return PTZ_BACKEND_ONVIF;
    if (!strncmp(cam->url, "netip://", 8)) return PTZ_BACKEND_NETIP;
    if (!strncmp(cam->url, "rtsp://", 7) || cam->onvif_url[0]) return PTZ_BACKEND_ONVIF;
    return PTZ_BACKEND_NONE;
}

static const ptz_drv_t *drv_of(ptz_backend_t b)
{
    return b == PTZ_BACKEND_NETIP ? g_drv_netip : b == PTZ_BACKEND_ONVIF ? g_drv_onvif : NULL;
}

/* ---- preset names ------------------------------------------------------------------- */

/* Kept per camera, not per channel: a camera moved to another channel
 * keeps its names. The key has no password. */
static pthread_mutex_t g_names_lock = PTHREAD_MUTEX_INITIALIZER;
static cJSON          *g_names;         /* {"<key>": [{"token","name"}, ...]} */
static char            g_names_path[256];

static void names_load(void)
{
    g_names = NULL;
    if (g_names_path[0]) {
        FILE *f = fopen(g_names_path, "rb");
        if (f) {
            char *buf = malloc(256 << 10);
            size_t n = buf ? fread(buf, 1, (256 << 10) - 1, f) : 0;
            fclose(f);
            if (buf) {
                buf[n] = 0;
                cJSON *doc = cJSON_Parse(buf);
                cJSON *cams = cJSON_GetObjectItemCaseSensitive(doc, "cameras");
                if (cJSON_IsObject(cams)) g_names = cJSON_DetachItemViaPointer(doc, cams);
                else if (n) vg_log("ptz: %s is not a preset names file; starting a new one", g_names_path);
                cJSON_Delete(doc);
                free(buf);
            }
        }
    }
    if (!g_names) g_names = cJSON_CreateObject();
}

/* Locked. */
static void names_save(void)
{
    if (!g_names_path[0] || !g_names) return;
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddNumberToObject(doc, "version", 1);
    cJSON_AddItemReferenceToObject(doc, "cameras", g_names);
    char *text = cJSON_Print(doc);
    cJSON_Delete(doc);
    char err[160];
    if (!text || config_write_atomic(g_names_path, text, strlen(text), err, sizeof err) < 0)
        vg_log("ptz: cannot save the preset names to %s: %s", g_names_path, text ? err : "out of memory");
    free(text);
}

/* The camera's list (created when `create`). Locked. */
static cJSON *names_of(const char *key, int create)
{
    if (!g_names || !key[0]) return NULL;
    cJSON *a = cJSON_GetObjectItemCaseSensitive(g_names, key);
    if (!a && create) a = cJSON_AddArrayToObject(g_names, key);
    return cJSON_IsArray(a) ? a : NULL;
}

static cJSON *names_find(cJSON *list, const char *token, int *idx)
{
    int i = 0;
    for (cJSON *e = list ? list->child : NULL; e; e = e->next, ++i) {
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(e, "token");
        if (cJSON_IsString(t) && !strcmp(t->valuestring, token)) {
            if (idx) *idx = i;
            return e;
        }
    }
    return NULL;
}

/* `token` named `name` (NULL: as it is, "" when new). */
static void names_put(const char *key, const char *token, const char *name)
{
    pthread_mutex_lock(&g_names_lock);
    cJSON *list = names_of(key, 1), *e = names_find(list, token, NULL);
    if (list && !e) {
        e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "token", token);
        cJSON_AddStringToObject(e, "name", name ? name : "");
        cJSON_AddItemToArray(list, e);
    } else if (e && name) {
        cJSON_ReplaceItemInObjectCaseSensitive(e, "name", cJSON_CreateString(name));
    }
    names_save();
    pthread_mutex_unlock(&g_names_lock);
}

static void names_drop(const char *key, const char *token)
{
    pthread_mutex_lock(&g_names_lock);
    int idx;
    cJSON *list = names_of(key, 0);
    if (names_find(list, token, &idx)) {
        cJSON_DeleteItemFromArray(list, idx);
        names_save();
    }
    pthread_mutex_unlock(&g_names_lock);
}

static int token_cmp(const void *a, const void *b)
{
    const ptz_preset_t *x = a, *y = b;
    char *ex, *ey;
    long nx = strtol(x->token, &ex, 10), ny = strtol(y->token, &ey, 10);
    if (!*ex && !*ey && x->token[0] && y->token[0]) return nx < ny ? -1 : nx > ny;
    return strcmp(x->token, y->token);
}

/* The list shown: the camera's own (n >= 0), with the names given here
 * over theirs, or (n < 0) the ones vigild keeps. */
static int names_merge(const char *key, const ptz_preset_t *cam, int n, ptz_preset_t *out)
{
    int k = 0;
    pthread_mutex_lock(&g_names_lock);
    cJSON *list = names_of(key, 0);
    if (n >= 0) {
        for (int i = 0; i < n && k < PTZ_PRESETS_MAX; ++i) {
            out[k] = cam[i];
            const cJSON *e = names_find(list, cam[i].token, NULL);
            const cJSON *nm = cJSON_GetObjectItemCaseSensitive(e, "name");
            if (cJSON_IsString(nm) && nm->valuestring[0]) snprintf(out[k].name, sizeof out[k].name, "%s", nm->valuestring);
            ++k;
        }
    } else {
        for (cJSON *e = list ? list->child : NULL; e && k < PTZ_PRESETS_MAX; e = e->next) {
            const cJSON *t = cJSON_GetObjectItemCaseSensitive(e, "token");
            const cJSON *nm = cJSON_GetObjectItemCaseSensitive(e, "name");
            if (!cJSON_IsString(t)) continue;
            snprintf(out[k].token, sizeof out[k].token, "%s", t->valuestring);
            snprintf(out[k].name, sizeof out[k].name, "%s", cJSON_IsString(nm) ? nm->valuestring : "");
            ++k;
        }
    }
    pthread_mutex_unlock(&g_names_lock);
    qsort(out, (size_t)k, sizeof *out, token_cmp);
    return k;
}

/* ---- slots ---------------------------------------------------------------------------- */

enum { OP_PRESET, OP_RENAME, OP_HOME, OP_TOUR, OP_FOCUS_AUTO };

typedef struct {
    int  kind, sub;                 /* sub: PTZ_PRESET_*, or start (tour) */
    int  n;                         /* tour */
    char token[PTZ_TOKEN_LEN];
    char name[PTZ_NAME_LEN];
    int  has_name;
} ptz_op_t;

typedef struct ptz_slot {
    pthread_mutex_t  lock;
    pthread_cond_t   wake;
    int              ch;
    ptz_camera_t     cam;
    ptz_backend_t    backend;
    const ptz_drv_t *drv;
    char             key[200];       /* the preset names' */
    volatile int     quit;
    int              th_up, done;
    pthread_t        th;

    ptz_info_t       info;
    int              probe_wanted;
    long long        probed_ms;      /* the last probe ended */
    ptz_preset_t     cam_presets[PTZ_PRESETS_MAX];
    int              n_cam_presets;  /* -1: the camera keeps no list */

    double           want[PTZ_AXES], sent[PTZ_AXES];
    long long        until_ms;       /* the move wanted lasts until */
    long long        sent_ms;        /* the move in force was last sent */
    long long        last_cmd_ms;
    ptz_op_t         q[OPS_MAX];
    int              qn;
    ptz_op_t         running;           /* the op being sent (out of q, its result not in info yet) */
    int              has_running;
    struct ptz_slot *next;           /* the retired list */
} ptz_slot_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static ptz_slot_t     *g_slot[PTZ_CH_MAX];
static ptz_slot_t     *g_retired;
static int             g_started;

static int any(const double v[PTZ_AXES])
{
    for (int i = 0; i < PTZ_AXES; ++i)
        if (fabs(v[i]) > EPS) return 1;
    return 0;
}

static int same(const double a[PTZ_AXES], const double b[PTZ_AXES])
{
    for (int i = 0; i < PTZ_AXES; ++i)
        if (fabs(a[i] - b[i]) > EPS) return 0;
    return 1;
}

/* Slot locked. */
static void bump(ptz_slot_t *s) { ++s->info.gen; }

static void fail(ptz_slot_t *s, const char *what, const char *err)
{
    snprintf(s->info.error, sizeof s->info.error, "%s%s%s", what, what[0] ? ": " : "", err);
    ++s->info.error_seq;
    bump(s);
}

static void cam_label(const ptz_slot_t *s, char *buf, size_t cap)
{
    snprintf(buf, cap, "CAM%d (%s)", s->ch + 1, ptz_backend_name(s->backend));
}

/* The presets shown, from the camera's list and the names. Slot locked. */
static void presets_refresh(ptz_slot_t *s)
{
    s->info.n_presets = names_merge(s->key, s->cam_presets, s->n_cam_presets, s->info.presets);
    bump(s);
}

static void camera_key(ptz_slot_t *s)
{
    char user[64], pass[64], host[128];
    const char *path = "";
    int port = 0;
    const char *url = s->cam.url;
    s->key[0] = 0;
    if (s->backend == PTZ_BACKEND_ONVIF && s->cam.onvif_url[0]) url = s->cam.onvif_url;
    const char *scheme = !strncmp(url, "netip://", 8) ? "netip" : !strncmp(url, "rtsp://", 7) ? "rtsp" : "http";
    if (url_split(url, scheme, user, sizeof user, pass, sizeof pass, host, sizeof host, &port, &path) < 0) return;
    char v[16] = "0";
    if (s->backend == PTZ_BACKEND_NETIP) url_query(path, "ch", v, sizeof v);
    snprintf(s->key, sizeof s->key, "%s:%s:%d:%s", ptz_backend_name(s->backend), host,
             s->backend == PTZ_BACKEND_NETIP && strcmp(scheme, "netip") ? 34567 : port, v);
    memset(pass, 0, sizeof pass);
}

/* ---- the worker ----------------------------------------------------------------------- */

static void run_op(ptz_slot_t *s, void *l, const ptz_op_t *op)
{
    char err[128] = "", out[PTZ_TOKEN_LEN] = "", who[48];
    const ptz_drv_t *d = s->drv;
    int rc = 0;
    cam_label(s, who, sizeof who);
    switch (op->kind) {
    case OP_PRESET:
        rc = d->preset ? d->preset(l, op->sub, op->token, op->has_name ? op->name : NULL, out, sizeof out, err,
                                   sizeof err)
                       : PTZ_ENOTSUP;
        if (rc == 0 && op->sub == PTZ_PRESET_SET && !out[0]) snprintf(out, sizeof out, "%s", op->token);
        vg_log("ptz: %s: preset %s %s%s", who,
               op->sub == PTZ_PRESET_GOTO ? "goto" : op->sub == PTZ_PRESET_SET ? "set" : "remove",
               out[0] ? out : op->token, rc == 0 ? "" : " failed");
        if (rc == 0 && op->sub == PTZ_PRESET_SET) names_put(s->key, out, op->has_name ? op->name : NULL);
        if (rc == 0 && op->sub == PTZ_PRESET_REMOVE) names_drop(s->key, op->token);
        if (rc == 0 && op->sub != PTZ_PRESET_GOTO && s->n_cam_presets >= 0 && d->presets) {
            ptz_preset_t p[PTZ_PRESETS_MAX];
            int n = 0;
            char e2[96];
            if (d->presets(l, p, &n, e2, sizeof e2) == 0 && n >= 0) {
                pthread_mutex_lock(&s->lock);
                memcpy(s->cam_presets, p, sizeof p);
                s->n_cam_presets = n;
                pthread_mutex_unlock(&s->lock);
            }
        }
        break;
    case OP_RENAME:
        names_put(s->key, op->token, op->name);
        break;
    case OP_HOME:
        rc = d->home ? d->home(l, err, sizeof err) : PTZ_ENOTSUP;
        vg_log("ptz: %s: home%s", who, rc == 0 ? "" : " failed");
        break;
    case OP_TOUR:
        rc = d->tour ? d->tour(l, op->n, op->sub, err, sizeof err) : PTZ_ENOTSUP;
        vg_log("ptz: %s: tour %d %s%s", who, op->n, op->sub ? "start" : "stop", rc == 0 ? "" : " failed");
        break;
    case OP_FOCUS_AUTO:
        rc = d->focus_auto ? d->focus_auto(l, err, sizeof err) : PTZ_ENOTSUP;
        break;
    }
    pthread_mutex_lock(&s->lock);
    if (rc == PTZ_ENOTSUP) fail(s, "", "the camera cannot do that");
    else if (rc != 0) fail(s, op->kind == OP_PRESET ? "preset" : op->kind == OP_HOME ? "home" : "tour", err);
    presets_refresh(s);
    pthread_mutex_unlock(&s->lock);
}

static void probe(ptz_slot_t *s, void *l)
{
    unsigned caps = 0;
    int tours = 0, n = -1;
    ptz_preset_t p[PTZ_PRESETS_MAX];
    char err[128] = "", who[48];
    int rc = s->drv->probe(l, &caps, &tours, p, &n, err, sizeof err);
    cam_label(s, who, sizeof who);
    pthread_mutex_lock(&s->lock);
    s->probed_ms = mono_ms();
    if (rc == 0) {
        s->info.state = PTZ_READY;
        s->info.caps = caps;
        s->info.tours = tours > PTZ_TOURS_MAX ? PTZ_TOURS_MAX : tours;
        s->info.error[0] = 0;
        memcpy(s->cam_presets, p, sizeof p);
        s->n_cam_presets = n;
        presets_refresh(s);
        char list[128] = "";
        for (int b = 0; b < PTZ_N_CAPS; ++b)
            if (caps & (1u << b))
                snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", list[0] ? " " : "", ptz_cap_name(b));
        vg_log("ptz: %s: ready (%s)", who, list);
    } else {
        s->info.state = rc == 1 ? PTZ_NONE : PTZ_ERROR;
        fail(s, "", err[0] ? err : "no answer");
        vg_log("ptz: %s: %s: %s", who, rc == 1 ? "no PTZ" : "unavailable", s->info.error);
    }
    bump(s);
    pthread_mutex_unlock(&s->lock);
}

static void describe(const double v[PTZ_AXES], char *buf, size_t cap)
{
    if (!any(v)) {
        snprintf(buf, cap, "stop");
        return;
    }
    snprintf(buf, cap, "pan %.2f tilt %.2f zoom %.2f focus %.2f iris %.2f", v[0], v[1], v[2], v[3], v[4]);
}

/* Send the move wanted (to), from the one in force. A stop that fails is
 * tried again PTZ_STOP_TRIES times, then the connection is dropped. */
static void send_motion(ptz_slot_t *s, void *l, const double from[PTZ_AXES], const double to[PTZ_AXES],
                        int expired)
{
    char err[128] = "", who[48], what[96];
    int stop = !any(to), rc = -1;
    cam_label(s, who, sizeof who);
    for (int tries = 0; tries < (stop ? PTZ_STOP_TRIES : 1); ++tries) {
        if (tries) {
            struct timespec ts = { 0, STOP_GAP_MS * 1000000L };
            nanosleep(&ts, NULL);
        }
        rc = s->drv->motion(l, from, to, err, sizeof err);
        if (rc == 0) break;
    }
    describe(to, what, sizeof what);
    if (!same(from, to) || rc != 0)
        vg_log("ptz: %s: %s%s%s%s", who, what, expired ? " (hold ran out)" : "", rc ? " failed: " : "", rc ? err : "");
    if (rc != 0 && stop && s->drv->hangup) {
        vg_log("ptz: %s: the stop did not get through; hanging up", who);
        s->drv->hangup(l);
    }
    pthread_mutex_lock(&s->lock);
    if (rc == 0) {
        memcpy(s->sent, to, sizeof s->sent);
    } else {
        fail(s, "move", err);
        /* Whatever the camera took, it is to be stopped: what was asked
         * counts as sent, and nothing is wanted any more. */
        if (!stop) memcpy(s->sent, to, sizeof s->sent);
        else memset(s->sent, 0, sizeof s->sent);
        memset(s->want, 0, sizeof s->want);
    }
    s->sent_ms = s->last_cmd_ms = mono_ms();
    s->info.moving = any(s->sent);
    bump(s);
    pthread_mutex_unlock(&s->lock);
}

static void *worker(void *arg)
{
    ptz_slot_t *s = arg;
    char err[128] = "";
    void *l = s->drv->create(&s->cam, &s->quit, err, sizeof err);
    pthread_mutex_lock(&s->lock);
    if (!l) {
        s->info.state = PTZ_ERROR;
        fail(s, "", err[0] ? err : "cannot reach the camera this way");
        s->probed_ms = mono_ms();
        s->probe_wanted = 0;
    }
    s->last_cmd_ms = mono_ms();
    while (l) {
        long long now = mono_ms();
        int moving = any(s->sent);
        if (s->quit) {
            if (moving) {
                double from[PTZ_AXES], zero[PTZ_AXES] = { 0 };
                memcpy(from, s->sent, sizeof from);
                pthread_mutex_unlock(&s->lock);
                send_motion(s, l, from, zero, 0);
                pthread_mutex_lock(&s->lock);
            }
            break;
        }
        if (s->probe_wanted) {
            s->probe_wanted = 0;
            pthread_mutex_unlock(&s->lock);
            probe(s, l);
            pthread_mutex_lock(&s->lock);
            continue;
        }
        if (s->info.state != PTZ_READY) {
            s->qn = 0;
            memset(s->want, 0, sizeof s->want);
        }
        int expired = any(s->want) && now >= s->until_ms;
        if (expired) memset(s->want, 0, sizeof s->want);
        if (!same(s->want, s->sent) ||
            (moving && s->drv->refresh_ms > 0 && now - s->sent_ms >= s->drv->refresh_ms)) {
            double from[PTZ_AXES], to[PTZ_AXES];
            memcpy(from, s->sent, sizeof from);
            memcpy(to, s->want, sizeof to);
            pthread_mutex_unlock(&s->lock);
            send_motion(s, l, from, to, expired);
            pthread_mutex_lock(&s->lock);
            continue;
        }
        if (s->qn) {
            ptz_op_t op = s->q[0];
            memmove(s->q, s->q + 1, sizeof s->q[0] * (size_t)--s->qn);
            s->running = op;
            s->has_running = 1;
            pthread_mutex_unlock(&s->lock);
            run_op(s, l, &op);
            pthread_mutex_lock(&s->lock);
            s->has_running = 0;
            s->last_cmd_ms = mono_ms();
            continue;
        }
        /* Nothing to send: until the move runs out, the next refresh, or
         * a second for the link's upkeep. */
        long long next = now + 1000;
        if (any(s->want) && s->until_ms < next) next = s->until_ms;
        if (moving && s->drv->refresh_ms > 0 && s->sent_ms + s->drv->refresh_ms < next)
            next = s->sent_ms + s->drv->refresh_ms;
        if (!moving && s->drv->idle) {
            long long idle = now - s->last_cmd_ms;
            pthread_mutex_unlock(&s->lock);
            s->drv->idle(l, idle);
            pthread_mutex_lock(&s->lock);
            if (s->quit || s->probe_wanted || s->qn || !same(s->want, s->sent)) continue;
            now = mono_ms();
        }
        if (next > now) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            long long ns = ts.tv_nsec + (next - now) * 1000000LL;
            ts.tv_sec += (time_t)(ns / 1000000000LL);
            ts.tv_nsec = (long)(ns % 1000000000LL);
            pthread_cond_timedwait(&s->wake, &s->lock, &ts);
        }
    }
    s->done = 1;
    s->info.moving = 0;
    pthread_mutex_unlock(&s->lock);
    if (l) s->drv->destroy(l);
    return NULL;
}

/* g_lock held. */
static int slot_run(ptz_slot_t *s)
{
    if (s->th_up) return 0;
    if (pthread_create(&s->th, NULL, worker, s) != 0) return -1;
    s->th_up = 1;
    return 0;
}

static ptz_slot_t *slot_new(int ch, const ptz_camera_t *cam)
{
    ptz_slot_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->wake, NULL);
    s->ch = ch;
    s->cam = *cam;
    s->backend = ptz_backend_for(cam);
    s->drv = drv_of(s->backend);
    s->info.backend = s->backend;
    s->info.state = PTZ_OFF;
    s->n_cam_presets = -1;
    camera_key(s);
    return s;
}

static void slot_free(ptz_slot_t *s)
{
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->wake);
    memset(&s->cam, 0, sizeof s->cam);
    free(s);
}

/* g_lock held: its worker stops what it has going and ends. */
static void slot_retire(ptz_slot_t *s)
{
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    s->quit = 1;
    memset(s->want, 0, sizeof s->want);
    pthread_cond_signal(&s->wake);
    pthread_mutex_unlock(&s->lock);
    s->next = g_retired;
    g_retired = s;
}

/* g_lock held: the retired slots whose worker ended (`all`: waiting for
 * every one). */
static void reap(int all)
{
    for (ptz_slot_t **pp = &g_retired; *pp;) {
        ptz_slot_t *s = *pp;
        pthread_mutex_lock(&s->lock);
        int done = s->done || !s->th_up;
        pthread_mutex_unlock(&s->lock);
        if (!done && !all) {
            pp = &s->next;
            continue;
        }
        if (s->th_up) pthread_join(s->th, NULL);
        *pp = s->next;
        slot_free(s);
    }
}

int ptz_start(const char *names_path)
{
    pthread_mutex_lock(&g_names_lock);
    snprintf(g_names_path, sizeof g_names_path, "%s", names_path ? names_path : "");
    cJSON_Delete(g_names);
    names_load();
    pthread_mutex_unlock(&g_names_lock);
    pthread_mutex_lock(&g_lock);
    g_started = 1;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int cam_same(const ptz_camera_t *a, const ptz_camera_t *b)
{
    return !strcmp(a->url, b->url) && !strcmp(a->onvif_url, b->onvif_url) && !strcmp(a->mode, b->mode) &&
           !a->enabled == !b->enabled;
}

void ptz_apply(const ptz_camera_t *cams, int n)
{
    pthread_mutex_lock(&g_lock);
    for (int ch = 0; ch < PTZ_CH_MAX; ++ch) {
        static const ptz_camera_t none = { .mode = "off" };
        const ptz_camera_t *c = ch < n ? &cams[ch] : &none;
        if (g_slot[ch] && cam_same(&g_slot[ch]->cam, c)) continue;
        slot_retire(g_slot[ch]);
        g_slot[ch] = slot_new(ch, c);
    }
    reap(0);
    pthread_mutex_unlock(&g_lock);
}

void ptz_shutdown(void)
{
    pthread_mutex_lock(&g_lock);
    for (int ch = 0; ch < PTZ_CH_MAX; ++ch) {
        slot_retire(g_slot[ch]);
        g_slot[ch] = NULL;
    }
    reap(1);
    g_started = 0;
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_lock(&g_names_lock);
    cJSON_Delete(g_names);
    g_names = NULL;
    pthread_mutex_unlock(&g_names_lock);
}

/* The slot of `ch` locked (with g_lock), its worker running when it has a
 * backend; NULL for no such channel. */
static ptz_slot_t *slot_lock(int ch)
{
    if (ch < 0 || ch >= PTZ_CH_MAX) return NULL;
    pthread_mutex_lock(&g_lock);
    ptz_slot_t *s = g_slot[ch];
    if (!s) {
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    pthread_mutex_lock(&s->lock);
    return s;
}

static void slot_unlock(ptz_slot_t *s)
{
    pthread_cond_signal(&s->wake);
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_unlock(&g_lock);
}

/* Slot locked: probe now if it is time. */
static void maybe_probe(ptz_slot_t *s)
{
    if (!s->drv || s->quit) return;
    long long since = mono_ms() - s->probed_ms;
    int again = s->info.state == PTZ_OFF || (s->info.state == PTZ_ERROR && since >= RETRY_ERROR_MS) ||
                (s->info.state == PTZ_NONE && since >= RETRY_NONE_MS);
    if (!again || s->probe_wanted) return;
    if (!s->th_up && slot_run(s) < 0) {
        fail(s, "", "cannot start a thread");
        s->info.state = PTZ_ERROR;
        s->probed_ms = mono_ms();
        return;
    }
    if (s->info.state == PTZ_OFF) s->info.state = PTZ_PROBING;
    s->probe_wanted = 1;
    bump(s);
}

int ptz_info(int ch, ptz_info_t *out)
{
    ptz_slot_t *s = slot_lock(ch);
    if (!s) {
        if (ch < 0 || ch >= PTZ_CH_MAX) return -1;
        memset(out, 0, sizeof *out);
        return 0;
    }
    maybe_probe(s);
    *out = s->info;
    slot_unlock(s);
    return 0;
}

/* The slot ready for a command needing `cap` (0: any), locked; NULL with
 * *rc. */
static ptz_slot_t *ready(int ch, unsigned cap, int *rc)
{
    ptz_slot_t *s = slot_lock(ch);
    if (!s) {
        *rc = ch < 0 || ch >= PTZ_CH_MAX ? PTZ_EINVAL : PTZ_ENOPTZ;
        return NULL;
    }
    if (!s->drv || s->info.state == PTZ_NONE || s->info.state == PTZ_OFF) {
        maybe_probe(s);
        *rc = !s->drv || s->info.state == PTZ_NONE ? PTZ_ENOPTZ : PTZ_OK;
        if (*rc) {
            slot_unlock(s);
            return NULL;
        }
    }
    if (s->info.state == PTZ_ERROR) {
        maybe_probe(s);
        if (s->info.state == PTZ_ERROR && !s->probe_wanted) {
            *rc = PTZ_ENOPTZ;
            slot_unlock(s);
            return NULL;
        }
    }
    /* While probing, a command waits for it; what the camera turns out
     * not to do is refused then (run_op). */
    if (cap && s->info.state == PTZ_READY && !(s->info.caps & cap)) {
        *rc = PTZ_ENOTSUP;
        slot_unlock(s);
        return NULL;
    }
    if (!s->th_up && slot_run(s) < 0) {
        *rc = PTZ_EBUSY;
        slot_unlock(s);
        return NULL;
    }
    *rc = PTZ_OK;
    return s;
}

static double clamp1(double v) { return isnan(v) ? 0 : v < -1 ? -1 : v > 1 ? 1 : v; }

static int hold_of(int ms) { return ms <= 0 ? PTZ_HOLD_MS : ms < 100 ? 100 : ms > PTZ_HOLD_MAX_MS ? PTZ_HOLD_MAX_MS : ms; }

static int want(int ch, unsigned cap, const double v[PTZ_AXES], int hold_ms)
{
    int rc;
    ptz_slot_t *s = ready(ch, any(v) ? cap : 0, &rc);
    if (!s) return rc;
    for (int i = 0; i < PTZ_AXES; ++i) s->want[i] = clamp1(v[i]);
    s->until_ms = mono_ms() + hold_of(hold_ms);
    slot_unlock(s);
    return PTZ_OK;
}

int ptz_move(int ch, double pan, double tilt, double zoom, int hold_ms)
{
    double v[PTZ_AXES] = { pan, tilt, zoom, 0, 0 };
    unsigned cap = (fabs(pan) > EPS || fabs(tilt) > EPS ? PTZ_CAP_PAN_TILT : 0) | (fabs(zoom) > EPS ? PTZ_CAP_ZOOM : 0);
    /* a camera that can do one of them moves that one */
    int rc;
    ptz_slot_t *s = ready(ch, 0, &rc);
    if (!s) return rc;
    unsigned have = s->info.state == PTZ_READY ? s->info.caps : cap;
    slot_unlock(s);
    if (cap && !(have & cap)) return PTZ_ENOTSUP;
    if (!(have & PTZ_CAP_PAN_TILT)) v[0] = v[1] = 0;
    if (!(have & PTZ_CAP_ZOOM)) v[2] = 0;
    return want(ch, 0, v, hold_ms);
}

int ptz_focus(int ch, double dir, int hold_ms)
{
    double v[PTZ_AXES] = { 0, 0, 0, dir, 0 };
    return want(ch, PTZ_CAP_FOCUS, v, hold_ms);
}

int ptz_iris(int ch, double dir, int hold_ms)
{
    double v[PTZ_AXES] = { 0, 0, 0, 0, dir };
    return want(ch, PTZ_CAP_IRIS, v, hold_ms);
}

int ptz_stop(int ch)
{
    int rc;
    ptz_slot_t *s = slot_lock(ch);
    if (!s) return ch < 0 || ch >= PTZ_CH_MAX ? PTZ_EINVAL : PTZ_OK;
    memset(s->want, 0, sizeof s->want);
    rc = PTZ_OK;
    slot_unlock(s);
    return rc;
}

static int queue(int ch, unsigned cap, const ptz_op_t *op)
{
    int rc;
    ptz_slot_t *s = ready(ch, cap, &rc);
    if (!s) return rc;
    if (s->qn == OPS_MAX) {
        rc = PTZ_EBUSY;
    } else {
        s->q[s->qn++] = *op;
        rc = PTZ_OK;
    }
    slot_unlock(s);
    return rc;
}

static int token_ok(const char *t) { return t && t[0] && strlen(t) < PTZ_TOKEN_LEN && !strpbrk(t, "<>&\"'"); }

static int name_ok(const char *n)
{
    if (!n) return 1;
    if (strlen(n) >= PTZ_NAME_LEN) return 0;
    for (const unsigned char *p = (const unsigned char *)n; *p; ++p)
        if (*p < 0x20) return 0;
    return 1;
}

int ptz_preset_goto(int ch, const char *token)
{
    if (!token_ok(token)) return PTZ_EINVAL;
    ptz_op_t op = { .kind = OP_PRESET, .sub = PTZ_PRESET_GOTO };
    snprintf(op.token, sizeof op.token, "%s", token);
    return queue(ch, PTZ_CAP_PRESETS, &op);
}

/* NETIP: the lowest preset number not in the list. Slot locked. */
static void free_number(const ptz_slot_t *s, char *out, size_t cap)
{
    for (int n = 1; n <= 255; ++n) {
        char t[8];
        snprintf(t, sizeof t, "%d", n);
        int used = 0;
        for (int i = 0; i < s->info.n_presets && !used; ++i) used = !strcmp(s->info.presets[i].token, t);
        for (int i = 0; i < s->qn && !used; ++i)
            used = s->q[i].kind == OP_PRESET && s->q[i].sub == PTZ_PRESET_SET && !strcmp(s->q[i].token, t);
        /* Nor the one being set now: out of the queue, not in the list yet. */
        if (!used && s->has_running)
            used = s->running.kind == OP_PRESET && s->running.sub == PTZ_PRESET_SET && !strcmp(s->running.token, t);
        if (!used) {
            snprintf(out, cap, "%s", t);
            return;
        }
    }
    out[0] = 0;
}

int ptz_preset_set(int ch, const char *token, const char *name)
{
    if ((token && token[0] && !token_ok(token)) || !name_ok(name)) return PTZ_EINVAL;
    ptz_op_t op = { .kind = OP_PRESET, .sub = PTZ_PRESET_SET, .has_name = name != NULL };
    snprintf(op.token, sizeof op.token, "%s", token ? token : "");
    snprintf(op.name, sizeof op.name, "%s", name ? name : "");
    int rc;
    ptz_slot_t *s = ready(ch, PTZ_CAP_PRESETS, &rc);
    if (!s) return rc;
    if (s->backend == PTZ_BACKEND_NETIP) {
        char *end;
        long n = op.token[0] ? strtol(op.token, &end, 10) : 0;
        if (op.token[0] && (*end || n < 1 || n > 255)) rc = PTZ_EINVAL;
        else if (!op.token[0]) free_number(s, op.token, sizeof op.token);
        if (!rc && !op.token[0]) rc = PTZ_EBUSY;
    }
    if (!rc && s->qn == OPS_MAX) rc = PTZ_EBUSY;
    if (!rc) s->q[s->qn++] = op;
    slot_unlock(s);
    if (!rc)
        oplog_pushf(NULL, NULL, "ptz_preset_set", "CAM%d preset %s%s%s", ch + 1, op.token, op.name[0] ? ": " : "",
                    op.name);
    return rc;
}

int ptz_preset_remove(int ch, const char *token)
{
    if (!token_ok(token)) return PTZ_EINVAL;
    ptz_op_t op = { .kind = OP_PRESET, .sub = PTZ_PRESET_REMOVE };
    snprintf(op.token, sizeof op.token, "%s", token);
    int rc = queue(ch, PTZ_CAP_PRESETS, &op);
    if (!rc) oplog_pushf(NULL, NULL, "ptz_preset_remove", "CAM%d preset %s", ch + 1, token);
    return rc;
}

int ptz_preset_rename(int ch, const char *token, const char *name)
{
    if (!token_ok(token) || !name || !name_ok(name)) return PTZ_EINVAL;
    ptz_op_t op = { .kind = OP_RENAME, .has_name = 1 };
    snprintf(op.token, sizeof op.token, "%s", token);
    snprintf(op.name, sizeof op.name, "%s", name);
    int rc = queue(ch, PTZ_CAP_PRESETS, &op);
    if (!rc) oplog_pushf(NULL, NULL, "ptz_preset_rename", "CAM%d preset %s: %s", ch + 1, token, name);
    return rc;
}

int ptz_home(int ch)
{
    ptz_op_t op = { .kind = OP_HOME };
    return queue(ch, PTZ_CAP_HOME, &op);
}

int ptz_focus_auto(int ch)
{
    ptz_op_t op = { .kind = OP_FOCUS_AUTO };
    return queue(ch, PTZ_CAP_FOCUS_AUTO, &op);
}

int ptz_tour(int ch, int n, int start)
{
    if (n < 1 || n > PTZ_TOURS_MAX) return PTZ_EINVAL;
    ptz_op_t op = { .kind = OP_TOUR, .n = n, .sub = start != 0 };
    return queue(ch, PTZ_CAP_TOURS, &op);
}
