#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/ingest.py and the ee'MALL station's drive (games/eemall: the C33 cabinet): its program in
C33/JAx/ by revision, its data the C33 folder itself (no data/ of its own). Given as the station's
backup (an extracted C: beside D/C33), as D, or as C33: machine.txt names the newest revision's
program, C33 as the data, the drives and the board. Moved into dumps/: the C33 folder goes. A folder
whose program lacks the station's mark is not taken. Made-up folder trees only (a small file with
the mark in it as the program); no game data.

    tests/tools/test_ingest_eemall.py      exit 0 when all hold
"""
import os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
INGEST = os.path.join(HERE, "..", "..", "tools", "ingest.py")
T = tempfile.mkdtemp(prefix="hk-eemall-")
failed = 0


def check(ok, what, log=""):
    global failed
    print(("ok    " if ok else "FAIL  ") + what)
    if not ok and log:
        print("      " + log.replace("\n", "\n      "))
    failed += not ok


def station(top, mark=b"GQC33", revisions=("JAD", "JAE")):
    """a made-up backup: C/ (a few folders, as an extracted system drive) and D/C33 with its revisions"""
    for d in ("C/WINDOWS/system32", "C/Program Files", "D/C33/resource/movie", "D/C33/boot"):
        os.makedirs(os.path.join(top, d), exist_ok=True)
    open(os.path.join(top, "D", "C33", "C33.csv"), "w").close()
    for r in revisions:
        os.makedirs(os.path.join(top, "D", "C33", r), exist_ok=True)
        with open(os.path.join(top, "D", "C33", r, "eemall.exe"), "wb") as f:
            f.write(b"MZ" + b"\0" * 62 + mark + b" made up\0")
    return os.path.join(top, "D", "C33")


def ingest(out, *args):
    p = subprocess.run([sys.executable, "-I", INGEST, "--out", out, *args], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def machine(out, gid="eemall"):
    path = os.path.join(out, gid, "machine.txt")
    if not os.path.isfile(path):
        return {}
    return dict(l.split("=", 1) for l in open(path).read().splitlines() if "=" in l)


try:
    out = os.path.join(T, "dumps")
    top = os.path.join(T, "backup")
    c33 = station(top)
    want = {"core": "pc", "family": "eemall", "exe": "eemall.exe", "program": os.path.join(c33, "JAE"), "data": c33,
            "drive": "D:\\C33", "save_drive": "E:\\", "save_drive2": "F:\\", "data_as": "D:\\C33", "board": "c33"}
    # (a) the whole backup (C beside D), (b) D, (c) C33 itself: the same machine.txt, read where it is
    for name, given in (("a", top), ("b", os.path.join(top, "D")), ("c", c33)):
        shutil.rmtree(out, ignore_errors=True)
        rc, log = ingest(out, given)
        m = machine(out)
        check(rc == 0 and "OK       dumps/eemall/machine.txt" in log, f"({name}) {os.path.relpath(given, T)}: added (exit {rc})", log)
        check(m == want, f"({name}) machine.txt: the newest revision, C33 the data, the drives and board", f"{m}\n{log}")
        check(os.path.isfile(os.path.join(c33, "JAE", "eemall.exe")), f"({name}) the folder left where it is")

    # (d) moved into dumps/: the C33 folder goes there, machine.txt names it there
    shutil.rmtree(out, ignore_errors=True)
    rc, log = ingest(out, "--place", "move", top)
    m = machine(out)
    moved = os.path.join(out, "eemall", "C33")
    check(rc == 0 and m.get("data") == moved and m.get("program") == os.path.join(moved, "JAE")
          and os.path.isfile(os.path.join(moved, "JAE", "eemall.exe")) and not os.path.exists(c33),
          f"(d) moved: C33 into dumps/eemall/ ({m.get('data')})", log)

    # (e) a program of that name without the station's mark: not taken for it
    shutil.rmtree(out, ignore_errors=True)
    other = os.path.join(T, "other")
    station(other, mark=b"SOMETHING ELSE")
    rc, log = ingest(out, other)
    check(not machine(out), f"(e) another program named eemall.exe: not added (exit {rc})", log)

    # (f) a copy of a revision someone changed, kept beside under a name of its own (last by name):
    # the revisions named as Konami's are preferred; alone, it is taken, with a note
    shutil.rmtree(out, ignore_errors=True)
    beside = os.path.join(T, "beside")
    c33b = station(beside, revisions=("JAD", "JAE", "JAEC"))
    rc, log = ingest(out, beside)
    check(rc == 0 and machine(out).get("program") == os.path.join(c33b, "JAE") and "JAEC: not a revision" in log,
          "(f) JAEC beside JAE: JAE is run, a note names the other", log)
    shutil.rmtree(out, ignore_errors=True)
    alone = os.path.join(T, "alone")
    c33a = station(alone, revisions=("JAEC",))
    rc, log = ingest(out, alone)
    check(rc == 0 and machine(out).get("program") == os.path.join(c33a, "JAEC") and "may not start" in log,
          "(f) JAEC alone: taken, with a note", log)
finally:
    shutil.rmtree(T, ignore_errors=True)

print("test_ingest_eemall: " + ("all passed" if not failed else f"{failed} FAILED"))
sys.exit(1 if failed else 0)
