#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# A Windows build as a zip (PLAN.md section 26), as tools/package.sh does for Linux:
#
#   tools/package-win.sh BUILD_DIR OUT.zip [portable]
#
# BUILD_DIR: a Windows build (cmake/x86_64-w64-mingw32.cmake). portable: the package has a folder
# hiraku/user, so its settings and saves stay in that folder (runtime/shell/userdir.h); without
# one they go in %LOCALAPPDATA%\hiraku.
#
# The zip holds hiraku/bin (the games and the launcher hiraku.exe, stripped, with SDL3.dll: the
# launcher's Qt is linked into it, tools/win-deps.sh), hiraku/games, hiraku/docs and an empty
# hiraku/dumps. The binaries take the folder above them
# as their root. Game data goes in with tools/pack-data.py, or tools/ingest.py on that PC.
set -euo pipefail
build=${1:?build dir}
out=${2:?output .zip}
mode=${3:-}
case $mode in ""|portable) ;; *) echo "the third argument is 'portable' or nothing" >&2; exit 2 ;; esac
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(realpath -m "$out")
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
d=$stage/hiraku
mkdir -p "$d/bin" "$d/dumps"
[ "$mode" = portable ] && mkdir -p "$d/user/config" "$d/user/data"
strip=$(command -v llvm-strip || command -v x86_64-w64-mingw32-strip)
for f in "$build"/hiraku*.exe; do
  [ -f "$f" ] || continue
  case $(basename "$f") in hiraku-recomp.exe) continue ;; esac
  "$strip" -o "$d/bin/$(basename "$f")" "$f"
done
cp "$build"/SDL3.dll "$d/bin/"
# HK_PACKAGE_BIN: more programs for bin\ (a release's 7-Zip, 7zr.exe: tools/mameset.py looks there first)
for f in ${HK_PACKAGE_BIN:-}; do cp -L "$f" "$d/bin/"; done
# HK_PYTHON_WIN: the Konami Python core's folder (the release workflow's python-windows job:
# hiraku-python.exe, its DLLs, PCSX2's resources\), into bin\ beside the other cores
if [ -n "${HK_PYTHON_WIN:-}" ]; then
  cp -n "$HK_PYTHON_WIN"/*.dll "$d/bin/"   # (SDL3.dll is the same release as the one above)
  cp "$HK_PYTHON_WIN"/hiraku-python.exe "$d/bin/"
  cp -r "$HK_PYTHON_WIN"/resources "$d/bin/resources"
fi
# Python for tools/ingest.py and unlock.py: python.org's embeddable one (tools/win-deps.sh), so
# adding a game needs nothing installed (the launcher looks for hiraku\python\python.exe first)
py=$root/out/win-deps/python
if [ -f "$py/python.exe" ]; then cp -r "$py" "$d/python"; else echo "no $py: the package will need Python installed" >&2; fi
cp -r "$root/games" "$root/docs" "$root/shaders" "$d/"
cp -r "$root/oracle/scripts" "$d/scripts"
mkdir -p "$d/tools"
cp "$root"/tools/{ingest.py,chd.py,fbexe.py,pack-data.py,unlock.py,chdread.py,mameset.py,catalogue.py} "$d/tools/"
cp -r "$root/tools/data" "$d/tools/"
cp "$root/LICENSE" "$d/" 2>/dev/null || true
for f in $(grep -o '"third_party/[^"]*"' "$root/launcher/aboutdialog.cpp" | tr -d '"'); do
  mkdir -p "$d/$(dirname "$f")" && cp "$root/$f" "$d/$f"
done
sed 's/$/\r/' > "$d/README.txt" <<'EOF'
HIRAKU for Windows (64-bit Windows 10 or later): BEMANI arcade machines, for your own game data.

Start the launcher: bin\hiraku.exe

Add a game: drop its files on the launcher's window, or File > Add game files, all at once:
the MAME ROM set (.zip, .7z, or a folder of its files) and its CHDs. HIRAKU tells the game from
the set's contents, copies what it needs into dumps\, and the files you dropped can go
afterwards. Nothing else to install: the Python that does the adding is in python\, and 7-Zip
and the CHD reader in bin\. A game whose page says it needs a first boot (an install, a clock
or a test-mode setting) says what to do there.

Windows may warn that the program is from an unknown publisher (it isn't signed): More info,
then Run anyway.

Your saves and the launcher's settings: in user\ beside this file when there is one (a portable
install: the folder runs as itself wherever it is moved, a USB stick included), else in
%LOCALAPPDATA%\hiraku. Copy user\ along to keep them when updating.

Konami Python games (pop'n music 9-14, DrumMania V/V2): add the game's folder together with the
folders bios (the arcade BIOS) and keys (the MagicGate key files), which HIRAKU can't ship. They
need a graphics driver with Vulkan (any current one).

Command line, from a command prompt in this folder (the launcher shows each game's under its
right-click menu):
    bin\hiraku-sys573 --game ddr3mj --window      (DJ Main games: bin\hiraku-djmain, Twinkle:
                                                  bin\hiraku-twinkle; a build with one binary per
                                                  game has bin\hiraku-<game>)
    bin\hiraku-sys573 --help                      (all options)
EOF
rm -f "$out"
(cd "$stage" && 7z a -tzip -mx=9 -bd -bso0 "$out" hiraku)
ls -la "$out"
