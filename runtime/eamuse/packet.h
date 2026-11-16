// SPDX-License-Identifier: GPL-3.0-or-later
// A datagram between cabinets, as NET TAISEN's relay (netrelay.h) and the Python cabinets' LAN cable
// (runtime/python/lancable.h) carry them.
#pragma once

#include <cstdint>
#include <vector>

namespace hk::ea {

// addresses as 32-bit numbers (192.168.50.2 = 0xc0a83202)
struct Packet {
    uint32_t src = 0, dst = 0;
    uint16_t sport = 0, dport = 0;
    std::vector<uint8_t> data;
};

}  // namespace hk::ea
