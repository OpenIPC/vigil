/* SPDX-License-Identifier: GPL-3.0-or-later */
/* snap_store.c — the snapshots kept (snap_store.h). */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "rec_place.h"
#include "snap_store.h"

int snap_dir(const char *snap_path, const char *rec_path, int allow_rootfs, char *out, size_t cap, char *why,
             size_t wcap)
{
    char cand[2][256], w[160] = "";
    int n = 0;
    if (snap_path && snap_path[0]) {
        snprintf(cand[n++], sizeof cand[0], "%s", snap_path);
    } else if (rec_path && rec_path[0] == '/') {
        char parent[224];
        snprintf(parent, sizeof parent, "%s", rec_path);
        size_t l = strlen(parent);
        while (l > 1 && parent[l - 1] == '/') parent[--l] = 0;
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) {
            *slash = 0;
            snprintf(cand[n++], sizeof cand[0], "%s/snapshots", parent);
        }
        snprintf(cand[n++], sizeof cand[0], "%.*s/snapshots", (int)l, rec_path);
    } else {
        snprintf(why, wcap, "no snapshot directory: neither snapshot.path nor record.path is set");
        return -1;
    }
    rec_place_t first = REC_PLACE_OK;
    for (int i = 0; i < n; ++i) {
        rec_place_t p = rec_place_check(cand[i], allow_rootfs, NULL, w, sizeof w);
        if (p == REC_PLACE_OK) {
            snprintf(out, cap, "%s", cand[i]);
            return 0;
        }
        if (i == 0) {
            first = p;
            snprintf(why, wcap, "%s: %s (%s)", cand[i], rec_place_name(p), w);
        }
    }
    (void)first;
    return -1;
}

static int kind_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

int snap_name(char *out, size_t cap, int ch, time_t t, int ms, const char *kind)
{
    struct tm tm;
    if (!localtime_r(&t, &tm) || ch < 1 || ch > 99) return -1;
    char k[SNAP_KIND_MAX];
    size_t i = 0;
    for (; kind && kind[i] && i + 1 < sizeof k; ++i) k[i] = kind_char((unsigned char)kind[i]) ? kind[i] : '_';
    k[i] = 0;
    if (!k[0]) snprintf(k, sizeof k, "manual");
    int n = snprintf(out, cap, "ch%02d_%04d%02d%02d-%02d%02d%02d-%03d_%s.jpg", ch, tm.tm_year + 1900, tm.tm_mon + 1,
                     tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms < 0 ? 0 : ms % 1000, k);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

int snap_name_parse(const char *name, snap_file_t *f)
{
    memset(f, 0, sizeof *f);
    size_t len = strlen(name);
    if (len >= SNAP_NAME_MAX || len < 30 || strcmp(name + len - 4, ".jpg") != 0) return -1;
    int ch, Y, M, D, h, m, s, ms, used = 0;
    if (sscanf(name, "ch%2d_%4d%2d%2d-%2d%2d%2d-%3d_%n", &ch, &Y, &M, &D, &h, &m, &s, &ms, &used) != 8 || used != 25)
        return -1;
    /* every character where sscanf let anything through */
    static const char shape[] = "chDD_DDDDDDDD-DDDDDD-DDD_";
    for (int i = 0; i < 25; ++i)
        if (shape[i] == 'D' ? (name[i] < '0' || name[i] > '9') : name[i] != shape[i]) return -1;
    size_t kl = len - 4 - 25;
    if (!kl || kl >= SNAP_KIND_MAX) return -1;
    for (size_t i = 0; i < kl; ++i)
        if (!kind_char((unsigned char)name[25 + i])) return -1;
    if (ch < 1 || M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || m > 59 || s > 60) return -1;
    struct tm tm = { .tm_year = Y - 1900, .tm_mon = M - 1, .tm_mday = D, .tm_hour = h, .tm_min = m, .tm_sec = s,
                     .tm_isdst = -1 };
    snprintf(f->name, sizeof f->name, "%s", name);
    f->ch = ch;
    f->time = mktime(&tm);
    f->ms = ms;
    memcpy(f->kind, name + 25, kl);
    f->kind[kl] = 0;
    return 0;
}

static int mkdir_p(const char *path)
{
    char tmp[256];
    if (snprintf(tmp, sizeof tmp, "%s", path) >= (int)sizeof tmp) return -1;
    for (char *p = tmp + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    return mkdir(tmp, 0755) < 0 && errno != EEXIST ? -1 : 0;
}

int snap_save(const char *dir, const char *name, const void *p, size_t len, char *err, size_t cap)
{
    snap_file_t f;
    if (snap_name_parse(name, &f) < 0) {
        snprintf(err, cap, "not a snapshot's name");
        return -1;
    }
    if (mkdir_p(dir) < 0) {
        snprintf(err, cap, "cannot create %s: %s", dir, strerror(errno));
        return -1;
    }
    char path[512], tmp[520];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    snprintf(tmp, sizeof tmp, "%s/.%s.tmp", dir, name);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        snprintf(err, cap, "cannot write %s: %s", dir, strerror(errno));
        return -1;
    }
    const unsigned char *b = p;
    for (size_t done = 0; done < len;) {
        ssize_t n = write(fd, b + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            snprintf(err, cap, "cannot write %s: %s", dir, n < 0 ? strerror(errno) : "disk full");
            close(fd);
            unlink(tmp);
            return -1;
        }
        done += (size_t)n;
    }
    if (close(fd) < 0 || rename(tmp, path) < 0) {
        snprintf(err, cap, "cannot write %s: %s", path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int newest_first(const void *a, const void *b)
{
    const snap_file_t *x = a, *y = b;
    return strcmp(y->name + 5, x->name + 5) ? strcmp(y->name + 5, x->name + 5) : strcmp(y->name, x->name);
}

/* Every snapshot in `dir` (malloc'd), newest first: the count, or -1. */
static int scan(const char *dir, snap_file_t **out, uint64_t *total)
{
    *out = NULL;
    if (total) *total = 0;
    DIR *d = opendir(dir);
    if (!d) return errno == ENOENT ? 0 : -1;
    int n = 0, cap = 0;
    snap_file_t *v = NULL;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        snap_file_t f;
        if (snap_name_parse(e->d_name, &f) < 0) continue;
        char path[512];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (stat(path, &st) < 0 || !S_ISREG(st.st_mode)) continue;
        f.size = (uint64_t)st.st_size;
        if (n == cap) {
            int nc = cap ? cap * 2 : 64;
            snap_file_t *nv = realloc(v, sizeof *v * (size_t)nc);
            if (!nv) break;
            v = nv;
            cap = nc;
        }
        v[n++] = f;
        if (total) *total += f.size;
    }
    closedir(d);
    /* By the time in the name (all channels' names sort alike after "chNN_") */
    if (n > 1) qsort(v, (size_t)n, sizeof *v, newest_first);
    *out = v;
    return n;
}

int snap_list(const char *dir, snap_file_t *out, int max, uint64_t *total)
{
    snap_file_t *v;
    int n = scan(dir, &v, total);
    if (n < 0) return -1;
    for (int i = 0; out && i < n && i < max; ++i) out[i] = v[i];
    free(v);
    return n;
}

int snap_delete(const char *dir, const char *name)
{
    snap_file_t f;
    if (snap_name_parse(name, &f) < 0) {
        errno = EINVAL;
        return -1;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    return unlink(path);
}

int snap_trim(const char *dir, uint64_t max_bytes)
{
    if (!max_bytes) return 0;
    snap_file_t *v;
    uint64_t total;
    int n = scan(dir, &v, &total), removed = 0;
    for (int i = n - 1; i >= 0 && total > max_bytes; --i) {
        if (snap_delete(dir, v[i].name) == 0) ++removed;
        total -= v[i].size;
    }
    free(v);
    return removed;
}
