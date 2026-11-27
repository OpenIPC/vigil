#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fake LAN cameras for vigil's discovery tests: NETIP, ONVIF and DHIP.

Each device answers the searches of the protocols it is given and lets
its address be changed, as real ones do (src/core/disc_proto.h has the
formats):

  NETIP  UDP search 1530 -> 1531 NetWork.NetCommon; UDP set 1532 -> 1533
         (MAC, user and sofia hash checked); TCP login 1000, ConfigGet
         1042 / ConfigSet 1040 of NetWork.NetCommon
  ONVIF  WS-Discovery Probe -> ProbeMatches; HTTP SOAP with a checked
         WS-Security UsernameToken: GetSystemDateAndTime, GetCapabilities,
         GetProfiles, GetStreamUri, GetNetworkInterfaces,
         SetNetworkDefaultGateway, SetNetworkInterfaces
  DHIP   UDP DHDiscover.search -> client.notifyDevInfo; HTTP RPC2 login
         (realm/random challenge), configManager.getConfig/setConfig Network

    fake_discovery.py --device netip,onvif@127.0.0.10/02:00:00:00:00:10 \\
                      --device dhip@127.0.0.11 [--password PW] [--ports P]

Each device binds its own address (127.0.0.0/8 works on Linux without
setup) and moves when its address is changed, so a test sees it answer at
the new one. --ports N,M,K sets the UDP ports (netip, onvif, dhip); the TCP
ports (--tcp-ports) 34567 (NETIP), 8899 (ONVIF HTTP) and 8080 (Dahua HTTP)
on the device's address. --shared ADDR answers on 0.0.0.0 instead (one socket
per protocol for all devices), joins the multicast groups on the
interface with address ADDR, and sends NETIP answers to the broadcast
address --netip-reply-to, as Xiongmai devices do: for broadcast and
multicast tests on a real interface. Prints one line per request.
"""
import argparse
import base64
import hashlib
import json
import os
import socket
import socketserver
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

LOCK = threading.Lock()


def log(*a):
    with LOCK:
        print("fake:", *a, flush=True)


def sofia(password):
    d = hashlib.md5(password.encode()).digest()
    tab = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
    return "".join(tab[(d[2 * i] + d[2 * i + 1]) % 62] for i in range(8))


def iphex(ip):
    return "0x%08X" % struct.unpack("<I", socket.inet_aton(ip))[0]


def hexip(h):
    return socket.inet_ntoa(struct.pack("<I", int(h, 16)))


def prefix_mask(n):
    return socket.inet_ntoa(struct.pack(">I", (0xFFFFFFFF << (32 - n)) & 0xFFFFFFFF))


def netip_msg(msgid, body, session=0):
    raw = json.dumps(body).encode() + b"\n\x00"
    return struct.pack("<BB2xII2xHI", 0xFF, 1, session, 0, msgid, len(raw)) + raw


def dhip_msg(obj):
    raw = json.dumps(obj).encode()
    return struct.pack("<IIIIIIII", 0x20, 0x50494844, 0, 0, len(raw), 0, len(raw), 0) + raw


def xml_text(body, local):
    """Text of the first element with this local name (no namespaces)."""
    import re
    m = re.search(r"<(?:[\w-]+:)?%s(?:\s[^>]*)?>(.*?)</(?:[\w-]+:)?%s>" % (local, local), body, re.S)
    return m.group(1).strip() if m else None


class Device:
    def __init__(self, spec, args, index):
        protos, _, rest = spec.partition("@")
        ip, _, mac = rest.partition("/")
        self.protos = set(protos.split(","))
        self.ip, self.mask, self.gw = ip, "255.0.0.0", "127.0.0.1"
        self.mac = mac or "02:00:00:00:00:%02x" % (0x10 + index)
        self.name = "fake%d" % index
        self.args = args
        self.index = index
        self.socks = []
        self.servers = []
        self.realm, self.random = "Login to fake%d" % index, "%d" % (1000 + index)
        self.sessions = set()

    def tcp(self, k):
        """TCP port k (netip, onvif, dahua): with --tcp-bind every device
        shares one address, so each gets its own ports (base + index)."""
        return self.args.tcp[k] + (self.index if self.args.tcp_bind else 0)

    # -- binding -------------------------------------------------------------
    def start(self):
        a = self.args
        if not a.shared:
            for proto, port in (("netip", a.ports[0]), ("onvif", a.ports[1]), ("dhip", a.ports[2])):
                if proto in self.protos:
                    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                    s.bind((self.ip, port))
                    self.socks.append(s)
                    threading.Thread(target=udp_loop, args=(s, [self], proto), daemon=True).start()
        if a.shared and not a.tcp_bind:
            return                  # discovery only: the addresses are made up
        if "netip" in self.protos:
            self.serve_tcp(self.tcp(0), NetipTcp)
        if "onvif" in self.protos:
            self.serve_http(self.tcp(1), OnvifHttp)
        if "dhip" in self.protos:
            self.serve_http(self.tcp(2), DahuaHttp)

    def serve_tcp(self, port, handler):
        srv = socketserver.ThreadingTCPServer((self.args.tcp_bind or self.ip, port), handler,
                                              bind_and_activate=False)
        srv.allow_reuse_address = True
        srv.daemon_threads = True
        srv.server_bind()
        srv.server_activate()
        srv.device = self
        self.servers.append(srv)
        threading.Thread(target=srv.serve_forever, daemon=True).start()

    def serve_http(self, port, handler):
        class Srv(socketserver.ThreadingMixIn, HTTPServer):
            allow_reuse_address = True
            daemon_threads = True
        srv = Srv((self.args.tcp_bind or self.ip, port), handler)
        srv.device = self
        self.servers.append(srv)
        threading.Thread(target=srv.serve_forever, daemon=True).start()

    def stop(self):
        for s in self.socks:
            s.close()
        for srv in self.servers:
            threading.Thread(target=lambda v=srv: (v.shutdown(), v.server_close()), daemon=True).start()
        self.socks, self.servers = [], []

    def move(self, ip, mask=None, gw=None, delay=0.3):
        """Change the address, as a device restarting its network does."""
        def later():
            time.sleep(delay)
            if self.args.shared:
                self.ip = ip
            else:
                self.stop()
                time.sleep(0.2)
                self.ip = ip
                self.start()
            log("%s now at %s" % (self.mac, ip))
        if mask:
            self.mask = mask
        if gw:
            self.gw = gw
        threading.Thread(target=later, daemon=True).start()

    # -- answers -------------------------------------------------------------
    def netcommon(self):
        return {"BuildDate": "2024-01-01 00:00:00", "ChannelNum": 1, "DeviceType": 1,
                "GateWay": iphex(self.gw), "HostIP": iphex(self.ip), "HostName": self.name,
                "HttpPort": 80, "MAC": self.mac, "MaxBps": 0, "MonMode": "TCP", "SN": "SN" + self.mac[-5:],
                "SSLPort": 8443, "Submask": iphex(self.mask), "TCPMaxConn": 10, "TCPPort": self.tcp(0),
                "TransferPlan": "Quality", "UDPPort": 34568, "UseHSDownLoad": False,
                "Version": "V5.00.R02.fake"}

    def probe_match(self, relates):
        return ('<?xml version="1.0" encoding="UTF-8"?><SOAP-ENV:Envelope '
                'xmlns:SOAP-ENV="http://www.w3.org/2003/05/soap-envelope" '
                'xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing" '
                'xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery" '
                'xmlns:dn="http://www.onvif.org/ver10/network/wsdl"><SOAP-ENV:Header>'
                '<wsa:MessageID>uuid:fake-%s</wsa:MessageID><wsa:RelatesTo>%s</wsa:RelatesTo>'
                '<wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</wsa:Action>'
                '</SOAP-ENV:Header><SOAP-ENV:Body><d:ProbeMatches><d:ProbeMatch><wsa:EndpointReference>'
                '<wsa:Address>urn:uuid:fake-%s</wsa:Address></wsa:EndpointReference>'
                '<d:Types>dn:NetworkVideoTransmitter</d:Types><d:Scopes>onvif://www.onvif.org/name/%s '
                'onvif://www.onvif.org/hardware/FAKE-ONVIF onvif://www.onvif.org/Manufacturer/Fake '
                'onvif://www.onvif.org/MAC/%s</d:Scopes><d:XAddrs>http://%s:%d/onvif/device_service</d:XAddrs>'
                '<d:MetadataVersion>1</d:MetadataVersion></d:ProbeMatch></d:ProbeMatches></SOAP-ENV:Body>'
                '</SOAP-ENV:Envelope>' % (self.mac, relates, self.mac, self.name, self.mac.upper(), self.ip,
                                         self.tcp(1)))

    def devinfo(self):
        return {"mac": self.mac, "method": "client.notifyDevInfo", "params": {"deviceInfo": {
            "DeviceClass": "IPC", "DeviceType": "FAKE-DH", "HttpPort": self.tcp(2), "Port": 37777,
            "IPv4Address": {"IPAddress": self.ip, "SubnetMask": self.mask, "DefaultGateway": self.gw,
                            "DhcpEnable": False},
            "MachineName": self.name, "SerialNo": "DH" + self.mac[-5:], "Vendor": "Fake",
            "Version": "2.800.fake", "VideoInputChannels": 1}}}


def udp_loop(sock, devices, proto):
    a = devices[0].args
    while True:
        try:
            data, peer = sock.recvfrom(65535)
        except OSError:
            return
        for dev in devices:
            try:
                answer_udp(sock, dev, proto, data, peer, a)
            except (OSError, ValueError, KeyError) as e:
                log("udp error", e)


def answer_udp(sock, dev, proto, data, peer, a):
    if proto == "netip" and len(data) >= 20 and data[0] == 0xFF:
        msgid = struct.unpack_from("<H", data, 14)[0]
        to = (a.netip_reply_to, 34569) if a.netip_reply_to else peer
        if msgid == 1530:
            log("netip search from %s:%d -> %s" % (peer[0], peer[1], dev.ip))
            sock.sendto(netip_msg(1531, {"NetWork.NetCommon": dev.netcommon(), "Ret": 100,
                                         "SessionID": "0x00000000"}), to)
        elif msgid == 1532 and not a.no_udp_set:
            js = json.loads(data[20:].rstrip(b"\x00\n").decode())
            if js.get("DvrMac", "").lower() != dev.mac:
                return
            ok = js.get("Username") == a.user and js.get("Password") == sofia(a.password)
            log("netip set %s -> %s %s" % (dev.mac, hexip(js["HostIP"]), "ok" if ok else "REFUSED"))
            sock.sendto(netip_msg(1533, {"Ret": 100 if ok else 203, "SessionID": "0x00000000"}), to)
            if ok:
                dev.move(hexip(js["HostIP"]), hexip(js["Submask"]), hexip(js["GateWay"]))
    elif proto == "onvif" and b"Probe" in data and b"ProbeMatch" not in data:
        mid = xml_text(data.decode(errors="replace"), "MessageID") or ""
        log("onvif probe from %s:%d -> %s" % (peer[0], peer[1], dev.ip))
        sock.sendto(dev.probe_match(mid).encode(), peer)
    elif proto == "dhip" and data[4:8] == b"DHIP" and b"DHDiscover.search" in data:
        log("dhip search from %s:%d -> %s" % (peer[0], peer[1], dev.ip))
        sock.sendto(dhip_msg(dev.devinfo()), peer)


class NetipTcp(socketserver.BaseRequestHandler):
    def recv(self):
        h = b""
        while len(h) < 20:
            c = self.request.recv(20 - len(h))
            if not c:
                return None, None
            h += c
        length = struct.unpack_from("<I", h, 16)[0]
        body = b""
        while len(body) < length:
            c = self.request.recv(length - len(body))
            if not c:
                return None, None
            body += c
        return struct.unpack_from("<H", h, 14)[0], json.loads(body.rstrip(b"\x00\n").decode() or "{}")

    def handle(self):
        dev, a = self.server.device, self.server.device.args
        session, authed = 0x55, False
        while True:
            msgid, js = self.recv()
            if msgid is None:
                return
            if msgid == 1000:
                authed = js.get("UserName") == a.user and js.get("PassWord") == sofia(a.password)
                log("netip tcp login %s" % ("ok" if authed else "REFUSED"))
                self.request.sendall(netip_msg(1001, {"Ret": 100 if authed else 203, "SessionID": "0x%08X" % session,
                                                      "AliveInterval": 20}, session))
                if not authed:
                    return
            elif msgid == 1042 and authed:
                self.request.sendall(netip_msg(1043, {"Name": "NetWork.NetCommon", "Ret": 100,
                                                      "NetWork.NetCommon": dev.netcommon()}, session))
            elif msgid == 1040 and authed:
                nc = js.get("NetWork.NetCommon", {})
                log("netip tcp set -> %s" % hexip(nc["HostIP"]))
                self.request.sendall(netip_msg(1041, {"Name": "NetWork.NetCommon", "Ret": 100}, session))
                dev.move(hexip(nc["HostIP"]), hexip(nc["Submask"]), hexip(nc["GateWay"]))
            else:
                self.request.sendall(netip_msg(msgid + 1, {"Ret": 607 if not authed else 100}, session))


class Quiet(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def reply(self, status, body, ctype):
        raw = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(raw)
        self.close_connection = True

    def body(self):
        return self.rfile.read(int(self.headers.get("Content-Length", "0"))).decode(errors="replace")


SOAP = ('<?xml version="1.0" encoding="UTF-8"?><s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" '
        'xmlns:tt="http://www.onvif.org/ver10/schema" xmlns:tds="http://www.onvif.org/ver10/device/wsdl" '
        'xmlns:trt="http://www.onvif.org/ver10/media/wsdl"><s:Body>%s</s:Body></s:Envelope>')


class OnvifHttp(Quiet):
    def authorized(self, body):
        a = self.server.device.args
        user, pw = xml_text(body, "Username"), xml_text(body, "Password")
        nonce, created = xml_text(body, "Nonce"), xml_text(body, "Created")
        if not (user and pw and nonce and created):
            return False
        want = base64.b64encode(hashlib.sha1(base64.b64decode(nonce) + created.encode() +
                                             a.password.encode()).digest()).decode()
        return user == a.user and pw == want

    def do_POST(self):
        dev = self.server.device
        body = self.body()
        action = None
        for name in ("GetSystemDateAndTime", "GetCapabilities", "GetProfiles", "GetStreamUri",
                     "GetNetworkInterfaces", "SetNetworkDefaultGateway", "SetNetworkInterfaces", "SystemReboot"):
            if "%s>" % name in body or "%s/>" % name in body:
                action = name
                break
        if action == "GetSystemDateAndTime":
            t = time.gmtime()
            return self.reply(200, SOAP % (
                "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime><tt:UTCDateTime><tt:Time>"
                "<tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second></tt:Time><tt:Date>"
                "<tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date></tt:UTCDateTime>"
                "</tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>" %
                (t.tm_hour, t.tm_min, t.tm_sec, t.tm_year, t.tm_mon, t.tm_mday)), "application/soap+xml")
        if not self.authorized(body):
            log("onvif %s NOT AUTHORIZED" % action)
            return self.reply(400, SOAP % (
                "<s:Fault><s:Code><s:Value>s:Sender</s:Value><s:Subcode><s:Value>ter:NotAuthorized</s:Value>"
                "</s:Subcode></s:Code><s:Reason><s:Text xml:lang=\"en\">Sender not Authorized</s:Text>"
                "</s:Reason></s:Fault>"), "application/soap+xml")
        log("onvif %s" % action)
        if action == "GetCapabilities":
            out = ("<tds:GetCapabilitiesResponse><tds:Capabilities><tt:Media><tt:XAddr>http://%s:%d/onvif/media"
                   "</tt:XAddr></tt:Media></tds:Capabilities></tds:GetCapabilitiesResponse>" % (dev.ip, dev.tcp(1)))
        elif action == "GetProfiles":
            out = ('<trt:GetProfilesResponse><trt:Profiles token="main" fixed="true"><tt:Name>main</tt:Name>'
                   '</trt:Profiles><trt:Profiles token="sub"><tt:Name>sub</tt:Name></trt:Profiles>'
                   '</trt:GetProfilesResponse>')
        elif action == "GetStreamUri":
            tok = xml_text(body, "ProfileToken")
            out = ("<trt:GetStreamUriResponse><trt:MediaUri><tt:Uri>rtsp://%s:%d/%s?a=1&amp;b=2</tt:Uri>"
                   "</trt:MediaUri></trt:GetStreamUriResponse>" % (dev.ip, dev.args.rtsp_port, tok))
        elif action == "GetNetworkInterfaces":
            out = ('<tds:GetNetworkInterfacesResponse><tds:NetworkInterfaces token="eth0"><tt:Enabled>true'
                   '</tt:Enabled></tds:NetworkInterfaces></tds:GetNetworkInterfacesResponse>')
        elif action == "SetNetworkDefaultGateway":
            dev.gw = xml_text(body, "IPv4Address")
            out = "<tds:SetNetworkDefaultGatewayResponse/>"
        elif action == "SetNetworkInterfaces":
            ip, pl = xml_text(body, "Address"), int(xml_text(body, "PrefixLength"))
            out = "<tds:SetNetworkInterfacesResponse><tds:RebootNeeded>false</tds:RebootNeeded></tds:SetNetworkInterfacesResponse>"
            self.reply(200, SOAP % out, "application/soap+xml")
            dev.move(ip, prefix_mask(pl))
            return
        else:
            out = ""
        self.reply(200, SOAP % out, "application/soap+xml")


class DahuaHttp(Quiet):
    def do_POST(self):
        dev, a = self.server.device, self.server.device.args
        if self.path not in ("/RPC2_Login", "/RPC2"):
            self.body()
            return self.reply(404, "<html><body>Not Found</body></html>", "text/html")
        js = json.loads(self.body() or "{}")
        m, p = js.get("method"), js.get("params") or {}
        if self.path == "/RPC2_Login":
            if not p.get("password"):
                log("dahua login challenge")
                return self.reply(200, json.dumps({"error": {"code": 268632079, "message": "Component error"},
                                                   "id": js.get("id"), "params": {"encryption": "Default",
                                                   "random": dev.random, "realm": dev.realm}, "result": False,
                                                   "session": "S1"}), "application/json")
            u = lambda s: hashlib.md5(s.encode()).hexdigest().upper()
            want = u("%s:%s:%s" % (a.user, dev.random, u("%s:%s:%s" % (a.user, dev.realm, a.password))))
            ok = p.get("userName") == a.user and p.get("password") == want and js.get("session") == "S1"
            log("dahua login %s" % ("ok" if ok else "REFUSED"))
            if ok:
                dev.sessions.add("S1")
            return self.reply(200, json.dumps({"id": js.get("id"), "params": None, "result": ok, "session": "S1",
                                               **({} if ok else {"error": {"code": 268632080}})}), "application/json")
        if self.path != "/RPC2" or js.get("session") not in dev.sessions:
            return self.reply(200, json.dumps({"id": js.get("id"), "result": False,
                                               "error": {"code": 287637505, "message": "Invalid session"}}),
                              "application/json")
        if m == "configManager.getConfig" and p.get("name") == "Network":
            log("dahua getConfig Network")
            table = {"DefaultInterface": "eth0", "Domain": "dahua", "Hostname": dev.name,
                     "eth0": {"DefaultGateway": dev.gw, "DhcpEnable": False, "IPAddress": dev.ip, "MTU": 1500,
                              "PhysicalAddress": dev.mac, "SubnetMask": dev.mask}}
            return self.reply(200, json.dumps({"id": js.get("id"), "params": {"table": table}, "result": True,
                                               "session": "S1"}), "application/json")
        if m == "configManager.setConfig" and p.get("name") == "Network":
            e = p["table"]["eth0"]
            log("dahua setConfig Network -> %s" % e["IPAddress"])
            self.reply(200, json.dumps({"id": js.get("id"), "params": None, "result": True, "session": "S1"}),
                       "application/json")
            dev.move(e["IPAddress"], e["SubnetMask"], e.get("DefaultGateway"))
            return
        self.reply(200, json.dumps({"id": js.get("id"), "result": False, "error": {"code": 405}}),
                   "application/json")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--device", action="append", required=True, help="PROTOS@IP[/MAC]")
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default="")
    ap.add_argument("--ports", default="34569,3702,37810", help="UDP ports: netip,onvif,dhip")
    ap.add_argument("--tcp-ports", default="34567,8899,8080", help="TCP ports: netip, onvif http, dahua http")
    ap.add_argument("--tcp-bind", help="serve TCP on this address instead of the device's, device i on "
                    "the TCP ports + i (devices behind a NAT or a QEMU guestfwd)")
    ap.add_argument("--rtsp-port", type=int, default=554, help="port of the RTSP URLs GetStreamUri gives")
    ap.add_argument("--shared", help="answer on 0.0.0.0, multicast on the interface with this address")
    ap.add_argument("--netip-reply-to", help="send NETIP answers to this broadcast address, port 34569")
    ap.add_argument("--no-udp-set", action="store_true", help="ignore NETIP's broadcast set (1532)")
    a = ap.parse_args()
    a.ports = [int(x) for x in a.ports.split(",")]
    a.tcp = [int(x) for x in a.tcp_ports.split(",")]
    devices = [Device(spec, a, i) for i, spec in enumerate(a.device)]
    for d in devices:
        d.start()
    if a.shared:
        for proto, port, group in (("netip", a.ports[0], None), ("onvif", a.ports[1], "239.255.255.250"),
                                   ("dhip", a.ports[2], "239.255.255.251")):
            mine = [d for d in devices if proto in d.protos]
            if not mine:
                continue
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            s.bind(("", port))
            if group:
                s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                             socket.inet_aton(group) + socket.inet_aton(a.shared))
            threading.Thread(target=udp_loop, args=(s, mine, proto), daemon=True).start()
    log("ready: " + " ".join("%s@%s" % (",".join(sorted(d.protos)), d.ip) for d in devices))
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
