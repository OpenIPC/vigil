/* SPDX-License-Identifier: GPL-3.0-or-later */
/* api_int.h — what the API's request files share (api.c, api_disc.c, api_mgmt.c,
 * api_users.c). */
#ifndef API_INT_H
#define API_INT_H

#include <stddef.h>

#include "qos.h"
#include "schema.h"

struct mg_connection;
struct mg_context;
typedef struct cJSON cJSON;

/* Answers; each returns the status sent. send_json takes `doc`. */
int send_text(struct mg_connection *conn, int status, const char *text, const char *extra_hdr,
              const char *extra_val);
int send_json(struct mg_connection *conn, int status, cJSON *doc);
int send_error(struct mg_connection *conn, int status, const char *msg, const schema_errors_t *errs);
int method_not_allowed(struct mg_connection *conn, const char *allow);

/* The request body, NUL-terminated (free it); NULL with *status set. */
char *read_body(struct mg_connection *conn, size_t *len, int *status);
/* The same, up to `max` bytes (an uploaded configuration export). */
char *read_body_max(struct mg_connection *conn, size_t max, size_t *len, int *status);

/* /api/v1/network/..., /api/v1/system/..., /api/v1/disks..., /api/v1/storage/...,
 * /api/v1/jobs/... (api_mgmt.c); 0 when `uri` is not one of them. */
int api_mgmt_request(struct mg_connection *conn, const char *uri, const char *method);

/* /api/v1/system/config/..., /api/v1/system/reset (api_backup.c); 0 when
 * `uri` is not one of them. */
int api_backup_request(struct mg_connection *conn, const char *uri, const char *method);

/* The permissions (users.h PERM_*) a request needs, all of them; 0: any
 * logged-in user. One table for every endpoint (api_perm.c). */
unsigned api_perm_needed(const char *method, const char *uri);

/* A login from client address `addr`, checked as the API checks its
 * Basic credentials (vigil's users, the system account; a good one is
 * remembered a few minutes): 1 with *perms; 0 refused, answered a second
 * late; -2 while the address is locked out after failures in a row,
 * *retry_after seconds more. The failures counted are the API's and the
 * zero channel's together. api_auth_init first. */
int  api_login(const char *addr, const char *user, const char *pass, unsigned *perms, long *retry_after);
void api_auth_init(void);
/* The same for a login whose password is proved, not sent (ONVIF's
 * digests): users_login_with's `check`, counted and locked out with the
 * others. */
int  api_login_with(const char *addr, const char *user, int (*check)(const char *pass, void *ctx), void *ctx,
                    unsigned *perms, long *retry_after);

/* Who sent the request being handled on this thread, as the
 * authorization hook found: the user ("" when api.auth is off) and the
 * permissions. */
const char *api_request_user(void);
unsigned    api_request_perms(void);

/* /api/v1/me, /api/v1/users..., /api/v1/groups... (api_users.c). */
int api_users_request(struct mg_connection *conn, const char *uri, const char *method);

/* /api/v1/channels/N/snapshot(.jpg), /api/v1/snapshots... (api_snap.c); 0
 * when `uri` is not one of them. */
int api_snap_request(struct mg_connection *conn, const char *uri, const char *method);
/* /api/v1/channels/N/ptz (api_ptz.c); 0 when `uri` is not that. */
int api_ptz_request(struct mg_connection *conn, const char *uri, const char *method);

/* /api/v1/discovery..., /api/v1/channels/N/assign (api_disc.c). */
int api_disc_request(struct mg_connection *conn, const char *uri, const char *method);

/* QoS (qos.h): mark a server's listeners, whose new connections inherit
 * the class; mark one connection (a WebSocket after its upgrade).
 * CivetWeb does not hand out its sockets: they are found by address. */
void api_qos_listeners(const struct mg_context *ctx, qos_class_t c);
void api_qos_conn(const struct mg_connection *conn, qos_class_t c);

/* The servers' state for api_listening, web_listening, onvif_listening
 * and netip_server_listening (`srv` 0: the API, 1: the web server, 2:
 * ONVIF, 3: NETIP): what one listens on
 * ("": nothing) and why not; after a restart (api_restart, web_restart)
 * to `listen`, whether it failed, and whether the move was asked for (it
 * clears an old failure) or is a way back (it keeps it); `err` gets the
 * failure's reason. */
void api_server_state(int srv, const char *listen, const char *why);
void api_server_moved(int srv, const char *listen, int failed, int requested, char *err, size_t cap);

#endif /* API_INT_H */
