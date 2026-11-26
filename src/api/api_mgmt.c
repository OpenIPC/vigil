/* SPDX-License-Identifier: GPL-3.0-or-later */
/* api_mgmt.c — the management pages over HTTP: network, time and name,
 * disks, network storage, the services' state and jobs (docs/api.md,
 * "Management"). Everything goes through mgmt.h, as the on-screen pages
 * do; slow changes answer 202 with a job to poll. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "civetweb.h"
#include "cJSON.h"

#include "api_int.h"
#include "config.h"
#include "mgmt.h"
#include "sys.h"
#include "sys_curl.h"
#include "sys_nas.h"

#define ETH "eth0"

/* ---- helpers ------------------------------------------------------------------------ */

static int is(const char *m, const char *want) { return !strcmp(m, want); }
static int get(const char *m) { return is(m, "GET") || is(m, "HEAD"); }

/* The body as a JSON object; NULL after answering 400/413. */
static cJSON *body_json(struct mg_connection *conn)
{
    size_t len = 0;
    int status = 0;
    char *b = read_body(conn, &len, &status);
    if (!b) {
        send_error(conn, status, status == 413 ? "body too large" : "out of memory", NULL);
        return NULL;
    }
    cJSON *doc = len ? cJSON_ParseWithLength(b, len) : cJSON_CreateObject();
    memset(b, 0, len);                      /* it may hold a password */
    free(b);
    if (!cJSON_IsObject(doc)) {
        cJSON_Delete(doc);
        send_error(conn, 400, "the body must be a JSON object", NULL);
        return NULL;
    }
    return doc;
}

static const char *jstr(const cJSON *o, const char *k, const char *dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : dflt;
}

static int jint(const cJSON *o, const char *k, int dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : cJSON_IsBool(v) ? cJSON_IsTrue(v) : dflt;
}

static int has(const cJSON *o, const char *k) { return cJSON_GetObjectItemCaseSensitive(o, k) != NULL; }

static int bad_request(struct mg_connection *conn, const char *path, const char *msg)
{
    schema_errors_t e = { 0 };
    schema_error(&e, path, "%s", msg);
    return send_error(conn, 400, "invalid request", &e);
}

/* A mgmt_* answer: a job id (202), or its error. */
static int answer(struct mg_connection *conn, int rc, const char *field, const char *err)
{
    if (rc > 0) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "job", rc);
        return send_json(conn, 202, d);
    }
    switch (rc) {
    case MGMT_OK: {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddBoolToObject(d, "ok", 1);
        return send_json(conn, 200, d);
    }
    case MGMT_EINVAL: return bad_request(conn, field ? field : "", err && *err ? err : "invalid");
    case MGMT_EUNMANAGED:
        return send_error(conn, 409, "vigild does not manage this system (it is not OpenIPC firmware)", NULL);
    case MGMT_EBUSY: return send_error(conn, 409, err && *err ? err : "busy", NULL);
    default: return send_error(conn, 500, err && *err ? err : "failed", NULL);
    }
}

static void add_time(cJSON *o, const char *k, time_t t)
{
    if (t) cJSON_AddNumberToObject(o, k, (double)t);
    else cJSON_AddNullToObject(o, k);
}

static cJSON *svc_json(const mgmt_svc_status_t *s, int enabled)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", enabled);
    add_time(o, "last", s->last);
    cJSON_AddBoolToObject(o, "ok", s->ok);
    cJSON_AddStringToObject(o, "result", s->result);
    return o;
}

/* ---- network ------------------------------------------------------------------------- */

static cJSON *net_status_json(const char *ifname)
{
    net_status_t st;
    net_status(ifname, &st);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "interface", ifname);
    cJSON_AddBoolToObject(o, "present", st.present);
    cJSON_AddBoolToObject(o, "up", st.up);
    cJSON_AddBoolToObject(o, "link", st.carrier);
    cJSON_AddNumberToObject(o, "speed", st.speed);
    cJSON_AddStringToObject(o, "duplex", st.duplex);
    cJSON_AddStringToObject(o, "mac", st.mac);
    cJSON_AddNumberToObject(o, "mtu", st.mtu);
    cJSON_AddStringToObject(o, "address", st.address);
    cJSON_AddStringToObject(o, "netmask", st.netmask);
    cJSON_AddStringToObject(o, "gateway", st.gateway);
    cJSON *dns = cJSON_AddArrayToObject(o, "dns");
    if (st.dns1[0]) cJSON_AddItemToArray(dns, cJSON_CreateString(st.dns1));
    if (st.dns2[0]) cJSON_AddItemToArray(dns, cJSON_CreateString(st.dns2));
    return o;
}

static cJSON *net_cfg_json(const net_cfg_t *c)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "dhcp", c->dhcp);
    cJSON_AddStringToObject(o, "address", c->address);
    cJSON_AddStringToObject(o, "netmask", c->netmask);
    cJSON_AddStringToObject(o, "gateway", c->gateway);
    cJSON_AddStringToObject(o, "dns1", c->dns1);
    cJSON_AddStringToObject(o, "dns2", c->dns2);
    cJSON_AddNumberToObject(o, "mtu", c->mtu);
    return o;
}

static int ethernet(struct mg_connection *conn, const char *m)
{
    if (get(m)) {
        net_cfg_t c;
        char err[128];
        net_read(ETH, &c, err, sizeof err);
        cJSON *d = cJSON_CreateObject();
        cJSON_AddBoolToObject(d, "managed", sys_managed());
        cJSON_AddItemToObject(d, "config", net_cfg_json(&c));
        cJSON_AddItemToObject(d, "status", net_status_json(ETH));
        char ifn[16];
        int left;
        if (mgmt_net_pending(ifn, sizeof ifn, &left)) {
            cJSON *p = cJSON_AddObjectToObject(d, "pending");
            cJSON_AddStringToObject(p, "interface", ifn);
            cJSON_AddNumberToObject(p, "seconds", left);
        } else {
            cJSON_AddNullToObject(d, "pending");
        }
        return send_json(conn, 200, d);
    }
    if (!is(m, "PUT")) return method_not_allowed(conn, "GET, HEAD, PUT");
    cJSON *b = body_json(conn);
    if (!b) return 400;
    net_cfg_t c;
    char err[256], field[32];
    net_read(ETH, &c, err, sizeof err);
    /* What the body leaves out stays as it is. */
    c.dhcp = jint(b, "dhcp", c.dhcp);
    snprintf(c.address, sizeof c.address, "%s", jstr(b, "address", c.address));
    snprintf(c.netmask, sizeof c.netmask, "%s", jstr(b, "netmask", c.netmask));
    snprintf(c.gateway, sizeof c.gateway, "%s", jstr(b, "gateway", c.gateway));
    snprintf(c.dns1, sizeof c.dns1, "%s", jstr(b, "dns1", c.dns1));
    snprintf(c.dns2, sizeof c.dns2, "%s", jstr(b, "dns2", c.dns2));
    c.mtu = jint(b, "mtu", c.mtu);
    /* Over the API the change is undone unless confirmed: the client may
     * be cut off by it. "confirm": false only for a client that knows. */
    int confirm = jint(b, "confirm", 1);
    cJSON_Delete(b);
    int rc = mgmt_net_apply(&c, confirm, field, sizeof field, err, sizeof err);
    if (rc > 0) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "job", rc);
        if (confirm) cJSON_AddNumberToObject(d, "confirm_within", MGMT_CONFIRM_SECS);
        return send_json(conn, 202, d);
    }
    char path[48];
    snprintf(path, sizeof path, "%s", field);
    return answer(conn, rc, path, err);
}

static int wifi(struct mg_connection *conn, const char *m)
{
    wifi_info_t w;
    wifi_info(&w);
    if (get(m)) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddBoolToObject(d, "present", w.ifname[0] != 0);
        if (w.ifname[0]) {
            cJSON_AddStringToObject(d, "interface", w.ifname);
            cJSON_AddStringToObject(d, "ssid", w.ssid);
            cJSON_AddBoolToObject(d, "password_set", w.has_password);
            cJSON_AddBoolToObject(d, "can_scan", w.can_scan);
            cJSON_AddItemToObject(d, "status", net_status_json(w.ifname));
        }
        return send_json(conn, 200, d);
    }
    if (!is(m, "PUT")) return method_not_allowed(conn, "GET, HEAD, PUT");
    if (!w.ifname[0]) return send_error(conn, 409, "no Wi-Fi adapter", NULL);
    cJSON *b = body_json(conn);
    if (!b) return 400;
    char err[256] = "";
    const char *ssid = jstr(b, "ssid", NULL), *pass = jstr(b, "password", "");
    int rc = ssid ? mgmt_wifi_set(w.ifname, ssid, pass, err, sizeof err) : MGMT_EINVAL;
    if (!ssid) snprintf(err, sizeof err, "needs an ssid");
    cJSON_Delete(b);
    return answer(conn, rc, "ssid", err);
}

static int wifi_scan_req(struct mg_connection *conn, const char *m)
{
    if (!get(m)) return method_not_allowed(conn, "GET, HEAD");
    wifi_info_t w;
    wifi_info(&w);
    if (!w.ifname[0]) return send_error(conn, 409, "no Wi-Fi adapter", NULL);
    wifi_net_t nets[32];
    char err[256];
    int n = wifi_scan(w.ifname, nets, 32, err, sizeof err);
    if (n < 0) return send_error(conn, 501, err, NULL);
    cJSON *d = cJSON_CreateObject(), *a = cJSON_AddArrayToObject(d, "networks");
    for (int i = 0; i < n; ++i) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", nets[i].ssid);
        cJSON_AddNumberToObject(o, "signal", nets[i].signal);
        cJSON_AddBoolToObject(o, "secured", nets[i].secured);
        cJSON_AddItemToArray(a, o);
    }
    return send_json(conn, 200, d);
}

static int lte(struct mg_connection *conn, const char *m)
{
    if (!get(m)) return method_not_allowed(conn, "GET, HEAD");
    lte_info_t l;
    lte_info(&l);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddBoolToObject(d, "present", l.ifname[0] != 0);
    cJSON_AddStringToObject(d, "profile", l.profile);
    if (l.ifname[0]) {
        cJSON_AddStringToObject(d, "interface", l.ifname);
        cJSON_AddStringToObject(d, "driver", l.driver);
        cJSON_AddItemToObject(d, "status", net_status_json(l.ifname));
    }
    return send_json(conn, 200, d);
}

static int services(struct mg_connection *conn, const char *m)
{
    if (!get(m)) return method_not_allowed(conn, "GET, HEAD");
    vigil_config_t *c = malloc(sizeof *c);
    if (!c) return send_error(conn, 500, "out of memory", NULL);
    config_get(c);
    mgmt_services_t s;
    mgmt_services(&s);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddBoolToObject(d, "curl", curl_available());
    cJSON_AddItemToObject(d, "ddns", svc_json(&s.ddns, c->ddns.enabled));
    cJSON_AddItemToObject(d, "smtp", svc_json(&s.smtp, c->smtp.enabled));
    cJSON_AddItemToObject(d, "alarm", svc_json(&s.alarm, c->alarm.enabled));
    cJSON *u = cJSON_AddObjectToObject(d, "upnp");
    cJSON_AddBoolToObject(u, "enabled", c->upnp.enabled);
    cJSON_AddBoolToObject(u, "active", 0);
    cJSON_AddStringToObject(u, "note", "vigild has no UPnP client yet; the settings are kept");
    cJSON *p = cJSON_AddObjectToObject(d, "p2p");
    cJSON_AddBoolToObject(p, "available", 0);
    cJSON_AddStringToObject(p, "note", "vigil has no P2P service");
    free(c);
    return send_json(conn, 200, d);
}

/* ---- time and name ---------------------------------------------------------------------- */

static cJSON *rule_json(int month, int week, int wday, int min)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "month", month);
    cJSON_AddNumberToObject(o, "week", week);
    cJSON_AddNumberToObject(o, "wday", wday);
    cJSON_AddNumberToObject(o, "minute", min);
    return o;
}

static cJSON *time_json(void)
{
    sys_clock_t c;
    clock_read(&c);
    mgmt_ntp_status_t ns;
    mgmt_ntp_status(&ns);
    vigil_config_t *cfg = malloc(sizeof *cfg);
    if (cfg) config_get(cfg);
    time_t now = time(NULL);
    struct tm tm;
    char local[40];
    localtime_r(&now, &tm);
    strftime(local, sizeof local, "%Y-%m-%d %H:%M:%S", &tm);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddBoolToObject(d, "managed", sys_managed());
    cJSON_AddNumberToObject(d, "time", (double)now);
    cJSON_AddStringToObject(d, "local", local);
    cJSON_AddStringToObject(d, "zone", c.zone);
    cJSON_AddStringToObject(d, "tz", c.tz);
    tz_rule_t r;
    if (c.tz[0] && tz_parse(c.tz, &r) == 0) {
        cJSON *o = cJSON_AddObjectToObject(d, "rule");
        cJSON_AddNumberToObject(o, "std_offset", r.std_offset);
        cJSON_AddBoolToObject(o, "dst", r.dst);
        if (r.dst) {
            cJSON_AddNumberToObject(o, "dst_offset", r.dst_offset);
            cJSON_AddItemToObject(o, "start", rule_json(r.start_month, r.start_week, r.start_wday, r.start_min));
            cJSON_AddItemToObject(o, "end", rule_json(r.end_month, r.end_week, r.end_wday, r.end_min));
        }
    } else {
        cJSON_AddNullToObject(d, "rule");
    }
    cJSON_AddBoolToObject(d, "rtc", c.rtc);
    cJSON *n = cJSON_AddObjectToObject(d, "ntp");
    cJSON_AddBoolToObject(n, "enabled", cfg ? cfg->time.ntp : 1);
    cJSON_AddNumberToObject(n, "interval", cfg ? cfg->time.ntp_interval : 60);
    cJSON *sv = cJSON_AddArrayToObject(n, "servers");
    for (int i = 0; i < c.n_servers; ++i) cJSON_AddItemToArray(sv, cJSON_CreateString(c.servers[i]));
    add_time(n, "last_try", ns.last_try);
    add_time(n, "last_ok", ns.last_ok);
    cJSON_AddStringToObject(n, "result", ns.result);
    free(cfg);
    return d;
}

static void rule_from(const cJSON *o, int *month, int *week, int *wday, int *min)
{
    *month = jint(o, "month", 0);
    *week = jint(o, "week", 0);
    *wday = jint(o, "wday", -1);
    *min = jint(o, "minute", 120);
}

static int system_time(struct mg_connection *conn, const char *m)
{
    if (get(m)) return send_json(conn, 200, time_json());
    if (!is(m, "PUT")) return method_not_allowed(conn, "GET, HEAD, PUT");
    cJSON *b = body_json(conn);
    if (!b) return 400;
    char err[256] = "", tz[96] = "";
    int rc = MGMT_OK;
    const char *zone = jstr(b, "zone", NULL);
    const cJSON *rule = cJSON_GetObjectItemCaseSensitive(b, "rule");
    if (cJSON_IsObject(rule)) {
        /* A zone of one's own, from its fields: offset, DST and its rules. */
        tz_rule_t r = { .std_offset = jint(rule, "std_offset", 0), .dst = jint(rule, "dst", 0),
                        .dst_offset = jint(rule, "dst_offset", 0) };
        if (r.dst) {
            rule_from(cJSON_GetObjectItemCaseSensitive(rule, "start"), &r.start_month, &r.start_week,
                      &r.start_wday, &r.start_min);
            rule_from(cJSON_GetObjectItemCaseSensitive(rule, "end"), &r.end_month, &r.end_week, &r.end_wday,
                      &r.end_min);
        }
        if (tz_build(&r, tz, sizeof tz, err, sizeof err) < 0) {
            cJSON_Delete(b);
            return bad_request(conn, "rule", err);
        }
        zone = "";
    } else if (jstr(b, "tz", NULL)) {
        snprintf(tz, sizeof tz, "%s", jstr(b, "tz", ""));
        zone = "";
    }
    int set_clock = has(b, "time");
    time_t t = set_clock ? (time_t)cJSON_GetObjectItemCaseSensitive(b, "time")->valuedouble : 0;
    if (zone || tz[0] || set_clock) rc = mgmt_time_set(zone, tz, set_clock, t, err, sizeof err);
    const cJSON *ntp = cJSON_GetObjectItemCaseSensitive(b, "ntp");
    if (rc == MGMT_OK && cJSON_IsObject(ntp)) {
        char servers[4][64];
        int n = 0;
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(ntp, "servers");
        if (cJSON_IsArray(a)) {
            for (const cJSON *s = a->child; s && n < 4; s = s->next)
                if (cJSON_IsString(s) && s->valuestring[0]) snprintf(servers[n++], 64, "%.63s", s->valuestring);
        } else {
            sys_clock_t c;
            clock_read(&c);
            n = c.n_servers;
            memcpy(servers, c.servers, sizeof servers);
        }
        vigil_config_t *cfg = malloc(sizeof *cfg);
        if (cfg) config_get(cfg);
        rc = mgmt_ntp_set(jint(ntp, "enabled", cfg ? cfg->time.ntp : 1), servers, n,
                          jint(ntp, "interval", cfg ? cfg->time.ntp_interval : 60), err, sizeof err);
        free(cfg);
        if (rc == MGMT_EINVAL) {
            cJSON_Delete(b);
            return bad_request(conn, "ntp", err);
        }
    }
    cJSON_Delete(b);
    if (rc != MGMT_OK) return answer(conn, rc, has(b, "time") ? "time" : "zone", err);
    return send_json(conn, 200, time_json());
}

static int zones(struct mg_connection *conn, const char *m)
{
    if (!get(m)) return method_not_allowed(conn, "GET, HEAD");
    cJSON *a = cJSON_CreateArray();
    for (const tz_zone_t *z = tz_zones; z->name; ++z) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", z->name);
        cJSON_AddStringToObject(o, "tz", z->posix);
        cJSON_AddItemToArray(a, o);
    }
    return send_json(conn, 200, a);
}

static int hostname(struct mg_connection *conn, const char *m)
{
    if (!get(m) && !is(m, "PUT")) return method_not_allowed(conn, "GET, HEAD, PUT");
    if (is(m, "PUT")) {
        cJSON *b = body_json(conn);
        if (!b) return 400;
        char err[160] = "";
        const char *h = jstr(b, "hostname", "");
        int rc = mgmt_hostname_set(h, err, sizeof err);
        cJSON_Delete(b);
        if (rc != MGMT_OK) return answer(conn, rc, "hostname", err);
    }
    char h[64];
    host_get(h, sizeof h);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "hostname", h);
    return send_json(conn, 200, d);
}

/* ---- disks and network storage --------------------------------------------------------------- */

static int disks(struct mg_connection *conn, const char *m)
{
    if (!get(m)) return method_not_allowed(conn, "GET, HEAD");
    sys_disk_t *d = calloc(SYS_DISK_MAX, sizeof *d);
    if (!d) return send_error(conn, 500, "out of memory", NULL);
    int n = mgmt_disks(d, SYS_DISK_MAX);
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "managed", sys_managed());
    cJSON *a = cJSON_AddArrayToObject(doc, "disks");
    for (int i = 0; i < n; ++i) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", d[i].name);
        cJSON_AddStringToObject(o, "parent", d[i].parent);
        cJSON_AddBoolToObject(o, "partition", d[i].is_part);
        cJSON_AddNumberToObject(o, "size", (double)d[i].size);
        cJSON_AddStringToObject(o, "model", d[i].model);
        cJSON_AddBoolToObject(o, "removable", d[i].removable);
        cJSON_AddBoolToObject(o, "read_only", d[i].ro);
        cJSON_AddStringToObject(o, "fs", d[i].fs);
        cJSON_AddStringToObject(o, "mount", d[i].mnt);
        cJSON_AddNumberToObject(o, "total", (double)d[i].total);
        cJSON_AddNumberToObject(o, "free", (double)d[i].avail);
        cJSON_AddBoolToObject(o, "system", d[i].system);
        cJSON_AddStringToObject(o, "state", d[i].state);
        cJSON_AddItemToArray(a, o);
    }
    free(d);
    char fss[4][8];
    int nf = sys_disk_formats(fss, 4);
    cJSON *f = cJSON_AddArrayToObject(doc, "formats");
    for (int i = 0; i < nf; ++i) cJSON_AddItemToArray(f, cJSON_CreateString(fss[i]));
    return send_json(conn, 200, doc);
}

/* /api/v1/disks/<name>/<action> */
static int disk_action(struct mg_connection *conn, const char *m, const char *rest)
{
    char name[32], action[16];
    const char *slash = strchr(rest, '/');
    if (!slash || (size_t)(slash - rest) >= sizeof name || strlen(slash + 1) >= sizeof action)
        return send_error(conn, 404, "not found", NULL);
    snprintf(name, sizeof name, "%.*s", (int)(slash - rest), rest);
    snprintf(action, sizeof action, "%s", slash + 1);
    if (!is(m, "POST")) return method_not_allowed(conn, "POST");
    char err[256] = "";
    int rc;
    if (!strcmp(action, "format")) {
        cJSON *b = body_json(conn);
        if (!b) return 400;
        char fss[4][8];
        int nf = sys_disk_formats(fss, 4);
        const char *fs = jstr(b, "fs", nf ? fss[0] : "");
        rc = mgmt_disk_format(name, fs, jstr(b, "confirm", ""), err, sizeof err);
        cJSON_Delete(b);
        return answer(conn, rc, strstr(err, "type the") ? "confirm" : strstr(err, "cannot make") ? "fs" : "name",
                      err);
    }
    if (!strcmp(action, "mount")) rc = mgmt_disk_mount(name, err, sizeof err);
    else if (!strcmp(action, "unmount")) rc = mgmt_disk_unmount(name, err, sizeof err);
    else return send_error(conn, 404, "not found", NULL);
    return answer(conn, rc, "name", err);
}

static int nas(struct mg_connection *conn, const char *m, int test)
{
    if (test) {
        if (!is(m, "POST")) return method_not_allowed(conn, "POST");
        return answer(conn, mgmt_nas_test(), NULL, NULL);
    }
    if (!get(m)) return method_not_allowed(conn, "GET, HEAD");
    mgmt_services_t s;
    mgmt_services(&s);
    cJSON *d = cJSON_CreateObject(), *sup = cJSON_AddObjectToObject(d, "support");
    static const char *const types[] = { "cifs", "nfs" };
    for (int i = 0; i < 2; ++i) {
        char why[256] = "";
        int ok = sys_nas_support(types[i], why, sizeof why);
        cJSON *o = cJSON_AddObjectToObject(sup, types[i]);
        cJSON_AddBoolToObject(o, "ok", ok == 1);
        cJSON_AddStringToObject(o, "why", ok == 1 ? "" : why);
    }
    cJSON_AddStringToObject(d, "mount", MGMT_NAS_MOUNT);
    cJSON_AddBoolToObject(d, "mounted", sys_nas_mounted(MGMT_NAS_MOUNT));
    cJSON_AddItemToObject(d, "status", svc_json(&s.nas, 1));
    return send_json(conn, 200, d);
}

static int job(struct mg_connection *conn, const char *m, const char *id)
{
    if (!get(m)) return method_not_allowed(conn, "GET, HEAD");
    mgmt_job_t j;
    char *end;
    long n = strtol(id, &end, 10);
    if (*end || n <= 0 || mgmt_job((int)n, &j) < 0) return send_error(conn, 404, "no such job", NULL);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "id", j.id);
    cJSON_AddStringToObject(d, "kind", j.kind);
    cJSON_AddStringToObject(d, "target", j.target);
    cJSON_AddStringToObject(d, "state", mgmt_job_state_name(j.state));
    cJSON_AddStringToObject(d, "message", j.msg);
    add_time(d, "started", j.started);
    add_time(d, "finished", j.finished);
    return send_json(conn, 200, d);
}

int api_mgmt_request(struct mg_connection *conn, const char *uri, const char *m)
{
    if (!strcmp(uri, "/api/v1/network/ethernet")) return ethernet(conn, m);
    if (!strcmp(uri, "/api/v1/network/ethernet/confirm") || !strcmp(uri, "/api/v1/network/ethernet/revert")) {
        if (!is(m, "POST")) return method_not_allowed(conn, "POST");
        int rc = strstr(uri, "confirm") ? mgmt_net_confirm() : mgmt_net_revert();
        if (rc < 0) return send_error(conn, 409, "no network change is waiting for a confirmation", NULL);
        return answer(conn, MGMT_OK, NULL, NULL);
    }
    if (!strcmp(uri, "/api/v1/network/wifi")) return wifi(conn, m);
    if (!strcmp(uri, "/api/v1/network/wifi/scan")) return wifi_scan_req(conn, m);
    if (!strcmp(uri, "/api/v1/network/lte")) return lte(conn, m);
    if (!strcmp(uri, "/api/v1/network/services")) return services(conn, m);
    if (!strcmp(uri, "/api/v1/network/ddns/update") || !strcmp(uri, "/api/v1/network/smtp/test")) {
        if (!is(m, "POST")) return method_not_allowed(conn, "POST");
        return answer(conn, strstr(uri, "ddns") ? mgmt_ddns_update() : mgmt_smtp_test(), NULL, "busy");
    }
    if (!strcmp(uri, "/api/v1/system/time")) return system_time(conn, m);
    if (!strcmp(uri, "/api/v1/system/time/sync")) {
        if (!is(m, "POST")) return method_not_allowed(conn, "POST");
        return answer(conn, mgmt_ntp_sync(), NULL, "busy");
    }
    if (!strcmp(uri, "/api/v1/system/zones")) return zones(conn, m);
    if (!strcmp(uri, "/api/v1/system/hostname")) return hostname(conn, m);
    if (!strcmp(uri, "/api/v1/disks")) return disks(conn, m);
    if (!strncmp(uri, "/api/v1/disks/", 14)) return disk_action(conn, m, uri + 14);
    if (!strcmp(uri, "/api/v1/storage/nas")) return nas(conn, m, 0);
    if (!strcmp(uri, "/api/v1/storage/nas/test")) return nas(conn, m, 1);
    if (!strncmp(uri, "/api/v1/jobs/", 13)) return job(conn, m, uri + 13);
    return 0;
}
