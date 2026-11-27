/* SPDX-License-Identifier: GPL-3.0-or-later */
/* users.h — vigil's own accounts: users in groups, groups with permissions.
 *
 * The file is api.users (default /etc/vigil.users.yaml, mode 0600), YAML
 * written by the schema engine like vigil.yaml, so it documents itself:
 *
 *   groups:
 *     - name: viewer
 *       live: true
 *       ...
 *   users:
 *     - name: anna
 *       group: viewer
 *       enabled: true
 *       password: "$6$...$..."       (crypt(3); never leaves the process)
 *       onvif: "..."                 (the password, base64: users_login_with)
 *       netip_password: "tlJwpbo6"   (Xiongmai's hash of it: users_login_netip)
 *   system:
 *     netip_password: "..."          (the system account's, learnt at its login)
 *     shadow: "..."                  (which shadow entry that was)
 *
 * The onvif key and netip_password are the password's secrets for clients
 * that never send the password itself; like the hash, the API never shows
 * them. Both are written when a password is set (the onvif key only while
 * onvif_server is on), and filled in at a login with the password when
 * the account lacks them (users_login).
 *
 * Two groups always exist, whether the file has them or not: "admin"
 * (every permission; cannot be changed or deleted) and "user" (live view
 * and playback by default; editable, not deletable).
 *
 * Logging in (users_login): a user of the file, enabled, with the right
 * password, gets its group's permissions. The system account (api.user,
 * root, checked in api.shadow) always logs in with every permission, so
 * nobody can be locked out by the file; and "admin" is that account too
 * until a user named "admin" is created.
 *
 * Every function is thread-safe (one lock; the file is read again when it
 * changed on disk). The API (api.c, api_users.c) and the UI's bridge use
 * it; src/ui does not include it.
 */
#ifndef USERS_H
#define USERS_H

#include <stddef.h>

#define USERS_MAX       32
#define GROUPS_MAX      16
#define USERS_NAME_MAX  32              /* with the NUL */
#define USERS_PASS_MIN  4

/* Permissions, as bits. */
enum {
    PERM_LIVE     = 1u << 0,            /* live view, status, events */
    PERM_PLAYBACK = 1u << 1,            /* recordings */
    PERM_PTZ      = 1u << 2,            /* camera control */
    PERM_CAMERAS  = 1u << 3,            /* the camera list, discovery */
    PERM_STORAGE  = 1u << 4,            /* recording, disks, network storage */
    PERM_NETWORK  = 1u << 5,            /* network, DDNS, mail, alarm center */
    PERM_SYSTEM   = 1u << 6,            /* time, host name, the UI, the whole configuration */
    PERM_USERS    = 1u << 7,            /* accounts, and the API's own settings */
    PERM_REMOTE   = 1u << 8,            /* the zero channel: the screen and its mouse and keyboard */
    PERM_N        = 9,
    PERM_ALL      = (1u << 9) - 1,
};

/* "live", "playback", "ptz", "cameras", "storage", "network", "system",
 * "users", "remote": bit i's name. */
extern const char *const users_perm_names[PERM_N];
/* The bit of a name, 0 if none. */
unsigned users_perm_bit(const char *name);

typedef struct {
    char     name[USERS_NAME_MAX];
    unsigned perms;
    int      builtin;                   /* admin or user */
    int      editable;                  /* not admin */
    int      members;                   /* users in it */
} users_group_t;

typedef struct {
    char name[USERS_NAME_MAX];
    char group[USERS_NAME_MAX];
    int  enabled;
    int  password_set;
} users_user_t;

typedef enum {
    USERS_OK = 0,
    USERS_EINVAL,                       /* a bad value: `field` and `msg` */
    USERS_ENOENT,                       /* no such user or group */
    USERS_EEXIST,                       /* the name is taken */
    USERS_EPROTECTED,                   /* a built-in group cannot be changed that way */
    USERS_EINUSE,                       /* a group that still has users */
    USERS_EIO,                          /* the file could not be read or written: msg */
} users_rc_t;

typedef struct {
    char field[16];                     /* "name", "group", "password", "permissions", "" */
    char msg[160];
} users_err_t;

/* The file now (api.users of the configuration in force); read again
 * when it changed. 0, or -1 when it exists and cannot be loaded (logged;
 * no user of the file can log in then, the system account still can). */
int users_reload(void);

/* Moves whenever the users or groups change (a write, or the file read
 * again): a UI list rebuilds when it does. */
unsigned users_generation(void);

/* Every user / group, in file order (built-ins first for groups); returns
 * how many (at most `max`). */
int users_list(users_user_t *u, int max);
int users_groups(users_group_t *g, int max);

/* A new user; `group` NULL or "" is "user". */
users_rc_t users_add(const char *name, const char *group, const char *password, int enabled,
                     users_err_t *e);
/* Change a user: `group` NULL keeps it, `enabled` < 0 keeps it, `password`
 * NULL keeps it. */
users_rc_t users_update(const char *name, const char *group, int enabled, const char *password,
                        users_err_t *e);
users_rc_t users_delete(const char *name, users_err_t *e);

/* A new group (`create`) or new permissions for one. */
users_rc_t users_group_set(const char *name, unsigned perms, int create, users_err_t *e);
users_rc_t users_group_delete(const char *name, users_err_t *e);

/* Check a login. Returns 1 with `*perms`, 0 when the password is
 * wrong or the user unknown or disabled, -1 when it cannot be checked
 * (`why`, for the log: no shadow file, a locked account, ...). */
int users_login(const char *name, const char *pass, unsigned *perms, char *why, size_t cap);

/* The realm of Digest logins (the RTSP server): a user's HA1 is
 * md5(name:USERS_REALM:password). */
#define USERS_REALM "vigil"

/* For a Digest login (RFC 2617): `name`'s HA1 (32 hex digits) and its
 * permissions. 1 when the user may log in and its HA1 is known; 0 when
 * not (unknown, disabled, no password); -1 when it may but its HA1 is not
 * known: a password set before vigild kept HA1s and not used to log in
 * since (crypt hashes cannot give one). The system account's is learnt
 * when it logs in (UI, API, web), as its NETIP hash is, and kept in the
 * file for as long as its shadow entry is the same. */
int users_digest(const char *name, char ha1[33], unsigned *perms);
/* Check a login whose password the client proves it knows without
 * sending it (ONVIF's WS-UsernameToken digest, HTTP Digest): `check`
 * gets the password and says whether the client's proof fits it (1).
 * That needs the password itself, which a crypt(3) hash cannot give:
 * users of the file have an ONVIF key (the password, base64) written
 * with it while onvif_server is on, or at their next login with it then;
 * the system account has none.
 * Returns as users_login: 1 with `*perms`, 0 refused, -1 cannot be
 * checked (`why`: no key yet, the system account). */
int users_login_with(const char *name, int (*check)(const char *pass, void *ctx), void *ctx, unsigned *perms,
                     char *why, size_t cap);

/* A login from a NETIP client (src/api/netip_srv.c), which sends only
 * Xiongmai's 8-character hash of the password (md5_sofia), never the
 * password: checked against the hash kept with the user's password
 * (`netip_password`, set with it), or for the system account the one
 * learnt at its last login with the password (users_login keeps it,
 * with which shadow entry it belongs to). -1 with `why` when there is
 * none to check against yet. */
int users_login_netip(const char *name, const char *xm, unsigned *perms, char *why, size_t cap);

/* A crypt(3) hash of `pass` with a fresh random salt: SHA-512, else
 * SHA-256, else MD5, whichever the C library has. 0, or -1. */
int users_hash(const char *pass, char *out, size_t cap);

#endif /* USERS_H */
