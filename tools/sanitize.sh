#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The unit tests under AddressSanitizer and UndefinedBehaviorSanitizer: everything built with
# HK_SANITIZE (CMakeLists.txt), then ctest. A read or write outside a buffer, memory used after
# it was freed or never freed, and a step the language doesn't define (a shift too far, a load
# from an address its type can't have, a null where none may be) each end the test that walked
# over it, with where it happened. What .github/workflows/release.yml runs on a push.
#
#   tools/sanitize.sh [BUILD [DEPS]]    BUILD: the build's folder (default out/asan)
#                                       DEPS:  tools/release-deps-linux.sh's folder, for a PC
#                                              without the libraries (the workflow's runner)
#
# The same build can run the games' regress cases (several times slower than the main build):
#   HIRAKU_ROOT=$PWD HK_REGRESS_BIN=$PWD/out/asan HK_REGRESS_OUT=$PWD/out/asan-regress \
#     ASAN_OPTIONS=detect_leaks=0 tools/regress.py check CASE...
# with out/asan-regress/baseline a link to the main build's baselines. The frames and the sound
# must come out identical; the run statistics differ where the main build has a game recompiled.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/out/asan}
deps=${2:-}
args=(-G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHK_SANITIZE=ON -DHK_GAME_BINARIES=OFF -DHK_TESTS=ON
      -DCMAKE_C_COMPILER="${CC:-clang}" -DCMAKE_CXX_COMPILER="${CXX:-clang++}")
if [ -n "$deps" ]; then
  deps=$(cd "$deps" && pwd)
  qt=$(echo "$deps"/qt/*/gcc_64)
  export PKG_CONFIG_LIBDIR=$deps/lib/pkgconfig PKG_CONFIG_PATH=
  args+=("-DCMAKE_PREFIX_PATH=$deps;$qt" -DHK_PCSC_DLOPEN=ON)
  # (tools/ingest.py's tests need Python 3.11 or later: the package's own)
  [ -x "$deps/python/bin/python3" ] && args+=("-DPython3_EXECUTABLE=$deps/python/bin/python3")
fi
cmake -S "$root" -B "$build" "${args[@]}"
cmake --build "$build" -j "${HK_JOBS:-$(nproc)}"
# a leak ends a test too; an undefined step prints the calls that led to it
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1} UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1}
ctest --test-dir "$build" --output-on-failure --timeout 600
