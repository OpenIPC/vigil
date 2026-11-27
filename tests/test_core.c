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
#include <math.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <pthread.h>

#include "aspect.h"
#include "au_norm.h"
#include "gop_cache.h"
#include "channel.h"
#include "cJSON.h"
#include "config.h"
#include "events.h"
#include "hal.h"
#include "hal_capture.h"
#include "hal_decoder.h"
#include "hal_display.h"
#include "disc_proto.h"
#include "net.h"
#include "netip_proto.h"
#include "overscan.h"
#include "picture.h"
#include "qos.h"
#include "rec_place.h"
#include "rec_sched.h"
#include "rtp_depay.h"
#include "source.h"
#include "rtsp_client.h"
#include "schema.h"
#include "schema_yaml.h"
#include "seg_writer.h"
#include "sps.h"
#include "stream_tap.h"

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
    static const struct { const char *file; vg_codec_t c; int sps; unsigned w, h, refs, sar_w, sar_h; } T[] = {
        /* x264 and x265 say 1:1 */
        { "testsrc_640x360.h264", VG_CODEC_H264, 7, 640, 360, 2, 1, 1 },
        /* x265 ref=1 still signals a DPB of 3 (max_dec_pic_buffering_minus1 = 2) */
        { "testsrc_640x360.h265", VG_CODEC_H265, 33, 640, 360, 2, 1, 1 },
        { "majestic_1080p.h265", VG_CODEC_H265, 33, 1920, 1080, 1, 0, 0 },   /* no VUI SAR */
        /* anamorphic D1 and CIF: a 16:9 D1 (aspect_ratio_idc 4), a 4:3 CIF
         * in H.265 (idc 2) after scaling lists and predicted short-term
         * reference picture sets */
        { "testsrc_704x576_sar16x11.h264", VG_CODEC_H264, 7, 704, 576, 1, 16, 11 },
        { "testsrc_352x288_sar12x11.h265", VG_CODEC_H265, 33, 352, 288, 3, 12, 11 },
        /* square pixels, as many D1 cameras claim */
        { "testsrc_704x576.h264", VG_CODEC_H264, 7, 704, 576, 1, 1, 1 },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; ++i) {
        size_t len, nl;
        uint8_t *b = load(T[i].file, &len);
        const uint8_t *sps = find_nal(b, len, T[i].c, T[i].sps, &nl);
        vg_stream_info_t si;
        CHECK(sps && sps_parse(T[i].c, sps, nl, &si) == 0, "%s: no SPS", T[i].file);
        CHECK(si.width == T[i].w && si.height == T[i].h && si.refs == T[i].refs,
              "%s: %ux%u refs %u", T[i].file, si.width, si.height, si.refs);
        CHECK(si.sar_w == T[i].sar_w && si.sar_h == T[i].sar_h, "%s: SAR %u:%u", T[i].file, si.sar_w, si.sar_h);
        /* a VUI cut short leaves the SAR unknown, never the size */
        vg_stream_info_t cut;
        CHECK(sps_parse(T[i].c, sps, nl - 2 > 12 ? 12 : nl - 2, &cut) < 0 ||
              (cut.width == T[i].w && cut.height == T[i].h), "%s: cut short", T[i].file);
        free(b);
    }
}

/* ---- the picture's shape and place (aspect.c) ------------------------------------ */

static int rect_eq(vg_rect_t a, vg_rect_t b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

static vg_stream_info_t SI(unsigned w, unsigned h, unsigned sw, unsigned sh)
{
    return (vg_stream_info_t){ .width = w, .height = h, .refs = 1, .sar_w = sw, .sar_h = sh };
}

static int ratio_is(vg_ratio_t r, unsigned w, unsigned h) { return r.w == w && r.h == h; }

static void test_aspect_ratio(void)
{
    static const struct { unsigned w, h, sw, sh, aw, ah; } T[] = {
        { 2592, 1944, 0, 0, 4, 3 },     /* 5 MP */
        { 2560, 1440, 0, 0, 16, 9 },    /* 4 MP */
        { 1920, 1080, 0, 0, 16, 9 },
        { 1920, 1088, 0, 0, 16, 9 },    /* the crop left out: within 2 % */
        { 800, 448, 0, 0, 16, 9 },      /* 450 rounded to 16 */
        { 640, 360, 1, 1, 16, 9 },
        { 2048, 1536, 0, 0, 4, 3 },
        { 1280, 1024, 0, 0, 5, 4 },
        { 3840, 1080, 0, 0, 32, 9 },
        { 704, 576, 0, 0, 11, 9 },      /* not 5:4: 2.2 % off */
        { 704, 480, 0, 0, 22, 15 },     /* not 3:2 either */
        { 352, 288, 0, 0, 11, 9 },
        { 720, 576, 0, 0, 5, 4 },
        { 704, 576, 16, 11, 16, 9 },    /* PAL 16:9 */
        { 704, 576, 12, 11, 4, 3 },     /* PAL 4:3 */
        { 352, 288, 12, 11, 4, 3 },
        { 720, 576, 64, 45, 16, 9 },
        { 704, 480, 10, 11, 4, 3 },     /* NTSC 4:3 */
        { 704, 480, 40, 33, 16, 9 },    /* NTSC 16:9 */
        { 1000, 300, 0, 0, 10, 3 },
        { 0, 0, 0, 0, 0, 0 },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; ++i) {
        vg_stream_info_t si = SI(T[i].w, T[i].h, T[i].sw, T[i].sh);
        vg_ratio_t r = aspect_of_stream(&si);
        CHECK(ratio_is(r, T[i].aw, T[i].ah), "%ux%u SAR %u:%u: %u:%u, not %u:%u", T[i].w, T[i].h, T[i].sw, T[i].sh,
              r.w, r.h, T[i].aw, T[i].ah);
    }

    /* A sub-stream at its main stream's shape, whatever its own. */
    static const struct { unsigned mw, mh, sw, sh, aw, ah; } C[] = {
        { 2592, 1944, 704, 576, 4, 3 },
        { 2592, 1944, 352, 288, 4, 3 },
        { 2560, 1440, 704, 576, 16, 9 },
        { 2560, 1440, 800, 448, 16, 9 },
        { 1920, 1080, 704, 576, 16, 9 },
        { 1920, 1080, 800, 448, 16, 9 },
        { 1920, 1080, 640, 480, 16, 9 },
        { 2592, 1944, 800, 448, 4, 3 },
    };
    for (size_t i = 0; i < sizeof C / sizeof C[0]; ++i) {
        vg_stream_info_t m = SI(C[i].mw, C[i].mh, 0, 0), sub = SI(C[i].sw, C[i].sh, 0, 0);
        vg_ratio_t r = aspect_of_camera(&sub, 1, &m), rm = aspect_of_camera(&m, 0, &m);
        CHECK(ratio_is(r, C[i].aw, C[i].ah) && ratio_is(rm, C[i].aw, C[i].ah),
              "sub %ux%u of %ux%u: %u:%u, main %u:%u", C[i].sw, C[i].sh, C[i].mw, C[i].mh, r.w, r.h, rm.w, rm.h);
    }
    vg_stream_info_t d1 = SI(704, 576, 0, 0), d1sar = SI(704, 576, 16, 11), fhd = SI(1920, 1080, 0, 0);
    CHECK(ratio_is(aspect_of_camera(&d1, 1, NULL), 11, 9), "main not known yet: the sub's own size");
    CHECK(ratio_is(aspect_of_camera(&d1sar, 1, NULL), 16, 9), "main not known yet: the sub's SAR");
    CHECK(ratio_is(aspect_of_camera(&d1, 0, &fhd), 11, 9), "a main stream keeps its own shape");
}

/* The UI's cells for an n x n layout on a w x h screen: edges at i * w / n. */
static vg_rect_t cell(int n, int i, unsigned w, unsigned h)
{
    int c = i % n, r = i / n;
    int x0 = (int)(c * w / n), x1 = (int)((c + 1) * w / n), y0 = (int)(r * h / n), y1 = (int)((r + 1) * h / n);
    return (vg_rect_t){ x0, y0, x1 - x0, y1 - y0 };
}

static void test_aspect_fit(void)
{
    /* Known places: full screen and 2x2 on each output. */
    static const struct { unsigned ow, oh; vg_rect_t tile; unsigned aw, ah; vg_rect_t want; } K[] = {
        { 1280, 720, { 0, 0, 1280, 720 }, 4, 3, { 160, 0, 960, 720 } },
        { 1280, 720, { 0, 0, 1280, 720 }, 16, 9, { 0, 0, 1280, 720 } },
        { 1920, 1080, { 0, 0, 1280, 720 }, 4, 3, { 240, 0, 1440, 1080 } },
        { 1280, 1024, { 0, 0, 1280, 720 }, 16, 9, { 0, 152, 1280, 720 } },
        { 1280, 1024, { 0, 0, 1280, 720 }, 4, 3, { 0, 32, 1280, 960 } },
        { 1440, 900, { 0, 0, 1280, 720 }, 16, 9, { 0, 44, 1440, 810 } },
        { 1440, 900, { 0, 0, 1280, 720 }, 4, 3, { 120, 0, 1200, 900 } },
        { 1280, 720, { 640, 360, 640, 360 }, 4, 3, { 720, 360, 480, 360 } },
        { 1920, 1080, { 640, 360, 640, 360 }, 4, 3, { 1080, 540, 720, 540 } },
        { 1280, 1024, { 640, 360, 640, 360 }, 16, 9, { 640, 588, 640, 360 } },
        { 1280, 720, { 0, 0, 1280, 720 }, 0, 0, { 0, 0, 1280, 720 } },
        { 1280, 1024, { 0, 0, 1280, 720 }, 0, 0, { 0, 0, 1280, 1024 } },
    };
    for (size_t i = 0; i < sizeof K / sizeof K[0]; ++i) {
        vg_rect_t t = aspect_map(&K[i].tile, 1280, 720, K[i].ow, K[i].oh, 2);
        vg_rect_t r = aspect_fit(&t, (vg_ratio_t){ K[i].aw, K[i].ah }, 2);
        CHECK(rect_eq(r, K[i].want), "%u:%u in %dx%d at %ux%u: %d,%d %dx%d", K[i].aw, K[i].ah, K[i].tile.w,
              K[i].tile.h, K[i].ow, K[i].oh, r.x, r.y, r.w, r.h);
    }

    /* Every camera size, every layout, every output, fit and stretch: the
     * picture inside its tile, centred, filling it one way, at its shape
     * to a pixel; stretched, the whole tile; neighbouring tiles meet. */
    static const unsigned sizes[][2] = { { 2592, 1944 }, { 2560, 1440 }, { 1920, 1080 }, { 800, 448 }, { 704, 576 } };
    static const unsigned outs[][2] = { { 1280, 720 }, { 1920, 1080 }, { 1280, 1024 }, { 1440, 900 } };
    /* the 704x576 sub-stream of a 5 MP camera and of a 16:9 one */
    static const unsigned mains[][2] = { { 0, 0 }, { 2592, 1944 }, { 1920, 1080 } };
    int bad = 0, cases = 0;
    for (size_t o = 0; o < 4; ++o)
        for (int n = 1; n <= 4; ++n)
            for (int i = 0; i < n * n; ++i) {
                vg_rect_t ui = cell(n, i, 1280, 720), t = aspect_map(&ui, 1280, 720, outs[o][0], outs[o][1], 2);
                if (i % n) {
                    vg_rect_t lc = cell(n, i - 1, 1280, 720);
                    vg_rect_t left = aspect_map(&lc, 1280, 720, outs[o][0], outs[o][1], 2);
                    bad += left.x + left.w != t.x;
                }
                for (size_t z = 0; z < 5; ++z)
                    for (size_t m = 0; m < 3; ++m)
                        for (int stretch = 0; stretch < 2; ++stretch) {
                            if (m && sizes[z][0] != 704) continue;
                            vg_stream_info_t si = SI(sizes[z][0], sizes[z][1], 0, 0);
                            vg_stream_info_t mi = SI(mains[m][0], mains[m][1], 0, 0);
                            vg_ratio_t a = stretch ? (vg_ratio_t){ 0, 0 } : aspect_of_camera(&si, m != 0, m ? &mi : NULL);
                            vg_rect_t r = aspect_fit(&t, a, 2);
                            ++cases;
                            int in = r.x >= t.x && r.y >= t.y && r.x + r.w <= t.x + t.w && r.y + r.h <= t.y + t.h &&
                                     r.w > 0 && r.h > 0 && !(r.x & 1) && !(r.y & 1) && !(r.w & 1) && !(r.h & 1);
                            int lm = r.x - t.x, rm = t.x + t.w - r.x - r.w, tm = r.y - t.y, bm = t.y + t.h - r.y - r.h;
                            int centred = abs(lm - rm) <= 2 && abs(tm - bm) <= 2;
                            int full = stretch ? r.w >= t.w - 1 && r.h >= t.h - 1 : r.w >= t.w - 1 || r.h >= t.h - 1;
                            /* the shape: w:h against a, to within the rounding of one side to 2 px */
                            long long d = stretch ? 0 : (long long)r.w * a.h - (long long)r.h * a.w;
                            int shape = stretch || llabs(d) <= (long long)(a.w > a.h ? a.w : a.h) * 2;
                            if (!(in && centred && full && shape)) {
                                if (bad < 10)
                                    fprintf(stderr, "  %ux%u%s %dx%d #%d out %ux%u %s: %d,%d %dx%d in %d,%d %dx%d\n",
                                            sizes[z][0], sizes[z][1], m ? " (sub)" : "", n, n, i, outs[o][0],
                                            outs[o][1], stretch ? "stretch" : "fit", r.x, r.y, r.w, r.h, t.x, t.y,
                                            t.w, t.h);
                                ++bad;
                            }
                        }
            }
    CHECK(bad == 0, "%d of %d placements wrong", bad, cases);
}

/* The output's margins: the inner rectangle, and the screen placed in it. */
static void test_overscan(void)
{
    vg_margins_t none = { 0, 0, 0, 0 }, five = { 5, 5, 5, 5 }, odd = { 3, 1, 7, 2 }, big = { 40, 0, 0, 0 };
    CHECK(rect_eq(overscan_inner(1920, 1080, &none, 2), (vg_rect_t){ 0, 0, 1920, 1080 }) && overscan_none(&none) &&
          overscan_none(NULL), "no margins: the whole output");
    CHECK(rect_eq(overscan_inner(1920, 1080, &five, 2), (vg_rect_t){ 96, 54, 1728, 972 }), "5 %% at 1080p");
    CHECK(rect_eq(overscan_inner(1280, 720, &five, 2), (vg_rect_t){ 64, 36, 1152, 648 }), "5 %% at 720p");
    /* 3 % of 720 = 21.6 -> 20 (even); 1 % = 7.2 -> 6; 7 % of 1280 = 89.6 -> 88; 2 % = 25.6 -> 24 */
    CHECK(rect_eq(overscan_inner(1280, 720, &odd, 2), (vg_rect_t){ 88, 20, 1280 - 88 - 24, 720 - 20 - 6 }),
          "each edge on its own, even");
    CHECK(rect_eq(overscan_inner(1280, 720, &big, 2), (vg_rect_t){ 0, 72, 1280, 648 }), "at most 10 %%");
    vg_rect_t in = overscan_inner(1920, 1080, &five, 2);
    /* the whole screen fills the inner rectangle; a 2x2 grid's cells still meet */
    CHECK(rect_eq(overscan_map(&(vg_rect_t){ 0, 0, 1280, 720 }, 1280, 720, &in, 2), in), "full screen: the inner rect");
    vg_rect_t a = overscan_map(&(vg_rect_t){ 0, 0, 640, 360 }, 1280, 720, &in, 2);
    vg_rect_t b = overscan_map(&(vg_rect_t){ 640, 360, 640, 360 }, 1280, 720, &in, 2);
    CHECK(a.x == 96 && a.y == 54 && a.x + a.w == b.x && a.y + a.h == b.y && b.x + b.w == 96 + 1728 &&
          b.y + b.h == 54 + 972 && !(a.w & 1) && !(b.x & 1), "2x2: cells meet, inside, even (%d,%d %dx%d)", b.x,
          b.y, b.w, b.h);
    int x, y;
    overscan_point(0, 0, 1280, 720, &in, &x, &y);
    CHECK(x == 96 && y == 54, "the pointer's top left: the inner rect's");
    overscan_point(1279, 719, 1280, 720, &in, &x, &y);
    CHECK(x < 96 + 1728 && y < 54 + 972 && x >= 96 + 1728 - 2 && y >= 54 + 972 - 2,
          "its bottom right: inside (%d, %d)", x, y);
    overscan_point(640, 360, 1280, 720, &in, &x, &y);
    CHECK(x == 96 + 864 && y == 54 + 486, "the centre");
    /* back to the screen: a cell, and something partly outside */
    vg_rect_t u = overscan_unmap(&b, &in, 1280, 720);
    CHECK(u.x >= 638 && u.x <= 640 && u.x + u.w == 1280 && u.y + u.h == 720, "unmap a cell (%d,%d %dx%d)", u.x,
          u.y, u.w, u.h);
    u = overscan_unmap(&(vg_rect_t){ 0, 0, 1920, 1080 }, &in, 1280, 720);
    CHECK(rect_eq(u, (vg_rect_t){ 0, 0, 1280, 720 }), "the margins cut off");
    vg_rect_t full = overscan_inner(1920, 1080, &none, 2);
    u = overscan_unmap(&(vg_rect_t){ 960, 540, 960, 540 }, &full, 1280, 720);
    CHECK(rect_eq(u, (vg_rect_t){ 640, 360, 640, 360 }), "no margins: plain scaling");
}

static void test_picture(void)
{
    vg_picture_t p;
    CHECK(picture_preset("standard", &p) == 0 && p.luma == 50 && p.contrast == 50 && p.hue == 50 &&
          p.saturation == 50 && p.gamma == 100, "standard: unchanged");
    CHECK(picture_preset("bright", &p) == 0 && p.luma > 50 && p.gamma < 100, "bright: luma up, gamma lifted");
    CHECK(picture_preset("soft", &p) == 0 && p.contrast < 50 && p.sharpness < 50, "soft: less contrast, sharpness");
    CHECK(picture_preset("vivid", &p) == 0 && p.saturation > 50 && p.contrast > 50, "vivid: colour and contrast");
    CHECK(picture_preset("cinema", &p) < 0 && p.gamma == 100 && p.luma == 50, "unknown: standard");
    unsigned char g[33];
    picture_gamma_curve(100, 33, g);
    int line = 1;
    for (int i = 0; i < 32; ++i) line &= g[i] == i * 8;
    CHECK(line && g[32] == 255, "gamma 1.0: the display's straight line (0, 8, ... 248, 255)");
    unsigned char lo[33], hi[33];
    picture_gamma_curve(80, 33, lo);
    picture_gamma_curve(110, 33, hi);
    int ok = lo[0] == 0 && hi[0] == 0 && lo[32] == 255 && hi[32] == 255;
    for (int i = 1; i < 32; ++i) ok &= lo[i] > g[i] && hi[i] < g[i] && lo[i] >= lo[i - 1] && hi[i] >= hi[i - 1];
    CHECK(ok, "0.8 above the line, 1.1 below, both rising, black and white kept (mid %u %u %u)", lo[16], g[16],
          hi[16]);
}

static void write_file(const char *path, const char *text);

static void test_output_adj_config(void)
{
    vigil_config_t c;
    config_defaults(&c);
    CHECK(!c.display.margin_top && !c.display.margin_bottom && !c.display.margin_left && !c.display.margin_right &&
          !strcmp(c.display.picture, "standard") && c.display.hdmi_volume == 80 && !c.display.hdmi_mute,
          "defaults: no margins, standard, volume 80, not muted");
    char dir[] = "/tmp/vigil-adj-XXXXXX", path[128];
    if (!mkdtemp(dir)) return;
    snprintf(path, sizeof path, "%s/vigil.yaml", dir);
    schema_errors_t e = { 0 };
    write_file(path, "version: 1\ndisplay:\n  margin_top: 3\n  margin_right: 10\n  picture: vivid\n"
                     "  hdmi_volume: 40\n  hdmi_mute: true\n");
    CHECK(config_load_file(path, &c, &e) == 0 && c.display.margin_top == 3 && c.display.margin_right == 10 &&
          !c.display.margin_left && !strcmp(c.display.picture, "vivid") && c.display.hdmi_volume == 40 &&
          c.display.hdmi_mute, "loaded: %s", e.n ? e.e[0].msg : "");
    memset(&e, 0, sizeof e);
    write_file(path, "version: 1\ndisplay:\n  margin_left: 11\n");
    CHECK(config_load_file(path, &c, &e) < 0 && e.n && strstr(e.e[0].path, "margin_left"), "11 %%: refused");
    memset(&e, 0, sizeof e);
    write_file(path, "version: 1\ndisplay:\n  picture: cinema\n");
    CHECK(config_load_file(path, &c, &e) < 0 && e.n && strstr(e.e[0].path, "picture"), "an unknown preset: refused");
    memset(&e, 0, sizeof e);
    write_file(path, "version: 1\ndisplay:\n  hdmi_volume: 101\n");
    CHECK(config_load_file(path, &c, &e) < 0, "volume 101: refused");
    unlink(path);
    rmdir(dir);
}

static void test_aspect_scaled(void)
{
    /* The SigmaStar decoder: shrinks only, to multiples of 8, at most 8x. */
    static const struct { unsigned sw, sh, ww, wh, ow, oh; } T[] = {
        { 2592, 1944, 960, 720, 960, 720 },     /* full screen at 720p */
        { 2592, 1944, 1440, 1080, 1440, 1080 }, /* ... at 1080p */
        { 2592, 1944, 480, 360, 480, 360 },     /* 2x2 */
        { 2592, 1944, 240, 180, 328, 248 },     /* 4x4: 8x is the most, the display crops */
        { 2560, 1440, 320, 180, 320, 184 },     /* 1440 / 8 = 180, a multiple of 8 up: 184 */
        { 1920, 1080, 960, 540, 960, 536 },     /* rounded down to 8: the display enlarges 4 lines */
        { 1920, 1080, 1920, 1080, 1920, 1080 }, /* as decoded: no scaler */
        { 704, 576, 640, 360, 640, 360 },       /* anamorphic, at 16:9 */
        { 704, 576, 480, 360, 480, 360 },       /* ... at 4:3 */
        { 704, 576, 1280, 720, 704, 576 },      /* larger: the display enlarges */
        { 704, 576, 960, 540, 704, 536 },       /* one way each */
        { 800, 448, 426, 240, 424, 240 },
        { 800, 448, 1440, 810, 800, 448 },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; ++i) {
        unsigned ow, oh;
        aspect_scaled_size(T[i].sw, T[i].sh, T[i].ww, T[i].wh, 8, 8, &ow, &oh);
        CHECK(ow == T[i].ow && oh == T[i].oh, "%ux%u for %ux%u: %ux%u, not %ux%u", T[i].sw, T[i].sh, T[i].ww, T[i].wh,
              ow, oh, T[i].ow, T[i].oh);
    }
}

static void test_aspect_fill(void)
{
    /* The picture over the whole tile: the rectangle it takes. */
    static const struct { vg_rect_t tile; unsigned aw, ah; vg_rect_t want; } V[] = {
        { { 0, 0, 1280, 720 }, 4, 3, { 0, -120, 1280, 960 } },       /* 4:3 in 16:9: over top and bottom */
        { { 0, 0, 1280, 720 }, 32, 9, { -640, 0, 2560, 720 } },      /* wider: over the sides */
        { { 640, 360, 640, 360 }, 16, 9, { 640, 360, 640, 360 } },   /* the same shape: the tile */
        { { 640, 360, 640, 360 }, 4, 3, { 640, 300, 640, 480 } },
        { { 0, 0, 1280, 720 }, 0, 0, { 0, 0, 1280, 720 } },
    };
    for (size_t i = 0; i < sizeof V / sizeof V[0]; ++i) {
        vg_rect_t r = aspect_cover(&V[i].tile, (vg_ratio_t){ V[i].aw, V[i].ah });
        CHECK(rect_eq(r, V[i].want), "cover %u:%u over %dx%d: %d,%d %dx%d", V[i].aw, V[i].ah, V[i].tile.w,
              V[i].tile.h, r.x, r.y, r.w, r.h);
    }

    /* The part of the picture that shows, in its pixels. */
    static const struct { unsigned sw, sh, aw, ah, tw, th, align; vg_rect_t want; } K[] = {
        { 1920, 1440, 4, 3, 1920, 1080, 2, { 0, 180, 1920, 1080 } },  /* 4:3 full screen at 1080p */
        { 704, 576, 4, 3, 640, 360, 2, { 0, 72, 704, 432 } },         /* anamorphic 4:3 D1 in a 16:9 tile */
        { 704, 576, 16, 9, 480, 360, 2, { 88, 0, 528, 576 } },        /* ... 16:9 D1 in a 4:3 tile */
        { 704, 576, 16, 9, 640, 360, 2, { 0, 0, 704, 576 } },         /* the tile's shape: all of it */
        { 2592, 1944, 0, 0, 1280, 720, 2, { 0, 242, 2592, 1458 } },   /* 0:0: the size's own shape */
        { 960, 720, 4, 3, 1280, 720, 8, { 0, 88, 960, 544 } },        /* to multiples of 8 */
        { 1920, 1080, 16, 9, 0, 0, 2, { 0, 0, 1920, 1080 } },         /* no tile: all of it */
    };
    for (size_t i = 0; i < sizeof K / sizeof K[0]; ++i) {
        vg_rect_t r = aspect_crop(K[i].sw, K[i].sh, (vg_ratio_t){ K[i].aw, K[i].ah }, K[i].tw, K[i].th, K[i].align);
        CHECK(rect_eq(r, K[i].want), "crop %ux%u %u:%u for %ux%u: %d,%d %dx%d", K[i].sw, K[i].sh, K[i].aw, K[i].ah,
              K[i].tw, K[i].th, r.x, r.y, r.w, r.h);
    }

    /* Every camera size and shape, every layout, every output: inside the
     * picture, even, centred, all of it one way, and shown at the camera's
     * shape it is the tile's shape, to within the rounding to 2 px. */
    static const unsigned sizes[][2] = { { 2592, 1944 }, { 2560, 1440 }, { 1920, 1080 }, { 800, 448 }, { 704, 576 } };
    static const vg_ratio_t shapes[] = { { 0, 0 }, { 4, 3 }, { 16, 9 }, { 32, 9 }, { 1, 1 } };
    static const unsigned outs[][2] = { { 1280, 720 }, { 1920, 1080 }, { 1280, 1024 }, { 1440, 900 } };
    int bad = 0, cases = 0;
    for (size_t o = 0; o < 4; ++o)
        for (int n = 1; n <= 4; ++n)
            for (int i = 0; i < n * n; ++i) {
                vg_rect_t ui = cell(n, i, 1280, 720), t = aspect_map(&ui, 1280, 720, outs[o][0], outs[o][1], 2);
                for (size_t z = 0; z < 5; ++z)
                    for (size_t k = 0; k < 5; ++k) {
                        unsigned sw = sizes[z][0], sh = sizes[z][1];
                        vg_ratio_t a = shapes[k].w ? shapes[k] : (vg_ratio_t){ sw, sh };
                        vg_rect_t r = aspect_crop(sw, sh, shapes[k], (unsigned)t.w, (unsigned)t.h, 2);
                        ++cases;
                        int in = r.x >= 0 && r.y >= 0 && r.x + r.w <= (int)sw && r.y + r.h <= (int)sh && r.w > 0 &&
                                 r.h > 0 && !(r.x & 1) && !(r.y & 1) && !(r.w & 1) && !(r.h & 1);
                        int lm = r.x, rm = (int)sw - r.x - r.w, tm = r.y, bm = (int)sh - r.y - r.h;
                        int centred = abs(lm - rm) <= 2 && abs(tm - bm) <= 2;
                        int full = r.w == (int)sw || r.h == (int)sh;
                        /* shown at a: (r.w / sw * a.w) : (r.h / sh * a.h) against t.w : t.h */
                        double shown = (double)r.w * a.w * sh / ((double)r.h * a.h * sw), want = (double)t.w / t.h;
                        int cut = r.w < (int)sw ? r.w : r.h;
                        int shape = fabs(shown / want - 1) <= 2.0 / cut;
                        if (!(in && centred && full && shape)) {
                            if (bad < 10)
                                fprintf(stderr, "  %ux%u %u:%u %dx%d #%d out %ux%u: %d,%d %dx%d for %dx%d\n", sw,
                                        sh, a.w, a.h, n, n, i, outs[o][0], outs[o][1], r.x, r.y, r.w, r.h, t.w, t.h);
                            ++bad;
                        }
                    }
            }
    CHECK(bad == 0, "%d of %d crops wrong", bad, cases);
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

/* ---- GOP cache ------------------------------------------------------------- */

static vg_au_t gop_test_au(uint8_t *buf, size_t len, int n, int key)
{
    memset(buf, n & 0xff, len);
    return (vg_au_t){ .codec = VG_CODEC_H264, .data = buf, .len = len, .pts_us = 1000u * (unsigned)n, .key = key };
}

static void test_gop_cache(void)
{
    static uint8_t buf[200000];
    gop_cache_t g;
    gop_cache_init(&g, 1u << 20);
    CHECK(gop_cache_used() == 0, "nothing held at first");

    /* Nothing before a keyframe; then the keyframe and every AU after it,
     * in order, as they came. */
    vg_au_t au = gop_test_au(buf, 100, 1, 0);
    CHECK(gop_cache_add(&g, &au, 1) == GOP_IDLE && !gop_cache_ready(&g), "no keyframe yet: nothing kept");
    for (int i = 0; i < 30; ++i) {
        au = gop_test_au(buf, 1000 + (size_t)i * 100, i, i == 0);
        CHECK(gop_cache_add(&g, &au, 1) == GOP_KEPT, "AU %d kept", i);
    }
    CHECK(gop_cache_ready(&g) && g.n == 30, "a GOP of 30 (%u)", g.n);
    int order = 1;
    for (unsigned i = 0; i < g.n; ++i) {
        vg_au_t o;
        gop_cache_get(&g, i, &o);
        order &= o.len == 1000 + i * 100 && o.data[0] == i && o.data[o.len - 1] == i &&
                 o.pts_us == 1000u * i && o.key == (i == 0) && o.codec == VG_CODEC_H264;
    }
    CHECK(order, "replayed in order, keyframe first, data and times intact");
    size_t held = gop_cache_used();
    CHECK(held >= g.len && held <= 1u << 20, "held %zu for %zu bytes", held, g.len);

    /* The next keyframe starts over in the same memory. */
    au = gop_test_au(buf, 500, 40, 1);
    CHECK(gop_cache_add(&g, &au, 1) == GOP_KEPT && g.n == 1 && gop_cache_used() == held,
          "a keyframe starts over, no new memory (%u, %zu)", g.n, gop_cache_used());

    /* Over the cap: dropped, its memory given back, nothing kept until
     * the next keyframe. */
    int rc = GOP_KEPT, i;
    for (i = 0; i < 20 && rc == GOP_KEPT; ++i) {
        au = gop_test_au(buf, sizeof buf, i, 0);
        rc = gop_cache_add(&g, &au, 1);
    }
    CHECK(rc == GOP_OVER_CAP && i == 6 && !gop_cache_ready(&g) && gop_cache_used() == 0,
          "over 1 MiB: dropped after %d AUs, %zu held", i, gop_cache_used());
    au = gop_test_au(buf, 100, 1, 0);
    CHECK(gop_cache_add(&g, &au, 1) == GOP_IDLE && !gop_cache_ready(&g), "then nothing until a keyframe");
    au = gop_test_au(buf, 100, 2, 1);
    CHECK(gop_cache_add(&g, &au, 1) == GOP_KEPT && gop_cache_ready(&g), "a keyframe: kept again");

    /* A new SPS without a keyframe: not a GOP a decoder could start. */
    au = gop_test_au(buf, 100, 3, 0);
    CHECK(gop_cache_add(&g, &au, 2) == GOP_OVER_CAP && !gop_cache_ready(&g), "stream changed mid-GOP: dropped");

    /* The budget, shared: a second cache cannot take what the first holds. */
    gop_cache_t h;
    gop_cache_init(&h, 1u << 20);
    gop_cache_set_budget(300000);
    au = gop_test_au(buf, 150000, 1, 1);
    CHECK(gop_cache_add(&g, &au, 1) == GOP_KEPT, "first cache within the budget");
    au = gop_test_au(buf, 150000, 2, 1);
    CHECK(gop_cache_add(&h, &au, 1) == GOP_OVER_BUDGET && !gop_cache_ready(&h) &&
              gop_cache_used() <= 300000, "second over the budget: dropped (%zu used)", gop_cache_used());
    gop_cache_free(&g);
    CHECK(gop_cache_add(&h, &au, 1) == GOP_KEPT, "the first freed: the second fits");
    gop_cache_free(&h);
    gop_cache_set_budget(GOP_CACHE_BUDGET);
    CHECK(gop_cache_used() == 0, "all given back (%zu)", gop_cache_used());
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
/* How decoders start: the first AU after each open not a keyframe (never
 * right), and opens while the camera was well into a GOP (from a GOP
 * cache: at a keyframe the camera's count is 0). */
static int g_dec_first[CHANNEL_MAX], g_dec_bad_start[CHANNEL_MAX], g_dec_mid_gop[CHANNEL_MAX];
static atomic_int g_cam_since_key[2];       /* the fake camera's, [0] main, [1] extra */

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
    g_dec_first[ch] = ok;
    if (ok && atomic_load(&g_cam_since_key[si->width == 640]) > 3) ++g_dec_mid_gop[ch];
    pthread_mutex_unlock(&g_dec_lock);
    return ok ? 0 : -1;
}
int hal_dec_send(int ch, const vg_au_t *au)
{
    pthread_mutex_lock(&g_dec_lock);
    int ok = g_dec_open[ch];
    g_dec_sent[ch] += ok;
    if (ok && g_dec_first[ch]) g_dec_bad_start[ch] += !au->key;
    g_dec_first[ch] = 0;
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

static vg_ratio_t  g_aspect_at[CHANNEL_MAX]; /* hal_display_set_aspect, last */
static vg_aspect_t g_aspect_how[CHANNEL_MAX];
static int         g_aspect_sets[CHANNEL_MAX];
void hal_display_set_aspect(int ch, unsigned w, unsigned h, vg_aspect_t how)
{
    pthread_mutex_lock(&g_dec_lock);
    g_aspect_at[ch] = (vg_ratio_t){ w, h };
    g_aspect_how[ch] = how;
    ++g_aspect_sets[ch];
    pthread_mutex_unlock(&g_dec_lock);
}

static int aspect_is(int ch, unsigned w, unsigned h, vg_aspect_t how)
{
    pthread_mutex_lock(&g_dec_lock);
    int v = g_aspect_sets[ch] && g_aspect_at[ch].w == w && g_aspect_at[ch].h == h && g_aspect_how[ch] == how;
    pthread_mutex_unlock(&g_dec_lock);
    return v;
}

static int aspect_now(int ch, unsigned w, unsigned h) { return aspect_is(ch, w, h, VG_ASPECT_FIT); }

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
 * streams being sent and remembers the last channel asked for. It names
 * its channel `title` (ChannelTitle); `refuse[]` makes it refuse to claim
 * the main [0] or the sub-stream [1]; `wide` sends the main stream's
 * I-frame headers as an XM 4 MP camera does (2560x1440: codec byte 0x13). */
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
    const char    *title;
    int            refuse[2];
    int            wide;
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
    static const char no[] = "{\"Ret\":103,\"SessionID\":\"0x00000001\"}\n";
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
        if (msgid == 1048) {
            char t[160];
            int n = snprintf(t, sizeof t, "{\"Name\":\"ChannelTitle\",\"ChannelTitle\":[\"%s\"],\"Ret\":100}\n",
                             g_cam.title ? g_cam.title : "");
            cam_send(fd, 1049, t, (size_t)n + 1);
        }
        if (msgid == 1413 && g_cam.refuse[extra]) {
            cam_send(fd, 1414, no, sizeof no);
            goto out;
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
            if (g_cam.wide && !extra) {                 /* 2560/8 = 0x140, 1440/8 = 180 */
                ihead[4] |= 0x10;
                ihead[6] = 0x40;
                ihead[7] = 180;
            }
            static const uint8_t phead[4] = { 0, 0, 1, 0xFD };
            size_t hl = key ? 16 : 8;
            uint8_t *m = msg + 16 - hl;
            memcpy(m, key ? ihead : phead, hl - 4);
            put_le32(m + hl - 4, len);
            if (cam_send(fd, 1412, m, hl + len) < 0) goto out_msg;
            if (key) atomic_store(&g_cam_since_key[extra], 0);
            else atomic_fetch_add(&g_cam_since_key[extra], 1);
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

    /* Back to 2x2: the sub-stream's connection was never dropped. */
    channels_set_tiles(quad);
    WAIT_FOR(view_is(0, "sub", 640), 5000);
    CHECK(view_is(0, "sub", 640), "2x2 again: the sub-stream");
    CHECK(atomic_load(&g_cam.streaming[1]) == 1, "the sub-stream's connection was kept");

    /* Off screen: no decoder; both streams stay connected, the main one
     * recording. */
    channels_set_tiles(none);
    WAIT_FOR(!dec_count(g_dec_open, 0), 3000);
    usleep(300 * 1000);
    channels_status(0, &st);
    CHECK(!st.decoding && !dec_count(g_dec_open, 0) && !st.view[0], "hidden: not decoding ('%s')", st.view);
    CHECK(atomic_load(&g_cam.streaming[1]) == 1 && atomic_load(&g_cam.streaming[0]) == 1 &&
              st.sub_status == CH_LIVE && st.main_status == CH_LIVE,
          "hidden: both connections stay (main %d, extra %d)", atomic_load(&g_cam.streaming[0]),
          atomic_load(&g_cam.streaming[1]));
    channels_set_tiles(quad);
    WAIT_FOR(view_is(0, "sub", 640), 5000);
    CHECK(view_is(0, "sub", 640), "shown again: the sub-stream decodes");

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

/* Full screen and back, and off screen and back: each time the stream
 * wanted decodes from the GOP it kept, without waiting for the camera's
 * next keyframe, every decoder starting at a keyframe all the same. */
static void test_channel_instant_switch(void)
{
    int port = cam_start("majestic_1080p.h265", "testsrc_640x360.h264");
    if (!port) return;
    char url[64];
    snprintf(url, sizeof url, "netip://127.0.0.1:%d/?ch=0&stream=main", port);
    g_fake_caps.dec_shown_only = 1;
    g_dec_slots = 2;
    channels_set_screen(1280, 720);
    vg_rect_t quad[CHANNEL_MAX] = { { 0, 40, 640, 300 } };
    vg_rect_t full[CHANNEL_MAX] = { { 0, 40, 1280, 600 } };
    vg_rect_t none[CHANNEL_MAX] = { { 0 } };
    channels_config_t cc = { .urls = { url }, .n = 1, .seg_secs = 60 };
    CHECK(channels_start(&cc, quad) == 0, "start");
    WAIT_FOR(view_is(0, "sub", 640), 5000);
    CHECK(view_is(0, "sub", 640), "2x2: the sub-stream");
    pthread_mutex_lock(&g_dec_lock);
    g_dec_mid_gop[0] = g_dec_bad_start[0] = 0;
    pthread_mutex_unlock(&g_dec_lock);

    int opens = dec_count(g_dec_opens, 0), ok = 1;
    for (int i = 0; i < 6; ++i) {
        usleep(300 * 1000);                 /* both GOPs kept again */
        channels_set_tiles(i % 3 == 2 ? none : full);
        if (i % 3 == 2) {
            WAIT_FOR(!dec_count(g_dec_open, 0), 2000);
            usleep(300 * 1000);
        } else {
            WAIT_FOR(view_is(0, "main", 1920), 2000);
            ok &= view_is(0, "main", 1920);
            usleep(300 * 1000);
        }
        channels_set_tiles(quad);
        WAIT_FOR(view_is(0, "sub", 640), 2000);
        ok &= view_is(0, "sub", 640);
    }
    CHECK(ok, "every switch decoded the stream wanted");
    int mid = dec_count(g_dec_mid_gop, 0), bad = dec_count(g_dec_bad_start, 0);
    CHECK(dec_count(g_dec_opens, 0) - opens >= 10 && mid >= 3,
          "decoders opened mid-GOP from the cache (%d of %d)", mid, dec_count(g_dec_opens, 0) - opens);
    CHECK(bad == 0, "every decoder started at a keyframe (%d did not)", bad);
    CHECK(atomic_load(&g_cam.streaming[0]) == 1 && atomic_load(&g_cam.streaming[1]) == 1,
          "one connection per stream all along");

    channels_stop();
    CHECK(gop_cache_used() == 0, "stopped: every GOP given back (%zu)", gop_cache_used());
    cam_stop();
    channels_set_screen(0, 0);
    g_fake_caps.dec_shown_only = 0;
    g_dec_slots = 0;
}

/* ---- stream taps: a camera's AUs for vigild's own servers ---------------------- */

#define TAP_LOG 512
typedef struct {
    pthread_mutex_t lock;
    int             n, keys;
    uint64_t        pts[TAP_LOG];
    int             key[TAP_LOG];
    vg_codec_t      codec;
    unsigned        width;
    atomic_int      calls;
} tap_log_t;

static void tap_rec(void *user, const vg_au_t *au, const vg_stream_info_t *si, vg_codec_t codec)
{
    tap_log_t *l = user;
    pthread_mutex_lock(&l->lock);
    if (l->n < TAP_LOG) {
        l->pts[l->n] = au->pts_us;
        l->key[l->n] = au->key;
        ++l->n;
    }
    l->keys += au->key;
    l->codec = codec;
    l->width = si->width;
    pthread_mutex_unlock(&l->lock);
    atomic_fetch_add(&l->calls, 1);
}

static void tap_log_init(tap_log_t *l)
{
    memset(l, 0, sizeof *l);
    pthread_mutex_init(&l->lock, NULL);
}

static int tap_count(tap_log_t *l)
{
    pthread_mutex_lock(&l->lock);
    int n = l->n;
    pthread_mutex_unlock(&l->lock);
    return n;
}

typedef struct {
    volatile int stop;
    atomic_int   opens;
} tap_churn_t;

static void tap_nop(void *user, const vg_au_t *au, const vg_stream_info_t *si, vg_codec_t codec)
{
    (void)au; (void)si; (void)codec;
    atomic_fetch_add((atomic_int *)user, 1);
}

static void *tap_churn(void *arg)
{
    tap_churn_t *c = arg;
    atomic_int calls = 0;
    while (!c->stop) {
        stream_tap_t *t = stream_tap_open(0, rand() & 1, tap_nop, &calls);
        if (t) atomic_fetch_add(&c->opens, 1);
        usleep((unsigned)(rand() % 3000));
        stream_tap_close(t);
    }
    return NULL;
}

static void test_stream_tap(void)
{
    vg_stream_info_t si;
    vg_codec_t codec;
    tap_log_t a, b;
    tap_log_init(&a);
    tap_log_init(&b);
    CHECK(stream_tap_open(0, 0, tap_rec, &a) == NULL && stream_tap_info(0, 0, &si, &codec) < 0,
          "no channel: no tap, no stream");
    CHECK(stream_tap_open(CHANNEL_MAX, 0, tap_rec, &a) == NULL && stream_tap_open(0, 2, tap_rec, &a) == NULL,
          "out of range: no tap");

    int port = cam_start("majestic_1080p.h265", "testsrc_640x360.h264");
    if (!port) return;
    char url[64];
    snprintf(url, sizeof url, "netip://127.0.0.1:%d/?ch=0&stream=main", port);
    /* Off screen: the decoders keep no main-stream GOP of their own. */
    vg_rect_t none[CHANNEL_MAX] = { { 0 } };
    channels_config_t cc = { .urls = { url }, .n = 1, .seg_secs = 60 };
    CHECK(channels_start(&cc, none) == 0, "start");
    WAIT_FOR(stream_tap_info(0, 0, &si, &codec) == 0 && stream_tap_info(0, 1, NULL, NULL) == 0, 5000);
    CHECK(stream_tap_info(0, 0, &si, &codec) == 0 && codec == VG_CODEC_H265 && si.width == 1920,
          "main stream: H.265 1920 (%s %u)", vg_codec_name(codec), si.width);
    CHECK(stream_tap_info(0, 1, &si, &codec) == 0 && codec == VG_CODEC_H264 && si.width == 640,
          "sub-stream: H.264 640 (%s %u)", vg_codec_name(codec), si.width);
    stream_tap_ps_t ps;
    CHECK(stream_tap_params(0, 0, &ps) == 3 && (ps.ps[0][0] >> 1 & 0x3f) == 32 && (ps.ps[1][0] >> 1 & 0x3f) == 33 &&
              (ps.ps[2][0] >> 1 & 0x3f) == 34, "main: VPS, SPS, PPS");
    CHECK(stream_tap_params(0, 1, &ps) == 2 && (ps.ps[0][0] & 0x1f) == 7 && (ps.ps[1][0] & 0x1f) == 8,
          "sub: SPS, PPS");
    CHECK(stream_tap_keep(0, 0) == TAP_KEEP_NONE, "no tap, no standby: nothing kept for one");

    /* The first tap on the main stream: no GOP kept, it starts at the
     * next keyframe; then its stream's GOP is kept. */
    stream_tap_t *ta = stream_tap_open(0, 0, tap_rec, &a);
    CHECK(ta && stream_tap_keep(0, 0) == TAP_KEEP_MUST, "a tap: its GOP kept");
    WAIT_FOR(a.keys >= 2, 3000);
    CHECK(a.n > 0 && a.key[0] && a.codec == VG_CODEC_H265 && a.width == 1920,
          "the first AU a tap gets is a keyframe, with the stream's codec and size");
    /* A second tap in the middle of a GOP: the GOP so far, at once. */
    WAIT_FOR(atomic_load(&g_cam_since_key[0]) >= 8, 2000);
    pthread_mutex_lock(&a.lock);
    int a_before = a.n;
    uint64_t last_key = 0;
    for (int i = 0; i < a.n; ++i)
        if (a.key[i]) last_key = a.pts[i];
    pthread_mutex_unlock(&a.lock);
    stream_tap_t *tb = stream_tap_open(0, 0, tap_rec, &b);
    WAIT_FOR(tap_count(&b) > 0, 1000);
    pthread_mutex_lock(&a.lock);
    pthread_mutex_lock(&b.lock);
    CHECK(b.n > 1 && b.key[0] && b.pts[0] == last_key,
          "a tap opened mid-GOP starts with the GOP so far, at its keyframe (%d AUs)", b.n);
    int same = 0, from = -1;
    for (int i = 0; i < a.n && from < 0; ++i)
        if (a.pts[i] == b.pts[0]) from = i;
    for (int i = 0; from >= 0 && i < b.n && from + i < a.n; ++i) same += a.pts[from + i] == b.pts[i];
    CHECK(from >= 0 && from < a_before && same == (b.n < a.n - from ? b.n : a.n - from),
          "in the order the stream had (%d of %d)", same, b.n);
    pthread_mutex_unlock(&b.lock);
    pthread_mutex_unlock(&a.lock);

    /* Closed: never called again. */
    stream_tap_close(tb);
    int bc = atomic_load(&b.calls);
    usleep(200 * 1000);
    CHECK(atomic_load(&b.calls) == bc, "a closed tap is not called (%d, %d)", bc, atomic_load(&b.calls));
    stream_tap_close(ta);
    CHECK(stream_tap_keep(0, 0) == TAP_KEEP_NONE, "the last tap closed: its GOP no longer wanted");

    /* Standing by: the main stream's GOP is kept off screen too, so the
     * first tap of it starts at once. */
    stream_tap_standby(1);
    CHECK(stream_tap_keep(0, 0) == TAP_KEEP_SPARE, "standing by");
    usleep(600 * 1000);
    tap_log_t c;
    tap_log_init(&c);
    WAIT_FOR(atomic_load(&g_cam_since_key[0]) >= 8, 2000);
    stream_tap_t *tc = stream_tap_open(0, 0, tap_rec, &c);
    WAIT_FOR(tap_count(&c) > 0, 1000);
    CHECK(c.n > 1 && c.key[0], "standing by: the first tap gets the GOP so far (%d AUs)", c.n);
    stream_tap_close(tc);
    stream_tap_standby(0);

    /* Taps opened and closed from several threads while the stream runs. */
    tap_churn_t ch = { 0 };
    pthread_t th[4];
    for (int i = 0; i < 4; ++i) pthread_create(&th[i], NULL, tap_churn, &ch);
    usleep(1000 * 1000);
    ch.stop = 1;
    for (int i = 0; i < 4; ++i) pthread_join(th[i], NULL);
    CHECK(atomic_load(&ch.opens) > 100 && stream_tap_keep(0, 0) == TAP_KEEP_NONE &&
              stream_tap_keep(0, 1) == TAP_KEEP_NONE, "%d taps opened and closed from 4 threads",
          atomic_load(&ch.opens));

    /* A tap outlives its channel's stop, and gets nothing after it. */
    tap_log_t d;
    tap_log_init(&d);
    stream_tap_t *td = stream_tap_open(0, 1, tap_rec, &d);
    WAIT_FOR(tap_count(&d) > 0, 1000);
    channels_stop();
    CHECK(stream_tap_info(0, 1, NULL, NULL) < 0 && stream_tap_open(0, 1, tap_rec, &d) == NULL,
          "stopped: the stream is gone");
    int dc = atomic_load(&d.calls);
    usleep(100 * 1000);
    CHECK(dc > 0 && atomic_load(&d.calls) == dc, "nothing after the stop");
    stream_tap_close(td);
    CHECK(gop_cache_used() == 0, "every GOP given back (%zu)", gop_cache_used());
    cam_stop();
}

/* A 16:9 camera whose sub-stream is anamorphic D1 (704x576, claiming square
 * pixels): the sub-stream is shown at the main stream's 16:9, before and
 * after a switch to full screen; stretch, at once 0:0; fill, at once at
 * 16:9. */
static void test_channel_aspect(void)
{
    int port = cam_start("majestic_1080p.h265", "testsrc_704x576.h264");
    if (!port) return;
    char url[64];
    snprintf(url, sizeof url, "netip://127.0.0.1:%d/?ch=0&stream=main", port);
    g_fake_caps.dec_shown_only = 1;
    g_dec_slots = 2;
    channels_set_screen(1280, 720);
    vg_rect_t quad[CHANNEL_MAX] = { { 0, 40, 640, 300 } };
    vg_rect_t full[CHANNEL_MAX] = { { 0, 40, 1280, 600 } };
    channels_config_t cc = { .urls = { url }, .n = 1, .seg_secs = 60 };
    CHECK(channels_start(&cc, quad) == 0, "start");
    WAIT_FOR(view_is(0, "sub", 704) && aspect_now(0, 16, 9), 6000);
    CHECK(view_is(0, "sub", 704) && aspect_now(0, 16, 9), "the D1 sub-stream at the main stream's 16:9 (%u:%u)",
          g_aspect_at[0].w, g_aspect_at[0].h);
    channels_set_aspect(VG_ASPECT_STRETCH);
    CHECK(aspect_is(0, 0, 0, VG_ASPECT_STRETCH), "stretch: at once");
    channels_set_aspect(VG_ASPECT_FILL);
    CHECK(aspect_is(0, 16, 9, VG_ASPECT_FILL), "fill: at once, at the main stream's 16:9");
    channels_set_aspect(VG_ASPECT_FIT);
    CHECK(aspect_now(0, 16, 9), "fit again");
    channels_set_tiles(full);
    WAIT_FOR(view_is(0, "main", 1920), 5000);
    CHECK(view_is(0, "main", 1920) && aspect_now(0, 16, 9), "full screen: the main stream, the same shape");
    channels_set_tiles(quad);
    WAIT_FOR(view_is(0, "sub", 704), 5000);
    CHECK(view_is(0, "sub", 704) && aspect_now(0, 16, 9), "and back: the same shape");
    channels_stop();
    cam_stop();
    channels_set_screen(0, 0);
    g_fake_caps.dec_shown_only = 0;
    g_dec_slots = 0;
}

/* ---- NETIP: the camera's quirks, the stream on screen -------------------------------- */

static void test_netip_title_parse(void)
{
    char out[32];
    static const struct { const char *reply; int ch, rc; const char *want; } cases[] = {
        { "{\"Name\":\"ChannelTitle\",\"ChannelTitle\":[\"CAM01\"],\"Ret\":100,\"SessionID\":\"0x1\"}\n", 0, 0,
          "CAM01" },
        { "{\"ChannelTitle\":[\"Gate\",\"Yard\"],\"Ret\":100}", 1, 0, "Yard" },
        { "{\"ChannelTitle\":[\"Gate\"],\"Ret\":100}", 1, -1, "" },            /* no such channel */
        { "{\"ChannelTitle\":[{\"Name\":\"Porch\",\"SetOSD\":true}],\"Ret\":100}", 0, 0, "Porch" },
        { "{\"ChannelTitle\":\"Shed\",\"Ret\":100}", 0, 0, "Shed" },          /* one, not a list */
        { "{\"ChannelTitle\":[\"  Back door  \"]}", 0, 0, "Back door" },
        { "{\"ChannelTitle\":[\"\u0421\u0430\u0434\"],\"Ret\":100}", 0, 0, "\xd0\xa1\xd0\xb0\xd0\xb4" },
        { "{\"ChannelTitle\":[\"a\\tb\"],\"Ret\":100}", 0, 0, "ab" },        /* control characters go */
        { "{\"ChannelTitle\":[\"\xc4\xe3\xba\xc3\"],\"Ret\":100}", 0, 0, "????" },   /* GB2312, not UTF-8 */
        { "{\"Name\":\"ChannelTitle\",\"Ret\":103}", 0, -1, "" },
        { "{\"ChannelTitle\":[\"CAM01\"],\"Ret\":107}", 0, -1, "" },
        { "not json", 0, -1, "" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        int rc = netip_title_parse(cases[i].reply, cases[i].ch, out, sizeof out);
        CHECK(rc == cases[i].rc && !strcmp(out, cases[i].want), "title %zu: %d '%s'", i, rc, out);
    }
    CHECK(netip_title_parse("{\"ChannelTitle\":[\"0123456789\xd0\xa1\"]}", 0, out, 12) == 0 &&
              !strcmp(out, "0123456789"), "cut on a character: '%s'", out);
}


/* vigild's NETIP server frames what its stream taps give it
 * (netip_proto.h); vigil's own NETIP source must read back the same
 * access units: codec, keyframes, every byte, a 4 MP picture's size bits
 * in the codec byte, frames split over messages as the server sends them. */
typedef struct { int n; vg_au_t au[4]; uint8_t *data[4]; } emitted_t;

static void collect(void *user, const vg_au_t *au)
{
    emitted_t *e = user;
    if (e->n >= 4) return;
    e->au[e->n] = *au;
    e->data[e->n] = malloc(au->len);
    memcpy(e->data[e->n], au->data, au->len);
    e->au[e->n].data = e->data[e->n];
    ++e->n;
}

/* As netip_srv.c sends a frame: its header and payload, 8 KiB a message. */
static void send_frame(source_t *src, const uint8_t *head, size_t hl, const uint8_t *p, size_t len, emitted_t *e)
{
    size_t total = hl + len;
    uint8_t *f = malloc(total);
    memcpy(f, head, hl);
    memcpy(f + hl, p, len);
    for (size_t off = 0; off < total; off += 8192)
        netip_media_feed(src, f + off, total - off > 8192 ? 8192 : total - off, collect, e);
    free(f);
}

static void test_netip_server_proto(void)
{
    /* The header. */
    uint8_t b[NETIP_HDR_LEN];
    netip_hdr_t h = { .version = 1, .session = 0x1234ABCD, .seq = 7, .total = 3, .cur = 1, .msgid = 1412,
                      .len = 8192 }, g;
    netip_hdr_put(b, &h);
    CHECK(b[0] == 0xff && b[14] == (1412 & 255) && b[15] == 1412 >> 8 && b[12] == 3 && b[16] == 0 && b[17] == 0x20,
          "header bytes");
    CHECK(netip_hdr_get(b, &g) == 0 && g.version == 1 && g.session == h.session && g.seq == 7 && g.total == 3 &&
          g.cur == 1 && g.msgid == 1412 && g.len == 8192, "header round trip");
    b[0] = 0;
    CHECK(netip_hdr_get(b, &g) < 0, "not NETIP without 0xFF");

    /* XM's packed time. */
    struct tm t = { .tm_year = 126, .tm_mon = 10, .tm_mday = 27, .tm_hour = 23, .tm_min = 59, .tm_sec = 58 }, u;
    netip_time_unpack(netip_time_pack(&t), &u);
    CHECK(u.tm_year == 126 && u.tm_mon == 10 && u.tm_mday == 27 && u.tm_hour == 23 && u.tm_min == 59 &&
          u.tm_sec == 58, "time round trip");
    struct tm doc = { .tm_year = 109, .tm_mon = 1, .tm_mday = 13, .tm_hour = 12, .tm_min = 3, .tm_sec = 12 };
    CHECK(netip_time_pack(&doc) == (12u | 3u << 6 | 12u << 12 | 13u << 17 | 2u << 22 | 9u << 26), "time layout");

    uint32_t sid;
    CHECK(netip_session_parse("0x0000000B", &sid) == 0 && sid == 11 && netip_session_parse("x", &sid) < 0,
          "session ids");
    CHECK(netip_stream_type("Main") == 0 && netip_stream_type("Extra1") == 1 && netip_stream_type("Extra") == 1 &&
          netip_stream_type("Sub") == 1 && netip_stream_type("Fourth") < 0, "stream types");

    /* Frames, through source_netip's parser. */
    uint8_t head[NETIP_FRAME_HEAD];
    CHECK(netip_frame_head(head, 1, VG_CODEC_NONE, 640, 360, 25, &t, 10) == 0, "no codec, no frame");
    size_t n = 20000, hl;
    uint8_t *key = malloc(n), *p = malloc(5000);
    for (size_t i = 0; i < n; ++i) key[i] = (uint8_t)(i * 7 + 3);
    memcpy(key, "\0\0\0\1\x40\x01", 6);                /* VPS ... */
    for (size_t i = 0; i < 5000; ++i) p[i] = (uint8_t)(i * 13 + 1);
    memcpy(p, "\0\0\0\1\x02\x01", 6);                  /* a TRAIL_R slice */
    source_t *src = source_create("netip://admin:x@127.0.0.1/", 0);
    CHECK(src != NULL, "a NETIP source to parse with");
    if (!src) return;
    emitted_t e = { 0 };
    hl = netip_frame_head(head, 1, VG_CODEC_H265, 2560, 1440, 25, &t, (uint32_t)n);
    CHECK(hl == 16 && head[3] == 0xfc && head[4] == 0x13 && head[5] == 25 && head[6] == (320 & 255) &&
          head[7] == 180, "a 2560x1440 H.265 I-frame header: codec byte 0x13 (%02x)", head[4]);
    send_frame(src, head, hl, key, n, &e);
    hl = netip_frame_head(head, 0, VG_CODEC_H265, 2560, 1440, 25, &t, 5000);
    CHECK(hl == 8 && head[3] == 0xfd, "a P-frame header");
    send_frame(src, head, hl, p, 5000, &e);
    CHECK(e.n == 2 && e.au[0].codec == VG_CODEC_H265 && e.au[0].key && e.au[0].len == n &&
          !memcmp(e.au[0].data, key, n) && e.au[1].codec == VG_CODEC_H265 && !e.au[1].key &&
          e.au[1].len == 5000 && !memcmp(e.au[1].data, p, 5000), "H.265 read back as sent (%d)", e.n);
    for (int i = 0; i < e.n; ++i) free(e.data[i]);
    memset(&e, 0, sizeof e);
    hl = netip_frame_head(head, 1, VG_CODEC_H264, 3840, 2160, 30, &t, 9000);
    CHECK(head[4] == (2 | 1 << 4 | 1 << 6) && head[6] == (480 & 255) && head[7] == (270 & 255),
          "3840x2160 H.264: 0x%02x", head[4]);
    key[4] = 0x67;
    send_frame(src, head, hl, key, 9000, &e);
    CHECK(e.n == 1 && e.au[0].codec == VG_CODEC_H264 && e.au[0].key && e.au[0].len == 9000 &&
          !memcmp(e.au[0].data, key, 9000), "H.264 read back as sent");
    for (int i = 0; i < e.n; ++i) free(e.data[i]);
    source_destroy(src);
    free(key);
    free(p);

    /* The search answer the server gives, as the camera search reads it. */
    disc_device_t d = { 0 }, r;
    snprintf(d.ip, sizeof d.ip, "192.168.1.10");
    snprintf(d.mask, sizeof d.mask, "255.255.255.0");
    snprintf(d.gateway, sizeof d.gateway, "192.168.1.1");
    snprintf(d.mac, sizeof d.mac, "02:00:00:00:00:42");
    snprintf(d.serial, sizeof d.serial, "0000020000000042");
    snprintf(d.name, sizeof d.name, "nvr");
    snprintf(d.version, sizeof d.version, "V0.1.0.vigil");
    d.netip_port = 34567;
    d.http_port = 80;
    d.channels = 9;
    uint8_t pkt[1024];
    size_t pl = disc_netip_reply(pkt, sizeof pkt, &d);
    CHECK(pl > 20 && pl <= 1024 && disc_netip_parse(pkt, pl, &r) == 0 && !strcmp(r.ip, "192.168.1.10") &&
          !strcmp(r.mask, "255.255.255.0") && !strcmp(r.gateway, "192.168.1.1") && !strcmp(r.mac, d.mac) &&
          !strcmp(r.serial, d.serial) && r.netip_port == 34567 && r.channels == 9 && !strcmp(r.name, "nvr"),
          "search answer round trip (%zu bytes)", pl);
    uint8_t q[32];
    CHECK(disc_netip_is_search(q, disc_netip_search(q, sizeof q)) && !disc_netip_is_search(pkt, pl), "a search");
    CHECK(disc_netip_reply(pkt, 100, &d) == 0, "too small a buffer");
}

static int status_is(int ch, ch_state_t want, int *live)
{
    channel_status_t st;
    channels_status(ch, &st);
    if (live) *live = st.live;
    return st.status == want;
}

/* The field report: a tile showing a camera's sub-stream said "No video"
 * while its main stream failed. The tile follows the stream on screen;
 * the main stream's state is said on its own. */
static void test_channel_stream_states(void)
{
    int port = cam_start("majestic_1080p.h265", "testsrc_640x360.h264");
    if (!port) return;
    char url[64], name[40];
    snprintf(url, sizeof url, "netip://127.0.0.1:%d/?ch=0&stream=main", port);
    g_cam.title = "Front door";
    g_cam.refuse[0] = 1;
    g_fake_caps.dec_shown_only = 1;
    g_dec_slots = 2;
    channels_set_screen(1280, 720);
    vg_rect_t quad[CHANNEL_MAX] = { { 0, 40, 640, 300 } };
    vg_rect_t full[CHANNEL_MAX] = { { 0, 40, 1280, 600 } };
    channels_config_t cc = { .urls = { url }, .n = 1, .seg_secs = 60 };
    CHECK(channels_start(&cc, quad) == 0, "start");

    /* 2x2, the main stream refused: the sub-stream plays, the tile is live. */
    WAIT_FOR(view_is(0, "sub", 640), 5000);
    channel_status_t st;
    WAIT_FOR((channels_status(0, &st), st.main_status == CH_ERROR), 3000);
    channels_status(0, &st);
    CHECK(view_is(0, "sub", 640) && st.status == CH_LIVE && st.live,
          "sub-stream on screen, main refused: live (%s '%s')", ch_state_name(st.status), st.state);
    CHECK(st.main_status == CH_ERROR && strstr(st.main_state, "monitor refused (Ret 103)") &&
              st.sub_status == CH_LIVE, "the main stream's failure is its own (%s '%s', sub %s)",
          ch_state_name(st.main_status), st.main_state, ch_state_name(st.sub_status));
    /* No name configured: the camera's. */
    CHECK(!strcmp(st.title, "Front door") && !strcmp(st.name, "Front door"), "the camera's name: '%s' '%s'",
          st.title, st.name);

    /* Full screen: the main stream never comes to take over, so the
     * sub-stream, connected all along, keeps the picture up; the main
     * stream's failure is still said as its own. */
    channels_set_tiles(full);
    usleep(1500 * 1000);
    channels_status(0, &st);
    CHECK(st.status == CH_LIVE && st.decoding && !strcmp(st.view, "sub") && st.sub_status == CH_LIVE &&
              st.main_status == CH_ERROR && strstr(st.main_state, "monitor refused"),
          "full screen, main refused: the sub-stream's picture stays (%s '%s', view '%s', main %s)",
          ch_state_name(st.status), st.state, st.view, ch_state_name(st.main_status));

    /* A configured name wins over the camera's. */
    cc.names[0] = "Gate";
    CHECK(channels_apply(&cc) == 0, "rename");
    channels_status(0, &st);
    CHECK(!strcmp(st.name, "Gate") && !strcmp(st.title, "Front door"), "configured name: '%s' (title '%s')",
          st.name, st.title);
    channels_stop();

    /* The other way round: the sub-stream refused. Full screen shows the
     * main stream; in 2x2 it keeps its picture up (no sub-stream keyframe
     * comes to take over), so the tile is live, and the sub-stream's
     * failure is its own. */
    g_cam.refuse[0] = 0;
    g_cam.refuse[1] = 1;
    cc.names[0] = NULL;
    CHECK(channels_start(&cc, full) == 0, "start full screen");
    WAIT_FOR(view_is(0, "main", 1920), 5000);
    CHECK(view_is(0, "main", 1920) && status_is(0, CH_LIVE, NULL), "full screen: main, live");
    channels_set_tiles(quad);
    WAIT_FOR((channels_status(0, &st), st.sub_status == CH_ERROR), 5000);
    channels_status(0, &st);
    CHECK(st.status == CH_LIVE && st.decoding && st.sub_status == CH_ERROR && st.main_status == CH_LIVE,
          "2x2, sub refused: the main picture stays, live (%s, sub %s '%s')", ch_state_name(st.status),
          ch_state_name(st.sub_status), st.sub_state);
    channels_stop();

    /* An XM 4 MP camera: the codec byte carries the size's high bits. */
    g_cam.refuse[1] = 0;
    g_cam.wide = 1;
    CHECK(channels_start(&cc, full) == 0, "start a wide camera");
    WAIT_FOR(view_is(0, "main", 1920), 5000);
    CHECK(view_is(0, "main", 1920) && status_is(0, CH_LIVE, NULL), "codec byte 0x13: H.265, decoded");
    channels_stop();

    /* Asked when a camera is added: the name it gives its channel. */
    snprintf(url, sizeof url, "netip://127.0.0.1:%d/?ch=0", port);
    CHECK(source_title(url, name, sizeof name, NULL) == 0 && !strcmp(name, "Front door"), "source_title: '%s'",
          name);
    CHECK(source_title("rtsp://127.0.0.1:1/x", name, sizeof name, NULL) < 0 && !name[0], "RTSP: no title");

    cam_stop();
    channels_set_screen(0, 0);
    g_fake_caps.dec_shown_only = 0;
    g_dec_slots = 0;
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


/* ---- recording: full disk, quotas, schedule --------------------------------------- */

static void touch_at(const char *dir, const char *name, size_t size, time_t mtime)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    for (size_t i = 0; i < size; ++i) fputc(0, f);
    fclose(f);
    struct timeval tv[2] = { { mtime, 0 }, { mtime, 0 } };
    utimes(path, tv);
}

static int exists_in(const char *dir, const char *name)
{
    char path[512];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    return stat(path, &st) == 0;
}

static void test_rec_policies(void)
{
    char dir[] = "/tmp/vigil-pol-XXXXXX";
    if (!mkdtemp(dir)) return;
    uint8_t data[100] = {0};

    /* record.on_full stop: below the reserve nothing is deleted, the open
     * segment is closed and seg_write says SEG_FULL until there is room. */
    touch_at(dir, "CAM2_20260101_000000.h264", 10, 1000);
    seg_writer_t w;
    seg_init(&w, dir, "CAM1", 3600, 0);
    seg_set_ext(&w, "h264");
    seg_set_overwrite(&w, 0);
    CHECK(seg_write(&w, data, sizeof data, 1) == 0 && w.f, "writes while there is room");
    w.reserve = UINT64_MAX;
    w.space_checked = 0;
    CHECK(seg_write(&w, data, sizeof data, 1) == SEG_FULL && !w.f, "full: SEG_FULL, segment closed");
    CHECK(exists_in(dir, "CAM2_20260101_000000.h264"), "stop: another camera's recording is kept");
    w.space_checked = 0;
    CHECK(seg_write(&w, data, sizeof data, 1) == SEG_FULL, "still full");
    w.reserve = 0;
    w.space_checked = 0;
    CHECK(seg_write(&w, data, sizeof data, 1) == 0 && w.f, "room again: writing");
    seg_close(&w);
    /* overwrite (the default) deletes the oldest in the directory instead */
    seg_set_overwrite(&w, 1);
    w.reserve = UINT64_MAX;
    w.space_checked = 0;
    CHECK(seg_write(&w, data, sizeof data, 1) == 0, "overwrite: no SEG_FULL");
    CHECK(!exists_in(dir, "CAM2_20260101_000000.h264"), "overwrite: the oldest recording made room");
    seg_close(&w);
    char cmd[80];
    snprintf(cmd, sizeof cmd, "rm -f %s/*", dir);
    if (system(cmd)) {}

    /* Quotas: by size the oldest of the camera's own go; by age those
     * older than the limit; another camera's (CAM10 is not CAM1) stay. */
    time_t now = 1800000000;
    touch_at(dir, "CAM1_20260101_000000.h264", 400, now - 10 * 86400);
    touch_at(dir, "CAM1_20260102_000000.h264", 400, now - 5 * 86400);
    touch_at(dir, "CAM1_20260103_000000.h265", 400, now - 2 * 86400);
    touch_at(dir, "CAM1_20260104_000000.h264", 400, now - 1 * 86400);
    touch_at(dir, "CAM10_20250101_000000.h264", 400, now - 100 * 86400);
    touch_at(dir, "CAM1_notes.txt", 400, now - 100 * 86400);
    CHECK(seg_quota_enforce(dir, "CAM1", 1000, 0, now) == 2, "size quota: two deleted");
    CHECK(!exists_in(dir, "CAM1_20260101_000000.h264") && !exists_in(dir, "CAM1_20260102_000000.h264") &&
          exists_in(dir, "CAM1_20260103_000000.h265") && exists_in(dir, "CAM1_20260104_000000.h264"),
          "size quota: the oldest two by name went");
    CHECK(exists_in(dir, "CAM10_20250101_000000.h264") && exists_in(dir, "CAM1_notes.txt"),
          "quota: CAM10 and other files untouched");
    CHECK(seg_quota_enforce(dir, "CAM1", 0, 36 * 3600, now) == 1 && !exists_in(dir, "CAM1_20260103_000000.h265") &&
          exists_in(dir, "CAM1_20260104_000000.h264"), "age quota: older than 36 h deleted");
    CHECK(seg_quota_enforce(dir, "CAM1", 0, 0, now) == 0, "no quota: nothing");
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd)) {}

    /* The week: Monday first, half hours from 00:00 local time. */
    rec_week_t wk;
    memset(&wk, 0, sizeof wk);
    struct tm tm = { .tm_wday = 1, .tm_hour = 12, .tm_min = 0 };
    CHECK(rec_week_at(&wk, &tm) == REC_SCHED_CONT, "no week: continuous");
    wk.set = 1;
    for (int d = 0; d < 7; ++d) memset(wk.day[d], 'C', REC_SCHED_SLOTS);
    wk.day[0][24] = '-';                    /* Monday 12:00-12:30 */
    wk.day[0][25] = 'M';                    /* Monday 12:30-13:00 */
    wk.day[6][47] = '-';                    /* Sunday 23:30-24:00 */
    CHECK(rec_week_at(&wk, &tm) == REC_SCHED_OFF, "Monday 12:00: off");
    tm.tm_min = 29;
    CHECK(rec_week_at(&wk, &tm) == REC_SCHED_OFF, "Monday 12:29: off");
    tm.tm_min = 30;
    CHECK(rec_week_at(&wk, &tm) == REC_SCHED_MOTION, "Monday 12:30: motion");
    tm.tm_hour = 13;
    tm.tm_min = 0;
    CHECK(rec_week_at(&wk, &tm) == REC_SCHED_CONT, "Monday 13:00: continuous");
    struct tm sun = { .tm_wday = 0, .tm_hour = 23, .tm_min = 45 };
    CHECK(rec_week_at(&wk, &sun) == REC_SCHED_OFF, "Sunday 23:45: off");

    /* From the configuration: per camera, the rest always on. */
    vigil_config_t c;
    config_defaults(&c);
    schema_errors_t e = { 0 };
    cJSON *doc = cJSON_Parse("{\"schedule\":[{\"camera\":3,\"sat\":\"------------------------------------------------\"}],"
                             "\"quota\":[{\"camera\":2,\"max_gb\":5,\"max_days\":7}],"
                             "\"record\":{\"path\":\"/x\",\"on_full\":\"stop\"}}");
    CHECK(schema_from_json(&config_schema, doc, &c, 1, &e) == 0, "schedule and quota load: %s",
          e.n ? e.e[0].msg : "");
    cJSON_Delete(doc);
    rec_week_t weeks[CHANNEL_MAX];
    config_to_schedule(&c, weeks);
    struct tm sat = { .tm_wday = 6, .tm_hour = 10 };
    CHECK(!weeks[0].set && weeks[2].set && rec_week_at(&weeks[2], &sat) == REC_SCHED_OFF &&
          rec_week_at(&weeks[2], &tm) == REC_SCHED_CONT, "CAM3: Saturday off, the rest on");
    channels_config_t cc;
    config_to_channels(&c, &cc);
    CHECK(cc.stop_when_full && cc.quota_bytes[1] == 5000000000ull && cc.quota_age[1] == 7 * 86400 &&
          !cc.quota_bytes[0], "on_full and quotas reach the channels");
    e = (schema_errors_t){ 0 };
    doc = cJSON_Parse("{\"schedule\":[{\"camera\":1,\"mon\":\"CCX\"}]}");
    CHECK(schema_from_json(&config_schema, doc, &c, 1, &e) < 0 && e.n && !strcmp(e.e[0].path, "schedule[0].mon"),
          "a day that is not 48 letters is refused");
    cJSON_Delete(doc);
}

static void test_secrets(void)
{
    char dir[] = "/tmp/vigil-sec-XXXXXX", path[128];
    if (!mkdtemp(dir)) return;
    snprintf(path, sizeof path, "%s/vigil.yaml", dir);
    write_file(path, "smtp:\n  server: mail.example.org\n  password: \"s3cret\"\nnas:\n  type: cifs\n");
    CHECK(config_init(path) == 0, "loaded");
    char *j = config_get_json("smtp", 1);
    CHECK(j && strstr(j, "\"password\":\t\"***\"") && !strstr(j, "s3cret"), "smtp password redacted: %s", j);
    free(j);
    j = config_get_json("nas", 1);
    CHECK(j && strstr(j, "\"password\":\t\"\""), "an empty password shows empty: %s", j);
    free(j);
    schema_errors_t e = { 0 };
    const char *body = "{\"server\":\"mail2.example.org\",\"password\":\"***\"}";
    CHECK(config_put_json("smtp", body, strlen(body), &e) == CONFIG_OK, "PUT with ***");
    vigil_config_t c;
    config_get(&c);
    CHECK(!strcmp(c.smtp.password, "s3cret") && !strcmp(c.smtp.server, "mail2.example.org"),
          "*** keeps the stored password");
    body = "{\"password\":\"new\"}";
    config_put_json("smtp", body, strlen(body), &e);
    config_get(&c);
    CHECK(!strcmp(c.smtp.password, "new"), "a new password replaces it");
    char *file = read_file(path);
    CHECK(file && strstr(file, "password: new"), "the file holds the password itself");
    free(file);
    body = "{\"to\":\"a@b.c, bad\"}";
    e = (schema_errors_t){ 0 };
    CHECK(config_put_json("smtp", body, strlen(body), &e) == CONFIG_EINVAL && !strcmp(e.e[0].path, "smtp.to"),
          "a bad address is refused");
    char cmd[80];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd)) {}
}

/* display: the output mode and what the live view draws over the video. */
/* ---- QoS (DSCP marking) ------------------------------------------------------------- */

static int sock_tos(int fd)
{
    int v = -1;
    socklen_t l = sizeof v;
    return getsockopt(fd, IPPROTO_IP, IP_TOS, &v, &l) < 0 ? -1 : v;
}

static void test_qos_config(void)
{
    CHECK(qos_tos(0) == 0 && qos_tos(34) == 136 && qos_tos(46) == 184 && qos_tos(63) == 252 &&
          qos_tos(64) == -1 && qos_tos(-1) == -1, "DSCP -> TOS: << 2, ECN bits 0, 0..63 only");
    CHECK(!strcmp(qos_dscp_name(34), "AF41") && !strcmp(qos_dscp_name(46), "EF") &&
          !strcmp(qos_dscp_name(16), "CS2") && !strcmp(qos_dscp_name(0), "CS0") && !qos_dscp_name(5),
          "DSCP names");

    vigil_config_t c;
    config_defaults(&c);
    int d[QOS_CLASSES];
    config_to_qos(&c, d);
    CHECK(!strcmp(c.qos.type, "off") && !d[QOS_VIDEO] && !d[QOS_MANAGEMENT] && !d[QOS_CAMERA] && !d[QOS_ALARM],
          "qos defaults: off, DSCP 0 everywhere");
    snprintf(c.qos.type, sizeof c.qos.type, "standard");
    c.qos.video = 7;                    /* ignored unless custom */
    config_to_qos(&c, d);
    CHECK(d[QOS_VIDEO] == 34 && d[QOS_MANAGEMENT] == 16 && d[QOS_CAMERA] == 34 && d[QOS_ALARM] == 18,
          "standard: AF41, CS2, AF41, AF21: %d %d %d %d", d[0], d[1], d[2], d[3]);
    snprintf(c.qos.type, sizeof c.qos.type, "custom");
    c.qos.management = 0;
    c.qos.camera = 63;
    config_to_qos(&c, d);
    CHECK(d[QOS_VIDEO] == 7 && d[QOS_MANAGEMENT] == 0 && d[QOS_CAMERA] == 63 && d[QOS_ALARM] == 18,
          "custom: the fields");

    const schema_section_t *sec = schema_section(&config_schema, "qos");
    schema_errors_t e = { 0 };
    cJSON *j = cJSON_Parse("{\"type\":\"custom\",\"video\":64}");
    CHECK(sec && schema_section_from_json(&config_schema, sec, j, &c, 1, &e) < 0 &&
          !strcmp(e.e[0].path, "qos.video"), "video 64: refused (%s %s)", e.n ? e.e[0].path : "", e.n ? e.e[0].msg : "");
    cJSON_Delete(j);
    memset(&e, 0, sizeof e);
    j = cJSON_Parse("{\"type\":\"custom\",\"alarm\":-1}");
    CHECK(sec && schema_section_from_json(&config_schema, sec, j, &c, 1, &e) < 0, "alarm -1: refused");
    cJSON_Delete(j);
    memset(&e, 0, sizeof e);
    j = cJSON_Parse("{\"type\":\"wfq\"}");
    CHECK(sec && schema_section_from_json(&config_schema, sec, j, &c, 1, &e) < 0, "an unknown type: refused");
    cJSON_Delete(j);
    memset(&e, 0, sizeof e);
    j = cJSON_Parse("{\"type\":\"custom\",\"video\":46,\"management\":0,\"camera\":63}");
    CHECK(sec && schema_section_from_json(&config_schema, sec, j, &c, 1, &e) == 0 && c.qos.video == 46 &&
          c.qos.camera == 63 && c.qos.alarm == 18, "custom 46/0/63: accepted, alarm its default");
    cJSON_Delete(j);
}

/* What qos_set and the markers do to real sockets: read back with
 * getsockopt(IP_TOS). */
static void test_qos_sockets(void)
{
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof a;
    if (l < 0 || bind(l, (struct sockaddr *)&a, sizeof a) < 0 || listen(l, 4) < 0 ||
        getsockname(l, (struct sockaddr *)&a, &al) < 0) {
        fprintf(stderr, "qos socket test: no listener, skipped\n");
        return;
    }
    int port = ntohs(a.sin_port);
    int dscp[QOS_CLASSES] = { [QOS_VIDEO] = 34, [QOS_MANAGEMENT] = 16, [QOS_CAMERA] = 26, [QOS_ALARM] = 18 };
    qos_set(dscp);
    CHECK(qos_mark_listeners(port, QOS_MANAGEMENT) == 1 && sock_tos(l) == 64, "the listener: CS2 (TOS %d)",
          sock_tos(l));

    char err[64];
    int c = net_connect("127.0.0.1", port, 2000, NULL, err, sizeof err);
    int s = c >= 0 ? accept(l, NULL, NULL) : -1;
    CHECK(c >= 0 && sock_tos(c) == 26 << 2, "net_connect: the camera class, AF31 (TOS %d)", sock_tos(c));
    CHECK(s >= 0 && sock_tos(s) == 64, "accepted: the listener's class (TOS %d)", sock_tos(s));

    /* the server's end found by its peer, as a WebSocket after its upgrade */
    struct sockaddr_in ca;
    socklen_t cal = sizeof ca;
    getsockname(c, (struct sockaddr *)&ca, &cal);
    CHECK(qos_mark_peer(port, "127.0.0.1", ntohs(ca.sin_port), QOS_VIDEO) == 0 && sock_tos(s) == 34 << 2,
          "by its peer: video (TOS %d)", sock_tos(s));
    CHECK(qos_mark_peer(port, "127.0.0.2", ntohs(ca.sin_port), QOS_VIDEO) < 0, "no such peer");

    /* a change marks the tracked ones again, each by its class */
    int dscp2[QOS_CLASSES] = { [QOS_VIDEO] = 46, [QOS_MANAGEMENT] = 8, [QOS_CAMERA] = 0, [QOS_ALARM] = 0 };
    int n = qos_set(dscp2);
    CHECK(n >= 3 && sock_tos(l) == 32 && sock_tos(s) == 184 && sock_tos(c) == 0,
          "qos_set: %d marked again; listener %d, video %d, camera %d", n, sock_tos(l), sock_tos(s), sock_tos(c));

    /* a closed fd whose number is reused by another socket is left alone */
    close(c);
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(u == c || u != c, "(fd %d reused as %d)", c, u);
    int dscp3[QOS_CLASSES] = { [QOS_CAMERA] = 10 };
    qos_set(dscp3);
    CHECK(u != c || sock_tos(u) == 0, "a reused fd: not marked (TOS %d)", sock_tos(u));

    /* IPv6: the traffic class */
    int s6 = socket(AF_INET6, SOCK_STREAM, 0);
    if (s6 >= 0) {
        int tc = -1;
        socklen_t tl = sizeof tc;
        CHECK(qos_apply(s6, 46) == 0 && getsockopt(s6, IPPROTO_IPV6, IPV6_TCLASS, &tc, &tl) == 0 && tc == 184,
              "IPv6: IPV6_TCLASS %d", tc);
        close(s6);
    }
    CHECK(qos_apply(l, 64) < 0, "64: refused");
    int zero[QOS_CLASSES] = { 0 };
    qos_set(zero);
    qos_forget(l);
    if (u >= 0) close(u);
    if (s >= 0) close(s);
    close(l);
}

/* ---- ports of vigild's servers ---------------------------------------------------- */

static char g_lc_what[8], g_lc_listen[64];
static int fake_listen_check(const char *what, const char *listen, char *err, size_t cap)
{
    snprintf(g_lc_what, sizeof g_lc_what, "%s", what);
    snprintf(g_lc_listen, sizeof g_lc_listen, "%s", listen);
    if (config_listen_port(listen) == 81) {
        snprintf(err, cap, "port 81 is in use");
        return -1;
    }
    return 0;
}

static void test_ports(void)
{
    CHECK(config_listen_port("0.0.0.0:80") == 80 && config_listen_port("8080") == 8080 &&
          config_listen_port("") == 0 && config_listen_port("x") == -1 && config_listen_port("1.2.3.4:") == -1,
          "the port of a listen setting");
    vigil_config_t c;
    config_defaults(&c);
    schema_errors_t e = { 0 };
    CHECK(c.rtsp_server.port == 554 && c.onvif_server.port == 8899 && c.netip_server.port == 34567 &&
          config_check_ports(&c, &e) == 0, "defaults: 80, 8080, 554, 8899, 34567, no clash");
    c.onvif_server.port = 80;
    CHECK(config_check_ports(&c, &e) < 0 && !strcmp(e.e[0].path, "onvif_server.port") &&
          strstr(e.e[0].msg, "web.listen"), "ONVIF on 80: the web server's (%s)", e.n ? e.e[0].msg : "");
    config_defaults(&c);
    memset(&e, 0, sizeof e);
    snprintf(c.api.listen, sizeof c.api.listen, "554");
    CHECK(config_check_ports(&c, &e) < 0 && !strcmp(e.e[0].path, "rtsp_server.port"), "API on 554: RTSP's");
    config_defaults(&c);
    memset(&e, 0, sizeof e);
    c.web.listen[0] = 0;
    c.netip_server.port = 80;
    CHECK(config_check_ports(&c, &e) == 0, "an empty web.listen holds no port");

    /* through the store: a clash, a listen the check refuses, then a good one */
    char dir[] = "/tmp/vigil-ports-XXXXXX", path[128];
    if (!mkdtemp(dir)) return;
    snprintf(path, sizeof path, "%s/vigil.yaml", dir);
    write_file(path, "web:\n  listen: \"0.0.0.0:80\"\n");
    config_init(path);
    memset(&e, 0, sizeof e);
    CHECK(config_put_json("netip_server", "{\"port\":80}", 11, &e) == CONFIG_EINVAL && e.n &&
          !strcmp(e.e[0].path, "netip_server.port"), "PUT NETIP onto the web server's port: refused");
    config_set_listen_check(fake_listen_check);
    memset(&e, 0, sizeof e);
    const char *w81 = "{\"listen\":\"0.0.0.0:81\"}";
    CHECK(config_put_json("web", w81, strlen(w81), &e) == CONFIG_EINVAL && !strcmp(e.e[0].path, "web.listen") &&
          strstr(e.e[0].msg, "in use") && !strcmp(g_lc_what, "web"), "web.listen onto a port in use: refused, why");
    const char *o81 = "{\"enabled\":true,\"port\":81}", *o81off = "{\"enabled\":false,\"port\":81}";
    memset(&e, 0, sizeof e);
    CHECK(config_put_json("onvif_server", o81, strlen(o81), &e) == CONFIG_EINVAL &&
          !strcmp(e.e[0].path, "onvif_server.port") && !strcmp(g_lc_what, "onvif") && !strcmp(g_lc_listen, "81"),
          "onvif_server.port onto a port in use: refused (%s)", e.n ? e.e[0].msg : "");
    g_lc_what[0] = 0;
    memset(&e, 0, sizeof e);
    CHECK(config_put_json("onvif_server", o81off, strlen(o81off), &e) == CONFIG_OK && !g_lc_what[0],
          "ONVIF off: its port is not tried");
    g_lc_what[0] = 0;
    const char *api_same = "{\"listen\":\"0.0.0.0:8080\"}";
    memset(&e, 0, sizeof e);
    CHECK(config_put_json("api", api_same, strlen(api_same), &e) == CONFIG_OK && !g_lc_what[0],
          "api.listen unchanged: not asked");
    const char *w82 = "{\"listen\":\"0.0.0.0:82\"}";
    vigil_config_t now;
    CHECK(config_put_json("web", w82, strlen(w82), &e) == CONFIG_OK && !strcmp(g_lc_listen, "0.0.0.0:82") &&
          (config_get(&now), !strcmp(now.web.listen, "0.0.0.0:82")), "web.listen to a free port: saved");
    config_set_listen_check(NULL);

    /* net_listen_check: a port this process listens on is in use */
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof a;
    if (l >= 0 && bind(l, (struct sockaddr *)&a, sizeof a) == 0 && listen(l, 1) == 0 &&
        getsockname(l, (struct sockaddr *)&a, &al) == 0) {
        char lst[32], err[160] = "";
        snprintf(lst, sizeof lst, "127.0.0.1:%d", ntohs(a.sin_port));
        CHECK(net_listen_check(lst, err, sizeof err) < 0 && strstr(err, "in use"), "a port in use: %s", err);
        close(l);
        CHECK(net_listen_check(lst, err, sizeof err) == 0, "free again: %s", err);
        CHECK(net_listen_check("1.2.3.4:x", err, sizeof err) < 0 && net_listen_check("10.0.0.1x:80", err, sizeof err) < 0,
              "not listen settings");
    }
    unlink(path);
    rmdir(dir);
}

static void test_display_config(void)
{
    vigil_config_t c;
    config_defaults(&c);
    CHECK(!strcmp(c.display.output, "1280x720@60") && !strcmp(c.display.clock_position, "top-right") &&
          !strcmp(c.display.clock_size, "medium") && !strcmp(c.display.clock_background, "outline") &&
          !strcmp(c.display.name_position, "bottom-left") && !strcmp(c.display.name_background, "outline") &&
          !strcmp(c.display.icons_position, "bottom-right") && !strcmp(c.display.icons_background, "outline"),
          "display defaults: 720p60, an outlined clock at the top right, names bottom left, icons bottom right");
    CHECK(c.display.rec_icon == 1 && c.display.alarm_icons == 1 && c.display.tile_dividers == 1 &&
              c.display.alert_frame == 0,
          "display defaults: REC and alarm icons, lines between tiles, no alarm frame");

    unsigned w = 0, h = 0, hz = 0;
    CHECK(config_output_mode("1920x1080@50", &w, &h, &hz) == 0 && w == 1920 && h == 1080 && hz == 50,
          "1920x1080@50: %ux%u@%u", w, h, hz);
    CHECK(config_output_mode("1920x1080", &w, &h, &hz) < 0 && config_output_mode("1920x1080@60Hz", &w, &h, &hz) < 0 &&
          config_output_mode("0x1080@60", &w, &h, &hz) < 0 && config_output_mode("", &w, &h, &hz) < 0,
          "not modes: no rate, a unit, a zero, nothing");

    char dir[] = "/tmp/vigil-disp-XXXXXX", path[128], err[160];
    if (!mkdtemp(dir)) return;
    snprintf(path, sizeof path, "%s/vigil.yaml", dir);
    write_file(path, "display:\n"
                     "  output: 1920x1080@60\n"
                     "  clock_position: bottom-center\n"
                     "  clock_size: xlarge\n"
                     "  clock_background: box\n"
                     "  name_position: \"off\"\n"
                     "  icons_position: top-left\n"
                     "  icons_background: none\n"
                     "  rec_icon: false\n"
                     "  tile_dividers: false\n"
                     "  alert_frame: true\n");
    schema_errors_t e = { 0 };
    CHECK(config_load_file(path, &c, &e) == 0, "display read: %s %s", e.n ? e.e[0].path : "", e.n ? e.e[0].msg : "");
    CHECK(!strcmp(c.display.output, "1920x1080@60") && !strcmp(c.display.clock_position, "bottom-center") &&
          !strcmp(c.display.clock_size, "xlarge") && !strcmp(c.display.clock_background, "box") &&
          !strcmp(c.display.name_position, "off") && !strcmp(c.display.name_size, "medium") &&
          !strcmp(c.display.icons_position, "top-left") && !strcmp(c.display.icons_background, "none"),
          "display as written, the rest defaults");
    CHECK(!c.display.rec_icon && c.display.alarm_icons && !c.display.tile_dividers && c.display.alert_frame,
          "display switches as written, alarm_icons by default: %d %d %d %d", c.display.rec_icon,
          c.display.alarm_icons, c.display.tile_dividers, c.display.alert_frame);

    /* written back with its help, and read again the same */
    CHECK(config_save_file(path, &c, err, sizeof err) == 0, "save: %s", err);
    char *raw = read_file(path);
    CHECK(raw && strstr(raw, "\n# Display: The HDMI/VGA output") && strstr(raw, "\ndisplay:\n") &&
          strstr(raw, "  output: \"1920x1080@60\"\n") && strstr(raw, "  clock_background: box\n") &&
          strstr(raw, "  name_position: \"off\"\n") && strstr(raw, "  icons_background: none\n") &&
          strstr(raw, "  # Clock background: outline: hard-edged text") && strstr(raw, "  rec_icon: false\n") &&
          strstr(raw, "  alarm_icons: true\n") && strstr(raw, "  alert_frame: true\n"),
          "display in the YAML, with its help:\n%s", raw ? raw : "");
    free(raw);
    vigil_config_t d;
    memset(&e, 0, sizeof e);
    CHECK(config_load_file(path, &d, &e) == 0 && !memcmp(&c.display, &d.display, sizeof c.display),
          "display round trip");

    /* values outside the lists: refused, each by its path */
    static const struct { const char *yaml, *path; } bad[] = {
        { "display:\n  output: 800x600@60\n", "display.output" },
        { "display:\n  output: 1920x1080\n", "display.output" },
        { "display:\n  clock_position: middle\n", "display.clock_position" },
        { "display:\n  name_position: top-center\n", "display.name_position" },
        { "display:\n  icons_position: \"off\"\n", "display.icons_position" },
        { "display:\n  clock_size: huge\n", "display.clock_size" },
        { "display:\n  name_background: shadow\n", "display.name_background" },
        { "display:\n  alert_frame: sometimes\n", "display.alert_frame" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        write_file(path, bad[i].yaml);
        memset(&e, 0, sizeof e);
        CHECK(config_load_file(path, &c, &e) < 0 && e.n && !strcmp(e.e[0].path, bad[i].path),
              "%s refused at %s (%s)", bad[i].yaml, bad[i].path, e.n ? e.e[0].path : "no error");
    }

    /* the API's section: a partial one, the rest its defaults */
    write_file(path, "display:\n  output: 1920x1080@60\n");
    CHECK(config_init(path) == 0, "store");
    const char *body = "{\"clock_position\":\"top-left\",\"clock_background\":\"box\"}";
    memset(&e, 0, sizeof e);
    CHECK(config_put_json("display", body, strlen(body), &e) == CONFIG_OK, "PUT display");
    config_get(&c);
    CHECK(!strcmp(c.display.clock_position, "top-left") && !strcmp(c.display.clock_background, "box") &&
          !strcmp(c.display.output, "1280x720@60"), "PUT display: what it says, the rest defaults");
    char *j = config_get_json("display", 1);
    CHECK(j && strstr(j, "\"output\":\t\"1280x720@60\"") && strstr(j, "\"clock_position\":\t\"top-left\""),
          "GET display: %s", j ? j : "");
    free(j);
    body = "{\"output\":\"640x480@60\"}";
    memset(&e, 0, sizeof e);
    CHECK(config_put_json("display", body, strlen(body), &e) == CONFIG_EINVAL && !strcmp(e.e[0].path, "display.output"),
          "PUT display: a mode not in the list is refused");
    body = "{\"colour\":\"red\"}";
    memset(&e, 0, sizeof e);
    CHECK(config_put_json("display", body, strlen(body), &e) == CONFIG_EINVAL && !strcmp(e.e[0].path, "display.colour"),
          "PUT display: an unknown setting is refused");
    char cmd[80];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd)) {}
}

int main(int argc, char **argv)
{
    g_data = argc > 1 ? argv[1] : "tests/data";
    test_sps();
    test_au_norm();
    test_gop_cache();
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
    test_aspect_ratio();
    test_aspect_fit();
    test_aspect_scaled();
    test_overscan();
    test_picture();
    test_output_adj_config();
    test_aspect_fill();
    test_channel_tiles();
    test_channel_decode();
    test_sub_url();
    test_channel_substream();
    test_channel_instant_switch();
    test_stream_tap();
    test_channel_aspect();
    test_netip_title_parse();
    test_netip_server_proto();
    test_channel_stream_states();
    test_camera_slots();
    test_fail_kinds();
    test_channel_events();
    test_rec_policies();
    test_secrets();
    test_display_config();
    test_qos_config();
    test_qos_sockets();
    test_ports();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
