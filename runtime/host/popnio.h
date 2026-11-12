// SPDX-License-Identifier: GPL-3.0-or-later
// A pop'n music cabinet's USB I/O board as the cabinet's buttons and lamps, for the pop'n music
// games (DJ Main, Firebeat, Python): --popn-io auto (or, on Linux, the board's USB port as it's named
// in /sys/bus/usb/devices, e.g. 3-2.1), --popn-io-dll FILE (the game's ezusb.dll).
//
// The board is the one pop'n music 15 and later drive through their ezusb.dll: a Cypress EZ-USB
// FX2LP (04b4:8613) that keeps no firmware of its own. The game's ezusb.dll loads it into the
// chip's RAM at every start (it's gone at power-off); a board that comes up bare gets it from
// --popn-io-dll, the user's own ezusb.dll (HIRAKU carries none: Konami's). It then says
// "KONAMI" (string descriptor 1) and keeps its IDs. Boards that copy it with firmware of their own
// (PopnIO in its REALIO mode) are taken as they are; bare is a board with no strings at all.
//
// After the firmware: 64-byte interrupt packets each way, EP 0x81 in, EP 0x01 out (docs/POPN-IO.md).
// Out: bytes 2-3 a command (0 0 while polling; 09 03, the coin mode pop'n sets, once), bytes 8-11
// the lamps as a little-endian word: bits 0-4 the top lamps, 8-11 the side lamps, 23-31 the
// buttons'. In: bytes 8-11 the controls, active low: bit 6 SERVICE, 7 TEST, 8-16 buttons 1-9;
// bytes 5-6 a coin count (big-endian). The board's EEPROM and security commands are never sent:
// out packets are only ever the polling and coin-mode ones, and the firmware load only writes the
// chip's RAM and its CPUCS register (popnio::allowed_*).
//
// Linux: usbdevfs (the board's /dev/bus/usb node must be the user's: docs/PLAY.md). Windows: through
// the ezusb.dll itself, as the games do, which also drives boards and controllers that replace it
// (UltimatePopnController's DLL, PopnIO): a 32-bit helper, hiraku-ezusb.exe, loads it (HIRAKU is
// 64-bit) and trades the controls and lamps through shared memory (ezusb/link.h).
// Coins: the board's count; a board whose count never moves, by its coin switch (bit 22).
// Not checked on a board yet. HK_POPNIO_LOG=1 shows the packets (Linux).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hk {
class Core;
}

namespace hk::host {

// --popn-io WHERE, --popn-io-dll FILE (--popn-io-firmware, the same): true if `name` was one
bool popnio_option(const std::string &name, const std::function<std::string()> &val);
// The game's own ezusb.dll, for --popn-io when --popn-io-dll gave none (the PC games: runtime/pc/)
void popnio_default_dll(const std::string &path);
std::string popnio_help();

void popnio_start();               // the board's thread, if --popn-io was given
// Once a frame on the core's thread. popnio_lamps as soon as the frame is emulated (the lamps
// out, before the wait for the frame's time); popnio_frame as the next one starts (the controls
// in; and the lamps, for who calls nothing else)
void popnio_lamps(hk::Core &core);
void popnio_frame(hk::Core &core);
void popnio_stop();

namespace popnio {

// The firmware's image in a file (the game's ezusb.dll): Cypress's 70-byte records (length, 0,
// address, type, 65 bytes of data), type 0 data up to one of another type, found by their shape.
// Returned as ezusb.dll sends them: runs of records that follow on, while under 0x3e0 bytes.
// Empty if the file has none.
struct Chunk {
    uint16_t addr = 0;
    std::vector<uint8_t> data;
};
std::vector<Chunk> find_firmware(const std::vector<uint8_t> &file);

uint32_t lamp_bits(const std::string &name);   // a lamp of cab::lamps' on the board's word; 0: not there
std::array<uint8_t, 64> out_packet(uint32_t lamps, uint8_t node = 0, uint8_t cmd = 0);
uint32_t pad(const uint8_t *in);               // the controls, bytes 8-11, active high
uint16_t coins(const uint8_t *in);             // the coin count, bytes 5-6

struct Input {
    int bit;            // of pad()
    std::string name;
};
const std::vector<Input> &inputs();

// What may be sent: a vendor request 0xa0 to the chip's RAM (below 0x4000) or CPUCS (0xe600, one
// byte 0 or 1); an out packet with no command but the coin mode; bulk out all zero.
bool allowed_control(uint8_t request, uint16_t value, const uint8_t *data, size_t n);
bool allowed_out(const uint8_t *packet, size_t n);
bool allowed_bulk(const uint8_t *data, size_t n);

// The USB side (Linux: usbdevfs), replaceable for tests
struct Device {
    std::string port;   // "3-2.1": stays the same when the board comes back with its firmware
    uint16_t vid = 0, pid = 0;
    bool bare = false;  // no string descriptors: a chip with no firmware
    std::string name;   // string descriptor 1 ("KONAMI"), if any
};
class Usb {
public:
    virtual ~Usb() = default;
    virtual std::vector<Device> scan() = 0;                 // every USB device
    virtual bool open(const std::string &port) = 0;
    virtual void close() = 0;
    // A vendor request to the device (bmRequestType 0x40)
    virtual bool control_out(uint8_t request, uint16_t value, uint16_t index, const uint8_t *data, uint16_t n) = 0;
    // An interrupt or bulk transfer: the bytes moved, or XFER_TIMEOUT, XFER_GONE (unplugged), XFER_ERROR
    enum { XFER_TIMEOUT = -1, XFER_GONE = -2, XFER_ERROR = -3 };
    virtual int transfer(uint8_t ep, uint8_t *data, int n, int timeout_ms) = 0;
};
std::unique_ptr<Usb> system_usb();                          // nullptr where there's none (Windows: the helper)
void use_usb(std::unique_ptr<Usb> usb);                     // before popnio_start: tests' pretend board, on any system

}  // namespace popnio

}  // namespace hk::host
