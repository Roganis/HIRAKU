// SPDX-License-Identifier: GPL-3.0-or-later
// The e-Amusement rental unit of Mambo a GoGo's rental version (GQA40 JRB, `mamboagga`): a box
// on the 573's SIO1 at 19,200 baud that the boot program asks for its version at power-on
// ("a5 c0"; the answer echoes it and gives three characters). Only that: the unit's presence. MAME
// has no such device (ksys573.cpp: "TODO: Rental serial device needs to be attached here"), so
// the game stops at ERROR 198 there; this is a departure from MAME (--rental), found from the
// game's own driver (PLAN.md section 24, phase 5). The rental billing the unit carried (topping up
// the cabinet's play count) is not emulated.
//
// HK_573_RENTAL_LOG=1 prints the bytes each way.
#pragma once

#include "emu.h"

#include <cstdint>
#include <deque>
#include <functional>

namespace hk::k573 {

class RentalUnit {
public:
    // once the machine exists; rxd drives SIO1's receive line, a bit a call
    void attach(running_machine &m, std::function<void(int)> rxd);
    void reset();
    // SIO1's transmit line, a bit a call (as its bit clock calls it)
    void txd(int bit);

private:
    void take(uint8_t b);
    void send(std::initializer_list<uint8_t> bytes);
    void send_tick(s32);

    running_machine *machine_ = nullptr;
    std::function<void(int)> rxd_;
    emu_timer *send_timer_ = nullptr;
    bool sending_ = false;
    bool log_ = false;
    int rx_bits_ = -1;      // bits since a start bit (-1: waiting for one)
    uint8_t rx_byte_ = 0;
    std::deque<uint8_t> in_, out_;
};

}  // namespace hk::k573
