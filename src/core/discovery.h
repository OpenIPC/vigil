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
 * the rest of its time. Each answer is merged by MAC (else by address)
 * into the device table the moment it is parsed (disc_table_add), so the
 * table can be shown while the scan runs: disc_generation() moves with
 * every change, and a reader takes a new disc_devices() snapshot only
 * then. An entry not heard from for DISC_TTL seconds is dropped.
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

typedef enum { DISC_JOB_NONE, DISC_JOB_SCAN, DISC_JOB_IP, DISC_JOB_ASSIGN, DISC_JOB_AUTOADD, DISC_JOB_TEST } disc_job_kind_t;

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
    char      url[512];             /* ASSIGN/AUTOADD/TEST: redacted */
    char      sub_url[512];         /* ASSIGN/TEST: redacted; "" none or derived */
    char      codec[8];             /* TEST: "H.264", "H.265" */
    int       width, height;        /* TEST: the main stream's picture */
} disc_result_t;

typedef struct {
    unsigned        id;             /* 0: none yet */
    disc_job_kind_t kind;
    int             running;
    int             dry_run;
    int             progress;       /* 0..100 */
    int             found;          /* SCAN: devices that answered it so far */
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

/* This recorder's own WS-Discovery endpoint reference ("urn:uuid:...",
 * the ONVIF server's; NULL or "" none): searches skip its answers. */
void disc_set_self(const char *uuid);

/* The same for its NETIP server (src/api/netip_srv.c): the serial and TCP
 * port its search answer gives ("" / 0: none). Any thread. */
void disc_set_self_netip(const char *serial, int port);

/* Preference order of protocols for a device's URL, "netip,onvif,dhip"
 * (the default: NETIP names both streams without guessing a path; ONVIF
 * asks the device; Dahua's RTSP path is fixed). -1 on an unknown name. */
int disc_set_prefer(const char *list);

/* Snapshot of the table (live entries), sorted by address; returns the
 * count. */
int disc_devices(disc_device_t *out, int max);

/* The 1-based channel whose camera URL points at `d`'s address; 0 none. */
int disc_device_channel(const disc_device_t *d);

/* The ONVIF device service a device at `ip` gave in its discovery answer
 * (while the table keeps it, DISC_TTL). 0 with `out` set, or -1. */
int disc_xaddr_of(const char *ip, char *out, size_t cap);

/* Forget every device. */
void disc_clear(void);

/* Moves whenever the table changes (a device heard for the first time or
 * telling something new, merged, dropped, a scan starting); a poller
 * takes a snapshot only when it moved. */
unsigned disc_generation(void);

/* The id of the last scan job (0: none yet); devices whose `scan` is not
 * this one have not answered it (yet). */
unsigned disc_last_scan(void);

/* Snapshot of the current (or last) job. */
void disc_job_get(disc_job_t *out);

/* ---- jobs: an id, or -1 (errno EBUSY: another job runs; EINVAL; EAGAIN:
 * disc_auth_wait) ------------------------------------------------------------ */

int disc_scan(const disc_scan_opts_t *o);

/* End a running scan now, keeping what it heard. Its id, or -1 if no scan
 * runs (another job is not stopped: an address change half done is
 * worse than one not started). */
int disc_scan_stop(void);

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

/* ---- a device entered by hand ------------------------------------------------- */

typedef enum { DISC_MANUAL_NETIP, DISC_MANUAL_ONVIF, DISC_MANUAL_DAHUA, DISC_MANUAL_RTSP } disc_manual_kind_t;

typedef struct {
    disc_manual_kind_t kind;
    char host[128];                 /* IPv4 address or host name */
    int  port;                      /* 0: the protocol's (34567, 80, 554, 554) */
    char user[64], pass[64];
    int  dev_channel;               /* 1-based channel of an NVR/DVR; 0 = 1 */
    char main_path[256];            /* RTSP: path ("/stream1") or a whole rtsp:// URL */
    char sub_path[256];             /* RTSP: the same for the sub-stream; "" none */
} disc_manual_t;

/* The protocol's default port: NETIP 34567, ONVIF 80, Dahua 554 (RTSP;
 * its 37777 is not used), RTSP 554. */
int disc_manual_port(disc_manual_kind_t kind);

/* "netip", "onvif", "dahua", "rtsp" <-> kind; -1 if unknown. */
int         disc_manual_kind(const char *name);
const char *disc_manual_name(disc_manual_kind_t kind);

/* The camera URLs for `m`: fixed forms for NETIP (sub-stream derived:
 * `sub` "") and Dahua, the paths for RTSP; ONVIF asks the device
 * (GetProfiles, GetStreamUri), a blocking call. */
disc_rc_t disc_manual_urls(const disc_manual_t *m, char *url, size_t ucap, char *sub, size_t scap, char *err,
                           size_t errcap);

/* Connect to `url` as a channel would (NETIP login and stream, RTSP
 * DESCRIBE, SETUP and PLAY) and wait up to `secs` for the first keyframe:
 * its codec and picture size. DISC_OK, or DISC_EAUTH / DISC_EUNREACHABLE /
 * DISC_ECODEC / DISC_ETIMEOUT (no video) / DISC_EPROTO with `err`, the
 * failure classification the tiles use. Blocking. */
disc_rc_t disc_test_url(const char *url, int secs, char codec[8], int *width, int *height, char *err,
                        size_t errcap);

/* Jobs: test `m` (its URLs, then the main stream: one result with codec
 * and size), and put `m` on `channel` (1-based) with `name`; a channel
 * that has a camera is refused unless `replace`. */
int disc_test_job(const disc_manual_t *m);
int disc_assign_manual_job(const disc_manual_t *m, int channel, const char *name, int replace);

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
char *disc_cameras_with(const char *cameras, int ch, const char *url, const char *sub_url, const char *name,
                        const char *onvif_url);

#endif /* DISCOVERY_H */
