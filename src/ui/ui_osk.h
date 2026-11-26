/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_osk.h — the on-screen keyboard for mouse-only text entry: a small
 * popup next to the text field being edited (see ui_osk.c). */
#ifndef UI_OSK_H
#define UI_OSK_H

#include "lvgl.h"

/* A keyboard for the page screen `scr`, hidden until ui_osk_show. It
 * sends LV_EVENT_READY (done) and LV_EVENT_CANCEL (closed by a click
 * outside) to itself; the page hides it on those. */
lv_obj_t *ui_osk_create(lv_obj_t *scr);

/* Edit `ta` with it: below the field, or above when there is no room,
 * never over it. `numeric`: digits and '-' only. */
void ui_osk_show(lv_obj_t *osk, lv_obj_t *ta, int numeric);
void ui_osk_hide(lv_obj_t *osk);
int  ui_osk_shown(lv_obj_t *osk);

#endif /* UI_OSK_H */
