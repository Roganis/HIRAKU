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
// - the memory patches (not those tied to a [[rate]]), at the program's own addresses, or (module
//   given) at an offset in one of the program's modules, wherever Windows put it;
// - the game's files that a mod's file takes the place of, in a folder medium of machine.txt
//   (the PC games' data=): the game's file -> the mod's, absolute paths.
struct MemoryPatch {
    std::string mod;
    uint32_t address = 0;
    std::string module;   // a module of the program's ("popn.dll"): address is an offset in it
    std::vector<uint8_t> original, replace;   // original empty: written every frame (a cheat)
    bool once = false;
};
std::vector<MemoryPatch> memory_patches();
// ... and those tied to a [[rate]] (while_rate; between_ticks: only on the frames between the
// cabinet's ticks), which the game's process puts in and takes out itself as the rate goes on and
// off (no module: the program's own addresses)
struct RatePatch {
    MemoryPatch patch;
    bool between_ticks = false;
};
std::vector<RatePatch> rate_patches();
// The words of the game's memory the [[rate]]s look at (while_changing, while_equal): such a core
// reads them for frame() through core().mem(address, 4)
std::vector<uint32_t> rate_words();
std::vector<std::pair<std::string, std::string>> folder_files();
// - the files a mod's folder (or a [[file]] at medium = "data") has that the folder medium "data"
//   lacks: new files there, the path they'd have (the folder's case for the folders it has) -> the
//   mod's, the first mod winning; the core takes them or leaves them out (the 32-bit PC games)
std::vector<std::pair<std::string, std::string>> added_files();
std::vector<std::string> folders();   // the mods' folders, in the list's order (the first wins)
// For a loader that lays the mods' folders over the game's data itself (the 64-bit and the last
// 32-bit PC pop'n): the folders to lay, in order (the first wins): each mod's [[files]] folders whose
// `when` holds (the first table's first), then the mod's own folder; and in each, the folders that
// aren't the game's files at their own paths (paths in it: the [[files]] folders, chosen or not)
struct Layer {
    std::string dir;
    std::vector<std::string> left_out;
};
std::vector<Layer> layers();

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

// ---- The mod API (docs/MODS.md, "Hooks, settings and actions") -------------------------------
// A mod saying `api = 1` may have [[hook]]s (run on the program's instructions, Core::mod_hook),
// [[setting]]s (kept per mod and game in the user's folder) and [[action]]s (a binding that moves a
// setting), and say what it needs (`requires`) and which program it was made for (`program`).
constexpr int API = 1;
// What a mod may need (requires = [...]): the core's and this build's (Core::mod_capabilities), the
// shell's ("settings"), and what main() offers (offer(): "actions" where the build has a window)
std::vector<std::string> capabilities();
void offer(const std::string &capability);
// --mod-api's text: the version, the capabilities, the registers, the byte order, the program
std::string api_text(const std::string &root, const std::string &game);
// --mod-setting NAME=VALUE (or MOD/NAME=VALUE, MOD the mod's folder's name): this run's value,
// not saved (scripts, tests). Before load(), which refuses a name no mod has
void override_setting(const std::string &spec);
// load() refused a mod that needs what this build hasn't and another program of the core's has
// (Core::mod_other_program): that program, beside this one, is run in this one's place, with
// --game <game> and the same arguments (execv: nothing returns). False: it can't be (none is
// needed, no such program here, or Windows, where the message says which program to start)
bool hand_over(int argc, char **argv, const std::string &game);
// The actions' bindings (a bindings file's spelling, host/input.h), each action's number its place
std::vector<std::string> action_bindings();
// An action's binding pressed (repeat: a key held down) or let go: what it changed, as lines to show
// for a moment (the mod's name, the setting and its value); {} when nothing changed
std::vector<std::string> action(size_t id, bool down, bool repeat = false);
std::string action_name(size_t id);   // "mod: action", for the log
// The settings now ("folder/name", value), the counters ("folder/name", count)
std::vector<std::pair<std::string, int64_t>> settings();
std::vector<std::pair<std::string, uint64_t>> counters();
// For a mod's script (host/modscript.h): a setting of the mod whose folder's name is `folder` (false:
// it has none of that name); set, clamped to its ends and kept as an action keeps it (now: its
// value then); a [[hook]] counter's count (0: none of that name)
bool setting_value(const std::string &folder, const std::string &name, int64_t &value);
bool set_setting(const std::string &folder, const std::string &name, int64_t value, int64_t &now);
uint64_t counter_value(const std::string &folder, const std::string &name);
void report();   // the counters and settings, in the log (at exit)
void clear();    // every mod forgotten (the tests; a core's hooks stay)

}  // namespace hk::mods
