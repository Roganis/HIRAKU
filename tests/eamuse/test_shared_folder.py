#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Several servers on one data folder (each cabinet's own on one PC; hiraku-eamuse more than once):
a call reads, changes and writes back the folder's files, so the folder is one call's at a time and
no server's write undoes another's. And what a server keeps is bounded: cards, files, a card's data.
Made-up calls.

    tests/eamuse/test_shared_folder.py [build/cmake/hiraku-eamuse]      exit 0 when all hold
"""
import os, re, shutil, socket, subprocess, sys, tempfile, threading, time
SERVER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'build', 'cmake', 'hiraku-eamuse')
T = tempfile.mkdtemp(prefix='hk-shared-')
fails = 0


def check(ok, what):
    global fails
    fails += not ok
    print(('ok    ' if ok else 'FAIL  ') + what, flush=True)


class Server:
    def __init__(self, data):
        with socket.socket() as s0:
            s0.bind(('127.0.0.1', 0)); self.port = s0.getsockname()[1]
        self.log = open(os.path.join(T, f'said-{self.port}.txt'), 'w+')
        self.p = subprocess.Popen([SERVER, '--port', str(self.port), '--data', data, '--quiet', '--no-relay'], stdout=subprocess.DEVNULL, stderr=self.log)
        for _ in range(300):
            try:
                socket.create_connection(('127.0.0.1', self.port)).close(); break
            except OSError:
                time.sleep(0.1)

    def call(self, model, body):
        x = f'<?xml version="1.0"?><call model="{model}" srcid="0"><{body}</call>'.encode()
        s = socket.create_connection(('127.0.0.1', self.port))
        s.sendall(b'POST / HTTP/1.0\r\nContent-Length: %d\r\n\r\n' % len(x) + x)
        out = b''
        while (d := s.recv(65536)): out += d
        return out.split(b'\r\n\r\n', 1)[-1].decode('cp932', 'replace')

    def said(self):
        self.log.flush()
        with open(self.log.name) as f:
            return f.read()

    def stop(self):
        self.p.terminate(); self.p.wait(5); self.log.close()


try:
    # two servers, one folder: new cards read at both at once
    D = os.path.join(T, 'shared')
    a, b = Server(D), Server(D)
    N = 150
    refs, numbers = [], []

    def cards(server, first):
        for i in range(N):
            r = server.call('E13:J:A:A', f'cardmng method="inquire" cardid="E0040100{first + i:08X}" cardtype="1" update="0"></cardmng>')
            refs.append(re.search(r'refid="([^"]*)"', r).group(1) if 'refid=' in r else None)
            r = server.call('C00:J:A:A', 'numbering method="assign"></numbering>')
            numbers.append(re.search(r'number="([^"]*)"', r).group(1) if 'number=' in r else None)

    threads = [threading.Thread(target=cards, args=(a, 0x1000)), threading.Thread(target=cards, args=(b, 0x2000))]
    for t in threads: t.start()
    for t in threads: t.join()
    kept = [l for l in open(os.path.join(D, 'cards')).read().splitlines() if l]
    check(len(kept) == 2 * N and len({re.search(r'cardid=(\S+)', l).group(1) for l in kept}) == 2 * N, f'{2 * N} new cards read at two servers at once: every card is kept ({len(kept)})')
    check(None not in refs and len(set(refs)) == 2 * N, f'... each with a ref id of its own ({len(set(refs))} different)')
    check(open(os.path.join(D, 'refid')).read().strip() == str(2 * N + 1), '... and the count after them all')
    check(None not in numbers and len(set(numbers)) == 2 * N, f'{2 * N} card numbers asked for at once: all different ({len(set(numbers))})')
    a.stop(); b.stop()

    # what a server keeps is bounded
    D = os.path.join(T, 'bounded')
    os.makedirs(D)
    open(os.path.join(D, 'limit-cards'), 'w').write('3\n')
    s = Server(D)
    got = [s.call('E13:J:A:A', f'cardmng method="inquire" cardid="E00401000000300{i}" cardtype="1" update="0"></cardmng>') for i in range(5)]
    check(all('status="0"' in g for g in got[:3]) and all('status="1"' in g and 'refid' not in g for g in got[3:]), 'limit-cards 3: three new cards taken, the fourth and fifth refused')
    check('status="0"' in s.call('E13:J:A:A', 'cardmng method="inquire" cardid="E004010000003001" cardtype="1" update="0"></cardmng>'), '... a card it knows is still read')
    check(len(open(os.path.join(D, 'cards')).read().splitlines()) == 3 and 'limit-cards' in s.said(), '... three cards kept, and the server says why')
    big = 'A' * 0x20001
    r = s.call('C00:J:A:A', f'userdata method="write" card="2003010000000019" time="1"><b>{big}</b></userdata>')
    check('status="1"' in r and not os.path.exists(os.path.join(D, 'userdata-2003010000000019')), "a card's data over 128 KiB: refused, nothing kept")
    r = s.call('C00:J:A:A', 'userdata method="write" card="2003010000000019" time="1"><b>QUJD</b></userdata>')
    check('status="0"' in r and os.path.exists(os.path.join(D, 'userdata-2003010000000019')), "... a card's record of its usual size is kept")
    s.stop()
    files = len(os.listdir(D))
    open(os.path.join(D, 'limit-files'), 'w').write(f'{files + 2}\n')   # (the limit's own file, and one more)
    s = Server(D)
    for i in range(4):
        s.call('C00:J:A:A', f'userdata method="write" card="20030100000001{i}9" time="1"><b>QUJD</b></userdata>')
    made = sorted(f for f in os.listdir(D) if f.startswith('userdata-20030100000001'))
    check(len(made) == 1, f'limit-files: one more file made, then no new ones ({len(made)})')
    r = s.call('C00:J:A:A', 'userdata method="write" card="2003010000000019" time="2"><b>REVG</b></userdata>')
    check('REVG' in open(os.path.join(D, 'userdata-2003010000000019')).read() and 'limit-files' in s.said(), '... a file already there is still written, and the server says why')
    s.stop()
finally:
    shutil.rmtree(T, ignore_errors=True)
print(f'{fails} failed' if fails else 'all hold')
sys.exit(1 if fails else 0)
