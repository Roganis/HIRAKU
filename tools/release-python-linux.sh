#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# A release's Konami Python core (PLAN.md section 28, R-4): hiraku-python, built inside PCSX2
# Reliquary's source as oracle/pcsx2/build-reliquary.sh builds it on the owner's PC, but for a
# package that runs on other PCs (Ubuntu 22.04's glibc, as tools/release.sh linux):
#
#   tools/release-python-linux.sh WORK LINUX_DEPS
#
#   - Reliquary 1.9.4's source, the pinned zip (oracle/pcsx2/README.md), from HIRAKU's own
#     "reliquary-1.9.4" release (no public copy of this version exists elsewhere), SHA-256 checked;
#     HIRAKU's patch applied (oracle/pcsx2/reliquary-oracle.patch);
#   - its libraries built by PCSX2's own script (.github/workflows/scripts/linux/
#     build-dependencies-qt.sh in the source), less what HIRAKU doesn't use: its Qt and
#     KDDockWidgets (PCSX2's own interface is off), its SDL (HIRAKU's, from LINUX_DEPS: the core
#     draws in HIRAKU's window) and its FFmpeg (only its headers: PCSX2 loads FFmpeg at run time,
#     for video capture);
#   - hiraku-python (--game ID: any Python game), with Reliquary's resources/ beside it.
# WORK/pcsx2-reliquary/build/bin/hiraku-python is then packaged by tools/release.sh linux.
# The build tools: tools/release-setup-ubuntu.sh python.
set -euo pipefail
work=${1:?work folder}
linux_deps=$(cd "${2:?the folder of tools/release-deps-linux.sh}" && pwd)
mkdir -p "$work"
work=$(cd "$work" && pwd)
root=$(cd "$(dirname "$0")/.." && pwd)
ZIP_URL=https://github.com/Roganis/HIRAKU/releases/download/reliquary-1.9.4/pcsx2-reliquary-1.9.4-reliquary.zip
ZIP_SHA256=da9b9a601e8b3eeede9eb8bae6d46e81e1001beb98b438859aa847c75b547884
src=$work/pcsx2-reliquary
deps=$work/deps
build=$src/build
jobs=${HK_JOBS:-$(nproc)}

if [ ! -d "$src" ]; then
  zip=${HK_RELIQUARY_ZIP:-$work/pcsx2-reliquary-1.9.4-reliquary.zip}
  [ -f "$zip" ] || curl -fL --retry 3 -o "$zip" "$ZIP_URL"
  echo "$ZIP_SHA256  $zip" | sha256sum -c --quiet -
  rm -rf "$work/unpack" && mkdir -p "$work/unpack"
  (cd "$work/unpack" && unzip -q "$zip")
  mv "$work/unpack"/pcsx2-reliquary-* "$src"
  rmdir "$work/unpack"
  (cd "$src" && patch -p1 < "$root/oracle/pcsx2/reliquary-oracle.patch")
  # the source zip has an empty paraLLEl-GS submodule: without the folder, the build leaves it out
  rmdir "$src/pcsx2/GS/parallel-gs" 2>/dev/null || true
fi

if [ ! -f "$deps/.done" ]; then
  # PCSX2's script, its blocks for Qt, KDDockWidgets and SDL left out (each block runs from its
  # `echo "Building ..."` or `echo "Installing ..."` line to the next)
  # (beside the original: it finds its patches from its own folder)
  s=$src/.github/workflows/scripts/linux/build-dependencies-qt.sh
  awk '/^echo "(Building|Installing|Cleaning)/ { skip = /Qt|KDDockWidgets|SDL/ } !skip' "$s" > "${s%.sh}-hiraku.sh"
  (cd "$work" && BUILD_FFMPEG=0 bash "${s%.sh}-hiraku.sh" "$deps")
  # the Vulkan headers and FFmpeg's headers, of the versions the script names
  eval "$(grep -E '^(VULKAN|FFMPEG)=' "$s")"
  t=$(mktemp -d)
  curl -fL --retry 3 -o "$t/vk.tar.gz" "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-$VULKAN.tar.gz"
  tar -xf "$t/vk.tar.gz" -C "$t"
  cmake -S "$t/Vulkan-Headers-vulkan-sdk-$VULKAN" -B "$t/vk" -DCMAKE_INSTALL_PREFIX="$deps" > /dev/null
  cmake --install "$t/vk" > /dev/null
  curl -fL --retry 3 -o "$t/ffmpeg.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG.tar.xz"
  tar -xf "$t/ffmpeg.tar.xz" -C "$t"
  (cd "$t/ffmpeg-$FFMPEG" && ./configure --prefix="$deps" --disable-all --disable-autodetect --disable-static --enable-shared \
     --enable-avcodec --enable-avformat --enable-avutil --enable-swresample --enable-swscale --disable-x86asm > /dev/null &&
   make -j"$jobs" > /dev/null && make install > /dev/null)
  rm -rf "$t"
  touch "$deps/.done"
fi

# the core alone: PCSX2's own interface off (no Qt), FFmpeg loaded at run time, HIRAKU's SDL3;
# for any x86-64 CPU with SSE4.1, as PCSX2's own releases (and the Windows job): its renderer in
# SSE4, AVX and AVX2 copies, picked at run time. Without DISABLE_ADVANCE_SIMD it builds with
# -march=native, for the build machine's CPU: on the runner's AMD EPYC, v0.2.0's and v0.3.0's
# hiraku-python got SSE4a instructions, which no Intel CPU has (it stops on them)
cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DDISABLE_ADVANCE_SIMD=ON \
  -DCMAKE_C_COMPILER="${CC:-clang}" -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
  -DCMAKE_PREFIX_PATH="$deps;$linux_deps" -DCMAKE_BUILD_RPATH='$ORIGIN/lib' \
  `# an RPATH, not a RUNPATH: it also finds the bundled libraries' own (libwebp's libsharpyuv)` \
  -DCMAKE_EXE_LINKER_FLAGS=-Wl,--disable-new-dtags \
  -DENABLE_QT_UI=OFF -DENABLE_TESTS=OFF -DUSE_LINKED_FFMPEG=OFF -DWAYLAND_SCANNER=OFF -DHK_PCSC_DLOPEN=ON \
  -DHIRAKU_DIR="$root" -DHK_RELEASE=ON
cmake --build "$build" -j "$jobs" --target hiraku-python
# no instruction some x86-64 CPUs lack: AMD's SSE4a (extrq, insertq), AVX-512 (zmm registers)
bad=$(objdump -d --no-show-raw-insn "$build/bin/hiraku-python" | grep -cE '\s(extrq|insertq)\s|%zmm' || true)
[ "$bad" = 0 ] || { echo "release-python-linux: $bad instructions not every x86-64 CPU has" >&2; exit 1; }
# libpcap (PCSX2's network adapter links it) goes in the package: Debian and Ubuntu call it
# libpcap.so.0.8, Fedora and Arch libpcap.so.1. In the deps' lib/, tools/package.sh takes it along.
pcap=$(ldd "$build/bin/hiraku-python" | awk '/libpcap\.so/ {print $3}')
[ -n "$pcap" ] && [ "${pcap#$deps/}" = "$pcap" ] && cp -L "$pcap" "$deps/lib/"
rm -rf "$build/bin/resources"
cp -r "$src/bin/resources" "$build/bin/resources"
echo "release-python-linux: $build/bin/hiraku-python"
