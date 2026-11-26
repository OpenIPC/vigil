/* SPDX-License-Identifier: GPL-3.0-or-later */
/* net.c — TCP helpers shared by the network sources (see net.h). */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "net.h"

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
