#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""pop'n music 16-18's playerdata set merged into their records where each game's loader reads
them (docs/EAMUSE3.md, "pop'n music 16, 17, 18"): made-up set calls, the kept records checked.

    tests/eamuse/test_pcrecords.py [build/cmake/hiraku-eamuse]      exit 0 when all hold
"""
import os, shutil, socket, struct, subprocess, sys, tempfile, time
SERVER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'build', 'cmake', 'hiraku-eamuse')
D = tempfile.mkdtemp(prefix='hk-pcrec-')
REF = '48524B00000000BB'
open(os.path.join(D, 'cards'), 'w').write(f'cardid=E0040100000000BB\tcardtype=1\tpass=1234\trefid={REF}\n')
with socket.socket() as s0:
    s0.bind(('127.0.0.1', 0)); port = s0.getsockname()[1]
srv = subprocess.Popen([SERVER, '--port', str(port), '--data', D, '--quiet', '--no-relay'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(300):   # up to 30 s: a freshly built server read from a slow disk
    try:
        socket.create_connection(('127.0.0.1', port)).close(); break
    except OSError:
        time.sleep(0.1)
def call(model, body):
    x = f'<?xml version="1.0"?><call model="{model}" srcid="0">{body}</call>'.encode()
    s = socket.create_connection(('127.0.0.1', port))
    s.sendall(b'POST / HTTP/1.0\r\nContent-Length: %d\r\n\r\n' % len(x) + x)
    out = b''
    while (d := s.recv(65536)): out += d
    return out
def rec(game): return open(os.path.join(D, f'pdata-{game}-{REF}'), 'rb').read()
u16 = lambda b, a: struct.unpack_from('<H', b, a)[0]
u32 = lambda b, a: struct.unpack_from('<I', b, a)[0]
fails = 0
def check(what, got, want):
    global fails
    ok = got == want; fails += not ok
    print(('ok  ' if ok else 'FAIL'), what, got, '' if ok else f'(want {want})')
try:
    M16, M17, M18 = 'H16:J:A:A:2008052700', 'I17:J:A:A:2009022500', 'J39:J:A:A:2010040500'
    for model in (M16, M17, M18):
        call(model, f'<playerdata method="new" ref_id="{REF}" name="TEST"></playerdata>')
    # 16: the party's guests and secrets, an extra ticket, an expert course and a random course
    call(M16, f'<playerdata method="set" ref_id="{REF}" mode_num="2" extra_ticket="1" expert_course_num="10" expert_data="3" '
              f'expert_random_course_num="12" expert_random_course_clear="5" expert_random_course_combo="2">'
              f'<private send_invite_type="0" recv_invite_type="1" zoom="0" seed="167" ticket_point="0" secret="1048577" '
              f'guest_count_0="0" guest_count_1="4" guest_count_2="1" guest_count_3="0" guest_count_4="9"/></playerdata>')
    r = rec('H16')
    check('16 guests', list(r[0x6564:0x6569]), [0, 4, 1, 0, 9])
    check('16 secret', u32(r, 0x656c), 1048577)
    check('16 extra ticket, mode', (r[0x26], r[0x1a]), (1, 2))
    check('16 course 10, random level 12', (u16(r, 0x83c + 20), r[0x852 + 12], r[0x87d + 12]), (3, 5, 2))
    # 17: an expert course in EXPERT mode (play_mode 6 = mode 3), its private
    call(M17, f'<playerdata method="set" ref_id="{REF}" play_mode="6" last_play_flag="3" extra_ticket="2" expert_course_num="18" '
              f'expert_data="4" expert_random_course_num="-1">'
              f'<private point="7" target="2" secret="33" ticket_point="1,2,3,4,5" latest_target="5,4,3,2,1" main_chara="10,11,12" '
              f'sub_chara="1,2,3,4,5,6,7,8,9,10" latest_main_0="20,21,22" latest_main_4="40,41,42" '
              f'latest_sub_0="1,1,1,1,1,1,1,1,1,1" latest_sub_4="9,8,7,6,5,4,3,2,1,0"/></playerdata>')
    r = rec('I17')
    check('17 mode, last_play_flag, ticket', (r[0x1a], r[0x1c], r[0x2a]), (3, 3, 2))
    check('17 course 18', u16(r, 0x990 + 36), 4)
    check('17 secret, main, sub', (u32(r, 0xc00), [u16(r, 0xc04 + 2 * i) for i in range(3)], u16(r, 0xc0a + 18)), (33, [10, 11, 12], 10))
    check('17 latest main 0 and 4', ([u16(r, 0xc1e + 2 * i) for i in range(3)], [u16(r, 0xc1e + 24 + 2 * i) for i in range(3)]), ([20, 21, 22], [40, 41, 42]))
    check('17 latest sub 4', [u16(r, 0xc3c + 0x50 + 2 * i) for i in range(10)], [9, 8, 7, 6, 5, 4, 3, 2, 1, 0])
    check('17 point, ticket_point, target, latest_target', (r[0xcba], list(r[0xcbb:0xcc0]), r[0xcc0], list(r[0xcc1:0xcc6])), (7, [1, 2, 3, 4, 5], 2, [5, 4, 3, 2, 1]))
    # 18: NET TAISEN (play_mode 7 = mode 3), a license's two parts, the norma
    call(M18, f'<playerdata method="set" ref_id="{REF}" play_mode="7" last_play_flag="73" norma_point="12" medal_and_friend="0" room_num="0">'
              f'<license license_num="2" sub_license_num="0" clear="1" data1="5" data2="0" data3="0" data4="0"/>'
              f'<license license_num="12" sub_license_num="10" clear="3" data1="1" data2="2" data3="3" data4="4"/></playerdata>')
    r = rec('J39')
    check('18 mode, last_play_flag, norma', (r[0x1a], r[0x1c], r[0x3e]), (3, 73, 12))
    lic = lambda l, s: [u32(r, 0x89c + 0xdc * l + 0x14 * s + 4 * k) for k in range(5)]
    check('18 license 2.0, 12.10', (lic(2, 0), lic(12, 10)), ([1, 5, 0, 0, 0], [3, 1, 2, 3, 4]))
    check('18 license block ends before NET TAISEN\'s', 0x89c + 0xdc * 13, 0x13c8)
    call(M18, f'<playerdata method="set" ref_id="{REF}" play_mode="2"></playerdata>')
    check('18 mode after play_mode 2', rec('J39')[0x1a], 1)
    # the rankings: 17's expert course boards (entry, then get's packed table: 11 boards of 0x1e4),
    # 16's stage_score list, 18's INTERNET RANKING by ir_num
    import re
    ans = call(M17, f'<ranking method="entry" ref_id="{REF}" course_num="3" stage_score_1="90000" stage_score_2="80000" '
                    f'stage_score_3="70000" stage_score_4="60000" cool="900" great="10" good="1" bad="0" combo="1000" pref="13"></ranking>').decode()
    check('17 entry', re.search(r'update="(\d)" entry="(\d+)" rank="(\d+)" hi_score="(\d+)"', ans).groups(), ('1', '1', '1', '300000'))
    ans = call(M17, '<ranking method="get"></ranking>').decode()
    def unlzss(b):
        n = struct.unpack_from('<I', b)[0]; i = 4; out = bytearray(); win = bytearray(4096); r = 0xfee
        while len(out) < n:
            flags = b[i]; i += 1
            for bit in range(8):
                if len(out) >= n: break
                if flags >> bit & 1:
                    c = b[i]; i += 1; out.append(c); win[r] = c; r = (r + 1) & 0xfff
                else:
                    at = b[i] | (b[i + 1] & 0xf0) << 4; ln = (b[i + 1] & 15) + 3; i += 2
                    for k in range(ln):
                        c = win[(at + k) & 0xfff]; out.append(c); win[r] = c; r = (r + 1) & 0xfff
        return bytes(out)
    t = unlzss(bytes.fromhex(re.search(r'<b[^>]*>([0-9a-f]+)</b>', ans).group(1)))
    e = 3 * 0x1e4
    check('17 table: size, course 3 count, name, score', (len(t), u32(t, e), t[e + 4:e + 8], u32(t, e + 4 + 0x10)), (0x14cc, 1, b'TEST', 300000))
    ans = call(M16, f'<ranking method="entry" ref_id="{REF}" course_num="0" stage_score="1,2,3,4" cool="5" great="0" good="0" bad="0" combo="9" pref="1"></ranking>').decode()
    check('16 entry', re.search(r'hi_score="(\d+)"', ans).group(1), '10')
    ans = call(M18, f'<ranking method="entry" ref_id="{REF}" ir_num="2" sheet_num="1" chara_num="5" option="0" score="88000" cool="400" norma_1="0" norma_2="0" pref="13"></ranking>').decode()
    check('18 entry', re.search(r'update="(\d)" entry="(\d+)" rank="(\d+)" hi_score="(\d+)"', ans).groups(), ('1', '1', '1', '88000'))
    ans = call(M18, f'<ranking method="entry" ref_id="{REF}" ir_num="2" sheet_num="1" chara_num="5" option="0" score="70000" cool="400" norma_1="0" norma_2="0" pref="13"></ranking>').decode()
    check('18 entry, a worse one', re.search(r'update="(\d)" entry="(\d+)" rank="(\d+)" hi_score="(\d+)"', ans).groups(), ('0', '1', '1', '88000'))
    # NET TAISEN: what was won and used, the room, the last members (17 0x9bc lower before its
    # titles, 0x9c0 after)
    nv = ('get_ojama_0="462879" get_ojama_1="28" get_item_0="129" get_item_1="0" used_ojama_0="65538" used_ojama_1="4" '
          'used_item_0="129" used_item_1="7" room="3" room_skill="19" room_start="2" room_upgrade="1" record="2" '
          'member_name="AAA,BBBB,CC" member_pref="10,0,47" recommend_ojama="5,6,7" set_ojama="1,16,34"')
    call(M18, f'<playerdata method="set" ref_id="{REF}" play_mode="7"><netvs {nv} get_title_0="268435456"/></playerdata>')
    r = rec('J39')
    check('18 won/used', [u32(r, 0x13c8 + 4 * k) for k in range(8)], [462879, 28, 129, 0, 65538, 4, 129, 7])
    check('18 room, skill, start, upgrade, record', list(r[0x1458:0x145d]), [3, 19, 2, 1, 2])
    check('18 members', (r[0x1470:0x1474], r[0x147d:0x1482], r[0x148a:0x148d], list(r[0x14e5:0x14e8])), (b'AAA\0', b'BBBB\0', b'CC\0', [10, 0, 47]))
    check('18 recommend, set ojama', (list(r[0x15b4:0x15b7]), list(r[0x15b1:0x15b4])), ([5, 6, 7], [1, 16, 34]))
    call(M17, f'<playerdata method="set" ref_id="{REF}" play_mode="7"><netvs {nv} get_title_0="1" get_title_3="9"/></playerdata>')
    r = rec('I17')
    check('17 won/used', [u32(r, 0xa0c + 4 * k) for k in range(8)], [462879, 28, 129, 0, 65538, 4, 129, 7])
    check('17 titles 0, 3', (u32(r, 0xa2c), u32(r, 0xa38)), (1, 9))
    check('17 room, skill, start, upgrade, record', list(r[0xa98:0xa9d]), [3, 19, 2, 1, 2])
    check('17 members', (r[0xab0:0xab4], list(r[0xb25:0xb28])), (b'AAA\0', [10, 0, 47]))
    check('17 recommend, set ojama', (list(r[0xbf4:0xbf7]), list(r[0xbf1:0xbf4])), ([5, 6, 7], [1, 16, 34]))
    # The 64-bit PC pop'n (M39's modules info and player): the common info's events' phases (each at
    # its last unless phases-phc says otherwise); the record's items: every song unless unlock-phc
    # says 0, and those the game wrote
    M39 = 'M39:K:D:A:2026041500'
    phases = lambda a: re.findall(r'<event_id __type="s16">(\d+)</event_id><phase __type="s16">(-?\d+)', a)
    ans = call(M39, '<info method="common" loc_id="X"/>').decode()
    check('M39 phases', phases(ans), [('0', '7'), ('1', '1'), ('2', '3'), ('4', '11'), ('5', '2'), ('6', '1'), ('7', '1')])
    open(os.path.join(D, 'phases-phc'), 'w').write('0=2 4=0\n')
    got = dict(phases(call(M39, '<info method="common" loc_id="X"/>').decode()))
    check('M39 phases set', (got['0'], got['4'], got['7']), ('2', '0', '1'))
    who = f'<ref_id __type="str">{REF}</ref_id>'
    call(M39, f'<player method="new">{who}<name __type="str">TEST</name></player>')
    items = lambda: re.findall(r'<item><type __type="u8">(\d+)</type><id __type="u16">(\d+)</id><param __type="u16">(\d+)',
                               call(M39, f'<player method="read">{who}</player>').decode())
    every = items()
    check('M39 items: every song, its Hyper and EX charts', (len(every), every[2324]), (2374, ('0', '2324', '12')))
    call(M39, f'<player method="write">{who}<item><type __type="u8">1</type><id __type="u16">5</id><param __type="u16">3</param>'
              f'<is_new __type="bool">1</is_new><get_time __type="u64">0</get_time></item></player>')
    open(os.path.join(D, 'unlock-phc'), 'w').write('0\n')
    check('M39 items: the one written', items(), [('1', '5', '3')])
finally:
    srv.terminate()
    shutil.rmtree(D, ignore_errors=True)
print('failures:', fails)
sys.exit(1 if fails else 0)
