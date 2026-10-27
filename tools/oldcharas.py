#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Characters the 64-bit PC pop'n removed, from an older game of the player's (docs/SONGS-BACK.md):
writes a mod folder whose music data describes them again (popnhax's <chara id> entries), for the
songs brought back that are theirs.

  tools/oldcharas.py [--game DIR] OLDER OUT

OLDER: an older PC pop'n's program, a 32-bit popn22.dll (pop'n 22 to the last 32-bit game).
OUT: the mod folder to write (put it in the launcher's mods folder for the game, or give it with
--mod, before the Omnimix pack); it holds the older game's names and texts for these characters and their hariai pictures
(tex/system/ha_merge_ifs/, from the older game's tex/system/ha_chara*.ifs: the 64-bit game's packs
haven't them, and it faults without one) and their names' pictures (chara_name_diff_ifs/, from its
chara_name*.ifs, the same), made this game's size (its pictures are 2 to 2.3 times the older
games': the older ones scaled up, Pillow), nothing else.
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

# characters no older PC game describes (a stand-in there too) whose pictures the older game still
# has: the record made from them, in the slot found for it (2062: the one removed amid pop'n 23's
# characters, 2044-2065, with this game's size for it; the names its pictures carry)
BY_PICTURES = {2062: ("umaru_23a", "23", "ウマル", "うまる", 23)}

REC32 = 0x4C   # a character in the 32-bit game: 7 words (name, flags, folder, gg, cs, icon1, icon2), ...
REMOVED = 1


def bigger(pack, scale):
    """a picture pack (bytes) with its pictures made `scale` times bigger, as this game's own are:
    each texture's one picture (argb8888rev, avslz) decoded, scaled, its rectangles and the
    texture's size made so; the pack written again. None when it isn't such a pack."""
    import hashlib
    import tempfile
    from PIL import Image
    with tempfile.NamedTemporaryFile(suffix=".ifs") as f:
        f.write(pack)
        f.flush()
        p = ifs.Pack(f.name)
        if "tex/texturelist.xml" not in p.members:
            return None
        tl = ifs.kbin(p.read("tex/texturelist.xml"))
        six, enc = ifs.kbin.six, ifs.kbin.encoding
        new = {}
        if dict(tl[4]).get("compress", b"").rstrip(b"\0") != b"avslz":
            return None
        for tex in tl[3]:
            attrs = {k: v.rstrip(b"\0") for k, v in tex[4]}
            images = [n for n in tex[3] if n[0] == "image"]
            size = [n for n in tex[3] if n[0] == "size"]
            if tex[0] != "texture" or attrs.get("format") != b"argb8888rev" or \
                    len(images) != 1 or len(size) != 1:
                return None
            im = images[0]
            name = dict(im[4]).get("name", b"").rstrip(b"\0")
            uv = [n for n in im[3] if n[0] == "uvrect"][0]
            rect = [n for n in im[3] if n[0] == "imgrect"][0]
            member = "tex/" + hashlib.md5(name).hexdigest()
            x0, x1, y0, y1 = rect[2]
            w, h = (x1 - x0) // 2, (y1 - y0) // 2
            raw = ifs.lz77_undo(p.read(member)) if member in p.members else b""
            if x0 or y0 or len(raw) != w * h * 4:
                return None
            W, H = round(w * scale), round(h * scale)
            img = Image.frombytes("RGBA", (w, h), raw).convert("RGBa").resize((W, H), Image.LANCZOS).convert("RGBA")
            new[member] = ifs.lz77_do(img.tobytes())
            inset = (uv[2][0] - x0, x1 - uv[2][1], uv[2][2] - y0, y1 - uv[2][3])
            rect[2] = (0, 2 * W, 0, 2 * H)
            uv[2] = (inset[0], 2 * W - inset[1], inset[2], 2 * H - inset[3])
            size[0][2] = tuple(1 << (n - 1).bit_length() for n in (W, H))
        new["tex/texturelist.xml"] = ifs.kbin_write(tl, six, enc)
        return p.rebuild(new)


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
    ap.add_argument("--pack", action="append", default=[], metavar="DIR",
                    help="a mod (an Omnimix pack) whose own characters' pictures are made this game's size too, "
                         "into OUT (which then goes before it in the mods' order)")
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
        if (r["flags"] & REMOVED or not r["chara_id"]) and i in BY_PICTURES:
            cid, folder, sort, disp, ver = BY_PICTURES[i]
            c = cid.encode()
            r = {"chara_id": c, "flags": 0, "folder": folder.encode(), "gg": b"gg_" + c, "cs": b"cs_" + c, "icon1": b"cs_" + c,
                 "icon2": b"cs_" + c[:-1] + b"b", "chara_xw": 0, "chara_yh": 0, "display_flags": 0, "flavor_idx": -1,
                 "sort_name": sort.encode("cp932"), "disp_name": disp.encode("cp932"), "file_type": 0, "lapis_shape": 1,
                 "ha": b"ha_" + c, "catchtext": b"", "game_version": ver}
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
    # the older game's pictures are smaller than this game's: the hariai by the slot's size over the
    # older game's (2.3 for the old style, 2 for the newer), the names' 2 times
    copied, small = 0, []
    for i, r, pics in out:
        slot = struct.unpack_from("<H", g.d, g.at(g.charas + i * 128) + 0x38)[0]
        ha_scale = slot / r["chara_xw"] if slot and r["chara_xw"] else 2.0 if r["lapis_shape"] else 2.3
        for k, folder, scale in (("ha", "ha_merge_ifs", ha_scale), ("gg", "chara_name_diff_ifs", 2.0)):
            m = pics[k] + ".ifs"
            if pics[k] in own[k] or m not in pictures[k]:
                continue
            b = pictures[k][m].read(m)
            big = bigger(b, scale) if 1.2 < scale < 4 else None
            if big is None:
                small.append(m)
            os.makedirs(os.path.join(a.out, "tex", "system", folder), exist_ok=True)
            with open(os.path.join(a.out, "tex", "system", folder, m), "wb") as f:
                f.write(big or b)
            copied += 1
    # a pack's own characters (its <chara id> for the slots removed) and their pictures, the older
    # games' size too: scaled copies here, which win over the pack's when this mod is before it
    from_packs = 0
    for pack in a.pack:
        for i, c in songsback.read_charas([pack]).items():
            if not 0 < i < g.ncharas or not names[i]["flags"] & REMOVED:
                continue
            at = g.at(g.charas + i * 128)
            slot, kind = struct.unpack_from("<H", g.d, at + 0x38)[0], struct.unpack_from("<I", g.d, at + 0x74)[0]
            xw = int(c.get("chara_xw") or 0)
            ha_scale = slot / xw if slot and xw else 2.0 if kind == 1 or c.get("lapis_shape") else 2.3
            for k, folders, scale in (("ha", ("ha_merge_ifs",), ha_scale), ("gg", ("chara_name_diff_ifs", "chara_name_new_diff_ifs"), 2.0)):
                for folder in folders:
                    src = os.path.join(pack, "tex", "system", folder, (c.get(k) or "") + ".ifs")
                    if not c.get(k) or not os.path.isfile(src):
                        continue
                    b = open(src, "rb").read()
                    big = bigger(b, scale) if 1.2 < scale < 4 else None
                    if big is None:
                        small.append(os.path.basename(src))
                        break
                    os.makedirs(os.path.join(a.out, "tex", "system", folder), exist_ok=True)
                    with open(os.path.join(a.out, "tex", "system", folder, os.path.basename(src)), "wb") as f:
                        f.write(big)
                    from_packs += 1
                    break
    x = [b"<?xml version='1.0' encoding='shift-jis'?>", b"<database>"]
    for i, r, pics in out:
        x.append(b'  <chara id="%d">' % i)
        for k in ("chara_id", "folder", "gg", "cs", "icon1", "icon2", "sort_name", "disp_name", "ha", "catchtext"):
            x.append(b'    <%s __type="str">%s</%s>' % (k.encode(), esc(r[k]), k.encode()))
        for k, t in (("flags", "u32"), ("display_flags", "u32"), ("file_type", "u32"), ("game_version", "u32"),
                     ("chara_xw", "u16"), ("chara_yh", "u16"), ("flavor_idx", "s16")):
            x.append(b'    <%s __type="%s">%d</%s>' % (k.encode(), t.encode(), r[k], k.encode()))
        # the gem's shape in popnhax's terms (dia 1; the others by number): set for the newer characters,
        # whose hariai is the newer kind in the 64-bit game (its +0x74)
        shape = b"dia" if r["lapis_shape"] == 1 else b"%d" % r["lapis_shape"] if r["lapis_shape"] else b""
        x.append(b'    <lapis_shape __type="str">%s</lapis_shape>' % shape)
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
    print("pictures copied from the older game (hariai, names) and made this game's size: %d" % copied)
    if a.pack:
        print("the packs' own characters' pictures made this game's size: %d (put this mod before the packs)" % from_packs)
    if small:
        print("left at the older game's size (not one picture a texture, or not argb8888rev): %d (%s)" % (len(small), ", ".join(small[:8])))
    if no_pic:
        print("a picture of theirs isn't in the older game's tex/system/ha_*.ifs or chara_name*.ifs: %d (%s); they come back when "
              "another mod has it (tex/system/ha_merge_ifs/, chara_name_diff_ifs/)" % (len(no_pic), ", ".join(n for _, n in no_pic[:10])))
    if missing:
        print("the older game has them, the 64-bit game's data hasn't their pictures: %d (%s)" %
              (len(missing), ", ".join(n for _, n in missing[:10]) + (", ..." if len(missing) > 10 else "")))


if __name__ == "__main__":
    main()
