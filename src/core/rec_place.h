/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rec_place.h — may a recording go into this directory?
 *
 * vigild creates the recording directory itself. With the disk it is meant
 * for not mounted (yet, or any more), that would create it on the root
 * filesystem underneath the mount point -- on a board, the flash -- and
 * record there until the flash is worn out or full. So a directory is used
 * only when it is, or would be created, on a disk:
 *
 *   - not on the filesystem that holds "/" (st_dev of the deepest part of
 *     the path that exists, against st_dev of "/"), unless record.allow_rootfs
 *     says the root filesystem is a disk;
 *   - not on a filesystem that holds no disk (tmpfs, ramfs, proc, ...) or that
 *     is flash (jffs2, ubifs, squashfs, ...), and not mounted read-only: the
 *     mount is the longest mount point in /proc/self/mountinfo that contains
 *     the resolved path.
 *
 * Only the part of the path below its deepest existing directory is ever
 * created, and that directory is on the disk, so nothing is created above
 * the mount point.
 */
#ifndef REC_PLACE_H
#define REC_PLACE_H

#include <stddef.h>
#include <sys/stat.h>

typedef enum {
    REC_PLACE_OK = 0,
    REC_PLACE_NO_DISK,          /* would land on the root filesystem: no disk mounted there */
    REC_PLACE_NOT_DISK,         /* on RAM, a virtual or a flash filesystem */
    REC_PLACE_READONLY,         /* the disk is mounted read-only */
    REC_PLACE_ERROR,            /* cannot be used: not a directory, no access, ... */
} rec_place_t;

/* What the check reads, replaceable for tests. NULL members (or a NULL
 * ops) mean the real thing; mountinfo NULL means /proc/self/mountinfo. A
 * mountinfo that cannot be opened (not Linux) leaves only the root check. */
typedef struct {
    int         (*stat_fn)(const char *path, struct stat *st);
    char       *(*realpath_fn)(const char *path, char *resolved);
    const char  *mountinfo;
} rec_place_ops_t;

/* Judge `dir` (absolute). `why` gets one line for the log: the reason, or
 * the filesystem it is on. */
rec_place_t rec_place_check(const char *dir, int allow_rootfs, const rec_place_ops_t *ops,
                            char *why, size_t cap);

/* Short state for the status and the events: "waiting for disk", ... */
const char *rec_place_name(rec_place_t p);

#endif /* REC_PLACE_H */
