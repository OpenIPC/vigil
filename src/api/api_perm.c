/* SPDX-License-Identifier: GPL-3.0-or-later */
/* api_perm.c — which permission each endpoint needs (api_int.h). One
 * table, read before any request is dispatched: an endpoint added later
 * gets "system" until it is listed here, never "anyone". */
#include <stdio.h>
#include <string.h>

#include "api_int.h"
#include "users.h"

/* `uri` is `p` or below it ("p/..."). */
static int at(const char *uri, const char *p)
{
    size_t n = strlen(p);
    return !strncmp(uri, p, n) && (uri[n] == 0 || uri[n] == '/');
}

static const struct { const char *key; unsigned perm; } SECTIONS[] = {
    { "cameras", PERM_CAMERAS }, { "discovery", PERM_CAMERAS },
    { "record", PERM_STORAGE }, { "schedule", PERM_STORAGE }, { "quota", PERM_STORAGE },
    { "nas", PERM_STORAGE },
    { "ddns", PERM_NETWORK }, { "upnp", PERM_NETWORK }, { "smtp", PERM_NETWORK },
    { "alarm", PERM_NETWORK }, { "qos", PERM_NETWORK }, { "onvif_server", PERM_NETWORK },
    { "netip_server", PERM_NETWORK },
    { "time", PERM_SYSTEM }, { "ui", PERM_SYSTEM }, { "display", PERM_SYSTEM },
    /* The live view's layout and page: what the screen's Split and page
     * arrows change, which need no login there either. */
    { "live", PERM_LIVE },
    /* api.auth and api.user decide who gets in at all; web, who reaches
     * the console; rtsp_server.auth, who sees every camera. */
    { "api", PERM_USERS }, { "web", PERM_USERS }, { "rtsp_server", PERM_USERS },
};

unsigned api_perm_needed(const char *method, const char *uri)
{
    int get = !strcmp(method, "GET") || !strcmp(method, "HEAD");
    if (!strcmp(uri, "/api/v1/status") || !strcmp(uri, "/api/v1/ws")) return PERM_LIVE;
    if (!strcmp(uri, "/api/v1/schema") || !strcmp(uri, "/api/v1/me") || at(uri, "/api/v1/jobs")) return 0;
    /* Every permission the API itself knows: "remote" is the zero
     * channel's (web.c), and a group with all the others always could. */
    if (!strcmp(uri, "/api/v1/config")) return get ? PERM_SYSTEM : PERM_ALL & ~PERM_REMOTE;
    if (!strncmp(uri, "/api/v1/config/", 15)) {
        for (size_t i = 0; i < sizeof SECTIONS / sizeof SECTIONS[0]; ++i)
            if (!strcmp(uri + 15, SECTIONS[i].key)) return SECTIONS[i].perm;
        return PERM_SYSTEM;
    }
    if (at(uri, "/api/v1/network")) return PERM_NETWORK;
    if (at(uri, "/api/v1/system")) return PERM_SYSTEM;
    if (!strncmp(uri, "/api/v1/disks", 13) || at(uri, "/api/v1/storage")) return PERM_STORAGE;
    /* Moving a camera is its own permission, reading its PTZ too. */
    int ch, used = 0;
    if (sscanf(uri, "/api/v1/channels/%d/ptz%n", &ch, &used) == 1 && used && !uri[used]) return PERM_PTZ;
    if (!strncmp(uri, "/api/v1/discovery", 17) || at(uri, "/api/v1/channels")) return PERM_CAMERAS;
    if (!strncmp(uri, "/api/v1/users", 13) || !strncmp(uri, "/api/v1/groups", 14)) return PERM_USERS;
    return PERM_SYSTEM;
}
