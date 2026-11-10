#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""hiraku-eamuse and calls no game would send (it listens on a network): each is answered or
dropped, the server goes on answering the next cabinet, and nothing is written outside its data
folder. Made-up calls only, no game data.

    tests/eamuse/test_malformed.py [build/cmake/hiraku-eamuse]      exit 0 when all hold
"""
import os, shutil, socket, struct, subprocess, sys, tempfile, time
SERVER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'build', 'cmake', 'hiraku-eamuse')
T = tempfile.mkdtemp(prefix='hk-malformed-')
D = os.path.join(T, 'in', 'data')   # two folders down: what a "../" in a name would reach is ours to look at
os.makedirs(D)
failed = 0


def check(ok, what):
    global failed
    print(('ok    ' if ok else 'FAIL  ') + what)
    failed += not ok


with socket.socket() as s0:
    s0.bind(('127.0.0.1', 0)); port = s0.getsockname()[1]
srv = subprocess.Popen([SERVER, '--port', str(port), '--bind', '127.0.0.1', '--data', D, '--quiet', '--no-relay'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(300):   # up to 30 s: a freshly built server read from a slow disk
    try:
        socket.create_connection(('127.0.0.1', port)).close(); break
    except OSError:
        time.sleep(0.1)


def post(body, wait=5.0, headers=b''):
    """The answer's status line and body, or None when the server gave none in `wait` seconds"""
    try:
        s = socket.create_connection(('127.0.0.1', port), timeout=wait)
        s.sendall(b'POST / HTTP/1.0\r\n' + headers + b'Content-Length: %d\r\n\r\n' % len(body) + body)
        out = b''
        while (d := s.recv(65536)): out += d
        s.close()
        return out
    except OSError:
        return None


def call(model, inner):
    return post(f'<?xml version="1.0"?><call model="{model}" srcid="0">{inner}</call>'.encode())


def alive(what):
    t0 = time.monotonic()
    a = call('C00:J:A:A', '<ntp method="get"/>')
    check(srv.poll() is None and a is not None and b'<ntp status="0"' in a and time.monotonic() - t0 < 2.0, what)


def outside():
    """Every file under the test's folder that isn't in the data folder"""
    return sorted(os.path.join(r, f) for r, _, fs in os.walk(T) for f in fs if not r.startswith(D))


try:
    alive('a plain call is answered')

    # a ref id shorter than the four digits its player number is cut at
    for model, module in (('M39:J:A:A', 'player'), ('M39:J:A:A', 'player24')):
        for ref in ('a', 'abc', 'abcde'):
            call(model, f'<{module} method="new"><ref_id>{ref}</ref_id><name>X</name></{module}>')
    alive('short ref ids: the server goes on')

    # a close tag cut before its '>'
    t0 = time.monotonic()
    a = post(b'<call model="C00:J:A:A"><a></a</call', wait=5.0)
    check(a is not None and time.monotonic() - t0 < 2.0, 'a close tag with no end: answered at once')
    alive('a close tag with no end: the server goes on')

    # names with a way out of the folder in them: a model, a card's id, a player's id
    call('../:J:A:A', '<rank method="exp" course="1" score="1" card="E004010000000001"/>')
    call('../../x', '<userdata method="write" card="../../x" time="0"><a/></userdata>')
    call('D40:J:A:A', '<gamedata method="cardregist"><card id="E004010000000001" irid="1" name="A" pass="1234" type="1"/></gamedata>')
    for gdid in ('../../../out', '..\\..\\out', '/tmp/hk-malformed-out'):
        call('D40:J:A:A', f'<gamedata method="gameend"><player card="use" no="1" gdid="{gdid}" skill="1"/>'
                          f'<music netid="1" score="1"/></gamedata>')
        call('D40:J:A:A', f'<gamedata method="gametop"><player card="use" no="1" gdid="{gdid}"/></gamedata>')
    alive('names with ../ in them: the server goes on')
    check(outside() == [], 'names with ../ in them: nothing written outside the data folder' + ''.join('\n        ' + f for f in outside()))
    check(not os.path.exists('/tmp/hk-malformed-out') and all('..' not in f and '\\' not in f for f in os.listdir(D)),
          'names with ../ in them: no such file in the data folder either')

    # binary XML, 200000 elements each inside the one before
    nodes = b'\x01\x01\x2c' * 200000   # void, a name of one letter (six bits: 'a')
    deep = b'\xa0\x42\x80\x7f' + struct.pack('>I', len(nodes)) + nodes + struct.pack('>I', 0)
    t0 = time.monotonic()
    post(deep, wait=10.0)
    check(time.monotonic() - t0 < 5.0, 'binary XML nested 200000 deep: refused at once')
    alive('binary XML nested 200000 deep: the server goes on')

    # text and binary rubbish
    for junk in (b'', b'<', b'<call', b'<call model=', b'<call model="C00"><', b'\xa0\x42\x80\x7f' + b'\xff' * 64, os.urandom(4096)):
        post(junk, wait=5.0)
    post(b'x', headers=b'X-Eamuse-Info: 1-zz-zz\r\n')
    post(b'x' * 100, headers=b'X-Compress: lz77\r\nX-Eamuse-Info: 1-00000000-0000\r\n')
    alive('rubbish: the server goes on')

    # connections that say nothing, or stop half way: the next cabinet isn't kept waiting
    idle = [socket.create_connection(('127.0.0.1', port)) for _ in range(8)]
    half = socket.create_connection(('127.0.0.1', port))
    half.sendall(b'POST / HTTP/1.0\r\nContent-Length: 1000\r\n\r\n<call')
    alive('nine connections left open and silent: a call is still answered at once')
    for s in idle + [half]:
        s.close()

    # more connections than it serves at once: back to work once they go
    many = []
    try:
        for _ in range(200):
            many.append(socket.create_connection(('127.0.0.1', port), timeout=2))
    except OSError:
        pass
    for s in many:
        s.close()
    time.sleep(0.5)
    alive('200 connections opened and closed: the server goes on')
finally:
    srv.kill()
    shutil.rmtree(T, ignore_errors=True)
print(f'{failed} failed' if failed else 'test_malformed: all hold')
sys.exit(1 if failed else 0)
