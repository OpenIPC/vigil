/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rk_display.h — DRM/KMS output on Rockchip VOP2 (RK3588/RK356x): one CRTC,
 * an NV12 "mosaic" plane carrying the composed video (double-buffered) and
 * an ARGB8888 plane above it for the UI, with per-pixel alpha. */
#ifndef RK_DISPLAY_H
#define RK_DISPLAY_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int      fd;            /* dmabuf fd (for RGA) */
    uint8_t *map;           /* CPU mapping */
    uint32_t pitch;         /* bytes per line (Y plane for NV12) */
    uint32_t width, height;
} rk_buf_t;

/* Modeset w x h @ 60 on the first connected HDMI/DP connector; with none
 * connected, drive the CRTC for the writeback connector alone (headless:
 * the composed output is still produced and can be captured). */
int  rk_display_open(unsigned w, unsigned h);
void rk_display_close(void);

/* The mosaic buffer to compose into next (not on screen). */
rk_buf_t *rk_display_back(void);
/* Show the back buffer (blocks until the next vblank). */
int  rk_display_flip(void);

/* The UI plane's buffer (single-buffered, always on screen). */
rk_buf_t *rk_display_ui(void);

/* Capture the composed output of the next flip through the writeback
 * connector into `path` (raw ARGB8888, width*height*4). Returns 0 once
 * written, -1 if the hardware has no writeback. Call from the thread that
 * flips (the compositor), or set a request with rk_display_request_shot. */
void rk_display_request_shot(const char *path);
int  rk_display_connected(void);    /* 1 = a monitor, 0 = headless */

#endif /* RK_DISPLAY_H */
