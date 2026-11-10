#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/ingest.py's walk of a dropped folder: links are followed (a dump's folders are often links
to another disk), but not back up the tree, not twice, and not out through a Wine prefix's drives.

    tests/tools/test_ingest_walk.py      exit 0 when all hold
"""
import importlib.util, os, shutil, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("ingest", os.path.join(HERE, "..", "..", "tools", "ingest.py"))
ingest = importlib.util.module_from_spec(spec)
sys.modules["ingest"] = ingest
spec.loader.exec_module(ingest)
T = tempfile.mkdtemp(prefix="hk-walk-")
failed = 0


def check(ok, what):
    global failed
    print(("ok    " if ok else "FAIL  ") + what)
    failed += not ok


def put(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(b"x")


try:
    drop, elsewhere, big = os.path.join(T, "drop"), os.path.join(T, "elsewhere"), os.path.join(T, "big")
    put(os.path.join(drop, "game", "disc.cue"))
    put(os.path.join(elsewhere, "data", "disc2.chd"))
    put(os.path.join(big, "not-the-game.chd"))
    os.symlink(os.path.join(elsewhere, "data"), os.path.join(drop, "game", "data"))    # a dump's folder on another disk
    os.symlink(os.path.join(elsewhere, "data"), os.path.join(drop, "game", "again"))   # the same folder a second time
    os.symlink(drop, os.path.join(drop, "game", "back"))                               # a loop
    os.symlink("/", os.path.join(drop, "game", "root"))                                # the whole disk
    # a Wine prefix left in the dump: its drives are links to whole disks
    put(os.path.join(drop, "pfx", "system.reg"))
    put(os.path.join(drop, "pfx", "drive_c", "game.iso"))
    os.makedirs(os.path.join(drop, "pfx", "dosdevices"))
    os.symlink("../drive_c", os.path.join(drop, "pfx", "dosdevices", "c:"))
    os.symlink("/", os.path.join(drop, "pfx", "dosdevices", "z:"))
    os.symlink(big, os.path.join(drop, "pfx", "dosdevices", "d:"))
    found, folders = set(), 0
    for base, _, names in ingest.walk_links(drop):
        folders += 1
        if folders > 200:
            break
        found |= {os.path.relpath(os.path.join(base, n), drop) for n in names}
    check(folders <= 200, f"the walk ends ({folders} folders)")
    check("game/disc.cue" in found and "pfx/drive_c/game.iso" in found, "the folder's own files are found")
    check(("game/data/disc2.chd" in found) != ("game/again/disc2.chd" in found), "a link to a folder elsewhere is followed, once")
    check(not any(f.startswith(("game/back", "game/root")) for f in found), "a link back up, or to the whole disk, is not")
    check(not any("dosdevices" in f or "not-the-game" in f for f in found), "nor a Wine prefix's drives")
    files = ingest.gather([drop], set())
    check(sorted(os.path.relpath(f, drop) for f in files if not os.path.isdir(f))[:1] != [] and
          any(f.endswith("disc2.chd") for f in files) and not any("not-the-game" in f for f in files), "gather() takes the files it walks")
finally:
    shutil.rmtree(T, ignore_errors=True)
print(f"{failed} failed" if failed else "all hold")
sys.exit(1 if failed else 0)
