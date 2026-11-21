#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The mods' files in a PC game's process (runtime/pc/cabinet.c), under Wine: tests/pc/test_modfiles.c
# with HIRAKU's board DLL beside it, a made-up data folder, two made-up mods and the list hiraku-pc
# would hand the game for them (pc/core.cpp's hand_mods: replaced files, new files, new folders).
#   tests/pc/test_modfiles.sh BUILD_DIR      (BUILD_DIR: with pc/ezusb.dll and pc/test_modfiles.exe
#                                             built: ninja hiraku-pc-test-modfiles)
# WINEPREFIX as you have it (a scratch one is made otherwise). Nothing shows: no window is made.
set -u
B=$(realpath "${1:-build/cmake}")
[ -f "$B/pc/test_modfiles.exe" ] && [ -f "$B/pc/ezusb.dll" ] || { echo "no $B/pc/test_modfiles.exe or pc/ezusb.dll: not built here"; exit 77; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export WINEPREFIX=${WINEPREFIX:-$T/wine} WINEDEBUG=-all
unset DISPLAY WAYLAND_DISPLAY
w() { printf 'Z:%s' "${1//\//\\}"; }   # (Wine's Z: is the root)
put() { mkdir -p "$(dirname "$1")"; printf '%s' "$2" > "$1"; }
put "$T/game/data/a.txt" "game a"
put "$T/game/data/sub/b.txt" "game b"
put "$T/game/data/sub/c.txt" "game c"
put "$T/modA/a.txt" "mod A a"
put "$T/modA/sub/new1.txt" "A new1"
put "$T/modA/newdir/x.txt" "A x"
put "$T/modA/newdir/deep/y.txt" "A y"
put "$T/modA/movie/0001.4" "A m"
put "$T/modB/sub/new1.txt" "B new1"
put "$T/modB/sub/new2.txt" "B new2"
put "$T/modB/newdir/z.txt" "B z"
mkdir -p "$T/prog"
cp "$B/pc/test_modfiles.exe" "$B/pc/ezusb.dll" "$T/prog/"
# the list, as hiraku-pc writes it: the first mod's file where two have one
{
  printf '..\\data\\a.txt\t%s\n' "$(w "$T/modA/a.txt")"
  printf '..\\data\\movie\t%s\tfolder\n' "$(w "$T/modA/movie")"
  printf '..\\data\\movie\\0001.4\t%s\tnew\n' "$(w "$T/modA/movie/0001.4")"
  printf '..\\data\\newdir\t%s\tfolder\n' "$(w "$T/modA/newdir")"
  printf '..\\data\\newdir\\deep\t%s\tfolder\n' "$(w "$T/modA/newdir/deep")"
  printf '..\\data\\newdir\\deep\\y.txt\t%s\tnew\n' "$(w "$T/modA/newdir/deep/y.txt")"
  printf '..\\data\\newdir\\x.txt\t%s\tnew\n' "$(w "$T/modA/newdir/x.txt")"
  printf '..\\data\\newdir\\z.txt\t%s\tnew\n' "$(w "$T/modB/newdir/z.txt")"
  printf '..\\data\\sub\\new1.txt\t%s\tnew\n' "$(w "$T/modA/sub/new1.txt")"
  printf '..\\data\\sub\\new2.txt\t%s\tnew\n' "$(w "$T/modB/sub/new2.txt")"
} > "$T/mods-files.txt"
wineboot -i > /dev/null 2>&1
export HK_PC_MAP="..\\data=$(w "$T/game/data")" HK_PC_LOG=$(w "$T/pc.log")
cd "$T/prog" || exit 1
echo "-- with two mods"
HK_PC_FILES=$(w "$T/mods-files.txt") timeout 120 wine test_modfiles.exe mods > "$T/mods.log" 2>&1
M=$?
grep -E "^(ok|FAIL|all ok|FAILED)" "$T/mods.log"
echo "-- no mods"
timeout 120 wine test_modfiles.exe plain > "$T/plain.log" 2>&1
P=$?
grep -E "^(ok|FAIL|all ok|FAILED)" "$T/plain.log"
echo "-- the DLL's log"
grep "mods" "$T/pc.log"
[ $M -eq 0 ] && [ $P -eq 0 ] && echo "test_modfiles: all passed" || { echo "test_modfiles: FAILED"; cat "$T/mods.log" "$T/pc.log"; exit 1; }
