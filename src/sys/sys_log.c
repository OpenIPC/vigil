/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys_log.c — syslog's ring and the kernel's log, read (see sys_log.h). */
#define _DEFAULT_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "sys.h"
#include "sys_log.h"

static const char *const LEVELS[8] = { "emerg", "alert", "crit", "err", "warning", "notice", "info", "debug" };

static time_t (*g_now)(void);

void sys_log_set_clock(time_t (*now)(void)) { g_now = now; }

const char *sys_log_level_name(int level) { return level >= 0 && level < 8 ? LEVELS[level] : ""; }

static int level_of(const char *name, size_t n)
{
    for (int i = 0; i < 8; ++i)
        if (strlen(LEVELS[i]) == n && !strncmp(LEVELS[i], name, n)) return i;
    if (n == 5 && !strncmp(name, "error", 5)) return 3;
    if (n == 4 && !strncmp(name, "warn", 4)) return 4;
    if (n == 5 && !strncmp(name, "panic", 5)) return 0;
    return -1;
}

static void put_text(sys_logline_t *o, const char *p, const char *end)
{
    while (p < end && *p == ' ') ++p;
    size_t n = (size_t)(end - p);
    if (n >= sizeof o->text) n = sizeof o->text - 1;
    memcpy(o->text, p, n);
    o->text[n] = 0;
    for (char *c = o->text; *c; ++c)
        if ((unsigned char)*c < 0x20) *c = ' ';
}

/* "tag[pid]: text" or "tag: text": the tag (without its pid) out of `p`. */
static const char *take_tag(sys_logline_t *o, const char *p, const char *end)
{
    const char *q = p;
    while (q < end && *q != ':' && *q != ' ') ++q;
    if (q < end && *q == ':' && q > p) {
        size_t n = (size_t)(q - p);
        const char *br = memchr(p, '[', n);
        if (br) n = (size_t)(br - p);
        if (n >= sizeof o->tag) n = sizeof o->tag - 1;
        memcpy(o->tag, p, n);
        o->tag[n] = 0;
        return q + 1;
    }
    return p;
}

int sys_log_parse_syslog(const char *line, size_t len, time_t now, sys_logline_t *out)
{
    memset(out, 0, sizeof *out);
    out->level = -1;
    const char *p = line, *end = line + len;
    while (end > p && (end[-1] == '\n' || end[-1] == '\r')) --end;
    if (p == end) return -1;
    /* "Mmm dd HH:MM:SS " */
    static const char *const MON = "JanFebMarAprMayJunJulAugSepOctNovDec";
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    int mon = -1, day, hh, mm, ss;
    if (len >= 16) {
        for (int i = 0; i < 12; ++i)
            if (!strncmp(p, MON + i * 3, 3)) mon = i;
    }
    if (mon >= 0 && sscanf(p + 3, "%d %d:%d:%d", &day, &hh, &mm, &ss) == 4 && p[15] == ' ') {
        struct tm nt;
        localtime_r(&now, &nt);
        tm.tm_year = nt.tm_year;
        tm.tm_mon = mon;
        tm.tm_mday = day;
        tm.tm_hour = hh;
        tm.tm_min = mm;
        tm.tm_sec = ss;
        tm.tm_isdst = -1;
        time_t t = mktime(&tm);
        if (t > now + 86400) {           /* December's lines read in January */
            tm.tm_year -= 1;
            tm.tm_isdst = -1;
            t = mktime(&tm);
        }
        out->time = t;
        p += 16;
        /* "host fac.pri " (not with syslogd -S) */
        const char *h = p, *he = memchr(h, ' ', (size_t)(end - h));
        const char *f = he ? he + 1 : NULL, *fe = f ? memchr(f, ' ', (size_t)(end - f)) : NULL;
        const char *dot = f && fe ? memchr(f, '.', (size_t)(fe - f)) : NULL;
        int lv = dot ? level_of(dot + 1, (size_t)(fe - dot - 1)) : -1;
        if (lv >= 0) {
            out->level = lv;
            p = fe + 1;
        }
    }
    p = take_tag(out, p, end);
    put_text(out, p, end);
    return 0;
}

int sys_log_parse_kernel(const char *line, size_t len, time_t boot, sys_logline_t *out)
{
    memset(out, 0, sizeof *out);
    out->level = -1;
    snprintf(out->tag, sizeof out->tag, "kernel");
    const char *p = line, *end = line + len;
    while (end > p && (end[-1] == '\n' || end[-1] == '\r')) --end;
    if (p == end) return -1;
    if (*p == '<') {
        char *e;
        long v = strtol(p + 1, &e, 10);
        if (e < end && *e == '>') {
            out->level = (int)(v & 7);
            p = e + 1;
        }
    }
    if (p < end && *p == '[') {
        char *e;
        double secs = strtod(p + 1, &e);
        const char *close = memchr(p, ']', (size_t)(end - p));
        if (close && e <= close) {
            if (boot) out->time = boot + (time_t)secs;
            p = close + 1;
        }
    }
    put_text(out, p, end);
    return 0;
}

static int has_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; ++hay)
        if (!strncasecmp(hay, needle, n)) return 1;
    return n == 0;
}

/* The newest SYS_LOG_MAX bytes of `path` (under the root): malloc'd, or NULL. */
static char *tail_of(const char *path, char *why, size_t cap)
{
    char p[512];
    sys_path(p, sizeof p, path);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(why, cap, "no logread, and %s: %s", path, strerror(errno));
        return NULL;
    }
    off_t size = lseek(fd, 0, SEEK_END);
    off_t from = size > (off_t)SYS_LOG_MAX ? size - (off_t)SYS_LOG_MAX : 0;
    char *buf = malloc(SYS_LOG_MAX + 1);
    ssize_t n = buf ? pread(fd, buf, (size_t)(size - from), from) : -1;
    close(fd);
    if (n < 0) {
        free(buf);
        snprintf(why, cap, "cannot read %s", path);
        return NULL;
    }
    buf[n] = 0;
    return buf;
}

static time_t boot_time(time_t now)
{
    char line[64];
    sys_read_line("/proc/uptime", line, sizeof line);
    double up = atof(line);
    return up > 0 ? now - (time_t)up : 0;
}

int sys_log_read(sys_log_src_t src, int max_level, const char *text, time_t from, time_t to, int offset, int max,
                 sys_logline_t *out, int *more, char *why, size_t cap)
{
    if (more) *more = 0;
    why[0] = 0;
    char *buf = NULL;
    if (src == SYS_LOG_KERNEL) {
        buf = malloc(SYS_LOG_MAX);
        if (!buf || sys_ops()->klog(buf, SYS_LOG_MAX) < 0) {
            snprintf(why, cap, "the kernel's log cannot be read: %s", buf ? strerror(errno) : "out of memory");
            free(buf);
            return -1;
        }
    } else {
        buf = malloc(SYS_LOG_MAX);
        if (!buf) {
            snprintf(why, cap, "out of memory");
            return -1;
        }
        int rc = sys_run(buf, SYS_LOG_MAX, 5, "logread", NULL);
        if (rc != 0) {
            free(buf);
            buf = tail_of("/var/log/messages", why, cap);
            if (!buf) return -1;
        }
    }
    time_t now = g_now ? g_now() : time(NULL), boot = src == SYS_LOG_KERNEL ? boot_time(now) : 0;
    int n = 0, skip = offset < 0 ? 0 : offset;
    char *end = buf + strlen(buf);
    /* Newest first: the lines from the end. */
    while (end > buf) {
        char *e = end;
        if (e[-1] == '\n') --e;
        char *s = e;
        while (s > buf && s[-1] != '\n') --s;
        end = s;
        sys_logline_t l;
        int ok = src == SYS_LOG_KERNEL ? sys_log_parse_kernel(s, (size_t)(e - s), boot, &l)
                                       : sys_log_parse_syslog(s, (size_t)(e - s), now, &l);
        if (ok < 0) continue;
        if (max_level >= 0 && l.level > max_level) continue;
        if (l.time && ((from && l.time < from) || (to && l.time > to))) continue;
        if (text && *text && !has_ci(l.text, text) && !has_ci(l.tag, text)) continue;
        if (skip > 0) {
            --skip;
            continue;
        }
        if (n >= max) {
            if (more) *more = 1;
            break;
        }
        out[n++] = l;
    }
    free(buf);
    return n;
}
