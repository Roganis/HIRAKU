#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The Mickey Tunes! songs' own characters for the 64-bit PC pop'n (docs/SONGS-BACK.md): from the
player's pop'n music Mickey Tunes! program disc (Firebeat, 976 JBA), a mod folder with each song's
dancer as this game's character (and Mickey and Minnie, the player's, for the two songs whose
dancers the disc hasn't), its pictures and hariai, and the songs (the Omnimix pack's, found by their
titles) given them in place of the pack's stand-in.

  tools/fbcharas.py [--pack DIR] DISC OUT

DISC: the program disc's .cue (or .bin/.iso). DIR: the pack whose songs get them (default
out/omnimix-mod; its music data's titles say which song is which). OUT: the mod folder to write; it
goes before the pack in the launcher's Mods… (the first mod's music data wins).

On the disc each song's dancer is a sheet of pictures (/NAME.GCZ: u32 LE size, Konami's LZSS; "GC",
width and height at 12, RGB555 big-endian, bit 15 opaque), and the program has, by sheet, its
animations (start, steps), the steps (picture, 60ths of a second shown), the pictures (first piece,
pieces) and the pieces (where from the anchor, size, where on the sheet): 16-byte records, in data
the program copies to 0x80100000 as it starts. Each picture made whole, 2.5 times bigger, on the old
characters' 320 x 640 stage (tools/afp.py writes their animations), the hariai from the biggest
picture of the dancer's cheer.
"""
import argparse
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import afp  # noqa: E402
import fbexe  # noqa: E402
import ifs  # noqa: E402

DATA = 0x80073BC4   # what the program copies to 0x80100000 (976 JBA)
# sheet: the character's id and name, its kind: a song's dancer, or the player's (Mickey, Minnie)
SHEETS = {
    "SWORLD": ("mt_scrooge", "スクルージ", "song"),
    "RIO": ("mt_rio", "デイジー", "song"),
    "EPARADE": ("mt_nephews", "ヒューイ・デューイ・ルーイ", "song"),
    "CRAZY": ("mt_donald_skate", "ドナルド", "song"),
    "ZIPA": ("mt_goofy", "グーフィー", "song"),
    "DANCE": ("mt_daisy", "デイジー", "song"),
    "TOTALLY": ("mt_pluto", "プルート", "song"),
    "TRAP": ("mt_beagle", "ビーグル・ボーイズ", "song"),
    "MMARCH": ("mt_donald_band", "ドナルド", "song"),
    "MMANIA": ("mt_chipdale", "チップとデール", "song"),
    "TURKEY": ("mt_grandma", "グランマ・ダック", "song"),
    "DDD": ("mt_donald", "ドナルド", "song"),
    "MICKEY": ("mt_mickey", "ミッキー", "player"),
    "MINNIE": ("mt_minnie", "ミニー", "player"),
}
# the songs (their titles in the pack's music data) and their characters, as the game's song cards
# (SS_00) name the actors: the two whose dancers this disc hasn't, Mickey and Minnie
SONGS = {
    "イッツ・ア・スモール・ワールド": ("SWORLD", "SWORLD"),
    "ウェルカム・トゥー・リオ": ("RIO", "RIO"),
    "エレクトリカルパレード": ("EPARADE", "EPARADE"),
    "クレイジー・バウト・ザ・マウス": ("CRAZY", "CRAZY"),
    "ジッパ・ディー・ドゥー・ダー": ("ZIPA", "ZIPA"),
    "ダンス・ウイズ・ミッキー": ("DANCE", "DANCE"),
    "トータリー・ミニー": ("TOTALLY", "TOTALLY"),
    "マウストラップ": ("TRAP", "TRAP"),
    "ミッキーマウス・マーチ": ("MMARCH", "MMARCH"),
    "ミッキー・マニア": ("MMANIA", "MMANIA"),
    "わらの中の七面鳥": ("TURKEY", "TURKEY"),
    "D.D.D!": ("DDD", "DDD"),
    "ザ・グレイテスト・バンド": ("MICKEY", "MINNIE"),
    "アイス・アイス・ミッキー": ("MICKEY", "MINNIE"),
}
# the game's animations from the sheet's: a dancer's (0 the dance, 1 in trouble, 2 lost, 3 a
# stumble, 4 good, 5 great, 6 the cheer, 7 cheering on); the player's (0 drumming, 1 drumming on,
# 2 the hat taken off, 3 the hat trick, 4 back to the drum: nothing for a miss)
ANIMS = {
    "song": [("fev", [0]), ("goo", [4]), ("gre", [5]), ("jam1", [3, 1]), ("jam2", [1]), ("los", [2]),
             ("mis", [3]), ("neu", [0]), ("stand", None), ("win1", [6, 7]), ("win2", [7])],
    "player": [("fev", [0]), ("goo", [1]), ("gre", [2, 3, 4]), ("jam1", [4]), ("jam2", [4]), ("los", [4]),
               ("mis", [4]), ("neu", [0]), ("stand", None), ("win1", [2, 3]), ("win2", [3])],
}
# the removed characters' slots this goes in (their sizes 0: the loader takes the mod's)
SLOTS = [1502, 1809, 1810, 2040, 2041, 2042, 2043, 2284, 2285, 2286, 2287, 2442, 2443, 2444, 2471, 2472]
FOLDER = "mickeytunes"
SCALE = 2.5
HARIAI = (572, 738)   # the old characters' (a pixel's border round)
NAME = (262, 112)
ICON = (162, 162)


class Program:
    def __init__(self, disc):
        d = fbexe.Disc(disc)
        iso = fbexe.Iso9660(d)
        self.iso, self.files = iso, {e.name: e for e in iso.listdir(iso.root)}
        self.img = fbexe.unpack_exe(iso.read_file(self.files[next(n for n in self.files if n.startswith("FIREBEAT"))])).image

    def at(self, a):
        return a - 0x80000000 if a < 0x80100000 else a - 0x80100000 + DATA - 0x80000000

    def u(self, fmt, a):
        return struct.unpack_from(fmt, self.img, self.at(a))

    def sheet(self, name):
        """the sheet's animations: [[(picture, 60ths)]], and its pictures: picture -> [pieces]"""
        s = self.img.find(b"/" + name.encode() + b".GCZ\0")
        ref = self.img.find(struct.pack(">I", 0x80000000 + s), 0, 0x100000) if s >= 0 else -1
        while ref >= 0 and ref % 4:
            ref = self.img.find(struct.pack(">I", 0x80000000 + s), ref + 1, 0x100000)
        if ref < 0:
            raise SystemExit(f"{name}: not in the program (not the Mickey Tunes! disc this knows, 976 JBA)")
        a, b, c, d = struct.unpack_from(">4I", self.img, ref + 8)
        anims = []
        for i in range((c - a) // 4):
            first, n = self.u(">hh", a + 4 * i)
            anims.append([self.u(">hh", b + 12 * k) for k in range(first, first + n)])
        pictures = {}
        for steps in anims:
            for pic, _ in steps:
                if pic not in pictures:
                    first, n = self.u(">hh", d + 4 * pic)
                    pictures[pic] = [self.u(">8h", c + 16 * k) for k in range(first, first + n)]
        if not anims or not all(pictures[pic] for pic, _ in anims[0]):
            raise SystemExit(f"{name}: its tables don't read right (not the Mickey Tunes! disc this knows, 976 JBA)")
        return anims, pictures

    def picture_sheet(self, name):
        from PIL import Image
        raw = self.iso.read_file(self.files[name + ".GCZ"])
        raw = fbexe.lzss_unpack(raw[4:], struct.unpack_from("<I", raw)[0])
        w, h = struct.unpack_from(">HH", raw, 12)
        px = struct.unpack_from(">%dH" % (w * h), raw, 24)
        out = bytearray(w * h * 4)
        for i, v in enumerate(px):
            if v & 0x8000:
                out[4 * i:4 * i + 4] = bytes((((v >> 10) & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, (v & 31) * 255 // 31, 255))
        return Image.frombytes("RGBA", (w, h), bytes(out))


def whole(sheet, pieces, scale):
    """a picture on the stage (with its border), its anchor (the feet) at the bottom middle"""
    from PIL import Image
    W, H = afp.STAGE
    small = Image.new("RGBA", (round(W / scale), round(H / scale)))
    for dx, dy, w, h, _, sx, sy, _ in pieces:
        if w > 0 and h > 0:
            small.alpha_composite(sheet.crop((sx, sy, sx + w, sy + h)), (small.width // 2 + dx, small.height + dy))
    big = small.convert("RGBa").resize((W, H), Image.LANCZOS).convert("RGBA")
    out = Image.new("RGBA", (W + 2, H + 2))
    out.paste(big, (1, 1))
    return out


def fit(img, size, bottom=True):
    """img (its own part) as big as fits size less a border, in its middle or on its bottom"""
    from PIL import Image
    img = img.crop(img.getbbox())
    W, H = size[0] - 2, size[1] - 2
    k = min(W / img.width, H / img.height)
    img = img.convert("RGBa").resize((max(1, round(img.width * k)), max(1, round(img.height * k))), Image.LANCZOS).convert("RGBA")
    out = Image.new("RGBA", size)
    out.paste(img, (1 + (W - img.width) // 2, 1 + (H - img.height if bottom else (H - img.height) // 2)))
    return out


def name_picture(text):
    from PIL import Image, ImageDraw, ImageFont
    W, H = NAME
    font_file = "/usr/share/fonts/noto-cjk/NotoSansCJK-Black.ttc"
    size = 64
    while True:
        try:
            font = ImageFont.truetype(font_file, size, index=0)
        except OSError:
            font = ImageFont.load_default()
        box = ImageDraw.Draw(Image.new("RGBA", (1, 1))).textbbox((0, 0), text, font=font, stroke_width=6)
        if box[2] - box[0] <= W - 12 or size <= 16:
            break
        size -= 2
    img = Image.new("RGBA", (W, H))
    d = ImageDraw.Draw(img)
    x = (W - (box[2] - box[0])) // 2 - box[0]
    y = (H - (box[3] - box[1])) // 2 - box[1]
    d.text((x, y), text, font=font, fill=(255, 255, 255, 255), stroke_width=6, stroke_fill=(40, 40, 120, 255))
    return img


def picture_pack(name, img, tree=1042):
    """a pack of one picture (the hariai's, the name's)"""
    tl, _ = afp.texturelist([(name, img.width, img.height)], size=max(1 << (max(img.size) - 1).bit_length(), 16))
    return ifs.write([("tex/" + afp.md5(name), ifs.lz77_do(img.tobytes("raw", "BGRA"))), ("tex/texturelist.xml", ifs.kbin_write(tl)),
                      ("magic", b"NGPF")], tree=tree)


def character(prog, sheet_name, chara_id, names, kind):
    from PIL import Image
    anims, pieces = prog.sheet(sheet_name)
    sheet = prog.picture_sheet(sheet_name)
    reach = max(max(abs(dx), abs(dx + w)) for ps in pieces.values() for dx, dy, w, h, *_ in ps)
    tall = max(-dy for ps in pieces.values() for dx, dy, *_ in ps)
    scale = min(SCALE, afp.STAGE[0] / 2 / reach, afp.STAGE[1] / tall)
    shows = {}
    for name, which in ANIMS[kind]:
        steps = [anims[0][0]] if which is None else [s for k in which for s in anims[k]]
        out, t = [], 0
        for pic, dur in steps:
            f = round(t / 2)
            if out and out[-1][0] == f:
                out[-1] = (f, pic)
            elif not out or out[-1][1] != pic:
                out.append((f, pic))
            t += dur
        shows[name] = (out, max(1, round(t / 2)) if which else 37)
    used = sorted({p for s, _ in shows.values() for _, p in s})
    pics = {p: whole(sheet, pieces[p], scale) for p in used}
    # the icon (its face: the top of the dance's first picture) and the name's picture
    first = pics[shows["neu"][0][0][1]]
    bx = first.getbbox()
    side = min(bx[2] - bx[0], (bx[3] - bx[1]) // 2)
    cx = (bx[0] + bx[2]) // 2
    icon = fit(first.crop((cx - side // 2, bx[1], cx + side // 2, bx[1] + side)), ICON, bottom=False)
    gg = name_picture(names)
    images = [("%03d" % p, 322, 642) for p in used] + [("cs_a", *ICON), ("cs_b", *ICON), ("gg_" + chara_id, *NAME)]
    tl, uv = afp.texturelist(images)
    members = []
    for p in used:
        members.append(("tex/" + afp.md5("%03d" % p), ifs.lz77_do(pics[p].tobytes("raw", "BGRA"))))
    for n, img in (("cs_a", icon), ("cs_b", icon), ("gg_" + chara_id, gg)):
        members.append(("tex/" + afp.md5(n), ifs.lz77_do(img.tobytes("raw", "BGRA"))))
    members.append(("tex/texturelist.xml", ifs.kbin_write(tl)))
    members.append(("magic", b"NGPF"))
    lists, geos, bsis, afps = [], [], [], []
    for name, _ in ANIMS[kind]:
        out, frames = shows[name]
        ids = {}
        for _, p in out:
            ids.setdefault(p, 10 + 3 * len(ids))
        sprite = 10 + 3 * len(ids) + 10
        a, b = afp.flipbook(name, frames, [(f, ids[p], 2 * (k + 1)) for k, (f, p) in enumerate(out)], list(ids.values()), sprite)
        afps.append(("afp/" + afp.md5(name), a))
        bsis.append(("afp/bsi/" + afp.md5(name), b))
        geos.append(("geo/" + afp.md5(f"{name}_shape5"), afp.MASK))
        for p, sid in ids.items():
            geos.append(("geo/" + afp.md5(f"{name}_shape{sid}"), afp.ge2d("%03d" % p, 320.0, 640.0, uv["%03d" % p])))
        lists.append((name, [5] + list(ids.values())))
    members += bsis + afps + [("afp/afplist.xml", ifs.kbin_write(afp.afplist(lists)))] + geos
    # the hariai: the biggest picture of the cheer
    cheer = [p for _, p in shows["win1"][0]]
    big = max(cheer, key=lambda p: (lambda b: (b[2] - b[0]) * (b[3] - b[1]))(pics[p].getbbox() or (0, 0, 0, 0)))
    hariai = fit(pics[big], HARIAI)
    hb = hariai.getbbox()
    face = ((hb[0] + hb[2]) // 2, hb[1] + (hb[3] - hb[1]) // 5)
    return ifs.write(members), hariai, gg, face


def pack_songs(pack):
    """title -> song id, from the pack's music data"""
    ids = {}
    for root, _, files in os.walk(pack):
        for f in files:
            if f.endswith(".xml"):
                t = open(os.path.join(root, f), "rb").read().decode("cp932", "replace")
                for m in re.finditer(r'<music id="(\d+)">.*?<title __type="str">(.*?)</title>', t, re.S):
                    ids.setdefault(m.group(2), int(m.group(1)))
    return ids


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--pack", default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "out", "omnimix-mod"))
    ap.add_argument("disc")
    ap.add_argument("out")
    a = ap.parse_args()
    prog = Program(a.disc)
    songs = pack_songs(a.pack)
    os.makedirs(os.path.join(a.out, "tex", FOLDER), exist_ok=True)
    for sub in ("ha_merge_ifs", "chara_name_diff_ifs"):
        os.makedirs(os.path.join(a.out, "tex", "system", sub), exist_ok=True)
    charas, music = [], []
    for slot, (sheet, (cid, names, kind)) in zip(SLOTS, SHEETS.items()):
        pack, hariai, gg, face = character(prog, sheet, cid, names, kind)
        open(os.path.join(a.out, "tex", FOLDER, cid + ".ifs"), "wb").write(pack)
        open(os.path.join(a.out, "tex", "system", "ha_merge_ifs", "ha_" + cid + ".ifs"), "wb").write(picture_pack("ha_" + cid, hariai))
        open(os.path.join(a.out, "tex", "system", "chara_name_diff_ifs", "gg_" + cid + ".ifs"), "wb").write(picture_pack("gg_" + cid, gg))
        charas.append(f"""  <chara id="{slot}">
    <chara_id __type="str">{cid}</chara_id>
    <folder __type="str">{FOLDER}</folder>
    <gg __type="str">gg_{cid}</gg>
    <cs __type="str">cs_{cid}</cs>
    <icon1 __type="str">cs_{cid}_a</icon1>
    <icon2 __type="str">cs_{cid}_b</icon2>
    <sort_name __type="str">{esc(names)}</sort_name>
    <disp_name __type="str">{esc(names)}</disp_name>
    <ha __type="str">ha_{cid}</ha>
    <catchtext __type="str"></catchtext>
    <flags __type="u32">0</flags>
    <display_flags __type="u32">0</display_flags>
    <file_type __type="u32">0</file_type>
    <game_version __type="u32">8</game_version>
    <chara_xw __type="u16">{round(face[0] / 2.3)}</chara_xw>
    <chara_yh __type="u16">{round(face[1] / 2.3)}</chara_yh>
    <flavor_idx __type="s16">-1</flavor_idx>
    <lapis_shape __type="str"></lapis_shape>
  </chara>""")
        print(f"{sheet}: {cid} (slot {slot})")
    for title, (one, two) in SONGS.items():
        sid = songs.get(title)
        if sid is None:
            print(f"no song titled {title} in {a.pack}: none given {SHEETS[one][0]}")
            continue
        # its character's hariai in the music select (the pack's own song card is an older game's size)
        music.append(f"""  <music id="{sid}">
    <chara1 __type="str">{SHEETS[one][0]}</chara1>
    <chara2 __type="str">{SHEETS[two][0]}</chara2>
    <ha __type="str"></ha>
  </music>""")
        print(f"song {sid}, {title}: {SHEETS[one][0]}, {SHEETS[two][0]}")
    with open(os.path.join(a.out, "charadb_mickeytunes.xml"), "w", encoding="utf-8") as f:
        f.write("<?xml version='1.0' encoding='utf-8'?>\n<database>\n" + "\n".join(charas + music) + "\n</database>\n")
    with open(os.path.join(a.out, "mod.toml"), "w", encoding="utf-8") as f:
        f.write('name = "Mickey Tunes characters"\ngame = "popn29"\n'
                'description = "The Mickey Tunes! songs\' own dancers, from the Firebeat disc (before Omnimix)"\n')


if __name__ == "__main__":
    main()
