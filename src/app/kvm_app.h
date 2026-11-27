/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kvm_app.h — the zero channel (kvm.h) on the UI's display: frontend_ui.c
 * attaches it once the display and the evdev reader are up. */
#ifndef KVM_APP_H
#define KVM_APP_H

#include "lvgl.h"

/* On the LVGL (main) thread. */
void kvm_attach(lv_display_t *disp);
void kvm_detach(void);

#endif /* KVM_APP_H */
