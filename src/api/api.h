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

/* Start serving as the configuration says ("" = no API). Returns 0 when
 * serving or not configured, -1 when it could not start (logged: vigild
 * records and shows video without it). */
int  api_start(void);
void api_stop(void);

#endif /* API_H */
