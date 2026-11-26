/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_core.c — unit tests for the hardware-free core. */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <pthread.h>

#include "au_norm.h"
#include "channel.h"
#include "cJSON.h"
#include "config.h"
#include "events.h"
#include "hal.h"
#include "hal_capture.h"
#include "hal_decoder.h"
#include "hal_display.h"
#include "net.h"
#include "rec_place.h"
#include "rtp_depay.h"
#include "source.h"
#include "rtsp_client.h"
#include "schema.h"
#include "schema_yaml.h"
#include "seg_writer.h"
#include "sps.h"

static int g_fail, g_checks;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_fail; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static const char *g_data;

static uint8_t *load(const char *name, size_t *len)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", g_data, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    rewind(f);
    uint8_t *b = malloc(*len);
    if (fread(b, 1, *len, f) != *len) exit(1);
    fclose(f);
    return b;
}

static int nal_type(vg_codec_t c, const uint8_t *nal)
{
    return c == VG_CODEC_H264 ? nal[0] & 0x1f : (nal[0] >> 1) & 0x3f;
}

/* First NAL unit of `type` in an Annex-B buffer. */
static const uint8_t *find_nal(const uint8_t *b, size_t len, vg_codec_t c, int type, size_t *nl)
{
    size_t pos = 0;
    const uint8_t *n;
    while ((n = au_next_nal(b, len, &pos, nl)) != NULL)
        if (nal_type(c, n) == type) return n;
    return NULL;
}

/* ---- SPS ----------------------------------------------------------------- */

static void test_sps(void)
{
    static const struct { const char *file; vg_codec_t c; int sps; unsigned w, h, refs; } T[] = {
        { "testsrc_640x360.h264", VG_CODEC_H264, 7, 640, 360, 2 },
        /* x265 ref=1 still signals a DPB of 3 (max_dec_pic_buffering_minus1 = 2) */
        { "testsrc_640x360.h265", VG_CODEC_H265, 33, 640, 360, 2 },
        { "majestic_1080p.h265", VG_CODEC_H265, 33, 1920, 1080, 1 },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; ++i) {
        size_t len, nl;
        uint8_t *b = load(T[i].file, &len);
        const uint8_t *sps = find_nal(b, len, T[i].c, T[i].sps, &nl);
        vg_stream_info_t si;
        CHECK(sps && sps_parse(T[i].c, sps, nl, &si) == 0, "%s: no SPS", T[i].file);
        CHECK(si.width == T[i].w && si.height == T[i].h && si.refs == T[i].refs,
              "%s: %ux%u refs %u", T[i].file, si.width, si.height, si.refs);
        free(b);
    }
}

/* ---- au_norm --------------------------------------------------------------- */

typedef struct { int n, keys; size_t len; int sps_count; uint8_t last[1 << 20]; } sink_t;

static void sink(void *user, const vg_au_t *au)
{
    sink_t *s = user;
    ++s->n;
    s->keys += au->key;
    s->len = au->len;
    memcpy(s->last, au->data, au->len < sizeof s->last ? au->len : sizeof s->last);
    size_t pos = 0, nl;
    const uint8_t *nal;
    s->sps_count = 0;
    while ((nal = au_next_nal(au->data, au->len, &pos, &nl)) != NULL)
        if (nal_type(au->codec, nal) == (au->codec == VG_CODEC_H264 ? 7 : 33)) ++s->sps_count;
}

/* Split a stream into NAL units with start codes, for building AUs. */
typedef struct { const uint8_t *p; size_t len; int type; } nalref_t;

static int split(const uint8_t *b, size_t len, vg_codec_t c, nalref_t *out, int max)
{
    size_t pos = 0, nl;
    const uint8_t *nal;
    int n = 0;
    while (n < max && (nal = au_next_nal(b, len, &pos, &nl)) != NULL) {
        out[n].p = nal;
        out[n].len = nl;
        out[n].type = nal_type(c, nal);
        ++n;
    }
    return n;
}

static vg_au_t make_au(vg_codec_t c, nalref_t *nals, int count, uint8_t *buf)
{
    size_t o = 0;
    for (int i = 0; i < count; ++i) {
        memcpy(buf + o, "\0\0\0\1", 4);
        memcpy(buf + o + 4, nals[i].p, nals[i].len);
        o += 4 + nals[i].len;
    }
    vg_au_t au = { .codec = c, .data = buf, .len = o };
    return au;
}

static void test_au_norm(void)
{
    static sink_t s;
    static uint8_t buf[1 << 20];
    static nalref_t nals[4096];
    size_t len;
    uint8_t *b = load("majestic_1080p.h265", &len);
    int n = split(b, len, VG_CODEC_H265, nals, 4096);
    CHECK(n > 10 && nals[0].type == 32 && nals[1].type == 33 && nals[2].type == 34 && nals[3].type == 19,
          "majestic stream starts VPS SPS PPS IDR (got %d %d %d %d)", nals[0].type, nals[1].type, nals[2].type, nals[3].type);

    au_norm_t a;
    au_norm_init(&a, VG_CODEC_H265);
    memset(&s, 0, sizeof s);

    /* A P-frame before any keyframe is dropped. */
    vg_au_t au = make_au(VG_CODEC_H265, &nals[4], 1, buf);
    au_norm_push(&a, &au, sink, &s);
    CHECK(s.n == 0, "P before keyframe emitted");

    /* majestic: parameter sets as their own "frame", then the IDR. */
    au = make_au(VG_CODEC_H265, &nals[0], 3, buf);
    au_norm_push(&a, &au, sink, &s);
    CHECK(s.n == 0, "parameter-set-only AU emitted");
    au = make_au(VG_CODEC_H265, &nals[3], 1, buf);
    au_norm_push(&a, &au, sink, &s);
    CHECK(s.n == 1 && s.keys == 1 && s.sps_count == 1, "IDR: n=%d keys=%d sps=%d", s.n, s.keys, s.sps_count);
    CHECK(a.info_gen == 1 && a.info.width == 1920 && a.info.height == 1080, "stream info");

    /* A later IDR without in-band parameter sets gets the cached ones. */
    au = make_au(VG_CODEC_H265, &nals[3], 1, buf);
    au_norm_push(&a, &au, sink, &s);
    CHECK(s.n == 2 && s.sps_count == 1, "cached parameter sets prepended (sps=%d)", s.sps_count);

    /* P-frames pass; after a discontinuity they wait for a keyframe. */
    au = make_au(VG_CODEC_H265, &nals[4], 1, buf);
    au_norm_push(&a, &au, sink, &s);
    CHECK(s.n == 3, "P after keyframe");
    au.discont = 1;
    au_norm_push(&a, &au, sink, &s);
    au.discont = 0;
    au_norm_push(&a, &au, sink, &s);
    CHECK(s.n == 3, "P after discontinuity emitted");
    au = make_au(VG_CODEC_H265, &nals[3], 1, buf);
    au_norm_push(&a, &au, sink, &s);
    CHECK(s.n == 4 && s.keys == 3, "keyframe after discontinuity");
    CHECK(a.info_gen == 1, "info_gen stable for the same SPS");

    /* A keyframe that carries its SPS but not its VPS/PPS gets the cached
     * VPS and PPS (not "has an SPS, so it has them all"). */
    nalref_t sps_idr[2] = { nals[1], nals[3] };
    au = make_au(VG_CODEC_H265, sps_idr, 2, buf);
    au_norm_push(&a, &au, sink, &s);
    {
        size_t pos = 0, nl;
        const uint8_t *nal;
        int vps = 0, pps = 0;
        while ((nal = au_next_nal(s.last, s.len, &pos, &nl)) != NULL) {
            vps += nal_type(VG_CODEC_H265, nal) == 32;
            pps += nal_type(VG_CODEC_H265, nal) == 34;
        }
        CHECK(s.n == 5 && s.sps_count == 1 && vps == 1 && pps == 1,
              "SPS-only keyframe: sps=%d vps=%d pps=%d", s.sps_count, vps, pps);
    }

    au_norm_free(&a);
    free(b);
}

/* ---- RTP depacketizer -------------------------------------------------------- */

typedef struct { int n, discont; size_t len; uint8_t data[1 << 20]; } dsink_t;

static void dsink(void *user, const vg_au_t *au)
{
    dsink_t *s = user;
    ++s->n;
    s->discont += au->discont;
    s->len = au->len;
    memcpy(s->data, au->data, au->len);
}

/* Packetize one AU (NAL list) like a camera: aggregate the small leading
 * units (STAP-A / AP), fragment big ones (FU-A / FU), marker on the last. */
static int packetize(vg_codec_t c, nalref_t *nals, int count, uint16_t *seq, uint32_t ts,
                     uint8_t pkts[][1500], size_t *plen, int max_pkts, size_t mtu)
{
    int np = 0;
    size_t hl = c == VG_CODEC_H264 ? 1 : 2;
    int i = 0;
    while (i < count && np < max_pkts) {
        uint8_t *p = pkts[np];
        p[0] = 0x80; p[1] = 96;
        p[2] = *seq >> 8; p[3] = *seq & 0xff; ++*seq;
        p[4] = ts >> 24; p[5] = ts >> 16; p[6] = ts >> 8; p[7] = ts;
        memset(p + 8, 0, 4);
        size_t o = 12;
        if (nals[i].len + 12 <= mtu && i + 1 < count && nals[i + 1].len + nals[i].len + 20 <= mtu) {
            /* aggregation of as many small units as fit */
            if (c == VG_CODEC_H264) p[o++] = 24;
            else { p[o++] = 48 << 1; p[o++] = 1; }
            while (i < count && o + 2 + nals[i].len <= mtu) {
                p[o++] = nals[i].len >> 8; p[o++] = nals[i].len & 0xff;
                memcpy(p + o, nals[i].p, nals[i].len); o += nals[i].len; ++i;
            }
        } else if (nals[i].len + 12 <= mtu) {
            memcpy(p + o, nals[i].p, nals[i].len); o += nals[i].len; ++i;
        } else {
            /* fragmentation */
            size_t off = hl, chunk = mtu - 12 - hl - 1;
            int first = 1;
            while (off < nals[i].len) {
                if (!first) {                   /* close the previous fragment */
                    plen[np++] = o;
                    p = pkts[np];
                    p[0] = 0x80; p[1] = 96;
                    p[2] = *seq >> 8; p[3] = *seq & 0xff; ++*seq;
                    p[4] = ts >> 24; p[5] = ts >> 16; p[6] = ts >> 8; p[7] = ts;
                    memset(p + 8, 0, 4);
                    o = 12;
                }
                size_t take = nals[i].len - off < chunk ? nals[i].len - off : chunk;
                int last = off + take == nals[i].len;
                if (c == VG_CODEC_H264) {
                    p[o++] = (nals[i].p[0] & 0xe0) | 28;
                    p[o++] = (first ? 0x80 : 0) | (last ? 0x40 : 0) | (nals[i].p[0] & 0x1f);
                } else {
                    p[o++] = (nals[i].p[0] & 0x81) | (49 << 1);
                    p[o++] = nals[i].p[1];
                    p[o++] = (first ? 0x80 : 0) | (last ? 0x40 : 0) | nal_type(c, nals[i].p);
                }
                memcpy(p + o, nals[i].p + off, take);
                o += take;
                off += take;
                first = 0;
            }
            ++i;
        }
        plen[np++] = o;
    }
    pkts[np - 1][1] |= 0x80;
    return np;
}

static void depay_case(const char *file, vg_codec_t c)
{
    static uint8_t pkts[512][1500], expect[1 << 20];
    static size_t plen[512];
    static nalref_t nals[4096];
    static dsink_t s;
    size_t len;
    uint8_t *b = load(file, &len);
    int n = split(b, len, c, nals, 4096);
    /* AU 0: everything up to and including the first picture. */
    int first_vcl = 0;
    while (first_vcl < n && !(c == VG_CODEC_H264 ? (nals[first_vcl].type >= 1 && nals[first_vcl].type <= 5)
                                                 : nals[first_vcl].type < 32)) ++first_vcl;
    int cnt = first_vcl + 1;
    size_t elen = 0;
    for (int i = 0; i < cnt; ++i) {
        memcpy(expect + elen, "\0\0\0\1", 4);
        memcpy(expect + elen + 4, nals[i].p, nals[i].len);
        elen += 4 + nals[i].len;
    }

    depay_t d;
    depay_init(&d, c);
    memset(&s, 0, sizeof s);
    uint16_t seq = 100;
    int np = packetize(c, nals, cnt, &seq, 9000, pkts, plen, 512, 1400);
    for (int i = 0; i < np; ++i) depay_push(&d, pkts[i], plen[i], dsink, &s);
    CHECK(s.n == 1 && s.len == elen && memcmp(s.data, expect, elen) == 0,
          "%s: round trip (%d packets, n=%d len=%zu/%zu)", file, np, s.n, s.len, elen);

    /* Lose one packet of the next AU: it is dropped, the one after is flagged. */
    np = packetize(c, nals, cnt, &seq, 12600, pkts, plen, 512, 1400);
    for (int i = 0; i < np; ++i) if (i != np / 2) depay_push(&d, pkts[i], plen[i], dsink, &s);
    CHECK(s.n == 1, "%s: AU with a lost packet emitted", file);
    np = packetize(c, nals, cnt, &seq, 16200, pkts, plen, 512, 1400);
    for (int i = 0; i < np; ++i) depay_push(&d, pkts[i], plen[i], dsink, &s);
    CHECK(s.n == 2 && s.discont == 1 && d.lost == 1, "%s: discontinuity flagged (n=%d discont=%d lost=%llu)",
          file, s.n, s.discont, (unsigned long long)d.lost);

    /* Lose the FIRST packet of an AU whose timestamp is new: the packet that
     * reveals the gap also starts the AU, and that AU must still be dropped. */
    np = packetize(c, nals, cnt, &seq, 19800, pkts, plen, 512, 1400);
    for (int i = 1; i < np; ++i) depay_push(&d, pkts[i], plen[i], dsink, &s);
    CHECK(s.n == 2, "%s: AU missing its first packet emitted", file);
    np = packetize(c, nals, cnt, &seq, 23400, pkts, plen, 512, 1400);
    for (int i = 0; i < np; ++i) depay_push(&d, pkts[i], plen[i], dsink, &s);
    CHECK(s.n == 3 && s.discont == 2, "%s: next AU flagged (n=%d discont=%d)", file, s.n, s.discont);
    depay_free(&d);
    free(b);
}

static void test_depay(void)
{
    depay_case("testsrc_640x360.h264", VG_CODEC_H264);
    depay_case("testsrc_640x360.h265", VG_CODEC_H265);
}

/* ---- RTSP interleaved reader ---------------------------------------------------- */

static void test_rtsp_read(void)
{
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    struct timeval tv = { .tv_usec = 100000 };
    setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    rtsp_t r;
    rtsp_init(&r, "rtsp://test/");
    r.fd = sv[0];

    const uint8_t *p;
    size_t n;
    int ch;
    /* A frame whose 4-byte header arrives in pieces must not be dropped. */
    if (write(sv[1], "$", 1) != 1) return;
    CHECK(rtsp_read(&r, &p, &n, &ch) == 0, "partial header returned a packet");
    if (write(sv[1], "\0\0", 2) != 2) return;
    CHECK(rtsp_read(&r, &p, &n, &ch) == 0, "partial header returned a packet");
    if (write(sv[1], "\5hello$\1\0\2ab", 13) != 13) return;
    CHECK(rtsp_read(&r, &p, &n, &ch) == 1 && n == 5 && ch == 0 && memcmp(p, "hello", 5) == 0, "first packet");
    CHECK(rtsp_read(&r, &p, &n, &ch) == 1 && n == 2 && ch == 1 && memcmp(p, "ab", 2) == 0, "second packet");
    /* An RTSP reply in between is consumed, not mistaken for data. */
    const char *msg = "RTSP/1.0 200 OK\r\nCSeq: 7\r\n\r\n$\0\0\1x";
    if (write(sv[1], msg, strlen(msg) + 5) < 0) return;
    CHECK(rtsp_read(&r, &p, &n, &ch) == 1 && n == 1 && p[0] == 'x', "packet after RTSP reply");
    close(sv[1]);
    rtsp_free(&r);
}

/* ---- RTSP request headers ------------------------------------------------------------ */

static void test_rtsp_request(void)
{
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    rtsp_t r;
    rtsp_init(&r, "rtsp://cam:8554/live");
    r.fd = sv[0];
    snprintf(r.session, sizeof r.session, "12345678");
    CHECK(rtsp_keepalive(&r) == 0, "keepalive sent");
    char req[1024];
    ssize_t n = read(sv[1], req, sizeof req - 1);
    req[n > 0 ? n : 0] = 0;
    CHECK(strncmp(req, "OPTIONS rtsp://cam:8554/live RTSP/1.0\r\n", 39) == 0, "request line: %.40s", req);
    CHECK(strstr(req, "\r\nCSeq: 1\r\n") != NULL, "CSeq");
    CHECK(strstr(req, "\r\nUser-Agent: vigil\r\n") != NULL, "User-Agent");
    CHECK(strstr(req, "\r\nSession: 12345678\r\n") != NULL, "Session");
    CHECK(n > 4 && strcmp(req + n - 4, "\r\n\r\n") == 0, "request ends with an empty line");
    close(sv[1]);
    r.fd = -1;
    close(sv[0]);
    rtsp_free(&r);
}

/* ---- RTSP helpers ------------------------------------------------------------------- */

static void test_rtsp_helpers(void)
{
    /* RFC 2617 section 3.5 example (qop=auth). */
    char r[33];
    rtsp_digest_response("Mufasa", "testrealm@host.com", "Circle Of Life", "GET", "/dir/index.html",
                         "dcd98b7102dd2f0e8b11d0f600bfb0c093", "00000001", "0a4f113b", "auth", r);
    CHECK(strcmp(r, "6629fae49393a05397450978507c4ef1") == 0, "digest qop=auth: %s", r);
    rtsp_digest_response("u", "r", "p", "DESCRIBE", "rtsp://h/s", "n", "", "", NULL, r);
    CHECK(strlen(r) == 32, "legacy digest");
    CHECK(rtsp_transport_channel("RTP/AVP/TCP;unicast;interleaved=2-3;ssrc=1234") == 2, "interleaved=2-3");
    CHECK(rtsp_transport_channel("RTP/AVP/TCP;unicast") == 0, "no interleaved");
}

/* ---- capture source (fake HAL) ------------------------------------------------------- */

static hal_cap_cb g_cap_cb;
static void      *g_cap_user;
int  hal_cap_start(int in, int ch, hal_cap_cb cb, void *user) { (void)ch; if (in != 0) return -1; g_cap_cb = cb; g_cap_user = user; return 0; }
void hal_cap_stop(int in) { (void)in; g_cap_cb = NULL; }
int  hal_cap_signal(int in) { return in == 0; }

typedef struct { int n, discont, first_discont_at; uint8_t first[8]; } csink_t;

static void csink(void *user, const vg_au_t *au)
{
    csink_t *s = user;
    if (au->discont && s->first_discont_at < 0) s->first_discont_at = s->n;
    if (s->n == 0) memcpy(s->first, au->data, 4);
    s->discont += au->discont;
    ++s->n;
}

static void test_capture_source(void)
{
    static const volatile int stop = 0;
    CHECK(source_create("capture:x", 0) == NULL, "bad capture URL accepted");
    source_t *src = source_create("capture:0", 2);
    CHECK(src && src->ops->self_displayed, "capture source");
    if (!src) return;
    src->stop = &stop;
    CHECK(src->ops->open(src) == 0 && g_cap_cb, "capture open");

    /* 40 AUs into a 32-deep queue: the 8 oldest are dropped, and the gap is
     * marked on the first AU that survives it. */
    uint8_t data[4];
    for (int i = 0; i < 40; ++i) {
        memcpy(data, &i, sizeof i);
        vg_au_t au = { .codec = VG_CODEC_H264, .data = data, .len = 4, .key = i == 0 };
        g_cap_cb(g_cap_user, &au);
    }
    csink_t cs = { .first_discont_at = -1 };
    CHECK(src->ops->poll(src, csink, &cs) == 1, "capture poll");
    int first;
    memcpy(&first, cs.first, sizeof first);
    CHECK(cs.n == 32 && first == 8 && cs.discont == 1 && cs.first_discont_at == 0,
          "overflow: n=%d first=%d discont=%d at=%d", cs.n, first, cs.discont, cs.first_discont_at);
    source_destroy(src);
    CHECK(g_cap_cb == NULL, "capture stopped on destroy");
}

/* ---- segment writer ---------------------------------------------------------------- */

static int count_files(const char *dir)
{
    DIR *d = opendir(dir);
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) n += e->d_name[0] != '.';
    if (d) closedir(d);
    return n;
}

static void test_seg_writer(void)
{
    char dir[] = "/tmp/vigil-test-XXXXXX";
    if (!mkdtemp(dir)) return;
    seg_writer_t w;
    CHECK(seg_init(&w, dir, "CAM1", 1, 2) == 0, "seg_init");
    seg_set_ext(&w, "h265");
    uint8_t data[1000] = {0};
    seg_write(&w, data, sizeof data, 0);
    CHECK(count_files(dir) == 0 && w.bytes == 0, "written before the first keyframe");
    seg_write(&w, data, sizeof data, 1);
    CHECK(count_files(dir) == 1 && w.bytes == 1000, "first segment");

    /* Closed and reopened within the same second: a second file, not a
     * truncated first one. */
    seg_close(&w);
    seg_write(&w, data, sizeof data, 1);
    CHECK(count_files(dir) == 2, "same-second reopen: %d files", count_files(dir));
    for (int i = 0; i < 3; ++i) {
        sleep(1);
        seg_write(&w, data, sizeof data, 1);           /* rotates every second */
    }
    seg_close(&w);
    CHECK(count_files(dir) == 2, "keep=2 left %d files", count_files(dir));

    /* Space guard: an idle open segment of another writer is never deleted,
     * and one check deletes at most one finished file. */
    seg_writer_t idle, busy;
    seg_init(&idle, dir, "CAM2", 3600, 0);
    seg_set_ext(&idle, "h265");
    seg_write(&idle, data, sizeof data, 1);
    fflush(idle.f);
    char idle_name[320];
    snprintf(idle_name, sizeof idle_name, "%s", seg_current(&idle));
    sleep(3);                                       /* older than any mtime heuristic */
    seg_init(&busy, dir, "CAM3", 3600, 0);
    seg_set_ext(&busy, "h265");
    busy.reserve = UINT64_MAX;                      /* always "low on space" */
    int before = count_files(dir);
    seg_write(&busy, data, sizeof data, 1);
    struct stat st;
    CHECK(stat(idle_name, &st) == 0, "the guard deleted another writer's open segment");
    CHECK(count_files(dir) >= before - 1 + 1, "one check deleted more than one file (%d -> %d)",
          before, count_files(dir));
    seg_close(&idle);
    seg_close(&busy);

    char cmd[64];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd)) {}
}


/* ---- net_connect gives up on a stop -------------------------------------------- */

static volatile int g_net_stop;

static void *set_stop_later(void *arg)
{
    (void)arg;
    usleep(300 * 1000);
    g_net_stop = 1;
    return NULL;
}

static double now_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static void test_net_connect_stop(void)
{
    /* A listener whose accept queue is full: Linux drops further SYNs, so
     * a connect stays pending as it does to a camera that is gone. */
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof a;
    if (l < 0 || bind(l, (struct sockaddr *)&a, sizeof a) < 0 || listen(l, 0) < 0 ||
        getsockname(l, (struct sockaddr *)&a, &al) < 0) {
        fprintf(stderr, "net_connect test: no listener, skipped\n");
        return;
    }
    int fill[4];
    for (int i = 0; i < 4; ++i) {
        fill[i] = socket(AF_INET, SOCK_STREAM, 0);
        fcntl(fill[i], F_SETFL, O_NONBLOCK);
        if (connect(fill[i], (struct sockaddr *)&a, sizeof a)) {}
    }
    usleep(100 * 1000);
    char port[8], err[64];
    snprintf(port, sizeof port, "%d", ntohs(a.sin_port));
    pthread_t t;
    g_net_stop = 0;
    pthread_create(&t, NULL, set_stop_later, NULL);
    double t0 = now_s();
    int fd = net_connect("127.0.0.1", ntohs(a.sin_port), 5000, &g_net_stop, err, sizeof err);
    double took = now_s() - t0;
    pthread_join(t, NULL);
    CHECK(fd < 0 && took < 1.5 && strstr(err, "stopped"), "connect: fd %d after %.1f s (%s)", fd, took, err);
    if (fd >= 0) close(fd);
    for (int i = 0; i < 4; ++i) close(fill[i]);
    close(l);
}

/* ---- settings schema ------------------------------------------------------------- */

/* A schema of its own, with every field type (vigild's has no enum yet). */
typedef struct { char host[16]; int port; } t_item_t;
typedef struct {
    struct { char mode[8]; int level; int on; char tag[8]; } main;
    t_item_t items[3];
    int      n_items;
} t_root_t;

static int t_check_tag(const char *v, char *msg, size_t cap)
{
    if (strchr(v, ' ')) { snprintf(msg, cap, "no spaces"); return -1; }
    return 0;
}

static const char *const t_modes[] = { "fast", "slow", NULL };
static const schema_field_t t_main[] = {
    { .key = "mode", .type = SCHEMA_ENUM, .off = offsetof(t_root_t, main.mode) - offsetof(t_root_t, main),
      .size = 8, .def_str = "fast", .values = t_modes },
    { .key = "level", .type = SCHEMA_INT, .off = offsetof(t_root_t, main.level) - offsetof(t_root_t, main),
      .def_int = 5, .min = 1, .max = 10 },
    { .key = "on", .type = SCHEMA_BOOL, .off = offsetof(t_root_t, main.on) - offsetof(t_root_t, main),
      .def_int = 1 },
    { .key = "tag", .type = SCHEMA_STRING, .off = offsetof(t_root_t, main.tag) - offsetof(t_root_t, main),
      .size = 8, .def_str = "x", .check = t_check_tag },
};
static const schema_field_t t_item[] = {
    { .key = "host", .type = SCHEMA_STRING, .off = offsetof(t_item_t, host), .size = 16, .required = 1 },
    { .key = "port", .type = SCHEMA_INT, .off = offsetof(t_item_t, port), .def_int = 80, .min = 1, .max = 65535 },
};
static const schema_section_t t_sections[] = {
    { .key = "main", .off = offsetof(t_root_t, main), .fields = t_main, .n_fields = 4 },
    { .key = "items", .is_array = 1, .off = offsetof(t_root_t, items), .count_off = offsetof(t_root_t, n_items),
      .item_size = sizeof(t_item_t), .max_items = 3, .fields = t_item, .n_fields = 2 },
};
static const schema_t t_schema = { .version = 1, .sections = t_sections, .n_sections = 2 };

/* Parse `json` into a fresh t_root_t; returns schema_from_json's result. */
static int t_parse(const char *json, t_root_t *r, int strict, schema_errors_t *errs)
{
    memset(r, 0, sizeof *r);
    memset(errs, 0, sizeof *errs);
    schema_defaults(&t_schema, r);
    cJSON *doc = cJSON_Parse(json);
    if (!doc) return -2;
    int rc = schema_from_json(&t_schema, doc, r, strict, errs);
    cJSON_Delete(doc);
    return rc;
}

static int has_error(const schema_errors_t *e, const char *path, const char *msg_part)
{
    for (int i = 0; i < e->n; ++i)
        if (!strcmp(e->e[i].path, path) && strstr(e->e[i].msg, msg_part)) return 1;
    return 0;
}

static void test_schema(void)
{
    t_root_t r;
    schema_errors_t e;

    CHECK(t_parse("{}", &r, 1, &e) == 0 && !strcmp(r.main.mode, "fast") && r.main.level == 5 &&
          r.main.on == 1 && !strcmp(r.main.tag, "x") && r.n_items == 0, "defaults");
    CHECK(t_parse("{\"version\":1,\"main\":{\"mode\":\"slow\",\"level\":10,\"on\":false},"
                  "\"items\":[{\"host\":\"a\"},{\"host\":\"b\",\"port\":554}]}", &r, 1, &e) == 0,
          "good document: %s", e.n ? e.e[0].msg : "");
    CHECK(!strcmp(r.main.mode, "slow") && r.main.level == 10 && r.main.on == 0 && r.n_items == 2 &&
          r.items[0].port == 80 && r.items[1].port == 554 && !strcmp(r.items[1].host, "b"), "values");

    /* limits, types, enums */
    CHECK(t_parse("{\"main\":{\"level\":11}}", &r, 1, &e) < 0 && has_error(&e, "main.level", "out of range 1..10"),
          "max");
    CHECK(t_parse("{\"main\":{\"level\":0}}", &r, 1, &e) < 0 && has_error(&e, "main.level", "out of range"), "min");
    CHECK(t_parse("{\"main\":{\"level\":2.5}}", &r, 1, &e) < 0 && has_error(&e, "main.level", "integer"),
          "fraction");
    CHECK(t_parse("{\"main\":{\"level\":1e12}}", &r, 1, &e) < 0 && has_error(&e, "main.level", "integer"),
          "beyond int");
    CHECK(t_parse("{\"main\":{\"level\":\"5\"}}", &r, 1, &e) < 0 && has_error(&e, "main.level", "integer"),
          "string for int");
    CHECK(t_parse("{\"main\":{\"on\":1}}", &r, 1, &e) < 0 && has_error(&e, "main.on", "true or false"),
          "number for bool");
    CHECK(t_parse("{\"main\":{\"mode\":\"medium\"}}", &r, 1, &e) < 0 &&
          has_error(&e, "main.mode", "one of: fast, slow"), "enum");
    CHECK(t_parse("{\"main\":{\"tag\":\"a b\"}}", &r, 1, &e) < 0 && has_error(&e, "main.tag", "no spaces"),
          "custom check");
    CHECK(t_parse("{\"main\":{\"tag\":\"12345678\"}}", &r, 1, &e) < 0 && has_error(&e, "main.tag", "at most 7"),
          "too long");
    CHECK(t_parse("{\"main\":{\"tag\":\"a\\nb\"}}", &r, 1, &e) < 0 && has_error(&e, "main.tag", "control"),
          "control character");
    CHECK(t_parse("{\"main\":[]}", &r, 1, &e) < 0 && has_error(&e, "main", "object"), "array for object");
    CHECK(t_parse("{\"items\":{}}", &r, 1, &e) < 0 && has_error(&e, "items", "array"), "object for array");
    CHECK(t_parse("{\"items\":[{},{},{},{}]}", &r, 1, &e) < 0 && has_error(&e, "items", "at most 3"),
          "too many items");
    CHECK(t_parse("{\"items\":[{\"port\":1}]}", &r, 1, &e) < 0 && has_error(&e, "items[0].host", "required"),
          "required");
    CHECK(t_parse("{\"items\":[{\"host\":\"\"}]}", &r, 1, &e) < 0 && has_error(&e, "items[0].host", "empty"),
          "required but empty");
    CHECK(t_parse("{\"version\":2}", &r, 1, &e) < 0 && has_error(&e, "version", "must be 1"), "version");
    CHECK(t_parse("[]", &r, 1, &e) < 0, "not an object");

    /* every error is reported, not only the first */
    CHECK(t_parse("{\"main\":{\"level\":0,\"on\":2},\"items\":[{\"port\":0}]}", &r, 1, &e) < 0 && e.total == 4,
          "all errors: %d", e.total);

    /* unknown keys: errors from the API (strict), skipped in a file */
    CHECK(t_parse("{\"main\":{\"levle\":3},\"extra\":{}}", &r, 1, &e) < 0 &&
          has_error(&e, "main.levle", "unknown setting") && has_error(&e, "extra", "unknown section"),
          "unknown keys, strict");
    CHECK(t_parse("{\"main\":{\"levle\":3,\"level\":4},\"extra\":{}}", &r, 0, &e) == 0 && r.main.level == 4,
          "unknown keys, lenient");

    /* schema_validate: the same limits on a struct built in C */
    t_parse("{}", &r, 1, &e);
    r.main.level = 99;
    snprintf(r.main.mode, sizeof r.main.mode, "x");
    memset(&e, 0, sizeof e);
    CHECK(schema_validate(&t_schema, &r, &e) < 0 && has_error(&e, "main.level", "range") &&
          has_error(&e, "main.mode", "one of"), "validate");

    /* to JSON and back */
    t_parse("{\"main\":{\"mode\":\"slow\",\"level\":7,\"on\":false,\"tag\":\"t\"},\"items\":[{\"host\":\"h\",\"port\":9}]}",
            &r, 1, &e);
    cJSON *doc = schema_to_json(&t_schema, &r, 0);
    char *text = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    t_root_t r2;
    CHECK(text && t_parse(text, &r2, 1, &e) == 0 && r2.main.level == 7 && !r2.main.on &&
          !strcmp(r2.main.mode, "slow") && r2.n_items == 1 && r2.items[0].port == 9, "round trip: %s", text);
    free(text);

    /* the schema describes itself */
    doc = schema_describe(&t_schema);
    text = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    CHECK(text && strstr(text, "\"path\":\"main.mode\"") && strstr(text, "\"values\":[\"fast\",\"slow\"]") &&
          strstr(text, "\"path\":\"items[].port\"") && strstr(text, "\"min\":1,\"max\":65535"),
          "describe: %s", text ? text : "");
    free(text);
}

static void test_redact(void)
{
    static const char *const cases[][2] = {
        { "rtsp://admin:12345@10.0.0.2/stream", "rtsp://admin:***@10.0.0.2/stream" },
        { "rtsp://10.0.0.2:554/stream", "rtsp://10.0.0.2:554/stream" },
        { "rtsp://admin@10.0.0.2/s", "rtsp://admin@10.0.0.2/s" },
        { "netip://admin:@10.0.0.3:34567/?ch=0", "netip://admin:@10.0.0.3:34567/?ch=0" },
        { "netip://admin:p@ss/w@10.0.0.3/?ch=0", "netip://admin:***@10.0.0.3/?ch=0" },
        /* the RTSP parser accepts '?' and '#' in a password */
        { "rtsp://u:pa?ss@h/x", "rtsp://u:***@h/x" },
        { "rtsp://u:pa#ss@h/x", "rtsp://u:***@h/x" },
        { "netip://u:p?a@ss@h:34567/?ch=1", "netip://u:***@h:34567/?ch=1" },
        /* an '@' in the query hides too much, never too little */
        { "rtsp://u:pw@h/x?token=a@b", "rtsp://u:***@b" },
        { "rtsp://h/x?token=a@b", "rtsp://h/x?token=a@b" },
        { "capture:0", "capture:0" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        char out[128];
        schema_redact_url(cases[i][0], out, sizeof out);
        CHECK(!strcmp(out, cases[i][1]), "redact %s -> %s", cases[i][0], out);
    }
}

/* ---- configuration file and store ---------------------------------------------------------- */

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *b = calloc(1, 65536);
    if (fread(b, 1, 65535, f)) {}
    fclose(f);
    return b;
}

static char *config_text(const vigil_config_t *c)
{
    cJSON *doc = schema_to_json(&config_schema, c, 0);
    char *t = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    return t;
}

/* Write `text` to `path` and load it into `d`, errors into `e`. */
static int load_text(const char *path, const char *text, vigil_config_t *d, schema_errors_t *e)
{
    write_file(path, text);
    memset(e, 0, sizeof *e);
    return config_load_file(path, d, e);
}

static void test_config_file(void)
{
    char dir[] = "/tmp/vigil-cfg-XXXXXX", path[128], tmp[160], err[160];
    if (!mkdtemp(dir)) return;
    snprintf(path, sizeof path, "%s/vigil.yaml", dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    vigil_config_t c, d;
    schema_errors_t e = { 0 };

    CHECK(config_format("/etc/vigil.yaml") == CONFIG_YAML && config_format("vigil.yml") == CONFIG_YAML &&
          config_format("/etc/vigil") == CONFIG_YAML && config_format("/etc/vigil.json") == CONFIG_JSON &&
          config_format(".json") == CONFIG_JSON && config_format("json") == CONFIG_YAML, "format by name");
    CHECK(config_load_file(path, &c, &e) == 1 && c.n_cameras == 0 && c.record.segment == 60 &&
          c.record.keep == 3 && !c.record.path[0] && c.api.auth == 1 && !strcmp(c.api.listen, "0.0.0.0:8080"),
          "missing file: defaults");

    /* save + load: the same settings */
    c.n_cameras = 2;
    snprintf(c.cameras[0].url, sizeof c.cameras[0].url, "rtsp://admin:secret@10.0.0.2/stream");
    snprintf(c.cameras[0].name, sizeof c.cameras[0].name, "Gate");
    c.cameras[0].enabled = 1;
    snprintf(c.cameras[1].url, sizeof c.cameras[1].url, "netip://admin:@10.0.0.3/?ch=1&stream=extra");
    c.cameras[1].enabled = 0;
    snprintf(c.record.path, sizeof c.record.path, "/mnt/sda1/rec");
    c.record.segment = 300;
    /* A world-readable file and a symlink where an older vigild put its
     * temporary file: neither may receive the passwords. */
    char victim[160];
    snprintf(victim, sizeof victim, "%s/victim", dir);
    write_file(victim, "victim");
    chmod(victim, 0644);
    if (symlink(victim, tmp)) {}
    write_file(path, "{}");
    chmod(path, 0644);
    CHECK(config_save_file(path, &c, err, sizeof err) == 0, "save: %s", err);
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600, "saved 0600 (it holds passwords)");
    char *v = read_file(victim);
    CHECK(v && !strcmp(v, "victim"), "a symlink at PATH.tmp is not written through");
    free(v);
    unlink(tmp);
    unlink(victim);
    CHECK(count_files(dir) == 1, "no temporary file left: %d files", count_files(dir));
    memset(&e, 0, sizeof e);
    CHECK(config_load_file(path, &d, &e) == 0, "load what was saved: %s", e.n ? e.e[0].msg : "");
    char *a = config_text(&c), *b = config_text(&d);
    CHECK(a && b && !strcmp(a, b), "round trip:\n%s\n%s", a, b);
    free(b);
    char *raw = read_file(path);
    CHECK(raw && strstr(raw, "admin:secret@"), "the file keeps the password");
    /* YAML, documented by the schema's own help text */
    CHECK(raw && !strncmp(raw, "# vigild configuration", 22) && strstr(raw, "\nversion: 1\n") &&
          strstr(raw, "\ncameras:\n  - url: \"rtsp://admin:secret@10.0.0.2/stream\"\n    name: Gate\n"
                      "    enabled: true\n") &&
          strstr(raw, "\nrecord:\n  # Recording directory: Where the streams are recorded") &&
          strstr(raw, "\n  path: /mnt/sda1/rec\n") &&
          strstr(raw, "  # Segment length (s): A new file starts at the first keyframe after this\n"
                      "  # many seconds. 10..3600, default 60.\n  segment: 300\n") &&
          strstr(raw, "# Cameras: In tile order: the first is CAM1. At most 16. Each one:\n#   url: Source URL.") &&
          strstr(raw, "\n  listen: \"0.0.0.0:8080\"\n") && strstr(raw, "not kept"),
          "YAML with the help as comments:\n%s", raw ? raw : "");
    free(raw);
    /* and saved again, the same file */
    char path2[160];
    snprintf(path2, sizeof path2, "%s/again.yaml", dir);
    CHECK(config_save_file(path2, &d, err, sizeof err) == 0, "save again: %s", err);
    raw = read_file(path);
    char *raw2 = read_file(path2);
    CHECK(raw && raw2 && !strcmp(raw, raw2), "load, save: the same text");
    free(raw);
    free(raw2);
    unlink(path2);

    /* Strings that YAML would read as something else, or not at all, are
     * quoted; each one comes back as it was. */
    static const char *const names[] = {
        "", "yes", "No", "on", "OFF", "true", "False", "y", "n", "null", "Null", "~", "123", "-1", "+1",
        "0x1f", "1e3", ".5", "1.0", "0", "007", " lead", "trail ", "a: b", "a #b", "#hash", "a#b", "key:",
        "-", "- x", "?", "? x", ":", "@at", "`tick", "!tag", "&anchor", "*alias", "%pct", "|lit", ">fold",
        "'single'", "\"double\"", "back\\slash", "[x]", "{y}", "a,b", "Gate 2", "Ворота", "門",
        "\xc2\x80" "c1", "nel\xc2\x85", "ls\xe2\x80\xa8", "bom\xef\xbb\xbf", "nb\xc2\xa0sp", "emoji \xf0\x9f\x93\xb7",
        "tab\tinside", "x\x7f",
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        int control = strchr(names[i], '\t') || strchr(names[i], 0x7f);
        snprintf(c.cameras[0].name, sizeof c.cameras[0].name, "%s", names[i]);
        memset(&e, 0, sizeof e);
        if (control) {                  /* refused before it could be saved */
            CHECK(schema_validate(&config_schema, &c, &e) < 0 && has_error(&e, "cameras[0].name", "control"),
                  "control character refused: %s", names[i]);
            continue;
        }
        CHECK(config_save_file(path, &c, err, sizeof err) == 0, "save name '%s': %s", names[i], err);
        memset(&e, 0, sizeof e);
        int rc = config_load_file(path, &d, &e);
        CHECK(rc == 0 && !strcmp(d.cameras[0].name, names[i]) && d.n_cameras == 2 && d.cameras[0].enabled,
              "name '%s' comes back as '%s' (%s)", names[i], d.cameras[0].name, e.n ? e.e[0].msg : "");
    }
    static const char *const urls[] = {
        "rtsp://admin:p?ss#w&rd:x@10.0.0.2:554/live?a=1&b=2",
        "rtsp://admin:\"q\" \\ @10.0.0.2/x",
        "rtsp://u:pw@h/x?token=a@b#frag",
        "netip://admin:@10.0.0.3:34567/?ch=0&stream=main",
        "capture:1",
    };
    snprintf(c.cameras[0].name, sizeof c.cameras[0].name, "Gate");
    for (size_t i = 0; i < sizeof urls / sizeof urls[0]; ++i) {
        snprintf(c.cameras[0].url, sizeof c.cameras[0].url, "%s", urls[i]);
        memset(&e, 0, sizeof e);
        CHECK(schema_validate(&config_schema, &c, &e) == 0, "URL %s valid: %s", urls[i], e.n ? e.e[0].msg : "");
        CHECK(config_save_file(path, &c, err, sizeof err) == 0, "save URL: %s", err);
        memset(&e, 0, sizeof e);
        CHECK(config_load_file(path, &d, &e) == 0 && !strcmp(d.cameras[0].url, urls[i]),
              "URL %s comes back as %s (%s)", urls[i], d.cameras[0].url, e.n ? e.e[0].msg : "");
    }
    snprintf(c.cameras[0].url, sizeof c.cameras[0].url, "rtsp://admin:secret@10.0.0.2/stream");

    /* the documented example stays valid, and is what vigild writes */
    char example[512];
    snprintf(example, sizeof example, "%s/../../docs/vigil.example.yaml", g_data);
    memset(&e, 0, sizeof e);
    CHECK(config_load_file(example, &d, &e) == 0 && d.n_cameras == 3 && !d.cameras[2].enabled &&
          !strcmp(d.cameras[1].url, "netip://admin:@192.168.1.11:34567/?ch=0&stream=main"),
          "docs/vigil.example.yaml: %s", e.n ? e.e[0].msg : "");
    char *ex = read_file(example), *gen = config_file_text(&d, CONFIG_YAML);
    CHECK(ex && gen && !strcmp(ex, gen),
          "docs/vigil.example.yaml is what vigild writes (vigild -f docs/vigil.example.yaml -w -):\n%s",
          gen ? gen : "");
    free(ex);
    free(gen);

    /* what people type */
    CHECK(load_text(path, "# just a comment\n", &d, &e) == 0 && d.record.segment == 60, "comments only: defaults");
    CHECK(load_text(path, "", &d, &e) == 0 && d.n_cameras == 0, "empty: defaults");
    CHECK(load_text(path,
                    "cameras:\n"
                    "- url: rtsp://10.0.0.2/live\n"         /* plain: a ':' not followed by a space */
                    "  enabled: yes\n"
                    "  name:\n"                             /* empty */
                    "- {url: 'netip://admin:@10.0.0.3/?ch=0', enabled: Off, name: 'it''s'}\n"
                    "- url: \"capture:0\"\n"
                    "  enabled: \"no\"\n"
                    "record:\n"
                    "  path: '/mnt/sda1/rec'\n"
                    "  segment: +90   # seconds\n"
                    "  keep: \"5\"\n"
                    "  allow_rootfs: TRUE\n"
                    "api:\n"
                    "  listen: 8080\n"                      /* a string setting given a number */
                    "  auth: on\n", &d, &e) == 0,
          "hand-written YAML: %s %s", e.n ? e.e[0].path : "", e.n ? e.e[0].msg : "");
    CHECK(d.n_cameras == 3 && d.cameras[0].enabled == 1 && !d.cameras[0].name[0] &&
          !strcmp(d.cameras[1].name, "it's") && d.cameras[1].enabled == 0 && d.cameras[2].enabled == 0 &&
          !strcmp(d.cameras[2].url, "capture:0") && !strcmp(d.record.path, "/mnt/sda1/rec") &&
          d.record.segment == 90 && d.record.keep == 5 && d.record.allow_rootfs == 1 &&
          !strcmp(d.api.listen, "8080") && d.api.auth == 1, "values as typed");
    /* the firmware's earlier vigil.yaml: a camera is just its URL */
    CHECK(load_text(path,
                    "cameras:\n"
                    "  - rtsp://admin:12345@192.168.1.10/stream=0\n"
                    "  - \"netip://admin:@192.168.1.11:34567/?ch=0&stream=main\"\n"
                    "record:\n"
                    "  path: \"\"\n"
                    "  segment: 60\n"
                    "  keep: 0\n", &d, &e) == 0 && d.n_cameras == 2 && d.cameras[0].enabled &&
          !strcmp(d.cameras[0].url, "rtsp://admin:12345@192.168.1.10/stream=0") && !d.cameras[0].name[0] &&
          !strcmp(d.cameras[1].url, "netip://admin:@192.168.1.11:34567/?ch=0&stream=main") && d.record.keep == 0,
          "a camera given as its URL: %s %s", e.n ? e.e[0].path : "", e.n ? e.e[0].msg : "");
    CHECK(load_text(path, "cameras:\n  - http://x/\n  -\n", &d, &e) < 0 &&
          has_error(&e, "cameras[0].url", "not a camera URL") && e.total == 1,
          "a bad URL; an empty item is an empty slot");
    CHECK(load_text(path, "cameras:\nrecord:\n", &d, &e) == 0 && d.n_cameras == 0 && d.record.segment == 60,
          "an empty section: no cameras, defaults");
    CHECK(load_text(path, "{\"version\": 1, \"record\": {\"segment\": 120, \"allow_rootfs\": true}}", &d, &e) == 0 &&
          d.record.segment == 120 && d.record.allow_rootfs, "JSON is YAML too");
    CHECK(load_text(path, "version: 1\nfuture:\n  x: [1, {y: 2}]\nrecord:\n  segment: 90\n  later: true\n",
                    &d, &e) == 0 && d.record.segment == 90, "a newer file's keys are skipped");

    /* wrong values: the defaults, with each error and its path */
    CHECK(load_text(path, "record:\n  allow_rootfs: maybe\n  segment: 1e2\ncameras:\n  - url: capture:0\n"
                          "    enabled: 1\n", &d, &e) < 0 &&
          has_error(&e, "record.allow_rootfs", "true or false") && has_error(&e, "record.segment", "integer") &&
          has_error(&e, "cameras[0].enabled", "true or false") && !d.n_cameras && !d.record.allow_rootfs,
          "wrong types: %d errors", e.total);
    CHECK(load_text(path, "record:\n  segment: 060\n", &d, &e) < 0 && has_error(&e, "record.segment", "integer"),
          "a leading 0 is not guessed at");
    CHECK(load_text(path, "record:\n  segment: 99999999999\n", &d, &e) < 0 &&
          has_error(&e, "record.segment", "integer"), "beyond int");
    CHECK(load_text(path, "record:\n  segment: ~\n", &d, &e) < 0 && has_error(&e, "record.segment", "integer"),
          "null for an integer");
    CHECK(load_text(path, "record:\n  segment: 5\ncameras:\n  - url: http://x/\n", &d, &e) < 0 &&
          has_error(&e, "record.segment", "range") && has_error(&e, "cameras[0].url", "not a camera URL") &&
          !d.n_cameras, "bad values");
    CHECK(load_text(path, "record: [1, 2]\ncameras:\n  url: capture:0\n", &d, &e) < 0 &&
          has_error(&e, "record", "object") && has_error(&e, "cameras", "array"), "wrong shapes");
    CHECK(load_text(path, "cameras:\n  - name: x\n", &d, &e) == 0 && d.n_cameras == 1 && !d.cameras[0].url[0],
          "no url: an empty slot");
    CHECK(load_text(path, "version: 2\n", &d, &e) < 0 && has_error(&e, "version", "must be 1"), "version");
    CHECK(load_text(path, "api:\n  listen: 1.2.3:80\n", &d, &e) < 0 && has_error(&e, "api.listen", "IPv4"),
          "listen address");
    CHECK(load_text(path, "api:\n  listen: 0.0.0.0:0\n", &d, &e) < 0 && has_error(&e, "api.listen", "port"),
          "listen port");
    CHECK(load_text(path, "record:\n  path: rec\n", &d, &e) < 0 && has_error(&e, "record.path", "absolute"),
          "relative path");
    CHECK(load_text(path, "cameras:\n  - url: capture:0\n    name: \"\\x01\"\n", &d, &e) < 0 &&
          has_error(&e, "cameras[0].name", "control"), "an escape for a control character");

    /* not YAML this reads: the defaults, with the reason */
    CHECK(load_text(path, "record:\n  segment: [60,\n", &d, &e) < 0 && e.n && strstr(e.e[0].msg, "not valid YAML") &&
          strstr(e.e[0].msg, "line 3") && d.record.segment == 60, "syntax error: %s", e.n ? e.e[0].msg : "");
    CHECK(load_text(path, "record:\n  segment: 60\n   keep: 1\n", &d, &e) < 0 && e.n &&
          strstr(e.e[0].msg, "not valid YAML"), "bad indentation: %s", e.n ? e.e[0].msg : "");
    CHECK(load_text(path, "record:\n  segment: 60\n\tkeep: 1\n", &d, &e) < 0 && e.n &&
          strstr(e.e[0].msg, "not valid YAML"), "a tab for indentation: %s", e.n ? e.e[0].msg : "");
    CHECK(load_text(path, "x: &a [1, 2]\nrecord:\n  keep: *a\n", &d, &e) < 0 &&
          has_error(&e, "record.keep", "aliases"), "an alias is refused");
    CHECK(load_text(path, "a: &a [x, x, x, x, x, x, x, x, x]\nb: &b [*a, *a, *a, *a, *a, *a, *a, *a, *a]\n"
                          "c: &c [*b, *b, *b, *b, *b, *b, *b, *b, *b]\n", &d, &e) < 0 && has_error(&e, "b[0]", "aliases"),
          "billion laughs: refused at the first alias");
    CHECK(load_text(path, "x: [[[[[[[[[[1]]]]]]]]]]\n", &d, &e) < 0 && has_error(&e, "x[0][0][0][0][0][0][0]", "deep"),
          "nested too deeply: %s %s", e.n ? e.e[0].path : "", e.n ? e.e[0].msg : "");
    {
        size_t cap = 64 * 1024, n = 0;
        char *big = malloc(cap);
        n += (size_t)snprintf(big + n, cap - n, "x: [");
        for (int i = 0; i < SCHEMA_YAML_MAX_NODES + 1; ++i) n += (size_t)snprintf(big + n, cap - n, "1,");
        snprintf(big + n, cap - n, "1]\n");
        CHECK(load_text(path, big, &d, &e) < 0 && e.n && strstr(e.e[0].msg, "too many"),
              "too many values: %s", e.n ? e.e[0].msg : "");
        free(big);
    }
    CHECK(load_text(path, "record:\n  keep: 1\n  keep: 2\n", &d, &e) < 0 && has_error(&e, "record.keep", "twice"),
          "a key twice");
    CHECK(load_text(path, "record:\n  ? [a]\n  : 1\n", &d, &e) < 0 && has_error(&e, "record", "plain text"),
          "a key that is not text");
    CHECK(load_text(path, "record:\n  path: \"/a\\0b\"\n", &d, &e) < 0 && has_error(&e, "record.path", "NUL"),
          "a NUL character");
    CHECK(load_text(path, "record: {}\n---\nrecord: {}\n", &d, &e) < 0 && e.n && strstr(e.e[0].msg, "one YAML document"),
          "two documents");
    CHECK(load_text(path, "- record\n", &d, &e) < 0 && e.n && strstr(e.e[0].msg, "mapping"), "a list");
    CHECK(load_text(path, "hello\n", &d, &e) < 0 && e.n && strstr(e.e[0].msg, "mapping"), "a word");
    CHECK(load_text(path, "record:\n  path: \"\xff\"\n", &d, &e) < 0 && e.n && strstr(e.e[0].msg, "not valid YAML"),
          "not UTF-8: %s", e.n ? e.e[0].msg : "");

    /* a .json file is still JSON, read and written */
    char jpath[160];
    snprintf(jpath, sizeof jpath, "%s/vigil.json", dir);
    CHECK(config_save_file(jpath, &c, err, sizeof err) == 0, "save JSON: %s", err);
    raw = read_file(jpath);
    CHECK(raw && raw[0] == '{' && strstr(raw, "\"segment\":\t300") && raw[strlen(raw) - 1] == '\n' &&
          raw[strlen(raw) - 2] == '}', "a .json file is written as JSON: %s", raw ? raw : "");
    free(raw);
    memset(&e, 0, sizeof e);
    CHECK(config_load_file(jpath, &d, &e) == 0 && (b = config_text(&d)) && !strcmp(a, b), "JSON round trip");
    free(b);
    CHECK(load_text(jpath, "{\"record\": {\"segment\": 60,}}", &d, &e) < 0 && e.n &&
          strstr(e.e[0].msg, "not valid JSON") && d.record.segment == 60 && !d.n_cameras,
          "bad JSON: %s", e.n ? e.e[0].msg : "");
    CHECK(load_text(jpath, "record:\n  segment: 90\n", &d, &e) < 0 && e.n && strstr(e.e[0].msg, "not valid JSON"),
          "a .json file is not read as YAML");
    /* An earlier version took any bytes: such a file still loads and is
     * saved as JSON, but cannot become YAML. */
    CHECK(load_text(jpath, "{\"cameras\": [{\"url\": \"capture:0\", \"name\": \"Garage \xe9\"}]}", &d, &e) == 0 &&
          !strcmp(d.cameras[0].name, "Garage \xe9"), "a .json file with a Latin-1 name loads");
    CHECK(config_save_file(jpath, &d, err, sizeof err) == 0, "and is saved as JSON: %s", err);
    CHECK(config_check_text(&d, CONFIG_JSON, err, sizeof err) == 0 &&
          config_check_text(&d, CONFIG_YAML, err, sizeof err) < 0 && strstr(err, "cameras[0].name: not valid UTF-8"),
          "not as YAML: %s", err);
    CHECK(config_save_file(path2, &d, err, sizeof err) < 0 && strstr(err, "UTF-8") && access(path2, F_OK) < 0,
          "a YAML save refuses it, writing nothing");
    free(a);

    /* atomic save: a failed save leaves the old file as it was */
    unlink(jpath);
    write_file(path, "old");
    if (geteuid() != 0) {                       /* root writes anywhere */
        chmod(dir, 0500);
        CHECK(config_write_atomic(path, "new", 3, err, sizeof err) < 0 && strstr(err, "cannot create"),
              "save into a read-only directory fails");
        chmod(dir, 0700);
        raw = read_file(path);
        CHECK(raw && !strcmp(raw, "old"), "old file intact after a failed save");
        free(raw);
    }
    CHECK(config_write_atomic(path, "new", 3, err, sizeof err) == 0, "save: %s", err);
    raw = read_file(path);
    CHECK(raw && !strcmp(raw, "new") && count_files(dir) == 1, "replaced");
    free(raw);

    char cmd[64];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd)) {}
}

static void test_config_store(void)
{
    char dir[] = "/tmp/vigil-cfg-XXXXXX", path[128], bad[140];
    if (!mkdtemp(dir)) return;
    snprintf(path, sizeof path, "%s/vigil.yaml", dir);
    snprintf(bad, sizeof bad, "%s.bad", path);
    char longp[400];
    memset(longp, 'a', sizeof longp - 1);
    longp[0] = '/';
    longp[sizeof longp - 1] = 0;
    CHECK(config_init(longp) == -2, "a path too long to keep is refused, not shortened");

    write_file(path, "record:\n  segment: 1\n");                   /* invalid: defaults */
    CHECK(config_init(path) < 0, "invalid file reported");

    /* The invalid file is not saved over unless a copy of it is kept. */
    char inbad[160];
    mkdir(bad, 0700);
    snprintf(inbad, sizeof inbad, "%s/x", bad);
    write_file(inbad, "x");
    schema_errors_t eb = { 0 };
    CHECK(config_put_json("record", "{}", 2, &eb) == CONFIG_EIO && eb.n && strstr(eb.e[0].msg, "not saving"),
          "no copy, no save: %s", eb.n ? eb.e[0].msg : "");
    char *still = read_file(path);
    CHECK(still && !strcmp(still, "record:\n  segment: 1\n"), "the invalid file is still there");
    free(still);
    unlink(inbad);
    rmdir(bad);
    vigil_config_t c;
    unsigned g0 = config_get(&c);
    CHECK(c.record.segment == 60, "defaults in force");

    schema_errors_t e = { 0 };
    static const char cams[] = "[{\"url\":\"rtsp://admin:s3cret@10.0.0.2/live\",\"name\":\"Gate\"},"
                               "{\"url\":\"netip://admin:@10.0.0.3/\",\"enabled\":false}]";
    CHECK(config_put_json("cameras", cams, strlen(cams), &e) == CONFIG_OK, "put cameras: %s",
          e.n ? e.e[0].msg : "");
    unsigned g1 = config_get(&c);
    CHECK(g1 == g0 + 1 && c.n_cameras == 2 && c.cameras[0].enabled && !c.cameras[1].enabled &&
          !strcmp(c.cameras[0].url, "rtsp://admin:s3cret@10.0.0.2/live"), "stored");
    struct stat st;
    CHECK(stat(bad, &st) == 0, "the invalid file is kept as .bad");
    char *raw = read_file(path);
    CHECK(raw && strstr(raw, "s3cret") && strstr(raw, "\n    name: Gate\n") && strstr(raw, "# Segment length"),
          "saved to the file, as YAML");
    free(raw);
    char *kept = read_file(bad);
    CHECK(kept && !strcmp(kept, "record:\n  segment: 1\n"), "the .bad copy is the file as it was");
    free(kept);
    /* Not UTF-8: it could be saved, but not read back from YAML. */
    static const char latin1[] = "[{\"url\":\"capture:0\",\"name\":\"Garage \xe9\"}]";
    memset(&e, 0, sizeof e);
    CHECK(config_put_json("cameras", latin1, strlen(latin1), &e) == CONFIG_EINVAL &&
          has_error(&e, "cameras[0].name", "UTF-8"), "a name that is not UTF-8 is refused");

    /* redacted out, "***" in = keep; cameras may be reordered */
    char *out = config_get_json("cameras", 1);
    CHECK(out && strstr(out, "admin:***@10.0.0.2") && !strstr(out, "s3cret"), "redacted: %s", out ? out : "");
    free(out);
    out = config_get_json(NULL, 1);
    CHECK(out && !strstr(out, "s3cret") && strstr(out, "\"record\""), "whole document redacted");
    free(out);
    static const char odd[] = "[{\"url\":\"rtsp://admin:s3?cr#t@10.0.0.4/live\"}]";
    memset(&e, 0, sizeof e);
    CHECK(config_put_json("cameras", odd, strlen(odd), &e) == CONFIG_OK, "a password with ? and #");
    out = config_get_json(NULL, 1);
    CHECK(out && !strstr(out, "s3?cr") && !strstr(out, "cr#t") && strstr(out, "admin:***@10.0.0.4/live"),
          "a password with ? and # is redacted: %s", out ? out : "");
    free(out);
    memset(&e, 0, sizeof e);
    CHECK(config_put_json("cameras", cams, strlen(cams), &e) == CONFIG_OK, "put cameras again");
    static const char back[] = "[{\"url\":\"netip://admin:@10.0.0.3/\"},"
                               "{\"url\":\"rtsp://admin:***@10.0.0.2/live\",\"name\":\"Gate 2\"}]";
    memset(&e, 0, sizeof e);
    CHECK(config_put_json("cameras", back, strlen(back), &e) == CONFIG_OK, "put redacted back");
    config_get(&c);
    CHECK(!strcmp(c.cameras[1].url, "rtsp://admin:s3cret@10.0.0.2/live") && !strcmp(c.cameras[1].name, "Gate 2"),
          "*** kept the stored password: %s", c.cameras[1].url);
    static const char wrong[] = "[{\"url\":\"rtsp://admin:***@10.0.0.9/live\"}]";
    memset(&e, 0, sizeof e);
    CHECK(config_put_json("cameras", wrong, strlen(wrong), &e) == CONFIG_EINVAL &&
          has_error(&e, "cameras[0].url", "does not match"), "*** for an unknown camera");

    /* errors change nothing */
    unsigned g2 = config_generation();
    raw = read_file(path);
    static const char badrec[] = "{\"segment\": 4000, \"keep\": -1}";
    memset(&e, 0, sizeof e);
    CHECK(config_put_json("record", badrec, strlen(badrec), &e) == CONFIG_EINVAL && e.total == 2 &&
          has_error(&e, "record.segment", "range") && has_error(&e, "record.keep", "range"), "bad record");
    memset(&e, 0, sizeof e);
    CHECK(config_put_json("record", "{\"segment\":", 11, &e) == CONFIG_EINVAL && e.n &&
          strstr(e.e[0].msg, "JSON"), "bad JSON");
    memset(&e, 0, sizeof e);
    CHECK(config_put_json("record", "{} {}", 5, &e) == CONFIG_EINVAL && e.n && strstr(e.e[0].msg, "byte 3"),
          "trailing data after the document: %s", e.n ? e.e[0].msg : "");
    CHECK(config_put_json("nope", "{}", 2, &e) == CONFIG_ENOENT, "unknown section");
    char *raw2 = read_file(path);
    CHECK(config_generation() == g2 && raw && raw2 && !strcmp(raw, raw2), "nothing changed on errors");
    free(raw);
    free(raw2);
    CHECK(config_put_json("record", "{} \n", 4, &e) == CONFIG_OK, "trailing white space is fine");

    /* the whole document: what it omits gets the defaults */
    static const char whole[] = "{\"version\":1,\"record\":{\"path\":\"/rec\",\"segment\":120}}";
    memset(&e, 0, sizeof e);
    CHECK(config_put_json(NULL, whole, strlen(whole), &e) == CONFIG_OK, "put whole");
    config_get(&c);
    CHECK(c.n_cameras == 0 && c.record.segment == 120 && c.record.keep == 3 && !strcmp(c.record.path, "/rec"),
          "whole document replaced");

    /* A write built from an old generation is refused, under the
     * writers' lock. */
    static const char K5[] = "{\"path\":\"/rec\",\"segment\":120,\"keep\":5}",
                      K6[] = "{\"path\":\"/rec\",\"segment\":120,\"keep\":6}";
    memset(&e, 0, sizeof e);
    unsigned gen = config_generation();
    CHECK(config_put_json_at("record", K5, strlen(K5), gen, &e) == CONFIG_OK, "current generation");
    CHECK(config_generation() == gen + 1, "generation moved");
    memset(&e, 0, sizeof e);
    CHECK(config_put_json_at("record", K6, strlen(K6), gen, &e) == CONFIG_ESTALE && e.n, "stale refused");
    config_get(&c);
    CHECK(c.record.keep == 5 && config_generation() == gen + 1, "nothing changed");

    channels_config_t cc;
    c.n_cameras = 2;
    snprintf(c.cameras[0].url, sizeof c.cameras[0].url, "capture:0");
    c.cameras[0].enabled = 0;
    snprintf(c.cameras[1].url, sizeof c.cameras[1].url, "capture:1");
    c.cameras[1].enabled = 1;
    config_to_channels(&c, &cc);
    CHECK(cc.n == 2 && cc.urls[0] == NULL && !strcmp(cc.urls[1], "capture:1") && !strcmp(cc.rec_dir, "/rec") &&
          cc.seg_secs == 120, "to channels: a disabled camera keeps its tile");

    char cmd[64];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd)) {}
}

/* ---- events: every reader sees every event ---------------------------------------- */

static void test_events(void)
{
    uint64_t ui = events_head(), api = events_head();
    events_push(2, "online");
    events_push(0, "config %s", "applied");
    event_t ev;
    CHECK(events_next(&ui, &ev) && ev.ch == 2 && !strcmp(ev.text, "online"), "first reader");
    CHECK(events_next(&ui, &ev) && ev.ch == 0 && !strcmp(ev.text, "config applied"), "first reader, 2");
    CHECK(!events_next(&ui, &ev), "first reader drained");
    CHECK(events_next(&api, &ev) && ev.ch == 2 && events_next(&api, &ev) && ev.ch == 0 && !ev.missed,
          "second reader sees the same events");

    /* a reader that falls behind skips to the oldest kept and is told */
    uint64_t slow = events_head();
    for (int i = 0; i < EVENTS_KEEP + 5; ++i) events_push(1, "e%d", i);
    CHECK(events_next(&slow, &ev) && ev.missed == 5 && !strcmp(ev.text, "e5"), "missed %u, %s", ev.missed, ev.text);
    int n = 1;
    while (events_next(&slow, &ev)) ++n;
    CHECK(n == EVENTS_KEEP && !strcmp(ev.text, "e68"), "kept %d, last %s", n, ev.text);
}

/* ---- rec_place: where a recording may go ------------------------------------ */

/* A pretend filesystem: path -> device, directory or not. Anything not listed
 * does not exist (ENOTDIR under a listed file), as stat(2) would say. */
typedef struct { const char *path; int dev; int dir; } fake_node_t;
static const fake_node_t *g_fake;
static const char *g_fake_link, *g_fake_target;    /* one symbolic link */

static const fake_node_t *fake_find(const char *path)
{
    for (const fake_node_t *n = g_fake; n->path; ++n)
        if (!strcmp(n->path, path)) return n;
    return NULL;
}

static int fake_stat(const char *path, struct stat *st)
{
    char p[256];
    snprintf(p, sizeof p, "%s", path);
    if (g_fake_link && !strcmp(p, g_fake_link)) snprintf(p, sizeof p, "%s", g_fake_target);
    const fake_node_t *n = fake_find(p);
    if (n) {
        memset(st, 0, sizeof *st);
        st->st_dev = (dev_t)n->dev;
        st->st_mode = n->dir ? S_IFDIR | 0755 : S_IFREG | 0644;
        return 0;
    }
    /* A listed file on the way makes it ENOTDIR. */
    for (char *s = strrchr(p, '/'); s && s != p; s = strrchr(p, '/')) {
        *s = 0;
        const fake_node_t *up = fake_find(p);
        if (up) { errno = up->dir ? ENOENT : ENOTDIR; return -1; }
    }
    errno = ENOENT;
    return -1;
}

static char *fake_realpath(const char *path, char *out)
{
    snprintf(out, 256, "%s", g_fake_link && !strcmp(path, g_fake_link) ? g_fake_target : path);
    return out;
}

static void test_rec_place(void)
{
    char dir[] = "/tmp/vigil-recXXXXXX", mi[300], why[300];
    if (!mkdtemp(dir)) return;
    snprintf(mi, sizeof mi, "%s/mountinfo", dir);
    /* An OpenIPC board: an overlay root over squashfs + jffs2, /tmp in RAM,
     * a card at /mnt/mmcblk0p1, a disk at "/mnt/Disk One" (escaped), a
     * read-only stick at /mnt/sdb1. */
    FILE *f = fopen(mi, "w");
    fputs("15 1 0:13 / / rw,relatime - overlay overlay rw,lowerdir=/,upperdir=/overlay/root\n"
          "14 1 31:3 / /rom ro,relatime - squashfs /dev/root ro\n"
          "16 15 31:5 / /overlay rw,relatime - jffs2 /dev/mtdblock5 rw\n"
          "17 15 0:14 / /tmp rw,relatime - tmpfs tmpfs rw\n"
          "30 15 179:1 / /mnt/mmcblk0p1 rw,relatime - vfat /dev/mmcblk0p1 rw,fmask=0022\n"
          "31 15 8:1 / /mnt/Disk\\040One rw,relatime - ext4 /dev/sda1 rw\n"
          "32 15 8:17 / /mnt/sdb1 ro,relatime - vfat /dev/sdb1 ro\n", f);
    fclose(f);
    static const fake_node_t board[] = {
        { "/", 13, 1 }, { "/mnt", 13, 1 }, { "/mnt/file", 13, 0 }, { "/rom", 3, 1 },
        { "/overlay", 5, 1 }, { "/tmp", 14, 1 }, { "/mnt/mmcblk0p1", 101, 1 },
        { "/mnt/mmcblk0p1/rec", 101, 1 }, { "/mnt/Disk One", 102, 1 }, { "/mnt/sdb1", 103, 1 },
        { NULL, 0, 0 },
    };
    g_fake = board;
    g_fake_link = g_fake_target = NULL;
    rec_place_ops_t ops = { fake_stat, fake_realpath, mi };
    struct { const char *path; int allow; rec_place_t want; const char *note; } T[] = {
        { "/mnt/sda1/rec",           0, REC_PLACE_NO_DISK,  "no disk mounted: would land on the overlay" },
        { "/mnt/sda1/rec",           1, REC_PLACE_OK,       "allow_rootfs: the root is a disk" },
        { "/mnt/mmcblk0p1/rec",      0, REC_PLACE_OK,       "a card, the directory there" },
        { "/mnt/mmcblk0p1/new/rec/", 0, REC_PLACE_OK,       "a card, the directory still to be made" },
        { "/mnt/mmcblk0p1",          0, REC_PLACE_OK,       "the card's own mount point" },
        { "/mnt/Disk One/rec",       0, REC_PLACE_OK,       "an escaped mount point" },
        { "/mnt/sdb1/rec",           0, REC_PLACE_READONLY, "a stick mounted read-only" },
        { "/tmp/rec",                0, REC_PLACE_NOT_DISK, "RAM" },
        { "/tmp/rec",                1, REC_PLACE_NOT_DISK, "RAM, allow_rootfs or not" },
        { "/overlay/rec",            0, REC_PLACE_NOT_DISK, "the jffs2 flash partition itself" },
        { "/rom/rec",                1, REC_PLACE_NOT_DISK, "the squashfs" },
        { "/mnt/file/rec",           0, REC_PLACE_ERROR,    "a file in the way" },
        { "/mnt/file",               1, REC_PLACE_ERROR,    "a file, not a directory" },
        { "rec",                     1, REC_PLACE_ERROR,    "a relative path" },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; ++i) {
        rec_place_t got = rec_place_check(T[i].path, T[i].allow, &ops, why, sizeof why);
        CHECK(got == T[i].want, "rec_place %s (allow_rootfs %d, %s): %s, want %s (%s)", T[i].path,
              T[i].allow, T[i].note, rec_place_name(got), rec_place_name(T[i].want), why);
    }
    /* A link on the root that leads into RAM is judged by where it leads. */
    static const fake_node_t linked[] = {
        { "/", 13, 1 }, { "/mnt", 13, 1 }, { "/tmp", 14, 1 }, { "/tmp/x", 14, 1 }, { NULL, 0, 0 },
    };
    g_fake = linked;
    g_fake_link = "/mnt/rec";
    g_fake_target = "/tmp/x";
    CHECK(rec_place_check("/mnt/rec/cams", 1, &ops, why, sizeof why) == REC_PLACE_NOT_DISK,
          "a link into tmpfs: %s", why);
    g_fake_link = g_fake_target = NULL;
    /* Without mountinfo a separate device cannot be told from tmpfs or
     * flash: refused. The root filesystem is refused before it is read. */
    g_fake = board;
    rec_place_ops_t nomi = { fake_stat, fake_realpath, "/nonexistent/mountinfo" };
    CHECK(rec_place_check("/mnt/sda1/rec", 0, &nomi, why, sizeof why) == REC_PLACE_NO_DISK,
          "no mountinfo: still not the root filesystem (%s)", why);
    CHECK(rec_place_check("/mnt/mmcblk0p1/rec", 0, &nomi, why, sizeof why) == REC_PLACE_ERROR,
          "no mountinfo: another device cannot be told apart, refused (%s)", why);
    /* allow_rootfs counts the root as a disk, not RAM or flash as one. */
    static const char *const roots[] = { "tmpfs", "rootfs", "jffs2", "ubifs" };
    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; ++i) {
        f = fopen(mi, "w");
        fprintf(f, "1 0 0:2 / / rw,relatime - %s %s rw\n", roots[i], roots[i]);
        fclose(f);
        CHECK(rec_place_check("/mnt/sda1/rec", 1, &ops, why, sizeof why) == REC_PLACE_NOT_DISK,
              "allow_rootfs on a %s root: still not a disk (%s)", roots[i], why);
    }
    f = fopen(mi, "w");
    fputs("1 0 8:2 / / rw,relatime - ext4 /dev/sda2 rw\n", f);
    fclose(f);
    CHECK(rec_place_check("/mnt/sda1/rec", 1, &ops, why, sizeof why) == REC_PLACE_OK,
          "allow_rootfs on an ext4 root: recorded (%s)", why);
    unlink(mi);
    rmdir(dir);

    /* The real thing, on this host: /dev/shm is tmpfs on Linux. */
    struct stat st;
    if (stat("/dev/shm", &st) == 0 && stat("/proc/self/mountinfo", &st) == 0)
        CHECK(rec_place_check("/dev/shm/vigil-test/rec", 1, NULL, why, sizeof why) == REC_PLACE_NOT_DISK,
              "/dev/shm on this host: %s", why);

    /* record.path: "." and ".." are refused before anything is judged. */
    vigil_config_t c;
    config_defaults(&c);
    schema_errors_t e = { 0 };
    snprintf(c.record.path, sizeof c.record.path, "/mnt/mmcblk0p1/x/../../../flash");
    CHECK(schema_validate(&config_schema, &c, &e) < 0 && e.n == 1 && !strcmp(e.e[0].path, "record.path"),
          "record.path with \"..\" refused");
    memset(&e, 0, sizeof e);
    snprintf(c.record.path, sizeof c.record.path, "/mnt/./rec");
    CHECK(schema_validate(&config_schema, &c, &e) < 0, "record.path with \".\" refused");
    memset(&e, 0, sizeof e);
    snprintf(c.record.path, sizeof c.record.path, "/mnt/..rec/.x");
    CHECK(schema_validate(&config_schema, &c, &e) == 0, "names starting with dots are fine");
    CHECK(!c.record.allow_rootfs, "allow_rootfs is off by default");
}

/* ---- channels: tiles follow the layout ------------------------------------------ */

/* A display platform with four channels and `g_dec_slots` decoders (none
 * at first: the tile tests' channels point at a closed port anyway). */
static hal_caps_t g_fake_caps = { .name = "fake", .display = 1, .decoder = 1,
                                  .dec_max_w = 1920, .dec_max_h = 1080, .max_channels = 4 };
const hal_caps_t *hal_caps(void) { return &g_fake_caps; }

static pthread_mutex_t g_dec_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_dec_slots;                     /* decoders the fake platform has */
static int g_dec_open[CHANNEL_MAX];         /* channel holds one */
static int g_dec_opens[CHANNEL_MAX], g_dec_fails[CHANNEL_MAX], g_dec_sent[CHANNEL_MAX];

static int dec_count(int *a, int ch)
{
    pthread_mutex_lock(&g_dec_lock);
    int v = a[ch];
    pthread_mutex_unlock(&g_dec_lock);
    return v;
}

int hal_dec_open(int ch, vg_codec_t c, const vg_stream_info_t *si)
{
    (void)c; (void)si;
    pthread_mutex_lock(&g_dec_lock);
    int used = 0;
    for (int i = 0; i < CHANNEL_MAX; ++i) used += g_dec_open[i] && i != ch;
    int ok = used < g_dec_slots;
    g_dec_open[ch] = ok;
    ++*(ok ? &g_dec_opens[ch] : &g_dec_fails[ch]);
    pthread_mutex_unlock(&g_dec_lock);
    return ok ? 0 : -1;
}
int hal_dec_send(int ch, const vg_au_t *au)
{
    (void)au;
    pthread_mutex_lock(&g_dec_lock);
    int ok = g_dec_open[ch];
    g_dec_sent[ch] += ok;
    pthread_mutex_unlock(&g_dec_lock);
    return ok ? 0 : -1;
}
void hal_dec_reset(int ch) { (void)ch; }
void hal_dec_close(int ch)
{
    pthread_mutex_lock(&g_dec_lock);
    g_dec_open[ch] = 0;
    pthread_mutex_unlock(&g_dec_lock);
}

static vg_rect_t g_tile_at[CHANNEL_MAX];    /* w == 0: cleared */
static int       g_tile_sets[CHANNEL_MAX], g_tile_clears[CHANNEL_MAX];
int hal_display_set_tile(int ch, const vg_rect_t *r)
{
    g_tile_at[ch] = *r;
    ++g_tile_sets[ch];
    return 0;
}
void hal_display_clear_tile(int ch)
{
    memset(&g_tile_at[ch], 0, sizeof g_tile_at[ch]);
    ++g_tile_clears[ch];
}

static int rect_eq(vg_rect_t a, vg_rect_t b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

static void test_channel_tiles(void)
{
    /* A port nothing listens on: connecting fails at once, the channels
     * stay up and retry. */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof a;
    bind(fd, (struct sockaddr *)&a, sizeof a);
    getsockname(fd, (struct sockaddr *)&a, &al);
    char url0[64], url1[64], url1b[64];
    snprintf(url0, sizeof url0, "netip://127.0.0.1:%d/", ntohs(a.sin_port));
    snprintf(url1, sizeof url1, "netip://127.0.0.1:%d/?ch=1", ntohs(a.sin_port));
    snprintf(url1b, sizeof url1b, "netip://127.0.0.1:%d/?ch=2", ntohs(a.sin_port));

    vg_rect_t quad[CHANNEL_MAX] = { { 0, 40, 640, 300 }, { 640, 40, 640, 300 },
                                    { 0, 340, 640, 300 }, { 640, 340, 640, 300 } };
    channels_config_t cc = { .urls = { url0, url1 }, .n = 2, .seg_secs = 60 };
    CHECK(channels_start(&cc, quad) == 0, "start");
    CHECK(rect_eq(g_tile_at[0], quad[0]) && rect_eq(g_tile_at[1], quad[1]), "quad tiles");
    CHECK(g_tile_sets[2] == 0 && g_tile_sets[3] == 0, "no tile for an empty slot");

    /* Channel 2 full screen: channel 1 is hidden, the empty slots untouched. */
    vg_rect_t one[CHANNEL_MAX] = { { 0 } };
    one[1] = (vg_rect_t){ 0, 40, 1280, 600 };
    channels_set_tiles(one);
    CHECK(rect_eq(g_tile_at[1], one[1]), "channel 2 full screen");
    CHECK(g_tile_at[0].w == 0 && g_tile_clears[0] == 1, "channel 1 hidden");
    CHECK(g_tile_sets[2] == 0 && g_tile_clears[2] == 0, "empty slot untouched");

    /* A channel started while hidden stays hidden; one shown starts in
     * its new tile. */
    int sets0 = g_tile_sets[0];
    cc.urls[1] = url1b;
    cc.urls[0] = url1;
    CHECK(channels_apply(&cc) == 0, "apply");
    CHECK(g_tile_sets[0] == sets0 && g_tile_at[0].w == 0, "restarted hidden channel not shown");
    CHECK(rect_eq(g_tile_at[1], one[1]), "restarted channel in its full-screen tile");

    channels_set_tiles(quad);
    CHECK(rect_eq(g_tile_at[0], quad[0]) && rect_eq(g_tile_at[1], quad[1]), "back to quad");
    channels_stop();
    close(fd);
}

/* ---- channels: decoders only for the tiles on screen ------------------------------ */

/* A NETIP camera on loopback: logs anyone in and streams a clip, one
 * access unit per message, at about 100 fps: the main stream (stream
 * "Main") one clip, the sub-stream ("Extra") another. It counts the
 * streams being sent and remembers the last channel asked for. */
typedef struct {
    const uint8_t *data;
    size_t         len;
    int            h265;
} cam_clip_t;

static struct {
    int            fd;
    volatile int   stop;
    cam_clip_t     main, extra;
    atomic_int     streaming[2];       /* [0] main, [1] extra */
    atomic_int     last_channel;
} g_cam;

static void put_le32(uint8_t *p, size_t v)
{
    p[0] = v & 0xff; p[1] = v >> 8 & 0xff; p[2] = v >> 16 & 0xff; p[3] = v >> 24 & 0xff;
}

static int cam_send(int fd, int msgid, const void *body, size_t len)
{
    uint8_t h[20] = { 0xff, 1 };
    h[14] = msgid & 0xff;
    h[15] = msgid >> 8;
    put_le32(h + 16, len);
    return send(fd, h, 20, MSG_NOSIGNAL) == 20 && send(fd, body, len, MSG_NOSIGNAL) == (ssize_t)len ? 0 : -1;
}

static int cam_recv(int fd, uint8_t *b, size_t len)
{
    size_t got = 0;
    while (got < len && !g_cam.stop) {
        ssize_t r = recv(fd, b + got, len - got, 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return got == len ? 0 : -1;
}

static void *cam_client(void *arg)
{
    int fd = (int)(intptr_t)arg, extra = 0;
    static const char ok[] = "{\"Ret\":100,\"SessionID\":\"0x00000001\",\"AliveInterval\":20}\n";
    uint8_t h[20], body[2049];
    for (;;) {                                      /* login, claim, start */
        if (cam_recv(fd, h, 20) < 0) goto out;
        uint32_t len = h[16] | h[17] << 8 | h[18] << 16 | (uint32_t)h[19] << 24;
        if (len >= sizeof body || cam_recv(fd, body, len) < 0) goto out;
        body[len] = 0;
        int msgid = h[14] | h[15] << 8;
        if (msgid == 1413) {
            extra = strstr((char *)body, "\"StreamType\":\"Extra\"") != NULL;
            const char *chn = strstr((char *)body, "\"Channel\":");
            if (chn) atomic_store(&g_cam.last_channel, atoi(chn + 10));
        }
        if (msgid == 1000 || msgid == 1413) cam_send(fd, msgid + 1, ok, sizeof ok);
        if (msgid == 1410) break;
    }
    /* The clip in a loop; a message per picture: an I-frame header (0x1FC,
     * 2 H.264 or 3 H.265, 100 fps) or a P-frame header, then the picture
     * with the parameter sets before it. */
    const cam_clip_t *clip = extra ? &g_cam.extra : &g_cam.main;
    uint8_t *msg = malloc(16 + clip->len + 4 * 64);
    atomic_fetch_add(&g_cam.streaming[extra], 1);
    while (msg && !g_cam.stop) {
        size_t pos = 0, nl, len = 0;
        int key = 0;
        const uint8_t *n;
        while (!g_cam.stop && (n = au_next_nal(clip->data, clip->len, &pos, &nl)) != NULL) {
            memcpy(msg + 16 + len, "\0\0\0\1", 4);
            memcpy(msg + 20 + len, n, nl);
            len += 4 + nl;
            int t = clip->h265 ? (n[0] >> 1) & 0x3f : n[0] & 0x1f;
            int vcl = clip->h265 ? t < 32 : t >= 1 && t <= 5;
            key |= clip->h265 ? t >= 16 && t <= 21 : t == 5;
            if (!vcl) continue;
            uint8_t ihead[12] = { 0, 0, 1, 0xFC, clip->h265 ? 3 : 2, 100, 80, 45 };
            static const uint8_t phead[4] = { 0, 0, 1, 0xFD };
            size_t hl = key ? 16 : 8;
            uint8_t *m = msg + 16 - hl;
            memcpy(m, key ? ihead : phead, hl - 4);
            put_le32(m + hl - 4, len);
            if (cam_send(fd, 1412, m, hl + len) < 0) goto out_msg;
            len = 0;
            key = 0;
            usleep(10 * 1000);
        }
    }
out_msg:
    atomic_fetch_sub(&g_cam.streaming[extra], 1);
    free(msg);
out:
    close(fd);
    return NULL;
}

static void *cam_server(void *arg)
{
    (void)arg;
    while (!g_cam.stop) {
        int c = accept(g_cam.fd, NULL, NULL);
        if (c < 0) break;
        pthread_t t;
        if (pthread_create(&t, NULL, cam_client, (void *)(intptr_t)c) == 0) pthread_detach(t);
        else close(c);
    }
    return NULL;
}

/* Start the camera: main stream `main_clip`, sub-stream `extra_clip`.
 * Returns its port, or 0. */
static pthread_t g_cam_thread;
static int cam_start(const char *main_clip, const char *extra_clip)
{
    memset(&g_cam, 0, sizeof g_cam);
    g_cam.main.data = load(main_clip, &g_cam.main.len);
    g_cam.main.h265 = strstr(main_clip, ".h265") != NULL;
    g_cam.extra.data = load(extra_clip, &g_cam.extra.len);
    g_cam.extra.h265 = strstr(extra_clip, ".h265") != NULL;
    g_cam.fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof a;
    if (g_cam.fd < 0 || bind(g_cam.fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(g_cam.fd, 8) < 0 ||
        getsockname(g_cam.fd, (struct sockaddr *)&a, &al) < 0) {
        fprintf(stderr, "fake camera: no listener, test skipped\n");
        return 0;
    }
    pthread_create(&g_cam_thread, NULL, cam_server, NULL);
    return ntohs(a.sin_port);
}

static void cam_stop(void)
{
    g_cam.stop = 1;
    shutdown(g_cam.fd, SHUT_RDWR);
    close(g_cam.fd);
    pthread_join(g_cam_thread, NULL);
    usleep(100 * 1000);                     /* the clients see the stop */
    free((void *)g_cam.main.data);
    free((void *)g_cam.extra.data);
}

/* Wait up to `ms` for cond. */
#define WAIT_FOR(cond, ms) do { for (int w_ = 0; w_ < (ms) / 20 && !(cond); ++w_) usleep(20 * 1000); } while (0)

static void test_channel_decode(void)
{
    int port = cam_start("testsrc_640x360.h264", "testsrc_640x360.h264");
    if (!port) return;
    char url[64];
    snprintf(url, sizeof url, "netip://127.0.0.1:%d/", port);

    /* One decoder for two cameras, both on screen: one gets it, the
     * other is told "no decoder" and keeps trying. (No screen size: the
     * main streams everywhere.) */
    g_fake_caps.dec_shown_only = 1;
    g_dec_slots = 1;
    vg_rect_t both[CHANNEL_MAX] = { { 0, 40, 640, 300 }, { 640, 40, 640, 300 } };
    channels_config_t cc = { .urls = { url, url }, .n = 2, .seg_secs = 60 };
    CHECK(channels_start(&cc, both) == 0, "start");
    WAIT_FOR((dec_count(g_dec_open, 0) || dec_count(g_dec_open, 1)) &&
             dec_count(g_dec_fails, 0) + dec_count(g_dec_fails, 1) > 0, 5000);
    int has = dec_count(g_dec_open, 0) ? 0 : 1, other = 1 - has;
    CHECK(dec_count(g_dec_open, has) && dec_count(g_dec_sent, has) > 0, "one channel decodes");
    CHECK(!dec_count(g_dec_open, other) && dec_count(g_dec_fails, other) > 0, "the other has no decoder");
    channel_status_t st;
    channels_status(other, &st);
    CHECK(st.no_decoder && !st.decoding, "its status says no decoder");
    channels_status(has, &st);
    CHECK(!st.no_decoder && st.decoding, "the decoding one's does not");

    /* The decoding channel leaves the screen: it frees its decoder, and
     * the other takes it at its next keyframe, not after the 10 s retry. */
    vg_rect_t one[CHANNEL_MAX] = { { 0 } };
    one[other] = (vg_rect_t){ 0, 40, 1280, 600 };
    channels_set_tiles(one);
    WAIT_FOR(dec_count(g_dec_open, other), 3000);
    CHECK(!dec_count(g_dec_open, has), "hidden channel freed its decoder");
    CHECK(dec_count(g_dec_open, other) && dec_count(g_dec_opens, other) == 1, "shown channel took it");
    usleep(500 * 1000);
    channels_status(has, &st);
    CHECK(st.live && !st.decoding && st.width == 640 && st.height == 360 && !st.view[0],
          "hidden channel live, not decoding, size known (%d %d %ux%u '%s')", st.live, st.decoding,
          st.width, st.height, st.view);
    CHECK(dec_count(g_dec_opens, has) == 1, "hidden channel did not decode again");

    /* Both back with a decoder each: the hidden one decodes again. */
    g_dec_slots = 2;
    channels_set_tiles(both);
    WAIT_FOR(dec_count(g_dec_open, has), 3000);
    CHECK(dec_count(g_dec_open, has) && dec_count(g_dec_open, other), "both decode");

    channels_stop();
    cam_stop();
    g_fake_caps.dec_shown_only = 0;
    g_dec_slots = 0;
}

static void test_sub_url(void)
{
    char out[512];
    static const struct { const char *url, *sub; } cases[] = {
        { "netip://admin:@10.0.0.5:34567/?ch=0&stream=main", "netip://admin:@10.0.0.5:34567/?ch=0&stream=extra" },
        { "netip://10.0.0.5", "netip://10.0.0.5/?stream=extra" },
        { "netip://10.0.0.5/", "netip://10.0.0.5/?stream=extra" },
        { "netip://10.0.0.5/?stream=Main&ch=2", "netip://10.0.0.5/?ch=2&stream=extra" },
        { "netip://10.0.0.5/?stream=extra", NULL },     /* the sub-stream already */
        { "netip://10.0.0.5/?ch=1&stream=sub", NULL },
        { "rtsp://10.0.0.5/stream1", NULL },            /* no vendor-neutral path */
        { "capture:0", NULL },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        int rc = source_sub_url(cases[i].url, out, sizeof out);
        if (cases[i].sub)
            CHECK(rc == 0 && !strcmp(out, cases[i].sub), "sub of %s: %s", cases[i].url, rc ? "none" : out);
        else
            CHECK(rc < 0, "%s has no sub-stream (%s)", cases[i].url, out);
        if (cases[i].sub) {
            source_t *src = source_create(out, 0);
            CHECK(src != NULL, "the derived %s is a URL", out);
            source_destroy(src);
        }
    }
    CHECK(source_sub_url("netip://10.0.0.5/?ch=1", out, 20) < 0, "too long for the buffer: none");
}

/* Count the files in `dir`, and whether one is open (the writer's .tmp?). */
static int dir_files(const char *dir)
{
    DIR *d = opendir(dir);
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) n += e->d_name[0] != '.';
    if (d) closedir(d);
    return n;
}

static int view_is(int ch, const char *view, unsigned w)
{
    channel_status_t st;
    channels_status(ch, &st);
    return st.decoding && !strcmp(st.view, view) && st.width == w;
}

static void test_channel_substream(void)
{
    /* main: 1080p H.265; sub: 640x360 H.264 */
    int port = cam_start("majestic_1080p.h265", "testsrc_640x360.h264");
    if (!port) return;
    char url[64], rtsp[64], dir[512], cwd[400];
    snprintf(url, sizeof url, "netip://127.0.0.1:%d/?ch=0&stream=main", port);
    snprintf(rtsp, sizeof rtsp, "rtsp://127.0.0.1:1/stream1");      /* nothing there */
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
    snprintf(dir, sizeof dir, "%s/test_substream_rec", cwd);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) {}

    g_fake_caps.dec_shown_only = 1;
    g_dec_slots = 2;
    channels_set_screen(1280, 720);
    vg_rect_t quad[CHANNEL_MAX] = { { 0, 40, 640, 300 }, { 640, 40, 640, 300 } };
    vg_rect_t full[CHANNEL_MAX] = { { 0, 40, 1280, 600 } };
    vg_rect_t none[CHANNEL_MAX] = { { 0 } };
    channels_config_t cc = { .urls = { url, rtsp }, .n = 2, .seg_secs = 60, .rec_dir = dir,
                             .allow_rootfs = 1 };
    CHECK(channels_start(&cc, quad) == 0, "start");

    /* 2x2: the sub-stream, derived from the NETIP URL; the main stream
     * runs too, for the recording. An RTSP camera with no sub_url shows
     * its main stream. */
    WAIT_FOR(view_is(0, "sub", 640), 5000);
    CHECK(view_is(0, "sub", 640), "2x2: the sub-stream decodes");
    CHECK(atomic_load(&g_cam.streaming[0]) == 1 && atomic_load(&g_cam.streaming[1]) == 1,
          "two connections: main %d, extra %d", atomic_load(&g_cam.streaming[0]),
          atomic_load(&g_cam.streaming[1]));
    channel_status_t st;
    channels_status(1, &st);
    CHECK(!strcmp(st.view, "main"), "RTSP without a sub_url: the main stream ('%s')", st.view);
    WAIT_FOR((channels_status(0, &st), st.recording), 3000);
    CHECK(st.recording && st.live, "recording the main stream");

    /* 1x1: the main stream, through the recording's connection. */
    channels_set_tiles(full);
    WAIT_FOR(view_is(0, "main", 1920), 5000);
    CHECK(view_is(0, "main", 1920), "full screen: the main stream decodes");
    CHECK(atomic_load(&g_cam.streaming[0]) == 1, "still one main connection");

    /* Back to 2x2 within the sub-stream's linger: no new connection. */
    channels_set_tiles(quad);
    WAIT_FOR(view_is(0, "sub", 640), 5000);
    CHECK(view_is(0, "sub", 640), "2x2 again: the sub-stream");
    CHECK(atomic_load(&g_cam.streaming[1]) == 1, "the sub-stream's connection was kept");

    /* Off screen: no decoder, and the sub-stream goes after its linger;
     * the main stream and its recording stay. */
    channels_set_tiles(none);
    WAIT_FOR(atomic_load(&g_cam.streaming[1]) == 0, 9000);
    channels_status(0, &st);
    CHECK(!st.decoding && !dec_count(g_dec_open, 0) && !st.view[0], "hidden: not decoding ('%s')", st.view);
    CHECK(atomic_load(&g_cam.streaming[1]) == 0 && atomic_load(&g_cam.streaming[0]) == 1,
          "hidden: the sub-stream's connection is gone, the main one stays");
    channels_set_tiles(quad);
    WAIT_FOR(view_is(0, "sub", 640), 5000);
    CHECK(view_is(0, "sub", 640), "shown again: the sub-stream comes back");

    /* An explicit sub_url wins, and changing it restarts only the
     * sub-stream. */
    char sub[64];
    snprintf(sub, sizeof sub, "netip://127.0.0.1:%d/?ch=7&stream=extra", port);
    cc.sub_urls[0] = sub;
    CHECK(channels_apply(&cc) == 0, "apply a sub_url");
    WAIT_FOR(atomic_load(&g_cam.last_channel) == 7 && view_is(0, "sub", 640), 5000);
    CHECK(atomic_load(&g_cam.last_channel) == 7 && view_is(0, "sub", 640), "the sub_url given is used");

    /* All along: one recording, never broken by the view changes. */
    channels_status(0, &st);
    CHECK(st.recording && dir_files(dir) == 1, "one segment through every switch (%d files)", dir_files(dir));
    CHECK(atomic_load(&g_cam.streaming[0]) == 1, "the main connection never restarted");

    channels_stop();
    cam_stop();
    channels_set_screen(0, 0);
    g_fake_caps.dec_shown_only = 0;
    g_dec_slots = 0;
    if (system(cmd)) {}
}

/* ---- empty camera slots, channel states, events ------------------------------------ */

static void test_camera_slots(void)
{
    char dir[] = "/tmp/vigil-slots-XXXXXX", path[128], err[160];
    if (!mkdtemp(dir)) return;
    vigil_config_t c, d;
    schema_errors_t e = { 0 };
    for (int fmt = 0; fmt < 2; ++fmt) {
        snprintf(path, sizeof path, "%s/vigil.%s", dir, fmt ? "json" : "yaml");
        config_defaults(&c);
        c.n_cameras = 4;
        snprintf(c.cameras[0].url, sizeof c.cameras[0].url, "rtsp://10.0.0.1/a");
        snprintf(c.cameras[3].url, sizeof c.cameras[3].url, "rtsp://10.0.0.4/a");
        for (int i = 0; i < 4; ++i) c.cameras[i].enabled = 1;
        CHECK(config_save_file(path, &c, err, sizeof err) == 0, "save with empty slots: %s", err);
        memset(&e, 0, sizeof e);
        CHECK(config_load_file(path, &d, &e) == 0 && d.n_cameras == 4 && !d.cameras[1].url[0] &&
              !d.cameras[2].url[0] && !strcmp(d.cameras[3].url, "rtsp://10.0.0.4/a"),
              "%s: empty slots between cameras round-trip (%d)", fmt ? "JSON" : "YAML", d.n_cameras);
    }
    /* A YAML slot left empty by hand ("- ~") is an empty slot too. */
    snprintf(path, sizeof path, "%s/hand.yaml", dir);
    FILE *f = fopen(path, "w");
    fputs("version: 1\ncameras:\n  - rtsp://10.0.0.1/a\n  - ~\n  - rtsp://10.0.0.3/a\n", f);
    fclose(f);
    memset(&e, 0, sizeof e);
    CHECK(config_load_file(path, &d, &e) == 0 && d.n_cameras == 3 && !d.cameras[1].url[0] &&
          !strcmp(d.cameras[2].url, "rtsp://10.0.0.3/a"), "\"- ~\" is an empty slot");

    channels_config_t cc;
    d.cameras[2].enabled = 0;
    config_to_channels(&d, &cc);
    CHECK(cc.urls[0] && !cc.urls[1] && !cc.disabled[1] && !cc.urls[2] && cc.disabled[2],
          "empty slot: no URL, not disabled; switched off: no URL, disabled");

    /* Through the store, as the UI and the API add a camera at channel 4
     * with 2 and 3 empty; trailing empty slots are not kept. */
    snprintf(path, sizeof path, "%s/store.yaml", dir);
    CHECK(config_init(path) == 0, "store");
    static const char put[] = "[{\"url\":\"rtsp://10.0.0.1/a\"},{\"url\":\"\"},{\"url\":\"\"},"
                              "{\"url\":\"rtsp://10.0.0.4/a\",\"name\":\"Four\"},{\"url\":\"\"},{\"url\":\"\"}]";
    memset(&e, 0, sizeof e);
    CHECK(config_put_json("cameras", put, strlen(put), &e) == CONFIG_OK, "put with empty slots: %s",
          e.n ? e.e[0].msg : "");
    config_get(&c);
    CHECK(c.n_cameras == 4 && !strcmp(c.cameras[3].name, "Four") && !c.cameras[1].url[0],
          "camera at channel 4 kept there, trailing slots trimmed (%d)", c.n_cameras);
    config_load_file(path, &d, &e);
    CHECK(d.n_cameras == 4 && !strcmp(d.cameras[3].url, "rtsp://10.0.0.4/a"), "and so in the file");
    char cmd[200];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd)) {}
}

/* A one-connection-at-a-time server on loopback that answers each request
 * with `reply` (RTSP: per request until the blank line; NETIP: per
 * message). */
static struct {
    int fd, netip;
    const char *reply;
    volatile int stop;
    pthread_t t;
} g_srv;

static int srv_recv(int fd, void *b, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t r = recv(fd, (char *)b + got, len - got, 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static void *srv_thread(void *arg)
{
    (void)arg;
    while (!g_srv.stop) {
        int c = accept(g_srv.fd, NULL, NULL);
        if (c < 0) break;
        char buf[4096];
        for (;;) {
            if (g_srv.netip) {
                uint8_t h[20];
                if (srv_recv(c, h, 20) < 0) break;
                uint32_t len = h[16] | h[17] << 8 | h[18] << 16 | (uint32_t)h[19] << 24;
                if (len >= sizeof buf || srv_recv(c, buf, len) < 0) break;
                cam_send(c, (h[14] | h[15] << 8) + 1, g_srv.reply, strlen(g_srv.reply));
                continue;
            }
            size_t got = 0;
            while (got < sizeof buf - 1 && !strstr(buf, "\r\n\r\n")) {
                ssize_t r = recv(c, buf + got, sizeof buf - 1 - got, 0);
                if (r <= 0) goto done;
                got += (size_t)r;
                buf[got] = 0;
            }
            const char *cs = strstr(buf, "CSeq:");
            char reply[2048];
            snprintf(reply, sizeof reply, g_srv.reply, cs ? atoi(cs + 5) : 1);
            if (send(c, reply, strlen(reply), MSG_NOSIGNAL) < 0) break;
            buf[0] = 0;
        }
    done:
        close(c);
    }
    return NULL;
}

static int srv_start(int netip, const char *reply)
{
    memset(&g_srv, 0, sizeof g_srv);
    g_srv.netip = netip;
    g_srv.reply = reply;
    g_srv.fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof a;
    if (bind(g_srv.fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(g_srv.fd, 4) < 0 ||
        getsockname(g_srv.fd, (struct sockaddr *)&a, &al) < 0)
        return 0;
    pthread_create(&g_srv.t, NULL, srv_thread, NULL);
    return ntohs(a.sin_port);
}

static void srv_stop(void)
{
    g_srv.stop = 1;
    shutdown(g_srv.fd, SHUT_RDWR);
    close(g_srv.fd);
    pthread_join(g_srv.t, NULL);
}

/* How a source failed, for the tile: unreachable, refused credentials,
 * a codec vigil has not. */
static src_fail_t open_fail(const char *url, char *err, size_t cap)
{
    source_t *s = source_create(url, 0);
    if (!s) return (src_fail_t)-1;
    static const volatile int no_stop;
    s->stop = &no_stop;
    int rc = s->ops->open(s);
    src_fail_t f = rc < 0 ? s->fail : (src_fail_t)-2;
    snprintf(err, cap, "%s", s->err);
    if (rc == 0) s->ops->close(s);
    source_destroy(s);
    return f;
}

static void test_fail_kinds(void)
{
    char url[128], err[160];
    int fd = socket(AF_INET, SOCK_STREAM, 0);       /* bound, never listening: refused */
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof a;
    bind(fd, (struct sockaddr *)&a, sizeof a);
    getsockname(fd, (struct sockaddr *)&a, &al);
    snprintf(url, sizeof url, "rtsp://127.0.0.1:%d/live", ntohs(a.sin_port));
    CHECK(open_fail(url, err, sizeof err) == SRC_FAIL_UNREACHABLE, "RTSP refused: unreachable (%s)", err);
    snprintf(url, sizeof url, "netip://127.0.0.1:%d/", ntohs(a.sin_port));
    CHECK(open_fail(url, err, sizeof err) == SRC_FAIL_UNREACHABLE, "NETIP refused: unreachable (%s)", err);
    close(fd);

    int port = srv_start(0, "RTSP/1.0 401 Unauthorized\r\nCSeq: %d\r\n"
                            "WWW-Authenticate: Basic realm=\"cam\"\r\n\r\n");
    snprintf(url, sizeof url, "rtsp://admin:wrong@127.0.0.1:%d/live", port);
    CHECK(port && open_fail(url, err, sizeof err) == SRC_FAIL_AUTH, "RTSP 401 with a password: auth (%s)", err);
    srv_stop();
    port = srv_start(0, "RTSP/1.0 200 OK\r\nCSeq: %d\r\nContent-Type: application/sdp\r\nContent-Length: 51\r\n\r\n"
                        "v=0\r\nm=video 0 RTP/AVP 26\r\na=rtpmap:26 JPEG/90000\r\n");
    snprintf(url, sizeof url, "rtsp://127.0.0.1:%d/live", port);
    CHECK(port && open_fail(url, err, sizeof err) == SRC_FAIL_CODEC, "RTSP MJPEG only: codec (%s)", err);
    srv_stop();
    port = srv_start(0, "RTSP/1.0 404 Not Found\r\nCSeq: %d\r\n\r\n");
    snprintf(url, sizeof url, "rtsp://127.0.0.1:%d/nothing", port);
    CHECK(port && open_fail(url, err, sizeof err) == SRC_FAIL_OTHER, "RTSP 404: other (%s)", err);
    srv_stop();
    port = srv_start(1, "{\"Ret\":203}\n");
    snprintf(url, sizeof url, "netip://admin:wrong@127.0.0.1:%d/", port);
    CHECK(port && open_fail(url, err, sizeof err) == SRC_FAIL_AUTH, "NETIP login 203: auth (%s)", err);

    /* ... and what the channel says: auth for that camera, disabled for a
     * camera switched off, nothing for an empty slot. */
    channels_config_t cc = { .urls = { url }, .n = 3, .seg_secs = 60 };
    cc.disabled[1] = 1;
    cc.names[1] = "Gate";
    CHECK(channels_start(&cc, NULL) == 0, "start");
    channel_status_t st;
    WAIT_FOR((channels_status(0, &st), st.status == CH_AUTH), 3000);
    CHECK(st.status == CH_AUTH && !strcmp(ch_state_name(st.status), "auth"), "channel: auth (%s, %s)",
          ch_state_name(st.status), st.state);
    channels_status(1, &st);
    CHECK(st.status == CH_DISABLED && !strcmp(st.name, "Gate"), "switched off: disabled, named");
    channels_status(2, &st);
    CHECK(st.status == CH_EMPTY && !st.configured, "empty slot: empty");
    channels_stop();
    srv_stop();
}

static void test_channel_events(void)
{
    time_t last[CH_EV_TYPES] = { 0 };
    CHECK(channel_events_held(last, 1000) == 0, "nothing reported");
    last[0] = 1000;
    last[3] = 997;
    CHECK(channel_events_held(last, 1000) == (CH_EV_MOTION | CH_EV_VEHICLE), "motion and vehicle held");
    CHECK(channel_events_held(last, 1000 + CHANNEL_EVENT_HOLD - 1) == CH_EV_MOTION, "vehicle's hold over");
    CHECK(channel_events_held(last, 1000 + CHANNEL_EVENT_HOLD) == 0, "both over");
    CHECK(!strcmp(ch_event_name(1), "human") && !strcmp(ch_event_name(2), "face") && !ch_event_name(CH_EV_TYPES),
          "names");
    channels_report_events(5, CH_EV_HUMAN | CH_EV_FACE);
    /* Not running: no status to show it on; the report is kept for when
     * it is, within the hold. */
    channel_status_t st;
    channels_status(5, &st);
    CHECK(st.events == 0 && st.status == CH_EMPTY, "an empty slot shows no events");
}

int main(int argc, char **argv)
{
    g_data = argc > 1 ? argv[1] : "tests/data";
    test_sps();
    test_au_norm();
    test_depay();
    test_rtsp_read();
    test_rtsp_request();
    test_rtsp_helpers();
    test_capture_source();
    test_seg_writer();
    test_rec_place();
    test_net_connect_stop();
    test_schema();
    test_redact();
    test_config_file();
    test_config_store();
    test_events();
    test_channel_tiles();
    test_channel_decode();
    test_sub_url();
    test_channel_substream();
    test_camera_slots();
    test_fail_kinds();
    test_channel_events();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
