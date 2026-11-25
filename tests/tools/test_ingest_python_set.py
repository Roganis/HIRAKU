#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/ingest.py and a Konami Python game given by its MAME set or CHD name: it is added from its
descriptor (.py2, .py1) beside them or in a folder given, at any depth; without one, the error says
what the game is added from, and never calls it a Firebeat game (which a Firebeat game without its
program CD still is). Nothing of a Python game's CHD is extracted. Made-up files only: an empty set
named like MAME's, a tiny CHD under the disk's name, a .py2 naming a disk of zeros; no game data.

    tests/tools/test_ingest_python_set.py      exit 0 when all hold
"""
import hashlib, os, shutil, struct, subprocess, sys, tempfile, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
INGEST = os.path.join(HERE, "..", "..", "tools", "ingest.py")
T = tempfile.mkdtemp(prefix="hk-pyset-")
failed = 0


def check(ok, what, log=""):
    global failed
    print(("ok    " if ok else "FAIL  ") + what)
    if not ok and log:
        print("      " + log.replace("\n", "\n      "))
    failed += not ok


def tiny_chd(path, frames=16):
    """An uncompressed CHD v5 (one CD track of 16 frames): only its name and header are read here."""
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
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(out + b"\0" * (data_off - len(out)) + data)
    return path


def empty_set(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("readme.txt", b"made up")
    return path


def py2_beside(folder, name="drmnvj"):
    """a .py2 naming a disk of zeros (no Konami partition: ingest stops there) and an HDD ID"""
    open(os.path.join(folder, "disk.img"), "wb").write(b"\0" * 4096)
    open(os.path.join(folder, "hdd_id.bin"), "wb").write(b"\0" * 128)
    with open(os.path.join(folder, name + ".py2"), "w") as f:
        f.write("[Game]\nName=Made up\nHddImagePath=disk.img\nHddIdPath=hdd_id.bin\n")


def ingest(out, *inputs):
    p = subprocess.run([sys.executable, "-I", INGEST, "--copy-chd", "--out", out, *inputs], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def errors(log):
    return [l for l in log.splitlines() if l.startswith("ERROR")]


try:
    out = os.path.join(T, "dumps")
    # (a) Python 2: MAME's set and CHD, no .py2 anywhere: one error naming the game and the .py2
    a = os.path.join(T, "a")
    rc, log = ingest(out, empty_set(os.path.join(a, "drmnvj.zip")), tiny_chd(os.path.join(a, "e02_jaa_2005050201.chd")))
    err = errors(log)
    check(rc == 1 and len(err) == 1 and "DrumMania V (drmnvj) is a Konami Python 2 game" in err[0]
          and "its .py2 file" in err[0] and "a MAME set and CHD alone are not enough" in err[0], f"(a) set + CHD: exit {rc}, {err}", log)
    check("Firebeat" not in log, "(a) not called a Firebeat game", log)
    check(not any(n.endswith((".img", ".part")) for _, _, ns in os.walk(out) for n in ns) if os.path.isdir(out) else True,
          "(a) nothing extracted from the CHD")
    # ... and the CHD alone: told by its name all the same
    rc, log = ingest(out, os.path.join(a, "e02_jaa_2005050201.chd"))
    check(rc == 1 and "(drmnvj) is a Konami Python 2 game" in log and "Firebeat" not in log, f"(a) the CHD alone: exit {rc}", log)

    # (b) the same with a .py2 beside them: the .py2 is read (and its made-up disk then refused)
    b = os.path.join(T, "b")
    zb, cb = empty_set(os.path.join(b, "drmnvj.zip")), tiny_chd(os.path.join(b, "e02_jaa_2005050201.chd"))
    py2_beside(b)
    rc, log = ingest(out, zb, cb)
    check("found    drmnvj.py2 beside the files given" in log and "no Konami game partition" in log and "Firebeat" not in log,
          f"(b) a .py2 beside the set: read (exit {rc})", log)
    # ... the three files given at once: the .py2 read once
    rc, log = ingest(out, os.path.join(b, "drmnvj.py2"), zb, cb)
    check(log.count("no Konami game partition") == 1 and "Firebeat" not in log and "is a Konami Python 2 game" not in log,
          f"(b) .py2, set and CHD given together: the .py2 read once (exit {rc})", log)

    # (c) a Firebeat game's set without its program CD: still the Firebeat line
    rc, log = ingest(out, empty_set(os.path.join(T, "c", "bm3.zip")))
    err = errors(log)
    check(rc == 1 and len(err) == 1 and "bm3 is a Firebeat game: its program CD is needed" in err[0], f"(c) Firebeat: exit {rc}, {err}", log)

    # (d) a folder holding the set's folder (the .py2 two levels down, with the set and the CHD):
    # the .py2 is found and read, nothing else said about the set
    d = os.path.join(T, "d", "collection")
    sub = os.path.join(d, "drmnvj")
    empty_set(os.path.join(sub, "drmnvj.zip"))
    tiny_chd(os.path.join(sub, "e02_jaa_2005050201.chd"))
    py2_beside(sub)
    rc, log = ingest(out, d)
    check(log.count("no Konami game partition") == 1 and "Firebeat" not in log and "which game" not in log,
          f"(d) a collection's folder: its .py2 read (exit {rc})", log)
    rc, log = ingest(out, sub)
    check(log.count("no Konami game partition") == 1 and "Firebeat" not in log, f"(d) the set's own folder: its .py2 read (exit {rc})", log)

    # ... a collection with a Python 1 game's .py1 and a Python 2 game's .py2: each read
    f = os.path.join(T, "f")
    os.makedirs(os.path.join(f, "popn9"))
    open(os.path.join(f, "popn9", "popn9.py1"), "w").write("[Game]\nName=Made up\nHddImagePath=nodisk.chd\n")
    os.makedirs(os.path.join(f, "drmnvj"))
    py2_beside(os.path.join(f, "drmnvj"))
    rc, log = ingest(out, f)
    check("HddImagePath = nodisk.chd: no such file" in log and "no Konami game partition" in log,
          f"(d) a folder with a .py1 and a .py2: both read (exit {rc})", log)

    # (e) Python 1: MAME's set alone, no .py1: the Python 1 line, not Firebeat
    rc, log = ingest(out, empty_set(os.path.join(T, "e", "popn9.zip")))
    err = errors(log)
    check(rc == 1 and len(err) == 1 and "(popn9) is a Konami Python game" in err[0] and ".py1" in err[0] and "Firebeat" not in log,
          f"(e) Python 1 set alone: exit {rc}, {err}", log)
finally:
    shutil.rmtree(T, ignore_errors=True)

print("test_ingest_python_set: " + ("all passed" if not failed else f"{failed} FAILED"))
sys.exit(1 if failed else 0)
