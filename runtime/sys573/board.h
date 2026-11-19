// SPDX-License-Identifier: GPL-3.0-or-later
// The Konami System 573: a PlayStation (CXD8530CQ R3000A + GTE, CXD8561Q GPU,
// SPU) with 4 MiB of RAM, 16 MiB of flash, an M48T58 timekeeper, a CR-589
// ATAPI CD-ROM drive, two PC Card slots, a security cassette and, for the music
// games, an analog or digital (MP3) I/O board.
//
// Every device is MAME's own code (third_party/mame, through the framework shim)
// and the board is a port of MAME's ksys573.cpp driver (BSD-3-Clause, R. Belmont,
// smf), so that it runs cycle for cycle as MAME does (PLAN.md section 19).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "shell/wired573.h"

namespace hk::k573 {

struct Config {
    std::string game;                      // MAME set name
    std::string family;                    // ddr, ...
    std::string rom_dir;                   // dumps/<game>/rom: the BIOS, cassette and I/O board chips
    std::string bios = "700a01.22g";
    std::string h8 = "h8a01.bin";
    std::string cassette = "x";            // MAME's cassette variant: x (X76F041), yi (X76F100 + DS2401)
    std::string cassette_eeprom, cassette_id;   // files in rom_dir (no id for x)
    // chips MAME's set has filled already, as "device:file": a set's installed flash (29f016a.27h)
    // and timekeeper (m48t58); each device starts from its region, as in MAME
    std::vector<std::string> preload;
    std::string install_kind;              // the install cassette's variant when not the game's (cassxzi: "xi")
    // MAME's install cassette, for the first boot of games that install from CD with one (ddr3mj:
    // ksys573.cpp's note 3): its files, and which cassette is in the slot, "game" or "install"
    std::string install_eeprom, install_id;
    std::string cassette_slot = "game";
    // the multi-session unit (DrumMania 4th Mix and later): windyfairy's emulation, not MAME's
    // stub (PLAN.md section 19); its CD (the songs' music), firmware and timekeeper
    bool msu = false;
    std::string msu_cd, msu_flash = "flash.20t", msu_rtc = "m48t58y.6t";
    std::string io_board = "analog";       // analog (GX700-PWB(F)) | digital (GX894-PWB(B), MP3) | none (a plain 573)
    bool cdrom = true;                     // the CR-589 drive; false: none fitted (MAME's konami573(config, true): Step Champ)
    std::string digital_id = "digital-id.bin";
    std::string cd;                        // the CD image (.cue or .iso)
    std::string pccard1;                   // "32mb": a Fujitsu 32 MiB flash card in slot 1 (GuitarFreaks)
    std::string pccard2;                   // "32mb": a Fujitsu 32 MiB flash card in slot 2; "npu": the network unit (npu.h)
    // the network unit's e-AMUSEMENT server (eagfdm.h): "local" (in the core, its data in the save
    // folder's eamuse/) or http://host[:port][/path]; HK_573_NET_LOG=FILE logs the traffic
    std::string eamuse = "local";
    std::string pcbid;   // "" or the cabinet's own PCBID for an online server (-p)
    std::string eakey;   // "" or an online server's key (-eakey, HK_EA_KEY; shell/keyed.h)
    // the e-AMUSEMENT card units (cardunit.h) on SIO1, switched with the multi-session unit by the
    // cassette's d7: 1 (DrumMania) or 2 (GuitarFreaks); 0 = none
    int card_readers = 0;
    int master_calendar = -1;              // the Master Calendar on JVS, with this area switch (mcal.h); -1: none
    bool rental = false;                   // the e-Amusement rental unit on SIO1 (rental.h; mamboagga)
    bool boot_cd = false;                  // DIP 4 "Start Up Device": the CD-ROM drive (MAME's default: the flash)
    bool memory_card_reader = false;       // the JVS memory card reader (ddr3m)
    std::string memcard1, memcard2;        // ... its slots' PlayStation memory cards, image files (memcard.h; "" = none)
    bool card_media = false;               // ... its cards changed by IC cards (--card-media: memcard_use)
    // The output's gain. MAME sums the SPU and the CD audio at 1.0 each and clips the sum at full
    // scale: DDR's mix peaks at 2.0 and crackles. The board mixes them in analog; 0.5 keeps MAME's
    // balance with no clipping. 1.0 (--mame-mix) is MAME's output, for comparing with its WAV.
    float mix_gain = 0.5f;
    // The sound for the host every millisecond of machine time, not at MAME's 50 Hz stream updates
    // (board.cpp, run()): the default. Off (--mame-sound) is MAME's, which regress compares with.
    bool fine_sound = true;
    // session play (link.h): the cabinet with the multi-session unit hosts the cable (a socket
    // path or host:port); the other joins it. Both run in lockstep, a quantum of emulated time apart.
    std::string session_host, session_join;
    int session_quantum_us = 500;
    std::string nvram_dir;                 // flash, timekeeper, cassette, card ("" = blank each run)
    int64_t rtc_time = 0;                  // the timekeeper's time at power-on (seconds since 1970)
};

constexpr double CPU_HZ = 67737600.0 / 4;  // MAME's PS1 CPU: one instruction per 4 clocks

bool init(const Config &cfg);              // false: a message was printed

using FrameHook = std::function<void(uint64_t frame, const uint32_t *rgb, int w, int h)>;
using AudioSink = std::function<void(const float *lr, int frames)>;   // 44.1 kHz stereo
void set_frame_hook(FrameHook h);
// The picture at the end of vertical blank (Core::set_present_hook), every frame; after init().
using PresentHook = std::function<void(const uint32_t *rgb, int w, int h)>;
void set_present_hook(PresentHook h);
// The picture on show with what moves carried on `t` of a frame (Core::render_between); nullptr
// while the GPU has nothing to draw it from yet
const uint32_t *render_between(double t, int &w, int &h);
void set_audio_sink(AudioSink s);
[[noreturn]] void run();
void save_nvram();
void save_nvram_now();   // while playing: what changed, handed to the writer thread (shell/saves.h)

double seconds();
uint64_t frame_count();
const std::string &family();
bool set_input(const std::string &name, int value);
bool card_read(int player, const std::string &number);   // a real card reader's card (cardunit.h)
// The memory card reader's slot (0, 1) as the "Memory Card N" control works it: the card out or in.
// What happened, in a few words ("" without a card given for it); `said` hears each (a notice)
std::string memcard_toggle(int slot);
void set_memcard_said(std::function<void(const std::string &)> said);
// --card-media (shell/cardmedia.h): another card in the slot (0, 1), the one in it written back now
// and taken out ("" = the slot left empty; label names it in messages). false (and why): not a card
bool memcard_use(int slot, const std::string &file, const std::string &label, std::string &why);
bool memcard_busy(int slot);   // the game wrote that slot's card in the last second
// A real cabinet on the board's own connectors (shell/core.h's wired_*, shell/wired573.h): its
// switches pull the JAMMA and EXT-IN pins low beside the keyboard's and controllers', and each
// change of the I/O board's outputs, EXT-OUT and the coin counters is kept for it, in order.
void wired_attach(bool on);
void wired_inputs(uint32_t switches, const uint8_t *analog);
void wired_outputs(std::vector<wired573::Output> &changes);
const std::vector<uint8_t> &lamp_state();
uint64_t lamp_changes();
std::string sound_report();                // the mix's peak and how much of it clips ("" without sound)
const uint8_t *mem(uint32_t addr, uint32_t len);
uint32_t pc();

}  // namespace hk::k573
