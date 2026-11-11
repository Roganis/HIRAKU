#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/update.py apply and recover: a package's files replaced by a newer one's, and whenever
that is stopped, the folder is put right as the old version, whole, or the new one - never a mix.
The player's own files stay through all of it. Made-up packages in a folder; every point an apply
can be killed at is tried (HK_UPDATE_STOP_AT).

    test_update_apply.py [TEST_FILE_LOCK]    exit 0 when all hold. TEST_FILE_LOCK: tests/sys's
                                             program, to check the launcher's side of the lock
"""
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UPDATE = os.path.join(ROOT, "tools", "update.py")
LINKS = sys.platform != "win32"
failed = 0


def check(ok: bool, what: str) -> None:
    global failed
    print(("ok    " if ok else "FAIL  ") + what, flush=True)
    failed += not ok


def put(folder: str, files: dict, manifest: bool = True) -> None:
    for rel, data in files.items():
        path = os.path.join(folder, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if data.startswith("-> "):
            os.symlink(data[3:], path)
        else:
            with open(path, "w", newline="") as f:
                f.write(data)
    if manifest:
        with open(os.path.join(folder, "MANIFEST.txt"), "w", newline="") as f:
            f.write("".join(n + "\n" for n in sorted(list(files) + ["MANIFEST.txt"])))


OLD = {"VERSION": "1.0\n", "bin/hiraku": "the old launcher\n", "bin/tool-old": "a tool the new version dropped\n",
       "docs/a.md": "old a\n", "docs/same.md": "the same in both\n", "games/g/game.toml": "old game\n",
       "gone/deep/x.txt": "its folder goes with it\n"}
NEW = {"VERSION": "2.0\n", "bin/hiraku": "the new launcher\n", "docs/a.md": "new a, longer\n",
       "docs/same.md": "the same in both\n", "games/g/game.toml": "new game\n", "added/sub/n.txt": "new in 2.0\n"}
MINE = {"user/settings.ini": "mine\n", "dumps/g/rom.bin": "a game\n", "games/g/mods/mine/mod.toml": "my mod\n",
        "out/shot.png": "a picture\n"}
if LINKS:
    OLD.update({"lib/libz.so.1": "old library\n", "lib/libz.so": "-> libz.so.1"})
    NEW.update({"lib/libz.so.2": "new library\n", "lib/libz.so": "-> libz.so.2"})


def fresh(tmp: str) -> tuple[str, str]:
    """an installed 1.0 with a player's files in it, and 2.0 unpacked in its update/ as the launcher has it"""
    root = os.path.join(tmp, "hiraku")
    shutil.rmtree(root, ignore_errors=True)
    put(root, OLD)
    put(root, MINE, manifest=False)
    new = os.path.join(root, "update", "new", "hiraku")
    put(new, NEW)
    return root, new


def snapshot(root: str) -> dict:
    """the folder's files, but the updater's own"""
    out = {}
    for d, dirs, files in os.walk(root):
        if d == root:
            dirs[:] = [x for x in dirs if x not in ("update", "update-old", "update-new")]
            files = [x for x in files if x not in ("update-lock", "update-journal", "update.log")]
        for name in files + [x for x in dirs if os.path.islink(os.path.join(d, x))]:
            path = os.path.join(d, name)
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            if rel == "user/update.log":
                continue
            if os.path.islink(path):
                out[rel] = "-> " + os.readlink(path)
            else:
                with open(path, newline="") as f:
                    out[rel] = f.read()
    return out


def manifest_of(files: dict) -> str:
    return "".join(n + "\n" for n in sorted(list(files) + ["MANIFEST.txt"]))


WAS = {**OLD, **MINE, "MANIFEST.txt": manifest_of(OLD)}
NOW = {**NEW, **MINE, "MANIFEST.txt": manifest_of(NEW)}


def run(*args: str, stop: int = 0, fail: int = 0) -> tuple[int, str]:
    env = {k: v for k, v in os.environ.items() if k not in ("HK_UPDATE_STOP_AT", "HK_UPDATE_FAIL_AT")}
    if stop:
        env["HK_UPDATE_STOP_AT"] = str(stop)
    if fail:
        env["HK_UPDATE_FAIL_AT"] = str(fail)
    r = subprocess.run([sys.executable, UPDATE, *args], env=env, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def left(root: str) -> list[str]:
    """what of an apply is still in the folder"""
    return [n for n in ("update-journal", "update-journal.tmp", "update-new") if os.path.lexists(os.path.join(root, n))]


def which(root: str) -> str:
    s = snapshot(root)
    return "old" if s == WAS else "new" if s == NOW else "mixed"


with tempfile.TemporaryDirectory() as tmp:
    # ---- an update that isn't stopped
    root, new = fresh(tmp)
    check(which(root) == "old", "(the made-up package, installed)")
    code, out = run("apply", root, new)
    check(code == 0 and "done" in out, "an update: done")
    check(which(root) == "new", "... the folder is the new version, and the player's files are as they were")
    check(not left(root), "... no journal, no new files waiting")
    check(not os.path.exists(os.path.join(root, "gone")), "... a folder the new version has nothing in is gone")
    old = os.path.join(root, "update-old")
    check(open(os.path.join(old, "docs", "a.md")).read() == "old a\n" and os.path.isfile(os.path.join(old, "bin", "tool-old")),
          "... the old files are in update-old")
    code, out = run("recover", root)
    check(code == 0 and "nothing was left" in out and which(root) == "new", "recover after it: nothing to do")
    code, out = run("apply", root, new)
    check(code == 0 and which(root) == "new" and not left(root), "the same update again: the same folder")

    # ---- killed at every file, then put right
    ends = {"old": 0, "new": 0}
    bad, first_new, n = [], 0, 0
    while True:
        n += 1
        root, new = fresh(tmp)
        code, out = run("apply", root, new, stop=n)
        if code != 9:
            break
        if "update-journal" not in left(root):
            bad.append(f"{n}: killed, and no journal")
        code, out = run("recover", root)
        end = which(root)
        if code != 0 or end == "mixed" or left(root):
            bad.append(f"{n}: recover gave {end} (exit {code}, left {left(root)}): {out.strip()[-200:]}")
        else:
            ends[end] += 1
            first_new = first_new or (n if end == "new" else 0)
            if ("finished" in out) != (end == "new") or ("undone" in out) != (end == "old"):
                bad.append(f"{n}: recover says the wrong thing for {end}: {out.strip()[-200:]}")
        # ... or by the update started again, as the launcher does while the download is there
        root, new = fresh(tmp)
        run("apply", root, new, stop=n)
        code, out = run("apply", root, new)
        if code != 0 or which(root) != "new" or left(root):
            bad.append(f"{n}: the update started again gave {which(root)} (exit {code}): {out.strip()[-200:]}")
    steps = n - 1
    check(code == 0 and which(root) == "new", f"(an update is {steps} files copied and moved)")
    check(not bad, f"killed at each of them, then recover: the old version whole ({ends['old']} times) or the new ({ends['new']}), never a mix"
          + "".join("\n        " + b for b in bad[:8]))
    check(ends["old"] >= len(NEW) and ends["new"] >= len(NEW), "... both while copying and while moving")

    # ---- killed again while being put right
    bad, tried = [], 0
    for n in (first_new, first_new + 3, steps):
        m = 0
        while True:
            m += 1
            root, new = fresh(tmp)
            run("apply", root, new, stop=n)
            code, out = run("recover", root, stop=m)
            if code != 9:
                break
            tried += 1
            code, out = run("recover", root)
            if code != 0 or which(root) != "new" or left(root):
                bad.append(f"{n}, then {m}: {which(root)} (exit {code}): {out.strip()[-200:]}")
    check(not bad and tried > 5, f"killed while moving, and again while that was finished ({tried} places): the new version"
          + "".join("\n        " + b for b in bad[:8]))

    # ---- a file that can't be moved: everything back
    bad, tried = [], 0
    for n in range(first_new, steps + 1):
        root, new = fresh(tmp)
        code, out = run("apply", root, new, fail=n)
        tried += 1
        if code != 1 or which(root) != "old" or left(root) or "the old version kept" not in out:
            bad.append(f"{n}: {which(root)} (exit {code}, left {left(root)}): {out.strip()[-200:]}")
    check(not bad, f"a file that can't be moved ({tried} places): the update fails and the old version is whole"
          + "".join("\n        " + b for b in bad[:8]))
    root, new = fresh(tmp)
    run("apply", root, new, fail=steps - 1)
    code, out = run("apply", root, new)
    check(code == 0 and which(root) == "new", "... and the update tried again goes in")
    # ... killed while putting things back
    bad, tried = [], 0
    for n in (first_new + 2, steps - 1):
        m = n
        while True:
            m += 1
            root, new = fresh(tmp)
            code, out = run("apply", root, new, fail=n, stop=m)
            if code != 9:
                break
            tried += 1
            code, out = run("recover", root)
            if code != 0 or which(root) != "old" or left(root):
                bad.append(f"fails at {n}, killed at {m}: {which(root)} (exit {code}): {out.strip()[-200:]}")
    check(not bad and tried > 5, f"... killed while the old files were put back ({tried} places): recover finishes putting them back"
          + "".join("\n        " + b for b in bad[:8]))

    # ---- a journal nobody can read: nothing is touched
    root, new = fresh(tmp)
    with open(os.path.join(root, "update-journal"), "w") as f:
        f.write("not a journal")
    code, out = run("apply", root, new)
    check(code == 1 and "can't be read" in out and which(root) == "old", "a journal that can't be read: no update, the folder as it was")

    # ---- a package unpacked over a folder whose update was cut short: the journal is about files no longer there
    root, new = fresh(tmp)
    run("apply", root, new, stop=first_new + 2)
    put(root, {"VERSION": "3.0\n", "bin/hiraku": "the newest launcher\n", "docs/a.md": "newest a\n", "docs/same.md": "the same in both\n",
               "games/g/game.toml": "newest game\n", "added/sub/n.txt": "new in 2.0\n"})
    before = snapshot(root)
    code, out = run("recover", root)
    check(code == 0 and "replaced since" in out and snapshot(root) == before and not left(root),
          "a newer package unpacked over an update cut short: recover leaves its files alone, and the journal goes")
    root, new = fresh(tmp)
    run("apply", root, new, stop=first_new + 2)
    os.remove(os.path.join(root, "VERSION")) if os.path.exists(os.path.join(root, "VERSION")) else None
    code, out = run("recover", root)
    check(code == 0 and "finished" in out and which(root) == "new", "... (a folder with no VERSION at that moment is still the update's: finished)")

    # ---- one at a time: the lock
    spec = importlib.util.spec_from_file_location("hk_update", UPDATE)
    update = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(update)
    root, new = fresh(tmp)
    lock_file = os.path.join(root, "update-lock")
    held = [sys.argv[1], "held", lock_file] if len(sys.argv) > 1 else None
    if held:
        check(subprocess.run(held).returncode == 1, "the launcher's question, before any update: none is running")
    lock = update.take_lock(root)
    check(lock is not None, "the lock is taken")
    if held:
        check(subprocess.run(held).returncode == 0, "... and the launcher's question says an update is running")
    p = subprocess.Popen([sys.executable, UPDATE, "apply", root, new], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(1.5)
    check(p.poll() is None and which(root) == "old" and not left(root), "an update started while another holds the folder: waits, touching nothing")
    update.let_go(lock)
    out = p.communicate(timeout=60)[0]
    check(p.returncode == 0 and which(root) == "new" and "waiting" in out, "... and goes in once the other is done")
    if held:
        check(subprocess.run(held).returncode == 1, "... after which the launcher's question says none is running")

print(f"{failed} failed" if failed else "all hold")
sys.exit(1 if failed else 0)
