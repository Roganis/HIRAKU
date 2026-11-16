#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""hiraku-server's web pages (web/, docs/SERVER.md), served on the API's port: each file with its type
and the security headers (CSP without inline script or style, nosniff, a referrer policy, no-cache);
"..", an absolute path, a dot-file, an unknown file and one outside the folder refused; /api/v1/ still
the API's; --no-web serves none; --web DIR without an index.html refused at start. And the files
themselves, read as text: no URL to another site, no inline <script>, style= or on...= attribute, no
HTML built from text in the scripts; `node --check` on each script when node is there.

    tests/eamuse/test_web.py HIRAKU-SERVER [WEB-FOLDER]      exit 0 when all hold
"""
import http.client, os, re, shutil, socket, subprocess, sys, tempfile, time

SERVER = sys.argv[1]
WEB = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'web')
WEB = os.path.abspath(WEB)
T = tempfile.mkdtemp(prefix='hk-web-')
DB = os.path.join(T, 'server.db')
CSP = "default-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'"
failed = 0


def check(ok, what):
    global failed
    print(('ok    ' if ok else 'FAIL  ') + what, flush=True)
    failed += not ok


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def start(*extra):
    port, api = free_port(), free_port()
    log = open(os.path.join(T, f'serve-{api}.txt'), 'w+')
    p = subprocess.Popen([SERVER, 'serve', '--db', DB, '--port', str(port), '--bind', '127.0.0.1', '--no-relay',
                          '--api', f'127.0.0.1:{api}', '--api-insecure-cookies', *extra], stdout=log, stderr=subprocess.STDOUT)
    for _ in range(300):
        try:
            socket.create_connection(('127.0.0.1', api)).close()
            return p, api, log
        except OSError:
            if p.poll() is not None:
                return p, api, log
            time.sleep(0.1)
    return p, api, log


def stop(p, log):
    if p.poll() is None:
        p.terminate()
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
    log.close()


def fetch(port, path, method='GET'):
    """(status, headers lower-cased, body bytes); the path sent as it is"""
    c = http.client.HTTPConnection('127.0.0.1', port, timeout=20)
    c.putrequest(method, path, skip_accept_encoding=True)
    c.endheaders()
    r = c.getresponse()
    body = r.read()
    hdrs = {k.lower(): v for k, v in r.getheaders()}
    c.close()
    return r.status, hdrs, body


def secure(h):
    return (h.get('content-security-policy') == CSP and h.get('x-content-type-options') == 'nosniff' and
            h.get('referrer-policy') == 'same-origin' and h.get('cache-control') == 'no-cache')


# --- the files, read as text
files = sorted(os.path.join(d, f) for d, _, fs in os.walk(WEB) for f in fs)
check(any(f.endswith('index.html') for f in files) and any(f.endswith('.js') for f in files) and any(f.endswith('.css') for f in files),
      f'web/ has index.html, scripts and a style sheet ({len(files)} files)')
url = re.compile(r'(?:https?:)?//[A-Za-z0-9.-]+\.[A-Za-z]{2,}', re.I)
for path in files:
    name = os.path.relpath(path, WEB)
    text = open(path, encoding='utf-8').read()
    # (an SVG's namespace is a name, not something loaded)
    others = [u for u in url.findall(text.replace('xmlns="http://www.w3.org/2000/svg"', '')) if not u.startswith('//')]
    check(not others, f'{name}: no URL to another site' + (f' ({others[0]})' if others else ''))
    check(not re.search(r'\sstyle\s*=', text, re.I) and "setAttribute('style'" not in text and 'setAttribute("style"' not in text,
          f'{name}: no style= attribute')
    if name.endswith(('.html', '.svg')):
        inline = [m for m in re.finditer(r'<script\b([^>]*)>', text, re.I) if 'src=' not in m.group(1)]
        check(not inline, f'{name}: no inline <script>')
        check(not re.search(r'<[^>]*\son[a-z]+\s*=', text, re.I), f'{name}: no on...= attribute')
        check(not re.search(r'<style\b', text, re.I), f'{name}: no <style> element')
        check(not re.search(r'javascript:', text, re.I), f'{name}: no javascript: URL')
    if name.endswith('.js'):
        bad = [w for w in ('innerHTML', 'outerHTML', 'insertAdjacentHTML', 'document.write', 'eval(', 'new Function', 'localStorage',
                           'sessionStorage', 'setTimeout(\'', 'setTimeout("') if w in text]
        check(not bad, f'{name}: no HTML from text, no eval, no storage' + (f' ({bad[0]})' if bad else ''))
        node = shutil.which('node')
        if node:
            r = subprocess.run([node, '--check', path], capture_output=True)
            check(r.returncode == 0, f'{name}: node --check' + (': ' + r.stderr.decode(errors='replace')[:200] if r.returncode else ''))

serve = None
try:
    # a copy of the folder, with what must never be served beside and inside it: a file outside, a
    # link out of it, a dot-file, a file of a kind not served
    PAGES = os.path.join(T, 'web')
    shutil.copytree(WEB, PAGES)
    with open(os.path.join(T, 'outside.js'), 'w') as f:
        f.write('// not a page\n')
    with open(os.path.join(PAGES, '.secret.js'), 'w') as f:
        f.write('// not a page\n')
    with open(os.path.join(PAGES, 'notes.bak'), 'w') as f:
        f.write('not a page\n')
    os.symlink(os.path.join(T, 'outside.js'), os.path.join(PAGES, 'link.js'))

    # --- served: types and headers
    serve, port, log = start('--web', PAGES)
    TYPES = {'.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8',
             '.svg': 'image/svg+xml'}
    st, h, body = fetch(port, '/')
    check(st == 200 and h.get('content-type') == TYPES['.html'] and b'<html' in body and secure(h), '/: index.html, with the security headers')
    for path in files:
        name = os.path.relpath(path, WEB).replace(os.sep, '/')
        if os.path.basename(name).startswith('.'):
            continue
        st, h, body = fetch(port, '/' + name)
        want = TYPES.get(os.path.splitext(name)[1])
        check(st == 200 and h.get('content-type') == want and body == open(path, 'rb').read() and secure(h),
              f'/{name}: {want}, as it is on disk, with the security headers')
    st, h, _ = fetch(port, '/', 'HEAD')
    check(st == 200 and secure(h), 'HEAD /: the headers')

    # --- refused
    for path, what in (('/../outside.js', '".."'), ('/%2e%2e/outside.js', '"..", escaped'), ('/js/../../outside.js', 'a ".." inside'),
                       ('//etc/passwd', 'an absolute path'), ('/' + PAGES.lstrip('/') + '/app.js', 'the folder as an absolute path'),
                       ('/link.js', 'a link out of the folder'), ('/.secret.js', 'a dot-file'), ('/notes.bak', 'a kind of file not served'),
                       ('/%2fetc%2fpasswd', 'an absolute path, escaped'), ('/.git/config', 'a dot-folder'), ('/.htaccess', 'a dot-file'),
                       ('/no-such-file.js', 'an unknown file'), ('/app.js.bak', 'an unknown kind of file'), ('/index.html%00.js', 'a NUL'),
                       ('/app.js/', 'a file as a folder'), ('/a\\b.js', 'a backslash')):
        st, h, body = fetch(port, path)
        check(st == 404 and b'not a page' not in body and b'root:' not in body and secure(h), f'{path}: {what} refused (404)')

    # --- the API is still the API's
    st, h, body = fetch(port, '/api/v1/server')
    check(st == 200 and h.get('content-type') == 'application/json; charset=utf-8' and body.startswith(b'{'), '/api/v1/server: still JSON')
    st, h, body = fetch(port, '/api/v1/no/such/thing')
    check(st == 404 and h.get('content-type') == 'application/json; charset=utf-8' and b'"error"' in body, '/api/v1/ unknown: the API\'s JSON 404')
    st, h, body = fetch(port, '/api/v1/me')
    check(st == 401 and b'not_signed_in' in body, '/api/v1/me signed out: 401 as before')
    st, h, _ = fetch(port, '/', 'POST')
    check(st in (404, 405) and secure(h), 'POST /: refused, with the headers')
    stop(serve, log)
    log = open(log.name)
    out = log.read()
    log.close()
    check(f'the web pages from {PAGES}' in out, 'serve says the pages\' folder at start')

    # --- none
    serve, port, log = start('--no-web')
    st, _, body = fetch(port, '/')
    check(st == 404 and b'<html' not in body and fetch(port, '/api/v1/server')[0] == 200, '--no-web: no pages, the API still there')
    stop(serve, log)

    # --- a folder without an index.html
    os.makedirs(os.path.join(T, 'nopages'))
    serve, port, log = start('--web', os.path.join(T, 'nopages'))
    try:
        serve.wait(10)
    except subprocess.TimeoutExpired:
        pass
    check(serve.poll() not in (None, 0), '--web without an index.html: serve refuses to start')
    stop(serve, log)
    serve = None
finally:
    if serve and serve.poll() is None:
        serve.kill()
        serve.wait()
    shutil.rmtree(T, ignore_errors=True)

print(f'{failed} failed' if failed else 'all passed')
sys.exit(1 if failed else 0)
