// SPDX-License-Identifier: GPL-3.0-or-later
// Turns of a processor's waiting loop counted instead of run: the part that knows no core.
//
// A sound board's 68000 goes round one main loop, looking in its own RAM for something to do; a
// turn that finds nothing leaves the processor and memory exactly as they were (loop_turns.h).
// HIRAKU's cores run a processor alone for a timeslice: nothing else can change what the turn
// reads before the timeslice ends. So once the loop's head is reached with the processor's state
// as a learned turn began with, and memory holding what that turn read, the whole turns left in
// the timeslice are taken at once: their cycles off the count, nothing else, which is all they
// would have done. The last, partial turn of a timeslice and the first of the next are run as
// ever, so an interrupt or a command arriving between timeslices is met at the cycle it always
// was.
//
// Nothing here knows a program: the head is found by watching the instructions' addresses
// repeat, a turn is learned from its own bus cycles, and one that touches a device, takes an
// interrupt or changes a byte is never counted.
//
// It pays where the loop mostly waits: Twinkle's sound board, three turns in four. Firebeat's
// board (Musashi, runtime/spu) was tried on 2026-11-15 and left as it is: its loop works on its
// voices every turn, and one cycle in a hundred could be counted.
//
// A core gives `Counter` a type `Cpu` with, for its processor `Cpu::Core &c`:
//   pc(c)            the instruction about to run (the core calls at(c) between two instructions)
//   left(c)          the cycles left in the timeslice, by reference
//   run(c)           a number that changes with every timeslice
//   snap(c, bytes)   everything the core keeps of the processor but left(c)
//   plain(c, a)      bit 0: the bus word at a is memory's own; bit 1: memory that can be written
//   word(c, a), put(c, a, v)   that memory, read and written with nothing else happening
//   noting(c, on)    while on, the core tells every bus cycle to note()
//   watch(c, pc, mask)   the core calls at(c) for instructions whose address agrees with pc in
//                        mask's bits (mask 0: all; pc 1: none)
//   name             for the messages
// and calls frame(c) once a frame.
//
// HK_68K_LOOPS=0: every turn run (the cores then attach nothing). =check: the turns that would be
// counted are run, and the processor and memory compared with what counting them would have left
// (the program stops on a difference). HK_68K_STATS=1: a line at exit.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "dev/loop_turns.h"

namespace hk::turns {

inline bool loops_wanted() {
    const char *e = getenv("HK_68K_LOOPS");
    return !e || strcmp(e, "0") != 0;
}

template <class Cpu> class Counter {
    using C = typename Cpu::Core;
    static constexpr size_t TRAIL = 12288;     // instructions watched to find the loop (a turn is a few hundred)
    static constexpr size_t MIN_BODY = 8;      // a loop of fewer different instructions is one inside the turn (a delay)
    static constexpr size_t MAX_LOG = 8192;    // a turn's bus cycles
    static constexpr int MIN_ROOM = 1024;      // cycles left in a timeslice for learning a turn to be tried
    static constexpr uint32_t MAX_FAILS = 12;  // turns in a row that were not one to count: the loop is looked for again

    bool check_ = false;
    // the loop's head: looked for (every instruction's address into `trail_`), or known
    bool seeking_ = false;
    std::vector<uint32_t> trail_;
    uint32_t head_ = 1;
    int seek_wait_ = 2, seek_backoff_ = 1;   // frames before looking (again)
    // the turn learned from it
    bool known_ = false;
    std::vector<char> state_;   // the processor at the head
    int cycles_ = 0;
    Turn turn_;
    // a turn being learned
    bool learning_ = false, log_bad_ = false;
    uint32_t learn_run_ = 0;
    int learn_left_ = 0;
    std::vector<char> learn_state_, now_;
    std::vector<Access> log_;
    uint32_t wait_ = 0, fails_ = 0;   // head visits to let pass; turns in a row that could not be counted
    // =check: turns being run that would have been counted
    int check_left_ = 0, check_done_ = 0, check_from_ = 0;
    uint32_t check_run_ = 0;
    // the frame's watch, and the report
    uint64_t visits_ = 0, visits_before_ = 0;
    int unmet_ = 0;
    uint64_t counted_ = 0, cycles_counted_ = 0, learned_ = 0, refused_ = 0, sought_ = 0, found_ = 0, checked_ = 0;

    // memory holds what the learned turn read, and what it writes is memory still
    bool holds(C &c) const {
        for (const Word &w : turn_.reads)
            if (Cpu::plain(c, w.addr) != 3 || (Cpu::word(c, w.addr) & w.mask) != w.value) return false;
        for (const Word &w : turn_.finals)
            if (Cpu::plain(c, w.addr) != 3) return false;
        return true;
    }
    // what the turn writes, as it leaves it
    bool settled(C &c) const {
        for (const Word &w : turn_.finals)
            if ((Cpu::word(c, w.addr) & w.mask) != w.value) return false;
        return true;
    }
    void settle(C &c) const {
        for (const Word &w : turn_.finals) {
            const uint16_t is = Cpu::word(c, w.addr);
            if ((is & w.mask) != w.value) Cpu::put(c, w.addr, (uint16_t)((is & ~w.mask) | w.value));
        }
    }

    void watch(C &c, uint32_t head) {
        head_ = head;
        seeking_ = known_ = learning_ = false;
        fails_ = wait_ = 0;
        Cpu::noting(c, false);
        Cpu::watch(c, head, 0xffffffff);
    }
    void seek(C &c) {
        seeking_ = true;
        known_ = learning_ = false;
        trail_.clear();
        Cpu::noting(c, false);
        Cpu::watch(c, 0, 0);   // every instruction
        sought_++;
    }

    // Seeking: the instructions' addresses repeat with the loop's length. The head is the lowest
    // address a turn passes once.
    void trailed(C &c) {
        trail_.push_back(Cpu::pc(c));
        if (trail_.size() < TRAIL) return;
        const std::vector<uint32_t> &t = trail_;
        const size_t n = t.size();
        uint32_t head = 1;
        for (size_t p = MIN_BODY; p <= n / 3 && head == 1; p++) {
            bool same = true;
            for (size_t i = n - 3 * p; i < n - p && same; i++) same = t[i] == t[i + p];
            if (!same) continue;
            std::vector<uint32_t> turn(t.end() - (long)p, t.end());
            std::sort(turn.begin(), turn.end());
            size_t different = 0;
            for (size_t i = 0; i < p; i++) {
                const bool first = i == 0 || turn[i - 1] != turn[i];
                if (first) different++;
                if (first && (i + 1 == p || turn[i + 1] != turn[i]) && head == 1) head = turn[i];
            }
            if (different < MIN_BODY) head = 1;   // a loop inside the turn: a longer period holds the turn
        }
        trail_.clear();
        if (head != 1) {
            found_++;
            watch(c, head);
        } else {   // none now (the program is busy): later
            watch(c, 1);
            seek_wait_ = seek_backoff_;
            seek_backoff_ = std::min(seek_backoff_ * 2, 64);
        }
    }

    [[noreturn]] void differs(const char *what) const {
        fprintf(stderr, "HK_68K_LOOPS=check: %s, the loop at %06x, turn %d of %d that would have been counted: %s\n", Cpu::name,
                head_, check_done_, check_done_ + check_left_, what);
        abort();
    }

public:
    Counter() {
        const char *e = getenv("HK_68K_LOOPS");
        check_ = e && !strcmp(e, "check");
    }
    ~Counter() {
        const char *e = getenv("HK_68K_STATS");
        if (!e || !*e || *e == '0') return;
        fprintf(stderr,
                "%s loops: %llu turns counted (%llu cycles) at %06x, %llu head visits, %llu turns learned, %llu refused, the loop "
                "looked for %llu times and found %llu",
                Cpu::name, (unsigned long long)counted_, (unsigned long long)cycles_counted_, head_, (unsigned long long)visits_,
                (unsigned long long)learned_, (unsigned long long)refused_, (unsigned long long)sought_, (unsigned long long)found_);
        if (check_) fprintf(stderr, ", %llu counted turns run and compared", (unsigned long long)checked_);
        fprintf(stderr, "\n");
    }
    uint64_t counted() const { return counted_; }

    // A bus cycle of the turn being learned: a read after it (data what was read), a write
    // before it is made (data what it writes).
    void note(C &c, uint32_t a, uint16_t data, uint16_t mask, bool write, int plain) {
        if (log_.size() >= MAX_LOG) {
            log_bad_ = true;
            return;
        }
        a &= 0xfffffe;
        log_.push_back({a, data, (uint16_t)(write && plain == 3 ? Cpu::word(c, a) : 0), mask, write, (uint8_t)plain});
    }

    // Between two instructions, at an address asked for by Cpu::watch.
    void at(C &c) {
        if (seeking_) return trailed(c);
        visits_++;
        int &left = Cpu::left(c);
        if (check_left_) {   // a turn that would have been counted has been run instead
            check_done_++, check_left_--;
            Cpu::snap(c, now_);
            if (Cpu::run(c) != check_run_) differs("the timeslice ended before it");
            if (left != check_from_ - check_done_ * cycles_) differs("its cycles are not the learned turn's");
            if (now_ != state_) differs("the processor is not as the learned turn left it");
            if (!holds(c) || !settled(c)) differs("memory is not as the learned turn left it");
            checked_++;
            return;
        }
        if (learning_) {   // the turn noted since the last visit
            learning_ = false;
            Cpu::noting(c, false);
            if (learn_run_ == Cpu::run(c)) {   // (else the timeslice ended inside it: nothing learned, nothing held against it)
                Cpu::snap(c, now_);
                const int cycles = learn_left_ - left;
                if (!log_bad_ && cycles > 0 && now_ == learn_state_ && analyse(log_, turn_)) {
                    known_ = true;
                    state_ = learn_state_;
                    cycles_ = cycles;
                    fails_ = 0;
                    seek_backoff_ = 1;
                    learned_++;
                } else {
                    refused_++;
                    fails_++;
                    wait_ = std::min<uint32_t>(1u << std::min<uint32_t>(fails_, 8), 256);
                }
            }
        }
        if (known_) {
            Cpu::snap(c, now_);
            if (now_ == state_ && holds(c)) {
                const int turns = (left - 1) / cycles_;   // (the instruction here starts with a cycle left, as it would)
                if (turns > 0) {
                    if (check_) {
                        check_left_ = turns, check_done_ = 0;
                        check_from_ = left, check_run_ = Cpu::run(c);
                    } else {
                        settle(c);
                        left -= turns * cycles_;
                        counted_ += (uint64_t)turns;
                        cycles_counted_ += (uint64_t)turns * (uint64_t)cycles_;
                    }
                }
                return;
            }
            known_ = false;   // something came in: this turn is another
        }
        if (wait_) {
            wait_--;
            return;
        }
        if (left < std::max(MIN_ROOM, cycles_)) return;
        Cpu::snap(c, learn_state_);
        learn_run_ = Cpu::run(c);
        learn_left_ = left;
        log_.clear();
        log_bad_ = false;
        learning_ = true;
        Cpu::noting(c, true);
    }

    // Once a frame: the known loop not met for a while, or its turns not ones to count (the
    // program is elsewhere): the loop is looked for again.
    void frame(C &c) {
        if (seeking_ || check_left_) return;
        unmet_ = head_ != 1 && visits_ != visits_before_ && fails_ < MAX_FAILS ? 0 : unmet_ + 1;
        visits_before_ = visits_;
        if (unmet_ < 2) return;
        if (seek_wait_ > 0) {
            seek_wait_--;
            return;
        }
        unmet_ = 0;
        if (head_ != 1 && fails_ >= MAX_FAILS) {   // (found, but never a turn to count: not at once again)
            seek_wait_ = seek_backoff_;
            seek_backoff_ = std::min(seek_backoff_ * 2, 64);
        }
        seek(c);
    }
};

}  // namespace hk::turns
