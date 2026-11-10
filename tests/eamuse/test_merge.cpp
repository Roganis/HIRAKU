// SPDX-License-Identifier: GPL-3.0-or-later
// One game's e-AMUSEMENT folder moved into the shared one (runtime/eamuse/merge.h): two folders'
// players have the same ref ids without being the same players, and each card must keep its own.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "eamuse/merge.h"

namespace fs = std::filesystem;
static int failed = 0;

static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
    failed += !ok;
}

static void put(const fs::path &p, const std::string &d) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << d;
}

static std::string get(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static std::string ref(int n) {
    char s[17];
    snprintf(s, sizeof s, "48524B%010X", n);
    return s;
}

static std::string card(const char *id, int n, const char *pass = "") {
    return std::string("cardid=") + id + "\tcardtype=1" + (*pass ? std::string("\tpass=") + pass : "") + "\trefid=" + ref(n) + "\n";
}

int main() {
    const fs::path top = fs::temp_directory_path() / ("hk-merge-" + std::to_string((unsigned long long)std::hash<std::string>{}(fs::current_path().string()) ^ (unsigned long long)time(nullptr)));
    fs::remove_all(top);
    const char *A = "E00401000000000A", *B = "E00401000000000B", *C = "E00401000000000C", *D = "E00401000000000D";

    // 1. the first folder into a shared folder that isn't there yet: as it is
    {
        const fs::path from = top / "one" / "popn13" / "eamuse", to = top / "one" / "eamuse";
        put(from / "cards", card(A, 1, "1111") + card(B, 2));
        put(from / "refid", "3\n");
        put(from / ("pdata-E13-" + ref(1)), "A13");
        put(from / ("pdata-E13-" + ref(2)), "B13");
        hk::ea::merge_folder(from.string(), to.string());
        check(get(to / "cards") == card(A, 1, "1111") + card(B, 2) && get(to / "refid") == "3\n", "a first folder: its cards and its count as they were");
        check(get(to / ("pdata-E13-" + ref(1))) == "A13" && get(to / ("pdata-E13-" + ref(2))) == "B13", "... and its players");
        check(!fs::exists(from) && fs::exists(from.string() + ".moved"), "... the folder kept, renamed");
    }

    // 2. a second game's folder, where the same cards were read in another order, with a card the
    //    shared folder doesn't know and one of its own
    {
        const fs::path from = top / "two" / "popn14" / "eamuse", to = top / "two" / "eamuse";
        put(to / "cards", card(A, 1, "1111") + card(B, 2) + card(C, 3));
        put(to / "refid", "4\n");
        put(to / "numbering", "7\n");
        put(to / "users", "cards=" + std::string(A) + "\tpass=pw\tuser=alice\n");
        put(to / ("pdata-E13-" + ref(1)), "A13");
        put(to / ("pdata-E13-" + ref(2)), "B13");
        put(to / ("pdata-E13-" + ref(3)), "C13");
        // pop'n 14's: B was read first there, then A, then D
        put(from / "cards", card(B, 1, "2222") + card(A, 2) + card(D, 3, "4444"));
        put(from / "refid", "4\n");
        put(from / "numbering", "12\n");
        put(from / "users", "cards=" + std::string(A) + "\tpass=pw\tuser=alice\ncards=" + D + "\tpass=x\tuser=ali\n");
        put(from / ("pdata-F14-" + ref(1)), "B14");
        put(from / ("pdata-F14-" + ref(2)), "A14");
        put(from / ("pdata-F14-" + ref(3)), "D14");
        put(from / ("pscore-F14-" + ref(3)), "D14 scores");
        put(from / "F14-rank-exp", "course=0\tscore=100\twho=" + ref(1) + "\ncourse=0\tscore=90\twho=" + ref(3) + "\n");
        put(from / (std::string("userdata-D11-") + D), "by card");
        hk::ea::merge_folder(from.string(), to.string());
        check(get(to / "cards") == card(A, 1, "1111") + card(B, 2) + card(C, 3) + card(D, 4, "4444"),
              "the cards: each once, the shared folder's players kept, the new card with a number nobody has");
        check(get(to / "refid") == "5\n" && get(to / "numbering") == "12\n", "the counts: after every card of both");
        check(get(to / ("pdata-F14-" + ref(1))) == "A14" && get(to / ("pdata-F14-" + ref(2))) == "B14",
              "a card both knew: its player of the other game under the shared folder's number (not the other card's)");
        check(get(to / ("pdata-F14-" + ref(4))) == "D14" && get(to / ("pscore-F14-" + ref(4))) == "D14 scores" && !fs::exists(to / ("pdata-F14-" + ref(3))),
              "the new card: its player and scores under its new number, none under another card's");
        check(get(to / ("pdata-E13-" + ref(1))) == "A13" && get(to / ("pdata-E13-" + ref(2))) == "B13" && get(to / ("pdata-E13-" + ref(3))) == "C13",
              "the shared folder's own players untouched");
        check(get(to / "F14-rank-exp") == "course=0\tscore=100\twho=" + ref(2) + "\ncourse=0\tscore=90\twho=" + ref(4) + "\n",
              "the ranking's entries follow their players");
        check(get(to / (std::string("userdata-D11-") + D)) == "by card", "a file kept by card number moves as it is");
        check(get(to / "users") == "cards=" + std::string(A) + "\tpass=pw\tuser=alice\ncards=" + D + "\tpass=x\tuser=ali\n",
              "the KONAMI IDs: the lines it lacked");
        check(get(fs::path(from.string() + ".moved") / "cards") == card(B, 1, "2222") + card(A, 2) + card(D, 3, "4444"),
              "the old folder keeps its card list as it was");
    }

    // 3. the same cards with the same numbers (a folder copied): nothing doubles
    {
        const fs::path from = top / "three" / "popn14" / "eamuse", to = top / "three" / "eamuse";
        put(to / "cards", card(A, 1) + card(B, 2));
        put(to / "refid", "3\n");
        put(from / "cards", card(A, 1) + card(B, 2));
        put(from / "refid", "3\n");
        put(from / ("pdata-F14-" + ref(2)), "B14");
        hk::ea::merge_folder(from.string(), to.string());
        check(get(to / "cards") == card(A, 1) + card(B, 2) && get(to / "refid") == "3\n", "the same cards and numbers: the list as it was");
        check(get(to / ("pdata-F14-" + ref(2))) == "B14", "... the player under its number");
    }

    // 4. a player both folders have for one card and game: the shared one stays, the other is kept in the old folder
    {
        const fs::path from = top / "four" / "popn14" / "eamuse", to = top / "four" / "eamuse";
        put(to / "cards", card(A, 5));
        put(to / "refid", "6\n");
        put(to / ("pdata-F14-" + ref(5)), "shared");
        put(from / "cards", card(A, 1));
        put(from / "refid", "2\n");
        put(from / ("pdata-F14-" + ref(1)), "other");
        put(from / ("pdata-E13-" + ref(5)), "no card's");   // (no card of this folder has ref id 5; A has it there)
        hk::ea::merge_folder(from.string(), to.string());
        check(!fs::exists(to / ("pdata-E13-" + ref(5))) && get(fs::path(from.string() + ".moved") / ("pdata-E13-" + ref(5))) == "no card's",
              "a player's file under a number no card of the folder has: not given to the card that has it there");
        check(get(to / ("pdata-F14-" + ref(5))) == "shared" && !fs::exists(to / ("pdata-F14-" + ref(1))), "a player in both: the shared folder's stays");
        check(get(fs::path(from.string() + ".moved") / ("pdata-F14-" + ref(1))) == "other" && get(to / "refid") == "6\n", "... the other kept in the old folder");
    }
    fs::remove_all(top);
    printf(failed ? "%d failed\n" : "all hold\n", failed);
    return failed ? 1 : 0;
}
