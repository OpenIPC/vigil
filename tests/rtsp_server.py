#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""RTSP test server (GStreamer gst-rtsp-server) for vigil's tests.

    rtsp_server.py [--port 8554] [--codec h264|h265] [--auth none|basic|digest]
                   [--user root] [--password 123456] [--size 640x360] [--refs 2]

Serves a live test pattern at rtsp://[user:pass@]host:PORT/test.
Exits with 77 (ctest "skipped") when GStreamer's RTSP server bindings or
the encoder are missing.
"""
import argparse
import sys

try:
    import gi
    gi.require_version("Gst", "1.0")
    gi.require_version("GstRtsp", "1.0")
    gi.require_version("GstRtspServer", "1.0")
    from gi.repository import GLib, Gst, GstRtsp, GstRtspServer
except (ImportError, ValueError):
    sys.exit(77)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="8554")
    ap.add_argument("--codec", default="h264", choices=["h264", "h265"])
    ap.add_argument("--auth", default="none", choices=["none", "basic", "digest"])
    ap.add_argument("--user", default="root")
    ap.add_argument("--password", default="123456")
    ap.add_argument("--size", default="640x360")
    ap.add_argument("--refs", type=int, default=2)
    a = ap.parse_args()
    Gst.init(None)
    w, h = a.size.split("x")
    if a.codec == "h264":
        enc = "x264enc tune=zerolatency key-int-max=25 bframes=0 ref=%d ! rtph264pay name=pay0 pt=96 config-interval=-1" % a.refs
        need = "x264enc"
    else:
        enc = "x265enc tune=zerolatency key-int-max=25 ! rtph265pay name=pay0 pt=96 config-interval=-1"
        need = "x265enc"
    if not Gst.ElementFactory.find(need):
        sys.exit(77)
    launch = "( videotestsrc is-live=true ! video/x-raw,width=%s,height=%s,framerate=25/1 ! %s )" % (w, h, enc)

    server = GstRtspServer.RTSPServer()
    server.set_service(a.port)
    f = GstRtspServer.RTSPMediaFactory()
    f.set_launch(launch)
    f.set_shared(True)
    if a.auth != "none":
        perms = GstRtspServer.RTSPPermissions()
        perms.add_role_from_structure(Gst.Structure.new_from_string(
            "user, media.factory.access=(boolean)true, media.factory.construct=(boolean)true"))
        f.set_permissions(perms)
        auth = GstRtspServer.RTSPAuth()
        token = GstRtspServer.RTSPToken()
        token.set_string(GstRtspServer.RTSP_TOKEN_MEDIA_FACTORY_ROLE, "user")
        if a.auth == "digest":
            auth.set_supported_methods(GstRtsp.RTSPAuthMethod.DIGEST)
            auth.add_digest(a.user, a.password, token)
        else:
            auth.add_basic(GstRtspServer.RTSPAuth.make_basic(a.user, a.password), token)
        server.set_auth(auth)
    server.get_mount_points().add_factory("/test", f)
    server.attach(None)
    print("rtsp_server: %s %s auth=%s on :%s" % (a.codec, a.size, a.auth, a.port), flush=True)
    GLib.MainLoop().run()


if __name__ == "__main__":
    main()
