/* SPDX-License-Identifier: GPL-3.0-or-later */
/* logs.c — the alarm log and the operation log (see logs.h). */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <libgen.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "config.h"
#include "log.h"
#include "logs.h"
#include "rec_place.h"

#define TICK_MS       500
#define SYNC_SECS     1
#define DISK_SECS     10            /* the recording disk looked for this often */
#define SEEN_N        16            /* logins remembered for oplog_login's quiet time */

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static logbook_t     *g_alarms, *g_ops;

static void books_init(void)
{
    logbook_conf_t a = { .name = "alarms", .ram = LOGS_ALARMS_RAM, .flash_keep = LOGS_FLASH_ALARMS,
                         .flash_secs = LOGS_FLASH_SECS, .flash_no_disk = 1, .file_cap = LOGS_ALARMS_FILE,
                         .files = 8 };
    logbook_conf_t o = { .name = "operations", .ram = LOGS_OPS_RAM, .flash_keep = LOGS_FLASH_OPS,
                         .flash_secs = LOGS_FLASH_SECS, .flash_no_disk = 0, .file_cap = LOGS_OPS_FILE,
                         .files = 4 };
    g_alarms = logbook_create(&a);
    g_ops = logbook_create(&o);
}

logbook_t *logs_alarms(void)
{
    pthread_once(&g_once, books_init);
    return g_alarms;
}

logbook_t *logs_ops(void)
{
    pthread_once(&g_once, books_init);
    return g_ops;
}

/* ---- the actor -------------------------------------------------------------------------- */

static __thread struct {
    int  set;
    char user[LOG_WHO], source[LOG_SOURCE], addr[48];
} t_actor;

void oplog_actor(const char *user, const char *source, const char *addr)
{
    if (!t_actor.set) {
        snprintf(t_actor.source, sizeof t_actor.source, "system");
        t_actor.set = 1;
    }
    if (user) snprintf(t_actor.user, sizeof t_actor.user, "%s", user);
    if (source) snprintf(t_actor.source, sizeof t_actor.source, "%s", source);
    if (addr) snprintf(t_actor.addr, sizeof t_actor.addr, "%s", addr);
}

void oplog_actor_get(char *user, size_t ucap, char *source, size_t scap, char *addr, size_t acap)
{
    if (user) snprintf(user, ucap, "%s", t_actor.set ? t_actor.user : "");
    if (source) snprintf(source, scap, "%s", t_actor.set ? t_actor.source : "system");
    if (addr) snprintf(addr, acap, "%s", t_actor.set ? t_actor.addr : "");
}

/* ---- the operation log ------------------------------------------------------------------ */

const char *const oplog_actions[] = {
    "login", "login_failed", "lockout", "logout", "config_saved", "user_added", "user_changed", "user_deleted",
    "group_added", "group_changed", "group_deleted", "password_changed", "reboot", "shutdown", "start", "stop",
    "disk_format", "disk_mount", "disk_unmount", "scan_start", "scan_pause", "scan_resume", "scan_stop",
    "smart_selftest", "network_changed", "network_confirmed", "network_rolled_back", "wifi_changed",
    "time_zone_set", "clock_set", "ntp_servers_set", "hostname_set", "log_export",
    /* the hooks others call */
    "firmware_upgrade", "config_import", "config_export", "config_reset",
    NULL,
};

/* A user name as typed at a failed login is anyone's text: printable
 * ASCII only, so that the log reads the same everywhere. */
static void clean(char *s)
{
    for (; *s; ++s)
        if ((unsigned char)*s < 0x20 || (unsigned char)*s >= 0x7f) *s = '?';
}

void oplog_push(const char *user, const char *source, const char *action, const char *detail)
{
    logrec_t r;
    memset(&r, 0, sizeof r);
    r.active = -1;
    oplog_actor_get(r.who, sizeof r.who, r.source, sizeof r.source, NULL, 0);
    if (user) snprintf(r.who, sizeof r.who, "%s", user);
    if (source) snprintf(r.source, sizeof r.source, "%s", source);
    snprintf(r.type, sizeof r.type, "%s", action ? action : "?");
    snprintf(r.detail, sizeof r.detail, "%s", detail ? detail : "");
    clean(r.who);
    logbook_push(logs_ops(), &r);
}

void oplog_pushf(const char *user, const char *source, const char *action, const char *fmt, ...)
{
    char d[LOG_DETAIL];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d, sizeof d, fmt, ap);
    va_end(ap);
    oplog_push(user, source, action, d);
}

static struct {
    char   user[LOG_WHO], source[LOG_SOURCE], addr[48];
    time_t last;                    /* monotonic; 0: free */
} g_seen[SEEN_N];
static pthread_mutex_t g_seen_lock = PTHREAD_MUTEX_INITIALIZER;

static time_t mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

/* Logged in recently enough not to log it again? Marks it seen now. */
static int seen_recently(const char *user, const char *source, const char *addr, int quiet)
{
    time_t now = mono();
    pthread_mutex_lock(&g_seen_lock);
    int k = 0, recent = 0;
    for (int i = 0; i < SEEN_N; ++i) {
        if (g_seen[i].last && !strcmp(g_seen[i].user, user) && !strcmp(g_seen[i].source, source) &&
            !strcmp(g_seen[i].addr, addr)) {
            recent = now - g_seen[i].last < quiet;
            k = i;
            break;
        }
        if (g_seen[i].last < g_seen[k].last) k = i;
    }
    snprintf(g_seen[k].user, sizeof g_seen[k].user, "%s", user);
    snprintf(g_seen[k].source, sizeof g_seen[k].source, "%s", source);
    snprintf(g_seen[k].addr, sizeof g_seen[k].addr, "%s", addr);
    g_seen[k].last = now;
    pthread_mutex_unlock(&g_seen_lock);
    return recent;
}

void oplog_login(const char *user, int ok, int fails, int quiet_secs)
{
    char source[LOG_SOURCE], addr[48], from[64] = "";
    oplog_actor_get(NULL, 0, source, sizeof source, addr, sizeof addr);
    if (addr[0]) snprintf(from, sizeof from, "from %s", addr);
    if (!user) user = "";
    if (ok) {
        oplog_actor(user, NULL, NULL);
        if (quiet_secs > 0 && seen_recently(user, source, addr, quiet_secs)) return;
        oplog_push(user, source, "login", from);
        return;
    }
    if (fails > 0) oplog_pushf(user, source, "login_failed", "%s%s%d in a row", from, from[0] ? ", " : "", fails);
    else oplog_push(user, source, "login_failed", from);
}

void oplog_lockout(int fails, long secs)
{
    char source[LOG_SOURCE], addr[48];
    oplog_actor_get(NULL, 0, source, sizeof source, addr, sizeof addr);
    oplog_pushf("", source, "lockout", "%s%s refused for %ld s after %d failed logins", addr[0] ? "from " : "",
                addr, secs, fails);
}

/* ---- settings saved: which fields changed -------------------------------------------------- */

static void add_name(char *out, size_t cap, const char *name)
{
    size_t n = strlen(out);
    if (n + 1 >= cap) return;
    snprintf(out + n, cap - n, "%s%s", n && out[n - 1] != ' ' ? ", " : "", name);
}

/* The keys of objects `a` and `b` whose values differ, into `out`. */
static void diff_object(const cJSON *a, const cJSON *b, char *out, size_t cap)
{
    for (const cJSON *x = b ? b->child : NULL; x; x = x->next) {
        const cJSON *y = cJSON_GetObjectItemCaseSensitive(a, x->string);
        if (!y || !cJSON_Compare(x, y, 1)) add_name(out, cap, x->string);
    }
    for (const cJSON *y = a ? a->child : NULL; y; y = y->next)
        if (!cJSON_GetObjectItemCaseSensitive(b, y->string)) add_name(out, cap, y->string);
}

/* What changed in a section: field names, for an array its size and its
 * items' fields ("2 -> 3 items; [1] url, name"). */
static void diff_section(const cJSON *a, const cJSON *b, char *out, size_t cap)
{
    out[0] = 0;
    if (cJSON_IsArray(a) && cJSON_IsArray(b)) {
        int na = cJSON_GetArraySize(a), nb = cJSON_GetArraySize(b);
        if (na != nb) snprintf(out, cap, "%d -> %d items; ", na, nb);
        for (int i = 0; i < na && i < nb; ++i) {
            const cJSON *x = cJSON_GetArrayItem(a, i), *y = cJSON_GetArrayItem(b, i);
            if (cJSON_Compare(x, y, 1)) continue;
            char f[LOG_DETAIL] = "";
            diff_object(x, y, f, sizeof f);
            size_t n = strlen(out);
            snprintf(out + n, cap - n, "%s[%d] %.100s", n && out[n - 1] != ' ' ? "; " : "", i + 1, f);
        }
        size_t n = strlen(out);
        while (n && (out[n - 1] == ' ' || out[n - 1] == ';')) out[--n] = 0;
        return;
    }
    diff_object(a, b, out, cap);
}

void oplog_config_saved(const char *section, const void *old, const void *nc)
{
    char detail[LOG_DETAIL] = "";
    if (section) {
        const schema_section_t *sec = schema_section(&config_schema, section);
        cJSON *a = sec ? schema_section_to_json(sec, old, 0) : NULL;
        cJSON *b = sec ? schema_section_to_json(sec, nc, 0) : NULL;
        char f[LOG_DETAIL];
        diff_section(a, b, f, sizeof f);
        snprintf(detail, sizeof detail, "%.24s: %.100s", section, f[0] ? f : "unchanged");
        cJSON_Delete(a);
        cJSON_Delete(b);
    } else {
        /* The whole document: which sections changed. */
        char f[LOG_DETAIL] = "";
        for (int i = 0; i < config_schema.n_sections; ++i) {
            const schema_section_t *sec = &config_schema.sections[i];
            cJSON *a = schema_section_to_json(sec, old, 0), *b = schema_section_to_json(sec, nc, 0);
            if (!cJSON_Compare(a, b, 1)) add_name(f, sizeof f, sec->key);
            cJSON_Delete(a);
            cJSON_Delete(b);
        }
        snprintf(detail, sizeof detail, "all: %.120s", f[0] ? f : "unchanged");
    }
    oplog_push(NULL, NULL, "config_saved", detail);
}

/* ---- the alarm log ------------------------------------------------------------------------- */

void alarmlog_push(int ch, const char *type, int active, const char *detail)
{
    logrec_t r;
    memset(&r, 0, sizeof r);
    r.ch = ch;
    r.active = active;
    snprintf(r.source, sizeof r.source, "vigild");
    snprintf(r.type, sizeof r.type, "%s", type ? type : "?");
    snprintf(r.detail, sizeof r.detail, "%s", detail ? detail : "");
    logbook_push(logs_alarms(), &r);
}

void alarmlog_pushf(int ch, const char *type, int active, const char *fmt, ...)
{
    char d[LOG_DETAIL];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d, sizeof d, fmt, ap);
    va_end(ap);
    alarmlog_push(ch, type, active, d);
}

/* ---- the disk and the thread ----------------------------------------------------------------- */

int logs_disk_dir(const char *rec_path, int allow_rootfs, char *out, size_t cap)
{
    if (!rec_path || rec_path[0] != '/') return -1;
    struct stat rs, ps;
    if (stat(rec_path, &rs) < 0 || !S_ISDIR(rs.st_mode)) return -1;     /* not recording there yet */
    char parent[256], cand[300];
    snprintf(parent, sizeof parent, "%s", rec_path);
    size_t n = strlen(parent);
    while (n > 1 && parent[n - 1] == '/') parent[--n] = 0;
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) *slash = 0;
    else parent[1] = 0;
    if (strcmp(parent, "/") && stat(parent, &ps) == 0 && ps.st_dev == rs.st_dev)
        snprintf(cand, sizeof cand, "%s/" LOGS_DIR, parent);
    else
        snprintf(cand, sizeof cand, "%s/" LOGS_DIR, rec_path);
    char why[160];
    if (rec_place_check(cand, allow_rootfs, NULL, why, sizeof why) != REC_PLACE_OK) return -1;
    snprintf(out, cap, "%s", cand);
    return 0;
}

static pthread_t       g_thread;
static int             g_running;
static volatile int    g_stop;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_wake = PTHREAD_COND_INITIALIZER;
static time_t          g_sync_at, g_disk_at;
static char            g_disk[300];

static void disk_check(void)
{
    vigil_config_t *c = malloc(sizeof *c);
    if (!c) return;
    config_get(c);
    char dir[300] = "";
    if (logs_disk_dir(c->record.path, c->record.allow_rootfs, dir, sizeof dir) < 0) dir[0] = 0;
    free(c);
    char a[300], o[300];
    logbook_disk(logs_alarms(), a, sizeof a);
    logbook_disk(logs_ops(), o, sizeof o);
    /* A new place, or one a book let go after a failure. */
    if (!strcmp(dir, g_disk) && !strcmp(a, dir) && !strcmp(o, dir)) return;
    if (dir[0]) {
        int ra = logbook_set_disk(logs_alarms(), dir), ro = logbook_set_disk(logs_ops(), dir);
        if (ra < 0 || ro < 0) {
            if (strcmp(dir, g_disk)) vg_log("logs: cannot keep the logs in %s: %s", dir, strerror(errno));
        } else {
            vg_log("logs: kept in %s", dir);
        }
    } else {
        logbook_set_disk(logs_alarms(), NULL);
        logbook_set_disk(logs_ops(), NULL);
        if (g_disk[0]) vg_log("logs: %s is gone; kept in memory (and a copy on flash)", g_disk);
    }
    snprintf(g_disk, sizeof g_disk, "%s", dir);
}

static void (*g_feed)(time_t now);

void logs_tick(time_t now, int final)
{
    if (g_feed) g_feed(now);
    if (final || now - g_disk_at >= DISK_SECS) {
        g_disk_at = now;
        disk_check();
    }
    if (final || now - g_sync_at >= SYNC_SECS) {
        g_sync_at = now;
        logbook_sync(logs_alarms(), now, final);
        logbook_sync(logs_ops(), now, final);
    }
}

static void *logs_thread(void *arg)
{
    (void)arg;
    oplog_actor("", "system", "");
    pthread_mutex_lock(&g_lock);
    while (!g_stop) {
        pthread_mutex_unlock(&g_lock);
        logs_tick(mono(), 0);
        pthread_mutex_lock(&g_lock);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += TICK_MS * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        if (!g_stop) pthread_cond_timedwait(&g_wake, &g_lock, &ts);
    }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

int logs_start(const char *dir, void (*feed)(time_t now))
{
    if (g_running) return 0;
    g_feed = feed;
    if (dir && *dir) {
        char path[320];
        mkdir(dir, 0700);
        snprintf(path, sizeof path, "%s/alarms-recent.log", dir);
        logbook_set_flash(logs_alarms(), path);
        snprintf(path, sizeof path, "%s/operations-recent.log", dir);
        logbook_set_flash(logs_ops(), path);
    }
    g_stop = 0;
    g_sync_at = g_disk_at = 0;
    if (pthread_create(&g_thread, NULL, logs_thread, NULL) != 0) return -1;
    g_running = 1;
    return 0;
}

void logs_stop(void)
{
    if (!g_running) return;
    pthread_mutex_lock(&g_lock);
    g_stop = 1;
    pthread_cond_broadcast(&g_wake);
    pthread_mutex_unlock(&g_lock);
    pthread_join(g_thread, NULL);
    g_running = 0;
    logs_tick(mono(), 1);
}
