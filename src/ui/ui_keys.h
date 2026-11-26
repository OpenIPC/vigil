/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_keys.h — keys as the keyboard reader (src/app/input_evdev.c) hands
 * them to the UI (ui_app_key).
 *
 * A key is what LVGL's keypad takes: a character (its Unicode value, here
 * ASCII) or an LV_KEY_* code (arrows, Enter, Esc, Tab as LV_KEY_NEXT and
 * Shift+Tab as LV_KEY_PREV, Backspace, Delete, Home, End). Keys LVGL has no
 * code for get one past the end of Unicode, so they can never be typed
 * into a text field by mistake. Modifiers come separately: with Ctrl or
 * Alt a letter is its lower-case self.
 */
#ifndef UI_KEYS_H
#define UI_KEYS_H

#define UI_KEY_BASE   0x110000u
#define UI_KEY_F(n)   (UI_KEY_BASE + (n))       /* F1 .. F12 */
#define UI_KEY_PGUP   (UI_KEY_BASE + 0x20)
#define UI_KEY_PGDN   (UI_KEY_BASE + 0x21)
#define UI_KEY_MENU   (UI_KEY_BASE + 0x22)      /* the context-menu key */
#define UI_KEY_OTHER  (UI_KEY_BASE + 0x2F)      /* a key the UI has no use for */

#define UI_MOD_SHIFT  1u
#define UI_MOD_CTRL   2u
#define UI_MOD_ALT    4u

#endif /* UI_KEYS_H */
