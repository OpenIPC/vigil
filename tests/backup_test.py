#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end test of the configuration export and import, and of the
reset of vigil's settings (docs/management.md, "Configuration").

    backup_test.py VIGILD CLIP

Two vigild, A and B, each with a key and a shadow file of its own. A
exports its configuration through the API, with the passwords sealed by a
password and without them; the export is opened here independently
(PBKDF2-HMAC-SHA256 and AES-256-GCM as docs/security.md describes them)
and holds no password in the clear. B refuses a wrong password, a changed
file and a settings change that cannot apply (A still holds the ports),
leaving its old configuration and accounts; once A is gone B imports it,
re-seals every password with its own key, and its camera, the user and
root log in with the passwords they had on A. Then B's settings are reset.
"""
import base64
import hashlib
import io
import json
import os
import shutil
import socket
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request

import netip_srv_test
import secrets_test
import vigil_secret

HASH = netip_srv_test.HASH                  # "secret"
CAM_PASS = "Cam-Exp0rt-pw"
ROOT_PASS = "R00t-exp0rt"
USER_PASS = "Dora pw 22"
EXPORT_PASS = "the export's pw"

check = secrets_test.check
failures = secrets_test.failures


def req(port, method, path, password, body=None, raw=None, headers=None, user="root"):
    data = raw if raw is not None else None if body is None else json.dumps(body).encode()
    r = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), method=method, data=data)
    r.add_header("Authorization", "Basic " + base64.b64encode(("%s:%s" % (user, password)).encode()).decode())
    r.add_header("Content-Type", "application/x-tar" if raw is not None else "application/json")
    for k, v in (headers or {}).items():
        r.add_header(k, v)
    try:
        with urllib.request.urlopen(r, timeout=60) as resp:
            return resp.status, resp.read(), dict(resp.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read(), dict(e.headers)
    except OSError as e:
        return 0, str(e).encode(), {}


def recorder(tmp, name, ports, cams):
    d = os.path.join(tmp, name)
    os.mkdir(d)
    shadow, users = os.path.join(d, "shadow"), os.path.join(d, "users.yaml")
    with open(shadow, "w") as f:
        f.write("root:%s:19000:0:99999:7:::\n" % HASH)
    os.chmod(shadow, 0o600)
    cfg, rom = os.path.join(d, "vigil.yaml"), os.path.join(d, "rom.yaml")
    for path, c in ((cfg, cams), (rom, " []")):
        with open(path, "w") as f:
            f.write(settings(c, ports, shadow, users))
    return {"dir": d, "cfg": cfg, "rom": rom, "users": users, "shadow": shadow, "key": os.path.join(d, "vigil.key"),
            "log": os.path.join(d, "vigild.log"), "ports": ports}


def settings(cams, ports, shadow, users):
    return ("cameras:%s\nrecord:\n  path: \"\"\n"
                "api:\n  listen: \"127.0.0.1:%d\"\n  shadow: %s\n  users: %s\n"
                "web:\n  enabled: false\n"
                "rtsp_server:\n  port: %d\n"
                "onvif_server:\n  enabled: false\n"
                "netip_server:\n  port: %d\n"
            % (cams, ports["api"], json.dumps(shadow), json.dumps(users), ports["rtsp"], ports["netip"]))


def live(port, password, secs=20):
    end = time.time() + secs
    while time.time() < end:
        st, body, _ = req(port, "GET", "/api/v1/status", password)
        if st == 200:
            ch = json.loads(body)["channels"]
            if ch and ch[0].get("live"):
                return True
        time.sleep(0.3)
    return False


def up(port, password, secs=20):
    end = time.time() + secs
    while time.time() < end:
        if req(port, "GET", "/api/v1/me", password)[0] == 200:
            return True
        time.sleep(0.3)
    return False


def members(blob):
    with tarfile.open(fileobj=io.BytesIO(blob)) as t:
        return {m.name: t.extractfile(m).read().decode() for m in t.getmembers()}


def repack(files):
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode="w", format=tarfile.USTAR_FORMAT) as t:
        for name, text in files.items():
            data = text.encode()
            info = tarfile.TarInfo(name)
            info.size = len(data)
            t.addfile(info, io.BytesIO(data))
    return out.getvalue()


def open_export(man, password, aad, value):
    """An export's sealed value opened as docs/security.md says: the key
    PBKDF2-HMAC-SHA256(password, salt, iterations), AES-256-GCM."""
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    k = man["kdf"]
    key = hashlib.pbkdf2_hmac("sha256", password.encode(), base64.b64decode(k["salt"]), int(k["iterations"]), 32)
    assert value.startswith("enc:p1:")
    raw = base64.b64decode(value[7:])
    return AESGCM(key).decrypt(raw[:12], raw[12:], aad.encode()).decode()


def main():
    vigild, clip = sys.argv[1], sys.argv[2]
    tmp = tempfile.mkdtemp(prefix="vigil-backup-", dir=os.getcwd())
    procs = []
    try:
        run(vigild, clip, tmp, procs)
    finally:
        for p in procs:
            if p.poll() is None:
                secrets_test.stop(p)
        shutil.rmtree(tmp, ignore_errors=True)
    print("%d failure(s)" % len(failures))
    return 1 if failures else 0


def start(vigild, r, procs):
    # A reset goes back to the image's vigil.yaml (/rom on the firmware):
    # here the recorder's own, without cameras.
    env = dict(os.environ, VIGIL_DEFAULTS=r["rom"])
    p = subprocess.Popen([vigild, "-f", r["cfg"], "-t", "400"], stdout=open(r["log"], "a"),
                         stderr=subprocess.STDOUT, env=env)
    procs.append(p)
    return p


def run(vigild, clip, tmp, procs):
    free = netip_srv_test.free_port
    cam = free()
    procs.append(subprocess.Popen([sys.executable, os.path.join(os.path.dirname(__file__), "netip_server.py"), clip,
                                   "--port", str(cam), "--password", CAM_PASS],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT))
    pa = {"api": free(), "rtsp": free(), "netip": free()}
    pb = {"api": free(), "rtsp": free(), "netip": free()}
    a = recorder(tmp, "a", pa, "\n  - url: \"netip://admin:%s@127.0.0.1:%d/\"\n    name: Gate" % (CAM_PASS, cam))
    b = recorder(tmp, "b", pb, " []")
    va = start(vigild, a, procs)
    vb = start(vigild, b, procs)
    if not check(live(pa["api"], "secret") and up(pb["api"], "secret"), "A (camera live) and B are up"):
        print(open(a["log"]).read(), open(b["log"]).read())
        return
    st, body, _ = req(pa["api"], "PUT", "/api/v1/users/root", "secret", {"password": ROOT_PASS})
    check(st == 200, "A: root's password set (%s)" % st)
    st, body, _ = req(pa["api"], "POST", "/api/v1/users", ROOT_PASS,
                      {"name": "dora", "group": "user", "password": USER_PASS})
    check(st == 201, "A: dora added (%s)" % st)

    # ---- the export, with the passwords sealed by a password
    st, body, _ = req(pa["api"], "POST", "/api/v1/system/config/export", ROOT_PASS, {"secrets": "password"})
    check(st == 400, "an export with the passwords needs a password (%s)" % st)
    st, body, _ = req(pa["api"], "POST", "/api/v1/system/config/export", "dora", {"secrets": "none"}, user="dora")
    check(st in (401, 403), "dora (live view, playback) may not export (%s)" % st)
    t0 = time.time()
    st, blob, hdr = req(pa["api"], "POST", "/api/v1/system/config/export", ROOT_PASS,
                        {"secrets": "password", "password": EXPORT_PASS})
    took = time.time() - t0
    check(st == 200 and hdr.get("Content-Type") == "application/x-tar" and
          "vigil-config-" in hdr.get("Content-Disposition", ""),
          "exported (%s, %.1f s, %d bytes)" % (st, took, len(blob)))
    for s in (CAM_PASS, ROOT_PASS, USER_PASS, EXPORT_PASS, HASH):
        check(s.encode() not in blob, "no %r in the export" % s)
    key_a = open(a["key"]).read().strip()
    check(key_a.encode() not in blob and b"$6$" not in blob, "nor A's key, nor a password hash")
    files = members(blob)
    check(sorted(files) == ["manifest.json", "system.json", "vigil.users.yaml", "vigil.yaml"] or
          sorted(files) == ["manifest.json", "vigil.users.yaml", "vigil.yaml"], "its files: %s" % sorted(files))
    man = json.loads(files["manifest.json"])
    check(man["format"] == "vigil-config" and man["secrets"] == "password" and man["kdf"]["iterations"] >= 100000 and
          man["included"]["cameras"] == 1 and man["included"]["user_passwords"] == 1 and
          man["included"]["root_password"] is True and "vigil.key (the recorder's own key)" in man["not_included"],
          "the manifest: %s" % {k: man[k] for k in ("secrets", "kdf", "included")})
    if vigil_secret.HAVE:
        import re
        url = re.search(r'url: "(netip://admin:enc:p1:[^@]+)@', files["vigil.yaml"]).group(1)
        check(open_export(man, EXPORT_PASS, "config:cameras[0].url", url.split("admin:")[1]) == CAM_PASS,
              "opened here with the export's password: the camera's password")
        sealed = re.findall(r'password_enc: "(enc:p1:[^"]+)"', files["vigil.users.yaml"])
        check(open_export(man, EXPORT_PASS, "users:dora.password", sealed[0]) == USER_PASS and
              open_export(man, EXPORT_PASS, "users:system:root.password", sealed[1]) == ROOT_PASS,
              "dora's and root's")
        check(open_export(man, EXPORT_PASS, "export:check", man["check"]) == "vigil-config", "the check value")

    st, plain, _ = req(pa["api"], "POST", "/api/v1/system/config/export", ROOT_PASS, {"secrets": "none"})
    pf = members(plain) if st == 200 else {}
    check(st == 200 and all(s.encode() not in plain for s in (CAM_PASS, ROOT_PASS, USER_PASS)) and
          b"enc:p1:" not in plain.replace(b"(enc:p1:...)", b"") and
          json.loads(pf["manifest.json"])["secrets"] == "none",
          "the export without passwords has none")

    # ---- B refuses what it should, and changes nothing
    def imp(body):
        st, out, _ = req(pb["api"], "POST", "/api/v1/system/config/import", "secret", body)
        try:
            return st, json.loads(out)
        except ValueError:
            return st, {"raw": out}

    b64 = base64.b64encode(blob).decode()
    st, out = imp({"file": b64, "password": "wrong", "dry_run": True})
    check(st == 403 and out.get("code") == "password", "B: a wrong password (%s %s)" % (st, out.get("error")))
    st, out = imp({"file": b64, "dry_run": True})
    check(st == 403 and out.get("code") == "password", "B: no password (%s)" % st)
    changed = dict(files)
    changed["vigil.yaml"] = files["vigil.yaml"].replace("name: Gate", "name: Evil")
    st, out = imp({"file": base64.b64encode(repack(changed)).decode(), "password": EXPORT_PASS, "dry_run": True})
    check(st == 422 and out.get("code") == "changed", "B: a changed file (%s %s)" % (st, out.get("error")))
    newer = dict(files)
    m2 = dict(man, format_version=2)
    newer["manifest.json"] = json.dumps(m2)
    st, out = imp({"file": base64.b64encode(repack(newer)).decode(), "password": EXPORT_PASS, "dry_run": True})
    check(st == 409 and out.get("code") == "version", "B: a newer format (%s %s)" % (st, out.get("error")))
    st, out = imp({"file": b64, "password": EXPORT_PASS, "dry_run": True})
    check(st == 200 and out["cameras"] == {"now": 0, "import": 1} and out["users"]["import"] == 1 and
          out["root_password"] is True and not out["applied"],
          "B: the dry run's summary (%s %s)" % (st, [l["text"] for l in out.get("lines", [])]))
    rst, rout, _ = req(pb["api"], "POST", "/api/v1/system/config/import?dry_run=1", "secret", raw=blob,
                       headers={"X-Config-Password": EXPORT_PASS})
    check(rst == 200 and json.loads(rout)["applied"] is False, "B: the file itself as application/x-tar (%s)" % rst)

    # A still holds the ports the export moves B to: refused, the old
    # configuration and accounts kept.
    users_before = open(b["users"]).read() if os.path.exists(b["users"]) else None
    st, out = imp({"file": b64, "password": EXPORT_PASS})
    check(st == 400 and "not saved" in out.get("error", ""), "B: refused while A holds the ports (%s %s)" %
          (st, out.get("error")))
    after = open(b["users"]).read() if os.path.exists(b["users"]) else None
    check(after == users_before and req(pb["api"], "GET", "/api/v1/me", USER_PASS, user="dora")[0] == 401 and
          req(pb["api"], "GET", "/api/v1/me", "secret")[0] == 200, "B: its accounts as they were")

    # ---- A gone: B takes its configuration, and its ports
    secrets_test.stop(va)
    st, out = imp({"file": b64, "password": EXPORT_PASS})
    check(st == 200 and out["applied"], "B: imported (%s %s)" % (st, out.get("error")))
    time.sleep(1)
    check(live(pa["api"], ROOT_PASS, 30), "B, now on A's API port: the camera live with its password, re-sealed")
    check(req(pa["api"], "GET", "/api/v1/me", USER_PASS, user="dora")[0] == 200, "B: dora logs in")
    check(secrets_test.netip_ok(pa["netip"], "dora", USER_PASS) == 100,
          "B: dora over NETIP (her sealed password opened with B's key)")
    check(secrets_test.rtsp_ok(pa["rtsp"], "root", ROOT_PASS) == 200, "B: root over RTSP with A's password")
    sh = open(b["shadow"]).read()
    check(sh.startswith("root:$") and HASH not in sh, "B's shadow file: root's password from the export")
    cfg_b = open(b["cfg"]).read()
    key_b = open(b["key"]).read().strip()
    check(key_b != key_a and CAM_PASS not in cfg_b and "enc:p1:" not in cfg_b.replace("(enc:p1:...)", ""),
          "B's vigil.yaml: no password in the clear, nothing sealed for the export")
    if vigil_secret.HAVE:
        import re
        sealed = re.search(r"enc:v1:[A-Za-z0-9+/=]+", cfg_b).group(0)
        check(vigil_secret.open_value(bytes.fromhex(key_b), "config:cameras[0].url", sealed) == CAM_PASS,
              "and the camera's password opens with B's own key")

    # ---- reset B's settings: the defaults, no users, a new key
    st, body, _ = req(pa["api"], "POST", "/api/v1/system/reset", ROOT_PASS, {"level": "factory"})
    check(st == 409 and json.loads(body)["code"] == "unmanaged", "no factory reset of a desktop (%s)" % st)
    st, body, _ = req(pa["api"], "POST", "/api/v1/system/reset", ROOT_PASS, {"level": "vigil"})
    check(st == 200 and json.loads(body)["rebooting"] is False, "B: vigil's settings reset (%s %s)" % (st, body))
    time.sleep(1.5)
    check(up(pb["api"], ROOT_PASS), "B: back on its own port (the image's settings); root as it was")
    st, body, _ = req(pb["api"], "GET", "/api/v1/config/cameras", ROOT_PASS)
    check(st == 200 and json.loads(body) == [], "B: no cameras (%s)" % body[:80])
    check(req(pb["api"], "GET", "/api/v1/me", USER_PASS, user="dora")[0] == 401, "B: dora gone")
    cfg_b = open(b["cfg"]).read()
    check("name: Gate" not in cfg_b and open(b["key"]).read().strip() != key_b, "B: no cameras in the file, a new key")
    users_b = open(b["users"]).read()
    check("dora" not in users_b, "B: no users")
    log = open(b["log"]).read() + open(a["log"]).read()
    check(not any(s in log for s in (CAM_PASS, ROOT_PASS, USER_PASS, EXPORT_PASS)), "no password in the logs")
    secrets_test.stop(vb)


if __name__ == "__main__":
    sys.exit(main())
