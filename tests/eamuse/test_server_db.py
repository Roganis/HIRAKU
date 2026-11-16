#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""hiraku-server's database while it is used: a backup taken while serve answers calls that write
to it is a whole database, and two programs putting things in at once both get them kept.

    tests/eamuse/test_server_db.py HIRAKU-SERVER
"""
import os, shutil, socket, subprocess, sys, tempfile, threading, time

SRCID = '0102030405060708090A'
failed = 0


def check(ok, what):
    global failed
    print(('ok    ' if ok else 'FAIL  ') + what)
    failed += not ok


def call(port, body):
    """a call in clear, pop'n 9's way: the answer's body"""
    data = f'<?xml version="1.0"?><call model="C00:J:A:A" srcid="{SRCID}">{body}</call>'.encode()
    with socket.create_connection(('127.0.0.1', port), timeout=20) as s:
        s.sendall(b'POST / HTTP/1.0\r\nHost: cabinet.test:8080\r\nContent-Length: %d\r\n\r\n' % len(data) + data)
        out = b''
        while True:
            d = s.recv(65536)
            if not d:
                break
            out += d
    return out.partition(b'\r\n\r\n')[2].decode('latin-1')


def card(n):
    return '20030100000%05d' % n


def main():
    program = sys.argv[1]
    top = tempfile.mkdtemp(prefix='hk-server-db-')
    db = os.path.join(top, 'eamuse.db')
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        port = s.getsockname()[1]
    serve = subprocess.Popen([program, 'serve', '--db', db, '--port', str(port), '--bind', '127.0.0.1', '--quiet', '--no-relay'],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(300):
            try:
                socket.create_connection(('127.0.0.1', port)).close()
                break
            except OSError:
                if serve.poll() is not None:
                    raise SystemExit(f'{program} serve ended at start (status {serve.returncode})')
                time.sleep(0.1)

        # 1. backups while calls write a card's data each
        answers, stop = [], threading.Event()

        def writer():
            n = 0
            while not stop.is_set() or n < 40:
                n += 1
                answers.append(call(port, f'<userdata method="write" card="{card(n)}" time="2003-06-02 06:42:41+9" model="C00:J:A:A" '
                                          f'label=""><b>CARD{n}</b></userdata>'))

        t = threading.Thread(target=writer)
        t.start()
        backups = []
        for i in range(5):
            time.sleep(0.05)
            to = os.path.join(top, f'backup{i}.db')
            done = subprocess.run([program, 'backup', to, '--db', db], capture_output=True)
            backups.append((to, done.returncode, done.stderr.decode(errors='replace').strip()))
        stop.set()
        t.join()
        check(all(r == 0 for _, r, _ in backups), 'backup: made while serve answers (' + '; '.join(e for _, _, e in backups if e) + ')')
        check(len(answers) >= 40 and all('<userdata status="0">' in a for a in answers), f'serve: {len(answers)} calls answered meanwhile')
        whole = True
        for to, _, _ in backups:
            got = subprocess.run([program, 'players', '--db', to], capture_output=True)
            exported = os.path.join(top, 'out-' + os.path.basename(to))
            ex = subprocess.run([program, 'export', exported, '--db', to], capture_output=True)
            whole = whole and got.returncode == 0 and ex.returncode == 0
            # each card's data in a backup is the whole of what its call wrote (or none at all)
            for name in os.listdir(exported) if ex.returncode == 0 else []:
                if name.startswith('userdata-'):
                    with open(os.path.join(exported, name), 'rb') as f:
                        whole = whole and f.read().endswith(b'</userdata>')
        check(whole, 'backup: each copy a whole database, read and exported')
        last = subprocess.run([program, 'get', f'userdata-{card(40)}', '--db', db], capture_output=True)
        check(b'CARD40' in last.stdout, 'serve: what the calls wrote is there')

        # 2. two programs putting things at once (and the same thing, the last's kept whole)
        def puts(who, results):
            for i in range(15):
                p = subprocess.run([program, 'put', f'eemall-{who}{i:03d}', '--db', db], input=f'{who} {i}\n'.encode(), capture_output=True)
                q = subprocess.run([program, 'put', 'message', '--db', db], input=f'{who * 40}\n'.encode(), capture_output=True)
                results.append(p.returncode or q.returncode)

        ra, rb = [], []
        a = threading.Thread(target=puts, args=('A', ra))
        b = threading.Thread(target=puts, args=('B', rb))
        a.start(), b.start()
        a.join(), b.join()
        check(ra and rb and not any(ra + rb), 'put: two programs at once, every put done')
        good = all(subprocess.run([program, 'get', f'eemall-{w}{i:03d}', '--db', db], capture_output=True).stdout == f'{w} {i}\n'.encode()
                   for w in 'AB' for i in range(15))
        msg = subprocess.run([program, 'get', 'message', '--db', db], capture_output=True).stdout
        check(good and msg in (b'A' * 40 + b'\n', b'B' * 40 + b'\n'), 'put: each kept, the one both wrote whole')
        check(subprocess.run([program, 'put', '../x', '--db', db], input=b'x', capture_output=True).returncode != 0 and
              subprocess.run([program, 'export', top, '--db', db], capture_output=True).returncode != 0,
              'put refuses a name no table has; export refuses a folder that is not empty')
    finally:
        serve.terminate()
        serve.wait()
        shutil.rmtree(top, ignore_errors=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
