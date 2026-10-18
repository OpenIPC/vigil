/* SPDX-License-Identifier: GPL-3.0-or-later */
/* main.c — vigild: open DVR/NVR recorder.
 *
 *   hal_init (media system + HDMI on platforms that have it)
 *   -> channels_start (one thread per camera: source -> decode + record)
 *   -> frontend_run (LVGL quad view, or status lines when headless)
 *   -> SIGINT/SIGTERM: channels_stop, hal_shutdown.
 */
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "channel.h"
#include "frontend.h"
#include "hal.h"
#include "log.h"
#include "source.h"

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

/* ---- -s: everything written to stdout/stderr goes to syslog ---------------
 * Not only vg_log: the platforms and the vendor SDK libraries print to
 * stderr themselves, so the descriptors are redirected into a pipe and a
 * thread turns its lines into syslog messages (tag "vigil"). The init
 * script runs vigild with -s and its stdio on /dev/null, as majestic. */

static int       g_log_fd = -1;
static pthread_t g_log_thread;

static void *log_reader(void *arg)
{
    (void)arg;
    char buf[1024];
    size_t have = 0;
    for (;;) {
        ssize_t n = read(g_log_fd, buf + have, sizeof buf - 1 - have);
        if (n <= 0) break;
        have += (size_t)n;
        size_t start = 0;
        for (size_t i = 0; i < have; ++i) {
            if (buf[i] != '\n') continue;
            buf[i] = 0;
            const char *line = buf + start;
            if (!strncmp(line, "vigil: ", 7)) line += 7;     /* the tag says so */
            if (*line) syslog(LOG_INFO, "%s", line);
            start = i + 1;
        }
        if (start == 0 && have == sizeof buf - 1) {         /* overlong line */
            buf[have] = 0;
            syslog(LOG_INFO, "%s", buf);
            have = 0;
        } else {
            memmove(buf, buf + start, have - start);
            have -= start;
        }
    }
    if (have) {
        buf[have] = 0;
        syslog(LOG_INFO, "%s", buf);
    }
    return NULL;
}

static int log_to_syslog_start(void)
{
    int p[2];
    if (pipe(p) < 0) return -1;
    openlog("vigil", LOG_PID, LOG_DAEMON);
    fflush(stdout);
    fflush(stderr);
    if (dup2(p[1], STDOUT_FILENO) < 0 || dup2(p[1], STDERR_FILENO) < 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    close(p[1]);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    g_log_fd = p[0];
    if (pthread_create(&g_log_thread, NULL, log_reader, NULL) != 0) {
        close(g_log_fd);
        g_log_fd = -1;
        return -1;
    }
    return 0;
}

/* Flush what is left: closing our ends of the pipe ends the reader. */
static void log_to_syslog_stop(void)
{
    if (g_log_fd < 0) return;
    fflush(stdout);
    fflush(stderr);
    int null = open("/dev/null", O_WRONLY);
    if (null >= 0) {
        dup2(null, STDOUT_FILENO);
        dup2(null, STDERR_FILENO);
        close(null);
    }
    pthread_join(g_log_thread, NULL);
    close(g_log_fd);
    g_log_fd = -1;
    closelog();
}

static void usage(const char *argv0)
{
    printf("usage: %s [-c URL]... [-o DIR|none] [-S SECS] [-K N] [-t SECS] [-s]\n"
           "  -c URL   camera source, in tile order (up to %d):\n"
           "             rtsp://[user:pass@]host[:554]/path\n"
           "             netip://[user:pass@]host[:34567]/[?ch=N&stream=main|extra]\n"
           "             capture:N   (local video input N, DVR boards)\n"
           "  -o DIR   record the streams here as received (default /tmp/rec; none = off)\n"
           "  -S SECS  segment length (default 60)\n"
           "  -K N     segments kept per camera (default 3; 0 = keep all)\n"
           "  -t SECS  exit after SECS seconds (default: run until SIGINT)\n"
           "  -s       log to syslog (tag vigil) instead of stderr\n",
           argv0, CHANNEL_MAX);
}

int main(int argc, char **argv)
{
    channels_config_t cfg = { .rec_dir = "/tmp/rec", .seg_secs = 60, .keep = 3 };
    int run_secs = 0, use_syslog = 0;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "-s")) {
            use_syslog = 1;
            continue;
        }
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

    if (use_syslog && log_to_syslog_start() < 0)
        perror("vigild: syslog");

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (hal_init(SCREEN_W, SCREEN_H, 60) < 0) {
        vg_log("platform init failed");
        log_to_syslog_stop();
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
    log_to_syslog_stop();
    return rc;
}
