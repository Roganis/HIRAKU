// SPDX-License-Identifier: GPL-3.0-or-later
// Real card readers (runtime/host/readers.h): the serial reader's dialogue against a pretend reader
// on a pseudo-terminal, which answers as spice2x's own reader emulation describes it: it numbers
// itself, says it is an ICCA, exchanges keys, and then gives its status encrypted. A card held there
// a while is read once; the 10-key's keys go to the game, the 00 key as Enter where it has no 00.
// `test_readers 2`: two readers on one port (a beatmania IIDX cabinet's, nodes 1 and 2 of one bus),
// each its own keys and card: the second's card is player 2's.
// `test_readers hold [nokeys]`: the reader's 10-key holds 5, 7 and 3 at known times (let go at 1 s,
// a reader that goes quiet with them down, answers that come late, then a whole 2.6 s), and the way
// out to the game menu (host/menukey.h) and the game's inputs are watched as readers_frame gives
// them, once a frame; `nokeys`: a game with no 10-key of its own (it refuses every key).
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "host/menukey.h"
#include "host/readers.h"

using hk::host::ReaderCrypt;

static int failures = 0;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                \
        }                                                              \
    } while (0)

// `hold`: when the pretend reader was first asked its status (menukey::now()'s clock)
static std::atomic<double> g_t0{0};
// What its 10-key does then, by seconds since: 5, 7 and 3 down from 0.5 to 1.5, from 2.0 on with
// no answer at all from 2.6 to 3.4 (up again when it answers), and from 6.0 to 8.6; from 1.6 to 1.9
// one status answer in five comes 150 ms late (past readers.cpp's 100 ms wait for it)
static bool hold_keys(double t) { return (t >= 0.5 && t < 1.5) || (t >= 2.0 && t < 3.4) || (t >= 6.0 && t < 8.6); }
static bool hold_silent(double t) { return t >= 2.6 && t < 3.4; }

// The pretend reader: the far end of the pseudo-terminal
static void pretend_reader(int fd, std::atomic<bool> &stop, int nodes, bool hold) {
    ReaderCrypt crypts[3];
    int polls_of[3] = {0, 0, 0};
    std::vector<uint8_t> frame;
    size_t want = 0;
    bool escape = false, framed = false;
    auto reply = [&](const std::vector<uint8_t> &head, const std::vector<uint8_t> &payload) {
        std::vector<uint8_t> body = head;
        body.push_back((uint8_t)payload.size());
        body.insert(body.end(), payload.begin(), payload.end());
        uint8_t sum = 0;
        for (uint8_t b : body) sum += b;
        body.push_back(sum);
        std::vector<uint8_t> out = {0xaa};
        for (uint8_t b : body)
            if (b == 0xaa || b == 0xff) out.push_back(0xff), out.push_back((uint8_t)~b);
            else out.push_back(b);
        if (write(fd, out.data(), out.size()) != (ssize_t)out.size()) stop = true;
    };
    while (!stop) {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 50) <= 0) continue;
        uint8_t in[1024];
        ssize_t n = read(fd, in, sizeof in);
        if (n <= 0) continue;
        bool hello = false;
        for (ssize_t i = 0; i < n; i++) {
            uint8_t b = in[i];
            if (b == 0xaa && !escape) {
                hello = !framed;   // before the first message, a run of 0xaa is the handshake
                frame.clear();
                want = 6;
                continue;
            }
            if (!want) continue;
            if (b == 0xff && !escape) { escape = true; continue; }
            if (escape) b = (uint8_t)~b;
            escape = false;
            frame.push_back(b);
            if (frame.size() == 5) want = 6 + frame[4];
            if (frame.size() < want) continue;
            want = 0;
            framed = true;
            hello = false;
            const uint8_t node = frame[0], hi = frame[1], lo = frame[2], id = frame[3];
            const std::vector<uint8_t> req(frame.begin() + 5, frame.end() - 1);
            const std::vector<uint8_t> head = {uint8_t(node ? node | 0x80 : 0), hi, lo, id};
            switch (lo) {
            case 0x01: reply(head, {(uint8_t)nodes}); break;   // how many nodes
            case 0x02: {
                std::vector<uint8_t> v(44, 0);
                v[0] = 3;
                v[5] = 1, v[6] = 7;
                memcpy(&v[8], "ICCA", 4);
                memcpy(&v[12], "Oct 16 2026", 11);
                memcpy(&v[28], "12:00:00", 8);
                reply(head, v);
                break;
            }
            case 0x60: {
                const uint32_t mine = 0x12345678;
                crypts[node < 3 ? node : 0].set_keys(mine, (uint32_t)req[0] << 24 | req[1] << 16 | req[2] << 8 | req[3]);
                reply(head, {0x12, 0x34, 0x56, 0x78});
                break;
            }
            case 0x64: {
                // polls 3-8: a card; 10-11: the 1 key; 13: the 00 key (node 2: its own card, no keys)
                const int polls = ++polls_of[node < 3 ? node : 0];
                ReaderCrypt &crypt = crypts[node < 3 ? node : 0];
                uint8_t s[18] = {};
                s[0] = 4;
                double t = 0;
                if (hold) {
                    if (g_t0 == 0) g_t0 = hk::host::menukey::now();
                    t = hk::host::menukey::now() - g_t0;
                    if (hold_silent(t)) break;
                    if (t >= 1.6 && t < 1.9 && polls % 5 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(150));
                }
                if (!hold && polls >= 3 && polls <= 8) {
                    static const uint8_t uid[8] = {0xe0, 0x04, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
                    static const uint8_t uid2[8] = {0xe0, 0x04, 0x01, 0x02, 0x03, 0x04, 0x05, 0x07};
                    s[0] = 2;
                    memcpy(s + 2, node == 2 ? uid2 : uid, 8);
                }
                s[11] = 1;
                uint16_t keys = node == 2 ? 0 : polls == 10 || polls == 11 ? 0x0002 : polls == 13 ? 0x0010 : 0;   // a real reader's bits for 1 and 00
                if (hold) keys = hold_keys(t) ? 0x0248 : 0;   // 5, 7 and 3
                s[14] = keys >> 8, s[15] = keys & 0xff;
                const uint16_t crc = ReaderCrypt::crc(s, 16);
                s[16] = crc >> 8, s[17] = crc & 0xff;
                crypt.crypt(s, 18);
                reply(head, std::vector<uint8_t>(s, s + 18));
                break;
            }
            default: reply(head, {0}); break;   // start, reinitialise, read the card
            }
        }
        if (hello) {
            const uint8_t aa = 0xaa;
            if (write(fd, &aa, 1) != 1) stop = true;
        }
    }
}

static int hold_test(bool game_has_keys);

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "hold") == 0) return hold_test(!(argc > 2 && strcmp(argv[2], "nokeys") == 0));
    const int nodes = argc > 1 ? atoi(argv[1]) : 1;
    // CRC-16 (0x1021, from 0) as spice2x's table has it: one byte's CRC is its entry
    const uint8_t one = 0x01, ten = 0x10, ff = 0xff;
    CHECK(ReaderCrypt::crc(&one, 1) == 0x1021);
    CHECK(ReaderCrypt::crc(&ten, 1) == 0x1231);
    CHECK(ReaderCrypt::crc(&ff, 1) == 0x1ef0);
    // the stream undoes itself with the same keys
    uint8_t block[18], copy[18];
    for (int i = 0; i < 18; i++) block[i] = copy[i] = (uint8_t)(i * 37);
    ReaderCrypt a, b;
    a.set_keys(0xdeadbeef, 0x01020304);
    b.set_keys(0xdeadbeef, 0x01020304);
    a.crypt(block, 18);
    CHECK(memcmp(block, copy, 18) != 0);
    b.crypt(block, 18);
    CHECK(memcmp(block, copy, 18) == 0);

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) || unlockpt(master)) {
        perror("posix_openpt");
        return 1;
    }
    const std::string slave = ptsname(master);
    std::atomic<bool> stop{false};
    std::thread fake(pretend_reader, master, std::ref(stop), nodes, false);

    std::string error;
    CHECK(hk::host::readers_option("--reader", [&] { return slave; }, &error) && error.empty());
    hk::host::readers_start();
    std::vector<std::string> cards, inputs;
    auto card = [&](int player, const std::string &n) {
        cards.push_back(std::to_string(player) + " " + n);
        return true;
    };
    auto input = [&](const std::string &name, int v) {
        if (name == "Keypad 00") return false;   // a game without the 00 key (pop'n 9-13, the 573)
        inputs.push_back(name + "=" + std::to_string(v));
        return true;
    };
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < until && inputs.size() < 4) {
        hk::host::readers_frame(card, input);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    hk::host::readers_stop();
    stop = true;
    fake.join();
    close(master);

    if (nodes == 2) {   // both readers' cards, each its player's (in either order)
        CHECK(cards.size() == 2);
        CHECK(std::count(cards.begin(), cards.end(), "0 E004010203040506") == 1);
        CHECK(std::count(cards.begin(), cards.end(), "1 E004010203040507") == 1);
    } else {
        CHECK(cards.size() == 1);
        CHECK(!cards.empty() && cards[0] == "0 E004010203040506");
    }
    const std::vector<std::string> want = {"Keypad 1=1", "Keypad 1=0", "Keypad Enter=1", "Keypad Enter=0"};
    CHECK(inputs == want);
    for (auto &s : cards) printf("card %s\n", s.c_str());
    for (auto &s : inputs) printf("input %s\n", s.c_str());
    if (failures) fprintf(stderr, "%d failed\n", failures);
    else printf("readers: all passed\n");
    return failures ? 1 : 0;
}

// The 10-key held at known times (hold_keys above): what the game and the way out to the game
// menu are given, frame by frame, and when
static int hold_test(bool game_has_keys) {
    namespace mk = hk::host::menukey;
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) || unlockpt(master)) {
        perror("posix_openpt");
        return 1;
    }
    const std::string slave = ptsname(master);
    std::atomic<bool> stop{false};
    std::thread fake(pretend_reader, master, std::ref(stop), 1, true);
    std::string error;
    CHECK(hk::host::readers_option("--reader", [&] { return slave; }, &error) && error.empty());
    mk::set(true, 2.0);
    hk::host::readers_start();
    struct Seen {
        std::string what;
        double t;
    };
    std::vector<Seen> inputs;
    double fired = -1;
    auto rel = [] { return g_t0 == 0 ? -1.0 : mk::now() - g_t0; };
    auto card = [](int, const std::string &) { return true; };
    auto input = [&](const std::string &name, int v) {
        if (!game_has_keys) return false;
        inputs.push_back({name + "=" + std::to_string(v), rel()});
        return true;
    };
    const double until = mk::now() + 15;
    while (mk::now() < until && fired < 0 && (g_t0 == 0 || rel() < 9.5)) {
        hk::host::readers_frame(card, input);
        if (mk::frame(mk::now())) fired = rel();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    const std::string report = hk::host::readers_report(), hold = mk::report();
    hk::host::readers_stop();
    stop = true;
    fake.join();
    close(master);

    for (auto &s : inputs) printf("%.3f %s\n", s.t, s.what.c_str());
    printf("%s%s\ncompleted at %.3f\n", report.c_str(), hold.c_str(), fired);
    // completed once, 2 s after the last press (6.0); not by the press let go at 1.5, nor by the one
    // whose reader went quiet with the keys down
    CHECK(fired > 7.95 && fired < 8.25);
    CHECK(hold.find("3 holds begun, 2 let go") != std::string::npos);
    CHECK(hold.find("1 of them by a reader gone quiet") != std::string::npos);
    CHECK(hold.find("1 completed") != std::string::npos);
    // read often (a status each 16 ms: 40 and more a second, late answers and the quiet stretch's
    // unanswered ones counted in), the late answers taken in turn: never out of step, never lost
    const char *at = strstr(report.c_str(), " times, ");
    CHECK(at && atof(at + 8) > 40);
    CHECK(report.find(" 0 out of step") != std::string::npos);
    CHECK(report.find("lost 0 times") != std::string::npos);
    CHECK(report.find(" 0 unanswered") == std::string::npos);
    if (game_has_keys) {
        // the game: each key down within 60 ms of the reader's, up likewise, and up STALE_SECONDS
        // (and a frame or so) after the reader went quiet
        auto when = [&](const std::string &what, int nth) {
            for (auto &s : inputs)
                if (s.what == what && nth-- == 0) return s.t;
            return -1.0;
        };
        const double d1 = when("Keypad 5=1", 0), u1 = when("Keypad 5=0", 0), d2 = when("Keypad 5=1", 1),
                     u2 = when("Keypad 5=0", 1), d3 = when("Keypad 5=1", 2);
        printf("Keypad 5 down %.3f up %.3f, down %.3f up %.3f, down %.3f\n", d1, u1, d2, u2, d3);
        CHECK(d1 >= 0.5 && d1 < 0.56);
        CHECK(u1 >= 1.5 && u1 < 1.56);
        CHECK(d2 >= 2.0 && d2 < 2.06);
        CHECK(u2 >= 2.6 + mk::STALE_SECONDS && u2 < 2.6 + mk::STALE_SECONDS + 0.08);
        CHECK(d3 >= 6.0 && d3 < 6.06);
        CHECK(std::count_if(inputs.begin(), inputs.end(), [](const Seen &s) { return s.what == "Keypad 7=1"; }) == 3);
    } else {
        CHECK(inputs.empty());
    }
    if (failures) fprintf(stderr, "%d failed\n", failures);
    else printf("readers (hold%s): all passed\n", game_has_keys ? "" : ", a game with no 10-key");
    return failures ? 1 : 0;
}
