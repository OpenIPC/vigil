#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End to end: vigild's discovery API (docs/api.md, "Discovery") against
fake devices (fake_discovery.py) on loopback addresses.

    discovery_test.py VIGILD

Scans by unicast (broadcast and multicast need a real interface; the
scanner's code for them is the same past the send), changes addresses
over each protocol (NETIP broadcast set, NETIP over TCP, ONVIF, Dahua
RPC2) with wrong and right passwords, previews a mass change, adds one
device to a channel and the rest with auto-add, and checks that no
password shows in an answer or the log. Slow devices (answering 0.2 s to
2.5 s late) show in GET /api/v1/discovery while the scan runs, one camera
answering two protocols as one entry whose protocols grow; a scan can be
stopped.
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
PW = "camPW1"
failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)
    return cond


def free_port(kind=socket.SOCK_STREAM):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Api:
    def __init__(self, port):
        self.base = "http://127.0.0.1:%d" % port

    def call(self, method, path, body=None):
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(self.base + path, data=data, method=method)
        if data is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=15) as r:
                raw = r.read()
                return r.status, json.loads(raw) if raw else None, raw
        except urllib.error.HTTPError as e:
            raw = e.read()
            try:
                return e.code, json.loads(raw), raw
            except ValueError:
                return e.code, None, raw

    def job(self, method, path, body):
        """Start a job and wait for its end; its final state."""
        st, doc, raw = self.call(method, path, body)
        end = time.time() + 30
        while st in (200, 202) and doc and doc.get("running") and time.time() < end:
            time.sleep(0.2)
            st2, doc, raw2 = self.call("GET", "/api/v1/discovery/job")
            raw += raw2
        return st, doc, raw


def by_mac(devs):
    return {d["mac"]: d for d in devs}


def main():
    vigild = sys.argv[1]
    clip = sys.argv[2] if len(sys.argv) > 2 else None
    tmp = tempfile.mkdtemp(prefix="vigil-disc-", dir=os.getcwd())
    api_port = free_port()
    udp = [free_port(socket.SOCK_DGRAM) for _ in range(3)]
    tcp = [free_port() for _ in range(3)]
    cfg = os.path.join(tmp, "vigil.yaml")
    with open(cfg, "w") as f:
        f.write("cameras:\n  - url: \"rtsp://admin:keep@127.0.0.99/x\"\n    name: Old\n"
                "api:\n  listen: \"127.0.0.1:%d\"\n  auth: false\n"
                "netip_server:\n  enabled: false\n" % api_port)
    common = ["--ports", ",".join(map(str, udp)), "--tcp-ports", ",".join(map(str, tcp)), "--password", PW]
    fakes = [
        subprocess.Popen([sys.executable, os.path.join(HERE, "fake_discovery.py")] + common +
                         ["--device", "netip,onvif@127.0.0.10/02:00:00:00:00:10",
                          "--device", "dhip@127.0.0.11/02:00:00:00:00:11",
                          "--device", "onvif@127.0.0.12/02:00:00:00:00:12"],
                         stdout=open(os.path.join(tmp, "fake1.log"), "w"), stderr=subprocess.STDOUT),
        # A NETIP firmware that ignores the broadcast set: changed over TCP.
        subprocess.Popen([sys.executable, os.path.join(HERE, "fake_discovery.py")] + common +
                         ["--no-udp-set", "--device", "netip@127.0.0.13/02:00:00:00:00:13"],
                         stdout=open(os.path.join(tmp, "fake2.log"), "w"), stderr=subprocess.STDOUT),
    ]
    # Slow devices on ports of their own: .21 answers NETIP at once-ish and
    # ONVIF later, .22 after a second, .23 near the end of a 4 s scan.
    udp3 = [free_port(socket.SOCK_DGRAM) for _ in range(3)]
    tcp3 = [free_port() for _ in range(3)]
    fakes.append(subprocess.Popen(
        [sys.executable, os.path.join(HERE, "fake_discovery.py"), "--ports", ",".join(map(str, udp3)),
         "--tcp-ports", ",".join(map(str, tcp3)),
         "--device", "netip,onvif@127.0.0.21/02:00:00:00:00:21", "--device", "dhip@127.0.0.22/02:00:00:00:00:22",
         "--device", "onvif@127.0.0.23/02:00:00:00:00:23",
         "--delay", "netip@127.0.0.21=0.2", "--delay", "onvif@127.0.0.21=1.6", "--delay", "127.0.0.22=1.0",
         "--delay", "127.0.0.23=2.5"],
        stdout=open(os.path.join(tmp, "fake3.log"), "w"), stderr=subprocess.STDOUT))
    netip_port = free_port()
    if clip:
        fakes.append(subprocess.Popen([sys.executable, os.path.join(HERE, "netip_server.py"), clip,
                                       "--port", str(netip_port), "--password", PW],
                                      stdout=open(os.path.join(tmp, "netip.log"), "w"), stderr=subprocess.STDOUT))
    log_path = os.path.join(tmp, "vigild.log")
    vd = subprocess.Popen([vigild, "-f", cfg, "-t", "120"], stdout=open(log_path, "w"), stderr=subprocess.STDOUT)
    api = Api(api_port)
    raw_all = b""
    try:
        end = time.time() + 10
        while time.time() < end:
            try:
                if api.call("GET", "/api/v1/discovery")[0] == 200:
                    break
            except OSError:
                pass
            time.sleep(0.2)
        targets = ["127.0.0.10", "127.0.0.11", "127.0.0.12", "127.0.0.13"]
        scan = {"targets": targets, "broadcast": False, "seconds": 2,
                "ports": {"netip": udp[0], "onvif": udp[1], "dhip": udp[2]}}

        st, doc, raw = api.call("POST", "/api/v1/discovery/scan", {"protocols": ["netip", "ftp"]})
        check(st == 400, "unknown protocol refused (%d)" % st)
        st, doc, raw = api.call("POST", "/api/v1/discovery/scan", {"seconds": 99})
        check(st == 400, "seconds out of range refused (%d)" % st)
        st, doc, raw = api.call("GET", "/api/v1/discovery/scan")
        check(st == 405, "GET scan: 405 (%d)" % st)

        st, doc, raw = api.call("POST", "/api/v1/discovery/scan", scan)
        check(st == 202 and doc["kind"] == "scan" and doc["running"], "scan started: %d %s" % (st, doc))
        st2, doc2, _ = api.call("POST", "/api/v1/discovery/scan", scan)
        check(st2 == 409, "a second job while one runs: 409 (%d)" % st2)
        time.sleep(2.5)
        st, doc, raw = api.call("GET", "/api/v1/discovery")
        devs = by_mac(doc["devices"])
        check(set(devs) == {"02:00:00:00:00:1%d" % i for i in range(4)}, "four devices: %s" % sorted(devs))
        d10 = devs.get("02:00:00:00:00:10", {})
        check(d10.get("protocols") == ["netip", "onvif"] and d10.get("ip") == "127.0.0.10" and
              d10.get("ports", {}).get("netip") == tcp[0] and d10.get("onvif", "").endswith(":%d/onvif/device_service" % tcp[1]),
              "NETIP and ONVIF merged by MAC: %s" % d10)
        check(devs.get("02:00:00:00:00:11", {}).get("urls", {}).get("dhip") ==
              "rtsp://127.0.0.11:554/cam/realmonitor?channel=1&subtype=0", "Dahua URL suggested")
        check(doc["job"]["message"] == "4 devices" and not doc["job"]["running"], "scan done: %s" % doc["job"]["message"])

        # The plan for all four, from .40: nothing changes.
        st, doc, raw = api.job("POST", "/api/v1/discovery/ip",
                               {"devices": sorted(devs), "start": "127.0.0.40", "mask": "255.0.0.0",
                                "gateway": "127.0.0.1", "dry_run": True})
        plan = [r["new_ip"] for r in doc["results"]]
        check(st == 200 and plan == ["127.0.0.40", "127.0.0.41", "127.0.0.42", "127.0.0.43"] and doc["dry_run"],
              "dry run plans .40-.43: %s" % plan)
        st, doc, raw = api.call("GET", "/api/v1/discovery")
        check(all(d["ip"] in targets for d in doc["devices"]), "dry run changed nothing")
        st, doc, raw = api.job("POST", "/api/v1/discovery/ip",
                               {"devices": ["02:00:00:00:00:10"], "start": "127.0.0.10", "mask": "255.0.0.0", "dry_run": True})
        check(doc["results"][0]["new_ip"] == "127.0.0.14", "plan skips known devices' addresses: %s" % doc["results"])

        # Wrong password: refused, no move, no other protocol tried.
        for mac, proto in (("02:00:00:00:00:10", "netip"), ("02:00:00:00:00:11", "dhip"), ("02:00:00:00:00:12", "onvif"),
                           ("02:00:00:00:00:13", "netip")):
            st, doc, raw = api.job("POST", "/api/v1/discovery/ip",
                                   {"device": mac, "ip": "127.0.0.60", "mask": "255.0.0.0", "gateway": "127.0.0.1",
                                    "password": "wrong"})
            raw_all += raw
            r = doc["results"][0]
            check(not r["ok"] and r.get("error") == "auth" and r.get("protocol") == proto,
                  "%s over %s, wrong password: %s" % (mac, proto, r))

        # Right password, each protocol: the device answers at its new address.
        moves = (("02:00:00:00:00:10", "127.0.0.50", "netip"),     # broadcast set (1532)
                 ("02:00:00:00:00:11", "127.0.0.51", "dhip"),      # Dahua RPC2
                 ("02:00:00:00:00:12", "127.0.0.52", "onvif"),     # ONVIF SetNetworkInterfaces
                 ("02:00:00:00:00:13", "127.0.0.53", "netip"))     # NETIP ConfigSet over TCP
        for mac, ip, proto in moves:
            st, doc, raw = api.job("POST", "/api/v1/discovery/ip",
                                   {"device": mac, "ip": ip, "mask": "255.0.0.0", "gateway": "127.0.0.1",
                                    "password": PW})
            raw_all += raw
            r = doc["results"][0]
            check(r["ok"] and r["reprobed"] and r["protocol"] == proto and r["new_ip"] == ip,
                  "%s -> %s over %s: %s" % (mac, ip, proto, r))
        st, doc, raw = api.call("GET", "/api/v1/discovery")
        devs = by_mac(doc["devices"])
        check([devs[m]["ip"] for m, _, _ in moves] == [ip for _, ip, _ in moves] and len(devs) == 4,
              "the table has the new addresses, once each: %s" % {m: d["ip"] for m, d in devs.items()})
        log2 = open(os.path.join(tmp, "fake2.log")).read()
        check("netip tcp set -> 127.0.0.53" in log2, "the second NETIP device was changed over TCP")

        # Add one to channel 3 (channels 2 and 3 empty before), then the rest.
        st, doc, raw = api.job("POST", "/api/v1/channels/3/assign",
                               {"device": "02:00:00:00:00:11", "password": PW, "name": "Gate"})
        raw_all += raw
        r = doc["results"][0]
        check(r["ok"] and r["channel"] == 3 and r["url"] == "rtsp://admin:***@127.0.0.51:554/cam/realmonitor?channel=1&subtype=0",
              "assign to channel 3: %s" % r)
        st, doc, raw = api.job("POST", "/api/v1/channels/1/assign", {"device": "02:00:00:00:00:12", "password": PW})
        check(not doc["results"][0]["ok"] and "has a camera" in doc["results"][0]["message"], "an occupied channel is refused")
        st, doc, raw = api.job("POST", "/api/v1/channels/2/assign", {"device": "02:00:00:00:00:11", "password": PW})
        check(not doc["results"][0]["ok"] and "already on channel 3" in doc["results"][0]["message"],
              "a device already added is refused")
        st, doc, raw = api.job("POST", "/api/v1/discovery/autoadd", {"user": "admin", "password": PW})
        raw_all += raw
        got = {r["device"]: (r.get("channel"), r.get("protocol"), r.get("url")) for r in doc["results"]}
        check(got.get("02:00:00:00:00:10") == (2, "netip", "netip://admin:***@127.0.0.50:%d/?ch=0&stream=main" % tcp[0]) and
              got.get("02:00:00:00:00:12", (0,))[0] == 4 and got["02:00:00:00:00:12"][1] == "onvif" and
              got.get("02:00:00:00:00:13", (0,))[0] == 5 and "02:00:00:00:00:11" not in got,
              "auto-add fills 2, 4, 5 by address order: %s" % got)
        st, cams, raw = api.call("GET", "/api/v1/config/cameras")
        urls = [c["url"] for c in cams]
        check(len(cams) == 5 and urls[0] == "rtsp://admin:***@127.0.0.99/x" and cams[2]["name"] == "Gate" and
              urls[3] == "rtsp://admin:***@127.0.0.52:554/main?a=1&b=2" and
              cams[3]["sub_url"] == "rtsp://admin:***@127.0.0.52:554/sub?a=1&b=2" and
              cams[2]["sub_url"].endswith("subtype=1"), "the configuration: %s" % cams)
        check(cams[1]["name"] == "Porch0" and cams[3]["name"] == "fake2",
              "names: the NETIP camera's channel title, the ONVIF device's name: %s / %s" %
              (cams[1]["name"], cams[3]["name"]))
        with open(cfg) as f:
            saved = f.read()
        check(PW in saved and "keep@127.0.0.99" in saved, "the file holds the passwords (and kept the old one)")
        st, doc, raw = api.call("GET", "/api/v1/discovery")
        ch = {d["mac"]: d["channel"] for d in doc["devices"]}
        check(ch == {"02:00:00:00:00:10": 2, "02:00:00:00:00:11": 3, "02:00:00:00:00:12": 4, "02:00:00:00:00:13": 5},
              "devices know their channels: %s" % ch)

        # A device entered by hand: tested, then put on a channel.
        refused = 4
        if clip:
            def test(dev):
                st, doc, raw = api.job("POST", "/api/v1/devices/test", dev)
                return st, (doc or {}).get("results", [{}])[0], raw
            nd = {"protocol": "netip", "address": "127.0.0.1", "port": netip_port, "user": "admin",
                  "password": PW}
            st, r, raw = test(nd)
            raw_all += raw
            check(st == 200 and r.get("ok") and r.get("codec") == "H.264" and r.get("width") == 640 and
                  r.get("height") == 360 and r.get("url") == "netip://admin:***@127.0.0.1:%d/?ch=0&stream=main" % netip_port,
                  "test NETIP: H.264 640x360: %s" % r)
            st, r, raw = test(dict(nd, password="wrong"))
            raw_all += raw
            refused += 1
            check(not r.get("ok") and r.get("error") == "auth", "test NETIP, wrong password: auth: %s" % r)
            st, r, raw = test(dict(nd, port=free_port()))
            check(not r.get("ok") and r.get("error") == "unreachable", "test NETIP, nothing there: offline: %s" % r)
        od = {"protocol": "onvif", "address": "127.0.0.52", "port": tcp[1], "user": "admin", "password": PW}
        st, doc, raw = api.job("POST", "/api/v1/devices/test", od)
        raw_all += raw
        r = doc["results"][0]
        check(r.get("url") == "rtsp://admin:***@127.0.0.52:554/main?a=1&b=2" and r.get("error") == "unreachable",
              "test ONVIF: GetStreamUri, then nothing at the RTSP address: %s" % r)
        st, doc, raw = api.job("POST", "/api/v1/devices/test", dict(od, password="wrong"))
        r = doc["results"][0]
        refused += 1
        check(not r["ok"] and r.get("error") == "auth", "test ONVIF, wrong password: auth: %s" % r)
        st, doc, raw = api.job("POST", "/api/v1/devices/test", dict(od, address="127.0.0.51", port=tcp[2]))
        r = doc["results"][0]
        check(not r["ok"] and r.get("error") == "unsupported" and "not an ONVIF device" in r["message"],
              "test ONVIF at a Dahua web port: not an ONVIF device: %s" % r)
        st, doc, raw = api.call("POST", "/api/v1/devices/test", {"protocol": "ftp", "address": "x"})
        check(st == 400, "unknown protocol: 400 (%d)" % st)
        st, doc, raw = api.call("POST", "/api/v1/devices/test", {"protocol": "rtsp", "address": "a b"})
        check(st == 200 and not doc["results"][0]["ok"] and doc["results"][0]["error"] == "invalid",
              "a bad address: invalid: %s" % doc)

        st, doc, raw = api.job("POST", "/api/v1/channels/6/assign",
                               {"manual": {"protocol": "rtsp", "address": "cam.example", "port": 8554, "user": "u",
                                           "password": PW, "main": "/live/ch1", "sub": "rtsp://10.0.0.9/sub"},
                                "name": "Hand"})
        raw_all += raw
        r = doc["results"][0]
        check(r["ok"] and r["channel"] == 6 and r["url"] == "rtsp://u:***@cam.example:8554/live/ch1" and
              r["sub_url"] == "rtsp://u:***@10.0.0.9/sub", "assign RTSP by hand to 6: %s" % r)
        st, doc, raw = api.job("POST", "/api/v1/channels/6/assign",
                               {"manual": {"protocol": "dahua", "address": "10.0.0.7", "password": PW, "channel": 3}})
        check(not doc["results"][0]["ok"] and "has a camera" in doc["results"][0]["message"],
              "an occupied channel is refused")
        st, doc, raw = api.job("POST", "/api/v1/channels/6/assign",
                               {"manual": {"protocol": "dahua", "address": "10.0.0.7", "password": PW, "channel": 3},
                                "replace": True, "name": "Hand"})
        raw_all += raw
        r = doc["results"][0]
        check(r["ok"] and r["url"] == "rtsp://admin:***@10.0.0.7:554/cam/realmonitor?channel=3&subtype=0" and
              r["sub_url"].endswith("channel=3&subtype=1"), "replace with Dahua channel 3: %s" % r)
        st, doc, raw = api.job("POST", "/api/v1/channels/7/assign", {"manual": od})
        raw_all += raw
        r = doc["results"][0]
        check(r["ok"] and r["url"] == "rtsp://admin:***@127.0.0.52:554/main?a=1&b=2" and
              r["sub_url"] == "rtsp://admin:***@127.0.0.52:554/sub?a=1&b=2", "assign ONVIF by hand: %s" % r)
        st, cams, raw = api.call("GET", "/api/v1/config/cameras")
        check(len(cams) == 7 and cams[5]["url"].startswith("rtsp://admin:***@10.0.0.7") and cams[5]["name"] == "Hand"
              and cams[6]["url"].endswith("/main?a=1&b=2"), "the configuration: %s" % cams[5:])

        # Results while the scan runs: polled, they grow device by device.
        slow = {"targets": ["127.0.0.21", "127.0.0.22", "127.0.0.23"], "broadcast": False, "seconds": 4,
                "ports": {"netip": udp3[0], "onvif": udp3[1], "dhip": udp3[2]}}
        st, doc, raw = api.call("POST", "/api/v1/discovery/scan", slow)
        check(st == 202 and doc["running"] and doc.get("found") == 0, "slow scan started: %s" % doc)
        seen, t0 = [], time.time()
        while time.time() - t0 < 8:
            st, doc, raw = api.call("GET", "/api/v1/discovery")
            mine = {d["mac"]: d["protocols"] for d in doc["devices"] if d["answered"]}
            seen.append((round(time.time() - t0, 2), doc["running"], doc["job"]["found"], doc["generation"],
                         doc["count"], mine))
            if not doc["running"]:
                break
            time.sleep(0.1)
        for s_ in seen:
            print("     %5.2f s running=%s found=%d gen=%d count=%d %s" % s_)
        partial = [s_ for s_ in seen if s_[1] and 0 < s_[2] < 3]
        check(partial and all(len(s_[5]) == s_[2] for s_ in seen), "partial results while running, found = answered")
        check([s_[2] for s_ in seen] == sorted(s_[2] for s_ in seen) and seen[-1][2] == 3 and not seen[-1][1],
              "found grows to 3, then the scan ends: %s" % [s_[2] for s_ in seen])
        p21 = [s_[5].get("02:00:00:00:00:21") for s_ in seen if "02:00:00:00:00:21" in s_[5]]
        check(p21 and p21[0] == ["netip"] and p21[-1] == ["netip", "onvif"],
              "one camera, two protocols: one entry whose protocols grow: %s" % p21)
        gens = [s_[3] for s_ in seen]
        check(gens == sorted(gens) and gens[-1] > gens[0], "the generation moves with them: %s" % gens)
        first = [s_[0] for s_ in seen if "02:00:00:00:00:23" in s_[5]]
        check(first and first[0] > 2.0 and seen[-1][0] > first[0], "the slowest shows up late (%s s)" % first[:1])
        check(seen[-1][4] == len(by_mac(doc["devices"])) == 7, "count: %d" % seen[-1][4])

        # Stop: the scan ends now, keeping what it heard.
        st, doc, raw = api.call("POST", "/api/v1/discovery/scan", dict(slow, seconds=10))
        time.sleep(0.6)
        st, doc, raw = api.call("POST", "/api/v1/discovery/stop")
        check(st == 200 and not doc["running"] and doc["message"].endswith("stopped") and doc["found"] == 1,
              "stopped after 0.6 s: %d %s" % (st, doc))
        st, doc, raw = api.call("POST", "/api/v1/discovery/stop")
        check(st == 409, "nothing to stop: 409 (%d)" % st)
        st, doc, raw = api.call("GET", "/api/v1/discovery/stop")
        check(st == 405, "GET stop: 405 (%d)" % st)

        # Refused credentials are counted: 10 a minute, then 429.
        codes = []
        for _ in range(8):
            st, doc, raw = api.job("POST", "/api/v1/discovery/ip",
                                   {"device": "02:00:00:00:00:12", "ip": "127.0.0.70", "mask": "255.0.0.0",
                                    "password": "wrong"})
            codes.append(st)
            raw_all += raw
        ok_n = 10 - refused
        check(codes[:ok_n] == [200] * ok_n and set(codes[ok_n:]) == {429},
              "refused credentials are rate limited after %d: %s" % (ok_n, codes))
        st, doc, raw = api.call("POST", "/api/v1/discovery/scan", scan)
        check(st == 202, "a scan is not (%d)" % st)
        time.sleep(2.5)
    finally:
        vd.terminate()
        try:
            vd.wait(10)
        except subprocess.TimeoutExpired:
            vd.kill()
            check(False, "vigild stops")
        for f in fakes:
            f.terminate()
    log = open(log_path, "rb").read()
    check(PW.encode() not in raw_all and b"wrong" not in raw_all, "no password in the answers")
    check(PW.encode() not in log, "no password in vigild's log")
    if failures:
        print("vigild log:\n" + log.decode(errors="replace")[-3000:])
        print("%d failed" % len(failures))
        return 1
    print("all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
