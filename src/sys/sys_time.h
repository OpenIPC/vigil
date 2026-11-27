/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys_time.h — the clock, the time zone, NTP and the host name, the
 * OpenIPC way.
 *
 * Time zone: OpenIPC keeps a POSIX TZ string in /etc/TZ ("GMT0",
 * "CET-1CEST,M3.5.0,M10.5.0/3") and the zone's name in /etc/timezone
 * ("Etc/GMT"); /etc/profile and the init scripts export TZ from /etc/TZ
 * (there is no zoneinfo database on the flash). DST is part of the
 * string: a second name and offset and the rules for when it starts and
 * ends (",Mm.w.d/time"). A changed zone reaches vigild at once (setenv,
 * tzset) and every other program when it next starts.
 *
 * NTP: busybox ntpd, started by /etc/init.d/S49ntpd with the servers of
 * /etc/ntp.conf ("server 0.pool.ntp.org iburst"). "Sync now" is a
 * one-shot `ntpd -n -q -p SERVER...`.
 *
 * The clock: settimeofday, then `hwclock -w -u` when there is an RTC and
 * `fake-hwclock save` where the firmware keeps the time across reboots
 * that way.
 *
 * Host name: /etc/hostname, sethostname, the 127.0.1.1 line of
 * /etc/hosts, and /etc/hostname.auto, which tells S31hostname the name
 * was chosen and must not be replaced by <soc>-<sensor>.
 */
#ifndef SYS_TIME_H
#define SYS_TIME_H

#include <stddef.h>
#include <time.h>

/* ---- time zones -------------------------------------------------------------------- */

typedef struct {
    const char *name;               /* "Europe/Berlin" */
    const char *posix;              /* "CET-1CEST,M3.5.0,M10.5.0/3" */
} tz_zone_t;

/* The zones offered by name, NULL-terminated. */
extern const tz_zone_t tz_zones[];
const tz_zone_t *tz_find(const char *name);

/* A time zone as fields, for building and showing a custom one. Offsets
 * are minutes east of UTC (UTC+3: 180). A rule is month 1..12, week 1..5
 * (5: the last), weekday 0..6 (0: Sunday), and minutes after local
 * midnight (may pass 24 h, as POSIX allows). */
typedef struct {
    int std_offset;
    int dst;                        /* has daylight saving time */
    int dst_offset;
    int start_month, start_week, start_wday, start_min;
    int end_month, end_week, end_wday, end_min;
} tz_rule_t;

/* `r` as a POSIX TZ string with numeric names ("<+03>-3",
 * "<+01>-1<+02>,M3.5.0/2,M10.5.0/3"). 0, or -1 with `err`. */
int tz_build(const tz_rule_t *r, char *out, size_t cap, char *err, size_t ecap);

/* A POSIX TZ string as fields; -1 when it is not one (or uses the Jn / n
 * day forms, which only a custom string can say). */
int tz_parse(const char *posix, tz_rule_t *r);

/* ---- the clock and the zone ------------------------------------------------------------ */

typedef struct {
    char zone[64];                  /* /etc/timezone ("" none) */
    char tz[96];                    /* /etc/TZ */
    int  rtc;                       /* /dev/rtc0 is there */
    char servers[4][64];            /* /etc/ntp.conf */
    int  n_servers;
} sys_clock_t;

void clock_read(sys_clock_t *c);

/* Set the zone: `zone` from tz_zones (its string is used), or "" with a
 * POSIX string of one's own. Writes /etc/TZ and /etc/timezone ("" removes
 * nothing: the name becomes "custom"), and sets TZ for this process. */
int clock_set_zone(const char *zone, const char *posix, char *err, size_t cap);

/* Set the system clock to `t` (UTC), then the RTC and fake-hwclock. */
int clock_set(time_t t, char *err, size_t cap);

/* The servers of /etc/ntp.conf, replaced (1..4 host names). */
int ntp_set_servers(char servers[][64], int n, char *err, size_t cap);

/* One-shot sync with `servers`; 0 when the clock was set, -1 with `msg`. */
int ntp_sync(char servers[][64], int n, char *msg, size_t cap);

/* Start (restart: it rereads its servers) or stop the firmware's ntpd. */
int ntp_daemon(int on, char *err, size_t cap);

/* "pool.ntp.org", "10.0.0.1": a host name or IPv4 address. */
int ntp_server_ok(const char *s);

/* ---- host name --------------------------------------------------------------------- */

/* /etc/hostname, or the kernel's. */
void host_get(char *name, size_t cap);

/* 1..63 of a-z, 0-9 and '-', not starting or ending with '-'. 0, or -1
 * with `err`. */
int host_check(const char *name, char *err, size_t cap);
int host_set(const char *name, char *err, size_t cap);

#endif /* SYS_TIME_H */
