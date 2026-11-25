#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The ee'MALL station's shop (docs/EAMUSE3.md, "ee'MALL"): tools/eemall_catalogue.py on a made-up
table of songs with a made-up key (no game, no station), then hiraku-eamuse serving the folder it
wrote as the station downloads it (the lists' sizes and sums, each file as written, nothing outside
the folder), a song bought (the station's i_set) and pop'n 12 reading it back.

    tests/eamuse/test_eemall_shop.py [build/cmake/hiraku-eamuse]      exit 0 when all hold
"""
import hashlib, os, re, shutil, socket, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', '..', 'build', 'cmake', 'hiraku-eamuse')
TOOL = os.path.join(HERE, '..', '..', 'tools', 'eemall_catalogue.py')
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools'))
import eemall_catalogue as cat   # noqa: E402

KEY = bytes(range(1, 33))   # made up: the station's own is read from its SSRNet.dll
T = tempfile.mkdtemp(prefix='hk-eemall-shop-')
fails = 0


def check(ok, what):
    global fails
    fails += not ok
    print(('ok    ' if ok else 'FAIL  ') + what, flush=True)


def tool(*args):
    return subprocess.run([sys.executable, TOOL, *args], capture_output=True, text=True)


class Server:
    def __init__(self, data):
        with socket.socket() as s0:
            s0.bind(('127.0.0.1', 0))
            self.port = s0.getsockname()[1]
        self.p = subprocess.Popen([SERVER, '--port', str(self.port), '--data', data, '--quiet', '--no-relay'],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(300):
            try:
                socket.create_connection(('127.0.0.1', self.port)).close()
                break
            except OSError:
                time.sleep(0.1)

    def raw(self, request):
        s = socket.create_connection(('127.0.0.1', self.port))
        s.sendall(request)
        out = b''
        while (d := s.recv(65536)):
            out += d
        return out

    def get(self, path):
        """as the station asks (SSRNet.dll: a bare LF after each line): (status, headers, body)"""
        out = self.raw(b'GET %s HTTP/1.0\nContent-Length: 0\n\n' % path.encode())
        head, _, body = out.partition(b'\r\n\r\n')
        return int(head.split()[1]), head.decode('latin-1'), body

    def call(self, model, body):
        x = f'<call model="{model}" srcid="0">{body}</call>'.encode()
        out = self.raw(b'POST / HTTP/1.0\r\nContent-Length: %d\r\n\r\n' % len(x) + x)
        return out.split(b'\r\n\r\n', 1)[-1].decode('cp932', 'replace')

    def close(self):
        self.p.terminate()
        self.p.wait()


try:
    # 1. the tool: a made-up table, a made-up key
    table = os.path.join(T, 'table.txt')
    with open(table, 'w', encoding='utf-8') as f:
        f.write('# family\titem\ttitle\tgames\nD11\t1\tSONG ONE\tpopn12,popn13\nD11\t2\tTWO & "THREE" <4>\tpopn12\n')
    data = os.path.join(T, 'eamuse')
    done = tool('--data', data, '--table', table, '--test-key', KEY.hex(), '--price', '30', '--quiet')
    check(done.returncode == 0, f'the tool writes a shop ({done.stderr.strip()})')
    shop = os.path.join(data, 'eemall-shop')
    c = cat.Cipher(dict(k_in=KEY[0:8], k_r1=KEY[8:12], k_mid=KEY[12:20], k_r2=KEY[20:24], k_out=KEY[24:32]))
    for n in (0, 1, 8, 9, 100):
        plain = os.urandom(n)
        enc = c.encode(plain)
        if not (enc[:4] == b'%eNc' and len(enc) == 8 + 8 * ((n + 7) // 8) and c.decode(enc) == plain):
            check(False, f'the cipher: {n} bytes')
            break
    else:
        check(True, 'the cipher: "%eNc", the length, 8-byte blocks; decode(encode(x)) == x')
    sound = c.decode(open(os.path.join(shop, 'C33/records/sound.xml'), 'rb').read())
    recs = re.findall(rb'<sound ([^>]*)/>', sound)
    attrs = [dict(re.findall(rb'(\w+)="([^"]*)"', r)) for r in recs]
    check(sound.startswith(b'<eamuse-download>') and sound.endswith(b'</eamuse-download>') and len(recs) == 2,
          'sound.xml: <eamuse-download>, a <sound/> a song')
    check([a[b'num'] for a in attrs] == [b'11001', b'11002'] and [a[b'url'] for a in attrs] == [b'1001', b'1002'] and all(a[b'point'] == b'30' for a in attrs),
          "sound.xml: the station's numbers (D11's base + item), the price, each record's files' name (4 characters)")
    check(attrs[1][b'title'].decode('cp932') == 'ＴＷＯ　＆　＂ＴＨＲＥＥ＂　＜４＞' and attrs[1][b'genre'].decode('cp932') == "ｐｏｐ’ｎ　ｍｕｓｉｃ　１２" and \
          attrs[0][b'artist'].decode('cp932') == "ｐｏｐ’ｎ　ｍｕｓｉｃ　１２",
          'sound.xml: titles full-width in Shift-JIS (the station draws no ASCII; no & < > " left), the games that open it, the first as the artist')
    chart = c.decode(open(os.path.join(shop, 'C33/records/1002.004'), 'rb').read())
    check(len(chart) == 16 and chart[5] == 4 and chart[13] == 6, "the DJ chart: a tempo, the end, no notes")
    pre = c.decode(open(os.path.join(shop, 'C33/records/1001.005'), 'rb').read())
    check(len(pre) > 0 and len(pre) % 32 == 0 and not any(pre),
          'no game: the preview in the cipher, stereo SPU ADPCM blocks of silence (as the station streams it)')

    # 2. served as the station downloads it: three lists, each file's size and MD5 as listed
    s = Server(data)
    try:
        st, head, top = s.get('/sum/file.xml')
        check(st == 200 and re.search(r'Content-Length: \d+', head) is not None, 'GET /sum/file.xml: 200 with a Content-Length')
        seen, todo = {}, [('sum/file.xml', top)]
        while todo:
            path, body = todo.pop()
            for p, size, sum_ in re.findall(rb'<list url="[^"]*" path="([^"]*)" size="(\d+)" sum="([0-9a-f]{32})"/>', body):
                st, _, got = s.get('/' + p.decode())
                seen[p.decode()] = st == 200 and len(got) == int(size) and hashlib.md5(got).hexdigest() == sum_.decode() and \
                    got == open(os.path.join(shop, p.decode()), 'rb').read()
                if p.endswith(b'file.xml'):
                    todo.append((p.decode(), got))
        check(len(seen) == 15 and all(seen.values()), f'every listed file served as written, size and MD5 as listed ({len(seen)} files)')
        check(all(s.get(p)[0] == 404 for p in ('/C33/../../users', '/C33//file.xml', '/.hidden', '/C33/nothing.xml', '/eemall-shop/file.xml')),
              'nothing outside the folder, nothing it lacks')
        # 3. a purchase: the station's inventory names D11's song 2 (11002); pop'n 12 opens it
        with open(os.path.join(data, 'users'), 'w') as f:
            f.write('active=1\tcards=2003010000000010\tpass=1234\tuser=hiraku33\n')
        card = '00100000000001032000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
        a = s.call('C33:J:A:E', f'<personaldata method="read" model="C33:J:A:E" card_header="68234101e9" card_data="{card}" ipg="0"/>')
        check('konami_id="hiraku33"' in a and 'record_shop="1"' in a, 'personaldata.read: the KONAMI ID, RECORDS open (record_shop)')
        s.call('C33:J:A:E', '<personaldata method="write" user_id="hiraku33" play_num="1" item_num="1" point="10" ticket_num="1" '
               'card_id="2003010000000010" seq="1"/><personaldata method="i_set" user_id="hiraku33" seq="1" item_num="1" '
               'model0="C00:J:A:A" item_no0="11002"/>')
        a = s.call('E12:J:A:A', '<eemall method="get" card_id="2003010000000010"/>')
        check('now_point="10"' in a and '<item num="2"/>' in a and a.count('<item') == 1, f'pop\'n 12: the song bought, the points ({a})')
    finally:
        s.close()

    # 4. no songs (no pop'n 12 or 13): no shop at all (an empty one ends RECORDS at SYSTEM ERROR)
    empty = os.path.join(T, 'empty.txt')
    open(empty, 'w').close()
    done = tool('--data', data, '--table', empty, '--test-key', KEY.hex(), '--quiet')
    check(done.returncode == 0 and not os.path.exists(shop), 'no songs: the shop removed, none written')
    tool('--data', data, '--table', table, '--test-key', KEY.hex(), '--quiet')

    # 5. no station among the player's games: the shop this tool made is removed
    root = os.path.join(T, 'root')
    os.makedirs(os.path.join(root, 'dumps'))
    done = tool('--data', data, '--root', root, '--quiet')
    check(done.returncode == 0 and not os.path.exists(shop), 'no station installed: the shop removed')
finally:
    shutil.rmtree(T, ignore_errors=True)

print(f'{"FAIL" if fails else "ok"}: {fails} failed')
sys.exit(1 if fails else 0)
