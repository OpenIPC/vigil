/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys_curl.c — mail, DDNS and the alarm center through curl (sys_curl.h). */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "qos.h"
#include "sys.h"
#include "sys_curl.h"

#ifndef VIGIL_VERSION
#define VIGIL_VERSION "dev"
#endif

#define CA_BUNDLE "/etc/ssl/certs/ca-certificates.crt"

int curl_available(void) { return sys_have("curl"); }

/* A private file under /tmp with `data`; its real path into `path`. */
static int temp_file(const char *what, const char *data, char *path, size_t cap)
{
    char rel[64], p[512];
    snprintf(rel, sizeof rel, "/tmp/vigil-%s.XXXXXX", what);
    snprintf(path, cap, "%s", sys_path(p, sizeof p, rel));
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    fchmod(fd, 0600);
    size_t len = strlen(data);
    for (size_t done = 0; done < len;) {
        ssize_t n = write(fd, data + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            close(fd);
            unlink(path);
            return -1;
        }
        done += (size_t)n;
    }
    close(fd);
    return 0;
}

/* Common options; curl's TLS checks the server against the firmware's CA
 * bundle when it is there. The traffic class's DSCP (qos.h) goes as
 * ip-tos, which curl has since 8.9; only when it is set, so that an older
 * curl still works with QoS off. */
static void base_opts(char *cfg, size_t cap, qos_class_t cls)
{
    char ca[512];
    size_t l = strlen(cfg);
    snprintf(cfg + l, cap - l, "silent\nshow-error\nconnect-timeout = 15\nmax-time = 60\n"
                               "user-agent = \"OpenIPC vigil/%s\"\n", VIGIL_VERSION);
    int tos = qos_tos(qos_dscp(cls));
    if (tos > 0) {
        l = strlen(cfg);
        snprintf(cfg + l, cap - l, "ip-tos = %d\n", tos);
    }
    if (sys_exists(CA_BUNDLE)) {
        l = strlen(cfg);
        snprintf(cfg + l, cap - l, "cacert = \"%s\"\n", sys_path(ca, sizeof ca, CA_BUNDLE));
    }
}

static int run_curl(const char *cfg, char *out, size_t cap)
{
    const char *argv[] = { "curl", "-K", "-", NULL };
    int rc = sys_ops()->run(argv, cfg, 75, out, cap);
    sys_trim(out);
    return rc;
}

/* The message: headers, a blank line, the text; CRLF throughout, as SMTP
 * wants it on the wire. */
static char *message(const config_smtp_t *s, const char *subject, const char *body)
{
    size_t cap = strlen(body) * 2 + 1024;
    char *m = malloc(cap), date[64];
    if (!m) return NULL;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S %z", &tm);
    size_t len = (size_t)snprintf(m, cap,
                                  "From: %s\r\nTo: %s\r\nSubject: %s\r\nDate: %s\r\nMIME-Version: 1.0\r\n"
                                  "Content-Type: text/plain; charset=UTF-8\r\n\r\n",
                                  s->from, s->to, subject, date);
    for (const char *p = body; *p && len + 3 < cap; ++p) {
        if (*p == '\n' && (p == body || p[-1] != '\r')) m[len++] = '\r';
        m[len++] = *p;
    }
    if (len + 3 < cap && (len < 2 || m[len - 1] != '\n')) {
        m[len++] = '\r';
        m[len++] = '\n';
    }
    m[len] = 0;
    return m;
}

/* `m` (the whole message) to every address of s->to. */
static int mail_run(const config_smtp_t *s, char *m, char *msg, size_t cap)
{
    char file[512];
    if (!m || temp_file("mail", m, file, sizeof file) < 0) {
        free(m);
        snprintf(msg, cap, "cannot write the message to /tmp");
        return -1;
    }
    free(m);
    char cfg[2048] = "";
    int tls = !strcmp(s->security, "tls");
    size_t l = (size_t)snprintf(cfg, sizeof cfg, "url = \"%s://%s:%d\"\nmail-from = \"%s\"\nupload-file = \"%s\"\n",
                                tls ? "smtps" : "smtp", s->server, s->port, s->from, file);
    if (strcmp(s->security, "none") != 0) l += (size_t)snprintf(cfg + l, sizeof cfg - l, "ssl-reqd\n");
    if (s->user[0])
        l += (size_t)snprintf(cfg + l, sizeof cfg - l, "user = \"%s:%s\"\n", s->user, s->password);
    for (const char *p = s->to; *p && l < sizeof cfg;) {
        while (*p == ' ' || *p == ',') ++p;
        size_t n = strcspn(p, ", ");
        if (n) l += (size_t)snprintf(cfg + l, sizeof cfg - l, "mail-rcpt = \"%.*s\"\n", (int)n, p);
        p += n;
    }
    base_opts(cfg, sizeof cfg, QOS_ALARM);
    char out[512];
    int rc = run_curl(cfg, out, sizeof out);
    memset(cfg, 0, sizeof cfg);             /* it held the password */
    unlink(file);
    if (rc == 0) {
        snprintf(msg, cap, "sent to %s", s->to);
        return 0;
    }
    snprintf(msg, cap, "%s", out[0] ? out : "curl failed");
    if (rc == 127) snprintf(msg, cap, "this firmware has no curl");
    return -1;
}

static int mail_check(const config_smtp_t *s, char *msg, size_t cap)
{
    if (!curl_available()) {
        snprintf(msg, cap, "this firmware has no curl to send mail with");
        return -1;
    }
    if (!s->server[0] || !s->from[0] || !s->to[0]) {
        snprintf(msg, cap, "set the server, the sender and at least one address first");
        return -1;
    }
    return 0;
}

int mail_send(const config_smtp_t *s, const char *subject, const char *body, char *msg, size_t cap)
{
    if (mail_check(s, msg, cap) < 0) return -1;
    return mail_run(s, message(s, subject, body), msg, cap);
}

/* A file's bytes (malloc'd), at most `max`. NULL when it cannot be read. */
static unsigned char *read_file(const char *path, size_t max, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char *b = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 65536;
            if (nc > max + 1) nc = max + 1;
            if (nc <= cap) break;
            unsigned char *nb = realloc(b, nc);
            if (!nb) break;
            b = nb;
            cap = nc;
        }
        size_t r = fread(b + n, 1, cap - n, f);
        if (!r) break;
        n += r;
    }
    int bad = ferror(f) || n > max;
    fclose(f);
    if (bad) {
        free(b);
        return NULL;
    }
    *len = n;
    return b;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* base64 in lines of 76, CRLF (RFC 2045), appended at m + *len. */
static void put_b64(char *m, size_t *len, const unsigned char *p, size_t n)
{
    size_t col = 0, o = *len;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)p[i] << 16 | (i + 1 < n ? (unsigned)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        m[o++] = B64[v >> 18 & 63];
        m[o++] = B64[v >> 12 & 63];
        m[o++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
        m[o++] = i + 2 < n ? B64[v & 63] : '=';
        if ((col += 4) == 76) {
            m[o++] = '\r';
            m[o++] = '\n';
            col = 0;
        }
    }
    if (col) {
        m[o++] = '\r';
        m[o++] = '\n';
    }
    *len = o;
}

#define ATTACH_MAX (8u << 20)               /* per file */

int mail_send_files(const config_smtp_t *s, const char *subject, const char *body, const char *const *files, int n,
                    char *msg, size_t cap)
{
    if (mail_check(s, msg, cap) < 0) return -1;
    unsigned char *data[MAIL_FILES_MAX] = { 0 };
    size_t dlen[MAIL_FILES_MAX] = { 0 }, total = strlen(body) * 2 + 2048;
    if (n > MAIL_FILES_MAX) n = MAIL_FILES_MAX;
    for (int i = 0; i < n; ++i) {
        data[i] = read_file(files[i], ATTACH_MAX, &dlen[i]);
        if (!data[i]) {
            snprintf(msg, cap, "cannot read %s", files[i]);
            for (int k = 0; k < i; ++k) free(data[k]);
            return -1;
        }
        total += dlen[i] / 57 * 78 + 160 + 512;
    }
    char *m = malloc(total), date[64], boundary[48];
    if (!m) {
        for (int i = 0; i < n; ++i) free(data[i]);
        snprintf(msg, cap, "out of memory");
        return -1;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S %z", &tm);
    snprintf(boundary, sizeof boundary, "vigil-%ld-%d", (long)now, (int)getpid());
    size_t len = (size_t)snprintf(m, total,
                                  "From: %s\r\nTo: %s\r\nSubject: %s\r\nDate: %s\r\nMIME-Version: 1.0\r\n"
                                  "Content-Type: multipart/mixed; boundary=\"%s\"\r\n\r\n"
                                  "--%s\r\nContent-Type: text/plain; charset=UTF-8\r\n\r\n",
                                  s->from, s->to, subject, date, boundary, boundary);
    for (const char *p = body; *p && len + 3 < total; ++p) {
        if (*p == '\n' && (p == body || p[-1] != '\r')) m[len++] = '\r';
        m[len++] = *p;
    }
    len += (size_t)snprintf(m + len, total - len, "\r\n");
    for (int i = 0; i < n; ++i) {
        const char *base = strrchr(files[i], '/');
        base = base ? base + 1 : files[i];
        len += (size_t)snprintf(m + len, total - len,
                                "--%s\r\nContent-Type: image/jpeg; name=\"%.80s\"\r\n"
                                "Content-Disposition: attachment; filename=\"%.80s\"\r\n"
                                "Content-Transfer-Encoding: base64\r\n\r\n",
                                boundary, base, base);
        put_b64(m, &len, data[i], dlen[i]);
        free(data[i]);
    }
    len += (size_t)snprintf(m + len, total - len, "--%s--\r\n", boundary);
    m[len] = 0;
    return mail_run(s, m, msg, cap);
}

/* `tmpl` with {host}, {user} and {password} replaced. */
static void expand(const char *tmpl, const config_ddns_t *d, char *out, size_t cap)
{
    size_t len = 0;
    for (const char *p = tmpl; *p && len + 1 < cap;) {
        const char *v = NULL;
        size_t skip = 0;
        if (!strncmp(p, "{host}", 6)) v = d->host, skip = 6;
        else if (!strncmp(p, "{user}", 6)) v = d->user, skip = 6;
        else if (!strncmp(p, "{password}", 10)) v = d->password, skip = 10;
        if (v) {
            len += (size_t)snprintf(out + len, cap - len, "%s", v);
            if (len >= cap) len = cap - 1;
            p += skip;
        } else {
            out[len++] = *p++;
        }
    }
    out[len] = 0;
}

int ddns_update(const config_ddns_t *d, char *msg, size_t cap)
{
    if (!curl_available()) {
        snprintf(msg, cap, "this firmware has no curl");
        return -1;
    }
    if (!d->host[0]) {
        snprintf(msg, cap, "no host name to update");
        return -1;
    }
    char url[512];
    int custom = !strcmp(d->provider, "custom");
    if (custom) {
        if (!d->url[0]) {
            snprintf(msg, cap, "a custom provider needs its update URL");
            return -1;
        }
        expand(d->url, d, url, sizeof url);
    } else if (!strcmp(d->provider, "noip")) {
        snprintf(url, sizeof url, "https://dynupdate.no-ip.com/nic/update?hostname=%s", d->host);
    } else {
        snprintf(url, sizeof url, "https://members.dyndns.org/nic/update?hostname=%s", d->host);
    }
    char cfg[2048];
    size_t l = (size_t)snprintf(cfg, sizeof cfg, "url = \"%s\"\nfail\n", url);
    if (d->user[0] && (!custom || !strstr(d->url, "{password}")))
        snprintf(cfg + l, sizeof cfg - l, "user = \"%s:%s\"\n", d->user, d->password);
    base_opts(cfg, sizeof cfg, QOS_MANAGEMENT);
    char out[512];
    int rc = run_curl(cfg, out, sizeof out);
    memset(cfg, 0, sizeof cfg);
    memset(url, 0, sizeof url);
    if (rc != 0) {
        snprintf(msg, cap, "%s", out[0] ? out : "the update request failed");
        return -1;
    }
    /* dyndns2 answers in words: good / nochg are success; badauth,
     * nohost, notfqdn, abuse, 911, ... are not. */
    if (!custom && strncmp(out, "good", 4) && strncmp(out, "nochg", 5)) {
        snprintf(msg, cap, "the service answered: %.100s", out);
        return -1;
    }
    snprintf(msg, cap, "updated%s%.100s", out[0] ? ": " : "", out);
    return 0;
}

int http_post_json(const char *url, const char *json, char *msg, size_t cap)
{
    if (!curl_available()) {
        snprintf(msg, cap, "this firmware has no curl");
        return -1;
    }
    char file[512];
    if (temp_file("post", json, file, sizeof file) < 0) {
        snprintf(msg, cap, "cannot write to /tmp");
        return -1;
    }
    char cfg[1024];
    snprintf(cfg, sizeof cfg, "url = \"%s\"\nfail\nheader = \"Content-Type: application/json\"\n"
                              "data-binary = \"@%s\"\n", url, file);
    base_opts(cfg, sizeof cfg, QOS_ALARM);
    char out[256];
    int rc = run_curl(cfg, out, sizeof out);
    unlink(file);
    if (rc != 0) {
        snprintf(msg, cap, "%s", out[0] ? out : "the request failed");
        return -1;
    }
    snprintf(msg, cap, "sent");
    return 0;
}
