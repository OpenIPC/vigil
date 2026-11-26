/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_sys_net.c — interfaces on a fake system (sys_net.h). */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "sys.h"
#include "sys_fake.h"
#include "sys_net.h"

void test_sys_net(void);

/* The firmware's own eth0 file. */
#define FW_ETH0 "iface eth0 inet dhcp\n    hwaddress ether $(get_mac eth0)\n"

void test_sys_net(void)
{
    fake_init();
    fake_file("/etc/network/interfaces.d/eth0", FW_ETH0);
    net_cfg_t c;
    char err[256], field[32];
    CHECK(net_read("eth0", &c, err, sizeof err) == 0 && c.dhcp && !c.address[0], "firmware eth0: DHCP");
    CHECK(net_read("../x", &c, err, sizeof err) < 0, "an interface name with a path is refused");

    /* Validation */
    net_cfg_t s = { .ifname = "eth0", .dhcp = 0, .address = "192.168.1.10", .netmask = "255.255.255.0",
                    .gateway = "192.168.1.1", .dns1 = "1.1.1.1", .dns2 = "8.8.8.8" };
    CHECK(net_validate(&s, field, sizeof field, err, sizeof err) == 0, "valid static: %s", err);
    net_cfg_t b = s;
    snprintf(b.netmask, sizeof b.netmask, "255.0.255.0");
    CHECK(net_validate(&b, field, sizeof field, err, sizeof err) < 0 && !strcmp(field, "netmask"),
          "non-contiguous netmask refused");
    b = s;
    snprintf(b.gateway, sizeof b.gateway, "10.0.0.1");
    CHECK(net_validate(&b, field, sizeof field, err, sizeof err) < 0 && !strcmp(field, "gateway"),
          "gateway outside the subnet refused");
    b = s;
    snprintf(b.address, sizeof b.address, "192.168.1.255");
    CHECK(net_validate(&b, field, sizeof field, err, sizeof err) < 0 && !strcmp(field, "address"),
          "broadcast address refused");
    b = s;
    snprintf(b.address, sizeof b.address, "300.1.1.1");
    CHECK(net_validate(&b, field, sizeof field, err, sizeof err) < 0, "not an address");
    b = s;
    snprintf(b.dns2, sizeof b.dns2, "dns.example");
    CHECK(net_validate(&b, field, sizeof field, err, sizeof err) < 0 && !strcmp(field, "dns2"), "bad DNS");
    b = s;
    b.mtu = 100;
    CHECK(net_validate(&b, field, sizeof field, err, sizeof err) < 0 && !strcmp(field, "mtu"), "bad MTU");
    b = s;
    b.dhcp = 1;
    b.address[0] = 0;
    CHECK(net_validate(&b, field, sizeof field, err, sizeof err) == 0, "DHCP needs no address");

    /* The stanza keeps what vigild does not manage. */
    char *t = net_stanza(&s, FW_ETH0);
    CHECK(t && strstr(t, "iface eth0 inet static\n") && strstr(t, "    address 192.168.1.10\n") &&
          strstr(t, "    netmask 255.255.255.0\n") && strstr(t, "    gateway 192.168.1.1\n") &&
          strstr(t, "nameserver 1.1.1.1\\nnameserver 8.8.8.8\" >/tmp/resolv.conf") &&
          strstr(t, "hwaddress ether $(get_mac eth0)"), "static stanza:\n%s", t);
    net_cfg_t back;
    fake_file("/etc/network/interfaces.d/eth0", t);
    net_read("eth0", &back, err, sizeof err);
    CHECK(!back.dhcp && !strcmp(back.address, s.address) && !strcmp(back.netmask, s.netmask) &&
          !strcmp(back.gateway, s.gateway) && !strcmp(back.dns1, "1.1.1.1") && !strcmp(back.dns2, "8.8.8.8"),
          "read back: %s %s %s %s %s", back.address, back.netmask, back.gateway, back.dns1, back.dns2);
    net_cfg_t d = { .ifname = "eth0", .dhcp = 1, .mtu = 1400 };
    char *t2 = net_stanza(&d, t);
    CHECK(t2 && strstr(t2, "iface eth0 inet dhcp\n") && !strstr(t2, "    address") && !strstr(t2, "resolv.conf") &&
          strstr(t2, "pre-up ip link set dev eth0 mtu 1400") && strstr(t2, "hwaddress"),
          "back to DHCP drops the static lines:\n%s", t2);
    free(t2);
    free(t);
    /* Other stanzas and comments in the file stay. */
    const char *multi = "# keep me\nauto eth0\niface eth0 inet dhcp\n    metric 5\n\niface eth0:1 inet static\n"
                        "    address 10.1.1.1\n    netmask 255.0.0.0\n";
    t = net_stanza(&s, multi);
    CHECK(t && strstr(t, "# keep me\nauto eth0\niface eth0 inet static") && strstr(t, "    metric 5") &&
          strstr(t, "iface eth0:1 inet static\n    address 10.1.1.1\n"), "other stanzas kept:\n%s", t);
    free(t);
    t = net_stanza(&s, NULL);
    CHECK(t && !strncmp(t, "iface eth0 inet static\n", 23), "no file: a new stanza:\n%s", t);
    free(t);
    /* "address a.b.c.d/nn" */
    fake_file("/etc/network/interfaces.d/eth0", "iface eth0 inet static\n  address 10.0.2.15/24\n");
    net_read("eth0", &back, err, sizeof err);
    CHECK(!strcmp(back.address, "10.0.2.15") && !strcmp(back.netmask, "255.255.255.0"), "CIDR address: %s %s",
          back.address, back.netmask);

    /* Apply: written atomically, the old text returned, ifdown/ifup run. */
    fake_file("/etc/network/interfaces.d/eth0", FW_ETH0);
    fake_clear_log();
    fake_snapshot("ifdown", "/etc/network/interfaces.d/eth0");
    char *old = NULL;
    CHECK(net_apply(&s, &old, err, sizeof err) == 0, "apply: %s", err);
    CHECK(old && !strcmp(old, FW_ETH0), "the old file is returned");
    CHECK(fake_ran("ifdown -f eth0\nifup eth0\n"), "restarted: %s", fake_log);
    CHECK(strstr(fake_snap, "inet dhcp"), "ifdown saw the old (DHCP) stanza, so it stops udhcpc: %s", fake_snap);
    char *now = fake_read("/etc/network/interfaces.d/eth0");
    CHECK(now && strstr(now, "inet static"), "written");
    free(now);
    fake_clear_log();
    fake_snapshot("ifdown", "/etc/network/interfaces.d/eth0");
    CHECK(net_restore("eth0", old, err, sizeof err) == 0, "restore: %s", err);
    CHECK(strstr(fake_snap, "inet static"), "restore: ifdown saw the stanza in force: %s", fake_snap);
    now = fake_read("/etc/network/interfaces.d/eth0");
    CHECK(now && !strcmp(now, FW_ETH0) && fake_ran("ifup eth0"), "restored and restarted");
    free(now);
    free(old);
    fake_answer("ifup", 1, "udhcpc: no lease");
    CHECK(net_apply(&s, &old, err, sizeof err) < 0 && strstr(err, "no lease"), "ifup failure reported: %s", err);
    free(old);
    old = NULL;
    fake_answer("ifup", 0, "");
    b = s;
    snprintf(b.netmask, sizeof b.netmask, "nope");
    fake_file("/etc/network/interfaces.d/eth0", FW_ETH0);
    fake_clear_log();
    CHECK(net_apply(&b, &old, err, sizeof err) < 0 && !old && !fake_ran("ifup"), "invalid: nothing written");
    now = fake_read("/etc/network/interfaces.d/eth0");
    CHECK(now && !strcmp(now, FW_ETH0), "invalid: the file is untouched");
    free(now);
    CHECK(net_restore("eth0", NULL, err, sizeof err) == 0 && !fake_exists("/etc/network/interfaces.d/eth0"),
          "restore to no file removes it");

    /* Status */
    fake_file("/sys/class/net/eth0/flags", "0x1003\n");
    fake_file("/sys/class/net/eth0/carrier", "1\n");
    fake_file("/sys/class/net/eth0/speed", "100\n");
    fake_file("/sys/class/net/eth0/duplex", "full\n");
    fake_file("/sys/class/net/eth0/address", "00:11:22:33:44:55\n");
    fake_file("/sys/class/net/eth0/mtu", "1500\n");
    fake_file("/proc/net/route", "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
                                 "eth0\t00000000\t0202000A\t0003\t0\t0\t0\t00000000\t0\t0\t0\n"
                                 "eth0\t0002000A\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0\n");
    fake_file("/etc/resolv.conf", "search lan\nnameserver 10.0.2.3\n");
    fake_ifaddr("eth0", "10.0.2.15", "255.255.255.0");
    net_status_t st;
    net_status("eth0", &st);
    CHECK(st.present && st.up && st.carrier && st.speed == 100 && !strcmp(st.mac, "00:11:22:33:44:55") &&
          st.mtu == 1500 && !strcmp(st.address, "10.0.2.15") && !strcmp(st.netmask, "255.255.255.0") &&
          !strcmp(st.gateway, "10.0.2.2") && !strcmp(st.dns1, "10.0.2.3") && !st.dns2[0],
          "status: %d %d %d %s %s %s %s", st.present, st.carrier, st.speed, st.mac, st.address, st.gateway,
          st.dns1);
    fake_file("/sys/class/net/eth0/carrier", "0\n");
    net_status("eth0", &st);
    CHECK(!st.carrier && st.speed == 0, "no cable: no speed");
    net_status("eth9", &st);
    CHECK(!st.present, "no such interface");

    /* Wi-Fi: none, then one; scan with and without iw. */
    wifi_info_t w;
    wifi_info(&w);
    CHECK(!w.ifname[0], "no wireless interface");
    fake_mkdir("/sys/class/net/wlan0/wireless");
    fake_answer("fw_printenv", 0, "HomeNet\n");
    fake_have("iw", 0);
    wifi_info(&w);
    CHECK(!strcmp(w.ifname, "wlan0") && !strcmp(w.ssid, "HomeNet") && !w.can_scan, "wlan0 found: %s %s",
          w.ifname, w.ssid);
    wifi_net_t nets[8];
    CHECK(wifi_scan("wlan0", nets, 8, err, sizeof err) < 0 && strstr(err, "iw"), "no iw: %s", err);
    fake_have("iw", 1);
    fake_answer("iw", 0, "BSS 00:11:22:33:44:55(on wlan0)\n\tsignal: -48.00 dBm\n\tSSID: HomeNet\n\tRSN:\t * Version: 1\n"
                         "BSS 00:11:22:33:44:66(on wlan0)\n\tsignal: -70.00 dBm\n\tSSID: Cafe\n"
                         "BSS 00:11:22:33:44:77(on wlan0)\n\tsignal: -40.00 dBm\n\tSSID: HomeNet\n\tRSN:\n"
                         "BSS 00:11:22:33:44:88(on wlan0)\n\tsignal: -60.00 dBm\n\tSSID: \n");
    int n = wifi_scan("wlan0", nets, 8, err, sizeof err);
    CHECK(n == 2 && !strcmp(nets[0].ssid, "HomeNet") && nets[0].signal == -40 && nets[0].secured &&
          !strcmp(nets[1].ssid, "Cafe") && !nets[1].secured, "scan: %d", n);
    CHECK(wifi_set("wlan0", "HomeNet", "short", err, sizeof err) < 0, "a 5-character passphrase is refused");
    fake_clear_log();
    CHECK(wifi_set("wlan0", "HomeNet", "longenough", err, sizeof err) == 0 &&
          fake_ran("fw_setenv wlanssid HomeNet\nfw_setenv wlanpass longenough\nifdown -f wlan0\nifup wlan0"),
          "wifi_set: %s", fake_log);

    /* LTE: a modem's RNDIS interface, and the firmware's modem profile. */
    lte_info_t l;
    fake_answer("fw_printenv", 0, "");
    lte_info(&l);
    CHECK(!l.ifname[0], "no modem");
    fake_mkdir("/sys/bus/usb/drivers/rndis_host");
    fake_mkdir("/sys/class/net/usb0/device");
    char a[512], b2[512];
    snprintf(a, sizeof a, "%s/sys/bus/usb/drivers/rndis_host", fake_root);
    snprintf(b2, sizeof b2, "%s/sys/class/net/usb0/device/driver", fake_root);
    CHECK(symlink(a, b2) == 0, "symlink");
    fake_file("/etc/wireless/modem", "if [ \"$1\" = \"rndis-e3372h\" ]; then\n");
    fake_answer("fw_printenv", 0, "rndis-e3372h\n");
    lte_info(&l);
    CHECK(!strcmp(l.ifname, "usb0") && !strcmp(l.driver, "rndis_host") && !strcmp(l.profile, "rndis-e3372h"),
          "modem: %s %s %s", l.ifname, l.driver, l.profile);
    fake_done();
}
