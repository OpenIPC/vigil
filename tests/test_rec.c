/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_rec.c — the recording store (docs/recording.md): its records and
 * MP4 boxes, writing a pool and reading it back, the ring and the quotas,
 * a power cut at every write, the rebuild against the saved index, a
 * 24-hour export with gaps, and the queries on an 8 TB table.
 *
 *   test_rec DATA_DIR [WORK_DIR]
 */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "au_norm.h"
#include "mp4.h"
#include "rec_fmt.h"
#include "rec_pool.h"
#include "rec_read.h"
#include "rec_store.h"
#include "sps.h"

static int g_fail, g_checks;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_fail; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static const char *g_data;
static char g_work[400];

static uint8_t *load(const char *name, size_t *len)
{
    char path[600];
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

static double ms_since(const struct timespec *a)
{
    struct timespec b;
    clock_gettime(CLOCK_MONOTONIC, &b);
    return (b.tv_sec - a->tv_sec) * 1e3 + (b.tv_nsec - a->tv_nsec) / 1e6;
}

static void rm_rf(const char *dir)
{
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) {}
}

/* ---- a clip as access units --------------------------------------------------- */

typedef struct {
    vg_codec_t codec;
    int        n;
    struct { const uint8_t *p; size_t len; int key; } au[4096];
    uint8_t   *buf;
} clip_t;

/* Split an Annex-B stream into access units: one starts at the first VCL
 * unit of a picture, with the units before it. */
static void clip_load(clip_t *c, const char *name, vg_codec_t codec)
{
    size_t len, pos = 0, nl;
    memset(c, 0, sizeof *c);
    c->codec = codec;
    c->buf = load(name, &len);
    const uint8_t *nal, *start = NULL;
    int vcl_seen = 0, key = 0;
    size_t prev_end = 0;
    while ((nal = au_next_nal(c->buf, len, &pos, &nl))) {
        /* back up to the start code */
        const uint8_t *sc = nal;
        while (sc > c->buf && sc[-1] == 0) --sc;
        if (sc > c->buf && sc[-1] == 1) --sc;
        while (sc > c->buf && sc[-1] == 0) --sc;
        int h265 = codec == VG_CODEC_H265;
        int type = h265 ? (nal[0] >> 1) & 0x3F : nal[0] & 0x1F;
        int vcl = h265 ? type < 32 : type >= 1 && type <= 5;
        int first = vcl && (h265 ? nal[2] & 0x80 : nal[1] & 0x80);
        if ((first || (!vcl && vcl_seen)) && vcl_seen && start) {
            c->au[c->n].p = start;
            c->au[c->n].len = (size_t)(sc - start);
            c->au[c->n].key = key;
            if (++c->n == 4096) break;
            start = NULL;
            vcl_seen = 0;
            key = 0;
        }
        if (!start) start = sc;
        if (vcl) {
            vcl_seen = 1;
            key |= h265 ? type >= 16 && type <= 21 : type == 5;
        }
        prev_end = (size_t)(nal - c->buf) + nl;
    }
    if (start && vcl_seen && c->n < 4096) {
        c->au[c->n].p = start;
        c->au[c->n].len = c->buf + prev_end - start;
        c->au[c->n].key = key;
        c->n++;
    }
}

static clip_t g_h264, g_h265;

/* Feed AU i of a clip as camera `cam`'s, at `t_us` (pts and wall alike). */
static void feed(rec_pool_t *p, const clip_t *c, int i, int cam, uint64_t t_us)
{
    vg_au_t au = { .codec = c->codec, .data = c->au[i % c->n].p, .len = c->au[i % c->n].len,
                   .pts_us = t_us, .key = c->au[i % c->n].key };
    rec_au_t *a = rec_au_from(cam, 0, &au, t_us, 0);
    if (!a) return;
    rec_pool_put(p, a);
    free(a);
}

#define T0 1795000000000000ull            /* 2026-11-18, a Unix time in us */
#define FRAME 40000ull

/* Record `secs` seconds of camera `cam` from `t0`, 25 fps, ticking the
 * pool every second; returns the time after. */
static uint64_t record(rec_pool_t *p, const clip_t *c, int cam, uint64_t t0, int secs)
{
    uint64_t t = t0;
    for (int i = 0; i < secs * 25; ++i, t += FRAME) {
        feed(p, c, i, cam, t);
        if (i % 25 == 24) rec_pool_tick(p, (time_t)(t / 1000000), t);
    }
    return t;
}

static rec_pool_cfg_t pool_cfg(const char *dir, unsigned chunk_mb, unsigned pool_mb)
{
    rec_pool_cfg_t c;
    memset(&c, 0, sizeof c);
    snprintf(c.path, sizeof c.path, "%s", dir);
    c.chunk_mb = chunk_mb;
    c.pool_mb = pool_mb;
    c.fragment_s = 4;
    c.flush_s = 2;
    c.index_save_s = 300;
    snprintf(c.owner, sizeof c.owner, "test");
    snprintf(c.host, sizeof c.host, "testhost");
    return c;
}

static int mount_full(rec_pool_t *p, const rec_pool_cfg_t *c)
{
    if (rec_pool_mount(p, c, (time_t)(T0 / 1000000)) < 0) return -1;
    volatile int stop = 0;
    while (rec_pool_rebuild_due(p)) rec_pool_rebuild(p, &stop);
    while (rec_pool_prealloc_due(p)) rec_pool_prealloc(p, &stop);
    return 0;
}

/* Every closed chunk of the pool reads back: its vgix, each fragment's
 * moof and mdat, its samples' NAL lengths, time going forward. Returns
 * the fragments checked, -1 on a fault. */
static int verify_pool(rec_pool_t *p, const char *what)
{
    int total = 0;
    for (uint32_t i = 0; i < p->n; ++i) {
        rec_chunk_t c;
        rec_frag_t *fr;
        if (p->tab[i].state != RC_CLOSED) continue;
        int n = rec_pool_frags(p, i, &c, &fr);
        if (n < 0 || (uint32_t)n != c.nfrag) {
            CHECK(0, "%s: chunk %u: fragment table %d of %u", what, i, n, c.nfrag);
            free(fr);
            return -1;
        }
        char path[600];
        rec_pool_chunk_path(p, i, path, sizeof path);
        int fd = open(path, O_RDONLY);
        uint64_t prev = 0;
        for (int k = 0; k < n; ++k) {
            uint32_t size = fr[k].size & REC_FRAG_SIZE_MASK;
            uint8_t *b = malloc(size);
            mp4_moof_t m;
            int ok = b && pread(fd, b, size, fr[k].off) == (ssize_t)size && mp4_moof_parse(b, size, &m) == 0 &&
                     m.seq == (uint32_t)k + 1 && m.tfdt >= prev && rec_be32(b + rec_be32(b)) == 8 + m.data_size &&
                     rec_be32(b) + 8 + m.data_size == size;
            /* the samples: 4-byte lengths that add up */
            size_t at = rec_be32(b) + 8;
            for (uint32_t s = 0; ok && s < m.n; ++s) {
                mp4_sample_t sm;
                mp4_moof_sample(b, &m, s, &sm);
                size_t end = at + sm.size;
                while (ok && at < end) {
                    uint32_t l = rec_be32(b + at);
                    ok = l && at + 4 + l <= end;
                    at += 4 + l;
                }
            }
            if (!ok) {
                mp4_moof_t mm;
                int pr = b ? mp4_moof_parse(b, size, &mm) : -9;
                CHECK(0, "%s: chunk %u fragment %d does not read back (off %u size %u parse %d seq %u tfdt %llu prev %llu)",
                      what, i, k, fr[k].off, size, pr, pr == 0 ? mm.seq : 0, pr == 0 ? (unsigned long long)mm.tfdt : 0ull,
                      (unsigned long long)prev);
                free(b);
                close(fd);
                free(fr);
                return -1;
            }
            prev = m.tfdt + m.dur;
            free(b);
            ++total;
        }
        close(fd);
        free(fr);
    }
    return total;
}

/* ---- records and boxes ----------------------------------------------------------- */

static void test_records(void)
{
    uint64_t pool = 0x1122334455667788ull;
    rec_chunk_t c = { .state = RC_CLOSED, .cam = 3, .stream = 1, .codec = 2, .flags = RCF_GAPS, .use = 7,
                      .nfrag = 123, .start_us = T0, .end_us = T0 + 512345678, .bytes = 12345678, .init_len = 777,
                      .width = 2560, .height = 1440, .last_off = 12000000, .vgix_len = 2024, .no = 31999 }, d;
    uint8_t b[REC_CHUNK_REC];
    rec_chunk_encode(&c, pool, b);
    CHECK(rec_chunk_decode(&d, pool, b) == 0 && !memcmp(&c, &d, sizeof c), "chunk record round trip");
    CHECK(rec_chunk_decode(&d, pool + 1, b) < 0, "another pool's chunk record does not check");
    b[17] ^= 1;
    CHECK(rec_chunk_decode(&d, pool, b) < 0, "a flipped bit does not check");

    /* vgch: B wins when it is this use's close; a B of an earlier use does not. */
    uint8_t box[REC_VGCH_SIZE];
    rec_chunk_t a = c;
    a.state = RC_OPEN;
    a.nfrag = 0;
    rec_vgch_build(box, pool, &a, &c);
    CHECK(rec_vgch_pool(box) == pool && rec_vgch_parse(box, pool, &d) == 0 && d.state == RC_CLOSED && d.nfrag == 123,
          "vgch: the close copy");
    rec_chunk_t old = c;
    old.use = 6;
    rec_vgch_build(box, pool, &a, &old);
    CHECK(rec_vgch_parse(box, pool, &d) == 0 && d.state == RC_OPEN && d.use == 7, "vgch: an earlier use's B is not taken");
    rec_vgch_build(box, pool, &a, &c);
    memset(box + REC_VGCH_B + 10, 0xAB, 20);             /* a torn rewrite of B */
    CHECK(rec_vgch_parse(box, pool, &d) == 0 && d.state == RC_OPEN, "vgch: a torn B leaves A");
    CHECK(rec_vgch_parse(box, pool ^ 1, &d) < 0, "vgch: another pool's");

    rec_jrec_t j = { .type = RJ_FRAG, .a = 0x40, .b = 0x34, .c = 0x12, .chunk = 9, .use = 3, .w0 = 1, .w1 = 2,
                     .w2 = 3, .w3 = 4 | REC_FRAG_KEY }, k;
    uint8_t jb[REC_JREC];
    rec_jrec_encode(&j, pool, jb);
    CHECK(rec_jrec_decode(&k, pool, jb) == 0 && !memcmp(&j, &k, sizeof j), "journal record round trip");
    rec_jrec_t o = { .type = RJ_OPEN, .a = 1, .b = 0, .c = 1, .chunk = 5, .use = 1, .u64 = T0, .w0 = 700,
                     .w1 = 640 | 360u << 16 };
    rec_jrec_encode(&o, pool, jb);
    CHECK(rec_jrec_decode(&k, pool, jb) == 0 && k.u64 == T0 && k.w1 == o.w1, "journal open record");
    jb[31] ^= 0x80;
    CHECK(rec_jrec_decode(&k, pool, jb) < 0, "a torn journal record does not check");

    rec_event_t e = { .time_us = T0 + 5, .dur_ms = 12000, .cam = 4, .type = REC_EV_HUMAN, .detail = 2, .chunk = 17,
                      .frag = 3, .seq = 99 }, f;
    uint8_t eb[REC_EVREC];
    rec_event_encode(&e, pool, eb);
    CHECK(rec_event_decode(&f, pool, eb) == 0 && !memcmp(&e, &f, sizeof e), "event record round trip");
    CHECK(rec_event_type("vehicle") == REC_EV_VEHICLE && !strcmp(rec_event_name(REC_EV_MOTION), "motion") &&
          !rec_event_type("nope") && !rec_event_name(0) && rec_event_type("line_crossing") == REC_EV_LINE &&
          !strcmp(rec_event_name(REC_EV_OTHER), "other"), "event names");

    rec_frag_t fr[3] = { { 0, 2000, 4800, 70000 | REC_FRAG_KEY }, { 2000, 2000, 74800, 60000 },
                         { 9000, 1000, 134800, 9000 | REC_FRAG_KEY | REC_FRAG_GAP } };
    size_t z = rec_vgix_size(3, 1);
    uint8_t *ix = malloc(z);
    rec_vgix_build(ix, pool, 7, fr, 3, &e, 1);
    uint32_t nf, ne;
    const uint8_t *pf, *pe;
    CHECK(rec_vgix_parse(ix, z, pool, 7, &nf, &pf, &ne, &pe) == 0 && nf == 3 && ne == 1, "vgix round trip");
    rec_frag_t g;
    rec_frag_decode(&g, pf + 2 * REC_FRAG);
    CHECK(!memcmp(&g, &fr[2], sizeof g), "vgix fragment entry");
    CHECK(rec_vgix_parse(ix, z, pool, 8, &nf, &pf, &ne, &pe) < 0, "vgix of another use");
    ix[z - 1] ^= 1;
    CHECK(rec_vgix_parse(ix, z, pool, 7, &nf, &pf, &ne, &pe) < 0, "vgix with a flipped bit");
    free(ix);

    rec_table_hdr_t th = { pool, 42, 256u << 20, 32000, 0xDEADBEEF }, th2;
    uint8_t hb[REC_TABLE_HDR];
    rec_table_hdr_encode(&th, hb);
    CHECK(rec_table_hdr_decode(&th2, hb) == 0 && !memcmp(&th, &th2, sizeof th), "table header round trip");
    CHECK(rec_crc32c(0, "123456789", 9) == 0xE3069283, "CRC32C check value");
}

/* Parameter sets of a clip's first keyframe, without start codes. */
static int clip_ps(const clip_t *c, mp4_track_t *t)
{
    memset(t, 0, sizeof *t);
    t->codec = c->codec;
    size_t pos = 0, nl;
    const uint8_t *nal;
    for (int i = 0; i < c->n && !c->au[i].key; ++i) {}
    int k = 0;
    while (!c->au[k].key) ++k;
    int h265 = c->codec == VG_CODEC_H265;
    while ((nal = au_next_nal(c->au[k].p, c->au[k].len, &pos, &nl))) {
        int type = h265 ? (nal[0] >> 1) & 0x3F : nal[0] & 0x1F;
        int slot = h265 ? (type >= 32 && type <= 34 ? type - 32 : -1) : type == 7 ? 0 : type == 8 ? 1 : -1;
        if (slot < 0 || t->ps[slot]) continue;
        t->ps[slot] = nal;
        t->ps_len[slot] = nl;
        t->n_ps++;
    }
    vg_stream_info_t si;
    if (sps_parse(c->codec, t->ps[h265 ? 1 : 0], t->ps_len[h265 ? 1 : 0], &si) < 0) return -1;
    t->width = si.width;
    t->height = si.height;
    return 0;
}

static void test_mp4(void)
{
    mp4_track_t t;
    char codecs[64], c2[64];
    for (int k = 0; k < 2; ++k) {
        const clip_t *c = k ? &g_h265 : &g_h264;
        CHECK(clip_ps(c, &t) == 0 && t.width == 640 && t.height == 360, "clip %d parameter sets", k);
        mp4_buf_t b = { 0 };
        CHECK(mp4_init(&b, &t, -1, 0) == 0 && b.len > 200 && !memcmp(b.p + 4, "ftyp", 4), "init segment %d", k);
        CHECK(mp4_codec_string(&t, codecs, sizeof codecs) == 0 &&
              mp4_init_codecs(b.p, b.len, c2, sizeof c2) == 0 && !strcmp(codecs, c2) &&
              !strncmp(codecs, k ? "hvc1." : "avc1.", 5), "codecs %s / %s", codecs, c2);
        mp4_buf_t e = { 0 };
        CHECK(mp4_init_edit(b.p, b.len, 45000, 7000, &e) == 0 && e.len > b.len + 36, "init with an edit list");
        CHECK(mp4_init_codecs(e.p, e.len, c2, sizeof c2) == 0 && !strcmp(codecs, c2), "the edited init still parses");
        mp4_buf_free(&b);
        mp4_buf_free(&e);
    }
    /* moof round trip, B-frames' signed offsets included */
    mp4_sample_t s[4] = { { 1000, 3600, MP4_SYNC, 0 }, { 200, 3600, MP4_NON_SYNC, 7200 },
                          { 300, 3600, MP4_NON_SYNC, -3600 }, { 300, 3600, MP4_NON_SYNC, -3600 } };
    uint8_t mb[256];
    size_t msz = mp4_moof_size(4, 1);
    mp4_moof_build(mb, 5, 123456789012ull, s, 4, 1);
    mp4_moof_t m;
    mp4_sample_t x;
    CHECK(mp4_moof_parse(mb, msz, &m) == 0 && m.seq == 5 && m.tfdt == 123456789012ull && m.n == 4 &&
          m.data_size == 1800 && m.dur == 14400 && m.data_offset == msz + 8 && m.first_flags == MP4_SYNC, "moof parse");
    mp4_moof_sample(mb, &m, 2, &x);
    CHECK(x.cts == -3600 && x.size == 300, "moof sample with a negative offset");
    mp4_moof_patch(mb, &m, 77, 42);
    CHECK(mp4_moof_parse(mb, msz, &m) == 0 && m.seq == 77 && m.tfdt == 42, "moof patch");
}

/* ---- writing and reading a pool ----------------------------------------------------- */

static void test_pool_basic(void)
{
    char dir[600];
    snprintf(dir, sizeof dir, "%s/basic", g_work);
    rm_rf(dir);
    rec_pool_t p;
    rec_pool_init(&p);
    rec_pool_cfg_t cfg = pool_cfg(dir, 1, 8);
    CHECK(mount_full(&p, &cfg) == 0 && p.n == 8 && p.state == RP_READY, "a new pool of 8 chunks: %s", p.why);
    uint64_t t = record(&p, &g_h264, 0, T0, 30);
    /* a 10 s pause, then 20 s more: a gap inside the chunk */
    rec_pool_break(&p, 0, 0);
    t = record(&p, &g_h264, 0, t + 10000000, 20);
    record(&p, &g_h265, 1, T0, 20);
    rec_pool_event(&p, 0, REC_EV_MOTION, 1, T0 + 5000000);
    rec_pool_event(&p, 0, REC_EV_MOTION, 1, T0 + 6000000);   /* going on: no second record */
    rec_pool_event(&p, 0, REC_EV_MOTION, 0, T0 + 12000000);
    rec_pool_tick(&p, (time_t)(T0 / 1000000) + 20, T0 + 20000000);
    rec_pool_event(&p, 1, REC_EV_HUMAN, 1, T0 + 21000000);
    rec_pool_unmount(&p);

    CHECK(mount_full(&p, &cfg) == 0, "mounted again");
    int closed = 0, cam1 = 0;
    for (uint32_t i = 0; i < p.n; ++i) {
        closed += p.tab[i].state == RC_CLOSED;
        cam1 += p.tab[i].cam == 1 && p.tab[i].state == RC_CLOSED;
    }
    CHECK(closed >= 3 && cam1 >= 2, "closed chunks: %d, CAM1's %d", closed, cam1);
    int frags = verify_pool(&p, "basic");
    CHECK(frags > 20, "fragments read back: %d", frags);

    rec_span_t *sp;
    int n = rec_q_spans(&p, 1, 0, T0 - 1000000, T0 + 3600000000ull, &sp);
    CHECK(n == 2 && sp[0].from_us <= T0 + 100 && sp[0].to_us >= T0 + 29900000 && sp[0].to_us <= T0 + 30100000 &&
          sp[1].from_us >= T0 + 39900000 && sp[1].to_us >= T0 + 59900000,
          "CAM1: two spans around the pause: %d (%.3f-%.3f, %.3f-%.3f)", n, n ? (sp[0].from_us - T0) / 1e6 : 0,
          n ? (sp[0].to_us - T0) / 1e6 : 0, n > 1 ? (sp[1].from_us - T0) / 1e6 : 0, n > 1 ? (sp[1].to_us - T0) / 1e6 : 0);
    free(sp);
    n = rec_q_spans(&p, 2, 0, T0, T0 + 10000000, &sp);
    CHECK(n == 1 && sp[0].from_us == T0 && sp[0].to_us == T0 + 10000000, "CAM2: a span clipped to the range");
    free(sp);

    uint32_t mask;
    time_t day = (time_t)(T0 / 1000000);
    struct tm tm;
    localtime_r(&day, &tm);
    CHECK(rec_q_days(&p, 1, 0, tm.tm_year + 1900, tm.tm_mon + 1, &mask) == 0 && mask == 1u << (tm.tm_mday - 1),
          "days: the day recorded (%08x)", mask);
    CHECK(rec_q_days(&p, 3, 0, tm.tm_year + 1900, tm.tm_mon + 1, &mask) == 0 && !mask, "days: none for CAM3");

    rec_event_t *ev;
    n = rec_q_events(&p, 0, 0, T0, T0 + 60000000, 0, 100, &ev);
    CHECK(n == 2 && ev[0].cam == 1 && ev[0].type == REC_EV_MOTION && ev[0].time_us == T0 + 5000000 &&
          ev[0].dur_ms == 7000 && ev[0].chunk != REC_NONE && ev[1].cam == 2 && ev[1].type == REC_EV_HUMAN,
          "events: two, the motion lasting 7 s: %d (%u ms)", n, n ? ev[0].dur_ms : 0);
    free(ev);
    n = rec_q_events(&p, 2, 0, T0, T0 + 60000000, 1, 100, &ev);
    CHECK(n == 1 && ev[0].cam == 2, "events of CAM2 only");
    free(ev);
    n = rec_q_events(&p, 0, REC_EV_MOTION, T0 + 6000000, T0 + 60000000, 0, 100, &ev);
    CHECK(n == 0, "events from a time on");
    free(ev);
    /* The chunk's vgix has CAM1's event too. */
    int found = 0;
    for (uint32_t i = 0; i < p.n; ++i) {
        rec_event_t *ce;
        int k = rec_pool_chunk_events(&p, i, &ce);
        for (int j = 0; j < k; ++j) found += ce[j].cam == 1 && ce[j].type == REC_EV_MOTION;
        free(ce);
    }
    CHECK(found == 1, "the motion event is in its chunk's vgix: %d", found);
    rec_pool_unmount(&p);
    rec_pool_free(&p);
}

/* ---- ring, quotas, on_full ---------------------------------------------------------- */

static void test_ring(void)
{
    char dir[600];
    snprintf(dir, sizeof dir, "%s/ring", g_work);
    rm_rf(dir);
    rec_pool_t p;
    rec_pool_init(&p);
    rec_pool_cfg_t cfg = pool_cfg(dir, 1, 6);
    CHECK(mount_full(&p, &cfg) == 0 && p.n == 6, "a pool of 6");
    /* Far more than 6 MiB: the ring goes round; the oldest go first. */
    uint64_t t = T0;
    for (int k = 0; k < 8; ++k) t = record(&p, &g_h264, 0, t, 60);
    rec_pool_save(&p);
    uint64_t oldest = UINT64_MAX;
    int used = 0, max_use = 0;
    for (uint32_t i = 0; i < p.n; ++i) {
        if (p.tab[i].state != RC_CLOSED && p.tab[i].state != RC_OPEN) continue;
        ++used;
        if (p.tab[i].start_us < oldest) oldest = p.tab[i].start_us;
        if ((int)p.tab[i].use > max_use) max_use = (int)p.tab[i].use;
    }
    CHECK(used == 6 && max_use >= 2 && oldest > T0 + 60000000ull, "the ring went round: %d used, use %d, oldest +%.0f s",
          used, max_use, (oldest - T0) / 1e6);
    rec_span_t *sp;
    int n = rec_q_spans(&p, 1, 0, T0, t + 1, &sp);
    CHECK(n == 1 && sp[0].to_us > t - 5000000, "one span to the end, the chunks joined: %d", n);
    free(sp);

    /* A quota: CAM2 limited to 2 chunks takes back its own; CAM1 keeps
     * the rest. */
    cfg.quota_bytes[1] = 2u << 20;
    rec_pool_update(&p, &cfg);
    for (int k = 0; k < 3; ++k) t = record(&p, &g_h264, 1, t, 60);
    int c2 = 0;
    for (uint32_t i = 0; i < p.n; ++i) c2 += p.tab[i].cam == 2 && p.tab[i].state != RC_FREE;
    CHECK(c2 <= 2, "CAM2 within its quota of 2 chunks: %d", c2);
    /* on_full stop: nothing is reused, the stream says it is full */
    cfg.stop_when_full = 1;
    cfg.quota_bytes[1] = 0;
    rec_pool_update(&p, &cfg);
    uint32_t uses[64];
    for (uint32_t i = 0; i < p.n; ++i) uses[i] = p.tab[i].use;
    t = record(&p, &g_h264, 0, t, 120);
    int reused = 0;
    for (uint32_t i = 0; i < p.n; ++i) reused += p.tab[i].use != uses[i];
    CHECK(reused == 0 && p.ws[0][0].full, "on_full stop: no chunk reused, full (%d)", reused);
    /* the age quota frees CAM2's chunks */
    cfg.stop_when_full = 0;
    cfg.quota_age[1] = 60;
    rec_pool_update(&p, &cfg);
    p.next_quota = 0;
    rec_pool_tick(&p, (time_t)(t / 1000000) + 3600, t + 3600000000ull);
    c2 = 0;
    for (uint32_t i = 0; i < p.n; ++i) c2 += p.tab[i].cam == 2 && p.tab[i].state == RC_CLOSED;
    CHECK(c2 == 0, "age quota: CAM2's old chunks freed (%d left)", c2);
    CHECK(rec_pool_delete(&p, 1, 0, UINT64_MAX) >= 1, "delete frees CAM1's closed chunks");
    rec_pool_unmount(&p);
    CHECK(mount_full(&p, &cfg) == 0, "mounted after the frees");
    int alive = 0;
    for (uint32_t i = 0; i < p.n; ++i) alive += p.tab[i].state == RC_CLOSED;
    CHECK(alive == 0, "the freed chunks stay free: %d closed", alive);
    rec_pool_unmount(&p);
    rec_pool_free(&p);
}

/* ---- the rebuild equals the index ---------------------------------------------------- */

static int same_chunk(const rec_chunk_t *a, const rec_chunk_t *b)
{
    if (a->state == RC_FREE && b->state == RC_FREE) return 1;
    return !memcmp(a, b, sizeof *a);
}

static void test_rebuild(void)
{
    char dir[600], path[700];
    snprintf(dir, sizeof dir, "%s/rebuild", g_work);
    rm_rf(dir);
    rec_pool_t p;
    rec_pool_init(&p);
    rec_pool_cfg_t cfg = pool_cfg(dir, 1, 10);
    mount_full(&p, &cfg);
    uint64_t t = T0;
    for (int k = 0; k < 6; ++k) {
        t = record(&p, &g_h264, 0, t, 40);
        record(&p, &g_h265, 1, t - 40000000, 40);
        rec_pool_event(&p, 0, REC_EV_MOTION, 1, t - 1000000);
        rec_pool_event(&p, 1, REC_EV_VEHICLE, 1, t - 500000);
        rec_pool_event(&p, 0, REC_EV_MOTION, 0, t + 4000000);
        rec_pool_event(&p, 1, REC_EV_VEHICLE, 0, t + 4500000);
        rec_pool_tick(&p, (time_t)(t / 1000000) + 6, t + 6000000);
        rec_pool_break(&p, 0, 0);
        t += 5000000;
    }
    rec_pool_unmount(&p);
    CHECK(mount_full(&p, &cfg) == 0, "mounted");
    uint32_t n = p.n;
    rec_chunk_t *saved = malloc(n * sizeof *saved);
    memcpy(saved, p.tab, n * sizeof *saved);
    rec_event_t *ev0;
    int ne0 = rec_q_events(&p, 0, 0, 0, UINT64_MAX, 0, 1000, &ev0);
    rec_pool_unmount(&p);

    /* The index gone: the chunks' headers tell it all again. */
    static const char *const files[] = { "index/chunks.a", "index/chunks.b", "index/journal", "index/events.log" };
    for (int i = 0; i < 4; ++i) {
        snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        unlink(path);
    }
    CHECK(rec_pool_mount(&p, &cfg, (time_t)(t / 1000000)) == 0 && p.rebuilding, "no index: a rebuild");
    volatile int stop = 0;
    /* Recording may start before the rebuild is through: with no chunk
     * known yet, it waits (not "disk full"). */
    feed(&p, &g_h264, 0, 2, t);
    CHECK(!p.ws[2][0].open && !p.ws[2][0].full, "nothing known yet: not recording, not full");
    rec_pool_rebuild(&p, &stop);
    CHECK(!p.rebuilding && p.n == n, "rebuilt: %u chunks", p.n);
    int same = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (same_chunk(&saved[i], &p.tab[i])) ++same;
        else fprintf(stderr, "  chunk %u: saved state %d use %u nfrag %u end %llu / rebuilt state %d use %u nfrag %u end %llu\n",
                     i, saved[i].state, saved[i].use, saved[i].nfrag, (unsigned long long)saved[i].end_us,
                     p.tab[i].state, p.tab[i].use, p.tab[i].nfrag, (unsigned long long)p.tab[i].end_us);
    }
    CHECK(same == (int)n, "the rebuilt table equals the saved one: %d of %u", same, n);
    rec_event_t *ev1;
    int ne1 = rec_q_events(&p, 0, 0, 0, UINT64_MAX, 0, 1000, &ev1);
    /* What the chunks still hold: the events of the chunks not reused since. */
    int keep = 0;
    for (int i = 0; i < ne0; ++i) {
        const rec_chunk_t *c = ev0[i].chunk < n ? &saved[ev0[i].chunk] : NULL;
        if (c && c->state == RC_CLOSED && c->cam == ev0[i].cam && ev0[i].time_us >= c->start_us &&
            ev0[i].time_us <= c->end_us + 6000000)
            ev0[keep++] = ev0[i];
    }
    int ev_same = keep == ne1 && keep >= 4;
    for (int i = 0; ev_same && i < keep; ++i)
        ev_same = ev0[i].time_us == ev1[i].time_us && ev0[i].cam == ev1[i].cam && ev0[i].type == ev1[i].type &&
                  ev0[i].dur_ms == ev1[i].dur_ms && ev0[i].chunk == ev1[i].chunk;
    CHECK(ev_same, "the event log rebuilt from the chunks: %d of %d events still in chunks, %d rebuilt", keep, ne0,
          ne1);
    free(ev0);
    free(ev1);
    rec_pool_unmount(&p);
    /* and saved: no rebuild the next time */
    CHECK(rec_pool_mount(&p, &cfg, (time_t)(t / 1000000)) == 0 && !p.rebuilding, "saved after the rebuild");
    rec_pool_unmount(&p);
    rec_pool_free(&p);
    free(saved);
}

/* ---- power cuts --------------------------------------------------------------------- */

/* The disk as a power cut leaves it: writes up to number `cut` happen
 * (that one torn in half), none after; with `revert`, every write not
 * synced since is undone too (the page cache lost). */
static struct {
    long   count, cut;
    int    revert, cutting;
    struct { char path[160]; off_t off, size; size_t len; uint8_t *old; int synced; } log[200000];
    int    n_log;
} g_pc;

/* Writes are logged by file, not by descriptor: after the cut the pool
 * goes on closing and opening chunks, and their numbers come back. */
static void fd_path(int fd, char *out, size_t cap)
{
    char l[32];
    snprintf(l, sizeof l, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(l, out, cap - 1);
    out[n > 0 ? n : 0] = 0;
}

static ssize_t pc_pwrite(int fd, const void *buf, size_t len, off_t off)
{
    size_t asked = len;
    if (g_pc.cutting) return (ssize_t)len;             /* the power is off: lost */
    if (++g_pc.count >= g_pc.cut) {
        g_pc.cutting = 1;
        len /= 2;
    }
    if (g_pc.revert && g_pc.n_log < (int)(sizeof g_pc.log / sizeof g_pc.log[0])) {
        __typeof__(g_pc.log[0]) *e = &g_pc.log[g_pc.n_log++];
        struct stat st;
        e->old = malloc(len ? len : 1);
        ssize_t r = pread(fd, e->old, len, off);
        e->len = r > 0 ? (size_t)r : 0;
        e->off = off;
        e->size = fstat(fd, &st) == 0 ? st.st_size : 0;
        e->synced = 0;
        fd_path(fd, e->path, sizeof e->path);
    }
    ssize_t w = pwrite(fd, buf, len, off);
    return w < 0 ? w : g_pc.cutting ? (ssize_t)asked : w;
}

static int pc_sync(int fd)
{
    if (g_pc.cutting) return 0;
    char path[160];
    fd_path(fd, path, sizeof path);
    for (int i = 0; i < g_pc.n_log; ++i)
        if (!strcmp(g_pc.log[i].path, path)) g_pc.log[i].synced = 1;
    return 0;
}

/* Undo what was not synced, newest first: old bytes back, a file grown
 * by the write back to its size. */
static void pc_revert(void)
{
    for (int i = g_pc.n_log; i-- > 0;) {
        __typeof__(g_pc.log[0]) *e = &g_pc.log[i];
        if (e->synced) continue;
        int fd = open(e->path, O_WRONLY);
        if (fd < 0) continue;
        if (e->len && pwrite(fd, e->old, e->len, e->off) < 0) {}
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size > e->size && ftruncate(fd, e->size) < 0) {}
        close(fd);
    }
}

/* What the disk holds for sure, after each flush that happened before the cut. */
typedef struct {
    rec_chunk_t tab[16];
    uint32_t    journaled[16];          /* open chunks: fragments the journal vouches for */
    int         n, valid;
} truth_t;
static truth_t g_truth;
static rec_pool_t *g_pc_pool;

static void pc_flush(void *user)
{
    (void)user;
    if (g_pc.cutting) return;
    rec_pool_t *p = g_pc_pool;
    g_truth.n = (int)p->n;
    memcpy(g_truth.tab, p->tab, p->n * sizeof *p->tab);
    memset(g_truth.journaled, 0, sizeof g_truth.journaled);
    for (int c = 0; c < REC_CAMS; ++c)
        if (p->ws[c][0].open) g_truth.journaled[p->ws[c][0].idx] = p->ws[c][0].journaled;
    g_truth.valid = 1;
}

/* Close chunks are only vouched for once their close is in the journal:
 * truth for a chunk is the last flush's. */
static int pc_run(const char *dir, long cut, int revert, long *writes)
{
    rm_rf(dir);
    rec_pool_t p;
    rec_pool_init(&p);
    rec_pool_cfg_t cfg = pool_cfg(dir, 1, 4);
    mount_full(&p, &cfg);
    memset(&g_truth, 0, sizeof g_truth);
    g_pc.count = 0;
    g_pc.cut = cut;
    g_pc.revert = revert;
    g_pc.cutting = 0;
    g_pc.n_log = 0;
    g_pc_pool = &p;
    p.io.pwrite = pc_pwrite;
    p.io.fdatasync = pc_sync;
    p.on_flush = pc_flush;
    pc_flush(NULL);
    uint64_t t = T0;
    /* Two cameras, the ring going round once, a pause. */
    for (int k = 0; k < 3; ++k) {
        for (int i = 0; i < 25 * 30; ++i, t += FRAME) {
            feed(&p, &g_h264, i, 0, t);
            if (i % 2 == 0) feed(&p, &g_h265, i / 2, 1, t);
            if (i % 25 == 24) rec_pool_tick(&p, (time_t)(t / 1000000), t);
        }
        rec_pool_break(&p, 0, 0);
    }
    *writes = g_pc.count;
    /* The power goes. */
    if (revert) pc_revert();
    for (int i = 0; i < g_pc.n_log; ++i) free(g_pc.log[i].old);
    g_pc.n_log = 0;
    rec_pool_abandon(&p);
    truth_t truth = g_truth;
    if (getenv("REC_CUT_DEBUG")) {
        char cmd[1400];
        snprintf(cmd, sizeof cmd, "rm -rf '%s.copy%d' && cp -a '%s' '%s.copy%d'", dir, revert, dir, dir, revert);
        if (system(cmd)) {}
        for (int i = 0; i < truth.n; ++i)
            fprintf(stderr, "  truth chunk %d: state %d use %u nfrag %u bytes %u journaled %u\n", i, truth.tab[i].state,
                    truth.tab[i].use, truth.tab[i].nfrag, truth.tab[i].bytes, truth.journaled[i]);
    }

    /* Back on. */
    rec_pool_cfg_t c2 = cfg;
    int bad = 0;
    p.io.pwrite = NULL;
    p.io.fdatasync = NULL;
    p.on_flush = NULL;
    if (mount_full(&p, &c2) < 0) {
        CHECK(0, "cut %ld%s: does not mount: %s", cut, revert ? " (reverted)" : "", p.why);
        rec_pool_free(&p);
        return -1;
    }
    for (int i = 0; truth.valid && i < truth.n; ++i) {
        const rec_chunk_t *w = &truth.tab[i], *g = &p.tab[i];
        if (w->state == RC_CLOSED && !(g->state == RC_CLOSED && g->use == w->use && g->nfrag == w->nfrag &&
                                       g->end_us == w->end_us && g->bytes == w->bytes)) {
            /* A chunk closed and vouched for may only have been reused
             * since, never lost. */
            if (!(g->use > w->use)) {
                CHECK(0, "cut %ld%s: closed chunk %d lost (state %d use %u/%u nfrag %u/%u)", cut,
                      revert ? " (reverted)" : "", i, g->state, g->use, w->use, g->nfrag, w->nfrag);
                bad = 1;
            }
        }
        if (w->state == RC_OPEN && truth.journaled[i] &&
            !(g->use > w->use || (g->state == RC_CLOSED && g->use == w->use && g->nfrag >= truth.journaled[i]))) {
            CHECK(0, "cut %ld%s: open chunk %d lost journaled fragments (state %d use %u/%u, %u of %u)", cut,
                  revert ? " (reverted)" : "", i, g->state, g->use, w->use, g->nfrag, truth.journaled[i]);
            bad = 1;
        }
    }
    for (uint32_t i = 0; i < p.n; ++i)
        if (p.tab[i].state == RC_OPEN) {
            CHECK(0, "cut %ld: chunk %u still open after the recovery", cut, i);
            bad = 1;
        }
    if (verify_pool(&p, "after a cut") < 0) bad = 1;
    /* and it records on */
    record(&p, &g_h264, 0, t + 1000000, 20);
    rec_pool_unmount(&p);
    if (mount_full(&p, &c2) < 0 || verify_pool(&p, "after a cut, recorded on") < 0) bad = 1;
    rec_pool_unmount(&p);
    rec_pool_free(&p);
    return bad ? -1 : 0;
}

static void test_power_cut(int all)
{
    char dir[600];
    snprintf(dir, sizeof dir, "%s/cut", g_work);
    long writes = 0;
    pc_run(dir, LONG_MAX, 0, &writes);          /* no cut: count the writes */
    CHECK(writes > 100, "the scenario writes %ld times", writes);
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int runs = 0, bad = 0;
    long step = all ? 1 : writes / 97 + 1, first = 1;
    if (getenv("REC_CUT_AT")) first = step = atol(getenv("REC_CUT_AT")), writes = first;
    for (long k = first; k <= writes; k += step)
        for (int rv = 0; rv < 2; ++rv) {
            long w;
            bad += pc_run(dir, k, rv, &w) < 0;
            ++runs;
        }
    CHECK(!bad, "power cuts: %d of %d runs lost what was vouched for", bad, runs);
    printf("power cut: %d runs (every %ld of %ld writes, torn, with and without the unsynced writes) in %.1f s, "
           "%d bad\n", runs, step, writes, ms_since(&t0) / 1000, bad);
    rm_rf(dir);
}

/* ---- a 24-hour export with gaps --------------------------------------------------------- */

typedef struct {
    FILE    *f;
    uint64_t bytes;
} fsink_t;

static int to_file(void *user, const void *data, size_t len)
{
    fsink_t *s = user;
    s->bytes += len;
    return fwrite(data, 1, len, s->f) == len ? 0 : -1;
}

/* ffprobe's answer about a file, "" without ffprobe. */
static void probe(const char *file, const char *entries, char *out, size_t cap)
{
    char cmd[900];
    out[0] = 0;
    if (system("command -v ffprobe >/dev/null 2>&1")) return;
    snprintf(cmd, sizeof cmd, "ffprobe -v error -count_frames -select_streams v:0 -show_entries %s -of csv=p=0 '%s' 2>&1",
             entries, file);
    FILE *f = popen(cmd, "r");
    if (!f) return;
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = 0;
    pclose(f);
    out[strcspn(out, "\n")] = 0;
}

static void test_export_day(void)
{
    char dir[600], file[700];
    snprintf(dir, sizeof dir, "%s/day", g_work);
    rm_rf(dir);
    rec_pool_t p;
    rec_pool_init(&p);
    rec_pool_cfg_t cfg = pool_cfg(dir, 1, 40);
    mount_full(&p, &cfg);
    /* A day of event recording: 60 s every two hours. */
    uint64_t day0 = T0;
    for (int h = 0; h < 24; h += 2) {
        record(&p, &g_h264, 0, day0 + (uint64_t)h * 3600000000ull, 60);
        rec_pool_break(&p, 0, 0);
    }
    rec_pool_unmount(&p);
    mount_full(&p, &cfg);
    rec_span_t *sp;
    int n = rec_q_spans(&p, 1, 0, day0, day0 + 86400000000ull, &sp);
    CHECK(n == 12, "12 spans in the day: %d", n);
    free(sp);

    for (int mode = 0; mode < 2; ++mode) {
        /* From 30.5 s into the first span to 20 s into the last. */
        uint64_t from = day0 + 30500000, to = day0 + 22 * 3600000000ull + 20000000;
        rec_plan_t pl;
        CHECK(rec_q_plan(&p, 1, 0, from, to, 0, 0, &pl) == 0 && pl.n > 12 && !pl.format_change, "a plan: %d", pl.n);
        CHECK(pl.p[0].key, "the export starts at a keyframe");
        mp4_buf_t init = { 0 };
        CHECK(rec_q_export_init(&pl, from, to, mode, &init) == 0 && rec_q_export_sidx(&pl, mode, &init) == 0,
              "export init and segment index");
        snprintf(file, sizeof file, "%s/day-%s.mp4", g_work, mode ? "remove" : "keep");
        fsink_t s = { fopen(file, "wb"), 0 };
        CHECK(s.f && to_file(&s, init.p, init.len) == 0 && rec_q_send(&p, &pl, pl.first_tick, mode, 1, to_file, &s) == 0,
              "export sent");
        fclose(s.f);
        CHECK(s.bytes == init.len + pl.bytes, "the size is known ahead: %llu", (unsigned long long)s.bytes);
        /* Read the decode times back: a jump at each gap, or none. */
        FILE *f = fopen(file, "rb");
        uint8_t *b = malloc(s.bytes);
        CHECK(fread(b, 1, s.bytes, f) == s.bytes, "read back");
        fclose(f);
        uint64_t at = init.len, prev_end = 0, jumps = 0, frames = 0, seq = 0, ok = 1;
        while (at + 8 <= s.bytes) {
            uint32_t sz = rec_be32(b + at);
            mp4_moof_t m;
            if (mp4_moof_parse(b + at, sz, &m) < 0 || m.seq != ++seq) {
                ok = 0;
                break;
            }
            if (m.tfdt > prev_end + 90000) ++jumps;
            if (m.tfdt < prev_end) ok = 0;
            prev_end = m.tfdt + m.dur;
            frames += m.n;
            at += sz + rec_be32(b + at + sz);
        }
        free(b);
        CHECK(ok && at == s.bytes, "%s: the fragments follow on", mode ? "remove" : "keep");
        CHECK(mode ? jumps == 0 : jumps == 11, "%s: %llu jumps in time", mode ? "remove" : "keep",
              (unsigned long long)jumps);
        /* 11 spans of 60 s, the first from its keyframe before 30.5 s, the last to 20 s, whole fragments */
        CHECK(frames >= (30 + 10 * 60 + 20) * 25 && frames <= (34 + 10 * 60 + 24) * 25, "%llu frames",
              (unsigned long long)frames);
        char out[256];
        probe(file, "stream=codec_name,nb_read_frames", out, sizeof out);
        if (out[0]) {
            unsigned long fr = 0;
            char codec[16] = "";
            sscanf(out, "%15[^,],%lu", codec, &fr);
            CHECK(!strcmp(codec, "h264") && fr == frames, "ffprobe %s: %s (want %llu frames)", file, out,
                  (unsigned long long)frames);
            probe(file, "format=duration", out, sizeof out);
            double d = atof(out);
            double want = mode ? (60 - 30.5) + 10 * 60 + 20 : 22 * 3600 + 20 - 30.5;
            /* ffprobe counts from the keyframe before the start (the edit
             * list starts the picture later) to the last fragment's end */
            CHECK(d >= want - 1 && d <= want + 8, "ffprobe %s: duration %.3f, the range %.1f", file, d, want);
        }
        mp4_buf_free(&init);
        rec_plan_free(&pl);
    }
    /* the MSE pieces: four fragments from a time, then what follows */
    rec_plan_t a, b2;
    rec_q_plan(&p, 1, 0, day0 + 10000000, UINT64_MAX, 4, 0, &a);
    rec_q_plan(&p, 1, 0, a.next_us, UINT64_MAX, 4, 1, &b2);
    CHECK(a.n == 4 && a.next_us && b2.n == 4 &&
          b2.p[0].chunk_start_us + b2.p[0].t_ms * 1000ull == a.next_us, "fragments: the next request follows on");
    rec_plan_free(&a);
    rec_plan_free(&b2);
    rec_pool_unmount(&p);
    rec_pool_free(&p);
    rm_rf(dir);
}

/* ---- B-frames ----------------------------------------------------------------------- */

static void test_bframes(void)
{
    char dir[600];
    snprintf(dir, sizeof dir, "%s/bframes", g_work);
    rm_rf(dir);
    rec_pool_t p;
    rec_pool_init(&p);
    rec_pool_cfg_t cfg = pool_cfg(dir, 1, 4);
    mount_full(&p, &cfg);
    /* I P B B P B B ... in decode order: pts 0 3 1 2 6 4 5 ... frames */
    const int order[] = { 0, 3, 1, 2 };
    for (int i = 0; i < 200; ++i) {
        int g = i / 4, k = i % 4;
        uint64_t pts = T0 + (uint64_t)(g * 4 + order[k]) * FRAME;
        int key = i % 48 == 0;
        vg_au_t au = { .codec = VG_CODEC_H264, .data = g_h264.au[key ? 0 : 1 + i % 20].p,
                       .len = g_h264.au[key ? 0 : 1 + i % 20].len, .pts_us = pts, .key = key };
        rec_au_t *a = rec_au_from(0, 0, &au, T0 + (uint64_t)i * FRAME, 0);
        rec_pool_put(&p, a);
        free(a);
    }
    rec_pool_unmount(&p);
    mount_full(&p, &cfg);
    rec_chunk_t c;
    rec_frag_t *fr;
    int n = rec_pool_frags(&p, 0, &c, &fr);
    CHECK(n >= 3, "B-frames: %d fragments", n);
    char path[700];
    rec_pool_chunk_path(&p, 0, path, sizeof path);
    int fd = open(path, O_RDONLY);
    uint8_t mb[8192];
    mp4_moof_t m;
    int ok = n > 0 && pread(fd, mb, sizeof mb, fr[1].off) > 0 && mp4_moof_parse(mb, rec_be32(mb), &m) == 0;
    int neg = 0, dur_ok = 1;
    for (uint32_t i = 0; ok && i < m.n; ++i) {
        mp4_sample_t s;
        mp4_moof_sample(mb, &m, i, &s);
        neg += s.cts < 0;
        dur_ok &= s.dur == 3600;
        /* presentation = decode + offset: back to the frames' own order */
    }
    CHECK(ok && neg > 0 && dur_ok && m.dur == m.n * 3600ull, "B-frames: composition offsets, 40 ms decode steps "
          "(%d fragments, %u samples, %d negative, durations %s, %llu)", n, ok ? m.n : 0, neg, dur_ok ? "ok" : "not 3600",
          ok ? (unsigned long long)m.dur : 0ull);
    close(fd);
    free(fr);
    rec_pool_unmount(&p);
    rec_pool_free(&p);
    rm_rf(dir);
}

/* ---- queries on an 8 TB table ----------------------------------------------------------- */

static void test_query_speed(void)
{
    rec_pool_t p;
    rec_pool_init(&p);
    p.mounted = 1;
    p.state = RP_READY;
    p.n = p.cap = 32000;
    p.tab = calloc(p.n, sizeof *p.tab);
    p.known = malloc(p.n);
    memset(p.known, 1, p.n);
    p.chunk_bytes = 256u << 20;
    /* 16 cameras, 256 MiB chunks of ~8.5 minutes each, back to back: 2000 chunks each, ~12 days */
    for (uint32_t i = 0; i < p.n; ++i) {
        int cam = (int)(i % 16), k = (int)(i / 16);
        rec_chunk_t *c = &p.tab[i];
        c->state = RC_CLOSED;
        c->cam = (uint8_t)(cam + 1);
        c->use = 1;
        c->nfrag = 128;
        c->start_us = T0 + (uint64_t)k * 512000000ull;
        c->end_us = c->start_us + 512000000ull;
        c->no = i;
    }
    time_t day = (time_t)(T0 / 1000000) + 5 * 86400;
    struct tm tm;
    localtime_r(&day, &tm);
    struct timespec t0;
    uint32_t mask = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int r = 0; r < 10; ++r) rec_q_days(&p, 7, 0, tm.tm_year + 1900, tm.tm_mon + 1, &mask);
    double days_ms = ms_since(&t0) / 10;
    rec_span_t *sp = NULL;
    int n = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int r = 0; r < 10; ++r) {
        free(sp);
        n = rec_q_spans(&p, 7, 0, (uint64_t)day * 1000000, (uint64_t)(day + 86400) * 1000000, &sp);
    }
    double spans_ms = ms_since(&t0) / 10;
    free(sp);
    printf("queries on 32000 chunks: days %.3f ms, a day's spans %.3f ms\n", days_ms, spans_ms);
    CHECK(mask && n == 1 && days_ms < 10 && spans_ms < 10, "queries under 10 ms: days %.3f, spans %.3f (%d)",
          days_ms, spans_ms, n);
    rec_store_status_t st;
    (void)st;
    p.mounted = 0;
    free(p.tab);
    free(p.known);
    p.tab = NULL;
    p.known = NULL;
    rec_pool_free(&p);
}

/* The writer's pace: 16 cameras at 4 Mbit/s and 25 fps (the clip's AUs
 * with filler data to ~20 KiB), as fast as it goes, ticking (flushes,
 * saves) on the recording's clock. A rough proxy for the board: run it
 * there (test_rec DATA DIR --bench SECS) with the disk in DIR. */
static int bench(const char *dir, int secs)
{
    rec_pool_t p;
    rec_pool_init(&p);
    rec_pool_cfg_t cfg = pool_cfg(dir, 64, 1024);
    if (mount_full(&p, &cfg) < 0) {
        fprintf(stderr, "bench: %s\n", p.why);
        return 1;
    }
    static uint8_t buf[64 << 10];
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    clock_t c0 = clock();
    uint64_t bytes = 0, t = T0;
    for (int f = 0; f < secs * 25; ++f, t += FRAME) {
        for (int cam = 0; cam < 16; ++cam) {
            const clip_t *c = cam % 2 ? &g_h265 : &g_h264;
            int i = f % c->n;
            size_t n = c->au[i].len;
            memcpy(buf, c->au[i].p, n);
            /* filler: H.264 type 12, H.265 type 38 */
            size_t fill = n < 20000 ? 20000 - n : 0;
            buf[n] = 0; buf[n + 1] = 0; buf[n + 2] = 1;
            if (c->codec == VG_CODEC_H264) {
                buf[n + 3] = 12;
                memset(buf + n + 4, 0xFF, fill);
                n += 4 + fill;
            } else {
                buf[n + 3] = 38 << 1;
                buf[n + 4] = 1;
                memset(buf + n + 5, 0xFF, fill);
                n += 5 + fill;
            }
            buf[n++] = 0x80;
            vg_au_t au = { .codec = c->codec, .data = buf, .len = n, .pts_us = t, .key = c->au[i].key };
            rec_au_t *a = rec_au_from(cam, 0, &au, t, 0);
            if (a) {
                rec_pool_put(&p, a);
                bytes += a->len;
                free(a);
            }
        }
        if (f % 25 == 24) rec_pool_tick(&p, (time_t)(t / 1000000), t);
    }
    rec_pool_unmount(&p);
    double wall = ms_since(&t0) / 1000, cpu = (double)(clock() - c0) / CLOCKS_PER_SEC;
    printf("bench: 16 streams, %d s of video, %.1f MiB in %.2f s (%.1f MiB/s, %.1fx real time), CPU %.2f s "
           "(%.1f %% of one core at real time)\n", secs, bytes / 1048576.0, wall, bytes / 1048576.0 / wall,
           secs / wall, cpu, cpu * 100 / secs);
    rec_pool_free(&p);
    return 0;
}

int main(int argc, char **argv)
{
    g_data = argc > 1 ? argv[1] : "tests/data";
    snprintf(g_work, sizeof g_work, "%s", argc > 2 ? argv[2] : "/tmp/vigil-test-rec");
    int all = getenv("REC_CUT_ALL") != NULL;
    mkdir(g_work, 0755);
    clip_load(&g_h264, "testsrc_640x360.h264", VG_CODEC_H264);
    clip_load(&g_h265, "testsrc_640x360.h265", VG_CODEC_H265);
    CHECK(g_h264.n >= 100 && g_h264.au[0].key && g_h265.n >= 100, "clips: %d and %d AUs", g_h264.n, g_h265.n);
    if (argc > 4 && !strcmp(argv[3], "--bench")) return bench(g_work, atoi(argv[4]));
    test_records();
    test_mp4();
    test_pool_basic();
    test_ring();
    test_rebuild();
    test_bframes();
    test_export_day();
    test_query_speed();
    test_power_cut(all);
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
