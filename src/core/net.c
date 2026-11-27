/* SPDX-License-Identifier: GPL-3.0-or-later */
/* net.c — TCP helpers shared by the network sources (see net.h). */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "net.h"
#include "qos.h"

/* Slices of the connect wait: a camera that drops SYNs would otherwise
 * hold a channel's stop (a configuration change, a shutdown) for the whole
 * timeout. */
#define CONNECT_SLICE_MS 100

int net_connect(const char *host, int port, int timeout_ms, const volatile int *stop,
                char *err, size_t errcap)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai;
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &ai) != 0) {
        snprintf(err, errcap, "cannot resolve %s", host);
        return -1;
    }
    int fd = socket(ai->ai_family, SOCK_STREAM, 0);
    if (fd < 0) {
        freeaddrinfo(ai);
        snprintf(err, errcap, "socket: %s", strerror(errno));
        return -1;
    }
    /* Before connect: the SYN already carries the class. */
    qos_mark(fd, QOS_CAMERA, 0);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    freeaddrinfo(ai);
    int stopped = 0;
    if (rc < 0 && errno == EINPROGRESS) {
        struct pollfd pf = { .fd = fd, .events = POLLOUT };
        int e = 0, ready = 0;
        socklen_t el = sizeof e;
        for (int left = timeout_ms; left > 0 && !ready; left -= CONNECT_SLICE_MS) {
            if (stop && *stop) {
                stopped = 1;
                break;
            }
            ready = poll(&pf, 1, left < CONNECT_SLICE_MS ? left : CONNECT_SLICE_MS) == 1;
        }
        if (ready && getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el) == 0 && e == 0)
            rc = 0;
        else
            errno = e ? e : ETIMEDOUT;
    }
    if (rc < 0) {
        if (stopped) snprintf(err, errcap, "connect: stopped");
        else snprintf(err, errcap, "connect: %s", strerror(errno));
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, 0);
    /* Tracked once connected (by its addresses), so that a camera session
     * open when the class changes is marked again. */
    qos_mark(fd, QOS_CAMERA, 1);

    struct timeval tv = { .tv_sec = 1 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    tv.tv_sec = 5;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    int rcvbuf = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    return fd;
}

int net_listen_check(const char *listen, char *err, size_t errcap)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY) };
    const char *colon = strrchr(listen, ':');
    char addr[INET_ADDRSTRLEN] = "", *end;
    long port = strtol(colon ? colon + 1 : listen, &end, 10);
    if (*end || port < 1 || port > 65535) {
        snprintf(err, errcap, "'%s' has no port 1..65535", listen);
        return -1;
    }
    if (colon) {
        size_t n = (size_t)(colon - listen);
        snprintf(addr, sizeof addr, "%.*s", (int)(n < sizeof addr ? n : sizeof addr - 1), listen);
        if (inet_pton(AF_INET, addr, &a.sin_addr) != 1) {
            snprintf(err, errcap, "'%s' is not an IPv4 address", addr);
            return -1;
        }
    }
    a.sin_port = htons((uint16_t)port);
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        snprintf(err, errcap, "socket: %s", strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    int rc = bind(fd, (struct sockaddr *)&a, sizeof a);
    int e = errno;
    close(fd);
    if (rc == 0) return 0;
    if (e == EADDRINUSE) snprintf(err, errcap, "port %ld is in use (another program, or another of vigild's servers)", port);
    else if (e == EACCES) snprintf(err, errcap, "port %ld: below 1024 needs root", port);
    else if (e == EADDRNOTAVAIL) snprintf(err, errcap, "%s is not an address of this recorder", addr);
    else snprintf(err, errcap, "port %ld: %s", port, strerror(e));
    return -1;
}
