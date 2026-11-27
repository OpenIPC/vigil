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
 *   title     : msgid 1048 {Name ChannelTitle} -> 1049 {Ret, ChannelTitle:
 *               ["CAM01", ...]}, one name per channel (the names the camera
 *               draws on the picture, AVEnc.VideoWidget[].ChannelTitle.Name);
 *               some firmwares list objects with a "Name" instead
 *   live      : msgid 1413 OPMonitor Claim, then 1410 OPMonitor Start;
 *               media then arrives as msgid 1412 messages
 *   media     : a frame starts with a big-endian tag and a header, and its
 *               payload may continue over the following messages:
 *                 0x000001FC  I-frame: u8 codec (2 = H.264, 3 = H.265,
 *                             in the low nibble; see media_chunk),
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

#include "cJSON.h"
#include "log.h"
#include "md5.h"
#include "net.h"
#include "source.h"

#define NETIP_PORT       34567
#define MSG_LOGIN        1000
#define MSG_KEEPALIVE    1006
#define MSG_TITLE        1048       /* config get "ChannelTitle" (python-dvr: ChannelTitle) */
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
    int      title_asked;           /* once per source, not at every reconnect */
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
    int      keyed;                 /* an I-frame header has said the codec */
    int      key_told;              /* this connection's first I-frame was logged */
} netip_t;

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

#define TRANSACT_SECS 10           /* a whole login/claim exchange */
#define TITLE_SECS     3           /* netip_title's wait: nice to have, not worth a long one */

/* Read exactly `len` bytes before `deadline` (1 s socket timeout per
 * read), giving up early when the channel is stopping. Login/claim only.
 * Returns 0, -1 on a broken connection, -2 on timeout or stop. */
static int recv_all(source_t *s, uint8_t *buf, size_t len, time_t deadline)
{
    netip_t *n = s->priv;
    size_t got = 0;
    while (got < len) {
        if ((s->stop && *s->stop) || time(NULL) >= deadline) return -2;
        ssize_t r = recv(n->fd, buf + got, len - got, 0);
        if (r > 0) { got += (size_t)r; continue; }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        return -1;
    }
    return 0;
}

static void take_title(source_t *s, const uint8_t *body, size_t len);

/* Request/response for the login, title and claim steps: the reply must
 * arrive within `secs`, however many unrelated messages come first. */
static int transact(source_t *s, int msgid, const char *json, char *reply, size_t cap, int secs)
{
    netip_t *n = s->priv;
    time_t deadline = time(NULL) + secs;
    if (send_msg(n, msgid, json) < 0) {
        snprintf(s->err, sizeof s->err, "send failed");
        return -1;
    }
    for (;;) {
        uint8_t h[20];
        int rc = recv_all(s, h, 20, deadline);
        if (rc == 0 && h[0] != 0xff) {
            snprintf(s->err, sizeof s->err, "protocol error (msg %d)", msgid);
            return -1;
        }
        uint32_t len = rc == 0 ? get32(h + 16) : 0;
        if (len > 64 * 1024) { snprintf(s->err, sizeof s->err, "bad reply"); return -1; }
        uint8_t *b = rc == 0 ? malloc(len + 1) : NULL;
        if (rc == 0 && !b) { snprintf(s->err, sizeof s->err, "out of memory"); return -1; }
        if (rc == 0) rc = recv_all(s, b, len, deadline);
        if (rc < 0) {
            free(b);
            snprintf(s->err, sizeof s->err, rc == -2 ? "no reply to msg %d within %d s" : "connection closed (msg %d)",
                     msgid, secs);
            return rc == -2 ? -2 : -1;
        }
        b[len] = 0;
        int rid = h[14] | h[15] << 8;
        /* Any reply ends a claim (python-dvr does not look either), except
         * the title's, asked just before it. */
        if (rid == msgid + 1 || (msgid == MSG_MONITOR_CLAIM && rid != MSG_TITLE + 1)) {
            n->session = get32(h + 4);
            snprintf(reply, cap, "%s", (char *)b);
            free(b);
            return 0;
        }
        if (rid == MSG_TITLE + 1) take_title(s, b, len);    /* asked without waiting */
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

/* Keep a name to printable text that fits: control characters go, and
 * bytes that are not UTF-8 (GB2312 from old firmwares) become '?', so the
 * name can be drawn and saved into the YAML file. Cut on a character. */
static void clean_name(const char *in, char *out, size_t cap)
{
    size_t k = 0;
    while (*in == ' ') ++in;
    for (const unsigned char *p = (const unsigned char *)in; *p && k + 1 < cap;) {
        size_t len = *p < 0x80 ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : (*p & 0xF8) == 0xF0 ? 4 : 0;
        for (size_t i = 1; i < len; ++i)
            if ((p[i] & 0xC0) != 0x80) len = 0;
        if (len == 0) {
            out[k++] = '?';
            ++p;
            continue;
        }
        if (k + len >= cap) break;
        if (len > 1 || *p >= 0x20) {
            memcpy(out + k, p, len);
            k += len;
        }
        p += len;
    }
    while (k && out[k - 1] == ' ') --k;
    out[k] = 0;
}

int netip_title_parse(const char *reply, int ch, char *out, size_t cap)
{
    if (cap) out[0] = 0;
    cJSON *doc = cJSON_Parse(reply);
    if (!doc) return -1;
    int rc = -1;
    const cJSON *ret = cJSON_GetObjectItemCaseSensitive(doc, "Ret");
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(doc, "ChannelTitle");
    /* A device with one channel may answer with the one entry alone. */
    const cJSON *it = cJSON_IsArray(list) ? cJSON_GetArrayItem(list, ch) : ch == 0 ? list : NULL;
    if (cJSON_IsObject(it)) {
        const cJSON *nm = cJSON_GetObjectItemCaseSensitive(it, "Name");
        if (!cJSON_IsString(nm)) nm = cJSON_GetObjectItemCaseSensitive(it, "ChannelTitle");
        it = nm;
    }
    if ((!ret || (cJSON_IsNumber(ret) && ret->valueint == 100)) && cJSON_IsString(it)) {
        clean_name(it->valuestring, out, cap);
        rc = 0;
    }
    cJSON_Delete(doc);
    return rc;
}

/* Connect and log in. */
static int netip_login(source_t *s)
{
    netip_t *n = s->priv;
    char hash[9], req[256], reply[4096];
    n->session = 0;
    n->seq = 0;
    s->fail = SRC_FAIL_OTHER;
    n->fd = net_connect(n->host, n->port, 3000, s->stop, s->err, sizeof s->err);
    if (n->fd < 0) {
        s->fail = SRC_FAIL_UNREACHABLE;
        return -1;
    }
    md5_sofia(n->pass, hash);
    snprintf(req, sizeof req,
             "{\"EncryptType\":\"MD5\",\"LoginType\":\"DVRIP-Web\",\"PassWord\":\"%s\",\"UserName\":\"%s\"}",
             hash, n->user);
    if (transact(s, MSG_LOGIN, req, reply, sizeof reply, TRANSACT_SECS) < 0) return -1;
    int ret = json_int(reply, "Ret", -1);
    if (ret != 100 && ret != 515) {
        /* 203 wrong password, 205 no such user, 206 locked out */
        int auth = ret == 203 || ret == 205 || ret == 206;
        snprintf(s->err, sizeof s->err, "login refused (Ret %d)%s", ret, auth ? ": user/password" : "");
        if (auth) s->fail = SRC_FAIL_AUTH;
        return -1;
    }
    n->alive = json_int(reply, "AliveInterval", 20);
    if (n->alive <= 0) n->alive = 20;
    return 0;
}

/* A ChannelTitle reply: this channel's name into s->title. */
static void take_title(source_t *s, const uint8_t *body, size_t len)
{
    netip_t *n = s->priv;
    char *j = malloc(len + 1), name[sizeof s->title];
    if (!j) return;
    memcpy(j, body, len);
    j[len] = 0;
    if (netip_title_parse(j, n->channel, name, sizeof name) == 0 && name[0])
        snprintf(s->title, sizeof s->title, "%s", name);
    free(j);
}

static void title_request(netip_t *n, char *req, size_t cap)
{
    n->title_asked = 1;
    snprintf(req, cap, "{\"Name\":\"ChannelTitle\",\"SessionID\":\"0x%08X\"}", n->session);
}

/* Log in, ask for the title and wait for it (TITLE_SECS), hang up. A
 * camera that does not answer leaves s->title "" and is not an error. */
static int netip_title(source_t *s)
{
    netip_t *n = s->priv;
    char reply[4096];
    int rc = netip_login(s);
    if (rc == 0) {
        char req[128], err[sizeof s->err];
        title_request(n, req, sizeof req);
        snprintf(err, sizeof err, "%s", s->err);
        rc = transact(s, MSG_TITLE, req, reply, sizeof reply, TITLE_SECS);
        if (rc == 0) take_title(s, (const uint8_t *)reply, strlen(reply));
        else if (rc == -2) snprintf(s->err, sizeof s->err, "%s", err);
        rc = rc == -1 ? -1 : 0;
    }
    if (n->fd >= 0) close(n->fd);
    n->fd = -1;
    return rc;
}

static int netip_open(source_t *s)
{
    netip_t *n = s->priv;
    char req[512], reply[4096];
    if (netip_login(s) < 0) goto fail;
    /* The channel's name, without waiting for it: the reply is taken
     * wherever it comes (transact, poll). */
    if (!n->title_asked) {
        title_request(n, req, sizeof req);
        if (send_msg(n, MSG_TITLE, req) < 0) {
            snprintf(s->err, sizeof s->err, "send failed");
            goto fail;
        }
    }
    n->key_told = 0;

    char params[192];
    snprintf(params, sizeof params,
             "{\"Channel\":%d,\"CombinMode\":\"NONE\",\"StreamType\":\"%s\",\"TransMode\":\"TCP\"}",
             n->channel, n->stream);
    snprintf(req, sizeof req,
             "{\"Name\":\"OPMonitor\",\"SessionID\":\"0x%08X\",\"OPMonitor\":{\"Action\":\"Claim\",\"Parameter\":%s}}",
             n->session, params);
    if (transact(s, MSG_MONITOR_CLAIM, req, reply, sizeof reply, TRANSACT_SECS) < 0) goto fail;
    int ret = json_int(reply, "Ret", -1);
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

/* XM's I-frame header keeps the codec in the low nibble of its byte: the
 * high bits are bits 8-9 of width/8 (4-5) and of height/8 (6-7), for
 * pictures 2048 or more wide or high (Sofia's own stream parser reads them
 * so). A 2560x1440 H.265 main stream says 0x13, a 3840x2160 one 0x53. */
static vg_codec_t xm_codec(uint8_t b)
{
    return (b & 0x0F) == 3 ? VG_CODEC_H265 : (b & 0x0F) == 2 ? VG_CODEC_H264 : VG_CODEC_NONE;
}

/* The codec of a picture with no header to say it (a P-frame before the
 * first I-frame), from its first NAL unit: an H.265 one has the layer and
 * temporal id 0/1 in its second byte, and the forbidden and layer bits of
 * the first clear; H.264 slices (types 1, 5) have bit 0 of the first set. */
static vg_codec_t sniff_codec(const uint8_t *p, size_t len)
{
    size_t pos = 0, nl = 0;
    const uint8_t *nal = au_next_nal(p, len, &pos, &nl);
    if (!nal || nl < 2) return VG_CODEC_NONE;
    return (nal[0] & 0x81) == 0 && nal[1] == 0x01 ? VG_CODEC_H265 : VG_CODEC_H264;
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
            n->codec = xm_codec(p[4]);
            n->keyed = 1;
            /* What a camera sends is the first thing to know when its
             * picture does not come: said once per connection. */
            if (!n->key_told) {
                n->key_told = 1;
                vg_log("netip %s ch%d %s: I-frame, codec byte 0x%02X (%s), %ux%u, %u fps", n->host, n->channel,
                       n->stream, p[4], n->codec != VG_CODEC_NONE ? vg_codec_name(n->codec) : "not H.264/H.265",
                       (p[6] | (p[4] >> 4 & 3u) << 8) * 8u, (p[7] | (unsigned)(p[4] >> 6) << 8) * 8u, p[5] & 0x1Fu);
            }
            want = get32(p + 12);
            off = 16;
            n->frame_kind = n->codec != VG_CODEC_NONE ? 1 : 3;
            break;
        case 0x1FD:
            want = get32(p + 4);
            off = 8;
            /* Before the first I-frame: still video, so it holds off the
             * no-data reconnect (au_norm drops it). A camera with a GOP
             * longer than that would otherwise be dropped before its first
             * I-frame, again and again. */
            if (!n->keyed) n->codec = sniff_codec(p + off, len - off);
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

void netip_media_feed(source_t *s, const uint8_t *body, size_t len, au_emit_fn emit, void *user)
{
    media_chunk(s, body, len, emit, user);
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
    /* The first read of each call waits (1 s socket timeout), whether it is
     * for a header or for the rest of a body; later ones only drain what is
     * already there. Never a busy loop. */
    for (int reads = 0; reads < 64; ++reads) {
        int flags = reads ? MSG_DONTWAIT : 0;
        if (n->hdr_got < 20) {
            ssize_t r = recv(n->fd, n->hdr + n->hdr_got, 20 - n->hdr_got, flags);
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
            ssize_t r = recv(n->fd, n->body + n->body_got, n->body_len - n->body_got, flags);
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
        } else if (msgid == MSG_TITLE + 1) {
            take_title(s, n->body, n->body_len);
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

/* Append `s` (n bytes) to out[*len]; -1 if it does not fit. */
static int put(char *out, size_t cap, size_t *len, const char *s, size_t n)
{
    if (*len + n >= cap) return -1;
    memcpy(out + *len, s, n);
    *len += n;
    out[*len] = 0;
    return 0;
}

/* Xiongmai's sub-stream is the same channel with stream=extra: the URL
 * with that instead of stream=main (or no stream). */
static int netip_sub_url(const char *url, char *out, size_t cap)
{
    char user[64], pass[64], host[128], v[16];
    int port = NETIP_PORT;
    const char *path;
    if (url_split(url, "netip", user, sizeof user, pass, sizeof pass, host, sizeof host, &port,
                  &path) < 0)
        return -1;
    if (url_query(path, "stream", v, sizeof v) && (v[0] == 'e' || v[0] == 'E' || v[0] == 's'))
        return -1;                          /* the sub-stream already */
    const char *q = strchr(path, '?');
    size_t len = 0;
    if (put(out, cap, &len, url, (size_t)((q ? q : path + strlen(path)) - url)) < 0 ||
        (!*path && put(out, cap, &len, "/", 1) < 0) || put(out, cap, &len, "?", 1) < 0)
        return -1;
    for (q = q ? q + 1 : NULL; q && *q; ) {
        size_t n = strcspn(q, "&");
        if (n && strncmp(q, "stream=", 7) != 0 &&
            (put(out, cap, &len, q, n) < 0 || put(out, cap, &len, "&", 1) < 0))
            return -1;
        q += n + (q[n] == '&');
    }
    return put(out, cap, &len, "stream=extra", 12);
}

const source_ops_t source_netip_ops = {
    .scheme  = "netip",
    .create  = netip_create,
    .open    = netip_open,
    .poll    = netip_poll,
    .close   = netip_close,
    .destroy = netip_destroy,
    .sub_url = netip_sub_url,
    .title   = netip_title,
};
