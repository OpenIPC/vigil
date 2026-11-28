/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_model.h — everything the UI shows or changes, as the app provides it.
 *
 * The UI (src/ui) knows LVGL and hal_ui.h only; it never includes the
 * core. The app fills a ui_model_t (src/app/ui_bridge.c) from the core's
 * snapshots and the configuration store, and the tests fill one with fake
 * channels. Every function is called on the UI (main) thread.
 *
 * Settings are described by the same table the HTTP API serves
 * (GET /api/v1/schema): sections of fields with a kind, limits, label,
 * help and whether a change applies at run time. Values travel as text:
 * integers in decimal, booleans "0"/"1", strings as they are; secret
 * fields (camera URLs) come redacted ("user:***@") and a value sent back
 * unchanged keeps the stored secret, as in the API.
 */
#ifndef UI_MODEL_H
#define UI_MODEL_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define UI_MAX_CHANNELS 64          /* the largest layout; platforms have fewer */
#define UI_ERRORS_MAX   16
#define UI_MAX_ITEMS    16          /* items of an array section */

/* ---- live view ------------------------------------------------------------------ */

/* What a channel is doing (the core's ch_state_t, in the same order). */
typedef enum {
    UI_CH_EMPTY = 0,            /* no camera in this slot: the tile offers to add one */
    UI_CH_DISABLED,
    UI_CH_CONNECTING,
    UI_CH_LIVE,
    UI_CH_NO_VIDEO,
    UI_CH_OFFLINE,              /* cannot be reached */
    UI_CH_AUTH,                 /* user or password refused */
    UI_CH_CODEC,                /* neither H.264 nor H.265 */
    UI_CH_ERROR,                /* `state` has the text */
} ui_ch_status_t;

/* Events a camera reports now (while they last, or a few seconds after a
 * moment's report), as bits; UI_EV_N of them. */
enum {
    UI_EV_MOTION = 1u << 0, UI_EV_HUMAN = 1u << 1, UI_EV_FACE = 1u << 2, UI_EV_VEHICLE = 1u << 3,
    UI_EV_LINE = 1u << 4, UI_EV_INTRUSION = 1u << 5, UI_EV_VIDEO_LOSS = 1u << 6, UI_EV_TAMPER = 1u << 7,
    UI_EV_ALARM_IN = 1u << 8, UI_EV_OTHER = 1u << 9,
    UI_EV_N = 10,
};

typedef struct {
    int      configured;        /* a camera is set for this channel */
    ui_ch_status_t status;      /* of the stream on screen (`view`) */
    int      live;              /* its video is arriving */
    /* Each stream on its own: the main one (recorded, large tiles) and the
     * sub-stream (small tiles; UI_CH_EMPTY while none runs). */
    ui_ch_status_t main_status, sub_status;
    char     main_state[48], sub_state[48];
    int      recording;
    unsigned events;            /* UI_EV_* */
    char     view[8];           /* the stream on screen: "main", "sub", "" */
    char     rec_state[24];     /* "" (not recording), "recording", "ready", "failed",
                                   "waiting for disk", "not a disk", "read-only", "error" */
    uint64_t rec_bytes;
    char     name[32];          /* the configured name, else the camera's; "": "CAM n" */
    char     title[32];         /* what the camera calls it (NETIP ChannelTitle), "" */
    char     state[48];         /* not live: "CONNECTING" or the last error */
    char     codec[8];          /* "H.264", "H.265", "" */
    int      no_decoder;        /* live, but the platform has no decoder for it now */
    unsigned width, height, fps;
    unsigned main_kbps, sub_kbps;   /* what each stream brings in (2 s average), 0: not running */
} ui_chan_t;

typedef struct {
    int    ch;                  /* 1-based; 0 = system */
    time_t time;
    char   text[40];
} ui_event_t;

typedef struct {
    int x, y, w, h;
} ui_rect_t;

/* ---- settings -------------------------------------------------------------------- */

typedef enum { UI_FIELD_STRING, UI_FIELD_INT, UI_FIELD_BOOL, UI_FIELD_ENUM } ui_field_kind_t;

typedef struct {
    const char         *key;
    const char         *label;
    const char         *help;
    ui_field_kind_t     kind;
    int                 min, max;   /* INT */
    int                 max_len;    /* STRING: bytes */
    const char *const  *values;     /* ENUM: NULL-terminated */
    const char         *def;        /* default, as text (new array items) */
    int                 secret;     /* shown redacted; "***" keeps the stored one */
    int                 live;       /* applies without restarting vigild */
    int                 required;
} ui_field_t;

typedef struct {
    const char       *key;
    const char       *label;
    const char       *help;
    int               is_array;     /* a list of items (cameras) */
    int               slots;        /* items are positions (channels): removing one
                                       empties its slot, one can be added anywhere */
    int               max_items;
    const ui_field_t *fields;
    int               n_fields;
} ui_section_t;

typedef struct {
    int  item;                  /* array item, or -1 */
    int  field;                 /* index in the section's fields, or -1 */
    char msg[200];              /* with the path when field is -1 */
} ui_error_t;

typedef struct {
    int        n, total;
    ui_error_t e[UI_ERRORS_MAX];
} ui_errors_t;

typedef enum {
    UI_SAVE_OK,
    UI_SAVE_INVALID,            /* nothing changed: errs says why, per field */
    UI_SAVE_FAILED,             /* not saved (I/O): errs->e[0] */
    UI_SAVE_UNSYNCED,           /* in force, but may not survive a power cut */
    UI_SAVE_STALE,              /* the settings changed since `gen`: nothing saved */
} ui_save_rc_t;

typedef struct {
    char label[32];
    char value[96];
} ui_info_t;

/* An output mode (HDMI/VGA): the UI's screen keeps its size, scaled to it. */
typedef struct {
    int w, h, hz;
} ui_mode_t;
#define UI_MODES_MAX 16

/* ---- camera discovery ------------------------------------------------------------- */

enum { UI_DISC_NETIP = 1u, UI_DISC_ONVIF = 2u, UI_DISC_DHIP = 4u };
#define UI_DISC_MAX 64

typedef struct {
    char     key[24];               /* what the jobs take: the MAC, else the address */
    char     ip[16], mac[18], mask[16], gateway[16];
    char     vendor[32], model[64], name[64];
    unsigned protos;                /* UI_DISC_* that answered */
    int      port;                  /* of the first protocol it would be added with */
    int      channel;               /* 1-based channel whose camera is this address; 0 none */
    int      stale;                 /* not heard by the last search (heard by one before) */
} ui_disc_dev_t;

typedef enum { UI_JOB_NONE, UI_JOB_SCAN, UI_JOB_IP, UI_JOB_ASSIGN, UI_JOB_AUTOADD, UI_JOB_TEST } ui_job_kind_t;

typedef struct {
    char     key[24], ip[16], new_ip[16];
    unsigned proto;                 /* UI_DISC_* used */
    int      channel;
    int      done, ok, reprobed;
    char     msg[96];
    char     error[16];             /* not ok: "auth", "unreachable", "codec", "unsupported",
                                       "timeout", "protocol", "invalid" */
    char     codec[8];              /* TEST: the main stream's */
    int      width, height;
} ui_disc_res_t;

typedef struct {
    unsigned      id;               /* 0: none yet */
    ui_job_kind_t kind;
    int           running, dry_run, progress;
    int           found;            /* SCAN: devices that answered it so far */
    char          msg[96];
    int           n;
    ui_disc_res_t r[UI_DISC_MAX];
} ui_disc_job_t;

typedef struct {
    char key[24];
    char ip[16];                    /* "": the next free one from the start address */
} ui_disc_ip_t;

/* A camera entered by hand (Channels -> Manual add). */
typedef enum { UI_MANUAL_NETIP, UI_MANUAL_ONVIF, UI_MANUAL_DAHUA, UI_MANUAL_RTSP } ui_manual_kind_t;

typedef struct {
    ui_manual_kind_t kind;
    char host[128];                 /* address or host name */
    int  port;                      /* 0: the protocol's */
    char user[64], pass[64];
    int  dev_channel;               /* 1-based, of an NVR/DVR */
    char main_path[256], sub_path[256];     /* RTSP: paths or whole URLs */
} ui_manual_t;


/* ---- management pages (NULL members: not there in this build) ------------------- */

/* Network, time, disks, accounts: what src/sys and the accounts file do,
 * as plain data. Calls that start something slow return a job id (> 0)
 * to poll with job(); a value <= 0 is an error with `err` (UI_MGMT_*). */
enum { UI_MGMT_EINVAL = -1, UI_MGMT_EUNMANAGED = -2, UI_MGMT_EBUSY = -3, UI_MGMT_EFAIL = -4 };
enum { UI_JOBST_QUEUED, UI_JOBST_RUNNING, UI_JOBST_DONE, UI_JOBST_FAILED };

typedef struct {
    int  dhcp;
    char address[16], netmask[16], gateway[16], dns1[16], dns2[16];
    int  mtu;
} ui_net_cfg_t;

typedef struct {
    int  present, up, link, speed;
    char duplex[8], mac[18];
    int  mtu;
    char address[16], netmask[16], gateway[16], dns1[46], dns2[46];
} ui_net_status_t;

/* A USB Wi-Fi adapter or modem plugged in, and the firmware's profile
 * (U-Boot wlandev) that loads its driver. */
typedef struct {
    char id[10], name[64], driver[32], profile[48];
    int  bound;
} ui_radio_t;

typedef struct {
    char       ifname[16];          /* "": no wireless interface (yet) */
    char       ssid[64];
    int        password_set, can_scan;
    char       profile[48];         /* wlandev */
    ui_radio_t adapters[4];
    int        n_adapters;
} ui_wifi_t;

typedef struct {
    char ssid[64];
    int  signal, secured;
} ui_wifi_net_t;

typedef struct {
    char       ifname[16], driver[32], profile[48];
    ui_radio_t modems[4];
    int        n_modems;
} ui_lte_t;

typedef struct {
    int  id, state;                 /* UI_JOBST_* */
    char kind[24];
    char msg[256];
} ui_mjob_t;

/* Operation -> Configuration (src/api/backup.h): where an export goes and
 * the exports found on the disks. */
typedef struct {
    char path[128];
    char label[128];
} ui_cfg_place_t;

typedef struct {
    char      path[256];
    char      name[100];
    long long size, mtime;
} ui_cfg_file_t;

/* The outcome of an export, an import (checked or applied) or a reset. */
enum { UI_CFG_OK = 0, UI_CFG_EINVAL = -1, UI_CFG_EPASSWORD = -2, UI_CFG_ECHANGED = -3, UI_CFG_EVERSION = -4,
       UI_CFG_EIO = -5, UI_CFG_EUNMANAGED = -6, UI_CFG_EBUSY = -7 };
#define UI_CFG_LINES 24
typedef struct {
    int  rc;                        /* UI_CFG_* */
    char msg[300];                  /* why not; or what was done */
    char path[256];                 /* an export: the file written */
    int  secrets;                   /* an import: the export has its passwords, sealed */
    int  applied, reboot;           /* applied; the network settings wait for a reboot */
    int  net_cuts;                  /* the recorder's address may change at that reboot */
    int  n_lines;
    int  kind[UI_CFG_LINES];        /* 0 information, 1 warning */
    char line[UI_CFG_LINES][200];
} ui_cfg_result_t;

typedef struct {
    long long last;                 /* 0: never */
    int       ok;
    char      result[160];
} ui_svc_t;

typedef struct {
    char name[32], parent[32];
    int  is_part;
    unsigned long long size;
    char model[64];
    int  removable, ro;
    char fs[16], mnt[128];
    unsigned long long total, avail;
    int  system;
    char state[24];
} ui_disk_t;

/* Disk Maintenance: a disk's SMART (sys_smart.h), as the page shows it. */
enum { UI_SMART_GOOD, UI_SMART_WARN, UI_SMART_FAIL, UI_SMART_UNKNOWN };
#define UI_SMART_ATTRS 30
#define UI_SMART_TESTS 8

typedef struct {
    int  id, value, worst, thresh;
    int  level;                     /* UI_SMART_GOOD / WARN / FAIL */
    int  critical;
    char name[32], raw[40], note[96];
} ui_smart_attr_t;

typedef struct {
    char type[24], status[48];
    int  hours, failed;
    unsigned long long lba;
} ui_smart_test_t;

typedef struct {
    int  available;                 /* the drive answered */
    char transport[24], model[44], serial[24], firmware[12];
    unsigned long long bytes;
    int  rpm;                       /* 0 unknown, 1 solid state */
    int  sata_max, sata_cur;        /* SATA generation 1..3, 0 unknown */
    int  smart_enabled;
    int  health;                    /* the drive's own check: 1 passed, 0 failing, -1 unknown */
    int  temp;                      /* °C, -1 */
    long hours;                     /* power on, -1 */
    int  verdict;                   /* UI_SMART_* */
    char reason[200];
    int  test_supported, test_running, test_remaining, short_min, ext_min;
    char test_status[48];
    int  n_attrs;
    ui_smart_attr_t attrs[UI_SMART_ATTRS];
    int  n_tests;                   /* newest first */
    ui_smart_test_t tests[UI_SMART_TESTS];
    long long read_at;
} ui_smart_t;

/* The surface scan (sys_scan.h): one at a time, in vigild. */
enum { UI_SCAN_READ, UI_SCAN_VERIFY, UI_SCAN_WRITE };
enum { UI_SCAN_IDLE, UI_SCAN_RUNNING, UI_SCAN_PAUSED, UI_SCAN_DONE, UI_SCAN_STOPPED, UI_SCAN_FAILED };
#define UI_SCAN_CLASSES 7           /* < 5, < 20, < 50, < 150, < 500, >= 500 ms, error */
#define UI_SCAN_MAP     2000
#define UI_SCAN_BAD     16

typedef struct {
    int    state, mode, read_instead;
    char   disk[32], model[64], serial[24];
    unsigned long long start, end, lba, blocks_done, blocks_total;
    unsigned long long counts[UI_SCAN_CLASSES];
    unsigned block, sector;
    int    pass, passes;
    double mbps;
    long   elapsed, eta;            /* s; eta -1 unknown */
    char   msg[200];
    int    n_bad;
    unsigned long long bad_more;    /* ranges not listed */
    unsigned long long bad_lba[UI_SCAN_BAD], bad_count[UI_SCAN_BAD];
    int    cells;
    unsigned char map[UI_SCAN_MAP]; /* 0 not yet, 1 + class */
} ui_scan_t;

/* A time zone as fields (minutes east of UTC; rules: month 1..12, week
 * 1..5 (5 = last), weekday 0..6 (0 = Sunday), minutes after midnight). */
typedef struct {
    int std_offset, dst, dst_offset;
    int start_month, start_week, start_wday, start_min;
    int end_month, end_week, end_wday, end_min;
} ui_tz_rule_t;

typedef struct {
    char      zone[64], tz[96];
    int       rtc;
    char      servers[4][64];
    int       n_servers;
    int       ntp, ntp_interval;
    long long ntp_last_try, ntp_last_ok;
    char      ntp_result[160];
} ui_time_t;

typedef struct {
    char name[32], group[32];
    int  enabled, password_set;
} ui_user_t;

typedef struct {
    char     name[32];
    unsigned perms;
    int      builtin, editable, members;
} ui_group_t;

/* One of vigild's servers (Network -> Ports): what it listens on now
 * ("": nothing), and why not, or the last move to another port that
 * failed (""). */
enum { UI_SERVER_WEB, UI_SERVER_API, UI_SERVER_ONVIF, UI_SERVER_RTSP, UI_SERVER_NETIP };
typedef struct {
    int  on;
    char listen[64];
    char why[200];
} ui_listen_t;

/* Permissions, users.h's PERM_* bits (the bridge asserts they agree). */
enum {
    UI_PERM_LIVE = 1u << 0, UI_PERM_PLAYBACK = 1u << 1, UI_PERM_PTZ = 1u << 2, UI_PERM_CAMERAS = 1u << 3,
    UI_PERM_STORAGE = 1u << 4, UI_PERM_NETWORK = 1u << 5, UI_PERM_SYSTEM = 1u << 6, UI_PERM_USERS = 1u << 7,
    UI_PERM_REMOTE = 1u << 8,
};

typedef struct {
    int (*managed)(void);           /* vigild may change this system */

    /* Ethernet; eth_apply: the change waits for net_confirm (or is undone) */
    void (*eth_get)(ui_net_cfg_t *cfg, ui_net_status_t *st);
    int  (*eth_apply)(const ui_net_cfg_t *cfg, char *field, size_t fcap, char *err, size_t cap);
    int  (*net_pending)(int *secs_left);    /* 1 while a change waits */
    int  (*net_confirm)(void);
    int  (*net_revert)(void);
    void (*wifi_get)(ui_wifi_t *w, ui_net_status_t *st);
    /* A scan runs in the background (iw takes up to 20 s): wifi_scan_start
     * starts one (0, or < 0 with err; one already running is joined), and
     * wifi_scan says UI_MGMT_EBUSY while it runs, then gives its networks
     * (>= 0) or < 0 with err. */
    int  (*wifi_scan_start)(char *err, size_t cap);
    int  (*wifi_scan)(ui_wifi_net_t *out, int max, char *err, size_t cap);
    /* `profile` ("" or NULL: as it is) becomes wlandev; a job id */
    int  (*wifi_set)(const char *ssid, const char *password, const char *profile, char *err, size_t cap);
    void (*lte_get)(ui_lte_t *l, ui_net_status_t *st);
    int  (*lte_profile)(const char *profile, char *msg, size_t cap);   /* 0, or < 0 with msg */

    /* services: the last DDNS update, mail, alarm post, network storage mount */
    void (*services)(ui_svc_t *ddns, ui_svc_t *smtp, ui_svc_t *alarm, ui_svc_t *nas);
    int  (*ddns_update)(void);
    int  (*smtp_test)(void);
    int  (*nas_test)(void);
    int  (*nas_support)(const char *type, char *why, size_t cap);  /* 1, or 0 with why */
    int  (*nas_mounted)(void);
    int  (*curl)(void);             /* mail and DDNS can work */

    int  (*job)(int id, ui_mjob_t *j);              /* 0, or -1 when forgotten */

    /* configuration export and import, the resets (NULL: not in this
     * build). Each of cfg_export, cfg_import and cfg_reset starts the
     * work on a thread of its own (the key derivation alone takes a
     * second): 0, or < 0 with err; cfg_poll says UI_MGMT_EBUSY while it
     * runs, then gives the outcome once (0). cfg_import with apply 0 only
     * checks and summarises. cfg_reset: level 0 vigil's settings (network
     * 1: the interfaces too, and a reboot), 1 the whole firmware. */
    int (*cfg_places)(ui_cfg_place_t *out, int max);
    int (*cfg_files)(ui_cfg_file_t *out, int max);
    int (*cfg_export)(const char *dir, const char *password, char *err, size_t cap);
    int (*cfg_import)(const char *path, const char *password, int hostname, int apply, char *err, size_t cap);
    int (*cfg_reset)(int level, int network, char *err, size_t cap);
    int (*cfg_poll)(ui_cfg_result_t *r);

    /* disks */
    int  (*disks)(ui_disk_t *out, int max);
    int  (*disk_formats)(char out[][8], int max);
    int  (*disk_format)(const char *name, const char *fs, const char *confirm, char *err, size_t cap);
    int  (*disk_mount)(const char *name, char *err, size_t cap);
    int  (*disk_unmount)(const char *name, char *err, size_t cap);

    /* disk maintenance: SMART (smart_read is a job; smart_get the last
     * read, 0, or -1 before one), self-tests (1 short, 2 extended, 0
     * abort), the surface scan (scan_start 0 or < 0 with err; Write
     * needs `confirm` = the disk's name) */
    int  (*smart_read)(const char *disk, char *err, size_t cap);
    int  (*smart_get)(const char *disk, ui_smart_t *out);
    int  (*selftest)(const char *disk, int type, char *err, size_t cap);
    int  (*scan_start)(const char *disk, int mode, unsigned long long start, unsigned long long end,
                       unsigned block, const char *confirm, char *err, size_t cap);
    void (*scan_get)(ui_scan_t *out);
    int  (*scan_pause)(int on);
    int  (*scan_stop)(void);

    /* time and name */
    void        (*time_get)(ui_time_t *t);
    int         (*zones)(void);                     /* how many named zones */
    const char *(*zone_name)(int i);
    const char *(*zone_tz)(int i);          /* its POSIX TZ string */
    int         (*tz_parse)(const char *posix, ui_tz_rule_t *r);
    int         (*tz_build)(const ui_tz_rule_t *r, char *out, size_t cap, char *err, size_t ecap);
    int         (*time_set)(const char *zone, const char *tz, int set_clock, long long t, char *err, size_t cap);
    int         (*ntp_set)(int enabled, const char *servers, int interval, char *err, size_t cap);
    int         (*ntp_sync)(void);
    void        (*hostname_get)(char *buf, size_t cap);
    int         (*hostname_set)(const char *name, char *err, size_t cap);

    /* accounts (users.h); errors name the field ("name", "password", ...) */
    unsigned           (*users_gen)(void);
    int                (*users)(ui_user_t *out, int max);
    int                (*groups)(ui_group_t *out, int max);
    const char *const *perm_names;  /* n_perms of them: bit i */
    int                n_perms;
    int (*user_add)(const char *name, const char *group, const char *pass, int enabled, char *field,
                    size_t fcap, char *err, size_t cap);
    /* group NULL, enabled < 0, pass NULL: unchanged */
    int (*user_update)(const char *name, const char *group, int enabled, const char *pass, char *field,
                       size_t fcap, char *err, size_t cap);
    int (*user_delete)(const char *name, char *err, size_t cap);
    int (*group_set)(const char *name, unsigned perms, int create, char *field, size_t fcap, char *err,
                     size_t cap);
    int (*group_delete)(const char *name, char *err, size_t cap);
    /* the system account's password (root's, also the console and SSH
     * login), given its current one ("current" names it when wrong) */
    int (*system_password)(const char *current, const char *pass, char *field, size_t fcap, char *err,
                           size_t cap);

    /* the on-screen login: 1 with *perms, 0 wrong user or password, -1
     * when it cannot be checked (`why`) */
    int  (*login)(const char *name, const char *pass, unsigned *perms, char *why, size_t cap);
    /* the system account (api.user), which always logs in */
    void (*system_user)(char *buf, size_t cap);
    /* Exit: reboot (1) or power off (0), after vigild stops cleanly; 0, or
     * < 0 on a system vigild does not manage */
    int  (*power)(int reboot);
    /* UI_SERVER_*; NULL: not known in this build */
    void (*listening)(int server, ui_listen_t *out);
} ui_mgmt_t;

typedef struct ui_model {
    /* platform */
    const char *role;           /* "NVR" / "DVR" */
    int         channels;       /* tiles the platform can show */
    int         fixed_tiles;    /* no layouts, no full screen */
    unsigned    max_in_kbps;    /* the chip's rated incoming bandwidth, kbit/s; 0: not known */

    /* live view */
    void   (*chan_status)(int idx, ui_chan_t *st);
    int    (*next_event)(ui_event_t *ev);           /* 1 and `ev`, 0 when none */
    /* Show channel i in tiles[i] (screen pixels; w == 0: hidden), n == channels. */
    void   (*show_tiles)(const ui_rect_t *tiles, int n);
    time_t (*now)(void);

    /* settings */
    const ui_section_t *sections;
    int                 n_sections;
    unsigned (*config_generation)(void);
    int      (*item_count)(int sec);
    /* Field `field` of item `item` (-1 for an object section) as text. */
    void     (*get_value)(int sec, int item, int field, char *buf, size_t cap);
    /* Replace section `sec` with `values` (n_items x n_fields strings, item
     * by item; n_items is ignored for an object section) through the
     * configuration store, as PUT /api/v1/config/<section> does, but only
     * if the settings are still generation `gen` (the one the values were
     * read from). `origin[k]` is the stored item that item k was read from
     * (-1: new; NULL for an object section): an unchanged redacted secret
     * keeps that item's own secret, even when another item's redacts the
     * same way. */
    ui_save_rc_t (*save)(int sec, int n_items, const char *const *values, const int *origin,
                         unsigned gen, ui_errors_t *errs);

    /* system page */
    int (*system_info)(ui_info_t *info, int max);

    /* The output: the modes the board can switch to (0: no choice), the
     * one in force, and a switch now, not saved (0, or -1 with the old one
     * still in force): the Display page keeps it by saving display.output,
     * or switches back. NULL: none. */
    int  (*out_modes)(ui_mode_t *out, int max);
    void (*out_mode)(ui_mode_t *cur);
    int  (*out_set)(const ui_mode_t *m);

    /* Camera discovery (NULL when vigild has none). Each starting call
     * returns the job's id, or -1 with `err` (another job runs, too many
     * refused passwords: wait, bad values); disc_job shows the current or
     * last one. Credentials go to that job only. */
    int  (*disc_scan)(unsigned protos, char *err, size_t cap);
    int  (*disc_devices)(ui_disc_dev_t *out, int max);      /* by address */
    /* Moves whenever disc_devices would answer something new (a scan
     * fills the table as devices answer); NULL: ask disc_devices on every
     * poll while a scan runs. */
    unsigned (*disc_gen)(void);
    /* End the running scan now, keeping what it found; -1 if none runs. */
    int  (*disc_stop)(void);
    void (*disc_job)(ui_disc_job_t *job);
    int  (*disc_ip)(const ui_disc_ip_t *items, int n, const char *start, const char *mask, const char *gw,
                    const char *user, const char *pass, int dry_run, char *err, size_t cap);
    int  (*disc_assign)(const char *key, int channel, const char *user, const char *pass, int replace,
                        char *err, size_t cap);
    int  (*disc_autoadd)(const char *user, const char *pass, char *err, size_t cap);
    /* A camera entered by hand: test it (one result: codec and size, or
     * why not), or put it on `channel` (1-based). Jobs as above. */
    int  (*disc_test)(const ui_manual_t *m, char *err, size_t cap);
    int  (*disc_assign_manual)(const ui_manual_t *m, int channel, const char *name, int replace, char *err,
                               size_t cap);

    /* The management pages (NULL: none in this build). */
    const ui_mgmt_t *mgmt;

    /* The output's adjustments (Display -> Output): what the board has
     * (UI_OUT_* bits; 0 or NULL: none), and each put in force now, not
     * saved, for the page's preview (display.margin_*, display.picture,
     * display.hdmi_volume and hdmi_mute save them). 0, or -1. */
    unsigned (*out_adj_caps)(void);
    int  (*out_margins)(int top, int bottom, int left, int right);     /* % of the output */
    int  (*out_picture)(const char *preset);
    int  (*out_volume)(int volume, int mute);
} ui_model_t;

enum { UI_OUT_MARGINS = 1u, UI_OUT_PICTURE = 2u, UI_OUT_AUDIO = 4u };

#endif /* UI_MODEL_H */
