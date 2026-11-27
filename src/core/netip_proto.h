/* SPDX-License-Identifier: GPL-3.0-or-later */
/* netip_proto.h — Xiongmai NETIP (DVRIP) as a device speaks it: the
 * message header and the media framing, as pure functions, for vigild's
 * NETIP server (src/api/netip_srv.c). The client side is
 * source/source_netip.c; the two must stay each other's inverse, which
 * test_core checks by playing what these write through that parser.
 *
 * Sources: Xiongmai's "DVR interface protocol" V1.0.0 and "stream frame
 * format" documents (in OpenIPC python-dvr, doc/), and what XM cameras
 * and recorders send.
 *
 *   header   20 bytes, little-endian: u8 0xFF, u8 version, u16 0,
 *            u32 session, u32 sequence, u8 total / u8 current packet (in
 *            a media message: u8 channel / u8 end flag), u16 msgid,
 *            u32 body length; bodies at most 16 KiB.
 *   control  JSON + "\n\0"; a reply's msgid is the request's + 1.
 *   media    msgid 1412 (live) messages carrying frames, a frame's header
 *            first and its payload continuing over the next messages:
 *              00 00 01 FC  I-frame: u8 codec (low nibble 2 H.264,
 *                           3 H.265; bits 4-5 bits 8-9 of width/8,
 *                           bits 6-7 those of height/8), u8 fps (bits
 *                           0-4), u8 width/8, u8 height/8, u32 time,
 *                           u32 length                      (16 bytes)
 *              00 00 01 FD  P-frame: u32 length             (8 bytes)
 *            the payload Annex-B; time = second | minute << 6 | hour << 12
 *            | day << 17 | month << 22 | (year - 2000) << 26.
 */
#ifndef NETIP_PROTO_H
#define NETIP_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "vg_types.h"

#define NETIP_HDR_LEN     20
#define NETIP_BODY_MAX    (16u << 10)       /* the documents' limit per message */
#define NETIP_FRAME_HEAD  16                /* room for the longest frame header */

enum {
    NETIP_LOGIN = 1000, NETIP_LOGOUT = 1002, NETIP_KEEPALIVE = 1006,
    NETIP_SYSINFO = 1020,
    NETIP_CONFIG_SET = 1040, NETIP_CONFIG_GET = 1042, NETIP_DEFAULT_GET = 1044,
    NETIP_TITLE_SET = 1046, NETIP_TITLE_GET = 1048,
    NETIP_ABILITY_GET = 1360,
    NETIP_MONITOR = 1410, NETIP_MONITOR_DATA = 1412, NETIP_MONITOR_CLAIM = 1413,
    NETIP_TIME_QUERY = 1452,
};

typedef struct {
    uint8_t  version;
    uint32_t session, seq;
    uint8_t  total, cur;            /* media: channel, end flag */
    uint16_t msgid;
    uint32_t len;
} netip_hdr_t;

void netip_hdr_put(uint8_t out[NETIP_HDR_LEN], const netip_hdr_t *h);
/* 0, or -1 when it does not start with 0xFF. */
int  netip_hdr_get(const uint8_t in[NETIP_HDR_LEN], netip_hdr_t *h);

/* The frame header for an access unit of `len` bytes: an I-frame (`key`)
 * says the codec, the picture's size and fps and the time `t` (local),
 * a P-frame only its length. Returns its length (16 or 8), 0 for a codec
 * NETIP cannot say. */
size_t netip_frame_head(uint8_t out[NETIP_FRAME_HEAD], int key, vg_codec_t codec, unsigned width,
                        unsigned height, unsigned fps, const struct tm *t, uint32_t len);

/* XM's packed date and time, and back (tm_year, tm_mon as struct tm). */
uint32_t netip_time_pack(const struct tm *t);
void     netip_time_unpack(uint32_t v, struct tm *t);

/* "0x0000000B" (or decimal) -> 11. 0, or -1. */
int netip_session_parse(const char *s, uint32_t *v);

/* OPMonitor's StreamType: "Main" 0, "Extra1"/"Extra"/"Sub" 1, -1 other. */
int netip_stream_type(const char *s);

#endif /* NETIP_PROTO_H */
