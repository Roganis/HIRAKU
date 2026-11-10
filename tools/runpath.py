#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A package's programs and libraries look for their libraries inside the package only.

    tools/runpath.py fix [--else PATH] FILE...   each ELF file's run path (RPATH or RUNPATH) with only
                                                 its entries that start at the file's own folder
                                                 ($ORIGIN...) left, in place; --else PATH: the run
                                                 path of a file that had one and none of it is left
    tools/runpath.py check FOLDER                every ELF file under FOLDER: exit 1, naming them,
                                                 when a run path has any other entry

A build leaves in each binary, after $ORIGIN/lib, the folders it linked against (CMake adds them
so the build's own programs run): on another PC those are looked in before the system's folders
for every library the package doesn't bring (libz, libc...), so whoever can make a folder of that
name there chooses what the program loads. tools/package.sh runs both on what it packs.

The new path is written over the old one, which is never shorter, and the rest filled with zeros:
nothing in the file moves. Files that aren't 64-bit little-endian ELF are passed over.
"""
import os
import struct
import sys

DT_NULL, DT_NEEDED, DT_STRTAB, DT_STRSZ, DT_SONAME, DT_RPATH, DT_RUNPATH = 0, 1, 5, 10, 14, 15, 29
DT_AUXILIARY, DT_FILTER = 0x7FFFFFFD, 0x7FFFFFFF
PT_LOAD, PT_DYNAMIC, SHT_DYNSYM = 1, 2, 11


class Elf:
    """the parts of a 64-bit little-endian ELF file that name strings; .dyn is empty for others"""

    def __init__(self, data: bytes):
        self.dyn = []        # (file offset of the entry, tag, value)
        self.strtab = 0      # file offset of the dynamic string table
        self.strsz = 0
        self.symnames = []   # the dynamic symbols' name offsets (from the section table, when the file has one)
        if len(data) < 64 or data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
            return
        phoff, shoff = struct.unpack_from("<QQ", data, 32)
        phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHH", data, 54)
        loads, dynamic = [], None
        for i in range(phnum):
            p_type, _, p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<IIQQQQ", data, phoff + i * phentsize)
            if p_type == PT_LOAD:
                loads.append((p_vaddr, p_offset, p_filesz))
            elif p_type == PT_DYNAMIC:
                dynamic = (p_offset, p_filesz)
        if not dynamic:
            return
        strtab_addr = None
        for at in range(dynamic[0], dynamic[0] + dynamic[1] - 15, 16):
            tag, val = struct.unpack_from("<qQ", data, at)
            if tag == DT_NULL:
                break
            self.dyn.append((at, tag, val))
            if tag == DT_STRTAB:
                strtab_addr = val
            elif tag == DT_STRSZ:
                self.strsz = val
        for vaddr, offset, filesz in loads:
            if strtab_addr is not None and vaddr <= strtab_addr < vaddr + filesz:
                self.strtab = offset + strtab_addr - vaddr
        if not self.strtab or self.strtab + self.strsz > len(data):
            self.dyn = []
            return
        for i in range(shnum if shoff else 0):
            base = shoff + i * shentsize
            if base + 64 > len(data):
                break
            sh_type = struct.unpack_from("<I", data, base + 4)[0]
            sh_offset, sh_size = struct.unpack_from("<QQ", data, base + 24)
            if sh_type == SHT_DYNSYM:
                self.symnames = [struct.unpack_from("<I", data, at)[0] for at in range(sh_offset, sh_offset + sh_size - 23, 24)]

    def string(self, data: bytes, off: int) -> str:
        end = data.index(b"\0", self.strtab + off)
        return data[self.strtab + off:end].decode("utf-8", "surrogateescape")

    def run_paths(self, data: bytes) -> list:
        """(string offset, "RPATH" or "RUNPATH", the path)"""
        return [(val, "RPATH" if tag == DT_RPATH else "RUNPATH", self.string(data, val))
                for _, tag, val in self.dyn if tag in (DT_RPATH, DT_RUNPATH) and val < self.strsz]


def inside(entry: str) -> bool:
    return entry == "$ORIGIN" or entry.startswith("$ORIGIN/") or entry == "${ORIGIN}" or entry.startswith("${ORIGIN}/")


def outside(path: str) -> list:
    """the entries of a run path that aren't in the file's own folder (an empty one is the folder the program is run from)"""
    return [e or "(the current folder)" for e in path.split(":") if not inside(e)] if path else []


def fix(file: str, otherwise: str) -> str:
    """"" (nothing to do), or what was done; raises ValueError when it can't be done safely"""
    with open(file, "rb") as f:
        data = bytearray(f.read())
    elf = Elf(bytes(data))
    done = []
    for off, kind, old in elf.run_paths(bytes(data)):
        if not outside(old):
            continue
        new = ":".join(e for e in old.split(":") if inside(e)) or otherwise
        raw_old, raw_new = old.encode("utf-8", "surrogateescape"), new.encode("utf-8", "surrogateescape")
        if len(raw_new) > len(raw_old):
            raise ValueError(f"{file}: {new} doesn't fit where {old} is")
        # the string is the path's alone: no longer string ends in it, no other name starts inside it
        if off and data[elf.strtab + off - 1] != 0:
            raise ValueError(f"{file}: its run path is the end of another string")
        others = [v for _, tag, v in elf.dyn if tag in (DT_NEEDED, DT_SONAME, DT_AUXILIARY, DT_FILTER)] + elf.symnames
        if any(off < v < off + len(raw_old) for v in others):
            raise ValueError(f"{file}: another name shares its run path's string")
        at = elf.strtab + off
        data[at:at + len(raw_old)] = raw_new + bytes(len(raw_old) - len(raw_new))
        done.append(f"{kind} {old} -> {new or '(none)'}")
    if done:
        tmp = file + ".runpath"
        with open(tmp, "wb") as f:
            f.write(data)
        os.chmod(tmp, os.stat(file).st_mode & 0o7777)
        os.replace(tmp, file)
    return "; ".join(done)


def check(folder: str) -> int:
    bad = 0
    for d, _, files in os.walk(folder):
        for name in sorted(files):
            file = os.path.join(d, name)
            if os.path.islink(file) or not os.path.isfile(file):
                continue
            with open(file, "rb") as f:
                if f.read(4) != b"\x7fELF":
                    continue
                f.seek(0)
                data = f.read()
            for _, kind, path in Elf(data).run_paths(data):
                if outside(path):
                    print(f"runpath: {os.path.relpath(file, folder)} looks for libraries outside the package: "
                          f"{', '.join(outside(path))} ({kind} {path})", file=sys.stderr)
                    bad += 1
    return 1 if bad else 0


def main() -> int:
    a = sys.argv[1:]
    if a[:1] == ["check"] and len(a) == 2:
        return check(a[1])
    if a[:1] == ["fix"] and len(a) >= 2:
        otherwise = ""
        files = a[1:]
        if files[0] == "--else" and len(files) >= 2:
            otherwise, files = files[1], files[2:]
        try:
            for file in files:
                if os.path.islink(file) or not os.path.isfile(file):
                    continue
                said = fix(file, otherwise)
                if said:
                    print(f"runpath: {file}: {said}")
        except ValueError as e:
            print(f"runpath: {e}", file=sys.stderr)
            return 1
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
