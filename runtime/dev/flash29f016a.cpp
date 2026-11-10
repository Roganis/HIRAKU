// SPDX-License-Identifier: GPL-3.0-or-later
#include "flash29f016a.h"

#include <algorithm>

namespace hk {

bool Flash29F016A::erasing() {
    if (busy_ && clock() >= busy_until_) {
        busy_ = false;
        status_ = 0x80;
        mode_ = Mode::normal;
    }
    return busy_;
}

void Flash29F016A::start_erase(uint64_t seconds) {
    if (!clock || !clock_hz) return;
    busy_ = true;
    busy_until_ = clock() + seconds * clock_hz;
    status_ = 1 << 3;
}

uint8_t Flash29F016A::read(uint32_t off) {
    off &= 0x1fffff;
    if (erasing()) {
        status_ ^= (1 << 6) | (1 << 2);   // DQ6 and DQ2 toggle on every read
        return status_;
    }
    if (mode_ == Mode::readid) {
        switch (off & 0xff) {
        case 0: return 0x04;   // Fujitsu
        case 1: return 0xad;   // MBM29F016A
        default: return 0;
        }
    }
    return data_[off];
}

void Flash29F016A::write(uint32_t off, uint8_t v) {
    off &= 0x1fffff;
    if (erasing()) return;
    const uint32_t a = off & 0xfff;
    if (v == 0xf0 && mode_ != Mode::program) { mode_ = Mode::normal; return; }
    switch (mode_) {
    case Mode::normal:
    case Mode::readid:
        if (v == 0xaa && a == 0x555) mode_ = Mode::id1;
        break;
    case Mode::id1:
        mode_ = (v == 0x55 && a == 0x2aa) ? Mode::id2 : Mode::normal;
        break;
    case Mode::id2:
        if (a == 0x555 && v == 0x90) mode_ = Mode::readid;
        else if (a == 0x555 && v == 0xa0) mode_ = Mode::program;
        else if (a == 0x555 && v == 0x80) mode_ = Mode::erase1;
        else mode_ = Mode::normal;
        break;
    case Mode::program:
        data_[off] &= v;   // programming can only clear bits
        dirty = unsaved = true;
        mode_ = Mode::normal;
        break;
    case Mode::erase1:
        mode_ = (v == 0xaa && a == 0x555) ? Mode::erase2 : Mode::normal;
        break;
    case Mode::erase2:
        mode_ = (v == 0x55 && a == 0x2aa) ? Mode::erase3 : Mode::normal;
        break;
    case Mode::erase3:
        if (v == 0x10 && a == 0x555) {                           // chip erase
            std::fill(data_.begin(), data_.end(), 0xff);
            dirty = unsaved = true;
            start_erase(16);
        } else if (v == 0x30) {                                  // sector erase (64 KiB)
            uint32_t base = off & ~0xffffu;
            std::fill(data_.begin() + base, data_.begin() + base + 0x10000, 0xff);
            dirty = unsaved = true;
            start_erase(1);
        }
        mode_ = Mode::normal;
        break;
    case Mode::erase4:
        mode_ = Mode::normal;
        break;
    }
}

}  // namespace hk
