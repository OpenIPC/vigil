/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys_time.c — clock, zone, NTP and host name (see sys_time.h). */
#define _DEFAULT_SOURCE
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "sys.h"
#include "sys_time.h"

/* The strings are tzdata's own (2025), as `tail -1` of each zoneinfo file
 * prints them; zones whose rules change often are left to custom. */
const tz_zone_t tz_zones[] = {
    { "Etc/GMT", "GMT0" },
    { "UTC", "UTC0" },
    { "Europe/London", "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0" },
    { "Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Prague", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Bucharest", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Riga", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Istanbul", "<+03>-3" },
    { "Europe/Minsk", "<+03>-3" },
    { "Europe/Moscow", "MSK-3" },
    { "Europe/Samara", "<+04>-4" },
    { "Asia/Yekaterinburg", "<+05>-5" },
    { "Asia/Omsk", "<+06>-6" },
    { "Asia/Novosibirsk", "<+07>-7" },
    { "Asia/Krasnoyarsk", "<+07>-7" },
    { "Asia/Irkutsk", "<+08>-8" },
    { "Asia/Yakutsk", "<+09>-9" },
    { "Asia/Vladivostok", "<+10>-10" },
    { "Asia/Magadan", "<+11>-11" },
    { "Asia/Kamchatka", "<+12>-12" },
    { "Asia/Tbilisi", "<+04>-4" },
    { "Asia/Yerevan", "<+04>-4" },
    { "Asia/Baku", "<+04>-4" },
    { "Asia/Dubai", "<+04>-4" },
    { "Asia/Tehran", "<+0330>-3:30" },
    { "Asia/Jerusalem", "IST-2IDT,M3.4.4/26,M10.5.0" },
    { "Asia/Tashkent", "<+05>-5" },
    { "Asia/Almaty", "<+05>-5" },
    { "Asia/Karachi", "PKT-5" },
    { "Asia/Kolkata", "IST-5:30" },
    { "Asia/Kathmandu", "<+0545>-5:45" },
    { "Asia/Dhaka", "<+06>-6" },
    { "Asia/Bangkok", "<+07>-7" },
    { "Asia/Ho_Chi_Minh", "<+07>-7" },
    { "Asia/Jakarta", "WIB-7" },
    { "Asia/Shanghai", "CST-8" },
    { "Asia/Hong_Kong", "HKT-8" },
    { "Asia/Singapore", "<+08>-8" },
    { "Asia/Taipei", "CST-8" },
    { "Asia/Tokyo", "JST-9" },
    { "Asia/Seoul", "KST-9" },
    { "Australia/Perth", "AWST-8" },
    { "Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
    { "Australia/Brisbane", "AEST-10" },
    { "Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3" },
    { "Africa/Lagos", "WAT-1" },
    { "Africa/Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24" },
    { "Africa/Johannesburg", "SAST-2" },
    { "Africa/Nairobi", "EAT-3" },
    { "Atlantic/Azores", "<-01>1<+00>,M3.5.0/0,M10.5.0/1" },
    { "America/Sao_Paulo", "<-03>3" },
    { "America/Argentina/Buenos_Aires", "<-03>3" },
    { "America/Santiago", "<-04>4<-03>,M9.1.6/24,M4.1.6/24" },
    { "America/Caracas", "<-04>4" },
    { "America/Bogota", "<-05>5" },
    { "America/Lima", "<-05>5" },
    { "America/St_Johns", "NST3:30NDT,M3.2.0,M11.1.0" },
    { "America/Halifax", "AST4ADT,M3.2.0,M11.1.0" },
    { "America/New_York", "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Chicago", "CST6CDT,M3.2.0,M11.1.0" },
    { "America/Mexico_City", "CST6" },
    { "America/Denver", "MST7MDT,M3.2.0,M11.1.0" },
    { "America/Phoenix", "MST7" },
    { "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0" },
    { "America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0" },
    { "Pacific/Honolulu", "HST10" },
    { NULL, NULL },
};

const tz_zone_t *tz_find(const char *name)
{
    for (const tz_zone_t *z = tz_zones; z->name; ++z)
        if (!strcmp(z->name, name)) return z;
    return NULL;
}

/* ---- POSIX TZ strings ------------------------------------------------------------- */

/* "<+0530>": the offset as its own name, as tzdata names zones without
 * an abbreviation. */
static void off_name(int east, char *out, size_t cap)
{
    int a = east < 0 ? -east : east;
    if (a % 60) snprintf(out, cap, "<%c%02d%02d>", east < 0 ? '-' : '+', a / 60, a % 60);
    else snprintf(out, cap, "<%c%02d>", east < 0 ? '-' : '+', a / 60);
}

/* POSIX counts west of UTC: UTC+3 is "-3". */
static void off_posix(int east, char *out, size_t cap)
{
    int w = -east, a = w < 0 ? -w : w;
    if (a % 60) snprintf(out, cap, "%s%d:%02d", w < 0 ? "-" : "", a / 60, a % 60);
    else snprintf(out, cap, "%s%d", w < 0 ? "-" : "", a / 60);
}

static int rule_ok(int month, int week, int wday, int min)
{
    return month >= 1 && month <= 12 && week >= 1 && week <= 5 && wday >= 0 && wday <= 6 && min >= 0 &&
           min <= 167 * 60;
}

static void rule_str(int month, int week, int wday, int min, char *out, size_t cap)
{
    if (min == 120) snprintf(out, cap, "M%d.%d.%d", month, week, wday);
    else if (min % 60) snprintf(out, cap, "M%d.%d.%d/%d:%02d", month, week, wday, min / 60, min % 60);
    else snprintf(out, cap, "M%d.%d.%d/%d", month, week, wday, min / 60);
}

int tz_build(const tz_rule_t *r, char *out, size_t cap, char *err, size_t ecap)
{
    if (r->std_offset < -12 * 60 || r->std_offset > 14 * 60) {
        snprintf(err, ecap, "the offset is UTC-12:00 .. UTC+14:00");
        return -1;
    }
    char sn[16], so[16];
    off_name(r->std_offset, sn, sizeof sn);
    off_posix(r->std_offset, so, sizeof so);
    if (!r->dst) {
        snprintf(out, cap, "%s%s", sn, so);
        return 0;
    }
    if (r->dst_offset < -12 * 60 || r->dst_offset > 15 * 60 || r->dst_offset == r->std_offset) {
        snprintf(err, ecap, "daylight saving time needs an offset of its own");
        return -1;
    }
    if (!rule_ok(r->start_month, r->start_week, r->start_wday, r->start_min) ||
        !rule_ok(r->end_month, r->end_week, r->end_wday, r->end_min)) {
        snprintf(err, ecap, "a DST rule is month 1..12, week 1..5, weekday 0..6 and a time");
        return -1;
    }
    if (r->start_month == r->end_month && r->start_week == r->end_week) {
        snprintf(err, ecap, "DST must start and end in different weeks");
        return -1;
    }
    char dn[16], dof[16], a[32], b[32];
    off_name(r->dst_offset, dn, sizeof dn);
    off_posix(r->dst_offset, dof, sizeof dof);
    rule_str(r->start_month, r->start_week, r->start_wday, r->start_min, a, sizeof a);
    rule_str(r->end_month, r->end_week, r->end_wday, r->end_min, b, sizeof b);
    /* The DST offset is left out when it is the usual one hour more. */
    if (r->dst_offset == r->std_offset + 60) snprintf(out, cap, "%s%s%s,%s,%s", sn, so, dn, a, b);
    else snprintf(out, cap, "%s%s%s%s,%s,%s", sn, so, dn, dof, a, b);
    return 0;
}

/* A name: 3+ letters, or <...>. */
static const char *skip_name(const char *p)
{
    if (*p == '<') {
        const char *e = strchr(p, '>');
        return e && e - p >= 4 ? e + 1 : NULL;
    }
    const char *s = p;
    while (isalpha((unsigned char)*p)) ++p;
    return p - s >= 3 ? p : NULL;
}

/* [+-]hh[:mm[:ss]] as minutes; returns the end, NULL when there is none. */
static const char *read_time(const char *p, int *min, int allow_sign)
{
    int sign = 1;
    if (allow_sign && (*p == '+' || *p == '-')) sign = *p++ == '-' ? -1 : 1;
    if (!isdigit((unsigned char)*p)) return NULL;
    int h = 0, m = 0;
    while (isdigit((unsigned char)*p)) h = h * 10 + (*p++ - '0');
    if (*p == ':') {
        ++p;
        if (!isdigit((unsigned char)*p)) return NULL;
        while (isdigit((unsigned char)*p)) m = m * 10 + (*p++ - '0');
        if (*p == ':') {                    /* seconds: dropped */
            ++p;
            while (isdigit((unsigned char)*p)) ++p;
        }
    }
    if (h > 167 || m > 59) return NULL;
    *min = sign * (h * 60 + m);
    return p;
}

static const char *read_rule(const char *p, int *month, int *week, int *wday, int *min)
{
    if (*p != 'M') return NULL;
    char *e;
    *month = (int)strtol(p + 1, &e, 10);
    if (*e != '.') return NULL;
    *week = (int)strtol(e + 1, &e, 10);
    if (*e != '.') return NULL;
    *wday = (int)strtol(e + 1, &e, 10);
    *min = 120;
    p = e;
    if (*p == '/' && !(p = read_time(p + 1, min, 1))) return NULL;
    return rule_ok(*month, *week, *wday, *min < 0 ? 0 : *min) ? p : NULL;
}

int tz_parse(const char *s, tz_rule_t *r)
{
    memset(r, 0, sizeof *r);
    const char *p = skip_name(s);
    int west;
    if (!p || !(p = read_time(p, &west, 1))) return -1;
    r->std_offset = -west;
    if (!*p) return 0;
    if (!(p = skip_name(p))) return -1;
    r->dst = 1;
    r->dst_offset = r->std_offset + 60;
    if (*p && *p != ',') {
        if (!(p = read_time(p, &west, 1))) return -1;
        r->dst_offset = -west;
    }
    if (*p != ',') return -1;               /* no rules: the US default is not assumed */
    if (!(p = read_rule(p + 1, &r->start_month, &r->start_week, &r->start_wday, &r->start_min))) return -1;
    if (*p != ',') return -1;
    if (!(p = read_rule(p + 1, &r->end_month, &r->end_week, &r->end_wday, &r->end_min))) return -1;
    return *p ? -1 : 0;
}

/* ---- the zone and the clock --------------------------------------------------------- */

static int tz_string_ok(const char *s)
{
    tz_rule_t r;
    if (!*s || strlen(s) >= 96) return 0;
    for (const char *p = s; *p; ++p)
        if (!isalnum((unsigned char)*p) && !strchr("<>+-:,./", *p)) return 0;
    return tz_parse(s, &r) == 0;
}

void clock_read(sys_clock_t *c)
{
    memset(c, 0, sizeof *c);
    sys_read_line("/etc/timezone", c->zone, sizeof c->zone);
    sys_read_line("/etc/TZ", c->tz, sizeof c->tz);
    c->rtc = sys_exists("/dev/rtc0") || sys_exists("/sys/class/rtc/rtc0");
    char buf[4096];
    if (sys_read("/etc/ntp.conf", buf, sizeof buf) > 0)
        for (char *line = buf; line && *line && c->n_servers < 4;) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            char host[64];
            if (sscanf(line, " server %63s", host) == 1 && ntp_server_ok(host))
                snprintf(c->servers[c->n_servers++], sizeof c->servers[0], "%s", host);
            line = nl ? nl + 1 : NULL;
        }
}

int clock_set_zone(const char *zone, const char *posix, char *err, size_t cap)
{
    const char *tz = posix;
    if (zone && *zone) {
        const tz_zone_t *z = tz_find(zone);
        if (!z) {
            snprintf(err, cap, "unknown time zone '%.64s'", zone);
            return -1;
        }
        tz = z->posix;
    }
    if (!tz || !tz_string_ok(tz)) {
        snprintf(err, cap, "not a POSIX time zone string (e.g. CET-1CEST,M3.5.0,M10.5.0/3)");
        return -1;
    }
    char line[128];
    snprintf(line, sizeof line, "%s\n", tz);
    if (sys_write("/etc/TZ", line, strlen(line), 0644, err, cap) < 0) return -1;
    snprintf(line, sizeof line, "%s\n", zone && *zone ? zone : "custom");
    if (sys_write("/etc/timezone", line, strlen(line), 0644, err, cap) < 0) return -1;
    /* This process's own local time (recording names, the clock on
     * screen). setenv replaces the pointer in environ: a thread reading
     * TZ meanwhile sees the old or the new string, both valid. */
    setenv("TZ", tz, 1);
    tzset();
    return 0;
}

int clock_set(time_t t, char *err, size_t cap)
{
    if (t < 946684800) {                    /* 2000-01-01 */
        snprintf(err, cap, "not a plausible date");
        return -1;
    }
    if (sys_ops()->settime(t) < 0) {
        snprintf(err, cap, "cannot set the clock: %s", strerror(errno));
        return -1;
    }
    char out[256];
    sys_clock_t c;
    clock_read(&c);
    if (c.rtc && sys_have("hwclock")) sys_run(out, sizeof out, 10, "hwclock", "-w", "-u", NULL);
    /* OpenIPC's software RTC: without a save the next boot starts from
     * the old checkpoint. */
    if (sys_have("fake-hwclock")) sys_run(out, sizeof out, 10, "fake-hwclock", "save", NULL);
    return 0;
}

int ntp_server_ok(const char *s)
{
    size_t n = strlen(s);
    return n > 0 && n < 64 && s[0] != '-' && s[0] != '.' &&
           strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") == n;
}

int ntp_set_servers(char servers[][64], int n, char *err, size_t cap)
{
    if (n < 1 || n > 4) {
        snprintf(err, cap, "one to four NTP servers");
        return -1;
    }
    char text[512] = "";
    for (int i = 0; i < n; ++i) {
        if (!ntp_server_ok(servers[i])) {
            snprintf(err, cap, "'%.64s' is not a host name or address", servers[i]);
            return -1;
        }
        size_t l = strlen(text);
        snprintf(text + l, sizeof text - l, "server %s iburst\n", servers[i]);
    }
    return sys_write("/etc/ntp.conf", text, strlen(text), 0644, err, cap);
}

int ntp_sync(char servers[][64], int n, char *msg, size_t cap)
{
    const char *argv[16];
    int k = 0;
    argv[k++] = "ntpd";
    argv[k++] = "-n";
    argv[k++] = "-q";
    for (int i = 0; i < n && i < 4; ++i) {
        if (!ntp_server_ok(servers[i])) continue;
        argv[k++] = "-p";
        argv[k++] = servers[i];
    }
    argv[k] = NULL;
    if (k == 3) {
        snprintf(msg, cap, "no NTP server");
        return -1;
    }
    char out[1024];
    /* busybox ntpd -q gives up by itself only after many tries. */
    int rc = sys_ops()->run(argv, NULL, 20, out, sizeof out);
    sys_trim(out);
    if (rc == 0) {
        snprintf(msg, cap, "the clock was set from %s", servers[0]);
        char o2[128];
        sys_clock_t c;
        clock_read(&c);
        if (c.rtc && sys_have("hwclock")) sys_run(o2, sizeof o2, 10, "hwclock", "-w", "-u", NULL);
        if (sys_have("fake-hwclock")) sys_run(o2, sizeof o2, 10, "fake-hwclock", "save", NULL);
        return 0;
    }
    /* busybox ntpd -q gives up by its own alarm (SIGALRM, 128 + 14) when
     * no server answers: seen in QEMU with an unreachable server. */
    if (rc == 124 || rc == 128 + 14) snprintf(msg, cap, "no answer from the NTP servers");
    else snprintf(msg, cap, "ntpd failed (%d)%s%.200s", rc, out[0] ? ": " : "", out);
    return -1;
}

int ntp_daemon(int on, char *err, size_t cap)
{
    char out[256];
    if (!sys_exists("/etc/init.d/S49ntpd")) {
        snprintf(err, cap, "this firmware has no /etc/init.d/S49ntpd");
        return -1;
    }
    int rc = sys_run(out, sizeof out, 30, "/etc/init.d/S49ntpd", on ? "restart" : "stop", NULL);
    if (rc != 0) {
        sys_trim(out);
        snprintf(err, cap, "S49ntpd %s failed: %.200s", on ? "restart" : "stop", out);
        return -1;
    }
    return 0;
}

/* ---- host name ---------------------------------------------------------------------- */

void host_get(char *name, size_t cap)
{
    sys_read_line("/etc/hostname", name, cap);
    if (!name[0] && !sys_root()[0] && gethostname(name, cap) < 0) name[0] = 0;
}

int host_check(const char *n, char *err, size_t cap)
{
    size_t l = strlen(n);
    if (l < 1 || l > 63 || strspn(n, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != l ||
        n[0] == '-' || n[l - 1] == '-') {
        snprintf(err, cap, "1 to 63 letters, digits and '-', not starting or ending with '-'");
        return -1;
    }
    return 0;
}

int host_set(const char *name, char *err, size_t cap)
{
    if (host_check(name, err, cap) < 0) return -1;
    char line[96];
    snprintf(line, sizeof line, "%s\n", name);
    if (sys_write("/etc/hostname", line, strlen(line), 0644, err, cap) < 0) return -1;
    if (sys_ops()->sethostname(name, strlen(name)) < 0) {
        snprintf(err, cap, "cannot set the host name: %s", strerror(errno));
        return -1;
    }
    /* /etc/hosts: the 127.0.1.1 line names this host (S31hostname keeps
     * it so); every other line stays. */
    char *hosts = malloc(16384), *out = malloc(16384 + 128);
    if (hosts && out) {
        if (sys_read("/etc/hosts", hosts, 16384) < 0) snprintf(hosts, 16384, "127.0.0.1\tlocalhost\n");
        size_t len = 0;
        out[0] = 0;
        for (char *l = hosts; l && *l;) {
            char *nl = strchr(l, '\n');
            if (nl) *nl = 0;
            if (strncmp(l, "127.0.1.1", 9) != 0) len += (size_t)snprintf(out + len, 16384 + 128 - len, "%s\n", l);
            l = nl ? nl + 1 : NULL;
        }
        snprintf(out + len, 16384 + 128 - len, "127.0.1.1\t%s\n", name);
        sys_write("/etc/hosts", out, strlen(out), 0644, err, cap);
    }
    free(hosts);
    free(out);
    /* The user chose this name: S31hostname must not rename the box to
     * <soc>-<sensor> at the next boot. */
    if (!sys_exists("/etc/hostname.auto")) sys_write("/etc/hostname.auto", "", 0, 0644, err, cap);
    err[0] = 0;
    return 0;
}
