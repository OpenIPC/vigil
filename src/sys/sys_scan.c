/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys_scan.c — the surface scan (sys_scan.h). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include "sys.h"
#include "sys_ata.h"
#include "sys_scan.h"

#define ATA_READ_VERIFY_EXT 0x42
#define VERIFY_TIMEOUT_MS   30000   /* a weak sector takes the drive seconds of retries */
#define BUF_MAX             (2u * 1024 * 1024)
#define SPEED_WINDOW_NS     2000000000LL
#define IOPRIO_CLASS_IDLE   3
#define IOPRIO_WHO_PROCESS  1

static const double LIMITS[] = SCAN_LIMITS;

/* Zero-initialised (BSS): the status alone is a few KiB the binary
 * need not carry. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_wake = PTHREAD_COND_INITIALIZER;

static struct {
    pthread_t       thread;
    int             live;           /* a thread to join */
    int             starting;       /* a scan_start between its checks and its thread */
    int             stop, pause;
    scan_status_t   st;
    scan_req_t      req;
    int             fd, direct;
    uint64_t        per_pass;       /* blocks */
    long long       t0, paused_ns, pause_from;
    long long       win_t0;
    uint64_t        win_bytes;
} G;

static scan_hook_t g_hook;

void scan_set_hook(scan_hook_t h) { g_hook = h; }

static long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

const char *scan_mode_name(int mode)
{
    return mode == SCAN_VERIFY ? "verify" : mode == SCAN_WRITE ? "write" : "read";
}

int scan_mode_parse(const char *s)
{
    if (!s) return -1;
    if (!strcmp(s, "read")) return SCAN_READ;
    if (!strcmp(s, "verify")) return SCAN_VERIFY;
    if (!strcmp(s, "write")) return SCAN_WRITE;
    return -1;
}

const char *scan_state_name(int state)
{
    static const char *const N[] = { "idle", "running", "paused", "done", "stopped", "failed" };
    return state >= 0 && state <= SCAN_FAILED ? N[state] : "idle";
}

int scan_class_of(double ms)
{
    for (int i = 0; i < (int)(sizeof LIMITS / sizeof LIMITS[0]); ++i)
        if (ms < LIMITS[i]) return i;
    return SCAN_CSLOW;
}

uint64_t scan_disk_sectors(const char *disk, unsigned *sector)
{
    char rel[128];
    snprintf(rel, sizeof rel, "/sys/block/%s/queue/logical_block_size", disk);
    long long ss = sys_read_ll(rel, 512);
    if (ss < 512 || ss > 65536 || (ss & (ss - 1))) ss = 512;
    snprintf(rel, sizeof rel, "/sys/block/%s/size", disk);
    long long n512 = sys_read_ll(rel, 0);     /* always in 512-byte units */
    if (sector) *sector = (unsigned)ss;
    return n512 > 0 ? (uint64_t)n512 * 512u / (uint64_t)ss : 0;
}

/* ---- status --------------------------------------------------------------------------- */

static long active_s(long long now)
{
    long long t = now - G.t0 - G.paused_ns;
    if (G.st.state == SCAN_PAUSED && G.pause_from) t -= now - G.pause_from;
    return t > 0 ? (long)(t / 1000000000LL) : 0;
}

void scan_status(scan_status_t *out)
{
    pthread_mutex_lock(&g_lock);
    *out = G.st;
    if (G.st.state == SCAN_RUNNING || G.st.state == SCAN_PAUSED) {
        long long now = now_ns();
        out->elapsed = active_s(now);
        if (out->blocks_done && out->elapsed > 0)
            out->eta = (long)((double)out->elapsed / (double)out->blocks_done *
                              (double)(out->blocks_total - out->blocks_done));
        else out->eta = -1;
    }
    pthread_mutex_unlock(&g_lock);
}

int scan_active(const char *disk)
{
    pthread_mutex_lock(&g_lock);
    int on = (G.st.state == SCAN_RUNNING || G.st.state == SCAN_PAUSED) && (!disk || !strcmp(disk, G.st.disk));
    int mode = on ? G.st.mode : -1;
    pthread_mutex_unlock(&g_lock);
    return mode;
}

int scan_pause(int on)
{
    pthread_mutex_lock(&g_lock);
    int ok = G.st.state == SCAN_RUNNING || G.st.state == SCAN_PAUSED;
    if (ok) G.pause = on;
    pthread_cond_broadcast(&g_wake);
    pthread_mutex_unlock(&g_lock);
    return ok ? 0 : -1;
}

int scan_stop(void)
{
    pthread_mutex_lock(&g_lock);
    int ok = G.st.state == SCAN_RUNNING || G.st.state == SCAN_PAUSED;
    if (ok) G.stop = 1;
    pthread_cond_broadcast(&g_wake);
    pthread_mutex_unlock(&g_lock);
    return ok ? 0 : -1;
}

void scan_shutdown(void)
{
    pthread_mutex_lock(&g_lock);
    G.stop = 1;
    pthread_cond_broadcast(&g_wake);
    int live = G.live;
    G.live = 0;
    pthread_mutex_unlock(&g_lock);
    if (live) pthread_join(G.thread, NULL);
}

/* ---- one block ----------------------------------------------------------------------- */

enum { IO_OK = 0, IO_ERR = -1, IO_GONE = -2, IO_UNSUP = -3 };

static int gone(int e) { return e == ENODEV || e == ENXIO || e == ENOMEDIUM; }

static int do_read(uint8_t *buf, uint64_t lba, unsigned n, char *why, size_t cap)
{
    size_t len = (size_t)n * G.st.sector;
    off_t off = (off_t)(lba * G.st.sector);
    ssize_t r = pread(G.fd, buf, len, off);
    int e = errno;
    if (!G.direct) posix_fadvise(G.fd, off, (off_t)len, POSIX_FADV_DONTNEED);
    if (r == (ssize_t)len) return IO_OK;
    if (r < 0) snprintf(why, cap, "read error at LBA %llu: %s", (unsigned long long)lba, strerror(e));
    else snprintf(why, cap, "short read at LBA %llu", (unsigned long long)lba);
    return r < 0 && gone(e) ? IO_GONE : IO_ERR;
}

static int do_verify(uint64_t lba, unsigned n, char *why, size_t cap)
{
    ata_tf_t tf = { .command = ATA_READ_VERIFY_EXT, .ext = 1, .lba = lba, .device = 0x40,
                    .count = (uint16_t)(n >= SCAN_BLOCK_MAX ? 0 : n) };
    int rc = ata_cmd(G.fd, &tf, ATA_NONDATA, NULL, 0, VERIFY_TIMEOUT_MS, why, cap);
    if (rc == ATA_OK) return IO_OK;
    if (rc == ATA_EUNSUP) return IO_UNSUP;
    return IO_ERR;
}

/* Each sector: its LBA, the scan's nonce, then a fixed byte. */
static void pattern(uint8_t *buf, uint64_t lba, unsigned n, uint64_t nonce)
{
    unsigned ss = G.st.sector;
    for (unsigned i = 0; i < n; ++i) {
        uint8_t *p = buf + (size_t)i * ss;
        uint64_t l = lba + i;
        memset(p, 0xA5, ss);
        for (int b = 0; b < 8; ++b) {
            p[b] = (uint8_t)(l >> (8 * b));
            p[8 + b] = (uint8_t)(nonce >> (8 * b));
        }
    }
}

static int do_write(uint8_t *buf, uint64_t lba, unsigned n, uint64_t nonce, char *why, size_t cap)
{
    size_t len = (size_t)n * G.st.sector;
    pattern(buf, lba, n, nonce);
    ssize_t r = pwrite(G.fd, buf, len, (off_t)(lba * G.st.sector));
    int e = errno;
    if (r == (ssize_t)len) return IO_OK;
    if (r < 0) snprintf(why, cap, "write error at LBA %llu: %s", (unsigned long long)lba, strerror(e));
    else snprintf(why, cap, "short write at LBA %llu", (unsigned long long)lba);
    return r < 0 && gone(e) ? IO_GONE : IO_ERR;
}

static int do_compare(uint8_t *buf, uint8_t *want, uint64_t lba, unsigned n, uint64_t nonce, char *why, size_t cap)
{
    int rc = do_read(buf, lba, n, why, cap);
    if (rc != IO_OK) return rc;
    pattern(want, lba, n, nonce);
    if (memcmp(buf, want, (size_t)n * G.st.sector)) {
        snprintf(why, cap, "what was written at LBA %llu reads back different", (unsigned long long)lba);
        return IO_ERR;
    }
    return IO_OK;
}

/* ---- the thread -------------------------------------------------------------------------- */

static void add_bad(uint64_t lba, uint64_t n)
{
    scan_status_t *s = &G.st;
    for (int i = 0; i < s->n_bad; ++i) {
        scan_range_t *r = &s->bad[i];
        if (lba >= r->lba && lba + n <= r->lba + r->count) return;   /* known (Write's second pass) */
        if (r->lba + r->count == lba) {
            r->count += n;
            return;
        }
    }
    if (s->n_bad < SCAN_BAD_MAX) s->bad[s->n_bad++] = (scan_range_t){ lba, n };
    else ++s->bad_more;
}

/* One block's result into the status (g_lock held). */
static void account(uint64_t lba, unsigned n, int cls, long long now)
{
    scan_status_t *s = &G.st;
    ++s->counts[cls];
    uint64_t bi = (lba - s->start) / s->block;
    uint64_t cell = G.per_pass ? bi * (uint64_t)s->cells / G.per_pass : 0;
    if (cell < (uint64_t)s->cells && s->map[cell] < cls + 1) s->map[cell] = (uint8_t)(cls + 1);
    if (cls == SCAN_CERR) add_bad(lba, n);
    s->lba = lba + n;
    ++s->blocks_done;
    G.win_bytes += (uint64_t)n * s->sector;
    long long dt = now - G.win_t0;
    if (dt >= SPEED_WINDOW_NS / 4 && dt > 0) {
        double m = (double)G.win_bytes / ((double)dt / 1e9) / 1e6;
        s->mbps = s->mbps > 0 ? s->mbps * 0.5 + m * 0.5 : m;
        G.win_t0 = now;
        G.win_bytes = 0;
    }
}

/* Paused: wait (g_lock held). Returns 1 to stop. */
static int wait_while_paused(void)
{
    while (G.pause && !G.stop) {
        if (G.st.state != SCAN_PAUSED) {
            G.st.state = SCAN_PAUSED;
            G.pause_from = now_ns();
        }
        pthread_cond_wait(&g_wake, &g_lock);
    }
    if (G.st.state == SCAN_PAUSED) {
        long long now = now_ns();
        G.paused_ns += now - G.pause_from;
        G.pause_from = 0;
        G.st.state = SCAN_RUNNING;
        G.win_t0 = now;
        G.win_bytes = 0;
    }
    return G.stop;
}

static void summary(char *out, size_t cap, const scan_status_t *s)
{
    /* Errors are counted per block read or written (Write's two passes
     * may count one block twice); the ranges are the bad LBAs. */
    uint64_t slow = s->counts[SCAN_C500] + s->counts[SCAN_CSLOW];
    unsigned long long ranges = (unsigned long long)s->n_bad + s->bad_more;
    snprintf(out, cap, "%s of %s %s: %llu error%s in %llu bad range%s, %llu slow (>= 150 ms)%s",
             scan_mode_name(s->mode), s->disk,
             s->state == SCAN_DONE ? "done" : s->state == SCAN_STOPPED ? "stopped" : "failed",
             (unsigned long long)s->counts[SCAN_CERR], s->counts[SCAN_CERR] == 1 ? "" : "s", ranges,
             ranges == 1 ? "" : "s", (unsigned long long)slow,
             s->read_instead ? "; read, as the device does not pass READ VERIFY" : "");
}

static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_lock);
    int mode = G.st.mode, idle = G.req.idle;
    uint64_t start = G.st.start, end = G.st.end;
    unsigned block = G.st.block;
    int passes = G.st.passes;
    pthread_mutex_unlock(&g_lock);

    /* Recording first: the scan's reads wait while the disk is busy.
     * Only the CFQ scheduler honours it; ATA commands (Verify) bypass
     * the scheduler altogether. */
    if (idle) syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO_CLASS_IDLE << 13);

    size_t bytes = (size_t)block * G.st.sector;
    uint8_t *buf = NULL, *want = NULL;
    int ok = mode == SCAN_VERIFY || posix_memalign((void **)&buf, 4096, bytes) == 0;
    if (ok && mode == SCAN_WRITE) ok = posix_memalign((void **)&want, 4096, bytes) == 0;
    char why[200] = "", fail[200] = "";
    int verify = mode == SCAN_VERIFY;
    if (ok && verify) {
        /* Does anything pass READ VERIFY? One sector at the start. */
        if (do_verify(start, 1, why, sizeof why) == IO_UNSUP) {
            verify = 0;
            ok = posix_memalign((void **)&buf, 4096, bytes) == 0;
            pthread_mutex_lock(&g_lock);
            G.st.read_instead = 1;
            snprintf(G.st.msg, sizeof G.st.msg, "The device does not pass READ VERIFY: reading instead");
            pthread_mutex_unlock(&g_lock);
        }
    }
    if (!ok) snprintf(fail, sizeof fail, "out of memory for a %zu-byte block", bytes);
    uint64_t nonce = (uint64_t)time(NULL) << 16 ^ (uint64_t)getpid();

    for (int pass = 1; ok && pass <= passes && !fail[0]; ++pass) {
        pthread_mutex_lock(&g_lock);
        G.st.pass = pass;
        G.st.lba = start;
        if (mode == SCAN_WRITE)
            snprintf(G.st.msg, sizeof G.st.msg, "%s", pass == 1 ? "Pass 1 of 2: writing the pattern"
                                                                : "Pass 2 of 2: reading it back");
        pthread_mutex_unlock(&g_lock);
        int stopped = 0;
        for (uint64_t lba = start; lba < end; lba += block) {
            pthread_mutex_lock(&g_lock);
            stopped = wait_while_paused();
            pthread_mutex_unlock(&g_lock);
            if (stopped) break;
            unsigned n = end - lba < block ? (unsigned)(end - lba) : block;
            long long t0 = now_ns();
            int rc;
            why[0] = 0;
            if (mode == SCAN_WRITE) rc = pass == 1 ? do_write(buf, lba, n, nonce, why, sizeof why)
                                                   : do_compare(buf, want, lba, n, nonce, why, sizeof why);
            else if (verify) rc = do_verify(lba, n, why, sizeof why);
            else rc = do_read(buf, lba, n, why, sizeof why);
            long long t1 = now_ns();
            double ms = (double)(t1 - t0) / 1e6;
            if (g_hook && g_hook(mode, pass, lba, n, &ms) < 0 && rc == IO_OK) {
                rc = IO_ERR;
                snprintf(why, sizeof why, "error at LBA %llu (test)", (unsigned long long)lba);
            }
            /* Unplugged: the reads fail with EIO as often as ENODEV, but
             * the disk is gone from sysfs. */
            if (rc == IO_ERR) {
                char rel[64];
                snprintf(rel, sizeof rel, "/sys/block/%s", G.st.disk);
                if (!sys_exists(rel)) rc = IO_GONE;
            }
            if (rc == IO_GONE) {
                snprintf(fail, sizeof fail, "the disk went away: %.170s", why);
                break;
            }
            pthread_mutex_lock(&g_lock);
            account(lba, n, rc == IO_OK ? scan_class_of(ms) : SCAN_CERR, t1);
            pthread_mutex_unlock(&g_lock);
        }
        if (stopped) break;
        /* The written pattern onto the platters before it is read back. */
        if (mode == SCAN_WRITE && pass == 1) fsync(G.fd);
    }

    if (mode == SCAN_WRITE) fsync(G.fd);
    close(G.fd);
    if (mode == SCAN_WRITE) {
        /* The partition table is gone: the kernel forgets the old ones. */
        char rel[64], p[512];
        snprintf(rel, sizeof rel, "/dev/%s", G.st.disk);
        sys_ops()->reread_parts(sys_path(p, sizeof p, rel));
    }
    free(buf);
    free(want);

    pthread_mutex_lock(&g_lock);
    G.fd = -1;
    long long t_end = now_ns();
    if (G.pause_from) {                     /* stopped while paused */
        G.paused_ns += t_end - G.pause_from;
        G.pause_from = 0;
    }
    long long active = t_end - G.t0 - G.paused_ns;
    G.st.elapsed = active_s(t_end);
    G.st.eta = 0;
    /* Done: the average over the whole scan. */
    if (active > 0)
        G.st.mbps = (double)G.st.blocks_done * G.st.block * G.st.sector / ((double)active / 1e9) / 1e6;
    G.st.state = fail[0] ? SCAN_FAILED : G.stop ? SCAN_STOPPED : SCAN_DONE;
    G.st.finished = time(NULL);
    if (fail[0]) snprintf(G.st.msg, sizeof G.st.msg, "%s", fail);
    else summary(G.st.msg, sizeof G.st.msg, &G.st);
    scan_status_t copy = G.st;
    void (*done)(const scan_status_t *) = G.req.done;
    pthread_mutex_unlock(&g_lock);
    if (done) done(&copy);
    return NULL;
}

int scan_start(const scan_req_t *r, char *err, size_t cap)
{
    err[0] = 0;
    if (r->mode < SCAN_READ || r->mode > SCAN_WRITE) {
        snprintf(err, cap, "no such mode");
        return -1;
    }
    if (!r->disk[0] || strchr(r->disk, '/')) {
        snprintf(err, cap, "no such disk");
        return -1;
    }
    unsigned sector = 512;
    uint64_t sectors = scan_disk_sectors(r->disk, &sector);
    if (!sectors) {
        snprintf(err, cap, "%s has no medium, or is not there", r->disk);
        return -1;
    }
    unsigned block = r->block ? r->block : 256;
    uint64_t end = r->end ? r->end : sectors;
    if (block > SCAN_BLOCK_MAX || (r->mode != SCAN_VERIFY && (size_t)block * sector > BUF_MAX)) {
        snprintf(err, cap, "a block of at most %u sectors", r->mode == SCAN_VERIFY ? SCAN_BLOCK_MAX
                                                                                    : BUF_MAX / sector);
        return -1;
    }
    if (end > sectors || r->start >= end) {
        snprintf(err, cap, "the range must be within 0..%llu, the start before the end",
                 (unsigned long long)sectors);
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    if (G.st.state == SCAN_RUNNING || G.st.state == SCAN_PAUSED || G.starting) {
        snprintf(err, cap, "a scan of %s is under way", G.starting ? "a disk" : G.st.disk);
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    int live = G.live;
    G.live = 0;
    G.starting = 1;
    pthread_mutex_unlock(&g_lock);
    if (live) pthread_join(G.thread, NULL);

    char rel[64], p[512];
    snprintf(rel, sizeof rel, "/dev/%s", r->disk);
    sys_path(p, sizeof p, rel);
    struct stat sb;
    if (stat(p, &sb) < 0) {
        snprintf(err, cap, "cannot open %s: %s", rel, strerror(errno));
        pthread_mutex_lock(&g_lock);
        G.starting = 0;
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    /* A block device: no page cache, and for writing exclusively, which
     * the kernel refuses while a partition of it is mounted. A plain
     * file (the tests' fake disk) as it is. */
    int blk = S_ISBLK(sb.st_mode);
    int flags = (r->mode == SCAN_WRITE ? O_RDWR : O_RDONLY) | O_CLOEXEC;
    if (blk) flags |= O_DIRECT | (r->mode == SCAN_WRITE ? O_EXCL : 0);
    int fd = open(p, flags);
    if (fd < 0) {
        int e = errno;
        if (e == EBUSY) snprintf(err, cap, "%s is in use (mounted?): it cannot be written", r->disk);
        else snprintf(err, cap, "cannot open %s: %s", rel, strerror(e));
        pthread_mutex_lock(&g_lock);
        G.starting = 0;
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    G.starting = 0;
    G.req = *r;
    G.fd = fd;
    G.direct = blk;
    G.stop = G.pause = 0;
    memset(&G.st, 0, sizeof G.st);
    scan_status_t *s = &G.st;
    s->state = SCAN_RUNNING;
    s->mode = r->mode;
    snprintf(s->disk, sizeof s->disk, "%s", r->disk);
    snprintf(s->model, sizeof s->model, "%s", r->model);
    snprintf(s->serial, sizeof s->serial, "%s", r->serial);
    s->start = r->start;
    s->end = end;
    s->block = block;
    s->sector = sector;
    s->passes = r->mode == SCAN_WRITE ? 2 : 1;
    s->pass = 1;
    s->lba = r->start;
    G.per_pass = (end - r->start + block - 1) / block;
    s->blocks_total = G.per_pass * (uint64_t)s->passes;
    s->cells = G.per_pass < SCAN_MAP ? (int)G.per_pass : SCAN_MAP;
    s->eta = -1;
    s->started = time(NULL);
    G.t0 = G.win_t0 = now_ns();
    G.paused_ns = G.pause_from = 0;
    G.win_bytes = 0;
    int rc = pthread_create(&G.thread, NULL, worker, NULL);
    if (rc) {
        s->state = SCAN_FAILED;
        snprintf(s->msg, sizeof s->msg, "cannot start a thread");
        close(fd);
        G.fd = -1;
    } else {
        G.live = 1;
    }
    pthread_mutex_unlock(&g_lock);
    if (rc) {
        snprintf(err, cap, "cannot start a thread");
        return -1;
    }
    return 0;
}
