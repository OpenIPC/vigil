/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys_log.h — the system's own log, read when it is shown: syslogd's ring
 * (BusyBox logread on OpenIPC firmware: vigild's lines are in it, with
 * -s) and the kernel's (dmesg). vigild keeps no copy of either: the ring
 * is the firmware's, in RAM, and a copy on the flash would wear it. A
 * page can save what it shows to a disk (the Logs page's Export).
 *
 * Without logread (a desktop), /var/log/messages is read instead, when
 * it is there and readable. */
#ifndef SYS_LOG_H
#define SYS_LOG_H

#include <stddef.h>
#include <time.h>

typedef enum { SYS_LOG_SYSLOG = 0, SYS_LOG_KERNEL = 1 } sys_log_src_t;

typedef struct {
    time_t time;                    /* wall clock; 0: not known */
    int    level;                   /* syslog's: 0 emerg .. 7 debug; -1 not known */
    char   tag[32];                 /* "vigil", "kernel", "udhcpc", ... ("" none) */
    char   text[256];
} sys_logline_t;

#define SYS_LOG_MAX (256u << 10)    /* bytes of either log read at most (the newest) */

/* The lines of `src`, newest first, that match: `max_level` >= 0 keeps
 * those at that level or more urgent (unknown ones too), `text` (NULL or
 * "": any) a substring of the tag or the text, any case, `from` and `to`
 * (0: open) the time (lines without one are kept). `offset` matches
 * skipped, at most `max` into `out`; *more (may be NULL) when there are
 * more. Returns how many, or -1 with `why` when there is no such log. */
int sys_log_read(sys_log_src_t src, int max_level, const char *text, time_t from, time_t to, int offset, int max,
                 sys_logline_t *out, int *more, char *why, size_t cap);

/* One line of logread ("Nov 28 12:34:56 host daemon.info vigil[123]: ...",
 * or with -S "Nov 28 12:34:56 ...") at `now` (the year is now's, or the
 * one before for a date ahead of it), and one of the kernel's ("<6>[
 * 12.345678] ...", `boot` the wall clock at its 0). 0, or -1 for an empty
 * line. */
int sys_log_parse_syslog(const char *line, size_t len, time_t now, sys_logline_t *out);
int sys_log_parse_kernel(const char *line, size_t len, time_t boot, sys_logline_t *out);

/* The clock the lines' year comes from (NULL: time(NULL)); the tests'. */
void sys_log_set_clock(time_t (*now)(void));

/* "emerg", "alert", "crit", "err", "warning", "notice", "info", "debug";
 * "" for -1. */
const char *sys_log_level_name(int level);

#endif /* SYS_LOG_H */
