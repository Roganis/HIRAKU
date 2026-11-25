#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# A package tried without any game data: does what was packed start at all? A library left out, a
# program linked against something the package hasn't, a tool that no longer runs with the
# package's own Python each pass every unit test (those run from the build's folder) and show
# first on a player's PC. Run on every push and every release, on the package as it will be
# downloaded: the Linux one in a bare Debian 12 container, the Windows one under Wine.
#
#   tools/smoke.sh ROOT             ROOT: an unpacked Linux package (its folder hiraku)
#   tools/smoke.sh ROOT wine        ... a Windows one, its programs run through Wine
#   tools/smoke.sh --debian FILE    the Linux package FILE (.tar.xz), unpacked and tried in a bare
#                                   Debian 12 container (docker) that has only what the package's
#                                   README asks of a system
#
# Checked: every program in bin answers --help (the shell, each core, the server, the tools); the
# game menu runs a list by a script to its choice (the shell's whole loop, no window, no data), and
# finds the package's language catalogues (lang/: HK_LANG=fr);
# the package's Python runs each tool's --help; and on Linux the launcher draws its main window
# into a picture (Qt's offscreen platform, which the package carries).
set -u
if [ "${1:-}" = --debian ]; then
  archive=$(realpath "${2:?the package, a .tar.xz}")
  self=$(realpath "$0")
  work=$(mktemp -d)
  trap 'rm -rf "$work"' EXIT
  tar -xJf "$archive" -C "$work" || exit 1
  chmod -R a+rX "$work"
  # What the README says a system has (tools/package.sh): the graphics libraries, X11's and
  # Wayland's clients, fontconfig, D-Bus, glib, OpenSSL, libcurl. Whatever else a program wants
  # and the package hasn't is the find.
  docker run --rm -v "$work/hiraku:/w/hiraku:ro" -v "$self:/smoke.sh:ro" debian:12 bash -c '
    apt-get update -qq >/dev/null && apt-get install -y -qq --no-install-recommends \
      libgl1 libegl1 libopengl0 libx11-6 libxext6 libxi6 libxrandr2 libxrender1 libxkbcommon0 libwayland-client0 \
      libfontconfig1 libdbus-1-3 libglib2.0-0 libssl3 libcurl4 >/dev/null 2>&1 || { echo "the container could not install its libraries"; exit 1; }
    setpriv --reuid=1000 --regid=1000 --clear-groups bash /smoke.sh /w/hiraku'
  exit $?
fi
root=$(cd "${1:?the folder of the unpacked package}" && pwd)
how=${2:-}
case $how in ""|wine) ;; *) echo "the second argument is 'wine' or nothing" >&2; exit 2 ;; esac
ext=""
[ "$how" = wine ] && ext=.exe
failed=0
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

run() {   # a program of the package, as its system runs it
  if [ "$how" = wine ]; then wine "$@"; else "$@"; fi
}
ok() { echo "ok    $1"; }
bad() { echo "FAIL  $1"; failed=$((failed + 1)); }

# --- every program answers --help ---
n=0
for f in "$root"/bin/hiraku-*"$ext"; do
  name=$(basename "$f" "$ext")
  [ -f "$f" ] || continue
  case $name in
    hiraku-ezusb) continue ;;   # the pop'n board's go-between: 32-bit, started by hiraku only
    hiraku-start) continue ;;   # HIRAKU.exe before it is packed: it starts the launcher, no more
    *.bin|*.dll|*.so) continue ;;
  esac
  [ "$how" = wine ] || [ -x "$f" ] || continue
  out=$(run "$f" --help 2>&1 </dev/null)
  st=$?
  # (hiraku-chdcat and hiraku-xt446 have no --help: they say how they are used and end with 2)
  want=0
  case $name in hiraku-chdcat|hiraku-xt446|hiraku-recomp) want=2 ;; esac
  if [ "$st" = "$want" ] && [ -n "$out" ]; then n=$((n + 1))
  else bad "$name --help: ended with $st$([ -n "$out" ] && echo ", saying: $(echo "$out" | tail -2 | tr '\n' ' ')" || echo " and said nothing")"; fi
done
[ "$n" -ge 8 ] && ok "$n programs answer --help" || bad "only $n programs answered --help (a package has ten or more)"
# hiraku-server is a package of its own (tools/package-server.sh): it needs the system's SQLite
if [ -e "$root/bin/hiraku-server$ext" ]; then bad "bin/hiraku-server$ext is in the package (it has its own: tools/package-server.sh)"
else ok "no hiraku-server in the package (its own: tools/package-server.sh)"; fi
# the PC games' cabinet DLLs: with pop'n's, the C02's (IIDX 9th style on) and the C33's (the ee'MALL station)
if [ -e "$root/bin/pc/ezusb.dll" ]; then
  miss=""
  for f in c02/ezusb.dll c33/ezusb.dll c33/ezusb_video.dll; do [ -f "$root/bin/pc/$f" ] || miss="$miss $f"; done
  [ -z "$miss" ] && ok "bin/pc: the C02 and C33 cabinets' DLLs" || bad "bin/pc lacks:$miss"
fi
# Windows: FFmpeg is one set of DLLs in bin\ for every program, the Python core's too (tools/ffmpeg-min.sh)
if [ "$how" = wine ]; then
  ff=$(ls "$root/bin" | grep -ciE '^(avcodec|avformat|avutil)-[0-9]+\.dll$' || true)
  other=$(ls "$root/bin" | grep -iE '^(avdevice|avfilter|swscale|swresample|postproc)-[0-9]+\.dll$' || true)
  [ "$ff" = 3 ] && [ -z "$other" ] && ok "FFmpeg: one avcodec, avformat and avutil DLL in bin" ||
    bad "FFmpeg's DLLs in bin: $ff of avcodec/avformat/avutil (3 wanted)${other:+, and $other}"
fi

# --- the game menu, by a script, to its choice ---
printf 'hiraku-menu-list 1\ncurrent\tfirst\ngame\tfirst\tA shelf\tThe first game\t2001\ngame\tsecond\tA shelf\tThe second game\t2002\n' > "$tmp/list.txt"
printf 'game menu\nat 40 press "Keypad 2" 4\nat 80 press "Keypad Enter" 4\n' > "$tmp/pick.his"
if [ -f "$root/bin/hiraku-menu$ext" ]; then
  list=$tmp/list.txt; script=$tmp/pick.his; choice=$tmp/choice; outdir=$tmp/menu
  if [ "$how" = wine ]; then
    list=$(winepath -w "$list"); script=$(winepath -w "$script"); choice=$(winepath -w "$choice"); outdir=$(winepath -w "$outdir")
  fi
  out=$(run "$root/bin/hiraku-menu$ext" --list "$list" --choice "$choice" --family popn --script "$script" --frames 400 --out "$outdir" 2>&1 </dev/null)
  if echo "$out" | grep -q "menu: chosen second" && [ "$(tr -d '\r\n' < "$tmp/choice" 2>/dev/null)" = second ]; then ok "hiraku-menu ran a list by a script to its choice"
  else bad "hiraku-menu didn't choose the second game: $(echo "$out" | tail -3 | tr '\n' ' ')"; fi
else
  bad "no bin/hiraku-menu$ext in the package"
fi

# --- the interface's languages: the catalogues are in the package, and its programs find them ---
if [ -f "$root/lang/hiraku.pot" ] && ls "$root"/lang/*.po >/dev/null 2>&1; then ok "lang: the template and $(ls "$root"/lang/*.po | wc -l) catalogues"
else bad "no lang/hiraku.pot or lang/*.po in the package"; fi
if [ -f "$root/bin/hiraku-menu$ext" ] && [ -f "$root/lang/fr.po" ]; then
  out=$(HK_LANG=fr XDG_DATA_HOME=$tmp/data XDG_CONFIG_HOME=$tmp/config run "$root/bin/hiraku-menu$ext" --list "$list" --choice "$choice" \
        --family popn --script "$script" --frames 400 --out "$outdir" 2>&1 </dev/null)
  if echo "$out" | grep -q "lang: fr, [0-9]* texts translated"; then ok "hiraku-menu found the package's French catalogue"
  else bad "hiraku-menu with HK_LANG=fr didn't read lang/fr.po: $(echo "$out" | grep "lang:" | tail -2 | tr '\n' ' ')"; fi
fi

# --- the package's Python runs the tools ---
py=$root/python/bin/python3
[ "$how" = wine ] && py=$root/python/python.exe
if [ -f "$py" ]; then
  for tool in ingest.py update.py unlock.py; do
    [ -f "$root/tools/$tool" ] || { bad "no tools/$tool in the package"; continue; }
    t=$root/tools/$tool
    [ "$how" = wine ] && t=$(winepath -w "$t")
    out=$(run "$py" "$t" --help 2>&1 </dev/null)
    st=$?
    [ $tool = update.py ] && [ $st = 2 ] && st=0   # (it has no --help: it says how it is used and ends with 2)
    if [ $st = 0 ] && [ -n "$out" ]; then ok "tools/$tool --help with the package's Python"
    else bad "tools/$tool --help with the package's Python: ended with $st: $(echo "$out" | tail -2 | tr '\n' ' ')"; fi
  done
else
  bad "no Python in the package ($py)"
fi

# --- the launcher's main window, drawn into a picture (Linux: Qt's offscreen platform) ---
if [ "$how" != wine ]; then
  out=$(cd "$tmp" && HOME=$tmp XDG_CONFIG_HOME=$tmp/config XDG_DATA_HOME=$tmp/data QT_QPA_PLATFORM=offscreen \
        timeout 120 "$root/bin/hiraku" --show main --shot "$tmp/main.png" 2>&1 </dev/null)
  st=$?
  if [ $st = 0 ] && [ "$(stat -c %s "$tmp/main.png" 2>/dev/null || echo 0)" -gt 2000 ]; then ok "the launcher drew its main window"
  else bad "the launcher's main window: ended with $st: $(echo "$out" | tail -3 | tr '\n' ' ')"; fi
fi

[ "$failed" = 0 ] && echo "smoke: all fine" || echo "smoke: $failed failed"
exit $((failed > 0))
