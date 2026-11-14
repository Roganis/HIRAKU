#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The libraries a 64-bit ARM Linux build needs that Arch's aarch64-linux-gnu toolchain doesn't
# have (cmake/aarch64-linux-gnu.cmake, PLAN.md section 22): zlib and zstd, cross-built static
# from their sources with clang into out/arm-deps (position-independent: libchdr_mini.so takes them).
#
#   tools/arm-deps.sh ZLIB_SOURCE ZSTD_SOURCE
#
# Each SOURCE is the source folder or its release tarball (zlib-1.3.1.tar.gz from https://zlib.net,
# zstd-1.5.7.tar.gz from zstd's GitHub releases; their sums are in tools/release-sums.txt).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
deps=$root/out/arm-deps
work=$root/out/arm-deps-build
tc=$root/cmake/aarch64-linux-gnu.cmake
mkdir -p "$deps" "$work"
# a tarball is unpacked into the work folder first
source_dir() {
  if [ -d "$1" ]; then echo "$1"; return; fi
  tar -C "$work" -xf "$1"
  echo "$work/$(tar -tf "$1" | head -1 | cut -d/ -f1)"
}
zlib=$(source_dir "${1:?zlib source folder or tarball}")
zstd=$(source_dir "${2:?zstd source folder or tarball}")

cmake -S "$zlib" -B "$work/zlib" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$tc" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$deps" -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DZLIB_BUILD_EXAMPLES=OFF
cmake --build "$work/zlib" && cmake --install "$work/zlib"
rm -f "$deps"/lib/libz.so*   # static only: the build links statically (qemu-aarch64 runs it as it is)

cmake -S "$zstd/build/cmake" -B "$work/zstd" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$tc" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$deps" -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_BUILD_PROGRAMS=OFF \
  -DZSTD_BUILD_TESTS=OFF -DZSTD_MULTITHREAD_SUPPORT=OFF
cmake --build "$work/zstd" && cmake --install "$work/zstd"
echo "arm-deps: $deps"
