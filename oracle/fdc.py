#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""beatmania III's floppy controller: an access log read as commands, and two logs compared.

  oracle/fdc.py list LOG            the commands, a line each: seeks, reads and writes with the
                                    bytes that went (count and CRC-32) and the result bytes
  oracle/fdc.py compare A B         the same commands, data and results in the same order?
                                    (times and frames aside); the first places that differ

A log is what oracle/lua/fdc.lua writes under MAME (<run>/fdc.txt) and the runtime with
HK_FDC_LOG=FILE: "seconds frame R|W port value" for every access to ports 0x3f0-0x3f7. Reads of
the status register are folded away. A game's own retries show as a command sent again; what a
game writes to a disk depends on the clock (the volume's serial number, the scrambling of its
file), so two runs' written bytes are only equal when their clocks were (HK_RTC, and the log's
first line under MAME).
"""
import re
import sys
import zlib

NAMES = {0x03: "SPECIFY", 0x04: "SENSE DRIVE STATUS", 0x07: "RECALIBRATE", 0x08: "SENSE INTERRUPT STATUS", 0x0f: "SEEK",
         0x13: "CONFIGURE", 0x05: "WRITE DATA", 0x06: "READ DATA", 0x0a: "READ ID", 0x10: "VERSION", 0x14: "LOCK", 0x12: "PERPENDICULAR"}
LENGTH = {0x03: 3, 0x04: 2, 0x07: 2, 0x08: 1, 0x0f: 3, 0x13: 4, 0x05: 9, 0x06: 9, 0x0a: 2, 0x10: 1, 0x14: 1, 0x12: 2}


def events(path):
    """the log without its status register reads: (seconds, frame, 'R5' | 'W2' ..., value)"""
    for l in open(path):
        w = l.split()
        if len(w) != 5 or w[2] not in ("R", "W"):
            continue   # the clock line, the runtime's interrupt lines
        port = int(w[3], 16) & 7
        if w[2] == "R" and port == 4:
            continue
        yield float(w[0]), w[1], w[2] + str(port), int(w[4], 16)


def commands(path):
    """the log as (seconds, frame, text) lines: a command with its data and result, or a register write"""
    out = []
    ev = list(events(path))
    i, n = 0, len(ev)
    while i < n:
        t, f, k, v = ev[i]
        if k == "W5":   # a command: its bytes, the data written, then what is read back (data, result)
            op = v & 0x1f
            size = LENGTH.get(op, 1)
            cmd = [e[3] for e in ev[i:i + size] if e[2] == "W5"]
            i += len(cmd)
            sent = []
            while i < n and ev[i][2] == "W5" and op == 0x05 and len(sent) < 18432:
                sent.append(ev[i][3])
                i += 1
            got = []
            while i < n and ev[i][2] == "R5":
                got.append(ev[i][3])
                i += 1
            text = "%-22s %s" % (NAMES.get(op, "command %02x" % v), " ".join("%02x" % b for b in cmd))
            if sent:
                text += "  wrote %d bytes crc %08x" % (len(sent), zlib.crc32(bytes(sent)))
            if op == 0x06 and len(got) > 7:
                text += "  read %d bytes crc %08x" % (len(got) - 7, zlib.crc32(bytes(got[:-7])))
                got = got[-7:]
            if got:
                text += "  ->" + "".join(" %02x" % b for b in got[:16]) + (" ..." if len(got) > 16 else "")
            out.append((t, f, text))
        else:
            name = {"W2": "DOR", "W4": "DSR", "W7": "CCR", "R7": "DIR", "W0": "config", "W1": "config data", "R1": "config data", "R2": "DOR?",
                    "R0": "SRA", "R5": "FIFO read"}.get(k, k)
            out.append((t, f, "%s %s %02x" % (name, "=" if k[0] == "W" else "is", v)))
            i += 1
    return out


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "list":
        for t, f, text in commands(sys.argv[2]):
            print("%11.6f %7s  %s" % (t, f, text))
        return 0
    if len(sys.argv) == 4 and sys.argv[1] == "compare":
        a, b = commands(sys.argv[2]), commands(sys.argv[3])
        # what a game writes to the disk depends on its clock: the bytes aside, a write is a write
        plain = lambda text: re.sub(r"(wrote|read) (\d+) bytes crc \w+", r"\1 \2 bytes", text)
        different = clock = 0
        for k, (x, y) in enumerate(zip(a, b)):
            if x[2] == y[2]:
                continue
            if plain(x[2]) == plain(y[2]):
                clock += 1
                continue
            different += 1
            if different <= 8:
                print("line %d:\n  A %11.6f %s  %s\n  B %11.6f %s  %s" % (k + 1, x[0], x[1], x[2], y[0], y[1], y[2]))
        print("%d and %d lines; %d differ%s" % (len(a), len(b), different + abs(len(a) - len(b)),
                                              "; %d more in the bytes written or read back alone (the clock's)" % clock if clock else ""))
        return 1 if different or len(a) != len(b) else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
