/* SPDX-License-Identifier: GPL-3.0-or-later */
/* source.h — where a channel's video comes from.
 *
 * A source is a protocol driver picked by the URL scheme:
 *
 *   rtsp://[user:pass@]host[:554]/path         RTSP, RTP over TCP (source_rtsp.c)
 *   netip://[user:pass@]host[:34567]/[?ch=N&stream=main|extra]
 *                                              Xiongmai NETIP / DVRIP (source_netip.c)
 *   capture:N                                  local video input N (source_capture.c,
 *                                              through hal_capture.h)
 *
 * Every driver hands out the same thing: Annex-B access units (vg_au_t),
 * which the channel runs through au_norm before decoding and recording.
 * Adding a protocol = one source_<name>.c with a source_ops_t, listed in
 * source.c.
 */
#ifndef SOURCE_H
#define SOURCE_H

#include "au_norm.h"
#include "vg_types.h"

typedef struct source source_t;

typedef struct {
    const char *scheme;
    /* Parse s->url and set up s->priv. Returns 0, or -1 (bad URL). */
    int  (*create)(source_t *s);
    /* Connect and start the stream. Returns 0, or -1 with s->err set.
     * s->codec may stay VG_CODEC_NONE until the first frame says. */
    int  (*open)(source_t *s);
    /* Wait up to about a second; emit any complete access units. Returns
     * 1 if data arrived, 0 if not, -1 on error / disconnect (s->err). */
    int  (*poll)(source_t *s, au_emit_fn emit, void *user);
    void (*close)(source_t *s);
    void (*destroy)(source_t *s);
    /* The platform shows this source itself (local capture): the channel
     * records it but does not decode it. */
    int  self_displayed;
} source_ops_t;

#define SOURCE_PS_MAX  3

struct source {
    const source_ops_t *ops;
    char        url[512];
    char        host[128];          /* for status lines */
    char        err[160];           /* last error */
    vg_codec_t  codec;
    /* Out-of-band parameter sets (e.g. from the SDP), valid after open. */
    const uint8_t *ps[SOURCE_PS_MAX];
    size_t      ps_len[SOURCE_PS_MAX];
    int         ps_n;
    int         index;              /* channel index (capture sources use it) */
    /* Set by the channel: drivers abandon connects and waits when *stop is
     * set, so a stop never waits for a slow camera. */
    const volatile int *stop;
    void       *priv;
};

/* NULL if the scheme is unknown or the URL is malformed. */
source_t *source_create(const char *url, int index);
void      source_destroy(source_t *s);

/* "rtsp, netip, capture" — for usage text. */
const char *source_schemes(void);

/* Small URL helpers shared by the drivers. */
int url_split(const char *url, const char *scheme, char *user, size_t ul,
              char *pass, size_t pl, char *host, size_t hl, int *port,
              const char **path);
int url_query(const char *path, const char *key, char *out, size_t cap);

#endif /* SOURCE_H */
