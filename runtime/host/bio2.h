// SPDX-License-Identifier: GPL-3.0-or-later
// beatmania IIDX's BIO2 I/O board (IIDX 25 and later cabinets) as the cabinet's controls and lamps
// for the beatmania games: --bio2 PATH, the board's serial port (on Linux, USB CDC: /dev/ttyACM0,
// or better its /dev/serial/by-id/ name).
//
// The board speaks ACIO (acio.h) with its BI2A firmware, as the IIDX 25-26 games drive it and
// spice2x's games/iidx/bi2a.cpp answers them: node 1, version "BI2A"; command 0152 carries the
// lamps out (0x30 bytes) and brings the controls back (0x2e bytes). Out: 03-06 the panel's lamps
// (1P start, 2P start, VEFX, EFFECT), 07-14 the 14 keys' lamps, 17-1f the 9-character ticker
// (ASCII), 20-23 spotlights 1-4, 24 the neon, 25-28 spotlights 5-8; a lamp is lit by its byte's
// bit 7. Back, by bit (bit n = byte n/8, 1 << n%8): 10 Service, 11 Test, 76 EFFECT, 77 VEFX,
// 78 2P start, 79 1P start, keys 151 + 16 k (1P) and 263 + 16 k (2P); bytes 16 and 17 the
// turntables (256 a turn), the high nibbles of 0, 2, 4, 6 and 7 the five effector sliders, byte 8
// a 6-bit coin count. A board with the BI2X firmware (IIDX 28 and later) speaks ACIO2 instead: not yet
// (docs/BIO2-BI2X.md).
//
// The start-up is IIDX 27's (docs/BIO2-BI2A.md: a break and aa echoes at 115200, 0001 [00], 0002, 0003,
// 0100 [2d], 0120 [00 00], then 0152 polls); not checked on a board yet, nor the turntables'
// direction. HK_ACIO_LOG=1 shows the packets.
#pragma once

#include <functional>
#include <string>

namespace hk {
class Core;
}

namespace hk::host {

// --bio2 PATH: true if `name` was it
bool bio2_option(const std::string &name, const std::function<std::string()> &val);
std::string bio2_help();

void bio2_start();               // the board's thread, if --bio2 was given
// Once a frame on the core's thread. bio2_lamps as soon as the frame is emulated (the lamps and
// the ticker out, before the wait for the frame's time); bio2_frame as the next one starts (the
// controls in; and the lamps, for who calls nothing else)
void bio2_lamps(hk::Core &core);
void bio2_frame(hk::Core &core);
void bio2_stop();

}  // namespace hk::host
