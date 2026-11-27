/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rtsp_server.c — the cameras' streams over RTSP (see rtsp_server.h).
 *
 * Each stream being served has a tap (stream_tap.h) and a ring of RTP
 * packets, written by the camera's session thread: an AU is packetized
 * there once (RFC 6184 FU-A, RFC 7798 FU), whatever the number of
 * clients, with one sequence number and SSRC per stream. A client is a
 * position in that ring; the server thread copies the packets from its
 * position into the client's output (framed for interleaved TCP, or sent
 * as UDP datagrams) as fast as the client takes them. A client the ring
 * overtakes (it stopped reading, or its link is slower than the camera)
 * has lost packets anyway: it skips to the next keyframe, and its RTP
 * sequence numbers show the gap.
 *
 * A new client starts at the newest keyframe in the ring; on a stream
 * not yet served, the tap hands over the camera's GOP so far first, so
 * the picture is there at once either way.
 *
 * The RTSP part is what VMSs and players use: OPTIONS, DESCRIBE, SETUP,
 * PLAY, PAUSE, TEARDOWN, GET_PARAMETER/SET_PARAMETER as keepalives; one
 * session (one video track) per connection, ended with it.
 */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "auth.h"
#include "cJSON.h"
#include "channel.h"
#include "config.h"
#include "events.h"
#include "log.h"
#include "md5.h"
#include "qos.h"
#include "rtsp_server.h"
#include "sha1.h"
#include "stream_tap.h"
#include "users.h"

#define CONN_MAX        33          /* connections; rtsp_server.max_clients streams among them */
#define IN_CAP          4096        /* a request with its headers */
#define OUT_CAP         (64u << 10) /* a client's output, replies and packets */
#define OUT_REPLY       4096        /* of it kept for replies */
#define RTP_PAYLOAD     1400        /* fits a 1500-byte Ethernet MTU with IP/UDP/RTP */
/* A ring holds a GOP for the next client to start from, and is how far
 * a client may fall behind before it skips: it doubles at a keyframe
 * when the GOP before did not fit, up to its cap (an 8 Mbit/s main
 * stream with a 4 s GOP is 4 MiB; sub-streams are a fraction). */
#define RING_MAIN       (2u << 20)
#define RING_MAIN_MAX   (4u << 20)
#define RING_SUB        (512u << 10)
#define RING_SUB_MAX    (1u << 20)
/* UDP has no flow control: a GOP sent at once to a new client overflows
 * socket buffers on the way, so a UDP client gets at most this much a
 * turn, turns UDP_TURN_MS apart while it is behind (about 64 Mbit/s). */
#define UDP_TURN        (32u << 10)
#define UDP_TURN_MS     4
#define SESSION_SECS    60          /* RTP/UDP without a request or RTCP for this long: gone */
#define IDLE_SECS       60          /* a connection without a session */
#define STALL_SECS      20          /* a TCP client that takes no data */
#define LINGER_SECS     10          /* a stream nobody watches stays tapped (a client back at once) */
#define SR_SECS         5           /* RTCP sender reports */
#define UDP_PORT_FIRST  6970
#define FAIL_ADDRS      16
#define FAIL_FREE       5           /* failed logins in a row before an address waits */
#define LOCKOUT_MAX     300
#define PT              96

/* Ring records: [len lo, len hi, flags, 0] + an RTP packet, padded to 4
 * bytes; len 0 marks the rest of the ring unused (the next record is at
 * its start). */
enum { R_AU = 1, R_KEY = 2 };
#define REC_SIZE(len)   ((4 + (size_t)(len) + 3) & ~(size_t)3)

typedef struct {
    int             used;
    int             ch, sub;
    stream_tap_t   *tap;
    pthread_mutex_t lock;           /* the ring, with the session thread */
    uint8_t        *ring;
    size_t          size;
    uint64_t        head, tail;     /* virtual offsets: [tail, head) is valid */
    uint64_t        key;            /* where the newest keyframe starts */
    uint64_t        moved_from, moved_to;   /* the ring's last growth: its old head, new start */
    int             have_key;
    uint16_t        seq;
    uint32_t        ssrc;
    vg_codec_t      codec;          /* what its SDP said */
    int             codec_changed;
    int             have_ts;
    uint64_t        pts_ref, last_pts;
    uint32_t        ts_ref, last_ts;
    uint64_t        last_at;        /* monotonic us of the last AU */
    /* The server thread's own. */
    int             clients;
    time_t          idle_since;
} rs_stream_t;

typedef struct {
    int             fd;
    char            addr[48];
    struct sockaddr_in peer;
    uint8_t         in[IN_CAP];
    size_t          in_len;
    uint8_t        *out;
    size_t          out_off, out_len;
    time_t          last_rx, last_progress, since;
    char            nonce[33];
    int             authed;
    char            user[USERS_NAME_MAX];
    int             close_after;    /* when the output is out */
    /* The session. */
    char            session[17];
    rs_stream_t    *st;
    int             ch, sub;
    int             playing;
    int             tcp;
    int             ich;            /* interleaved channel of RTP (RTCP: + 1) */
    uint16_t        cport, cport_rtcp;
    uint64_t        pos;
    int             wait_key;
    uint64_t        wait_from;
    uint32_t        pkts, octets, last_ts;
    time_t          sr_at, play_at;
    unsigned        drops;
    uint64_t        bytes;
} rs_conn_t;

typedef struct {
    char   addr[48];
    int    fails;
    time_t locked_until, last;
} rs_fail_t;

static rs_stream_t     g_streams[CHANNEL_MAX * 2];
static rs_conn_t      *g_conns[CONN_MAX];
static rs_fail_t       g_fails[FAIL_ADDRS];
static pthread_t       g_thread;
static int             g_up;
static volatile int    g_stop;
static int             g_lfd = -1, g_udp[2] = { -1, -1 }, g_wake[2] = { -1, -1 };
static int             g_udp_port;
static atomic_int      g_wake_pending;
static int             g_port, g_enabled;          /* what runs */
static int             g_auth = 1, g_max = 8;      /* read as the configuration changes */
static unsigned        g_cfg_gen;
static unsigned char   g_key[16];
static unsigned        g_nonce_n;
static uint8_t         g_udp_buf[UDP_TURN];
static int             g_behind;   /* a UDP client has more to send now */

static time_t mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static uint64_t mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void wake(void)
{
    if (!atomic_exchange(&g_wake_pending, 1) && g_wake[1] >= 0) {
        ssize_t r = write(g_wake[1], "", 1);
        (void)r;
    }
}

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* ---- the ring and the packetizer (session threads, under st->lock) ----------- */

/* Make room for `n` more bytes at head: the oldest records go. */
static void ring_room(rs_stream_t *st, size_t n)
{
    while (st->head + n - st->tail > st->size) {
        size_t phys = (size_t)(st->tail % st->size);
        const uint8_t *r = st->ring + phys;
        size_t len = r[0] | (size_t)r[1] << 8;
        st->tail += len ? REC_SIZE(len) : st->size - phys;
    }
}

/* A record of `pkt` bytes at head; its virtual offset in *at. */
static uint8_t *ring_put(rs_stream_t *st, size_t pkt, unsigned flags, uint64_t *at)
{
    size_t need = REC_SIZE(pkt), phys = (size_t)(st->head % st->size);
    if (phys + need > st->size) {
        size_t skip = st->size - phys;
        ring_room(st, skip);
        st->ring[phys] = st->ring[phys + 1] = 0;
        st->head += skip;
        phys = 0;
    }
    ring_room(st, need);
    uint8_t *r = st->ring + phys;
    r[0] = (uint8_t)pkt;
    r[1] = (uint8_t)(pkt >> 8);
    r[2] = (uint8_t)flags;
    r[3] = 0;
    *at = st->head;
    st->head += need;
    return r + 4;
}

/* At a keyframe: when the GOP before it did not fit (its keyframe was
 * overwritten), a ring twice the size, starting with this keyframe. The
 * clients' positions are all before it: they go on from it. */
static void ring_fit(rs_stream_t *st)
{
    size_t cap = st->sub ? RING_SUB_MAX : RING_MAIN_MAX;
    if (!st->have_key || st->key >= st->tail || st->size >= cap) return;
    size_t size = st->size * 2 < cap ? st->size * 2 : cap;
    uint8_t *ring = malloc(size);
    if (!ring) return;
    free(st->ring);
    st->ring = ring;
    st->moved_from = st->head;
    st->head = st->tail = st->moved_to = (st->head + size - 1) / size * size;
    st->size = size;
    st->have_key = 0;
}

/* One RTP packet: `pre` (a FU's headers) then `data`. */
static void rtp_put(rs_stream_t *st, uint32_t ts, int marker, unsigned flags, const uint8_t *pre,
                    size_t pre_len, const uint8_t *data, size_t len)
{
    uint64_t at;
    uint8_t *p = ring_put(st, 12 + pre_len + len, flags, &at);
    p[0] = 0x80;
    p[1] = (uint8_t)((marker ? 0x80 : 0) | PT);
    put16(p + 2, st->seq++);
    put32(p + 4, ts);
    put32(p + 8, st->ssrc);
    memcpy(p + 12, pre, pre_len);
    memcpy(p + 12 + pre_len, data, len);
    if (flags & R_KEY) {
        st->key = at;
        st->have_key = 1;
    }
}

/* One NAL unit (no start code): as it is, or in fragments. */
static void nal_put(rs_stream_t *st, uint32_t ts, int marker, unsigned *flags, const uint8_t *nal, size_t len)
{
    if (len <= RTP_PAYLOAD) {
        rtp_put(st, ts, marker, *flags, NULL, 0, nal, len);
        *flags = 0;
        return;
    }
    uint8_t pre[3];
    size_t hl, skip;
    if (st->codec == VG_CODEC_H265) {           /* RFC 7798 4.4.3: PayloadHdr type 49, FU header */
        pre[0] = (uint8_t)((nal[0] & 0x81) | 49 << 1);
        pre[1] = nal[1];
        pre[2] = (nal[0] >> 1) & 0x3f;
        hl = 3;
        skip = 2;
    } else {                                    /* RFC 6184 5.8: FU-A */
        pre[0] = (uint8_t)((nal[0] & 0xe0) | 28);
        pre[1] = nal[0] & 0x1f;
        hl = 2;
        skip = 1;
    }
    uint8_t type = pre[hl - 1];
    const uint8_t *p = nal + skip;
    size_t left = len - skip, chunk = RTP_PAYLOAD - hl;
    int first = 1;
    while (left) {
        size_t n = left < chunk ? left : chunk;
        int last = n == left;
        pre[hl - 1] = (uint8_t)(type | (first ? 0x80 : 0) | (last ? 0x40 : 0));
        rtp_put(st, ts, marker && last, *flags, pre, hl, p, n);
        *flags = 0;
        p += n;
        left -= n;
        first = 0;
    }
}

/* The byte after the next start code at or after i, or len. */
static size_t after_sc(const uint8_t *b, size_t len, size_t i)
{
    while (i + 3 <= len) {
        if (b[i + 2] > 1) i += 3;
        else if (b[i + 2] == 1 && b[i] == 0 && b[i + 1] == 0) return i + 3;
        else ++i;
    }
    return len;
}

/* The AU's RTP timestamp: 90 kHz from its pts, going on smoothly over a
 * jump (a reconnect, a camera whose clock restarts). */
static uint32_t rtp_ts(rs_stream_t *st, const vg_au_t *au)
{
    uint64_t now = mono_us(), p = au->pts_us ? au->pts_us : now;
    if (!st->have_ts) {
        st->have_ts = 1;
        st->pts_ref = p;
        st->ts_ref = st->ssrc * 2654435761u;
    } else if (p < st->last_pts || p - st->last_pts > 10000000u) {
        st->ts_ref = st->last_ts + 3600;
        st->pts_ref = p;
    }
    st->last_pts = p;
    st->last_ts = st->ts_ref + (uint32_t)((p - st->pts_ref) * 9 / 100);
    st->last_at = now;
    return st->last_ts;
}

static void on_au(void *user, const vg_au_t *au, const vg_stream_info_t *si, vg_codec_t codec)
{
    (void)si;
    rs_stream_t *st = user;
    pthread_mutex_lock(&st->lock);
    if (codec != st->codec) {
        st->codec_changed = 1;
    } else {
        if (au->key) ring_fit(st);
        uint32_t ts = rtp_ts(st, au);
        unsigned flags = R_AU | (au->key ? R_KEY : 0);
        const uint8_t *b = au->data, *pend = NULL;
        size_t len = au->len, i = after_sc(b, len, 0), pend_len = 0;
        while (i < len) {
            size_t next = after_sc(b, len, i), end = next < len ? next - 3 : len;
            while (end > i && b[end - 1] == 0) --end;   /* a 4-byte start code's zero */
            if (end - i >= 2) {
                if (pend) nal_put(st, ts, 0, &flags, pend, pend_len);
                pend = b + i;
                pend_len = end - i;
            }
            i = next;
        }
        if (pend) nal_put(st, ts, 1, &flags, pend, pend_len);
    }
    pthread_mutex_unlock(&st->lock);
    wake();
}

/* ---- streams (server thread) ---------------------------------------------------- */

static rs_stream_t *stream_get(int ch, int sub, vg_codec_t codec)
{
    rs_stream_t *free_st = NULL;
    for (int i = 0; i < CHANNEL_MAX * 2; ++i) {
        rs_stream_t *st = &g_streams[i];
        if (st->used && st->ch == ch && st->sub == sub && !st->codec_changed && st->codec == codec) return st;
        if (!st->used && !free_st) free_st = st;
    }
    if (!free_st) return NULL;
    rs_stream_t *st = free_st;
    size_t size = sub ? RING_SUB : RING_MAIN;
    uint8_t *ring = malloc(size);
    if (!ring) return NULL;
    pthread_mutex_lock(&st->lock);
    st->ring = ring;
    st->size = size;
    st->head = st->tail = st->key = st->moved_from = st->moved_to = 0;
    st->have_key = st->have_ts = st->codec_changed = 0;
    st->ch = ch;
    st->sub = sub;
    st->codec = codec;
    uint8_t r[16];
    md5_raw(g_key, sizeof g_key, r);
    st->ssrc = (uint32_t)r[0] << 24 ^ (uint32_t)r[1] << 16 ^ (uint32_t)r[2] << 8 ^ r[3] ^ (uint32_t)(ch * 2 + sub) ^
               (uint32_t)mono_us();
    st->seq = (uint16_t)(st->ssrc >> 7);
    pthread_mutex_unlock(&st->lock);
    st->tap = stream_tap_open(ch, sub, on_au, st);
    if (!st->tap) {
        free(ring);
        st->ring = NULL;
        return NULL;
    }
    st->used = 1;
    st->clients = 0;
    st->idle_since = mono();
    return st;
}

static void stream_drop(rs_stream_t *st)
{
    stream_tap_close(st->tap);              /* on_au is not called after this */
    st->tap = NULL;
    free(st->ring);
    st->ring = NULL;
    st->used = 0;
}

/* ---- connections ------------------------------------------------------------------- */

static int open_streams(void)
{
    int n = 0;
    for (int i = 0; i < CHANNEL_MAX * 2; ++i) n += g_streams[i].used;
    return n;
}

static int sessions(void)
{
    int n = 0;
    for (int i = 0; i < CONN_MAX; ++i) n += g_conns[i] && g_conns[i]->st;
    return n;
}

static const char *stream_name(int sub) { return sub ? "sub" : "main"; }

static void session_end(rs_conn_t *c, const char *why)
{
    if (!c->st) return;
    if (c->play_at)
        vg_log("rtsp: %s%s%s CH%d %s: %s after %ld s, %llu KiB, skipped to a keyframe %u times", c->addr,
               c->user[0] ? " " : "", c->user, c->ch + 1, stream_name(c->sub), why, (long)(mono() - c->play_at),
               (unsigned long long)(c->bytes >> 10), c->drops);
    rs_stream_t *st = c->st;
    if (c->tcp && c->play_at) qos_mark(c->fd, QOS_MANAGEMENT, 1);
    c->st = NULL;
    c->playing = 0;
    c->session[0] = 0;
    c->play_at = 0;
    c->bytes = 0;
    c->drops = 0;
    if (--st->clients == 0) {
        st->idle_since = mono();
        /* Lingering costs a tap and its ring: only while within the limit. */
        if (open_streams() > g_max) stream_drop(st);
    }
}

static void conn_close(int i, const char *why)
{
    rs_conn_t *c = g_conns[i];
    if (!c) return;
    session_end(c, why);
    qos_forget(c->fd);
    close(c->fd);
    free(c->out);
    free(c);
    g_conns[i] = NULL;
}

static void make_nonce(char out[33])
{
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%.16s%u:%llu", (const char *)g_key, ++g_nonce_n,
                     (unsigned long long)mono_us());
    md5_hex(buf, (size_t)n, out);
}

static void conn_accept(void)
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int fd = accept(g_lfd, (struct sockaddr *)&a, &al);
    if (fd < 0) return;
    int slot = -1;
    for (int i = 0; i < CONN_MAX && slot < 0; ++i)
        if (!g_conns[i]) slot = i;
    rs_conn_t *c = slot < 0 ? NULL : calloc(1, sizeof *c);
    uint8_t *out = c ? malloc(OUT_CAP) : NULL;
    if (!out) {
        free(c);
        close(fd);
        return;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    c->fd = fd;
    c->out = out;
    c->peer = a;
    inet_ntop(AF_INET, &a.sin_addr, c->addr, sizeof c->addr);
    c->last_rx = c->last_progress = c->since = mono();
    make_nonce(c->nonce);
    g_conns[slot] = c;
}

/* Send what the output holds. -1: the connection is gone. */
static int conn_flush(rs_conn_t *c)
{
    while (c->out_off < c->out_len) {
        ssize_t n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) {
            c->out_off += (size_t)n;
            c->last_progress = mono();
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
        return -1;
    }
    if (c->out_off == c->out_len) {
        c->out_off = c->out_len = 0;
        if (c->close_after) return -1;
    } else if (c->out_off > OUT_CAP / 2) {
        memmove(c->out, c->out + c->out_off, c->out_len - c->out_off);
        c->out_len -= c->out_off;
        c->out_off = 0;
    }
    return 0;
}

static void out_add(rs_conn_t *c, const void *data, size_t len)
{
    if (c->out_len + len > OUT_CAP) {           /* a reply that does not fit: the client is stuck */
        c->close_after = 1;
        return;
    }
    memcpy(c->out + c->out_len, data, len);
    c->out_len += len;
}

/* ---- media (server thread) ------------------------------------------------------- */

/* Copy the client's next packets into `buf` (up to `cap`), framed for
 * interleaved TCP ($, channel, length) or for the UDP loop (length).
 * Returns the bytes added. */
static size_t media_fill(rs_conn_t *c, uint8_t *buf, size_t cap)
{
    rs_stream_t *st = c->st;
    size_t used = 0, hl = c->tcp ? 4 : 2;
    pthread_mutex_lock(&st->lock);
    if (c->pos == st->moved_from) c->pos = st->moved_to;     /* up to date when it grew */
    if (!c->wait_key && c->pos < st->tail) {
        /* Overtaken: what it missed is gone; on at the next keyframe
         * still in the ring, or the next to come. */
        ++c->drops;
        c->wait_key = 1;
        c->wait_from = c->pos;
    }
    if (c->wait_key) {
        if (st->have_key && st->key >= st->tail && st->key >= c->wait_from) {
            c->pos = st->key;
            c->wait_key = 0;
        } else {
            pthread_mutex_unlock(&st->lock);
            return 0;
        }
    }
    while (c->pos < st->head) {
        size_t phys = (size_t)(c->pos % st->size);
        const uint8_t *r = st->ring + phys;
        size_t len = r[0] | (size_t)r[1] << 8;
        if (!len) {
            c->pos += st->size - phys;
            continue;
        }
        if (used + hl + len > cap) break;
        uint8_t *o = buf + used;
        if (c->tcp) {
            o[0] = '$';
            o[1] = (uint8_t)c->ich;
            put16(o + 2, (unsigned)len);
        } else {
            put16(o, (unsigned)len);
        }
        memcpy(o + hl, r + 4, len);
        c->last_ts = (uint32_t)r[8] << 24 | (uint32_t)r[9] << 16 | (uint32_t)r[10] << 8 | r[11];
        used += hl + len;
        c->pos += REC_SIZE(len);
        ++c->pkts;
        c->octets += (uint32_t)(len - 12);
    }
    pthread_mutex_unlock(&st->lock);
    c->bytes += used;
    return used;
}

static void udp_send(rs_conn_t *c, const uint8_t *buf, size_t len, int rtcp)
{
    struct sockaddr_in to = c->peer;
    to.sin_port = htons(rtcp ? c->cport_rtcp : c->cport);
    sendto(g_udp[rtcp], buf, len, MSG_DONTWAIT, (struct sockaddr *)&to, sizeof to);
}

/* Send the client what it can take now. -1: the connection is gone. */
static int media_pump(rs_conn_t *c)
{
    if (!c->playing || !c->st) return conn_flush(c);
    if (c->tcp) {
        size_t cap = OUT_CAP - OUT_REPLY;
        for (int round = 0; round < 8; ++round) {
            size_t n = c->out_len < cap ? media_fill(c, c->out + c->out_len, cap - c->out_len) : 0;
            c->out_len += n;
            if (conn_flush(c) < 0) return -1;
            if (!n || c->out_len) break;        /* caught up, or the socket is full */
        }
        return 0;
    }
    size_t n = media_fill(c, g_udp_buf, sizeof g_udp_buf);
    for (size_t o = 0; o + 2 <= n;) {
        size_t l = (size_t)g_udp_buf[o] << 8 | g_udp_buf[o + 1];
        udp_send(c, g_udp_buf + o + 2, l, 0);
        o += 2 + l;
    }
    pthread_mutex_lock(&c->st->lock);
    if (!c->wait_key && c->pos < c->st->head) g_behind = 1;
    pthread_mutex_unlock(&c->st->lock);
    return conn_flush(c);
}

/* An RTCP sender report with the CNAME (RFC 3550 6.4.1, 6.5.1). */
static void send_sr(rs_conn_t *c)
{
    rs_stream_t *st = c->st;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    pthread_mutex_lock(&st->lock);
    uint32_t ts = st->last_ts + (uint32_t)((mono_us() - st->last_at) * 9 / 100);
    uint32_t ssrc = st->ssrc;
    pthread_mutex_unlock(&st->lock);
    uint8_t p[4 + 28 + 20];
    uint8_t *sr = p + 4;
    sr[0] = 0x80;
    sr[1] = 200;
    put16(sr + 2, 6);
    put32(sr + 4, ssrc);
    put32(sr + 8, (uint32_t)now.tv_sec + 2208988800u);
    put32(sr + 12, (uint32_t)(((uint64_t)now.tv_nsec << 32) / 1000000000u));
    put32(sr + 16, ts);
    put32(sr + 20, c->pkts);
    put32(sr + 24, c->octets);
    uint8_t *sd = sr + 28;                      /* SDES: CNAME "vigil", padded to a word */
    sd[0] = 0x81;
    sd[1] = 202;
    put16(sd + 2, 4);
    put32(sd + 4, ssrc);
    sd[8] = 1;
    sd[9] = 5;
    memcpy(sd + 10, "vigil", 5);
    memset(sd + 15, 0, 5);
    size_t len = 28 + 20;
    if (c->tcp) {
        if (c->out_len + 4 + len > OUT_CAP - OUT_REPLY) return;     /* behind anyway */
        p[0] = '$';
        p[1] = (uint8_t)(c->ich + 1);
        put16(p + 2, (unsigned)len);
        out_add(c, p, 4 + len);
    } else {
        udp_send(c, sr, len, 1);
    }
}

/* ---- RTSP requests --------------------------------------------------------------- */

/* Header `name`'s value in a request's header block, or NULL. */
static const char *header(const char *req, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    for (const char *p = strstr(req, "\r\n"); p; p = strstr(p, "\r\n")) {
        p += 2;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') ++v;
            size_t n = strcspn(v, "\r\n");
            if (n >= cap) n = cap - 1;
            memcpy(out, v, n);
            out[n] = 0;
            return out;
        }
    }
    return NULL;
}

/* key=value or key="value" of an Authorization or Transport header. */
static int param(const char *h, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    for (const char *p = h; (p = strstr(p, key)) != NULL; p += kl) {
        if (p > h && p[-1] != ' ' && p[-1] != ',' && p[-1] != ';') continue;
        if (p[kl] != '=') continue;
        const char *v = p + kl + 1;
        size_t n;
        if (*v == '"') {
            ++v;
            n = strcspn(v, "\"");
        } else {
            n = strcspn(v, ",; \r\n");
        }
        if (n >= cap) return -1;
        memcpy(out, v, n);
        out[n] = 0;
        return 0;
    }
    return -1;
}

void rtsp_server_digest(const char *ha1, const char *nonce, const char *method, const char *uri,
                        const char *nc, const char *cnonce, const char *qop, char out[33])
{
    char buf[1024], ha2[33];
    int n = snprintf(buf, sizeof buf, "%s:%s", method, uri);
    md5_hex(buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1, ha2);
    if (qop && *qop)
        n = snprintf(buf, sizeof buf, "%s:%s:%s:%s:%s:%s", ha1, nonce, nc, cnonce, qop, ha2);
    else
        n = snprintf(buf, sizeof buf, "%s:%s:%s", ha1, nonce, ha2);
    md5_hex(buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1, out);
}

static rs_fail_t *fail_entry(const char *addr, int make)
{
    rs_fail_t *old = &g_fails[0];
    for (int i = 0; i < FAIL_ADDRS; ++i) {
        rs_fail_t *f = &g_fails[i];
        if (f->addr[0] && !strcmp(f->addr, addr)) return f;
        if (!f->addr[0] || (old->addr[0] && f->last < old->last)) old = f;
    }
    if (!make) return NULL;
    memset(old, 0, sizeof *old);
    snprintf(old->addr, sizeof old->addr, "%s", addr);
    return old;
}

/* The request's credentials: 1 let in (c->user, *perms), 0 refused, -1
 * none given, -2 the address waits after failures. */
static int login(rs_conn_t *c, const char *method, const char *auth, unsigned *perms)
{
    *perms = 0;
    time_t now = mono();
    rs_fail_t *f = fail_entry(c->addr, 0);
    if (f && f->locked_until > now) return -2;
    if (!auth || strncasecmp(auth, "Digest ", 7)) {
        if (!auth) return -1;
        /* Basic is not offered: the password would cross the network. */
        f = fail_entry(c->addr, 1);
    } else {
        char user[USERS_NAME_MAX], nonce[64], uri[512], resp[40], nc[16] = "", cnonce[80] = "", qop[16] = "";
        if (param(auth, "username", user, sizeof user) == 0 && param(auth, "nonce", nonce, sizeof nonce) == 0 &&
            param(auth, "uri", uri, sizeof uri) == 0 && param(auth, "response", resp, sizeof resp) == 0) {
            param(auth, "qop", qop, sizeof qop);
            param(auth, "nc", nc, sizeof nc);
            param(auth, "cnonce", cnonce, sizeof cnonce);
            char ha1[33], want[33];
            int known = users_digest(user, ha1, perms);
            if (known > 0 && !strcmp(nonce, c->nonce)) {
                rtsp_server_digest(ha1, nonce, method, uri, nc, cnonce, qop, want);
                if (auth_equal(want, resp)) {
                    if (f) memset(f, 0, sizeof *f);
                    snprintf(c->user, sizeof c->user, "%s", user);
                    return 1;
                }
            } else if (known < 0) {
                vg_log("rtsp: %s cannot log in yet: vigil does not know its password; set it again, "
                       "or log in once on the screen, the web page or the API", user);
            }
            if (strcmp(nonce, c->nonce)) {
                *perms = 0;
                return 0;                       /* stale: a fresh challenge, not a failure */
            }
        }
        *perms = 0;
        f = fail_entry(c->addr, 1);
    }
    f->last = now;
    if (++f->fails >= FAIL_FREE) {
        int s = f->fails - FAIL_FREE + 1;
        f->locked_until = now + (s >= 8 ? LOCKOUT_MAX : 1 << s);
    }
    vg_log("rtsp: login failed from %s (%d in a row)", c->addr, f->fails);
    return 0;
}

static void reply(rs_conn_t *c, int code, const char *reason, const char *cseq, const char *extra,
                  const char *body, size_t blen)
{
    char h[1536];
    int n = snprintf(h, sizeof h, "RTSP/1.0 %d %s\r\nCSeq: %s\r\nServer: vigil\r\n", code, reason, cseq);
    if (c->session[0] && n < (int)sizeof h)
        n += snprintf(h + n, sizeof h - (size_t)n, "Session: %s;timeout=%d\r\n", c->session, SESSION_SECS);
    if (extra && n < (int)sizeof h) n += snprintf(h + n, sizeof h - (size_t)n, "%s", extra);
    if (body && n < (int)sizeof h)
        n += snprintf(h + n, sizeof h - (size_t)n, "Content-Length: %zu\r\n", blen);
    if (n < (int)sizeof h) n += snprintf(h + n, sizeof h - (size_t)n, "\r\n");
    if (n >= (int)sizeof h) return;
    out_add(c, h, (size_t)n);
    if (body) out_add(c, body, blen);
}

int rtsp_server_path(const char *url, int *ch, int *sub)
{
    const char *p = url;
    if (!strncasecmp(p, "rtsp://", 7)) {
        p = strchr(p + 7, '/');
        if (!p) return -1;
    }
    char path[256];
    if (snprintf(path, sizeof path, "%s", p) >= (int)sizeof path) return -1;
    char *t = strstr(path, "/trackID=");
    if (t) *t = 0;
    int dahua = !strncmp(path, "/cam/realmonitor", 16) && (path[16] == '?' || !path[16]);
    if (!dahua && (t = strchr(path, '?')) != NULL) *t = 0;  /* ?transportmode=unicast and such */
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') path[--n] = 0;

    int c = 0, s = 0, k = 0;
    char word[16];
    if (dahua) {
        const char *q = path + 16;
        c = 1;
        for (const char *a = q; a && *a; a = strchr(a, '&')) {
            ++a;
            if (!strncmp(a, "channel=", 8)) c = atoi(a + 8);
            else if (!strncmp(a, "subtype=", 8)) s = atoi(a + 8);
        }
        if (s != 0 && s != 1) return -1;
    } else if (!strncasecmp(path, "/Streaming/Channels/", 20) || !strncasecmp(path, "/ISAPI/Streaming/Channels/", 26)) {
        const char *d = strrchr(path, '/') + 1;
        if (!*d || strspn(d, "0123456789") != strlen(d) || strlen(d) > 4) return -1;
        int v = atoi(d);
        c = v / 100;
        if (v % 100 != 1 && v % 100 != 2) return -1;
        s = v % 100 == 2;
    } else if (sscanf(path, "/h264/ch%d/%15[a-z]/av_stream%n", &c, word, &k) == 2 && !path[k]) {
        if (!strcmp(word, "sub")) s = 1;
        else if (strcmp(word, "main")) return -1;
    } else if (sscanf(path, "/ch%d/%15[a-z]%n", &c, word, &k) == 2 && !path[k]) {
        if (!strcmp(word, "sub")) s = 1;
        else if (strcmp(word, "main")) return -1;
    } else if (!(sscanf(path, "/ch%d%n", &c, &k) == 1 && !path[k])) {
        return -1;
    }
    if (c < 1 || c > CHANNEL_MAX) return -1;
    *ch = c - 1;
    *sub = s;
    return 0;
}

int rtsp_server_sdp(char *out, size_t cap, vg_codec_t codec, const stream_tap_ps_t *ps,
                    const char *local_ip, const char *title, unsigned long id)
{
    char fmtp[1400] = "", b64[3][AU_NORM_PS_BYTES * 4 / 3 + 8];
    for (int i = 0; i < ps->n && i < AU_NORM_PS_MAX; ++i)
        if (base64_encode(ps->ps[i], ps->len[i], b64[i], sizeof b64[i]) < 0) return -1;
    if (codec == VG_CODEC_H264) {
        int n = snprintf(fmtp, sizeof fmtp, "a=fmtp:%d packetization-mode=1", PT);
        if (ps->n == 2 && ps->len[0] >= 4)
            snprintf(fmtp + n, sizeof fmtp - (size_t)n, ";profile-level-id=%02X%02X%02X;sprop-parameter-sets=%s,%s",
                     ps->ps[0][1], ps->ps[0][2], ps->ps[0][3], b64[0], b64[1]);
    } else if (ps->n == 3) {
        snprintf(fmtp, sizeof fmtp, "a=fmtp:%d sprop-vps=%s;sprop-sps=%s;sprop-pps=%s", PT, b64[0], b64[1], b64[2]);
    }
    int n = snprintf(out, cap,
                     "v=0\r\n"
                     "o=- %lu 1 IN IP4 %s\r\n"
                     "s=%s\r\n"
                     "c=IN IP4 0.0.0.0\r\n"
                     "t=0 0\r\n"
                     "a=tool:vigil\r\n"
                     "a=range:npt=0-\r\n"
                     "a=control:*\r\n"
                     "m=video 0 RTP/AVP %d\r\n"
                     "a=rtpmap:%d %s/90000\r\n"
                     "%s%s"
                     "a=control:trackID=0\r\n",
                     id, local_ip, title, PT, PT, codec == VG_CODEC_H265 ? "H265" : "H264", fmtp,
                     fmtp[0] ? "\r\n" : "");
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

/* Does channel ch have that stream? 1 yes, 0 not now (connecting), -1 no. */
static int stream_exists(int ch, int sub)
{
    if (!stream_tap_exists(ch, sub)) return -1;
    return stream_tap_info(ch, sub, NULL, NULL) == 0;
}

static void do_describe(rs_conn_t *c, const char *url, const char *cseq, int ch, int sub)
{
    int ex = stream_exists(ch, sub);
    vg_stream_info_t si;
    vg_codec_t codec;
    if (ex < 0) {
        reply(c, 404, "Not Found", cseq, NULL, NULL, 0);
        return;
    }
    if (!ex || stream_tap_info(ch, sub, &si, &codec) < 0) {
        reply(c, 503, "Service Unavailable", cseq, "Retry-After: 2\r\n", NULL, 0);
        return;
    }
    stream_tap_ps_t ps;
    stream_tap_params(ch, sub, &ps);
    if (ps.n && ps.codec != codec) ps.n = 0;
    struct sockaddr_in me;
    socklen_t ml = sizeof me;
    char ip[48] = "0.0.0.0";
    if (getsockname(c->fd, (struct sockaddr *)&me, &ml) == 0) inet_ntop(AF_INET, &me.sin_addr, ip, sizeof ip);
    char title[32];
    snprintf(title, sizeof title, "CAM%d %s", ch + 1, stream_name(sub));
    char sdp[2048], extra[600];
    int n = rtsp_server_sdp(sdp, sizeof sdp, codec, &ps, ip, title, (unsigned long)mono_us());
    if (n < 0) {
        reply(c, 500, "Internal Server Error", cseq, NULL, NULL, 0);
        return;
    }
    size_t ul = strlen(url);
    snprintf(extra, sizeof extra, "Content-Base: %s%s\r\nContent-Type: application/sdp\r\n", url,
             ul && url[ul - 1] == '/' ? "" : "/");
    reply(c, 200, "OK", cseq, extra, sdp, (size_t)n);
}

static void do_setup(rs_conn_t *c, const char *req, const char *cseq, int ch, int sub)
{
    char tr[256], v[64];
    if (!header(req, "Transport", tr, sizeof tr)) {
        reply(c, 461, "Unsupported Transport", cseq, NULL, NULL, 0);
        return;
    }
    if (c->st && (c->ch != ch || c->sub != sub)) {     /* one track per session */
        reply(c, 459, "Aggregate Operation Not Allowed", cseq, NULL, NULL, 0);
        return;
    }
    /* The first transport the client offers that the server has. */
    int tcp = -1, a = 0, b = 0;
    for (char *t = tr, *next; t && *t && tcp < 0; t = next) {
        next = strchr(t, ',');
        if (next) *next++ = 0;
        if (strstr(t, "multicast")) continue;
        if (!strncmp(t, "RTP/AVP/TCP", 11)) {
            tcp = 1;
            if (param(t, "interleaved", v, sizeof v) == 0 && sscanf(v, "%d-%d", &a, &b) >= 1 && a >= 0 && a < 254) {
            } else {
                a = 0;
            }
        } else if (!strncmp(t, "RTP/AVP", 7) && g_udp[0] >= 0 && param(t, "client_port", v, sizeof v) == 0) {
            int n = sscanf(v, "%d-%d", &a, &b);
            if (n < 1 || a <= 0 || a > 65535) continue;
            if (n < 2 || b <= 0 || b > 65535) b = a + 1;
            tcp = 0;
        }
    }
    if (tcp < 0) {
        reply(c, 461, "Unsupported Transport", cseq, NULL, NULL, 0);
        return;
    }
    int ex = stream_exists(ch, sub);
    vg_codec_t codec;
    if (ex < 0) {
        reply(c, 404, "Not Found", cseq, NULL, NULL, 0);
        return;
    }
    if (!ex || stream_tap_info(ch, sub, NULL, &codec) < 0) {
        reply(c, 503, "Service Unavailable", cseq, "Retry-After: 2\r\n", NULL, 0);
        return;
    }
    if (!c->st) {
        if (sessions() >= g_max) {
            vg_log("rtsp: %s: %d clients already (rtsp_server.max_clients)", c->addr, g_max);
            reply(c, 453, "Not Enough Bandwidth", cseq, NULL, NULL, 0);
            return;
        }
        rs_stream_t *st = stream_get(ch, sub, codec);
        if (!st) {
            reply(c, 503, "Service Unavailable", cseq, NULL, NULL, 0);
            return;
        }
        ++st->clients;
        c->st = st;
        c->ch = ch;
        c->sub = sub;
        uint8_t r[16];
        char buf[64];
        int n = snprintf(buf, sizeof buf, "%.16s%s%llu", (const char *)g_key, c->nonce, (unsigned long long)mono_us());
        md5_raw(buf, (size_t)n, r);
        for (int i = 0; i < 8; ++i) snprintf(c->session + i * 2, 3, "%02X", r[i]);
    }
    c->tcp = tcp;
    char extra[256];
    if (tcp) {
        c->ich = a;
        snprintf(extra, sizeof extra, "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d;ssrc=%08X\r\n", a, a + 1,
                 (unsigned)c->st->ssrc);
    } else {
        c->cport = (uint16_t)a;
        c->cport_rtcp = (uint16_t)b;
        snprintf(extra, sizeof extra,
                 "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d;ssrc=%08X\r\n", a, b, g_udp_port,
                 g_udp_port + 1, (unsigned)c->st->ssrc);
    }
    reply(c, 200, "OK", cseq, extra, NULL, 0);
}

static void do_play(rs_conn_t *c, const char *url, const char *cseq)
{
    rs_stream_t *st = c->st;
    char extra[1200];
    int n = snprintf(extra, sizeof extra, "Range: npt=0.000-\r\n");
    pthread_mutex_lock(&st->lock);
    c->pkts = c->octets = 0;
    if (st->have_key && st->key >= st->tail) {
        /* The newest keyframe in the ring: the picture at once. */
        c->pos = st->key;
        c->wait_key = 0;
        const uint8_t *r = st->ring + st->key % st->size + 4;
        unsigned seq = (unsigned)r[2] << 8 | r[3];
        uint32_t ts = (uint32_t)r[4] << 24 | (uint32_t)r[5] << 16 | (uint32_t)r[6] << 8 | r[7];
        size_t ul = strlen(url);
        snprintf(extra + n, sizeof extra - (size_t)n, "RTP-Info: url=%s%strackID=0;seq=%u;rtptime=%u\r\n", url,
                 ul && url[ul - 1] == '/' ? "" : "/", seq, (unsigned)ts);
    } else {
        c->wait_key = 1;
        c->wait_from = 0;
    }
    pthread_mutex_unlock(&st->lock);
    reply(c, 200, "OK", cseq, extra, NULL, 0);
    if (!c->playing) {
        c->playing = 1;
        /* Interleaved: the connection is the video now. */
        if (c->tcp) qos_mark(c->fd, QOS_VIDEO, 1);
        if (!c->play_at) {
            c->play_at = mono();
            vg_log("rtsp: %s%s%s plays CH%d %s over %s", c->addr, c->user[0] ? " " : "", c->user, c->ch + 1,
                   stream_name(c->sub), c->tcp ? "TCP" : "UDP");
        }
        c->sr_at = mono();
    }
}

/* One request: `req` is its header block, NUL-terminated. */
static void handle(rs_conn_t *c, char *req)
{
    char method[24], url[512], cseq[16] = "0", sess[64], auth[1024];
    if (sscanf(req, "%23s %511s RTSP/", method, url) != 2) {
        reply(c, 400, "Bad Request", cseq, NULL, NULL, 0);
        c->close_after = 1;
        return;
    }
    header(req, "CSeq", cseq, sizeof cseq);
    cseq[strspn(cseq, "0123456789")] = 0;

    if (!strcmp(method, "OPTIONS")) {
        reply(c, 200, "OK", cseq,
              "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER, SET_PARAMETER\r\n", NULL, 0);
        return;
    }
    if (g_auth) {
        unsigned perms = 0;
        int given = header(req, "Authorization", auth, sizeof auth) != NULL;
        int rc = given ? login(c, method, auth, &perms) : c->authed ? 1 : -1;
        if (rc == 1 && given && !(perms & PERM_LIVE)) {
            vg_log("rtsp: %s %s: its group has no live permission", c->addr, c->user);
            reply(c, 403, "Forbidden", cseq, NULL, NULL, 0);
            return;
        }
        if (rc == 1) {
            c->authed = 1;
        } else {
            char ch[160];
            if (rc != -2) make_nonce(c->nonce);
            snprintf(ch, sizeof ch, "WWW-Authenticate: Digest realm=\"%s\", nonce=\"%s\"\r\n", USERS_REALM, c->nonce);
            reply(c, 401, "Unauthorized", cseq, ch, NULL, 0);
            return;
        }
    }
    if (header(req, "Session", sess, sizeof sess)) {
        sess[strcspn(sess, "; ")] = 0;
        if (strcmp(sess, c->session) && strcmp(method, "SETUP")) {
            reply(c, 454, "Session Not Found", cseq, NULL, NULL, 0);
            return;
        }
    }
    int ch = 0, sub = 0;
    if (!strcmp(method, "DESCRIBE") || !strcmp(method, "SETUP")) {
        if (rtsp_server_path(url, &ch, &sub) < 0) {
            reply(c, 404, "Not Found", cseq, NULL, NULL, 0);
            return;
        }
        if (!strcmp(method, "DESCRIBE")) do_describe(c, url, cseq, ch, sub);
        else do_setup(c, req, cseq, ch, sub);
    } else if (!strcmp(method, "PLAY")) {
        if (!c->st) reply(c, 455, "Method Not Valid in This State", cseq, NULL, NULL, 0);
        else {
            char base[512];
            snprintf(base, sizeof base, "%s", url);
            char *t = strstr(base, "/trackID=");
            if (t) *t = 0;
            do_play(c, base, cseq);
        }
    } else if (!strcmp(method, "PAUSE")) {
        if (!c->st) reply(c, 455, "Method Not Valid in This State", cseq, NULL, NULL, 0);
        else {
            c->playing = 0;
            reply(c, 200, "OK", cseq, NULL, NULL, 0);
        }
    } else if (!strcmp(method, "TEARDOWN")) {
        reply(c, 200, "OK", cseq, NULL, NULL, 0);
        session_end(c, "teardown");
    } else if (!strcmp(method, "GET_PARAMETER") || !strcmp(method, "SET_PARAMETER")) {
        reply(c, 200, "OK", cseq, NULL, NULL, 0);
    } else {
        reply(c, 501, "Not Implemented", cseq, NULL, NULL, 0);
    }
}

/* Read what the client sent; handle each whole request. -1: gone. */
static int conn_read(rs_conn_t *c)
{
    ssize_t n = recv(c->fd, c->in + c->in_len, IN_CAP - c->in_len, MSG_DONTWAIT);
    if (n == 0) return -1;
    if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
    c->in_len += (size_t)n;
    c->last_rx = mono();
    for (;;) {
        if (!c->in_len) break;
        if (c->in[0] == '$') {                  /* interleaved RTCP from the client */
            if (c->in_len < 4) break;
            size_t len = 4 + ((size_t)c->in[2] << 8 | c->in[3]);
            if (c->in_len < len) {
                if (len > IN_CAP) return -1;
                break;
            }
            memmove(c->in, c->in + len, c->in_len - len);
            c->in_len -= len;
            continue;
        }
        uint8_t *end = NULL;
        for (size_t i = 0; i + 4 <= c->in_len; ++i)
            if (!memcmp(c->in + i, "\r\n\r\n", 4)) {
                end = c->in + i + 4;
                break;
            }
        if (!end) {
            if (c->in_len == IN_CAP) return -1;     /* a request too big to be one */
            break;
        }
        size_t hl = (size_t)(end - c->in), body = 0;
        char req[IN_CAP + 1], v[16];
        memcpy(req, c->in, hl);
        req[hl] = 0;
        if (header(req, "Content-Length", v, sizeof v)) body = (size_t)strtoul(v, NULL, 10);
        if (hl + body > IN_CAP) return -1;
        if (c->in_len < hl + body) break;
        handle(c, req);
        memmove(c->in, c->in + hl + body, c->in_len - hl - body);
        c->in_len -= hl + body;
        if (c->close_after) break;
    }
    return 0;
}

/* RTCP (receiver reports) from a UDP client: it is still there. */
static void udp_read(int fd)
{
    uint8_t buf[1500];
    for (int k = 0; k < 64; ++k) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(fd, buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
        if (n < 0) return;
        for (int i = 0; i < CONN_MAX; ++i) {
            rs_conn_t *c = g_conns[i];
            if (c && c->st && !c->tcp && c->peer.sin_addr.s_addr == from.sin_addr.s_addr &&
                (ntohs(from.sin_port) == c->cport_rtcp || ntohs(from.sin_port) == c->cport))
                c->last_rx = mono();
        }
    }
}

/* ---- the thread ------------------------------------------------------------------- */

static void read_config(void)
{
    unsigned gen = config_generation();
    if (gen == g_cfg_gen) return;
    vigil_config_t *cfg = malloc(sizeof *cfg);
    if (!cfg) return;
    config_get(cfg);
    g_cfg_gen = gen;
    g_auth = cfg->rtsp_server.auth;
    g_max = cfg->rtsp_server.max_clients;
    free(cfg);
}

static void tick(void)
{
    time_t now = mono();
    read_config();
    for (int i = 0; i < CONN_MAX; ++i) {
        rs_conn_t *c = g_conns[i];
        if (!c) continue;
        if (c->st && c->st->codec_changed) {
            conn_close(i, "the camera changed its codec");
            continue;
        }
        if (!c->st && now - c->last_rx > IDLE_SECS) {
            conn_close(i, "idle");
            continue;
        }
        if (c->st && !c->tcp && now - c->last_rx > SESSION_SECS + 5) {
            conn_close(i, "timed out");
            continue;
        }
        if (c->out_len > c->out_off && now - c->last_progress > STALL_SECS) {
            conn_close(i, "not reading");
            continue;
        }
        if (c->playing && c->pkts && now - c->sr_at >= SR_SECS) {
            c->sr_at = now;
            send_sr(c);
        }
    }
    for (int i = 0; i < CHANNEL_MAX * 2; ++i) {
        rs_stream_t *st = &g_streams[i];
        if (st->used && !st->clients && (now - st->idle_since >= LINGER_SECS || st->codec_changed))
            stream_drop(st);
    }
}

static void *server_thread(void *arg)
{
    (void)arg;
    struct pollfd pf[4 + CONN_MAX];
    int map[4 + CONN_MAX];
    time_t last_tick = mono();
    while (!g_stop) {
        int n = 0;
        pf[n] = (struct pollfd){ g_wake[0], POLLIN, 0 };
        map[n++] = -1;
        int conns = 0;
        for (int i = 0; i < CONN_MAX; ++i) conns += g_conns[i] != NULL;
        pf[n] = (struct pollfd){ g_lfd, conns < CONN_MAX ? POLLIN : 0, 0 };
        map[n++] = -2;
        for (int u = 0; u < 2; ++u)
            if (g_udp[u] >= 0) {
                pf[n] = (struct pollfd){ g_udp[u], POLLIN, 0 };
                map[n++] = -3 - u;
            }
        for (int i = 0; i < CONN_MAX; ++i) {
            rs_conn_t *c = g_conns[i];
            if (!c) continue;
            pf[n] = (struct pollfd){ c->fd, (short)(POLLIN | (c->out_len > c->out_off ? POLLOUT : 0)), 0 };
            map[n++] = i;
        }
        int rc = poll(pf, (nfds_t)n, g_behind ? UDP_TURN_MS : 500);
        g_behind = 0;
        if (rc < 0 && errno != EINTR) break;
        for (int k = 0; rc > 0 && k < n; ++k) {
            if (!pf[k].revents) continue;
            if (map[k] == -1) {
                /* Drained before the flag is cleared: a wake in between
                 * finds it still set and writes nothing, and what it
                 * announced is sent below. */
                char b[64];
                while (read(g_wake[0], b, sizeof b) > 0) {}
                atomic_store(&g_wake_pending, 0);
            } else if (map[k] == -2) {
                conn_accept();
            } else if (map[k] <= -3) {
                udp_read(g_udp[-3 - map[k]]);
            } else {
                rs_conn_t *c = g_conns[map[k]];
                if (!c) continue;
                if ((pf[k].revents & (POLLIN | POLLHUP | POLLERR)) && conn_read(c) < 0) {
                    conn_close(map[k], "closed");
                    continue;
                }
            }
        }
        /* Every client, whatever woke the thread: new packets are what
         * usually does. */
        for (int i = 0; i < CONN_MAX; ++i) {
            rs_conn_t *c = g_conns[i];
            if (c && media_pump(c) < 0) conn_close(i, c->close_after ? "done" : "closed");
        }
        time_t now = mono();
        if (now != last_tick) {
            last_tick = now;
            tick();
        }
    }
    for (int i = 0; i < CONN_MAX; ++i) conn_close(i, "stopping");
    for (int i = 0; i < CHANNEL_MAX * 2; ++i)
        if (g_streams[i].used) stream_drop(&g_streams[i]);
    return NULL;
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        qos_forget(*fd);
        close(*fd);
    }
    *fd = -1;
}

/* A UDP socket on `port`, or -1. */
static int udp_bind(int port)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    /* A keyframe goes out as a burst of packets. */
    int sz = 512 << 10;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
    return fd;
}

static void random_key(void)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, g_key, sizeof g_key) : -1;
    if (fd >= 0) close(fd);
    if (n != (ssize_t)sizeof g_key) {
        uint64_t v = mono_us() ^ (uint64_t)getpid() << 32;
        for (size_t i = 0; i < sizeof g_key; ++i) g_key[i] = (unsigned char)(v >> (i % 8 * 8)) ^ (unsigned char)i;
    }
    for (size_t i = 0; i < sizeof g_key; ++i) g_key[i] = (unsigned char)('!' + g_key[i] % 90);
}

/* What runs, for rtsp_server_listening (Network -> Ports). */
static pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_listening;
static char            g_listen_on[24], g_why[160];

static void set_state(int on, const char *why)
{
    pthread_mutex_lock(&g_state_lock);
    g_listening = on;
    if (on) snprintf(g_listen_on, sizeof g_listen_on, "0.0.0.0:%d", g_port);
    else g_listen_on[0] = 0;
    snprintf(g_why, sizeof g_why, "%s", why);
    pthread_mutex_unlock(&g_state_lock);
}

int rtsp_server_listening(char *listen, size_t cap, char *why, size_t wcap)
{
    pthread_mutex_lock(&g_state_lock);
    int on = g_listening;
    if (listen && cap) snprintf(listen, cap, "%s", g_listen_on);
    if (why && wcap) snprintf(why, wcap, "%s", g_why);
    pthread_mutex_unlock(&g_state_lock);
    return on;
}

/* Serve on `port`: 0, or -1 with `err` (nothing runs then). */
static int serve(int port, int auth, char *err, size_t cap)
{
    g_port = port;
    g_lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (g_lfd >= 0) setsockopt(g_lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (g_lfd < 0 || bind(g_lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(g_lfd, 16) < 0) {
        snprintf(err, cap, "port %d: %s", port, strerror(errno));
        close_fd(&g_lfd);
        return -1;
    }
    fcntl(g_lfd, F_SETFL, fcntl(g_lfd, F_GETFL) | O_NONBLOCK);
    /* Requests are management; a connection becomes video at its PLAY,
     * and the UDP ports carry only RTP and RTCP. */
    qos_mark(g_lfd, QOS_MANAGEMENT, 1);
    for (int p = UDP_PORT_FIRST; p < UDP_PORT_FIRST + 200; p += 2) {
        g_udp[0] = udp_bind(p);
        g_udp[1] = g_udp[0] >= 0 ? udp_bind(p + 1) : -1;
        if (g_udp[1] >= 0) {
            g_udp_port = p;
            break;
        }
        close_fd(&g_udp[0]);
    }
    if (g_udp[0] < 0) vg_log("rtsp: no UDP port pair free from %d; RTP over TCP only", UDP_PORT_FIRST);
    for (int u = 0; u < 2; ++u)
        if (g_udp[u] >= 0) qos_mark(g_udp[u], QOS_VIDEO, 1);
    if (pipe(g_wake) < 0) {
        snprintf(err, cap, "no pipe: %s", strerror(errno));
        goto fail;
    }
    for (int i = 0; i < 2; ++i) {
        fcntl(g_wake[i], F_SETFL, fcntl(g_wake[i], F_GETFL) | O_NONBLOCK);
        fcntl(g_wake[i], F_SETFD, FD_CLOEXEC);
    }
    atomic_store(&g_wake_pending, 0);
    g_stop = 0;
    if (pthread_create(&g_thread, NULL, server_thread, NULL) != 0) {
        snprintf(err, cap, "cannot start its thread");
        goto fail;
    }
    g_up = 1;
    stream_tap_standby(1);
    if (g_udp[0] >= 0)
        vg_log("rtsp: listening on port %d (RTP/UDP from %d-%d)%s", port, g_udp_port, g_udp_port + 1,
               auth ? "" : " WITHOUT authentication (rtsp_server.auth is off)");
    else
        vg_log("rtsp: listening on port %d%s", port, auth ? "" : " WITHOUT authentication (rtsp_server.auth is off)");
    return 0;
fail:
    close_fd(&g_lfd);
    close_fd(&g_udp[0]);
    close_fd(&g_udp[1]);
    close_fd(&g_wake[0]);
    close_fd(&g_wake[1]);
    return -1;
}

static void unserve(void)
{
    if (!g_up) return;
    g_stop = 1;
    wake();
    pthread_join(g_thread, NULL);
    g_up = 0;
    stream_tap_standby(0);
    close_fd(&g_lfd);
    close_fd(&g_udp[0]);
    close_fd(&g_udp[1]);
    close_fd(&g_wake[0]);
    close_fd(&g_wake[1]);
}

static int read_section(config_rtsp_server_t *rs)
{
    vigil_config_t *cfg = malloc(sizeof *cfg);
    if (!cfg) return -1;
    g_cfg_gen = config_get(cfg);
    *rs = cfg->rtsp_server;
    free(cfg);
    return 0;
}

int rtsp_server_start(void)
{
    static int locks_made;
    if (!locks_made) {
        for (int i = 0; i < CHANNEL_MAX * 2; ++i) pthread_mutex_init(&g_streams[i].lock, NULL);
        locks_made = 1;
        random_key();
    }
    config_rtsp_server_t rs;
    if (read_section(&rs) < 0) return -1;
    g_enabled = rs.enabled;
    g_port = rs.port;
    g_auth = rs.auth;
    g_max = rs.max_clients;
    if (!rs.enabled) {
        vg_log("rtsp: off (rtsp_server.enabled)");
        set_state(0, "off: rtsp_server.enabled is off");
        return 0;
    }
    char err[160];
    if (serve(rs.port, rs.auth, err, sizeof err) < 0) {
        vg_log("rtsp: cannot listen: %s; running without the RTSP server", err);
        set_state(0, err);
        return -1;
    }
    set_state(1, "");
    return 0;
}

void rtsp_server_stop(void)
{
    unserve();
    set_state(0, "stopped");
}

/* The port setting back to what runs (`port`), so the file says it. */
static void put_port_back(int port)
{
    char *json = config_get_json("rtsp_server", 0);
    cJSON *doc = json ? cJSON_Parse(json) : NULL;
    free(json);
    if (!doc) return;
    cJSON_ReplaceItemInObjectCaseSensitive(doc, "port", cJSON_CreateNumber(port));
    char *body = cJSON_PrintUnformatted(doc);
    schema_errors_t errs = { 0 };
    if (!body || config_put_json("rtsp_server", body, strlen(body), &errs) != CONFIG_OK)
        vg_log("rtsp_server.port: cannot put %d back: %s", port, errs.n ? errs.e[0].msg : "out of memory");
    free(body);
    cJSON_Delete(doc);
}

void rtsp_server_apply(void)
{
    config_rtsp_server_t rs;
    if (read_section(&rs) < 0) return;
    if (rs.enabled == g_enabled && (!rs.enabled || rs.port == g_port)) return;
    int was_on = g_up, was_port = g_port;
    unserve();
    g_enabled = rs.enabled;
    g_auth = rs.auth;
    g_max = rs.max_clients;
    if (!rs.enabled) {
        vg_log("rtsp: off (rtsp_server.enabled)");
        set_state(0, "off: rtsp_server.enabled is off");
        return;
    }
    char err[160];
    if (serve(rs.port, rs.auth, err, sizeof err) == 0) {
        vg_log("rtsp: now on port %d", rs.port);
        set_state(1, "");
        return;
    }
    /* The port was taken since the store checked it: back where it was. */
    vg_log("rtsp_server.port %d: %s", rs.port, err);
    events_push(0, "rtsp port: %.24s", err);
    char why[200];
    snprintf(why, sizeof why, "port %d refused: %s", rs.port, err);
    if (was_on && serve(was_port, rs.auth, err, sizeof err) == 0) {
        set_state(1, why);
        put_port_back(was_port);
    } else {
        set_state(0, why);
    }
}
