#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""hiraku-server's accounts, cards, arcades and machines (server/accounts.h), through its commands and
serve: the first admin, sign-up in each mode, what each role may do, card linking by any game's PIN
(the lock after five wrong ones), hidden stats, a reset code, a removed account, the machines
answered, an arcade's shop name and event setting in the answers, and an audit log with no secret
in it. Made-up names, cards and PINs.

    tests/eamuse/test_accounts.py HIRAKU-SERVER
"""
import os, shutil, socket, subprocess, sys, tempfile, time

failed = 0
program = db = None


def check(ok, what):
    global failed
    print(('ok    ' if ok else 'FAIL  ') + what)
    failed += not ok


def run(*args, stdin='', who=None):
    """a command on the database: (status, stdout, stderr)"""
    cmd = [program, *args, '--db', db] + (['--as', who] if who else [])
    done = subprocess.run(cmd, input=stdin.encode(), capture_output=True)
    return done.returncode, done.stdout.decode(errors='replace'), done.stderr.decode(errors='replace')


def ok(*args, stdin='', who=None):
    status, out, err = run(*args, stdin=stdin, who=who)
    if status:
        print(f'      ({" ".join(args)} as {who or "the command line"}: {status} {err.strip()})')
    return status == 0, out


def refused(*args, stdin='', who=None):
    status, out, err = run(*args, stdin=stdin, who=who)
    return status == 1 and err.strip() != '', err


def call(port, srcid, model, body):
    """a call in clear: the whole HTTP answer"""
    data = f'<?xml version="1.0"?><call model="{model}" srcid="{srcid}">{body}</call>'.encode()
    with socket.create_connection(('127.0.0.1', port), timeout=20) as s:
        s.sendall(b'POST / HTTP/1.0\r\nHost: cabinet.test:8080\r\nContent-Length: %d\r\n\r\n' % len(data) + data)
        out = b''
        while True:
            d = s.recv(65536)
            if not d:
                break
            out += d
    return out.decode('latin-1')


CARD_A = '2010010000000A01'   # pop'n 13 on (the cards' table) and pop'n 12 (its record), two PINs
CARD_B = '2010010000000B02'   # DrumMania V's cards
CARD_C = '2010010000000C03'   # the cards' table: the lock
PIN_A13, PIN_A12, PIN_B, PIN_C = '1357', '2468', '8642', '9753'
PASSWORDS = {'bob': 'bob-password-1', 'carol': 'carol-password-1', 'dave': 'dave-password-1', 'op1': 'operator-one-1',
             'op2': 'operator-two-2', 'erin': 'erin-password-1'}


def e12_record(name, pin):
    rec = bytearray(0x708)
    rec[:len(name)] = name.encode()
    for i, d in enumerate(pin):
        rec[0x34 + i] = int(d)
    return bytes(rec)


def main():
    global program, db
    program = sys.argv[1]
    top = tempfile.mkdtemp(prefix='hk-accounts-')
    db = os.path.join(top, 'eamuse.db')
    secrets = list(PASSWORDS.values()) + [PIN_A13, PIN_A12, PIN_B, PIN_C]
    serve = None
    try:
        # 1. the first admin: made with the database, its password shown once
        status, out, _ = run('put', 'cards', stdin=f'cardid={CARD_A}\trefid=R000000000000A01\tpass={PIN_A13}\n'
                                                   f'cardid={CARD_C}\trefid=R000000000000C03\tpass={PIN_C}\n')
        first = [l for l in out.splitlines() if 'first admin' in l]
        admin_pw = first[0].split('password ')[1].split(' ')[0] if first else ''
        check(status == 0 and len(admin_pw) >= 16, 'the first admin made with the database, its password shown')
        secrets.append(admin_pw)
        done = subprocess.run([program, 'put', f'pdata-E12-{CARD_A}', '--db', db], input=e12_record('ALICE', PIN_A12), capture_output=True)
        check(done.returncode == 0 and b'first admin' not in done.stdout, '... shown once only')
        good, out = ok('user', 'login', 'admin', stdin=admin_pw + '\n')
        check(good and out.strip() == 'admin\tadmin\tmust change', 'the first admin logs in, marked "must change"')
        check(refused('user', 'login', 'admin', stdin='not-the-password\n')[0], 'a wrong password refused')
        good, _ = ok('user', 'password', 'admin', stdin=admin_pw + '\nadmin-new-password\n', who='admin')
        good2, out = ok('user', 'login', 'admin', stdin='admin-new-password\n')
        check(good and good2 and out.strip() == 'admin\tadmin\t-', 'the admin changes its password (the old one first)')
        secrets.append('admin-new-password')
        run('put', 'gfdmv-cards', stdin=f'cardid={CARD_B}\tgdid=GD00000001\tname=DAVE\tpass={PIN_B}\n')

        # 2. sign-up in each mode
        check(ok('user', 'signup', 'bob', stdin=PASSWORDS['bob'] + '\n')[0], 'sign-up open: anyone signs up')
        check(refused('user', 'signup', 'bob', stdin='another-password\n')[0], 'a name taken is refused')
        check(refused('user', 'signup', 'shorty', stdin='short\n')[0], 'a short password is refused')
        check(refused('config', 'signup', 'closed', who='bob')[0], 'a player cannot change the sign-up mode')
        check(ok('config', 'signup', 'closed')[0], 'the admin closes sign-up')
        check(refused('user', 'signup', 'carol', stdin=PASSWORDS['carol'] + '\n')[0], 'sign-up closed: refused')
        check(ok('user', 'add', 'carol', stdin=PASSWORDS['carol'] + '\n')[0], '... the admin still makes an account')
        check(ok('config', 'signup', 'invite')[0] and ok('config', 'show')[1].strip() == 'signup\tinvite', 'sign-up by invitation')
        check(refused('user', 'signup', 'dave', stdin=PASSWORDS['dave'] + '\n')[0], '... without a code: refused')
        check(refused('user', 'signup', 'dave', '--invite', stdin='0' * 32 + '\n' + PASSWORDS['dave'] + '\n')[0], '... a wrong code: refused')
        check(refused('config', 'invite', who='carol')[0], '... a player makes no invitation')
        good, code = ok('config', 'invite', '1')
        code = code.strip()
        secrets.append(code)
        check(good and len(code) == 32, 'an invitation for one sign-up')
        check(ok('user', 'signup', 'dave', '--invite', stdin=code + '\n' + PASSWORDS['dave'] + '\n')[0], '... used: dave signs up')
        check(refused('user', 'signup', 'erin', '--invite', stdin=code + '\n' + PASSWORDS['erin'] + '\n')[0], '... used up: refused')
        ok('config', 'signup', 'open')
        for name in ('op1', 'op2', 'erin'):
            ok('user', 'signup', name, stdin=PASSWORDS[name] + '\n')

        # 3. roles
        check(refused('arcade', 'add', 'BOB PLACE', '13', who='bob')[0], 'a player cannot add an arcade')
        check(refused('user', 'role', 'op1', 'operator', who='bob')[0], 'a player cannot give a role')
        check(ok('user', 'role', 'op1', 'operator')[0] and ok('user', 'role', 'op2', 'operator')[0], 'the admin makes two operators')
        good, a1 = ok('arcade', 'add', 'SHOP ONE', '27', who='op1')
        good2, a2 = ok('arcade', 'add', 'SHOP TWO', '1', who='op2')
        a1, a2 = a1.strip(), a2.strip()
        check(good and good2 and a1 != a2, 'each operator adds an arcade')
        check(refused('machine', 'add', a1, 'CAB', who='bob')[0], 'a player cannot add a machine')
        check(refused('machine', 'add', a1, 'CAB', who='op2')[0], "an operator cannot add a machine to another's arcade")
        check(refused('arcade', 'rename', a1, 'MINE NOW', who='op2')[0], "... nor rename another's arcade")
        good, m1 = ok('machine', 'add', a1, 'CAB ONE', who='op1')
        good2, m2 = ok('machine', 'add', a2, 'CAB TWO', who='op2')
        k1, k2 = m1.strip(), m2.strip()   # (a machine's key, hk2.PCBID.SECRET: its PCBID in it)
        m1, m2 = k1.split('.')[1] if k1.count('.') == 2 else '', k2.split('.')[1] if k2.count('.') == 2 else ''
        check(good and good2 and len(m1) == 20 and len(m2) == 20, 'each adds a machine to its own arcade (a new PCBID)')
        _, listed = ok('machine', 'list', who='op1')
        check(m1 in listed and m2 not in listed, "an operator lists its own machines only")
        _, listed = ok('arcade', 'list')
        check(f'{a1}\tSHOP ONE\t27\top1' in listed and f'{a2}\tSHOP TWO\t1\top2' in listed, 'the admin lists every arcade')
        check(ok('arcade', 'rename', a2, 'SHOP 2')[0] and ok('arcade', 'rename', a2, 'SHOP TWO', who='op2')[0], 'the admin edits any arcade')
        _, extra = ok('machine', 'add', a2, 'SPARE')
        secrets += [k1, k2, extra.strip(), k1.split('.')[2]]
        extra = extra.strip().split('.')[1]
        check(ok('machine', 'disable', extra.strip())[0], 'the admin adds and disables a machine of any arcade')

        # 4. cards: a right PIN from two games, five wrong ones, the lock, one card one account
        check(refused('card', 'link', '2010010000000F99', stdin='1111\n', who='bob')[0], 'a card the server never saw: refused')
        check(refused('card', 'link', CARD_A, stdin=PIN_A12 + '\n')[0], 'the command line links no card')
        check(ok('card', 'link', CARD_A.lower(), stdin=PIN_A12 + '\n', who='bob')[0], "a card linked by pop'n 12's PIN (its record)")
        check(ok('card', 'link', CARD_B, stdin=PIN_B + '\n', who='carol')[0], "a card linked by DrumMania V's PIN")
        check(refused('card', 'link', CARD_A, stdin=PIN_A13 + '\n', who='carol')[0], "one card one account: another's is refused")
        for i in range(5):
            refused('card', 'link', CARD_C, stdin='0000\n', who='dave')
        locked, err = refused('card', 'link', CARD_C, stdin=PIN_C + '\n', who='dave')
        check(locked and 'locked' in err, 'five wrong PINs lock the card: the right one is refused')
        check(refused('card', 'unlock', CARD_C, who='op1')[0], 'an operator cannot unlock a card')
        check(ok('card', 'unlock', CARD_C)[0], 'the admin unlocks it')
        check(ok('card', 'link', CARD_C, stdin=PIN_C + '\n', who='dave')[0], "... then the right PIN links it (the cards' table's)")
        _, mine = ok('card', 'list', who='bob')
        check(mine.startswith(CARD_A + '\tbob\t'), "one's own cards listed, as the store writes the number")
        check(refused('card', 'list', 'bob', who='carol')[0], "another's card numbers aren't shown")

        # 5. stats, hidden or not
        _, seen = ok('user', 'stats', 'bob', who='carol')
        check('popn12\t-\tALICE' in seen and CARD_A not in seen, "another's stats shown without the card number")
        _, own = ok('user', 'stats', 'bob', who='bob')
        check(f'popn12\t{CARD_A}\tALICE' in own, '... the owner sees the number')
        check(ok('user', 'hide', 'bob', 'yes', who='bob')[0], 'a player hides its stats')
        check(refused('user', 'stats', 'bob', who='carol')[0], '... then others are refused')
        check(ok('user', 'stats', 'bob')[1].count(CARD_A) == 1, '... an admin still sees them')

        # 6. a reset code, used once
        check(refused('user', 'reset', 'carol', who='op1')[0], 'an operator makes no reset code')
        good, rc = ok('user', 'reset', 'carol')
        rc = rc.strip()
        secrets.append(rc)
        check(good and len(rc) == 32, 'the admin makes a reset code')
        check(ok('user', 'reset-use', 'carol', stdin=rc + '\ncarol-new-password\n')[0], '... carol sets a new password with it')
        secrets.append('carol-new-password')
        check(refused('user', 'reset-use', 'carol', stdin=rc + '\ncarol-third-password\n')[0], '... a second use is refused')
        check(ok('user', 'login', 'carol', stdin='carol-new-password\n')[0], '... the new password works')

        # 7. a PIN reset in every place it is kept
        good, places = ok('card', 'pin', CARD_A, stdin='4321\n')
        check(good and "pop'n music 12" in places and 'the cards' in places, "the admin resets a card's PIN in each place: " +
              ', '.join(l.split('\t')[1] for l in places.splitlines()))
        check(refused('card', 'pin', CARD_A, stdin='4321\n', who='op1')[0], "... an operator cannot")

        # 8. an account removed: its cards unlinked, the games' data kept
        check(ok('user', 'remove', 'bob')[0], 'the admin removes an account')
        check(refused('card', 'list', 'bob')[0] and CARD_A in ok('players')[1], "... its card's players stay")
        check(refused('card', 'link', CARD_A, stdin=PIN_A12 + '\n', who='erin')[0], '... the old PIN no longer links it')
        check(ok('card', 'link', CARD_A, stdin='4321\n', who='erin')[0], '... the new PIN links it to another account')

        # 9. serve: the machines, their arcade's name and settings
        check(refused('arcade', 'set', 'server', 'popn14', 'net_event', '1', who='op1')[0], "an operator cannot set the server's settings")
        check(ok('arcade', 'set', 'server', 'popn14', 'net_event', '1')[0], "the admin sets the server's net_event")
        check(refused('arcade', 'set', a1, 'popn14', 'net_event', '0', who='op2')[0], "an operator cannot set another arcade's")
        check(refused('arcade', 'set', a1, 'popn14', 'net_event', '9', who='op1')[0], '... a value out of range is refused')
        check(ok('arcade', 'set', a1, 'popn14', 'net_event', '3', who='op1')[0], 'an operator sets its arcade\'s net_event')
        _, sets = ok('arcade', 'settings', a1, 'popn14', who='op1')
        check('popn14\tnet_event\t3\t1\t2' in sets, "... listed: the arcade's, the server's, the default")
        with socket.socket() as s:
            s.bind(('127.0.0.1', 0))
            port = s.getsockname()[1]
        serve = subprocess.Popen([program, 'serve', '--db', db, '--port', str(port), '--bind', '127.0.0.1', '--quiet', '--no-relay', '--no-keys'],
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for _ in range(300):
            try:
                socket.create_connection(('127.0.0.1', port)).close()
                break
            except OSError:
                if serve.poll() is not None:
                    raise SystemExit(f'{program} serve ended at start (status {serve.returncode})')
                time.sleep(0.1)
        game = '<F14 type="game" method="get"/>'
        one = call(port, m1, 'F14:J:A:A', game)
        two = call(port, m2, 'F14:J:A:A', game)
        check('net_event="3"' in one, "a machine of the arcade gets the arcade's net_event")
        check('net_event="1"' in two, "another arcade's machine gets the server's")
        fac = call(port, m1, 'F14:J:A:A', '<facility method="get"/>')
        check('<name>SHOP ONE</name>' in fac and '<region>27</region>' in fac, "the arcade's shop name and prefecture in the facility answer")
        check(call(port, '0102030405060708090A', 'F14:J:A:A', game).startswith('HTTP/1.0 403'), 'a PCBID no machine has: refused')
        check(call(port, extra.strip(), 'F14:J:A:A', game).startswith('HTTP/1.0 403'), 'a disabled machine: refused')
        _, listed = ok('machine', 'list', a1)
        check(m1 in listed and listed.strip().split('\t')[-1] != '-', "the machine's last_seen noted")
        serve.terminate()
        log = serve.communicate(timeout=20)[0].decode(errors='replace')
        serve = None

        # 10. the audit log: the actions, no secret
        check(refused('audit', who='op1')[0], 'an operator cannot read the audit log')
        _, audit = ok('audit')
        actions = {l.split('\t')[2] for l in audit.splitlines() if l.count('\t') >= 3}
        want = {'user.add', 'user.password', 'config.signup', 'config.invite', 'user.role', 'arcade.add', 'machine.add', 'card.link',
                'card.link_refused', 'card.unlock', 'user.hide_stats', 'user.reset_code', 'user.reset_used', 'card.pin_reset', 'user.remove',
                'setting.set', 'machine.disable', 'arcade.rename'}
        check(want <= actions, 'the audit log holds the actions' + (f' (missing {sorted(want - actions)})' if want - actions else ''))
        def without_ids(text):   # (a PCBID's hex digits may hold four digits that are a PIN's)
            for pcbid in (m1, m2, extra.strip(), '0102030405060708090A'):
                text = text.replace(pcbid, '')
            return text
        check(not any(s in without_ids(audit) for s in secrets), 'the audit log holds no password, PIN or code')
        check(not any(s in without_ids(log) for s in secrets), "serve's output holds none")
        raw = open(db, 'rb').read() + (open(db + '-wal', 'rb').read() if os.path.exists(db + '-wal') else b'')
        check(not any(s.encode() in raw for s in secrets if len(s) > 4), 'the database holds no password or code in clear')
    finally:
        if serve:
            serve.kill()
            serve.wait()
        shutil.rmtree(top, ignore_errors=True)
    print(f'{failed} failed' if failed else 'all passed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
