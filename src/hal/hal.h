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
    int      fixed_tiles;       /* the platform places the tiles itself, in the
                                 * quad view: hal_display_set_tile cannot move
                                 * them (no other layouts, no full screen) */
    int      dec_shown_only;    /* decoders (memory, channel numbers) do not
                                 * stretch to every channel at once: the core
                                 * decodes only the channels with a tile on
                                 * screen, and frees a channel's decoder when
                                 * its tile goes (recording carries on) */
    unsigned max_in_kbps;       /* the chip's rated incoming bandwidth for
                                 * network cameras (every stream, main and
                                 * sub), kbit/s, as its datasheet or product
                                 * brief gives it; 0: not known */
} hal_caps_t;

/* Bring up the media system and the display: a w x h screen (the UI's
 * and the tiles' coordinates), sent out at `out` (NULL: w x h at 60 Hz)
 * where the platform can (hal_display_modes); otherwise at its own mode.
 * Returns 0 or -1. */
int  hal_init(unsigned w, unsigned h, const vg_mode_t *out);
const hal_caps_t *hal_caps(void);
void hal_shutdown(void);

#endif /* HAL_H */
