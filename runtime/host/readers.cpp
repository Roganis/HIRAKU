// SPDX-License-Identifier: GPL-3.0-or-later
// The serial reader's dialogue is spice2x's (reader/: reader.cpp, crypt.cpp, GPLv3, the sp2xdev
// team and the SpiceTools contributors), on acio.h's port; the PC/SC and cardio readers follow
// spice2x's external/scard (nolm, MIT) and external/cardio (Felix, MIT) in what they read.
#include "readers.h"

#include "acio.h"

#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#if defined(_WIN32) && defined(HK_WITH_SDL)
#include <SDL3/SDL.h>   // cardio readers through SDL's HID calls
#endif

#ifdef HK_WITH_PCSC
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winscard.h>
#define MAX_ATR_SIZE 33   // pcsc-lite's name for it
#else
#include <PCSC/winscard.h>
#include <PCSC/wintypes.h>
#endif
#endif

namespace hk::host {

namespace {

using Clock = std::chrono::steady_clock;

std::vector<std::string> g_serial;   // --reader paths, player 1's then 2's
bool g_scard = false, g_cardio = false;

std::atomic<bool> g_running{false};
std::vector<std::thread> g_threads;

// What the threads read, for readers_frame
std::mutex g_mutex;
std::vector<std::pair<int, std::string>> g_cards;   // (player, number), in the order read
uint16_t g_keys[2] = {0, 0};                         // the serial readers' 10-keys (the reader's bits)
std::string g_last[2];                               // the card each player last held, for the 3 s
Clock::time_point g_last_seen[2];

void card_read(int player, const uint8_t uid[8]) {
    char hex[17];
    for (int i = 0; i < 8; i++) snprintf(hex + 2 * i, 3, "%02X", uid[i]);
    std::string number = hex;
    if (number == "0000000000000000") return;
    if (number.compare(0, 4, "E004") != 0) number = "E00401" + number.substr(0, 10);   // spice2x's -scardfix
    std::lock_guard<std::mutex> l(g_mutex);
    const auto now = Clock::now();
    if (number == g_last[player] && now - g_last_seen[player] < std::chrono::seconds(3)) {
        g_last_seen[player] = now;   // still held there
        return;
    }
    g_last[player] = number;
    g_last_seen[player] = now;
    g_cards.emplace_back(player, number);
}

void sleep_ms(int ms) {
    for (int t = 0; t < ms && g_running; t += 50) std::this_thread::sleep_for(std::chrono::milliseconds(std::min(50, ms - t)));
}

// ---- the serial reader (ICCA/ICCB/ICCC), after spice2x's reader/ ----

// Readers share a port as nodes of one ACIO bus: a beatmania IIDX cabinet's two (players 1 and 2) are
// nodes 1 and 2 of one serial line. The bus numbers its nodes (command 0001's answer: how many);
// each reader gets its own session (keys) and is polled in turn. The second is player 2's unless a
// --reader of its own is given for player 2.
class SerialReader {
public:
    SerialReader(std::string path, int player, bool second_on_bus) : port_(std::move(path)), player_(player), second_on_bus_(second_on_bus) {}
    void run() {
        bool told = false;
        while (g_running) {
            if (!port_.open()) {
                if (!told) fprintf(stderr, "reader %s: cannot open it (%s); trying again every 5 s\n", port_.path().c_str(), strerror(errno));
                told = true;
            } else if (connect()) {
                told = false;
                while (g_running && poll_card()) {}
                if (g_running) fprintf(stderr, "reader %s: lost; connecting again\n", port_.path().c_str());
            }
            port_.close();
            sleep_ms(5000);
        }
    }

private:
    struct Node {
        uint8_t addr;
        int player;
        ReaderCrypt crypt;
    };

    bool connect() {
        nodes_.clear();
        if (!port_.handshake({57600, 38400, 19200, 9600}, [] { return g_running.load(); })) {
            fprintf(stderr, "reader %s: no answer at 57600, 38400, 19200 or 9600 baud\n", port_.path().c_str());
            return false;
        }
        AcioPort::Message m;
        if (!port_.ask(0, 0x0001, {0x00}, m)) return false;   // number the nodes: how many there are
        const int count = m.payload.empty() || !m.payload[0] ? 1 : m.payload[0];
        for (int addr = 1; addr <= count && addr <= 2; addr++) {
            const int player = addr == 1 ? player_ : player_ == 0 && second_on_bus_ ? 1 : -1;
            if (player < 0) {
                fprintf(stderr, "reader %s: a reader at node %d too, left alone (player 2 has a --reader of its own)\n", port_.path().c_str(), addr);
                continue;
            }
            Node n{(uint8_t)addr, player, {}};
            if (!start(n)) return false;
            nodes_.push_back(n);
        }
        return !nodes_.empty();
    }
    bool start(Node &n) {
        AcioPort::Message m;
        if (!port_.ask(n.addr, 0x0002, {}, m)) return false;   // its version
        if (m.payload.size() >= 44)
            fprintf(stderr, "reader %s: %.4s, %.16s %.16s, player %d's%s\n", port_.path().c_str(), (const char *)&m.payload[8],
                    (const char *)&m.payload[12], (const char *)&m.payload[28], n.player + 1, n.addr > 1 ? " (the bus's second reader)" : "");
        if (!port_.ask(n.addr, 0x0003, {}, m)) return false;       // start
        if (!port_.ask(n.addr, 0x0130, {0x00}, m)) return false;   // reinitialise
        // the session's keys: ours, and the reader's answer
        std::random_device rd;
        uint8_t mine[4];
        for (uint8_t &b : mine) b = (uint8_t)rd();
        if (!port_.ask(n.addr, 0x0160, {mine[0], mine[1], mine[2], mine[3]}, m) || m.payload.size() != 4) return false;
        n.crypt.set_keys((uint32_t)m.payload[0] << 24 | m.payload[1] << 16 | m.payload[2] << 8 | m.payload[3],
                         (uint32_t)mine[0] << 24 | mine[1] << 16 | mine[2] << 8 | mine[3]);
        return true;
    }

    // Read whatever card is there, then the status: 0 2 when there is a card, 2-9 its id, 11 the
    // 10-key's, 14-15 the keys held. spice2x waits 200 ms after the read for one status, so a key
    // is seen every ~220 ms (the 10-key's lag a tester found); here the status is asked every
    // 20 ms through those 200 ms, the keys with it (each reader on the bus in turn)
    bool poll_card() {
        AcioPort::Message m;
        for (Node &n : nodes_)
            if (!port_.ask(n.addr, 0x0161, {0x00, 0x03, 0xff, 0xff}, m)) return false;
        for (int i = 0; i < 10 && g_running; i++) {
            sleep_ms(20);
            for (Node &n : nodes_) {
                if (!port_.ask(n.addr, 0x0164, {0x10}, m)) return false;
                if (m.payload.size() != 18) continue;
                n.crypt.crypt(m.payload.data(), 18);
                if (ReaderCrypt::crc(m.payload.data(), 16) != (m.payload[16] << 8 | m.payload[17])) return false;   // out of step
                const uint8_t *s = m.payload.data();
                if (s[11]) {
                    std::lock_guard<std::mutex> l(g_mutex);
                    g_keys[n.player] = (uint16_t)(s[14] << 8 | s[15]);
                }
                if (s[0] == 2) card_read(n.player, s + 2);
            }
        }
        return true;
    }

    AcioPort port_;
    int player_;
    bool second_on_bus_;   // the bus's second reader is player 2's (no --reader of its own)
    std::vector<Node> nodes_;
};

#ifdef HK_WITH_PCSC
// ---- NFC readers through PC/SC ----

void scard_read(SCARDCONTEXT ctx, const char *name, int player) {
    SCARDHANDLE card;
    DWORD protocol;
    if (SCardConnect(ctx, name, SCARD_SHARE_SHARED, SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1, &card, &protocol) != SCARD_S_SUCCESS) return;
    const SCARD_IO_REQUEST *pci = protocol == SCARD_PROTOCOL_T1 ? SCARD_PCI_T1 : SCARD_PCI_T0;
    BYTE out[258];
    DWORD n = sizeof out;
    // the ACR122U's polling for every kind of card (others refuse it, which is no matter)
    static const BYTE picc[] = {0xff, 0x00, 0x51, 0xdf, 0x00};
    SCardTransmit(card, pci, picc, sizeof picc, nullptr, out, &n);
    // the ATR says which kind: an ISO 15693 card (an e-AMUSEMENT PASS) gives its id backwards
    BYTE atr[MAX_ATR_SIZE];
    DWORD atr_len = sizeof atr, state, plen;
    char rname[256];
    DWORD rlen = sizeof rname;
    bool reverse = false;
    if (SCardStatus(card, rname, &rlen, &state, &plen, atr, &atr_len) == SCARD_S_SUCCESS && atr_len == 20)
        reverse = atr[12] == 0x0b || atr[12] == 0x0c;
    static const BYTE get_uid[] = {0xff, 0xca, 0x00, 0x00, 0x00};
    n = sizeof out;
    LONG r = SCardTransmit(card, pci, get_uid, sizeof get_uid, nullptr, out, &n);
    SCardDisconnect(card, SCARD_LEAVE_CARD);
    if (r != SCARD_S_SUCCESS || n < 3 || out[n - 2] != 0x90 || out[n - 1] != 0x00) return;
    n -= 2;
    uint8_t uid[8] = {};
    for (DWORD i = 0; i < 8 && i < n; i++) uid[i] = reverse && n >= 8 ? out[7 - i] : out[i];
    card_read(player, uid);
}

void scard_run() {
    bool told = false;
    while (g_running) {
        SCARDCONTEXT ctx;
        if (SCardEstablishContext(SCARD_SCOPE_SYSTEM, nullptr, nullptr, &ctx) != SCARD_S_SUCCESS) {
            if (!told) fprintf(stderr, "scard: PC/SC isn't running (systemctl enable --now pcscd.socket); trying again every 5 s\n");
            told = true;
            sleep_ms(5000);
            continue;
        }
        told = false;
        std::vector<std::string> names;
        DWORD len = 0;
        if (SCardListReaders(ctx, nullptr, nullptr, &len) == SCARD_S_SUCCESS && len > 1) {
            std::vector<char> buf(len);
            if (SCardListReaders(ctx, nullptr, buf.data(), &len) == SCARD_S_SUCCESS)
                for (const char *p = buf.data(); *p; p += strlen(p) + 1) names.emplace_back(p);
        }
        if (names.size() > 2) names.resize(2);
        static size_t shown = (size_t)-1;
        if (names.size() != shown)
            for (size_t i = 0; i < names.size(); i++) fprintf(stderr, "scard: %s, player %zu's\n", names[i].c_str(), i + 1);
        shown = names.size();
        // wait for a card, or for a reader to come or go (the PnP name), a few seconds at a time
        std::vector<SCARD_READERSTATE> st(names.size() + 1);
        for (size_t i = 0; i < names.size(); i++) st[i] = {names[i].c_str(), nullptr, SCARD_STATE_UNAWARE, 0, 0, {}};
        st.back() = {"\\\\?PnP?\\Notification", nullptr, (DWORD)(names.size() << 16), 0, 0, {}};
        while (g_running) {
            LONG r = SCardGetStatusChange(ctx, 500, st.data(), (DWORD)st.size());
            if (r == (LONG)SCARD_E_TIMEOUT) continue;
            if (r != SCARD_S_SUCCESS) break;
            if (st.back().dwEventState & SCARD_STATE_CHANGED) break;   // readers changed: list them again
            for (size_t i = 0; i < names.size(); i++) {
                if (!(st[i].dwEventState & SCARD_STATE_CHANGED)) continue;
                const bool was = st[i].dwCurrentState & SCARD_STATE_PRESENT;
                st[i].dwCurrentState = st[i].dwEventState & ~SCARD_STATE_CHANGED;
                if ((st[i].dwEventState & SCARD_STATE_PRESENT) && !was) scard_read(ctx, names[i].c_str(), (int)i);
            }
        }
        SCardReleaseContext(ctx);
        sleep_ms(500);
    }
}
#endif

// ---- cardio readers through hidraw (Linux) or SDL's HID calls (Windows) ----

// A HID report descriptor's items: whether it has usage page 0xffca (cardio), and report ids
[[maybe_unused]] bool cardio_descriptor(const std::vector<uint8_t> &d, bool &ids) {
    bool cardio = false;
    ids = false;
    for (size_t i = 0; i < d.size();) {
        uint8_t item = d[i];
        if (item == 0xfe) {   // a long item
            if (i + 1 >= d.size()) break;
            i += 3 + d[i + 1];
            continue;
        }
        size_t n = item & 3;
        if (n == 3) n = 4;
        uint32_t v = 0;
        for (size_t k = 0; k < n && i + 1 + k < d.size(); k++) v |= (uint32_t)d[i + 1 + k] << (8 * k);
        if ((item & 0xfc) == 0x04 && v == 0xffca) cardio = true;   // Usage Page
        if ((item & 0xfc) == 0x84) ids = true;                     // Report ID
        i += 1 + n;
    }
    return cardio;
}

#ifndef _WIN32
void cardio_run() {
    struct Device {
        std::string path;
        int fd;
        bool ids;
    };
    std::vector<Device> devs;
    bool told = false;
    auto scan = [&] {
        std::vector<std::string> found;
        if (DIR *dir = opendir("/sys/class/hidraw")) {
            while (dirent *e = readdir(dir))
                if (strncmp(e->d_name, "hidraw", 6) == 0) found.emplace_back(e->d_name);
            closedir(dir);
        }
        std::sort(found.begin(), found.end(), [](const std::string &a, const std::string &b) {
            return atoi(a.c_str() + 6) < atoi(b.c_str() + 6);
        });
        for (const std::string &h : found) {
            if (devs.size() == 2) break;
            const std::string path = "/dev/" + h;
            if (std::any_of(devs.begin(), devs.end(), [&](const Device &d) { return d.path == path; })) continue;
            std::ifstream f("/sys/class/hidraw/" + h + "/device/report_descriptor", std::ios::binary);
            std::vector<uint8_t> desc((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            bool ids;
            if (!cardio_descriptor(desc, ids)) continue;
            int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
            if (fd < 0) {
                if (!told)
                    fprintf(stderr, "cardio: %s is a cardio reader, but cannot be opened (%s): a udev rule can let you "
                                    "(docs/PLAY.md, Card readers)\n", path.c_str(), strerror(errno));
                told = true;
                continue;
            }
            fprintf(stderr, "cardio: %s, player %zu's\n", path.c_str(), devs.size() + 1);
            devs.push_back({path, fd, ids});
        }
    };
    auto last_scan = Clock::now() - std::chrono::seconds(10);
    while (g_running) {
        if (devs.size() < 2 && Clock::now() - last_scan > std::chrono::seconds(5)) {
            scan();
            last_scan = Clock::now();
        }
        if (devs.empty()) {
            sleep_ms(500);
            continue;
        }
        std::vector<pollfd> p;
        for (auto &d : devs) p.push_back({d.fd, POLLIN, 0});
        if (poll(p.data(), p.size(), 200) <= 0) continue;
        for (size_t i = 0; i < devs.size(); i++) {
            if (p[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {   // unplugged
                close(devs[i].fd);
                devs[i].fd = -1;
                continue;
            }
            if (!(p[i].revents & POLLIN)) continue;
            uint8_t r[64];
            ssize_t n = read(devs[i].fd, r, sizeof r);
            // a report id (1 ISO 15693, 2 FeliCa) and the card's 8 bytes
            const size_t at = devs[i].ids ? 1 : 0;
            if (n >= (ssize_t)(at + 8)) card_read((int)i, r + at);
        }
        devs.erase(std::remove_if(devs.begin(), devs.end(), [](const Device &d) { return d.fd < 0; }), devs.end());
    }
    for (auto &d : devs) close(d.fd);
}
#elif defined(HK_WITH_SDL)
// Windows: through SDL's HID calls (hidapi), which give the reports as hidraw does (the report id
// first when the reader numbers its reports)
void cardio_run() {
    struct Device {
        std::string path;
        SDL_hid_device *dev;
        int ids;   // 1 numbered reports, 0 not, -1 unknown (no descriptor: told by the report)
    };
    std::vector<Device> devs;
    SDL_SetHint(SDL_HINT_HIDAPI_ENUMERATE_ONLY_CONTROLLERS, "0");   // card readers are not game controllers
    if (SDL_hid_init() != 0) {
        fprintf(stderr, "cardio: no HID access (%s)\n", SDL_GetError());
        return;
    }
    bool told = false;
    auto scan = [&] {
        SDL_hid_device_info *list = SDL_hid_enumerate(0, 0);
        for (auto *e = list; e && devs.size() < 2; e = e->next) {
            if (e->usage_page != 0xffca || !e->path) continue;
            const std::string path = e->path;
            if (std::any_of(devs.begin(), devs.end(), [&](const Device &d) { return d.path == path; })) continue;
            SDL_hid_device *dev = SDL_hid_open_path(e->path);
            if (!dev) {
                if (!told) fprintf(stderr, "cardio: a cardio reader cannot be opened (%s)\n", SDL_GetError());
                told = true;
                continue;
            }
            unsigned char d[4096];
            const int n = SDL_hid_get_report_descriptor(dev, d, sizeof d);
            bool ids = false;
            const int known = n > 0 && cardio_descriptor(std::vector<uint8_t>(d, d + n), ids) ? (ids ? 1 : 0) : -1;
            fprintf(stderr, "cardio: %04x:%04x, player %zu's\n", e->vendor_id, e->product_id, devs.size() + 1);
            devs.push_back({path, dev, known});
        }
        SDL_hid_free_enumeration(list);
    };
    auto last_scan = Clock::now() - std::chrono::seconds(10);
    while (g_running) {
        if (devs.size() < 2 && Clock::now() - last_scan > std::chrono::seconds(5)) {
            scan();
            last_scan = Clock::now();
        }
        if (devs.empty()) {
            sleep_ms(500);
            continue;
        }
        for (size_t i = 0; i < devs.size(); i++) {
            uint8_t r[64];
            const int n = SDL_hid_read_timeout(devs[i].dev, r, sizeof r, devs.size() == 1 ? 200 : 100);
            if (n < 0) {   // unplugged
                SDL_hid_close(devs[i].dev);
                devs[i].dev = nullptr;
                continue;
            }
            // a report id (1 ISO 15693, 2 FeliCa) and the card's 8 bytes
            const bool numbered = devs[i].ids == 1 || (devs[i].ids < 0 && n == 9 && (r[0] == 1 || r[0] == 2));
            const int at = numbered ? 1 : 0;
            if (n >= at + 8) card_read((int)i, r + at);
        }
        devs.erase(std::remove_if(devs.begin(), devs.end(), [](const Device &d) { return !d.dev; }), devs.end());
    }
    for (auto &d : devs) SDL_hid_close(d.dev);
}
#else
void cardio_run() {
    fprintf(stderr, "cardio: this build has no HID access (built without SDL)\n");
}
#endif

// The reader's 10-key bits, as a real reader's encrypted status gives them (bytes 14-15): one bit
// a key in the order 0 1 4 7 00 2 5 8 . 3 6 9 (spice2x's reader/reader.cpp hands them on as its
// eam_io scan codes, misc/eamuse.h). Not the layout a game's reader protocol uses (spice2x's
// acioemu/icca.cpp, pop'n 14's python/icca.h), which this table had first: a real reader's 1 and
// 3, 7 and 9 came out swapped, its 0 as the key beside it and 2, 5, 8, 00 not at all.
struct Key {
    uint16_t bit;
    const char *name, *instead;   // the game's input, and one to try when the game hasn't that
};
constexpr Key KEYS[] = {{0x0001, "Keypad 0", nullptr}, {0x0002, "Keypad 1", nullptr}, {0x0020, "Keypad 2", nullptr},
                        {0x0200, "Keypad 3", nullptr}, {0x0004, "Keypad 4", nullptr}, {0x0040, "Keypad 5", nullptr},
                        {0x0400, "Keypad 6", nullptr}, {0x0008, "Keypad 7", nullptr}, {0x0080, "Keypad 8", nullptr},
                        {0x0800, "Keypad 9", nullptr}, {0x0100, "Keypad .", nullptr},
                        {0x0010, "Keypad 00", "Keypad Enter"}};
uint16_t g_applied[2] = {0, 0};
const char *g_used[2][std::size(KEYS)];   // the input each held key went to

}  // namespace

void ReaderCrypt::set_keys(uint32_t reader_key, uint32_t game_key) {
    keys[0] = reader_key ^ 0x05491333;
    keys[1] = game_key ^ 0x1f123bb5;
    keys[2] = reader_key ^ 0x159a55e5;
    keys[3] = game_key ^ 0x075bcd15;
}

void ReaderCrypt::crypt(uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        const int low = (int)(i & 3);
        if (low == 0) {
            uint32_t k = (keys[3] << 11) ^ keys[3];
            keys[3] = keys[2];
            keys[2] = keys[1];
            keys[1] = keys[0];
            keys[0] = (((keys[0] >> 11) ^ k) >> 8) ^ k ^ keys[0];
        }
        data[i] ^= (uint8_t)(keys[0] >> ((3 - low) << 3));
    }
}

uint16_t ReaderCrypt::crc(const uint8_t *data, size_t len) {
    uint16_t crc = 0;   // CRC-16, polynomial 0x1021, from 0
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)(data[i] << 8);
        for (int b = 0; b < 8; b++) crc = crc & 0x8000 ? (uint16_t)(crc << 1 ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

bool readers_option(const std::string &name0, const std::function<std::string()> &val, std::string *error) {
    const std::string name = name0.compare(0, 2, "--") == 0 ? name0.substr(1) : name0;   // -reader as spice2x, or --reader
    if (name == "-reader") {
        if (g_serial.size() == 2) {
            *error = "--reader: two readers at most, player 1's and player 2's";
            return true;
        }
        g_serial.push_back(val());
        return true;
    }
    if (name == "-scard") return g_scard = true;
    if (name == "-cardio") return g_cardio = true;
    return false;
}

std::string readers_help() {
    return "  --reader PATH        a Konami e-AMUSEMENT card reader on a serial port (/dev/ttyUSB0); twice: player 2's\n"
           "                       (a second reader on the same port, as a cabinet's two: player 2's)\n"
           "  --scard              NFC card readers through PC/SC (pcscd)\n"
           "  --cardio             cardio card readers (USB HID)\n";
}

void readers_start() {
    if (std::all_of(g_serial.begin(), g_serial.end(), [](const std::string &p) { return p.empty(); }) && !g_scard && !g_cardio) return;
    g_running = true;
    atexit(readers_stop);   // before the threads' state goes (the game leaves through exit())
    // the same port twice: one bus, its second reader player 2's (a cabinet's two readers on one line)
    if (g_serial.size() == 2 && g_serial[0] == g_serial[1]) g_serial[1].clear();
    const bool second_on_bus = g_serial.size() < 2 || g_serial[1].empty();
    for (size_t i = 0; i < g_serial.size(); i++)
        if (!g_serial[i].empty()) g_threads.emplace_back([path = g_serial[i], i, second_on_bus] { SerialReader(path, (int)i, second_on_bus).run(); });
#ifdef HK_WITH_PCSC
    if (g_scard) g_threads.emplace_back(scard_run);
#else
    if (g_scard) fprintf(stderr, "scard: this build has no PC/SC (libpcsclite)\n");
#endif
    if (g_cardio) g_threads.emplace_back(cardio_run);
}

void readers_frame(const std::function<bool(int, const std::string &)> &card,
                   const std::function<bool(const std::string &, int)> &input) {
    if (!g_running) return;
    std::vector<std::pair<int, std::string>> cards;
    uint16_t keys[2];
    {
        std::lock_guard<std::mutex> l(g_mutex);
        cards.swap(g_cards);
        keys[0] = g_keys[0];
        keys[1] = g_keys[1];
    }
    for (auto &[player, number] : cards)
        if (card(player, number)) printf("card: %s held to player %d's reader\n", number.c_str(), player + 1);
        else printf("card: %s held to player %d's reader, but this game has no card reader there\n", number.c_str(), player + 1);
    for (int p = 0; p < 2; p++) {
        const uint16_t changed = keys[p] ^ g_applied[p];
        const std::string prefix = p ? "P2 " : "";
        for (size_t k = 0; k < std::size(KEYS); k++) {
            if (!(changed & KEYS[k].bit)) continue;
            if (keys[p] & KEYS[k].bit) {
                g_used[p][k] = input(prefix + KEYS[k].name, 1)                                   ? KEYS[k].name
                               : KEYS[k].instead && input(prefix + KEYS[k].instead, 1) ? KEYS[k].instead
                                                                                       : nullptr;
            } else if (g_used[p][k]) {
                input(prefix + g_used[p][k], 0);
                g_used[p][k] = nullptr;
            }
        }
        g_applied[p] = keys[p];
    }
}

void readers_stop() {
    g_running = false;
    for (auto &t : g_threads) t.join();
    g_threads.clear();
}

}  // namespace hk::host
