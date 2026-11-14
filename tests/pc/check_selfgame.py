#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""What hiraku-pc showed and played of the self-test game (tests/pc/selfgame.c), read back from a
run's snapshots, sound and save folder (tests/pc/test_selfgame.sh runs it):

    check_selfgame.py OUT WAV SAVE AT [LOG]

OUT: the run's snapshots from frame AT - 100 on, one a frame; WAV: its sound; SAVE: its save
folder. The run's script held button 1 from frame AT for 30 frames, put a coin in at AT + 100 and
held button 1 again from AT + 160 for 10. Checked:

  - the game's pictures reach the shell whole and in step: each names its frame (the white square's
    place, the ground's green), and each shell frame shows the next one, never the same twice, never
    one skipped;
  - the pad: the ground is red in the picture after each frame the button was held in, and only
    those; the coin moves the yellow square;
  - the files: the ground is blue (the data on drive D: was read through the path map), and drive
    E:'s file is in the save folder;
  - the sound: silent until the first press, then the game's 441 Hz square at its own level for a
    quarter of a second, from the very frame of the press; again at the second;
  - the lamps and the board: the log says the game started its board and its pictures came."""
import array
import glob
import os
import re
import sys
import wave

failed = 0


def check(ok, what):
    global failed
    print(("ok    " if ok else "FAIL  ") + what)
    failed += not ok


def picture(path):
    b = open(path, "rb").read()
    m = re.match(rb"P6\n(\d+) (\d+)\n255\n", b)
    w, h = int(m.group(1)), int(m.group(2))
    px = b[m.end():]

    def at(x, y):
        return tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3])
    if (w, h) != (160, 120):
        return None   # (not the game's yet: the shell's black while it starts)
    ground = at(1, 1)
    white = [x for x in range(w) if at(x, 55) == (255, 255, 255)]
    yellow = [x for x in range(w) if at(x, 85) == (255, 255, 0)]
    # whole: the squares are squares, and nothing else is on the ground
    whole = (len(white) == min(10, 160 - white[0]) if white else False) and len(yellow) == 10 and at(1, 118) == ground and at(158, 1) == ground
    return {"ground": ground, "white": white[0] if white else None, "yellow": yellow[0] if yellow else None, "whole": whole}


def main():
    out, wav, save, at = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
    log = open(sys.argv[5], errors="replace").read() if len(sys.argv) > 5 else ""
    shots = {}
    for p in glob.glob(os.path.join(out, "frame*.ppm")):
        shots[int(re.search(r"frame(\d+)\.ppm$", p).group(1))] = picture(p)
    frames = sorted(shots)
    check(len(frames) >= 280 and frames[0] <= at - 90, f"the run's snapshots are there ({len(frames)}, from frame {frames[0] if frames else '-'})")
    if not frames:
        return 1
    game = [f for f in frames if shots[f]]
    if not game or game[0] > at - 30:
        check(False, f"the game was drawing by frame {at - 30} (its first picture: {game[0] if game else 'none'}; Wine was slow to start it: "
                     "HK_SELFGAME_AT gives it longer)")
        print(log[-2000:])
        return 1
    check(game == list(range(game[0], frames[-1] + 1)), "once the game draws, every frame shown is the game's")
    check(all(shots[f]["whole"] for f in game), "each picture is whole: the ground, a white square and a yellow one, 160x120")
    # in step: the next frame's picture is the game's next (the square one pixel on, the green two up)
    steps = [(f, shots[f]["white"], shots[f + 1]["white"]) for f in game[:-1]
             if (shots[f + 1]["white"] - shots[f]["white"]) % 150 != 1 or (shots[f + 1]["ground"][1] - shots[f]["ground"][1]) % 256 != 2]
    check(not steps, f"the game's pictures come one a frame, none twice, none skipped{': not at ' + str(steps[:4]) if steps else ''}")
    check(all(shots[f]["ground"][2] == 0x80 for f in game), "the data on drive D: was read (the ground's blue)")
    red = [f for f in game if shots[f]["ground"][0] == 0xff]
    want = list(range(at + 1, at + 31)) + list(range(at + 161, at + 171))
    check(red == want, f"button 1 shows in the picture after each frame it was held in, and in no other ({len(red)} frames"
                       f"{', from ' + str(red[0]) if red else ''}; held from {at} for 30 and from {at + 160} for 10)")
    check(all(shots[f]["ground"][0] == 0x20 for f in game if f not in red), "the ground's red is the game's own otherwise")
    coin = [f for f in game if shots[f]["yellow"] != 0]
    check(coin and coin[0] in (at + 101, at + 102) and all(shots[f]["yellow"] == 4 for f in coin) and coin[-1] == game[-1],
          f"the coin put in at frame {at + 100} is counted once (the yellow square moves{' at ' + str(coin[0]) if coin else ' never'})")

    try:
        text = open(os.path.join(save, "e", "selfgame.txt")).read()
    except OSError:
        text = ""
    check(text.startswith("selfgame: data read, frame 10"), f"drive E:'s file is in the save folder ({text.strip()!r})")

    w = wave.open(wav)
    a = array.array("h")
    a.frombytes(w.readframes(w.getnframes()))
    if sys.byteorder == "big":
        a.byteswap()
    left, right = a[0::2], a[1::2]
    check(w.getframerate() == 44100 and w.getnchannels() == 2 and left == right, "the sound: 44.1 kHz, the two sides alike")
    loud = [i for i, v in enumerate(left) if v]
    per = 735   # samples a frame
    first, second = at * per, (at + 160) * per
    check(bool(loud) and first <= loud[0] <= first + 2 * per, f"silent until the press, the tone from its frame (first sound at frame {loud[0] / per if loud else -1:.2f}, the press at {at})")
    if loud:
        start = loud[0]
        tone = left[start:start + 11025]
        square = all(v == (8000 if k % 100 < 50 else -8000) for k, v in enumerate(tone))
        check(len(tone) == 11025 and square, "the game's own samples: a 441 Hz square at its level, a quarter of a second")
        check(not any(left[start + 11025:second]), "then silence until the second press")
        again = [i for i in loud if i >= second]
        check(bool(again) and second <= again[0] <= second + 2 * per and again[-1] - again[0] == 11024, "and the tone again at the second press, as long")
    for line in ("the game started its board", "the game's first Present", "the pictures go to HIRAKU's window (160x120"):
        check(line in log, f"the log says: {line}")
    print("%d failed" % failed if failed else "all hold")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
