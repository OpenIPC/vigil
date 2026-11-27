/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kvm_input.h — the zero channel's input: what a browser sends (JSON
 * messages, docs/api.md "Zero channel") as Linux input events, the ones a
 * USB mouse and keyboard produce, for the evdev reader (input_evdev.c). */
#ifndef KVM_INPUT_H
#define KVM_INPUT_H

enum { KVM_DEV_POINTER = 0, KVM_DEV_KEYBOARD = 1 };

typedef struct {
    int dev;                        /* KVM_DEV_* */
    unsigned short type, code;      /* EV_*, and its code */
    int value;
} kvm_ev_t;

/* The key (KEY_*) of a KeyboardEvent.code ("KeyA", "Enter", "F1",
 * "ArrowUp", ...), 0 for none: keys by their place on a US keyboard, as
 * the console reads a USB keyboard. */
unsigned kvm_key_code(const char *code);

/* The events for one message: {"t":"move","x":X,"y":Y} (screen pixels of
 * a w x h screen; clamped), {"t":"btn","b":0|1|2,"down":true} (left,
 * middle, right), {"t":"wheel","n":N} (notches, > 0 away from the user),
 * {"t":"key","code":"KeyA","v":1|0|2} (press, release, repeat). Returns
 * how many were written (at most max), 0 for a message that moves
 * nothing, -1 for one that is not input. */
int kvm_input_events(const char *json, unsigned long len, unsigned w, unsigned h, kvm_ev_t *out, int max);

#endif /* KVM_INPUT_H */
