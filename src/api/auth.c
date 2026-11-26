/* SPDX-License-Identifier: GPL-3.0-or-later */
/* auth.c — HTTP Basic credentials against a shadow file (see auth.h). */
#define _GNU_SOURCE                     /* crypt() in <unistd.h> where there is no <crypt.h> */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef VIGIL_HAVE_CRYPT_H               /* set by CMake: glibc/libxcrypt and musl have it, uClibc not */
#include <crypt.h>
#endif

#include "auth.h"

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
