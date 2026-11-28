/* SPDX-License-Identifier: GPL-3.0-or-later */
/* api_logs.c — /api/v1/logs...: the system log, the alarm log and the
 * operation log, as JSON pages or exported whole (docs/api.md, "Logs"). */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "civetweb.h"
#include "cJSON.h"

#include "api_int.h"
#include "logs.h"
#include "sys_log.h"

#define LIMIT_DEF   100
#define LIMIT_MAX   1000
#define LINES_DEF   200
#define LINES_MAX   2000
#define EXPORT_LINES 4096           /* SYS_LOG_MAX holds fewer lines than that */

static int qvar(struct mg_connection *conn, const char *name, char *out, size_t cap)
{
    const char *q = mg_get_request_info(conn)->query_string;
    out[0] = 0;
    return q && mg_get_var(q, strlen(q), name, out, cap) >= 0;
}

static long qint(struct mg_connection *conn, const char *name, long dflt, long lo, long hi)
{
    char v[24];
    if (!qvar(conn, name, v, sizeof v) || !v[0]) return dflt;
    long n = strtol(v, NULL, 10);
    return n < lo ? lo : n > hi ? hi : n;
}

/* A time: seconds since 1970, or local "YYYY-MM-DD[THH:MM[:SS]]" (a
 * space for the T too). 0 when empty, -1 when not one. */
static time_t parse_time(const char *s, int end_of_day)
{
    if (!*s) return 0;
    char *e;
    long long n = strtoll(s, &e, 10);
    if (!*e && n > 0) return (time_t)n;
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    int y, mo, d, h = 0, mi = 0, sec = 0;
    int got = sscanf(s, "%d-%d-%d%*[T ]%d:%d:%d", &y, &mo, &d, &h, &mi, &sec);
    if (got < 3) return -1;
    if (got == 3 && end_of_day) h = 23, mi = 59, sec = 59;
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

/* from, to, range (today, 24h, 7d): -1 with the bad one's name. */
static int time_range(struct mg_connection *conn, time_t *from, time_t *to, const char **bad)
{
    char v[40];
    *from = *to = 0;
    qvar(conn, "range", v, sizeof v);
    time_t now = time(NULL);
    if (!strcmp(v, "today")) {
        struct tm tm;
        localtime_r(&now, &tm);
        tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
        tm.tm_isdst = -1;
        *from = mktime(&tm);
    } else if (!strcmp(v, "24h")) {
        *from = now - 86400;
    } else if (!strcmp(v, "7d")) {
        *from = now - 7 * 86400;
    } else if (v[0] && strcmp(v, "all")) {
        *bad = "range";
        return -1;
    }
    qvar(conn, "from", v, sizeof v);
    if (v[0] && (*from = parse_time(v, 0)) < 0) {
        *bad = "from";
        return -1;
    }
    qvar(conn, "to", v, sizeof v);
    if (v[0] && (*to = parse_time(v, 1)) < 0) {
        *bad = "to";
        return -1;
    }
    return 0;
}

static int bad_param(struct mg_connection *conn, const char *name)
{
    char msg[64];
    snprintf(msg, sizeof msg, "bad parameter: %s", name);
    return send_error(conn, 400, msg, NULL);
}

static cJSON *rec_json(const logrec_t *r, int ops)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "id", r->id);
    cJSON_AddNumberToObject(o, "time", (double)r->time);
    if (ops) {
        cJSON_AddStringToObject(o, "user", r->who);
        cJSON_AddStringToObject(o, "source", r->source);
        cJSON_AddStringToObject(o, "action", r->type);
    } else {
        if (r->ch > 0) cJSON_AddNumberToObject(o, "channel", r->ch);
        else cJSON_AddNullToObject(o, "channel");
        cJSON_AddStringToObject(o, "type", r->type);
        cJSON_AddStringToObject(o, "source", r->source);
        if (r->active >= 0) cJSON_AddStringToObject(o, "state", r->active ? "start" : "stop");
        else cJSON_AddNullToObject(o, "state");
    }
    cJSON_AddStringToObject(o, "detail", r->detail);
    return o;
}

/* The filter of a request to the alarm (ops 0) or the operation log. */
typedef struct {
    logfilter_t f;
    char        type[200], who[40], source[16], text[100];
} req_filter_t;

static int filter_of(struct mg_connection *conn, int ops, req_filter_t *rf, const char **bad)
{
    memset(rf, 0, sizeof *rf);
    rf->f.ch = -1;
    if (time_range(conn, &rf->f.from, &rf->f.to, bad) < 0) return -1;
    qvar(conn, ops ? "action" : "type", rf->type, sizeof rf->type);
    qvar(conn, "q", rf->text, sizeof rf->text);
    if (ops) {
        qvar(conn, "user", rf->who, sizeof rf->who);
        qvar(conn, "source", rf->source, sizeof rf->source);
    } else {
        rf->f.ch = (int)qint(conn, "channel", -1, -1, 64);
    }
    rf->f.type = rf->type;
    rf->f.who = rf->who;
    rf->f.source = rf->source;
    rf->f.text = rf->text;
    return 0;
}

static int send_page(struct mg_connection *conn, int ops)
{
    req_filter_t rf;
    const char *bad = NULL;
    if (filter_of(conn, ops, &rf, &bad) < 0) return bad_param(conn, bad);
    int limit = (int)qint(conn, "limit", LIMIT_DEF, 1, LIMIT_MAX), offset = (int)qint(conn, "offset", 0, 0, 1 << 30);
    logrec_t *recs = malloc((size_t)limit * sizeof *recs);
    if (!recs) return send_error(conn, 500, "out of memory", NULL);
    int more = 0;
    logbook_t *b = ops ? logs_ops() : logs_alarms();
    int n = logbook_query(b, &rf.f, offset, limit, recs, &more);
    cJSON *doc = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(doc, "entries");
    for (int i = 0; list && i < n; ++i) cJSON_AddItemToArray(list, rec_json(&recs[i], ops));
    free(recs);
    cJSON_AddNumberToObject(doc, "offset", offset);
    cJSON_AddNumberToObject(doc, "count", n);
    cJSON_AddBoolToObject(doc, "more", more);
    char disk[300];
    logbook_disk(b, disk, sizeof disk);
    cJSON_AddStringToObject(doc, "disk", disk);
    return send_json(conn, 200, doc);
}

/* ---- the system log ------------------------------------------------------------------------- */

static int system_filter(struct mg_connection *conn, sys_log_src_t *src, int *level, char *grep, size_t cap,
                         time_t *from, time_t *to, const char **bad)
{
    char v[24];
    if (time_range(conn, from, to, bad) < 0) return -1;
    qvar(conn, "source", v, sizeof v);
    if (!v[0] || !strcmp(v, "syslog")) *src = SYS_LOG_SYSLOG;
    else if (!strcmp(v, "kernel") || !strcmp(v, "dmesg")) *src = SYS_LOG_KERNEL;
    else {
        *bad = "source";
        return -1;
    }
    qvar(conn, "level", v, sizeof v);
    *level = -1;
    if (v[0]) {
        for (int i = 0; i < 8 && *level < 0; ++i)
            if (!strcmp(v, sys_log_level_name(i))) *level = i;
        if (*level < 0 && v[0] >= '0' && v[0] <= '7' && !v[1]) *level = v[0] - '0';
        if (*level < 0) {
            *bad = "level";
            return -1;
        }
    }
    qvar(conn, "grep", grep, cap);
    return 0;
}

static int send_system(struct mg_connection *conn)
{
    sys_log_src_t src;
    int level;
    char grep[100];
    time_t from, to;
    const char *bad = NULL;
    if (system_filter(conn, &src, &level, grep, sizeof grep, &from, &to, &bad) < 0) return bad_param(conn, bad);
    int lines = (int)qint(conn, "lines", LINES_DEF, 1, LINES_MAX), offset = (int)qint(conn, "offset", 0, 0, 1 << 30);
    sys_logline_t *l = malloc((size_t)lines * sizeof *l);
    if (!l) return send_error(conn, 500, "out of memory", NULL);
    int more = 0;
    char why[200];
    int n = sys_log_read(src, level, grep, from, to, offset, lines, l, &more, why, sizeof why);
    if (n < 0) {
        free(l);
        cJSON *doc = cJSON_CreateObject();
        cJSON_AddStringToObject(doc, "error", why);
        return send_json(conn, 503, doc);
    }
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "source", src == SYS_LOG_KERNEL ? "kernel" : "syslog");
    cJSON *list = cJSON_AddArrayToObject(doc, "lines");
    for (int i = 0; list && i < n; ++i) {
        cJSON *o = cJSON_CreateObject();
        if (!o) break;
        if (l[i].time) cJSON_AddNumberToObject(o, "time", (double)l[i].time);
        else cJSON_AddNullToObject(o, "time");
        if (l[i].level >= 0) cJSON_AddStringToObject(o, "level", sys_log_level_name(l[i].level));
        else cJSON_AddNullToObject(o, "level");
        cJSON_AddStringToObject(o, "tag", l[i].tag);
        cJSON_AddStringToObject(o, "text", l[i].text);
        cJSON_AddItemToArray(list, o);
    }
    free(l);
    cJSON_AddNumberToObject(doc, "offset", offset);
    cJSON_AddNumberToObject(doc, "count", n);
    cJSON_AddBoolToObject(doc, "more", more);
    return send_json(conn, 200, doc);
}

/* ---- exports ------------------------------------------------------------------------------- */

typedef struct {
    struct mg_connection *conn;
    int                   csv, ok;
    unsigned long         n;
    char                  buf[16384];
    size_t                len;
} out_t;

static void out_flush(out_t *o)
{
    if (o->len && o->ok && mg_write(o->conn, o->buf, o->len) != (int)o->len) o->ok = 0;
    o->len = 0;
}

static void out_put(out_t *o, const char *s, size_t n)
{
    if (o->len + n > sizeof o->buf) out_flush(o);
    if (n > sizeof o->buf) return;
    memcpy(o->buf + o->len, s, n);
    o->len += n;
}

static int export_cb(void *user, const logrec_t *r)
{
    out_t *o = user;
    char line[1200];
    int n = o->csv ? logbook_csv(r, 0, line, sizeof line) : logbook_text(r, line, sizeof line);
    if (n > 0) out_put(o, line, (size_t)n);
    ++o->n;
    return !o->ok;
}

/* The header of a download: no length (it ends with the connection). */
static void export_header(struct mg_connection *conn, const char *name, int csv)
{
    char disp[160], when[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(when, sizeof when, "%Y%m%d-%H%M%S", &tm);
    snprintf(disp, sizeof disp, "attachment; filename=\"vigil-%s-%s.%s\"", name, when, csv ? "csv" : "txt");
    mg_response_header_start(conn, 200);
    mg_response_header_add(conn, "Content-Type", csv ? "text/csv; charset=utf-8" : "text/plain; charset=utf-8",
                           -1);
    mg_response_header_add(conn, "Content-Disposition", disp, -1);
    mg_response_header_add(conn, "Cache-Control", "no-store", -1);
    mg_response_header_add(conn, "Connection", "close", -1);
    mg_response_header_send(conn);
}

static int format_of(struct mg_connection *conn, int *csv)
{
    char v[16];
    qvar(conn, "format", v, sizeof v);
    if (!v[0] || !strcmp(v, "csv")) *csv = 1;
    else if (!strcmp(v, "text") || !strcmp(v, "txt")) *csv = 0;
    else return -1;
    return 0;
}

static int export_log(struct mg_connection *conn, int ops)
{
    req_filter_t rf;
    const char *bad = NULL;
    int csv;
    if (format_of(conn, &csv) < 0) return bad_param(conn, "format");
    if (filter_of(conn, ops, &rf, &bad) < 0) return bad_param(conn, bad);
    out_t *o = calloc(1, sizeof *o);
    if (!o) return send_error(conn, 500, "out of memory", NULL);
    o->conn = conn;
    o->csv = csv;
    o->ok = 1;
    export_header(conn, ops ? "operations" : "alarms", csv);
    if (strcmp(mg_get_request_info(conn)->request_method, "HEAD")) {
        if (csv) out_put(o, "time,channel,state,source,user,type,detail,id\n", 46);
        logbook_each(ops ? logs_ops() : logs_alarms(), &rf.f, export_cb, o);
        out_flush(o);
    }
    free(o);
    oplog_pushf(NULL, NULL, "log_export", "%s log, %s, to a browser", ops ? "operation" : "alarm", csv ? "CSV" : "text");
    return 200;
}

static int export_system(struct mg_connection *conn)
{
    sys_log_src_t src;
    int level, csv;
    char grep[100];
    time_t from, to;
    const char *bad = NULL;
    if (format_of(conn, &csv) < 0) return bad_param(conn, "format");
    if (system_filter(conn, &src, &level, grep, sizeof grep, &from, &to, &bad) < 0) return bad_param(conn, bad);
    sys_logline_t *l = malloc(EXPORT_LINES * sizeof *l);
    if (!l) return send_error(conn, 500, "out of memory", NULL);
    char why[200];
    int n = sys_log_read(src, level, grep, from, to, 0, EXPORT_LINES, l, NULL, why, sizeof why);
    if (n < 0) {
        free(l);
        return send_error(conn, 503, why, NULL);
    }
    out_t *o = calloc(1, sizeof *o);
    if (!o) {
        free(l);
        return send_error(conn, 500, "out of memory", NULL);
    }
    o->conn = conn;
    o->ok = 1;
    export_header(conn, src == SYS_LOG_KERNEL ? "kernel" : "syslog", csv);
    if (csv) out_put(o, "time,level,tag,text\n", 20);
    /* Oldest first, as a log file reads. */
    for (int i = n - 1; i >= 0 && o->ok; --i) {
        char when[32] = "", line[600];
        struct tm tm;
        if (l[i].time && localtime_r(&l[i].time, &tm)) strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
        int k;
        if (csv) {
            const char *cells[4] = { when, sys_log_level_name(l[i].level), l[i].tag, l[i].text };
            k = logbook_csv_row(cells, 4, line, sizeof line);
        } else {
            k = snprintf(line, sizeof line, "%s %-7s %s: %s\n", when[0] ? when : "-",
                         l[i].level >= 0 ? sys_log_level_name(l[i].level) : "-", l[i].tag, l[i].text);
        }
        if (k > 0 && (size_t)k <= sizeof line) out_put(o, line, (size_t)k);
    }
    out_flush(o);
    free(o);
    free(l);
    return 200;
}

/* ---- the endpoints ------------------------------------------------------------------------- */

static cJSON *book_json(logbook_t *b)
{
    cJSON *o = cJSON_CreateObject();
    char disk[300];
    logbook_disk(b, disk, sizeof disk);
    cJSON_AddStringToObject(o, "disk", disk);
    cJSON_AddNumberToObject(o, "since_start", (double)logbook_count(b));
    return o;
}

int api_logs_request(struct mg_connection *conn, const char *uri, const char *method)
{
    int get = !strcmp(method, "GET") || !strcmp(method, "HEAD");
    const char *p = uri + 12;                   /* after /api/v1/logs */
    if (*p && *p != '/') return send_error(conn, 404, "not found", NULL);
    if (!get) return method_not_allowed(conn, "GET, HEAD");
    if (!*p || !strcmp(p, "/")) {
        cJSON *doc = cJSON_CreateObject();
        cJSON_AddItemToObject(doc, "alarms", book_json(logs_alarms()));
        cJSON_AddItemToObject(doc, "operations", book_json(logs_ops()));
        cJSON *types = cJSON_AddArrayToObject(doc, "alarm_types");
        const char *t[64];
        int n = alarm_types(t, 64);
        for (int i = 0; types && i < n; ++i) cJSON_AddItemToArray(types, cJSON_CreateString(t[i]));
        cJSON *acts = cJSON_AddArrayToObject(doc, "actions");
        for (int i = 0; acts && oplog_actions[i]; ++i) cJSON_AddItemToArray(acts, cJSON_CreateString(oplog_actions[i]));
        return send_json(conn, 200, doc);
    }
    if (!strcmp(p, "/system")) return send_system(conn);
    if (!strcmp(p, "/system/export")) return export_system(conn);
    if (!strcmp(p, "/alarms")) return send_page(conn, 0);
    if (!strcmp(p, "/alarms/export")) return export_log(conn, 0);
    if (!strcmp(p, "/operations")) return send_page(conn, 1);
    if (!strcmp(p, "/operations/export")) return export_log(conn, 1);
    return send_error(conn, 404, "not found", NULL);
}
