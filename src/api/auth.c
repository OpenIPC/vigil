/* SPDX-License-Identifier: GPL-3.0-or-later */
/* auth.c — HTTP Basic credentials against a shadow file (see auth.h). */
#define _GNU_SOURCE                     /* crypt() in <unistd.h> where there is no <crypt.h> */
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef VIGIL_HAVE_CRYPT_H               /* set by CMake: glibc/libxcrypt and musl have it, uClibc not */
#include <crypt.h>
#endif

#include "auth.h"
#include "md5.h"

int auth_base64_decode(const char *in, size_t len, unsigned char *out, size_t cap)
{
    unsigned acc = 0;
    int bits = 0;
    size_t n = 0;
    while (len && in[len - 1] == '=') --len;
    for (size_t i = 0; i < len; ++i) {
        int c = (unsigned char)in[i], v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else return -1;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n == cap) return -1;
            out[n++] = (unsigned char)(acc >> bits);
        }
    }
    if (bits >= 6) return -1;               /* one character too many */
    return (int)n;
}

int auth_parse_basic(const char *header, char *user, size_t ul, char *pass, size_t pl)
{
    if (!header || strncasecmp(header, "Basic ", 6) != 0) return -1;
    const char *b = header + 6;
    while (*b == ' ') ++b;
    size_t len = strcspn(b, " \t\r\n");
    unsigned char dec[384];
    int n = auth_base64_decode(b, len, dec, sizeof dec - 1);
    if (n < 0) return -1;
    dec[n] = 0;
    if (memchr(dec, 0, (size_t)n)) return -1;
    char *colon = strchr((char *)dec, ':');
    if (!colon) return -1;
    size_t u = (size_t)(colon - (char *)dec), p = strlen(colon + 1);
    int rc = -1;
    if (u < ul && p < pl) {
        memcpy(user, dec, u);
        user[u] = 0;
        memcpy(pass, colon + 1, p + 1);
        rc = 0;
    }
    volatile unsigned char *w = dec;        /* do not leave the password on the stack */
    for (size_t i = 0; i < sizeof dec; ++i) w[i] = 0;
    return rc;
}

int auth_equal(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned char d = la != lb;
    for (size_t i = 0; i < la; ++i) d |= (unsigned char)(a[i] ^ b[i % (lb ? lb : 1)]);
    return d == 0;
}

/* `user`'s hash from the shadow file; 0, or -1 with `why`. */
static int shadow_hash(const char *shadow, const char *user, char *hash, size_t cap,
                       char *why, size_t wcap)
{
    FILE *f = fopen(shadow, "r");
    if (!f) {
        snprintf(why, wcap, "cannot open %s: %s", shadow, strerror(errno));
        return -1;
    }
    char line[512];
    size_t ul = strlen(user);
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, user, ul) != 0 || line[ul] != ':') continue;
        const char *h = line + ul + 1;
        size_t hl = strcspn(h, ":\n");
        if (hl >= cap) break;
        memcpy(hash, h, hl);
        hash[hl] = 0;
        found = 1;
        break;
    }
    fclose(f);
    volatile char *w = line;
    for (size_t i = 0; i < sizeof line; ++i) w[i] = 0;
    if (!found) {
        snprintf(why, wcap, "no account '%s' in %s", user, shadow);
        return -1;
    }
    if (!hash[0]) {
        snprintf(why, wcap, "account '%s' has no password; set one (passwd) or turn api.auth off", user);
        return -1;
    }
    if (hash[0] == '!' || hash[0] == '*') {
        snprintf(why, wcap, "account '%s' is locked", user);
        return -1;
    }
    return 0;
}

/* crypt() keeps its result in a static buffer. */
static pthread_mutex_t g_crypt_lock = PTHREAD_MUTEX_INITIALIZER;

int auth_crypt(const char *password, const char *setting, char *out, size_t cap)
{
    pthread_mutex_lock(&g_crypt_lock);
    const char *r = crypt(password, setting);
    /* libxcrypt answers an unknown method with "*0"/"*1", others with NULL. */
    int rc = !r || r[0] == '*' || strlen(r) >= cap ? -1 : 0;
    if (rc == 0) memcpy(out, r, strlen(r) + 1);
    pthread_mutex_unlock(&g_crypt_lock);
    return rc;
}

int auth_shadow_id(const char *shadow, const char *user, char out[33], char *why, size_t cap)
{
    char hash[256];
    out[0] = 0;
    if (shadow_hash(shadow, user, hash, sizeof hash, why, cap) < 0) return -1;
    md5_hex(hash, strlen(hash), out);
    volatile char *w = hash;
    for (size_t i = 0; i < sizeof hash; ++i) w[i] = 0;
    return 0;
}

int auth_check_shadow(const char *shadow, const char *user, const char *password,
                      char *why, size_t cap)
{
    char hash[256], r[256];
    if (shadow_hash(shadow, user, hash, sizeof hash, why, cap) < 0) return -1;
    if (auth_crypt(password, hash, r, sizeof r) < 0) {
        snprintf(why, cap, "crypt() cannot check the hash of '%s' (method %.4s)", user, hash);
        return -1;
    }
    return auth_equal(r, hash);
}

#define SHADOW_MAX (64u << 10)

static int write_all(int fd, const char *p, size_t len)
{
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int auth_shadow_set(const char *shadow, const char *user, const char *hash, char *why, size_t cap)
{
    if (strchr(hash, ':') || strchr(hash, '\n') || !*user) {
        snprintf(why, cap, "not a password hash");
        return -1;
    }
    /* The file itself where /etc/shadow is a link to it: the rename must
     * replace the file, not the link. */
    char path[PATH_MAX];
    if (!realpath(shadow, path)) {
        snprintf(why, cap, "cannot open %s: %s", shadow, strerror(errno));
        return -1;
    }
    struct stat st;
    FILE *f = fopen(path, "r");
    if (!f || fstat(fileno(f), &st) < 0) {
        snprintf(why, cap, "cannot open %s: %s", path, strerror(errno));
        if (f) fclose(f);
        return -1;
    }
    char *in = malloc(SHADOW_MAX + 1), *out = malloc(SHADOW_MAX + 512);
    size_t n = in ? fread(in, 1, SHADOW_MAX + 1, f) : 0;
    int rd_err = ferror(f);
    fclose(f);
    int rc = -1, found = 0, full = 0;
    size_t o = 0, ul = strlen(user);
    if (!in || !out || rd_err || n > SHADOW_MAX) {
        snprintf(why, cap, "cannot read %s", path);
        goto done;
    }
    in[n] = 0;
    for (char *line = in; *line;) {
        char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) + 1 : strlen(line);
        if (!found && !strncmp(line, user, ul) && line[ul] == ':') {
            /* name:hash:lastchg:rest -- the rest as it was */
            char *h = line + ul + 1, *end = line + len - (nl ? 1 : 0);
            char *f3 = memchr(h, ':', (size_t)(end - h));
            char *rest = f3 ? memchr(f3 + 1, ':', (size_t)(end - f3 - 1)) : NULL;
            /* The day of the change; a board whose clock is not set yet
             * (1970) keeps the old one: 0 there means "change it at the
             * next login" to whatever reads the field. */
            char day[24];
            long today = (long)(time(NULL) / 86400);
            if (today > 18000) snprintf(day, sizeof day, "%ld", today);         /* 2019 */
            else snprintf(day, sizeof day, "%.*s", f3 ? (int)((rest ? rest : end) - f3 - 1) : 0, f3 ? f3 + 1 : "");
            int w = snprintf(out + o, SHADOW_MAX + 512 - o, "%s:%s:%s%.*s\n", user, hash, day,
                             rest ? (int)(end - rest) : 0, rest ? rest : "");
            if (w < 0 || (size_t)w >= SHADOW_MAX + 512 - o) {
                full = 1;
                break;
            }
            o += (size_t)w;
            found = 1;
        } else {
            if (o + len > SHADOW_MAX + 511) {
                full = 1;
                break;
            }
            memcpy(out + o, line, len);
            o += len;
        }
        line += len;
    }
    if (full || !found) {
        if (full) snprintf(why, cap, "%s is too large", path);
        else snprintf(why, cap, "no account '%s' in %s", user, path);
        goto done;
    }
    /* A new file beside it, as passwd does, then renamed over it: a power
     * cut leaves the old file or the new one, never half of one. */
    char tmp[PATH_MAX + 8], dir[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) {
        snprintf(why, cap, "cannot create a file next to %s: %s", path, strerror(errno));
        goto done;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int bad = fchmod(fd, st.st_mode & 07777) < 0 || (fchown(fd, st.st_uid, st.st_gid) < 0 && errno != EPERM) ||
              write_all(fd, out, o) < 0 || fsync(fd) < 0;
    int err = errno;
    if (close(fd) < 0 && !bad) {
        bad = 1;
        err = errno;
    }
    if (bad) {
        snprintf(why, cap, "cannot write %s: %s", tmp, strerror(err));
        unlink(tmp);
        goto done;
    }
    if (rename(tmp, path) < 0) {
        snprintf(why, cap, "cannot rename %s: %s", tmp, strerror(errno));
        unlink(tmp);
        goto done;
    }
    snprintf(dir, sizeof dir, "%s", path);
    int dfd = open(dirname(dir), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
    rc = 0;
done:
    if (in) {
        volatile char *w = in;
        for (size_t i = 0; i < n && i <= SHADOW_MAX; ++i) w[i] = 0;
    }
    if (out) {
        volatile char *w = out;
        for (size_t i = 0; i < o; ++i) w[i] = 0;
    }
    free(in);
    free(out);
    return rc;
}
