/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_cursor.h — the software mouse pointer (lv_indev_set_cursor). */
#ifndef UI_CURSOR_H
#define UI_CURSOR_H

#include "lvgl.h"

/* An arrow image on the system layer; its top-left is the hot spot. */
lv_obj_t *ui_cursor_create(void);

#endif /* UI_CURSOR_H */
