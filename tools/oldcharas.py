#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Characters the 64-bit PC pop'n removed, from an older game of the player's (docs/SONGS-BACK.md):
writes a mod folder whose music data describes them again (popnhax's <chara id> entries), for the
songs brought back that are theirs.

  tools/oldcharas.py [--game DIR] OLDER OUT

OLDER: an older PC pop'n's program, a 32-bit popn22.dll (pop'n 22 to the last 32-bit game).
OUT: the mod folder to write (put it in the launcher's mods folder for the game, or give it with
--mod); it holds the older game's names and texts for these characters and their hariai pictures
(tex/system/ha_merge_ifs/, from the older game's tex/system/ha_chara*.ifs: the 64-bit game's packs
haven't them, and it faults without one) and their names' pictures (chara_name_diff_ifs/, from its
chara_name*.ifs, the same), nothing else.
DIR: the 64-bit game's folder (modules/, plain_data/ or data/); by default dumps/popn29/machine.txt's.

The 64-bit game keeps a slot for each character it removed (its flags' bit 0, a stand-in's names),
and the ids are the same in every game: a slot is described again when the older game has the
character there, the 64-bit game's data still has its pictures (tex/FOLDER/NAME.ifs) and the older
game's has its hariai picture. The
loader (runtime/pc64/songs.c) fills these slots before the songs' characters are looked up.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ifs  # noqa: E402
import songsback  # noqa: E402

REC32 = 0x4C   # a character in the 32-bit game: 7 words (name, flags, folder, gg, cs, icon1, icon2), ...
REMOVED = 1


class Older:
    """The 32-bit game's character table: the longest run of records whose name, folder and gg
    picture name are texts (gg_...)"""

    def __init__(self, path):
        self.d = d = open(path, "rb").read()
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if struct.unpack_from("<H", d, pe + 24)[0] != 0x10B:
            raise SystemExit(path + ": not a 32-bit program")
        self.base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
        n, opt = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
        self.secs = []
        for i in range(n):
            vs, va, rs, ro = struct.unpack_from("<IIII", d, pe + 24 + opt + 40 * i + 8)
            self.secs.append((va, max(vs, rs), ro, rs))
        best = (0, 0)
        for va, size, ro, rs in self.secs:
            at = ro
            while at + REC32 <= ro + rs:
                n = 0
                while self.looks(at + n * REC32):
                    n += 1
                if n > best[1]:
                    best = (at, n)
                at += REC32 * n if n else 4
        self.table, self.n = best
        if self.n < 1000:
            raise SystemExit(path + ": its character table not found")

    def text(self, v):
        r = v - self.base
        for va, size, ro, rs in self.secs:
            if v and va <= r < va + min(size, rs):
                o = ro + r - va
                e = self.d.find(b"\0", o, o + 256)
                return self.d[o:e] if e >= 0 else None
        return None

    def looks(self, at):
        if at < 0 or at + REC32 > len(self.d):
            return False
        name, _, folder, gg = struct.unpack_from("<IIII", self.d, at)
        n, f, g = self.text(name), self.text(folder), self.text(gg)
        return bool(n) and f is not None and len(f) < 32 and bool(g) and g.startswith(b"gg_")

    def record(self, i):
        o = self.table + i * REC32
        w = struct.unpack_from("<19I", self.d, o)
        t = lambda v: self.text(v) or b""
        return {"chara_id": t(w[0]), "flags": w[1], "folder": t(w[2]), "gg": t(w[3]), "cs": t(w[4]),
                "icon1": t(w[5]), "icon2": t(w[6]), "chara_xw": w[7] & 0xFFFF, "chara_yh": w[7] >> 16,
                "display_flags": w[8], "flavor_idx": struct.unpack_from("<h", self.d, o + 0x24)[0],
                "sort_name": t(w[10]), "disp_name": t(w[11]), "file_type": w[12], "lapis_shape": w[13],
                "ha": t(w[15]), "catchtext": t(w[16]), "game_version": w[18]}


def esc(b):
    return b.replace(b"&", b"&amp;").replace(b"<", b"&lt;").replace(b">", b"&gt;")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--game", help="the 64-bit game's folder (modules/, plain_data/ or data/)")
    ap.add_argument("older", help="an older PC pop'n's popn22.dll")
    ap.add_argument("out", help="the mod folder to write")
    a = ap.parse_args()
    game = songsback.game_folder(a.game)
    data = os.path.join(game, "plain_data") if os.path.isdir(os.path.join(game, "plain_data")) else os.path.join(game, "data")
    g = songsback.Game(os.path.join(game, "modules", "popn.dll"))
    old = Older(a.older)
    # the two tables lined up by the characters both have (the older one's may start a record off)
    names = [g.chara(i) for i in range(g.ncharas)]
    shift = max(range(-4, 5), key=lambda k: sum(1 for i in range(0, min(g.ncharas, old.n), 7)
                                                if 0 <= i + k < old.n and names[i]["chara_id"] and
                                                old.record(i + k)["chara_id"] == names[i]["chara_id"].encode("cp932", "replace")))
    # the older game's picture packs (its data folder next to its modules/ folder): the hariai
    # (ha_*.ifs) and the names' (chara_name*.ifs), each picture a pack of its own in them
    older_data = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(a.older))), "data", "tex", "system")
    pictures = {"ha": {}, "gg": {}}
    for f in sorted(os.listdir(older_data)) if os.path.isdir(older_data) else []:
        kind = "ha" if f.startswith("ha_") else "gg" if f.startswith("chara_name") else None
        if not kind or not f.endswith(".ifs"):
            continue
        try:
            pack = ifs.Pack(os.path.join(older_data, f))
        except ValueError:
            continue
        for m in pack.members:
            if "/" not in m:
                pictures[kind].setdefault(m, pack)
    # the 64-bit game's own characters' pictures (it has those)
    own = {k: {c[k] for c in names if c[k] and not c["flags"] & REMOVED} for k in ("ha", "gg")}
    out, missing, no_pic = [], [], []
    for i in range(1, g.ncharas):
        if not names[i]["flags"] & REMOVED or not 0 <= i + shift < old.n:
            continue
        r = old.record(i + shift)
        if r["flags"] & REMOVED or not r["chara_id"]:
            continue
        rel = os.path.join("tex", r["folder"].decode("cp932"), r["chara_id"].decode("cp932") + ".ifs")
        if not os.path.exists(os.path.join(data, rel)):
            missing.append((i, r["chara_id"].decode("cp932")))
            continue
        pics = {k: r[k].decode("cp932") for k in ("ha", "gg")}
        if any(pics[k] not in own[k] and pics[k] + ".ifs" not in pictures[k] for k in pics):
            no_pic.append((i, r["chara_id"].decode("cp932")))   # described all the same: another mod may have them
        out.append((i, r, pics))
    copied = 0
    for i, r, pics in out:
        for k, folder in (("ha", "ha_merge_ifs"), ("gg", "chara_name_diff_ifs")):
            m = pics[k] + ".ifs"
            if pics[k] in own[k] or m not in pictures[k]:
                continue
            os.makedirs(os.path.join(a.out, "tex", "system", folder), exist_ok=True)
            with open(os.path.join(a.out, "tex", "system", folder, m), "wb") as f:
                f.write(pictures[k][m].read(m))
            copied += 1
    x = [b"<?xml version='1.0' encoding='shift-jis'?>", b"<database>"]
    for i, r, pics in out:
        x.append(b'  <chara id="%d">' % i)
        for k in ("chara_id", "folder", "gg", "cs", "icon1", "icon2", "sort_name", "disp_name", "ha", "catchtext"):
            x.append(b'    <%s __type="str">%s</%s>' % (k.encode(), esc(r[k]), k.encode()))
        for k, t in (("flags", "u32"), ("display_flags", "u32"), ("file_type", "u32"), ("game_version", "u32"),
                     ("chara_xw", "u16"), ("chara_yh", "u16"), ("flavor_idx", "s16")):
            x.append(b'    <%s __type="%s">%d</%s>' % (k.encode(), t.encode(), r[k], k.encode()))
        x.append(b'    <lapis_shape __type="str">%s</lapis_shape>' % (b"dia" if r["lapis_shape"] == 1 else b""))
        x.append(b"  </chara>")
    x.append(b"</database>")
    with open(os.path.join(a.out, "charadb_older.xml"), "wb") as f:
        f.write(b"\n".join(x) + b"\n")
    toml = os.path.join(a.out, "mod.toml")
    if not os.path.exists(toml):
        with open(toml, "w", encoding="utf-8") as f:
            f.write('name = "Characters back"\ngame = "popn29"\ndescription = "The characters the game removed, from an older game"\n')
    print("the 64-bit game: %d characters, %d removed; the older game's table: %d (ids %+d)" %
          (g.ncharas, sum(1 for c in names if c["flags"] & REMOVED), old.n, shift))
    print("described again: %d -> %s" % (len(out), os.path.join(a.out, "charadb_older.xml")))
    print("pictures copied from the older game (hariai, names): %d" % copied)
    if no_pic:
        print("a picture of theirs isn't in the older game's tex/system/ha_*.ifs or chara_name*.ifs: %d (%s); they come back when "
              "another mod has it (tex/system/ha_merge_ifs/, chara_name_diff_ifs/)" % (len(no_pic), ", ".join(n for _, n in no_pic[:10])))
    if missing:
        print("the older game has them, the 64-bit game's data hasn't their pictures: %d (%s)" %
              (len(missing), ", ".join(n for _, n in missing[:10]) + (", ..." if len(missing) > 10 else "")))


if __name__ == "__main__":
    main()
