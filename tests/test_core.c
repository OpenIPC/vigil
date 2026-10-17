/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_core.c — unit tests for the hardware-free core. */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <pthread.h>

#include "au_norm.h"
#include "hal_capture.h"
#include "rtp_depay.h"
#include "source.h"
#include "rtsp_client.h"
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

int main(int argc, char **argv)
{
    g_data = argc > 1 ? argv[1] : "tests/data";
    test_sps();
    test_au_norm();
    test_depay();
    test_rtsp_read();
    test_rtsp_helpers();
    test_capture_source();
    test_seg_writer();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
