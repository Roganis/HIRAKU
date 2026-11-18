#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""hiraku-server's limits and choices (docs/SERVER.md): --connections (more cabinets than it serves at
once wait their turn and are all answered, none closed), --connections-per-address (those beyond it
closed, and said), --sync full|normal (the PRAGMA in force, said at start; a wrong value refused,
on the command line and in a config file); the accounts' refusals as kinds (every one mapped by the
API, no sentence matched); GET /api/v1/cards/{card} for a linked, an unlinked, a locked and an unknown
card, a non-admin refused; a refused link's tries_left counting down to the lock; no PIN in any
answer. Made-up cards, PINs and names.

    tests/eamuse/test_server_limits.py HIRAKU-SERVER SOURCE-DIR      exit 0 when all hold
"""
import http.client, json, os, re, shutil, socket, subprocess, sys, tempfile, threading, time

SERVER, SRC = sys.argv[1], sys.argv[2]
T = tempfile.mkdtemp(prefix='hk-limits-')
failed = 0
CARD_L, REF_L, PIN_L = '2010010000001A01', 'R000000000001A01', '3826'   # linked to ann
CARD_U, REF_U, PIN_U = '2010010000001B02', 'R000000000001B02', '6194'   # no one's
CARD_K, REF_K, PIN_K = '2010010000001C03', 'R000000000001C03', '5072'   # ann's tries: the lock
CARD_X = '2010010000001F99'                                             # never played
PINS = [PIN_L, PIN_U, PIN_K]
ANN_PW, ROOT_PW = 'ann-pass-phrase-1', 'root-pass-phrase-1'


def check(ok, what):
    global failed
    print(('ok    ' if ok else 'FAIL  ') + what, flush=True)
    failed += not ok


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class Serve:
    """serve on a database of its own, waited for until its first line (not only its port)"""
    def __init__(self, db, *options, env=None, api=False):
        self.port, self.api = free_port(), free_port() if api else None
        self.log = open(os.path.join(T, f'serve-{self.port}.txt'), 'w+')
        args = [SERVER, 'serve', '--db', db, '--port', str(self.port), '--bind', '127.0.0.1', '--no-relay', '--no-keys', '--quiet']
        args += ['--api', f'127.0.0.1:{self.api}', '--api-insecure-cookies'] if api else ['--no-api']
        self.p = subprocess.Popen(args + list(options), stdout=self.log, stderr=subprocess.STDOUT, env=env)
        for _ in range(300):
            if self.p.poll() is not None or 'e-AMUSE3 on' in self.said(): break
            time.sleep(0.05)

    def said(self):
        self.log.flush()
        with open(self.log.name, errors='replace') as f:
            return f.read()

    def stop(self):
        if self.p.poll() is None:
            self.p.terminate()
            try:
                self.p.wait(15)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait()
        self.log.close()


CALL = (b'<?xml version="1.0"?><call model="C00:J:A:A" srcid="0"><ntp method="get"/></call>')
REQUEST = b'POST / HTTP/1.0\r\nContent-Length: %d\r\n\r\n' % len(CALL) + CALL


def answer_on(s):
    """the call sent on an open connection, its whole answer read (None: closed or reset without one)"""
    try:
        s.sendall(REQUEST)
        out = b''
        while (d := s.recv(65536)):
            out += d
        return out
    except OSError:
        return None
    finally:
        s.close()


def call(port, results, i):
    try:
        s = socket.create_connection(('127.0.0.1', port), timeout=20)
    except OSError:
        results[i] = None
        return
    s.settimeout(20)
    results[i] = answer_on(s)


def answered(a):
    return a is not None and a.startswith(b'HTTP/1.') and b'<ntp status="0"' in a


def new_db(name):
    folder = os.path.join(T, name)
    os.makedirs(os.path.join(folder, 'empty'))
    db = os.path.join(folder, 'server.db')
    r = subprocess.run([SERVER, 'import', os.path.join(folder, 'empty'), '--db', db], capture_output=True)
    return db if r.returncode == 0 else None


try:
    # --- 1. more cabinets than served at once: they wait their turn, every one answered
    db = new_db('conn')
    s = Serve(db, '--connections', '16')
    check(s.p.poll() is None and '16 connections at once, any more wait their turn' in s.said(), '--connections 16: said at start')
    held = [socket.create_connection(('127.0.0.1', s.port), timeout=20) for _ in range(16)]   # every one of the 16 taken, silent
    time.sleep(0.3)
    more = [None] * 40
    threads = [threading.Thread(target=call, args=(s.port, more, i)) for i in range(40)]
    t0 = time.monotonic()
    for t in threads: t.start()
    time.sleep(1.0)
    check(all(t.is_alive() for t in threads), '... 40 more connections wait while the 16 are held (none closed)')
    held_answers = [None] * 16
    held_threads = [threading.Thread(target=lambda i=i: held_answers.__setitem__(i, answer_on(held[i]))) for i in range(16)]
    for t in held_threads: t.start()
    for t in threads + held_threads: t.join(30)
    took = time.monotonic() - t0
    check(all(answered(a) for a in more) and all(answered(a) for a in held_answers),
          f'... all 56 answered, none refused ({sum(answered(a) for a in more + held_answers)} of 56)')
    check(took < 10, f'... within 10 s ({took:.1f} s)')
    check(s.p.poll() is None and 'refused' not in s.said(), '... and nothing said refused')
    s.stop()

    # ... the default, said
    s = Serve(db)
    check('256 connections at once' in s.said() and 'at most 64 at once from one address' in s.said(), 'the defaults: 256 at once, 64 from one address')
    s.stop()

    # --- ... one address's share: beyond it closed, and said once (this computer's counted here, as a test)
    env = dict(os.environ, HK_TEST_PER_ADDRESS_ALL='1')
    s = Serve(db, '--connections', '32', '--connections-per-address', '4', env=env)
    four = [socket.create_connection(('127.0.0.1', s.port), timeout=5) for _ in range(4)]
    time.sleep(0.3)
    extra = [None] * 3
    for i in range(3): call(s.port, extra, i)
    check(not any(answered(a) for a in extra) and all(a in (None, b'') for a in extra), '--connections-per-address 4: the fifth to seventh closed at once')
    time.sleep(0.2)
    check(s.said().count('at once from one address already (--connections-per-address)') == 1, '... said once')
    answers = [answer_on(c) for c in four]
    check(all(answered(a) for a in answers), '... the four held are answered')
    time.sleep(0.3)
    again = [None]
    call(s.port, again, 0)
    check(answered(again[0]), '... and once they end, another is')
    s.stop()
    # ... this computer's own addresses have no share by default
    s = Serve(db, '--connections', '32', '--connections-per-address', '2')
    held = [socket.create_connection(('127.0.0.1', s.port), timeout=5) for _ in range(5)]
    answers = [answer_on(c) for c in held]
    check(all(answered(a) for a in answers) and 'from one address already' not in s.said(), "... this computer's own: no limit (5 of 5 with a limit of 2)")
    s.stop()

    # wrong values refused at start
    for bad in (['--connections', '8'], ['--connections', '5000'], ['--connections-per-address', '-1'], ['--sync', 'fast']):
        r = subprocess.run([SERVER, 'serve', '--db', db, '--no-api', *bad], capture_output=True, timeout=30)
        check(r.returncode == 2 and bad[0] in r.stderr.decode(), f'{" ".join(bad)}: refused at start (2)')
    r = subprocess.run([SERVER, 'players', '--db', db, '--sync', 'normal'], capture_output=True, timeout=30)
    check(r.returncode == 2 and "serve's" in r.stderr.decode(), "--sync with another command than serve: refused")

    # --- 2. --sync: the PRAGMA in force on its connections, said at start
    s = Serve(db)
    check('--sync full: each change on the disk before its answer goes (synchronous=FULL)' in s.said(), 'the default: synchronous=FULL in force, said')
    a = [None]
    call(s.port, a, 0)
    check(answered(a[0]), '... a call answered')
    s.stop()
    s = Serve(db, '--sync', 'normal')
    check('--sync normal' in s.said() and 'synchronous=NORMAL' in s.said(), '--sync normal: synchronous=NORMAL in force, said')
    a = [None]
    call(s.port, a, 0)
    check(answered(a[0]), '... a call answered')
    s.stop()
    conf = os.path.join(T, 'server.conf')
    with open(conf, 'w') as f:
        f.write('sync = normal\nconnections = 64\nconnections-per-address = 0\n')
    s = Serve(db, '--config', conf)
    check('synchronous=NORMAL' in s.said() and '64 connections at once' in s.said() and 'any number from one address' in s.said(),
          'the config file: sync, connections, connections-per-address')
    s.stop()
    with open(conf, 'w') as f:
        f.write('# made-up\nsync = sometimes\n')
    r = subprocess.run([SERVER, 'serve', '--db', db, '--no-api', '--config', conf], capture_output=True, timeout=30)
    check(r.returncode == 1 and 'line 2' in r.stderr.decode() and 'full or normal' in r.stderr.decode(), "the config file's wrong sync: refused with its line")

    # --- 3. the refusals as kinds: each one mapped by the API, none matched by its words
    with open(os.path.join(SRC, 'server', 'accounts.h')) as f:
        header = f.read()
    with open(os.path.join(SRC, 'server', 'api.cpp')) as f:
        api_src = f.read()
    with open(os.path.join(SRC, 'server', 'accounts.cpp')) as f:
        acc_src = f.read()
    kinds = re.findall(r'^\s+(\w+),', re.search(r'enum class Refused \{(.*?)\};', header, re.S).group(1), re.M)
    mapping = api_src[api_src.index('void refused(Response &res, const Refusal &why)'):]
    mapping = mapping[:mapping.index('\n}\n')]
    check(len(kinds) >= 10 and all(f'case Refused::{k}:' in mapping for k in kinds), f'every kind of refusal ({len(kinds)}) has its HTTP status in the API')
    check(all(f'case Refused::{k}: return "{k}"' in acc_src for k in kinds), '... and its name (refused_name)')
    check(not re.search(r'Refusal\(std::string', header) and not re.search(r'Refusal\(const (std::)?string', header),
          'a refusal is never made of a sentence alone (no constructor from a string)')
    check('KINDS' not in api_src and 'why.find(' not in api_src and 'why.rfind(' not in api_src, "the API matches no refusal's words")
    body = acc_src[acc_src.index('// --- users'):]
    check(not re.search(r'return "[^"]', body) and not re.search(r'return NOT_ALLOWED', body), 'no refusal returned as bare text in accounts.cpp')

    # --- 4. a card as the admin sees it; tries left
    db = new_db('cards')
    cards = ''.join(f'cardid={c}\trefid={r}\tpass={p}\n' for c, r, p in ((CARD_L, REF_L, PIN_L), (CARD_U, REF_U, PIN_U), (CARD_K, REF_K, PIN_K)))
    players = (f'card={CARD_L}\trid={REF_L}\tid=10000011\tname=ANNA\n'
               f'card={CARD_U}\trid={REF_U}\tid=10000012\tname=NOONE\n'
               f'card={CARD_K}\trid={REF_K}\tid=10000013\tname=KAY\n').encode()
    put = lambda name, data: subprocess.run([SERVER, 'put', name, '--db', db], input=data, capture_output=True).returncode == 0
    adm = subprocess.run([SERVER, 'user', 'add', 'root', '--role', 'admin', '--db', db], input=(ROOT_PW + '\n').encode(), capture_output=True)
    ann = subprocess.run([SERVER, 'user', 'add', 'ann', '--db', db], input=(ANN_PW + '\n').encode(), capture_output=True)
    check(put('cards', cards.encode()) and put('iidx-FDD-players', players) and adm.returncode == 0 and ann.returncode == 0,
          'made-up cards, players and two users')
    r = subprocess.run([SERVER, 'card', 'link', CARD_L, '--as', 'ann', '--db', db], input=(PIN_L + '\n').encode(), capture_output=True)
    check(r.returncode == 0, 'ann links a card (command line)')
    r = subprocess.run([SERVER, 'card', 'link', CARD_X, '--as', 'ann', '--db', db], input=b'1111\n', capture_output=True)
    check(r.returncode != 0 and 'has played on this server' in r.stderr.decode(), "... the command line says a refusal's sentence")

    s = Serve(db, api=True)
    seen = []

    def api(method, path, body=None, cookie=None, csrf=None):
        c = http.client.HTTPConnection('127.0.0.1', s.api, timeout=20)
        headers = {}
        data = json.dumps(body).encode() if body is not None else None
        if data is not None: headers['Content-Type'] = 'application/json'
        if cookie: headers['Cookie'] = 'hk_session=' + cookie
        if csrf: headers['X-HK-CSRF'] = csrf
        c.request(method, '/api/v1' + path, body=data, headers=headers)
        r = c.getresponse()
        text = r.read().decode()
        cookies = r.getheader('Set-Cookie') or ''
        c.close()
        seen.append(text)
        try:
            return r.status, json.loads(text), cookies
        except ValueError:
            return r.status, None, cookies

    def login(name, pw):
        st, j, ck = api('POST', '/login', {'name': name, 'password': pw})
        m = re.search(r'hk_session=([^;]*)', ck)
        return (m.group(1), j['csrf']) if st == 200 and m else (None, None)

    root, ann_s = login('root', ROOT_PW), login('ann', ANN_PW)
    check(root[0] is not None and ann_s[0] is not None, 'root (an admin) and ann log in')

    st, j, _ = api('GET', f'/cards/{CARD_L}', cookie=root[0])
    check(st == 200 and j['known'] is True and j['user'] == 'ann' and j['locked'] is False and j['wrong_pins'] == 0 and j['tries_left'] == 5 and
          j['linked'] and any(g['player'] == 'ANNA' and g['id'] and g['title'] for g in j['games']),
          'GET /cards/{card}, a linked card: known, its user, unlocked, its game (id, title, player)')
    st, j, _ = api('GET', f'/cards/{CARD_U.lower()}', cookie=root[0])
    check(st == 200 and j['known'] is True and j['user'] is None and j['card'] == CARD_U and any(g['player'] == 'NOONE' for g in j['games']),
          '... an unlinked card: no user, its game (any case of its number)')
    st, j, _ = api('GET', f'/cards/{CARD_X}', cookie=root[0])
    check(st == 200 and j['known'] is False and j['user'] is None and j['games'] == [] and j['locked'] is False, '... an unknown card: known false')
    check(api('GET', f'/cards/{CARD_L}', cookie=ann_s[0])[0] == 403, '... a player refused (403)')
    check(api('GET', f'/cards/{CARD_L}')[0] == 401, '... nobody signed in refused (401)')

    lefts = []
    for _ in range(5):
        st, j, _ = api('POST', '/me/cards', {'card': CARD_K, 'pin': '0000'}, *ann_s)
        lefts.append((st, j.get('error'), j.get('tries_left'), j.get('locked'), j.get('message', '')))
    check([x[2] for x in lefts] == [4, 3, 2, 1, 0], f'wrong PINs: tries_left counts down to the lock ({[x[2] for x in lefts]})')
    check(all(x[0] == 403 and x[1] == 'wrong_pin' and f'{x[2]} tr' in x[4] for x in lefts[:4]), '... wrong_pin (403), the tries left in the message too')
    check(lefts[4][1] == 'card_locked' and lefts[4][3] is True and 'locked' in lefts[4][4], '... the fifth locks it, and says so')
    st, j, _ = api('POST', '/me/cards', {'card': CARD_K, 'pin': PIN_K}, *ann_s)
    check(st == 403 and j['error'] == 'card_locked' and j['locked'] is True and j['tries_left'] == 0, '... the right PIN then: card_locked, locked, 0 left')
    st, j, _ = api('GET', f'/cards/{CARD_K}', cookie=root[0])
    check(st == 200 and j['locked'] is True and j['wrong_pins'] == 5 and j['tries_left'] == 0 and j['user'] is None, '... the admin sees it locked, 5 wrong PINs')
    check(api('POST', f'/cards/{CARD_K}/unlock', cookie=root[0], csrf=root[1])[0] == 200, '... unlocks it')
    st, j, _ = api('GET', f'/cards/{CARD_K}', cookie=root[0])
    check(st == 200 and j['locked'] is False and j['wrong_pins'] == 0 and j['tries_left'] == 5, '... then unlocked, 5 tries again')
    st, j, _ = api('POST', '/me/cards', {'card': CARD_K, 'pin': '0001'}, *ann_s)
    check(st == 403 and j['tries_left'] == 4, '... and a wrong PIN counts from the start')
    st, j, _ = api('GET', f'/cards/{CARD_K}', cookie=root[0])
    check(st == 200 and j['wrong_pins'] == 1 and j['tries_left'] == 4, '... seen by the admin (1 wrong, 4 left)')
    check(api('POST', '/me/cards', {'card': CARD_K, 'pin': PIN_K}, *ann_s)[0] == 201, '... the right one links it')
    st, j, _ = api('GET', f'/cards/{CARD_K}', cookie=root[0])
    check(st == 200 and j['user'] == 'ann' and j['wrong_pins'] == 0, '... linked: its tries forgotten')
    # the statuses of a few refusals by their kind
    check(api('POST', f'/cards/{CARD_U}/unlock', cookie=root[0], csrf=root[1])[0] == 403, "unlock a card that isn't locked: 403 as before")
    check(api('DELETE', f'/cards/{CARD_U}', cookie=root[0], csrf=root[1])[0] == 404, "unlink one that isn't linked: 404")
    check(api('POST', '/me/cards', {'card': CARD_L, 'pin': PIN_L}, *ann_s)[0] == 409, 'link one linked already: 409')
    check(api('POST', '/me/cards', {'card': CARD_X, 'pin': '1111'}, *ann_s)[0] == 404, 'link a card never played: 404')

    leaks = [t for t in seen for p in PINS if re.search(r'(?<![0-9A-Za-z])' + p + r'(?![0-9A-Za-z])', t)]
    check(not leaks, f'no PIN in any of {len(seen)} answers')
    # (as whole numbers: a port or a time in the output may hold a PIN's four digits by chance)
    said = s.said()
    check(not any(re.search(r'(?<![0-9A-Za-z])' + p + r'(?![0-9A-Za-z])', said) for p in PINS), "... nor in serve's output")
    s.stop()
finally:
    shutil.rmtree(T, ignore_errors=True)
print(f'{failed} failed' if failed else 'test_server_limits: all hold')
sys.exit(1 if failed else 0)
