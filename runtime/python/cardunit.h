// SPDX-License-Identifier: GPL-3.0-or-later
// The e-AMUSEMENT card reader/writer and 10-key of a Konami Python 1 pop'n cabinet, on the
// interface board's serial port (9600 baud), as pop'n music 9's driver uses them
// (EE program 0x1a1050-0x1a6c00; PLAN.md section 23, "e-AMUSEMENT"). The bus is half-duplex:
// the game reads its own packet back before each answer.
//
//   game:   aa aa aa 55                         sync, echoed
//           aa T ADDR CMD LEN [payload] SUM     T = aa (bus) or 00 (card unit); SUM = 8-bit
//                                               sum of everything after the first byte
//   device: the packet, then  aa T' ADDR CMD LEN [payload] SUM   (T' = a5 bus, 01 card unit)
//
// Bus commands: 01 assign the address, 02 revision, 03 start. Card unit: 10/11 reset,
// 12 status, 14/15 control (1 open, 2 eject, 4 read, 5 write), 16 write buffer (128 bytes),
// 18 read buffer, 1e/1f format, 20 10-key init, 24 10-key status, 26 10-key events
// (PS/2 set 2 scan codes). Odd commands (11, 15, 1f) poll an even one still busy; this
// device is never busy.
//
// The card is a file of 128 bytes: what the game writes is kept, and a new file is a fresh
// card of the game running ("C00 NEW CARD" for pop'n 9, "C20 NEW CARD" for pop'n 10, ...). A card is taken in when the game opens the slot and the
// player has put one in, and given back when the game ejects it.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "python/cardreader.h"

namespace hk::py {

class CardUnit final : public CardReader {
public:
    // fresh: a new card's first four bytes, for the game running (fresh_header)
    using Header = std::array<uint8_t, 4>;
    explicit CardUnit(std::string card_file, Header fresh = fresh_header("popn9"));
    // A game's new-card header ({} if not known): pop'n 9 "C00 NEW CARD" 28 23 41 01 (0x1a2728
    // type 2), pop'n 10 "C20 NEW CARD" 28 23 49 01 (0x1ad3c8 type 7), pop'n 11 "D11 NEW CARD"
    // 28 24 45 11 (0x1a6200), pop'n 12 28 25 45 21 (0x1ad4e8), pop'n 13 28 25 45 31 (type 0x13 of
    // the header builder 0x180b498, in RAM). A used card has 0x40 more in the first byte.
    static Header fresh_header(const std::string &game);

    // CardReader (the bus is half-duplex: the answer follows the game's packet, read back)
    bool write(const uint8_t *data, uint32_t size, std::vector<uint8_t> &reply) override;
    bool echoes() const override { return true; }
    void card_button() override;
    void key(uint8_t scan, bool down) override;
    bool card_offered() const override { return offered_; }   // in the slot, or in the reader
    std::string state() const override;

    bool card_inside() const override { return inside_; }
    bool slot_ever_opened() const override { return opened_; }

    // Carrying a player to this game from the one before (docs/EAMUSE3.md, "One card for all"):
    // until the game writes the card, it is shown `older`, that game's card with the same number.
    // A game that rewrites it as its own (pop'n 10) keeps it so; one that gives it back unchanged
    // (pop'n 11-13 read the old card, then want a new one) next gets a fresh card of its own.
    void present_older(std::vector<uint8_t> older);
    // the game has asked the server about the older card's player. The card given back after that
    // and after the old PIN on the 10-key is a carry-over agreed (pop'n 11 then asks for an unused
    // card of its own); given back without them (the attract mode reading an inserted card, the
    // player saying no), the older card stays, for another try
    void older_asked() { older_asked_ = !older_.empty(); older_keyed_ = false; }
    // called with a line to print when the card shown changes from the older one to a fresh one
    std::function<void(const std::string &)> note;

    // A fresh card: the header and a CRC-8 of it, the rest blank.
    static std::vector<uint8_t> new_card(const Header &h);

private:
    void load();
    void save();
    void answer(uint8_t type, uint8_t cmd, const std::vector<uint8_t> &payload, uint8_t len,
                std::vector<uint8_t> &reply) const;
    uint8_t status() const;

    std::string file_;
    Header fresh_;
    std::vector<uint8_t> card_, buffer_;
    std::vector<uint8_t> older_;   // present_older's card, while it is the one shown
    bool older_asked_ = false, older_keyed_ = false;
    void given_back();
    uint8_t addr_ = 1;
    bool offered_ = false;   // the player has put the card in the slot
    bool inside_ = false;    // the reader has it
    bool accepting_ = false; // the game has opened the slot
    bool opened_ = false;    // ... at least once
    bool read_ready_ = false, buffer_ready_ = false;
    std::deque<uint8_t> keys_;   // 10-key events not yet sent
};

}  // namespace hk::py
