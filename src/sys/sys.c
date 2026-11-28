/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys.c — the seams of src/sys (see sys.h): the root, the runner and the
 * system calls. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/fs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include "sys.h"

extern char **environ;

/* busybox puts its applets in all four; the firmware's own tools are in
 * /usr/sbin. */
static const char *const PATH_DIRS[] = { "/usr/sbin", "/usr/bin", "/sbin", "/bin" };

static char g_root[256];
static int  g_managed = -1;             /* -1: not decided yet */

void sys_set_root(const char *root)
{
    snprintf(g_root, sizeof g_root, "%s", root ? root : "");
    size_t n = strlen(g_root);
    while (n && g_root[n - 1] == '/') g_root[--n] = 0;
    if (g_root[0]) g_managed = 1;
}

const char *sys_root(void) { return g_root; }

char *sys_path(char *buf, size_t cap, const char *path)
{
    snprintf(buf, cap, "%s%s", g_root, path);
    return buf;
}

void sys_set_managed(int on) { g_managed = on; }

int sys_managed(void)
{
    if (g_managed < 0) {
        char buf[2048];
        g_managed = (sys_read("/etc/os-release", buf, sizeof buf) > 0 && strstr(buf, "OPENIPC_VERSION=")) ||
                    (sys_read("/usr/lib/os-release", buf, sizeof buf) > 0 && strstr(buf, "OPENIPC_VERSION="));
    }
    return g_managed;
}

/* ---- files ------------------------------------------------------------------------- */

long sys_read(const char *path, char *buf, size_t cap)
{
    char p[512];
    if (!cap) return -1;
    buf[0] = 0;
    int fd = open(sys_path(p, sizeof p, path), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t n = 0;
    while (n < cap - 1) {
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = 0;
    return (long)n;
}

void sys_trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

char *sys_read_line(const char *path, char *buf, size_t cap)
{
    if (sys_read(path, buf, cap) < 0) {
        if (cap) buf[0] = 0;
        return buf;
    }
    buf[strcspn(buf, "\n")] = 0;
    sys_trim(buf);
    return buf;
}

long long sys_read_ll(const char *path, long long dflt)
{
    char b[64], *end;
    if (sys_read(path, b, sizeof b) <= 0) return dflt;
    long long v = strtoll(b, &end, 10);
    return end == b ? dflt : v;
}

int sys_exists(const char *path)
{
    char p[512];
    struct stat st;
    return stat(sys_path(p, sizeof p, path), &st) == 0;
}

int sys_write(const char *path, const char *data, size_t len, mode_t mode, char *err, size_t cap)
{
    char p[512], tmp[540], dir[512];
    sys_path(p, sizeof p, path);
    snprintf(tmp, sizeof tmp, "%s.XXXXXX", p);
    int fd = mkstemp(tmp);
    if (fd < 0) {
        snprintf(err, cap, "cannot create a file next to %s: %s", path, strerror(errno));
        return -1;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int ok = fchmod(fd, mode) == 0;
    for (size_t done = 0; ok && done < len;) {
        ssize_t n = write(fd, data + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) ok = 0;
        else done += (size_t)n;
    }
    if (ok) ok = fsync(fd) == 0;
    if (close(fd) < 0) ok = 0;
    if (!ok || rename(tmp, p) < 0) {
        snprintf(err, cap, "cannot write %s: %s", path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    snprintf(dir, sizeof dir, "%s", p);
    int dfd = open(dirname(dir), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
    return 0;
}

/* ---- the real actions ------------------------------------------------------------------ */

static int real_have(const char *name)
{
    char p[512];
    if (strchr(name, '/')) return access(sys_path(p, sizeof p, name), X_OK) == 0;
    for (size_t i = 0; i < sizeof PATH_DIRS / sizeof PATH_DIRS[0]; ++i) {
        char rel[256];
        snprintf(rel, sizeof rel, "%s/%s", PATH_DIRS[i], name);
        if (access(sys_path(p, sizeof p, rel), X_OK) == 0) return 1;
    }
    return 0;
}

static int which(const char *name, char *out, size_t cap)
{
    if (strchr(name, '/')) {
        snprintf(out, cap, "%s", name);
        return access(out, X_OK) == 0 ? 0 : -1;
    }
    for (size_t i = 0; i < sizeof PATH_DIRS / sizeof PATH_DIRS[0]; ++i) {
        snprintf(out, cap, "%s/%s", PATH_DIRS[i], name);
        if (access(out, X_OK) == 0) return 0;
    }
    return -1;
}

static long long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* posix_spawn rather than fork: vigild has many threads, and the child
 * of a fork may only call async-signal-safe functions before exec. */
static int real_run(const char *const *argv, const char *input, int timeout_s, char *out, size_t cap)
{
    char prog[512];
    if (cap) out[0] = 0;
    if (which(argv[0], prog, sizeof prog) < 0) {
        snprintf(out, cap, "%s: not found", argv[0]);
        return 127;
    }
    int op[2] = { -1, -1 }, ip[2] = { -1, -1 };
    if (pipe2(op, O_CLOEXEC) < 0 || pipe2(ip, O_CLOEXEC) < 0) {
        if (op[0] >= 0) { close(op[0]); close(op[1]); }
        return -1;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, ip[0], 0);
    posix_spawn_file_actions_adddup2(&fa, op[1], 1);
    posix_spawn_file_actions_adddup2(&fa, op[1], 2);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, prog, &fa, &at, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(ip[0]);
    close(op[1]);
    if (rc != 0) {
        close(ip[1]);
        close(op[0]);
        snprintf(out, cap, "%s: %s", argv[0], strerror(rc));
        return -1;
    }
    /* The input is small (a mail, a configuration): written before the
     * output is read, and the pipe holds it. */
    if (input) {
        size_t len = strlen(input);
        for (size_t done = 0; done < len;) {
            ssize_t n = write(ip[1], input + done, len - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            done += (size_t)n;
        }
    }
    close(ip[1]);
    size_t got = 0;
    long long deadline = mono_ms() + (long long)(timeout_s > 0 ? timeout_s : 30) * 1000;
    int timed_out = 0;
    for (;;) {
        long long left = deadline - mono_ms();
        if (left <= 0) {
            timed_out = 1;
            break;
        }
        struct pollfd pfd = { op[0], POLLIN, 0 };
        int pr = poll(&pfd, 1, (int)(left > 1000 ? 1000 : left));
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) continue;
        char buf[512];
        ssize_t n = read(op[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;                              /* EOF: every writer is gone */
        for (ssize_t i = 0; i < n && got + 1 < cap; ++i) out[got++] = buf[i];
    }
    close(op[0]);
    if (cap) out[got < cap ? got : cap - 1] = 0;
    if (timed_out) kill(-pid, SIGKILL);
    int status = 0;
    for (;;) {
        pid_t w = waitpid(pid, &status, timed_out ? 0 : WNOHANG);
        if (w == pid) break;
        if (w < 0 && errno != EINTR) return -1;
        if (w == 0) {
            if (mono_ms() > deadline) {
                kill(-pid, SIGKILL);
                timed_out = 1;
            } else {
                usleep(20 * 1000);
            }
        }
    }
    if (timed_out) return 124;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

static int real_sethostname(const char *name, size_t len) { return sethostname(name, len); }

static int real_settime(time_t t)
{
    struct timeval tv = { t, 0 };
    return settimeofday(&tv, NULL);
}

static int real_mount(const char *src, const char *target, const char *fstype, unsigned long flags,
                      const void *data)
{
    return mount(src, target, fstype, flags, data);
}

static int real_umount(const char *target, int flags) { return umount2(target, flags); }

static int real_reread_parts(const char *dev)
{
    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = ioctl(fd, BLKRRPART);
    int e = errno;
    close(fd);
    errno = e;
    return rc;
}

static int real_ifaddr(const char *ifname, char *addr, char *mask, size_t cap)
{
    addr[0] = mask[0] = 0;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifname);
    int rc = -1;
    if (ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)(void *)&ifr.ifr_addr)->sin_addr, addr, (socklen_t)cap);
        if (ioctl(fd, SIOCGIFNETMASK, &ifr) == 0)
            inet_ntop(AF_INET, &((struct sockaddr_in *)(void *)&ifr.ifr_netmask)->sin_addr, mask,
                      (socklen_t)cap);
        rc = 0;
    }
    close(fd);
    return rc;
}

static int real_klog(char *buf, size_t cap)
{
    if (cap < 2) return -1;
    int size = klogctl(10 /* SYSLOG_ACTION_SIZE_BUFFER */, NULL, 0);
    if (size <= 0) size = 1 << 17;
    char *all = malloc((size_t)size + 1);
    if (!all) return -1;
    int n = klogctl(3 /* SYSLOG_ACTION_READ_ALL */, all, size);
    if (n < 0) {
        free(all);
        return -1;
    }
    size_t keep = (size_t)n < cap - 1 ? (size_t)n : cap - 1;
    memcpy(buf, all + n - keep, keep);
    buf[keep] = 0;
    free(all);
    return (int)keep;
}

static pid_t real_spawn(const char *const *argv, const char *log)
{
    char prog[512], lp[512];
    if (which(argv[0], prog, sizeof prog) < 0) {
        errno = ENOENT;
        return -1;
    }
    int fd = open(sys_path(lp, sizeof lp, log), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fd, 1);
    posix_spawn_file_actions_adddup2(&fa, fd, 2);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    /* Its own session: nothing aimed at vigild's process group (a
     * terminal's hang-up, a supervisor) reaches it. A C library without
     * POSIX_SPAWN_SETSID still gives it a process group of its own. */
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSID);
#else
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
#endif
    pid_t pid;
    int rc = posix_spawn(&pid, prog, &fa, &at, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(fd);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return pid;
}

static const sys_ops_t REAL = {
    .run = real_run, .have = real_have, .sethostname = real_sethostname, .settime = real_settime,
    .mount = real_mount, .umount = real_umount, .reread_parts = real_reread_parts, .ifaddr = real_ifaddr, .klog = real_klog,
    .spawn = real_spawn,
};

static sys_ops_t g_ops = REAL;

void sys_set_ops(const sys_ops_t *ops)
{
    g_ops = REAL;
    if (!ops) return;
#define TAKE(m) if (ops->m) g_ops.m = ops->m
    TAKE(run); TAKE(have); TAKE(sethostname); TAKE(settime); TAKE(mount); TAKE(umount);
    TAKE(reread_parts); TAKE(ifaddr); TAKE(klog); TAKE(spawn);
#undef TAKE
}

const sys_ops_t *sys_ops(void) { return &g_ops; }

int sys_run(char *out, size_t cap, int timeout_s, const char *prog, ...)
{
    const char *argv[24];
    int n = 0;
    argv[n++] = prog;
    va_list ap;
    va_start(ap, prog);
    for (const char *a; n < 23 && (a = va_arg(ap, const char *)) != NULL;) argv[n++] = a;
    va_end(ap);
    argv[n] = NULL;
    char dummy[8];
    return g_ops.run(argv, NULL, timeout_s, out ? out : dummy, out ? cap : sizeof dummy);
}

int sys_have(const char *prog) { return g_ops.have(prog); }
