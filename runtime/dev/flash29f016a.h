// SPDX-License-Identifier: GPL-3.0-or-later
// Fujitsu MBM29F016A 2 MiB byte-wide flash (AMD command set). Command handling
// follows MAME's intelfsh.cpp for this part, erase times included when the
// owner gives it a clock: a 64 KiB sector takes 1 s and the chip 16 s, and until
// then every read (whatever the address: pop'n polls sector 0 while erasing
// others) returns the toggling status and writes are ignored.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hk {

class Flash29F016A {
public:
    Flash29F016A() : data_(0x200000, 0xff) {}
    uint8_t read(uint32_t offset);
    void write(uint32_t offset, uint8_t v);
    std::vector<uint8_t> &data() { return data_; }
    bool dirty = false;     // written at all in this run
    bool unsaved = false;   // ... since its owner last saved it (cleared by the owner)
    // the owner's clock and its rate; without one, erases complete at once
    std::function<uint64_t()> clock;
    uint64_t clock_hz = 0;

private:
    enum class Mode { normal, id1, id2, readid, program, erase1, erase2, erase3, erase4 };
    bool erasing();                   // an erase still running (MAME FM_ERASEAMD4)
    void start_erase(uint64_t seconds);
    std::vector<uint8_t> data_;
    Mode mode_ = Mode::normal;
    uint64_t busy_until_ = 0;
    bool busy_ = false;
    uint8_t status_ = 0x80;
};

}  // namespace hk
