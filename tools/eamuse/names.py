#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Song names' packs for hiraku-server (docs/SERVER.md, "Song names"), made from YOUR OWN games: each
game of the server's table that HIRAKU_ROOT has ingested (dumps/ID) has its own song table read in
place, and DIR/ID.json written. The packs are the owner's: never share or commit them.

    tools/eamuse/names.py --root HIRAKU_ROOT --out DIR [--game ID ...]

Prints, per game, how many songs it wrote, or one line why not. Nothing large is unpacked: a CHD is
read through tools/chdread.py, a disk's file system and a program's tables where they lie.

The song numbers are the ones the server keeps scores by (runtime/eamuse/readers.cpp), by these rules:

  pop'n music 9-18: song n is entry n of the game's song table; the player's record keeps a word per
  song at index n of it, and the scores' music number is that index (runtime/eamuse/server_share.cpp).
  The tables (genre and comment texts each; 9 and 10 a title, 17 and 18 a title and an artist):
    9  0x60 bytes an entry, 325 from 0x235180 (boot record C at 0x100000), genre +8, title +12
    10 0x48 bytes, 390 from 0x34cbc0, genre +8, title +12        11 0x38, 450 from 0x328420, genre +0
    12 0x38, 595 from 0x301460                                    13 0x38, 649 from 0x19416f0 (game.bin at 0x173d600)
    14 0x30, 700 from 0x193d7f8 (game.bin at 0x172ed00)           15-18: the program's table (below)
    15 40 bytes, 763 at 0x5084f0   16 40, 839 at 0x523ab8   17 52, 907 at 0x52d028   18 72, 976 at 0x55adc0
  a song taken out is left out: its genre "-" (or "---"), in 15 and 16 its flag 0x80000000 at +16.
  each table checked: every entry's genre a text, the entry past its end none. A game whose table has
  no title (11-16) takes the title, and the artist, of the same song number in a table that has them
  (the later PC games', 18's, 17's, 10's, 9's) when that table's genre for the number is the game's
  own (the same song); else its title is left out, the genre kept as the title.
  The later PC pop'n games (one row, model M39): song n is the music table's id n (the scores' music):
  the 64-bit game's table found as tools/songsback.py finds it, the 32-bit's as runtime/pc32/songs.c;
  each the half-width title, artist and genre (the full-width ones when those are empty).
  beatmania IIDX 9th style: the song number (style x 100 + n) is each record's own (data/info/1.s,
  decrypted as the program does: 0x25c bytes a record, u16 at +0, its ASCII title at +4). 10th style
  to EMPRESS: data/info/eout.bin (decrypted with the program's key), each record's own number (u16 or
  u32), checked by the file's own number-to-record table pointing back at it. Every song has its
  chart folder data/sd_data/NNNN. Their genres and artists are pictures: titles only.
  GuitarFreaks and DrumMania V, V2: the netid the game sends is the music table's number (net_id[i] =
  music_info[i]+0, the program's music_id2net_id); System 573 GuitarFreaks 11th / DrumMania 10th: the
  netid is the song's place in the flash's net_id table (its own binary search). Their titles are
  pictures: the text kept is the name-sort key, at most 15 letters.
"""
import argparse
import json
import os
import re
import struct
import sys
import unicodedata
import zlib

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, TOOLS)

TEXT_MAX = 200


class NotHere(Exception):
    """why a game's pack isn't made: one plain line"""


# --- text

def clean(s):
    s = ''.join(c for c in s if unicodedata.category(c) != 'Cc').strip()
    return s[:TEXT_MAX]


def decode(b, enc='cp932'):
    try:
        return b.decode(enc)
    except UnicodeDecodeError:
        return b.decode(enc, 'replace')


def norm(s):
    """a text as compared between games: compatibility forms, no spaces, case folded, dashes alike"""
    s = unicodedata.normalize('NFKC', s or '').casefold()
    s = re.sub(r'[\s・･]', '', s)
    return re.sub(r'[‐‑‒–—―−ー〜～~\-]', '-', s)


def placeholder(s):
    return re.fullmatch(r'[-?]*', norm(s)) is not None   # ("-", "---": a song taken out)


def cstr(img, base, va, maxlen=400):
    """the NUL-ended text at address va of an image loaded at base (None: not one)"""
    o = va - base
    if va == 0 or not 0 <= o < len(img):
        return None
    e = img.find(b'\0', o, o + maxlen)
    if e < 0:
        return None
    b = img[o:e]
    try:
        t = b.decode('cp932')
    except UnicodeDecodeError:
        return None
    return t if not any(ord(c) < 0x20 for c in t) else None


# --- where a game's files are: dumps/ID/machine.txt

def machine(root, gid):
    d = os.path.join(root, 'dumps', gid)
    path = os.path.join(d, 'machine.txt')
    if not os.path.isfile(path):
        raise NotHere('not ingested (no dumps/%s/machine.txt)' % gid)
    m = {'_dir': d}
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.strip()
        if '=' in line and not line.startswith('#'):
            k, _, v = line.partition('=')
            m[k.strip()] = v.strip()
    return m


def need(path, what):
    if not os.path.exists(path):
        raise NotHere('%s not found (%s)' % (what, path))
    return path


class PE:
    """a Windows program or library as its image: its sections at their addresses"""

    def __init__(self, path):
        d = open(path, 'rb').read()
        if d[:2] != b'MZ':
            raise NotHere('%s is not a Windows program' % os.path.basename(path))
        pe = struct.unpack_from('<I', d, 0x3C)[0]
        n, opt = struct.unpack_from('<HH', d, pe + 6)[0], struct.unpack_from('<H', d, pe + 20)[0]
        magic = struct.unpack_from('<H', d, pe + 24)[0]
        self.base = struct.unpack_from('<Q', d, pe + 48)[0] if magic == 0x20B else struct.unpack_from('<I', d, pe + 52)[0]
        self.secs = [struct.unpack_from('<8sIIII', d, pe + 24 + opt + 40 * i) + (struct.unpack_from('<I', d, pe + 24 + opt + 40 * i + 36)[0],)
                     for i in range(n)]
        size = max(va + max(vs, rs) for _, vs, va, rs, ro, ch in self.secs)
        img = bytearray(size)
        for _, vs, va, rs, ro, ch in self.secs:
            img[va:va + rs] = d[ro:ro + rs]
        self.img = bytes(img)

    def find(self, pattern):
        """a byte pattern ("8b X c3": X any byte) in the code: the addresses of its matches"""
        rx = re.compile(b''.join(b'.' if x == 'X' else re.escape(bytes([int(x, 16)])) for x in pattern.split()), re.S)
        hits = []
        for _, vs, va, rs, ro, ch in self.secs:
            if ch & 0x20:
                hits += [self.base + m.start() for m in rx.finditer(self.img, va, va + max(vs, rs))]
        return hits

    def u32(self, va):
        return struct.unpack_from('<I', self.img, va - self.base)[0]

    def text(self, va):
        return cstr(self.img, self.base, va)


# --- pop'n music 9-18

# id: (where the program is, its load address, the table's first genre pointer, entry size, count,
#      title's and artist's offsets from the genre's, or None)
POPN = {
    'popn9': ('recC', 0x100000, 0x235188, 0x60, 325, 4, None),
    'popn10': ('recC', 0x100000, 0x34cbc8, 0x48, 390, 4, None),
    'popn11': ('recC', 0x100000, 0x328420, 0x38, 450, None, None),
    'popn12': ('recC', 0x100000, 0x301460, 0x38, 595, None, None),
    'popn13': ('game.bin', 0x173d600, 0x19416f0, 0x38, 649, None, None),
    'popn14': ('game.bin', 0x172ed00, 0x193d7f8, 0x30, 700, None, None),
    'popn15': ('exe', 0, 0x5084f0, 40, 763, None, None),
    'popn16': ('exe', 0, 0x523ab8, 40, 839, None, None),
    'popn17': ('exe', 0, 0x52d028, 52, 907, 4, 8),
    'popn18': ('exe', 0, 0x55adc0, 72, 976, 4, 8),
}
POPN_CHARTS = {'0': 'Normal', '1': 'Hyper', '2': '5 Buttons', '3': 'EX'}
POPN_CHARTS_13 = dict(POPN_CHARTS, **{'2': "5 Buttons (pop'n 12's)", '6': '5 Buttons'})   # (13 on: their own 5-button charts are 6)
LATER_CHARTS = {'0': 'Easy', '1': 'Normal', '2': 'Hyper', '3': 'EX'}


def popn_image(root, gid):
    m = machine(root, gid)
    where, base = POPN[gid][0], POPN[gid][1]
    if where == 'exe':
        prog = need(os.path.join(m.get('program', ''), m.get('exe', '')), 'the program')
        pe = PE(prog)
        return pe.img, pe.base
    import pythonfs
    hdd = need(os.path.join(m['_dir'], m.get('hdd', '')), 'the disk')
    if where == 'recC':
        img = pythonfs.open_image(hdd)
        off, size, _ = pythonfs.records(img)['C']
        return pythonfs.lz_decode(img.read(off, size)), base
    import popnfs
    disk = popnfs.Disk(hdd)
    e = disk.lookup('ee/boot/game.bin')
    if not e:
        raise NotHere('no ee/boot/game.bin on the disk')
    return disk.decrypt(disk.raw(e))[0], base


def popn_table(root, gid):
    """[(genre, title or None, artist or None)] by song number, checked"""
    img, base = popn_image(root, gid)
    return read_popn_table(img, base, *POPN[gid][2:])


def read_popn_table(img, base, at, size, count, toff, aoff):
    """a song table of `count` entries of `size` bytes, the first genre pointer at address `at` of an
    image loaded at `base`; title and artist pointers at toff and aoff from the genre's (None: none)"""
    out = []
    for n in range(count + 1):
        va = at + n * size
        if va - base + 20 > len(img):
            break
        g = cstr(img, base, struct.unpack_from('<I', img, va - base)[0])
        if g is not None and size == 40 and struct.unpack_from('<I', img, va - base + 16)[0] & 0x80000000:
            g = '-'   # (pop'n 15 and 16: a song taken out, its flag 0x80000000)
        t = cstr(img, base, struct.unpack_from('<I', img, va - base + toff)[0]) if toff else None
        a = cstr(img, base, struct.unpack_from('<I', img, va - base + aoff)[0]) if aoff else None
        out.append((g, t, a))
    if len(out) != count + 1 or any(g is None for g, _, _ in out[:count]) or out[count][0] is not None:
        raise NotHere('its song table is not where this build keeps it (another version of the program?)')
    return out[:count]


def later_table(root, gid):
    """the later PC pop'n games' music table: {id: (title, artist, genre)}"""
    m = machine(root, gid)
    dll = need(os.path.join(m.get('contents', ''), 'modules', m.get('exe', '')), 'the game\'s library')
    out = {}
    if gid == 'popn29':
        import songsback
        g = songsback.Game(dll)
        for i in range(g.songs):
            r = g.record(i)
            t = [g.text(struct.unpack_from('<Q', r, k)[0]) for k in (0, 8, 16, 24, 40, 48)]
            out[i] = t
    else:   # (runtime/pc32/songs.c's patterns)
        pe = PE(dll)
        c = pe.find('66 83 f8 ab 7e X b9 X X 00 00 66 3b c8')
        s = pe.find('98 69 c0 ac 00 00 00 66 8b 80 X X X X c3 0f bf c8 69 c9 ac 00 00 00 66 8b 81 X X X X c3')
        if len(c) != 1 or len(s) != 1 or pe.u32(s[0] + 10) != pe.u32(s[0] + 27) + 2:
            raise NotHere('its music table not found (not the build this reads)')
        songs, music = pe.u32(c[0] + 7), pe.u32(s[0] + 27) - 24
        for i in range(songs):
            out[i] = [pe.text(pe.u32(music + i * 172 + 4 * k)) for k in range(6)]
    table = {}
    for i, t in out.items():
        fw_genre, fw_title, fw_artist, genre, title, artist = [x or '' for x in t]
        title = title if not placeholder(title) else unicodedata.normalize('NFKC', fw_title)
        if placeholder(title):
            continue
        table[i] = (title, artist or unicodedata.normalize('NFKC', fw_artist), genre or unicodedata.normalize('NFKC', fw_genre), fw_genre)
    if not table:
        raise NotHere('its music table holds no song')
    return table


class Sources:
    """the tables with titles, read once each, for the games without: {number: (title, artist, genres)}"""

    def __init__(self, root):
        self.root, self.cache = root, {}

    def get(self, gid):
        if gid not in self.cache:
            try:
                if gid in ('popn29', 'popn28'):
                    t = {i: (title, artist, {norm(genre), norm(fwg)}) for i, (title, artist, genre, fwg) in later_table(self.root, gid).items()}
                else:
                    t = {i: (unicodedata.normalize('NFKC', title), unicodedata.normalize('NFKC', a or ''), {norm(g)})
                         for i, (g, title, a) in enumerate(popn_table(self.root, gid)) if title and not placeholder(g)}
            except NotHere:
                t = {}
            self.cache[gid] = t
        return self.cache[gid]


def popn_pack(root, gid, sources):
    songs = popn_songs(popn_table(root, gid), sources)
    charts = POPN_CHARTS if gid in ('popn9', 'popn10', 'popn11', 'popn12') else POPN_CHARTS_13
    titled = sum(1 for s in songs.values() if s['title'] != s['genre'])
    return songs, charts, '%d with a title' % titled


def popn_songs(table, sources):
    """a pack's songs from a pop'n song table (by song number), titles borrowed by number and genre"""
    songs = {}
    for n, (genre, title, artist) in enumerate(table):
        if placeholder(genre):
            continue   # (a song taken out: "-")
        s = {'genre': clean(genre)}
        own_title = title and not placeholder(title)
        for src in ('popn29', 'popn28', 'popn18', 'popn17', 'popn10', 'popn9'):
            hit = sources.get(src).get(n)
            if hit and norm(genre) in hit[2]:
                s['title'] = clean(hit[0])
                if hit[1] and not artist:
                    s['artist'] = clean(hit[1])
                break
        if 'title' not in s:
            s['title'] = clean(unicodedata.normalize('NFKC', title)) if own_title else s['genre']
        if artist and not placeholder(artist):
            s['artist'] = clean(unicodedata.normalize('NFKC', artist))
        songs[str(n)] = s
    return songs


def later_pack(root, gid, sources):
    songs = {}
    for g in ('popn29', 'popn28'):   # (the newer's first; the ids are both games')
        try:
            t = later_table(root, g)
        except NotHere:
            continue
        for i, (title, artist, genre, _) in t.items():
            if str(i) not in songs:
                s = {'title': clean(title)}
                if artist and not placeholder(artist):
                    s['artist'] = clean(artist)
                if genre and not placeholder(genre):
                    s['genre'] = clean(genre)
                songs[str(i)] = s
    if not songs:
        raise NotHere('not ingested (dumps/popn28, dumps/popn29: the later PC games)')
    return songs, LATER_CHARTS, ''


# --- beatmania IIDX

IIDX_CHARTS = {'0': 'SP Normal', '1': 'SP Hyper', '2': 'SP Another', '3': 'DP Normal', '4': 'DP Hyper', '5': 'DP Another'}
# id: (the key's address in the program, record size, the number's offset, its bytes)
IIDX = {
    'iidx10': (0x4badf8, 0x2dc, 0x144, 2), 'iidx11': (0x4d6c24, 0x2dc, 0x144, 2), 'iidx12': (0x4f6560, 0x2d8, 0x140, 2),
    'iidx13': (0x5cb3f8, 0x2a8, 0x110, 2), 'iidx14': (0x536428, 0x31c, 0x184, 4), 'iidx15': (0x546480, 0x31c, 0x184, 4),
    'iidx16': (0x56f684, 0x31c, 0x184, 4),
}

MBIG = 1000000000


def _ran3(seed):
    """Knuth's subtractive generator's table after its seeding (three refills): eout.bin's key stream"""
    ma = [0] * 56
    seed %= MBIG
    ma[55] = mj = seed
    mk = 1
    for i in range(1, 55):
        ii = 21 * i % 55
        ma[ii] = mk
        mk = mj - mk
        if mk < 0:
            mk += MBIG
        mj = ma[ii]
    for _ in range(3):
        _refill(ma)
    return ma


def _refill(ma):
    for i in range(1, 25):
        v = ma[i] - ma[i + 31]
        ma[i] = v + MBIG if v < 0 else v
    for i in range(25, 56):
        v = ma[i] - ma[i - 24]
        ma[i] = v + MBIG if v < 0 else v


def eout_decrypt(data, key):
    """eout.bin as the program reads it: each 16 bytes less the generator's bytes, seeded by a key byte"""
    streams = {}
    out = bytearray(data)
    for at in range(0, len(out), 16):
        k = key[(at // 16) % 16]
        if k not in streams:
            ma, idx, s = _ran3(k), 55, []
            for _ in range(16):
                idx += 1
                if idx > 55:
                    _refill(ma)
                    idx = 1
                s.append(ma[idx] & 0xFF)
            streams[k] = s
        s = streams[k]
        for j in range(min(16, len(out) - at)):
            out[at + j] = (out[at + j] - s[j]) & 0xFF
    return bytes(out)


def s9_decrypt(b):
    """IIDX 9th style's data/info/N.s as its program reads it"""
    if b[:4] != b'tv\x10\x94':
        raise NotHere('data/info/1.s is not the file this reads')
    seed = x = struct.unpack_from('<I', b, 4)[0]
    n = (len(b) - 8) // 4
    words = list(struct.unpack_from('<%dI' % n, b, 8))
    for i in range(n):
        x = (0x25 * x + 0x3E01A1EF) & 0xFFFFFFFF
        r = (i + seed) & 31
        words[i] ^= ((x << r) | (x >> (32 - r))) & 0xFFFFFFFF if r else x
    return struct.pack('<%dI' % n, *words)


def s9_songs(d):
    """{number: title} from 9th style's decrypted song file: 0x25c bytes a record from 12, the number
    a u16 at +0, the title at +4"""
    size, count = struct.unpack_from('<HH', d, 2)
    if size != 0x25C or 12 + count * size > len(d):
        raise NotHere('data/info/1.s is not laid out as this reads it')
    songs = {}
    for i in range(count):
        r = d[12 + i * size:12 + (i + 1) * size]
        songs[struct.unpack_from('<H', r, 0)[0]] = ascii_title(r[4:4 + 0x30])
    return songs


def eout_songs(d, rs, mo, mb):
    """{number: title} from a decrypted eout.bin: "IIDX", count n, the numbers' table's size m, the
    table (number -> record) at 16, then n records of rs bytes, each its title at +0 and its own number
    at mo (mb bytes), which the table must give back"""
    n, mcount = struct.unpack_from('<HH', d, 8)
    if d[:4] != b'IIDX' or 16 + 2 * mcount + n * rs != len(d):
        raise NotHere('data/info/eout.bin does not read with this build\'s key and layout')
    index = struct.unpack_from('<%dH' % mcount, d, 16)
    recs = 16 + 2 * mcount
    songs = {}
    for i in range(n):
        r = d[recs + i * rs:recs + (i + 1) * rs]
        mid = struct.unpack_from('<H' if mb == 2 else '<I', r, mo)[0]
        if mid >= mcount or index[mid] != i:
            raise NotHere('record %d\'s number %d isn\'t the one its table gives it' % (i, mid))
        songs[mid] = ascii_title(r[:0x40])
    return songs


def ascii_title(b):
    t = b.split(b'\0')[0]
    return clean(decode(t, 'cp932'))


def sd_folders(data):
    d = os.path.join(data, 'sd_data')
    return {int(x) for x in os.listdir(d) if re.fullmatch(r'\d{4}', x)} if os.path.isdir(d) else set()


def iidx_pack(root, gid, sources):
    m = machine(root, gid)
    data = need(m.get('data', ''), 'the game\'s data folder')
    folders = sd_folders(data)
    if gid == 'iidx9':
        songs = s9_songs(s9_decrypt(open(need(os.path.join(data, 'info', '1.s'), 'data/info/1.s'), 'rb').read()))
    else:
        keyva, rs, mo, mb = IIDX[gid]
        pe = PE(need(os.path.join(m.get('program', ''), m.get('exe', '')), 'the program'))
        key = pe.img[keyva - pe.base:keyva - pe.base + 16]
        raw = open(need(os.path.join(data, 'info', 'eout.bin'), 'data/info/eout.bin'), 'rb').read()
        songs = eout_songs(eout_decrypt(raw, key), rs, mo, mb)
    if folders and not set(songs) <= folders:
        raise NotHere('%d song numbers have no chart folder in data/sd_data' % len(set(songs) - folders))
    return {str(k): {'title': v} for k, v in sorted(songs.items()) if v}, IIDX_CHARTS, ''


# --- GuitarFreaks and DrumMania: V and V2 (Python 2's disk: APA partitions, PFS), System 573 (a CD)

class PFS:
    """a PS2 APA partition's PFS file system, read in place from the disk (a CHD)"""

    def __init__(self, chd, name):
        from chdread import Chd
        self.d = Chd(chd)
        lba, seen = 0, set()
        while True:
            h = self.d.read(lba * 512, 1024)
            seen.add(lba)
            if h[0x10:0x30].split(b'\0')[0].decode('latin-1') == name:
                ns = struct.unpack_from('<I', h, 0x4C)[0]
                self.subs = [struct.unpack_from('<I', h, 0x40)[0]] + [struct.unpack_from('<I', h, 0x200 + 8 * i)[0] for i in range(ns)]
                break
            lba = struct.unpack_from('<I', h, 8)[0]
            if lba in seen or lba == 0:
                raise NotHere('no partition %s on the disk' % name)
        sb = self.d.read(self.subs[0] * 512 + 8192 * 512, 512)
        if sb[:4] != b'\0SFP':
            raise NotHere('partition %s holds no PFS' % name)
        self.zs = struct.unpack_from('<I', sb, 16)[0]
        self.root = struct.unpack_from('<IHH', sb, 0x20)

    def zone(self, num, sub, count=1):
        return self.d.read((self.subs[sub] + num * (self.zs // 512)) * 512, count * self.zs)

    def inode(self, bi):
        b = self.zone(bi[0], bi[1])[:1024]
        mode, size, nd = struct.unpack_from('<H', b, 0x3B8)[0], struct.unpack_from('<Q', b, 0x3D8)[0], struct.unpack_from('<I', b, 0x3E0)[0]
        ext = [struct.unpack_from('<IHH', b, 0x28 + 8 * i) for i in range(114)]
        nxt = struct.unpack_from('<IHH', b, 0x10)
        while nxt[2] and len(ext) < nd:
            s = self.zone(nxt[0], nxt[1])[:1024]
            ext += [struct.unpack_from('<IHH', s, 0x28 + 8 * i) for i in range(114)]
            nxt = struct.unpack_from('<IHH', s, 0x10)
        return mode, size, ext[:nd]

    def read(self, bi, upto=None):
        _, size, ext = self.inode(bi)
        upto = size if upto is None else min(size, upto)
        out = bytearray()
        for i, (n, sub, c) in enumerate(ext):
            if i == 0:
                n, c = n + 1, c - 1   # (the inode's own zone)
            if c > 0:
                out += self.zone(n, sub, c)
            if len(out) >= upto:
                break
        return bytes(out[:upto])

    def lookup(self, path):
        bi = self.root
        for part in path.strip('/').split('/'):
            data = self.read(bi)
            found = None
            for sec in range(0, len(data), 512):
                p = sec
                while p < sec + 512:
                    ino, sub, pl, al = struct.unpack_from('<IBBH', data, p)
                    if al & 0xFFF == 0:
                        break
                    if ino and data[p + 8:p + 8 + pl].decode('latin-1') == part:
                        found = (ino, sub, 1)
                    p += al & 0xFFF
            if not found:
                raise NotHere('no %s on the disk' % path)
            bi = found
        return bi


def gfdm_records(net_id, music, rs, name_at, by_place=False):
    """{netid: name} from a net_id table and its music_info records"""
    ver, count, toff = struct.unpack_from('<HHH', net_id, 0)
    ids = struct.unpack_from('<%dh' % count, net_id, toff)
    if len(music) < count * rs:
        raise NotHere('the music table is shorter than its net_id table')
    out = {}
    for i in range(count):
        r = music[i * rs:(i + 1) * rs]
        num = struct.unpack_from('<H', r, 0)[0]
        if not by_place and num != ids[i]:
            raise NotHere('music_info[%d] is not net_id[%d]\'s song' % (i, i))
        name = clean(decode(r[name_at:name_at + 16].split(b'\0')[0]))
        if name:
            out[i if by_place else num] = name
    return out


def gfdmv_pack(root, gid, sources):
    m = machine(root, gid)
    chd = need(os.path.join(m['_dir'], m.get('hdd', '')), 'the disk')
    if gid == 'drmnvj':
        fs = PFS(chd, 'PC.KNAC_00001.00672.E02-JA')
        pi, fl = fs.read(fs.lookup('/data/pack/packinfo.bin')), fs.read(fs.lookup('/data/finfolist.bin'))
        n = struct.unpack_from('<H', pi, 12)[0]
        ents = {}
        for i in range(n):
            crc, _, pk, off, sz = struct.unpack_from('<IHHII', pi, 16 + i * 32 + 16)
            ents[crc] = (pk, off, sz)
        nf = struct.unpack_from('<I', fl, 0)[0]
        paks = [fl[4 + i * 148:4 + i * 148 + 128].split(b'\0')[0].decode('latin-1') for i in range(nf)]

        def get(name):
            e = ents.get(zlib.crc32(('/data/product/music/%s.bin' % name).encode()))
            if not e:
                raise NotHere('no music/%s.bin in its packs' % name)
            pk, off, sz = e
            return fs.read(fs.lookup(paks[pk]), off + sz)[off:off + sz]
        names = gfdm_records(get('net_id'), get('music_info'), 92, 0x4C)
    else:
        fs = PFS(chd, 'PC.KNAC_00001.00672.F02-JA')
        pkg = fs.read(fs.lookup('/data/pkg/sys_v02a/sys_minfo.pkg'))
        items = {}
        for it in re.finditer(rb'<item>(.*?)</item>', pkg[:1024], re.S):   # (its XML head: name, offset, fsize)
            nm, o, s = (re.search(rb'<%s[^>]*>([^<]*)<' % k, it.group(1)) for k in (b'name', b'offset', b'fsize'))
            if nm and o and s:
                items[nm.group(1).decode('latin-1').strip()] = (1024 + int(o.group(1)), int(s.group(1)))
        if 'minf.net_id' not in items or 'minf.music_info' not in items:
            raise NotHere('sys_minfo.pkg is not laid out as this reads it')
        part = lambda k: pkg[items[k][0]:items[k][0] + items[k][1]]
        names = gfdm_records(part('minf.net_id'), part('minf.music_info'), 96, 0x50)
    names.pop(0, None)   # (number 0: no song)
    return {str(k): {'title': v} for k, v in sorted(names.items())}, None, 'name-sort keys, 15 letters at most'


class ISO:
    """an ISO 9660 CD image of raw 2352-byte sectors"""

    def __init__(self, path):
        self.f = open(path, 'rb')

    def read(self, lba, size):
        out = b''
        for i in range((size + 2047) // 2048):
            self.f.seek((lba + i) * 2352 + 16)
            out += self.f.read(2048)
        return out[:size]

    def find(self, want):
        pvd = self.read(16, 2048)
        lba, size = struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0]
        d = self.read(lba, size)
        p = 0
        while p < len(d):
            n = d[p]
            if n == 0:
                p = (p // 2048 + 1) * 2048
                continue
            name = d[p + 33:p + 33 + d[p + 32]].decode('latin-1').split(';')[0]
            if name == want:
                return self.read(struct.unpack_from('<I', d, p + 2)[0], struct.unpack_from('<I', d, p + 10)[0])
            p += n
        raise NotHere('no /%s on its CD' % want)


def gfdm573_pack(root, gid, sources):
    for set_id in (['gtfrk11m'] if gid == 'gfdm573-gf' else ['drmn10m']):
        try:
            m = machine(root, set_id)
        except NotHere:
            continue
        cue = m.get('cd', '')
        game = ISO(need(os.path.join(m['_dir'], cue[:-4] + '.bin' if cue.endswith('.cue') else cue), 'its CD')).find('GAME.DAT')
        # the net_id table: its header (02 01, count, 0x18) and its numbers in order; music_info's
        # records (92 bytes) end where it begins
        for h in re.finditer(rb'\x02\x01(..)\x18\x00', game, re.S):
            at = h.start()
            count = struct.unpack_from('<H', game, at + 2)[0]
            if not 16 <= count <= 1024 or at + 0x18 + 2 * count > len(game):
                continue
            ids = struct.unpack_from('<%dh' % count, game, at + 0x18)
            if list(ids) != sorted(set(ids)):
                continue
            k = 0
            while at - 92 * (k + 1) >= 0 and struct.unpack_from('<H', game, at - 92 * (k + 1))[0] == struct.unpack_from('<H', game, at - 92 * (k + 1) + 0x12)[0] \
                    and struct.unpack_from('<H', game, at - 92 * (k + 1))[0] in ids:
                k += 1
            if k < 16:
                continue
            music = game[at - 92 * k:at]
            place = {v: i for i, v in enumerate(ids)}
            names = {}
            for i in range(k):
                r = music[i * 92:(i + 1) * 92]
                name = clean(decode(r[0x4C:0x5C].split(b'\0')[0]))
                if name:
                    names[place[struct.unpack_from('<H', r, 0)[0]]] = name
            return {str(n): {'title': v} for n, v in sorted(names.items())}, None, 'from %s; name-sort keys, 15 letters at most' % set_id
        raise NotHere('no net_id table in %s\'s GAME.DAT' % set_id)
    raise NotHere('not ingested (the System 573 set with e-AMUSEMENT: %s)' % ('gtfrk11m' if gid == 'gfdm573-gf' else 'drmn10m'))


# --- the server's games, in the order they are made

GAMES = ([(g, popn_pack) for g in ('popn12', 'popn13', 'popn14', 'popn15', 'popn16', 'popn17', 'popn18', 'popn9', 'popn10', 'popn11')] +
         [('popn-pc-later', later_pack)] + [('iidx%d' % n, iidx_pack) for n in range(9, 17)] +
         [('drmnvj', gfdmv_pack), ('drmnv2j', gfdmv_pack), ('gfdm573-gf', gfdm573_pack), ('gfdm573-dm', gfdm573_pack)])
NOT_READ = {'E03': 'GuitarFreaks V: not ingested here, and its row has no id', 'F03': 'GuitarFreaks V2: not ingested here, and its row has no id'}


def write_pack(out_dir, gid, songs, charts):
    p = {'format': 1, 'game': gid, 'songs': songs}
    if charts:
        p['charts'] = charts
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, gid + '.json')
    tmp = path + '.part'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(p, f, ensure_ascii=False, separators=(',', ':'))
    os.replace(tmp, path)
    return path


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--root', required=True, help='the HIRAKU folder whose dumps/ has your games')
    ap.add_argument('--out', required=True, help='the folder the packs go to (serve --names DIR)')
    ap.add_argument('--game', action='append', help='only this game (the server\'s id); again for more')
    a = ap.parse_args(argv)
    known = [g for g, _ in GAMES]
    for g in a.game or []:
        if g not in known:
            ap.error('no game %s (%s)' % (g, ', '.join(known)))
    sources = Sources(a.root)
    for gid, make in GAMES:
        if a.game and gid not in a.game:
            continue
        try:
            songs, charts, note = make(a.root, gid, sources)
            if not songs:
                raise NotHere('its table holds no song')
            write_pack(a.out, gid, songs, charts)
            print('%s: %d songs written%s' % (gid, len(songs), ' (%s)' % note if note else ''))
        except NotHere as e:
            print('%s: not made: %s' % (gid, e))
    return 0


if __name__ == '__main__':
    sys.exit(main())
