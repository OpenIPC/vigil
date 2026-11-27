/* SPDX-License-Identifier: GPL-3.0-or-later */
/* netip_proto.c — NETIP's header and media framing (see netip_proto.h). */
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "netip_proto.h"

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void netip_hdr_put(uint8_t out[NETIP_HDR_LEN], const netip_hdr_t *h)
{
    out[0] = 0xff;
    out[1] = h->version;
    out[2] = out[3] = 0;
    put32(out + 4, h->session);
    put32(out + 8, h->seq);
    out[12] = h->total;
    out[13] = h->cur;
    out[14] = h->msgid & 0xff;
    out[15] = h->msgid >> 8;
    put32(out + 16, h->len);
}

int netip_hdr_get(const uint8_t in[NETIP_HDR_LEN], netip_hdr_t *h)
{
    if (in[0] != 0xff) return -1;
    h->version = in[1];
    h->session = get32(in + 4);
    h->seq = get32(in + 8);
    h->total = in[12];
    h->cur = in[13];
    h->msgid = (uint16_t)(in[14] | in[15] << 8);
    h->len = get32(in + 16);
    return 0;
}

uint32_t netip_time_pack(const struct tm *t)
{
    unsigned year = t->tm_year >= 100 ? (unsigned)t->tm_year - 100 : 0;    /* from 2000 */
    if (year > 63) year = 63;
    return (uint32_t)(t->tm_sec & 63) | (uint32_t)(t->tm_min & 63) << 6 | (uint32_t)(t->tm_hour & 31) << 12 |
           (uint32_t)(t->tm_mday & 31) << 17 | (uint32_t)((t->tm_mon + 1) & 15) << 22 | year << 26;
}

void netip_time_unpack(uint32_t v, struct tm *t)
{
    memset(t, 0, sizeof *t);
    t->tm_sec = v & 63;
    t->tm_min = v >> 6 & 63;
    t->tm_hour = v >> 12 & 31;
    t->tm_mday = v >> 17 & 31;
    t->tm_mon = (int)(v >> 22 & 15) - 1;
    t->tm_year = (int)(v >> 26) + 100;
}

size_t netip_frame_head(uint8_t out[NETIP_FRAME_HEAD], int key, vg_codec_t codec, unsigned width,
                        unsigned height, unsigned fps, const struct tm *t, uint32_t len)
{
    if (codec != VG_CODEC_H264 && codec != VG_CODEC_H265) return 0;
    out[0] = out[1] = 0;
    out[2] = 1;
    if (!key) {
        out[3] = 0xfd;
        put32(out + 4, len);
        return 8;
    }
    /* The size in units of 8 pixels, its bits 8-9 in the codec byte: a
     * 2560x1440 H.265 picture says 0x13 (source_netip.c, xm_codec). */
    unsigned w = width / 8, h = height / 8;
    if (w > 1023) w = 1023;
    if (h > 1023) h = 1023;
    out[3] = 0xfc;
    out[4] = (uint8_t)((codec == VG_CODEC_H265 ? 3 : 2) | (w >> 8 & 3) << 4 | (h >> 8 & 3) << 6);
    out[5] = (uint8_t)(fps > 31 ? 31 : fps);
    out[6] = (uint8_t)(w & 255);
    out[7] = (uint8_t)(h & 255);
    put32(out + 8, t ? netip_time_pack(t) : 0);
    put32(out + 12, len);
    return 16;
}

int netip_session_parse(const char *s, uint32_t *v)
{
    if (!s || !*s) return -1;
    char *end;
    unsigned long n = strtoul(s, &end, 0);
    if (*end || n > 0xffffffffUL) return -1;
    *v = (uint32_t)n;
    return 0;
}

int netip_stream_type(const char *s)
{
    if (!s) return -1;
    if (!strcasecmp(s, "Main")) return 0;
    if (!strncasecmp(s, "Extra", 5) || !strcasecmp(s, "Sub")) return 1;
    return -1;
}
