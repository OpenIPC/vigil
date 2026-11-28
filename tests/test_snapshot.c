/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_snapshot.c — snapshot.c's choices and the recorder's own pictures,
 * on a faked platform: a decoder picture (video range, a known pattern)
 * for channel 1, none for channel 2. Writes the JPEGs it made into DIR
 * (nvr.jpg at 704x576, full.jpg at 1920x1080) for snapshot_test.py to
 * decode with PIL.
 *
 *   test_snapshot DIR
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "config.h"
#include "hal.h"
#include "hal_capture.h"
#include "hal_decoder.h"
#include "hal_display.h"
#include "snap_cam.h"
#include "snap_store.h"
#include "snapshot.h"

static int g_fail, g_checks;
#define CHECK(c, ...)                                                                \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(c)) {                                                                  \
            ++g_fail;                                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                     \
            fprintf(stderr, __VA_ARGS__);                                            \
            fputc('\n', stderr);                                                     \
        } else {                                                                     \
            printf("ok   ");                                                         \
            printf(__VA_ARGS__);                                                     \
            putchar('\n');                                                           \
        }                                                                            \
    } while (0)

/* ---- the platform, faked ----------------------------------------------------------- */

static const hal_caps_t g_caps = { .name = "test", .display = 1, .decoder = 1, .max_channels = 4 };
const hal_caps_t *hal_caps(void) { return &g_caps; }
int  hal_dec_open(int ch, vg_codec_t c, const vg_stream_info_t *si) { (void)ch; (void)c; (void)si; return -1; }
int  hal_dec_send(int ch, const vg_au_t *au) { (void)ch; (void)au; return -1; }
void hal_dec_reset(int ch) { (void)ch; }
void hal_dec_close(int ch) { (void)ch; }
int  hal_display_set_tile(int ch, const vg_rect_t *r) { (void)ch; (void)r; return 0; }
void hal_display_clear_tile(int ch) { (void)ch; }
void hal_display_set_aspect(int ch, unsigned w, unsigned h, vg_aspect_t how) { (void)ch; (void)w; (void)h; (void)how; }
int  hal_cap_start(int in, int ch, hal_cap_cb cb, void *user) { (void)in; (void)ch; (void)cb; (void)user; return -1; }
void hal_cap_stop(int in) { (void)in; }
int  hal_cap_signal(int in) { (void)in; return 0; }

/* Channel 1's decoder: g_w x g_h, Y from 16 (left) to 235 (right) in video
 * range, grey, with a red block (Cr 240) in the top left quarter of it.
 * Channel 2 has none. */
static unsigned g_w = 704, g_h = 576;
static int      g_stills;
int hal_display_still(int ch, hal_pic_t *pic, int timeout_ms)
{
    (void)timeout_ms;
    ++g_stills;
    if (ch != 0) return -1;
    unsigned w = g_w, h = g_h;
    unsigned char *y = malloc((size_t)w * h * 3 / 2), *u = y + (size_t)w * h, *v = u + (size_t)(w / 2) * (h / 2);
    for (unsigned r = 0; r < h; ++r)
        for (unsigned x = 0; x < w; ++x) y[(size_t)r * w + x] = (unsigned char)(16 + x * 219 / (w - 1));
    for (unsigned r = 0; r < h / 2; ++r)
        for (unsigned x = 0; x < w / 2; ++x) {
            int red = r < h / 8 && x < w / 8;
            u[(size_t)r * (w / 2) + x] = 128;
            v[(size_t)r * (w / 2) + x] = red ? 240 : 128;
        }
    *pic = (hal_pic_t){ y, u, v, w, h, w, w / 2 };
    return 0;
}

static void write_file(const char *path, const void *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(p, 1, n, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : ".";
    mkdir(out, 0755);
    char path[512], text[1024], rec[512];
    /* The recordings (and so the snapshots) beside the output, on the
     * build's disk: tmpfs is refused, as on a board. */
    snprintf(rec, sizeof rec, "%s/rec/cams", out);
    snprintf(path, sizeof path, "%s/vigil.yaml", out);
    /* Nobody at 127.0.0.1:1: the camera's ONVIF is refused at once. */
    snprintf(text, sizeof text,
             "version: 1\ncameras:\n"
             "  - url: \"rtsp://admin:pw@127.0.0.1:1/main\"\n"
             "    onvif_url: \"http://127.0.0.1:1/onvif/device_service\"\n"
             "  - url: \"rtsp://admin:pw@127.0.0.1:1/other\"\n"
             "    onvif_url: \"http://127.0.0.1:1/onvif/device_service\"\n"
             "  - url: \"rtsp://127.0.0.1:1/off\"\n    enabled: false\n"
             "record:\n  path: %s\n  allow_rootfs: true\nsnapshot:\n  quality: 85\n", rec);
    write_file(path, text, strlen(text));
    if (config_init(path) != 0) {
        fprintf(stderr, "config_init %s failed\n", path);
        return 1;
    }

    unsigned char *jpeg;
    size_t len;
    char used[32], err[300];
    int status = 0;
    unsigned w, h;

    CHECK(snapshot_take(0, SNAPSHOT_NVR, &jpeg, &len, used, sizeof used, &status, err, sizeof err) == 0 &&
              !strcmp(used, "nvr"), "nvr: the decoder's picture (%s)", used);
    size_t n = len;
    CHECK(snap_jpeg_check(jpeg, &n, &w, &h) == 0 && w == 704 && h == 576, "nvr: a JPEG at the decoder's size (%ux%u)",
          w, h);
    snprintf(path, sizeof path, "%s/nvr.jpg", out);
    write_file(path, jpeg, len);
    free(jpeg);

    int before = g_stills;
    CHECK(snapshot_take(0, SNAPSHOT_AUTO, &jpeg, &len, used, sizeof used, &status, err, sizeof err) == 0 &&
              !strcmp(used, "nvr") && g_stills == before + 1,
          "auto: the camera has no snapshot (refused), the decoder's then (%s)", used);
    free(jpeg);
    CHECK(snapshot_take(0, SNAPSHOT_CAMERA, &jpeg, &len, used, sizeof used, &status, err, sizeof err) < 0 &&
              status >= 502 && strstr(err, "the camera"),
          "camera: none, said (%d %s)", status, err);
    CHECK(snapshot_take(1, SNAPSHOT_NVR, &jpeg, &len, used, sizeof used, &status, err, sizeof err) < 0 &&
              status == 503 && strstr(err, "not being decoded"),
          "nvr of a channel with no decoder: 503 (%s)", err);
    CHECK(snapshot_take(1, SNAPSHOT_AUTO, &jpeg, &len, used, sizeof used, &status, err, sizeof err) < 0 &&
              status == 503 && strstr(err, "the camera") && strstr(err, "not being decoded"),
          "auto, neither: both reasons (%s)", err);
    CHECK(snapshot_take(2, SNAPSHOT_AUTO, &jpeg, &len, used, sizeof used, &status, err, sizeof err) < 0 &&
              status == 404, "a disabled camera: 404");
    CHECK(snapshot_take(7, SNAPSHOT_AUTO, &jpeg, &len, used, sizeof used, &status, err, sizeof err) < 0 &&
              status == 404, "no camera: 404");

    char name[SNAP_NAME_MAX];
    CHECK(snapshot_save(0, SNAPSHOT_NVR, "manual", name, sizeof name, used, sizeof used, &status, err, sizeof err) ==
              0, "kept: %s", name);
    snprintf(path, sizeof path, "%s/rec/snapshots/%s", out, name);
    CHECK(access(path, R_OK) == 0, "in the directory next to the recordings: %s", path);

    /* The encoder's cost at the size a full-screen main stream decodes to. */
    g_w = 1920;
    g_h = 1080;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int ok = snapshot_take(0, SNAPSHOT_NVR, &jpeg, &len, used, sizeof used, &status, err, sizeof err) == 0;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    CHECK(ok, "1920x1080: %zu KiB in %ld ms (fake picture and encode, this host)", len >> 10,
          (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
    if (ok) {
        snprintf(path, sizeof path, "%s/full.jpg", out);
        write_file(path, jpeg, len);
        free(jpeg);
    }
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
