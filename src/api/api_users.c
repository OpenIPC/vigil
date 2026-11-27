/* SPDX-License-Identifier: GPL-3.0-or-later */
/* api_users.c — /api/v1/me, /api/v1/users, /api/v1/groups (docs/api.md,
 * "Accounts and permissions"). The accounts themselves are users.c's; this
 * file is their JSON. Password hashes never appear in an answer. */
#define _DEFAULT_SOURCE
#include <stdlib.h>
#include <string.h>

#include "civetweb.h"
#include "cJSON.h"

#include "api_int.h"
#include "users.h"

static cJSON *perms_object(unsigned perms)
{
    cJSON *o = cJSON_CreateObject();
    for (int i = 0; o && i < PERM_N; ++i) cJSON_AddBoolToObject(o, users_perm_names[i], (perms >> i) & 1);
    return o;
}

static cJSON *perms_array(unsigned perms)
{
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; a && i < PERM_N; ++i)
        if ((perms >> i) & 1) cJSON_AddItemToArray(a, cJSON_CreateString(users_perm_names[i]));
    return a;
}

static cJSON *user_json(const users_user_t *u)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", u->name);
    cJSON_AddStringToObject(o, "group", u->group);
    cJSON_AddBoolToObject(o, "enabled", u->enabled);
    cJSON_AddBoolToObject(o, "password_set", u->password_set);
    return o;
}

static cJSON *group_json(const users_group_t *g)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", g->name);
    cJSON_AddBoolToObject(o, "builtin", g->builtin);
    cJSON_AddBoolToObject(o, "editable", g->editable);
    cJSON_AddNumberToObject(o, "members", g->members);
    cJSON_AddItemToObject(o, "permissions", perms_object(g->perms));
    return o;
}

static int find_user(const char *name, users_user_t *out)
{
    users_user_t all[USERS_MAX];
    int n = users_list(all, USERS_MAX);
    for (int i = 0; i < n; ++i)
        if (!strcmp(all[i].name, name)) {
            *out = all[i];
            return 0;
        }
    return -1;
}

static int find_group(const char *name, users_group_t *out)
{
    users_group_t all[GROUPS_MAX];
    int n = users_groups(all, GROUPS_MAX);
    for (int i = 0; i < n; ++i)
        if (!strcmp(all[i].name, name)) {
            *out = all[i];
            return 0;
        }
    return -1;
}

/* A failed change, as the API answers errors: validation errors with
 * their path, the rest as one message. */
static int send_users_error(struct mg_connection *conn, const char *base, users_rc_t rc, const users_err_t *e)
{
    schema_errors_t errs = { 0 };
    switch (rc) {
    case USERS_EINVAL: {
        char path[64];
        snprintf(path, sizeof path, "%s%s%s", base, e->field[0] ? "." : "", e->field);
        schema_error(&errs, path, "%s", e->msg);
        return send_error(conn, 400, "invalid account", &errs);
    }
    case USERS_ENOENT:    return send_error(conn, 404, e->msg, NULL);
    case USERS_EEXIST:
    case USERS_EINUSE:
    case USERS_EPROTECTED: return send_error(conn, 409, e->msg, NULL);
    default:              return send_error(conn, 500, e->msg[0] ? e->msg : "cannot save the accounts", NULL);
    }
}

/* The body as a JSON object, or an answer already sent (NULL). */
static cJSON *body_object(struct mg_connection *conn)
{
    size_t len = 0;
    int status = 0;
    char *body = read_body(conn, &len, &status);
    if (!body) {
        send_error(conn, status, status == 413 ? "body too large" : "out of memory", NULL);
        return NULL;
    }
    cJSON *doc = cJSON_ParseWithLength(body, len);
    /* It may hold a password. */
    memset(body, 0, len);
    free(body);
    if (!cJSON_IsObject(doc)) {
        cJSON_Delete(doc);
        send_error(conn, 400, "the body must be a JSON object", NULL);
        return NULL;
    }
    return doc;
}

static void wipe_strings(cJSON *doc)
{
    cJSON *p = cJSON_GetObjectItemCaseSensitive(doc, "password");
    if (cJSON_IsString(p)) memset(p->valuestring, 0, strlen(p->valuestring));
}

/* Unknown keys and wrong types, as errors at their paths; 0 when none. */
static int check_keys(const cJSON *doc, const char *base, const char *const *allowed,
                      const char *const *types, schema_errors_t *errs)
{
    int bad = 0;
    for (const cJSON *c = doc->child; c; c = c->next) {
        char path[64];
        snprintf(path, sizeof path, "%s.%s", base, c->string);
        int k = -1;
        for (int i = 0; allowed[i]; ++i)
            if (!strcmp(allowed[i], c->string)) k = i;
        if (k < 0) {
            schema_error(errs, path, "unknown setting");
            bad = 1;
        } else if ((types[k][0] == 's' && !cJSON_IsString(c)) || (types[k][0] == 'b' && !cJSON_IsBool(c)) ||
                   (types[k][0] == 'o' && !cJSON_IsObject(c))) {
            schema_error(errs, path, types[k][0] == 's' ? "must be a string"
                                     : types[k][0] == 'b' ? "must be true or false" : "must be an object");
            bad = 1;
        }
    }
    return bad;
}

static const char *str_of(const cJSON *doc, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(doc, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int bool_of(const cJSON *doc, const char *key, int dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(doc, key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : dflt;
}

/* {"live": true, ...} -> bits; `base` the current ones (PUT may name only
 * some). -1 with errs on an unknown name or a value not boolean. */
static int perms_from(const cJSON *o, unsigned base, unsigned *out, schema_errors_t *errs)
{
    int bad = 0;
    for (const cJSON *c = o->child; c; c = c->next) {
        char path[80];
        snprintf(path, sizeof path, "group.permissions.%s", c->string);
        unsigned bit = users_perm_bit(c->string);
        if (!bit) {
            schema_error(errs, path, "unknown permission");
            bad = 1;
        } else if (!cJSON_IsBool(c)) {
            schema_error(errs, path, "must be true or false");
            bad = 1;
        } else if (cJSON_IsTrue(c)) {
            base |= bit;
        } else {
            base &= ~bit;
        }
    }
    *out = base;
    return bad ? -1 : 0;
}

static int users_collection(struct mg_connection *conn, const char *m)
{
    if (!strcmp(m, "GET") || !strcmp(m, "HEAD")) {
        users_user_t all[USERS_MAX];
        int n = users_list(all, USERS_MAX);
        cJSON *a = cJSON_CreateArray();
        for (int i = 0; a && i < n; ++i) cJSON_AddItemToArray(a, user_json(&all[i]));
        return send_json(conn, 200, a);
    }
    if (strcmp(m, "POST")) return method_not_allowed(conn, "GET, HEAD, POST");
    cJSON *doc = body_object(conn);
    if (!doc) return 0;
    static const char *const keys[] = { "name", "group", "password", "enabled", NULL };
    static const char *const types[] = { "s", "s", "s", "b" };
    schema_errors_t errs = { 0 };
    if (check_keys(doc, "user", keys, types, &errs)) {
        wipe_strings(doc);
        cJSON_Delete(doc);
        return send_error(conn, 400, "invalid account", &errs);
    }
    const char *name = str_of(doc, "name");
    users_err_t e;
    users_rc_t rc = users_add(name ? name : "", str_of(doc, "group"), str_of(doc, "password"),
                              bool_of(doc, "enabled", 1), &e);
    char nm[USERS_NAME_MAX];
    snprintf(nm, sizeof nm, "%s", name ? name : "");
    wipe_strings(doc);
    cJSON_Delete(doc);
    if (rc) return send_users_error(conn, "user", rc, &e);
    users_user_t u;
    if (find_user(nm, &u) < 0) return send_error(conn, 500, "the user went away", NULL);
    return send_json(conn, 201, user_json(&u));
}

/* PUT /api/v1/users/<the system account> {"password": "..."}: root's
 * password, in the shadow file and sealed for RTSP, ONVIF and NETIP. Only
 * the system account itself: it is also the board's SSH and console
 * login, which no user of vigil's file gets by being in "admin". */
static int system_password(struct mg_connection *conn, const char *name)
{
    if (!users_is_system(api_request_user()))
        return send_error(conn, 403, "only the system account can change its own password", NULL);
    cJSON *doc = body_object(conn);
    if (!doc) return 0;
    static const char *const keys[] = { "password", "name", NULL };
    static const char *const types[] = { "s", "s" };
    schema_errors_t errs = { 0 };
    const char *nm = str_of(doc, "name");
    if (nm && strcmp(nm, name)) schema_error(&errs, "user.name", "a user cannot be renamed");
    const char *pw = str_of(doc, "password");
    if (!check_keys(doc, "user", keys, types, &errs) && !errs.n && !pw)
        schema_error(&errs, "user.password", "the system account's password is all that can be set");
    if (errs.n) {
        wipe_strings(doc);
        cJSON_Delete(doc);
        return send_error(conn, 400, "invalid account", &errs);
    }
    users_err_t e;
    users_rc_t rc = users_set_system_password(pw, &e);
    wipe_strings(doc);
    cJSON_Delete(doc);
    if (rc) return send_users_error(conn, "user", rc, &e);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "name", name);
    cJSON_AddBoolToObject(d, "system", 1);
    cJSON_AddBoolToObject(d, "password_set", 1);
    return send_json(conn, 200, d);
}

static int user_item(struct mg_connection *conn, const char *m, const char *name)
{
    users_user_t u;
    users_err_t e;
    if (!strcmp(m, "GET") || !strcmp(m, "HEAD")) {
        if (find_user(name, &u) < 0) return send_error(conn, 404, "no such user", NULL);
        return send_json(conn, 200, user_json(&u));
    }
    if (!strcmp(m, "DELETE")) {
        users_rc_t rc = users_delete(name, &e);
        if (rc) return send_users_error(conn, "user", rc, &e);
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "deleted", name);
        return send_json(conn, 200, d);
    }
    if (strcmp(m, "PUT")) return method_not_allowed(conn, "GET, HEAD, PUT, DELETE");
    if (find_user(name, &u) < 0 && users_is_system(name)) return system_password(conn, name);
    cJSON *doc = body_object(conn);
    if (!doc) return 0;
    static const char *const keys[] = { "group", "password", "enabled", "name", NULL };
    static const char *const types[] = { "s", "s", "b", "s" };
    schema_errors_t errs = { 0 };
    const char *nm = str_of(doc, "name");
    if (nm && strcmp(nm, name)) schema_error(&errs, "user.name", "a user cannot be renamed");
    if (check_keys(doc, "user", keys, types, &errs) || errs.n) {
        wipe_strings(doc);
        cJSON_Delete(doc);
        return send_error(conn, 400, "invalid account", &errs);
    }
    const cJSON *en = cJSON_GetObjectItemCaseSensitive(doc, "enabled");
    users_rc_t rc = users_update(name, str_of(doc, "group"), en ? cJSON_IsTrue(en) : -1,
                                 str_of(doc, "password"), &e);
    wipe_strings(doc);
    cJSON_Delete(doc);
    if (rc) return send_users_error(conn, "user", rc, &e);
    if (find_user(name, &u) < 0) return send_error(conn, 500, "the user went away", NULL);
    return send_json(conn, 200, user_json(&u));
}

static int groups_collection(struct mg_connection *conn, const char *m)
{
    if (!strcmp(m, "GET") || !strcmp(m, "HEAD")) {
        users_group_t all[GROUPS_MAX];
        int n = users_groups(all, GROUPS_MAX);
        cJSON *a = cJSON_CreateArray();
        for (int i = 0; a && i < n; ++i) cJSON_AddItemToArray(a, group_json(&all[i]));
        return send_json(conn, 200, a);
    }
    if (strcmp(m, "POST")) return method_not_allowed(conn, "GET, HEAD, POST");
    cJSON *doc = body_object(conn);
    if (!doc) return 0;
    static const char *const keys[] = { "name", "permissions", NULL };
    static const char *const types[] = { "s", "o" };
    schema_errors_t errs = { 0 };
    unsigned perms = 0;
    const cJSON *po = cJSON_GetObjectItemCaseSensitive(doc, "permissions");
    int bad = check_keys(doc, "group", keys, types, &errs);
    if (!bad && po && perms_from(po, 0, &perms, &errs) < 0) bad = 1;
    const char *name = str_of(doc, "name");
    char nm[USERS_NAME_MAX];
    snprintf(nm, sizeof nm, "%s", name ? name : "");
    cJSON_Delete(doc);
    if (bad) return send_error(conn, 400, "invalid group", &errs);
    users_err_t e;
    users_rc_t rc = users_group_set(nm, perms, 1, &e);
    if (rc) return send_users_error(conn, "group", rc, &e);
    users_group_t g;
    if (find_group(nm, &g) < 0) return send_error(conn, 500, "the group went away", NULL);
    return send_json(conn, 201, group_json(&g));
}

static int group_item(struct mg_connection *conn, const char *m, const char *name)
{
    users_group_t g;
    users_err_t e;
    if (!strcmp(m, "GET") || !strcmp(m, "HEAD")) {
        if (find_group(name, &g) < 0) return send_error(conn, 404, "no such group", NULL);
        return send_json(conn, 200, group_json(&g));
    }
    if (!strcmp(m, "DELETE")) {
        users_rc_t rc = users_group_delete(name, &e);
        if (rc) return send_users_error(conn, "group", rc, &e);
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "deleted", name);
        return send_json(conn, 200, d);
    }
    if (strcmp(m, "PUT")) return method_not_allowed(conn, "GET, HEAD, PUT, DELETE");
    if (find_group(name, &g) < 0) return send_error(conn, 404, "no such group", NULL);
    cJSON *doc = body_object(conn);
    if (!doc) return 0;
    static const char *const keys[] = { "permissions", "name", NULL };
    static const char *const types[] = { "o", "s" };
    schema_errors_t errs = { 0 };
    unsigned perms = g.perms;
    const char *nm = str_of(doc, "name");
    if (nm && strcmp(nm, name)) schema_error(&errs, "group.name", "a group cannot be renamed");
    const cJSON *po = cJSON_GetObjectItemCaseSensitive(doc, "permissions");
    int bad = check_keys(doc, "group", keys, types, &errs) || errs.n;
    if (!bad && po && perms_from(po, g.perms, &perms, &errs) < 0) bad = 1;
    cJSON_Delete(doc);
    if (bad) return send_error(conn, 400, "invalid group", &errs);
    users_rc_t rc = users_group_set(name, perms, 0, &e);
    if (rc) return send_users_error(conn, "group", rc, &e);
    if (find_group(name, &g) < 0) return send_error(conn, 500, "the group went away", NULL);
    return send_json(conn, 200, group_json(&g));
}

int api_users_request(struct mg_connection *conn, const char *uri, const char *m)
{
    if (!strcmp(uri, "/api/v1/me")) {
        if (strcmp(m, "GET") && strcmp(m, "HEAD")) return method_not_allowed(conn, "GET, HEAD");
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "user", api_request_user());
        cJSON_AddItemToObject(d, "permissions", perms_array(api_request_perms()));
        return send_json(conn, 200, d);
    }
    if (!strcmp(uri, "/api/v1/users")) return users_collection(conn, m);
    if (!strcmp(uri, "/api/v1/groups")) return groups_collection(conn, m);
    if (!strncmp(uri, "/api/v1/users/", 14) && uri[14] && !strchr(uri + 14, '/'))
        return user_item(conn, m, uri + 14);
    if (!strncmp(uri, "/api/v1/groups/", 15) && uri[15] && !strchr(uri + 15, '/'))
        return group_item(conn, m, uri + 15);
    return send_error(conn, 404, "not found", NULL);
}
