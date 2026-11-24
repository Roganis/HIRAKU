# SPDX-License-Identifier: GPL-3.0-or-later
"""Firebeat program discs and executables.

A Firebeat BIOS boots the program CD, reads one executable from its ISO9660
root, LZSS-unpacks it to RAM at 0x80000000 and jumps through the last word of
the unpacked image. This module does the same thing offline:

    disc image (.iso / .bin+.cue: 2048-byte sectors, or raw 2352-byte ones of mode 1 or mode 2 XA)
      -> ISO9660 root -> HIKARU.EXE / FIREBEAT.EXE
      -> optional ParaPara "!:EXE:00" wrapper
      -> u32 LE unpacked size + Konami LZSS
      -> flat big-endian PPC image, entry = target of the trailing branch

Format facts from DragonMinded's firebeat notes (README "Executable Format");
the code here is an independent implementation.
"""
from __future__ import annotations

import os
import struct
from dataclasses import dataclass

EXE_NAMES = ("HIKARU.EXE", "FIREBEAT.EXE")
LOAD_ADDR = 0x80000000
PPP_MAGIC = b"!:EXE:00"


# --------------------------------------------------------------------------
# Disc images
# --------------------------------------------------------------------------

class Disc:
    """Read-only 2048-byte logical sectors from an ISO, BIN/CUE, or raw track."""

    SYNC = b"\x00" + b"\xff" * 10 + b"\x00"

    def __init__(self, path: str) -> None:
        if path.lower().endswith(".cue"):
            path = self._bin_from_cue(path)
        self.path = path
        self.f = open(path, "rb")
        size = os.fstat(self.f.fileno()).st_size
        head = self.f.read(24)
        if head[:12] == self.SYNC and size % 2352 == 0:
            # the sector's own header says its mode: mode 1 has its data after the 16-byte header, a
            # mode 2 sector of a CD-ROM XA disc after an 8-byte subheader too (4 bytes, twice)
            mode = head[15]
            if mode not in (1, 2):
                raise ValueError(f"{path}: raw sector mode {mode}, expected mode 1 or 2")
            xa = mode == 2 and head[16:20] == head[20:24]
            self.raw, self.stride, self.skip = True, 2352, 24 if xa else 16
        elif size % 2048 == 0:
            self.raw, self.stride, self.skip = False, 2048, 0
        else:
            raise ValueError(f"{path}: size {size} is neither 2048- nor 2352-byte sectors")
        self.nsectors = size // self.stride

    @staticmethod
    def _bin_from_cue(cue: str) -> str:
        base = os.path.dirname(cue)
        with open(cue, encoding="latin-1") as fh:
            for line in fh:
                line = line.strip()
                if line.upper().startswith("FILE"):
                    name = line[line.index('"') + 1:line.rindex('"')]
                    return os.path.join(base, name)  # first track = data track
        raise ValueError(f"{cue}: no FILE line")

    def read(self, lba: int, count: int = 1) -> bytes:
        if not self.raw:
            self.f.seek(lba * 2048)
            return self.f.read(count * 2048)
        out = bytearray()
        for i in range(count):
            self.f.seek((lba + i) * self.stride + self.skip)
            out += self.f.read(2048)
        return bytes(out)

    def close(self) -> None:
        self.f.close()

    def __enter__(self) -> "Disc":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


@dataclass
class IsoEntry:
    name: str
    lba: int
    size: int
    is_dir: bool


class Iso9660:
    def __init__(self, disc: Disc) -> None:
        self.disc = disc
        pvd = disc.read(16)
        if pvd[0] != 1 or pvd[1:6] != b"CD001":
            raise ValueError("no ISO9660 primary volume descriptor at sector 16")
        self.volume_id = pvd[40:72].decode("ascii", "replace").strip()
        self.root = self._record(pvd[156:156 + 34])

    @staticmethod
    def _record(r: bytes) -> IsoEntry:
        lba = struct.unpack_from("<I", r, 2)[0]
        size = struct.unpack_from("<I", r, 10)[0]
        flags = r[25]
        nlen = r[32]
        name = r[33:33 + nlen].decode("ascii", "replace")
        name = name.split(";")[0]
        return IsoEntry(name, lba, size, bool(flags & 2))

    def listdir(self, d: IsoEntry) -> list[IsoEntry]:
        data = self.disc.read(d.lba, (d.size + 2047) // 2048)
        out, pos = [], 0
        while pos < len(data):
            n = data[pos]
            if n == 0:  # records never straddle sectors; skip to the next one
                pos = (pos // 2048 + 1) * 2048
                continue
            e = self._record(data[pos:pos + n])
            if e.name not in ("\x00", "\x01"):
                out.append(e)
            pos += n
        return out

    def find(self, path: str) -> IsoEntry | None:
        cur = self.root
        for part in [p for p in path.upper().split("/") if p]:
            hit = [e for e in self.listdir(cur) if e.name.upper() == part]
            if not hit:
                return None
            cur = hit[0]
        return cur

    def read_file(self, e: IsoEntry) -> bytes:
        return self.disc.read(e.lba, (e.size + 2047) // 2048)[:e.size]


# --------------------------------------------------------------------------
# Executables
# --------------------------------------------------------------------------

def lzss_unpack(src: bytes, limit: int | None = None) -> bytes:
    """Konami LZSS: 4 KiB ring initialised to 0, write cursor starts at 0xFEE,
    flag bits LSB first (1 = literal), back-reference = 12-bit position +
    4-bit length (+3). A zero reference at the very end terminates."""
    ring = bytearray(0x1000)
    w = 0xFEE
    out = bytearray()
    i, n = 0, len(src)
    while i < n:
        flags = src[i]
        i += 1
        for bit in range(8):
            if flags & (1 << bit):
                if i >= n:
                    return bytes(out)
                b = src[i]
                i += 1
                out.append(b)
                ring[w] = b
                w = (w + 1) & 0xFFF
            else:
                if i + 1 >= n:
                    return bytes(out)
                hi, lo = src[i], src[i + 1]
                i += 2
                if hi == 0 and lo == 0 and i == n:
                    return bytes(out)
                pos = hi | ((lo << 4) & 0xF00)
                for _ in range((lo & 0xF) + 3):
                    b = ring[pos]
                    out.append(b)
                    ring[w] = b
                    w = (w + 1) & 0xFFF
                    pos = (pos + 1) & 0xFFF
            if limit is not None and len(out) >= limit:
                return bytes(out[:limit])
    return bytes(out)


@dataclass
class Program:
    image: bytes          # flat big-endian PPC image, loaded at LOAD_ADDR
    load_addr: int
    entry: int            # target of the trailing branch
    ppp_wrapped: bool


def branch_target(word: int, at: int) -> int | None:
    """Decode an I-form `b` (no link, no absolute) located at guest address `at`."""
    if word >> 26 != 18 or word & 3:
        return None
    off = word & 0x03FFFFFC
    if off & 0x02000000:
        off -= 0x04000000
    return (at + off) & 0xFFFFFFFF


def unpack_exe(data: bytes) -> Program:
    wrapped = data[:8] == PPP_MAGIC
    load = LOAD_ADDR
    if wrapped:
        if data[-8:] != PPP_MAGIC:
            raise ValueError("ParaPara exe: header magic without footer")
        csize, load = struct.unpack_from(">II", data, 8)
        if csize + 40 != len(data):
            raise ValueError(f"ParaPara exe: size field {csize} != {len(data) - 40}")
        data = data[32:-8]
    size = struct.unpack_from("<I", data, 0)[0]
    image = lzss_unpack(data[4:], limit=size)
    if len(image) != size:
        raise ValueError(f"LZSS: unpacked {len(image)} bytes, header says {size}")
    last = load + size - 4
    entry = branch_target(struct.unpack_from(">I", image, size - 4)[0], last)
    if entry is None:
        raise ValueError("last word of the image is not a relative branch")
    return Program(image, load, entry, wrapped)


def find_exe(iso: Iso9660) -> IsoEntry:
    root = {e.name.upper(): e for e in iso.listdir(iso.root)}
    for name in EXE_NAMES:
        if name in root:
            return root[name]
    raise FileNotFoundError("no HIKARU.EXE / FIREBEAT.EXE in the disc root")
