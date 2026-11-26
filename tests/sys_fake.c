/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys_fake.c — a fake system for src/sys (see sys_fake.h). */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sys.h"
#include "sys_fake.h"

int g_fail, g_checks;
char   fake_root[256];
char   fake_log[16384];
char   fake_hostname[256];
time_t fake_time;
char   fake_mounted[512];
char   fake_umounted[256];
int    fake_mount_errno, fake_umount_errno;
char   fake_stdin[8192];

#define ANSWERS 32
static struct { char prog[64]; int status; char out[1024]; } g_ans[ANSWERS];
static struct { char prog[64]; int there; } g_have[ANSWERS];
static struct { char ifname[16], addr[16], mask[16]; } g_if[8];

static void logf_(const char *line)
{
    size_t n = strlen(fake_log);
    snprintf(fake_log + n, sizeof fake_log - n, "%s\n", line);
}

static int f_run(const char *const *argv, const char *input, int timeout_s, char *out, size_t cap)
{
    (void)timeout_s;
    char line[1024] = "";
    for (int i = 0; argv[i]; ++i) {
        size_t n = strlen(line);
        snprintf(line + n, sizeof line - n, "%s%s", i ? " " : "", argv[i]);
    }
    logf_(line);
    snprintf(fake_stdin, sizeof fake_stdin, "%s", input ? input : "");
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    if (cap) out[0] = 0;
    for (int i = 0; i < ANSWERS; ++i)
        if (g_have[i].prog[0] && (!strcmp(g_have[i].prog, base) || !strcmp(g_have[i].prog, argv[0])) &&
            !g_have[i].there) {
            snprintf(out, cap, "%s: not found", argv[0]);
            return 127;
        }
    for (int i = 0; i < ANSWERS; ++i)
        if (g_ans[i].prog[0] && (!strcmp(g_ans[i].prog, base) || !strcmp(g_ans[i].prog, argv[0]))) {
            snprintf(out, cap, "%s", g_ans[i].out);
            return g_ans[i].status;
        }
    return 0;
}

static int f_have(const char *prog)
{
    const char *base = strrchr(prog, '/');
    base = base ? base + 1 : prog;
    for (int i = 0; i < ANSWERS; ++i)
        if (g_have[i].prog[0] && (!strcmp(g_have[i].prog, base) || !strcmp(g_have[i].prog, prog)))
            return g_have[i].there;
    return 1;
}

static int f_sethostname(const char *name, size_t len)
{
    snprintf(fake_hostname, sizeof fake_hostname, "%.*s", (int)len, name);
    char l[300];
    snprintf(l, sizeof l, "sethostname %s", fake_hostname);
    logf_(l);
    return 0;
}

static int f_settime(time_t t)
{
    fake_time = t;
    char l[64];
    snprintf(l, sizeof l, "settime %lld", (long long)t);
    logf_(l);
    return 0;
}

static int f_mount(const char *src, const char *target, const char *fstype, unsigned long flags,
                   const void *data)
{
    snprintf(fake_mounted, sizeof fake_mounted, "%s %s %s %lu %s", src, target, fstype ? fstype : "-",
             flags, data ? (const char *)data : "");
    char l[600];
    snprintf(l, sizeof l, "mount %s", fake_mounted);
    logf_(l);
    if (fake_mount_errno) {
        errno = fake_mount_errno;
        return -1;
    }
    return 0;
}

static int f_umount(const char *target, int flags)
{
    snprintf(fake_umounted, sizeof fake_umounted, "%s", target);
    char l[300];
    snprintf(l, sizeof l, "umount %s %d", target, flags);
    logf_(l);
    if (fake_umount_errno) {
        errno = fake_umount_errno;
        return -1;
    }
    return 0;
}

static int f_reread(const char *dev)
{
    char l[300];
    snprintf(l, sizeof l, "reread %s", dev);
    logf_(l);
    return 0;
}

static int f_ifaddr(const char *ifname, char *addr, char *mask, size_t cap)
{
    addr[0] = mask[0] = 0;
    for (int i = 0; i < 8; ++i)
        if (!strcmp(g_if[i].ifname, ifname)) {
            snprintf(addr, cap, "%s", g_if[i].addr);
            snprintf(mask, cap, "%s", g_if[i].mask);
            return 0;
        }
    return -1;
}

void fake_ifaddr(const char *ifname, const char *addr, const char *mask)
{
    for (int i = 0; i < 8; ++i)
        if (!g_if[i].ifname[0] || !strcmp(g_if[i].ifname, ifname)) {
            snprintf(g_if[i].ifname, sizeof g_if[i].ifname, "%s", ifname);
            snprintf(g_if[i].addr, sizeof g_if[i].addr, "%s", addr);
            snprintf(g_if[i].mask, sizeof g_if[i].mask, "%s", mask);
            return;
        }
}

void fake_init(void)
{
    if (fake_root[0]) fake_done();
    snprintf(fake_root, sizeof fake_root, "/tmp/vigil-sys-XXXXXX");
    if (!mkdtemp(fake_root)) {
        perror("mkdtemp");
        exit(1);
    }
    sys_set_root(fake_root);
    static const sys_ops_t ops = { .run = f_run, .have = f_have, .sethostname = f_sethostname,
                                   .settime = f_settime, .mount = f_mount, .umount = f_umount,
                                   .reread_parts = f_reread, .ifaddr = f_ifaddr };
    sys_set_ops(&ops);
    memset(g_ans, 0, sizeof g_ans);
    memset(g_have, 0, sizeof g_have);
    memset(g_if, 0, sizeof g_if);
    fake_log[0] = fake_hostname[0] = fake_mounted[0] = fake_umounted[0] = fake_stdin[0] = 0;
    fake_time = 0;
    fake_mount_errno = fake_umount_errno = 0;
}

void fake_done(void)
{
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", fake_root);
    if (fake_root[0] && system(cmd)) {}
    fake_root[0] = 0;
}

void fake_mkdir(const char *path)
{
    char p[512];
    snprintf(p, sizeof p, "%s%s", fake_root, path);
    for (char *s = p + strlen(fake_root) + 1; *s; ++s)
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
    mkdir(p, 0755);
}

void fake_file(const char *path, const char *text)
{
    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = 0;
        fake_mkdir(dir);
    }
    char p[512];
    snprintf(p, sizeof p, "%s%s", fake_root, path);
    FILE *f = fopen(p, "wb");
    if (!f) {
        perror(p);
        return;
    }
    fputs(text, f);
    fclose(f);
}

char *fake_read(const char *path)
{
    char p[512];
    snprintf(p, sizeof p, "%s%s", fake_root, path);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    char *b = calloc(1, 1 << 16);
    size_t n = b ? fread(b, 1, (1 << 16) - 1, f) : 0;
    (void)n;
    fclose(f);
    return b;
}

int fake_exists(const char *path)
{
    char p[512];
    struct stat st;
    snprintf(p, sizeof p, "%s%s", fake_root, path);
    return lstat(p, &st) == 0;
}

void fake_answer(const char *prog, int status, const char *output)
{
    for (int i = 0; i < ANSWERS; ++i)
        if (!g_ans[i].prog[0] || !strcmp(g_ans[i].prog, prog)) {
            snprintf(g_ans[i].prog, sizeof g_ans[i].prog, "%s", prog);
            g_ans[i].status = status;
            snprintf(g_ans[i].out, sizeof g_ans[i].out, "%s", output ? output : "");
            return;
        }
}

void fake_have(const char *prog, int there)
{
    for (int i = 0; i < ANSWERS; ++i)
        if (!g_have[i].prog[0] || !strcmp(g_have[i].prog, prog)) {
            snprintf(g_have[i].prog, sizeof g_have[i].prog, "%s", prog);
            g_have[i].there = there;
            return;
        }
}

int fake_ran(const char *substr) { return strstr(fake_log, substr) != NULL; }

void fake_clear_log(void) { fake_log[0] = 0; }

/* ---- no platform: the core's channels are never started here ----------------------- */

#include "hal.h"
#include "hal_capture.h"
#include "hal_decoder.h"
#include "hal_display.h"

static const hal_caps_t g_caps = { .name = "test" };
const hal_caps_t *hal_caps(void) { return &g_caps; }
int  hal_dec_open(int ch, vg_codec_t c, const vg_stream_info_t *si) { (void)ch; (void)c; (void)si; return -1; }
int  hal_dec_send(int ch, const vg_au_t *au) { (void)ch; (void)au; return -1; }
void hal_dec_reset(int ch) { (void)ch; }
void hal_dec_close(int ch) { (void)ch; }
int  hal_display_set_tile(int ch, const vg_rect_t *r) { (void)ch; (void)r; return 0; }
void hal_display_clear_tile(int ch) { (void)ch; }
int  hal_cap_start(int in, int ch, hal_cap_cb cb, void *user) { (void)in; (void)ch; (void)cb; (void)user; return -1; }
int  hal_cap_signal(int in) { (void)in; return 0; }
void hal_cap_stop(int in) { (void)in; }
