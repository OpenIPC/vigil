/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hal.h — what a platform provides, and how the core asks for it.
 *
 * One platform is linked into vigild (CMake VIGIL_PLATFORM). It implements
 * the functions declared in the hal_*.h headers; what it cannot do it
 * reports through hal_caps() and stubs out. The core and the UI never
 * include a vendor header and never #ifdef on the platform.
 */
#ifndef HAL_H
#define HAL_H

#include "vg_types.h"

typedef struct {
    const char *name;           /* "hi3536dv100", "rockchip", "generic", ... */
    int      display;           /* HDMI/VO output with a UI layer */
    int      decoder;           /* can decode network streams into tiles */
    unsigned dec_max_w, dec_max_h;
    int      max_channels;      /* tiles / decoder channels */
    int      capture;           /* local video inputs (capture:N sources) */
} hal_caps_t;

/* Bring up the media system and the display (w x h @ fps when the
 * platform has one). Returns 0 or -1. */
int  hal_init(unsigned w, unsigned h, unsigned fps);
const hal_caps_t *hal_caps(void);
void hal_shutdown(void);

#endif /* HAL_H */
