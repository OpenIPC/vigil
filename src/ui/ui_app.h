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
 * Back is the header's back button or the right mouse button; on the live
 * view the right button opens the context menu. State comes from the
 * model (ui_model.h): channel snapshots, events and the configuration
 * generation are polled on a timer and published as lv_subjects that the
 * widgets observe. LVGL thread (the main thread) only.
 */
#ifndef UI_APP_H
#define UI_APP_H

#include "lvgl.h"
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

/* The pointer's position, or 0 when there is no pointer. */
int ui_pointer(lv_point_t *p);

/* Screens. */
lv_obj_t *ui_live_create(void);
void      ui_live_context_menu(const lv_point_t *at);
void      ui_live_event(const ui_event_t *ev);
void      ui_menu_open(void);
void      ui_form_open(int sec, int item);  /* item -1: object section; n: new item */
void      ui_list_open(int sec);
void      ui_system_open(void);

/* "CAM 3" or the camera's name. */
void ui_chan_title(int ch, char *buf, size_t cap);

#endif /* UI_APP_H */
