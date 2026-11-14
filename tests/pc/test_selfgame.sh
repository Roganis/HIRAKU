#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# hiraku-pc with a game of HIRAKU's own (tests/pc/selfgame.c) and no game's data: the whole way
# between a PC game and the shell, under Wine. The game loads HIRAKU's board DLL, reads the pad,
# draws with Direct3D 9, plays a sound, and reads and writes files by drive letters; a script
# presses a button and puts a coin in; tests/pc/check_selfgame.py reads back what was shown and
# played.
#
#   tests/pc/test_selfgame.sh BUILD_DIR     BUILD_DIR: with hiraku-pc, its DLLs (pc/) and
#                                           pc/selfgame.exe built (ninja hiraku-pc hiraku-pc-selfgame)
#
# A Linux build's hiraku-pc starts the game under Wine itself; a Windows build made on Linux is
# run under Wine whole (hiraku-pc.exe starts the game as Windows does). Wine draws through X: with
# no DISPLAY the test isn't run (exit 77), nor without HK_PC_SELFGAME=1. HK_SELFGAME_AT=FRAME: when the script's first press
# comes (default 1500, 25 s in: Wine must have the game drawing by then).
set -u
B=$(realpath "${1:-build/cmake}")
here=$(dirname "$(realpath "$0")")
at=${HK_SELFGAME_AT:-1500}
# Asked for, not run with every ctest: on a desktop it would start Wine there (the game's window
# stays hidden, but a new Wine folder says so in a window of its own). The workflow asks.
[ "${HK_PC_SELFGAME:-}" = 1 ] || { echo "not asked for (HK_PC_SELFGAME=1 runs it, where there is a DISPLAY): not run"; exit 77; }
[ -n "${DISPLAY:-}" ] || { echo "no display for the game's Direct3D (Wine draws through X): not run"; exit 77; }
[ -f "$B/pc/selfgame.exe" ] && [ -f "$B/pc/ezusb.dll" ] || { echo "no $B/pc/selfgame.exe or pc/ezusb.dll: not built here"; exit 77; }
T=$B/tests/selfgame-run   # (on a disk: a second of snapshots is megabytes)
rm -rf "$T/root" "$T/out" "$T/save" "$T/run.log" "$T/sound.wav"
game=$T/root/dumps/selfgame
mkdir -p "$game/prog/1" "$game/data" "$T/out"
cp "$B/pc/selfgame.exe" "$game/prog/1/"
: > "$game/prog/1/ezusb.dll"   # (Konami's place: hiraku-pc puts HIRAKU's there in the copy it runs)
printf 'core=pc\nfamily=pcpopn\nexe=selfgame.exe\nprogram=prog/1\ndata=data\ndrive=D:\\selfgame\nsave_drive=E:\\\n' > "$game/machine.txt"
printf hello > "$game/data/hello.txt"
printf 'game selfgame\nat %d press "P1 Button 1" 30\nat %d press "Coin 1" 4\nat %d press "P1 Button 1" 10\n' "$at" $((at + 100)) $((at + 160)) > "$T/press.his"
export WINEDEBUG=${WINEDEBUG:--all}
opts=(--game selfgame --frames $((at + 200)) --snap-every 1 --snap-from $((at - 100)))
if [ -x "$B/hiraku-pc" ]; then
  HIRAKU_ROOT=$T/root timeout 600 "$B/hiraku-pc" "${opts[@]}" --nvram "$T/save" --out "$T/out" --script "$T/press.his" --wav "$T/sound.wav" > "$T/run.log" 2>&1
else
  export WINEPREFIX=${WINEPREFIX:-$T/wine}
  wineboot -i > /dev/null 2>&1
  w() { winepath -w "$1" 2>/dev/null; }
  HIRAKU_ROOT=$(w "$T/root") timeout 600 wine "$B/hiraku-pc.exe" "${opts[@]}" --nvram "$(w "$T/save")" --out "$(w "$T/out")" --script "$(w "$T/press.his")" \
    --wav "$(w "$T/sound.wav")" > "$T/run.log" 2>&1
fi
echo "hiraku-pc ended with $?"
grep -E "^(hiraku-pc|selfgame):" "$T/run.log" | head -12
cat "$T/save/pc.log" >> "$T/run.log" 2>/dev/null
# Wine with no Direct3D at all (a display without OpenGL): nothing of HIRAKU's to judge. Said, and
# not counted as run, unless the build asks for this test (HK_TESTS_REQUIRED: then it fails)
if grep -q "^selfgame: no Direct3D 9" "$T/run.log"; then
  echo "Wine has no Direct3D 9 on this display: not run"
  exit 77
fi
python3 "$here/check_selfgame.py" "$T/out" "$T/sound.wav" "$T/save" "$at" "$T/run.log"
st=$?
rm -rf "$T/out" "$T/root"   # (the save folder and the log stay, to look at)
exit $st
