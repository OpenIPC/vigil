/* SPDX-License-Identifier: GPL-3.0-or-later */
/* api_backup.c — the configuration's export and import, and the resets,
 * over HTTP (docs/api.md, "Configuration export and import"); the work is
 * backup.c's, as for the on-screen Operation page. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/base64.h>

#include "civetweb.h"
#include "cJSON.h"

#include "api_int.h"
#include "backup.h"
#include "secret.h"

/* base64 of BACKUP_FILE_MAX, the JSON around it */
#define IMPORT_BODY_MAX (BACKUP_FILE_MAX / 3 * 4 + 8192)

static int is(const char *m, const char *want) { return !strcmp(m, want); }

static const char *jstr(const cJSON *o, const char *k, const char *dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : dflt;
}

static int jbool(const cJSON *o, const char *k, int dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : cJSON_IsNumber(v) ? v->valueint != 0 : dflt;
}

static int status_of(int rc)
{
    switch (rc) {
    case BACKUP_EINVAL:     return 400;
    case BACKUP_EPASSWORD:  return 403;
    case BACKUP_ECHANGED:   return 422;
    case BACKUP_EVERSION:   return 409;
    case BACKUP_EBUSY:      return 409;
    case BACKUP_EUNMANAGED: return 409;
    default:                return 500;
    }
}

static const char *code_of(int rc)
{
    switch (rc) {
    case BACKUP_EINVAL:     return "invalid";
    case BACKUP_EPASSWORD:  return "password";
    case BACKUP_ECHANGED:   return "changed";
    case BACKUP_EVERSION:   return "version";
    case BACKUP_EBUSY:      return "busy";
    case BACKUP_EUNMANAGED: return "unmanaged";
    default:                return "failed";
    }
}

static int fail(struct mg_connection *conn, int rc, const char *err, cJSON *summary)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "error", err[0] ? err : "failed");
    cJSON_AddStringToObject(d, "code", code_of(rc));
    if (summary) cJSON_AddItemToObject(d, "summary", summary);
    return send_json(conn, status_of(rc), d);
}

/* The body as a JSON object (a password may be in it: wiped). NULL after
 * answering. */
static cJSON *body_json(struct mg_connection *conn, size_t max)
{
    size_t len = 0;
    int status = 0;
    char *b = read_body_max(conn, max, &len, &status);
    if (!b) {
        send_error(conn, status, status == 413 ? "body too large" : "out of memory", NULL);
        return NULL;
    }
    cJSON *doc = len ? cJSON_ParseWithLength(b, len) : cJSON_CreateObject();
    secret_wipe(b, len);
    free(b);
    if (!cJSON_IsObject(doc)) {
        cJSON_Delete(doc);
        send_error(conn, 400, "the body must be a JSON object", NULL);
        return NULL;
    }
    return doc;
}

static void wipe_json(cJSON *d)
{
    for (cJSON *c = d ? d->child : NULL; c; c = c->next)
        if (cJSON_IsString(c) && c->valuestring) secret_wipe(c->valuestring, strlen(c->valuestring));
    cJSON_Delete(d);
}

static int export_req(struct mg_connection *conn, const char *m)
{
    if (!is(m, "POST")) return method_not_allowed(conn, "POST");
    cJSON *b = body_json(conn, 8192);
    if (!b) return 400;
    const char *mode = jstr(b, "secrets", "password"), *pw = jstr(b, "password", "");
    const char *dest = jstr(b, "destination", "");
    char err[300] = "";
    if (strcmp(mode, "password") && strcmp(mode, "none")) {
        wipe_json(b);
        return send_error(conn, 400, "secrets is \"password\" or \"none\"", NULL);
    }
    int with = !strcmp(mode, "password");
    if (with && !*pw) {
        wipe_json(b);
        return send_error(conn, 400, "an export with the passwords needs a password to protect them "
                                     "(\"password\"), or \"secrets\": \"none\"", NULL);
    }
    if (*dest) {
        char path[400];
        int rc = backup_export_to(dest, with ? pw : NULL, path, sizeof path, err, sizeof err);
        wipe_json(b);
        if (rc != BACKUP_OK) return fail(conn, rc, err, NULL);
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "path", path);
        return send_json(conn, 200, d);
    }
    char *data = NULL, name[160];
    size_t len = 0;
    int rc = backup_export(with ? pw : NULL, &data, &len, name, sizeof name, err, sizeof err);
    wipe_json(b);
    if (rc != BACKUP_OK) return fail(conn, rc, err, NULL);
    char cl[24], cd[220];
    snprintf(cl, sizeof cl, "%zu", len);
    snprintf(cd, sizeof cd, "attachment; filename=\"%s\"", name);
    mg_response_header_start(conn, 200);
    mg_response_header_add(conn, "Content-Type", "application/x-tar", -1);
    mg_response_header_add(conn, "Content-Disposition", cd, -1);
    mg_response_header_add(conn, "Cache-Control", "no-store", -1);
    mg_response_header_add(conn, "Content-Length", cl, -1);
    mg_response_header_send(conn);
    mg_write(conn, data, len);
    secret_wipe(data, len);
    free(data);
    return 200;
}

static cJSON *summary_json(const backup_summary_t *s)
{
    cJSON *d = cJSON_CreateObject();
    cJSON *f = cJSON_AddObjectToObject(d, "from");
    cJSON_AddStringToObject(f, "hostname", s->from_host);
    cJSON_AddStringToObject(f, "mac", s->from_mac);
    cJSON_AddStringToObject(f, "vigil_version", s->from_version);
    cJSON_AddStringToObject(f, "board", s->from_board);
    cJSON_AddStringToObject(f, "created", s->created);
    cJSON_AddStringToObject(d, "secrets", s->secrets ? "password" : "none");
    cJSON *c = cJSON_AddObjectToObject(d, "cameras");
    cJSON_AddNumberToObject(c, "now", s->cameras_now);
    cJSON_AddNumberToObject(c, "import", s->cameras_new);
    cJSON *u = cJSON_AddObjectToObject(d, "users");
    cJSON_AddNumberToObject(u, "now", s->users_now);
    cJSON_AddNumberToObject(u, "import", s->users_new);
    cJSON_AddNumberToObject(u, "without_password", s->users_no_password);
    cJSON_AddNumberToObject(u, "groups", s->groups_new);
    cJSON_AddBoolToObject(d, "root_password", s->root_password);
    cJSON *sy = cJSON_AddObjectToObject(d, "system");
    cJSON_AddBoolToObject(sy, "applies", s->system);
    cJSON_AddBoolToObject(sy, "hostname", s->hostname_change);
    cJSON_AddStringToObject(sy, "hostname_now", s->host_now);
    cJSON_AddStringToObject(sy, "hostname_import", s->host_new);
    cJSON_AddBoolToObject(sy, "timezone", s->tz_change);
    cJSON_AddBoolToObject(sy, "ntp", s->ntp_change);
    cJSON_AddBoolToObject(sy, "ethernet", s->net_change);
    cJSON_AddBoolToObject(sy, "wifi", s->wifi_change);
    cJSON_AddBoolToObject(d, "address_may_change", s->net_cuts);
    cJSON_AddBoolToObject(d, "reboot_required", s->reboot);
    cJSON_AddBoolToObject(d, "applied", s->applied);
    cJSON *ls = cJSON_AddArrayToObject(d, "lines");
    for (int i = 0; i < s->n_lines; ++i) {
        cJSON *l = cJSON_CreateObject();
        cJSON_AddStringToObject(l, "kind", s->kind[i] ? "warning" : "info");
        cJSON_AddStringToObject(l, "text", s->line[i]);
        cJSON_AddItemToArray(ls, l);
    }
    return d;
}

/* One query variable as a flag: "1", "true" or "yes" (or `dflt` when not there). */
static int qflag(const struct mg_request_info *ri, const char *name, int dflt)
{
    char v[8];
    if (!ri->query_string || mg_get_var(ri->query_string, strlen(ri->query_string), name, v, sizeof v) < 0)
        return dflt;
    return !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes");
}

static int import_req(struct mg_connection *conn, const char *m)
{
    if (!is(m, "POST")) return method_not_allowed(conn, "POST");
    const struct mg_request_info *ri = mg_get_request_info(conn);
    const char *ct = mg_get_header(conn, "Content-Type");
    backup_import_opts_t o = { .hostname = 1 };
    backup_summary_t *s = calloc(1, sizeof *s);
    char err[300] = "", pw[160] = "", path[256] = "";
    unsigned char *data = NULL;
    size_t len = 0;
    if (!s) return send_error(conn, 500, "out of memory", NULL);
    if (ct && !strncmp(ct, "application/x-tar", 17)) {
        /* The file itself: options in the query, the password in a header. */
        int status = 0;
        data = (unsigned char *)read_body_max(conn, BACKUP_FILE_MAX, &len, &status);
        if (!data) {
            free(s);
            return send_error(conn, status, status == 413 ? "larger than a configuration export" : "out of memory",
                              NULL);
        }
        const char *h = mg_get_header(conn, "X-Config-Password");
        snprintf(pw, sizeof pw, "%s", h ? h : "");
        o.apply = !qflag(ri, "dry_run", 0);
        o.hostname = qflag(ri, "hostname", 1);
    } else {
        cJSON *b = body_json(conn, IMPORT_BODY_MAX);
        if (!b) {
            free(s);
            return 400;
        }
        snprintf(pw, sizeof pw, "%s", jstr(b, "password", ""));
        snprintf(path, sizeof path, "%s", jstr(b, "path", ""));
        o.apply = !jbool(b, "dry_run", 0);
        o.hostname = jbool(b, "hostname", 1);
        const char *f = jstr(b, "file", "");
        if (*f) {
            size_t fl = strlen(f);
            data = malloc(fl / 4 * 3 + 4);
            if (!data || mbedtls_base64_decode(data, fl / 4 * 3 + 4, &len, (const unsigned char *)f, fl)) {
                wipe_json(b);
                free(data);
                free(s);
                return send_error(conn, 400, "\"file\" must be the export, base64-encoded", NULL);
            }
        } else if (!path[0]) {
            wipe_json(b);
            free(s);
            return send_error(conn, 400, "send the export: \"file\" (base64) or \"path\" (on a mounted disk), or "
                                         "the file itself as application/x-tar", NULL);
        }
        wipe_json(b);
    }
    o.password = pw;
    int rc = data ? backup_import(data, len, &o, s, err, sizeof err)
                  : backup_import_file(path, &o, s, err, sizeof err);
    secret_wipe(pw, sizeof pw);
    if (data) secret_wipe(data, len);
    free(data);
    int st = rc == BACKUP_OK ? send_json(conn, 200, summary_json(s)) : fail(conn, rc, err, summary_json(s));
    free(s);
    return st;
}

static int files_req(struct mg_connection *conn, const char *m)
{
    if (!is(m, "GET") && !is(m, "HEAD")) return method_not_allowed(conn, "GET, HEAD");
    backup_place_t pl[16];
    backup_file_t *fl = calloc(64, sizeof *fl);
    if (!fl) return send_error(conn, 500, "out of memory", NULL);
    int np = backup_places(pl, 16), nf = backup_files(fl, 64);
    cJSON *d = cJSON_CreateObject();
    cJSON *a = cJSON_AddArrayToObject(d, "destinations");
    for (int i = 0; i < np; ++i) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "path", pl[i].path);
        cJSON_AddStringToObject(o, "label", pl[i].label);
        cJSON_AddItemToArray(a, o);
    }
    cJSON *b = cJSON_AddArrayToObject(d, "files");
    for (int i = 0; i < nf; ++i) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "path", fl[i].path);
        cJSON_AddStringToObject(o, "name", fl[i].name);
        cJSON_AddNumberToObject(o, "size", (double)fl[i].size);
        cJSON_AddNumberToObject(o, "mtime", (double)fl[i].mtime);
        cJSON_AddItemToArray(b, o);
    }
    free(fl);
    return send_json(conn, 200, d);
}

static int reset_req(struct mg_connection *conn, const char *m)
{
    if (!is(m, "POST")) return method_not_allowed(conn, "POST");
    cJSON *b = body_json(conn, 4096);
    if (!b) return 400;
    char level[16];
    snprintf(level, sizeof level, "%s", jstr(b, "level", ""));
    int network = jbool(b, "network", 0);
    char err[300] = "";
    int rc, reboot = 0;
    if (!strcmp(level, "vigil")) {
        rc = backup_reset_vigil(network, &reboot, err, sizeof err);
    } else if (!strcmp(level, "factory")) {
        rc = backup_reset_factory(err, sizeof err);
        reboot = rc == BACKUP_OK;
    } else {
        cJSON_Delete(b);
        return send_error(conn, 400, "level is \"vigil\" (vigil's settings) or \"factory\" (the whole firmware)",
                          NULL);
    }
    cJSON_Delete(b);
    if (rc != BACKUP_OK) return fail(conn, rc, err, NULL);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "level", level);
    cJSON_AddBoolToObject(d, "rebooting", reboot);
    return send_json(conn, 200, d);
}

int api_backup_request(struct mg_connection *conn, const char *uri, const char *m)
{
    if (!strcmp(uri, "/api/v1/system/config/export")) return export_req(conn, m);
    if (!strcmp(uri, "/api/v1/system/config/import")) return import_req(conn, m);
    if (!strcmp(uri, "/api/v1/system/config/files")) return files_req(conn, m);
    if (!strcmp(uri, "/api/v1/system/reset")) return reset_req(conn, m);
    return 0;
}
