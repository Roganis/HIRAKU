#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""hiraku-recomp: a function marked nopoll has no polls, but still asks at its entry whether a mod
changed its code, as the executor does for every function (a patch that works in hiraku-firebeat
must not be skipped in a game's own binary). And files whose content didn't change aren't written
again, so the build doesn't compile a game's code again for nothing. A made-up program, no game's.

    test_recomp_nopoll.py HIRAKU_RECOMP [CC INCLUDE_DIR]    exit 0 when all hold
"""
import os
import re
import struct
import subprocess
import sys
import tempfile

failed = 0


def check(ok: bool, what: str) -> None:
    global failed
    print(("ok    " if ok else "FAIL  ") + what, flush=True)
    failed += not ok


def body(code: str, name: str) -> str:
    m = re.search(r"void %s\(hk_ctx \*c\) \{\n(.*?)\n\}\n" % name, code, re.S)
    return m.group(1) if m else ""


# two functions, each a counted loop: li r3,4; mtctr r3; addi r4,r4,1; bdnz -4; blr
LOOP = [0x38600004, 0x7C6903A6, 0x38840001, 0x4200FFFC, 0x4E800020]
with tempfile.TemporaryDirectory() as tmp:
    image = os.path.join(tmp, "program.bin")
    with open(image, "wb") as f:
        f.write(b"".join(struct.pack(">I", w) for w in LOOP + LOOP))
    spec = os.path.join(tmp, "recomp.txt")
    with open(spec, "w") as f:
        f.write(f"image {image}\nbase 1000\nfunc 1000\nrange 1000 1014\nend\nfunc 1014 nopoll\nrange 1014 1028\nend\n")
    out = os.path.join(tmp, "gen")
    os.mkdir(out)
    r = subprocess.run([sys.argv[1], spec, out, "--files", "1"], capture_output=True, text=True)
    check(r.returncode == 0 and "(3 changed)" in r.stdout, "the made-up program is recompiled: " + (r.stdout + r.stderr).strip())
    with open(os.path.join(out, "code_000.c")) as f:
        code = f.read()
    plain, quiet = body(code, "f_00001000"), body(code, "f_00001014")
    check(plain.count("HK_POLL(c, 0x00001000u") == 1 and plain.count("HK_POLL(") == 2, "a function: a poll at its entry and one at its loop")
    check("hk_patched(" not in plain, "... (the poll is what asks about a mod's patch there)")
    check(quiet and "HK_POLL(" not in quiet, "a nopoll function: no polls")
    first = quiet.strip().splitlines()[0] if quiet else ""
    check("hk_patched_any" in first and "hk_patched(0x00001014u)" in first and "hk_interp_function(c, 0x00001014u); return;" in first,
          "... and its first line asks whether a mod changed it, and runs it interpreted if so")
    if len(sys.argv) > 3:
        r = subprocess.run([sys.argv[2], "-fsyntax-only", "-w", "-I", sys.argv[3], "-I", out, os.path.join(out, "code_000.c"),
                            os.path.join(out, "table.c")], capture_output=True, text=True)
        check(r.returncode == 0, "... the C it wrote compiles" + ("" if r.returncode == 0 else ": " + r.stderr.strip()[:300]))
    # the same again: nothing rewritten
    times = {n: os.stat(os.path.join(out, n)).st_mtime_ns for n in os.listdir(out)}
    os.utime(os.path.join(out, "table.c"), ns=(1, 1))   # (a time a rewrite would change, whatever the clock's step)
    times["table.c"] = 1
    r = subprocess.run([sys.argv[1], spec, out, "--files", "1"], capture_output=True, text=True)
    check(r.returncode == 0 and "(0 changed)" in r.stdout and times == {n: os.stat(os.path.join(out, n)).st_mtime_ns for n in os.listdir(out)},
          "recompiled again with nothing changed: no file is written again")
    with open(spec, "a") as f:
        f.write("func 1010\nrange 1010 1014\nend\n")
    r = subprocess.run([sys.argv[1], spec, out, "--files", "1"], capture_output=True, text=True)
    check(r.returncode == 0 and "(3 changed)" in r.stdout and os.stat(os.path.join(out, "table.c")).st_mtime_ns != 1,
          "... and with a function more, they are")

print(f"{failed} failed" if failed else "all hold")
sys.exit(1 if failed else 0)
