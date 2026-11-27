/* SPDX-License-Identifier: GPL-3.0-or-later */
/* disc_proto.c — discovery wire formats (see disc_proto.h). Everything
 * here reads bytes from the network: lengths are checked before every
 * access, and text is copied only into buffers it fits. */
#define _DEFAULT_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "disc_proto.h"
#include "md5.h"

#define JSON_MAX (16u << 10)        /* a NetCommon or a deviceInfo is ~1-2 KiB */

const char *disc_proto_name(unsigned proto)
{
    return proto == DISC_NETIP ? "netip" : proto == DISC_ONVIF ? "onvif" : proto == DISC_DHIP ? "dhip" : "";
}

unsigned disc_proto_bits(const char *list)
{
    unsigned bits = 0;
    for (const char *p = list; *p; ) {
        while (*p == ',' || *p == ' ') ++p;
        size_t n = strcspn(p, ", ");
        if (!n) break;
        if (n == 3 && !strncasecmp(p, "all", 3)) bits |= DISC_ALL;
        else if (n == 5 && !strncasecmp(p, "netip", 5)) bits |= DISC_NETIP;
        else if (n == 5 && !strncasecmp(p, "onvif", 5)) bits |= DISC_ONVIF;
        else if (n == 4 && !strncasecmp(p, "dhip", 4)) bits |= DISC_DHIP;
        else return 0;
        p += n;
    }
    return bits;
}

static void copy(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src ? src : "");
}

static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static const char *jstr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : NULL;
}

static int jint(const cJSON *o, const char *key, int dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= 65535) return (int)v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring) {
        char *end;
        long n = strtol(v->valuestring, &end, 0);
        if (end != v->valuestring && !*end && n >= 0 && n <= 65535) return (int)n;
    }
    return dflt;
}

/* Printable text only: names come from devices and end up on the screen
 * and in JSON. Control bytes become '?'. */
static void copy_text(char *dst, size_t cap, const char *src)
{
    copy(dst, cap, src);
    for (char *p = dst; *p; ++p)
        if ((unsigned char)*p < 0x20 || *p == 0x7f) *p = '?';
}

static cJSON *parse_json(const uint8_t *p, size_t n)
{
    while (n && (p[n - 1] == 0 || p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' ')) --n;
    if (!n || n > JSON_MAX) return NULL;
    return cJSON_ParseWithLength((const char *)p, n);
}

/* ---- IPv4 --------------------------------------------------------------------- */

int disc_ip_parse(const char *s, uint32_t *v)
{
    unsigned a[4];
    int used = 0;
    if (!s || sscanf(s, "%3u.%3u.%3u.%3u%n", &a[0], &a[1], &a[2], &a[3], &used) != 4 || s[used]) return -1;
    for (int i = 0; i < 4; ++i)
        if (a[i] > 255) return -1;
    /* sscanf takes "+1" and " 1": only digits and dots. */
    for (const char *p = s; *p; ++p)
        if (!isdigit((unsigned char)*p) && *p != '.') return -1;
    *v = a[0] << 24 | a[1] << 16 | a[2] << 8 | a[3];
    return 0;
}

void disc_ip_str(uint32_t v, char *out, size_t cap)
{
    snprintf(out, cap, "%u.%u.%u.%u", v >> 24, v >> 16 & 255, v >> 8 & 255, v & 255);
}

int disc_mask_prefix(const char *mask)
{
    uint32_t m;
    if (disc_ip_parse(mask, &m) < 0) return -1;
    int n = 0;
    while (n < 32 && (m & (0x80000000u >> n))) ++n;
    if (n < 32 && (m << n)) return -1;           /* not contiguous */
    return n;
}

/* ---- NETIP --------------------------------------------------------------------- */

static size_t netip_hdr(uint8_t *buf, int msgid, size_t body)
{
    memset(buf, 0, 20);
    buf[0] = 0xff;
    buf[14] = msgid & 0xff;
    buf[15] = msgid >> 8 & 0xff;
    put32(buf + 16, (uint32_t)body);
    return 20;
}

size_t disc_netip_search(uint8_t *buf, size_t cap)
{
    if (cap < 20) return 0;
    return netip_hdr(buf, DISC_NETIP_SEARCH, 0);
}

int disc_netip_hexip(const char *hex, char *out, size_t cap)
{
    if (!hex || strncasecmp(hex, "0x", 2) != 0) return -1;
    char *end;
    unsigned long v = strtoul(hex + 2, &end, 16);
    if (end == hex + 2 || *end || end - hex > 10) return -1;
    /* The address in memory order on a little-endian CPU: the first
     * octet is the low byte. */
    snprintf(out, cap, "%lu.%lu.%lu.%lu", v & 255, v >> 8 & 255, v >> 16 & 255, v >> 24 & 255);
    return 0;
}

int disc_netip_iphex(const char *ip, char *out, size_t cap)
{
    uint32_t v;
    if (disc_ip_parse(ip, &v) < 0) return -1;
    uint32_t le = (v >> 24) | (v >> 8 & 0xff00) | (v << 8 & 0xff0000) | v << 24;
    snprintf(out, cap, "0x%08X", le);
    return 0;
}

/* The NETIP body: the header's length, bounded by what arrived. */
static cJSON *netip_body(const uint8_t *pkt, size_t len, int msgid)
{
    if (len < 20 || pkt[0] != 0xff) return NULL;
    if ((pkt[14] | pkt[15] << 8) != msgid) return NULL;
    size_t bl = get32(pkt + 16);
    if (bl > len - 20) bl = len - 20;            /* some devices count the trailing "\n\0" they leave off */
    return parse_json(pkt + 20, bl);
}

int disc_netip_parse(const uint8_t *pkt, size_t len, disc_device_t *d)
{
    memset(d, 0, sizeof *d);
    d->dhcp = -1;
    cJSON *doc = netip_body(pkt, len, DISC_NETIP_SEARCH_REP);
    if (!doc) return -1;
    const cJSON *nc = cJSON_GetObjectItemCaseSensitive(doc, "NetWork.NetCommon");
    int rc = -1;
    if (cJSON_IsObject(nc) && disc_netip_hexip(jstr(nc, "HostIP"), d->ip, sizeof d->ip) == 0) {
        if (disc_netip_hexip(jstr(nc, "Submask"), d->mask, sizeof d->mask) < 0) d->mask[0] = 0;
        if (disc_netip_hexip(jstr(nc, "GateWay"), d->gateway, sizeof d->gateway) < 0) d->gateway[0] = 0;
        disc_mac_norm(jstr(nc, "MAC") ? jstr(nc, "MAC") : "", d->mac, sizeof d->mac);
        d->netip_port = jint(nc, "TCPPort", 34567);
        d->http_port = jint(nc, "HttpPort", 80);
        d->channels = jint(nc, "ChannelNum", 0);
        copy_text(d->serial, sizeof d->serial, jstr(nc, "SN"));
        copy_text(d->name, sizeof d->name, jstr(nc, "HostName"));
        copy_text(d->version, sizeof d->version, jstr(nc, "Version"));
        /* "DeviceType" is a number on some firmwares (1 IPC, 2 DVR, ...);
         * the text form names the model. */
        const char *dt = jstr(nc, "DeviceType");
        if (dt) copy_text(d->model, sizeof d->model, dt);
        else if (jint(nc, "DeviceType", -1) >= 0) {
            int t = jint(nc, "DeviceType", -1);
            copy(d->model, sizeof d->model, t == 1 ? "IPC" : t == 2 ? "DVR" : t == 3 ? "NVR" : "");
        }
        copy(d->vendor, sizeof d->vendor, "Xiongmai");
        d->protos = DISC_NETIP;
        rc = 0;
    }
    cJSON_Delete(doc);
    return rc;
}

size_t disc_netip_set(uint8_t *buf, size_t cap, const disc_device_t *d, const char *ip,
                      const char *mask, const char *gw, const char *user, const char *pass)
{
    char hip[16], hmask[16], hgw[16], hash[9];
    if (!d->mac[0] || disc_netip_iphex(ip, hip, sizeof hip) < 0 ||
        disc_netip_iphex(mask, hmask, sizeof hmask) < 0 || disc_netip_iphex(gw, hgw, sizeof hgw) < 0)
        return 0;
    md5_sofia(pass ? pass : "", hash);
    cJSON *o = cJSON_CreateObject();
    if (!o) return 0;
    /* What ConfigXM sends: the device's own ports and name back, the new
     * addresses, the MAC as "DvrMac" (which device), the credentials. */
    cJSON_AddStringToObject(o, "DvrMac", d->mac);
    cJSON_AddNumberToObject(o, "EncryptType", 1);
    cJSON_AddStringToObject(o, "GateWay", hgw);
    cJSON_AddStringToObject(o, "HostIP", hip);
    if (d->name[0]) cJSON_AddStringToObject(o, "HostName", d->name);
    cJSON_AddNumberToObject(o, "HttpPort", d->http_port ? d->http_port : 80);
    cJSON_AddStringToObject(o, "MAC", d->mac);
    cJSON_AddStringToObject(o, "Password", hash);
    cJSON_AddStringToObject(o, "Submask", hmask);
    cJSON_AddNumberToObject(o, "TCPPort", d->netip_port ? d->netip_port : 34567);
    cJSON_AddStringToObject(o, "Username", user && *user ? user : "admin");
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!text) return 0;
    size_t tl = strlen(text), total = 20 + tl + 2;
    if (total > cap) {
        free(text);
        return 0;
    }
    netip_hdr(buf, DISC_NETIP_SET, tl + 2);
    buf[2] = 254;                               /* as ConfigXM sends it */
    memcpy(buf + 20, text, tl);
    buf[20 + tl] = '\n';
    buf[21 + tl] = 0;
    memset(text, 0, tl);
    free(text);
    return total;
}

int disc_netip_set_reply(const uint8_t *pkt, size_t len)
{
    cJSON *doc = netip_body(pkt, len, DISC_NETIP_SET_REP);
    if (!doc) return -1;
    int ret = jint(doc, "Ret", -1);
    cJSON_Delete(doc);
    return ret;
}

/* ---- XML (just enough for WS-Discovery and a few SOAP answers) ----------------- */

/* Does the tag name at p (after '<' or '</') have local name `local`?
 * Returns the end of the name, or NULL. */
static const char *tag_is(const char *p, const char *end, const char *local)
{
    const char *n = p;
    while (n < end && !strchr(" \t\r\n/>", *n)) ++n;
    const char *colon = NULL;
    for (const char *q = p; q < n; ++q)
        if (*q == ':') colon = q;
    const char *ln = colon ? colon + 1 : p;
    size_t ll = strlen(local);
    return (size_t)(n - ln) == ll && !memcmp(ln, local, ll) ? n : NULL;
}

/* Finds the next element `local` at or after xml+*pos: the offsets of its
 * start tag's attributes [*a0,*a1), its content [*c0,*c1) and the end of
 * the element. -1 if none. */
static int find_elem(const char *xml, size_t len, const char *local, size_t start, size_t *a0, size_t *a1,
                     size_t *c0, size_t *c1, size_t *after)
{
    const char *end = xml + len;
    for (const char *p = xml + start; p < end; ) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt || lt + 1 >= end) return -1;
        const char *ne = tag_is(lt + 1, end, local);
        if (!ne || lt[1] == '/' || lt[1] == '?' || lt[1] == '!') {
            p = lt + 1;
            continue;
        }
        const char *gt = memchr(ne, '>', (size_t)(end - ne));
        if (!gt) return -1;
        *a0 = (size_t)(ne - xml);
        *a1 = (size_t)(gt - xml);
        if (gt > ne && gt[-1] == '/') {            /* <x/> */
            *a1 = (size_t)(gt - 1 - xml);
            *c0 = *c1 = (size_t)(gt + 1 - xml);
            *after = *c1;
            return 0;
        }
        /* The matching close tag; nested elements of the same name are
         * not expected in these messages, and the first close wins. */
        for (const char *q = gt + 1; q < end; ) {
            const char *cl = memchr(q, '<', (size_t)(end - q));
            if (!cl || cl + 2 > end) return -1;
            const char *e2;
            if (cl[1] == '/' && (e2 = tag_is(cl + 2, end, local)) != NULL) {
                const char *g2 = memchr(e2, '>', (size_t)(end - e2));
                if (!g2) return -1;
                *c0 = (size_t)(gt + 1 - xml);
                *c1 = (size_t)(cl - xml);
                *after = (size_t)(g2 + 1 - xml);
                return 0;
            }
            q = cl + 1;
        }
        return -1;
    }
    return -1;
}

/* Copy xml[a..b) into out, entities decoded and white space trimmed. */
static int unescape(const char *s, size_t n, char *out, size_t cap)
{
    while (n && isspace((unsigned char)*s)) ++s, --n;
    while (n && isspace((unsigned char)s[n - 1])) --n;
    size_t o = 0;
    for (size_t i = 0; i < n; ) {
        static const struct { const char *e; char c; } E[] = {
            { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' },
        };
        char c = s[i];
        size_t step = 1;
        if (c == '&')
            for (size_t k = 0; k < sizeof E / sizeof E[0]; ++k) {
                size_t el = strlen(E[k].e);
                if (n - i >= el && !memcmp(s + i, E[k].e, el)) {
                    c = E[k].c;
                    step = el;
                    break;
                }
            }
        if (o + 1 >= cap) return -1;
        out[o++] = ((unsigned char)c < 0x20 && c != '\t' && c != '\n' && c != '\r') ? '?' : c;
        i += step;
    }
    out[o] = 0;
    return 0;
}

int disc_xml_text(const char *xml, size_t len, const char *local, char *out, size_t cap, size_t *from)
{
    size_t a0, a1, c0, c1, after;
    if (cap) out[0] = 0;
    if (find_elem(xml, len, local, from ? *from : 0, &a0, &a1, &c0, &c1, &after) < 0) return -1;
    if (from) *from = after;
    return unescape(xml + c0, c1 - c0, out, cap);
}

int disc_xml_attr(const char *xml, size_t len, const char *local, const char *attr, char *out,
                  size_t cap, size_t *from)
{
    size_t a0, a1, c0, c1, after;
    if (cap) out[0] = 0;
    if (find_elem(xml, len, local, from ? *from : 0, &a0, &a1, &c0, &c1, &after) < 0) return -1;
    if (from) *from = a1;                        /* the content may hold more elements */
    size_t al = strlen(attr);
    for (size_t i = a0; i + al + 2 <= a1; ++i) {
        const char *p = xml + i;
        if ((p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == ':') && !memcmp(p, attr, al) &&
            p[al] == '=' && (p[al + 1] == '"' || p[al + 1] == '\'')) {
            const char *v = p + al + 2;
            const char *q = memchr(v, p[al + 1], (size_t)(xml + a1 - v));
            if (!q) return -1;
            return unescape(v, (size_t)(q - v), out, cap);
        }
    }
    return -1;
}

size_t disc_onvif_probe(char *buf, size_t cap, const char *msgid)
{
    int n = snprintf(buf, cap,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
        " xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\""
        " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
        "<s:Header><a:MessageID>%s</a:MessageID>"
        "<a:To s:mustUnderstand=\"true\">urn:schemas-xmlsoap-org:ws:2005:04:discovery</a:To>"
        "<a:Action s:mustUnderstand=\"true\">http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</a:Action>"
        "</s:Header><s:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></s:Body>"
        "</s:Envelope>", msgid);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

/* %XX in a scope value ("onvif://www.onvif.org/name/Front%20door"). */
static void pct_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; ++p) {
        if (p[0] == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char h[3] = { p[1], p[2], 0 };
            char c = (char)strtol(h, NULL, 16);
            *o++ = (unsigned char)c < 0x20 ? '?' : c;
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = 0;
}

/* The value of scope ".../<key>/<value>" (key case-insensitive). */
static void scope_value(const char *scopes, const char *key, char *out, size_t cap)
{
    char pat[48];
    snprintf(pat, sizeof pat, "onvif://www.onvif.org/%s/", key);
    size_t pl = strlen(pat);
    for (const char *p = scopes; *p; ) {
        while (*p && isspace((unsigned char)*p)) ++p;
        size_t n = strcspn(p, " \t\r\n");
        if (n > pl && !strncasecmp(p, pat, pl)) {
            size_t vl = n - pl;
            if (vl >= cap) vl = cap - 1;
            memcpy(out, p + pl, vl);
            out[vl] = 0;
            pct_decode(out);
            return;
        }
        p += n;
    }
}

/* "http://10.0.0.5:8899/onvif/device_service" -> host, port. */
static int http_hostport(const char *url, char *host, size_t hcap, int *port)
{
    if (strncasecmp(url, "http://", 7) != 0) return -1;
    const char *h = url + 7;
    size_t n = strcspn(h, ":/");
    if (!n || n >= hcap) return -1;
    memcpy(host, h, n);
    host[n] = 0;
    *port = h[n] == ':' ? atoi(h + n + 1) : 80;
    return *port > 0 && *port < 65536 ? 0 : -1;
}

int disc_onvif_parse(const char *xml, size_t len, disc_device_t *d)
{
    memset(d, 0, sizeof *d);
    d->dhcp = -1;
    size_t a0, a1, c0, c1, after;
    if (find_elem(xml, len, "ProbeMatch", 0, &a0, &a1, &c0, &c1, &after) < 0) return -1;
    const char *m = xml + c0;
    size_t ml = c1 - c0;
    char xaddrs[512], scopes[1024];
    if (disc_xml_text(m, ml, "XAddrs", xaddrs, sizeof xaddrs, NULL) < 0 || !xaddrs[0]) return -1;
    /* The first http:// address with an IPv4 host; devices list one per
     * interface (and IPv6 ones in brackets). */
    for (const char *p = xaddrs; *p; ) {
        while (*p && isspace((unsigned char)*p)) ++p;
        size_t n = strcspn(p, " \t\r\n");
        char u[160], host[64];
        int port;
        uint32_t v;
        if (n && n < sizeof u) {
            memcpy(u, p, n);
            u[n] = 0;
            if (http_hostport(u, host, sizeof host, &port) == 0 && disc_ip_parse(host, &v) == 0) {
                copy(d->xaddr, sizeof d->xaddr, u);
                copy(d->ip, sizeof d->ip, host);
                d->http_port = port;
                break;
            }
        }
        p += n;
    }
    if (!d->ip[0]) return -1;
    disc_xml_text(m, ml, "Address", d->uuid, sizeof d->uuid, NULL);
    if (disc_xml_text(m, ml, "Scopes", scopes, sizeof scopes, NULL) == 0) {
        char mac[32] = "";
        scope_value(scopes, "name", d->name, sizeof d->name);
        scope_value(scopes, "hardware", d->model, sizeof d->model);
        scope_value(scopes, "manufacturer", d->vendor, sizeof d->vendor);
        scope_value(scopes, "mac", mac, sizeof mac);
        disc_mac_norm(mac, d->mac, sizeof d->mac);
    }
    d->protos = DISC_ONVIF;
    return 0;
}

/* ---- DHIP ------------------------------------------------------------------------- */

#define DHIP_MAGIC 0x50494844u               /* "DHIP" */

size_t disc_dhip_search(uint8_t *buf, size_t cap)
{
    static const char body[] = "{\"method\":\"DHDiscover.search\",\"params\":{\"mac\":\"\",\"uni\":1}}";
    size_t bl = sizeof body - 1;
    if (cap < 32 + bl) return 0;
    memset(buf, 0, 32);
    put32(buf, 32);
    put32(buf + 4, DHIP_MAGIC);
    put32(buf + 16, (uint32_t)bl);
    put32(buf + 24, (uint32_t)bl);
    memcpy(buf + 32, body, bl);
    return 32 + bl;
}

int disc_dhip_parse(const uint8_t *pkt, size_t len, disc_device_t *d)
{
    memset(d, 0, sizeof *d);
    d->dhcp = -1;
    if (len < 32 || get32(pkt + 4) != DHIP_MAGIC) return -1;
    size_t ml = get32(pkt + 24);
    if (!ml) ml = get32(pkt + 16);
    if (ml > len - 32) ml = len - 32;
    cJSON *doc = parse_json(pkt + 32, ml);
    if (!doc) return -1;
    int rc = -1;
    const char *method = jstr(doc, "method");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(doc, "params");
    const cJSON *info = cJSON_GetObjectItemCaseSensitive(params, "deviceInfo");
    const cJSON *v4 = cJSON_GetObjectItemCaseSensitive(info, "IPv4Address");
    uint32_t ipv;
    if (method && !strcmp(method, "client.notifyDevInfo") && cJSON_IsObject(info) && cJSON_IsObject(v4) &&
        disc_ip_parse(jstr(v4, "IPAddress"), &ipv) == 0) {
        copy(d->ip, sizeof d->ip, jstr(v4, "IPAddress"));
        uint32_t t;
        if (disc_ip_parse(jstr(v4, "SubnetMask"), &t) == 0) copy(d->mask, sizeof d->mask, jstr(v4, "SubnetMask"));
        if (disc_ip_parse(jstr(v4, "DefaultGateway"), &t) == 0)
            copy(d->gateway, sizeof d->gateway, jstr(v4, "DefaultGateway"));
        const cJSON *dh = cJSON_GetObjectItemCaseSensitive(v4, "DhcpEnable");
        d->dhcp = cJSON_IsBool(dh) ? cJSON_IsTrue(dh) : -1;
        const char *mac = jstr(doc, "mac");
        if (!mac) mac = jstr(info, "MAC");
        disc_mac_norm(mac ? mac : "", d->mac, sizeof d->mac);
        d->http_port = jint(info, "HttpPort", 80);
        d->dh_port = jint(info, "Port", 37777);
        d->channels = jint(info, "VideoInputChannels", 0);
        copy_text(d->model, sizeof d->model, jstr(info, "DeviceType"));
        if (!d->model[0]) copy_text(d->model, sizeof d->model, jstr(info, "DeviceClass"));
        copy_text(d->name, sizeof d->name, jstr(info, "MachineName"));
        copy_text(d->serial, sizeof d->serial, jstr(info, "SerialNo"));
        copy_text(d->vendor, sizeof d->vendor, jstr(info, "Vendor"));
        if (!d->vendor[0]) copy_text(d->vendor, sizeof d->vendor, jstr(info, "Manufacturer"));
        copy_text(d->version, sizeof d->version, jstr(info, "Version"));
        d->protos = DISC_DHIP;
        rc = 0;
    }
    cJSON_Delete(doc);
    return rc;
}

/* ---- devices ---------------------------------------------------------------------------- */

int disc_mac_norm(const char *in, char *out, size_t cap)
{
    int nib[12], n = 0;
    if (cap) out[0] = 0;
    for (const char *p = in; *p; ++p) {
        if (isxdigit((unsigned char)*p)) {
            if (n == 12) return -1;
            nib[n++] = isdigit((unsigned char)*p) ? *p - '0' : tolower((unsigned char)*p) - 'a' + 10;
        } else if (!strchr(":-. ", *p)) {
            return -1;
        }
    }
    if (n != 12 || cap < 18) return -1;
    int zero = 1;
    for (int i = 0; i < 12; ++i) zero &= nib[i] == 0;
    if (zero) return -1;
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 6; ++i) {
        out[3 * i] = hx[nib[2 * i]];
        out[3 * i + 1] = hx[nib[2 * i + 1]];
        out[3 * i + 2] = i < 5 ? ':' : 0;
    }
    return 0;
}

int disc_same(const disc_device_t *a, const disc_device_t *b)
{
    if (a->mac[0] && b->mac[0]) return !strcmp(a->mac, b->mac);
    return a->ip[0] && !strcmp(a->ip, b->ip);
}

#define FILL(f) do { if (!d->f[0] && in->f[0]) copy(d->f, sizeof d->f, in->f); } while (0)
#define FILLN(f) do { if (!d->f && in->f) d->f = in->f; } while (0)

void disc_merge(disc_device_t *d, const disc_device_t *in)
{
    /* The newest answer has the current address (after a change, the old
     * one is gone). */
    if (in->ip[0] && strcmp(d->ip, in->ip) != 0) {
        copy(d->ip, sizeof d->ip, in->ip);
        if (in->mask[0]) copy(d->mask, sizeof d->mask, in->mask);
        if (in->gateway[0]) copy(d->gateway, sizeof d->gateway, in->gateway);
        if (in->xaddr[0]) copy(d->xaddr, sizeof d->xaddr, in->xaddr);
        else if (!(in->protos & DISC_ONVIF)) d->xaddr[0] = 0;
    }
    FILL(mac); FILL(mask); FILL(gateway); FILL(vendor); FILL(model); FILL(name); FILL(serial);
    FILL(version); FILL(xaddr); FILL(uuid); FILL(iface);
    FILLN(netip_port); FILLN(http_port); FILLN(rtsp_port); FILLN(dh_port); FILLN(channels);
    if (in->dhcp >= 0) d->dhcp = in->dhcp;
    /* XM's NetCommon names no vendor but "Xiongmai" (set by the parser):
     * an ONVIF or DHIP vendor is the better word. */
    if (in->protos & (DISC_ONVIF | DISC_DHIP) && in->vendor[0] && !strcmp(d->vendor, "Xiongmai"))
        copy(d->vendor, sizeof d->vendor, in->vendor);
    d->protos |= in->protos;
    if (in->seen > d->seen) d->seen = in->seen;
    if (in->scan > d->scan) d->scan = in->scan;      /* scan ids only grow */
}

/* Equal but for the time last heard (field by field: padding is not
 * copied reliably). */
static int same_news(const disc_device_t *a, const disc_device_t *b)
{
#define S(f) (strcmp(a->f, b->f) == 0)
#define N(f) (a->f == b->f)
    return S(mac) && S(ip) && S(mask) && S(gateway) && N(dhcp) && N(protos) && N(netip_port) && N(http_port) &&
           N(rtsp_port) && N(dh_port) && N(channels) && S(vendor) && S(model) && S(name) && S(serial) &&
           S(version) && S(xaddr) && S(uuid) && S(iface) && N(scan);
#undef S
#undef N
}

int disc_table_add(disc_device_t *tab, int *n, int max, const disc_device_t *in)
{
    int i;
    for (i = 0; i < *n; ++i)
        if (disc_same(&tab[i], in)) break;
    if (i == *n) {
        if (max < 1) return 0;
        if (*n >= max) {                        /* full: the one heard least recently goes */
            int old = 0;
            for (int j = 1; j < *n; ++j)
                if (tab[j].seen < tab[old].seen) old = j;
            memmove(&tab[old], &tab[old + 1], (size_t)(*n - old - 1) * sizeof *tab);
            --*n;
        }
        tab[(*n)++] = *in;
        return 1;
    }
    disc_device_t before = tab[i];
    disc_merge(&tab[i], in);
    int changed = !same_news(&before, &tab[i]);
    /* The address changed hands: another entry still claiming it without
     * a MAC is the same device, heard before its MAC was. */
    for (int j = 0; j < *n; ++j)
        if (j != i && !tab[j].mac[0] && !strcmp(tab[j].ip, tab[i].ip)) {
            disc_merge(&tab[i], &tab[j]);
            memmove(&tab[j], &tab[j + 1], (size_t)(*n - j - 1) * sizeof *tab);
            --*n;
            changed = 1;
            break;
        }
    return changed;
}

int disc_cred_ok(const char *s, int is_user)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
        if (*p <= 0x20 || *p == 0x7f || strchr("@/?#\"\\", *p) || (is_user && *p == ':')) return 0;
    return 1;
}

int disc_url(const disc_device_t *d, unsigned proto, const char *user, const char *pass,
             char *url, size_t ucap, char *sub, size_t scap)
{
    if (ucap) url[0] = 0;
    if (scap) sub[0] = 0;
    if (!user || !*user) user = "admin";
    if (!pass) pass = "";
    if (!d->ip[0] || !disc_cred_ok(user, 1) || !disc_cred_ok(pass, 0)) return -1;
    int n, m = 0;
    if (proto == DISC_NETIP) {
        n = snprintf(url, ucap, "netip://%s:%s@%s:%d/?ch=0&stream=main", user, pass, d->ip,
                     d->netip_port ? d->netip_port : 34567);
    } else if (proto == DISC_DHIP) {
        int rp = d->rtsp_port ? d->rtsp_port : 554;
        n = snprintf(url, ucap, "rtsp://%s:%s@%s:%d/cam/realmonitor?channel=1&subtype=0", user, pass, d->ip, rp);
        m = snprintf(sub, scap, "rtsp://%s:%s@%s:%d/cam/realmonitor?channel=1&subtype=1", user, pass, d->ip, rp);
    } else {
        return -1;
    }
    if (n < 0 || (size_t)n >= ucap || m < 0 || (size_t)m >= scap) {
        if (ucap) url[0] = 0;
        if (scap) sub[0] = 0;
        return -1;
    }
    return 0;
}

void disc_url_host(const char *url, char *host, size_t cap)
{
    if (cap) host[0] = 0;
    const char *p = strstr(url, "://");
    if (!p) return;
    p += 3;
    size_t auth = strcspn(p, "/?#");
    const char *at = NULL;
    for (const char *q = p; q < p + auth; ++q)
        if (*q == '@') at = q;                  /* the last '@' before the path */
    if (at) p = at + 1;
    size_t n = strcspn(p, ":/?#");
    if (n >= cap) return;
    memcpy(host, p, n);
    host[n] = 0;
}
