#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""hiraku-eamuse with a key (key.txt, --new-key; runtime/shell/keyed.h): cabinets that have it are
answered, behind it; a call in clear, another key or a changed byte gets nothing, and the server
says who it refused. What goes over the network holds no PCBID, card or PIN. The same for NET
TAISEN's relay. Made-up calls; key_client is a cabinet's side.

    tests/eamuse/test_key.py [build/cmake/hiraku-eamuse build/cmake/tests/key_client]      exit 0 when all hold
"""
import os, shutil, socket, stat, subprocess, sys, tempfile, threading, time
HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', '..', 'build', 'cmake', 'hiraku-eamuse')
CLIENT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, '..', '..', 'build', 'cmake', 'tests', 'key_client')
T = tempfile.mkdtemp(prefix='hk-key-')
failed = 0


def check(ok, what):
    global failed
    print(('ok    ' if ok else 'FAIL  ') + what, flush=True)
    failed += not ok


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class Server:
    def __init__(self, data, *options, relay=False):
        self.port, self.data = free_port(), data
        os.makedirs(data, exist_ok=True)
        self.log = open(os.path.join(T, f'log-{self.port}.txt'), 'w+')
        self.p = subprocess.Popen([SERVER, '--port', str(self.port), '--bind', '127.0.0.1', '--data', data, '--quiet', *options,
                                   *([] if relay else ['--no-relay'])], stdout=self.log, stderr=subprocess.STDOUT)
        for _ in range(300):
            if self.p.poll() is not None: break
            try:
                socket.create_connection(('127.0.0.1', self.port)).close(); break
            except OSError:
                time.sleep(0.1)

    def said(self):
        self.log.flush()
        with open(self.log.name) as f:
            return f.read()

    def stop(self):
        if self.p.poll() is None:
            self.p.terminate()
            self.p.wait(5)
        self.log.close()


MINE, OTHER, CARD, PIN = '01A1B2C3D4E5F6071899', '0102030405060708090A', 'E004010000ABCDEF', '4711'


def request(srcid=MINE, inner=f'<cardmng method="inquire" cardid="{CARD}" pass="{PIN}"/>'):
    body = f'<?xml version="1.0"?><call model="G15:J:A:A" srcid="{srcid}">{inner}</call>'.encode()
    return b'POST / HTTP/1.0\r\nContent-Length: %d\r\n\r\n' % len(body) + body


def plain(port, req=None, wait=5.0):
    """a call in clear: the whole answer, b'' when the server closed without one"""
    try:
        s = socket.create_connection(('127.0.0.1', port), timeout=wait)
        s.sendall(req or request())
        out = b''
        while (d := s.recv(65536)): out += d
        s.close()
        return out
    except OSError:
        return b''


def client(mode, port, key, *more, req=None, wait=20):
    r = subprocess.run([CLIENT, mode, '127.0.0.1', str(port), key, *more], input=req if req is not None else request(),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=wait)
    return r.stdout


def answered(a): return a.startswith(b'HTTP/1.0 200') and b'<response' in a


class Tap:
    """A port passed on to the server's, keeping what went each way (someone listening on the way);
    flip: the number of a byte of the cabinet's to change on its way (someone doing more than listen)"""
    def __init__(self, to, flip=None):
        self.to, self.flip, self.up, self.down = to, flip, b'', b''
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
            threading.Thread(target=self.pump, args=(a, b, True), daemon=True).start()
            threading.Thread(target=self.pump, args=(b, a, False), daemon=True).start()

    def pump(self, src, dst, up):
        try:
            while (d := src.recv(65536)):
                if up:
                    if self.flip is not None and len(self.up) <= self.flip < len(self.up) + len(d):
                        i = self.flip - len(self.up)
                        d = d[:i] + bytes([d[i] ^ 1]) + d[i + 1:]
                    self.up += d
                else:
                    self.down += d
                dst.sendall(d)
        except OSError:
            pass
        for s in (src, dst):
            try: s.shutdown(socket.SHUT_RDWR)
            except OSError: pass


try:
    # --- the key's file ---
    data = os.path.join(T, 'keyed')
    r = subprocess.run([SERVER, '--data', data, '--new-key'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    KEY = r.stdout.decode().strip()
    file = os.path.join(data, 'key.txt')
    check(r.returncode == 0 and len(KEY) == 29 and KEY in open(file).read(), '--new-key: a key printed and written to key.txt in the data folder')
    if os.name != 'nt':
        check(stat.S_IMODE(os.stat(file).st_mode) == 0o600, '... a file only its owner reads')
    r2 = subprocess.run([SERVER, '--data', data, '--new-key'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    check(r2.returncode != 0 and KEY in open(file).read(), '... a second --new-key leaves the key that is there')
    weak = os.path.join(T, 'weak')
    os.makedirs(weak)
    open(os.path.join(weak, 'key.txt'), 'w').write('hunter2\n')
    s = Server(weak)
    time.sleep(0.3)
    check(s.p.poll() not in (None, 0) and 'soon guessed' in s.said(), 'a key of a few characters: the server doesn\'t start, and says why')
    s.stop()
    s = Server(os.path.join(T, 'nofile'), '--key-file', os.path.join(T, 'nowhere.txt'))
    time.sleep(0.3)
    check(s.p.poll() not in (None, 0), '--key-file naming no file: the server doesn\'t start (never without the key it was meant to have)')
    s.stop()

    # --- calls ---
    WRONG = KEY[:-1] + ('a' if KEY[-1] != 'a' else 'b')
    s = Server(data)
    check('only behind the key' in s.said(), 'the server says at its start that it takes calls only behind the key')
    a = client('call', s.port, KEY)
    check(answered(a), 'a cabinet with the key: answered')
    check(plain(s.port) == b'', 'a call in clear: nothing')
    check('refused: a connection from 127.0.0.1 (e-AMUSE3) without the key' in s.said(), '... and the server says so')
    a = client('call', s.port, WRONG)
    check(a.startswith(b'error:') and b'another' in a, 'a cabinet with another key: no answer, and it is told the keys differ')
    a = client('garbled', s.port, KEY)
    check(a.startswith(b'error: no answer'), 'a call with a bit changed on its way: nothing')
    time.sleep(0.2)
    check('(e-AMUSE3) with another key' in s.said(), '... and the server says so')
    check(answered(client('call', s.port, KEY)), 'the cabinet with the key is still answered after all that')

    # someone on the way
    tap = Tap(s.port)
    a = client('call', tap.port, KEY)
    seen = tap.up + tap.down
    check(answered(a) and len(tap.up) > 100 and len(tap.down) > 100, 'through a listener on the way: answered')
    check(not any(x.encode() in seen for x in (MINE, CARD, PIN, 'cardmng', 'srcid', 'HTTP', 'response')),
          '... who saw no PCBID, card number, PIN, call name or answer')
    tap = Tap(s.port, flip=60)
    a = client('call', tap.port, KEY)
    check(a.startswith(b'error:'), '... and who changes a byte of the call gets it refused')
    tap = Tap(s.port, flip=7)
    a = client('call', tap.port, KEY)
    check(a.startswith(b'error:'), '... as who changes a byte of the hello')

    # a PC game: its own connections, in clear, to a port on this computer that passes them on
    f = subprocess.Popen([CLIENT, 'forward', '127.0.0.1', str(s.port), KEY], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    fport = int(f.stdout.readline().split()[1])
    check(answered(plain(fport)), 'a port passed on behind the key (a PC game\'s way): a call in clear to it is answered')
    check(answered(plain(fport)) and answered(plain(fport)), '... and the next ones')
    f.stdin.close(); f.wait(5)
    g = subprocess.Popen([CLIENT, 'forward', '127.0.0.1', str(s.port), WRONG], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    gport = int(g.stdout.readline().split()[1])
    check(plain(gport) == b'', '... with another key: nothing')
    g.stdin.close(); g.wait(5)
    check(b'another' in g.stderr.read(), '... and the cabinet\'s log says the keys differ')
    s.stop()

    # the key and the cabinets' list together: both must hold
    open(os.path.join(data, 'cabinets.txt'), 'w').write(MINE + '  mine\n')
    s = Server(data)
    check(answered(client('call', s.port, KEY)), 'with cabinets.txt too: a listed cabinet with the key is answered')
    a = client('call', s.port, KEY, req=request(OTHER))
    check(a.startswith(b'HTTP/1.0 403'), '... a cabinet with the key that the list doesn\'t name is not')
    s.stop()
    os.remove(os.path.join(data, 'cabinets.txt'))

    # a server without a key
    s = Server(os.path.join(T, 'open'))
    check(answered(plain(s.port)), 'a server without a key: a call in clear is answered, as before')
    a = client('call', s.port, KEY)
    check(a.startswith(b'error:') and b'no key' in a, '... a cabinet with a key is told the server has none')
    time.sleep(0.2)
    check('with a key, and this server has none' in s.said(), '... and the server says so')
    s.stop()

    # --- NET TAISEN's relay (its port is fixed: tried only when this server got it) ---
    s = Server(data, relay=True)
    time.sleep(0.3)
    if 'lobby and relay on' not in s.said():
        print('(the relay\'s port is in use here: not tried)')
    else:
        a = client('relay', 5740, '-')
        check(a.startswith(b'error:'), 'the relay: a cabinet without the key is not taken')
        time.sleep(0.2)
        check('(the relay) without the key' in s.said(), '... and the server says so')
        a = client('relay', 5740, WRONG)
        check(a.startswith(b'error:') and b'another' in a, '... nor one with another key')
        # two cabinets with the key: one's datagram reaches the other
        b = subprocess.Popen([CLIENT, 'relay', '127.0.0.1', '5740', KEY], stdout=subprocess.PIPE)
        addr = b.stdout.readline().decode().split()
        check(len(addr) == 2 and addr[0] == 'address' and addr[1].startswith('10.57.'), 'a cabinet with the key is given its address')
        a = client('relay', 5740, KEY, addr[1] if len(addr) == 2 else '10.57.0.1')
        got = b.stdout.readline().decode().strip()
        b.wait(10)
        check(a.startswith(b'address 10.57.') and got == 'got hello', '... and another\'s datagram reaches it')
        # in clear, the relay's own hello ("HKPR", version 1): closed
        c = socket.create_connection(('127.0.0.1', 5740), timeout=3)
        c.sendall(b'RPKH' + bytes([1, 0, 0, 0, 0, 0, 0, 0]))
        c.settimeout(3)
        try:
            closed = c.recv(16) == b''
        except OSError:
            closed = False
        c.close()
        check(closed, '... and a relay hello in clear gets the connection closed, no address')
    s.stop()
    s = Server(os.path.join(T, 'open'), relay=True)
    time.sleep(0.3)
    if 'lobby and relay on' in s.said():
        check(client('relay', 5740, '-').startswith(b'address 10.57.'), 'a relay without a key: a cabinet without one is taken, as before')
        a = client('relay', 5740, KEY)
        check(a.startswith(b'error:') and b'no key' in a, '... a cabinet with a key is told the server has none')
    s.stop()
finally:
    shutil.rmtree(T, ignore_errors=True)
print(f'{failed} failed' if failed else 'all hold')
sys.exit(1 if failed else 0)
