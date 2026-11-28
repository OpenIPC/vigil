/* SPDX-License-Identifier: GPL-3.0-or-later */
/* logs.h — vigild's two logs of its own (docs/management.md, "Logs"):
 *
 *   alarm log       what needs attention: a camera's events (motion,
 *                   people, video loss, ...) as they start and stop, a
 *                   camera going offline and back, recording failing, the
 *                   disk full, missing or failing (SMART, a surface scan's
 *                   bad sectors). Fed by the channels' snapshots and the
 *                   events ring on the logs' thread (alarm_feed_*), and by
 *                   alarmlog_push from where it is known.
 *   operation log   who did what (an audit): logins and failed ones,
 *                   lockouts, logouts, settings saved (the section and the
 *                   names of the fields changed, never a value), users and
 *                   groups changed, reboots, shutdowns, disk formats, scans,
 *                   firmware upgrades, configuration imports and resets.
 *                   oplog_push from wherever that happens.
 *
 * Both are logbooks (logbook.h): RAM rings, appended to the recording
 * disk's vigil-logs/ directory when there is one, and a small copy beside
 * vigil.yaml written at most every LOGS_FLASH_SECS and at the clean stop
 * (the alarms' only while there is no disk). The system log (syslog and
 * the kernel's) is not kept here: it is the firmware's own ring, read
 * when shown (sys_log.h).
 *
 * Who did it: every thread has an actor (user, source, address), set by
 * whoever takes requests on it -- the API's authorization hook, the zero
 * channel, ONVIF, the NETIP and RTSP servers, the UI bridge on the main
 * thread -- so that a function called from any of them (config_put_json,
 * users_add, mgmt_power_request, ...) says who asked without being told.
 * Nobody set: source "system".
 */
#ifndef LOGS_H
#define LOGS_H

#include <stdarg.h>
#include <time.h>

#include "logbook.h"

#define LOGS_ALARMS_RAM   1000
#define LOGS_OPS_RAM      500
#define LOGS_FLASH_OPS    200           /* the operation log's newest, kept on flash */
#define LOGS_FLASH_ALARMS 100           /* the alarm log's, while there is no disk */
#define LOGS_FLASH_SECS   600           /* the flash copy at most this often */
#define LOGS_ALARMS_FILE  (2L << 20)    /* bytes per file on the disk, 8 files */
#define LOGS_OPS_FILE     (512L << 10)  /* 4 files */
#define LOGS_DIR          "vigil-logs"  /* beside the recording directory, on its disk */

/* The books (created at the first use, so a push before logs_start is
 * kept in RAM). */
logbook_t *logs_alarms(void);
logbook_t *logs_ops(void);

/* Read the flash copies from `dir` (beside vigil.yaml; NULL: none, RAM
 * only) and start the logs' thread: `feed` (alarm_feed_poll; NULL: none)
 * twice a second, the disk (found from record.path every few seconds),
 * the flash copies. 0 or -1. */
int  logs_start(const char *dir, void (*feed)(time_t now));
/* Stop it, writing everything out (the disk, then the flash copies). */
void logs_stop(void);
/* One pass of the thread's work at `now` (monotonic): the tests'. */
void logs_tick(time_t now, int final);

/* Where the logs go on the disk for recording directory `rec_path`
 * (allow_rootfs: record.allow_rootfs): 0 with `out` when it is on a disk
 * now, -1 when not. Its parent's vigil-logs when that is on the same
 * disk (/mnt/sda1/rec -> /mnt/sda1/vigil-logs), else inside it. */
int logs_disk_dir(const char *rec_path, int allow_rootfs, char *out, size_t cap);

/* ---- the operation log ---------------------------------------------------------------- */

/* `user` (NULL: this thread's actor's; "" none) did `action` from
 * `source` (NULL: the actor's), `detail` (may be NULL). A secret never
 * goes into `detail`. Any thread. The actions vigild logs are listed in
 * docs/management.md; others' code adds its own (firmware_upgrade,
 * config_import, config_export, config_reset, ...). */
void oplog_push(const char *user, const char *source, const char *action, const char *detail);
void oplog_pushf(const char *user, const char *source, const char *action, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* This thread's actor: who the requests it handles now come from. `user`
 * "" while not known yet (before a login is checked); `source` "screen",
 * "web", "api", "onvif", "netip", "rtsp", "system", ...; `addr` the
 * client's address ("" none). NULL keeps that part. */
void oplog_actor(const char *user, const char *source, const char *addr);
/* The actor's user, source and address into the buffers (any may be NULL). */
void oplog_actor_get(char *user, size_t ucap, char *source, size_t scap, char *addr, size_t acap);

/* A login checked on this thread (the actor's source and address): `ok`
 * 1 let in, 0 refused. A success is logged once per user, source and
 * address while they keep coming within `quiet_secs` (0: every one; the
 * API checks every request), a failure always, with `fails` (in a row,
 * 0: not counted). On success the actor's user becomes `user`. */
void oplog_login(const char *user, int ok, int fails, int quiet_secs);
/* After failed logins the address is refused for `secs` s. */
void oplog_lockout(int fails, long secs);

/* A configuration section (NULL: the whole document) was saved: its
 * name and the fields that changed between `old` and `nc` (both
 * vigil_config_t), by name only. Called by config_put_json. */
void oplog_config_saved(const char *section, const void *old, const void *nc);

/* ---- the alarm log -------------------------------------------------------------------- */

/* Alarm types vigild itself raises (a camera's events use their
 * ch_event_name: "motion", "human", ...). */
#define ALARM_OFFLINE     "offline"         /* a camera cannot be reached / lost (start), back (stop) */
#define ALARM_REC_FAILED  "rec_failed"      /* writing a recording failed */
#define ALARM_REC_OFF     "rec_off"         /* recording cannot start: no disk, not a disk, read-only */
#define ALARM_DISK_FULL   "disk_full"       /* record.on_full stop: no room */
#define ALARM_DISK_ERROR  "disk_error"      /* a format failed, ... */
#define ALARM_DISK_SMART  "disk_smart"      /* SMART says warning or failing (start), good (stop) */
#define ALARM_DISK_BAD    "disk_bad_sectors" /* a surface scan found bad ranges */
#define ALARM_NO_DECODER  "no_decoder"

/* Channel `ch` (1-based; 0: none) `type` began (1), ended (0) or
 * happened (-1); `detail` may be NULL. Any thread. */
void alarmlog_push(int ch, const char *type, int active, const char *detail);
void alarmlog_pushf(int ch, const char *type, int active, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* The feed of the alarm log from what the core reports today: the
 * channels' event bits (their edges) and the events ring's texts. Called
 * by the logs' thread twice a second; the tests call it themselves. When
 * the camera events are subscribed to instead (cam_events.h), only the
 * events ring's part stays. */
void alarm_feed_poll(time_t now);
/* What an event of the events ring means for the alarm log: 1 with the
 * type and state, 0 when it means nothing there. */
int  alarm_feed_classify(int ch, const char *text, char *type, size_t cap, int *active);

#endif /* LOGS_H */
