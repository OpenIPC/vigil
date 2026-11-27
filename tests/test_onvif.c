/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_onvif.c — the ONVIF server's wire formats (onvif_proto.h): SOAP
 * requests, WS-UsernameToken and HTTP Digest, WS-Discovery probes and
 * our answers. Requests are untrusted bytes, so each is also parsed cut
 * at every length; run under ASan/UBSan to see the reads stay in bounds. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disc_proto.h"
#include "hal_capture.h"
#include "onvif_proto.h"

static int g_fail, g_checks;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_fail; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* The configuration's URL check can create a capture: source; no HAL here. */
int hal_cap_start(int in, int ch, hal_cap_cb cb, void *user) { (void)in; (void)ch; (void)cb; (void)user; return -1; }
void hal_cap_stop(int in) { (void)in; }

/* Every prefix of `xml`, in a buffer of exactly that size: nothing may
 * read past it, whatever it returns. */
static void cut_everywhere(const char *xml)
{
    size_t len = strlen(xml);
    for (size_t n = 0; n <= len; ++n) {
        char *b = malloc(n ? n : 1);
        memcpy(b, xml, n);
        onvif_soap_t r;
        onvif_probe_t p;
        if (onvif_soap_parse(b, n, &r) == 0) CHECK(r.arg0 <= r.arg1 && r.arg1 <= n, "args in bounds at %zu", n);
        if (onvif_wsd_probe_parse(b, n, &p) == 0) onvif_wsd_match(b, n, &p, "onvif://www.onvif.org/name/x");
        free(b);
    }
}

/* What onvif-zeep sends: prefixed SOAP 1.2, a digest UsernameToken. */
static const char ZEEP_GETPROFILES[] =
    "<?xml version='1.0' encoding='utf-8'?>\n"
    "<soap-env:Envelope xmlns:soap-env=\"http://www.w3.org/2003/05/soap-envelope\"><soap-env:Header>"
    "<wsse:Security xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\">"
    "<wsse:UsernameToken><wsse:Username>anna</wsse:Username>"
    "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">"
    "tuOSpGlFlIXsozq4HFNeeGeFLEI=</wsse:Password>"
    "<wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-soap-message-security-1.0#Base64Binary\">"
    "LKqI6G/AikKCQrN0zqZFlg==</wsse:Nonce>"
    "<wsu:Created xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
    "2010-09-16T07:50:45Z</wsu:Created></wsse:UsernameToken></wsse:Security></soap-env:Header>"
    "<soap-env:Body><ns0:GetStreamUri xmlns:ns0=\"http://www.onvif.org/ver10/media/wsdl\">"
    "<ns0:StreamSetup><ns1:Stream xmlns:ns1=\"http://www.onvif.org/ver10/schema\">RTP-Unicast</ns1:Stream>"
    "</ns0:StreamSetup><ns0:ProfileToken>ch2_sub</ns0:ProfileToken></ns0:GetStreamUri></soap-env:Body>"
    "</soap-env:Envelope>";

static void test_soap(void)
{
    onvif_soap_t r;
    const char *x = ZEEP_GETPROFILES;
    CHECK(onvif_soap_parse(x, strlen(x), &r) == 0, "zeep's request parses");
    CHECK(!strcmp(r.op, "GetStreamUri") && !strcmp(r.ns, ONVIF_NS_MEDIA), "op %s in %s", r.op, r.ns);
    char tok[32];
    CHECK(disc_xml_text(x + r.arg0, r.arg1 - r.arg0, "ProfileToken", tok, sizeof tok, NULL) == 0 &&
          !strcmp(tok, "ch2_sub"), "argument ProfileToken");
    CHECK(r.has_token && r.digest && !strcmp(r.user, "anna") && !strcmp(r.nonce, "LKqI6G/AikKCQrN0zqZFlg==") &&
          !strcmp(r.created, "2010-09-16T07:50:45Z") && !strcmp(r.password, "tuOSpGlFlIXsozq4HFNeeGeFLEI="),
          "UsernameToken: %s %s", r.user, r.created);
    cut_everywhere(x);

    /* A default namespace on the operation, no header, an empty element. */
    const char *d = "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"><s:Body>\n  <!-- hi -->\n"
                    "<GetDeviceInformation xmlns=\"http://www.onvif.org/ver10/device/wsdl\"/></s:Body></s:Envelope>";
    CHECK(onvif_soap_parse(d, strlen(d), &r) == 0 && !strcmp(r.op, "GetDeviceInformation") &&
          !strcmp(r.ns, ONVIF_NS_DEVICE) && r.arg0 == r.arg1 && !r.has_token, "default namespace, self-closed");
    cut_everywhere(d);

    /* The prefix declared on the Envelope (ODM's way). */
    const char *e = "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
                    "xmlns:tr2=\"http://www.onvif.org/ver20/media/wsdl\"><s:Body>"
                    "<tr2:GetProfiles><tr2:Type>All</tr2:Type></tr2:GetProfiles></s:Body></s:Envelope>";
    CHECK(onvif_soap_parse(e, strlen(e), &r) == 0 && !strcmp(r.op, "GetProfiles") && !strcmp(r.ns, ONVIF_NS_MEDIA2),
          "prefix from the Envelope: %s", r.ns);

    /* PasswordText: Type says so, or no Type at all. */
    const char *t = "<Envelope xmlns=\"http://www.w3.org/2003/05/soap-envelope\"><Header><Security><UsernameToken>"
                    "<Username>root</Username><Password>a&amp;b</Password></UsernameToken></Security></Header>"
                    "<Body><GetUsers xmlns=\"http://www.onvif.org/ver10/device/wsdl\"/></Body></Envelope>";
    CHECK(onvif_soap_parse(t, strlen(t), &r) == 0 && r.has_token && !r.digest && !strcmp(r.password, "a&b") &&
          !strcmp(r.ns, ONVIF_NS_DEVICE), "PasswordText, entities decoded");

    /* No namespace anywhere: "" (the server goes by the path then). */
    const char *n = "<Envelope><Body><GetProfiles/></Body></Envelope>";
    CHECK(onvif_soap_parse(n, strlen(n), &r) == 0 && !r.ns[0], "undeclared namespace");

    CHECK(onvif_soap_parse("", 0, &r) < 0, "empty");
    const char *nb = "<s:Envelope xmlns:s=\"x\"><s:Body></s:Body></s:Envelope>";
    CHECK(onvif_soap_parse(nb, strlen(nb), &r) < 0, "empty Body");
    const char *nenv = "<Body><GetProfiles/></Body>";
    CHECK(onvif_soap_parse(nenv, strlen(nenv), &r) < 0, "no Envelope");
    const char *junk = "GET / HTTP/1.1\r\n\r\n<<<>>><Envelope><Body><";
    CHECK(onvif_soap_parse(junk, strlen(junk), &r) < 0, "junk");
    cut_everywhere(t);
}

static void test_wsse(void)
{
    /* ONVIF Application Programmer's Guide, 6.1.1.3. */
    char d[32];
    CHECK(onvif_wsse_digest("LKqI6G/AikKCQrN0zqZFlg==", "2010-09-16T07:50:45Z", "userpassword", d) == 0 &&
          !strcmp(d, "tuOSpGlFlIXsozq4HFNeeGeFLEI="), "UsernameToken digest: %s", d);
    CHECK(onvif_wsse_digest("not*base64", "x", "y", d) < 0, "a nonce that does not decode");
    CHECK(onvif_equal("abc", "abc") && !onvif_equal("abc", "abd") && !onvif_equal("abc", "ab") &&
          !onvif_equal("", "a") && onvif_equal("", ""), "equal");

    time_t t;
    CHECK(onvif_time_parse("2010-09-16T07:50:45Z", &t) == 0 && t == 1284623445, "Created, Z: %ld", (long)t);
    CHECK(onvif_time_parse("2010-09-16T07:50:45.123Z", &t) == 0 && t == 1284623445, "fractions");
    CHECK(onvif_time_parse("2010-09-16T09:50:45+02:00", &t) == 0 && t == 1284623445, "an offset");
    CHECK(onvif_time_parse("2010-09-16T07:50:45", &t) == 0 && t == 1284623445, "no zone is UTC");
    CHECK(onvif_time_parse("1970-01-01T00:00:00Z", &t) == 0 && t == 0, "the epoch");
    CHECK(onvif_time_parse("2024-02-29T12:00:00Z", &t) == 0 && t == 1709208000, "a leap day");
    CHECK(onvif_time_parse("2010-13-16T07:50:45Z", &t) < 0, "month 13");
    CHECK(onvif_time_parse("2010-09-16 07:50:45Z", &t) < 0, "no T");
    CHECK(onvif_time_parse("2010-09-16T07:50:45Zjunk", &t) < 0, "trailing junk");
    CHECK(onvif_time_parse("", &t) < 0, "empty");
}

static void test_digest(void)
{
    /* RFC 2617, 3.5. */
    onvif_digest_t d;
    const char *h = "Digest username=\"Mufasa\", realm=\"testrealm@host.com\", "
                    "nonce=\"dcd98b7102dd2f0e8b11d0f600bfb0c093\", uri=\"/dir/index.html\", qop=auth, nc=00000001, "
                    "cnonce=\"0a4f113b\", response=\"6629fae49393a05397450978507c4ef1\", "
                    "opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"";
    CHECK(onvif_digest_parse(h, &d) == 0 && !strcmp(d.username, "Mufasa") && !strcmp(d.uri, "/dir/index.html") &&
          !strcmp(d.qop, "auth") && !strcmp(d.nc, "00000001"), "RFC 2617 header");
    char want[33];
    onvif_digest_expect(&d, "GET", "Circle Of Life", want);
    CHECK(!strcmp(want, d.response), "RFC 2617 response: %s", want);
    onvif_digest_expect(&d, "GET", "circle of life", want);
    CHECK(strcmp(want, d.response) != 0, "another password");
    /* Without qop (RFC 2069), quoted escapes, odd spacing. */
    CHECK(onvif_digest_parse("Digest  username = \"a\\\"b\" ,realm=r,nonce=n,uri=/x,response=0", &d) == 0 &&
          !strcmp(d.username, "a\"b") && !d.qop[0], "no qop, escapes");
    CHECK(onvif_digest_parse("Basic YTpi", &d) < 0, "not Digest");
    CHECK(onvif_digest_parse("Digest username=\"a\", realm=r, uri=/x, response=0", &d) < 0, "no nonce");
    CHECK(onvif_digest_parse("Digest username=a,realm=r,nonce=n,uri=/x,response=0,algorithm=SHA-256", &d) < 0,
          "SHA-256 is not ours");
    CHECK(onvif_digest_parse("Digest username=a,realm=r,nonce=n,uri=/x,response=0,qop=auth", &d) < 0,
          "qop without nc and cnonce");
    CHECK(onvif_digest_parse("Digest username=\"unterminated", &d) < 0, "unterminated quote");

    unsigned char key[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }, other[16] = { 0 };
    char nonce[41], again[41];
    onvif_digest_nonce(key, 1000, 7, nonce);
    CHECK(strlen(nonce) == 40, "nonce: %s", nonce);
    CHECK(onvif_digest_nonce_age(key, nonce, 1000) == 0 && onvif_digest_nonce_age(key, nonce, 1300) == 300,
          "a nonce's age");
    onvif_digest_nonce(key, 1000, 8, again);
    CHECK(strcmp(nonce, again) != 0 && onvif_digest_nonce_age(key, again, 1000) == 0,
          "two nonces of the same second differ");
    CHECK(onvif_digest_nonce_age(other, nonce, 1000) < 0, "another key's nonce");
    nonce[39] ^= 1;
    CHECK(onvif_digest_nonce_age(key, nonce, 1000) < 0, "a changed nonce");
    onvif_digest_nonce(key, 1000, 7, nonce);
    nonce[7] = nonce[7] == '0' ? '1' : '0';             /* its time moved */
    CHECK(onvif_digest_nonce_age(key, nonce, 5000) < 0, "a nonce whose time was changed");
    onvif_digest_nonce(key, 1000, 7, nonce);
    nonce[15] = nonce[15] == '0' ? '1' : '0';           /* its serial */
    CHECK(onvif_digest_nonce_age(key, nonce, 1000) < 0, "a nonce whose serial was changed");
    CHECK(onvif_digest_nonce_age(key, "short", 1000) < 0, "not a nonce");

    /* Replays: each nonce count once. */
    static onvif_nc_t nc;
    memset(&nc, 0, sizeof nc);
    onvif_digest_t r;
    memset(&r, 0, sizeof r);
    snprintf(r.nonce, sizeof r.nonce, "n1");
    snprintf(r.qop, sizeof r.qop, "auth");
    snprintf(r.cnonce, sizeof r.cnonce, "c1");
    snprintf(r.nc, sizeof r.nc, "00000001");
    CHECK(onvif_nc_use(&nc, &r, 1000, 1000, 300) == 1, "a first request");
    CHECK(onvif_nc_use(&nc, &r, 1000, 1001, 300) == 0, "the same request again: a replay");
    snprintf(r.nc, sizeof r.nc, "00000002");
    CHECK(onvif_nc_use(&nc, &r, 1000, 1001, 300) == 1, "the next count");
    snprintf(r.nc, sizeof r.nc, "00000001");
    CHECK(onvif_nc_use(&nc, &r, 1000, 1001, 300) == 0, "an older count");
    snprintf(r.cnonce, sizeof r.cnonce, "c2");
    CHECK(onvif_nc_use(&nc, &r, 1000, 1001, 300) == 1, "another client's cnonce on the same nonce");
    snprintf(r.nc, sizeof r.nc, "0");
    CHECK(onvif_nc_use(&nc, &r, 1000, 1001, 300) == 0, "count 0");
    onvif_digest_t p = r;
    p.qop[0] = p.nc[0] = p.cnonce[0] = 0;
    snprintf(p.nonce, sizeof p.nonce, "n2");
    CHECK(onvif_nc_use(&nc, &p, 1000, 1001, 300) == 1 && onvif_nc_use(&nc, &p, 1000, 1001, 300) == 0,
          "without qop: a nonce once");
    /* Full: the oldest go, and their time's nonces turn stale. */
    snprintf(r.nc, sizeof r.nc, "1");
    for (int i = 0; i < ONVIF_NC_MAX; ++i) {
        snprintf(r.nonce, sizeof r.nonce, "m%d", i);
        onvif_nc_use(&nc, &r, 1100 + i, 1200, 300);
    }
    snprintf(r.nonce, sizeof r.nonce, "n1");
    snprintf(r.cnonce, sizeof r.cnonce, "c1");
    snprintf(r.nc, sizeof r.nc, "00000001");
    CHECK(onvif_nc_use(&nc, &r, 1000, 1200, 300) == -1, "a forgotten nonce's request again: stale");
    snprintf(r.nonce, sizeof r.nonce, "m%d", ONVIF_NC_MAX - 1);
    snprintf(r.cnonce, sizeof r.cnonce, "c2");
    CHECK(onvif_nc_use(&nc, &r, 1100 + ONVIF_NC_MAX - 1, 1200, 300) == 0, "a kept one's: a replay");
    snprintf(r.nonce, sizeof r.nonce, "late");
    CHECK(onvif_nc_use(&nc, &r, 1500, 1500, 300) == 1 && nc.n < ONVIF_NC_MAX, "expired counts make room");
}

/* ONVIF Device Manager's probe: default namespaces, wsa on each element,
 * an empty Scopes. */
static const char ODM_PROBE[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?><Envelope xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\" "
    "xmlns=\"http://www.w3.org/2003/05/soap-envelope\"><Header><wsa:MessageID "
    "xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\">uuid:2ac6f7ba-1bcb-4d02-ad3c-3e0b7d5e9a8f"
    "</wsa:MessageID><wsa:To xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\">"
    "urn:schemas-xmlsoap-org:ws:2005:04:discovery</wsa:To><wsa:Action "
    "xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\">"
    "http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</wsa:Action></Header><Body><Probe "
    "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" "
    "xmlns=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\"><Types>dn:NetworkVideoTransmitter</Types>"
    "<Scopes /></Probe></Body></Envelope>";

static const char SCOPES[] = "onvif://www.onvif.org/type/NetworkVideoTransmitter "
                             "onvif://www.onvif.org/Profile/Streaming onvif://www.onvif.org/name/Front%20gate "
                             "onvif://www.onvif.org/hardware/SSR621Q%20NVR";

static int probe_matches(const char *xml)
{
    onvif_probe_t p;
    size_t n = strlen(xml);
    return onvif_wsd_probe_parse(xml, n, &p) == 0 && onvif_wsd_match(xml, n, &p, SCOPES);
}

/* A probe with these Types (and the dn/tds prefixes declared) and Scopes. */
static void probe(char *out, size_t cap, const char *types, const char *scopes, const char *match_by)
{
    snprintf(out, cap,
             "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
             "xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
             "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
             "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\" xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
             "xmlns:bad=\"http://example.com/other\"><s:Header><a:MessageID>uuid:1</a:MessageID></s:Header>"
             "<s:Body><d:Probe>%s%s%s%s%s%s%s%s</d:Probe></s:Body></s:Envelope>",
             types ? "<d:Types>" : "", types ? types : "", types ? "</d:Types>" : "", scopes ? "<d:Scopes" : "",
             scopes && match_by ? match_by : "", scopes ? ">" : "", scopes ? scopes : "", scopes ? "</d:Scopes>" : "");
}

static void test_probe(void)
{
    onvif_probe_t p;
    CHECK(onvif_wsd_probe_parse(ODM_PROBE, strlen(ODM_PROBE), &p) == 0 &&
          !strcmp(p.msgid, "uuid:2ac6f7ba-1bcb-4d02-ad3c-3e0b7d5e9a8f") && !strcmp(p.types, "dn:NetworkVideoTransmitter") &&
          !p.scopes[0], "ODM's probe: %s", p.msgid);
    CHECK(probe_matches(ODM_PROBE), "ODM's probe matches");
    cut_everywhere(ODM_PROBE);

    /* vigild's own camera search (disc_proto.c) finds this recorder too
     * (discovery.c then skips it by its endpoint reference). */
    char own[2048];
    CHECK(disc_onvif_probe(own, sizeof own, "uuid:own") > 0 && probe_matches(own), "vigild's own probe matches");

    char b[2048];
    probe(b, sizeof b, NULL, NULL, NULL);
    CHECK(probe_matches(b), "no Types, no Scopes: everything matches");
    probe(b, sizeof b, "tds:Device", NULL, NULL);
    CHECK(probe_matches(b), "tds:Device");
    probe(b, sizeof b, "dn:NetworkVideoTransmitter tds:Device", NULL, NULL);
    CHECK(probe_matches(b), "both types");
    probe(b, sizeof b, "dn:NetworkVideoDisplay", NULL, NULL);
    CHECK(!probe_matches(b), "a display is not us");
    probe(b, sizeof b, "bad:NetworkVideoTransmitter", NULL, NULL);
    CHECK(!probe_matches(b), "the right name in another namespace");
    probe(b, sizeof b, "undeclared:NetworkVideoTransmitter", NULL, NULL);
    CHECK(probe_matches(b), "an undeclared prefix: by the local name");
    probe(b, sizeof b, NULL, "onvif://www.onvif.org/Profile/Streaming", NULL);
    CHECK(probe_matches(b), "a scope of ours");
    probe(b, sizeof b, NULL, "ONVIF://WWW.ONVIF.ORG/name", NULL);
    CHECK(probe_matches(b), "rfc3986: a path prefix, scheme and host in any case");
    probe(b, sizeof b, NULL, "onvif://www.onvif.org/nam", NULL);
    CHECK(!probe_matches(b), "rfc3986: only whole segments");
    probe(b, sizeof b, NULL, "onvif://www.onvif.org/Name/Front%20gate", NULL);
    CHECK(!probe_matches(b), "rfc3986: the path's case counts");
    probe(b, sizeof b, NULL, "onvif://www.onvif.org/name/Front%20gate/", NULL);
    CHECK(probe_matches(b), "rfc3986: a trailing slash");
    probe(b, sizeof b, NULL, "onvif://www.onvif.org/Profile/Streaming onvif://www.onvif.org/location/x", NULL);
    CHECK(!probe_matches(b), "every scope asked for must be ours");
    probe(b, sizeof b, NULL, "onvif://www.onvif.org/name",
          " MatchBy=\"http://schemas.xmlsoap.org/ws/2005/04/discovery/strcmp0\"");
    CHECK(!probe_matches(b), "strcmp0: whole strings only");
    probe(b, sizeof b, NULL, "onvif://www.onvif.org/Profile/Streaming",
          " MatchBy=\"http://schemas.xmlsoap.org/ws/2005/04/discovery/strcmp0\"");
    CHECK(probe_matches(b), "strcmp0: equal");
    probe(b, sizeof b, NULL, "onvif://www.onvif.org/Profile/Streaming", " MatchBy=\"http://example.com/ldap\"");
    CHECK(!probe_matches(b), "a matching rule we do not have");

    CHECK(onvif_scope_match("onvif://a/b", 11, "onvif://a/b/c", 13) && !onvif_scope_match("onvif://a/b/c", 13, "onvif://a/b", 11) &&
          onvif_scope_match("onvif://a", 9, "onvif://A/x", 11) && !onvif_scope_match("onvif://ab", 10, "onvif://a/b", 11),
          "scope rules");

    /* Not probes: our own answers looped back, other messages. */
    onvif_wsd_t w = { .uuid = "urn:uuid:11111111-2222-3333-4444-555555555555", .scopes = SCOPES,
                      .xaddrs = "http://192.168.1.10:8899/onvif/device_service", .instance = 7, .msgno = 3,
                      .metadata = 2 };
    char m[4096];
    size_t n = onvif_wsd_probe_matches(m, sizeof m, &w, "urn:uuid:aa", "uuid:2ac6f7ba-<&>");
    CHECK(n > 0 && n == strlen(m), "ProbeMatches built");
    CHECK(strstr(m, "<a:RelatesTo>uuid:2ac6f7ba-&lt;&amp;&gt;</a:RelatesTo>") != NULL, "RelatesTo escaped");
    CHECK(onvif_wsd_probe_parse(m, n, &p) < 0, "a ProbeMatches is no probe");
    /* ... and a client (vigild's own) reads it as a device. */
    disc_device_t dev;
    CHECK(disc_onvif_parse(m, n, &dev) == 0 && !strcmp(dev.ip, "192.168.1.10") && dev.http_port == 8899 &&
          !strcmp(dev.uuid, w.uuid) && !strcmp(dev.name, "Front gate") && !strcmp(dev.model, "SSR621Q NVR"),
          "ProbeMatches as a client reads it: %s:%d %s / %s", dev.ip, dev.http_port, dev.name, dev.model);
    CHECK(onvif_wsd_probe_matches(m, 200, &w, "urn:uuid:aa", "uuid:1") == 0, "does not fit");
    n = onvif_wsd_hello(m, sizeof m, &w, "urn:uuid:bb");
    CHECK(n > 0 && strstr(m, "/discovery/Hello</a:Action>") && strstr(m, "<d:XAddrs>http://192.168.1.10:8899") &&
          strstr(m, "InstanceId=\"7\" MessageNumber=\"3\"") && strstr(m, "<d:MetadataVersion>2<"), "Hello");
    CHECK(onvif_wsd_probe_parse(m, n, &p) < 0, "a Hello is no probe");
    n = onvif_wsd_bye(m, sizeof m, &w, "urn:uuid:cc");
    CHECK(n > 0 && strstr(m, "/discovery/Bye</a:Action>") && strstr(m, w.uuid) && !strstr(m, "XAddrs"), "Bye");

    char enc[64];
    onvif_scope_encode("Front gate/1 ü", enc, sizeof enc);
    CHECK(!strcmp(enc, "Front%20gate%2F1%20%C3%BC"), "scope value encoded: %s", enc);
    onvif_scope_encode("abcdefghij", enc, 6);
    CHECK(!strcmp(enc, "abcde"), "cut to fit");
    char esc[16];
    CHECK(onvif_xml_escape("a<b>&\"'", esc, sizeof esc) < 0, "escape: does not fit");
    CHECK(onvif_xml_escape("<&>", esc, sizeof esc) == 0 && !strcmp(esc, "&lt;&amp;&gt;"), "escape");
}

int main(void)
{
    test_soap();
    test_wsse();
    test_digest();
    test_probe();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
