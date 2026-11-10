#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/package-files.sh: a package takes what git tracks in the repo's folders, and nothing a
# checkout holds besides. A made-up checkout with a file of each kind.
#
#   test_package_files.sh HIRAKU_ROOT      exit 0 when all hold
set -uo pipefail
root=${1:?the HIRAKU folder}
. "$root/tools/package-files.sh"
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
failed=0
check() {   # WHAT COMMAND...
  local what=$1; shift
  if "$@"; then echo "ok    $what"; else echo "FAIL  $what"; failed=$((failed + 1)); fi
}
absent() { [ ! -e "$1" ] && [ ! -L "$1" ]; }

r=$t/repo
mkdir -p "$r/games/a/mods/mine" "$r/games/b" "$r/docs" "$r/oracle/scripts/sub" "$r/tools/data"
git -C "$r" init -q
echo 'core = "x"' > "$r/games/a/game.toml"
echo text > "$r/games/a/a name with spaces.txt"
echo gone > "$r/games/b/deleted.toml"
echo kept > "$r/games/b/changed.toml"
ln -s game.toml "$r/games/a/link.toml"
echo doc > "$r/docs/PLAY.md"
echo s > "$r/oracle/scripts/one.his"
echo s > "$r/oracle/scripts/sub/two.his"
echo d > "$r/tools/data/sets.json"
printf '*.bin\n' > "$r/.gitignore"
git -C "$r" add -A
git -C "$r" -c user.name=t -c user.email=t@example.invalid commit -q -m files
# what a checkout that is worked in holds besides
echo 'a mod being made' > "$r/games/a/mods/mine/mod.toml"   # not tracked
echo dump > "$r/games/a/program.bin"                        # ignored
echo notes > "$r/oracle/scripts/notes.txt"                  # not tracked
echo scratch > "$r/tools/data/scratch.json"                 # not tracked
rm "$r/games/b/deleted.toml"                                # tracked, deleted here
echo 'changed here' > "$r/games/b/changed.toml"             # tracked, changed and not committed

d=$t/package
copy_tracked "$r" "$d" games docs
copy_tracked_as "$r" "$d/scripts" oracle/scripts
copy_tracked "$r" "$d" tools/data
check "a tracked file is in the package" test -f "$d/games/a/game.toml"
check "... one with spaces in its name" test -f "$d/games/a/a name with spaces.txt"
check "... a link, as a link" test -L "$d/games/a/link.toml"
check "... a second folder's" test -f "$d/docs/PLAY.md"
check "a file that isn't tracked is not" absent "$d/games/a/mods"
check "... nor an ignored one" absent "$d/games/a/program.bin"
check "a tracked file deleted in the checkout is left out" absent "$d/games/b/deleted.toml"
check "a tracked file changed in the checkout goes as it is now" grep -q 'changed here' "$d/games/b/changed.toml"
check "a folder under another name: its files at the top" test -f "$d/scripts/one.his"
check "... and its folders" test -f "$d/scripts/sub/two.his"
check "... without what isn't tracked" absent "$d/scripts/notes.txt"
check "a folder two deep keeps its place" test -f "$d/tools/data/sets.json"
check "... without what isn't tracked" absent "$d/tools/data/scratch.json"

# not a checkout (a source archive): the folders whole
s=$t/source
mkdir -p "$s/games/a" "$s/oracle/scripts"
echo 'core = "x"' > "$s/games/a/game.toml"
echo s > "$s/oracle/scripts/one.his"
d2=$t/package2
copy_tracked "$s" "$d2" games
copy_tracked_as "$s" "$d2/scripts" oracle/scripts
check "not a git checkout: the folder whole" test -f "$d2/games/a/game.toml"
check "... and under another name" test -f "$d2/scripts/one.his"

if [ "$failed" = 0 ]; then echo "all hold"; else echo "$failed failed"; fi
exit $((failed != 0))
