/* SPDX-License-Identifier: GPL-3.0-or-later */
/* main.c — vigild: open DVR/NVR recorder.
 *
 *   config_init (-f FILE) + command-line overrides
 *   -> hal_init (media system + HDMI on platforms that have it)
 *   -> channels_start (one thread per camera: source -> decode + record)
 *   -> frontend_run (LVGL quad view, or status lines when headless); on
 *      this thread, a new configuration in the store is applied
 *      (channels_apply)
 *   -> SIGINT/SIGTERM: channels_stop, hal_shutdown.
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "channel.h"
#include "config.h"
#include "events.h"
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
static int       g_saved_out = -1, g_saved_err = -1;     /* the original stdio */
static pthread_t g_log_thread;

static void *log_reader(void *arg)
{
    (void)arg;
    char buf[1024];
    size_t have = 0;
    for (;;) {
        if (have == sizeof buf - 1) {                       /* overlong line */
            buf[have] = 0;
            syslog(LOG_INFO, "%s", buf);
            have = 0;
        }
        ssize_t n = read(g_log_fd, buf + have, sizeof buf - 1 - have);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;                                  /* EOF: every writer is gone */
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
        memmove(buf, buf + start, have - start);
        have -= start;
    }
    if (have) {
        buf[have] = 0;
        syslog(LOG_INFO, "%s", buf);
    }
    return NULL;
}

/* Put the original stdout/stderr back. This is also what ends the reader:
 * the pipe's last write ends were descriptors 1 and 2. */
static void restore_fd(int *saved, int fd)
{
    if (*saved < 0) return;
    int rc;
    while ((rc = dup2(*saved, fd)) < 0 && errno == EINTR)
        ;
    if (rc < 0) close(fd);              /* at least release the pipe's write end */
    close(*saved);
    *saved = -1;
}

static void restore_stdio(void)
{
    fflush(stdout);
    fflush(stderr);
    restore_fd(&g_saved_out, STDOUT_FILENO);
    restore_fd(&g_saved_err, STDERR_FILENO);
}

static int log_to_syslog_start(void)
{
    int p[2];
    g_saved_out = dup(STDOUT_FILENO);
    g_saved_err = dup(STDERR_FILENO);
    if (g_saved_out < 0 || g_saved_err < 0 || pipe(p) < 0) {
        if (g_saved_out >= 0) close(g_saved_out);
        if (g_saved_err >= 0) close(g_saved_err);
        g_saved_out = g_saved_err = -1;
        return -1;
    }
    fflush(stdout);
    fflush(stderr);
    if (dup2(p[1], STDOUT_FILENO) < 0 || dup2(p[1], STDERR_FILENO) < 0) {
        close(p[0]);
        close(p[1]);
        restore_stdio();
        return -1;
    }
    close(p[1]);
    g_log_fd = p[0];
    if (pthread_create(&g_log_thread, NULL, log_reader, NULL) != 0) {
        restore_stdio();                /* before anyone writes into a pipe nobody reads */
        close(g_log_fd);
        g_log_fd = -1;
        return -1;
    }
    openlog("vigil", LOG_PID, LOG_DAEMON);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    return 0;
}

/* Flush what is left and wait for the reader to log it. */
static void log_to_syslog_stop(void)
{
    if (g_log_fd < 0) return;
    restore_stdio();
    pthread_join(g_log_thread, NULL);
    close(g_log_fd);
    g_log_fd = -1;
    closelog();
}

static void usage(const char *argv0)
{
    printf("usage: %s [-f FILE] [-c URL]... [-o DIR|none] [-S SECS] [-K N] [-t SECS] [-s]\n"
           "  -f FILE  configuration (default %s; missing = defaults)\n"
           "  -c URL   camera source, in tile order (up to %d); replaces the file's cameras:\n"
           "             rtsp://[user:pass@]host[:554]/path\n"
           "             netip://[user:pass@]host[:34567]/[?ch=N&stream=main|extra]\n"
           "             capture:N   (local video input N, DVR boards)\n"
           "  -o DIR   record the streams here as received (none = off)\n"
           "  -S SECS  segment length (default 60)\n"
           "  -K N     segments kept per camera (default 3; 0 = keep all)\n"
           "  -t SECS  exit after SECS seconds (default: run until SIGINT)\n"
           "  -s       log to syslog (tag vigil) instead of stderr\n"
           "Options override the file for this run.\n",
           argv0, CONFIG_DEFAULT_PATH, CHANNEL_MAX);
}

/* -S / -K: out of range is brought into range, as the segment writer
 * always did with -K, rather than refusing to start an NVR over it. */
static int int_option(const char *opt, const char *v, const char *section, const char *key)
{
    const schema_field_t *f = schema_field(schema_section(&config_schema, section), key);
    int n = atoi(v);
    int c = n < f->min ? f->min : n > f->max ? f->max : n;
    if (c != n) vg_log("%s %s: out of range %d..%d, using %d", opt, v, f->min, f->max, c);
    return c;
}

/* ---- applying a new configuration (main thread) ------------------------------------ */

static unsigned g_applied_gen;

static void apply_config(void)
{
    if (config_generation() == g_applied_gen) return;
    static vigil_config_t c;            /* channels_config_t points into it */
    unsigned gen = config_get(&c);
    channels_config_t cc;
    config_to_channels(&c, &cc);
    int failed = channels_apply(&cc);
    g_applied_gen = gen;
    config_mark_applied(gen);
    if (failed) {
        vg_log("configuration %u applied; %d camera(s) could not start", gen, failed);
        events_push(0, "config: %d camera(s) failed", failed);
    } else {
        vg_log("configuration %u applied", gen);
        events_push(0, "config applied");
    }
}

int main(int argc, char **argv)
{
    const char *cfg_path = CONFIG_DEFAULT_PATH, *rec_dir = NULL, *seg = NULL, *keep = NULL;
    const char *urls[CHANNEL_MAX];
    int n_urls = 0, run_secs = 0, use_syslog = 0;

    /* -s first, so that what parsing the rest has to say (a bad option, too
     * many cameras) reaches syslog too: at boot stdio is /dev/null. */
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!strcmp(a, "-s"))
            use_syslog = 1;
        else if (a[0] == '-' && a[1] && !a[2] && strchr("fcoSKt", a[1]))
            ++i;                                    /* skip the option's value */
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (use_syslog && log_to_syslog_start() < 0)
        perror("vigild: cannot log to syslog, logging to stderr");

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "-s"))
            continue;
        if (!strcmp(a, "-c") && v) {
            if (n_urls == CHANNEL_MAX) {
                fprintf(stderr, "at most %d cameras\n", CHANNEL_MAX);
                log_to_syslog_stop();
                return 2;
            }
            urls[n_urls++] = v;
        } else if (!strcmp(a, "-f") && v) {
            cfg_path = v;
        } else if (!strcmp(a, "-o") && v) {
            rec_dir = v;
        } else if (!strcmp(a, "-S") && v) {
            seg = v;
        } else if (!strcmp(a, "-K") && v) {
            keep = v;
        } else if (!strcmp(a, "-t") && v) {
            run_secs = atoi(v);
        } else {
            usage(argv[0]);
            log_to_syslog_stop();
            return strcmp(a, "-h") && strcmp(a, "--help") ? 2 : 0;
        }
        ++i;
    }

    /* defaults < the file < the options */
    config_init(cfg_path);
    vigil_config_t cfg;
    config_get(&cfg);
    if (n_urls) {
        cfg.n_cameras = n_urls;
        for (int i = 0; i < n_urls; ++i) {
            config_camera_t *cam = &cfg.cameras[i];
            memset(cam, 0, sizeof *cam);
            snprintf(cam->url, sizeof cam->url, "%s", urls[i]);
            cam->enabled = 1;
        }
    }
    if (rec_dir) snprintf(cfg.record.path, sizeof cfg.record.path, "%s", strcmp(rec_dir, "none") ? rec_dir : "");
    if (seg) cfg.record.segment = int_option("-S", seg, "record", "segment");
    if (keep) cfg.record.keep = int_option("-K", keep, "record", "keep");
    schema_errors_t errs = { 0 };
    if (schema_validate(&config_schema, &cfg, &errs) < 0) {
        for (int i = 0; i < errs.n; ++i) {
            /* Only an option can be wrong here: the file was checked. */
            const char *p = errs.e[i].path;
            int cam = -1;
            if (sscanf(p, "cameras[%d]", &cam) == 1 && cam >= 0 && cam < n_urls) {
                char red[512];
                schema_redact_url(urls[cam], red, sizeof red);
                vg_log("-c %s: %s", red, errs.e[i].msg);
            } else {
                vg_log("%s: %s", p, errs.e[i].msg);
            }
        }
        log_to_syslog_stop();
        return 2;
    }
    config_override(&cfg);

    if (hal_init(SCREEN_W, SCREEN_H, 60) < 0) {
        vg_log("platform init failed");
        log_to_syslog_stop();
        return 1;
    }
    const hal_caps_t *caps = hal_caps();
    vg_log("platform %s: display %s, decoder %s, capture inputs %d; %d camera(s)",
           caps->name, caps->display ? "yes" : "no", caps->decoder ? "yes" : "no",
           caps->capture, cfg.n_cameras);

    vg_rect_t tiles[CHANNEL_MAX];
    frontend_tiles(tiles, CHANNEL_MAX);
    int rc = 1;
    static vigil_config_t running;      /* channels_config_t points into it */
    g_applied_gen = config_get(&running);
    channels_config_t cc;
    config_to_channels(&running, &cc);
    if (channels_start(&cc, caps->display ? tiles : NULL) == 0) {
        config_mark_applied(g_applied_gen);
        rc = frontend_run(run_secs, &g_stop, apply_config) < 0 ? 1 : 0;
        channels_stop();
    }
    hal_shutdown();
    vg_log("stopped");
    log_to_syslog_stop();
    return rc;
}
