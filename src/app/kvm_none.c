/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kvm_none.c — no zero channel: vigild without a display (kvm.h). */
#include <stdio.h>

#include "kvm.h"

int kvm_available(void) { return 0; }

kvm_client_t *kvm_open(const char *who, char *why, size_t cap)
{
    (void)who;
    snprintf(why, cap, "no screen on this platform");
    return NULL;
}

void kvm_close(kvm_client_t *c) { (void)c; }
void kvm_screen(unsigned *w, unsigned *h) { *w = *h = 0; }
void kvm_options(kvm_client_t *c, int scale, int quality) { (void)c; (void)scale; (void)quality; }
void kvm_wake(kvm_client_t *c) { (void)c; }
void kvm_last(kvm_client_t *c, unsigned *ms, size_t *bytes) { (void)c; *ms = 0; *bytes = 0; }

int kvm_next(kvm_client_t *c, int max_fps, unsigned char **msg, size_t *len, int timeout_ms)
{
    (void)c;
    (void)max_fps;
    (void)timeout_ms;
    *msg = NULL;
    *len = 0;
    return -1;
}

int kvm_input(kvm_client_t *c, const char *msg, size_t len)
{
    (void)c;
    (void)msg;
    (void)len;
    return -1;
}
