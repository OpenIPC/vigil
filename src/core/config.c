/* SPDX-License-Identifier: GPL-3.0-or-later */
/* config.c — the configuration file and store (see config.h). */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "config.h"
#include "log.h"
#include "logs.h"
#include "schema_yaml.h"
#include "secret.h"

#define FILE_MAX (256u << 10)       /* far above any real configuration */

void config_defaults(vigil_config_t *c)
{
    memset(c, 0, sizeof *c);
    schema_defaults(&config_schema, c);
}

void config_trim(vigil_config_t *c)
{
    while (c->n_cameras > 0 && !c->cameras[c->n_cameras - 1].url[0]) --c->n_cameras;
}

void config_to_channels(const vigil_config_t *c, channels_config_t *cc)
{
    memset(cc, 0, sizeof *cc);
    cc->n = c->n_cameras;
    for (int i = 0; i < c->n_cameras && i < CHANNEL_MAX; ++i) {
        /* An empty URL is an empty slot: cameras are in tile order, so a
         * camera can sit at channel 7 with nothing at 3 to 6. */
        int has = c->cameras[i].url[0] != 0;
        cc->urls[i] = has && c->cameras[i].enabled ? c->cameras[i].url : NULL;
        cc->disabled[i] = has && !c->cameras[i].enabled;
        cc->names[i] = c->cameras[i].name;
        cc->sub_urls[i] = c->cameras[i].sub_url;
        int m = ch_events_mode(c->cameras[i].events);
        cc->events[i] = m < 0 ? CH_EVENTS_AUTO : (ch_events_mode_t)m;
        cc->onvif_urls[i] = c->cameras[i].onvif_url;
    }
    cc->rec_dir = c->record.path[0] ? c->record.path : NULL;
    cc->stop_when_full = !strcmp(c->record.on_full, "stop");
    for (int i = 0; i < c->n_quotas; ++i) {
        int k = c->quota[i].camera - 1;
        if (k < 0 || k >= CHANNEL_MAX) continue;
        cc->quota_bytes[k] = (uint64_t)c->quota[i].max_gb * 1000000000ull;
        cc->quota_age[k] = (time_t)c->quota[i].max_days * 86400;
    }
    cc->seg_secs = c->record.segment;
    cc->keep = c->record.keep;
    cc->allow_rootfs = c->record.allow_rootfs;
}

int config_to_schedule(const vigil_config_t *c, rec_week_t weeks[CHANNEL_MAX])
{
    memset(weeks, 0, sizeof(rec_week_t) * CHANNEL_MAX);
    for (int i = 0; i < c->n_schedules; ++i) {
        int k = c->schedule[i].camera - 1;
        if (k < 0 || k >= CHANNEL_MAX) continue;
        weeks[k].set = 1;
        for (int d = 0; d < 7; ++d)
            memcpy(weeks[k].day[d], c->schedule[i].day[d], sizeof weeks[k].day[d]);
    }
    return CHANNEL_MAX;
}

/* ---- files ---------------------------------------------------------------------- */

/* `text` holds `len` bytes and a NUL after them. Anything but white space
 * after the document is an error, not ignored. */
static cJSON *parse_json(const char *text, size_t len, schema_errors_t *errs)
{
    const char *end = NULL;
    cJSON *doc = cJSON_ParseWithLengthOpts(text, len + 1, &end, 1);
    if (!doc) schema_error(errs, "", "not valid JSON (at byte %ld)", end ? (long)(end - text) : 0L);
    return doc;
}

config_format_t config_format(const char *path)
{
    size_t n = strlen(path);
    return n >= 5 && !strcmp(path + n - 5, ".json") ? CONFIG_JSON : CONFIG_YAML;
}

/* ---- the passwords in the file (docs/security.md) -------------------------------------
 * In memory every password is itself (the channels connect with it, the
 * API hides it); in the file each is sealed (secret.h), the field's path
 * its additional data. A camera URL keeps its shape, only the password in
 * it sealed: "rtsp://admin:enc:v1:...@10.0.0.2/stream". */

/* A sealed value that did not open (no key, another key). Its field in
 * memory is unknown ("", or the URL without a password); the file keeps
 * the sealed text as it was, so that saving other settings does not turn
 * it into no password, and putting the old key back brings it back. Once
 * the field holds anything else (a password set again, another URL), that
 * is sealed instead. */
typedef struct kept {
    struct kept *next;
    char        aad[128];           /* the field */
    char       *mem;                /* its value in memory */
    char       *file;               /* its value in the file */
    int         used;               /* written back by the last save */
} kept_t;

typedef struct {
    int plain;                      /* passwords found in the clear (an older file, a hand edit) */
    int lost;                       /* sealed ones that did not open */
    secret_rc_t why;                /* the first failure */
    kept_t *kept;                   /* opening: those that did not open; sealing: those to keep */
} seal_stats_t;

/* The sealed values of the store's file that did not open (config_init). */
static pthread_mutex_t g_kept_lock = PTHREAD_MUTEX_INITIALIZER;
static kept_t         *g_kept;

static void kept_free(kept_t *k)
{
    while (k) {
        kept_t *next = k->next;
        free(k->mem);
        free(k->file);
        free(k);
        k = next;
    }
}

/* The password of a URL's credentials: [*b, *e) (the same span
 * schema_redact_url hides). 0 when the URL has one, non-empty. */
static int url_password(const char *url, size_t *b, size_t *e)
{
    const char *auth = strstr(url, "://");
    if (!auth) return -1;
    auth += 3;
    const char *at = strrchr(auth, '@');
    const char *colon = at ? memchr(auth, ':', (size_t)(at - auth)) : NULL;
    if (!colon || colon + 1 == at) return -1;
    *b = (size_t)(colon + 1 - url);
    *e = (size_t)(at - url);
    return 0;
}

/* Sealing: a field that still holds what a sealed value that did not
 * open left in memory gets that sealed value back. 1 when it did. */
static int keep_field(cJSON *v, const char *aad, seal_stats_t *st)
{
    for (kept_t *k = st->kept; k; k = k->next) {
        if (strcmp(k->aad, aad) != 0) continue;
        if (!cJSON_IsString(v) || strcmp(v->valuestring, k->mem) != 0) return 0;
        if (!cJSON_SetValuestring(v, k->file)) return -1;
        k->used = 1;
        return 1;
    }
    return 0;
}

/* One secret field's value in `o`, sealed (`seal`) or opened in place.
 * -1 (`st->why`) when sealing fails, or there is no memory for an opened
 * value: a field is never left in the clear when it should be sealed, or
 * sealed when it should be in the clear. */
static int seal_field(cJSON *o, const schema_field_t *f, const char *aad, int seal, seal_stats_t *st)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, f->key);
    if (seal && st->kept) {
        int k = keep_field(v, aad, st);
        if (k) {
            if (k < 0) st->why = SECRET_ENOMEM;
            return k < 0 ? -1 : 0;
        }
    }
    if (!cJSON_IsString(v) || !v->valuestring[0]) return 0;
    const char *s = v->valuestring;
    size_t b = 0, e = strlen(s);
    if (f->secret == SCHEMA_SECRET_URL && url_password(s, &b, &e) < 0) return 0;
    size_t plen = e - b, cap = SECRET_SEALED_SIZE(plen) + 1;
    char *part = malloc(plen + 1), *res = malloc(cap > plen + 1 ? cap : plen + 1);
    char *text = NULL;
    kept_t *k = NULL;
    int rc = 0;
    if (!part || !res) {
        st->why = SECRET_ENOMEM;
        rc = -1;
        goto out;
    }
    memcpy(part, s + b, plen);
    part[plen] = 0;
    res[0] = 0;
    if (seal) {
        /* Even one that looks sealed: it is opened once, and is itself again. */
        secret_rc_t r = secret_seal(aad, part, res, cap);
        if (r != SECRET_OK) {
            st->why = r;
            rc = -1;
            goto out;
        }
    } else if (secret_is_sealed(part)) {
        secret_rc_t r = secret_open(aad, part, res, plen + 1);
        if (r != SECRET_OK) {
            vg_log("%s: the stored password cannot be read (%s); set it again", aad + 7, secret_strerror(r));
            if (!st->lost++) st->why = r;
            res[0] = 0;
            k = calloc(1, sizeof *k);
            if (!k || !(k->file = strdup(s))) {
                st->why = SECRET_ENOMEM;
                rc = -1;
                goto out;
            }
            snprintf(k->aad, sizeof k->aad, "%s", aad);
        }
    } else {
        ++st->plain;
        goto out;
    }
    /* A URL whose password did not open loses the ':' with it. */
    size_t rl = strlen(res), lead = b - (!res[0] && f->secret == SCHEMA_SECRET_URL);
    text = malloc(lead + rl + strlen(s + e) + 1);
    if (text) {
        memcpy(text, s, lead);
        memcpy(text + lead, res, rl);
        strcpy(text + lead + rl, s + e);
    }
    if (!text || (k && !(k->mem = strdup(text))) || !cJSON_SetValuestring(v, text)) {
        st->why = SECRET_ENOMEM;
        rc = -1;
        goto out;
    }
    if (k) {
        k->next = st->kept;
        st->kept = k;
        k = NULL;
    }
out:
    kept_free(k);
    if (part) secret_wipe(part, plen);
    if (res) secret_wipe(res, strlen(res));
    if (text) secret_wipe(text, strlen(text));
    free(part);
    free(res);
    free(text);
    return rc;
}

/* Every secret field of the configuration document `doc`. */
static int seal_doc(cJSON *doc, int seal, seal_stats_t *st)
{
    for (int i = 0; i < config_schema.n_sections; ++i) {
        const schema_section_t *sec = &config_schema.sections[i];
        cJSON *so = cJSON_GetObjectItemCaseSensitive(doc, sec->key);
        int k = 0;
        for (cJSON *o = sec->is_array ? (cJSON_IsArray(so) ? so->child : NULL) : so; o;
             o = sec->is_array ? o->next : NULL, ++k) {
            if (!cJSON_IsObject(o)) continue;
            for (int j = 0; j < sec->n_fields; ++j) {
                const schema_field_t *f = &sec->fields[j];
                if (!f->secret) continue;
                char aad[128];
                if (sec->is_array) snprintf(aad, sizeof aad, "config:%s[%d].%s", sec->key, k, f->key);
                else snprintf(aad, sizeof aad, "config:%s.%s", sec->key, f->key);
                if (seal_field(o, f, aad, seal, st) < 0) return -1;
            }
        }
    }
    return 0;
}

static int load_file(const char *path, vigil_config_t *c, schema_errors_t *errs, seal_stats_t *st)
{
    config_defaults(c);
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno == ENOENT) return 1;
        schema_error(errs, "", "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    char *buf = malloc(FILE_MAX + 1);
    size_t n = buf ? fread(buf, 1, FILE_MAX + 1, f) : 0;
    int rd_err = ferror(f);
    fclose(f);
    if (!buf || rd_err || n > FILE_MAX) {
        schema_error(errs, "", !buf ? "out of memory" : rd_err ? "cannot read %s" : "%s is too large",
                     path);
        free(buf);
        return -1;
    }
    buf[n] = 0;
    cJSON *doc = config_format(path) == CONFIG_JSON ? parse_json(buf, n, errs)
                                                    : schema_yaml_parse(&config_schema, buf, n, errs);
    free(buf);
    if (!doc) return -1;
    if (seal_doc(doc, 0, st) < 0) {
        schema_error(errs, "", "out of memory");
        cJSON_Delete(doc);
        return -1;
    }
    /* Not strict: a key this vigild does not know (a newer one wrote the
     * file) is skipped instead of costing every other setting. */
    int rc = schema_from_json(&config_schema, doc, c, 0, errs);
    cJSON_Delete(doc);
    if (rc == 0) rc = schema_validate(&config_schema, c, errs);
    if (rc < 0) {
        config_defaults(c);
        return -1;
    }
    return 0;
}

int config_load_file(const char *path, vigil_config_t *c, schema_errors_t *errs)
{
    seal_stats_t st = { 0 };
    int rc = load_file(path, c, errs, &st);
    kept_free(st.kept);
    return rc;
}

static int write_all(int fd, const char *p, size_t len)
{
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int config_write_atomic(const char *path, const char *data, size_t len, char *err, size_t cap)
{
    char tmp[512], dir[512];
    if (snprintf(tmp, sizeof tmp, "%s.XXXXXX", path) >= (int)sizeof tmp) {
        snprintf(err, cap, "path too long");
        return -1;
    }
    /* A fresh, exclusively created name: a fixed PATH.tmp could already be
     * there as someone's world-readable file or a symlink, and the write
     * would go into it. mkstemp creates 0600 (fchmod says so anyway). */
    int fd = mkstemp(tmp);
    if (fd < 0) {
        snprintf(err, cap, "cannot create a file next to %s: %s", path, strerror(errno));
        return -1;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (fchmod(fd, 0600) < 0 || write_all(fd, data, len) < 0 || fsync(fd) < 0) {
        snprintf(err, cap, "cannot write %s: %s", tmp, strerror(errno));
        close(fd);
        unlink(tmp);
        return -1;
    }
    if (close(fd) < 0) {
        snprintf(err, cap, "cannot write %s: %s", tmp, strerror(errno));
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) < 0) {
        snprintf(err, cap, "cannot rename %s: %s", tmp, strerror(errno));
        unlink(tmp);
        return -1;
    }
    /* The rename itself must reach the disk, or a power cut can bring the
     * old file back after the API said "saved". A filesystem that cannot
     * sync a directory at all (EINVAL) has nothing more to offer. */
    snprintf(dir, sizeof dir, "%s", path);
    int dfd = open(dirname(dir), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int rc = dfd < 0 ? -1 : fsync(dfd);
    if (rc < 0 && errno != EINVAL) {
        snprintf(err, cap, "%s replaced, but syncing its directory failed: %s", path, strerror(errno));
        if (dfd >= 0) close(dfd);
        return 1;
    }
    if (dfd >= 0) close(dfd);
    return 0;
}

static const char yaml_header[] =
    "vigild configuration: https://github.com/OpenIPC/vigil/blob/main/docs/api.md\n"
    "vigild rewrites this file when the settings are changed through its HTTP\n"
    "API or on-screen UI. The comments come from vigild's own description of\n"
    "each setting: comments and formatting written by hand are not kept.";

int config_check_text(const vigil_config_t *c, config_format_t fmt, char *err, size_t cap)
{
    schema_errors_t errs = { 0 };
    if (fmt != CONFIG_YAML || schema_check_utf8(&config_schema, NULL, c, &errs) == 0) return 0;
    snprintf(err, cap, "%s: %s; it cannot be written as YAML", errs.e[0].path, errs.e[0].msg);
    return -1;
}

static char *file_text(const vigil_config_t *c, config_format_t fmt, char *err, size_t cap)
{
    cJSON *doc = schema_to_json(&config_schema, c, 0);
    char *text = NULL;
    seal_stats_t st = { 0 };
    pthread_mutex_lock(&g_kept_lock);
    st.kept = g_kept;
    int sealed = doc ? seal_doc(doc, 1, &st) : 0;
    if (sealed == 0) {
        /* A field that holds something else now was set again: its old
         * sealed value is not brought back by a later save. */
        for (kept_t **pk = &g_kept; *pk;) {
            kept_t *k = *pk;
            if (k->used) {
                k->used = 0;
                pk = &k->next;
            } else {
                *pk = k->next;
                k->next = NULL;
                kept_free(k);
            }
        }
    }
    pthread_mutex_unlock(&g_kept_lock);
    if (sealed < 0) {
        char key[300];
        snprintf(err, cap, "cannot seal the passwords: %s (%s)", secret_strerror(st.why),
                 secret_key_path(key, sizeof key));
        cJSON_Delete(doc);
        return NULL;
    }
    if (!doc) snprintf(err, cap, "out of memory");
    if (doc && fmt == CONFIG_YAML) {
        text = schema_yaml_emit(&config_schema, doc, yaml_header);
    } else if (doc && (text = cJSON_Print(doc)) != NULL) {
        /* cJSON_Print's terminator becomes the newline that ends a POSIX
         * text file; the string needs one byte more for its own. */
        size_t len = strlen(text);
        char *t = realloc(text, len + 2);
        if (t) {
            t[len] = '\n';
            t[len + 1] = 0;
        } else {
            free(text);
        }
        text = t;
    }
    if (doc && !text) snprintf(err, cap, "out of memory");
    cJSON_Delete(doc);
    return text;
}

char *config_file_text(const vigil_config_t *c, config_format_t fmt)
{
    char err[400];
    char *text = file_text(c, fmt, err, sizeof err);
    if (!text) vg_log("config: %s", err);
    return text;
}

int config_save_file(const char *path, const vigil_config_t *c, char *err, size_t cap)
{
    if (config_check_text(c, config_format(path), err, cap) < 0) return -1;
    char *text = file_text(c, config_format(path), err, cap);
    if (!text) return -1;
    int rc = config_write_atomic(path, text, strlen(text), err, cap);
    free(text);
    return rc;
}

/* Keep the file at `path` as `bad` before it is replaced: a hard link
 * where the filesystem has them (vfat has not), a copy otherwise. 0 when
 * kept (or there is no file), -1 with `err`. */
static int keep_copy(const char *path, const char *bad, char *err, size_t cap)
{
    if (unlink(bad) < 0 && errno != ENOENT) {
        snprintf(err, cap, "cannot replace %s: %s", bad, strerror(errno));
        return -1;
    }
    if (link(path, bad) == 0) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno == ENOENT) return 0;
        snprintf(err, cap, "cannot keep %s as %s: %s", path, bad, strerror(errno));
        return -1;
    }
    char *buf = malloc(FILE_MAX + 1);
    size_t n = buf ? fread(buf, 1, FILE_MAX + 1, f) : 0;
    int bad_read = !buf || ferror(f) || n > FILE_MAX;
    fclose(f);
    int rc = bad_read ? -1 : config_write_atomic(bad, buf, n, err, cap);
    free(buf);
    if (bad_read) snprintf(err, cap, "cannot keep %s as %s: cannot read it", path, bad);
    return rc < 0 ? -1 : 0;
}

/* ---- the store ---------------------------------------------------------------------- */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;   /* g_cfg, g_gen */
static pthread_mutex_t g_write = PTHREAD_MUTEX_INITIALIZER;  /* one writer at a time */
static vigil_config_t  g_cfg;
static int             g_cfg_init;
static unsigned        g_gen = 1, g_applied;
static char            g_path[256] = CONFIG_DEFAULT_PATH;
static int             g_file_bad;  /* the file failed to load: keep it as .bad */

static void log_errors(const char *what, const schema_errors_t *errs)
{
    for (int i = 0; i < errs->n; ++i)
        vg_log("%s: %s%s%s", what, errs->e[i].path, errs->e[i].path[0] ? ": " : "", errs->e[i].msg);
    if (errs->total > errs->n) vg_log("%s: %d more errors", what, errs->total - errs->n);
}

int config_init(const char *path)
{
    vigil_config_t c;
    schema_errors_t errs = { 0 };
    if (strlen(path) >= sizeof g_path) {
        vg_log("%.64s...: the configuration path is too long (at most %zu bytes)", path, sizeof g_path - 1);
        return -2;
    }
    snprintf(g_path, sizeof g_path, "%s", path);
    seal_stats_t st = { 0 };
    int rc = load_file(g_path, &c, &errs, &st);
    if (rc == 1) vg_log("%s: not there, using the defaults", g_path);
    else if (rc == 0) vg_log("%s: loaded", g_path);
    pthread_mutex_lock(&g_kept_lock);
    kept_free(g_kept);
    g_kept = rc == 0 ? st.kept : NULL;
    pthread_mutex_unlock(&g_kept_lock);
    if (rc != 0) kept_free(st.kept);
    if (rc == 0 && st.lost)
        vg_log("%s: %d password(s) cannot be read; kept sealed in the file until set again", g_path, st.lost);
    if (rc == 0 && st.plain) {
        /* Passwords in the clear (a file from before they were sealed, or
         * typed in by hand): sealed now, not at the next change. */
        char err[700];
        if (config_save_file(g_path, &c, err, sizeof err) < 0)
            vg_log("%s: %d password(s) in the clear, not sealed: %s", g_path, st.plain, err);
        else
            vg_log("%s: %d password(s) found in the clear, sealed", g_path, st.plain);
    }
    schema_errors_t perrs = { 0 };
    if (rc == 0) {
        if (config_check_ports(&c, &perrs) < 0) log_errors(g_path, &perrs);
    } else if (rc < 0) {
        log_errors(g_path, &errs);
        vg_log("%s is invalid; using the defaults (the file is kept)", g_path);
    }
    pthread_mutex_lock(&g_lock);
    g_cfg = c;
    g_cfg_init = 1;
    g_file_bad = rc < 0;
    ++g_gen;
    pthread_mutex_unlock(&g_lock);
    return rc < 0 ? -1 : 0;
}

void config_override(const vigil_config_t *c)
{
    pthread_mutex_lock(&g_lock);
    g_cfg = *c;
    g_cfg_init = 1;
    ++g_gen;
    pthread_mutex_unlock(&g_lock);
}

unsigned config_get(vigil_config_t *c)
{
    pthread_mutex_lock(&g_lock);
    if (!g_cfg_init) {
        config_defaults(&g_cfg);
        g_cfg_init = 1;
    }
    *c = g_cfg;
    unsigned gen = g_gen;
    pthread_mutex_unlock(&g_lock);
    return gen;
}

unsigned config_generation(void)
{
    pthread_mutex_lock(&g_lock);
    unsigned gen = g_gen;
    pthread_mutex_unlock(&g_lock);
    return gen;
}

void config_mark_applied(unsigned gen)
{
    pthread_mutex_lock(&g_lock);
    g_applied = gen;
    pthread_mutex_unlock(&g_lock);
}

unsigned config_applied(void)
{
    pthread_mutex_lock(&g_lock);
    unsigned gen = g_applied;
    pthread_mutex_unlock(&g_lock);
    return gen;
}

const char *config_path(void) { return g_path; }

static config_listen_check_fn g_listen_check;

void config_set_listen_check(config_listen_check_fn fn) { g_listen_check = fn; }

/* A changed api.listen, web.listen, onvif_server.port, rtsp_server.port or
 * netip_server.port (while that server is on) that cannot be listened on is refused here, so the API and the UI
 * say why at once instead of vigild finding out when it applies it. */
static int check_listens(const vigil_config_t *nc, const vigil_config_t *old, schema_errors_t *errs)
{
    if (!g_listen_check) return 0;
    char onvif_now[8] = "", onvif_was[8] = "", rtsp_now[8] = "", rtsp_was[8] = "";
    if (nc->onvif_server.enabled) snprintf(onvif_now, sizeof onvif_now, "%d", nc->onvif_server.port);
    if (old->onvif_server.enabled) snprintf(onvif_was, sizeof onvif_was, "%d", old->onvif_server.port);
    if (nc->rtsp_server.enabled) snprintf(rtsp_now, sizeof rtsp_now, "%d", nc->rtsp_server.port);
    if (old->rtsp_server.enabled) snprintf(rtsp_was, sizeof rtsp_was, "%d", old->rtsp_server.port);
    char netip_now[8] = "", netip_was[8] = "";
    if (nc->netip_server.enabled) snprintf(netip_now, sizeof netip_now, "%d", nc->netip_server.port);
    if (old->netip_server.enabled) snprintf(netip_was, sizeof netip_was, "%d", old->netip_server.port);
    const struct { const char *what, *path; const char *now, *was; } l[] = {
        { "api", "api.listen", nc->api.listen, old->api.listen },
        { "web", "web.listen", nc->web.listen, old->web.listen },
        { "onvif", "onvif_server.port", onvif_now, onvif_was },
        { "rtsp", "rtsp_server.port", rtsp_now, rtsp_was },
        { "netip", "netip_server.port", netip_now, netip_was },
    };
    int rc = 0;
    for (size_t i = 0; i < sizeof l / sizeof l[0]; ++i) {
        char err[128] = "";
        if (!strcmp(l[i].now, l[i].was) || !l[i].now[0]) continue;
        if (g_listen_check(l[i].what, l[i].now, err, sizeof err) < 0) {
            schema_error(errs, l[i].path, "%s", err[0] ? err : "cannot listen there");
            rc = -1;
        }
    }
    return rc;
}

/* A URL sent back exactly as GET showed it (password "***") means "keep
 * the stored one". Matched on the whole redacted URL, not the position,
 * so cameras can be reordered. */
static int resolve_secrets(vigil_config_t *nc, const vigil_config_t *old, schema_errors_t *errs)
{
    int rc = 0;
    for (int i = 0; i < nc->n_cameras; ++i) {
        char *url = nc->cameras[i].url, red[1024];
        schema_redact_url(url, red, sizeof red);
        if (strcmp(red, url) != 0 || !strstr(url, ":***@")) continue;   /* a real password */
        int found = 0;
        for (int k = 0; k < old->n_cameras && !found; ++k) {
            schema_redact_url(old->cameras[k].url, red, sizeof red);
            if (!strcmp(red, url)) {
                snprintf(url, sizeof nc->cameras[i].url, "%s", old->cameras[k].url);
                found = 1;
            }
        }
        if (!found) {
            char path[64];
            snprintf(path, sizeof path, "cameras[%d].url", i);
            schema_error(errs, path, "password \"***\" does not match a stored camera; send the password");
            rc = -1;
        }
    }
    return rc;
}

config_rc_t config_put_json(const char *section, const char *body, size_t len,
                            schema_errors_t *errs)
{
    return config_put_json_at(section, body, len, 0, errs);
}

config_rc_t config_put_json_at(const char *section, const char *body, size_t len, unsigned gen,
                               schema_errors_t *errs)
{
    const schema_section_t *sec = NULL;
    if (section && !(sec = schema_section(&config_schema, section))) return CONFIG_ENOENT;

    char *text = malloc(len + 1);
    if (!text) {
        schema_error(errs, "", "out of memory");
        return CONFIG_EIO;
    }
    memcpy(text, body, len);
    text[len] = 0;
    cJSON *doc = parse_json(text, len, errs);
    free(text);
    if (!doc) return CONFIG_EINVAL;

    pthread_mutex_lock(&g_write);
    vigil_config_t old, nc;
    unsigned cur = config_get(&old);
    if (gen && cur != gen) {
        pthread_mutex_unlock(&g_write);
        cJSON_Delete(doc);
        schema_error(errs, "", "the configuration changed meanwhile (generation %u, now %u)", gen, cur);
        return CONFIG_ESTALE;
    }
    int rc;
    if (sec) {
        nc = old;
        rc = schema_section_from_json(&config_schema, sec, doc, &nc, 1, errs);
    } else {
        config_defaults(&nc);
        rc = schema_from_json(&config_schema, doc, &nc, 1, errs);
    }
    cJSON_Delete(doc);
    /* What is sent must be UTF-8 (the file is YAML). Only what is sent:
     * a value loaded from an older .json file is not the client's to fix. */
    if (rc == 0) rc = schema_check_utf8(&config_schema, sec, &nc, errs);
    if (rc == 0) rc = resolve_secrets(&nc, &old, errs);
    if (rc == 0)
        for (int i = 0; i < config_schema.n_sections; ++i)
            if (!sec || sec == &config_schema.sections[i])
                schema_keep_secrets(&config_schema.sections[i], &nc, &old);
    if (rc == 0) rc = schema_validate(&config_schema, &nc, errs);
    if (rc == 0) rc = config_check_ports(&nc, errs);
    if (rc == 0) rc = check_listens(&nc, &old, errs);
    if (rc < 0) {
        pthread_mutex_unlock(&g_write);
        return CONFIG_EINVAL;
    }
    config_trim(&nc);

    char err[700];
    if (g_file_bad) {
        /* The file the user wrote failed to load; it is not replaced
         * unless a copy of it is kept. */
        char bad[300];
        snprintf(bad, sizeof bad, "%s.bad", g_path);
        if (keep_copy(g_path, bad, err, sizeof err) < 0) {
            pthread_mutex_unlock(&g_write);
            vg_log("config: %s; not saving over it", err);
            schema_error(errs, "", "%s; not saving over it", err);
            return CONFIG_EIO;
        }
        vg_log("the invalid %s is kept as %s", g_path, bad);
    }
    int saved = config_save_file(g_path, &nc, err, sizeof err);
    if (saved < 0) {
        pthread_mutex_unlock(&g_write);
        vg_log("config: %s", err);
        schema_error(errs, "", "%s", err);
        return CONFIG_EIO;
    }
    /* Saved, or at least renamed into place (saved == 1): the new file is
     * what the next start reads, so it is also what runs now. */
    pthread_mutex_lock(&g_lock);
    g_cfg = nc;
    g_file_bad = 0;
    ++g_gen;
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_unlock(&g_write);
    oplog_config_saved(section, &old, &nc);
    if (saved > 0) {
        vg_log("config: %s; it is in force, but may not survive a power cut", err);
        schema_error(errs, "", "%s; the configuration is in force, but may not survive a power cut", err);
        return CONFIG_EUNSYNCED;
    }
    vg_log("config: %s saved", section ? section : "configuration");
    return CONFIG_OK;
}

char *config_get_json(const char *section, int redact)
{
    const schema_section_t *sec = NULL;
    if (section && !(sec = schema_section(&config_schema, section))) return NULL;
    vigil_config_t *c = malloc(sizeof *c);
    if (!c) return NULL;
    config_get(c);
    cJSON *doc = sec ? schema_section_to_json(sec, c, redact) : schema_to_json(&config_schema, c, redact);
    free(c);
    char *text = doc ? cJSON_Print(doc) : NULL;
    cJSON_Delete(doc);
    return text;
}

char *config_schema_json(void)
{
    cJSON *doc = schema_describe(&config_schema);
    char *text = doc ? cJSON_Print(doc) : NULL;
    cJSON_Delete(doc);
    return text;
}
