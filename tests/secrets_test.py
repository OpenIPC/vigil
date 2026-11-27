#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end test of the passwords vigild keeps (docs/security.md).

    secrets_test.py VIGILD CLIP

A password set through the API -- a new user's, and the system account's
(root, in a shadow file of the test's own) -- works at once on every
server that never sees the password itself: RTSP (Digest), ONVIF
(WS-UsernameToken) and NETIP (Xiongmai's hash); and still after vigild
restarts, as does the camera, whose password is sealed in vigil.yaml.
None of it is in the clear in any file, and the API shows none of it.
"""
import base64
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

import netip_srv_test
import onvif_test
import rtsp_server_test
import vigil_secret

HERE = os.path.dirname(os.path.abspath(__file__))
HASH = netip_srv_test.HASH                  # "secret"
CAM_PASS = "Cam-Pa55word"
ROOT_PASS = "R00t-new:pw"
USER_PASS = "Carol pw 1"

failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)
    return cond


def api(port, method, path, user, password, body=None):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), method=method,
                                 data=None if body is None else json.dumps(body).encode())
    req.add_header("Authorization", "Basic " + base64.b64encode(("%s:%s" % (user, password)).encode()).decode())
    req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()
    except OSError as e:
        return 0, str(e)


def rtsp_ok(port, user, password):
    try:
        c = rtsp_server_test.Rtsp(port, user, password)
        code = c.request("DESCRIBE", "rtsp://127.0.0.1:%d/ch1/main" % port, "Accept: application/sdp\r\n")[0]
        c.close()
        return code
    except OSError as e:
        return str(e)


def netip_ok(port, user, password):
    try:
        c = netip_srv_test.Conn(port)
        _, r = c.login(user, password)
        c.close()
        return r.get("Ret")
    except (OSError, ConnectionError) as e:
        return str(e)


def onvif_ok(port, user, password):
    st, _, _ = onvif_test.Onvif(port).call(onvif_test.MEDIA, "GetProfiles", header=onvif_test.wsse(user, password))
    return st


def logins(ports, who, when):
    """Every account in `who` on RTSP, ONVIF and NETIP."""
    for user, password in who:
        r = rtsp_ok(ports["rtsp"], user, password)
        check(r == 200, "%s: %s over RTSP (%s)" % (when, user, r))
        r = onvif_ok(ports["onvif"], user, password)
        check(r == 200, "%s: %s over ONVIF (%s)" % (when, user, r))
        r = netip_ok(ports["netip"], user, password)
        check(r == 100, "%s: %s over NETIP (%s)" % (when, user, r))


def start(vigild, cfg, log_path, wsd_port):
    env = dict(os.environ, VIGIL_ONVIF_WSD_PORT=str(wsd_port))
    return subprocess.Popen([vigild, "-f", cfg, "-t", "120"], stdout=open(log_path, "a"), stderr=subprocess.STDOUT,
                            env=env)


def wait_up(ports, user, password, secs=20):
    """The API answers, and CH1 is live (the camera's password worked)."""
    end = time.time() + secs
    while time.time() < end:
        st, body = api(ports["api"], "GET", "/api/v1/status", user, password)
        if st == 200 and json.loads(body)["channels"] and json.loads(body)["channels"][0].get("live"):
            return True
        time.sleep(0.3)
    return False


def stop(p):
    p.send_signal(signal.SIGTERM)
    try:
        p.wait(15)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()


def main():
    vigild, clip = sys.argv[1], sys.argv[2]
    tmp = tempfile.mkdtemp(prefix="vigil-secrets-", dir=os.getcwd())
    procs = []
    try:
        run(vigild, clip, tmp, procs)
    finally:
        for p in procs:
            if p.poll() is None:
                stop(p)
        shutil.rmtree(tmp, ignore_errors=True)
    print("%d failure(s)" % len(failures))
    return 1 if failures else 0


def run(vigild, clip, tmp, procs):
    free = netip_srv_test.free_port
    ports = {"api": free(), "cam": free(), "rtsp": free(), "onvif": free(), "netip": free()}
    wsd_port = free(socket.SOCK_DGRAM)
    procs.append(subprocess.Popen([sys.executable, os.path.join(HERE, "netip_server.py"), clip, "--port",
                                   str(ports["cam"]), "--password", CAM_PASS],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT))
    shadow, users = os.path.join(tmp, "shadow"), os.path.join(tmp, "users.yaml")
    with open(shadow, "w") as f:
        f.write("root:%s:19000:0:99999:7:::\nnobody:*:19000:0:99999:7:::\n" % HASH)
    os.chmod(shadow, 0o600)
    cfg = os.path.join(tmp, "vigil.yaml")
    with open(cfg, "w") as f:
        f.write("cameras:\n  - url: \"netip://admin:%s@127.0.0.1:%d/\"\n    name: Gate\n"
                "record:\n  path: \"\"\n"
                "api:\n  listen: \"127.0.0.1:%d\"\n  shadow: %s\n  users: %s\n"
                "web:\n  enabled: false\n"
                "rtsp_server:\n  port: %d\n"
                "onvif_server:\n  enabled: true\n  port: %d\n"
                "netip_server:\n  port: %d\n"
                % (CAM_PASS, ports["cam"], ports["api"], json.dumps(shadow), json.dumps(users), ports["rtsp"],
                   ports["onvif"], ports["netip"]))
    log_path = os.path.join(tmp, "vigild.log")
    vd = start(vigild, cfg, log_path, wsd_port)
    procs.append(vd)
    if not check(wait_up(ports, "root", "secret"), "vigild is up, CH1 live with the camera's password"):
        print(open(log_path).read())
        return

    # ---- set: a new user, and the system account's password
    st, body = api(ports["api"], "POST", "/api/v1/users", "root", "secret",
                   {"name": "carol", "group": "user", "password": USER_PASS})
    check(st == 201 and "Carol" not in body and "enc:" not in body, "carol added (%s %s)" % (st, body))
    st, body = api(ports["api"], "PUT", "/api/v1/users/root", "carol", USER_PASS, {"password": "x" * 8})
    check(st == 403, "only the system account sets its own password: carol gets %s" % st)
    st, body = api(ports["api"], "PUT", "/api/v1/users/root", "root", "secret", {"group": "admin"})
    check(st == 400, "the system account has a password and nothing else to set (%s)" % st)
    st, body = api(ports["api"], "PUT", "/api/v1/users/root", "root", "secret", {"password": ROOT_PASS})
    check(st == 200 and json.loads(body).get("system") is True, "root's password set (%s %s)" % (st, body))
    st, _ = api(ports["api"], "GET", "/api/v1/me", "root", "secret")
    check(st == 401, "the old one no longer logs in (%s)" % st)
    st, _ = api(ports["api"], "GET", "/api/v1/me", "root", ROOT_PASS)
    check(st == 200, "the new one does (%s)" % st)
    shadow_text = open(shadow).read()
    check(HASH not in shadow_text and shadow_text.startswith("root:$") and
          "\nnobody:*:19000:0:99999:7:::\n" in shadow_text and (os.stat(shadow).st_mode & 0o777) == 0o600,
          "the shadow file: root's new hash, the rest as it was, its mode kept")

    who = [("root", ROOT_PASS), ("admin", ROOT_PASS), ("carol", USER_PASS)]
    logins(ports, who, "at once")
    check(rtsp_ok(ports["rtsp"], "root", "secret") == 401 and netip_ok(ports["netip"], "root", "secret") != 100 and
          onvif_ok(ports["onvif"], "root", "secret") != 200, "root's old password: refused everywhere")

    # ---- what is in the files, and what the API shows
    texts = {p: open(p).read() for p in (cfg, users)}
    for p, t in texts.items():
        for secret in (CAM_PASS, ROOT_PASS, USER_PASS, base64.b64encode(USER_PASS.encode()).decode()):
            check(secret not in t, "%s: no %r in the clear" % (os.path.basename(p), secret))
    check("netip://admin:enc:v1:" in texts[cfg] and texts[users].count('password_enc: "enc:v1:') == 2,
          "the camera's password, carol's and root's are sealed")
    key_path = os.path.join(tmp, "vigil.key")
    check((os.stat(key_path).st_mode & 0o777) == 0o600 and (os.stat(users).st_mode & 0o777) == 0o600 and
          (os.stat(cfg).st_mode & 0o777) == 0o600, "vigil.key, the users file and vigil.yaml: 0600")
    if vigil_secret.HAVE:
        import re
        key = vigil_secret.read_key(key_path)
        sealed = re.findall(r'password_enc: "(enc:v1:[^"]+)"', texts[users])
        check(vigil_secret.open_value(key, "users:carol.password", sealed[0]) == USER_PASS and
              vigil_secret.open_value(key, "users:system:root.password", sealed[1]) == ROOT_PASS,
              "they open with vigil.key, each for its own field")
        try:
            vigil_secret.open_value(key, "users:root.password", sealed[1])
            check(False, "a sealed password does not open for another field")
        except Exception:  # noqa: BLE001 (InvalidTag)
            check(True, "a sealed password does not open for another field")
    for path in ("/api/v1/users", "/api/v1/users/carol", "/api/v1/config", "/api/v1/config/cameras", "/api/v1/me"):
        st, body = api(ports["api"], "GET", path, "root", ROOT_PASS)
        check(st == 200 and not any(s in body for s in ("enc:v1", "password_enc", "$6$", CAM_PASS, ROOT_PASS,
                                                        USER_PASS)),
              "GET %s shows no password, hash or sealed value" % path)

    # ---- restarted: the same, the camera included
    stop(vd)
    vd = start(vigild, cfg, log_path, wsd_port)
    procs.append(vd)
    check(wait_up(ports, "root", ROOT_PASS), "restarted; CH1 live again: the camera's sealed password opened")
    logins(ports, who, "after a restart")
    log = open(log_path).read()
    check(not any(s in log for s in (CAM_PASS, ROOT_PASS, USER_PASS)), "no password in the log")


if __name__ == "__main__":
    sys.exit(main())
