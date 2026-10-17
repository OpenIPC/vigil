/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hal_decoder.h — hardware decoder per channel; its pictures go to the
 * channel's tile (hal_display_set_tile). */
#ifndef HAL_DECODER_H
#define HAL_DECODER_H

#include "vg_types.h"

/* (Re)create channel `ch`'s decoder for `codec` at the stream's size and
 * reference count. Returns 0, or -1 (e.g. not enough media memory). */
int  hal_dec_open(int ch, vg_codec_t codec, const vg_stream_info_t *si);
/* One normalized access unit (au_norm.h). Returns 0 or -1 (dropped). */
int  hal_dec_send(int ch, const vg_au_t *au);
/* Drop queued data after a reconnect; keep the decoder. */
void hal_dec_reset(int ch);
void hal_dec_close(int ch);

#endif /* HAL_DECODER_H */
