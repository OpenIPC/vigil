/* SPDX-License-Identifier: GPL-3.0-or-later */
/* snapshot.c — still pictures of the cameras (snapshot.h). */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cam_events.h"
#include "channel.h"
#include "config.h"
#include "events.h"
#include "hal_display.h"
#include "jpeg_enc.h"
#include "log.h"
#include "snap_cam.h"
#include "snap_store.h"
#include "snapshot.h"
#include "sys_curl.h"

#define CAMERA_MS    8000           /* the camera's whole answer */
#define STILL_MS     1000           /* a decoder's next picture (1 fps is the slowest camera) */
#define JOBS         16
#define RESULTS      16
#define AUTH_RETRY   60             /* s before a camera that refused the password is asked again */

int snapshot_src(const char *s)
{
    if (!s || !*s || !strcmp(s, "auto")) return SNAPSHOT_AUTO;
    if (!strcmp(s, "camera")) return SNAPSHOT_CAMERA;
    if (!strcmp(s, "nvr")) return SNAPSHOT_NVR;
    return -1;
}

static long long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static vigil_config_t *cfg_get(void)
{
    vigil_config_t *c = malloc(sizeof *c);
    if (c) config_get(c);
    return c;
}

/* ---- from the camera ------------------------------------------------------------- */

/* One request at a time to each camera: a camera's web server or NETIP
 * stack is small, and an alarm burst and an API client together should
 * not knock it over. */
static pthread_mutex_t g_cam_lock[CHANNEL_MAX] = { [0 ... CHANNEL_MAX - 1] = PTHREAD_MUTEX_INITIALIZER };

/* What ONVIF said, per channel, for the camera it was asked of. */
static pthread_mutex_t g_onvif_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char   key[700];                /* url | onvif_url */
    char   uri[512];
    time_t fail_until;              /* not asked again before then */
} g_onvif[CHANNEL_MAX];

/* A camera found to have no snapshot of its own: "auto" goes straight to
 * the decoder's picture for SNAPSHOT_ONVIF_RETRY s (a NETIP camera that
 * never answers OPSNAP would otherwise cost every request CAMERA_MS). */
static struct {
    char   key[sizeof g_onvif[0].key + 256];
    time_t until;
} g_nocam[CHANNEL_MAX];

static void cam_key(const config_camera_t *cam, char *out, size_t cap)
{
    snprintf(out, cap, "%s|%s|%s", cam->url, cam->onvif_url, cam->snapshot_url);
}

static int self_onvif_port(const vigil_config_t *c) { return c->onvif_server.enabled ? c->onvif_server.port : 0; }

static snap_rc_t by_onvif(const vigil_config_t *c, int ch, const char *user, const char *pass, snap_jpeg_t *j,
                          char *err, size_t cap)
{
    const config_camera_t *cam = &c->cameras[ch];
    char key[sizeof g_onvif[0].key], uri[sizeof g_onvif[0].uri] = "";
    snprintf(key, sizeof key, "%s|%s", cam->url, cam->onvif_url);
    time_t now = time(NULL);
    pthread_mutex_lock(&g_onvif_lock);
    int known = !strcmp(g_onvif[ch].key, key);
    if (known) snprintf(uri, sizeof uri, "%s", g_onvif[ch].uri);
    time_t until = known ? g_onvif[ch].fail_until : 0;
    pthread_mutex_unlock(&g_onvif_lock);
    if (!uri[0] && until > now) {
        snprintf(err, cap, "no ONVIF snapshot (asked again in %ld s)", (long)(until - now));
        return SNAP_EUNSUPPORTED;
    }
    for (int pass_n = 0; pass_n < 2; ++pass_n) {
        if (!uri[0]) {
            snap_rc_t rc = snap_onvif_uri(cam->url, cam->onvif_url, self_onvif_port(c), NULL, uri, sizeof uri, err, cap);
            pthread_mutex_lock(&g_onvif_lock);
            snprintf(g_onvif[ch].key, sizeof g_onvif[ch].key, "%s", key);
            snprintf(g_onvif[ch].uri, sizeof g_onvif[ch].uri, "%s", rc == SNAP_OK ? uri : "");
            g_onvif[ch].fail_until = rc == SNAP_OK ? 0 : now + (rc == SNAP_EAUTH ? AUTH_RETRY : SNAPSHOT_ONVIF_RETRY);
            pthread_mutex_unlock(&g_onvif_lock);
            if (rc != SNAP_OK) return rc;
            vg_log("CAM%d: ONVIF snapshot at %.*s", ch + 1, (int)strcspn(uri, "?"), uri);
            pass_n = 1;                 /* a fresh address: no second try */
        }
        snap_rc_t rc = snap_http_get(uri, user, pass, CAMERA_MS, NULL, j, err, cap);
        if (rc == SNAP_OK || rc == SNAP_EAUTH) return rc;
        /* The address it gave may be stale (the camera's settings changed):
         * ask again once. */
        uri[0] = 0;
        pthread_mutex_lock(&g_onvif_lock);
        g_onvif[ch].uri[0] = 0;
        pthread_mutex_unlock(&g_onvif_lock);
    }
    return SNAP_EFAIL;
}

/* The camera's own JPEG. */
static snap_rc_t by_camera(const vigil_config_t *c, int ch, snap_jpeg_t *j, char *used, size_t ucap, char *err,
                           size_t cap)
{
    const config_camera_t *cam = &c->cameras[ch];
    char user[64], pass[64], host[128];
    if (snap_url_creds(cam->url, user, sizeof user, pass, sizeof pass, host, sizeof host) < 0) {
        snprintf(err, cap, "a local input has no camera of its own to ask");
        return SNAP_EUNSUPPORTED;
    }
    snap_rc_t rc;
    pthread_mutex_lock(&g_cam_lock[ch]);
    if (cam->snapshot_url[0]) {
        snprintf(used, ucap, "camera/url");
        rc = snap_http_get(cam->snapshot_url, user, pass, CAMERA_MS, NULL, j, err, cap);
    } else if (!strncmp(cam->url, "netip://", 8)) {
        snprintf(used, ucap, "camera/netip");
        rc = snap_netip(cam->url, CAMERA_MS, NULL, j, err, cap);
        /* An XM camera whose NETIP has no snapshot may still have ONVIF's. */
        if (rc == SNAP_EUNSUPPORTED) {
            char e2[160];
            snap_rc_t r2 = by_onvif(c, ch, user, pass, j, e2, sizeof e2);
            if (r2 == SNAP_OK) snprintf(used, ucap, "camera/onvif");
            if (r2 == SNAP_OK || r2 == SNAP_EAUTH) {
                rc = r2;
                snprintf(err, cap, "%s", e2);
            }
        }
    } else {
        snprintf(used, ucap, "camera/onvif");
        rc = by_onvif(c, ch, user, pass, j, err, cap);
    }
    pthread_mutex_unlock(&g_cam_lock[ch]);
    memset(pass, 0, sizeof pass);
    return rc;
}

/* ---- from the recorder ----------------------------------------------------------- */

/* The decoder's picture is video range (BT.601, Y 16..235); JFIF is full. */
static unsigned char g_lut_y[256], g_lut_c[256];
static pthread_once_t g_lut_once = PTHREAD_ONCE_INIT;

static void lut_init(void)
{
    for (int i = 0; i < 256; ++i) {
        int y = (i - 16) * 255 / 219, u = (i - 128) * 255 / 224 + 128;
        g_lut_y[i] = (unsigned char)(y < 0 ? 0 : y > 255 ? 255 : y);
        g_lut_c[i] = (unsigned char)(u < 0 ? 0 : u > 255 ? 255 : u);
    }
}

static int by_nvr(const vigil_config_t *c, int ch, unsigned char **jpeg, size_t *len, char *err, size_t cap)
{
    hal_pic_t pic;
    if (hal_display_still(ch, &pic, STILL_MS) < 0) {
        snprintf(err, cap, "no picture: the channel is not being decoded (not on the screen, or no decoder free)");
        return -1;
    }
    pthread_once(&g_lut_once, lut_init);
    long long t0 = mono_ms();
    for (unsigned y = 0; y < pic.h; ++y)
        for (unsigned x = 0; x < pic.w; ++x) pic.y[(size_t)y * pic.ys + x] = g_lut_y[pic.y[(size_t)y * pic.ys + x]];
    for (unsigned y = 0; y < pic.h / 2; ++y)
        for (unsigned x = 0; x < pic.w / 2; ++x) {
            pic.u[(size_t)y * pic.cs + x] = g_lut_c[pic.u[(size_t)y * pic.cs + x]];
            pic.v[(size_t)y * pic.cs + x] = g_lut_c[pic.v[(size_t)y * pic.cs + x]];
        }
    jpeg_planes_t pl = { pic.y, pic.u, pic.v, pic.w, pic.h, pic.ys, pic.cs };
    jpeg_buf_t out = { 0 };
    long n = jpeg_encode(&pl, 0, 0, pic.w, pic.h, c->snapshot.quality, &out);
    long long ms = mono_ms() - t0;
    free(pic.y);
    if (n < 0) {
        free(out.p);
        snprintf(err, cap, "out of memory");
        return -1;
    }
    vg_log("CAM%d: snapshot %ux%u encoded here in %lld ms (%ld KiB, quality %d)", ch + 1, pic.w, pic.h, ms, n >> 10,
           c->snapshot.quality);
    *jpeg = out.p;
    *len = out.len;
    return 0;
}

/* ---- taking one ----------------------------------------------------------------- */

static int take_cfg(const vigil_config_t *c, int ch, snapshot_src_t src, unsigned char **jpeg, size_t *len,
                    char *used, size_t ucap, int *status, char *err, size_t cap)
{
    *jpeg = NULL;
    *len = 0;
    if (ucap) used[0] = 0;
    if (ch < 0 || ch >= CHANNEL_MAX || ch >= c->n_cameras || !c->cameras[ch].url[0] || !c->cameras[ch].enabled) {
        *status = 404;
        snprintf(err, cap, "no camera on channel %d", ch + 1);
        return -1;
    }
    char cerr[160] = "", key[sizeof g_nocam[0].key];
    cam_key(&c->cameras[ch], key, sizeof key);
    time_t now = time(NULL);
    pthread_mutex_lock(&g_onvif_lock);
    long none_for = !strcmp(g_nocam[ch].key, key) && g_nocam[ch].until > now ? (long)(g_nocam[ch].until - now) : 0;
    pthread_mutex_unlock(&g_onvif_lock);
    if (src == SNAPSHOT_AUTO && none_for)
        snprintf(cerr, sizeof cerr, "it has no snapshot of its own (asked again in %ld s)", none_for);
    else if (src != SNAPSHOT_NVR) {
        snap_jpeg_t j;
        snap_rc_t rc = by_camera(c, ch, &j, used, ucap, cerr, sizeof cerr);
        pthread_mutex_lock(&g_onvif_lock);
        snprintf(g_nocam[ch].key, sizeof g_nocam[ch].key, "%s", key);
        g_nocam[ch].until = rc == SNAP_EUNSUPPORTED ? now + SNAPSHOT_ONVIF_RETRY : 0;
        pthread_mutex_unlock(&g_onvif_lock);
        if (rc == SNAP_OK) {
            *jpeg = j.p;
            *len = j.len;
            return 0;
        }
        if (src == SNAPSHOT_CAMERA) {
            *status = rc == SNAP_EUNSUPPORTED ? 503 : 502;
            snprintf(err, cap, "the camera: %s", cerr);
            return -1;
        }
    }
    char nerr[160];
    if (by_nvr(c, ch, jpeg, len, nerr, sizeof nerr) == 0) {
        snprintf(used, ucap, "nvr");
        return 0;
    }
    *status = 503;
    if (cerr[0]) snprintf(err, cap, "the camera: %s; %s", cerr, nerr);
    else snprintf(err, cap, "%s", nerr);
    return -1;
}

int snapshot_take(int ch, snapshot_src_t src, unsigned char **jpeg, size_t *len, char *used, size_t ucap,
                  int *status, char *err, size_t cap)
{
    vigil_config_t *c = cfg_get();
    if (!c) {
        *status = 500;
        snprintf(err, cap, "out of memory");
        return -1;
    }
    int rc = take_cfg(c, ch, src, jpeg, len, used, ucap, status, err, cap);
    free(c);
    return rc;
}

static int save_cfg(const vigil_config_t *c, int ch, snapshot_src_t src, const char *kind, char *name, size_t ncap,
                    char *used, size_t ucap, char *path, size_t pcap, int *status, char *err, size_t cap)
{
    char dir[256], why[200];
    if (snap_dir(c->snapshot.path, c->record.path, c->record.allow_rootfs, dir, sizeof dir, why, sizeof why) < 0) {
        *status = 507;
        snprintf(err, cap, "nowhere to keep it: %s", why);
        return -1;
    }
    if (channels_rec_held(dir)) {
        *status = 503;
        snprintf(err, cap, "the disk is being worked on");
        return -1;
    }
    unsigned char *jpeg;
    size_t len;
    if (take_cfg(c, ch, src, &jpeg, &len, used, ucap, status, err, cap) < 0) return -1;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    char nm[SNAP_NAME_MAX];
    int rc = snap_name(nm, sizeof nm, ch + 1, now.tv_sec, (int)(now.tv_nsec / 1000000), kind);
    if (rc == 0) rc = snap_save(dir, nm, jpeg, len, err, cap);
    else snprintf(err, cap, "no name for it");
    free(jpeg);
    if (rc < 0) {
        *status = 507;
        return -1;
    }
    int gone = snap_trim(dir, (uint64_t)c->snapshot.max_mb << 20);
    vg_log("CAM%d: snapshot %s (%s, %zu KiB) in %s%s", ch + 1, nm, used, len >> 10, dir,
           gone ? ", the oldest removed for room" : "");
    events_push(ch + 1, "snapshot %s", kind);
    snprintf(name, ncap, "%s", nm);
    if (path) snprintf(path, pcap, "%s/%s", dir, nm);
    return 0;
}

int snapshot_save(int ch, snapshot_src_t src, const char *kind, char *name, size_t ncap, char *used, size_t ucap,
                  int *status, char *err, size_t cap)
{
    vigil_config_t *c = cfg_get();
    if (!c) {
        *status = 500;
        snprintf(err, cap, "out of memory");
        return -1;
    }
    int rc = save_cfg(c, ch, src, kind, name, ncap, used, ucap, NULL, 0, status, err, cap);
    free(c);
    return rc;
}

/* ---- the worker: the screen's requests and the events' pictures ----------------------- */

typedef struct {
    int       id;                   /* 0: free */
    int       ch;
    char      kind[SNAP_KIND_MAX];
    int       event;                /* taken for a camera event (its settings apply) */
    int       taken, left;          /* event: pictures so far and to come */
    long long next_at;              /* monotonic ms */
    time_t    when;                 /* the event's start */
    char      files[MAIL_FILES_MAX][320];
} job_t;

static struct {
    pthread_mutex_t lock;
    pthread_cond_t  wake;
    pthread_t       thread;
    int             up, stop, sub;
    int             next_id;
    job_t           jobs[JOBS];
    struct { int id, rc; char msg[200]; } res[RESULTS];
    int             res_at;
} W = { .lock = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER, .sub = -1 };

/* Locked. */
static int job_add(int ch, const char *kind, int event, time_t when)
{
    job_t *free_job = NULL;
    for (int i = 0; i < JOBS; ++i) {
        job_t *j = &W.jobs[i];
        /* An event's pictures under way are that event's: a second type
         * starting with it (motion, then a person) adds none. */
        if (j->id && event && j->event && j->ch == ch) return -1;
        if (!j->id && !free_job) free_job = j;
    }
    if (!free_job) return -1;
    memset(free_job, 0, sizeof *free_job);
    free_job->id = ++W.next_id > 0 ? W.next_id : (W.next_id = 1);
    free_job->ch = ch;
    snprintf(free_job->kind, sizeof free_job->kind, "%s", kind);
    free_job->event = event;
    free_job->left = 1;
    free_job->next_at = mono_ms();
    free_job->when = when;
    pthread_cond_signal(&W.wake);
    return free_job->id;
}

/* Locked. */
static void result_put(int id, int rc, const char *msg)
{
    W.res[W.res_at].id = id;
    W.res[W.res_at].rc = rc;
    snprintf(W.res[W.res_at].msg, sizeof W.res[W.res_at].msg, "%s", msg);
    W.res_at = (W.res_at + 1) % RESULTS;
}

int snapshot_request(int ch)
{
    pthread_mutex_lock(&W.lock);
    int id = W.up && !W.stop ? job_add(ch, "manual", 0, time(NULL)) : -1;
    pthread_mutex_unlock(&W.lock);
    return id;
}

int snapshot_result(int id, char *msg, size_t cap)
{
    pthread_mutex_lock(&W.lock);
    int rc = -1;
    snprintf(msg, cap, "forgotten");
    for (int i = 0; i < JOBS; ++i)
        if (W.jobs[i].id == id) rc = 0;
    for (int i = 0; i < RESULTS && rc; ++i)
        if (W.res[i].id == id) {
            rc = W.res[i].rc;
            snprintf(msg, cap, "%s", W.res[i].msg);
        }
    if (rc == 0) snprintf(msg, cap, "taking it");
    pthread_mutex_unlock(&W.lock);
    return rc;
}

/* A camera event began (on the reporting thread: only queued here). */
static void on_event(void *user, const cam_event_t *ev)
{
    (void)user;
    if (!ev->active || ev->channel < 0 || ev->channel >= CHANNEL_MAX) return;
    int bit = 0;
    while (bit < CH_EV_TYPES && !(ev->type & 1u << bit)) ++bit;
    const char *name = ch_event_name(bit);
    pthread_mutex_lock(&W.lock);
    if (W.up && !W.stop) job_add(ev->channel, name ? name : "event", 1, ev->time);
    pthread_mutex_unlock(&W.lock);
}

static void mail_event(const vigil_config_t *c, const job_t *j, int n)
{
    if (!c->snapshot.mail || !c->smtp.enabled || n <= 0) return;
    const char *files[MAIL_FILES_MAX];
    for (int i = 0; i < n; ++i) files[i] = j->files[i];
    const config_camera_t *cam = &c->cameras[j->ch];
    char subject[160], body[512], when[32], msg[256];
    struct tm tm;
    localtime_r(&j->when, &tm);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
    snprintf(subject, sizeof subject, "vigil: CAM%d%s%s%s: %s", j->ch + 1, cam->name[0] ? " (" : "", cam->name,
             cam->name[0] ? ")" : "", j->kind);
    snprintf(body, sizeof body, "Camera %d%s%s%s reported %s at %s.\n%d snapshot%s attached.\n", j->ch + 1,
             cam->name[0] ? " (" : "", cam->name, cam->name[0] ? ")" : "", j->kind, when, n, n == 1 ? "" : "s");
    int rc = mail_send_files(&c->smtp, subject, body, files, n, msg, sizeof msg);
    vg_log("CAM%d: %s snapshots %s: %s", j->ch + 1, j->kind, rc == 0 ? "mailed" : "not mailed", msg);
}

static void run_job(job_t *j)
{
    vigil_config_t *c = cfg_get();
    if (!c) return;
    if (j->event && j->taken == 0) {
        /* Its settings when it is first taken: the event came from any
         * camera, the subscription does not know which want pictures. */
        int on = j->ch < c->n_cameras && !strcmp(c->cameras[j->ch].snapshot_on_event, "on");
        if (!on) {
            j->left = 0;
            free(c);
            return;
        }
        j->left = c->snapshot.event_count;
    }
    char name[SNAP_NAME_MAX] = "", used[24], err[160], path[320];
    int status;
    int src = snapshot_src(c->snapshot.source);
    int rc = save_cfg(c, j->ch, src < 0 ? SNAPSHOT_AUTO : (snapshot_src_t)src, j->kind, name, sizeof name, used,
                      sizeof used, path, sizeof path, &status, err, sizeof err);
    if (rc < 0) vg_log("CAM%d: %s snapshot failed: %s", j->ch + 1, j->kind, err);
    if (rc == 0 && j->taken < MAIL_FILES_MAX) snprintf(j->files[j->taken], sizeof j->files[0], "%s", path);
    if (rc == 0) ++j->taken;
    --j->left;
    if (!j->event) {
        pthread_mutex_lock(&W.lock);
        result_put(j->id, rc == 0 ? 1 : -1, rc == 0 ? name : err);
        pthread_mutex_unlock(&W.lock);
    } else if (j->left <= 0) {
        mail_event(c, j, j->taken < MAIL_FILES_MAX ? j->taken : MAIL_FILES_MAX);
    }
    j->next_at = mono_ms() + (long long)c->snapshot.event_interval * 1000;
    free(c);
}

static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&W.lock);
    while (!W.stop) {
        job_t *due = NULL;
        long long now = mono_ms(), wait = 1000;
        for (int i = 0; i < JOBS; ++i) {
            job_t *j = &W.jobs[i];
            if (!j->id) continue;
            if (j->next_at <= now) {
                if (!due || j->next_at < due->next_at) due = j;
            } else if (j->next_at - now < wait) {
                wait = j->next_at - now;
            }
        }
        if (!due) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += (time_t)(wait / 1000);
            ts.tv_nsec += (long)(wait % 1000) * 1000000;
            if (ts.tv_nsec >= 1000000000) {
                ++ts.tv_sec;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&W.wake, &W.lock, &ts);
            continue;
        }
        /* The job is the worker's while it runs (nothing else frees it). */
        job_t j = *due;
        pthread_mutex_unlock(&W.lock);
        run_job(&j);
        pthread_mutex_lock(&W.lock);
        if (j.left > 0) *due = j;
        else memset(due, 0, sizeof *due);
    }
    pthread_mutex_unlock(&W.lock);
    return NULL;
}

int snapshot_start(void)
{
    pthread_mutex_lock(&W.lock);
    if (W.up) {
        pthread_mutex_unlock(&W.lock);
        return 0;
    }
    W.stop = 0;
    W.up = pthread_create(&W.thread, NULL, worker, NULL) == 0;
    pthread_mutex_unlock(&W.lock);
    if (!W.up) return -1;
    W.sub = cam_events_subscribe(on_event, NULL);
    if (W.sub < 0) vg_log("snapshots: no room for another camera events subscriber; no pictures on events");
    return 0;
}

void snapshot_stop(void)
{
    if (W.sub >= 0) cam_events_unsubscribe(W.sub);
    W.sub = -1;
    pthread_mutex_lock(&W.lock);
    int up = W.up;
    W.stop = 1;
    pthread_cond_signal(&W.wake);
    pthread_mutex_unlock(&W.lock);
    if (up) pthread_join(W.thread, NULL);
    pthread_mutex_lock(&W.lock);
    W.up = 0;
    memset(W.jobs, 0, sizeof W.jobs);
    pthread_mutex_unlock(&W.lock);
}
