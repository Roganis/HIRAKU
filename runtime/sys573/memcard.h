// SPDX-License-Identifier: GPL-3.0-or-later
// PlayStation memory cards in the System 573's memory card reader (GE885-PWB(A), MAME's k573mcr):
// the JVS device DDR 2ndMIX Link Ver. to EXTREME and GuitarFreaks 2ndMIX Link Kit to 4thMIX fit,
// which takes a player's card from the home version (edit data, link data, the system file).
//
// The reader and the card are MAME's (k573mcr.cpp, bus/psx/memcard.cpp: the card's own protocol,
// 128-byte frames with their checksum). This is the cards' files, a slot each (--memcard1,
// --memcard2):
//
//   The file is the card. It is read whole when the card goes in (at power-on, or the "Memory Card
//   N" control); a file that isn't there is made, a formatted card with no saves on it, as a new
//   card comes from the shop. Only raw images of a 128 KiB card (.mcr, .mcd, .mc, .srm: 131,072
//   bytes); others are refused with why.
//
//   The game's writes are written back to the file whole (shell/saves.h: never half a file after a
//   crash or a full disk), while playing once the card has had no write for a second (a save of
//   several frames is over then), when the card is taken out, and at the end. A file that can't be
//   written still plays, its changes kept for the run alone.
//
//   A departure from MAME: the reader's first write after a card goes in doesn't reach the card.
//   The games send it as soon as they see a card (a block of ff to frame 63). MAME's reader drops
//   the first write after power-on only (its flag isn't cleared when a card comes out), so a card
//   put in again later had frame 63 overwritten; MAME's own notes say the real reader never puts
//   that block on the card. HIRAKU plays on the player's own file, so it writes nothing the
//   hardware wouldn't: the board brackets each of the reader's write commands with
//   before_reader_write() / after_reader_write().
//
// No card given: the slots stay empty, as before this file (the reader says no card).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class device_memcard_image_interface;

namespace hk::k573 {

constexpr size_t kMemoryCardBytes = 131072;   // 16 blocks of 8 KiB; 1024 frames of 128 bytes

// A formatted card with no saves: the header frame ("MC"), 15 free directory frames, the 20
// frames of the broken-frame list with none listed, the header's copy in frame 63
std::vector<uint8_t> blank_memory_card();
// A frame's checksum as the card's directory has it in its last byte: the other 127 XORed
uint8_t memory_card_frame_sum(const uint8_t *frame);

class MemoryCards {
public:
    // files[k]: slot k+1's card image ("" = no card in that slot)
    void configure(const std::string &slot1, const std::string &slot2);
    bool given(int slot) const { return slot >= 0 && slot < 2 && !slots_[slot].file.empty(); }
    bool any_given() const { return given(0) || given(1); }

    // The reader's two card devices, once the machine is built; each given card goes in. false
    // (and why): a file isn't a card's image
    bool attach(device_memcard_image_interface *slot1, device_memcard_image_interface *slot2, std::string &why);
    // Each frame: notes when the game last wrote a card (machine seconds)
    void tick(double seconds);
    // The cards the game changed, to their files: now (before returning, at the end) or on the saves'
    // writer thread, and then only a card the game hasn't written for a second
    void keep(bool now, double seconds);
    // The "Memory Card N" control: takes the card out of slot N (0, 1) or puts it in. What happened,
    // in a few words; "" when that slot was given no card
    std::string toggle(int slot);
    bool in(int slot) const;
    // The game's write command to slot N's card is about to run (board.cpp, the JVS packet): the
    // first since that card went in goes nowhere. after_reader_write() when it has run
    void before_reader_write(int slot);
    void after_reader_write();
    std::function<void(const std::string &)> said;   // each toggle's words (an on-screen notice)

private:
    struct Slot {
        std::string file;
        std::vector<uint8_t> bytes;
        device_memcard_image_interface *dev = nullptr;
        bool inserted = false;
        bool fresh = false;         // in, and the reader hasn't written to it yet
        bool read_only = false;     // the file can't be written: the card plays, its changes not kept
        uint64_t kept = 0;          // the device's writes as the file has them
        uint64_t seen = 0;          // ... as tick() last saw them, and when they changed
        double changed_at = 0;
        bool known = false;         // false: what the file holds isn't known (a write failed)
        unsigned failed = 0;        // the saves writer's failures when last looked at
    };
    bool insert(int slot, bool at_power_on, std::string &why);
    void keep_slot(Slot &s, bool now);
    Slot slots_[2];
};

}  // namespace hk::k573
