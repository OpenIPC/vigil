/* SPDX-License-Identifier: GPL-3.0-or-later */
/* config_schema.c — every vigild setting, as data (see schema.h). A new
 * setting is a row here plus its member in vigil_config_t. */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "disc_proto.h"
#include "seg_writer.h"
#include "source.h"

/* The source drivers' own parsers decide: a URL vigild accepts here is one
 * a channel can be created from (cheap: no connection is made). */
static int check_url(const char *v, char *msg, size_t cap)
{
    if (!*v) return 0;                  /* an empty slot (camera_fields: url) */
    source_t *s = source_create(v, 0);
    if (!s) {
        snprintf(msg, cap, "not a camera URL (schemes: %s)", source_schemes());
        return -1;
    }
    source_destroy(s);
    return 0;
}

static int check_sub_url(const char *v, char *msg, size_t cap)
{
    return *v ? check_url(v, msg, cap) : 0;
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


/* ---- management settings -------------------------------------------------------- */

const char *const config_day_keys[7] = { "mon", "tue", "wed", "thu", "fri", "sat", "sun" };

/* One letter a half hour: C continuous, M motion only, - off (rec_sched.h). */
static int check_slots(const char *v, char *msg, size_t cap)
{
    if (strlen(v) != CONFIG_DAY_SLOTS || strspn(v, "CM-") != CONFIG_DAY_SLOTS) {
        snprintf(msg, cap, "must be %d letters, one per half hour: C (continuous), M (motion), - (off)",
                 CONFIG_DAY_SLOTS);
        return -1;
    }
    return 0;
}

/* A host name or an IPv4 address; "" = not set. */
static int check_host(const char *v, char *msg, size_t cap)
{
    if (!*v) return 0;
    size_t n = strspn(v, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_");
    if (v[n] || v[0] == '-' || v[0] == '.') {
        snprintf(msg, cap, "must be a host name or an IPv4 address");
        return -1;
    }
    return 0;
}

/* Text that goes into mount options, a URL or a command's configuration:
 * no quotes, backslashes or white space, which every one of them reads in
 * its own way. */
static int check_word(const char *v, char *msg, size_t cap)
{
    if (strpbrk(v, "\"'\\ \t`$")) {
        snprintf(msg, cap, "must not contain spaces, quotes, backslashes, ` or $");
        return -1;
    }
    return 0;
}

static int check_mount_opts(const char *v, char *msg, size_t cap)
{
    size_t n = strspn(v, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789=,._-:/");
    if (v[n]) {
        snprintf(msg, cap, "mount options: letters, digits and = , . _ - : / only");
        return -1;
    }
    if (strstr(v, "pass") || strstr(v, "cred")) {
        snprintf(msg, cap, "give the password in its own setting");
        return -1;
    }
    return 0;
}

/* "a@b.c, d@e.f" */
static int check_mails(const char *v, char *msg, size_t cap)
{
    for (const char *p = v; *p;) {
        while (*p == ' ' || *p == ',') ++p;
        if (!*p) break;
        size_t n = strcspn(p, ", ");
        const char *at = memchr(p, '@', n);
        int bad = !at || at == p || at == p + n - 1 || memchr(at + 1, '@', (size_t)(p + n - at - 1));
        for (size_t i = 0; i < n && !bad; ++i) bad = strchr("<>\"'\\`$;", p[i]) != NULL;
        if (bad) {
            snprintf(msg, cap, "'%.*s' is not a mail address", (int)(n > 40 ? 40 : n), p);
            return -1;
        }
        p += n;
    }
    return 0;
}

/* "eth0,wlan0": interface names, comma separated. */
static int check_ifaces(const char *v, char *msg, size_t cap)
{
    for (const char *p = v; *p; ++p)
        if (!(isalnum((unsigned char)*p) || strchr("_-.@:, ", *p))) {
            snprintf(msg, cap, "interface names, separated by commas");
            return -1;
        }
    return 0;
}

/* "10.0.1.5,10.0.2.0": IPv4 addresses, comma separated. */
static int check_targets(const char *v, char *msg, size_t cap)
{
    for (const char *p = v; *p; ) {
        while (*p == ',' || *p == ' ') ++p;
        size_t n = strcspn(p, ", ");
        char a[16];
        uint32_t ip;
        if (!n) break;
        if (n >= sizeof a) n = sizeof a - 1;
        memcpy(a, p, n);
        a[n] = 0;
        if (disc_ip_parse(a, &ip) < 0) {
            snprintf(msg, cap, "'%s' is not an IPv4 address", a);
            return -1;
        }
        p += strcspn(p, ", ");
    }
    return 0;
}

static int check_mail(const char *v, char *msg, size_t cap)
{
    if (strchr(v, ',')) {
        snprintf(msg, cap, "one address");
        return -1;
    }
    return check_mails(v, msg, cap);
}

static int check_http_url(const char *v, char *msg, size_t cap)
{
    if (!*v) return 0;
    if (strncmp(v, "http://", 7) && strncmp(v, "https://", 8)) {
        snprintf(msg, cap, "must start with http:// or https://");
        return -1;
    }
    return check_word(v, msg, cap);
}

static int check_url_path(const char *v, char *msg, size_t cap)
{
    if (v[0] != '/') {
        snprintf(msg, cap, "must start with /");
        return -1;
    }
    return check_word(v, msg, cap);
}

static const char *const on_full_values[] = { "overwrite", "stop", NULL };
static const char *const nas_types[] = { "none", "cifs", "nfs", NULL };
static const char *const ddns_providers[] = { "dyndns", "noip", "custom", NULL };
static const char *const smtp_security[] = { "none", "starttls", "tls", NULL };
static const char *const alarm_protocols[] = { "http", NULL };
static const char *const languages[] = { "en", NULL };
static const char *const date_formats[] = { "YYYY-MM-DD", "DD.MM.YYYY", "MM/DD/YYYY", NULL };
static const char *const time_formats[] = { "24h", "12h", NULL };

/* Every mode any platform drives; each platform offers its own subset
 * (hal_display_modes) and starts in its default when the one set here is
 * not among them. */
static const char *const output_modes[] = { "1280x720@60", "1280x720@50", "1920x1080@60", "1920x1080@50",
                                            "1920x1080@30", "1024x768@60", "1280x1024@60", "1440x900@60",
                                            "3840x2160@30", NULL };
static const char *const clock_positions[] = { "off", "top-left", "top-center", "top-right", "bottom-left",
                                               "bottom-center", "bottom-right", NULL };
static const char *const corners_off[] = { "off", "top-left", "top-right", "bottom-left", "bottom-right", NULL };
static const char *const corners[] = { "top-left", "top-right", "bottom-left", "bottom-right", NULL };
static const char *const osd_sizes[] = { "small", "medium", "large", "xlarge", NULL };
static const char *const osd_backgrounds[] = { "outline", "none", "box", NULL };
static const char *const video_aspects[] = { "fit", "stretch", "fill", NULL };

int config_output_mode(const char *s, unsigned *w, unsigned *h, unsigned *hz)
{
    char tail;
    if (!s || sscanf(s, "%ux%u@%u%c", w, h, hz, &tail) != 3 || !*w || !*h || !*hz) return -1;
    return 0;
}

static int check_prefer(const char *v, char *msg, size_t cap)
{
    if (*v && !disc_proto_bits(v)) {
        snprintf(msg, cap, "protocols: netip, onvif, dhip, separated by commas");
        return -1;
    }
    return 0;
}

#define STR(T, M) .type = SCHEMA_STRING, .off = offsetof(T, M), .size = sizeof(((T *)0)->M)
#define INT(T, M) .type = SCHEMA_INT, .off = offsetof(T, M)
#define BOOL(T, M) .type = SCHEMA_BOOL, .off = offsetof(T, M)
#define ENUM(T, M, V) .type = SCHEMA_ENUM, .off = offsetof(T, M), .size = sizeof(((T *)0)->M), .values = V


static const schema_field_t camera_fields[] = {
    { .key = "url", STR(config_camera_t, url), .def_str = "", .secret = SCHEMA_SECRET_URL,
      .live = 1, .check = check_url, .label = "Source URL",
      .help = "rtsp://[user:pass@]host[:554]/path, "
              "netip://[user:pass@]host[:34567]/[?ch=N&stream=main|extra] or capture:N. "
              "The password is shown as *** and kept when sent back unchanged. "
              "Empty: no camera in this channel (the list is in channel order)." },
    { .key = "name", STR(config_camera_t, name), .def_str = "", .live = 1,
      .label = "Name", .help = "Shown in the status and the API; empty: the camera's host." },
    { .key = "enabled", BOOL(config_camera_t, enabled), .def_int = 1, .live = 1,
      .label = "Enabled", .help = "A disabled camera keeps its tile, empty." },
    { .key = "sub_url", STR(config_camera_t, sub_url), .def_str = "", .secret = SCHEMA_SECRET_URL,
      .live = 1, .check = check_sub_url, .label = "Sub-stream URL",
      .help = "Used in multi-window views; the main URL is used full screen and for recording. "
              "Empty: derived for NETIP, otherwise the main stream everywhere." },
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
    { .key = "on_full", ENUM(config_record_t, on_full, on_full_values), .def_str = "overwrite",
      .live = 1, .label = "When the disk is full",
      .help = "overwrite: the oldest recordings, whichever camera made them, make room; stop: "
              "recording stops (event \"REC disk full\") until there is room again." },
};



#define ALL_DAY "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
#define DAY(K, I, L) { .key = K, STR(config_sched_t, day[I]), .def_str = ALL_DAY, .live = 1, \
                       .check = check_slots, .label = L }

static const schema_field_t sched_fields[] = {
    { .key = "camera", INT(config_sched_t, camera), .min = 1, .max = CHANNEL_MAX, .required = 1,
      .live = 1, .label = "Camera", .help = "CAMn: the camera's channel." },
    DAY("mon", 0, "Monday"), DAY("tue", 1, "Tuesday"), DAY("wed", 2, "Wednesday"),
    DAY("thu", 3, "Thursday"), DAY("fri", 4, "Friday"), DAY("sat", 5, "Saturday"),
    DAY("sun", 6, "Sunday"),
};

static const schema_field_t quota_fields[] = {
    { .key = "camera", INT(config_quota_t, camera), .min = 1, .max = CHANNEL_MAX, .required = 1,
      .live = 1, .label = "Camera", .help = "CAMn: the camera's channel." },
    { .key = "max_gb", INT(config_quota_t, max_gb), .def_int = 0, .min = 0, .max = 1000000, .live = 1,
      .label = "Largest size (GB)",
      .help = "The camera's oldest recordings are deleted beyond this. 0: no limit." },
    { .key = "max_days", INT(config_quota_t, max_days), .def_int = 0, .min = 0, .max = 3650, .live = 1,
      .label = "Kept at most (days)",
      .help = "Recordings older than this are deleted. 0: no limit." },
};

static const schema_field_t nas_fields[] = {
    { .key = "type", ENUM(config_nas_t, type, nas_types), .def_str = "none", .live = 1,
      .label = "Network storage",
      .help = "A share mounted at /mnt/nas; record.path under /mnt/nas records onto it. "
              "none: not used." },
    { .key = "server", STR(config_nas_t, server), .def_str = "", .live = 1, .check = check_host,
      .label = "Server", .help = "Host name or IPv4 address." },
    { .key = "share", STR(config_nas_t, share), .def_str = "", .live = 1, .check = check_word,
      .label = "Share", .help = "SMB: the share's name (and a folder in it: share/dir). "
                                "NFS: the exported path (/export/nvr)." },
    { .key = "user", STR(config_nas_t, user), .def_str = "", .live = 1, .check = check_word,
      .label = "User", .help = "SMB only; empty: guest." },
    { .key = "password", STR(config_nas_t, password), .def_str = "", .secret = SCHEMA_SECRET,
      .live = 1, .label = "Password", .help = "Shown as ***; sent back as *** it is kept." },
    { .key = "options", STR(config_nas_t, options), .def_str = "", .live = 1,
      .check = check_mount_opts, .label = "Mount options",
      .help = "Added to the mount's own (vers=2.1, ...)." },
};

static const schema_field_t ddns_fields[] = {
    { .key = "enabled", BOOL(config_ddns_t, enabled), .def_int = 0, .live = 1, .label = "Enabled",
      .help = "Tell a dynamic DNS service this recorder's public address." },
    { .key = "provider", ENUM(config_ddns_t, provider, ddns_providers), .def_str = "dyndns", .live = 1,
      .label = "Provider",
      .help = "dyndns: the dyndns2 protocol (Dyn and compatibles); noip: No-IP; custom: the URL "
              "below." },
    { .key = "host", STR(config_ddns_t, host), .def_str = "", .live = 1, .check = check_host,
      .label = "Host name", .help = "The name to update (nvr.example.org)." },
    { .key = "user", STR(config_ddns_t, user), .def_str = "", .live = 1, .check = check_word,
      .label = "User", .help = "" },
    { .key = "password", STR(config_ddns_t, password), .def_str = "", .secret = SCHEMA_SECRET,
      .live = 1, .check = check_word, .label = "Password", .help = "Shown as ***; sent back as *** it is kept." },
    { .key = "url", STR(config_ddns_t, url), .def_str = "", .live = 1, .check = check_http_url,
      .label = "Update URL",
      .help = "custom: requested with GET; {host}, {user} and {password} are replaced." },
    { .key = "interval", INT(config_ddns_t, interval), .def_int = 10, .min = 5, .max = 1440, .live = 1,
      .label = "Update every (min)", .help = "" },
};

static const schema_field_t upnp_fields[] = {
    { .key = "enabled", BOOL(config_upnp_t, enabled), .def_int = 0, .live = 1, .label = "Enabled",
      .help = "Ask the router to forward ports to this recorder (not active yet: vigild has no "
              "UPnP client; the settings are kept)." },
    { .key = "http_port", INT(config_upnp_t, http_port), .def_int = 0, .min = 0, .max = 65535,
      .live = 1, .label = "External HTTP port", .help = "Forwarded to the HTTP API. 0: none." },
    { .key = "rtsp_port", INT(config_upnp_t, rtsp_port), .def_int = 0, .min = 0, .max = 65535,
      .live = 1, .label = "External RTSP port", .help = "0: none." },
};

static const schema_field_t smtp_fields[] = {
    { .key = "enabled", BOOL(config_smtp_t, enabled), .def_int = 0, .live = 1, .label = "Enabled",
      .help = "Mail the events that need attention (disk full, recording failed, a camera "
              "offline) to the addresses below." },
    { .key = "server", STR(config_smtp_t, server), .def_str = "", .live = 1, .check = check_host,
      .label = "Server", .help = "The SMTP server (smtp.example.org)." },
    { .key = "port", INT(config_smtp_t, port), .def_int = 587, .min = 1, .max = 65535, .live = 1,
      .label = "Port", .help = "587 with STARTTLS, 465 with TLS, 25 without." },
    { .key = "security", ENUM(config_smtp_t, security, smtp_security), .def_str = "starttls",
      .live = 1, .label = "Encryption", .help = "" },
    { .key = "user", STR(config_smtp_t, user), .def_str = "", .live = 1, .check = check_word,
      .label = "User", .help = "Empty: no login." },
    { .key = "password", STR(config_smtp_t, password), .def_str = "", .secret = SCHEMA_SECRET,
      .live = 1, .check = check_word, .label = "Password", .help = "Shown as ***; sent back as *** it is kept." },
    { .key = "from", STR(config_smtp_t, from), .def_str = "", .live = 1, .check = check_mail,
      .label = "From", .help = "The sender's address." },
    { .key = "to", STR(config_smtp_t, to), .def_str = "", .live = 1, .check = check_mails,
      .label = "To", .help = "Addresses, separated by commas." },
};

static const schema_field_t alarm_fields[] = {
    { .key = "enabled", BOOL(config_alarm_t, enabled), .def_int = 0, .live = 1, .label = "Enabled",
      .help = "Send every event to an alarm center as it happens." },
    { .key = "protocol", ENUM(config_alarm_t, protocol, alarm_protocols), .def_str = "http", .live = 1,
      .label = "Protocol", .help = "http: each event as JSON, POSTed to http://server:port/path." },
    { .key = "host", STR(config_alarm_t, host), .def_str = "", .live = 1, .check = check_host,
      .label = "Server", .help = "" },
    { .key = "port", INT(config_alarm_t, port), .def_int = 80, .min = 1, .max = 65535, .live = 1,
      .label = "Port", .help = "" },
    { .key = "path", STR(config_alarm_t, path), .def_str = "/", .live = 1, .check = check_url_path,
      .label = "Path", .help = "" },
    { .key = "interval", INT(config_alarm_t, interval), .def_int = 60, .min = 0, .max = 86400, .live = 1,
      .label = "Heartbeat (s)", .help = "A heartbeat this often while there are no events. 0: none." },
};

static const schema_field_t time_fields[] = {
    { .key = "ntp", BOOL(config_time_t, ntp), .def_int = 1, .live = 1, .label = "Synchronise with NTP",
      .help = "Keep the clock from the servers in /etc/ntp.conf (the firmware's ntpd)." },
    { .key = "ntp_interval", INT(config_time_t, ntp_interval), .def_int = 60, .min = 10, .max = 1440,
      .live = 1, .label = "Check every (min)",
      .help = "How often vigild asks the servers itself, to show the last sync and to set the "
              "clock if the firmware's ntpd is not running." },
};

static const schema_field_t ui_fields[] = {
    { .key = "language", ENUM(config_ui_t, language, languages), .def_str = "en", .live = 1,
      .label = "Language", .help = "Of the on-screen UI." },
    { .key = "date_format", ENUM(config_ui_t, date_format, date_formats), .def_str = "YYYY-MM-DD",
      .live = 1, .label = "Date format", .help = "" },
    { .key = "time_format", ENUM(config_ui_t, time_format, time_formats), .def_str = "24h", .live = 1,
      .label = "Time format", .help = "" },
    { .key = "login", BOOL(config_ui_t, login), .def_int = 0, .live = 1, .label = "Log in on screen",
      .help = "The menus and settings ask for a user and its password (vigil's users, or the "
              "system account, which always works); the live view stays." },
    { .key = "idle_logout", INT(config_ui_t, idle_logout), .def_int = 10, .min = 0, .max = 240, .live = 1,
      .label = "Log out after (min)", .help = "Without a key or the mouse for this long. 0: never." },
};

#define OSD_BG_HELP "outline: hard-edged text with a thin dark edge, straight on the video; none: " \
                    "the text alone; box: on a dark box."
static const schema_field_t display_fields[] = {
    { .key = "output", ENUM(config_display_t, output, output_modes), .def_str = "1280x720@60", .live = 1,
      .label = "Output resolution",
      .help = "HDMI and VGA, as WIDTHxHEIGHT@HZ. The menus keep their layout, scaled to it; the video "
              "is shown at the output's own resolution. A mode the board does not drive is ignored." },
    { .key = "video_aspect", ENUM(config_display_t, video_aspect, video_aspects), .def_str = "fit", .live = 1,
      .label = "Video aspect",
      .help = "fit: each picture keeps its shape inside its tile or the full screen, with black bars "
              "where the shapes differ; a sub-stream (704x576, 352x288, ...) is shown at its main "
              "stream's shape. stretch: the picture fills the tile, whatever its shape. fill: the picture "
              "keeps its shape and covers the whole tile, what overflows cut off (a 4:3 camera in a 16:9 "
              "tile loses a strip at the top and at the bottom)." },
    { .key = "clock_position", ENUM(config_display_t, clock_position, clock_positions), .def_str = "top-right",
      .live = 1, .label = "Clock",
      .help = "Where the date and time are on the live view; their format is ui.date_format and "
              "ui.time_format." },
    { .key = "clock_size", ENUM(config_display_t, clock_size, osd_sizes), .def_str = "medium", .live = 1,
      .label = "Clock size", .help = "" },
    { .key = "clock_background", ENUM(config_display_t, clock_background, osd_backgrounds),
      .def_str = "outline", .live = 1, .label = "Clock background", .help = OSD_BG_HELP },
    { .key = "name_position", ENUM(config_display_t, name_position, corners_off), .def_str = "bottom-left",
      .live = 1, .label = "Camera names", .help = "The corner of each tile with its camera's name." },
    { .key = "name_size", ENUM(config_display_t, name_size, osd_sizes), .def_str = "medium", .live = 1,
      .label = "Camera name size", .help = "A size smaller in small tiles." },
    { .key = "name_background", ENUM(config_display_t, name_background, osd_backgrounds),
      .def_str = "outline", .live = 1, .label = "Camera name background", .help = OSD_BG_HELP },
    { .key = "icons_position", ENUM(config_display_t, icons_position, corners), .def_str = "bottom-right",
      .live = 1, .label = "Status icons",
      .help = "The corner of each tile with REC and the camera's events; in the names' corner they go "
              "just above the name (below it at the top)." },
    { .key = "icons_background", ENUM(config_display_t, icons_background, osd_backgrounds),
      .def_str = "outline", .live = 1, .label = "Status icons background", .help = OSD_BG_HELP },
};

static const schema_field_t api_fields[] = {
    { .key = "listen", STR(config_api_t, listen), .def_str = "0.0.0.0:8080", .check = check_listen,
      .label = "Listen on", .help = "IPv4 address and port of the HTTP API, or just the port. "
                                    "Empty: no API. Takes effect when vigild starts." },
    { .key = "auth", BOOL(config_api_t, auth), .def_int = 1, .live = 1,
      .label = "Require a password",
      .help = "HTTP Basic authentication: vigil's users (api.users), or the system account. "
              "Turning it off lets anyone on the network change the configuration." },
    { .key = "user", STR(config_api_t, user), .def_str = "root", .live = 1, .check = check_user,
      .label = "Account", .help = "The system account that logs in with every permission "
                                  "(also as \"admin\" while no user of that name exists)." },
    { .key = "shadow", STR(config_api_t, shadow), .def_str = "/etc/shadow", .live = 1,
      .check = check_shadow, .label = "Password file",
      .help = "The shadow file holding the account's password hash." },
    { .key = "users", STR(config_api_t, users), .def_str = "/etc/vigil.users.yaml", .live = 1,
      .check = check_shadow, .label = "Accounts file",
      .help = "vigil's own users and groups, with their permissions (written by the API and "
              "the UI, mode 0600). The account above always logs in too, with every "
              "permission, so nobody is locked out." },
};

static const schema_field_t web_fields[] = {
    { .key = "enabled", BOOL(config_web_t, enabled), .def_int = 1, .live = 1, .label = "On",
      .help = "This screen, with its mouse and keyboard, in a browser at http://<this recorder>/ "
              "(docs/api.md, Zero channel). Only users whose group has the remote permission "
              "(admin) get in, and nobody until such an account has a password." },
    { .key = "listen", STR(config_web_t, listen), .def_str = "0.0.0.0:80", .check = check_listen,
      .label = "Listen on", .help = "IPv4 address and port of its web server, or just the port. "
                                    "Empty: off. Takes effect when vigild starts." },
    { .key = "fps", INT(config_web_t, fps), .def_int = 15, .min = 1, .max = 30, .live = 1,
      .label = "Pictures a second, at most",
      .help = "Fewer when nothing moves, the network or the browser is slower, or encoding would "
              "take more than half a CPU core." },
};

static const schema_field_t discovery_fields[] = {
    { .key = "interfaces", STR(config_discovery_t, interfaces), .def_str = "", .live = 1,
      .check = check_ifaces, .label = "Search on interfaces",
      .help = "Network interfaces to search for cameras on, separated by commas. Empty: every "
              "IPv4 interface." },
    { .key = "targets", STR(config_discovery_t, targets), .def_str = "", .live = 1,
      .check = check_targets, .label = "Also ask addresses",
      .help = "IPv4 addresses searched by unicast as well, separated by commas: cameras on a "
              "routed subnet, which searches by broadcast do not reach." },
    { .key = "prefer", STR(config_discovery_t, prefer), .def_str = "netip,onvif,dhip", .live = 1,
      .check = check_prefer, .label = "Protocol order",
      .help = "Which protocol a camera found is added with when it answers more than one: "
              "NETIP names both streams, ONVIF asks the camera, Dahua's RTSP path is fixed." },
};

#define N(a) (int)(sizeof(a) / sizeof((a)[0]))

static const schema_section_t sections[] = {
    { .key = "cameras", .is_array = 1, .off = offsetof(vigil_config_t, cameras),
      .count_off = offsetof(vigil_config_t, n_cameras), .item_size = sizeof(config_camera_t),
      .max_items = CHANNEL_MAX, .fields = camera_fields, .n_fields = N(camera_fields),
      .label = "Cameras", .help = "In tile order: the first is CAM1." },
    { .key = "record", .off = offsetof(vigil_config_t, record), .fields = record_fields,
      .n_fields = N(record_fields), .label = "Recording" },
#define ARRAY(K, M, T, F, L, H) \
    { .key = K, .is_array = 1, .off = offsetof(vigil_config_t, M), \
      .count_off = offsetof(vigil_config_t, n_##M##s), .item_size = sizeof(T), \
      .max_items = CHANNEL_MAX, .fields = F, .n_fields = N(F), .label = L, .help = H }
#define OBJECT(K, M, F, L) { .key = K, .off = offsetof(vigil_config_t, M), .fields = F, \
                             .n_fields = N(F), .label = L }
    ARRAY("schedule", schedule, config_sched_t, sched_fields, "Recording schedule",
          "A week per camera; a camera without one records all the time. Each day is 48 "
          "letters, one per half hour from 00:00: C continuous, M when the camera reports "
          "motion, - not at all."),
    ARRAY("quota", quota, config_quota_t, quota_fields, "Disk quota",
          "Per camera; a camera without one is limited only by the disk."),
    OBJECT("nas", nas, nas_fields, "Network storage"),
    OBJECT("ddns", ddns, ddns_fields, "Dynamic DNS"),
    OBJECT("upnp", upnp, upnp_fields, "UPnP"),
    OBJECT("smtp", smtp, smtp_fields, "Mail (SMTP)"),
    OBJECT("alarm", alarm, alarm_fields, "Alarm center"),
    OBJECT("time", time, time_fields, "Time"),
    OBJECT("ui", ui, ui_fields, "On-screen UI"),
    { .key = "display", .off = offsetof(vigil_config_t, display), .fields = display_fields,
      .n_fields = N(display_fields), .label = "Display",
      .help = "The HDMI/VGA output, and the clock, names and icons over the live view." },
    { .key = "api", .off = offsetof(vigil_config_t, api), .fields = api_fields,
      .n_fields = N(api_fields), .label = "HTTP API" },
    OBJECT("web", web, web_fields, "Zero channel"),
    { .key = "discovery", .off = offsetof(vigil_config_t, discovery), .fields = discovery_fields,
      .n_fields = N(discovery_fields), .label = "Camera search" },
};

const schema_t config_schema = {
    .version = CONFIG_VERSION,
    .sections = sections,
    .n_sections = N(sections),
};
