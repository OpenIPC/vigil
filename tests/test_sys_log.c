/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_sys_log.c — the system log (sys_log.h): logread's lines in both of
 * BusyBox's formats, the kernel's, filters, paging, no logread. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sys.h"
#include "sys_fake.h"
#include "sys_log.h"

void test_sys_log(void);

void test_sys_log(void)
{
    setenv("TZ", "UTC", 1);
    tzset();
    sys_logline_t l;
    time_t now = 1795851600;                    /* 2026-11-28 07:40:00 UTC */
    const char *a = "Nov 28 07:39:58 nvr daemon.info vigil[812]: CAM2 192.168.1.11: online";
    CHECK(sys_log_parse_syslog(a, strlen(a), now, &l) == 0 && l.level == 6 && !strcmp(l.tag, "vigil") &&
              !strcmp(l.text, "CAM2 192.168.1.11: online") && l.time == now - 2,
          "logread: %d %s %s %lld", l.level, l.tag, l.text, (long long)l.time);
    const char *b = "Dec 31 23:59:59 kern.err kernel: oops";
    CHECK(sys_log_parse_syslog(b, strlen(b), now, &l) == 0 && l.level == -1 && l.time < now,
          "-S (no host, no level): last year's December: %lld", (long long)l.time);
    const char *c = "Nov 28 07:00:00 nvr user.warn kernel: [   12.5] usb 1-1: new device";
    CHECK(sys_log_parse_syslog(c, strlen(c), now, &l) == 0 && l.level == 4 && !strcmp(l.tag, "kernel"),
          "warn: %d %s", l.level, l.tag);
    const char *k = "<3>[   12.250000] mmc0: error -110";
    CHECK(sys_log_parse_kernel(k, strlen(k), now - 100, &l) == 0 && l.level == 3 && l.time == now - 88 &&
              !strcmp(l.text, "mmc0: error -110"),
          "kernel: %d %lld %s", l.level, (long long)l.time, l.text);
    CHECK(!strcmp(sys_log_level_name(4), "warning") && !*sys_log_level_name(-1), "level names");

    fake_init();
    fake_answer("logread", 0,
                "Nov 28 07:00:01 nvr daemon.info vigil[1]: one\n"
                "Nov 28 07:00:02 nvr daemon.err vigil[1]: two FAILED\n"
                "Nov 28 07:00:03 nvr daemon.info udhcpc[9]: three\n"
                "Nov 28 07:00:04 nvr daemon.debug vigil[1]: four\n");
    sys_logline_t out[8];
    int more = 0;
    char why[200];
    int n = sys_log_read(SYS_LOG_SYSLOG, -1, NULL, 0, 8, out, &more, why, sizeof why);
    CHECK(n == 4 && !more && !strcmp(out[0].text, "four") && !strcmp(out[3].text, "one"), "newest first: %d", n);
    n = sys_log_read(SYS_LOG_SYSLOG, 4, NULL, 0, 8, out, &more, why, sizeof why);
    CHECK(n == 1 && !strcmp(out[0].text, "two FAILED"), "warning or worse: %d", n);
    n = sys_log_read(SYS_LOG_SYSLOG, -1, "UDHCPC", 0, 8, out, &more, why, sizeof why);
    CHECK(n == 1 && !strcmp(out[0].tag, "udhcpc"), "by tag, any case: %d", n);
    n = sys_log_read(SYS_LOG_SYSLOG, -1, NULL, 1, 2, out, &more, why, sizeof why);
    CHECK(n == 2 && more && !strcmp(out[0].text, "three"), "paging: %d %d", n, more);
    fake_file("/proc/uptime", "100.00 90.00\n");
    snprintf(fake_klog, sizeof fake_klog, "<6>[    1.000000] Booting Linux\n<4>[   50.000000] eth0: link down\n");
    n = sys_log_read(SYS_LOG_KERNEL, -1, NULL, 0, 8, out, &more, why, sizeof why);
    CHECK(n == 2 && out[0].level == 4 && !strcmp(out[0].text, "eth0: link down") &&
              out[1].time && out[0].time - out[1].time == 49,
          "dmesg: %d", n);
    fake_have("logread", 0);
    fake_file("/var/log/messages", "Nov 28 07:00:09 host daemon.notice x: from the file\n");
    n = sys_log_read(SYS_LOG_SYSLOG, -1, NULL, 0, 8, out, &more, why, sizeof why);
    CHECK(n == 1 && !strcmp(out[0].text, "from the file"), "no logread: /var/log/messages (%d, %s)", n, why);
    fake_done();
    fake_init();
    fake_have("logread", 0);
    CHECK(sys_log_read(SYS_LOG_SYSLOG, -1, NULL, 0, 8, out, &more, why, sizeof why) < 0 && why[0],
          "neither: says why (%s)", why);
    fake_done();
}
