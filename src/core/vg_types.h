/* SPDX-License-Identifier: GPL-3.0-or-later */
/* vg_types.h — types shared by the core, the sources and the HAL. */
#ifndef VG_TYPES_H
#define VG_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef enum { VG_CODEC_NONE = 0, VG_CODEC_H264, VG_CODEC_H265 } vg_codec_t;

/* One access unit (a picture, with any parameter sets / SEI before it),
 * Annex-B with start codes. */
typedef struct {
    vg_codec_t     codec;
    const uint8_t *data;
    size_t         len;
    uint64_t       pts_us;      /* presentation time, microseconds (0 = unknown) */
    int            key;         /* holds a keyframe (IDR / IRAP) */
    int            discont;     /* data was lost before this AU */
} vg_au_t;

/* What a decoder needs to know about a stream (from its SPS). */
typedef struct {
    unsigned width, height;     /* displayed size, cropping applied */
    unsigned refs;              /* reference frames the decoder must keep */
} vg_stream_info_t;

typedef struct { int x, y, w, h; } vg_rect_t;

static inline const char *vg_codec_name(vg_codec_t c)
{
    return c == VG_CODEC_H264 ? "H.264" : c == VG_CODEC_H265 ? "H.265" : "?";
}

#endif /* VG_TYPES_H */
