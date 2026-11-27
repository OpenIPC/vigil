/* SPDX-License-Identifier: GPL-3.0-or-later */
/* users.c — vigil's own accounts (see users.h). */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "auth.h"
#include "cJSON.h"
#include "config.h"
#include "log.h"
#include "schema.h"
#include "schema_yaml.h"
#include "sha1.h"
#include "users.h"

#define FILE_MAX (64u << 10)
#define HASH_MAX 128
#define KEY_MAX  176                    /* base64 of a password of 128 bytes, and the NUL */

const char *const users_perm_names[PERM_N] = {
    "live", "playback", "ptz", "cameras", "storage", "network", "system", "users", "remote",
};

unsigned users_perm_bit(const char *name)
{
    for (int i = 0; i < PERM_N; ++i)
        if (!strcmp(users_perm_names[i], name)) return 1u << i;
    return 0;
}

/* ---- the file, as a schema ------------------------------------------------------- */

typedef struct {
    char name[USERS_NAME_MAX];
    int  perm[PERM_N];
} group_rec_t;

typedef struct {
    char name[USERS_NAME_MAX];
    char group[USERS_NAME_MAX];
    int  enabled;
    char password[HASH_MAX];
    char onvif[KEY_MAX];                /* the password, base64; "" none (users_login_with) */
} user_rec_t;

typedef struct {
    int         n_groups;
    group_rec_t groups[GROUPS_MAX];
    int         n_users;
    user_rec_t  users[USERS_MAX];
} users_db_t;

static int name_ok(const char *v)
{
    size_t n = strlen(v);
    return n >= 1 && n < USERS_NAME_MAX && strspn(v, "abcdefghijklmnopqrstuvwxyz0123456789_.-") == n &&
           v[0] != '.' && v[0] != '-';
}

static int check_name(const char *v, char *msg, size_t cap)
{
    if (!name_ok(v)) {
        snprintf(msg, cap, "1 to %d of a-z 0-9 _ . - (not starting with . or -)", USERS_NAME_MAX - 1);
        return -1;
    }
    return 0;
}

static int check_user_name(const char *v, char *msg, size_t cap)
{
    if (check_name(v, msg, cap) < 0) return -1;
    if (!strcmp(v, "root")) {
        snprintf(msg, cap, "\"root\" is the system account; choose another name");
        return -1;
    }
    return 0;
}

#define PERM_FIELD(I, K, L) { .key = K, .type = SCHEMA_BOOL, .off = offsetof(group_rec_t, perm[I]), \
                              .label = L }

static const schema_field_t group_fields[] = {
    { .key = "name", .type = SCHEMA_STRING, .off = offsetof(group_rec_t, name),
      .size = USERS_NAME_MAX, .required = 1, .check = check_name, .label = "Name" },
    PERM_FIELD(0, "live", "Live view, status and events"),
    PERM_FIELD(1, "playback", "Playback"),
    PERM_FIELD(2, "ptz", "Camera control (PTZ)"),
    PERM_FIELD(3, "cameras", "Cameras and discovery"),
    PERM_FIELD(4, "storage", "Recording, disks and network storage"),
    PERM_FIELD(5, "network", "Network, DDNS, mail and alarm center"),
    PERM_FIELD(6, "system", "Time, host name, UI and the whole configuration"),
    PERM_FIELD(7, "users", "Users, groups and the API's settings"),
    PERM_FIELD(8, "remote", "The zero channel: the screen, mouse and keyboard from a browser"),
};

static const schema_field_t user_fields[] = {
    { .key = "name", .type = SCHEMA_STRING, .off = offsetof(user_rec_t, name), .size = USERS_NAME_MAX,
      .required = 1, .check = check_user_name, .label = "Name" },
    { .key = "group", .type = SCHEMA_STRING, .off = offsetof(user_rec_t, group), .size = USERS_NAME_MAX,
      .def_str = "user", .check = check_name, .label = "Group" },
    { .key = "enabled", .type = SCHEMA_BOOL, .off = offsetof(user_rec_t, enabled), .def_int = 1,
      .label = "Enabled", .help = "A disabled user cannot log in." },
    { .key = "password", .type = SCHEMA_STRING, .off = offsetof(user_rec_t, password), .size = HASH_MAX,
      .def_str = "", .secret = SCHEMA_SECRET, .label = "Password",
      .help = "A crypt(3) hash, set through the API or the UI; empty: cannot log in." },
    { .key = "onvif", .type = SCHEMA_STRING, .off = offsetof(user_rec_t, onvif), .size = KEY_MAX,
      .def_str = "", .secret = SCHEMA_SECRET, .label = "ONVIF key",
      .help = "The password itself, base64-encoded: ONVIF clients prove they know it with a "
              "digest, which a crypt(3) hash cannot check. Written with the password while "
              "onvif_server is on; empty: this user cannot log in to ONVIF with a digest." },
};

#define N(a) (int)(sizeof(a) / sizeof((a)[0]))

static const schema_section_t user_sections[] = {
    { .key = "groups", .is_array = 1, .off = offsetof(users_db_t, groups),
      .count_off = offsetof(users_db_t, n_groups), .item_size = sizeof(group_rec_t),
      .max_items = GROUPS_MAX, .fields = group_fields, .n_fields = N(group_fields), .label = "Groups",
      .help = "What their users may do. \"admin\" (everything) and \"user\" are always there." },
    { .key = "users", .is_array = 1, .off = offsetof(users_db_t, users),
      .count_off = offsetof(users_db_t, n_users), .item_size = sizeof(user_rec_t),
      .max_items = USERS_MAX, .fields = user_fields, .n_fields = N(user_fields), .label = "Users",
      .help = "Besides these, the system account (api.user) logs in with every permission." },
};

static const schema_t users_schema = { .version = 1, .sections = user_sections,
                                       .n_sections = N(user_sections) };

static const char file_header[] =
    "vigil's users and groups: https://github.com/OpenIPC/vigil/blob/main/docs/api.md\n"
    "Written by vigild when accounts are changed through its HTTP API or on-screen\n"
    "UI; comments written by hand are not kept. Passwords are crypt(3) hashes; while\n"
    "the ONVIF server is on, each also has an \"onvif\" key: the password itself.";

/* ---- the store --------------------------------------------------------------------- */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static users_db_t      g_db;
static char            g_path[192];
static struct stat     g_st;            /* of the file as loaded; st_ino 0: none */
static int             g_loaded, g_bad;
static unsigned        g_gen = 1;

static unsigned rec_perms(const group_rec_t *g)
{
    unsigned p = 0;
    for (int i = 0; i < PERM_N; ++i)
        if (g->perm[i]) p |= 1u << i;
    return p;
}

static void rec_set_perms(group_rec_t *g, unsigned p)
{
    for (int i = 0; i < PERM_N; ++i) g->perm[i] = (p >> i) & 1;
}

static group_rec_t *find_group(users_db_t *db, const char *name)
{
    for (int i = 0; i < db->n_groups; ++i)
        if (!strcmp(db->groups[i].name, name)) return &db->groups[i];
    return NULL;
}

static user_rec_t *find_user(users_db_t *db, const char *name)
{
    for (int i = 0; i < db->n_users; ++i)
        if (!strcmp(db->users[i].name, name)) return &db->users[i];
    return NULL;
}

/* The built-ins first, whatever the file says: admin has everything. */
static void builtins(users_db_t *db)
{
    users_db_t out;
    memset(&out, 0, sizeof out);
    group_rec_t *adm = &out.groups[out.n_groups++];
    snprintf(adm->name, sizeof adm->name, "admin");
    rec_set_perms(adm, PERM_ALL);
    group_rec_t *usr = &out.groups[out.n_groups++];
    group_rec_t *had = find_group(db, "user");
    if (had) *usr = *had;
    else {
        snprintf(usr->name, sizeof usr->name, "user");
        rec_set_perms(usr, PERM_LIVE | PERM_PLAYBACK);
    }
    for (int i = 0; i < db->n_groups && out.n_groups < GROUPS_MAX; ++i)
        if (strcmp(db->groups[i].name, "admin") && strcmp(db->groups[i].name, "user") &&
            !find_group(&out, db->groups[i].name))
            out.groups[out.n_groups++] = db->groups[i];
    out.n_users = db->n_users;
    memcpy(out.users, db->users, sizeof out.users);
    *db = out;
}

static void api_settings(config_api_t *api)
{
    vigil_config_t *c = malloc(sizeof *c);
    if (!c) {
        memset(api, 0, sizeof *api);
        return;
    }
    config_get(c);
    *api = c->api;
    free(c);
}

static int same_stat(const struct stat *a, const struct stat *b)
{
    return a->st_ino == b->st_ino && a->st_dev == b->st_dev && a->st_mtime == b->st_mtime &&
           a->st_size == b->st_size;
}

/* Under g_lock: make g_db the file at `path`, if it is not already. */
static int load_locked(const char *path)
{
    struct stat st;
    memset(&st, 0, sizeof st);
    int there = stat(path, &st) == 0;
    if (g_loaded && !strcmp(path, g_path) && (there ? same_stat(&st, &g_st) : g_st.st_ino == 0))
        return g_bad ? -1 : 0;
    snprintf(g_path, sizeof g_path, "%s", path);
    g_st = st;
    if (!there) g_st.st_ino = 0;
    g_loaded = 1;
    g_bad = 0;
    ++g_gen;
    memset(&g_db, 0, sizeof g_db);
    if (there) {
        schema_errors_t errs = { 0 };
        FILE *f = fopen(path, "rb");
        char *buf = malloc(FILE_MAX + 1);
        size_t n = f && buf ? fread(buf, 1, FILE_MAX + 1, f) : 0;
        if (f) fclose(f);
        cJSON *doc = NULL;
        if (!f || !buf || n > FILE_MAX) {
            schema_error(&errs, "", "cannot read it");
        } else {
            buf[n] = 0;
            doc = schema_yaml_parse(&users_schema, buf, n, &errs);
        }
        free(buf);
        if (!doc || schema_from_json(&users_schema, doc, &g_db, 0, &errs) < 0 ||
            schema_validate(&users_schema, &g_db, &errs) < 0) {
            for (int i = 0; i < errs.n; ++i)
                vg_log("%s: %s%s%s", path, errs.e[i].path, errs.e[i].path[0] ? ": " : "", errs.e[i].msg);
            vg_log("%s is invalid: only the system account can log in", path);
            memset(&g_db, 0, sizeof g_db);
            g_bad = 1;
        }
        cJSON_Delete(doc);
    }
    builtins(&g_db);
    return g_bad ? -1 : 0;
}

static int load(void)
{
    config_api_t api;
    api_settings(&api);
    return load_locked(api.users);
}

/* Under g_lock: write `db` as the file and make it the one in force. */
static users_rc_t save_locked(users_db_t *db, users_err_t *e)
{
    if (g_bad) {
        snprintf(e->msg, sizeof e->msg, "%.100s is invalid; fix or remove it first (not saving over it)", g_path);
        return USERS_EIO;
    }
    builtins(db);
    cJSON *doc = schema_to_json(&users_schema, db, 0);
    char *text = doc ? schema_yaml_emit(&users_schema, doc, file_header) : NULL;
    cJSON_Delete(doc);
    if (!text) {
        snprintf(e->msg, sizeof e->msg, "out of memory");
        return USERS_EIO;
    }
    char err[300];
    int rc = config_write_atomic(g_path, text, strlen(text), err, sizeof err);
    free(text);
    if (rc < 0) {
        vg_log("users: %s", err);
        snprintf(e->msg, sizeof e->msg, "%.159s", err);
        return USERS_EIO;
    }
    g_db = *db;
    struct stat st;
    if (stat(g_path, &st) == 0) g_st = st;
    ++g_gen;
    return USERS_OK;
}

static users_rc_t fail(users_err_t *e, users_rc_t rc, const char *field, const char *fmt, const char *arg)
{
    snprintf(e->field, sizeof e->field, "%s", field);
    snprintf(e->msg, sizeof e->msg, fmt, arg);
    return rc;
}

int users_reload(void)
{
    pthread_mutex_lock(&g_lock);
    int rc = load();
    pthread_mutex_unlock(&g_lock);
    return rc;
}

unsigned users_generation(void)
{
    pthread_mutex_lock(&g_lock);
    load();
    unsigned g = g_gen;
    pthread_mutex_unlock(&g_lock);
    return g;
}

int users_list(users_user_t *u, int max)
{
    pthread_mutex_lock(&g_lock);
    load();
    int n = 0;
    for (int i = 0; i < g_db.n_users && n < max; ++i, ++n) {
        const user_rec_t *r = &g_db.users[i];
        memset(&u[n], 0, sizeof u[n]);
        snprintf(u[n].name, sizeof u[n].name, "%s", r->name);
        snprintf(u[n].group, sizeof u[n].group, "%s", r->group);
        u[n].enabled = r->enabled;
        u[n].password_set = r->password[0] != 0;
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

int users_groups(users_group_t *g, int max)
{
    pthread_mutex_lock(&g_lock);
    load();
    int n = 0;
    for (int i = 0; i < g_db.n_groups && n < max; ++i, ++n) {
        const group_rec_t *r = &g_db.groups[i];
        memset(&g[n], 0, sizeof g[n]);
        snprintf(g[n].name, sizeof g[n].name, "%s", r->name);
        g[n].perms = rec_perms(r);
        g[n].builtin = i < 2;
        g[n].editable = i != 0;
        for (int k = 0; k < g_db.n_users; ++k) g[n].members += !strcmp(g_db.users[k].group, r->name);
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

/* ---- passwords ----------------------------------------------------------------------- */

static const char SALT_CHARS[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

int users_hash(const char *pass, char *out, size_t cap)
{
    unsigned char rnd[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, rnd, sizeof rnd) : -1;
    if (fd >= 0) close(fd);
    if (n != (ssize_t)sizeof rnd) return -1;            /* no salt without randomness */
    char salt[sizeof rnd + 1];
    for (size_t i = 0; i < sizeof rnd; ++i) salt[i] = SALT_CHARS[rnd[i] % 64];
    salt[sizeof rnd] = 0;
    /* SHA-512 where the C library has it (glibc, musl); uClibc may have
     * only MD5. */
    static const char *const methods[] = { "$6$", "$5$", "$1$" };
    for (size_t m = 0; m < sizeof methods / sizeof methods[0]; ++m) {
        char setting[32];
        /* MD5's salt is at most 8 characters. */
        snprintf(setting, sizeof setting, "%s%.*s", methods[m], m == 2 ? 8 : 16, salt);
        if (auth_crypt(pass, setting, out, cap) == 0 && !strncmp(out, methods[m], 3)) return 0;
    }
    return -1;
}

static users_rc_t check_password(const char *pass, users_err_t *e)
{
    if (strlen(pass) < USERS_PASS_MIN)
        return fail(e, USERS_EINVAL, "password", "at least %s characters", "4");
    if (strlen(pass) > 128) return fail(e, USERS_EINVAL, "password", "at most %s characters", "128");
    for (const unsigned char *p = (const unsigned char *)pass; *p; ++p)
        if (*p < 0x20 || *p == 0x7f)
            return fail(e, USERS_EINVAL, "password", "%scontrol characters are not allowed", "");
    return USERS_OK;
}

/* The ONVIF key of `pass` into out (users.h, users_login_with): only
 * while the ONVIF server is on, "" otherwise. */
static void onvif_key(const char *pass, char *out, size_t cap)
{
    out[0] = 0;
    vigil_config_t *c = malloc(sizeof *c);
    if (!c) return;
    config_get(c);
    int on = c->onvif_server.enabled;
    free(c);
    if (on && base64_encode(pass, strlen(pass), out, cap) < 0) out[0] = 0;
}

static void wipe(void *p, size_t n)
{
    volatile unsigned char *w = p;
    while (n--) *w++ = 0;
}

/* ---- changes ------------------------------------------------------------------------------ */

users_rc_t users_add(const char *name, const char *group, const char *password, int enabled,
                     users_err_t *e)
{
    memset(e, 0, sizeof *e);
    char msg[128];
    if (check_user_name(name, msg, sizeof msg) < 0) return fail(e, USERS_EINVAL, "name", "%s", msg);
    if (!group || !*group) group = "user";
    if (!password) return fail(e, USERS_EINVAL, "password", "%sis required", "");
    users_rc_t rc = check_password(password, e);
    if (rc) return rc;
    char hash[HASH_MAX], key[KEY_MAX];
    if (users_hash(password, hash, sizeof hash) < 0)
        return fail(e, USERS_EIO, "password", "%scannot hash passwords here (crypt)", "");
    onvif_key(password, key, sizeof key);
    pthread_mutex_lock(&g_lock);
    load();
    users_db_t db = g_db;
    if (find_user(&db, name)) rc = fail(e, USERS_EEXIST, "name", "user '%s' exists", name);
    else if (!find_group(&db, group)) rc = fail(e, USERS_EINVAL, "group", "no group '%s'", group);
    else if (db.n_users >= USERS_MAX) rc = fail(e, USERS_EINVAL, "name", "at most %s users", "32");
    else {
        user_rec_t *u = &db.users[db.n_users++];
        memset(u, 0, sizeof *u);
        snprintf(u->name, sizeof u->name, "%s", name);
        snprintf(u->group, sizeof u->group, "%s", group);
        u->enabled = !!enabled;
        snprintf(u->password, sizeof u->password, "%s", hash);
        snprintf(u->onvif, sizeof u->onvif, "%s", key);
        rc = save_locked(&db, e);
        if (!rc) vg_log("users: added %s (group %s%s)", name, group, enabled ? "" : ", disabled");
    }
    pthread_mutex_unlock(&g_lock);
    wipe(key, sizeof key);
    wipe(&db, sizeof db);
    return rc;
}

users_rc_t users_update(const char *name, const char *group, int enabled, const char *password,
                        users_err_t *e)
{
    memset(e, 0, sizeof *e);
    char hash[HASH_MAX] = "", key[KEY_MAX] = "";
    if (password) {
        users_rc_t rc = check_password(password, e);
        if (rc) return rc;
        if (users_hash(password, hash, sizeof hash) < 0)
            return fail(e, USERS_EIO, "password", "%scannot hash passwords here (crypt)", "");
        onvif_key(password, key, sizeof key);
    }
    pthread_mutex_lock(&g_lock);
    load();
    users_db_t db = g_db;
    users_rc_t rc = USERS_OK;
    user_rec_t *u = find_user(&db, name);
    if (!u) rc = fail(e, USERS_ENOENT, "name", "no user '%s'", name);
    else if (group && !find_group(&db, group)) rc = fail(e, USERS_EINVAL, "group", "no group '%s'", group);
    else {
        if (group) snprintf(u->group, sizeof u->group, "%s", group);
        if (enabled >= 0) u->enabled = !!enabled;
        if (password) {
            snprintf(u->password, sizeof u->password, "%s", hash);
            snprintf(u->onvif, sizeof u->onvif, "%s", key);
        }
        rc = save_locked(&db, e);
        if (!rc)
            vg_log("users: changed %s:%s%s%s%s", name, group ? " group " : "", group ? group : "",
                   enabled < 0 ? "" : enabled ? " enabled" : " disabled", password ? " password" : "");
    }
    pthread_mutex_unlock(&g_lock);
    wipe(key, sizeof key);
    wipe(&db, sizeof db);
    return rc;
}

users_rc_t users_delete(const char *name, users_err_t *e)
{
    memset(e, 0, sizeof *e);
    pthread_mutex_lock(&g_lock);
    load();
    users_db_t db = g_db;
    users_rc_t rc;
    user_rec_t *u = find_user(&db, name);
    if (!u) rc = fail(e, USERS_ENOENT, "name", "no user '%s'", name);
    else {
        int i = (int)(u - db.users);
        memmove(&db.users[i], &db.users[i + 1], sizeof db.users[0] * (size_t)(db.n_users - i - 1));
        --db.n_users;
        rc = save_locked(&db, e);
        if (!rc) vg_log("users: deleted %s", name);
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

users_rc_t users_group_set(const char *name, unsigned perms, int create, users_err_t *e)
{
    memset(e, 0, sizeof *e);
    char msg[128];
    if (check_name(name, msg, sizeof msg) < 0) return fail(e, USERS_EINVAL, "name", "%s", msg);
    if (perms & ~(unsigned)PERM_ALL) return fail(e, USERS_EINVAL, "permissions", "%sunknown permission", "");
    if (!strcmp(name, "admin"))
        return fail(e, create ? USERS_EEXIST : USERS_EPROTECTED, "name",
                    "'%s' is built in and has every permission", name);
    pthread_mutex_lock(&g_lock);
    load();
    users_db_t db = g_db;
    users_rc_t rc = USERS_OK;
    group_rec_t *g = find_group(&db, name);
    if (create && g) rc = fail(e, USERS_EEXIST, "name", "group '%s' exists", name);
    else if (!create && !g) rc = fail(e, USERS_ENOENT, "name", "no group '%s'", name);
    else if (create && db.n_groups >= GROUPS_MAX) rc = fail(e, USERS_EINVAL, "name", "at most %s groups", "16");
    else {
        if (!g) {
            g = &db.groups[db.n_groups++];
            memset(g, 0, sizeof *g);
            snprintf(g->name, sizeof g->name, "%s", name);
        }
        rec_set_perms(g, perms);
        rc = save_locked(&db, e);
        if (!rc) {
            char list[128] = "";
            for (int i = 0; i < PERM_N; ++i)
                if (perms & (1u << i)) {
                    size_t l = strlen(list);
                    snprintf(list + l, sizeof list - l, "%s%s", l ? "," : "", users_perm_names[i]);
                }
            vg_log("users: group %s %s: %s", name, create ? "added" : "changed", list[0] ? list : "nothing");
        }
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

users_rc_t users_group_delete(const char *name, users_err_t *e)
{
    memset(e, 0, sizeof *e);
    if (!strcmp(name, "admin") || !strcmp(name, "user"))
        return fail(e, USERS_EPROTECTED, "name", "'%s' is built in", name);
    pthread_mutex_lock(&g_lock);
    load();
    users_db_t db = g_db;
    users_rc_t rc;
    group_rec_t *g = find_group(&db, name);
    int used = 0;
    for (int i = 0; i < db.n_users; ++i) used += !strcmp(db.users[i].group, name);
    if (!g) rc = fail(e, USERS_ENOENT, "name", "no group '%s'", name);
    else if (used) rc = fail(e, USERS_EINUSE, "name", "group '%s' still has users", name);
    else {
        int i = (int)(g - db.groups);
        memmove(&db.groups[i], &db.groups[i + 1], sizeof db.groups[0] * (size_t)(db.n_groups - i - 1));
        --db.n_groups;
        rc = save_locked(&db, e);
        if (!rc) vg_log("users: group %s deleted", name);
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

/* ---- logging in ------------------------------------------------------------------------------ */

int users_login(const char *name, const char *pass, unsigned *perms, char *why, size_t cap)
{
    *perms = 0;
    if (cap) why[0] = 0;
    config_api_t api;
    api_settings(&api);
    char hash[HASH_MAX] = "";
    unsigned gperms = 0;
    int found = 0, enabled = 0, has_admin = 0;
    pthread_mutex_lock(&g_lock);
    load_locked(api.users);
    user_rec_t *u = find_user(&g_db, name);
    if (u) {
        found = 1;
        enabled = u->enabled;
        snprintf(hash, sizeof hash, "%s", u->password);
        group_rec_t *g = find_group(&g_db, u->group);
        gperms = g ? rec_perms(g) : 0;          /* a group gone from the file: nothing */
    }
    has_admin = find_user(&g_db, "admin") != NULL;
    pthread_mutex_unlock(&g_lock);

    if (found) {
        /* crypt() runs anyway, so a disabled user takes as long as a wrong
         * password and does not tell itself apart. */
        char r[HASH_MAX];
        int ok = hash[0] && auth_crypt(pass, hash, r, sizeof r) == 0 && auth_equal(r, hash);
        if (!ok || !enabled) return 0;
        *perms = gperms;
        return 1;
    }
    /* The system account, and "admin" as its other name until a user of
     * that name exists: whatever the file holds, root gets in. */
    if (!strcmp(name, api.user) || (!strcmp(name, "admin") && !has_admin)) {
        int rc = auth_check_shadow(api.shadow, api.user, pass, why, cap);
        if (rc == 1) *perms = PERM_ALL;
        return rc;
    }
    /* Unknown user: the same crypt() as a known one, so the time does not
     * say which names exist. */
    char r[HASH_MAX];
    auth_crypt(pass, "$6$vigilnouser$", r, sizeof r);
    return 0;
}

int users_login_with(const char *name, int (*check)(const char *pass, void *ctx), void *ctx, unsigned *perms,
                     char *why, size_t cap)
{
    *perms = 0;
    if (cap) why[0] = 0;
    config_api_t api;
    api_settings(&api);
    char key[KEY_MAX] = "";
    unsigned gperms = 0;
    int found = 0, enabled = 0, has_admin = 0;
    pthread_mutex_lock(&g_lock);
    load_locked(api.users);
    user_rec_t *u = find_user(&g_db, name);
    if (u) {
        found = 1;
        enabled = u->enabled && u->password[0];
        snprintf(key, sizeof key, "%s", u->onvif);
        group_rec_t *g = find_group(&g_db, u->group);
        gperms = g ? rec_perms(g) : 0;
    }
    has_admin = find_user(&g_db, "admin") != NULL;
    pthread_mutex_unlock(&g_lock);

    if (found) {
        unsigned char pass[KEY_MAX];
        int n = key[0] ? auth_base64_decode(key, strlen(key), pass, sizeof pass - 1) : -1;
        wipe(key, sizeof key);
        if (n < 0) {
            if (enabled)
                snprintf(why, cap, "user '%s' has no ONVIF key: set its password again while onvif_server is on",
                         name);
            return enabled ? -1 : 0;
        }
        pass[n] = 0;
        int ok = check((const char *)pass, ctx);
        wipe(pass, sizeof pass);
        if (!ok || !enabled) return 0;
        *perms = gperms;
        return 1;
    }
    if (!strcmp(name, api.user) || (!strcmp(name, "admin") && !has_admin)) {
        snprintf(why, cap, "the system account (%s) has only a crypt(3) hash, which a digest login cannot be "
                           "checked against: log in to ONVIF as one of vigil's users", api.user);
        return -1;
    }
    return 0;
}
