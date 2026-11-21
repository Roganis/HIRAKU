#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# The PNG files test_screenshot wrote (runtime/host/png.h, encode), read with Python's own zlib and
# struct: the signature, every chunk's CRC, IHDR (8-bit RGB, not interlaced), the data's size after
# inflating and each row's filter; and with PIL too where it is installed.
#   check_png.py DIR    DIR: test_screenshot's folder (DIR/check/*.png)
import glob
import os
import struct
import sys
import zlib


def check(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "signature"
    at, chunks, idat = 8, [], b""
    while at < len(data):
        (n,) = struct.unpack(">I", data[at:at + 4])
        kind, body = data[at + 4:at + 8], data[at + 8:at + 8 + n]
        (crc,) = struct.unpack(">I", data[at + 8 + n:at + 12 + n])
        assert crc == zlib.crc32(kind + body) & 0xFFFFFFFF, f"{kind} CRC"
        chunks.append(kind)
        if kind == b"IHDR":
            w, h, depth, ctype, comp, filt, inter = struct.unpack(">IIBBBBB", body)
            assert (depth, ctype, comp, filt, inter) == (8, 2, 0, 0, 0), "IHDR kind"
        if kind == b"IDAT":
            idat += body
        at += 12 + n
    assert at == len(data), "chunks run to the end"
    assert chunks[0] == b"IHDR" and chunks[-1] == b"IEND", chunks
    raw = zlib.decompress(idat)
    assert len(raw) == (w * 3 + 1) * h, "inflated size"
    assert all(raw[y * (w * 3 + 1)] <= 4 for y in range(h)), "row filters"
    name = os.path.basename(path)[:-4]
    assert name == f"{w}x{h}", "size as named"
    try:
        from PIL import Image
    except ImportError:
        return "zlib"
    with Image.open(path) as im:
        im.load()
        assert im.size == (w, h) and im.mode == "RGB"
    return "zlib, PIL"


files = sorted(glob.glob(os.path.join(sys.argv[1], "check", "*.png")))
if not files:
    sys.exit("no PNG files in " + sys.argv[1])
for f in files:
    how = check(f)
print(f"{len(files)} PNG files hold ({how})")
