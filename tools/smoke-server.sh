#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# hiraku-server's package (tools/package-server.sh) tried as it will be downloaded: unpacked into a
# temporary folder, its program run from there with its own web pages, no build or checkout files.
#
#   tools/smoke-server.sh PACKAGE        PACKAGE: hiraku-server[-VERSION]-linux-x86_64-experimental.tar.xz
#
# Checked: the package's files; that it says it is experimental (its name, README.txt, serve's
# start, --help); --version; serve --config on a new database with the package's
# web folder; health; the home page and /api/v1/server answer; the ten transcripts
# (tests/eamuse/test_transcripts.py) against the packaged program; backup --dir; SIGTERM: exit 0
# and the clean stop's line. Needs python3 (the transcripts and the HTTP checks).
set -u
archive=$(realpath "${1:?the package, hiraku-server-...-linux-x86_64-experimental.tar.xz}")
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
pid=""
cleanup() {
  if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then kill -KILL "$pid" 2>/dev/null; fi
  rm -rf "$work"
}
trap cleanup EXIT
failed=0
ok() { echo "ok    $1"; }
bad() { echo "FAIL  $1"; failed=$((failed + 1)); }

tar -xJf "$archive" -C "$work" || { echo "smoke-server: cannot unpack $archive"; exit 1; }
p=$work/hiraku-server
srv=$p/bin/hiraku-server
for f in bin/hiraku-server share/hiraku-server/web/index.html share/doc/hiraku-server/SERVER.md share/doc/hiraku-server/LICENSE \
         share/doc/hiraku-server/third_party/httplib/LICENSE share/doc/hiraku-server/third_party/nlohmann/LICENSE.MIT \
         share/doc/hiraku-server/third_party/SQLITE.txt deploy/hiraku-server.service deploy/hiraku-server-backup.service \
         deploy/hiraku-server-backup.timer deploy/server.conf.example deploy/Caddyfile.example deploy/Dockerfile README.txt; do
  [ -e "$p/$f" ] || bad "no $f in the package"
done
extra=$(cd "$p" && find . -type f \( -name '*.chd' -o -name '*.bin' -o -name '*.so*' -o -name 'hiraku' -o -name 'hiraku-eamuse' \) | head -3)
[ -z "$extra" ] && ok "the package's files (no library, game data or desktop program in it)" || bad "the package holds $extra"
names=$(cd "$p" && find . \( -name names -o -name '*.json' \) | head -3)
[ -z "$names" ] && ok "no song names in the package (no names folder, no .json pack)" || bad "the package holds $names"

# a first version, and said so wherever someone meets it (docs/SERVER.md, "Experimental")
[[ $(basename "$archive") == hiraku-server-*experimental.tar.xz ]] && ok "the package's name says experimental" || bad "the package's name: $(basename "$archive")"
grep -q EXPERIMENTAL "$p/README.txt" && ok "README.txt says experimental" || bad "README.txt doesn't say experimental"
grep -q '^## Experimental' "$p/share/doc/hiraku-server/SERVER.md" && ok "SERVER.md has its Experimental part" || bad "SERVER.md has no Experimental part"
"$srv" --help 2>&1 | grep -qi experimental && ok "--help says experimental" || bad "--help doesn't say experimental"

out=$("$srv" --version 2>&1); st=$?
[ $st = 0 ] && [[ $out == hiraku-server* ]] && ok "--version: $out" || bad "--version: ended with $st: $out"

free_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }
port=$(free_port); api=$(free_port)
mkdir -p "$work/state"
cat > "$work/server.conf" <<EOF
# made for this test (deploy/server.conf.example's names)
db = $work/state/server.db
port = $port
bind = 127.0.0.1
api = 127.0.0.1:$api
no-relay = true
api-insecure-cookies = true
EOF
"$srv" serve --config "$work/server.conf" > "$work/serve.log" 2>&1 &
pid=$!
for _ in $(seq 100); do
  "$srv" health --api "127.0.0.1:$api" > /dev/null 2>&1 && break
  kill -0 "$pid" 2>/dev/null || break
  sleep 0.1
done
out=$("$srv" health --api "127.0.0.1:$api" 2>&1); st=$?
[ $st = 0 ] && ok "serve --config started; health: $out" || bad "health: ended with $st: $out; serve said: $(tail -3 "$work/serve.log" | tr '\n' ' ')"
grep -q "share/hiraku-server/web" "$work/serve.log" && ok "serve took the package's web folder" || bad "serve's web folder: $(grep -i web "$work/serve.log" | head -1)"

http_get() {   # PATH: the status and the body's first line
  python3 - "$api" "$1" <<'PY'
import http.client, sys
c = http.client.HTTPConnection('127.0.0.1', int(sys.argv[1]), timeout=10)
c.request('GET', sys.argv[2])
r = c.getresponse()
body = r.read().decode(errors='replace')
print(r.status, body.replace('\n', ' ')[:4000])
PY
}
out=$(http_get /); [[ $out == "200 "*"<html"* || $out == "200 "*"<!doctype"* || $out == "200 "*"<!DOCTYPE"* ]] && ok "the home page answers (200, HTML)" || bad "GET /: $out"
out=$(http_get /api/v1/server); [[ $out == "200 {"*'"signup"'* ]] && ok "/api/v1/server answers (signup: $(echo "$out" | grep -o "\"signup\":\"[a-z]*\"" | cut -d\" -f4))" || bad "GET /api/v1/server: $out"

out=$(python3 "$root/tests/eamuse/test_transcripts.py" "$srv" 2>&1); st=$?
n=$(echo "$out" | grep -c '^ok ')
[ $st = 0 ] && [ "$n" -ge 10 ] && ok "the $n transcripts against the packaged program" || bad "the transcripts: ended with $st: $(echo "$out" | grep -v '^ok ' | head -5 | tr '\n' ' ')"

out=$("$srv" backup --dir "$work/state/backups" --keep 3 --db "$work/state/server.db" 2>&1); st=$?
nb=$(find "$work/state/backups" -name 'hiraku-server-*.db' 2>/dev/null | wc -l)
[ $st = 0 ] && [ "$nb" = 1 ] && ok "backup --dir while serving: $(echo "$out" | tail -1)" || bad "backup --dir: ended with $st ($nb files): $out"

kill -TERM "$pid"
st=0; wait "$pid" || st=$?
pid=""
grep -q "stopped (SIGTERM)" "$work/serve.log" && [ $st = 0 ] && ok "SIGTERM: exit 0, $(grep -o 'stopped (SIGTERM).*' "$work/serve.log")" \
  || bad "SIGTERM: ended with $st: $(tail -2 "$work/serve.log" | tr '\n' ' ')"
# (read once serve has stopped: its start-up lines reach the file together, after the API answers)
grep -q "EXPERIMENTAL" "$work/serve.log" && ok "serve said at its start that it is experimental" || bad "serve's start didn't say experimental"
[ ! -e "$work/state/server.db-wal" ] && ok "no WAL left after the stop" || bad "the WAL is left after the stop"
if grep -qE 'password [A-Za-z0-9]{12,}' "$work/serve.log"; then ok "the first admin's password was printed at the first start"
else bad "no first admin's password in serve's log"; fi

[ "$failed" = 0 ] && echo "smoke-server: all fine" || echo "smoke-server: $failed failed"
exit $((failed > 0))
