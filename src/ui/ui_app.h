/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_app.h — vigild's on-screen application: a stack of screens on top of
 * the live view.
 *
 *   live view (ui_live.c)   tiles over the video, header, event strip,
 *                           auto-hiding navigation bar, context menu
 *     -> main menu (ui_menu.c)
 *        -> settings: one page per schema section (ui_form.c), the
 *           camera list and a camera's form; system (ui_system.c)
 *
 * Back is the header's back button, the right mouse button or Esc; on the
 * live view the right button opens the context menu. Every screen and
 * popup has an lv_group for the keyboard (LVGL keypad): Tab and the arrows
 * move the focus (lv_gridnav in grids and menus), Enter activates, and the
 * shortcuts are in ui_app_key (docs/ui.md lists them). State comes from the
 * model (ui_model.h): channel snapshots, events and the configuration
 * generation are polled on a timer and published as lv_subjects that the
 * widgets observe. LVGL thread (the main thread) only.
 */
#ifndef UI_APP_H
#define UI_APP_H

#include "lvgl.h"
#include "ui_keys.h"
#include "ui_model.h"

/* ---- for the app --------------------------------------------------------------- */

/* Build the UI on the default display (after hal_ui_create). `m` must
 * outlive the UI. */
void ui_app_create(const ui_model_t *m);

/* The right mouse button: close a popup, go back one screen, or on the
 * live view open the context menu at the pointer. */
void ui_app_back(void);

/* The pointer device (for the navigation bar's bottom edge and where the
 * context menu opens); NULL: none. */
void ui_app_set_pointer(lv_indev_t *indev);

/* The keypad (keyboards); NULL: none. */
void ui_app_set_keypad(lv_indev_t *indev);

/* A key from the keyboard (ui_keys.h): a shortcut is started (after the
 * current input processing) and 0 returned, else the key LVGL's keypad
 * gets. The first key shows the focus and stops the on-screen keyboard
 * opening on text fields. */
uint32_t ui_app_key(uint32_t key, unsigned mods);

/* `n` keyboards are plugged in; with none, the UI is for the mouse again
 * (no focus shown, the on-screen keyboard back). */
void ui_app_keyboards(int n);

/* A keyboard has been used (and is still plugged in). */
int ui_keyboard_in_use(void);

/* Poll the model now instead of at the next refresh tick (tests). */
void ui_app_refresh(void);

/* ---- between the screens ------------------------------------------------------------- */

const ui_model_t *ui_model(void);

/* Snapshots, published as subjects: one pointer subject per channel (its
 * ui_chan_t; notified when it changes), the configuration generation (int),
 * the clock ("YYYY-MM-DD HH:MM:SS"), and whether the navigation bar is
 * pinned (int). */
lv_subject_t    *ui_subject_channel(int ch);
const ui_chan_t *ui_channel(int ch);
extern lv_subject_t ui_subj_config;
extern lv_subject_t ui_subj_clock;
extern lv_subject_t ui_subj_nav_pinned;

/* A page: a screen with a header (back button, title, actions on the
 * right) and a body below it. */
typedef struct ui_page ui_page_t;
struct ui_page {
    lv_obj_t *scr;
    lv_group_t *group;              /* the keyboard's focus order: Back, the body, actions */
    lv_obj_t *save;                 /* Ctrl+S clicks it (NULL: none) */
    lv_obj_t *header;
    lv_obj_t *title;
    lv_obj_t *actions;              /* header, right: add buttons here */
    lv_obj_t *body;                 /* below the header, scrolls */
    /* Back was asked for: return 1 when handled here (a keyboard closed, a
     * "discard changes?" question asked), 0 to close the page. */
    int  (*on_back)(ui_page_t *p);
    void (*on_close)(ui_page_t *p); /* free `user` */
    void *user;
};

ui_page_t *ui_page_push(const char *title);
/* A page without the header: `body` is the whole screen, laid out by the
 * caller (the main menu). */
ui_page_t *ui_page_push_plain(void);
/* The page is built: its actions join the focus order after the body
 * (whose widgets the builder added to p->group), and with a keyboard in
 * use the focus starts on the first of them. */
void ui_page_ready(ui_page_t *p);
/* Close the top page (it must be `p`) and show the one below. */
void ui_page_close(ui_page_t *p);
ui_page_t *ui_page_top(void);       /* NULL on the live view */

typedef enum { UI_TOAST_OK, UI_TOAST_WARN, UI_TOAST_BAD } ui_toast_t;
void ui_toast(ui_toast_t kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* A modal question on top of everything: `on_yes(user)` if confirmed. */
void ui_confirm(const char *text, const char *yes, void (*on_yes)(void *), void *user);

/* One popup at a time (context menu, question) on the top layer, closed by
 * a click outside it or by back. ui_popup_open returns the panel to fill;
 * it is placed at `at` (kept on screen) or centred when `at` is NULL. */
lv_obj_t *ui_popup_open(const lv_point_t *at, int w);
void      ui_popup_place(const lv_point_t *at);    /* after it is filled */
void      ui_popup_close(void);
int       ui_popup_is_open(void);
/* The keyboard moves among `cont`'s clickable children with the arrows
 * (lv_gridnav with `ctrl`), starting at `first` (NULL: the first). */
void      ui_popup_focus(lv_obj_t *cont, lv_obj_t *first, lv_gridnav_ctrl_t ctrl);
/* A submenu: a second panel beside `item` (an item of the popup's panel),
 * filled by the caller; ui_popup_sub_place then places it and gives it
 * the keyboard, on `first` (NULL: its first item). Up and Down move in
 * it; back (Esc, Left, the right button) closes it before the popup. */
lv_obj_t *ui_popup_sub(lv_obj_t *item, int w);
void      ui_popup_sub_place(lv_obj_t *first);
void      ui_popup_sub_close(void);
int       ui_popup_sub_is_open(void);
/* "<what> is not available yet": what a menu item or a card of a feature
 * vigil does not have yet (playback, PTZ, ...) opens. */
void      ui_not_yet(const char *what);
/* The keyboard shortcuts (F1 or ?); open again: closed. */
void      ui_help_toggle(void);

/* Arrow keys go to this object (a gridnav container, a number field)
 * rather than moving the focus. */
#define UI_FLAG_ARROWS LV_OBJ_FLAG_USER_1
/* Make `o` a gridnav container that takes the arrows, its clickable
 * children the items. */
void      ui_gridnav(lv_obj_t *o, lv_gridnav_ctrl_t ctrl);
/* Make `item` the gridnav container `cont`'s current item (marked
 * focused only while `cont` has the focus). */
void      ui_gridnav_focus(lv_obj_t *cont, lv_obj_t *item);
/* Not clickable, so not an item of a gridnav container (separators,
 * plain rows). */
void      ui_inert(lv_obj_t *o);

/* The pointer's position, or 0 when there is no pointer. */
int ui_pointer(lv_point_t *p);

/* Screens. */
lv_obj_t *ui_live_create(void);
lv_group_t *ui_live_group(void);
/* A key on the live view with nothing over it: 1 when it was a shortcut
 * (started after the current input processing). */
int       ui_live_key(uint32_t key, unsigned mods);
void      ui_live_context_menu(const lv_point_t *at);
void      ui_live_event(const ui_event_t *ev);
void      ui_menu_open(void);
void      ui_form_open(int sec, int item);  /* item -1: object section; n: new item */
void      ui_list_open(int sec);
void      ui_system_open(void);
void      ui_display_open(void);
/* The live view's layouts (windows per page) the platform can show. */
int       ui_live_layout_offered(int layout);
/* Where the clock is ("top right"), or "off". */
const char *ui_live_clock_corner(void);

/* "CAM 3" or the camera's name. */
void ui_chan_title(int ch, char *buf, size_t cap);

#endif /* UI_APP_H */
