/* SPDX-License-Identifier: GPL-3.0-or-later */
/* disc_ops.c — ONVIF, Dahua RPC2 and NETIP calls on one device (see
 * disc_ops.h). */
#define _DEFAULT_SOURCE
#include <ctype.h>
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
#include "md5.h"
#include "net.h"
#include "rtsp_client.h"
#include "sha1.h"
#include "vg_random.h"

#define HTTP_MAX     (256u << 10)
#define CALL_MS      5000            /* one HTTP exchange */
#define CONNECT_MS   2000

const char *disc_rc_name(disc_rc_t rc)
{
    static const char *names[] = { "ok", "auth", "unsupported", "timeout", "unreachable", "protocol", "invalid",
                                   "codec" };
    return (unsigned)rc < sizeof names / sizeof names[0] ? names[rc] : "error";
}

static void wipe(void *p, size_t n)
{
    volatile unsigned char *w = p;
    while (n--) *w++ = 0;
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- HTTP ----------------------------------------------------------------------- */

static const char *ci_find(const char *hay, size_t n, const char *needle)
{
    size_t nl = strlen(needle);
    for (size_t i = 0; i + nl <= n; ++i)
        if (!strncasecmp(hay + i, needle, nl)) return hay + i;
    return NULL;
}

/* De-chunk body[0..*len) in place; -1 if malformed. */
static int dechunk(char *body, size_t *len)
{
    size_t in = 0, out = 0, n = *len;
    for (;;) {
        char *end;
        if (in >= n) return -1;
        unsigned long sz = strtoul(body + in, &end, 16);
        if (end == body + in) return -1;
        char *nl = memchr(end, '\n', n - (size_t)(end - body));
        if (!nl) return -1;
        in = (size_t)(nl + 1 - body);
        if (sz == 0) break;
        if (sz > n - in) return -1;
        memmove(body + out, body + in, sz);
        out += sz;
        in += sz;
        if (in < n && body[in] == '\r') ++in;
        if (in < n && body[in] == '\n') ++in;
    }
    body[out] = 0;
    *len = out;
    return 0;
}

static int http_post(const char *host, int port, const char *path, const char *ctype, const char *auth,
                     const char *body, int timeout_ms, const volatile int *stop, disc_http_t *r, char *err,
                     size_t errcap)
{
    memset(r, 0, sizeof *r);
    int fd = net_connect(host, port, CONNECT_MS, stop, err, errcap);
    if (fd < 0) return -1;
    size_t bl = strlen(body);
    char head[1024];
    int hl = snprintf(head, sizeof head,
                      "POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                      "Connection: close\r\n%s%s%s\r\n",
                      path, host, port, ctype, bl, auth ? "Authorization: " : "", auth ? auth : "",
                      auth ? "\r\n" : "");
    if (hl < 0 || (size_t)hl >= sizeof head || send(fd, head, (size_t)hl, MSG_NOSIGNAL) != hl ||
        send(fd, body, bl, MSG_NOSIGNAL) != (ssize_t)bl) {
        snprintf(err, errcap, "send failed");
        close(fd);
        return -1;
    }
    size_t cap = 16384, got = 0;
    char *buf = malloc(cap + 1);
    long long deadline = now_ms() + timeout_ms;
    long long want = -1;                         /* header + Content-Length, once known */
    size_t hdr_end = 0;
    while (buf) {
        if (stop && *stop) {
            snprintf(err, errcap, "stopped");
            free(buf);
            close(fd);
            return -1;
        }
        if (now_ms() >= deadline) {
            snprintf(err, errcap, "no answer within %d s", timeout_ms / 1000);
            free(buf);
            close(fd);
            return -1;
        }
        if (got == cap) {
            if (cap >= HTTP_MAX) break;
            char *nb = realloc(buf, cap * 2 + 1);
            if (!nb) break;
            buf = nb;
            cap *= 2;
        }
        ssize_t n = recv(fd, buf + got, cap - got, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        if (n <= 0) break;
        got += (size_t)n;
        buf[got] = 0;
        if (!hdr_end) {
            char *e = strstr(buf, "\r\n\r\n");
            if (e) {
                hdr_end = (size_t)(e + 4 - buf);
                const char *cl = ci_find(buf, hdr_end, "\nContent-Length:");
                if (cl) want = (long long)hdr_end + atoll(cl + 16);
            }
        }
        if (want >= 0 && (long long)got >= want) break;
    }
    close(fd);
    if (!buf) {
        snprintf(err, errcap, "out of memory");
        return -1;
    }
    buf[got] = 0;
    if (!hdr_end || sscanf(buf, "HTTP/%*d.%*d %d", &r->status) != 1) {
        snprintf(err, errcap, got ? "not an HTTP answer" : "connection closed without an answer");
        free(buf);
        return -1;
    }
    /* A Digest challenge, if any (Basic is not offered to a camera's
     * password in clear). */
    for (const char *p = buf; (p = ci_find(p, hdr_end - (size_t)(p - buf), "\nWWW-Authenticate:")) != NULL;) {
        p += 18;
        while (*p == ' ') ++p;
        if (!strncasecmp(p, "Digest", 6)) {
            size_t n = strcspn(p, "\r\n");
            if (n >= sizeof r->www_auth) n = sizeof r->www_auth - 1;
            memcpy(r->www_auth, p, n);
            r->www_auth[n] = 0;
            break;
        }
    }
    int chunked = ci_find(buf, hdr_end, "\nTransfer-Encoding: chunked") != NULL;
    r->len = got - hdr_end;
    memmove(buf, buf + hdr_end, r->len + 1);
    r->body = buf;
    if (chunked && dechunk(r->body, &r->len) < 0) {
        snprintf(err, errcap, "bad chunked answer");
        free(buf);
        r->body = NULL;
        return -1;
    }
    return 0;
}

int disc_http_post(const char *host, int port, const char *path, const char *ctype, const char *auth,
                   const char *body, int timeout_ms, disc_http_t *r, char *err, size_t errcap)
{
    return http_post(host, port, path, ctype, auth, body, timeout_ms, NULL, r, err, errcap);
}

/* key="value" or key=value from a Digest challenge. */
static void auth_param(const char *h, const char *key, char *out, size_t cap)
{
    out[0] = 0;
    size_t kl = strlen(key);
    for (const char *p = h; (p = strstr(p, key)) != NULL; p += kl) {
        if (p != h && p[-1] != ' ' && p[-1] != ',') continue;
        if (p[kl] != '=') continue;
        const char *v = p + kl + 1;
        size_t n;
        if (*v == '"') n = strcspn(++v, "\"");
        else n = strcspn(v, ", ");
        if (n >= cap) n = cap - 1;
        memcpy(out, v, n);
        out[n] = 0;
        return;
    }
}

/* The nonces vigil sends a camera as a client (Digest's cnonce,
 * WS-Security's Nonce): the camera checks them, they only have to differ,
 * and talking to cameras does not wait for the kernel's random numbers
 * after power-on. Weak (vg_random.h). */
static void random_bytes(unsigned char *b, size_t n) { vg_random(b, n, VG_RAND_WEAK); }

static void digest_header(const char *www, const char *user, const char *pass, const char *path, char *out,
                          size_t cap)
{
    char realm[128], nonce[192], opaque[128], qop[64], resp[33], cn[17];
    unsigned char rb[8];
    auth_param(www, "realm", realm, sizeof realm);
    auth_param(www, "nonce", nonce, sizeof nonce);
    auth_param(www, "opaque", opaque, sizeof opaque);
    auth_param(www, "qop", qop, sizeof qop);
    random_bytes(rb, sizeof rb);
    for (int i = 0; i < 8; ++i) snprintf(cn + 2 * i, 3, "%02x", rb[i]);
    int q = strstr(qop, "auth") != NULL;
    rtsp_digest_response(user, realm, pass, "POST", path, nonce, "00000001", cn, q ? "auth" : NULL, resp);
    snprintf(out, cap,
             "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\", algorithm=MD5%s%s%s%s%s%s",
             user, realm, nonce, path, resp, q ? ", qop=auth, nc=00000001, cnonce=\"" : "", q ? cn : "",
             q ? "\"" : "", opaque[0] ? ", opaque=\"" : "", opaque, opaque[0] ? "\"" : "");
}

/* ---- ONVIF ------------------------------------------------------------------------ */

static void xml_escape(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (; *s && o + 7 < cap; ++s) {
        const char *e = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '"' ? "&quot;" : NULL;
        if (e) {
            memcpy(out + o, e, strlen(e));
            o += strlen(e);
        } else {
            out[o++] = *s;
        }
    }
    out[o] = 0;
}

int disc_wsse_header(const char *user, const char *pass, const char *created, const unsigned char nonce[16],
                     char *out, size_t cap)
{
    size_t pl = strlen(pass), cl = strlen(created);
    unsigned char *buf = malloc(16 + cl + pl), dig[20];
    if (!buf) return -1;
    memcpy(buf, nonce, 16);
    memcpy(buf + 16, created, cl);
    memcpy(buf + 16 + cl, pass, pl);
    sha1_raw(buf, 16 + cl + pl, dig);
    wipe(buf, 16 + cl + pl);
    free(buf);
    char d64[32], n64[32], u[192];
    base64_encode(dig, 20, d64, sizeof d64);
    base64_encode(nonce, 16, n64, sizeof n64);
    xml_escape(user, u, sizeof u);
    int n = snprintf(out, cap,
        "<Security s:mustUnderstand=\"1\" xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\">"
        "<UsernameToken><Username>%s</Username>"
        "<Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">%s</Password>"
        "<Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-soap-message-security-1.0#Base64Binary\">%s</Nonce>"
        "<Created xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">%s</Created>"
        "</UsernameToken></Security>", u, d64, n64, created);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

typedef disc_onvif_t onvif_t;

int disc_onvif_init(disc_onvif_t *o, const char *xaddr, const char *ip, const char *user, const char *pass)
{
    memset(o, 0, sizeof *o);
    o->user = user && *user ? user : "admin";
    o->pass = pass ? pass : "";
    if (strncasecmp(xaddr, "http://", 7) != 0) return -1;
    const char *h = xaddr + 7;
    size_t n = strcspn(h, ":/");
    if (!n || n >= sizeof o->host) return -1;
    memcpy(o->host, h, n);
    o->host[n] = 0;
    o->port = h[n] == ':' ? atoi(h + n + 1) : 80;
    const char *p = strchr(h, '/');
    snprintf(o->path, sizeof o->path, "%s", p ? p : "/onvif/device_service");
    /* A service address naming another host (a NAT'd or stale address in
     * the device's own settings) is reached at the address it answered
     * discovery from. */
    if (ip && ip[0] && strcmp(o->host, ip) != 0) snprintf(o->host, sizeof o->host, "%s", ip);
    return o->port > 0 && o->port < 65536 ? 0 : -1;
}

static int onvif_init(onvif_t *o, const char *xaddr, const disc_device_t *d, const char *user, const char *pass)
{
    return disc_onvif_init(o, xaddr, d ? d->ip : NULL, user, pass);
}

static disc_rc_t soap_fail(const disc_http_t *r, const char *what, char *err, size_t errcap)
{
    char reason[160] = "";
    if (r->body) {
        disc_xml_text(r->body, r->len, "Text", reason, sizeof reason, NULL);
        if (!reason[0]) disc_xml_text(r->body, r->len, "faultstring", reason, sizeof reason, NULL);
    }
    if (r->status != 401 && (!r->body || !strstr(r->body, "Envelope"))) {
        /* A web server, but no SOAP: something else on that port. */
        snprintf(err, errcap, "%s: not an ONVIF device (HTTP %d)", what, r->status);
        return DISC_EUNSUPPORTED;
    }
    if (r->status == 401 || (r->body && (strstr(r->body, "NotAuthorized") || strstr(r->body, "FailedAuthentication")))) {
        snprintf(err, errcap, "%s: not authorized", what);
        return DISC_EAUTH;
    }
    if (reason[0]) snprintf(err, errcap, "%s: %s", what, reason);
    else snprintf(err, errcap, "%s: HTTP %d", what, r->status);
    if (r->body && (strstr(r->body, "ActionNotSupported") || strstr(r->body, "NoSuchService"))) return DISC_EUNSUPPORTED;
    return DISC_EPROTO;
}

/* One SOAP call to `path` on the device; 0 with r->body (free it). */
static disc_rc_t soap_ex(onvif_t *o, const char *path, const char *hdr_xml, const char *body_xml, int secure,
                         int timeout_ms, disc_http_t *r, const char *what, char *err, size_t errcap)
{
    char sec[1536] = "";
    if (secure) {
        unsigned char nonce[16];
        char created[32];
        random_bytes(nonce, sizeof nonce);
        time_t t = time(NULL) + o->clock_offset;
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(created, sizeof created, "%Y-%m-%dT%H:%M:%SZ", &tm);
        if (disc_wsse_header(o->user, o->pass, created, nonce, sec, sizeof sec) < 0) {
            snprintf(err, errcap, "%s: credentials too long", what);
            return DISC_EINVAL;
        }
    }
    if (!hdr_xml) hdr_xml = "";
    size_t cap = strlen(sec) + strlen(hdr_xml) + strlen(body_xml) + 768;
    char *env = malloc(cap);
    if (!env) {
        snprintf(err, errcap, "out of memory");
        return DISC_EPROTO;
    }
    snprintf(env, cap,
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
             "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
             " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\""
             " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\""
             " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\""
             " xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\""
             " xmlns:wsa=\"http://www.w3.org/2005/08/addressing\""
             " xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
             "<s:Header>%s%s</s:Header><s:Body>%s</s:Body></s:Envelope>", hdr_xml, sec, body_xml);
    wipe(sec, sizeof sec);
    const char *ct = "application/soap+xml; charset=utf-8";
    char e2[128];
    int rc = http_post(o->host, o->port, path, ct, NULL, env, timeout_ms, o->stop, r, e2, sizeof e2);
    if (rc == 0 && r->status == 401 && r->www_auth[0] && secure) {
        /* HTTP Digest as well as (or instead of) WS-Security. */
        char auth[1024];
        digest_header(r->www_auth, o->user, o->pass, path, auth, sizeof auth);
        free(r->body);
        rc = http_post(o->host, o->port, path, ct, auth, env, timeout_ms, o->stop, r, e2, sizeof e2);
        wipe(auth, sizeof auth);
    }
    wipe(env, cap);
    free(env);
    if (rc < 0) {
        snprintf(err, errcap, "%s: %s", what, e2);
        /* "connect: ..." and "cannot resolve": nobody there; otherwise it
         * answered with nothing usable, or not in time. */
        if (!strncmp(e2, "connect:", 8) || !strncmp(e2, "cannot resolve", 14)) return DISC_EUNREACHABLE;
        return strstr(e2, "no answer") ? DISC_ETIMEOUT : DISC_EPROTO;
    }
    if (r->status != 200 || !r->body || !strstr(r->body, "Envelope")) {
        disc_rc_t f = soap_fail(r, what, err, errcap);
        free(r->body);
        r->body = NULL;
        return f;
    }
    return DISC_OK;
}

static disc_rc_t soap(onvif_t *o, const char *path, const char *body_xml, int secure, disc_http_t *r,
                      const char *what, char *err, size_t errcap)
{
    return soap_ex(o, path, NULL, body_xml, secure, CALL_MS, r, what, err, errcap);
}

disc_rc_t disc_onvif_call(disc_onvif_t *o, const char *path, const char *hdr_xml, const char *body_xml,
                          int timeout_ms, disc_http_t *r, const char *what, char *err, size_t errcap)
{
    return soap_ex(o, path ? path : o->path, hdr_xml, body_xml, 1, timeout_ms > 0 ? timeout_ms : CALL_MS, r, what,
                   err, errcap);
}

static void onvif_clock(onvif_t *o);
void disc_onvif_clock(disc_onvif_t *o) { onvif_clock(o); }

/* The device's clock: WS-Security's Created must be close to it. */
static void onvif_clock(onvif_t *o)
{
    disc_http_t r;
    char err[64];
    if (soap(o, o->path, "<tds:GetSystemDateAndTime/>", 0, &r, "GetSystemDateAndTime", err, sizeof err) != DISC_OK)
        return;
    char v[16];
    struct tm tm = { 0 };
    const char *b = r.body;
    const char *u = strstr(b, "UTCDateTime");
    if (u) {
        size_t off = (size_t)(u - b), n = r.len - off;
        int ok = 1;
        static const char *keys[] = { "Year", "Month", "Day", "Hour", "Minute", "Second" };
        int vals[6];
        for (int i = 0; i < 6 && ok; ++i) {
            ok = disc_xml_text(u, n, keys[i], v, sizeof v, NULL) == 0;
            vals[i] = ok ? atoi(v) : 0;
        }
        if (ok && vals[0] > 2000 && vals[1] >= 1 && vals[1] <= 12) {
            tm.tm_year = vals[0] - 1900;
            tm.tm_mon = vals[1] - 1;
            tm.tm_mday = vals[2];
            tm.tm_hour = vals[3];
            tm.tm_min = vals[4];
            tm.tm_sec = vals[5];
            time_t dev = timegm(&tm);
            if (dev > 0) o->clock_offset = (long)(dev - time(NULL));
        }
    }
    free(r.body);
}

int disc_url_with_creds(const char *uri, const char *user, const char *pass, char *out, size_t cap)
{
    const char *p = strstr(uri, "://");
    if (!p || !disc_cred_ok(user, 1) || !disc_cred_ok(pass, 0)) return -1;
    p += 3;
    size_t auth = strcspn(p, "/?#");
    const char *at = NULL;
    for (const char *q = p; q < p + auth; ++q)
        if (*q == '@') at = q;
    const char *hostpart = at ? at + 1 : p;
    int n = snprintf(out, cap, "%.*s%s:%s@%s", (int)(p - uri), uri, user, pass, hostpart);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

disc_rc_t disc_onvif_stream_uris(const disc_device_t *d, const char *user, const char *pass, int channel,
                                 char *url, size_t ucap, char *sub, size_t scap, char *err, size_t errcap)
{
    onvif_t o, m;
    if (ucap) url[0] = 0;
    if (scap) sub[0] = 0;
    if (!d->xaddr[0] || onvif_init(&o, d->xaddr, d, user, pass) < 0) {
        snprintf(err, errcap, "no ONVIF service address");
        return DISC_EUNSUPPORTED;
    }
    if (!disc_cred_ok(o.user, 1) || !disc_cred_ok(o.pass, 0)) {
        snprintf(err, errcap, "the user or password has characters a camera URL cannot carry");
        return DISC_EINVAL;
    }
    onvif_clock(&o);
    disc_http_t r;
    disc_rc_t rc = soap(&o, o.path, "<tds:GetCapabilities><tds:Category>Media</tds:Category></tds:GetCapabilities>",
                        1, &r, "GetCapabilities", err, errcap);
    if (rc != DISC_OK) return rc;
    m = o;
    char xa[192];
    /* Capabilities/Media/XAddr: the first XAddr after the Media tag. */
    const char *mp = NULL;
    for (const char *q = r.body; (q = strstr(q, "Media")) != NULL; ++q)
        if (q > r.body && (q[-1] == ':' || q[-1] == '<')) { mp = q; break; }
    if (mp && disc_xml_text(mp, r.len - (size_t)(mp - r.body), "XAddr", xa, sizeof xa, NULL) == 0 && xa[0]) {
        if (onvif_init(&m, xa, d, user, pass) < 0) m = o;
        m.clock_offset = o.clock_offset;
    }
    free(r.body);

    rc = soap(&m, m.path, "<trt:GetProfiles/>", 1, &r, "GetProfiles", err, errcap);
    if (rc != DISC_OK) return rc;
    /* Main and sub of channel `channel`: an NVR or encoder lists its
     * channels' profiles in order, two to a channel. */
    char tokens[2][64];
    int nt = 0, seen = 0;
    size_t from = 0;
    while (nt < 2 && disc_xml_attr(r.body, r.len, "Profiles", "token", tokens[nt], sizeof tokens[nt], &from) == 0)
        if (tokens[nt][0] && seen++ >= 2 * channel) ++nt;
    free(r.body);
    if (!nt) {
        if (channel && seen) snprintf(err, errcap, "GetProfiles: no channel %d (%d profiles)", channel + 1, seen);
        else snprintf(err, errcap, "GetProfiles: no media profile");
        return DISC_EUNSUPPORTED;
    }
    for (int i = 0; i < nt; ++i) {
        char body[512], tok[96], uri[512];
        xml_escape(tokens[i], tok, sizeof tok);
        snprintf(body, sizeof body,
                 "<trt:GetStreamUri><trt:StreamSetup><tt:Stream>RTP-Unicast</tt:Stream>"
                 "<tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport></trt:StreamSetup>"
                 "<trt:ProfileToken>%s</trt:ProfileToken></trt:GetStreamUri>", tok);
        rc = soap(&m, m.path, body, 1, &r, "GetStreamUri", err, errcap);
        if (rc != DISC_OK) {
            if (i) break;                         /* a main stream is enough */
            return rc;
        }
        int got = disc_xml_text(r.body, r.len, "Uri", uri, sizeof uri, NULL) == 0 && !strncasecmp(uri, "rtsp://", 7);
        free(r.body);
        if (!got) {
            if (i) break;
            snprintf(err, errcap, "GetStreamUri: no rtsp:// URI");
            return DISC_EPROTO;
        }
        if (disc_url_with_creds(uri, o.user, o.pass, i ? sub : url, i ? scap : ucap) < 0) {
            if (i) break;
            snprintf(err, errcap, "the stream URL is too long");
            return DISC_EPROTO;
        }
    }
    return DISC_OK;
}

disc_rc_t disc_onvif_set_ip(const disc_device_t *d, const char *ip, const char *mask, const char *gw,
                            const char *user, const char *pass, char *err, size_t errcap)
{
    onvif_t o;
    int prefix = disc_mask_prefix(mask);
    uint32_t t;
    if (prefix < 1 || disc_ip_parse(ip, &t) < 0 || (gw[0] && disc_ip_parse(gw, &t) < 0)) {
        snprintf(err, errcap, "bad address, mask or gateway");
        return DISC_EINVAL;
    }
    if (!d->xaddr[0] || onvif_init(&o, d->xaddr, d, user, pass) < 0) {
        snprintf(err, errcap, "no ONVIF service address");
        return DISC_EUNSUPPORTED;
    }
    onvif_clock(&o);
    disc_http_t r;
    disc_rc_t rc = soap(&o, o.path, "<tds:GetNetworkInterfaces/>", 1, &r, "GetNetworkInterfaces", err, errcap);
    if (rc != DISC_OK) return rc;
    char token[64], tok[96];
    int ok = disc_xml_attr(r.body, r.len, "NetworkInterfaces", "token", token, sizeof token, NULL) == 0 && token[0];
    free(r.body);
    if (!ok) {
        snprintf(err, errcap, "GetNetworkInterfaces: no interface token");
        return DISC_EPROTO;
    }
    xml_escape(token, tok, sizeof tok);
    char body[1024];
    /* The gateway first: once the address changes, the device is no longer
     * where this request goes. */
    if (gw[0]) {
        snprintf(body, sizeof body,
                 "<tds:SetNetworkDefaultGateway><tds:IPv4Address>%s</tds:IPv4Address></tds:SetNetworkDefaultGateway>", gw);
        rc = soap(&o, o.path, body, 1, &r, "SetNetworkDefaultGateway", err, errcap);
        if (rc != DISC_OK) return rc;
        free(r.body);
    }
    snprintf(body, sizeof body,
             "<tds:SetNetworkInterfaces><tds:InterfaceToken>%s</tds:InterfaceToken><tds:NetworkInterface>"
             "<tt:Enabled>true</tt:Enabled><tt:IPv4><tt:Enabled>true</tt:Enabled>"
             "<tt:Manual><tt:Address>%s</tt:Address><tt:PrefixLength>%d</tt:PrefixLength></tt:Manual>"
             "<tt:DHCP>false</tt:DHCP></tt:IPv4></tds:NetworkInterface></tds:SetNetworkInterfaces>", tok, ip, prefix);
    rc = soap(&o, o.path, body, 1, &r, "SetNetworkInterfaces", err, errcap);
    if (rc == DISC_ETIMEOUT || (rc == DISC_EPROTO && strstr(err, "connection closed"))) {
        /* Some devices switch before they answer. The re-probe says. */
        snprintf(err, errcap, "SetNetworkInterfaces: no answer (the device may have changed already)");
        return DISC_OK;
    }
    if (rc != DISC_OK) return rc;
    char reboot[16] = "";
    disc_xml_text(r.body, r.len, "RebootNeeded", reboot, sizeof reboot, NULL);
    free(r.body);
    if (!strcasecmp(reboot, "true")) {
        if (soap(&o, o.path, "<tds:SystemReboot/>", 1, &r, "SystemReboot", err, errcap) == DISC_OK) free(r.body);
        snprintf(err, errcap, "rebooting");
    } else {
        err[0] = 0;
    }
    return DISC_OK;
}

/* ---- Dahua RPC2 ------------------------------------------------------------------------- */

static void md5_upper(const char *s, char out[33])
{
    md5_hex(s, strlen(s), out);
    for (int i = 0; out[i]; ++i) out[i] = (char)toupper((unsigned char)out[i]);
}

void disc_dahua_hash(const char *user, const char *pass, const char *realm, const char *random, char out[33])
{
    char buf[512], h1[33];
    snprintf(buf, sizeof buf, "%s:%s:%s", user, realm, pass);
    md5_upper(buf, h1);
    snprintf(buf, sizeof buf, "%s:%s:%s", user, random, h1);
    md5_upper(buf, out);
    wipe(buf, sizeof buf);
}

/* POST a JSON call; the parsed answer, or NULL with `err`/`rc`. */
static cJSON *rpc2(const disc_device_t *d, const char *path, cJSON *call, disc_rc_t *rc, char *err, size_t errcap)
{
    char *text = cJSON_PrintUnformatted(call);
    if (!text) {
        *rc = DISC_EPROTO;
        snprintf(err, errcap, "out of memory");
        return NULL;
    }
    disc_http_t r;
    char e2[128];
    int port = d->http_port ? d->http_port : 80;
    int st = disc_http_post(d->ip, port, path, "application/json", NULL, text, CALL_MS, &r, e2, sizeof e2);
    wipe(text, strlen(text));
    free(text);
    if (st < 0) {
        *rc = !strncmp(e2, "connect:", 8) || !strncmp(e2, "cannot resolve", 14) ? DISC_EUNREACHABLE
            : strstr(e2, "no answer") ? DISC_ETIMEOUT : DISC_EPROTO;
        snprintf(err, errcap, "%s: %s", path, e2);
        return NULL;
    }
    cJSON *doc = r.status == 200 && r.body ? cJSON_ParseWithLength(r.body, r.len) : NULL;
    if (!doc) {
        *rc = r.status == 404 ? DISC_EUNSUPPORTED : DISC_EPROTO;
        snprintf(err, errcap, "%s: HTTP %d%s", path, r.status, r.status == 200 ? ", not JSON" : "");
    }
    free(r.body);
    return doc;
}

static int rpc2_ok(const cJSON *doc)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(doc, "result"));
}

static int rpc2_err(const cJSON *doc)
{
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(doc, "error"), "code");
    return cJSON_IsNumber(e) ? (int)e->valuedouble : 0;
}

disc_rc_t disc_dahua_set_ip(const disc_device_t *d, const char *ip, const char *mask, const char *gw,
                            const char *user, const char *pass, char *err, size_t errcap)
{
    uint32_t t;
    if (disc_ip_parse(ip, &t) < 0 || disc_mask_prefix(mask) < 1 || (gw[0] && disc_ip_parse(gw, &t) < 0)) {
        snprintf(err, errcap, "bad address, mask or gateway");
        return DISC_EINVAL;
    }
    if (!user || !*user) user = "admin";
    if (!pass) pass = "";
    disc_rc_t rc = DISC_EPROTO;
    cJSON *call = cJSON_CreateObject(), *p = cJSON_AddObjectToObject(call, "params");
    cJSON_AddStringToObject(call, "method", "global.login");
    cJSON_AddStringToObject(p, "userName", user);
    cJSON_AddStringToObject(p, "password", "");
    cJSON_AddStringToObject(p, "clientType", "Web3.0");
    cJSON_AddStringToObject(p, "loginType", "Direct");
    cJSON_AddNumberToObject(call, "id", 1);
    cJSON *a = rpc2(d, "/RPC2_Login", call, &rc, err, errcap);
    cJSON_Delete(call);
    if (!a) return rc;
    const cJSON *ap = cJSON_GetObjectItemCaseSensitive(a, "params");
    const cJSON *realm = cJSON_GetObjectItemCaseSensitive(ap, "realm");
    const cJSON *rnd = cJSON_GetObjectItemCaseSensitive(ap, "random");
    const cJSON *enc = cJSON_GetObjectItemCaseSensitive(ap, "encryption");
    cJSON *session = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(a, "session"), 1);
    if (!cJSON_IsString(realm) || !cJSON_IsString(rnd) || !session) {
        snprintf(err, errcap, "RPC2_Login: no challenge");
        cJSON_Delete(a);
        cJSON_Delete(session);
        return DISC_EUNSUPPORTED;
    }
    if (cJSON_IsString(enc) && strcmp(enc->valuestring, "Default") != 0) {
        snprintf(err, errcap, "RPC2_Login: \"%.32s\" login is not supported", enc->valuestring);
        cJSON_Delete(a);
        cJSON_Delete(session);
        return DISC_EUNSUPPORTED;
    }
    char hash[33];
    disc_dahua_hash(user, pass, realm->valuestring, rnd->valuestring, hash);
    call = cJSON_CreateObject();
    p = cJSON_AddObjectToObject(call, "params");
    cJSON_AddStringToObject(call, "method", "global.login");
    cJSON_AddStringToObject(p, "userName", user);
    cJSON_AddStringToObject(p, "password", hash);
    cJSON_AddStringToObject(p, "clientType", "Web3.0");
    cJSON_AddStringToObject(p, "loginType", "Direct");
    cJSON_AddStringToObject(p, "authorityType", "Default");
    cJSON_AddStringToObject(p, "passwordType", "Default");
    cJSON_AddStringToObject(p, "realm", realm->valuestring);
    cJSON_AddStringToObject(p, "random", rnd->valuestring);
    cJSON_AddNumberToObject(call, "id", 2);
    cJSON_AddItemToObject(call, "session", cJSON_Duplicate(session, 1));
    wipe(hash, sizeof hash);
    cJSON_Delete(a);
    a = rpc2(d, "/RPC2_Login", call, &rc, err, errcap);
    cJSON_Delete(call);
    if (!a) {
        cJSON_Delete(session);
        return rc;
    }
    if (!rpc2_ok(a)) {
        int code = rpc2_err(a);
        snprintf(err, errcap, "login refused (%d)", code);
        cJSON_Delete(a);
        cJSON_Delete(session);
        /* 268632080 user/password, 268632081 locked (python-dhip const.py) */
        return code == 268632080 || code == 268632081 || code == 268632082 ? DISC_EAUTH : DISC_EPROTO;
    }
    const cJSON *s2 = cJSON_GetObjectItemCaseSensitive(a, "session");
    if (s2) {
        cJSON_Delete(session);
        session = cJSON_Duplicate(s2, 1);
    }
    cJSON_Delete(a);

    call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "method", "configManager.getConfig");
    cJSON_AddStringToObject(cJSON_AddObjectToObject(call, "params"), "name", "Network");
    cJSON_AddNumberToObject(call, "id", 3);
    cJSON_AddItemToObject(call, "session", cJSON_Duplicate(session, 1));
    a = rpc2(d, "/RPC2", call, &rc, err, errcap);
    cJSON_Delete(call);
    if (!a || !rpc2_ok(a)) {
        if (a) snprintf(err, errcap, "getConfig Network refused (%d)", rpc2_err(a));
        cJSON_Delete(a);
        cJSON_Delete(session);
        return a ? DISC_EPROTO : rc;
    }
    cJSON *table = cJSON_DetachItemFromObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(a, "params"), "table");
    cJSON_Delete(a);
    const cJSON *di = cJSON_GetObjectItemCaseSensitive(table, "DefaultInterface");
    cJSON *ifc = cJSON_GetObjectItemCaseSensitive(table, cJSON_IsString(di) ? di->valuestring : "eth0");
    if (!cJSON_IsObject(ifc)) {
        snprintf(err, errcap, "getConfig Network: no interface");
        cJSON_Delete(table);
        cJSON_Delete(session);
        return DISC_EPROTO;
    }
    cJSON_ReplaceItemInObjectCaseSensitive(ifc, "IPAddress", cJSON_CreateString(ip));
    cJSON_ReplaceItemInObjectCaseSensitive(ifc, "SubnetMask", cJSON_CreateString(mask));
    if (gw[0]) {
        if (cJSON_GetObjectItemCaseSensitive(ifc, "DefaultGateway"))
            cJSON_ReplaceItemInObjectCaseSensitive(ifc, "DefaultGateway", cJSON_CreateString(gw));
        else
            cJSON_AddStringToObject(ifc, "DefaultGateway", gw);
    }
    if (cJSON_GetObjectItemCaseSensitive(ifc, "DhcpEnable"))
        cJSON_ReplaceItemInObjectCaseSensitive(ifc, "DhcpEnable", cJSON_CreateFalse());
    if (!cJSON_GetObjectItemCaseSensitive(ifc, "IPAddress")) cJSON_AddStringToObject(ifc, "IPAddress", ip);
    if (!cJSON_GetObjectItemCaseSensitive(ifc, "SubnetMask")) cJSON_AddStringToObject(ifc, "SubnetMask", mask);

    call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "method", "configManager.setConfig");
    p = cJSON_AddObjectToObject(call, "params");
    cJSON_AddStringToObject(p, "name", "Network");
    cJSON_AddItemToObject(p, "table", table);
    cJSON_AddNumberToObject(call, "id", 4);
    cJSON_AddItemToObject(call, "session", session);
    a = rpc2(d, "/RPC2", call, &rc, err, errcap);
    cJSON_Delete(call);
    if (!a) {
        if (rc == DISC_ETIMEOUT) {
            snprintf(err, errcap, "setConfig: no answer (the device may have changed already)");
            return DISC_OK;
        }
        return rc;
    }
    int ok = rpc2_ok(a);
    if (!ok) snprintf(err, errcap, "setConfig Network refused (%d)", rpc2_err(a));
    else err[0] = 0;
    cJSON_Delete(a);
    return ok ? DISC_OK : DISC_EPROTO;
}

/* ---- NETIP over TCP ------------------------------------------------------------------------ */

typedef struct {
    int      fd;
    uint32_t session, seq;
} xm_t;

static int xm_send(xm_t *x, int msgid, const char *json)
{
    size_t jl = strlen(json);
    uint8_t *b = malloc(jl + 22);
    if (!b) return -1;
    memset(b, 0, 20);
    b[0] = 0xff;
    b[4] = x->session; b[5] = x->session >> 8; b[6] = x->session >> 16; b[7] = x->session >> 24;
    b[8] = x->seq; b[9] = x->seq >> 8; b[10] = x->seq >> 16; b[11] = x->seq >> 24;
    ++x->seq;
    b[14] = msgid & 0xff;
    b[15] = msgid >> 8;
    uint32_t l = (uint32_t)jl + 2;
    b[16] = l; b[17] = l >> 8; b[18] = l >> 16; b[19] = l >> 24;
    memcpy(b + 20, json, jl);
    b[20 + jl] = '\n';
    b[21 + jl] = 0;
    int ok = send(x->fd, b, jl + 22, MSG_NOSIGNAL) == (ssize_t)(jl + 22);
    wipe(b, jl + 22);
    free(b);
    return ok ? 0 : -1;
}

static int xm_recv_all(int fd, uint8_t *b, size_t n, long long deadline)
{
    size_t got = 0;
    while (got < n) {
        if (now_ms() >= deadline) return -2;
        ssize_t r = recv(fd, b + got, n - got, 0);
        if (r > 0) got += (size_t)r;
        else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        else return -1;
    }
    return 0;
}

/* The JSON answer to `msgid` (msgid + 1), skipping others. */
static cJSON *xm_call(xm_t *x, int msgid, const char *json, disc_rc_t *rc, char *err, size_t errcap)
{
    if (xm_send(x, msgid, json) < 0) {
        *rc = DISC_EUNREACHABLE;
        snprintf(err, errcap, "send failed");
        return NULL;
    }
    long long deadline = now_ms() + CALL_MS;
    for (;;) {
        uint8_t h[20];
        int r = xm_recv_all(x->fd, h, 20, deadline);
        if (r == 0 && h[0] != 0xff) r = -1;
        uint32_t len = r == 0 ? (uint32_t)(h[16] | h[17] << 8 | h[18] << 16 | (uint32_t)h[19] << 24) : 0;
        if (r == 0 && len > 64 * 1024) r = -1;
        uint8_t *b = r == 0 ? malloc(len + 1) : NULL;
        if (r == 0 && !b) r = -1;
        if (r == 0) r = xm_recv_all(x->fd, b, len, deadline);
        if (r < 0) {
            free(b);
            *rc = r == -2 ? DISC_ETIMEOUT : DISC_EPROTO;
            snprintf(err, errcap, r == -2 ? "no answer to msg %d" : "connection lost (msg %d)", msgid);
            return NULL;
        }
        int rid = h[14] | h[15] << 8;
        if (rid != msgid + 1) {
            free(b);
            continue;
        }
        x->session = h[4] | h[5] << 8 | h[6] << 16 | (uint32_t)h[7] << 24;
        size_t n = len;
        while (n && (b[n - 1] == 0 || b[n - 1] == '\n')) --n;
        cJSON *doc = cJSON_ParseWithLength((const char *)b, n);
        free(b);
        if (!doc) {
            *rc = DISC_EPROTO;
            snprintf(err, errcap, "bad answer to msg %d", msgid);
        }
        return doc;
    }
}

static int xm_ret(const cJSON *doc)
{
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(doc, "Ret");
    return cJSON_IsNumber(r) ? (int)r->valuedouble : -1;
}

disc_rc_t disc_netip_tcp_set_ip(const disc_device_t *d, const char *ip, const char *mask, const char *gw,
                                const char *user, const char *pass, char *err, size_t errcap)
{
    char hip[16], hmask[16], hgw[16], hash[9], req[512];
    if (disc_netip_iphex(ip, hip, sizeof hip) < 0 || disc_netip_iphex(mask, hmask, sizeof hmask) < 0 ||
        (gw[0] && disc_netip_iphex(gw, hgw, sizeof hgw) < 0)) {
        snprintf(err, errcap, "bad address, mask or gateway");
        return DISC_EINVAL;
    }
    if (!user || !*user) user = "admin";
    xm_t x = { .fd = net_connect(d->ip, d->netip_port ? d->netip_port : 34567, CONNECT_MS, NULL, err, errcap) };
    if (x.fd < 0) return DISC_EUNREACHABLE;
    disc_rc_t rc = DISC_EPROTO;
    md5_sofia(pass ? pass : "", hash);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "EncryptType", "MD5");
    cJSON_AddStringToObject(o, "LoginType", "DVRIP-Web");
    cJSON_AddStringToObject(o, "PassWord", hash);
    cJSON_AddStringToObject(o, "UserName", user);
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    wipe(hash, sizeof hash);
    cJSON *a = text ? xm_call(&x, 1000, text, &rc, err, errcap) : NULL;
    if (text) wipe(text, strlen(text));
    free(text);
    if (!a) goto out;
    int ret = xm_ret(a);
    cJSON_Delete(a);
    if (ret != 100 && ret != 515) {
        snprintf(err, errcap, "login refused (Ret %d)", ret);
        rc = ret == 203 || ret == 205 || ret == 206 ? DISC_EAUTH : DISC_EPROTO;
        goto out;
    }
    snprintf(req, sizeof req, "{\"Name\":\"NetWork.NetCommon\",\"SessionID\":\"0x%08X\"}", x.session);
    a = xm_call(&x, 1042, req, &rc, err, errcap);
    if (!a) goto out;
    cJSON *nc = cJSON_DetachItemFromObjectCaseSensitive(a, "NetWork.NetCommon");
    ret = xm_ret(a);
    cJSON_Delete(a);
    if (!cJSON_IsObject(nc)) {
        snprintf(err, errcap, "ConfigGet NetWork.NetCommon refused (Ret %d)", ret);
        rc = ret == 607 ? DISC_EAUTH : DISC_EPROTO;   /* 607: no permission */
        cJSON_Delete(nc);
        goto out;
    }
    cJSON_ReplaceItemInObjectCaseSensitive(nc, "HostIP", cJSON_CreateString(hip));
    cJSON_ReplaceItemInObjectCaseSensitive(nc, "Submask", cJSON_CreateString(hmask));
    if (gw[0]) cJSON_ReplaceItemInObjectCaseSensitive(nc, "GateWay", cJSON_CreateString(hgw));
    o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "Name", "NetWork.NetCommon");
    cJSON_AddItemToObject(o, "NetWork.NetCommon", nc);
    snprintf(req, sizeof req, "0x%08X", x.session);
    cJSON_AddStringToObject(o, "SessionID", req);
    text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    a = text ? xm_call(&x, 1040, text, &rc, err, errcap) : NULL;
    free(text);
    if (!a) {
        if (rc == DISC_ETIMEOUT || rc == DISC_EPROTO) {
            snprintf(err, errcap, "ConfigSet: no answer (the device may have changed already)");
            rc = DISC_OK;
        }
        goto out;
    }
    ret = xm_ret(a);
    cJSON_Delete(a);
    /* 603: done, the application restarts; 604: done, the device reboots */
    if (ret == 100 || ret == 603 || ret == 604) {
        rc = DISC_OK;
        snprintf(err, errcap, ret == 100 ? "" : "restarting");
    } else {
        snprintf(err, errcap, "ConfigSet refused (Ret %d)", ret);
        rc = ret == 607 ? DISC_EAUTH : DISC_EPROTO;
    }
out:
    close(x.fd);
    return rc;
}
