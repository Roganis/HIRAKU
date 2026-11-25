#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/ingest.py and a ROM set given as a folder (a MAME rompath folder, the set's files loose):
its .bin ROMs are copied (DDR 2ndMIX's h8a01.bin, Keyboardmania's synth program were left out as
disc images once), a disc's .bin (named by a .cue beside it, or of a disc's size), CHDs and logs
are not. Made-up files only; no game data.

    tests/tools/test_ingest_romdir.py      exit 0 when all hold
"""
import os, shutil, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import ingest  # noqa: E402

T = tempfile.mkdtemp(prefix="hk-romdir-")
failed = 0


def check(ok, what, log=""):
    global failed
    print(("ok    " if ok else "FAIL  ") + what)
    if not ok and log:
        print("      " + str(log))
    failed += not ok


try:
    s = os.path.join(T, "set")
    os.makedirs(s)
    for n, size in (("h8a01.bin", 4096), ("rom.27", 64), ("Synth.BIN", 512), ("disc.bin", 2352 * 4),
                    ("track02.bin", 2352), ("big.bin", 0), ("game.chd", 8), ("x.log", 3), ("notes.txt", 3)):
        with open(os.path.join(s, n), "wb") as f:
            f.write(b"\0" * size)
    os.truncate(os.path.join(s, "big.bin"), 65 << 20)        # sparse: a disc's size, no disk used
    with open(os.path.join(s, "disc.cue"), "w") as f:
        f.write('FILE "disc.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n'
                'FILE Track02.bin BINARY\n  TRACK 02 AUDIO\n    INDEX 01 00:00:00\n')
    out = os.path.join(T, "rom")
    names = sorted(ingest.copy_romset(s, out))
    check(names == ["Synth.BIN", "h8a01.bin", "rom.27"], "the ROMs copied, .bin ROMs among them", names)
    check(sorted(os.listdir(out)) == names, "the disc's files, the CHD and the logs left out", sorted(os.listdir(out)))
finally:
    shutil.rmtree(T, ignore_errors=True)

print("test_ingest_romdir: " + ("all passed" if not failed else f"{failed} FAILED"))
sys.exit(1 if failed else 0)
