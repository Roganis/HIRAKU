#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""hiraku-server's JSON API (server/api.h, docs/SERVER.md), through serve's API port: sign-up, log-in,
the session's cookie and CSRF value, personal tokens (read scope can't write), a password that must
be changed, each role allowed and refused, card linking and its lock, hidden stats and card numbers
never shown to others, names in a game's own encoding as UTF-8 (or hex), a machine's key shown once
and taken by the game port, an arcade's settings and their effect on a game's answer, the unlisted
games' row, the limits on how often (429), malformed and oversized bodies, and no password, PIN,
token, code or key in any answer but the one that made it, nor in the server's output. Made-up
names, cards, PINs and players; key_client is a cabinet's side.

    tests/eamuse/test_api.py HIRAKU-SERVER KEY_CLIENT      exit 0 when all hold
"""
import http.client, json, os, re, shutil, socket, subprocess, sys, tempfile, time

SERVER, CLIENT = sys.argv[1], sys.argv[2]
T = tempfile.mkdtemp(prefix='hk-api-')
DB = os.path.join(T, 'server.db')
failed = 0
seen = []        # every answer's body: (body, the secrets it may hold: the ones it made)
secrets = []     # every password, PIN, token, code and key the test used or was given

CARD_A, REF_A, PIN_A = '2010010000000A01', 'R000000000000A01', '1357'   # bob's (DistorteD)
CARD_B, REF_B, PIN_B = '2010010000000B02', 'R000000000000B02', '2468'   # carol's
CARD_C, REF_C, PIN_C = '2010010000000C03', 'R000000000000C03', '9753'   # dave's: the lock
CARD_D, REF_D, PIN_D = '2010010000000D04', 'R000000000000D04', '8642'   # no one's: by the game's name
CARD_M, REF_M, PIN_M = '2010010000000E05', 'R000000000000E05', '7531'   # bob's too: the unlisted row
PINS = [PIN_A, PIN_B, PIN_C, PIN_D, PIN_M]
SJIS_NAME = 'アルファ'
PASSWORDS = {'bob': 'bob-password-1', 'carol': 'carol-password-1', 'dave': 'dave-password-1', 'erin': 'erin-password-1',
             'op1': 'operator-one-1', 'op2': 'operator-two-2'}


def check(ok, what):
    global failed
    print(('ok    ' if ok else 'FAIL  ') + what, flush=True)
    failed += not ok


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def cmd(*args, stdin=b''):
    r = subprocess.run([SERVER, *args, '--db', DB], input=stdin, capture_output=True)
    return r.returncode, r.stdout.decode(errors='replace'), r.stderr.decode(errors='replace')


class Who:
    """a caller: a session (cookie, CSRF value) or a token"""
    def __init__(self, cookie=None, csrf=None, token=None):
        self.cookie, self.csrf, self.token = cookie, csrf, token


hop = [0]


def addr():
    """a new caller's address (X-Forwarded-For: the server trusts it here, --api-trust-proxy)"""
    hop[0] += 1
    return f'10.0.{hop[0] // 250}.{hop[0] % 250 + 1}'


def api(method, path, body=None, who=None, csrf=True, raw=None, xff=None, makes=()):
    """(status, the answer as JSON or None, headers)"""
    c = http.client.HTTPConnection('127.0.0.1', API_PORT, timeout=20)
    headers = {'X-Forwarded-For': xff or '10.9.9.9'}
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    if data is not None:
        headers['Content-Type'] = 'application/json'
    if who and who.token:
        headers['Authorization'] = 'Bearer ' + who.token
    elif who and who.cookie:
        headers['Cookie'] = 'hk_session=' + who.cookie
        if csrf and who.csrf:
            headers['X-HK-CSRF'] = who.csrf
    c.request(method, '/api/v1' + path, body=data, headers=headers)
    r = c.getresponse()
    text = r.read()
    hdrs = {k.lower(): v for k, v in r.getheaders()}
    c.close()
    seen.append((text.decode('utf-8', errors='strict'), set(makes)))
    try:
        return r.status, json.loads(text), hdrs
    except ValueError:
        return r.status, None, hdrs


def login(name, password):
    st, j, h = api('POST', '/login', {'name': name, 'password': password}, xff=addr(), makes=['__cookie__'])
    cookie = re.search(r'hk_session=([^;]*)', h.get('set-cookie', ''))
    if st != 200 or not cookie:
        return None, st, j, h
    secrets.append(cookie.group(1))
    seen[-1] = (seen[-1][0], {cookie.group(1)})   # (the cookie isn't in the body; nothing else may be)
    return Who(cookie.group(1), j['csrf']), st, j, h


def put(name, data):
    st = subprocess.run([SERVER, 'put', name, '--db', DB], input=data, capture_output=True).returncode
    return st == 0


def game_call(port, key, pcbid, body):
    xml = f'<?xml version="1.0"?><call model="F14:J:A:A" srcid="{pcbid}">{body}</call>'.encode()
    req = b'POST / HTTP/1.0\r\nContent-Length: %d\r\n\r\n' % len(xml) + xml
    return subprocess.run([CLIENT, 'call', '127.0.0.1', str(port), key], input=req, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=20).stdout


serve = None
log = None
try:
    # --- the data: made-up cards and players
    os.makedirs(os.path.join(T, 'empty'))
    _, out, _ = cmd('import', os.path.join(T, 'empty'))
    m = re.search(r'password (\S+)', out)
    ADMIN_PW = m.group(1) if m else ''
    secrets += [ADMIN_PW] + list(PASSWORDS.values())
    cards = ''.join(f'cardid={c}\trefid={r}\tpass={p}\n' for c, r, p in
                    ((CARD_A, REF_A, PIN_A), (CARD_B, REF_B, PIN_B), (CARD_C, REF_C, PIN_C), (CARD_D, REF_D, PIN_D), (CARD_M, REF_M, PIN_M)))
    players = (f'card={CARD_A}\trid={REF_A}\tid=10000001\tname='.encode() + SJIS_NAME.encode('cp932') + b'\n' +
               f'card={CARD_B}\trid={REF_B}\tid=10000002\tname=CAROL\n'.encode() +
               f'card={CARD_D}\trid={REF_D}\tid=10000003\tname='.encode() + b'\x81\xff' + b'\n')
    scores = (b'id=10000001\tmid=1001\tcl=1\tex=1500\tplays=2\tclear=4\n'
              b'id=10000002\tmid=1001\tcl=1\tex=1700\tplays=3\tclear=4\n'
              b'id=10000003\tmid=1001\tcl=1\tex=1600\tplays=1\tclear=2\n'
              b'id=10000002\tmid=1002\tcl=0\tex=800\tplays=1\tclear=1\n'
              b'id=10000002\tmid=1003\tcl=0\tex=0\tplays=1\tclear=1\n')   # (a score of 0: no best score)
    # carol's System 573 DrumMania: a stage cleared (its score) and one failed (no score kept)
    gfdm = f'id={CARD_B}\tgdid=7\tname=CAROL\tpass={PIN_B}\n'.encode()
    # carol's pop'n 12 record: song 5's marks 1, 2 and 3 on charts 0-2 (no PIN kept in it: 0xff)
    popn12 = bytearray(0x708)
    popn12[0:5], popn12[0x34:0x38], popn12[0x3c + 2 * 5] = b'CAROL', b'\xff' * 4, 0b111001
    gfdm_rows = b'netid=12\tseqmode=1\tperc=91\tclear=1\tscore=80000\tplays=2\nnetid=13\tseqmode=2\tperc=60\tplays=1\n'
    check(ADMIN_PW != '' and put('cards', cards.encode()) and put('iidx-FDD-players', players) and put('iidx-FDD-scores', scores) and
          put(f'phc-{REF_M}', b'name=MPLAYER\taccount.total_play_cnt=4\n') and
          put(f'phcs-{REF_M}', b'music=5\tsheet=1\tscore=90000\tcnt=1\tclear_type=3\n') and
          put('gfdm-cards', gfdm) and put('gfdm-D40-7', gfdm_rows) and put(f'pdata-E12-{CARD_B}', bytes(popn12)), 'the made-up data put in a new database')

    PORT, API_PORT = free_port(), free_port()
    log = open(os.path.join(T, 'serve.txt'), 'w+')
    serve = subprocess.Popen([SERVER, 'serve', '--db', DB, '--port', str(PORT), '--bind', '127.0.0.1', '--no-relay',
                              '--api', f'127.0.0.1:{API_PORT}', '--api-insecure-cookies', '--api-trust-proxy'],
                             stdout=log, stderr=subprocess.STDOUT)
    for _ in range(300):
        try:
            socket.create_connection(('127.0.0.1', API_PORT)).close()
            socket.create_connection(('127.0.0.1', PORT)).close()
            break
        except OSError:
            if serve.poll() is not None:
                raise SystemExit('serve ended at start')
            time.sleep(0.1)

    # --- public
    st, j, h = api('GET', '/server')
    ids = [g['id'] for g in j['games']] if j else []
    check(st == 200 and h.get('content-type') == 'application/json; charset=utf-8' and j['signup'] == 'open' and 'iidx13' in ids,
          'GET /server: JSON, the sign-up mode, the games')
    check('popn-pc-later' not in ids and 'M39' not in json.dumps(j), '... the unlisted row is not listed')
    st, j, _ = api('POST', '/signup', {'name': 'bob', 'password': PASSWORDS['bob']}, xff=addr())
    check(st == 201 and j == {'name': 'bob'}, 'POST /signup: 201')
    check(api('POST', '/signup', {'name': 'bob', 'password': 'another-pass-1'}, xff=addr())[0] == 409, '... a name taken: 409')
    check(api('POST', '/signup', {'name': 'shorty', 'password': 'short'}, xff=addr())[0] == 400, '... a short password: 400')
    for n in ('carol', 'dave', 'erin', 'op1', 'op2'):
        api('POST', '/signup', {'name': n, 'password': PASSWORDS[n]}, xff=addr())

    # --- malformed and oversized
    st, j, _ = api('POST', '/signup', raw=b'{"name": "x", ', xff=addr())
    check(st == 400 and j['error'] == 'bad_json' and 'message' in j, 'malformed JSON: 400 with an error and a message')
    check(api('POST', '/signup', raw=b'[1, 2]', xff=addr())[0] == 400, 'a JSON array, not an object: 400')
    check(api('POST', '/signup', {'name': 5, 'password': ['x']}, xff=addr())[0] == 400, 'fields of the wrong type: 400')
    check(api('POST', '/signup', raw=b'{"name": "\xff\xfe"}', xff=addr())[0] == 400, 'invalid UTF-8 in the body: 400')
    st, j, _ = api('POST', '/login', raw=b'{"name": "' + b'a' * 70000 + b'"}', xff=addr())
    check(st == 413 and j and j['error'] == 'too_large', 'a body over 64 KiB: 413')
    check(api('GET', '/no/such/thing')[0] == 404, 'a path the API has not: 404')
    check(api('GET', '/server')[0] == 200, '... and the server goes on')

    # --- log-in, the cookie, CSRF
    st, j, _ = api('POST', '/login', {'name': 'bob', 'password': 'not-the-password'}, xff=addr())
    check(st == 401 and j['error'] == 'wrong_password', 'a wrong password: 401')
    bob, st, j, h = login('bob', PASSWORDS['bob'])
    sc = h.get('set-cookie', '')
    check(bob is not None and 'HttpOnly' in sc and 'SameSite=Strict' in sc and 'Secure' not in sc and len(j['csrf']) >= 32,
          'POST /login: the session in an HttpOnly, SameSite=Strict cookie (no Secure: --api-insecure-cookies), a CSRF value')
    check(bob and bob.cookie not in json.dumps(j), '... the session itself is not in the answer')
    st, j, _ = api('GET', '/me', who=bob)
    check(st == 200 and j['name'] == 'bob' and j['role'] == 'player' and j['auth'] == 'session', 'GET /me with the cookie')
    st, j, _ = api('PUT', '/me/hide_stats', {'hide_stats': False}, who=bob, csrf=False)
    check(st == 403 and j['error'] == 'csrf', 'a change with the cookie but no CSRF value: refused (403)')
    st, _, _ = api('PUT', '/me/hide_stats', {'hide_stats': False}, who=Who(bob.cookie, 'f' * 64))
    check(st == 403, '... a wrong CSRF value: refused')
    check(api('PUT', '/me/hide_stats', {'hide_stats': False}, who=bob)[0] == 200, '... with it: done')
    check(api('GET', '/me', who=Who('hks_' + '0' * 64))[0] == 401, 'a made-up session: 401')
    st, _, _ = api('POST', '/login', {'name': 'bob', 'password': 'not-the-password'}, who=Who('hks_' + '0' * 64), xff=addr())
    check(st == 401 and api('GET', '/server', who=Who('hks_' + '0' * 64))[0] == 200, "... an old cookie keeps no one from the public calls")

    # --- the first admin must change its password
    admin, st, j, _ = login('admin', ADMIN_PW)
    check(admin is not None and j['must_change_password'] is True, 'the first admin logs in, marked must change')
    st, j, _ = api('GET', '/users', who=admin)
    check(st == 403 and j['error'] == 'must_change_password', '... and can do nothing else first (403)')
    check(api('GET', '/me', who=admin)[0] == 200, '... but read /me')
    NEW_ADMIN = 'admin-new-password-1'
    secrets.append(NEW_ADMIN)
    st, _, _ = api('POST', '/me/password', {'old_password': ADMIN_PW, 'password': NEW_ADMIN}, who=admin)
    check(st == 200 and api('GET', '/users', who=admin)[0] == 200, '... changes it, then may')

    # --- roles
    op1, _, _, _ = login('op1', PASSWORDS['op1'])
    op2, _, _, _ = login('op2', PASSWORDS['op2'])
    check(api('PUT', '/users/op1', {'role': 'operator'}, who=bob)[0] == 403, 'a player gives no role (403)')
    st, j, _ = api('PUT', '/users/op1', {'role': 'operator'}, who=admin)
    check(st == 200 and j['role'] == 'operator' and api('PUT', '/users/op2', {'role': 'operator'}, who=admin)[0] == 200,
          'the admin makes two operators')
    for method, path, body in (('GET', '/arcades', None), ('POST', '/arcades', {'name': 'X', 'prefecture': 13}), ('GET', '/machines', None),
                               ('GET', '/users', None), ('GET', '/audit', None), ('GET', '/config', None), ('GET', '/settings', None),
                               ('PUT', '/config', {'signup': 'closed'}), ('POST', '/users/carol/reset', None)):
        check(api(method, path, body, who=bob)[0] == 403, f'a player: {method} {path} refused (403)')
    for method, path, body in (('GET', '/users', None), ('GET', '/audit', None), ('PUT', '/config', {'signup': 'closed'}),
                               ('GET', '/settings', None), ('POST', f'/cards/{CARD_C}/unlock', None)):
        check(api(method, path, body, who=op1)[0] == 403, f'an operator: {method} {path} refused (403)')
    st, a1, _ = api('POST', '/arcades', {'name': 'SHOP ONE', 'prefecture': 27}, who=op1)
    st2, a2, _ = api('POST', '/arcades', {'name': 'SHOP TWO', 'prefecture': 1}, who=op2)
    check(st == 201 and st2 == 201 and a1['owner'] == 'op1' and a1['prefecture'] == 27, 'each operator adds an arcade (201)')
    A1, A2 = a1['id'], a2['id']
    check([x['id'] for x in api('GET', '/arcades', who=op1)[1]['arcades']] == [A1], 'an operator lists its own arcades only')
    check(len(api('GET', '/arcades', who=admin)[1]['arcades']) == 2, 'the admin lists every one')
    check(api('PUT', f'/arcades/{A1}', {'name': 'MINE NOW'}, who=op2)[0] in (403, 404), "an operator can't rename another's arcade")
    check(api('POST', f'/arcades/{A1}/machines', {'name': 'CAB'}, who=op2)[0] in (403, 404), "... nor add a machine to it")
    st, j, _ = api('PUT', f'/arcades/{A2}', {'name': 'SHOP 2', 'prefecture': 2}, who=admin)
    check(st == 200 and j['name'] == 'SHOP 2' and j['prefecture'] == 2, 'the admin edits any arcade')
    check(api('POST', '/arcades', {'name': 'BAD', 'prefecture': 99}, who=op1)[0] == 400, 'a prefecture out of range: 400')

    # --- machines and their keys
    st, mj, _ = api('POST', f'/arcades/{A1}/machines', {'name': 'CAB ONE'}, who=op1, makes=['key'])
    KEY = mj.get('key', '') if mj else ''
    PCBID = mj.get('pcbid', '') if mj else ''
    seen[-1] = (seen[-1][0], {KEY, KEY.split('.')[-1]})
    secrets += [KEY, KEY.split('.')[-1]] if KEY else []
    check(st == 201 and KEY.startswith('hk2.' + PCBID + '.') and len(PCBID) == 20, 'a machine added: its key in that answer (201)')
    st, j, _ = api('GET', f'/machines/{PCBID}', who=op1)
    check(st == 200 and j['has_key'] is True and 'key' not in j, '... GET it: has_key, never the key')
    check(api('GET', f'/machines/{PCBID}', who=op2)[0] in (403, 404), "another operator doesn't see it")
    a = game_call(PORT, KEY, PCBID, '<F14 type="game" method="get"/>')
    check(a.startswith(b'HTTP/1.0 200') and b'<response' in a, 'the key works on the game port (key_client)')
    st, kj, _ = api('POST', f'/machines/{PCBID}/key', who=op1)
    KEY2 = kj.get('key', '') if kj else ''
    seen[-1] = (seen[-1][0], {KEY2, KEY2.split('.')[-1]})
    secrets += [KEY2, KEY2.split('.')[-1]] if KEY2 else []
    check(st == 200 and KEY2.startswith('hk2.' + PCBID + '.') and KEY2 != KEY, 'POST /machines/PCBID/key: a new key, once')
    check(game_call(PORT, KEY, PCBID, '<F14 type="game" method="get"/>').startswith(b'error:'), '... the old one refused')
    check(game_call(PORT, KEY2, PCBID, '<F14 type="game" method="get"/>').startswith(b'HTTP/1.0 200'), '... the new one taken')
    st, j, _ = api('PUT', f'/machines/{PCBID}', {'name': 'CAB 1', 'enabled': True}, who=op1)
    check(st == 200 and j['name'] == 'CAB 1', 'PUT /machines/PCBID: renamed')

    # --- an arcade's settings and a game's answer
    st, j, _ = api('GET', f'/arcades/{A1}/settings', who=op1)
    games = {g['id']: g for g in j['games']} if j else {}
    ne = [s for s in games.get('popn14', {}).get('settings', []) if s['name'] == 'net_event']
    check(st == 200 and ne and ne[0]['kind'] == 'number' and 'min' in ne[0] and 'max' in ne[0] and ne[0]['help'] and ne[0]['label'] and
          ne[0]['value'] is None and 'default' in ne[0], 'GET /arcades/ID/settings: each setting with value, default, range and help')
    check('popn-pc-later' not in games, '... the unlisted row is not among them')
    st, j, _ = api('PUT', f'/arcades/{A1}/settings', {'settings': [{'game': 'popn14', 'name': 'net_event', 'value': 3}]}, who=op1)
    ne = [s for g in (j or {}).get('games', []) if g['id'] == 'popn14' for s in g['settings'] if s['name'] == 'net_event']
    check(st == 200 and ne and ne[0]['value'] == '3', 'PUT /arcades/ID/settings: set, and listed back')
    check(api('PUT', f'/arcades/{A1}/settings', {'settings': [{'game': 'popn14', 'name': 'net_event', 'value': 9}]}, who=op1)[0] == 400,
          '... a value out of range: 400')
    check(api('PUT', f'/arcades/{A1}/settings', {'settings': [{'game': 'popn14', 'name': 'net_event', 'value': 1}]}, who=op2)[0] in (403, 404),
          "... another operator's arcade: refused")
    check(api('PUT', f'/arcades/{A1}/settings', {'settings': [{'game': 'popn-pc-later', 'name': 'unlock', 'value': 0}]}, who=op1)[0] == 404,
          '... the unlisted row: 404')
    a = game_call(PORT, KEY2, PCBID, '<F14 type="game" method="get"/>')
    check(b'net_event="3"' in a, "the arcade's machine gets the arcade's value in the game's answer")
    st, j, _ = api('GET', '/settings', who=admin)
    check(st == 200 and j['server'] and all('default' in s for s in j['server']) and j['games'], "GET /settings: the server's own and the games'")
    st, _, _ = api('PUT', '/settings', {'settings': [{'game': 'popn14', 'name': 'net_event', 'value': '1'}]}, who=admin)
    check(st == 200, "PUT /settings: the server's value")
    st, j, _ = api('PUT', f'/arcades/{A1}/settings', {'settings': [{'game': 'popn14', 'name': 'net_event', 'value': None}]}, who=op1)
    a = game_call(PORT, KEY2, PCBID, '<F14 type="game" method="get"/>')
    check(st == 200 and b'net_event="1"' in a, "the arcade's value cleared (null): the server's in the answer")

    # --- cards: linking, the lock, the limit
    st, _, _ = api('POST', '/me/cards', {'card': CARD_A.lower(), 'pin': PIN_A}, who=bob)
    check(st == 201, 'POST /me/cards: linked by the PIN (201)')
    st, j, _ = api('GET', '/me/cards', who=bob)
    check(st == 200 and [c['card'] for c in j['cards']] == [CARD_A], 'GET /me/cards: as the store writes the number')
    carol, _, _, _ = login('carol', PASSWORDS['carol'])
    dave, _, _, _ = login('dave', PASSWORDS['dave'])
    erin, _, _, _ = login('erin', PASSWORDS['erin'])
    check(api('POST', '/me/cards', {'card': CARD_B, 'pin': PIN_B}, who=carol)[0] == 201, "carol links hers")
    check(api('POST', '/me/cards', {'card': CARD_A, 'pin': PIN_A}, who=carol)[0] == 409, "another's card: 409")
    st, j, _ = api('GET', f'/cards/{CARD_C}', who=admin)
    check(st == 200 and j['locked'] is False and j['wrong_pins'] == 0 and j['tries_left'] == 5,
          'GET /cards/CARD: locked, wrong_pins and tries_left there with no wrong PIN yet')
    api('POST', '/me/cards', {'card': CARD_C, 'pin': '0000'}, who=dave)
    st, j, _ = api('GET', f'/cards/{CARD_C}', who=admin)
    check(st == 200 and j['locked'] is False and j['wrong_pins'] == 1 and j['tries_left'] == 4, '... one wrong PIN: 1, 4 left')
    results = [api('POST', '/me/cards', {'card': CARD_C, 'pin': '0000'}, who=dave)[1]['error'] for _ in range(4)]
    st, j, _ = api('POST', '/me/cards', {'card': CARD_C, 'pin': PIN_C}, who=dave)
    check(results[0] == 'wrong_pin' and st == 403 and j['error'] == 'card_locked', 'five wrong PINs lock the card: the right one refused')
    st, j, _ = api('GET', f'/cards/{CARD_C}', who=admin)
    check(st == 200 and j['locked'] is True and j['wrong_pins'] == 5 and j['tries_left'] == 0, '... GET /cards/CARD: locked, 5, none left')
    check(api('POST', f'/cards/{CARD_C}/unlock', who=admin)[0] == 200, '... the admin unlocks it')
    check(api('POST', '/me/cards', {'card': CARD_C, 'pin': PIN_C}, who=dave)[0] == 201, '... then it links')
    check(api('DELETE', f'/me/cards/{CARD_B}', who=bob)[0] == 404, "unlinking another's card: 404")
    st, j, _ = api('GET', '/users/bob/cards', who=admin)
    check(st == 200 and [c['card'] for c in j['cards']] == [CARD_A], "GET /users/NAME/cards: an admin lists a user's cards")
    check(api('GET', '/users/bob/cards', who=carol)[0] == 403 and api('GET', '/users/nobody/cards', who=admin)[0] == 404,
          '... a player is refused (403); no such user: 404')
    codes = [api('POST', '/me/cards', {'card': '2010010000000F99', 'pin': '1111'}, who=erin) for _ in range(21)]
    check(all(c[0] == 404 for c in codes[:20]) and codes[20][0] == 429 and codes[20][2].get('retry-after'),
          'card links: 20 an hour an account, then 429 with Retry-After')

    # --- stats, names, hidden stats, card numbers
    st, j, _ = api('GET', '/me/stats', who=bob)
    s13 = [s for s in j['stats'] if s['game'] == 'iidx13'] if j else []
    check(st == 200 and s13 and s13[0]['card'] == CARD_A and s13[0]['player'] == SJIS_NAME and s13[0]['scores'][0]['score'] == 1500,
          "GET /me/stats: one's own card, the game's Shift-JIS name as UTF-8, the scores")
    st, j, _ = api('GET', '/players/bob/stats', who=carol)
    check(st == 200 and j['stats'] and CARD_A not in json.dumps(j) and 'card' not in j['stats'][0], "another's stats: never the card number")
    st, j, _ = api('GET', '/players/bob/stats', who=admin)
    check(st == 200 and CARD_A in json.dumps(j), '... an admin sees it')
    check({'name': 'bob'} in api('GET', '/players', who=carol)[1]['players'], 'GET /players: bob listed')
    st, j, _ = api('GET', '/games/iidx13/top?song=1001&chart=1', who=carol)
    rows = j['scores'] if j else []
    check(st == 200 and [r['score'] for r in rows] == [1700, 1600, 1500] and rows[0]['user'] == 'carol' and rows[2]['user'] == 'bob',
          'GET /games/ID/top: best first, with user names')
    check(rows and rows[1]['user'] is None and rows[1]['player'] == '' and rows[1].get('player_hex') == '81ff',
          "... a card no one linked by the game's name, which can't be text: its bytes as hex")
    check(not any(c in json.dumps(j) for c in (CARD_A, CARD_B, CARD_D)), '... no card number')
    check(rows and [r['clear_name'] for r in rows] == ['Hard clear', 'Easy clear', 'Hard clear'] and [r['clear'] for r in rows] == [4, 2, 4],
          "... each row's clear mark as a word (clear_name) beside its number")
    check(j and j['shows'] == {'plays': True, 'song_plays': True, 'clear': True}, '... and what the game\'s rows can hold (shows)')
    st, j, _ = api('GET', '/games/iidx13/top?song=1003', who=carol)
    check(st == 200 and j['scores'] == [], '... a score of 0: not among the best scores')
    st, j, _ = api('GET', '/me/stats', who=carol)
    games = {s['game']: s for s in j['stats']} if j else {}
    r1003 = [r for r in games.get('iidx13', {}).get('scores', []) if r['song'] == 1003]
    check(st == 200 and r1003 and r1003[0]['score'] == 0 and r1003[0]['clear_name'] == 'Failed',
          "... the player's own stats keep it (DistorteD's clear 1: Failed)")
    dm = games.get('gfdm573-dm', {})
    check(dm.get('title') == 'DrumMania (System 573)' and dm.get('shows') == {'plays': True, 'song_plays': True, 'clear': True} and
          sorted((r['song'], r['score'], r['clear'], r['clear_name']) for r in dm.get('scores', [])) == [(12, 80000, 1, 'Cleared'), (13, None, 0, 'Failed')],
          "System 573 DrumMania in one's stats: its title, a cleared stage's score, a failed one's mark (no score)")
    p12 = sorted((r['chart'], r['clear'], r['clear_name']) for r in games.get('popn12', {}).get('scores', []) if r['song'] == 5)
    check(p12 == [(0, 1, 'Cleared'), (1, 2, 'Cleared'), (2, 3, 'Cleared')], "pop'n's better medals (2, 3): Cleared too")
    st, j, _ = api('GET', '/games', who=carol)
    shows = {g['id']: g['shows'] for g in j['games']} if j else {}
    want = {'popn12': (False, False, True), 'popn15': (True, False, True), 'iidx9': (False, True, True), 'iidx10': (False, True, True),
            'iidx13': (True, True, True), 'drmnvj': (True, True, False), 'gfdm573-gf': (True, True, True),
            'iidx16': (True, True, False)}   # (EMPRESS: its marks have no word, so no clear column)
    check(st == 200 and all(shows.get(k) == dict(zip(('plays', 'song_plays', 'clear'), v)) for k, v in want.items()),
          'GET /games: each game says which columns its rows can fill (shows)')
    st, j, _ = api('GET', f'/cards/{CARD_A}', who=admin)
    p13 = [g for g in j['games'] if g['id'] == 'iidx13'] if j else []
    check(st == 200 and p13 and p13[0]['player'] == SJIS_NAME, "GET /cards/CARD: the game's Shift-JIS name as UTF-8")
    st, j, _ = api('GET', f'/cards/{CARD_D}', who=admin)
    p13 = [g for g in j['games'] if g['id'] == 'iidx13'] if j else []
    check(st == 200 and p13 and p13[0]['player'] == '' and p13[0].get('player_hex') == '81ff', '... one that can\'t be text: its bytes as hex')
    st, j, _ = api('GET', '/players', who=carol)
    check(st == 200 and sorted(p['name'] for p in j['players']) == ['bob', 'carol', 'dave'],
          'GET /players: only accounts with a card (no admin, operators, nor erin)')
    st, j, _ = api('GET', '/users', who=admin)
    check(st == 200 and {'admin', 'op1', 'erin'} <= {u['name'] for u in j['users']}, "... the admin's users list: every account")
    st, j, _ = api('GET', '/names', who=admin)
    check(st == 200 and isinstance(j.get('set'), bool) and 'folder' not in j, 'GET /names: "set", no folder')
    check(api('PUT', '/me/hide_stats', {'hide_stats': True}, who=bob)[0] == 200, 'bob hides his stats')
    st, j, _ = api('GET', '/players/bob/stats', who=carol)
    check(st == 403 and j['error'] == 'stats_hidden', '... others are refused (403)')
    check({'name': 'bob'} not in api('GET', '/players', who=carol)[1]['players'], '... and he is not listed')
    rows = api('GET', '/games/iidx13/top?song=1001', who=carol)[1]['scores']
    check([r['score'] for r in rows] == [1700, 1600], '... nor among the best scores')
    check(api('GET', '/me/stats', who=bob)[0] == 200, '... his own still shown to him')
    check(api('GET', '/games/iidx13/top?limit=0', who=carol)[0] == 400 and api('GET', '/games/nogame/top', who=carol)[0] == 404,
          'a bad limit: 400; no such game: 404')

    # --- the unlisted row
    check(api('POST', '/me/cards', {'card': CARD_M, 'pin': PIN_M}, who=bob)[0] == 201, "bob links a card with the unlisted row's player")
    check(not any(s['game'] == 'popn-pc-later' for s in api('GET', '/me/stats', who=bob)[1]['stats']), '... its stats not shown')
    check(not any(g['id'] == 'popn-pc-later' for g in api('GET', '/games', who=bob)[1]['games']), '... not in GET /games')
    check(api('GET', '/games/popn-pc-later/top', who=bob)[0] == 404, '... its best scores: 404')
    check(api('PUT', '/config', {'list_all_games': True}, who=admin)[1]['list_all_games'] is True, 'the admin lists all games')
    later = [s for s in api('GET', '/me/stats', who=bob)[1]['stats'] if s['game'] == 'popn-pc-later']
    check(any(g['id'] == 'popn-pc-later' for g in api('GET', '/server')[1]['games']) and later, '... then it is listed and shown')
    check(later and later[0]['scores'][0]['clear'] == 3 and later[0]['scores'][0]['clear_name'] is None and later[0]['shows']['clear'] is False,
          "... a clear mark whose meaning isn't known: its number, no word, and no clear column (shows)")
    api('PUT', '/config', {'list_all_games': False}, who=admin)
    check(not any(g['id'] == 'popn-pc-later' for g in api('GET', '/server')[1]['games']), '... and hidden again')

    # --- personal tokens
    st, j, _ = api('POST', '/me/tokens', {'label': 'phone', 'scope': 'read'}, who=bob, makes=['token'])
    RT = j.get('token', '') if j else ''
    seen[-1] = (seen[-1][0], {RT})
    st2, j2, _ = api('POST', '/me/tokens', {'label': 'script', 'scope': 'write'}, who=bob)
    WT = j2.get('token', '') if j2 else ''
    seen[-1] = (seen[-1][0], {WT})
    secrets += [RT, WT]
    check(st == 201 and st2 == 201 and RT.startswith('hkt_') and WT.startswith('hkt_'), 'POST /me/tokens: a token, once (201)')
    st, j, _ = api('GET', '/me', who=Who(token=RT))
    check(st == 200 and j['name'] == 'bob' and j['auth'] == 'token' and j['scope'] == 'read', 'a bearer token reads /me')
    st, j, _ = api('PUT', '/me/hide_stats', {'hide_stats': False}, who=Who(token=RT))
    check(st == 403 and j['error'] == 'read_only_token', '... a read token cannot write (403)')
    check(api('PUT', '/me/hide_stats', {'hide_stats': False}, who=Who(token=WT))[0] == 200, '... a write one can, with no CSRF value')
    check(api('POST', '/me/tokens', {'label': 'more', 'scope': 'write'}, who=Who(token=WT))[0] == 403, '... but makes no token')
    st, j, _ = api('GET', '/me/tokens', who=bob)
    toks = j['tokens'] if j else []
    check(st == 200 and len(toks) == 2 and RT not in json.dumps(j) and WT not in json.dumps(j), 'GET /me/tokens: listed, never their text')
    rid = [t['id'] for t in toks if t['label'] == 'phone'][0] if toks else 0
    check(api('DELETE', f'/me/tokens/{rid}', who=carol)[0] == 404, "another's token can't be revoked")
    check(api('DELETE', f'/me/tokens/{rid}', who=bob)[0] == 200 and api('GET', '/me', who=Who(token=RT))[0] == 401,
          'a revoked token: 401 at once')
    # 600 a minute a token
    stats = [api('GET', '/me', who=Who(token=WT))[0] for _ in range(601)]
    check(stats.count(200) <= 600 and stats[-1] == 429, 'more than 600 calls a minute: 429')

    # --- a disabled user: sessions and tokens stop at once
    st, j, _ = api('POST', '/me/tokens', {'label': 'carol app', 'scope': 'read'}, who=carol)
    CT = j.get('token', '') if j else ''
    seen[-1] = (seen[-1][0], {CT})
    secrets.append(CT)
    check(api('PUT', '/users/carol', {'disabled': True}, who=admin)[0] == 200, 'the admin disables carol')
    check(api('GET', '/me', who=carol)[0] == 401 and api('GET', '/me', who=Who(token=CT))[0] == 401, "... her session and token: 401 at once")
    st, j, _ = api('POST', '/login', {'name': 'carol', 'password': PASSWORDS['carol']}, xff=addr())
    check(st == 403 and j['error'] == 'disabled', '... she cannot log in (403)')
    api('PUT', '/users/carol', {'disabled': False}, who=admin)
    check(api('GET', '/me', who=carol)[0] == 200, '... enabled: her session works again')

    # --- a reset code
    st, j, _ = api('POST', '/users/carol/reset', who=admin, makes=['code'])
    RC = j.get('code', '') if j else ''
    seen[-1] = (seen[-1][0], {RC})
    secrets.append(RC)
    check(st == 200 and len(RC) == 32, 'POST /users/NAME/reset: a code, once')
    CAROL2 = 'carol-new-password-2'
    secrets.append(CAROL2)
    check(api('POST', '/reset', {'code': RC, 'password': CAROL2}, xff=addr())[0] == 200, 'POST /reset: the code and a new password')
    check(api('POST', '/reset', {'code': RC, 'password': 'carol-third-pw'}, xff=addr())[0] == 403, '... used once only')
    check(login('carol', CAROL2)[0] is not None, '... the new password logs in')

    # --- admin's own: config, invitations, users, audit, PIN reset
    check(api('PUT', '/config', {'signup': 'invite'}, who=admin)[1]['signup'] == 'invite', 'PUT /config: sign-up by invitation')
    check(api('POST', '/signup', {'name': 'frank', 'password': 'frank-password-1'}, xff=addr())[0] == 403, '... without a code: 403')
    st, j, _ = api('POST', '/config/invites', {'uses': 1, 'days': 1}, who=admin, makes=['code'])
    IC = j.get('code', '') if j else ''
    seen[-1] = (seen[-1][0], {IC})
    secrets += [IC, 'frank-password-1']
    check(st == 201 and api('POST', '/signup', {'name': 'frank', 'password': 'frank-password-1', 'invite': IC}, xff=addr())[0] == 201,
          '... an invitation (201), used')
    api('PUT', '/config', {'signup': 'open'}, who=admin)
    st, j, _ = api('PUT', f'/cards/{CARD_D}/pin', {'pin': '4321'}, who=admin)
    secrets.append('4321')
    check(st == 200 and j['changed'], "PUT /cards/CARD/pin: the admin resets a card's PIN")
    check(api('DELETE', '/users/frank', who=admin)[0] == 200 and api('GET', '/users/frank', who=admin)[0] == 404, 'DELETE /users/NAME')
    st, j, _ = api('GET', '/audit?limit=500', who=admin)
    acts = {l['action'] for l in j['audit']} if j else set()
    check(st == 200 and {'user.login', 'token.add', 'token.revoke', 'machine.key', 'setting.set', 'card.link'} <= acts, 'GET /audit: the actions')

    # --- the limits on log-in and sign-up
    tries = [api('POST', '/login', {'name': 'ghost', 'password': 'nope-nope-1'}, xff=addr())[0] for _ in range(11)]
    check(tries[:10] == [401] * 10 and tries[10] == 429, 'log-in: 10 tries a minute a name, then 429')
    one = addr()
    tries = [api('POST', '/login', {'name': f'ghost{i}', 'password': 'nope-nope-1'}, xff=one) for i in range(11)]
    check([t[0] for t in tries[:10]] == [401] * 10 and tries[10][0] == 429 and int(tries[10][2].get('retry-after', '0')) > 0,
          '... 10 a minute an address, then 429 with Retry-After')
    one = addr()
    tries = [api('POST', '/signup', {'name': f'newbie{i}', 'password': 'newbie-password-1'}, xff=one)[0] for i in range(6)]
    check(tries[:5] == [201] * 5 and tries[5] == 429, 'sign-up: 5 an hour an address, then 429')
    secrets.append('newbie-password-1')

    # --- logout
    check(api('POST', '/logout', who=dave)[0] == 200 and api('GET', '/me', who=dave)[0] == 401, 'POST /logout: the session ends')

    # --- nothing secret in any answer, nor in the server's output
    time.sleep(0.3)
    serve.terminate()
    serve.wait(10)
    serve = None
    log.flush()
    log.seek(0)
    output = log.read()
    secret_list = [s for s in secrets if s]

    def holds(text, s):
        if not s.isdigit():
            return s in text
        # a PIN: as a number of its own, not four digits inside a longer one, an id or a key
        text = re.sub(r'[A-Za-z0-9_.-]*[A-Za-z][A-Za-z0-9_.-]*', ' ', text)
        return re.search(r'(?<!\d)' + re.escape(s) + r'(?!\d)', text) is not None
    leaks = [(i, s) for i, (body, allowed) in enumerate(seen) for s in secret_list + PINS if s not in allowed and holds(body, s)]
    check(not leaks, f'no password, PIN, token, code or key in any of {len(seen)} answers but the one that made it' +
          (f' (answer {leaks[0][0]})' if leaks else ''))
    check(not any('pbkdf2' in b or '$' in b and 'sha256' in b for b, _ in seen), '... and no hash')
    check(not any(T in b or T.replace('/', '\\/') in b for b, _ in seen), "... and no path on the server's disk")
    check(not any(holds(output, s) for s in secret_list + PINS), "none in serve's output")
finally:
    if serve:
        serve.kill()
        serve.wait()
    if log:
        log.close()
    shutil.rmtree(T, ignore_errors=True)

print(f'{failed} failed' if failed else 'all passed')
sys.exit(1 if failed else 0)
