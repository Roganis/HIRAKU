// SPDX-License-Identifier: GPL-3.0-or-later
// A System 573 cabinet on a USB_2_573 board (github.com/Roganis/USB_2_573), for the System 573
// games: --usb573 auto (or present: only a board plugged in at the start; or the board's HID path). The board stands where the 573 stood: the
// cabinet's JAMMA harness, its EXT-IN, EXT-OUT and ANALOG plugs and the I/O board's light plugs
// go to it, and it is on USB as two HID interfaces, a gamepad for anything and the raw one this
// talks to (its docs/PROTOCOL.md; 64-byte reports each way, a thousand a second).
//
// Nothing is named or bound: the board tells which pins the cabinet's switches pull low and the
// core puts them on the 573's input registers (Core::wired_inputs), so each game finds its
// controls where its cabinet has them, test mode's I/O check included. The other way, every change
// the game makes to the I/O board's outputs, EXT-OUT and the coin counters (Core::wired_outputs)
// goes to the board's pins in order: the lamps, and on a DDR cabinet the clock and data lines to
// each stage's GN845-PWB(B) board, which gives the panels only once the game has been through a
// dialogue with it. Those edges go out one by one, at the game's own pace; a stage's panels count
// as let go until its board has had that dialogue, and during any later one (docs/USB573.md).
//
// The game menu (runtime/menu/) in such a cabinet is no 573: it gets the cabinet's switches by the
// names its controls have, so the cabinet chooses its games too (its lamps stay dark there).
//
// The keyboard and other controllers still work beside it. The board's gamepad interface is left
// alone while this drives it (host/input.cpp). Without the board at the start, or unplugged, the
// game runs on: the board is looked for every second and takes up where the game is.
// Not checked on a board or a cabinet yet. HK_USB573_LOG=1 shows the reports.
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

// --usb573 WHERE, --usb573-debounce US: true if `name` was one
bool usb573_option(const std::string &name, const std::function<std::string()> &val);
std::string usb573_help();

// The board's thread, if --usb573 was given and the machine has a 573's connectors
void usb573_start(hk::Core &core);
// Once a frame on the core's thread. usb573_lamps as soon as the frame is emulated (the outputs'
// changes out, before the wait for the frame's time); usb573_frame as the next one starts (the
// switches in)
void usb573_lamps(hk::Core &core);
void usb573_frame(hk::Core &core);
void usb573_stop();   // the outputs off, the board let go
// Whether a controller is the board's gamepad interface while this drives the board: its buttons
// are the same switches, so the bindings leave it alone
bool usb573_owns(uint16_t vid, uint16_t pid, const char *product);

namespace usb573 {

constexpr uint16_t VID = 0x1209, PID = 0x0001;   // pid.codes' test ID (the board's protocol header)
constexpr uint16_t USAGE_PAGE = 0xff73;          // the raw interface's
constexpr const char *PRODUCT = "USB_2_573";     // what its product string starts with
constexpr int REPORT = 64;
constexpr int ENTRIES_MAX = 7;                   // output entries in a report

// The board's input report
struct In {
    uint8_t version = 0, status = 0;
    uint32_t switches = 0;       // wired573's bits, 1 = closed (bits 26-29: the board's option switches)
    uint8_t analog[4] = {};
    uint16_t applied = 0;        // output entries it has put on its pins since the last reset (wraps)
    uint8_t queued = 0, capacity = 0;
};
bool parse_in(const uint8_t *report, size_t n, In &in);

// An output entry: the state to drive, `delay_us` after the entry before it (0: at once)
struct Entry {
    uint64_t state = 0;
    uint32_t delay_us = 0;
};
std::array<uint8_t, REPORT> outputs_report(const Entry *entries, int n, bool flush = false);
std::array<uint8_t, REPORT> keepalive_report();
std::array<uint8_t, REPORT> reset_report();
std::array<uint8_t, REPORT> config_report(uint16_t debounce_us, uint16_t failsafe_ms, uint16_t counter_ms);

// The pace of a DDR stage's clock and data edges: the game's own, no two closer than the first and
// none held back longer than the second (microseconds)
constexpr uint32_t STAGE_GAP_MIN = 500, STAGE_GAP_MAX = 20000;
// A stage's panels count once its board has had the last edge of the game's dialogue for this long (seconds)
constexpr double STAGE_SETTLE = 0.1;

// The USB side (SDL's hidapi), replaceable for tests
class Link {
public:
    virtual ~Link() = default;
    // "auto": the first board found; else the HID path of one. false: none (said in `why`, if it's
    // worth saying: a board that is there but may not be opened)
    virtual bool open(const std::string &where, std::string &why) = 0;
    virtual void close() = 0;
    // An input report: the bytes read, 0 when none came in time, -1 when the board is gone
    virtual int read(uint8_t *report, int max, int timeout_ms) = 0;
    virtual bool write(const uint8_t *report, int n) = 0;   // an output report (no report ID in it)
};
std::unique_ptr<Link> system_link();          // nullptr in a build without SDL
void use_link(std::unique_ptr<Link> link);    // before usb573_start: tests' pretend board

}  // namespace usb573

}  // namespace hk::host
