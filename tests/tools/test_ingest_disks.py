#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/ingest.py, a System 573 game's disks given without its MAME set (issue 11): told what to
add, the disks kept for the set on its own; then a disk alone for the game added before. Made-up
files: a tiny CHD under the game's disk names and a set of the right names.

    tests/tools/test_ingest_disks.py [hiraku-chdcat]      exit 0 when all hold
"""
import hashlib, os, shutil, struct, subprocess, sys, tempfile, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
INGEST = os.path.join(HERE, "..", "..", "tools", "ingest.py")
T = tempfile.mkdtemp(prefix="hk-disks-")
ENV = dict(os.environ, HK_CHD_TOOL="chdcat", **({"HK_CHDCAT": sys.argv[1]} if len(sys.argv) > 1 else {}))
failed = 0


def check(ok, what):
    global failed
    print(("ok    " if ok else "FAIL  ") + what)
    failed += not ok


def tiny_cd_chd(path, frames=16):
    """An uncompressed CHD v5 of a one-track MODE1_RAW CD (2352-byte frames, 96 bytes of subcode)."""
    unit, hunk = 2448, 2448 * 8
    data = b"".join(bytes([i & 0xff]) * 2352 + b"\0" * 96 for i in range(frames))
    data += b"\0" * (-len(data) % hunk)
    hunks = len(data) // hunk
    meta = b"TRACK:1 TYPE:MODE1_RAW SUBTYPE:NONE FRAMES:%d PREGAP:0 PGTYPE:MODE1 PGSUB:NONE POSTGAP:0\0" % frames
    meta_off = 124 + 4 * hunks
    data_off = -(-(meta_off + 16 + len(meta)) // hunk) * hunk
    sha = hashlib.sha1(data).digest()
    out = b"MComprHD" + struct.pack(">II4IQQQII", 124, 5, 0, 0, 0, 0, len(data), 124, meta_off, hunk, unit) + sha + sha + b"\0" * 20
    out += b"".join(struct.pack(">I", data_off // hunk + i) for i in range(hunks))
    out += b"CHT2" + struct.pack(">IQ", (1 << 24) | len(meta), 0) + meta
    open(path, "wb").write(out + b"\0" * (data_off - len(out)) + data)
    return path


def machine(out):
    with open(os.path.join(out, "gtfrk11m", "machine.txt")) as fh:
        return dict(l.rstrip("\n").split("=", 1) for l in fh if "=" in l)


def ingest(out, *a):
    p = subprocess.run([sys.executable, "-I", INGEST, "--copy-chd", "--out", out, *a], capture_output=True, text=True, env=ENV)
    return p.returncode, p.stdout + p.stderr


try:
    os.makedirs(os.path.join(T, "in"))
    cd, unit = (tiny_cd_chd(os.path.join(T, "in", n + ".chd")) for n in ("d39jaa02", "d39jba02"))
    romset = os.path.join(T, "in", "gtfrk11m.zip")
    with zipfile.ZipFile(romset, "w") as z:
        for n in ("700a01.22g", "h8a01.bin", "gcd39ja.u1", "gcd39ja.u6", "digital-id.bin"):
            z.writestr(n, n.encode() * 8)
    out = os.path.join(T, "dumps")

    # the CHDs alone: an ERROR line (the launcher shows those) naming the set; kept, not added
    rc, log = ingest(out, cd, unit)
    err = [l for l in log.splitlines() if l.startswith("ERROR")]
    check(rc == 1 and len(err) == 1 and "gtfrk11m.zip" in err[0] and "set on its own" in err[0], f"disks alone: exit {rc}, {err}")
    check(not os.path.exists(os.path.join(out, "gtfrk11m", "machine.txt")) and
          os.path.exists(os.path.join(out, "gtfrk11m", "d39jaa02.chd")), "disks alone: kept, the game not added")

    # then the set on its own: the disks kept are its CD and its network unit's
    rc, log = ingest(out, romset)
    m = machine(out) if rc == 0 else {}
    check(rc == 0 and m.get("cd") == "d39jaa02.chd" and m.get("hdd_cd") == "d39jba02.chd", f"the set afterwards: exit {rc}, cd {m.get('cd')}, hdd_cd {m.get('hdd_cd')}")

    # the network unit's CD alone, for the game added before (its set's ROMs in rom/)
    os.remove(os.path.join(out, "gtfrk11m", "d39jba02.chd"))
    with open(os.path.join(out, "gtfrk11m", "machine.txt"), "w") as fh:
        fh.write("".join(f"{k}={v}\n" for k, v in m.items() if k != "hdd_cd"))
    rc, log = ingest(out, unit)
    m = machine(out) if rc == 0 else {}
    check(rc == 0 and m.get("hdd_cd") == "d39jba02.chd" and m.get("bios") == "700a01.22g", f"a disk alone, the game added before: exit {rc}, hdd_cd {m.get('hdd_cd')}")
    if failed:
        print(log)
finally:
    shutil.rmtree(T, ignore_errors=True)
print("test_ingest_disks: " + ("all passed" if not failed else f"{failed} failed"))
sys.exit(1 if failed else 0)
