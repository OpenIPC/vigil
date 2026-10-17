/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rtsp_client.h — minimal RTSP client for IP cameras.
 *
 * RTP over the RTSP TCP connection (interleaved, RFC 2326 §10.12), so no
 * UDP ports, no loss on I-frame bursts and no NAT trouble. Basic and Digest
 * (RFC 2617, no qop) authentication. Only the first video stream of the
 * SDP is set up, and only H.264 / H.265.
 */
#ifndef RTSP_CLIENT_H
#define RTSP_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "vg_types.h"

#define RTSP_SPROP_MAX    3
#define RTSP_SPROP_BYTES  256

typedef struct {
    /* From the URL. */
    char host[128];
    int  port;
    char user[64], pass[64];
    char url[512];                  /* request URL, credentials stripped */

    /* Connection state. */
    int  fd;
    int  cseq;
    char session[128];
    int  session_timeout;           /* seconds, from the Session header */
    int  auth;                      /* 0 none, 1 Basic, 2 Digest */
    char realm[128], nonce[128];

    /* From the SDP. */
    vg_codec_t codec;
    int     pt;                     /* RTP payload type of the video stream */
    char    control[800];           /* absolute URL to SETUP */
    uint8_t sprop[RTSP_SPROP_MAX][RTSP_SPROP_BYTES];
    size_t  sprop_len[RTSP_SPROP_MAX];
    int     sprop_n;

    /* Receive buffer. */
    uint8_t *buf;
    size_t   buf_off, buf_len, buf_cap;   /* unread data is [buf_off, buf_len) */
    size_t   handed_out;            /* bytes of the packet rtsp_read returned */
    char     err[160];              /* last error, for the UI/log */
} rtsp_t;

/* Parse rtsp://[user[:pass]@]host[:port]/path into `r`. Returns 0 or -1. */
int rtsp_init(rtsp_t *r, const char *url);

/* Connect (timeout `connect_ms`), DESCRIBE, SETUP, PLAY. Returns 0 on
 * success; on failure -1 with r->err set and the connection closed. */
int rtsp_open(rtsp_t *r, int connect_ms);

/* Wait up to about one second for the next interleaved packet. Returns 1
 * with the packet, its length and its interleaved channel set (the packet
 * stays valid until the next call), 0 on timeout, -1 on error or
 * disconnect. RTSP replies in between are consumed. */
int rtsp_read(rtsp_t *r, const uint8_t **pkt, size_t *len, int *channel);

/* Send an OPTIONS keepalive (reply is consumed by rtsp_read). */
int rtsp_keepalive(rtsp_t *r);

/* TEARDOWN (best effort) and close. */
void rtsp_close(rtsp_t *r);

void rtsp_free(rtsp_t *r);

#endif /* RTSP_CLIENT_H */
