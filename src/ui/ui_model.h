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

/* Events a camera reports now (each held a few seconds), as bits. */
enum { UI_EV_MOTION = 1u << 0, UI_EV_HUMAN = 1u << 1, UI_EV_FACE = 1u << 2, UI_EV_VEHICLE = 1u << 3 };

typedef struct {
    int      configured;        /* a camera is set for this channel */
    ui_ch_status_t status;
    int      live;              /* video is arriving */
    int      recording;
    unsigned events;            /* UI_EV_* */
    char     view[8];           /* the stream on screen: "main", "sub", "" */
    char     rec_state[24];     /* "" (not recording), "recording", "ready", "failed",
                                   "waiting for disk", "not a disk", "read-only", "error" */
    uint64_t rec_bytes;
    char     name[32];          /* "" : the UI shows "CAM n" */
    char     state[48];         /* not live: "CONNECTING" or the last error */
    char     codec[8];          /* "H.264", "H.265", "" */
    int      no_decoder;        /* live, but the platform has no decoder for it now */
    unsigned width, height, fps;
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
} ui_disc_dev_t;

typedef enum { UI_JOB_NONE, UI_JOB_SCAN, UI_JOB_IP, UI_JOB_ASSIGN, UI_JOB_AUTOADD } ui_job_kind_t;

typedef struct {
    char     key[24], ip[16], new_ip[16];
    unsigned proto;                 /* UI_DISC_* used */
    int      channel;
    int      done, ok, reprobed;
    char     msg[96];
} ui_disc_res_t;

typedef struct {
    unsigned      id;               /* 0: none yet */
    ui_job_kind_t kind;
    int           running, dry_run, progress;
    char          msg[96];
    int           n;
    ui_disc_res_t r[UI_DISC_MAX];
} ui_disc_job_t;

typedef struct {
    char key[24];
    char ip[16];                    /* "": the next free one from the start address */
} ui_disc_ip_t;

typedef struct ui_model {
    /* platform */
    const char *role;           /* "NVR" / "DVR" */
    int         channels;       /* tiles the platform can show */
    int         fixed_tiles;    /* no layouts, no full screen */

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

    /* Camera discovery (NULL when vigild has none). Each starting call
     * returns the job's id, or -1 with `err` (another job runs, too many
     * refused passwords: wait, bad values); disc_job shows the current or
     * last one. Credentials go to that job only. */
    int  (*disc_scan)(unsigned protos, char *err, size_t cap);
    int  (*disc_devices)(ui_disc_dev_t *out, int max);      /* by address */
    void (*disc_job)(ui_disc_job_t *job);
    int  (*disc_ip)(const ui_disc_ip_t *items, int n, const char *start, const char *mask, const char *gw,
                    const char *user, const char *pass, int dry_run, char *err, size_t cap);
    int  (*disc_assign)(const char *key, int channel, const char *user, const char *pass, int replace,
                        char *err, size_t cap);
    int  (*disc_autoadd)(const char *user, const char *pass, char *err, size_t cap);
} ui_model_t;

#endif /* UI_MODEL_H */
