/* SPDX-License-Identifier: GPL-3.0-or-later */
/* seg_writer.c — record an elementary stream into segments (see seg_writer.h). */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "seg_writer.h"

/* Segments currently open, across all writers of the process: the space
 * guard never deletes one of these, however long it has been idle. */
#define OPEN_MAX 64
static char            g_open[OPEN_MAX][320];
static pthread_mutex_t g_open_lock = PTHREAD_MUTEX_INITIALIZER;

static void open_add(const char *path)
{
    pthread_mutex_lock(&g_open_lock);
    for (int i = 0; i < OPEN_MAX; ++i)
        if (!g_open[i][0]) { snprintf(g_open[i], sizeof g_open[i], "%s", path); break; }
    pthread_mutex_unlock(&g_open_lock);
}

static void open_del(const char *path)
{
    pthread_mutex_lock(&g_open_lock);
    for (int i = 0; i < OPEN_MAX; ++i)
        if (g_open[i][0] && strcmp(g_open[i], path) == 0) { g_open[i][0] = 0; break; }
    pthread_mutex_unlock(&g_open_lock);
}

static int is_open(const char *path)
{
    int found = 0;
    pthread_mutex_lock(&g_open_lock);
    for (int i = 0; i < OPEN_MAX && !found; ++i) found = g_open[i][0] && strcmp(g_open[i], path) == 0;
    pthread_mutex_unlock(&g_open_lock);
    return found;
}

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

/* The newest segment name (the open one while w->f is set). */
static const char *last_name(const seg_writer_t *w)
{
    return w->names[(w->head + SEG_KEEP_MAX - 1) % SEG_KEEP_MAX];
}

const char *seg_current(const seg_writer_t *w)
{
    return w->f ? last_name(w) : "";
}

int seg_close(seg_writer_t *w)
{
    if (!w->f) return 0;
    /* fclose flushes the stdio buffer: its error is the segment's tail. */
    int rc = fclose(w->f) == 0 ? 0 : -1;
    w->f = NULL;
    open_del(last_name(w));
    if (rc < 0) w->failed = 1;
    return rc;
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
    if (!is_open(w->names[oldest])) unlink(w->names[oldest]);
    --w->n_names;
    return 0;
}

/* Low on space: delete the oldest finished recording in the directory,
 * whichever camera made it (CAM*_*.h26x not open in this process).
 * Returns 0 if one was deleted. */
static int drop_oldest_in_dir(const seg_writer_t *w)
{
    DIR *d = opendir(w->dir);
    if (!d) return -1;
    char oldest[sizeof w->dir + 256 + 2] = "";
    time_t oldest_t = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name, *dot = strrchr(n, '.');
        if (strncmp(n, "CAM", 3) != 0 || !dot || strncmp(dot, ".h26", 4) != 0) continue;
        char path[sizeof oldest];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", w->dir, n);
        if (stat(path, &st) < 0 || !S_ISREG(st.st_mode) || is_open(path)) continue;
        if (!oldest[0] || st.st_mtime < oldest_t) {
            snprintf(oldest, sizeof oldest, "%s", path);
            oldest_t = st.st_mtime;
        }
    }
    closedir(d);
    return oldest[0] ? unlink(oldest) : -1;
}

/* Keep the reserve free. Checked once a second, and at most one file is
 * deleted per check: if deleting does not bring the space back (a file
 * still held by a reader, a disk filled by something else), the guard
 * must not go on to wipe every recording in one pass. */
static void guard_space(seg_writer_t *w)
{
    time_t now = time(NULL);
    if (now == w->space_checked) return;
    w->space_checked = now;
    uint64_t before = free_bytes(w);
    if (before >= w->reserve) return;
    if (drop_oldest_in_dir(w) == 0 && free_bytes(w) > before) return;
    /* Nothing finished to delete, or deleting did not help: roll the open
     * segment over so it becomes deletable at the next check. */
    seg_close(w);
}

static int seg_open(seg_writer_t *w)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tm);

    /* Retention: drop the oldest of our segments before adding one. (Low
     * space is guard_space's job, which runs before every write.) */
    if (w->keep && w->n_names >= w->keep) drop_oldest(w);

    /* A unique name: a segment closed and reopened within the same second
     * (codec change, roll-over) must not truncate the previous one. */
    char name[sizeof w->names[0]];
    int fd = -1;
    for (int n = 0; n < 100 && fd < 0; ++n) {
        if (n == 0) snprintf(name, sizeof name, "%s/%s_%s.%s", w->dir, w->prefix, stamp, w->ext);
        else        snprintf(name, sizeof name, "%s/%s_%s_%d.%s", w->dir, w->prefix, stamp, n, w->ext);
        fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0 && errno != EEXIST) break;
    }
    if (fd < 0 || !(w->f = fdopen(fd, "wb"))) {
        if (fd >= 0) close(fd);
        w->failed = 1;
        return -1;
    }
    snprintf(w->names[w->head], sizeof w->names[0], "%s", name);
    open_add(name);
    w->head = (w->head + 1) % SEG_KEEP_MAX;
    if (w->n_names < SEG_KEEP_MAX) ++w->n_names;
    w->seg_start = now;
    w->failed = 0;
    return 0;
}

int seg_write(seg_writer_t *w, const uint8_t *data, size_t len, int key)
{
    int rc = 0;
    guard_space(w);
    if (key && (!w->f || time(NULL) - w->seg_start >= w->seg_secs)) {
        if (seg_close(w) < 0) rc = -1;               /* the old tail is lost; go on */
        if (seg_open(w) < 0) return -1;
    }
    if (!w->f) return rc;                            /* waiting for the first keyframe */
    if (fwrite(data, 1, len, w->f) != len) {
        w->failed = 1;
        seg_close(w);
        return -1;
    }
    w->bytes += len;
    return rc;
}
