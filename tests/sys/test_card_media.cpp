// SPDX-License-Identifier: GPL-3.0-or-later
// An IC card as a player's floppy disk or memory card (runtime/shell/cardmedia.h), without a game:
//   - a card's folder: beside the saves when the run's save folder is in its default place (also a
//     game with mods), else the run's own cards/ folder; none without a save folder
//   - a card's medium's file, made blank by its maker when new (here the floppy's: a formatted disk)
//   - a card's folder locked while its medium is in: a second cabinet is told it is in use, and gets
//     it once the first takes it out; the number shown is its last four digits only
//   - a touch puts the card's medium in, the same card again takes it out, another swaps it
//   - a change waits while the game writes that slot, says so once, and happens when it is done
//   - two slots: reader 1 -> slot 1, reader 2 -> slot 2; one reader: a second card to slot 2
// The card numbers are made up.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "dev/fdc.h"
#include "shell/cardmedia.h"

namespace {

namespace fs = std::filesystem;
using namespace hk;
int failed = 0;
void check(bool ok, const char *what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what);
    fflush(stdout);
    failed += !ok;
}

const std::string A = "E004010000000001", B = "E004010000000002", C = "E004010000000003";

// A drive as a core has it: what is in each slot, and whether the game is writing it
struct Drive {
    std::string in[2];
    bool busy[2] = {false, false};
    std::vector<std::string> said;
    int uses = 0;
    cardmedia::Changer::Medium medium(int slots) {
        cardmedia::Changer::Medium m;
        m.what = slots == 1 ? "Floppy disk" : "Memory card", m.file = slots == 1 ? "floppy.img" : "memcard.mcr", m.slots = slots;
        m.busy = [this](int s) { return busy[s]; };
        m.use = [this](int s, const std::string &number, const std::string &file, std::string &why) {
            uses++;
            if (!file.empty() && !fs::exists(file)) {   // as the floppy's: a new card gets a blank disk
                const auto d = Fdc37c665::blank_disk();
                FILE *f = fopen(file.c_str(), "wb");
                if (!f) return why = "cannot write", false;
                fwrite(d.data(), 1, d.size(), f);
                fclose(f);
            }
            in[s] = number;
            return true;
        };
        m.say = [this](const std::string &s) { said.push_back(s); };
        return m;
    }
    bool said_has(const std::string &part) const {
        for (const auto &s : said)
            if (s.find(part) != std::string::npos) return true;
        return false;
    }
    bool said_number() const {   // a whole number anywhere: never
        for (const auto &s : said)
            if (s.find("E00401") != std::string::npos) return true;
        return false;
    }
};

}  // namespace

int main() {
    const fs::path tmp = fs::temp_directory_path() / ("hk-card-media-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(tmp);
    fs::create_directories(tmp / "root");
    const std::string root = (tmp / "root").string();
    fs::create_directories(tmp / "root" / "user");   // a portable install: user_data_dir is in root
    const std::string saves = user_data_dir(root);

    // ---- where a card's folder is ----
    check(cardmedia::folder(root, "bm3final", saves + "/bm3final") == saves + "/card-media", "the default save folder: beside the saves");
    check(cardmedia::folder(root, "bm3final", saves + "/bm3final+turntable") == saves + "/card-media", "a game with mods: the same folder");
    check(cardmedia::folder(root, "bm3final", "/elsewhere/nv") == "/elsewhere/nv/cards", "--nvram elsewhere: its own cards/");
    check(cardmedia::folder(root, "bm3final", "").empty(), "no save folder: none");
    check(cardmedia::shown(A) == "\xE2\x80\xA6" "0001", "a card is named by its last four digits");

    // ---- one slot: in, out, swap; a blank disk for a new card ----
    const std::string dir = (tmp / "cards").string();
    cardmedia::FolderStore store(dir);
    Drive d;
    cardmedia::Changer floppy(store, d.medium(1));
    floppy.touch(0, A);
    check(d.in[0] == A && floppy.held(0) == A, "a touch: the card's disk in the drive");
    check(fs::file_size(dir + "/" + A + "/floppy.img") == Fdc37c665::kDiskBytes, "... a new card's: a blank 1.44 MB disk in its folder");
    check(d.said_has("Floppy disk of card \xE2\x80\xA6" "0001 in the drive"), "... and said");
    floppy.touch(1, A);   // (the other reader: the floppy takes any)
    check(d.in[0].empty() && floppy.held(0).empty(), "the same card again: taken out");
    check(d.said_has("taken out"), "... and said");
    floppy.touch(0, A);
    floppy.touch(0, B);
    check(d.in[0] == B && floppy.held(0) == B, "another card: swapped");
    check(fs::exists(dir + "/" + B + "/floppy.img"), "... its own disk");

    // ---- a second cabinet on the same folder ----
    {
        cardmedia::FolderStore other(dir);
        Drive d2;
        cardmedia::Changer floppy2(other, d2.medium(1));
        floppy2.touch(0, B);
        check(d2.in[0].empty() && d2.said_has("in use at another cabinet"), "a card in at one cabinet: in use at another");
        floppy2.touch(0, A);
        check(d2.in[0] == A, "... a card taken out there is free here");
        floppy.touch(0, A);
        check(d.in[0] == B && d.said_has("in use at another cabinet"), "... and now in use for the first");
        floppy2.touch(0, A);   // out at the second
        floppy.touch(0, A);
        check(d.in[0] == A, "... free again once taken out");
        check(!d2.said_number(), "the second cabinet's lines: no whole number");
    }

    // ---- the game writing: the change waits ----
    d.said.clear();
    d.busy[0] = true;
    floppy.touch(0, C);
    check(d.in[0] == A && floppy.waiting(), "the game writing: a swap waits");
    check(d.said_has("waits: the game is writing the disk"), "... and says so");
    const size_t lines = d.said.size();
    floppy.frame();
    floppy.frame();
    check(d.said.size() == lines && d.in[0] == A, "... said once, still waiting");
    d.busy[0] = false;
    floppy.frame();
    check(d.in[0] == C && !floppy.waiting(), "the writing over: swapped");
    d.busy[0] = true;
    floppy.touch(0, C);
    check(d.in[0] == C, "taking out waits too");
    d.busy[0] = false;
    floppy.frame();
    check(d.in[0].empty(), "... then out");
    check(!d.said_number(), "no whole number said");

    // ---- two slots ----
    {
        cardmedia::FolderStore cards(dir + "2");
        Drive m;
        cardmedia::Changer mc(cards, m.medium(2));
        mc.touch(0, A);
        check(m.in[0] == A, "memory cards, one reader: the first card to slot 1");
        mc.touch(0, B);
        check(m.in[1] == B && m.in[0] == A, "... a second card to slot 2 while slot 1 is full");
        check(m.said_has("Memory card 2 of card"), "... said by its slot");
        mc.touch(0, A);
        check(m.in[0].empty() && m.in[1] == B, "... the first again: out of slot 1");
        mc.touch(0, C);
        check(m.in[0] == C, "... a third: the empty slot 1");
        mc.touch(0, C), mc.touch(0, B);
        check(m.in[0].empty() && m.in[1].empty(), "both out");
        mc.touch(1, A);
        check(m.in[1] == A && m.in[0].empty(), "reader 2: slot 2");
        mc.touch(0, B);
        check(m.in[0] == B, "reader 1: slot 1");
        mc.touch(0, C);
        check(m.in[0] == C && m.in[1] == A, "two readers seen: another card at reader 1 swaps slot 1");
    }

    fs::remove_all(tmp);
    printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
