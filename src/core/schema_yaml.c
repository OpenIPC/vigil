/* SPDX-License-Identifier: GPL-3.0-or-later */
/* schema_yaml.c — YAML in and out for a schema's document (see schema_yaml.h). */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <yaml.h>

#include "cJSON.h"
#include "schema_yaml.h"

/* ---- YAML -> cJSON ----------------------------------------------------------- */

/* Where a node sits, which says how to type a scalar there. */
typedef enum { AT_ROOT, AT_VERSION, AT_SECTION, AT_ITEM, AT_FIELD, AT_OTHER } at_t;

typedef struct {
    at_t                    at;
    const schema_section_t *sec;    /* AT_SECTION, AT_ITEM, AT_FIELD */
    const schema_field_t   *f;      /* AT_FIELD */
} where_t;

typedef struct {
    yaml_parser_t    p;
    const schema_t  *s;
    schema_errors_t *errs;
    int              nodes;
} parse_t;

static int next_event(parse_t *y, yaml_event_t *ev)
{
    if (yaml_parser_parse(&y->p, ev)) return 0;
    if (y->p.error == YAML_MEMORY_ERROR) {
        schema_error(y->errs, "", "out of memory");
    } else if (y->p.error == YAML_READER_ERROR) {      /* the encoding: no line yet */
        schema_error(y->errs, "", "not valid YAML: %s (at byte %lu)",
                     y->p.problem ? y->p.problem : "unreadable text", (unsigned long)y->p.problem_offset);
    } else {
        const yaml_mark_t *m = &y->p.problem_mark;
        schema_error(y->errs, "", "not valid YAML: %s%s%s (line %lu, column %lu)",
                     y->p.context ? y->p.context : "", y->p.context ? ": " : "",
                     y->p.problem ? y->p.problem : "syntax error",
                     (unsigned long)m->line + 1, (unsigned long)m->column + 1);
    }
    return -1;
}

static void child_path(char *out, size_t cap, const char *path, const char *key)
{
    snprintf(out, cap, "%s%s%s", path, *path ? "." : "", key);
}

static int is_null(const yaml_event_t *ev)
{
    if (ev->data.scalar.style != YAML_PLAIN_SCALAR_STYLE) return 0;
    const char *t = (const char *)ev->data.scalar.value;
    return !*t || !strcmp(t, "~") || !strcmp(t, "null") || !strcmp(t, "Null") || !strcmp(t, "NULL");
}

/* The spellings majestic's users write. -1: not a boolean. */
static int bool_word(const char *t)
{
    static const char *const yes[] = { "true", "yes", "on" }, *const no[] = { "false", "no", "off" };
    for (int i = 0; i < 3; ++i) {
        if (!strcasecmp(t, yes[i])) return 1;
        if (!strcasecmp(t, no[i])) return 0;
    }
    return -1;
}

/* A decimal integer that fits an int. A leading 0 (YAML 1.1's octal) is
 * refused rather than guessed. */
static int int_word(const char *t, int *out)
{
    const char *d = t + (*t == '-' || *t == '+');
    if (*d < '0' || *d > '9' || (d[0] == '0' && d[1])) return 0;
    for (const char *p = d; *p; ++p)
        if (*p < '0' || *p > '9') return 0;
    errno = 0;
    long v = strtol(t, NULL, 10);
    if (errno || v < INT_MIN || v > INT_MAX) return 0;
    *out = (int)v;
    return 1;
}

/* A scalar typed by the setting it is the value of. One that does not
 * convert stays text, and schema_from_json() reports it as the wrong type,
 * with its path. */
static cJSON *scalar(const yaml_event_t *ev, where_t w)
{
    const char *t = (const char *)ev->data.scalar.value;
    int null = is_null(ev), n;
    switch (w.at) {
    case AT_SECTION:                    /* "cameras:" with nothing after it */
        if (null) return w.sec->is_array ? cJSON_CreateArray() : cJSON_CreateObject();
        break;
    case AT_ITEM:                       /* "- rtsp://...": the item's first field */
        if (!null && w.sec->n_fields > 0) {
            where_t fw = { AT_FIELD, w.sec, &w.sec->fields[0] };
            cJSON *obj = cJSON_CreateObject(), *v = scalar(ev, fw);
            if (obj && v && cJSON_AddItemToObject(obj, fw.f->key, v)) return obj;
            cJSON_Delete(v);
            cJSON_Delete(obj);
            return NULL;
        }
        break;
    case AT_VERSION:
        if (int_word(t, &n)) return cJSON_CreateNumber(n);
        break;
    case AT_FIELD:
        switch (w.f->type) {
        case SCHEMA_STRING:
        case SCHEMA_ENUM:
            return cJSON_CreateString(null ? "" : t);
        case SCHEMA_BOOL:
            if ((n = bool_word(t)) >= 0) return cJSON_CreateBool(n);
            break;
        case SCHEMA_INT:
            if (int_word(t, &n)) return cJSON_CreateNumber(n);
            break;
        }
        break;
    default:
        break;
    }
    return null ? cJSON_CreateNull() : cJSON_CreateString(t);
}

static cJSON *node(parse_t *y, yaml_event_t *ev, where_t w, const char *path, int depth);

static int check_text(parse_t *y, const yaml_event_t *ev, const char *path)
{
    /* "\0" in a double-quoted scalar: C strings would cut the value short. */
    if (memchr(ev->data.scalar.value, 0, ev->data.scalar.length)) {
        schema_error(y->errs, path, "a NUL character is not allowed");
        return -1;
    }
    return 0;
}

static cJSON *mapping(parse_t *y, where_t w, const char *path, int depth)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        schema_error(y->errs, path, "out of memory");
        return NULL;
    }
    for (;;) {
        yaml_event_t ev;
        if (next_event(y, &ev) < 0) goto fail;
        if (ev.type == YAML_MAPPING_END_EVENT) {
            yaml_event_delete(&ev);
            return obj;
        }
        if (ev.type != YAML_SCALAR_EVENT || check_text(y, &ev, path) < 0) {
            if (ev.type != YAML_SCALAR_EVENT)
                schema_error(y->errs, path, "a key must be plain text (line %lu)",
                             (unsigned long)ev.start_mark.line + 1);
            yaml_event_delete(&ev);
            goto fail;
        }
        /* The key's event lives until its value is added under it. */
        yaml_event_t kev = ev;
        const char *key = (const char *)kev.data.scalar.value;
        char sub[64];
        child_path(sub, sizeof sub, path, key);
        if (cJSON_GetObjectItemCaseSensitive(obj, key)) {
            schema_error(y->errs, sub, "appears twice (line %lu)", (unsigned long)kev.start_mark.line + 1);
            yaml_event_delete(&kev);
            goto fail;
        }

        where_t cw = { AT_OTHER, NULL, NULL };
        if (w.at == AT_ROOT) {
            if (!strcmp(key, "version")) cw.at = AT_VERSION;
            else if ((cw.sec = schema_section(y->s, key)) != NULL) cw.at = AT_SECTION;
        } else if ((w.at == AT_SECTION && !w.sec->is_array) || w.at == AT_ITEM) {
            if ((cw.f = schema_field(w.sec, key)) != NULL) {
                cw.at = AT_FIELD;
                cw.sec = w.sec;
            }
        }
        cJSON *v = next_event(y, &ev) < 0 ? NULL : node(y, &ev, cw, sub, depth);
        int added = v && cJSON_AddItemToObject(obj, key, v);
        yaml_event_delete(&kev);
        if (!added) {
            if (v) schema_error(y->errs, sub, "out of memory");
            cJSON_Delete(v);
            goto fail;
        }
    }
fail:
    cJSON_Delete(obj);
    return NULL;
}

static cJSON *sequence(parse_t *y, where_t w, const char *path, int depth)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        schema_error(y->errs, path, "out of memory");
        return NULL;
    }
    where_t cw = { AT_OTHER, NULL, NULL };
    if (w.at == AT_SECTION && w.sec->is_array) {
        cw.at = AT_ITEM;
        cw.sec = w.sec;
    }
    for (int i = 0;; ++i) {
        yaml_event_t ev;
        char sub[64];
        if (next_event(y, &ev) < 0) goto fail;
        if (ev.type == YAML_SEQUENCE_END_EVENT) {
            yaml_event_delete(&ev);
            return arr;
        }
        snprintf(sub, sizeof sub, "%s[%d]", path, i);
        cJSON *v = node(y, &ev, cw, sub, depth);
        if (!v) goto fail;
        if (!cJSON_AddItemToArray(arr, v)) {
            cJSON_Delete(v);
            schema_error(y->errs, sub, "out of memory");
            goto fail;
        }
    }
fail:
    cJSON_Delete(arr);
    return NULL;
}

/* The node that starts with `ev` (consumed here). */
static cJSON *node(parse_t *y, yaml_event_t *ev, where_t w, const char *path, int depth)
{
    yaml_event_type_t type = ev->type;
    unsigned long line = (unsigned long)ev->start_mark.line + 1;
    if (++y->nodes > SCHEMA_YAML_MAX_NODES) {
        schema_error(y->errs, path, "too many entries (more than %d, line %lu)", SCHEMA_YAML_MAX_NODES, line);
        yaml_event_delete(ev);
        return NULL;
    }
    if (type == YAML_SCALAR_EVENT) {
        cJSON *v = NULL;
        if (check_text(y, ev, path) == 0 && !(v = scalar(ev, w))) schema_error(y->errs, path, "out of memory");
        yaml_event_delete(ev);
        return v;
    }
    if (type == YAML_ALIAS_EVENT) {
        /* An alias can repeat a node any number of times: a small file
         * would become a huge document ("billion laughs"). */
        schema_error(y->errs, path, "aliases (*%.32s) are not supported (line %lu)",
                     (const char *)ev->data.alias.anchor, line);
        yaml_event_delete(ev);
        return NULL;
    }
    yaml_event_delete(ev);
    if (type != YAML_MAPPING_START_EVENT && type != YAML_SEQUENCE_START_EVENT) {
        schema_error(y->errs, path, "unexpected YAML (line %lu)", line);
        return NULL;
    }
    if (depth >= SCHEMA_YAML_MAX_DEPTH) {
        schema_error(y->errs, path, "nested too deeply (line %lu)", line);
        return NULL;
    }
    return type == YAML_MAPPING_START_EVENT ? mapping(y, w, path, depth + 1)
                                            : sequence(y, w, path, depth + 1);
}

cJSON *schema_yaml_parse(const schema_t *s, const char *text, size_t len, schema_errors_t *errs)
{
    parse_t y = { .s = s, .errs = errs };
    yaml_event_t ev;
    cJSON *doc = NULL;
    if (!yaml_parser_initialize(&y.p)) {
        schema_error(errs, "", "out of memory");
        return NULL;
    }
    yaml_parser_set_input_string(&y.p, (const unsigned char *)text, len);
    yaml_parser_set_encoding(&y.p, YAML_UTF8_ENCODING);

    if (next_event(&y, &ev) < 0) goto out;              /* STREAM-START */
    yaml_event_delete(&ev);
    if (next_event(&y, &ev) < 0) goto out;
    if (ev.type == YAML_STREAM_END_EVENT) {             /* nothing but comments */
        yaml_event_delete(&ev);
        doc = cJSON_CreateObject();
        if (!doc) schema_error(errs, "", "out of memory");
        goto out;
    }
    yaml_event_delete(&ev);                             /* DOCUMENT-START */
    if (next_event(&y, &ev) < 0) goto out;
    where_t root = { AT_ROOT, NULL, NULL };
    int empty = ev.type == YAML_SCALAR_EVENT && is_null(&ev);
    if (!(doc = node(&y, &ev, root, "", 0))) goto out;
    if (empty) {
        cJSON_Delete(doc);
        doc = cJSON_CreateObject();
    } else if (!cJSON_IsObject(doc)) {
        schema_error(errs, "", "the configuration must be a mapping of sections (key: value)");
        goto fail;
    }
    if (next_event(&y, &ev) < 0) goto fail;             /* DOCUMENT-END */
    yaml_event_delete(&ev);
    if (next_event(&y, &ev) < 0) goto fail;
    int more = ev.type != YAML_STREAM_END_EVENT;
    unsigned long line = (unsigned long)ev.start_mark.line + 1;
    yaml_event_delete(&ev);
    if (more) {
        schema_error(errs, "", "only one YAML document is allowed (another starts at line %lu)", line);
        goto fail;
    }
    goto out;
fail:
    cJSON_Delete(doc);
    doc = NULL;
out:
    yaml_parser_delete(&y.p);
    return doc;
}

/* ---- cJSON -> YAML ----------------------------------------------------------- */

#define WIDTH 78                    /* comment lines are wrapped here */

typedef struct {
    char  *b;
    size_t n, cap;
    int    oom;
} out_t;

static void put(out_t *o, const char *s, size_t n)
{
    if (o->oom) return;
    if (o->n + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap : 4096;
        while (cap < o->n + n + 1) cap *= 2;
        char *b = realloc(o->b, cap);
        if (!b) {
            o->oom = 1;
            return;
        }
        o->b = b;
        o->cap = cap;
    }
    memcpy(o->b + o->n, s, n);
    o->n += n;
    o->b[o->n] = 0;
}

static void puts_(out_t *o, const char *s) { put(o, s, strlen(s)); }

static void putf(out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void putf(out_t *o, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) put(o, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

static void spaces(out_t *o, int n)
{
    while (n-- > 0) put(o, " ", 1);
}

/* `text` as comment lines at `indent`, wrapped at WIDTH: the first line
 * starts with `lead`, the others with `cont`. */
static void comment(out_t *o, int indent, const char *lead, const char *cont, const char *text)
{
    const char *p = text;
    int first = 1;
    while (*p == ' ') ++p;
    while (*p || first) {
        const char *pre = first ? lead : cont;
        int col = indent + 2 + (int)strlen(pre);
        spaces(o, indent);
        puts_(o, "# ");
        puts_(o, pre);
        int words = 0;
        while (*p) {
            size_t w = strcspn(p, " \n");
            if (words && col + 1 + (int)w > WIDTH) break;
            if (words) put(o, " ", 1);
            put(o, p, w);
            col += (int)w + !!words;
            ++words;
            p += w;
            while (*p == ' ' || *p == '\n') ++p;
        }
        put(o, "\n", 1);
        first = 0;
    }
}

/* "Label: help. Limits." for a setting ("Label. help. Limits." after a
 * key in a list: `sep`); "Label: help." or "Label" for a section. */
static void describe(char *out, size_t cap, const char *label, const char *sep, const char *help,
                     const schema_field_t *f)
{
    size_t n = (size_t)snprintf(out, cap, "%s", label);
    if (help && *help && n < cap) n += (size_t)snprintf(out + n, cap - n, "%s%s", sep, help);
    if ((f || (help && *help)) && n < cap && n && out[n - 1] != '.')
        n += (size_t)snprintf(out + n, cap - n, ".");
    if (!f || n >= cap) return;
    switch (f->type) {
    case SCHEMA_STRING:
        if (f->required) snprintf(out + n, cap - n, " Required.");
        else if (f->def_str && *f->def_str) snprintf(out + n, cap - n, " Default %s.", f->def_str);
        break;
    case SCHEMA_ENUM:
        n += (size_t)snprintf(out + n, cap - n, " One of:");
        for (const char *const *e = f->values; *e && n < cap; ++e)
            n += (size_t)snprintf(out + n, cap - n, " %s%s", *e, e[1] ? "," : ".");
        if (f->def_str && n < cap) snprintf(out + n, cap - n, " Default %s.", f->def_str);
        break;
    case SCHEMA_BOOL:
        snprintf(out + n, cap - n, " Default %s.", f->def_int ? "true" : "false");
        break;
    case SCHEMA_INT:
        snprintf(out + n, cap - n, " %d..%d, default %d.", f->min, f->max, f->def_int);
        break;
    }
}

/* What YAML lets appear in a file as it is (YAML 1.2, 5.1). */
static int printable(unsigned c)
{
    return c == 0x9 || c == 0xa || c == 0xd || (c >= 0x20 && c <= 0x7e) || c == 0x85 ||
           (c >= 0xa0 && c <= 0xd7ff) || (c >= 0xe000 && c <= 0xfffd) || (c >= 0x10000 && c <= 0x10ffff);
}

/* Written without quotes only when nothing could read it differently:
 * letters, digits, space inside, "_./=+-", and printable non-ASCII text;
 * not starting like a number or an indicator, not a YAML keyword. A URL
 * (":", "?", "#", "&", "@") is always quoted. */
static int plain_ok(const char *s)
{
    size_t len = strlen(s);
    if (!len || s[0] == ' ' || s[len - 1] == ' ' || strchr("+-.0123456789", s[0])) return 0;
    static const char *const words[] = { "true", "false", "yes", "no", "on", "off", "y", "n", "null" };
    for (size_t i = 0; i < sizeof words / sizeof words[0]; ++i)
        if (!strcasecmp(s, words[i])) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        unsigned c;
        int n = schema_utf8_char((const char *)p, &c);
        if (!n) return 0;
        if (c < 0x80 ? !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                         strchr(" _./=+-", (int)c))
                     /* NEL, LS, PS end a line in YAML 1.1; the BOM is a BOM */
                     : !printable(c) || c == 0x85 || c == 0x2028 || c == 0x2029 || c == 0xfeff)
            return 0;
        p += n;
    }
    return 1;
}

static void scalar_out(out_t *o, const char *s)
{
    if (plain_ok(s)) {
        puts_(o, s);
        return;
    }
    put(o, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        unsigned c;
        int n = schema_utf8_char((const char *)p, &c);
        if (!n) {                       /* not UTF-8: the byte as it would read back */
            putf(o, "\\x%02x", *p++);
            continue;
        }
        if (c == '"' || c == '\\') {
            put(o, "\\", 1);
            put(o, (const char *)p, 1);
        } else if (c != 0x9 && c != 0x20 && (!printable(c) || c < 0x20 || c == 0x85 || c == 0x2028 ||
                                             c == 0x2029 || c == 0xfeff)) {
            if (c <= 0xff) putf(o, "\\x%02x", c);
            else if (c <= 0xffff) putf(o, "\\u%04x", c);
            else putf(o, "\\U%08x", c);
        } else {
            put(o, (const char *)p, (size_t)n);
        }
        p += n;
    }
    put(o, "\"", 1);
}

static void value_out(out_t *o, const cJSON *v)
{
    if (cJSON_IsString(v)) scalar_out(o, v->valuestring);
    else if (cJSON_IsBool(v)) puts_(o, cJSON_IsTrue(v) ? "true" : "false");
    else if (cJSON_IsNumber(v) && v->valuedouble == (double)v->valueint) putf(o, "%d", v->valueint);
    else if (cJSON_IsNumber(v)) putf(o, "%.17g", v->valuedouble);
    else puts_(o, "null");
}

/* The fields of one object. `item`: the first line starts with "- " (an
 * item of a list), and fields are described in the list's comment. */
static void fields_out(out_t *o, const schema_section_t *sec, const cJSON *obj, int indent, int item)
{
    char text[1024];
    int first = 1;
    for (int i = 0; i < sec->n_fields; ++i) {
        const schema_field_t *f = &sec->fields[i];
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, f->key);
        if (!v) continue;
        if (!item) {
            describe(text, sizeof text, f->label ? f->label : f->key, ": ", f->help, f);
            comment(o, indent, "", "", text);
        }
        if (item && first) {
            spaces(o, indent - 2);
            puts_(o, "- ");
        } else {
            spaces(o, indent);
        }
        first = 0;
        putf(o, "%s: ", f->key);
        value_out(o, v);
        put(o, "\n", 1);
    }
    if (item && first) {                /* nothing known in it */
        spaces(o, indent - 2);
        puts_(o, "- {}\n");
    }
}

char *schema_yaml_emit(const schema_t *s, const cJSON *doc, const char *header)
{
    out_t o = { 0 };
    char text[1024];
    if (header) {
        for (const char *p = header; *p;) {
            size_t n = strcspn(p, "\n");
            puts_(&o, n ? "# " : "#");
            put(&o, p, n);
            put(&o, "\n", 1);
            p += n + (p[n] == '\n');
        }
        put(&o, "\n", 1);
    }
    comment(&o, 0, "", "", "The format of this file.");
    putf(&o, "version: %d\n", s->version);

    for (int i = 0; i < s->n_sections; ++i) {
        const schema_section_t *sec = &s->sections[i];
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(doc, sec->key);
        put(&o, "\n", 1);
        describe(text, sizeof text, sec->label ? sec->label : sec->key, ": ", sec->help, NULL);
        if (sec->is_array) {
            size_t n = strlen(text);
            snprintf(text + n, sizeof text - n, "%s At most %d. Each one:", n && text[n - 1] == '.' ? "" : ".",
                     sec->max_items);
        }
        comment(&o, 0, "", "", text);
        if (sec->is_array) {
            for (int k = 0; k < sec->n_fields; ++k) {
                const schema_field_t *f = &sec->fields[k];
                char lead[64];
                describe(text, sizeof text, f->label ? f->label : f->key, ". ", f->help, f);
                snprintf(lead, sizeof lead, "  %s: ", f->key);
                comment(&o, 0, lead, "    ", text);
            }
            if (!cJSON_IsArray(v) || !v->child) {
                putf(&o, "%s: []\n", sec->key);
                continue;
            }
            putf(&o, "%s:\n", sec->key);
            for (const cJSON *it = v->child; it; it = it->next) fields_out(&o, sec, it, 4, 1);
        } else {
            putf(&o, "%s:\n", sec->key);
            fields_out(&o, sec, v, 2, 0);
        }
    }
    if (o.oom) {
        free(o.b);
        return NULL;
    }
    return o.b;
}
