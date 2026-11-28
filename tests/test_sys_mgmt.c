/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_sys_mgmt.c — the management layer's jobs and timers on a fake
 * system: a network change confirmed and one rolled back, NTP, mail and
 * the alarm center from events, a refused format (mgmt.h). */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "events.h"
#include "logs.h"
#include "mgmt.h"
#include "sys.h"
#include "sys_fake.h"

void test_sys_mgmt(void);

#define FW_ETH0 "iface eth0 inet dhcp\n    hwaddress ether $(get_mac eth0)\n"

/* The job's end state, waiting up to `secs`. */
static mgmt_job_state_t wait_job(int id, int secs, mgmt_job_t *j)
{
    for (int i = 0; i < secs * 20; ++i) {
        if (mgmt_job(id, j) == 0 && j->state >= JOB_DONE) return j->state;
        usleep(50 * 1000);
    }
    return mgmt_job(id, j) == 0 ? j->state : JOB_FAILED;
}

static int wait_for(const char *substr, int secs)
{
    for (int i = 0; i < secs * 20; ++i) {
        if (fake_ran(substr)) return 1;
        usleep(50 * 1000);
    }
    return 0;
}

void test_sys_mgmt(void)
{
    fake_init();
    fake_mkdir("/tmp");
    fake_file("/etc/network/interfaces.d/eth0", FW_ETH0);
    fake_file("/etc/ntp.conf", "server 0.pool.ntp.org iburst\n");
    fake_file("/etc/init.d/S49ntpd", "#!/bin/sh\n");
    char cfgpath[300];
    snprintf(cfgpath, sizeof cfgpath, "%s/etc/vigil.yaml", fake_root);
    fake_file("/etc/vigil.yaml", "smtp:\n  enabled: true\n  server: mail.example.org\n  from: nvr@example.org\n"
                                 "  to: me@example.org\nalarm:\n  enabled: true\n  host: 10.0.0.5\n  port: 8000\n"
                                 "  path: /events\n  interval: 0\n");
    CHECK(config_init(cfgpath) == 0, "config loaded");
    mgmt_set_confirm_secs(2);
    CHECK(mgmt_start() == 0, "start");

    char field[32], err[256];
    mgmt_job_t j;
    net_cfg_t bad = { .ifname = "eth0", .dhcp = 0, .address = "10.0.2.15", .netmask = "255.0.255.0" };
    CHECK(mgmt_net_apply(&bad, 1, field, sizeof field, err, sizeof err) == MGMT_EINVAL && !strcmp(field, "netmask"),
          "invalid: refused before any job (%s: %s)", field, err);

    /* Static, confirmed in time: it stays. */
    net_cfg_t st = { .ifname = "eth0", .dhcp = 0, .address = "10.0.2.15", .netmask = "255.255.255.0",
                     .gateway = "10.0.2.2", .dns1 = "10.0.2.3" };
    int id = mgmt_net_apply(&st, 1, field, sizeof field, err, sizeof err);
    CHECK(id > 0 && wait_job(id, 5, &j) == JOB_DONE, "static applied: %s", j.msg);
    char ifn[16];
    int left = 0;
    CHECK(mgmt_net_pending(ifn, sizeof ifn, &left) && !strcmp(ifn, "eth0") && left <= 2, "waits for a confirmation");
    CHECK(mgmt_net_confirm() == 0 && !mgmt_net_pending(NULL, 0, NULL), "confirmed");
    CHECK(mgmt_net_confirm() < 0, "nothing more to confirm");
    sleep(3);
    char *t = fake_read("/etc/network/interfaces.d/eth0");
    CHECK(t && strstr(t, "inet static") && strstr(t, "address 10.0.2.15"), "confirmed: kept");
    free(t);

    /* Back to DHCP, not confirmed: the static settings come back. */
    net_cfg_t dh = { .ifname = "eth0", .dhcp = 1 };
    fake_clear_log();
    id = mgmt_net_apply(&dh, 1, field, sizeof field, err, sizeof err);
    CHECK(id > 0 && wait_job(id, 5, &j) == JOB_DONE, "DHCP applied");
    t = fake_read("/etc/network/interfaces.d/eth0");
    CHECK(t && strstr(t, "inet dhcp"), "DHCP written");
    free(t);
    for (int i = 0; i < 100 && mgmt_net_pending(NULL, 0, NULL); ++i) usleep(100 * 1000);
    usleep(300 * 1000);
    t = fake_read("/etc/network/interfaces.d/eth0");
    CHECK(t && strstr(t, "inet static") && strstr(t, "address 10.0.2.15"), "not confirmed: rolled back:\n%s", t);
    free(t);
    const char *second = strstr(fake_log, "ifup eth0");
    CHECK(second && strstr(second + 1, "ifup eth0"), "restarted twice: %s", fake_log);

    /* ifup fails: the old file is back at once, the job says so. */
    fake_answer("ifup", 1, "ip: RTNETLINK answers: File exists");
    id = mgmt_net_apply(&dh, 1, field, sizeof field, err, sizeof err);
    CHECK(wait_job(id, 5, &j) == JOB_FAILED && strstr(j.msg, "previous settings are back") &&
          !mgmt_net_pending(NULL, 0, NULL), "ifup failure: %s", j.msg);
    t = fake_read("/etc/network/interfaces.d/eth0");
    CHECK(t && strstr(t, "inet static"), "ifup failure: file restored");
    free(t);
    fake_answer("ifup", 0, "");

    /* NTP: servers into ntp.conf and time.* into vigil.yaml; sync now. */
    char srv[4][64] = { "10.0.2.2" };
    CHECK(mgmt_ntp_set(1, srv, 1, 30, err, sizeof err) == MGMT_OK, "ntp settings: %s", err);
    vigil_config_t *c = malloc(sizeof *c);
    config_get(c);
    CHECK(c->time.ntp == 1 && c->time.ntp_interval == 30, "time section saved");
    t = fake_read("/etc/ntp.conf");
    CHECK(t && !strcmp(t, "server 10.0.2.2 iburst\n"), "ntp.conf");
    free(t);
    CHECK(wait_for("/etc/init.d/S49ntpd restart", 5), "ntpd restarted for the new servers");
    id = mgmt_ntp_sync();
    CHECK(wait_job(id, 5, &j) == JOB_DONE && fake_ran("ntpd -n -q -p 10.0.2.2"), "sync now: %s", j.msg);
    mgmt_ntp_status_t ns;
    mgmt_ntp_status(&ns);
    CHECK(ns.last_ok && ns.last_try == ns.last_ok, "last sync recorded");
    CHECK(mgmt_ntp_set(0, srv, 1, 5, err, sizeof err) == MGMT_EINVAL, "interval 5 min refused: %s", err);
    CHECK(mgmt_ntp_set(0, srv, 1, 60, err, sizeof err) == MGMT_OK && wait_for("/etc/init.d/S49ntpd stop", 5),
          "NTP off stops ntpd");

    /* A test mail through curl; trouble events become a mail and alarm posts. */
    fake_clear_log();
    id = mgmt_smtp_test();
    CHECK(wait_job(id, 5, &j) == JOB_DONE && strstr(fake_stdin, "smtp://mail.example.org:587"), "test mail: %s",
          j.msg);
    mgmt_services_t sv;
    mgmt_services(&sv);
    CHECK(sv.smtp.ok && sv.smtp.last, "smtp status");
    fake_clear_log();
    events_push(3, "REC disk full");
    CHECK(wait_for("curl -K -\ncurl -K -", 5), "event: alarm post and mail: %s", fake_log);
    mgmt_services(&sv);
    CHECK(sv.alarm.last && sv.alarm.ok, "alarm status");

    /* Disks: a format must be confirmed by the name; no such disk. */
    CHECK(mgmt_disk_format("sda", "exfat", "sdb", err, sizeof err) == MGMT_EINVAL && strstr(err, "type"),
          "format needs the name typed: %s", err);
    CHECK(mgmt_disk_format("sdz", "exfat", "sdz", err, sizeof err) == MGMT_EINVAL, "unknown disk: %s", err);

    /* Host name and time zone go straight through. */
    CHECK(mgmt_hostname_set("nvr-2", err, sizeof err) == MGMT_OK && !strcmp(fake_hostname, "nvr-2"), "hostname");
    CHECK(mgmt_time_set("Europe/Berlin", NULL, 1, 1800000000, err, sizeof err) == MGMT_OK && fake_time == 1800000000,
          "zone and clock: %s", err);
    CHECK(mgmt_time_set("", "nonsense", 0, 0, err, sizeof err) == MGMT_EINVAL, "bad zone refused");

    /* Reboot: asked for, seen by the main loop, then busybox's reboot;
     * the operation log says who asked (the thread's actor, logs.h). */
    oplog_actor("anna", "api", "10.1.2.3");
    CHECK(mgmt_power_pending() == MGMT_POWER_NONE, "nothing asked for");
    CHECK(mgmt_power_request(MGMT_POWER_NONE) == MGMT_EINVAL, "neither reboot nor shutdown");
    CHECK(mgmt_power_request(MGMT_POWER_REBOOT) == MGMT_OK && mgmt_power_pending() == MGMT_POWER_REBOOT,
          "reboot asked for");
    {
        logrec_t r[8];
        logfilter_t f = { .ch = -1, .type = "reboot,hostname_set,time_zone_set,clock_set" };
        int n = logbook_query(logs_ops(), &f, 0, 8, r, NULL);
        CHECK(n == 4 && !strcmp(r[0].type, "reboot") && !strcmp(r[0].who, "anna") && !strcmp(r[0].source, "api") &&
                  !strcmp(r[3].type, "hostname_set") && !strcmp(r[3].detail, "nvr-2") && !strcmp(r[3].source, "system"),
              "the operation log: %d, %s by %s/%s", n, n ? r[0].type : "", n ? r[0].who : "", n ? r[0].source : "");
    }
    oplog_actor("", "system", "");
    mgmt_power_run(MGMT_POWER_OFF);
    CHECK(fake_ran("poweroff"), "poweroff run");
    mgmt_power_run(MGMT_POWER_REBOOT);
    CHECK(fake_ran("reboot"), "reboot run");

    mgmt_stop();
    free(c);
    /* Not OpenIPC: every setter refuses. */
    sys_set_managed(0);
    CHECK(mgmt_net_apply(&st, 1, field, sizeof field, err, sizeof err) == MGMT_EUNMANAGED &&
          mgmt_hostname_set("x", err, sizeof err) == MGMT_EUNMANAGED &&
          mgmt_power_request(MGMT_POWER_OFF) == MGMT_EUNMANAGED, "unmanaged: refused");
    sys_set_managed(1);
    fake_done();
}
