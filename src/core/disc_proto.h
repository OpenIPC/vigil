/* SPDX-License-Identifier: GPL-3.0-or-later */
/* disc_proto.h — the wire formats of LAN camera discovery, as pure
 * functions: build a search, parse a reply into a disc_device_t. No
 * sockets here (discovery.c sends and receives); every parser takes
 * untrusted bytes with their length and never reads past it.
 *
 * NETIP (Xiongmai "DVRIP", UDP 34569). The 20-byte NETIP header
 * (source_netip.c) with msgid 1530 and no body is the search; devices
 * answer with msgid 1531 and a JSON body {"NetWork.NetCommon": {HostIP,
 * Submask, GateWay: "0x0A01A8C0" = 192.168.1.10, i.e. the address as a
 * little-endian u32 in hex; MAC, TCPPort, HttpPort, SN, HostName,
 * DeviceType, ChannelNum, Version, ...}, "Ret": 100}. The answer goes to
 * the broadcast address, port 34569, not back to the asker: the scanner
 * listens on 34569. msgid 1532 (JSON: the NetCommon fields to set plus
 * DvrMac, Username, Password = sofia hash, EncryptType 1) changes a
 * device's address by broadcast, answered by 1533 {"Ret": 100}.
 * Source: OpenIPC python-dvr, DeviceManager.py (SearchXM, ConfigXM).
 *
 * ONVIF WS-Discovery (UDP 3702, multicast 239.255.255.250). A SOAP 1.2
 * Probe for dn:NetworkVideoTransmitter; devices answer to the asker with
 * ProbeMatches: EndpointReference/Address (urn:uuid:...), Scopes
 * (onvif://www.onvif.org/name/..., /hardware/..., /location/...,
 * vendors add /Manufacturer/, /MAC/), XAddrs (the device service URLs).
 * Namespace prefixes differ between devices (d:/wsdd:, dn:/tdn:), so
 * elements are matched by local name. Source: WS-Discovery 2005/04
 * (schemas.xmlsoap.org/ws/2005/04/discovery), ONVIF Core Specification,
 * section 7 "Device discovery".
 *
 * DHIP (Dahua, UDP 37810, multicast 239.255.255.251 and broadcast). A
 * 32-byte header of eight little-endian u32 -- 0x20 (header size), "DHIP"
 * (0x50494844), session id, request id, packet length, packet index,
 * message length (JSON), data length (binary after the JSON) -- and a
 * JSON body. The search is {"method":"DHDiscover.search","params":{"mac":
 * "","uni":1}}; devices answer {"method":"client.notifyDevInfo","mac":..,
 * "params":{"deviceInfo":{DeviceType, DeviceClass, IPv4Address
 * {IPAddress, SubnetMask, DefaultGateway, DhcpEnable}, MachineName,
 * HttpPort, Port, SerialNo, Vendor, Version, ...}}}. Sources: OpenIPC
 * python-dhip (dahua/const.py: the header; dahua/discovery.py) and
 * mcw0/DahuaConsole (net.py, dh_discover); Wireshark has no DHIP
 * dissector to compare with.
 */
#ifndef DISC_PROTO_H
#define DISC_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define DISC_NETIP 1u
#define DISC_ONVIF 2u
#define DISC_DHIP  4u
#define DISC_ALL   (DISC_NETIP | DISC_ONVIF | DISC_DHIP)

#define DISC_NETIP_UDP   34569
#define DISC_ONVIF_UDP   3702
#define DISC_DHIP_UDP    37810
#define DISC_ONVIF_GROUP "239.255.255.250"
#define DISC_DHIP_GROUP  "239.255.255.251"

#define DISC_NETIP_SEARCH     1530
#define DISC_NETIP_SEARCH_REP 1531
#define DISC_NETIP_SET        1532
#define DISC_NETIP_SET_REP    1533

typedef struct {
    char     mac[18];               /* "aa:bb:cc:dd:ee:ff"; "" unknown */
    char     ip[16], mask[16], gateway[16];
    int      dhcp;                  /* 1, 0, -1 unknown */
    unsigned protos;                /* DISC_* that answered */
    int      netip_port;            /* NETIP TCP (34567) */
    int      http_port;             /* web / ONVIF / Dahua RPC2 */
    int      rtsp_port;             /* 0: the default 554 */
    int      dh_port;               /* Dahua TCP (37777) */
    int      channels;              /* video inputs (NVRs and DVRs: more than 1) */
    char     vendor[32];
    char     model[64];             /* device type / hardware */
    char     name[64];              /* host name, machine name, ONVIF name */
    char     serial[48];
    char     version[48];
    char     xaddr[160];            /* ONVIF device service URL */
    char     uuid[64];              /* WS-Discovery endpoint reference */
    char     iface[16];             /* where it was heard */
    time_t   seen;                  /* monotonic seconds */
    unsigned scan;                  /* the scan job that heard it last; 0 none */
} disc_device_t;

/* "netip", "onvif", "dhip". */
const char *disc_proto_name(unsigned proto);
/* Bits from "netip,onvif" / "all"; 0 on an unknown name. */
unsigned disc_proto_bits(const char *list);

/* ---- NETIP --------------------------------------------------------------- */

/* The 20-byte search (msgid 1530). Returns its length. */
size_t disc_netip_search(uint8_t *buf, size_t cap);

/* A search reply (msgid 1531) into `d` (cleared first). 0, or -1 if it is
 * not one (another msgid, our own search echoed, malformed). */
int disc_netip_parse(const uint8_t *pkt, size_t len, disc_device_t *d);

/* A search (msgid 1530), as a device hears it: 1 or 0. */
int disc_netip_is_search(const uint8_t *pkt, size_t len);

/* The answer of a device that is `d` (ip, mask, gateway, mac, netip_port,
 * http_port, serial, name, version, channels) to a search: msgid 1531,
 * as disc_netip_parse reads it; no DeviceType, which XM devices leave
 * out. Returns its length, 0 if it does not fit `cap` (an XM search tool
 * reads 1 KiB) or `d` has no address. */
size_t disc_netip_reply(uint8_t *buf, size_t cap, const disc_device_t *d);

/* The broadcast set (msgid 1532) for device `d` (its MAC and ports) to
 * ip/mask/gw, with the sofia hash of `pass`. Returns the length, 0 if it
 * does not fit `cap` or `d` has no MAC. */
size_t disc_netip_set(uint8_t *buf, size_t cap, const disc_device_t *d, const char *ip,
                      const char *mask, const char *gw, const char *user, const char *pass);

/* A set reply (msgid 1533): its Ret (100 = done, 203 wrong password, ...),
 * or -1 if it is not one. */
int disc_netip_set_reply(const uint8_t *pkt, size_t len);

/* "0x0A01A8C0" <-> "192.168.1.10". -1 if malformed. */
int disc_netip_hexip(const char *hex, char *out, size_t cap);
int disc_netip_iphex(const char *ip, char *out, size_t cap);

/* ---- ONVIF WS-Discovery ------------------------------------------------- */

/* A Probe with MessageID `msgid` ("uuid:..."). Returns its length, 0 if it
 * does not fit. */
size_t disc_onvif_probe(char *buf, size_t cap, const char *msgid);

/* The first ProbeMatch of a ProbeMatches message into `d`. 0, or -1. */
int disc_onvif_parse(const char *xml, size_t len, disc_device_t *d);

/* The text of the first element named `local` (any namespace prefix)
 * inside xml[0..len) — entities &amp; &lt; &gt; &quot; &apos; decoded,
 * surrounding white space trimmed — into `out`. 0, or -1 if there is no
 * such element or it does not fit. With `from`, the search starts there
 * and *from is set past the element. */
int disc_xml_text(const char *xml, size_t len, const char *local, char *out, size_t cap,
                  size_t *from);

/* The value of attribute `attr` of the first element `local`, as above. */
int disc_xml_attr(const char *xml, size_t len, const char *local, const char *attr, char *out,
                  size_t cap, size_t *from);

/* Where the first element `local` at or after xml+start is: the offset of
 * its '<' (*tag), its content [*c0, *c1) and the end of the element
 * (*after); any may be NULL. 0, or -1 if there is none. */
int disc_xml_elem(const char *xml, size_t len, const char *local, size_t start, size_t *tag, size_t *c0,
                  size_t *c1, size_t *after);

/* ---- DHIP ---------------------------------------------------------------- */

/* The DHDiscover.search datagram. Returns its length. */
size_t disc_dhip_search(uint8_t *buf, size_t cap);

/* A client.notifyDevInfo reply into `d`. 0, or -1 (our own search heard
 * back, malformed, another method). */
int disc_dhip_parse(const uint8_t *pkt, size_t len, disc_device_t *d);

/* ---- devices --------------------------------------------------------------- */

/* "AA-BB-CC-DD-EE-FF", "aabb.ccdd.eeff", "AABBCCDDEEFF" -> "aa:bb:cc:dd:ee:ff".
 * -1 (and "") if not a MAC. */
int disc_mac_norm(const char *in, char *out, size_t cap);

/* The same device: equal MACs, or (one MAC unknown) equal IPs. */
int disc_same(const disc_device_t *a, const disc_device_t *b);

/* Fold what `in` says into `d` (same device): fields `d` lacks, the
 * protocols, the newest address and time. */
void disc_merge(disc_device_t *d, const disc_device_t *in);

/* One answer into a table of `*n` devices (at most `max`): merged into
 * the entry of the same device, else appended at the end (the entry heard
 * least recently goes when the table is full). An entry without a MAC
 * that turns out to be the device just heard (its address) is folded in
 * and removed, the others keeping their order. 1 if the table changed in
 * anything but the time last heard, else 0: a caller can show a device
 * as its answers arrive and redraw only when there is news. */
int disc_table_add(disc_device_t *tab, int *n, int max, const disc_device_t *in);

/* Text that can go into a camera URL as user or password: no ':' (user),
 * '@', '/', '?', '#', white space, control characters, quotes or '\'. */
int disc_cred_ok(const char *s, int is_user);

/* The source URLs for `d` over `proto` (DISC_NETIP or DISC_DHIP: fixed
 * forms; ONVIF needs GetStreamUri, discovery.c) with credentials:
 *   NETIP  netip://user:pass@ip:port/?ch=0&stream=main   (sub: derived)
 *   DHIP   rtsp://user:pass@ip:554/cam/realmonitor?channel=1&subtype=0
 *          sub_url ...subtype=1
 * 0, or -1 (unsupported, credentials that a URL cannot carry, too long). */
int disc_url(const disc_device_t *d, unsigned proto, const char *user, const char *pass,
             char *url, size_t ucap, char *sub, size_t scap);

/* The host of a camera URL ("rtsp://u:p@10.0.0.5:554/x" -> "10.0.0.5"),
 * "" for capture: and malformed ones. */
void disc_url_host(const char *url, char *host, size_t cap);

/* IPv4 helpers: dotted quad <-> host-order u32. -1 if malformed. */
int  disc_ip_parse(const char *s, uint32_t *v);
void disc_ip_str(uint32_t v, char *out, size_t cap);
/* A contiguous netmask ("255.255.255.0"); its prefix length or -1. */
int  disc_mask_prefix(const char *mask);

#endif /* DISC_PROTO_H */
