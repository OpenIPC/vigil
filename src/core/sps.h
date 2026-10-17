/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sps.h — picture size and reference count from an H.264 / H.265 SPS,
 * enough to size a decoder channel for the actual camera stream. */
#ifndef SPS_H
#define SPS_H

#include <stddef.h>
#include <stdint.h>

#include "vg_types.h"

/* Parse one SPS NAL unit (header included, no start code). Returns 0. */
int sps_parse(vg_codec_t codec, const uint8_t *nal, size_t len, vg_stream_info_t *out);

#endif /* SPS_H */
