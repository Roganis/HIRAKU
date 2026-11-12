#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# What a Linux release is built against, in one folder (PLAN.md section 28, R-1), so the package
# runs on PCs other than the one that built it:
#
#   tools/release-deps-linux.sh PREFIX
#
#   - SDL3, shared (bin/lib/libSDL3.so.0 in the package): it loads X11, Wayland, ALSA, PulseAudio
#     and PipeWire itself when it finds them, so it is built with their headers there (the
#     workflow installs them) and needs none of them to start;
#   - zstd, static (libchdr's CHDs);
#   - FFmpeg's MPEG-1/2 decoding, static (tools/ffmpeg-min.sh: Twinkle's background video);
#   - Qt 6.12 for the launcher, from the Qt Company's binaries (aqtinstall), bundled by
#     tools/package.sh;
#   - 7-Zip's 7zz, static, for .7z sets (tools/mameset.py looks in the package's bin/ first);
#   - Python 3.14 for adding games (python-build-standalone), in the package's python/;
#   - DXVK's 64-bit d3d9.dll (zlib licence) for the 64-bit PC games under Wine (Wine's own
#     Direct3D 9 misplaces its pictures: PLAN.md section 32), in the package's bin/pc/.
# Each step is skipped when its result is already there (the workflow caches PREFIX). The source
# tarballs are kept in PREFIX/src (the LGPL ones are published beside the packages).
set -euo pipefail
prefix=${1:?prefix}
mkdir -p "$prefix/src"
prefix=$(cd "$prefix" && pwd)
root=$(cd "$(dirname "$0")/.." && pwd)
SDL_VER=3.4.18
ZSTD_VER=1.5.7
QT_VER=6.12.0
SEVENZIP=2602
PY_VER=3.14.8
PY_BUILD=20261013
DXVK_VER=2.6.2
jobs=$(nproc)

. "$root/tools/release-fetch.sh"
fetch() {   # URL FILE: into PREFIX/src, once, checked against tools/release-sums.txt
  hk_fetch "$1" "$prefix/src/$2"
}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

if [ ! -f "$prefix/lib/libSDL3.so.0" ]; then
  fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL3-$SDL_VER.tar.gz" "SDL3-$SDL_VER.tar.gz"
  tar -xf "$prefix/src/SDL3-$SDL_VER.tar.gz" -C "$work"
  cmake -S "$work/SDL3-$SDL_VER" -B "$work/sdl" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib -DSDL_SHARED=ON -DSDL_STATIC=OFF \
    -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
  cmake --build "$work/sdl" -j "$jobs"
  cmake --install "$work/sdl"
fi

if [ ! -f "$prefix/lib/libzstd.a" ]; then
  fetch "https://github.com/facebook/zstd/releases/download/v$ZSTD_VER/zstd-$ZSTD_VER.tar.gz" "zstd-$ZSTD_VER.tar.gz"
  tar -xf "$prefix/src/zstd-$ZSTD_VER.tar.gz" -C "$work"
  make -C "$work/zstd-$ZSTD_VER/lib" -j "$jobs" libzstd.a libzstd.pc PREFIX="$prefix" CFLAGS="-O2 -fPIC"
  mkdir -p "$prefix/include" "$prefix/lib/pkgconfig"
  cp "$work/zstd-$ZSTD_VER/lib/libzstd.a" "$prefix/lib/"
  cp "$work/zstd-$ZSTD_VER/lib/"{zstd.h,zstd_errors.h,zdict.h} "$prefix/include/"
  sed -e "s|^prefix=.*|prefix=$prefix|" -e "s|^libdir=.*|libdir=$prefix/lib|" \
    "$work/zstd-$ZSTD_VER/lib/libzstd.pc" > "$prefix/lib/pkgconfig/libzstd.pc"
fi

"$root/tools/ffmpeg-min.sh" "$prefix"

if [ ! -d "$prefix/qt/$QT_VER/gcc_64" ]; then
  python3 -m venv "$work/aqt"
  "$work/aqt/bin/pip" install -q aqtinstall
  "$work/aqt/bin/aqt" install-qt linux desktop "$QT_VER" linux_gcc_64 -O "$prefix/qt"
fi

# Python for adding games (tools/ingest.py needs 3.11 or later: tomllib), as the Windows zip
# carries python.org's: python-build-standalone's, which runs on glibc 2.17 on; its tests, Tk and
# headers left out
if [ ! -x "$prefix/python/bin/python3" ]; then
  fetch "https://github.com/astral-sh/python-build-standalone/releases/download/$PY_BUILD/cpython-$PY_VER%2B$PY_BUILD-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz" \
    "cpython-$PY_VER+$PY_BUILD-linux.tar.gz"
  rm -rf "$prefix/python"
  tar -xf "$prefix/src/cpython-$PY_VER+$PY_BUILD-linux.tar.gz" -C "$prefix"
  lib=$(echo "$prefix"/python/lib/python3.*)
  rm -rf "$lib"/{test,idlelib,tkinter,turtledemo,ensurepip,lib2to3} "$lib"/config-* "$prefix"/python/include \
    "$prefix"/python/share "$prefix"/python/lib/{libtcl*,libtk*,tcl*,tk*,itcl*,thread*,pkgconfig} 2>/dev/null || true
  find "$prefix/python" -name __pycache__ -prune -exec rm -rf {} +
fi

if [ ! -x "$prefix/bin/7zz" ]; then
  fetch "https://github.com/ip7z/7zip/releases/download/${SEVENZIP:0:2}.${SEVENZIP:2}/7z$SEVENZIP-linux-x64.tar.xz" "7z$SEVENZIP-linux-x64.tar.xz"
  mkdir -p "$prefix/bin" "$work/7z"
  tar -xf "$prefix/src/7z$SEVENZIP-linux-x64.tar.xz" -C "$work/7z"
  cp "$work/7z/7zzs" "$prefix/bin/7zz"   # the static build
  cp "$work/7z/License.txt" "$prefix/src/7-Zip-License.txt"
fi
if [ ! -f "$prefix/dxvk/d3d9.dll" ]; then
  fetch "https://github.com/doitsujin/dxvk/releases/download/v$DXVK_VER/dxvk-$DXVK_VER.tar.gz" "dxvk-$DXVK_VER.tar.gz"
  fetch "https://raw.githubusercontent.com/doitsujin/dxvk/v$DXVK_VER/LICENSE" "dxvk-$DXVK_VER-LICENSE"
  tar -xf "$prefix/src/dxvk-$DXVK_VER.tar.gz" -C "$work"
  mkdir -p "$prefix/dxvk"
  cp "$work/dxvk-$DXVK_VER/x64/d3d9.dll" "$prefix/dxvk/"
  cp "$prefix/src/dxvk-$DXVK_VER-LICENSE" "$prefix/dxvk/LICENSE"
fi
echo "release-deps-linux: in $prefix"
