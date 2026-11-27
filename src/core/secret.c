/* SPDX-License-Identifier: GPL-3.0-or-later */
/* secret.c — the credential store's cipher (see secret.h). */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <mbedtls/base64.h>
#include <mbedtls/gcm.h>

#include "config.h"
#include "log.h"
#include "secret.h"

#define NONCE_LEN 12
#define TAG_LEN   16
#define PLAIN_MAX 1024                  /* the longest value sealed (a camera URL's password) */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char            g_path[256];     /* "": the default */
static unsigned char   g_key[SECRET_KEY_LEN];
static int             g_have;          /* g_key holds the key */
static secret_rc_t     g_why = SECRET_OK; /* why not, once logged (not logged again) */

void secret_wipe(void *p, size_t n)
{
    volatile unsigned char *w = p;
    while (n--) *w++ = 0;
}

static void default_path(char *out, size_t cap)
{
    char dir[256];
    snprintf(dir, sizeof dir, "%s", config_path());
    snprintf(out, cap, "%s/%s", dirname(dir), SECRET_FILE);
}

static void path_locked(char *out, size_t cap)
{
    if (g_path[0]) snprintf(out, cap, "%s", g_path);
    else default_path(out, cap);
}

void secret_set_key_path(const char *path)
{
    pthread_mutex_lock(&g_lock);
    snprintf(g_path, sizeof g_path, "%s", path ? path : "");
    secret_wipe(g_key, sizeof g_key);
    g_have = 0;
    g_why = SECRET_OK;
    pthread_mutex_unlock(&g_lock);
}

const char *secret_key_path(char *out, size_t cap)
{
    pthread_mutex_lock(&g_lock);
    path_locked(out, cap);
    pthread_mutex_unlock(&g_lock);
    return out;
}

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* The key file is 64 hex digits and a newline: readable with cat, and a
 * truncated or edited one is told apart from a key. */
static secret_rc_t read_key(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? SECRET_ENOKEY : SECRET_EBADKEY;
    char text[2 * SECRET_KEY_LEN + 4];
    ssize_t n = read(fd, text, sizeof text);
    struct stat st;
    int mode_ok = fstat(fd, &st) == 0 && !(st.st_mode & 077);
    close(fd);
    secret_rc_t rc = SECRET_EBADKEY;
    if (n >= 2 * SECRET_KEY_LEN && (n == 2 * SECRET_KEY_LEN || text[2 * SECRET_KEY_LEN] == '\n') &&
        n <= 2 * SECRET_KEY_LEN + 1) {
        rc = SECRET_OK;
        for (int i = 0; i < SECRET_KEY_LEN && rc == SECRET_OK; ++i) {
            int hi = hexval((unsigned char)text[2 * i]), lo = hexval((unsigned char)text[2 * i + 1]);
            if (hi < 0 || lo < 0) rc = SECRET_EBADKEY;
            else g_key[i] = (unsigned char)(hi << 4 | lo);
        }
    }
    secret_wipe(text, sizeof text);
    if (rc != SECRET_OK) {
        secret_wipe(g_key, sizeof g_key);
        return rc;
    }
    if (!mode_ok) vg_log("%s: readable by others than its owner; chmod 600 it", path);
    g_have = 1;
    return SECRET_OK;
}

static int random_bytes(void *buf, size_t len)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t done = 0;
    while (done < len) {
        ssize_t n = read(fd, (char *)buf + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += (size_t)n;
    }
    close(fd);
    return done == len ? 0 : -1;
}

/* A new key at `path`, only where there is no file: an unreadable one is
 * someone's key, never replaced. */
static secret_rc_t make_key(const char *path)
{
    unsigned char key[SECRET_KEY_LEN];
    char text[2 * SECRET_KEY_LEN + 2], err[300];
    if (random_bytes(key, sizeof key) < 0) {
        vg_log("%s: no randomness (/dev/urandom) for a key", path);
        return SECRET_ENOKEY;
    }
    for (int i = 0; i < SECRET_KEY_LEN; ++i) snprintf(text + 2 * i, 3, "%02x", key[i]);
    text[2 * SECRET_KEY_LEN] = '\n';
    text[2 * SECRET_KEY_LEN + 1] = 0;
    int rc = config_write_atomic(path, text, 2 * SECRET_KEY_LEN + 1, err, sizeof err);
    secret_wipe(text, sizeof text);
    if (rc < 0) {
        secret_wipe(key, sizeof key);
        vg_log("cannot make the key for vigil's passwords: %s", err);
        return SECRET_ENOKEY;
    }
    memcpy(g_key, key, sizeof key);
    secret_wipe(key, sizeof key);
    g_have = 1;
    vg_log("%s: a new key for the passwords vigil keeps (keep it with the board, not with its backups)", path);
    return SECRET_OK;
}

/* Under g_lock: the key in g_key, read or (`create`) made. */
static secret_rc_t key_locked(int create)
{
    if (g_have) return SECRET_OK;
    char path[300];
    path_locked(path, sizeof path);
    secret_rc_t rc = read_key(path);
    if (rc == SECRET_ENOKEY && create) rc = make_key(path);
    if (rc != SECRET_OK && rc != g_why) {
        g_why = rc;
        vg_log("%s: %s", path, rc == SECRET_ENOKEY ? "no key: the passwords sealed with one cannot be read"
                                                   : "not a key vigil can use (64 hex digits): left as it is, "
                                                     "and the passwords sealed with it cannot be read");
    }
    return rc;
}

secret_rc_t secret_ensure_key(void)
{
    pthread_mutex_lock(&g_lock);
    secret_rc_t rc = key_locked(1);
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int secret_is_sealed(const char *v)
{
    return v && !strncmp(v, SECRET_PREFIX, sizeof SECRET_PREFIX - 1);
}

secret_rc_t secret_seal(const char *aad, const char *plain, char *out, size_t cap)
{
    if (cap) out[0] = 0;
    size_t len = strlen(plain);
    if (len > PLAIN_MAX || cap < SECRET_SEALED_SIZE(len)) return SECRET_EFORMAT;
    unsigned char key[SECRET_KEY_LEN];
    pthread_mutex_lock(&g_lock);
    secret_rc_t rc = key_locked(1);
    if (rc == SECRET_OK) memcpy(key, g_key, sizeof key);
    pthread_mutex_unlock(&g_lock);
    if (rc != SECRET_OK) return rc;

    unsigned char buf[NONCE_LEN + PLAIN_MAX + TAG_LEN];
    if (random_bytes(buf, NONCE_LEN) < 0) {
        secret_wipe(key, sizeof key);
        return SECRET_ENOKEY;
    }
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int bad = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 8 * SECRET_KEY_LEN) ||
              mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, len, buf, NONCE_LEN, (const unsigned char *)aad,
                                        strlen(aad), (const unsigned char *)plain, buf + NONCE_LEN, TAG_LEN,
                                        buf + NONCE_LEN + len);
    mbedtls_gcm_free(&gcm);
    secret_wipe(key, sizeof key);
    size_t olen = 0, pre = sizeof SECRET_PREFIX - 1;
    if (!bad) {
        memcpy(out, SECRET_PREFIX, pre);
        bad = mbedtls_base64_encode((unsigned char *)out + pre, cap - pre, &olen, buf, NONCE_LEN + len + TAG_LEN);
    }
    secret_wipe(buf, sizeof buf);
    if (bad) {
        if (cap) out[0] = 0;
        return SECRET_EFORMAT;
    }
    out[pre + olen] = 0;
    return SECRET_OK;
}

secret_rc_t secret_open(const char *aad, const char *in, char *out, size_t cap)
{
    if (cap) out[0] = 0;
    if (!secret_is_sealed(in) || !cap) return SECRET_EFORMAT;
    const char *b64 = in + sizeof SECRET_PREFIX - 1;
    unsigned char buf[NONCE_LEN + PLAIN_MAX + TAG_LEN];
    size_t n = 0;
    if (mbedtls_base64_decode(buf, sizeof buf, &n, (const unsigned char *)b64, strlen(b64)) ||
        n < NONCE_LEN + TAG_LEN)
        return SECRET_EFORMAT;
    size_t len = n - NONCE_LEN - TAG_LEN;
    if (len >= cap) return SECRET_EFORMAT;
    unsigned char key[SECRET_KEY_LEN];
    pthread_mutex_lock(&g_lock);
    secret_rc_t rc = key_locked(0);
    if (rc == SECRET_OK) memcpy(key, g_key, sizeof key);
    pthread_mutex_unlock(&g_lock);
    if (rc != SECRET_OK) return rc;

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int bad = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 8 * SECRET_KEY_LEN);
    if (!bad)
        bad = mbedtls_gcm_auth_decrypt(&gcm, len, buf, NONCE_LEN, (const unsigned char *)aad, strlen(aad),
                                       buf + NONCE_LEN + len, TAG_LEN, buf + NONCE_LEN, (unsigned char *)out);
    mbedtls_gcm_free(&gcm);
    secret_wipe(key, sizeof key);
    secret_wipe(buf, sizeof buf);
    if (bad || memchr(out, 0, len)) {
        secret_wipe(out, len < cap ? len : cap);
        out[0] = 0;
        return SECRET_EAUTH;
    }
    out[len] = 0;
    return SECRET_OK;
}

const char *secret_strerror(secret_rc_t rc)
{
    switch (rc) {
    case SECRET_OK:      return "ok";
    case SECRET_ENOKEY:  return "no key (" SECRET_FILE ")";
    case SECRET_EBADKEY: return "the key file (" SECRET_FILE ") is unusable";
    case SECRET_EAUTH:   return "sealed with another key, or for another setting";
    case SECRET_EFORMAT: return "not a sealed value";
    case SECRET_ENOMEM:  return "out of memory";
    }
    return "?";
}
