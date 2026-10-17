/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hal_generic.c — the platform without media hardware: any Linux host or
 * cloud instance. Channels ingest and record; nothing is decoded or shown,
 * and status goes to the log (frontend_headless.c). */
#include "hal.h"
#include "hal_capture.h"
#include "hal_decoder.h"
#include "hal_display.h"

static const hal_caps_t g_caps = { .name = "generic" };

int hal_init(unsigned w, unsigned h, unsigned fps) { (void)w; (void)h; (void)fps; return 0; }
const hal_caps_t *hal_caps(void) { return &g_caps; }
void hal_shutdown(void) {}

int  hal_display_set_tile(int ch, const vg_rect_t *r) { (void)ch; (void)r; return -1; }
void hal_display_clear_tile(int ch) { (void)ch; }

int  hal_dec_open(int ch, vg_codec_t codec, const vg_stream_info_t *si) { (void)ch; (void)codec; (void)si; return -1; }
int  hal_dec_send(int ch, const vg_au_t *au) { (void)ch; (void)au; return -1; }
void hal_dec_reset(int ch) { (void)ch; }
void hal_dec_close(int ch) { (void)ch; }

int  hal_cap_start(int in, int ch, hal_cap_cb cb, void *user) { (void)in; (void)ch; (void)cb; (void)user; return -1; }
void hal_cap_stop(int in) { (void)in; }
int  hal_cap_signal(int in) { (void)in; return 0; }
