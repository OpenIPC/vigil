/* SPDX-License-Identifier: GPL-3.0-or-later */
/* snap_cam.c — a still picture from the camera itself (snap_cam.h). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

#include "cJSON.h"
#include "disc_ops.h"
#include "disc_proto.h"
#include "discovery.h"
#include "md5.h"
#include "net.h"
#include "netip_proto.h"
#include "onvif_proto.h"
#include "sha1.h"
#include "snap_cam.h"
#include "vg_random.h"
#include "source.h"

#define CONNECT_MS   3000
#define NETIP_PORT   34567
#define NETIP_IDLE_MS 1000          /* no count and no EOI yet: this long without more ends it */
#define ONVIF_MS     5000

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void wipe(void *p, size_t n)
{
    volatile unsigned char *w = p;
    while (n--) *w++ = 0;
}

/* ---- JPEG ----------------------------------------------------------------------- */

void snap_jpeg_free(snap_jpeg_t *j)
{
    free(j->p);
    memset(j, 0, sizeof *j);
}

int snap_jpeg_put(snap_jpeg_t *j, const void *p, size_t n)
{
    if (j->len + n > SNAP_JPEG_MAX) return -1;
    if (j->len + n > j->cap) {
        size_t cap = j->cap ? j->cap : 64 << 10;
        while (cap < j->len + n) cap *= 2;
        if (cap > SNAP_JPEG_MAX) cap = SNAP_JPEG_MAX;
        unsigned char *np = realloc(j->p, cap);
        if (!np) return -1;
        j->p = np;
        j->cap = cap;
    }
    memcpy(j->p + j->len, p, n);
    j->len += n;
    return 0;
}

int snap_jpeg_check(const unsigned char *p, size_t *len, unsigned *w, unsigned *h)
{
    size_t n = *len;
    if (n < 4 || p[0] != 0xFF || p[1] != 0xD8) return -1;
    /* NETIP bodies end in "\n\0"; some cameras pad. */
    while (n > 4 && !(p[n - 2] == 0xFF && p[n - 1] == 0xD9) && (p[n - 1] == 0 || p[n - 1] == '\n' ||
                                                                  p[n - 1] == '\r' || p[n - 1] == 0xFF))
        --n;
    if (p[n - 2] != 0xFF || p[n - 1] != 0xD9) return -1;
    /* The markers up to the first frame header: its size. */
    size_t i = 2;
    int sof = 0;
    while (i + 4 <= n) {
        if (p[i] != 0xFF) return -1;
        unsigned m = p[i + 1];
        if (m == 0xFF) { ++i; continue; }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { i += 2; continue; }
        size_t sl = (size_t)p[i + 2] << 8 | p[i + 3];
        if (sl < 2 || i + 2 + sl > n) return -1;
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (sl < 7) return -1;
            if (h) *h = (unsigned)p[i + 5] << 8 | p[i + 6];
            if (w) *w = (unsigned)p[i + 7] << 8 | p[i + 8];
            sof = 1;
            break;
        }
        if (m == 0xDA) break;           /* scan data before a frame header */
        i += 2 + sl;
    }
    if (!sof) return -1;
    *len = n;
    return 0;
}

/* ---- NETIP ------------------------------------------------------------------------ */

static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int snap_netip_body(char *out, size_t cap, uint32_t session, int ch)
{
    int n = snprintf(out, cap, "{\"Name\":\"OPSNAP\",\"OPSNAP\":{\"Channel\":%d},\"SessionID\":\"0x%08X\"}", ch,
                     (unsigned)session);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

int snap_netip_take(snap_netip_rx_t *rx, const netip_hdr_t *h, const uint8_t *body, size_t len, char *err,
                    size_t cap)
{
    if (rx->chunks == 0 && !rx->framed) {
        /* What the first message is: JSON (a refusal, or an "ok" the
         * picture follows), a framed picture, or the JPEG. */
        size_t k = 0;
        while (k < len && (body[k] == ' ' || body[k] == '\n' || body[k] == '\r' || body[k] == '\t')) ++k;
        if (k < len && body[k] == '{') {
            char *j = malloc(len + 1);
            if (!j) { snprintf(err, cap, "out of memory"); return -1; }
            memcpy(j, body, len);
            j[len] = 0;
            cJSON *doc = cJSON_Parse(j + k);
            free(j);
            const cJSON *ret = doc ? cJSON_GetObjectItemCaseSensitive(doc, "Ret") : NULL;
            int r = cJSON_IsNumber(ret) ? ret->valueint : -1;
            cJSON_Delete(doc);
            if (r == 100) return 0;     /* the picture comes next */
            snprintf(err, cap, "the camera refused the snapshot (Ret %d)", r);
            return -1;
        }
        if (len >= 16 && body[0] == 0 && body[1] == 0 && body[2] == 1 && body[3] == 0xFE) {
            rx->framed = 1;
            rx->want = get32(body + 12);
            if (rx->want < 4 || rx->want > SNAP_JPEG_MAX) {
                snprintf(err, cap, "a snapshot frame of %zu bytes", rx->want);
                return -1;
            }
            body += 16;
            len -= 16;
        } else if (len < 2 || body[0] != 0xFF || body[1] != 0xD8) {
            snprintf(err, cap, "the camera's answer is not a JPEG");
            return -1;
        }
    }
    if (rx->framed && rx->jpeg.len + len > rx->want) len = rx->want - rx->jpeg.len;
    if (snap_jpeg_put(&rx->jpeg, body, len) < 0) {
        snprintf(err, cap, "the snapshot is larger than %u MiB", SNAP_JPEG_MAX >> 20);
        return -1;
    }
    ++rx->chunks;
    int complete;
    if (rx->framed) complete = rx->jpeg.len >= rx->want;
    else if (h->total > 1) complete = rx->chunks >= h->total;
    else complete = h->total == 1 || snap_netip_done(rx);
    if (!complete) return 0;
    if (!snap_netip_done(rx)) {
        snprintf(err, cap, "the camera's JPEG is broken (%zu bytes)", rx->jpeg.len);
        return -1;
    }
    return 1;
}

int snap_netip_done(snap_netip_rx_t *rx)
{
    if (!rx->jpeg.len) return 0;
    size_t n = rx->jpeg.len;
    if (snap_jpeg_check(rx->jpeg.p, &n, NULL, NULL) < 0) return 0;
    rx->jpeg.len = n;
    return 1;
}

typedef struct {
    int      fd;
    uint32_t session, seq;
} xm_t;

static int xm_send(xm_t *x, int msgid, const char *json)
{
    size_t jl = strlen(json);
    uint8_t buf[512];
    if (jl + 22 > sizeof buf) return -1;
    netip_hdr_t h = { .version = 0, .session = x->session, .seq = x->seq++, .msgid = (uint16_t)msgid,
                      .len = (uint32_t)(jl + 2) };
    netip_hdr_put(buf, &h);
    memcpy(buf + NETIP_HDR_LEN, json, jl);
    buf[NETIP_HDR_LEN + jl] = '\n';
    buf[NETIP_HDR_LEN + jl + 1] = 0;
    size_t total = jl + 22;
    return send(x->fd, buf, total, MSG_NOSIGNAL) == (ssize_t)total ? 0 : -1;
}

/* Exactly n bytes before the deadline: 0, -1 closed, -2 timed out or stopped. */
static int xm_recv(int fd, uint8_t *b, size_t n, long long deadline, const volatile int *stop)
{
    size_t got = 0;
    while (got < n) {
        if ((stop && *stop) || now_ms() >= deadline) return -2;
        ssize_t r = recv(fd, b + got, n - got, 0);
        if (r > 0) { got += (size_t)r; continue; }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        return -1;
    }
    return 0;
}

/* One message: 0 with *h and a malloc'd body (NUL-terminated), -1, -2. */
static int xm_msg(xm_t *x, netip_hdr_t *h, uint8_t **body, long long deadline, const volatile int *stop)
{
    uint8_t hb[NETIP_HDR_LEN];
    *body = NULL;
    int rc = xm_recv(x->fd, hb, sizeof hb, deadline, stop);
    if (rc < 0) return rc;
    if (netip_hdr_get(hb, h) < 0 || h->len > 1u << 20) return -1;
    uint8_t *b = malloc(h->len + 1);
    if (!b) return -1;
    rc = xm_recv(x->fd, b, h->len, deadline, stop);
    if (rc < 0) {
        free(b);
        return rc;
    }
    b[h->len] = 0;
    *body = b;
    return 0;
}

snap_rc_t snap_netip(const char *url, int timeout_ms, const volatile int *stop, snap_jpeg_t *out, char *err,
                     size_t cap)
{
    char user[64], pass[64], host[128], v[16], req[256];
    int port = NETIP_PORT;
    const char *path;
    memset(out, 0, sizeof *out);
    if (url_split(url, "netip", user, sizeof user, pass, sizeof pass, host, sizeof host, &port, &path) < 0) {
        snprintf(err, cap, "not a netip:// URL");
        return SNAP_EUNSUPPORTED;
    }
    if (!user[0]) snprintf(user, sizeof user, "admin");
    int ch = url_query(path, "ch", v, sizeof v) ? atoi(v) : 0;
    long long deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 8000);
    xm_t x = { .fd = net_connect(host, port, CONNECT_MS, stop, err, cap) };
    if (x.fd < 0) {
        wipe(pass, sizeof pass);
        return SNAP_EFAIL;
    }
    char hash[9];
    md5_sofia(pass, hash);
    wipe(pass, sizeof pass);
    snprintf(req, sizeof req,
             "{\"EncryptType\":\"MD5\",\"LoginType\":\"DVRIP-Web\",\"PassWord\":\"%s\",\"UserName\":\"%s\"}", hash,
             user);
    wipe(hash, sizeof hash);
    snap_rc_t rc = SNAP_EFAIL;
    netip_hdr_t h;
    uint8_t *b = NULL;
    snap_netip_rx_t rx = { 0 };
    if (xm_send(&x, NETIP_LOGIN, req) < 0) {
        snprintf(err, cap, "send failed");
        goto out;
    }
    wipe(req, sizeof req);
    for (;;) {
        int r = xm_msg(&x, &h, &b, deadline, stop);
        if (r < 0) {
            snprintf(err, cap, r == -2 ? "no answer to the login" : "the camera closed the connection");
            goto out;
        }
        if (h.msgid == NETIP_LOGIN + 1) break;
        free(b);
        b = NULL;
    }
    cJSON *doc = cJSON_Parse((char *)b);
    const cJSON *ret = doc ? cJSON_GetObjectItemCaseSensitive(doc, "Ret") : NULL;
    const cJSON *sid = doc ? cJSON_GetObjectItemCaseSensitive(doc, "SessionID") : NULL;
    int lr = cJSON_IsNumber(ret) ? ret->valueint : -1;
    x.session = h.session;
    if (cJSON_IsString(sid)) netip_session_parse(sid->valuestring, &x.session);
    cJSON_Delete(doc);
    free(b);
    b = NULL;
    if (lr != 100 && lr != 515) {
        int auth = lr == 203 || lr == 205 || lr == 206;
        snprintf(err, cap, "login refused (Ret %d)%s", lr, auth ? ": user/password" : "");
        rc = auth ? SNAP_EAUTH : SNAP_EFAIL;
        goto out;
    }
    if (snap_netip_body(req, sizeof req, x.session, ch) < 0 || xm_send(&x, SNAP_NETIP_REQ, req) < 0) {
        snprintf(err, cap, "send failed");
        goto out;
    }
    for (;;) {
        /* Without a count or an end yet, a pause in the messages ends it. */
        long long until = deadline;
        if (rx.chunks && !rx.framed && now_ms() + NETIP_IDLE_MS < deadline) until = now_ms() + NETIP_IDLE_MS;
        int r = xm_msg(&x, &h, &b, until, stop);
        if (r == -2 && until < deadline && !(stop && *stop)) {
            if (snap_netip_done(&rx)) break;
            snprintf(err, cap, "the camera's JPEG is broken (%zu bytes)", rx.jpeg.len);
            goto out;
        }
        if (r < 0) {
            if (r == -1 && snap_netip_done(&rx)) break;     /* sent, then hung up */
            snprintf(err, cap, r == -2 ? "no snapshot from the camera in time" : "the camera closed the connection");
            /* A camera that does not know the message says nothing. */
            if (r == -2 && !rx.chunks) rc = SNAP_EUNSUPPORTED;
            goto out;
        }
        if (h.msgid != SNAP_NETIP_RSP) {
            free(b);
            b = NULL;
            continue;
        }
        int t = snap_netip_take(&rx, &h, b, h.len, err, cap);
        free(b);
        b = NULL;
        if (t < 0) {
            rc = strstr(err, "refused") ? SNAP_EUNSUPPORTED : SNAP_EFAIL;
            goto out;
        }
        if (t > 0) break;
    }
    *out = rx.jpeg;
    rx.jpeg = (snap_jpeg_t){ 0 };
    rc = SNAP_OK;
    xm_send(&x, NETIP_LOGOUT, "{\"Name\":\"\"}");
out:
    free(b);
    snap_jpeg_free(&rx.jpeg);
    close(x.fd);
    return rc;
}

/* ---- HTTP --------------------------------------------------------------------------- */

static const char *ci_find(const char *hay, size_t n, const char *needle)
{
    size_t nl = strlen(needle);
    for (size_t i = 0; i + nl <= n; ++i)
        if (!strncasecmp(hay + i, needle, nl)) return hay + i;
    return NULL;
}

static void auth_param(const char *h, const char *key, char *out, size_t cap)
{
    out[0] = 0;
    size_t kl = strlen(key);
    for (const char *p = h; (p = strcasestr(p, key)) != NULL; p += kl) {
        if (p != h && p[-1] != ' ' && p[-1] != ',' && p[-1] != '\t') continue;
        const char *v = p + kl;
        while (*v == ' ') ++v;
        if (*v != '=') continue;
        ++v;
        while (*v == ' ') ++v;
        size_t n;
        if (*v == '"') n = strcspn(++v, "\"");
        else n = strcspn(v, ", \r\n");
        if (n >= cap) n = cap - 1;
        memcpy(out, v, n);
        out[n] = 0;
        return;
    }
}

int snap_digest(const char *www, const char *user, const char *pass, const char *method, const char *uri,
                const char *cnonce, char *out, size_t cap)
{
    char realm[128], nonce[192], opaque[128], qop[64], alg[32];
    if (strncasecmp(www, "Digest", 6) != 0) return -1;
    auth_param(www, "realm", realm, sizeof realm);
    auth_param(www, "nonce", nonce, sizeof nonce);
    auth_param(www, "opaque", opaque, sizeof opaque);
    auth_param(www, "qop", qop, sizeof qop);
    auth_param(www, "algorithm", alg, sizeof alg);
    if (!nonce[0] || (alg[0] && strcasecmp(alg, "MD5") != 0)) return -1;
    /* qop is a list ("auth,auth-int"); only "auth" is answered (no body
     * to hash), and without one the challenge is RFC 2069's. */
    int use_qop = 0;
    for (const char *t = qop; *t;) {
        while (*t == ' ' || *t == ',') ++t;
        size_t tl = strcspn(t, ", ");
        if (tl == 4 && !strncasecmp(t, "auth", 4)) use_qop = 1;
        t += tl;
    }
    if (qop[0] && !use_qop) return -1;
    char buf[640], ha1[33], ha2[33], resp[33];
    snprintf(buf, sizeof buf, "%s:%s:%s", user, realm, pass);
    md5_hex(buf, strlen(buf), ha1);
    wipe(buf, sizeof buf);
    snprintf(buf, sizeof buf, "%s:%s", method, uri);
    md5_hex(buf, strlen(buf), ha2);
    if (use_qop) snprintf(buf, sizeof buf, "%s:%s:00000001:%s:auth:%s", ha1, nonce, cnonce, ha2);
    else snprintf(buf, sizeof buf, "%s:%s:%s", ha1, nonce, ha2);
    md5_hex(buf, strlen(buf), resp);
    wipe(ha1, sizeof ha1);
    int n = snprintf(out, cap, "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\"%s%s%s%s%s%s",
                     user, realm, nonce, uri, resp, alg[0] ? ", algorithm=MD5" : "", opaque[0] ? ", opaque=\"" : "",
                     opaque, opaque[0] ? "\"" : "", use_qop ? ", qop=auth, nc=00000001, cnonce=\"" : "",
                     use_qop ? cnonce : "");
    if (n > 0 && (size_t)n < cap && use_qop) n += snprintf(out + n, cap - (size_t)n, "\"");
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

static int dechunk(unsigned char *body, size_t *len)
{
    size_t in = 0, o = 0, n = *len;
    for (;;) {
        char *end;
        if (in >= n) return -1;
        char hex[20];
        size_t hl = 0;
        while (in + hl < n && hl < sizeof hex - 1 && body[in + hl] != '\r' && body[in + hl] != '\n') {
            hex[hl] = (char)body[in + hl];
            ++hl;
        }
        hex[hl] = 0;
        unsigned long sz = strtoul(hex, &end, 16);
        if (end == hex) return -1;
        unsigned char *nl = memchr(body + in, '\n', n - in);
        if (!nl) return -1;
        in = (size_t)(nl + 1 - body);
        if (sz == 0) break;
        if (sz > n - in) return -1;
        memmove(body + o, body + in, sz);
        o += sz;
        in += sz;
        if (in < n && body[in] == '\r') ++in;
        if (in < n && body[in] == '\n') ++in;
    }
    *len = o;
    return 0;
}

typedef struct {
    int           status;
    char          www[384];         /* the first Digest challenge, else the Basic one */
    char          ctype[64];
    snap_jpeg_t   body;
} http_ans_t;

static int http_get_once(const char *host, int port, const char *path, const char *auth, long long deadline,
                         const volatile int *stop, http_ans_t *a, char *err, size_t cap)
{
    memset(a, 0, sizeof *a);
    int fd = net_connect(host, port, CONNECT_MS, stop, err, cap);
    if (fd < 0) return -1;
    char head[1536];
    int hl = snprintf(head, sizeof head,
                      "GET %s HTTP/1.1\r\nHost: %s:%d\r\nAccept: image/jpeg, */*\r\nUser-Agent: vigil\r\n"
                      "Connection: close\r\n%s%s%s\r\n",
                      path, host, port, auth ? "Authorization: " : "", auth ? auth : "", auth ? "\r\n" : "");
    if (hl < 0 || (size_t)hl >= sizeof head || send(fd, head, (size_t)hl, MSG_NOSIGNAL) != hl) {
        wipe(head, sizeof head);
        snprintf(err, cap, "send failed");
        close(fd);
        return -1;
    }
    wipe(head, sizeof head);
    snap_jpeg_t raw = { 0 };
    size_t hdr_end = 0;
    long long want = -1;
    unsigned char buf[16384];
    int rc = 0;
    for (;;) {
        if ((stop && *stop) || now_ms() >= deadline) {
            snprintf(err, cap, "no answer in time");
            rc = -1;
            break;
        }
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        if (n <= 0) break;
        if (snap_jpeg_put(&raw, buf, (size_t)n) < 0) {
            snprintf(err, cap, "the answer is larger than %u MiB", SNAP_JPEG_MAX >> 20);
            rc = -1;
            break;
        }
        if (!hdr_end) {
            const char *e = memmem(raw.p, raw.len, "\r\n\r\n", 4);
            if (e) {
                hdr_end = (size_t)((const unsigned char *)e + 4 - raw.p);
                const char *cl = ci_find((char *)raw.p, hdr_end, "\nContent-Length:");
                if (cl) want = (long long)hdr_end + atoll(cl + 16);
            }
        }
        if (want >= 0 && (long long)raw.len >= want) break;
    }
    close(fd);
    if (rc < 0) {
        snap_jpeg_free(&raw);
        return -1;
    }
    if (!hdr_end || sscanf((char *)raw.p, "HTTP/%*d.%*d %d", &a->status) != 1) {
        snprintf(err, cap, raw.len ? "not an HTTP answer" : "connection closed without an answer");
        snap_jpeg_free(&raw);
        return -1;
    }
    const char *hs = (char *)raw.p;
    for (const char *p = hs; (p = ci_find(p, hdr_end - (size_t)(p - hs), "\nWWW-Authenticate:")) != NULL;) {
        p += 18;
        while (*p == ' ') ++p;
        int digest = !strncasecmp(p, "Digest", 6);
        if (digest || !a->www[0]) {
            size_t n = strcspn(p, "\r\n");
            if (n >= sizeof a->www) n = sizeof a->www - 1;
            memcpy(a->www, p, n);
            a->www[n] = 0;
        }
        if (digest) break;
    }
    const char *ct = ci_find(hs, hdr_end, "\nContent-Type:");
    if (ct) {
        ct += 14;
        while (*ct == ' ') ++ct;
        size_t n = strcspn(ct, ";\r\n");
        if (n >= sizeof a->ctype) n = sizeof a->ctype - 1;
        memcpy(a->ctype, ct, n);
        a->ctype[n] = 0;
    }
    int chunked = ci_find(hs, hdr_end, "\nTransfer-Encoding: chunked") != NULL;
    size_t bl = raw.len - hdr_end;
    memmove(raw.p, raw.p + hdr_end, bl);
    raw.len = bl;
    if (chunked && dechunk(raw.p, &raw.len) < 0) {
        snprintf(err, cap, "a broken chunked answer");
        snap_jpeg_free(&raw);
        return -1;
    }
    a->body = raw;
    return 0;
}

static void random_hex(char *out, size_t n)
{
    unsigned char b[16];
    /* Weak is enough (vg_random.h): a client's cnonce to a camera only has
     * to differ. */
    vg_random(b, sizeof b, VG_RAND_WEAK);
    for (size_t i = 0; i < n / 2 && i < sizeof b; ++i) snprintf(out + 2 * i, 3, "%02x", b[i]);
}

int snap_url_creds(const char *url, char *user, size_t ul, char *pass, size_t pl, char *host, size_t hl)
{
    const char *p = strstr(url, "://");
    if (!p) return -1;
    p += 3;
    size_t auth = strcspn(p, "/?#");
    const char *at = NULL;
    for (const char *q = p; q < p + auth; ++q)
        if (*q == '@') at = q;
    if (ul) user[0] = 0;
    if (pl) pass[0] = 0;
    if (at) {
        const char *c = memchr(p, ':', (size_t)(at - p));
        size_t n = (size_t)((c ? c : at) - p);
        if (n >= ul) return -1;
        memcpy(user, p, n);
        user[n] = 0;
        if (c) {
            n = (size_t)(at - c - 1);
            if (n >= pl) return -1;
            memcpy(pass, c + 1, n);
            pass[n] = 0;
        }
        p = at + 1;
        auth = strcspn(p, "/?#");
    }
    size_t n = strcspn(p, ":/?#");
    if (!n || n >= hl) return -1;
    memcpy(host, p, n);
    host[n] = 0;
    return 0;
}

snap_rc_t snap_http_get(const char *url, const char *user, const char *pass, int timeout_ms,
                        const volatile int *stop, snap_jpeg_t *out, char *err, size_t cap)
{
    memset(out, 0, sizeof *out);
    if (strncasecmp(url, "http://", 7) != 0) {
        snprintf(err, cap, "not an http:// address");
        return SNAP_EUNSUPPORTED;
    }
    char u[64], pw[64], host[128], path[768];
    if (snap_url_creds(url, u, sizeof u, pw, sizeof pw, host, sizeof host) < 0) {
        snprintf(err, cap, "a bad snapshot address");
        return SNAP_EUNSUPPORTED;
    }
    if (!u[0] && user) snprintf(u, sizeof u, "%s", user);
    if (!pw[0] && pass) snprintf(pw, sizeof pw, "%s", pass);
    const char *h = url + 7, *at = NULL;
    size_t auth = strcspn(h, "/?#");
    for (const char *q = h; q < h + auth; ++q)
        if (*q == '@') at = q;
    if (at) h = at + 1;
    size_t hn = strcspn(h, "/?#");
    int port = 80;
    const char *colon = memchr(h, ':', hn);
    if (colon) port = atoi(colon + 1);
    const char *p = h + hn;
    int pn = snprintf(path, sizeof path, "%s%s", *p == '/' ? "" : "/", p);
    if (pn < 0 || (size_t)pn >= sizeof path || port <= 0 || port > 65535) {
        wipe(pw, sizeof pw);
        snprintf(err, cap, "a bad snapshot address");
        return SNAP_EUNSUPPORTED;
    }
    long long deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 8000);
    http_ans_t a;
    char authz[1024];
    snap_rc_t rc = SNAP_EFAIL;
    if (http_get_once(host, port, path, NULL, deadline, stop, &a, err, cap) < 0) goto out;
    if (a.status == 401 && a.www[0]) {
        int ok = -1;
        if (!strncasecmp(a.www, "Digest", 6)) {
            char cn[17];
            random_hex(cn, 16);
            ok = snap_digest(a.www, u[0] ? u : "admin", pw, "GET", path, cn, authz, sizeof authz);
        } else if (!strncasecmp(a.www, "Basic", 5)) {
            char cred[160], b64[224];
            int n = snprintf(cred, sizeof cred, "%s:%s", u[0] ? u : "admin", pw);
            ok = n > 0 && (size_t)n < sizeof cred && base64_encode(cred, (size_t)n, b64, sizeof b64) >= 0 ? 0 : -1;
            if (!ok) snprintf(authz, sizeof authz, "Basic %s", b64);
            wipe(cred, sizeof cred);
            wipe(b64, sizeof b64);
        }
        snap_jpeg_free(&a.body);
        if (ok < 0) {
            snprintf(err, cap, "the camera asks for a login vigil cannot give (%.40s)", a.www);
            rc = SNAP_EAUTH;
            goto out;
        }
        int r = http_get_once(host, port, path, authz, deadline, stop, &a, err, cap);
        wipe(authz, sizeof authz);
        if (r < 0) goto out;
    }
    if (a.status == 401 || a.status == 403) {
        snprintf(err, cap, "the camera refused the user or the password (HTTP %d)", a.status);
        rc = SNAP_EAUTH;
    } else if (a.status == 404 || a.status == 501) {
        snprintf(err, cap, "no snapshot there (HTTP %d)", a.status);
        rc = SNAP_EUNSUPPORTED;
    } else if (a.status != 200) {
        snprintf(err, cap, "HTTP %d", a.status);
    } else {
        size_t n = a.body.len;
        if (snap_jpeg_check(a.body.p, &n, NULL, NULL) < 0) {
            snprintf(err, cap, "the answer is not a JPEG (%s, %zu bytes)", a.ctype[0] ? a.ctype : "no type", a.body.len);
        } else {
            a.body.len = n;
            *out = a.body;
            a.body = (snap_jpeg_t){ 0 };
            rc = SNAP_OK;
        }
    }
    snap_jpeg_free(&a.body);
out:
    wipe(pw, sizeof pw);
    return rc;
}

/* ---- ONVIF ------------------------------------------------------------------------------ */

static snap_rc_t from_disc(disc_rc_t rc)
{
    return rc == DISC_OK ? SNAP_OK : rc == DISC_EAUTH ? SNAP_EAUTH : rc == DISC_EUNSUPPORTED ? SNAP_EUNSUPPORTED
                                                                                          : SNAP_EFAIL;
}

/* GetSnapshotUri of the device whose service `dev` answered. */
static snap_rc_t snapshot_uri(disc_onvif_t *dev, char *uri, size_t ucap, char *err, size_t cap)
{
    disc_http_t r;
    disc_onvif_t m = *dev;
    disc_rc_t rc = disc_onvif_call(dev, NULL, NULL,
                                   "<tds:GetCapabilities><tds:Category>Media</tds:Category></tds:GetCapabilities>",
                                   ONVIF_MS, &r, "GetCapabilities", err, cap);
    if (rc != DISC_OK) return from_disc(rc);
    char xa[192];
    const char *mp = NULL;
    for (const char *q = r.body; (q = strstr(q, "Media")) != NULL; ++q)
        if (q > r.body && (q[-1] == ':' || q[-1] == '<')) { mp = q; break; }
    if (mp && disc_xml_text(mp, r.len - (size_t)(mp - r.body), "XAddr", xa, sizeof xa, NULL) == 0 && xa[0]) {
        /* reached where the device service was, whatever host it names */
        if (disc_onvif_init(&m, xa, dev->host, dev->user, dev->pass) < 0) m = *dev;
        m.clock_offset = dev->clock_offset;
        m.stop = dev->stop;
    }
    free(r.body);
    if (!mp) {
        snprintf(err, cap, "the camera has no media service");
        return SNAP_EUNSUPPORTED;
    }
    rc = disc_onvif_call(&m, NULL, NULL, "<trt:GetProfiles/>", ONVIF_MS, &r, "GetProfiles", err, cap);
    if (rc != DISC_OK) return from_disc(rc);
    char tok[64];
    int got = disc_xml_attr(r.body, r.len, "Profiles", "token", tok, sizeof tok, NULL) == 0 && tok[0];
    free(r.body);
    if (!got) {
        snprintf(err, cap, "the camera lists no media profile");
        return SNAP_EUNSUPPORTED;
    }
    char body[256], te[160];
    onvif_xml_escape(tok, te, sizeof te);
    snprintf(body, sizeof body, "<trt:GetSnapshotUri><trt:ProfileToken>%s</trt:ProfileToken></trt:GetSnapshotUri>", te);
    rc = disc_onvif_call(&m, NULL, NULL, body, ONVIF_MS, &r, "GetSnapshotUri", err, cap);
    if (rc != DISC_OK) return from_disc(rc);
    int ok = disc_xml_text(r.body, r.len, "Uri", uri, ucap, NULL) == 0 && !strncasecmp(uri, "http", 4);
    free(r.body);
    if (!ok) {
        snprintf(err, cap, "GetSnapshotUri gave no http address");
        return SNAP_EUNSUPPORTED;
    }
    return SNAP_OK;
}

snap_rc_t snap_onvif_uri(const char *cam_url, const char *onvif_url, int self_port, const volatile int *stop,
                         char *uri, size_t ucap, char *err, size_t cap)
{
    char user[64], pass[64], host[128];
    if (ucap) uri[0] = 0;
    if (snap_url_creds(cam_url, user, sizeof user, pass, sizeof pass, host, sizeof host) < 0) {
        snprintf(err, cap, "the camera's URL has no host");
        return SNAP_EUNSUPPORTED;
    }
    char cand[4][192];
    int n = 0;
    if (onvif_url && onvif_url[0]) {
        snprintf(cand[n++], sizeof cand[0], "%s", onvif_url);
    } else {
        char x[160];
        if (disc_xaddr_of(host, x, sizeof x) == 0 && !strncasecmp(x, "http://", 7))
            snprintf(cand[n++], sizeof cand[0], "%s", x);
        int local = !strncmp(host, "127.", 4) || !strcmp(host, "localhost");
        if (!(local && self_port == 80)) snprintf(cand[n++], sizeof cand[0], "http://%s/onvif/device_service", host);
        if (!(local && self_port == 8899))
            snprintf(cand[n++], sizeof cand[0], "http://%s:8899/onvif/device_service", host);
    }
    snap_rc_t rc = SNAP_EUNSUPPORTED;
    snprintf(err, cap, "no ONVIF address to try");
    for (int i = 0; i < n && !(stop && *stop); ++i) {
        disc_onvif_t dev;
        if (disc_onvif_init(&dev, cand[i], onvif_url && onvif_url[0] ? NULL : host, user, pass) < 0) continue;
        dev.stop = stop;
        disc_onvif_clock(&dev);
        rc = snapshot_uri(&dev, uri, ucap, err, cap);
        if (rc == SNAP_OK || rc == SNAP_EAUTH) break;
    }
    wipe(pass, sizeof pass);
    return rc;
}
