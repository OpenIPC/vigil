/* SPDX-License-Identifier: GPL-3.0-or-later */
/* api_snap.c — snapshots over the API (docs/api.md, Snapshots):
 *
 *   GET  /api/v1/channels/N/snapshot.jpg[?source=auto|camera|nvr]   a picture now (live)
 *   POST /api/v1/channels/N/snapshot[?source=...]                   one kept (live)
 *   GET  /api/v1/snapshots[?channel=N&limit=M]                      the kept ones (playback)
 *   GET  /api/v1/snapshots/<file>                                   one of them (playback)
 *   DELETE /api/v1/snapshots/<file>                                 (storage)
 *
 * Taking one is the app's (api_set_snapshots); the kept ones are files in
 * the snapshot directory (snap_store.h), read here. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "civetweb.h"
#include "cJSON.h"

#include "api.h"
#include "api_int.h"
#include "channel.h"
#include "config.h"
#include "snap_store.h"

#define LIST_MAX 2000

static const api_snapshot_ops_t *g_ops;

void api_set_snapshots(const api_snapshot_ops_t *ops) { g_ops = ops; }

static void query(struct mg_connection *conn, const char *key, char *out, size_t cap)
{
    const char *q = mg_get_request_info(conn)->query_string;
    out[0] = 0;
    if (q && mg_get_var(q, strlen(q), key, out, cap) < 0) out[0] = 0;
}

static int send_jpeg(struct mg_connection *conn, const unsigned char *p, size_t len, const char *source,
                     const char *filename)
{
    char cl[24], disp[96];
    snprintf(cl, sizeof cl, "%zu", len);
    mg_response_header_start(conn, 200);
    mg_response_header_add(conn, "Content-Type", "image/jpeg", -1);
    mg_response_header_add(conn, "Content-Length", cl, -1);
    mg_response_header_add(conn, "Cache-Control", "no-store", -1);
    if (source) mg_response_header_add(conn, "X-Vigil-Source", source, -1);
    if (filename) {
        snprintf(disp, sizeof disp, "inline; filename=\"%s\"", filename);
        mg_response_header_add(conn, "Content-Disposition", disp, -1);
    }
    mg_response_header_send(conn);
    if (strcmp(mg_get_request_info(conn)->request_method, "HEAD") != 0) mg_write(conn, p, len);
    return 200;
}

static int source_of(struct mg_connection *conn, char *src, size_t cap)
{
    query(conn, "source", src, cap);
    if (!src[0]) snprintf(src, cap, "auto");
    return !strcmp(src, "auto") || !strcmp(src, "camera") || !strcmp(src, "nvr") ? 0 : -1;
}

static int take(struct mg_connection *conn, int ch, int keep)
{
    char src[16], used[32] = "", err[300], name[SNAP_NAME_MAX];
    if (source_of(conn, src, sizeof src) < 0) return send_error(conn, 400, "source: auto, camera or nvr", NULL);
    if (!g_ops) return send_error(conn, 503, "snapshots are not available in this vigild", NULL);
    int status = 500;
    if (keep) {
        if (g_ops->save(ch - 1, src, name, sizeof name, used, sizeof used, &status, err, sizeof err) < 0)
            return send_error(conn, status, err, NULL);
        cJSON *doc = cJSON_CreateObject();
        cJSON_AddStringToObject(doc, "file", name);
        cJSON_AddNumberToObject(doc, "channel", ch);
        cJSON_AddStringToObject(doc, "source", used);
        return send_json(conn, 201, doc);
    }
    unsigned char *jpeg;
    size_t len;
    if (g_ops->take(ch - 1, src, &jpeg, &len, used, sizeof used, &status, err, sizeof err) < 0)
        return send_error(conn, status, err, NULL);
    int rc = send_jpeg(conn, jpeg, len, used, NULL);
    free(jpeg);
    return rc;
}

/* The snapshot directory as the settings say: 0, or -1 with the answer sent. */
static int snap_dir_of(struct mg_connection *conn, char *dir, size_t cap, uint64_t *max_bytes, int *rc)
{
    vigil_config_t *c = malloc(sizeof *c);
    if (!c) {
        *rc = send_error(conn, 500, "out of memory", NULL);
        return -1;
    }
    config_get(c);
    char why[200];
    int r = snap_dir(c->snapshot.path, c->record.path, c->record.allow_rootfs, dir, cap, why, sizeof why);
    if (max_bytes) *max_bytes = (uint64_t)c->snapshot.max_mb << 20;
    free(c);
    if (r < 0) {
        *rc = send_error(conn, 507, why, NULL);
        return -1;
    }
    return 0;
}

static int list(struct mg_connection *conn)
{
    char dir[256], v[16];
    uint64_t max_bytes, total = 0;
    int rc;
    if (snap_dir_of(conn, dir, sizeof dir, &max_bytes, &rc) < 0) return rc;
    query(conn, "channel", v, sizeof v);
    int only = v[0] ? atoi(v) : 0;
    query(conn, "limit", v, sizeof v);
    int limit = v[0] ? atoi(v) : 500;
    if (limit <= 0 || limit > LIST_MAX) limit = LIST_MAX;
    snap_file_t *f = malloc(sizeof *f * LIST_MAX);
    if (!f) return send_error(conn, 500, "out of memory", NULL);
    int n = snap_list(dir, f, LIST_MAX, &total);
    if (n < 0) {
        free(f);
        return send_error(conn, 500, "cannot read the snapshot directory", NULL);
    }
    cJSON *doc = cJSON_CreateObject(), *arr = cJSON_CreateArray();
    cJSON_AddStringToObject(doc, "dir", dir);
    cJSON_AddNumberToObject(doc, "count", n);
    cJSON_AddNumberToObject(doc, "bytes", (double)total);
    cJSON_AddNumberToObject(doc, "max_bytes", (double)max_bytes);
    int shown = 0;
    for (int i = 0; i < n && i < LIST_MAX && shown < limit; ++i) {
        if (only && f[i].ch != only) continue;
        cJSON *e = cJSON_CreateObject();
        char t[32];
        struct tm tm;
        localtime_r(&f[i].time, &tm);
        strftime(t, sizeof t, "%Y-%m-%dT%H:%M:%S", &tm);
        cJSON_AddStringToObject(e, "file", f[i].name);
        cJSON_AddNumberToObject(e, "channel", f[i].ch);
        cJSON_AddStringToObject(e, "time", t);
        cJSON_AddNumberToObject(e, "unix", (double)f[i].time + f[i].ms / 1000.0);
        cJSON_AddStringToObject(e, "kind", f[i].kind);
        cJSON_AddNumberToObject(e, "size", (double)f[i].size);
        cJSON_AddItemToArray(arr, e);
        ++shown;
    }
    cJSON_AddItemToObject(doc, "snapshots", arr);
    free(f);
    return send_json(conn, 200, doc);
}

static int one(struct mg_connection *conn, const char *name, int del)
{
    snap_file_t f;
    if (snap_name_parse(name, &f) < 0) return send_error(conn, 404, "no such snapshot", NULL);
    char dir[256], path[512];
    int rc;
    if (snap_dir_of(conn, dir, sizeof dir, NULL, &rc) < 0) return rc;
    if (del) {
        if (snap_delete(dir, name) < 0)
            return send_error(conn, errno == ENOENT ? 404 : 500, errno == ENOENT ? "no such snapshot" : strerror(errno),
                              NULL);
        return send_text(conn, 200, "{\"deleted\":true}\n", NULL, NULL);
    }
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *fp = fopen(path, "rb");
    if (!fp) return send_error(conn, 404, "no such snapshot", NULL);
    unsigned char *b = NULL;
    size_t len = 0;
    if (fseek(fp, 0, SEEK_END) == 0) {
        long sz = ftell(fp);
        if (sz > 0 && sz <= (16L << 20) && fseek(fp, 0, SEEK_SET) == 0 && (b = malloc((size_t)sz)) != NULL)
            len = fread(b, 1, (size_t)sz, fp);
    }
    fclose(fp);
    if (!b || !len) {
        free(b);
        return send_error(conn, 500, "cannot read it", NULL);
    }
    rc = send_jpeg(conn, b, len, NULL, name);
    free(b);
    return rc;
}

int api_snap_request(struct mg_connection *conn, const char *uri, const char *method)
{
    int get = !strcmp(method, "GET") || !strcmp(method, "HEAD");
    int ch, used = 0;
    if (sscanf(uri, "/api/v1/channels/%d/snapshot%n", &ch, &used) == 1 && used) {
        int jpg = !strcmp(uri + used, ".jpg");
        if (uri[used] && !jpg) return 0;
        if (ch < 1 || ch > CHANNEL_MAX) return send_error(conn, 404, "no such channel", NULL);
        if (jpg) return get ? take(conn, ch, 0) : method_not_allowed(conn, "GET, HEAD");
        return !strcmp(method, "POST") ? take(conn, ch, 1) : method_not_allowed(conn, "POST");
    }
    if (!strcmp(uri, "/api/v1/snapshots")) return get ? list(conn) : method_not_allowed(conn, "GET, HEAD");
    if (!strncmp(uri, "/api/v1/snapshots/", 18)) {
        if (get) return one(conn, uri + 18, 0);
        if (!strcmp(method, "DELETE")) return one(conn, uri + 18, 1);
        return method_not_allowed(conn, "GET, HEAD, DELETE");
    }
    return 0;
}
