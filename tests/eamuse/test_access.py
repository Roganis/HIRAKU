#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""hiraku-eamuse told which cabinets to answer (cabinets.txt, by PCBID) and which addresses to take
connections from (--allow): the others get nothing, and it says who it refused. Made-up calls.

    tests/eamuse/test_access.py [build/cmake/hiraku-eamuse]      exit 0 when all hold
"""
import os, re, shutil, socket, subprocess, sys, tempfile, time
SERVER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'build', 'cmake', 'hiraku-eamuse')
T = tempfile.mkdtemp(prefix='hk-access-')
failed = 0


def check(ok, what):
    global failed
    print(('ok    ' if ok else 'FAIL  ') + what, flush=True)
    failed += not ok


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def own_address():
    """This computer's address on its network (not 127.x), or None: no packet is sent to find it"""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(('192.0.2.1', 9))
            a = s.getsockname()[0]
            return None if a.startswith('127.') or a == '0.0.0.0' else a
    except OSError:
        return None


class Server:
    def __init__(self, data, *options, bind='127.0.0.1', relay=False):
        self.port, self.data, self.bind = free_port(), data, bind
        os.makedirs(data, exist_ok=True)
        self.log = open(os.path.join(T, f'log-{self.port}.txt'), 'w+')
        self.p = subprocess.Popen([SERVER, '--port', str(self.port), '--bind', bind, '--data', data, '--quiet', *options,
                                   *([] if relay else ['--no-relay'])], stdout=self.log, stderr=subprocess.STDOUT)
        for _ in range(300):
            try:
                socket.create_connection(('127.0.0.1', self.port)).close(); break
            except OSError:
                time.sleep(0.1)

    def post(self, body, to='127.0.0.1', wait=5.0):
        """the whole answer; b'' when the server closed without one; None when it couldn't be reached"""
        try:
            s = socket.create_connection((to, self.port), timeout=wait)
            s.sendall(b'POST / HTTP/1.0\r\nContent-Length: %d\r\n\r\n' % len(body) + body)
            out = b''
            while (d := s.recv(65536)): out += d
            s.close()
            return out
        except OSError:
            return b''

    def call(self, srcid, model='C00:J:A:A', inner='<ntp method="get"/>', to='127.0.0.1'):
        return self.post(f'<?xml version="1.0"?><call model="{model}" srcid="{srcid}">{inner}</call>'.encode(), to)

    def said(self):
        self.log.flush()
        with open(self.log.name) as f:
            return f.read()

    def stop(self):
        self.p.terminate()
        self.p.wait(5)
        self.log.close()


def answered(a): return a is not None and a.startswith(b'HTTP/1.0 200') and b'status="0"' in a
def refused(a): return a is not None and a.startswith(b'HTTP/1.0 403') and b'<' not in a


MINE, OTHER = '01A1B2C3D4E5F6071899', '0102030405060708090A'
try:
    # no list: every cabinet, as before
    s = Server(os.path.join(T, 'open'))
    check(answered(s.call(MINE)) and answered(s.call('0')), 'no cabinets.txt: every cabinet is answered')
    s.stop()

    # a list of cabinets
    d = os.path.join(T, 'listed')
    os.makedirs(d)
    with open(os.path.join(d, 'cabinets.txt'), 'w') as f:
        f.write(f'# the cabinets this server answers\n{MINE.lower()}  the living room\n\nnot an id\n')
    s = Server(d)
    check(answered(s.call(MINE)), 'a cabinet in cabinets.txt (written in small letters there): answered')
    check(answered(s.call(MINE.lower())), '... its PCBID in small letters in the call too')
    check(refused(s.call(OTHER)), 'a cabinet that is not: 403, and nothing to read')
    check(refused(s.call('0')) and refused(s.call('')), '... nor one with no PCBID')
    check(refused(s.call(OTHER, 'D40:J:A:A', '<gfdm method="check"/>')), '... on the GuitarFreaks and DrumMania server too')
    check(answered(s.call(MINE, 'D40:J:A:A', '<ntp method="get"/>')), '... which answers the listed one')
    time.sleep(0.2)
    log = s.said()
    check(f'refused: a call from 127.0.0.1 as cabinet {OTHER}' in log and 'cabinets.txt' in log, 'the server says which cabinet it refused, from where')
    check(log.count(f'as cabinet {OTHER}') == 1, '... once, not for each call')
    check('1 cabinet' in log and 'not a PCBID' in log, '... and at its start how many it answers, and the line it could not read')
    # the list changed while it runs
    time.sleep(1.1)   # (a file's time is told to the second on some disks)
    with open(os.path.join(d, 'cabinets.txt'), 'a') as f:
        f.write(f'{OTHER} the arcade\n')
    check(answered(s.call(OTHER)) and answered(s.call(MINE)), 'a cabinet added to the file while the server runs: answered, with no restart')
    s.stop()

    # --new-cabinet
    d = os.path.join(T, 'new')
    r = subprocess.run([SERVER, '--data', d, '--new-cabinet', 'the garage'], capture_output=True, text=True)
    made = r.stdout.strip()
    check(r.returncode == 0 and re.fullmatch(r'01[0-9A-F]{18}', made) is not None, '--new-cabinet: prints a PCBID')
    with open(os.path.join(d, 'cabinets.txt')) as f:
        check(f.read() == f'{made}  the garage\n', '... and adds it to cabinets.txt with its name')
    r2 = subprocess.run([SERVER, '--data', d, '--new-cabinet'], capture_output=True, text=True)
    check(r2.returncode == 0 and r2.stdout.strip() != made, '... another one each time')
    s = Server(d)
    check(answered(s.call(made)) and answered(s.call(r2.stdout.strip())) and refused(s.call(OTHER)), '... and the server answers those and no other')
    s.stop()
    r = subprocess.run([SERVER, '--data', d, '--cabinets', os.path.join(T, 'no-such.txt')], capture_output=True, text=True, timeout=10)
    check(r.returncode == 1 and 'no-such.txt' in r.stderr, '--cabinets with a file that is not there: the server does not start open')

    # addresses
    r = subprocess.run([SERVER, '--data', d, '--allow', '192.168.1.300'], capture_output=True, text=True, timeout=10)
    check(r.returncode == 2 and '192.168.1.300' in r.stderr, '--allow with what is not an address: said, not started')
    me = own_address()
    if not me:
        print('(this computer has no network address of its own: --allow not tried from one)')
    else:
        s = Server(os.path.join(T, 'allow-other'), '--allow', '203.0.113.0/24', bind='0.0.0.0', relay=False)
        check(answered(s.call(MINE)), '--allow another network: this computer itself (127.0.0.1) is still answered')
        a = s.call(MINE, to=me)
        check(a == b'', '... a connection from an address not named: closed, no answer')
        time.sleep(0.2)
        check(f'refused: a connection from {me}' in s.said(), '... and said')
        s.stop()
        net = '.'.join(me.split('.')[:3]) + '.0/24'
        s = Server(os.path.join(T, 'allow-mine'), '--allow', f'203.0.113.7, {net}', bind='0.0.0.0')
        check(answered(s.call(MINE, to=me)), f'--allow this computer\'s network ({net.split(".")[0]}.x.x.0/24): answered')
        check('connections taken from' in s.said(), '... and the server says at its start which it takes')
        s.stop()
        # NET TAISEN's relay (its port is fixed: tried only when this server got it)
        def relay_open(to):
            """whether a connection to the relay stays open (taken) or is closed at once (refused)"""
            try:
                c = socket.create_connection((to, 5740), timeout=3)
                c.settimeout(1.0)
                try:
                    return c.recv(16) != b''
                except socket.timeout:
                    return True
                finally:
                    c.close()
            except OSError:
                return False
        s = Server(os.path.join(T, 'relay'), '--allow', '203.0.113.0/24', bind='0.0.0.0', relay=True)
        time.sleep(0.3)
        if 'lobby and relay on' not in s.said():
            print('(the relay\'s port is in use here: not tried)')
        else:
            check(relay_open('127.0.0.1'), 'the relay: this computer is taken')
            check(not relay_open(me), '... an address --allow does not name is not')
            time.sleep(0.2)
            check(f'refused: a connection from {me} (the relay)' in s.said(), '... and said')
        s.stop()
finally:
    shutil.rmtree(T, ignore_errors=True)
print(f'{failed} failed' if failed else 'all hold')
sys.exit(1 if failed else 0)
