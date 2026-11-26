/* SPDX-License-Identifier: GPL-3.0-or-later */
/* seg_writer.h — record an elementary stream as-is into time-based segments.
 *
 * Files are <dir>/<prefix>_<YYYYmmdd_HHMMSS>.<ext>. A segment starts only on
 * a keyframe (which carries its parameter sets, see rtp_depay.h), so every
 * file plays on its own. A new segment starts at the first keyframe after
 * `seg_secs`; with `keep` > 0, only the newest `keep` segments this writer
 * made are kept (older files in the directory are left alone).
 *
 * Whatever `keep` says, the filesystem keeps a reserve free (10%, at least
 * 8 MiB): below it the oldest finished recording in the directory goes,
 * whichever camera made it — one per second at most, and never a segment
 * another writer has open. With none left, or when deleting does not free
 * space, the current segment is closed and a new one starts at the next
 * keyframe. Recording rolls over instead of failing on a full disk.
 *
 * With `overwrite` off (record.on_full: stop) nothing is deleted for space:
 * below the reserve the segment is closed and seg_write answers SEG_FULL,
 * writing nothing, until the reserve is free again.
 *
 * A quota (seg_set_quota) limits this writer's own files (prefix_*): the
 * oldest go beyond `max_bytes` in all, and any older than `max_age`
 * seconds. Checked when a segment opens, at most every SEG_QUOTA_SECS.
 *
 * Names never collide: a segment reopened within the same second gets a
 * _N suffix, and files are created with O_EXCL.
 */
#ifndef SEG_WRITER_H
#define SEG_WRITER_H

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define SEG_KEEP_MAX  64
#define SEG_FULL      (-2)          /* seg_write: the disk is full and nothing may go */
#define SEG_QUOTA_SECS 60

typedef struct {
    char     dir[192];
    char     prefix[32];
    char     ext[8];
    int      seg_secs;
    int      keep;
    FILE    *f;
    time_t   seg_start;
    char     names[SEG_KEEP_MAX][320];  /* ring of the files we made */
    int      n_names, head;
    uint64_t bytes;                     /* written this session */
    int      failed;                    /* last open/write failed */
    uint64_t reserve;                   /* bytes to keep free on the filesystem */
    time_t   space_checked;
    int      overwrite;                 /* 1: old recordings make room (default) */
    int      full;                      /* !overwrite and below the reserve */
    uint64_t quota_bytes;               /* 0: none */
    time_t   quota_age;                 /* seconds; 0: none */
    time_t   quota_checked;
} seg_writer_t;

/* Set up (creates `dir` if needed). Returns 0 or -1. */
int seg_init(seg_writer_t *w, const char *dir, const char *prefix,
             int seg_secs, int keep);

/* Room on a full disk: delete the oldest recordings (1, the default), or
 * stop writing (0). */
void seg_set_overwrite(seg_writer_t *w, int overwrite);

/* Limit this writer's recordings to `max_bytes` (0: none) and `max_age`
 * seconds (0: none); checked when the next segment opens. */
void seg_set_quota(seg_writer_t *w, uint64_t max_bytes, time_t max_age);

/* Enforce the quota on the files of `prefix` in `dir` now: delete the
 * oldest (by name, which is by time) beyond `max_bytes`, and those whose
 * modification time is before `now - max_age`; never a segment open in
 * this process. Returns the number deleted. */
int seg_quota_enforce(const char *dir, const char *prefix, uint64_t max_bytes, time_t max_age,
                      time_t now);

/* Change the extension for the next segment ("h264" / "h265"). */
void seg_set_ext(seg_writer_t *w, const char *ext);

/* Append one access unit. Nothing is written until the first keyframe.
 * Returns 0, -1 on a write error or when closing the previous segment
 * failed to flush it (w->failed is set), or SEG_FULL. */
int seg_write(seg_writer_t *w, const uint8_t *data, size_t len, int key);

/* Close the current segment; the next keyframe starts a new one.
 * Returns -1 if flushing it failed (its tail is lost). */
int seg_close(seg_writer_t *w);

/* Name of the open segment, or "" when none. */
const char *seg_current(const seg_writer_t *w);

#endif /* SEG_WRITER_H */
