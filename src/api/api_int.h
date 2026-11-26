/* SPDX-License-Identifier: GPL-3.0-or-later */
/* api_int.h — what the API's request files share (api.c, api_mgmt.c). */
#ifndef API_INT_H
#define API_INT_H

#include <stddef.h>

#include "schema.h"

struct mg_connection;
typedef struct cJSON cJSON;

/* Answers; each returns the status sent. send_json takes `doc`. */
int send_text(struct mg_connection *conn, int status, const char *text, const char *extra_hdr,
              const char *extra_val);
int send_json(struct mg_connection *conn, int status, cJSON *doc);
int send_error(struct mg_connection *conn, int status, const char *msg, const schema_errors_t *errs);
int method_not_allowed(struct mg_connection *conn, const char *allow);

/* The request body, NUL-terminated (free it); NULL with *status set. */
char *read_body(struct mg_connection *conn, size_t *len, int *status);

/* /api/v1/network/..., /api/v1/system/..., /api/v1/disks..., /api/v1/storage/...,
 * /api/v1/jobs/... (api_mgmt.c); 0 when `uri` is not one of them. */
int api_mgmt_request(struct mg_connection *conn, const char *uri, const char *method);

#endif /* API_INT_H */
