/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_osk.c — the on-screen keyboard.
 *
 * LVGL's keyboard with maps of our own, about 560 x 190 pixels: a popup
 * attached to the text field it types into, below it (or above when the
 * screen ends first), aligned with it and kept on the screen. Pages:
 *
 *   letters    digits on top, QWERTY, Shift (one letter; twice: caps lock),
 *              "#+=" to the symbols, cursor keys, space, done
 *   symbols    every character a URL or a password may need:
 *              : / @ . - _ ? & = % # ! $ * + ~ , ; ' " ( ) [ ] { } < > \ | ^ `
 *   numbers    digits and '-' for a number field
 *
 * A click outside it closes it (a transparent catcher behind it), and a
 * click there on another text field of the page moves it there. Only for
 * mouse users: once a keyboard has been used the forms do not open it.
 */
#include <string.h>

#include "ui_osk.h"
#include "ui_theme.h"

#define SHIFT  LV_SYMBOL_UP
#define BKSP   LV_SYMBOL_BACKSPACE
#define DONE   LV_SYMBOL_OK
#define SHIFT_ID 31                 /* its index in the letters' maps */

enum { M_LOWER = LV_KEYBOARD_MODE_USER_1, M_UPPER, M_SYMBOLS, M_NUMBERS };

static const char *const k_lower[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", BKSP, "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", ".", "\n",
    SHIFT, "z", "x", "c", "v", "b", "n", "m", ":", "/", "@", "\n",
    "#+=", LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, DONE, "",
};
static const char *const k_upper[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", BKSP, "\n",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", ".", "\n",
    SHIFT, "Z", "X", "C", "V", "B", "N", "M", ":", "/", "@", "\n",
    "#+=", LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, DONE, "",
};
static const char *const k_symbols[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", BKSP, "\n",
    ":", "/", "@", ".", "-", "_", "?", "&", "=", "%", "\n",
    "#", "!", "$", "*", "+", "~", ",", ";", "'", "\"", "\n",
    "(", ")", "[", "]", "{", "}", "<", ">", "\\", "|", "^", "`", "\n",
    "abc", LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, DONE, "",
};
static const char *const k_numbers[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", BKSP, "\n",
    "-", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, DONE, "",
};

#define W(n) ((lv_buttonmatrix_ctrl_t)(n))
#define K    (LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG)
/* Widths (relative within a row); the special keys do not repeat. */
static const lv_buttonmatrix_ctrl_t k_letters_ctrl[] = {
    W(2), W(2), W(2), W(2), W(2), W(2), W(2), W(2), W(2), W(2), W(3) | K,
    W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1),
    W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1),
    W(3) | K, W(2), W(2), W(2), W(2), W(2), W(2), W(2), W(2), W(2), W(2),
    W(2) | K, W(1), W(5), W(1), W(2) | K,
};
static const lv_buttonmatrix_ctrl_t k_symbols_ctrl[] = {
    W(2), W(2), W(2), W(2), W(2), W(2), W(2), W(2), W(2), W(2), W(3) | K,
    W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1),
    W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1),
    W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1),
    W(2) | K, W(1), W(5), W(1), W(2) | K,
};
static const lv_buttonmatrix_ctrl_t k_numbers_ctrl[] = {
    W(2), W(2), W(2), W(2), W(2), W(2), W(2), W(2), W(2), W(2), W(3) | K,
    W(2), W(1), W(1), W(2) | K,
};

typedef struct {
    lv_obj_t *catcher;          /* behind the keyboard, over the page: a click there closes it */
    int       caps;             /* 0 lower, 1 one capital, 2 caps lock */
} osk_t;

static void set_page(lv_obj_t *kb, int mode)
{
    lv_keyboard_set_mode(kb, (lv_keyboard_mode_t)mode);
}

static void key_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_current_target(e);
    osk_t *o = lv_obj_get_user_data(kb);
    lv_obj_t *ta = lv_keyboard_get_textarea(kb);
    uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *t = lv_buttonmatrix_get_button_text(kb, id);
    if (!t) return;
    if (!strcmp(t, SHIFT)) {
        /* lower -> one capital -> caps lock -> lower */
        o->caps = (o->caps + 1) % 3;
        set_page(kb, o->caps ? M_UPPER : M_LOWER);
        if (o->caps == 2) lv_buttonmatrix_set_button_ctrl(kb, SHIFT_ID, LV_BUTTONMATRIX_CTRL_CHECKED);
        return;
    }
    if (!strcmp(t, "#+=")) {
        set_page(kb, M_SYMBOLS);
        return;
    }
    if (!strcmp(t, "abc")) {
        o->caps = 0;
        set_page(kb, M_LOWER);
        return;
    }
    if (!strcmp(t, DONE)) {
        lv_obj_send_event(kb, LV_EVENT_READY, NULL);
        return;
    }
    if (!ta) return;
    if (!strcmp(t, BKSP)) lv_textarea_delete_char(ta);
    else if (!strcmp(t, LV_SYMBOL_LEFT)) lv_textarea_cursor_left(ta);
    else if (!strcmp(t, LV_SYMBOL_RIGHT)) lv_textarea_cursor_right(ta);
    else {
        lv_textarea_add_text(ta, t);
        if (o->caps == 1) {                         /* one capital only */
            o->caps = 0;
            set_page(kb, M_LOWER);
        }
    }
}

/* A click outside: closed; on another text field of the page: moved there. */
static void catcher_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_user_data(e);
    osk_t *o = lv_obj_get_user_data(kb);
    lv_point_t p;
    lv_indev_t *in = lv_indev_active();
    lv_obj_add_flag(o->catcher, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *under = NULL;
    if (in) {
        lv_indev_get_point(in, &p);
        under = lv_indev_search_obj(lv_obj_get_screen(kb), &p);
    }
    if (under && lv_obj_check_type(under, &lv_textarea_class) && under != lv_keyboard_get_textarea(kb)) {
        lv_obj_remove_flag(o->catcher, LV_OBJ_FLAG_HIDDEN);
        lv_obj_send_event(under, LV_EVENT_CLICKED, NULL);       /* its page shows the keyboard on it */
        return;
    }
    lv_obj_send_event(kb, LV_EVENT_CANCEL, NULL);
}

static void deleted_cb(lv_event_t *e) { lv_free(lv_event_get_user_data(e)); }

lv_obj_t *ui_osk_create(lv_obj_t *scr)
{
    osk_t *o = lv_malloc_zeroed(sizeof *o);
    if (!o) return NULL;
    o->catcher = lv_obj_create(scr);
    ui_plain(o->catcher);
    lv_obj_set_size(o->catcher, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(o->catcher, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *kb = lv_keyboard_create(scr);
    lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(kb, key_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(kb, deleted_cb, LV_EVENT_DELETE, o);
    lv_obj_add_event_cb(o->catcher, catcher_cb, LV_EVENT_CLICKED, kb);
    lv_obj_set_user_data(kb, o);
    lv_keyboard_set_map(kb, M_LOWER, k_lower, k_letters_ctrl);
    lv_keyboard_set_map(kb, M_UPPER, k_upper, k_letters_ctrl);
    lv_keyboard_set_map(kb, M_SYMBOLS, k_symbols, k_symbols_ctrl);
    lv_keyboard_set_map(kb, M_NUMBERS, k_numbers, k_numbers_ctrl);
    lv_keyboard_set_popovers(kb, 0);
    lv_obj_set_size(kb, VG_SIZE_OSK_W, VG_SIZE_OSK_H);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
    /* The look from the tokens: a panel of flat keys. */
    lv_obj_set_style_bg_color(kb, UI_C(PANEL), 0);
    lv_obj_set_style_border_color(kb, UI_C(ACCENT), 0);
    lv_obj_set_style_border_width(kb, 1, 0);
    lv_obj_set_style_radius(kb, VG_RADIUS_MD, 0);
    lv_obj_set_style_pad_all(kb, VG_SPACE_XS, 0);
    lv_obj_set_style_pad_gap(kb, 3, 0);
    lv_obj_set_style_bg_color(kb, UI_C(PANEL2), LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, UI_C(TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, VG_FONT_MD_LV, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, VG_RADIUS_SM, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, UI_C(ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kb, UI_C(HOVER), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    return kb;
}

void ui_osk_show(lv_obj_t *kb, lv_obj_t *ta, int numeric)
{
    osk_t *o = lv_obj_get_user_data(kb);
    o->caps = 0;
    set_page(kb, numeric ? M_NUMBERS : M_LOWER);
    lv_keyboard_set_textarea(kb, ta);
    /* The field in view, then the keyboard under it, or over it when the
     * screen ends first; never on it. */
    lv_obj_scroll_to_view_recursive(ta, LV_ANIM_OFF);
    lv_obj_update_layout(lv_obj_get_screen(kb));
    lv_area_t a;
    lv_obj_get_coords(ta, &a);
    int sw = lv_display_get_horizontal_resolution(NULL), sh = lv_display_get_vertical_resolution(NULL);
    int w = VG_SIZE_OSK_W, h = VG_SIZE_OSK_H, gap = VG_SPACE_XS;
    int x = a.x1, y = a.y2 + 1 + gap;
    if (x + w > sw - VG_SPACE_SM) x = sw - VG_SPACE_SM - w;
    if (x < VG_SPACE_SM) x = VG_SPACE_SM;
    if (y + h > sh - VG_SPACE_SM) y = a.y1 - gap - h;
    if (y < 0) y = 0;
    lv_obj_set_align(kb, LV_ALIGN_TOP_LEFT);      /* lv_keyboard aligns itself to the bottom */
    lv_obj_set_pos(kb, x, y);
    lv_obj_remove_flag(o->catcher, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(o->catcher);
    lv_obj_move_foreground(kb);
}

void ui_osk_hide(lv_obj_t *kb)
{
    if (!kb) return;
    osk_t *o = lv_obj_get_user_data(kb);
    lv_keyboard_set_textarea(kb, NULL);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(o->catcher, LV_OBJ_FLAG_HIDDEN);
}

int ui_osk_shown(lv_obj_t *kb) { return kb && !lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN); }
