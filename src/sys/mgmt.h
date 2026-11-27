/* SPDX-License-Identifier: GPL-3.0-or-later */
/* mgmt.h — the management pages' actions, for the API and the UI alike.
 *
 * Between the backends (sys_*.h: one change to the system each) and the
 * two front ends (src/api, src/app/ui_bridge.c) sits this layer, which
 * owns what spans time or the rest of vigild:
 *
 *   jobs      slow actions (restarting an interface, formatting a disk,
 *             a test mail, "sync now") run one at a time on a worker
 *             thread; each gets an id whose state the caller polls
 *             (queued, running, done, failed, with a message). A front
 *             end never blocks on one.
 *   timers    a thread for what happens by the clock: rolling back a
 *             network change nobody confirmed, NTP checks, DDNS updates,
 *             mounting the network storage, the alarm center's events
 *             and heartbeat, mails about trouble.
 *   vigild    recording is held off a disk before it is unmounted or
 *             formatted (channels_rec_hold); settings that live in
 *             vigil.yaml go through the configuration store as the API's
 *             PUT does; what happened goes to the event strip.
 *
 * Every function is thread-safe. Nothing changes the system unless
 * sys_managed() (OpenIPC firmware, or a test root); otherwise the
 * setters answer MGMT_EUNMANAGED.
 */
#ifndef MGMT_H
#define MGMT_H

#include <stddef.h>
#include <time.h>

#include "sys_disk.h"
#include "sys_net.h"
#include "sys_time.h"

enum {
    MGMT_OK = 0,
    MGMT_EINVAL = -1,               /* bad input: `field` and `err` say which and why */
    MGMT_EUNMANAGED = -2,           /* not a system vigild manages */
    MGMT_EBUSY = -3,                /* another change of the same kind is under way */
    MGMT_EFAIL = -4,                /* `err` */
};

/* ---- jobs ------------------------------------------------------------------------- */

typedef enum { JOB_QUEUED, JOB_RUNNING, JOB_DONE, JOB_FAILED } mgmt_job_state_t;

typedef struct {
    int              id;
    char             kind[24];      /* "network", "format", "mount", "ntp-sync", ... */
    char             target[32];    /* the interface, the disk, ... */
    mgmt_job_state_t state;
    char             msg[256];
    time_t           started, finished;
} mgmt_job_t;

/* 0 and the job, -1 when it is no longer remembered (the last 16 are). */
int  mgmt_job(int id, mgmt_job_t *out);
const char *mgmt_job_state_name(mgmt_job_state_t s);

/* ---- life ------------------------------------------------------------------------- */

int  mgmt_start(void);
void mgmt_stop(void);

/* ---- network ----------------------------------------------------------------------- */

/* A network change waits this long for a confirmation, then is undone. */
#define MGMT_CONFIRM_SECS 60

/* Apply `cfg` (validated now: MGMT_EINVAL with field and err) as a job,
 * after a second, so that the answer to the request that asked for it
 * still goes out over the old address. With `confirm` the change is
 * undone unless mgmt_net_confirm comes within MGMT_CONFIRM_SECS. Returns
 * the job id (> 0) or an error. */
int mgmt_net_apply(const net_cfg_t *cfg, int confirm, char *field, size_t fcap, char *err, size_t cap);

/* The change waiting for a confirmation: 1 with its interface and the
 * seconds left, 0 when none. */
int mgmt_net_pending(char *ifname, size_t cap, int *secs_left);
int mgmt_net_confirm(void);         /* 0, or -1 when nothing waits */
/* For the tests: a shorter wait (and no pause before applying). */
void mgmt_set_confirm_secs(int secs);
int mgmt_net_revert(void);          /* undo now: the job id, or -1 */

/* Wi-Fi: store the network (and `profile` as wlandev when given) and
 * bring the interface up, loading the adapter's driver first when there
 * is no interface yet; a job. */
int mgmt_wifi_set(const char *ifname, const char *ssid, const char *password, const char *profile, char *err,
                  size_t cap);
/* LTE: wlandev = a modem profile; the firmware loads it at the next boot.
 * `err` gets the message either way. */
int mgmt_lte_profile(const char *profile, char *err, size_t cap);

/* ---- time and name ----------------------------------------------------------------- */

typedef struct {
    time_t last_try, last_ok;       /* 0: never */
    char   result[160];             /* of the last try */
} mgmt_ntp_status_t;

void mgmt_ntp_status(mgmt_ntp_status_t *st);

/* The zone (one of tz_zones by `zone`, or "" and a POSIX `tz`), and the
 * clock when `set_clock`. */
int mgmt_time_set(const char *zone, const char *tz, int set_clock, time_t t, char *err, size_t cap);

/* NTP: the servers in /etc/ntp.conf, the firmware's ntpd started or
 * stopped, and time.ntp / time.ntp_interval saved in vigil.yaml. */
int mgmt_ntp_set(int enabled, char servers[][64], int n, int interval_min, char *err, size_t cap);

/* One-shot sync now, as a job. */
int mgmt_ntp_sync(void);

int mgmt_hostname_set(const char *name, char *err, size_t cap);

/* ---- reboot and shutdown ----------------------------------------------------------- */

typedef enum { MGMT_POWER_NONE = 0, MGMT_POWER_REBOOT, MGMT_POWER_OFF } mgmt_power_t;

/* Ask for a reboot or a shutdown (the UI's Exit, the API's
 * /system/reboot and /system/shutdown): vigild's main loop sees it
 * (mgmt_power_pending), stops like on SIGTERM (the channels close their
 * segments) and then calls mgmt_power_run. MGMT_OK, or MGMT_EUNMANAGED
 * when this is not a system vigild manages (a desktop is not rebooted). */
int          mgmt_power_request(mgmt_power_t what);
mgmt_power_t mgmt_power_pending(void);
/* sync(), then busybox's reboot or poweroff. Returns only when that
 * failed (logged). */
void         mgmt_power_run(mgmt_power_t what);

/* ---- services (vigil.yaml: ddns, smtp, alarm) --------------------------------------- */

typedef struct {
    time_t last;                    /* last attempt, 0: never */
    int    ok;
    char   result[160];
} mgmt_svc_status_t;

typedef struct {
    mgmt_svc_status_t ddns, smtp, alarm, nas;
} mgmt_services_t;

void mgmt_services(mgmt_services_t *s);

int mgmt_ddns_update(void);         /* job id */
int mgmt_smtp_test(void);           /* job id: a test mail with the settings in force */

/* ---- disks and network storage --------------------------------------------------------- */

/* Where the network storage is mounted (record.path under it records
 * there). */
#define MGMT_NAS_MOUNT "/mnt/nas"

/* sys_disk_list with the recording path in force. */
int mgmt_disks(sys_disk_t *out, int max);

/* Format `name` (a disk or a partition) as `fs`; `confirm` must be the
 * name again, as typed by the user. Recording onto it is held first.
 * Validated now (MGMT_EINVAL with err), done as a job (its id). */
int mgmt_disk_format(const char *name, const char *fs, const char *confirm, char *err, size_t cap);
int mgmt_disk_mount(const char *name, char *err, size_t cap);
int mgmt_disk_unmount(const char *name, char *err, size_t cap);

/* Mount the network storage settings in force at a test directory, write
 * and read a file, unmount: a job. */
int mgmt_nas_test(void);

#endif /* MGMT_H */
