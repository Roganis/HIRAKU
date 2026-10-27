#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A character's pictures as the 64-bit PC pop'n reads them (docs/SONGS-BACK.md): the flip-book
animations the old characters' packs hold (afp/NAME, AP2 big-endian, its byte-swap list afp/bsi/NAME),
their shapes (geo/NAME_shapeN, GE2D: a picture's quad), the list of both (afp/afplist.xml), the
pictures (tex/, tex/texturelist.xml) and the pack itself (tools/ifs.py's).

The old characters' animations (pop'n 1 to 8's, the game's own packs) are made by a tool, all alike:
a sprite whose timeline shows one whole picture after another (each placed at a new depth, the one
before removed, a small script on it giving the frame it was placed at), the same timeline again at
the top, two sprites of the game's library before. `flipbook` writes one, given which picture shows
from which frame; written for an old character's own timeline it gives that animation byte for byte.

The byte-swap list (bsi) says how to read the file little-endian: 16-bit runs, each [b15: the
count's low bit][14-8: bytes to skip first, in twos][6-5: 2 u32, 1 u16][4-0: the count's other
bits], the count less one; 0 ends it.
"""
import hashlib
import struct

FPS = 30
STAGE = (320, 640)   # the old characters' stage, a picture's quad
LIB = ("aep_mask_dummy", "aeplibset", "aeplib", "__Packages.aeplib", "aep_dummy")


class Out:
    """bytes written with what each field is (u32, u16, or bytes left as they are)"""

    def __init__(self):
        self.b = bytearray()
        self.fields = []   # (offset, 4 or 2)

    def u32(self, v, at=None):
        if at is not None:
            struct.pack_into(">I", self.b, at, v & 0xFFFFFFFF)
            return
        self.fields.append((len(self.b), 4))
        self.b += struct.pack(">I", v & 0xFFFFFFFF)

    def u16(self, v):
        self.fields.append((len(self.b), 2))
        self.b += struct.pack(">H", v & 0xFFFF)

    def raw(self, b):
        self.b += b

    def align(self):
        self.b += b"\0" * (-len(self.b) % 4)


def bsi(fields):
    runs = []   # [skip in bytes, size, count]
    end = 0
    for at, size in sorted(fields):
        if runs and at == end and runs[-1][1] == size and runs[-1][2] < 64:
            runs[-1][2] += 1
        else:
            runs.append([at - end, size, 1])
        end = at + size
    out = bytearray()
    for skip, size, count in runs:
        assert skip % 2 == 0 and skip // 2 < 128, skip
        c = count - 1
        out += struct.pack(">H", (c & 1) << 15 | (skip // 2) << 8 | (0x40 if size == 4 else 0x20) | c >> 1)
    return bytes(out + b"\0\0")


def strings(name):
    """the string table (each name padded to 4 with at least one 0) and where each is"""
    table, at = bytearray(b"\0\0\0\0"), {}
    for s in (name,) + LIB:
        at[s] = len(table)
        table += s.encode() + b"\0"
        table += b"\0" * (-len(table) % 4)
    return bytes(table), at


def _section(o, frames, tags):
    """a tag section: frames as (first tag, count), tags as (id, write(o))"""
    base = len(o.b)
    o.u16(0)
    o.u16(0)
    o.u32(len(frames))
    o.u32(len(tags))
    o.u32(0x18)
    o.u32(0x18)
    o.u32(0x18 + 4 * len(frames))
    for first, count in frames:
        o.u32(count << 20 | first)
    for tid, write in tags:
        head = len(o.b)
        o.u32(0)
        write(o)
        size = len(o.b) - head - 4
        o.align()
        o.u32(tid << 22 | size, at=head)
    return base


def _library_sprite(sid, depth, src):
    def write(o):
        o.u16(1)
        o.u16(sid)
        o.u32(8)
        _section(o, [(0, 1)], [(0x7F, _place_plain(depth, 1, src))])
    return write


def _place_plain(depth, obj, src):
    def write(o):
        o.u32(6)
        o.u16(depth)
        o.u16(obj)
        o.u16(src)
        o.raw(b"\0\0")
    return write


def _place(depth, obj, src, frame, at, script):
    """a picture placed: at its place (the stage's middle, in twentieths), from the second on with
    the script that tells it the frame it was placed at"""
    def write(o):
        o.u32(0x01000486 if script else 0x01000406)
        o.u16(depth)
        o.u16(obj)
        o.u16(src)
        o.raw(b"\0\0")
        o.u32(at[0])
        o.u32(at[1])
        if script:
            push = bytes([0x37, frame]) if frame < 0x80 else b"\x07" + struct.pack(">I", frame)
            code = bytes.fromhex("ff004304") + push + bytes.fromhex("0c37021390 0e43012736320d".replace(" ", ""))
            code += b"\0" * (-len(code) % 4)
            o.u32(1)
            o.u32(20 + len(code))
            o.u16(0)
            o.u16(1)
            o.u32(1)
            o.raw(b"\0\0")
            o.u16(8)
            o.raw(code)
        o.u32(at[0])
        o.u32(at[1])
    return write


def _remove(depth):
    def write(o):
        o.u16(0)
        o.u16(depth)
    return write


def flipbook(name, frames, shows, shapes, sprite, at=(160 * 20, 320 * 20)):
    """an animation `frames` long (at 30 a second) showing the shapes of `shows` [(frame, shape,
    object[, where])] (object: the frame the shape ends at, as the game's own number them), the first at frame 0 (where: its place, in twentieths, the stage's middle `at`
    if not given); `shapes` the picture shapes' ids in their tags' order, `sprite`
    the animation's sprite id. Returns (afp, bsi)."""
    table, sat = strings(name)
    o = Out()
    o.raw(b"\xc1\xd0\xb2\x08")
    o.fields.append((0, 4))
    o.u32(0)   # length
    o.u16(0x200)
    o.u16(sat[name])
    o.u32(0xC7)
    for v in (0, STAGE[0], 0, STAGE[1]):
        o.u16(v)
    o.u32(FPS * 1024)
    o.u32(0xFF000000)
    o.u16(3)   # exported
    o.u16(1)   # imported
    for v in (0x60, 0x3C, 0x48, 0, 0, 0x50):   # tags, exports, imports, strings, their size, initializer
        o.u32(v)
    for tid, s in ((6, LIB[0]), (3, LIB[1]), (sprite, name)):
        o.u16(tid)
        o.u16(sat[s])
    for v in (sat[LIB[2]], 1, 2, sat[LIB[3]], 0, 1, 2, 0):
        o.u16(v)
    o.u32(0)
    o.u32(0)

    timeline = []   # tags, frames
    by_frame = {}
    for k, show in enumerate(shows):
        f, shape, obj = show[:3]
        where = show[3] if len(show) > 3 else at
        if k == 0:
            assert f == 0
            by_frame[f] = [(0x7F, _place(2, obj, shape, f, where, False))]
        else:
            by_frame[f] = [(0x7F, _place(2 + k, obj, shape, f, where, True)), (0x80, _remove(1 + k))]
    tframes, n = [], 0
    for f in range(frames):
        t = by_frame.get(f, [])
        tframes.append((n, len(t)))
        timeline += t
        n += len(t)

    defs = [(0x79, _library_sprite(3, 0, 2)), (0x84, lambda o: (o.u16(0), o.u16(5))), (0x79, _library_sprite(6, 1, 5))]
    defs += [(0x84, (lambda s: lambda o: (o.u16(2), o.u16(s)))(s)) for s in shapes]

    def main(o):
        o.u16(1)
        o.u16(sprite)
        o.u32(8)
        _section(o, tframes, timeline)
    defs.append((0x79, main))
    _section(o, [(len(defs) + first, count) for first, count in tframes], defs + timeline)
    strings_at = len(o.b)
    o.raw(bytes((c + 0x80 + i) & 0xFF for i, c in enumerate(table)))
    o.u32(len(o.b), at=4)
    o.u32(strings_at, at=48)
    o.u32(len(table), at=52)
    return bytes(o.b), bsi([f for f in o.fields if f[0] < strings_at])


# a shape: a picture's quad, w x h, the picture named (3 letters: the old characters' are their
# numbers) and where it is in its texture (u0, v0, u1, v1); the mask's 16 x 16 one (shape 5)
_GE2D_HEAD = bytes.fromhex(
    "474532440001000000010100000000a8000000240004000400000001000100000000004c0000006c000000000000004400"
    "00008c00000000")
_GE2D_TAIL = bytes.fromhex("040300ff00060000000000000000009c000000010002000200010003")
MASK = bytes.fromhex(
    "474532440001000000010100000000800000002400040000000000000001000000000044000000000000000000000000"
    "000000640000000041800000000000004180000000000000000000004180000000000000000000004180000041800000"
    "418000000409ffff00060000ff00ffff00000074000000010002000200010003")


def ge2d(name, w, h, uv, at=(0, 0)):
    """the quad w x h from `at` (its bounds, then its corners), the picture's UVs"""
    assert len(name) == 3
    u0, v0, u1, v1 = uv
    x, y = at
    return (_GE2D_HEAD[:0x34] + struct.pack(">ffffI", x, x + w, y, y + h, 0x48) + name.encode() + b"\0" +
            struct.pack(">8f", x, y, x + w, y, x, y + h, x + w, y + h) + struct.pack(">8f", u0, v0, u1, v0, u0, v1, u1, v1) +
            _GE2D_TAIL)


def texturelist(images, size=2048):
    """pictures [(name, w, h)] (each with a pixel's border) laid out in rows on size x size
    textures: the texturelist (kbin tree) and each picture's UV (u0, v0, u1, v1)"""
    textures, uv = [], {}
    x = y = row = 0
    tex = None
    for name, w, h in images:
        if tex is None or x + w > size:
            x, y, row = 0, y + row, 0
        if tex is None or y + h > size:
            tex = ["texture", 1, None, [["size", 19, (size, size), [], [], 0]],
                   [("cfg", b"eaa-\0"), ("format", b"argb8888rev\0"), ("mag_filter", b"nearest\0"),
                    ("min_filter", b"nearest\0"), ("name", b"tex%03d\0" % len(textures)), ("wrap_s", b"clamp\0"),
                    ("wrap_t", b"clamp\0")], 0]
            textures.append(tex)
            x = y = row = 0
        img = (2 * x, 2 * (x + w), 2 * y, 2 * (y + h))
        inner = (img[0] + 2, img[1] - 2, img[2] + 2, img[3] - 2)
        tex[3].append(["image", 1, None, [["uvrect", 39, inner, [], [], 0], ["imgrect", 39, img, [], [], 0]],
                       [("name", name.encode() + b"\0")], 0])
        uv[name] = (inner[0] / (2 * size), inner[2] / (2 * size), inner[1] / (2 * size), inner[3] / (2 * size))
        x += w
        row = max(row, h)
    return ["texturelist", 1, None, textures, [("compress", b"avslz\0")], 0], uv


def afplist(anims):
    """[(name, shape ids)]"""
    return ["afplist", 1, None,
            [["afp", 1, None, [["geo", 5, tuple(ids), [], [], 64]], [("name", n.encode() + b"\0")], 0] for n, ids in anims],
            [], 0]


def md5(s):
    return hashlib.md5(s.encode() if isinstance(s, str) else s).hexdigest()
