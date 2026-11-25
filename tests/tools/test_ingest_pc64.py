#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/ingest.py and an AVS2 PC game's program folder (contents/ with modules/<dll> and prop/): each
64-bit game is told by its game.toml's DLL and model, and its machine.txt names the loader, its family
and the folder, read where it is. Made-up folder trees only (empty DLLs, a short ea3-config.xml); no
game data.

    tests/tools/test_ingest_pc64.py      exit 0 when all hold
"""
import os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
INGEST = os.path.join(HERE, "..", "..", "tools", "ingest.py")
T = tempfile.mkdtemp(prefix="hk-pc64-")
failed = 0


def check(ok, what, log=""):
    global failed
    print(("ok    " if ok else "FAIL  ") + what)
    if not ok and log:
        print("      " + log.replace("\n", "\n      "))
    failed += not ok


def contents(top, dll, model, dest="J", ext="2000010100"):
    """a made-up contents/: modules/<dll> (empty), prop/ea3-config.xml with the soft's fields, data/"""
    c = os.path.join(top, "contents")
    for d in ("modules", "prop", "data"):
        os.makedirs(os.path.join(c, d), exist_ok=True)
    open(os.path.join(c, "modules", dll), "wb").close()
    with open(os.path.join(c, "prop", "ea3-config.xml"), "w") as f:
        f.write('<?xml version="1.0" encoding="SHIFT_JIS"?>\n<ea3><soft>'
                f'<model __type="str">{model}</model><dest __type="str">{dest}</dest>'
                f'<spec __type="str">A</spec><rev __type="str">A</rev><ext __type="str">{ext}</ext>'
                '</soft></ea3>\n')
    return c


def ingest(out, *inputs):
    p = subprocess.run([sys.executable, "-I", INGEST, "--out", out, *inputs], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def machine(out, gid):
    path = os.path.join(out, gid, "machine.txt")
    if not os.path.isfile(path):
        return {}
    return dict(l.split("=", 1) for l in open(path).read().splitlines() if "=" in l)


try:
    out = os.path.join(T, "dumps")
    # (a) the 64-bit PC beatmania IIDX's program folder, given as the folder above contents/
    a = os.path.join(T, "a", "program")
    ca = contents(a, "bm2dx.dll", "LDJ")
    rc, log = ingest(out, a)
    m = machine(out, "iidx33")
    check(rc == 0 and "OK       dumps/iidx33/machine.txt" in log, f"(a) the folder above contents/: added (exit {rc})", log)
    check(m.get("core") == "pc" and m.get("loader") == "pc64" and m.get("family") == "iidx" and m.get("exe") == "bm2dx.dll"
          and m.get("contents") == os.path.abspath(ca), f"(a) machine.txt: {m}", log)
    # ... and contents/ itself given
    shutil.rmtree(out)
    rc, log = ingest(out, ca)
    check(rc == 0 and machine(out, "iidx33").get("contents") == os.path.abspath(ca), f"(a) contents/ itself: added (exit {rc})", log)

    # (b) the 64-bit pop'n's beside it: still told by its own DLL and model
    shutil.rmtree(out)
    cb = contents(os.path.join(T, "b"), "popn.dll", "M39")
    rc, log = ingest(out, os.path.join(T, "b"))
    m = machine(out, "popn29")
    check(rc == 0 and m.get("loader") == "pc64" and m.get("family") == "pcpopnhc" and m.get("contents") == os.path.abspath(cb)
          and not os.path.exists(os.path.join(out, "iidx33")), f"(b) the 64-bit pop'n: {m}", log)

    # (c) the model without the game's DLL, or the DLL under another model: not taken for it
    shutil.rmtree(out)
    contents(os.path.join(T, "c1"), "other.dll", "LDJ")
    contents(os.path.join(T, "c2"), "bm2dx.dll", "XXX")
    for c in ("c1", "c2"):
        rc, log = ingest(out, os.path.join(T, c))
        check(not os.path.exists(os.path.join(out, "iidx33", "machine.txt")), f"({c}) not added as iidx33 (exit {rc})", log)
finally:
    shutil.rmtree(T, ignore_errors=True)

print("test_ingest_pc64: " + ("all passed" if not failed else f"{failed} FAILED"))
sys.exit(1 if failed else 0)
