/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_logs_bridge.c — the Logs page's source (ui_logs.h) from the core's
 * logbooks (logs.h) and the system log (sys_log.h); exports on a thread
 * of their own, since a long log onto a USB stick takes seconds. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "logs.h"
#include "sys_log.h"
#include "ui_bridge.h"
#include "ui_logs.h"

#define EXPORT_LINES 4096

static void to_filter(const ui_log_filter_t *u, logfilter_t *f)
{
    memset(f, 0, sizeof *f);
    f->from = (time_t)u->from;
    f->to = (time_t)u->to;
    f->ch = u->ch;
    f->type = u->type;
    f->source = u->source;
    f->text = u->text;
}

static void to_row(const logrec_t *r, ui_log_row_t *o)
{
    memset(o, 0, sizeof *o);
    o->time = (long long)r->time;
    o->ch = r->ch;
    o->level = -1;
    o->active = r->active;
    snprintf(o->who, sizeof o->who, "%s", r->who);
    snprintf(o->source, sizeof o->source, "%s", r->source);
    snprintf(o->type, sizeof o->type, "%s", r->type);
    snprintf(o->detail, sizeof o->detail, "%s", r->detail);
}

static logbook_t *book(int kind) { return kind == UI_LOG_ALARMS ? logs_alarms() : logs_ops(); }

static int query(int kind, const ui_log_filter_t *uf, int offset, int limit, ui_log_row_t *out, int *more, char *why,
                 size_t cap)
{
    if (kind == UI_LOG_SYSTEM) {
        sys_logline_t *l = malloc((size_t)limit * sizeof *l);
        if (!l) return -1;
        int n = sys_log_read(uf->kernel ? SYS_LOG_KERNEL : SYS_LOG_SYSLOG, uf->level, uf->text, (time_t)uf->from,
                             (time_t)uf->to, offset, limit, l, more, why, cap);
        for (int i = 0; i < n; ++i) {
            ui_log_row_t *o = &out[i];
            memset(o, 0, sizeof *o);
            o->time = (long long)l[i].time;
            o->level = l[i].level;
            o->active = -1;
            snprintf(o->who, sizeof o->who, "%s", l[i].tag);
            snprintf(o->detail, sizeof o->detail, "%s", l[i].text);
        }
        free(l);
        return n;
    }
    logrec_t *r = malloc((size_t)limit * sizeof *r);
    if (!r) return -1;
    logfilter_t f;
    to_filter(uf, &f);
    int n = logbook_query(book(kind), &f, offset, limit, r, more);
    for (int i = 0; i < n; ++i) to_row(&r[i], &out[i]);
    free(r);
    return n;
}

static void where(int kind, char *out, size_t cap)
{
    if (kind == UI_LOG_SYSTEM) {
        snprintf(out, cap, "The firmware's syslog ring and the kernel's, in RAM: what is older is gone. "
                           "Export saves what is shown.");
        return;
    }
    char disk[300];
    logbook_disk(book(kind), disk, sizeof disk);
    if (disk[0]) snprintf(out, cap, "Kept on the recording disk, %s; the newest also in memory.", disk);
    else if (kind == UI_LOG_OPS)
        snprintf(out, cap, "No recording disk: kept in memory, the last %d on the flash (every %d min and at a clean "
                           "stop).", LOGS_FLASH_OPS, LOGS_FLASH_SECS / 60);
    else
        snprintf(out, cap, "No recording disk: kept in memory, the last %d on the flash (every %d min and at a clean "
                           "stop).", LOGS_FLASH_ALARMS, LOGS_FLASH_SECS / 60);
}

static unsigned long count(int kind) { return kind == UI_LOG_SYSTEM ? 0 : logbook_count(book(kind)); }

static int types(int kind, const char **out, int max)
{
    if (kind == UI_LOG_ALARMS) return alarm_types(out, max);
    int n = 0;
    for (; n < max && oplog_actions[n]; ++n) out[n] = oplog_actions[n];
    return n;
}

/* Mounted disks and sticks a file may go to: /mnt/... and /media/...,
 * writable. */
static int export_targets(char out[][96], int max)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return 0;
    int n = 0;
    char dev[256], dir[256], fs[64], opt[256];
    while (n < max && fscanf(f, "%255s %255s %63s %255s %*d %*d", dev, dir, fs, opt) == 4) {
        if (strncmp(dir, "/mnt/", 5) && strncmp(dir, "/media/", 7)) continue;
        if (!strncmp(opt, "ro", 2) && (opt[2] == ',' || !opt[2])) continue;
        if (access(dir, W_OK) < 0 || strlen(dir) >= sizeof out[0]) continue;
        int dup = 0;
        for (int i = 0; i < n; ++i) dup |= !strcmp(out[i], dir);
        if (!dup) memcpy(out[n++], dir, strlen(dir) + 1);
    }
    fclose(f);
    return n;
}

/* ---- the export ------------------------------------------------------------------------------- */

static struct {
    pthread_mutex_t lock;
    int             running, done;
    long            count;
    char            msg[320];
    /* the job */
    int             kind, csv;
    ui_log_filter_t f;
    char            path[320];
} X = { .lock = PTHREAD_MUTEX_INITIALIZER };

typedef struct {
    FILE *f;
    int   csv;
    long  n;
} wr_t;

static int write_cb(void *user, const logrec_t *r)
{
    wr_t *w = user;
    char line[1200];
    int n = w->csv ? logbook_csv(r, 0, line, sizeof line) : logbook_text(r, line, sizeof line);
    if (n > 0 && fwrite(line, 1, (size_t)n, w->f) != (size_t)n) return 1;
    ++w->n;
    return 0;
}

static long export_system(FILE *f, const ui_log_filter_t *uf, int csv, char *err, size_t cap)
{
    sys_logline_t *l = malloc(EXPORT_LINES * sizeof *l);
    if (!l) {
        snprintf(err, cap, "out of memory");
        return -1;
    }
    int n = sys_log_read(uf->kernel ? SYS_LOG_KERNEL : SYS_LOG_SYSLOG, uf->level, uf->text, (time_t)uf->from,
                         (time_t)uf->to, 0, EXPORT_LINES, l, NULL, err, cap);
    long k = 0;
    if (csv && n >= 0) fputs("time,level,tag,text\n", f);
    for (int i = n - 1; i >= 0; --i) {      /* oldest first, as a log file reads */
        char when[32] = "-", line[600];
        struct tm tm;
        if (l[i].time && localtime_r(&l[i].time, &tm)) strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
        if (csv) {
            const char *cells[4] = { l[i].time ? when : "", sys_log_level_name(l[i].level), l[i].tag, l[i].text };
            if (logbook_csv_row(cells, 4, line, sizeof line) < 0) continue;
        } else {
            snprintf(line, sizeof line, "%s %-7s %s: %s\n", when, l[i].level >= 0 ? sys_log_level_name(l[i].level) : "-",
                     l[i].tag, l[i].text);
        }
        fputs(line, f);
        ++k;
    }
    free(l);
    return n < 0 ? -1 : k;
}

static void *export_thread(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&X.lock);
    int kind = X.kind, csv = X.csv;
    ui_log_filter_t uf = X.f;
    char path[320];
    snprintf(path, sizeof path, "%s", X.path);
    pthread_mutex_unlock(&X.lock);

    char err[200] = "";
    long n = -1;
    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(err, sizeof err, "%.150s: %s", path, strerror(errno));
    } else if (kind == UI_LOG_SYSTEM) {
        n = export_system(f, &uf, csv, err, sizeof err);
    } else {
        logfilter_t lf;
        to_filter(&uf, &lf);
        wr_t w = { f, csv, 0 };
        if (csv) fputs("time,channel,state,source,user,type,detail,id\n", f);
        logbook_each(book(kind), &lf, write_cb, &w);
        n = w.n;
    }
    if (f && (fflush(f) != 0 || fsync(fileno(f)) != 0 || ferror(f))) {
        snprintf(err, sizeof err, "%.150s: %s", path, strerror(errno));
        n = -1;
    }
    if (f) fclose(f);
    if (n < 0 && f) unlink(path);
    if (n >= 0)
        oplog_pushf(NULL, "screen", "log_export", "%s log, %s, %ld entries, to %s",
                    kind == UI_LOG_SYSTEM ? "system" : kind == UI_LOG_ALARMS ? "alarm" : "operation", csv ? "CSV" : "text",
                    n, path);
    pthread_mutex_lock(&X.lock);
    X.count = n;
    snprintf(X.msg, sizeof X.msg, "%s", n >= 0 ? path : err);
    X.running = 0;
    X.done = 1;
    pthread_mutex_unlock(&X.lock);
    return NULL;
}

static int export_start(int kind, const ui_log_filter_t *f, int csv, const char *dir, char *err, size_t cap)
{
    pthread_mutex_lock(&X.lock);
    if (X.running) {
        pthread_mutex_unlock(&X.lock);
        snprintf(err, cap, "one is under way");
        return -1;
    }
    char when[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(when, sizeof when, "%Y%m%d-%H%M%S", &tm);
    static const char *const name[UI_LOG_KINDS] = { "system", "alarms", "operations" };
    snprintf(X.path, sizeof X.path, "%s/vigil-%s-%s.%s", dir, name[kind < 0 || kind >= UI_LOG_KINDS ? 0 : kind], when,
             csv ? "csv" : "txt");
    X.kind = kind;
    X.csv = csv;
    X.f = *f;
    X.running = 1;
    X.done = 0;
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &a, export_thread, NULL);
    pthread_attr_destroy(&a);
    if (rc != 0) {
        X.running = 0;
        snprintf(err, cap, "cannot start a thread");
    }
    pthread_mutex_unlock(&X.lock);
    return rc ? -1 : 0;
}

static int export_poll(int *done, long *n, char *msg, size_t cap)
{
    pthread_mutex_lock(&X.lock);
    int running = X.running;
    *done = X.done;
    if (X.done) {
        *n = X.count;
        snprintf(msg, cap, "%s", X.msg);
        X.done = 0;
    }
    pthread_mutex_unlock(&X.lock);
    return running;
}

static void audit(const char *action, const char *detail)
{
    oplog_push(NULL, "screen", action, detail);
    if (!strcmp(action, "logout")) oplog_actor("", "screen", "");
}

static const ui_logs_src_t g_src = {
    .query = query, .where = where, .count = count, .types = types, .export_targets = export_targets,
    .export_start = export_start, .export_poll = export_poll, .audit = audit,
};

const ui_logs_src_t *ui_logs_bridge(void)
{
    /* The screen's actions happen on this thread: the operation log's
     * actor (logs.h), its user once someone logs in. */
    oplog_actor("", "screen", "");
    return &g_src;
}
