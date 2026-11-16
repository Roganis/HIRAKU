#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""hiraku-server as a service (docs/SERVER.md, "Running it as a service"): GET /api/v1/health and the
health command (server up, server down); serve's default log (a line a game call and a line an API
request, no card, PIN, password, token or key in it); --log-calls (the bodies, PINs as ***); the
config file (values taken, the command line wins, an unknown name refused with its line); backup
--dir --keep (N kept, other files untouched, each backup opens and exports); SIGTERM during calls
(exit 0, no WAL left, every call answered before the stop kept). Made-up cards, PINs and names.

    tests/eamuse/test_service.py HIRAKU-SERVER KEY_CLIENT      exit 0 when all hold
"""
import http.client, json, os, re, shutil, signal, socket, subprocess, sys, tempfile, threading, time
SERVER, CLIENT = sys.argv[1], sys.argv[2]
T = tempfile.mkdtemp(prefix='hk-service-')
DB = os.path.join(T, 'db', 'server.db')
failed = 0
CARD, PIN = 'E004010000C0FFEE', '7316'
CARD2, PIN2 = 'E004010000BEEF02', '5820'
PASSWORD = 'a made-up pass phrase'
MODEL = 'E13:J:A:A:2005112400'
ISO = r'\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ'


def check(ok, what):
    global failed
    print(('ok    ' if ok else 'FAIL  ') + what, flush=True)
    failed += not ok


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def cmd(*args, stdin=b'', db=DB):
    r = subprocess.run([SERVER, *args, *(['--db', db] if db else [])], input=stdin, capture_output=True, timeout=60)
    return r.returncode, r.stdout.decode(errors='replace'), r.stderr.decode(errors='replace')


class Serve:
    def __init__(self, *options, port=None, api=None, wait=True):
        self.port, self.api = port, api
        self.log = open(os.path.join(T, f'serve-{time.monotonic_ns()}.txt'), 'w+')
        self.p = subprocess.Popen([SERVER, 'serve', *options], stdout=self.log, stderr=subprocess.STDOUT)
        for _ in range(300 if wait else 0):
            if self.p.poll() is not None: break
            try:
                socket.create_connection(('127.0.0.1', self.port)).close()
                socket.create_connection(('127.0.0.1', self.api)).close()
                break
            except OSError:
                time.sleep(0.1)
        # (its ports take connections a moment before its first lines are in the file: the port is
        # opened, then the line that says so is printed and flushed)
        for _ in range(100 if wait else 0):
            if self.p.poll() is not None or 'e-AMUSE3 on' in self.said(): break
            time.sleep(0.05)

    def said(self):
        self.log.flush()
        with open(self.log.name) as f:
            return f.read()

    def stop(self, sig=signal.SIGTERM):
        if self.p.poll() is None:
            self.p.send_signal(sig)
            try:
                self.p.wait(15)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait()
        self.log.close()
        return self.p.returncode


def game_call(port, key, pcbid, body):
    xml = f'<?xml version="1.0"?><call model="{MODEL}" srcid="{pcbid}">{body}</call>'.encode()
    req = b'POST / HTTP/1.0\r\nContent-Length: %d\r\n\r\n' % len(xml) + xml
    return subprocess.run([CLIENT, 'call', '127.0.0.1', str(port), key], input=req, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=20).stdout


def api(port, method, path, body=None, cookie=None, csrf=None, token=None):
    c = http.client.HTTPConnection('127.0.0.1', port, timeout=20)
    headers = {}
    data = json.dumps(body).encode() if body is not None else None
    if data is not None: headers['Content-Type'] = 'application/json'
    if token: headers['Authorization'] = 'Bearer ' + token
    if cookie: headers['Cookie'] = 'hk_session=' + cookie
    if csrf: headers['X-HK-CSRF'] = csrf
    c.request(method, path, body=data, headers=headers)
    r = c.getresponse()
    text = r.read()
    hdrs = {k.lower(): v for k, v in r.getheaders()}
    c.close()
    try:
        return r.status, json.loads(text), hdrs
    except ValueError:
        return r.status, None, hdrs


def holds(text, s):
    if not s.isdigit():
        return s in text
    # a PIN: as a number of its own, not four digits inside a longer one, an id or a key
    text = re.sub(r'[A-Za-z0-9_.-]*[A-Za-z][A-Za-z0-9_.-]*', ' ', text)
    return re.search(r'(?<!\d)' + re.escape(s) + r'(?!\d)', text) is not None


s = None
try:
    os.makedirs(os.path.join(T, 'db'))
    os.makedirs(os.path.join(T, 'empty'))
    _, out, _ = cmd('import', os.path.join(T, 'empty'))
    m = re.search(r'password (\S+)', out)
    ADMIN_PW = m.group(1) if m else ''
    st, _, _ = cmd('user', 'add', 'opal', '--role', 'admin', stdin=(PASSWORD + '\n').encode())
    arcade = cmd('arcade', 'add', 'SHOP ONE', '13')[1].strip().splitlines()[-1]
    KEY = cmd('machine', 'add', arcade, 'CAB ONE')[1].strip()
    ID = KEY.split('.')[1] if KEY.count('.') == 2 else ''
    check(ADMIN_PW and st == 0 and len(ID) == 20, 'a database with an admin, an arcade and a machine with its key')

    # --- the config file: values taken, the command line wins, an unknown name refused
    P_CONF, PORT, API = free_port(), free_port(), free_port()
    conf = os.path.join(T, 'server.conf')
    with open(conf, 'w') as f:
        f.write(f'# made-up settings\n\ndb = {DB}\nport = {P_CONF}   # the command line says otherwise\nbind = 127.0.0.1\n'
                f'api = 127.0.0.1:{API}\nno-relay = true\napi-insecure-cookies = yes\nquiet = false\nno-web = true\n')
    s = Serve('--config', conf, '--port', str(PORT), port=PORT, api=API)
    started = s.said()
    check(s.p.poll() is None and f'127.0.0.1:{PORT}' in started and f'127.0.0.1:{API}' in started and str(DB) in started,
          '--config: db, bind, api taken from the file, --port from the command line')
    try:
        socket.create_connection(('127.0.0.1', P_CONF), timeout=1).close()
        conf_port_free = False
    except OSError:
        conf_port_free = True
    check(conf_port_free, "... the file's port not listened on (the command line won)")
    check('no web pages (--no-web)' in started and 'relay' not in started, '... and its switches (no-web, no-relay)')
    st, out, _ = cmd('--version', db=None)
    VERSION = out.strip()[len('hiraku-server '):] or '?'
    check(st == 0 and out.startswith('hiraku-server ') and len(out.strip().splitlines()) == 1, '--version: one line, the build')
    check('build ' + VERSION in started, "... the same build in serve's first line")

    # --- health
    st, j, _ = api(API, 'GET', '/api/v1/health')
    check(st == 200 and j == {'ok': True, 'version': VERSION, 'uptime': j.get('uptime')} and isinstance(j.get('uptime'), int),
          'GET /api/v1/health: 200 {ok, version, uptime}, nothing else, no log-in')
    r = subprocess.run([SERVER, 'health', '--api', f'127.0.0.1:{API}'], capture_output=True, timeout=30)
    check(r.returncode == 0 and len(r.stdout.decode().strip().splitlines()) == 1 and 'healthy' in r.stdout.decode(),
          'health: exit 0 and one line while serve runs')

    # --- traffic with PINs, passwords, tokens: the default log
    calls = [f'<cardmng method="inquire" cardtype="1" cardid="{CARD}" update="0"/>',
             f'<cardmng method="getrefid" cardtype="1" cardid="{CARD}" newflag="1" passwd="{PIN}"/>',
             f'<cardmng method="inquire" cardtype="1" cardid="{CARD2}" update="0"/>',
             f'<cardmng method="getrefid" cardtype="1" cardid="{CARD2}" newflag="1" passwd="{PIN2}"/>',
             f'<E13 type="game" method="get"/><cardmng method="inquire" cardtype="1" cardid="{CARD}" update="0"/>']
    answers = [game_call(PORT, KEY, ID, c) for c in calls]
    refid = re.search(rb'refid="([0-9A-F]+)"', answers[1])
    answers.append(game_call(PORT, KEY, ID, f'<cardmng method="authpass" refid="{refid.group(1).decode() if refid else ""}" pass="{PIN}"/>'))
    check(all(a.startswith(b'HTTP/1.0 200') for a in answers), f'{len(answers)} game calls behind the machine\'s key answered')
    st, j, h = api(API, 'POST', '/api/v1/login', {'name': 'opal', 'password': PASSWORD})
    cookie = re.search(r'hk_session=([^;]*)', h.get('set-cookie', ''))
    COOKIE, CSRF = (cookie.group(1) if cookie else ''), (j or {}).get('csrf', '')
    st1 = api(API, 'POST', '/api/v1/me/cards', {'card': CARD, 'pin': PIN}, cookie=COOKIE, csrf=CSRF)[0]
    st2, tj, _ = api(API, 'POST', '/api/v1/me/tokens', {'label': 'made up', 'scope': 'read'}, cookie=COOKIE, csrf=CSRF)
    TOKEN = (tj or {}).get('token', '')
    st3 = api(API, 'GET', '/api/v1/me', token=TOKEN)[0]
    st4 = api(API, 'GET', '/api/v1/games/popn13/top?limit=5&song=1', cookie=COOKIE)[0]
    st5 = api(API, 'DELETE', f'/api/v1/me/cards/{CARD}', cookie=COOKIE, csrf=CSRF)[0]
    st6 = api(API, 'POST', '/api/v1/login', {'name': 'opal', 'password': 'not ' + PASSWORD})[0]
    check(st == 200 and COOKIE and st1 in (200, 201) and st2 in (200, 201) and TOKEN and st3 == 200 and st4 == 200 and st5 == 200 and st6 == 401,
          'API calls: log-in, a card linked by its PIN, a token made and used, a query, the card unlinked, a wrong password')
    time.sleep(0.3)
    log = s.said()
    call_lines = [l for l in log.splitlines() if re.fullmatch(ISO + r' call \S+ ("[^"]*"|-) \S+ \S+ (\d{3}|-) \d+ms', l)]
    check(len(call_lines) == len(answers), f'one log line a game call ({len(call_lines)} for {len(answers)})')
    first = call_lines[0].split(' ') if call_lines else []
    check(first[2:7] == [ID, '"CAB', 'ONE"', MODEL, 'cardmng.inquire'] and first[7] == '200',
          "... when, the machine's PCBID and name, the model, module.method, the status, ms")
    check(any(' E13.get,cardmng.inquire 200 ' in l for l in call_lines), '... each request of a call, in order')
    api_lines = [l for l in log.splitlines() if re.fullmatch(ISO + r' api [A-Z]+ /\S* \d{3} (\d+ms|-) \S+', l)]
    check(len(api_lines) >= 8, f'a log line an API request ({len(api_lines)})')
    check(any(re.search(r' api GET /api/v1/me 200 \d+ms opal$', l) for l in api_lines) and
          any(re.search(r' api POST /api/v1/login 401 \d+ms -$', l) for l in api_lines),
          '... method, path, status, ms, the user when logged in')
    check(any(' api GET /api/v1/games/popn13/top 200 ' in l for l in api_lines) and '?' not in ''.join(api_lines),
          '... the path without its query string')
    check(any(' api DELETE /api/v1/me/cards/* 200 ' in l for l in api_lines), "... a card's number in a path as *")
    secrets = [CARD, CARD2, PIN, PIN2, PASSWORD, ADMIN_PW, COOKIE, CSRF, TOKEN, KEY, KEY.split('.')[-1], refid.group(1).decode() if refid else 'x']
    leaked = [x for x in secrets if x and holds(log, x)]
    check(not leaked and 'cardmng method' not in log and 'passwd' not in log, 'no card, PIN, password, cookie, CSRF value, token, key or body in it' +
          (f' ({len(leaked)} found)' if leaked else ''))

    # --- SIGTERM while calls come: exit 0, the calls answered before it kept, no WAL left
    kept, stop_now = [], threading.Event()

    def traffic(n):
        for i in range(400):
            if stop_now.is_set(): return
            card = f'E00401{n:02d}{i:08X}'
            a = game_call(PORT, KEY, ID, f'<cardmng method="inquire" cardtype="1" cardid="{card}" update="0"/>')
            if a.startswith(b'HTTP/1.0 200') and b'refid=' in a: kept.append(card)
            elif not a.startswith(b'HTTP/1.0'): return
    workers = [threading.Thread(target=traffic, args=(n,)) for n in range(3)]
    for w in workers: w.start()
    time.sleep(1.5)
    status = s.stop()
    stop_now.set()
    for w in workers: w.join(30)
    log = open(s.log.name).read()
    s = None
    check(status == 0 and re.search(r'hiraku-server: stopped \(SIGTERM\)', log), f'SIGTERM during calls: one line, exit 0 (status {status})')
    check(not os.path.exists(DB + '-wal'), "... the database closed: no WAL left beside it")
    r = subprocess.run([SERVER, 'health', '--api', f'127.0.0.1:{API}'], capture_output=True, timeout=30)
    check(r.returncode == 1 and len(r.stdout.decode().strip().splitlines()) == 1, 'health: exit 1 and one line with serve stopped')
    st, cards, _ = cmd('get', 'cards')
    missing = [c for c in kept if c not in cards]
    check(st == 0 and len(kept) > 5 and not missing, f'... it opens after, and each of the {len(kept)} cards answered before the stop is in it' +
          (f' ({len(missing)} missing)' if missing else ''))

    # --- --log-calls: the bodies, PINs and passwords as ***
    PORT, API = free_port(), free_port()
    s = Serve('--db', DB, '--port', str(PORT), '--bind', '127.0.0.1', '--api', f'127.0.0.1:{API}', '--no-relay', '--log-calls', port=PORT, api=API)
    a = game_call(PORT, KEY, ID, f'<cardmng method="getrefid" cardtype="1" cardid="{CARD2}" newflag="1" passwd="{PIN2}"/>'
                                 f'<cardmng method="authpass" refid="48524B0000000001" pass="{PIN2}"/>')
    time.sleep(0.3)
    log = s.said()
    check(a.startswith(b'HTTP/1.0 200') and 'every call\'s body is logged' in log, '--log-calls: said at the start')
    check('passwd="***"' in log and 'pass="***"' in log and CARD2 in log and not holds(log, PIN2), '... the bodies, the PINs as ***')
    check(s.stop(signal.SIGINT) == 0, '... SIGINT stops it as SIGTERM does (exit 0)')
    s = None

    # --- start-up refusals: one line, status 1 (2: a wrong command line)
    bad = os.path.join(T, 'bad.conf')
    with open(bad, 'w') as f:
        f.write(f'db = {DB}\n# a comment\nportt = 8080\n')
    r = subprocess.run([SERVER, 'serve', '--config', bad], capture_output=True, timeout=30)
    err = r.stderr.decode().strip()
    check(r.returncode == 1 and 'line 3' in err and 'portt' in err and len(err.splitlines()) == 1 and not r.stdout,
          'config: an unknown name stops the start, its line said in one line')
    with open(bad, 'w') as f:
        f.write(f'db = {DB}\ntz = 40\n')
    r = subprocess.run([SERVER, 'serve', '--config', bad], capture_output=True, timeout=30)
    check(r.returncode == 1 and 'line 2' in r.stderr.decode() and len(r.stderr.decode().strip().splitlines()) == 1, 'config: a value out of range, its line')
    r = subprocess.run([SERVER, 'serve', '--db', DB, '--port', '0'], capture_output=True, timeout=30)
    check(r.returncode == 2 and len(r.stderr.decode().strip().splitlines()) == 1, 'the command line: a port out of range, status 2')
    with socket.socket() as taken:
        taken.bind(('127.0.0.1', 0))
        taken.listen(1)
        r = subprocess.run([SERVER, 'serve', '--db', DB, '--bind', '127.0.0.1', '--port', str(taken.getsockname()[1]), '--no-relay'],
                           capture_output=True, timeout=30)
    out = (r.stdout + r.stderr).decode().strip()
    check(r.returncode == 1 and len(out.splitlines()) == 1 and "games' port" in out, 'the port taken: one line, status 1')
    r = subprocess.run([SERVER, 'serve', '--db', os.path.join(T, 'nowhere', 'x.db')], capture_output=True, timeout=30)
    out = (r.stdout + r.stderr).decode().strip()
    check(r.returncode == 1 and len(out.splitlines()) == 1 and 'nowhere' in out, "the database's folder not there: one line, status 1")
    r = subprocess.run([SERVER, 'serve', '--db', DB, '--web', os.path.join(T, 'empty')], capture_output=True, timeout=30)
    out = (r.stdout + r.stderr).decode().strip()
    check(r.returncode == 1 and len(out.splitlines()) == 1 and 'index.html' in out, 'the web folder without its pages: one line, status 1')

    # --- backup --dir --keep
    B = os.path.join(T, 'backups')
    os.makedirs(B)
    foreign = [os.path.join(B, n) for n in ('notes.txt', 'hiraku-server-old.db', 'hiraku-server-20000101-000000.db.bak')]
    for n in foreign:
        with open(n, 'w') as f: f.write('made up\n')
    made = []
    for i in range(3):
        if i: time.sleep(1.1)
        st, out, err = cmd('backup', '--dir', B, '--keep', '2')
        made.append(st)
    ours = sorted(n for n in os.listdir(B) if re.fullmatch(r'hiraku-server-\d{8}-\d{6}\.db', n))
    check(made == [0, 0, 0] and len(ours) == 2, f'backup --dir --keep 2, three times: the newest two kept ({len(ours)})')
    check(all(os.path.exists(n) for n in foreign), '... the files it did not name untouched')
    good = True
    for n in ours:
        ex = os.path.join(T, 'export-' + n)
        good = good and cmd('export', ex, db=os.path.join(B, n))[0] == 0 and CARD in open(os.path.join(ex, 'cards'), errors='replace').read()
    check(good, '... each opens and exports, with what the server kept')
    check(cmd('backup', '--keep', '2')[0] == 2, '--keep without --dir: a wrong command line')
finally:
    if s:
        s.p.kill()
        s.p.wait()
    shutil.rmtree(T, ignore_errors=True)
print('all hold' if not failed else f'{failed} failed')
sys.exit(1 if failed else 0)
