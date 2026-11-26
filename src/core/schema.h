/* SPDX-License-Identifier: GPL-3.0-or-later */
/* schema.h — settings described as data.
 *
 * A schema is a table: sections (a JSON object, or an array of objects such
 * as the camera list), each with fields that say where the value lives in
 * a C struct, its type, default, limits, a label and help text, and whether
 * a change applies at run time. One generic engine (schema.c) then does
 * every job from that one table, so a new setting is one row and one
 * struct member:
 *
 *   defaults      schema_defaults()
 *   JSON -> C     schema_from_json(), schema_section_from_json(): type and
 *                 limit checks, errors as {path, message} pairs
 *   checks        schema_validate(): the same limits on a struct built in C
 *                 (command-line overrides)
 *   C -> JSON     schema_to_json(), with secret fields redacted
 *   describe      schema_describe(): the table itself as JSON, so a UI can
 *                 render a form for every setting without knowing them
 *
 * JSON goes through cJSON (third_party/cJSON); this file and the callers
 * see only cJSON pointers.
 */
#ifndef SCHEMA_H
#define SCHEMA_H

#include <stddef.h>

typedef struct cJSON cJSON;

typedef enum {
    SCHEMA_STRING,          /* char[size] */
    SCHEMA_INT,             /* int, min..max */
    SCHEMA_BOOL,            /* int, 0/1 */
    SCHEMA_ENUM,            /* char[size], one of `values` */
} schema_type_t;

/* How a string field is shown outside: SCHEMA_SECRET_URL replaces the
 * password in "scheme://user:pass@host" with "***" (schema_redact_url). */
enum { SCHEMA_PLAIN = 0, SCHEMA_SECRET_URL = 1 };

/* Extra check for a string value. Returns 0, or -1 with `msg` set. */
typedef int (*schema_check_fn)(const char *value, char *msg, size_t cap);

typedef struct {
    const char         *key;
    schema_type_t       type;
    size_t              off;        /* in the section struct / array item */
    size_t              size;       /* char[] fields: buffer size */
    const char         *def_str;    /* STRING / ENUM default */
    int                 def_int;    /* INT / BOOL default */
    int                 min, max;   /* INT limits */
    const char *const  *values;     /* ENUM: NULL-terminated */
    int                 required;   /* must be present (array items) */
    int                 secret;     /* SCHEMA_PLAIN / SCHEMA_SECRET_URL */
    int                 live;       /* applied without restarting vigild */
    schema_check_fn     check;
    const char         *label;
    const char         *help;
} schema_field_t;

typedef struct {
    const char           *key;
    int                   is_array;     /* array of objects */
    size_t                off;          /* the struct (or the array) in the root */
    size_t                count_off;    /* arrays: int item count in the root */
    size_t                item_size;    /* arrays: one item */
    int                   max_items;
    const schema_field_t *fields;
    int                   n_fields;
    const char           *label;
    const char           *help;
} schema_section_t;

typedef struct {
    int                     version;    /* the document's "version" */
    const schema_section_t *sections;
    int                     n_sections;
} schema_t;

/* ---- errors ---------------------------------------------------------------- */

#define SCHEMA_ERRORS_MAX 16

typedef struct {
    char path[64];                  /* "record.segment", "cameras[1].url", "" */
    char msg[128];
} schema_error_t;

typedef struct {
    int            n;               /* stored (at most SCHEMA_ERRORS_MAX) */
    int            total;           /* found */
    schema_error_t e[SCHEMA_ERRORS_MAX];
} schema_errors_t;

void schema_error(schema_errors_t *errs, const char *path, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* ---- the engine ---------------------------------------------------------------- */

const schema_section_t *schema_section(const schema_t *s, const char *key);
const schema_field_t   *schema_field(const schema_section_t *sec, const char *key);

/* Every field of `root` to its default; arrays empty. */
void schema_defaults(const schema_t *s, void *root);

/* Fill `root` from a whole document. Sections present in `doc` replace the
 * ones in `root` (fields they omit get their defaults); absent sections
 * keep what `root` holds. `strict`: unknown keys are errors (API input);
 * otherwise they are ignored, so an older vigild still reads a newer file.
 * Returns 0, or -1 with `errs` filled; `root` is then undefined. */
int schema_from_json(const schema_t *s, const cJSON *doc, void *root, int strict,
                     schema_errors_t *errs);

/* The same for one section; `json` is that section's value. */
int schema_section_from_json(const schema_t *s, const schema_section_t *sec,
                             const cJSON *json, void *root, int strict,
                             schema_errors_t *errs);

/* Check limits on a struct (e.g. one changed from C). Returns 0 or -1. */
int schema_validate(const schema_t *s, const void *root, schema_errors_t *errs);

/* `root` as JSON ({"version": N, sections...}); `redact` hides secrets. */
cJSON *schema_to_json(const schema_t *s, const void *root, int redact);
cJSON *schema_section_to_json(const schema_section_t *sec, const void *root, int redact);

/* The schema as JSON, for UIs (see docs/api.md). */
cJSON *schema_describe(const schema_t *s);

/* "scheme://user:pass@host/..." -> "scheme://user:***@host/...". URLs
 * without a password, or with an empty one, are copied as they are. Errs
 * on the side of hiding too much: everything up to the last '@' before the
 * query counts as the credentials. */
void schema_redact_url(const char *url, char *out, size_t cap);

#endif /* SCHEMA_H */
