/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rtsp_server.h — the cameras' streams to other clients over RTSP
 * (docs/rtsp.md): rtsp://<recorder>:<rtsp_server.port>/ch1/main, /ch1/sub
 * and the paths Hikvision and Dahua recorders use, as the cameras send
 * them (stream_tap.h; no re-encoding). RTP over the RTSP connection
 * (interleaved) or over UDP unicast; H.264 and H.265, video only.
 * Digest logins against vigil's users (users.h), whose group needs the
 * live permission.
 *
 * One thread with poll() for every client: the cameras' session threads
 * packetize each AU once per stream into a ring, and the server thread
 * copies the packets out to each client as its socket takes them. A
 * client that falls a ring behind skips to the next keyframe; nothing a
 * client does ever holds up a camera.
 */
#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

#include <stddef.h>

#include "stream_tap.h"

/* From the main thread, after the channels: start serving if
 * rtsp_server.enabled. 0 when serving or off (logged), -1 when it could
 * not listen (logged; vigild runs on without it). */
int  rtsp_server_start(void);
void rtsp_server_stop(void);
/* After a new configuration is applied (main thread): a new port or
 * enabled restarts it (a port that cannot be had after all: back on the
 * old one, the setting put back); auth and max_clients are read as they
 * change. */
void rtsp_server_apply(void);
/* As api_listening (api.h): 1 while it listens, with `listen` what on and
 * `why` the last move that failed ("" if none); 0 with `why` it does not
 * (off, the port in use, ...). Any thread. */
int  rtsp_server_listening(char *listen, size_t cap, char *why, size_t wcap);

/* ---- exposed for tests ------------------------------------------------- */

/* The channel (0-based) and stream (0 main, 1 sub) a request URL or path
 * names: /chN/main, /chN/sub, /chN; /Streaming/Channels/N01|N02 (also
 * under /ISAPI); /h264/chN/main|sub/av_stream; /cam/realmonitor?channel=N
 * &subtype=0|1. A "/trackID=..." after it is the track. 0, or -1. */
int rtsp_server_path(const char *url, int *ch, int *sub);

/* The SDP of a stream. Returns its length, or -1 if it does not fit. */
int rtsp_server_sdp(char *out, size_t cap, vg_codec_t codec, const stream_tap_ps_t *ps,
                    const char *local_ip, const char *title, unsigned long id);

/* A Digest response (RFC 2617) from HA1: with qop "auth" when `qop` is
 * given, the legacy form otherwise. */
void rtsp_server_digest(const char *ha1, const char *nonce, const char *method, const char *uri,
                        const char *nc, const char *cnonce, const char *qop, char out[33]);

#endif /* RTSP_SERVER_H */
