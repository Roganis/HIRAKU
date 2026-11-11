// SPDX-License-Identifier: GPL-3.0-or-later
// The Konami Firebeat board around the CPU: address decoding, the devices,
// interrupt wiring and video timing. Memory map and device behaviour follow
// MAME's firebeat.cpp (BSD-3-Clause, Ville Linde et al.).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "hiraku/ppc.h"

namespace hk::fb {

struct Config {
    std::string game = "bm3final";
    std::string family = "bm3";        // bm3 | popn | ppp | kbm
    std::string rom_dir;               // dumps/<game>/rom: dongle image, rtc default NVRAM
    int cabinet = -1;                  // cabinet info register (-1 = the family's default)
    std::string dongle_file;           // file name inside rom_dir (e.g. gcc01-jc)
    std::string cd_master, cd_slave;   // main ATA bus disc images ("" = drive with no disc)
    std::string nvram_dir;             // persistent RTC/flash images ("" = none)
    std::string floppy;                // beatmania III: the floppy disk's image file ("" = the drive stays empty)
    bool mame_fdc = false;             // ... its controller's FIFO as MAME has it, not as the chip's data sheet (dev/fdc.h)
    std::string trace_dir;             // write an oracle-format MMIO trace here ("" = off)
    bool ata_log = false;
};

// Inputs as the board's ports see them (active levels as on the real hardware).
struct Inputs {
    uint8_t in[4] = {0x0f, 0xff, 0xff, 0xff};      // IN0..IN3 (IN3 = DIP switches)
    uint16_t io[4] = {0x003f, 0x0001, 0x003f, 0xffff};
    uint16_t turntable[2] = {0, 0};
    uint8_t effect[7] = {};
    uint16_t sensor[4] = {};                        // ParaParaParadise sensor bars (active high)
};

void init(const Config &cfg);
const std::string &family();       // "bm3", "popn", ... (selects port layouts and default controls)

// Set an input by its MAME name ("P1 Button 3", "Coin 1", "TURNTABLE_P1/Turntable",
// "Effect 2", ...): 1/0 for buttons, the position for analog inputs. Shared by the
// input scripts and the host controls. Returns false for an unknown name.
bool set_input(const std::string &name, int value);
// Move a turntable by `delta` counts (it is a 10-bit wrapping counter).
void turntable_add(int player, int delta);
// Latency instrumentation: the virtual time of the last change to a player key.
void note_input_change();
uint64_t last_input_change();
// beatmania III's floppy disk (Config::floppy), as the player would at the cabinet's drive: taken
// out if it is in (written back to its file first, if the game changed it), else put in (read
// again from its file, so another disk can be given under the same name meanwhile). Also what the
// "Floppy" input does when pressed. What happened, in a few words; "" without the drive or a file.
std::string floppy_toggle();
bool floppy_in();
// ... and told to `f` each time (the window's notice)
void set_floppy_said(std::function<void(const std::string &)> f);
void shutdown();                       // flush NVRAM and traces
void save_now();                       // while playing: the saved memories that changed, to the writer thread (shell/saves.h)
Inputs &inputs();

// Called at the start of each vertical blank, with the composed frame.
using FrameHook = std::function<void(uint64_t frame, const uint32_t *rgb, int w, int h)>;
void set_frame_hook(FrameHook h);
// The picture at the end of vertical blank (Core::set_present_hook), every frame.
using PresentHook = std::function<void(const uint32_t *rgb, int w, int h)>;
void set_present_hook(PresentHook h);
// The GCU's refresh rate (its CRTC's timing), Hz
double frame_rate();
// The picture on show with what moves carried on `t` of a frame (Core::render_between); nullptr
// while the GCU has nothing to draw it from yet, or on Keyboardmania's two GCUs
const uint32_t *render_between(double t, int &w, int &h);
// The board's own sound (YMZ280B), 44.1 kHz interleaved stereo. Games with an SPU
// play through it instead (spu::set_audio_sink); there this stays unset.
using AudioSink = std::function<void(const float *lr, int frames)>;
void set_audio_sink(AudioSink sink);
uint64_t ymz_key_ons();
// Keyboardmania, ParaParaParadise: the YMZ280B's sound flash (chips 1 and 2) has never been
// written (test mode's SOUND FLASH INIT copies it from the CD): its sounds then read erased flash
bool sound_flash_blank();
int screens();   // monitors side by side in a frame: Keyboardmania's two GCUs, 2
uint64_t midi_out_bytes();         // Keyboardmania: bytes sent to the synth card
uint64_t frame_count();
// Lamps as the game last drove them, in cab::lamps(family()) order (1 = lit), and
// a counter that moves on every change.
const std::vector<uint8_t> &lamp_state();
uint64_t lamp_changes();
void set_trace_ctx(hk_ctx *c);   // PCs in the MMIO trace come from here

}  // namespace hk::fb
