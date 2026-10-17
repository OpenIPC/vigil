/* SPDX-License-Identifier: GPL-3.0-or-later */
/* main.c — vigild: open DVR/NVR recorder.
 *
 *   hal_init (media system + HDMI on platforms that have it)
 *   -> channels_start (one thread per camera: source -> decode + record)
 *   -> frontend_run (LVGL quad view, or status lines when headless)
 *   -> SIGINT/SIGTERM: channels_stop, hal_shutdown.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "channel.h"
#include "frontend.h"
#include "hal.h"
#include "log.h"
#include "source.h"

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static void usage(const char *argv0)
{
    printf("usage: %s [-c URL]... [-o DIR|none] [-S SECS] [-K N] [-t SECS]\n"
           "  -c URL   camera source, in tile order (up to %d):\n"
           "             rtsp://[user:pass@]host[:554]/path\n"
           "             netip://[user:pass@]host[:34567]/[?ch=N&stream=main|extra]\n"
           "             capture:N   (local video input N, DVR boards)\n"
           "  -o DIR   record the streams here as received (default /tmp/rec; none = off)\n"
           "  -S SECS  segment length (default 60)\n"
           "  -K N     segments kept per camera (default 3; 0 = keep all)\n"
           "  -t SECS  exit after SECS seconds (default: run until SIGINT)\n",
           argv0, CHANNEL_MAX);
}

int main(int argc, char **argv)
{
    channels_config_t cfg = { .rec_dir = "/tmp/rec", .seg_secs = 60, .keep = 3 };
    int run_secs = 0;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "-c") && v) {
            if (cfg.n == CHANNEL_MAX) { fprintf(stderr, "at most %d cameras\n", CHANNEL_MAX); return 2; }
            cfg.urls[cfg.n++] = v;
        } else if (!strcmp(a, "-o") && v) {
            cfg.rec_dir = strcmp(v, "none") ? v : NULL;
        } else if (!strcmp(a, "-S") && v) {
            cfg.seg_secs = atoi(v);
        } else if (!strcmp(a, "-K") && v) {
            cfg.keep = atoi(v);
        } else if (!strcmp(a, "-t") && v) {
            run_secs = atoi(v);
        } else {
            usage(argv[0]);
            return strcmp(a, "-h") && strcmp(a, "--help") ? 2 : 0;
        }
        ++i;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (hal_init(SCREEN_W, SCREEN_H, 60) < 0) {
        vg_log("platform init failed");
        return 1;
    }
    const hal_caps_t *caps = hal_caps();
    vg_log("platform %s: display %s, decoder %s, capture inputs %d; %d camera(s)",
           caps->name, caps->display ? "yes" : "no", caps->decoder ? "yes" : "no",
           caps->capture, cfg.n);

    vg_rect_t tiles[CHANNEL_MAX];
    frontend_tiles(tiles, CHANNEL_MAX);
    int rc = 1;
    if (channels_start(&cfg, caps->display ? tiles : NULL) == 0) {
        rc = frontend_run(run_secs, &g_stop) < 0 ? 1 : 0;
        channels_stop();
    }
    hal_shutdown();
    vg_log("stopped");
    return rc;
}
