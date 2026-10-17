/* SPDX-License-Identifier: GPL-3.0-or-later */
/* seg_writer.c — record an elementary stream into segments (see seg_writer.h). */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "seg_writer.h"

static int mkdir_p(const char *path)
{
    char tmp[192];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    return (mkdir(tmp, 0755) < 0 && errno != EEXIST) ? -1 : 0;
}

int seg_init(seg_writer_t *w, const char *dir, const char *prefix,
             int seg_secs, int keep)
{
    memset(w, 0, sizeof *w);
    snprintf(w->dir, sizeof w->dir, "%s", dir);
    snprintf(w->prefix, sizeof w->prefix, "%s", prefix);
    snprintf(w->ext, sizeof w->ext, "es");
    w->seg_secs = seg_secs > 0 ? seg_secs : 60;
    w->keep = keep < 0 ? 0 : (keep > SEG_KEEP_MAX ? SEG_KEEP_MAX : keep);
    if (mkdir_p(dir) < 0) return -1;
    struct statvfs fs;
    w->reserve = 8ull << 20;
    if (statvfs(dir, &fs) == 0) {
        uint64_t tenth = (uint64_t)fs.f_blocks * fs.f_frsize / 10;
        if (tenth > w->reserve) w->reserve = tenth;
    }
    return 0;
}

void seg_set_ext(seg_writer_t *w, const char *ext)
{
    snprintf(w->ext, sizeof w->ext, "%s", ext);
}

void seg_close(seg_writer_t *w)
{
    if (w->f) fclose(w->f);
    w->f = NULL;
}

const char *seg_current(const seg_writer_t *w)
{
    return w->f ? w->names[(w->head + SEG_KEEP_MAX - 1) % SEG_KEEP_MAX] : "";
}

static uint64_t free_bytes(const seg_writer_t *w)
{
    struct statvfs fs;
    if (statvfs(w->dir, &fs) < 0) return UINT64_MAX;
    return (uint64_t)fs.f_bavail * fs.f_frsize;
}

/* Delete our oldest segment that is not the open one. Returns 0 if one
 * was deleted. */
static int drop_oldest(seg_writer_t *w)
{
    int closed = w->f ? 1 : 0;              /* the newest entry is open */
    if (w->n_names - closed <= 0) return -1;
    int oldest = (w->head + SEG_KEEP_MAX - w->n_names) % SEG_KEEP_MAX;
    unlink(w->names[oldest]);
    --w->n_names;
    return 0;
}

/* Low on space: delete the oldest finished recording in the directory,
 * whichever camera made it (CAM*_*.h26x, last written more than 2 s ago,
 * so not an open segment). Returns 0 if one was deleted. */
static int drop_oldest_in_dir(const seg_writer_t *w)
{
    DIR *d = opendir(w->dir);
    if (!d) return -1;
    char oldest[sizeof w->dir + 256 + 2] = "";
    time_t oldest_t = 0, now = time(NULL);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name, *dot = strrchr(n, '.');
        if (strncmp(n, "CAM", 3) != 0 || !dot || strncmp(dot, ".h26", 4) != 0) continue;
        char path[sizeof oldest];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", w->dir, n);
        if (stat(path, &st) < 0 || !S_ISREG(st.st_mode) || now - st.st_mtime < 2) continue;
        if (!oldest[0] || st.st_mtime < oldest_t) {
            snprintf(oldest, sizeof oldest, "%s", path);
            oldest_t = st.st_mtime;
        }
    }
    closedir(d);
    return oldest[0] ? unlink(oldest) : -1;
}

/* Keep the reserve free; checked once a second. */
static void guard_space(seg_writer_t *w)
{
    time_t now = time(NULL);
    if (now == w->space_checked) return;
    w->space_checked = now;
    while (free_bytes(w) < w->reserve) {
        if (drop_oldest_in_dir(w) == 0) continue;
        seg_close(w);                       /* nothing finished left: roll over */
        break;
    }
}

static int seg_open(seg_writer_t *w)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tm);

    /* Retention: drop the oldest of our segments before adding one. */
    if (w->keep && w->n_names >= w->keep) drop_oldest(w);
    if (free_bytes(w) < w->reserve) drop_oldest_in_dir(w);
    char name[sizeof w->names[0]];
    snprintf(name, sizeof name, "%s/%s_%s.%s", w->dir, w->prefix, stamp, w->ext);
    snprintf(w->names[w->head], sizeof w->names[0], "%s", name);
    w->f = fopen(name, "wb");
    if (!w->f) {
        w->failed = 1;
        return -1;
    }
    w->head = (w->head + 1) % SEG_KEEP_MAX;
    if (w->n_names < SEG_KEEP_MAX) ++w->n_names;
    w->seg_start = now;
    w->failed = 0;
    return 0;
}

int seg_write(seg_writer_t *w, const uint8_t *data, size_t len, int key)
{
    guard_space(w);
    if (key && (!w->f || time(NULL) - w->seg_start >= w->seg_secs)) {
        seg_close(w);
        if (seg_open(w) < 0) return -1;
    }
    if (!w->f) return 0;                /* waiting for the first keyframe */
    if (fwrite(data, 1, len, w->f) != len) {
        w->failed = 1;
        seg_close(w);
        return -1;
    }
    w->bytes += len;
    return 0;
}
