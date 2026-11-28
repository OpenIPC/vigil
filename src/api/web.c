/* SPDX-License-Identifier: GPL-3.0-or-later */
/* web.c — the zero channel's web server (web.h, docs/api.md "Zero channel").
 *
 * A second CivetWeb server, at web.listen (port 80), with one job: a page
 * that shows the monitor's picture live and sends the browser's mouse and
 * keyboard back to the UI. That is the console, so:
 *
 *   - Logging in is always required, whatever api.auth says, with the
 *     API's accounts and its check (api_login: vigil's users, the system
 *     account; failures from an address count with the API's and lock it
 *     out the same way). Until the system account has a password and no
 *     user of the file has the permission, nobody gets in.
 *   - Only users whose group has the "remote" permission (admin's has
 *     every permission) get in. What they do on screen is then what
 *     someone at the monitor could do: with ui.login on, the menus still
 *     ask for the UI's own login and check that user's permissions.
 *   - A login is a session cookie (HttpOnly, SameSite=Strict), not cached
 *     Basic credentials; the WebSocket must come from this host's own page
 *     (Origin), and the pages may not be framed.
 *
 * The page talks over one WebSocket (/ws): binary pictures (kvm_enc.h),
 * acked by the browser, so at most two are on the way and a slow link or
 * browser slows the pictures down rather than queueing them; JSON for
 * input (kvm_input.h), options, pings and stats. The pictures and the
 * input are the app's (kvm.h): this file never touches the UI.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include "civetweb.h"
#include "cJSON.h"

#include "api_int.h"
#include "auth.h"
#include "config.h"
#include "hal.h"
#include "kvm.h"
#include "log.h"
#include "logs.h"
#include "net.h"
#include "users.h"
#include "web.h"

#define THREADS      "6"            /* KVM_CLIENTS WebSockets + pages and logins */
#define SESS_MAX     16
#define SESS_SECS    (12 * 3600)    /* a login lasts this long at most */
#define COOKIE       "vigil_zc"
#define INFLIGHT     2              /* pictures sent and not yet acked */
#define ACK_WAIT_MS  3000           /* ... after which the next goes anyway */
#define STATS_MS     1000

extern const unsigned char web_login_html[], web_kvm_html[], web_kvm_js[];
extern const size_t        web_login_html_len, web_kvm_html_len, web_kvm_js_len;

static struct mg_context *g_ctx;

static time_t mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static long long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static config_web_t web_config(config_api_t *api)
{
    config_web_t w = { 0 };
    vigil_config_t *c = malloc(sizeof *c);
    if (!c) return w;
    config_get(c);
    w = c->web;
    if (api) *api = c->api;
    free(c);
    return w;
}

/* ---- sessions -------------------------------------------------------------------------- */

typedef struct {
    char        tok[65];            /* "": free */
    char        user[USERS_NAME_MAX];
    unsigned    perms;
    time_t      made;
    struct stat shadow_st, users_st;    /* the files it was checked against */
    unsigned    users_gen;              /* and vigild's users then */
} sess_t;

static pthread_mutex_t g_sess_lock = PTHREAD_MUTEX_INITIALIZER;
static sess_t          g_sess[SESS_MAX];

static void stat_or_none(const char *path, struct stat *st)
{
    if (stat(path, st) < 0) memset(st, 0, sizeof *st);
}

static int random_hex(char *out, size_t bytes)
{
    unsigned char r[32];
    if (bytes > sizeof r) return -1;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, r, bytes) : -1;
    if (fd >= 0) close(fd);
    if (n != (ssize_t)bytes) return -1;
    for (size_t i = 0; i < bytes; ++i) snprintf(out + 2 * i, 3, "%02x", r[i]);
    return 0;
}

static int sess_new(const char *user, unsigned perms, char tok[65])
{
    config_api_t api;
    web_config(&api);
    if (random_hex(tok, 32) < 0) return -1;
    unsigned gen = users_generation();
    pthread_mutex_lock(&g_sess_lock);
    sess_t *s = &g_sess[0];
    for (int i = 0; i < SESS_MAX; ++i) {
        if (!g_sess[i].tok[0]) {
            s = &g_sess[i];
            break;
        }
        if (g_sess[i].made < s->made) s = &g_sess[i];       /* the oldest goes */
    }
    memset(s, 0, sizeof *s);
    memcpy(s->tok, tok, 64);
    s->tok[64] = 0;
    snprintf(s->user, sizeof s->user, "%.*s", (int)sizeof s->user - 1, user);
    s->perms = perms;
    s->made = mono();
    stat_or_none(api.shadow, &s->shadow_st);
    stat_or_none(api.users, &s->users_st);
    s->users_gen = gen;
    pthread_mutex_unlock(&g_sess_lock);
    return 0;
}

/* The session of token `t`: 1 with its user and permissions. A session
 * ends after SESS_SECS, and when the shadow or users file changes (a
 * password changed, a user disabled or moved to another group: log in
 * again). */
static int sess_find(const char *t, char *user, size_t ucap, unsigned *perms)
{
    config_api_t api;
    web_config(&api);
    struct stat sh, us;
    stat_or_none(api.shadow, &sh);
    stat_or_none(api.users, &us);
    unsigned gen = users_generation();
    int ok = 0;
    pthread_mutex_lock(&g_sess_lock);
    for (int i = 0; i < SESS_MAX && !ok; ++i) {
        sess_t *s = &g_sess[i];
        if (!s->tok[0] || !auth_equal(s->tok, t)) continue;
        if (mono() - s->made >= SESS_SECS || !users_same_file(&sh, &s->shadow_st) ||
            !users_same_file(&us, &s->users_st) || s->users_gen != gen) {
            memset(s, 0, sizeof *s);
            break;
        }
        if (user) snprintf(user, ucap, "%s", s->user);
        if (perms) *perms = s->perms;
        ok = 1;
    }
    pthread_mutex_unlock(&g_sess_lock);
    return ok;
}

/* The request's session (its cookie), with its token. */
static int sess_get(const struct mg_connection *conn, char *user, size_t ucap, unsigned *perms, char tok[65])
{
    const char *hdr = mg_get_header(conn, "Cookie");
    char t[80];
    if (!hdr || mg_get_cookie(hdr, COOKIE, t, sizeof t) != 64) return 0;
    if (!sess_find(t, user, ucap, perms)) return 0;
    if (tok) memcpy(tok, t, 65);
    return 1;
}

static void sess_drop(const char *tok)
{
    pthread_mutex_lock(&g_sess_lock);
    for (int i = 0; i < SESS_MAX; ++i)
        if (g_sess[i].tok[0] && auth_equal(g_sess[i].tok, tok)) memset(&g_sess[i], 0, sizeof g_sess[i]);
    pthread_mutex_unlock(&g_sess_lock);
}

/* ---- pages ----------------------------------------------------------------------------- */

static void common_headers(struct mg_connection *conn)
{
    mg_response_header_add(conn, "Cache-Control", "no-store", -1);
    mg_response_header_add(conn, "X-Frame-Options", "DENY", -1);
    mg_response_header_add(conn, "X-Content-Type-Options", "nosniff", -1);
    /* Not no-referrer: with it browsers send "Origin: null" with the
     * login form, which origin_ok must refuse. */
    mg_response_header_add(conn, "Referrer-Policy", "same-origin", -1);
    mg_response_header_add(conn, "Content-Security-Policy",
                           "default-src 'none'; script-src 'self'; style-src 'unsafe-inline'; "
                           "img-src 'self' blob: data:; connect-src 'self'; form-action 'self'; "
                           "frame-ancestors 'none'; base-uri 'none'", -1);
}

static int send_body(struct mg_connection *conn, int status, const char *type, const void *body, size_t len,
                     const char *h1, const char *v1, const char *h2, const char *v2)
{
    char cl[24];
    snprintf(cl, sizeof cl, "%zu", len);
    mg_response_header_start(conn, status);
    mg_response_header_add(conn, "Content-Type", type, -1);
    mg_response_header_add(conn, "Content-Length", cl, -1);
    common_headers(conn);
    if (h1) mg_response_header_add(conn, h1, v1, -1);
    if (h2) mg_response_header_add(conn, h2, v2, -1);
    mg_response_header_send(conn);
    if (strcmp(mg_get_request_info(conn)->request_method, "HEAD") != 0) mg_write(conn, body, len);
    return status;
}

static int redirect(struct mg_connection *conn, int status, const char *to, const char *cookie)
{
    return send_body(conn, status, "text/plain; charset=utf-8", "", 0, "Location", to,
                     cookie ? "Set-Cookie" : NULL, cookie);
}

/* The login page with `msg` (plain text, escaped here) in its place. */
static int login_page(struct mg_connection *conn, int status, const char *msg, const char *h, const char *v)
{
    const char *page = (const char *)web_login_html, *mark = "<!--MSG-->";
    const char *at = strstr(page, mark);
    char esc[512] = "";
    size_t n = 0;
    if (msg && *msg) {
        n = (size_t)snprintf(esc, sizeof esc, "<p class=\"err\">");
        for (const char *p = msg; *p && n + 8 < sizeof esc; ++p) {
            const char *r = *p == '<' ? "&lt;" : *p == '>' ? "&gt;" : *p == '&' ? "&amp;" : *p == '"' ? "&quot;" : NULL;
            if (r) n += (size_t)snprintf(esc + n, sizeof esc - n, "%s", r);
            else esc[n++] = *p;
        }
        n += (size_t)snprintf(esc + n, sizeof esc - n, "</p>");
    }
    size_t len = web_login_html_len, pre = at ? (size_t)(at - page) : len;
    char *out = malloc(len + n + 1);
    if (!out) return send_body(conn, 500, "text/plain", "out of memory", 13, NULL, NULL, NULL, NULL);
    memcpy(out, page, pre);
    memcpy(out + pre, esc, n);
    size_t tail = at ? len - pre - strlen(mark) : 0;
    if (at) memcpy(out + pre + n, at + strlen(mark), tail);
    int rc = send_body(conn, status, "text/html; charset=utf-8", out, pre + n + tail, h, v, NULL, NULL);
    free(out);
    return rc;
}

/* A browser sends its page's Origin with a POST and a WebSocket upgrade:
 * it must be this host's, so another site cannot log a browser in or
 * drive the console through its cookie. */
static int origin_ok(const struct mg_connection *conn)
{
    const char *origin = mg_get_header(conn, "Origin");
    const char *host = mg_get_header(conn, "Host");
    if (!origin) return 1;
    const char *p = strstr(origin, "://");
    p = p ? p + 3 : origin;
    return host && !strcasecmp(p, host);
}

static int do_login(struct mg_connection *conn)
{
    const struct mg_request_info *ri = mg_get_request_info(conn);
    if (!origin_ok(conn)) return login_page(conn, 403, "This login did not come from this page.", NULL, NULL);
    char body[1024];
    int n = 0, r;
    while (n < (int)sizeof body - 1 && (r = mg_read(conn, body + n, sizeof body - 1 - (size_t)n)) > 0) n += r;
    body[n] = 0;
    char user[64] = "", pass[256] = "";
    mg_get_var(body, (size_t)n, "user", user, sizeof user);
    mg_get_var(body, (size_t)n, "password", pass, sizeof pass);
    volatile char *w = body;
    for (size_t i = 0; i < sizeof body; ++i) w[i] = 0;
    unsigned perms = 0;
    long retry = 0;
    oplog_actor("", "web", ri->remote_addr);
    int rc = api_login(ri->remote_addr, user, pass, &perms, &retry);
    w = pass;
    for (size_t i = 0; i < sizeof pass; ++i) w[i] = 0;
    if (rc == -2) {
        char m[96], ra[24];
        snprintf(m, sizeof m, "Too many failed logins from this address: try again in %ld s.", retry);
        snprintf(ra, sizeof ra, "%ld", retry);
        return login_page(conn, 429, m, "Retry-After", ra);
    }
    if (rc != 1) return login_page(conn, 401, "Wrong user name or password.", NULL, NULL);
    if (!(perms & PERM_REMOTE)) {
        vg_log("web: %s logged in from %s, but has no remote permission", user, ri->remote_addr);
        return login_page(conn, 403, "This user may not use the zero channel (it needs the remote permission).",
                          NULL, NULL);
    }
    char tok[65], cookie[160];
    if (sess_new(user, perms, tok) < 0) return login_page(conn, 500, "No random numbers for a session.", NULL, NULL);
    snprintf(cookie, sizeof cookie, COOKIE "=%s; Path=/; HttpOnly; SameSite=Strict", tok);
    vg_log("web: %s logged in from %s", user, ri->remote_addr);
    return redirect(conn, 303, "/", cookie);
}

static int request_handler(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    const char *uri = ri->local_uri, *m = ri->request_method;
    int get = !strcmp(m, "GET") || !strcmp(m, "HEAD"), post = !strcmp(m, "POST");
    if (!web_config(NULL).enabled) {
        static const char off[] = "The zero channel is turned off (web.enabled).\n";
        return send_body(conn, 503, "text/plain; charset=utf-8", off, sizeof off - 1, NULL, NULL, NULL, NULL);
    }
    char user[USERS_NAME_MAX], tok[65];
    unsigned perms = 0;
    int in = sess_get(conn, user, sizeof user, &perms, tok) && (perms & PERM_REMOTE);

    if (!strcmp(uri, "/")) {
        if (!get) return send_body(conn, 405, "text/plain", "", 0, "Allow", "GET, HEAD", NULL, NULL);
        if (!in) return redirect(conn, 302, "/login", NULL);
        return send_body(conn, 200, "text/html; charset=utf-8", web_kvm_html, web_kvm_html_len, NULL, NULL, NULL, NULL);
    }
    if (!strcmp(uri, "/kvm.js") && get)
        return send_body(conn, 200, "text/javascript; charset=utf-8", web_kvm_js, web_kvm_js_len, NULL, NULL, NULL,
                         NULL);
    if (!strcmp(uri, "/login")) {
        if (post) return do_login(conn);
        if (!get) return send_body(conn, 405, "text/plain", "", 0, "Allow", "GET, HEAD, POST", NULL, NULL);
        return login_page(conn, 200, NULL, NULL, NULL);
    }
    if (!strcmp(uri, "/logout")) {
        if (!post) return send_body(conn, 405, "text/plain", "", 0, "Allow", "POST", NULL, NULL);
        if (!origin_ok(conn)) return send_body(conn, 403, "text/plain", "", 0, NULL, NULL, NULL, NULL);
        if (in) {
            sess_drop(tok);
            vg_log("web: %s logged out", user);
            oplog_pushf(user, "web", "logout", "from %s", ri->remote_addr);
        }
        return redirect(conn, 303, "/login", COOKIE "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    }
    if (!strcmp(uri, "/session")) {
        /* For the page: still logged in (after its WebSocket closed)? */
        if (!in) return send_body(conn, 401, "application/json", "{\"user\":null}", 13, NULL, NULL, NULL, NULL);
        char j[96];
        int n = snprintf(j, sizeof j, "{\"user\":\"%s\"}", user);
        return send_body(conn, 200, "application/json", j, (size_t)n, NULL, NULL, NULL, NULL);
    }
    if (!strcmp(uri, "/ws"))
        return send_body(conn, 400, "text/plain", "WebSocket upgrade required\n", 27, NULL, NULL, NULL, NULL);
    return send_body(conn, 404, "text/plain", "not found\n", 10, NULL, NULL, NULL, NULL);
}

/* ---- the WebSocket ----------------------------------------------------------------------- */

typedef struct {
    const struct mg_connection *reserved;   /* accepted, handshake not done */
    struct mg_connection       *conn;
    char            user[USERS_NAME_MAX], addr[48], tok[65];
    kvm_client_t   *k;
    pthread_t       thread;
    int             thread_up, stop;
    unsigned        sent, acked;
    long long       sent_at;                /* ms, the last picture */
} ws_t;

static pthread_mutex_t g_ws_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_ws_cond = PTHREAD_COND_INITIALIZER;
static ws_t            g_ws[KVM_CLIENTS];

/* A refused upgrade, written as it goes on the wire (CivetWeb's header
 * API is closed to those). */
static void refuse(const struct mg_connection *conn, int status, const char *why)
{
    struct mg_connection *c = (struct mg_connection *)conn;
    mg_printf(c, "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
              status, mg_get_response_code_text(c, status), strlen(why), why);
}

static int ws_connect(const struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    const char *addr = mg_get_request_info(conn)->remote_addr;
    char user[USERS_NAME_MAX], tok[65];
    unsigned perms = 0;
    if (!web_config(NULL).enabled) {
        refuse(conn, 503, "the zero channel is turned off\n");
        return 1;
    }
    if (!sess_get(conn, user, sizeof user, &perms, tok)) {
        refuse(conn, 401, "log in first\n");
        return 1;
    }
    if (!(perms & PERM_REMOTE)) {
        refuse(conn, 403, "needs the remote permission\n");
        return 1;
    }
    if (!origin_ok(conn)) {
        vg_log("web: WebSocket from %s refused: Origin %s is not this host", addr, mg_get_header(conn, "Origin"));
        refuse(conn, 403, "not from this host's page\n");
        return 1;
    }
    ws_t *w = NULL;
    pthread_mutex_lock(&g_ws_lock);
    for (int i = 0; i < KVM_CLIENTS && !w; ++i)
        if (!g_ws[i].conn && !g_ws[i].reserved && !g_ws[i].thread_up) w = &g_ws[i];
    if (w) {
        memset(w, 0, sizeof *w);
        w->reserved = conn;
        snprintf(w->user, sizeof w->user, "%s", user);
        snprintf(w->addr, sizeof w->addr, "%s", addr);
        snprintf(w->tok, sizeof w->tok, "%s", tok);
    }
    pthread_mutex_unlock(&g_ws_lock);
    if (!w) {
        vg_log("web: zero channel for %s at %s refused: %d viewers already", user, addr, KVM_CLIENTS);
        refuse(conn, 503, "too many viewers\n");
        return 1;
    }
    return 0;
}

static int ws_text(struct mg_connection *conn, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int ws_text(struct mg_connection *conn, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if (n >= (int)sizeof buf) n = sizeof buf - 1;
    return mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, buf, (size_t)n);
}

static void *viewer(void *arg)
{
    ws_t *w = arg;
    /* Below the channel threads: a viewer never slows the cameras. */
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
    unsigned sw, sh;
    kvm_screen(&sw, &sh);
    char who[96];
    snprintf(who, sizeof who, "%s at %s", w->user, w->addr);
    char why[128] = "";
    kvm_client_t *k = kvm_open(who, why, sizeof why);
    pthread_mutex_lock(&g_ws_lock);
    w->k = k;
    struct mg_connection *conn = w->conn;
    pthread_mutex_unlock(&g_ws_lock);
    if (!k) {
        ws_text(conn, "{\"type\":\"error\",\"message\":\"%s\"}", why);
        mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE, "", 0);
        return NULL;
    }
    config_web_t cfg = web_config(NULL);
    ws_text(conn, "{\"type\":\"hello\",\"width\":%u,\"height\":%u,\"user\":\"%s\",\"fps\":%d}", sw, sh, w->user,
            cfg.fps);
    long long stats_at = mono_ms(), cfg_at = stats_at;
    unsigned frames = 0;
    size_t bytes = 0;
    for (;;) {
        long long now = mono_ms();
        if (now - cfg_at >= 1000) {
            cfg = web_config(NULL);
            cfg_at = now;
            if (!cfg.enabled) {
                ws_text(conn, "{\"type\":\"error\",\"message\":\"the zero channel was turned off\"}");
                break;
            }
            /* Logged out elsewhere, the password or the group changed: the
             * console is not kept open on an old login. */
            if (!sess_find(w->tok, NULL, 0, NULL)) {
                vg_log("web: %s's session ended; zero channel closed", w->user);
                ws_text(conn, "{\"type\":\"error\",\"message\":\"the session ended: log in again\"}");
                break;
            }
        }
        if (now - stats_at >= STATS_MS) {
            unsigned ms;
            size_t last;
            kvm_last(k, &ms, &last);
            if (ws_text(conn, "{\"type\":\"stats\",\"fps\":%.1f,\"kbps\":%.0f,\"ms\":%u}",
                        frames * 1000.0 / (double)(now - stats_at), bytes * 8.0 / (double)(now - stats_at), ms) <= 0)
                break;
            frames = 0;
            bytes = 0;
            stats_at = now;
        }
        pthread_mutex_lock(&g_ws_lock);
        int stop = w->stop;
        int wait = !stop && w->sent - w->acked >= INFLIGHT && now - w->sent_at < ACK_WAIT_MS;
        if (wait) {
            struct timespec t;
            clock_gettime(CLOCK_REALTIME, &t);
            t.tv_nsec += 100 * 1000000L;
            if (t.tv_nsec >= 1000000000L) {
                ++t.tv_sec;
                t.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&g_ws_cond, &g_ws_lock, &t);
        }
        stop = w->stop;
        pthread_mutex_unlock(&g_ws_lock);
        if (stop) break;
        if (wait) continue;
        unsigned char *msg;
        size_t len;
        int rc = kvm_next(k, cfg.fps, &msg, &len, 250);
        if (rc < 0) {
            ws_text(conn, "{\"type\":\"error\",\"message\":\"the screen is gone\"}");
            break;
        }
        if (!rc) continue;
        pthread_mutex_lock(&g_ws_lock);
        unsigned seq = ++w->sent;
        w->sent_at = mono_ms();
        pthread_mutex_unlock(&g_ws_lock);
        /* the web server's own numbering: the browser acks these */
        msg[8] = (unsigned char)seq;
        msg[9] = (unsigned char)(seq >> 8);
        msg[10] = (unsigned char)(seq >> 16);
        msg[11] = (unsigned char)(seq >> 24);
        int wr = mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_BINARY, (const char *)msg, len);
        free(msg);
        if (wr <= 0) break;
        ++frames;
        bytes += len;
    }
    mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE, "", 0);
    return NULL;
}

static void ws_ready(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    /* The pages and the login were management; this is the picture. */
    api_qos_conn(conn, QOS_VIDEO);
    pthread_mutex_lock(&g_ws_lock);
    for (int i = 0; i < KVM_CLIENTS; ++i) {
        ws_t *w = &g_ws[i];
        if (w->reserved != conn) continue;
        w->reserved = NULL;
        w->conn = conn;
        w->thread_up = pthread_create(&w->thread, NULL, viewer, w) == 0;
        if (w->thread_up) vg_log("web: zero channel to %s at %s", w->user, w->addr);
        else vg_log("web: cannot start a thread for %s", w->user);
        break;
    }
    pthread_mutex_unlock(&g_ws_lock);
}

static ws_t *ws_of(const struct mg_connection *conn)
{
    for (int i = 0; i < KVM_CLIENTS; ++i)
        if (g_ws[i].conn == conn) return &g_ws[i];
    return NULL;
}

static int ws_data(struct mg_connection *conn, int bits, char *data, size_t len, void *cbdata)
{
    (void)cbdata;
    int op = bits & 0x0f;
    if (op == MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE) return 0;
    if (op != MG_WEBSOCKET_OPCODE_TEXT || len > 1024) return 1;
    pthread_mutex_lock(&g_ws_lock);
    ws_t *w = ws_of(conn);
    kvm_client_t *k = w ? w->k : NULL;
    pthread_mutex_unlock(&g_ws_lock);
    if (!w || !k) return 1;
    /* Input first: most of what comes is the mouse. */
    if (kvm_input(k, data, len) == 0) return 1;
    cJSON *o = cJSON_ParseWithLength(data, len);
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(o, "t");
    if (cJSON_IsString(t)) {
        if (!strcmp(t->valuestring, "ack")) {
            const cJSON *s = cJSON_GetObjectItemCaseSensitive(o, "seq");
            if (cJSON_IsNumber(s)) {
                pthread_mutex_lock(&g_ws_lock);
                unsigned seq = (unsigned)s->valuedouble;
                if (seq - w->acked <= w->sent - w->acked) w->acked = seq;
                pthread_cond_broadcast(&g_ws_cond);
                pthread_mutex_unlock(&g_ws_lock);
            }
        } else if (!strcmp(t->valuestring, "opts")) {
            const cJSON *sc = cJSON_GetObjectItemCaseSensitive(o, "scale");
            const cJSON *q = cJSON_GetObjectItemCaseSensitive(o, "quality");
            kvm_options(k, cJSON_IsNumber(sc) ? sc->valueint : 1, cJSON_IsNumber(q) ? q->valueint : 75);
        } else if (!strcmp(t->valuestring, "ping")) {
            const cJSON *c = cJSON_GetObjectItemCaseSensitive(o, "c");
            ws_text(conn, "{\"type\":\"pong\",\"c\":%.3f}", cJSON_IsNumber(c) ? c->valuedouble : 0.0);
        }
    }
    cJSON_Delete(o);
    return 1;
}

/* CivetWeb frees the connection after this returns: the viewer's thread
 * goes first (a write under way ends at its timeout, or at once on
 * mg_stop), then its mouse and keyboard. */
static void ws_close(const struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    pthread_mutex_lock(&g_ws_lock);
    ws_t *w = NULL;
    for (int i = 0; i < KVM_CLIENTS && !w; ++i) {
        if (g_ws[i].reserved == conn) {
            g_ws[i].reserved = NULL;
            pthread_mutex_unlock(&g_ws_lock);
            return;
        }
        if (g_ws[i].conn == conn) w = &g_ws[i];
    }
    if (!w) {
        pthread_mutex_unlock(&g_ws_lock);
        return;
    }
    w->stop = 1;
    kvm_client_t *k = w->k;
    pthread_cond_broadcast(&g_ws_cond);
    pthread_mutex_unlock(&g_ws_lock);
    if (k) kvm_wake(k);
    if (w->thread_up) pthread_join(w->thread, NULL);
    pthread_mutex_lock(&g_ws_lock);
    k = w->k;
    vg_log("web: zero channel to %s at %s closed", w->user, w->addr);
    memset(w, 0, sizeof *w);
    pthread_mutex_unlock(&g_ws_lock);
    kvm_close(k);
}

/* ---- server ------------------------------------------------------------------------------- */

static int log_message(const struct mg_connection *conn, const char *msg)
{
    (void)conn;
    vg_log("web: %s", msg);
    return 1;
}

/* What the server listens on, for Network -> Ports (kept by api.c). */
static void set_listening(const char *listen, const char *why) { api_server_state(1, listen, why); }

static int start_at(const char *listen)
{
    config_web_t cfg = web_config(NULL);
    snprintf(cfg.listen, sizeof cfg.listen, "%s", listen);
    if (!cfg.enabled || !cfg.listen[0]) {
        vg_log("web: zero channel off (web.%s)", cfg.enabled ? "listen is empty" : "enabled is false");
        set_listening("", cfg.enabled ? "off: web.listen is empty" : "off: web.enabled is off");
        return 0;
    }
    if (!hal_caps()->display) {
        vg_log("web: no screen on this platform; zero channel off");
        set_listening("", "off: no screen on this platform");
        return 0;
    }
    char why[128];
    if (net_listen_check(cfg.listen, why, sizeof why) < 0) {
        vg_log("web: cannot listen on %s: %s; no zero channel", cfg.listen, why);
        set_listening("", why);
        return -1;
    }
    api_auth_init();
    const char *options[] = {
        "listening_ports", cfg.listen,
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
        vg_log("web: cannot listen on %s; no zero channel", cfg.listen);
        set_listening("", "the web server library could not start there");
        mg_exit_library();
        return -1;
    }
    set_listening(cfg.listen, "");
    api_qos_listeners(g_ctx, QOS_MANAGEMENT);
    mg_set_request_handler(g_ctx, "/", request_handler, NULL);
    mg_set_websocket_handler(g_ctx, "/ws", ws_connect, ws_ready, ws_data, ws_close, NULL);
    vg_log("web: zero channel on %s", cfg.listen);
    return 0;
}

int web_start(void) { return start_at(web_config(NULL).listen); }

int web_restart(const char *listen, char *err, size_t cap)
{
    web_stop();
    int rc = start_at(listen);
    api_server_moved(1, listen, rc < 0, err != NULL, err, cap);
    return rc;
}

void web_stop(void)
{
    if (!g_ctx) return;
    /* mg_stop closes every connection; ws_close ends each viewer. */
    mg_stop(g_ctx);
    g_ctx = NULL;
    mg_exit_library();
    set_listening("", "stopped");
}
