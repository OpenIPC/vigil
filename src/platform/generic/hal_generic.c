/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hal_generic.c — the platform without media hardware: any Linux host or
 * cloud instance. Channels ingest and record; nothing is decoded or shown,
 * and status goes to the log (frontend_headless.c). */
#include "hal.h"
#include "hal_capture.h"
#include "hal_decoder.h"
#include "hal_display.h"
#include "hal_audio.h"

static const hal_caps_t g_caps = { .name = "generic" };

int hal_init(unsigned w, unsigned h, const vg_mode_t *out) { (void)w; (void)h; (void)out; return 0; }
const hal_caps_t *hal_caps(void) { return &g_caps; }
void hal_shutdown(void) {}

int  hal_display_set_tile(int ch, const vg_rect_t *r) { (void)ch; (void)r; return -1; }
void hal_display_clear_tile(int ch) { (void)ch; }
void hal_display_set_aspect(int ch, unsigned w, unsigned h) { (void)ch; (void)w; (void)h; }
/* No output. */
int  hal_display_modes(vg_mode_t *out, int max) { (void)out; (void)max; return 0; }
void hal_display_mode(vg_mode_t *cur) { *cur = (vg_mode_t){ 0, 0, 0 }; }
int  hal_display_set_mode(const vg_mode_t *m) { (void)m; return -1; }

int  hal_dec_open(int ch, vg_codec_t codec, const vg_stream_info_t *si) { (void)ch; (void)codec; (void)si; return -1; }
int  hal_dec_send(int ch, const vg_au_t *au) { (void)ch; (void)au; return -1; }
void hal_dec_reset(int ch) { (void)ch; }
void hal_dec_close(int ch) { (void)ch; }

int  hal_cap_start(int in, int ch, hal_cap_cb cb, void *user) { (void)in; (void)ch; (void)cb; (void)user; return -1; }
void hal_cap_stop(int in) { (void)in; }
int  hal_cap_signal(int in) { (void)in; return 0; }

/* No picture to read back: the zero channel shows the UI over black. */
void hal_display_grab_keep(int on) { (void)on; }
int  hal_display_grab(const hal_pic_t *pic) { (void)pic; return -1; }

/* No margins, picture controls or HDMI sound here yet (hal_caps: 0). */
int hal_display_set_margins(const vg_margins_t *m) { (void)m; return -1; }
int hal_display_set_picture(const vg_picture_t *p) { (void)p; return -1; }
int hal_audio_set_volume(unsigned volume, int mute) { (void)volume; (void)mute; return -1; }
