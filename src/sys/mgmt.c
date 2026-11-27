/* SPDX-License-Identifier: GPL-3.0-or-later */
/* mgmt.c — jobs, timers and the glue of the management pages (mgmt.h). */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "channel.h"
#include "config.h"
#include "events.h"
#include "log.h"
#include "mgmt.h"
#include "sys.h"
#include "sys_curl.h"
#include "sys_disk.h"
#include "sys_nas.h"

#define JOBS          16
#define NAS_RETRY     60            /* seconds between attempts to mount the share */
#define MAIL_GAP      600           /* at most one trouble mail this often */
#define MAIL_LINES    40

typedef enum {
    J_NET, J_WIFI, J_NTP_SYNC, J_NTP_CHECK, J_NTP_DAEMON, J_DDNS, J_SMTP_TEST, J_MAIL, J_ALARM,
    J_FORMAT, J_MOUNT, J_UNMOUNT, J_NAS_TEST, J_NAS_MOUNT, J_SMART,
} jkind_t;

/* Two queues, so a format that takes minutes does not hold up an NTP
 * check or an alarm. */
enum { Q_SLOW, Q_IO, Q_N };

typedef struct {
    mgmt_job_t pub;
    jkind_t    kind;
    int        queue;
    int        internal;            /* timers' own: not announced */
    net_cfg_t  net;
    int        confirm, flag;
    char       a[64], b[96], c[48];
    char      *payload;             /* mail text, alarm JSON (malloc'd) */
} slot_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_wake = PTHREAD_COND_INITIALIZER;
static slot_t          g_jobs[JOBS];
static int             g_next_id = 1;
static int             g_stop, g_up;
static pthread_t       g_workers[Q_N], g_timer;

/* The network change waiting for its confirmation. */
static struct {
    int    active;
    char   ifname[16];
    char  *old_text;                /* the stanza before the first unconfirmed change */
    int    had_old;
    time_t deadline;
} g_pend;

static int               g_confirm_secs = MGMT_CONFIRM_SECS;
static mgmt_ntp_status_t g_ntp;
static mgmt_services_t   g_svc;

static time_t mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

const char *mgmt_job_state_name(mgmt_job_state_t s)
{
    switch (s) {
    case JOB_QUEUED:  return "queued";
    case JOB_RUNNING: return "running";
    case JOB_DONE:    return "done";
    default:          return "failed";
    }
}

/* ---- the queues ------------------------------------------------------------------- */

static const char *kind_name(jkind_t k)
{
    static const char *const n[] = { "network", "wifi", "ntp-sync", "ntp-check", "ntp-daemon", "ddns",
                                     "smtp-test", "mail", "alarm", "format", "mount", "unmount",
                                     "nas-test", "nas-mount", "smart" };
    return n[k];
}

/* A slot for a new job (caller holds g_lock): a free one, else the
 * oldest finished. NULL when all are queued or running. */
static slot_t *new_slot(void)
{
    slot_t *best = NULL;
    for (int i = 0; i < JOBS; ++i) {
        slot_t *s = &g_jobs[i];
        if (!s->pub.id) return s;
        if (s->pub.state >= JOB_DONE && (!best || s->pub.finished < best->pub.finished)) best = s;
    }
    return best;
}

/* Queue `proto`; returns the id, or -1 when the queue is full. Takes
 * proto->payload. */
static int submit(slot_t *proto)
{
    pthread_mutex_lock(&g_lock);
    slot_t *s = g_up ? new_slot() : NULL;
    if (!s) {
        pthread_mutex_unlock(&g_lock);
        free(proto->payload);
        return -1;
    }
    free(s->payload);
    *s = *proto;
    s->pub.id = g_next_id++;
    snprintf(s->pub.kind, sizeof s->pub.kind, "%s", kind_name(s->kind));
    s->pub.state = JOB_QUEUED;
    s->pub.msg[0] = 0;
    s->pub.started = s->pub.finished = 0;
    s->queue = s->kind == J_NTP_SYNC || s->kind == J_NTP_CHECK || s->kind == J_DDNS || s->kind == J_SMTP_TEST ||
               s->kind == J_MAIL || s->kind == J_ALARM || s->kind == J_SMART ? Q_IO : Q_SLOW;
    int id = s->pub.id;
    pthread_cond_broadcast(&g_wake);
    pthread_mutex_unlock(&g_lock);
    return id;
}

int mgmt_job(int id, mgmt_job_t *out)
{
    int rc = -1;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < JOBS; ++i)
        if (g_jobs[i].pub.id == id && id > 0) {
            *out = g_jobs[i].pub;
            rc = 0;
        }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

static void cfg_get(vigil_config_t **c)
{
    *c = malloc(sizeof **c);
    if (*c) config_get(*c);
}

static int  smart_job(const char *name, char *msg, size_t cap);
static const char *disk_of(const char *name);

/* ---- running a job ------------------------------------------------------------------ */

/* Recording is held off the mount that holds record.path, if it is one
 * of `name`'s. Returns 1 when a hold was taken (release it after). */
static int hold_for(const char *name)
{
    char mnts[8][128];
    int n = sys_disk_mounts(name, mnts, 8), held = 0;
    vigil_config_t *c;
    cfg_get(&c);
    for (int i = 0; c && i < n && !held; ++i) {
        size_t l = strlen(mnts[i]);
        if (c->record.path[0] && !strncmp(c->record.path, mnts[i], l) &&
            (c->record.path[l] == '/' || !c->record.path[l])) {
            int open = channels_rec_hold(mnts[i], 5000);
            vg_log("mgmt: recording held off %s for %s%s", mnts[i], name, open ? " (segments still open)" : "");
            held = 1;
        }
    }
    free(c);
    return held;
}

static void ntp_servers(char servers[][64], int *n)
{
    sys_clock_t c;
    clock_read(&c);
    *n = c.n_servers;
    memcpy(servers, c.servers, sizeof c.servers);
}

static void svc_set(mgmt_svc_status_t *st, int ok, const char *msg)
{
    pthread_mutex_lock(&g_lock);
    st->last = time(NULL);
    st->ok = ok;
    snprintf(st->result, sizeof st->result, "%s", msg);
    pthread_mutex_unlock(&g_lock);
}

static int run_job(slot_t *s, char *msg, size_t cap)
{
    char err[256] = "";
    int rc = -1;
    switch (s->kind) {
    case J_NET: {
        /* The answer to the request that asked for this goes out first. */
        if (g_confirm_secs >= 10) sleep(1);
        char *old = NULL;
        rc = net_apply(&s->net, &old, err, sizeof err);
        if (rc < 0 && old) {
            /* ifup failed: put the old file back at once. */
            char e2[256];
            net_restore(s->net.ifname, old, e2, sizeof e2);
            snprintf(msg, cap, "%.200s; the previous settings are back", err);
            free(old);
            events_push(0, "network: change failed");
            return -1;
        }
        if (rc < 0) {
            snprintf(msg, cap, "%s", err);
            free(old);
            return -1;
        }
        pthread_mutex_lock(&g_lock);
        if (s->confirm) {
            /* A second change before the first was confirmed keeps the
             * first's old file: rolling back goes to what was confirmed. */
            if (!g_pend.active || strcmp(g_pend.ifname, s->net.ifname)) {
                free(g_pend.old_text);
                g_pend.old_text = old;
                g_pend.had_old = old != NULL;
                old = NULL;
            }
            g_pend.active = 1;
            snprintf(g_pend.ifname, sizeof g_pend.ifname, "%s", s->net.ifname);
            g_pend.deadline = mono() + g_confirm_secs;
        }
        pthread_mutex_unlock(&g_lock);
        free(old);
        snprintf(msg, cap, s->confirm ? "applied; confirm within %d s or it is undone" : "applied",
                 g_confirm_secs);
        events_push(0, s->confirm ? "network: confirm in %d s" : "network changed", g_confirm_secs);
        return 0;
    }
    case J_WIFI:
        rc = wifi_set(s->a, s->b, s->payload ? s->payload : "", s->c, err, sizeof err);
        if (s->payload) memset(s->payload, 0, strlen(s->payload));
        snprintf(msg, cap, "%s", rc ? err : "joining the network");
        return rc < 0 ? -1 : 0;
    case J_NTP_SYNC:
    case J_NTP_CHECK: {
        char servers[4][64];
        int n;
        ntp_servers(servers, &n);
        rc = ntp_sync(servers, n, err, sizeof err);
        pthread_mutex_lock(&g_lock);
        g_ntp.last_try = time(NULL);
        if (rc == 0) g_ntp.last_ok = g_ntp.last_try;
        snprintf(g_ntp.result, sizeof g_ntp.result, "%.159s", err);
        pthread_mutex_unlock(&g_lock);
        snprintf(msg, cap, "%s", err);
        if (rc < 0 && s->kind == J_NTP_CHECK) vg_log("mgmt: NTP: %s", err);
        return rc;
    }
    case J_NTP_DAEMON:
        rc = ntp_daemon(s->flag, err, sizeof err);
        snprintf(msg, cap, "%s", rc ? err : s->flag ? "ntpd restarted" : "ntpd stopped");
        return rc;
    case J_DDNS: {
        vigil_config_t *c;
        cfg_get(&c);
        rc = c ? ddns_update(&c->ddns, err, sizeof err) : -1;
        free(c);
        svc_set(&g_svc.ddns, rc == 0, err);
        snprintf(msg, cap, "%s", err);
        if (rc < 0 && !s->internal) vg_log("mgmt: DDNS: %s", err);
        return rc;
    }
    case J_SMTP_TEST:
    case J_MAIL: {
        vigil_config_t *c;
        cfg_get(&c);
        char host[64], subj[128];
        host_get(host, sizeof host);
        if (s->kind == J_SMTP_TEST) snprintf(subj, sizeof subj, "vigil on %s: test mail", host);
        else snprintf(subj, sizeof subj, "vigil on %.63s: %.50s", host, s->b);
        const char *body = s->payload ? s->payload : "This is a test mail from vigil. Mail settings work.\n";
        rc = c ? mail_send(&c->smtp, subj, body, err, sizeof err) : -1;
        free(c);
        svc_set(&g_svc.smtp, rc == 0, err);
        snprintf(msg, cap, "%s", err);
        if (rc < 0) vg_log("mgmt: mail: %s", err);
        return rc;
    }
    case J_ALARM:
        rc = http_post_json(s->a, s->payload ? s->payload : "{}", err, sizeof err);
        svc_set(&g_svc.alarm, rc == 0, err);
        snprintf(msg, cap, "%s", err);
        return rc;
    case J_FORMAT: {
        int held = hold_for(s->a);
        rc = sys_disk_format(s->a, s->b, err, sizeof err);
        if (held) channels_rec_release();
        snprintf(msg, cap, "%s", err);
        vg_log("mgmt: format %s as %s: %s", s->a, s->b, err);
        events_push(0, rc ? "disk %s: format failed" : "disk %s formatted", s->a);
        return rc;
    }
    case J_MOUNT:
        rc = sys_disk_mount(s->a, err, sizeof err);
        snprintf(msg, cap, "%s", err);
        vg_log("mgmt: mount %s: %s", s->a, err);
        return rc;
    case J_UNMOUNT: {
        int held = hold_for(s->a);
        rc = sys_disk_unmount(s->a, err, sizeof err);
        /* Unmounted, the hold has nothing left to protect: recording
         * waits for the disk by itself (rec_place). Still busy: it goes
         * on recording there. */
        if (held) channels_rec_release();
        snprintf(msg, cap, "%s", err);
        vg_log("mgmt: unmount %s: %s", s->a, err);
        return rc;
    }
    case J_SMART:
        rc = smart_job(s->a, msg, cap);
        return rc;
    case J_NAS_TEST: {
        vigil_config_t *c;
        cfg_get(&c);
        rc = c ? sys_nas_test(&c->nas, err, sizeof err) : -1;
        free(c);
        snprintf(msg, cap, "%s", err);
        return rc;
    }
    case J_NAS_MOUNT: {
        vigil_config_t *c;
        cfg_get(&c);
        if (!c) return -1;
        if (s->flag && sys_nas_mounted(MGMT_NAS_MOUNT)) {
            /* new settings: off the old share first */
            int open = channels_rec_hold(MGMT_NAS_MOUNT, 5000);
            (void)open;
            rc = sys_nas_umount(MGMT_NAS_MOUNT, err, sizeof err);
            channels_rec_release();
            if (rc < 0) {
                snprintf(msg, cap, "%s", err);
                svc_set(&g_svc.nas, 0, err);
                free(c);
                return -1;
            }
        }
        if (!strcmp(c->nas.type, "none")) {
            free(c);
            svc_set(&g_svc.nas, 1, "not used");
            snprintf(msg, cap, "not used");
            return 0;
        }
        rc = sys_nas_mount(&c->nas, MGMT_NAS_MOUNT, err, sizeof err);
        free(c);
        int was_ok = g_svc.nas.ok;
        svc_set(&g_svc.nas, rc == 0, err);
        if (rc == 0) events_push(0, "NAS mounted");
        else if (was_ok || !g_svc.nas.last) vg_log("mgmt: network storage: %s", err);
        snprintf(msg, cap, "%s", err);
        return rc;
    }
    }
    snprintf(msg, cap, "unknown job");
    return -1;
}

static void *worker(void *arg)
{
    int q = (int)(intptr_t)arg;
    pthread_mutex_lock(&g_lock);
    while (!g_stop) {
        slot_t *s = NULL;
        for (int i = 0; i < JOBS; ++i) {
            slot_t *j = &g_jobs[i];
            if (j->pub.id && j->queue == q && j->pub.state == JOB_QUEUED && (!s || j->pub.id < s->pub.id)) s = j;
        }
        if (!s) {
            pthread_cond_wait(&g_wake, &g_lock);
            continue;
        }
        s->pub.state = JOB_RUNNING;
        s->pub.started = time(NULL);
        slot_t copy = *s;
        int id = s->pub.id;
        pthread_mutex_unlock(&g_lock);
        char msg[256] = "";
        int rc = run_job(&copy, msg, sizeof msg);
        pthread_mutex_lock(&g_lock);
        /* The slot is still this job's: only finished slots are reused. */
        if (s->pub.id == id) {
            s->pub.state = rc == 0 ? JOB_DONE : JOB_FAILED;
            s->pub.finished = time(NULL);
            snprintf(s->pub.msg, sizeof s->pub.msg, "%s", msg);
            if (s->payload) {
                memset(s->payload, 0, strlen(s->payload));
                free(s->payload);
                s->payload = NULL;
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

/* ---- the timers ----------------------------------------------------------------------- */

/* Events worth a mail: trouble a person must act on. */
static int mail_worthy(const char *text)
{
    static const char *const pre[] = { "REC failed", "REC disk full", "REC waiting for disk", "REC not a disk",
                                       "REC read-only", "REC error", "offline", "down", "no decoder", NULL };
    for (int i = 0; pre[i]; ++i)
        if (!strncmp(text, pre[i], strlen(pre[i]))) return 1;
    return 0;
}

static void alarm_post(const vigil_config_t *c, cJSON *doc)
{
    char host[64];
    host_get(host, sizeof host);
    cJSON_AddStringToObject(doc, "device", host);
    slot_t j = { .kind = J_ALARM, .internal = 1 };
    snprintf(j.a, sizeof j.a, "http://%.30s:%d", c->alarm.host, c->alarm.port);
    size_t l = strlen(j.a);
    snprintf(j.a + l, sizeof j.a - l, "%s", c->alarm.path);
    j.payload = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    if (j.payload) submit(&j);
}

static void *timer(void *arg)
{
    (void)arg;
    vigil_config_t *c = malloc(sizeof *c);
    if (!c) return NULL;
    config_nas_t nas_applied;
    memset(&nas_applied, 0, sizeof nas_applied);
    int ntp_applied = -1, nas_known = 0;
    time_t now0 = mono(), ntp_at = now0 + 30, ddns_at = now0 + 20, nas_at = now0 + 2, beat_at = now0;
    time_t mail_at = 0;
    uint64_t cursor = events_head();
    char mail[MAIL_LINES * 64] = "";
    int mail_n = 0;

    pthread_mutex_lock(&g_lock);
    while (!g_stop) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_cond_timedwait(&g_wake, &g_lock, &ts);
        if (g_stop) break;
        time_t now = mono();
        /* A network change nobody confirmed: undone here, not queued
         * behind a format. */
        int revert = g_pend.active && now >= g_pend.deadline;
        char ifname[16];
        char *old = NULL;
        int had_old = 0;
        if (revert) {
            snprintf(ifname, sizeof ifname, "%s", g_pend.ifname);
            old = g_pend.old_text;
            had_old = g_pend.had_old;
            g_pend.old_text = NULL;
            g_pend.active = 0;
        }
        pthread_mutex_unlock(&g_lock);

        if (revert) {
            char err[256];
            int rc = net_restore(ifname, had_old ? old : NULL, err, sizeof err);
            vg_log("mgmt: %s: no confirmation in time; previous settings restored%s%s", ifname,
                   rc ? ": " : "", rc ? err : "");
            events_push(0, "network: rolled back");
            free(old);
        }

        config_get(c);
        if (sys_managed()) {
            if (c->time.ntp != ntp_applied) {
                /* At start only stop a daemon the settings turn off; the
                 * firmware starts it, and restarting it for nothing would
                 * drop its synchronisation. */
                if (ntp_applied >= 0 || !c->time.ntp) {
                    slot_t j = { .kind = J_NTP_DAEMON, .internal = 1, .flag = c->time.ntp };
                    submit(&j);
                }
                ntp_applied = c->time.ntp;
            }
            if (c->time.ntp && now >= ntp_at) {
                slot_t j = { .kind = J_NTP_CHECK, .internal = 1 };
                submit(&j);
                ntp_at = now + (time_t)c->time.ntp_interval * 60;
            }
            if (c->ddns.enabled && now >= ddns_at) {
                slot_t j = { .kind = J_DDNS, .internal = 1 };
                submit(&j);
                ddns_at = now + (time_t)c->ddns.interval * 60;
            }
            int nas_changed = nas_known && memcmp(&nas_applied, &c->nas, sizeof nas_applied) != 0;
            if (nas_changed || (strcmp(c->nas.type, "none") && now >= nas_at && !sys_nas_mounted(MGMT_NAS_MOUNT))) {
                slot_t j = { .kind = J_NAS_MOUNT, .internal = 1, .flag = nas_changed };
                submit(&j);
                nas_at = now + NAS_RETRY;
            }
            nas_applied = c->nas;
            nas_known = 1;
        }

        /* Events: to the alarm center as they come, and the troubling
         * ones by mail, gathered. */
        event_t ev;
        while (events_next(&cursor, &ev)) {
            if (c->alarm.enabled && c->alarm.host[0]) {
                cJSON *d = cJSON_CreateObject();
                cJSON_AddStringToObject(d, "type", "event");
                cJSON_AddNumberToObject(d, "time", (double)ev.time);
                cJSON_AddNumberToObject(d, "camera", ev.ch);
                cJSON_AddStringToObject(d, "text", ev.text);
                alarm_post(c, d);
                beat_at = now + c->alarm.interval;
            }
            if (c->smtp.enabled && mail_worthy(ev.text) && mail_n < MAIL_LINES) {
                char line[96], when[32];
                struct tm tm;
                localtime_r(&ev.time, &tm);
                strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
                if (ev.ch) snprintf(line, sizeof line, "%s  CAM%d %s\n", when, ev.ch, ev.text);
                else snprintf(line, sizeof line, "%s  %s\n", when, ev.text);
                size_t l = strlen(mail);
                snprintf(mail + l, sizeof mail - l, "%s", line);
                ++mail_n;
            }
        }
        if (c->alarm.enabled && c->alarm.host[0] && c->alarm.interval && now >= beat_at) {
            cJSON *d = cJSON_CreateObject();
            cJSON_AddStringToObject(d, "type", "heartbeat");
            cJSON_AddNumberToObject(d, "time", (double)time(NULL));
            alarm_post(c, d);
            beat_at = now + c->alarm.interval;
        }
        if (mail_n && c->smtp.enabled && (!mail_at || now - mail_at >= MAIL_GAP)) {
            slot_t j = { .kind = J_MAIL, .internal = 1 };
            snprintf(j.b, sizeof j.b, "%d event%s", mail_n, mail_n > 1 ? "s" : "");
            size_t need = strlen(mail) + 128;
            j.payload = malloc(need);
            if (j.payload) snprintf(j.payload, need, "Events that need attention:\n\n%s", mail);
            submit(&j);
            mail[0] = 0;
            mail_n = 0;
            mail_at = now;
        }
        pthread_mutex_lock(&g_lock);
    }
    pthread_mutex_unlock(&g_lock);
    free(c);
    return NULL;
}

/* ---- life ------------------------------------------------------------------------------- */

int mgmt_start(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_up) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    g_stop = 0;
    g_up = 1;
    pthread_mutex_unlock(&g_lock);
    for (int q = 0; q < Q_N; ++q)
        if (pthread_create(&g_workers[q], NULL, worker, (void *)(intptr_t)q) != 0) return -1;
    if (pthread_create(&g_timer, NULL, timer, NULL) != 0) return -1;
    vg_log("mgmt: %s", sys_managed() ? "managing this system (OpenIPC)"
                                     : "not an OpenIPC system: network, time and disks are left alone");
    return 0;
}

void mgmt_stop(void)
{
    pthread_mutex_lock(&g_lock);
    if (!g_up) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_stop = 1;
    pthread_cond_broadcast(&g_wake);
    pthread_mutex_unlock(&g_lock);
    /* A surface scan is stopped, not waited for: it can take a day. */
    scan_shutdown();
    /* A job under way (a format) is waited for: stopping halfway through
     * would leave the disk without a filesystem. */
    for (int q = 0; q < Q_N; ++q) pthread_join(g_workers[q], NULL);
    pthread_join(g_timer, NULL);
    pthread_mutex_lock(&g_lock);
    g_up = 0;
    for (int i = 0; i < JOBS; ++i) {
        free(g_jobs[i].payload);
        g_jobs[i].payload = NULL;
    }
    free(g_pend.old_text);
    memset(&g_pend, 0, sizeof g_pend);
    pthread_mutex_unlock(&g_lock);
}

/* ---- network ----------------------------------------------------------------------- */

int mgmt_net_apply(const net_cfg_t *cfg, int confirm, char *field, size_t fcap, char *err, size_t cap)
{
    field[0] = err[0] = 0;
    if (!sys_managed()) return MGMT_EUNMANAGED;
    if (net_validate(cfg, field, fcap, err, cap) < 0) return MGMT_EINVAL;
    slot_t j = { .kind = J_NET, .net = *cfg, .confirm = confirm };
    snprintf(j.pub.target, sizeof j.pub.target, "%s", cfg->ifname);
    int id = submit(&j);
    if (id < 0) {
        snprintf(err, cap, "too many changes under way");
        return MGMT_EBUSY;
    }
    return id;
}

void mgmt_set_confirm_secs(int secs) { g_confirm_secs = secs; }

int mgmt_net_pending(char *ifname, size_t cap, int *secs_left)
{
    pthread_mutex_lock(&g_lock);
    int a = g_pend.active;
    if (a) {
        if (ifname) snprintf(ifname, cap, "%s", g_pend.ifname);
        if (secs_left) {
            long left = (long)(g_pend.deadline - mono());
            *secs_left = left < 0 ? 0 : (int)left;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return a;
}

int mgmt_net_confirm(void)
{
    pthread_mutex_lock(&g_lock);
    int a = g_pend.active;
    if (a) {
        free(g_pend.old_text);
        memset(&g_pend, 0, sizeof g_pend);
    }
    pthread_mutex_unlock(&g_lock);
    if (a) {
        vg_log("mgmt: network settings confirmed");
        events_push(0, "network: confirmed");
    }
    return a ? 0 : -1;
}

int mgmt_net_revert(void)
{
    /* The timer does it at once. */
    pthread_mutex_lock(&g_lock);
    int a = g_pend.active;
    if (a) g_pend.deadline = 0;
    pthread_cond_broadcast(&g_wake);
    pthread_mutex_unlock(&g_lock);
    return a ? 0 : -1;
}

int mgmt_wifi_set(const char *ifname, const char *ssid, const char *password, const char *profile, char *err,
                  size_t cap)
{
    if (!sys_managed()) return MGMT_EUNMANAGED;
    slot_t j = { .kind = J_WIFI };
    snprintf(j.a, sizeof j.a, "%s", ifname);
    snprintf(j.c, sizeof j.c, "%s", profile ? profile : "");
    snprintf(j.b, sizeof j.b, "%s", ssid);
    snprintf(j.pub.target, sizeof j.pub.target, "%s", ifname);
    j.payload = strdup(password);
    int id = submit(&j);
    if (id < 0) snprintf(err, cap, "too many changes under way");
    return id < 0 ? MGMT_EBUSY : id;
}

int mgmt_lte_profile(const char *profile, char *err, size_t cap)
{
    if (!sys_managed()) return MGMT_EUNMANAGED;
    return lte_set_profile(profile, err, cap) < 0 ? MGMT_EINVAL : MGMT_OK;
}

/* ---- time and name ----------------------------------------------------------------- */

void mgmt_ntp_status(mgmt_ntp_status_t *st)
{
    pthread_mutex_lock(&g_lock);
    *st = g_ntp;
    pthread_mutex_unlock(&g_lock);
}

int mgmt_time_set(const char *zone, const char *tz, int set_clock, time_t t, char *err, size_t cap)
{
    err[0] = 0;
    if (!sys_managed()) return MGMT_EUNMANAGED;
    if ((zone && *zone) || (tz && *tz)) {
        if (clock_set_zone(zone, tz, err, cap) < 0) return MGMT_EINVAL;
        vg_log("mgmt: time zone %s", zone && *zone ? zone : tz);
    }
    if (set_clock) {
        if (clock_set(t, err, cap) < 0) return MGMT_EINVAL;
        vg_log("mgmt: clock set");
        events_push(0, "clock set");
    }
    return MGMT_OK;
}

int mgmt_ntp_set(int enabled, char servers[][64], int n, int interval_min, char *err, size_t cap)
{
    err[0] = 0;
    if (!sys_managed()) return MGMT_EUNMANAGED;
    sys_clock_t now;
    clock_read(&now);
    int changed = n != now.n_servers;
    for (int i = 0; i < n && !changed; ++i) changed = strcmp(servers[i], now.servers[i]) != 0;
    if (n && changed && ntp_set_servers(servers, n, err, cap) < 0) return MGMT_EINVAL;
    cJSON *d = cJSON_CreateObject();
    cJSON_AddBoolToObject(d, "ntp", enabled);
    cJSON_AddNumberToObject(d, "ntp_interval", interval_min);
    char *body = cJSON_PrintUnformatted(d);
    cJSON_Delete(d);
    schema_errors_t e = { 0 };
    config_rc_t rc = body ? config_put_json("time", body, strlen(body), &e) : CONFIG_EIO;
    free(body);
    if (rc != CONFIG_OK && rc != CONFIG_EUNSYNCED) {
        snprintf(err, cap, "%s%s%s", e.n ? e.e[0].path : "", e.n && e.e[0].path[0] ? ": " : "",
                 e.n ? e.e[0].msg : "cannot save");
        return rc == CONFIG_EINVAL ? MGMT_EINVAL : MGMT_EFAIL;
    }
    /* New servers reach a running ntpd only through a restart; on/off
     * is the timer's (it follows time.ntp). */
    if (changed && enabled) {
        slot_t j = { .kind = J_NTP_DAEMON, .flag = 1 };
        submit(&j);
    }
    return MGMT_OK;
}

int mgmt_ntp_sync(void)
{
    if (!sys_managed()) return MGMT_EUNMANAGED;
    slot_t j = { .kind = J_NTP_SYNC };
    return submit(&j);
}

/* ---- reboot and shutdown --------------------------------------------------------------- */

static volatile int g_power;            /* mgmt_power_t; set once, read by the main loop */

int mgmt_power_request(mgmt_power_t what)
{
    if (what != MGMT_POWER_REBOOT && what != MGMT_POWER_OFF) return MGMT_EINVAL;
    if (!sys_managed()) return MGMT_EUNMANAGED;
    vg_log("%s asked for", what == MGMT_POWER_REBOOT ? "reboot" : "shutdown");
    events_push(0, "%s", what == MGMT_POWER_REBOOT ? "rebooting" : "shutting down");
    __atomic_store_n(&g_power, (int)what, __ATOMIC_SEQ_CST);
    return MGMT_OK;
}

mgmt_power_t mgmt_power_pending(void) { return (mgmt_power_t)__atomic_load_n(&g_power, __ATOMIC_SEQ_CST); }

void mgmt_power_run(mgmt_power_t what)
{
    if (what == MGMT_POWER_NONE) return;
    /* The recordings were closed by the stop; the disks get them now. */
    sync();
    char out[256];
    const char *prog = what == MGMT_POWER_REBOOT ? "reboot" : "poweroff";
    int rc = sys_run(out, sizeof out, 30, prog, NULL);
    if (rc != 0) vg_log("%s: exit status %d: %s", prog, rc, out);
}

int mgmt_hostname_set(const char *name, char *err, size_t cap)
{
    if (!sys_managed()) return MGMT_EUNMANAGED;
    if (host_set(name, err, cap) < 0) return MGMT_EINVAL;
    vg_log("mgmt: host name %s", name);
    return MGMT_OK;
}

/* ---- services ---------------------------------------------------------------------- */

void mgmt_services(mgmt_services_t *s)
{
    pthread_mutex_lock(&g_lock);
    *s = g_svc;
    pthread_mutex_unlock(&g_lock);
}

int mgmt_ddns_update(void)
{
    slot_t j = { .kind = J_DDNS };
    return submit(&j);
}

int mgmt_smtp_test(void)
{
    slot_t j = { .kind = J_SMTP_TEST };
    return submit(&j);
}

/* ---- disks ------------------------------------------------------------------------- */

int mgmt_disks(sys_disk_t *out, int max)
{
    vigil_config_t *c;
    cfg_get(&c);
    int n = sys_disk_list(out, max, c && c->record.path[0] ? c->record.path : NULL);
    free(c);
    return n;
}

static int disk_job(jkind_t kind, const char *name, const char *fs, char *err, size_t cap)
{
    slot_t j = { .kind = kind };
    snprintf(j.a, sizeof j.a, "%s", name);
    snprintf(j.b, sizeof j.b, "%s", fs ? fs : "");
    snprintf(j.pub.target, sizeof j.pub.target, "%s", name);
    /* One disk job at a time for a disk. */
    pthread_mutex_lock(&g_lock);
    int busy = 0;
    for (int i = 0; i < JOBS; ++i)
        busy |= g_jobs[i].pub.id && g_jobs[i].pub.state < JOB_DONE && !strcmp(g_jobs[i].pub.target, name) &&
                (g_jobs[i].kind == J_FORMAT || g_jobs[i].kind == J_MOUNT || g_jobs[i].kind == J_UNMOUNT);
    pthread_mutex_unlock(&g_lock);
    if (busy) {
        snprintf(err, cap, "%s is busy with another change", name);
        return MGMT_EBUSY;
    }
    /* A scan reads the disk under the format; a written one has no
     * filesystem left to mount. */
    int scan = scan_active(disk_of(name));
    if (scan >= 0 && (kind == J_FORMAT || scan == SCAN_WRITE)) {
        snprintf(err, cap, "a surface scan (%s) of %s is under way: stop it first", scan_mode_name(scan),
                 disk_of(name));
        return MGMT_EBUSY;
    }
    int id = submit(&j);
    if (id < 0) {
        snprintf(err, cap, "too many changes under way");
        return MGMT_EBUSY;
    }
    return id;
}

int mgmt_disk_format(const char *name, const char *fs, const char *confirm, char *err, size_t cap)
{
    err[0] = 0;
    if (!sys_managed()) return MGMT_EUNMANAGED;
    if (!confirm || strcmp(confirm, name) != 0) {
        snprintf(err, cap, "type the disk's name (%.31s) to confirm", name);
        return MGMT_EINVAL;
    }
    char fss[4][8];
    int n = sys_disk_formats(fss, 4), ok = 0;
    for (int i = 0; i < n; ++i) ok |= !strcmp(fss[i], fs);
    if (!ok) {
        snprintf(err, cap, "this firmware cannot make %.16s", fs);
        return MGMT_EINVAL;
    }
    if (sys_disk_can_format(name, err, cap) < 0) return MGMT_EINVAL;
    return disk_job(J_FORMAT, name, fs, err, cap);
}

int mgmt_disk_mount(const char *name, char *err, size_t cap)
{
    err[0] = 0;
    if (!sys_managed()) return MGMT_EUNMANAGED;
    return disk_job(J_MOUNT, name, NULL, err, cap);
}

int mgmt_disk_unmount(const char *name, char *err, size_t cap)
{
    err[0] = 0;
    if (!sys_managed()) return MGMT_EUNMANAGED;
    sys_disk_t *d = calloc(SYS_DISK_MAX, sizeof *d);
    int n = d ? sys_disk_list(d, SYS_DISK_MAX, NULL) : 0, sys = 0, found = 0;
    for (int i = 0; i < n; ++i)
        if (!strcmp(d[i].name, name)) found = 1, sys = d[i].system;
    free(d);
    if (!found || sys) {
        snprintf(err, cap, found ? "%.31s holds the system" : "no disk %.31s", name);
        return MGMT_EINVAL;
    }
    return disk_job(J_UNMOUNT, name, NULL, err, cap);
}

int mgmt_nas_test(void)
{
    if (!sys_managed()) return MGMT_EUNMANAGED;
    slot_t j = { .kind = J_NAS_TEST };
    return submit(&j);
}

/* ---- disk maintenance ---------------------------------------------------------------- */

#define SMART_CACHE 4

/* The last SMART read of the last few disks, for the screen (a job
 * reads, the page shows what it read) and the scan's confirmation. */
static struct {
    char         name[32];
    time_t       when;
    smart_info_t info;
} g_smart[SMART_CACHE];

/* The disk a device is on: sda1 -> sda (per the disk list; a name that
 * is not a partition is its own). Returns a static buffer. */
static const char *disk_of(const char *name)
{
    static __thread char out[32];
    snprintf(out, sizeof out, "%s", name ? name : "");
    sys_disk_t *d = calloc(SYS_DISK_MAX, sizeof *d);
    int n = d ? sys_disk_list(d, SYS_DISK_MAX, NULL) : 0;
    for (int i = 0; i < n; ++i)
        if (d[i].is_part && !strcmp(d[i].name, out)) snprintf(out, sizeof out, "%s", d[i].parent);
    free(d);
    return out;
}

/* `name` is a whole disk with a medium: 0, else -1 with err. */
static int whole_disk(const char *name, sys_disk_t *out, char *err, size_t cap)
{
    if (!name || !*name || strchr(name, '/')) {
        snprintf(err, cap, "no such disk");
        return -1;
    }
    sys_disk_t *d = calloc(SYS_DISK_MAX, sizeof *d);
    int n = d ? mgmt_disks(d, SYS_DISK_MAX) : 0, rc = -1;
    for (int i = 0; i < n; ++i) {
        if (strcmp(d[i].name, name)) continue;
        if (d[i].is_part) snprintf(err, cap, "%.31s is a partition: choose its disk, %.31s", name, d[i].parent);
        else if (!d[i].size) snprintf(err, cap, "%.31s has no medium", name);
        else {
            if (out) *out = d[i];
            rc = 0;
        }
        break;
    }
    if (rc && !err[0]) snprintf(err, cap, "there is no disk %.31s", name);
    free(d);
    return rc;
}

static void smart_keep(const char *name, const smart_info_t *info)
{
    pthread_mutex_lock(&g_lock);
    int k = 0;
    for (int i = 0; i < SMART_CACHE; ++i) {
        if (!strcmp(g_smart[i].name, name)) {
            k = i;
            break;
        }
        if (g_smart[i].when < g_smart[k].when) k = i;
    }
    snprintf(g_smart[k].name, sizeof g_smart[k].name, "%s", name);
    g_smart[k].info = *info;
    g_smart[k].when = time(NULL);
    pthread_mutex_unlock(&g_lock);
}

int mgmt_smart_cached(const char *name, smart_info_t *out, time_t *when)
{
    int rc = -1;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < SMART_CACHE && name; ++i)
        if (g_smart[i].when && !strcmp(g_smart[i].name, name)) {
            if (out) *out = g_smart[i].info;
            if (when) *when = g_smart[i].when;
            rc = 0;
        }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int mgmt_smart(const char *name, smart_info_t *out, char *err, size_t cap)
{
    err[0] = 0;
    if (!sys_managed()) return MGMT_EUNMANAGED;
    if (whole_disk(name, NULL, err, cap) < 0) return MGMT_EINVAL;
    smart_read(name, out);
    smart_keep(name, out);
    return MGMT_OK;
}

static int smart_job(const char *name, char *msg, size_t cap)
{
    smart_info_t *s = malloc(sizeof *s);
    if (!s) {
        snprintf(msg, cap, "out of memory");
        return -1;
    }
    smart_read(name, s);
    smart_keep(name, s);
    snprintf(msg, cap, "SMART of %s read: %s", name, s->available ? smart_verdict_name(s->verdict) : "not available");
    free(s);
    return 0;
}

int mgmt_smart_job(const char *name, char *err, size_t cap)
{
    err[0] = 0;
    if (!sys_managed()) return MGMT_EUNMANAGED;
    if (whole_disk(name, NULL, err, cap) < 0) return MGMT_EINVAL;
    slot_t j = { .kind = J_SMART };
    snprintf(j.a, sizeof j.a, "%s", name);
    snprintf(j.pub.target, sizeof j.pub.target, "%s", name);
    int id = submit(&j);
    if (id < 0) {
        snprintf(err, cap, "too many changes under way");
        return MGMT_EBUSY;
    }
    return id;
}

int mgmt_selftest(const char *name, int type, char *err, size_t cap)
{
    err[0] = 0;
    if (!sys_managed()) return MGMT_EUNMANAGED;
    if (whole_disk(name, NULL, err, cap) < 0) return MGMT_EINVAL;
    if (smart_selftest(name, type, err, cap) < 0) return MGMT_EFAIL;
    vg_log("mgmt: %s self-test of %s", type == 0 ? "aborted the" : type == 1 ? "short" : "extended", name);
    if (type) events_push(0, "disk %s: %s self-test started", name, type == 1 ? "short" : "extended");
    return MGMT_OK;
}

static void scan_done(const scan_status_t *st)
{
    vg_log("mgmt: %s", st->msg);
    unsigned long long ranges = (unsigned long long)st->n_bad + st->bad_more;
    events_push(0, "disk %s: surface scan (%s) %s, %llu bad range%s", st->disk, scan_mode_name(st->mode),
                st->state == SCAN_DONE ? "done" : st->state == SCAN_STOPPED ? "stopped" : "failed", ranges,
                ranges == 1 ? "" : "s");
}

/* Is the recording directory on `dev` (/mnt/<dev>, as automount.sh and
 * vigild mount)? */
static int records_on(const char *path, const char *dev)
{
    char m[64];
    int n = snprintf(m, sizeof m, "/mnt/%s", dev);
    return path[0] && !strncmp(path, m, (size_t)n) && (path[n] == 0 || path[n] == '/');
}

int mgmt_scan_start(const char *name, int mode, unsigned long long start, unsigned long long end,
                    unsigned block, const char *confirm, char *err, size_t cap)
{
    err[0] = 0;
    if (!sys_managed()) return MGMT_EUNMANAGED;
    if (mode < SCAN_READ || mode > SCAN_WRITE) {
        snprintf(err, cap, "the mode is read, verify or write");
        return MGMT_EINVAL;
    }
    sys_disk_t disk;
    if (whole_disk(name, &disk, err, cap) < 0) return MGMT_EINVAL;
    char mnts[8][128];
    int nm = sys_disk_mounts(name, mnts, 8);
    if (mode == SCAN_WRITE) {
        if (!confirm || strcmp(confirm, name)) {
            snprintf(err, cap, "type the disk's name (%.31s) to confirm that everything on it is erased", name);
            return MGMT_EINVAL;
        }
        if (disk.system) {
            snprintf(err, cap, "%.31s holds the system", name);
            return MGMT_EINVAL;
        }
        if (disk.ro) {
            snprintf(err, cap, "%.31s is read-only", name);
            return MGMT_EINVAL;
        }
        if (nm) {
            snprintf(err, cap, "%.127s is mounted: unmount %.31s first", mnts[0], name);
            return MGMT_EINVAL;
        }
        /* Not mounted now, but recording would come back to it with
         * the disk: the directory goes elsewhere first. */
        vigil_config_t *c;
        cfg_get(&c);
        int rec = 0;
        sys_disk_t *d = calloc(SYS_DISK_MAX, sizeof *d);
        int n = d ? sys_disk_list(d, SYS_DISK_MAX, NULL) : 0;
        for (int i = 0; c && i < n && !rec; ++i)
            if (!strcmp(d[i].name, name) || !strcmp(d[i].parent, name)) rec = records_on(c->record.path, d[i].name);
        free(d);
        free(c);
        if (rec) {
            snprintf(err, cap, "the recording directory is on %.31s: choose another one first", name);
            return MGMT_EINVAL;
        }
    }
    /* Not under a format or another change of the same disk. */
    char targets[JOBS][32];
    int nt = 0, busy = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < JOBS; ++i)
        if (g_jobs[i].pub.id && g_jobs[i].pub.state < JOB_DONE &&
            (g_jobs[i].kind == J_FORMAT || g_jobs[i].kind == J_MOUNT || g_jobs[i].kind == J_UNMOUNT))
            snprintf(targets[nt++], 32, "%s", g_jobs[i].pub.target);
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < nt; ++i) busy |= !strcmp(disk_of(targets[i]), name);
    if (busy) {
        snprintf(err, cap, "%.31s is busy with another change", name);
        return MGMT_EBUSY;
    }
    if (scan_active(NULL) >= 0) {
        scan_status_t *st = malloc(sizeof *st);
        if (st) scan_status(st);
        snprintf(err, cap, "a surface scan of %.31s is under way", st ? st->disk : "a disk");
        free(st);
        return MGMT_EBUSY;
    }
    scan_req_t r = { .mode = mode, .start = start, .end = end, .block = block, .idle = nm > 0, .done = scan_done };
    snprintf(r.disk, sizeof r.disk, "%s", name);
    snprintf(r.model, sizeof r.model, "%s", disk.model);
    smart_info_t *si = malloc(sizeof *si);
    if (si && mgmt_smart_cached(name, si, NULL) == 0 && si->available) {
        snprintf(r.model, sizeof r.model, "%s", si->model);
        snprintf(r.serial, sizeof r.serial, "%s", si->serial);
    }
    free(si);
    if (scan_start(&r, err, cap) < 0) return strstr(err, "under way") ? MGMT_EBUSY : MGMT_EINVAL;
    scan_status_t *st = malloc(sizeof *st);
    if (st) {
        scan_status(st);
        vg_log("mgmt: surface scan (%s) of %s started, LBA %llu..%llu, %u sectors a block%s", scan_mode_name(mode),
               name, (unsigned long long)st->start, (unsigned long long)st->end, st->block,
               nm ? ", at idle I/O priority" : "");
        free(st);
    }
    events_push(0, "disk %s: surface scan (%s) started", name, scan_mode_name(mode));
    return MGMT_OK;
}

void mgmt_scan_status(scan_status_t *st) { scan_status(st); }

int mgmt_scan_pause(int on) { return scan_pause(on) < 0 ? MGMT_EINVAL : MGMT_OK; }

int mgmt_scan_stop(void) { return scan_stop() < 0 ? MGMT_EINVAL : MGMT_OK; }
