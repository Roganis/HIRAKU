#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/eamuse/names.py's reading and numbering, on tiny MADE-UP tables in the games' layouts: a pop'n
song table (pointers into a program image, Shift-JIS texts, a song taken out), a title borrowed by
song number only when the genre is the same song's, beatmania IIDX's eout.bin (encrypted, its number
table checked) and 9th style's song file, a GuitarFreaks/DrumMania net_id and music table; texts
cleaned; a root with no games: each game's one line why not. Every title here is made up.

    tests/eamuse/test_names_tool.py      exit 0 when all hold
"""
import contextlib, io, json, os, struct, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools', 'eamuse'))
import names   # noqa: E402

failed = 0


def check(ok, what):
    global failed
    print(('ok    ' if ok else 'FAIL  ') + what, flush=True)
    failed += not ok


def raises(fn):
    try:
        fn()
    except names.NotHere:
        return True
    return False


# --- a pop'n 9-style table: 0x60 bytes an entry, pointers (name, initial, genre, title, comment)
BASE, AT, SIZE = 0x100000, 0x100100, 0x60
img = bytearray(0x1000)
strings = 0x100800
def put(text, enc='cp932'):
    global strings
    b = text.encode(enc) + b'\0'
    img[strings - BASE:strings - BASE + len(b)] = b
    strings += len(b) + (-len(b) % 4)
    return strings - len(b) - (-len(b) % 4)
rows = [('ゲンレ', 'Made-up Song 0'), ('-', '-'), ('テスト', 'Made-up Song 2')]   # (song 1 taken out)
for n, (genre, title) in enumerate(rows):
    struct.pack_into('<5I', img, AT - BASE + n * SIZE, put('name%d' % n, 'ascii'), put('ア'), put(genre), put(title, 'ascii'), put('Made-up comment'))
table = names.read_popn_table(bytes(img), BASE, AT + 8, SIZE, 3, 4, None)
check([t[0] for t in table] == ['ゲンレ', '-', 'テスト'] and table[2][1] == 'Made-up Song 2', "pop'n table: Shift-JIS genres as text, entry n = song n")
check(raises(lambda: names.read_popn_table(bytes(img), BASE, AT + 8, SIZE, 2, 4, None)), "pop'n table: a wrong count (an entry past the end) refused")
check(raises(lambda: names.read_popn_table(bytes(img), BASE, AT + 8 + SIZE, SIZE, 3, 4, None)), "pop'n table: started an entry late: refused")


class Sources:   # (stands for the tables with titles: by number, (title, artist, genres))
    def __init__(self, t):
        self.t = t

    def get(self, gid):
        return self.t.get(gid, {})


no_titles = [(g, None, None) for g, _ in rows]
src = Sources({'popn18': {0: ('Made-up Song A', 'Made-up Artist', {names.norm('ゲンレ')}), 2: ('Made-up Song B', '', {names.norm('ほか')})}})
songs = names.popn_songs(no_titles, src)
check(set(songs) == {'0', '2'}, "pop'n songs: the one taken out left out, the others by their numbers")
check(songs['0'] == {'genre': 'ゲンレ', 'title': 'Made-up Song A', 'artist': 'Made-up Artist'}, "pop'n songs: a title borrowed when the genre is the same at the same number")
check(songs['2'] == {'genre': 'テスト', 'title': 'テスト'}, "pop'n songs: another genre at that number: no title borrowed (the genre stands)")
check(names.popn_songs(table, Sources({}))['2']['title'] == 'Made-up Song 2', "pop'n songs: a table's own title")

# --- beatmania IIDX eout.bin: "IIDX", n, m, the number -> record table, records (title, number)
RS, MO = 0x80, 0x44
plain = bytearray(b'IIDX' + struct.pack('<IHH4x', 16, 2, 1200))
index = [0xFFFF] * 1200
index[1001], index[1102] = 0, 1
plain += struct.pack('<1200H', *index)
for title, mid in (('Made-up Song 1001', 1001), ('Made-up Song 1102', 1102)):
    r = bytearray(RS)
    r[:len(title)] = title.encode()
    struct.pack_into('<H', r, MO, mid)
    plain += r
key = bytes(range(7, 23))


def encrypt(data, key):   # (the program's decryption undone: each byte plus the generator's)
    out = bytearray(data)
    for at in range(0, len(out), 16):
        ma, idx = names._ran3(key[(at // 16) % 16]), 55
        for j in range(min(16, len(out) - at)):
            idx += 1
            if idx > 55:
                names._refill(ma)
                idx = 1
            out[at + j] = (out[at + j] + ma[idx]) & 0xFF
    return bytes(out)


enc = encrypt(bytes(plain), key)
check(enc != bytes(plain) and names.eout_decrypt(enc, key) == bytes(plain), 'eout.bin: decrypted with its key')
check(names.eout_songs(names.eout_decrypt(enc, key), RS, MO, 2) == {1001: 'Made-up Song 1001', 1102: 'Made-up Song 1102'},
      'eout.bin: each record by its own number')
bad = bytearray(plain)
struct.pack_into('<H', bad, 16 + 2 * 1001, 1)
check(raises(lambda: names.eout_songs(bytes(bad), RS, MO, 2)), 'eout.bin: a number its table doesn\'t give back: refused')
check(raises(lambda: names.eout_songs(names.eout_decrypt(enc, bytes(16)), RS, MO, 2)), 'eout.bin: another key: refused')

# --- 9th style's song file: "tv\x10\x94", a seed, then words xored; 0x25c-byte records from 12
body = bytearray(struct.pack('<HHHH', 0, 0x25C, 2, 0) + b'\0' * 4)
for mid, title in ((905, 'Made-up Song 905'), (101, 'Made-up Song 101')):
    r = bytearray(0x25C)
    struct.pack_into('<H', r, 0, mid)
    r[4:4 + len(title)] = title.encode()
    body += r
body += b'\0' * (-len(body) % 4)
head = b'tv\x10\x94' + struct.pack('<I', 0x1234567)
enc9 = head + names.s9_decrypt(head + bytes(body))   # (an xor: the same steps make it)
check(enc9[8:] != bytes(body) and names.s9_songs(names.s9_decrypt(enc9)) == {905: 'Made-up Song 905', 101: 'Made-up Song 101'},
      "9th style's song file: decrypted, each record by its own number")

# --- GuitarFreaks/DrumMania: net_id (header, sorted numbers) and music_info (92 bytes, number at +0)
nums = [4, 17, 30]
net_id = struct.pack('<6H', 0x102, 3, 0x18, 6, 0x10, 0) + bytes([4, 4, 1, 0, 0, 0]) + bytes(6) + struct.pack('<3h', *nums) + b'\0' * 16
music = b''
for n in nums:
    r = bytearray(92)
    struct.pack_into('<H', r, 0, n)
    struct.pack_into('<H', r, 0x12, n)
    r[0x4C:0x4C + 13] = b'MADE UP %5d' % n
    music += r
check(names.gfdm_records(net_id, music, 92, 0x4C) == {4: 'MADE UP     4', 17: 'MADE UP    17', 30: 'MADE UP    30'},
      'GuitarFreaks/DrumMania V: the netid is the record\'s number')
check(names.gfdm_records(net_id, music, 92, 0x4C, by_place=True) == {0: 'MADE UP     4', 1: 'MADE UP    17', 2: 'MADE UP    30'},
      'System 573: the netid is the song\'s place in net_id')
check(raises(lambda: names.gfdm_records(net_id, music[92:] + music[:92], 92, 0x4C)), 'a music table out of net_id\'s order: refused')

# --- texts
check(names.clean('Made-up\x01 Song\x7f' + 'x' * 300) == ('Made-up Song' + 'x' * 300)[:200], 'a text: control characters dropped, cut at 200')
check(names.norm('ＭＡＤＥ－ＵＰ　ソング') == names.norm('made-up ソング'), 'genres compared: full width as half, spaces and case aside')
check(names.placeholder('-') and names.placeholder('---') and not names.placeholder('ゲンレ'), 'a song taken out: "-" or "---"')

# --- the whole tool on a root with no games: a line each, why not; a pack written as the server reads it
with tempfile.TemporaryDirectory() as t:
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        status = names.main(['--root', t, '--out', os.path.join(t, 'out')])
    lines = out.getvalue().splitlines()
    check(status == 0 and len(lines) == len(names.GAMES) and all(': not made: not ingested' in ln for ln in lines) and
          not os.path.exists(os.path.join(t, 'out')), 'no games ingested: one line each, nothing written')
    path = names.write_pack(os.path.join(t, 'out'), 'popn12', {'5': {'title': 'Made-up Song 5'}}, names.POPN_CHARTS)
    j = json.load(open(path, encoding='utf-8'))
    check(os.path.basename(path) == 'popn12.json' and j['format'] == 1 and j['game'] == 'popn12' and j['songs']['5']['title'] == 'Made-up Song 5',
          'a pack: GAME.json, format 1')

sys.exit(1 if failed else 0)
