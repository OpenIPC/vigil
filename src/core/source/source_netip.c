/* SPDX-License-Identifier: GPL-3.0-or-later */
/* source_netip.c — Xiongmai NETIP / DVRIP live source (TCP, default port
 * 34567), as spoken by XM cameras and DVRs and by OpenIPC majestic with
 * netip enabled. Protocol as implemented by python-dvr (dvrip.py,
 * asyncio_dvrip.py):
 *
 *   message   = 20-byte header + body
 *   header    = u8 0xFF, u8 version, u16 0, u32 session, u32 sequence,
 *               u8 total, u8 current, u16 msgid, u32 body length (LE)
 *   requests  : JSON body + "\n\0"
 *   login     : msgid 1000 {EncryptType MD5, LoginType DVRIP-Web,
 *               PassWord <sofia hash>, UserName} -> {Ret, SessionID,
 *               AliveInterval}
 *   keepalive : msgid 1006 every AliveInterval seconds
 *   live      : msgid 1413 OPMonitor Claim, then 1410 OPMonitor Start;
 *               media then arrives as msgid 1412 messages
 *   media     : a frame starts with a big-endian tag and a header, and its
 *               payload may continue over the following messages:
 *                 0x000001FC  I-frame: u8 codec (2 = H.264, 3 = H.265),
 *                             u8 fps, u8 width/8, u8 height/8,
 *                             u32 datetime, u32 length       (16 bytes)
 *                 0x000001FD  P-frame: u32 length            (8 bytes)
 *                 0x000001FA  audio:   u8 codec, u8 rate, u16 length
 *                 0x000001F9  info:    u8 type, u8 n, u16 length
 *                 0x000001FE  JPEG:    as 0x1FC
 *               Video payloads are Annex-B.
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

#include "md5.h"
#include "net.h"
#include "source.h"

#define NETIP_PORT       34567
#define MSG_LOGIN        1000
#define MSG_KEEPALIVE    1006
#define MSG_MONITOR      1410
#define MSG_MONITOR_DATA 1412
#define MSG_MONITOR_CLAIM 1413
#define FRAME_MAX        (4u << 20)

typedef struct {
    char     user[64], pass[64], host[128];
    int      port, channel;
    char     stream[8];             /* "Main" / "Extra" */
    int      fd;
    uint32_t session, seq;
    int      alive;                 /* keepalive interval, s */
    time_t   last_ka;
    /* receive state */
    uint8_t  hdr[20];
    size_t   hdr_got;
    uint8_t *body;
    size_t   body_len, body_got, body_cap;
    /* frame reassembly */
    uint8_t *frame;
    size_t   frame_len, frame_want, frame_cap;
    int      frame_kind;            /* 0 none, 1 video I, 2 video P, 3 skip */
    vg_codec_t codec;
} netip_t;

/* "sofia" hash: MD5, then each byte pair -> one of 62 characters. */
static void sofia_hash(const char *pass, char out[9])
{
    static const char chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    uint8_t d[16];
    md5_raw(pass, strlen(pass), d);
    for (int i = 0; i < 8; ++i) out[i] = chars[(d[2 * i] + d[2 * i + 1]) % 62];
    out[8] = 0;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t get32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* Header layout "BB2xII2xHI": 0 head 0xFF, 1 version, 2..3 pad,
 * 4..7 session, 8..11 sequence, 12..13 total/current, 14..15 msgid,
 * 16..19 body length. Body = JSON + "\n\0". */
static int send_msg(netip_t *n, int msgid, const char *json)
{
    size_t jl = strlen(json);
    uint8_t buf[2048];
    if (jl + 22 > sizeof buf) return -1;
    memset(buf, 0, 20);
    buf[0] = 0xff;
    put32(buf + 4, n->session);
    put32(buf + 8, n->seq++);
    buf[14] = msgid & 0xff;
    buf[15] = (msgid >> 8) & 0xff;
    put32(buf + 16, (uint32_t)(jl + 2));
    memcpy(buf + 20, json, jl);
    buf[20 + jl] = '\n';
    buf[21 + jl] = 0;
    size_t total = jl + 22;
    return send(n->fd, buf, total, MSG_NOSIGNAL) == (ssize_t)total ? 0 : -1;
}

/* Read exactly `len` bytes, waiting up to `secs` (1 s socket timeout per
 * read). Used only during login/claim. */
static int recv_all(netip_t *n, uint8_t *buf, size_t len, int secs)
{
    size_t got = 0;
    for (int waited = 0; got < len; ) {
        ssize_t r = recv(n->fd, buf + got, len - got, 0);
        if (r > 0) { got += (size_t)r; continue; }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) && ++waited < secs) continue;
        return -1;
    }
    return 0;
}

/* Request/response for the login and claim steps. */
static int transact(source_t *s, int msgid, const char *json, char *reply, size_t cap)
{
    netip_t *n = s->priv;
    if (send_msg(n, msgid, json) < 0) {
        snprintf(s->err, sizeof s->err, "send failed");
        return -1;
    }
    for (;;) {
        uint8_t h[20];
        if (recv_all(n, h, 20, 5) < 0 || h[0] != 0xff) {
            snprintf(s->err, sizeof s->err, "no reply (msg %d)", msgid);
            return -1;
        }
        uint32_t len = get32(h + 16);
        if (len > 64 * 1024) { snprintf(s->err, sizeof s->err, "bad reply"); return -1; }
        uint8_t *b = malloc(len + 1);
        if (!b || recv_all(n, b, len, 5) < 0) { free(b); snprintf(s->err, sizeof s->err, "short reply"); return -1; }
        b[len] = 0;
        int rid = h[14] | h[15] << 8;
        if (rid == msgid + 1 || msgid == MSG_MONITOR_CLAIM) {
            n->session = get32(h + 4);
            snprintf(reply, cap, "%s", (char *)b);
            free(b);
            return 0;
        }
        free(b);                                 /* something else; keep waiting */
    }
}

static int json_int(const char *j, const char *key, int dflt)
{
    char k[48];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(j, k);
    if (!p) return dflt;
    p = strchr(p + strlen(k), ':');
    if (!p) return dflt;
    while (*++p == ' ' || *p == '"') ;
    return (int)strtol(p, NULL, 0);
}

static int netip_create(source_t *s)
{
    netip_t *n = calloc(1, sizeof *n);
    if (!n) return -1;
    const char *path;
    n->port = NETIP_PORT;
    if (url_split(s->url, "netip", n->user, sizeof n->user, n->pass, sizeof n->pass,
                  n->host, sizeof n->host, &n->port, &path) < 0) {
        free(n);
        return -1;
    }
    if (!n->user[0]) snprintf(n->user, sizeof n->user, "admin");
    char v[16];
    n->channel = url_query(path, "ch", v, sizeof v) ? atoi(v) : 0;
    snprintf(n->stream, sizeof n->stream, "Main");
    if (url_query(path, "stream", v, sizeof v) && (v[0] == 'e' || v[0] == 'E' || v[0] == 's'))
        snprintf(n->stream, sizeof n->stream, "Extra");
    n->fd = -1;
    snprintf(s->host, sizeof s->host, "%s", n->host);
    s->priv = n;
    return 0;
}

static void netip_close(source_t *s)
{
    netip_t *n = s->priv;
    if (n && n->fd >= 0) close(n->fd);
    if (n) {
        n->fd = -1;
        n->hdr_got = n->body_got = n->body_len = 0;
        n->frame_len = n->frame_want = 0;
        n->frame_kind = 0;
    }
}

static int netip_open(source_t *s)
{
    netip_t *n = s->priv;
    char hash[9], req[512], reply[4096];
    n->session = 0;
    n->seq = 0;
    n->fd = net_connect(n->host, n->port, 3000, s->err, sizeof s->err);
    if (n->fd < 0) return -1;

    sofia_hash(n->pass, hash);
    snprintf(req, sizeof req,
             "{\"EncryptType\":\"MD5\",\"LoginType\":\"DVRIP-Web\",\"PassWord\":\"%s\",\"UserName\":\"%s\"}",
             hash, n->user);
    if (transact(s, MSG_LOGIN, req, reply, sizeof reply) < 0) goto fail;
    int ret = json_int(reply, "Ret", -1);
    if (ret != 100 && ret != 515) {
        snprintf(s->err, sizeof s->err, "login refused (Ret %d)%s", ret,
                 ret == 203 || ret == 205 ? ": user/password" : "");
        goto fail;
    }
    n->alive = json_int(reply, "AliveInterval", 20);
    if (n->alive <= 0) n->alive = 20;

    char params[192];
    snprintf(params, sizeof params,
             "{\"Channel\":%d,\"CombinMode\":\"NONE\",\"StreamType\":\"%s\",\"TransMode\":\"TCP\"}",
             n->channel, n->stream);
    snprintf(req, sizeof req,
             "{\"Name\":\"OPMonitor\",\"SessionID\":\"0x%08X\",\"OPMonitor\":{\"Action\":\"Claim\",\"Parameter\":%s}}",
             n->session, params);
    if (transact(s, MSG_MONITOR_CLAIM, req, reply, sizeof reply) < 0) goto fail;
    ret = json_int(reply, "Ret", -1);
    if (ret != 100) {
        snprintf(s->err, sizeof s->err, "monitor refused (Ret %d)", ret);
        goto fail;
    }
    snprintf(req, sizeof req,
             "{\"Name\":\"OPMonitor\",\"SessionID\":\"0x%08X\",\"OPMonitor\":{\"Action\":\"Start\",\"Parameter\":%s}}",
             n->session, params);
    if (send_msg(n, MSG_MONITOR, req) < 0) {
        snprintf(s->err, sizeof s->err, "send failed");
        goto fail;
    }
    n->last_ka = time(NULL);
    s->codec = VG_CODEC_NONE;               /* the first I-frame says */
    return 0;
fail:
    netip_close(s);
    return -1;
}

static int frame_reserve(netip_t *n, size_t need)
{
    if (need <= n->frame_cap) return 0;
    if (need > FRAME_MAX) return -1;
    size_t c = n->frame_cap ? n->frame_cap : 256 * 1024;
    while (c < need) c *= 2;
    uint8_t *p = realloc(n->frame, c);
    if (!p) return -1;
    n->frame = p;
    n->frame_cap = c;
    return 0;
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* One media message body: start or continue a frame. */
static void media_chunk(source_t *s, const uint8_t *p, size_t len, au_emit_fn emit, void *user)
{
    netip_t *n = s->priv;
    size_t off = 0;
    if (n->frame_want == 0) {
        if (len < 8) return;
        uint32_t tag = get32be(p);
        size_t want;
        switch (tag) {
        case 0x1FC:
            if (len < 16) return;
            n->codec = p[4] == 3 ? VG_CODEC_H265 : p[4] == 2 ? VG_CODEC_H264 : VG_CODEC_NONE;
            want = get32(p + 12);
            off = 16;
            n->frame_kind = n->codec != VG_CODEC_NONE ? 1 : 3;
            break;
        case 0x1FD:
            want = get32(p + 4);
            off = 8;
            n->frame_kind = n->codec != VG_CODEC_NONE ? 2 : 3;
            break;
        case 0x1FE:
            if (len < 16) return;
            want = get32(p + 12);
            off = 16;
            n->frame_kind = 3;
            break;
        case 0x1FA:
        case 0x1F9:
            want = p[6] | p[7] << 8;
            off = 8;
            n->frame_kind = 3;
            break;
        default:
            return;                         /* lost sync; wait for the next tag */
        }
        if (want == 0 || want > FRAME_MAX) { n->frame_kind = 0; return; }
        n->frame_want = want;
        n->frame_len = 0;
    }
    size_t take = len - off;
    if (take > n->frame_want - n->frame_len) take = n->frame_want - n->frame_len;
    if (n->frame_kind != 3) {
        if (frame_reserve(n, n->frame_len + take) < 0) { n->frame_kind = 3; }
        else memcpy(n->frame + n->frame_len, p + off, take);
    }
    n->frame_len += take;
    if (n->frame_len < n->frame_want) return;

    if (n->frame_kind == 1 || n->frame_kind == 2) {
        s->codec = n->codec;
        vg_au_t au = {
            .codec  = n->codec,
            .data   = n->frame,
            .len    = n->frame_len,
            .pts_us = now_us(),
            .key    = n->frame_kind == 1,
        };
        emit(user, &au);
    }
    n->frame_want = n->frame_len = 0;
    n->frame_kind = 0;
}

static int netip_poll(source_t *s, au_emit_fn emit, void *user)
{
    netip_t *n = s->priv;
    int got = 0;
    time_t now = time(NULL);
    if (now - n->last_ka >= n->alive) {
        char req[128];
        snprintf(req, sizeof req, "{\"Name\":\"KeepAlive\",\"SessionID\":\"0x%08X\"}", n->session);
        send_msg(n, MSG_KEEPALIVE, req);
        n->last_ka = now;
    }
    for (int reads = 0; reads < 64; ++reads) {
        if (n->hdr_got < 20) {
            ssize_t r = recv(n->fd, n->hdr + n->hdr_got, 20 - n->hdr_got, reads ? MSG_DONTWAIT : 0);
            if (r == 0) { snprintf(s->err, sizeof s->err, "connection closed"); return -1; }
            if (r < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return got;
                snprintf(s->err, sizeof s->err, "recv: %s", strerror(errno));
                return -1;
            }
            n->hdr_got += (size_t)r;
            if (n->hdr_got < 20) continue;
            if (n->hdr[0] != 0xff) { snprintf(s->err, sizeof s->err, "protocol error"); return -1; }
            n->body_len = get32(n->hdr + 16);
            n->body_got = 0;
            if (n->body_len > FRAME_MAX) { snprintf(s->err, sizeof s->err, "protocol error"); return -1; }
            if (n->body_len > n->body_cap) {
                uint8_t *b = realloc(n->body, n->body_len);
                if (!b) return -1;
                n->body = b;
                n->body_cap = n->body_len;
            }
        }
        if (n->body_got < n->body_len) {
            ssize_t r = recv(n->fd, n->body + n->body_got, n->body_len - n->body_got, MSG_DONTWAIT);
            if (r == 0) { snprintf(s->err, sizeof s->err, "connection closed"); return -1; }
            if (r < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return got;
                snprintf(s->err, sizeof s->err, "recv: %s", strerror(errno));
                return -1;
            }
            n->body_got += (size_t)r;
            if (n->body_got < n->body_len) continue;
        }
        int msgid = n->hdr[14] | n->hdr[15] << 8;
        if (msgid == MSG_MONITOR_DATA) {
            media_chunk(s, n->body, n->body_len, emit, user);
            got = 1;
        }
        /* Other messages (keepalive replies, alarms) are ignored. */
        n->hdr_got = 0;
        n->body_len = n->body_got = 0;
    }
    return got;
}

static void netip_destroy(source_t *s)
{
    netip_t *n = s->priv;
    if (!n) return;
    free(n->body);
    free(n->frame);
    free(n);
    s->priv = NULL;
}

const source_ops_t source_netip_ops = {
    .scheme  = "netip",
    .create  = netip_create,
    .open    = netip_open,
    .poll    = netip_poll,
    .close   = netip_close,
    .destroy = netip_destroy,
};
