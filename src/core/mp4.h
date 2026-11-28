/* SPDX-License-Identifier: GPL-3.0-or-later */
/* mp4.h — the few ISO BMFF (MP4) boxes the recording store writes and reads.
 *
 * Fragmented MP4 with one video track (docs/recording.md): an init
 * segment (ftyp + moov: H.264 'avc1' with 'avcC', H.265 'hvc1' with
 * 'hvcC', timescale 90 kHz, an empty sample table and 'mvex'), then
 * fragments, each a 'moof' (mfhd, traf: tfhd with default-base-is-moof,
 * tfdt version 1, trun with every sample's duration, size and flags, and
 * composition offsets when there are B-frames) and an 'mdat' of samples
 * whose NAL units carry 4-byte lengths instead of start codes.
 * Hand-written: no muxer library, nothing allocated but the output.
 */
#ifndef MP4_H
#define MP4_H

#include <stddef.h>
#include <stdint.h>

#include "vg_types.h"

#define MP4_TIMESCALE 90000
#define MP4_SYNC      0x02000000u       /* sample flags: a sync sample (depends on nothing) */
#define MP4_NON_SYNC  0x01010000u       /* depends on others, not a sync sample */

/* A growing output buffer; `err` set once it could not grow. */
typedef struct {
    uint8_t *p;
    size_t   len, cap;
    int      err;
} mp4_buf_t;

void   mp4_buf_free(mp4_buf_t *b);
void   mp4_put(mp4_buf_t *b, const void *data, size_t len);
void   mp4_u8(mp4_buf_t *b, uint8_t v);
void   mp4_u16(mp4_buf_t *b, uint16_t v);
void   mp4_u32(mp4_buf_t *b, uint32_t v);
void   mp4_u64(mp4_buf_t *b, uint64_t v);
size_t mp4_box_open(mp4_buf_t *b, const char type[4]);
size_t mp4_full_box_open(mp4_buf_t *b, const char type[4], uint8_t version, uint32_t flags);
void   mp4_box_close(mp4_buf_t *b, size_t at);

/* The stream an init segment describes. Parameter sets without start
 * codes: H.264 SPS, PPS; H.265 VPS, SPS, PPS. */
typedef struct {
    vg_codec_t     codec;
    unsigned       width, height;
    int            n_ps;
    const uint8_t *ps[3];
    size_t         ps_len[3];
} mp4_track_t;

/* ftyp + moov into `b`. With `elst_media` >= 0, an edit list starting the
 * presentation `elst_media` ticks into the media, `dur_ms` long (0: to
 * the end), and the movie's duration (mehd) said to be `dur_ms`.
 * Returns 0, or -1 when the parameter sets do not parse. */
int mp4_init(mp4_buf_t *b, const mp4_track_t *t, int64_t elst_media, uint64_t dur_ms);

/* The RFC 6381 codecs string for MSE: "avc1.640028", "hvc1.1.6.L93.B0". */
int mp4_codec_string(const mp4_track_t *t, char *out, size_t cap);

/* A stored init segment (ftyp + moov as mp4_init wrote it) with an edit
 * list added: the presentation starts `elst_media` ticks into the media
 * and lasts `dur_ms` (also the movie's duration: mehd, mvhd, tkhd). 0 or -1. */
int mp4_init_edit(const uint8_t *init, size_t len, int64_t elst_media, uint64_t dur_ms, mp4_buf_t *out);

/* The codecs string of a stored init segment (from its avcC / hvcC). */
int mp4_init_codecs(const uint8_t *init, size_t len, char *out, size_t cap);

typedef struct {
    uint32_t size, dur, flags;
    int32_t  cts;                       /* composition offset, ticks */
} mp4_sample_t;

/* The size of a moof for n samples (with or without composition offsets). */
size_t mp4_moof_size(uint32_t n, int with_cts);
/* Write it to `out` (mp4_moof_size bytes); the samples follow in an mdat
 * right after it (its 8-byte header included in the data offset). */
void mp4_moof_build(uint8_t *out, uint32_t seq, uint64_t tfdt, const mp4_sample_t *s, uint32_t n, int with_cts);

typedef struct {
    uint32_t seq;
    uint64_t tfdt;
    uint32_t n;                         /* samples */
    uint64_t data_size, dur;            /* their sizes and durations, summed */
    uint32_t data_offset;               /* from the moof's start */
    uint32_t first_flags;
    size_t   seq_at, tfdt_at;           /* where the values are, for rewriting */
    int      tfdt_v1;
    size_t   trun_at;                   /* the trun box, for mp4_moof_sample */
} mp4_moof_t;

/* Parse a moof box (`len` bytes from its start). 0, or -1 when it is not
 * one this file format writes (one track, default-base-is-moof). */
int mp4_moof_parse(const uint8_t *moof, size_t len, mp4_moof_t *m);
/* Sample i of a parsed moof. */
void mp4_moof_sample(const uint8_t *moof, const mp4_moof_t *m, uint32_t i, mp4_sample_t *s);
/* Rewrite a parsed moof's sequence number and decode time in place. */
void mp4_moof_patch(uint8_t *moof, const mp4_moof_t *m, uint32_t seq, uint64_t tfdt);

#endif /* MP4_H */
