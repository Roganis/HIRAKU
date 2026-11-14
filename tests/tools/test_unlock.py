#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/unlock.py, which edits a game's saved memory, against every games/*/unlocks.toml and saves
made up here (blank ones, sealed with the tool's own sums): no game's data.

For every game: listing with no save and with one; each level set and read back; each switch on
and off; only the bytes the table names change (and the sums); the save before is kept beside it;
a save whose sum is wrong, whose copies disagree or that isn't there is refused with words, not a
traceback; a value the table hasn't is refused; every progress entry's "all" and "none" end in
words too."""
import glob
import json
import os
import subprocess
import sys
import tempfile
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import unlock  # noqa: E402

failed = 0
checks = 0


def check(ok, what):
    global failed, checks
    checks += 1
    if not ok:
        failed += 1
        print("FAIL  " + what)


def run(game, nvram, *args):
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "unlock.py"), game, "--nvram", nvram, *args],
                       capture_output=True, text=True)
    check("Traceback" not in r.stderr, f"{game} {' '.join(args)}: a traceback:\n{r.stderr[-600:]}")
    return r


def listed(game, nvram):
    r = run(game, nvram, "--json")
    try:
        return json.loads(r.stdout)
    except json.JSONDecodeError:
        check(False, f"{game} --json: not JSON: {r.stdout[:200]!r} {r.stderr[-300:]!r}")
        return {}


def sum_bytes(u):
    """the save's bytes that hold a sum: they change with any change"""
    c = u.get("checksum")
    out = set()
    if not c:
        return out
    if c.get("kind") == "mirror":
        for base in c["files"]:
            out |= set(range(base, base + 4)) | set(range(base + 8, base + 12))
            if c.get("inner"):
                at = base + 0x10 + c["inner"]["at"]
                out |= set(range(at, at + 4))
    else:
        out |= {c["at"], c["at"] + 1}
    return out


def blank(u, size=0x10000):
    """a save the tool takes: zeros, with the record sizes and sums it checks"""
    data = bytearray(size)
    c = u.get("checksum")
    if c and c.get("kind") == "mirror":
        for base in c["files"]:
            data[base + 4:base + 8] = c["size"].to_bytes(4, "little")
    unlock.seal(u, data)
    return data


def differing(a, b):
    return {k for k in range(max(len(a), len(b))) if k >= len(a) or k >= len(b) or a[k] != b[k]}


def one_game(game, d):
    spec = tomllib.load(open(os.path.join(ROOT, "games", game, "unlocks.toml"), "rb"))
    unlocks = spec.get("unlock", [])
    nv = os.path.join(d, game)
    os.makedirs(nv)
    # no save yet: said, not fallen over
    state = listed(game, nv)
    check(all(u.get("current") is None for u in state.get("unlocks", [])), f"{game}: with no save, something is 'current'")
    check(run(game, nv).returncode == 0, f"{game}: listing with no save")
    for u in unlocks:
        if u.get("kind", "level") == "level" and u.get("level"):
            r = run(game, nv, f"{u['id']}={u['level'][0]['value']}")
            check(r.returncode == 1 and "no save yet" in r.stderr, f"{game} {u['id']}: setting with no save isn't refused in words")
    # blank saves, one per file the unlocks name (several unlocks may share a file: the same sums)
    files = {}
    for u in unlocks:
        files.setdefault(u["nvram"], u)
    for name, u in files.items():
        with open(os.path.join(nv, name), "wb") as f:
            f.write(blank(u))
    check(run(game, nv).returncode == 0, f"{game}: listing with a save")
    for u in unlocks:
        path = os.path.join(nv, u["nvram"])
        sums = set().union(*[sum_bytes(v) for v in unlocks if v["nvram"] == u["nvram"]])
        if u.get("kind") == "switches":
            ids = [sw["id"] for sw in u["switch"]]
            allowed = sums | {u["menu"]["at"]} | {sw["code_at"] for sw in u["switch"]} | {sw["on_at"] for sw in u["switch"] if "on_at" in sw}
            before = open(path, "rb").read()
            check(run(game, nv, f"{u['id']}=all").returncode == 0, f"{game} {u['id']}=all")
            cur = next(x for x in listed(game, nv)["unlocks"] if x["id"] == u["id"])["current"]
            check(all(cur[i]["on"] for i in ids), f"{game} {u['id']}=all: not all on: {cur}")
            check(differing(before, open(path, "rb").read()) <= allowed, f"{game} {u['id']}=all changed bytes the table doesn't name")
            check(run(game, nv, f"{u['id']}=none").returncode == 0, f"{game} {u['id']}=none")
            cur = next(x for x in listed(game, nv)["unlocks"] if x["id"] == u["id"])["current"]
            check(not any(cur[i]["on"] for i in ids), f"{game} {u['id']}=none: some still on: {cur}")
            check(run(game, nv, f"{ids[0]}=1").returncode == 0, f"{game} {ids[0]}=1")
            cur = next(x for x in listed(game, nv)["unlocks"] if x["id"] == u["id"])["current"]
            check(cur[ids[0]]["on"] and not any(cur[i]["on"] for i in ids[1:]), f"{game} {ids[0]}=1: {cur}")
            continue
        if not u.get("level"):
            continue
        allowed = sums | set(u["offsets"])
        for lv in u["level"] + u["level"][:1]:   # every level, and back to the first
            before = open(path, "rb").read()
            n_backups = len(glob.glob(path + ".*.bak"))
            r = run(game, nv, f"{u['id']}={lv['value']}")
            check(r.returncode == 0, f"{game} {u['id']}={lv['value']}: {r.stderr.strip()[-200:]}")
            after = open(path, "rb").read()
            cur = next((x for x in listed(game, nv).get("unlocks", []) if x["id"] == u["id"]), {}).get("current")
            check(cur == lv["value"], f"{game} {u['id']}={lv['value']}: read back {cur}")
            check(len(after) == len(before) and differing(before, after) <= allowed,
                  f"{game} {u['id']}={lv['value']}: changed bytes the table doesn't name: {sorted(differing(before, after) - allowed)[:8]}")
            backups = sorted(glob.glob(path + ".*.bak"), key=os.path.getmtime)
            check(len(backups) == n_backups + 1 and open(backups[-1], "rb").read() == before, f"{game} {u['id']}={lv['value']}: the save before isn't kept")
            check(unlock.checksum_ok(u, after), f"{game} {u['id']}={lv['value']}: the save's sum is wrong afterwards")
        # a value the table hasn't
        r = run(game, nv, f"{u['id']}=250")
        if 250 not in [lv["value"] for lv in u["level"]]:
            check(r.returncode == 1 and "not one of" in r.stderr, f"{game} {u['id']}=250 isn't refused")
        good = open(path, "rb").read()
        # its copies disagree
        if len(u["offsets"]) > 1:
            bad = bytearray(good)
            bad[u["offsets"][0]] ^= 0x55
            unlock.seal(u, bad)
            open(path, "wb").write(bad)
            r = run(game, nv, f"{u['id']}={u['level'][0]['value']}")
            check(r.returncode == 1 and "disagree" in r.stderr, f"{game} {u['id']}: copies that disagree aren't refused")
            check(open(path, "rb").read() == bytes(bad), f"{game} {u['id']}: a refused save was changed")
        # its sum is wrong
        c = u.get("checksum")
        if c:
            bad = bytearray(good)
            at = c["files"][0] + 0x10 + 1 if c.get("kind") == "mirror" else c["from"]
            if at in u["offsets"]:
                at += 1
            bad[at] ^= 0x01
            open(path, "wb").write(bad)
            r = run(game, nv, f"{u['id']}={u['level'][0]['value']}")
            check(r.returncode == 1 and "checksum" in r.stderr, f"{game} {u['id']}: a save with a wrong sum isn't refused")
            check(open(path, "rb").read() == bytes(bad), f"{game} {u['id']}: a refused save was changed")
        open(path, "wb").write(good)
    # the progress entries: read and set on a blank save end in words (run() looks for a traceback)
    for p in spec.get("progress", []):
        for v in ("all", "none"):
            run(game, nv, f"{p['id']}={v}")
    listed(game, nv)
    run(game, nv)
    r = run(game, nv, "no_such_unlock=1")
    check(r.returncode == 1 and "expected ID=VALUE" in r.stderr, f"{game}: an unlock it hasn't isn't refused")


def main():
    games = sorted(os.path.basename(os.path.dirname(p)) for p in glob.glob(os.path.join(ROOT, "games", "*", "unlocks.toml")))
    check(len(games) >= 10, f"only {len(games)} games with unlocks.toml")
    with tempfile.TemporaryDirectory() as d:
        for g in games:
            one_game(g, d)
        r = run("no-such-game", d)
        check(r.returncode == 1 and "no unlocks known" in r.stderr, "a game with no unlocks.toml isn't said so")
    print(f"{len(games)} games, {checks} checks, " + (f"{failed} failed" if failed else "all hold"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
