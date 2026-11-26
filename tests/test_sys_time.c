/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_sys_time.c — time zones, DST, the clock, NTP and the host name on
 * a fake system (sys_time.h). */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sys.h"
#include "sys_fake.h"
#include "sys_time.h"

void test_sys_time(void);

/* UTC offset in seconds of local time `t` under POSIX TZ `tz`, as the C
 * library computes it: the strings must mean to libc what they mean to
 * us. */
static long gmtoff(const char *tz, time_t t)
{
    setenv("TZ", tz, 1);
    tzset();
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_gmtoff;
}

void test_sys_time(void)
{
    const time_t jan = 1767268800;          /* 2026-01-01 12:00 UTC */
    const time_t jul = 1782907200;          /* 2026-07-01 12:00 UTC */
    char *saved_tz = getenv("TZ") ? strdup(getenv("TZ")) : NULL;
    char out[128], err[200];

    /* Every zone offered parses, and libc agrees with the parsed offsets. */
    int n = 0, bad = 0;
    for (const tz_zone_t *z = tz_zones; z->name; ++z, ++n) {
        tz_rule_t r;
        if (tz_parse(z->posix, &r) < 0) {
            fprintf(stderr, "zone %s: %s does not parse\n", z->name, z->posix);
            ++bad;
            continue;
        }
        int south = r.dst && r.start_month > r.end_month;
        long w = gmtoff(z->posix, jan), s = gmtoff(z->posix, jul);
        long want_w = (r.dst && south ? r.dst_offset : r.std_offset) * 60L;
        long want_s = (r.dst && !south ? r.dst_offset : r.std_offset) * 60L;
        if (w != want_w || s != want_s) {
            fprintf(stderr, "zone %s: libc %ld/%ld, parsed %ld/%ld\n", z->name, w, s, want_w, want_s);
            ++bad;
        }
    }
    CHECK(n > 60 && !bad, "%d zones, %d wrong", n, bad);
    CHECK(tz_find("Europe/Berlin") && !strcmp(tz_find("Europe/Berlin")->posix, "CET-1CEST,M3.5.0,M10.5.0/3") &&
          !tz_find("Mars/Base"), "tz_find");

    /* Custom zones: built strings, read back, and what libc makes of them. */
    tz_rule_t r = { .std_offset = 180 };
    CHECK(tz_build(&r, out, sizeof out, err, sizeof err) == 0 && !strcmp(out, "<+03>-3"), "UTC+3: %s", out);
    CHECK(gmtoff(out, jan) == 10800, "UTC+3 in libc");
    r = (tz_rule_t){ .std_offset = 330 };
    CHECK(tz_build(&r, out, sizeof out, err, sizeof err) == 0 && !strcmp(out, "<+0530>-5:30") &&
          gmtoff(out, jul) == 19800, "UTC+5:30: %s", out);
    r = (tz_rule_t){ .std_offset = -300 };
    CHECK(tz_build(&r, out, sizeof out, err, sizeof err) == 0 && !strcmp(out, "<-05>5") && gmtoff(out, jan) == -18000,
          "UTC-5: %s", out);
    /* Central Europe's rules, written out: last Sunday of March 02:00 to
     * last Sunday of October 03:00. */
    r = (tz_rule_t){ .std_offset = 60, .dst = 1, .dst_offset = 120, .start_month = 3, .start_week = 5,
                     .start_wday = 0, .start_min = 120, .end_month = 10, .end_week = 5, .end_wday = 0,
                     .end_min = 180 };
    CHECK(tz_build(&r, out, sizeof out, err, sizeof err) == 0 && !strcmp(out, "<+01>-1<+02>,M3.5.0,M10.5.0/3"),
          "CET with DST: %s", out);
    CHECK(gmtoff(out, jan) == 3600 && gmtoff(out, jul) == 7200, "DST in libc");
    /* the switch itself: 2026-03-29 01:00 UTC is 03:00 CEST */
    CHECK(gmtoff(out, 1774745999) == 3600 && gmtoff(out, 1774746000) == 7200, "DST starts on time");
    tz_rule_t back;
    CHECK(tz_parse(out, &back) == 0 && !memcmp(&back, &r, sizeof r), "custom string parses back");
    /* A DST of 30 minutes keeps its offset in the string. */
    r.dst_offset = 90;
    CHECK(tz_build(&r, out, sizeof out, err, sizeof err) == 0 && !strcmp(out, "<+01>-1<+0130>-1:30,M3.5.0,M10.5.0/3"),
          "half-hour DST: %s", out);
    CHECK(gmtoff(out, jul) == 5400, "half-hour DST in libc");
    r.dst_offset = 60;
    CHECK(tz_build(&r, out, sizeof out, err, sizeof err) < 0, "DST equal to standard time refused");
    r.dst_offset = 120;
    r.start_month = 13;
    CHECK(tz_build(&r, out, sizeof out, err, sizeof err) < 0, "month 13 refused");
    r = (tz_rule_t){ .std_offset = 15 * 60 };
    CHECK(tz_build(&r, out, sizeof out, err, sizeof err) < 0, "UTC+15 refused");
    CHECK(tz_parse("EST5EDT", &back) < 0, "DST without rules is refused (no implied US rules)");
    CHECK(tz_parse("EST5EDT,M3.2.0/2:30,M11.1.0", &back) == 0 && back.start_min == 150 && back.std_offset == -300,
          "rule with minutes");
    CHECK(tz_parse("garbage", &back) < 0 && tz_parse("UTC", &back) < 0 && tz_parse("", &back) < 0,
          "not TZ strings");

    /* The zone on the system: /etc/TZ and /etc/timezone, the OpenIPC way. */
    fake_init();
    fake_file("/etc/TZ", "GMT0\n");
    fake_file("/etc/timezone", "Etc/GMT\n");
    fake_file("/etc/ntp.conf", "server 0.pool.ntp.org iburst\nserver 1.pool.ntp.org iburst\n# x\n");
    sys_clock_t c;
    clock_read(&c);
    CHECK(!strcmp(c.tz, "GMT0") && !strcmp(c.zone, "Etc/GMT") && c.n_servers == 2 &&
          !strcmp(c.servers[1], "1.pool.ntp.org") && !c.rtc, "read: %s %s %d", c.tz, c.zone, c.n_servers);
    CHECK(clock_set_zone("Europe/Kyiv", NULL, err, sizeof err) == 0, "set zone: %s", err);
    char *t = fake_read("/etc/TZ"), *z = fake_read("/etc/timezone");
    CHECK(t && z && !strcmp(t, "EET-2EEST,M3.5.0/3,M10.5.0/4\n") && !strcmp(z, "Europe/Kyiv\n"), "files: %s %s",
          t, z);
    free(t);
    free(z);
    CHECK(!strcmp(getenv("TZ"), "EET-2EEST,M3.5.0/3,M10.5.0/4"), "this process's TZ follows");
    CHECK(clock_set_zone("", "<+03>-3", err, sizeof err) == 0, "custom zone");
    z = fake_read("/etc/timezone");
    CHECK(z && !strcmp(z, "custom\n"), "custom name");
    free(z);
    CHECK(clock_set_zone("", "x; rm -rf /", err, sizeof err) < 0 && clock_set_zone("Nowhere/X", NULL, err, sizeof err) < 0,
          "bad zones refused");

    /* The clock: settimeofday, the RTC when there is one, fake-hwclock. */
    fake_clear_log();
    CHECK(clock_set(1800000000, err, sizeof err) == 0 && fake_time == 1800000000 && !fake_ran("hwclock -w") &&
          fake_ran("fake-hwclock save"), "set without RTC: %s", fake_log);
    fake_file("/dev/rtc0", "");
    fake_clear_log();
    CHECK(clock_set(1800000000, err, sizeof err) == 0 && fake_ran("hwclock -w -u"), "with an RTC: %s", fake_log);
    CHECK(clock_set(1000, err, sizeof err) < 0, "1970 refused");

    /* NTP */
    char srv[4][64] = { "pool.ntp.org", "10.0.2.2" };
    CHECK(ntp_set_servers(srv, 2, err, sizeof err) == 0, "servers");
    t = fake_read("/etc/ntp.conf");
    CHECK(t && !strcmp(t, "server pool.ntp.org iburst\nserver 10.0.2.2 iburst\n"), "ntp.conf: %s", t);
    free(t);
    char evil[4][64] = { "a.org", "-p" };
    CHECK(ntp_set_servers(evil, 2, err, sizeof err) < 0, "an option as a server is refused");
    fake_clear_log();
    CHECK(ntp_sync(srv, 2, err, sizeof err) == 0 && fake_ran("ntpd -n -q -p pool.ntp.org -p 10.0.2.2"),
          "sync now: %s", fake_log);
    fake_answer("ntpd", 124, "");
    CHECK(ntp_sync(srv, 2, err, sizeof err) < 0 && strstr(err, "no answer"), "sync timeout: %s", err);
    fake_answer("ntpd", 1, "ntpd: bad address 'x'");
    CHECK(ntp_sync(srv, 1, err, sizeof err) < 0 && strstr(err, "bad address"), "sync error: %s", err);
    CHECK(ntp_daemon(1, err, sizeof err) < 0, "no init script: said so");
    fake_file("/etc/init.d/S49ntpd", "#!/bin/sh\n");
    fake_clear_log();
    CHECK(ntp_daemon(1, err, sizeof err) == 0 && fake_ran("/etc/init.d/S49ntpd restart"), "restart ntpd");
    CHECK(ntp_daemon(0, err, sizeof err) == 0 && fake_ran("/etc/init.d/S49ntpd stop"), "stop ntpd");

    /* Host name */
    fake_file("/etc/hostname", "openipc-ssr621q\n");
    fake_file("/etc/hosts", "127.0.0.1\tlocalhost\n127.0.1.1\topenipc-ssr621q\n");
    char h[64];
    host_get(h, sizeof h);
    CHECK(!strcmp(h, "openipc-ssr621q"), "host_get: %s", h);
    CHECK(host_set("-bad", err, sizeof err) < 0 && host_set("a b", err, sizeof err) < 0 &&
          host_set("", err, sizeof err) < 0, "bad names refused");
    CHECK(host_set("nvr-hall", err, sizeof err) == 0 && !strcmp(fake_hostname, "nvr-hall"), "host_set: %s", err);
    t = fake_read("/etc/hostname");
    char *hs = fake_read("/etc/hosts");
    CHECK(t && !strcmp(t, "nvr-hall\n") && hs && !strcmp(hs, "127.0.0.1\tlocalhost\n127.0.1.1\tnvr-hall\n") &&
          fake_exists("/etc/hostname.auto"), "files: %s / %s", t, hs);
    free(t);
    free(hs);
    fake_done();

    if (saved_tz) setenv("TZ", saved_tz, 1);
    else unsetenv("TZ");
    tzset();
    free(saved_tz);
}
