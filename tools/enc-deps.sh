#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# What a release FFmpeg's H.264 encoders are built with (runtime/host/record.h, --record-format
# h264): tools/ffmpeg-min.sh's static libraries and tools/release-python-linux.sh's shared ones.
#
#   - x264 (GPL-2.0-or-later), static and position-independent, 8-bit only, no program of its own:
#     FFmpeg's libx264, in the binaries themselves;
#   - nv-codec-headers (MIT), AMD's AMF headers (MIT) and Khronos' Vulkan-Headers (Apache-2.0 or
#     MIT), headers only: FFmpeg's h264_nvenc, h264_amf and h264_vulkan load the graphics card
#     driver's own library (libvulkan.so.1 / vulkan-1.dll for Vulkan video: AMD and Intel on Linux)
#     when a recording asks for them, so a PC without one needs nothing more and is only told it
#     isn't there.
#
#   tools/enc-deps.sh PREFIX                        (Linux)
#   tools/enc-deps.sh PREFIX x86_64-w64-mingw32     (Windows, cross-built)
#
# Into PREFIX: lib/libx264.a, include/ (x264.h, ffnvcodec/, AMF/, vulkan/, vk_video/), lib/pkgconfig (x264.pc,
# ffnvcodec.pc). Their sources are kept in PREFIX/src, checked against tools/release-sums.txt
# (tools/release-fetch.sh); the release workflow publishes x264's beside the packages (GPL).
# The versions are PCSX2 Reliquary 1.9.4's (its Windows build-dependencies.bat; Vulkan-Headers its
# Linux build-dependencies-qt.sh's), whose sums are the same files'. FFmpeg's h264_vulkan wants
# Vulkan headers of 1.3.277 or later (its configure). HK_NO_ASM=1: x264 without its assembly, on a PC without nasm (a check of the
# wiring only: x264 is then several times slower).
set -euo pipefail
prefix=${1:?prefix}
host=${2:-}
root=$(cd "$(dirname "$0")/.." && pwd)
X264=b35605ace3ddf7c1a5d67a2eb553f034aef41d55   # x264's stable branch, 2025-06-08
NVENC=13.0.19.0
AMF=1.5.2
VULKAN=1.4.328.1
asm=on
[ "${HK_NO_ASM:-}" = 1 ] && asm=off
sig="x264-$X264 nvenc-$NVENC amf-$AMF vulkan-$VULKAN host=$host asm=$asm"
mkdir -p "$prefix/src"
prefix=$(cd "$prefix" && pwd)
if [ -f "$prefix/lib/libx264.a" ] && [ "$(cat "$prefix/.enc-deps" 2>/dev/null)" = "$sig" ]; then
  echo "enc-deps: already in $prefix"
  exit 0
fi
. "$root/tools/release-fetch.sh"
hk_fetch "https://code.videolan.org/videolan/x264/-/archive/$X264.zip" "$prefix/src/x264-$X264.zip"
hk_fetch "https://github.com/FFmpeg/nv-codec-headers/releases/download/n$NVENC/nv-codec-headers-$NVENC.tar.gz" \
  "$prefix/src/nv-codec-headers-$NVENC.tar.gz"
hk_fetch "https://github.com/GPUOpen-LibrariesAndSDKs/AMF/releases/download/v$AMF/AMF-headers-v$AMF.tar.gz" \
  "$prefix/src/AMF-headers-v$AMF.tar.gz"
hk_fetch "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-$VULKAN.tar.gz" \
  "$prefix/src/Vulkan-Headers-vulkan-sdk-$VULKAN.tar.gz"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
unzip -q "$prefix/src/x264-$X264.zip" -d "$work"
tar -xf "$prefix/src/nv-codec-headers-$NVENC.tar.gz" -C "$work"
tar -xf "$prefix/src/AMF-headers-v$AMF.tar.gz" -C "$work"
tar -xf "$prefix/src/Vulkan-Headers-vulkan-sdk-$VULKAN.tar.gz" -C "$work"

opts=(--prefix="$prefix" --enable-static --enable-pic --disable-cli --bit-depth=8 --chroma-format=420
      --disable-opencl --disable-avs --disable-swscale --disable-lavf --disable-ffms --disable-gpac --disable-lsmash)
[ -n "$host" ] && opts+=(--host="$host" --cross-prefix="$host-")
if [ $asm = off ]; then
  opts+=(--disable-asm)
else
  command -v nasm >/dev/null 2>&1 || { echo "enc-deps: x264 needs nasm (the nasm package)" >&2; exit 1; }
fi
(cd "$work/x264-$X264" && ./configure "${opts[@]}" && make -j"${HK_JOBS:-$(nproc)}" && make install)
make -C "$work/nv-codec-headers-$NVENC" install PREFIX="$prefix" > /dev/null
mkdir -p "$prefix/include"
rm -rf "$prefix/include/AMF"
cp -r "$work/amf-headers-v$AMF/AMF" "$prefix/include/AMF"
rm -rf "$prefix/include/vulkan" "$prefix/include/vk_video"
cp -r "$work/Vulkan-Headers-vulkan-sdk-$VULKAN/include/vulkan" "$work/Vulkan-Headers-vulkan-sdk-$VULKAN/include/vk_video" "$prefix/include/"
echo "$sig" > "$prefix/.enc-deps"
echo "enc-deps: in $prefix"
