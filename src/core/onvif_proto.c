/* SPDX-License-Identifier: GPL-3.0-or-later */
/* onvif_proto.c — the ONVIF server's wire formats (see onvif_proto.h). */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "disc_proto.h"
#include "md5.h"
#include "onvif_proto.h"
#include "sha1.h"

static void wipe(void *p, size_t n)
{
    volatile unsigned char *w = p;
    while (n--) *w++ = 0;
}

int onvif_equal(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned char d = la != lb;
    for (size_t i = 0; i < la; ++i) d |= (unsigned char)(a[i] ^ b[i % (lb ? lb : 1)]);
    return d == 0;
}

int onvif_xml_escape(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (; *s; ++s) {
        const char *e = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;"
                      : *s == '"' ? "&quot;" : *s == '\'' ? "&apos;" : NULL;
        size_t n = e ? strlen(e) : 1;
        if (o + n >= cap) {
            if (cap) out[0] = 0;
            return -1;
        }
        if (e) memcpy(out + o, e, n);
        else out[o] = *s;
        o += n;
    }
    if (!cap) return -1;
    out[o] = 0;
    return 0;
}

/* ---- XML namespaces ------------------------------------------------------------- */

/* Is xml[i..] the attribute `name` (after white space), with a quoted
 * value? Copies the value. */
static int attr_at(const char *xml, size_t len, size_t i, const char *name, size_t nl, char *out, size_t cap)
{
    if (i == 0 || i + nl + 2 > len || memcmp(xml + i, name, nl) != 0) return 0;
    char before = xml[i - 1];
    if (before != ' ' && before != '\t' && before != '\r' && before != '\n') return 0;
    if (xml[i + nl] != '=' || (xml[i + nl + 1] != '"' && xml[i + nl + 1] != '\'')) return 0;
    const char *v = xml + i + nl + 2;
    const char *q = memchr(v, xml[i + nl + 1], len - (size_t)(v - xml));
    if (!q || (size_t)(q - v) >= cap) return 0;
    memcpy(out, v, (size_t)(q - v));
    out[q - v] = 0;
    return 1;
}

int onvif_xml_ns(const char *xml, size_t len, size_t at, const char *p, size_t plen, char *out, size_t cap)
{
    char name[80];
    if (cap) out[0] = 0;
    if (plen >= sizeof name - 7 || at >= len) return -1;
    memcpy(name, "xmlns", 5);
    size_t nl = 5;
    if (plen) {
        name[nl++] = ':';
        memcpy(name + nl, p, plen);
        nl += plen;
    }
    name[nl] = 0;
    const char *gt = memchr(xml + at, '>', len - at);
    size_t end = gt ? (size_t)(gt - xml) : len;
    for (size_t i = at; i < end; ++i)
        if (attr_at(xml, len, i, name, nl, out, cap)) return 0;
    for (size_t i = at; i-- > 0;)
        if (attr_at(xml, len, i, name, nl, out, cap)) return 0;
    return -1;
}

/* The first element's start inside xml[from, to): the offset of its '<',
 * or -1. Comments and processing instructions are skipped. */
static long first_elem(const char *xml, size_t from, size_t to)
{
    size_t i = from;
    while (i < to) {
        const char *lt = memchr(xml + i, '<', to - i);
        if (!lt) return -1;
        size_t k = (size_t)(lt - xml);
        if (k + 1 >= to) return -1;
        if (xml[k + 1] == '!' || xml[k + 1] == '?') {
            const char *gt = memchr(xml + k, '>', to - k);
            if (!gt) return -1;
            i = (size_t)(gt - xml) + 1;
            continue;
        }
        if (xml[k + 1] == '/') return -1;           /* the Body closes: empty */
        return (long)k;
    }
    return -1;
}

/* ---- SOAP ------------------------------------------------------------------------- */

int onvif_soap_parse(const char *xml, size_t len, onvif_soap_t *r)
{
    memset(r, 0, sizeof *r);
    size_t b0, b1;
    if (disc_xml_elem(xml, len, "Envelope", 0, NULL, NULL, NULL, NULL) < 0) return -1;
    if (disc_xml_elem(xml, len, "Body", 0, NULL, &b0, &b1, NULL) < 0) return -1;
    long lt = first_elem(xml, b0, b1);
    if (lt < 0) return -1;
    const char *n = xml + lt + 1, *ne = n;
    while (ne < xml + b1 && !strchr(" \t\r\n/>", *ne)) ++ne;
    const char *colon = memchr(n, ':', (size_t)(ne - n));
    const char *local = colon ? colon + 1 : n;
    size_t ll = (size_t)(ne - local);
    if (!ll || ll >= sizeof r->op) return -1;
    memcpy(r->op, local, ll);
    r->op[ll] = 0;
    onvif_xml_ns(xml, len, (size_t)lt, n, colon ? (size_t)(colon - n) : 0, r->ns, sizeof r->ns);
    size_t c0, c1;
    if (disc_xml_elem(xml, b1, r->op, (size_t)lt, NULL, &c0, &c1, NULL) < 0) return -1;
    r->arg0 = c0;
    r->arg1 = c1;

    size_t h0, h1, u0, u1;
    if (disc_xml_elem(xml, len, "Header", 0, NULL, &h0, &h1, NULL) == 0 && h1 <= b0 &&
        disc_xml_elem(xml + h0, h1 - h0, "UsernameToken", 0, NULL, &u0, &u1, NULL) == 0) {
        const char *u = xml + h0 + u0;
        size_t ul = u1 - u0;
        char type[160] = "";
        r->has_token = 1;
        disc_xml_text(u, ul, "Username", r->user, sizeof r->user, NULL);
        disc_xml_text(u, ul, "Password", r->password, sizeof r->password, NULL);
        disc_xml_attr(u, ul, "Password", "Type", type, sizeof type, NULL);
        disc_xml_text(u, ul, "Nonce", r->nonce, sizeof r->nonce, NULL);
        disc_xml_text(u, ul, "Created", r->created, sizeof r->created, NULL);
        /* No Type is PasswordText (UsernameToken Profile 1.0, 3.1). */
        size_t tl = strlen(type);
        r->digest = tl >= 15 && !strcmp(type + tl - 15, "#PasswordDigest");
    }
    return 0;
}

/* ---- WS-UsernameToken -------------------------------------------------------------- */

static int b64_decode(const char *in, unsigned char *out, size_t cap)
{
    unsigned acc = 0;
    int bits = 0;
    size_t n = 0;
    for (; *in && *in != '='; ++in) {
        int c = (unsigned char)*in, v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (isspace(c)) continue;
        else return -1;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n == cap) return -1;
            out[n++] = (unsigned char)(acc >> bits);
        }
    }
    return (int)n;
}

int onvif_wsse_digest(const char *nonce_b64, const char *created, const char *pass, char out[32])
{
    unsigned char nonce[96], dig[20];
    out[0] = 0;
    int nl = b64_decode(nonce_b64, nonce, sizeof nonce);
    if (nl <= 0) return -1;
    size_t cl = strlen(created), pl = strlen(pass), tl = (size_t)nl + cl + pl;
    unsigned char *buf = malloc(tl ? tl : 1);
    if (!buf) return -1;
    memcpy(buf, nonce, (size_t)nl);
    memcpy(buf + nl, created, cl);
    memcpy(buf + (size_t)nl + cl, pass, pl);
    sha1_raw(buf, tl, dig);
    wipe(buf, tl);
    free(buf);
    return base64_encode(dig, sizeof dig, out, 32);
}

static int digits(const char **p, int n, int *v)
{
    *v = 0;
    for (int i = 0; i < n; ++i, ++*p) {
        if (!isdigit((unsigned char)**p)) return -1;
        *v = *v * 10 + (**p - '0');
    }
    return 0;
}

/* Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's
 * days_from_civil). */
static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int onvif_time_parse(const char *s, time_t *t)
{
    int Y, M, D, h, m, sec, oh = 0, om = 0, sign = 0;
    const char *p = s;
    while (isspace((unsigned char)*p)) ++p;
    if (digits(&p, 4, &Y) || *p++ != '-' || digits(&p, 2, &M) || *p++ != '-' || digits(&p, 2, &D) ||
        (*p != 'T' && *p != 't') || (++p, digits(&p, 2, &h)) || *p++ != ':' || digits(&p, 2, &m) ||
        *p++ != ':' || digits(&p, 2, &sec))
        return -1;
    if (*p == '.')
        for (++p; isdigit((unsigned char)*p); ++p) {}
    if (*p == 'Z' || *p == 'z') ++p;
    else if (*p == '+' || *p == '-') {
        sign = *p++ == '-' ? -1 : 1;
        if (digits(&p, 2, &oh) || *p++ != ':' || digits(&p, 2, &om)) return -1;
    }
    while (isspace((unsigned char)*p)) ++p;
    if (*p || M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || m > 59 || sec > 60 || oh > 14 || om > 59)
        return -1;
    long long v = (long long)days_from_civil(Y, M, D) * 86400 + h * 3600 + m * 60 + sec;
    v -= sign * (oh * 3600 + om * 60);
    *t = (time_t)v;
    return 0;
}

/* ---- HTTP Digest ------------------------------------------------------------------ */

int onvif_digest_parse(const char *hdr, onvif_digest_t *d)
{
    memset(d, 0, sizeof *d);
    if (!hdr || strncasecmp(hdr, "Digest", 6) != 0 || !isspace((unsigned char)hdr[6])) return -1;
    struct { const char *k; char *v; size_t cap; } F[] = {
        { "username", d->username, sizeof d->username }, { "realm", d->realm, sizeof d->realm },
        { "nonce", d->nonce, sizeof d->nonce }, { "uri", d->uri, sizeof d->uri },
        { "response", d->response, sizeof d->response }, { "qop", d->qop, sizeof d->qop },
        { "nc", d->nc, sizeof d->nc }, { "cnonce", d->cnonce, sizeof d->cnonce },
        { "algorithm", d->algorithm, sizeof d->algorithm }, { "opaque", d->opaque, sizeof d->opaque },
    };
    const char *p = hdr + 7;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == ',') ++p;
        if (!*p) break;
        const char *k = p;
        while (isalnum((unsigned char)*p) || *p == '_' || *p == '-') ++p;
        size_t kl = (size_t)(p - k);
        while (*p == ' ' || *p == '\t') ++p;
        if (!kl || *p++ != '=') return -1;
        while (*p == ' ' || *p == '\t') ++p;
        char val[300];
        size_t vl = 0;
        if (*p == '"') {
            for (++p; *p && *p != '"'; ++p) {
                if (*p == '\\' && p[1]) ++p;
                if (vl + 1 >= sizeof val) return -1;
                val[vl++] = *p;
            }
            if (*p++ != '"') return -1;
        } else {
            while (*p && *p != ',' && *p != ' ' && *p != '\t') {
                if (vl + 1 >= sizeof val) return -1;
                val[vl++] = *p++;
            }
        }
        val[vl] = 0;
        for (size_t i = 0; i < sizeof F / sizeof F[0]; ++i) {
            if (strlen(F[i].k) != kl || strncasecmp(F[i].k, k, kl) != 0) continue;
            if (vl >= F[i].cap) return -1;
            memcpy(F[i].v, val, vl + 1);
        }
    }
    if (!d->username[0] || !d->nonce[0] || !d->uri[0] || !d->response[0]) return -1;
    if (d->algorithm[0] && strcasecmp(d->algorithm, "MD5") != 0) return -1;
    if (d->qop[0] && (strcmp(d->qop, "auth") != 0 || !d->nc[0] || !d->cnonce[0])) return -1;
    return 0;
}

void onvif_digest_expect(const onvif_digest_t *d, const char *method, const char *pass, char out[33])
{
    char a1[512], a2[320], ha1[33], ha2[33], all[400];
    int n = snprintf(a1, sizeof a1, "%s:%s:%s", d->username, d->realm, pass);
    md5_hex(a1, n < 0 ? 0 : (size_t)n >= sizeof a1 ? sizeof a1 - 1 : (size_t)n, ha1);
    wipe(a1, sizeof a1);
    snprintf(a2, sizeof a2, "%s:%s", method, d->uri);
    md5_hex(a2, strlen(a2), ha2);
    if (d->qop[0])
        snprintf(all, sizeof all, "%s:%s:%s:%s:%s:%s", ha1, d->nonce, d->nc, d->cnonce, d->qop, ha2);
    else
        snprintf(all, sizeof all, "%s:%s:%s", ha1, d->nonce, ha2);
    md5_hex(all, strlen(all), out);
    wipe(ha1, sizeof ha1);
    wipe(all, sizeof all);
}

static void nonce_mac(const unsigned char key[16], const char *ts, char out[25])
{
    unsigned char buf[16 + 9];
    char hex[33];
    memcpy(buf, key, 16);
    memcpy(buf + 16, ts, 8);
    buf[24] = 0;
    md5_hex(buf, 24, hex);
    memcpy(out, hex, 24);
    out[24] = 0;
}

void onvif_digest_nonce(const unsigned char key[16], long t, char out[33])
{
    char mac[25];
    snprintf(out, 9, "%08lx", (unsigned long)t & 0xffffffffUL);
    nonce_mac(key, out, mac);
    memcpy(out + 8, mac, 25);
}

long onvif_digest_nonce_age(const unsigned char key[16], const char *nonce, long now)
{
    if (strlen(nonce) != 32) return -1;
    char ts[9], mac[25];
    memcpy(ts, nonce, 8);
    ts[8] = 0;
    for (int i = 0; i < 8; ++i)
        if (!isxdigit((unsigned char)ts[i])) return -1;
    nonce_mac(key, ts, mac);
    if (!onvif_equal(mac, nonce + 8)) return -1;
    long t = (long)strtoul(ts, NULL, 16);
    return now >= t ? now - t : -1;
}

/* ---- WS-Discovery ----------------------------------------------------------------- */

#define WSD_NS   "http://schemas.xmlsoap.org/ws/2005/04/discovery"
#define WSA_NS   "http://schemas.xmlsoap.org/ws/2004/08/addressing"

int onvif_wsd_probe_parse(const char *xml, size_t len, onvif_probe_t *p)
{
    memset(p, 0, sizeof *p);
    size_t b0, b1, tag, c0, c1;
    if (disc_xml_elem(xml, len, "Body", 0, NULL, &b0, &b1, NULL) < 0) return -1;
    long lt = first_elem(xml, b0, b1);
    if (lt < 0) return -1;
    /* The Body's element is a Probe: <d:Probe ...>. */
    const char *n = xml + lt + 1, *ne = n;
    while (ne < xml + b1 && !strchr(" \t\r\n/>", *ne)) ++ne;
    const char *colon = memchr(n, ':', (size_t)(ne - n));
    const char *local = colon ? colon + 1 : n;
    if ((size_t)(ne - local) != 5 || memcmp(local, "Probe", 5) != 0) return -1;
    if (disc_xml_text(xml, b0, "MessageID", p->msgid, sizeof p->msgid, NULL) < 0) return -1;
    size_t pc0, pc1;
    if (disc_xml_elem(xml, b1, "Probe", (size_t)lt, NULL, &pc0, &pc1, NULL) < 0) return -1;
    if (disc_xml_elem(xml, pc1, "Types", pc0, &tag, &c0, &c1, NULL) == 0) {
        p->types_at = tag;
        if (disc_xml_text(xml + tag, pc1 - tag, "Types", p->types, sizeof p->types, NULL) < 0) return -1;
    }
    if (disc_xml_elem(xml, pc1, "Scopes", pc0, &tag, &c0, &c1, NULL) == 0) {
        if (disc_xml_text(xml + tag, pc1 - tag, "Scopes", p->scopes, sizeof p->scopes, NULL) < 0) return -1;
        disc_xml_attr(xml + tag, pc1 - tag, "Scopes", "MatchBy", p->match_by, sizeof p->match_by, NULL);
    }
    return 0;
}

static int type_ok(const char *xml, size_t len, size_t at, const char *q, size_t ql)
{
    const char *colon = memchr(q, ':', ql);
    const char *local = colon ? colon + 1 : q;
    size_t ll = ql - (size_t)(local - q);
    const char *want;
    if (ll == 23 && !memcmp(local, "NetworkVideoTransmitter", 23)) want = ONVIF_NS_NETWORK;
    else if (ll == 6 && !memcmp(local, "Device", 6)) want = ONVIF_NS_DEVICE;
    else return 0;
    /* A prefix nobody declared: by its local name, as most devices do. */
    char ns[160];
    if (onvif_xml_ns(xml, len, at, q, colon ? (size_t)(colon - q) : 0, ns, sizeof ns) < 0) return 1;
    return !strcmp(ns, want);
}

int onvif_scope_match(const char *s, size_t sl, const char *t, size_t tl)
{
    /* scheme://authority: case-insensitive, and the whole of it. */
    size_t as = 0, at = 0;
    for (size_t i = 0; i + 3 <= sl; ++i)
        if (!memcmp(s + i, "://", 3)) {
            const char *sl2 = memchr(s + i + 3, '/', sl - i - 3);
            as = sl2 ? (size_t)(sl2 - s) : sl;
            break;
        }
    for (size_t i = 0; i + 3 <= tl; ++i)
        if (!memcmp(t + i, "://", 3)) {
            const char *tl2 = memchr(t + i + 3, '/', tl - i - 3);
            at = tl2 ? (size_t)(tl2 - t) : tl;
            break;
        }
    if (as != at || strncasecmp(s, t, as) != 0) return 0;
    /* Then the path, by whole segments: "/name" matches "/name/x", not "/namex". */
    const char *ps = s + as, *pt = t + at;
    size_t pls = sl - as, plt = tl - at;
    while (pls && ps[pls - 1] == '/') --pls;
    if (pls > plt || memcmp(ps, pt, pls) != 0) return 0;
    return pls == plt || pt[pls] == '/';
}

int onvif_wsd_match(const char *xml, size_t len, const onvif_probe_t *p, const char *scopes)
{
    for (const char *q = p->types; *q;) {
        while (*q && isspace((unsigned char)*q)) ++q;
        size_t n = strcspn(q, " \t\r\n");
        if (n && !type_ok(xml, len, p->types_at, q, n)) return 0;
        q += n;
    }
    int exact;
    if (!p->match_by[0] || !strcmp(p->match_by, WSD_NS "/rfc3986")) exact = 0;
    else if (!strcmp(p->match_by, WSD_NS "/strcmp0")) exact = 1;
    else return 0;                                   /* a rule we do not have */
    for (const char *q = p->scopes; *q;) {
        while (*q && isspace((unsigned char)*q)) ++q;
        size_t n = strcspn(q, " \t\r\n");
        if (!n) break;
        int found = 0;
        for (const char *t = scopes; *t && !found;) {
            while (*t == ' ') ++t;
            size_t m = strcspn(t, " ");
            if (m) found = exact ? (m == n && !memcmp(q, t, n)) : onvif_scope_match(q, n, t, m);
            t += m;
        }
        if (!found) return 0;
        q += n;
    }
    return 1;
}

#define WSD_HEAD                                                                                     \
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"                                                     \
    "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" xmlns:a=\"" WSA_NS "\""        \
    " xmlns:d=\"" WSD_NS "\" xmlns:dn=\"" ONVIF_NS_NETWORK "\" xmlns:tds=\"" ONVIF_NS_DEVICE "\">"   \
    "<s:Header><a:MessageID>%s</a:MessageID>"

static size_t done(int n, size_t cap) { return n > 0 && (size_t)n < cap ? (size_t)n : 0; }

/* EndpointReference, Types, Scopes, XAddrs, MetadataVersion. */
#define WSD_MATCH                                                                                    \
    "<a:EndpointReference><a:Address>%s</a:Address></a:EndpointReference>"                           \
    "<d:Types>dn:NetworkVideoTransmitter tds:Device</d:Types><d:Scopes>%s</d:Scopes>"                \
    "<d:XAddrs>%s</d:XAddrs><d:MetadataVersion>%u</d:MetadataVersion>"

size_t onvif_wsd_probe_matches(char *buf, size_t cap, const onvif_wsd_t *w, const char *msgid,
                               const char *relates_to)
{
    char rel[200];
    if (onvif_xml_escape(relates_to, rel, sizeof rel) < 0) return 0;
    int n = snprintf(buf, cap,
        WSD_HEAD "<a:RelatesTo>%s</a:RelatesTo>"
        "<a:To>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</a:To>"
        "<a:Action>" WSD_NS "/ProbeMatches</a:Action>"
        "<d:AppSequence InstanceId=\"%u\" MessageNumber=\"%u\"/></s:Header>"
        "<s:Body><d:ProbeMatches><d:ProbeMatch>" WSD_MATCH "</d:ProbeMatch></d:ProbeMatches></s:Body></s:Envelope>",
        msgid, rel, w->instance, w->msgno, w->uuid, w->scopes, w->xaddrs, w->metadata);
    return done(n, cap);
}

size_t onvif_wsd_hello(char *buf, size_t cap, const onvif_wsd_t *w, const char *msgid)
{
    int n = snprintf(buf, cap,
        WSD_HEAD "<a:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</a:To>"
        "<a:Action>" WSD_NS "/Hello</a:Action>"
        "<d:AppSequence InstanceId=\"%u\" MessageNumber=\"%u\"/></s:Header>"
        "<s:Body><d:Hello>" WSD_MATCH "</d:Hello></s:Body></s:Envelope>",
        msgid, w->instance, w->msgno, w->uuid, w->scopes, w->xaddrs, w->metadata);
    return done(n, cap);
}

size_t onvif_wsd_bye(char *buf, size_t cap, const onvif_wsd_t *w, const char *msgid)
{
    int n = snprintf(buf, cap,
        WSD_HEAD "<a:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</a:To>"
        "<a:Action>" WSD_NS "/Bye</a:Action>"
        "<d:AppSequence InstanceId=\"%u\" MessageNumber=\"%u\"/></s:Header>"
        "<s:Body><d:Bye><a:EndpointReference><a:Address>%s</a:Address></a:EndpointReference>"
        "</d:Bye></s:Body></s:Envelope>",
        msgid, w->instance, w->msgno, w->uuid);
    return done(n, cap);
}

void onvif_scope_encode(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    if (!cap) return;
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        int plain = isalnum(c) || strchr("-._~", c);
        if (o + (plain ? 1 : 3) >= cap) break;
        if (plain) out[o++] = (char)c;
        else o += (size_t)snprintf(out + o, cap - o, "%%%02X", c);
    }
    out[o] = 0;
}
