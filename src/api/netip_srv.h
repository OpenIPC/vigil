/* SPDX-License-Identifier: GPL-3.0-or-later */
/* netip_srv.h — vigild as a Xiongmai NETIP (DVRIP) device, for XM's own
 * clients (CMS, VMS, XMEye on the LAN; docs/api.md, "NETIP server").
 *
 * On netip_server.port (TCP 34567): log in as one of vigil's users
 * (users_login_netip: XM's password hash), the device's information,
 * channel names, time and a few configurations read-only, and the
 * cameras' live streams (both of each) passed on as they come
 * (stream_tap.h), in XM's media framing (netip_proto.h): on the
 * connection that logged in (python-dvr, vigil's own NETIP source) or on
 * a second one that claims the session's monitor (CMS, XMEye), as XM
 * devices do. Live view needs the live permission; nothing can be
 * changed. On UDP 34569 it answers XM's LAN search.
 *
 * Its own threads: one listening (TCP and the search), one per
 * connection. A tap's callback only copies the access unit into its
 * connection's queue, never waits: a client that falls behind loses
 * pictures up to the next keyframe.
 *
 * A build with -DVIGIL_API=OFF has none (api_none.c): it logs in as
 * vigil's users, which come with the API.
 */
#ifndef NETIP_SRV_H
#define NETIP_SRV_H

#include "config.h"

/* Serve as `cfg` says (off: nothing). `platform` names the board in the
 * device's information ("OpenIPC <platform>"). 0, or -1 when it could
 * not listen (logged; vigild runs on without it). Main thread. */
int  netip_server_start(const config_netip_server_t *cfg, const char *platform);
void netip_server_stop(void);

/* A new configuration: start, stop or move to another port when it
 * changed (its connections close; a port that cannot be had leaves it on
 * the old one, said by netip_server_listening). Main thread. */
void netip_server_apply(const config_netip_server_t *cfg);

/* As api_listening (api.h): 1 while it listens, with `listen`
 * ("0.0.0.0:34567") and `why` a move that failed; 0 with `why` it does
 * not (off, the port in use). Any thread. */
int  netip_server_listening(char *listen, size_t cap, char *why, size_t wcap);

#endif /* NETIP_SRV_H */
