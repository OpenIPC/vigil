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
#include "auth.h"
#include "channel.h"
#include "config.h"
#include "events.h"
#include "hal.h"
#include "log.h"
#include "md5.h"

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

static int send_text(struct mg_connection *conn, int status, const char *text, const char *extra_hdr,
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

static int send_json(struct mg_connection *conn, int status, cJSON *doc)
{
    char *text = doc ? cJSON_Print(doc) : NULL;
    cJSON_Delete(doc);
    if (!text) return send_text(conn, 500, "{\"error\":\"out of memory\"}\n", NULL, NULL);
    int rc = send_text(conn, status, text, NULL, NULL);
    free(text);
    return rc;
}

static int send_error(struct mg_connection *conn, int status, const char *msg,
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

static pthread_mutex_t g_auth_lock = PTHREAD_MUTEX_INITIALIZER;
static fail_entry_t    g_failed[FAIL_ADDRS];
static unsigned char   g_cache_key[16];     /* random, so the cache is no password hash */
static unsigned char   g_cache[16];         /* md5(key, user:password:shadow) of the last good login */
static time_t          g_cache_at;
static struct stat     g_cache_st;          /* the shadow file it was checked against */
static char            g_why_told[160];     /* last "cannot check" reason logged */

static void cache_digest(const char *user, const char *pass, const char *shadow, unsigned char d[16])
{
    char buf[512];
    int n = snprintf(buf, sizeof buf, "%.16s%s:%s:%s", (const char *)g_cache_key, user, pass, shadow);
    md5_raw(buf, n < 0 ? 0 : (size_t)n >= sizeof buf ? sizeof buf - 1 : (size_t)n, d);
    volatile char *w = buf;
    for (size_t i = 0; i < sizeof buf; ++i) w[i] = 0;
}

static int same_file(const struct stat *a, const struct stat *b)
{
    return a->st_ino == b->st_ino && a->st_dev == b->st_dev && a->st_mtime == b->st_mtime &&
           a->st_size == b->st_size;
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

/* CivetWeb's authorization hook for everything under /api: 1 = go on.
 * Failed logins are answered after a delay, and after FAIL_FREE in a row
 * from one address, further attempts from it are refused for a doubling
 * time without looking at the password: crypt() is slow on these CPUs by
 * design, and an attacker must not get to run it at will. */
static int auth_handler(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    vigil_config_t *c = malloc(sizeof *c);
    if (!c) {
        send_text(conn, 500, "{\"error\":\"out of memory\"}\n", NULL, NULL);
        return 0;
    }
    config_get(c);
    config_api_t api = c->api;
    free(c);
    if (!api.auth) return 1;

    const char *hdr = mg_get_header(conn, "Authorization");
    if (!hdr) {
        send_401(conn);
        return 0;
    }
    const char *addr = mg_get_request_info(conn)->remote_addr;
    time_t now = mono();
    pthread_mutex_lock(&g_auth_lock);
    fail_entry_t *fe = fail_entry(addr, 0);
    time_t wait = fe ? fe->locked_until - now : 0;
    pthread_mutex_unlock(&g_auth_lock);
    if (wait > 0) {
        char ra[24];
        snprintf(ra, sizeof ra, "%ld", (long)wait);
        send_text(conn, 429, "{\"error\":\"too many failed logins; try again later\"}\n", "Retry-After", ra);
        return 0;
    }

    char user[128], pass[256], why[160] = "";
    int ok = 0;
    if (auth_parse_basic(hdr, user, sizeof user, pass, sizeof pass) == 0 && !strcmp(user, api.user)) {
        unsigned char d[16];
        struct stat st;
        int have_st = stat(api.shadow, &st) == 0;
        cache_digest(user, pass, api.shadow, d);
        pthread_mutex_lock(&g_auth_lock);
        ok = have_st && g_cache_at && now - g_cache_at < CACHE_SECS && same_file(&st, &g_cache_st) &&
             memcmp(d, g_cache, sizeof d) == 0;
        pthread_mutex_unlock(&g_auth_lock);
        if (!ok) {
            int rc = auth_check_shadow(api.shadow, api.user, pass, why, sizeof why);
            ok = rc == 1;
            if (ok && have_st) {
                pthread_mutex_lock(&g_auth_lock);
                memcpy(g_cache, d, sizeof d);
                g_cache_at = now;
                g_cache_st = st;
                pthread_mutex_unlock(&g_auth_lock);
            }
        }
    }
    volatile char *w = pass;
    for (size_t i = 0; i < sizeof pass; ++i) w[i] = 0;

    pthread_mutex_lock(&g_auth_lock);
    int fails = 0;
    fe = fail_entry(addr, !ok);
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

    if (tell) vg_log("api: cannot check passwords: %s", why);
    vg_log("api: login failed from %s (%d in a row)", addr, fails);
    sleep(FAIL_DELAY_S);
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
        cJSON_AddBoolToObject(o, "decoding", st.decoding);
        cJSON_AddBoolToObject(o, "recording", st.recording);
        cJSON_AddNumberToObject(o, "rec_bytes", (double)st.rec_bytes);
        if (st.codec != VG_CODEC_NONE) cJSON_AddStringToObject(o, "codec", vg_codec_name(st.codec));
        else cJSON_AddNullToObject(o, "codec");
        cJSON_AddNumberToObject(o, "width", st.width);
        cJSON_AddNumberToObject(o, "height", st.height);
        cJSON_AddNumberToObject(o, "fps", st.fps);
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
        int n = snprintf(out + len, cap - len, "%d%d%d%s|", st.configured, st.live, st.recording,
                         st.live ? "" : st.state);
        if (n < 0) break;
        len += (size_t)n;
    }
}

/* ---- requests ---------------------------------------------------------------------- */

/* The body, NUL-terminated; NULL with *status set on failure. */
static char *read_body(struct mg_connection *conn, size_t *len, int *status)
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

static int method_not_allowed(struct mg_connection *conn, const char *allow)
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

static int request_handler(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    const char *uri = ri->local_uri, *m = ri->request_method;
    int get = !strcmp(m, "GET") || !strcmp(m, "HEAD");

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
    char sig[CHANNEL_MAX * 64], last_sig[sizeof sig] = "";

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

static void random_key(void)
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

int api_start(void)
{
    vigil_config_t *c = malloc(sizeof *c);
    if (!c) return -1;
    config_get(c);
    config_api_t api = c->api;
    free(c);
    if (!api.listen[0]) {
        vg_log("api: off (api.listen is empty)");
        return 0;
    }
    g_t0 = mono();
    random_key();

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
        mg_exit_library();
        return -1;
    }
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
}
