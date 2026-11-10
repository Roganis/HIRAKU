#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The server never makes a new player of an old one: a "new" (or a carry-over from the game before)
asked for a card that has its player leaves the player as it is and answers it; and a player's file
that isn't one (another size than the game's record) is kept aside before a new one takes its name.
Made-up calls, the kept files checked.

    tests/eamuse/test_players_kept.py [build/cmake/hiraku-eamuse]      exit 0 when all hold
"""
import glob, os, shutil, socket, subprocess, sys, tempfile, time
SERVER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'build', 'cmake', 'hiraku-eamuse')
D = tempfile.mkdtemp(prefix='hk-kept-')
LOG = open(os.path.join(D, 'server-said.txt'), 'w+')
with socket.socket() as s0:
    s0.bind(('127.0.0.1', 0)); port = s0.getsockname()[1]
srv = subprocess.Popen([SERVER, '--port', str(port), '--data', D, '--quiet', '--no-relay'], stdout=subprocess.DEVNULL, stderr=LOG)
for _ in range(300):   # up to 30 s: a freshly built server read from a slow disk
    try:
        socket.create_connection(('127.0.0.1', port)).close(); break
    except OSError:
        time.sleep(0.1)
fails = 0


def check(ok, what):
    global fails
    fails += not ok
    print(('ok    ' if ok else 'FAIL  ') + what, flush=True)


def call(model, body):
    x = f'<?xml version="1.0"?><call model="{model}" srcid="0"><{body}</call>'.encode()
    s = socket.create_connection(('127.0.0.1', port))
    s.sendall(b'POST / HTTP/1.0\r\nContent-Length: %d\r\n\r\n' % len(x) + x)
    out = b''
    while (d := s.recv(65536)): out += d
    return out.split(b'\r\n\r\n', 1)[1].decode('cp932', 'replace')


def kept(name):
    try:
        with open(os.path.join(D, name), 'rb') as f:
            return f.read()
    except OSError:
        return None


def put(name, data):
    with open(os.path.join(D, name), 'wb') as f:
        f.write(data)


def aside(name): return sorted(glob.glob(os.path.join(D, name + '.unread-*')))
def said():
    LOG.flush()
    with open(LOG.name) as f:
        return f.read()


CARD, REF = 'E004010000000012', '48524B00000000A1'
# each game: its model, its player's file, its record's size, and its calls (new, a credit's set,
# the carry-over from the game before or None) with the name to give
GAMES = [
    ('E12:J:A:A', f'pdata-E12-{CARD}', 0x708, lambda m, n: f'E12 type="playerdata" method="{m}" card_id="{CARD}" old_card_id="{CARD}" name="{n}" password="1234" option="7"></E12>', 'conv'),
    ('E13:J:A:A', f'pdata-E13-{REF}', 0xd6c, lambda m, n: f'E13 type="playerdata" method="{m}" ref_id="{REF}" old_card_id="{CARD}" name="{n}" password="1234" option="7"></E13>', 'conv'),
    ('F14:J:A:A', f'pdata-F14-{REF}', 0xdb8, lambda m, n: f'F14 type="playerdata" method="{m}" ref_id="{REF}" name="{n}" option="7"></F14>', 'conv'),
    ('G15:J:A:A:2007091800', f'pdata-G15-{REF}', 0xb38, lambda m, n: f'G15 method="playerdata_{m}" ref_id="{REF}" name="{n}" option="7"></G15>', 'conv'),
    ('H16:J:A:A:2008052700', f'pdata-H16-{REF}', 0x6784, lambda m, n: f'playerdata method="{m}" ref_id="{REF}" name="{n}" option="7"></playerdata>', 'conversion'),
    ('I17:J:A:A:2009022500', f'pdata-I17-{REF}', 0x74bc, lambda m, n: f'playerdata method="{m}" ref_id="{REF}" name="{n}" option="7"></playerdata>', 'conversion'),
    ('J39:J:A:A:2010040500', f'pdata-J39-{REF}', 0x1700, lambda m, n: f'playerdata method="{m}" ref_id="{REF}" name="{n}" option="7"></playerdata>', 'conversion'),
]
try:
    for model, file, size, body, carry in GAMES:
        g = model[:3]
        a = call(model, body('new', 'ALICE'))
        first = kept(file)
        check(first is not None and len(first) == size and first[:5] == b'ALICE' and 'status="0"' in a, f'{g}: new makes the player')
        call(model, body('set', 'ALICE'))
        played = kept(file)
        check(played is not None and played != first and played[:5] == b'ALICE', f'{g}: a credit is kept in it')
        a = call(model, body('new', 'EVE'))
        check(kept(file) == played and 'status="0"' in a and '<b' in a, f'{g}: new again leaves the player as it is, and answers it')
        a = call(model, body(carry, 'EVE'))
        check(kept(file) == played and 'status="0"' in a, f'{g}: so does a carry-over from the game before ({carry})')
        check(not aside(file), f'{g}: nothing was put aside')
    check(said().count('a new player was asked for where there is one') == 2 * len(GAMES), 'the server says each time that it kept the player')

    # the two games that keep a player as lines
    for model, module, file in (('M39:J:A:A', 'player24', f'p24-{REF}'), ('M39:J:A:A', 'player', f'phc-{REF}')):
        body = lambda m, n: f'{module} method="{m}"><ref_id>{REF}</ref_id><name>{n}</name></{module}>'
        a = call(model, body('new', 'ALICE'))
        first = kept(file)
        check(first is not None and b'ALICE' in first and 'ALICE' in a, f'{module}: new makes the player')
        a = call(model, body('new', 'EVE'))
        check(kept(file) == first and 'ALICE' in a and 'EVE' not in a, f'{module}: new again leaves the player as it is, and answers it')

    # a file that isn't a player's: read as none, kept aside when a new one takes its name
    R2 = '48524B00000000A2'
    cut = b'ALICE' + bytes(95)
    put(f'pdata-G15-{R2}', cut)
    a = call('G15:J:A:A:2007091800', f'G15 method="playerdata_get" ref_id="{R2}"></G15>')
    check('status="109"' in a and kept(f'pdata-G15-{R2}') == cut and not aside(f'pdata-G15-{R2}'), 'a cut record: get says no player, and touches nothing')
    call('G15:J:A:A:2007091800', f'G15 method="playerdata_new" ref_id="{R2}" name="BOB"></G15>')
    now, was = kept(f'pdata-G15-{R2}'), aside(f'pdata-G15-{R2}')
    check(now is not None and len(now) == 0xb38 and now[:3] == b'BOB', '... new makes a player')
    check(len(was) == 1 and open(was[0], 'rb').read() == cut, '... with the cut record kept beside it (.unread-DATE)')
    check(f'pdata-G15-{R2} isn\'t a player\'s file' in said(), '... and the server says so')
    # a credit's end on one (pop'n 13 finding pop'n 12's size; pop'n 18 a cut one)
    for model, file, size, wrong, body in (
            ('E13:J:A:A', f'pdata-E13-{R2}', 0xd6c, bytes([1]) * 0x708, f'E13 type="playerdata" method="set" ref_id="{R2}" option="3"></E13>'),
            ('J39:J:A:A:2010040500', f'pdata-J39-{R2}', 0x1700, bytes([2]) * 0x16ff, f'playerdata method="set" ref_id="{R2}" option="3"></playerdata>')):
        put(file, wrong)
        call(model, body)
        now, was = kept(file), aside(file)
        check(now is not None and len(now) == size and len(was) == 1 and open(was[0], 'rb').read() == wrong,
              f'{model[:3]}: set on a record of another size: a new record, the other kept beside it')
    # no file, or an empty one: nothing to keep
    put(f'pdata-H16-{R2}', b'')
    call('H16:J:A:A:2008052700', f'playerdata method="new" ref_id="{R2}" name="BOB"></playerdata>')
    check(len(kept(f'pdata-H16-{R2}') or b'') == 0x6784 and not aside(f'pdata-H16-{R2}'), 'an empty file: a new player, nothing put aside')
    # the games that keep lines: a file of zeros (a disk's cut write)
    for module, file in (('player', f'phc-{R2}'), ('player24', f'p24-{R2}')):
        put(file, bytes(64))
        call('M39:J:A:A', f'{module} method="start"><ref_id>{R2}</ref_id></{module}>')
        a = call('M39:J:A:A', f'{module} method="read"><ref_id>{R2}</ref_id></{module}>')
        check('>2</result>' in a and kept(file) == bytes(64), f'{module}: a file of zeros reads as no player, untouched')
        call('M39:J:A:A', f'{module} method="new"><ref_id>{R2}</ref_id><name>BOB</name></{module}>')
        was = aside(file)
        check(b'BOB' in (kept(file) or b'') and len(was) == 1 and open(was[0], 'rb').read() == bytes(64), '... new makes a player, the zeros kept beside it')
finally:
    srv.terminate()
    srv.wait(5)
    LOG.close()
    shutil.rmtree(D, ignore_errors=True)
print(f'{fails} failed' if fails else 'all hold')
sys.exit(1 if fails else 0)
