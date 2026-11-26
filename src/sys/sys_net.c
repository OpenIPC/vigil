/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys_net.c — interfaces the OpenIPC way (see sys_net.h). */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "sys.h"
#include "sys_net.h"

#define STANZA_MAX (16u << 10)
#define IFDOWN_SECS 20
/* ifup on DHCP: udhcpc tries for -t 5 -T 1 and then goes to the
 * background (-b, the firmware's options), so this is ample. */
#define IFUP_SECS   40

static void stanza_path(const char *ifname, char *buf, size_t cap)
{
    snprintf(buf, cap, "/etc/network/interfaces.d/%s", ifname);
}

static int ifname_ok(const char *n)
{
    size_t l = strlen(n);
    return l > 0 && l < 16 && strspn(n, "abcdefghijklmnopqrstuvwxyz0123456789_.-") == l && n[0] != '.';
}

/* ---- the stanza ------------------------------------------------------------------- */

/* A line that starts a new stanza (or is a top-level statement). */
static int top_level(const char *line)
{
    static const char *const kw[] = { "iface", "auto", "allow-", "mapping", "source", NULL };
    if (*line == ' ' || *line == '\t') return 0;
    for (int i = 0; kw[i]; ++i)
        if (!strncmp(line, kw[i], strlen(kw[i]))) return 1;
    return 0;
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

/* Lines vigild writes: the managed options, the DNS and MTU pre-ups. */
static int managed_line(const char *opt)
{
    static const char *const keys[] = { "address", "netmask", "gateway", "mtu", "dns-nameservers", NULL };
    for (int i = 0; keys[i]; ++i) {
        size_t n = strlen(keys[i]);
        if (!strncmp(opt, keys[i], n) && (opt[n] == ' ' || opt[n] == '\t' || !opt[n])) return 1;
    }
    if (!strncmp(opt, "pre-up", 6) && (strstr(opt, "resolv.conf") || strstr(opt, " mtu "))) return 1;
    return 0;
}

/* Every dotted quad in `s`, in order, into a/b. */
static void quads(const char *s, char *a, char *b, size_t cap)
{
    int found = 0;
    for (const char *p = s; *p && found < 2; ++p) {
        if (*p < '0' || *p > '9' || (p > s && (p[-1] == '.' || (p[-1] >= '0' && p[-1] <= '9')))) continue;
        size_t n = strspn(p, "0123456789.");
        char q[20];
        if (n >= sizeof q) continue;
        memcpy(q, p, n);
        q[n] = 0;
        struct in_addr ia;
        if (inet_pton(AF_INET, q, &ia) == 1) {
            snprintf(found ? b : a, cap, "%.15s", q);
            ++found;
        }
        p += n - 1;
    }
}

static void parse_stanza(const char *text, const char *ifname, net_cfg_t *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    snprintf(cfg->ifname, sizeof cfg->ifname, "%s", ifname);
    cfg->dhcp = 1;
    int in = 0;
    for (const char *line = text; line && *line;) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        char l[512];
        snprintf(l, sizeof l, "%.*s", (int)(len < sizeof l ? len : sizeof l - 1), line);
        line = nl ? nl + 1 : NULL;
        if (top_level(l)) {
            char name[32], fam[16], method[16];
            in = sscanf(l, "iface %31s %15s %15s", name, fam, method) == 3 && !strcmp(name, ifname) &&
                 !strcmp(fam, "inet");
            if (in) cfg->dhcp = strcmp(method, "static") != 0;
            continue;
        }
        if (!in) continue;
        const char *o = skip_ws(l);
        char v[64];
        if (sscanf(o, "address %63s", v) == 1) snprintf(cfg->address, sizeof cfg->address, "%.15s", v);
        else if (sscanf(o, "netmask %63s", v) == 1) snprintf(cfg->netmask, sizeof cfg->netmask, "%.15s", v);
        else if (sscanf(o, "gateway %63s", v) == 1) snprintf(cfg->gateway, sizeof cfg->gateway, "%.15s", v);
        else if (sscanf(o, "mtu %63s", v) == 1) cfg->mtu = atoi(v);
        else if (!strncmp(o, "pre-up", 6) && strstr(o, "resolv.conf"))
            quads(o, cfg->dns1, cfg->dns2, sizeof cfg->dns1);
        else if (!strncmp(o, "pre-up", 6) && strstr(o, " mtu ")) cfg->mtu = atoi(strstr(o, " mtu ") + 5);
    }
    /* "address 192.168.1.10/24" is valid ifupdown too. */
    char *slash = strchr(cfg->address, '/');
    if (slash) {
        int bits = atoi(slash + 1);
        *slash = 0;
        if (!cfg->netmask[0] && bits > 0 && bits <= 32) {
            struct in_addr m = { htonl(bits == 32 ? 0xffffffffu : ~(0xffffffffu >> bits)) };
            inet_ntop(AF_INET, &m, cfg->netmask, sizeof cfg->netmask);
        }
    }
}

int net_read(const char *ifname, net_cfg_t *cfg, char *err, size_t cap)
{
    if (!ifname_ok(ifname)) {
        snprintf(err, cap, "not an interface name");
        return -1;
    }
    char path[64], *text = malloc(STANZA_MAX);
    if (!text) {
        snprintf(err, cap, "out of memory");
        return -1;
    }
    stanza_path(ifname, path, sizeof path);
    if (sys_read(path, text, STANZA_MAX) < 0) text[0] = 0;
    parse_stanza(text, ifname, cfg);
    free(text);
    return 0;
}

static int ip4(const char *s, uint32_t *v)
{
    struct in_addr a;
    if (inet_pton(AF_INET, s, &a) != 1) return -1;
    if (v) *v = ntohl(a.s_addr);
    return 0;
}

#define BAD(f, ...) do { snprintf(field, fcap, "%s", f); snprintf(err, cap, __VA_ARGS__); return -1; } while (0)

int net_validate(const net_cfg_t *c, char *field, size_t fcap, char *err, size_t cap)
{
    if (!ifname_ok(c->ifname)) BAD("ifname", "not an interface name");
    if (c->mtu && (c->mtu < 576 || c->mtu > 9000)) BAD("mtu", "576..9000, or 0 for the default");
    if (c->dhcp) return 0;
    uint32_t a, m, g = 0, d;
    if (ip4(c->address, &a) < 0) BAD("address", "not an IPv4 address");
    if (ip4(c->netmask, &m) < 0) BAD("netmask", "not an IPv4 netmask");
    if (m == 0 || (~m & (~m + 1)) != 0) BAD("netmask", "not a netmask (255.255.255.0, ...)");
    if ((a >> 24) == 0 || (a >> 24) == 127 || (a >> 24) >= 224) BAD("address", "not a usable address");
    if (m != 0xffffffffu && m != 0xfffffffeu && ((a & ~m) == 0 || (a & ~m) == ~m))
        BAD("address", "the subnet's own or broadcast address");
    if (c->gateway[0]) {
        if (ip4(c->gateway, &g) < 0) BAD("gateway", "not an IPv4 address");
        if ((g & m) != (a & m)) BAD("gateway", "not in the subnet of the address");
        if (g == a) BAD("gateway", "the address itself");
    }
    if (c->dns1[0] && ip4(c->dns1, &d) < 0) BAD("dns1", "not an IPv4 address");
    if (c->dns2[0] && ip4(c->dns2, &d) < 0) BAD("dns2", "not an IPv4 address");
    return 0;
}

char *net_stanza(const net_cfg_t *c, const char *old)
{
    size_t cap = (old ? strlen(old) : 0) + 1024, len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    out[0] = 0;
#define PUT(...) do { int n_ = snprintf(out + len, cap - len, __VA_ARGS__); \
                      if (n_ > 0) len += (size_t)n_ < cap - len ? (size_t)n_ : cap - len - 1; } while (0)
    int done = 0, in = 0;
    /* The lines of the old stanza vigild does not manage, written after
     * its own: hwaddress, metric, the Wi-Fi pre-ups, comments. */
    for (const char *line = old; line && *line;) {
        const char *nl = strchr(line, '\n');
        size_t n = nl ? (size_t)(nl - line) : strlen(line);
        char l[512];
        snprintf(l, sizeof l, "%.*s", (int)(n < sizeof l ? n : sizeof l - 1), line);
        line = nl ? nl + 1 : NULL;
        if (top_level(l)) {
            char name[32], fam[16];
            in = sscanf(l, "iface %31s %15s", name, fam) == 2 && !strcmp(name, c->ifname) &&
                 !strcmp(fam, "inet") && !done;
            if (in) {
                PUT("iface %s inet %s\n", c->ifname, c->dhcp ? "dhcp" : "static");
                if (!c->dhcp) {
                    PUT("    address %s\n    netmask %s\n", c->address, c->netmask);
                    if (c->gateway[0]) PUT("    gateway %s\n", c->gateway);
                    if (c->dns1[0] || c->dns2[0]) {
                        PUT("    pre-up echo -e \"");
                        if (c->dns1[0]) PUT("nameserver %s", c->dns1);
                        if (c->dns1[0] && c->dns2[0]) PUT("\\n");
                        if (c->dns2[0]) PUT("nameserver %s", c->dns2);
                        PUT("\" >/tmp/resolv.conf\n");
                    }
                }
                if (c->mtu) PUT("    pre-up ip link set dev %s mtu %d\n", c->ifname, c->mtu);
                done = 1;
                continue;
            }
            PUT("%s\n", l);
            continue;
        }
        if (in && managed_line(skip_ws(l))) continue;
        PUT("%s\n", l);
    }
    if (!done) {
        if (len && out[len - 1] != '\n') PUT("\n");
        net_cfg_t copy = *c;
        free(out);
        char head[64];
        snprintf(head, sizeof head, "iface %s inet dhcp\n", copy.ifname);
        char *more = malloc(strlen(old ? old : "") + strlen(head) + 2);
        if (!more) return NULL;
        /* No stanza for it yet: the old text, then a new one. */
        snprintf(more, strlen(old ? old : "") + strlen(head) + 2, "%s%s%s", old ? old : "",
                 old && *old && old[strlen(old) - 1] != '\n' ? "\n" : "", head);
        char *r = net_stanza(&copy, more);
        free(more);
        return r;
    }
#undef PUT
    return out;
}

/* Taken down with the stanza it was brought up with: busybox ifdown
 * reads the file as it is now, so after a DHCP -> static change it would
 * "take down" a static address and leave the DHCP client running, which
 * then renews its lease over the new address. -f: also when ifupdown's
 * state file does not list it (S40network brought eth0 up by other means
 * in some modes). */
static void if_down(const char *ifname)
{
    char out[512];
    sys_run(out, sizeof out, IFDOWN_SECS, "ifdown", "-f", ifname, NULL);
}

static int if_up(const char *ifname, char *err, size_t cap)
{
    char out[512];
    int rc = sys_run(out, sizeof out, IFUP_SECS, "ifup", ifname, NULL);
    if (rc != 0) {
        sys_trim(out);
        snprintf(err, cap, "ifup %s failed (%d)%s%.200s", ifname, rc, out[0] ? ": " : "", out);
        return -1;
    }
    return 0;
}

static int restart(const char *ifname, char *err, size_t cap)
{
    if_down(ifname);
    return if_up(ifname, err, cap);
}

int net_apply(const net_cfg_t *cfg, char **old_text, char *err, size_t cap)
{
    char field[32];
    *old_text = NULL;
    if (net_validate(cfg, field, sizeof field, err, cap) < 0) return -1;
    char path[64];
    stanza_path(cfg->ifname, path, sizeof path);
    char *old = malloc(STANZA_MAX);
    if (!old) {
        snprintf(err, cap, "out of memory");
        return -1;
    }
    if (sys_read(path, old, STANZA_MAX) < 0) {
        free(old);
        old = NULL;
    }
    char *text = net_stanza(cfg, old);
    if (!text) {
        snprintf(err, cap, "out of memory");
        free(old);
        return -1;
    }
    if_down(cfg->ifname);
    if (sys_write(path, text, strlen(text), 0644, err, cap) < 0) {
        char e2[64];
        if_up(cfg->ifname, e2, sizeof e2);      /* the old file is still there */
        free(text);
        free(old);
        return -1;
    }
    free(text);
    *old_text = old;
    return if_up(cfg->ifname, err, cap);
}

int net_restore(const char *ifname, const char *old_text, char *err, size_t cap)
{
    char path[64], p[512];
    if (!ifname_ok(ifname)) {
        snprintf(err, cap, "not an interface name");
        return -1;
    }
    stanza_path(ifname, path, sizeof path);
    if_down(ifname);
    if (old_text) {
        if (sys_write(path, old_text, strlen(old_text), 0644, err, cap) < 0) {
            char e2[64];
            if_up(ifname, e2, sizeof e2);
            return -1;
        }
    } else if (unlink(sys_path(p, sizeof p, path)) < 0 && errno != ENOENT) {
        snprintf(err, cap, "cannot remove %s: %s", path, strerror(errno));
        char e2[64];
        if_up(ifname, e2, sizeof e2);
        return -1;
    }
    return if_up(ifname, err, cap);
}

/* ---- status --------------------------------------------------------------------- */

static void default_gw(const char *ifname, char *gw, size_t cap)
{
    char *buf = malloc(16384);
    gw[0] = 0;
    if (!buf) return;
    if (sys_read("/proc/net/route", buf, 16384) > 0) {
        for (char *line = strchr(buf, '\n'); line && *++line;) {
            char ifc[32];
            unsigned long dst, g, mask;
            unsigned flags;
            int refcnt, use, metric;
            if (sscanf(line, "%31s %lx %lx %x %d %d %d %lx", ifc, &dst, &g, &flags, &refcnt, &use, &metric,
                       &mask) == 8 && !strcmp(ifc, ifname) && dst == 0 && mask == 0 && (flags & 2)) {
                /* The kernel prints the network-order word as a native
                 * integer: its bytes in memory are the address. */
                struct in_addr a = { (uint32_t)g };
                inet_ntop(AF_INET, &a, gw, (socklen_t)cap);
                break;
            }
            line = strchr(line, '\n');
        }
    }
    free(buf);
}

void net_status(const char *ifname, net_status_t *st)
{
    memset(st, 0, sizeof *st);
    if (!ifname_ok(ifname)) return;
    char p[128], v[64];
    snprintf(p, sizeof p, "/sys/class/net/%s", ifname);
    st->present = sys_exists(p);
    if (st->present) {
        snprintf(p, sizeof p, "/sys/class/net/%s/flags", ifname);
        char fl[32];
        st->up = (strtol(sys_read_line(p, fl, sizeof fl), NULL, 0) & 1) != 0;
        snprintf(p, sizeof p, "/sys/class/net/%s/carrier", ifname);
        st->carrier = st->up && sys_read_ll(p, 0) == 1;
        snprintf(p, sizeof p, "/sys/class/net/%s/speed", ifname);
        long long sp = st->carrier ? sys_read_ll(p, 0) : 0;
        st->speed = sp > 0 && sp < 1000000 ? (int)sp : 0;
        snprintf(p, sizeof p, "/sys/class/net/%s/duplex", ifname);
        if (st->carrier) snprintf(st->duplex, sizeof st->duplex, "%s", sys_read_line(p, v, sizeof v));
        snprintf(p, sizeof p, "/sys/class/net/%s/address", ifname);
        snprintf(st->mac, sizeof st->mac, "%s", sys_read_line(p, v, sizeof v));
        snprintf(p, sizeof p, "/sys/class/net/%s/mtu", ifname);
        st->mtu = (int)sys_read_ll(p, 0);
        sys_ops()->ifaddr(ifname, st->address, st->netmask, sizeof st->address);
        default_gw(ifname, st->gateway, sizeof st->gateway);
    }
    stanza_path(ifname, p, sizeof p);
    st->stanza = sys_exists(p);
    char rc[2048];
    if (sys_read("/etc/resolv.conf", rc, sizeof rc) > 0) {
        int n = 0;
        for (char *line = rc; line && *line && n < 2;) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            char ns[64];
            if (sscanf(line, " nameserver %63s", ns) == 1)
                snprintf(n++ ? st->dns2 : st->dns1, sizeof st->dns1, "%.45s", ns);
            line = nl ? nl + 1 : NULL;
        }
    }
}

/* ---- Wi-Fi ---------------------------------------------------------------------- */

static int is_wireless(const char *ifname)
{
    char p[128];
    snprintf(p, sizeof p, "/sys/class/net/%.15s/wireless", ifname);
    if (sys_exists(p)) return 1;
    snprintf(p, sizeof p, "/sys/class/net/%.15s/phy80211", ifname);
    return sys_exists(p);
}

void wifi_info(wifi_info_t *w)
{
    memset(w, 0, sizeof *w);
    char p[512];
    DIR *d = opendir(sys_path(p, sizeof p, "/sys/class/net"));
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.' && ifname_ok(e->d_name) && is_wireless(e->d_name) &&
            (!w->ifname[0] || strcmp(e->d_name, w->ifname) < 0))
            snprintf(w->ifname, sizeof w->ifname, "%.15s", e->d_name);
    closedir(d);
    if (!w->ifname[0]) return;
    char out[128];
    if (sys_run(out, sizeof out, 5, "fw_printenv", "-n", "wlanssid", NULL) == 0) {
        sys_trim(out);
        snprintf(w->ssid, sizeof w->ssid, "%.63s", out);
    }
    w->has_password = sys_run(out, sizeof out, 5, "fw_printenv", "-n", "wlanpass", NULL) == 0 &&
                      (sys_trim(out), out[0] != 0);
    memset(out, 0, sizeof out);
    w->can_scan = sys_have("iw");
}

int wifi_scan(const char *ifname, wifi_net_t *out, int max, char *err, size_t cap)
{
    if (!ifname_ok(ifname)) {
        snprintf(err, cap, "not an interface name");
        return -1;
    }
    if (!sys_have("iw")) {
        snprintf(err, cap, "this firmware has no iw to scan with");
        return -1;
    }
    char *buf = malloc(32768);
    if (!buf) {
        snprintf(err, cap, "out of memory");
        return -1;
    }
    sys_run(NULL, 0, 5, "ip", "link", "set", "dev", ifname, "up", NULL);
    int rc = sys_run(buf, 32768, 20, "iw", "dev", ifname, "scan", NULL);
    if (rc != 0) {
        sys_trim(buf);
        snprintf(err, cap, "scan failed: %.200s", buf);
        free(buf);
        return -1;
    }
    int n = -1;
    for (char *line = buf; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        const char *t = skip_ws(line);
        if (!strncmp(t, "BSS ", 4)) {
            if (n + 1 < max) {
                ++n;
                memset(&out[n], 0, sizeof out[n]);
                out[n].signal = -100;
            }
        } else if (n >= 0) {
            float dbm;
            if (sscanf(t, "signal: %f dBm", &dbm) == 1) out[n].signal = (int)dbm;
            else if (!strncmp(t, "SSID: ", 6)) snprintf(out[n].ssid, sizeof out[n].ssid, "%s", t + 6);
            else if (!strncmp(t, "RSN:", 4) || !strncmp(t, "WPA:", 4)) out[n].secured = 1;
        }
        line = nl ? nl + 1 : NULL;
    }
    free(buf);
    /* Hidden networks (no SSID) are left out; one entry per name, the
     * strongest. */
    int k = 0;
    for (int i = 0; i <= n; ++i) {
        if (!out[i].ssid[0]) continue;
        int dup = -1;
        for (int j = 0; j < k; ++j)
            if (!strcmp(out[j].ssid, out[i].ssid)) dup = j;
        if (dup < 0) out[k++] = out[i];
        else if (out[i].signal > out[dup].signal) out[dup] = out[i];
    }
    return k;
}

int wifi_set(const char *ifname, const char *ssid, const char *password, char *err, size_t cap)
{
    size_t pl = strlen(password), sl = strlen(ssid);
    if (!ifname_ok(ifname)) {
        snprintf(err, cap, "not an interface name");
        return -1;
    }
    if (sl < 1 || sl > 32) {
        snprintf(err, cap, "an SSID is 1 to 32 bytes");
        return -1;
    }
    /* wlan_conf: empty = open, 64 hex digits = a raw key, else a WPA
     * passphrase of 8 to 63 characters. */
    int hex = pl == 64 && strspn(password, "0123456789abcdefABCDEF") == 64;
    if (pl && !hex && (pl < 8 || pl > 63)) {
        snprintf(err, cap, "a WPA passphrase is 8 to 63 characters");
        return -1;
    }
    for (const char *p = ssid; *p; ++p)
        if ((unsigned char)*p < 0x20) {
            snprintf(err, cap, "no control characters in the SSID");
            return -1;
        }
    char out[256];
    if (sys_run(out, sizeof out, 10, "fw_setenv", "wlanssid", ssid, NULL) != 0 ||
        sys_run(out, sizeof out, 10, "fw_setenv", "wlanpass", password, NULL) != 0) {
        sys_trim(out);
        snprintf(err, cap, "fw_setenv failed: %.200s", out);
        return -1;
    }
    return restart(ifname, err, cap);
}

/* ---- LTE ------------------------------------------------------------------------ */

static void driver_of(const char *ifname, char *out, size_t cap)
{
    char rel[128], p[512], link[512];
    out[0] = 0;
    snprintf(rel, sizeof rel, "/sys/class/net/%.15s/device/driver", ifname);
    ssize_t n = readlink(sys_path(p, sizeof p, rel), link, sizeof link - 1);
    if (n <= 0) return;
    link[n] = 0;
    const char *b = strrchr(link, '/');
    snprintf(out, cap, "%.31s", b ? b + 1 : link);
}

void lte_info(lte_info_t *l)
{
    static const char *const modem_drivers[] = { "rndis_host", "cdc_ether", "cdc_ncm", "huawei_cdc_ncm",
                                                 "qmi_wwan", "cdc_mbim", "option", NULL };
    memset(l, 0, sizeof *l);
    char p[512];
    DIR *d = opendir(sys_path(p, sizeof p, "/sys/class/net"));
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && !l->ifname[0]) {
            if (e->d_name[0] == '.' || !ifname_ok(e->d_name)) continue;
            char drv[32];
            driver_of(e->d_name, drv, sizeof drv);
            int modem = !strncmp(e->d_name, "wwan", 4);
            for (int i = 0; modem_drivers[i] && !modem; ++i) modem = !strcmp(drv, modem_drivers[i]);
            if (!modem) continue;
            snprintf(l->ifname, sizeof l->ifname, "%.15s", e->d_name);
            snprintf(l->driver, sizeof l->driver, "%s", drv);
        }
        closedir(d);
    }
    char out[128], modem[8192];
    if (sys_run(out, sizeof out, 5, "fw_printenv", "-n", "wlandev", NULL) == 0) {
        sys_trim(out);
        char pat[160];
        snprintf(pat, sizeof pat, "\"%s\"", out);
        if (out[0] && sys_read("/etc/wireless/modem", modem, sizeof modem) > 0 && strstr(modem, pat))
            snprintf(l->profile, sizeof l->profile, "%.47s", out);
    }
}
