#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""hiraku-eamuse's answers held to recorded ones, a game family a file (tests/eamuse/transcripts/):
made-up calls in the games' own shapes (docs/EAMUSE3.md), each with the answer the server gave when
the file was recorded, and the data folder's files as the calls left them. Work on the server that
is meant to change nothing (moving its files, another way of keeping its data) changes nothing here.

    tests/eamuse/test_transcripts.py [SERVER] [FILE...]            exit 0 when every answer holds
    tests/eamuse/test_transcripts.py [SERVER] --record [FILE...]   write the answers into the files
    tests/eamuse/test_transcripts.py [SERVER] --full [FILE...]     print each file as this server answers
                                                                   it, long answers whole (to compare by hand)

A transcript's lines:
    # ...            a comment
    clock TIME       the cabinets' time the file's server starts at ("2003-06-01 12:00:00", the
                     default), where a file's answers depend on it
    model M          the model of the calls that follow ("E12:J:A:A:2004111700")
    srcid S          their cabinet (the default: a made-up PCBID)
    file NAME TEXT   a file of the data folder, written before the next call (TEXT and a newline;
                     \\t and \\n as written)
    zeros NAME SIZE [AT=HEX ...]   a made-up record there: SIZE zero bytes, HEX's bytes put at AT
    mask REGEX       from here on: what the regex's group 1 matches in an answer isn't compared
                     (it is recorded as *)
    volatile REGEX   the data folder's files of such names are listed at the end, not compared
    > BODY           a call in clear: <call model=.. srcid=..>BODY</call>
    >e BODY          the same, encrypted as the games encrypt (the answer comes back in kind)
    >x HEX           a call as binary XML (the later PC games' way), its bytes in hex: the line above
                     it says the call in text. The answer, binary too, is recorded as kbin: and hex
    >z BODY          a call packed (X-Compress: lz77). The letters combine: >xze is binary, packed
                     and encrypted
                     In a BODY, {b64 SIZE [AT=HEX ...]} and {hex SIZE [AT=HEX ...]} stand for such a
                     made-up record as base64 or hex digits
    get PATH         a GET (the title message's download)
    < ...            the answer to the call above it: the HTTP status, info= for an encrypted answer's
                     X-Eamuse-Info, compress= for its X-Compress, then the body in clear. A long
                     body is kept as its length, its SHA-256 and its beginning (--full shows them whole)
    = HASH SIZE NAME the data folder's files after the last call (SHA-256)

--record runs each file twice, in new folders, and writes nothing for a file whose two runs differ:
an answer that changes from run to run needs a mask (or the server a fix) first.

The clock: each file has a server of its own, started with --clock, so its time begins at the same
second at every run and then runs on. A file is done in a second or so: the seconds an answer says
(ntp.get's, beatmania IIDX's nowtim) are never compared (CLOCK_MASKS), and a run that took so long
that the minute may have changed is done again.
"""
import base64, hashlib, os, re, shutil, socket, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools', 'eamuse'))
import check as ea   # the protocol's cipher (tools/eamuse/check.py)

SRCID = '0102030405060708090A'
# what an answer says of the server's running clock, to the second
CLOCK_MASKS = [re.compile(r'<time year="\d+" month="\d+" day="\d+" hour="\d+" min="\d+" sec="(\d+)"'),
               re.compile(r'<ntp status="0" time="\d{4}-\d\d-\d\d \d\d:\d\d:(\d\d)\+9"'),
               re.compile(r' nowtim="(\d+)"')]
SLOW = 45     # seconds: a file's run past this is done again (the server's minute may have changed)
LONG = 1500   # an answer's body past this many characters is recorded by its hash
FULL = False
CLOCK = '2003-06-01 12:00:00'


def unescape(text):
    return text.encode('latin-1', 'backslashreplace').decode('unicode_escape')


def escape(data):
    return data.decode('latin-1').encode('unicode_escape').decode('ascii')


def made_up(spec):
    """SIZE [AT=HEX ...]: SIZE zero bytes with HEX's bytes at AT."""
    words = spec.split()
    data = bytearray(int(words[0], 0))
    for w in words[1:]:
        at, _, digits = w.partition('=')
        patch = bytes.fromhex(digits)
        data[int(at, 0):int(at, 0) + len(patch)] = patch
    return bytes(data)


def expand(body):
    return re.sub(r'\{(b64|hex) ([^}]*)\}',
                  lambda m: base64.b64encode(made_up(m.group(2))).decode() if m.group(1) == 'b64' else made_up(m.group(2)).hex(), body)


class Server:
    def __init__(self, program, clock):
        self.dir = tempfile.mkdtemp(prefix='hk-transcript-')
        with socket.socket() as s:
            s.bind(('127.0.0.1', 0))
            self.port = s.getsockname()[1]
        self.proc = subprocess.Popen([program, '--port', str(self.port), '--bind', '127.0.0.1', '--data', self.dir, '--quiet',
                                      '--no-relay', '--clock', clock],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(300):   # up to 30 s: a freshly built server read from a slow disk
            try:
                socket.create_connection(('127.0.0.1', self.port)).close()
                return
            except OSError:
                if self.proc.poll() is not None:
                    shutil.rmtree(self.dir, ignore_errors=True)
                    raise SystemExit(f'{program} ended at start (status {self.proc.returncode})')
                time.sleep(0.1)
        self.close()
        raise SystemExit(f'{program} takes no connection')

    def exchange(self, request):
        with socket.create_connection(('127.0.0.1', self.port), timeout=20) as s:
            s.sendall(request)
            out = b''
            while True:
                d = s.recv(65536)
                if not d:
                    break
                out += d
        return out

    def close(self):
        self.proc.terminate()
        self.proc.wait()
        shutil.rmtree(self.dir, ignore_errors=True)


def answer_line(raw, masks):
    """An HTTP answer as a transcript's line (without its "< ")."""
    head, _, body = raw.partition(b'\r\n\r\n')
    lines = head.decode('latin-1').split('\r\n')
    parts = [lines[0].split(' ')[1] if ' ' in lines[0] else '???']
    headers = {k.strip().lower(): v.strip() for k, _, v in (line.partition(':') for line in lines[1:])}
    if 'x-eamuse-info' in headers:
        parts.append('info=' + headers['x-eamuse-info'])
        body = ea.rc4(ea.key_for(headers['x-eamuse-info']), body)
    if 'x-compress' in headers:
        parts.append('compress=' + headers['x-compress'])
    text = 'kbin:' + body.hex() if body[:1] == b'\xa0' else escape(body)
    for m in CLOCK_MASKS + masks:   # group 1 made *
        text = m.sub(lambda g: g.group(0)[:g.start(1) - g.start(0)] + '*' + g.group(0)[g.end(1) - g.start(0):] if g.groups() and g.group(1) else g.group(0), text)
    if len(text) > LONG and not FULL:
        text = f'long:{len(text)} sha256:{hashlib.sha256(text.encode("ascii")).hexdigest()} {text[:200]}...'
    if text:
        parts.append(text)
    return ' '.join(parts)


class Slow(Exception):
    pass


def run(program, path):
    """The transcript's lines with the answers and the folder's state as this server gives them."""
    for _ in range(3):
        try:
            return run_once(program, path)
        except Slow:
            pass
    raise SystemExit(f'{path}: three runs each took over {SLOW} s: too slow a machine to hold the server\'s clock')


def run_once(program, path):
    out, masks, volatile = [], [], []
    model, srcid, serial = 'C00:J:A:A', SRCID, 0
    with open(path, encoding='ascii') as f:
        lines = [line.rstrip('\n') for line in f]
    clock = next((line.partition(' ')[2].strip() for line in lines if line.startswith('clock ')), CLOCK)
    began = time.monotonic()
    server = Server(program, clock)
    try:
        for line in lines:
            if line.startswith('<') or line.startswith('='):
                continue   # what a run before this one answered
            out.append(line)
            word, _, rest = line.partition(' ')
            if not line or word.startswith('#'):
                continue
            if word == 'clock':
                pass   # (read before the server started)
            elif word == 'model':
                model = rest.strip()
            elif word == 'srcid':
                srcid = rest.strip()
            elif word == 'file':
                name, _, text = rest.partition(' ')
                with open(os.path.join(server.dir, name), 'wb') as f:
                    f.write(unescape(text).encode('latin-1') + b'\n')
            elif word == 'zeros':
                name, _, spec = rest.partition(' ')
                with open(os.path.join(server.dir, name), 'wb') as f:
                    f.write(made_up(spec))
            elif word == 'mask':
                masks.append(re.compile(rest))
            elif word == 'volatile':
                volatile.append(re.compile(rest))
            elif word == 'get':
                out.append('< ' + answer_line(server.exchange(f'GET {rest.strip()} HTTP/1.0\r\n\r\n'.encode()), masks))
            elif word[0] == '>' and set(word[1:]) <= set('exz'):
                if 'x' in word:
                    body = bytes.fromhex(rest.strip())
                else:
                    body = f'<?xml version="1.0"?><call model="{model}" srcid="{srcid}">{unescape(expand(rest))}</call>'.encode('latin-1')
                info = b''
                if 'z' in word:   # packed with nothing found to repeat: a flag byte of eight literals, then the eight
                    body = b''.join(b'\xff' + body[i:i + 8] for i in range(0, len(body), 8))
                    info = b'X-Compress: lz77\r\n'
                if 'e' in word:
                    serial += 1
                    tag = '1-%08x-%04x' % (0x5eed0000 + serial, serial)
                    body = ea.rc4(ea.key_for(tag), body)
                    info += b'X-Eamuse-Info: ' + tag.encode() + b'\r\n'
                raw = server.exchange(b'POST / HTTP/1.0\r\nHost: cabinet.test:8080\r\n' + info + b'Content-Length: %d\r\n\r\n' % len(body) + body)
                out.append('< ' + answer_line(raw, masks))
            else:
                raise SystemExit(f'{path}: not a transcript line: {line[:60]}')
        if time.monotonic() - began > SLOW:
            raise Slow()
        for name in sorted(os.listdir(server.dir)):
            if name == '.lock':
                continue
            with open(os.path.join(server.dir, name), 'rb') as f:
                data = f.read()
            # (a file kept aside is named after the host's clock: NAME.unread-DATE)
            shown = re.sub(r'\.unread-\d{8}-\d{6}$', '.unread-DATE', name)
            if any(v.search(name) for v in volatile):
                out.append(f'= {"-" * 64} - {shown}')
            else:
                out.append(f'= {hashlib.sha256(data).hexdigest()} {len(data)} {shown}')
    finally:
        server.close()
    return out


def differences(a, b, most=8):
    """Where two transcripts' lines part, for a person to read."""
    said = []
    for n, (x, y) in enumerate(zip(a, b)):
        if x == y:
            continue
        at = next((i for i, (p, q) in enumerate(zip(x, y)) if p != q), min(len(x), len(y)))
        call = next((a[k] for k in range(n - 1, -1, -1) if a[k].startswith(('>', 'get '))), '') if x.startswith('<') else ''
        said.append(f'  line {n + 1}' + (f', after {call[:100]}' if call else '') +
                    f'\n    recorded: ...{x[max(0, at - 60):at + 100]}\n    now:      ...{y[max(0, at - 60):at + 100]}')
        if len(said) >= most:
            said.append('  ...')
            break
    if len(a) != len(b):
        said.append(f'  {len(a)} lines recorded, {len(b)} now')
    return '\n'.join(said)


def main():
    global FULL
    args = [a for a in sys.argv[1:] if a not in ('--record', '--full')]
    record, FULL = '--record' in sys.argv[1:], '--full' in sys.argv[1:]
    program = args[0] if args and not args[0].endswith('.txt') else os.path.join(HERE, '..', '..', 'build', 'cmake', 'hiraku-eamuse')
    files = [a for a in args if a.endswith('.txt')]
    if not files:
        folder = os.path.join(HERE, 'transcripts')
        files = sorted(os.path.join(folder, n) for n in os.listdir(folder) if n.endswith('.txt'))
    fails = 0
    for path in files:
        name = os.path.basename(path)
        with open(path, encoding='ascii') as f:
            recorded = [line.rstrip('\n') for line in f]
        now = run(program, path)
        calls = sum(1 for line in now if line.startswith(('>', 'get ')))
        if FULL:
            print('\n'.join(now))
            continue
        if record:
            again = run(program, path)
            if now != again:
                fails += 1
                print(f'FAIL {name}: two runs differ, nothing written (a mask, a volatile, or a fix)\n{differences(now, again)}')
                continue
            if now != recorded:
                with open(path, 'w', encoding='ascii') as f:
                    f.write('\n'.join(now) + '\n')
            print(f'{"recorded" if now != recorded else "the same"} {name}: {calls} calls')
        elif now != recorded:
            fails += 1
            print(f'FAIL {name}\n{differences(recorded, now)}')
        else:
            print(f'ok   {name}: {calls} calls')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
