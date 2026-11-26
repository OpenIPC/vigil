/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_settings.h — the management pages' screen: a classic NVR settings
 * layout, and the widgets its sub-pages are made of.
 *
 *   +-------------------------------------------------------------------+
 *   | < Back   Network                         Cancel  Save    14:32:05 |
 *   +--------------+----------------------------------------------------+
 *   | > Ethernet   |  the sub-page: a form from the schema, or widgets  |
 *   |   Wi-Fi      |  of its own (a grid, a disk table, ...)            |
 *   |   LTE        |                                                    |
 *   |   ...        |                                                    |
 *   +--------------+----------------------------------------------------+
 *
 * The left pane lists the sub-pages (icon and label), the current one
 * highlighted; the arrows move in it, Enter opens one. The right pane is
 * the sub-page. Save and Cancel in the header belong to the sub-page
 * shown (hidden when it has nothing to save); leaving a sub-page, or the
 * screen, with unsaved changes asks first. The keyboard's order: Back,
 * the list, the sub-page, Cancel, Save.
 */
#ifndef UI_SETTINGS_H
#define UI_SETTINGS_H

#include "lvgl.h"
#include "ui_app.h"

typedef struct ui_set ui_set_t;

typedef struct {
    const char           *label;
    const lv_image_dsc_t *icon;
    /* Build the sub-page into `pane`; returns its state (passed to the
     * others), or NULL. Widgets that take the keyboard join
     * ui_set_group(s) in reading order. */
    void *(*open)(ui_set_t *s, lv_obj_t *pane);
    int   (*dirty)(void *st);       /* NULL: never */
    int   (*save)(void *st);        /* NULL: no Save; 0 saved */
    void  (*cancel)(void *st);      /* back to what is stored */
    void  (*close)(void *st);       /* free it (the widgets go with the pane) */
} ui_set_item_t;

/* Open the screen `title` with `n` sub-pages, showing `first`. The items
 * must outlive the screen (static tables). */
void ui_settings_open(const char *title, const ui_set_item_t *items, int n, int first);

/* The management row's screens (ui_set_*.c). `first`: the sub-page. */
void ui_network_open(int first);
void ui_storage_open(int first);
void ui_sysset_open(int first);
void ui_account_open(int first);

/* Storage's sub-pages, for the context menu's Record. */
enum { UI_STORAGE_BASIC, UI_STORAGE_SCHEDULE, UI_STORAGE_DISKS, UI_STORAGE_QUOTA, UI_STORAGE_NAS };

lv_group_t *ui_set_group(ui_set_t *s);
lv_obj_t   *ui_set_save_btn(ui_set_t *s);
lv_obj_t   *ui_set_kb(ui_set_t *s);
/* From open(): this sub-page has nothing to save now (no adapter, read
 * only): Save and Cancel are not shown. */
void        ui_set_hide_actions(ui_set_t *s);
/* Rebuild the sub-page shown (after a change it cannot show in place). */
void        ui_set_reopen(ui_set_t *s);

/* Section `key` of the model's settings, or -1. */
int ui_set_section_index(const char *key);
/* A form of the object section `key` inside `parent` (NULL if none). */
ui_form_t *ui_set_form(ui_set_t *s, lv_obj_t *parent, const char *key);

/* ---- building blocks for the sub-pages ---------------------------------------------- */

/* A titled group of rows (a panel); returns the panel to add rows to. */
lv_obj_t *ui_set_section(lv_obj_t *pane, const char *title);
/* A row: a label on the left (LABEL_W), the returned box on the right for
 * the widgets. `help` (may be NULL) goes under them. */
lv_obj_t *ui_set_row(lv_obj_t *sect, const char *label, const char *help);
/* A line of text (wrapped), in `color` (UI_C(TEXT2), UI_C(WARN), ...). */
lv_obj_t *ui_set_note(lv_obj_t *parent, const char *text, lv_color_t color);
/* "label   value" read-only rows; returns the value label. */
lv_obj_t *ui_set_info(lv_obj_t *sect, const char *label, const char *value);

/* Widgets that join the screen's group. A text field opens the
 * on-screen keyboard when clicked by a mouse user; Enter goes to the
 * next one. `w` 0: the row's width. */
lv_obj_t *ui_set_text(ui_set_t *s, lv_obj_t *box, const char *value, int max_len, int w);
lv_obj_t *ui_set_password(ui_set_t *s, lv_obj_t *box, int max_len, int w);
/* A number: - [ n ] + within min..max (Up/Down step it). */
lv_obj_t *ui_set_number(ui_set_t *s, lv_obj_t *box, int value, int min, int max);
int       ui_set_number_get(lv_obj_t *ta);
lv_obj_t *ui_set_switch(ui_set_t *s, lv_obj_t *box, int on);
/* A drop-down of '\n'-separated `options`, `sel` selected. */
lv_obj_t *ui_set_dropdown(ui_set_t *s, lv_obj_t *box, const char *options, int sel, int w);
lv_obj_t *ui_set_button(ui_set_t *s, lv_obj_t *box, const char *symbol, const char *text);
/* A row of widgets side by side (inside a row's box). */
lv_obj_t *ui_set_hbox(lv_obj_t *parent);

/* Follow job `id` (ui_mgmt_t.job) and call `done(user, ok, msg)` once it
 * ends; `label` (may be NULL) shows how it goes meanwhile. Cancelled when
 * `label` is deleted (or the screen closes). */
void ui_set_follow_job(ui_set_t *s, int id, lv_obj_t *label, void (*done)(void *user, int ok, const char *msg),
                       void *user);

/* "12.3 GB", "512 MB" */
void ui_set_size_text(unsigned long long bytes, char *out, size_t cap);
/* "2026-11-26 14:32" in the UI's date format, "" for 0. */
void ui_set_time_text(long long t, char *out, size_t cap);

#endif /* UI_SETTINGS_H */
