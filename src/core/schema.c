/* SPDX-License-Identifier: GPL-3.0-or-later */
/* schema.c — the settings engine (see schema.h). */
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "schema.h"

void schema_error(schema_errors_t *errs, const char *path, const char *fmt, ...)
{
    if (!errs) return;
    ++errs->total;
    if (errs->n == SCHEMA_ERRORS_MAX) return;
    schema_error_t *e = &errs->e[errs->n++];
    snprintf(e->path, sizeof e->path, "%s", path);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof e->msg, fmt, ap);
    va_end(ap);
}

const schema_section_t *schema_section(const schema_t *s, const char *key)
{
    for (int i = 0; i < s->n_sections; ++i)
        if (!strcmp(s->sections[i].key, key)) return &s->sections[i];
    return NULL;
}

const schema_field_t *schema_field(const schema_section_t *sec, const char *key)
{
    for (int i = 0; i < sec->n_fields; ++i)
        if (!strcmp(sec->fields[i].key, key)) return &sec->fields[i];
    return NULL;
}

static char *str_at(void *base, const schema_field_t *f) { return (char *)base + f->off; }
static int  *int_at(void *base, const schema_field_t *f) { return (int *)(void *)((char *)base + f->off); }
static const char *cstr_at(const void *base, const schema_field_t *f) { return (const char *)base + f->off; }
static int  cint_at(const void *base, const schema_field_t *f)
{
    return *(const int *)(const void *)((const char *)base + f->off);
}

static void field_default(void *base, const schema_field_t *f)
{
    if (f->type == SCHEMA_STRING || f->type == SCHEMA_ENUM)
        snprintf(str_at(base, f), f->size, "%s", f->def_str ? f->def_str : "");
    else
        *int_at(base, f) = f->def_int;
}

static void fields_default(const schema_section_t *sec, void *base)
{
    for (int i = 0; i < sec->n_fields; ++i) field_default(base, &sec->fields[i]);
}

static void section_default(const schema_section_t *sec, void *root)
{
    if (sec->is_array) {
        memset((char *)root + sec->off, 0, sec->item_size * (size_t)sec->max_items);
        *(int *)(void *)((char *)root + sec->count_off) = 0;
    } else {
        fields_default(sec, (char *)root + sec->off);
    }
}

void schema_defaults(const schema_t *s, void *root)
{
    for (int i = 0; i < s->n_sections; ++i) section_default(&s->sections[i], root);
}

/* ---- checks shared by JSON input and schema_validate ------------------------ */

int schema_utf8_char(const char *s, unsigned *cp)
{
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) {
        *cp = u[0];
        return 1;
    }
    int n = u[0] >= 0xf0 ? 4 : u[0] >= 0xe0 ? 3 : u[0] >= 0xc2 ? 2 : 0;
    if (!n || u[0] > 0xf4) return 0;
    unsigned c = u[0] & (0x7fu >> n);
    for (int i = 1; i < n; ++i) {
        if ((u[i] & 0xc0) != 0x80) return 0;
        c = c << 6 | (u[i] & 0x3f);
    }
    static const unsigned min[] = { 0, 0, 0x80, 0x800, 0x10000 };
    if (c < min[n] || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return 0;
    *cp = c;
    return n;
}

static int check_string(const schema_field_t *f, const char *v, const char *path,
                        schema_errors_t *errs)
{
    for (const unsigned char *p = (const unsigned char *)v; *p; ++p)
        if (*p < 0x20 || *p == 0x7f) {
            schema_error(errs, path, "control characters are not allowed");
            return -1;
        }
    if (strlen(v) >= f->size) {
        schema_error(errs, path, "too long (at most %zu bytes)", f->size - 1);
        return -1;
    }
    if (f->required && !*v) {
        schema_error(errs, path, "must not be empty");
        return -1;
    }
    if (f->type == SCHEMA_ENUM) {
        for (const char *const *e = f->values; *e; ++e)
            if (!strcmp(*e, v)) return 0;
        char list[128] = "";
        for (const char *const *e = f->values; *e; ++e) {
            size_t l = strlen(list);
            snprintf(list + l, sizeof list - l, "%s%s", l ? ", " : "", *e);
        }
        schema_error(errs, path, "must be one of: %s", list);
        return -1;
    }
    if (f->check) {
        char msg[128] = "invalid value";
        if (f->check(v, msg, sizeof msg) < 0) {
            schema_error(errs, path, "%s", msg);
            return -1;
        }
    }
    return 0;
}

static int check_int(const schema_field_t *f, int v, const char *path, schema_errors_t *errs)
{
    if (f->type == SCHEMA_BOOL) {
        if (v == 0 || v == 1) return 0;
        schema_error(errs, path, "must be true or false");
        return -1;
    }
    if (v < f->min || v > f->max) {
        schema_error(errs, path, "%d is out of range %d..%d", v, f->min, f->max);
        return -1;
    }
    return 0;
}

static void field_path(char *out, size_t cap, const schema_section_t *sec, int item, const char *key)
{
    if (item >= 0) snprintf(out, cap, "%s[%d].%s", sec->key, item, key);
    else snprintf(out, cap, "%s.%s", sec->key, key);
}

/* ---- JSON -> C ------------------------------------------------------------------ */

static int field_from_json(const schema_field_t *f, const cJSON *v, void *base,
                           const char *path, schema_errors_t *errs)
{
    switch (f->type) {
    case SCHEMA_STRING:
    case SCHEMA_ENUM:
        if (!cJSON_IsString(v)) {
            schema_error(errs, path, "must be a string");
            return -1;
        }
        if (check_string(f, v->valuestring, path, errs) < 0) return -1;
        snprintf(str_at(base, f), f->size, "%s", v->valuestring);
        return 0;
    case SCHEMA_BOOL:
        if (!cJSON_IsBool(v)) {
            schema_error(errs, path, "must be true or false");
            return -1;
        }
        *int_at(base, f) = cJSON_IsTrue(v);
        return 0;
    case SCHEMA_INT: {
        double d = cJSON_IsNumber(v) ? v->valuedouble : 0.5;
        if (!cJSON_IsNumber(v) || !(d >= INT_MIN && d <= INT_MAX) || d != (double)(int)d) {
            schema_error(errs, path, "must be an integer");
            return -1;
        }
        if (check_int(f, (int)d, path, errs) < 0) return -1;
        *int_at(base, f) = (int)d;
        return 0;
    }
    }
    return -1;
}

/* One object: every key known, each value valid; absent keys get their
 * default (or are an error when required). */
static int object_from_json(const schema_section_t *sec, int item, const cJSON *obj,
                            void *base, int strict, schema_errors_t *errs)
{
    char path[64];
    int rc = 0;
    if (!cJSON_IsObject(obj)) {
        if (item >= 0) snprintf(path, sizeof path, "%s[%d]", sec->key, item);
        else snprintf(path, sizeof path, "%s", sec->key);
        schema_error(errs, path, "must be an object");
        return -1;
    }
    fields_default(sec, base);
    for (const cJSON *c = obj->child; c; c = c->next) {
        const schema_field_t *f = schema_field(sec, c->string);
        if (!f) {
            if (strict) {
                field_path(path, sizeof path, sec, item, c->string);
                schema_error(errs, path, "unknown setting");
                rc = -1;
            }
            continue;
        }
        field_path(path, sizeof path, sec, item, f->key);
        if (field_from_json(f, c, base, path, errs) < 0) rc = -1;
    }
    for (int i = 0; i < sec->n_fields; ++i) {
        const schema_field_t *f = &sec->fields[i];
        if (f->required && !cJSON_GetObjectItemCaseSensitive(obj, f->key)) {
            field_path(path, sizeof path, sec, item, f->key);
            schema_error(errs, path, "is required");
            rc = -1;
        }
    }
    return rc;
}

int schema_section_from_json(const schema_t *s, const schema_section_t *sec,
                             const cJSON *json, void *root, int strict,
                             schema_errors_t *errs)
{
    (void)s;
    if (!sec->is_array) return object_from_json(sec, -1, json, (char *)root + sec->off, strict, errs);

    if (!cJSON_IsArray(json)) {
        schema_error(errs, sec->key, "must be an array");
        return -1;
    }
    int n = cJSON_GetArraySize(json);
    if (n > sec->max_items) {
        schema_error(errs, sec->key, "at most %d entries", sec->max_items);
        return -1;
    }
    section_default(sec, root);
    int rc = 0, i = 0;
    for (const cJSON *c = json->child; c; c = c->next, ++i) {
        void *item = (char *)root + sec->off + sec->item_size * (size_t)i;
        if (object_from_json(sec, i, c, item, strict, errs) < 0) rc = -1;
    }
    *(int *)(void *)((char *)root + sec->count_off) = n;
    return rc;
}

int schema_from_json(const schema_t *s, const cJSON *doc, void *root, int strict,
                     schema_errors_t *errs)
{
    if (!cJSON_IsObject(doc)) {
        schema_error(errs, "", "the configuration must be a JSON object");
        return -1;
    }
    int rc = 0;
    for (const cJSON *c = doc->child; c; c = c->next) {
        if (!strcmp(c->string, "version")) {
            if (!cJSON_IsNumber(c) || c->valuedouble != s->version) {
                schema_error(errs, "version", "must be %d", s->version);
                rc = -1;
            }
            continue;
        }
        const schema_section_t *sec = schema_section(s, c->string);
        if (!sec) {
            if (strict) {
                schema_error(errs, c->string, "unknown section");
                rc = -1;
            }
            continue;
        }
        if (schema_section_from_json(s, sec, c, root, strict, errs) < 0) rc = -1;
    }
    return rc;
}

/* ---- checks on a struct ------------------------------------------------------------ */

static int object_validate(const schema_section_t *sec, int item, const void *base,
                           schema_errors_t *errs)
{
    char path[64];
    int rc = 0;
    for (int i = 0; i < sec->n_fields; ++i) {
        const schema_field_t *f = &sec->fields[i];
        field_path(path, sizeof path, sec, item, f->key);
        if (f->type == SCHEMA_STRING || f->type == SCHEMA_ENUM) {
            const char *v = cstr_at(base, f);
            if (!memchr(v, 0, f->size)) {
                schema_error(errs, path, "not terminated");
                rc = -1;
            } else if (check_string(f, v, path, errs) < 0) {
                rc = -1;
            }
        } else if (check_int(f, cint_at(base, f), path, errs) < 0) {
            rc = -1;
        }
    }
    return rc;
}

int schema_validate(const schema_t *s, const void *root, schema_errors_t *errs)
{
    int rc = 0;
    for (int i = 0; i < s->n_sections; ++i) {
        const schema_section_t *sec = &s->sections[i];
        if (!sec->is_array) {
            if (object_validate(sec, -1, (const char *)root + sec->off, errs) < 0) rc = -1;
            continue;
        }
        int n = *(const int *)(const void *)((const char *)root + sec->count_off);
        if (n < 0 || n > sec->max_items) {
            schema_error(errs, sec->key, "at most %d entries", sec->max_items);
            rc = -1;
            continue;
        }
        for (int k = 0; k < n; ++k)
            if (object_validate(sec, k, (const char *)root + sec->off + sec->item_size * (size_t)k, errs) < 0)
                rc = -1;
    }
    return rc;
}

static int object_utf8(const schema_section_t *sec, int item, const void *base, schema_errors_t *errs)
{
    char path[64];
    int rc = 0;
    for (int i = 0; i < sec->n_fields; ++i) {
        const schema_field_t *f = &sec->fields[i];
        if (f->type != SCHEMA_STRING && f->type != SCHEMA_ENUM) continue;
        const char *v = cstr_at(base, f);
        unsigned c;
        for (int n; *v; v += n)
            if (!(n = schema_utf8_char(v, &c))) {
                field_path(path, sizeof path, sec, item, f->key);
                schema_error(errs, path, "not valid UTF-8 text");
                rc = -1;
                break;
            }
    }
    return rc;
}

int schema_check_utf8(const schema_t *s, const schema_section_t *only, const void *root,
                      schema_errors_t *errs)
{
    int rc = 0;
    for (int i = 0; i < s->n_sections; ++i) {
        const schema_section_t *sec = &s->sections[i];
        if (only && only != sec) continue;
        if (!sec->is_array) {
            if (object_utf8(sec, -1, (const char *)root + sec->off, errs) < 0) rc = -1;
            continue;
        }
        int n = *(const int *)(const void *)((const char *)root + sec->count_off);
        for (int k = 0; k < n && k < sec->max_items; ++k)
            if (object_utf8(sec, k, (const char *)root + sec->off + sec->item_size * (size_t)k, errs) < 0)
                rc = -1;
    }
    return rc;
}

/* ---- C -> JSON ----------------------------------------------------------------------- */

void schema_redact_url(const char *url, char *out, size_t cap)
{
    const char *auth = strstr(url, "://");
    if (!auth) {
        snprintf(out, cap, "%s", url);
        return;
    }
    auth += 3;
    /* The last '@' anywhere: the drivers' parsers do not agree on where
     * the credentials end (RTSP takes '?' and '#' in a password, NETIP
     * stops at '?'), and a password holding '/', '?', '#' or '@' must not
     * leave its tail visible. A query with '@' in it is hidden as well. */
    const char *at = strrchr(auth, '@');
    const char *colon = at ? memchr(auth, ':', (size_t)(at - auth)) : NULL;
    if (!colon || colon + 1 == at) {                    /* no password, or an empty one */
        snprintf(out, cap, "%s", url);
        return;
    }
    snprintf(out, cap, "%.*s***%s", (int)(colon + 1 - url), url, at);
}

static cJSON *object_to_json(const schema_section_t *sec, const void *base, int redact)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    for (int i = 0; i < sec->n_fields; ++i) {
        const schema_field_t *f = &sec->fields[i];
        cJSON *v;
        if (f->type == SCHEMA_STRING || f->type == SCHEMA_ENUM) {
            const char *s = cstr_at(base, f);
            char red[1024];
            if (redact && f->secret == SCHEMA_SECRET_URL) {
                schema_redact_url(s, red, sizeof red);
                s = red;
            }
            v = cJSON_CreateString(s);
        } else if (f->type == SCHEMA_BOOL) {
            v = cJSON_CreateBool(cint_at(base, f));
        } else {
            v = cJSON_CreateNumber(cint_at(base, f));
        }
        if (!v || !cJSON_AddItemToObject(o, f->key, v)) {
            cJSON_Delete(v);
            cJSON_Delete(o);
            return NULL;
        }
    }
    return o;
}

cJSON *schema_section_to_json(const schema_section_t *sec, const void *root, int redact)
{
    if (!sec->is_array) return object_to_json(sec, (const char *)root + sec->off, redact);
    cJSON *a = cJSON_CreateArray();
    int n = *(const int *)(const void *)((const char *)root + sec->count_off);
    for (int k = 0; a && k < n && k < sec->max_items; ++k) {
        cJSON *o = object_to_json(sec, (const char *)root + sec->off + sec->item_size * (size_t)k, redact);
        if (!o || !cJSON_AddItemToArray(a, o)) {
            cJSON_Delete(o);
            cJSON_Delete(a);
            return NULL;
        }
    }
    return a;
}

cJSON *schema_to_json(const schema_t *s, const void *root, int redact)
{
    cJSON *doc = cJSON_CreateObject();
    if (!doc || !cJSON_AddNumberToObject(doc, "version", s->version)) goto fail;
    for (int i = 0; i < s->n_sections; ++i) {
        cJSON *v = schema_section_to_json(&s->sections[i], root, redact);
        if (!v || !cJSON_AddItemToObject(doc, s->sections[i].key, v)) {
            cJSON_Delete(v);
            goto fail;
        }
    }
    return doc;
fail:
    cJSON_Delete(doc);
    return NULL;
}

/* ---- the schema itself --------------------------------------------------------------- */

static const char *type_name(schema_type_t t)
{
    switch (t) {
    case SCHEMA_STRING: return "string";
    case SCHEMA_INT:    return "integer";
    case SCHEMA_BOOL:   return "boolean";
    case SCHEMA_ENUM:   return "enum";
    }
    return "?";
}

static cJSON *field_describe(const schema_section_t *sec, const schema_field_t *f)
{
    cJSON *o = cJSON_CreateObject();
    char path[64];
    if (!o) return NULL;
    snprintf(path, sizeof path, sec->is_array ? "%s[].%s" : "%s.%s", sec->key, f->key);
    cJSON_AddStringToObject(o, "key", f->key);
    cJSON_AddStringToObject(o, "path", path);
    cJSON_AddStringToObject(o, "type", type_name(f->type));
    cJSON_AddStringToObject(o, "label", f->label ? f->label : f->key);
    cJSON_AddStringToObject(o, "help", f->help ? f->help : "");
    cJSON_AddBoolToObject(o, "live", f->live);
    cJSON_AddBoolToObject(o, "required", f->required);
    switch (f->type) {
    case SCHEMA_STRING:
        cJSON_AddStringToObject(o, "default", f->def_str ? f->def_str : "");
        cJSON_AddNumberToObject(o, "max_length", (double)(f->size - 1));
        if (f->secret == SCHEMA_SECRET_URL) cJSON_AddStringToObject(o, "secret", "url-password");
        break;
    case SCHEMA_ENUM: {
        cJSON_AddStringToObject(o, "default", f->def_str ? f->def_str : "");
        cJSON *vals = cJSON_AddArrayToObject(o, "values");
        for (const char *const *e = f->values; vals && *e; ++e)
            cJSON_AddItemToArray(vals, cJSON_CreateString(*e));
        break;
    }
    case SCHEMA_BOOL:
        cJSON_AddBoolToObject(o, "default", f->def_int);
        break;
    case SCHEMA_INT:
        cJSON_AddNumberToObject(o, "default", f->def_int);
        cJSON_AddNumberToObject(o, "min", f->min);
        cJSON_AddNumberToObject(o, "max", f->max);
        break;
    }
    return o;
}

cJSON *schema_describe(const schema_t *s)
{
    cJSON *doc = cJSON_CreateObject();
    if (!doc) return NULL;
    cJSON_AddNumberToObject(doc, "version", s->version);
    cJSON *secs = cJSON_AddArrayToObject(doc, "sections");
    for (int i = 0; secs && i < s->n_sections; ++i) {
        const schema_section_t *sec = &s->sections[i];
        cJSON *o = cJSON_CreateObject();
        if (!o) break;
        cJSON_AddStringToObject(o, "key", sec->key);
        cJSON_AddStringToObject(o, "type", sec->is_array ? "array" : "object");
        cJSON_AddStringToObject(o, "label", sec->label ? sec->label : sec->key);
        cJSON_AddStringToObject(o, "help", sec->help ? sec->help : "");
        if (sec->is_array) cJSON_AddNumberToObject(o, "max_items", sec->max_items);
        cJSON *fields = cJSON_AddArrayToObject(o, "fields");
        for (int k = 0; fields && k < sec->n_fields; ++k)
            cJSON_AddItemToArray(fields, field_describe(sec, &sec->fields[k]));
        cJSON_AddItemToArray(secs, o);
    }
    if (!secs) {
        cJSON_Delete(doc);
        return NULL;
    }
    return doc;
}
