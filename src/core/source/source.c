/* SPDX-License-Identifier: GPL-3.0-or-later */
/* source.c — source registry and URL helpers (see source.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "source.h"

extern const source_ops_t source_rtsp_ops;
extern const source_ops_t source_netip_ops;
extern const source_ops_t source_capture_ops;

static const source_ops_t *const g_sources[] = {
    &source_rtsp_ops,
    &source_netip_ops,
    &source_capture_ops,
};

const char *source_schemes(void)
{
    return "rtsp, netip, capture";
}

source_t *source_create(const char *url, int index)
{
    const char *colon = strchr(url, ':');
    if (!colon) return NULL;
    size_t sl = (size_t)(colon - url);
    for (size_t i = 0; i < sizeof g_sources / sizeof g_sources[0]; ++i) {
        const source_ops_t *ops = g_sources[i];
        if (strlen(ops->scheme) != sl || strncmp(url, ops->scheme, sl) != 0) continue;
        source_t *s = calloc(1, sizeof *s);
        if (!s) return NULL;
        s->ops = ops;
        s->index = index;
        snprintf(s->url, sizeof s->url, "%s", url);
        if (ops->create(s) < 0) {
            free(s);
            return NULL;
        }
        return s;
    }
    return NULL;
}

void source_destroy(source_t *s)
{
    if (!s) return;
    s->ops->close(s);
    s->ops->destroy(s);
    free(s);
}

int url_split(const char *url, const char *scheme, char *user, size_t ul,
              char *pass, size_t pl, char *host, size_t hl, int *port,
              const char **path)
{
    size_t sl = strlen(scheme);
    if (strncmp(url, scheme, sl) != 0 || strncmp(url + sl, "://", 3) != 0) return -1;
    const char *p = url + sl + 3;
    const char *slash = p + strcspn(p, "/?");
    const char *at = memchr(p, '@', (size_t)(slash - p));
    user[0] = pass[0] = 0;
    if (at) {
        const char *c = memchr(p, ':', (size_t)(at - p));
        size_t n = (size_t)((c ? c : at) - p);
        if (n >= ul) return -1;
        memcpy(user, p, n);
        user[n] = 0;
        if (c) {
            n = (size_t)(at - c - 1);
            if (n >= pl) return -1;
            memcpy(pass, c + 1, n);
            pass[n] = 0;
        }
        p = at + 1;
    }
    const char *c = memchr(p, ':', (size_t)(slash - p));
    size_t n = (size_t)((c ? c : slash) - p);
    if (n == 0 || n >= hl) return -1;
    memcpy(host, p, n);
    host[n] = 0;
    if (c) *port = atoi(c + 1);
    *path = slash;
    return 0;
}

int url_query(const char *path, const char *key, char *out, size_t cap)
{
    const char *q = strchr(path, '?');
    size_t kl = strlen(key);
    for (q = q ? q + 1 : NULL; q && *q; ) {
        const char *amp = q + strcspn(q, "&");
        if ((size_t)(amp - q) > kl && strncmp(q, key, kl) == 0 && q[kl] == '=') {
            size_t n = (size_t)(amp - q - kl - 1);
            if (n >= cap) n = cap - 1;
            memcpy(out, q + kl + 1, n);
            out[n] = 0;
            return 1;
        }
        q = *amp ? amp + 1 : amp;
    }
    return 0;
}
