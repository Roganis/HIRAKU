// SPDX-License-Identifier: GPL-3.0-or-later
// The problem report's cleaner (launcher/cleaner.h): what says who the player is or where their
// files and servers are is gone from a log, and what a report is read for stays. Made-up logs.
//
//   test_cleaner      exit 0 when all hold
#include <cstdio>

#include <QString>

#include "cleaner.h"

// (forThisComputer() is the launcher's: it asks the system; the tests name their own)
static int failed = 0;

static void gone(report::Cleaner &c, const char *line, const char *secret) {
    const QString out = c.clean(QString::fromUtf8(line));
    const bool ok = !out.contains(QString::fromUtf8(secret), Qt::CaseInsensitive);
    printf("%s%s\n      -> %s\n", ok ? "ok    " : "FAIL  ", line, out.toUtf8().constData());
    failed += !ok;
}

static void same(report::Cleaner &c, const char *line) {
    const QString out = c.clean(QString::fromUtf8(line));
    const bool ok = out == QString::fromUtf8(line);
    printf("%s%s (kept as it is)\n", ok ? "ok    " : "FAIL  ", line);
    if (!ok) printf("      -> %s\n", out.toUtf8().constData());
    failed += !ok;
}

static void becomes(report::Cleaner &c, const char *line, const char *want) {
    const QString out = c.clean(QString::fromUtf8(line));
    const bool ok = out == QString::fromUtf8(want);
    printf("%s%s\n      -> %s\n", ok ? "ok    " : "FAIL  ", line, out.toUtf8().constData());
    if (!ok) printf("      wanted %s\n", want);
    failed += !ok;
}

int main() {
    report::Cleaner c;
    c.folders << "/home/kimiko/games/HIRAKU";
    c.homes << "/home/kimiko";
    c.names << "kimiko" << "arcade-pc";

    // folders and names
    gone(c, "reading /home/kimiko/games/HIRAKU/dumps/popn14/machine.txt", "kimiko");
    gone(c, "wine: Z:\\home\\kimiko\\.local\\share\\hiraku\\popn15", "kimiko");
    gone(c, "data in /mnt/big disk/roms/iidx16/data", "big disk");
    gone(c, "hostname arcade-pc, user Kimiko", "kimiko");
    gone(c, "hostname arcade-pc, user Kimiko", "arcade-pc");
    same(c, "loading /usr/lib/libSDL3.so.0");

    // cards, in either case, the same card the same number
    gone(c, "card E004010000ABCDEF read", "E004010000ABCDEF");
    gone(c, "card: e004010000abcdef (e-AMUSEMENT pass)", "e004010000abcdef");
    gone(c, "card: 012e4567890abcde (FeliCa)", "012e4567890abcde");
    report::Cleaner fresh;   // (cards are numbered through a report: a new one counts from 1)
    becomes(fresh, "E004010000ABCDEF then e004010000abcdef then E004010000000002", "#1 then #1 then #2");
    gone(c, "pcbid 0123456789ABCDEF0123 mac 00:1A:2b:3C:4d:5E", "0123456789ABCDEF0123");
    gone(c, "pcbid 0123456789ABCDEF0123 mac 00:1A:2b:3C:4d:5E", "1A:2b");

    // the network: addresses, and a server by name wherever it is written
    gone(c, "relay at 192.168.1.20:5740", "192.168");
    same(c, "relay at 127.0.0.1:5740, a cabinet at 10.57.0.2");
    gone(c, "peer fe80::1ff:fe23:4567:890a answered", "fe80");
    gone(c, "peer 2001:db8:0:0:0:0:0:1 answered", "2001:db8");
    same(c, "listening on ::1 and [::]");
    gone(c, "$ hiraku-pc --game popn15 -url my-server.example.org:8080 -smartea", "example.org");
    gone(c, "$ hiraku-pc --game popn15 -url my-server.example.org:8080/secretkey/ -smartea", "secretkey");
    gone(c, "$ hiraku-drmn10m --npu --eamuse http://my-server.example.org:8080/secretkey", "example.org");
    gone(c, "$ hiraku-drmn10m --npu --eamuse http://my-server.example.org:8080/secretkey", "secretkey");
    gone(c, "$ hiraku-popn14 -url 192.168.1.20:8080/secretkey", "secretkey");
    gone(c, "$ hiraku-popn14 --url=myserver", "myserver");
    gone(c, "e-AMUSEMENT: no answer from my-server.example.org:8080", "example.org");
    becomes(c, "$ hiraku-pc -url my-server.example.org:8080/key -p 1234", "$ hiraku-pc -url <server>:8080/... -p 1234");
    same(c, "see https://github.com/Roganis/HIRAKU/issues/10");
    same(c, "-url localhost:8080");

    // what a report is read for: times, a crash's place, C++ names, versions
    same(c, "12:34:56 frame 600");
    same(c, "crash in hiraku-popn14+0x1a2b3c (runtime/host/window.cpp:1122)");
    same(c, "terminate called after throwing an instance of 'std::out_of_range'");
    same(c, "hk::ea::Store::write: 0 bytes");
    same(c, "HIRAKU v0.9.2, build 253bbed 2026-11-10");
    same(c, "mods: smooth-notes: patch at 0x00123456 (4 bytes) in place");

    printf(failed ? "%d failed\n" : "all hold\n", failed);
    return failed ? 1 : 0;
}
