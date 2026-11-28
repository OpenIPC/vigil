/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_mgmt.c — the management pages' model (ui_model.h: ui_mgmt_t) on top
 * of mgmt.h, the system backends and the accounts file. The UI does not
 * include any of them; this file copies between their structs and the
 * model's plain ones. Everything is quick, or a job the page polls. */
#define _DEFAULT_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

#include "mgmt.h"
#include "rec_store.h"
#include "sys.h"
#include "sys_curl.h"
#include "sys_disk.h"
#include "sys_nas.h"
#include "ui_bridge.h"
#ifdef VIGIL_HAVE_USERS
#include "api.h"
#include "netip_srv.h"
#include "onvif.h"
#include "rtsp_server.h"
#include "users.h"
#include "web.h"
#endif

#define ETH "eth0"

/* Copy, cut to fit: the model's fields are smaller than the backends'. */
static void cp(char *d, size_t n, const char *s)
{
    size_t l = strnlen(s, n - 1);
    memcpy(d, s, l);
    d[l] = 0;
}
#define CP(dst, src) cp((dst), sizeof(dst), (src))

static int managed(void) { return sys_managed(); }

static void status_copy(const char *ifname, ui_net_status_t *u)
{
    net_status_t st;
    memset(u, 0, sizeof *u);
    if (!ifname || !*ifname) return;
    net_status(ifname, &st);
    u->present = st.present;
    u->up = st.up;
    u->link = st.carrier;
    u->speed = st.speed;
    CP(u->duplex, st.duplex);
    CP(u->mac, st.mac);
    u->mtu = st.mtu;
    CP(u->address, st.address);
    CP(u->netmask, st.netmask);
    CP(u->gateway, st.gateway);
    CP(u->dns1, st.dns1);
    CP(u->dns2, st.dns2);
}

static void eth_get(ui_net_cfg_t *c, ui_net_status_t *st)
{
    net_cfg_t n;
    char err[128];
    net_read(ETH, &n, err, sizeof err);
    memset(c, 0, sizeof *c);
    c->dhcp = n.dhcp;
    CP(c->address, n.address);
    CP(c->netmask, n.netmask);
    CP(c->gateway, n.gateway);
    CP(c->dns1, n.dns1);
    CP(c->dns2, n.dns2);
    c->mtu = n.mtu;
    status_copy(ETH, st);
}

static int eth_apply(const ui_net_cfg_t *c, char *field, size_t fcap, char *err, size_t cap)
{
    net_cfg_t n = { .ifname = ETH, .dhcp = c->dhcp, .mtu = c->mtu };
    CP(n.address, c->address);
    CP(n.netmask, c->netmask);
    CP(n.gateway, c->gateway);
    CP(n.dns1, c->dns1);
    CP(n.dns2, c->dns2);
    return mgmt_net_apply(&n, 1, field, fcap, err, cap);
}

static int net_pending(int *secs) { return mgmt_net_pending(NULL, 0, secs); }
static int net_confirm(void) { return mgmt_net_confirm(); }
static int net_revert(void) { return mgmt_net_revert(); }

static void radios_copy(ui_radio_t *u, int *nu, const usb_radio_t *r, int n)
{
    *nu = n > 4 ? 4 : n;
    for (int i = 0; i < *nu; ++i) {
        CP(u[i].id, r[i].id);
        CP(u[i].name, r[i].name);
        CP(u[i].driver, r[i].driver);
        CP(u[i].profile, r[i].profile);
        u[i].bound = r[i].bound;
    }
}

static void wifi_get(ui_wifi_t *u, ui_net_status_t *st)
{
    wifi_info_t w;
    wifi_info(&w);
    memset(u, 0, sizeof *u);
    CP(u->ifname, w.ifname);
    CP(u->ssid, w.ssid);
    u->password_set = w.has_password;
    u->can_scan = w.can_scan && w.ifname[0];
    CP(u->profile, w.profile);
    radios_copy(u->adapters, &u->n_adapters, w.adapters, w.n_adapters);
    status_copy(w.ifname, st);
}

static int scan_now(ui_wifi_net_t *out, int max, char *err, size_t cap)
{
    wifi_info_t w;
    wifi_info(&w);
    if (!w.ifname[0]) {
        snprintf(err, cap, "no Wi-Fi adapter");
        return -1;
    }
    wifi_net_t nets[32];
    int n = wifi_scan(w.ifname, nets, max < 32 ? max : 32, err, cap);
    for (int i = 0; i < n; ++i) {
        CP(out[i].ssid, nets[i].ssid);
        out[i].signal = nets[i].signal;
        out[i].secured = nets[i].secured;
    }
    return n;
}

/* The Wi-Fi scan, off the UI thread: iw takes seconds, up to its 20 s
 * timeout, and the screen must go on meanwhile. */
#define SCAN_NETS 16
static pthread_mutex_t g_scan_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int           running, n;
    ui_wifi_net_t nets[SCAN_NETS];
    char          err[160];
} g_scan;                           /* under g_scan_lock */

static void *scan_main(void *arg)
{
    (void)arg;
    ui_wifi_net_t nets[SCAN_NETS];
    char err[160] = "";
    int n = scan_now(nets, SCAN_NETS, err, sizeof err);
    pthread_mutex_lock(&g_scan_lock);
    g_scan.n = n;
    if (n > 0) memcpy(g_scan.nets, nets, (size_t)n * sizeof nets[0]);
    snprintf(g_scan.err, sizeof g_scan.err, "%s", err);
    g_scan.running = 0;
    pthread_mutex_unlock(&g_scan_lock);
    return NULL;
}

static int wifi_scan_start(char *err, size_t cap)
{
    if (!sys_have("iw")) {
        snprintf(err, cap, "this firmware has no iw to scan with");
        return UI_MGMT_EFAIL;
    }
    pthread_mutex_lock(&g_scan_lock);
    int busy = g_scan.running;
    g_scan.running = 1;
    pthread_mutex_unlock(&g_scan_lock);
    if (busy) return 0;
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&th, &at, scan_main, NULL);
    pthread_attr_destroy(&at);
    if (rc != 0) {
        pthread_mutex_lock(&g_scan_lock);
        g_scan.running = 0;
        pthread_mutex_unlock(&g_scan_lock);
        snprintf(err, cap, "cannot start the scan");
        return UI_MGMT_EFAIL;
    }
    return 0;
}

static int wifi_scan_(ui_wifi_net_t *out, int max, char *err, size_t cap)
{
    pthread_mutex_lock(&g_scan_lock);
    int n = g_scan.running ? UI_MGMT_EBUSY : g_scan.n;
    if (n > max) n = max;
    if (n > 0) memcpy(out, g_scan.nets, (size_t)n * sizeof out[0]);
    if (n < 0) snprintf(err, cap, "%s", g_scan.err);
    pthread_mutex_unlock(&g_scan_lock);
    return n;
}

static int wifi_set_(const char *ssid, const char *pass, const char *profile, char *err, size_t cap)
{
    wifi_info_t w;
    wifi_info(&w);
    if (!w.ifname[0] && !w.n_adapters) {
        snprintf(err, cap, "no Wi-Fi adapter");
        return UI_MGMT_EINVAL;
    }
    if ((!profile || !*profile) && !w.profile[0] && w.n_adapters) profile = w.adapters[0].profile;
    return mgmt_wifi_set(w.ifname, ssid, pass, profile, err, cap);
}

static void lte_get(ui_lte_t *u, ui_net_status_t *st)
{
    lte_info_t l;
    lte_info(&l);
    memset(u, 0, sizeof *u);
    CP(u->ifname, l.ifname);
    CP(u->driver, l.driver);
    CP(u->profile, l.profile);
    radios_copy(u->modems, &u->n_modems, l.modems, l.n_modems);
    status_copy(l.ifname, st);
}

static void svc(ui_svc_t *u, const mgmt_svc_status_t *s)
{
    u->last = (long long)s->last;
    u->ok = s->ok;
    CP(u->result, s->result);
}

static void services(ui_svc_t *ddns, ui_svc_t *smtp, ui_svc_t *alarm, ui_svc_t *nas)
{
    mgmt_services_t s;
    mgmt_services(&s);
    svc(ddns, &s.ddns);
    svc(smtp, &s.smtp);
    svc(alarm, &s.alarm);
    svc(nas, &s.nas);
}

static int nas_mounted(void) { return sys_nas_mounted(MGMT_NAS_MOUNT); }

static int job(int id, ui_mjob_t *u)
{
    mgmt_job_t j;
    if (mgmt_job(id, &j) < 0) return -1;
    u->id = j.id;
    u->state = (int)j.state;
    CP(u->kind, j.kind);
    CP(u->msg, j.msg);
    return 0;
}

static int disks(ui_disk_t *out, int max)
{
    sys_disk_t *d = calloc(SYS_DISK_MAX, sizeof *d);
    if (!d) return 0;
    int n = mgmt_disks(d, SYS_DISK_MAX);
    if (n > max) n = max;
    for (int i = 0; i < n; ++i) {
        ui_disk_t *u = &out[i];
        memset(u, 0, sizeof *u);
        CP(u->name, d[i].name);
        CP(u->parent, d[i].parent);
        u->is_part = d[i].is_part;
        u->size = d[i].size;
        CP(u->model, d[i].model);
        u->removable = d[i].removable;
        u->ro = d[i].ro;
        CP(u->fs, d[i].fs);
        CP(u->mnt, d[i].mnt);
        u->total = d[i].total;
        u->avail = d[i].avail;
        u->system = d[i].system;
        CP(u->state, d[i].state);
    }
    free(d);
    return n;
}

/* ---- disk maintenance ------------------------------------------------------------- */

_Static_assert((int)UI_SMART_GOOD == (int)SMART_GOOD && (int)UI_SMART_WARN == (int)SMART_WARN &&
               (int)UI_SMART_FAIL == (int)SMART_FAIL && (int)UI_SMART_UNKNOWN == (int)SMART_UNKNOWN,
               "SMART verdicts");
_Static_assert((int)UI_SCAN_READ == (int)SCAN_READ && (int)UI_SCAN_VERIFY == (int)SCAN_VERIFY &&
               (int)UI_SCAN_WRITE == (int)SCAN_WRITE && (int)UI_SCAN_RUNNING == (int)SCAN_RUNNING &&
               (int)UI_SCAN_PAUSED == (int)SCAN_PAUSED && (int)UI_SCAN_FAILED == (int)SCAN_FAILED &&
               (int)UI_SCAN_CLASSES == (int)SCAN_NCLASS && UI_SCAN_MAP == SCAN_MAP,
               "scan modes, states, classes, map");

/* The UI's fonts have no degree sign: "37 °C" shows as "37 C". */
static void cp_text(char *d, size_t n, const char *s)
{
    size_t o = 0;
    for (; *s && o + 1 < n; ++s) {
        if ((unsigned char)s[0] == 0xc2 && (unsigned char)s[1] == 0xb0) {
            ++s;
            continue;
        }
        d[o++] = *s;
    }
    d[o] = 0;
}
#define CPT(dst, src) cp_text((dst), sizeof(dst), (src))

static int smart_read_(const char *disk, char *err, size_t cap) { return mgmt_smart_job(disk, err, cap); }

static int smart_get(const char *disk, ui_smart_t *u)
{
    smart_info_t *s = malloc(sizeof *s);
    time_t when = 0;
    if (!s || mgmt_smart_cached(disk, s, &when) < 0) {
        free(s);
        return -1;
    }
    memset(u, 0, sizeof *u);
    u->available = s->available;
    CP(u->transport, s->transport);
    CP(u->model, s->model);
    CP(u->serial, s->serial);
    CP(u->firmware, s->firmware);
    u->bytes = (unsigned long long)s->sectors * s->sector_size;
    u->rpm = s->rpm;
    u->sata_max = s->sata_max;
    u->sata_cur = s->sata_cur;
    u->smart_enabled = s->smart_enabled;
    u->health = s->health;
    u->temp = s->temp;
    u->hours = s->hours;
    u->verdict = s->verdict;
    CPT(u->reason, s->reason);
    u->test_supported = s->selftest_supported;
    u->test_running = s->test_status == 15;
    u->test_remaining = s->test_remaining;
    u->short_min = s->short_min;
    u->ext_min = s->ext_min;
    if (s->has_data)
        smart_test_status_text((uint8_t)(s->test_status << 4 | s->test_remaining / 10), u->test_status,
                               sizeof u->test_status);
    u->n_attrs = s->n_attrs < UI_SMART_ATTRS ? s->n_attrs : UI_SMART_ATTRS;
    for (int i = 0; i < u->n_attrs; ++i) {
        const smart_attr_t *a = &s->attrs[i];
        ui_smart_attr_t *x = &u->attrs[i];
        x->id = a->id;
        x->value = a->value;
        x->worst = a->worst;
        x->thresh = a->thresh;
        x->level = a->level;
        x->critical = a->critical;
        CP(x->name, a->name);
        CPT(x->raw, a->raw_text);
        CPT(x->note, a->note);
    }
    u->n_tests = s->n_tests < UI_SMART_TESTS ? s->n_tests : UI_SMART_TESTS;
    for (int i = 0; i < u->n_tests; ++i) {
        const smart_test_t *t = &s->tests[i];
        CP(u->tests[i].type, smart_test_type_text(t->type));
        smart_test_status_text(t->status, u->tests[i].status, sizeof u->tests[i].status);
        u->tests[i].hours = t->hours;
        u->tests[i].failed = (t->status >> 4) >= 3 && (t->status >> 4) <= 8;
        u->tests[i].lba = t->lba;
    }
    u->read_at = (long long)when;
    free(s);
    return 0;
}

static void scan_get(ui_scan_t *u)
{
    scan_status_t *s = malloc(sizeof *s);
    memset(u, 0, sizeof *u);
    if (!s) return;
    mgmt_scan_status(s);
    u->state = s->state;
    u->mode = s->mode;
    u->read_instead = s->read_instead;
    CP(u->disk, s->disk);
    CP(u->model, s->model);
    CP(u->serial, s->serial);
    u->start = s->start;
    u->end = s->end;
    u->lba = s->lba;
    u->blocks_done = s->blocks_done;
    u->blocks_total = s->blocks_total;
    for (int i = 0; i < UI_SCAN_CLASSES; ++i) u->counts[i] = s->counts[i];
    u->block = s->block;
    u->sector = s->sector;
    u->pass = s->pass;
    u->passes = s->passes;
    u->mbps = s->mbps;
    u->elapsed = s->elapsed;
    u->eta = s->eta;
    CP(u->msg, s->msg);
    u->n_bad = s->n_bad < UI_SCAN_BAD ? s->n_bad : UI_SCAN_BAD;
    u->bad_more = s->bad_more + (unsigned long long)(s->n_bad - u->n_bad);
    for (int i = 0; i < u->n_bad; ++i) {
        u->bad_lba[i] = s->bad[i].lba;
        u->bad_count[i] = s->bad[i].count;
    }
    u->cells = s->cells;
    memcpy(u->map, s->map, sizeof u->map);
    free(s);
}

static int scan_start_(const char *disk, int mode, unsigned long long start, unsigned long long end, unsigned block,
                       const char *confirm, char *err, size_t cap)
{
    return mgmt_scan_start(disk, mode, start, end, block, confirm, err, cap);
}

static void time_get(ui_time_t *u)
{
    sys_clock_t c;
    clock_read(&c);
    mgmt_ntp_status_t ns;
    mgmt_ntp_status(&ns);
    vigil_config_t *cfg = malloc(sizeof *cfg);
    if (cfg) config_get(cfg);
    memset(u, 0, sizeof *u);
    CP(u->zone, c.zone);
    CP(u->tz, c.tz);
    u->rtc = c.rtc;
    u->n_servers = c.n_servers;
    memcpy(u->servers, c.servers, sizeof u->servers);
    u->ntp = cfg ? cfg->time.ntp : 1;
    u->ntp_interval = cfg ? cfg->time.ntp_interval : 60;
    u->ntp_last_try = (long long)ns.last_try;
    u->ntp_last_ok = (long long)ns.last_ok;
    CP(u->ntp_result, ns.result);
    free(cfg);
}

static int zones(void)
{
    int n = 0;
    while (tz_zones[n].name) ++n;
    return n;
}

static const char *zone_name(int i) { return i >= 0 && i < zones() ? tz_zones[i].name : NULL; }

static const char *zone_tz(int i) { return i >= 0 && i < zones() ? tz_zones[i].posix : NULL; }

static int tz_parse_(const char *s, ui_tz_rule_t *u)
{
    tz_rule_t r;
    if (tz_parse(s, &r) < 0) return -1;
    *u = (ui_tz_rule_t){ r.std_offset, r.dst, r.dst_offset, r.start_month, r.start_week, r.start_wday,
                         r.start_min, r.end_month, r.end_week, r.end_wday, r.end_min };
    return 0;
}

static int tz_build_(const ui_tz_rule_t *u, char *out, size_t cap, char *err, size_t ecap)
{
    tz_rule_t r = { u->std_offset, u->dst, u->dst_offset, u->start_month, u->start_week, u->start_wday,
                    u->start_min, u->end_month, u->end_week, u->end_wday, u->end_min };
    return tz_build(&r, out, cap, err, ecap);
}

static int time_set(const char *zone, const char *tz, int set_clock, long long t, char *err, size_t cap)
{
    return mgmt_time_set(zone, tz, set_clock, (time_t)t, err, cap);
}

/* "a.org, b.org c.org" -> up to four names. */
static int ntp_set(int enabled, const char *list, int interval, char *err, size_t cap)
{
    char servers[4][64];
    int n = 0;
    for (const char *p = list; *p && n < 4;) {
        while (*p == ' ' || *p == ',') ++p;
        size_t l = strcspn(p, ", ");
        if (l) snprintf(servers[n++], sizeof servers[0], "%.*s", (int)(l < 63 ? l : 63), p);
        p += l;
    }
    if (!n && enabled) {
        snprintf(err, cap, "give at least one NTP server");
        return UI_MGMT_EINVAL;
    }
    return mgmt_ntp_set(enabled, servers, n, interval, err, cap);
}

static int ntp_sync_now(void) { return mgmt_ntp_sync(); }
static void hostname_get(char *b, size_t cap) { host_get(b, cap); }

#ifdef VIGIL_HAVE_USERS
static int urc(users_rc_t rc, const users_err_t *e, char *field, size_t fcap, char *err, size_t cap)
{
    if (field) snprintf(field, fcap, "%s", e->field);
    snprintf(err, cap, "%s", e->msg);
    if (rc == USERS_OK) return 0;
    if (!err[0]) snprintf(err, cap, rc == USERS_ENOENT ? "no such name" : rc == USERS_EEXIST ? "the name is taken"
                                    : rc == USERS_EPROTECTED ? "a built-in group cannot be changed so"
                                    : rc == USERS_EINUSE ? "the group still has users" : "cannot save");
    return rc == USERS_EIO ? UI_MGMT_EFAIL : UI_MGMT_EINVAL;
}

static int users_(ui_user_t *out, int max)
{
    users_user_t u[USERS_MAX];
    int n = users_list(u, USERS_MAX);
    if (n > max) n = max;
    for (int i = 0; i < n; ++i) {
        CP(out[i].name, u[i].name);
        CP(out[i].group, u[i].group);
        out[i].enabled = u[i].enabled;
        out[i].password_set = u[i].password_set;
    }
    return n;
}

static int groups_(ui_group_t *out, int max)
{
    users_group_t g[GROUPS_MAX];
    int n = users_groups(g, GROUPS_MAX);
    if (n > max) n = max;
    for (int i = 0; i < n; ++i) {
        CP(out[i].name, g[i].name);
        out[i].perms = g[i].perms;
        out[i].builtin = g[i].builtin;
        out[i].editable = g[i].editable;
        out[i].members = g[i].members;
    }
    return n;
}

static unsigned users_gen(void)
{
    users_reload();
    return users_generation();
}

static int user_add(const char *name, const char *group, const char *pass, int enabled, char *field,
                    size_t fcap, char *err, size_t cap)
{
    users_err_t e = { 0 };
    return urc(users_add(name, group, pass, enabled, &e), &e, field, fcap, err, cap);
}

static int user_update(const char *name, const char *group, int enabled, const char *pass, char *field,
                       size_t fcap, char *err, size_t cap)
{
    users_err_t e = { 0 };
    return urc(users_update(name, group, enabled, pass, &e), &e, field, fcap, err, cap);
}

static int user_delete(const char *name, char *err, size_t cap)
{
    users_err_t e = { 0 };
    return urc(users_delete(name, &e), &e, NULL, 0, err, cap);
}

static int group_set(const char *name, unsigned perms, int create, char *field, size_t fcap, char *err,
                     size_t cap)
{
    users_err_t e = { 0 };
    return urc(users_group_set(name, perms, create, &e), &e, field, fcap, err, cap);
}

static int group_delete(const char *name, char *err, size_t cap)
{
    users_err_t e = { 0 };
    return urc(users_group_delete(name, &e), &e, NULL, 0, err, cap);
}

_Static_assert((unsigned)UI_PERM_LIVE == (unsigned)PERM_LIVE && (unsigned)UI_PERM_CAMERAS == (unsigned)PERM_CAMERAS &&
                   (unsigned)UI_PERM_STORAGE == (unsigned)PERM_STORAGE &&
                   (unsigned)UI_PERM_NETWORK == (unsigned)PERM_NETWORK &&
                   (unsigned)UI_PERM_SYSTEM == (unsigned)PERM_SYSTEM && (unsigned)UI_PERM_USERS == (unsigned)PERM_USERS &&
                   (unsigned)UI_PERM_REMOTE == (unsigned)PERM_REMOTE,
               "UI_PERM_* are users.h's PERM_*");

static void system_user(char *buf, size_t cap);

static int system_password(const char *current, const char *pass, char *field, size_t fcap, char *err,
                           size_t cap)
{
    char user[64], why[200] = "";
    unsigned perms;
    system_user(user, sizeof user);
    users_reload();
    /* The screen may be anyone's while ui.login is off: the password
     * opens the board's console and SSH too, so it takes the current one. */
    int rc = users_login(user, current, &perms, why, sizeof why);
    if (rc != 1) {
        snprintf(field, fcap, "current");
        snprintf(err, cap, "%s", rc < 0 ? why : "not the current password");
        return -1;
    }
    users_err_t e = { 0 };
    return urc(users_set_system_password(pass, &e), &e, field, fcap, err, cap);
}

static int login_(const char *name, const char *pass, unsigned *perms, char *why, size_t cap)
{
    users_reload();
    return users_login(name, pass, perms, why, cap);
}
#endif

static void system_user(char *buf, size_t cap)
{
    vigil_config_t *c = malloc(sizeof *c);
    if (!c) {
        snprintf(buf, cap, "root");
        return;
    }
    config_get(c);
    snprintf(buf, cap, "%s", c->api.user[0] ? c->api.user : "root");
    free(c);
}

static int power(int reboot) { return mgmt_power_request(reboot ? MGMT_POWER_REBOOT : MGMT_POWER_OFF); }

#ifdef VIGIL_HAVE_USERS
static void listening(int server, ui_listen_t *out)
{
    memset(out, 0, sizeof *out);
    int (*fn)(char *, size_t, char *, size_t) = server == UI_SERVER_API     ? api_listening
                                                : server == UI_SERVER_ONVIF ? onvif_listening
                                                : server == UI_SERVER_RTSP  ? rtsp_server_listening
                                                : server == UI_SERVER_NETIP ? netip_server_listening
                                                                            : web_listening;
    out->on = fn(out->listen, sizeof out->listen, out->why, sizeof out->why);
}
#endif

static void human(char *out, size_t cap, uint64_t bytes)
{
    if (bytes >= (1ull << 40)) snprintf(out, cap, "%.1f TiB", bytes / 1099511627776.0);
    else if (bytes >= (1ull << 30)) snprintf(out, cap, "%.1f GiB", bytes / 1073741824.0);
    else snprintf(out, cap, "%llu MiB", (unsigned long long)(bytes >> 20));
}

static int rec_pool(char *out, size_t cap)
{
    rec_store_status_t st;
    rec_store_status(&st);
    out[0] = 0;
    if (st.state == RP_OFF) return 0;
    size_t n = 0;
#define ADDF(...) do { if (n < cap) n += (size_t)snprintf(out + n, cap - n, __VA_ARGS__); } while (0)
    if (st.state == RP_WAITING || st.state == RP_ERROR) {
        ADDF("Pool: %s", st.state == RP_WAITING && st.place[0] ? st.place : "error");
        if (st.why[0]) ADDF(" (%.100s)", st.why);
        return 1;
    }
    char size[24], used[24];
    human(size, sizeof size, st.size_bytes);
    human(used, sizeof used, st.used_bytes);
    ADDF("Pool: %s in %u chunks of %u MiB, %s used; %u free, %u recording", size, st.chunks, st.chunk_mb, used,
         st.free, st.open);
    if (st.chunks < st.want) ADDF("; %u more being made", st.want - st.chunks);
    ADDF(". On %s, flushed every %u s.", st.media, st.flush_s);
    int lines = 1;
    if (st.state == RP_READONLY) {
        ADDF("\nAnother recorder's pool, %s: playback only.", st.why);
        ++lines;
    }
    if (st.rebuilding) {
        ADDF("\nRebuilding the index: %u of %u chunks read.", st.rebuild_done, st.chunks);
        ++lines;
    }
    int first = 1;
    for (int i = 0; i < REC_CAMS; ++i) {
        if (!st.cam[i].chunks) continue;
        time_t t = (time_t)(st.cam[i].oldest_us / 1000000);
        struct tm tm;
        localtime_r(&t, &tm);
        ADDF("%sCAM%d %04d-%02d-%02d %02d:%02d", first ? "\nOldest video: " : ", ", i + 1, tm.tm_year + 1900,
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
        first = 0;
    }
    if (!first) ++lines;
#undef ADDF
    return lines;
}

static const ui_mgmt_t g_mgmt = {
    .managed = managed,
    .eth_get = eth_get, .eth_apply = eth_apply, .net_pending = net_pending, .net_confirm = net_confirm,
    .net_revert = net_revert, .wifi_get = wifi_get, .wifi_scan_start = wifi_scan_start, .wifi_scan = wifi_scan_, .wifi_set = wifi_set_,
    .lte_get = lte_get, .lte_profile = mgmt_lte_profile,
    .services = services, .ddns_update = mgmt_ddns_update, .smtp_test = mgmt_smtp_test,
    .nas_test = mgmt_nas_test, .nas_support = sys_nas_support, .nas_mounted = nas_mounted,
    .curl = curl_available, .job = job,
    .rec_pool = rec_pool,
    .disks = disks, .disk_formats = sys_disk_formats, .disk_format = mgmt_disk_format,
    .disk_mount = mgmt_disk_mount, .disk_unmount = mgmt_disk_unmount,
    .smart_read = smart_read_, .smart_get = smart_get, .selftest = mgmt_selftest, .scan_start = scan_start_,
    .scan_get = scan_get, .scan_pause = mgmt_scan_pause, .scan_stop = mgmt_scan_stop,
    .time_get = time_get, .zones = zones, .zone_name = zone_name, .zone_tz = zone_tz, .tz_parse = tz_parse_,
    .tz_build = tz_build_, .time_set = time_set, .ntp_set = ntp_set, .ntp_sync = ntp_sync_now,
    .hostname_get = hostname_get, .hostname_set = mgmt_hostname_set,
#ifdef VIGIL_HAVE_USERS
    .users_gen = users_gen, .users = users_, .groups = groups_, .perm_names = users_perm_names,
    .n_perms = PERM_N, .user_add = user_add, .user_update = user_update, .user_delete = user_delete,
    .group_set = group_set, .group_delete = group_delete, .login = login_, .listening = listening,
    .system_password = system_password,
#endif
    .system_user = system_user, .power = power,
};

const ui_mgmt_t *ui_mgmt_model(void) { return &g_mgmt; }
