#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/release-fetch.sh: a download is used only if it is the file tools/release-sums.txt names.
# Local files stand for the servers (curl's file:// URLs).
#   test_release_fetch.sh ROOT     ROOT: the checkout
set -u
root=${1:?the checkout}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failed=0
check() {   # WHAT, then the test's command
  local what=$1; shift
  if "$@"; then echo "ok    $what"; else echo "FAIL  $what"; failed=$((failed + 1)); fi
}
mkdir "$work/server" "$work/kept"
echo "the real file" > "$work/server/good.tar.gz"
echo "another file under its name" > "$work/server/swapped.tar.gz"
echo "a file nobody listed" > "$work/server/unlisted.tar.gz"
good=$(sha256sum "$work/server/good.tar.gz" | cut -d' ' -f1)
printf '# a comment\n%s  good.tar.gz\n%s  swapped.tar.gz\n' "$good" "$good" > "$work/sums.txt"
export HK_RELEASE_SUMS=$work/sums.txt
. "$root/tools/release-fetch.sh"

check "the file the list names is fetched and kept" hk_fetch "file://$work/server/good.tar.gz" "$work/kept/good.tar.gz"
check "... whole" cmp -s "$work/server/good.tar.gz" "$work/kept/good.tar.gz"

refused() { ! hk_fetch "file://$work/server/swapped.tar.gz" "$work/kept/swapped.tar.gz" 2> "$work/err"; }
check "another file under a listed name is refused" refused
check "... said, with both sums" grep -q "not the file tools/release-sums.txt names.*$good" "$work/err"
nothing_left() { [ ! -e "$work/kept/swapped.tar.gz" ] && [ ! -e "$work/kept/swapped.tar.gz.part" ]; }
check "... and nothing of it is left" nothing_left

# a kept file that was changed since (a cache): fetched again
echo "changed in the cache" > "$work/kept/good.tar.gz"
check "a kept file that isn't the one named is fetched again" hk_fetch "file://$work/server/good.tar.gz" "$work/kept/good.tar.gz"
check "... and is the real one" cmp -s "$work/server/good.tar.gz" "$work/kept/good.tar.gz"
# ... and if its source no longer has the real one either: refused, the changed one gone
echo "changed in the cache" > "$work/kept/good.tar.gz"
gone() { ! hk_fetch "file://$work/server/swapped.tar.gz" "$work/kept/good.tar.gz" 2>/dev/null && [ ! -e "$work/kept/good.tar.gz" ]; }
check "a changed kept file is never used, even when the real one can't be had" gone

# a kept, right file: no download at all (the source may be gone)
cp "$work/server/good.tar.gz" "$work/kept/good.tar.gz"
check "a kept file that is the one named needs no source" hk_fetch "file://$work/server/not-there" "$work/kept/good.tar.gz"

unlisted() { hk_fetch "file://$work/server/unlisted.tar.gz" "$work/kept/unlisted.tar.gz" 2> "$work/err" && grep -q "no SHA-256" "$work/err"; }
check "a file with no line in the list is taken, and said" unlisted

# the real list: every line a sum and a name, no name twice, and the names the scripts fetch are in it
unset HK_RELEASE_SUMS
sums=$root/tools/release-sums.txt
well_formed() { ! grep -v '^#' "$sums" | grep -vqE '^[0-9a-f]{64}  [^ ]+$'; }
check "tools/release-sums.txt: each line a SHA-256, two spaces, a name" well_formed
once() { [ -z "$(grep -v '^#' "$sums" | awk '{ print $2 }' | sort | uniq -d)" ]; }
check "... no name twice" once
[ $failed = 0 ] && echo "release fetch ok" || echo "$failed FAILED"
exit $((failed != 0))
