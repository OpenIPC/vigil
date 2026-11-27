/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys.h — vigild looking after the system it runs on.
 *
 * The management pages (network, time, host name, disks, network storage,
 * mail) change the operating system, not vigild: files in /etc, the
 * interfaces, the clock, mounts. src/sys does that the way OpenIPC
 * firmware does it (docs/management.md), in plain C and Linux, outside
 * src/core (which must stay POSIX and host-testable without a system to
 * change).
 *
 * Everything goes through two seams, so the host tests run every backend
 * against a fake system:
 *
 *   files     every path is taken under a root (sys_path): "" on the
 *             device, a temporary directory holding fake /etc, /sys, /proc
 *             and /dev files in the tests;
 *   actions   external commands (ifup, mkfs.*, ntpd, hwclock, curl, ...)
 *             and the system calls that change the system (sethostname,
 *             settimeofday, mount, ...) go through sys_ops_t, which the
 *             tests replace with recorders.
 *
 * Nothing here is used unless the system is one vigild may manage
 * (sys_managed): OpenIPC firmware, or a root given for tests. On a
 * desktop or a Debian board the pages say so instead of rewriting the
 * host's network.
 */
#ifndef SYS_H
#define SYS_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

typedef struct {
    /* Run argv[0] (a program name, looked up in the system's PATH) with
     * `input` on its stdin (NULL: nothing). stdout and stderr go to `out`
     * (cut at `cap`, NUL-terminated). Returns the exit status, 124 after
     * `timeout_s` (the program is killed), 127 when it is not there, -1
     * when it could not be started. */
    int (*run)(const char *const *argv, const char *input, int timeout_s, char *out, size_t cap);
    /* Is program `name` there (in /sbin, /usr/sbin, /bin, /usr/bin)? */
    int (*have)(const char *name);
    int (*sethostname)(const char *name, size_t len);
    int (*settime)(time_t t);
    int (*mount)(const char *src, const char *target, const char *fstype, unsigned long flags,
                 const void *data);
    int (*umount)(const char *target, int flags);
    /* The kernel re-reads a disk's partition table (BLKRRPART). */
    int (*reread_parts)(const char *dev);
    /* The IPv4 address and netmask of interface `ifname` ("" when none). */
    int (*ifaddr)(const char *ifname, char *addr, char *mask, size_t cap);
    /* The kernel's log (the newest `cap` - 1 bytes of it), as dmesg shows
     * it; length or -1. Some refusals (a mount's EINVAL) say why only
     * there. */
    int (*klog)(char *buf, size_t cap);
} sys_ops_t;

/* Replace the actions; NULL members (or a NULL ops) are the real ones. */
void sys_set_ops(const sys_ops_t *ops);
const sys_ops_t *sys_ops(void);

/* The root every path is taken under ("" = the real one). */
void        sys_set_root(const char *root);
const char *sys_root(void);

/* `path` (absolute) under the root, into `buf`. Returns buf. */
char *sys_path(char *buf, size_t cap, const char *path);

/* May vigild change this system? OpenIPC firmware (/etc/os-release names
 * an OPENIPC_VERSION), or any system once sys_set_root or
 * sys_set_managed(1) said so. */
int  sys_managed(void);
void sys_set_managed(int on);

/* Shorthands for sys_ops()->run with a NULL-terminated argument list. */
int sys_run(char *out, size_t cap, int timeout_s, const char *prog, ...);
int sys_have(const char *prog);

/* The file at `path` (under the root) into `buf`, NUL-terminated; its
 * length, or -1 (errno). */
long sys_read(const char *path, char *buf, size_t cap);
/* The first line of `path`, without the newline and trailing blanks; ""
 * when it cannot be read. */
char *sys_read_line(const char *path, char *buf, size_t cap);
long long sys_read_ll(const char *path, long long dflt);
int sys_exists(const char *path);

/* Replace `path` (under the root) with `data`: a new file beside it,
 * fsync, rename, fsync of the directory; `mode` for the new file. 0, or
 * -1 with `err`. */
int sys_write(const char *path, const char *data, size_t len, mode_t mode, char *err, size_t cap);

/* Cut trailing newlines and blanks off `s`. */
void sys_trim(char *s);

#endif /* SYS_H */
