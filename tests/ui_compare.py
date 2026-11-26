#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run test_ui and compare its screenshots with the references in tests/ui.

  ui_compare.py TEST_UI REFDIR OUTDIR
  UPDATE=1 ui_compare.py ...      write OUTDIR's screenshots as the references

test_ui writes one PPM per screen into OUTDIR; each must match REFDIR/<name>.png
within a tolerance (a pixel differs when a channel is off by more than 24;
at most 0.3 % of the pixels may differ), so a change to the look shows up as
a failing test with a diff image (OUTDIR/<name>.diff.ppm) until the
references are regenerated. Python 3 standard library only (zlib for PNG).
"""
import os
import struct
import subprocess
import sys
import zlib

TOL, MAX_FRACTION = 24, 0.003


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts, pos = [], 0
    while len(parts) < 4:                       # P6 w h maxval, whitespace separated
        while data[pos:pos + 1].isspace():
            pos += 1
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[pos:end])
        pos = end
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[pos + 1:pos + 1 + w * h * 3]


def write_png(path, w, h, rgb):
    stride = w * 3
    # Filter "Sub" on every row: flat UI areas become runs of zeros.
    raw = bytearray()
    for y in range(h):
        row = rgb[y * stride:(y + 1) * stride]
        raw.append(1)
        raw += row[:3]
        raw += bytes((row[i] - row[i - 3]) & 0xFF for i in range(3, stride))

    def chunk(kind, body):
        c = struct.pack(">I", len(body)) + kind + body
        return c + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, w = 8, b"", None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, inter = struct.unpack(">IIBBBBB", body)
            if (depth, ctype, inter) != (8, 2, 0):
                raise ValueError(f"{path}: only 8-bit RGB, not interlaced")
        elif kind == b"IDAT":
            idat += body
    raw, stride, bpp = zlib.decompress(idat), w * 3, 3
    out, prev = bytearray(), bytearray(stride)
    for y in range(h):
        ft = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                line[i] = (line[i] + a) & 0xFF
            elif ft == 2:
                line[i] = (line[i] + b) & 0xFF
            elif ft == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else b if pb <= pc else c
                line[i] = (line[i] + pr) & 0xFF
        out += line
        prev = line
    return w, h, bytes(out)


def compare(name, ref, out):
    w, h, got = read_ppm(os.path.join(out, name + ".ppm"))
    rw, rh, want = read_png(os.path.join(ref, name + ".png"))
    if (w, h) != (rw, rh):
        return f"{name}: {w}x{h}, reference {rw}x{rh}"
    bad, diff = 0, bytearray(len(got))
    for i in range(0, len(got), 3):
        if (abs(got[i] - want[i]) > TOL or abs(got[i + 1] - want[i + 1]) > TOL
                or abs(got[i + 2] - want[i + 2]) > TOL):
            bad += 1
            diff[i] = 255
    if bad > MAX_FRACTION * w * h:
        with open(os.path.join(out, name + ".diff.ppm"), "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (w, h) + bytes(diff))
        return f"{name}: {bad} pixels differ ({100.0 * bad / (w * h):.2f} %)"
    print(f"{name}: matches ({bad} pixels off)")
    return None


def main():
    test_ui, ref, out = sys.argv[1:4]
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.endswith(".ppm"):
            os.unlink(os.path.join(out, f))
    rc = subprocess.call([test_ui, out])
    shots = sorted(f[:-4] for f in os.listdir(out) if f.endswith(".ppm") and ".diff" not in f)
    if os.environ.get("UPDATE"):
        os.makedirs(ref, exist_ok=True)
        for name in shots:
            w, h, rgb = read_ppm(os.path.join(out, name + ".ppm"))
            write_png(os.path.join(ref, name + ".png"), w, h, rgb)
            print("wrote", os.path.join(ref, name + ".png"))
        return rc
    errors = [] if rc == 0 else [f"test_ui failed ({rc})"]
    refs = sorted(f[:-4] for f in os.listdir(ref) if f.endswith(".png"))
    for name in refs:
        if name not in shots:
            errors.append(f"{name}: no screenshot")
            continue
        e = compare(name, ref, out)
        if e:
            errors.append(e)
    for name in shots:
        if name not in refs:
            errors.append(f"{name}: no reference (UPDATE=1 to add it)")
    for e in errors:
        print("FAIL", e)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
