// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Disk read-ahead for a disk or disc image the emulation reads on its own thread.
//
// A read the emulation waits for stops the machine: on a slow drive (the N4020's eMMC, a USB
// stick, a 5400 rpm disk) a cold one is a frozen frame and a break in the sound. A thread of its
// own follows the emulation's reads through the image and reads the next few MB before the game
// asks for them, in small pieces, one at a time: the game's next read is then in the system's
// cache, and if it comes while a piece is still on its way it waits for that one piece only.
//
// What it replaced (still there with HK_DISK_AHEAD=0): a posix_fadvise(WILLNEED) of 8-16 MB asked
// as each READ began. That range held the game's own read too, and on the eMMC the read then waited
// for most of the 16 MB: 100-300 ms instead of 1-20 (measured 2026-11-25).
//
// The game's data still comes from its own read (a pread, or the mapped image): this thread only
// reads into a buffer of its own, so what the machine computes is the same with it on or off; only
// how long the host waits differs.
//
// HK_DISK_AHEAD=0: off (the old hints); HK_DISK_AHEAD=N: up to N MiB ahead (default 4). Each
// image's line at exit says what it did and how long the emulation waited on that image;
// HK_DISK_AHEAD_LOG=1 adds a line for each stretch the game read before it went elsewhere.
#include <cstdint>
#include <memory>
#include <string>

namespace hk {

class DiskAhead {
public:
    struct Stats {
        uint64_t reads = 0, bytes = 0;        // the emulation's reads of the image
        uint64_t hit_bytes = 0;               // ... of those bytes, read ahead already
        uint64_t jumps = 0;                   // reads somewhere else than where the last ones went
        uint64_t ahead_bytes = 0;             // read by the thread
        uint64_t waited_ns = 0;               // the emulation's time in reads over 4 ms, in all
        uint64_t longest_ns = 0, longest_at = 0;
        bool longest_hit = false;
        uint64_t over_4ms = 0, over_16ms = 0, over_50ms = 0;
    };

    // window: bytes to stay ahead of the emulation's last read (0: the switch's); piece: the size of
    // each read. on: HK_DISK_AHEAD unless told. Off, nothing is read; the reads are still timed.
    DiskAhead(const std::string &path, uint64_t size, uint64_t window = 0, uint64_t piece = 256 << 10, int on = -1);
    ~DiskAhead();
    DiskAhead(const DiskAhead &) = delete;
    DiskAhead &operator=(const DiskAhead &) = delete;

    static bool switched_on();   // HK_DISK_AHEAD isn't 0
    bool on() const;

    // Around each read the emulation makes itself, [off, off + n) of the image: before() as it
    // starts, after() when it has its bytes (on the emulation's thread, one read at a time)
    void before(uint64_t off, uint64_t n);
    void after();

    Stats stats() const;
    std::string report() const;   // the line printed at exit (or when this is destroyed)
    void wait_caught_up() const;  // (tests) until the thread has read all it wants to for now

    struct State;

private:
    std::shared_ptr<State> s_;
};

}  // namespace hk
