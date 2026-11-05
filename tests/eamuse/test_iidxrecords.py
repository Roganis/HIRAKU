#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""beatmania IIDX EMPRESS's records on HIRAKU's server (docs/EAMUSE3.md, "GOLD, DJ TROOPERS and
EMPRESS"): a credit's DJ points and plays (pc save), the dan grades (grade raised) and the EXPERT
courses' results (ranking entry, getranker), and the course results of DistorteD, GOLD and DJ
TROOPERS, from made-up calls.

    tests/eamuse/test_iidxrecords.py [build/cmake/hiraku-eamuse]      exit 0 when all hold
"""
import os, re, shutil, socket, subprocess, sys, tempfile, time
SERVER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'build', 'cmake', 'hiraku-eamuse')
D = tempfile.mkdtemp(prefix='hk-iidxrec-')
with socket.socket() as s0:
    s0.bind(('127.0.0.1', 0)); port = s0.getsockname()[1]
srv = subprocess.Popen([SERVER, '--port', str(port), '--data', D, '--quiet', '--no-relay'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(300):
    try:
        socket.create_connection(('127.0.0.1', port)).close(); break
    except OSError:
        time.sleep(0.1)
M = 'I00:J:A:A:2009072200'
def call(body):
    x = f'<?xml version="1.0"?><call model="{M}" srcid="0">{body}</call>'.encode()
    s = socket.create_connection(('127.0.0.1', port))
    s.sendall(b'POST / HTTP/1.0\r\nContent-Length: %d\r\n\r\n' % len(x) + x)
    out = b''
    while (d := s.recv(65536)): out += d
    return out.decode('utf-8', 'replace')
fails = 0
def check(what, got, want):
    global fails
    ok = got == want; fails += not ok
    print(('ok  ' if ok else 'FAIL'), what, got, '' if ok else f'(want {want})')
def attr(text, el, name):
    m = re.search(r'<%s\b[^>]*\b%s="([^"]*)"' % (el, name), text)
    return m.group(1) if m else None
try:
    ids = []
    for rid, name in (('48524B00000000A1', 'ALPHA'), ('48524B00000000A2', 'BRAVO')):
        ids.append(attr(call(f'<pc method="reg" rid="{rid}" name="{name}" pid="13" did="0"/>'), 'pc', 'id'))
    a, b = ids
    # a credit on SP, then one on DP: each side's DJ points and plays
    call(f'<pc method="save" iidxid="{a}" cltype="0" achi="1234" pnum="7" gno="3" mode="1"/>')
    call(f'<pc method="save" iidxid="{a}" cltype="1" achi="55" pnum="2" gno="3" mode="1"/>')
    g = call('<pc method="get" rid="48524B00000000A1"/>')
    check('SP DJ points and plays', (attr(g, 'pcdata', 'sach'), attr(g, 'pcdata', 'spnum')), ('1234', '7'))
    check('DP DJ points and plays', (attr(g, 'pcdata', 'dach'), attr(g, 'pcdata', 'dpnum')), ('55', '2'))
    # dans: 9th cleared (4 stages), 10th failed at stage 2, 8th on DP cleared
    call(f'<grade method="raised" iidxid="{a}" gtype="0" gid="9" cflg="4" achi="88"/>')
    call(f'<grade method="raised" iidxid="{a}" gtype="0" gid="10" cflg="2" achi="40"/>')
    call(f'<grade method="raised" iidxid="{a}" gtype="0" gid="10" cflg="1" achi="20"/>')   # worse: not kept
    call(f'<grade method="raised" iidxid="{a}" gtype="1" gid="8" cflg="4" achi="70"/>')
    g = call('<pc method="get" rid="48524B00000000A1"/>')
    check('grades', (attr(g, 'grade', 'sgid'), attr(g, 'grade', 'dgid')), ('9', '8'))
    check('grade list', sorted(re.findall(r'<g __type="u8" __count="4">([^<]*)</g>', g)), ['0 10 2 40', '0 9 4 88', '1 8 4 70'])
    g = call('<pc method="get" rid="48524B00000000A2"/>')
    check('no grade', (attr(g, 'grade', 'sgid'), attr(g, 'grade', 'dgid'), '<g ' in g), ('-1', '-1', False))
    # an EXPERT course: two players, a better and a worse run again
    e = call(f'<ranking method="entry" iidxid="{a}" coid="5" clid="2" pgnum="300" gnum="100" clr="1" cstage="4" opt="0"/>')
    check('entry alone', (attr(e, 'ranking', 'anum'), attr(e, 'ranking', 'jun')), ('1', '1'))
    e = call(f'<ranking method="entry" iidxid="{b}" coid="5" clid="2" pgnum="400" gnum="50" clr="1" cstage="4" opt="0"/>')
    check('entry second, above', (attr(e, 'ranking', 'anum'), attr(e, 'ranking', 'jun')), ('2', '1'))
    call(f'<ranking method="entry" iidxid="{a}" coid="5" clid="2" pgnum="10" gnum="10" clr="0" cstage="1" opt="0"/>')
    r = call('<ranking method="getranker" coid="5" clid="2"/>')
    check('rankers', re.findall(r'<ranker gnum="(\d+)" pgnum="(\d+)" name="(\w+)"', r), [('50', '400', 'BRAVO'), ('100', '300', 'ALPHA')])
    r = call('<ranking method="getranker" coid="6" clid="2"/>')
    check('another course empty', '<ranker' in r, False)
    # the same course rankings for DistorteD (its answers named after the model), GOLD and DJ TROOPERS
    for M, el in (('FDD:J:A:G', 'FDD'), ('GLD:J:A:A:2007072301', 'ranking'), ('HDD:J:A:A:2008031100', 'ranking')):
        a, b = (attr(call(f'<pc method="reg" rid="{rid}" name="{name}" pid="13" did="0"/>'), 'FDD' if el == 'FDD' else 'pc', 'id')
                for rid, name in (('48524B00000000B1', 'CHARLIE'), ('48524B00000000B2', 'DELTA')))
        call(f'<ranking method="entry" iidxid="{a}" coid="3" clid="1" pgnum="200" gnum="10" clr="1" cstage="4" opt="0"/>')
        e = call(f'<ranking method="entry" iidxid="{b}" coid="3" clid="1" pgnum="100" gnum="5" clr="1" cstage="4" opt="0"/>')
        check(M[:3] + ' entry second, below', (attr(e, el, 'anum'), attr(e, el, 'jun')), ('2', '2'))
        r = call('<ranking method="getranker" coid="3" clid="1"/>')
        check(M[:3] + ' rankers', re.findall(r'<ranker gnum="(\d+)" pgnum="(\d+)" name="(\w+)"', r), [('10', '200', 'CHARLIE'), ('5', '100', 'DELTA')])
finally:
    srv.terminate(); srv.wait()
    shutil.rmtree(D, ignore_errors=True)
sys.exit(1 if fails else 0)
