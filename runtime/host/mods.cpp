// SPDX-License-Identifier: GPL-3.0-or-later
#include "host/mods.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <set>
#include <map>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <iconv.h>
#include <unistd.h>
#endif

#include "host/modscript.h"
#include "host/modsettings.h"
#include "host/png.h"
#include "host/pythondisk.h"
#include "shell/core.h"
#include "shell/kvfile.h"
#include "shell/overlay.h"
#include "shell/saves.h"
#include "shell/sha1.h"
#include "shell/sys.h"
#include "lang/lang.h"
#include "shell/userdir.h"
#include "shell/wordcond.h"

namespace hk::mods {

namespace {

// ---- mod.toml: the TOML a mod needs (docs/MODS.md): host/modsettings.h, shared with the launcher
using modsettings::Table;
using modsettings::parse;

struct Patch {
    std::string mod;
    uint32_t address = 0;
    std::string module;   // the PC games' (a module of the program's, address an offset in it): not frame()'s
    std::vector<uint8_t> original, replace;
    bool once = false, done = false, logged = false;
    bool under = false;   // left out: written every frame (no 'original') where a mod before it in the list patches
    bool while_rate = false;   // in place only while a [[rate]] is on; the original bytes otherwise
    // ... and then only on the frames between the cabinet's ticks: an increment made 0 there keeps
    // the cabinet's pace at any rate (pop'n music 14's objects' frame counters)
    bool between_ticks = false;
    // a float that holds one cabinet frame's time (a step a picture: the PC pop'n's animation time):
    // while the rate is on, the original times the cabinet's rate over the rate's (`replace` made so)
    bool per_picture = false;
};

// [[text]]: a string of the game's, found where it is (MemoryPatches once found)
struct Text {
    TextPatch t;
    bool done = false;
    uint64_t look_at = 0;   // the frame to look for it again (emulated cores: once a second)
};

// A mod's UTF-8 in the game's Shift-JIS (Windows' code page 932, as Konami's tools wrote it);
// plain ASCII as it is. false: a character Shift-JIS hasn't
bool to_sjis(const std::string &utf8, std::vector<uint8_t> &out) {
    out.clear();
    if (std::all_of(utf8.begin(), utf8.end(), [](char c) { return (unsigned char)c < 0x80; })) {
        out.assign(utf8.begin(), utf8.end());
        return true;
    }
#ifdef _WIN32
    const int nw = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (nw <= 0) return false;
    std::wstring w((size_t)nw, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), w.data(), nw);
    BOOL lost = FALSE;
    const int n = WideCharToMultiByte(932, 0, w.data(), nw, nullptr, 0, nullptr, &lost);
    if (n <= 0 || lost) return false;
    out.resize((size_t)n);
    WideCharToMultiByte(932, 0, w.data(), nw, (char *)out.data(), n, nullptr, nullptr);
    return true;
#else
    iconv_t cd = iconv_open("CP932", "UTF-8");
    if (cd == (iconv_t)-1) return false;
    std::string in = utf8;
    out.resize(in.size() * 2 + 4);
    char *ip = in.data(), *op = (char *)out.data();
    size_t il = in.size(), ol = out.size();
    const size_t r = iconv(cd, &ip, &il, &op, &ol);
    iconv_close(cd);
    if (r == (size_t)-1 || il) return false;
    out.resize(out.size() - ol);
    return true;
#endif
}

// [[rate]]: the machine's vsync at hz while a word of the game's memory changes from frame to
// frame (pop'n music 14's song clock: the notes are drawn from it, so they move smoothly); hz 0:
// the display's refresh rate
struct Rate {
    std::string mod;
    double hz = 0, now = 0;   // as asked; as set
    int times = 0;            // hz = "x2"...: that many times the machine's own rate (as it is: a PC IIDX's display type)
    WordCondition when;       // while_changing, while_equal (shell/wordcond.h)
    bool on = false, refused = false;
    int chart_times = 0;   // chart_times: the game's chart files read with their times this many times (a PC IIDX's)
};

constexpr double kDisplayFallbackHz = 119.88;   // hz = "display" with no window, or not known
double g_display_hz = 0;
// Whether the coming frame ends a cabinet frame's worth of time (1/59.94 s): between_ticks
// patches are out then. Kept by frame() while a rate is on; always so otherwise
double g_owed = 0;
bool g_tick = true;

struct Mod {
    std::string dir, name, version;
    std::vector<std::string> folders, chosen;   // its [[files]] folders (paths in it); those laid this run, in order
};

std::vector<Patch> g_patches;
std::vector<Text> g_texts;
std::vector<Rate> g_rates;
std::vector<Mod> g_mods;
std::map<std::string, std::string> g_folder_files;   // a folder medium's file -> the mod's (folder_files())
// the files a mod adds to the folder medium "data" (added_files()): its path there in lower case ->
// (that path, the mod's file); one entry for two mods' spellings of one path, the first mod's
std::map<std::string, std::pair<std::string, std::string>> g_added_files;

// ---- the mod API's state (docs/MODS.md, "Hooks, settings and actions") ----------------------
struct Setting {
    std::string mod, folder, name, label;   // the mod's name, its folder's name; the setting's
    int64_t min = 0, max = 0, dflt = 0, step = 1;
    std::vector<std::string> choices;       // its values' names (0..n-1), if it has them
    bool chooses = false;                   // a `when` of its mod's names it: the files and patches chosen at start
    std::atomic<int64_t> value{0};          // (hooks read it on the machine's thread, actions set it)
};
struct Action {
    std::string mod, name, label, binding;
    Setting *setting = nullptr;
    enum class Do { up, down, toggle } what = Do::up;
    bool notify = true;   // false (notify = false, "quiet"): no box over the game, but for what the mod can't show
};
struct Counter {
    std::string mod, folder, name;
    std::atomic<uint64_t> n{0};
};
// where a hook reads or writes: a register (by the core's numbering) or `size` bytes of RAM
struct Place {
    int reg = -1;
    uint32_t addr = 0;
    int size = 4;
};
struct Hook {
    std::string mod, where;
    uint32_t at = 0;
    enum class Op { set, add, sub, count, call } op = Op::set;
    Place target;
    int64_t imm = 0;
    Setting *setting = nullptr;   // the operand: a setting's value, else imm
    double scale = 1;
    bool when = false;            // only while `when_at` is in [min, max] (signed)
    Place when_at;
    int64_t min = INT64_MIN, max = INT64_MAX;
    Counter *counter = nullptr;
    int script = -1, ref = 0;     // call: the mod's script's function (host/modscript.h)
    bool missed = false;          // its RAM wasn't there once (said once)
};
// (deques: hooks and actions point into them, and the core holds the hooks)
std::deque<Setting> g_settings;
std::deque<Action> g_actions;
std::deque<Counter> g_counters;
std::deque<Hook> g_hooks;
std::vector<std::string> g_offered;                       // offer(): the shell's own capabilities
std::vector<std::pair<std::string, std::string>> g_overrides;   // --mod-setting: name (or folder/name), value
std::string g_hand_over;                                  // the program to hand the run to (hand_over())
bool g_big_endian = false;                                // core().mem()'s byte order (cached at load)
std::string g_settings_dir;                               // <user data>/mod-settings/<game>

uint32_t read_place(const Place &p, const uint32_t *regs, Hook &h, bool &ok) {
    ok = true;
    if (p.reg >= 0) return regs[p.reg];
    const uint8_t *m = core().mem(p.addr, (uint32_t)p.size);
    if (!m) {
        if (!h.missed) fprintf(stderr, "mods: %s: a hook's memory at 0x%08x isn't there (%s)\n", h.mod.c_str(), p.addr, h.where.c_str());
        h.missed = true;
        ok = false;
        return 0;
    }
    uint32_t v = 0;
    for (int i = 0; i < p.size; i++) v |= (uint32_t)m[i] << (8 * (g_big_endian ? p.size - 1 - i : i));
    return v;
}
int64_t signed_of(uint32_t v, int size) {
    return size == 1 ? (int64_t)(int8_t)v : size == 2 ? (int64_t)(int16_t)v : (int64_t)(int32_t)v;
}

// A [[hook]], run before its instruction (Core::mod_hook)
void run_hook(void *ctx, uint32_t *regs) {
    Hook &h = *static_cast<Hook *>(ctx);
    bool ok;
    if (h.when) {
        const int64_t v = signed_of(read_place(h.when_at, regs, h, ok), h.when_at.reg >= 0 ? 4 : h.when_at.size);
        if (!ok || v < h.min || v > h.max) return;
    }
    if (h.op == Hook::Op::count) {
        h.counter->n.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (h.op == Hook::Op::call) {
        modscript::call_hook(h.script, h.ref, regs, h.at);
        return;
    }
    int64_t operand = h.setting ? h.setting->value.load(std::memory_order_relaxed) : h.imm;
    if (h.scale != 1) operand = std::llround((double)operand * h.scale);
    const uint32_t now = read_place(h.target, regs, h, ok);
    if (!ok) return;
    const uint32_t v = h.op == Hook::Op::set ? (uint32_t)operand : h.op == Hook::Op::add ? now + (uint32_t)operand : now - (uint32_t)operand;
    if (h.target.reg >= 0) {
        regs[h.target.reg] = v;
        return;
    }
    auto *m = const_cast<uint8_t *>(core().mem(h.target.addr, (uint32_t)h.target.size));
    for (int i = 0; i < h.target.size; i++) m[i] = (uint8_t)(v >> (8 * (g_big_endian ? h.target.size - 1 - i : i)));
}

// a mod's settings file: <user data>/mod-settings/<game>/<the mod's folder's name>.txt, key=value
// lines (shell/kvfile.h): each setting's value, each action's binding as action.<name>
std::string settings_file(const std::string &folder) { return g_settings_dir + "/" + folder + ".txt"; }

void save_settings(const std::string &folder) {
    std::string text = "# HIRAKU: a mod's settings and its actions' bindings (docs/MODS.md). Edit while the game isn't running\n";
    for (const Setting &s : g_settings)
        if (s.folder == folder) text += s.name + "=" + std::to_string(s.value.load()) + "\n";
    for (const Action &a : g_actions)
        if (a.setting && a.setting->folder == folder) text += "action." + a.name + "=" + a.binding + "\n";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(std::u8string(g_settings_dir.begin(), g_settings_dir.end())), ec);
    saves::writer().write(g_settings_dir, {{folder + ".txt", text}});
}

modsettings::Setting def_of(const Setting &s) {
    modsettings::Setting d;
    d.name = s.name, d.label = s.label, d.min = s.min, d.max = s.max, d.dflt = s.dflt, d.step = s.step, d.choices = s.choices;
    return d;
}

std::string shown(const Setting &s) { return modsettings::shown(def_of(s), s.value.load()); }

// a setting that chooses files or patches changed while playing: from the next start (said)
const char *kNextStart = "from the game's next start";
// ... said on the screen (one text: translated where shown, host/notice.h)
const char *kNextStartSaid = HK_N("boxes", "Files and patches: from the game's next start");

std::string read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// the media of dumps/<game>/machine.txt: key -> path (relative paths are the game's folder's)
std::map<std::string, std::string> media(const std::string &root, const std::string &game) {
    std::map<std::string, std::string> m;
    const std::string dir = root + "/dumps/" + game;
    std::istringstream in(read_file(dir + "/machine.txt"));
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string v = line.substr(eq + 1);
        const bool absolute = !v.empty() && (v[0] == '/' || v[0] == '\\' || (v.size() > 1 && v[1] == ':'));
        std::string path = absolute ? v : dir + "/" + v;
        // a cue sheet's medium is the data file it names (its first FILE, as chdman extractcd
        // writes one): that's what the core reads
        if (path.size() > 4 && (path.substr(path.size() - 4) == ".cue" || path.substr(path.size() - 4) == ".CUE")) {
            std::istringstream cue(read_file(path));
            std::string l;
            while (std::getline(cue, l)) {
                const size_t q1 = l.find('"'), q2 = q1 == std::string::npos ? q1 : l.find('"', q1 + 1);
                if (l.find("FILE") == std::string::npos || q2 == std::string::npos) continue;
                const std::string f = l.substr(q1 + 1, q2 - q1 - 1);
                const size_t slash = path.find_last_of("/\\");
                path = (f.size() && (f[0] == '/' || (f.size() > 1 && f[1] == ':'))) ? f
                                                                                    : path.substr(0, slash == std::string::npos ? 0 : slash + 1) + f;
                break;
            }
        }
        m[line.substr(0, eq)] = path;
    }
    return m;
}

// [[file]]: a file on an ISO 9660 disc (docs/MODS.md), its data laid over the sectors it holds.
// The data track's sectors are told from where the volume descriptor is: 2048 bytes of data
// each (a cooked image), or 2352 with a 16-byte (mode 1) or 24-byte (mode 2 form 1) header.
// false when the image has no ISO 9660 volume.
bool iso_file(const std::string &image, const std::string &name, std::vector<uint8_t> data, const std::string &label,
              bool quiet = false) {
    struct Layout { uint64_t sector, header; };
    const Layout layouts[] = {{2048, 0}, {2352, 16}, {2352, 24}};
    const Layout *lay = nullptr;
    uint8_t pvd[2048];
    for (const Layout &l : layouts)
        if (overlay::read(image, 16 * l.sector + l.header, pvd, sizeof pvd) && pvd[0] == 1 && !memcmp(pvd + 1, "CD001", 5)) {
            lay = &l;
            break;
        }
    if (!lay) return false;
    auto u32 = [](const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; };
    auto read_sector = [&](uint32_t lba, uint8_t *out) {
        if (!overlay::read(image, (uint64_t)lba * lay->sector + lay->header, out, 2048))
            throw std::runtime_error(image + ": can't read sector " + std::to_string(lba));
    };
    uint32_t lba = u32(pvd + 156 + 2), size = u32(pvd + 156 + 10);   // the root directory
    bool dir = true;
    std::string rest = name;
    while (!rest.empty()) {
        const size_t slash = rest.find('/');
        const std::string part = rest.substr(0, slash);
        rest = slash == std::string::npos ? "" : rest.substr(slash + 1);
        if (part.empty()) continue;
        if (!dir) throw std::runtime_error(image + ": " + name + ": not a directory on the way");
        bool found = false;
        std::vector<uint8_t> sec(2048);
        for (uint32_t s = 0; s * 2048 < size && !found; s++) {
            read_sector(lba + s, sec.data());
            for (size_t at = 0; at < 2048 && sec[at];) {
                const uint8_t *r = &sec[at];
                // (a record is in its sector whole, its name in the record: a damaged disc's isn't read past)
                if (at + r[0] > 2048 || r[0] < 34 || 33u + r[32] > r[0]) break;
                std::string id((const char *)r + 33, r[32]);
                if (const size_t semi = id.find(';'); semi != std::string::npos) id.erase(semi);   // the version
                if (!id.empty() && id.back() == '.') id.pop_back();
                bool same = id.size() == part.size();
                for (size_t i = 0; same && i < id.size(); i++) same = std::toupper((unsigned char)id[i]) == std::toupper((unsigned char)part[i]);
                if (same && r[32] > 1) {
                    lba = u32(r + 2), size = u32(r + 10), dir = r[25] & 2;
                    found = true;
                    break;
                }
                at += r[0];
            }
        }
        if (!found) {
            if (quiet) return false;   // a mod's mirrored file that isn't on this disc
            throw std::runtime_error(image + ": no " + name + " on the disc");
        }
    }
    if (dir) {
        if (quiet) return false;
        throw std::runtime_error(image + ": " + name + " is a directory");
    }
    if (data.size() > size)
        throw std::runtime_error(name + ": the mod's file is " + std::to_string(data.size()) + " bytes, the disc's " +
                                 std::to_string(size) + " (a file can't grow yet)");
    data.resize(size, 0);   // shorter: the rest of the file's bytes are zeros
    if (lay->sector == 2048) {
        overlay::add(image, (uint64_t)lba * 2048, std::move(data), label);
        return true;
    }
    for (uint32_t s = 0; (uint64_t)s * 2048 < size; s++) {   // raw sectors: each one's 2048 data bytes
        const size_t from = (size_t)s * 2048, n = std::min<size_t>(2048, size - from);
        overlay::add(image, (uint64_t)(lba + s) * lay->sector + lay->header,
                     std::vector<uint8_t>(data.begin() + (long)from, data.begin() + (long)(from + n)), label);
    }
    return true;
}

// paths as UTF-8 std::strings, whatever the platform's (C++20's u8string is char8_t)
std::filesystem::path fs_path(const std::string &s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
std::string utf8(const std::filesystem::path &p, bool generic = false) {
    const std::u8string s = generic ? p.generic_u8string() : p.u8string();
    return std::string(s.begin(), s.end());
}

// The names a folder of the game's has (a folder medium's), read once a load: name_in() below
std::map<std::string, std::vector<std::string>> g_listed;

// The name `part` has in the folder `at` as the folder has it: `part` itself if that is there, else
// the one that differs from it in case only; "" when neither is. Never fs::exists(at / part): on
// Windows (a file system that ignores case) that holds for the mod's spelling too, and one game file
// would be two paths (hiraku-pc would hand the game both, and the DLL take the first in the list's
// order, not the first mod's)
std::string name_in(const std::filesystem::path &at, const std::string &part) {
    namespace fs = std::filesystem;
    auto [it, fresh] = g_listed.try_emplace(utf8(at));
    if (fresh) {
        std::error_code ec;
        for (auto d = fs::directory_iterator(at, ec); !ec && d != fs::directory_iterator(); d.increment(ec))
            it->second.push_back(utf8(d->path().filename()));
        std::sort(it->second.begin(), it->second.end());   // (the same one each time where two differ in case only)
    }
    std::string other;
    for (const std::string &name : it->second) {
        if (name == part) return name;
        bool same = other.empty() && name.size() == part.size();
        for (size_t i = 0; same && i < name.size(); i++) same = std::tolower((unsigned char)name[i]) == std::tolower((unsigned char)part[i]);
        if (same) other = name;
    }
    return other;
}

// `rel` in `folder` (a folder medium: the PC games' data), its name's case as the folder has it
// (the mod's may differ, as Windows doesn't mind), with '/' between its parts on every system;
// "" if it isn't there
std::string in_folder(const std::string &folder, const std::string &rel) {
    namespace fs = std::filesystem;
    fs::path at = fs_path(folder);
    std::string part, rest = rel;
    while (!rest.empty()) {
        const size_t slash = rest.find('/');
        part = rest.substr(0, slash);
        rest = slash == std::string::npos ? "" : rest.substr(slash + 1);
        if (part.empty() || part == "." || part == "..") return {};
        const std::string name = name_in(at, part);
        if (name.empty()) return {};
        at /= fs_path(name);
    }
    std::error_code ec;
    return fs::is_regular_file(at, ec) ? utf8(at, true) : std::string();
}

// `rel` new to `folder` (it isn't there): its path there, with the folder's case for the folders it
// has and the mod's for the rest, '/' between the parts; "" when it can't be (a part of the way is a
// file, or the path is a folder there)
std::string new_in_folder(const std::string &folder, const std::string &rel) {
    namespace fs = std::filesystem;
    fs::path at = fs_path(folder);
    std::string part, rest = rel;
    while (!rest.empty()) {
        const size_t slash = rest.find('/');
        part = rest.substr(0, slash);
        rest = slash == std::string::npos ? "" : rest.substr(slash + 1);
        if (part.empty() || part == "." || part == "..") return {};
        const std::string name = name_in(at, part);
        if (name.empty()) return utf8(at / fs_path(part), true) + (rest.empty() ? "" : "/" + rest);   // the rest is the mod's
        std::error_code ec;
        if (!fs::is_directory(at / fs_path(name), ec)) return {};
        at /= fs_path(name);
    }
    return {};   // (a folder of the game's)
}

void add_file(const std::string &game_file, const std::string &mod_file) {
    std::string low = game_file;
    for (char &c : low) c = (char)std::tolower((unsigned char)c);
    g_added_files[low] = {game_file, mod_file};   // (laid on from the last mod: the first's stays)
}

// A mod folder's files laid over the game's media by their paths, as ifs_layeredfs lays a mod's
// folder over a PC game's data: a file at image/kc/kc_001.gzz in the folder is that file on the
// game's disk (or a disc's DATA/TITLE.BIN on an ISO 9660 disc), and a PNG at image/kc/kc_001.png
// is that picture, converted (the size and kind of the game's own, 8-bit ones to its folder's
// palette). A file the game's data folder (the medium "data", the PC games') lacks is added to it
// (added_files(): whether the game can take it is the core's business). What isn't on any medium
// of the game's is left out (said once).
// `sub` "": the mod's own folder; else one of its [[files]] folders (a path in it), laid as the mod's
// own folder is. `files_folders`: all its [[files]] folders, chosen or not: a file in one is that folder's
// only (the deepest one's), never laid at its own path in the mod's
void lay_files(const std::string &dir, const std::string &name, const std::set<std::string> &named,
               const std::vector<std::string> &files_folders, const std::string &sub,
               const std::map<std::string, std::string> &medium_paths,
               std::map<std::string, std::unique_ptr<pythondisk::Files>> &python_disks) {
    namespace fs = std::filesystem;
    std::vector<std::string> files;
    const fs::path root = fs_path(sub.empty() ? dir : dir + "/" + sub);
    auto owner = [&](const std::string &in_mod) {   // the [[files]] folder a file of the mod's is in ("": none)
        std::string best;
        for (const std::string &f : files_folders)
            if (in_mod.size() > f.size() && in_mod[f.size()] == '/' && modsettings::same_name(in_mod.substr(0, f.size()), f) &&
                f.size() > best.size())
                best = f;
        return best;
    };
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        const std::string leaf = utf8(it->path().filename());
        if (!leaf.empty() && leaf[0] == '.') {   // hidden: skipped, folders and all
            if (it->is_directory()) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file()) continue;
        std::string rel = utf8(fs::relative(it->path(), root), true);
        const std::string in_mod = sub.empty() ? rel : sub + "/" + rel;
        if (in_mod == "mod.toml" || in_mod == "macros.txt" || named.count(in_mod)) continue;   // (macros.txt: its codes, macro.h)
        if (!modsettings::same_name(owner(in_mod), sub)) continue;
        files.push_back(rel);
    }
    if (files.empty()) return;
    std::sort(files.begin(), files.end());
    // the game's media this can lay files over: Konami Python disks and ISO 9660 discs, and the PC
    // games' folders (their files taken by the game's process from the mod's folder instead)
    std::set<std::string> images, folders;
    std::string data_folder;   // the folder new files go to
    for (const auto &[key, path] : medium_paths) {
        std::error_code ec;
        if (std::filesystem::is_directory(fs_path(path), ec)) {
            folders.insert(path);
            if (key == "data") data_folder = path;
        } else {
            images.insert(path);
        }
    }
    for (const std::string &image : images)
        if (!python_disks.count(image))
            if (auto disk = pythondisk::Files::open(image)) python_disks[image] = std::move(disk);
    int left_out = 0;
    std::string first_left_out;
    for (const std::string &rel : files) {
        const std::string label = name + ": " + (sub.empty() ? rel : sub + "/" + rel);
        const std::string src = dir + "/" + (sub.empty() ? rel : sub + "/" + rel);   // the mod's file
        bool in_a_folder = false;
        for (const std::string &folder : folders)
            if (const std::string game_file = in_folder(folder, rel); !game_file.empty()) {
                g_folder_files[game_file] = src;
                in_a_folder = true;
                break;
            }
        if (in_a_folder) continue;
        if (!data_folder.empty()) {   // new to the game's data
            if (const std::string game_file = new_in_folder(data_folder, rel); !game_file.empty()) {
                add_file(game_file, src);
                continue;
            }
        }
        if (images.empty()) {
            if (!left_out++) first_left_out = rel;
            continue;
        }
        const std::string data = read_file(src);
        std::vector<uint8_t> bytes(data.begin(), data.end());
        bool laid = false;
        const bool is_png = rel.size() > 4 && (rel.substr(rel.size() - 4) == ".png" || rel.substr(rel.size() - 4) == ".PNG");
        for (auto &[image, disk] : python_disks) {
            if (is_png) {   // a picture: the .gzz of the same name
                const std::string target = rel.substr(0, rel.size() - 4) + ".gzz";
                if (!disk->has(target)) continue;
                int w, h;
                std::vector<uint8_t> rgba;
                std::string err;
                if (!png::decode(bytes, w, h, rgba, err)) throw std::runtime_error(src + ": " + err);
                const std::vector<uint8_t> like = disk->get(target);
                const size_t slash = target.find_last_of('/');
                const std::vector<uint8_t> idx = slash == std::string::npos ? std::vector<uint8_t>() : disk->get(target.substr(0, slash) + "/system.idx");
                try {
                    disk->put(target, pythondisk::picture(like, rgba.data(), w, h, idx), name + ": " + target + " (from " + rel + ")");
                } catch (const std::exception &e) {
                    throw std::runtime_error(src + ": " + e.what());
                }
                laid = true;
                break;
            }
            if (disk->has(rel)) {
                disk->put(rel, bytes, label);
                laid = true;
                break;
            }
        }
        for (const std::string &image : images) {
            if (laid || python_disks.count(image)) break;
            laid = iso_file(image, rel, bytes, label, true);
        }
        if (!laid && !left_out++) first_left_out = rel;
    }
    if (left_out)
        fprintf(stderr, "mods: %s: %d file%s%s on none of the game's media, left out (%s%s)\n", name.c_str(), left_out,
                left_out == 1 ? "" : "s", sub.empty() ? "" : (" of " + sub).c_str(), first_left_out.c_str(), left_out > 1 ? ", ..." : "");
}

}  // namespace

bool load(const std::vector<std::string> &dirs, const std::string &game, const std::string &root, std::string &err) {
    try {
        const auto medium_paths = media(root, game);
        std::map<std::string, std::unique_ptr<pythondisk::Files>> python_disks;   // [[file]]s on Konami Python disks
        g_big_endian = core().mod_big_endian();
        g_listed.clear();   // (the game's folders as they are now)
        g_settings_dir = user_data_dir(root) + "/mod-settings/" + game;
        std::vector<std::vector<Hook>> mod_hooks;   // each mod's [[hook]]s, last mod first (put in at the end)
        // The first mod in the list wins where two change the same thing, as with ifs_layeredfs: a
        // later range, file or patch wins, so the list is laid on from its last mod to its first
        for (auto d = dirs.rbegin(); d != dirs.rend(); ++d) {
            const std::string &dir = *d;
            const size_t patches_before = g_patches.size();
            if (!std::filesystem::is_directory(fs_path(dir))) throw std::runtime_error(dir + ": no such mod folder");
            const std::string file = dir + "/mod.toml";
            const std::string text = read_file(file);
            // no mod.toml: a folder of files laid over the game's (below), named after the folder
            const std::vector<Table> tables = text.empty() ? std::vector<Table>(1) : parse(text, file);
            Mod mod{dir, utf8(fs_path(dir).filename()), "", {}, {}};
            const std::string folder = mod.name;
            std::set<std::string> named;   // the files its tables name (not laid over the game's)
            // the mod API's (docs/MODS.md, "Hooks, settings and actions"): what the mod says of
            // itself, and its tables, put in once the whole file is read
            int api = 0;
            std::vector<std::string> needs;
            bool needs_given = false;
            std::string program, script;
            std::map<std::string, std::string> used;   // capability -> the first table of it ("[[hook]]")
            struct PendingHook { Hook h; std::string setting, count, call; };
            struct PendingAction { Action a; std::string setting, where; };
            std::vector<PendingHook> hooks;
            std::vector<PendingAction> actions;
            // [[files]]: its folders (paths in the mod's), each "folder (when)", and those whose `when`
            // holds this run, in the file's order (the first wins)
            std::vector<std::string> files_folders;
            std::vector<std::pair<std::string, std::string>> chosen;
            int left_by_when = 0;   // tables whose `when` doesn't hold: read and checked, not used
            // Its top and its [[setting]]s are read first, then its settings as kept and --mod-setting's
            // (settings_as_kept), then the rest: a `when` sees the values this run plays with
            std::vector<const Table *> order;
            for (const Table &t : tables)
                if (t.kind.empty() || t.kind == "setting") order.push_back(&t);
            const size_t rest = order.size();
            for (const Table &t : tables)
                if (!t.kind.empty() && t.kind != "setting") order.push_back(&t);
            std::map<std::string, std::string> kv;   // its settings file's lines
            std::string kept;
            bool kept_read = false;
            auto settings_as_kept = [&] {
                if (kept_read) return;
                kept_read = true;
                if (!used.count("settings")) return;
                // its settings as last kept (<user data>/mod-settings/<game>/<folder>.txt): a number,
                // or a choice's name
                kept = settings_file(folder);
                kv = read_kv(kept);
                for (Setting &s : g_settings) {
                    if (s.folder != folder) continue;
                    auto it = kv.find(s.name);
                    const char *from = "the mod's default";
                    int64_t v;
                    if (it != kv.end() && modsettings::value_of(def_of(s), it->second, v)) {
                        s.value = std::clamp<int64_t>(v, s.min, s.max);
                        from = "kept";
                    }
                    fprintf(stderr, "mods: %s: %s = %s (%s)\n", mod.name.c_str(), s.name.c_str(), shown(s).c_str(), from);
                }
                if (!kv.empty()) fprintf(stderr, "mods: %s: settings from %s\n", mod.name.c_str(), kept.c_str());
                // --mod-setting: this run's values (checked once all mods are read, below)
                for (const auto &[name, value] : g_overrides)
                    for (Setting &s : g_settings) {
                        if (s.folder != folder || (s.name != name && s.folder + "/" + s.name != name)) continue;
                        int64_t v;
                        if (value.empty() || !modsettings::value_of(def_of(s), value, v))
                            throw std::runtime_error("--mod-setting " + name + "=" + value + ": a whole number" +
                                                     (s.choices.empty() ? std::string() : " or one of its choices"));
                        s.value = std::clamp<int64_t>(v, s.min, s.max);
                        fprintf(stderr, "mods: %s: %s = %s (--mod-setting, this run's)\n", s.mod.c_str(), s.name.c_str(), shown(s).c_str());
                    }
            };
            for (size_t k = 0; k < order.size(); k++) {
                if (k == rest) settings_as_kept();
                const Table &t = *order[k];
                const std::string where = file + (t.line ? ":" + std::to_string(t.line) : std::string());
                auto fail = [&](const std::string &why) { throw std::runtime_error(where + ": " + why); };
                auto str = [&](const char *k, bool need) -> std::string {
                    auto it = t.values.find(k);
                    if (it == t.values.end()) {
                        if (need) fail(std::string("'") + k + "' is missing");
                        return {};
                    }
                    if (!t.is_string.at(k)) fail(std::string("'") + k + "' is a string (in quotes)");
                    return it->second;
                };
                auto num = [&](const char *k) -> uint64_t {
                    auto it = t.values.find(k);
                    if (it == t.values.end()) fail(std::string("'") + k + "' is missing");
                    std::string v;
                    for (char c : it->second) if (c != '_') v += c;
                    char *end = nullptr;
                    const uint64_t r = std::strtoull(v.c_str(), &end, 0);
                    if (v.empty() || *end) fail(std::string("'") + k + "' isn't a number");
                    return r;
                };
                auto inum = [&](const char *k) -> int64_t {   // a signed number
                    auto it = t.values.find(k);
                    if (it == t.values.end()) fail(std::string("'") + k + "' is missing");
                    if (t.is_string.at(k)) fail(std::string("'") + k + "' is a number (no quotes)");
                    std::string v;
                    for (char c : it->second) if (c != '_') v += c;
                    char *end = nullptr;
                    errno = 0;
                    const long long r = std::strtoll(v.c_str(), &end, 0);
                    if (v.empty() || *end || errno) fail(std::string("'") + k + "' isn't a whole number");
                    return r;
                };
                auto ident = [&](const char *k) {   // a name: letters, digits, _ and -
                    const std::string s = str(k, true);
                    if (s.empty() || !std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '-'; }))
                        fail(std::string("'") + k + "' is a name of letters, digits, _ and - (\"" + s + "\")");
                    return s;
                };
                // a register by the machine's own name ("r3"), or memory: an address and its size
                auto place = [&](const char *k, const char *size_key) {
                    Place p;
                    if (t.is_string.at(k)) {
                        const std::string name = t.values.at(k);
                        const std::vector<std::string> regs = core().mod_registers();
                        for (size_t i = 0; i < regs.size(); i++)
                            if (regs[i] == name) p.reg = (int)i;
                        if (p.reg < 0) {
                            std::string all;
                            for (const std::string &r : regs) all += (all.empty() ? "" : " ") + r;
                            fail("no register '" + name + "' on this machine" + (all.empty() ? std::string(" (its core names none)") : " (" + all + ")"));
                        }
                        if (t.values.count(size_key)) fail(std::string("'") + size_key + "' is for memory, not a register");
                        return p;
                    }
                    p.addr = (uint32_t)num(k);
                    if (t.values.count(size_key)) {
                        p.size = (int)inum(size_key);
                        if (p.size != 1 && p.size != 2 && p.size != 4) fail(std::string("'") + size_key + "' is 1, 2 or 4 (bytes)");
                    }
                    return p;
                };
                auto real = [&](const char *k) -> double {
                    auto it = t.values.find(k);
                    if (it == t.values.end()) fail(std::string("'") + k + "' is missing");
                    char *end = nullptr;
                    const double r = std::strtod(it->second.c_str(), &end);
                    if (it->second.empty() || *end) fail(std::string("'") + k + "' isn't a number");
                    return r;
                };
                auto flag = [&](const char *k) {
                    auto it = t.values.find(k);
                    if (it == t.values.end()) return false;
                    if (it->second != "true" && it->second != "false") fail(std::string("'") + k + "' is true or false");
                    return it->second == "true";
                };
                auto hex = [&](const char *k) {
                    std::vector<uint8_t> b;
                    std::string h;
                    for (char c : str(k, true))
                        if (!std::isspace((unsigned char)c)) h += c;
                    if (h.size() % 2) fail(std::string("'") + k + "' has an odd number of hex digits");
                    for (size_t i = 0; i < h.size(); i += 2) {
                        if (!std::isxdigit((unsigned char)h[i]) || !std::isxdigit((unsigned char)h[i + 1])) fail(std::string("'") + k + "' isn't hex");
                        b.push_back((uint8_t)std::stoul(h.substr(i, 2), nullptr, 16));
                    }
                    return b;
                };
                auto known = [&](std::initializer_list<const char *> keys) {
                    for (const auto &[k, v] : t.values) {
                        bool ok = false;
                        for (const char *x : keys) ok |= k == x;
                        if (!ok) fail("unknown key '" + k + "'");
                    }
                };
                auto from = [&]() {
                    const std::string f = str("from", true);
                    if (f.find("..") != std::string::npos) fail("'from' is a file inside the mod's folder");
                    named.insert(f);
                    std::string data = read_file(dir + "/" + f);
                    if (data.empty()) fail("'from' file " + f + " not found (or empty)");
                    return std::vector<uint8_t>(data.begin(), data.end());
                };
                // `when = "SETTING=VALUE"` (not a [[hook]]'s, whose `when` is a register): the table is
                // read and checked as any, and used only while the mod's setting has that value at start
                bool off = false;
                if (!t.kind.empty() && t.kind != "hook" && t.kind != "setting" && t.kind != "action" && t.values.count("when")) {
                    used.try_emplace("choose", "'when'");
                    std::vector<modsettings::Setting> defs;
                    std::vector<Setting *> mine;
                    for (Setting &s : g_settings)
                        if (s.folder == folder) defs.push_back(def_of(s)), mine.push_back(&s);
                    size_t which = 0;
                    int64_t value = 0;
                    std::string why;
                    if (!modsettings::condition(defs, str("when", true), which, value, why)) fail(why);
                    mine[which]->chooses = true;
                    off = mine[which]->value.load() != value;
                    left_by_when += off;
                }
                if (t.kind.empty()) {
                    if (text.empty()) continue;
                    known({"name", "version", "game", "author", "description", "api", "requires", "program", "script"});
                    mod.name = str("name", true);
                    mod.version = str("version", false);
                    if (const std::string g = str("game", true); g != game) fail("this mod is for " + g + ", not " + game);
                    if (t.values.count("api")) {
                        api = (int)inum("api");
                        if (api > API) fail("this mod is for mod API " + std::to_string(api) + ", this HIRAKU's is " + std::to_string(API) + " (a newer HIRAKU's?)");
                        if (api < 1) fail("'api' is 1");
                    }
                    if (t.values.count("requires")) {   // ["hook", "settings"]: strings in a list, on one line
                        if (!api) fail("'requires' is the mod API's: api = 1 goes with it");
                        if (t.is_string.at("requires")) fail("'requires' is a list: [\"hook\", \"settings\"]");
                        std::string v = t.values.at("requires");
                        if (v.size() < 2 || v.front() != '[' || v.back() != ']') fail("'requires' is a list on one line: [\"hook\", \"settings\"]");
                        v = v.substr(1, v.size() - 2);
                        for (size_t at = 0;;) {
                            at = v.find_first_not_of(" \t,", at);
                            if (at == std::string::npos) break;
                            const size_t close = v[at] == '"' ? v.find('"', at + 1) : std::string::npos;
                            if (close == std::string::npos) fail("'requires' is a list of names in quotes: [\"hook\", \"settings\"]");
                            needs.push_back(v.substr(at + 1, close - at - 1));
                            at = close + 1;
                        }
                        needs_given = true;
                    }
                    if (t.values.count("script")) {   // its Lua script (host/modscript.h): a file in its folder
                        if (!api) fail("'script' is the mod API's: api = 1 goes with it");
                        script = str("script", true);
                        const std::filesystem::path sp = fs_path(script);
                        if (script.empty() || sp.is_absolute() || sp.has_root_name() || script.find("..") != std::string::npos ||
                            script.find(':') != std::string::npos)
                            fail("'script' is a file in the mod's folder (\"mod.lua\")");
                        std::error_code ec;
                        if (!std::filesystem::is_regular_file(fs_path(dir + "/" + script), ec)) fail("'script' file " + script + " not found");
                        named.insert(script);
                        used.try_emplace("script", "script = \"" + script + "\"");
                    }
                    if (t.values.count("program")) {   // the SHA-1 of the program it was made for
                        if (!api) fail("'program' is the mod API's: api = 1 goes with it");
                        program = str("program", true);
                        std::transform(program.begin(), program.end(), program.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                        if (program.size() != 40 || !std::all_of(program.begin(), program.end(), [](unsigned char c) { return std::isxdigit(c); }))
                            fail("'program' is a SHA-1: 40 hex digits (the program image's, as games/" + game + "/game.toml's [program] sha1)");
                    }
                } else if (t.kind == "setting") {
                    used.try_emplace("settings", "[[setting]]");
                    if (t.values.count("choices")) used.try_emplace("choose", "'choices'");
                    const modsettings::Setting d = modsettings::setting(t, where);   // (the launcher's reader too)
                    for (const Setting &s : g_settings)
                        if (s.folder == folder && s.name == d.name) fail("a second setting '" + d.name + "'");
                    Setting &s = g_settings.emplace_back();
                    s.mod = mod.name, s.folder = folder, s.name = d.name, s.label = d.label;
                    s.min = d.min, s.max = d.max, s.dflt = d.dflt, s.step = d.step, s.choices = d.choices;
                    s.value = s.dflt;
                } else if (t.kind == "action") {
                    used.try_emplace("actions", "[[action]]");
                    const modsettings::Action d = modsettings::action(t, where);
                    if (!d.notify) used.try_emplace("quiet", "an [[action]] with notify = false");
                    PendingAction p;
                    p.a.mod = mod.name;
                    p.a.name = d.name;
                    for (const PendingAction &q : actions)
                        if (q.a.name == p.a.name) fail("a second action '" + p.a.name + "'");
                    p.a.label = d.label;
                    p.a.binding = d.binding;
                    p.a.what = d.what == "up" ? Action::Do::up : d.what == "down" ? Action::Do::down : Action::Do::toggle;
                    p.a.notify = d.notify;
                    p.setting = d.setting;
                    p.where = where;
                    actions.push_back(std::move(p));
                } else if (t.kind == "hook") {
                    used.try_emplace("hook", "[[hook]]");
                    known({"at", "set", "add", "sub", "count", "call", "value", "size", "scale", "when", "when_size", "min", "max"});
                    PendingHook p;
                    Hook &h = p.h;
                    h.mod = mod.name;
                    h.where = where;
                    h.at = (uint32_t)num("at");
                    const char *does = nullptr;
                    for (const char *k : {"set", "add", "sub", "count", "call"})
                        if (t.values.count(k)) {
                            if (does) fail("a hook does one thing: set, add, sub, count or call");
                            does = k;
                        }
                    if (!does) fail("a hook does one thing: set, add, sub (a register or memory, by a value), count or call (a function of the mod's script)");
                    if (!strcmp(does, "call")) {
                        h.op = Hook::Op::call;
                        p.call = ident("call");
                        used.try_emplace("script", "[[hook]] with call");
                        for (const char *k : {"value", "scale", "size"})
                            if (t.values.count(k)) fail(std::string("'") + k + "' isn't a call's");
                    } else if (!strcmp(does, "count")) {
                        h.op = Hook::Op::count;
                        p.count = ident("count");
                        for (const char *k : {"value", "scale", "size"})
                            if (t.values.count(k)) fail(std::string("'") + k + "' isn't a counter's");
                    } else {
                        h.op = !strcmp(does, "set") ? Hook::Op::set : !strcmp(does, "add") ? Hook::Op::add : Hook::Op::sub;
                        h.target = place(does, "size");
                        if (!t.values.count("value")) fail("'value' is missing (a number, or \"setting:NAME\")");
                        if (t.is_string.at("value")) {
                            const std::string v = t.values.at("value");
                            if (v.rfind("setting:", 0) != 0 || v.size() == 8) fail("'value' is a number, or \"setting:NAME\" (one of the mod's [[setting]]s)");
                            p.setting = v.substr(8);
                        } else {
                            h.imm = inum("value");
                        }
                        if (t.values.count("scale")) {
                            h.scale = real("scale");
                            if (!std::isfinite(h.scale)) fail("'scale' is a number");
                        }
                    }
                    if (t.values.count("when")) {
                        h.when = true;
                        h.when_at = place("when", "when_size");
                        if (!t.values.count("min") && !t.values.count("max")) fail("'when' goes with 'min', 'max' or both");
                        if (t.values.count("min")) h.min = inum("min");
                        if (t.values.count("max")) h.max = inum("max");
                    } else if (t.values.count("min") || t.values.count("max") || t.values.count("when_size")) {
                        fail("'min', 'max' and 'when_size' go with 'when'");
                    }
                    hooks.push_back(std::move(p));
                } else if (t.kind == "patch") {
                    known({"address", "module", "original", "replace", "once", "while_rate", "between_ticks", "per_picture", "when"});
                    Patch p;
                    p.mod = mod.name;
                    p.address = (uint32_t)num("address");
                    p.module = str("module", false);
                    if (!p.module.empty() && (p.while_rate || flag("while_rate") || flag("between_ticks") || flag("per_picture")))
                        fail("a patch in a module (the PC games) can't be tied to a [[rate]]");
                    if (t.values.count("original")) p.original = hex("original");   // none: written every frame (a cheat)
                    p.per_picture = flag("per_picture");
                    if (p.per_picture) {   // its replace is the rate's: made as the rate comes on
                        if (p.original.size() != 4 || t.values.count("replace") || flag("between_ticks"))
                            fail("a per_picture patch is a float (4 bytes of 'original'), without 'replace' or between_ticks");
                        p.replace = p.original;
                    } else
                        p.replace = hex("replace");
                    p.once = flag("once");
                    p.between_ticks = flag("between_ticks");
                    p.while_rate = flag("while_rate") || p.between_ticks || p.per_picture;
                    if (p.while_rate && (p.once || !t.values.count("original")))
                        fail("a while_rate patch has its 'original' (put back when the rate is off) and isn't 'once'");
                    if (p.replace.empty()) fail("'replace' is empty");
                    if (!p.original.empty() && p.original.size() != p.replace.size()) fail("'original' and 'replace' are as long as each other");
                    if (!off) g_patches.push_back(std::move(p));
                } else if (t.kind == "text") {
                    known({"find", "replace", "from", "to", "every", "when"});
                    Text x;
                    x.t.mod = mod.name;
                    x.t.line = where;
                    x.t.label = str("find", true);
                    if (!to_sjis(str("find", true), x.t.find)) fail("'find' has a character Shift-JIS hasn't");
                    if (!to_sjis(str("replace", true), x.t.replace)) fail("'replace' has a character Shift-JIS hasn't");
                    if (x.t.find.empty()) fail("'find' is empty");
                    if (std::count(x.t.find.begin(), x.t.find.end(), 0) || std::count(x.t.replace.begin(), x.t.replace.end(), 0))
                        fail("a text has no zero byte in it");
                    if (t.values.count("from") != t.values.count("to")) fail("'from' and 'to' go together");
                    if (t.values.count("from")) {
                        x.t.from = (uint32_t)num("from"), x.t.to = (uint32_t)num("to");
                        if (x.t.to <= x.t.from) fail("'to' is after 'from'");
                    }
                    x.t.every = flag("every");
                    if (!off) g_texts.push_back(std::move(x));
                } else if (t.kind == "rate") {
                    known({"hz", "while_changing", "while_equal", "value", "chart_times", "when"});
                    Rate r;
                    r.mod = mod.name;
                    if (t.values.count("hz") && t.is_string.at("hz")) {
                        const std::string h = str("hz", true);
                        if (h.size() == 2 && h[0] == 'x' && h[1] >= '2' && h[1] <= '8') r.times = h[1] - '0';
                        else if (h != "display")
                            fail("'hz' is a number, \"display\" (the display's refresh rate) or \"x2\" to \"x8\" (times the machine's own)");
                    } else {
                        r.hz = real("hz");
                        if (r.hz < 30 || r.hz > 1000) fail("'hz' is between 30 and 1000");
                    }
                    if (t.values.count("while_changing")) r.when.address = (uint32_t)num("while_changing");
                    else r.when.always = true;
                    if (t.values.count("while_equal") || t.values.count("value")) {
                        r.when.gated = true;
                        r.when.gate = (uint32_t)num("while_equal");
                        r.when.value = (uint32_t)num("value");
                    }
                    if (t.values.count("chart_times")) {
                        r.chart_times = (int)num("chart_times");
                        if (r.chart_times < 2 || r.chart_times > 8 || r.chart_times != r.times)
                            fail("'chart_times' is the rate's \"x\" (2 to 8): the charts counted in frames as many times shorter");
                    }
                    if (!off) g_rates.push_back(r);
                } else if (t.kind == "bytes") {
                    known({"medium", "offset", "from", "when"});
                    const std::string medium = str("medium", true);
                    auto it = medium_paths.find(medium);
                    if (it == medium_paths.end()) fail("no medium '" + medium + "' in dumps/" + game + "/machine.txt");
                    std::error_code ec;
                    if (std::filesystem::is_directory(fs_path(it->second), ec))
                        fail("'" + medium + "' is a folder: its files are changed whole ([[file]], or the files at their paths)");
                    char what[64];
                    snprintf(what, sizeof what, ": %s bytes at 0x%llx", medium.c_str(), (unsigned long long)num("offset"));
                    auto data = from();
                    if (!off) overlay::add(it->second, num("offset"), std::move(data), mod.name + what);
                } else if (t.kind == "file") {
                    known({"medium", "path", "from", "when"});
                    const std::string medium = str("medium", true);
                    auto it = medium_paths.find(medium);
                    if (it == medium_paths.end()) fail("no medium '" + medium + "' in dumps/" + game + "/machine.txt");
                    std::error_code ec;
                    if (std::filesystem::is_directory(fs_path(it->second), ec)) {   // a PC game's folder: the file, whole
                        std::string path = str("path", true);
                        for (char &c : path)
                            if (c == '\\') c = '/';
                        const std::string game_file = in_folder(it->second, path);
                        // a path the game's data folder lacks: a new file there (new_in_folder: none
                        // with an empty, "." or ".." part, nor over a folder of the game's)
                        const std::string new_file = game_file.empty() && medium == "data" ? new_in_folder(it->second, path) : "";
                        if (game_file.empty() && new_file.empty()) fail("no " + str("path", true) + " in " + it->second);
                        const std::string f = str("from", true);
                        if (f.find("..") != std::string::npos) fail("'from' is a file inside the mod's folder");
                        if (!std::filesystem::is_regular_file(fs_path(dir + "/" + f), ec)) fail("'from' file " + f + " not found");
                        named.insert(f);
                        if (off) continue;
                        if (game_file.empty()) add_file(new_file, dir + "/" + f);
                        else g_folder_files[game_file] = dir + "/" + f;
                        continue;
                    }
                    try {
                        const std::string label = mod.name + ": " + str("path", true);
                        auto data = from();
                        if (off) continue;
                        if (!python_disks.count(it->second) && !iso_file(it->second, str("path", true), data, label)) {
                            auto disk = pythondisk::Files::open(it->second);
                            if (!disk) throw std::runtime_error(it->second + ": neither an ISO 9660 disc nor a disk whose file system HIRAKU knows");
                            python_disks[it->second] = std::move(disk);
                        }
                        if (python_disks.count(it->second)) python_disks[it->second]->put(str("path", true), data, label);
                    } catch (const std::exception &e) {
                        fail(e.what());
                    }
                } else if (t.kind == "files") {
                    // a folder of the mod's laid over the game's files as the mod's own folder is, its
                    // files winning over the mod's own; never laid at its own path in the mod's
                    known({"folder", "when"});
                    used.try_emplace("choose", "[[files]]");
                    std::string f = str("folder", true);
                    for (char &c : f)
                        if (c == '\\') c = '/';
                    while (!f.empty() && f.back() == '/') f.pop_back();
                    const std::filesystem::path fp = fs_path(f);
                    if (f.empty() || f[0] == '/' || f[0] == '.' || fp.has_root_name() || f.find("..") != std::string::npos ||
                        f.find(':') != std::string::npos || f.find("/.") != std::string::npos)
                        fail("'folder' is a folder inside the mod's, not hidden (\"alt/ps2-timing\")");
                    std::error_code ec;
                    if (!std::filesystem::is_directory(fs_path(dir + "/" + f), ec)) fail("'folder' " + f + " isn't a folder in the mod's");
                    for (const std::string &o : files_folders)
                        if (modsettings::same_name(o, f)) fail("a second [[files]] for " + f);
                    files_folders.push_back(f);
                    if (!off) chosen.emplace_back(f, t.values.count("when") ? t.values.at("when") : std::string());
                } else {
                    fail("unknown table [[" + t.kind + "]]");
                }
            }
            settings_as_kept();   // (a mod of settings alone)
            // the mod API: what the mod needs, against what this build has; the program it's for
            auto fail = [&](const std::string &why) { throw std::runtime_error(file + ": " + why); };
            if (!used.empty() && !api)
                fail(used.begin()->second + " is the mod API's: the mod says api = 1 at its top (and requires = [\"" + used.begin()->first + "\"])");
            for (const auto &[cap, table] : used)
                if (std::find(needs.begin(), needs.end(), cap) == needs.end())
                    fail("this mod has " + table + " but its 'requires' doesn't list \"" + cap + "\"" + (needs_given ? "" : " (it has no 'requires')"));
            {
                const std::vector<std::string> have = capabilities();
                std::string missing, all;
                for (const std::string &n : needs)
                    if (std::find(have.begin(), have.end(), n) == have.end()) missing += (missing.empty() ? "" : ", ") + n;
                for (const std::string &h : have) all += (all.empty() ? "" : ", ") + h;
                if (!missing.empty()) {
                    std::string why = "this mod needs " + missing + "; this core/build has none (it has: " + (all.empty() ? "nothing of the mod API" : all) + ")";
                    if (const std::string other = core().mod_other_program(); !other.empty()) {
                        why += ". " + other + " has it: start that one, with --game " + game;
                        g_hand_over = other;
                    }
                    fail(why);
                }
            }
            if (!program.empty()) {
                const std::string path = core().mod_program_file(root, game);
                if (path.empty()) fail("this machine's core doesn't say which program it runs, so 'program' can't be checked");
                std::ifstream f(fs_path(path), std::ios::binary);
                if (!f) fail("can't read the program " + path + " ('program')");
                Sha1 sha;
                char buf[65536];
                while (f.read(buf, sizeof buf) || f.gcount()) sha.add(buf, (size_t)f.gcount());
                const std::string have = sha.hex();
                if (have != program) fail("this mod is for the program whose SHA-1 is " + program + "; this run's (" + path + ") is " + have);
            }
            // its hooks' and actions' settings and counters, by name
            auto setting_of = [&](const std::string &name, const std::string &where) -> Setting * {
                for (Setting &s : g_settings)
                    if (s.folder == folder && s.name == name) return &s;
                throw std::runtime_error(where + ": no [[setting]] '" + name + "' in this mod");
            };
            // its script: run once now (its functions defined), refused with the mod if it fails
            int script_id = -1;
            if (!script.empty()) {
                std::string why;
                script_id = modscript::load(mod.name, folder, dir + "/" + script, needs, why);
                if (script_id < 0) fail(why);
            } else if (used.count("script")) {
                fail("a [[hook]] calls a function of the mod's script, and the mod has no script = \"...\"");
            }
            mod_hooks.emplace_back();
            for (PendingHook &p : hooks) {
                if (!p.call.empty()) {
                    p.h.script = script_id;
                    p.h.ref = modscript::function_ref(script_id, p.call);
                    if (!p.h.ref) throw std::runtime_error(p.h.where + ": its script has no function " + p.call + " (a global one)");
                }
                if (!p.setting.empty()) p.h.setting = setting_of(p.setting, p.h.where);
                if (!p.count.empty()) {
                    for (Counter &c : g_counters)
                        if (c.folder == folder && c.name == p.count) p.h.counter = &c;
                    if (!p.h.counter) {
                        Counter &c = g_counters.emplace_back();
                        c.mod = mod.name, c.folder = folder, c.name = p.count;
                        p.h.counter = &c;
                    }
                }
                mod_hooks.back().push_back(std::move(p.h));
            }
            for (PendingAction &p : actions) {
                p.a.setting = setting_of(p.setting, p.where);
                if (p.a.label.empty()) p.a.label = p.a.name;
                g_actions.push_back(std::move(p.a));
            }
            // the actions' bindings as the player changed them in its settings file
            for (Action &a : g_actions)
                if (a.setting && a.setting->folder == folder)
                    if (auto it = kv.find("action." + a.name); it != kv.end() && !it->second.empty()) a.binding = it->second;
            // its files: its own folder's, then its chosen [[files]] folders' over them (the first last: it wins)
            lay_files(dir, mod.name, named, files_folders, "", medium_paths, python_disks);
            for (auto c = chosen.rbegin(); c != chosen.rend(); ++c) {
                fprintf(stderr, "mods: %s: the files of %s%s\n", mod.name.c_str(), c->first.c_str(),
                        c->second.empty() ? "" : (" (" + c->second + ")").c_str());
                lay_files(dir, mod.name, named, files_folders, c->first, medium_paths, python_disks);
            }
            if (left_by_when)
                fprintf(stderr, "mods: %s: %d table%s left out by %s 'when'\n", mod.name.c_str(), left_by_when, left_by_when == 1 ? "" : "s",
                        left_by_when == 1 ? "its" : "their");
            for (const std::string &f : files_folders) mod.folders.push_back(f);
            for (const auto &c : chosen) mod.chosen.push_back(c.first);
            g_mods.insert(g_mods.begin(), mod);   // in the list's order
            // ... and its memory patches before those of the mods after it: patches go in in their
            // order, and the one that finds its 'original' bytes there first is the one in place
            std::rotate(g_patches.begin(), g_patches.begin() + (ptrdiff_t)patches_before, g_patches.end());
        }
        // A patch with no 'original' is written every frame whatever is there: over a patch of a mod
        // before it in the list it would win (and the two be written in turn, the code translated
        // again each time). It is left out, and said
        for (size_t i = 0; i < g_patches.size(); i++)
            for (size_t j = 0; j < i && !g_patches[i].under; j++) {
                Patch &a = g_patches[j], &b = g_patches[i];
                if (a.mod == b.mod || a.module != b.module || !b.original.empty()) continue;
                if ((uint64_t)a.address < (uint64_t)b.address + b.replace.size() && (uint64_t)b.address < (uint64_t)a.address + a.replace.size()) {
                    b.under = true;
                    fprintf(stderr, "mods: %s: its patch at 0x%08x is left out: %s, before it in the list, changes the same bytes\n",
                            b.mod.c_str(), b.address, a.mod.c_str());
                }
            }
        for (auto &d : python_disks) d.second->finish();
        // the hooks, in the list's order (its first mod's first): several at one instruction run so
        if (!mod_hooks.empty())
            for (auto m = mod_hooks.rbegin(); m != mod_hooks.rend(); ++m)
                for (Hook &h : *m) {
                    Hook &kept = g_hooks.emplace_back(std::move(h));
                    std::string why;
                    if (!core().mod_hook(kept.at, run_hook, &kept, kept.where, why)) throw std::runtime_error(kept.where + ": " + why);
                }
        // --mod-setting: this run's values, each set as its mod was read (before its `when`s): here,
        // a name no mod has, or two mods have, refuses the run
        for (const auto &[name, value] : g_overrides) {
            int n = 0;
            for (Setting &s : g_settings) n += s.name == name || s.folder + "/" + s.name == name;
            if (!n) throw std::runtime_error("--mod-setting " + name + ": no mod here has a setting of that name");
            if (n > 1) throw std::runtime_error("--mod-setting " + name + ": two mods have one: give it as FOLDER/" + name);
        }
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }
    return true;
}

void display_rate(double hz) { g_display_hz = hz; }

void frame() {
    for (Rate &r : g_rates) {
        if (r.refused || core().frame_repeated()) continue;
        bool known;
        const bool want = r.when.step(r.on, [](uint32_t a) { return core().mem(a, 4); }, known);
        if (!known) continue;
        // hz = "display": follow the display (a window moved to another one)
        const double hz = r.times ? r.times * core().native_rate() : r.hz > 0 ? r.hz : g_display_hz > 30 ? g_display_hz : kDisplayFallbackHz;
        if (want == r.on && (!want || hz == r.now)) continue;
        if (!core().set_vsync_rate(want ? hz : 0)) {
            fprintf(stderr, "mods: %s: this machine's core can't change its vsync rate ([[rate]] ignored)\n", r.mod.c_str());
            r.refused = true;
            continue;
        }
        r.on = want;
        r.now = want ? hz : 0;
        fprintf(stderr, "mods: %s: frame %llu, vsync at %s\n", r.mod.c_str(), (unsigned long long)core().frame_count(),
                want ? (std::to_string(hz) + " Hz").c_str() : "the machine's own rate");
    }
    bool rate_on = false;
    double rate_hz = 0;
    for (const Rate &r : g_rates)
        if (r.on) rate_on = true, rate_hz = r.now;
    // the coming frame: a cabinet tick when the time owed reaches a cabinet frame
    if (rate_on) {
        g_owed += core().native_rate() / rate_hz;
        g_tick = g_owed >= 1 - 1e-9;
        if (g_tick) g_owed -= 1;
    } else {
        g_owed = 0;
        g_tick = true;
    }
    // [[text]]: looked for once a second until found, then patches as any
    const uint64_t f = core().frame_count();
    for (Text &x : g_texts) {
        if (x.done || f < x.look_at) continue;
        x.look_at = f + 60;
        if (!x.t.to) {
            fprintf(stderr, "mods: %s: a [[text]] on this machine needs 'from' and 'to' (where to look) (%s)\n", x.t.mod.c_str(),
                    x.t.line.c_str());
            x.done = true;
            continue;
        }
        const uint8_t *m = core().mem(x.t.from, x.t.to - x.t.from);
        if (!m) continue;
        std::string why;
        const auto found = find_text(x.t, m, x.t.to - x.t.from, x.t.from, &why);
        if (found.empty() && why.empty()) continue;   // not loaded yet
        x.done = true;
        if (!why.empty()) {
            fprintf(stderr, "mods: %s: %s\n", x.t.mod.c_str(), why.c_str());
            continue;
        }
        for (const MemoryPatch &mp : found) {
            Patch p;
            p.mod = mp.mod, p.address = mp.address, p.original = mp.original, p.replace = mp.replace;
            g_patches.push_back(std::move(p));
        }
    }
    for (Patch &p : g_patches) {
        if (p.done || p.under || !p.module.empty()) continue;   // (a module's: the PC games' process puts it in)
        if (p.per_picture && rate_on) {   // a cabinet frame's time: the rate's picture's
            float f;
            memcpy(&f, p.original.data(), 4);
            f = (float)(f * core().native_rate() / rate_hz);
            if (memcmp(&f, p.replace.data(), 4) != 0) {   // a new rate: the last one's value out first
                auto *m = const_cast<uint8_t *>(core().mem(p.address, 4));
                if (m && memcmp(m, p.replace.data(), 4) == 0 && memcmp(p.replace.data(), p.original.data(), 4) != 0) {
                    memcpy(m, p.original.data(), 4);
                    core().code_changed(p.address, 4);
                }
                memcpy(p.replace.data(), &f, 4);
            }
        }
        if (p.while_rate && (!rate_on || (p.between_ticks && g_tick))) {   // the original back, if the patch is there
            const size_t n = p.original.size();
            auto *m = const_cast<uint8_t *>(core().mem(p.address, (uint32_t)n));
            if (m && memcmp(m, p.replace.data(), n) == 0) {
                memcpy(m, p.original.data(), n);
                core().code_changed(p.address, (uint32_t)n);
            }
            continue;
        }
        const size_t n = p.replace.size();
        auto *m = const_cast<uint8_t *>(core().mem(p.address, (uint32_t)n));
        if (!m || memcmp(m, p.replace.data(), n) == 0) continue;   // not mapped, or in place
        if (!p.original.empty() && memcmp(m, p.original.data(), n) != 0) continue;   // not loaded (yet): wait
        memcpy(m, p.replace.data(), n);
        core().code_changed(p.address, (uint32_t)n);
        if (!p.logged) {
            fprintf(stderr, "mods: %s: patch at 0x%08x (%zu bytes) in place\n", p.mod.c_str(), p.address, n);
            p.logged = true;
        }
        if (p.once) p.done = true;
    }
}

std::vector<MemoryPatch> memory_patches() {
    std::vector<MemoryPatch> out;
    for (const Patch &p : g_patches)
        if (!p.while_rate && !p.under) out.push_back({p.mod, p.address, p.module, p.original, p.replace, p.once});
    return out;
}

std::vector<RatePatch> rate_patches() {
    std::vector<RatePatch> out;
    for (const Patch &p : g_patches)
        if (p.while_rate && p.module.empty()) out.push_back({{p.mod, p.address, p.module, p.original, p.replace, false}, p.between_ticks, p.per_picture});
    return out;
}

std::vector<uint32_t> rate_words() {
    std::vector<uint32_t> out;
    for (const Rate &r : g_rates) {
        if (!r.when.always) out.push_back(r.when.address);
        if (r.when.gated) out.push_back(r.when.gate);
    }
    return out;
}

int chart_times() {
    int k = 1;
    for (const Rate &r : g_rates) k = std::max(k, r.chart_times);
    return k;
}

std::vector<TextPatch> text_patches() {
    std::vector<TextPatch> out;
    for (Text &x : g_texts) out.push_back(x.t), x.done = true;   // the core's to look for now, not frame()'s
    return out;
}

std::vector<MemoryPatch> find_text(const TextPatch &t, const uint8_t *mem, size_t n, uint32_t base, std::string *why) {
    std::vector<MemoryPatch> out;
    const size_t len = t.find.size();
    size_t places = 0, tight = SIZE_MAX;
    for (size_t i = 0; i + len < n; i++) {
        if (mem[i] != t.find[0] || (i && mem[i - 1]) || mem[i + len] || memcmp(mem + i, t.find.data(), len)) continue;
        places++;
        // its room: the string, its zero and the zeros after it (up to the next string), a zero kept
        size_t room = len;
        while (i + room < n && !mem[i + room] && room < len + 256) room++;
        const size_t w = std::max(len, t.replace.size() + 1);
        if (w > room) {
            tight = std::min(tight, room - 1);
            continue;
        }
        MemoryPatch p;
        p.mod = t.mod, p.address = base + (uint32_t)i;
        p.original.assign(mem + i, mem + i + w);
        p.replace = t.replace;
        p.replace.resize(w, 0);
        out.push_back(std::move(p));
    }
    char a[96];
    if (places > 1 && !t.every) {
        snprintf(a, sizeof a, "found in %zu places (give 'from' and 'to', or every = true)", places);
        *why = "\"" + t.label + "\" " + a + " (" + t.line + ")";
        out.clear();
    } else if (out.empty() && places) {
        snprintf(a, sizeof a, "has room for %zu bytes, the replacement is %zu", tight, t.replace.size());
        *why = "\"" + t.label + "\" " + a + " (" + t.line + ")";
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> folder_files() {
    return {g_folder_files.begin(), g_folder_files.end()};
}

std::vector<std::pair<std::string, std::string>> added_files() {
    std::vector<std::pair<std::string, std::string>> v;
    for (const auto &[low, file] : g_added_files) v.push_back(file);
    return v;
}

std::vector<std::string> folders() {
    std::vector<std::string> f;
    for (const Mod &m : g_mods) f.push_back(m.dir);
    return f;
}

std::vector<Layer> layers() {
    std::vector<Layer> out;
    for (const Mod &m : g_mods) {
        for (const std::string &c : m.chosen) {
            Layer l{m.dir + "/" + c, {}};
            for (const std::string &f : m.folders)   // (a [[files]] folder inside this one)
                if (f.size() > c.size() && f[c.size()] == '/' && modsettings::same_name(f.substr(0, c.size()), c)) l.left_out.push_back(f.substr(c.size() + 1));
            out.push_back(std::move(l));
        }
        out.push_back({m.dir, m.folders});
    }
    return out;
}

std::vector<std::string> describe() {
    std::vector<std::string> d;
    for (const Mod &m : g_mods) d.push_back(m.name + (m.version.empty() ? "" : " " + m.version) + " (" + m.dir + ")");
    return d;
}

// ---- the mod API ----------------------------------------------------------------------------

std::vector<std::string> capabilities() {
    std::vector<std::string> c = core().mod_capabilities();
    c.push_back("settings");   // (the shell's: settings files, read by hooks)
    c.push_back("script");     // ... a script (host/modscript.h), the controls the game is given
    c.push_back("input");
    c.push_back("choose");     // ... files and patches chosen by a setting (`when`, [[files]], `choices`)
    if (core().mod_memory()) c.push_back("memory");   // ... and the machine's memory, where it's its own
    for (const std::string &o : g_offered)
        if (std::find(c.begin(), c.end(), o) == c.end()) {
            c.push_back(o);
            if (o == "actions") c.push_back("quiet");   // ... and with them, actions that show no box (notify = false)
        }
    return c;
}

void offer(const std::string &capability) {
    if (std::find(g_offered.begin(), g_offered.end(), capability) == g_offered.end()) g_offered.push_back(capability);
}

std::string api_text(const std::string &root, const std::string &game) {
    std::string s = "mod API " + std::to_string(API) + "\ncapabilities:";
    for (const std::string &c : capabilities()) s += " " + c;
    std::string regs;
    for (const std::string &r : core().mod_registers()) regs += " " + r;
    s += "\nregisters:" + (regs.empty() ? std::string(" none") : regs);
    s += std::string("\nmemory: ") + (core().mod_big_endian() ? "big-endian" : "little-endian");
    const std::string path = core().mod_program_file(root, game);
    if (!path.empty()) {
        std::ifstream f(fs_path(path), std::ios::binary);
        if (f) {
            Sha1 sha;
            char buf[65536];
            while (f.read(buf, sizeof buf) || f.gcount()) sha.add(buf, (size_t)f.gcount());
            s += "\nprogram: " + sha.hex() + " (" + path + ")";
        } else {
            s += "\nprogram: " + path + " (not there)";
        }
    }
    if (const std::string other = core().mod_other_program(); !other.empty()) s += "\nanother program of this core's: " + other;
    return s + "\n";
}

void override_setting(const std::string &spec) {
    const size_t eq = spec.find('=');
    g_overrides.emplace_back(spec.substr(0, eq), eq == std::string::npos ? std::string() : spec.substr(eq + 1));
}

bool hand_over(int argc, char **argv, const std::string &game) {
    if (g_hand_over.empty()) return false;
#ifdef _WIN32
    (void)argc, (void)argv, (void)game;
    return false;
#else
    const std::string exe = sys::exe_path();
    if (exe.empty()) return false;
    const std::string path = exe.substr(0, exe.rfind('/') + 1) + g_hand_over;
    if (access(path.c_str(), X_OK) != 0) return false;
    std::vector<std::string> args = {path};
    bool has_game = false;
    for (int k = 1; k < argc; k++) has_game |= !strcmp(argv[k], "--game") || !strcmp(argv[k], "-game");
    if (!has_game) args.insert(args.end(), {"--game", game});
    for (int k = 1; k < argc; k++) args.push_back(argv[k]);
    std::vector<char *> a;
    for (std::string &s : args) a.push_back(s.data());
    a.push_back(nullptr);
    fprintf(stderr, "mods: the run goes on in %s\n", path.c_str());
    fflush(stdout);
    fflush(stderr);
    execv(path.c_str(), a.data());
    fprintf(stderr, "mods: %s couldn't be started: %s\n", path.c_str(), strerror(errno));
    return false;
#endif
}

std::vector<std::string> action_bindings() {
    std::vector<std::string> b;
    for (const Action &a : g_actions) b.push_back(a.binding);
    return b;
}

std::string action_name(size_t id) { return id < g_actions.size() ? g_actions[id].mod + ": " + g_actions[id].name : std::string(); }

std::vector<std::string> action(size_t id, bool down, bool repeat) {
    if (id >= g_actions.size() || !down) return {};
    const Action &a = g_actions[id];
    Setting &s = *a.setting;
    const int64_t was = s.value.load();
    int64_t v = was;
    switch (a.what) {
    case Action::Do::up: v = std::min(s.max, was + s.step); break;
    case Action::Do::down: v = std::max(s.min, was - s.step); break;
    case Action::Do::toggle:
        if (repeat) return {};
        v = was == s.min ? s.max : s.min;
        break;
    }
    if (v != was) {
        s.value = v;
        save_settings(s.folder);
        fprintf(stderr, "mods: %s: %s = %s (%s)%s\n", s.mod.c_str(), s.name.c_str(), shown(s).c_str(), a.name.c_str(),
                s.chooses ? (std::string(": the files and patches it chooses change ") + kNextStart).c_str() : "");
    }
    // (said even when it's at its end: the player sees where it is)
    if (s.chooses) {
        // a quiet action (notify = false) still says this once it changed: no mod can show that the
        // files and patches change only at the next start (as hk.set_setting's change says it)
        if (!a.notify && v == was) return {};
        return {s.mod, s.label + ": " + shown(s), kNextStartSaid};
    }
    if (!a.notify) return {};   // the mod shows its values itself
    return {s.mod, s.label + ": " + shown(s)};
}

std::vector<std::pair<std::string, int64_t>> settings() {
    std::vector<std::pair<std::string, int64_t>> out;
    for (const Setting &s : g_settings) out.emplace_back(s.folder + "/" + s.name, s.value.load());
    return out;
}

std::vector<std::pair<std::string, uint64_t>> counters() {
    std::vector<std::pair<std::string, uint64_t>> out;
    for (const Counter &c : g_counters) out.emplace_back(c.folder + "/" + c.name, c.n.load());
    return out;
}

bool setting_value(const std::string &folder, const std::string &name, int64_t &value) {
    for (const Setting &s : g_settings)
        if (s.folder == folder && s.name == name) {
            value = s.value.load();
            return true;
        }
    return false;
}

bool set_setting(const std::string &folder, const std::string &name, int64_t value, int64_t &now) {
    for (Setting &s : g_settings)
        if (s.folder == folder && s.name == name) {
            now = std::clamp(value, s.min, s.max);
            if (s.value.exchange(now) != now) {
                save_settings(folder);
                fprintf(stderr, "mods: %s: %s = %s (its script)%s\n", s.mod.c_str(), s.name.c_str(), shown(s).c_str(),
                        s.chooses ? (std::string(": the files and patches it chooses change ") + kNextStart).c_str() : "");
                if (s.chooses) modscript::notice({s.mod, s.label + ": " + shown(s), kNextStartSaid});
            }
            return true;
        }
    return false;
}

uint64_t counter_value(const std::string &folder, const std::string &name) {
    for (const Counter &c : g_counters)
        if (c.folder == folder && c.name == name) return c.n.load();
    return 0;
}

void report() {
    modscript::report();
    for (const Counter &c : g_counters)
        fprintf(stderr, "mods: %s: count %s = %llu\n", c.mod.c_str(), c.name.c_str(), (unsigned long long)c.n.load());
    for (const Setting &s : g_settings) fprintf(stderr, "mods: %s: %s = %s at the end\n", s.mod.c_str(), s.name.c_str(), shown(s).c_str());
}

void clear() {
    g_patches.clear();
    g_texts.clear();
    g_rates.clear();
    g_mods.clear();
    g_folder_files.clear();
    g_added_files.clear();
    g_listed.clear();
    g_actions.clear();
    g_counters.clear();
    g_settings.clear();
    g_overrides.clear();
    g_hand_over.clear();
    modscript::clear();
    // (g_hooks: the core may still hold them; kept)
}

}  // namespace hk::mods
