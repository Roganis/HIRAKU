#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The parsers that read what HIRAKU didn't write, fed made-up bytes (tests/fuzz/): clang's
# libFuzzer changes its inputs towards code not yet run, under AddressSanitizer and
# UndefinedBehaviorSanitizer, and a read or write outside a buffer, a step the language doesn't
# define, a leak or an endless loop ends the run with the input that did it.
#
#   tools/fuzz.sh [BUILD [SECONDS [DEPS]]]   BUILD: the build's folder (default out/fuzz)
#                                            SECONDS: for each target (default 60)
#                                            DEPS: tools/release-deps-linux.sh's folder, for a PC
#                                                  without the libraries (the workflow's runner)
#
# The targets: fuzz_kbin (Konami's binary XML and LZ77, off the network), fuzz_ea (an e-AMUSEMENT
# call and its HTTP message, off the network), fuzz_menu (the game menu's list and font files).
# An input that ends a run is kept in BUILD/found/; run its target on it again to see it:
#   BUILD/tests/fuzz_kbin BUILD/found/kbin-crash-...
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/out/fuzz}
seconds=${2:-60}
deps=${3:-}
args=(-G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHK_FUZZ=ON -DHK_GAME_BINARIES=OFF -DHK_TESTS=ON
      -DCMAKE_C_COMPILER="${CC:-clang}" -DCMAKE_CXX_COMPILER="${CXX:-clang++}")
if [ -n "$deps" ]; then
  deps=$(cd "$deps" && pwd)
  qt=$(echo "$deps"/qt/*/gcc_64)
  export PKG_CONFIG_LIBDIR=$deps/lib/pkgconfig PKG_CONFIG_PATH=
  args+=("-DCMAKE_PREFIX_PATH=$deps;$qt" -DHK_PCSC_DLOPEN=ON)
fi
targets=(kbin ea menu)
cmake -S "$root" -B "$build" "${args[@]}" > "$build.configure.log" 2>&1 || { tail -20 "$build.configure.log"; exit 1; }
rm -f "$build.configure.log"
cmake --build "$build" -j "${HK_JOBS:-$(nproc)}" --target fuzz_seeds "${targets[@]/#/fuzz_}"
"$build/tests/fuzz_seeds" "$build/corpus"
mkdir -p "$build/found"
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1} UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1}
failed=0
for t in "${targets[@]}"; do
  echo "--- fuzz_$t, $seconds s ---"
  if "$build/tests/fuzz_$t" -max_total_time="$seconds" -max_len=4096 -timeout=20 -rss_limit_mb=3072 -print_final_stats=1 \
       -artifact_prefix="$build/found/$t-" "$build/corpus/$t" > "$build/fuzz_$t.log" 2>&1; then
    grep -E "stat::number_of_executed_units|stat::new_units_added" "$build/fuzz_$t.log" | tr '\n' ' '; echo
  else
    failed=$((failed + 1))
    echo "fuzz_$t ended on an input (kept in $build/found/):"
    grep -vE "^#[0-9]+\s+(NEW|REDUCE|pulse|INITED|RELOAD)" "$build/fuzz_$t.log" | tail -40
  fi
done
[ "$failed" = 0 ] && echo "fuzz: nothing found" || echo "fuzz: $failed target(s) ended on an input"
exit $((failed > 0))
