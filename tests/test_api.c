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
#include "rtsp_client.h"
#include "rtsp_server.h"
#include "secret.h"
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

/* users_digest gives `name` with md5(name:vigil:pass). */
static int digest_is(const char *name, const char *pass, unsigned want_perms)
{
    char ha1[33], buf[160], want[33];
    unsigned perms;
    snprintf(buf, sizeof buf, "%s:%s:%s", name, USERS_REALM, pass);
    md5_hex(buf, strlen(buf), want);
    return users_digest(name, ha1, &perms) == 1 && !strcmp(ha1, want) && perms == want_perms;
}
/* users_login_with's check: is it the password in ctx? */
static int pass_is(const char *pass, void *ctx) { return !strcmp(pass, (const char *)ctx); }

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
    char ha1[33];
    CHECK(digest_is("root", "secret", PERM_ALL) && digest_is("admin", "secret", PERM_ALL),
          "the system account's HA1, learnt at its login, under both names");
    CHECK(users_digest("anna", ha1, &perms) == 0, "no HA1 for an unknown user");

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
    CHECK(digest_is("anna", "annapw", PERM_LIVE) && digest_is("bob", "bobpw1", PERM_STORAGE | PERM_LIVE),
          "users' HA1s kept with their passwords");
    CHECK(users_digest("carl", ha1, &perms) == 0, "none for a disabled user");

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
    CHECK(digest_is("carl", "newpw1", PERM_LIVE), "the HA1 follows the password");

    /* ONVIF's digests: checked against the password itself, sealed with
     * it; the system account's since its login above. */
    CHECK(users_login_with("carl", pass_is, "newpw1", &perms, why, sizeof why) == 1 && perms == PERM_LIVE,
          "digest login with the ONVIF key: %s", why);
    CHECK(users_login_with("carl", pass_is, "carlpw", &perms, why, sizeof why) == 0 && perms == 0,
          "digest login, wrong password");
    CHECK(users_login_with("nobody", pass_is, "x", &perms, why, sizeof why) == 0, "digest login, unknown user");
    CHECK(users_login_with("root", pass_is, "secret", &perms, why, sizeof why) == 1 && perms == PERM_ALL,
          "the system account too: %s", why);
    CHECK(users_login_with("admin", pass_is, "secret", &perms, why, sizeof why) == 1, "and as admin");

    /* NETIP clients send Xiongmai's hash of the password: made from the
     * sealed one. */
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
          "root over NETIP, known since its login: %s", why);
    CHECK(users_login_netip("nobody", xm, &perms, why, sizeof why) == 0, "unknown over NETIP");
    f = fopen(shadow, "w");
    fprintf(f, "root:$6$other$abc:19000:0:99999:7:::\n");
    fclose(f);
    CHECK(users_login_netip("root", xm, &perms, why, sizeof why) == -1 && strstr(why, "changed"),
          "root's password changed (passwd): the sealed one is not used: %s", why);
    f = fopen(shadow, "w");
    fprintf(f, "root:" SHA512_SECRET ":19000:0:99999:7:::\n");
    fclose(f);
    CHECK(users_login_netip("root", xm, &perms, why, sizeof why) == 1, "and back");

    /* The file: 0600, commented YAML, hashes only; read back as it was. */
    struct stat st;
    CHECK(stat(users, &st) == 0 && (st.st_mode & 0777) == 0600, "users file is 0600 (%o)", st.st_mode & 0777);
    char *text = slurp(users);
    CHECK(text && strstr(text, "# Groups") && strstr(text, "name: anna") && strstr(text, "password: \"$6$") &&
          strstr(text, "password_enc: \"enc:v1:") && !strstr(text, "annapw") && !strstr(text, "onvif") &&
          !strstr(text, "netip_password") && !strstr(text, "digest"), "commented YAML: hashes, sealed passwords:\n%s",
          text ? text : "");
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
    /* Edited in place, the same length, within the same second. */
    gen = users_generation();
    stat(users, &st);
    text = slurp(users);
    char *grp = text ? strstr(text, "# Groups") : NULL;
    if (grp) {
        grp[2] = 'g';
        f = fopen(users, "r+");
        fputs(text, f);
        fclose(f);
        struct timespec ts[2] = { st.st_mtim, st.st_mtim };
        ts[0].tv_nsec = ts[1].tv_nsec = (st.st_mtim.tv_nsec + 1) % 1000000000L;
        utimensat(AT_FDCWD, users, ts, 0);
    }
    free(text);
    CHECK(grp && users_generation() != gen, "an edit in place of the same length, in the same second: read again");

    /* An admin user replaces the alias. */
    CHECK(users_add("admin", "user", "adminpw", 1, &e) == USERS_OK, "add admin");
    CHECK(users_login("admin", "secret", &perms, why, sizeof why) == 0, "admin is no longer root");
    CHECK(users_login("admin", "adminpw", &perms, why, sizeof why) == 1 && perms == PERM_LIVE,
          "admin user: its group's perms");
    CHECK(users_login("root", "secret", &perms, why, sizeof why) == 1 && perms == PERM_ALL,
          "root still gets in");
    CHECK(users_delete("admin", &e) == USERS_OK && users_delete("admin", &e) == USERS_ENOENT, "delete");
    CHECK(users_login("admin", "secret", &perms, why, sizeof why) == 1, "admin is root's again");

    /* With the ONVIF server off the password is sealed all the same:
     * turned on later, ONVIF logins work at once. */
    f = fopen(cfg, "w");
    fprintf(f, "api:\n  user: root\n  shadow: %s\n  users: %s\nonvif_server:\n  enabled: false\n", shadow, users);
    fclose(f);
    CHECK(config_init(cfg) == 0, "configuration, ONVIF off");
    CHECK(users_add("dan", "viewer", "danpw1", 1, &e) == USERS_OK, "add dan");
    CHECK(users_login_with("dan", pass_is, "danpw1", &perms, why, sizeof why) == 1, "dan's ONVIF login: %s", why);
    f = fopen(cfg, "w");
    fprintf(f, "api:\n  user: root\n  shadow: %s\n  users: %s\n", shadow, users);
    fclose(f);
    CHECK(config_init(cfg) == 0, "configuration, ONVIF on");

    /* A file from an older vigild: the password in base64 ("onvif"), with
     * its hashes; sealed as it is read, the old fields gone. */
    char xm_sec[9], ha1_sec[33], buf[200];
    md5_sofia("secret", xm_sec);
    snprintf(buf, sizeof buf, "dora:%s:secret", USERS_REALM);
    md5_hex(buf, strlen(buf), ha1_sec);
    f = fopen(users, "w");
    fprintf(f, "users:\n  - name: dora\n    group: user\n    password: \"" SHA512_SECRET "\"\n"
               "    digest: \"%s\"\n    onvif: c2VjcmV0\n    netip_password: \"%s\"\n", ha1_sec, xm_sec);
    fclose(f);
    CHECK(users_reload() == 0, "an older file with an ONVIF key");
    text = slurp(users);
    CHECK(text && strstr(text, "password_enc: \"enc:v1:") && !strstr(text, "c2VjcmV0") && !strstr(text, "onvif") &&
              !strstr(text, "digest") && !strstr(text, "netip_password"),
          "rewritten at once: sealed, the old fields gone:\n%s", text ? text : "");
    free(text);
    CHECK(users_login_with("dora", pass_is, "secret", &perms, why, sizeof why) == 1 &&
              users_login_netip("dora", xm_sec, &perms, why, sizeof why) == 1 &&
              digest_is("dora", "secret", PERM_LIVE | PERM_PLAYBACK),
          "ONVIF, NETIP and RTSP from the sealed password: %s", why);

    /* An older file with only the hashes (the ONVIF server was off): they
     * keep working; ONVIF waits for the password, which the next login
     * with it seals, the hashes then gone. */
    snprintf(buf, sizeof buf, "fay:%s:secret", USERS_REALM);
    char ha1_fay[33];
    md5_hex(buf, strlen(buf), ha1_fay);
    f = fopen(users, "w");
    fprintf(f, "users:\n  - name: fay\n    password: \"" SHA512_SECRET "\"\n    digest: \"%s\"\n"
               "    netip_password: \"%s\"\n  - name: gus\n    password: \"" SHA512_SECRET "\"\n", ha1_fay, xm_sec);
    fclose(f);
    CHECK(users_reload() == 0, "an older file with hashes only");
    CHECK(digest_is("fay", "secret", PERM_LIVE | PERM_PLAYBACK) &&
              users_login_netip("fay", xm_sec, &perms, why, sizeof why) == 1, "the old hashes work: %s", why);
    CHECK(users_login_with("fay", pass_is, "secret", &perms, why, sizeof why) == -1 && strstr(why, "log in once"),
          "ONVIF cannot yet: %s", why);
    CHECK(users_digest("gus", ha1, &perms) == -1 && users_login_netip("gus", xm_sec, &perms, why, sizeof why) == -1,
          "a user with only its crypt hash: nothing yet");
    CHECK(users_login("gus", "secret", &perms, why, sizeof why) == 1, "gus logs in with the password");
    text = slurp(users);
    CHECK(text && strstr(text, "netip_password: ") && strstr(text, "From an older vigild"),
          "the file rewritten, fay's old hashes kept until she does:\n%s", text ? text : "");
    free(text);
    CHECK(users_login("fay", "secret", &perms, why, sizeof why) == 1, "fay logs in with the password");
    CHECK(users_login_with("fay", pass_is, "secret", &perms, why, sizeof why) == 1 &&
              users_login_netip("gus", xm_sec, &perms, why, sizeof why) == 1 &&
              digest_is("gus", "secret", PERM_LIVE | PERM_PLAYBACK), "then everything: %s", why);
    text = slurp(users);
    CHECK(text && strstr(text, "password_enc: \"enc:v1:") && !strstr(text, "netip_password") &&
              !strstr(text, "digest") && !strstr(text, "older vigild"), "sealed, the hashes gone:\n%s",
          text ? text : "");
    free(text);

    /* The system account in an older file: its hashes, for this shadow
     * entry, until it logs in. */
    char id[33], ha1_root[33];
    CHECK(auth_shadow_id(shadow, "root", id, why, sizeof why) == 0, "shadow id");
    snprintf(buf, sizeof buf, "root:%s:secret", USERS_REALM);
    md5_hex(buf, strlen(buf), ha1_root);
    f = fopen(users, "w");
    fprintf(f, "system:\n  netip_password: \"%s\"\n  digest: \"%s\"\n  shadow: \"%s\"\n", xm_sec, ha1_root, id);
    fclose(f);
    users_reload();
    CHECK(users_login_netip("root", xm_sec, &perms, why, sizeof why) == 1 && digest_is("root", "secret", PERM_ALL),
          "root's old hashes: %s", why);
    CHECK(users_login_with("root", pass_is, "secret", &perms, why, sizeof why) == -1, "not ONVIF: %s", why);
    CHECK(users_login("root", "secret", &perms, why, sizeof why) == 1 &&
              users_login_with("root", pass_is, "secret", &perms, why, sizeof why) == 1, "after its login, ONVIF too");
    text = slurp(users);
    CHECK(text && strstr(text, "system:\n") && strstr(text, "  password_enc: \"enc:v1:") && !strstr(text, "digest"),
          "root's sealed:\n%s", text ? text : "");
    free(text);

    /* The shadow file changed (passwd, even to the same password): root's
     * sealed password is not used. */
    f = fopen(shadow, "w");
    fprintf(f, "root:" MD5_SECRET ":19000:0:99999:7:::\nnobody:*:19000:0:99999:7:::\n");
    fclose(f);
    CHECK(users_digest("root", ha1, &perms) == -1 && users_login_netip("root", xm_sec, &perms, why, sizeof why) == -1 &&
              users_login_with("root", pass_is, "secret", &perms, why, sizeof why) == -1 && strstr(why, "changed"),
          "root's unknown with a new shadow file: %s", why);
    CHECK(users_login("root", "secret", &perms, why, sizeof why) == 1 && digest_is("root", "secret", PERM_ALL),
          "and known again at its login");

    /* Set through vigil: the shadow file follows (mode, the other lines
     * kept), and every protocol has it at once, root and admin alike. */
    chmod(shadow, 0640);
    CHECK(users_set_system_password("abc", &e) == USERS_EINVAL, "too short for root too");
    CHECK(users_set_system_password("N3w-root", &e) == USERS_OK, "root's password set: %s", e.msg);
    text = slurp(shadow);
    char *rl = text ? strstr(text, "root:$6$") : NULL;
    CHECK(rl && strstr(text, "nobody:*:19000:0:99999:7:::\n") && strstr(rl, ":0:99999:7:::\n") &&
              !strstr(text, SHA512_SECRET), "the shadow file: a new SHA-512 hash, the rest kept:\n%s", text ? text : "");
    free(text);
    CHECK(stat(shadow, &st) == 0 && (st.st_mode & 0777) == 0640, "its mode kept (%o)", st.st_mode & 0777);
    CHECK(users_login("root", "secret", &perms, why, sizeof why) == 0 &&
              users_login("root", "N3w-root", &perms, why, sizeof why) == 1, "root's new password in the shadow file");
    md5_sofia("N3w-root", xm);
    CHECK(digest_is("root", "N3w-root", PERM_ALL) && digest_is("admin", "N3w-root", PERM_ALL) &&
              users_login_netip("root", xm, &perms, why, sizeof why) == 1 &&
              users_login_netip("admin", xm, &perms, why, sizeof why) == 1 &&
              users_login_with("root", pass_is, "N3w-root", &perms, why, sizeof why) == 1,
          "RTSP, NETIP and ONVIF at once: %s", why);
    CHECK(users_is_system("root") && users_is_system("admin") && !users_is_system("fay"), "who is the system account");
    char link[160];
    snprintf(link, sizeof link, "%s/shadow-link", dir);
    if (symlink(shadow, link)) {}
    f = fopen(cfg, "w");
    fprintf(f, "api:\n  user: root\n  shadow: %s\n  users: %s\n", link, users);
    fclose(f);
    CHECK(config_init(cfg) == 0, "configuration, shadow through a link");
    CHECK(users_set_system_password("N3w-root2", &e) == USERS_OK && lstat(link, &st) == 0 && S_ISLNK(st.st_mode) &&
              users_login("root", "N3w-root2", &perms, why, sizeof why) == 1,
          "a link to the shadow file stays a link: %s", e.msg);
    f = fopen(cfg, "w");
    fprintf(f, "api:\n  user: nosuch\n  shadow: %s\n  users: %s\n", shadow, users);
    fclose(f);
    config_init(cfg);
    CHECK(users_set_system_password("N3w-root3", &e) == USERS_EIO && strstr(e.msg, "no account"),
          "an account not in the shadow file: %s", e.msg);
    f = fopen(cfg, "w");
    fprintf(f, "api:\n  user: root\n  shadow: %s\n  users: %s\n", shadow, users);
    fclose(f);
    config_init(cfg);

    /* The key lost (deleted, the file copied to another board): the
     * passwords are unknown, nothing crashes, a login seals them again. */
    char key[160], key2[170];
    CHECK(users_add("hal", NULL, "secret", 1, &e) == USERS_OK && digest_is("hal", "secret", PERM_LIVE | PERM_PLAYBACK),
          "add hal");
    secret_key_path(key, sizeof key);
    snprintf(key2, sizeof key2, "%s.new", key);
    secret_set_key_path(key2);
    CHECK(users_digest("hal", ha1, &perms) == -1 && users_login_netip("hal", xm_sec, &perms, why, sizeof why) == -1 &&
              strstr(why, "cannot be read"), "hal's password unknown without its key: %s", why);
    CHECK(users_digest("root", ha1, &perms) == -1, "and root's");
    CHECK(users_login("hal", "secret", &perms, why, sizeof why) == 1 && digest_is("hal", "secret", PERM_LIVE | PERM_PLAYBACK),
          "and sealed again with the new key at his login");
    secret_set_key_path(key);
    text = slurp(users);
    CHECK(text && !strstr(text, "secret") && !strstr(text, "N3w-root") && !strstr(text, "annapw"),
          "no password in the clear in the users file");
    free(text);

    /* An invalid file: only root, and nothing is saved over it. */
    f = fopen(users, "w");
    fprintf(f, "users:\n  - name: Bad Name\n");
    fclose(f);
    CHECK(users_reload() < 0, "invalid file");
    CHECK(users_login("anna", "annapw", &perms, why, sizeof why) == 0, "no users from an invalid file");
    CHECK(users_login("root", "N3w-root2", &perms, why, sizeof why) == 1, "root still");
    CHECK(users_add("eve", NULL, "evepw1", 1, &e) == USERS_EIO, "not saved over an invalid file");

    char cmd[160];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd)) {}
}

static void test_rtsp_server_paths(void)
{
    static const struct { const char *url; int ch, sub; } OK[] = {
        { "rtsp://nvr:554/ch1/main", 0, 0 },
        { "rtsp://nvr/ch1/sub", 0, 1 },
        { "rtsp://nvr/ch16/main/", 15, 0 },
        { "rtsp://nvr/ch2", 1, 0 },
        { "rtsp://nvr/ch3/sub/trackID=0", 2, 1 },
        { "/ch4/main", 3, 0 },
        { "rtsp://nvr/Streaming/Channels/101", 0, 0 },
        { "rtsp://nvr/Streaming/Channels/1602?transportmode=unicast", 15, 1 },
        { "rtsp://nvr/streaming/channels/302/trackID=0", 2, 1 },
        { "rtsp://nvr/ISAPI/Streaming/Channels/201", 1, 0 },
        { "rtsp://nvr/h264/ch5/sub/av_stream", 4, 1 },
        { "rtsp://nvr/cam/realmonitor?channel=2&subtype=1", 1, 1 },
        { "rtsp://nvr/cam/realmonitor?channel=7&subtype=0/trackID=0", 6, 0 },
        { "rtsp://nvr/cam/realmonitor?channel=3", 2, 0 },
    };
    for (size_t i = 0; i < sizeof OK / sizeof OK[0]; ++i) {
        int ch = -1, sub = -1;
        CHECK(rtsp_server_path(OK[i].url, &ch, &sub) == 0 && ch == OK[i].ch && sub == OK[i].sub,
              "%s -> ch %d sub %d (%d, %d)", OK[i].url, OK[i].ch, OK[i].sub, ch, sub);
    }
    static const char *const BAD[] = {
        "rtsp://nvr/", "rtsp://nvr/ch0/main", "rtsp://nvr/ch17/main", "rtsp://nvr/ch1/third",
        "rtsp://nvr/Streaming/Channels/103", "rtsp://nvr/Streaming/Channels/1", "rtsp://nvr/Streaming/Channels/x01",
        "rtsp://nvr/cam/realmonitor?channel=1&subtype=2", "rtsp://nvr/cam/realmonitor?channel=99",
        "rtsp://nvr/ch1/main/extra", "rtsp://nvr", "rtsp://nvr/../etc/passwd",
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; ++i) {
        int ch, sub;
        CHECK(rtsp_server_path(BAD[i], &ch, &sub) < 0, "%s refused", BAD[i]);
    }
}

static void test_rtsp_server_sdp(void)
{
    stream_tap_ps_t ps = { .codec = VG_CODEC_H264, .n = 2 };
    static const uint8_t sps[] = { 0x67, 0x64, 0x00, 0x1f, 0xac }, pps[] = { 0x68, 0xeb, 0xcc };
    memcpy(ps.ps[0], sps, sizeof sps);
    ps.len[0] = sizeof sps;
    memcpy(ps.ps[1], pps, sizeof pps);
    ps.len[1] = sizeof pps;
    char sdp[2048];
    int n = rtsp_server_sdp(sdp, sizeof sdp, VG_CODEC_H264, &ps, "10.0.0.5", "CAM1 main", 42);
    CHECK(n > 0 && strstr(sdp, "o=- 42 1 IN IP4 10.0.0.5\r\n") && strstr(sdp, "m=video 0 RTP/AVP 96\r\n") &&
              strstr(sdp, "a=rtpmap:96 H264/90000\r\n") &&
              strstr(sdp, "a=fmtp:96 packetization-mode=1;profile-level-id=64001F;sprop-parameter-sets=Z2QAH6w=,aOvM\r\n") &&
              strstr(sdp, "a=control:trackID=0\r\n"), "H.264 SDP:\n%s", sdp);
    static const uint8_t vps[] = { 0x40, 0x01, 0x0c }, hsps[] = { 0x42, 0x01, 0x01 }, hpps[] = { 0x44, 0x01, 0xc1 };
    stream_tap_ps_t h = { .codec = VG_CODEC_H265, .n = 3 };
    memcpy(h.ps[0], vps, 3);
    memcpy(h.ps[1], hsps, 3);
    memcpy(h.ps[2], hpps, 3);
    h.len[0] = h.len[1] = h.len[2] = 3;
    n = rtsp_server_sdp(sdp, sizeof sdp, VG_CODEC_H265, &h, "10.0.0.5", "CAM2 sub", 1);
    CHECK(n > 0 && strstr(sdp, "a=rtpmap:96 H265/90000\r\n") &&
              strstr(sdp, "a=fmtp:96 sprop-vps=QAEM;sprop-sps=QgEB;sprop-pps=RAHB\r\n"), "H.265 SDP:\n%s", sdp);
    stream_tap_ps_t none = { 0 };
    n = rtsp_server_sdp(sdp, sizeof sdp, VG_CODEC_H264, &none, "10.0.0.5", "CAM1 main", 1);
    CHECK(n > 0 && strstr(sdp, "a=fmtp:96 packetization-mode=1\r\n") && !strstr(sdp, "sprop"),
          "no parameter sets yet: in-band only");
    CHECK(rtsp_server_sdp(sdp, 64, VG_CODEC_H264, &ps, "10.0.0.5", "CAM1 main", 1) < 0, "too small");
}

static void test_rtsp_server_digest(void)
{
    /* The server's check from HA1 against the client's from the password. */
    char ha1[33], buf[128], srv[33], cli[33];
    snprintf(buf, sizeof buf, "anna:%s:s3cret", USERS_REALM);
    md5_hex(buf, strlen(buf), ha1);
    rtsp_server_digest(ha1, "abcdef", "DESCRIBE", "rtsp://nvr/ch1/main", NULL, NULL, NULL, srv);
    rtsp_digest_response("anna", USERS_REALM, "s3cret", "DESCRIBE", "rtsp://nvr/ch1/main", "abcdef", NULL, NULL,
                         NULL, cli);
    CHECK(!strcmp(srv, cli), "Digest without qop: %s %s", srv, cli);
    rtsp_server_digest(ha1, "abcdef", "SETUP", "rtsp://nvr/ch1/main/trackID=0", "00000001", "xyz", "auth", srv);
    rtsp_digest_response("anna", USERS_REALM, "s3cret", "SETUP", "rtsp://nvr/ch1/main/trackID=0", "abcdef",
                         "00000001", "xyz", "auth", cli);
    CHECK(!strcmp(srv, cli), "Digest with qop=auth: %s %s", srv, cli);
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
        { "PUT", "/api/v1/config/rtsp_server", PERM_USERS },
        { "GET", "/api/v1/config/cameras", PERM_CAMERAS },
        { "PUT", "/api/v1/config/record", PERM_STORAGE },
        { "PUT", "/api/v1/config/schedule", PERM_STORAGE },
        { "GET", "/api/v1/config/quota", PERM_STORAGE },
        { "PUT", "/api/v1/config/nas", PERM_STORAGE },
        { "PUT", "/api/v1/config/smtp", PERM_NETWORK },
        { "PUT", "/api/v1/config/ddns", PERM_NETWORK },
        { "PUT", "/api/v1/config/time", PERM_SYSTEM },
        { "PUT", "/api/v1/config/ui", PERM_SYSTEM },
        { "PUT", "/api/v1/config/live", PERM_LIVE },
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
    test_rtsp_server_paths();
    test_rtsp_server_sdp();
    test_rtsp_server_digest();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
