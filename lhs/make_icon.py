"""Makes lhs/icon.ico, the dashboard exe's icon, from the icon the dashboard draws itself.

  python lhs/make_icon.py [path to QuestLHSync.exe]

Renders each size with "QuestLHSync.exe --icon", then packs them as PNG images (zlib level 9) into one .ico.
Run it after changing DrawIcon in src/dashboard/dashboard_main.cpp, then build.bat again.
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = (256, 64, 48, 40, 32, 24, 20, 16)


def recompress(png):
    """the same PNG with its image data deflated properly (the dashboard writes stored blocks)"""
    assert png[:8] == b"\x89PNG\r\n\x1a\n"
    chunks, idat, i = [], b"", 8
    while i < len(png):
        n, kind = struct.unpack(">I4s", png[i:i + 8])
        data = png[i + 8:i + 8 + n]
        i += 12 + n
        if kind == b"IDAT":
            idat += data
        else:
            chunks.append((kind, data))
    raw = zlib.decompress(idat)
    out = png[:8]
    for kind, data in chunks:
        if kind == b"IEND":
            data_idat = zlib.compress(raw, 9)
            out += struct.pack(">I", len(data_idat)) + b"IDAT" + data_idat
            out += struct.pack(">I", zlib.crc32(b"IDAT" + data_idat) & 0xFFFFFFFF)
        out += struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return out


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "driver", "questlhsync", "bin", "win64", "QuestLHSync.exe")
    images = []
    with tempfile.TemporaryDirectory() as tmp:
        for n in SIZES:
            path = os.path.join(tmp, f"{n}.png")
            subprocess.run([exe, "--icon", path, str(n)], check=True)
            images.append((n, recompress(open(path, "rb").read())))
    head = struct.pack("<HHH", 0, 1, len(images))
    offset = len(head) + 16 * len(images)
    entries, blobs = b"", b""
    for n, png in images:
        entries += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(png), offset + len(blobs))
        blobs += png
    out = os.path.join(ROOT, "lhs", "icon.ico")
    with open(out, "wb") as f:
        f.write(head + entries + blobs)
    print(f"wrote {out} ({os.path.getsize(out) / 1e3:.0f} KB, {len(images)} sizes)")


if __name__ == "__main__":
    main()
