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
#include "md5.h"
#include "schema.h"
#include "schema_yaml.h"
#include "secret.h"
#include "sha1.h"
#include "users.h"

#define FILE_MAX (64u << 10)
#define HASH_MAX 128
#define PASS_MAX 128                    /* check_password */
#define ENC_MAX  SECRET_SEALED_SIZE(PASS_MAX)
#define KEY_MAX  176                    /* base64 of a password of 128 bytes, and the NUL */
#define NETIP_HASH 16                   /* Xiongmai's hash is 8 characters */

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

/* A user. `password_enc` is the password itself, sealed (secret.h): every
 * client's own proof is made from it when it logs in (Digest's HA1 for
 * RTSP and ONVIF's HTTP Digest, Xiongmai's hash for NETIP, the
 * WS-UsernameToken digest), so a password set once works everywhere.
 * The crypt(3) hash checks the logins that send the password (the UI, the
 * web page, the API) without opening it.
 *
 * The last three are what vigild kept before (the password base64, or
 * only its hashes for RTSP and NETIP): read from an older file, used
 * until password_enc replaces them (at once for the base64, at the
 * user's next login with the password otherwise), then left out. */
typedef struct {
    char name[USERS_NAME_MAX];
    char group[USERS_NAME_MAX];
    int  enabled;
    char password[HASH_MAX];
    char password_enc[ENC_MAX];
    char digest[33];
    char onvif[KEY_MAX];
    char netip[NETIP_HASH];
} user_rec_t;

/* The system account (api.user, and "admin" while no user has that name),
 * whose password is root's in the shadow file: sealed here too when vigil
 * set it or saw it at a login, with which shadow entry it was (the MD5 of
 * the account's hash there), so a password changed elsewhere (passwd)
 * makes it unknown until the account logs in to vigil again. The last
 * three are an older vigild's hashes of it, as for users. */
typedef struct {
    char password_enc[ENC_MAX];
    char shadow[40];
    char netip[NETIP_HASH];
    char digest[33], admin_digest[33];
} system_rec_t;

typedef struct {
    int         n_groups;
    group_rec_t groups[GROUPS_MAX];
    int         n_users;
    user_rec_t  users[USERS_MAX];
    system_rec_t system;
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

#define OLD_HELP "From an older vigild; left out once the sealed password replaces it."

/* The current fields first: a file without old ones is written with only
 * those (USER_FIELDS_NOW, SYSTEM_FIELDS_NOW). */
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
    { .key = "password_enc", .type = SCHEMA_STRING, .off = offsetof(user_rec_t, password_enc), .size = ENC_MAX,
      .def_str = "", .secret = SCHEMA_SECRET, .label = "Sealed password",
      .help = "The same password, encrypted with the key in vigil.key (docs/security.md), for the "
              "clients that prove they know it without sending it: RTSP, ONVIF, NETIP." },
    { .key = "digest", .type = SCHEMA_STRING, .off = offsetof(user_rec_t, digest), .size = 33,
      .def_str = "", .secret = SCHEMA_SECRET, .label = "RTSP password hash", .help = OLD_HELP },
    { .key = "onvif", .type = SCHEMA_STRING, .off = offsetof(user_rec_t, onvif), .size = KEY_MAX,
      .def_str = "", .secret = SCHEMA_SECRET, .label = "ONVIF key", .help = OLD_HELP },
    { .key = "netip_password", .type = SCHEMA_STRING, .off = offsetof(user_rec_t, netip), .size = NETIP_HASH,
      .def_str = "", .secret = SCHEMA_SECRET, .label = "NETIP password", .help = OLD_HELP },
};
#define USER_FIELDS_NOW 5

static const schema_field_t system_fields[] = {
    { .key = "password_enc", .type = SCHEMA_STRING, .off = offsetof(system_rec_t, password_enc), .size = ENC_MAX,
      .def_str = "", .secret = SCHEMA_SECRET, .label = "Sealed password",
      .help = "The system account's password, encrypted as users' are: set with it through vigil, "
              "or seen when the account logs in to the API, the web page or the UI." },
    { .key = "shadow", .type = SCHEMA_STRING, .off = offsetof(system_rec_t, shadow), .size = 40,
      .def_str = "", .label = "Shadow entry",
      .help = "Which password that was (an MD5 of the account's shadow hash): one changed "
              "elsewhere (passwd) is not used." },
    { .key = "netip_password", .type = SCHEMA_STRING, .off = offsetof(system_rec_t, netip), .size = NETIP_HASH,
      .def_str = "", .secret = SCHEMA_SECRET, .label = "NETIP password", .help = OLD_HELP },
    { .key = "digest", .type = SCHEMA_STRING, .off = offsetof(system_rec_t, digest), .size = 33,
      .def_str = "", .secret = SCHEMA_SECRET, .label = "RTSP password hash", .help = OLD_HELP },
    { .key = "admin_digest", .type = SCHEMA_STRING, .off = offsetof(system_rec_t, admin_digest), .size = 33,
      .def_str = "", .secret = SCHEMA_SECRET, .label = "RTSP password hash as admin", .help = OLD_HELP },
};
#define SYSTEM_FIELDS_NOW 2

#define N(a) (int)(sizeof(a) / sizeof((a)[0]))

#define SECTIONS(NU, NS)                                                                                     \
    { .key = "groups", .is_array = 1, .off = offsetof(users_db_t, groups),                                   \
      .count_off = offsetof(users_db_t, n_groups), .item_size = sizeof(group_rec_t),                         \
      .max_items = GROUPS_MAX, .fields = group_fields, .n_fields = N(group_fields), .label = "Groups",        \
      .help = "What their users may do. \"admin\" (everything) and \"user\" are always there." },           \
    { .key = "users", .is_array = 1, .off = offsetof(users_db_t, users),                                     \
      .count_off = offsetof(users_db_t, n_users), .item_size = sizeof(user_rec_t),                           \
      .max_items = USERS_MAX, .fields = user_fields, .n_fields = NU, .label = "Users",                       \
      .help = "Besides these, the system account (api.user) logs in with every permission." },              \
    { .key = "system", .off = offsetof(users_db_t, system), .fields = system_fields,                         \
      .n_fields = NS, .label = "System account",                                                             \
      .help = "What RTSP, ONVIF and NETIP clients need to log in as the system account." }

/* Read with the old fields; written without them once none is left. */
static const schema_section_t sections_all[] = { SECTIONS(N(user_fields), N(system_fields)) };
static const schema_section_t sections_now[] = { SECTIONS(USER_FIELDS_NOW, SYSTEM_FIELDS_NOW) };
static const schema_t users_schema = { .version = 1, .sections = sections_all, .n_sections = N(sections_all) };
static const schema_t users_schema_now = { .version = 1, .sections = sections_now, .n_sections = N(sections_now) };

static const char file_header[] =
    "vigil's users and groups: https://github.com/OpenIPC/vigil/blob/main/docs/api.md\n"
    "Written by vigild when accounts are changed through its HTTP API or on-screen\n"
    "UI; comments written by hand are not kept. Each password is here twice: a\n"
    "crypt(3) hash, and sealed with the key in vigil.key (docs/security.md) for\n"
    "the RTSP, ONVIF and NETIP clients, which never send it.";

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
    out.system = db->system;
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

int users_same_file(const struct stat *a, const struct stat *b)
{
    return a->st_ino == b->st_ino && a->st_dev == b->st_dev && a->st_size == b->st_size &&
           a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
           a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

static void wipe(void *p, size_t n) { secret_wipe(p, n); }

/* ---- sealed passwords (secret.h) -------------------------------------------------------- */

/* The field each is sealed for: a user's cannot be moved to another user,
 * nor the system account's to another account name. */
static void user_aad(const char *name, char *out, size_t cap) { snprintf(out, cap, "users:%s.password", name); }
static void system_aad(const char *acct, char *out, size_t cap)
{
    snprintf(out, cap, "users:system:%s.password", acct);
}

/* The password sealed in `enc` for `aad` into `pass`: 1, or 0 when there
 * is none or it does not open (no key or another one: secret.c logs it
 * once; the callers say what it means for a login). */
static int open_pass(const char *aad, const char *enc, char *pass, size_t cap)
{
    if (!enc[0]) return 0;
    return secret_open(aad, enc, pass, cap) == SECRET_OK;
}

/* `enc` is `pass` sealed for `aad`. */
static int sealed_is(const char *aad, const char *enc, const char *pass)
{
    char have[PASS_MAX + 1];
    int same = open_pass(aad, enc, have, sizeof have) && auth_equal(have, pass);
    wipe(have, sizeof have);
    return same;
}

/* What an older vigild kept besides the hash (user_rec_t); 1 if any. */
static int drop_old_user(user_rec_t *u)
{
    int had = u->digest[0] || u->onvif[0] || u->netip[0];
    wipe(u->digest, sizeof u->digest);
    wipe(u->onvif, sizeof u->onvif);
    wipe(u->netip, sizeof u->netip);
    return had;
}

static int drop_old_system(system_rec_t *s)
{
    int had = s->netip[0] || s->digest[0] || s->admin_digest[0];
    wipe(s->netip, sizeof s->netip);
    wipe(s->digest, sizeof s->digest);
    wipe(s->admin_digest, sizeof s->admin_digest);
    return had;
}

/* A file from an older vigild: a user's base64 password ("onvif") is
 * sealed at once; old fields where the password is sealed already go.
 * The hashes alone cannot give the password: they stay, and work, until
 * the user's next login with it (keep_password). 1 if `db` changed. */
static int migrate(users_db_t *db)
{
    int change = 0, sealed = 0;
    for (int i = 0; i < db->n_users; ++i) {
        user_rec_t *u = &db->users[i];
        if (!u->password_enc[0] && u->onvif[0]) {
            unsigned char pass[KEY_MAX];
            char aad[64];
            int n = auth_base64_decode(u->onvif, strlen(u->onvif), pass, sizeof pass - 1);
            user_aad(u->name, aad, sizeof aad);
            if (n > 0 && !memchr(pass, 0, (size_t)n)) {
                pass[n] = 0;
                if (secret_seal(aad, (const char *)pass, u->password_enc, sizeof u->password_enc) == SECRET_OK)
                    ++sealed;
            }
            wipe(pass, sizeof pass);
        }
        if (u->password_enc[0] && drop_old_user(u)) change = 1;
    }
    if (db->system.password_enc[0] && drop_old_system(&db->system)) change = 1;
    if (sealed) vg_log("users: %d password(s) from an older file sealed", sealed);
    return change;
}

/* The old fields that are empty out of every object of `doc`: 1 if any
 * other is left (the file then needs them described). */
static int strip_old(cJSON *doc)
{
    int left = 0;
    cJSON *us = cJSON_GetObjectItemCaseSensitive(doc, "users");
    for (cJSON *u = cJSON_IsArray(us) ? us->child : NULL; u; u = u->next)
        for (int k = USER_FIELDS_NOW; k < N(user_fields); ++k) {
            cJSON *v = cJSON_GetObjectItemCaseSensitive(u, user_fields[k].key);
            if (cJSON_IsString(v) && v->valuestring[0]) left = 1;
            else cJSON_DeleteItemFromObjectCaseSensitive(u, user_fields[k].key);
        }
    cJSON *sys = cJSON_GetObjectItemCaseSensitive(doc, "system");
    for (int k = SYSTEM_FIELDS_NOW; cJSON_IsObject(sys) && k < N(system_fields); ++k) {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(sys, system_fields[k].key);
        if (cJSON_IsString(v) && v->valuestring[0]) left = 1;
        else cJSON_DeleteItemFromObjectCaseSensitive(sys, system_fields[k].key);
    }
    return left;
}

/* ---- the file ---------------------------------------------------------------------------- */

/* Under g_lock: write `db` as the file and make it the one in force. */
static users_rc_t save_locked(users_db_t *db, users_err_t *e)
{
    if (g_bad) {
        snprintf(e->msg, sizeof e->msg, "%.100s is invalid; fix or remove it first (not saving over it)", g_path);
        return USERS_EIO;
    }
    builtins(db);
    cJSON *doc = schema_to_json(&users_schema, db, 0);
    int old = doc ? strip_old(doc) : 0;
    char *text = doc ? schema_yaml_emit(old ? &users_schema : &users_schema_now, doc, file_header) : NULL;
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

/* Under g_lock: make g_db the file at `path`, if it is not already. */
static int load_locked(const char *path)
{
    struct stat st;
    memset(&st, 0, sizeof st);
    int there = stat(path, &st) == 0;
    if (g_loaded && !strcmp(path, g_path) && (there ? users_same_file(&st, &g_st) : g_st.st_ino == 0))
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
        if (buf) wipe(buf, n);
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
    users_db_t *db = !g_bad && there ? malloc(sizeof *db) : NULL;
    if (db) {
        *db = g_db;
        users_err_t e;
        if (migrate(db) && save_locked(db, &e) != USERS_OK) vg_log("users: %s: cannot rewrite it: %s", path, e.msg);
        wipe(db, sizeof *db);
        free(db);
    }
    return g_bad ? -1 : 0;
}

static int load(void)
{
    config_api_t api;
    api_settings(&api);
    return load_locked(api.users);
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

/* Digest's HA1 for `name` and `pass` in vigil's realm. */
static void ha1_of(const char *name, const char *pass, char out[33])
{
    char buf[USERS_NAME_MAX + sizeof USERS_REALM + 260];
    int n = snprintf(buf, sizeof buf, "%s:%s:%s", name, USERS_REALM, pass);
    md5_hex(buf, n < 0 ? 0 : (size_t)n >= sizeof buf ? sizeof buf - 1 : (size_t)n, out);
    wipe(buf, sizeof buf);
}

static users_rc_t check_password(const char *pass, users_err_t *e)
{
    if (strlen(pass) < USERS_PASS_MIN)
        return fail(e, USERS_EINVAL, "password", "at least %s characters", "4");
    if (strlen(pass) > PASS_MAX) return fail(e, USERS_EINVAL, "password", "at most %s characters", "128");
    for (const unsigned char *p = (const unsigned char *)pass; *p; ++p)
        if (*p < 0x20 || *p == 0x7f)
            return fail(e, USERS_EINVAL, "password", "%scontrol characters are not allowed", "");
    return USERS_OK;
}

/* A new password: its crypt(3) hash and its sealed copy. */
static users_rc_t new_password(const char *aad, const char *pass, char hash[HASH_MAX], char enc[ENC_MAX],
                               users_err_t *e)
{
    users_rc_t rc = check_password(pass, e);
    if (rc) return rc;
    if (users_hash(pass, hash, HASH_MAX) < 0)
        return fail(e, USERS_EIO, "password", "%scannot hash passwords here (crypt)", "");
    secret_rc_t s = secret_seal(aad, pass, enc, ENC_MAX);
    if (s != SECRET_OK) {
        char key[200];
        snprintf(e->field, sizeof e->field, "password");
        snprintf(e->msg, sizeof e->msg, "cannot seal the password: %s (%.100s)", secret_strerror(s),
                 secret_key_path(key, sizeof key));
        return USERS_EIO;
    }
    return USERS_OK;
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
    char hash[HASH_MAX], enc[ENC_MAX], aad[64];
    user_aad(name, aad, sizeof aad);
    users_rc_t rc = new_password(aad, password, hash, enc, e);
    if (rc) return rc;
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
        snprintf(u->password_enc, sizeof u->password_enc, "%s", enc);
        rc = save_locked(&db, e);
        if (!rc) vg_log("users: added %s (group %s%s)", name, group, enabled ? "" : ", disabled");
    }
    pthread_mutex_unlock(&g_lock);
    wipe(enc, sizeof enc);
    wipe(&db, sizeof db);
    return rc;
}

users_rc_t users_update(const char *name, const char *group, int enabled, const char *password,
                        users_err_t *e)
{
    memset(e, 0, sizeof *e);
    char hash[HASH_MAX] = "", enc[ENC_MAX] = "", aad[64];
    user_aad(name, aad, sizeof aad);
    if (password) {
        users_rc_t rc = new_password(aad, password, hash, enc, e);
        if (rc) return rc;
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
            snprintf(u->password_enc, sizeof u->password_enc, "%s", enc);
            drop_old_user(u);
        }
        rc = save_locked(&db, e);
        if (!rc)
            vg_log("users: changed %s:%s%s%s%s", name, group ? " group " : "", group ? group : "",
                   enabled < 0 ? "" : enabled ? " enabled" : " disabled", password ? " password" : "");
    }
    pthread_mutex_unlock(&g_lock);
    wipe(enc, sizeof enc);
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
    wipe(&db, sizeof db);
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
    wipe(&db, sizeof db);
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
    wipe(&db, sizeof db);
    return rc;
}

int users_is_system(const char *name)
{
    config_api_t api;
    api_settings(&api);
    pthread_mutex_lock(&g_lock);
    load_locked(api.users);
    int sys = !find_user(&g_db, name) &&
              (!strcmp(name, api.user) || (!strcmp(name, "admin") && !find_user(&g_db, "admin")));
    pthread_mutex_unlock(&g_lock);
    return sys;
}

users_rc_t users_set_system_password(const char *password, users_err_t *e)
{
    memset(e, 0, sizeof *e);
    config_api_t api;
    api_settings(&api);
    char hash[HASH_MAX], enc[ENC_MAX], aad[96], id[33], why[200];
    system_aad(api.user, aad, sizeof aad);
    users_rc_t rc = new_password(aad, password, hash, enc, e);
    if (rc) return rc;
    /* The shadow file first: it is the password; the sealed copy follows it. */
    if (auth_shadow_set(api.shadow, api.user, hash, why, sizeof why) < 0) {
        wipe(enc, sizeof enc);
        vg_log("users: the system account's password not changed: %s", why);
        snprintf(e->msg, sizeof e->msg, "%.159s", why);
        return USERS_EIO;
    }
    md5_hex(hash, strlen(hash), id);
    pthread_mutex_lock(&g_lock);
    load_locked(api.users);
    users_db_t *db = malloc(sizeof *db);
    if (!db) {
        rc = fail(e, USERS_EIO, "", "%sout of memory", "");
    } else {
        *db = g_db;
        snprintf(db->system.password_enc, sizeof db->system.password_enc, "%s", enc);
        snprintf(db->system.shadow, sizeof db->system.shadow, "%s", id);
        drop_old_system(&db->system);
        rc = save_locked(db, e);
        wipe(db, sizeof *db);
        free(db);
    }
    pthread_mutex_unlock(&g_lock);
    wipe(enc, sizeof enc);
    if (rc == USERS_OK) vg_log("users: the system account's (%s) password changed", api.user);
    else vg_log("users: the system account's password changed in %s, but not kept for RTSP, ONVIF and NETIP: %s",
                api.shadow, e->msg);
    return rc;
}

/* ---- logging in ------------------------------------------------------------------------------ */

/* After a login with the password (users_login): kept sealed when the
 * record lacks it, holds another (the key was lost), or for the system
 * account belongs to another shadow entry (`id`, auth_shadow_id; NULL for
 * a user of the file). The one-time step for a user from an older file
 * with only hashes, and how the system account's password changed with
 * passwd becomes known again. */
static void keep_password(const char *name, const char *pass, const char *acct, const char *id)
{
    char aad[96];
    if (id) system_aad(acct, aad, sizeof aad);
    else user_aad(name, aad, sizeof aad);
    users_db_t *db = malloc(sizeof *db);
    if (!db) return;
    pthread_mutex_lock(&g_lock);
    load();
    *db = g_db;
    char *enc = NULL;
    if (id) {
        if (strcmp(db->system.shadow, id) || !sealed_is(aad, db->system.password_enc, pass)) {
            enc = db->system.password_enc;
            snprintf(db->system.shadow, sizeof db->system.shadow, "%s", id);
            drop_old_system(&db->system);
        }
    } else {
        user_rec_t *u = find_user(db, name);
        if (u && !sealed_is(aad, u->password_enc, pass)) {
            enc = u->password_enc;
            drop_old_user(u);
        }
    }
    users_err_t e;
    if (enc && !g_bad && secret_seal(aad, pass, enc, ENC_MAX) == SECRET_OK && save_locked(db, &e) == USERS_OK)
        vg_log("users: %s's password kept for the RTSP, ONVIF and NETIP clients", id ? acct : name);
    pthread_mutex_unlock(&g_lock);
    wipe(db, sizeof *db);
    free(db);
}

/* Who logs in as `name`, as far as the clients that never send the
 * password need to know. */
typedef struct {
    int      kind;                      /* 0 nobody, 1 a user of the file, 2 the system account */
    int      may;                       /* a user: enabled, with a password */
    unsigned perms;
    char     pass[PASS_MAX + 1];        /* "" unknown */
    /* what an older vigild kept, while the password is not sealed */
    char     digest[33], netip[NETIP_HASH], onvif[KEY_MAX];
    char     why[200];                  /* why the password is not known */
} who_t;

static void who(const char *name, who_t *w)
{
    memset(w, 0, sizeof *w);
    config_api_t api;
    api_settings(&api);
    char aad[96], enc[ENC_MAX] = "", sys_id[40] = "";
    pthread_mutex_lock(&g_lock);
    load_locked(api.users);
    user_rec_t *u = find_user(&g_db, name);
    if (u) {
        w->kind = 1;
        w->may = u->enabled && u->password[0];
        group_rec_t *g = find_group(&g_db, u->group);
        w->perms = g ? rec_perms(g) : 0;            /* a group gone from the file: nothing */
        user_aad(name, aad, sizeof aad);
        snprintf(enc, sizeof enc, "%s", u->password_enc);
        snprintf(w->digest, sizeof w->digest, "%s", u->digest);
        snprintf(w->netip, sizeof w->netip, "%s", u->netip);
        snprintf(w->onvif, sizeof w->onvif, "%s", u->onvif);
    } else if (!strcmp(name, api.user) || (!strcmp(name, "admin") && !find_user(&g_db, "admin"))) {
        const system_rec_t *s = &g_db.system;
        w->kind = 2;
        w->may = 1;
        w->perms = PERM_ALL;
        system_aad(api.user, aad, sizeof aad);
        snprintf(enc, sizeof enc, "%s", s->password_enc);
        snprintf(w->digest, sizeof w->digest, "%s", !strcmp(name, api.user) ? s->digest : s->admin_digest);
        snprintf(w->netip, sizeof w->netip, "%s", s->netip);
        snprintf(sys_id, sizeof sys_id, "%s", s->shadow);
    }
    pthread_mutex_unlock(&g_lock);
    if (w->kind == 2) {
        /* Only while the shadow entry is the one it was kept for. */
        char id[33];
        if (auth_shadow_id(api.shadow, api.user, id, w->why, sizeof w->why) < 0) {
            enc[0] = w->digest[0] = w->netip[0] = 0;            /* cannot be checked: why */
        } else if (strcmp(id, sys_id)) {
            snprintf(w->why, sizeof w->why, "the system account's password is not known to vigil%s: set it through "
                     "vigil, or log in once to the web page, the API or the UI",
                     sys_id[0] ? " since it changed" : " yet");
            enc[0] = w->digest[0] = w->netip[0] = 0;
        }
    }
    if (w->kind && w->may && enc[0] && !open_pass(aad, enc, w->pass, sizeof w->pass))
        snprintf(w->why, sizeof w->why, "the stored password of '%s' cannot be read (%s missing, or another one): "
                 "set it again, or log in once to the web page, the API or the UI", name, SECRET_FILE);
    else if (w->kind == 1 && w->may && !enc[0])
        snprintf(w->why, sizeof w->why, "'%s' has no stored password yet: set it again, or log in once to the web "
                 "page, the API or the UI", name);
    wipe(enc, sizeof enc);
}

static void who_wipe(who_t *w) { wipe(w, sizeof *w); }

int users_digest(const char *name, char ha1[33], unsigned *perms)
{
    *perms = 0;
    ha1[0] = 0;
    who_t w;
    who(name, &w);
    int rc = 0;
    if (w.kind && w.may) {
        rc = -1;
        if (w.pass[0]) {
            ha1_of(name, w.pass, ha1);
            rc = 1;
        } else if (strlen(w.digest) == 32) {
            memcpy(ha1, w.digest, 33);
            rc = 1;
        }
    }
    if (rc == 1) *perms = w.perms;
    who_wipe(&w);
    return rc;
}

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
        keep_password(name, pass, NULL, NULL);
        return 1;
    }
    /* The system account, and "admin" as its other name until a user of
     * that name exists: whatever the file holds, root gets in. */
    if (!strcmp(name, api.user) || (!strcmp(name, "admin") && !has_admin)) {
        int rc = auth_check_shadow(api.shadow, api.user, pass, why, cap);
        if (rc == 1) {
            *perms = PERM_ALL;
            char id[33], w[8];
            if (auth_shadow_id(api.shadow, api.user, id, w, sizeof w) == 0) keep_password(name, pass, api.user, id);
        }
        return rc;
    }
    /* Unknown user: the same crypt() as a known one, so the time does not
     * say which names exist. */
    char r[HASH_MAX];
    auth_crypt(pass, "$6$vigilnouser$", r, sizeof r);
    return 0;
}

int users_login_netip(const char *name, const char *xm, unsigned *perms, char *why, size_t cap)
{
    *perms = 0;
    if (cap) why[0] = 0;
    who_t w;
    who(name, &w);
    int rc = 0;
    char have[NETIP_HASH] = "";
    if (w.pass[0]) md5_sofia(w.pass, have);
    else snprintf(have, sizeof have, "%s", w.netip);
    if (!w.kind) {
        rc = 0;
    } else if (!have[0]) {
        snprintf(why, cap, "%s", w.why[0] ? w.why : "no password");
        rc = w.may ? -1 : 0;
    } else if (auth_equal(xm, have) && w.may) {
        *perms = w.perms;
        rc = 1;
    }
    who_wipe(&w);
    wipe(have, sizeof have);
    return rc;
}

int users_login_with(const char *name, int (*check)(const char *pass, void *ctx), void *ctx, unsigned *perms,
                     char *why, size_t cap)
{
    *perms = 0;
    if (cap) why[0] = 0;
    who_t w;
    who(name, &w);
    if (!w.pass[0] && w.onvif[0]) {
        /* An older file's base64 password that could not be sealed (no key). */
        unsigned char p[KEY_MAX];
        int n = auth_base64_decode(w.onvif, strlen(w.onvif), p, sizeof p - 1);
        if (n > 0 && n <= PASS_MAX) {
            memcpy(w.pass, p, (size_t)n);
            w.pass[n] = 0;
        }
        wipe(p, sizeof p);
    }
    int rc = 0;
    if (!w.kind) {
        rc = 0;
    } else if (!w.pass[0]) {
        snprintf(why, cap, "%s", w.why[0] ? w.why : "no password");
        rc = w.may ? -1 : 0;
    } else if (check(w.pass, ctx) && w.may) {
        *perms = w.perms;
        rc = 1;
    }
    who_wipe(&w);
    return rc;
}

/* ---- configuration exports (src/api/backup.c) ------------------------------------------ */

char *users_export_text(users_secret_fn fn, void *ctx, int *n_users, int *n_pass, int *root, char *err, size_t cap)
{
    *n_users = *n_pass = *root = 0;
    config_api_t api;
    api_settings(&api);
    users_db_t *db = malloc(sizeof *db);
    if (!db) {
        snprintf(err, cap, "out of memory");
        return NULL;
    }
    pthread_mutex_lock(&g_lock);
    int bad = load_locked(api.users) < 0;
    *db = g_db;
    pthread_mutex_unlock(&g_lock);
    if (bad) {
        wipe(db, sizeof *db);
        free(db);
        snprintf(err, cap, "%.100s is invalid: fix it before exporting the accounts", api.users);
        return NULL;
    }
    char pass[PASS_MAX + 1], aad[96];
    int fail_fn = 0;
    *n_users = db->n_users;
    for (int i = 0; i < db->n_users; ++i) {
        user_rec_t *u = &db->users[i];
        user_aad(u->name, aad, sizeof aad);
        int have = fn && open_pass(aad, u->password_enc, pass, sizeof pass);
        /* No hash goes out: an import makes a new one from the password,
         * and a file without passwords has neither. */
        wipe(u->password, sizeof u->password);
        wipe(u->password_enc, sizeof u->password_enc);
        drop_old_user(u);
        if (have) {
            if (fn(ctx, aad, pass, u->password_enc, sizeof u->password_enc) < 0) fail_fn = 1;
            else ++*n_pass;
        }
        wipe(pass, sizeof pass);
    }
    /* The system account's, only while it is root's password now. */
    system_rec_t *s = &db->system;
    char id[33], why[200];
    system_aad(api.user, aad, sizeof aad);
    int have = fn && s->password_enc[0] && auth_shadow_id(api.shadow, api.user, id, why, sizeof why) == 0 &&
               !strcmp(id, s->shadow) && open_pass(aad, s->password_enc, pass, sizeof pass);
    memset(s, 0, sizeof *s);
    if (have) {
        if (fn(ctx, aad, pass, s->password_enc, sizeof s->password_enc) < 0) fail_fn = 1;
        else *root = 1;
    }
    wipe(pass, sizeof pass);
    char *text = NULL;
    if (fail_fn) {
        snprintf(err, cap, "cannot seal the accounts' passwords for the export");
    } else {
        builtins(db);
        cJSON *doc = schema_to_json(&users_schema_now, db, 0);
        text = doc ? schema_yaml_emit(&users_schema_now, doc, file_header) : NULL;
        cJSON_Delete(doc);
        if (!text) snprintf(err, cap, "out of memory");
    }
    wipe(db, sizeof *db);
    free(db);
    return text;
}

struct users_import {
    users_db_t db;
    char       root[PASS_MAX + 1];      /* the system account's password, "" none */
    int        n_pass;
};

users_import_t *users_import_prepare(const char *text, size_t len, users_secret_fn fn, void *ctx, const char *acct,
                                     char *err, size_t cap)
{
    users_import_t *im = calloc(1, sizeof *im);
    if (!im) {
        snprintf(err, cap, "out of memory");
        return NULL;
    }
    schema_errors_t errs = { 0 };
    cJSON *doc = len > FILE_MAX ? NULL : schema_yaml_parse(&users_schema, text, len, &errs);
    if (!doc || schema_from_json(&users_schema, doc, &im->db, 0, &errs) < 0 ||
        schema_validate(&users_schema, &im->db, &errs) < 0) {
        if (len > FILE_MAX) snprintf(err, cap, "the accounts are too large");
        else snprintf(err, cap, "accounts: %.60s%s%.120s", errs.n ? errs.e[0].path : "",
                      errs.n && errs.e[0].path[0] ? ": " : "", errs.n ? errs.e[0].msg : "not valid");
        cJSON_Delete(doc);
        users_import_free(im);
        return NULL;
    }
    cJSON_Delete(doc);
    users_db_t *db = &im->db;
    char pass[PASS_MAX + 1], aad[96];
    for (int i = 0; i < db->n_users; ++i) {
        user_rec_t *u = &db->users[i];
        if (!find_group(db, u->group) && strcmp(u->group, "admin") && strcmp(u->group, "user")) {
            snprintf(err, cap, "accounts: user '%.31s' is in group '%.31s', which the export does not have", u->name,
                     u->group);
            users_import_free(im);
            return NULL;
        }
        drop_old_user(u);
        if (!u->password_enc[0]) continue;      /* a hash of its own (if any) stays; else no login */
        user_aad(u->name, aad, sizeof aad);
        users_err_t e = { 0 };
        int rc = fn(ctx, aad, u->password_enc, pass, sizeof pass);
        if (rc == 0 && pass[0]) rc = new_password(aad, pass, u->password, u->password_enc, &e) ? -1 : 0;
        else wipe(u->password_enc, sizeof u->password_enc);
        wipe(pass, sizeof pass);
        if (rc < 0) {
            snprintf(err, cap, "accounts: the password of '%.31s' %.120s", u->name,
                     e.msg[0] ? e.msg : "does not open: the file was changed");
            users_import_free(im);
            return NULL;
        }
        if (u->password[0]) ++im->n_pass;
    }
    system_rec_t *s = &db->system;
    if (s->password_enc[0]) {
        system_aad(acct, aad, sizeof aad);
        int rc = fn(ctx, aad, s->password_enc, im->root, sizeof im->root);
        if (rc < 0) {
            snprintf(err, cap, "accounts: the system account's password does not open: the file was changed");
            users_import_free(im);
            return NULL;
        }
    }
    memset(s, 0, sizeof *s);                    /* users_set_system_password seals it anew */
    builtins(db);
    return im;
}

void users_import_info(const users_import_t *im, int *n_users, int *n_pass, int *n_groups, int *root)
{
    *n_users = im->db.n_users;
    *n_pass = im->n_pass;
    *n_groups = im->db.n_groups;
    *root = im->root[0] != 0;
}

const char *users_import_user(const users_import_t *im, int i, int *has_pass)
{
    if (i < 0 || i >= im->db.n_users) return NULL;
    *has_pass = im->db.users[i].password[0] != 0;
    return im->db.users[i].name;
}

const char *users_import_root(const users_import_t *im) { return im->root; }

/* Under g_lock: an invalid file is not written over without a copy. */
static void keep_bad_locked(void)
{
    if (!g_bad) return;
    char bad[220];
    snprintf(bad, sizeof bad, "%s.bad", g_path);
    if (rename(g_path, bad) == 0) vg_log("users: the invalid %s is kept as %s", g_path, bad);
    g_bad = 0;
}

users_rc_t users_import_commit(users_import_t *im, users_err_t *e)
{
    memset(e, 0, sizeof *e);
    pthread_mutex_lock(&g_lock);
    load();
    keep_bad_locked();
    users_db_t *db = malloc(sizeof *db);
    users_rc_t rc = USERS_EIO;
    if (!db) {
        snprintf(e->msg, sizeof e->msg, "out of memory");
    } else {
        *db = im->db;
        /* root's password is the shadow file's, which an import changes
         * only through users_set_system_password: until then its sealed
         * copy here still belongs to it. */
        db->system = g_db.system;
        rc = save_locked(db, e);
        wipe(db, sizeof *db);
        free(db);
    }
    pthread_mutex_unlock(&g_lock);
    if (rc == USERS_OK) vg_log("users: %d user(s) and %d group(s) imported", im->db.n_users, im->db.n_groups);
    return rc;
}

void users_import_free(users_import_t *im)
{
    if (!im) return;
    wipe(im, sizeof *im);
    free(im);
}

users_rc_t users_reset(users_err_t *e)
{
    memset(e, 0, sizeof *e);
    users_db_t *db = calloc(1, sizeof *db);
    if (!db) return fail(e, USERS_EIO, "", "%sout of memory", "");
    pthread_mutex_lock(&g_lock);
    load();
    keep_bad_locked();
    users_rc_t rc = save_locked(db, e);
    pthread_mutex_unlock(&g_lock);
    free(db);
    if (rc == USERS_OK) vg_log("users: reset (no users; the built-in groups)");
    return rc;
}
