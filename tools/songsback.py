#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check Omnimix packs for the 64-bit PC pop'n before playing (docs/SONGS-BACK.md): which songs the
game left out the packs bring back, and why the others don't. Reads, writes nothing.

  tools/songsback.py [--game DIR] MOD [MOD...]

MOD: a mod folder (popnhax music data: .xml files with <music id> entries, sd/ with the songs'
packs, tex/system/kc_diff_ifs/ with their banners), in the launcher's order (the first wins).
DIR: the game's folder (modules/, plain_data/ or data/); by default dumps/popn29/machine.txt's.

What it decides is what the loader decides (runtime/pc64/songs.c): the game's music table found by
the code that reads it, a song left out (its mask 0x80000000) brought back when the music data
gives its genre, title and artist and its Normal chart's file is there, and its other charts
(Easy, Hyper, EX) aren't missing files, and it has a banner (the game's, or the mod's).
"""
import argparse
import os
import re
import struct
import sys

SLOTS = ("ep", "np", "hp", "op", "bp_n", "bp_h", "bp_ex")
REMOVED = 0x80000000
RECORD = 312


def pe_sections(d):
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    n, opt = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
    base = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
    sec = pe + 24 + opt
    return base, [struct.unpack_from("<8sIIIII", d, sec + 40 * i)[:5] + (struct.unpack_from("<I", d, sec + 40 * i + 36)[0],) for i in range(n)]


class Game:
    """The game's program, its tables found by the code that reads them"""

    def __init__(self, dll):
        self.d = d = open(dll, "rb").read()
        self.base, self.secs = pe_sections(d)
        code = [s for s in self.secs if s[5] & 0x20]
        if not code:
            raise SystemExit(dll + ": no code")

        def find(pat):
            rx = re.compile(b"".join(b"." if x == "X" else re.escape(bytes([int(x, 16)])) for x in pat.split()), re.S)
            hits = []
            for name, vs, va, rs, ro, ch in code:
                hits += [(va, ro, m.start()) for m in rx.finditer(d, ro, ro + rs)]
            return hits

        def rva(va, ro, at):   # the RVA of the file offset at, in that section
            return va + at - ro

        m = find("79 07 33 c0 48 83 c4 28 c3 80 3d X X X X 00 75 X 48 69 c1 38 01 00 00 48 8d 0d X X X X 8b 84 08")
        s = find("8d 41 55 ba X X 00 00 66 3b c2")
        c = find("48 8d 0d X X X X 4b 8d 04 80 48 8b 04 c1")
        ch = find("48 c1 e2 07 48 8d 05 X X X X 41 b8 00 01 00 00 48 8b 14 02")
        nch = find("ba X X 00 00 b8 X X 00 00 66 3b ca 66 0f 46 c1 c3")
        if len(m) != 1 or len(s) != 1 or not c or len(ch) != 1 or len(nch) != 1:
            raise SystemExit(dll + ": the game's tables not found (not the 64-bit PC pop'n, or another build)")
        va, ro, at = m[0]
        self.music = struct.unpack_from("<i", d, at + 35)[0] - 60   # an RVA (the image's base + mask offset)
        self.songs = struct.unpack_from("<i", d, s[0][2] + 4)[0] - 0x55 + 1
        self.charts = min(rva(va, ro, at) + 7 + struct.unpack_from("<i", d, at + 3)[0] - 8 for va, ro, at in c)
        va, ro, at = ch[0]
        self.charas = rva(va, ro, at) + 11 + struct.unpack_from("<i", d, at + 7)[0]
        self.ncharas = struct.unpack_from("<i", d, nch[0][2] + 1)[0] + 1

    def at(self, rva):
        for name, vs, va, rs, ro, ch in self.secs:
            if va <= rva < va + max(vs, rs):
                return ro + rva - va
        return None

    def text(self, ptr):
        o = self.at(ptr - self.base) if ptr else None
        return self.d[o:self.d.index(b"\0", o)].decode("cp932", "replace") if o is not None else None

    def record(self, i):
        return self.d[self.at(self.music + i * RECORD):][:RECORD]

    def chara(self, i):
        """a character's name, folder and flags (bit 0: removed, its slot kept)"""
        o = self.at(self.charas + i * 128)
        q = lambda k: struct.unpack_from("<Q", self.d, o + k)[0]
        return {"chara_id": self.text(q(0)) or "", "folder": self.text(q(0x10)) or "", "gg": self.text(q(0x18)) or "",
                "ha": self.text(q(0x60)) or "", "flags": struct.unpack_from("<I", self.d, o + 8)[0]}

    def character_names(self, back=()):
        """the names of the game's characters and of those the mods describe again (back: their ids)"""
        names = set()
        for i in range(self.ncharas):
            c = self.chara(i)
            if c["chara_id"] and not c["flags"] & 1:
                names.add(c["chara_id"])
        return names | set(back.values()) if back else names


def tag(body, name):
    m = re.search(r"<%s(?:\s[^>]*)?(?:/>|>(.*?)</%s>)" % (name, name), body, re.S)
    if not m:
        return None
    v = m.group(1) or ""
    for a, b in (("&lt;", "<"), ("&gt;", ">"), ("&quot;", '"'), ("&apos;", "'"), ("&amp;", "&")):
        v = v.replace(a, b)
    return v


MAX_IDS = 32768   # the music data's ids the loader keeps (past the game's table, its tables grow to them)


def read_music(mods):
    """id -> the song's fields, the first mod's winning (read from the last mod to the first, each
    mod's files in their names' order)"""
    songs = {}
    for mod in reversed(mods):
        files = []
        for root, dirs, names in os.walk(mod):
            dirs[:] = [x for x in dirs if not x.startswith(".")]
            files += [os.path.join(root, n) for n in names if n.lower().endswith(".xml")]
        for f in sorted(files, key=str.lower):
            raw = open(f, "rb").read()
            # UTF-8 unless it says Shift-JIS, and then too when it's valid UTF-8 (a pack's file says
            # shift-jis and isn't), as the loader reads it
            sj = re.search(rb"encoding=.(shift.jis|cp932|windows-31j)", raw[:200], re.I)
            try:
                text = raw.decode("utf-8")
            except UnicodeDecodeError:
                text = raw.decode("cp932" if sj else "utf-8", "replace")
            text = re.sub(r"<!--.*?-->", "", text, flags=re.S)
            for mid, body in re.findall(r'<music\s+id=["\'](\d+)["\']\s*>(.*?)</music>', text, re.S):
                i = int(mid)
                if i >= MAX_IDS:
                    continue
                s = songs.setdefault(i, {"charts": {}})
                own = body.split("<chart ", 1)[0]
                for k in ("genre", "title", "artist", "chara1", "chara2"):
                    v = tag(own, k)
                    if v is not None:
                        s[k] = v
                for slot, cbody in re.findall(r'<chart\s+idx=["\'](\w+)["\']\s*>(.*?)</chart>', body, re.S):
                    folder, file = tag(cbody, "folder"), tag(cbody, "filename")
                    if slot in SLOTS and folder and file:
                        s["charts"][slot] = (folder, file, tag(cbody, "diff"))
    return songs


def read_charas(mods):
    """id -> a character's name and folder as the mods' music data describes it (popnhax's <chara id>)"""
    charas = {}
    for mod in reversed(mods):
        files = []
        for root, dirs, names in os.walk(mod):
            dirs[:] = [x for x in dirs if not x.startswith(".")]
            files += [os.path.join(root, n) for n in names if n.lower().endswith(".xml")]
        for f in sorted(files, key=str.lower):
            raw = open(f, "rb").read()
            sj = re.search(rb"encoding=.(shift.jis|cp932|windows-31j)", raw[:200], re.I)
            try:
                text = raw.decode("utf-8")
            except UnicodeDecodeError:
                text = raw.decode("cp932" if sj else "utf-8", "replace")
            for cid, body in re.findall(r'<chara\s+id=["\'](\d+)["\']\s*>(.*?)</chara>', text, re.S):
                c = charas.setdefault(int(cid), {})
                for k in ("chara_id", "folder", "gg", "ha"):
                    v = tag(body, k)
                    if v is not None:
                        c[k] = v
    return charas


def charas_back(g, mods, data):
    """id -> name: the characters the game removed that the mods describe again, their pictures there
    (tex/FOLDER/NAME.ifs, the hariai and the name's picture: a mod's, or the game's own characters')"""
    own = [g.chara(i) for i in range(g.ncharas)]
    own = {k: {c[k] for c in own if c[k] and not c["flags"] & 1} for k in ("ha", "gg")}

    def picture(k, folders, name):
        return name in own[k] or any(exists(mods, data, "tex/system/%s/%s.ifs" % (f, name)) for f in folders)
    back = {}
    for i, c in read_charas(mods).items():
        if 0 < i < g.ncharas and c.get("chara_id") and c.get("folder") and g.chara(i)["flags"] & 1 and \
                exists(mods, data, "tex/%s/%s.ifs" % (c["folder"], c["chara_id"])) and \
                picture("ha", ("ha_merge_ifs", "ha_merge_jka_ifs"), c.get("ha", "")) and \
                picture("gg", ("chara_name_diff_ifs", "chara_name_new_diff_ifs", "chara_name_jka_ifs"), c.get("gg", "")):
            back[i] = c["chara_id"]
    return back


def game_folder(arg):
    """the 64-bit game's folder: arg, or dumps/popn29/machine.txt's"""
    if arg:
        return arg
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    mt = os.path.join(here, "dumps", "popn29", "machine.txt")
    for line in open(mt, encoding="utf-8") if os.path.exists(mt) else []:
        if line.startswith("contents="):
            return line.split("=", 1)[1].strip()
    raise SystemExit("the game's folder: --game DIR (no dumps/popn29/machine.txt)")


def game_banners(data):
    """the song numbers whose banner the game's own packs hold (tex/system/kc*.ifs: kc_NNNN inside)"""
    ids = set()
    folder = os.path.join(data, "tex", "system")
    for f in os.listdir(folder) if os.path.isdir(folder) else []:
        if f.startswith("kc") and f.endswith(".ifs"):
            ids |= {int(m) for m in re.findall(rb"kc_(\d{4})(?!\d)", open(os.path.join(folder, f), "rb").read())}
    return ids


def exists(mods, data, rel):
    return any(os.path.isfile(os.path.join(m, rel)) for m in mods) or os.path.isfile(os.path.join(data, rel))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--game", help="the game's folder (modules/, plain_data/ or data/)")
    ap.add_argument("mods", nargs="+")
    ap.add_argument("-v", "--verbose", action="store_true", help="every song, not only the totals and the first of each kind")
    a = ap.parse_args()
    game = game_folder(a.game)
    dll = os.path.join(game, "modules", "popn.dll")
    data = os.path.join(game, "plain_data") if os.path.isdir(os.path.join(game, "plain_data")) else os.path.join(game, "data")
    for m in a.mods:
        if not os.path.isdir(m):
            raise SystemExit(m + ": no such folder")
    g = Game(dll)
    left_out = [i for i in range(g.songs) if struct.unpack_from("<I", g.record(i), 60)[0] == REMOVED]
    print("the game: %d songs, %d left out; its characters: %d" % (g.songs, len(left_out), g.ncharas))
    songs = read_music(a.mods)
    # the songs past the game's table: as songs left out, in the slots the loader adds for them
    past = sorted(i for i in songs if i >= g.songs)
    back_charas = charas_back(g, a.mods, data)
    names = g.character_names(back_charas)
    if back_charas:
        print("characters the game removed, described again by the mods: %d" % len(back_charas))
    back, why = [], {}
    no_chara = []
    banners = game_banners(data)
    for i in left_out + past:
        s = songs.get(i)
        if not s:
            continue
        reason = None
        if not all(s.get(k) for k in ("genre", "title", "artist")):
            reason = "no genre, title or artist"
        elif "np" not in s["charts"]:
            reason = "no Normal chart"
        else:
            for slot, (folder, file, diff) in s["charts"].items():
                if "/" in folder + file or "\\" in folder + file or ".." in folder + file:
                    continue
                if not exists(a.mods, data, "sd/%s/%s.ifs" % (folder, file)) and slot in ("ep", "np", "hp", "op"):
                    reason = "missing sd/%s/%s.ifs (%s)" % (folder, file, slot)
                    break
        if not reason and i not in banners and not any(
                exists(a.mods, data, "tex/system/%s/kc_%04d.ifs" % (d, i)) for d in ("kc_diff_ifs", "kc_jka_diff_ifs", "kc_jka_ifs")):
            reason = "no banner (tex/system/kc_diff_ifs/kc_%04d.ifs)" % i
        if reason:
            why.setdefault(reason.split(" (")[0].split(" sd/")[0], []).append((i, s.get("title"), reason))
            continue
        back.append((i, s["title"]))
        missing = [s[k] for k in ("chara1", "chara2") if s.get(k) and s[k] != "0" and s[k] not in names]
        if missing:
            no_chara.append((i, s["title"], ", ".join(missing), len(missing) == 2))
    print("brought back: %d songs" % len(back))
    for i, t in back if a.verbose else back[:10]:
        print("  %5d  %s" % (i, t))
    if len(back) > 10 and not a.verbose:
        print("  ... (-v: all)")
    for reason, l in sorted(why.items()):
        print("left out, %s: %d" % (reason, len(l)))
        for i, t, r in l if a.verbose else l[:5]:
            print("  %5d  %s: %s" % (i, t, r))
    if no_chara:
        print("songs with a character the game hasn't: %d (shown with their other one; %d with neither, shown with the game's first)" %
              (len(no_chara), sum(1 for x in no_chara if x[3])))
        for i, t, c, both in no_chara if a.verbose else no_chara[:5]:
            print("  %5d  %s: %s" % (i, t, c))
    if past:
        print("of them past the game's table (the console games' songs, custom ones: its tables grow to %d songs): %d" %
              (past[-1] + 1, sum(1 for i, t in back if i >= g.songs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
