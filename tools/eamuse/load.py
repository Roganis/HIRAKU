#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""hiraku-server under load (docs/SERVER.md, "Load"): many made-up cabinets at once, each playing
credits as fast as the server answers, against a `serve --no-keys` on 127.0.0.1 of its own.

    tools/eamuse/load.py HIRAKU-SERVER [--cabinets N] [--seconds S] [--dir DIR] [--workers W]
                         [--no-backup] [--keep] [--quiet]

The cabinets are registered as machines of one made-up arcade (`machine add`) and call by their
PCBID (--no-keys: the keyed transport has no Python client). Each plays one game, in turn among
three, with the calls' shapes of tests/eamuse/transcripts/: pop'n music 12 (popn12-14.txt), pop'n
music 15 (popn15-18.txt) and beatmania IIDX 16 (iidx-13-16.txt). A credit: the card read
(cardmng), the player got (made, with a PIN, for a new card), the credit's set with two songs'
scores, a ranking entry. A third of the credits bring a new card, the rest a card seen before.
No wait between calls: what is measured is how much the server takes, not how a cabinet paces.

While it runs, a process of its own asks GET /api/v1/health twice a second, and at half time one
`backup --dir` runs (--no-backup: none). At the end it prints the calls a second, the calls'
latency (p50, p95, p99, max), errors (an HTTP status other than 200, an answer without the status
the call should have, a connection reset or refused: serve takes 64 connections at once, and closes
any more at once), the health checks, the backup's time, and the database's size (with its WAL). A
cabinet whose call failed waits a second, as a game would, and starts its credit again.

The machines are registered in a temporary folder (the system's, often in memory: one `machine
add` each) and the database then moved to DIR, where serve keeps it.

DIR (default: a new folder in the current one) holds the database, the backups and serve's log; it
is removed at the end unless --keep. Exit 0 when nothing failed.
"""
import argparse, http.client, multiprocessing, os, random, re, shutil, signal, socket, subprocess, sys, tempfile, threading, time

ARCADE = 'Made-up load arcade'


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def run(server, *args):
    r = subprocess.run([server, *args], capture_output=True, timeout=120)
    if r.returncode:
        raise SystemExit(f'hiraku-server {" ".join(args[:2])}: status {r.returncode}: {r.stderr.decode(errors="replace").strip()}')
    return r.stdout.decode()


class Failed(Exception):
    pass


class Cabinet:
    """One cabinet's calls: HTTP/1.0, a connection a call, as the games make them."""

    def __init__(self, port, pcbid, model, lat):
        self.port, self.pcbid, self.model, self.lat = port, pcbid, model, lat

    def call(self, body, want=r' status="0"'):
        xml = f'<?xml version="1.0"?><call model="{self.model}" srcid="{self.pcbid}">{body}</call>'.encode()
        req = b'POST / HTTP/1.0\r\nHost: cabinet.test\r\nContent-Length: %d\r\n\r\n' % len(xml) + xml
        began = time.perf_counter()
        with socket.create_connection(('127.0.0.1', self.port), timeout=30) as s:
            s.sendall(req)
            out = bytearray()
            while True:
                d = s.recv(65536)
                if not d:
                    break
                out += d
        self.lat.append(time.perf_counter() - began)
        head, _, answer = bytes(out).partition(b'\r\n\r\n')
        if not head.startswith(b'HTTP/1.') or head.split(b' ', 2)[1] != b'200':
            raise Failed(f'HTTP {head[:40]!r}')
        text = answer.decode('latin-1')
        if want and not re.search(want, text):
            raise Failed(f'{body[:60]}: {text[:120]}')
        return text


def attr(text, name):
    m = re.search(rf' {name}="([^"]*)"', text)
    if not m:
        raise Failed(f'no {name} in {text[:120]}')
    return m.group(1)


# The three games: a credit each, with a card new or seen before (state: what the cabinet knows of it)
def popn12(c, card, state, rng):
    c.call(f'<cardmng method="inquire" cardtype="1" cardid="{card}" update="0"/>', want=r'<cardmng status="(0|112)"')
    got = c.call(f'<E12 type="playerdata" method="get" card_id="{card}"/>', want=r'<E12 status="(0|109)"')
    if 'status="109"' in got:
        c.call(f'<E12 type="playerdata" method="new" card_id="{card}" name="LOAD{rng.randrange(1000):03d}" password="{state["pin"]}"/>')
    songs = ''.join(f'<music num="{rng.randrange(1, 300)}" sheet="{rng.randrange(4)}" data="{rng.choice((1, 3, 192))}" score="{rng.randrange(30000, 100001)}"/>'
                    for _ in range(2))
    c.call(f'<E12 type="playerdata" method="set" card_id="{card}" option="5" chara_num="12">{songs}</E12>')
    c.call(f'<E12 type="rank" method="exp" card_id="{card}" name="4c4f4144" course_num="{rng.randrange(1, 6)}" option="0" '
           f'score="{rng.randrange(1000, 9000)}" cool="50" great="5" good="3" bad="1" combo="60" shop_name="LOADSHOP" pref="13"/>')


def popn15(c, card, state, rng):
    got = c.call(f'<cardmng method="inquire" cardtype="1" cardid="{card}" update="0"/>')
    ref = attr(got, 'refid')
    # (no player yet: a new card, or one whose credit was cut before its player was made)
    if 'status="109"' in c.call(f'<G15 method="playerdata_get" ref_id="{ref}"/>', want=r'<G15 status="(0|109)"'):
        c.call(f'<G15 method="playerdata_new" ref_id="{ref}" name="LOAD{rng.randrange(1000):03d}" ea_passwd="{state["pin"]}"/>')
    else:
        c.call(f'<cardmng method="authpass" refid="{ref}" pass="{state["pin"]}"/>')
    songs = ''.join(f'<music music_num="{rng.randrange(1, 400)}" sheet_num="{rng.randrange(4)}" data="12" score="{rng.randrange(30000, 100001)}"/>'
                    for _ in range(2))
    c.call(f'<G15 method="playerdata_set" ref_id="{ref}" option="5" chara_num="12" mode_num="2">{songs}</G15>')
    c.call(f'<G15 method="rank_exp-entry" ref_id="{ref}" course_num="{rng.randrange(1, 6)}" stage_score="1000,2000,3000,4000" '
           f'cool="50" great="5" good="3" bad="1" combo="60" pref="13"/>')


def iidx16(c, card, state, rng):
    got = c.call(f'<cardmng method="inquire" cardtype="1" cardid="{card}" update="0"/>', want=r'<cardmng status="(0|112)"')
    if 'status="112"' in got:
        ref = attr(c.call(f'<cardmng method="getrefid" cardtype="1" cardid="{card}" newflag="1" passwd="{state["pin"]}"/>'), 'refid')
    else:
        ref = attr(got, 'refid')
        c.call(f'<cardmng method="authpass" refid="{ref}" pass="{state["pin"]}"/>')
    got = c.call(f'<pc method="get" rid="{ref}"/>', want=r'<pc status="[01]"')
    if 'status="1"' in got:
        iidxid = attr(c.call(f'<pc method="reg" rid="{ref}" name="LOAD" pid="13" did="0"/>'), 'id')
    else:
        iidxid = attr(got, 'id')
    c.call(f'<pc method="save" iidxid="{iidxid}" cltype="0" achi="{rng.randrange(2000)}" pnum="1" gno="3" mode="1"/>')
    for _ in range(2):
        c.call(f'<music method="reg" iidxid="{iidxid}" mid="{rng.randrange(1000, 1700)}" clid="{rng.randrange(6)}" '
               f'pgnum="{rng.randrange(1000)}" gnum="{rng.randrange(500)}" cflg="{rng.randrange(1, 8)}" mnum="3"/>')
    c.call(f'<ranking method="entry" iidxid="{iidxid}" coid="{rng.randrange(1, 8)}" clid="2" pgnum="{rng.randrange(800)}" '
           f'gnum="{rng.randrange(400)}" clr="1" cstage="4" opt="0"/>')


GAMES = [('E12:J:A:A:2004111700', popn12), ('G15:J:A:A:2007091800', popn15), ('I00:J:A:A:2009072200', iidx16)]


def worker(port, cabinets, until, out):
    """One process: its cabinets (index, PCBID) as threads, until the time. Puts (latencies, credits, errors, said)"""
    lat, lock = [], threading.Lock()
    totals = {'credits': 0, 'errors': 0, 'resets': 0, 'said': []}

    def cabinet(index, pcbid):
        rng = random.Random(index)
        model, play = GAMES[index % len(GAMES)]
        mine = []
        c = Cabinet(port, pcbid, model, lat)
        cards, made = [], 0
        try:
            c.call('<services method="get"/>')   # a cabinet's start
        except (OSError, Failed) as e:
            with lock:
                totals['errors'] += 1
                totals['resets'] += isinstance(e, OSError) or str(e) == "HTTP b''"
        while time.time() < until:
            if not cards or rng.random() < 1 / 3:
                made += 1
                cards.append((f'E004{index:04X}{made:08X}', {'pin': f'{rng.randrange(10000):04d}'}))
                card, state = cards[-1]
            else:
                card, state = rng.choice(cards)
            try:
                play(c, card, state, rng)
                mine.append(1)
            except (OSError, Failed) as e:
                with lock:
                    totals['errors'] += 1
                    cut = isinstance(e, OSError) or str(e) == "HTTP b''"
                    totals['resets'] += cut
                    if not cut and len(totals['said']) < 5:   # (the connection limit's are counted, not shown)
                        totals['said'].append(f'{pcbid} {model[:3]} {card}: {e}')
                time.sleep(1)   # (a cabinet tries again later, not at once)
        with lock:
            totals['credits'] += len(mine)

    threads = [threading.Thread(target=cabinet, args=cab, daemon=True) for cab in cabinets]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    out.put((lat, totals['credits'], totals['errors'], totals['resets'], totals['said']))


def health_poller(api_port, until, out):
    ok = bad = 0
    worst = 0.0
    while time.time() < until:
        began = time.perf_counter()
        try:
            c = http.client.HTTPConnection('127.0.0.1', api_port, timeout=10)
            c.request('GET', '/api/v1/health')
            r = c.getresponse()
            r.read()
            c.close()
            ok, bad = (ok + 1, bad) if r.status == 200 else (ok, bad + 1)
        except OSError:
            bad += 1
        worst = max(worst, time.perf_counter() - began)
        time.sleep(0.5)
    out.put((ok, bad, worst))


def pct(sorted_values, p):
    if not sorted_values:
        return 0.0
    return sorted_values[min(len(sorted_values) - 1, int(p / 100 * len(sorted_values)))]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('server')
    ap.add_argument('--cabinets', type=int, default=20)
    ap.add_argument('--seconds', type=float, default=30)
    ap.add_argument('--dir')
    ap.add_argument('--workers', type=int, default=0, help='client processes (default: the CPUs, at most the cabinets)')
    ap.add_argument('--no-backup', action='store_true')
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--quiet', action='store_true', help='only the result line')
    a = ap.parse_args()
    server = os.path.abspath(a.server)
    folder = os.path.abspath(a.dir) if a.dir else tempfile.mkdtemp(prefix='hk-load-', dir='.')
    os.makedirs(folder, exist_ok=True)
    db = os.path.join(folder, 'load.db')
    say = (lambda *x: None) if a.quiet else (lambda *x: print(*x, flush=True))
    proc = None
    try:
        began = time.monotonic()
        pcbids = []
        with tempfile.TemporaryDirectory(prefix='hk-load-') as t:
            made = os.path.join(t, 'load.db')
            os.makedirs(os.path.join(t, 'empty'))
            run(server, 'import', os.path.join(t, 'empty'), '--db', made)
            arcade = run(server, 'arcade', 'add', ARCADE, '13', '--db', made).split()[0]
            for i in range(a.cabinets):
                key = run(server, 'machine', 'add', arcade, f'Load cabinet {i + 1}', '--db', made).split()[0]
                pcbids.append(key.split('.')[1])   # hk2.PCBID.SECRET
            for ext in ('', '-wal', '-shm'):
                if os.path.exists(made + ext):
                    shutil.move(made + ext, db + ext)
        say(f'load: {a.cabinets} machines registered in {time.monotonic() - began:.1f} s')
        port, api_port = free_port(), free_port()
        log = open(os.path.join(folder, 'serve.log'), 'w')
        proc = subprocess.Popen([server, 'serve', '--db', db, '--port', str(port), '--bind', '127.0.0.1', '--api', f'127.0.0.1:{api_port}',
                                 '--no-keys', '--no-relay', '--no-web', '--quiet'], stdout=log, stderr=subprocess.STDOUT)
        for _ in range(300):
            try:
                socket.create_connection(('127.0.0.1', port)).close()
                socket.create_connection(('127.0.0.1', api_port)).close()
                break
            except OSError:
                if proc.poll() is not None:
                    raise SystemExit(f'serve ended at start (status {proc.returncode}); its log: {os.path.join(folder, "serve.log")}')
                time.sleep(0.1)
        workers = max(1, min(a.cabinets, a.workers or os.cpu_count() or 4))
        start = time.time() + 0.5
        until = start + a.seconds
        q = multiprocessing.Queue()
        groups = [[(i, pcbids[i]) for i in range(w, a.cabinets, workers)] for w in range(workers)]
        procs = [multiprocessing.Process(target=worker, args=(port, g, until, q)) for g in groups]
        hq = multiprocessing.Queue()
        hp = multiprocessing.Process(target=health_poller, args=(api_port, until, hq))
        for p in procs + [hp]:
            p.start()
        say(f'load: {a.cabinets} cabinets in {workers} processes for {a.seconds:g} s')
        backup = None
        if not a.no_backup:
            time.sleep(max(0.0, start + a.seconds / 2 - time.time()))
            b0 = time.monotonic()
            r = subprocess.run([server, 'backup', '--dir', os.path.join(folder, 'backups'), '--keep', '2', '--db', db], capture_output=True, timeout=300)
            backup = (r.returncode, time.monotonic() - b0)
        results = [q.get() for _ in procs]
        h_ok, h_bad, h_worst = hq.get()
        for p in procs + [hp]:
            p.join()
        took = max(1e-9, time.time() - start)
        lat = sorted(x for r in results for x in r[0])
        credits = sum(r[1] for r in results)
        errors = sum(r[2] for r in results)
        resets = sum(r[3] for r in results)
        said = [s for r in results for s in r[4]]
        size = sum(os.path.getsize(db + ext) for ext in ('', '-wal') if os.path.exists(db + ext))
        ms = lambda x: f'{x * 1000:.1f}'
        print(f'load: {a.cabinets} cabinets, {a.seconds:g} s: {len(lat)} calls ({len(lat) / took:.0f} a second), {credits} credits; '
              f'latency ms p50 {ms(pct(lat, 50))} p95 {ms(pct(lat, 95))} p99 {ms(pct(lat, 99))} max {ms(lat[-1] if lat else 0)}; '
              f'errors {errors}' + (f' ({resets} connections reset, refused or closed without an answer)' if resets else '') + f'; health {h_ok} ok {h_bad} failed (slowest {ms(h_worst)} ms); '
              + (f'backup {"ok" if backup[0] == 0 else "FAILED"} in {backup[1]:.2f} s; ' if backup else '')
              + f'database {size / 1e6:.1f} MB', flush=True)
        for s in said[:5]:
            print('  ' + s)
        failed = errors or h_bad or not lat or (backup and backup[0])
    finally:
        if proc and proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(30)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        if proc and proc.returncode not in (0, None) and not a.quiet:
            print(f'load: serve ended with {proc.returncode}')
        if not a.keep:
            shutil.rmtree(folder, ignore_errors=True)
    return 1 if failed or (proc and proc.returncode != 0) else 0


if __name__ == '__main__':
    sys.exit(main())
