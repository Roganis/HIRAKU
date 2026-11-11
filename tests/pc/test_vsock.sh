#!/bin/bash
# runtime/pc/vsock.c between two processes on one relay (tests/pc/test_vsock.c), under Wine on Linux.
#   tests/pc/test_vsock.sh BUILD_DIR      (BUILD_DIR: with hiraku-eamuse and pc/test_vsock.exe built:
#                                          ninja hiraku-eamuse hiraku-pc-test-vsock)
# Needs port 5740 free (the relay's). WINEPREFIX as you have it (a scratch one is made otherwise).
set -u
B=$(realpath "${1:-build/cmake}")
T=$(mktemp -d)
trap 'kill $E 2>/dev/null; rm -rf "$T"' EXIT
export WINEPREFIX=${WINEPREFIX:-$T/wine} WINEDEBUG=-all
wineboot -i > /dev/null 2>&1   # the prefix made now: two programs making it at once start seconds late
"$B/hiraku-eamuse" --port 0 --data "$T/data" --quiet > "$T/server.log" 2>&1 &
E=$!
sleep 1
export HK_PC_RELAY=127.0.0.1:5740
wine "$B/pc/test_vsock.exe" host > "$T/host.log" 2>&1 &
H=$!
sleep 3
wine "$B/pc/test_vsock.exe" client > "$T/client.log" 2>&1
C=$?
wait $H
HR=$?
# a relay address where something takes the connection and says nothing: the game's call comes back
python3 -c 'import socket, time
s = socket.socket(); s.bind(("127.0.0.1", 0)); s.listen(1); print(s.getsockname()[1], flush=True)
c, _ = s.accept(); time.sleep(60)' > "$T/silent.port" &
S=$!
sleep 1
HK_PC_RELAY=127.0.0.1:$(cat "$T/silent.port") timeout 60 wine "$B/pc/test_vsock.exe" silent > "$T/silent.log" 2>&1
SR=$?
kill $S 2>/dev/null
echo "-- host"; grep -E "^(ok|FAIL)" "$T/host.log"
echo "-- client"; grep -E "^(ok|FAIL)" "$T/client.log"
echo "-- a silent relay"; grep -E "^(ok|FAIL)" "$T/silent.log"
[ $C -eq 0 ] && [ $HR -eq 0 ] && [ $SR -eq 0 ] && echo "test_vsock: all passed" || { echo "test_vsock: FAILED"; cat "$T/server.log"; exit 1; }
