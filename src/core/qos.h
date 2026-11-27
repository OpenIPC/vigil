/* SPDX-License-Identifier: GPL-3.0-or-later */
/* qos.h — DSCP marking of vigild's own packets (vigil.yaml `qos`).
 *
 * Switches and routers that honour DSCP queue a packet by the class in
 * its IP header; vigild marks the sockets it owns, one class per kind of
 * traffic:
 *
 *   QOS_VIDEO       the zero channel's picture (web.c's WebSocket), the
 *                   NETIP server's live streams (netip_srv.c)
 *   QOS_MANAGEMENT  the HTTP API, the web pages and logins, the ONVIF
 *                   service and WS-Discovery, the NETIP server's requests
 *                   and search answers, DDNS, camera searches
 *   QOS_CAMERA      vigild's connections to cameras: RTSP and NETIP
 *                   control, and the ACKs of the video they send
 *   QOS_ALARM       events: the API's event WebSocket, the alarm center's
 *                   POSTs and mails (curl --ip-tos)
 *
 * The DSCP goes into the upper six bits of IP_TOS (IPv4) or IPV6_TCLASS
 * (IPv6); the ECN bits stay 0. A socket is marked when it is made; one
 * tracked here (qos_mark ... track) is marked again when the values
 * change (qos_set). A listening socket passes its mark to the connections
 * it accepts (Linux copies the listener), so marking a server's listener
 * marks its new connections; the ones already open keep theirs.
 */
#ifndef QOS_H
#define QOS_H

typedef enum { QOS_VIDEO, QOS_MANAGEMENT, QOS_CAMERA, QOS_ALARM, QOS_CLASSES } qos_class_t;

#define QOS_DSCP_MAX 63

/* The TOS/traffic-class byte for `dscp` (DSCP << 2, ECN 0); -1 when it is
 * not 0..63. */
int qos_tos(int dscp);

/* "CS0".."CS7", "AF11".."AF43", "EF", "VA" for those values; NULL for the
 * others. */
const char *qos_dscp_name(int dscp);

/* The class's DSCP in force (0 until qos_set). */
int qos_dscp(qos_class_t c);

/* Set the values in force (dscp[QOS_CLASSES]) and mark every tracked
 * socket again; returns how many were marked. */
int qos_set(const int dscp[QOS_CLASSES]);

/* Set `fd`'s TOS (IPv4) or traffic class (IPv6) to `dscp`; 0, or -1 with
 * errno (not a socket, not IP). */
int qos_apply(int fd, int dscp);

/* Mark `fd` with class `c` now; `track`: and whenever the values change,
 * for as long as the socket is the same (its addresses are kept and
 * checked: a closed fd whose number is reused is not touched). Returns as
 * qos_apply. */
int qos_mark(int fd, qos_class_t c, int track);

/* Stop tracking `fd` (before closing it; optional, see qos_mark). */
void qos_forget(int fd);

/* Mark and track this process's listening TCP sockets on local port
 * `port` (a server whose fds are its own, as CivetWeb's are); returns how
 * many were found. */
int qos_mark_listeners(int port, qos_class_t c);

/* Mark and track this process's TCP connection from local port
 * `local_port` to `peer_addr` ("192.168.1.5", "::1"):`peer_port` (a
 * server's connection that changes class, as a WebSocket after its
 * upgrade). 0, or -1 if there is no such socket. */
int qos_mark_peer(int local_port, const char *peer_addr, int peer_port, qos_class_t c);

#endif /* QOS_H */
