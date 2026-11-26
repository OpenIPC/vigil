/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_api.c — unit tests for the API's password check (src/api/auth.c). */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "auth.h"

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

int main(void)
{
    test_base64();
    test_basic();
    test_equal();
    test_shadow();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
