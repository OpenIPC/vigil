/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tar.h — the few files of a configuration export as one ustar archive
 * (POSIX.1-1988): readable with any tar, and small enough to write and
 * read here without a library.
 *
 * Writing: regular files only, names up to 99 bytes without a directory,
 * mode 0600, owner 0. Reading: the same, every header's checksum checked;
 * anything else (links, directories, a name with '/', a file that runs
 * past the end) makes the archive invalid rather than skipped. */
#ifndef TAR_H
#define TAR_H

#include <stddef.h>
#include <time.h>

typedef struct {
    char  *data;                    /* malloc'd */
    size_t len, cap;
} tar_out_t;

/* Add a file; 0, or -1 (memory, a bad name). */
int  tar_add(tar_out_t *t, const char *name, const void *data, size_t len, time_t mtime);
/* The two zero blocks that end an archive; 0, or -1. */
int  tar_finish(tar_out_t *t);
void tar_out_free(tar_out_t *t);

#define TAR_FILES_MAX 8

typedef struct {
    char        name[100];
    const char *data;               /* into the archive */
    size_t      len;
} tar_file_t;

/* The files of the archive `buf` (pointers into it), at most `max`: how
 * many, or -1 with `err` when it is not an archive this reads. */
int tar_read(const void *buf, size_t len, tar_file_t *files, int max, char *err, size_t cap);

/* The file `name` of a tar_read list, or NULL. */
const tar_file_t *tar_find(const tar_file_t *files, int n, const char *name);

#endif /* TAR_H */
