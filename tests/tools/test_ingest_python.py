#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/ingest.py and the Konami Python games' BIOS and keys (docs/PLAY.md): a game added without
them waits (machine.pending); the files dropped later, loose or in their folders, are kept for every
Python game and finish it. A keys folder counts only whole: every file the board needs, each of its
size (a half set boots to the BIOS's "Boot Program Error"). Made-up files only (the names and sizes
are what count, the bytes are zeros), no game data.

    tests/tools/test_ingest_python.py      exit 0 when all hold
"""
import json, os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
INGEST = os.path.join(HERE, "..", "..", "tools", "ingest.py")
T = tempfile.mkdtemp(prefix="hk-ingest-")
OUT = os.path.join(T, "dumps")
failed = 0


def check(ok, what):
    global failed
    print(("ok    " if ok else "FAIL  ") + what)
    failed += not ok


def touch(path, data=b"\0" * 16):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


SIZES = {"civ": 8, "cks": 96, "eks": 1024, "kek": 16, "arcade_kelf_override_kbit": 16, "arcade_kelf_override_kc": 16}


def keys(folder, skip=(), sizes=None):
    """a made-up keys folder: SIZES' files of zeros, but those in skip, sizes overriding"""
    for k, n in SIZES.items():
        if k not in skip:
            touch(os.path.join(folder, k + ".bin"), b"\0" * (sizes or {}).get(k, n))


def waiting(game, board, name):
    d = os.path.join(OUT, game)
    for sub in ("bios", "keys"):
        os.makedirs(os.path.join(d, sub), exist_ok=True)
    machine = {"name": name, "core": "python", "disk": "disk.img"}
    with open(os.path.join(d, "machine.pending"), "w") as f:
        json.dump({"machine": machine, "order": ["name", "core", "disk", "bios", "keys"], "board": board}, f)
    return d


def ingest(*inputs):
    p = subprocess.run([sys.executable, "-I", INGEST, "--out", OUT, *inputs], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


try:
    p1 = waiting("popn12", 1, "pop'n music 12")
    p2 = waiting("drmnvj", 2, "DrumMania V")
    drop = os.path.join(T, "drop")
    for ext in ("BIN", "NVM", "MEC"):
        touch(os.path.join(drop, "bios", "COH-H31000." + ext))
    keys(os.path.join(drop, "keys"))

    # the keys' folder alone: kept; both games still lack a BIOS
    rc, log = ingest(os.path.join(drop, "keys"))
    check(rc != 0, f"keys alone: the games still wait (exit {rc})")
    check("KEPT" in log and os.path.exists(os.path.join(OUT, "_python", "keys", "kek.bin")), "the keys kept in dumps/_python/keys")
    check(os.path.exists(os.path.join(p1, "machine.pending")), "pop'n 12 still waiting")
    check("needs the arcade BIOS" in log, "it says pop'n 12 needs the arcade BIOS")

    # Python 1's BIOS, loose files: pop'n 12 is finished; DrumMania V (Python 2's BIOS) still waits
    rc, log = ingest(*[os.path.join(drop, "bios", "COH-H31000." + e) for e in ("BIN", "NVM", "MEC")])
    check(not os.path.exists(os.path.join(p1, "machine.pending")), "pop'n 12 no longer waiting")
    mt = open(os.path.join(p1, "machine.txt")).read() if os.path.exists(os.path.join(p1, "machine.txt")) else ""
    check("bios=bios/COH-H31000.BIN\n" in mt and "keys=keys\n" in mt, "pop'n 12's machine.txt names its BIOS and keys")
    check(all(os.path.exists(os.path.join(p1, "bios", "COH-H31000." + e)) for e in ("BIN", "NVM", "MEC")),
          "pop'n 12's BIOS copied with its .NVM and .MEC")
    check(len(os.listdir(os.path.join(p1, "keys"))) == 6, "pop'n 12's six key files copied")
    check(os.path.exists(os.path.join(p2, "machine.pending")) and "ps2-0190j" in log,
          "DrumMania V still waits, for ps2-0190j")
    check(rc != 0, f"exit {rc}: a game still waits")

    # Python 2's BIOS: DrumMania V finished with the kept keys (its four)
    touch(os.path.join(drop, "ps2-0190j-20030822.bin"))
    rc, log = ingest(os.path.join(drop, "ps2-0190j-20030822.bin"))
    check(rc == 0, f"Python 2's BIOS: exit {rc}")
    check(not os.path.exists(os.path.join(p2, "machine.pending")) and os.path.exists(os.path.join(p2, "machine.txt")),
          "DrumMania V added")
    check(sorted(os.listdir(os.path.join(p2, "keys"))) == ["civ.bin", "cks.bin", "eks.bin", "kek.bin"],
          "DrumMania V's four key files copied")

    # The keys folder's check, on its own (python_keys): which folder a game takes, and what is wrong
    import importlib.util, types
    spec = importlib.util.spec_from_file_location("ingest", INGEST)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    K = os.path.join(T, "k")
    whole, short, half, wrong = (os.path.join(K, n, "keys") for n in ("whole", "short", "half", "wrong"))
    keys(whole)
    keys(short, skip=("arcade_kelf_override_kc",))
    keys(half, skip=("arcade_kelf_override_kbit", "arcade_kelf_override_kc"))
    keys(wrong, sizes={"kek": 12})
    empty = os.path.join(T, "k-out")
    os.makedirs(empty)

    def pick(given, board=1, near=os.path.join(T, "nowhere")):
        return mod.python_keys(types.SimpleNamespace(keys=given, out=empty), near, board)

    check(pick(whole) == (whole, []), "a whole keys folder taken")
    check(pick(short) == (short, ["arcade_kelf_override_kc.bin is missing"]), "one file missing: said by name")
    check(pick(wrong) == (wrong, ["kek.bin is 12 bytes, not 16"]), "one file of the wrong size: said with both sizes")
    check(pick(half) == (half, ["arcade_kelf_override_kbit.bin is missing", "arcade_kelf_override_kc.bin is missing"]),
          "Python 1 without the two arcade KELF overrides: not whole")
    check(pick(half, board=2) == (half, []), "Python 2 without them: whole (it needs four)")
    check(pick(None) == (None, [k + ".bin is missing" for k in SIZES]), "no keys anywhere: every file named")
    # several places: a whole folder after an incomplete one is taken (--keys given, then one near the set)
    near = os.path.join(T, "set")
    keys(os.path.join(near, "keys"))
    check(pick(short, near=near) == (os.path.join(near, "keys"), []), "a whole folder near the set over an incomplete --keys")
    os.makedirs(os.path.join(T, "set2", "keys"))   # (an empty keys folder near the set: the given one is closer to whole)
    check(pick(wrong, near=os.path.join(T, "set2")) == (wrong, ["kek.bin is 12 bytes, not 16"]),
          "two incomplete folders: the one closer to whole said")

    # A game waiting with a half set of its own: a key file of the wrong size dropped is refused (not
    # over a good one kept), and the game keeps waiting, saying which files; the whole set finishes it
    p3 = waiting("popn14", 1, "pop'n music 14")
    for d in (os.path.join(OUT, "_python", "keys"), os.path.join(p1, "keys")):   # (no whole set anywhere else)
        shutil.rmtree(d)
    keys(os.path.join(p3, "keys"), skip=("arcade_kelf_override_kbit", "arcade_kelf_override_kc"))
    bad = os.path.join(T, "bad")
    keys(bad, skip=("civ", "cks", "eks", "kek", "arcade_kelf_override_kbit"), sizes={"arcade_kelf_override_kc": 0})
    rc, log = ingest(os.path.join(bad, "arcade_kelf_override_kc.bin"))
    check(rc != 0 and "arcade_kelf_override_kc.bin is 0 bytes, not 16" in log and "not kept" in log,
          f"a 0-byte key file refused (exit {rc})")
    check(not os.path.exists(os.path.join(OUT, "_python", "keys", "arcade_kelf_override_kc.bin")), "and not kept")
    check(os.path.exists(os.path.join(p3, "machine.pending")) and not os.path.exists(os.path.join(p3, "machine.txt")),
          "pop'n 14 still waits")
    check("needs all 6 MagicGate key files" in log and "arcade_kelf_override_kbit.bin is missing" in log,
          "it says which of pop'n 14's key files are missing")
    rc, log = ingest(whole)
    check(rc == 0 and os.path.exists(os.path.join(p3, "machine.txt")) and not os.path.exists(os.path.join(p3, "machine.pending")),
          f"the whole keys folder dropped: pop'n 14 added (exit {rc})")
    check(mod.key_problems(os.path.join(p3, "keys"), mod.PY_KEYS) == [], "pop'n 14's own keys folder now whole")
finally:
    shutil.rmtree(T, ignore_errors=True)

print("test_ingest_python: " + ("all passed" if not failed else f"{failed} FAILED"))
sys.exit(1 if failed else 0)
