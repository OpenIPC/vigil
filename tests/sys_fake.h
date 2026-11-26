/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys_fake.h — src/sys on a fake system, for the host tests: a temporary
 * root with whatever /etc, /sys, /proc and /dev files a test writes, and
 * a recorder in place of every command and system call (sys_ops_t).
 *
 *   fake_init()               a fresh root (sys_set_root), the recorder on
 *   fake_file(path, text)     a file under the root (directories made)
 *   fake_answer(prog, status, output)
 *                             what the next runs of `prog` answer (until
 *                             changed; default 0 and "")
 *   fake_ran(fmt...)          was a command line run (argv joined by
 *                             spaces, matched as a substring)?
 *   fake_log                  every command and call, one per line
 */
#ifndef SYS_FAKE_H
#define SYS_FAKE_H

#include <stddef.h>
#include <time.h>

extern int g_fail, g_checks;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_fail; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

extern char fake_root[256];
extern char fake_log[16384];

/* The last sethostname / settime / mount (data included) / umount. */
extern char   fake_hostname[256];
extern time_t fake_time;
extern char   fake_mounted[512];
extern char   fake_umounted[256];
extern int    fake_mount_errno;     /* nonzero: mount() fails with it */
extern int    fake_umount_errno;
/* What the last run of each program got on stdin (one buffer, the last). */
extern char   fake_stdin[8192];

void  fake_init(void);
void  fake_done(void);                  /* removes the root */
void  fake_file(const char *path, const char *text);
char *fake_read(const char *path);      /* malloc'd, NULL when missing */
int   fake_exists(const char *path);
void  fake_mkdir(const char *path);
void  fake_answer(const char *prog, int status, const char *output);
/* `prog` is (not) installed (sys_have); every program is by default. */
void  fake_have(const char *prog, int there);
int   fake_ran(const char *substr);
void  fake_clear_log(void);
/* An IPv4 address for fake interface `ifname` (ops->ifaddr). */
void  fake_ifaddr(const char *ifname, const char *addr, const char *mask);

#endif
