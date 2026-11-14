#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# A game run in MAME and in HIRAKU on the same input script, compared frame by frame.
#
#   oracle/lockstep.sh GAME SCRIPT [RUN] [-- HIRAKU ARGS...]
#
# MAME runs first (oracle/mame.sh: the 573 and Twinkle cores use the builds at HIRAKU's pin,
# MAME_573 and MAME_TWINKLE, by default mame-pin/k573 and mame-pin/twinkle beside the HIRAKU
# folder; the others the flatpak); HIRAKU then starts from MAME's power-on clock (--rtc, read from MAME's log)
# and runs to the script's quit frame. Prints oracle/snapcmp.py's verdict.
#
#   LOCKSTEP_NVRAM=DIR   both start from a copy of this NVRAM directory (MAME's file names), e.g.
#                        what HIRAKU's run of the game's first boot left
#   LOCKSTEP_NVRAM_OUT=DIR  keep HIRAKU's NVRAM at the end in DIR (for the next step's start)
#   HIRAKU_ROMS          MAME's rompath (default out/oracle-roms: the owner's sets, current names)
#   MAME_ARGS            more MAME options (e.g. "-cassette install"); pass HIRAKU's own after --
#   HIRAKU_HASH_EVERY    MAME hashes RAM on every Nth frame only (default 10: hashing 4 MiB in Lua
#                        every frame was ~96 % of a 573 run). A difference is then pinned to its
#                        frame by a second MAME run hashing every frame around it.
#   LOCKSTEP_RERUN=1     run MAME again even when this script, start NVRAM, MAME build and options
#                        match a run already made (out/oracle/GAME/RUN/lockstep.key)
#
# Twinkle games get --mame-cd (MAME's CD drive timing) and --video off, as regress runs them.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(dirname "$here")"
cd "$root"
game="${1:?usage: oracle/lockstep.sh GAME SCRIPT [RUN] [-- HIRAKU ARGS]}"
script="${2:?usage: oracle/lockstep.sh GAME SCRIPT [RUN] [-- HIRAKU ARGS]}"
shift 2
run="lockstep"
if [[ $# -gt 0 && "$1" != "--" ]]; then run="$1"; shift; fi
[[ "${1:-}" == "--" ]] && shift
toml="games/$game/game.toml"
core="$(python3 -c "import tomllib,sys; print(tomllib.load(open(sys.argv[1],'rb')).get('core',''))" "$toml")"
case "$core" in
    sys573) bin="${MAME_573:-$root/../mame-pin/k573}"; extra=() ;;
    twinkle) bin="${MAME_TWINKLE:-$root/../mame-pin/twinkle}"; extra=(--mame-cd --video off) ;;
    *) bin=""; extra=() ;;
esac
export HIRAKU_ROMS="${HIRAKU_ROMS:-out/oracle-roms}"
quit="$(awk '$1=="at" && $3=="quit" {print $2}' "$script" | tail -1)"
[[ -n "$quit" ]] || { echo "$script: no 'at N quit'" >&2; exit 1; }

m="out/oracle/$game/$run"
h="out/oracle-h/$game-$run"
# HIRAKU's binary as it is now: a rebuild during MAME's half can't change what the run compares
exe="out/oracle-h/.bin-$game-$run-$$"
mkdir -p out/oracle-h
# (no binary yet, e.g. a new game while the build lock is busy: MAME's half alone, kept for reuse)
if [[ -x "build/cmake/hiraku-$game" ]]; then cp "build/cmake/hiraku-$game" "$exe"; else exe=""; fi
trap '[[ -z "$exe" ]] || rm -f "$exe"' EXIT
nv="out/oracle-h/$game-$run-nvram"
rm -rf "$h" "$nv"
mkdir -p "$h" "$nv"
mame_nv=""
if [[ -n "${LOCKSTEP_NVRAM:-}" ]]; then
    mame_nv="out/oracle-h/$game-$run-nvram-mame"
    rm -rf "$mame_nv"; cp -r "$LOCKSTEP_NVRAM" "$mame_nv"
    cp -r "$LOCKSTEP_NVRAM"/. "$nv"/
fi

export HIRAKU_HASH_EVERY="${HIRAKU_HASH_EVERY:-10}"
# MAME's run is deterministic: the same script, start NVRAM, build and options give the same result
key="$( { cat "$script"; [[ -n "${LOCKSTEP_NVRAM:-}" ]] && find "$LOCKSTEP_NVRAM" -type f -print0 | sort -z | xargs -0 cat;
          echo "$bin $(stat -c %Y "$bin" 2>/dev/null) ${MAME_ARGS:-} $HIRAKU_ROMS every=$HIRAKU_HASH_EVERY";
          [[ -n "${HIRAKU_LUA:-}" ]] && cat "$HIRAKU_LUA";   # a probe that changes the machine (oracle/lua/boot-cd.lua)
          # the files the rompath holds for the set, its parents and its BIOS (a relinked zip is a new run)
          python3 - "$toml" "$HIRAKU_ROMS" <<'PY' || true
import os, sys, tomllib
sys.path.insert(0, "tools")
import mameset
name = tomllib.load(open(sys.argv[1], "rb")).get("mame", "")
for n in (mameset.chain(name) if name in mameset.load() else [name]):
    for e in (n + ".zip", n + ".7z", n):
        p = os.path.join(sys.argv[2], e)
        if os.path.exists(p):
            st = os.stat(p)
            print(os.path.realpath(p), st.st_size, int(st.st_mtime))
PY
        } | sha1sum | cut -c1-40)"
if [[ -z "${LOCKSTEP_RERUN:-}" && -f "$m/lockstep.key" && "$(cat "$m/lockstep.key")" == "$key" && -f "$m/frames.txt" ]]; then
    echo "(MAME's $run run is reused: same script, NVRAM, build and options)"
else
    HIRAKU_NVRAM="${mame_nv:+$root/$mame_nv}" MAME_BIN="$bin" HIRAKU_TAPS="${HIRAKU_TAPS:-none}" nice oracle/mame.sh "$game" "$script" "$run" > "$h.mame.txt" 2>&1 || true
    echo "$key" > "$m/lockstep.key"
fi
if [[ -z "$exe" ]]; then echo "=== $game $run: MAME's half only (no build/cmake/hiraku-$game yet)"; exit 0; fi
rtc="$(grep -o 'power-on time [0-9]*' "$m/log.txt" 2>/dev/null | head -1 | awk '{print $NF}' || true)"
args=(--frames $((quit + 1)) --out "$h" --script "$script" --nvram "$nv" "${extra[@]}")
[[ -n "$rtc" ]] && args+=(--rtc "$rtc")
systemd-run --user --scope -q -p MemoryMax=3G nice "$root/$exe" "${args[@]}" "$@" > "$h.log" 2>&1 || true
echo "=== $game $run ($(grep -h '^frames' "$h.log" | cut -c1-80))"
verdict="$(python3 oracle/snapcmp.py "$h" "$m" 2>&1 | grep -E "^hash|^snapshots|Error|error" || true)"
echo "$verdict"
# a hash difference seen on a hashed frame: pin it down, hashing every frame up to it
first="$(echo "$verdict" | grep -o 'first difference at MAME frame [0-9]*' | awk '{print $NF}' | sort -n | head -1 || true)"
if [[ -n "$first" && "$HIRAKU_HASH_EVERY" -gt 1 ]]; then
    lo=$(( first - HIRAKU_HASH_EVERY )); (( lo < 0 )) && lo=0
    pin="$(mktemp -d)"
    { grep -v -E '^at [0-9]+ quit' "$script"; echo "at $((first + 1)) quit"; } > "$pin/pin.his"
    # the same power-on time as the first run (MAME's -rtc, local time as the Lua's os.time reads
    # it): MAME otherwise starts from the host's clock again, and on the 573 and Twinkle a clock a
    # minute later is a different run from its first frames, so the frame it names means nothing
    pin_rtc=()
    [[ -n "$rtc" ]] && pin_rtc=(-rtc "$(date -d "@$rtc" +%Y%m%d%H%M%S)")
    HIRAKU_HASH_EVERY=1000000000 HIRAKU_HASH_FROM=$lo HIRAKU_HASH_TO=$((first + 1)) \
        HIRAKU_NVRAM="${mame_nv:+$root/$mame_nv}" MAME_BIN="$bin" HIRAKU_TAPS="${HIRAKU_TAPS:-none}" \
        MAME_ARGS="${MAME_ARGS:-}${pin_rtc:+ ${pin_rtc[*]}}" \
        nice oracle/mame.sh "$game" "$pin/pin.his" "$run-pin" > "$h.pin.txt" 2>&1 || true
    exact="$(python3 oracle/snapcmp.py "$h" "out/oracle/$game/$run-pin" 2>&1 | grep -o 'first difference at MAME frame [0-9]*' | awk '{print $NF}' | sort -n | head -1 || true)"
    echo "first difference pinned to MAME frame ${exact:-?} (every frame hashed from $lo)"
    rm -rf "$pin"
fi
if [[ -n "${LOCKSTEP_NVRAM_OUT:-}" ]]; then rm -rf "$LOCKSTEP_NVRAM_OUT"; cp -r "$nv" "$LOCKSTEP_NVRAM_OUT"; fi
