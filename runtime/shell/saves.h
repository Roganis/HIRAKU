// SPDX-License-Identifier: GPL-3.0-or-later
// A machine's saved memories (its NVRAM: clock chip, backup RAM, flash, security cassette) written
// while a game is played, not only when HIRAKU is closed: a crash, a kill or a power cut then costs
// the last seconds, not the session (scores, settings, an installed game).
//
// Two things make that safe:
//
//   A set is the folder's whole or not at all. The files of one moment are a state the machine was
//   in, as after a real cabinet's power cut at that moment, which its game is written to survive.
//   Some files of one moment beside others of another are a state it never was in (a clock chip
//   saying "installed" beside a flash chip from before). write_set() puts each file beside its
//   place, then a list of them, then renames them in; recover() at the next start finishes a set
//   whose list was written and drops one cut before.
//
//   The game never waits for the disk. A core hands the set to writer(), whose thread writes it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hk::saves {

struct File {
    std::string name;   // in the save folder
    std::string data;   // the whole file
};

// For "has it changed since it was written": 64 bits over the content (2 MiB in ~0.3 ms)
uint64_t hash(const void *data, size_t n);
inline uint64_t hash(const std::string &d) { return hash(d.data(), d.size()); }

// The set written into `dir` as one: false if it couldn't be (a full disk: the files from before stay)
bool write_set(const std::string &dir, const std::vector<File> &set);
// Before a save folder is read: a set cut short is finished (its list was written) or dropped
void recover(const std::string &dir);

// Sets written in the order given, on a thread of their own. Sets that come faster than the disk
// takes them are joined into one (the latest moment), so nothing piles up
class Writer {
public:
    Writer();
    ~Writer();   // (what is queued is written first)
    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;
    void write(std::string dir, std::vector<File> set);
    void drain();              // returns once everything given is written (before a write of one's own)
    unsigned failed() const;   // how many sets couldn't be written so far: their files aren't the folder's

private:
    struct State;
    State *s_;
};
Writer &writer();   // the program's one

}  // namespace hk::saves
