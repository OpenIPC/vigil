/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kvm.h — the zero channel's screen and input, as the web server (web.c)
 * uses them. The web server never draws or drives the UI itself: the app
 * implements this (src/app/kvm.c where there is a display, kvm_none.c
 * where there is not). Every function is safe on any thread. */
#ifndef KVM_H
#define KVM_H

#include <stddef.h>

#define KVM_CLIENTS 2               /* each encodes its own pictures */

typedef struct kvm_client kvm_client_t;

/* 1 when there is a screen to show. */
int kvm_available(void);

/* A viewer, its own mouse and keyboard plugged into the UI until
 * kvm_close. NULL with `why` when there is no screen or KVM_CLIENTS are
 * connected. `who` names it in the log. */
kvm_client_t *kvm_open(const char *who, char *why, size_t cap);
void          kvm_close(kvm_client_t *c);

/* The screen's size: what input coordinates are in. */
void kvm_screen(unsigned *w, unsigned *h);

/* Pictures at the screen's size (scale 1) or half (2), JPEG quality
 * 10..95. Takes effect with the next picture (a whole one). */
void kvm_options(kvm_client_t *c, int scale, int quality);

/* The next picture, a message as kvm_enc.h lays it out (free it): once
 * the screen changed, and no sooner than max_fps and the encoding time
 * allow. 1 with *msg; 0 when nothing changed within timeout_ms; -1 when
 * the screen is gone. kvm_wake ends a wait early (with 0). */
int  kvm_next(kvm_client_t *c, int max_fps, unsigned char **msg, size_t *len, int timeout_ms);
void kvm_wake(kvm_client_t *c);

/* The last picture's numbers, for the page: milliseconds it took to make
 * (compose and encode) and its size. */
void kvm_last(kvm_client_t *c, unsigned *ms, size_t *bytes);

/* One input message (JSON, kvm_input.h). 0, or -1 when it is not one. */
int kvm_input(kvm_client_t *c, const char *msg, size_t len);

#endif /* KVM_H */
