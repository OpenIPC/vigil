/* SPDX-License-Identifier: GPL-3.0-or-later */
/* net.h — TCP helpers shared by the network sources. */
#ifndef NET_H
#define NET_H

#include <stddef.h>

/* Connect to host:port within timeout_ms, giving up early once *stop is
 * set (stop may be NULL). The socket then blocks with a 1 s receive
 * timeout (so readers can poll a stop flag) and a 5 s send timeout.
 * Returns the fd, or -1 with `err` set. */
int net_connect(const char *host, int port, int timeout_ms, const volatile int *stop,
                char *err, size_t errcap);

#endif /* NET_H */
