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

typedef struct {
    int      configured;        /* a camera is set for this channel */
    int      live;              /* video is arriving */
    int      recording;
    int      motion;            /* the camera reports motion */
    char     rec_state[24];     /* "" (not recording), "recording", "ready", "failed",
                                   "waiting for disk", "not a disk", "read-only", "error" */
    uint64_t rec_bytes;
    char     name[32];          /* "" : the UI shows "CAM n" */
    char     state[48];         /* not live: "CONNECTING" or the last error */
    char     codec[8];          /* "H.264", "H.265", "" */
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
} ui_model_t;

#endif /* UI_MODEL_H */
