#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""hiraku-server's API description (docs/openapi.yaml, its JSON twin docs/openapi.json, docs/SERVER-API.md)
kept with the code that answers (server/api.cpp):

  - the twin is the YAML's (when Python has PyYAML; tools/eamuse/openapi.py makes it), the document
    is OpenAPI 3.1, every $ref resolves, every operation has a camelCase operationId of its own, a
    summary, its tags, its path's parameters, and only schema keywords this test checks;
  - the operations (method and path) are the routes server/api.cpp registers, no more, no fewer, and
    each one's role (x-hiraku-role, x-hiraku-session-only, x-hiraku-while-must-change) and security
    are the route's rule;
  - the server serves the two files as they are (GET /api/v1/openapi.yaml, .json);
  - every answer fits the description: each operation asked with no one logged in, then a made-up
    admin's walk through every operation, then every answer of tests/eamuse/test_api.py (run here
    with HK_API_RECORD): its status is among the operation's responses and its body fits that
    response's schema. Every operation must have answered a 2xx that fits.

Made-up names, cards and PINs.

    tests/eamuse/test_openapi.py HIRAKU-SERVER KEY_CLIENT SOURCE-DIR      exit 0 when all hold
"""
import http.client, json, os, re, shutil, socket, subprocess, sys, tempfile, time

SERVER, CLIENT, SRC = sys.argv[1], sys.argv[2], os.path.abspath(sys.argv[3])
YAML_FILE = os.path.join(SRC, 'docs', 'openapi.yaml')
JSON_FILE = os.path.join(SRC, 'docs', 'openapi.json')
API_CPP = os.path.join(SRC, 'server', 'api.cpp')
T = tempfile.mkdtemp(prefix='hk-openapi-')
DB = os.path.join(T, 'server.db')
METHODS = ('get', 'put', 'post', 'delete')
failed = 0


def check(ok, what):
    global failed
    print(('ok    ' if ok else 'FAIL  ') + what, flush=True)
    failed += not ok


with open(JSON_FILE, encoding='utf-8') as f:
    TWIN_TEXT = f.read()
DOC = json.loads(TWIN_TEXT)

# --- the twin, and the document itself
try:
    sys.path.insert(0, os.path.join(SRC, 'tools', 'eamuse'))
    import openapi as tool
    import yaml  # noqa: F401  (whether PyYAML is there)
    have_yaml = True
except ImportError:
    have_yaml = False
if have_yaml:
    check(tool.twin_text(tool.plain(tool.load_yaml(YAML_FILE))) == TWIN_TEXT,
          'docs/openapi.json is docs/openapi.yaml\'s (else run tools/eamuse/openapi.py)')
else:
    print('skip  no PyYAML here: docs/openapi.json not compared with the YAML (the rest reads the JSON)')

check(DOC.get('openapi') == '3.1.0' and DOC.get('info', {}).get('version') and DOC['info'].get('license', {}).get('identifier') == 'GPL-3.0-or-later',
      'OpenAPI 3.1.0, a version, the GPL licence')


def resolve(ref):
    if not ref.startswith('#/'):
        raise KeyError(ref)
    node = DOC
    for part in ref[2:].split('/'):
        node = node[part.replace('~1', '/').replace('~0', '~')]
    return node


def all_refs(node):
    if isinstance(node, dict):
        for k, v in node.items():
            if k == '$ref' and isinstance(v, str):
                yield v
            else:
                yield from all_refs(v)
    elif isinstance(node, list):
        for v in node:
            yield from all_refs(v)


bad_refs = []
for r in all_refs(DOC):
    try:
        resolve(r)
    except (KeyError, TypeError):
        bad_refs.append(r)
check(not bad_refs, f'every $ref resolves ({sum(1 for _ in all_refs(DOC))} of them)' + (f': not {bad_refs[:3]}' if bad_refs else ''))


def deref(node):
    while isinstance(node, dict) and '$ref' in node:
        node = resolve(node['$ref'])
    return node


OPS = {}   # (METHOD, path) -> the operation
for path, item in DOC['paths'].items():
    for m in METHODS:
        if m in item:
            OPS[(m.upper(), path)] = item[m]
tags = {t['name'] for t in DOC.get('tags', [])}
ids = [op.get('operationId', '') for op in OPS.values()]
check(all(re.fullmatch(r'[a-z][A-Za-z0-9]*', i) for i in ids) and len(set(ids)) == len(ids),
      f'{len(ids)} operations, each with a camelCase operationId of its own')
check(all(op.get('summary') and op.get('description') and op.get('tags') and set(op['tags']) <= tags and op.get('responses') for op in OPS.values()),
      '... each with a summary, a description, known tags and responses')
missing = []
for (m, path), op in OPS.items():
    params = [deref(p) for p in DOC['paths'][path].get('parameters', []) + op.get('parameters', [])]
    declared = {p['name'] for p in params if p.get('in') == 'path'}
    if declared != set(re.findall(r'\{(\w+)\}', path)):
        missing.append(f'{m} {path}')
check(not missing, "... and its path's parameters declared" + (f': not {missing[:3]}' if missing else ''))

# the schema keywords the validator below understands (another one would be taken without a check)
KEYWORDS = {'$ref', 'type', 'enum', 'const', 'properties', 'required', 'additionalProperties', 'items', 'minItems', 'maxItems',
            'minimum', 'maximum', 'minLength', 'maxLength', 'pattern', 'anyOf', 'oneOf'}
NOTES = {'description', 'example', 'examples', 'default', 'title', 'format', 'deprecated', 'readOnly', 'writeOnly'}
unknown = set()


def keywords(schema):
    if isinstance(schema, dict):
        unknown.update(set(schema) - KEYWORDS - NOTES)
        for k in ('items', 'additionalProperties'):
            if isinstance(schema.get(k), dict):
                keywords(schema[k])
        for s in schema.get('properties', {}).values():
            keywords(s)
        for k in ('anyOf', 'oneOf'):
            for s in schema.get(k, []):
                keywords(s)


for s in DOC['components']['schemas'].values():
    keywords(s)
check(not unknown, f"{len(DOC['components']['schemas'])} schemas, every keyword one this test checks" + (f': not {sorted(unknown)}' if unknown else ''))


# --- a validator for the JSON Schema the description uses
def json_type(v):
    if v is None:
        return 'null'
    if isinstance(v, bool):
        return 'boolean'
    if isinstance(v, int):
        return 'integer'
    if isinstance(v, float):
        return 'number'
    if isinstance(v, str):
        return 'string'
    if isinstance(v, list):
        return 'array'
    return 'object'


def same_value(a, b):
    return json_type(a) == json_type(b) and a == b


def validate(v, schema, where='$'):
    """the first way v doesn't fit schema, or None"""
    if '$ref' in schema:
        why = validate(v, resolve(schema['$ref']), where)
        if why:
            return why
    t = json_type(v)
    if 'type' in schema:
        want = schema['type'] if isinstance(schema['type'], list) else [schema['type']]
        if t not in want and not (t == 'integer' and 'number' in want):
            return f'{where}: {t}, not {"/".join(want)}'
    if 'enum' in schema and not any(same_value(v, e) for e in schema['enum']):
        return f'{where}: {v!r} not one of {schema["enum"]}'
    if 'const' in schema and not same_value(v, schema['const']):
        return f'{where}: {v!r}, not {schema["const"]!r}'
    if t == 'object':
        for k in schema.get('required', []):
            if k not in v:
                return f'{where}: no "{k}"'
        props = schema.get('properties', {})
        extra = schema.get('additionalProperties', True)
        for k, x in v.items():
            if k in props:
                why = validate(x, props[k], f'{where}.{k}')
            elif extra is False:
                why = f'{where}: "{k}" is not described'
            elif isinstance(extra, dict):
                why = validate(x, extra, f'{where}.{k}')
            else:
                why = None
            if why:
                return why
    if t == 'array':
        if len(v) < schema.get('minItems', 0) or len(v) > schema.get('maxItems', len(v)):
            return f'{where}: {len(v)} items'
        for i, x in enumerate(v):
            why = validate(x, schema.get('items', {}), f'{where}[{i}]')
            if why:
                return why
    if t in ('integer', 'number'):
        if v < schema.get('minimum', v) or v > schema.get('maximum', v):
            return f'{where}: {v} out of range'
    if t == 'string':
        if len(v) < schema.get('minLength', 0) or len(v) > schema.get('maxLength', len(v)):
            return f'{where}: length {len(v)}'
        if 'pattern' in schema and not re.search(schema['pattern'], v):
            return f'{where}: {v!r} not {schema["pattern"]}'
    for k in ('anyOf', 'oneOf'):
        if k in schema:
            fits = sum(validate(v, s, where) is None for s in schema[k])
            if fits == 0 or (k == 'oneOf' and fits > 1):
                return f'{where}: fits {fits} of {k}'
    return None


def find_op(method, path):
    """the operation for a concrete path (a literal part beats a parameter), or (None, None)"""
    parts = path.split('?')[0].split('/')
    best, score = None, -1
    for (m, p) in OPS:
        tp = p.split('/')
        if m != method or len(tp) != len(parts):
            continue
        if all(a == b or (a.startswith('{') and b) for a, b in zip(tp, parts)):
            s = sum(a == b for a, b in zip(tp, parts))
            if s > score:
                best, score = (m, p), s
    return best if best else (None, None)


fits_2xx = set()      # the operations that answered a 2xx that fits
misfits = []          # (what, why)
answers = [0]


def judge(method, path, status, ctype, body, source):
    """an answer against the description"""
    answers[0] += 1
    key = find_op(method, path)
    what = f'{source}: {method} {path} {status}'
    if key[0] is None:
        resp = DOC['components']['responses']['NotFound'] if status == 404 else None
    else:
        resp = OPS[key]['responses'].get(str(status))
    if resp is None:
        misfits.append((what, 'status not described'))
        return
    resp = deref(resp)
    content = resp.get('content', {})
    if 'application/json' in content:
        if ctype != 'application/json; charset=utf-8':
            misfits.append((what, f'Content-Type {ctype}'))
            return
        try:
            j = json.loads(body)
        except ValueError:
            misfits.append((what, 'not JSON'))
            return
        why = validate(j, content['application/json'].get('schema', {}))
        if why:
            misfits.append((what, why))
            return
    elif content and not any(ctype.startswith(t) for t in content):
        misfits.append((what, f'Content-Type {ctype}, not {list(content)}'))
        return
    if 200 <= status < 300 and key[0]:
        fits_2xx.add(key)


# --- the routes the code registers
with open(API_CPP, encoding='utf-8') as f:
    CPP = f.read()
RULES = {'anyone': ('anyone', False, False), 'user': ('user', False, False), 'changing': ('user', False, True),
         'session': ('user', True, False), 'password': ('user', True, True), 'op': ('operator', False, False), 'admin': ('admin', False, False)}
found = re.findall(r'\broute\("(GET|POST|PUT|DELETE)",\s*"(/[^"]*)",\s*(\w+)', CPP)
calls = len(re.findall(r'\broute\(', CPP)) - len(re.findall(r'void route\(', CPP))
check(found and len(found) == calls, f'server/api.cpp: {len(found)} routes registered, each with a literal method, path and rule')
CODE = {(m, re.sub(r':(\w+)', r'{\1}', p)): rule for m, p, rule in found}
check(set(RULES) >= set(CODE.values()), 'the test knows every rule the routes use' +
      (f': not {sorted(set(CODE.values()) - set(RULES))}' if not set(RULES) >= set(CODE.values()) else ''))
only_code, only_spec = sorted(set(CODE) - set(OPS)), sorted(set(OPS) - set(CODE))
check(not only_code and not only_spec, "the description's operations are the code's routes" +
      (f': in the code only {only_code}' if only_code else '') + (f'; in the description only {only_spec}' if only_spec else ''))
wrong = []
for key in set(CODE) & set(OPS):
    op, (role, session_only, while_changing) = OPS[key], RULES.get(CODE[key], ('?', False, False))
    if (op.get('x-hiraku-role'), bool(op.get('x-hiraku-session-only')), bool(op.get('x-hiraku-while-must-change'))) != (role, session_only, while_changing):
        wrong.append(f'{key[0]} {key[1]}: x-hiraku-* say {op.get("x-hiraku-role")}, the code {CODE[key]}')
        continue
    if role == 'anyone':
        want = []
    else:
        cookie = ['csrfHeader', 'sessionCookie'] if key[0] != 'GET' else ['sessionCookie']
        want = [cookie] if session_only else [cookie, ['bearerToken']]
    if sorted(sorted(s) for s in op.get('security', [{}])) != sorted(want):
        wrong.append(f'{key[0]} {key[1]}: security {op.get("security")}')
    if role != 'anyone' and f'Role: {role}' not in op['description']:
        wrong.append(f'{key[0]} {key[1]}: the description does not say "Role: {role}"')
check(not wrong, "each operation's role, session-only, must-change and security are its route's rule" + (f': {wrong[:3]}' if wrong else ''))


# --- the live server
def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def put(name, data):
    return subprocess.run([SERVER, 'put', name, '--db', DB], input=data, capture_output=True).returncode == 0


class Who:
    def __init__(self, cookie=None, csrf=None, token=None):
        self.cookie, self.csrf, self.token = cookie, csrf, token


def call(method, path, body=None, who=None, raw=None):
    """(status, JSON or None, headers, body bytes); each answer judged"""
    c = http.client.HTTPConnection('127.0.0.1', API_PORT, timeout=20)
    headers = {}
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    if data is not None:
        headers['Content-Type'] = 'application/json'
    if who and who.token:
        headers['Authorization'] = 'Bearer ' + who.token
    elif who and who.cookie:
        headers['Cookie'] = 'hk_session=' + who.cookie
        if who.csrf:
            headers['X-HK-CSRF'] = who.csrf
    c.request(method, '/api/v1' + path, body=data, headers=headers)
    r = c.getresponse()
    text = r.read()
    hdrs = {k.lower(): v for k, v in r.getheaders()}
    c.close()
    judge(method, path, r.status, hdrs.get('content-type', ''), text.decode('utf-8', errors='replace'), 'walk')
    try:
        return r.status, json.loads(text), hdrs, text
    except ValueError:
        return r.status, None, hdrs, text


serve = None
log = None
try:
    os.makedirs(os.path.join(T, 'empty'))
    out = subprocess.run([SERVER, 'import', os.path.join(T, 'empty'), '--db', DB], capture_output=True).stdout.decode(errors='replace')
    m = re.search(r'password (\S+)', out)
    ADMIN_PW = m.group(1) if m else ''
    CARD, REF, PIN = 'E0040000000000A1', 'R0000000000000A1', '1357'
    check(ADMIN_PW and put('cards', f'cardid={CARD}\trefid={REF}\tpass={PIN}\n'.encode()) and
          put('iidx-FDD-players', f'card={CARD}\trid={REF}\tid=10000001\tname=ALEX\n'.encode()) and
          put('iidx-FDD-scores', b'id=10000001\tmid=1001\tcl=1\tex=1500\tplays=2\tclear=4\n'),
          'a new database with a made-up card, player and score')
    PORT, API_PORT = free_port(), free_port()
    log = open(os.path.join(T, 'serve.txt'), 'w+')
    serve = subprocess.Popen([SERVER, 'serve', '--db', DB, '--port', str(PORT), '--bind', '127.0.0.1', '--no-relay',
                              '--api', f'127.0.0.1:{API_PORT}', '--api-insecure-cookies'], stdout=log, stderr=subprocess.STDOUT)
    for _ in range(300):
        try:
            socket.create_connection(('127.0.0.1', API_PORT)).close()
            break
        except OSError:
            if serve.poll() is not None:
                raise SystemExit('serve ended at start')
            time.sleep(0.1)

    # the two files as they are
    for name, local in (('openapi.yaml', YAML_FILE), ('openapi.json', JSON_FILE)):
        st, _, h, text = call('GET', '/' + name)
        with open(local, 'rb') as f:
            check(st == 200 and text == f.read() and h.get('content-type', '').startswith('application/yaml' if name.endswith('yaml') else 'application/json'),
                  f'GET /api/v1/{name}: the file as it is, with its type')

    # each operation with no one logged in: a route of the server's (never "no such thing in the API")
    SAMPLE = {'card': CARD, 'name': 'nobody', 'id': '1', 'pcbid': '0120A1B2C3D4E5F60718'}
    generic = []
    for (m, path) in sorted(OPS):
        st, j, _, _ = call(m, re.sub(r'\{(\w+)\}', lambda x: SAMPLE[x.group(1)], path))
        if st == 404 and j and j.get('message') == 'no such thing in the API':
            generic.append(f'{m} {path}')
    check(not generic, f'each of the {len(OPS)} operations is a route of the server' + (f': not {generic[:3]}' if generic else ''))
    check(call('GET', '/no/such/thing')[0] == 404 and call('PATCH', '/me')[0] == 404, '... and a path or method it has not: 404')

    # a made-up admin's walk through every operation
    st, j, h, _ = call('POST', '/login', {'name': 'admin', 'password': ADMIN_PW})
    ck = re.search(r'hk_session=([^;]*)', h.get('set-cookie', ''))
    admin = Who(ck.group(1) if ck else '', j.get('csrf') if j else '')
    NEW_PW = 'made-up-admin-password-1'
    call('GET', '/me', who=admin)
    call('GET', '/users', who=admin)   # (403 must_change_password)
    check(call('POST', '/me/password', {'old_password': ADMIN_PW, 'password': NEW_PW}, who=admin)[0] == 200, 'the admin logs in and changes its password')
    call('GET', '/server')
    call('GET', '/health')
    call('POST', '/signup', {'name': 'robin', 'password': 'made-up-password-2'})
    call('POST', '/signup', {'name': 'robin', 'password': 'made-up-password-2'})   # (409)
    st, j, _, _ = call('POST', '/me/tokens', {'label': 'walk', 'scope': 'write'}, who=admin)
    tok = Who(token=j['token'] if j else '')
    st, j, _, _ = call('POST', '/me/tokens', {'label': 'walk read', 'scope': 'read'}, who=admin)
    rtok = Who(token=j['token'] if j else '')
    call('GET', '/me/tokens', who=admin)
    call('GET', '/me', who=tok)
    call('GET', '/me/tokens', who=tok)   # (403 session_only)
    call('PUT', '/me/hide_stats', {'hide_stats': False}, who=rtok)   # (403 read_only_token)
    call('DELETE', f'/me/tokens/{j["id"] if j else 0}', who=admin)
    call('DELETE', '/me/tokens/x', who=admin)   # (404)
    call('PUT', '/me/hide_stats', {'hide_stats': False}, who=tok)
    call('POST', '/me/cards', {'card': CARD, 'pin': '0000'}, who=tok)   # (403 wrong_pin)
    call('POST', '/me/cards', {'card': CARD, 'pin': PIN}, who=tok)
    call('GET', '/me/cards', who=tok)
    call('GET', '/me/stats', who=tok)
    call('DELETE', f'/me/cards/{CARD}', who=tok)
    call('POST', '/me/cards', {'card': CARD, 'pin': PIN}, who=tok)
    call('GET', '/players', who=tok)
    call('GET', '/players/admin/stats', who=tok)
    call('GET', '/players/nobody/stats', who=tok)   # (404)
    call('GET', '/games', who=tok)
    call('GET', '/games/iidx13/songs', who=tok)
    call('GET', '/games/iidx13/top?limit=5', who=tok)
    call('GET', '/games/iidx13/top?limit=500', who=tok)   # (400)
    call('PUT', '/users/robin', {'role': 'operator'}, who=tok)
    call('PUT', '/users/admin', {'role': 'player'}, who=tok)   # (409: the last admin)
    st, a, _, _ = call('POST', '/arcades', {'name': 'Example Arcade', 'prefecture': 13, 'owner': 'robin'}, who=tok)
    A = a['id'] if a else 0
    call('GET', '/arcades', who=tok)
    call('GET', f'/arcades/{A}', who=tok)
    call('PUT', f'/arcades/{A}', {'name': 'Example Arcade 2', 'prefecture': 27}, who=tok)
    call('PUT', f'/arcades/{A}', {'prefecture': 48}, who=tok)   # (400)
    st, mj, _, _ = call('POST', f'/arcades/{A}/machines', {'name': 'Front left'}, who=tok)
    P = mj['pcbid'] if mj else 'x'
    call('GET', f'/arcades/{A}/machines', who=tok)
    call('GET', '/machines', who=tok)
    call('GET', f'/machines/{P.lower()}', who=tok)
    call('PUT', f'/machines/{P}', {'name': 'Front right', 'enabled': False}, who=tok)
    call('POST', f'/machines/{P}/key', who=tok)
    call('GET', f'/arcades/{A}/settings', who=tok)
    call('PUT', f'/arcades/{A}/settings', {'settings': [{'game': 'popn14', 'name': 'net_event', 'value': 3}]}, who=tok)
    call('PUT', f'/arcades/{A}/settings', {'settings': [{'game': 'popn14', 'name': 'no_such', 'value': 3}]}, who=tok)   # (400)
    call('GET', '/settings', who=tok)
    call('PUT', '/settings', {'settings': [{'game': 'popn14', 'name': 'net_event', 'value': None}]}, who=tok)
    call('DELETE', f'/machines/{P}', who=tok)
    call('DELETE', f'/arcades/{A}', who=tok)
    call('GET', f'/arcades/{A}', who=tok)   # (404)
    call('GET', '/users', who=tok)
    call('GET', '/users/robin', who=tok)
    call('GET', '/users/admin/cards', who=tok)
    st, rc, _, _ = call('POST', '/users/robin/reset', who=tok)
    call('POST', '/reset', {'code': rc['code'] if rc else '', 'password': 'made-up-password-3'})
    call('POST', '/reset', {'code': rc['code'] if rc else '', 'password': 'made-up-password-3'})   # (403 bad_code)
    call('GET', '/config', who=tok)
    call('PUT', '/config', {'name': 'Example Games'}, who=tok)
    call('PUT', '/config', {'signup': 'sometimes'}, who=tok)   # (400)
    call('POST', '/config/invites', who=tok)
    call('POST', '/config/invites', {'uses': 2, 'days': 3}, who=tok)
    call('GET', f'/cards/{CARD}', who=tok)
    call('GET', '/cards/E0040000000000FF', who=tok)   # (known false)
    call('POST', f'/cards/{CARD}/unlock', who=tok)   # (403: not locked)
    call('PUT', f'/cards/{CARD}/pin', {'pin': '2468'}, who=tok)
    call('PUT', f'/cards/{CARD}/pin', {'pin': '12'}, who=tok)   # (400)
    call('DELETE', f'/cards/{CARD}', who=tok)
    call('DELETE', f'/cards/{CARD}', who=tok)   # (404)
    for _ in range(5):
        call('POST', '/me/cards', {'card': CARD, 'pin': '0000'}, who=tok)   # (403 wrong_pin, then card_locked)
    call('POST', f'/cards/{CARD}/unlock', who=tok)
    call('GET', '/names', who=tok)
    call('GET', '/audit?limit=50', who=tok)
    call('GET', '/audit?limit=0', who=tok)   # (400)
    call('DELETE', '/users/robin', who=tok)
    call('POST', '/login', {'name': 'admin', 'password': 'not-the-password'})   # (401)
    call('POST', '/login', raw=b'{"name": ')   # (400 bad_json)
    call('POST', '/login', raw=b'{"name": "' + b'a' * 70000 + b'"}')   # (413)
    call('PUT', '/me/hide_stats', {'hide_stats': True}, who=Who(admin.cookie))   # (403 csrf)
    call('POST', '/logout', who=admin)
    call('GET', '/me', who=admin)   # (401)
    walked = len(fits_2xx)
    check(walked == len(OPS), f'the walk: {walked} of {len(OPS)} operations answered a 2xx that fits' +
          (f'; not {sorted(set(OPS) - fits_2xx)[:4]}' if walked != len(OPS) else ''))
    check(not misfits, f'every answer of the walk fits the description ({answers[0]} answers)' +
          (f': {misfits[0][0]}: {misfits[0][1]}' if misfits else ''))
    serve.terminate()
    serve.wait(10)
    serve = None

    # every answer of test_api.py
    record = os.path.join(T, 'api.jsonl')
    misfits.clear()
    before = answers[0]
    r = subprocess.run([sys.executable, os.path.join(SRC, 'tests', 'eamuse', 'test_api.py'), SERVER, CLIENT],
                       env={**os.environ, 'HK_API_RECORD': record}, capture_output=True, timeout=600)
    check(r.returncode == 0, 'tests/eamuse/test_api.py passes with its answers recorded' +
          ('' if r.returncode == 0 else ': ' + r.stdout.decode(errors='replace').strip().splitlines()[-1:][0] if r.stdout.strip() else ''))
    with open(record, encoding='utf-8') as f:
        for line in f:
            a = json.loads(line)
            judge(a['method'], a['path'], a['status'], a['type'], a['body'], 'test_api')
    check(answers[0] - before > 500 and not misfits, f"every one of test_api.py's {answers[0] - before} answers fits the description" +
          (f' ({len(misfits)} not; first {misfits[0][0]}: {misfits[0][1]})' if misfits else ''))
finally:
    if serve:
        serve.kill()
        serve.wait()
    if log:
        log.close()
    shutil.rmtree(T, ignore_errors=True)

print(f'{failed} failed' if failed else 'all passed')
sys.exit(1 if failed else 0)
