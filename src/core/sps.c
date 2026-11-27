/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sps.c — picture size, reference count and sample aspect ratio from an
 * H.264 (ITU-T H.264 7.3.2.1.1, E.1.1) or H.265 (ITU-T H.265 7.3.2.2,
 * E.2.1) sequence parameter set. */
#include <string.h>

#include "sps.h"

typedef struct {
    uint8_t buf[1024];          /* RBSP: emulation prevention removed */
    size_t  len, bit;
    int     err;
} bits_t;

static void bits_init(bits_t *b, const uint8_t *nal, size_t len)
{
    memset(b, 0, sizeof *b);
    int zeros = 0;
    for (size_t i = 0; i < len && b->len < sizeof b->buf; ++i) {
        if (zeros >= 2 && nal[i] == 3) { zeros = 0; continue; }
        zeros = nal[i] == 0 ? zeros + 1 : 0;
        b->buf[b->len++] = nal[i];
    }
}

static unsigned u(bits_t *b, int n)
{
    unsigned v = 0;
    for (int i = 0; i < n; ++i) {
        if (b->bit >= b->len * 8) { b->err = 1; return 0; }
        v = (v << 1) | ((b->buf[b->bit >> 3] >> (7 - (b->bit & 7))) & 1);
        ++b->bit;
    }
    return v;
}

static unsigned ue(bits_t *b)
{
    int lz = 0;
    while (!b->err && u(b, 1) == 0) if (++lz > 31) { b->err = 1; return 0; }
    return lz ? ((1u << lz) - 1 + u(b, lz)) : 0;
}

static int se(bits_t *b)
{
    unsigned v = ue(b);
    return (v & 1) ? (int)((v + 1) / 2) : -(int)(v / 2);
}

static void skip_scaling_list(bits_t *b, int size)
{
    int last = 8, next = 8;
    for (int j = 0; j < size && !b->err; ++j) {
        if (next) next = (last + se(b) + 256) % 256;
        last = next ? next : last;
    }
}

/* Table E-1, aspect_ratio_idc 1..16 */
static const uint8_t k_sar[17][2] = {
    { 0, 0 }, { 1, 1 }, { 12, 11 }, { 10, 11 }, { 16, 11 }, { 40, 33 }, { 24, 11 }, { 20, 11 }, { 32, 11 },
    { 80, 33 }, { 18, 11 }, { 15, 11 }, { 64, 33 }, { 160, 99 }, { 4, 3 }, { 3, 2 }, { 2, 1 },
};

/* vui_parameters() up to the sample aspect ratio, the same in both
 * codecs, after vui_parameters_present_flag. Best effort: a VUI the
 * parser does not get through leaves the SAR unknown, never the size. */
static void parse_vui_sar(bits_t *b, vg_stream_info_t *o)
{
    unsigned w = 0, h = 0;
    if (u(b, 1)) {                                      /* aspect_ratio_info_present_flag */
        unsigned idc = u(b, 8);
        if (idc == 255) {                               /* Extended_SAR */
            w = u(b, 16);
            h = u(b, 16);
        } else if (idc < 17) {
            w = k_sar[idc][0];
            h = k_sar[idc][1];
        }
    }
    if (b->err || !w || !h) return;
    o->sar_w = w;
    o->sar_h = h;
}

static int parse_h264(bits_t *b, vg_stream_info_t *o)
{
    u(b, 8);                                            /* NAL header */
    unsigned profile = u(b, 8);
    u(b, 16);                                           /* constraints, level */
    ue(b);                                              /* sps id */
    unsigned chroma = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
        profile == 44 || profile == 83 || profile == 86 || profile == 118 ||
        profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
        profile == 135) {
        chroma = ue(b);
        if (chroma == 3) u(b, 1);
        ue(b);                                          /* bit depth luma */
        ue(b);                                          /* bit depth chroma */
        u(b, 1);                                        /* qpprime y zero */
        if (u(b, 1))                                    /* scaling matrix */
            for (int i = 0; i < (chroma == 3 ? 12 : 8); ++i)
                if (u(b, 1)) skip_scaling_list(b, i < 6 ? 16 : 64);
    }
    ue(b);                                              /* log2 max frame num */
    unsigned poc = ue(b);
    if (poc == 0) {
        ue(b);
    } else if (poc == 1) {
        u(b, 1);
        se(b);
        se(b);
        unsigned n = ue(b);
        for (unsigned i = 0; i < n && !b->err; ++i) se(b);
    }
    o->refs = ue(b);
    u(b, 1);                                            /* gaps allowed */
    unsigned w_mbs = ue(b) + 1, h_map = ue(b) + 1;
    unsigned frame_mbs_only = u(b, 1);
    if (!frame_mbs_only) u(b, 1);
    u(b, 1);                                            /* direct 8x8 */
    unsigned cl = 0, cr = 0, ct = 0, cb = 0;
    if (u(b, 1)) { cl = ue(b); cr = ue(b); ct = ue(b); cb = ue(b); }
    unsigned cx = chroma == 0 || chroma == 3 ? 1 : 2;
    unsigned cy = (chroma == 1 ? 2 : 1) * (2 - frame_mbs_only);
    o->width  = w_mbs * 16 - cx * (cl + cr);
    o->height = (2 - frame_mbs_only) * h_map * 16 - cy * (ct + cb);
    if (b->err) return -1;
    if (u(b, 1)) parse_vui_sar(b, o);                   /* vui_parameters_present_flag */
    b->err = 0;
    return 0;
}

static void skip_ptl(bits_t *b, int max_sub_layers_minus1)
{
    u(b, 8);                                            /* space, tier, profile */
    u(b, 32);                                           /* compatibility flags */
    u(b, 32);                                           /* constraint flags ... */
    u(b, 16);
    u(b, 8);                                            /* general level */
    int prof[8] = {0}, lvl[8] = {0};
    for (int i = 0; i < max_sub_layers_minus1; ++i) {
        prof[i] = u(b, 1);
        lvl[i] = u(b, 1);
    }
    if (max_sub_layers_minus1 > 0)
        for (int i = max_sub_layers_minus1; i < 8; ++i) u(b, 2);
    for (int i = 0; i < max_sub_layers_minus1; ++i) {
        if (prof[i]) { u(b, 32); u(b, 32); u(b, 24); }   /* 88 bits */
        if (lvl[i]) u(b, 8);
    }
}

/* scaling_list_data() (7.3.4) */
static void h265_skip_scaling_lists(bits_t *b)
{
    for (int size = 0; size < 4 && !b->err; ++size)
        for (int m = 0; m < 6 && !b->err; m += size == 3 ? 3 : 1) {
            if (!u(b, 1)) {                             /* scaling_list_pred_mode_flag */
                ue(b);                                  /* pred_matrix_id_delta */
                continue;
            }
            int coefs = size == 0 ? 16 : 64;
            if (size > 1) se(b);                        /* dc_coef_minus8 */
            for (int i = 0; i < coefs && !b->err; ++i) se(b);
        }
}

/* The rest of the SPS after the DPB sizes, up to vui_parameters()'s SAR:
 * coding block sizes, scaling lists, PCM, the short-term reference
 * picture sets (7.3.7, each may be predicted from the one before it) and
 * the long-term ones. */
static void h265_vui_sar(bits_t *b, unsigned poc_lsb_bits, vg_stream_info_t *o)
{
    for (int i = 0; i < 6; ++i) ue(b);                  /* block sizes, transform depths */
    if (u(b, 1) && u(b, 1))                             /* scaling_list_enabled, sps_..._data_present */
        h265_skip_scaling_lists(b);
    u(b, 1);                                            /* amp_enabled_flag */
    u(b, 1);                                            /* sample_adaptive_offset_enabled_flag */
    if (u(b, 1)) {                                      /* pcm_enabled_flag */
        u(b, 8);                                        /* pcm bit depths */
        ue(b);
        ue(b);
        u(b, 1);
    }
    unsigned sets = ue(b);
    if (sets > 64) return;
    unsigned deltas[64];
    for (unsigned i = 0; i < sets && !b->err; ++i) {
        if (i && u(b, 1)) {                             /* inter_ref_pic_set_prediction_flag */
            u(b, 1);                                    /* delta_rps_sign */
            ue(b);                                      /* abs_delta_rps_minus1 */
            unsigned n = 0;
            for (unsigned j = 0; j <= deltas[i - 1] && !b->err; ++j) {
                int used = (int)u(b, 1);
                if (used || u(b, 1)) ++n;               /* use_delta_flag */
            }
            deltas[i] = n;
        } else {
            unsigned neg = ue(b), pos = ue(b);
            if (neg > 16 || pos > 16) return;
            for (unsigned j = 0; j < neg + pos && !b->err; ++j) {
                ue(b);                                  /* delta_poc_minus1 */
                u(b, 1);                                /* used_by_curr_pic */
            }
            deltas[i] = neg + pos;
        }
    }
    if (u(b, 1)) {                                      /* long_term_ref_pics_present_flag */
        unsigned lt = ue(b);
        if (lt > 32) return;
        for (unsigned i = 0; i < lt && !b->err; ++i) {
            u(b, (int)poc_lsb_bits);
            u(b, 1);
        }
    }
    u(b, 1);                                            /* sps_temporal_mvp_enabled_flag */
    u(b, 1);                                            /* strong_intra_smoothing_enabled_flag */
    if (!b->err && u(b, 1)) parse_vui_sar(b, o);        /* vui_parameters_present_flag */
}

static int parse_h265(bits_t *b, vg_stream_info_t *o)
{
    u(b, 16);                                           /* NAL header */
    u(b, 4);                                            /* vps id */
    int max_sub = (int)u(b, 3);
    u(b, 1);                                            /* temporal id nesting */
    skip_ptl(b, max_sub);
    ue(b);                                              /* sps id */
    unsigned chroma = ue(b);
    if (chroma == 3) u(b, 1);
    unsigned w = ue(b), h = ue(b);
    unsigned cl = 0, cr = 0, ct = 0, cb = 0;
    if (u(b, 1)) { cl = ue(b); cr = ue(b); ct = ue(b); cb = ue(b); }
    ue(b);                                              /* bit depth luma */
    ue(b);                                              /* bit depth chroma */
    unsigned poc_lsb_bits = ue(b) + 4;                  /* log2_max_pic_order_cnt_lsb */
    int all = (int)u(b, 1);
    unsigned dpb = 1;
    for (int i = all ? 0 : max_sub; i <= max_sub && !b->err; ++i) {
        unsigned d = ue(b) + 1;                         /* max_dec_pic_buffering */
        if (d > dpb) dpb = d;
        ue(b);                                          /* num reorder */
        ue(b);                                          /* max latency */
    }
    unsigned cx = chroma == 1 || chroma == 2 ? 2 : 1, cy = chroma == 1 ? 2 : 1;
    o->width  = w - cx * (cl + cr);
    o->height = h - cy * (ct + cb);
    o->refs   = dpb > 1 ? dpb - 1 : 1;                  /* DPB holds the current picture too */
    if (b->err) return -1;
    h265_vui_sar(b, poc_lsb_bits, o);
    b->err = 0;
    return 0;
}

int sps_parse(vg_codec_t codec, const uint8_t *nal, size_t len, vg_stream_info_t *out)
{
    bits_t b;
    memset(out, 0, sizeof *out);
    bits_init(&b, nal, len);
    int rc = codec == VG_CODEC_H264 ? parse_h264(&b, out) : parse_h265(&b, out);
    if (rc < 0 || !out->width || !out->height || out->width > 8192 || out->height > 8192)
        return -1;
    if (out->refs == 0) out->refs = 1;
    return 0;
}
