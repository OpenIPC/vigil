/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_bridge.c — the UI's model (ui_model.h) on top of the core: channel
 * snapshots, the UI's own events cursor, the tiles, and the settings as
 * the configuration store and its schema hold them.
 *
 * The UI never includes the core; this file translates. Settings go
 * through config_put_json, the function behind PUT /api/v1/config/<section>,
 * so the on-screen UI and the HTTP API validate, save and apply a change
 * the same way, and the schema (config_schema.c) drives both. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>

#include "cJSON.h"
#include "channel.h"
#include "config.h"
#include "discovery.h"
#include "events.h"
#include "hal.h"
#include "ui_bridge.h"

#ifndef VIGIL_VERSION
#define VIGIL_VERSION "dev"
#endif

#define FIELDS_MAX 32                   /* per section; sections_build checks */

static ui_section_t *g_sections;
static uint64_t      g_ev_cursor;

/* A snapshot of the settings, taken again when the generation moves: the
 * settings pages read field by field. */
static vigil_config_t g_cfg;
static unsigned       g_cfg_gen;

static const vigil_config_t *cfg(void)
{
    if (!g_cfg_gen || config_generation() != g_cfg_gen) g_cfg_gen = config_get(&g_cfg);
    return &g_cfg;
}

/* ---- live view -------------------------------------------------------------------- */

/* The UI's copies of the core's states and event bits. */
_Static_assert((int)UI_CH_EMPTY == (int)CH_EMPTY && (int)UI_CH_DISABLED == (int)CH_DISABLED &&
                   (int)UI_CH_LIVE == (int)CH_LIVE && (int)UI_CH_AUTH == (int)CH_AUTH &&
                   (int)UI_CH_ERROR == (int)CH_ERROR,
               "ui_ch_status_t follows ch_state_t");
_Static_assert((unsigned)UI_EV_MOTION == (unsigned)CH_EV_MOTION && (unsigned)UI_EV_HUMAN == (unsigned)CH_EV_HUMAN &&
                   (unsigned)UI_EV_FACE == (unsigned)CH_EV_FACE && (unsigned)UI_EV_VEHICLE == (unsigned)CH_EV_VEHICLE,
               "UI_EV_* are CH_EV_*");

static void chan_status(int idx, ui_chan_t *st)
{
    channel_status_t cs;
    channels_status(idx, &cs);
    st->configured = cs.configured;
    st->live = cs.live;
    st->recording = cs.recording;
    st->status = (ui_ch_status_t)cs.status;         /* the same order (ui_model.h) */
    st->events = cs.events;                         /* the same bits */
    snprintf(st->view, sizeof st->view, "%s", cs.view);
    snprintf(st->rec_state, sizeof st->rec_state, "%s", cs.rec_state);
    st->rec_bytes = cs.rec_bytes;
    snprintf(st->name, sizeof st->name, "%s", cs.name);
    snprintf(st->state, sizeof st->state, "%s", cs.state);
    snprintf(st->codec, sizeof st->codec, "%s", cs.codec != VG_CODEC_NONE ? vg_codec_name(cs.codec) : "");
    st->width = cs.width;
    st->height = cs.height;
    st->fps = cs.fps;
    st->no_decoder = cs.no_decoder;
}

static int next_event(ui_event_t *ev)
{
    event_t e;
    if (!events_next(&g_ev_cursor, &e)) return 0;
    ev->ch = e.ch;
    ev->time = e.time;
    snprintf(ev->text, sizeof ev->text, "%s", e.text);
    return 1;
}

static void show_tiles(const ui_rect_t *r, int n)
{
    vg_rect_t tiles[CHANNEL_MAX];
    memset(tiles, 0, sizeof tiles);
    for (int i = 0; i < n && i < CHANNEL_MAX; ++i) tiles[i] = (vg_rect_t){ r[i].x, r[i].y, r[i].w, r[i].h };
    channels_set_tiles(tiles);
}

static time_t now(void) { return time(NULL); }

/* ---- settings ---------------------------------------------------------------------- */

static const schema_section_t *ssec(int sec) { return &config_schema.sections[sec]; }

static int item_count(int sec)
{
    const schema_section_t *s = ssec(sec);
    if (!s->is_array) return 1;
    return *(const int *)(const void *)((const char *)cfg() + s->count_off);
}

static const char *field_at(int sec, int item, const schema_field_t *f)
{
    const schema_section_t *s = ssec(sec);
    const char *base = (const char *)cfg() + s->off;
    if (s->is_array) base += s->item_size * (size_t)(item < 0 ? 0 : item);
    return base + f->off;
}

static void get_value(int sec, int item, int field, char *buf, size_t cap)
{
    const schema_section_t *s = ssec(sec);
    if (field < 0 || field >= s->n_fields || (s->is_array && (item < 0 || item >= item_count(sec)))) {
        snprintf(buf, cap, "%s", "");
        return;
    }
    const schema_field_t *f = &s->fields[field];
    const char *p = field_at(sec, item, f);
    if (f->type == SCHEMA_STRING || f->type == SCHEMA_ENUM) {
        schema_redact_field(f, p - f->off, buf, cap);
    } else {
        int v = *(const int *)(const void *)p;
        snprintf(buf, cap, "%d", f->type == SCHEMA_BOOL ? !!v : v);
    }
}

/* One item's values as a JSON object, typed by the schema. A number that
 * does not parse goes as a string, so the store says what is wrong with
 * it, at its path. */
static cJSON *object_json(const schema_section_t *s, const char *const *v)
{
    cJSON *o = cJSON_CreateObject();
    for (int i = 0; o && i < s->n_fields; ++i) {
        const schema_field_t *f = &s->fields[i];
        cJSON *j;
        if (f->type == SCHEMA_INT) {
            char *end;
            long n = strtol(v[i], &end, 10);
            j = *v[i] && !*end ? cJSON_CreateNumber((double)n) : cJSON_CreateString(v[i]);
        } else if (f->type == SCHEMA_BOOL) {
            j = cJSON_CreateBool(!strcmp(v[i], "1"));
        } else {
            j = cJSON_CreateString(v[i]);
        }
        if (!j || !cJSON_AddItemToObject(o, f->key, j)) {
            cJSON_Delete(j);
            cJSON_Delete(o);
            return NULL;
        }
    }
    return o;
}

/* "cameras[1].url" -> item 1, the index of "url"; "record.segment" ->
 * item -1, the index of "segment"; anything else -> field -1. */
static void map_error(const schema_section_t *s, const schema_error_t *se, ui_error_t *ue)
{
    ue->item = -1;
    ue->field = -1;
    size_t kl = strlen(s->key);
    const char *p = se->path;
    if (!strncmp(p, s->key, kl)) {
        p += kl;
        if (*p == '[') {
            char *end;
            ue->item = (int)strtol(p + 1, &end, 10);
            p = *end == ']' ? end + 1 : "";
        }
        if (*p == '.') {
            const schema_field_t *f = schema_field(s, p + 1);
            if (f) ue->field = (int)(f - s->fields);
        }
    }
    if (ue->field >= 0 || !se->path[0])
        snprintf(ue->msg, sizeof ue->msg, "%s", se->msg);
    else
        snprintf(ue->msg, sizeof ue->msg, "%s: %s", se->path, se->msg);
}

static void one_error(ui_errors_t *errs, const char *msg)
{
    snprintf(errs->e[0].msg, sizeof errs->e[0].msg, "%s", msg);
    errs->e[0].item = errs->e[0].field = -1;
    errs->n = errs->total = 1;
}

static ui_save_rc_t save(int sec, int n_items, const char *const *values, const int *origin,
                         unsigned gen, ui_errors_t *errs)
{
    const schema_section_t *s = ssec(sec);
    cfg();                              /* the snapshot of the generation in force */
    memset(errs, 0, sizeof *errs);
    if (gen && g_cfg_gen != gen) {
        one_error(errs, "the settings were changed elsewhere meanwhile");
        return UI_SAVE_STALE;
    }
    cJSON *doc;
    if (s->is_array) {
        int stored = item_count(sec);
        doc = cJSON_CreateArray();
        for (int k = 0; doc && k < n_items; ++k) {
            const char *row[FIELDS_MAX];
            const char *const *v = values + (size_t)k * (size_t)s->n_fields;
            for (int i = 0; i < s->n_fields && i < FIELDS_MAX; ++i) {
                row[i] = v[i];
                /* An unchanged "***" stands for this item's own password:
                 * the store would take the first camera whose URL redacts
                 * the same way, maybe another one's. */
                const schema_field_t *f = &s->fields[i];
                int o = origin ? origin[k] : -1;
                if (f->secret == SCHEMA_SECRET_URL && o >= 0 && o < stored) {
                    char red[1024];
                    const char *real = field_at(sec, o, f);
                    schema_redact_url(real, red, sizeof red);
                    if (!strcmp(red, v[i])) row[i] = real;
                }
            }
            cJSON *o = object_json(s, row);
            if (!o || !cJSON_AddItemToArray(doc, o)) {
                cJSON_Delete(o);
                cJSON_Delete(doc);
                doc = NULL;
            }
        }
    } else {
        doc = object_json(s, values);
    }
    char *text = doc ? cJSON_PrintUnformatted(doc) : NULL;
    cJSON_Delete(doc);
    if (!text) {
        one_error(errs, "out of memory");
        return UI_SAVE_FAILED;
    }
    schema_errors_t se = { 0 };
    config_rc_t rc = config_put_json_at(s->key, text, strlen(text), gen, &se);
    /* It may hold camera passwords. */
    memset(text, 0, strlen(text));
    free(text);
    for (int i = 0; i < se.n && i < UI_ERRORS_MAX; ++i) map_error(s, &se.e[i], &errs->e[i]);
    errs->n = se.n < UI_ERRORS_MAX ? se.n : UI_ERRORS_MAX;
    errs->total = se.total;
    switch (rc) {
    case CONFIG_OK: return UI_SAVE_OK;
    case CONFIG_EINVAL: return UI_SAVE_INVALID;
    case CONFIG_EUNSYNCED: return UI_SAVE_UNSYNCED;
    case CONFIG_ESTALE: return UI_SAVE_STALE;
    default: return UI_SAVE_FAILED;
    }
}

static unsigned generation(void) { return config_generation(); }

/* The schema as the UI's tables; built once, kept for the process. */
static int sections_build(void)
{
    int n = config_schema.n_sections;
    g_sections = calloc((size_t)n, sizeof *g_sections);
    if (!g_sections) return -1;
    for (int i = 0; i < n; ++i) {
        const schema_section_t *s = ssec(i);
        if (s->n_fields > FIELDS_MAX || s->max_items > UI_MAX_ITEMS) return -1;
        ui_field_t *fs = calloc((size_t)s->n_fields, sizeof *fs);
        if (!fs) return -1;
        for (int k = 0; k < s->n_fields; ++k) {
            const schema_field_t *f = &s->fields[k];
            ui_field_t *u = &fs[k];
            u->key = f->key;
            u->label = f->label ? f->label : f->key;
            u->help = f->help ? f->help : "";
            u->live = f->live;
            u->required = f->required;
            u->secret = f->secret != SCHEMA_PLAIN;
            switch (f->type) {
            case SCHEMA_STRING:
                u->kind = UI_FIELD_STRING;
                u->max_len = (int)f->size - 1;
                u->def = f->def_str ? f->def_str : "";
                break;
            case SCHEMA_ENUM:
                u->kind = UI_FIELD_ENUM;
                u->values = f->values;
                u->def = f->def_str ? f->def_str : "";
                break;
            case SCHEMA_INT:
            case SCHEMA_BOOL: {
                u->kind = f->type == SCHEMA_INT ? UI_FIELD_INT : UI_FIELD_BOOL;
                u->min = f->min;
                u->max = f->max;
                char *d = malloc(16);
                if (!d) return -1;
                snprintf(d, 16, "%d", f->type == SCHEMA_BOOL ? !!f->def_int : f->def_int);
                u->def = d;
                break;
            }
            }
        }
        g_sections[i] = (ui_section_t){ .key = s->key, .label = s->label ? s->label : s->key,
                                        .help = s->help ? s->help : "", .is_array = s->is_array,
                                        /* cameras[i] is channel i + 1 (config_to_channels) */
                                        .slots = !strcmp(s->key, "cameras"),
                                        .max_items = s->max_items, .fields = fs,
                                        .n_fields = s->n_fields };
    }
    return 0;
}

/* ---- system page ------------------------------------------------------------------------ */

static long meminfo_kib(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    long v = -1;
    size_t kl = strlen(key);
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, key, kl) && line[kl] == ':') {
            v = atol(line + kl + 1);
            break;
        }
    if (f) fclose(f);
    return v;
}

static long self_rss_kib(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[128];
    long v = -1;
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "VmRSS:", 6)) {
            v = atol(line + 6);
            break;
        }
    if (f) fclose(f);
    return v;
}

static int system_info(ui_info_t *info, int max)
{
    const hal_caps_t *c = hal_caps();
    int n = 0;
#define ADD(l, ...) do { if (n < max) { snprintf(info[n].label, sizeof info[n].label, "%s", l); \
        snprintf(info[n].value, sizeof info[n].value, __VA_ARGS__); ++n; } } while (0)
    ADD("Version", "vigild %s", VIGIL_VERSION);
    ADD("Platform", "%s", c->name);
    if (c->decoder)
        ADD("Decoder", "H.264 / H.265, up to %ux%u", c->dec_max_w, c->dec_max_h);
    else
        ADD("Decoder", "none");
    ADD("Channels", "%d%s", c->max_channels ? c->max_channels : CHANNEL_MAX,
        c->capture ? " (local inputs)" : "");
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        long up = si.uptime;
        ADD("Uptime", "%ld d %02ld:%02ld:%02ld", up / 86400, up / 3600 % 24, up / 60 % 60, up % 60);
    }
    long total = meminfo_kib("MemTotal"), avail = meminfo_kib("MemAvailable"), rss = self_rss_kib();
    if (total > 0 && avail >= 0)
        ADD("Memory", "%ld MiB available of %ld MiB; vigild uses %ld MiB", avail >> 10, total >> 10,
            rss > 0 ? rss >> 10 : 0);
    const vigil_config_t *k = cfg();
    int cams = 0, live = 0;
    for (int i = 0; i < CHANNEL_MAX; ++i) {
        channel_status_t st;
        channels_status(i, &st);
        cams += st.configured;
        live += st.configured && st.live;
    }
    ADD("Cameras", "%d configured, %d live", cams, live);
    if (k->record.path[0]) {
        struct statvfs fs;
        if (statvfs(k->record.path, &fs) == 0)
            ADD("Recording", "%.48s: %llu MiB free of %llu MiB", k->record.path,
                (unsigned long long)fs.f_bavail * fs.f_frsize >> 20,
                (unsigned long long)fs.f_blocks * fs.f_frsize >> 20);
        else
            ADD("Recording", "%.60s: not mounted", k->record.path);
    } else {
        ADD("Recording", "off");
    }
    ADD("Configuration", "%.48s, generation %u (applied %u)", config_path(), config_generation(),
        config_applied());
    ADD("HTTP API", "%s", k->api.listen[0] ? k->api.listen : "off");
#undef ADD
    return n;
}

/* ---- camera discovery --------------------------------------------------------------- */

_Static_assert(UI_DISC_NETIP == DISC_NETIP && UI_DISC_ONVIF == DISC_ONVIF && UI_DISC_DHIP == DISC_DHIP,
               "UI_DISC_* are DISC_*");
_Static_assert(UI_DISC_MAX >= DISC_JOB_MAX && UI_DISC_MAX >= DISC_MAX, "the UI holds every result");
_Static_assert((int)UI_JOB_SCAN == (int)DISC_JOB_SCAN && (int)UI_JOB_AUTOADD == (int)DISC_JOB_AUTOADD &&
                   (int)UI_JOB_TEST == (int)DISC_JOB_TEST && (int)UI_MANUAL_RTSP == (int)DISC_MANUAL_RTSP &&
                   (int)UI_MANUAL_DAHUA == (int)DISC_MANUAL_DAHUA,
               "ui_job_kind_t follows disc_job_kind_t");

static void job_err(char *err, size_t cap)
{
    if (errno == EBUSY) snprintf(err, cap, "Another search or change is running");
    else if (errno == EAGAIN && disc_auth_wait() > 0)
        snprintf(err, cap, "Too many refused passwords: try again in %d s", disc_auth_wait());
    else if (errno == EINVAL) snprintf(err, cap, "Check the values");
    else snprintf(err, cap, "Cannot start");
}

static int disc_scan_ui(unsigned protos, char *err, size_t cap)
{
    disc_scan_opts_t o;
    memset(&o, 0, sizeof o);
    o.protos = protos;
    o.seconds = 4;
    int id = disc_scan(&o);
    if (id < 0) job_err(err, cap);
    return id;
}

static int disc_devices_ui(ui_disc_dev_t *out, int max)
{
    disc_device_t *d = malloc(DISC_MAX * sizeof *d);
    if (!d) return 0;
    int n = disc_devices(d, DISC_MAX);
    if (n > max) n = max;
    for (int i = 0; i < n; ++i) {
        ui_disc_dev_t *u = &out[i];
        memset(u, 0, sizeof *u);
        snprintf(u->key, sizeof u->key, "%s", d[i].mac[0] ? d[i].mac : d[i].ip);
        snprintf(u->ip, sizeof u->ip, "%s", d[i].ip);
        snprintf(u->mac, sizeof u->mac, "%s", d[i].mac);
        snprintf(u->mask, sizeof u->mask, "%s", d[i].mask);
        snprintf(u->gateway, sizeof u->gateway, "%s", d[i].gateway);
        snprintf(u->vendor, sizeof u->vendor, "%s", d[i].vendor);
        snprintf(u->model, sizeof u->model, "%s", d[i].model);
        snprintf(u->name, sizeof u->name, "%s", d[i].name);
        u->protos = d[i].protos;
        u->port = d[i].protos & DISC_NETIP ? d[i].netip_port : d[i].http_port;
        u->channel = disc_device_channel(&d[i]);
    }
    free(d);
    return n;
}

static void disc_job_ui(ui_disc_job_t *out)
{
    disc_job_t *j = malloc(sizeof *j);
    memset(out, 0, sizeof *out);
    if (!j) return;
    disc_job_get(j);
    out->id = j->id;
    out->kind = (ui_job_kind_t)j->kind;
    out->running = j->running;
    out->dry_run = j->dry_run;
    out->progress = j->progress;
    snprintf(out->msg, sizeof out->msg, "%s", j->msg);
    out->n = j->n;
    for (int i = 0; i < j->n; ++i) {
        const disc_result_t *r = &j->r[i];
        ui_disc_res_t *u = &out->r[i];
        snprintf(u->key, sizeof u->key, "%s", r->key);
        snprintf(u->ip, sizeof u->ip, "%s", r->ip);
        snprintf(u->new_ip, sizeof u->new_ip, "%s", r->new_ip);
        u->proto = r->proto;
        u->channel = r->channel;
        u->done = r->done;
        u->ok = r->done && r->rc == DISC_OK;
        u->reprobed = r->reprobed;
        snprintf(u->msg, sizeof u->msg, "%s", r->msg);
        if (r->done && r->rc != DISC_OK) snprintf(u->error, sizeof u->error, "%s", disc_rc_name(r->rc));
        snprintf(u->codec, sizeof u->codec, "%s", r->codec);
        u->width = r->width;
        u->height = r->height;
    }
    free(j);
}

static int disc_ip_ui(const ui_disc_ip_t *items, int n, const char *start, const char *mask, const char *gw,
                      const char *user, const char *pass, int dry_run, char *err, size_t cap)
{
    disc_ip_item_t *it = calloc((size_t)(n > 0 ? n : 1), sizeof *it);
    if (!it) return -1;
    for (int i = 0; i < n; ++i) {
        snprintf(it[i].key, sizeof it[i].key, "%s", items[i].key);
        snprintf(it[i].ip, sizeof it[i].ip, "%s", items[i].ip);
    }
    int id = disc_ip_job(it, n, start, mask, gw, user, pass, dry_run);
    if (id < 0) job_err(err, cap);
    free(it);
    return id;
}

static int disc_assign_ui(const char *key, int channel, const char *user, const char *pass, int replace,
                          char *err, size_t cap)
{
    int id = disc_assign_job(key, channel, 0, user, pass, "", replace);
    if (id < 0) job_err(err, cap);
    return id;
}

static int disc_autoadd_ui(const char *user, const char *pass, char *err, size_t cap)
{
    int id = disc_autoadd_job(user, pass);
    if (id < 0) job_err(err, cap);
    return id;
}

static void manual_to_core(const ui_manual_t *u, disc_manual_t *m)
{
    memset(m, 0, sizeof *m);
    m->kind = (disc_manual_kind_t)u->kind;
    snprintf(m->host, sizeof m->host, "%s", u->host);
    m->port = u->port;
    snprintf(m->user, sizeof m->user, "%s", u->user);
    snprintf(m->pass, sizeof m->pass, "%s", u->pass);
    m->dev_channel = u->dev_channel;
    snprintf(m->main_path, sizeof m->main_path, "%s", u->main_path);
    snprintf(m->sub_path, sizeof m->sub_path, "%s", u->sub_path);
}

static int disc_test_ui(const ui_manual_t *u, char *err, size_t cap)
{
    disc_manual_t m;
    manual_to_core(u, &m);
    int id = disc_test_job(&m);
    if (id < 0) job_err(err, cap);
    volatile char *w = (volatile char *)&m;
    for (size_t i = 0; i < sizeof m; ++i) w[i] = 0;
    return id;
}

static int disc_assign_manual_ui(const ui_manual_t *u, int channel, const char *name, int replace, char *err,
                                 size_t cap)
{
    disc_manual_t m;
    manual_to_core(u, &m);
    int id = disc_assign_manual_job(&m, channel, name, replace);
    if (id < 0) job_err(err, cap);
    volatile char *w = (volatile char *)&m;
    for (size_t i = 0; i < sizeof m; ++i) w[i] = 0;
    return id;
}

int ui_bridge_init(ui_model_t *m)
{
    if (!g_sections && sections_build() < 0) return -1;
    const hal_caps_t *c = hal_caps();
    memset(m, 0, sizeof *m);
    m->role = c->capture ? "DVR" : "NVR";
    m->channels = c->max_channels > 0 && c->max_channels < CHANNEL_MAX ? c->max_channels : CHANNEL_MAX;
    if (m->channels > UI_MAX_CHANNELS) m->channels = UI_MAX_CHANNELS;
    m->fixed_tiles = c->fixed_tiles;
    m->chan_status = chan_status;
    m->next_event = next_event;
    m->show_tiles = show_tiles;
    m->now = now;
    m->sections = g_sections;
    m->n_sections = config_schema.n_sections;
    m->config_generation = generation;
    m->item_count = item_count;
    m->get_value = get_value;
    m->save = save;
    m->system_info = system_info;
    m->disc_scan = disc_scan_ui;
    m->disc_devices = disc_devices_ui;
    m->disc_job = disc_job_ui;
    m->disc_ip = disc_ip_ui;
    m->disc_assign = disc_assign_ui;
    m->disc_autoadd = disc_autoadd_ui;
    m->disc_test = disc_test_ui;
    m->disc_assign_manual = disc_assign_manual_ui;
    g_ev_cursor = 0;                    /* every event still kept, the start-up ones too */
    m->mgmt = ui_mgmt_model();
    return 0;
}
