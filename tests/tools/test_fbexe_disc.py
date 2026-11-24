#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/fbexe.py's Disc (what tools/ingest.py reads a Firebeat program disc with), on discs made up
here: plain 2048-byte sectors, raw 2352-byte sectors of mode 1 (data after the 16-byte header) and of
mode 2, a CD-ROM XA disc (data after the 8-byte subheader too), each alone and through a sheet. A
mode 2 disc was once refused ("expected MODE1"). No game's data."""
import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import fbexe  # noqa: E402

N = 24
failed = 0


def check(ok, what):
    global failed
    print(("ok    " if ok else "FAIL  ") + what)
    failed += not ok


def data_of(lba):
    return bytes((lba + 1) & 0xff if k % 2 == 0 else (k >> 1) & 0xff for k in range(2048))


def disc(mode):
    out = bytearray()
    for lba in range(N):
        if mode == 0:
            out += data_of(lba)
            continue
        raw = bytearray(2352)
        raw[1:11] = b"\xff" * 10
        raw[12:16] = bytes((0, 2, lba, mode))
        if mode == 2:
            raw[16:24] = bytes((0, 0, 8, 0)) * 2
            raw[24:24 + 2048] = data_of(lba)
        else:
            raw[16:16 + 2048] = data_of(lba)
        out += raw
    return bytes(out)


def main():
    with tempfile.TemporaryDirectory() as d:
        for mode, name in ((0, "plain.iso"), (1, "m1.bin"), (2, "m2.bin")):
            with open(os.path.join(d, name), "wb") as fh:
                fh.write(disc(mode))
        for name, bin_name, kind in (("m1.cue", "m1.bin", "MODE1/2352"), ("m2.cue", "m2.bin", "MODE2/2352")):
            with open(os.path.join(d, name), "w") as fh:
                fh.write(f'FILE "{bin_name}" BINARY\n  TRACK 01 {kind}\n    INDEX 01 00:00:00\n')
        for name, what in (("plain.iso", "2048-byte sectors"), ("m1.bin", "raw mode 1, the file alone"),
                           ("m2.bin", "raw mode 2, the file alone"), ("m1.cue", "raw mode 1, by its sheet"),
                           ("m2.cue", "raw mode 2, by its sheet")):
            try:
                with fbexe.Disc(os.path.join(d, name)) as dk:
                    ok = dk.nsectors == N and all(dk.read(lba) == data_of(lba) for lba in range(N))
                    ok = ok and dk.read(5, 3) == data_of(5) + data_of(6) + data_of(7)
            except ValueError as e:
                print("     ", e)
                ok = False
            check(ok, what)
        # a mode this reader doesn't know is still refused, by name
        bad = bytearray(disc(1))
        bad[15] = 3
        with open(os.path.join(d, "m3.bin"), "wb") as fh:
            fh.write(bad)
        try:
            fbexe.Disc(os.path.join(d, "m3.bin"))
            check(False, "an unknown mode refused")
        except ValueError:
            check(True, "an unknown mode refused")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
