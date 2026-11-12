# SPDX-License-Identifier: GPL-3.0-or-later
# The release scripts' downloads (sourced by tools/release-deps-linux.sh and -windows.sh): each
# file is checked against its SHA-256 in tools/release-sums.txt before anything is unpacked, built
# or packaged from it, so a file changed at its source, or on the way, stops the build instead of
# ending up in a release. A file kept from an earlier run (the workflow caches them) is checked
# again, and fetched again if it isn't the one named.
#
#   hk_fetch URL FILE     FILE: where it is kept
#
# A file with no line in the list is taken as it came, and said: only for a version that follows
# the build machine's (the Windows build's qtbase must be the host Qt's). A new version of anything
# else gets its line with the version's change: sha256sum of the file, checked against what its
# publisher gives where there is one.
hk_fetch() {
  local url=$1 file=$2 name sums want got
  name=$(basename "$file")
  sums=${HK_RELEASE_SUMS:-$(dirname "${BASH_SOURCE[0]}")/release-sums.txt}
  want=$(awk -v n="$name" '$2 == n { print $1 }' "$sums")
  if [ -f "$file" ] && [ -n "$want" ] && [ "$(sha256sum "$file" | cut -d' ' -f1)" != "$want" ]; then
    echo "release-fetch: the $name kept here isn't the file tools/release-sums.txt names: fetched again" >&2
    rm -f "$file"
  fi
  if [ ! -f "$file" ]; then
    curl -fL --retry 3 -o "$file.part" "$url"
    if [ -n "$want" ]; then
      got=$(sha256sum "$file.part" | cut -d' ' -f1)
      if [ "$got" != "$want" ]; then
        echo "release-fetch: $url is not the file tools/release-sums.txt names ($name: SHA-256 $got, not $want): not used" >&2
        rm -f "$file.part"
        return 1
      fi
    fi
    mv "$file.part" "$file"
  fi
  [ -n "$want" ] || echo "release-fetch: $name has no SHA-256 in tools/release-sums.txt: taken as it came" >&2
}
