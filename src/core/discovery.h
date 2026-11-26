/* SPDX-License-Identifier: GPL-3.0-or-later */
/* discovery.h — finding cameras on the LAN, changing their addresses and
 * adding them to channels. Used by the API and the Channels screen.
 *
 * Everything that touches the network runs as a *job* on the module's
 * own thread, one job at a time: a scan, an address change (one device or
 * many, with a dry run), adding one device or all of them. Callers start
 * a job (its id, or -1 with errno EBUSY while another runs) and look at
 * it with disc_job_get() until it is done; nothing blocks the UI's or the
 * API's thread.
 *
 * A scan sends each protocol's search (disc_proto.h) three times, a
 * second apart, to the broadcast address of every IPv4 interface picked
 * (all that are up, not loopback, by default), to 255.255.255.255 out of
 * each of them, to the multicast groups (ONVIF, DHIP), and to any unicast
 * targets (hosts on routed subnets, or test responders), then listens for
 * the rest of its time. Answers are merged by MAC (else by address) into
 * the device table; an entry not heard from for DISC_TTL seconds is
 * dropped.
 *
 * Credentials come with each request, are used for that job and wiped
 * after it; they are stored only as part of a camera URL when a device is
 * added to a channel. Nothing here logs a password.
 */
#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <stddef.h>

#include "disc_ops.h"
#include "disc_proto.h"

#define DISC_MAX        64          /* devices kept */
#define DISC_TTL        600         /* s since a device was last heard */
#define DISC_SECS_MAX   15
#define DISC_JOB_MAX    DISC_MAX    /* results per job */

typedef struct {
    unsigned protos;                /* DISC_*; 0 = all */
    int      seconds;               /* 1..DISC_SECS_MAX; 0 = 3 */
    char     ifaces[128];           /* "eth0,eth1"; "" = every IPv4 interface */
    char     targets[256];          /* unicast hosts, "10.0.2.2,192.168.5.20" */
    int      no_broadcast;          /* only the targets */
    int      port[3];               /* where to send: netip, onvif, dhip; 0 = the standard port */
} disc_scan_opts_t;

typedef enum { DISC_JOB_NONE, DISC_JOB_SCAN, DISC_JOB_IP, DISC_JOB_ASSIGN, DISC_JOB_AUTOADD } disc_job_kind_t;

typedef struct {
    char      key[24];              /* the device: MAC, else address */
    char      ip[16];               /* its address before */
    char      new_ip[16];           /* IP jobs: the new one */
    unsigned  proto;                /* the protocol used */
    int       channel;              /* ASSIGN/AUTOADD: 1-based */
    disc_rc_t rc;
    int       done;                 /* this item ran (or was planned, dry run) */
    int       reprobed;             /* IP jobs: answered at the new address */
    char      msg[96];
    char      url[512];             /* ASSIGN/AUTOADD: redacted */
} disc_result_t;

typedef struct {
    unsigned        id;             /* 0: none yet */
    disc_job_kind_t kind;
    int             running;
    int             dry_run;
    int             progress;       /* 0..100 */
    char            msg[96];        /* what it does now, or how it ended */
    int             n;
    disc_result_t   r[DISC_JOB_MAX];
} disc_job_t;

typedef struct {
    char key[24];                   /* MAC or address of a device in the table */
    char ip[16];                    /* the new address; "" = next from `start` */
} disc_ip_item_t;

/* Tune for the board: the number of channels (hal caps), whether a
 * camera is "known" — set once from main. Default CHANNEL_MAX. */
void disc_set_max_channels(int n);

/* Preference order of protocols for a device's URL, "netip,onvif,dhip"
 * (the default: NETIP names both streams without guessing a path; ONVIF
 * asks the device; Dahua's RTSP path is fixed). -1 on an unknown name. */
int disc_set_prefer(const char *list);

/* Snapshot of the table (live entries), sorted by address; returns the
 * count. */
int disc_devices(disc_device_t *out, int max);

/* The 1-based channel whose camera URL points at `d`'s address; 0 none. */
int disc_device_channel(const disc_device_t *d);

/* Forget every device. */
void disc_clear(void);

/* Snapshot of the current (or last) job. */
void disc_job_get(disc_job_t *out);

/* ---- jobs: an id, or -1 (errno EBUSY: another job runs; EINVAL; EAGAIN:
 * disc_auth_wait) ------------------------------------------------------------ */

int disc_scan(const disc_scan_opts_t *o);

/* Change the addresses of `n` devices. Items without an ip get the next
 * free one from `start` (skipping addresses that answer ARP or ping, our
 * own, and those of known devices and cameras). With dry_run, only the
 * plan: results say which device would get which address. After a
 * change, the device is looked for at its new address. */
int disc_ip_job(const disc_ip_item_t *items, int n, const char *start, const char *mask, const char *gw,
                const char *user, const char *pass, int dry_run);

/* Add device `key` to `channel` (1-based; 0 = the first empty one) over
 * `proto` (0 = by preference among those it answered). `name` "" = the
 * device's. A channel that has a camera is refused unless `replace`; a
 * device already on a channel is refused. */
int disc_assign_job(const char *key, int channel, unsigned proto, const char *user, const char *pass,
                    const char *name, int replace);

/* Every device in the table not on a channel yet, into the empty channels
 * in order, with these credentials. */
int disc_autoadd_job(const char *user, const char *pass);

/* Credentials refused by devices are counted: after DISC_AUTH_FAILS in
 * DISC_AUTH_WINDOW seconds, jobs with credentials are refused (errno
 * EAGAIN) until the oldest of them is that old, so neither the API nor the
 * screen can be used to guess a camera's password at speed. The seconds
 * left, or 0. */
#define DISC_AUTH_FAILS  10
#define DISC_AUTH_WINDOW 60
int disc_auth_wait(void);

/* Stop a running job soon and wait for the thread (at exit). */
void disc_shutdown(void);

/* ---- exposed for tests ------------------------------------------------------- */

/* Sequential addresses from `start` for `n` devices within start's
 * subnet (`mask`), skipping those `used` says are taken (it may be NULL;
 * user data `u`). Writes out[i]; returns how many were assigned (fewer
 * when the subnet runs out). */
int disc_plan(const char *start, const char *mask, int n, int (*used)(const char *ip, void *u), void *u,
              char out[][16]);

/* The cameras section with `url`/`sub_url`/`name` put at channel `ch`
 * (1-based), empty slots padding up to it: JSON text (malloc'd) for
 * config_put_json("cameras", ...). `cameras` is the current section as
 * JSON text (unredacted). */
char *disc_cameras_with(const char *cameras, int ch, const char *url, const char *sub_url, const char *name);

#endif /* DISCOVERY_H */
