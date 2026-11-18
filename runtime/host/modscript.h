// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Mods' scripts (docs/MODS.md, "Scripts"): a mod's `script = "mod.lua"`, run by Lua 5.4
// (third_party/lua) in a sandbox of its own. host/mods.h reads mod.toml and hands each script here;
// everything a script does runs on the machine's thread (the frame hook, a hook before an
// instruction, the window's drawing, which is on that thread too).
//
// The sandbox: Lua's base library (no dofile, loadfile or require; load takes text only;
// collectgarbage only "count"; print goes to the log), string, table, math (math.random from a fixed
// seed, math.randomseed only with one given) and utf8; nothing of io, os, package, debug. Each
// callback has an instruction budget, and each script a memory cap: over either, or an error in a
// callback, and the script is stopped (said in the log and on the window) while the game goes on.
// An error as the file first runs (at start) refuses the mod, as any mod.toml error does.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hk::modscript {

constexpr int64_t kBudget = 1'000'000;        // Lua instructions a callback may take (on_frame, on_input, drawing)
constexpr int64_t kHookBudget = 100'000;      // ... a hook's function (before an instruction: it may run often)
constexpr int64_t kLoadBudget = 10'000'000;   // ... the file as it first runs
constexpr size_t kMemory = 32u << 20;         // bytes a script's Lua may hold

// A mod's script: `file` (in the mod's folder, checked by host/mods.cpp), for the mod named `mod`
// whose folder's name is `folder` (its settings), with what its mod.toml's `requires` lists. Run
// once now. -1, with why in err: it can't be (an error, or it defines a callback its requires
// doesn't allow: on_input needs "input", draw_game and draw_window "draw")
int load(const std::string &mod, const std::string &folder, const std::string &file, const std::vector<std::string> &needs,
         std::string &err);
// A global function of script id's, for a [[hook]]'s call: its reference, 0 when there's none
int function_ref(int id, const std::string &name);
// A [[hook]]'s call, before the instruction at `at`: fn(regs, at), regs read and written by the
// registers' names (Core::mod_registers)
void call_hook(int id, int ref, uint32_t *regs, uint32_t at);

// A control the game was given (the logical name, as the game's control table names it: after the
// bindings, from a key or controller, an input script or a macro): value 1/0 for a button, or an
// analog control's position. The scripts' on_input gets its edges, with the machine's time now,
// before their next on_frame. Any thread
void input(const std::string &name, int value);

// Once a game frame, from the frame hook: the controls given since the last (on_input), on_frame,
// then, where the game has a window (`draw`), draw_game (its picture w x h) and draw_window
bool any();   // a script is loaded
void frame(uint64_t f, int w, int h, bool draw);

// ---- Drawing ----------------------------------------------------------------------------------
// What the scripts drew, as pictures: argb (straight alpha) w x h, shown at x, y as dw x dh (a
// filled rectangle is one pixel, stretched)
struct Piece {
    int x = 0, y = 0, dw = 0, dh = 0;
    int w = 0, h = 0;
    std::vector<uint32_t> argb;
};
// The game layer laid over a copy of the game's picture (rgb, w x h): the copy in `out`, or rgb
// itself when nothing is drawn. The window's copy only: the frame hook's picture (digests,
// snapshots, recordings) never has it
const uint32_t *compose(const uint32_t *rgb, int w, int h, std::vector<uint32_t> &out);
// ... the same with the shell's own two copies: the frame's picture, and an in-between one
// (--smooth-motion) (nullptr stays nullptr)
const uint32_t *compose_frame(const uint32_t *rgb, int w, int h);
const uint32_t *compose_between(const uint32_t *rgb, int w, int h);
// The window layer, in the window's pixels, drawn after the picture mode (picture::overlay each)
const std::vector<Piece> &window_pieces();
const std::vector<Piece> &game_pieces();
// Where the window last showed the picture: its rectangle x, y, w x h in the window's ww x wh
void set_view(int x, int y, int w, int h, int ww, int wh);
// Pieces laid on argb pixels (cw x ch), as the window's overlays are: for tests
void paint(const std::vector<Piece> &pieces, uint32_t *argb, int cw, int ch);

// The host's: the 8x8 font of text (96 glyphs from ' ', a byte a row, bit 0 the left pixel), and
// the on-screen line of hk.notice (nothing: the log only)
void set_font(const uint8_t (*glyphs)[8]);
void set_notice(std::function<void(const std::vector<std::string> &)> fn);

void report();   // a line each in the log (at exit): what each script took
std::vector<std::string> stopped();   // "mod: why" for each script stopped (the tests)
void clear();    // every script closed (host/mods.h clear(): the tests)

}  // namespace hk::modscript
