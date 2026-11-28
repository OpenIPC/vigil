/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_logs.h — Operation -> Logs: the system log, the alarm log and the
 * operation log, each a page of the settings screen (ui_settings.h):
 *
 *   filters    time (today, 24 h, 7 days, all, from/to), type or action,
 *              channel / source / level, a text search (the on-screen
 *              keyboard for a mouse user)
 *   table      50 rows a page, newest first; the rows are made once and
 *              refilled, so a log of any length costs the same LVGL
 *              memory. Enter or a click on a row: the whole entry.
 *   pager      Newer / Older; the first page follows what comes in.
 *   Export     the log with the filters to a disk or USB stick, CSV or
 *              text, on a thread of the app's (a long log takes a while).
 *
 * What it shows comes from the app (src/app/ui_logs_bridge.c), not the
 * core, like the rest of the UI (ui_model.h); the tests set their own
 * source. Needs the `system` permission (the Operation card's). LVGL
 * thread only. */
#ifndef UI_LOGS_H
#define UI_LOGS_H

#include <stddef.h>

#include "lvgl.h"

typedef enum { UI_LOG_SYSTEM = 0, UI_LOG_ALARMS, UI_LOG_OPS, UI_LOG_KINDS } ui_log_kind_t;

typedef struct {
    long long time;                 /* 0: not known */
    int       ch;                   /* alarms: 1-based camera, 0 none */
    int       level;                /* system: 0 emerg .. 7 debug, -1 none */
    int       active;               /* alarms: 1 start, 0 stop, -1 neither */
    char      who[32];              /* operations: the user; system: the tag */
    char      source[16];           /* operations: screen, web, api, ... */
    char      type[32];             /* alarms: the type; operations: the action */
    char      detail[256];
} ui_log_row_t;

typedef struct {
    long long from, to;             /* 0: open */
    int       ch;                   /* alarms: -1 any */
    int       level;                /* system: -1 any, else that and more urgent */
    int       kernel;               /* system: dmesg instead of syslog */
    char      type[32];             /* "" any */
    char      source[16];           /* operations: "" any */
    char      text[64];             /* "" anything */
} ui_log_filter_t;

typedef struct ui_logs_src {
    /* A page: rows after `offset` matches, at most `limit`, newest first;
     * how many, *more when there are others; -1 with `why` when the log
     * cannot be read. */
    int (*query)(int kind, const ui_log_filter_t *f, int offset, int limit, ui_log_row_t *out, int *more,
                 char *why, size_t cap);
    /* Where the log is kept, in a few words. */
    void (*where)(int kind, char *out, size_t cap);
    /* Moves when something new is in the log (alarms, operations). */
    unsigned long (*count)(int kind);
    /* The type (alarms) or action (operations) filter's choices. */
    int (*types)(int kind, const char **out, int max);
    /* Where an export may go: mounted disks and sticks; how many. */
    int (*export_targets)(char out[][96], int max);
    /* Export `kind` with `f` into directory `dir` as CSV (`csv`) or text:
     * started (0), or -1 with `err` (one runs already, ...). */
    int (*export_start)(int kind, const ui_log_filter_t *f, int csv, const char *dir, char *err, size_t cap);
    /* The export: 1 while it runs; else 0 and, once after it ended, *done
     * set with the entries written (or -1) and the file or the error. */
    int (*export_poll)(int *done, long *count, char *msg, size_t cap);
    /* The screen did something the operation log keeps (a logout). */
    void (*audit)(const char *action, const char *detail);
} ui_logs_src_t;

/* The source (it must outlive the UI); NULL: the page says there is none. */
void ui_logs_set_source(const ui_logs_src_t *src);
const ui_logs_src_t *ui_logs_source(void);

/* The Logs screen, on `first` (ui_log_kind_t). */
void ui_logs_open(int first);

/* For another settings screen (ui_settings.h) that lists the logs among
 * its entries, as Operation may, an ui_set_item_t's open and close: one
 * log as a sub-page (ui_logs_page_open, closed by ui_logs_close), or all
 * three in one sub-page with System / Alarm / Operation log buttons on
 * top, showing `first` (ui_logs_pane_open, closed by
 * ui_logs_pane_close). */
struct ui_set;
void *ui_logs_page_open(struct ui_set *s, lv_obj_t *pane, int kind);
void  ui_logs_close(void *st);
void *ui_logs_pane_open(struct ui_set *s, lv_obj_t *pane, int first);
void  ui_logs_pane_close(void *st);

/* Tell the operation log what the screen did (ui_session.c's logout). */
void ui_logs_audit(const char *action, const char *detail);

#endif /* UI_LOGS_H */
