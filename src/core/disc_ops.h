/* SPDX-License-Identifier: GPL-3.0-or-later */
/* disc_ops.h — what discovery does to one device over TCP, with the
 * credentials the user typed: ask an ONVIF camera for its stream URLs,
 * and change a device's address (ONVIF, Dahua RPC2, NETIP ConfigSet).
 * Blocking calls with their own time limits; run them off the UI and API
 * threads (discovery.c runs them in its job thread). Credentials are used
 * for the call and never logged or kept.
 *
 * ONVIF: SOAP 1.2 over HTTP with a WS-Security UsernameToken
 * (PasswordDigest = base64(sha1(nonce + created + password)), "created"
 * in the device's own clock, read with GetSystemDateAndTime first so a
 * board without NTP is not refused), and HTTP Digest when the device asks
 * for it with a 401. Device service: GetCapabilities (the Media service
 * address), GetNetworkInterfaces, SetNetworkDefaultGateway,
 * SetNetworkInterfaces, SystemReboot when it says RebootNeeded. Media:
 * GetProfiles, GetStreamUri (RTP-Unicast over RTSP). Source: ONVIF Core
 * and Media Service Specifications, WS-Security UsernameToken Profile 1.0.
 *
 * Dahua RPC2 over HTTP: POST /RPC2_Login global.login twice (the first,
 * without a password, returns realm and random; the second sends
 * MD5U(user:random:MD5U(user:realm:password)), "Default" encryption
 * only), then POST /RPC2 configManager.getConfig / setConfig "Network"
 * with the session. Source: OpenIPC python-dhip (dahua/transport.py,
 * login), the same JSON over HTTP as the web UI sends.
 *
 * NETIP over TCP (34567): login (msgid 1000), ConfigGet (1042) and
 * ConfigSet (1040) of "NetWork.NetCommon". Source: OpenIPC python-dvr
 * (dvrip.py: QCODES ConfigGet/ConfigSet). The broadcast set (msgid 1532)
 * needs no TCP and is tried first, by discovery.c.
 */
#ifndef DISC_OPS_H
#define DISC_OPS_H

#include <stddef.h>

#include "disc_proto.h"

/* Outcome of an operation on a device. */
typedef enum {
    DISC_OK = 0,
    DISC_EAUTH,                     /* the device refused the credentials */
    DISC_EUNSUPPORTED,              /* the device or protocol cannot do it */
    DISC_ETIMEOUT,                  /* no answer */
    DISC_EUNREACHABLE,              /* no connection */
    DISC_EPROTO,                    /* an answer we could not use */
    DISC_EINVAL,                    /* bad arguments */
} disc_rc_t;

const char *disc_rc_name(disc_rc_t rc);   /* "ok", "auth", "unsupported", ... */

typedef struct {
    int  status;                    /* HTTP status; 0 none */
    char www_auth[384];             /* the Digest challenge of a 401 */
    char *body;                     /* malloc'd, NUL-terminated */
    size_t len;
} disc_http_t;

/* POST `body` to http://host:port/path. 0 with `r` filled (free r->body),
 * or -1 with `err`. At most 256 KiB of answer is kept. */
int disc_http_post(const char *host, int port, const char *path, const char *ctype, const char *auth,
                   const char *body, int timeout_ms, disc_http_t *r, char *err, size_t errcap);

/* ONVIF: the main and sub-stream RTSP URLs (the first two media
 * profiles), with user:pass put into them. `sub` may come back "". */
disc_rc_t disc_onvif_stream_uris(const disc_device_t *d, const char *user, const char *pass,
                                 char *url, size_t ucap, char *sub, size_t scap, char *err, size_t errcap);

/* ONVIF: the device's address to ip/mask (static) and the default gateway. */
disc_rc_t disc_onvif_set_ip(const disc_device_t *d, const char *ip, const char *mask, const char *gw,
                            const char *user, const char *pass, char *err, size_t errcap);

/* Dahua RPC2 (HTTP): the default interface's address. */
disc_rc_t disc_dahua_set_ip(const disc_device_t *d, const char *ip, const char *mask, const char *gw,
                            const char *user, const char *pass, char *err, size_t errcap);

/* NETIP TCP: NetWork.NetCommon's HostIP, Submask, GateWay. */
disc_rc_t disc_netip_tcp_set_ip(const disc_device_t *d, const char *ip, const char *mask, const char *gw,
                                const char *user, const char *pass, char *err, size_t errcap);

/* Exposed for tests: the WS-Security header for user/pass at `created`
 * ("2026-11-26T12:00:00Z") with the raw 16-byte `nonce`. */
int disc_wsse_header(const char *user, const char *pass, const char *created, const unsigned char nonce[16],
                     char *out, size_t cap);

/* Exposed for tests: an rtsp:// URL with user:pass put in (replacing any). */
int disc_url_with_creds(const char *uri, const char *user, const char *pass, char *out, size_t cap);

/* Exposed for tests: the Dahua RPC2 login hash. out: 33 bytes. */
void disc_dahua_hash(const char *user, const char *pass, const char *realm, const char *random, char out[33]);

#endif /* DISC_OPS_H */
