/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys_net.c — interfaces the OpenIPC way (see sys_net.h). */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/utsname.h>

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

/* ---- USB radios: Wi-Fi adapters and modems, by modalias ------------------------------- */

#define PROFILES_MAX 96
#define WIFI_PROFILES "/etc/wireless/usb"
#define MODEM_PROFILES "/etc/wireless/modem"

typedef struct {
    char name[48];
    char module[32];                /* the first module it loads */
    char ids[6][10];                /* "vvvv:pppp" the profile names */
    int  n_ids;
} profile_t;

static void hex4(const char *p, char *out)
{
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    size_t n = strspn(p, "0123456789abcdefABCDEF");
    snprintf(out, 5, "%.*s", (int)(n > 4 ? 4 : n), p);
    for (char *c = out; *c; ++c)
        if (*c >= 'A' && *c <= 'F') *c = (char)(*c + 32);
}

static void add_id(profile_t *pr, const char *v, const char *p)
{
    char a[5], b[5];
    hex4(v, a);
    hex4(p, b);
    if (strlen(a) == 4 && strlen(b) == 4 && pr->n_ids < 6)
        snprintf(pr->ids[pr->n_ids++], sizeof pr->ids[0], "%s:%s", a, b);
}

/* The profiles of a /etc/wireless script: each `if [ "$1" = "name" ]`
 * block, the first module it modprobes and the USB ids it names
 * (usb_modeswitch -v/-p, usbserial vendor=/product=, new_id). */
static int read_profiles(const char *file, profile_t *out, int max)
{
    char *buf = malloc(65536);
    if (!buf) return 0;
    int n = 0;
    profile_t *cur = NULL;
    if (sys_read(file, buf, 65536) > 0)
        for (char *line = buf; line && *line;) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            const char *t = skip_ws(line);
            char name[64];
            if (sscanf(t, "if [ \"$1\" = \"%63[^\"]\" ]", name) == 1 && n < max) {
                cur = &out[n++];
                memset(cur, 0, sizeof *cur);
                snprintf(cur->name, sizeof cur->name, "%.47s", name);
            } else if (!strcmp(t, "fi") && line == t) {
                cur = NULL;
            } else if (cur) {
                const char *m = strstr(t, "modprobe ");
                if (m && !cur->module[0]) {
                    m += 9;
                    while (*m == '-') {                 /* modprobe -q x */
                        m += strcspn(m, " ");
                        m = skip_ws(m);
                    }
                    snprintf(cur->module, sizeof cur->module, "%.*s", (int)strcspn(m, " \t;|&"), m);
                }
                const char *v = strstr(t, "vendor=0x"), *p = strstr(t, "product=0x");
                if (v && p) add_id(cur, v + 7, p + 8);
                v = strstr(t, "-v 0x");
                p = strstr(t, "-p 0x");
                if (v && p) add_id(cur, v + 3, p + 3);
                if (strstr(t, "new_id") && (v = strchr(t, '"')) && strlen(v) > 10) add_id(cur, v + 1, v + 6);
            }
            line = nl ? nl + 1 : NULL;
        }
    free(buf);
    return n;
}

/* modules.alias of the running kernel (of the only tree there, under a
 * test root). */
static char *read_aliases(void)
{
    char rel[160], p[512];
    struct utsname u;
    rel[0] = 0;
    if (uname(&u) == 0) {
        snprintf(rel, sizeof rel, "/lib/modules/%.64s/modules.alias", u.release);
        if (!sys_exists(rel)) rel[0] = 0;
    }
    if (!rel[0]) {
        DIR *d = opendir(sys_path(p, sizeof p, "/lib/modules"));
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL && !rel[0])
            if (e->d_name[0] != '.') {
                snprintf(rel, sizeof rel, "/lib/modules/%.64s/modules.alias", e->d_name);
                if (!sys_exists(rel)) rel[0] = 0;
            }
        if (d) closedir(d);
    }
    if (!rel[0]) return NULL;
    char *buf = malloc(1 << 20);
    if (buf && sys_read(rel, buf, 1 << 20) < 0) buf[0] = 0;
    return buf;
}

/* The module whose usb: alias matches `modalias`; "" none. */
static void module_for(const char *aliases, const char *modalias, char *out, size_t cap)
{
    out[0] = 0;
    for (const char *line = aliases; line && *line && !out[0];) {
        const char *nl = strchr(line, '\n');
        char l[256], pat[200], mod[64];
        size_t n = nl ? (size_t)(nl - line) : strlen(line);
        snprintf(l, sizeof l, "%.*s", (int)(n < sizeof l ? n : sizeof l - 1), line);
        if (sscanf(l, "alias %199s %63s", pat, mod) == 2 && !strncmp(pat, "usb:", 4) &&
            fnmatch(pat, modalias, 0) == 0)
            snprintf(out, cap, "%.31s", mod);
        line = nl ? nl + 1 : NULL;
    }
}

static int is_modem_module(const char *m)
{
    static const char *const mods[] = { "rndis_host", "cdc_ether", "cdc_ncm", "huawei_cdc_ncm", "qmi_wwan",
                                        "cdc_mbim", "option", NULL };
    for (int i = 0; mods[i]; ++i)
        if (!strcmp(m, mods[i])) return 1;
    return 0;
}

int usb_radios(int modems, usb_radio_t *out, int max)
{
    profile_t *pr = calloc(PROFILES_MAX, sizeof *pr);
    char *aliases = read_aliases();
    int np = pr ? read_profiles(modems ? MODEM_PROFILES : WIFI_PROFILES, pr, PROFILES_MAX) : 0, n = 0;
    char p[512];
    DIR *d = opendir(sys_path(p, sizeof p, "/sys/bus/usb/devices"));
    struct dirent *e;
    while (pr && d && (e = readdir(d)) != NULL && n < max) {
        const char *colon = strchr(e->d_name, ':');
        if (e->d_name[0] == '.' || !colon) continue;            /* interfaces only */
        char rel[300], dev[64], ma[256], v[8], pd[8];
        snprintf(dev, sizeof dev, "%.*s", (int)(colon - e->d_name), e->d_name);
        snprintf(rel, sizeof rel, "/sys/bus/usb/devices/%.60s/modalias", e->d_name);
        sys_read_line(rel, ma, sizeof ma);
        snprintf(rel, sizeof rel, "/sys/bus/usb/devices/%.60s/idVendor", dev);
        sys_read_line(rel, v, sizeof v);
        snprintf(rel, sizeof rel, "/sys/bus/usb/devices/%.60s/idProduct", dev);
        sys_read_line(rel, pd, sizeof pd);
        char id[10];
        snprintf(id, sizeof id, "%.4s:%.4s", v, pd);
        for (char *c = id; *c; ++c)
            if (*c >= 'A' && *c <= 'F') *c = (char)(*c + 32);
        int dup = 0;
        for (int i = 0; i < n; ++i) dup |= !strcmp(out[i].id, id);
        if (dup || strlen(id) != 9) continue;
        char mod[32] = "";
        if (aliases && ma[0]) module_for(aliases, ma, mod, sizeof mod);
        /* The profile: one naming this device (a modem in another mode
         * at first, e3372h), else the one loading its module; for Wi-Fi
         * only the "-generic" ones, the others power a camera's own
         * radio through its GPIOs. */
        const profile_t *hit = NULL;
        for (int i = 0; i < np && !hit; ++i)
            for (int k = 0; k < pr[i].n_ids && !hit; ++k)
                if (!strcmp(pr[i].ids[k], id)) hit = &pr[i];
        for (int i = 0; i < np && !hit && mod[0]; ++i)
            if (!strcmp(pr[i].module, mod) && (modems || strstr(pr[i].name, "-generic"))) hit = &pr[i];
        int ours = hit || (modems ? is_modem_module(mod) : 0);
        if (!ours) continue;
        usb_radio_t *r = &out[n++];
        memset(r, 0, sizeof *r);
        snprintf(r->id, sizeof r->id, "%s", id);
        snprintf(r->driver, sizeof r->driver, "%s", mod[0] ? mod : hit ? hit->module : "");
        if (hit) snprintf(r->profile, sizeof r->profile, "%s", hit->name);
        char mf[48], prod[48];
        snprintf(rel, sizeof rel, "/sys/bus/usb/devices/%.60s/manufacturer", dev);
        sys_read_line(rel, mf, sizeof mf);
        snprintf(rel, sizeof rel, "/sys/bus/usb/devices/%.60s/product", dev);
        sys_read_line(rel, prod, sizeof prod);
        snprintf(r->name, sizeof r->name, "%.30s%s%.31s", mf, mf[0] && prod[0] ? " " : "", prod);
        snprintf(rel, sizeof rel, "/sys/bus/usb/devices/%.60s/driver", e->d_name);
        r->bound = sys_exists(rel);
    }
    if (d) closedir(d);
    free(pr);
    free(aliases);
    return n;
}

static void wlandev(char *out, size_t cap)
{
    char o[128];
    out[0] = 0;
    if (sys_run(o, sizeof o, 5, "fw_printenv", "-n", "wlandev", NULL) == 0) {
        sys_trim(o);
        snprintf(out, cap, "%.47s", o);
    }
}

static int profile_in(const char *file, const char *name)
{
    profile_t *pr = calloc(PROFILES_MAX, sizeof *pr);
    int n = pr ? read_profiles(file, pr, PROFILES_MAX) : 0, found = 0;
    for (int i = 0; i < n && !found; ++i) found = !strcmp(pr[i].name, name);
    free(pr);
    return found;
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

static void wireless_if(char *out, size_t cap)
{
    char p[512];
    out[0] = 0;
    DIR *d = opendir(sys_path(p, sizeof p, "/sys/class/net"));
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.' && ifname_ok(e->d_name) && is_wireless(e->d_name) &&
            (!out[0] || strcmp(e->d_name, out) < 0))
            snprintf(out, cap, "%.15s", e->d_name);
    closedir(d);
}

void wifi_info(wifi_info_t *w)
{
    memset(w, 0, sizeof *w);
    wireless_if(w->ifname, sizeof w->ifname);
    w->n_adapters = usb_radios(0, w->adapters, USB_RADIOS_MAX);
    if (!w->ifname[0] && !w->n_adapters) return;
    char out[128];
    if (sys_run(out, sizeof out, 5, "fw_printenv", "-n", "wlanssid", NULL) == 0) {
        sys_trim(out);
        snprintf(w->ssid, sizeof w->ssid, "%.63s", out);
    }
    w->has_password = sys_run(out, sizeof out, 5, "fw_printenv", "-n", "wlanpass", NULL) == 0 &&
                      (sys_trim(out), out[0] != 0);
    memset(out, 0, sizeof out);
    w->can_scan = sys_have("iw");
    wlandev(w->profile, sizeof w->profile);
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

int wifi_set(const char *ifname, const char *ssid, const char *password, const char *profile, char *err,
             size_t cap)
{
    size_t pl = strlen(password), sl = strlen(ssid);
    if (ifname && *ifname && !ifname_ok(ifname)) {
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
    if (profile && *profile && !profile_in(WIFI_PROFILES, profile)) {
        snprintf(err, cap, "no Wi-Fi profile '%.47s' in %s", profile, WIFI_PROFILES);
        return -1;
    }
    char out[256], cur[48];
    wlandev(cur, sizeof cur);
    if ((profile && *profile && strcmp(profile, cur) &&
         sys_run(out, sizeof out, 10, "fw_setenv", "wlandev", profile, NULL) != 0) ||
        sys_run(out, sizeof out, 10, "fw_setenv", "wlanssid", ssid, NULL) != 0 ||
        sys_run(out, sizeof out, 10, "fw_setenv", "wlanpass", password, NULL) != 0) {
        sys_trim(out);
        snprintf(err, cap, "fw_setenv failed: %.200s", out);
        return -1;
    }
    char ifn[16];
    snprintf(ifn, sizeof ifn, "%s", ifname ? ifname : "");
    if (!ifn[0] && profile && *profile) {
        /* No interface yet: load the driver as S40network does at boot
         * (/etc/wireless/usb <profile>) and wait for it. */
        sys_run(out, sizeof out, 60, WIFI_PROFILES, profile, NULL);
        for (int i = 0; i < 20 && !ifn[0]; ++i) {
            wireless_if(ifn, sizeof ifn);
            if (!ifn[0]) usleep(500 * 1000);
        }
    }
    if (!ifn[0]) {
        snprintf(err, cap, "saved (wlandev %.47s); the adapter starts at the next boot", profile ? profile : cur);
        return 1;
    }
    return restart(ifn, err, cap);
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
    memset(l, 0, sizeof *l);
    char p[512];
    DIR *d = opendir(sys_path(p, sizeof p, "/sys/class/net"));
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && !l->ifname[0]) {
            if (e->d_name[0] == '.' || !ifname_ok(e->d_name)) continue;
            char drv[32];
            driver_of(e->d_name, drv, sizeof drv);
            if (strncmp(e->d_name, "wwan", 4) && !is_modem_module(drv)) continue;
            snprintf(l->ifname, sizeof l->ifname, "%.15s", e->d_name);
            snprintf(l->driver, sizeof l->driver, "%s", drv);
        }
        closedir(d);
    }
    l->n_modems = usb_radios(1, l->modems, USB_RADIOS_MAX);
    char cur[48];
    wlandev(cur, sizeof cur);
    if (cur[0] && profile_in(MODEM_PROFILES, cur)) snprintf(l->profile, sizeof l->profile, "%s", cur);
}

int lte_set_profile(const char *profile, char *err, size_t cap)
{
    if (!profile || !*profile || !profile_in(MODEM_PROFILES, profile)) {
        snprintf(err, cap, "no modem profile '%.47s' in %s", profile ? profile : "", MODEM_PROFILES);
        return -1;
    }
    char out[256];
    if (sys_run(out, sizeof out, 10, "fw_setenv", "wlandev", profile, NULL) != 0) {
        sys_trim(out);
        snprintf(err, cap, "fw_setenv failed: %.200s", out);
        return -1;
    }
    snprintf(err, cap, "wlandev is %.47s: the modem starts at the next boot", profile);
    return 0;
}
