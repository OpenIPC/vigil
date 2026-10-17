/* SPDX-License-Identifier: GPL-3.0-or-later */
/* source_rtsp.c — RTSP source: rtsp_client (RTP interleaved over TCP) +
 * rtp_depay. The SDP's sprop parameter sets go out of band to au_norm. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rtp_depay.h"
#include "rtsp_client.h"
#include "source.h"

typedef struct {
    rtsp_t  rtsp;
    depay_t depay;
    int     depay_up;
    time_t  last_ka;
    au_emit_fn emit;
    void   *user;
} rtsp_src_t;

static int rtsp_src_create(source_t *s)
{
    rtsp_src_t *r = calloc(1, sizeof *r);
    if (!r) return -1;
    if (rtsp_init(&r->rtsp, s->url) < 0) {
        free(r);
        return -1;
    }
    snprintf(s->host, sizeof s->host, "%s", r->rtsp.host);
    s->priv = r;
    return 0;
}

static int rtsp_src_open(source_t *s)
{
    rtsp_src_t *r = s->priv;
    if (rtsp_open(&r->rtsp, 3000) < 0) {
        snprintf(s->err, sizeof s->err, "%s", r->rtsp.err);
        return -1;
    }
    s->codec = r->rtsp.codec;
    s->ps_n = r->rtsp.sprop_n;
    for (int i = 0; i < r->rtsp.sprop_n && i < SOURCE_PS_MAX; ++i) {
        s->ps[i] = r->rtsp.sprop[i];
        s->ps_len[i] = r->rtsp.sprop_len[i];
    }
    if (depay_init(&r->depay, s->codec) < 0) {
        rtsp_close(&r->rtsp);
        snprintf(s->err, sizeof s->err, "out of memory");
        return -1;
    }
    r->depay_up = 1;
    r->last_ka = time(NULL);
    return 0;
}

static int rtsp_src_poll(source_t *s, au_emit_fn emit, void *user)
{
    rtsp_src_t *r = s->priv;
    const uint8_t *p;
    size_t n;
    int chan;
    int rc = rtsp_read(&r->rtsp, &p, &n, &chan);
    if (rc < 0) {
        snprintf(s->err, sizeof s->err, "%s", r->rtsp.err);
        return -1;
    }
    if (rc == 1 && chan == 0) depay_push(&r->depay, p, n, (depay_au_cb)emit, user);

    time_t now = time(NULL);
    if (now - r->last_ka >= r->rtsp.session_timeout / 2) {
        rtsp_keepalive(&r->rtsp);
        r->last_ka = now;
    }
    return rc == 1;
}

static void rtsp_src_close(source_t *s)
{
    rtsp_src_t *r = s->priv;
    if (!r) return;
    rtsp_close(&r->rtsp);
    if (r->depay_up) depay_free(&r->depay);
    r->depay_up = 0;
}

static void rtsp_src_destroy(source_t *s)
{
    rtsp_src_t *r = s->priv;
    if (!r) return;
    rtsp_free(&r->rtsp);
    free(r);
    s->priv = NULL;
}

const source_ops_t source_rtsp_ops = {
    .scheme  = "rtsp",
    .create  = rtsp_src_create,
    .open    = rtsp_src_open,
    .poll    = rtsp_src_poll,
    .close   = rtsp_src_close,
    .destroy = rtsp_src_destroy,
};
