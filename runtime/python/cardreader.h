// SPDX-License-Identifier: GPL-3.0-or-later
// A Python cabinet's e-AMUSEMENT card reader, as the core drives it: the magnetic card
// reader/writer of pop'n 9-13 (cardunit.h) or pop'n 14's IC card reader (icca.h), both on the
// interface board's serial port, each with its 10-key.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hk::py {

class CardReader {
public:
    virtual ~CardReader() = default;

    // A write from the game to the serial port: false if it isn't this device's (left to the
    // emulator). The answer, if any, is appended to `reply`.
    virtual bool write(const uint8_t *data, uint32_t size, std::vector<uint8_t> &reply) = 0;
    // Whether `reply` starts with the game's own bytes (a half-duplex bus reads them back).
    virtual bool echoes() const = 0;

    // The player: the card button (puts the card in the slot, or takes it back when the
    // reader has given it out), and the 10-key (PS/2 set 2 scan code of the key, pressed or not).
    virtual void card_button() = 0;
    virtual void key(uint8_t scan, bool down) = 0;

    // The card is in the slot or in the reader (not with the player, nor given back).
    virtual bool card_offered() const = 0;
    // The reader has taken it in (the game holds it until it gives it back); false while it only
    // waits at the slot
    virtual bool card_inside() const { return card_offered(); }
    // The game has opened the slot since the start (an old reader's does only with e-AMUSEMENT on
    // in its test mode, and pop'n 9 only after Start)
    virtual bool slot_ever_opened() const { return true; }
    // A line for the window title ("card in the reader", ...), "" when nothing to say.
    virtual std::string state() const = 0;
};

}  // namespace hk::py
