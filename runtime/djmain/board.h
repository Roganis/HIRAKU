// SPDX-License-Identifier: GPL-3.0-or-later
// The Konami DJ Main board (GQ753): a 68EC020 at 16 MHz with 64 KiB of RAM,
// K056832 tilemaps, sprites, a K055555 priority encoder, two K054539 PCM chips
// on 16 MiB of sample RAM, and an IDE hard disk. Memory map, interrupts, inputs
// and video composition follow MAME's djmain.cpp (BSD-3-Clause, smf et al.).
//
// The CPU is interpreted (Musashi): at 16 MHz it costs almost nothing.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hk::dj {

struct Config {
    std::string family;                    // djbm | djpopn
    std::string rom_dir;                   // dumps/<game>/rom
    std::vector<std::string> program;      // 2 files, 16-bit interleaved (even, odd)
    std::vector<std::string> sprites;      // 4 files: two interleaved pairs
    std::vector<std::string> tiles;        // 4 files: two interleaved pairs
    std::string hdd;                       // raw IDE disk image
    uint32_t hdd_chs[3] = {0, 16, 63};     // its geometry: cylinders (0: from the size), heads, sectors a track
    std::string map = "j";                 // MAME's address map: main_map_j, _u (UA/KA/AA sets) or _a (popn1 TA/AA)
    std::vector<uint8_t> ata_master_password, ata_user_password;   // 32 bytes each, or none
    bool ata_log = false;
};

constexpr uint32_t CPU_HZ = 16'000'000;        // 32 MHz / 2
constexpr int SCREEN_W = 512, SCREEN_H = 384;
constexpr uint64_t FRAME_CYCLES = 660 * 418;  // pixel clock = CPU clock: 57.9962 Hz

bool init(const Config &cfg);                  // false: a message was printed

using FrameHook = std::function<void(uint64_t frame, const uint32_t *rgb, int w, int h)>;
using AudioSink = std::function<void(const float *lr, int frames)>;   // 48 kHz stereo (K054539::RATE)
void set_frame_hook(FrameHook h);
// The picture at the end of vertical blank (Core::set_present_hook), every frame.
using PresentHook = std::function<void(const uint32_t *rgb, int w, int h)>;
void set_present_hook(PresentHook h);
// Nobody looks at the frame hook's picture (a window shows the present hook's, and nothing hashes or
// saves frames): the hook is then given no picture, only its size (Core::frame_picture_unused)
void frame_picture_unused();
void set_audio_sink(AudioSink s);
[[noreturn]] void run();

uint64_t cycles();                     // CPU clocks since power-on
uint64_t frame_count();
// A mod's [[rate]] (host/mods.h): frames (the vblank interrupt) at hz, from the next frame on; 0:
// the board's own 57.9962 Hz. The CPU keeps its clock: each frame has fewer cycles
void set_frame_rate(double hz);
// A picture between the last frame and the next (0 < t < 1): what moved drawn further along its
// move (Core::render_between). nullptr before there are two frames to go by
const uint32_t *render_between(double t);
const std::string &family();
bool set_input(const std::string &name, int value);
// A DIP switch bank (DSW1, DSW2, DSW3) as MAME's port reads it: active low, 0xff all off, MAME's
// default for every DJ Main game (games/<id>/dips.txt names the switches). false: no such bank
bool set_dip(const std::string &bank, uint8_t value);
void turntable_add(int player, int delta);
const std::vector<uint8_t> &lamp_state();
uint64_t lamp_changes();
const uint8_t *mem(uint32_t addr, uint32_t len);   // RAM / ROM / sprite list, big-endian bytes
uint32_t pc();

struct Stats {
    uint64_t ata_commands = 0, irq4 = 0, irq1 = 0, unmapped = 0;
    uint64_t sound_samples = 0;
};
const Stats &stats();

}  // namespace hk::dj
