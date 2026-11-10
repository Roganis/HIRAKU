#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/ingest.py --place (the launcher's "move into HIRAKU's folder" or "leave where they are"),
with a made-up PC game's folder (prog/<date>/popn16.exe and data/: the names are what count).

    tests/tools/test_ingest_place.py      exit 0 when all hold
"""
import os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
INGEST = os.path.join(HERE, "..", "..", "tools", "ingest.py")
T = tempfile.mkdtemp(prefix="hk-place-")
failed = 0


def check(ok, what):
    global failed
    print(("ok    " if ok else "FAIL  ") + what)
    failed += not ok


def pc_game(where):
    os.makedirs(os.path.join(where, "prog", "20080101"))
    os.makedirs(os.path.join(where, "data", "sd"))
    open(os.path.join(where, "prog", "20080101", "popn16.exe"), "wb").write(b"MZ")
    open(os.path.join(where, "data", "sd", "a.bin"), "wb").write(b"\0" * 64)
    return where


def machine(out):
    kv = {}
    for line in open(os.path.join(out, "popn16", "machine.txt")):
        k, _, v = line.rstrip("\n").partition("=")
        kv[k] = v
    return kv


def ingest(out, *a):
    p = subprocess.run([sys.executable, "-I", INGEST, "--out", out, *a], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


try:
    # left where it is: machine.txt names the folder given
    src = pc_game(os.path.join(T, "in", "popn16 game"))
    rc, log = ingest(os.path.join(T, "dumps1"), "--place", "stay", src)
    m = machine(os.path.join(T, "dumps1"))
    check(rc == 0 and os.path.isdir(src), f"stay: added (exit {rc}), the folder still there")
    check(m.get("data") == os.path.join(src, "data"), "stay: machine.txt reads data/ where it is")

    # moved: the folder into dumps/popn16/, read there, gone from where it was
    rc, log = ingest(os.path.join(T, "dumps2"), "--place", "move", src)
    to = os.path.join(T, "dumps2", "popn16", "popn16 game")
    m = machine(os.path.join(T, "dumps2"))
    check(rc == 0 and not os.path.exists(src), f"move: added (exit {rc}), the folder gone from where it was")
    check(os.path.exists(os.path.join(to, "data", "sd", "a.bin")), "move: its files in dumps/popn16/")
    check(m.get("program") == os.path.join(to, "prog", "20080101") and m.get("data") == os.path.join(to, "data"),
          "move: machine.txt reads it there")

    # several games moved in one go: one that fails puts back its own files, not those of the games
    # added before it (the adding itself stood in for here: what counts is whose moves are undone)
    sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
    import ingest
    a, b = os.path.join(T, "in", "a.chd"), os.path.join(T, "in", "b.chd")
    for f in (a, b):
        open(f, "wb").write(b"x")
    out3 = os.path.join(T, "dumps3")

    def add(args, inputs):
        ingest.move_file(inputs[0], os.path.join(out3, os.path.basename(inputs[0])))
        if inputs[0] != b:
            return 0
        ingest.undo_moves()
        return 1

    ingest.ingest_auto, ingest.split_by_game = add, lambda inputs: [[f] for f in inputs]
    sys.argv = ["ingest.py", "--out", out3, "--place", "move", a, b]
    rc = ingest.main()
    check(rc == 1 and os.path.exists(b) and not os.path.exists(os.path.join(out3, "b.chd")),
          "two games, the second fails: its file back where it was")
    check(os.path.exists(os.path.join(out3, "a.chd")) and not os.path.exists(a),
          "two games, the second fails: the first one's file stays moved")
finally:
    shutil.rmtree(T, ignore_errors=True)

print("test_ingest_place: " + ("all passed" if not failed else f"{failed} FAILED"))
sys.exit(1 if failed else 0)
