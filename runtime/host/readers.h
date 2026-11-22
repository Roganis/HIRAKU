// SPDX-License-Identifier: GPL-3.0-or-later
// Real e-AMUSEMENT card readers on the PC: a card held to one is that player's card in the game
// (Core::card_read: pop'n music 9-14, and GuitarFreaks and DrumMania with their card units), and a
// reader's 10-key is the game's ("Keypad 0".., "P2 Keypad 0".. for player 2's). Each reader runs on
// a thread of its own; readers_frame() hands the core what they read, once a frame, on its thread.
//
//   --reader PATH   a Konami e-AMUSEMENT reader with its 10-key (the ICCA/ICCB/ICCC family that
//                   spice2x's -reader drives) on a serial port: a USB-serial adapter, /dev/ttyUSB0,
//                   or better its /dev/serial/by-id/ name. Given twice, the second is player 2's
//                   (--reader "" --reader PATH: player 2's only)
//   --scard         NFC readers (ACR122U and the like) through PC/SC (the pcscd service): the first
//                   reader listed is player 1's, the second player 2's
//   --cardio        cardio readers (USB HID, usage page 0xffca) through /dev/hidraw*: likewise
//
// A card's number is its 16 hex digits. One that isn't an e-AMUSEMENT PASS (E004...; a FeliCa
// card, a phone) is made one as spice2x's -scardfix does: E00401 and its first 5 bytes, since
// these games know only those. A card held there stays read for 3 s (the reader sees it again
// and again).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace hk::host {

// One of the options above ("-reader" too, as spice2x spells it): true if `name` was one, its
// value taken with val(), and *error set if that was wrong.
bool readers_option(const std::string &name, const std::function<std::string()> &val, std::string *error);
std::string readers_help();

void readers_start();   // the readers asked for, each on a thread
// Once a frame on the core's thread: each card read goes to card(player, number) (Core::card_read),
// the 10-keys to input(name, value) (Core::set_input); both false for what the game hasn't. The
// 10-keys' presses and releases go to the way out to the game menu too (menukey.h), with each
// reader's last reading, whatever the game takes. A reader with no reading for
// menukey::STALE_SECONDS has its keys let go
void readers_frame(const std::function<bool(int, const std::string &)> &card,
                   const std::function<bool(const std::string &, int)> &input);
void readers_stop();
// The exit report's lines for the serial readers' 10-keys (each ends in a newline; empty with none):
// how often each was read, the longest gap, the answers that failed
std::string readers_report();

// The serial reader's dialogue, for tests (tests/readers_test.cpp): the reader's status block
// (18 bytes, 16 and a CRC) as the reader encrypts it, decrypted in place with the session's keys
struct ReaderCrypt {
    void set_keys(uint32_t reader_key, uint32_t game_key);
    void crypt(uint8_t *data, size_t len);
    static uint16_t crc(const uint8_t *data, size_t len);
    uint32_t keys[4] = {};
};

}  // namespace hk::host
