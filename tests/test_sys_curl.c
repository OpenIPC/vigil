/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_sys_curl.c — mail, DDNS and the alarm center's requests, as the
 * curl options they become (sys_curl.h). */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sys.h"
#include "sys_curl.h"
#include "sys_fake.h"

void test_sys_curl(void);

static int tmp_files(void)
{
    char p[512];
    snprintf(p, sizeof p, "%s/tmp", fake_root);
    DIR *d = opendir(p);
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) n += e->d_name[0] != '.';
    if (d) closedir(d);
    return n;
}

void test_sys_curl(void)
{
    fake_init();
    fake_mkdir("/tmp");
    fake_file("/etc/ssl/certs/ca-certificates.crt", "x");
    config_smtp_t s = { .enabled = 1, .server = "smtp.example.org", .port = 587, .security = "starttls",
                        .user = "nvr", .password = "pa:ss", .from = "nvr@example.org",
                        .to = "a@example.org, b@example.org" };
    char msg[256];
    fake_clear_log();
    CHECK(mail_send(&s, "Test", "line 1\nline 2\n", msg, sizeof msg) == 0, "mail: %s", msg);
    CHECK(fake_ran("curl -K -") && !strstr(fake_log, "pa:ss"), "the password is not on the command line");
    CHECK(strstr(fake_stdin, "url = \"smtp://smtp.example.org:587\"\n") && strstr(fake_stdin, "ssl-reqd\n") &&
          strstr(fake_stdin, "user = \"nvr:pa:ss\"\n") && strstr(fake_stdin, "mail-rcpt = \"a@example.org\"\n") &&
          strstr(fake_stdin, "mail-rcpt = \"b@example.org\"\n") &&
          strstr(fake_stdin, "mail-from = \"nvr@example.org\"\n") && strstr(fake_stdin, "cacert = \""),
          "curl options:\n%s", fake_stdin);
    CHECK(tmp_files() == 0, "the message file is removed");
    s.security[0] = 0;
    snprintf(s.security, sizeof s.security, "tls");
    s.port = 465;
    mail_send(&s, "Test", "x", msg, sizeof msg);
    CHECK(strstr(fake_stdin, "url = \"smtps://smtp.example.org:465\"") && strstr(fake_stdin, "ssl-reqd"), "TLS");
    snprintf(s.security, sizeof s.security, "none");
    s.user[0] = 0;
    mail_send(&s, "Test", "x", msg, sizeof msg);
    CHECK(!strstr(fake_stdin, "ssl-reqd") && !strstr(fake_stdin, "user ="), "no TLS, no login:\n%s", fake_stdin);
    fake_answer("curl", 67, "curl: (67) Login denied");
    CHECK(mail_send(&s, "T", "x", msg, sizeof msg) < 0 && strstr(msg, "Login denied"), "failure: %s", msg);
    fake_have("curl", 0);
    CHECK(mail_send(&s, "T", "x", msg, sizeof msg) < 0 && strstr(msg, "no curl"), "no curl: %s", msg);
    fake_have("curl", 1);
    s.to[0] = 0;
    CHECK(mail_send(&s, "T", "x", msg, sizeof msg) < 0, "no recipient");

    config_ddns_t d = { .enabled = 1, .provider = "dyndns", .host = "nvr.example.org", .user = "u",
                        .password = "p", .interval = 10 };
    fake_answer("curl", 0, "good 203.0.113.5\n");
    CHECK(ddns_update(&d, msg, sizeof msg) == 0 && strstr(msg, "good 203.0.113.5"), "dyndns: %s", msg);
    CHECK(strstr(fake_stdin, "url = \"https://members.dyndns.org/nic/update?hostname=nvr.example.org\"") &&
          strstr(fake_stdin, "user = \"u:p\""), "dyndns request:\n%s", fake_stdin);
    fake_answer("curl", 0, "badauth\n");
    CHECK(ddns_update(&d, msg, sizeof msg) < 0 && strstr(msg, "badauth"), "badauth: %s", msg);
    snprintf(d.provider, sizeof d.provider, "noip");
    fake_answer("curl", 0, "nochg 203.0.113.5");
    CHECK(ddns_update(&d, msg, sizeof msg) == 0 && strstr(fake_stdin, "dynupdate.no-ip.com"), "noip");
    snprintf(d.provider, sizeof d.provider, "custom");
    snprintf(d.url, sizeof d.url, "https://dns.example/update?h={host}&k={password}");
    fake_answer("curl", 0, "OK");
    CHECK(ddns_update(&d, msg, sizeof msg) == 0 &&
          strstr(fake_stdin, "url = \"https://dns.example/update?h=nvr.example.org&k=p\"") &&
          !strstr(fake_stdin, "user ="), "custom URL:\n%s", fake_stdin);

    fake_answer("curl", 0, "");
    CHECK(http_post_json("http://10.0.0.5:8000/alarm", "{\"a\":1}", msg, sizeof msg) == 0 &&
          strstr(fake_stdin, "url = \"http://10.0.0.5:8000/alarm\"") && strstr(fake_stdin, "data-binary = \"@") &&
          strstr(fake_stdin, "Content-Type: application/json") && tmp_files() == 0, "alarm POST:\n%s", fake_stdin);
    fake_done();
}
