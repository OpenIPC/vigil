/* SPDX-License-Identifier: GPL-3.0-or-later */
/* qos.c — DSCP marking of vigild's sockets (qos.h). */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>

#include "qos.h"

/* Sockets marked again when the values change: camera sessions, the
 * servers' listeners and WebSockets; a few dozen on a 16-channel NVR. */
#define TRACK_MAX 256
/* Scanning for a server's sockets (qos_mark_listeners, qos_mark_peer)
 * stops here even when the fd limit is higher. */
#define SCAN_FDS_MAX 8192

typedef struct {
    int                     fd;     /* -1: free */
    qos_class_t             cls;
    struct sockaddr_storage local, peer;
    socklen_t               local_len, peer_len;    /* peer_len 0: a listener */
} track_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_dscp[QOS_CLASSES];
static track_t         g_track[TRACK_MAX];
static int             g_track_init;

int qos_tos(int dscp)
{
    return dscp < 0 || dscp > QOS_DSCP_MAX ? -1 : dscp << 2;
}

const char *qos_dscp_name(int dscp)
{
    static const struct { int v; const char *n; } names[] = {
        { 0, "CS0" },   { 1, "LE" },    { 8, "CS1" },   { 10, "AF11" }, { 12, "AF12" }, { 14, "AF13" },
        { 16, "CS2" },  { 18, "AF21" }, { 20, "AF22" }, { 22, "AF23" }, { 24, "CS3" },  { 26, "AF31" },
        { 28, "AF32" }, { 30, "AF33" }, { 32, "CS4" },  { 34, "AF41" }, { 36, "AF42" }, { 38, "AF43" },
        { 40, "CS5" },  { 44, "VA" },   { 46, "EF" },   { 48, "CS6" },  { 56, "CS7" },
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i)
        if (names[i].v == dscp) return names[i].n;
    return NULL;
}

int qos_dscp(qos_class_t c)
{
    if ((unsigned)c >= QOS_CLASSES) return 0;
    pthread_mutex_lock(&g_lock);
    int v = g_dscp[c];
    pthread_mutex_unlock(&g_lock);
    return v;
}

int qos_apply(int fd, int dscp)
{
    int tos = qos_tos(dscp);
    if (tos < 0) {
        errno = EINVAL;
        return -1;
    }
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    if (getsockname(fd, (struct sockaddr *)&ss, &len) < 0) return -1;
    if (ss.ss_family == AF_INET) return setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
    if (ss.ss_family == AF_INET6) {
        int rc = setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &tos, sizeof tos);
        /* A dual-stack socket sends IPv4 (to ::ffff:a.b.c.d) with IP_TOS. */
        setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
        return rc;
    }
    errno = EAFNOSUPPORT;
    return -1;
}

static void track_init(void)
{
    if (g_track_init) return;
    for (int i = 0; i < TRACK_MAX; ++i) g_track[i].fd = -1;
    g_track_init = 1;
}

/* The socket behind t->fd is still the one tracked. */
static int same_socket(const track_t *t)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    if (getsockname(t->fd, (struct sockaddr *)&ss, &len) < 0 || len != t->local_len ||
        memcmp(&ss, &t->local, len))
        return 0;
    len = sizeof ss;
    int connected = getpeername(t->fd, (struct sockaddr *)&ss, &len) == 0;
    if (!t->peer_len) return !connected;
    return connected && len == t->peer_len && !memcmp(&ss, &t->peer, len);
}

/* Under g_lock. */
static void track(int fd, qos_class_t c)
{
    track_init();
    track_t n = { .fd = fd, .cls = c };
    n.local_len = sizeof n.local;
    if (getsockname(fd, (struct sockaddr *)&n.local, &n.local_len) < 0) return;
    n.peer_len = sizeof n.peer;
    if (getpeername(fd, (struct sockaddr *)&n.peer, &n.peer_len) < 0) n.peer_len = 0;
    int slot = -1;
    for (int i = 0; i < TRACK_MAX; ++i) {
        if (g_track[i].fd == fd) {          /* the number is reused: the old socket is gone */
            g_track[i] = n;
            return;
        }
        if (slot < 0 && g_track[i].fd < 0) slot = i;
    }
    if (slot < 0)                           /* full: drop the closed ones */
        for (int i = 0; i < TRACK_MAX; ++i)
            if (!same_socket(&g_track[i])) {
                g_track[i].fd = -1;
                if (slot < 0) slot = i;
            }
    if (slot >= 0) g_track[slot] = n;
}

int qos_mark(int fd, qos_class_t c, int track_it)
{
    if ((unsigned)c >= QOS_CLASSES || fd < 0) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&g_lock);
    int rc = qos_apply(fd, g_dscp[c]);
    if (rc == 0 && track_it) track(fd, c);
    pthread_mutex_unlock(&g_lock);
    return rc;
}

void qos_forget(int fd)
{
    pthread_mutex_lock(&g_lock);
    track_init();
    for (int i = 0; i < TRACK_MAX; ++i)
        if (g_track[i].fd == fd) g_track[i].fd = -1;
    pthread_mutex_unlock(&g_lock);
}

int qos_set(const int dscp[QOS_CLASSES])
{
    int n = 0;
    pthread_mutex_lock(&g_lock);
    track_init();
    for (int c = 0; c < QOS_CLASSES; ++c) g_dscp[c] = qos_tos(dscp[c]) < 0 ? 0 : dscp[c];
    for (int i = 0; i < TRACK_MAX; ++i) {
        track_t *t = &g_track[i];
        if (t->fd < 0) continue;
        if (!same_socket(t)) t->fd = -1;
        else if (qos_apply(t->fd, g_dscp[t->cls]) == 0) ++n;
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

static int port_of(const struct sockaddr_storage *ss)
{
    if (ss->ss_family == AF_INET) return ntohs(((const struct sockaddr_in *)(const void *)ss)->sin_port);
    if (ss->ss_family == AF_INET6) return ntohs(((const struct sockaddr_in6 *)(const void *)ss)->sin6_port);
    return -1;
}

/* "a.b.c.d" or "x::y"; an IPv4-mapped IPv6 address as its IPv4 form. */
static void addr_text(const struct sockaddr_storage *ss, char *out, size_t cap)
{
    out[0] = 0;
    if (ss->ss_family == AF_INET) {
        inet_ntop(AF_INET, &((const struct sockaddr_in *)(const void *)ss)->sin_addr, out, (socklen_t)cap);
    } else if (ss->ss_family == AF_INET6) {
        const struct in6_addr *a = &((const struct sockaddr_in6 *)(const void *)ss)->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(a)) inet_ntop(AF_INET, &a->s6_addr[12], out, (socklen_t)cap);
        else inet_ntop(AF_INET6, a, out, (socklen_t)cap);
    }
}

static int scan_limit(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) < 0 || rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > SCAN_FDS_MAX)
        return SCAN_FDS_MAX;
    return (int)rl.rlim_cur;
}

static int is_stream(int fd, int want_listen)
{
    int type = 0, acc = 0;
    socklen_t l = sizeof type;
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &l) < 0 || type != SOCK_STREAM) return 0;
    l = sizeof acc;
    if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &acc, &l) < 0) return 0;
    return !!acc == !!want_listen;
}

int qos_mark_listeners(int port, qos_class_t c)
{
    int n = 0, max = scan_limit();
    for (int fd = 0; fd < max; ++fd) {
        struct sockaddr_storage ss;
        socklen_t len = sizeof ss;
        if (getsockname(fd, (struct sockaddr *)&ss, &len) < 0 || port_of(&ss) != port) continue;
        if (is_stream(fd, 1) && qos_mark(fd, c, 1) == 0) ++n;
    }
    return n;
}

int qos_mark_peer(int local_port, const char *peer_addr, int peer_port, qos_class_t c)
{
    int max = scan_limit();
    for (int fd = 0; fd < max; ++fd) {
        struct sockaddr_storage ss;
        socklen_t len = sizeof ss;
        if (getsockname(fd, (struct sockaddr *)&ss, &len) < 0 || port_of(&ss) != local_port) continue;
        len = sizeof ss;
        if (getpeername(fd, (struct sockaddr *)&ss, &len) < 0 || port_of(&ss) != peer_port) continue;
        char a[INET6_ADDRSTRLEN];
        addr_text(&ss, a, sizeof a);
        if (strcmp(a, peer_addr) || !is_stream(fd, 0)) continue;
        return qos_mark(fd, c, 1);
    }
    errno = ENOENT;
    return -1;
}
