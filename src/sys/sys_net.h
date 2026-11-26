/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys_net.h — the network interfaces, the OpenIPC way.
 *
 * OpenIPC firmware configures each interface with a busybox ifupdown
 * stanza in /etc/network/interfaces.d/<if> (sourced by
 * /etc/network/interfaces) and brings it up with ifup at boot
 * (/etc/init.d/S40network):
 *
 *   iface eth0 inet dhcp
 *       hwaddress ether $(get_mac eth0)
 *
 *   iface eth0 inet static
 *       address 192.168.1.10
 *       netmask 255.255.255.0
 *       gateway 192.168.1.1
 *       hwaddress ether $(get_mac eth0)
 *       pre-up echo -e "nameserver 1.1.1.1\nnameserver 8.8.8.8" >/tmp/resolv.conf
 *
 * DHCP leases bring their DNS servers through udhcpc's script
 * (/tmp/resolv.d, /tmp/resolv.conf; /etc/resolv.conf links there); a
 * static address writes them itself (the OpenIPC wiki's form above).
 * Wi-Fi credentials are in the U-Boot environment (fw_setenv wlanssid /
 * wlanpass), which the wlan0 stanza turns into a wpa_supplicant
 * configuration (wlan_conf).
 *
 * Only the lines vigild manages are rewritten: the method, address,
 * netmask, gateway, the DNS pre-up and an MTU pre-up. Anything else in
 * the stanza (hwaddress, metric, the Wi-Fi pre-ups, comments) is kept as
 * it was.
 */
#ifndef SYS_NET_H
#define SYS_NET_H

#include <stddef.h>

typedef struct {
    char ifname[16];
    int  dhcp;
    char address[16], netmask[16], gateway[16];
    char dns1[16], dns2[16];
    int  mtu;                       /* 0: the interface's default */
} net_cfg_t;

typedef struct {
    int  present;                   /* /sys/class/net/<if> */
    int  up, carrier;
    int  speed;                     /* Mb/s, 0 unknown */
    char duplex[8];
    char mac[18];
    int  mtu;
    char address[16], netmask[16], gateway[16];
    char dns1[46], dns2[46];        /* /etc/resolv.conf */
    int  stanza;                    /* /etc/network/interfaces.d/<if> exists */
} net_status_t;

/* The stanza in /etc/network/interfaces.d/<cfg->ifname> as settings; an
 * interface without one is DHCP. 0, or -1 with `err`. */
int net_read(const char *ifname, net_cfg_t *cfg, char *err, size_t cap);

/* What the interface has now (sysfs, its address, the default route in
 * /proc/net/route, /etc/resolv.conf). */
void net_status(const char *ifname, net_status_t *st);

/* Check `cfg`: addresses, a contiguous netmask, the gateway in the
 * subnet and not the address itself, DNS servers, MTU 576..9000. Returns
 * 0, or -1 with the field ("address", ...) and the reason. */
int net_validate(const net_cfg_t *cfg, char *field, size_t fcap, char *err, size_t cap);

/* The stanza text for `cfg`, keeping the unmanaged lines of `old` (the
 * file as it is, or NULL). malloc'd. */
char *net_stanza(const net_cfg_t *cfg, const char *old);

/* Write the stanza (atomically, 0644) and restart the interface:
 * ifdown -f, ifup. `old_text` gets the file as it was (malloc'd, NULL
 * when there was none) so the change can be undone with net_restore.
 * 0, or -1 with `err` (nothing written when validation fails). */
int net_apply(const net_cfg_t *cfg, char **old_text, char *err, size_t cap);

/* Put back a file net_apply replaced (NULL: there was none) and restart
 * the interface. */
int net_restore(const char *ifname, const char *old_text, char *err, size_t cap);

/* ---- Wi-Fi and LTE ---------------------------------------------------------------- */

typedef struct {
    char ifname[16];                /* "" when there is no wireless interface */
    char ssid[64];                  /* the U-Boot environment's wlanssid */
    int  has_password;
    int  can_scan;                  /* iw is there */
} wifi_info_t;

typedef struct {
    char ssid[64];
    int  signal;                    /* dBm */
    int  secured;
} wifi_net_t;

/* The first interface with /sys/class/net/<if>/wireless (wlan0, ...). */
void wifi_info(wifi_info_t *w);

/* Networks in range (`iw dev <if> scan`); -1 with `err` without iw. */
int wifi_scan(const char *ifname, wifi_net_t *out, int max, char *err, size_t cap);

/* Store the network to join (fw_setenv wlanssid / wlanpass, as the
 * firmware's own onboarding does) and restart the interface. An empty
 * password is an open network. */
int wifi_set(const char *ifname, const char *ssid, const char *password, char *err, size_t cap);

typedef struct {
    char ifname[16];                /* "" when no modem interface is there */
    char driver[32];                /* rndis_host, cdc_ether, qmi_wwan, ... */
    char profile[48];               /* the U-Boot environment's wlandev, when it names a modem */
} lte_info_t;

/* A modem's network interface: wwan*, or usb0 / eth1 from a USB modem
 * driver (OpenIPC brings modems up as RNDIS/ECM, /etc/wireless/modem). */
void lte_info(lte_info_t *l);

#endif /* SYS_NET_H */
