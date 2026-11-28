/* SPDX-License-Identifier: GPL-3.0-or-later */
/* api_disc.c — the discovery endpoints (docs/api.md, "Discovery"):
 *
 *   POST /api/v1/discovery/scan        start a scan
 *   POST /api/v1/discovery/stop        end a running scan now
 *   GET  /api/v1/discovery             the devices found (so far, while a
 *                                      scan runs), and the job
 *   GET  /api/v1/discovery/job         the current or last job
 *   POST /api/v1/discovery/ip          change addresses (dry_run: the plan)
 *   POST /api/v1/discovery/autoadd     every new device into the empty channels
 *   POST /api/v1/channels/N/assign     one device into channel N
 *   POST /api/v1/channels/N/events     events of channel N's camera, from
 *                                      outside (an alarm input, a test)
 *
 * The work is a job on discovery.c's thread; these handlers start it and,
 * except for a scan, wait up to WAIT_MS for it to end, so a short change
 * answers with its outcome (200) and a long one with the running job
 * (202) to poll. Credentials in a body go to the job and are wiped here;
 * none is logged or answered back (URLs are redacted).
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "civetweb.h"
#include "cJSON.h"

#include "api_int.h"
#include "cam_events.h"
#include "channel.h"
#include "discovery.h"
#include "log.h"

#define WAIT_MS        8000          /* under CivetWeb's 10 s request timeout */
#define CRED_JOBS      30            /* jobs with credentials ... */
#define CRED_WINDOW_S  60            /* ... per this long, whoever asks */

static pthread_mutex_t g_rl_lock = PTHREAD_MUTEX_INITIALIZER;
static time_t          g_cred_at[CRED_JOBS];   /* ring of start times */
static int             g_cred_i;

static time_t mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static void wipe(void *p, size_t n)
{
    volatile unsigned char *w = p;
    while (n--) *w++ = 0;
}

/* Jobs that try a password on cameras are limited, on top of
 * disc_auth_wait()'s count of refused ones: the API must not be a way to
 * guess camera passwords at speed (or to lock their accounts). 0, or the
 * seconds to wait. */
static int cred_rate(void)
{
    time_t now = mono();
    pthread_mutex_lock(&g_rl_lock);
    time_t oldest = g_cred_at[g_cred_i];
    int wait = oldest && now - oldest < CRED_WINDOW_S ? (int)(CRED_WINDOW_S - (now - oldest)) : 0;
    if (!wait) {
        g_cred_at[g_cred_i] = now ? now : 1;
        g_cred_i = (g_cred_i + 1) % CRED_JOBS;
    }
    pthread_mutex_unlock(&g_rl_lock);
    return wait;
}

static const char *jstr(const cJSON *o, const char *key, const char *dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : dflt;
}

static void wipe_json_secret(cJSON *o, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(v) && v->valuestring) wipe(v->valuestring, strlen(v->valuestring));
}

static cJSON *protos_json(unsigned bits)
{
    cJSON *a = cJSON_CreateArray();
    for (unsigned b = DISC_NETIP; b <= DISC_DHIP; b <<= 1)
        if (bits & b) cJSON_AddItemToArray(a, cJSON_CreateString(disc_proto_name(b)));
    return a;
}

static cJSON *device_json(const disc_device_t *d, time_t now, unsigned last_scan)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "device", d->mac[0] ? d->mac : d->ip);
    cJSON_AddStringToObject(o, "ip", d->ip);
    cJSON_AddStringToObject(o, "mac", d->mac);
    cJSON_AddStringToObject(o, "mask", d->mask);
    cJSON_AddStringToObject(o, "gateway", d->gateway);
    if (d->dhcp < 0) cJSON_AddNullToObject(o, "dhcp");
    else cJSON_AddBoolToObject(o, "dhcp", d->dhcp);
    cJSON_AddItemToObject(o, "protocols", protos_json(d->protos));
    cJSON_AddStringToObject(o, "vendor", d->vendor);
    cJSON_AddStringToObject(o, "model", d->model);
    cJSON_AddStringToObject(o, "name", d->name);
    cJSON_AddStringToObject(o, "serial", d->serial);
    cJSON_AddStringToObject(o, "version", d->version);
    cJSON_AddNumberToObject(o, "channels", d->channels);
    cJSON *p = cJSON_AddObjectToObject(o, "ports");
    if (d->netip_port) cJSON_AddNumberToObject(p, "netip", d->netip_port);
    if (d->http_port) cJSON_AddNumberToObject(p, "http", d->http_port);
    if (d->dh_port) cJSON_AddNumberToObject(p, "dahua", d->dh_port);
    if (d->xaddr[0]) cJSON_AddStringToObject(o, "onvif", d->xaddr);
    cJSON_AddStringToObject(o, "interface", d->iface);
    cJSON_AddNumberToObject(o, "age", (double)(now - d->seen));
    cJSON_AddBoolToObject(o, "answered", last_scan && d->scan == last_scan);
    int ch = disc_device_channel(d);
    if (ch) cJSON_AddNumberToObject(o, "channel", ch);
    else cJSON_AddNullToObject(o, "channel");
    /* What adding it would use, without credentials. */
    cJSON *u = cJSON_AddObjectToObject(o, "urls");
    char url[512], sub[512];
    for (unsigned b = DISC_NETIP; b <= DISC_DHIP; b <<= 1) {
        if (!(d->protos & b) || b == DISC_ONVIF) continue;
        if (disc_url(d, b, "admin", "", url, sizeof url, sub, sizeof sub) == 0) {
            /* "admin:@" -> no credentials at all */
            char *at = strstr(url, "admin:@");
            if (at) memmove(at, at + 7, strlen(at + 7) + 1);
            cJSON_AddStringToObject(u, disc_proto_name(b), url);
        }
    }
    return o;
}

static const char *kind_name(disc_job_kind_t k)
{
    return k == DISC_JOB_SCAN ? "scan" : k == DISC_JOB_IP ? "ip" : k == DISC_JOB_ASSIGN ? "assign"
         : k == DISC_JOB_AUTOADD ? "autoadd" : k == DISC_JOB_TEST ? "test" : "none";
}

static cJSON *job_json(const disc_job_t *j)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddNumberToObject(o, "id", j->id);
    cJSON_AddStringToObject(o, "kind", kind_name(j->kind));
    cJSON_AddBoolToObject(o, "running", j->running);
    if (j->kind == DISC_JOB_IP) cJSON_AddBoolToObject(o, "dry_run", j->dry_run);
    cJSON_AddNumberToObject(o, "progress", j->progress);
    if (j->kind == DISC_JOB_SCAN) cJSON_AddNumberToObject(o, "found", j->found);
    cJSON_AddStringToObject(o, "message", j->msg);
    cJSON *rs = cJSON_AddArrayToObject(o, "results");
    for (int i = 0; i < j->n && rs; ++i) {
        const disc_result_t *r = &j->r[i];
        cJSON *e = cJSON_CreateObject();
        if (!e) break;
        cJSON_AddStringToObject(e, "device", r->key);
        cJSON_AddStringToObject(e, "ip", r->ip);
        if (j->kind == DISC_JOB_IP) {
            cJSON_AddStringToObject(e, "new_ip", r->new_ip);
            if (!j->dry_run) cJSON_AddBoolToObject(e, "reprobed", r->reprobed);
        }
        if (r->proto) cJSON_AddStringToObject(e, "protocol", disc_proto_name(r->proto));
        if (r->channel) cJSON_AddNumberToObject(e, "channel", r->channel);
        if (r->url[0]) cJSON_AddStringToObject(e, "url", r->url);
        if (r->sub_url[0]) cJSON_AddStringToObject(e, "sub_url", r->sub_url);
        if (r->codec[0]) {
            cJSON_AddStringToObject(e, "codec", r->codec);
            cJSON_AddNumberToObject(e, "width", r->width);
            cJSON_AddNumberToObject(e, "height", r->height);
        }
        cJSON_AddBoolToObject(e, "done", r->done);
        cJSON_AddBoolToObject(e, "ok", r->done && r->rc == DISC_OK);
        if (r->done && r->rc != DISC_OK) cJSON_AddStringToObject(e, "error", disc_rc_name(r->rc));
        cJSON_AddStringToObject(e, "message", r->msg);
        cJSON_AddItemToArray(rs, e);
    }
    return o;
}

/* The job's state, after waiting up to `wait_ms` for job `id` to end. */
static int answer_job(struct mg_connection *conn, unsigned id, int wait_ms)
{
    disc_job_t *j = malloc(sizeof *j);
    if (!j) return send_error(conn, 500, "out of memory", NULL);
    for (int waited = 0;; waited += 100) {
        disc_job_get(j);
        if (j->id != id || !j->running || waited >= wait_ms) break;
        usleep(100 * 1000);
    }
    int running = j->id == id && j->running;
    cJSON *doc = job_json(j);
    free(j);
    return send_json(conn, running ? 202 : 200, doc);
}

static int rate_limited(struct mg_connection *conn, int wait);

static int job_refused(struct mg_connection *conn)
{
    if (errno == EBUSY) return send_error(conn, 409, "another discovery job is running", NULL);
    if (errno == EAGAIN && disc_auth_wait() > 0) return rate_limited(conn, disc_auth_wait());
    if (errno == EINVAL) return send_error(conn, 400, "invalid request", NULL);
    return send_error(conn, 500, "cannot start the job", NULL);
}

static int rate_limited(struct mg_connection *conn, int wait)
{
    char ra[16];
    snprintf(ra, sizeof ra, "%d", wait);
    return send_text(conn, 429, "{\"error\":\"too many requests with camera credentials; try again later\"}\n",
                     "Retry-After", ra);
}

/* The body as a JSON object (NULL with an answer sent). */
static cJSON *body_object(struct mg_connection *conn, int *rc)
{
    size_t len = 0;
    int status = 0;
    char *body = read_body(conn, &len, &status);
    if (!body) {
        *rc = send_error(conn, status, status == 413 ? "body too large" : "out of memory", NULL);
        return NULL;
    }
    cJSON *doc = len ? cJSON_ParseWithLength(body, len) : cJSON_CreateObject();
    wipe(body, len);
    free(body);
    if (!cJSON_IsObject(doc)) {
        cJSON_Delete(doc);
        *rc = send_error(conn, 400, "the body must be a JSON object", NULL);
        return NULL;
    }
    return doc;
}

/* "x" or ["x", "y"] -> "x,y" (each must be `ok`). -1 when malformed. */
static int list_text(const cJSON *v, char *out, size_t cap)
{
    out[0] = 0;
    if (!v || cJSON_IsNull(v)) return 0;
    if (cJSON_IsString(v)) {
        if (strlen(v->valuestring) >= cap) return -1;
        snprintf(out, cap, "%s", v->valuestring);
        return 0;
    }
    if (!cJSON_IsArray(v)) return -1;
    size_t len = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, v) {
        if (!cJSON_IsString(e)) return -1;
        int n = snprintf(out + len, cap - len, "%s%s", len ? "," : "", e->valuestring);
        if (n < 0 || (size_t)n >= cap - len) return -1;
        len += (size_t)n;
    }
    return 0;
}

static int do_scan(struct mg_connection *conn)
{
    int rc;
    cJSON *b = body_object(conn, &rc);
    if (!b) return rc;
    disc_scan_opts_t o;
    memset(&o, 0, sizeof o);
    char protos[64];
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(b, "protocols");
    int bad = list_text(v, protos, sizeof protos) < 0;
    o.protos = !bad && protos[0] ? disc_proto_bits(protos) : DISC_ALL;
    bad |= !o.protos;
    v = cJSON_GetObjectItemCaseSensitive(b, "seconds");
    if (v && (!cJSON_IsNumber(v) || v->valuedouble < 1 || v->valuedouble > DISC_SECS_MAX)) bad = 1;
    o.seconds = cJSON_IsNumber(v) ? (int)v->valuedouble : 3;
    bad |= list_text(cJSON_GetObjectItemCaseSensitive(b, "interfaces"), o.ifaces, sizeof o.ifaces) < 0;
    bad |= list_text(cJSON_GetObjectItemCaseSensitive(b, "targets"), o.targets, sizeof o.targets) < 0;
    v = cJSON_GetObjectItemCaseSensitive(b, "broadcast");
    if (v && !cJSON_IsBool(v)) bad = 1;
    o.no_broadcast = cJSON_IsFalse(v);
    const cJSON *ports = cJSON_GetObjectItemCaseSensitive(b, "ports");
    static const char *pk[3] = { "netip", "onvif", "dhip" };
    for (int i = 0; i < 3 && ports; ++i) {
        v = cJSON_GetObjectItemCaseSensitive(ports, pk[i]);
        if (!v) continue;
        if (!cJSON_IsNumber(v) || v->valuedouble < 1 || v->valuedouble > 65535) bad = 1;
        else o.port[i] = (int)v->valuedouble;
    }
    cJSON_Delete(b);
    if (bad) return send_error(conn, 400, "invalid scan request", NULL);
    int id = disc_scan(&o);
    if (id < 0) return job_refused(conn);
    return answer_job(conn, (unsigned)id, 0);
}

static int do_ip(struct mg_connection *conn)
{
    int rc;
    cJSON *b = body_object(conn, &rc);
    if (!b) return rc;
    disc_ip_item_t *items = calloc(DISC_JOB_MAX, sizeof *items);
    int n = 0, bad = !items;
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(b, "devices");
    if (cJSON_IsArray(list)) {
        const cJSON *e;
        cJSON_ArrayForEach(e, list) {
            const char *key = cJSON_IsString(e) ? e->valuestring : jstr(e, "device", NULL);
            const char *ip = cJSON_IsObject(e) ? jstr(e, "ip", "") : "";
            if (!items || n == DISC_JOB_MAX || !key || strlen(key) >= sizeof items->key || strlen(ip) >= sizeof items->ip) {
                bad = 1;
                break;
            }
            snprintf(items[n].key, sizeof items[n].key, "%s", key);
            snprintf(items[n].ip, sizeof items[n].ip, "%s", ip);
            ++n;
        }
    } else if (items && jstr(b, "device", NULL)) {
        const char *key = jstr(b, "device", ""), *ip = jstr(b, "ip", "");
        if (strlen(key) >= sizeof items->key || strlen(ip) >= sizeof items->ip) bad = 1;
        else {
            snprintf(items[0].key, sizeof items[0].key, "%s", key);
            snprintf(items[0].ip, sizeof items[0].ip, "%s", ip);
            n = 1;
        }
    }
    const cJSON *dr = cJSON_GetObjectItemCaseSensitive(b, "dry_run");
    if (dr && !cJSON_IsBool(dr)) bad = 1;
    int dry = cJSON_IsTrue(dr);
    if (!n) bad = 1;
    int wait = 0;
    int id = -1;
    if (!bad && !dry && (wait = cred_rate()) > 0) {
        id = -2;
    } else if (!bad) {
        id = disc_ip_job(items, n, jstr(b, "start", ""), jstr(b, "mask", "255.255.255.0"), jstr(b, "gateway", ""),
                         jstr(b, "user", "admin"), jstr(b, "password", ""), dry);
    }
    wipe_json_secret(b, "password");
    cJSON_Delete(b);
    free(items);
    if (bad) return send_error(conn, 400, "invalid request: devices (or device), mask, start or ip", NULL);
    if (id == -2) return rate_limited(conn, wait);
    if (id < 0) return job_refused(conn);
    return answer_job(conn, (unsigned)id, WAIT_MS);
}

/* A device described by hand: {"protocol", "address", "port", "user",
 * "password", "channel", "main", "sub"}. 0, or -1 (malformed). */
static int manual_from(const cJSON *o, disc_manual_t *m)
{
    memset(m, 0, sizeof *m);
    if (!cJSON_IsObject(o)) return -1;
    int kind = disc_manual_kind(jstr(o, "protocol", ""));
    if (kind < 0) return -1;
    m->kind = (disc_manual_kind_t)kind;
    static const struct { const char *key; size_t off, size; } S[] = {
        { "address", offsetof(disc_manual_t, host), sizeof m->host },
        { "user", offsetof(disc_manual_t, user), sizeof m->user },
        { "password", offsetof(disc_manual_t, pass), sizeof m->pass },
        { "main", offsetof(disc_manual_t, main_path), sizeof m->main_path },
        { "sub", offsetof(disc_manual_t, sub_path), sizeof m->sub_path },
    };
    for (size_t i = 0; i < sizeof S / sizeof S[0]; ++i) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, S[i].key);
        if (v && !cJSON_IsString(v)) return -1;
        if (v && strlen(v->valuestring) >= S[i].size) return -1;
        if (v) memcpy((char *)m + S[i].off, v->valuestring, strlen(v->valuestring) + 1);
    }
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(o, "port");
    const cJSON *ch = cJSON_GetObjectItemCaseSensitive(o, "channel");
    if ((port && (!cJSON_IsNumber(port) || port->valuedouble < 1 || port->valuedouble > 65535)) ||
        (ch && (!cJSON_IsNumber(ch) || ch->valuedouble < 1 || ch->valuedouble > 256)))
        return -1;
    m->port = port ? (int)port->valuedouble : 0;
    m->dev_channel = ch ? (int)ch->valuedouble : 1;
    return m->host[0] ? 0 : -1;
}

static void manual_wipe(disc_manual_t *m) { wipe(m, sizeof *m); }

static int do_test(struct mg_connection *conn)
{
    int rc;
    cJSON *b = body_object(conn, &rc);
    if (!b) return rc;
    disc_manual_t m;
    int bad = manual_from(b, &m) < 0, wait = 0, id = -1;
    wipe_json_secret(b, "password");
    cJSON_Delete(b);
    if (bad) {
        manual_wipe(&m);
        return send_error(conn, 400, "invalid device: protocol (netip, onvif, dahua, rtsp), address, port, "
                                     "user, password, channel, main, sub", NULL);
    }
    if ((wait = cred_rate()) > 0) id = -2;
    else id = disc_test_job(&m);
    manual_wipe(&m);
    if (id == -2) return rate_limited(conn, wait);
    if (id < 0) return job_refused(conn);
    return answer_job(conn, (unsigned)id, WAIT_MS);
}

static int do_assign_manual(struct mg_connection *conn, int ch, cJSON *b)
{
    disc_manual_t m;
    const cJSON *rep = cJSON_GetObjectItemCaseSensitive(b, "replace");
    const char *name = jstr(b, "name", "");
    int bad = manual_from(cJSON_GetObjectItemCaseSensitive(b, "manual"), &m) < 0 || (rep && !cJSON_IsBool(rep)) ||
              strlen(name) >= 32;
    int wait = 0, id = -1;
    if (!bad && (wait = cred_rate()) > 0) id = -2;
    else if (!bad) id = disc_assign_manual_job(&m, ch, name, cJSON_IsTrue(rep));
    manual_wipe(&m);
    cJSON *man = cJSON_GetObjectItemCaseSensitive(b, "manual");
    wipe_json_secret(man, "password");
    cJSON_Delete(b);
    if (bad) return send_error(conn, 400, "invalid request: manual (protocol, address, ...), name, replace", NULL);
    if (id == -2) return rate_limited(conn, wait);
    if (id < 0) return job_refused(conn);
    return answer_job(conn, (unsigned)id, WAIT_MS);
}

static int do_assign(struct mg_connection *conn, int ch)
{
    int rc;
    cJSON *b = body_object(conn, &rc);
    if (!b) return rc;
    if (cJSON_GetObjectItemCaseSensitive(b, "manual")) return do_assign_manual(conn, ch, b);
    const char *key = jstr(b, "device", NULL), *proto = jstr(b, "protocol", "");
    unsigned bits = *proto ? disc_proto_bits(proto) : 0;
    const cJSON *rep = cJSON_GetObjectItemCaseSensitive(b, "replace");
    int bad = !key || (*proto && (!bits || bits == DISC_ALL)) || (rep && !cJSON_IsBool(rep));
    int wait = 0, id = -1;
    if (!bad && (wait = cred_rate()) > 0) id = -2;
    else if (!bad)
        id = disc_assign_job(key, ch, bits, jstr(b, "user", "admin"), jstr(b, "password", ""), jstr(b, "name", ""),
                             cJSON_IsTrue(rep));
    wipe_json_secret(b, "password");
    cJSON_Delete(b);
    if (bad) return send_error(conn, 400, "invalid request: device, protocol, replace", NULL);
    if (id == -2) return rate_limited(conn, wait);
    if (id < 0) return job_refused(conn);
    return answer_job(conn, (unsigned)id, WAIT_MS);
}

/* {"events": ["motion", ...], "state": "start" | "stop"}: as if the
 * camera had reported them now. Without a state, a moment (held
 * CHANNEL_EVENT_HOLD s, channels_report_events); a start holds until its
 * stop (cam_events.h, origin "api"). The answer has the events held now.
 * Nothing else of the channel changes. */
static int do_events(struct mg_connection *conn, int ch)
{
    int rc;
    cJSON *b = body_object(conn, &rc);
    if (!b) return rc;
    const cJSON *ev = cJSON_GetObjectItemCaseSensitive(b, "events"), *e;
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(b, "state");
    unsigned bits = 0;
    int bad = !cJSON_IsArray(ev), what = CAM_EV_PULSE;
    cJSON_ArrayForEach(e, ev) {
        int bit = cJSON_IsString(e) ? ch_event_bit(e->valuestring) : -1;
        if (bit < 0) bad = 1;
        else bits |= 1u << bit;
    }
    if (state && !cJSON_IsNull(state)) {
        if (cJSON_IsString(state) && !strcmp(state->valuestring, "start")) what = CAM_EV_START;
        else if (cJSON_IsString(state) && !strcmp(state->valuestring, "stop")) what = CAM_EV_STOP;
        else if (!cJSON_IsString(state) || strcmp(state->valuestring, "pulse")) bad = 1;
    }
    cJSON_Delete(b);
    if (bad) {
        char names[256];
        size_t len = 0;
        names[0] = 0;
        for (int k = 0; ch_event_name(k) && len < sizeof names; ++k) {
            int n = snprintf(names + len, sizeof names - len, "%s%s", k ? ", " : "", ch_event_name(k));
            if (n < 0) break;
            len += (size_t)n;
        }
        char msg[320];
        snprintf(msg, sizeof msg, "invalid request: events (%s), state (start, stop, pulse)", names);
        return send_error(conn, 400, msg, NULL);
    }
    if (what == CAM_EV_PULSE) channels_report_events(ch - 1, bits);
    else cam_event_report(ch - 1, bits, what, CAM_EV_SRC_API, NULL);
    channel_status_t st;
    channels_status(ch - 1, &st);
    cJSON *doc = cJSON_CreateObject(), *held = cJSON_AddArrayToObject(doc, "events");
    cJSON_AddNumberToObject(doc, "channel", ch);
    for (int k = 0; held && ch_event_name(k); ++k)
        if (st.events & (1u << k)) cJSON_AddItemToArray(held, cJSON_CreateString(ch_event_name(k)));
    return send_json(conn, 200, doc);
}

static int do_autoadd(struct mg_connection *conn)
{
    int rc;
    cJSON *b = body_object(conn, &rc);
    if (!b) return rc;
    int wait = cred_rate(), id = -2;
    if (!wait) id = disc_autoadd_job(jstr(b, "user", "admin"), jstr(b, "password", ""));
    wipe_json_secret(b, "password");
    cJSON_Delete(b);
    if (id == -2) return rate_limited(conn, wait);
    if (id < 0) return job_refused(conn);
    return answer_job(conn, (unsigned)id, WAIT_MS);
}

static int list_devices(struct mg_connection *conn)
{
    disc_device_t *list = malloc(DISC_MAX * sizeof *list);
    disc_job_t *j = malloc(sizeof *j);
    cJSON *doc = cJSON_CreateObject();
    if (!list || !j || !doc) {
        free(list);
        free(j);
        cJSON_Delete(doc);
        return send_error(conn, 500, "out of memory", NULL);
    }
    /* The generation first: a client that sees it move polls again and
     * misses nothing that changed while this answer was made. */
    unsigned gen = disc_generation(), last = disc_last_scan();
    int n = disc_devices(list, DISC_MAX);
    time_t now = mono();
    cJSON *arr = cJSON_AddArrayToObject(doc, "devices");
    for (int i = 0; i < n && arr; ++i) cJSON_AddItemToArray(arr, device_json(&list[i], now, last));
    cJSON_AddNumberToObject(doc, "count", n);
    cJSON_AddNumberToObject(doc, "generation", gen);
    disc_job_get(j);
    cJSON_AddBoolToObject(doc, "running", j->running);
    cJSON_AddItemToObject(doc, "job", job_json(j));
    free(list);
    free(j);
    return send_json(conn, 200, doc);
}

int api_disc_request(struct mg_connection *conn, const char *uri, const char *m)
{
    int get = !strcmp(m, "GET") || !strcmp(m, "HEAD"), post = !strcmp(m, "POST");
    if (!strcmp(uri, "/api/v1/discovery")) {
        if (get) return list_devices(conn);
        if (!strcmp(m, "DELETE")) {
            disc_clear();
            return list_devices(conn);
        }
        return method_not_allowed(conn, "GET, HEAD, DELETE");
    }
    if (!strcmp(uri, "/api/v1/discovery/stop")) {
        if (!post) return method_not_allowed(conn, "POST");
        int id = disc_scan_stop();
        if (id < 0) return send_error(conn, 409, "no scan is running", NULL);
        return answer_job(conn, (unsigned)id, 2000);    /* it ends within its 200 ms poll */
    }
    if (!strcmp(uri, "/api/v1/discovery/job")) {
        if (!get) return method_not_allowed(conn, "GET, HEAD");
        disc_job_t *j = malloc(sizeof *j);
        if (!j) return send_error(conn, 500, "out of memory", NULL);
        disc_job_get(j);
        cJSON *doc = job_json(j);
        free(j);
        return send_json(conn, 200, doc);
    }
    static const struct { const char *path; int (*fn)(struct mg_connection *); } posts[] = {
        { "/api/v1/discovery/scan", do_scan },
        { "/api/v1/discovery/ip", do_ip },
        { "/api/v1/discovery/autoadd", do_autoadd },
        { "/api/v1/devices/test", do_test },
    };
    for (size_t i = 0; i < sizeof posts / sizeof posts[0]; ++i)
        if (!strcmp(uri, posts[i].path)) return post ? posts[i].fn(conn) : method_not_allowed(conn, "POST");
    int ch, used = 0;
    if (sscanf(uri, "/api/v1/channels/%d/assign%n", &ch, &used) == 1 && used && !uri[used]) {
        if (!post) return method_not_allowed(conn, "POST");
        if (ch < 1 || ch > CHANNEL_MAX) return send_error(conn, 404, "no such channel", NULL);
        return do_assign(conn, ch);
    }
    if (sscanf(uri, "/api/v1/channels/%d/events%n", &ch, &used) == 1 && used && !uri[used]) {
        if (!post) return method_not_allowed(conn, "POST");
        if (ch < 1 || ch > channels_count()) return send_error(conn, 404, "no such channel", NULL);
        return do_events(conn, ch);
    }
    return send_error(conn, 404, "not found", NULL);
}
