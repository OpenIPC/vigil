/* SPDX-License-Identifier: GPL-3.0-or-later */
/* api.c — the HTTP/WebSocket API on CivetWeb (see api.h, docs/api.md). */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "civetweb.h"
#include "cJSON.h"

#include "api.h"
#include "api_int.h"
#include "netip_srv.h"
#include "auth.h"
#include "channel.h"
#include "config.h"
#include "events.h"
#include "hal.h"
#include "log.h"
#include "md5.h"
#include "net.h"
#include "sys_time.h"
#include "users.h"

#define BODY_MAX        (64u << 10)  /* a full configuration is ~10 KiB */
#define WS_MAX          4            /* each WebSocket holds a CivetWeb worker and a writer */
#define THREADS         "8"          /* WS_MAX + room for plain requests */
#define PUSH_MS         200
#define STATUS_SECS     1
#define FAIL_FREE       5            /* failed logins in a row (per address) that start a lockout */
#define FAIL_ADDRS      32           /* addresses whose failures are remembered */
#define FAIL_DELAY_S    1            /* every failed login answers this late */
#define LOCKOUT_MAX_S   60
#define CACHE_SECS      300          /* a good password skips crypt() this long */

static struct mg_context *g_ctx;
static time_t             g_t0;      /* monotonic, for "uptime" */

static time_t mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

/* ---- responses ---------------------------------------------------------------- */

int send_text(struct mg_connection *conn, int status, const char *text, const char *extra_hdr,
                     const char *extra_val)
{
    size_t len = strlen(text);
    char cl[24];
    snprintf(cl, sizeof cl, "%zu", len);
    if (mg_response_header_start(conn, status) == -2) {
        /* A refused WebSocket upgrade: CivetWeb's header API is closed to
         * those, so the answer is written as it goes on the wire. */
        mg_printf(conn, "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n"
                        "Content-Length: %s\r\nConnection: close\r\n", status,
                  mg_get_response_code_text(conn, status), cl);
        if (extra_hdr) mg_printf(conn, "%s: %s\r\n", extra_hdr, extra_val);
        mg_printf(conn, "\r\n");
        mg_write(conn, text, len);
        return status;
    }
    mg_response_header_add(conn, "Content-Type", "application/json", -1);
    mg_response_header_add(conn, "Cache-Control", "no-store", -1);
    mg_response_header_add(conn, "Content-Length", cl, -1);
    if (extra_hdr) mg_response_header_add(conn, extra_hdr, extra_val, -1);
    mg_response_header_send(conn);
    if (strcmp(mg_get_request_info(conn)->request_method, "HEAD") != 0) mg_write(conn, text, len);
    return status;
}

int send_json(struct mg_connection *conn, int status, cJSON *doc)
{
    char *text = doc ? cJSON_Print(doc) : NULL;
    cJSON_Delete(doc);
    if (!text) return send_text(conn, 500, "{\"error\":\"out of memory\"}\n", NULL, NULL);
    int rc = send_text(conn, status, text, NULL, NULL);
    free(text);
    return rc;
}

int send_error(struct mg_connection *conn, int status, const char *msg,
                      const schema_errors_t *errs)
{
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "error", msg);
    if (errs) {
        cJSON *list = cJSON_AddArrayToObject(doc, "errors");
        for (int i = 0; list && i < errs->n; ++i) {
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "path", errs->e[i].path);
            cJSON_AddStringToObject(e, "message", errs->e[i].msg);
            cJSON_AddItemToArray(list, e);
        }
        if (errs->total > errs->n) cJSON_AddNumberToObject(doc, "more_errors", errs->total - errs->n);
    }
    return send_json(conn, status, doc);
}

/* ---- authentication ------------------------------------------------------------- */

/* Failed logins are counted per client address, and only that address is
 * locked out: a global lockout would let anyone on the network keep the
 * owner out with a few wrong passwords. What bounds guessing overall is
 * that every failure is answered a second late on its worker (at most
 * THREADS guesses a second, whatever the addresses) and that crypt() runs
 * one at a time (auth.c). */
typedef struct {
    char   addr[48];
    int    fails;                           /* in a row */
    time_t locked_until, last;              /* monotonic */
} fail_entry_t;

/* Good logins are remembered (as keyed digests, not passwords) for
 * CACHE_SECS, a few at once so that clients of different users polling
 * side by side do not push each other out; each stays valid only while
 * the shadow file and the users file are the ones it was checked
 * against, and vigild has changed no user since (users_generation: a
 * change it made within the file's time resolution, on jffs2 a second). */
#define CACHE_N 4
typedef struct {
    unsigned char d[16];                    /* md5(key, user:password:shadow:users) */
    time_t        at;                       /* monotonic; 0: empty */
    struct stat   shadow_st, users_st;      /* st_ino 0: the file was not there */
    unsigned      users_gen;
    unsigned      perms;
} cache_entry_t;

static pthread_mutex_t g_auth_lock = PTHREAD_MUTEX_INITIALIZER;
static fail_entry_t    g_failed[FAIL_ADDRS];
static unsigned char   g_cache_key[16];     /* random, so the cache is no password hash */
static cache_entry_t   g_cache[CACHE_N];
static char            g_why_told[200];     /* last "cannot check" reason logged */

/* The authorization hook and the request handler of one request run on
 * the same CivetWeb worker thread: the hook leaves who it let in here. */
static _Thread_local unsigned t_perms;
static _Thread_local char     t_user[USERS_NAME_MAX + 96];

const char *api_request_user(void) { return t_user; }
unsigned    api_request_perms(void) { return t_perms; }

static void cache_digest(const char *user, const char *pass, const char *files, unsigned char d[16])
{
    char buf[768];
    int n = snprintf(buf, sizeof buf, "%.16s%s:%s:%s", (const char *)g_cache_key, user, pass, files);
    md5_raw(buf, n < 0 ? 0 : (size_t)n >= sizeof buf ? sizeof buf - 1 : (size_t)n, d);
    volatile char *w = buf;
    for (size_t i = 0; i < sizeof buf; ++i) w[i] = 0;
}

static void stat_or_none(const char *path, struct stat *st)
{
    if (stat(path, st) < 0) memset(st, 0, sizeof *st);
}

/* The entry for `addr` (caller holds g_auth_lock); with `make`, a new one
 * if there is none, replacing the one least recently failed. */
static fail_entry_t *fail_entry(const char *addr, int make)
{
    fail_entry_t *oldest = &g_failed[0];
    for (int i = 0; i < FAIL_ADDRS; ++i) {
        fail_entry_t *f = &g_failed[i];
        if (f->addr[0] && !strcmp(f->addr, addr)) return f;
        if (!f->addr[0] || (oldest->addr[0] && f->last < oldest->last)) oldest = f;
    }
    if (!make) return NULL;
    memset(oldest, 0, sizeof *oldest);
    snprintf(oldest->addr, sizeof oldest->addr, "%s", addr);
    return oldest;
}

static int send_401(struct mg_connection *conn)
{
    return send_text(conn, 401, "{\"error\":\"authentication required\"}\n", "WWW-Authenticate",
                     "Basic realm=\"vigil\", charset=\"UTF-8\"");
}

/* One login from `addr`: 1 with *perms, 0 refused (after FAIL_DELAY_S),
 * -2 while the address is locked out (*retry_after seconds). After
 * FAIL_FREE failures in a row from one address, further attempts from it
 * are refused for a doubling time without looking at the password:
 * crypt() is slow on these CPUs by design, and an attacker must not get to
 * run it at will. */
/* Seconds `addr` is still locked out for; 0 when it is not. */
static long locked_out(const char *addr)
{
    pthread_mutex_lock(&g_auth_lock);
    fail_entry_t *fe = fail_entry(addr, 0);
    time_t wait = fe ? fe->locked_until - mono() : 0;
    pthread_mutex_unlock(&g_auth_lock);
    return wait > 0 ? (long)wait : 0;
}

/* After a login was checked: a success clears the address's failures, a
 * failure counts (and is answered FAIL_DELAY_S late). Returns 1 or 0. */
static int login_counted(const char *addr, int ok, const char *why)
{
    pthread_mutex_lock(&g_auth_lock);
    int fails = 0;
    fail_entry_t *fe = fail_entry(addr, !ok);
    if (ok) {
        if (fe) memset(fe, 0, sizeof *fe);
    } else {
        fails = ++fe->fails;
        fe->last = mono();
        if (fails >= FAIL_FREE) {
            int s = fails - FAIL_FREE + 1;
            fe->locked_until = fe->last + (s >= 6 ? LOCKOUT_MAX_S : 1 << s);
        }
    }
    int tell = why[0] && strcmp(why, g_why_told) != 0;
    if (tell) snprintf(g_why_told, sizeof g_why_told, "%s", why);
    pthread_mutex_unlock(&g_auth_lock);
    if (ok) return 1;
    /* "auth": the API's logins, the zero channel's (web.c) and ONVIF's alike */
    if (tell) vg_log("auth: cannot check passwords: %s", why);
    vg_log("auth: login failed from %s (%d in a row)", addr, fails);
    sleep(FAIL_DELAY_S);
    return 0;
}

int api_login_with(const char *addr, const char *user, int (*check)(const char *pass, void *ctx), void *ctx,
                   unsigned *perms, long *retry_after)
{
    *perms = 0;
    *retry_after = locked_out(addr);
    if (*retry_after) return -2;
    char why[200] = "";
    int ok = users_login_with(user, check, ctx, perms, why, sizeof why) == 1;
    if (!ok) *perms = 0;
    return login_counted(addr, ok, why);
}

int api_login(const char *addr, const char *user, const char *pass, unsigned *perms, long *retry_after)
{
    vigil_config_t *c = malloc(sizeof *c);
    if (!c) return 0;
    config_get(c);
    config_api_t api = c->api;
    free(c);
    *perms = 0;
    *retry_after = locked_out(addr);
    if (*retry_after) return -2;
    time_t now = mono();

    char why[160] = "";
    int ok = 0;
    unsigned char d[16];
    struct stat sh, us;
    char files[400];
    stat_or_none(api.shadow, &sh);
    stat_or_none(api.users, &us);
    snprintf(files, sizeof files, "%s:%s", api.shadow, api.users);
    cache_digest(user, pass, files, d);
    unsigned gen = users_generation();
    pthread_mutex_lock(&g_auth_lock);
    for (int i = 0; i < CACHE_N && !ok; ++i) {
        cache_entry_t *ce = &g_cache[i];
        if (ce->at && now - ce->at < CACHE_SECS && users_same_file(&sh, &ce->shadow_st) &&
            users_same_file(&us, &ce->users_st) && ce->users_gen == gen && memcmp(d, ce->d, sizeof d) == 0) {
            ok = 1;
            *perms = ce->perms;
        }
    }
    pthread_mutex_unlock(&g_auth_lock);
    if (!ok) {
        ok = users_login(user, pass, perms, why, sizeof why) == 1;
        if (ok) {
            /* After the login, which may have written the file (sealing
             * the password at a first login). */
            gen = users_generation();
            stat_or_none(api.shadow, &sh);
            stat_or_none(api.users, &us);
            pthread_mutex_lock(&g_auth_lock);
            cache_entry_t *ce = &g_cache[0];
            for (int i = 1; i < CACHE_N; ++i)
                if (g_cache[i].at < ce->at) ce = &g_cache[i];
            memcpy(ce->d, d, sizeof d);
            ce->at = now;
            ce->shadow_st = sh;
            ce->users_st = us;
            ce->users_gen = gen;
            ce->perms = *perms;
            pthread_mutex_unlock(&g_auth_lock);
        }
    }

    if (!ok) *perms = 0;
    return login_counted(addr, ok, why);
}

/* CivetWeb's authorization hook for everything under /api: 1 = go on. */
static int auth_handler(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    vigil_config_t *c = malloc(sizeof *c);
    if (!c) {
        send_text(conn, 500, "{\"error\":\"out of memory\"}\n", NULL, NULL);
        return 0;
    }
    config_get(c);
    int auth = c->api.auth;
    free(c);
    t_perms = 0;
    t_user[0] = 0;
    if (!auth) {
        t_perms = PERM_ALL;
        return 1;
    }

    const char *hdr = mg_get_header(conn, "Authorization");
    if (!hdr) {
        send_401(conn);
        return 0;
    }
    char user[128], pass[256];
    unsigned perms = 0;
    long retry = 0;
    int rc = 0;
    if (auth_parse_basic(hdr, user, sizeof user, pass, sizeof pass) == 0) {
        rc = api_login(mg_get_request_info(conn)->remote_addr, user, pass, &perms, &retry);
    } else {
        /* Not Basic, or not decodable: a failure like a wrong password. */
        rc = api_login(mg_get_request_info(conn)->remote_addr, "", "", &perms, &retry);
    }
    volatile char *w = pass;
    for (size_t i = 0; i < sizeof pass; ++i) w[i] = 0;
    if (rc == -2) {
        char ra[24];
        snprintf(ra, sizeof ra, "%ld", retry);
        send_text(conn, 429, "{\"error\":\"too many failed logins; try again later\"}\n", "Retry-After", ra);
        return 0;
    }
    if (rc == 1) {
        t_perms = perms;
        snprintf(t_user, sizeof t_user, "%s", user);
        return 1;
    }
    send_401(conn);
    return 0;
}

/* ---- status ---------------------------------------------------------------------- */

static cJSON *status_json(void)
{
    const hal_caps_t *caps = hal_caps();
    vigil_config_t *c = malloc(sizeof *c);
    cJSON *doc = cJSON_CreateObject();
    if (!c || !doc) {
        free(c);
        cJSON_Delete(doc);
        return NULL;
    }
    unsigned gen = config_get(c);

    cJSON_AddStringToObject(doc, "version", VIGIL_VERSION);
    char host[64];
    host_get(host, sizeof host);
    cJSON_AddStringToObject(doc, "hostname", host);
    cJSON_AddNumberToObject(doc, "uptime", (double)(mono() - g_t0));
    cJSON_AddNumberToObject(doc, "time", (double)time(NULL));
    cJSON *cfg = cJSON_AddObjectToObject(doc, "config");
    cJSON_AddNumberToObject(cfg, "generation", gen);
    cJSON_AddNumberToObject(cfg, "applied", config_applied());

    cJSON *p = cJSON_AddObjectToObject(doc, "platform");
    cJSON_AddStringToObject(p, "name", caps->name);
    cJSON_AddBoolToObject(p, "display", caps->display);
    cJSON_AddBoolToObject(p, "decoder", caps->decoder);
    cJSON_AddNumberToObject(p, "decoder_max_width", caps->dec_max_w);
    cJSON_AddNumberToObject(p, "decoder_max_height", caps->dec_max_h);
    cJSON_AddNumberToObject(p, "max_channels", caps->max_channels);
    cJSON_AddNumberToObject(p, "capture_inputs", caps->capture);
    cJSON_AddNumberToObject(p, "max_in_kbps", caps->max_in_kbps);

    cJSON *chans = cJSON_AddArrayToObject(doc, "channels");
    int n = channels_count();
    if (c->n_cameras > n) n = c->n_cameras;
    for (int i = 0; i < n; ++i) {
        channel_status_t st;
        channels_status(i, &st);
        cJSON *o = cJSON_CreateObject();
        if (!o) break;
        cJSON_AddNumberToObject(o, "camera", i + 1);
        if (i < c->n_cameras) {
            char red[1024];
            schema_redact_url(c->cameras[i].url, red, sizeof red);
            cJSON_AddStringToObject(o, "url", red);
            cJSON_AddStringToObject(o, "name", c->cameras[i].name);
            cJSON_AddBoolToObject(o, "enabled", c->cameras[i].enabled);
        } else {
            cJSON_AddStringToObject(o, "name", st.name);
        }
        cJSON_AddStringToObject(o, "host", st.host);
        cJSON_AddBoolToObject(o, "running", st.configured);
        cJSON_AddBoolToObject(o, "live", st.live);
        cJSON_AddStringToObject(o, "state", st.configured ? (st.live ? "live" : st.state) : "off");
        cJSON_AddStringToObject(o, "status", ch_state_name(st.status));
        cJSON_AddStringToObject(o, "title", st.title);
        /* Each stream on its own: `status` above is the one on screen. */
        if (st.configured) {
            cJSON *m = cJSON_AddObjectToObject(o, "main");
            if (m) {
                cJSON_AddStringToObject(m, "status", ch_state_name(st.main_status));
                cJSON_AddStringToObject(m, "state", st.main_state);
            }
            if (st.sub_status == CH_EMPTY) {
                cJSON_AddNullToObject(o, "sub");
            } else {
                cJSON *sb = cJSON_AddObjectToObject(o, "sub");
                if (sb) {
                    cJSON_AddStringToObject(sb, "status", ch_state_name(st.sub_status));
                    cJSON_AddStringToObject(sb, "state", st.sub_state);
                }
            }
        }
        cJSON *ev = cJSON_AddArrayToObject(o, "events");
        for (int b = 0; ev && ch_event_name(b); ++b)
            if (st.events & (1u << b)) cJSON_AddItemToArray(ev, cJSON_CreateString(ch_event_name(b)));
        cJSON_AddBoolToObject(o, "decoding", st.decoding);
        cJSON_AddStringToObject(o, "view", st.view);
        cJSON_AddBoolToObject(o, "no_decoder", st.no_decoder);
        cJSON_AddBoolToObject(o, "recording", st.recording);
        cJSON_AddStringToObject(o, "rec_state", st.rec_state[0] ? st.rec_state : "off");
        cJSON_AddNumberToObject(o, "rec_bytes", (double)st.rec_bytes);
        if (st.codec != VG_CODEC_NONE) cJSON_AddStringToObject(o, "codec", vg_codec_name(st.codec));
        else cJSON_AddNullToObject(o, "codec");
        cJSON_AddNumberToObject(o, "width", st.width);
        cJSON_AddNumberToObject(o, "height", st.height);
        cJSON_AddNumberToObject(o, "fps", st.fps);
        cJSON_AddNumberToObject(o, "main_kbps", st.main_kbps);
        cJSON_AddNumberToObject(o, "sub_kbps", st.sub_kbps);
        cJSON_AddItemToArray(chans, o);
    }
    free(c);
    return doc;
}

/* What makes a status worth pushing before the next second: a channel
 * coming up, going down or changing state. */
static void status_signature(char *out, size_t cap)
{
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < CHANNEL_MAX && len < cap; ++i) {
        channel_status_t st;
        channels_status(i, &st);
        int n = snprintf(out + len, cap - len, "%d%d%d%d%d%s|%s|%s|", st.configured, st.live, st.recording,
                         (int)st.status, (int)st.main_status, st.live ? "" : st.state, st.rec_state, st.name);
        if (n < 0) break;
        len += (size_t)n;
    }
}

/* ---- requests ---------------------------------------------------------------------- */

/* The body, NUL-terminated; NULL with *status set on failure. */
char *read_body(struct mg_connection *conn, size_t *len, int *status)
{
    const struct mg_request_info *ri = mg_get_request_info(conn);
    if (ri->content_length > (long long)BODY_MAX) {
        *status = 413;
        return NULL;
    }
    char *buf = malloc(BODY_MAX + 1);
    if (!buf) {
        *status = 500;
        return NULL;
    }
    size_t n = 0;
    for (;;) {
        int r = mg_read(conn, buf + n, BODY_MAX + 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
        if (n > BODY_MAX) {
            free(buf);
            *status = 413;
            return NULL;
        }
    }
    buf[n] = 0;
    *len = n;
    return buf;
}

int method_not_allowed(struct mg_connection *conn, const char *allow)
{
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "error", "method not allowed");
    char *text = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    int rc = send_text(conn, 405, text ? text : "{}", "Allow", allow);
    free(text);
    return rc;
}

static int handle_config(struct mg_connection *conn, const char *section)
{
    const char *m = mg_get_request_info(conn)->request_method;
    if (!strcmp(m, "PUT")) {
        size_t len = 0;
        int status = 0;
        char *body = read_body(conn, &len, &status);
        if (!body) return send_error(conn, status, status == 413 ? "body too large" : "out of memory", NULL);
        schema_errors_t errs = { 0 };
        config_rc_t rc = config_put_json(section, body, len, &errs);
        free(body);
        if (rc == CONFIG_ENOENT) return send_error(conn, 404, "no such section", NULL);
        if (rc == CONFIG_EINVAL) return send_error(conn, 400, "invalid configuration", &errs);
        if (rc == CONFIG_EIO) return send_error(conn, 500, "cannot save the configuration", &errs);
        if (rc == CONFIG_EUNSYNCED)
            return send_error(conn, 500, "the configuration is in force, but saving it is not confirmed", &errs);
    } else if (strcmp(m, "GET") != 0 && strcmp(m, "HEAD") != 0) {
        return method_not_allowed(conn, "GET, HEAD, PUT");
    }
    char *text = config_get_json(section, 1);
    if (!text) return send_error(conn, section ? 404 : 500, section ? "no such section" : "out of memory", NULL);
    int rc = send_text(conn, 200, text, NULL, NULL);
    free(text);
    return rc;
}

/* 403, naming the first permission the user lacks. */
static int send_denied(struct mg_connection *conn, unsigned missing)
{
    const char *name = "";
    for (int i = 0; i < PERM_N; ++i)
        if (missing & (1u << i)) {
            name = users_perm_names[i];
            break;
        }
    vg_log("api: %s %s refused to %s: needs %s", mg_get_request_info(conn)->request_method,
           mg_get_request_info(conn)->local_uri, t_user[0] ? t_user : "?", name);
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "error", "permission denied");
    cJSON_AddStringToObject(doc, "needs", name);
    return send_json(conn, 403, doc);
}

static int request_handler(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    const char *uri = ri->local_uri, *m = ri->request_method;
    int get = !strcmp(m, "GET") || !strcmp(m, "HEAD");
    unsigned need = api_perm_needed(m, uri);
    if ((t_perms & need) != need) return send_denied(conn, need & ~t_perms);

    if (!strcmp(uri, "/api/v1/status")) {
        if (!get) return method_not_allowed(conn, "GET, HEAD");
        return send_json(conn, 200, status_json());
    }
    if (!strcmp(uri, "/api/v1/schema")) {
        if (!get) return method_not_allowed(conn, "GET, HEAD");
        char *text = config_schema_json();
        if (!text) return send_error(conn, 500, "out of memory", NULL);
        int rc = send_text(conn, 200, text, NULL, NULL);
        free(text);
        return rc;
    }
    if (!strcmp(uri, "/api/v1/config")) return handle_config(conn, NULL);
    if (!strncmp(uri, "/api/v1/config/", 15)) return handle_config(conn, uri + 15);
    if (!strcmp(uri, "/api/v1/ws")) return send_error(conn, 400, "WebSocket upgrade required", NULL);
    if (!strcmp(uri, "/api/v1/me") || !strncmp(uri, "/api/v1/users", 13) || !strncmp(uri, "/api/v1/groups", 14))
        return api_users_request(conn, uri, m);
    int rc = api_mgmt_request(conn, uri, m);
    if (rc) return rc;
    if (!strncmp(uri, "/api/v1/discovery", 17) || !strncmp(uri, "/api/v1/channels/", 17) ||
        !strncmp(uri, "/api/v1/devices/", 16))
        return api_disc_request(conn, uri, m);
    return send_error(conn, 404, "not found", NULL);
}

/* ---- WebSocket -------------------------------------------------------------------------- */

/* Each client has a slot with its own writer thread and mailbox: a client
 * that stops reading blocks only its own writer (for up to CivetWeb's
 * request timeout per frame, then it is dropped), never the pusher, the
 * other clients, connects, closes or shutdown. Events and configuration
 * messages queue (the oldest go when a slow client lets WS_QUEUE fill);
 * of statuses only the latest is kept. */
#define WS_QUEUE 32

typedef struct {
    const struct mg_connection *reserved;   /* connect accepted, handshake not done */
    struct mg_connection       *conn;       /* ready: messages go here */
    int             closing, busy, dead;
    char           *q[WS_QUEUE];
    int             q_head, q_n;
    char           *status;
    pthread_cond_t  wake;
    pthread_t       thread;
    int             thread_up;
} ws_client_t;

static pthread_mutex_t g_ws_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_ws_idle = PTHREAD_COND_INITIALIZER;   /* a write finished */
static ws_client_t     g_wsc[WS_MAX];
static int             g_ws_stop;
static pthread_t       g_pusher;
static volatile int    g_pusher_stop;
static int             g_pusher_up;

static char *message(const char *type, cJSON *data)
{
    cJSON *doc = cJSON_CreateObject();
    if (!doc || !data) {
        cJSON_Delete(doc);
        cJSON_Delete(data);
        return NULL;
    }
    cJSON_AddStringToObject(doc, "type", type);
    cJSON_AddItemToObject(doc, "data", data);
    char *text = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    return text;
}

static char *config_message(unsigned gen)
{
    cJSON *d = cJSON_CreateObject();
    if (d) {
        cJSON_AddNumberToObject(d, "generation", gen);
        cJSON_AddNumberToObject(d, "applied", config_applied());
    }
    return message("config", d);
}

static void client_clear(ws_client_t *c)
{
    for (int k = 0; k < c->q_n; ++k) free(c->q[(c->q_head + k) % WS_QUEUE]);
    c->q_head = c->q_n = 0;
    free(c->status);
    c->status = NULL;
}

/* Into one client's mailbox (caller holds g_ws_lock); takes `text`. */
static void post(ws_client_t *c, char *text, int is_status)
{
    if (!text || !c->conn || c->closing || c->dead) {
        free(text);
        return;
    }
    if (is_status) {
        free(c->status);
        c->status = text;
    } else {
        if (c->q_n == WS_QUEUE) {
            free(c->q[c->q_head]);
            c->q_head = (c->q_head + 1) % WS_QUEUE;
            --c->q_n;
        }
        c->q[(c->q_head + c->q_n++) % WS_QUEUE] = text;
    }
    pthread_cond_signal(&c->wake);
}

static int clients(void)
{
    int n = 0;
    pthread_mutex_lock(&g_ws_lock);
    for (int i = 0; i < WS_MAX; ++i) n += g_wsc[i].conn != NULL;
    pthread_mutex_unlock(&g_ws_lock);
    return n;
}

/* A copy to every ready client; frees `text`. */
static void broadcast(char *text, int is_status)
{
    if (!text) return;
    pthread_mutex_lock(&g_ws_lock);
    for (int i = 0; i < WS_MAX; ++i)
        if (g_wsc[i].conn) post(&g_wsc[i], strdup(text), is_status);
    pthread_mutex_unlock(&g_ws_lock);
    free(text);
}

static void *writer(void *arg)
{
    ws_client_t *c = arg;
    pthread_mutex_lock(&g_ws_lock);
    while (!g_ws_stop) {
        char *msg = NULL;
        if (c->conn && !c->closing && !c->dead) {
            if (c->q_n) {
                msg = c->q[c->q_head];
                c->q_head = (c->q_head + 1) % WS_QUEUE;
                --c->q_n;
            } else if (c->status) {
                msg = c->status;
                c->status = NULL;
            }
        }
        if (!msg) {
            pthread_cond_wait(&c->wake, &g_ws_lock);
            continue;
        }
        struct mg_connection *conn = c->conn;
        c->busy = 1;
        pthread_mutex_unlock(&g_ws_lock);
        int rc = mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, msg, strlen(msg));
        free(msg);
        pthread_mutex_lock(&g_ws_lock);
        c->busy = 0;
        /* Failed or timed out: write no more. CivetWeb's own ping to it
         * fails the same way and closes the connection. */
        if (rc <= 0 && !c->dead) {
            c->dead = 1;
            client_clear(c);
        }
        pthread_cond_broadcast(&g_ws_idle);
    }
    pthread_mutex_unlock(&g_ws_lock);
    return NULL;
}

/* A browser sends a page's Origin with its WebSocket upgrade, and its
 * cached Basic credentials with it: a page from elsewhere must not get to
 * read the NVR's status through the user's login. Clients that are not
 * browsers send no Origin. */
static int origin_ok(const struct mg_connection *conn)
{
    const char *origin = mg_get_header(conn, "Origin");
    const char *host = mg_get_header(conn, "Host");
    if (!origin) return 1;
    const char *p = strstr(origin, "://");
    p = p ? p + 3 : origin;
    return host && !strcasecmp(p, host);
}

static int ws_connect(const struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    if (!(t_perms & PERM_LIVE)) {
        send_denied((struct mg_connection *)conn, PERM_LIVE);
        return 1;
    }
    if (!origin_ok(conn)) {
        vg_log("api: WebSocket from %s refused: Origin %s is not this host",
               mg_get_request_info(conn)->remote_addr, mg_get_header(conn, "Origin"));
        return 1;
    }
    int got = 0;
    pthread_mutex_lock(&g_ws_lock);
    for (int i = 0; i < WS_MAX && !got; ++i) {
        ws_client_t *c = &g_wsc[i];
        if (c->thread_up && !c->conn && !c->reserved) {
            c->reserved = conn;
            got = 1;
        }
    }
    pthread_mutex_unlock(&g_ws_lock);
    if (!got) vg_log("api: WebSocket from %s refused: %d clients already",
                     mg_get_request_info(conn)->remote_addr, WS_MAX);
    return !got;            /* nonzero: close */
}

static void ws_ready(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    /* What this WebSocket carries is events, the alarm class's. */
    api_qos_conn(conn, QOS_ALARM);
    /* A client starts with the state of things, not a second of silence. */
    char *st = message("status", status_json());
    char *cf = config_message(config_generation());
    pthread_mutex_lock(&g_ws_lock);
    for (int i = 0; i < WS_MAX; ++i) {
        ws_client_t *c = &g_wsc[i];
        if (c->reserved != conn) continue;
        c->reserved = NULL;
        c->conn = conn;
        post(c, st, 1);
        post(c, cf, 0);
        st = cf = NULL;
        break;
    }
    pthread_mutex_unlock(&g_ws_lock);
    free(st);
    free(cf);
}

static int ws_data(struct mg_connection *conn, int bits, char *data, size_t len, void *cbdata)
{
    (void)conn;
    (void)data;
    (void)len;
    (void)cbdata;
    /* Nothing is read from clients yet; only a close ends the session. */
    return (bits & 0x0f) != MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE;
}

/* CivetWeb frees the connection after this returns: wait for a write to
 * it that is under way (it ends at its timeout, or at once on mg_stop). */
static void ws_close(const struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    pthread_mutex_lock(&g_ws_lock);
    for (int i = 0; i < WS_MAX; ++i) {
        ws_client_t *c = &g_wsc[i];
        if (c->reserved == conn) {
            c->reserved = NULL;
            break;
        }
        if (c->conn != conn) continue;
        c->closing = 1;
        while (c->busy) pthread_cond_wait(&g_ws_idle, &g_ws_lock);
        client_clear(c);
        c->conn = NULL;
        c->closing = c->dead = 0;
        break;
    }
    pthread_mutex_unlock(&g_ws_lock);
}

/* One thread watches for news and puts it in every client's mailbox:
 * events as they come (from its own events cursor, so the UI still sees
 * them all), a config message when the generation changes, the status
 * every second and when a channel changes state. */
static void *pusher(void *arg)
{
    (void)arg;
    uint64_t cursor = events_head();
    unsigned gen = config_generation(), applied = config_applied();
    time_t last_status = 0;
    char sig[CHANNEL_MAX * 160], last_sig[sizeof sig] = "";

    while (!g_pusher_stop) {
        usleep(PUSH_MS * 1000);
        int n = clients();

        event_t ev;
        while (events_next(&cursor, &ev)) {
            if (!n) continue;
            cJSON *d = cJSON_CreateObject();
            if (d) {
                cJSON_AddNumberToObject(d, "seq", (double)ev.seq);
                cJSON_AddNumberToObject(d, "time", (double)ev.time);
                cJSON_AddNumberToObject(d, "camera", ev.ch);
                cJSON_AddStringToObject(d, "text", ev.text);
                if (ev.missed) cJSON_AddNumberToObject(d, "missed", ev.missed);
                /* A camera's event: which, began or ended, its own name for it. */
                if (ev.cam_type) {
                    cJSON_AddStringToObject(d, "event", ch_event_name(__builtin_ctz(ev.cam_type)));
                    cJSON_AddStringToObject(d, "state", ev.cam_active ? "start" : "stop");
                    if (ev.cam_detail[0]) cJSON_AddStringToObject(d, "detail", ev.cam_detail);
                }
            }
            broadcast(message("event", d), 0);
        }

        unsigned g = config_generation(), a = config_applied();
        if (g != gen || a != applied) {
            gen = g;
            applied = a;
            if (n) broadcast(config_message(g), 0);
        }

        status_signature(sig, sizeof sig);
        time_t now = mono();
        if (n && (now - last_status >= STATUS_SECS || strcmp(sig, last_sig) != 0)) {
            broadcast(message("status", status_json()), 1);
            last_status = now;
        }
        memcpy(last_sig, sig, sizeof sig);
    }
    return NULL;
}

/* ---- server ------------------------------------------------------------------------------ */

static int log_message(const struct mg_connection *conn, const char *msg)
{
    (void)conn;
    vg_log("api: %s", msg);
    return 1;
}

static void random_key_once(void)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, g_cache_key, sizeof g_cache_key) : -1;
    if (fd >= 0) close(fd);
    if (n != (ssize_t)sizeof g_cache_key) {
        /* No urandom: the cache key only has to differ from run to run. */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        unsigned v = (unsigned)ts.tv_nsec ^ (unsigned)getpid() << 16;
        for (size_t i = 0; i < sizeof g_cache_key; ++i) g_cache_key[i] = (unsigned char)(v >> (i % 4 * 8)) ^ (unsigned char)i;
    }
    for (size_t i = 0; i < sizeof g_cache_key; ++i)       /* printable for %.16s */
        g_cache_key[i] = (unsigned char)('!' + g_cache_key[i] % 90);
}

void api_qos_listeners(const struct mg_context *ctx, qos_class_t c)
{
    struct mg_server_port ports[4];
    int n = mg_get_server_ports(ctx, 4, ports);
    for (int i = 0; i < n; ++i)
        if (qos_mark_listeners(ports[i].port, c) <= 0) vg_log("qos: no listener found on port %d", ports[i].port);
}

void api_qos_conn(const struct mg_connection *conn, qos_class_t c)
{
    const struct mg_request_info *ri = mg_get_request_info(conn);
    if (ri && qos_mark_peer(ri->server_port, ri->remote_addr, ri->remote_port, c) < 0)
        vg_log("qos: the connection from %s:%d not found", ri->remote_addr, ri->remote_port);
}

void api_auth_init(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, random_key_once);
}

/* What each server listens on, for Network -> Ports (api.h, web.h,
 * onvif.h, netip_srv.h); here for all of them, so that reading it needs none of the
 * servers linked. */
static pthread_mutex_t g_listen_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char listening[64], why[160];
    char move_failed[200];          /* the last move that failed, until the next one */
} g_srv[4];                          /* api, web, onvif, netip */

void api_server_state(int srv, const char *listen, const char *why)
{
    pthread_mutex_lock(&g_listen_lock);
    snprintf(g_srv[srv].listening, sizeof g_srv[0].listening, "%s", listen);
    snprintf(g_srv[srv].why, sizeof g_srv[0].why, "%s", why);
    pthread_mutex_unlock(&g_listen_lock);
}

void api_server_moved(int srv, const char *listen, int failed, int requested, char *err, size_t cap)
{
    pthread_mutex_lock(&g_listen_lock);
    char why[sizeof g_srv[0].why];
    memcpy(why, g_srv[srv].why, sizeof why);
    if (failed)
        snprintf(g_srv[srv].move_failed, sizeof g_srv[0].move_failed, "could not move to '%s': %s", listen, why);
    else if (requested)
        g_srv[srv].move_failed[0] = 0;
    if (failed && err) snprintf(err, cap, "%s", why);
    pthread_mutex_unlock(&g_listen_lock);
}

static int server_listening(int srv, char *listen, size_t cap, char *why, size_t wcap)
{
    pthread_mutex_lock(&g_listen_lock);
    int on = g_srv[srv].listening[0] != 0;
    if (listen) snprintf(listen, cap, "%s", g_srv[srv].listening);
    if (why) snprintf(why, wcap, "%s", on ? g_srv[srv].move_failed : g_srv[srv].why);
    pthread_mutex_unlock(&g_listen_lock);
    return on;
}

int api_listening(char *listen, size_t cap, char *why, size_t wcap)
{
    return server_listening(0, listen, cap, why, wcap);
}

int web_listening(char *listen, size_t cap, char *why, size_t wcap)
{
    return server_listening(1, listen, cap, why, wcap);
}

int onvif_listening(char *listen, size_t cap, char *why, size_t wcap)
{
    return server_listening(2, listen, cap, why, wcap);
}

int netip_server_listening(char *listen, size_t cap, char *why, size_t wcap)
{
    return server_listening(3, listen, cap, why, wcap);
}

static void set_listening(const char *listen, const char *why) { api_server_state(0, listen, why); }

static int start_at(const char *listen, int auth)
{
    config_api_t api;
    memset(&api, 0, sizeof api);
    snprintf(api.listen, sizeof api.listen, "%s", listen);
    api.auth = auth;
    if (!api.listen[0]) {
        vg_log("api: off (api.listen is empty)");
        set_listening("", "off: api.listen is empty");
        return 0;
    }
    char why[128];
    if (net_listen_check(api.listen, why, sizeof why) < 0) {
        vg_log("api: cannot listen on %s: %s; running without the API", api.listen, why);
        set_listening("", why);
        return -1;
    }
    g_t0 = mono();
    api_auth_init();

    const char *options[] = {
        "listening_ports", api.listen,
        "num_threads", THREADS,
        "request_timeout_ms", "10000",
        "websocket_timeout_ms", "10000",
        "enable_websocket_ping_pong", "yes",
        "max_request_size", "16384",
        NULL,
    };
    struct mg_callbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.log_message = log_message;
    mg_init_library(0);
    g_ctx = mg_start(&cb, NULL, options);
    if (!g_ctx) {
        vg_log("api: cannot listen on %s; running without the API", api.listen);
        set_listening("", "the web server library could not start there");
        mg_exit_library();
        return -1;
    }
    set_listening(api.listen, "");
    api_qos_listeners(g_ctx, QOS_MANAGEMENT);
    mg_set_auth_handler(g_ctx, "/api", auth_handler, NULL);
    mg_set_request_handler(g_ctx, "/api/v1", request_handler, NULL);
    mg_set_websocket_handler(g_ctx, "/api/v1/ws", ws_connect, ws_ready, ws_data, ws_close, NULL);

    g_ws_stop = 0;
    for (int i = 0; i < WS_MAX; ++i) {
        ws_client_t *c = &g_wsc[i];
        pthread_cond_init(&c->wake, NULL);
        c->thread_up = pthread_create(&c->thread, NULL, writer, c) == 0;
    }
    g_pusher_stop = 0;
    g_pusher_up = pthread_create(&g_pusher, NULL, pusher, NULL) == 0;
    if (!g_pusher_up) vg_log("api: cannot start the WebSocket thread; no pushed updates");
    vg_log("api: listening on %s%s", api.listen,
           api.auth ? "" : " WITHOUT authentication (api.auth is off)");
    return 0;
}

int api_start(void)
{
    vigil_config_t *c = malloc(sizeof *c);
    if (!c) return -1;
    config_get(c);
    config_api_t api = c->api;
    free(c);
    return start_at(api.listen, api.auth);
}

int api_restart(const char *listen, char *err, size_t cap)
{
    vigil_config_t *c = malloc(sizeof *c);
    if (!c) return -1;
    config_get(c);
    int auth = c->api.auth;
    free(c);
    api_stop();
    int rc = start_at(listen, auth);
    api_server_moved(0, listen, rc < 0, err != NULL, err, cap);
    return rc;
}

void api_stop(void)
{
    if (!g_ctx) return;
    if (g_pusher_up) {
        g_pusher_stop = 1;
        pthread_join(g_pusher, NULL);
        g_pusher_up = 0;
    }
    /* mg_stop cuts short any write in progress, and each connection's
     * close (ws_close) waits for its writer to let go of it. */
    mg_stop(g_ctx);
    g_ctx = NULL;
    pthread_mutex_lock(&g_ws_lock);
    g_ws_stop = 1;
    for (int i = 0; i < WS_MAX; ++i) pthread_cond_signal(&g_wsc[i].wake);
    pthread_mutex_unlock(&g_ws_lock);
    for (int i = 0; i < WS_MAX; ++i) {
        ws_client_t *c = &g_wsc[i];
        if (c->thread_up) pthread_join(c->thread, NULL);
        c->thread_up = 0;
        client_clear(c);
        pthread_cond_destroy(&c->wake);
    }
    mg_exit_library();
    set_listening("", "stopped");
}
