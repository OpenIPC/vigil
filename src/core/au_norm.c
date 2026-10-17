/* SPDX-License-Identifier: GPL-3.0-or-later */
/* au_norm.c — normalize access units from any source (see au_norm.h). */
#include <stdlib.h>
#include <string.h>

#include "au_norm.h"
#include "sps.h"

#define PEND_MAX  (64 * 1024)           /* picture-less units kept at most */

static const uint8_t START_CODE[4] = { 0, 0, 0, 1 };

int au_norm_init(au_norm_t *n, vg_codec_t codec)
{
    memset(n, 0, sizeof *n);
    n->codec = codec;
    n->need_key = 1;
    return 0;
}

void au_norm_free(au_norm_t *n)
{
    free(n->pend);
    free(n->out);
    memset(n, 0, sizeof *n);
}

void au_norm_reset(au_norm_t *n)
{
    n->need_key = 1;
    n->pend_len = 0;
    n->pend_sps = 0;
}

const uint8_t *au_next_nal(const uint8_t *buf, size_t len, size_t *pos, size_t *nal_len)
{
    size_t i = *pos;
    /* Find the start code. */
    while (i + 3 <= len && !(buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)) ++i;
    if (i + 3 > len) return NULL;
    size_t start = i + 3, end = start;
    while (end + 3 <= len && !(buf[end] == 0 && buf[end + 1] == 0 &&
                               (buf[end + 2] == 1 || (buf[end + 2] == 0 && end + 3 < len && buf[end + 3] == 1))))
        ++end;
    if (end + 3 > len) end = len;
    *pos = end;
    *nal_len = end - start;
    return *nal_len ? buf + start : au_next_nal(buf, len, pos, nal_len);
}

/* NAL properties. */
enum { NAL_OTHER, NAL_VPS, NAL_SPS, NAL_PPS };

static int nal_ps(vg_codec_t c, const uint8_t *nal)
{
    if (c == VG_CODEC_H264) {
        switch (nal[0] & 0x1f) { case 7: return NAL_SPS; case 8: return NAL_PPS; }
    } else {
        switch ((nal[0] >> 1) & 0x3f) { case 32: return NAL_VPS; case 33: return NAL_SPS; case 34: return NAL_PPS; }
    }
    return NAL_OTHER;
}

static int nal_vcl(vg_codec_t c, const uint8_t *nal)
{
    if (c == VG_CODEC_H264) { int t = nal[0] & 0x1f; return t >= 1 && t <= 5; }
    return ((nal[0] >> 1) & 0x3f) < 32;
}

static int nal_key(vg_codec_t c, const uint8_t *nal)
{
    if (c == VG_CODEC_H264) return (nal[0] & 0x1f) == 5;
    int t = (nal[0] >> 1) & 0x3f;
    return t >= 16 && t <= 21;
}

/* Cache slot: H.264 SPS 0, PPS 1; H.265 VPS 0, SPS 1, PPS 2. */
static int ps_slot(vg_codec_t c, int kind)
{
    if (c == VG_CODEC_H264) return kind == NAL_SPS ? 0 : kind == NAL_PPS ? 1 : -1;
    return kind == NAL_VPS ? 0 : kind == NAL_SPS ? 1 : kind == NAL_PPS ? 2 : -1;
}

static void cache_ps(au_norm_t *n, const uint8_t *nal, size_t len, int kind)
{
    int s = ps_slot(n->codec, kind);
    if (s < 0 || len > AU_NORM_PS_BYTES || len < 2) return;
    if (n->ps_len[s] == len && memcmp(n->ps[s], nal, len) == 0) return;
    memcpy(n->ps[s], nal, len);
    n->ps_len[s] = len;
    if (kind == NAL_SPS) {
        vg_stream_info_t si;
        if (sps_parse(n->codec, nal, len, &si) == 0 &&
            (n->info_gen == 0 || si.width != n->info.width ||
             si.height != n->info.height || si.refs != n->info.refs)) {
            n->info = si;
            ++n->info_gen;
        }
    }
}

void au_norm_add_param_set(au_norm_t *n, const uint8_t *nal, size_t len)
{
    if (len >= 2) cache_ps(n, nal, len, nal_ps(n->codec, nal));
}

static int grow(uint8_t **buf, size_t *cap, size_t need)
{
    if (need <= *cap) return 0;
    size_t c = *cap ? *cap : 64 * 1024;
    while (c < need) c *= 2;
    uint8_t *p = realloc(*buf, c);
    if (!p) return -1;
    *buf = p;
    *cap = c;
    return 0;
}

void au_norm_push(au_norm_t *n, const vg_au_t *in, au_emit_fn emit, void *user)
{
    int vcl = 0, key = 0, sps = 0;
    size_t pos = 0, nl;
    const uint8_t *nal;
    while ((nal = au_next_nal(in->data, in->len, &pos, &nl)) != NULL) {
        if (nl < 2) continue;
        int kind = nal_ps(n->codec, nal);
        if (kind != NAL_OTHER) cache_ps(n, nal, nl, kind);
        if (kind == NAL_SPS) sps = 1;
        if (nal_vcl(n->codec, nal)) vcl = 1;
        if (nal_key(n->codec, nal)) key = 1;
    }
    if (in->discont) {
        n->need_key = 1;
        n->pend_len = 0;
        n->pend_sps = 0;
    }

    if (!vcl) {
        /* Parameter sets / SEI on their own: keep them for the next picture. */
        if (n->pend_len + in->len > PEND_MAX || grow(&n->pend, &n->pend_cap, n->pend_len + in->len) < 0) {
            n->pend_len = 0;
            n->pend_sps = 0;
            return;
        }
        memcpy(n->pend + n->pend_len, in->data, in->len);
        n->pend_len += in->len;
        n->pend_sps |= sps;
        return;
    }
    if (n->need_key && !key) {
        n->pend_len = 0;
        n->pend_sps = 0;
        return;
    }
    n->need_key = 0;

    /* Output: [cached parameter sets, for a keyframe that lacks them]
     *         + [carried picture-less units] + the AU. */
    size_t ps = 0;
    int add_ps = key && !sps && !n->pend_sps;
    if (add_ps)
        for (int i = 0; i < AU_NORM_PS_MAX; ++i)
            if (n->ps_len[i]) ps += sizeof START_CODE + n->ps_len[i];

    vg_au_t out = *in;
    out.key = key;
    out.discont = 0;
    if (ps || n->pend_len) {
        size_t total = ps + n->pend_len + in->len;
        if (grow(&n->out, &n->out_cap, total) < 0) return;
        size_t o = 0;
        if (add_ps)
            for (int i = 0; i < AU_NORM_PS_MAX; ++i) {
                if (!n->ps_len[i]) continue;
                memcpy(n->out + o, START_CODE, sizeof START_CODE);
                o += sizeof START_CODE;
                memcpy(n->out + o, n->ps[i], n->ps_len[i]);
                o += n->ps_len[i];
            }
        memcpy(n->out + o, n->pend, n->pend_len);
        o += n->pend_len;
        memcpy(n->out + o, in->data, in->len);
        out.data = n->out;
        out.len = total;
    }
    n->pend_len = 0;
    n->pend_sps = 0;
    emit(user, &out);
}
