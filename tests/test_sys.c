/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_sys.c — src/sys on a fake system (sys_fake.h): one function per
 * area, each in its own test_sys_<area>.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sys.h"
#include "sys_fake.h"

void test_sys_seams(void);
void test_sys_net(void);
void test_sys_time(void);
void test_sys_curl(void);
void test_sys_disk(void);
void test_sys_mgmt(void);
void test_sys_smart(void);
void test_sys_scan(void);

void test_sys_seams(void)
{
    fake_init();
    char p[512];
    sys_path(p, sizeof p, "/etc/hostname");
    CHECK(!strncmp(p, fake_root, strlen(fake_root)) && !strcmp(p + strlen(fake_root), "/etc/hostname"),
          "sys_path: under the root (%s)", p);
    CHECK(sys_managed(), "a test root is managed");
    fake_file("/etc/hostname", "nvr1\n");
    char line[64];
    CHECK(!strcmp(sys_read_line("/etc/hostname", line, sizeof line), "nvr1"), "read_line: %s", line);
    CHECK(sys_read_ll("/etc/hostname", 7) == 7, "not a number: the default");
    fake_file("/sys/x", "42\n");
    CHECK(sys_read_ll("/sys/x", 0) == 42, "read_ll");
    char err[200];
    CHECK(sys_write("/etc/new", "abc", 3, 0644, err, sizeof err) == 0, "sys_write: %s", err);
    char *t = fake_read("/etc/new");
    CHECK(t && !strcmp(t, "abc"), "written");
    free(t);
    fake_answer("ifup", 1, "no luck");
    char out[64];
    CHECK(sys_run(out, sizeof out, 5, "ifup", "eth0", NULL) == 1 && !strcmp(out, "no luck") &&
          fake_ran("ifup eth0"), "the runner is the fake's");
    fake_have("mkfs.ext4", 0);
    CHECK(!sys_have("mkfs.ext4") && sys_have("mkfs.vfat"), "have");
    CHECK(sys_run(out, sizeof out, 5, "mkfs.ext4", NULL) == 127, "not there: 127");
    fake_done();

    /* The real runner (no fake): status, output, stdin, a timeout. */
    sys_set_ops(NULL);
    sys_set_root("");
    CHECK(sys_run(out, sizeof out, 5, "sh", "-c", "echo hi; exit 3", NULL) == 3 && !strcmp(out, "hi\n"),
          "real run: %s", out);
    const char *argv[] = { "cat", NULL };
    CHECK(sys_ops()->run(argv, "in put", 5, out, sizeof out) == 0 && !strcmp(out, "in put"), "stdin: %s", out);
    CHECK(sys_run(out, sizeof out, 1, "sleep", "5", NULL) == 124, "timeout: 124");
    CHECK(sys_run(out, sizeof out, 1, "no-such-program-here", NULL) == 127, "not found: 127");
}

int main(void)
{
    test_sys_seams();
    test_sys_net();
    test_sys_time();
    test_sys_curl();
    test_sys_disk();
    test_sys_mgmt();
    test_sys_smart();
    test_sys_scan();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
