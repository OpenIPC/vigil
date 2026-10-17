/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rtsp_client.c — minimal RTSP client for IP cameras (see rtsp_client.h). */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

#include "md5.h"
#include "net.h"
#include "rtsp_client.h"

#define BUF_MAX  (1u << 20)         /* an interleaved frame is at most 64 KiB */

/* Unread bytes of the receive buffer. */
#define DATA(r)   ((r)->buf + (r)->buf_off)
#define AVAIL(r)  ((r)->buf_len - (r)->buf_off)

/* ---- small helpers ------------------------------------------------------ */

/* Case-insensitive strstr: strcasestr is a GNU extension (musl hides it). */
static const char *ci_strstr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; ++hay)
        if (strncasecmp(hay, needle, n) == 0) return hay;
    return NULL;
}

static void set_err(rtsp_t *r, const char *fmt, const char *arg)
{
    snprintf(r->err, sizeof r->err, fmt, arg ? arg : "");
}

static int b64_val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* Decode base64 from `s` (up to `n` chars or a non-base64 char). */
static size_t b64_decode(const char *s, size_t n, uint8_t *out, size_t cap)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; ++i) {
        int v = b64_val((unsigned char)s[i]);
        if (v < 0) break;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o < cap) out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return o;
}

static void b64_encode(const char *in, char *out, size_t cap)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(in), o = 0;
    for (size_t i = 0; i < n && o + 5 < cap; i += 3) {
        uint32_t v = (uint8_t)in[i] << 16;
        if (i + 1 < n) v |= (uint8_t)in[i + 1] << 8;
        if (i + 2 < n) v |= (uint8_t)in[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? T[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? T[v & 63] : '=';
    }
    out[o] = 0;
}

/* Value of header `name` in a response head (case-insensitive), copied
 * up to end of line. Returns 1 if found. */
static int header_get(const char *head, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    for (const char *p = head; (p = strchr(p, '\n')) != NULL; ) {
        ++p;
        if (strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            p += nl + 1;
            while (*p == ' ' || *p == '\t') ++p;
            size_t i = 0;
            while (p[i] && p[i] != '\r' && p[i] != '\n' && i + 1 < cap) { out[i] = p[i]; ++i; }
            out[i] = 0;
            return 1;
        }
    }
    return 0;
}

/* key="value" or key=value from an auth header. */
static void auth_param(const char *h, const char *key, char *out, size_t cap)
{
    out[0] = 0;
    size_t kl = strlen(key);
    for (const char *p = h; (p = ci_strstr(p, key)) != NULL; p += kl) {
        if (p > h && isalnum((unsigned char)p[-1])) continue;
        const char *v = p + kl;
        while (*v == ' ') ++v;
        if (*v != '=') continue;
        ++v;
        while (*v == ' ') ++v;
        char term = ',';
        if (*v == '"') { term = '"'; ++v; }
        size_t i = 0;
        while (v[i] && v[i] != term && v[i] != '\r' && v[i] != '\n' && i + 1 < cap) { out[i] = v[i]; ++i; }
        out[i] = 0;
        return;
    }
}

/* ---- connection ---------------------------------------------------------- */

int rtsp_init(rtsp_t *r, const char *url)
{
    memset(r, 0, sizeof *r);
    r->fd = -1;
    r->port = 554;
    if (strncmp(url, "rtsp://", 7) != 0) return -1;

    const char *p = url + 7;
    const char *slash = strchr(p, '/');
    const char *at = strchr(p, '@');
    if (at && (!slash || at < slash)) {
        const char *colon = memchr(p, ':', (size_t)(at - p));
        size_t ul = (size_t)((colon ? colon : at) - p);
        if (ul >= sizeof r->user) return -1;
        memcpy(r->user, p, ul);
        if (colon) {
            size_t pl = (size_t)(at - colon - 1);
            if (pl >= sizeof r->pass) return -1;
            memcpy(r->pass, colon + 1, pl);
        }
        p = at + 1;
    }
    const char *hend = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(hend - p));
    size_t hl = (size_t)((colon ? colon : hend) - p);
    if (hl == 0 || hl >= sizeof r->host) return -1;
    memcpy(r->host, p, hl);
    if (colon) r->port = atoi(colon + 1);
    snprintf(r->url, sizeof r->url, "rtsp://%.*s%s", (int)(hend - p), p, slash ? slash : "/");

    r->buf_cap = 128 * 1024;
    r->buf = malloc(r->buf_cap);
    return r->buf ? 0 : -1;
}

static int tcp_connect(rtsp_t *r, int timeout_ms)
{
    r->fd = net_connect(r->host, r->port, timeout_ms, r->err, sizeof r->err);
    return r->fd < 0 ? -1 : 0;
}

/* Read more bytes into the buffer. 1 = got some, 0 = timeout, -1 = error. */
static int fill(rtsp_t *r)
{
    if (r->buf_len == r->buf_cap && r->buf_off) {
        memmove(r->buf, DATA(r), AVAIL(r));
        r->buf_len -= r->buf_off;
        r->buf_off = 0;
    }
    if (r->buf_len == r->buf_cap) {
        if (r->buf_cap >= BUF_MAX) { set_err(r, "receive buffer overflow%s", NULL); return -1; }
        uint8_t *p = realloc(r->buf, r->buf_cap * 2);
        if (!p) return -1;
        r->buf = p;
        r->buf_cap *= 2;
    }
    ssize_t n = recv(r->fd, r->buf + r->buf_len, r->buf_cap - r->buf_len, 0);
    if (n > 0) { r->buf_len += (size_t)n; return 1; }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    set_err(r, "connection closed%s", n < 0 ? strerror(errno) : NULL);
    return -1;
}

static void consume(rtsp_t *r, size_t n)
{
    r->buf_off += n;
    if (r->buf_off == r->buf_len) r->buf_off = r->buf_len = 0;
}

/* If the buffer starts with a complete RTSP message, return its total
 * length and the head/body split; 0 if incomplete. */
static size_t message_len(rtsp_t *r, size_t *head_len)
{
    for (size_t i = 3; i < AVAIL(r); ++i) {
        if (memcmp(DATA(r) + i - 3, "\r\n\r\n", 4) == 0) {
            *head_len = i + 1;
            char tmp[16] = "", head[4096];
            size_t hl = *head_len < sizeof head - 1 ? *head_len : sizeof head - 1;
            memcpy(head, DATA(r), hl);
            head[hl] = 0;
            size_t body = header_get(head, "Content-Length", tmp, sizeof tmp) ? (size_t)atoi(tmp) : 0;
            return (*head_len + body <= AVAIL(r)) ? *head_len + body : 0;
        }
    }
    return 0;
}

void rtsp_digest_response(const char *user, const char *realm, const char *pass,
                          const char *method, const char *uri, const char *nonce,
                          const char *nc, const char *cnonce, const char *qop,
                          char out[33])
{
    char a1[512], a2[640], ha1[33], ha2[33], all[512];
    snprintf(a1, sizeof a1, "%s:%s:%s", user, realm, pass);
    snprintf(a2, sizeof a2, "%s:%s", method, uri);
    md5_hex(a1, strlen(a1), ha1);
    md5_hex(a2, strlen(a2), ha2);
    if (qop && qop[0])
        snprintf(all, sizeof all, "%s:%s:%s:%s:%s:%s", ha1, nonce, nc, cnonce, qop, ha2);
    else
        snprintf(all, sizeof all, "%s:%s:%s", ha1, nonce, ha2);
    md5_hex(all, strlen(all), out);
}

int rtsp_transport_channel(const char *transport)
{
    const char *p = strstr(transport, "interleaved=");
    return p ? atoi(p + 12) : 0;
}

static void auth_header(rtsp_t *r, const char *method, const char *uri, char *out, size_t cap)
{
    out[0] = 0;
    if (r->auth == 1) {
        char cred[160], b64[224];
        snprintf(cred, sizeof cred, "%s:%s", r->user, r->pass);
        b64_encode(cred, b64, sizeof b64);
        snprintf(out, cap, "Authorization: Basic %s\r\n", b64);
    } else if (r->auth == 2) {
        char resp[33], nc[9] = "", cnonce[17] = "", extra[256] = "";
        if (r->qop_auth) {
            snprintf(nc, sizeof nc, "%08x", ++r->nc);
            snprintf(cnonce, sizeof cnonce, "%08lx%08lx",
                     (unsigned long)random() ^ (unsigned long)time(NULL),
                     (unsigned long)random() ^ ((unsigned long)getpid() << 8));
            snprintf(extra, sizeof extra, ", qop=auth, nc=%s, cnonce=\"%s\"", nc, cnonce);
        }
        rtsp_digest_response(r->user, r->realm, r->pass, method, uri, r->nonce,
                             nc, cnonce, r->qop_auth ? "auth" : NULL, resp);
        int n = snprintf(out, cap,
                         "Authorization: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", "
                         "uri=\"%s\", response=\"%s\"%s",
                         r->user, r->realm, r->nonce, uri, resp, extra);
        if (n > 0 && (size_t)n < cap && r->opaque[0])
            n += snprintf(out + n, cap - (size_t)n, ", opaque=\"%s\"", r->opaque);
        if (n > 0 && (size_t)n < cap) snprintf(out + n, cap - (size_t)n, "\r\n");
    }
}

static int send_request(rtsp_t *r, const char *method, const char *uri, const char *extra)
{
    char auth[768], req[2048];
    auth_header(r, method, uri, auth, sizeof auth);
    int n = snprintf(req, sizeof req,
                     "%s %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: vigil\r\n%s%s%s%s%s\r\n",
                     method, uri, ++r->cseq, auth,
                     r->session[0] ? "Session: " : "", r->session,
                     r->session[0] ? "\r\n" : "", extra ? extra : "");
    if (n <= 0 || (size_t)n >= sizeof req) return -1;
    if (send(r->fd, req, (size_t)n, MSG_NOSIGNAL) != n) {
        set_err(r, "send: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* Wait for the reply to our last request (skipping interleaved data).
 * Returns the status code with the head and body copied out, or -1. */
static int read_reply(rtsp_t *r, char *head, size_t head_cap, char *body, size_t body_cap)
{
    for (int waited = 0; waited < 10; ) {
        if (AVAIL(r) > 0 && DATA(r)[0] == '$') {
            if (AVAIL(r) >= 4) {
                size_t n = ((size_t)DATA(r)[2] << 8) | DATA(r)[3];
                if (AVAIL(r) >= 4 + n) { consume(r, 4 + n); continue; }
            }
        } else if (AVAIL(r) > 0) {
            size_t hl, total = message_len(r, &hl);
            if (total) {
                size_t c = hl < head_cap - 1 ? hl : head_cap - 1;
                memcpy(head, DATA(r), c);
                head[c] = 0;
                size_t bl = total - hl;
                c = bl < body_cap - 1 ? bl : body_cap - 1;
                memcpy(body, DATA(r) + hl, c);
                body[c] = 0;
                consume(r, total);
                if (strncmp(head, "RTSP/1.0 ", 9) != 0) continue;   /* a request from the server */
                char cs[16];
                if (header_get(head, "CSeq", cs, sizeof cs) && atoi(cs) != r->cseq) continue;
                return atoi(head + 9);
            }
        }
        if (r->stop && *r->stop) {
            set_err(r, "stopped%s", NULL);
            return -1;
        }
        int f = fill(r);
        if (f < 0) return -1;
        if (f == 0) ++waited;
    }
    set_err(r, "no reply from camera%s", NULL);
    return -1;
}

/* One request with a single retry after a 401. */
static int request(rtsp_t *r, const char *method, const char *uri, const char *extra,
                   char *head, size_t head_cap, char *body, size_t body_cap)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (send_request(r, method, uri, extra) < 0) return -1;
        int st = read_reply(r, head, head_cap, body, body_cap);
        if (st != 401 || attempt || !r->user[0]) return st;

        /* Prefer Digest when the camera offers both. */
        char www[512] = "";
        const char *p = head, *dig = NULL;
        while ((p = ci_strstr(p, "\nWWW-Authenticate:")) != NULL) {
            p += 18;
            while (*p == ' ') ++p;
            if (strncasecmp(p, "Digest", 6) == 0) { dig = p; break; }
        }
        if (dig) {
            size_t i = 0;
            while (dig[i] && dig[i] != '\r' && dig[i] != '\n' && i + 1 < sizeof www) { www[i] = dig[i]; ++i; }
            www[i] = 0;
            r->auth = 2;
            r->nc = 0;
            auth_param(www, "realm", r->realm, sizeof r->realm);
            auth_param(www, "nonce", r->nonce, sizeof r->nonce);
            auth_param(www, "opaque", r->opaque, sizeof r->opaque);
            char qop[64];
            auth_param(www, "qop", qop, sizeof qop);
            /* "auth" or a list such as "auth,auth-int": we do auth. */
            r->qop_auth = strstr(qop, "auth") != NULL;
        } else {
            r->auth = 1;
        }
    }
    return -1;
}

/* ---- SDP ----------------------------------------------------------------- */

static void add_sprop(rtsp_t *r, const char *b64, size_t n)
{
    if (r->sprop_n >= RTSP_SPROP_MAX) return;
    size_t l = b64_decode(b64, n, r->sprop[r->sprop_n], RTSP_SPROP_BYTES);
    if (l) r->sprop_len[r->sprop_n++] = l;
}

static int parse_sdp(rtsp_t *r, const char *sdp, const char *base)
{
    int in_video = 0;
    char control[256] = "";
    for (const char *l = sdp; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : NULL) {
        if (strncmp(l, "m=", 2) == 0) {
            if (in_video) break;                        /* only the first video stream */
            in_video = strncmp(l, "m=video", 7) == 0;
            if (in_video) {
                const char *s = strstr(l, "RTP/AVP");
                r->pt = s ? atoi(s + 8) : -1;
            }
            continue;
        }
        if (!in_video) continue;
        if (strncmp(l, "a=rtpmap:", 9) == 0 && atoi(l + 9) == r->pt) {
            const char *c = strchr(l, ' ');
            if (c && strncasecmp(c + 1, "H264/", 5) == 0) r->codec = VG_CODEC_H264;
            else if (c && (strncasecmp(c + 1, "H265/", 5) == 0 || strncasecmp(c + 1, "HEVC/", 5) == 0))
                r->codec = VG_CODEC_H265;
        } else if (strncmp(l, "a=fmtp:", 7) == 0 && atoi(l + 7) == r->pt) {
            const char *s;
            if ((s = strstr(l, "sprop-parameter-sets=")) != NULL) {
                s += 21;
                while (*s && *s != ';' && *s != '\r' && *s != '\n') {
                    size_t n = strcspn(s, ",;\r\n");
                    add_sprop(r, s, n);
                    s += n;
                    if (*s == ',') ++s;
                }
            }
            static const char *const keys[] = { "sprop-vps=", "sprop-sps=", "sprop-pps=" };
            for (int k = 0; k < 3; ++k)
                if ((s = strstr(l, keys[k])) != NULL) {
                    s += strlen(keys[k]);
                    add_sprop(r, s, strcspn(s, ";\r\n "));
                }
        } else if (strncmp(l, "a=control:", 10) == 0) {
            size_t n = strcspn(l + 10, "\r\n");
            if (n >= sizeof control) n = sizeof control - 1;
            memcpy(control, l + 10, n);
            control[n] = 0;
        }
    }
    if (r->codec == VG_CODEC_NONE) {
        set_err(r, "no H.264/H.265 video in SDP%s", NULL);
        return -1;
    }
    /* Resolve the control URL the way ffmpeg/live555 do: append to the base. */
    if (strncmp(control, "rtsp://", 7) == 0)
        snprintf(r->control, sizeof r->control, "%s", control);
    else if (!control[0] || strcmp(control, "*") == 0)
        snprintf(r->control, sizeof r->control, "%s", base);
    else
        snprintf(r->control, sizeof r->control, "%s%s%s", base,
                 base[strlen(base) - 1] == '/' ? "" : "/", control);
    return 0;
}

/* ---- public -------------------------------------------------------------- */

int rtsp_open(rtsp_t *r, int connect_ms)
{
    enum { HEAD = 4096, BODY = 8192 };
    char base[512];

    r->cseq = 0;
    r->session[0] = 0;
    r->session_timeout = 60;
    r->buf_off = r->buf_len = 0;
    r->rtp_channel = 0;
    r->codec = VG_CODEC_NONE;
    r->sprop_n = 0;
    r->err[0] = 0;
    r->handed_out = 0;

    if (tcp_connect(r, connect_ms) < 0) return -1;

    char *h = malloc(HEAD), *b = malloc(BODY);
    if (!h || !b) { free(h); free(b); rtsp_close(r); return -1; }

    int st = request(r, "DESCRIBE", r->url, "Accept: application/sdp\r\n", h, HEAD, b, BODY);
    if (st != 200) {
        if (st > 0) snprintf(r->err, sizeof r->err, "DESCRIBE: %d", st);
        goto fail;
    }
    if (!header_get(h, "Content-Base", base, sizeof base) &&
        !header_get(h, "Content-Location", base, sizeof base))
        snprintf(base, sizeof base, "%s", r->url);
    if (parse_sdp(r, b, base) < 0) goto fail;

    st = request(r, "SETUP", r->control,
                 "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n", h, HEAD, b, BODY);
    if (st != 200) {
        if (st > 0) snprintf(r->err, sizeof r->err, "SETUP: %d", st);
        goto fail;
    }
    char transport[256];
    r->rtp_channel = header_get(h, "Transport", transport, sizeof transport)
                         ? rtsp_transport_channel(transport) : 0;
    char sess[sizeof r->session];
    if (header_get(h, "Session", sess, sizeof sess)) {
        char *semi = strchr(sess, ';');
        if (semi) {
            const char *t = strstr(semi, "timeout=");
            if (t && atoi(t + 8) > 0) r->session_timeout = atoi(t + 8);
            *semi = 0;
        }
        snprintf(r->session, sizeof r->session, "%s", sess);
    }

    st = request(r, "PLAY", r->url, "Range: npt=0.000-\r\n", h, HEAD, b, BODY);
    if (st != 200) {
        if (st > 0) snprintf(r->err, sizeof r->err, "PLAY: %d", st);
        goto fail;
    }
    free(h);
    free(b);
    return 0;

fail:
    free(h);
    free(b);
    rtsp_close(r);
    return -1;
}

int rtsp_read(rtsp_t *r, const uint8_t **pkt, size_t *len, int *channel)
{
    if (r->handed_out) {
        consume(r, r->handed_out);
        r->handed_out = 0;
    }
    for (int filled = 0; ; ) {
        if (AVAIL(r) > 0 && DATA(r)[0] == '$') {
            /* Interleaved frame: '$', channel, 16-bit length, data. */
            size_t n = AVAIL(r) >= 4 ? ((size_t)DATA(r)[2] << 8) | DATA(r)[3] : 0;
            if (AVAIL(r) >= 4 && AVAIL(r) >= 4 + n) {
                *channel = DATA(r)[1];
                *pkt = DATA(r) + 4;
                *len = n;
                r->handed_out = 4 + n;
                return 1;
            }
        } else if (AVAIL(r) > 0 && isupper(DATA(r)[0])) {
            /* An RTSP message: a keepalive reply, or a request from the
             * camera. Wait for all of it, then drop it. */
            size_t hl, total = message_len(r, &hl);
            if (total) { consume(r, total); continue; }
        } else if (AVAIL(r) > 0) {
            /* Neither: resync to the next '$'. */
            uint8_t *d = memchr(DATA(r) + 1, '$', AVAIL(r) - 1);
            consume(r, d ? (size_t)(d - DATA(r)) : AVAIL(r));
            continue;
        }
        if (filled) return 0;
        int f = fill(r);
        if (f < 0) return -1;
        if (f == 0) return 0;
        filled = 1;
    }
}

int rtsp_keepalive(rtsp_t *r)
{
    return send_request(r, "OPTIONS", r->url, NULL);
}

void rtsp_close(rtsp_t *r)
{
    if (r->fd >= 0) {
        if (r->session[0]) send_request(r, "TEARDOWN", r->url, NULL);
        close(r->fd);
    }
    r->fd = -1;
    r->session[0] = 0;
    r->buf_off = r->buf_len = 0;
    r->handed_out = 0;
}

void rtsp_free(rtsp_t *r)
{
    rtsp_close(r);
    free(r->buf);
    r->buf = NULL;
}
