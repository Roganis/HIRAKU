// SPDX-License-Identifier: GPL-3.0-or-later
// Konami Python 1 (a PS2 with an i.LINK interface board): PCSX2 Reliquary's emulator
// hosted as a HIRAKU machine core (PLAN.md §23, PY-1). Reliquary is the reference for
// these boards, so this core is the reference by construction; later phases replace
// parts of it (the EE by static recompilation) and check them against it.
//
// The machine comes from dumps/<game>/machine.txt (tools/ingest.py): the interface
// board's files (boot ROM, config ROM, BBSRAM, dongles), the KN00002 memory-card
// dongle, the disk image (CHD), the COH-H31000 BIOS and the MagicGate key files.
//
// Each run works on its own copy of the files Reliquary writes back (BBSRAM, dongles,
// boot ROM, memory card; the disk's writes go to an overlay): in the NVRAM directory
// when there is one, else a fresh copy under the output directory, so scripted runs
// start from the dumps' state and repeat exactly.
//
// Threads: run() is PCSX2's CPU thread (EE, IOP, SPU2, the interface board). The GS
// runs on PCSX2's own thread. At each vsync, once it is posted to the GS, the CPU thread
// waits for that vsync's picture and hands it to the shell: pictures, digests and
// scripts stay deterministic. --picture-overlap hands over the previous vsync's picture
// instead (taken before the post), so the GS overlaps the EE, at a frame more latency.
//
// Linking Reliquary (GPL-3.0) makes the hiraku-<python game> binaries GPL-3.0 as a
// whole; this file is GPL-3.0-or-later like the rest of HIRAKU. The Host:: glue below
// follows pcsx2-gsrunner (GPL-3.0+).
#ifndef _WIN32
#include <cxxabi.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdlib.h>   // on_exit (glibc)
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "shell/kvfile.h"
#include "shell/sys.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/SmallString.h"

#include "pcsx2/PrecompiledHeader.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/Config.h"
#include "pcsx2/Counters.h"
#include "pcsx2/FireWire/FireWire.h"
#include "pcsx2/GS.h"
#include "pcsx2/GameList.h"
#include "pcsx2/HirakuHooks.h"
#include "pcsx2/Host.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/Memory.h"
#include "pcsx2/Memory.h"
#include "pcsx2/R5900.h"
#include "shell/overlay.h"
#include "libchdr/chd.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/USB/USB.h"
#include "pcsx2/VMManager.h"

#include "host/macro.h"
#include "host/notice.h"
#include "python/cardunit.h"
#include "python/icca.h"
#include "python/eanet.h"
#include "python/ethnet.h"
#include "python/gfdmcard.h"
#include "eamuse/remote.h"
#include "python/easerver.h"
#include "python/lancable.h"
#include "python/lobby.h"
#include "python/netrelay.h"
#include "shell/cabinet.h"
#include "shell/core.h"
#include "shell/eaoptions.h"
#include "shell/userdir.h"

#ifndef _WIN32
// clang's profile runtime, present only in an instrumented build (oracle/pcsx2/build-reliquary.sh pgo)
extern "C" int __llvm_profile_write_file(void) __attribute__((weak));
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX   // (windows.h's min and max macros: std::max below)
#endif
#include <objbase.h>
#endif

namespace {

using hk::read_kv;

// Mods find files on the board's disk through this (shell/overlay.h read(), host/pythondisk.h):
// the disk as it is, a CHD read with PCSX2's libchdr (this core has no HIRAKU image layer), any
// other image as a plain file. The last disk's handle and hunk are kept between reads.
bool disk_reader(const std::string &path, uint64_t off, void *buf, size_t n) {
    static std::string open_path;
    static chd_file *chd = nullptr;
    static std::vector<uint8_t> hunk;
    static uint32_t hunk_no = UINT32_MAX;
    if (path.size() < 4 || (path.substr(path.size() - 4) != ".chd" && path.substr(path.size() - 4) != ".CHD")) {
        FILE *f = fopen(path.c_str(), "rb");
        if (!f) return false;
#ifdef _WIN32
        const bool ok = _fseeki64(f, (long long)off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
#else
        const bool ok = fseeko(f, (off_t)off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
#endif
        fclose(f);
        return ok;
    }
    if (path != open_path) {
        if (chd) chd_close(chd);
        chd = nullptr;
        hunk_no = UINT32_MAX;
        if (chd_open(path.c_str(), CHD_OPEN_READ, nullptr, &chd) != CHDERR_NONE) {
            chd = nullptr;
            open_path.clear();
            return false;
        }
        open_path = path;
        hunk.resize(chd_get_header(chd)->hunkbytes);
    }
    const chd_header *h = chd_get_header(chd);
    if (off + n > h->logicalbytes) return false;
    auto *out = static_cast<uint8_t *>(buf);
    while (n) {
        const uint32_t no = (uint32_t)(off / h->hunkbytes), at = (uint32_t)(off % h->hunkbytes);
        if (no != hunk_no) {
            if (chd_read(chd, no, hunk.data()) != CHDERR_NONE) return false;
            hunk_no = no;
        }
        const size_t take = std::min<size_t>(n, h->hunkbytes - at);
        memcpy(out, hunk.data() + at, take);
        out += take, off += take, n -= take;
    }
    return true;
}
const bool g_disk_reader_set = (hk::overlay::set_reader(disk_reader), true);

#ifdef _WIN32
// PCSX2's CPU thread (here the main thread) needs COM multithreaded (VMManager's
// CPUThreadInitialize), and the game window's SDL would set the thread single-threaded first:
// multithreaded from the start, before main (SDL takes COM as it finds it)
[[maybe_unused]] const bool g_com_mta = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
#endif

using hk::sys::copy_file;
using hk::sys::exists;
inline void mkdirs(const std::string &path) { hk::sys::make_dirs(path); }

#ifdef _WIN32
std::string base_name(const std::string &p) { return p.substr(p.find_last_of("/\\") + 1); }   // either separator
#else
std::string base_name(const std::string &p) { return p.substr(p.rfind('/') + 1); }
#endif

// One game's e-AMUSEMENT folder moved into the folder all the pop'n games share (pop'n 9-14 in the
// saves' default places: a player carried from one game to the next is found there). A file the
// shared folder lacks moves over; the card counters keep the larger; the card and KONAMI ID lists
// gain the lines they lack. Anything else already there stays, and the old folder is kept, renamed.
void merge_eamuse(const std::string &from, const std::string &to) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(from, ec)) return;
    mkdirs(to);
    auto read = [](const fs::path &p) {
        std::ifstream f(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    };
    auto write = [](const fs::path &p, const std::string &d) { std::ofstream(p, std::ios::binary | std::ios::trunc) << d; };
    bool left = false;
    for (const fs::directory_entry &e : fs::directory_iterator(from, ec)) {
        const std::string name = e.path().filename().string();
        const fs::path dst = fs::path(to) / name;
        if (!fs::exists(dst, ec)) {
            fs::rename(e.path(), dst, ec);
            if (ec) left = true;
        } else if (name == "numbering" || name == "refid") {
            const unsigned long long a = std::strtoull(read(e.path()).c_str(), nullptr, 10), b = std::strtoull(read(dst).c_str(), nullptr, 10);
            write(dst, std::to_string(std::max(a, b)) + "\n");
        } else if (name == "cards" || name == "users") {
            std::string have = read(dst), add;
            std::istringstream lines(read(e.path()));
            for (std::string l; std::getline(lines, l);)
                if (!l.empty() && have.find(l) == std::string::npos) add += l + "\n";
            if (!have.empty() && have.back() != '\n') have += "\n";
            write(dst, have + add);
        } else {
            left = true;
            fprintf(stderr, "e-AMUSEMENT: %s is in both %s and %s; the shared one is kept\n", name.c_str(), from.c_str(), to.c_str());
        }
    }
    fs::rename(from, from + ".moved", ec);
    printf("e-AMUSEMENT: this game's data moved to %s, which the pop'n games share%s\n", to.c_str(),
           left ? " (what couldn't move stays in eamuse.moved)" : "");
}

// pop'n buttons 1-9, left to right (MAME's names), on the interface board's JAMMA
// inputs as Reliquary's POPN profile reads them (KonamiPython1.cpp,
// BuildP1IOPopnInputSourceBits): SW1-SW9 are P1 Up/Down/Left/Right/Button1 and
// P2 Up/Down/Left/Right.
const std::map<std::string, std::string> POPN_INPUTS = {
    {"P1 Button 1", "P1Up"},   {"P1 Button 2", "P1Down"}, {"P1 Button 3", "P1Left"},
    {"P1 Button 4", "P1Right"}, {"P1 Button 5", "P1Button1"}, {"P1 Button 6", "P2Up"},
    {"P1 Button 7", "P2Down"}, {"P1 Button 8", "P2Left"}, {"P1 Button 9", "P2Right"},
    {"Coin 1", "Coin1"},       {"Coin 2", "Coin2"},       {"Test", "Test"},
    {"Service", "Service"},    {"1 Player Start", "P1Start"},
};

// Konami Python 2 (PY-6): the cabinet families' inputs (MAME's names, shell/cabinet.cpp) on the
// P2IO board, as Reliquary's Python 2 USB device names them (usb-python2.cpp, Bindings)
const std::map<std::string, std::string> PY2_INPUTS = {
    {"Hi-Hat", "DMHiHat"},        {"Snare", "DMSnare"},       {"High Tom", "DMHighTom"}, {"Low Tom", "DMLowTom"},
    {"Cymbal", "DMCymbal"},       {"Bass Drum", "DMBassDrum"}, {"Select L", "DMSelectLeft"},
    {"Select R", "DMSelectRight"}, {"1 Player Start", "DMStart"}, {"Coin 1", "Coin1"},
    {"Coin 2", "Coin2"},          {"Test", "Test"},           {"Service", "Service"},
};

// ... and the lamps, on the P2IO's four output banks (LAMP_OUT; DrumMania V's LAMP CHECK, item by
// item): the pads and the spot lit by a 0, the rest by a 1. The cabinet's SPEAKER neons are the
// drm family's Neon Top.
struct Py2Lamp { const char *name; int bank, bit; bool active_low; };
const Py2Lamp PY2_DRMN_LAMPS[] = {
    {"Hi-Hat", 3, 0, true},     {"Snare", 3, 1, true},          {"High Tom", 3, 2, true}, {"Low Tom", 3, 3, true},
    {"Cymbal", 3, 4, true},     {"Start Lamp", 3, 6, false},    {"Select Lamp", 3, 7, false},
    {"Spot Left/Right", 2, 0, true}, {"Neon Woofer", 2, 1, false}, {"Neon Top", 2, 2, false},
};

// The e-AMUSEMENT 10-key (cardunit.h): its keys' PS/2 set 2 scan codes, as the game maps them
// (0x1a54c0). Enter and / are sent without their 0xe0 prefix, which the game skips.
const std::map<std::string, uint8_t> KEYPAD_SCAN = {
    {"Keypad 0", 0x70}, {"Keypad 1", 0x69}, {"Keypad 2", 0x72}, {"Keypad 3", 0x7a}, {"Keypad 4", 0x6b},
    {"Keypad 5", 0x73}, {"Keypad 6", 0x74}, {"Keypad 7", 0x6c}, {"Keypad 8", 0x75}, {"Keypad 9", 0x7d},
    {"Keypad .", 0x71}, {"Keypad Enter", 0x5a}, {"Keypad Clear", 0x66}, {"Keypad /", 0x4a},
    {"Keypad *", 0x7c}, {"Keypad -", 0x7b}, {"Keypad +", 0x79},
};

MemorySettingsInterface s_settings;

// HK_PY_PROFILE=1: a 1 kHz SIGPROF sampler of the host instruction pointer, by thread,
// printed at exit with each sample attributed to one of PCSX2's code caches (EE, IOP,
// VU0, VU1, VIF, the software GS's JIT) or to its function. HIRAKU's own sampler
// (runtime/core/sampler.cpp) can't see into the caches, and dumps from atexit, which
// this core's exit path skips. Linux only.
#ifdef _WIN32
namespace prof {
void start() { fprintf(stderr, "HK_PY_PROFILE: the sampler is Linux only\n"); }
void cpu_thread() {}
}  // namespace prof
#else
namespace prof {
constexpr int MAX = 1 << 20;
uintptr_t g_rip[MAX];
pid_t g_tid[MAX];
std::atomic<int> g_n{0};
pid_t g_cpu_tid = 0;

void on_sig(int, siginfo_t *, void *uc) {
    int n = g_n.fetch_add(1, std::memory_order_relaxed);
    if (n >= MAX) return;
    g_rip[n] = (uintptr_t) static_cast<ucontext_t *>(uc)->uc_mcontext.gregs[REG_RIP];
    g_tid[n] = (pid_t)syscall(SYS_gettid);
}
void cpu_thread() { g_cpu_tid = (pid_t)syscall(SYS_gettid); }   // the calling thread is the CPU thread
void start() {
    struct sigaction sa {};
    sa.sa_sigaction = on_sig;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, nullptr);
    itimerval t{{0, 1000}, {0, 1000}};
    setitimer(ITIMER_PROF, &t, nullptr);
}
void report() {
    itimerval off{};
    setitimer(ITIMER_PROF, &off, nullptr);
    const int n = std::min(g_n.load(), MAX);
    struct Range { const char *name; uintptr_t lo, hi; };
    const Range ranges[] = {
        {"[EE JIT code]", (uintptr_t)SysMemory::GetEERec(), (uintptr_t)SysMemory::GetEERecEnd()},
        {"[IOP JIT code]", (uintptr_t)SysMemory::GetIOPRec(), (uintptr_t)SysMemory::GetIOPRecEnd()},
        {"[VU0 JIT code]", (uintptr_t)SysMemory::GetVU0Rec(), (uintptr_t)SysMemory::GetVU0RecEnd()},
        {"[VU1 JIT code]", (uintptr_t)SysMemory::GetVU1Rec(), (uintptr_t)SysMemory::GetVU1RecEnd()},
    };
    std::map<std::string, int> cpu, other;
    std::map<char, std::map<uintptr_t, int>> exe;   // thread class -> executable offset -> samples
    for (int k = 0; k < n; k++) {
        const uintptr_t a = g_rip[k];
        std::string name;
        for (const Range &r : ranges)
            if (a >= r.lo && a < r.hi) name = r.name;
        if (name.empty()) {
            Dl_info i{};
            Dl_info self{};
            dladdr((void *)&report, &self);
            if (dladdr((void *)a, &i) && i.dli_fbase == self.dli_fbase) {
                exe[(g_tid[k] == g_cpu_tid ? 'c' : 'o')][a - (uintptr_t)self.dli_fbase]++;   // symbolised below
                continue;
            } else if (i.dli_sname) {
                int st = 0;
                char *d = abi::__cxa_demangle(i.dli_sname, nullptr, nullptr, &st);
                name = d ? d : i.dli_sname;
                free(d);
                if (size_t p = name.find('('); p != std::string::npos) name.resize(p);
            } else if (i.dli_fname) {
                name = std::string("[") + base_name(i.dli_fname) + "]";
            } else {
                name = "[anonymous: other JIT code]";
            }
        }
        (g_tid[k] == g_cpu_tid ? cpu : other)[name]++;
    }
    auto print = [](const char *title, const std::map<std::string, int> &h, int total_all) {
        std::vector<std::pair<int, std::string>> v;
        int total = 0;
        for (auto &[k, c] : h) v.push_back({c, k}), total += c;
        std::sort(v.rbegin(), v.rend());
        fprintf(stderr, "profile: %s: %d samples (%.0f %%)\n", title, total, 100.0 * total / std::max(1, total_all));
        for (size_t i = 0; i < v.size() && i < 25; i++)
            fprintf(stderr, "profile:   %5.1f %%  %s\n", 100.0 * v[i].first / std::max(1, total), v[i].second.c_str());
    };
    // the executable's own functions: llvm-symbolizer on the offsets, one call for all
    if (!exe.empty()) {
        char path[4096];
        ssize_t len = readlink("/proc/self/exe", path, sizeof path - 1);
        path[len > 0 ? len : 0] = 0;
        const std::string in = "/tmp/hk-py-profile-" + std::to_string(getpid()) + ".txt";
        FILE *f = fopen(in.c_str(), "w");
        std::vector<std::tuple<char, uintptr_t, int>> order;
        for (auto &[cls, h] : exe)
            for (auto &[off, c] : h) fprintf(f, "0x%lx\n", (unsigned long)off), order.emplace_back(cls, off, c);
        fclose(f);
        const std::string cmd = "llvm-symbolizer --functions=short --no-inlines --obj='" + std::string(path) + "' < " + in;
        if (FILE *p = popen(cmd.c_str(), "r")) {
            char line[4096];
            size_t k = 0;
            std::string fn;
            int state = 0;   // llvm-symbolizer prints: function, file:line, blank
            while (fgets(line, sizeof line, p) && k < order.size()) {
                std::string l(line);
                if (!l.empty() && l.back() == '\n') l.pop_back();
                if (state == 0) { fn = l; state = 1; }
                else if (state == 1) {
                    auto &[cls, off, c] = order[k++];
                    (cls == 'c' ? cpu : other)[fn] += c;
                    state = 2;
                } else state = 0;
            }
            pclose(p);
        }
        unlink(in.c_str());
    }
    print("CPU thread (EE, IOP, VU, SPU2, the interface board)", cpu, n);
    print("other threads (GS, its workers)", other, n);
}
}  // namespace prof
#endif


class PythonCore final : public hk::Core {
public:
    bool option(const std::string &name, const std::function<std::string()> &value) override {
        if (name == "--renderer") {
            const std::string v = value();
            renderer_ = v == "vk" ? GSRendererType::VK : v == "gl" ? GSRendererType::OGL : GSRendererType::SW;
#ifdef _WIN32
            if (v == "dx11") renderer_ = GSRendererType::DX11;
            if (v == "dx12") renderer_ = GSRendererType::DX12;
#endif
            return true;
        }
        if (name == "--interp") { interp_ = true; return true; }
        if (name == "--pcsx2-log") { pcsx2_log_ = true; return true; }
        if (name == "--no-picture") { no_picture_ = true; return true; }
        if (name == "--bare-board") { bare_board_ = true; return true; }
        if (name == "--picture-overlap") { picture_now_ = picture_now_user_ = want_picture_now_ = false; return true; }
        if (name == "--interlaced") { full_frames_ = false; return true; }
        if (name == "--fill-screen") { fill_screen_ = true; return true; }
        if (name == "--lan-host") { lan_where_ = value(); lan_host_ = true; return true; }
        if (name == "--lan-join") { lan_where_ = value(); lan_host_ = false; return true; }
        if (name == "--lan-cabinets") { lan_cabinets_ = std::stoi(value()); return true; }
        if (name == "--autoplay") {
            autoplay_ = true;   // (whether the game has it: init, once the game is known)
            return true;
        }
        if (name == "--eamuse") {   // the older name: local (the server in the core) or -url's URL
            const std::string v = value();
            ea_.url = v == "local" ? "" : v;
            if (ea_.online() && !hk::ea::Remote(ea_.http_url()).ok()) return false;
            return true;
        }
        std::string error;
        if (ea_.option(name, value, &error)) {   // spice2x's -ea, -url, -p, -card0, ... (shell/eaoptions.h)
            if (error.empty() && name == "--url" && !hk::ea::Remote(ea_.http_url()).ok())
                error = "-url: " + ea_.url + " is not host:port[/path] or http://host[:port][/path] (the game speaks plain HTTP)";
            if (!error.empty()) { fprintf(stderr, "%s\n", error.c_str()); exit(2); }
            return true;
        }
        if (name == "--pcsx2-set") {   // Section/Key=value, after the core's own settings
            const std::string v = value();
            const size_t slash = v.rfind('/', v.find('=')), eq = v.find('=');
            if (slash == std::string::npos || eq == std::string::npos || slash > eq) return false;
            extra_settings_.push_back({v.substr(0, slash), v.substr(slash + 1, eq - slash - 1), v.substr(eq + 1)});
            return true;
        }
        return false;
    }
    std::string options_help() const override {
        return "  --renderer sw|vk|gl  the GS renderer: software (default: what the oracle runs), Vulkan or OpenGL\n"
               "                       (OpenGL needs a window of its own: not with HIRAKU's); Windows: also dx11, dx12\n"
               "  --interp             every CPU interpreted (EE, IOP, VU0, VU1), as the oracle's --interp\n"
               "  --pcsx2-log          the emulator's own log on stderr\n"
               "  --no-picture         don't read the GS's pictures back (black frames; for measuring)\n"
               "  --bare-board         no e-AMUSEMENT card unit or network on the board, as in plain Reliquary\n"
               "                       (for checking against the oracle: pop'n 13 probes the card unit at frame 1,325)\n"
               "  --picture-overlap    hand over the previous vsync's picture, so the GS overlaps the EE: less\n"
               "                       time per frame, a frame (16.7 ms) more latency. The default waits for each\n"
               "                       vsync's own picture\n"
               "  --interlaced         let the game draw only the lines of the TV field it shows (its SCANMSK), as\n"
               "                       on the cabinet: the other lines stay a frame old, which combs when shown\n"
               "                       progressive. The default draws every line of every frame (as at 480p)\n"
               "  --fill-screen        stretch the game's picture over the whole 4:3 screen (as PCSX2 does). The\n"
               "                       default places it in the TV's raster, black around it as on a TV: the\n"
               "                       picture at its true shape, 448 lines on 480\n"
               "  --autoplay           the game plays every song itself, as in its attract demo: every note GREAT\n"
               "                       (a mod for testing what clears unlock; pop'n music 9)\n"
               "  --lan-host SOCKET    NET TAISEN between HIRAKU cabinets (pop'n 12-14): this is the first cabinet\n"
               "                       (192.168.50.2, it keeps the lobby); wait at SOCKET (a path, or host:port for\n"
               "                       TCP) for the others. They run in lockstep, a frame at a time\n"
               "  --lan-join SOCKET    another cabinet on that LAN (192.168.50.3 on)\n"
               "  --lan-cabinets N     with --lan-host: how many cabinets in all, 2 to 4 (default 2)\n"
               "  --pcsx2-set S/K=V    set one of the emulator's settings (its ini's section S, key K), for experiments\n"
               "  --eamuse local|URL   the older -ea / -url (below)\n" +
               std::string(hk::EaOptions::help()) +
               "  (e-AMUSEMENT when the game has it on in test mode; the server inside HIRAKU is offline and\n"
               "  reproducible. pop'n has one card reader: player 1's. docs/EAMUSE3.md)\n";
    }

    bool init(const Options &o) override {
        if (autoplay_ && !has_autoplay(o.game.c_str()))
            fprintf(stderr, "autoplay: not available for %s yet (pop'n music 9 only)\n", o.game.c_str());
        opt_ = o;
        auto m = read_kv(o.root + "/dumps/" + o.game + "/machine.txt");
        if (m.empty() || m["core"] != "python") {
            fprintf(stderr, "no Python dumps/%s/machine.txt - run tools/ingest.py <the set's .py1> first\n", o.game.c_str());
            return false;
        }
        family_ = m.count("family") ? m["family"] : "pypopn";
        lamps_.assign(hk::cab::lamps(family_).size(), 0);

        // The run's own copy of what Reliquary writes back
        run_dir_ = o.nvram_dir.empty() ? o.out + "/python-run" : o.nvram_dir;
#ifdef _WIN32
        if (!std::filesystem::path(run_dir_).is_absolute()) {   // the descriptor's paths must not depend on the working directory
            std::error_code ec;
            const std::filesystem::path abs = std::filesystem::absolute(run_dir_, ec);
            if (!ec) run_dir_ = abs.generic_string();
        }
#else
        if (run_dir_[0] != '/') {   // the descriptor's paths must not depend on the working directory
            char cwd[4096];
            if (getcwd(cwd, sizeof cwd)) run_dir_ = std::string(cwd) + "/" + run_dir_;
        }
#endif
        if (o.nvram_dir.empty() && exists(run_dir_)) FileSystem::RecursiveDeleteDirectory(run_dir_.c_str());
        mkdirs(run_dir_);
        if (m["board"] == "python2") {
            python2_ = true;
            if (!lan_where_.empty()) { fprintf(stderr, "--lan-host/--lan-join: pop'n music only\n"); return false; }
            if (!write_py2(o, m)) return false;
            const std::string bios = o.data_path(m["bios"]);
            if (!exists(bios)) { fprintf(stderr, "python: missing BIOS %s\n", bios.c_str()); return false; }
            return init_emulator(bios, o.data_path(m["keys"]));
        }
        io_mode_ = m["io_mode"];
        std::string py1 = "[Game]\nName=" + m["name"] + "\nIoMode=" + io_mode_ + "\n";
        if (!m["game_id"].empty()) py1 += "GameId=" + m["game_id"] + "\n";
        struct F { const char *key, *ini; bool writable; };
        const F files[] = {
            {"hdd", "HddImagePath", false},           {"cf", "CfImagePath", false},
            {"bbsram", "BbsRamPath", true},           {"io_bootrom", "IoBootRomPath", true},
            {"io_config_rom", "IoConfigRomPath", false}, {"internal_dongle", "InternalDonglePath", true},
            {"external_dongle", "ExternalDonglePath", true}, {"memcard", "MemoryCardDonglePath", true},
            {"memcard_id", "MemoryCardIdPath", false},
        };
        // mods' replaced bytes on the disk or CF card (shell/overlay.h, docs/MODS.md)
        if (!m["hdd"].empty()) hdd_overlay_ = hk::overlay::find(o.data_path(m["hdd"]));
        if (!m["cf"].empty()) cf_overlay_ = hk::overlay::find(o.data_path(m["cf"]));
        for (const F &f : files) {
            if (m[f.key].empty()) continue;
            std::string src = o.data_path(m[f.key]);
            if (!exists(src)) { fprintf(stderr, "python: missing %s (%s)\n", src.c_str(), f.key); return false; }
            std::string path = src;
            if (f.writable) {
                path = run_dir_ + "/" + base_name(src);
                if (!exists(path) && !copy_file(src, path)) { fprintf(stderr, "python: cannot copy %s\n", src.c_str()); return false; }
            }
            py1 += std::string(f.ini) + "=" + path + "\n";
        }
        py1_path_ = run_dir_ + "/" + o.game + ".py1";
        if (family_ == "pypopn" && !bare_board_) {
            // the saves' folder when this game's save is in its default place there (the game window's)
            if (const std::string saves = hk::user_data_dir(o.root); run_dir_ == saves + "/" + o.game) shared_saves_ = saves;
            std::string error;
            ea_.read_card_files(run_dir_, &error);
            if (!error.empty()) { fprintf(stderr, "%s\n", error.c_str()); return false; }
            if (!ea_.card[1].empty() || ea_.autocard[1]) printf("e-AMUSEMENT: pop'n has one card reader, player 1's (player 2's card not used)\n");
            // A card with a number (-card0, card0.txt) is its own file, cards/NUMBER.bin; without one,
            // card.bin. A fresh card is one of this game (cardunit.h), and gets its number when the
            // game asks the server for one (on_call).
            if (o.game == "popn14") {
                // pop'n 14's e-AMUSEMENT PASS: an IC card, only a number. Without -card0 or
                // card0.txt, a new number, kept in the save folder's card0.txt; a run without a
                // save folder (--nvram) gets a fixed one, so that it is reproducible
                if (ea_.card[0].empty() && o.nvram_dir.empty()) {
                    ea_.card[0] = "E004010000000001";
                } else if (ea_.card[0].empty()) {
                    std::random_device rd;
                    char id[17];
                    snprintf(id, sizeof id, "E004%04X%08X", rd() & 0xffff, rd());
                    ea_.card[0] = id;
                    FileSystem::WriteStringToFile((run_dir_ + "/card0.txt").c_str(), ea_.card[0] + "\n");
                }
            }
            game_ = o.game;
            make_card();
            hk::py::VirtualNet::Io io;
            io.write = [](uint32_t addr, const uint8_t *d, uint32_t n) { Hiraku::net_write(addr, d, n); };
            io.respond = [](uint32_t ch, const uint32_t w[8]) { Hiraku::net_respond(ch, w); };
            io.read = [](uint32_t addr, uint8_t *d, uint32_t n) { return Hiraku::iop_read(addr, d, n); };
            net_ = std::make_unique<hk::py::VirtualNet>(io, [this](const hk::py::VirtualNet::Target &t, std::string req,
                                                                   std::function<void(std::string)> done) {
                if (std::string answer = on_call(req, done); !answer.empty()) { done(std::move(answer)); return; }
                if (!remote_) { done(server_->handle(req)); return; }
                // online: off the emulator's thread; the recv waits for the answer
                std::thread([this, t, req, done = std::move(done)] {
                    std::string error;
                    std::string answer = remote_->exchange(t, req, &error);
                    if (net_log_) {
                        std::lock_guard<std::mutex> l(net_log_mutex_);
                        fprintf(net_log_, "online %s:%u %s\n  -> %s\n", t.services ? remote_->host().c_str() : t.host.c_str(),
                                t.services ? remote_->port() : t.port, hk::py::EamuseServer::clear(req).c_str(),
                                answer.empty() ? ("failed: " + error).c_str() : hk::py::EamuseServer::clear(answer).c_str());
                        fflush(net_log_);
                    }
                    if (answer.empty()) fprintf(stderr, "e-AMUSEMENT: %s\n", error.c_str());
                    done(std::move(answer));
                }).detach();
            });
            if (ea_.online()) {   // -url (-ea is ignored with it, as in spice2x)
                remote_ = std::make_unique<hk::ea::Remote>(ea_.http_url());
                std::string why;
                if (ea_.smartea && !remote_->reachable(&why)) {
                    printf("e-AMUSEMENT: %s doesn't answer (%s): the server inside HIRAKU (-smartea)\n", ea_.url.c_str(), why.c_str());
                    remote_.reset();
                } else {
                    printf("e-AMUSEMENT: online, %s\n", ea_.http_url().c_str());
                }
            }
            if (!ea_.pcbid.empty()) printf("e-AMUSEMENT: PCBID %s\n", ea_.pcbid.c_str());
            // the server's files: in the saves' default places (the game window's), one folder for all
            // the pop'n games, beside their saves, so a card's player carries from one to the next;
            // with --nvram elsewhere, that folder's own
            std::string ea_dir = run_dir_ + "/eamuse";
            if (!shared_saves_.empty()) {
                merge_eamuse(ea_dir, shared_saves_ + "/eamuse");
                ea_dir = shared_saves_ + "/eamuse";
            }
            server_ = std::make_unique<hk::py::EamuseServer>(ea_dir);
            // the cabinet's clock (init_emulator: 2003-06-01 12:00 JST) plus the machine's own time
            server_->now = [this] { return (int64_t)1054468800 + (int64_t)seconds(); };
            // -autopin: pop'n 14 asks for the PIN 3 seconds after the card is read, and takes it at
            // its fourth digit
            if (ea_.autopin && o.game == "popn14")
                server_->on_card_pin = [](const std::string &pin) { hk::macro::post("PIN", hk::EaOptions::pin_steps(pin, 180, false)); };
            if (!lan_where_.empty()) {   // NET TAISEN with cabinets in other processes (lancable.h)
                if (!lan_.open(lan_where_, lan_host_, lan_cabinets_)) return false;
                net_->set_cabinet(lan_.index());
                net_->cable = [this](std::vector<hk::py::LanCable::Packet> &out, std::vector<hk::py::LanCable::Packet> &in) {
                    lan_.exchange(out, in);
                };
            }
            // online: NET TAISEN through the server's relay, when it is hiraku-eamuse (netrelay.h)
            if (remote_ && lan_where_.empty()) {
                std::string why;
                if (relay_.open(remote_->host(), hk::py::Lobby::PORT, 3, &why)) {
                    const uint32_t a = relay_.address();
                    printf("e-AMUSEMENT: NET TAISEN online through %s:%u, this cabinet %u.%u.%u.%u\n", remote_->host().c_str(),
                           hk::py::Lobby::PORT, a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
                    net_->set_online(a);
                    net_->cable = [this](std::vector<hk::py::LanCable::Packet> &out, std::vector<hk::py::LanCable::Packet> &in) {
                        relay_.exchange(out, in);
                    };
                } else {
                    printf("e-AMUSEMENT: no NET TAISEN relay (%s): online battles find nobody\n", why.c_str());
                }
            }
        } else if (!lan_where_.empty()) {
            fprintf(stderr, "--lan-host/--lan-join: pop'n music only, and not with --bare-board (the board has no network then)\n");
            return false;
        }
        if (!FileSystem::WriteStringToFile(py1_path_.c_str(), py1)) { fprintf(stderr, "python: cannot write %s\n", py1_path_.c_str()); return false; }

        const std::string bios = o.data_path(m["bios"]);
        const std::string keys = o.data_path(m["keys"]);
        if (!exists(bios)) { fprintf(stderr, "python: missing BIOS %s\n", bios.c_str()); return false; }
        return init_emulator(bios, keys);
    }
    const std::string &family() const override { return family_; }

    // Konami Python 2 (PLAN.md section 23, PY-6): Reliquary's .py2 descriptor. The console's NVRAM
    // (the BIOS writes it) is the run's own copy; the disk's writes go to the overlay under
    // pcsx2/, so a run starts from the dumps. The network is the core's own (ethnet.h).
    bool write_py2(const Options &o, std::map<std::string, std::string> &m) {
        std::string py2 = "[Game]\nName=" + m["name"] + "\nRegion=NTSC-J\nInputType=" +
                          (m["input_type"].empty() ? std::string("0") : m["input_type"]) + "\n";
        if (!m["game_id"].empty()) py2 += "GameId=" + m["game_id"] + "\n";
        for (const char *dip : {"DIPSW1", "DIPSW2", "DIPSW3", "DIPSW4", "Force31kHz"}) py2 += std::string(dip) + "=false\n";
        struct F { const char *key, *ini; bool writable; };
        const F files[] = {
            {"hdd", "HddImagePath", false},         {"hdd_id", "HddIdPath", false},
            {"ilink_id", "IlinkIdPath", false},     {"nvram", "NvRamPath", true},
            {"dongle_black", "DongleBlackPath", false}, {"dongle_white", "DongleWhitePath", false},
        };
        for (const F &f : files) {
            if (m[f.key].empty()) continue;
            const std::string src = o.data_path(m[f.key]);
            if (!exists(src)) { fprintf(stderr, "python: missing %s (%s)\n", src.c_str(), f.key); return false; }
            std::string path = src;
            if (f.writable) {
                path = run_dir_ + "/" + base_name(src);
                if (!exists(path) && !copy_file(src, path)) { fprintf(stderr, "python: cannot copy %s\n", src.c_str()); return false; }
            }
            py2 += std::string(f.ini) + "=" + path + "\n";
        }
        py1_path_ = run_dir_ + "/" + o.game + ".py2";
        eth_ = std::make_unique<hk::py::EtherNet>();
        // e-AMUSEMENT (on in the game's NETWORK OPTIONS): the server inside HIRAKU, its files beside
        // the run's NVRAM
        server_ = std::make_unique<hk::py::EamuseServer>(run_dir_ + "/eamuse");
        server_->now = [this] { return (int64_t)1054468800 + (int64_t)seconds(); };
        eth_->http = [this](const std::string &req) { return server_->handle(req); };
        // the card unit (gfdmcard.h: the 573 GuitarFreaks and DrumMania's magnetic card reader and
        // 10-key) on the P2IO's serial port 0; its card, the run folder's card1.bin
        card_ = std::make_unique<hk::py::GfdmCardUnit>(run_dir_ + "/card1.bin");
        eth_->now = [this] { return server_->now(); };
        if (!FileSystem::WriteStringToFile(py1_path_.c_str(), py2)) { fprintf(stderr, "python: cannot write %s\n", py1_path_.c_str()); return false; }
        return true;
    }

    void set_frame_hook(FrameHook h) override { frame_hook_ = std::move(h); }
    void set_audio_sink(AudioSink s) override { audio_sink_ = std::move(s); }
    void set_on_fatal(std::function<void()> f) override { on_fatal_ = std::move(f); }

    [[noreturn]] void run() override {
        // each vsync handed over once: at its post to the GS (its own picture), or at the CPU's
        // vsync (the previous one's: --picture-overlap, or a [[rate]] above 90 Hz). The choice for
        // a vsync is made at its first hook, the CPU's
        Hiraku::vsync_cpu = [] {
            self_->picture_now_ = self_->want_picture_now_;
            if (!self_->picture_now_) self_->on_vsync();
        };
        Hiraku::vsync_posted = [] {
            if (self_->picture_now_) self_->on_vsync();
        };
        Hiraku::audio_sample = [](float l, float r) {
            self_->audio_.push_back(l);
            self_->audio_.push_back(r);
        };
        Hiraku::p1io_output = [](const u32 *payload, u32 quads) { self_->on_p1io_output(payload, quads); };
        if (const char *f = getenv("HK_PY_P1IO_LOG")) p1io_log_ = fopen(f, "w");
        Hiraku::full_frames = full_frames_;
        if (!no_picture_) Hiraku::gs_frame = [](const u32 *rgba, u32 w, u32 h) { self_->on_gs_frame(rgba, w, h); };
        if (const char *f = getenv("HK_PY_CARD_LOG")) card_log_ = fopen(f, "w");
        if (const char *f = getenv("HK_PY_NET_LOG")) net_log_ = fopen(f, "w");
        if (net_) {
            if (net_log_) net_->log = server_->log = [this](const std::string &s) {
                std::lock_guard<std::mutex> l(net_log_mutex_);
                fprintf(net_log_, "%llu %s\n", (unsigned long long)frame_, s.c_str());
                fflush(net_log_);
            };
            Hiraku::net_command = [](u32 ch, const u32 *p, u32 q) { return self_->net_->command(ch, p, q); };
        }
        // HK_PY_DISK_LOG=FILE: every read of the board's disk and CF card, "frame medium offset
        // size" (medium 0 the disk, 1 the CF card): where a game keeps what it reads
        if (const char *f = getenv("HK_PY_DISK_LOG")) disk_log_ = fopen(f, "w");
        if (hdd_overlay_ || cf_overlay_ || disk_log_)   // mods' replaced bytes, laid over the board's disk reads
            Hiraku::disk_read = [](u32 medium, u64 off, u8 *data, u32 size) {
                if (self_->disk_log_) {
                    fprintf(self_->disk_log_, "%llu %u 0x%llx 0x%x\n", (unsigned long long)self_->frame_, medium,
                            (unsigned long long)off, size);
                    fflush(self_->disk_log_);
                }
                hk::overlay::apply(medium ? self_->cf_overlay_ : self_->hdd_overlay_, off, data, size);
            };
        if (eth_) {   // Python 2: the PS2's own network adapter, answered here (ethnet.h)
            if (net_log_) eth_->log = server_->log = [this](const std::string &s) {
                std::lock_guard<std::mutex> l(net_log_mutex_);
                fprintf(net_log_, "%llu %s\n", (unsigned long long)frame_, s.c_str());
                fflush(net_log_);
            };
            Hiraku::eth_send = [](const u8 *f, u32 n) { self_->eth_->send(f, n); };
            Hiraku::eth_poll = [](u8 *f, u32 capacity) { return self_->eth_->take(f, capacity); };
            Hiraku::sync_disk = true;   // the hard disk's writes at once, not at the I/O thread's pace
            Hiraku::p2io_lamp = [](u8 bank, u8 value) { self_->on_p2io_lamp(bank, value); };
            // the card reader on serial port 0 (write_py2); HK_PY_P2IO_LOG: both ways
            Hiraku::p2io_sci_ports = 1;
            Hiraku::p2io_sci_write = [](u8 port, const u8 *d, u32 n) {
                std::vector<uint8_t> reply;
                if (port == 0 && self_->card_) {
                    self_->card_->write(d, n, reply);
                    self_->sci_in_.insert(self_->sci_in_.end(), reply.begin(), reply.end());
                    self_->card_changed();
                }
                if (FILE *f = self_->p1io_log_) {
                    fprintf(f, "%llu sci%u >", (unsigned long long)self_->frame_, port);
                    for (u32 i = 0; i < n; i++) fprintf(f, " %02x", d[i]);
                    if (!reply.empty()) {
                        fprintf(f, "\n%llu sci%u <", (unsigned long long)self_->frame_, port);
                        for (uint8_t b : reply) fprintf(f, " %02x", b);
                    }
                    fputc('\n', f);
                }
            };
            Hiraku::p2io_sci_read = [](u8 port, u8 *d, u32 capacity) -> u32 {
                auto &q = self_->sci_in_;
                const u32 n = (u32)std::min<size_t>(capacity, q.size());
                std::copy(q.begin(), q.begin() + n, d);
                q.erase(q.begin(), q.begin() + n);
                if (FILE *f = self_->p1io_log_) {
                    fprintf(f, "%llu sci%u read %u:", (unsigned long long)self_->frame_, port, capacity);
                    for (u32 i = 0; i < n; i++) fprintf(f, " %02x", d[i]);
                    fputc('\n', f);
                }
                return n;
            };
            if (const char *f = getenv("HK_PY_P2IO_LOG")) p1io_log_ = fopen(f, "w");
        }
        if (card_) Hiraku::uart_write = [](const u8 *data, u32 size) {
            std::vector<uint8_t> reply;
            if (!self_->card_->write(data, size, reply)) {
                if (FILE *f = self_->card_log_) {   // what the card unit isn't, as written
                    fprintf(f, "%llu > (left to the emulator)", (unsigned long long)self_->frame_);
                    for (u32 i = 0; i < size; i++) fprintf(f, " %02x", data[i]);
                    fprintf(f, "\n");
                    fflush(f);
                }
                return false;
            }
            if (FILE *f = self_->card_log_) {   // HK_PY_CARD_LOG: the bus, packet by packet
                if (!(size == 4 && data[3] == 0x55)) {
                    fprintf(f, "%llu >", (unsigned long long)self_->frame_);
                    for (u32 i = 0; i < size; i++) fprintf(f, " %02x", data[i]);
                    fprintf(f, "\n%llu <", (unsigned long long)self_->frame_);
                    for (size_t i = self_->card_->echoes() ? size : 0; i < reply.size(); i++) fprintf(f, " %02x", reply[i]);
                    fprintf(f, "\n");
                    fflush(f);
                }
            }
            if (!reply.empty()) Hiraku::uart_reply(reply.data(), (u32)reply.size());
            self_->card_changed();
            return true;
        };

        if (!VMManager::Internal::CPUThreadInitialize()) fatal("the emulator's CPU thread could not start");
        VMManager::ApplySettings();
        VMBootParameters params;
        params.filename = py1_path_;
        Error error;
        if (VMManager::Initialize(params, &error) != VMBootResult::StartupSuccess)
            fatal("the machine did not start: " + error.GetDescription());
        bind_inputs();
        VMManager::SetLimiterMode(LimiterModeType::Unlimited);   // the shell paces the machine
        VMManager::SetState(VMState::Running);
        while (VMManager::GetState() == VMState::Running) VMManager::Execute();
        fatal("the machine stopped");
    }
    void shutdown() override {
        // Reliquary writes the BBSRAM, dongles and disk overlay as the game goes
        fflush(nullptr);
    }
    const char *renderer_name() const {
        switch (renderer_) {
        case GSRendererType::SW: return "software";
        case GSRendererType::VK: return "Vulkan";
#ifdef _WIN32
        case GSRendererType::DX11: return "Direct3D 11";
        case GSRendererType::DX12: return "Direct3D 12";
#endif
        default: return "OpenGL";
        }
    }
    void report() override {
        printf("python: %llu frames (%.1f s), renderer %s%s, run files in %s\n", (unsigned long long)frame_,
               seconds(), renderer_name(),
               interp_ ? ", interpreters" : "", run_dir_.c_str());
        if (gs_waits_)   // host timing: "profile:" lines are not compared by tools/regress.py
            printf("profile: python: waited for the GS on %llu frames, %.1f ms in all\n", (unsigned long long)gs_waits_, gs_wait_ms_);
    }
    void debug_line(uint64_t f) override {
        printf("frame %llu: EE pc %08x, cycle %llu\n", (unsigned long long)f, cpuRegs.pc, (unsigned long long)cpuRegs.cycle);
    }

    double seconds() const override { return seconds_; }
    // a mod's [[rate]] (host/mods.h): PCSX2's NTSC frame rate is the vsync's interval in the
    // machine's own time (Counters.cpp vSyncInfoCalc), so the EE gets that many frames a second;
    // applied in the frame hook, on the emulator's side, at the next vsync
    bool set_vsync_rate(double hz) override {
        want_rate_ = hz > 0 ? hz : Pcsx2Config::GSOptions::DEFAULT_FRAME_RATE_NTSC;
        // above 90 Hz the GS overlaps the EE (the previous vsync's picture handed over): waiting
        // for each vsync's own took 2.4 ms of a 4.2 ms frame at 240 Hz (the owner's PC, the sound
        // breaking up when frames ran late), and the frame of latency it costs is 4 ms there
        want_picture_now_ = picture_now_user_ && want_rate_ <= 90;
        return true;
    }
    // a mod's patch (host/mods.h): the EE recompiler's blocks over it go (PCSX2 also notices a
    // write to a page it has translated; this makes sure)
    void code_changed(uint32_t addr, uint32_t len) override {
        if (Cpu) Cpu->Clear(addr & 0x1ffffffc, (len + (addr & 3) + 3) / 4);
    }
    const uint8_t *mem(uint32_t addr, uint32_t len) const override {
        addr &= 0x1fffffff;
        if (!eeMem || (uint64_t)addr + len > Ps2MemSize::MainRam) return nullptr;
        return eeMem->Main + addr;
    }
    uint64_t frame_count() const override { return frame_; }

    // The card reader for ea_.card[0] (-card0, card0.txt, a real reader's card): pop'n 14's IC card,
    // or the magnetic card of pop'n 9-13, a file per number (cards/NUMBER.bin; card.bin without one)
    void make_card() {
        card_unit_ = nullptr;
        if (game_ == "popn14") {
            printf("e-AMUSEMENT: IC card %s%s\n", ea_.card[0].c_str(), ea_.autocard[0] ? ", presented by itself" : "");
            card_ = std::make_unique<hk::py::IcCardUnit>(ea_.card[0]);
            return;
        }
        std::string card_file = run_dir_ + "/card.bin";
        if (!ea_.card[0].empty()) {
            mkdirs(run_dir_ + "/cards");
            card_file = run_dir_ + "/cards/" + ea_.card[0] + ".bin";
            printf("e-AMUSEMENT: card %s%s\n", ea_.card[0].c_str(), ea_.autocard[0] ? ", presented by itself" : "");
        }
        const bool had_card = exists(card_file);
        auto unit = std::make_unique<hk::py::CardUnit>(card_file, hk::py::CardUnit::fresh_header(game_));
        // one card for all the pop'n games (docs/EAMUSE3.md, "One card for all"): a numbered
        // card this game has never seen is shown as the game before's card, as that game left
        // it in its save, so this game offers to carry the player over
        static const std::map<std::string, std::string> before = {
            {"popn10", "popn9"}, {"popn11", "popn10"}, {"popn12", "popn11"}, {"popn13", "popn12"}};
        if (!had_card && !ea_.card[0].empty() && !shared_saves_.empty())
            if (auto it = before.find(game_); it != before.end()) {
                std::ifstream f(shared_saves_ + "/" + it->second + "/cards/" + ea_.card[0] + ".bin", std::ios::binary);
                std::vector<uint8_t> older((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                if (older.size() == 128 && (older[0] & 0x40)) {   // a card that game registered
                    unit->present_older(std::move(older));
                    printf("e-AMUSEMENT: card %s is new to this game: it is shown as %s's, to carry its player over\n",
                           ea_.card[0].c_str(), it->second.c_str());
                    unit->note = [](const std::string &s) { printf("card: %s\n", s.c_str()); fflush(stdout); };
                    card_unit_ = unit.get();
                }
            }
        card_ = std::move(unit);
    }

    // A real card reader's card (runtime/host/readers.h): player 1's card from now on, as -card0
    // would have made it, and put to the slot. A card only waiting at the slot (-autocard's, or
    // the card key's) makes way for it; while the reader has one in, nothing: the game gives that
    // back first. With a real reader, -autocard stops: its card would sit at the slot (the front
    // sensor on in test mode) and turn the reader's away.
    bool card_read(int player, const std::string &number) override {
        if (!card_ || player != 0) return false;
        if (ea_.autocard[0]) {
            ea_.autocard[0] = false;
            printf("e-AMUSEMENT: a card from the reader: the card is no longer presented by itself\n");
        }
        if (card_->card_offered() && !card_->card_inside()) card_->card_button();   // taken from the slot
        if (card_->card_offered()) {
            if (number != ea_.card[0]) printf("card: the reader has a card: take it back first\n");
            return true;
        }
        if (number != ea_.card[0]) {
            ea_.card[0] = number;
            make_card();
        }
        card_->card_button();
        if (!card_->card_offered()) card_->card_button();   // that took back the card given out: now this one
        card_changed();
        return true;
    }

    bool set_input(const std::string &name, int value) override {
        if (card_ && (name == "Card" || KEYPAD_SCAN.count(name))) {
            int &was = held_[name];
            const bool edge = (value != 0) != (was != 0);
            was = value;
            if (!edge) return true;
            if (name == "Card") {
                if (value) card_->card_button();
                card_changed();
            } else {
                card_->key(KEYPAD_SCAN.at(name), value != 0);
            }
            return true;
        }
        auto it = binds_.find(name);
        if (it == binds_.end()) return false;
        if (python2_) USB::SetDeviceBindValue(0, it->second, value ? 1.0f : 0.0f);   // the P2IO, on USB port 1
        else FireWire::SetP1IOBindValue(it->second, value ? 1.0f : 0.0f);
        return true;
    }
    int audio_rate() const override { return 48000; }
    bool audio_per_frame() const override { return true; }   // on_vsync()
    void display_cover(int, int, double &cw, double &ch) const override { cw = cover_w_, ch = cover_h_; }
    void turntable_add(int, int) override {}
    const std::vector<uint8_t> &lamp_state() const override { return lamps_; }
    uint64_t lamp_changes() const override { return lamp_changes_; }

    static PythonCore *self_;

private:
    bool init_emulator(const std::string &bios, const std::string &keys) {
        EmuFolders::SetAppRoot();
        if (!EmuFolders::SetResourcesDirectory()) { fprintf(stderr, "python: no emulator resources next to the binary\n"); return false; }
        // not EmuFolders::SetDataDirectory(): it would create the user's own ~/.config/PCSX2
        EmuFolders::DataRoot = run_dir_ + "/pcsx2";   // settings, game settings, the disk overlay
        EmuFolders::Settings = EmuFolders::DataRoot + "/inis";   // never the user's own PCSX2 settings
        mkdirs(EmuFolders::Settings);
        const char *why;
        if (!VMManager::PerformEarlyHardwareChecks(&why)) { fprintf(stderr, "python: %s\n", why); return false; }

        const std::string font = EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
        font_data_ = FileSystem::MapBinaryFileForRead(font.c_str());
        if (!font_data_.empty()) {
            std::vector<ImGuiManager::FontInfo> fonts;
            ImGuiManager::FontInfo fi{};
            fi.data = font_data_;
            fonts.push_back(fi);
            ImGuiManager::SetFonts(std::move(fonts));
        }

        SettingsInterface &si = s_settings;
        Host::Internal::SetBaseSettingsLayer(&s_settings);
        VMManager::SetDefaultSettings(si, true, true, true, true, true);

        // As the oracle runs (oracle/pcsx2/run.py): pinned, repeatable
        si.SetStringValue("Folders", "Bios", std::string(Path::GetDirectory(bios)).c_str());
        si.SetStringValue("Filenames", "BIOS", base_name(bios).c_str());
        si.SetStringValue("Filenames", "ArcadeBIOS", base_name(bios).c_str());
        const std::pair<const char *, const char *> key_files[] = {
            {"MgChallengeIvFile", "civ"}, {"MgCardKeyStoreFile", "cks"}, {"MgEncryptedKeyStoreFile", "eks"},
            {"MgKeyStoreKeyFile", "kek"}, {"ArcadeKelfOverrideKbitFile", "arcade_kelf_override_kbit"},
            {"ArcadeKelfOverrideKcFile", "arcade_kelf_override_kc"}};
        for (const auto &[k, f] : key_files)
            if (!python2_ || !strstr(k, "Arcade"))   // Python 2 boots the retail way: no arcade KELF overrides
                si.SetStringValue("Security", k, (keys + "/" + f + ".bin").c_str());
        si.SetBoolValue("EmuCore", "EnablePatches", false);
        si.SetBoolValue("EmuCore", "EnableCheats", false);
        si.SetBoolValue("EmuCore", "EnablePINE", false);
        // The cabinet's clock: 2003-06-01 12:00 JST, the time HIRAKU's server gives. The board's RTC
        // keeps UTC (pop'n 11-14 add 9 hours to what they read, pop'n 12 0x1a8468), and Reliquary
        // stores the manual time plus 9 hours (CDVD.cpp: the PS2's RTC is JST), so this is 9 hours
        // before 03:00 UTC. pop'n 12's NINPOUCHO only runs from 10:55 to 23:05 JST (0x1a2058).
        si.SetBoolValue("EmuCore", "ManuallySetRealTimeClock", true);
        si.SetIntValue("EmuCore", "RtcYear", 3);
        si.SetIntValue("EmuCore", "RtcMonth", 5);
        si.SetIntValue("EmuCore", "RtcDay", 31);
        si.SetIntValue("EmuCore", "RtcHour", 18);
        si.SetIntValue("EmuCore", "RtcMinute", 0);
        si.SetIntValue("EmuCore", "RtcSecond", 0);
        si.SetBoolValue("EmuCore/Speedhacks", "vuThread", false);
        // as Reliquary's game list sets up every Python 1 game (GameList.cpp), but Perfect Pool
        si.SetBoolValue("EmuCore/Gamefixes", "OPHFlagHack", io_mode_ != "PPOOL");
        for (const char *cpu : {"EnableEE", "EnableIOP", "EnableVU0", "EnableVU1"})
            si.SetBoolValue("EmuCore/CPU/Recompiler", cpu, !interp_);
        si.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(renderer_));
        si.SetBoolValue("EmuCore/GS", "FrameLimitEnable", false);
        si.SetIntValue("EmuCore/GS", "VsyncEnable", 0);
        si.SetBoolValue("EmuCore/GS", "OsdShowMessages", false);
        si.SetStringValue("SPU2/Output", "Backend", "Null");
        si.SetBoolValue("InputSources", "SDL", false);
        si.SetBoolValue("InputSources", "XInput", false);
        Pad::ClearPortBindings(si, 0);
        si.ClearSection("Hotkeys");
        si.ClearSection("FireWire");   // no host bindings: the shell sets the inputs
        si.SetBoolValue("MemoryCards", "Slot2_Enable", false);
        if (python2_) si.SetBoolValue("MemoryCards", "Slot1_Enable", false);   // Python 2 has no memory-card dongle
        si.SetBoolValue("Logging", "EnableSystemConsole", pcsx2_log_);
        si.SetBoolValue("Logging", "EnableFileLogging", false);
        si.SetBoolValue("Logging", "EnableEEConsole", pcsx2_log_);
        si.SetBoolValue("Logging", "EnableIOPConsole", pcsx2_log_);

        for (const auto &[section, key, v] : extra_settings_) si.SetStringValue(section.c_str(), key.c_str(), v.c_str());
        VMManager::Internal::LoadStartupSettings();
        // The shell ends a run with exit() from inside a vsync. PCSX2's static destructors
        // then assert (the VM is still mapped), so leave before them with the same status;
        // the shell has already saved and reported everything by then.
#ifdef _WIN32
        // Windows has no on_exit, and atexit isn't told the status: the shell's exits once a run
        // is going are exit(0) and an interrupted run's exit(130), which leaves with 0 here.
        std::atexit([] {
            fflush(nullptr);
            _exit(0);
        });
#else
        on_exit([](int status, void *) {
            if (getenv("HK_PY_PROFILE")) prof::report();
            if (__llvm_profile_write_file) __llvm_profile_write_file();   // a PGO training build: _exit skips its atexit
            fflush(nullptr);
            _exit(status);
        }, nullptr);
#endif
        if (getenv("HK_PY_PROFILE")) prof::start();
        if (!EmuFolders::EnsureFoldersExist()) { fprintf(stderr, "python: cannot create %s\n", EmuFolders::DataRoot.c_str()); return false; }
        self_ = this;
        return true;
    }

    // MAME input names -> the interface board's bind indices (Reliquary's names)
    void bind_inputs() {
        std::map<std::string, u32> by_name;
        if (python2_) {
            for (const InputBindingInfo &b : USB::GetDeviceBindings(0)) by_name[b.name] = b.bind_index;
            for (const auto &[mame, p2io] : PY2_INPUTS)
                if (auto it = by_name.find(p2io); it != by_name.end()) binds_[mame] = it->second;
            return;
        }
        for (const InputBindingInfo &b : FireWire::GetP1IOBindings()) by_name[b.name] = b.bind_index;
        for (const auto &[mame, p1io] : POPN_INPUTS)
            if (auto it = by_name.find(p1io); it != by_name.end()) binds_[mame] = it->second;
    }

    // CPU thread: a JAMMA output command to the interface board (outputs; the input poll).
    // HK_PY_P1IO_LOG=<file>: each change of its payload, and the commands per frame.
    void on_p1io_output(const u32 *payload, u32 quads) {
        p1io_polls_++;
        // lamps: word 8, as shell/cabinet.cpp's pypopn family reads it
        if (quads > 8 && family_ == "pypopn") {
            const auto &defs = hk::cab::lamps(family_);
            for (size_t k = 0; k < defs.size() && k < lamps_.size(); k++) {
                const uint8_t lit = (payload[8] >> defs[k].bit) & 1;
                if (lit != lamps_[k]) lamps_[k] = lit, lamp_changes_++;
            }
        }
        std::vector<u32> words(payload, payload + std::min<u32>(quads, 16));
        if (words == p1io_last_) return;
        p1io_last_ = words;
        if (p1io_log_) {
            fprintf(p1io_log_, "%llu out", (unsigned long long)frame_);
            for (u32 w : words) fprintf(p1io_log_, " %08x", w);
            fputc('\n', p1io_log_);
        }
    }

    // CPU thread: a Python 2 lamp output (P2IO, a bank of 8). HK_PY_P2IO_LOG=<file>: each change.
    void on_p2io_lamp(u8 bank, u8 value) {
        if (bank >= 4 || p2io_lamps_[bank] == value) return;
        p2io_lamps_[bank] = value;
        if (family_ == "drmn") {
            const auto &defs = hk::cab::lamps(family_);
            for (size_t k = 0; k < defs.size() && k < lamps_.size(); k++)
                for (const Py2Lamp &l : PY2_DRMN_LAMPS)
                    if (defs[k].name == l.name) {
                        const uint8_t lit = (uint8_t)(((p2io_lamps_[l.bank] >> l.bit) & 1) ^ (l.active_low ? 1 : 0));
                        if (lit != lamps_[k]) lamps_[k] = lit, lamp_changes_++;
                    }
        }
        if (p1io_log_) {
            fprintf(p1io_log_, "%llu lamps %02x %02x %02x %02x\n", (unsigned long long)frame_, p2io_lamps_[0], p2io_lamps_[1],
                    p2io_lamps_[2], p2io_lamps_[3]);
            fflush(p1io_log_);
        }
    }

    // GS thread: the picture shown at this vsync, as 0x00RRGGBB
    void on_gs_frame(const u32 *rgba, u32 w, u32 h) {
        std::lock_guard lock(gs_mutex_);
        if (rgba && w && h) {
            gs_pic_.resize((size_t)w * h);
            uint32_t *out = gs_pic_.data();
            const size_t n = gs_pic_.size();
            for (size_t k = 0; k < n; k++) out[k] = __builtin_bswap32(rgba[k]) >> 8;   // R G B A bytes -> 0x00RRGGBB
            gs_w_ = (int)w, gs_h_ = (int)h;
        } else {
            gs_pic_.assign((size_t)gs_w_ * gs_h_, 0u);   // a blank frame: black, the last size
        }
        gs_fresh_ = true;
        gs_done_++;
        gs_cv_.notify_all();
    }

    // CPU thread, at each vsync: sound so far, the previous vsync's picture, the shell
    void on_vsync() {
        if (!frame_) {
            prof::cpu_thread();
            // HK_PY_WATCH=addr,addr,...: EE RAM words, printed when they change (for reading the game)
            if (const char *w = getenv("HK_PY_WATCH"))
                for (const char *p = w; *p;) {
                    char *e;
                    watch_.push_back({(uint32_t)strtoul(p, &e, 16), 0xdeadbeef});
                    p = *e ? e + 1 : e;
                }
        }
        // HK_PY_PACE=N: frames at N times the cabinet's 59.94 Hz, for runs without a window that
        // must keep time with something outside (NET TAISEN online between headless cabinets)
        static const double pace = getenv("HK_PY_PACE") ? atof(getenv("HK_PY_PACE")) : 0.0;
        if (pace > 0) {
            static auto next = std::chrono::steady_clock::now();
            next += std::chrono::nanoseconds((int64_t)(1e9 / (59.94 * pace)));
            std::this_thread::sleep_until(next);
        }
        if (net_) net_->poll();   // online answers that arrived
        if (autoplay_) apply_autoplay();
        if (card_ && ea_.autocard[0]) autocard();
        if (card_) card_hint();
        // HK_PY_EE_DUMP=FRAME[-LAST]:FILE: EE RAM (32 MiB) at that frame (FILE; the IOP's in FILE.iop),
        // or at each frame to LAST (FILE.FRAME), for reading a game that only exists in RAM (pop'n 13 and 14 load
        // theirs from a second stage)
        static const char *dump = getenv("HK_PY_EE_DUMP");
        if (dump) {
            char *e;
            const uint64_t first = strtoull(dump, &e, 10), last = *e == '-' ? strtoull(e + 1, nullptr, 10) : first;
            if (frame_ >= first && frame_ <= last) {
                std::string path = strchr(dump, ':') ? strchr(dump, ':') + 1 : "ee.bin";
                if (last != first) path += "." + std::to_string(frame_);
                if (FILE *f = fopen(path.c_str(), "wb")) {
                    fwrite(eeMem->Main, 1, Ps2MemSize::MainRam, f);
                    fclose(f);
                }
                if (FILE *f = fopen((path + ".iop").c_str(), "wb")) {   // and the IOP's 2 MiB
                    fwrite(iopMem->Main, 1, Ps2MemSize::IopRam, f);
                    fclose(f);
                }
            }
        }
        for (auto &[addr, last] : watch_) {
            uint32_t v;
            memcpy(&v, eeMem->Main + (addr & 0x1fffffc), 4);
            if (v != last) printf("watch: frame %llu %08x = %08x\n", (unsigned long long)frame_, addr, v);
            last = v;
        }
        if (p1io_log_ && p1io_polls_ != p1io_polls_logged_) {
            fprintf(p1io_log_, "%llu polls %llu\n", (unsigned long long)frame_, (unsigned long long)(p1io_polls_ - p1io_polls_logged_));
            p1io_polls_logged_ = p1io_polls_;
            fflush(p1io_log_);
        }
        // the machine's time at this vsync: none at the first (the machine has run no cycles yet: its
        // sound up to here is what the SPU2 made, and the frame hook's time must match it)
        if (frame_) seconds_ += 1.0 / rate_;
        frame_++;
        if (want_rate_ != rate_) {
            rate_ = want_rate_;
            EmuConfig.GS.FramerateNTSC = (float)rate_;
            UpdateVSyncRate(false);
        }
        if (audio_sink_ && !audio_.empty()) audio_sink_(audio_.data(), (int)(audio_.size() / 2));
        audio_.clear();
        {
            std::unique_lock lock(gs_mutex_);
            // the picture wanted: this vsync's (just posted to the GS) or the previous one's
            const uint64_t want = picture_now_ ? frame_ : frame_ - 1;
            if (!no_picture_ && want >= 1 && gs_done_ < want) {
                const auto t0 = std::chrono::steady_clock::now();
                if (!gs_cv_.wait_for(lock, std::chrono::seconds(10), [&] { return gs_done_ >= want; }))
                    fprintf(stderr, "python: frame %llu: no picture from the GS after 10 s\n", (unsigned long long)frame_);
                gs_waits_++;
                gs_wait_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            }
            // take the GS's picture by swapping buffers: it writes the whole of its next one
            if (gs_fresh_) {
                std::swap(pic_, gs_pic_);
                pic_w_ = gs_w_, pic_h_ = gs_h_;
                gs_fresh_ = false;
                update_cover();
            } else if (pic_.empty()) {
                pic_.assign((size_t)gs_w_ * gs_h_, 0u);   // before the first picture: black
                pic_w_ = gs_w_, pic_h_ = gs_h_;
            }
        }
        if (frame_hook_) frame_hook_(frame_, pic_.data(), pic_w_, pic_h_);
    }

    // display_cover(): the GS's display area inside an NTSC TV's raster. The PCRTC counts width in
    // its 54 MHz clocks (2560 for a 640 or a 512 picture), and a TV's 4:3 is the whole active
    // line, 52.7 us or 2844 clocks (ITU-R BT.601, PCSX2's overscan area: 711 pixels of 4 clocks),
    // by 480 lines (240 a field). The games are centred in it, so the picture is placed centred.
    void update_cover() {
        cover_w_ = cover_h_ = 1.0;
        if (fill_screen_) return;
        auto reg = [](unsigned off) { uint64_t v; memcpy(&v, PS2MEM_GS + off, 8); return v; };
        const uint64_t pmode = reg(0x00), smode1 = reg(0x10), smode2 = reg(0x20);
        if (((smode1 >> 13) & 3) != 2) return;   // not NTSC
        const uint64_t display = reg((pmode & 2) ? 0xa0 : 0x80);   // circuit 2's area if it's on, else 1's
        const double dw = (double)((display >> 32) & 0xfff) + 1, dh = (double)((display >> 44) & 0x7ff) + 1;
        cover_w_ = std::clamp(dw / 2844.0, 0.1, 1.0);
        cover_h_ = std::clamp(dh / ((smode2 & 1) ? 480.0 : 240.0), 0.1, 1.0);
    }

    // --autoplay: the game's own demo play. At each song's start pop'n music 9 sets its autoplay
    // flag (0x257084) to "mode == attract demo" (FUN_0010e570); while it is set, every note that
    // reaches the line is judged GREAT (FUN_0010cbe8). The play screen's button reader
    // (FUN_0010dd80) gives nothing in the demo, by its own test of the mode. The mod makes both
    // behave as in the demo, outside it too. Written as PCSX2's patches are, at a vsync on the CPU
    // thread through the TLB, so the recompiler drops the blocks; checked each vsync, because the
    // program is loaded from the disk after boot.
    struct CodePatch { const char *game; uint32_t addr, was, now; };
    static constexpr CodePatch AUTOPLAY[] = {
        {"popn9", 0x10e644, 0x54430003, 0x00000000},   // bnel v0,v1 (mode != 2: flag = 0) -> nop
        {"popn9", 0x10dd9c, 0x10430036, 0x10000036},   // beq v0,v1 (mode == 2: no buttons) -> b
    };
    static bool has_autoplay(const std::string &game) {
        for (const CodePatch &p : AUTOPLAY)
            if (game == p.game) return true;
        return false;
    }
    void apply_autoplay() {
        for (const CodePatch &p : AUTOPLAY)
            if (opt_.game == p.game && memRead32(p.addr) == p.was) {
                memWrite32(p.addr, p.now);
                printf("autoplay: %08x patched (frame %llu)\n", p.addr, (unsigned long long)frame_);
            }
    }

    [[noreturn]] void fatal(const std::string &why) {
        fprintf(stderr, "python: %s\n", why.c_str());
        if (on_fatal_) on_fatal_();
        _exit(1);
    }

    Options opt_;
    std::string family_, run_dir_, py1_path_, io_mode_;
    std::string game_;   // the game's id (popn9 ... popn14)
    std::unique_ptr<hk::py::CardReader> card_;
    FILE *card_log_ = nullptr;
    std::unique_ptr<hk::py::VirtualNet> net_;
    hk::py::CardUnit *card_unit_ = nullptr;   // the card unit, while it shows an older game's card
    std::string shared_saves_;   // the saves' folder when this game's is in its default place there, else ""
    hk::py::LanCable lan_;   // --lan-host/--lan-join
    hk::py::RelayClient relay_;   // online NET TAISEN (-url with hiraku-eamuse)
    std::string lan_where_;
    bool lan_host_ = false;
    int lan_cabinets_ = 2;
    FILE *net_log_ = nullptr;
    std::unique_ptr<hk::py::EamuseServer> server_;
    std::vector<std::pair<uint32_t, uint32_t>> watch_;   // HK_PY_WATCH: address, last value
    std::unique_ptr<hk::ea::Remote> remote_;   // -url: online
    hk::EaOptions ea_;   // spice2x's e-AMUSEMENT options
    uint64_t autocard_wait_ = 0;
    // A call from the game, as it goes out: the PCBID put in (-p). The answer, when the core
    // gives it itself: a new card's number (-card0), so the server never numbers a card that
    // won't carry its number. "": the server answers (through done, which this may wrap).
    std::string on_call(std::string &req, std::function<void(std::string)> &done) {
        bool numbering = false, getnewid = false, portfw = false;
        const std::string &number = ea_.card[0];
        hk::py::EamuseServer::edit(req, [&](std::string &xml) {
            if (!ea_.pcbid.empty()) {
                hk::py::EamuseServer::set_attr(xml, "srcid", ea_.pcbid);
                hk::py::EamuseServer::set_attr(xml, "gpcb_id", ea_.pcbid);
            }
            numbering = xml.find("<numbering") != std::string::npos && xml.find("method=\"assign\"") != std::string::npos;
            // carrying the older card's player over: the game asks about it (pop'n 10/11 read
            // "NUMBER:MODEL", pop'n 11 renewc, pop'n 12 playerdata get with the older game's
            // model, playerdata conv)
            if (card_unit_ && ((!number.empty() && xml.find(number + ":") != std::string::npos) || xml.find("\"renewc\"") != std::string::npos ||
                               (xml.find("\"playerdata\"") != std::string::npos && xml.find(" model=\"") != std::string::npos) ||
                               xml.find("method=\"conv\"") != std::string::npos))
                card_unit_->older_asked();
            // pop'n 14 asks for pop'n 13's player by the transfer number typed (ref_id), with no
            // card (card_id "-"): one card for all, the card's number goes along, so the server
            // finds the player of this card whatever was typed
            if (!number.empty() && xml.find("model=\"E13\"") != std::string::npos && xml.find("\"playerdata\"") != std::string::npos)
                hk::py::EamuseServer::set_attr(xml, "card_id", number);
            portfw = (xml.find("<portfw") != std::string::npos || xml.find("<facility") != std::string::npos) &&
                     xml.find("method=\"get\"") != std::string::npos;
            // pop'n 13 numbers a new card with cardmng.getnewid (a card id and its player's ref id):
            // with -card0 it asks the server about that number instead (inquire registers a card
            // it doesn't know), and the answer gets the number as the card id
            static const std::string method = "method=\"getnewid\"";
            const size_t at = xml.find(method);
            getnewid = !number.empty() && xml.find("<cardmng") != std::string::npos && at != std::string::npos;
            if (getnewid) xml.replace(at, method.size(), "method=\"inquire\" cardid=\"" + number + "\" update=\"1\"");
        });
        if (getnewid) {
            if (net_log_) {
                std::lock_guard<std::mutex> l(net_log_mutex_);
                fprintf(net_log_, "%llu cardmng.getnewid: inquire about the card's own number, %s (-card0)\n", (unsigned long long)frame_, number.c_str());
                fflush(net_log_);
            }
            done = [done = std::move(done), number](std::string answer) {
                hk::py::EamuseServer::edit(answer, [&](std::string &xml) {
                    if (const size_t at = xml.find("<cardmng"); at != std::string::npos) xml.insert(at + 8, " cardid=\"" + number + "\"");
                });
                done(std::move(answer));
            };
            return "";
        }
        // NET TAISEN: where other cabinets reach this one, the relay's address for it online
        // (netrelay.h) or its own on a LAN cable (lancable.h), its battle socket's port; in
        // portfw get's answer (pop'n 12 and 13) and in facility get's <portfw> (pop'n 14)
        const uint32_t reach = relay_.is_open() ? relay_.address() : lan_.is_open() ? 0xc0a83202u + (uint32_t)lan_.index() : 0;
        if (portfw && reach) {
            char ip[20];
            snprintf(ip, sizeof ip, "%u.%u.%u.%u", reach >> 24, (reach >> 16) & 255, (reach >> 8) & 255, reach & 255);
            if (net_log_) {
                std::lock_guard<std::mutex> l(net_log_mutex_);
                fprintf(net_log_, "%llu NET TAISEN: other cabinets reach this one at %s:5700 (in the answer)\n", (unsigned long long)frame_, ip);
                fflush(net_log_);
            }
            done = [done = std::move(done), ip = std::string(ip)](std::string answer) {
                hk::py::EamuseServer::edit(answer, [&](std::string &xml) {
                    if (const size_t g = xml.find("<globalip>"); g != std::string::npos) {   // facility
                        const size_t e = xml.find("</globalip>", g);
                        if (e != std::string::npos) xml.replace(g + 10, e - g - 10, ip);
                        return;
                    }
                    const size_t at = xml.find("<portfw");
                    if (at == std::string::npos) return;
                    const size_t end = xml.find('>', at);
                    const std::string el = xml.substr(at, end == std::string::npos ? std::string::npos : end - at);
                    if (el.find(" globalip=\"") == std::string::npos) xml.insert(at + 7, " globalip=\"\" port=\"\"");
                    hk::py::EamuseServer::set_attr(xml, "globalip", ip);
                    hk::py::EamuseServer::set_attr(xml, "port", "5700");
                });
                done(std::move(answer));
            };
            return "";
        }
        if (!numbering || number.empty()) return "";
        if (net_log_) {
            std::lock_guard<std::mutex> l(net_log_mutex_);
            fprintf(net_log_, "%llu numbering.assign: the card's own number, %s (-card0)\n", (unsigned long long)frame_, ea_.card[0].c_str());
            fflush(net_log_);
        }
        return server_->reply(req, "<response><numbering status=\"0\" number=\"" + ea_.card[0] + "\"></numbering></response>");
    }
    // -autocard: the card in the slot from the start, and again a while after the reader gives it
    // back (spice2x waits 8 s too), so the game takes it whenever it opens the slot.
    void autocard() {
        if (card_->card_offered()) { autocard_wait_ = 8 * 60; return; }
        if (autocard_wait_ && --autocard_wait_) return;
        card_->card_button();
        card_changed();
    }
    std::mutex net_log_mutex_;
    std::map<std::string, int> held_;   // the card button's and 10-key's last values
    std::string card_state_;
    // A card left at a slot the game has never opened: said once, on the window too, after 5 s
    // there in all (the player pressing the card key again only takes it back)
    void card_hint() {
        if (card_hinted_ || card_->slot_ever_opened() || !card_->card_offered() || card_->card_inside()) return;
        if (++card_waited_ < 5 * 60) return;
        card_hinted_ = true;
        const std::vector<std::string> lines = {
            "The card waits at the slot: the game hasn't opened it.",
            "Is e-AMUSEMENT on in its test mode? (e-AMUSEMENT OPTIONS,",
            "under NETWORK OPTIONS from pop'n 10; SAVE AND EXIT, start again)",
            "pop'n 9 opens it after Start."};
        printf("card:");
        for (const std::string &l : lines) printf(" %s", l.c_str());
        printf("\n");
        fflush(stdout);
#ifdef HK_WITH_SDL
        hk::host::notice::hint(lines);
#endif
    }
    bool card_hinted_ = false;
    uint64_t card_waited_ = 0;   // frames at a slot never opened

    void card_changed() {
        const std::string s = card_->state();
        if (s == card_state_) return;
        card_state_ = s;
        printf("card: %s (frame %llu)\n", s.empty() ? "back with the player" : s.c_str(), (unsigned long long)frame_);
        fflush(stdout);
    }
    GSRendererType renderer_ = GSRendererType::SW;
    bool interp_ = false, pcsx2_log_ = false, no_picture_ = false, picture_now_ = true, autoplay_ = false;
    bool picture_now_user_ = true, want_picture_now_ = true;   // --picture-overlap's choice; the next vsync's
    bool full_frames_ = true, fill_screen_ = false;
    double cover_w_ = 1.0, cover_h_ = 1.0;   // display_cover() of pic_
    bool python2_ = false;   // a Konami Python 2 (write_py2)
    int hdd_overlay_ = 0, cf_overlay_ = 0;   // mods' replaced bytes on the disk, the CF card (shell/overlay.h)
    FILE *disk_log_ = nullptr;               // HK_PY_DISK_LOG
    std::deque<uint8_t> sci_in_;   // the card reader's bytes for the game (P2IO serial port 0)
    uint8_t p2io_lamps_[4] = {0, 0, 0x01, 0x1f};   // the P2IO's lamp banks: all lamps off till the game writes them
    std::unique_ptr<hk::py::EtherNet> eth_;
    bool bare_board_ = false;   // --bare-board: no card unit or e-AMUSEMENT network, as plain Reliquary
    std::span<const u8> font_data_;   // mapped for the process's life
    std::map<std::string, u32> binds_;
    struct Setting { std::string section, key, value; };
    std::vector<Setting> extra_settings_;
    std::vector<uint8_t> lamps_;
    uint64_t lamp_changes_ = 0;
    FrameHook frame_hook_;
    AudioSink audio_sink_;
    std::function<void()> on_fatal_;
    uint64_t frame_ = 0;
    double seconds_ = 0;                     // the machine's time, through any change of rate
    double rate_ = Pcsx2Config::GSOptions::DEFAULT_FRAME_RATE_NTSC, want_rate_ = rate_;   // vsyncs a second
    std::vector<float> audio_;
    FILE *p1io_log_ = nullptr;
    uint64_t p1io_polls_ = 0, p1io_polls_logged_ = 0;
    std::vector<u32> p1io_last_;

    std::mutex gs_mutex_;
    std::condition_variable gs_cv_;
    uint64_t gs_done_ = 0;                    // GS vsyncs presented
    bool gs_fresh_ = false;                   // gs_pic_ holds a picture the CPU thread hasn't taken
    std::vector<uint32_t> gs_pic_, pic_;
    int gs_w_ = 640, gs_h_ = 448, pic_w_ = 640, pic_h_ = 448;
    uint64_t gs_waits_ = 0;
    double gs_wait_ms_ = 0;
};

PythonCore *PythonCore::self_ = nullptr;
PythonCore g_core;

// Host::RunOnCPUThread work, run at the next vsync's message pump (CPU thread)
std::mutex s_cpu_queue_mutex;
std::deque<std::function<void()>> s_cpu_queue;
std::thread::id s_cpu_thread;

}  // namespace

hk::Core &hk::core() { return g_core; }

//////////////////////////////////////////////////////////////////////////
// PCSX2's Host interface: what its core asks of a front end. Mostly nothing here:
// no UI, no window of its own (the GS runs surfaceless), no host input.
//////////////////////////////////////////////////////////////////////////

void Host::CommitBaseSettingChanges() {}
void Host::LoadSettings(SettingsInterface &, std::unique_lock<std::mutex> &) {}
void Host::CheckForSettingsChanges(const Pcsx2Config &) {}
bool Host::RequestResetSettings(bool, bool, bool, bool, bool) { return false; }
void Host::SetDefaultUISettings(SettingsInterface &) {}
bool Host::LocaleCircleConfirm() { return false; }
std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback() { return ProgressCallback::CreateNullProgressCallback(); }
void Host::ReportInfoAsync(const std::string_view title, const std::string_view message) {
    fprintf(stderr, "python: %.*s: %.*s\n", (int)title.size(), title.data(), (int)message.size(), message.data());
}
void Host::ReportErrorAsync(const std::string_view title, const std::string_view message) {
    fprintf(stderr, "python: error: %.*s: %.*s\n", (int)title.size(), title.data(), (int)message.size(), message.data());
}
void Host::OpenURL(const std::string_view) {}
bool Host::CopyTextToClipboard(const std::string_view) { return false; }
std::string Host::GetTextFromClipboard() { return {}; }
void Host::BeginTextInput() {}
void Host::EndTextInput() {}
std::optional<WindowInfo> Host::GetTopLevelWindowInfo() {
    WindowInfo wi;
    wi.type = WindowInfo::Type::Surfaceless;
    return wi;
}
void Host::OnInputDeviceConnected(const std::string_view, const std::string_view) {}
void Host::OnInputDeviceDisconnected(const InputBindingKey, const std::string_view) {}
void Host::SetMouseMode(bool, bool) {}
void Host::SetMouseLock(bool) {}
std::optional<WindowInfo> Host::AcquireRenderWindow(bool) { return GetTopLevelWindowInfo(); }
void Host::ReleaseRenderWindow() {}
void Host::BeginPresentFrame() {}
void Host::RequestResizeHostDisplay(s32, s32) {}
void Host::OnVMStarting() {}
void Host::OnVMStarted() {}
void Host::OnVMDestroyed() {}
void Host::OnVMPaused() {}
void Host::OnVMResumed() {}
void Host::OnGameChanged(const std::string &, const std::string &, const std::string &, const std::string &, u32, u32) {}
void Host::OnPerformanceMetricsUpdated() {}
void Host::OnSaveStateLoading(const std::string_view) {}
void Host::OnSaveStateLoaded(const std::string_view, bool) {}
void Host::OnSaveStateSaved(const std::string_view) {}
void Host::RunOnCPUThread(std::function<void()> function, bool block) {
    if (std::this_thread::get_id() == s_cpu_thread || s_cpu_thread == std::thread::id()) {
        function();
        return;
    }
    std::lock_guard lock(s_cpu_queue_mutex);
    s_cpu_queue.push_back(std::move(function));
    (void)block;   // only the CPU thread drains the queue; a blocking caller would deadlock it
}
void Host::RefreshGameListAsync(bool) {}
void Host::CancelGameListRefresh() {}
bool Host::IsFullscreen() { return false; }
void Host::SetFullscreen(bool) {}
void Host::OnCaptureStarted(const std::string &) {}
void Host::OnCaptureStopped() {}
void Host::RequestExitApplication(bool) {}
void Host::RequestExitBigPicture() {}
void Host::RequestVMShutdown(bool, bool, bool) { VMManager::SetState(VMState::Stopping); }
void Host::OnAchievementsLoginSuccess(const char *, u32, u32, u32) {}
void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason) {}
void Host::OnAchievementsHardcoreModeChanged(bool) {}
void Host::OnAchievementsRefreshed() {}
bool Host::InBatchMode() { return true; }
bool Host::InNoGUIMode() { return true; }
bool Host::ShouldPreferHostFileSelector() { return false; }
void Host::OpenHostFileSelectorAsync(std::string_view, bool, FileSelectorCallback callback, FileSelectorFilters, std::string_view) {
    callback(std::string());
}
int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs) { return lhs.compare(rhs); }
std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view) { return std::nullopt; }
std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32) { return std::nullopt; }
const char *InputManager::ConvertHostKeyboardCodeToIcon(u32) { return nullptr; }
BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

void Host::PumpMessagesOnCPUThread() {
    s_cpu_thread = std::this_thread::get_id();
    std::deque<std::function<void()>> work;
    {
        std::lock_guard lock(s_cpu_queue_mutex);
        work.swap(s_cpu_queue);
    }
    for (auto &f : work) f();
}

s32 Host::Internal::GetTranslatedStringImpl(const std::string_view, const std::string_view msg, char *tbuf, size_t tbuf_space) {
    if (msg.size() > tbuf_space) return -1;
    std::memcpy(tbuf, msg.data(), msg.size());
    return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char *, const char *msg, const char *, int count) {
    std::string ret(msg);
    const std::string n = std::to_string(count);
    for (size_t pos; (pos = ret.find("%n")) != std::string::npos;) ret.replace(pos, 2, n);
    return ret;
}
