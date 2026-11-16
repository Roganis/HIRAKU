#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A game's unlocks (games/<id>/unlocks.toml), read from and written to its saved NVRAM.

    tools/unlock.py GAME                     what can be unlocked, and what the save has
    tools/unlock.py GAME ID=VALUE...         set it (e.g. popn8 phase=4); the save is backed up first
                                             switches: SWITCH=1/0 (e.g. popn6 secret_music=1),
                                             or ID=all / ID=none for all of them
    tools/unlock.py GAME --json              the same, for the launcher

  --nvram DIR   the save (default: ~/.local/share/hiraku/GAME, as the game window uses; in a
                portable install, user/data/GAME)

The game must not be running: it writes its NVRAM back when it exits. An unlock is the bytes
the game itself writes when an operator enters the code (unlocks.toml says how each was found).
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import time
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Error(Exception):
    pass


def load(game: str) -> dict:
    path = os.path.join(ROOT, "games", game, "unlocks.toml")
    if not os.path.exists(path):
        raise Error(f"{game}: no unlocks known (no games/{game}/unlocks.toml)")
    with open(path, "rb") as f:
        return tomllib.load(f)


def default_nvram(game: str) -> str:
    # as runtime/shell/userdir.h: a portable install (a HIRAKU folder holding user/) keeps its saves in there
    if os.path.isdir(os.path.join(ROOT, "user")):
        return os.path.join(ROOT, "user", "data", game)
    if os.name == "nt" and os.environ.get("LOCALAPPDATA"):
        return os.path.join(os.environ["LOCALAPPDATA"], "hiraku", game)
    base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    return os.path.join(base, "hiraku", game)


def backup_name(path: str) -> str:
    """Where the old save goes: path.YYYYmmdd-HHMMSS.bak, numbered when that exists already."""
    base = f"{path}.{time.strftime('%Y%m%d-%H%M%S')}"
    name, k = base + ".bak", 2
    while os.path.exists(name):
        name, k = f"{base}-{k}.bak", k + 1
    return name


def mirror_sum(b: bytes) -> int:
    """Konami Python's BBSRAM file sum (mirror.c, pop'n 9 0x1ab6f8): little-endian words, each XORed with its index."""
    return sum(int.from_bytes(b[k:k + 4], "little") ^ (k // 4) for k in range(0, len(b) - 3, 4)) & 0xffffffff


def record_sum(c: dict, rec: bytes) -> int:
    """A record's own sum (checksum.inner): its segments' bytes, each XORed with its index in the
    segment, plus a constant (pop'n 9's settings: 0x1a9b28). A segment is a length (it follows the
    previous one) or [start, length] (pop'n 10's skip bytes 4-7)."""
    s, o = c["add"], 0
    for seg in c["segments"]:
        if isinstance(seg, list):
            o, n = seg
        else:
            n = seg
        s += sum(rec[o + k] ^ k for k in range(n))
        o += n
    return s & 0xffffffff


def checksum_ok(u: dict, data: bytes) -> bool:
    c = u.get("checksum")
    if not c:
        return True
    if c.get("kind") == "mirror":
        # each copy: a header {sum of the next two words, data size, data sum}, the data at +0x10
        for base in c["files"]:
            size = int.from_bytes(data[base + 4:base + 8], "little")
            rec = data[base + 0x10:base + 0x10 + size]
            if (size != c["size"] or mirror_sum(data[base + 4:base + 12]) != int.from_bytes(data[base:base + 4], "little")
                    or mirror_sum(rec) != int.from_bytes(data[base + 8:base + 12], "little")):
                return False
            i = c.get("inner")
            if i and record_sum(i, rec) != int.from_bytes(rec[i["at"]:i["at"] + 4], "little"):
                return False
        return True
    return sum(data[c["from"]:c["to"]]) & 0xffff == int.from_bytes(data[c["at"]:c["at"] + 2], "big")


def seal(u: dict, data: bytearray):
    """After a change: the checksums, as the game writes them."""
    c = u.get("checksum")
    if not c:
        return
    if c.get("kind") == "mirror":
        for base in c["files"]:
            rec = base + 0x10
            i = c.get("inner")
            if i:
                data[rec + i["at"]:rec + i["at"] + 4] = record_sum(i, data[rec:rec + c["size"]]).to_bytes(4, "little")
            data[base + 8:base + 12] = mirror_sum(data[rec:rec + c["size"]]).to_bytes(4, "little")
            data[base:base + 4] = mirror_sum(data[base + 4:base + 12]).to_bytes(4, "little")
    else:   # a 16-bit sum of the bytes from..to, kept at `at` (big-endian)
        data[c["at"]:c["at"] + 2] = (sum(data[c["from"]:c["to"]]) & 0xffff).to_bytes(2, "big")


def switches(u: dict, nvram: str):
    """kind = "switches": {switch id: {"code": entered, "on": switched on}}, or None (no save)."""
    path = os.path.join(nvram, u["nvram"])
    if not os.path.exists(path):
        return None
    data = open(path, "rb").read()
    if not checksum_ok(u, data):
        raise Error(f"{path}: its checksum doesn't match: not a save this tool knows")
    return {sw["id"]: {"code": bool(data[sw["code_at"]]) and bool(data[u["menu"]["at"]]),
                       "on": bool(data[sw["on_at"]]) if "on_at" in sw else bool(data[sw["code_at"]])}
            for sw in u["switch"]}


def apply_switches(game: str, u: dict, nvram: str, changes: dict) -> str:
    """Set switches on (their menu, code and switch bytes, as the game does) or off."""
    path = os.path.join(nvram, u["nvram"])
    if not os.path.exists(path):
        raise Error(f"{path}: no save yet. Start {game} once (with its first-boot setup), quit it, then unlock.")
    switches(u, nvram)   # refuses a save it doesn't understand
    data = bytearray(open(path, "rb").read())
    backup = backup_name(path)
    shutil.copy2(path, backup)
    for sw in u["switch"]:
        if sw["id"] not in changes:
            continue
        if changes[sw["id"]]:
            data[u["menu"]["at"]] = 1
            data[sw["code_at"]] = 1
            if "on_at" in sw:
                data[sw["on_at"]] = 1
        elif "on_at" in sw:
            data[sw["on_at"]] = 0     # switched off; the code stays entered, as in the game
        else:
            data[sw["code_at"]] = 0
    seal(u, data)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)
    return backup


def current(u: dict, nvram: str):
    """The value in the save, or None (no save yet); Error if the copies disagree."""
    path = os.path.join(nvram, u["nvram"])
    if not os.path.exists(path):
        return None
    data = open(path, "rb").read()
    vals = {data[o] for o in u["offsets"] if o < len(data)}
    if len(vals) != 1:
        raise Error(f"{path}: the copies of {u['id']} disagree ({sorted(vals)}): not a save this tool knows")
    if not checksum_ok(u, data):
        raise Error(f"{path}: its checksum doesn't match: not a save this tool knows")
    return vals.pop()


def progress(p: dict, nvram: str, unlocks: dict) -> dict | None:
    """An event's state from the save (games/<id>/unlocks.toml [[progress]]), or None (no save)."""
    if p.get("kind") == "tickets":
        return tickets(p, nvram)
    path = os.path.join(nvram, p["nvram"])
    if not os.path.exists(path):
        return None
    data = open(path, "rb").read()
    def field(off):
        vals = {int.from_bytes(data[c + off:c + off + 4], "big") for c in p.get("copies", [0])}
        if len(vals) != 1:
            raise Error(f"{path}: the copies of {p['id']} disagree: not a save this tool knows")
        return vals.pop()
    def matches(cond):
        return cond is not None and (current(unlocks[cond["unlock"]], nvram) & cond.get("mask", 0xffffffff)) in cond["values"]
    active = p.get("active") is None or matches(p.get("active"))
    if p.get("kind") == "bingo":
        return bingo(p, data, active, matches(p.get("released")))
    if p.get("kind") == "gamblerz":
        return gamblerz(p, data, active, matches(p.get("released")))
    if p.get("kind") == "wishes":
        return wishes(p, data, active, matches(p.get("released")))
    if p.get("kind") == "eventmask":
        return eventmask(p, data, active, matches(p.get("released")))
    if p.get("kind") == "points":
        return points(p, data, active, matches(p.get("released")))
    if p.get("kind") == "thresholds":
        return thresholds(p, data, unlocks, nvram)
    stage, unl, missed, pos, lap = (field(p[k]) for k in ("stage", "unlocked", "missed", "position", "lap"))
    st = p["stages"][min(stage, len(p["stages"]) - 1)]
    goal = st["goal"]
    here = [c for c in p["cities"] if c["stages"] >> stage & 1]
    ahead = sorted(here, key=lambda c: (c["space"] - pos - 1) % p["spaces"])
    nxt = ahead[0] if ahead else None
    return {
        "active": active, "stage": stage + 1, "stages": len(p["stages"]), "stage_name": st["name"],
        "unlocked": bin(unl & goal).count("1"), "goal": bin(goal).count("1"),
        "position": pos, "spaces": p["spaces"], "lap": lap,
        "next": nxt and nxt["name"], "next_romaji": nxt and nxt["romaji"],
        "next_in": nxt and (nxt["space"] - pos - 1) % p["spaces"] + 1,
        "next_unlocked": bool(nxt and nxt["masks"][stage] & unl),
        "missed": [c["name"] for c in here if c["masks"][stage] & missed & ~unl],
        "done": unl & goal == goal and stage == len(p["stages"]) - 1,
    }


def bingo(p: dict, data: bytes, active: bool, released: bool) -> dict:
    """A bingo card (pop'n 7): which squares are open (bits of a 32-bit word, kept in copies)."""
    words = {int.from_bytes(data[o:o + 4], "big") for o in p["offsets"]}
    if len(words) != 1:
        raise Error(f"the copies of {p['id']} disagree: not a save this tool knows")
    word = words.pop()
    cells = p["cells"]
    open_ = [c.get("center", False) or bool(word >> c["bit"] & 1) for c in cells]
    lines = [ln["name"] for ln in p["lines"] if all(open_[i] for i in ln["cells"])]
    closed = [c for c, o in zip(cells, open_) if not o]
    near = sorted({ln["name"] for ln in p["lines"] if sum(not open_[i] for i in ln["cells"]) == 1})
    return {"active": active, "released": released, "open": sum(open_) - 1, "squares": len(cells) - 1,
            "lines": lines, "line_count": len(p["lines"]), "near": near,
            "closed": [{"shown": c["shown"], "condition": c["condition"]} for c in closed]}


def gamblerz(p: dict, data: bytes, active: bool, released: bool) -> dict:
    """pop'n 9's Gambler Z labyrinth: the items won (a little-endian word in the settings record,
    kept in two copies), the position, floor and a boss fight under way."""
    def field(off, n, signed=False):
        vals = {int.from_bytes(data[c + off:c + off + n], "little", signed=signed) for c in p["copies"]}
        if len(vals) != 1:
            raise Error(f"the copies of {p['id']} disagree: not a save this tool knows")
        return vals.pop()
    flags = field(p["flags"], 4)
    return {"active": active, "released": released, "flags": flags,
            "prog": bin(flags & 0x1ffffff).count("1"), "cleared": bool(flags & p["cleared"]),
            "items": [i["name"] for i in p["items"] if flags >> i["bit"] & 1],
            "items_missing": [i["name"] for i in p["items"] if not flags >> i["bit"] & 1],
            "item_count": len(p["items"]),
            "bosses": [b["name"] for b in p["bosses"] if flags & b["mask"] == b["mask"]],
            "bosses_missing": [b["name"] for b in p["bosses"] if flags & b["mask"] != b["mask"]],
            "boss_count": len(p["bosses"]),
            "position": [field(p["position"], 2, True), field(p["position"] + 2, 2, True)],
            "floor": field(p["floor"], 1), "boss_hp": field(p["boss_hp"], 2)}


def wishes_fields(p: dict, data: bytes) -> tuple[list, list, int]:
    """pop'n 10's HOSHI NI NEGAIO: the buckets' levels and stars, and the items word (little-endian,
    in the settings record's copies)."""
    vals = set()
    for c in p["copies"]:
        lv = tuple(data[c + p["levels"]:c + p["levels"] + len(p["buckets"])])
        st = tuple(int.from_bytes(data[c + p["stars"] + 2 * k:c + p["stars"] + 2 * k + 2], "little") for k in range(len(p["buckets"])))
        vals.add((lv, st, int.from_bytes(data[c + p["items"]:c + p["items"] + 8], "little")))
    if len(vals) != 1:
        raise Error(f"the copies of {p['id']} disagree: not a save this tool knows")
    lv, st, items = vals.pop()
    return list(lv), list(st), items


def wishes(p: dict, data: bytes, active: bool, released: bool) -> dict:
    lv, st, items = wishes_fields(p, data)
    w = {x["bit"]: x for x in p["wishes"]}
    buckets = []
    for b, (bucket, level, stars) in enumerate(zip(p["buckets"], lv, st)):
        nxt = w[bucket["wishes"][level]] if level < len(bucket["wishes"]) else None
        buckets.append({"name": bucket["name"], "level": level, "of": len(bucket["wishes"]), "stars": stars,
                        "next": nxt and nxt["gives"], "next_stars": nxt and nxt["stars"]})
    black = p["black"]
    return {"active": active, "released": released, "items": items, "buckets": buckets,
            "granted": [w[k]["gives"] for k in sorted(w) if items >> k & 1],
            "missing": [w[k]["gives"] for k in sorted(w) if not items >> k & 1],
            "wish_count": len(w), "black": bool(items >> black["bit"] & 1), "black_gives": black["gives"],
            "black_stars": black["stars"], "all_buckets": all(b["level"] >= b["of"] for b in buckets)}


def eventmask_word(p: dict, data: bytes) -> int:
    vals = {int.from_bytes(data[c + p["offset"]:c + p["offset"] + 4], "little") for c in p["copies"]}
    if len(vals) != 1:
        raise Error(f"the copies of {p['id']} disagree: not a save this tool knows")
    return vals.pop()


def eventmask(p: dict, data: bytes, active: bool, released: bool) -> dict:
    """An event whose finds are bits of a word in the settings record (pop'n 11's POPN TOURIST)."""
    word = eventmask_word(p, data)
    return {"active": active, "released": released, "word": word, "count": len(p["bits"]),
            "found": [b["gives"] for b in p["bits"] if word >> b["bit"] & 1],
            "missing": [b["gives"] for b in p["bits"] if not word >> b["bit"] & 1]}


def points_fields(p: dict, data: bytes) -> list[int]:
    """kind = "points": the groups' counters (32-bit words from `offset`), the copies agreeing."""
    vals = {tuple(int.from_bytes(data[c + p["offset"] + 4 * g:c + p["offset"] + 4 * g + 4], "little")
                  for g in range(len(p["groups"]))) for c in p["copies"]}
    if len(vals) != 1:
        raise Error(f"the copies of {p['id']} disagree: not a save this tool knows")
    return list(vals.pop())


def points_word(p: dict, counters: list[int]) -> int:
    """What the counters reach, as the game works it out (pop'n 12 0x1a21d8): in each group, the
    items in order while the points left cover each one's cost."""
    word = 0
    for g, left in zip(p["groups"], counters):
        for item in g["items"]:
            if left < item["cost"]:
                break
            left -= item["cost"]
            word |= 1 << item["bit"]
    return word


def points(p: dict, data: bytes, active: bool, released: bool) -> dict:
    """An event whose groups each earn points, reaching their items in order (pop'n 12's NINPOUCHO)."""
    counters = points_fields(p, data)
    word = points_word(p, counters)
    groups = []
    for g, n in zip(p["groups"], counters):
        reached = [i for i in g["items"] if word >> i["bit"] & 1]
        nxt = g["items"][len(reached)] if len(reached) < len(g["items"]) else None
        groups.append({"name": g["name"], "points": n, "of": sum(i["cost"] for i in g["items"]),
                       "reached": [i["gives"] for i in reached], "count": len(g["items"]),
                       "next": nxt and nxt["gives"],
                       "next_in": nxt and sum(i["cost"] for i in g["items"][:len(reached) + 1]) - n})
    return {"active": active, "released": released, "groups": groups,
            "reached": sum(len(g["reached"]) for g in groups), "count": sum(g["count"] for g in groups)}


def apply_points(game: str, p: dict, unlocks: dict, nvram: str, value: str) -> str:
    """all (every group's points cover all its items) or none (a new event)."""
    path = os.path.join(nvram, p["nvram"])
    if not os.path.exists(path):
        raise Error(f"{path}: no save yet. Start {game} once (with its first-boot setup), quit it, then unlock.")
    u = unlocks[p["checksum_of"]]
    current(u, nvram)
    data = bytearray(open(path, "rb").read())
    points_fields(p, data)
    backup = backup_name(path)
    shutil.copy2(path, backup)
    counters = [sum(i["cost"] for i in g["items"]) if value == "all" else 0 for g in p["groups"]]
    for c in p["copies"]:
        for g, n in enumerate(counters):
            data[c + p["offset"] + 4 * g:c + p["offset"] + 4 * g + 4] = n.to_bytes(4, "little")
        data[c + p["word"]:c + p["word"] + 4] = points_word(p, counters).to_bytes(4, "little")
    seal(u, data)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)
    return backup


def thresholds_fields(p: dict, data: bytes) -> list[int]:
    """kind = "thresholds": every song's counter (32-bit words from `offset`), the copies agreeing."""
    n = max(song["counter"] for a in p["areas"] for song in a["songs"]) + 1
    vals = {tuple(int.from_bytes(data[c + p["offset"] + 4 * k:c + p["offset"] + 4 * k + 4], "little")
                  for k in range(n)) for c in p["copies"]}
    if len(vals) != 1:
        raise Error(f"the copies of {p['id']} disagree: not a save this tool knows")
    return list(vals.pop())


def thresholds_words(area: dict, counters: list[int]) -> int:
    """What an area's counters reach, as the game works it out (pop'n 13 0x17fb208, pop'n 14
    0x17f2b38): a chart once its song's counter reaches the chart's mark. Where the game keeps no
    words (pop'n 14), `bit` only numbers the area's items."""
    word = 0
    for song in area["songs"]:
        for ch in song["charts"]:
            if counters[song["counter"]] >= ch["at"]:
                word |= 1 << ch["bit"]
    return word


def thresholds(p: dict, data: bytes, unlocks: dict, nvram: str) -> dict:
    """An event whose songs each earn points, opening their charts at marks (pop'n 13's POP'N
    LAND and POP'N WORLD)."""
    counters = thresholds_fields(p, data)
    phase = current(unlocks[p["checksum_of"]], nvram)
    areas = []
    for a in p["areas"]:
        word = thresholds_words(a, counters)
        charts = [ch for song in a["songs"] for ch in song["charts"]]
        nxt = sorted(((ch["at"] - counters[song["counter"]], ch["gives"]) for song in a["songs"]
                      for ch in song["charts"] if not word >> ch["bit"] & 1))
        areas.append({"name": a["name"], "active": phase in a["active"], "released": phase in a["released"],
                      "reached": [ch["gives"] for ch in charts if word >> ch["bit"] & 1], "count": len(charts),
                      "next": nxt[0][1] if nxt else None, "next_in": nxt[0][0] if nxt else None})
    return {"areas": areas, "reached": sum(len(a["reached"]) for a in areas), "count": sum(a["count"] for a in areas)}


def apply_thresholds(game: str, p: dict, unlocks: dict, nvram: str, value: str) -> str:
    """all (every song's counter at its last mark) or none (a new event)."""
    path = os.path.join(nvram, p["nvram"])
    if not os.path.exists(path):
        raise Error(f"{path}: no save yet. Start {game} once (with its first-boot setup), quit it, then unlock.")
    u = unlocks[p["checksum_of"]]
    current(u, nvram)
    data = bytearray(open(path, "rb").read())
    counters = thresholds_fields(p, data)
    backup = backup_name(path)
    shutil.copy2(path, backup)
    for a in p["areas"]:
        for song in a["songs"]:
            counters[song["counter"]] = max(ch["at"] for ch in song["charts"]) if value == "all" else 0
    for c in p["copies"]:
        for k, n in enumerate(counters):
            data[c + p["offset"] + 4 * k:c + p["offset"] + 4 * k + 4] = n.to_bytes(4, "little")
        for a in p["areas"]:   # pop'n 14 keeps no words: the game reads the counters
            word = thresholds_words(a, counters)
            for w, at in enumerate(a.get("words", [])):
                data[c + p["offset"] + at:c + p["offset"] + at + 4] = (word >> (32 * w) & 0xffffffff).to_bytes(4, "little")
    seal(u, data)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)
    return backup


def store_folder(p: dict, nvram: str) -> str:
    """The server's folder: the save's own, or, for a save in its default place, the one the pop'n
    games share beside the saves (runtime/python/core.cpp: one card for all)"""
    own = os.path.join(nvram, p["store"])
    shared = os.path.join(os.path.dirname(os.path.normpath(nvram)), p["store"])
    if not os.path.isdir(own) and os.path.isdir(shared):
        return shared
    return own


def ticket_files(p: dict, nvram: str) -> list[str]:
    folder = store_folder(p, nvram)
    if not os.path.isdir(folder):
        return []
    return sorted(os.path.join(folder, f) for f in os.listdir(folder)
                  if f.startswith(p["prefix"]) and not f.endswith((".tmp", ".bak")))


def read_record(path: str) -> dict:
    """One record of HIRAKU's server (runtime/eamuse/server.cpp): tab-separated key=value."""
    line = open(path, encoding="utf-8").readline().rstrip("\n")
    return dict(f.partition("=")[::2] for f in line.split("\t") if f)


def tickets(p: dict, nvram: str) -> dict:
    """Each card's mileage and tickets, as the server inside HIRAKU keeps them (pop'n 11)."""
    cards = []
    for path in ticket_files(p, nvram):
        r = read_record(path)
        counts = [int(x or 0) for x in r.get("ticket", "").split(",")[:len(p["songs"])]]
        counts += [0] * (len(p["songs"]) - len(counts))
        cards.append({"card": os.path.basename(path)[len(p["prefix"]):], "mileage": int(r.get("mileage") or 0), "tickets": counts})
    return {"cards": cards, "songs": [x["gives"] for x in p["songs"]], "max": p["max"]}


def apply_tickets(p: dict, nvram: str, value: str) -> str:
    files = ticket_files(p, nvram)
    if not files:
        raise Error(f"no cards yet in {store_folder(p, nvram)}: play a credit with a card on the server inside HIRAKU first")
    n = p["max"] if value == "all" else 0
    for path in files:
        r = read_record(path)
        shutil.copy2(path, backup_name(path))
        r["ticket"] = ",".join([str(n)] * len(p["songs"])) + ","
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            f.write("\t".join(f"{k}={v}" for k, v in r.items()) + "\n")
        os.replace(tmp, path)
    return f"{len(files)} card(s)"


def apply_eventmask(game: str, p: dict, unlocks: dict, nvram: str, value: str) -> str:
    path = os.path.join(nvram, p["nvram"])
    if not os.path.exists(path):
        raise Error(f"{path}: no save yet. Start {game} once (with its first-boot setup), quit it, then unlock.")
    u = unlocks[p["checksum_of"]]
    current(u, nvram)
    data = bytearray(open(path, "rb").read())
    eventmask_word(p, data)
    backup = backup_name(path)
    shutil.copy2(path, backup)
    word = sum(1 << b["bit"] for b in p["bits"]) if value == "all" else 0
    for c in p["copies"]:
        data[c + p["offset"]:c + p["offset"] + 4] = word.to_bytes(4, "little")
    seal(u, data)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)
    return backup


def apply_wishes(game: str, p: dict, unlocks: dict, nvram: str, value: str) -> str:
    """HOSHI NI NEGAIO all (every wish granted and the black bucket done, as the game leaves a shop
    that finished the event) or none (a new event)."""
    path = os.path.join(nvram, p["nvram"])
    if not os.path.exists(path):
        raise Error(f"{path}: no save yet. Start {game} once (with its first-boot setup), quit it, then unlock.")
    u = unlocks[p["checksum_of"]]
    current(u, nvram)   # refuses a save it doesn't understand
    data = bytearray(open(path, "rb").read())
    wishes_fields(p, data)
    backup = backup_name(path)
    shutil.copy2(path, backup)
    every = value == "all"
    items = sum(1 << x["bit"] for x in p["wishes"]) | (1 << p["black"]["bit"]) if every else 0
    for c in p["copies"]:
        for b, bucket in enumerate(p["buckets"]):
            data[c + p["levels"] + b] = len(bucket["wishes"]) if every else 0
            data[c + p["stars"] + 2 * b:c + p["stars"] + 2 * b + 2] = (0).to_bytes(2, "little")
        data[c + p["items"]:c + p["items"] + 8] = items.to_bytes(8, "little")
    seal(u, data)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)
    return backup


def describe_progress(p: dict, s: dict) -> str:
    if s is None:
        return "no save yet"
    if p.get("kind") == "eventmask":
        lines = [] if s["active"] else ["the event is not running (it runs at phase 2 or 3)"]
        if s["released"]:
            lines.append("DATA RELEASE (phase 4 or MAX): everything it gives is playable anyway")
        lines.append(f"found: {len(s['found'])} of {s['count']}")
        if s["missing"]:
            lines.append("still to find: " + "; ".join(s["missing"]))
        return "\n".join(lines)
    if p.get("kind") == "points":
        lines = [] if s["active"] else [f"the event is not running ({p.get('runs', 'see its phase')})"]
        if s["released"]:
            lines.append("DATA RELEASE: everything it gives is playable anyway")
        lines.append(f"reached: {s['reached']} of {s['count']}")
        for g in s["groups"]:
            line = f"{g['name']}: {g['points']} of {g['of']} points, {len(g['reached'])} of {g['count']}"
            if g["next"]:
                line += f"; next: {g['next']} in {g['next_in']} points"
            lines.append(line)
        return "\n".join(lines)
    if p.get("kind") == "thresholds":
        lines = [f"reached: {s['reached']} of {s['count']}"]
        for a in s["areas"]:
            state = "running" if a["active"] else "DATA RELEASE: all playable anyway" if a["released"] else "not running"
            line = f"{a['name']} ({state}): {len(a['reached'])} of {a['count']}"
            if a["next"]:
                line += f"; nearest: {a['next']} in {a['next_in']} points"
            lines.append(line)
        return "\n".join(lines)
    if p.get("kind") == "tickets":
        if not s["cards"]:
            return "no card yet on the server inside HIRAKU (its eamuse/ folder, beside the saves)"
        lines = []
        for c in s["cards"]:
            lines.append(f"card {c['card']}: mileage {c['mileage']}; tickets " + ", ".join(str(t) for t in c["tickets"]))
        lines.append("the tickets, in order: " + "; ".join(f"{i + 1}. {g}" for i, g in enumerate(s["songs"])))
        return "\n".join(lines)
    if p.get("kind") == "wishes":
        lines = [] if s["active"] else ["the event is not running (it runs at phase 2 or 3)"]
        if s["released"]:
            lines.append("DATA RELEASE (phase 4 or MAX): its songs and characters are playable anyway")
        lines.append(f"wishes granted: {len(s['granted'])} of {s['wish_count']}"
                     + ("; the black bucket is done: " + s["black_gives"] if s["black"] else ""))
        for b in s["buckets"]:
            lines.append(f"  {b['name']}: wish {b['level']} of {b['of']}" +
                         (f", {b['stars']} of {b['next_stars']} stars toward {b['next']}" if b["next"] else ", done"))
        if s["all_buckets"] and not s["black"]:
            lines.append(f"  the black bucket: {s['black_stars']} stars toward {s['black_gives']}")
        if s["missing"]:
            lines.append("still to wish for: " + "; ".join(s["missing"]))
        return "\n".join(lines)
    if p.get("kind") == "gamblerz":
        lines = [] if s["active"] else ["the event is not running (it runs at phase 1 or 2)"]
        if s["released"]:
            lines.append("DATA RELEASE (phase 3 or MAX): its songs are unlocked anyway")
        if s["cleared"]:
            lines.append("the labyrinth is cleared")
        lines.append(f"songs won: {len(s['items'])} of {s['item_count']}; bosses beaten: {len(s['bosses'])} of "
                     f"{s['boss_count']} (the e-AMUSEMENT ranking's prog {s['prog']}, parts {s['flags']:08x})")
        lines.append(f"floor index {s['floor']}, position {s['position'][0]}, {s['position'][1]}"
                     + (f"; a boss fight under way, {s['boss_hp']} HP left" if s["boss_hp"] else ""))
        if s["items_missing"]:
            lines.append("songs still to win: " + ", ".join(s["items_missing"]))
        if s["bosses_missing"]:
            lines.append("bosses still to beat (an EX chart and a course each): " + "; ".join(s["bosses_missing"]))
        return "\n".join(lines)
    if p.get("kind") == "bingo":
        lines = [] if s["active"] else ["the event is not running (BINGO de 7: NO)"]
        if s["released"]:
            lines.append("BINGO de 7 DATA RELEASE is on: its songs are unlocked anyway")
        lines.append(f"{s['open']} of {s['squares']} squares open; lines {len(s['lines'])} of {s['line_count']}"
                     + (f" ({', '.join(s['lines'])})" if s["lines"] else ""))
        if s["near"]:
            lines.append("one square from a line: " + ", ".join(s["near"]))
        if s["closed"]:
            lines.append("still closed (the number on the card, and the game's name for its condition):")
            lines += [f"  {c['shown']}: {c['condition']}" for c in s["closed"]]
        return "\n".join(lines)
    lines = [] if s["active"] else ["the event is not running (it runs at phase 1 or 2)"]
    if s["done"]:
        return "\n".join(lines + ["all done: everything unlocked"])
    lines.append(f"Stage {s['stage']} of {s['stages']} ({s['stage_name']}): {s['unlocked']} of {s['goal']} unlocked")
    lines.append(f"KRAFT: space {s['position']} of {s['spaces']}, lap {s['lap'] + 1}; next city {s['next']} "
                 f"({s['next_romaji']}), {s['next_in']} spaces away" + (" (already unlocked)" if s["next_unlocked"] else ""))
    if s["missed"]:
        lines.append("passed without landing (given on the next pass): " + ", ".join(s["missed"]))
    return "\n".join(lines)


def level_name(u: dict, v) -> str:
    for lv in u.get("level", []):
        if lv["value"] == v:
            return lv["name"]
    return "none" if v is None else f"other ({v:#x}: set in the game's test mode)"


def apply(game: str, u: dict, nvram: str, value: int) -> str:
    if value not in [lv["value"] for lv in u.get("level", [])]:
        raise Error(f"{u['id']}={value}: not one of {[lv['value'] for lv in u['level']]}")
    path = os.path.join(nvram, u["nvram"])
    if not os.path.exists(path):
        raise Error(f"{path}: no save yet. Start {game} once (with its first-boot setup), quit it, then unlock.")
    current(u, nvram)   # refuses a save it doesn't understand
    data = bytearray(open(path, "rb").read())
    backup = backup_name(path)
    shutil.copy2(path, backup)
    for o in u["offsets"]:
        data[o] = value
    seal(u, data)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)
    return backup


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game")
    ap.add_argument("set", nargs="*", metavar="ID=VALUE")
    ap.add_argument("--nvram")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_intermixed_args()   # (ID=VALUE after an option too: Python before 3.12 took none there, and the launcher puts them last)
    nvram = a.nvram or default_nvram(a.game)
    try:
        spec = load(a.game)
        unlocks = {u["id"]: u for u in spec.get("unlock", [])}
        owner = {sw["id"]: u for u in unlocks.values() for sw in u.get("switch", [])}
        pending: dict[str, dict] = {}
        wishes_sets: list[tuple[str, str]] = []
        wish_ids = {p["id"] for p in spec.get("progress", []) if p.get("kind") in ("wishes", "eventmask", "points", "thresholds", "tickets")}
        for s in a.set:
            uid, _, v = s.partition("=")
            if uid in wish_ids and v in ("all", "none"):
                wishes_sets.append((uid, v))
            elif uid in owner and v in ("0", "1"):
                pending.setdefault(owner[uid]["id"], {})[uid] = v == "1"
            elif uid in unlocks and unlocks[uid].get("kind") == "switches" and v in ("all", "none"):
                pending.setdefault(uid, {}).update({sw["id"]: v == "all" for sw in unlocks[uid]["switch"]})
            elif uid in unlocks and v and unlocks[uid].get("kind", "level") == "level":
                u = unlocks[uid]
                before = current(u, nvram)
                backup = apply(a.game, u, nvram, int(v, 0))
                if not a.json:
                    print(f"{uid}: {level_name(u, before)} -> {level_name(u, int(v, 0))} (the old save: {backup})")
            else:
                raise Error(f"{s}: expected ID=VALUE, ID one of {list(unlocks) + list(owner)}")
        progresses = {p["id"]: p for p in spec.get("progress", [])}
        for uid, v in wishes_sets:
            kind = progresses[uid].get("kind")
            if kind == "eventmask":
                backup = apply_eventmask(a.game, progresses[uid], unlocks, nvram, v)
            elif kind == "points":
                backup = apply_points(a.game, progresses[uid], unlocks, nvram, v)
            elif kind == "thresholds":
                backup = apply_thresholds(a.game, progresses[uid], unlocks, nvram, v)
            elif kind == "tickets":
                backup = apply_tickets(progresses[uid], nvram, v)
            else:
                backup = apply_wishes(a.game, progresses[uid], unlocks, nvram, v)
            if not a.json:
                print(f"{uid}: {v} ({'the old save: ' + backup if os.path.isabs(backup) else backup + ', each backed up'})")
        for uid, changes in pending.items():
            backup = apply_switches(a.game, unlocks[uid], nvram, changes)
            if not a.json:
                print(f"{uid}: " + ", ".join(f"{k} {'on' if v else 'off'}" for k, v in changes.items()) +
                      f" (the old save: {backup})")
        state = []
        for u in spec.get("unlock", []):
            if u.get("kind") == "switches":
                state.append({**u, "current": switches(u, nvram)})
            else:
                state.append({**u, "current": current(u, nvram)})
        prog = []
        for p in spec.get("progress", []):
            s = progress(p, nvram, unlocks)
            entry = {"id": p["id"], "title": p["title"], "state": s, "text": describe_progress(p, s)}
            if p.get("kind") == "wishes":   # the launcher offers these
                entry["choices"] = [{"value": "none", "name": "A new event: no wish granted"},
                                    {"value": "all", "name": "Every wish granted, the black bucket done"}]
            elif p.get("kind") == "eventmask":
                entry["choices"] = [{"value": "none", "name": "A new event: nothing found"},
                                    {"value": "all", "name": "Everything found"}]
            elif p.get("kind") in ("points", "thresholds"):
                entry["choices"] = [{"value": "none", "name": "A new event: no points"},
                                    {"value": "all", "name": "Everything reached"}]
            elif p.get("kind") == "tickets":
                entry["choices"] = [{"value": "all", "name": f"Every card: {p['max']} tickets of each kind"},
                                    {"value": "none", "name": "Every card: no tickets"}]
            prog.append(entry)
        if a.json:
            print(json.dumps({"game": a.game, "nvram": nvram, "unlocks": state, "progress": prog,
                              "notes": spec.get("note", [])}, ensure_ascii=False))
        elif not a.set:
            print(f"{a.game}, save {nvram}")
            for u in state:
                if u.get("kind") == "switches":
                    cur = u["current"] or {}
                    print(f"\n{u['title']} ({u['id']}={{all,none}}; menu code {u['menu']['code']})")
                    for sw in u["switch"]:
                        st = cur.get(sw["id"], {})
                        mark = "*" if st.get("on") else " "
                        print(f" {mark} {sw['id']}=1  {sw['name']}  code {sw['code']}\n      {sw['text']}")
                    continue
                print(f"\n{u['title']} ({u['id']}): now {level_name(u, u['current'])}")
                for lv in u.get("level", []):
                    mark = "*" if lv["value"] == u["current"] else " "
                    code = f"  code {lv['code']}" if lv.get("code") else ""
                    print(f" {mark} {u['id']}={lv['value']}  {lv['name']}{code}\n      {lv['text']}")
            for p in prog:
                print(f"\n{p['title']}:\n{p['text']}")
            for n in spec.get("note", []):
                print(f"\n{n['title']}:\n{n['text']}")
    except Error as e:
        if a.json:
            print(json.dumps({"error": str(e)}, ensure_ascii=False))
        else:
            print(f"unlock: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
