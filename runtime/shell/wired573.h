// SPDX-License-Identifier: GPL-3.0-or-later
// A System 573 cabinet's own wiring, pin by pin: the switches on the JAMMA edge and EXT-IN, and
// the outputs of the I/O board, EXT-OUT and the coin counters. It is what a USB_2_573 board
// (host/usb573.h) carries between a real cabinet and the 573 core (sys573/board.h), and that
// board's own numbering (its docs/PROTOCOL.md), so nothing is renumbered on the way.
#pragma once

#include <cstdint>

namespace hk::wired573 {

// Switches: one bit each, 1 = closed (the pin pulled to ground). Bits 0-15 are the 573's
// register 0x1f400008 inverted: player 2 in the low byte, player 1 in the high one.
enum : uint32_t {
    P2_LEFT = 1u << 0, P2_RIGHT = 1u << 1, P2_UP = 1u << 2, P2_DOWN = 1u << 3,
    P2_B1 = 1u << 4, P2_B2 = 1u << 5, P2_B3 = 1u << 6, P2_START = 1u << 7,
    P1_LEFT = 1u << 8, P1_RIGHT = 1u << 9, P1_UP = 1u << 10, P1_DOWN = 1u << 11,
    P1_B1 = 1u << 12, P1_B2 = 1u << 13, P1_B3 = 1u << 14, P1_START = 1u << 15,
    P1_B4 = 1u << 16, P1_B5 = 1u << 17, P1_B6 = 1u << 18,
    P2_B4 = 1u << 19, P2_B5 = 1u << 20, P2_B6 = 1u << 21,
    COIN1 = 1u << 22, COIN2 = 1u << 23, SERVICE = 1u << 24, TEST = 1u << 25,
    CABINET = 0x03ffffffu,   // the 26 of them
    // a DDR stage's four panels, which its GN845-PWB(B) board gives only once the game has spoken to it
    P1_STAGE = P1_LEFT | P1_RIGHT | P1_UP | P1_DOWN,
    P2_STAGE = P2_LEFT | P2_RIGHT | P2_UP | P2_DOWN,
};

// Outputs: a 48-bit state. Bits 0-27 the I/O board's A0-A7, B0-B7, C0-C7, D0-D3, 1 = the output
// sinks to its bank's common (what lights a lamp wired to it); bits 28-31 the security cartridge
// slot's D4-D7, the pin's level (what a cartridge has hung on them: Step Champ's lamp drivers, an
// amplifier's control; D0-D3 are the cartridge's security chip and stay in the core); bits 32-39
// EXT-OUT's OUT0-OUT7, the pin's level; bits 40 and 41 the coin counters' coils, 1 = pulled.
constexpr uint64_t out_a(int n) { return 1ull << n; }
constexpr uint64_t out_b(int n) { return 1ull << (8 + n); }
constexpr uint64_t out_c(int n) { return 1ull << (16 + n); }
constexpr uint64_t out_d(int n) { return 1ull << (24 + n); }
constexpr uint64_t out_ext(int n) { return 1ull << (32 + n); }
constexpr uint64_t out_cart(int d) { return 1ull << (28 + d - 4); }   // d: 4-7
constexpr uint64_t OUT_COUNTER1 = 1ull << 40, OUT_COUNTER2 = 1ull << 41;
constexpr uint64_t OUT_ALL = 0x03ffffffffffull;
// The cartridge's lines carry serial words (a shift register's clock and data), so their edges
// count one by one and in order, as a DDR stage's do
constexpr uint64_t OUT_CART = 0xfull << 28;
// A DDR cabinet's stage lines: data and clock to each stage's board (A4, A5; B4, B5). Their
// edges are a serial bus, so they count one by one and in order, unlike a lamp's.
constexpr uint64_t OUT_STAGE1 = out_a(4) | out_a(5), OUT_STAGE2 = out_b(4) | out_b(5);

// Where a DDR stage's board (GN845-PWB(B)) stands after a change, as the one emulated in the core
// has it: STAGE_BUSY << p while stage p's is in a dialogue with the game (its lines then carry
// its answers, not the panels); STAGE_OPEN << p once it has been through one since power-on (a
// real one holds right and up until then)
constexpr uint8_t STAGE_BUSY = 0x01, STAGE_OPEN = 0x10;

// One change of the outputs, as the game made it
struct Output {
    uint64_t state = 0;      // all the outputs after it
    uint64_t at_us = 0;      // the machine's time, microseconds since power-on
    uint8_t stage = 0;       // STAGE_BUSY, STAGE_OPEN for each stage, after it
};

}  // namespace hk::wired573
