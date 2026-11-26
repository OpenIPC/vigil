/* SPDX-License-Identifier: GPL-3.0-or-later */
/* schema_yaml.h — a schema's document as a YAML file.
 *
 * The configuration file is YAML, like majestic's; everything else (the
 * API, validation, redaction) works on the cJSON document of schema.h. This
 * is the bridge, in both directions:
 *
 *   schema_yaml_parse()  YAML text -> the cJSON document schema_from_json()
 *                        takes. YAML scalars are text; each one is typed by
 *                        the schema field it is the value of (booleans as
 *                        true/false, yes/no, on/off; integers in decimal),
 *                        and one that does not convert stays a string, so
 *                        schema_from_json() reports it with its path.
 *   schema_yaml_emit()   a cJSON document (schema_to_json) -> YAML text,
 *                        each section and setting preceded by its label and
 *                        help text as comments, so a saved file documents
 *                        itself. Comments are generated: those written by
 *                        hand are not kept.
 *
 * The parser (libyaml, MIT) takes maps, sequences and scalars only: aliases
 * are refused (no "billion laughs"), nesting and the number of values are
 * limited, a key may appear once, and a file holds one document. Tags and
 * anchors are ignored.
 */
#ifndef SCHEMA_YAML_H
#define SCHEMA_YAML_H

#include <stddef.h>

#include "schema.h"

/* Nesting deeper than this is refused (the schema needs 3: the document,
 * a list of cameras, a camera). */
#define SCHEMA_YAML_MAX_DEPTH 8

/* So is a document of more values than this (vigild's own, 16 cameras
 * and all, has under 100): keys are looked up one by one. */
#define SCHEMA_YAML_MAX_NODES 4096

/* `text` (`len` bytes) as a cJSON document, or NULL with `errs` filled.
 * An empty document (no text, or only comments) is an empty object: all
 * defaults. */
cJSON *schema_yaml_parse(const schema_t *s, const char *text, size_t len, schema_errors_t *errs);

/* `doc` as YAML text (malloc'd, ends in a newline; NULL when out of
 * memory). `header`, if not NULL, is put first as comment lines (one per
 * '\n'-separated line). Only what the schema knows is written. */
char *schema_yaml_emit(const schema_t *s, const cJSON *doc, const char *header);

#endif /* SCHEMA_YAML_H */
