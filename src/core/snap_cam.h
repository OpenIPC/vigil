/* SPDX-License-Identifier: GPL-3.0-or-later */
/* snap_cam.h — a still picture (JPEG) from the camera itself: its own
 * encoder, its full resolution, none of the recorder's CPU. Blocking calls
 * with their own time limits, for a worker or an API thread; the camera's
 * user and password come from its URL and are used for the call only.
 *
 * NETIP (netip:// cameras; Xiongmai's "DVR interface protocol" V1.0.0:
 * NET_SNAP_REQ 1560 / NET_SNAP_RSP 1561; OpenIPC python-dvr's snapshot()):
 * a connection of its own, the login (msgid 1000), then
 *   1560 {"Name": "OPSNAP", "SessionID": "0x...", "OPSNAP": {"Channel": N}}
 * answered by 1561 messages whose body is the JPEG itself (FF D8 ...), in
 * one message or split over several (the header's total/current packet
 * bytes say how many), or framed as a media frame of type 0x000001FE (the
 * I-frame's 16-byte header, its length at offset 12) continued over the
 * next messages, as python-dvr's reassemble_bin_payload reads it. A
 * device that cannot answers 1561 with JSON and a Ret other than 100.
 *
 * ONVIF (Media Service Specification, GetSnapshotUri): the device service
 * (cameras[].onvif_url, else the address discovery heard, else port 80
 * and 8899, as onvif_events.h finds it), GetCapabilities (Media's XAddr),
 * GetProfiles (the first profile: a camera's main stream), GetSnapshotUri
 * of it. The URI is then fetched like any snapshot URL.
 *
 * HTTP (GetSnapshotUri's answer, or cameras[].snapshot_url for a camera
 * with neither): GET, with HTTP Digest (MD5, qop auth or none) or Basic
 * when the camera answers 401; chunked or not; at most SNAP_JPEG_MAX.
 */
#ifndef SNAP_CAM_H
#define SNAP_CAM_H

#include <stddef.h>
#include <stdint.h>

#include "netip_proto.h"

#define SNAP_JPEG_MAX    (8u << 20)     /* a 4K camera's JPEG is 1-3 MiB */
#define SNAP_NETIP_REQ   1560
#define SNAP_NETIP_RSP   1561

typedef enum {
    SNAP_OK = 0,
    SNAP_EUNSUPPORTED,              /* the camera has no snapshot this way */
    SNAP_EAUTH,                     /* it refused the user or the password */
    SNAP_EFAIL,                     /* unreachable, no answer, not a JPEG, ... */
} snap_rc_t;

/* A JPEG, malloc'd. */
typedef struct {
    unsigned char *p;
    size_t         len, cap;
} snap_jpeg_t;

void snap_jpeg_free(snap_jpeg_t *j);
/* Append `n` bytes. 0, or -1 (out of memory, or past SNAP_JPEG_MAX). */
int  snap_jpeg_put(snap_jpeg_t *j, const void *p, size_t n);

/* Is it a JPEG a viewer opens (SOI, a frame header, EOI at the end, give or
 * take trailing padding)? 0 with its size in *w, *h (may be NULL), or -1.
 * A trailing "\n\0" or zero padding (NETIP bodies) is cut off `*len`. */
int snap_jpeg_check(const unsigned char *p, size_t *len, unsigned *w, unsigned *h);

/* ---- NETIP ---------------------------------------------------------------------- */

/* The OPSNAP request's JSON body for channel `ch` (0-based) of the session. */
int snap_netip_body(char *out, size_t cap, uint32_t session, int ch);

/* The 1561 messages, one at a time, into a JPEG. */
typedef struct {
    snap_jpeg_t jpeg;
    int         chunks;             /* messages taken */
    int         framed;             /* 0x1FE: the payload's length is known */
    size_t      want;               /* framed: the payload's length */
} snap_netip_rx_t;

/* One 1561 message (its header and body). 1: the JPEG is complete (in
 * rx->jpeg, checked); 0: more to come (or, without a count, maybe: the
 * caller decides after a pause, snap_netip_done); -1 refused or not a
 * snapshot (`err`). Free rx->jpeg in any case. */
int snap_netip_take(snap_netip_rx_t *rx, const netip_hdr_t *h, const uint8_t *body, size_t len, char *err,
                    size_t cap);
/* Nothing more came: 1 when what was taken is a whole JPEG, else 0. */
int snap_netip_done(snap_netip_rx_t *rx);

/* The snapshot of the camera at `url` (netip://user:pass@host[:port]/?ch=N). */
snap_rc_t snap_netip(const char *url, int timeout_ms, const volatile int *stop, snap_jpeg_t *out, char *err,
                     size_t cap);

/* ---- HTTP and ONVIF ----------------------------------------------------------------- */

/* GET `url` (http://[user:pass@]host[:port]/path; `user`/`pass` used when it
 * has none). SNAP_OK with a checked JPEG in `out`. */
snap_rc_t snap_http_get(const char *url, const char *user, const char *pass, int timeout_ms,
                        const volatile int *stop, snap_jpeg_t *out, char *err, size_t cap);

/* The Digest Authorization header for `method` `uri` against the challenge
 * `www` ("Digest realm=..., nonce=..., qop=..."), with client nonce
 * `cnonce` and nonce count 1. 0, or -1 (not a challenge it can answer). */
int snap_digest(const char *www, const char *user, const char *pass, const char *method, const char *uri,
                const char *cnonce, char *out, size_t cap);

/* The camera's ONVIF snapshot URI: the device service at `onvif_url` (NULL
 * or "": found from the host of `cam_url`, whose user and password log
 * in). SNAP_OK with `uri`. `self_port`: vigild's own ONVIF port, not tried
 * on this host (0: none). */
snap_rc_t snap_onvif_uri(const char *cam_url, const char *onvif_url, int self_port, const volatile int *stop,
                         char *uri, size_t ucap, char *err, size_t cap);

/* The user, password and host of any camera URL (scheme://[user[:pass]@]host...). 0, or -1. */
int snap_url_creds(const char *url, char *user, size_t ul, char *pass, size_t pl, char *host, size_t hl);

#endif /* SNAP_CAM_H */
