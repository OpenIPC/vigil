#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Render the UI's icons from SVG into LVGL images.

  scripts/gen-icons.py           write src/ui/icons.c and src/ui/icons.h
  scripts/gen-icons.py --check   exit 1 if they are out of date (ctest)

The sources are Tabler Icons (MIT, src/ui/icons/LICENSE), outline icons on
a 24x24 grid drawn with a stroke. Each is rendered at the size it is shown
at into an LVGL A8 image (alpha only): one byte a pixel, coloured when it
is drawn (image_recolor, from the design tokens), which costs a quarter of
ARGB8888 and lets one image serve any colour.

Rendering takes librsvg and cairo (the shared libraries, through ctypes;
no Python packages). The check needs neither: icons.c records a hash of
each source and of the rendering parameters, and --check compares those,
so it runs anywhere and does not depend on a librsvg version's
anti-aliasing.
"""
import ctypes
import ctypes.util
import hashlib
import os
import re
import sys

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(TOP, "src", "ui", "icons")
OUT_C = os.path.join(TOP, "src", "ui", "icons.c")
OUT_H = os.path.join(TOP, "src", "ui", "icons.h")
VERSION = 1                     # bump when the rendering changes

# C name, source, size in pixels, stroke width (in the 24-unit grid)
ICONS = [
    ("playback", "player-play.svg", 96, 1.5),
    ("ai", "brain.svg", 96, 1.5),
    ("alarm", "urgent.svg", 96, 1.5),
    ("operation", "gauge.svg", 96, 1.5),
    ("backup", "database-export.svg", 96, 1.5),
    ("display", "device-desktop.svg", 96, 1.5),
    ("camera", "device-cctv.svg", 28, 2),
    ("network", "network.svg", 28, 2),
    ("storage", "database.svg", 28, 2),
    ("system", "settings.svg", 28, 2),
    ("account", "user-circle.svg", 28, 2),
    # On the video tiles: what the channel is doing, and an empty slot.
    ("rec", "player-record-filled.svg", 20, 2),
    ("ev_motion", "activity.svg", 20, 2.25),
    ("ev_human", "walk.svg", 20, 2.25),
    ("ev_face", "face-id.svg", 20, 2.25),
    ("ev_vehicle", "car.svg", 20, 2.25),
    ("add", "plus.svg", 20, 2),
    # The Channels page: its two panes.
    ("search", "search.svg", 24, 2),
    ("channels", "device-cctv.svg", 24, 2),
    # the settings screens' left panes
    ("nav_ethernet", "network.svg", 22, 2),
    ("nav_wifi", "wifi.svg", 22, 2),
    ("nav_lte", "antenna-bars-5.svg", 22, 2),
    ("nav_ddns", "world-www.svg", 22, 2),
    ("nav_upnp", "router.svg", 22, 2),
    ("nav_smtp", "mail.svg", 22, 2),
    ("nav_alarm", "bell-ringing.svg", 22, 2),
    ("nav_p2p", "cloud-off.svg", 22, 2),
    ("nav_basic", "device-floppy.svg", 22, 2),
    ("nav_schedule", "calendar-time.svg", 22, 2),
    ("nav_disks", "server-2.svg", 22, 2),
    ("nav_maint", "stethoscope.svg", 22, 2),
    ("nav_quota", "chart-pie.svg", 22, 2),
    ("nav_nas", "folder-share.svg", 22, 2),
    ("nav_general", "settings.svg", 22, 2),
    ("nav_users", "users.svg", 22, 2),
    ("nav_groups", "users-group.svg", 22, 2),
    ("nav_output", "device-desktop.svg", 22, 2),
    ("nav_clock", "clock.svg", 22, 2),
    ("nav_names", "device-cctv.svg", 22, 2),
    ("nav_icons", "activity.svg", 22, 2),
]


def source(icon):
    name, file, size, stroke = icon
    with open(os.path.join(SRC, file), "rb") as f:
        svg = f.read()
    return re.sub(rb'stroke-width="[0-9.]+"', b'stroke-width="%g"' % stroke, svg, count=1)


def stamp(icon):
    h = hashlib.sha256(source(icon))
    h.update(repr((VERSION, icon)).encode())
    return h.hexdigest()[:16]


class Viewport(ctypes.Structure):
    _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double),
                ("width", ctypes.c_double), ("height", ctypes.c_double)]


def render(icon):
    """Alpha bytes, size x size, row by row."""
    def lib(name, soname):
        path = ctypes.util.find_library(name) or soname
        return ctypes.CDLL(path)
    rsvg = lib("rsvg-2", "librsvg-2.so.2")
    cairo = lib("cairo", "libcairo.so.2")
    rsvg.rsvg_handle_new_from_data.restype = ctypes.c_void_p
    rsvg.rsvg_handle_new_from_data.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p]
    rsvg.rsvg_handle_render_document.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                                 ctypes.POINTER(Viewport), ctypes.c_void_p]
    cairo.cairo_image_surface_create.restype = ctypes.c_void_p
    cairo.cairo_create.restype = ctypes.c_void_p
    cairo.cairo_create.argtypes = [ctypes.c_void_p]
    cairo.cairo_image_surface_get_data.restype = ctypes.POINTER(ctypes.c_uint8)
    cairo.cairo_image_surface_get_data.argtypes = [ctypes.c_void_p]
    cairo.cairo_image_surface_get_stride.argtypes = [ctypes.c_void_p]
    cairo.cairo_surface_flush.argtypes = [ctypes.c_void_p]
    cairo.cairo_destroy.argtypes = [ctypes.c_void_p]
    cairo.cairo_surface_destroy.argtypes = [ctypes.c_void_p]

    size = icon[2]
    svg = source(icon)
    handle = rsvg.rsvg_handle_new_from_data(svg, len(svg), None)
    if not handle:
        sys.exit(f"{icon[1]}: librsvg cannot read it")
    surface = cairo.cairo_image_surface_create(0, size, size)     # CAIRO_FORMAT_ARGB32
    cr = cairo.cairo_create(surface)
    vp = Viewport(0, 0, size, size)
    if not rsvg.rsvg_handle_render_document(ctypes.c_void_p(handle), cr, ctypes.byref(vp), None):
        sys.exit(f"{icon[1]}: librsvg cannot render it")
    cairo.cairo_surface_flush(surface)
    stride = cairo.cairo_image_surface_get_stride(surface)
    data = cairo.cairo_image_surface_get_data(surface)
    # ARGB32 in native (little-endian) order: alpha is the fourth byte.
    alpha = bytes(data[y * stride + x * 4 + 3] for y in range(size) for x in range(size))
    cairo.cairo_destroy(cr)
    cairo.cairo_surface_destroy(surface)
    return alpha


def gen_h():
    out = ["/* SPDX-License-Identifier: GPL-3.0-or-later */",
           "/* icons.h — GENERATED by scripts/gen-icons.py from the SVGs in src/ui/icons",
           " * (Tabler Icons, MIT: src/ui/icons/LICENSE); do not edit. A8 images:",
           " * colour them with the image_recolor style. */",
           "#ifndef ICONS_H", "#define ICONS_H", "", '#include "lvgl.h"', ""]
    for name, file, size, _ in ICONS:
        out.append(f"extern const lv_image_dsc_t ui_icon_{name};{'':<4}/* {size} px, {file} */")
    out += ["", "#endif /* ICONS_H */"]
    return "\n".join(out) + "\n"


def gen_c():
    out = ["/* SPDX-License-Identifier: GPL-3.0-or-later */",
           "/* icons.c — GENERATED by scripts/gen-icons.py; do not edit. */",
           '#include "icons.h"', ""]
    for icon in ICONS:
        name, file, size, _ = icon
        a = render(icon)
        out.append(f"/* stamp {name} {stamp(icon)} */")
        out.append(f"static const uint8_t k_{name}[{size * size}] = {{")
        for i in range(0, len(a), 24):
            out.append("    " + ",".join(f"0x{b:02x}" for b in a[i:i + 24]) + ",")
        out.append("};")
        out.append(f"const lv_image_dsc_t ui_icon_{name} = {{")
        out.append("    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8,")
        out.append(f"                .w = {size}, .h = {size}, .stride = {size} }},")
        out.append(f"    .data_size = sizeof k_{name},")
        out.append(f"    .data = k_{name},")
        out.append("};")
        out.append("")
    return "\n".join(out)


def stamps_in(path):
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as f:
        return re.findall(r"/\* stamp (\w+) ([0-9a-f]+) \*/", f.read())


def main():
    if "--check" in sys.argv[1:]:
        want = [(icon[0], stamp(icon)) for icon in ICONS]
        stale = []
        if stamps_in(OUT_C) != want:
            stale.append(os.path.relpath(OUT_C, TOP))
        with open(OUT_H, encoding="utf-8") as f:
            if f.read() != gen_h():
                stale.append(os.path.relpath(OUT_H, TOP))
        if stale:
            print("out of date (run scripts/gen-icons.py):", ", ".join(stale))
            return 1
        return 0
    for path, text in ((OUT_H, gen_h()), (OUT_C, gen_c())):
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        print("wrote", os.path.relpath(path, TOP))
    return 0


if __name__ == "__main__":
    sys.exit(main())
