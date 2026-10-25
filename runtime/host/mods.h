// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Mods (docs/MODS.md): folders with a mod.toml, laid over the game as it runs. load() reads them
// before the machine starts (their media ranges go to shell/overlay.h, so the core's images see
// them from the first read); frame() puts their memory patches in place, once a frame.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace hk::mods {

// Each folder's mod.toml and the game's files at their paths in it (as ifs_layeredfs), for `game`,
// in order: the first mod wins where two touch the same place.
// root is the HIRAKU folder (dumps/<game>/machine.txt names the media). false, with a message in
// err, when a mod can't be used as it is.
bool load(const std::vector<std::string> &dirs, const std::string &game, const std::string &root, std::string &err);

// The memory patches: each put in place when its original bytes are there (call once a frame)
void frame();

// The display's refresh rate (Hz, 0: none or unknown), for a [[rate]] at hz = "display"; the shell
// tells it once a frame
void display_rate(double hz);

// The mods loaded, for the log: "name version (folder)"
std::vector<std::string> describe();

// For a core whose game runs as a program of its own (the PC games: core().mem() has nothing),
// what the mods change there, for the core to hand to the game's process:
// - the memory patches (not those tied to a [[rate]]), at the program's own addresses;
// - the game's files that a mod's file takes the place of, in a folder medium of machine.txt
//   (the PC games' data=): the game's file -> the mod's, absolute paths.
struct MemoryPatch {
    std::string mod;
    uint32_t address = 0;
    std::vector<uint8_t> original, replace;   // original empty: written every frame (a cheat)
    bool once = false;
};
std::vector<MemoryPatch> memory_patches();
std::vector<std::pair<std::string, std::string>> folder_files();
std::vector<std::string> folders();   // the mods' folders, in the list's order (the first wins)

// [[text]]: a string of the game's (Shift-JIS here, converted from the mod's UTF-8) replaced
// wherever it is, as a whole string (a zero before and after it), by one that fits its room (the
// string, its zero and the zeros after it), the rest zeros; from, to (0: not given) where to look
struct TextPatch {
    std::string mod, line, label;   // the mod; its table's place in mod.toml; find as written
    std::vector<uint8_t> find, replace;
    uint32_t from = 0, to = 0;
    bool every = false;   // each place it is; else it must be in one place only
};
std::vector<TextPatch> text_patches();   // the core takes them (frame() then leaves them alone)
// Where `t` is in `mem` (n bytes, its first at address `base`): a patch for each place it can go.
// None and *why empty: not there; *why says why it can't go in (several places, no room)
std::vector<MemoryPatch> find_text(const TextPatch &t, const uint8_t *mem, size_t n, uint32_t base, std::string *why);

}  // namespace hk::mods
