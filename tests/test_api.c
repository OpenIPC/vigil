/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_api.c — unit tests for the API's password check (src/api/auth.c). */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>
#include <sys/stat.h>

#include "api_int.h"
#include "auth.h"
#include "config.h"
#include "hal_capture.h"
#include "md5.h"
#include "users.h"

/* The configuration's URL check links the capture source; it never runs. */
int  hal_cap_start(int in, int ch, hal_cap_cb cb, void *user) { (void)in; (void)ch; (void)cb; (void)user; return -1; }
void hal_cap_stop(int in) { (void)in; }

static int g_fail, g_checks;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_fail; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* openssl passwd -6 -salt vigiltestsalt secret; openssl passwd -1 -salt abcdefgh secret */
#define SHA512_SECRET "$6$vigiltestsalt$YNf0cVVTPqcmB27N7LaoJSiqJTsS7DvT6GejoILXrgfuBwdj8rCp9lnFrtSM5TO4kEOwYmcBLCsL4TzvT5U4b1"
#define MD5_SECRET    "$1$abcdefgh$cHJi5PXp/ki/ktXzqlk6I1"

static void test_base64(void)
{
    unsigned char out[64];
    int n = auth_base64_decode("cm9vdDpzZWNyZXQ=", 16, out, sizeof out);
    CHECK(n == 11 && !memcmp(out, "root:secret", 11), "decode, padded");
    n = auth_base64_decode("cm9vdDpzZWNyZXQ", 15, out, sizeof out);
    CHECK(n == 11 && !memcmp(out, "root:secret", 11), "decode, unpadded");
    CHECK(auth_base64_decode("cm9v*DpzZWNyZXQ=", 16, out, sizeof out) < 0, "bad character");
    CHECK(auth_base64_decode("cm9vd", 5, out, sizeof out) < 0, "dangling character");
    CHECK(auth_base64_decode("cm9vdDpzZWNyZXQ=", 16, out, 4) < 0, "output too small");
}

static void test_basic(void)
{
    char user[32], pass[32];
    CHECK(auth_parse_basic("Basic cm9vdDpzZWNyZXQ=", user, sizeof user, pass, sizeof pass) == 0 &&
          !strcmp(user, "root") && !strcmp(pass, "secret"), "Basic");
    CHECK(auth_parse_basic("basic   cm9vdDpzZWNyZXQ=", user, sizeof user, pass, sizeof pass) == 0,
          "scheme is case-insensitive");
    /* "root:pa:ss" -- only the first colon splits */
    CHECK(auth_parse_basic("Basic cm9vdDpwYTpzcw==", user, sizeof user, pass, sizeof pass) == 0 &&
          !strcmp(pass, "pa:ss"), "colon in the password");
    CHECK(auth_parse_basic("Digest x", user, sizeof user, pass, sizeof pass) < 0, "not Basic");
    CHECK(auth_parse_basic("Basic cm9vdA==", user, sizeof user, pass, sizeof pass) < 0, "no colon");
    CHECK(auth_parse_basic("Basic cm9vdDpzZWNyZXQ=", user, 4, pass, sizeof pass) < 0, "user too long");
    CHECK(auth_parse_basic(NULL, user, sizeof user, pass, sizeof pass) < 0, "no header");
}

static void test_equal(void)
{
    CHECK(auth_equal("abc", "abc"), "equal");
    CHECK(!auth_equal("abc", "abd"), "differ");
    CHECK(!auth_equal("abc", "abcd") && !auth_equal("abcd", "abc"), "lengths");
    CHECK(!auth_equal("", "a") && auth_equal("", ""), "empty");
}

static void test_shadow(void)
{
    char path[] = "/tmp/vigil-shadow-XXXXXX", why[160];
    int fd = mkstemp(path);
    if (fd < 0) return;
    FILE *f = fdopen(fd, "w");
    fprintf(f, "daemon:*:0:0:99999:7:::\n"
               "root:" SHA512_SECRET ":19000:0:99999:7:::\n"
               "admin:" MD5_SECRET ":19000:0:99999:7:::\n"
               "rootless:!" SHA512_SECRET ":19000::::::\n"
               "nopass::19000::::::\n");
    fclose(f);

    CHECK(auth_check_shadow(path, "root", "secret", why, sizeof why) == 1, "SHA-512 hash: %s", why);
    CHECK(auth_check_shadow(path, "root", "Secret", why, sizeof why) == 0, "wrong password");
    CHECK(auth_check_shadow(path, "root", "", why, sizeof why) == 0, "empty password");
    CHECK(auth_check_shadow(path, "admin", "secret", why, sizeof why) == 1, "MD5 hash: %s", why);
    CHECK(auth_check_shadow(path, "roo", "secret", why, sizeof why) < 0 && strstr(why, "no account"),
          "prefix of a user name");
    CHECK(auth_check_shadow(path, "rootless", "secret", why, sizeof why) < 0 && strstr(why, "locked"),
          "locked account");
    CHECK(auth_check_shadow(path, "daemon", "", why, sizeof why) < 0 && strstr(why, "locked"), "'*' account");
    CHECK(auth_check_shadow(path, "nopass", "", why, sizeof why) < 0 && strstr(why, "no password"),
          "account without a password");
    CHECK(auth_check_shadow("/nonexistent/shadow", "root", "secret", why, sizeof why) < 0 &&
          strstr(why, "cannot open"), "no file");
    unlink(path);
}


/* ---- accounts ------------------------------------------------------------------------ */

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = calloc(1, 65536);
    if (fread(b, 1, 65535, f)) {}
    fclose(f);
    return b;
}

static void test_users(void)
{
    char dir[] = "/tmp/vigil-users-XXXXXX", cfg[128], shadow[128], users[128], why[160];
    if (!mkdtemp(dir)) return;
    snprintf(cfg, sizeof cfg, "%s/vigil.yaml", dir);
    snprintf(shadow, sizeof shadow, "%s/shadow", dir);
    snprintf(users, sizeof users, "%s/users.yaml", dir);
    FILE *f = fopen(shadow, "w");
    fprintf(f, "root:" SHA512_SECRET ":19000:0:99999:7:::\n");
    fclose(f);
    f = fopen(cfg, "w");
    fprintf(f, "api:\n  user: root\n  shadow: %s\n  users: %s\n", shadow, users);
    fclose(f);
    CHECK(config_init(cfg) == 0, "configuration");

    char h[128];
    CHECK(users_hash("pw1234", h, sizeof h) == 0 && !strncmp(h, "$6$", 3), "SHA-512 hash: %s", h);
    char r[128];
    CHECK(auth_crypt("pw1234", h, r, sizeof r) == 0 && !strcmp(r, h), "the hash verifies");
    char h2[128];
    users_hash("pw1234", h2, sizeof h2);
    CHECK(strcmp(h, h2) != 0, "a fresh salt each time");

    /* No file: built-ins only; root logs in, admin is root's alias. */
    users_group_t g[GROUPS_MAX];
    int ng = users_groups(g, GROUPS_MAX);
    CHECK(ng == 2 && !strcmp(g[0].name, "admin") && g[0].perms == PERM_ALL && !g[0].editable &&
          !strcmp(g[1].name, "user") && g[1].perms == (PERM_LIVE | PERM_PLAYBACK) && g[1].editable,
          "built-in groups");
    unsigned perms = 0;
    CHECK(users_login("root", "secret", &perms, why, sizeof why) == 1 && perms == PERM_ALL, "root: %s", why);
    CHECK(users_login("root", "nope", &perms, why, sizeof why) == 0, "root, wrong password");
    CHECK(users_login("admin", "secret", &perms, why, sizeof why) == 1 && perms == PERM_ALL,
          "admin is root until there is an admin user");
    CHECK(users_login("anna", "secret", &perms, why, sizeof why) == 0, "unknown user");

    users_err_t e;
    CHECK(users_group_set("viewer", PERM_LIVE, 1, &e) == USERS_OK, "add viewer: %s", e.msg);
    CHECK(users_group_set("storage", PERM_STORAGE | PERM_LIVE, 1, &e) == USERS_OK, "add storage");
    CHECK(users_group_set("viewer", PERM_LIVE, 1, &e) == USERS_EEXIST, "a group twice");
    CHECK(users_group_set("admin", PERM_LIVE, 0, &e) == USERS_EPROTECTED, "admin cannot change");
    CHECK(users_group_set("Bad Name", 0, 1, &e) == USERS_EINVAL && !strcmp(e.field, "name"), "bad name");
    CHECK(users_group_delete("user", &e) == USERS_EPROTECTED, "user group cannot be deleted");
    CHECK(users_group_delete("admin", &e) == USERS_EPROTECTED, "admin group cannot be deleted");
    CHECK(users_group_set("user", PERM_LIVE, 0, &e) == USERS_OK, "user group is editable");

    CHECK(users_add("anna", "viewer", "annapw", 1, &e) == USERS_OK, "add anna: %s", e.msg);
    CHECK(users_add("bob", "storage", "bobpw1", 1, &e) == USERS_OK, "add bob");
    CHECK(users_add("carl", NULL, "carlpw", 0, &e) == USERS_OK, "add carl (disabled, group user)");
    CHECK(users_add("root", "admin", "rootpw", 1, &e) == USERS_EINVAL && !strcmp(e.field, "name"),
          "root is reserved");
    CHECK(users_add("dan", "nogroup", "danpw1", 1, &e) == USERS_EINVAL && !strcmp(e.field, "group"),
          "unknown group");
    CHECK(users_add("dan", "viewer", "abc", 1, &e) == USERS_EINVAL && !strcmp(e.field, "password"),
          "password too short");
    CHECK(users_add("anna", "viewer", "annapw", 1, &e) == USERS_EEXIST, "a user twice");
    CHECK(users_group_delete("viewer", &e) == USERS_EINUSE, "a group with users stays");

    CHECK(users_login("anna", "annapw", &perms, why, sizeof why) == 1 && perms == PERM_LIVE, "anna: live");
    CHECK(users_login("anna", "wrong", &perms, why, sizeof why) == 0 && perms == 0, "anna, wrong password");
    CHECK(users_login("bob", "bobpw1", &perms, why, sizeof why) == 1 && perms == (PERM_STORAGE | PERM_LIVE),
          "bob: storage");
    CHECK(users_login("carl", "carlpw", &perms, why, sizeof why) == 0, "a disabled user is refused");
    CHECK(users_update("carl", NULL, 1, NULL, &e) == USERS_OK &&
          users_login("carl", "carlpw", &perms, why, sizeof why) == 1 && perms == PERM_LIVE,
          "enabled, in the changed user group");
    CHECK(users_update("carl", "viewer", -1, "newpw1", &e) == USERS_OK &&
          users_login("carl", "carlpw", &perms, why, sizeof why) == 0 &&
          users_login("carl", "newpw1", &perms, why, sizeof why) == 1, "password changed");

    /* NETIP clients send Xiongmai's hash of the password: the one set
     * with the password, and the system account's learnt at its login. */
    char xm[9];
    md5_sofia("annapw", xm);
    CHECK(users_login_netip("anna", xm, &perms, why, sizeof why) == 1 && perms == PERM_LIVE, "anna over NETIP");
    md5_sofia("wrong", xm);
    CHECK(users_login_netip("anna", xm, &perms, why, sizeof why) == 0 && perms == 0, "anna over NETIP, wrong");
    md5_sofia("newpw1", xm);
    CHECK(users_login_netip("carl", xm, &perms, why, sizeof why) == 1, "carl's new password over NETIP");
    md5_sofia("carlpw", xm);
    CHECK(users_login_netip("carl", xm, &perms, why, sizeof why) == 0, "not carl's old one");
    md5_sofia("secret", xm);
    CHECK(users_login_netip("root", xm, &perms, why, sizeof why) == 1 && perms == PERM_ALL,
          "root over NETIP, learnt at its login: %s", why);
    CHECK(users_login_netip("nobody", xm, &perms, why, sizeof why) == 0, "unknown over NETIP");
    f = fopen(shadow, "w");
    fprintf(f, "root:$6$other$abc:19000:0:99999:7:::\n");
    fclose(f);
    CHECK(users_login_netip("root", xm, &perms, why, sizeof why) == -1 && strstr(why, "changed"),
          "root's password changed (passwd): its NETIP hash is forgotten: %s", why);
    f = fopen(shadow, "w");
    fprintf(f, "root:" SHA512_SECRET ":19000:0:99999:7:::\n");
    fclose(f);
    CHECK(users_login_netip("root", xm, &perms, why, sizeof why) == 1, "and back");

    /* The file: 0600, commented YAML, hashes only; read back as it was. */
    struct stat st;
    CHECK(stat(users, &st) == 0 && (st.st_mode & 0777) == 0600, "users file is 0600 (%o)", st.st_mode & 0777);
    char *text = slurp(users);
    CHECK(text && strstr(text, "# Groups") && strstr(text, "name: anna") && strstr(text, "password: \"$6$") &&
          !strstr(text, "annapw"), "commented YAML with hashes");
    free(text);
    unsigned gen = users_generation();
    users_user_t u[USERS_MAX];
    int nu = users_list(u, USERS_MAX);
    CHECK(nu == 3 && !strcmp(u[0].name, "anna") && u[0].password_set && !strcmp(u[2].group, "viewer"),
          "list: %d users", nu);
    /* Changed on disk by someone else: read again. */
    f = fopen(users, "a");
    fprintf(f, "\n");
    fclose(f);
    struct stat st2;
    stat(users, &st2);
    if (st2.st_mtime == st.st_mtime) {
        struct timespec ts[2] = { { st.st_mtime + 2, 0 }, { st.st_mtime + 2, 0 } };
        utimensat(AT_FDCWD, users, ts, 0);
    }
    CHECK(users_generation() != gen, "a changed file is read again");
    CHECK(users_list(u, USERS_MAX) == 3, "same users after reading again");

    /* An admin user replaces the alias. */
    CHECK(users_add("admin", "user", "adminpw", 1, &e) == USERS_OK, "add admin");
    CHECK(users_login("admin", "secret", &perms, why, sizeof why) == 0, "admin is no longer root");
    CHECK(users_login("admin", "adminpw", &perms, why, sizeof why) == 1 && perms == PERM_LIVE,
          "admin user: its group's perms");
    CHECK(users_login("root", "secret", &perms, why, sizeof why) == 1 && perms == PERM_ALL,
          "root still gets in");
    CHECK(users_delete("admin", &e) == USERS_OK && users_delete("admin", &e) == USERS_ENOENT, "delete");
    CHECK(users_login("admin", "secret", &perms, why, sizeof why) == 1, "admin is root's again");

    /* An invalid file: only root, and nothing is saved over it. */
    f = fopen(users, "w");
    fprintf(f, "users:\n  - name: Bad Name\n");
    fclose(f);
    CHECK(users_reload() < 0, "invalid file");
    CHECK(users_login("anna", "annapw", &perms, why, sizeof why) == 0, "no users from an invalid file");
    CHECK(users_login("root", "secret", &perms, why, sizeof why) == 1, "root still");
    CHECK(users_add("eve", NULL, "evepw1", 1, &e) == USERS_EIO, "not saved over an invalid file");

    char cmd[160];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd)) {}
}

static void test_perm_table(void)
{
    static const struct { const char *m, *uri; unsigned need; } T[] = {
        { "GET", "/api/v1/status", PERM_LIVE },
        { "GET", "/api/v1/ws", PERM_LIVE },
        { "GET", "/api/v1/schema", 0 },
        { "GET", "/api/v1/me", 0 },
        { "GET", "/api/v1/jobs/3", 0 },
        { "GET", "/api/v1/config", PERM_SYSTEM },
        { "PUT", "/api/v1/config", PERM_ALL & ~PERM_REMOTE },
        { "PUT", "/api/v1/config/web", PERM_USERS },
        { "GET", "/api/v1/config/cameras", PERM_CAMERAS },
        { "PUT", "/api/v1/config/record", PERM_STORAGE },
        { "PUT", "/api/v1/config/schedule", PERM_STORAGE },
        { "GET", "/api/v1/config/quota", PERM_STORAGE },
        { "PUT", "/api/v1/config/nas", PERM_STORAGE },
        { "PUT", "/api/v1/config/smtp", PERM_NETWORK },
        { "PUT", "/api/v1/config/ddns", PERM_NETWORK },
        { "PUT", "/api/v1/config/time", PERM_SYSTEM },
        { "PUT", "/api/v1/config/ui", PERM_SYSTEM },
        { "PUT", "/api/v1/config/api", PERM_USERS },
        { "GET", "/api/v1/config/unknown", PERM_SYSTEM },
        { "PUT", "/api/v1/network/ethernet", PERM_NETWORK },
        { "POST", "/api/v1/system/time", PERM_SYSTEM },
        { "GET", "/api/v1/disks", PERM_STORAGE },
        { "POST", "/api/v1/disks/sda/format", PERM_STORAGE },
        { "POST", "/api/v1/storage/nas/test", PERM_STORAGE },
        { "GET", "/api/v1/discovery", PERM_CAMERAS },
        { "POST", "/api/v1/channels/3/assign", PERM_CAMERAS },
        { "GET", "/api/v1/users", PERM_USERS },
        { "DELETE", "/api/v1/groups/viewer", PERM_USERS },
        { "GET", "/api/v1/networking", PERM_SYSTEM },
        { "GET", "/api/v1/whatever", PERM_SYSTEM },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; ++i) {
        unsigned n = api_perm_needed(T[i].m, T[i].uri);
        CHECK(n == T[i].need, "%s %s needs %#x, not %#x", T[i].m, T[i].uri, T[i].need, n);
    }
    CHECK(users_perm_bit("storage") == PERM_STORAGE && users_perm_bit("nope") == 0, "names");
}

int main(void)
{
    test_base64();
    test_basic();
    test_equal();
    test_shadow();
    test_users();
    test_perm_table();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
