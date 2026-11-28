/* SPDX-License-Identifier: GPL-3.0-or-later */
/* vg_random.c — random bytes from the kernel (see vg_random.h). */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "log.h"
#include "vg_random.h"

#ifndef GRND_NONBLOCK
#define GRND_NONBLOCK 0x0001            /* linux/random.h; the kernel's ABI */
#endif

#define POLL_MS 50                      /* between two looks at a pool that is not ready */

static _Atomic(vg_random_source_fn) g_source;
static atomic_int g_ready;              /* the pool was seen initialised */
static atomic_int g_told;               /* "waiting" was logged */

/* The system call itself rather than the C library's getrandom(): glibc
 * has that only from 2.25, uClibc and older toolchains not at all, while
 * the number is in any kernel headers from 3.17 on. Without it (or on an
 * older kernel, ENOSYS at run time) the devices stand in. */
static long sys_getrandom(void *buf, size_t n, int nonblock)
{
#ifdef SYS_getrandom
    return syscall(SYS_getrandom, buf, n, nonblock ? GRND_NONBLOCK : 0);
#else
    (void)buf;
    (void)n;
    (void)nonblock;
    errno = ENOSYS;
    return -1;
#endif
}

/* All of `n` (getrandom may give fewer on a signal): 0, or -1 with errno. */
static int from_getrandom(unsigned char *b, size_t n, int nonblock)
{
    vg_random_source_fn src = atomic_load(&g_source);
    size_t done = 0;
    while (done < n) {
        long r = src ? src(b + done, n - done, nonblock) : sys_getrandom(b + done, n - done, nonblock);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            if (r == 0) errno = EIO;
            return -1;
        }
        done += (size_t)r;
    }
    return 0;
}

/* No getrandom: ready once /dev/random is readable (waiting up to
 * `timeout_ms`, < 0 for ever), the bytes from /dev/urandom. */
static int from_devices(unsigned char *b, size_t n, int timeout_ms)
{
    if (!atomic_load(&g_ready)) {
        int fd = open("/dev/random", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) return -1;
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int r;
        do r = poll(&p, 1, timeout_ms);
        while (r < 0 && errno == EINTR);
        int e = errno;
        close(fd);
        if (r <= 0) {
            errno = r == 0 ? EAGAIN : e;
            return -1;
        }
        atomic_store(&g_ready, 1);
    }
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, b + done, n - done);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        done += (size_t)r;
    }
    close(fd);
    if (done < n) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static long elapsed_ms(const struct timespec *t0)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)(t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

/* The kernel's bytes: not waiting (`timeout_ms` 0), waiting up to it, or
 * (< 0) for as long as the pool takes. 0, or -1 with errno. */
static int strong(unsigned char *b, size_t n, int timeout_ms)
{
    int rc = from_getrandom(b, n, timeout_ms >= 0);
    if (rc < 0 && errno == ENOSYS && !atomic_load(&g_source)) return from_devices(b, n, timeout_ms);
    if (rc < 0 && errno == EAGAIN && timeout_ms > 0) {
        /* getrandom cannot wait for a while only: look again until then. */
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            long left = timeout_ms - elapsed_ms(&t0);
            if (left <= 0) {
                errno = EAGAIN;
                break;
            }
            struct timespec nap = { 0, (left < POLL_MS ? left : POLL_MS) * 1000000L };
            nanosleep(&nap, NULL);
            if ((rc = from_getrandom(b, n, 1)) == 0 || errno != EAGAIN) break;
        }
    }
    if (rc == 0) atomic_store(&g_ready, 1);
    return rc;
}

/* Not secret, only different each time: the clocks, the pid and a
 * counter through splitmix64. */
static void mix(unsigned char *b, size_t n)
{
    static atomic_ullong ctr;
    struct timespec m, r;
    clock_gettime(CLOCK_MONOTONIC, &m);
    clock_gettime(CLOCK_REALTIME, &r);
    uint64_t s = ((uint64_t)m.tv_sec * 1000000000u + (uint64_t)m.tv_nsec) ^
                 ((uint64_t)r.tv_sec << 30 ^ (uint64_t)r.tv_nsec) ^ (uint64_t)getpid() << 48 ^
                 atomic_fetch_add(&ctr, 0x9e3779b97f4a7c15ULL);
    for (size_t i = 0; i < n; i += 8) {
        uint64_t z = (s += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        z ^= z >> 31;
        for (size_t k = 0; k < 8 && i + k < n; ++k) b[i + k] = (unsigned char)(z >> (8 * k));
    }
}

int vg_random(void *buf, size_t n, vg_rand_t how)
{
    if (!n) return 0;
    if (strong(buf, n, 0) == 0) return 0;
    if (how == VG_RAND_STRONG) return -1;
    mix(buf, n);
    return 0;
}

int vg_random_wait(void *buf, size_t n, int timeout_ms)
{
    if (!n) return 0;
    if (strong(buf, n, 0) == 0) return 0;
    if (errno != EAGAIN || timeout_ms == 0) return -1;
    if (!atomic_exchange(&g_told, 1)) {
        if (timeout_ms < 0) vg_log("random: the kernel's pool is not initialised yet; waiting for it");
        else vg_log("random: the kernel's pool is not initialised yet; waiting up to %d ms for it", timeout_ms);
    }
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = strong(buf, n, timeout_ms);
    if (rc == 0) vg_log("random: the kernel's pool is ready (waited %ld ms)", elapsed_ms(&t0));
    else if (errno == EAGAIN) atomic_store(&g_told, 0);     /* said again by the next one that waits */
    return rc;
}

int vg_random_ready(void)
{
    if (atomic_load(&g_ready)) return 1;
    unsigned char x;
    return strong(&x, 1, 0) == 0;
}

void vg_random_set_source(vg_random_source_fn fn)
{
    atomic_store(&g_source, fn);
    atomic_store(&g_ready, 0);
    atomic_store(&g_told, 0);
}
