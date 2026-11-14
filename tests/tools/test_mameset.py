#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/mameset.py, which tells tools/ingest.py which MAME set a file is, against a MAME made up
here (a -listxml of four machines and their ROMs, written as zip, 7z and folder): no game's data.

The data is remade from the list (--refresh) and holds what it should; a set's ROMs come with its
parent's, its BIOS's and its device's; an archive is told by what it holds, under any name, also
when an older MAME named it otherwise or dumped one chip differently; files that are no set's
are nobody's; a CHD is told by its header's SHA-1 in each version of the header."""
import json
import os
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import mameset  # noqa: E402

failed = 0


def check(ok, what):
    global failed
    print(("ok    " if ok else "FAIL  ") + what)
    failed += not ok


def rom(name, seed, size=64):
    data = bytes((seed * 37 + k * 11) & 0xff for k in range(size))
    return name, data, f"{zlib.crc32(data):08x}"


BIOS = rom("bios.bin", 1)
OTHER_BIOS = rom("bios-other.bin", 2)
A1, A2 = rom("a1.bin", 3), rom("a2.bin", 4)
B1 = rom("b1.bin", 5)
DEV = rom("unit.bin", 6)
ODD = rom("odd.bin", 7)
A_DISK = "0123456789abcdef0123456789abcdef01234567"


def tag(r, **more):
    extra = "".join(f' {k}="{v}"' for k, v in more.items())
    return f'<rom name="{r[0]}" size="{len(r[1])}" crc="{r[2]}"{extra}/>'


LISTXML = f"""<?xml version="1.0"?>
<mame build="0.999 (made up)">
 <machine name="testbios" sourcefile="konami/djmain.cpp" isbios="yes">
  <description>Test BIOS</description><year>1997</year>
  <biosset name="first" default="yes"/><biosset name="second"/>
  {tag(BIOS, bios="first")}{tag(OTHER_BIOS, bios="second")}
 </machine>
 <machine name="gamea" sourcefile="konami/djmain.cpp" romof="testbios">
  <description>beatmania made up (ver JA-A)</description><year>1998</year>
  {tag(A1, region="maincpu")}{tag(A2)}{tag(BIOS, merge="bios.bin", bios="first")}
  <rom name="missing.bin" size="16" status="nodump"/>
  <disk name="a-hdd" sha1="{A_DISK}" region="ata:0:hdd"/>
  <device_ref name="unit"/><device_ref name="z80"/>
  <driver status="good"/>
 </machine>
 <machine name="gameb" sourcefile="konami/djmain.cpp" cloneof="gamea" romof="gamea">
  <description>beatmania made up (ver JA-B)</description><year>1998</year>
  {tag(B1)}{tag(A2, merge="a2.bin")}{tag(BIOS, merge="bios.bin", bios="first")}
  <driver status="imperfect"/>
 </machine>
 <machine name="unit" sourcefile="konami/unit.cpp" isdevice="yes">
  <description>A unit with a ROM</description>{tag(DEV)}
 </machine>
 <machine name="z80" sourcefile="cpu/z80.cpp" isdevice="yes"><description>A CPU</description></machine>
 <machine name="elsewhere" sourcefile="namco/elsewhere.cpp">
  <description>Another maker's game</description><year>1999</year>{tag(ODD)}
 </machine>
</mame>
"""


def zipped(path, roms):
    with zipfile.ZipFile(path, "w") as z:
        for name, data, _ in roms:
            z.writestr(name, data)
    return path


def chd(path, version, sha1):
    at = {3: 80, 4: 48, 5: 84}[version]
    h = bytearray(124)
    h[:8] = b"MComprHD"
    h[8:12] = struct.pack(">I", 124)
    h[12:16] = struct.pack(">I", version)
    h[at:at + 20] = bytes.fromhex(sha1)
    with open(path, "wb") as f:
        f.write(h)
    return path


with tempfile.TemporaryDirectory() as d:
    xml = os.path.join(d, "list.xml")
    open(xml, "w").write(LISTXML)
    data = os.path.join(d, "data", "sets.json")
    check(mameset.refresh(xml, out=data) == 0, "the data is remade from a -listxml")
    made = json.load(open(data))
    sets = made["sets"]
    check(sorted(sets) == ["gamea", "gameb", "testbios", "unit"], f"the BEMANI boards' sets, their BIOS and the device with a ROM: {sorted(sets)}")
    check(made["mame"].startswith("0.999") and sets["gamea"]["board"] == "djmain" and sets["gamea"]["refs"] == ["unit"], "a set's board and its devices")
    check([r[0] for r in sets["testbios"]["roms"]] == ["bios.bin"], "a BIOS's other choices are left out")
    check("missing.bin" not in [r[0] for r in sets["gamea"]["roms"]], "a ROM never dumped is asked of nobody")
    mameset.DATA, mameset._data, mameset._disk_index = data, None, None
    check(mameset.load_build() == "0.999", "the MAME version the data came from")
    check(mameset.chain("gameb") == ["gameb", "gamea", "testbios"], "a clone's sets: itself, its parent, the BIOS")
    need = {r[0] for r in mameset.needed_roms("gamea")}
    check(need == {"a1.bin", "a2.bin", "bios.bin", "unit.bin"}, f"a set needs its own, its BIOS's and its device's ROMs: {sorted(need)}")
    check({r[0] for r in mameset.own_roms("gameb")} == {"b1.bin"} and {r[0] for r in mameset.own_roms("gamea")} == {"a1.bin", "a2.bin"},
          "a set's own ROMs: not those merged from a parent or BIOS")
    check(mameset.needed_disks("gamea") == [("a-hdd", A_DISK, "ata:0:hdd")] and mameset.needed_disks("gameb") == [], "a set's disks")
    check(mameset.is_bemani("gamea") and not mameset.is_bemani("testbios") and not mameset.is_bemani("unit"), "a game is a game, a BIOS and a device aren't")

    # the same ROMs as a zip, a folder and (where 7z is) a 7z: the same sizes and CRCs
    a_zip = zipped(os.path.join(d, "gamea.zip"), [A1, A2])
    folder = os.path.join(d, "folder")
    os.makedirs(folder)
    for name, body, _ in (A1, A2):
        open(os.path.join(folder, name), "wb").write(body)
    open(os.path.join(folder, "a-hdd.chd"), "wb").write(b"not looked at")
    want = {A1[0]: (64, A1[2]), A2[0]: (64, A2[2])}
    check(mameset.archive_crcs(a_zip) == want and mameset.archive_crcs(folder) == want, "a zip's and a folder's members, by size and CRC (a CHD left out)")
    seven = mameset.seven_zip()
    if seven:
        a_7z = os.path.join(d, "gamea.7z")
        subprocess.run([seven, "a", "-bso0", "-bsp0", a_7z, os.path.join(folder, A1[0]), os.path.join(folder, A2[0])], capture_output=True)
        if os.path.exists(a_7z):
            check(mameset.archive_crcs(a_7z) == want, "a 7z's members, by size and CRC")
            check(mameset.identify(a_7z)[0] == ("gamea", 2, 2), "a 7z is told")
        else:
            print("      (this 7z makes no archive: the 7z part not run)")
    else:
        print("      (no 7z here: the 7z part not run)")

    check(mameset.identify(a_zip)[0] == ("gamea", 2, 2) and mameset.identify(folder)[0] == ("gamea", 2, 2), "an archive is told by what it holds")
    check(mameset.current_name("gamea", a_zip) == "gamea", "a set under its own name keeps it")
    # an older MAME's name for it
    old = zipped(os.path.join(d, "oldname.zip"), [A1, A2])
    check(mameset.current_name("oldname", old) == "gamea", "a set under a name MAME no longer has is told by its ROMs")
    # a non-merged clone's archive: its own ROM and its parent's
    b_zip = zipped(os.path.join(d, "gameb.zip"), [B1, A2, BIOS])
    check(mameset.identify(b_zip)[0][0] == "gameb" and mameset.current_name("gameb", b_zip) == "gameb", "a clone with its parent's ROMs beside its own is the clone")
    # an older MAME called the clone by the parent's name
    os.makedirs(os.path.join(d, "kin"))
    as_parent = zipped(os.path.join(d, "kin", "gamea.zip"), [B1, A2])
    check(mameset.current_name("gamea", as_parent) == "gameb", "a sibling's ROMs under this set's name: the sibling")
    # an older MAME's dump of one chip: the name is there, the CRC another
    redump = zipped(os.path.join(d, "redump.zip"), [A1, (A2[0], b"\x55" * 64, "")])
    check(mameset.identify(redump)[0][0] == "gamea" and mameset.identify(redump)[0][1] == 1, "one chip dumped differently: still told, one ROM found by CRC")
    # files that are no set's
    stray = zipped(os.path.join(d, "stray.zip"), [ODD, rom("readme.txt", 9)])
    check(mameset.identify(stray) == [] and mameset.current_name("stray", stray) is None, "files that are no set's are nobody's")
    check(mameset.identify(zipped(os.path.join(d, "empty.zip"), [])) == [], "an empty archive is nobody's")
    # a zip of another board's game that shares one chip with a set here isn't that set
    mostly = zipped(os.path.join(d, "mostly.zip"), [A2, ODD, rom("x1.bin", 10), rom("x2.bin", 11), rom("x3.bin", 12)])
    check(mameset.current_name("mostly", mostly) is None, "an archive that is mostly something else isn't the set it shares a chip with")

    # disks: a CHD by its header's SHA-1, in each header version
    for v in (3, 4, 5):
        check(mameset.chd_sha1(chd(os.path.join(d, f"v{v}.chd"), v, A_DISK)) == A_DISK, f"a CHD's SHA-1 from a version {v} header")
    open(os.path.join(d, "not.chd"), "wb").write(b"something else entirely, long enough to hold a header's worth of bytes" * 3)
    open(os.path.join(d, "short.chd"), "wb").write(b"MComprHD")
    check(mameset.chd_sha1(os.path.join(d, "not.chd")) is None and mameset.chd_sha1(os.path.join(d, "short.chd")) is None, "a file that is no CHD, and one cut short")
    check(mameset.disk_owner(A_DISK) == [("gamea", "a-hdd")] and mameset.disk_owner("00" * 20) == [], "a disk's set, by SHA-1")

# the repository's own data, as tools/ingest.py reads it: every game the games/ folder names by a
# MAME set is in it
mameset.DATA = os.path.join(ROOT, "tools", "data", "mame-bemani.json")
mameset._data = mameset._disk_index = None
real = mameset.load()
import glob  # noqa: E402
import re  # noqa: E402
named = {}
for toml in glob.glob(os.path.join(ROOT, "games", "*", "game.toml")):
    m = re.search(r'^mame\s*=\s*"([^"]+)"', open(toml, encoding="utf-8").read().split("\n[")[0], re.M)
    if m:
        named[os.path.basename(os.path.dirname(toml))] = m.group(1)
missing = sorted(g for g, s in named.items() if s not in real)
check(len(real) > 200 and len(named) > 100 and not missing, f"the data has every MAME set games/ names ({len(named)} of them): missing {missing[:6]}")
for name in list(named.values())[:40]:
    mameset.chain(name), mameset.needed_roms(name), mameset.needed_disks(name), mameset.own_roms(name)
r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "mameset.py")], capture_output=True, text=True)
check(r.returncode == 2 and "which set each file is" in r.stdout, "with nothing asked it says how it is used")
print("%d failed" % failed if failed else "all hold")
sys.exit(1 if failed else 0)
