/* SPDX-License-Identifier: GPL-3.0-or-later */
/* api_rec.c — the recordings (docs/api.md "Recordings", docs/recording.md):
 *
 *   GET    /api/v1/record/status        the pool: state, sizes, oldest video, rebuild
 *   GET    /api/v1/record/days          days of a month with video
 *   GET    /api/v1/record/spans         recorded spans of a range, with their events
 *   GET    /api/v1/record/events        events of a range
 *   GET    /api/v1/record/init          the MSE init segment at a time
 *   GET    /api/v1/record/fragments     MSE fragments from a time, re-based
 *   GET    /api/v1/record/export.mp4    a range as one fragmented MP4
 *   DELETE /api/v1/record/recordings    free the chunks inside a range (storage)
 *   POST   /api/v1/record/takeover      record into another recorder's pool (storage)
 *
 * Times are Unix seconds (decimals allowed), cameras 1-based, stream
 * main (default) or sub. Reads only: the writer thread is told what to
 * free or take over and does it (rec_store.h).
 */
#define _DEFAULT_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "civetweb.h"
#include "cJSON.h"

#include "api_int.h"
#include "rec_read.h"
#include "rec_store.h"

#define PLAN_MAX   400000               /* fragments in one export: days of a camera */
#define FRAGS_MAX  64                   /* fragments in one MSE request */

static int qvar(const struct mg_connection *conn, const char *name, char *out, size_t cap)
{
    const char *q = mg_get_request_info(conn)->query_string;
    out[0] = 0;
    return q && mg_get_var(q, strlen(q), name, out, cap) >= 0 ? 0 : -1;
}

/* A time: Unix seconds, decimals allowed. */
static int qtime(const struct mg_connection *conn, const char *name, uint64_t *us)
{
    char v[40], *end;
    if (qvar(conn, name, v, sizeof v) < 0 || !v[0]) return -1;
    double d = strtod(v, &end);
    if (*end || !(d >= 0) || d > 4e12) return -1;
    *us = (uint64_t)llround(d * 1e6);
    return 0;
}

static int qint(const struct mg_connection *conn, const char *name, long def, long lo, long hi, long *out)
{
    char v[24], *end;
    *out = def;
    if (qvar(conn, name, v, sizeof v) < 0 || !v[0]) return 0;
    long n = strtol(v, &end, 10);
    if (*end || n < lo || n > hi) return -1;
    *out = n;
    return 0;
}

/* camera (required) and stream: 0 or a 400 answered. */
static int cam_stream(struct mg_connection *conn, int *cam, int *stream, int *status)
{
    long c;
    if (qint(conn, "camera", 0, 1, REC_CAMS, &c) < 0 || !c) {
        *status = send_error(conn, 400, "camera: 1..16 required", NULL);
        return -1;
    }
    char s[8];
    qvar(conn, "stream", s, sizeof s);
    if (!s[0] || !strcmp(s, "main") || !strcmp(s, "0")) *stream = 0;
    else if (!strcmp(s, "sub") || !strcmp(s, "1")) *stream = 1;
    else {
        *status = send_error(conn, 400, "stream: main or sub", NULL);
        return -1;
    }
    *cam = (int)c;
    return 0;
}

static double secs(uint64_t us) { return (double)(us / 1000) / 1000.0; }

static rec_pool_t *pool_or_503(struct mg_connection *conn, int *status)
{
    rec_store_status_t st;
    rec_store_status(&st);
    if (st.state == RP_READY || st.state == RP_READONLY) return rec_store_pool();
    *status = send_error(conn, 503, st.state == RP_OFF ? "not recording: record.path is empty"
                                    : st.why[0] ? st.why : "the recording pool is not mounted", NULL);
    return NULL;
}

static const char *state_name(rec_pstate_t s)
{
    switch (s) {
    case RP_READY: return "ready";
    case RP_WAITING: return "waiting";
    case RP_READONLY: return "read-only";
    case RP_ERROR: return "error";
    default: return "off";
    }
}

static int h_status(struct mg_connection *conn)
{
    rec_store_status_t st;
    rec_store_status(&st);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "state", state_name(st.state));
    if (st.state == RP_WAITING && st.place[0]) cJSON_AddStringToObject(d, "place", st.place);
    if (st.why[0]) cJSON_AddStringToObject(d, "why", st.why);
    cJSON_AddStringToObject(d, "path", st.path);
    if (st.state == RP_READY || st.state == RP_READONLY) {
        char id[20];
        snprintf(id, sizeof id, "%016llx", (unsigned long long)st.pool_id);
        cJSON *p = cJSON_AddObjectToObject(d, "pool");
        cJSON_AddStringToObject(p, "id", id);
        cJSON_AddNumberToObject(p, "chunk_mb", st.chunk_mb);
        cJSON_AddNumberToObject(p, "chunks", st.chunks);
        cJSON_AddNumberToObject(p, "wanted", st.want);
        cJSON_AddNumberToObject(p, "free", st.free);
        cJSON_AddNumberToObject(p, "open", st.open);
        cJSON_AddNumberToObject(p, "closed", st.closed);
        cJSON_AddNumberToObject(p, "bad", st.bad);
        cJSON_AddNumberToObject(p, "size_bytes", (double)st.size_bytes);
        cJSON_AddNumberToObject(p, "used_bytes", (double)st.used_bytes);
        cJSON *r = cJSON_AddObjectToObject(d, "rebuild");
        cJSON_AddBoolToObject(r, "running", st.rebuilding);
        cJSON_AddNumberToObject(r, "done", st.rebuild_done);
        cJSON_AddNumberToObject(r, "total", st.chunks);
        cJSON_AddStringToObject(d, "media", st.media);
        cJSON_AddNumberToObject(d, "flush_s", st.flush_s);
        cJSON_AddNumberToObject(d, "write_kbps", (double)st.rate * 8 / 1000);
        cJSON *q = cJSON_AddObjectToObject(d, "queue");
        cJSON_AddNumberToObject(q, "bytes", st.queue_bytes);
        cJSON_AddNumberToObject(q, "max", st.queue_max);
        cJSON_AddNumberToObject(d, "dropped", (double)st.dropped);
        if (st.oldest_us) cJSON_AddNumberToObject(d, "oldest", secs(st.oldest_us));
        if (st.newest_us) cJSON_AddNumberToObject(d, "newest", secs(st.newest_us));
        if (st.rate) cJSON_AddNumberToObject(d, "days_kept", round((double)st.size_bytes / st.rate / 864.0) / 100);
        cJSON *cams = cJSON_AddArrayToObject(d, "cameras");
        for (int i = 0; i < REC_CAMS; ++i) {
            if (!st.cam[i].chunks) continue;
            cJSON *c = cJSON_CreateObject();
            cJSON_AddNumberToObject(c, "camera", i + 1);
            cJSON_AddNumberToObject(c, "oldest", secs(st.cam[i].oldest_us));
            cJSON_AddNumberToObject(c, "newest", secs(st.cam[i].newest_us));
            cJSON_AddNumberToObject(c, "chunks", st.cam[i].chunks);
            cJSON_AddNumberToObject(c, "bytes", (double)st.cam[i].bytes);
            cJSON_AddItemToArray(cams, c);
        }
    }
    return send_json(conn, 200, d);
}

static int h_days(struct mg_connection *conn)
{
    int cam, stream, rc;
    if (cam_stream(conn, &cam, &stream, &rc) < 0) return rc;
    char m[16];
    int y, mo;
    if (qvar(conn, "month", m, sizeof m) < 0 || sscanf(m, "%4d-%2d", &y, &mo) != 2 || mo < 1 || mo > 12 ||
        y < 1970 || y > 2200)
        return send_error(conn, 400, "month: YYYY-MM required", NULL);
    rec_pool_t *p = pool_or_503(conn, &rc);
    if (!p) return rc;
    uint32_t mask;
    if (rec_q_days(p, cam, stream, y, mo, &mask) < 0) return send_error(conn, 400, "month: not a month", NULL);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "camera", cam);
    cJSON_AddStringToObject(d, "stream", stream ? "sub" : "main");
    cJSON_AddStringToObject(d, "month", m);
    cJSON *a = cJSON_AddArrayToObject(d, "days");
    for (int i = 0; i < 31; ++i)
        if (mask & (1u << i)) cJSON_AddItemToArray(a, cJSON_CreateNumber(i + 1));
    return send_json(conn, 200, d);
}

static cJSON *event_json(const rec_event_t *e)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "time", secs(e->time_us));
    cJSON_AddNumberToObject(o, "duration", e->dur_ms / 1000.0);
    cJSON_AddNumberToObject(o, "camera", e->cam);
    const char *t = rec_event_name(e->type);
    cJSON_AddStringToObject(o, "type", t ? t : "other");
    if (e->detail) cJSON_AddNumberToObject(o, "detail", e->detail);
    cJSON_AddBoolToObject(o, "recorded", e->chunk != REC_NONE);
    return o;
}

static int range(struct mg_connection *conn, uint64_t *from, uint64_t *to, int *rc)
{
    if (qtime(conn, "from", from) < 0 || qtime(conn, "to", to) < 0 || *to <= *from) {
        *rc = send_error(conn, 400, "from, to: Unix times, from before to", NULL);
        return -1;
    }
    return 0;
}

static int h_spans(struct mg_connection *conn)
{
    int cam, stream, rc;
    uint64_t from, to;
    if (cam_stream(conn, &cam, &stream, &rc) < 0 || range(conn, &from, &to, &rc) < 0) return rc;
    rec_pool_t *p = pool_or_503(conn, &rc);
    if (!p) return rc;
    rec_span_t *sp;
    int n = rec_q_spans(p, cam, stream, from, to, &sp);
    rec_event_t *ev = NULL;
    int ne = rec_q_events(p, cam, 0, from, to, 0, 100000, &ev), k = 0;
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "camera", cam);
    cJSON_AddStringToObject(d, "stream", stream ? "sub" : "main");
    cJSON_AddNumberToObject(d, "from", secs(from));
    cJSON_AddNumberToObject(d, "to", secs(to));
    cJSON *a = cJSON_AddArrayToObject(d, "spans");
    for (int i = 0; i < n; ++i) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddNumberToObject(s, "from", secs(sp[i].from_us));
        cJSON_AddNumberToObject(s, "to", secs(sp[i].to_us));
        cJSON *e = cJSON_AddArrayToObject(s, "events");
        while (k < ne && ev[k].time_us < sp[i].from_us) ++k;
        for (int j = k; j < ne && ev[j].time_us < sp[i].to_us; ++j) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "time", secs(ev[j].time_us));
            const char *t = rec_event_name(ev[j].type);
            cJSON_AddStringToObject(o, "type", t ? t : "other");
            cJSON_AddNumberToObject(o, "duration", ev[j].dur_ms / 1000.0);
            cJSON_AddItemToArray(e, o);
        }
        cJSON_AddItemToArray(a, s);
    }
    free(sp);
    free(ev);
    return send_json(conn, 200, d);
}

static int h_events(struct mg_connection *conn)
{
    long cam, limit;
    int rc, type = 0;
    uint64_t from, to;
    if (qint(conn, "camera", 0, 0, REC_CAMS, &cam) < 0) return send_error(conn, 400, "camera: 1..16", NULL);
    if (qint(conn, "limit", 1000, 1, 100000, &limit) < 0) return send_error(conn, 400, "limit: 1..100000", NULL);
    char t[24], o[8];
    qvar(conn, "type", t, sizeof t);
    if (t[0] && !(type = rec_event_type(t))) return send_error(conn, 400, "type: not an event type", NULL);
    qvar(conn, "order", o, sizeof o);
    if (o[0] && strcmp(o, "asc") && strcmp(o, "desc")) return send_error(conn, 400, "order: asc or desc", NULL);
    if (range(conn, &from, &to, &rc) < 0) return rc;
    rec_pool_t *p = pool_or_503(conn, &rc);
    if (!p) return rc;
    rec_event_t *ev;
    int n = rec_q_events(p, (int)cam, type, from, to, !strcmp(o, "desc"), (int)limit, &ev);
    cJSON *d = cJSON_CreateObject();
    cJSON *a = cJSON_AddArrayToObject(d, "events");
    for (int i = 0; i < n; ++i) cJSON_AddItemToArray(a, event_json(&ev[i]));
    free(ev);
    return send_json(conn, 200, d);
}

/* ---- video ------------------------------------------------------------------------- */

typedef struct {
    struct mg_connection *conn;
} sink_t;

static int sink(void *user, const void *data, size_t len)
{
    sink_t *s = user;
    return mg_write(s->conn, data, len) == (int)len ? 0 : -1;
}

static void hdr_time(struct mg_connection *conn, const char *name, uint64_t us)
{
    char v[32];
    snprintf(v, sizeof v, "%.3f", secs(us));
    mg_response_header_add(conn, name, v, -1);
}

static int video_head(struct mg_connection *conn, uint64_t len, const char *codecs)
{
    char cl[24];
    snprintf(cl, sizeof cl, "%llu", (unsigned long long)len);
    mg_response_header_start(conn, 200);
    mg_response_header_add(conn, "Content-Type", "video/mp4", -1);
    mg_response_header_add(conn, "Cache-Control", "no-store", -1);
    mg_response_header_add(conn, "Content-Length", cl, -1);
    if (codecs && codecs[0]) mg_response_header_add(conn, "X-Vigil-Codecs", codecs, -1);
    return 0;
}

static int is_head(const struct mg_connection *conn)
{
    return !strcmp(mg_get_request_info(conn)->request_method, "HEAD");
}

static int h_init(struct mg_connection *conn)
{
    int cam, stream, rc;
    uint64_t at;
    if (cam_stream(conn, &cam, &stream, &rc) < 0) return rc;
    if (qtime(conn, "at", &at) < 0) return send_error(conn, 400, "at: a Unix time required", NULL);
    rec_pool_t *p = pool_or_503(conn, &rc);
    if (!p) return rc;
    rec_plan_t pl;
    rec_q_plan(p, cam, stream, at, UINT64_MAX, 1, 0, &pl);
    if (!pl.n || !pl.init) {
        rec_plan_free(&pl);
        return send_error(conn, 404, "no video at or after that time", NULL);
    }
    video_head(conn, pl.init_len, pl.codecs);
    hdr_time(conn, "X-Vigil-Start", pl.p[0].chunk_start_us + (uint64_t)pl.p[0].t_ms * 1000);
    mg_response_header_send(conn);
    if (!is_head(conn)) mg_write(conn, pl.init, pl.init_len);
    rec_plan_free(&pl);
    return 200;
}

/* Local midnight of the day holding `us`. */
static uint64_t midnight(uint64_t us)
{
    time_t t = (time_t)(us / 1000000);
    struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    time_t m = mktime(&tm);
    return m == (time_t)-1 ? 0 : (uint64_t)m * 1000000;
}

static int h_fragments(struct mg_connection *conn)
{
    int cam, stream, rc;
    uint64_t from, base;
    long count, cont;
    if (cam_stream(conn, &cam, &stream, &rc) < 0) return rc;
    if (qtime(conn, "from", &from) < 0) return send_error(conn, 400, "from: a Unix time required", NULL);
    if (qint(conn, "count", 4, 1, FRAGS_MAX, &count) < 0) return send_error(conn, 400, "count: 1..64", NULL);
    if (qint(conn, "cont", 0, 0, 1, &cont) < 0) return send_error(conn, 400, "cont: 0 or 1", NULL);
    if (qtime(conn, "base", &base) < 0) base = midnight(from);
    rec_pool_t *p = pool_or_503(conn, &rc);
    if (!p) return rc;
    rec_plan_t pl;
    rec_q_plan(p, cam, stream, from, UINT64_MAX, (int)count, (int)cont, &pl);
    if (!pl.n) {
        rec_plan_free(&pl);
        return send_error(conn, 404, "no video at or after that time", NULL);
    }
    video_head(conn, pl.bytes, pl.codecs);
    hdr_time(conn, "X-Vigil-Base", base);
    hdr_time(conn, "X-Vigil-Start", pl.p[0].chunk_start_us + (uint64_t)pl.p[0].t_ms * 1000);
    if (pl.next_us) hdr_time(conn, "X-Vigil-Next", pl.next_us);
    if (pl.format_change) mg_response_header_add(conn, "X-Vigil-Format-Change", "1", -1);
    mg_response_header_send(conn);
    if (!is_head(conn)) {
        sink_t s = { conn };
        rec_q_send(p, &pl, (int64_t)(base * 9 / 100), REC_GAPS_KEEP, 1, sink, &s);
    }
    rec_plan_free(&pl);
    return 200;
}

static int h_export(struct mg_connection *conn)
{
    int cam, stream, rc;
    uint64_t from, to;
    if (cam_stream(conn, &cam, &stream, &rc) < 0 || range(conn, &from, &to, &rc) < 0) return rc;
    char gaps[12], layout[12];
    qvar(conn, "gaps", gaps, sizeof gaps);
    qvar(conn, "layout", layout, sizeof layout);
    if (gaps[0] && strcmp(gaps, "keep") && strcmp(gaps, "remove") && strcmp(gaps, "split"))
        return send_error(conn, 400, "gaps: keep, remove or split", NULL);
    if (layout[0] && strcmp(layout, "frag") && strcmp(layout, "faststart"))
        return send_error(conn, 400, "layout: frag or faststart", NULL);
    if (!strcmp(layout, "faststart")) return send_error(conn, 501, "layout faststart: not yet (frag only)", NULL);
    rec_pool_t *p = pool_or_503(conn, &rc);
    if (!p) return rc;
    if (!strcmp(gaps, "split")) {
        /* One file per recorded span: their URLs. */
        rec_span_t *sp;
        int n = rec_q_spans(p, cam, stream, from, to, &sp);
        cJSON *d = cJSON_CreateObject();
        cJSON *a = cJSON_AddArrayToObject(d, "files");
        for (int i = 0; i < n; ++i) {
            char url[200];
            snprintf(url, sizeof url, "/api/v1/record/export.mp4?camera=%d&stream=%s&from=%.3f&to=%.3f&gaps=keep",
                     cam, stream ? "sub" : "main", secs(sp[i].from_us), secs(sp[i].to_us));
            cJSON *f = cJSON_CreateObject();
            cJSON_AddNumberToObject(f, "from", secs(sp[i].from_us));
            cJSON_AddNumberToObject(f, "to", secs(sp[i].to_us));
            cJSON_AddStringToObject(f, "url", url);
            cJSON_AddItemToArray(a, f);
        }
        free(sp);
        return send_json(conn, 200, d);
    }
    int mode = !strcmp(gaps, "remove") ? REC_GAPS_REMOVE : REC_GAPS_KEEP;
    rec_plan_t pl;
    rec_q_plan(p, cam, stream, from, to, PLAN_MAX, 0, &pl);
    mp4_buf_t init = { 0 };
    if (!pl.n || rec_q_export_init(&pl, from, to, mode, &init) < 0) {
        rec_plan_free(&pl);
        mp4_buf_free(&init);
        return send_error(conn, 404, "no video in that range", NULL);
    }
    video_head(conn, init.len + pl.bytes, pl.codecs);
    uint64_t first = (uint64_t)pl.first_tick * 100 / 9;
    hdr_time(conn, "X-Vigil-Start", first);
    if (pl.format_change) hdr_time(conn, "X-Vigil-Format-Change", pl.next_us);
    else if (pl.next_us) hdr_time(conn, "X-Vigil-Truncated", pl.next_us);
    char disp[96];
    time_t t = (time_t)(from / 1000000);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(disp, sizeof disp, "attachment; filename=\"CAM%d_%04d%02d%02d_%02d%02d%02d.mp4\"", cam,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    mg_response_header_add(conn, "Content-Disposition", disp, -1);
    mg_response_header_send(conn);
    if (!is_head(conn)) {
        sink_t s = { conn };
        if (sink(&s, init.p, init.len) == 0) rec_q_send(p, &pl, pl.first_tick, mode, 1, sink, &s);
    }
    mp4_buf_free(&init);
    rec_plan_free(&pl);
    return 200;
}

static int h_delete(struct mg_connection *conn)
{
    long cam;
    int rc;
    uint64_t from, to;
    if (qint(conn, "camera", 0, 0, REC_CAMS, &cam) < 0) return send_error(conn, 400, "camera: 1..16", NULL);
    if (range(conn, &from, &to, &rc) < 0) return rc;
    if (!pool_or_503(conn, &rc)) return rc;
    int n = rec_store_delete((int)cam, from, to);
    if (n < 0) return send_error(conn, 409, "the pool is not writable here", NULL);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "freed", n);
    return send_json(conn, 200, d);
}

int api_rec_request(struct mg_connection *conn, const char *uri, const char *method)
{
    if (strncmp(uri, "/api/v1/record/", 15)) return 0;
    const char *r = uri + 15;
    int get = !strcmp(method, "GET") || !strcmp(method, "HEAD");
    if (!strcmp(r, "recordings")) {
        if (strcmp(method, "DELETE")) return method_not_allowed(conn, "DELETE");
        return h_delete(conn);
    }
    if (!strcmp(r, "takeover")) {
        if (strcmp(method, "POST")) return method_not_allowed(conn, "POST");
        rec_store_status_t st;
        rec_store_status(&st);
        if (st.state != RP_READONLY) return send_error(conn, 409, "the pool is not another recorder's", NULL);
        if (rec_store_takeover() < 0) return send_error(conn, 500, "cannot take the pool over", NULL);
        return h_status(conn);
    }
    static const struct { const char *name; int (*fn)(struct mg_connection *); } GETS[] = {
        { "status", h_status }, { "days", h_days }, { "spans", h_spans }, { "events", h_events },
        { "init", h_init }, { "fragments", h_fragments }, { "export.mp4", h_export },
    };
    for (size_t i = 0; i < sizeof GETS / sizeof GETS[0]; ++i)
        if (!strcmp(r, GETS[i].name)) return get ? GETS[i].fn(conn) : method_not_allowed(conn, "GET, HEAD");
    return send_error(conn, 404, "not found", NULL);
}
