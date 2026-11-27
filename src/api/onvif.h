/* SPDX-License-Identifier: GPL-3.0-or-later */
/* onvif.h — vigild as an ONVIF device (onvif.c, docs/onvif.md): the
 * device, media (ver10) and media2 (ver20) services on their own small
 * web server at onvif_server.port, and a WS-Discovery target on UDP 3702.
 * A build with -DVIGIL_API=OFF has none (api_none.c). */
#ifndef ONVIF_H
#define ONVIF_H

/* A thread that follows onvif_server in the configuration: serves while
 * it is on, at its port, and stops (with a WS-Discovery Bye) when it is
 * turned off. 0, or -1 when the thread cannot start (logged). */
int  onvif_start(void);
void onvif_stop(void);
/* As api_listening (api.h): what the ONVIF web service listens on now,
 * or why it does not. */
int  onvif_listening(char *listen, size_t cap, char *why, size_t wcap);

#endif /* ONVIF_H */
