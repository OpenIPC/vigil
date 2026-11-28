/* SPDX-License-Identifier: GPL-3.0-or-later */
/* mp4.c — ISO BMFF boxes for the recording store (mp4.h).
 * ISO/IEC 14496-12 (boxes), 14496-15 (avcC 5.3.3.1, hvcC 8.3.3.1). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp4.h"
#include "sps.h"

void mp4_buf_free(mp4_buf_t *b)
{
    free(b->p);
    memset(b, 0, sizeof *b);
}

void mp4_put(mp4_buf_t *b, const void *data, size_t len)
{
    if (b->err) return;
    if (b->len + len > b->cap) {
        size_t cap = b->cap ? b->cap : 512;
        while (cap < b->len + len) cap *= 2;
        uint8_t *p = realloc(b->p, cap);
        if (!p) {
            b->err = 1;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, data, len);
    b->len += len;
}

void mp4_u8(mp4_buf_t *b, uint8_t v) { mp4_put(b, &v, 1); }
void mp4_u16(mp4_buf_t *b, uint16_t v)
{
    uint8_t x[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    mp4_put(b, x, 2);
}
void mp4_u32(mp4_buf_t *b, uint32_t v)
{
    uint8_t x[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    mp4_put(b, x, 4);
}
void mp4_u64(mp4_buf_t *b, uint64_t v)
{
    mp4_u32(b, (uint32_t)(v >> 32));
    mp4_u32(b, (uint32_t)v);
}

size_t mp4_box_open(mp4_buf_t *b, const char type[4])
{
    size_t at = b->len;
    mp4_u32(b, 0);
    mp4_put(b, type, 4);
    return at;
}

size_t mp4_full_box_open(mp4_buf_t *b, const char type[4], uint8_t version, uint32_t flags)
{
    size_t at = mp4_box_open(b, type);
    mp4_u32(b, (uint32_t)version << 24 | (flags & 0xFFFFFF));
    return at;
}

void mp4_box_close(mp4_buf_t *b, size_t at)
{
    if (b->err) return;
    uint32_t size = (uint32_t)(b->len - at);
    b->p[at] = (uint8_t)(size >> 24);
    b->p[at + 1] = (uint8_t)(size >> 16);
    b->p[at + 2] = (uint8_t)(size >> 8);
    b->p[at + 3] = (uint8_t)size;
}

static void zeros(mp4_buf_t *b, size_t n)
{
    static const uint8_t z[32];
    while (n) {
        size_t k = n < sizeof z ? n : sizeof z;
        mp4_put(b, z, k);
        n -= k;
    }
}

static void matrix(mp4_buf_t *b)
{
    static const uint32_t m[9] = { 0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000 };
    for (int i = 0; i < 9; ++i) mp4_u32(b, m[i]);
}

/* ---- sample entries ---- */

static int avcc(mp4_buf_t *b, const mp4_track_t *t)
{
    if (t->n_ps < 2 || t->ps_len[0] < 4 || !t->ps_len[1] || t->ps_len[0] > 0xFFFF || t->ps_len[1] > 0xFFFF)
        return -1;
    const uint8_t *sps = t->ps[0];
    size_t at = mp4_box_open(b, "avcC");
    mp4_u8(b, 1);                                       /* configurationVersion */
    mp4_u8(b, sps[1]);                                  /* AVCProfileIndication */
    mp4_u8(b, sps[2]);                                  /* profile_compatibility */
    mp4_u8(b, sps[3]);                                  /* AVCLevelIndication */
    mp4_u8(b, 0xFC | 3);                                /* lengthSizeMinusOne: 4-byte lengths */
    mp4_u8(b, 0xE0 | 1);                                /* one SPS */
    mp4_u16(b, (uint16_t)t->ps_len[0]);
    mp4_put(b, t->ps[0], t->ps_len[0]);
    mp4_u8(b, 1);                                       /* one PPS */
    mp4_u16(b, (uint16_t)t->ps_len[1]);
    mp4_put(b, t->ps[1], t->ps_len[1]);
    mp4_box_close(b, at);
    return 0;
}

static int hvcc(mp4_buf_t *b, const mp4_track_t *t)
{
    sps_hevc_t h;
    if (t->n_ps < 3 || sps_hevc_parse(t->ps[1], t->ps_len[1], &h) < 0) return -1;
    for (int i = 0; i < 3; ++i)
        if (!t->ps_len[i] || t->ps_len[i] > 0xFFFF) return -1;
    size_t at = mp4_box_open(b, "hvcC");
    mp4_u8(b, 1);                                       /* configurationVersion */
    mp4_put(b, h.ptl, 12);                              /* profile ... level, as in the SPS */
    mp4_u16(b, 0xF000);                                 /* min_spatial_segmentation_idc 0 */
    mp4_u8(b, 0xFC);                                    /* parallelismType 0 */
    mp4_u8(b, (uint8_t)(0xFC | h.chroma));
    mp4_u8(b, (uint8_t)(0xF8 | h.depth_luma));
    mp4_u8(b, (uint8_t)(0xF8 | h.depth_chroma));
    mp4_u16(b, 0);                                      /* avgFrameRate */
    mp4_u8(b, (uint8_t)((h.sub_layers & 7) << 3 | (h.nesting & 1) << 2 | 3));
    mp4_u8(b, 3);                                       /* arrays: VPS, SPS, PPS */
    static const uint8_t types[3] = { 32, 33, 34 };
    for (int i = 0; i < 3; ++i) {
        mp4_u8(b, 0x80 | types[i]);                     /* array_completeness: all in here */
        mp4_u16(b, 1);
        mp4_u16(b, (uint16_t)t->ps_len[i]);
        mp4_put(b, t->ps[i], t->ps_len[i]);
    }
    mp4_box_close(b, at);
    return 0;
}

int mp4_init(mp4_buf_t *b, const mp4_track_t *t, int64_t elst_media, uint64_t dur_ms)
{
    int h265 = t->codec == VG_CODEC_H265;
    size_t box = mp4_box_open(b, "ftyp");
    mp4_put(b, "iso6", 4);
    mp4_u32(b, 0);
    mp4_put(b, "iso6iso5isommp41", 16);
    mp4_put(b, h265 ? "hvc1" : "avc1", 4);
    mp4_box_close(b, box);

    size_t moov = mp4_box_open(b, "moov");
    box = mp4_full_box_open(b, "mvhd", 0, 0);
    mp4_u32(b, 0);                                      /* creation, modification */
    mp4_u32(b, 0);
    mp4_u32(b, 1000);                                   /* movie timescale: ms */
    mp4_u32(b, 0);                                      /* duration: in the fragments */
    mp4_u32(b, 0x00010000);                             /* rate 1.0 */
    mp4_u16(b, 0x0100);                                 /* volume */
    zeros(b, 10);
    matrix(b);
    zeros(b, 24);
    mp4_u32(b, 2);                                      /* next_track_ID */
    mp4_box_close(b, box);

    size_t trak = mp4_box_open(b, "trak");
    box = mp4_full_box_open(b, "tkhd", 0, 3);           /* enabled, in the movie */
    mp4_u32(b, 0);
    mp4_u32(b, 0);
    mp4_u32(b, 1);                                      /* track_ID */
    mp4_u32(b, 0);
    mp4_u32(b, 0);                                      /* duration */
    zeros(b, 8);
    mp4_u16(b, 0);                                      /* layer */
    mp4_u16(b, 0);                                      /* alternate_group */
    mp4_u16(b, 0);                                      /* volume */
    mp4_u16(b, 0);
    matrix(b);
    mp4_u32(b, t->width << 16);
    mp4_u32(b, t->height << 16);
    mp4_box_close(b, box);

    if (elst_media >= 0) {
        size_t edts = mp4_box_open(b, "edts");
        box = mp4_full_box_open(b, "elst", 1, 0);
        mp4_u32(b, 1);
        mp4_u64(b, dur_ms);                             /* segment_duration, movie timescale */
        mp4_u64(b, (uint64_t)elst_media);               /* media_time */
        mp4_u16(b, 1);                                  /* media_rate 1.0 */
        mp4_u16(b, 0);
        mp4_box_close(b, box);
        mp4_box_close(b, edts);
    }

    size_t mdia = mp4_box_open(b, "mdia");
    box = mp4_full_box_open(b, "mdhd", 0, 0);
    mp4_u32(b, 0);
    mp4_u32(b, 0);
    mp4_u32(b, MP4_TIMESCALE);
    mp4_u32(b, 0);
    mp4_u16(b, 0x55C4);                                 /* "und" */
    mp4_u16(b, 0);
    mp4_box_close(b, box);
    box = mp4_full_box_open(b, "hdlr", 0, 0);
    mp4_u32(b, 0);
    mp4_put(b, "vide", 4);
    zeros(b, 12);
    mp4_put(b, "VideoHandler", 13);
    mp4_box_close(b, box);

    size_t minf = mp4_box_open(b, "minf");
    box = mp4_full_box_open(b, "vmhd", 0, 1);
    zeros(b, 8);
    mp4_box_close(b, box);
    size_t dinf = mp4_box_open(b, "dinf");
    size_t dref = mp4_full_box_open(b, "dref", 0, 0);
    mp4_u32(b, 1);
    box = mp4_full_box_open(b, "url ", 0, 1);           /* the data is in this file */
    mp4_box_close(b, box);
    mp4_box_close(b, dref);
    mp4_box_close(b, dinf);

    size_t stbl = mp4_box_open(b, "stbl");
    size_t stsd = mp4_full_box_open(b, "stsd", 0, 0);
    mp4_u32(b, 1);
    size_t entry = mp4_box_open(b, h265 ? "hvc1" : "avc1");
    zeros(b, 6);
    mp4_u16(b, 1);                                      /* data_reference_index */
    zeros(b, 16);
    mp4_u16(b, (uint16_t)t->width);
    mp4_u16(b, (uint16_t)t->height);
    mp4_u32(b, 0x00480000);                             /* 72 dpi */
    mp4_u32(b, 0x00480000);
    mp4_u32(b, 0);
    mp4_u16(b, 1);                                      /* frame_count */
    uint8_t name[32] = { 5, 'v', 'i', 'g', 'i', 'l' };
    mp4_put(b, name, sizeof name);
    mp4_u16(b, 0x0018);                                 /* depth */
    mp4_u16(b, 0xFFFF);                                 /* pre_defined -1 */
    if ((h265 ? hvcc(b, t) : avcc(b, t)) < 0) return -1;
    mp4_box_close(b, entry);
    mp4_box_close(b, stsd);
    static const char *const empty[4] = { "stts", "stsc", "stsz", "stco" };
    for (int i = 0; i < 4; ++i) {
        box = mp4_full_box_open(b, empty[i], 0, 0);
        if (i == 2) mp4_u32(b, 0);                      /* stsz: sample_size */
        mp4_u32(b, 0);
        mp4_box_close(b, box);
    }
    mp4_box_close(b, stbl);
    mp4_box_close(b, minf);
    mp4_box_close(b, mdia);
    mp4_box_close(b, trak);

    size_t mvex = mp4_box_open(b, "mvex");
    if (elst_media >= 0 && dur_ms) {
        box = mp4_full_box_open(b, "mehd", 1, 0);
        mp4_u64(b, dur_ms);
        mp4_box_close(b, box);
    }
    box = mp4_full_box_open(b, "trex", 0, 0);
    mp4_u32(b, 1);                                      /* track_ID */
    mp4_u32(b, 1);                                      /* default_sample_description_index */
    mp4_u32(b, 0);
    mp4_u32(b, 0);
    mp4_u32(b, 0);
    mp4_box_close(b, box);
    mp4_box_close(b, mvex);
    mp4_box_close(b, moov);
    return b->err ? -1 : 0;
}

/* "hvc1.1.6.L93.B0" from the 12 bytes of a general profile_tier_level. */
static void hvc1_string(const uint8_t ptl[12], char *out, size_t cap)
{
    static const char *const space[4] = { "", "A", "B", "C" };
    uint32_t compat = (uint32_t)ptl[1] << 24 | (uint32_t)ptl[2] << 16 | (uint32_t)ptl[3] << 8 | ptl[4];
    uint32_t rev = 0;
    for (int i = 0; i < 32; ++i) rev |= ((compat >> i) & 1u) << (31 - i);
    int n = snprintf(out, cap, "hvc1.%s%u.%X.%c%u", space[ptl[0] >> 6], ptl[0] & 0x1F, rev,
                     (ptl[0] & 0x20) ? 'H' : 'L', ptl[11]);
    int last = 6;
    while (last > 0 && !ptl[4 + last]) --last;          /* trailing zero bytes are left out */
    for (int i = 0; i < last && n > 0 && (size_t)n < cap; ++i)
        n += snprintf(out + n, cap - (size_t)n, ".%X", ptl[5 + i]);
}

int mp4_codec_string(const mp4_track_t *t, char *out, size_t cap)
{
    if (t->codec == VG_CODEC_H264) {
        if (t->n_ps < 1 || t->ps_len[0] < 4) return -1;
        snprintf(out, cap, "avc1.%02X%02X%02X", t->ps[0][1], t->ps[0][2], t->ps[0][3]);
        return 0;
    }
    sps_hevc_t h;
    if (t->n_ps < 2 || sps_hevc_parse(t->ps[1], t->ps_len[1], &h) < 0) return -1;
    hvc1_string(h.ptl, out, cap);
    return 0;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* The child of type `t` among the boxes in [p, p + len), or NULL. */
static const uint8_t *child(const uint8_t *p, size_t len, const char *t, size_t *clen)
{
    for (size_t at = 0; at + 8 <= len;) {
        uint32_t sz = rd32(p + at);
        if (sz < 8 || at + sz > len) return NULL;
        if (!memcmp(p + at + 4, t, 4)) {
            *clen = sz;
            return p + at;
        }
        at += sz;
    }
    return NULL;
}

int mp4_init_codecs(const uint8_t *init, size_t len, char *out, size_t cap)
{
    static const char *const path[] = { "moov", "trak", "mdia", "minf", "stbl", "stsd" };
    const uint8_t *b = init;
    size_t bl = len;
    for (size_t i = 0; i < sizeof path / sizeof path[0]; ++i) {
        b = child(i ? b + 8 : b, i ? bl - 8 : bl, path[i], &bl);
        if (!b) return -1;
    }
    /* stsd: full box + entry_count, then the sample entry; its own
     * fields are 78 bytes before the codec's box. */
    if (bl < 16 + 86) return -1;
    const uint8_t *e = b + 16;
    size_t el = rd32(e);
    if (el > bl - 16 || el < 86) return -1;
    size_t cl;
    const uint8_t *c;
    if (!memcmp(e + 4, "avc1", 4) && (c = child(e + 86, el - 86, "avcC", &cl)) && cl >= 12) {
        snprintf(out, cap, "avc1.%02X%02X%02X", c[9], c[10], c[11]);
        return 0;
    }
    if (!memcmp(e + 4, "hvc1", 4) && (c = child(e + 86, el - 86, "hvcC", &cl)) && cl >= 8 + 13) {
        hvc1_string(c + 9, out, cap);
        return 0;
    }
    return -1;
}

/* Copy box `b` (`len` bytes) into `o`, adding the edit list to a trak and
 * the movie's duration to mvex. */
static void copy_edit(mp4_buf_t *o, const uint8_t *b, size_t len, int64_t media, uint64_t dur_ms)
{
    int trak = !memcmp(b + 4, "trak", 4), mvex = !memcmp(b + 4, "mvex", 4), moov = !memcmp(b + 4, "moov", 4);
    if (!trak && !mvex && !moov) {
        size_t at = o->len;
        mp4_put(o, b, len);
        /* The movie's and the track's duration too (version 0 boxes, as
         * mp4_init writes them): a browser playing the file takes it from
         * there, not from mehd or the edit list. */
        if (!o->err && dur_ms && dur_ms <= 0xFFFFFFFFu && b[8] == 0) {
            size_t f = !memcmp(b + 4, "mvhd", 4) && len >= 28 ? 24 : !memcmp(b + 4, "tkhd", 4) && len >= 32 ? 28 : 0;
            if (f) {
                o->p[at + f] = (uint8_t)(dur_ms >> 24);
                o->p[at + f + 1] = (uint8_t)(dur_ms >> 16);
                o->p[at + f + 2] = (uint8_t)(dur_ms >> 8);
                o->p[at + f + 3] = (uint8_t)dur_ms;
            }
        }
        return;
    }
    size_t at = o->len;
    mp4_put(o, b, 8);
    if (mvex && dur_ms) {
        size_t box = mp4_full_box_open(o, "mehd", 1, 0);
        mp4_u64(o, dur_ms);
        mp4_box_close(o, box);
    }
    for (size_t c = 8; c + 8 <= len;) {
        uint32_t sz = rd32(b + c);
        if (sz < 8 || c + sz > len) break;
        if (mvex && !memcmp(b + c + 4, "mehd", 4)) {
            c += sz;
            continue;
        }
        if (trak && !memcmp(b + c + 4, "edts", 4)) {
            c += sz;
            continue;
        }
        copy_edit(o, b + c, sz, media, dur_ms);
        if (trak && !memcmp(b + c + 4, "tkhd", 4)) {
            size_t edts = mp4_box_open(o, "edts");
            size_t box = mp4_full_box_open(o, "elst", 1, 0);
            mp4_u32(o, 1);
            mp4_u64(o, dur_ms);
            mp4_u64(o, (uint64_t)media);
            mp4_u16(o, 1);
            mp4_u16(o, 0);
            mp4_box_close(o, box);
            mp4_box_close(o, edts);
        }
        c += sz;
    }
    mp4_box_close(o, at);
}

int mp4_init_edit(const uint8_t *init, size_t len, int64_t elst_media, uint64_t dur_ms, mp4_buf_t *out)
{
    for (size_t at = 0; at + 8 <= len;) {
        uint32_t sz = rd32(init + at);
        if (sz < 8 || at + sz > len) return -1;
        copy_edit(out, init + at, sz, elst_media, dur_ms);
        at += sz;
    }
    return out->err ? -1 : 0;
}

/* ---- fragments ---- */

size_t mp4_moof_size(uint32_t n, int with_cts)
{
    return 88 + (size_t)n * (with_cts ? 16 : 12);
}

static uint8_t *w32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
    return p + 4;
}

static uint8_t *hdr(uint8_t *p, uint32_t size, const char *type)
{
    p = w32(p, size);
    memcpy(p, type, 4);
    return p + 4;
}

void mp4_moof_build(uint8_t *out, uint32_t seq, uint64_t tfdt, const mp4_sample_t *s, uint32_t n, int with_cts)
{
    uint32_t size = (uint32_t)mp4_moof_size(n, with_cts);
    uint8_t *p = hdr(out, size, "moof");
    p = hdr(p, 16, "mfhd");
    p = w32(p, 0);
    p = w32(p, seq);
    p = hdr(p, size - 24, "traf");
    p = hdr(p, 16, "tfhd");
    p = w32(p, 0x020000);                               /* default-base-is-moof */
    p = w32(p, 1);                                      /* track_ID */
    p = hdr(p, 20, "tfdt");
    p = w32(p, 1u << 24);                               /* version 1: 64-bit time */
    p = w32(p, (uint32_t)(tfdt >> 32));
    p = w32(p, (uint32_t)tfdt);
    uint32_t flags = 0x000001 | 0x000100 | 0x000200 | 0x000400 | (with_cts ? 0x000800 : 0);
    p = hdr(p, (uint32_t)(20 + (size_t)n * (with_cts ? 16 : 12)), "trun");
    /* Version 1: signed composition offsets (a B-frame may show before
     * its decode time once the decode times are packed). */
    p = w32(p, (with_cts ? 1u << 24 : 0) | flags);
    p = w32(p, n);
    p = w32(p, size + 8);                               /* data_offset: past the mdat header */
    for (uint32_t i = 0; i < n; ++i) {
        p = w32(p, s[i].dur);
        p = w32(p, s[i].size);
        p = w32(p, s[i].flags);
        if (with_cts) p = w32(p, (uint32_t)s[i].cts);
    }
}

static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* Per-sample field count and the offset of the first sample in a trun. */
static void trun_layout(uint32_t flags, size_t *first, size_t *stride)
{
    size_t off = 16;                                    /* header, version/flags, sample_count */
    if (flags & 0x000001) off += 4;
    if (flags & 0x000004) off += 4;
    *first = off;
    *stride = 4 * (size_t)(!!(flags & 0x100) + !!(flags & 0x200) + !!(flags & 0x400) + !!(flags & 0x800));
}

int mp4_moof_parse(const uint8_t *moof, size_t len, mp4_moof_t *m)
{
    memset(m, 0, sizeof *m);
    if (len < 8 || memcmp(moof + 4, "moof", 4)) return -1;
    uint32_t size = r32(moof);
    if (size < 8 || size > len) return -1;
    int have_mfhd = 0, have_tfdt = 0, have_trun = 0, trafs = 0;
    for (size_t at = 8; at + 8 <= size;) {
        uint32_t bs = r32(moof + at);
        if (bs < 8 || at + bs > size) return -1;
        if (!memcmp(moof + at + 4, "mfhd", 4) && bs >= 16) {
            m->seq = r32(moof + at + 12);
            m->seq_at = at + 12;
            have_mfhd = 1;
        } else if (!memcmp(moof + at + 4, "traf", 4)) {
            if (++trafs > 1) return -1;
            for (size_t t = at + 8; t + 8 <= at + bs;) {
                uint32_t ts = r32(moof + t);
                if (ts < 8 || t + ts > at + bs) return -1;
                const uint8_t *b = moof + t;
                if (!memcmp(b + 4, "tfhd", 4)) {
                    if (ts < 16 || !(r32(b + 8) & 0x020000) || r32(b + 12) != 1) return -1;
                } else if (!memcmp(b + 4, "tfdt", 4) && ts >= 16) {
                    m->tfdt_v1 = b[8] == 1;
                    if (m->tfdt_v1 && ts < 20) return -1;
                    m->tfdt = m->tfdt_v1 ? (uint64_t)r32(b + 12) << 32 | r32(b + 16) : r32(b + 12);
                    m->tfdt_at = t + 12;
                    have_tfdt = 1;
                } else if (!memcmp(b + 4, "trun", 4) && ts >= 16) {
                    if (have_trun) return -1;
                    uint32_t fl = r32(b + 8) & 0xFFFFFF;
                    size_t first, stride;
                    trun_layout(fl, &first, &stride);
                    m->n = r32(b + 12);
                    if (!(fl & 0x000001) || !(fl & 0x100) || !(fl & 0x200) || first + (size_t)m->n * stride > ts)
                        return -1;
                    m->data_offset = r32(b + 16);
                    m->trun_at = t;
                    have_trun = 1;
                }
                t += ts;
            }
        }
        at += bs;
    }
    if (!have_mfhd || !have_tfdt || !have_trun) return -1;
    for (uint32_t i = 0; i < m->n; ++i) {
        mp4_sample_t s;
        mp4_moof_sample(moof, m, i, &s);
        m->data_size += s.size;
        m->dur += s.dur;
        if (!i) m->first_flags = s.flags;
    }
    return 0;
}

void mp4_moof_sample(const uint8_t *moof, const mp4_moof_t *m, uint32_t i, mp4_sample_t *s)
{
    const uint8_t *b = moof + m->trun_at;
    uint32_t fl = r32(b + 8) & 0xFFFFFF;
    size_t first, stride;
    trun_layout(fl, &first, &stride);
    const uint8_t *p = b + first + (size_t)i * stride;
    memset(s, 0, sizeof *s);
    if (fl & 0x100) { s->dur = r32(p); p += 4; }
    if (fl & 0x200) { s->size = r32(p); p += 4; }
    if (fl & 0x400) { s->flags = r32(p); p += 4; }
    else s->flags = i == 0 && (fl & 0x004) ? r32(b + 20) : 0;
    if (fl & 0x800) s->cts = (int32_t)r32(p);
}

void mp4_moof_patch(uint8_t *moof, const mp4_moof_t *m, uint32_t seq, uint64_t tfdt)
{
    w32(moof + m->seq_at, seq);
    if (m->tfdt_v1) {
        w32(moof + m->tfdt_at, (uint32_t)(tfdt >> 32));
        w32(moof + m->tfdt_at + 4, (uint32_t)tfdt);
    } else {
        w32(moof + m->tfdt_at, (uint32_t)tfdt);
    }
}
