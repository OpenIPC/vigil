/* SPDX-License-Identifier: GPL-3.0-or-later */
/* snap_store.h — the snapshots kept: JPEG files in one directory on a disk.
 *
 * Where: snapshot.path, else next to the recordings (record.path's
 * parent + "/snapshots": /mnt/sda1/rec -> /mnt/sda1/snapshots), else, when
 * that parent is not on the disk (record.path is the mount point itself),
 * record.path + "/snapshots". Like a recording, never onto the flash or a
 * RAM disk (rec_place.h, with record.allow_rootfs).
 *
 * Names say what they are, so the directory is its own index:
 *   ch01_20261128-101500-123_manual.jpg      (channel, local time to the ms, why)
 * the why being "manual" (the screen, the API) or the camera event that
 * took it ("motion", "human", ... ch_event_name). Oldest first goes when
 * the files together pass snapshot.max_mb.
 */
#ifndef SNAP_STORE_H
#define SNAP_STORE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define SNAP_NAME_MAX 64
#define SNAP_KIND_MAX 16

typedef struct {
    char     name[SNAP_NAME_MAX];
    int      ch;                    /* 1-based */
    time_t   time;                  /* local time of the name, as a time_t */
    int      ms;
    char     kind[SNAP_KIND_MAX];
    uint64_t size;
} snap_file_t;

/* The directory: `snap_path` when set, else from `rec_path` as above. 0
 * with `out`, or -1 with `why` (no recording directory to put it by, or
 * not a disk: rec_place_name's words and the reason). */
int snap_dir(const char *snap_path, const char *rec_path, int allow_rootfs, char *out, size_t cap, char *why,
             size_t wcap);

/* The name for channel `ch` (1-based) at `t` (wall clock) + `ms`, for `kind`
 * (letters, digits and _ only; others become _). 0, or -1. */
int snap_name(char *out, size_t cap, int ch, time_t t, int ms, const char *kind);

/* A name snap_name made, back into its parts (f->size 0). 0, or -1 for any
 * other name (a file that is not one of ours, "../x", ...). */
int snap_name_parse(const char *name, snap_file_t *f);

/* Write `len` bytes as `dir`/`name`, the directory created below its
 * deepest existing part (snap_dir checked where that is): a temporary
 * file renamed into place, so a reader never sees half of one. 0, or -1
 * with `err`. */
int snap_save(const char *dir, const char *name, const void *p, size_t len, char *err, size_t cap);

/* The snapshots in `dir`, newest first, at most `max` (out may be NULL to
 * count); *total (may be NULL) their bytes. The number there is, or -1 when
 * the directory cannot be read (none yet: 0). */
int snap_list(const char *dir, snap_file_t *out, int max, uint64_t *total);

/* `dir`/`name` removed, if `name` is one of ours. 0, or -1 (errno ENOENT,
 * EINVAL). */
int snap_delete(const char *dir, const char *name);

/* Oldest first until the rest is at most `max_bytes` (0: no limit). The
 * number of files removed. */
int snap_trim(const char *dir, uint64_t max_bytes);

#endif /* SNAP_STORE_H */
