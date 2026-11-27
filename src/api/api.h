/* SPDX-License-Identifier: GPL-3.0-or-later */
/* api.h — vigild's HTTP/WebSocket API (docs/api.md).
 *
 * CivetWeb serves it on its own threads, at api.listen from the
 * configuration: JSON endpoints under /api/v1 for the status, the
 * configuration and its schema, and a WebSocket (/api/v1/ws) that pushes
 * status, events and configuration changes. The API never touches the
 * channels or the UI: it reads snapshots (channels_status, config_get,
 * events_next) and writes the configuration store, whose new generation
 * the main thread applies.
 *
 * A build with -DVIGIL_API=OFF links api_none.c instead.
 */
#ifndef API_H
#define API_H

#include <stddef.h>

/* Start serving as the configuration says ("" = no API). Returns 0 when
 * serving or not configured, -1 when it could not start (logged: vigild
 * records and shows video without it). */
int  api_start(void);
void api_stop(void);

/* Stop and listen on `listen` instead (api.listen changed while running):
 * 0, or -1 with `err` (nothing listens then; the caller starts the old
 * one again, with `err` NULL, which keeps the failure reported). Main
 * thread, as api_start. */
int  api_restart(const char *listen, char *err, size_t cap);
/* 1 while the API listens, with `listen` what on and `why` the last move
 * that failed ("" if none); 0 with `why` it does not listen (off, the
 * port in use, ...). Any thread. */
int  api_listening(char *listen, size_t cap, char *why, size_t wcap);

#endif /* API_H */
