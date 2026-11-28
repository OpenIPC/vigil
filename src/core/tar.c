/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tar.c — a ustar archive of a few small files (see tar.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tar.h"

#define BLOCK 512

static int reserve(tar_out_t *t, size_t more)
{
    if (t->len + more <= t->cap) return 0;
    size_t cap = t->cap ? t->cap : 16 * BLOCK;
    while (cap < t->len + more) cap *= 2;
    char *d = realloc(t->data, cap);
    if (!d) return -1;
    t->data = d;
    t->cap = cap;
    return 0;
}

static int name_ok(const char *name)
{
    size_t n = strlen(name);
    return n >= 1 && n < 100 && !strchr(name, '/') && strcmp(name, ".") && strcmp(name, "..");
}

static unsigned checksum(const unsigned char *h)
{
    unsigned sum = 0;
    for (int i = 0; i < BLOCK; ++i) sum += i >= 148 && i < 156 ? ' ' : h[i];
    return sum;
}

int tar_add(tar_out_t *t, const char *name, const void *data, size_t len, time_t mtime)
{
    if (!name_ok(name) || len > 077777777777ULL) return -1;
    size_t padded = (len + BLOCK - 1) / BLOCK * BLOCK;
    if (reserve(t, BLOCK + padded) < 0) return -1;
    unsigned char *h = (unsigned char *)t->data + t->len;
    memset(h, 0, BLOCK + padded);
    memcpy(h, name, strlen(name));
    snprintf((char *)h + 100, 8, "%07o", 0600);
    snprintf((char *)h + 108, 8, "%07o", 0);
    snprintf((char *)h + 116, 8, "%07o", 0);
    snprintf((char *)h + 124, 12, "%011llo", (unsigned long long)len & 077777777777ULL);
    unsigned long long mt = mtime > 0 ? (unsigned long long)mtime & 077777777777ULL : 0;
    snprintf((char *)h + 136, 12, "%011llo", mt);
    h[156] = '0';
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    memcpy(h + 265, "root", 4);
    memcpy(h + 297, "root", 4);
    snprintf((char *)h + 148, 8, "%06o", checksum(h));
    h[155] = ' ';
    if (len) memcpy(h + BLOCK, data, len);
    t->len += BLOCK + padded;
    return 0;
}

int tar_finish(tar_out_t *t)
{
    if (reserve(t, 2 * BLOCK) < 0) return -1;
    memset(t->data + t->len, 0, 2 * BLOCK);
    t->len += 2 * BLOCK;
    return 0;
}

void tar_out_free(tar_out_t *t)
{
    if (t->data) memset(t->data, 0, t->cap);
    free(t->data);
    memset(t, 0, sizeof *t);
}

/* An octal field of `n` bytes (NUL or space terminated): -1 when not one. */
static long long octal(const unsigned char *p, size_t n)
{
    long long v = 0;
    size_t i = 0;
    while (i < n && p[i] == ' ') ++i;
    if (i == n || p[i] < '0' || p[i] > '7') return -1;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; ++i) v = v * 8 + (p[i] - '0');
    for (; i < n; ++i)
        if (p[i] != 0 && p[i] != ' ') return -1;
    return v;
}

static int zero_block(const unsigned char *h)
{
    for (int i = 0; i < BLOCK; ++i)
        if (h[i]) return 0;
    return 1;
}

int tar_read(const void *buf, size_t len, tar_file_t *files, int max, char *err, size_t cap)
{
    const unsigned char *b = buf;
    size_t off = 0;
    int n = 0;
    while (off + BLOCK <= len) {
        const unsigned char *h = b + off;
        if (zero_block(h)) return n;
        long long sum = octal(h + 148, 8), size = octal(h + 124, 12);
        if (sum < 0 || (unsigned)sum != checksum(h)) {
            snprintf(err, cap, "not a tar archive (a header's checksum is wrong)");
            return -1;
        }
        if (memcmp(h + 257, "ustar", 5) != 0) {
            snprintf(err, cap, "not a ustar archive");
            return -1;
        }
        char name[101];
        memcpy(name, h, 100);
        name[100] = 0;
        if (!strncmp(name, "./", 2)) memmove(name, name + 2, strlen(name + 2) + 1);
        if (h[156] != '0' && h[156] != 0) {
            snprintf(err, cap, "%.60s: only plain files belong in a configuration export", name);
            return -1;
        }
        if (!name_ok(name) || h[345]) {
            snprintf(err, cap, "%.60s: a name this archive should not have", name);
            return -1;
        }
        off += BLOCK;
        if (size < 0 || (unsigned long long)size > len - off) {
            snprintf(err, cap, "%.60s: cut short", name);
            return -1;
        }
        if (tar_find(files, n, name)) {
            snprintf(err, cap, "%.60s: twice in the archive", name);
            return -1;
        }
        if (n >= max) {
            snprintf(err, cap, "more files than a configuration export has");
            return -1;
        }
        memcpy(files[n].name, name, strlen(name) + 1);     /* name_ok: under 100 */
        files[n].data = (const char *)b + off;
        files[n].len = (size_t)size;
        ++n;
        off += ((size_t)size + BLOCK - 1) / BLOCK * BLOCK;
    }
    if (off != len || !n) {
        snprintf(err, cap, n ? "the archive is cut short" : "not a tar archive");
        return -1;
    }
    return n;
}

const tar_file_t *tar_find(const tar_file_t *files, int n, const char *name)
{
    for (int i = 0; i < n; ++i)
        if (!strcmp(files[i].name, name)) return &files[i];
    return NULL;
}
