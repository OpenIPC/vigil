/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rec_place.c — may a recording go into this directory? (rec_place.h)
 *
 * Why st_dev for the root and mountinfo for the rest: comparing st_dev with
 * "/" needs no parsing, follows links and bind mounts by itself, and still
 * works where /proc is not mounted -- the root filesystem is the failure that
 * wears out a board's flash, so it is the check that must not depend on
 * anything else. (On OpenIPC "/" is an overlay of the squashfs and the jffs2
 * partition; overlayfs gives its directories the overlay's st_dev, so every
 * directory that is not on a mounted disk compares equal.) The type of the
 * filesystem and the read-only flag are only in mountinfo; there the path
 * is matched against mount points after realpath() of the part that exists,
 * so a link cannot make it match a mount it does not lead to. Matching by
 * major:minor instead would miss btrfs, whose st_dev is not the one
 * mountinfo lists.
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rec_place.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Filesystems that hold no disk (RAM, kernel views) or that are a board's
 * flash. A recording there fills RAM or wears the flash out. Anything else
 * -- vfat, exfat, ext4, xfs, btrfs, ntfs, nfs, cifs, fuse, ... -- is taken
 * for a disk: a list of what is allowed would refuse a filesystem nobody
 * thought of on a host. */
static const char *const NOT_DISK[] = {
    "tmpfs", "ramfs", "devtmpfs", "proc", "sysfs", "debugfs", "tracefs", "securityfs",
    "configfs", "pstore", "cgroup", "cgroup2", "devpts", "mqueue", "bpf", "efivarfs",
    "hugetlbfs", "autofs", "rootfs",
    "squashfs", "cramfs", "romfs", "jffs2", "ubifs", "yaffs", "yaffs2",
};

static int not_disk_type(const char *fs)
{
    for (size_t i = 0; i < sizeof NOT_DISK / sizeof NOT_DISK[0]; ++i)
        if (!strcmp(fs, NOT_DISK[i])) return 1;
    return 0;
}

/* mountinfo escapes space, tab, newline and backslash as \040 \011 \012 \134. */
static void unescape(char *s)
{
    char *o = s;
    for (char *p = s; *p; ++p) {
        if (p[0] == '\\' && p[1] >= '0' && p[1] <= '3' && p[2] >= '0' && p[2] <= '7' &&
            p[3] >= '0' && p[3] <= '7') {
            *o++ = (char)((p[1] - '0') * 64 + (p[2] - '0') * 8 + (p[3] - '0'));
            p += 3;
        } else {
            *o++ = *p;
        }
    }
    *o = 0;
}

static int has_opt(const char *opts, const char *opt)
{
    size_t n = strlen(opt);
    for (const char *p = opts; p && *p;) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !strncmp(p, opt, n)) return 1;
        p = e ? e + 1 : NULL;
    }
    return 0;
}

/* Is `mnt` the mount point of `path` or one of its parents? */
static int contains(const char *mnt, const char *path)
{
    size_t n = strlen(mnt);
    if (!strcmp(mnt, "/")) return 1;
    return !strncmp(path, mnt, n) && (path[n] == 0 || path[n] == '/');
}

typedef struct {
    char mnt[PATH_MAX];
    char fs[64];
    char src[256];
    int  ro;
    int  found;
} mount_t;

/* The longest mount point in `file` that contains `path`. -1 (errno set)
 * when the file cannot be read: then nothing is known about the mount. */
static int find_mount(const char *file, const char *path, mount_t *best)
{
    memset(best, 0, sizeof *best);
    FILE *f = fopen(file, "r");
    if (!f) return -1;
    char line[2 * PATH_MAX];
    while (fgets(line, sizeof line, f)) {
        /* id parent major:minor root mount-point options [optional...] - fstype source super-options */
        char *sep = strstr(line, " - ");
        if (!sep) continue;
        *sep = 0;
        char *save = NULL, *fld[6];
        int n = 0;
        for (char *t = strtok_r(line, " ", &save); t && n < 6; t = strtok_r(NULL, " ", &save)) fld[n++] = t;
        if (n < 6) continue;
        char *mnt = fld[4], *opts = fld[5];
        char *rest = sep + 3, *fs = strtok_r(rest, " ", &save);
        char *src = strtok_r(NULL, " ", &save);
        char *sopts = strtok_r(NULL, " \n", &save);
        if (!fs) continue;
        unescape(mnt);
        if (!contains(mnt, path)) continue;
        size_t len = strlen(mnt);
        /* The last of equal mount points wins: it is mounted over the others. */
        if (best->found && len < strlen(best->mnt)) continue;
        snprintf(best->mnt, sizeof best->mnt, "%s", mnt);
        snprintf(best->fs, sizeof best->fs, "%s", fs);
        snprintf(best->src, sizeof best->src, "%s", src ? src : "");
        unescape(best->src);
        best->ro = has_opt(opts, "ro") || (sopts && has_opt(sopts, "ro"));
        best->found = 1;
    }
    int err = ferror(f);
    fclose(f);
    if (err) {
        errno = EIO;
        return -1;
    }
    return 0;
}

const char *rec_place_name(rec_place_t p)
{
    switch (p) {
    case REC_PLACE_OK:       return "ok";
    case REC_PLACE_NO_DISK:  return "waiting for disk";
    case REC_PLACE_NOT_DISK: return "not a disk";
    case REC_PLACE_READONLY: return "read-only";
    default:                 return "error";
    }
}

rec_place_t rec_place_check(const char *dir, int allow_rootfs, const rec_place_ops_t *ops,
                            char *why, size_t cap)
{
    int (*st_fn)(const char *, struct stat *) = ops && ops->stat_fn ? ops->stat_fn : stat;
    char *(*rp_fn)(const char *, char *) = ops && ops->realpath_fn ? ops->realpath_fn : realpath;
    const char *mi = ops && ops->mountinfo ? ops->mountinfo : "/proc/self/mountinfo";
    why[0] = 0;

    if (!dir || dir[0] != '/') {
        snprintf(why, cap, "not an absolute path");
        return REC_PLACE_ERROR;
    }
    /* The deepest part of the path that exists. */
    char p[PATH_MAX];
    if (snprintf(p, sizeof p, "%s", dir) >= (int)sizeof p) {
        snprintf(why, cap, "path too long");
        return REC_PLACE_ERROR;
    }
    struct stat st;
    for (;;) {
        size_t len = strlen(p);
        while (len > 1 && p[len - 1] == '/') p[--len] = 0;
        if (st_fn(p, &st) == 0) break;
        /* ENOTDIR: a file in the way, found and refused one level up. */
        if (errno != ENOENT && errno != ENOTDIR) {
            snprintf(why, cap, "%s: %s", p, strerror(errno));
            return REC_PLACE_ERROR;
        }
        char *slash = strrchr(p, '/');
        if (slash == p) p[1] = 0; else *slash = 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        snprintf(why, cap, "%s is not a directory", p);
        return REC_PLACE_ERROR;
    }

    struct stat root;
    int on_root = st_fn("/", &root) == 0 && root.st_dev == st.st_dev;
    if (on_root && !allow_rootfs) {
        snprintf(why, cap, "%s is on the root filesystem: no disk mounted there", p);
        return REC_PLACE_NO_DISK;
    }

    char real[PATH_MAX];
    if (!rp_fn(p, real)) {
        snprintf(why, cap, "%s: %s", p, strerror(errno));
        return REC_PLACE_ERROR;
    }
    /* Without mountinfo a separate tmpfs (/dev/shm) or flash partition
     * would pass the root check: what cannot be told is not used. */
    mount_t m;
    if (find_mount(mi, real, &m) < 0) {
        snprintf(why, cap, "cannot tell what %s is on: %s: %s", p, mi, strerror(errno));
        return REC_PLACE_ERROR;
    }
    if (!m.found) {
        snprintf(why, cap, "cannot tell what %s is on: no mount for it in %s", p, mi);
        return REC_PLACE_ERROR;
    }
    /* "/" from mountinfo when st_dev did not tell (a root without a
     * distinct st_dev): the root filesystem all the same. */
    if (!strcmp(m.mnt, "/") && !allow_rootfs) {
        snprintf(why, cap, "%s is on the root filesystem: no disk mounted there", p);
        return REC_PLACE_NO_DISK;
    }
    /* allow_rootfs vouches for the root filesystem being a disk, not for
     * RAM or flash: a tmpfs, jffs2 or ubifs root is refused all the same. */
    if (not_disk_type(m.fs)) {
        snprintf(why, cap, "%s is on %s (%s), not a disk", p, m.mnt, m.fs);
        return REC_PLACE_NOT_DISK;
    }
    if (m.ro) {
        snprintf(why, cap, "%s is mounted read-only", m.mnt);
        return REC_PLACE_READONLY;
    }
    snprintf(why, cap, "%s (%s)", m.mnt, m.fs);
    return REC_PLACE_OK;
}

int rec_place_mount(const char *dir, const rec_place_ops_t *ops, char *fs, size_t fs_cap, char *src,
                    size_t src_cap)
{
    char *(*rp_fn)(const char *, char *) = ops && ops->realpath_fn ? ops->realpath_fn : realpath;
    const char *mi = ops && ops->mountinfo ? ops->mountinfo : "/proc/self/mountinfo";
    char real[PATH_MAX];
    mount_t m;
    if (!rp_fn(dir, real) || find_mount(mi, real, &m) < 0 || !m.found) return -1;
    snprintf(fs, fs_cap, "%s", m.fs);
    snprintf(src, src_cap, "%s", m.src);
    return 0;
}
