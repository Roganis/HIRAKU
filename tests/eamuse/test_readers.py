#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""hiraku-eamuse's reading side (runtime/eamuse/readers.h, games.h's settings): a server on a new
folder, made-up calls (the transcripts' shapes, tests/eamuse/transcripts/) that make players and
scores in pop'n music 12, 14 and 18, beatmania IIDX EMPRESS and DrumMania (System 573); then
--players, --scores, --settings and --set on that folder, a setting checked by the game's next answer.

    tests/eamuse/test_readers.py [SERVER]      exit 0 when all holds
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_transcripts as tt   # its server on a folder of its own, and the answers in clear

CARD12, CARD14, CARD16, CARD573 = '2004010000000017', 'E004010000000A01', 'E004010000000B01', '2003010000000010'
PINS = ('4321', '1234', '1111', '5678')
failed = 0


def check(ok, what):
    global failed
    print(('ok    ' if ok else 'FAIL  ') + what)
    failed += not ok


def main():
    program = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', '..', 'build', 'cmake', 'hiraku-eamuse')
    server = tt.Server(program, tt.CLOCK)

    def call(model, body):
        data = f'<?xml version="1.0"?><call model="{model}" srcid="{tt.SRCID}">{body}</call>'.encode('latin-1')
        raw = server.exchange(b'POST / HTTP/1.0\r\nHost: cabinet.test:8080\r\nContent-Length: %d\r\n\r\n' % len(data) + data)
        return tt.answer_line(raw, [])

    def tool(*args):
        r = subprocess.run([program, '--data', server.dir, *args], capture_output=True)
        return r.returncode, r.stdout.decode('latin-1')

    try:
        E12, F14, J39, I00, D40 = 'E12:J:A:A:2004111700', 'F14:J:A:A:2006071000', 'J39:J:A:A:2010040500', 'I00:J:A:A:2009072200', 'D40:J:A:A'
        # pop'n 12: a player by card, a credit of two songs
        call(E12, f'<E12 type="playerdata" method="new" card_id="{CARD12}" name="ALICE" password="4321"/>')
        call(E12, f'<E12 type="playerdata" method="set" card_id="{CARD12}"><music num="5" sheet="0" data="1" score="80000"/>'
                  '<music num="20" sheet="3" data="192" score="65000"/></E12>')
        # pop'n 14 and 18: one card's player in both, by its ref id
        call(F14, f'<cardmng method="getrefid" cardtype="1" cardid="{CARD14}" newflag="1" passwd="1234"/>')
        call(F14, '<F14 type="playerdata" method="new" ref_id="48524B0000000001" name="BOB"/>')
        call(F14, '<F14 type="playerdata" method="set" ref_id="48524B0000000001"><music num="5" sheet="1" data="12" score="95000"/></F14>')
        call(J39, '<playerdata method="new" ref_id="48524B0000000001" name="BOB"/>')
        call(J39, '<playerdata method="set" ref_id="48524B0000000001" play_mode="7"><music music_num="900" sheet_num="1" data="4" score="99999"/></playerdata>')
        # beatmania IIDX EMPRESS: a pass, its player, a chart
        call(I00, f'<cardmng method="getrefid" cardtype="1" cardid="{CARD16}" newflag="1" passwd="1111"/>')
        call(I00, '<pc method="reg" rid="48524B0000000002" name="ALPHA" pid="13" did="0"/>')
        call(I00, '<music method="reg" iidxid="10000001" mid="1601" clid="1" pgnum="500" gnum="100" cflg="4" mnum="3"/>')
        # DrumMania (System 573): a card registered, a stage cleared
        call(D40, f'<gamedata method="cardregist"><card id="{CARD573}" irid="0" name="DAVE" pass="5678" type="1" update="0" pazzleno="0" recovery="0"/></gamedata>')
        call(D40, '<gamedata method="gameend"><stagedata><stage no="1" netid="12"/></stagedata><player no="1" gdid="1"><play>'
                  '<stage no="1" seqmode="1" perc="91" clear="1" score="80000" skill="45" combo="120"/></play></player></gamedata>')

        code, out = tool('--players')
        lines = {line.split('\t')[0]: line.split('\t') for line in out.splitlines()}
        check(code == 0 and lines.get(CARD12) == [CARD12, '-', 'popn12', 'ALICE'], 'players: pop\'n 12\'s, by card')
        check(lines.get(CARD14) == [CARD14, '48524B0000000001', 'popn14', 'BOB', 'popn18', 'BOB'], 'players: one card in pop\'n 14 and 18')
        check(lines.get(CARD16) == [CARD16, '48524B0000000002', 'iidx16', 'ALPHA'], 'players: EMPRESS\'s, by the card\'s ref id')
        check(lines.get(CARD573) == [CARD573, '-', 'gfdm573-gf', 'DAVE', 'gfdm573-dm', 'DAVE'], 'players: the System 573 card (both games\' cards are one)')
        check(len(lines) == 4, f'players: four cards ({len(lines)})')
        check(not any(pin in line.split('\t') for line in out.splitlines() for pin in PINS), 'players: no PIN printed')

        def scores(card, game=None):
            return tool('--scores', card, *([game] if game else []))[1].splitlines()
        check(scores(CARD12) == ['popn12\t5\t0\t80000\t1\t-1', 'popn12\t20\t3\t65000\t3\t-1'], 'scores: pop\'n 12, marks and scores')
        check(scores(CARD14) == ['popn14\t5\t1\t95000\t3\t-1', 'popn18\t900\t1\t99999\t1\t-1'], 'scores: pop\'n 14 and 18')
        check(scores(CARD14, 'popn18') == ['popn18\t900\t1\t99999\t1\t-1'], 'scores: one game')
        check(scores(CARD16) == ['iidx16\t1601\t1\t1100\t4\t1'], 'scores: EMPRESS (EX score, clear, plays)')
        check(scores(CARD573, 'gfdm573-dm') == ['gfdm573-dm\t12\t1\t80000\t1\t1'], 'scores: DrumMania')

        code, out = tool('--settings', 'popn14')
        rows = [line.split('\t') for line in out.splitlines()]
        check(code == 0 and [r[:5] for r in rows] == [['popn14', 'phase', '-1', '-1', '-1..9'], ['popn14', 'net_event', '2', '2', '0..3']],
              'settings: pop\'n 14\'s, defaults')
        code, out = tool('--settings')
        names = {(line.split('\t')[0], line.split('\t')[1]) for line in out.splitlines()}
        check(('server', 'message') in names and ('popn15', 'job_sp_rate') in names and ('iidx9', 'courses') in names, 'settings: all')

        def net_event():
            a = call(F14, '<F14 type="game" method="get"/>')
            return a[a.find(' net_event="') + 12:].split('"')[0]
        check(net_event() == '2', 'game get: net_event 2 by default')
        check(tool('--set', 'popn14', 'net_event', '1')[0] == 0 and net_event() == '1', '--set net_event 1: the next answer says 1')
        check(tool('--set', 'popn14', 'net_event', '7')[0] == 2 and net_event() == '1', '--set out of the range: refused, nothing changed')
        check(tool('--set', 'popn14', 'net_event', 'x')[0] == 2 and tool('--set', 'popn14', 'nothing', '1')[0] == 2 and
              tool('--set', 'nogame', 'phase', '1')[0] == 2, '--set: not a number, no such setting, no such game: refused')
        check(tool('--set', 'popn14', 'net_event', '')[0] == 0 and net_event() == '2', '--set "": the default back')
        check(tool('--set', 'server', 'limit-cards', '-5')[0] == 2, '--set server: checked too')
        code, _ = tool('--set', 'server', 'message', 'HELLO')
        check(code == 0 and tool('--settings', 'server')[1].splitlines()[0].split('\t')[:3] == ['server', 'message', 'HELLO'], '--set server message')
    finally:
        server.close()
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
