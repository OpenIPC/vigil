/* SPDX-License-Identifier: GPL-3.0-or-later */
/* config_schema.c — every vigild setting, as data (see schema.h). A new
 * setting is a row here plus its member in vigil_config_t. */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "seg_writer.h"
#include "source.h"

/* The source drivers' own parsers decide: a URL vigild accepts here is one
 * a channel can be created from (cheap: no connection is made). */
static int check_url(const char *v, char *msg, size_t cap)
{
    source_t *s = source_create(v, 0);
    if (!s) {
        snprintf(msg, cap, "not a camera URL (schemes: %s)", source_schemes());
        return -1;
    }
    source_destroy(s);
    return 0;
}

static int check_abs_path(const char *v, char *msg, size_t cap, int empty_ok)
{
    if (!*v && empty_ok) return 0;
    if (v[0] != '/') {
        snprintf(msg, cap, "must be an absolute path");
        return -1;
    }
    return 0;
}

/* "." and ".." are refused: the directory is judged by the part of the path
 * that exists (rec_place.c), and a ".." in the part still to be created
 * could climb out of the disk it names. */
static int check_rec_path(const char *v, char *msg, size_t cap)
{
    if (check_abs_path(v, msg, cap, 1) < 0) return -1;
    for (const char *p = v; *p; ) {
        while (*p == '/') ++p;
        size_t n = strcspn(p, "/");
        if ((n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) {
            snprintf(msg, cap, "must not contain \".\" or \"..\"");
            return -1;
        }
        p += n;
    }
    return 0;
}
static int check_shadow(const char *v, char *msg, size_t cap) { return check_abs_path(v, msg, cap, 0); }

/* "port" or "a.b.c.d:port" (IPv4: CivetWeb is built without IPv6); "" =
 * no API. */
static int check_listen(const char *v, char *msg, size_t cap)
{
    if (!*v) return 0;
    const char *colon = strrchr(v, ':');
    const char *port = colon ? colon + 1 : v;
    char *end;
    long p = strtol(port, &end, 10);
    if (end == port || *end || p < 1 || p > 65535) {
        snprintf(msg, cap, "needs a port 1..65535 (\"8080\" or \"0.0.0.0:8080\")");
        return -1;
    }
    if (colon) {
        char addr[INET_ADDRSTRLEN];
        struct in_addr a;
        size_t n = (size_t)(colon - v);
        if (n >= sizeof addr) n = sizeof addr - 1;
        memcpy(addr, v, n);
        addr[n] = 0;
        if (inet_pton(AF_INET, addr, &a) != 1) {
            snprintf(msg, cap, "'%s' is not an IPv4 address", addr);
            return -1;
        }
    }
    return 0;
}

static int check_user(const char *v, char *msg, size_t cap)
{
    if (!*v || strpbrk(v, ": /")) {
        snprintf(msg, cap, "must be an account name");
        return -1;
    }
    return 0;
}

#define STR(T, M) .type = SCHEMA_STRING, .off = offsetof(T, M), .size = sizeof(((T *)0)->M)
#define INT(T, M) .type = SCHEMA_INT, .off = offsetof(T, M)
#define BOOL(T, M) .type = SCHEMA_BOOL, .off = offsetof(T, M)

static const schema_field_t camera_fields[] = {
    { .key = "url", STR(config_camera_t, url), .required = 1, .secret = SCHEMA_SECRET_URL,
      .live = 1, .check = check_url, .label = "Source URL",
      .help = "rtsp://[user:pass@]host[:554]/path, "
              "netip://[user:pass@]host[:34567]/[?ch=N&stream=main|extra] or capture:N. "
              "The password is shown as *** and kept when sent back unchanged." },
    { .key = "name", STR(config_camera_t, name), .def_str = "", .live = 1,
      .label = "Name", .help = "Shown in the status and the API; empty: the camera's host." },
    { .key = "enabled", BOOL(config_camera_t, enabled), .def_int = 1, .live = 1,
      .label = "Enabled", .help = "A disabled camera keeps its tile, empty." },
};

static const schema_field_t record_fields[] = {
    { .key = "path", STR(config_record_t, path), .def_str = "", .live = 1, .check = check_rec_path,
      .label = "Recording directory",
      .help = "Where the streams are recorded, as received. Empty: no recording." },
    { .key = "segment", INT(config_record_t, segment), .def_int = 60, .min = 10, .max = 3600,
      .live = 1, .label = "Segment length (s)",
      .help = "A new file starts at the first keyframe after this many seconds." },
    /* The writer remembers the files it made in a ring of SEG_KEEP_MAX. */
    { .key = "keep", INT(config_record_t, keep), .def_int = 3, .min = 0, .max = SEG_KEEP_MAX,
      .live = 1, .label = "Segments kept per camera",
      .help = "0: all (the oldest recordings are deleted anyway when the disk runs low)." },
    { .key = "allow_rootfs", BOOL(config_record_t, allow_rootfs), .def_int = 0, .live = 1,
      .label = "Record onto the root filesystem",
      .help = "Off: recording waits until a disk is mounted where the path leads, and never "
              "writes to the root filesystem (on a board, its flash). On only where the root "
              "filesystem is itself a disk. RAM and flash filesystems are refused either way, "
              "the root included." },
};

static const schema_field_t api_fields[] = {
    { .key = "listen", STR(config_api_t, listen), .def_str = "0.0.0.0:8080", .check = check_listen,
      .label = "Listen on", .help = "IPv4 address and port of the HTTP API, or just the port. "
                                    "Empty: no API. Takes effect when vigild starts." },
    { .key = "auth", BOOL(config_api_t, auth), .def_int = 1, .live = 1,
      .label = "Require a password",
      .help = "HTTP Basic authentication against the system account. Turning it off lets "
              "anyone on the network change the configuration." },
    { .key = "user", STR(config_api_t, user), .def_str = "root", .live = 1, .check = check_user,
      .label = "Account", .help = "The system account whose password the API asks for." },
    { .key = "shadow", STR(config_api_t, shadow), .def_str = "/etc/shadow", .live = 1,
      .check = check_shadow, .label = "Password file",
      .help = "The shadow file holding the account's password hash." },
};

#define N(a) (int)(sizeof(a) / sizeof((a)[0]))

static const schema_section_t sections[] = {
    { .key = "cameras", .is_array = 1, .off = offsetof(vigil_config_t, cameras),
      .count_off = offsetof(vigil_config_t, n_cameras), .item_size = sizeof(config_camera_t),
      .max_items = CHANNEL_MAX, .fields = camera_fields, .n_fields = N(camera_fields),
      .label = "Cameras", .help = "In tile order: the first is CAM1." },
    { .key = "record", .off = offsetof(vigil_config_t, record), .fields = record_fields,
      .n_fields = N(record_fields), .label = "Recording" },
    { .key = "api", .off = offsetof(vigil_config_t, api), .fields = api_fields,
      .n_fields = N(api_fields), .label = "HTTP API" },
};

const schema_t config_schema = {
    .version = CONFIG_VERSION,
    .sections = sections,
    .n_sections = N(sections),
};
