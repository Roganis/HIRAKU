#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The FFmpeg a release needs, and nothing else: Twinkle's background video (runtime/twinkle/video.cpp)
# reads the player's Video CD or DVD as an MPEG program stream ("mpeg" demuxer) and decodes its
# MPEG-1 or MPEG-2 video; a game recorded to a video file (runtime/host/record.h, --record) is
# FFV1 and FLAC in Matroska, FFmpeg's own encoders (and their decoders and the Matroska reader, for
# its test, tests/host/test_record.cpp). Built static (LGPL: no --enable-gpl), so the games carry
# it and a PC needs no FFmpeg of its own (PLAN.md section 28, R-2).
#
#   tools/ffmpeg-min.sh PREFIX                        (Linux: position-independent, for the games)
#   tools/ffmpeg-min.sh PREFIX x86_64-w64-mingw32     (Windows, cross-built with mingw-w64's gcc)
#
# The libraries and their .pc files go to PREFIX (lib/pkgconfig: CMake finds them there through
# pkg-config). The source tarball is kept in PREFIX/src: LGPL wants the library's source to go
# with the binaries, so the release workflow publishes it beside the packages.
set -euo pipefail
prefix=${1:?prefix}
host=${2:-}
VER=9.0.2
# mpegvideo (the raw stream demuxer): the program stream demuxer probes its video stream with it,
# else the stream has no codec. The audio streams' parsers and decoders (a Video CD's MP2, a DVD's
# AC-3 or LPCM): without them stream detection reads on to its time limit before giving up on them.
DEMUXERS=mpegps,mpegvideo,matroska
DECODERS=mpeg1video,mpeg2video,mp2,ac3,pcm_dvd,ffv1,flac
PARSERS=mpegvideo,mpegaudio,ac3
ENCODERS=ffv1,flac
MUXERS=matroska
PROTOCOLS=file   # (the recorder writes its file through it; --disable-everything takes it away)
sig="ffmpeg-$VER host=$host demuxers=$DEMUXERS decoders=$DECODERS parsers=$PARSERS encoders=$ENCODERS muxers=$MUXERS protocols=$PROTOCOLS"
if [ -f "$prefix/lib/libavcodec.a" ] && [ "$(cat "$prefix/.ffmpeg-min" 2>/dev/null)" = "$sig" ]; then
  echo "ffmpeg-min: already in $prefix"
  exit 0
fi
mkdir -p "$prefix/src"
prefix=$(cd "$prefix" && pwd)
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
command -v nasm >/dev/null 2>&1 || { echo "ffmpeg-min: needs nasm (the nasm package)" >&2; exit 1; }
./configure --prefix="$prefix" "${cross[@]}" --enable-pic \
  --disable-autodetect --disable-everything \
  --enable-demuxer="$DEMUXERS" --enable-decoder="$DECODERS" --enable-parser="$PARSERS" \
  --enable-encoder="$ENCODERS" --enable-muxer="$MUXERS" --enable-protocol="$PROTOCOLS" \
  --disable-programs --disable-doc --disable-avdevice --disable-avfilter --disable-swscale \
  --disable-swresample --disable-network --disable-devices \
  --enable-static --disable-shared
make -j"$(nproc)"
make install
echo "$sig" > "$prefix/.ffmpeg-min"
echo "ffmpeg-min: in $prefix"
