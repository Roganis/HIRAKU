#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# A copy of a build for another Linux PC (a test laptop):
#
#   tools/package.sh BUILD_DIR OUT.tar.xz [portable]
#
# portable: the package has a folder hiraku/user, which makes it a portable install
# (runtime/shell/userdir.h): its settings and saves stay in that folder, not in ~/.config and
# ~/.local/share, so it moves (or goes on a USB stick) as one.
#
# BUILD_DIR should be configured for it:
#   cmake -S . -B BUILD_DIR -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
#     -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON \
#     '-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc' '-DCMAKE_BUILD_RPATH=$ORIGIN/lib'
# (CMAKE_BUILD_RPATH, not -Wl,-rpath: ninja hands the flags to a shell, which eats $ORIGIN)
# (no FluidSynth, the C++ runtime linked in, SDL3 looked for beside the binaries; clang, as the
# main build: GCC takes about five times as long, and far more memory, on the generated recompiler
# blocks).
# Faster with profile-guided optimisation: add -DHK_PGO_DIR=<this PC's build/pgo> (tools/pgo.py)
#
# The archive holds hiraku/bin (the binaries, stripped, with libSDL3 in bin/lib),
# hiraku/games, hiraku/docs and an empty hiraku/dumps: the binaries take the folder
# above them as their root. Game data goes in with tools/pack-data.py.
set -euo pipefail
build=${1:?build dir}
out=${2:?output .tar.xz}
mode=${3:-}
case $mode in ""|portable) ;; *) echo "the third argument is 'portable' or nothing" >&2; exit 2 ;; esac
root=$(cd "$(dirname "$0")/.." && pwd)
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
d=$stage/hiraku
mkdir -p "$d/bin/lib" "$d/dumps"
[ "$mode" = portable ] && mkdir -p "$d/user/config" "$d/user/data"
for f in "$build"/hiraku "$build"/hiraku-*; do
  [ -x "$f" ] && [ -f "$f" ] || continue
  case $(basename "$f") in hiraku-recomp) continue ;; esac
  strip -o "$d/bin/$(basename "$f")" "$f"
  # Konami Python games (runtime/python/) are built in PCSX2 Reliquary's tree and linked
  # into the build dir: they need Reliquary's resources/ beside them, and the libraries
  # built for it outside the system (oracle/pcsx2/build-reliquary.sh's deps/), which
  # they find in bin/lib through their $ORIGIN/lib run path
  real=$(readlink -f "$f")
  if [ -d "$(dirname "$real")/resources" ]; then
    [ -d "$d/bin/resources" ] || cp -r "$(dirname "$real")/resources" "$d/bin/resources"
    # (HK_PYTHON_LIBS: where a release's Reliquary libraries are, which the binary finds in bin/lib)
    LD_LIBRARY_PATH=${HK_PYTHON_LIBS:-}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} ldd "$real" |
      awk '$3 ~ /^\// && $3 !~ /^\/(usr\/)?lib(64)?\// {print $3}' | while read -r lib; do
      cp -L "$lib" "$d/bin/lib/"
      # a library built elsewhere may name its build folder as its run path (PCSX2's libwebp,
      # libjpeg): its own libraries are beside it in the package
      if command -v patchelf > /dev/null && readelf -d "$d/bin/lib/$(basename "$lib")" | grep -q RUNPATH; then
        patchelf --set-rpath '$ORIGIN' "$d/bin/lib/$(basename "$lib")"
      fi
    done
  fi
done
# The launcher (bin/hiraku) with its own Qt, so a PC without Qt 6.12 runs it: the Qt libraries
# and everything they need but the system's own (glibc, the graphics and windowing stack, fonts,
# D-Bus, glib, OpenSSL, sound) in bin/qt/lib, the plugins it uses in bin/qt/plugins (qt.conf).
# No run paths can be written into copied libraries here (no patchelf), so bin/hiraku is a
# script that sets LD_LIBRARY_PATH for bin/hiraku.bin, which puts the user's own back once Qt is
# up (launcher/main.cpp): the games and tools it starts never see the bundled Qt.
bundle_qt() {
  # HK_QT_DIR: a Qt not the system's (the Qt Company's, tools/release-deps-linux.sh), its plugins in plugins/
  local qtplugins=${HK_QT_DIR:+$HK_QT_DIR/plugins}
  [ -n "$qtplugins" ] || qtplugins=/usr/lib/qt6/plugins
  [ -d "$qtplugins" ] || qtplugins=$(dirname "$(ldd "$d/bin/hiraku.bin" | awk '/libQt6Core/ {print $3}')")/qt6/plugins
  mkdir -p "$d/bin/qt/lib" "$d/bin/qt/plugins"
  local p
  for p in platforms/libqwayland.so platforms/libqxcb.so platforms/libqoffscreen.so \
           wayland-shell-integration/libxdg-shell.so wayland-decoration-client/libbradient.so \
           wayland-graphics-integration-client/libqt-plugin-wayland-egl.so \
           xcbglintegrations/libqxcb-egl-integration.so xcbglintegrations/libqxcb-glx-integration.so \
           platformthemes/libqxdgdesktopportal.so platforminputcontexts/libcomposeplatforminputcontextplugin.so \
           tls/libqopensslbackend.so tls/libqcertonlybackend.so; do
    [ -f "$qtplugins/$p" ] || continue
    mkdir -p "$d/bin/qt/plugins/$(dirname "$p")"
    cp -L "$qtplugins/$p" "$d/bin/qt/plugins/$p"
  done
  # (with HK_QT_DIR's libraries findable: a plugin finds its Qt libraries by its own run path,
  # which points at bin/qt/lib, still empty here)
  { LD_LIBRARY_PATH=${HK_QT_DIR:+$HK_QT_DIR/lib}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} ldd "$d/bin/hiraku.bin"
    find "$d/bin/qt/plugins" -name '*.so' -exec env LD_LIBRARY_PATH=${HK_QT_DIR:+$HK_QT_DIR/lib} ldd {} \; ; } |
    awk '$2 == "=>" && $3 ~ /^\// {print $3}' | sort -u | while read -r lib; do
      local n; n=$(basename "$lib")
      # HK_QT_DIR (the Qt Company's Qt, which carries its own ICU): its libraries and the xcb
      # extensions below only; the rest is the system's, whose newer libstdc++, crypto and
      # compression libraries the graphics drivers may need
      if [ -n "${HK_QT_DIR:-}" ]; then
        case $lib in
          "$HK_QT_DIR"/*) ;;
          *) case $n in libxcb-*|libxkbcommon-x11*) ;; *) continue ;; esac ;;
        esac
      fi
      case $n in
        # the xcb extensions Qt's xcb plugin needs and a desktop may lack (Qt's list); libxcb itself and
        # the graphics stack's (dri, glx, present, sync) stay the system's
        libxcb-icccm*|libxcb-image*|libxcb-keysyms*|libxcb-render-util*|libxcb-util*|libxcb-cursor*|libxcb-ewmh*) ;;
        libxcb-shape*|libxcb-xinerama*|libxcb-xkb*|libxcb-xinput*|libxcb-randr*|libxcb-render.so*|libxcb-xfixes*|libxcb-shm*) ;;
        libxkbcommon-x11*) ;;
        ld-linux*|libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|libresolv.so*|libutil.so*|libanl.so*|libmvec.so*) continue ;;
        libGL*|libGLX*|libOpenGL*|libEGL*|libGLES*|libdrm*|libgbm*|libvulkan*) continue ;;
        libwayland-*|libX*|libxcb*|libxkbcommon*|libxshmfence*) continue ;;
        libfontconfig*|libfreetype*|libharfbuzz*|libgraphite2*|libexpat*|libz.so*|libffi*) continue ;;
        libdbus-1*|libsystemd*|libudev*|libglib-2.0*|libgobject-2.0*|libgio-2.0*|libgmodule-2.0*|libgthread-2.0*) continue ;;
        libSDL3*) continue ;;   # in bin/lib, for the games too
        libssl*|libcrypto*|libasound*|libpulse*|libselinux*|libmount*|libblkid*|libuuid*|libcap.so*) continue ;;
      esac
      cp -L "$lib" "$d/bin/qt/lib/$n"
    done
  printf '[Paths]\nPrefix = qt\nPlugins = plugins\n' > "$d/bin/qt.conf"
  cat > "$d/bin/hiraku" <<'WRAP'
#!/bin/sh
# HIRAKU's launcher with the Qt in bin/qt (tools/package.sh; launcher/main.cpp puts these back)
here=$(dirname "$(readlink -f "$0")")
HK_SAVED_ENV=1; export HK_SAVED_ENV
[ "${LD_LIBRARY_PATH+set}" = set ] && { HK_SAVED_LD_LIBRARY_PATH=$LD_LIBRARY_PATH; export HK_SAVED_LD_LIBRARY_PATH; }
[ "${QT_PLUGIN_PATH+set}" = set ] && { HK_SAVED_QT_PLUGIN_PATH=$QT_PLUGIN_PATH; export HK_SAVED_QT_PLUGIN_PATH; }
LD_LIBRARY_PATH="$here/qt/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"; export LD_LIBRARY_PATH
unset QT_PLUGIN_PATH
exec "$here/hiraku.bin" "$@"
WRAP
  chmod +x "$d/bin/hiraku"
}
if [ -f "$d/bin/hiraku" ]; then
  mv "$d/bin/hiraku" "$d/bin/hiraku.bin"
  bundle_qt
fi
sdl=""
for f in "$build"/hiraku "$build"/hiraku-*; do   # the first binary that has SDL (hiraku-eamuse hasn't)
  [ -n "$sdl" ] || sdl=$(ldd "$f" 2>/dev/null | awk '/libSDL3/ {print $3}')
done
[ -n "$sdl" ] && cp -L "$sdl" "$d/bin/lib/"
# HK_PACKAGE_BIN: more programs for bin/ (a release's 7-Zip, bin/7zz: tools/mameset.py looks there first)
for f in ${HK_PACKAGE_BIN:-}; do cp -L "$f" "$d/bin/"; done
# the PC games' DLLs (runtime/pc/), 32-bit Windows ones that hiraku-pc puts beside the game's
# program under Wine, and the 64-bit games' loader (runtime/pc64/)
if [ -d "$build/pc" ]; then
  mkdir -p "$d/bin/pc"
  cp "$build"/pc/ezusb.dll "$build"/pc/libacio.dll "$d/bin/pc/"
  # the C02 cabinet's board (beatmania IIDX 9th style) and its later games' (RED), in a folder of their own
  for f in ezusb.dll RtEffect.dll; do if [ -f "$build/pc/c02/$f" ]; then mkdir -p "$d/bin/pc/c02" && cp "$build/pc/c02/$f" "$d/bin/pc/c02/"; fi; done
  cp "$build"/pc/hiraku-pc64.exe "$d/bin/pc/" 2>/dev/null || true
  cp "$build"/pc/hiraku-pc32.exe "$d/bin/pc/" 2>/dev/null || true
fi
# HK_PACKAGE_DXVK: a folder with DXVK's 64-bit d3d9.dll and its LICENSE, beside the loader (hiraku-pc
# has Wine load it for the 64-bit PC games: Wine's own Direct3D 9 misplaces its pictures)
# (and its 32-bit one, DXVK's x32/d3d9.dll there, in bin/pc/dxvk32/ for the 32-bit AVS2 PC games)
if [ -n "${HK_PACKAGE_DXVK:-}" ] && [ -d "$d/bin/pc" ]; then
  cp "$HK_PACKAGE_DXVK/d3d9.dll" "$d/bin/pc/"
  if [ -f "$HK_PACKAGE_DXVK/x32/d3d9.dll" ]; then mkdir -p "$d/bin/pc/dxvk32" && cp "$HK_PACKAGE_DXVK/x32/d3d9.dll" "$d/bin/pc/dxvk32/"; fi
  mkdir -p "$d/third_party/dxvk" && cp "$HK_PACKAGE_DXVK/LICENSE" "$d/third_party/dxvk/LICENSE"
fi
# HK_PACKAGE_LIBS: libraries for bin/lib that the binaries load themselves (dlopen: ldd doesn't
# see them), a release's PCSX2 shader compiler (libshaderc_shared.so.1)
for f in ${HK_PACKAGE_LIBS:-}; do
  cp -L "$f" "$d/bin/lib/"
  if command -v patchelf > /dev/null && readelf -d "$d/bin/lib/$(basename "$f")" | grep -q RUNPATH; then
    patchelf --set-rpath '$ORIGIN' "$d/bin/lib/$(basename "$f")"
  fi
done
# HK_PYTHON_DIR: a Python for adding games, in python/ (the launcher looks there first)
[ -n "${HK_PYTHON_DIR:-}" ] && cp -r "$HK_PYTHON_DIR" "$d/python"
cp -r "$root/games" "$root/docs" "$root/shaders" "$d/"
cp -r "$root/oracle/scripts" "$d/scripts"
mkdir -p "$d/tools"
cp "$root"/tools/{ingest.py,checklist.py,chd.py,fbexe.py,pack-data.py,regress.py,unlock.py,pythonfs.py,chdread.py,mameset.py,catalogue.py,update.py} "$d/tools/"
# MAME's BEMANI sets as data: ingest tells a set by its contents (tools/mameset.py)
cp -r "$root/tools/data" "$d/tools/"
# e-AMUSEMENT (Konami Python): the server checker and the game's calls (docs/EAMUSE3.md)
mkdir -p "$d/tools/eamuse" && cp "$root"/tools/eamuse/{check.py,corpus.json} "$d/tools/eamuse/"
mkdir -p "$d/tools/eamuse/bemaniutils" && cp "$root"/tools/eamuse/bemaniutils/{README.md,popn9-python.patch} "$d/tools/eamuse/bemaniutils/"
# Konami Python ingest reads disks in place through libchdr (tools/chdread.py)
for f in "$build/libchdr_mini.so" "$root/build/libchdr_mini.so"; do   # the build's (CMake), else tools/build-libchdr.sh's
  if [ -f "$f" ]; then mkdir -p "$d/build" && cp -L "$f" "$d/build/libchdr_mini.so" && break; fi
done
cp "$root/LICENSE" "$d/" 2>/dev/null || true
# the licences of the code taken from other projects, where Help > About HIRAKU reads them
for f in $(grep -o '"third_party/[^"]*"' "$root/launcher/aboutdialog.cpp" | tr -d '"'); do
	[ -f "$root/$f" ] || continue   # one the package brings (DXVK's, above)
	mkdir -p "$d/$(dirname "$f")" && cp "$root/$f" "$d/$f"
done
# the oldest glibc the binaries and their libraries run with, from their symbol versions
glibc=$(find "$d/bin" -type f \( -perm -u+x -o -name '*.so*' \) -exec objdump -T {} + 2>/dev/null |
  grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1 || true)   # (bin/hiraku is a script)
cat > "$d/README.txt" <<EOF
HIRAKU for Linux (x86-64): BEMANI arcade machines, for your own game data.

Start the launcher:  bin/hiraku
(a script: it starts bin/hiraku.bin with the Qt in bin/qt)

Add a game: drop its files on the launcher's window, or File > Add game files, all at once:
the MAME ROM set (.zip, .7z, or a folder of its files) and its CHDs. HIRAKU tells the game from
the set's contents, copies what it needs into dumps/, and the files you dropped can go
afterwards. Nothing else to install: the Python that does the adding is in python/, and
7-Zip and the CHD reader in bin/. A game whose page says it needs a first boot (an
install, a clock or a test-mode setting) says what to do there.

Needs glibc $glibc or later. From the system: the graphics drivers, X11 and Wayland's client
libraries (libwayland-client: an X11 desktop has it too), fontconfig, D-Bus, glib and OpenSSL,
which every desktop has. SDL3 is in bin/lib. NFC card readers (PC/SC):
when the system has pcsc-lite and its daemon.

Your saves and the launcher's settings: in user/ beside this file when there is one (a portable
install: the folder runs as itself wherever it is moved, a USB stick included), else in
~/.local/share/hiraku and ~/.config/hiraku. Updating: the launcher says when a new release is out (Help > Check for updates), and
puts it in this folder itself; your games (dumps/), saves and settings stay as they are.

Command line (the launcher shows each game's under its right-click menu):
    bin/hiraku-sys573 --game ddr3mj --window      (DJ Main games: bin/hiraku-djmain, Twinkle:
                                                  bin/hiraku-twinkle; a build with one binary per
                                                  game has bin/hiraku-<game>)
    bin/hiraku-sys573 --game ddr3mj --window 3 --fullscreen --picture light   (F10: the CRT look)
    bin/hiraku-sys573 --help                      (all options)
Each run prints its frame times when it ends. Smooth frames (display sync, on by default): every
frame stays up the same number of refreshes on a 60, 120 or 240 Hz display (docs/PLAY.md,
"Frame pacing"); --display-sync off for a variable-refresh display.

Game data from a PC that has it already: tools/pack-data.py there (docs/PLAY.md).
EOF
if [ -d "$d/bin/resources" ]; then cat >> "$d/README.txt" <<'EOF'

Konami Python games (pop'n music 9-14; bin/hiraku-python --game popn9 on the command line):
add a game's folder (with or without its .py1) together with the folders bios (the arcade BIOS,
COH-H31000) and keys (the MagicGate key files), which HIRAKU can't ship; later games find them.
The default software renderer draws on the CPU; --renderer vk (or gl) on the GPU, with the
system's Vulkan or OpenGL drivers. Their e-AMUSEMENT runs offline inside the game by default;
bin/hiraku-eamuse --port 8080 is the same server for several PCs, which then run with
--eamuse http://that-pc:8080/ (docs/EAMUSE3.md).
EOF
fi
# What the updater needs (tools/update.py): the release this is (HK_VERSION, the tag; none for a
# build by hand, which the launcher then doesn't offer to update) and the package's own files
[ -n "${HK_VERSION:-}" ] && printf '%s\n' "${HK_VERSION#v}" > "$d/VERSION"
(cd "$d" && { find . \( -type f -o -type l \) ! -path './user/*' ! -path './dumps/*' | sed 's|^\./||'; echo MANIFEST.txt; } |
  LC_ALL=C sort -u > MANIFEST.txt)
# xz with a window wider than one binary: the binaries share most of their code,
# so the archive is a quarter of a .tar.gz (about 14 MB)
tar -C "$stage" -cf - hiraku | xz -9e -T1 --lzma2=preset=9e,dict=192MiB > "$out"
ls -la "$out"
