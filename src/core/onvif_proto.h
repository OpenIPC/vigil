/* SPDX-License-Identifier: GPL-3.0-or-later */
/* onvif_proto.h — the wire formats of vigild's ONVIF server (src/api/onvif.c,
 * docs/onvif.md), as pure functions over untrusted bytes with their length:
 *
 *   - SOAP 1.2 requests: which operation the Body holds (its first
 *     element: local name and namespace) and the WS-Security
 *     UsernameToken in the Header, if any. Tolerant, like disc_proto.h's
 *     XML: elements are matched by local name, a prefix is looked up in
 *     the xmlns declarations before it. No DTDs, no entities but the five.
 *   - WS-UsernameToken (OASIS UsernameToken Profile 1.0):
 *     PasswordDigest = base64(sha1(nonce + created + password)).
 *   - HTTP Digest (RFC 7616 with MD5, qop "auth" or none): the
 *     Authorization header's fields, the expected response, and nonces
 *     that carry their own time and a keyed check, so the server keeps no
 *     table of them.
 *   - WS-Discovery 2005/04 as a target service: is a datagram a Probe,
 *     does it match our Types and Scopes (MatchBy rfc3986, the default,
 *     or strcmp0), and the ProbeMatches, Hello and Bye messages.
 */
#ifndef ONVIF_PROTO_H
#define ONVIF_PROTO_H

#include <stddef.h>
#include <time.h>

#define ONVIF_NS_DEVICE  "http://www.onvif.org/ver10/device/wsdl"
#define ONVIF_NS_MEDIA   "http://www.onvif.org/ver10/media/wsdl"
#define ONVIF_NS_MEDIA2  "http://www.onvif.org/ver20/media/wsdl"
#define ONVIF_NS_NETWORK "http://www.onvif.org/ver10/network/wsdl"

/* ---- SOAP --------------------------------------------------------------- */

typedef struct {
    char   op[64];                  /* local name of the Body's first element */
    char   ns[128];                 /* its namespace URI; "" when not declared */
    size_t arg0, arg1;              /* its content: xml[arg0, arg1) */
    /* WS-Security UsernameToken, when has_token */
    int    has_token;
    int    digest;                  /* PasswordDigest; 0: PasswordText */
    char   user[64];
    char   password[160];
    char   nonce[128];              /* base64 */
    char   created[48];
} onvif_soap_t;

/* A SOAP request. 0, or -1 when there is no Envelope with a Body
 * holding an element (or a field does not fit). */
int onvif_soap_parse(const char *xml, size_t len, onvif_soap_t *r);

/* The namespace URI bound to the prefix p[0..plen) (plen 0: the default
 * namespace) for the element whose '<' is at xml+at: its own start tag's
 * declarations, else the nearest one before it. 0, or -1 if undeclared. */
int onvif_xml_ns(const char *xml, size_t len, size_t at, const char *p, size_t plen, char *out, size_t cap);

/* `s` with & < > " ' as entities into out; -1 if it does not fit. */
int onvif_xml_escape(const char *s, char *out, size_t cap);

/* ---- WS-UsernameToken ----------------------------------------------------- */

/* base64(sha1(base64-decoded `nonce` + `created` + `pass`)) into out. 0,
 * or -1 when the nonce does not decode. */
int onvif_wsse_digest(const char *nonce_b64, const char *created, const char *pass, char out[32]);

/* xs:dateTime ("2026-11-27T10:00:00Z", fractions, "+01:00"; no zone is
 * UTC) as UTC seconds. 0, or -1. */
int onvif_time_parse(const char *s, time_t *t);

/* Equal strings, in a time that depends only on their lengths. */
int onvif_equal(const char *a, const char *b);

/* ---- HTTP Digest ------------------------------------------------------------ */

typedef struct {
    char username[64], realm[64], nonce[64], uri[256], response[40];
    char qop[16], nc[16], cnonce[96], algorithm[16], opaque[64];
} onvif_digest_t;

/* "Digest username=..., ..." into d. 0, or -1 when it is not Digest, a
 * field does not fit, or one it needs is missing. */
int onvif_digest_parse(const char *hdr, onvif_digest_t *d);

/* The response the client should have sent for `method` with `pass`. */
void onvif_digest_expect(const onvif_digest_t *d, const char *method, const char *pass, char out[33]);

/* A nonce for time `t` (seconds, monotonic) under `key`: 32 hex digits. */
void onvif_digest_nonce(const unsigned char key[16], long t, char out[33]);
/* How old a nonce of ours is at `now`; -1 if it is not one of ours. */
long onvif_digest_nonce_age(const unsigned char key[16], const char *nonce, long now);

/* ---- WS-Discovery --------------------------------------------------------- */

#define ONVIF_WSD_PORT  3702
#define ONVIF_WSD_GROUP "239.255.255.250"

typedef struct {
    char   msgid[160];
    char   types[512];              /* QNames, separated by white space */
    char   scopes[1024];            /* URIs, separated by white space */
    char   match_by[160];           /* "" : rfc3986 */
    size_t types_at;                /* the Types element's '<', for its prefixes */
} onvif_probe_t;

/* A Probe datagram. 0, or -1 when it is something else (a ProbeMatches,
 * a Hello, our own message looped back, garbage). */
int onvif_wsd_probe_parse(const char *xml, size_t len, onvif_probe_t *p);

/* Does the probe (parsed from xml[0..len)) ask for a device with our
 * types (dn:NetworkVideoTransmitter, tds:Device) and `scopes` (ours,
 * separated by spaces)? 1 or 0. */
int onvif_wsd_match(const char *xml, size_t len, const onvif_probe_t *p, const char *scopes);

/* Does probe scope s match our scope t, by RFC 3986 rules (scheme and
 * authority case-insensitive, then whole path segments as a prefix)? */
int onvif_scope_match(const char *s, size_t sl, const char *t, size_t tl);

/* Our side of WS-Discovery. */
typedef struct {
    const char *uuid;               /* "urn:uuid:..." endpoint reference */
    const char *scopes;
    const char *xaddrs;             /* device service URLs, separated by spaces */
    unsigned    instance;           /* AppSequence InstanceId: changes at each start */
    unsigned    msgno;              /* AppSequence MessageNumber */
    unsigned    metadata;           /* MetadataVersion: changes with the XAddrs */
} onvif_wsd_t;

/* The messages, with MessageID `msgid` ("urn:uuid:..."). Each returns its
 * length, 0 when it does not fit `cap`. */
size_t onvif_wsd_probe_matches(char *buf, size_t cap, const onvif_wsd_t *w, const char *msgid,
                               const char *relates_to);
size_t onvif_wsd_hello(char *buf, size_t cap, const onvif_wsd_t *w, const char *msgid);
size_t onvif_wsd_bye(char *buf, size_t cap, const onvif_wsd_t *w, const char *msgid);

/* A scope value with what may not go into a scope URI as %XX ("Front
 * door" -> "Front%20door"). */
void onvif_scope_encode(const char *s, char *out, size_t cap);

#endif /* ONVIF_PROTO_H */
