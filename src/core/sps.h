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

/* What an MP4 'hvcC' box repeats from an H.265 SPS: the general
 * profile_tier_level as it is in the SPS (12 bytes: space, tier and
 * profile; 32 compatibility flags; 48 constraint flags; level), the
 * chroma format, the bit depths less 8, the sub-layers and their nesting. */
typedef struct {
    uint8_t  ptl[12];
    unsigned chroma, depth_luma, depth_chroma;
    unsigned sub_layers;                /* sps_max_sub_layers_minus1 + 1 */
    unsigned nesting;                   /* sps_temporal_id_nesting_flag */
} sps_hevc_t;

/* Parse one H.265 SPS NAL unit (header included). Returns 0 or -1. */
int sps_hevc_parse(const uint8_t *nal, size_t len, sps_hevc_t *out);

#endif /* SPS_H */
