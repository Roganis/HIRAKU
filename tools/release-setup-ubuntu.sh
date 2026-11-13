#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The build tools and headers a release is built with, on Ubuntu 22.04 (the GitHub runner, or a
# container: docker run ... ubuntu:22.04), as root or with sudo (PLAN.md section 28, R-1):
#
#   tools/release-setup-ubuntu.sh linux     (tools/release-deps-linux.sh, tools/release.sh linux)
#   tools/release-setup-ubuntu.sh windows   (tools/win-deps.sh, tools/release.sh windows)
#   tools/release-setup-ubuntu.sh python    (linux's, and tools/release-python-linux.sh's)
#
# 22.04 for Linux: the package then runs on glibc 2.35 and later. Its own clang and CMake are too
# old for HIRAKU, so clang comes from apt.llvm.org and CMake from PyPI.
set -euo pipefail
target=${1:?linux, python or windows}
sudo=""
[ "$(id -u)" = 0 ] || sudo=sudo
export DEBIAN_FRONTEND=noninteractive
LLVM=21

# Ubuntu's package index. A GitHub runner takes Ubuntu's packages from a list of servers
# (/etc/apt/apt-mirrors.txt), Azure's own mirror first, and now and then a runner can't reach it:
# on 2026-11-11 the name couldn't be looked up in five attempts in a row over an hour, and the job
# ended here each time. So a download is tried three times, and where the index still can't be
# had, the first of the servers below that answers is used in place of the list. Any of them will
# do: apt checks every index against Ubuntu's own signature, whichever server hands it over.
echo 'Acquire::Retries "3";' | $sudo tee /etc/apt/apt.conf.d/80-hiraku-retries >/dev/null
apt_update() {
  local log st=0; log=$(mktemp)
  $sudo apt-get update -q 2>&1 | tee "$log" || st=$?
  # apt-get update ends with 0 when a server couldn't be reached ("W: Failed to fetch"): read what it said
  if grep -qE "(Failed to fetch|Temporary failure resolving|Could not resolve|Could not connect|Connection timed out).*(ubuntu|apt-mirrors)" "$log"; then
    st=1
  fi
  rm -f "$log"
  return $st
}
answers() {   # an Ubuntu mirror's address: its name is found, and it hands over the release's index
  local host=${1#*://}; host=${host%%/*}
  getent hosts "$host" >/dev/null || return 1
  if command -v curl >/dev/null; then curl -fsS --max-time 20 -o /dev/null "$1/dists/$2/InRelease"; fi
}
if ! apt_update; then
  codename=$(. /etc/os-release && echo "$VERSION_CODENAME")
  for m in http://archive.ubuntu.com/ubuntu http://us.archive.ubuntu.com/ubuntu \
           http://mirrors.kernel.org/ubuntu http://mirror.math.princeton.edu/pub/ubuntu; do
    if answers $m "$codename"; then
      echo "release-setup-ubuntu: the package servers set here can't be reached; using $m"
      if [ -f /etc/apt/apt-mirrors.txt ]; then   # the runner's list: this one server
        printf '%s/\tpriority:1\n' $m | $sudo tee /etc/apt/apt-mirrors.txt >/dev/null
      fi
      for f in /etc/apt/sources.list /etc/apt/sources.list.d/ubuntu.sources; do   # a plain Ubuntu's
        if [ -f $f ]; then $sudo sed -i -E "s#https?://[a-z0-9.-]*\.ubuntu\.com/ubuntu/?#$m/#g" $f; fi
      done
      break
    fi
    echo "release-setup-ubuntu: $m doesn't answer either"
  done
  apt_update || echo "release-setup-ubuntu: part of the package index is still missing; going on with what there is"
fi
$sudo apt-get install -y -q --no-install-recommends ca-certificates curl wget gnupg lsb-release software-properties-common \
  build-essential git make ninja-build nasm pkg-config python3 python3-venv python3-pip xz-utils p7zip-full file binutils patch
# clang and lld from LLVM's own packages
if ! command -v clang-$LLVM >/dev/null; then
  curl -fsSL https://apt.llvm.org/llvm.sh -o /tmp/llvm.sh
  $sudo bash /tmp/llvm.sh $LLVM
fi
$sudo apt-get install -y -q lld-$LLVM llvm-$LLVM
for t in clang clang++ lld ld.lld llvm-strip llvm-ar llvm-ranlib llvm-profdata; do
  $sudo ln -sf "$(command -v $t-$LLVM)" /usr/local/bin/$t
done
# CMake 3.25 or later
command -v cmake >/dev/null && cmake --version | head -1 | grep -qE ' (3\.(2[5-9]|[3-9][0-9])|[4-9])\.' ||
  $sudo python3 -m pip install -q --upgrade cmake
case $target in
linux)
  # SDL3's video and sound back ends (it loads them at run time: headers only matter), GL/EGL and
  # Wayland for the runtime, Qt's xcb plugin's run-time libraries (bundled by tools/package.sh),
  # PC/SC's headers (loaded at run time)
  $sudo apt-get install -y -q --no-install-recommends \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev \
    libxkbcommon-dev libxkbcommon-x11-0 libwayland-dev wayland-protocols libegl-dev libgl-dev libgles-dev \
    libdrm-dev libgbm-dev libdecor-0-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libdbus-1-dev \
    libudev-dev libibus-1.0-dev libusb-1.0-0-dev libpcsclite-dev zlib1g-dev \
    libfontconfig1 libfreetype6 libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 \
    libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1 libxcb-randr0 libxcb-util1 libxcb-ewmh2 \
    libssl3 libglib2.0-0
  # for the unit tests: qemu-ppc (the PowerPC instructions held to QEMU's), and a display with
  # OpenGL in software for the picture engine's (Xvfb and Mesa: the workflow runs ctest in it)
  $sudo apt-get install -y -q --no-install-recommends qemu-user xvfb libglx-mesa0 libgl1-mesa-dri libegl-mesa0
  # mingw-w64's i686 CRT for the PC games' 32-bit DLLs (runtime/pc/, built by clang), and its gcc
  # for libgcc (clang links it, and finds the CRT's headers by that gcc); its x86_64 CRT and gcc for
  # the 64-bit PC pop'n's loader (runtime/pc64/, hiraku-pc64.exe)
  $sudo apt-get install -y -q --no-install-recommends mingw-w64-i686-dev gcc-mingw-w64-i686 \
    mingw-w64-x86-64-dev gcc-mingw-w64-x86-64
  ;;
python)
  # PCSX2 Reliquary's build (tools/release-python-linux.sh), on top of linux's: what PCSX2's own
  # Linux CI installs (.github/workflows/linux_build_qt.yml in its source), less Qt's
  "$0" linux
  $sudo apt-get install -y -q --no-install-recommends \
    autoconf automake libtool extra-cmake-modules libaio-dev libcurl4-openssl-dev libevdev-dev libfontconfig-dev \
    libfreetype-dev libgtk-3-dev libgudev-1.0-dev libharfbuzz-dev libinput-dev libopengl-dev libpcap-dev libssl-dev \
    libva-dev libx11-xcb-dev libxcb1-dev libxcb-cursor-dev libxkbcommon-x11-dev unzip patch libpcsclite-dev patchelf
  ;;
windows)
  # mingw-w64's headers, CRT and winpthreads for clang (cmake/x86_64-w64-mingw32.cmake), its gcc
  # for FFmpeg (tools/ffmpeg-min.sh); Wine for the build's own generators; the host's Qt tools of
  # the same version as the static Windows Qt (moc, rcc, uic: tools/win-deps.sh)
  $sudo apt-get install -y -q --no-install-recommends mingw-w64 wine64 zip
  ;;
*) echo "linux, python or windows" >&2; exit 2 ;;
esac
echo "release-setup-ubuntu: ready for $target"
