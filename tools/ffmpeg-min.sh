#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The FFmpeg a release needs, and nothing else: Twinkle's background video (runtime/twinkle/video.cpp)
# reads the player's Video CD or DVD as an MPEG program stream ("mpeg" demuxer) and decodes its
# MPEG-1 or MPEG-2 video; a game recorded to a video file (runtime/host/record.h, --record) is
# FFV1 and FLAC in Matroska, FFmpeg's own encoders, or H.264 and AAC in MP4 (--record-format h264,
# and --convert-recording, which reads the first kind back): x264 (tools/enc-deps.sh), the graphics
# cards' encoders that load their driver's library only when asked (NVIDIA's NVENC, AMD's AMF,
# Vulkan video: libvulkan.so.1 or vulkan-1.dll, loaded by FFmpeg, never linked;
# Windows' Media Foundation, any card's or Microsoft's own), FFmpeg's AAC encoder. Their decoders
# and the MP4 reader are there for the recorder's test (tests/host/test_record.cpp). Built static,
# so the games carry it and a PC needs no FFmpeg of its own (PLAN.md section 28, R-2); with x264 it
# is GPL (--enable-gpl), as HIRAKU is.
#
#   tools/ffmpeg-min.sh PREFIX                        (Linux: position-independent, for the games)
#   tools/ffmpeg-min.sh PREFIX x86_64-w64-mingw32     (Windows, cross-built with mingw-w64's gcc)
#
# The libraries and their .pc files go to PREFIX (lib/pkgconfig: CMake finds them there through
# pkg-config). The source tarballs are kept in PREFIX/src (FFmpeg's, x264's): the release workflow
# publishes them beside the packages. HK_NO_ASM=1: no assembly (FFmpeg's or x264's), for a PC
# without nasm: a check of the build's wiring, not a release.
set -euo pipefail
prefix=${1:?prefix}
host=${2:-}
here=$(cd "$(dirname "$0")" && pwd)
VER=9.0.2
# mpegvideo (the raw stream demuxer): the program stream demuxer probes its video stream with it,
# else the stream has no codec. The audio streams' parsers and decoders (a Video CD's MP2, a DVD's
# AC-3 or LPCM): without them stream detection reads on to its time limit before giving up on them.
DEMUXERS=mpegps,mpegvideo,matroska,mov
DECODERS=mpeg1video,mpeg2video,mp2,ac3,pcm_dvd,ffv1,flac,h264,aac
PARSERS=mpegvideo,mpegaudio,ac3
ENCODERS=ffv1,flac,libx264,aac,h264_nvenc,h264_amf,h264_vulkan
HW=(--enable-ffnvcodec --enable-nvenc --enable-amf --enable-vulkan)
if [ -n "$host" ]; then   # Windows' own: Media Foundation (mfplat.dll, loaded when asked; its encoder needs D3D11VA's frames)
  ENCODERS=$ENCODERS,h264_mf
  HW+=(--enable-mediafoundation --enable-d3d11va)
fi
MUXERS=matroska,mp4
PROTOCOLS=file   # (the recorder writes its file through it; --disable-everything takes it away)
asm=on
[ "${HK_NO_ASM:-}" = 1 ] && asm=off
sig="ffmpeg-$VER host=$host demuxers=$DEMUXERS decoders=$DECODERS parsers=$PARSERS encoders=$ENCODERS muxers=$MUXERS protocols=$PROTOCOLS hw=${HW[*]} asm=$asm enc-deps=$(sha256sum < "$here/enc-deps.sh" | cut -c1-16)"
if [ -f "$prefix/lib/libavcodec.a" ] && [ "$(cat "$prefix/.ffmpeg-min" 2>/dev/null)" = "$sig" ]; then
  echo "ffmpeg-min: already in $prefix"
  exit 0
fi
mkdir -p "$prefix/src"
prefix=$(cd "$prefix" && pwd)
# x264, and the graphics cards' encoders' headers, into the same PREFIX
"$here/enc-deps.sh" "$prefix" "$host"
tarball=$prefix/src/ffmpeg-$VER.tar.xz
[ -f "$tarball" ] || curl -fL --retry 3 -o "$tarball" "https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
tar -xf "$tarball" -C "$work"
cd "$work/ffmpeg-$VER"
cross=()
[ -n "$host" ] && cross=(--arch=x86_64 --target-os=mingw32 --cross-prefix="$host-" --enable-cross-compile)
# nasm: the assembly kernels, as the distributions' FFmpeg has them (the regress video cases'
# baselines); without them decoding is about twice as slow
noasm=()
if [ $asm = off ]; then
  noasm=(--disable-x86asm)
else
  command -v nasm >/dev/null 2>&1 || { echo "ffmpeg-min: needs nasm (the nasm package)" >&2; exit 1; }
fi
# (pkg-config: only PREFIX's, x264's and ffnvcodec's, not the build PC's own)
PKG_CONFIG_LIBDIR=$prefix/lib/pkgconfig PKG_CONFIG_PATH= ./configure --prefix="$prefix" "${cross[@]}" "${noasm[@]}" --enable-pic \
  --disable-autodetect --disable-everything --enable-gpl --enable-libx264 "${HW[@]}" \
  --extra-cflags="-I$prefix/include" --extra-ldflags="-L$prefix/lib" --pkg-config=pkg-config --pkg-config-flags=--static \
  --enable-demuxer="$DEMUXERS" --enable-decoder="$DECODERS" --enable-parser="$PARSERS" \
  --enable-encoder="$ENCODERS" --enable-muxer="$MUXERS" --enable-protocol="$PROTOCOLS" \
  --disable-programs --disable-doc --disable-avdevice --disable-avfilter --disable-swscale \
  --disable-swresample --disable-network --disable-devices \
  --enable-static --disable-shared
make -j"${HK_JOBS:-$(nproc)}"
make install
echo "$sig" > "$prefix/.ffmpeg-min"
echo "ffmpeg-min: in $prefix"
