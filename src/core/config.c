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
#include "schema_yaml.h"

#define FILE_MAX (256u << 10)       /* far above any real configuration */

void config_defaults(vigil_config_t *c)
{
    memset(c, 0, sizeof *c);
    schema_defaults(&config_schema, c);
}

void config_to_channels(const vigil_config_t *c, channels_config_t *cc)
{
    memset(cc, 0, sizeof *cc);
    cc->n = c->n_cameras;
    for (int i = 0; i < c->n_cameras && i < CHANNEL_MAX; ++i) {
        cc->urls[i] = c->cameras[i].enabled ? c->cameras[i].url : NULL;
        cc->names[i] = c->cameras[i].name;
    }
    cc->rec_dir = c->record.path[0] ? c->record.path : NULL;
    cc->seg_secs = c->record.segment;
    cc->keep = c->record.keep;
    cc->allow_rootfs = c->record.allow_rootfs;
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

int config_load_file(const char *path, vigil_config_t *c, schema_errors_t *errs)
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

char *config_file_text(const vigil_config_t *c, config_format_t fmt)
{
    cJSON *doc = schema_to_json(&config_schema, c, 0);
    char *text = NULL;
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
    cJSON_Delete(doc);
    return text;
}

int config_save_file(const char *path, const vigil_config_t *c, char *err, size_t cap)
{
    if (config_check_text(c, config_format(path), err, cap) < 0) return -1;
    char *text = config_file_text(c, config_format(path));
    if (!text) {
        snprintf(err, cap, "out of memory");
        return -1;
    }
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
    int rc = config_load_file(g_path, &c, &errs);
    if (rc == 1) vg_log("%s: not there, using the defaults", g_path);
    else if (rc == 0) vg_log("%s: loaded", g_path);
    else {
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
    if (rc == 0) rc = schema_validate(&config_schema, &nc, errs);
    if (rc < 0) {
        pthread_mutex_unlock(&g_write);
        return CONFIG_EINVAL;
    }

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
