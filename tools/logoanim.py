#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The game menu's logo animation, baked from AniCraft's PNG frames into runtime/menu/logo.anim.

    tools/logoanim.py LOGIN_DIR LOGOUT_DIR [OUT]

LOGIN_DIR and LOGOUT_DIR hold the frames tools/data/logo/build_anim.py exports (anim/out/
hiraku_login_png, hiraku_logout_png: 1920x1080 RGBA, frame_00001.png ...). Each frame is cropped
to the box (1504x436 at 208,322: nothing is drawn outside it) and scaled to a third, 501x145, the
size the menu shows it at in its 640x480 picture. The pictures are grey only, so two bytes a pixel
are kept: how light, how opaque. A frame the same as the one before is kept once.

The file (little-endian), read by runtime/menu/logo.cpp:
    "HKLOGO1\\0"
    u16 width, u16 height, u16 animations
    for each: u16 frames, then u16 a picture's number for each frame
    u16 pictures, u32 the packed size, u32 the unpacked size
    zlib: the pictures one after the other, width x height x (light, opaque), each byte the
          difference from the picture before's (the first's from nothing), mod 256
Needs Pillow.
"""
import os
import struct
import sys
import zlib

from PIL import Image

BOX = (208, 322, 208 + 1504, 322 + 436)
SIZE = (501, 145)


def frames(d):
    names = sorted(f for f in os.listdir(d) if f.endswith(".png"))
    if not names:
        sys.exit(f"logoanim: no PNG frames in {d}")
    for name in names:
        im = Image.open(os.path.join(d, name)).convert("RGBA")
        if im.size != (1920, 1080):
            sys.exit(f"logoanim: {name} is {im.size[0]}x{im.size[1]}, not 1920x1080")
        rgba = im.crop(BOX).resize(SIZE, Image.LANCZOS).tobytes()
        la = bytearray(len(rgba) // 2)
        la[0::2], la[1::2] = rgba[0::4], rgba[3::4]
        yield bytes(la)


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    out = sys.argv[3] if len(sys.argv) == 4 else os.path.join(os.path.dirname(__file__), "..", "runtime", "menu", "logo.anim")
    pictures, anims = [], []
    for d in sys.argv[1:3]:
        order = []
        for f in frames(d):
            if not pictures or pictures[-1] != f:
                pictures.append(f)
            order.append(len(pictures) - 1)
        anims.append(order)
    raw, before = bytearray(), bytes(len(pictures[0]))
    for p in pictures:
        raw += bytes((a - b) & 255 for a, b in zip(p, before))
        before = p
    packed = zlib.compress(bytes(raw), 9)
    head = b"HKLOGO1\0" + struct.pack("<HHH", SIZE[0], SIZE[1], len(anims))
    for order in anims:
        head += struct.pack(f"<H{len(order)}H", len(order), *order)
    head += struct.pack("<HII", len(pictures), len(packed), len(raw))
    with open(out, "wb") as f:
        f.write(head + packed)
    print(f"{out}: {len(anims[0])} + {len(anims[1])} frames, {len(pictures)} pictures, {len(head) + len(packed)} bytes")


if __name__ == "__main__":
    main()
