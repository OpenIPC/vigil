/* SPDX-License-Identifier: GPL-3.0-or-later */
/* frontend.h — what vigild runs once the channels are up: the LVGL UI
 * (frontend_ui.c, platforms with a display) or periodic status lines
 * (frontend_headless.c). CMake links exactly one. */
#ifndef FRONTEND_H
#define FRONTEND_H

#include <signal.h>

#include "vg_types.h"

#define SCREEN_W   1280
#define SCREEN_H   720
#define HEADER_H   40
#define STRIP_H    80

/* Tile rectangles of the quad view, for the decoders' output. */
void frontend_tiles(vg_rect_t *tiles, int n);

/* Run until *stop or run_secs elapse (0 = no limit). Returns 0 or -1. */
int frontend_run(int run_secs, volatile sig_atomic_t *stop);

#endif /* FRONTEND_H */
