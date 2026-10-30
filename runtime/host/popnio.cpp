// SPDX-License-Identifier: GPL-3.0-or-later
// The board's dialogue is ezusb.dll's (pop'n music 15-18's, read for docs/POPN-IO.md); the
// packets' layout matches bemanitools' ezusb2-popn (Unlicense) and CrazyRedMachine's PopnForwarder.
#include "popnio.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <regex>
#include <thread>

#include "../shell/cabinet.h"
#include "../shell/core.h"
#include "warp.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../shell/sys.h"
#include "ezusb/link.h"
#endif

#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace hk::host {

namespace popnio {

namespace {

constexpr size_t RECORD = 70;   // length, 0, address (2), type, 65 bytes
constexpr uint16_t RAM_END = 0x4000, CPUCS = 0xe600;

bool data_record(const uint8_t *r) {
    const unsigned len = r[0], addr = r[2] | r[3] << 8;
    return len >= 1 && len <= 64 && r[1] == 0 && r[4] == 0 && addr + len <= RAM_END;
}

}  // namespace

std::vector<Chunk> find_firmware(const std::vector<uint8_t> &f) {
    // the longest run of data records that ends in a record of another type
    size_t best = 0, best_n = 0;
    for (size_t o = 0; o + RECORD <= f.size(); o++) {
        if (!data_record(&f[o])) continue;
        size_t n = 0, p = o;
        while (p + RECORD <= f.size() && data_record(&f[p])) n++, p += RECORD;
        const bool ends = p + RECORD <= f.size() && f[p + 1] == 0 && f[p + 4] != 0 && f[p] <= 64;
        if (ends && n > best_n) best = o, best_n = n;
        if (n > 1) o = p - 1;   // records inside a run start no other run
    }
    std::vector<Chunk> out;
    if (best_n < 16) return out;   // not a firmware: a few look-alike bytes
    for (size_t k = 0; k < best_n;) {
        const uint8_t *r = &f[best + k * RECORD];
        Chunk c;
        c.addr = (uint16_t)(r[2] | r[3] << 8);
        c.data.assign(r + 5, r + 5 + r[0]);
        // ezusb.dll's chunks: the next record joins while it follows on and the chunk is under 0x3e0
        for (k++; k < best_n; k++) {
            const uint8_t *n = &f[best + k * RECORD];
            if ((size_t)(n[2] | n[3] << 8) != c.addr + c.data.size() || c.data.size() > 0x3e0) break;
            c.data.insert(c.data.end(), n + 5, n + 5 + n[0]);
        }
        out.push_back(std::move(c));
    }
    return out;
}

uint32_t lamp_bits(const std::string &name) {
    static const std::regex button(R"(Button Lamp ([1-9]))"), top(R"(Top Lamp ([1-5]))"), side(R"(Side Lamp ([1-4]))");
    std::smatch m;
    if (std::regex_match(name, m, button)) {
        const int k = std::stoi(m[1]);
        return k == 1 ? 0xfu << 20 : 1u << (22 + k);   // button 1: bits 20-23, as the game writes it
    }
    if (std::regex_match(name, m, top)) return 1u << (std::stoi(m[1]) - 1);
    if (std::regex_match(name, m, side)) return 1u << (7 + std::stoi(m[1]));   // left 1-2, right 1-2
    return 0;
}

std::array<uint8_t, 64> out_packet(uint32_t lamps, uint8_t node, uint8_t cmd) {
    std::array<uint8_t, 64> p{};
    p[2] = node;
    p[3] = cmd;
    p[8] = lamps & 0x1f;   // ezusb.dll keeps bits 0-4, 8-11 and 16-31 (12-15 are dropped)
    p[9] = lamps >> 8 & 0x0f;
    p[10] = lamps >> 16 & 0xff;
    p[11] = lamps >> 24 & 0xff;
    return p;
}

uint32_t pad(const uint8_t *in) { return ~(uint32_t)(in[8] | in[9] << 8 | in[10] << 16 | (uint32_t)in[11] << 24); }
uint16_t coins(const uint8_t *in) { return (uint16_t)(in[5] << 8 | in[6]); }

const std::vector<Input> &inputs() {
    static const std::vector<Input> all = [] {
        std::vector<Input> v = {{6, "Service"}, {7, "Test"}};
        for (int k = 0; k < 9; k++) v.push_back({8 + k, "P1 Button " + std::to_string(k + 1)});
        return v;
    }();
    return all;
}

bool allowed_control(uint8_t request, uint16_t value, const uint8_t *data, size_t n) {
    if (request != 0xa0) return false;
    if (value == CPUCS) return n == 1 && data[0] <= 1;
    return n > 0 && value + n <= RAM_END;
}

bool allowed_out(const uint8_t *p, size_t n) {
    if (n != 64) return false;
    for (int k : {0, 1, 4, 5, 6, 7})
        if (p[k]) return false;
    for (size_t k = 12; k < n; k++)
        if (p[k]) return false;
    return (p[2] == 0 && p[3] == 0) || (p[2] == 0x09 && p[3] == 0x03);   // polling; the coin mode
}

bool allowed_bulk(const uint8_t *data, size_t n) {
    for (size_t k = 0; k < n; k++)
        if (data[k]) return false;
    return true;
}

#ifdef __linux__
namespace {

std::string read_line(const std::string &path) {
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    return s;
}

class LinuxUsb : public Usb {
public:
    ~LinuxUsb() override { close(); }

    std::vector<Device> scan() override {
        std::vector<Device> out;
        DIR *d = opendir("/sys/bus/usb/devices");
        if (!d) return out;
        while (dirent *e = readdir(d)) {
            const std::string port = e->d_name;
            if (port.empty() || port[0] == '.' || port.find(':') != std::string::npos || port.compare(0, 3, "usb") == 0) continue;
            const std::string dir = "/sys/bus/usb/devices/" + port + "/";
            Device v;
            v.port = port;
            v.vid = (uint16_t)strtoul(read_line(dir + "idVendor").c_str(), nullptr, 16);
            v.pid = (uint16_t)strtoul(read_line(dir + "idProduct").c_str(), nullptr, 16);
            // the device descriptor's string indexes (bytes 14-16): all 0 on a bare chip
            std::ifstream f(dir + "descriptors", std::ios::binary);
            uint8_t desc[18] = {};
            if (!f.read((char *)desc, sizeof desc)) continue;
            v.bare = !desc[14] && !desc[15] && !desc[16];
            v.name = desc[14] == 1 ? read_line(dir + "manufacturer") : desc[15] == 1 ? read_line(dir + "product")
                                                                                     : read_line(dir + "serial");
            out.push_back(v);
        }
        closedir(d);
        return out;
    }

    bool open(const std::string &port) override {
        close();
        const std::string dir = "/sys/bus/usb/devices/" + port + "/";
        const int bus = atoi(read_line(dir + "busnum").c_str()), dev = atoi(read_line(dir + "devnum").c_str());
        char node[64];
        snprintf(node, sizeof node, "/dev/bus/usb/%03d/%03d", bus, dev);
        m_fd = ::open(node, O_RDWR | O_CLOEXEC);
        m_node = node;
        return m_fd >= 0;
    }

    void close() override {
        if (m_fd < 0) return;
        if (m_claimed) {
            unsigned int i = 0;
            ioctl(m_fd, USBDEVFS_RELEASEINTERFACE, &i);
        }
        ::close(m_fd);
        m_fd = -1;
        m_claimed = false;
    }

    bool control_out(uint8_t request, uint16_t value, uint16_t index, const uint8_t *data, uint16_t n) override {
        usbdevfs_ctrltransfer c{};
        c.bRequestType = 0x40;
        c.bRequest = request;
        c.wValue = value;
        c.wIndex = index;
        c.wLength = n;
        c.timeout = 10000;   // ezusb.dll's
        c.data = (void *)data;
        return m_fd >= 0 && ioctl(m_fd, USBDEVFS_CONTROL, &c) == n;
    }

    int transfer(uint8_t ep, uint8_t *data, int n, int timeout_ms) override {
        if (m_fd < 0) return XFER_GONE;
        if (!m_claimed && !claim()) return errno == ENODEV ? XFER_GONE : XFER_ERROR;
        usbdevfs_bulktransfer b{};   // the kernel sends it as an interrupt transfer on an interrupt endpoint
        b.ep = ep;
        b.len = (unsigned)n;
        b.timeout = (unsigned)timeout_ms;
        b.data = data;
        const int r = ioctl(m_fd, USBDEVFS_BULK, &b);
        if (r >= 0) return r;
        if (errno == ETIMEDOUT) return XFER_TIMEOUT;
        if (errno == ENODEV || errno == ESHUTDOWN) return XFER_GONE;
        return XFER_ERROR;
    }

private:
    bool claim() {
        unsigned int i = 0;
        if (ioctl(m_fd, USBDEVFS_CLAIMINTERFACE, &i) != 0) {
            if (errno != EBUSY) return false;
            // a kernel driver has it (usbtest binds EZ-USB chips by their IDs): take it back
            usbdevfs_ioctl c{};
            c.ifno = 0;
            c.ioctl_code = USBDEVFS_DISCONNECT;
            ioctl(m_fd, USBDEVFS_IOCTL, &c);
            if (ioctl(m_fd, USBDEVFS_CLAIMINTERFACE, &i) != 0) return false;
        }
        m_claimed = true;
        return true;
    }

    int m_fd = -1;
    bool m_claimed = false;
    std::string m_node;
};

}  // namespace

std::unique_ptr<Usb> system_usb() { return std::make_unique<LinuxUsb>(); }
#else
std::unique_ptr<Usb> system_usb() { return nullptr; }
#endif

}  // namespace popnio

namespace {

using namespace popnio;

std::string g_where;      // "auto" or a port
std::string g_dll;        // the game's ezusb.dll (Linux: the firmware; Windows: what drives the board)
std::unique_ptr<Usb> g_usb;
std::atomic<bool> g_running{false};
std::thread g_thread;
std::mutex g_mutex;
uint32_t g_lamps = 0;     // from popnio_frame
uint32_t g_pad = 0;       // the controls, active high (pad())
uint32_t g_coins = 0;     // the board's coin count
bool g_have_in = false;
const bool g_log = getenv("HK_POPNIO_LOG") != nullptr;

void sleep_ms(int ms) {
    for (int t = 0; t < ms && g_running; t += 50) std::this_thread::sleep_for(std::chrono::milliseconds(std::min(50, ms - t)));
}

uint32_t lamps_now() {
    std::lock_guard<std::mutex> l(g_mutex);
    return g_lamps;
}

void have(uint32_t pad, uint32_t coins) {
    std::lock_guard<std::mutex> l(g_mutex);
    g_pad = pad;
    g_coins = coins;
    g_have_in = true;
}

void lost() {
    std::lock_guard<std::mutex> l(g_mutex);
    g_have_in = false;
}

void log_packet(const char *what, const uint8_t *p, int n) {
    if (!g_log) return;
    fprintf(stderr, "popn-io %s", what);
    for (int k = 0; k < n && k < 16; k++) fprintf(stderr, " %02x", p[k]);
    fprintf(stderr, "\n");
}

// Every out packet goes through here and is checked: nothing but polling and the coin mode
int send_out(const std::array<uint8_t, 64> &p) {
    if (!allowed_out(p.data(), p.size())) {
        fprintf(stderr, "popn-io: refused to send an out packet with command %02x %02x\n", p[2], p[3]);
        return Usb::XFER_ERROR;
    }
    log_packet("out", p.data(), 64);
    std::array<uint8_t, 64> q = p;
    return g_usb->transfer(0x01, q.data(), 64, 1000);
}

bool send_control(uint8_t request, uint16_t value, const uint8_t *data, size_t n) {
    if (!allowed_control(request, value, data, n)) {
        fprintf(stderr, "popn-io: refused a vendor request %02x at %04x\n", request, value);
        return false;
    }
    return g_usb->control_out(request, value, 0, data, (uint16_t)n);
}

// ezusb.dll's firmware load: the 8051 held in reset (CPUCS 1), the image into its RAM a chunk at a
// time, 16 ms apart, then let go (CPUCS 0): the chip starts it and comes back as the firmware's device
bool load_firmware(const std::vector<Chunk> &image) {
    const uint8_t hold = 1, run = 0;
    if (!send_control(0xa0, CPUCS, &hold, 1)) return false;
    for (auto &c : image) {
        if (!send_control(0xa0, c.addr, c.data.data(), c.data.size())) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    return send_control(0xa0, CPUCS, &run, 1);
}

bool wanted(const Device &d) {
    if (g_where != "auto") return d.port == g_where;
    return d.vid == 0x04b4 && d.pid == 0x8613;
}

// The board over USB itself (Linux, and the tests' pretend board)
void run() {
    std::vector<Chunk> image;
    bool told_none = false, told_bare = false;
    int loads = 0;
    while (g_running) {
        std::vector<Device> found;
        for (auto &d : g_usb->scan())
            if (wanted(d)) found.push_back(d);
        if (found.empty()) {
            if (!told_none)
                fprintf(stderr, "popn-io: no board %s; looking every 2 s\n",
                        g_where == "auto" ? "(04b4:8613) plugged in" : ("at USB port " + g_where).c_str());
            told_none = true;
            sleep_ms(2000);
            continue;
        }
        told_none = false;
        if (found.size() > 1) fprintf(stderr, "popn-io: %zu boards; taking the one at USB port %s (--popn-io PORT chooses)\n",
                                      found.size(), found[0].port.c_str());
        const Device d = found[0];

        if (d.bare && !(d.vid == 0x04b4 && d.pid == 0x8613)) {
            fprintf(stderr, "popn-io: the device at USB port %s (%04x:%04x) isn't an EZ-USB FX2LP\n", d.port.c_str(), d.vid, d.pid);
            return;
        }
        if (d.bare) {   // no firmware: the game's, from its ezusb.dll
            if (image.empty() && !g_dll.empty()) {
                std::ifstream f(g_dll, std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                image = find_firmware(bytes);
                if (image.empty()) fprintf(stderr, "popn-io: no firmware found in %s\n", g_dll.c_str());
            }
            if (image.empty()) {
                if (!told_bare)
                    fprintf(stderr, "popn-io: the board at USB port %s has no firmware: --popn-io-dll gives it "
                                    "(the game's ezusb.dll, pop'n music 15 or later)\n", d.port.c_str());
                told_bare = true;
                sleep_ms(5000);
                continue;
            }
            if (++loads > 4) {   // ezusb.dll tries four times
                fprintf(stderr, "popn-io: the board at USB port %s doesn't start the firmware; giving up\n", d.port.c_str());
                return;
            }
            size_t bytes = 0;
            for (auto &c : image) bytes += c.data.size();
            fprintf(stderr, "popn-io: loading the firmware (%zu bytes) into the board at USB port %s\n", bytes, d.port.c_str());
            const bool ok = g_usb->open(d.port) && load_firmware(image);
            g_usb->close();
            if (!ok) {
                fprintf(stderr, "popn-io: the firmware load failed (%s)\n", strerror(errno));
                sleep_ms(2000);
                continue;
            }
            // it leaves and comes back on the same port, with its firmware (ezusb.dll waits 15 s)
            for (int t = 0; t < 80 && g_running; t++) {
                sleep_ms(250);
                bool back = false;
                for (auto &e : g_usb->scan())
                    if (e.port == d.port && !e.bare) back = true;
                if (back) break;
            }
            continue;
        }

        if (!g_usb->open(d.port)) {
            fprintf(stderr, "popn-io: cannot open the board at USB port %s (%s): see docs/PLAY.md for the udev rule\n",
                    d.port.c_str(), strerror(errno));
            sleep_ms(5000);
            continue;
        }
        // ezusb.dll's start: an in, an empty out, two bulk reads and two empty bulk writes; then
        // the coin mode (09 03), which pop'n music sets
        uint8_t in[64] = {}, bulk[512] = {};
        int r = g_usb->transfer(0x81, in, 64, 1000);
        bool ok = r == 64 && send_out(out_packet(0)) == 64;
        if (ok) {
            for (int k = 0; k < 2; k++) g_usb->transfer(0x86, bulk, 512, 200);
            memset(bulk, 0, sizeof bulk);
            for (int k = 0; k < 2; k++)
                if (allowed_bulk(bulk, sizeof bulk)) g_usb->transfer(0x02, bulk, 512, 200);
            ok = send_out(out_packet(lamps_now(), 0x09, 0x03)) == 64;
        }
        if (!ok) {
            fprintf(stderr, "popn-io: the board at USB port %s doesn't answer as pop'n music's I/O board\n", d.port.c_str());
            g_usb->close();
            sleep_ms(5000);
            continue;
        }
        fprintf(stderr, "popn-io: USB port %s (%04x:%04x \"%s\"): the cabinet's buttons and lamps\n", d.port.c_str(), d.vid, d.pid,
                d.name.c_str());
        loads = 0;

        uint32_t sent = ~0u;
        auto last_out = std::chrono::steady_clock::now();
        int failures = 0;
        while (g_running && failures < 50) {
            const auto t0 = std::chrono::steady_clock::now();
            r = g_usb->transfer(0x81, in, 64, 100);
            if (r == Usb::XFER_GONE) break;
            if (r == 64) {
                failures = 0;
                static uint8_t logged[64];
                if (g_log && (memcmp(in, logged, 3) || memcmp(in + 4, logged + 4, 60))) {   // what changed (not byte 3, the count)
                    log_packet("in", in, 64);
                    memcpy(logged, in, 64);
                }
                have(pad(in), coins(in));
            } else {
                failures++;
            }
            const uint32_t lamps = lamps_now();
            const auto now = std::chrono::steady_clock::now();
            if (lamps != sent || now - last_out > std::chrono::milliseconds(500)) {
                r = send_out(out_packet(lamps));
                if (r == Usb::XFER_GONE) break;
                if (r == 64) sent = lamps, last_out = now;
            }
            // at most a poll a millisecond (a high-speed board answers every 125 µs)
            if (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1)) std::this_thread::sleep_until(t0 + std::chrono::milliseconds(1));
        }
        if (g_running) fprintf(stderr, "popn-io: lost the board; looking for it again\n");
        g_usb->close();
        lost();
        sleep_ms(1000);
    }
}

#ifdef _WIN32
// The board through an ezusb.dll (Windows): the 32-bit hiraku-ezusb.exe beside this program loads it
// (ezusb/link.h) and HIRAKU trades the controls and lamps with it through shared memory
void run_helper() {
    if (g_dll.empty()) {
        fprintf(stderr, "popn-io: on Windows the board is driven through an ezusb.dll: --popn-io-dll FILE (the game's, "
                        "or a replacement such as UltimatePopnController's)\n");
        return;
    }
    const std::string name = "Local\\hiraku-ezusb-" + std::to_string(GetCurrentProcessId());
    HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(hk_ezusb_link), name.c_str());
    auto *link = map ? (hk_ezusb_link *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hk_ezusb_link)) : nullptr;
    if (!link) {
        fprintf(stderr, "popn-io: no shared memory (%lu)\n", GetLastError());
        return;
    }
    std::string helper = hk::sys::exe_path();
    helper = helper.substr(0, helper.find_last_of('/') + 1) + "hiraku-ezusb.exe";
    for (char &c : helper)
        if (c == '/') c = '\\';
    fprintf(stderr, "popn-io: %s drives the board through %s\n", helper.c_str(), g_dll.c_str());
    for (int tries = 0; g_running && tries < 3; tries++) {
        memset((void *)link, 0, sizeof *link);
        link->magic = HK_EZUSB_MAGIC;
        std::string cmd = "\"" + helper + "\" " + name + " \"" + g_dll + "\" " + std::to_string(GetCurrentProcessId());
        STARTUPINFOA si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;   // its messages go where HIRAKU's go
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessA(helper.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            fprintf(stderr, "popn-io: cannot start %s (%lu)\n", helper.c_str(), GetLastError());
            break;
        }
        int shown = 99;
        while (g_running) {
            const int state = link->state;
            if (state != shown) {
                shown = state;
                if (state == HK_EZUSB_STARTING) fprintf(stderr, "popn-io: %s: starting the board\n", g_dll.c_str());
                if (state == HK_EZUSB_FIRMWARE) fprintf(stderr, "popn-io: the board takes its firmware (about 20 s)\n");
                if (state == HK_EZUSB_RUNNING) fprintf(stderr, "popn-io: through ezusb.dll: the cabinet's buttons and lamps\n");
            }
            if (state == HK_EZUSB_RUNNING) have(link->pad, link->coins);
            link->lamps = lamps_now();
            if (WaitForSingleObject(pi.hProcess, 1) == WAIT_OBJECT_0) break;   // also the 1 ms between polls
        }
        lost();
        if (!g_running) {
            link->quit = 1;   // it turns the lamps off and lets the board go
            if (WaitForSingleObject(pi.hProcess, 3000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
        }
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (!g_running) break;
        // what went wrong, said here too: the helper's own messages may not reach the game's log
        const int state = link->state;
        const int result = link->result;
        if (state == HK_EZUSB_NO_DLL) fprintf(stderr, "popn-io: the DLL didn't load (Windows error %d)\n", result);
        else if (state == HK_EZUSB_NO_EXPORT) fprintf(stderr, "popn-io: the DLL isn't an ezusb.dll (no usbStart, usbPadRead...)\n");
        else if (state == HK_EZUSB_FAILED) fprintf(stderr, "popn-io: the DLL's call failed (result %d)\n", result);
        else if (state == HK_EZUSB_LOADING) fprintf(stderr, "popn-io: hiraku-ezusb.exe stopped before loading the DLL\n");
        if (state == HK_EZUSB_NO_DLL || state == HK_EZUSB_NO_EXPORT) break;   // trying again won't help
        fprintf(stderr, "popn-io: hiraku-ezusb.exe stopped (exit %lu); starting it again in 5 s\n", code);
        sleep_ms(5000);
    }
    UnmapViewOfFile((void *)link);
    CloseHandle(map);
}
#endif

}  // namespace

bool popnio_option(const std::string &name, const std::function<std::string()> &val) {
    if (name == "--popn-io") g_where = val();
    else if (name == "--popn-io-dll" || name == "--popn-io-firmware") g_dll = val();
    else return false;
    return true;
}

void popnio_default_dll(const std::string &path) {
    if (g_dll.empty()) g_dll = path;
}

std::string popnio_help() {
    return "  --popn-io WHERE      a pop'n music cabinet's USB I/O board (pop'n 15 and later): auto, or (Linux) its\n"
           "                       USB port (3-2.1): the cabinet's buttons and lamps\n"
           "  --popn-io-dll FILE   the game's ezusb.dll: Linux loads a bare board's firmware from it; on Windows it\n"
           "                       drives the board (or a replacement ezusb.dll, such as UltimatePopnController's)\n";
}

void popnio_start() {
    if (g_where.empty()) return;
#ifdef _WIN32
    if (!g_usb) {
        g_running = true;
        g_thread = std::thread(run_helper);
        atexit(popnio_stop);
        return;
    }
#endif
    if (!g_usb) g_usb = system_usb();
    if (!g_usb) {
        fprintf(stderr, "popn-io: not on this system\n");
        return;
    }
    g_running = true;
    g_thread = std::thread(run);
    atexit(popnio_stop);   // before the thread's state goes (the game leaves through exit())
}

void popnio_stop() {
    g_running = false;
    if (g_thread.joinable()) g_thread.join();
}

namespace popnio {
void use_usb(std::unique_ptr<Usb> usb) { g_usb = std::move(usb); }
}  // namespace popnio

void popnio_frame(hk::Core &core) {
    if (!g_running) return;
    // the lamps out
    static std::string family;
    static std::vector<uint32_t> bits;   // each of cab::lamps(family)'s on the board's word
    if (family != core.family()) {
        family = core.family();
        bits.clear();
        for (auto &l : cab::lamps(family)) bits.push_back(lamp_bits(l.name));
    }
    uint32_t lamps = 0;
    const auto &lit = core.lamp_state();
    for (size_t i = 0; i < bits.size() && i < lit.size(); i++)
        if (lit[i]) lamps |= bits[i];
    uint32_t now, count;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        g_lamps = lamps;
        if (!g_have_in) return;
        now = g_pad, count = g_coins;
    }

    // the controls in: what changed since the last frame (all up to begin with)
    static uint32_t last = 0, last_count = 0;
    static bool first = true;
    if (first) last_count = count, first = false;
    for (auto &b : inputs())
        if ((now >> b.bit & 1) != (last >> b.bit & 1)) {
            core.set_input(b.name, now >> b.bit & 1);
            if (now >> b.bit & 1) warp::pressed();   // the fast start-up ends (host/warp.h)
        }
    // coins: the board's count (Konami's board counts them); a board whose count never moves
    // (UltimatePopnController's ezusb.dll) by its coin switch, bit 22, unless the count moves
    // within 15 frames of the switch
    static int pending = 0, coin_frames = 0, switch_wait = 0;
    static bool counting = false;
    if (uint32_t d = (count - last_count) & 0xffff; d && d < 16) pending += (int)d, counting = true, switch_wait = 0;
    last_count = count;
    if (!counting && (now >> 22 & 1) && !(last >> 22 & 1)) switch_wait = 15;
    if (switch_wait && --switch_wait == 0 && !counting) pending++;
    last = now;
    if (coin_frames) {
        if (--coin_frames == 2) core.set_input("Coin 1", 0);
    } else if (pending) {
        pending--;
        core.set_input("Coin 1", 1);
        warp::pressed();
        coin_frames = 4;   // 2 down, 2 up
    }
}

}  // namespace hk::host
