#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The ee'MALL station's shop, made from the player's own games (docs/EAMUSE3.md, "ee'MALL").

Konami's ee'MALL 1st Avenue station (game id eemall, C33) downloaded its shop's catalogue from a
shop server in the arcade; that catalogue is lost. HIRAKU's server is the shop server: it serves the
folder this tool writes, <data>/eemall-shop, where <data> is the e-AMUSEMENT server's data folder.
The shop sells the ee'MALL songs of the player's installed pop'n music 12 and 13 (pop'n 11's
numbering, which both read: family D11); a song bought opens in those games' ee'MALL folder for the
card that bought it. pop'n 10's and 11's own ee'MALL songs were downloads, lost: not sold. pop'n 14
opens its ee'MALL songs by its event phase only: not sold either.

    tools/eemall_catalogue.py [--data DIR] [--price N] [--quiet]
        writes DIR/eemall-shop (default DIR: the saves' e-AMUSEMENT folder, <saves>/eamuse, as the
        games' built-in server keeps it; hiraku-eamuse: its --data). Without the station installed
        (dumps/eemall/machine.txt) a shop this tool made is removed and nothing is written.
    tools/eemall_catalogue.py --items GAME IMAGE [--comments] [--all]
        what each ee'MALL item number opens in a pop'n music game (popn9..popn14), read from its disk

What it writes (the station's own formats, SSRNet.dll and eemall.exe of revision JAE;
out of the repo's notes: the station's program was read for them):
    sum/file.xml, file.xml, C33/file.xml   the shop server's lists: <datasync><list url path size sum/>,
                                           sum the MD5 (lower-case hex) of the file as served
    C33/date.dat                           the data's version (19 characters)
    C33/records/sound.xml                  the songs: <eamuse-download><sound .../></eamuse-download>,
                                           Shift-JIS; num = the family's base + item (D11: 11000 + N;
                                           runtime/eamuse/server_eemall.cpp reads it back the same way)
    C33/records/NNNN.001-.004              each record's files for the station's DJ mini-game (NNNN its
                                           item number), whose real ones are lost: the same short song
                                           with no notes for all (.001 its silent backing track,
                                           .002/.003 no key sounds, .004 a chart with only its tempo
                                           and its end). HIRAKU's cabinet DLL leaves the DJ game out of
                                           AMUSEMENT (runtime/pc/ezusb_c33.c), but the files stay:
                                           without them the station drops every record and RECORDS'
                                           BUY stops at SYSTEM ERROR (NO FILE)
    C33/records/NNNN.005                   the record's preview, played while RECORDS' cursor rests on
                                           it: the installed pop'n music 13's own music-select preview
                                           of the song (its slot's "pre" part, tools/popnsong.py), which
                                           is already the station's format (PREVIEW below); silence for
                                           a song only pop'n 12 has (its disk's song slots are not read)
    C33/mychara/mychara.xml                GOODS' catalogue: empty (Konami's goods, characters and
                                           clothes are lost)
    catalogue.txt                          the same songs for a person to read
Every file but the lists is in the station's cipher (SSRNet.dll's decode_file: a two-round
block cipher in CBC mode, "%eNc" + the length); its constant key is read from the player's own
SSRNet.dll at run time, never kept here.

Prices: --price, else the server's setting song_price (the data folder's song_price-C33), else 300
points: HIRAKU's guess, Konami's prices are lost (the station's RESTAURANT pays 200-500 points a dish).
Nothing of the games is in this file: addresses and sizes only; names come from the disks.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import struct
import sys
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))


class Error(Exception):
    pass


# ---- the pop'n games' ee'MALL items (out/eemall notes, stage 3: each game's eemall.get parser)
#
# How the games use an item number: the server's <item num="N"/> (eemall.get) is stored as N - 1
# and turned into a music number: pop'n 10: N - 1 + 0x186; pop'n 11-14: N - 1 + 0x1c2.
#   - pop'n 10 and 11: those music numbers are songs downloaded from the arcade's shop server onto
#     the disk (128 / 99 slots); a factory disk has none;
#   - pop'n 12 and 13: the songs are on the disk, marked in the song table (bit `ee`); one is
#     playable when the card's item list holds it (and the card has a KONAMI ID);
#   - pop'n 14: the same songs, marked, opened by the event phase only (no item call).
# program: "C" = boot record C (Konami LZ) at 0x100000; "game.bin" = ee/boot/game.bin (popnfs).
# table: entry 0's flags (u64), stride, count; name/comment: the string pointers' offsets from the
# flags; first: item 1's music number; owned: how many the game's own check takes (12/13: 0x57).
# The tables are those of the revision whose EE program games/<id>/game.toml's ee_sha1 names.
GAMES = {
    "popn10": dict(program="C", base=0x100000, table=0x34CBE8, stride=0x48, count=0x186, name=0x20, comment=None,
                   first=0x186, items=128, family="C20", downloaded=dict(slot=0x1CC, sector=0x150000, step=0xA000)),
    "popn11": dict(program="C", base=0x100000, table=0x328438, stride=0x38, count=0x1C2, name=-0x18, comment=-0x14,
                   first=0x1C2, items=99, family="D11", downloaded=dict(slot=0x208, sector=0x150000, step=0xA000)),
    "popn12": dict(program="C", base=0x100000, table=0x301478, stride=0x38, count=0x253, name=-0x18, comment=-0x14,
                   ee=0x400000, first=0x1C2, items=0x253 - 0x1C2, owned=0x57, family="D11"),
    "popn13": dict(program="game.bin", base=0x173D600, table=0x1941708, stride=0x38, count=0x289, name=-0x18,
                   comment=-0x14, ee=0x100000, first=0x1C2, items=0x289 - 0x1C2, owned=0x57, family="D11"),
    "popn14": dict(program="game.bin", base=0x172ED00, table=0x193D808, stride=0x30, count=700, name=-0x10,
                   comment=-0xC, ee=0x100000, first=0x1C2, items=700 - 0x1C2, phase_only=True, family="D11"),
}
# The games whose ee'MALL songs a purchase opens (their disks hold them): the shop sells these
SOLD_BY = ("popn12", "popn13")
# The station's item number of a family's item N (runtime/eamuse/server_eemall.cpp: EEMALL_FAMILIES)
FAMILY_BASE = {"D11": 11000, "C20": 20000}


def boot_record_c(img):
    import pythonfs
    off, size, _ = pythonfs.records(img)["C"]
    return pythonfs.lz_decode(img.read(off, size))


def revision_ok(game: str, img) -> bool:
    """The disk is the revision the tables are for: its EE program's SHA-1 is the game.toml's ee_sha1
    (read here, at run time; the same check tools/ingest.py identifies the game by)"""
    with open(os.path.join(ROOT, "games", game, "game.toml"), "rb") as f:
        want = tomllib.load(f).get("python", {}).get("ee_sha1", "")
    return bool(want) and hashlib.sha1(boot_record_c(img)).hexdigest() == want


def program(game: str, img_path: str):
    import pythonfs
    g = GAMES[game]
    img = pythonfs.open_image(img_path)
    if g["program"] == "C":
        return img, boot_record_c(img)
    import popnfs
    d = popnfs.Disk(img_path)
    e = d.lookup("ee/boot/game.bin")
    if not e:
        raise Error(f"{game}: no ee/boot/game.bin on this disk")
    return img, d.decrypt(d.raw(e))[0]


def cstr(prog: bytes, base: int, ptr: int):
    o = ptr - base
    if not 0 <= o < len(prog):
        return None
    e = prog.find(b"\0", o)
    try:
        return prog[o:e].decode("cp932")
    except UnicodeDecodeError:
        return None


def entry(prog: bytes, g: dict, music: int):
    at = g["table"] - g["base"] + music * g["stride"]
    flags = struct.unpack_from("<Q", prog, at)[0]

    def text(rel):
        if rel is None:
            return ""
        return cstr(prog, g["base"], struct.unpack_from("<I", prog, at + rel)[0]) or ""
    return flags, text(g["name"]), text(g["comment"])


def items(game: str, img_path: str, every: bool = False, check: bool = True):
    """(item, kind, music, flags, on_disk, name, comment) for each item number the game takes"""
    if game == "popn9":
        return []
    g = GAMES[game]
    img, prog = program(game, img_path)
    if check and not revision_ok(game, img):
        raise Error(f"{game}: {img_path} is another revision than games/{game}/game.toml's (ee_sha1): its tables aren't known")
    out = []
    for n in range(1, g["items"] + 1):
        music = g["first"] + n - 1
        if "downloaded" in g:
            dl = g["downloaded"]
            slot = dl["slot"] + n - 1
            # a downloaded song's first bytes: any that aren't all one value count (a factory disk
            # has zeros or a filler byte there)
            present = len(set(img.read((dl["sector"] + slot * dl["step"]) * 512, 16))) > 1
            if present or every:
                out.append((n, "song (downloaded)", music, None, present, f"(slot {slot}: its text block not decoded)" if present else "", ""))
            continue
        if music >= g["count"]:
            break
        flags, name, comment = entry(prog, g, music)
        marked = bool(flags & g["ee"])
        in_range = n <= g.get("owned", g["items"])
        if (marked and in_range) or every:
            kind = "song" if marked else "(not an ee'MALL song)"
            if marked and not in_range:
                kind = "song (beyond the game's own check)"
            out.append((n, kind, music, flags, True, name, comment))
    return out


def print_items(game: str, path: str, comments: bool, every: bool):
    if game == "popn9":
        print("# pop'n 9: eemall.i_get exists but is never sent (its caller is unreachable): no item opens anything")
        return
    if game not in GAMES:
        raise Error("GAME: one of popn9, " + ", ".join(GAMES))
    g = GAMES[game]
    print("# %s: item -> music = item - 1 + 0x%x%s" % (game, g["first"],
          "; opened by the event phase only (MAX), never by items" if g.get("phase_only") else ""))
    print("# item\tkind\tmusic\tflags\ton_disk\tname" + ("\tcomment" if comments else ""))
    for n, kind, music, flags, present, name, comment in items(game, path, every):
        line = "%d\t%s\t%d\t%s\t%s\t%s" % (n, kind, music, "-" if flags is None else "0x%x" % flags, "yes" if present else "no", name or "-")
        print(line + ("\t" + comment if comments else ""))


# ---- the station's cipher (SSRNet.dll decode_file, revision JAE: 0x100061e0; block 0x10007040;
# round function 0x10006ef0). Five constants: two 8-byte xors, two 4-byte round keys, one 8-byte xor
# at the end; read from the player's SSRNet.dll (KEY_RVAS), never kept here.
MAGIC = b"%eNc"
KEY_RVAS = dict(k_in=(0x1B8D8, 8), k_r1=(0x1B8D0, 4), k_mid=(0x1B8C8, 8), k_r2=(0x1B8D4, 4), k_out=(0x1B8C0, 8))
DECODE_FILE = ("?decode_file@@YAHPAD0@Z", 0x61E0)   # the export's RVA in the revision these are for


def _f(a: bytes, k: bytes) -> bytes:
    a0, a1, a2, a3 = a
    k0, k1, k2, k3 = k
    M = 0xFF
    t1 = ((((a0 << 3) & M) | k0) + a2) & M
    t2 = (k2 + a1 + a3) & M
    t3 = ((k0 + a3) & M) ^ k3
    t4 = ((t3 + t1 + t2) & M) ^ (k1 | a0)
    if t4 < 0x80:
        t2 ^= ((k0 + t3) & M) ^ a3
        t3 ^= ((k1 + t1) & M) ^ a2
    else:
        t1 ^= ((a0 | k2) + (a1 ^ t4)) & M
        t4 ^= ((k3 | a1) + (a0 ^ t3)) & M
    t2 ^= ((k1 | a2) + t4) & M
    t1 ^= (k3 ^ t3) | a0
    t3 ^= ((a3 + t2) & M) ^ k1
    t4 ^= ((k0 | a1) + t1) & M
    return bytes([t2, t1, t3, t4])


def _x(a: bytes, b: bytes) -> bytes:
    return bytes(p ^ q for p, q in zip(a, b))


class Cipher:
    def __init__(self, keys: dict):
        self.k = keys

    def _d(self, c: bytes) -> bytes:   # the station's block transform
        k = self.k
        w = _x(c, k["k_in"])
        w = w[:4] + _x(w[4:], _f(w[:4], k["k_r1"]))
        w = _x(w, k["k_mid"])
        w = w[4:] + w[:4]
        w = w[:4] + _x(w[4:], _f(w[:4], k["k_r2"]))
        return _x(w, k["k_out"])

    def _e(self, p: bytes) -> bytes:   # its inverse
        k = self.k
        w = _x(p, k["k_out"])
        x0 = w[:4]
        w = _x(w[4:], _f(x0, k["k_r2"])) + x0
        w = _x(w, k["k_mid"])
        y0 = w[:4]
        return _x(y0 + _x(w[4:], _f(y0, k["k_r1"])), k["k_in"])

    def encode(self, plain: bytes) -> bytes:
        n = len(plain)
        padded = plain + bytes(-n % 8)
        prev, out = bytes(8), bytearray(MAGIC + struct.pack("<I", n))
        for i in range(0, len(padded), 8):
            prev = self._e(_x(padded[i:i + 8], prev))
            out += prev
        return bytes(out)

    def decode(self, data: bytes) -> bytes:
        if data[:4] != MAGIC or len(data) % 8:
            raise Error("not a file in the station's cipher")
        n = struct.unpack_from("<I", data, 4)[0]
        if (len(data) - 8) // 8 != (n + 7) // 8:
            raise Error("the station's cipher: the length doesn't match the blocks")
        prev, out = bytes(8), bytearray()
        for i in range(8, len(data), 8):
            c = data[i:i + 8]
            out += _x(self._d(c), prev)
            prev = c
        return bytes(out[:n])


class _Pe:
    """a 32-bit Windows DLL's bytes by address (RVA), and its exports by name"""
    def __init__(self, path: str):
        with open(path, "rb") as f:
            self.b = f.read()
        b = self.b
        if b[:2] != b"MZ":
            raise Error(f"{path}: not a Windows program")
        pe = struct.unpack_from("<I", b, 0x3C)[0]
        if b[pe:pe + 4] != b"PE\0\0":
            raise Error(f"{path}: not a Windows program")
        nsec, opt = struct.unpack_from("<H", b, pe + 6)[0], struct.unpack_from("<H", b, pe + 20)[0]
        o = pe + 24
        self.export = struct.unpack_from("<II", b, o + 96)   # (RVA, size) of the export directory
        self.sections = []
        for i in range(nsec):
            s = pe + 24 + opt + 40 * i
            vsize, va, rsize, raw = struct.unpack_from("<IIII", b, s + 8)
            self.sections.append((va, max(vsize, rsize), raw, rsize))

    def at(self, rva: int, n: int) -> bytes:
        for va, size, raw, rsize in self.sections:
            if va <= rva and rva + n <= va + size:
                d = self.b[raw + rva - va: raw + min(rva - va + n, rsize)]
                return d + bytes(n - len(d))
        raise Error("an address outside the program")

    def exported(self, name: str):
        rva, size = self.export
        if not rva:
            return None
        nnames, funcs, names, ords = struct.unpack_from("<IIII", self.at(rva + 24, 16))
        for i in range(nnames):
            p = struct.unpack_from("<I", self.at(names + 4 * i, 4))[0]
            if self.at(p, len(name) + 1) == name.encode() + b"\0":
                o = struct.unpack_from("<H", self.at(ords + 2 * i, 2))[0]
                return struct.unpack_from("<I", self.at(funcs + 4 * o, 4))[0]
        return None


def station_cipher(ssrnet: str) -> Cipher:
    """The cipher with the keys of the player's SSRNet.dll (the station's network library)"""
    pe = _Pe(ssrnet)
    if pe.exported(DECODE_FILE[0]) != DECODE_FILE[1]:
        raise Error(f"{ssrnet}: another revision of the station's network library than this tool knows (decode_file not at its place)")
    return Cipher({name: pe.at(at, size) for name, (at, size) in KEY_RVAS.items()})


# ---- the player's installation (as the launcher sees it: dumps/<id>/machine.txt; the saves as
# runtime/shell/userdir.h)
def machine(game: str) -> dict | None:
    path = os.path.join(ROOT, "dumps", game, "machine.txt")
    if not os.path.exists(path):
        return None
    out = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            k, sep, v = line.rstrip("\r\n").partition("=")
            if sep:
                out[k.strip()] = v.strip()
    return out


def dump_path(game: str, value: str) -> str:
    return value if os.path.isabs(value) else os.path.join(ROOT, "dumps", game, value)


def default_data() -> str:
    if os.path.isdir(os.path.join(ROOT, "user")):
        return os.path.join(ROOT, "user", "data", "eamuse")
    if os.name == "nt" and os.environ.get("LOCALAPPDATA"):
        return os.path.join(os.environ["LOCALAPPDATA"], "hiraku", "eamuse")
    base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    return os.path.join(base, "hiraku", "eamuse")


# ---- the shop's files

def record_url(num: int) -> str:
    """a record's files' name, its sound.xml `url`: four characters (eemall.exe 0x40a910 compares four)"""
    return "%04d" % (num % 10000)


# The RECORDS preview (.005). The station streams it itself (DXAudio.dll's streaming, eemall.exe
# 0x41c641: after the cursor rests 30 frames on a record, SoundPrepareStreamingAddBacktrack(path, 1)):
# the file in the station's cipher ("%eNc", decoded as it streams), holding PlayStation SPU ADPCM,
# stereo at 44.1 kHz, the left and right channels' 16-byte blocks taking turns (DXAudio 0x10005ea2;
# a plain WAV, as served before, failed its cipher check: silence). pop'n music 13's music-select
# previews are exactly that (10 s each), so a song's own one is used as it is.
# Where a game keeps a song: slot = the song table entry's u16 at `slot_at` + 8 (seen in both: the
# music select reads that slot's preview as its cursor reaches the song); slot n = the 20 MB at byte
# 0x20000000 + n * 0x1400000 (tools/popnsong.py), scrambled: XOR with bits 5-12 of an LCG (state =
# slot + `seed`, then state * 0x5d588b65 + `step` before each byte; pop'n 12 0x17ce58/0x17ce60, pop'n
# 13 as popnsong.py), restarted at each read, so one 8 KB pattern a slot. Its header: `magic`, then
# its parts from `parts_at` (sector, size, tag); the preview is the part tagged "pre".
PREVIEW_FROM = {"popn12": dict(slot_at=0x18, magic=b"LB12", parts_at=8, count=16, seed=0, step=3),
                "popn13": dict(slot_at=0x1C, magic=b"LB13", parts_at=12, count=12, seed=13, step=13)}
SLOT_BASE, SLOT_SIZE = 0x20000000, 0x1400000


def silent_preview(seconds: float = 1.0) -> bytes:
    """the station's preview format with nothing in it: zero ADPCM blocks (filter 0, shift 0)"""
    return bytes(32 * int(seconds * RATE / 28))


def _unscramble(data: bytes, k: bytes) -> bytes:
    """tools/popnsong.py's descramble, a whole part at once (its 8 KB pattern repeated)"""
    pad = (k * (len(data) // len(k) + 1))[:len(data)]
    return (int.from_bytes(data, "little") ^ int.from_bytes(pad, "little")).to_bytes(len(data), "little")


def slot_key(slot: int, seed: int, step: int) -> bytes:
    """a song slot's 8 KB XOR pattern"""
    s, out = (slot + seed) & 0xFFFFFFFF, bytearray()
    for _ in range(0x2000):
        s = (s * 0x5D588B65 + step) & 0xFFFFFFFF
        out.append((s >> 5) & 0xFF)
    return bytes(out)


def previews(game: str, img_path: str, musics: dict) -> dict:
    """{item: the game's own preview of that item's song} for the items in `musics` (item: music)"""
    p = PREVIEW_FROM.get(game)
    if not p:
        return {}
    g = GAMES[game]
    img, prog = program(game, img_path)
    out = {}
    for n, music in musics.items():
        slot = struct.unpack_from("<H", prog, g["table"] - g["base"] + music * g["stride"] + p["slot_at"])[0] + 8
        k = slot_key(slot, p["seed"], p["step"])
        head = _unscramble(img.read(SLOT_BASE + slot * SLOT_SIZE, p["parts_at"] + 12 * p["count"]), k)
        if head[:4] != p["magic"]:
            continue
        for i in range(p["count"]):
            sector, size, tag = struct.unpack_from("<3I", head, p["parts_at"] + 12 * i)
            # (a stereo stream: whole pairs of 16-byte blocks, in the slot)
            if tag == 0x657270 and 0 < size <= 0x200000 and size % 32 == 0 and \
                    SLOT_BASE + slot * SLOT_SIZE <= sector * 512 < SLOT_BASE + (slot + 1) * SLOT_SIZE:
                out[n] = _unscramble(img.read(sector * 512, size), k)
                break
    return out
RATE = 44100


def wav(seconds: float) -> bytes:
    """silence, 44.1 kHz 16-bit mono (the station's own sounds' format)"""
    data = bytes(int(RATE * seconds) * 2)
    return (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16) +
            b"data" + struct.pack("<I", len(data)) + data)


def chart(end_ms: int, bpm: int) -> bytes:
    """the DJ game's chart (eemall.exe 0x425dbb, 0x425f14): 8-byte events, u32 time, byte 5 the
    kind (4 tempo, value u16 at 6; 6 the end), read up to the end: a tempo and the end, no notes"""
    return struct.pack("<IBBH", 0, 0, 4, bpm) + struct.pack("<IBBH", end_ms, 0, 6, 0)


def text(s: str, most: int = 120) -> bytes:
    """a record's text as the station draws it (Shift-JIS, full-width), at most `most` bytes"""
    # the station's font draws full-width letters only (its own texts are all full-width; ASCII
    # titles showed as blank lines in RECORDS): ASCII as its full-width forms, which also keeps
    # & < > " out of the XML (the apostrophe as ’: its full-width form isn't drawn either)
    s = "".join("\u3000" if c == " " else "\u2019" if c == "'" else chr(ord(c) + 0xFEE0) if "!" <= c <= "~" else c for c in s)
    b = s.encode("cp932", "replace")
    while len(b) > most:
        s = s[:-1]
        b = s.encode("cp932", "replace")
    return b


def catalogue_from_games(quiet: bool) -> list[dict]:
    """the songs a purchase opens, from the installed pop'n 12 and 13: [{family, item, title, games}]"""
    songs: dict[int, dict] = {}
    for game in SOLD_BY:
        m = machine(game)
        if not m or not m.get("hdd"):
            continue
        try:
            found = items(game, dump_path(game, m["hdd"]))
        except Error as e:
            print(f"eemall_catalogue: {e}: its songs are left out", file=sys.stderr)
            continue
        except OSError as e:
            print(f"eemall_catalogue: {game}: {e}: its songs are left out", file=sys.stderr)
            continue
        for n, kind, music, flags, present, name, comment in found:
            s = songs.setdefault(n, dict(family=GAMES[game]["family"], item=n, title=name, games=[]))
            s["games"].append(game)
        try:
            for n, data in previews(game, dump_path(game, m["hdd"]), {f[0]: f[2] for f in found}).items():
                songs[n].setdefault("preview", data)
        except (Error, OSError) as e:
            print(f"eemall_catalogue: {game}: its previews not read ({e})", file=sys.stderr)
        if not quiet:
            print(f"eemall_catalogue: {game}: {len(found)} ee'MALL songs")
    return [songs[n] for n in sorted(songs)]


def catalogue_from_table(path: str) -> list[dict]:
    """a made-up table (tests): FAMILY<TAB>ITEM<TAB>TITLE[<TAB>GAMES, comma-separated] a line"""
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.rstrip("\n").split("\t")
            out.append(dict(family=parts[0], item=int(parts[1]), title=parts[2], games=(parts[3].split(",") if len(parts) > 3 else [])))
    return out


GAME_TITLES = {"popn12": "pop'n music 12", "popn13": "pop'n music 13"}


# A record's `target`: the SUPPORT box's machine icons in RECORDS (eemall.exe 0x41bbb3: bits 1, 2, 4,
# 8; with all four the box shows pop'n music, GuitarFreaks, drummania, beatmania IIDX top to bottom,
# seen in a run): pop'n music's
TARGET_POPN = 1


def sound_xml(songs: list[dict], price: int, target: int = TARGET_POPN) -> bytes:
    out = bytearray(b"<eamuse-download>")
    for s in songs:
        num = FAMILY_BASE[s["family"]] + s["item"]
        if not 0 < num <= 0xFFFF:
            raise Error(f"item {s['item']} of {s['family']}: no station number for it")
        genre = " / ".join(GAME_TITLES.get(g, g) for g in s["games"])
        # the artist isn't text on the disks: the first game that opens the song in its place (the
        # SELL list shows "title/artist"; an empty one left a bare "/")
        artist = GAME_TITLES.get(s["games"][0], s["games"][0]) if s["games"] else ""
        out += (b'<sound target="' + str(target).encode() + b'" title="' + text(s["title"]) + b'" artist="' + text(artist) + b'" genre="' + text(genre) + b'" point="' +
                str(price).encode() + b'" bpm_start="0" level_ee="0" level_eh="0" num="' + str(num).encode() + b'" url="' +
                record_url(num).encode() + b'" vol_bt="100" vol_ele="100" vol_2mix="100" anime="0"/>')
    return bytes(out + b"</eamuse-download>")


def listing(files: list[tuple[str, bytes]]) -> bytes:
    """the shop server's list of files: <datasync><list url path size sum/>...</datasync>"""
    return ("<datasync>" + "".join(f'<list url="/{p}" path="{p}" size="{len(d)}" sum="{hashlib.md5(d).hexdigest()}"/>' for p, d in files) +
            "</datasync>").encode()


def shop_files(songs: list[dict], price: int, cipher: Cipher, target: int = TARGET_POPN) -> list[tuple[str, bytes]]:
    version = hashlib.sha1(sound_xml(songs, price, target)).hexdigest()[:12]
    station = [("C33/date.dat", cipher.encode(("HIRAKU " + version).encode()[:19].ljust(19))),
               ("C33/records/sound.xml", cipher.encode(sound_xml(songs, price, target))),
               ("C33/mychara/mychara.xml", cipher.encode(b"<eamuse-download></eamuse-download>"))]
    if songs:   # a record without its DJ files is dropped by the station (eemall.exe 0x40a910), even with the DJ game left out
        dj = [cipher.encode(wav(3.0)), cipher.encode(bytes(8)), cipher.encode(bytes(8)), cipher.encode(chart(2500, 120))]
        silent = cipher.encode(silent_preview())
        for s in songs:   # (.002 key sounds' data: none; .003 their index, 18 bytes each: none)
            url = record_url(FAMILY_BASE[s["family"]] + s["item"])
            station += [(f"C33/records/{url}.00{i + 1}", d) for i, d in enumerate(dj)]
            station.append((f"C33/records/{url}.005", cipher.encode(s["preview"]) if s.get("preview") else silent))
    c33 = listing(station)
    top = listing([("C33/file.xml", c33)])
    return station + [("C33/file.xml", c33), ("file.xml", top), ("sum/file.xml", listing([("file.xml", top)]))]


def write_shop(data: str, files: list[tuple[str, bytes]], songs: list[dict], price: int):
    shop = os.path.join(data, "eemall-shop")
    new = shop + ".new"
    shutil.rmtree(new, ignore_errors=True)
    for p, d in files:
        os.makedirs(os.path.dirname(os.path.join(new, p)), exist_ok=True)
        with open(os.path.join(new, p), "wb") as f:
            f.write(d)
    with open(os.path.join(new, "catalogue.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# ee'MALL station's shop, made by tools/eemall_catalogue.py from the installed games\n")
        f.write("# station_num\tfamily\titem\tprice\ttitle\tgames\tpreview\n")
        for s in songs:
            f.write(f"{FAMILY_BASE[s['family']] + s['item']}\t{s['family']}\t{s['item']}\t{price}\t{s['title']}\t{','.join(s['games'])}"
                    f"\t{'yes' if s.get('preview') else 'none'}\n")
    old = shop + ".old"
    shutil.rmtree(old, ignore_errors=True)
    if os.path.exists(shop):
        os.replace(shop, old)
    os.replace(new, shop)
    shutil.rmtree(old, ignore_errors=True)


def owner_price(data: str) -> int:
    try:
        with open(os.path.join(data, "song_price-C33"), encoding="utf-8") as f:
            v = int(f.read().strip())
        if 1 <= v <= 99999:
            return v
    except (OSError, ValueError):
        pass
    return 300


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--data", help="the e-AMUSEMENT server's data folder (default: the saves' eamuse)")
    ap.add_argument("--price", type=int, help="a song's price in points (default: the server's song_price, else 300)")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--items", nargs=2, metavar=("GAME", "IMAGE"), help="print what each ee'MALL item opens in GAME's disk")
    ap.add_argument("--comments", action="store_true", help="--items: with each song's comment")
    ap.add_argument("--all", action="store_true", help="--items: every item number the game takes")
    # tests: songs from a made-up table and a made-up key (no station, no games)
    ap.add_argument("--table", help=argparse.SUPPRESS)
    ap.add_argument("--test-key", help=argparse.SUPPRESS)
    ap.add_argument("--root", help=argparse.SUPPRESS)   # another HIRAKU folder's dumps (tests)
    ap.add_argument("--target", type=int, default=1, help=argparse.SUPPRESS)   # (tests: the records' SUPPORT icons)
    a = ap.parse_args()
    global ROOT
    if a.root:
        ROOT = os.path.abspath(a.root)
    try:
        if a.items:
            print_items(a.items[0], a.items[1], a.comments, a.all)
            return 0
        data = a.data or default_data()
        shop = os.path.join(data, "eemall-shop")
        if a.test_key:
            k = bytes.fromhex(a.test_key)
            if len(k) != 32:
                raise Error("--test-key: 32 bytes in hex")
            cipher = Cipher(dict(k_in=k[0:8], k_r1=k[8:12], k_mid=k[12:20], k_r2=k[20:24], k_out=k[24:32]))
        else:
            m = machine("eemall")
            if not m:   # no station among the player's games: no shop (one made before is removed)
                if os.path.exists(os.path.join(shop, "catalogue.txt")):
                    shutil.rmtree(shop)
                    if not a.quiet:
                        print(f"eemall_catalogue: no ee'MALL station installed: {shop} removed")
                elif not a.quiet:
                    print("eemall_catalogue: no ee'MALL station installed (dumps/eemall): no shop made")
                return 0
            cipher = station_cipher(os.path.join(dump_path("eemall", m.get("program", "")), "SSRNet.dll"))
        songs = catalogue_from_table(a.table) if a.table else catalogue_from_games(a.quiet)
        price = a.price if a.price else owner_price(data)
        if not songs:
            # a shop with no records is worse than none: the station opens RECORDS and its BUY stops
            # at SYSTEM ERROR (NO FILE) (seen 2026-11-24); with no shop it keeps RECORDS closed
            if os.path.exists(os.path.join(shop, "catalogue.txt")):
                shutil.rmtree(shop)
            if not a.quiet:
                print("eemall_catalogue: no ee'MALL songs (no pop'n music 12 or 13 installed): no shop")
            return 0
        os.makedirs(data, exist_ok=True)
        write_shop(data, shop_files(songs, price, cipher, a.target), songs, price)
        if not a.quiet:
            print(f"eemall_catalogue: {len(songs)} songs at {price} points ({sum(1 for s in songs if s.get('preview'))} with a preview) in {shop}")
        return 0
    except Error as e:
        print(f"eemall_catalogue: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
