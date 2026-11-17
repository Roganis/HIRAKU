#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# hiraku-server's own package (docs/SERVER.md, "Installing"): the hosted e-AMUSEMENT server for a
# Linux machine that serves many cabinets, apart from the desktop package (tools/package.sh, which
# leaves hiraku-server out).
#
#   tools/package-server.sh BUILD_DIR [OUT_DIR]     OUT_DIR/hiraku-server[-VERSION]-linux-x86_64-experimental.tar.xz
#                                                   (OUT_DIR: .; VERSION: HK_VERSION's, the release's tag less its v)
#
# "experimental" in the name while hiraku-server is a first version (docs/SERVER.md, "Experimental"),
# so that nobody takes it for a finished program or for one of the desktop packages.
#
# BUILD_DIR: a build that has made hiraku-server (CMake builds it when the system has SQLite 3; only
# that target is needed: `ninja -C BUILD_DIR hiraku-server`). The archive holds one folder,
# hiraku-server/:
#   bin/hiraku-server             stripped; linked to the system's SQLite 3 (libsqlite3.so.0)
#   share/hiraku-server/web/      the web pages (the program looks there: ../share/hiraku-server/web)
#   share/doc/hiraku-server/      docs/SERVER.md, the licence, the bundled headers' licences
#   deploy/                       the systemd units, the config and Caddy examples, the Dockerfile
#   README.txt
#   VERSION                       the release's number (as the desktop packages'; none without HK_VERSION)
# No game data and nothing of the desktop package.
set -euo pipefail
build=${1:?build dir}
outdir=${2:-.}
root=$(cd "$(dirname "$0")/.." && pwd)
. "$root/tools/package-files.sh"
prog=$build/hiraku-server
[ -x "$prog" ] || { echo "package-server: no $prog (ninja -C $build hiraku-server; it needs SQLite 3's headers)" >&2; exit 1; }
mkdir -p "$outdir"
release=${HK_VERSION:-}
release=${release#v}
case $release in *[!A-Za-z0-9._-]*) echo "package-server: HK_VERSION $HK_VERSION isn't a release's number" >&2; exit 1 ;; esac
name=hiraku-server${release:+-$release}-linux-x86_64-experimental.tar.xz
out=$(realpath "$outdir")/$name
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
d=$stage/hiraku-server
mkdir -p "$d/bin" "$d/share/hiraku-server" "$d/share/doc/hiraku-server" "$d/deploy"
strip -o "$d/bin/hiraku-server" "$prog"
copy_tracked_as "$root" "$d/share/hiraku-server/web" web
[ -f "$d/share/hiraku-server/web/index.html" ] || { echo "package-server: no web/index.html" >&2; exit 1; }
# (never a song names' pack: they are the owner's own, docs/SERVER.md "Song names")
if [ -n "$(find "$d" \( -name names -o -name '*.json' \) | head -1)" ]; then echo "package-server: a names folder or .json file in the package" >&2; exit 1; fi
doc=$d/share/doc/hiraku-server
cp "$root/docs/SERVER.md" "$root/LICENSE" "$doc/"
for lib in httplib nlohmann; do
  mkdir -p "$doc/third_party/$lib"
  cp "$root/third_party/$lib"/LICENSE* "$root/third_party/$lib/README.md" "$doc/third_party/$lib/"
done
cat > "$doc/third_party/SQLITE.txt" <<'EOF'
SQLite is in the public domain (https://sqlite.org/copyright.html). hiraku-server links the
system's own SQLite 3 library (libsqlite3.so.0); none is in this package.
EOF
# (each by name: deploy/ is what the repo has there, nothing a checkout holds besides)
for f in hiraku-server.service hiraku-server-backup.service hiraku-server-backup.timer server.conf.example \
         Caddyfile.example Dockerfile Dockerfile.dockerignore; do
  cp "$root/deploy/$f" "$d/deploy/"
done
# what the program needs of the system: glibc's and libstdc++'s newest symbol versions, its libraries
glibc=$(objdump -T "$d/bin/hiraku-server" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)
glibcxx=$(objdump -T "$d/bin/hiraku-server" | grep -o 'GLIBCXX_[0-9.]*' | sed 's/GLIBCXX_//' | sort -uV | tail -1 || true)
needed=$(objdump -p "$d/bin/hiraku-server" | awk '/NEEDED/ {print $2}' | tr '\n' ' ' | sed 's/ $//')
case " $needed " in *" libsqlite3.so.0 "*) ;; *) echo "package-server: hiraku-server doesn't link libsqlite3.so.0 ($needed)" >&2; exit 1 ;; esac
stdcxx_line="libstdc++ is linked into the program."
[ -n "$glibcxx" ] && stdcxx_line="And the system's C++ library (Debian/Ubuntu: libstdc++6) with GLIBCXX_$glibcxx or later."
version=$("$d/bin/hiraku-server" --version 2>/dev/null || echo "hiraku-server")
cat > "$d/README.txt" <<EOF
$version${release:+ (HIRAKU $release)} for Linux (x86-64): HIRAKU's hosted e-AMUSEMENT server, for many
cabinets on a network. Cards, players and scores for every game HIRAKU serves, kept in one SQLite
database, with accounts, arcades and their machines (each with its own key), a JSON API and web pages.

EXPERIMENTAL: this is its first version. What it doesn't do yet, and what wasn't tried, is listed at
the top of share/doc/hiraku-server/SERVER.md ("Experimental"): read it before you run it for others.
Card PINs are kept in clear in the database and its backups; the games' port needs machine keys
before it faces the internet. Problems: https://github.com/Roganis/HIRAKU/issues

Needs glibc $glibc or later and the system's SQLite 3 library: Debian/Ubuntu: libsqlite3-0, Fedora:
sqlite-libs, Arch: sqlite.
$stdcxx_line
The libraries it loads: $needed.

A server running, by hand (the folder anywhere; the first admin's password is printed once):
    sudo apt install libsqlite3-0
    tar -xJf $name && cd hiraku-server
    mkdir -p data
    bin/hiraku-server serve --db data/server.db
    xdg-open http://127.0.0.1:8081/     (the web pages; the games' port is 8080)

As a service (systemd, its own user, daily backups), behind Caddy for HTTPS, or as a container:
share/doc/hiraku-server/SERVER.md, "Installing", with the files in deploy/.

Never put the games' port behind an HTTP proxy, nor the database, its key and cabinet files or the
backups where anyone else can read them.

HIRAKU is free software under the GNU GPL version 3 or later (share/doc/hiraku-server/LICENSE). It
uses cpp-httplib (MIT) and JSON for Modern C++ (MIT): their licences are in
share/doc/hiraku-server/third_party/.
EOF
[ -n "$release" ] && printf '%s\n' "$release" > "$d/VERSION"
check_build_paths "$root" "$d/bin"
(cd "$d" && find . -type f | sed 's|^\./||' | LC_ALL=C sort > MANIFEST.txt)
tar -C "$stage" --owner=0 --group=0 -cf - hiraku-server | xz -9 -T1 > "$out"
ls -la "$out"
