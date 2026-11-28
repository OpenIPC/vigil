/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_discovery.c — the discovery parsers and helpers (disc_proto.h,
 * disc_ops.h, discovery.h). The parsers read untrusted datagrams, so
 * besides the good cases every reply is also fed truncated at every
 * length and with random bytes changed; run under ASan/UBSan to see the
 * reads stay in bounds. ONVIF replies in data/discovery are real devices'
 * (an OpenIPC camera, a Xiongmai camera), addresses and MACs changed. */
#define _DEFAULT_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "disc_ops.h"
#include "disc_proto.h"
#include "discovery.h"
#include "hal_capture.h"
#include "md5.h"
#include "sha1.h"

static int g_fail, g_checks;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_fail; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static const char *g_data;

/* The configuration's URL check can create a capture: source; no HAL here. */
int hal_cap_start(int in, int ch, hal_cap_cb cb, void *user) { (void)in; (void)ch; (void)cb; (void)user; return -1; }
void hal_cap_stop(int in) { (void)in; }

static char *load(const char *name, size_t *len)
{
    char path[512];
    snprintf(path, sizeof path, "%s/discovery/%s", g_data, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    rewind(f);
    char *b = malloc(*len + 1);
    if (fread(b, 1, *len, f) != *len) exit(1);
    b[*len] = 0;
    fclose(f);
    return b;
}

static uint32_t g_rng = 12345;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

typedef int (*parse_fn)(const uint8_t *, size_t, disc_device_t *);
static int onvif_bytes(const uint8_t *p, size_t n, disc_device_t *d) { return disc_onvif_parse((const char *)p, n, d); }

/* Truncated at every length, then mutated: never out of bounds (ASan),
 * and what parses has printable, terminated fields. Each truncation is
 * its own allocation, so a read past it is caught. */
static void hammer(parse_fn fn, const uint8_t *pkt, size_t len, const char *what)
{
    int odd = 0;
    for (size_t n = 0; n <= len; ++n) {
        uint8_t *b = malloc(n ? n : 1);
        memcpy(b, pkt, n);
        disc_device_t d;
        if (fn(b, n, &d) == 0 && (strlen(d.ip) >= sizeof d.ip || strlen(d.name) >= sizeof d.name)) ++odd;
        free(b);
    }
    for (int k = 0; k < 3000; ++k) {
        uint8_t *b = malloc(len);
        memcpy(b, pkt, len);
        int edits = 1 + (int)(rnd() % 8);
        for (int e = 0; e < edits; ++e) {
            size_t at = rnd() % len;
            switch (rnd() % 4) {
            case 0: b[at] = (uint8_t)rnd(); break;
            case 1: b[at] = 0; break;
            case 2: b[at] = (uint8_t)"<>\"&:{}[],0xFF\\"[rnd() % 15]; break;
            default: b[at] ^= 0x80; break;
            }
        }
        size_t n = rnd() % 4 ? len : rnd() % (len + 1);
        disc_device_t d;
        if (fn(b, n, &d) == 0) {
            for (const char *p = d.name; *p; ++p) odd += (unsigned char)*p < 0x20;
            for (const char *p = d.model; *p; ++p) odd += (unsigned char)*p < 0x20;
            uint32_t v;
            odd += disc_ip_parse(d.ip, &v) != 0;
        }
        free(b);
    }
    CHECK(!odd, "%s: %d parses with bad fields", what, odd);
}

/* ---- NETIP --------------------------------------------------------------- */

static const char NETCOMMON[] =
    "{ \"NetWork.NetCommon\" : { \"BuildDate\" : \"2020-04-10 10:32:56\", \"ChannelNum\" : 1, "
    "\"DeviceType\" : 1, \"GateWay\" : \"0x0101A8C0\", \"HostIP\" : \"0x0A01A8C0\", \"HostName\" : \"LocalHost\", "
    "\"HttpPort\" : 80, \"MAC\" : \"00:12:31:4F:2A:10\", \"MaxBps\" : 0, \"MonMode\" : \"TCP\", "
    "\"SN\" : \"abcdef0123456789\", \"SSLPort\" : 8443, \"Submask\" : \"0x00FFFFFF\", \"TCPMaxConn\" : 10, "
    "\"TCPPort\" : 34567, \"TransferPlan\" : \"Quality\", \"UDPPort\" : 34568, \"UseHSDownLoad\" : false, "
    "\"Version\" : \"V4.02.R11.00000117.10001.131900.00000\" }, \"Ret\" : 100, \"SessionID\" : \"0x00000000\" }";

static size_t netip_pkt(uint8_t *buf, int msgid, const char *json, int lie)
{
    size_t jl = strlen(json);
    memset(buf, 0, 20);
    buf[0] = 0xff;
    buf[1] = 1;
    buf[14] = msgid & 0xff;
    buf[15] = msgid >> 8;
    uint32_t l = (uint32_t)jl + 2 + (uint32_t)lie;
    buf[16] = l; buf[17] = l >> 8; buf[18] = l >> 16; buf[19] = l >> 24;
    memcpy(buf + 20, json, jl);
    buf[20 + jl] = '\n';
    buf[21 + jl] = 0;
    return jl + 22;
}

static void test_netip(void)
{
    char s[32];
    CHECK(disc_netip_hexip("0x0A01A8C0", s, sizeof s) == 0 && !strcmp(s, "192.168.1.10"), "hexip %s", s);
    CHECK(disc_netip_hexip("0x00FFFFFF", s, sizeof s) == 0 && !strcmp(s, "255.255.255.0"), "mask %s", s);
    CHECK(disc_netip_iphex("192.168.1.10", s, sizeof s) == 0 && !strcmp(s, "0x0A01A8C0"), "iphex %s", s);
    CHECK(disc_netip_hexip("0x1234567890", s, sizeof s) < 0, "too long hex");
    CHECK(disc_netip_hexip("192.168.1.1", s, sizeof s) < 0 && disc_netip_hexip("0x", s, sizeof s) < 0 &&
          disc_netip_hexip("0xZZ", s, sizeof s) < 0, "not hex");

    uint8_t buf[4096];
    size_t n = disc_netip_search(buf, sizeof buf);
    CHECK(n == 20 && buf[0] == 0xff && (buf[14] | buf[15] << 8) == 1530, "search");
    disc_device_t d;
    CHECK(disc_netip_parse(buf, n, &d) < 0, "our own search heard back");

    n = netip_pkt(buf, 1531, NETCOMMON, 0);
    CHECK(disc_netip_parse(buf, n, &d) == 0, "NetCommon parses");
    CHECK(!strcmp(d.ip, "192.168.1.10") && !strcmp(d.mask, "255.255.255.0") && !strcmp(d.gateway, "192.168.1.1"),
          "addresses %s %s %s", d.ip, d.mask, d.gateway);
    CHECK(!strcmp(d.mac, "00:12:31:4f:2a:10") && d.netip_port == 34567 && d.http_port == 80 && d.channels == 1,
          "mac %s port %d", d.mac, d.netip_port);
    CHECK(!strcmp(d.serial, "abcdef0123456789") && !strcmp(d.model, "IPC") && d.protos == DISC_NETIP,
          "sn %s model %s", d.serial, d.model);
    hammer(disc_netip_parse, buf, n, "netip");

    /* The header's length larger than what arrived: bounded by the datagram. */
    n = netip_pkt(buf, 1531, NETCOMMON, 100000);
    CHECK(disc_netip_parse(buf, n, &d) == 0, "length beyond the datagram");
    /* Without "\n\0", as some firmwares send it. */
    n = netip_pkt(buf, 1531, NETCOMMON, -2) - 2;
    CHECK(disc_netip_parse(buf, n, &d) == 0, "no trailing newline");
    static const char *bad[] = {
        "{\"NetWork.NetCommon\":[1,2]}",
        "{\"NetWork.NetCommon\":{\"HostIP\":17}}",
        "{\"NetWork.NetCommon\":{\"HostIP\":\"10.0.0.1\"}}",
        "{\"Ret\":100}",
        "[]", "", "nul\x01l",
        "{\"NetWork.NetCommon\":{\"HostIP\":\"0x0A01A8C0\"",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        n = netip_pkt(buf, 1531, bad[i], 0);
        CHECK(disc_netip_parse(buf, n, &d) < 0, "bad NetCommon %zu", i);
    }
    /* Odd types where numbers and strings go; a duplicate key. */
    n = netip_pkt(buf, 1531, "{\"NetWork.NetCommon\":{\"HostIP\":\"0x0A01A8C0\",\"HostIP\":\"0x0B01A8C0\","
                             "\"TCPPort\":\"x\",\"HttpPort\":1e300,\"MAC\":5,\"HostName\":\"a\\u0001b\","
                             "\"DeviceType\":\"NVR-16\",\"ChannelNum\":-4}}", 0);
    CHECK(disc_netip_parse(buf, n, &d) == 0 && d.netip_port == 34567 && d.http_port == 80 && !d.mac[0] &&
          !strcmp(d.name, "a?b") && !strcmp(d.model, "NVR-16") && d.channels == 0,
          "odd types: port %d http %d name %s", d.netip_port, d.http_port, d.name);
    /* A huge body is not parsed at all. */
    char *big = malloc(40000);
    memset(big, ' ', 39999);
    big[0] = '{';
    big[39998] = '}';
    big[39999] = 0;
    uint8_t *bb = malloc(40100);
    n = netip_pkt(bb, 1531, big, 0);
    CHECK(disc_netip_parse(bb, n, &d) < 0, "oversized body");
    free(big);
    free(bb);

    /* The broadcast set and its reply. */
    n = netip_pkt(buf, 1531, NETCOMMON, 0);
    disc_netip_parse(buf, n, &d);
    n = disc_netip_set(buf, sizeof buf, &d, "192.168.1.77", "255.255.255.0", "192.168.1.1", "admin", "pw");
    char hash[9];
    md5_sofia("pw", hash);
    CHECK(n > 20 && (buf[14] | buf[15] << 8) == 1532 && strstr((char *)buf + 20, "\"HostIP\":\"0x4D01A8C0\"") &&
          strstr((char *)buf + 20, "\"DvrMac\":\"00:12:31:4f:2a:10\"") && strstr((char *)buf + 20, hash),
          "set: %.*s", (int)(n > 20 ? n - 20 : 0), buf + 20);
    CHECK(disc_netip_set(buf, 30, &d, "192.168.1.77", "255.255.255.0", "192.168.1.1", "admin", "pw") == 0,
          "set does not fit");
    disc_device_t nomac = d;
    nomac.mac[0] = 0;
    CHECK(disc_netip_set(buf, sizeof buf, &nomac, "192.168.1.77", "255.255.255.0", "", "admin", "") == 0,
          "set needs a MAC");
    n = netip_pkt(buf, 1533, "{\"Ret\":100}", 0);
    CHECK(disc_netip_set_reply(buf, n) == 100, "set reply");
    n = netip_pkt(buf, 1533, "{\"Ret\":203}", 0);
    CHECK(disc_netip_set_reply(buf, n) == 203, "set reply 203");
    CHECK(disc_netip_set_reply(buf, 19) < 0, "short set reply");
}

/* ---- ONVIF ---------------------------------------------------------------- */

static void test_onvif(void)
{
    size_t len;
    char *x = load("onvif_openipc.xml", &len);
    disc_device_t d;
    CHECK(disc_onvif_parse(x, len, &d) == 0, "OpenIPC ProbeMatches");
    CHECK(!strcmp(d.ip, "192.168.1.175") && d.http_port == 80 && !strcmp(d.xaddr, "http://192.168.1.175:80/onvif/device_service"),
          "ip %s port %d xaddr %s", d.ip, d.http_port, d.xaddr);
    CHECK(!strcmp(d.mac, "b4:c9:b9:00:00:01") && !strcmp(d.vendor, "OpenIPC") && !strcmp(d.model, "SigmaStar") &&
          !strcmp(d.name, "ssc325de-sc2239"), "mac %s vendor %s model %s name %s", d.mac, d.vendor, d.model, d.name);
    CHECK(!strcmp(d.uuid, "urn:uuid:00000000-0000-5000-a000-b4c9b9000001"), "uuid %s", d.uuid);
    hammer(onvif_bytes, (const uint8_t *)x, len, "onvif openipc");
    free(x);

    x = load("onvif_xm.xml", &len);
    CHECK(disc_onvif_parse(x, len, &d) == 0, "Xiongmai ProbeMatches");
    CHECK(!strcmp(d.ip, "192.168.1.53") && d.http_port == 8899 && !d.mac[0] && !strcmp(d.model, "IPC-model") &&
          !strcmp(d.name, "NVT"), "ip %s port %d model %s name %s", d.ip, d.http_port, d.model, d.name);
    hammer(onvif_bytes, (const uint8_t *)x, len, "onvif xm");
    free(x);

    /* Our own Probe, heard back on the multicast group, is no match. */
    char probe[1024];
    size_t pl = disc_onvif_probe(probe, sizeof probe, "uuid:1");
    CHECK(pl > 0 && strstr(probe, "NetworkVideoTransmitter"), "probe");
    CHECK(disc_onvif_parse(probe, pl, &d) < 0, "own probe");
    CHECK(disc_onvif_probe(probe, 100, "uuid:1") == 0, "probe does not fit");

    static const char odd[] =
        "<e:Envelope><e:Body><x:ProbeMatches><x:ProbeMatch>"
        "<a:EndpointReference><a:Address>urn:uuid:1&amp;2</a:Address></a:EndpointReference>"
        "<x:Scopes>\n onvif://www.onvif.org/name/Front%20door onvif://www.onvif.org/Hardware/HW%01 \n"
        "onvif://www.onvif.org/MAC/aa-bb-cc-dd-ee-ff</x:Scopes>"
        "<x:XAddrs>http://[fe80::1]/onvif/device_service https://10.0.0.9/x http://cam.local/x "
        "http://10.0.0.8:8080/onvif/device_service</x:XAddrs>"
        "</x:ProbeMatch></x:ProbeMatches></e:Body></e:Envelope>";
    CHECK(disc_onvif_parse(odd, sizeof odd - 1, &d) == 0 && !strcmp(d.ip, "10.0.0.8") && d.http_port == 8080 &&
          !strcmp(d.name, "Front door") && !strcmp(d.model, "HW?") && !strcmp(d.mac, "aa:bb:cc:dd:ee:ff") &&
          !strcmp(d.uuid, "urn:uuid:1&2"), "odd ProbeMatch: %s:%d '%s' '%s' %s %s", d.ip, d.http_port, d.name,
          d.model, d.mac, d.uuid);
    static const char *bad[] = {
        "<ProbeMatch><XAddrs>http://cam.local/</XAddrs></ProbeMatch>",
        "<ProbeMatch><XAddrs></XAddrs></ProbeMatch>",
        "<ProbeMatch><XAddrs>http://10.0.0.1/</XAddrs>",
        "<ProbeMatches/>",
        "<ProbeMatch/>",
        "<ProbeMatch><XAddrs>http://10.0.0.1:99999/</XAddrs></ProbeMatch>",
        "<ProbeMatch><XAddrs>http://10.0.0.1:0/</XAddrs></ProbeMatch>",
        "<", "", "<ProbeMatch",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i)
        CHECK(disc_onvif_parse(bad[i], strlen(bad[i]), &d) < 0, "bad ProbeMatch %zu", i);

    char out[64];
    size_t from = 0;
    static const char prof[] = "<r><trt:Profiles fixed=\"true\" token=\"main&amp;1\"><tt:Name>a</tt:Name></trt:Profiles>"
                               "<trt:Profiles token='sub'/><Profiles token=\"x\"></r>";
    CHECK(disc_xml_attr(prof, sizeof prof - 1, "Profiles", "token", out, sizeof out, &from) == 0 && !strcmp(out, "main&1"),
          "attr 1 %s", out);
    CHECK(disc_xml_attr(prof, sizeof prof - 1, "Profiles", "token", out, sizeof out, &from) == 0 && !strcmp(out, "sub"),
          "attr 2 %s", out);
    CHECK(disc_xml_attr(prof, sizeof prof - 1, "Profiles", "token", out, sizeof out, &from) < 0, "unclosed element");
    CHECK(disc_xml_text("<a>0123456789</a>", 17, "a", out, 5, NULL) < 0, "text does not fit");
}

/* ---- DHIP ------------------------------------------------------------------- */

static const char DEVINFO[] =
    "{\"mac\":\"3c:ef:8c:00:00:01\",\"method\":\"client.notifyDevInfo\",\"params\":{\"deviceInfo\":{"
    "\"AlarmInputChannels\":0,\"DeviceClass\":\"IPC\",\"DeviceType\":\"IPC-HFW1230S\",\"HttpPort\":80,"
    "\"IPv4Address\":{\"DefaultGateway\":\"192.168.1.1\",\"DhcpEnable\":false,\"IPAddress\":\"192.168.1.108\","
    "\"SubnetMask\":\"255.255.255.0\"},\"IPv6Address\":{\"IPAddress\":\"fe80::1/64\"},\"MachineName\":\"Gate\","
    "\"Manufacturer\":\"Dahua\",\"Port\":37777,\"SerialNo\":\"5J0000000000001\",\"Vendor\":\"Dahua\","
    "\"Version\":\"2.800.0000000.25.R\",\"VideoInputChannels\":1}}}";

static size_t dhip_pkt(uint8_t *buf, const char *json, uint32_t lie_msg, uint32_t magic)
{
    size_t jl = strlen(json);
    uint32_t h[8] = { 32, magic, 0, 0, (uint32_t)jl, 0, lie_msg ? lie_msg : (uint32_t)jl, 0 };
    for (int i = 0; i < 8; ++i)
        for (int k = 0; k < 4; ++k) buf[4 * i + k] = (uint8_t)(h[i] >> (8 * k));
    memcpy(buf + 32, json, jl);
    return 32 + jl;
}

static void test_dhip(void)
{
    uint8_t buf[4096];
    size_t n = disc_dhip_search(buf, sizeof buf);
    disc_device_t d;
    CHECK(n > 32 && !memcmp(buf + 4, "DHIP", 4) && buf[0] == 0x20 && strstr((char *)buf + 32, "DHDiscover.search"),
          "search");
    CHECK(disc_dhip_parse(buf, n, &d) < 0, "our own search heard back");
    CHECK(disc_dhip_search(buf, 40) == 0, "search does not fit");

    n = dhip_pkt(buf, DEVINFO, 0, 0x50494844);
    CHECK(disc_dhip_parse(buf, n, &d) == 0, "notifyDevInfo parses");
    CHECK(!strcmp(d.ip, "192.168.1.108") && !strcmp(d.mask, "255.255.255.0") && !strcmp(d.gateway, "192.168.1.1") &&
          d.dhcp == 0, "addresses %s %s %s %d", d.ip, d.mask, d.gateway, d.dhcp);
    CHECK(!strcmp(d.mac, "3c:ef:8c:00:00:01") && d.http_port == 80 && d.dh_port == 37777 &&
          !strcmp(d.model, "IPC-HFW1230S") && !strcmp(d.name, "Gate") && !strcmp(d.vendor, "Dahua") &&
          !strcmp(d.serial, "5J0000000000001") && d.channels == 1, "fields %s %s %s", d.mac, d.model, d.vendor);
    hammer(disc_dhip_parse, buf, n, "dhip");

    n = dhip_pkt(buf, DEVINFO, 0xffffffffu, 0x50494844);
    CHECK(disc_dhip_parse(buf, n, &d) == 0, "message length beyond the datagram");
    n = dhip_pkt(buf, DEVINFO, 0, 0x50494845);
    CHECK(disc_dhip_parse(buf, n, &d) < 0, "bad magic");
    static const char *bad[] = {
        "{\"method\":\"client.notifyDevInfo\",\"params\":{\"deviceInfo\":{\"IPv4Address\":{\"IPAddress\":\"1.2.3\"}}}}",
        "{\"method\":\"client.notifyDevInfo\",\"params\":{\"deviceInfo\":{}}}",
        "{\"method\":\"client.notifyDevInfo\",\"params\":[]}",
        "{\"method\":7,\"params\":{\"deviceInfo\":{\"IPv4Address\":{\"IPAddress\":\"1.2.3.4\"}}}}",
        "{\"method\":\"other\",\"params\":{\"deviceInfo\":{\"IPv4Address\":{\"IPAddress\":\"1.2.3.4\"}}}}",
        "", "{", "[]",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        n = dhip_pkt(buf, bad[i], 0, 0x50494844);
        CHECK(disc_dhip_parse(buf, n, &d) < 0, "bad devinfo %zu", i);
    }
    n = dhip_pkt(buf, "{\"method\":\"client.notifyDevInfo\",\"params\":{\"deviceInfo\":{\"IPv4Address\":"
                      "{\"IPAddress\":\"1.2.3.4\",\"SubnetMask\":\"nope\",\"DhcpEnable\":\"yes\"},\"HttpPort\":-1,"
                      "\"Port\":70000,\"MAC\":\"00-11-22-33-44-55\",\"DeviceClass\":\"NVR\"}}}", 0, 0x50494844);
    CHECK(disc_dhip_parse(buf, n, &d) == 0 && !d.mask[0] && d.dhcp == -1 && d.http_port == 80 &&
          d.dh_port == 37777 && !strcmp(d.mac, "00:11:22:33:44:55") && !strcmp(d.model, "NVR"),
          "odd devinfo: mask '%s' dhcp %d http %d mac %s", d.mask, d.dhcp, d.http_port, d.mac);
    CHECK(disc_dhip_parse(buf, 31, &d) < 0, "short header");
}

/* ---- devices, URLs, plans ----------------------------------------------------------- */

static int used_odd(const char *ip, void *u)
{
    (void)u;
    int last = atoi(strrchr(ip, '.') + 1);
    return last % 2 == 1 || last == 20;
}

static void test_devices(void)
{
    char m[32];
    CHECK(disc_mac_norm("AA-BB-CC-DD-EE-FF", m, sizeof m) == 0 && !strcmp(m, "aa:bb:cc:dd:ee:ff"), "mac -");
    CHECK(disc_mac_norm("aabb.ccdd.eeff", m, sizeof m) == 0 && !strcmp(m, "aa:bb:cc:dd:ee:ff"), "mac .");
    CHECK(disc_mac_norm("AABBCCDDEEFF", m, sizeof m) == 0, "mac bare");
    CHECK(disc_mac_norm("00:00:00:00:00:00", m, sizeof m) < 0 && !m[0], "zero mac");
    CHECK(disc_mac_norm("aa:bb:cc:dd:ee", m, sizeof m) < 0 && disc_mac_norm("aa:bb:cc:dd:ee:ff:00", m, sizeof m) < 0 &&
          disc_mac_norm("gg:bb:cc:dd:ee:ff", m, sizeof m) < 0, "bad macs");

    disc_device_t a = { .ip = "10.0.0.5", .mac = "aa:bb:cc:dd:ee:ff", .protos = DISC_NETIP, .netip_port = 34567,
                        .vendor = "Xiongmai", .seen = 5, .dhcp = -1 };
    disc_device_t b = { .ip = "10.0.0.5", .protos = DISC_ONVIF, .http_port = 8899, .vendor = "Acme",
                        .xaddr = "http://10.0.0.5:8899/onvif/device_service", .seen = 9, .dhcp = -1 };
    disc_device_t c = { .ip = "10.0.0.6", .mac = "aa:bb:cc:dd:ee:00", .seen = 1, .dhcp = -1 };
    CHECK(disc_same(&a, &b) && !disc_same(&a, &c) && !disc_same(&b, &c), "same");
    disc_merge(&a, &b);
    CHECK(a.protos == (DISC_NETIP | DISC_ONVIF) && a.http_port == 8899 && a.seen == 9 && !strcmp(a.vendor, "Acme") &&
          a.xaddr[0], "merge: protos %u vendor %s", a.protos, a.vendor);
    disc_device_t moved = { .ip = "10.0.0.50", .mac = "aa:bb:cc:dd:ee:ff", .protos = DISC_NETIP, .seen = 12, .dhcp = -1 };
    disc_merge(&a, &moved);
    CHECK(!strcmp(a.ip, "10.0.0.50") && !a.xaddr[0], "moved: %s xaddr '%s'", a.ip, a.xaddr);

    char url[512], sub[512];
    CHECK(disc_url(&a, DISC_NETIP, "admin", "pw", url, sizeof url, sub, sizeof sub) == 0 &&
          !strcmp(url, "netip://admin:pw@10.0.0.50:34567/?ch=0&stream=main") && !sub[0], "netip url %s", url);
    CHECK(disc_url(&a, DISC_DHIP, "", NULL, url, sizeof url, sub, sizeof sub) == 0 &&
          !strcmp(url, "rtsp://admin:@10.0.0.50:554/cam/realmonitor?channel=1&subtype=0") &&
          !strcmp(sub, "rtsp://admin:@10.0.0.50:554/cam/realmonitor?channel=1&subtype=1"), "dhip url %s", url);
    CHECK(disc_url(&a, DISC_ONVIF, "admin", "pw", url, sizeof url, sub, sizeof sub) < 0, "onvif needs the device");
    CHECK(disc_url(&a, DISC_NETIP, "admin", "p@ss", url, sizeof url, sub, sizeof sub) < 0 && !url[0], "@ in a password");
    CHECK(disc_url(&a, DISC_NETIP, "ad:min", "x", url, sizeof url, sub, sizeof sub) < 0, ": in a user");
    CHECK(disc_url(&a, DISC_NETIP, "admin", "pw", url, 20, sub, sizeof sub) < 0, "url does not fit");
    CHECK(disc_cred_ok("p:w!$%", 0) && !disc_cred_ok("a b", 0) && !disc_cred_ok("x/y", 0) && !disc_cred_ok("\x01", 0),
          "cred_ok");

    char host[64];
    disc_url_host("rtsp://u:p@ss@10.1.2.3:554/x?a=@", host, sizeof host);
    CHECK(!strcmp(host, "10.1.2.3"), "url host %s", host);
    disc_url_host("netip://10.1.2.4/?ch=0", host, sizeof host);
    CHECK(!strcmp(host, "10.1.2.4"), "url host %s", host);
    disc_url_host("capture:0", host, sizeof host);
    CHECK(!host[0], "capture host");

    uint32_t v;
    CHECK(disc_ip_parse("10.0.0.1", &v) == 0 && v == 0x0a000001, "ip parse");
    CHECK(disc_ip_parse("10.0.0.256", &v) < 0 && disc_ip_parse("10.0.0", &v) < 0 && disc_ip_parse(" 1.2.3.4", &v) < 0 &&
          disc_ip_parse("1.2.3.4 ", &v) < 0 && disc_ip_parse("+1.2.3.4", &v) < 0 && disc_ip_parse("", &v) < 0, "bad ips");
    CHECK(disc_mask_prefix("255.255.255.0") == 24 && disc_mask_prefix("255.255.254.0") == 23 &&
          disc_mask_prefix("255.0.255.0") < 0 && disc_mask_prefix("0.0.0.0") == 0, "prefix");

    char plan[8][16];
    int k = disc_plan("192.168.1.18", "255.255.255.0", 4, used_odd, NULL, plan);
    CHECK(k == 4 && !strcmp(plan[0], "192.168.1.18") && !strcmp(plan[1], "192.168.1.22") &&
          !strcmp(plan[2], "192.168.1.24") && !strcmp(plan[3], "192.168.1.26"), "plan %d %s %s", k, plan[0], plan[1]);
    k = disc_plan("192.168.1.252", "255.255.255.0", 4, NULL, NULL, plan);
    CHECK(k == 3 && !strcmp(plan[2], "192.168.1.254"), "plan stops at the broadcast address: %d", k);
    CHECK(disc_plan("192.168.1.0", "255.255.255.0", 1, NULL, NULL, plan) == 0, "network address is no start");
    CHECK(disc_plan("192.168.1.5", "255.0.255.0", 1, NULL, NULL, plan) == 0, "bad mask");

    char *t = disc_cameras_with("[{\"url\":\"rtsp://a/x\",\"name\":\"A\"}]", 3, "netip://u:p@h/", "", "C", NULL);
    cJSON *arr = t ? cJSON_Parse(t) : NULL;
    CHECK(cJSON_GetArraySize(arr) == 3 &&
          !strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 0), "url")->valuestring, "rtsp://a/x") &&
          !strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 1), "url")->valuestring, "") &&
          !strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 2), "name")->valuestring, "C"), "cameras_with %s", t);
    cJSON_Delete(arr);
    free(t);
    t = disc_cameras_with("[{\"url\":\"rtsp://a/x\"},{\"url\":\"rtsp://b/x\"}]", 1, "netip://h/", "", "N",
                          "http://h:8899/onvif/device_service");
    CHECK(t && strstr(t, "netip://h/") && !strstr(t, "rtsp://a/x") && strstr(t, "rtsp://b/x") &&
              strstr(t, "\"onvif_url\":\"http://h:8899/onvif/device_service\""),
          "replace, with the ONVIF address %s", t);
    free(t);
    CHECK(!disc_cameras_with("{}", 1, "x", "", "", NULL) && !disc_cameras_with("[]", 0, "x", "", "", NULL) &&
          !disc_cameras_with("[]", 999, "x", "", "", NULL), "cameras_with refuses");

    CHECK(disc_proto_bits("all") == DISC_ALL && disc_proto_bits("netip, dhip") == (DISC_NETIP | DISC_DHIP) &&
          disc_proto_bits("netip,ftp") == 0, "proto bits");
    CHECK(disc_set_prefer("dhip") == 0 && disc_set_prefer("onvif,netip,dhip") == 0 && disc_set_prefer("x") < 0,
          "prefer");
    disc_set_prefer("netip,onvif,dhip");
}

/* ---- the table, filled as answers come ------------------------------------------------- */

static int find_ip(const disc_device_t *t, int n, const char *ip)
{
    for (int i = 0; i < n; ++i)
        if (!strcmp(t[i].ip, ip)) return i;
    return -1;
}

/* Answers one by one, as a scan hears them: each new device at the end,
 * the same device over another protocol merged into its entry (which stays
 * where it is), an answer saying nothing new not counted as a change. */
static void test_table(void)
{
    disc_device_t t[4];
    int n = 0;
    disc_device_t xm = { .ip = "10.0.0.5", .mac = "aa:bb:cc:dd:ee:01", .protos = DISC_NETIP, .netip_port = 34567,
                         .vendor = "Xiongmai", .seen = 1, .dhcp = -1, .scan = 3 };
    disc_device_t dh = { .ip = "10.0.0.7", .mac = "aa:bb:cc:dd:ee:02", .protos = DISC_DHIP, .vendor = "Dahua",
                         .seen = 1, .dhcp = -1, .scan = 3 };
    disc_device_t ov = { .ip = "10.0.0.5", .protos = DISC_ONVIF, .http_port = 8899, .vendor = "Acme",
                         .xaddr = "http://10.0.0.5:8899/onvif/device_service", .seen = 2, .dhcp = -1, .scan = 3 };
    CHECK(disc_table_add(t, &n, 4, &xm) == 1 && n == 1, "first answer: a device");
    CHECK(disc_table_add(t, &n, 4, &dh) == 1 && n == 2 && !strcmp(t[1].ip, "10.0.0.7"), "second: at the end");
    CHECK(disc_table_add(t, &n, 4, &ov) == 1 && n == 2 && t[0].protos == (DISC_NETIP | DISC_ONVIF) &&
          !strcmp(t[0].vendor, "Acme") && t[0].http_port == 8899 && !strcmp(t[0].mac, "aa:bb:cc:dd:ee:01"),
          "the same camera over ONVIF: one entry, in its place, protocols %u", t[0].protos);
    xm.seen = 9;
    CHECK(disc_table_add(t, &n, 4, &xm) == 0 && n == 2 && t[0].seen == 9, "heard again, nothing new: no change");
    xm.scan = 4;
    CHECK(disc_table_add(t, &n, 4, &xm) == 1 && t[0].scan == 4, "heard by the next scan: a change");
    disc_device_t ov0 = ov;
    ov0.scan = 0;                                   /* an address change's re-probe */
    CHECK(disc_table_add(t, &n, 4, &ov0) == 0 && t[0].scan == 4, "not a scan's answer: the scan it was heard by stays");

    /* ONVIF first (no MAC: keyed by address), then NETIP with the MAC. */
    n = 0;
    disc_device_t a = { .ip = "10.0.0.9", .protos = DISC_ONVIF, .seen = 1, .dhcp = -1 };
    disc_device_t b = { .ip = "10.0.0.8", .mac = "aa:bb:cc:dd:ee:08", .protos = DISC_NETIP, .seen = 1, .dhcp = -1 };
    disc_device_t c = { .ip = "10.0.0.9", .mac = "aa:bb:cc:dd:ee:09", .protos = DISC_NETIP, .seen = 2, .dhcp = -1 };
    disc_table_add(t, &n, 4, &a);
    disc_table_add(t, &n, 4, &b);
    CHECK(disc_table_add(t, &n, 4, &c) == 1 && n == 2 && find_ip(t, n, "10.0.0.9") == 0 &&
          !strcmp(t[0].mac, "aa:bb:cc:dd:ee:09") && t[0].protos == (DISC_NETIP | DISC_ONVIF),
          "the MAC learned later: still one entry, still first");

    /* A device moved to an address an old MAC-less entry still has: that
     * entry is folded in and taken out, the rest keeping their order. */
    n = 0;
    disc_device_t m1 = { .ip = "10.0.0.20", .mac = "aa:bb:cc:dd:ee:20", .protos = DISC_NETIP, .seen = 1, .dhcp = -1 };
    disc_device_t old = { .ip = "10.0.0.30", .protos = DISC_ONVIF, .seen = 1, .dhcp = -1 };
    disc_device_t m3 = { .ip = "10.0.0.40", .mac = "aa:bb:cc:dd:ee:40", .protos = DISC_DHIP, .seen = 1, .dhcp = -1 };
    disc_device_t moved = { .ip = "10.0.0.30", .mac = "aa:bb:cc:dd:ee:20", .protos = DISC_NETIP, .seen = 5, .dhcp = -1 };
    disc_table_add(t, &n, 4, &m1);
    disc_table_add(t, &n, 4, &old);
    disc_table_add(t, &n, 4, &m3);
    CHECK(disc_table_add(t, &n, 4, &moved) == 1 && n == 2 && !strcmp(t[0].ip, "10.0.0.30") &&
          t[0].protos == (DISC_NETIP | DISC_ONVIF) && !strcmp(t[1].ip, "10.0.0.40"), "moved: %d entries", n);

    /* Full: the one heard least recently goes, the order otherwise kept. */
    n = 0;
    for (int i = 0; i < 4; ++i) {
        disc_device_t d = { .protos = DISC_NETIP, .seen = 10 + i, .dhcp = -1 };
        snprintf(d.ip, sizeof d.ip, "10.0.1.%d", i);
        if (i == 2) d.seen = 1;
        disc_table_add(t, &n, 4, &d);
    }
    disc_device_t e = { .ip = "10.0.1.9", .protos = DISC_NETIP, .seen = 20, .dhcp = -1 };
    CHECK(disc_table_add(t, &n, 4, &e) == 1 && n == 4 && find_ip(t, n, "10.0.1.2") < 0 &&
          !strcmp(t[2].ip, "10.0.1.3") && !strcmp(t[3].ip, "10.0.1.9"), "full: the oldest out, the rest in order");
    int none = 0;
    CHECK(disc_table_add(t, &none, 0, &e) == 0 && none == 0, "no room at all: nothing");
}

/* ---- credentials ------------------------------------------------------------------------ */

static void test_creds(void)
{
    uint8_t d[20];
    sha1_raw("abc", 3, d);
    CHECK(d[0] == 0xa9 && d[1] == 0x99 && d[19] == 0x9d, "sha1 abc");
    char long_in[200];
    memset(long_in, 'a', sizeof long_in);
    sha1_raw(long_in, 56, d);                     /* the padding needs a second block */
    CHECK(d[0] == 0xc2 && d[19] == 0x99, "sha1 56: %02x..%02x", d[0], d[19]);
    char b[16];
    CHECK(base64_encode("ab", 2, b, sizeof b) == 0 && !strcmp(b, "YWI="), "b64 %s", b);
    CHECK(base64_encode("abc", 3, b, 4) < 0, "b64 does not fit");

    /* The digest example of ONVIF's Application Programmer's Guide. */
    static const unsigned char nonce[16] = { 0x2c, 0xaa, 0x88, 0xe8, 0x6f, 0xc0, 0x8a, 0x42,
                                             0x82, 0x42, 0xb3, 0x74, 0xce, 0xa6, 0x45, 0x96 };
    char sec[2048];
    CHECK(disc_wsse_header("user<1>", "userpassword", "2010-09-16T07:50:45Z", nonce, sec, sizeof sec) == 0 &&
          strstr(sec, ">tuOSpGlFlIXsozq4HFNeeGeFLEI=<") && strstr(sec, ">LKqI6G/AikKCQrN0zqZFlg==<") &&
          strstr(sec, "user&lt;1&gt;"), "wsse %s", sec);

    char h[33];
    disc_dahua_hash("admin", "secret", "Login to 2J0000", "1234567", h);
    CHECK(!strcmp(h, "17F04A414CF27D2A66B8297646D3FFE2"), "dahua hash %s", h);

    char u[256];
    CHECK(disc_url_with_creds("rtsp://10.0.0.5:554/stream1", "admin", "pw", u, sizeof u) == 0 &&
          !strcmp(u, "rtsp://admin:pw@10.0.0.5:554/stream1"), "creds in %s", u);
    CHECK(disc_url_with_creds("rtsp://old:x@10.0.0.5/s?a=b@c", "admin", "", u, sizeof u) == 0 &&
          !strcmp(u, "rtsp://admin:@10.0.0.5/s?a=b@c"), "creds replaced %s", u);
    CHECK(disc_url_with_creds("10.0.0.5/x", "a", "b", u, sizeof u) < 0, "no scheme");
}

/* ---- devices entered by hand ---------------------------------------------------------- */

static void test_manual(void)
{
    char u[512], sub[512], err[96];
    disc_manual_t m = { .kind = DISC_MANUAL_NETIP, .host = "192.168.1.30", .pass = "pw", .dev_channel = 3 };
    CHECK(disc_manual_urls(&m, u, sizeof u, sub, sizeof sub, err, sizeof err) == DISC_OK &&
          !strcmp(u, "netip://admin:pw@192.168.1.30:34567/?ch=2&stream=main") && !sub[0], "netip %s", u);
    m = (disc_manual_t){ .kind = DISC_MANUAL_DAHUA, .host = "nvr.local", .user = "op", .pass = "x" };
    CHECK(disc_manual_urls(&m, u, sizeof u, sub, sizeof sub, err, sizeof err) == DISC_OK &&
          !strcmp(u, "rtsp://op:x@nvr.local:554/cam/realmonitor?channel=1&subtype=0") &&
          !strcmp(sub, "rtsp://op:x@nvr.local:554/cam/realmonitor?channel=1&subtype=1"), "dahua %s %s", u, sub);
    m = (disc_manual_t){ .kind = DISC_MANUAL_RTSP, .host = "10.0.0.5", .port = 8554, .main_path = "live" };
    CHECK(disc_manual_urls(&m, u, sizeof u, sub, sizeof sub, err, sizeof err) == DISC_OK &&
          !strcmp(u, "rtsp://10.0.0.5:8554/live") && !sub[0], "rtsp without user %s", u);
    m = (disc_manual_t){ .kind = DISC_MANUAL_RTSP, .host = "10.0.0.5", .user = "a", .pass = "b",
                         .main_path = "rtsp://10.0.0.6:10554/x?y=1", .sub_path = "rtsp://c:d@10.0.0.6/s" };
    CHECK(disc_manual_urls(&m, u, sizeof u, sub, sizeof sub, err, sizeof err) == DISC_OK &&
          !strcmp(u, "rtsp://a:b@10.0.0.6:10554/x?y=1") && !strcmp(sub, "rtsp://c:d@10.0.0.6/s"),
          "whole URLs %s %s", u, sub);
    static const disc_manual_t bad[] = {
        { .kind = DISC_MANUAL_NETIP, .host = "" },
        { .kind = DISC_MANUAL_NETIP, .host = "a b" },
        { .kind = DISC_MANUAL_NETIP, .host = "x@y" },
        { .kind = DISC_MANUAL_NETIP, .host = "10.0.0.1", .pass = "p@ss" },
        { .kind = DISC_MANUAL_NETIP, .host = "10.0.0.1", .port = 70000 },
        { .kind = DISC_MANUAL_RTSP, .host = "10.0.0.1" },
        { .kind = DISC_MANUAL_RTSP, .host = "10.0.0.1", .main_path = "a b" },
        { .kind = 9, .host = "10.0.0.1" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i)
        CHECK(disc_manual_urls(&bad[i], u, sizeof u, sub, sizeof sub, err, sizeof err) == DISC_EINVAL && !u[0],
              "bad manual %zu: %s", i, u);
    CHECK(disc_manual_port(DISC_MANUAL_NETIP) == 34567 && disc_manual_port(DISC_MANUAL_ONVIF) == 80 &&
          disc_manual_port(DISC_MANUAL_DAHUA) == 554 && disc_manual_port(DISC_MANUAL_RTSP) == 554, "ports");
    CHECK(disc_manual_kind("DHIP") == DISC_MANUAL_DAHUA && disc_manual_kind("rtsp") == DISC_MANUAL_RTSP &&
          disc_manual_kind("x") < 0, "kinds");
    char codec[8];
    int w, h;
    CHECK(disc_test_url("capture:0", 1, codec, &w, &h, err, sizeof err) == DISC_EINVAL, "a local input is no camera");
}

int main(int argc, char **argv)
{
    g_data = argc > 1 ? argv[1] : "tests/data";
    test_netip();
    test_onvif();
    test_dhip();
    test_devices();
    test_table();
    test_creds();
    test_manual();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
