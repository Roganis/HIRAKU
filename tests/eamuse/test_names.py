#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""hiraku-server's song names (server/names.h, docs/SERVER.md "Song names"): made-up packs in the folder
beside the database (the default) and in --names DIR: read at the start, read again after a change,
a file gone dropped; each refusal (over 4 MB, not JSON, not a pack, a game the table lacks, over 20000
songs, a file named after another game) said in one line, the others still loaded; texts cut at 200
characters and their control characters dropped; GET /games/ID/songs (a logged-in user's; none: empty
objects), has_names in /games and /server, an admin's GET /names; the games' port answers all along.
Every title here is made up.

    tests/eamuse/test_names.py HIRAKU-SERVER      exit 0 when all hold
"""
import http.client, json, os, re, shutil, socket, subprocess, sys, tempfile, time

SERVER = sys.argv[1]
T = tempfile.mkdtemp(prefix='hk-names-')
DB = os.path.join(T, 'server.db')
NAMES = os.path.join(T, 'names')   # (the default: beside the database)
CARD, PIN = '2004010000000017', '4321'
failed = 0


def check(ok, what):
    global failed
    print(('ok    ' if ok else 'FAIL  ') + what, flush=True)
    failed += not ok


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def write(name, data):
    path = os.path.join(NAMES, name)
    with open(path, 'wb') as f:
        f.write(data if isinstance(data, bytes) else json.dumps(data).encode())
    return path


def pack(game, songs, charts=None):
    p = {'format': 1, 'game': game, 'songs': songs}
    if charts is not None:
        p['charts'] = charts
    return p


class Serve:
    def __init__(self, *extra):
        self.port, self.api = free_port(), free_port()
        self.log_path = os.path.join(T, f'serve-{self.api}.txt')
        self.log = open(self.log_path, 'w+')
        self.p = subprocess.Popen([SERVER, 'serve', '--db', DB, '--port', str(self.port), '--bind', '127.0.0.1', '--no-relay',
                                   '--api', f'127.0.0.1:{self.api}', '--api-insecure-cookies', *extra],
                                  stdout=self.log, stderr=subprocess.STDOUT)

    def text(self):
        with open(self.log_path, encoding='utf-8', errors='replace') as f:
            return f.read()

    def wait_for(self, pattern, seconds=30):
        """the log's text once pattern is in it (None: it never came, or serve ended)"""
        for _ in range(seconds * 10):
            t = self.text()
            if re.search(pattern, t):
                return t
            if self.p.poll() is not None:
                return None
            time.sleep(0.1)
        return None

    def stop(self):
        if self.p.poll() is None:
            self.p.terminate()
            try:
                self.p.wait(10)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait()
        self.log.close()

    def call(self, model, body):
        """a game's call on the games' port, in clear: the answer's bytes"""
        data = f'<?xml version="1.0"?><call model="{model}" srcid="0102030405060708090A">{body}</call>'.encode()
        req = b'POST / HTTP/1.0\r\nHost: cabinet.test:8080\r\nContent-Length: %d\r\n\r\n' % len(data) + data
        with socket.create_connection(('127.0.0.1', self.port), timeout=20) as s:
            s.sendall(req)
            out = b''
            while True:
                d = s.recv(65536)
                if not d:
                    return out
                out += d

    def api_call(self, method, path, body=None, who=None):
        c = http.client.HTTPConnection('127.0.0.1', self.api, timeout=20)
        headers = {}
        if body is not None:
            headers['Content-Type'] = 'application/json'
        if who:
            headers['Cookie'] = 'hk_session=' + who[0]
            headers['X-HK-CSRF'] = who[1]
        c.request(method, '/api/v1' + path, body=json.dumps(body).encode() if body is not None else None, headers=headers)
        r = c.getresponse()
        text = r.read()
        hdrs = {k.lower(): v for k, v in r.getheaders()}
        c.close()
        try:
            return r.status, json.loads(text), hdrs
        except ValueError:
            return r.status, None, hdrs

    def login(self, name, password):
        st, j, h = self.api_call('POST', '/login', {'name': name, 'password': password})
        m = re.search(r'hk_session=([^;]*)', h.get('set-cookie', ''))
        return (m.group(1), j['csrf']) if st == 200 and m else None


def game_ok(s):
    return b' 200 ' in s.call('E12:J:A:A:2004111700', '<E12 type="game" method="get"/>')[:20]


serve = None
try:
    os.makedirs(os.path.join(T, 'empty'))
    os.makedirs(NAMES)
    r = subprocess.run([SERVER, 'import', os.path.join(T, 'empty'), '--db', DB], capture_output=True, text=True)
    m = re.search(r'password (\S+)', r.stdout)
    admin_pw = m.group(1) if m else ''
    check(admin_pw != '', 'a new database and its first admin')

    # --- made-up packs at the start: one good, one a game without scores read yet
    long_title = 'Made-up Song 7 ' + 'x' * 300
    write('popn12.json', pack('popn12', {'5': {'title': 'Made-up Song 5', 'genre': 'Made-up Genre'},
                                         '7': {'title': long_title + '\u0001\u0007', 'artist': 'Made-up\u0000 Artist\n'}},
                              {'0': 'Easy-ish', '3': 'Hardest'}))
    serve = Serve()
    t = serve.wait_for(r'names: popn12\.json: 2 songs, 2 charts')
    check(t is not None, 'serve says the pack it read at the start, in one line')
    check(serve.wait_for(r'the API on') is not None, 'serve listens')

    # the games' side: a player and two scores in pop'n 12
    serve.call('E12:J:A:A:2004111700', f'<E12 type="playerdata" method="new" card_id="{CARD}" name="ALICE" password="{PIN}"/>')
    serve.call('E12:J:A:A:2004111700', f'<E12 type="playerdata" method="set" card_id="{CARD}"><music num="5" sheet="0" data="1" score="80000"/>'
                                       '<music num="20" sheet="3" data="192" score="65000"/></E12>')
    check(game_ok(serve), 'the games\' port answers')

    # --- who may ask
    st, _, _ = serve.api_call('GET', '/games/popn12/songs')
    check(st == 401, 'GET /games/popn12/songs without a log-in: 401')
    st, j, _ = serve.api_call('GET', '/server')
    has = {g['id']: g.get('has_names') for g in j['games']} if j else {}
    check(st == 200 and has.get('popn12') is True and has.get('popn13') is False, '/server: has_names per game (popn12 yes, popn13 no)')
    admin = serve.login('admin', admin_pw)
    if admin:   # (the first admin changes the password made for it first)
        serve.api_call('POST', '/me/password', {'old_password': admin_pw, 'password': 'admin-password-1'}, who=admin)
        admin = serve.login('admin', 'admin-password-1')
    check(admin is not None, 'the admin logs in')
    st, _, _ = serve.api_call('POST', '/signup', {'name': 'bob', 'password': 'bob-password-1'})
    bob = serve.login('bob', 'bob-password-1')
    check(st == 201 and bob is not None, 'a player signs up and logs in')

    st, j, _ = serve.api_call('GET', '/games', who=bob)
    has = {g['id']: g.get('has_names') for g in j['games']} if j else {}
    check(st == 200 and has.get('popn12') is True and has.get('iidx16') is False and all(isinstance(v, bool) for v in has.values()),
          '/games: has_names per game')
    st, j, _ = serve.api_call('GET', '/games/popn12/songs', who=bob)
    songs = j.get('songs', {}) if j else {}
    check(st == 200 and songs.get('5') == {'title': 'Made-up Song 5', 'genre': 'Made-up Genre'} and j.get('charts') == {'0': 'Easy-ish', '3': 'Hardest'},
          'GET /games/popn12/songs: the pack\'s songs and charts')
    t7 = songs.get('7', {}).get('title', '')
    check(len(t7) == 200 and t7 == long_title[:200] and songs.get('7', {}).get('artist') == 'Made-up Artist',
          'a text cut at 200 characters, its control characters dropped')
    st, j, _ = serve.api_call('GET', '/games/popn13/songs', who=bob)
    check(st == 200 and j.get('songs') == {} and j.get('charts') == {}, 'a game without a pack: empty objects')
    check(serve.api_call('GET', '/games/nogame/songs', who=bob)[0] == 404, 'a game the table lacks: 404')
    check(serve.api_call('GET', '/games/popn-pc-later/songs', who=bob)[0] == 404, 'an unlisted game: 404')
    check(serve.api_call('GET', '/names', who=bob)[0] == 403, 'GET /names: a player is refused')
    st, j, _ = serve.api_call('GET', '/names', who=admin)
    packs = {p['game']: p for p in j['packs']} if j else {}
    check(st == 200 and packs.get('popn12', {}).get('songs') == 2 and packs['popn12'].get('charts') == 2 and packs['popn12'].get('time', 0) > 0 and
          j.get('refused') == [], 'GET /names (an admin): the pack, its songs, charts and time; none refused')
    check(j and j.get('set') is True and 'folder' not in j and not any(os.sep + 'names' in str(v) for v in j.values()),
          "... a names folder is set (\"set\"), and no path on the server's disk")

    # --- a change read again; the refusals, each in one line, the good packs kept
    write('popn12.json', pack('popn12', {'5': {'title': 'Made-up Song 5 again'}, '20': {'title': 'Made-up Song 20'}}))
    write('popn13.json', b'{"format": 1, "game": "popn13", "songs": {')
    with open(os.path.join(NAMES, 'popn14.json'), 'wb') as f:
        f.write(b'{"format": 1, "game": "popn14", "songs": {}, "pad": "' + b'x' * (4 << 20) + b'"}')
    write('nogame.json', pack('nogame', {'1': {'title': 'Made-up Song 1'}}))
    write('popn15.json', pack('popn15', {str(n): {'title': f'Made-up Song {n}'} for n in range(20001)}))
    write('popn16.json', pack('popn17', {'1': {'title': 'Made-up Song 1'}}))
    write('popn17.json', {'format': 2, 'game': 'popn17', 'songs': {}})
    write('popn18.json', pack('popn18', {'x': {'title': 'Made-up Song x'}}))
    write('iidx16.json', pack('iidx16', {'1601': {'title': 'Made-up Song 1601', 'artist': 'Made-up Artist'}}))
    time.sleep(4)   # (one more look: every line said)
    serve.api_call('GET', '/games', who=bob)
    t = serve.wait_for(r'iidx16\.json: 1 songs', 30) or ''
    lines = [ln for ln in t.splitlines() if ' names: ' in ln]
    def refusal(name, words):
        return any(f'names: {name}: refused: ' in ln and words in ln for ln in lines)
    check(refusal('popn13.json', 'not JSON'), 'not JSON: refused in one line')
    check(refusal('popn14.json', 'over 4 MB'), 'over 4 MB: refused')
    check(refusal('nogame.json', "a game the server doesn't know"), 'a game the table lacks: refused')
    check(refusal('popn15.json', 'over 20000 songs'), 'over 20000 songs: refused')
    check(refusal('popn16.json', 'popn17.json'), 'a pack in another game\'s file: refused')
    check(refusal('popn17.json', 'not a names pack'), 'another format: refused')
    check(refusal('popn18.json', "isn't a song number"), 'a song that isn\'t a number: refused')
    st, j, _ = serve.api_call('GET', '/games/popn12/songs', who=bob)
    check(st == 200 and j['songs'] == {'5': {'title': 'Made-up Song 5 again'}, '20': {'title': 'Made-up Song 20'}} and j['charts'] == {},
          'the changed pack read again')
    st, j, _ = serve.api_call('GET', '/names', who=admin)
    refused = {r['file']: r['why'] for r in j['refused']} if j else {}
    check(st == 200 and sorted(p['game'] for p in j['packs']) == ['iidx16', 'popn12'] and len(refused) == 7 and all(refused.values()),
          'GET /names: two packs loaded, seven refused with why')
    st, j, _ = serve.api_call('GET', '/games', who=bob)
    has = {g['id']: g.get('has_names') for g in j['games']}
    check(has.get('iidx16') is True and has.get('popn13') is False and has.get('popn15') is False, 'has_names follows: a refused pack is none')
    check(game_ok(serve), 'the games\' port answers after the refusals')

    # --- a file gone: dropped
    os.remove(os.path.join(NAMES, 'iidx16.json'))
    time.sleep(4)
    st, j, _ = serve.api_call('GET', '/games/iidx16/songs', who=bob)
    check(st == 200 and j['songs'] == {} and serve.wait_for(r'iidx16\.json: gone', 10) is not None, 'a pack removed: its names gone, said')
    serve.stop()

    # --- --names DIR, and a folder that isn't there
    other = os.path.join(T, 'elsewhere')
    os.makedirs(other)
    with open(os.path.join(other, 'popn14.json'), 'w', encoding='utf-8') as f:
        json.dump(pack('popn14', {'3': {'title': 'Made-up Song 3'}}), f)
    serve = Serve('--names', other)
    check(serve.wait_for(r'names: popn14\.json: 1 songs') is not None and 'popn12.json' not in serve.text(), '--names DIR: its packs, not the default folder\'s')
    serve.stop()
    with open(os.path.join(T, 'serve.conf'), 'w') as f:
        f.write(f'names = {other}\n')
    serve = Serve('--config', os.path.join(T, 'serve.conf'))
    check(serve.wait_for(r'names: popn14\.json: 1 songs') is not None, 'the config file\'s names = DIR')
    serve.stop()
    serve = Serve('--names', os.path.join(T, 'nowhere'))
    t = serve.wait_for(r'the API on')
    check(t is not None and 'no song names' in t and game_ok(serve), 'no such folder: no names, said once, the server serves')
    serve.stop()
    serve = None
finally:
    if serve:
        serve.stop()
    if failed:
        print('kept for a look: ' + T)
    else:
        shutil.rmtree(T, ignore_errors=True)
sys.exit(1 if failed else 0)
