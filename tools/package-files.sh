# SPDX-License-Identifier: GPL-3.0-or-later
# For tools/package.sh and tools/package-win.sh (sourced): the repo's folders as a package takes them.
#
#   copy_tracked ROOT DEST FOLDER...     ROOT/FOLDER's files into DEST/FOLDER
#   copy_tracked_as ROOT DEST FOLDER     ROOT/FOLDER's files into DEST itself
#
# Only what git tracks: a checkout that is worked in holds more (a mod being made under games/, notes,
# a dump beside a script), and none of that is the package's. Files as they are in the folder now (a
# tracked file changed and not committed goes as changed; one deleted is left out). Where ROOT isn't
# a git checkout (a release's source archive, unpacked), the folders whole: such a copy has nothing
# else in it.

# The tracked files under ROOT's folders, each ended by a NUL; status 1 when ROOT isn't a checkout
hk_tracked() {
  local root=$1; shift
  if [ ! -e "$root/.git" ]; then return 1; fi
  # (safe.directory: a checkout owned by another user, as in a container, is still this one)
  git -c safe.directory="$root" -C "$root" ls-files -z -- "$@" | while IFS= read -r -d '' f; do
    if [ -e "$root/$f" ] || [ -L "$root/$f" ]; then printf '%s\0' "$f"; fi
  done
  [ "${PIPESTATUS[0]}" = 0 ] || { echo "package: git can't list $root's files" >&2; exit 1; }
}

copy_tracked() {
  local root=$1 dest=$2 list; shift 2
  mkdir -p "$dest"
  list=$(mktemp)
  if hk_tracked "$root" "$@" > "$list"; then
    [ -s "$list" ] || { rm -f "$list"; echo "package: git tracks nothing under $* in $root" >&2; exit 1; }
    tar -C "$root" --null -T "$list" -cf - | tar -C "$dest" -xf -
  else
    (cd "$root" && cp -r "$@" "$dest/")
  fi
  rm -f "$list"
}

copy_tracked_as() {
  local root=$1 dest=$2 folder=$3 list depth
  mkdir -p "$dest"
  list=$(mktemp)
  if hk_tracked "$root" "$folder" > "$list"; then
    [ -s "$list" ] || { rm -f "$list"; echo "package: git tracks nothing under $folder in $root" >&2; exit 1; }
    depth=$(awk -F/ '{print NF}' <<< "${folder%/}")
    tar -C "$root" --null -T "$list" -cf - | tar -C "$dest" --strip-components="$depth" -xf -
  else
    cp -r "$root/$folder/." "$dest/"
  fi
  rm -f "$list"
}

#   check_build_paths ROOT BIN     after BIN (the package's bin/) is filled
#
# A binary that names the PC it was built on. HK_ROOT (CMakeLists.txt: the checkout's path, which a
# build to work with looks for dumps/ in) is in none of a release's: with HK_VERSION set, the
# release's number, one that has it stops the package. A package made by hand is told which of its
# files name this PC's folders (the checkout, the home folder: HK_ROOT, a library built here with its
# own paths in it), since such a package is for one's own PCs.
check_build_paths() {
  local root=$1 bin=$2 f named=""
  if [ -n "${HK_VERSION:-}" ]; then
    while IFS= read -r -d '' f; do
      if grep -qxF "$root" < <(strings -a "$f"); then   # (not a pipe: grep leaving early would fail it under pipefail)
        echo "package: $f has this checkout's path in it (HK_ROOT: built with HK_GAME_BINARIES on?); not a release's" >&2
        exit 1
      fi
    done < <(find "$bin" -type f -print0)
  fi
  [ -z "${GITHUB_ACTIONS:-}" ] || return 0
  named=$(grep -rlF -e "$root" ${HOME:+-e "$HOME"} "$bin" 2>/dev/null | sed "s|^$bin/||" | sort | tr '\n' ' ' || true)
  [ -z "$named" ] || echo "package: these name this PC's folders ($root${HOME:+, $HOME}), which whoever gets the package can read: $named" >&2
}
