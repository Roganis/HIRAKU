#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""hiraku-server's key per machine (runtime/shell/keyed.h, server/accounts.h): machine add prints
the machine's key once (hk2.PCBID.SECRET), a cabinet with it is answered behind it and as itself
only; a disabled machine, an old key after machine key, a key for all, no key: refused once the
database has machines; --no-keys answers by the PCBID a call says (as before keys). The relay the
same way. hiraku-eamuse turns a machine's key away. Nothing of a call is readable on the wire.
Made-up calls; key_client is a cabinet's side.

    tests/eamuse/test_machine_keys.py HIRAKU-SERVER KEY_CLIENT [HIRAKU-EAMUSE]      exit 0 when all hold
"""
import os, re, shutil, socket, subprocess, sys, tempfile, threading, time
SERVER, CLIENT = sys.argv[1], sys.argv[2]
EAMUSE = sys.argv[3] if len(sys.argv) > 3 else None
T = tempfile.mkdtemp(prefix='hk-mkey-')
failed = 0
CARD, PIN = 'E004010000ABCDEF', '4711'
SHARED = 'k7m2x-pq9rs-tuv3w-xyz45-abcde'   # a key for all, made up


def check(ok, what):
    global failed
    print(('ok    ' if ok else 'FAIL  ') + what, flush=True)
    failed += not ok


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def cmd(db, *args):
    """hiraku-server ... --db db: (status, stdout, stderr)"""
    r = subprocess.run([SERVER, *args, '--db', db], capture_output=True, stdin=subprocess.DEVNULL)
    return r.returncode, r.stdout.decode(errors='replace'), r.stderr.decode(errors='replace')


class Serve:
    def __init__(self, program, *options, relay=False):
        self.port = free_port()
        self.log = open(os.path.join(T, f'log-{self.port}.txt'), 'w+')
        self.p = subprocess.Popen([program, *options, '--port', str(self.port), '--bind', '127.0.0.1', '--quiet',
                                   *([] if relay else ['--no-relay'])], stdout=self.log, stderr=subprocess.STDOUT)
        for _ in range(300):
            if self.p.poll() is not None: break
            try:
                socket.create_connection(('127.0.0.1', self.port)).close(); break
            except OSError:
                time.sleep(0.1)
        if relay:
            time.sleep(0.3)

    def said(self):
        time.sleep(0.2)
        self.log.flush()
        with open(self.log.name) as f:
            return f.read()

    def stop(self):
        if self.p.poll() is None:
            self.p.terminate()
            self.p.wait(5)
        self.log.close()


def request(srcid):
    body = f'<?xml version="1.0"?><call model="G15:J:A:A" srcid="{srcid}"><cardmng method="inquire" cardid="{CARD}" pass="{PIN}"/></call>'.encode()
    return b'POST / HTTP/1.0\r\nContent-Length: %d\r\n\r\n' % len(body) + body


def plain(port, req):
    try:
        s = socket.create_connection(('127.0.0.1', port), timeout=5)
        s.sendall(req)
        out = b''
        while (d := s.recv(65536)): out += d
        s.close()
        return out
    except OSError:
        return b''


def client(mode, port, key, *more, req=b''):
    return subprocess.run([CLIENT, mode, '127.0.0.1', str(port), key, *more], input=req, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=20).stdout


def answered(a): return a.startswith(b'HTTP/1.0 200') and b'<response' in a


class Tap:
    """a port passed on to the server's, keeping what went each way"""
    def __init__(self, to):
        self.to, self.seen = to, b''
        self.ls = socket.socket()
        self.ls.bind(('127.0.0.1', 0))
        self.ls.listen(4)
        self.port = self.ls.getsockname()[1]
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        while True:
            try:
                a, _ = self.ls.accept()
            except OSError:
                return
            b = socket.create_connection(('127.0.0.1', self.to))
            for src, dst in ((a, b), (b, a)):
                threading.Thread(target=self.pump, args=(src, dst), daemon=True).start()

    def pump(self, src, dst):
        try:
            while (d := src.recv(65536)):
                self.seen += d
                dst.sendall(d)
        except OSError:
            pass
        for s in (src, dst):
            try: s.shutdown(socket.SHUT_RDWR)
            except OSError: pass


s = None
try:
    db = os.path.join(T, 'one', 'server.db')
    os.makedirs(os.path.dirname(db))
    os.makedirs(os.path.join(T, 'empty'))
    cmd(db, 'import', os.path.join(T, 'empty'))   # (the database made, its first admin said)
    arcade = cmd(db, 'arcade', 'add', 'SHOP ONE', '13')[1].strip().splitlines()[-1]
    st, out, err = cmd(db, 'machine', 'add', arcade, 'CAB ONE')
    K1 = out.strip()
    ID1 = K1.split('.')[1] if K1.count('.') == 2 else ''
    check(st == 0 and out.count('\n') == 1 and K1.startswith('hk2.') and len(ID1) == 20 and K1 not in err,
          'machine add: one line on stdout, the key (hk2.PCBID.SECRET), not on stderr')
    K2 = cmd(db, 'machine', 'add', arcade, 'CAB TWO')[1].strip()
    K3 = cmd(db, 'machine', 'add', arcade, 'CAB THREE')[1].strip()
    ID2, ID3 = K2.split('.')[1], K3.split('.')[1]
    listed = cmd(db, 'machine', 'list')[1]
    check(listed.count('\tkey\t') == 3 and K1.split('.')[2] not in listed, 'machine list: each has a key, which it doesn\'t show')

    # --- serve, machines with keys ---
    s = Serve(SERVER, 'serve', '--db', db)
    check('each behind its own key' in s.said(), 'serve says it takes each machine behind its own key')
    check(answered(client('call', s.port, K1, req=request(ID1))), 'a call behind the machine\'s key, as that machine: answered')
    line = [l for l in cmd(db, 'machine', 'list')[1].splitlines() if l.startswith(ID1)]
    check(line and line[0].split('\t')[-1] != '-', '... and its last_seen noted')
    check(answered(client('remote', s.port, K1, req=request(ID1))), '... through ea::Remote, as a core sends it: answered')
    f = subprocess.Popen([CLIENT, 'forward', '127.0.0.1', str(s.port), K1], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    fport = int(f.stdout.readline().split()[1])
    check(answered(plain(fport, request(ID1))), '... through a port passed on behind it (a PC game\'s way): answered')
    f.stdin.close(); f.wait(5)
    a = client('call', s.port, K1, req=request(ID2))
    check(a.startswith(b'HTTP/1.0 403'), 'the same key with another machine\'s srcid: refused (403)')
    check('a machine\'s key is for its own calls only' in s.said(), '... and the server says so')
    check(plain(s.port, request(ID1)) == b'', 'a call in clear, by an enabled machine\'s PCBID: nothing')
    a = client('call', s.port, SHARED, req=request(ID1))
    check(a.startswith(b'error:') and b'machine' in a, 'a key for all: refused, and the cabinet is told machines\' own keys are wanted')
    a = client('call', s.port, 'hk2.0120FFFFFFFFFFFFFF00.' + SHARED, req=request('0120FFFFFFFFFFFFFF00'))
    check(a.startswith(b'error:') and b"doesn't take this machine" in a, 'a machine\'s key the database has no machine for: refused, the cabinet told so (HKKU)')
    check('which isn\'t an enabled machine of the database with a key' in s.said(), '... and the server says so')
    cmd(db, 'machine', 'disable', ID2)
    a = client('call', s.port, K2, req=request(ID2))
    check(a.startswith(b'error:') and b"doesn't take this machine (unknown or switched off" in a and b'another' not in a,
          'a disabled machine\'s key: refused, the cabinet told the machine isn\'t taken (not a wrong key)')
    st, out, err = cmd(db, 'machine', 'key', ID1)
    K1b = out.strip()
    check(st == 0 and out.count('\n') == 1 and K1b.startswith('hk2.' + ID1 + '.') and K1b != K1, 'machine key: a new key, one line on stdout')
    a = client('call', s.port, K1, req=request(ID1))
    check(a.startswith(b'error:') and b'another' in a and b"doesn't take" not in a, '... the old one refused at once (the keys differ), told so')
    said = s.said()
    check(f"another key than machine {ID1}'s (an old one?)" in said, '... and the server says so (the cabinet closed at its hello)')
    check(f"with machine 0120FFFFFFFFFFFFFF00's key, which isn't" in said and f"with machine {ID2}'s key, which isn't" in said,
          'refusals within a minute from one address: each machine\'s said (once a minute by reason and by whom)')
    stamped = [l for l in said.splitlines() if 'refused:' in l]
    check(stamped and all(re.match(r'\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ hiraku-server: refused: ', l) for l in stamped),
          '... each refusal line dated (ISO 8601, UTC)')
    check(answered(client('call', s.port, K1b, req=request(ID1))), '... the new one answered')
    tap = Tap(s.port)
    a = client('call', tap.port, K1b, req=request(ID1))
    check(answered(a) and len(tap.seen) > 200 and not any(x.encode() in tap.seen for x in (ID1, CARD, PIN, 'cardmng', 'srcid', 'HTTP', 'response')),
          'through a listener on the way: answered, and it saw no PCBID, card, PIN, call or answer')
    log = s.said()
    s.stop(); s = None
    keys = [K1, K2, K3, K1b]
    secrets = [k.split('.')[2] for k in keys]
    _, audit, _ = cmd(db, 'audit')
    check('machine.key' in audit and not any(x in audit or x in log for x in secrets), 'the audit log says a key was made; no key in it or in serve\'s output')
    raw = b''.join(open(p, 'rb').read() for p in (db, db + '-wal') if os.path.exists(p))
    check(not any(x.encode() in raw for x in secrets), 'the database holds no key\'s text')

    # --- --no-keys: by the PCBID a call says (step 2's rule) ---
    s = Serve(SERVER, 'serve', '--db', db, '--no-keys')
    check(answered(plain(s.port, request(ID1))), '--no-keys: a call in clear from an enabled machine\'s PCBID: answered')
    check(plain(s.port, request(ID2)).startswith(b'HTTP/1.0 403'), '... a disabled machine\'s: refused')
    check(plain(s.port, request('0120FFFFFFFFFFFFFF00')).startswith(b'HTTP/1.0 403'), '... a PCBID no machine has: refused')
    s.stop(); s = None

    # --- a key for all and the machines (key.txt beside the database) ---
    shared_db = os.path.join(T, 'two', 'server.db')
    os.makedirs(os.path.dirname(shared_db))
    st, out, _ = cmd(shared_db, 'serve', '--new-key')
    KEY = out.strip()
    s = Serve(SERVER, 'serve', '--db', shared_db)
    check(answered(client('call', s.port, KEY, req=request(ID1))), 'a key for all (key.txt) and no machines: taken as before')
    check(client('call', s.port, 'hk2.0120FFFFFFFFFFFFFF00.' + SHARED, req=request('0120FFFFFFFFFFFFFF00')).startswith(b'error:'),
          '... a machine\'s key with no such machine: refused')
    s.stop(); s = None
    arcade2 = cmd(shared_db, 'arcade', 'add', 'SHOP TWO', '13')[1].strip().splitlines()[-1]
    K4 = cmd(shared_db, 'machine', 'add', arcade2, 'CAB')[1].strip()
    s = Serve(SERVER, 'serve', '--db', shared_db)
    a = client('call', s.port, KEY, req=request(K4.split('.')[1]))
    check(a.startswith(b'error:') and b'machine' in a, 'once there are machines, the key for all is refused')
    check(answered(client('call', s.port, K4, req=request(K4.split('.')[1]))), '... and the machine\'s own taken')
    s.stop(); s = None

    # --- hiraku-eamuse: one key for all, or none ---
    if EAMUSE:
        for name, opts, said in (('one key', [], 'one key for all'), ('none', [], 'no key')):
            data = os.path.join(T, 'ea-' + name.replace(' ', ''))
            os.makedirs(data, exist_ok=True)
            if name == 'one key':
                subprocess.run([EAMUSE, '--data', data, '--new-key'], capture_output=True)
            s = Serve(EAMUSE, '--data', data, *opts)
            a = client('call', s.port, K1b, req=request(ID1))
            check(a.startswith(b'error:') and (b'one key for all' in a if name == 'one key' else b'no key' in a),
                  f'hiraku-eamuse with {name}: a machine\'s key is turned away, and the cabinet told why')
            check(said in s.said(), '... and the server says so')
            s.stop(); s = None

    # --- NET TAISEN's relay (its port is fixed: tried only when this server got it) ---
    s = Serve(SERVER, 'serve', '--db', db, relay=True)
    if 'lobby and relay on' not in s.said():
        print('(the relay\'s port is in use here: not tried)')
    else:
        check(client('relay', 5740, '-').startswith(b'error:'), 'the relay: a cabinet without a key is not taken')
        check(client('relay', 5740, K1).startswith(b'error:'), '... nor one with an old key')
        a = client('relay', 5740, K2)
        check(a.startswith(b'error:') and b"doesn't take this machine" in a, '... nor a disabled machine\'s (told so)')
        check(client('relay', 5740, SHARED).startswith(b'error:'), '... nor a key for all')
        b = subprocess.Popen([CLIENT, 'relay', '127.0.0.1', '5740', K3], stdout=subprocess.PIPE)
        addr = b.stdout.readline().decode().split()
        check(len(addr) == 2 and addr[1].startswith('10.57.'), 'a machine with its key is given its address')
        a = client('relay', 5740, K1b, addr[1] if len(addr) == 2 else '10.57.0.1')
        got = b.stdout.readline().decode().strip()
        b.wait(10)
        check(a.startswith(b'address 10.57.') and got == 'got hello', '... and another machine\'s datagram reaches it')
    s.stop(); s = None
    s = Serve(SERVER, 'serve', '--db', db, '--no-keys', relay=True)
    if 'lobby and relay on' in s.said():
        check(client('relay', 5740, '-').startswith(b'address 10.57.'), '--no-keys: the relay takes a cabinet without a key, as before')
        a = client('relay', 5740, K3)
        check(a.startswith(b'error:'), '... and turns a machine\'s key away')
    s.stop(); s = None
finally:
    if s:
        s.stop()
    shutil.rmtree(T, ignore_errors=True)
print(f'{failed} failed' if failed else 'all hold')
sys.exit(1 if failed else 0)
