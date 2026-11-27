/* SPDX-License-Identifier: GPL-3.0-or-later */
/* discovery.c — the device table, the scanner and the jobs (see
 * discovery.h). */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

#include "cJSON.h"
#include "config.h"
#include "discovery.h"
#include "qos.h"
#include "log.h"
#include "schema.h"
#include "source.h"

#define IFACE_MAX   16
#define ROUNDS      3                /* searches per scan, a second apart */
#define REPROBE_S   10               /* look for a device at its new address this long */
#define NETIP_SET_S 3                /* wait for the broadcast set's answer */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static disc_device_t   g_dev[DISC_MAX];
static int             g_ndev;
static disc_job_t      g_job;
static pthread_t       g_thread;
static int             g_thread_up;
static volatile int    g_stop;
static int             g_max_ch = CHANNEL_MAX;
static unsigned        g_prefer[3] = { DISC_NETIP, DISC_ONVIF, DISC_DHIP };
static disc_scan_opts_t g_last;      /* the last scan's interfaces, targets and ports */
static unsigned        g_gen;        /* the table's generation: moves with every change */
static unsigned        g_scan;       /* the id of the last scan job */

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
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void wipe(void *p, size_t n)
{
    volatile unsigned char *w = p;
    while (n--) *w++ = 0;
}

void disc_set_max_channels(int n)
{
    if (n > 0) g_max_ch = n < CHANNEL_MAX ? n : CHANNEL_MAX;
}

int disc_set_prefer(const char *list)
{
    unsigned order[3] = { 0 }, seen = 0;
    int k = 0;
    for (const char *p = list; *p && k < 3; ) {
        while (*p == ',' || *p == ' ' || *p == '>') ++p;
        size_t n = strcspn(p, ", >");
        if (!n) break;
        char w[8];
        if (n >= sizeof w) return -1;
        memcpy(w, p, n);
        w[n] = 0;
        unsigned b = disc_proto_bits(w);
        if (!b || (b & (b - 1))) return -1;
        if (!(seen & b)) order[k++] = b;
        seen |= b;
        p += n;
    }
    /* What the list leaves out comes after, in the default order. */
    static const unsigned dflt[3] = { DISC_NETIP, DISC_ONVIF, DISC_DHIP };
    for (int i = 0; i < 3 && k < 3; ++i)
        if (!(seen & dflt[i])) order[k++] = dflt[i];
    pthread_mutex_lock(&g_lock);
    memcpy(g_prefer, order, sizeof g_prefer);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* ---- the table ------------------------------------------------------------------- */

static void expire(void)
{
    time_t now = mono();
    int k = 0;
    for (int i = 0; i < g_ndev; ++i)
        if (now - g_dev[i].seen <= DISC_TTL) g_dev[k++] = g_dev[i];
    if (k != g_ndev) ++g_gen;
    g_ndev = k;
}

/* Each answer as it comes: the screen and the API see a device the
 * moment it is heard, not when the scan ends. */
static void table_add(const disc_device_t *in)
{
    pthread_mutex_lock(&g_lock);
    if (disc_table_add(g_dev, &g_ndev, DISC_MAX, in)) ++g_gen;
    pthread_mutex_unlock(&g_lock);
}

unsigned disc_generation(void)
{
    pthread_mutex_lock(&g_lock);
    expire();
    unsigned g = g_gen;
    pthread_mutex_unlock(&g_lock);
    return g;
}

unsigned disc_last_scan(void)
{
    pthread_mutex_lock(&g_lock);
    unsigned id = g_scan;
    pthread_mutex_unlock(&g_lock);
    return id;
}

static int cmp_ip(const void *a, const void *b)
{
    uint32_t x = 0, y = 0;
    disc_ip_parse(((const disc_device_t *)a)->ip, &x);
    disc_ip_parse(((const disc_device_t *)b)->ip, &y);
    return x < y ? -1 : x > y;
}

int disc_devices(disc_device_t *out, int max)
{
    pthread_mutex_lock(&g_lock);
    expire();
    int n = g_ndev < max ? g_ndev : max;
    memcpy(out, g_dev, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&g_lock);
    qsort(out, (size_t)n, sizeof *out, cmp_ip);
    return n;
}

void disc_clear(void)
{
    pthread_mutex_lock(&g_lock);
    g_ndev = 0;
    ++g_gen;
    pthread_mutex_unlock(&g_lock);
}

/* A device by MAC or address (copied out). */
static int table_find(const char *key, disc_device_t *out)
{
    char mac[18];
    int is_mac = disc_mac_norm(key, mac, sizeof mac) == 0;
    int found = 0;
    pthread_mutex_lock(&g_lock);
    expire();
    for (int i = 0; i < g_ndev && !found; ++i)
        if (is_mac ? !strcmp(g_dev[i].mac, mac) : !strcmp(g_dev[i].ip, key)) {
            *out = g_dev[i];
            found = 1;
        }
    pthread_mutex_unlock(&g_lock);
    return found;
}

static void device_key(const disc_device_t *d, char *out, size_t cap)
{
    snprintf(out, cap, "%s", d->mac[0] ? d->mac : d->ip);
}

static int channel_of(const vigil_config_t *c, const char *ip)
{
    for (int i = 0; i < c->n_cameras; ++i) {
        char host[128];
        disc_url_host(c->cameras[i].url, host, sizeof host);
        if (host[0] && !strcmp(host, ip)) return i + 1;
    }
    return 0;
}

int disc_device_channel(const disc_device_t *d)
{
    vigil_config_t *c = malloc(sizeof *c);
    if (!c) return 0;
    config_get(c);
    int ch = d->ip[0] ? channel_of(c, d->ip) : 0;
    free(c);
    return ch;
}

/* ---- jobs ------------------------------------------------------------------------- */

static void job_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void job_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_lock);
    vsnprintf(g_job.msg, sizeof g_job.msg, fmt, ap);
    pthread_mutex_unlock(&g_lock);
    va_end(ap);
}

static void job_progress(int pct)
{
    pthread_mutex_lock(&g_lock);
    g_job.progress = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    pthread_mutex_unlock(&g_lock);
}

static time_t g_auth_fail[DISC_AUTH_FAILS];   /* ring of refusal times */
static int    g_auth_i;

int disc_auth_wait(void)
{
    time_t now = mono();
    pthread_mutex_lock(&g_lock);
    time_t oldest = g_auth_fail[g_auth_i];
    pthread_mutex_unlock(&g_lock);
    return oldest && now - oldest < DISC_AUTH_WINDOW ? (int)(DISC_AUTH_WINDOW - (now - oldest)) : 0;
}

static void job_result(int i, const disc_result_t *r)
{
    pthread_mutex_lock(&g_lock);
    if (r->done && r->rc == DISC_EAUTH && (i >= g_job.n || g_job.r[i].rc != DISC_EAUTH || !g_job.r[i].done)) {
        time_t now = mono();
        g_auth_fail[g_auth_i] = now ? now : 1;
        g_auth_i = (g_auth_i + 1) % DISC_AUTH_FAILS;
    }
    if (i >= 0 && i < DISC_JOB_MAX) {
        g_job.r[i] = *r;
        if (g_job.n < i + 1) g_job.n = i + 1;
    }
    pthread_mutex_unlock(&g_lock);
}

void disc_job_get(disc_job_t *out)
{
    pthread_mutex_lock(&g_lock);
    *out = g_job;
    out->found = 0;
    if (g_job.kind == DISC_JOB_SCAN)
        for (int i = 0; i < g_ndev; ++i) out->found += g_dev[i].scan == g_job.id;
    pthread_mutex_unlock(&g_lock);
}

typedef struct {
    disc_job_kind_t  kind;
    disc_scan_opts_t scan;
    disc_ip_item_t   items[DISC_JOB_MAX];
    int              n;
    char             start[16], mask[16], gw[16];
    char             user[64], pass[64], name[32];
    char             key[24];
    int              channel, dry_run, replace;
    unsigned         proto;
    disc_manual_t    manual;        /* TEST, and ASSIGN with is_manual */
    int              is_manual;
} job_args_t;

static void *job_main(void *arg);

static int job_start(job_args_t *a)
{
    pthread_mutex_lock(&g_lock);
    if (g_job.running) {
        pthread_mutex_unlock(&g_lock);
        wipe(a, sizeof *a);
        free(a);
        errno = EBUSY;
        return -1;
    }
    int up = g_thread_up;
    g_thread_up = 0;
    pthread_mutex_unlock(&g_lock);
    if (up) pthread_join(g_thread, NULL);       /* the last one, finished */

    pthread_mutex_lock(&g_lock);
    unsigned id = g_job.id + 1;
    memset(&g_job, 0, sizeof g_job);
    g_job.id = id;
    g_job.kind = a->kind;
    g_job.running = 1;
    g_job.dry_run = a->dry_run;
    g_stop = 0;
    if (a->kind == DISC_JOB_SCAN) {
        g_scan = id;
        ++g_gen;                     /* every device "not heard by it" until it answers */
    }
    if (pthread_create(&g_thread, NULL, job_main, a) != 0) {
        g_job.running = 0;
        snprintf(g_job.msg, sizeof g_job.msg, "cannot start a thread");
        pthread_mutex_unlock(&g_lock);
        wipe(a, sizeof *a);
        free(a);
        errno = EAGAIN;
        return -1;
    }
    g_thread_up = 1;
    pthread_mutex_unlock(&g_lock);
    return (int)id;
}

int disc_scan_stop(void)
{
    pthread_mutex_lock(&g_lock);
    int id = g_job.running && g_job.kind == DISC_JOB_SCAN ? (int)g_job.id : -1;
    if (id > 0) g_stop = 1;
    pthread_mutex_unlock(&g_lock);
    return id;
}

void disc_shutdown(void)
{
    g_stop = 1;
    pthread_mutex_lock(&g_lock);
    int up = g_thread_up;
    g_thread_up = 0;
    pthread_mutex_unlock(&g_lock);
    if (up) pthread_join(g_thread, NULL);
}

/* ---- interfaces and sockets ------------------------------------------------------- */

typedef struct {
    char     name[IF_NAMESIZE];
    unsigned index;
    uint32_t addr, mask, bcast;      /* host order */
} iface_t;

static int in_list(const char *list, const char *name)
{
    size_t nl = strlen(name);
    for (const char *p = list; *p; ) {
        while (*p == ',' || *p == ' ') ++p;
        size_t n = strcspn(p, ", ");
        if (n == nl && !strncmp(p, name, n)) return 1;
        p += n;
    }
    return 0;
}

static int ifaces(const char *want, iface_t *out, int max)
{
    struct ifaddrs *ifa, *i;
    if (getifaddrs(&ifa) < 0) return 0;
    int n = 0;
    for (i = ifa; i && n < max; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) ||
            (i->ifa_flags & IFF_LOOPBACK) || !i->ifa_netmask)
            continue;
        if (want && *want && !in_list(want, i->ifa_name)) continue;
        iface_t *f = &out[n];
        snprintf(f->name, sizeof f->name, "%s", i->ifa_name);
        f->index = if_nametoindex(i->ifa_name);
        f->addr = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
        f->mask = ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
        f->bcast = (i->ifa_flags & IFF_BROADCAST) ? (f->addr & f->mask) | ~f->mask : 0;
        ++n;
    }
    freeifaddrs(ifa);
    return n;
}

static int udp_socket(int port)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof one);
    qos_mark(fd, QOS_MANAGEMENT, 0);    /* the searches: short-lived, not tracked */
    if (port) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        if (!port) {
            close(fd);
            return -1;
        }
        /* The port is taken (another tool, a second vigild): replies sent
         * to the broadcast address are missed; those sent back to the
         * asker still arrive. */
        a.sin_port = 0;
        if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

/* Send out of interface `ifindex` (0: by the routing table). */
static void send_to(int fd, const void *buf, size_t len, uint32_t ip, int port, const iface_t *f)
{
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                              .sin_addr.s_addr = htonl(ip) };
    if ((ip >> 28) == 14 && f) {             /* multicast: out of this interface */
        struct in_addr ia = { .s_addr = htonl(f->addr) };
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ia, sizeof ia);
        unsigned char ttl = 1;
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    }
    char cbuf[CMSG_SPACE(sizeof(struct in_pktinfo))];
    struct iovec iov = { .iov_base = (void *)buf, .iov_len = len };
    struct msghdr m = { .msg_name = &to, .msg_namelen = sizeof to, .msg_iov = &iov, .msg_iovlen = 1 };
    if (f && ip == 0xffffffffu) {
        /* 255.255.255.255 leaves by the default route unless told which
         * interface: IP_PKTINFO does that without privileges. */
        memset(cbuf, 0, sizeof cbuf);
        m.msg_control = cbuf;
        m.msg_controllen = sizeof cbuf;
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = IPPROTO_IP;
        c->cmsg_type = IP_PKTINFO;
        c->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
        struct in_pktinfo pi = { .ipi_ifindex = (int)f->index, .ipi_spec_dst.s_addr = htonl(f->addr) };
        memcpy(CMSG_DATA(c), &pi, sizeof pi);
    }
    sendmsg(fd, &m, MSG_NOSIGNAL);
}

/* One datagram; its sender and the interface it came in on. */
static ssize_t recv_from(int fd, void *buf, size_t cap, uint32_t *from, char *ifname, size_t ifcap)
{
    struct sockaddr_in a;
    char cbuf[CMSG_SPACE(sizeof(struct in_pktinfo))];
    struct iovec iov = { .iov_base = buf, .iov_len = cap };
    struct msghdr m = { .msg_name = &a, .msg_namelen = sizeof a, .msg_iov = &iov, .msg_iovlen = 1,
                        .msg_control = cbuf, .msg_controllen = sizeof cbuf };
    ssize_t n = recvmsg(fd, &m, MSG_DONTWAIT);
    if (n < 0) return -1;
    *from = ntohl(a.sin_addr.s_addr);
    if (ifcap) ifname[0] = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) {
            struct in_pktinfo pi;
            memcpy(&pi, CMSG_DATA(c), sizeof pi);
            char nm[IF_NAMESIZE];
            if (if_indextoname((unsigned)pi.ipi_ifindex, nm)) snprintf(ifname, ifcap, "%s", nm);
        }
    return n;
}

/* ---- scanning --------------------------------------------------------------------- */

typedef struct {
    int fd[3];                       /* netip, onvif, dhip */
    unsigned protos;
    iface_t f[IFACE_MAX];
    int nf;
    uint32_t targets[32];
    int nt;
    int broadcast;
    int port[3];
    char msgid[64];
    unsigned scan;                   /* the scan job's id; 0: another job listens */
} scanner_t;

static void scanner_close(scanner_t *s)
{
    for (int i = 0; i < 3; ++i)
        if (s->fd[i] >= 0) close(s->fd[i]);
}

static int scanner_open(scanner_t *s, const disc_scan_opts_t *o)
{
    memset(s, 0, sizeof *s);
    s->protos = o->protos ? o->protos : DISC_ALL;
    s->broadcast = !o->no_broadcast;
    s->nf = ifaces(o->ifaces, s->f, IFACE_MAX);
    static const int std[3] = { DISC_NETIP_UDP, DISC_ONVIF_UDP, DISC_DHIP_UDP };
    for (int i = 0; i < 3; ++i) s->port[i] = o->port[i] > 0 && o->port[i] < 65536 ? o->port[i] : std[i];
    for (const char *p = o->targets; *p && s->nt < 32; ) {
        while (*p == ',' || *p == ' ') ++p;
        size_t n = strcspn(p, ", ");
        char h[16];
        uint32_t v;
        if (n && n < sizeof h) {
            memcpy(h, p, n);
            h[n] = 0;
            if (disc_ip_parse(h, &v) == 0) s->targets[s->nt++] = v;
        }
        p += n;
    }
    /* NETIP devices answer to the broadcast address on 34569, so that is
     * where the NETIP socket listens; asking only unicast targets, the
     * answers come back to the asker, and a port of its own keeps them
     * from another program listening on 34569. */
    s->fd[0] = s->protos & DISC_NETIP ? udp_socket(s->broadcast ? DISC_NETIP_UDP : 0) : -1;
    s->fd[1] = s->protos & DISC_ONVIF ? udp_socket(0) : -1;
    s->fd[2] = s->protos & DISC_DHIP ? udp_socket(0) : -1;
    unsigned char rb[8];
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(rb, 1, sizeof rb, f) != sizeof rb) {
        long long t = mono_ms();
        memcpy(rb, &t, sizeof rb);
    }
    if (f) fclose(f);
    snprintf(s->msgid, sizeof s->msgid, "uuid:%02x%02x%02x%02x-%02x%02x-4%01x%02x-8000-%012llx",
             rb[0], rb[1], rb[2], rb[3], rb[4], rb[5], rb[6] & 15, rb[7], (unsigned long long)mono_ms() & 0xffffffffffffULL);
    return (s->fd[0] >= 0 || s->fd[1] >= 0 || s->fd[2] >= 0) ? 0 : -1;
}

/* One round of searches, to everything (or, `only` != 0, to that address). */
static void scanner_send(scanner_t *s, uint32_t only)
{
    uint8_t xm[32], dh[128];
    char ov[1024];
    size_t xl = disc_netip_search(xm, sizeof xm), dl = disc_dhip_search(dh, sizeof dh);
    size_t ol = disc_onvif_probe(ov, sizeof ov, s->msgid);
    const void *pkt[3] = { xm, ov, dh };
    size_t len[3] = { xl, ol, dl };
    uint32_t group[3] = { 0, 0, 0 };
    struct in_addr g;
    if (inet_pton(AF_INET, DISC_ONVIF_GROUP, &g) == 1) group[1] = ntohl(g.s_addr);
    if (inet_pton(AF_INET, DISC_DHIP_GROUP, &g) == 1) group[2] = ntohl(g.s_addr);
    for (int p = 0; p < 3; ++p) {
        if (s->fd[p] < 0 || !len[p]) continue;
        if (only) {
            send_to(s->fd[p], pkt[p], len[p], only, s->port[p], NULL);
            continue;
        }
        for (int t = 0; t < s->nt; ++t) send_to(s->fd[p], pkt[p], len[p], s->targets[t], s->port[p], NULL);
        if (!s->broadcast) continue;
        for (int i = 0; i < s->nf; ++i) {
            const iface_t *f = &s->f[i];
            if (group[p]) send_to(s->fd[p], pkt[p], len[p], group[p], s->port[p], f);
            /* ONVIF is multicast only; NETIP and DHIP also by broadcast. */
            if (p == 1) continue;
            if (f->bcast) send_to(s->fd[p], pkt[p], len[p], f->bcast, s->port[p], f);
            send_to(s->fd[p], pkt[p], len[p], 0xffffffffu, s->port[p], f);
        }
    }
}

/* Read what has arrived for up to `ms`; each device heard goes into the
 * table (and to `heard`, if given: called with each parsed device). */
static void scanner_listen(scanner_t *s, int ms, void (*heard)(const disc_device_t *, void *), void *u)
{
    long long end = mono_ms() + ms;
    static uint8_t buf[16384];                /* the job thread's only; replies are < 8 KiB */
    for (;;) {
        long long left = end - mono_ms();
        if (left <= 0 || g_stop) return;
        struct pollfd pf[3];
        int map[3], n = 0;
        for (int i = 0; i < 3; ++i)
            if (s->fd[i] >= 0) {
                pf[n] = (struct pollfd){ .fd = s->fd[i], .events = POLLIN };
                map[n++] = i;
            }
        if (poll(pf, (nfds_t)n, (int)(left > 200 ? 200 : left)) <= 0) continue;
        for (int k = 0; k < n; ++k) {
            if (!(pf[k].revents & POLLIN)) continue;
            for (int burst = 0; burst < 32; ++burst) {
                uint32_t from;
                char ifn[IF_NAMESIZE];
                ssize_t r = recv_from(s->fd[map[k]], buf, sizeof buf - 1, &from, ifn, sizeof ifn);
                if (r <= 0) break;
                buf[r] = 0;
                disc_device_t d;
                int ok = map[k] == 0 ? disc_netip_parse(buf, (size_t)r, &d)
                       : map[k] == 1 ? disc_onvif_parse((const char *)buf, (size_t)r, &d)
                                     : disc_dhip_parse(buf, (size_t)r, &d);
                if (ok < 0) continue;
                /* A reply from a unicast target (or through NAT) gives the
                 * address that answered; devices on another subnet still
                 * report their own. */
                d.seen = mono();
                d.scan = s->scan;
                snprintf(d.iface, sizeof d.iface, "%s", ifn);
                (void)from;
                table_add(&d);
                if (heard) heard(&d, u);
            }
        }
    }
}

static void run_scan(const job_args_t *a)
{
    scanner_t s;
    int secs = a->scan.seconds > 0 ? a->scan.seconds : 3;
    if (secs > DISC_SECS_MAX) secs = DISC_SECS_MAX;
    pthread_mutex_lock(&g_lock);
    g_last = a->scan;
    pthread_mutex_unlock(&g_lock);
    if (scanner_open(&s, &a->scan) < 0) {
        job_msg("cannot open a UDP socket");
        return;
    }
    if (!s.nf && !s.nt) {
        scanner_close(&s);
        job_msg("no interface to search on");
        return;
    }
    pthread_mutex_lock(&g_lock);
    s.scan = g_job.id;
    pthread_mutex_unlock(&g_lock);
    job_msg("searching");
    long long t0 = mono_ms(), total = secs * 1000LL;
    for (int round = 0; round < ROUNDS && round < secs && !g_stop; ++round) {
        scanner_send(&s, 0);
        /* In quarters: the progress moves while the results come in. */
        for (int q = 0; q < 4 && !g_stop; ++q) {
            scanner_listen(&s, 250, NULL, NULL);
            job_progress((int)((mono_ms() - t0) * 100 / total));
        }
    }
    while (!g_stop && mono_ms() - t0 < total) {
        scanner_listen(&s, 250, NULL, NULL);
        job_progress((int)((mono_ms() - t0) * 100 / total));
    }
    scanner_close(&s);
    int stopped = g_stop;
    disc_device_t *list = malloc(DISC_MAX * sizeof *list);
    int n = list ? disc_devices(list, DISC_MAX) : 0;
    free(list);
    job_msg("%d device%s%s", n, n == 1 ? "" : "s", stopped ? ", stopped" : "");
}

/* ---- address changes ---------------------------------------------------------------- */

/* Does `ip` look taken: our own address, a device or camera we know, or
 * something that answers ARP (on-link) or ping? */
typedef struct {
    iface_t f[IFACE_MAX];
    int nf;
    vigil_config_t *cfg;
    disc_device_t *dev;
    int ndev;
    const char *skip_ip;             /* the device being moved: its own address is fine */
} used_ctx_t;

static int arp_complete(const char *ip)
{
    FILE *f = fopen("/proc/net/arp", "r");
    if (!f) return 0;
    char line[256];
    int hit = 0;
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return 0;
    }
    while (!hit && fgets(line, sizeof line, f)) {
        char a[32];
        unsigned type, flags;
        if (sscanf(line, "%31s 0x%x 0x%x", a, &type, &flags) == 3 && !strcmp(a, ip) && (flags & 2)) hit = 1;
    }
    fclose(f);
    return hit;
}

/* Ping (an ICMP datagram socket, where the kernel allows it) and an ARP
 * look, together within ~400 ms. */
static int probe_alive(const char *ip)
{
    struct sockaddr_in a = { .sin_family = AF_INET };
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) return 0;
    if (arp_complete(ip)) return 1;
    /* A datagram to the discard port makes the kernel resolve the address
     * if it is on-link; the ARP table then says whether anyone answered. */
    int u = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (u >= 0) {
        a.sin_port = htons(9);
        sendto(u, "", 0, MSG_DONTWAIT, (struct sockaddr *)&a, sizeof a);
    }
    int alive = 0;
    int ic = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_ICMP);
    if (ic >= 0) {
        unsigned char echo[16] = { 8, 0, 0, 0, 0, 0, 0, 1, 'v', 'i', 'g', 'i', 'l' };
        a.sin_port = 0;
        if (sendto(ic, echo, sizeof echo, MSG_DONTWAIT, (struct sockaddr *)&a, sizeof a) > 0) {
            struct pollfd pf = { .fd = ic, .events = POLLIN };
            if (poll(&pf, 1, 400) == 1) {
                unsigned char r[64];
                ssize_t n = recv(ic, r, sizeof r, MSG_DONTWAIT);
                alive = n >= 8 && r[0] == 0;
            }
        }
        close(ic);
    } else {
        usleep(400 * 1000);
    }
    if (u >= 0) close(u);
    return alive || arp_complete(ip);
}

static int ip_used(const char *ip, void *u)
{
    used_ctx_t *c = u;
    uint32_t v;
    if (disc_ip_parse(ip, &v) < 0) return 1;
    if (c->skip_ip && !strcmp(ip, c->skip_ip)) return 0;
    for (int i = 0; i < c->nf; ++i)
        if (c->f[i].addr == v) return 1;
    for (int i = 0; i < c->ndev; ++i)
        if (!strcmp(c->dev[i].ip, ip)) return 1;
    if (c->cfg && channel_of(c->cfg, ip)) return 1;
    /* The loopback network always answers; no camera lives there but a
     * test's. */
    if (v >> 24 == 127) return 0;
    return probe_alive(ip);
}

int disc_plan(const char *start, const char *mask, int n, int (*used)(const char *ip, void *u), void *u,
              char out[][16])
{
    uint32_t s, m;
    if (disc_ip_parse(start, &s) < 0 || disc_mask_prefix(mask) < 1 || disc_ip_parse(mask, &m) < 0) return 0;
    uint32_t net = s & m, bcast = net | ~m;
    int k = 0;
    for (uint32_t ip = s; k < n && ip > net && ip < bcast && !g_stop; ++ip) {
        char t[16];
        disc_ip_str(ip, t, sizeof t);
        if (used && used(t, u)) continue;
        snprintf(out[k++], 16, "%s", t);
    }
    return k;
}

/* NETIP's broadcast set: no TCP, works across subnets. */
static disc_rc_t netip_udp_set(const disc_device_t *d, const char *ip, const char *mask, const char *gw,
                               const char *user, const char *pass, char *err, size_t errcap)
{
    uint8_t pkt[2048];
    size_t len = disc_netip_set(pkt, sizeof pkt, d, ip, mask, gw, user, pass);
    if (!len) {
        snprintf(err, errcap, d->mac[0] ? "bad address" : "no MAC to address the device by");
        return d->mac[0] ? DISC_EINVAL : DISC_EUNSUPPORTED;
    }
    scanner_t s;
    disc_scan_opts_t o;
    pthread_mutex_lock(&g_lock);
    o = g_last;
    pthread_mutex_unlock(&g_lock);
    o.protos = DISC_NETIP;
    if (scanner_open(&s, &o) < 0 || s.fd[0] < 0) {
        wipe(pkt, sizeof pkt);
        scanner_close(&s);
        snprintf(err, errcap, "cannot open a UDP socket");
        return DISC_EUNREACHABLE;
    }
    /* Once to each place the device can hear it from: a second copy would
     * apply the change twice (XM devices restart their network for each).
     * 255.255.255.255 reaches it whatever its subnet; a device asked by
     * unicast in the scan (a routed subnet, a test) is told by unicast. */
    uint32_t dev;
    int unicast = !s.broadcast;
    for (int t = 0; t < s.nt; ++t) unicast |= disc_ip_parse(d->ip, &dev) == 0 && s.targets[t] == dev;
    if (unicast && disc_ip_parse(d->ip, &dev) == 0) send_to(s.fd[0], pkt, len, dev, s.port[0], NULL);
    for (int i = 0; i < s.nf && s.broadcast && !unicast; ++i)
        if (!d->iface[0] || !strcmp(d->iface, s.f[i].name)) send_to(s.fd[0], pkt, len, 0xffffffffu, s.port[0], &s.f[i]);
    wipe(pkt, sizeof pkt);
    long long end = mono_ms() + NETIP_SET_S * 1000;
    int ret = -1;
    uint8_t buf[2048];
    while (ret < 0 && mono_ms() < end && !g_stop) {
        struct pollfd pf = { .fd = s.fd[0], .events = POLLIN };
        if (poll(&pf, 1, 200) != 1) continue;
        uint32_t from;
        char ifn[IF_NAMESIZE];
        ssize_t r;
        while (ret < 0 && (r = recv_from(s.fd[0], buf, sizeof buf, &from, ifn, sizeof ifn)) > 0)
            ret = disc_netip_set_reply(buf, (size_t)r);
    }
    scanner_close(&s);
    if (ret < 0) {
        snprintf(err, errcap, "no answer to the broadcast set");
        return DISC_ETIMEOUT;
    }
    if (ret == 100) {
        err[0] = 0;
        return DISC_OK;
    }
    snprintf(err, errcap, "refused (Ret %d)", ret);
    return ret == 203 || ret == 205 || ret == 206 ? DISC_EAUTH : DISC_EPROTO;
}

static disc_rc_t set_ip_proto(const disc_device_t *d, unsigned proto, const char *ip, const char *mask,
                              const char *gw, const char *user, const char *pass, char *err, size_t errcap)
{
    if (proto == DISC_NETIP) {
        disc_rc_t rc = netip_udp_set(d, ip, mask, gw, user, pass, err, errcap);
        if (rc != DISC_ETIMEOUT && rc != DISC_EUNSUPPORTED) return rc;
        /* Not every firmware answers the broadcast set: log in instead. */
        return disc_netip_tcp_set_ip(d, ip, mask, gw, user, pass, err, errcap);
    }
    if (proto == DISC_DHIP) return disc_dahua_set_ip(d, ip, mask, gw, user, pass, err, errcap);
    if (proto == DISC_ONVIF) return disc_onvif_set_ip(d, ip, mask, gw, user, pass, err, errcap);
    snprintf(err, errcap, "unsupported");
    return DISC_EUNSUPPORTED;
}

typedef struct {
    const char *mac, *ip;
    int found;
} reprobe_t;

static void reprobe_heard(const disc_device_t *d, void *u)
{
    reprobe_t *r = u;
    if (!strcmp(d->ip, r->ip) && (!r->mac[0] || !d->mac[0] || !strcmp(d->mac, r->mac))) r->found = 1;
}

/* Look for the device at its new address: unicast searches there and
 * broadcast ones (NETIP answers by broadcast), for up to REPROBE_S. */
static int reprobe(const disc_device_t *d, const char *new_ip)
{
    scanner_t s;
    disc_scan_opts_t o;
    pthread_mutex_lock(&g_lock);
    o = g_last;
    pthread_mutex_unlock(&g_lock);
    o.protos = d->protos ? d->protos : DISC_ALL;
    uint32_t nip;
    if (disc_ip_parse(new_ip, &nip) < 0 || scanner_open(&s, &o) < 0) return 0;
    reprobe_t r = { .mac = d->mac, .ip = new_ip };
    for (int t = 0; t < REPROBE_S && !r.found && !g_stop; ++t) {
        scanner_send(&s, nip);
        if (t % 3 == 0) scanner_send(&s, 0);
        scanner_listen(&s, 1000, reprobe_heard, &r);
    }
    scanner_close(&s);
    return r.found;
}

/* The protocols to try for `d`, in preference order. */
static int proto_order(const disc_device_t *d, unsigned only, unsigned out[3])
{
    unsigned pref[3];
    pthread_mutex_lock(&g_lock);
    memcpy(pref, g_prefer, sizeof pref);
    pthread_mutex_unlock(&g_lock);
    int n = 0;
    for (int i = 0; i < 3; ++i)
        if ((d->protos & pref[i]) && (!only || only == pref[i])) out[n++] = pref[i];
    return n;
}

static void run_ip(job_args_t *a)
{
    int n = a->n;
    used_ctx_t *uc = calloc(1, sizeof *uc);
    disc_device_t *devs = calloc((size_t)n, sizeof *devs);
    if (!uc || !devs) {
        job_msg("out of memory");
        goto out;
    }
    uc->nf = ifaces("", uc->f, IFACE_MAX);
    uc->cfg = malloc(sizeof *uc->cfg);
    uc->dev = malloc(DISC_MAX * sizeof *uc->dev);
    if (uc->cfg) config_get(uc->cfg);
    if (uc->dev) uc->ndev = disc_devices(uc->dev, DISC_MAX);

    /* Every item: its device, its address (given, or planned). */
    job_msg(a->dry_run ? "planning" : "checking addresses");
    int need = 0;
    for (int i = 0; i < n; ++i) {
        disc_result_t r = { 0 };
        snprintf(r.key, sizeof r.key, "%s", a->items[i].key);
        if (!table_find(a->items[i].key, &devs[i])) {
            r.rc = DISC_EINVAL;
            r.done = 1;
            snprintf(r.msg, sizeof r.msg, "not found (scan again)");
            devs[i].ip[0] = 0;
        } else {
            snprintf(r.ip, sizeof r.ip, "%s", devs[i].ip);
            if (a->items[i].ip[0]) snprintf(r.new_ip, sizeof r.new_ip, "%s", a->items[i].ip);
            else ++need;
        }
        job_result(i, &r);
    }
    if (need && !a->start[0]) {
        job_msg("no start address");
        goto out;
    }
    if (need) {
        /* One plan for all: each next address after the previous one. */
        char (*got)[16] = calloc((size_t)need, 16);
        int k = got ? disc_plan(a->start, a->mask, need, ip_used, uc, got) : 0;
        int j = 0;
        for (int i = 0; i < n; ++i) {
            if (!devs[i].ip[0] || a->items[i].ip[0]) continue;
            pthread_mutex_lock(&g_lock);
            disc_result_t r = g_job.r[i];
            pthread_mutex_unlock(&g_lock);
            if (j < k) snprintf(r.new_ip, sizeof r.new_ip, "%s", got[j++]);
            else {
                r.rc = DISC_EINVAL;
                r.done = 1;
                snprintf(r.msg, sizeof r.msg, "no free address left in the subnet");
            }
            job_result(i, &r);
        }
        free(got);
    } else {
        /* Given addresses: say if they look taken. */
        for (int i = 0; i < n; ++i) {
            if (!devs[i].ip[0]) continue;
            pthread_mutex_lock(&g_lock);
            disc_result_t r = g_job.r[i];
            pthread_mutex_unlock(&g_lock);
            uint32_t v;
            if (disc_ip_parse(r.new_ip, &v) < 0) {
                r.rc = DISC_EINVAL;
                r.done = 1;
                snprintf(r.msg, sizeof r.msg, "not an IPv4 address");
            } else if (strcmp(r.new_ip, devs[i].ip) != 0) {
                uc->skip_ip = devs[i].ip;
                if (ip_used(r.new_ip, uc)) {
                    r.rc = DISC_EINVAL;
                    r.done = 1;
                    snprintf(r.msg, sizeof r.msg, "%s is in use", r.new_ip);
                }
            }
            job_result(i, &r);
        }
    }
    if (a->dry_run) {
        for (int i = 0; i < n; ++i) {
            unsigned order[3];
            int np = proto_order(&devs[i], 0, order);
            pthread_mutex_lock(&g_lock);
            disc_result_t r = g_job.r[i];
            pthread_mutex_unlock(&g_lock);
            if (r.done) continue;
            r.done = 1;
            r.proto = np ? order[0] : 0;
            snprintf(r.msg, sizeof r.msg, "%s -> %s", r.ip, r.new_ip);
            job_result(i, &r);
        }
        job_msg("plan for %d device%s", n, n == 1 ? "" : "s");
        goto out;
    }

    int ok = 0, failed = 0;
    for (int i = 0; i < n && !g_stop; ++i) {
        pthread_mutex_lock(&g_lock);
        disc_result_t r = g_job.r[i];
        pthread_mutex_unlock(&g_lock);
        if (r.done) {
            ++failed;
            continue;
        }
        job_msg("%s -> %s", r.ip, r.new_ip);
        job_progress(i * 100 / n);
        unsigned order[3];
        int np = proto_order(&devs[i], 0, order);
        r.rc = DISC_EUNSUPPORTED;
        snprintf(r.msg, sizeof r.msg, "no protocol to change it with");
        for (int p = 0; p < np; ++p) {
            r.proto = order[p];
            r.rc = set_ip_proto(&devs[i], order[p], r.new_ip, a->mask, a->gw, a->user, a->pass, r.msg, sizeof r.msg);
            /* Wrong credentials: do not try them on the next protocol
             * (devices lock an account after a few). */
            if (r.rc == DISC_OK || r.rc == DISC_EAUTH || r.rc == DISC_EINVAL) break;
        }
        if (r.rc == DISC_ETIMEOUT || r.rc == DISC_EUNREACHABLE) {
            /* No answer, or gone from the old address: the change may have
             * been made all the same (a device that moves before it
             * answers; an answer lost). Where it answers says. */
            job_msg("looking for %s", r.new_ip);
            if ((r.reprobed = reprobe(&devs[i], r.new_ip)) != 0) {
                r.rc = DISC_OK;
                snprintf(r.msg, sizeof r.msg, "changed (the request was not answered)");
            }
        } else if (r.rc == DISC_OK) {
            job_msg("looking for %s", r.new_ip);
            r.reprobed = reprobe(&devs[i], r.new_ip);
            if (!r.msg[0]) snprintf(r.msg, sizeof r.msg, r.reprobed ? "changed" : "changed; not seen at the new address yet");
            ++ok;
        } else {
            ++failed;
        }
        r.done = 1;
        job_result(i, &r);
    }
    job_msg("%d changed, %d failed", ok, failed);
out:
    if (uc) {
        free(uc->cfg);
        free(uc->dev);
    }
    free(uc);
    free(devs);
}

/* ---- adding to channels ------------------------------------------------------------------ */

char *disc_cameras_with(const char *cameras, int ch, const char *url, const char *sub_url, const char *name)
{
    if (ch < 1 || ch > CHANNEL_MAX) return NULL;
    cJSON *arr = cameras && *cameras ? cJSON_Parse(cameras) : cJSON_CreateArray();
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return NULL;
    }
    while (cJSON_GetArraySize(arr) < ch) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "url", "");
        cJSON_AddItemToArray(arr, e);
    }
    cJSON *cam = cJSON_CreateObject();
    cJSON_AddStringToObject(cam, "url", url);
    cJSON_AddStringToObject(cam, "name", name ? name : "");
    cJSON_AddBoolToObject(cam, "enabled", 1);
    cJSON_AddStringToObject(cam, "sub_url", sub_url ? sub_url : "");
    cJSON_ReplaceItemInArray(arr, ch - 1, cam);
    char *text = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return text;
}

/* The URLs for `d` over `proto` (0: by preference); the protocol used. */
static unsigned device_urls(const disc_device_t *d, unsigned proto, const char *user, const char *pass,
                            char *url, size_t ucap, char *sub, size_t scap, disc_rc_t *rc, char *err, size_t errcap,
                            unsigned *tried)
{
    unsigned order[3];
    *tried = 0;
    int np = proto_order(d, proto, order);
    *rc = DISC_EUNSUPPORTED;
    snprintf(err, errcap, proto ? "the device did not answer %s" : "no protocol to add it with",
             disc_proto_name(proto));
    for (int p = 0; p < np && !g_stop; ++p) {
        *tried = order[p];
        if (order[p] == DISC_ONVIF) {
            *rc = disc_onvif_stream_uris(d, user, pass, 0, url, ucap, sub, scap, err, errcap);
        } else if (disc_url(d, order[p], user, pass, url, ucap, sub, scap) == 0) {
            *rc = DISC_OK;
        } else {
            *rc = DISC_EINVAL;
            snprintf(err, errcap, "the user or password has characters a camera URL cannot carry");
        }
        if (*rc == DISC_OK) {
            err[0] = 0;
            return order[p];
        }
        if (*rc == DISC_EAUTH || *rc == DISC_EINVAL) break;
    }
    return 0;
}

/* Write the cameras section (`text`, a JSON array) as the configuration,
 * from generation `gen`. */
static disc_rc_t save_cameras(const char *text, unsigned gen, char *err, size_t errcap)
{
    schema_errors_t errs = { 0 };
    config_rc_t rc = config_put_json_at("cameras", text, strlen(text), gen, &errs);
    if (rc == CONFIG_OK) return DISC_OK;
    if (rc == CONFIG_ESTALE) snprintf(err, errcap, "the configuration changed meanwhile; try again");
    else snprintf(err, errcap, "not saved: %.30s%s%.50s", errs.n ? errs.e[0].path : "", errs.n ? ": " : "",
                  errs.n ? errs.e[0].msg : "error");
    return DISC_EPROTO;
}

/* The camera's name: given, or the device's, cut to fit. A device's
 * name is kept to printable ASCII: cut in the middle of a UTF-8 sequence
 * it would be text the YAML file cannot hold. */
static void device_name(const disc_device_t *d, const char *given, char *out, size_t cap)
{
    const char *n = given && *given ? given : d->name[0] ? d->name : d->model[0] ? d->model : d->ip;
    size_t k = 0;
    for (; n[k] && k + 1 < cap; ++k)
        out[k] = n == given || (n[k] >= 0x20 && n[k] < 0x7f) ? n[k] : '?';
    out[k] = 0;
}

/* The name a new camera's channel gets when none is given. A NETIP camera
 * names its channels itself (ChannelTitle, the name it draws on the
 * picture): that, rather than the device's host name from the search,
 * which is the same on every camera of a make ("XM"). One that does not
 * answer gets "": the channel shows the camera's title once it connects.
 * Other protocols: the device's name (device_name). */
static void channel_name(const disc_device_t *d, const char *given, const char *url, char *out, size_t cap)
{
    if ((given && *given) || strncmp(url, "netip://", 8) != 0) {
        if (d) device_name(d, given, out, cap);
        else snprintf(out, cap, "%s", given ? given : "");
        return;
    }
    job_msg("asking for the channel's name");
    if (source_title(url, out, cap, &g_stop) < 0) out[0] = 0;
}

static int first_empty(const vigil_config_t *c, int after)
{
    for (int ch = after + 1; ch <= g_max_ch; ++ch)
        if (ch > c->n_cameras || !c->cameras[ch - 1].url[0]) return ch;
    return 0;
}

static void run_assign(job_args_t *a)
{
    disc_result_t r = { 0 };
    snprintf(r.key, sizeof r.key, "%s", a->key);
    disc_device_t d;
    vigil_config_t *c = malloc(sizeof *c);
    char *cams = NULL, *text = NULL;
    char url[512], sub[512], red[512];
    r.done = 1;
    if (!c) {
        r.rc = DISC_EPROTO;
        snprintf(r.msg, sizeof r.msg, "out of memory");
        goto out;
    }
    if (!table_find(a->key, &d)) {
        r.rc = DISC_EINVAL;
        snprintf(r.msg, sizeof r.msg, "not found (scan again)");
        goto out;
    }
    snprintf(r.ip, sizeof r.ip, "%s", d.ip);
    unsigned gen = config_get(c);
    int on = channel_of(c, d.ip);
    int ch = a->channel ? a->channel : first_empty(c, 0);
    if (on && on != ch) {
        r.rc = DISC_EINVAL;
        snprintf(r.msg, sizeof r.msg, "already on channel %d", on);
        goto out;
    }
    if (ch < 1 || ch > g_max_ch) {
        r.rc = DISC_EINVAL;
        snprintf(r.msg, sizeof r.msg, a->channel ? "no channel %d" : "no empty channel", a->channel);
        goto out;
    }
    if (ch <= c->n_cameras && c->cameras[ch - 1].url[0] && !a->replace && on != ch) {
        r.rc = DISC_EINVAL;
        snprintf(r.msg, sizeof r.msg, "channel %d has a camera", ch);
        goto out;
    }
    r.channel = ch;
    job_msg("adding %s to channel %d", d.ip, ch);
    device_urls(&d, a->proto, a->user, a->pass, url, sizeof url, sub, sizeof sub, &r.rc, r.msg, sizeof r.msg, &r.proto);
    if (r.rc != DISC_OK) goto out;
    char name[32];
    channel_name(&d, a->name, url, name, sizeof name);
    cams = config_get_json("cameras", 0);
    text = cams ? disc_cameras_with(cams, ch, url, sub, name) : NULL;
    if (!text) {
        r.rc = DISC_EPROTO;
        snprintf(r.msg, sizeof r.msg, "out of memory");
        goto out;
    }
    r.rc = save_cameras(text, gen, r.msg, sizeof r.msg);
    schema_redact_url(url, red, sizeof red);
    snprintf(r.url, sizeof r.url, "%s", red);
    if (r.rc == DISC_OK) snprintf(r.msg, sizeof r.msg, "added to channel %d", ch);
out:
    job_result(0, &r);
    job_msg("%s", r.msg);
    if (cams) wipe(cams, strlen(cams));
    if (text) wipe(text, strlen(text));
    free(cams);
    free(text);
    wipe(url, sizeof url);
    wipe(sub, sizeof sub);
    if (c) wipe(c, sizeof *c);
    free(c);
}

static void run_autoadd(job_args_t *a)
{
    vigil_config_t *c = malloc(sizeof *c);
    disc_device_t *list = malloc(DISC_MAX * sizeof *list);
    char *cams = NULL;
    if (!c || !list) {
        job_msg("out of memory");
        goto out;
    }
    unsigned gen = config_get(c);
    int n = disc_devices(list, DISC_MAX), k = 0, added = 0, ch = 0;
    cams = config_get_json("cameras", 0);
    for (int i = 0; i < n && !g_stop && cams; ++i) {
        disc_device_t *d = &list[i];
        if (channel_of(c, d->ip)) continue;               /* on a channel already */
        disc_result_t r = { .done = 1 };
        device_key(d, r.key, sizeof r.key);
        snprintf(r.ip, sizeof r.ip, "%s", d->ip);
        ch = first_empty(c, ch);
        if (!ch) {
            r.rc = DISC_EINVAL;
            snprintf(r.msg, sizeof r.msg, "no empty channel left");
            job_result(k++, &r);
            continue;
        }
        job_msg("%s", d->ip);
        job_progress(i * 100 / n);
        char url[512], sub[512], name[32];
        device_urls(d, 0, a->user, a->pass, url, sizeof url, sub, sizeof sub, &r.rc, r.msg, sizeof r.msg, &r.proto);
        if (r.rc == DISC_OK) {
            channel_name(d, NULL, url, name, sizeof name);
            char *t = disc_cameras_with(cams, ch, url, sub, name);
            if (t) {
                wipe(cams, strlen(cams));
                free(cams);
                cams = t;
                r.channel = ch;
                char red[512];
                schema_redact_url(url, red, sizeof red);
                snprintf(r.url, sizeof r.url, "%s", red);
                snprintf(r.msg, sizeof r.msg, "channel %d", ch);
                /* the channel is taken now, for first_empty */
                if (ch > c->n_cameras) c->n_cameras = ch;
                snprintf(c->cameras[ch - 1].url, sizeof c->cameras[ch - 1].url, "%s", url);
                ++added;
            } else {
                r.rc = DISC_EPROTO;
                snprintf(r.msg, sizeof r.msg, "out of memory");
                --ch;
            }
        } else {
            --ch;                                         /* the channel is still free */
        }
        wipe(url, sizeof url);
        wipe(sub, sizeof sub);
        job_result(k++, &r);
    }
    if (added) {
        char err[96];
        if (save_cameras(cams, gen, err, sizeof err) != DISC_OK) {
            job_msg("%s", err);
            pthread_mutex_lock(&g_lock);
            for (int i = 0; i < g_job.n; ++i)
                if (g_job.r[i].rc == DISC_OK) {
                    g_job.r[i].rc = DISC_EPROTO;
                    snprintf(g_job.r[i].msg, sizeof g_job.r[i].msg, "not saved");
                    g_job.r[i].channel = 0;
                }
            pthread_mutex_unlock(&g_lock);
            goto out;
        }
    }
    job_msg("%d added, %d not", added, k - added);
out:
    if (cams) wipe(cams, strlen(cams));
    free(cams);
    if (c) wipe(c, sizeof *c);
    free(c);
    free(list);
}

/* The configuration's discovery settings under the request's: its
 * interfaces when the request names none, its targets as well, its
 * protocol order. */
static void with_config(job_args_t *a)
{
    vigil_config_t *c = malloc(sizeof *c);
    if (!c) return;
    config_get(c);
    if (c->discovery.prefer[0]) disc_set_prefer(c->discovery.prefer);
    disc_scan_opts_t *o = &a->scan;
    if (!o->ifaces[0]) snprintf(o->ifaces, sizeof o->ifaces, "%s", c->discovery.interfaces);
    if (c->discovery.targets[0]) {
        size_t n = strlen(o->targets);
        size_t m = strlen(c->discovery.targets);
        if (n + 1 + m < sizeof o->targets) {
            if (n) o->targets[n++] = ',';
            memcpy(o->targets + n, c->discovery.targets, m + 1);
        }
    }
    free(c);
}

/* ---- a device entered by hand ----------------------------------------------------------- */

static const char *const k_manual_names[] = { "netip", "onvif", "dahua", "rtsp" };

int disc_manual_port(disc_manual_kind_t kind)
{
    return kind == DISC_MANUAL_NETIP ? 34567 : kind == DISC_MANUAL_ONVIF ? 80 : 554;
}

int disc_manual_kind(const char *name)
{
    for (int i = 0; i < 4; ++i)
        if (name && !strcasecmp(name, k_manual_names[i])) return i;
    if (name && !strcasecmp(name, "dhip")) return DISC_MANUAL_DAHUA;
    return -1;
}

const char *disc_manual_name(disc_manual_kind_t kind)
{
    return (unsigned)kind < 4 ? k_manual_names[kind] : "";
}

/* A host name or an IPv4 address, nothing a URL would read as more. */
static int host_ok(const char *h)
{
    if (!*h || strlen(h) > 127 || *h == '-' || *h == '.') return 0;
    for (const char *p = h; *p; ++p)
        if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '.' || *p == '_')) return 0;
    return 1;
}

/* An RTSP URL from a path or a whole URL, with the credentials put in
 * when there are some and the URL has none. */
static int rtsp_from(const disc_manual_t *m, int port, const char *path, char *out, size_t cap)
{
    while (*path == ' ') ++path;
    if (!strncasecmp(path, "rtsp://", 7)) {
        const char *auth = path + 7;
        size_t n = strcspn(auth, "/?#");
        if (!m->user[0] || memchr(auth, '@', n)) return snprintf(out, cap, "%s", path) < (int)cap ? 0 : -1;
        return disc_url_with_creds(path, m->user, m->pass, out, cap);
    }
    if (strpbrk(path, " \"\t\r\n")) return -1;
    int n = m->user[0] ? snprintf(out, cap, "rtsp://%s:%s@%s:%d%s%s", m->user, m->pass, m->host, port,
                                  *path == '/' ? "" : "/", path)
                       : snprintf(out, cap, "rtsp://%s:%d%s%s", m->host, port, *path == '/' ? "" : "/", path);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

disc_rc_t disc_manual_urls(const disc_manual_t *m, char *url, size_t ucap, char *sub, size_t scap, char *err,
                           size_t errcap)
{
    if (ucap) url[0] = 0;
    if (scap) sub[0] = 0;
    err[0] = 0;
    const char *user = m->user[0] || m->kind == DISC_MANUAL_RTSP ? m->user : "admin";
    int port = m->port > 0 && m->port < 65536 ? m->port : disc_manual_port(m->kind);
    int ch = m->dev_channel > 0 ? m->dev_channel : 1;
    if (!host_ok(m->host)) {
        snprintf(err, errcap, "not an address or host name");
        return DISC_EINVAL;
    }
    if (m->port < 0 || m->port > 65535 || ch > 256) {
        snprintf(err, errcap, m->port < 0 || m->port > 65535 ? "port out of range" : "channel out of range");
        return DISC_EINVAL;
    }
    if ((user[0] && !disc_cred_ok(user, 1)) || !disc_cred_ok(m->pass, 0)) {
        snprintf(err, errcap, "the user or password has characters a camera URL cannot carry");
        return DISC_EINVAL;
    }
    int n = 0, k = 0;
    switch (m->kind) {
    case DISC_MANUAL_NETIP:
        n = snprintf(url, ucap, "netip://%s:%s@%s:%d/?ch=%d&stream=main", user, m->pass, m->host, port, ch - 1);
        break;
    case DISC_MANUAL_DAHUA:
        n = snprintf(url, ucap, "rtsp://%s:%s@%s:%d/cam/realmonitor?channel=%d&subtype=0", user, m->pass, m->host,
                     port, ch);
        k = snprintf(sub, scap, "rtsp://%s:%s@%s:%d/cam/realmonitor?channel=%d&subtype=1", user, m->pass, m->host,
                     port, ch);
        break;
    case DISC_MANUAL_RTSP:
        if (!m->main_path[0]) {
            snprintf(err, errcap, "the main stream's path or URL is missing");
            return DISC_EINVAL;
        }
        n = 1;                                  /* rtsp_from checked the length */
        if (rtsp_from(m, port, m->main_path, url, ucap) < 0 ||
            (m->sub_path[0] && rtsp_from(m, port, m->sub_path, sub, scap) < 0)) {
            snprintf(err, errcap, "the stream path or URL is not usable");
            if (ucap) url[0] = 0;
            if (scap) sub[0] = 0;
            return DISC_EINVAL;
        }
        break;
    case DISC_MANUAL_ONVIF: {
        disc_device_t d;
        memset(&d, 0, sizeof d);
        d.dhcp = -1;
        d.http_port = port;
        size_t hl = strlen(m->host);
        if (hl < sizeof d.ip) memcpy(d.ip, m->host, hl + 1);
        snprintf(d.xaddr, sizeof d.xaddr, "http://%s:%d/onvif/device_service", m->host, port);
        d.protos = DISC_ONVIF;
        return disc_onvif_stream_uris(&d, user, m->pass, ch - 1, url, ucap, sub, scap, err, errcap);
    }
    default:
        snprintf(err, errcap, "unknown protocol");
        return DISC_EINVAL;
    }
    if (n <= 0 || (size_t)n >= ucap || k < 0 || (size_t)k >= scap) {
        if (ucap) url[0] = 0;
        if (scap) sub[0] = 0;
        snprintf(err, errcap, "too long for a camera URL");
        return DISC_EINVAL;
    }
    /* What the configuration would refuse is refused here. */
    source_t *t = source_create(url, 0);
    if (!t) {
        snprintf(err, errcap, "not a usable camera URL");
        url[0] = 0;
        if (scap) sub[0] = 0;
        return DISC_EINVAL;
    }
    source_destroy(t);
    return DISC_OK;
}

/* The first keyframe, through the channel's own normalizer: it knows a
 * keyframe whatever the protocol marks, and keeps the SPS (in-band or
 * from the SDP) for the picture size. */
typedef struct {
    au_norm_t  norm;
    int        norm_up, got;
    source_t  *src;
} probe_t;

static void probe_key(void *user, const vg_au_t *au)
{
    probe_t *p = user;
    if (au->key) p->got = 1;
}

static void probe_au(void *user, const vg_au_t *au)
{
    probe_t *p = user;
    if (p->got || (au->codec != VG_CODEC_H264 && au->codec != VG_CODEC_H265)) return;
    if (!p->norm_up || p->norm.codec != au->codec) {
        if (p->norm_up) au_norm_free(&p->norm);
        if (au_norm_init(&p->norm, au->codec) < 0) {
            p->norm_up = 0;
            return;
        }
        p->norm_up = 1;
        for (int i = 0; i < p->src->ps_n; ++i)
            if (p->src->ps_len[i]) au_norm_add_param_set(&p->norm, p->src->ps[i], p->src->ps_len[i]);
    }
    au_norm_push(&p->norm, au, probe_key, p);
}

static disc_rc_t fail_rc(src_fail_t f)
{
    return f == SRC_FAIL_AUTH ? DISC_EAUTH : f == SRC_FAIL_UNREACHABLE ? DISC_EUNREACHABLE
         : f == SRC_FAIL_CODEC ? DISC_ECODEC : DISC_EPROTO;
}

disc_rc_t disc_test_url(const char *url, int secs, char codec[8], int *width, int *height, char *err,
                        size_t errcap)
{
    codec[0] = 0;
    *width = *height = 0;
    source_t *s = source_create(url, 0);
    if (!s) {
        snprintf(err, errcap, "not a usable camera URL");
        return DISC_EINVAL;
    }
    if (s->ops->self_displayed) {
        source_destroy(s);
        snprintf(err, errcap, "not a network camera");
        return DISC_EINVAL;
    }
    s->stop = &g_stop;              /* disc_shutdown ends a test at once */
    disc_rc_t rc;
    if (s->ops->open(s) < 0) {
        rc = fail_rc(s->fail);
        snprintf(err, errcap, "%s", s->err[0] ? s->err : "cannot connect");
        source_destroy(s);
        return rc;
    }
    probe_t p = { .src = s };
    long long end = mono_ms() + secs * 1000LL;
    rc = DISC_ETIMEOUT;
    snprintf(err, errcap, "connected, but no video within %d s", secs);
    while (!p.got && !g_stop && mono_ms() < end) {
        if (s->ops->poll(s, probe_au, &p) < 0) {
            rc = fail_rc(s->fail);
            snprintf(err, errcap, "%s", s->err[0] ? s->err : "connection lost");
            break;
        }
    }
    if (p.got) {
        rc = DISC_OK;
        snprintf(codec, 8, "%s", vg_codec_name(p.norm.codec));
        if (p.norm.info_gen) {
            *width = (int)p.norm.info.width;
            *height = (int)p.norm.info.height;
        }
        err[0] = 0;
    }
    if (p.norm_up) au_norm_free(&p.norm); else if (rc == DISC_ETIMEOUT && s->codec != VG_CODEC_NONE && s->codec != VG_CODEC_H264 &&
               s->codec != VG_CODEC_H265) {
        rc = DISC_ECODEC;
        snprintf(err, errcap, "video, but neither H.264 nor H.265");
    }
    s->ops->close(s);
    source_destroy(s);
    return rc;
}

#define TEST_SECS 6

static void run_test(job_args_t *a)
{
    disc_result_t r = { .done = 1 };
    snprintf(r.key, sizeof r.key, "%.23s", a->manual.host);
    snprintf(r.ip, sizeof r.ip, "%.15s", a->manual.host);
    r.proto = a->manual.kind == DISC_MANUAL_NETIP ? DISC_NETIP : a->manual.kind == DISC_MANUAL_ONVIF ? DISC_ONVIF
            : a->manual.kind == DISC_MANUAL_DAHUA ? DISC_DHIP : 0;
    char url[512], sub[512];
    job_msg("%s", a->manual.kind == DISC_MANUAL_ONVIF ? "asking for the streams" : "connecting");
    r.rc = disc_manual_urls(&a->manual, url, sizeof url, sub, sizeof sub, r.msg, sizeof r.msg);
    if (r.rc == DISC_OK) {
        schema_redact_url(url, r.url, sizeof r.url);
        if (sub[0]) schema_redact_url(sub, r.sub_url, sizeof r.sub_url);
        job_msg("connecting");
        r.rc = disc_test_url(url, TEST_SECS, r.codec, &r.width, &r.height, r.msg, sizeof r.msg);
        if (r.rc == DISC_OK) {
            if (r.width) snprintf(r.msg, sizeof r.msg, "%s %dx%d", r.codec, r.width, r.height);
            else snprintf(r.msg, sizeof r.msg, "%s", r.codec);
        }
    }
    job_result(0, &r);
    job_msg("%s", r.rc == DISC_OK ? "OK" : r.msg);
    wipe(url, sizeof url);
    wipe(sub, sizeof sub);
}

static void run_assign_manual(job_args_t *a)
{
    disc_result_t r = { .done = 1 };
    snprintf(r.key, sizeof r.key, "%.23s", a->manual.host);
    snprintf(r.ip, sizeof r.ip, "%.15s", a->manual.host);
    vigil_config_t *c = malloc(sizeof *c);
    char url[512], sub[512], *cams = NULL, *text = NULL;
    url[0] = sub[0] = 0;
    int ch = a->channel;
    if (!c) {
        r.rc = DISC_EPROTO;
        snprintf(r.msg, sizeof r.msg, "out of memory");
        goto out;
    }
    unsigned gen = config_get(c);
    if (!ch) ch = first_empty(c, 0);
    if (ch < 1 || ch > g_max_ch) {
        r.rc = DISC_EINVAL;
        snprintf(r.msg, sizeof r.msg, a->channel ? "no channel %d" : "no empty channel", a->channel);
        goto out;
    }
    if (ch <= c->n_cameras && c->cameras[ch - 1].url[0] && !a->replace) {
        r.rc = DISC_EINVAL;
        snprintf(r.msg, sizeof r.msg, "channel %d has a camera", ch);
        goto out;
    }
    r.channel = ch;
    job_msg("adding %s to channel %d", a->manual.host, ch);
    r.rc = disc_manual_urls(&a->manual, url, sizeof url, sub, sizeof sub, r.msg, sizeof r.msg);
    if (r.rc != DISC_OK) goto out;
    char name[32];
    channel_name(NULL, a->name, url, name, sizeof name);
    cams = config_get_json("cameras", 0);
    text = cams ? disc_cameras_with(cams, ch, url, sub, name) : NULL;
    if (!text) {
        r.rc = DISC_EPROTO;
        snprintf(r.msg, sizeof r.msg, "out of memory");
        goto out;
    }
    r.rc = save_cameras(text, gen, r.msg, sizeof r.msg);
    schema_redact_url(url, r.url, sizeof r.url);
    if (sub[0]) schema_redact_url(sub, r.sub_url, sizeof r.sub_url);
    if (r.rc == DISC_OK) snprintf(r.msg, sizeof r.msg, "added to channel %d", ch);
out:
    job_result(0, &r);
    job_msg("%s", r.msg);
    if (cams) wipe(cams, strlen(cams));
    if (text) wipe(text, strlen(text));
    free(cams);
    free(text);
    wipe(url, sizeof url);
    wipe(sub, sizeof sub);
    if (c) wipe(c, sizeof *c);
    free(c);
}

static void *job_main(void *arg)
{
    job_args_t *a = arg;
    with_config(a);
    switch (a->kind) {
    case DISC_JOB_SCAN:    run_scan(a); break;
    case DISC_JOB_IP:      run_ip(a); break;
    case DISC_JOB_TEST:    run_test(a); break;
    case DISC_JOB_ASSIGN:  if (a->is_manual) run_assign_manual(a); else run_assign(a); break;
    case DISC_JOB_AUTOADD: run_autoadd(a); break;
    default: break;
    }
    wipe(a, sizeof *a);
    free(a);
    pthread_mutex_lock(&g_lock);
    g_job.running = 0;
    g_job.progress = 100;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

/* ---- starting jobs ------------------------------------------------------------------------- */

static job_args_t *args_new(disc_job_kind_t kind, const char *user, const char *pass)
{
    if (kind != DISC_JOB_SCAN && disc_auth_wait() > 0) {
        errno = EAGAIN;
        return NULL;
    }
    job_args_t *a = calloc(1, sizeof *a);
    if (!a) {
        errno = ENOMEM;
        return NULL;
    }
    a->kind = kind;
    if ((user && strlen(user) >= sizeof a->user) || (pass && strlen(pass) >= sizeof a->pass)) {
        free(a);
        errno = EINVAL;
        return NULL;
    }
    snprintf(a->user, sizeof a->user, "%s", user && *user ? user : "admin");
    snprintf(a->pass, sizeof a->pass, "%s", pass ? pass : "");
    return a;
}

int disc_scan(const disc_scan_opts_t *o)
{
    job_args_t *a = args_new(DISC_JOB_SCAN, NULL, NULL);
    if (!a) return -1;
    a->scan = *o;
    return job_start(a);
}

int disc_ip_job(const disc_ip_item_t *items, int n, const char *start, const char *mask, const char *gw,
                const char *user, const char *pass, int dry_run)
{
    uint32_t v;
    if (n < 1 || n > DISC_JOB_MAX || disc_mask_prefix(mask ? mask : "") < 1 ||
        (start && *start && disc_ip_parse(start, &v) < 0) || (gw && *gw && disc_ip_parse(gw, &v) < 0)) {
        errno = EINVAL;
        return -1;
    }
    job_args_t *a = args_new(DISC_JOB_IP, user, pass);
    if (!a) return -1;
    memcpy(a->items, items, (size_t)n * sizeof *items);
    a->n = n;
    a->dry_run = dry_run;
    snprintf(a->start, sizeof a->start, "%s", start ? start : "");
    snprintf(a->mask, sizeof a->mask, "%s", mask);
    snprintf(a->gw, sizeof a->gw, "%s", gw ? gw : "");
    return job_start(a);
}

int disc_assign_job(const char *key, int channel, unsigned proto, const char *user, const char *pass,
                    const char *name, int replace)
{
    if (!key || !*key || strlen(key) >= 24 || channel < 0 || channel > CHANNEL_MAX ||
        (proto & (proto - 1)) || (name && strlen(name) >= 32)) {
        errno = EINVAL;
        return -1;
    }
    job_args_t *a = args_new(DISC_JOB_ASSIGN, user, pass);
    if (!a) return -1;
    snprintf(a->key, sizeof a->key, "%s", key);
    snprintf(a->name, sizeof a->name, "%s", name ? name : "");
    a->channel = channel;
    a->proto = proto;
    a->replace = replace;
    return job_start(a);
}

static int manual_ok(const disc_manual_t *m)
{
    return (unsigned)m->kind < 4 && m->host[0] && memchr(m->host, 0, sizeof m->host) &&
           memchr(m->user, 0, sizeof m->user) && memchr(m->pass, 0, sizeof m->pass) &&
           memchr(m->main_path, 0, sizeof m->main_path) && memchr(m->sub_path, 0, sizeof m->sub_path);
}

int disc_test_job(const disc_manual_t *m)
{
    if (!manual_ok(m)) {
        errno = EINVAL;
        return -1;
    }
    job_args_t *a = args_new(DISC_JOB_TEST, NULL, NULL);
    if (!a) return -1;
    a->manual = *m;
    return job_start(a);
}

int disc_assign_manual_job(const disc_manual_t *m, int channel, const char *name, int replace)
{
    if (!manual_ok(m) || channel < 0 || channel > CHANNEL_MAX || (name && strlen(name) >= 32)) {
        errno = EINVAL;
        return -1;
    }
    job_args_t *a = args_new(DISC_JOB_ASSIGN, NULL, NULL);
    if (!a) return -1;
    a->manual = *m;
    a->is_manual = 1;
    a->channel = channel;
    a->replace = replace;
    snprintf(a->name, sizeof a->name, "%s", name ? name : "");
    return job_start(a);
}

int disc_autoadd_job(const char *user, const char *pass)
{
    job_args_t *a = args_new(DISC_JOB_AUTOADD, user, pass);
    if (!a) return -1;
    return job_start(a);
}
