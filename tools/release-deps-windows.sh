#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# What a Windows release is built against (PLAN.md section 28, R-1), downloaded and built into
# out/win-deps (cmake/x86_64-w64-mingw32.cmake's folder): tools/win-deps.sh's SDL3, zlib, zstd,
# static qtbase and embeddable Python, plus what makes adding and playing games need nothing
# installed: FFmpeg's MPEG-1/2 decoding (tools/ffmpeg-min.sh, Twinkle's background video) and
# 7-Zip's 7zr.exe for .7z sets (tools/mameset.py looks in the package's bin/ first).
#
#   tools/release-deps-windows.sh
#
# The host's Qt must be QT_VER (its moc, rcc and uic build the launcher; the workflow runs this in
# an Arch container, whose Qt is). Each step is skipped when its result is there (the workflow
# caches out/win-deps; qtbase takes about an hour).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
deps=$root/out/win-deps
dl=$deps/dl
mkdir -p "$dl"
SDL_VER=3.4.18
ZLIB_VER=1.3.1
ZSTD_VER=1.5.7
# the static qtbase's version must be the host's (its moc, rcc and uic build the launcher)
QT_VER=${HK_QT_VER:-$(qmake6 -query QT_VERSION 2>/dev/null || echo 6.12.0)}
PY_VER=3.14.8
SEVENZIP=26.02

. "$root/tools/release-fetch.sh"
fetch() {   # URL FILE: into out/win-deps/dl, once, checked against tools/release-sums.txt
  hk_fetch "$1" "$dl/$2"
}

if [ ! -f "$deps/lib/libzstd.a" ] || [ ! -f "$deps/qt6/lib/libQt6Core.a" ] || [ ! -f "$deps/python/python.exe" ]; then
  fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL3-devel-$SDL_VER-mingw.tar.gz" "SDL3-devel-$SDL_VER-mingw.tar.gz"
  fetch "https://github.com/madler/zlib/releases/download/v$ZLIB_VER/zlib-$ZLIB_VER.tar.gz" "zlib-$ZLIB_VER.tar.gz"
  fetch "https://github.com/facebook/zstd/releases/download/v$ZSTD_VER/zstd-$ZSTD_VER.tar.gz" "zstd-$ZSTD_VER.tar.gz"
  fetch "https://download.qt.io/archive/qt/${QT_VER%.*}/$QT_VER/submodules/qtbase-everywhere-src-$QT_VER.tar.xz" "qtbase-everywhere-src-$QT_VER.tar.xz"
  fetch "https://www.python.org/ftp/python/$PY_VER/python-$PY_VER-embed-amd64.zip" "python-$PY_VER-embed-amd64.zip"
  src=$(mktemp -d)
  trap 'rm -rf "$src"' EXIT
  for f in "SDL3-devel-$SDL_VER-mingw.tar.gz" "zlib-$ZLIB_VER.tar.gz" "zstd-$ZSTD_VER.tar.gz"; do tar -xf "$dl/$f" -C "$src"; done
  qt=""
  [ -f "$deps/qt6/lib/libQt6Core.a" ] || qt=$dl/qtbase-everywhere-src-$QT_VER.tar.xz
  "$root/tools/win-deps.sh" "$src/SDL3-$SDL_VER" "$src/zlib-$ZLIB_VER" "$src/zstd-$ZSTD_VER" "$qt" "$dl/python-$PY_VER-embed-amd64.zip"
fi

"$root/tools/ffmpeg-min.sh" "$deps" x86_64-w64-mingw32

if [ ! -f "$deps/bin/7zr.exe" ]; then
  # (a release's own file: 7-zip.org's 7zr.exe is whichever version is the latest)
  fetch "https://github.com/ip7z/7zip/releases/download/$SEVENZIP/7zr.exe" "7zr-${SEVENZIP/./}.exe"
  mkdir -p "$deps/bin"
  cp "$dl/7zr-${SEVENZIP/./}.exe" "$deps/bin/7zr.exe"
fi
echo "release-deps-windows: in $deps"
