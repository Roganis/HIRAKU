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
FOLDER = "mickeytunes"   # the packs' folder (tex/FOLDER/NAME.ifs): this one's, "animelo" Animelo's
SCALE = 2.5
HARIAI = (572, 738)   # the old characters' (a pixel's border round)
NAME = (262, 112)
ICON = (162, 162)


# pop'n music Animelo 2 (Firebeat, A02 JAC): an anime song's character is a few cut-out pictures of
# the series' characters on its sheet `CODE_02.GCZ` (the game shows them in its "DATA" screens, not
# while playing: its play screen has the player's navigator), taken from the rectangles here (each
# trimmed to its pixels): the character's own poses, the first its hariai's. Animelo moves them about, it doesn't
# animate them: this game's animations are made by moving them so (a bob, hops, a shake, a fall:
# each step a shape of its own, its quad moved, as the old characters' are all shown at one place).
# The songs whose character the game itself has from the same series (the pack names them: db,
# saint, niku, devil, moon) keep it.
ANIMELO = {
    "GE1": ("ゲゲゲの鬼太郎", "an_kitaro", "鬼太郎", [(0, 0, 470, 384)]),
    "GET": ("ゲッターロボ！", "an_getter", "ゲッター1", [(512, 0, 800, 384)]),
    "GIN": ("銀河鉄道999", "an_maetel", "メーテル", [(600, 0, 900, 480)]),
    "RUN": ("花の子ルンルン", "an_lunlun", "ルンルン", [(730, 585, 1024, 1024)]),
    "CAP": ("キャプテンハーロック", "an_harlock", "ハーロック", [(130, 645, 500, 1024)]),
    "AK1": ("ひみつのアッコちゃん", "an_akko", "アッコちゃん", [(490, 386, 690, 770), (690, 386, 870, 770)]),
    "GE2": ("カランコロンの歌", "an_kitaro2", "鬼太郎", [(0, 0, 420, 384)]),
    "SAR": ("魔法使いサリー", "an_sally", "サリー", [(0, 0, 210, 300), (0, 300, 150, 600)]),
    "TIG": ("タイガーマスク", "an_tiger", "タイガーマスク", [(0, 180, 340, 640)]),
    "SLP": ("ワイワイワールド", "an_arale", "アラレちゃん", [(0, 256, 500, 640), (500, 230, 900, 530)]),
    "ENM": ("ドロロンえん魔くん", "an_enma", "えん魔くん", [(512, 0, 960, 384)]),
    "GRM": ("グレートマジンガー", "an_greatmazin", "グレートマジンガー", [(540, 60, 1024, 1024)]),
    "LAL": ("ハロー・ララベル", "an_lalabel", "ララベル", [(0, 0, 505, 960), (512, 0, 1024, 520)]),
    "WI2": ("異次元ストーリー", "an_aoi", "あおい", [(0, 600, 850, 980)]),
    "MAY": ("メイプルタウン物語", "an_patty", "パティ", [(0, 200, 170, 387), (0, 571, 170, 757), (0, 760, 170, 950)]),
    "UFO": ("とべ！グレンダイザー", "an_duke", "デューク・フリード", [(370, 380, 700, 740), (0, 380, 370, 740)]),
    "MEM": ("とんがり帽子のメモル", "an_memol", "メモル", [(0, 386, 330, 780)]),
    "KIN": ("わぴこ元気予報！", "an_wapiko", "わぴこ", [(512, 380, 1000, 765)]),
    "WI1": ("WING LOVE", "an_wingman", "ウイングマン", [(100, 0, 400, 380), (560, 0, 900, 380)]),
}
# the removed characters' slots left after Mickey Tunes', then places past the game's table
ANIMELO_SLOTS = [1144, 2138, 2320, 2471, 2472] + list(range(2508, 2508 + 14))
# the moves: (picture, frames held, x and y off the stage's middle, in pixels), the frames at 30 a
# second; P0 the character's first picture, P1 its second (else its first)
def _bob(p, h=8, hold=8):
    return [(p, hold, 0, 0), (p, hold, 0, -h // 2), (p, hold, 0, -h), (p, hold, 0, -h // 2)]
def _hop(p, h, hold=3):
    return [(p, hold, 0, 0), (p, hold, 0, -h // 2), (p, hold, 0, -h), (p, hold, 0, -h // 2), (p, hold, 0, 0)]
def _shake(p, w, hold=2):
    return [(p, hold, x, 0) for x in (0, -w, w, -w, w, 0)]
MOVES = {
    "stand": lambda P0, P1: [(P0, 37, 0, 0)],
    "neu": lambda P0, P1: _bob(P0),
    "fev": lambda P0, P1: _bob(P0, h=12, hold=4) + _bob(P1, h=12, hold=4),
    "goo": lambda P0, P1: _hop(P1, 36) + [(P1, 10, 0, 0)],
    "gre": lambda P0, P1: _hop(P1, 36) + _hop(P1, 36) + [(P1, 10, 0, 0)],
    "mis": lambda P0, P1: _shake(P0, 14) + [(P0, 8, 0, 0)],
    "jam1": lambda P0, P1: _shake(P0, 14) + _shake(P0, 14),
    "jam2": lambda P0, P1: _shake(P0, 14) + [(P0, 10, 0, 0)],
    "los": lambda P0, P1: [(P0, 6, 0, y) for y in (0, 12, 24, 36)] + [(P0, 30, 0, 36)],
    "win1": lambda P0, P1: _hop(P1, 36) + _hop(P1, 36) + _hop(P1, 36) + [(P1, 20, 0, 0)],
    "win2": lambda P0, P1: _hop(P1, 36) + [(P1, 6, 0, 0)],
}


def animelo_character(sheet, rects, chara_id, name):
    """an Animelo character: its pictures each at most 280 x 540 (and 3 times bigger), on the stage's
    bottom less 40 pixels and in its middle; its animations moving them about there (a shape for
    each place: a picture's quad moved)"""
    from PIL import Image
    W, H = afp.STAGE
    pics, places = {}, {}
    for k, r in enumerate(rects):
        part = sheet.crop(r)
        part = part.crop(part.getbbox())
        z = min(3.0, (W - 40) / part.width, (H - 100) / part.height)
        part = part.convert("RGBa").resize((round(part.width * z), round(part.height * z)), Image.LANCZOS).convert("RGBA")
        pic = Image.new("RGBA", (part.width + 2, part.height + 2))
        pic.paste(part, (1, 1))
        pics[k], places[k] = pic, ((W - part.width) // 2, H - 40 - part.height)
    P0, P1 = 0, 1 if len(pics) > 1 else 0
    shows = {}
    for anim, move in MOVES.items():
        out, t = [], 0
        for p, hold, x, y in move(P0, P1):
            if not out or out[-1][1:] != (p, (x, y)):   # (one shape never twice in a row, as the game's)
                out.append((t, p, (x, y)))
            t += hold
        shows[anim] = (out, t)
    return build(pics, shows, chara_id, name, hariai_of=0, places=places)


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


def build(pics, shows, chara_id, names, hariai_of=None, places={}):
    """a character's pack from its pictures ({key: a picture with a pixel's border round}, each the
    stage's size or at places[key] on it) and animations ({name: ([(frame, key[, (x, y) moved by,
    in pixels])], frames)}): each step a shape, its quad the picture's place, moved. Its hariai
    (hariai_of's picture, else the biggest of its cheer), its name's picture and where its face is
    in the hariai"""
    # each animation's shapes: a picture, where it's moved to
    shapes_of = {}
    for name in sorted(shows):
        keys = {}
        for show in shows[name][0]:
            keys.setdefault((show[1],) + tuple(show[2:3]), show[1])
        shapes_of[name] = keys
    images_of = sorted({show[1] for s, _ in shows.values() for show in s})
    names_of = {p: "%03d" % k for k, p in enumerate(images_of)}
    # the icon (its face: the top of the dance's first picture) and the name's picture
    first = pics[shows["neu"][0][0][1]]
    bx = first.getbbox()
    side = min(bx[2] - bx[0], (bx[3] - bx[1]) // 2)
    cx = (bx[0] + bx[2]) // 2
    icon = fit(first.crop((cx - side // 2, bx[1], cx + side // 2, bx[1] + side)), ICON, bottom=False)
    gg = name_picture(names)
    images = [(names_of[p], *pics[p].size) for p in images_of] + [("cs_a", *ICON), ("cs_b", *ICON), ("gg_" + chara_id, *NAME)]
    tl, uv = afp.texturelist(images)
    members = []
    for p in images_of:
        members.append(("tex/" + afp.md5(names_of[p]), ifs.lz77_do(pics[p].tobytes("raw", "BGRA"))))
    for n, img in (("cs_a", icon), ("cs_b", icon), ("gg_" + chara_id, gg)):
        members.append(("tex/" + afp.md5(n), ifs.lz77_do(img.tobytes("raw", "BGRA"))))
    members.append(("tex/texturelist.xml", ifs.kbin_write(tl)))
    members.append(("magic", b"NGPF"))
    lists, geos, bsis, afps = [], [], [], []
    for name in sorted(shows):
        out, frames = shows[name]
        ids = {key: 10 + 3 * k for k, key in enumerate(shapes_of[name])}
        sprite = 10 + 3 * len(ids) + 10
        # each placed picture's object number the frame it ends at (the next one's, the last the
        # animation's length), as the game's own: the player takes it as when to take it off
        ends = [show[0] for show in out[1:]] + [frames]
        steps = [(show[0], ids[(show[1],) + tuple(show[2:3])], ends[k]) for k, show in enumerate(out)]
        a, b = afp.flipbook(name, frames, steps, list(ids.values()), sprite)
        afps.append(("afp/" + afp.md5(name), a))
        bsis.append(("afp/bsi/" + afp.md5(name), b))
        geos.append(("geo/" + afp.md5(f"{name}_shape5"), afp.MASK))
        for key, sid in ids.items():
            n, (w, h) = names_of[shapes_of[name][key]], pics[key[0]].size
            x, y = places.get(key[0], (0, 0))
            dx, dy = key[1] if len(key) > 1 else (0, 0)
            geos.append(("geo/" + afp.md5(f"{name}_shape{sid}"), afp.ge2d(n, w - 2.0, h - 2.0, uv[n], (x + dx, y + dy))))
        lists.append((name, [5] + list(ids.values())))
    members += bsis + afps + [("afp/afplist.xml", ifs.kbin_write(afp.afplist(lists)))] + geos
    cheer = [show[1] for show in shows["win1"][0]]
    big = max(cheer, key=lambda p: (lambda b: (b[2] - b[0]) * (b[3] - b[1]))(pics[p].getbbox() or (0, 0, 0, 0)))
    hariai = fit(pics[big if hariai_of is None else hariai_of], HARIAI)
    hb = hariai.getbbox()
    face = ((hb[0] + hb[2]) // 2, hb[1] + (hb[3] - hb[1]) // 5)
    return ifs.write(members), hariai, gg, face


def character(prog, sheet_name, chara_id, names, kind):
    """a Mickey Tunes dancer"""
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
    used = {show[1] for s, _ in shows.values() for show in s}
    return build({p: whole(sheet, pieces[p], scale) for p in used}, shows, chara_id, names)


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


def chara_entry(slot, cid, names, face, folder=FOLDER):
    return f"""  <chara id="{slot}">
    <chara_id __type="str">{cid}</chara_id>
    <folder __type="str">{folder}</folder>
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
  </chara>"""


def music_entry(sid, one, two):
    # its character's hariai in the music select (a pack's own song card is an older game's size)
    return f"""  <music id="{sid}">
    <chara1 __type="str">{one}</chara1>
    <chara2 __type="str">{two}</chara2>
    <ha __type="str"></ha>
  </music>"""


def write_pictures(out, cid, pack, hariai, gg, folder=FOLDER):
    os.makedirs(os.path.join(out, "tex", folder), exist_ok=True)
    open(os.path.join(out, "tex", folder, cid + ".ifs"), "wb").write(pack)
    open(os.path.join(out, "tex", "system", "ha_merge_ifs", "ha_" + cid + ".ifs"), "wb").write(picture_pack("ha_" + cid, hariai))
    open(os.path.join(out, "tex", "system", "chara_name_diff_ifs", "gg_" + cid + ".ifs"), "wb").write(picture_pack("gg_" + cid, gg))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--pack", default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "out", "omnimix-mod"))
    ap.add_argument("disc")
    ap.add_argument("out")
    a = ap.parse_args()
    prog = Program(a.disc)
    songs = pack_songs(a.pack)
    for sub in ("ha_merge_ifs", "chara_name_diff_ifs"):
        os.makedirs(os.path.join(a.out, "tex", "system", sub), exist_ok=True)
    charas, music = [], []
    if "CRAZY.GCZ" in prog.files:
        what, title, desc = "mickeytunes", "Mickey Tunes characters", "The Mickey Tunes! songs' own dancers, from the Firebeat disc"
        for slot, (sheet, (cid, names, kind)) in zip(SLOTS, SHEETS.items()):
            pack, hariai, gg, face = character(prog, sheet, cid, names, kind)
            write_pictures(a.out, cid, pack, hariai, gg)
            charas.append(chara_entry(slot, cid, names, face))
            print(f"{sheet}: {cid} (slot {slot})")
        for t, (one, two) in SONGS.items():
            if t in songs:
                music.append(music_entry(songs[t], SHEETS[one][0], SHEETS[two][0]))
                print(f"song {songs[t]}, {t}: {SHEETS[one][0]}, {SHEETS[two][0]}")
            else:
                print(f"no song titled {t} in {a.pack}: none given {SHEETS[one][0]}")
    elif "GRM_02.GCZ" in prog.files:
        what, title, desc = "animelo", "Animelo characters", "The Animelo anime songs' own characters, from the Animelo 2 disc"
        for slot, (code, (t, cid, names, rects)) in zip(ANIMELO_SLOTS, ANIMELO.items()):
            pack, hariai, gg, face = animelo_character(prog.picture_sheet(code + "_02"), rects, cid, names)
            write_pictures(a.out, cid, pack, hariai, gg, "animelo")
            charas.append(chara_entry(slot, cid, names, face, "animelo"))
            if t in songs:
                music.append(music_entry(songs[t], cid, cid))
                print(f"{code}: {cid} (slot {slot}) for song {songs[t]}, {t}")
            else:
                print(f"{code}: {cid} (slot {slot}), no song titled {t} in {a.pack}")
    else:
        raise SystemExit(a.disc + ": not a disc this knows (pop'n music Mickey Tunes!, 976 JBA; Animelo 2, A02 JAC)")
    with open(os.path.join(a.out, f"charadb_{what}.xml"), "w", encoding="utf-8") as f:
        f.write("<?xml version='1.0' encoding='utf-8'?>\n<database>\n" + "\n".join(charas + music) + "\n</database>\n")
    with open(os.path.join(a.out, "mod.toml"), "w", encoding="utf-8") as f:
        f.write(f'name = "{title}"\ngame = "popn29"\ndescription = "{desc} (before Omnimix)"\n')


if __name__ == "__main__":
    main()
