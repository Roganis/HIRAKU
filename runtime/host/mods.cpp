// SPDX-License-Identifier: GPL-3.0-or-later
#include "host/mods.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
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
#endif

#include "host/png.h"
#include "host/pythondisk.h"
#include "shell/core.h"
#include "shell/overlay.h"

namespace hk::mods {

namespace {

// ---- mod.toml: the TOML a mod needs (docs/MODS.md) -----------------------------------------
// key = "string" | number (decimal, 0x hex, _ separators) | true | false, # comments, and
// [[patch]] / [[bytes]] / [[file]] tables. Values keep their text; the reader below converts.
struct Table {
    std::string kind;   // "" for the top, else patch / bytes / file
    int line = 0;
    std::map<std::string, std::string> values;
    std::map<std::string, bool> is_string;
};

std::vector<Table> parse(const std::string &text, const std::string &file) {
    std::vector<Table> out(1);
    std::istringstream in(text);
    std::string line;
    int n = 0;
    auto fail = [&](const std::string &why) { throw std::runtime_error(file + ":" + std::to_string(n) + ": " + why); };
    while (std::getline(in, line)) {
        n++;
        // the comment, outside a string
        bool str = false;
        for (size_t i = 0; i < line.size(); i++) {
            if (line[i] == '"' && (i == 0 || line[i - 1] != '\\')) str = !str;
            else if (line[i] == '#' && !str) { line.erase(i); break; }
        }
        size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
        if (a == std::string::npos) continue;
        line = line.substr(a, b - a + 1);
        if (line.rfind("[[", 0) == 0) {
            if (line.size() < 4 || line.substr(line.size() - 2) != "]]") fail("a table header is [[name]]");
            Table t;
            t.kind = line.substr(2, line.size() - 4);
            t.line = n;
            out.push_back(t);
            continue;
        }
        if (line[0] == '[') fail("only [[patch]], [[text]], [[bytes]], [[file]] and [[rate]] tables");
        const size_t eq = line.find('=');
        if (eq == std::string::npos) fail("expected key = value");
        std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        key.erase(key.find_last_not_of(" \t") + 1);
        val.erase(0, val.find_first_not_of(" \t"));
        Table &t = out.back();
        if (t.values.count(key)) fail("'" + key + "' given twice");
        if (!val.empty() && val[0] == '"') {
            std::string s;
            size_t i = 1;
            for (; i < val.size() && val[i] != '"'; i++) {
                if (val[i] == '\\' && i + 1 < val.size()) {
                    const char e = val[++i];
                    s += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                } else
                    s += val[i];
            }
            if (i >= val.size()) fail("unterminated string");
            t.values[key] = s;
            t.is_string[key] = true;
        } else {
            if (val.empty()) fail("'" + key + "' has no value");
            t.values[key] = val;
            t.is_string[key] = false;
        }
    }
    return out;
}

struct Patch {
    std::string mod;
    uint32_t address = 0;
    std::vector<uint8_t> original, replace;
    bool once = false, done = false, logged = false;
    bool while_rate = false;   // in place only while a [[rate]] is on; the original bytes otherwise
    // ... and then only on the frames between the cabinet's ticks: an increment made 0 there keeps
    // the cabinet's pace at any rate (pop'n music 14's objects' frame counters)
    bool between_ticks = false;
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
    uint32_t address = 0, last = 0;
    int changed = 0, still = 0;   // frames in a row the word changed, didn't
    bool always = false;          // no while_changing: from the first frame on
    bool gated = false;           // while_equal: and only while this word holds `value`
    uint32_t gate = 0, value = 0;
    bool on = false, refused = false;
};

constexpr double kDisplayFallbackHz = 119.88;   // hz = "display" with no window, or not known
double g_display_hz = 0;
// Whether the coming frame ends a cabinet frame's worth of time (1/59.94 s): between_ticks
// patches are out then. Kept by frame() while a rate is on; always so otherwise
double g_owed = 0;
bool g_tick = true;

struct Mod {
    std::string dir, name, version;
};

std::vector<Patch> g_patches;
std::vector<Text> g_texts;
std::vector<Rate> g_rates;
std::vector<Mod> g_mods;
std::map<std::string, std::string> g_folder_files;   // a folder medium's file -> the mod's (folder_files())

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

// `rel` in `folder` (a folder medium: the PC games' data), its name's case as the folder has it
// (the mod's may differ, as Windows doesn't mind); "" if it isn't there
std::string in_folder(const std::string &folder, const std::string &rel) {
    namespace fs = std::filesystem;
    fs::path at = fs_path(folder);
    std::string part, rest = rel;
    while (!rest.empty()) {
        const size_t slash = rest.find('/');
        part = rest.substr(0, slash);
        rest = slash == std::string::npos ? "" : rest.substr(slash + 1);
        if (part.empty() || part == "." || part == "..") return {};
        std::error_code ec;
        if (fs::exists(at / fs_path(part), ec)) {
            at /= fs_path(part);
            continue;
        }
        bool found = false;
        for (auto it = fs::directory_iterator(at, ec); !ec && it != fs::directory_iterator(); ++it) {
            const std::string name = utf8(it->path().filename());
            bool same = name.size() == part.size();
            for (size_t i = 0; same && i < name.size(); i++) same = std::tolower((unsigned char)name[i]) == std::tolower((unsigned char)part[i]);
            if (same) {
                at = it->path();
                found = true;
                break;
            }
        }
        if (!found) return {};
    }
    std::error_code ec;
    return fs::is_regular_file(at, ec) ? utf8(at) : std::string();
}

// A mod folder's files laid over the game's media by their paths, as ifs_layeredfs lays a mod's
// folder over a PC game's data: a file at image/kc/kc_001.gzz in the folder is that file on the
// game's disk (or a disc's DATA/TITLE.BIN on an ISO 9660 disc), and a PNG at image/kc/kc_001.png
// is that picture, converted (the size and kind of the game's own, 8-bit ones to its folder's
// palette). What isn't on any medium of the game's is left out (said once).
void lay_files(const std::string &dir, const std::string &name, const std::set<std::string> &named,
               const std::map<std::string, std::string> &medium_paths,
               std::map<std::string, std::unique_ptr<pythondisk::Files>> &python_disks) {
    namespace fs = std::filesystem;
    std::vector<std::string> files;
    const fs::path root = fs_path(dir);
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        const std::string leaf = utf8(it->path().filename());
        if (!leaf.empty() && leaf[0] == '.') {   // hidden: skipped, folders and all
            if (it->is_directory()) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file()) continue;
        std::string rel = utf8(fs::relative(it->path(), root), true);
        if (rel == "mod.toml" || named.count(rel)) continue;
        files.push_back(rel);
    }
    if (files.empty()) return;
    std::sort(files.begin(), files.end());
    // the game's media this can lay files over: Konami Python disks and ISO 9660 discs, and the PC
    // games' folders (their files taken by the game's process from the mod's folder instead)
    std::set<std::string> images, folders;
    for (const auto &[key, path] : medium_paths) {
        std::error_code ec;
        if (std::filesystem::is_directory(fs_path(path), ec)) folders.insert(path);
        else images.insert(path);
    }
    for (const std::string &image : images)
        if (!python_disks.count(image))
            if (auto disk = pythondisk::Files::open(image)) python_disks[image] = std::move(disk);
    int left_out = 0;
    std::string first_left_out;
    for (const std::string &rel : files) {
        const std::string label = name + ": " + rel;
        bool in_a_folder = false;
        for (const std::string &folder : folders)
            if (const std::string game_file = in_folder(folder, rel); !game_file.empty()) {
                g_folder_files[game_file] = dir + "/" + rel;
                in_a_folder = true;
                break;
            }
        if (in_a_folder) continue;
        if (images.empty()) {
            if (!left_out++) first_left_out = rel;
            continue;
        }
        const std::string data = read_file(dir + "/" + rel);
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
                if (!png::decode(bytes, w, h, rgba, err)) throw std::runtime_error(dir + "/" + rel + ": " + err);
                const std::vector<uint8_t> like = disk->get(target);
                const size_t slash = target.find_last_of('/');
                const std::vector<uint8_t> idx = slash == std::string::npos ? std::vector<uint8_t>() : disk->get(target.substr(0, slash) + "/system.idx");
                try {
                    disk->put(target, pythondisk::picture(like, rgba.data(), w, h, idx), name + ": " + target + " (from " + rel + ")");
                } catch (const std::exception &e) {
                    throw std::runtime_error(dir + "/" + rel + ": " + e.what());
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
        fprintf(stderr, "mods: %s: %d file%s on none of the game's media, left out (%s%s)\n", name.c_str(), left_out,
                left_out == 1 ? "" : "s", first_left_out.c_str(), left_out > 1 ? ", ..." : "");
}

}  // namespace

bool load(const std::vector<std::string> &dirs, const std::string &game, const std::string &root, std::string &err) {
    try {
        const auto medium_paths = media(root, game);
        std::map<std::string, std::unique_ptr<pythondisk::Files>> python_disks;   // [[file]]s on Konami Python disks
        // The first mod in the list wins where two change the same thing, as with ifs_layeredfs: a
        // later range, file or patch wins, so the list is laid on from its last mod to its first
        for (auto d = dirs.rbegin(); d != dirs.rend(); ++d) {
            const std::string &dir = *d;
            if (!std::filesystem::is_directory(fs_path(dir))) throw std::runtime_error(dir + ": no such mod folder");
            const std::string file = dir + "/mod.toml";
            const std::string text = read_file(file);
            // no mod.toml: a folder of files laid over the game's (below), named after the folder
            const std::vector<Table> tables = text.empty() ? std::vector<Table>(1) : parse(text, file);
            Mod mod{dir, utf8(fs_path(dir).filename()), ""};
            std::set<std::string> named;   // the files its tables name (not laid over the game's)
            for (const Table &t : tables) {
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
                if (t.kind.empty()) {
                    if (text.empty()) continue;
                    known({"name", "version", "game", "author", "description"});
                    mod.name = str("name", true);
                    mod.version = str("version", false);
                    if (const std::string g = str("game", true); g != game) fail("this mod is for " + g + ", not " + game);
                } else if (t.kind == "patch") {
                    known({"address", "original", "replace", "once", "while_rate", "between_ticks"});
                    Patch p;
                    p.mod = mod.name;
                    p.address = (uint32_t)num("address");
                    if (t.values.count("original")) p.original = hex("original");   // none: written every frame (a cheat)
                    p.replace = hex("replace");
                    p.once = flag("once");
                    p.between_ticks = flag("between_ticks");
                    p.while_rate = flag("while_rate") || p.between_ticks;
                    if (p.while_rate && (p.once || !t.values.count("original")))
                        fail("a while_rate patch has its 'original' (put back when the rate is off) and isn't 'once'");
                    if (p.replace.empty()) fail("'replace' is empty");
                    if (!p.original.empty() && p.original.size() != p.replace.size()) fail("'original' and 'replace' are as long as each other");
                    g_patches.push_back(std::move(p));
                } else if (t.kind == "text") {
                    known({"find", "replace", "from", "to", "every"});
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
                    g_texts.push_back(std::move(x));
                } else if (t.kind == "rate") {
                    known({"hz", "while_changing", "while_equal", "value"});
                    Rate r;
                    r.mod = mod.name;
                    if (t.values.count("hz") && t.is_string.at("hz")) {
                        if (str("hz", true) != "display") fail("'hz' is a number, or \"display\" (the display's refresh rate)");
                    } else {
                        r.hz = real("hz");
                        if (r.hz < 30 || r.hz > 1000) fail("'hz' is between 30 and 1000");
                    }
                    if (t.values.count("while_changing")) r.address = (uint32_t)num("while_changing");
                    else r.always = true;
                    if (t.values.count("while_equal") || t.values.count("value")) {
                        r.gated = true;
                        r.gate = (uint32_t)num("while_equal");
                        r.value = (uint32_t)num("value");
                    }
                    g_rates.push_back(r);
                } else if (t.kind == "bytes") {
                    known({"medium", "offset", "from"});
                    const std::string medium = str("medium", true);
                    auto it = medium_paths.find(medium);
                    if (it == medium_paths.end()) fail("no medium '" + medium + "' in dumps/" + game + "/machine.txt");
                    std::error_code ec;
                    if (std::filesystem::is_directory(fs_path(it->second), ec))
                        fail("'" + medium + "' is a folder: its files are changed whole ([[file]], or the files at their paths)");
                    char what[64];
                    snprintf(what, sizeof what, ": %s bytes at 0x%llx", medium.c_str(), (unsigned long long)num("offset"));
                    overlay::add(it->second, num("offset"), from(), mod.name + what);
                } else if (t.kind == "file") {
                    known({"medium", "path", "from"});
                    const std::string medium = str("medium", true);
                    auto it = medium_paths.find(medium);
                    if (it == medium_paths.end()) fail("no medium '" + medium + "' in dumps/" + game + "/machine.txt");
                    std::error_code ec;
                    if (std::filesystem::is_directory(fs_path(it->second), ec)) {   // a PC game's folder: the file, whole
                        const std::string game_file = in_folder(it->second, str("path", true));
                        if (game_file.empty()) fail("no " + str("path", true) + " in " + it->second);
                        const std::string f = str("from", true);
                        if (f.find("..") != std::string::npos) fail("'from' is a file inside the mod's folder");
                        if (!std::filesystem::is_regular_file(fs_path(dir + "/" + f), ec)) fail("'from' file " + f + " not found");
                        named.insert(f);
                        g_folder_files[game_file] = dir + "/" + f;
                        continue;
                    }
                    try {
                        const std::string label = mod.name + ": " + str("path", true);
                        auto data = from();
                        if (!python_disks.count(it->second) && !iso_file(it->second, str("path", true), data, label)) {
                            auto disk = pythondisk::Files::open(it->second);
                            if (!disk) throw std::runtime_error(it->second + ": neither an ISO 9660 disc nor a disk whose file system HIRAKU knows");
                            python_disks[it->second] = std::move(disk);
                        }
                        if (python_disks.count(it->second)) python_disks[it->second]->put(str("path", true), data, label);
                    } catch (const std::exception &e) {
                        fail(e.what());
                    }
                } else {
                    fail("unknown table [[" + t.kind + "]]");
                }
            }
            lay_files(dir, mod.name, named, medium_paths, python_disks);
            g_mods.insert(g_mods.begin(), mod);   // in the list's order
        }
        for (auto &d : python_disks) d.second->finish();
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }
    return true;
}

void display_rate(double hz) { g_display_hz = hz; }

void frame() {
    for (Rate &r : g_rates) {
        if (r.refused) continue;
        if (r.always) {
            r.changed = 2, r.still = 0;
        } else {
            const uint8_t *m = core().mem(r.address, 4);
            if (!m) continue;
            uint32_t v;
            memcpy(&v, m, 4);
            if (v != r.last) r.changed++, r.still = 0;
            else r.still++, r.changed = 0;
            r.last = v;
        }
        // two frames in a row either way, so a word that changes once (a load) doesn't count; and
        // with while_equal, only while that word holds the value (off at once when it doesn't)
        bool in_state = true;
        if (r.gated) {
            const uint8_t *g = core().mem(r.gate, 4);
            uint32_t gv = 0;
            if (g) memcpy(&gv, g, 4);
            in_state = g && gv == r.value;
        }
        const bool want = in_state && (r.on ? r.still < 2 : r.changed >= 2);
        // hz = "display": follow the display (a window moved to another one)
        const double hz = r.hz > 0 ? r.hz : g_display_hz > 30 ? g_display_hz : kDisplayFallbackHz;
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
        if (p.done) continue;
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
        if (!p.while_rate) out.push_back({p.mod, p.address, p.original, p.replace, p.once});
    return out;
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

std::vector<std::string> folders() {
    std::vector<std::string> f;
    for (const Mod &m : g_mods) f.push_back(m.dir);
    return f;
}

std::vector<std::string> describe() {
    std::vector<std::string> d;
    for (const Mod &m : g_mods) d.push_back(m.name + (m.version.empty() ? "" : " " + m.version) + " (" + m.dir + ")");
    return d;
}

}  // namespace hk::mods
