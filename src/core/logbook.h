/* SPDX-License-Identifier: GPL-3.0-or-later */
/* logbook.h — a log that outlives a reboot without wearing out the flash:
 * the alarm log and the operation log (logs.h) are each one.
 *
 *   RAM       the newest `ram` records, a ring: what is pushed lands here
 *             at once, on any thread, without touching a file.
 *   disk      when a directory on a disk is set (the recording disk's
 *             vigil-logs/), every record is appended to <name>.log there
 *             by logbook_sync (the logs' own thread, once a second); past
 *             `file_cap` bytes the file is rotated (<name>.log.1, ...,
 *             `files` in all), so the history is bounded. Queries go on
 *             into these files once the RAM ring is exhausted.
 *   flash     the newest `flash_keep` records, written whole into one
 *             small file (beside vigil.yaml) at most every `flash_secs`
 *             and on the clean stop, atomically (a new file, fsync,
 *             rename): what is left when there is no disk.
 *
 * Every record has an id unique across boots ("<boot>.<n>", the boot a
 * random number), so a record that is both in the flash copy and on the
 * disk is found once: when a disk is set, the records in RAM that it does
 * not have yet are appended to it, and the RAM ring becomes the disk's
 * newest records. Order is the order of pushing (a clock set by NTP
 * after the start does not reorder anything).
 *
 * The disk format is one record per line, tab-separated, with \t \n \r
 * and \\ escaped:   id  time  channel  active  source  who  type  detail
 */
#ifndef LOGBOOK_H
#define LOGBOOK_H

#include <stddef.h>
#include <time.h>

#define LOG_ID      24
#define LOG_SOURCE  12
#define LOG_WHO     32
#define LOG_TYPE    24
#define LOG_DETAIL  128

typedef struct {
    char   id[LOG_ID];          /* set by logbook_push */
    time_t time;                /* wall clock; 0: now */
    int    ch;                  /* 1-based camera; 0: none (the system, a disk) */
    int    active;              /* 1: began, 0: ended, -1: neither (a single event) */
    char   source[LOG_SOURCE];  /* where from: "screen", "api", "netip", "system", ... */
    char   who[LOG_WHO];        /* the user ("" none) */
    char   type[LOG_TYPE];      /* what: "motion", "offline", "login", "config_saved", ... */
    char   detail[LOG_DETAIL];
} logrec_t;

typedef struct {
    time_t      from, to;       /* time >= from and <= to; 0: open */
    int         ch;             /* -1: any channel */
    const char *type;           /* NULL or "": any; else one type, or several separated by ',' */
    const char *who;            /* NULL or "": anyone; else exactly */
    const char *text;           /* NULL or "": anything; else in who, source, type or detail
                                   (case-insensitive) */
    int         ram_only;       /* do not read the disk's files (the UI's first look) */
} logfilter_t;

typedef struct {
    const char *name;           /* the files' name: <name>.log */
    int         ram;            /* records kept in RAM */
    int         flash_keep;     /* the newest records copied to flash; 0: none */
    int         flash_secs;     /* at most this often (the clean stop: always) */
    int         flash_no_disk;  /* 1: only while no disk is set */
    long        file_cap;       /* bytes per file on the disk */
    int         files;          /* files on the disk, the current one included */
} logbook_conf_t;

typedef struct logbook logbook_t;

/* `conf` is copied (its name too). NULL without memory. */
logbook_t *logbook_create(const logbook_conf_t *conf);
void       logbook_destroy(logbook_t *b);

/* Add a record (a copy; its id and, when 0, its time are set). Any thread. */
void logbook_push(logbook_t *b, const logrec_t *r);

/* Records that match `f`, newest first: those after the first `offset`
 * matches, at most `limit`, into `out`. Returns how many; *more (may be
 * NULL) says whether there are more after them. Any thread; past the RAM
 * ring it reads the disk's files (a few KiB at a time, backwards). */
int logbook_query(logbook_t *b, const logfilter_t *f, int offset, int limit, logrec_t *out, int *more);

/* Does `r` match `f`? */
int logbook_match(const logrec_t *r, const logfilter_t *f);

/* The flash copy: read `path` now (its records join the RAM ring, before
 * whatever was pushed already), and write it there from now on. NULL:
 * none. */
void logbook_set_flash(logbook_t *b, const char *path);

/* The disk's directory ("" or NULL: none). A new one is created if need
 * be, the records it lacks are appended, and the RAM ring becomes its
 * newest records. 0, or -1 (errno; the book stays without a disk). */
int logbook_set_disk(logbook_t *b, const char *dir);
/* The directory in use, "" when none. */
void logbook_disk(logbook_t *b, char *out, size_t cap);

/* Write what is due: the new records to the disk; the flash copy when it
 * is out of date and `flash_secs` have passed since the last one (`now`:
 * a monotonic clock), or always with `final`. A disk that fails to write
 * is let go (logged); the next logbook_set_disk takes it back. */
void logbook_sync(logbook_t *b, time_t now, int final);

/* Records pushed since the start (for the tests and the status). */
unsigned long logbook_count(logbook_t *b);

/* One record as a line of the disk's files (with the '\n'), into `out`;
 * its length, or -1 when it does not fit. And back: 0, or -1 when `line`
 * (without its '\n') is not one. */
int logbook_format(const logrec_t *r, char *out, size_t cap);
int logbook_parse(const char *line, size_t len, logrec_t *r);

/* `r` as CSV (RFC 4180, a header line first when `header`): time
 * (local, "YYYY-MM-DD HH:MM:SS"), channel, active, source, who, type,
 * detail, id. Returns the length, or -1 when it does not fit. */
int logbook_csv(const logrec_t *r, int header, char *out, size_t cap);

#endif /* LOGBOOK_H */
