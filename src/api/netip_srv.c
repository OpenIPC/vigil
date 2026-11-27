/* SPDX-License-Identifier: GPL-3.0-or-later */
/* netip_srv.c — vigild as a NETIP device (see netip_srv.h).
 *
 * The protocol as XM's documents give it (netip_proto.h) and as XM
 * devices behave towards their clients:
 *   - login (1000) with the 8-character hash of the password; the reply
 *     says "DeviceType " (with the space: python-dvr reads that key),
 *     the channels and the keepalive interval;
 *   - live view: an OPMonitor Claim (1413) on a connection, then an
 *     OPMonitor Start (1410). XM's clients claim on a new connection that
 *     never logs in, with the session's id in the header, and start on
 *     the logged-in one: the media goes to the claiming connection and the
 *     start is answered (1411). python-dvr and vigil's own source claim
 *     and start on the logged-in connection and read media from it at
 *     once, so there the start gets no answer (a JSON reply among the
 *     media would be taken for a frame);
 *   - media as 1412 messages of at most 8 KiB, each frame its XM header
 *     and payload, the channel in the header's byte 12, sequence from 1.
 * Everything else is read-only: the few configurations clients read at
 * login are answered from the channels' status, writes are refused
 * (107, no permission), unknown configurations get 607 (no such
 * configuration) and unknown messages a plain OK, each logged once.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netpacket/packet.h>
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
#include "disc_proto.h"
#include "discovery.h"
#include "log.h"
#include "md5.h"
#include "netip_proto.h"
#include "netip_srv.h"
#include "stream_tap.h"
#include "sys_net.h"
#include "users.h"

#ifndef VIGIL_VERSION
#define VIGIL_VERSION "0"
#endif

#define CONN_MAX     24                 /* connections, logged in or carrying media */
#define STREAMS_MAX  16                 /* live streams at once */
#define RING_MAIN    (1u << 20)         /* a main stream's queue: a 4 MP keyframe and more */
#define RING_SUB     (256u << 10)
#define RING_BUDGET  (6u << 20)         /* every queue together */
#define CHUNK        8192               /* media bytes per message, as XM devices send */
#define REQ_MAX      (32u << 10)        /* a request's body; XM's are 16 KiB at most */
#define ALIVE_SECS   20                 /* the keepalive interval clients are given */
#define LOGIN_SECS   15                 /* to log in or claim after connecting */
#define SEND_SECS    5                  /* a client that takes no data this long is gone */
#define BURST        (512u << 10)       /* media sent between looks at the requests */
#define FAILS_MAX    5                  /* wrong logins from an address ... */
#define LOCK_SECS    60                 /* ... lock it out this long */
#define SEARCH_RATE  20                 /* search answers a second, at most */
#define TOLD_MAX     64

enum { MON_NONE, MON_CLAIMED, MON_START, MON_ON };

typedef struct {
    int             used, done;         /* slot taken; its thread has ended (to join) */
    pthread_t       th;
    int             fd;
    int             wake[2];            /* media queued, or end: wakes the thread */
    uint32_t        peer;               /* IPv4, host order */
    char            peer_s[16];
    atomic_int      kill;
    time_t          since, last_rx;
    /* Under g_lock (other connections look them up). */
    uint32_t        session;            /* logged in: its session; 0 not */
    uint32_t        owner;              /* a media connection: the session it serves */
    unsigned        perms;
    char            user[USERS_NAME_MAX];
    int             mon, mon_ch, mon_sub;
    size_t          reserved;           /* its queue's bytes, taken from the budget at the claim */
    int             mon_self;           /* the logged-in connection carries its own media */
    int             pend, pend_ch, pend_sub;    /* a start that came before its claim */
    /* The media queue: whole frames (u32 length, XM header, payload) back
     * to back, wrapping; head and tail count bytes ever written and sent. */
    pthread_mutex_t qlock;
    uint8_t        *ring;
    size_t          cap, head, tail;
    int             need_key;
    int             started;            /* its first keyframe is in */
    int             too_big;            /* a picture larger than the queue was logged */
    unsigned        dropped;            /* frames a full queue lost */
    stream_tap_t   *tap;
    /* The tap's thread only. */
    unsigned        fps, fcount;
    struct timespec f_t0;
    uint32_t        mseq;
    /* The connection's thread only. */
    uint8_t        *rx;
    size_t          rx_len;
} conn_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static conn_t          g_conn[CONN_MAX];
static size_t          g_ring_used;     /* under g_lock */
static int             g_streams;       /* under g_lock */
static uint32_t        g_next_session;  /* under g_lock */
static pthread_t       g_listen_th;
static int             g_running, g_tcp = -1, g_udp = -1, g_stop_pipe[2] = { -1, -1 };
static atomic_int      g_stop;
static config_netip_server_t g_cfg;
static char            g_platform[48], g_serial[24];

static struct { uint32_t ip; int fails; time_t last, until; } g_fail[32];   /* under g_lock */
static struct { int msgid; char name[40]; } g_told[TOLD_MAX];         /* under g_lock */
static int g_ntold;

static time_t mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

/* Say a request we do not serve once per process, not at every login. */
static void tell_once(int msgid, const char *name, const char *what)
{
    pthread_mutex_lock(&g_lock);
    int seen = 0;
    for (int i = 0; i < g_ntold && !seen; ++i)
        seen = g_told[i].msgid == msgid && !strncmp(g_told[i].name, name, sizeof g_told[i].name - 1);
    if (!seen && g_ntold < TOLD_MAX) {
        g_told[g_ntold].msgid = msgid;
        snprintf(g_told[g_ntold].name, sizeof g_told[g_ntold].name, "%s", name);
        ++g_ntold;
    }
    pthread_mutex_unlock(&g_lock);
    if (!seen) vg_log("netip server: %s: msg %d%s%s", what, msgid, name[0] ? " " : "", name);
}

/* ---- this device: addresses, serial ------------------------------------------------- */

/* The interface holding `addr` (host order), or the first that is up and
 * not loopback when addr is 0: name and what it has (sys_net.h). */
static int iface_of(uint32_t addr, char *name, size_t cap)
{
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) < 0) return -1;
    int rc = -1;
    for (struct ifaddrs *i = ifa; i && rc < 0; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP)) continue;
        uint32_t a = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
        if (addr ? a == addr : !(i->ifa_flags & IFF_LOOPBACK)) {
            snprintf(name, cap, "%s", i->ifa_name);
            rc = 0;
        }
    }
    freeifaddrs(ifa);
    return rc;
}

/* What the search answer and NetWork.NetCommon say about us, reached at
 * `addr` (host order). */
static void this_device(uint32_t addr, disc_device_t *d)
{
    memset(d, 0, sizeof *d);
    char ifn[IF_NAMESIZE] = "";
    disc_ip_str(addr, d->ip, sizeof d->ip);
    if (iface_of(addr, ifn, sizeof ifn) == 0) {
        net_status_t st;
        net_status(ifn, &st);
        snprintf(d->mac, sizeof d->mac, "%s", st.mac);
        if (!strcmp(st.address, d->ip)) {
            snprintf(d->mask, sizeof d->mask, "%s", st.netmask);
            snprintf(d->gateway, sizeof d->gateway, "%s", st.gateway);
        }
    }
    if (!d->mask[0]) snprintf(d->mask, sizeof d->mask, "255.255.255.0");
    gethostname(d->name, sizeof d->name - 1);
    snprintf(d->serial, sizeof d->serial, "%s", g_serial);
    snprintf(d->version, sizeof d->version, "V" VIGIL_VERSION ".vigil");
    d->netip_port = g_cfg.port;
    d->http_port = 80;
    d->channels = channels_count();
}

/* The serial clients tell devices apart by: XM's are 16 hex digits; ours
 * the MAC of the first Ethernet interface (else any but loopback), so it
 * stays the same across restarts, whether or not it has an address yet. */
static void make_serial(void)
{
    char hex[13] = "";
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) == 0) {
        for (int pass = 0; pass < 2 && !hex[0]; ++pass)
            for (struct ifaddrs *i = ifa; i && !hex[0]; i = i->ifa_next) {
                if (!i->ifa_addr || i->ifa_addr->sa_family != AF_PACKET || (i->ifa_flags & IFF_LOOPBACK)) continue;
                if (!pass && strncmp(i->ifa_name, "eth", 3)) continue;
                const struct sockaddr_ll *ll = (const struct sockaddr_ll *)i->ifa_addr;
                unsigned any = 0;
                for (int k = 0; k < 6 && ll->sll_halen == 6; ++k) any |= ll->sll_addr[k];
                if (!any) continue;
                for (int k = 0; k < 6; ++k) snprintf(hex + 2 * k, 3, "%02X", ll->sll_addr[k]);
            }
        freeifaddrs(ifa);
    }
    if (!hex[0]) {
        /* No MAC to be had: the host name's, stable as well. */
        char host[64] = "";
        uint8_t d[16];
        gethostname(host, sizeof host - 1);
        md5_raw(host, strlen(host), d);
        for (int i = 0; i < 6; ++i) snprintf(hex + 2 * i, 3, "%02X", d[i]);
    }
    snprintf(g_serial, sizeof g_serial, "0000%s", hex);
}

/* ---- sending -------------------------------------------------------------------------- */

static int send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t r = send(fd, p, len, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;              /* SO_SNDTIMEO: a client that takes nothing */
        p += r;
        len -= (size_t)r;
    }
    return 0;
}

static void hex_session(uint32_t s, char out[12]) { snprintf(out, 12, "0x%08X", s); }

/* A JSON reply (+ "\n\0") to request `req`; deletes `doc`. */
static int reply(conn_t *c, const netip_hdr_t *req, cJSON *doc)
{
    char *text = doc ? cJSON_PrintUnformatted(doc) : NULL;
    cJSON_Delete(doc);
    if (!text) return -1;
    size_t tl = strlen(text);
    uint8_t *buf = malloc(NETIP_HDR_LEN + tl + 2);
    int rc = -1;
    if (buf) {
        pthread_mutex_lock(&g_lock);
        uint32_t sess = c->session ? c->session : c->owner;
        pthread_mutex_unlock(&g_lock);
        netip_hdr_t h = { .version = 1, .session = sess, .seq = req->seq,
                          .msgid = (uint16_t)(req->msgid + 1), .len = (uint32_t)(tl + 2) };
        netip_hdr_put(buf, &h);
        memcpy(buf + NETIP_HDR_LEN, text, tl);
        buf[NETIP_HDR_LEN + tl] = '\n';
        buf[NETIP_HDR_LEN + tl + 1] = 0;
        rc = send_all(c->fd, buf, NETIP_HDR_LEN + tl + 2);
        free(buf);
    }
    free(text);
    return rc;
}

/* {"Name": name, "Ret": ret, "SessionID": ...}, for adding to. */
static cJSON *answer(conn_t *c, const char *name, int ret)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    char s[12];
    pthread_mutex_lock(&g_lock);
    hex_session(c->session ? c->session : c->owner, s);
    pthread_mutex_unlock(&g_lock);
    cJSON_AddStringToObject(o, "Name", name);
    cJSON_AddNumberToObject(o, "Ret", ret);
    cJSON_AddStringToObject(o, "SessionID", s);
    return o;
}

static int reply_ret(conn_t *c, const netip_hdr_t *req, const char *name, int ret)
{
    return reply(c, req, answer(c, name, ret));
}

/* ---- the media queue ------------------------------------------------------------------- */

static void ring_put(conn_t *c, const void *data, size_t n)
{
    size_t at = c->head % c->cap, first = c->cap - at < n ? c->cap - at : n;
    memcpy(c->ring + at, data, first);
    memcpy(c->ring, (const uint8_t *)data + first, n - first);
    c->head += n;
}

static void ring_get(const conn_t *c, size_t from, void *out, size_t n)
{
    size_t at = from % c->cap, first = c->cap - at < n ? c->cap - at : n;
    memcpy(out, c->ring + at, first);
    memcpy((uint8_t *)out + first, c->ring, n - first);
}

/* The tap: on the channel's session thread, so copy or drop, never wait. */
static void on_au(void *user, const vg_au_t *au, const vg_stream_info_t *si, vg_codec_t codec)
{
    conn_t *c = user;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    /* The frame rate the I-frame header says, counted over 2 s. */
    ++c->fcount;
    long ms = (now.tv_sec - c->f_t0.tv_sec) * 1000 + (now.tv_nsec - c->f_t0.tv_nsec) / 1000000;
    if (ms >= 2000) {
        if (c->f_t0.tv_sec) c->fps = (unsigned)((c->fcount * 1000u + (unsigned)ms / 2) / (unsigned)ms);
        c->fcount = 0;
        c->f_t0 = now;
    }
    uint8_t head[NETIP_FRAME_HEAD];
    struct tm tm;
    time_t t = time(NULL);
    localtime_r(&t, &tm);
    size_t hl = netip_frame_head(head, au->key, codec, si->width, si->height, c->fps ? c->fps : 25, &tm,
                                 (uint32_t)au->len);
    if (!hl) return;
    uint32_t total = (uint32_t)(hl + au->len);
    int wake = 0, big = 0;
    pthread_mutex_lock(&c->qlock);
    if (c->ring) {
        if (c->need_key && !au->key) {
            c->dropped += c->started;           /* not before the first keyframe */
        } else if (c->cap - (c->head - c->tail) < 4 + (size_t)total) {
            /* Behind: drop up to the next keyframe that fits. */
            c->need_key = 1;
            ++c->dropped;
            if (4 + (size_t)total > c->cap && !c->too_big) c->too_big = big = 1;
        } else {
            wake = c->head == c->tail;
            ring_put(c, &total, 4);
            ring_put(c, head, hl);
            ring_put(c, au->data, au->len);
            c->need_key = 0;
            c->started = 1;
        }
    }
    pthread_mutex_unlock(&c->qlock);
    if (wake && write(c->wake[1], "m", 1) < 0) { /* full: it is awake anyway */ }
    if (big)
        vg_log("netip server: CAM%d %s: a %u KiB picture is larger than a client's queue (%u KiB)",
               c->mon_ch + 1, c->mon_sub ? "sub" : "main", total >> 10,
               (unsigned)((c->mon_sub ? RING_SUB : RING_MAIN) >> 10));
}

/* A claim takes its stream's queue out of the budget (under g_lock): 0,
 * or -1 when STREAMS_MAX streams or RING_BUDGET are taken. */
static int reserve(conn_t *c, int sub)
{
    size_t cap = sub ? RING_SUB : RING_MAIN;
    if (g_streams >= STREAMS_MAX || g_ring_used + cap > RING_BUDGET) return -1;
    ++g_streams;
    g_ring_used += cap;
    c->reserved = cap;
    return 0;
}

/* Start the claimed stream on this connection (its own thread). */
static int mon_start(conn_t *c)
{
    pthread_mutex_lock(&g_lock);
    int ch = c->mon_ch, sub = c->mon_sub;
    size_t cap = c->reserved;
    pthread_mutex_unlock(&g_lock);
    uint8_t *ring = cap ? malloc(cap) : NULL;
    if (!ring) return -1;
    channel_status_t st;
    channels_status(ch, &st);
    c->fps = st.fps;
    c->fcount = 0;
    memset(&c->f_t0, 0, sizeof c->f_t0);
    c->mseq = 1;
    pthread_mutex_lock(&c->qlock);
    c->ring = ring;
    c->cap = cap;
    c->head = c->tail = 0;
    c->need_key = 1;                        /* a tap may open mid-GOP */
    c->started = 0;
    c->too_big = 0;
    c->dropped = 0;
    pthread_mutex_unlock(&c->qlock);
    c->tap = stream_tap_open(ch, sub, on_au, c);
    if (!c->tap) {
        pthread_mutex_lock(&c->qlock);
        c->ring = NULL;
        pthread_mutex_unlock(&c->qlock);
        free(ring);
        return -1;
    }
    pthread_mutex_lock(&g_lock);
    c->mon = MON_ON;
    pthread_mutex_unlock(&g_lock);
    vg_log("netip server: %s (%s): live CAM%d %s", c->peer_s, c->user, ch + 1, sub ? "sub" : "main");
    return 0;
}

static void mon_stop(conn_t *c)
{
    if (c->tap) {
        stream_tap_close(c->tap);           /* on_au is not called after this */
        c->tap = NULL;
    }
    pthread_mutex_lock(&c->qlock);
    uint8_t *ring = c->ring;
    unsigned dropped = c->dropped;
    c->ring = NULL;
    c->head = c->tail = 0;
    pthread_mutex_unlock(&c->qlock);
    pthread_mutex_lock(&g_lock);
    int was = c->mon == MON_ON;
    c->mon = MON_NONE;
    c->mon_self = 0;
    if (c->reserved) {
        --g_streams;
        g_ring_used -= c->reserved;
        c->reserved = 0;
    }
    pthread_mutex_unlock(&g_lock);
    free(ring);
    if (was)
        vg_log("netip server: %s: CAM%d %s ended%s", c->peer_s, c->mon_ch + 1, c->mon_sub ? "sub" : "main",
               dropped ? ", the client was too slow for some pictures" : "");
}

/* Send what the queue holds, up to BURST bytes. -1 when the client is gone. */
static int mon_send(conn_t *c)
{
    uint8_t msg[NETIP_HDR_LEN + CHUNK];
    size_t sent = 0;
    pthread_mutex_lock(&g_lock);
    uint32_t sess = c->owner ? c->owner : c->session;
    pthread_mutex_unlock(&g_lock);
    for (;;) {
        pthread_mutex_lock(&c->qlock);
        size_t tail = c->tail, avail = c->ring ? c->head - c->tail : 0;
        pthread_mutex_unlock(&c->qlock);
        if (avail < 4 || sent >= BURST) return 0;
        /* The bytes from tail to head are the producer's no more: read
         * them without the lock. */
        uint32_t total;
        ring_get(c, tail, &total, 4);
        for (uint32_t off = 0; off < total;) {
            uint32_t n = total - off > CHUNK ? CHUNK : total - off;
            netip_hdr_t h = { .version = 1, .session = sess, .seq = c->mseq++, .total = (uint8_t)c->mon_ch,
                              .msgid = NETIP_MONITOR_DATA, .len = n };
            netip_hdr_put(msg, &h);
            ring_get(c, tail + 4 + off, msg + NETIP_HDR_LEN, n);
            if (send_all(c->fd, msg, NETIP_HDR_LEN + n) < 0) return -1;
            off += n;
        }
        sent += total;
        pthread_mutex_lock(&c->qlock);
        c->tail = tail + 4 + total;
        pthread_mutex_unlock(&c->qlock);
    }
}

/* ---- requests ------------------------------------------------------------------------- */

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int jint(const cJSON *o, const char *k, int dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) return v->valueint;
    if (cJSON_IsString(v)) return atoi(v->valuestring);
    return dflt;
}

/* Wrong logins per address: FAILS_MAX within LOCK_SECS lock it out for
 * LOCK_SECS (an 8-character hash is not much of a password). */
static int locked_out(uint32_t ip)
{
    time_t now = mono();
    int out = 0;
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < sizeof g_fail / sizeof g_fail[0]; ++i)
        if (g_fail[i].ip == ip && g_fail[i].until > now) out = 1;
    pthread_mutex_unlock(&g_lock);
    return out;
}

static void login_result(uint32_t ip, int ok)
{
    time_t now = mono();
    pthread_mutex_lock(&g_lock);
    size_t n = sizeof g_fail / sizeof g_fail[0], k = n, old = 0;
    for (size_t i = 0; i < n; ++i) {
        if (g_fail[i].ip == ip) k = i;
        if (g_fail[i].last < g_fail[old].last) old = i;
    }
    if (ok) {
        if (k < n) memset(&g_fail[k], 0, sizeof g_fail[k]);
    } else {
        if (k == n) {
            k = old;
            memset(&g_fail[k], 0, sizeof g_fail[k]);
            g_fail[k].ip = ip;
        }
        if (now - g_fail[k].last > LOCK_SECS) g_fail[k].fails = 0;
        g_fail[k].last = now;
        if (++g_fail[k].fails >= FAILS_MAX) {
            g_fail[k].fails = 0;
            g_fail[k].until = now + LOCK_SECS;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

static int do_login(conn_t *c, const netip_hdr_t *h, const cJSON *req)
{
    const char *user = jstr(req, "UserName"), *pass = jstr(req, "PassWord");
    if (!user || !pass) return reply_ret(c, h, "", 117) < 0 ? -1 : 0;
    if (locked_out(c->peer)) {
        vg_log("netip server: %s: login as '%s' refused: too many wrong ones, wait %d s", c->peer_s, user, LOCK_SECS);
        return reply_ret(c, h, "", 205) < 0 ? -1 : 0;
    }
    unsigned perms = 0;
    char why[200] = "";
    int ok = users_login_netip(user, pass, &perms, why, sizeof why);
    if (ok != 1) {
        login_result(c->peer, 0);
        vg_log("netip server: %s: login as '%.31s' refused%s%s", c->peer_s, user, why[0] ? ": " : "", why);
        reply_ret(c, h, "", 203);
        return -1;                          /* as XM devices: the connection ends */
    }
    login_result(c->peer, 1);
    pthread_mutex_lock(&g_lock);
    uint32_t s;
    for (int clash = 1; clash;) {
        s = ++g_next_session ? g_next_session : ++g_next_session;
        clash = 0;
        for (int i = 0; i < CONN_MAX; ++i) clash |= g_conn[i].used && g_conn[i].session == s;
    }
    c->session = s;
    c->perms = perms;
    snprintf(c->user, sizeof c->user, "%s", user);
    pthread_mutex_unlock(&g_lock);
    vg_log("netip server: %s: %s logged in", c->peer_s, c->user);
    cJSON *o = answer(c, "", 100);
    if (!o) return -1;
    cJSON_DeleteItemFromObject(o, "Name");
    cJSON_AddNumberToObject(o, "AliveInterval", ALIVE_SECS);
    cJSON_AddNumberToObject(o, "ChannelNum", channels_count());
    /* "DeviceType " with the space, as the documents and XM devices have
     * it; DVR is the documents' name for a recorder. */
    cJSON_AddStringToObject(o, "DeviceType ", "DVR");
    cJSON_AddNumberToObject(o, "ExtraChannel", 0);
    return reply(c, h, o);
}

static void put_time(char *out, size_t cap)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tm);
}

static void chan_name(int ch, char *out, size_t cap)
{
    channel_status_t st;
    channels_status(ch, &st);
    if (st.name[0]) snprintf(out, cap, "%s", st.name);
    else snprintf(out, cap, "CAM%d", ch + 1);
}

static cJSON *channel_titles(void)
{
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; a && i < channels_count(); ++i) {
        char n[48];
        chan_name(i, n, sizeof n);
        cJSON_AddItemToArray(a, cJSON_CreateString(n));
    }
    return a;
}

static cJSON *system_info(void)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    char up[16], hw[64];
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    snprintf(up, sizeof up, "0x%08X", (unsigned)(ts.tv_sec / 60));      /* minutes, as XM's */
    snprintf(hw, sizeof hw, "OpenIPC %s", g_platform);
    cJSON_AddNumberToObject(o, "AlarmInChannel", 0);
    cJSON_AddNumberToObject(o, "AlarmOutChannel", 0);
    cJSON_AddNumberToObject(o, "AudioInChannel", 0);
    cJSON_AddNumberToObject(o, "CombineSwitch", 0);
    cJSON_AddStringToObject(o, "DeviceModel", "vigil");
    cJSON_AddStringToObject(o, "DeviceRunTime", up);
    cJSON_AddNumberToObject(o, "DigChannel", channels_count());
    cJSON_AddStringToObject(o, "EncryptVersion", "Unknown");
    cJSON_AddNumberToObject(o, "ExtraChannel", 0);
    cJSON_AddStringToObject(o, "HardWare", hw);
    cJSON_AddStringToObject(o, "HardWareVersion", "Unknown");
    cJSON_AddStringToObject(o, "SerialNo", g_serial);
    cJSON_AddStringToObject(o, "SoftWareVersion", "V" VIGIL_VERSION ".vigil");
    cJSON_AddNumberToObject(o, "TalkInChannel", 0);
    cJSON_AddNumberToObject(o, "TalkOutChannel", 0);
    cJSON_AddStringToObject(o, "UpdataTime", "");
    cJSON_AddStringToObject(o, "UpdataType", "0x00000000");
    /* The cameras are network channels (an NVR's): no analog inputs. */
    cJSON_AddNumberToObject(o, "VideoInChannel", 0);
    cJSON_AddNumberToObject(o, "VideoOutChannel", 1);
    return o;
}

static cJSON *work_state(void)
{
    cJSON *o = cJSON_CreateObject(), *al = cJSON_CreateObject(), *chs = cJSON_CreateArray();
    if (!o || !al || !chs) {
        cJSON_Delete(o);
        cJSON_Delete(al);
        cJSON_Delete(chs);
        return NULL;
    }
    unsigned loss = 0, motion = 0;
    for (int i = 0; i < channels_count() && i < 32; ++i) {
        channel_status_t st;
        channels_status(i, &st);
        if (st.configured && !st.live) loss |= 1u << i;
        if (st.events & CH_EV_MOTION) motion |= 1u << i;
        cJSON *c = cJSON_CreateObject();
        if (!c) continue;
        cJSON_AddNumberToObject(c, "Bitrate", 0);
        cJSON_AddBoolToObject(c, "Record", st.recording);
        cJSON_AddItemToArray(chs, c);
    }
    cJSON_AddNumberToObject(al, "AlarmIn", 0);
    cJSON_AddNumberToObject(al, "AlarmOut", 0);
    cJSON_AddNumberToObject(al, "VideoBlind", 0);
    cJSON_AddNumberToObject(al, "VideoLoss", loss);
    cJSON_AddNumberToObject(al, "VideoMotion", motion);
    cJSON_AddItemToObject(o, "AlarmState", al);
    cJSON_AddItemToObject(o, "ChannelState", chs);
    return o;
}

static cJSON *net_common(conn_t *c)
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    uint32_t addr = getsockname(c->fd, (struct sockaddr *)&a, &al) == 0 ? ntohl(a.sin_addr.s_addr) : 0;
    disc_device_t d;
    this_device(addr, &d);
    uint8_t buf[2048];
    size_t n = disc_netip_reply(buf, sizeof buf, &d);
    if (!n) return NULL;
    cJSON *doc = cJSON_ParseWithLength((const char *)buf + NETIP_HDR_LEN, n - NETIP_HDR_LEN - 2);
    cJSON *nc = doc ? cJSON_DetachItemFromObject(doc, "NetWork.NetCommon") : NULL;
    cJSON_Delete(doc);
    return nc;
}

static const char *res_name(unsigned w, unsigned h)
{
    static const struct { unsigned w, h; const char *n; } names[] = {
        { 3840, 2160, "8M" }, { 2592, 1944, "5M" }, { 2560, 1440, "4M" }, { 2304, 1296, "3M" },
        { 2048, 1536, "3M" }, { 1920, 1080, "1080P" }, { 1280, 960, "960P" }, { 1280, 720, "720P" },
        { 960, 576, "960H" }, { 704, 576, "D1" }, { 640, 480, "VGA" }, { 640, 360, "nHD" },
        { 352, 288, "CIF" }, { 320, 240, "QVGA" },
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i)
        if (names[i].w == w && names[i].h == h) return names[i].n;
    return NULL;
}

static cJSON *stream_format(int ch, int sub)
{
    cJSON *f = cJSON_CreateObject(), *v = cJSON_CreateObject();
    if (!f || !v) {
        cJSON_Delete(f);
        cJSON_Delete(v);
        return NULL;
    }
    vg_stream_info_t si;
    vg_codec_t codec = VG_CODEC_NONE;
    int known = stream_tap_info(ch, sub, &si, &codec) == 0;
    channel_status_t st;
    channels_status(ch, &st);
    char wh[24];
    const char *res = known ? res_name(si.width, si.height) : NULL;
    if (known && !res) {
        snprintf(wh, sizeof wh, "%ux%u", si.width, si.height);
        res = wh;
    }
    cJSON_AddBoolToObject(f, "AudioEnable", 0);
    cJSON_AddBoolToObject(f, "VideoEnable", known);
    cJSON_AddStringToObject(v, "BitRateControl", "VBR");
    cJSON_AddStringToObject(v, "Compression", codec == VG_CODEC_H265 ? "H.265" : "H.264");
    cJSON_AddNumberToObject(v, "FPS", st.fps ? (int)st.fps : 25);
    cJSON_AddNumberToObject(v, "GOP", 2);
    cJSON_AddNumberToObject(v, "Quality", 4);
    cJSON_AddStringToObject(v, "Resolution", res ? res : "1080P");
    cJSON_AddItemToObject(f, "Video", v);
    return f;
}

static cJSON *simplify_encode(void)
{
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; a && i < channels_count(); ++i) {
        cJSON *e = cJSON_CreateObject();
        if (!e) break;
        cJSON *m = stream_format(i, 0), *x = stream_format(i, 1);
        if (m) cJSON_AddItemToObject(e, "MainFormat", m);
        if (x) cJSON_AddItemToObject(e, "ExtraFormat", x);
        cJSON_AddItemToArray(a, e);
    }
    return a;
}

static cJSON *general(void)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    char host[64] = "";
    gethostname(host, sizeof host - 1);
    cJSON_AddNumberToObject(o, "AutoLogout", 0);
    cJSON_AddNumberToObject(o, "LocalNo", 0);
    cJSON_AddStringToObject(o, "MachineName", host);
    cJSON_AddStringToObject(o, "OverWrite", "OverWrite");
    cJSON_AddNumberToObject(o, "ScreenAutoShutdown", 0);
    cJSON_AddNumberToObject(o, "ScreenSaveTime", 0);
    cJSON_AddStringToObject(o, "VideoOutPut", "Auto");
    return o;
}

static cJSON *location(void)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "DSTRule", "Off");
    cJSON_AddStringToObject(o, "DateFormat", "YYMMDD");
    cJSON_AddStringToObject(o, "DateSeparator", "-");
    cJSON_AddStringToObject(o, "Language", "English");
    cJSON_AddStringToObject(o, "TimeFormat", "24");
    cJSON_AddStringToObject(o, "VideoFormat", "PAL");
    cJSON_AddNumberToObject(o, "WorkDay", 62);
    return o;
}

static cJSON *system_function(void)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON *enc = cJSON_AddObjectToObject(o, "EncodeFunction");
    if (enc) {
        cJSON_AddBoolToObject(enc, "DoubleStream", 1);
        cJSON_AddBoolToObject(enc, "SnapStream", 0);
        cJSON_AddBoolToObject(enc, "WaterMark", 0);
    }
    cJSON *pre = cJSON_AddObjectToObject(o, "PreviewFunction");
    if (pre) {
        cJSON_AddBoolToObject(pre, "GUISet", 0);
        cJSON_AddBoolToObject(pre, "Talk", 0);
        cJSON_AddBoolToObject(pre, "Tour", 0);
    }
    cJSON *al = cJSON_AddObjectToObject(o, "AlarmFunction");
    if (al) cJSON_AddBoolToObject(al, "MotionDetect", 0);
    cJSON_AddObjectToObject(o, "CommFunction");
    cJSON_AddObjectToObject(o, "NetServerFunction");
    cJSON *oth = cJSON_AddObjectToObject(o, "OtherFunction");
    if (oth) cJSON_AddBoolToObject(oth, "SupportNetLocalSearch", 1);
    return o;
}

/* A configuration or information by name: NULL when we have no such. */
static cJSON *named(conn_t *c, int msgid, const char *name)
{
    if (msgid == NETIP_SYSINFO) {
        if (!strcmp(name, "SystemInfo")) return system_info();
        if (!strcmp(name, "WorkState")) return work_state();
        if (!strcmp(name, "StorageInfo")) return cJSON_CreateArray();
        return NULL;
    }
    if (msgid == NETIP_ABILITY_GET) return !strcmp(name, "SystemFunction") ? system_function() : NULL;
    if (!strcmp(name, "ChannelTitle")) return channel_titles();
    if (!strcmp(name, "General.General")) return general();
    if (!strcmp(name, "General")) {
        cJSON *o = cJSON_CreateObject();
        if (o) {
            cJSON_AddItemToObject(o, "General", general());
            cJSON_AddItemToObject(o, "Location", location());
        }
        return o;
    }
    if (!strcmp(name, "General.Location")) return location();
    if (!strcmp(name, "NetWork.NetCommon")) return net_common(c);
    if (!strcmp(name, "Simplify.Encode")) return simplify_encode();
    if (!strcmp(name, "SystemInfo")) return system_info();
    return NULL;
}

static int do_get(conn_t *c, const netip_hdr_t *h, const cJSON *req)
{
    const char *name = jstr(req, "Name");
    if (!name) name = "";
    if (h->msgid == NETIP_TIME_QUERY) {
        cJSON *o = answer(c, "OPTimeQuery", 100);
        char t[24];
        put_time(t, sizeof t);
        if (o) cJSON_AddStringToObject(o, "OPTimeQuery", t);
        return reply(c, h, o);
    }
    if (h->msgid == NETIP_TITLE_GET) name = "ChannelTitle";
    cJSON *v = named(c, h->msgid, name);
    if (!v) {
        tell_once(h->msgid, name, "no such configuration (607)");
        return reply_ret(c, h, name, 607);
    }
    cJSON *o = answer(c, name, 100);
    if (!o) {
        cJSON_Delete(v);
        return -1;
    }
    cJSON_AddItemToObject(o, name, v);
    return reply(c, h, o);
}

/* The logged-in connection of `session` (under g_lock), or NULL. */
static conn_t *session_conn(uint32_t session)
{
    for (int i = 0; i < CONN_MAX && session; ++i)
        if (g_conn[i].used && !g_conn[i].done && g_conn[i].session == session) return &g_conn[i];
    return NULL;
}

static void wake(conn_t *c)
{
    if (write(c->wake[1], "w", 1) < 0) { /* full: awake anyway */ }
}

static int do_monitor(conn_t *c, const netip_hdr_t *h, const cJSON *req)
{
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(req, "OPMonitor");
    const cJSON *par = cJSON_GetObjectItemCaseSensitive(op, "Parameter");
    const char *action = jstr(op, "Action");
    int ch = jint(par, "Channel", 0), sub = netip_stream_type(jstr(par, "StreamType"));
    if (sub < 0) sub = 0;
    int claim = h->msgid == NETIP_MONITOR_CLAIM;
    if (!claim && !action) return reply_ret(c, h, "OPMonitor", 117) < 0 ? -1 : 0;
    int start = !claim && !strcasecmp(action, "Start"), stop = !claim && !strcasecmp(action, "Stop");

    pthread_mutex_lock(&g_lock);
    if (claim) {
        int ret = 100;
        if (!c->session && !c->owner) {
            /* A new connection claiming a logged-in session's media. */
            uint32_t s = h->session;
            const char *js = jstr(req, "SessionID");
            if (js) netip_session_parse(js, &s);
            conn_t *o = session_conn(s);
            if (!o) ret = 105;
            else {
                c->owner = s;
                c->perms = o->perms;
                snprintf(c->user, sizeof c->user, "%s", o->user);
            }
        }
        if (ret == 100 && !(c->perms & PERM_LIVE)) ret = 107;
        if (ret == 100 && (ch < 0 || ch >= channels_count())) ret = 103;
        if (ret == 100 && c->mon != MON_NONE) ret = 103;        /* one stream per connection */
        int full = ret == 100 && reserve(c, sub) < 0;
        if (full) ret = 103;
        if (ret == 100) {
            c->mon = MON_CLAIMED;
            c->mon_ch = ch;
            c->mon_sub = sub;
            c->mon_self = c->session != 0;
            /* Its start came first (on the logged-in connection). */
            conn_t *o = c->owner ? session_conn(c->owner) : NULL;
            if (o && o->pend && o->pend_ch == ch && o->pend_sub == sub) {
                o->pend = 0;
                c->mon = MON_START;
            }
        }
        pthread_mutex_unlock(&g_lock);
        if (full)
            vg_log("netip server: %s: CAM%d %s refused: %d streams or %u KiB of queues at most", c->peer_s, ch + 1,
                   sub ? "sub" : "main", STREAMS_MAX, RING_BUDGET >> 10);
        else if (ret != 100)
            vg_log("netip server: %s: claim of CAM%d %s refused (Ret %d)", c->peer_s, ch + 1, sub ? "sub" : "main", ret);
        if (reply_ret(c, h, "OPMonitor", ret) < 0) return -1;
        return ret == 100 || c->session ? 0 : -1;
    }
    if (!c->session && c->owner) {
        /* A start on the media connection itself: no answer among the media. */
        if (start && c->mon == MON_CLAIMED) c->mon = MON_START;
        if (stop) atomic_store(&c->kill, 1);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (c->mon_self && c->mon != MON_NONE && (start || stop)) {
        /* python-dvr, vigil: the media on this connection, read at once. */
        if (start && c->mon == MON_CLAIMED) c->mon = MON_START;
        int stopping = stop;
        pthread_mutex_unlock(&g_lock);
        if (stopping) {
            mon_stop(c);
            return reply_ret(c, h, "OPMonitor", 100);
        }
        return 0;
    }
    int ret = 100;
    conn_t *m = NULL;
    for (int i = 0; i < CONN_MAX && !m; ++i) {
        conn_t *x = &g_conn[i];
        if (x->used && !x->done && x->owner == c->session && x->mon != MON_NONE && x->mon_ch == ch &&
            x->mon_sub == sub)
            m = x;
    }
    if (!(c->perms & PERM_LIVE)) ret = 107;
    else if (start && m && m->mon == MON_CLAIMED) {
        m->mon = MON_START;
        wake(m);
    } else if (start && !m) {
        c->pend = 1;                        /* its claim is still to come */
        c->pend_ch = ch;
        c->pend_sub = sub;
    } else if (stop && m) {
        atomic_store(&m->kill, 1);
        wake(m);
    } else if (!start && !stop) {
        tell_once(h->msgid, action, "OPMonitor action not served (OK)");
    }
    pthread_mutex_unlock(&g_lock);
    return reply_ret(c, h, "OPMonitor", ret);
}

static int is_write(int msgid)
{
    switch (msgid) {
    case NETIP_CONFIG_SET: case NETIP_TITLE_SET: case 1050:    /* the title's bitmap */
    case 1400:                                                 /* PTZ */
    case 1450:                                                 /* reboot, set the time, ... */
    case 1460:                                                 /* disks */
    case 1476: case 1478: case 1480: case 1482: case 1484: case 1486: case 1488:   /* users */
    case 1500: case 1502:                                      /* arm, disarm */
    case 1520: case 1522:                                      /* upgrade */
    case 1532: case 1540: case 1590:                           /* IP set, config import, time sync */
        return 1;
    }
    return 0;
}

/* One request. -1 ends the connection. */
static int request(conn_t *c, const netip_hdr_t *h, const uint8_t *body, size_t len)
{
    while (len && (body[len - 1] == 0 || body[len - 1] == '\n')) --len;
    cJSON *req = len ? cJSON_ParseWithLength((const char *)body, len) : cJSON_CreateObject();
    if (!req) req = cJSON_CreateObject();
    if (!req) return -1;
    const char *name = jstr(req, "Name");
    int rc = 0;
    pthread_mutex_lock(&g_lock);
    int logged = c->session != 0, media = c->owner != 0;
    pthread_mutex_unlock(&g_lock);

    if (h->msgid == NETIP_LOGIN) {
        rc = logged ? reply_ret(c, h, "", 104) : do_login(c, h, req);
    } else if (h->msgid == NETIP_MONITOR_CLAIM || h->msgid == NETIP_MONITOR) {
        rc = do_monitor(c, h, req);
    } else if (media) {
        /* A media connection only claims and streams; keepalives and the
         * rest go over the logged-in one. */
        if (h->msgid == NETIP_KEEPALIVE) rc = 0;
        else tell_once(h->msgid, name ? name : "", "on a media connection, ignored");
    } else if (!logged) {
        rc = reply_ret(c, h, name ? name : "", 105);   /* not logged in */
    } else if (h->msgid == NETIP_KEEPALIVE) {
        rc = reply_ret(c, h, "KeepAlive", 100);
    } else if (h->msgid == NETIP_LOGOUT) {
        reply_ret(c, h, "", 100);
        rc = -1;
    } else if (h->msgid == NETIP_SYSINFO || h->msgid == NETIP_CONFIG_GET || h->msgid == NETIP_DEFAULT_GET ||
               h->msgid == NETIP_TITLE_GET || h->msgid == NETIP_ABILITY_GET || h->msgid == NETIP_TIME_QUERY) {
        rc = do_get(c, h, req);
    } else if (is_write(h->msgid)) {
        tell_once(h->msgid, name ? name : "", "refused, read-only (107)");
        rc = reply_ret(c, h, name ? name : "", 107);
    } else {
        tell_once(h->msgid, name ? name : "", "not served, answered OK");
        rc = reply_ret(c, h, name ? name : "", 100);
    }
    cJSON_Delete(req);
    return rc;
}

/* Read what has come; each whole message goes to request(). -1: closed. */
static int receive(conn_t *c)
{
    size_t room = NETIP_HDR_LEN + REQ_MAX - c->rx_len;
    ssize_t r = recv(c->fd, c->rx + c->rx_len, room, MSG_DONTWAIT);
    if (r == 0) return -1;
    if (r < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
    c->rx_len += (size_t)r;
    c->last_rx = mono();
    for (;;) {
        netip_hdr_t h;
        if (c->rx_len < NETIP_HDR_LEN) return 0;
        if (netip_hdr_get(c->rx, &h) < 0 || h.len > REQ_MAX) {
            vg_log("netip server: %s: not NETIP, closing", c->peer_s);
            return -1;
        }
        size_t whole = NETIP_HDR_LEN + h.len;
        if (c->rx_len < whole) return 0;
        if (request(c, &h, c->rx + NETIP_HDR_LEN, h.len) < 0) return -1;
        memmove(c->rx, c->rx + whole, c->rx_len - whole);
        c->rx_len -= whole;
    }
}

/* ---- connections ----------------------------------------------------------------------- */

static void *conn_main(void *arg)
{
    conn_t *c = arg;
    c->rx = malloc(NETIP_HDR_LEN + REQ_MAX);
    while (c->rx && !atomic_load(&g_stop) && !atomic_load(&c->kill)) {
        pthread_mutex_lock(&g_lock);
        int state = c->mon, logged = c->session != 0, self = c->mon_self;
        uint32_t owner = c->owner;
        int owner_gone = owner && !session_conn(owner);
        pthread_mutex_unlock(&g_lock);
        time_t now = mono();
        if (owner_gone) break;                              /* its session logged out */
        if (!logged && !owner && now - c->since > LOGIN_SECS) break;
        if (logged && !(self && state != MON_NONE) && now - c->last_rx > 3 * ALIVE_SECS) {
            vg_log("netip server: %s: no keepalive for %d s, closing", c->peer_s, 3 * ALIVE_SECS);
            break;
        }
        if (state == MON_START && mon_start(c) < 0) break;
        int queued = 0;
        if (state == MON_ON || state == MON_START) {
            pthread_mutex_lock(&c->qlock);
            queued = c->ring && c->head - c->tail >= 4;
            pthread_mutex_unlock(&c->qlock);
        }
        struct pollfd pf[2] = { { .fd = c->fd, .events = POLLIN }, { .fd = c->wake[0], .events = POLLIN } };
        if (poll(pf, 2, queued ? 0 : 1000) < 0 && errno != EINTR) break;
        if (pf[1].revents & POLLIN) {
            char b[64];
            while (read(c->wake[0], b, sizeof b) > 0) {}
        }
        if ((pf[0].revents & (POLLIN | POLLHUP | POLLERR)) && receive(c) < 0) break;
        if (queued && mon_send(c) < 0) break;
    }
    mon_stop(c);
    pthread_mutex_lock(&g_lock);
    if (c->session) vg_log("netip server: %s: %s logged out", c->peer_s, c->user);
    c->session = c->owner = 0;
    c->done = 1;
    pthread_mutex_unlock(&g_lock);
    close(c->fd);
    c->fd = -1;
    free(c->rx);
    c->rx = NULL;
    return NULL;
}

/* Join the connections that ended (listener thread, or the stop). */
static void reap(int all)
{
    for (int i = 0; i < CONN_MAX; ++i) {
        conn_t *c = &g_conn[i];
        pthread_mutex_lock(&g_lock);
        int join = c->used && (c->done || all);
        pthread_mutex_unlock(&g_lock);
        if (!join) continue;
        pthread_join(c->th, NULL);
        close(c->wake[0]);
        close(c->wake[1]);
        pthread_mutex_destroy(&c->qlock);
        pthread_mutex_lock(&g_lock);
        c->used = 0;
        pthread_mutex_unlock(&g_lock);
    }
}

static void accept_one(void)
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int fd = accept4(g_tcp, (struct sockaddr *)&a, &al, SOCK_CLOEXEC);
    if (fd < 0) return;
    conn_t *c = NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < CONN_MAX && !c; ++i)
        if (!g_conn[i].used) c = &g_conn[i];
    if (c) {
        memset(c, 0, sizeof *c);
        c->used = 1;
    }
    pthread_mutex_unlock(&g_lock);
    if (!c) {
        vg_log("netip server: %d connections already; refusing another", CONN_MAX);
        close(fd);
        return;
    }
    c->fd = fd;
    c->peer = ntohl(a.sin_addr.s_addr);
    inet_ntop(AF_INET, &a.sin_addr, c->peer_s, sizeof c->peer_s);
    c->since = c->last_rx = mono();
    struct timeval tv = { .tv_sec = SEND_SECS };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    pthread_mutex_init(&c->qlock, NULL);
    int ok = pipe2(c->wake, O_CLOEXEC | O_NONBLOCK) == 0;
    if (ok && pthread_create(&c->th, NULL, conn_main, c) == 0) return;
    if (ok) {
        close(c->wake[0]);
        close(c->wake[1]);
    }
    pthread_mutex_destroy(&c->qlock);
    close(fd);
    pthread_mutex_lock(&g_lock);
    c->used = 0;
    pthread_mutex_unlock(&g_lock);
}

/* ---- the LAN search (UDP 34569) --------------------------------------------------------- */

static void search_answer(void)
{
    static time_t sec;
    static int in_sec;
    uint8_t pkt[512];
    char cbuf[CMSG_SPACE(sizeof(struct in_pktinfo))];
    struct sockaddr_in from;
    struct iovec iov = { .iov_base = pkt, .iov_len = sizeof pkt };
    struct msghdr mh = { .msg_name = &from, .msg_namelen = sizeof from, .msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = cbuf, .msg_controllen = sizeof cbuf };
    ssize_t r = recvmsg(g_udp, &mh, MSG_DONTWAIT);
    if (r <= 0 || !disc_netip_is_search(pkt, (size_t)r)) return;
    time_t now = mono();
    if (now != sec) sec = now, in_sec = 0;
    if (++in_sec > SEARCH_RATE) return;         /* not an amplifier */
    struct in_pktinfo pi = { 0 };
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm))
        if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_PKTINFO) memcpy(&pi, CMSG_DATA(cm), sizeof pi);
    /* Answer with the address of the interface the search came in on. */
    uint32_t local = ntohl(pi.ipi_spec_dst.s_addr);
    char ifn[IF_NAMESIZE] = "";
    if (pi.ipi_ifindex && if_indextoname((unsigned)pi.ipi_ifindex, ifn)) {
        net_status_t st;
        net_status(ifn, &st);
        uint32_t a;
        if (disc_ip_parse(st.address, &a) == 0) local = a;
    }
    if (!local) return;
    disc_device_t d;
    this_device(local, &d);
    uint8_t out[1024];                          /* XM's search tools read 1 KiB */
    size_t n = disc_netip_reply(out, sizeof out, &d);
    if (!n) return;
    /* XM devices answer to the broadcast address, port 34569, where the
     * searching tools listen; a search from another port (vigil asking
     * an address) also gets the answer back where it came from. */
    struct in_pktinfo spi = { .ipi_ifindex = pi.ipi_ifindex };
    char sbuf[CMSG_SPACE(sizeof(struct in_pktinfo))];
    memset(sbuf, 0, sizeof sbuf);
    struct iovec oiov = { .iov_base = out, .iov_len = n };
    struct sockaddr_in bc = { .sin_family = AF_INET, .sin_port = htons(DISC_NETIP_UDP),
                              .sin_addr.s_addr = htonl(0xffffffffu) };
    struct msghdr om = { .msg_name = &bc, .msg_namelen = sizeof bc, .msg_iov = &oiov, .msg_iovlen = 1,
                         .msg_control = sbuf, .msg_controllen = sizeof sbuf };
    struct cmsghdr *cm = CMSG_FIRSTHDR(&om);
    cm->cmsg_level = IPPROTO_IP;
    cm->cmsg_type = IP_PKTINFO;
    cm->cmsg_len = CMSG_LEN(sizeof spi);
    memcpy(CMSG_DATA(cm), &spi, sizeof spi);
    if ((local >> 24) != 127 && pi.ipi_ifindex) sendmsg(g_udp, &om, 0);
    if (ntohs(from.sin_port) != DISC_NETIP_UDP || (local >> 24) == 127)
        sendto(g_udp, out, n, 0, (struct sockaddr *)&from, sizeof from);
}

static void *listen_main(void *arg)
{
    (void)arg;
    while (!atomic_load(&g_stop)) {
        struct pollfd pf[3] = { { .fd = g_tcp, .events = POLLIN }, { .fd = g_udp, .events = POLLIN },
                                { .fd = g_stop_pipe[0], .events = POLLIN } };
        int r = poll(pf, 3, 1000);
        if (r < 0 && errno != EINTR) break;
        if (pf[0].revents & POLLIN) accept_one();
        if (pf[1].revents & POLLIN) search_answer();
        reap(0);
    }
    return NULL;
}

static int open_tcp(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int open_udp(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    /* Shared with the camera search's own socket (discovery.c), which
     * listens there for the devices' broadcast answers. */
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(DISC_NETIP_UDP) };
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int netip_server_start(const config_netip_server_t *cfg, const char *platform)
{
    if (platform) snprintf(g_platform, sizeof g_platform, "%s", platform);
    g_cfg = *cfg;
    if (!cfg->enabled || g_running) return 0;
    g_tcp = open_tcp(cfg->port);
    if (g_tcp < 0) {
        vg_log("netip server: cannot listen on TCP %d: %s", cfg->port, strerror(errno));
        return -1;
    }
    g_udp = open_udp();
    if (g_udp < 0) vg_log("netip server: cannot answer searches on UDP %d: %s", DISC_NETIP_UDP, strerror(errno));
    if (pipe2(g_stop_pipe, O_CLOEXEC | O_NONBLOCK) < 0) {
        close(g_tcp);
        if (g_udp >= 0) close(g_udp);
        g_tcp = g_udp = -1;
        return -1;
    }
    if (!g_next_session) {
        uint32_t r = (uint32_t)time(NULL) ^ (uint32_t)getpid() << 16;
        g_next_session = r & 0x7fffffu;
    }
    make_serial();
    atomic_store(&g_stop, 0);
    if (pthread_create(&g_listen_th, NULL, listen_main, NULL) != 0) {
        close(g_tcp);
        if (g_udp >= 0) close(g_udp);
        close(g_stop_pipe[0]);
        close(g_stop_pipe[1]);
        g_tcp = g_udp = -1;
        return -1;
    }
    g_running = 1;
    disc_set_self(g_serial, cfg->port);
    stream_tap_standby(1);
    vg_log("netip server: on TCP %d%s (serial %s)", cfg->port, g_udp >= 0 ? ", answering searches" : "",
           g_serial);
    return 0;
}

void netip_server_stop(void)
{
    if (!g_running) return;
    atomic_store(&g_stop, 1);
    if (write(g_stop_pipe[1], "s", 1) < 0) { /* the poll's timeout ends it */ }
    pthread_join(g_listen_th, NULL);
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < CONN_MAX; ++i)
        if (g_conn[i].used && !g_conn[i].done) {
            atomic_store(&g_conn[i].kill, 1);
            shutdown(g_conn[i].fd, SHUT_RDWR);
            wake(&g_conn[i]);
        }
    pthread_mutex_unlock(&g_lock);
    reap(1);
    close(g_tcp);
    if (g_udp >= 0) close(g_udp);
    close(g_stop_pipe[0]);
    close(g_stop_pipe[1]);
    g_tcp = g_udp = g_stop_pipe[0] = g_stop_pipe[1] = -1;
    g_running = 0;
    disc_set_self("", 0);
    stream_tap_standby(0);
    vg_log("netip server: stopped");
}

void netip_server_apply(const config_netip_server_t *cfg)
{
    if (cfg->enabled == g_cfg.enabled && cfg->port == g_cfg.port) return;
    netip_server_stop();
    netip_server_start(cfg, NULL);
}
