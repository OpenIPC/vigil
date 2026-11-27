/* SPDX-License-Identifier: GPL-3.0-or-later */
/* config.h — vigild's settings: one YAML file and the store around it.
 *
 * The file (default /etc/vigil.yaml, -f FILE) is YAML, like majestic's:
 *
 *   version: 1
 *   cameras:
 *     - url: "rtsp://admin:12345@192.168.1.10/stream=0"
 *       name: Gate
 *       enabled: true
 *   record:
 *     path: /mnt/sda1/rec
 *     segment: 60
 *     keep: 0
 *   api:
 *     listen: "0.0.0.0:8080"
 *     auth: true
 *
 * and is written back with each setting's help text as a comment
 * (schema_yaml.h). A file whose name ends in ".json" is read and written
 * as JSON instead (the format of earlier versions). Inside vigild, and in
 * the API, the settings are JSON (cJSON) either way.
 *
 * Every setting is a row of config_schema (config_schema.c): defaults,
 * validation, the API's GET /api/v1/schema and redaction all come from it.
 *
 * The store holds the settings in force. Readers (the main loop, API
 * threads) take a snapshot with config_get(); writers (the API) replace a
 * section or the whole document with config_put_json(), which validates,
 * saves the file atomically and only then bumps the generation. The main
 * thread watches the generation and applies a new one (channels_apply).
 * Precedence: schema defaults < the file < command-line options; options
 * change the settings in force for that run (config_override) but are
 * written to the file only when the API saves the document.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

#include "channel.h"
#include "rec_sched.h"
#include "schema.h"

#define CONFIG_VERSION      1
#define CONFIG_DEFAULT_PATH "/etc/vigil.yaml"

typedef struct {
    char url[512];                  /* source.h's URL buffer */
    char sub_url[512];              /* "": derived, or none (channels_config_t) */
    char name[32];
    int  enabled;
} config_camera_t;

typedef struct {
    char path[192];                 /* seg_writer_t.dir; "" = not recording */
    int  segment;                   /* seconds */
    int  keep;                      /* segments per camera, 0 = all */
    int  allow_rootfs;              /* the root filesystem counts as a disk (rec_place.h) */
    char on_full[12];               /* "overwrite" | "stop" (seg_writer.h) */
} config_record_t;

/* A camera's week: one letter per half hour, 48 a day (rec_sched.h). */
#define CONFIG_DAY_SLOTS 48
typedef struct {
    int  camera;                    /* 1-based */
    char day[7][CONFIG_DAY_SLOTS + 1];  /* mon .. sun */
} config_sched_t;

typedef struct {
    int camera;                     /* 1-based */
    int max_gb;                     /* 0: no limit */
    int max_days;                   /* 0: no limit */
} config_quota_t;

typedef struct {
    char type[8];                   /* "none" | "cifs" | "nfs" */
    char server[64];
    char share[128];
    char user[64];
    char password[64];
    char options[128];
} config_nas_t;

typedef struct {
    int  enabled;
    char provider[12];              /* "dyndns" | "noip" | "custom" */
    char host[128];
    char user[64];
    char password[64];
    char url[256];                  /* custom: {host} {ip} {user} {password} */
    int  interval;                  /* minutes */
} config_ddns_t;

typedef struct {
    int enabled;
    int http_port;                  /* external ports to map; 0: none */
    int rtsp_port;
} config_upnp_t;

typedef struct {
    int  enabled;
    char server[128];
    int  port;
    char security[12];              /* "none" | "starttls" | "tls" */
    char user[64];
    char password[64];
    char from[128];
    char to[256];                   /* addresses, separated by commas */
} config_smtp_t;

typedef struct {
    int  enabled;
    char protocol[8];               /* "http" (JSON POST) */
    char host[128];
    int  port;
    char path[128];
    int  interval;                  /* heartbeat, seconds; 0: none */
} config_alarm_t;

typedef struct {
    int ntp;                        /* sync the clock from the servers in /etc/ntp.conf */
    int ntp_interval;               /* minutes between vigild's own checks */
} config_time_t;

typedef struct {
    char language[8];
    char date_format[12];           /* "YYYY-MM-DD" | "DD.MM.YYYY" | "MM/DD/YYYY" */
    char time_format[4];            /* "24h" | "12h" */
    int  login;                     /* the menus ask for a user (users.h) */
    int  idle_logout;               /* minutes without input that log out; 0: never */
} config_ui_t;

/* The HDMI/VGA output and what the live view draws over the video. */
typedef struct {
    char output[16];                /* "1920x1080@60" (config_output_mode) */
    char video_aspect[8];           /* "fit" | "stretch" */
    char clock_position[16];        /* "off" | "top-left" | "top-center" | ... | "bottom-right" */
    char clock_size[8];             /* "small" | "medium" | "large" | "xlarge" */
    char clock_background[8];       /* "outline" | "none" | "box" */
    char name_position[16];         /* "off" | a tile corner */
    char name_size[8];
    char name_background[8];
    char icons_position[16];        /* a tile corner */
    char icons_background[8];
} config_display_t;

typedef struct {
    char listen[64];                /* "addr:port", "port"; "" = no API */
    int  auth;
    char user[32];
    char shadow[192];
    char users[192];                /* vigil's own accounts (src/api/users.h) */
} config_api_t;

typedef struct {
    int  enabled;
    char listen[64];                /* "addr:port", "port"; "" = off */
    int  fps;                       /* at most this many pictures a second */
} config_web_t;

typedef struct {
    char interfaces[128];           /* "eth0,eth1"; "" = every IPv4 interface */
    char targets[256];              /* unicast addresses also searched */
    char prefer[32];                /* "netip,onvif,dhip" */
} config_discovery_t;

/* vigild as an ONVIF device (src/api/onvif.c, docs/onvif.md). */
typedef struct {
    int  enabled;
    int  port;                      /* its web service; WS-Discovery is UDP 3702 */
} config_onvif_server_t;

typedef struct {
    int             n_cameras;
    config_camera_t cameras[CHANNEL_MAX];
    config_record_t record;
    int             n_schedules;
    config_sched_t  schedule[CHANNEL_MAX];
    int             n_quotas;
    config_quota_t  quota[CHANNEL_MAX];
    config_nas_t    nas;
    config_ddns_t   ddns;
    config_upnp_t   upnp;
    config_smtp_t   smtp;
    config_alarm_t  alarm;
    config_time_t   time;
    config_ui_t     ui;
    config_display_t display;
    config_api_t    api;
    config_web_t    web;
    config_discovery_t discovery;
    config_onvif_server_t onvif_server;
} vigil_config_t;

/* "mon".."sun" */
extern const char *const config_day_keys[7];

extern const schema_t config_schema;

void config_defaults(vigil_config_t *c);

/* Pointers into `c` (which must outlive `cc`); a disabled camera keeps its
 * tile, with no source. */
/* Drop the empty camera slots at the end of the list (the ones between
 * cameras stay: they keep the later cameras on their channels). Done to
 * what config_put_json saves. */
void config_trim(vigil_config_t *c);

void config_to_channels(const vigil_config_t *c, channels_config_t *cc);

/* The cameras' weeks (record schedule) for rec_sched_set; returns the
 * number of entries (CHANNEL_MAX). A camera listed twice: the last wins. */
int config_to_schedule(const vigil_config_t *c, rec_week_t weeks[CHANNEL_MAX]);

/* display.output "1920x1080@60" as numbers; 0, or -1 when it is not one. */
int config_output_mode(const char *s, unsigned *w, unsigned *h, unsigned *hz);

/* ---- files ---------------------------------------------------------------------- */

typedef enum { CONFIG_YAML, CONFIG_JSON } config_format_t;

/* By the name: ".json" at the end is JSON, anything else YAML. */
config_format_t config_format(const char *path);

/* Whether `c` can be written in `fmt`: YAML takes UTF-8 text only (a
 * value loaded from a .json file may be other bytes). 0, or -1 with `err`
 * naming the setting. */
int config_check_text(const vigil_config_t *c, config_format_t fmt, char *err, size_t cap);

/* `c` as the text of a file in `fmt` (malloc'd, ends in a newline; NULL
 * when out of memory). Not redacted: the file holds the passwords. Check
 * it with config_check_text first. */
char *config_file_text(const vigil_config_t *c, config_format_t fmt);

/* Read `path` into `c`. Returns 1 if there is no such file (`c` holds the
 * defaults), 0 when loaded, -1 when it cannot be read or is invalid
 * (`c` holds the defaults; `errs` says why). */
int config_load_file(const char *path, vigil_config_t *c, schema_errors_t *errs);

/* Write `c` to `path` atomically, in the format its name says; returns
 * as config_write_atomic. */
int config_save_file(const char *path, const vigil_config_t *c, char *err, size_t cap);

/* Replace `path` with `data` so that a crash or power cut at any moment
 * leaves either the old file or the new one: write a new, exclusively
 * created PATH.XXXXXX (mode 0600, the file holds camera passwords), fsync
 * it, rename it over PATH, fsync the directory. Returns 0; -1 with `err`
 * when PATH is untouched; 1 with `err` when PATH was replaced but the
 * directory could not be synced (the rename may not survive a power cut). */
int config_write_atomic(const char *path, const char *data, size_t len, char *err, size_t cap);

/* ---- the store (thread-safe) ------------------------------------------------------------ */

/* Load `path` into the store and remember it for saves. A missing file
 * gives the defaults; an invalid one is logged field by field and the
 * defaults are used, the file left as it is (it is moved to PATH.bad when
 * a new configuration is saved over it; if no copy can be kept, nothing is
 * saved over it). Returns 0, -1 if invalid, -2 if `path` is too long to
 * keep (nothing loaded: refuse to run rather than use a shortened path). */
int config_init(const char *path);

/* Replace the settings in force without saving (command-line options at
 * startup). `c` must be valid (schema_validate). */
void config_override(const vigil_config_t *c);

/* Snapshot of the settings in force; returns their generation. */
unsigned config_get(vigil_config_t *c);
unsigned config_generation(void);

/* The main thread reports which generation runs (for the API's status). */
void     config_mark_applied(unsigned gen);
unsigned config_applied(void);

const char *config_path(void);

typedef enum {
    CONFIG_OK = 0,
    CONFIG_EINVAL,                  /* bad JSON or values: `errs` */
    CONFIG_ENOENT,                  /* no such section */
    CONFIG_EIO,                     /* not saved, nothing changed: errs->e[0] */
    CONFIG_EUNSYNCED,               /* the file was replaced and is in force, but
                                     * its directory sync failed: errs->e[0] */
    CONFIG_ESTALE,                  /* config_put_json_at: another write came
                                     * first; nothing changed */
} config_rc_t;

/* Replace one section (`section`) or the whole document (NULL) from JSON
 * text. Settings the JSON omits get their defaults. Unknown keys are
 * errors. A camera URL equal to the redacted form of a stored one (the
 * password shown as "***") keeps the stored URL. Saved before it takes
 * effect; nothing changes on EINVAL, ENOENT and EIO. */
config_rc_t config_put_json(const char *section, const char *body, size_t len,
                            schema_errors_t *errs);

/* The same, only if the settings in force are still generation `gen`
 * (0: any), checked under the writers' lock: a client that built `body`
 * from generation `gen` cannot overwrite a newer change it has not seen. */
config_rc_t config_put_json_at(const char *section, const char *body, size_t len, unsigned gen,
                               schema_errors_t *errs);

/* The settings in force as JSON text (malloc'd; NULL for an unknown
 * section). `redact` hides camera passwords. */
char *config_get_json(const char *section, int redact);

/* The schema as JSON text (malloc'd). */
char *config_schema_json(void);

#endif /* CONFIG_H */
