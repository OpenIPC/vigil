/* SPDX-License-Identifier: GPL-3.0-or-later */
/* vg_random.h — random bytes from the kernel, and whether they can be
 * trusted yet.
 *
 * Until the kernel's pool is initialised (on a board without a hardware
 * generator, the first tens of seconds after power-on, more on an idle
 * one or in emulation), /dev/urandom hands out bytes that may be
 * predictable, and Linux logs "random: <process>: uninitialized urandom
 * read". getrandom(2) says EAGAIN instead. So:
 *
 *   VG_RAND_STRONG — keys, salts, nonces a server checks, session tokens:
 *     the kernel's bytes or nothing. Never blocks; -1 with errno EAGAIN
 *     until the pool is ready, and the caller refuses (a login is asked
 *     to come back) rather than settling for less.
 *   VG_RAND_WEAK — values that only have to differ (message ids, cache
 *     keys, a client's nonce sent to a camera): the kernel's bytes when it
 *     has them, before that a mix of the clocks, the pid and a counter.
 *     Never blocks, never fails, never reads /dev/urandom early.
 *
 * Where the C library or the kernel has no getrandom (kernels before 3.17,
 * old uClibc), readiness is /dev/random being readable and the bytes come
 * from /dev/urandom: that kernel has no other notion of "initialised", and
 * reading /dev/random itself would drain its estimate on every login. */
#ifndef VG_RANDOM_H
#define VG_RANDOM_H

#include <stddef.h>

typedef enum {
    VG_RAND_WEAK = 0,
    VG_RAND_STRONG = 1,
} vg_rand_t;

/* `n` bytes into `buf`. STRONG: 0, or -1 with errno (EAGAIN: the pool is
 * not initialised yet; anything else: no source at all). WEAK: 0, always. */
int vg_random(void *buf, size_t n, vg_rand_t how);

/* STRONG, but waiting for the pool up to `timeout_ms` (< 0: as long as it
 * takes): 0, or -1 with errno (EAGAIN: still not ready). Says in the log
 * that it waits, once. Only on a thread that may wait: never the UI's or
 * a camera's while vigild starts. */
int vg_random_wait(void *buf, size_t n, int timeout_ms);

/* 1 once the pool is initialised (and from then on), 0 before. Never
 * blocks. */
int vg_random_ready(void);

/* Tests: a source in place of getrandom(2), with its contract: the bytes
 * given (all of `n` here), or -1 with errno; EAGAIN when `nonblock` and
 * the pool is not ready; without `nonblock` it may wait. NULL: the
 * kernel's again. Also forgets that the pool was seen ready. */
typedef long (*vg_random_source_fn)(void *buf, size_t n, int nonblock);
void vg_random_set_source(vg_random_source_fn fn);

#endif /* VG_RANDOM_H */
