// SPDX-License-Identifier: GPL-3.0-or-later
// An e-AMUSEMENT IC card as a player's floppy disk or memory card (--card-media): for a cabinet
// that has an IC card reader and no box of floppies or memory cards. The games that keep a player's
// data on a medium the cabinet takes (beatmania III's floppy disk, the System 573's PlayStation
// memory cards) get, when a card is touched to a reader, that card's own medium in their drive:
// one image per IC card, kept in the card's folder.
//
//   <the saves' folder>/card-media/NUMBER/floppy.img     beatmania III's disk
//   <the saves' folder>/card-media/NUMBER/memcard.mcr    a PlayStation memory card
//
// beside the shared e-AMUSEMENT folder (eamuse/), shared by every game, when the run's save folder
// is in its default place (shell/userdir.h: <saves>/<game>, or <game>+<mod>); a run with its saves
// elsewhere (--nvram DIR) keeps them in DIR/cards/NUMBER/, as the pop'n games' shared saves go.
//
// A touch puts that card's medium in (a card new here gets a blank one: the medium's own blank
// maker, a formatted disk or card); the same card again takes it out; another card swaps it. While
// the game is writing the medium in that slot (the floppy's motor on; a memory card written in the
// last second) the change waits, and says so. While its medium is in, the card's folder is locked
// (lock.txt): a second cabinet on the same folder is told the card is in use at another cabinet.
//
// The number to folder mapping (Store) is behind a small interface so that a later sync through
// hiraku-eamuse can replace it: resolve (the file a medium is read from and written to), lock and
// unlock. Loading and storing are the cores' own (whole files, written through shell/saves.h).
//
// Numbers never show whole: on screen and in the log a card is its last four digits (shown()).
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "shell/sys.h"
#include "shell/userdir.h"

namespace hk::cardmedia {

// "…1234": how a card is named to the player and in the log
inline std::string shown(const std::string &number) {
    return "\xE2\x80\xA6" + (number.size() > 4 ? number.substr(number.size() - 4) : number);
}

// The folder the cards' folders go in, for a run of `game` saving in nvram_dir ("" when there is no
// save folder: no card's medium can be kept)
inline std::string folder(const std::string &root, const std::string &game, const std::string &nvram_dir) {
    if (nvram_dir.empty()) return {};
    const std::string saves = user_data_dir(root), mine = saves + "/" + game;
    if (nvram_dir == mine || nvram_dir.rfind(mine + "+", 0) == 0) return saves + "/card-media";
    return nvram_dir + "/cards";
}

// The number -> medium mapping
class Store {
public:
    virtual ~Store() = default;
    // The file holding `number`'s medium of that name ("floppy.img", "memcard.mcr"): read when it goes
    // in (a file that isn't there: a blank medium, made there), written back as the game changes it
    virtual std::string resolve(const std::string &number, const std::string &medium) = 0;
    // The card taken for this cabinet while a medium of it is in: Taken (also when taken already),
    // Held (another cabinet has it), None (nothing could be known: go on)
    virtual sys::Lock lock(const std::string &number) = 0;
    virtual void unlock(const std::string &number) = 0;
};

// The folders of a cabinet's own disk (or one shared over the network as a folder)
class FolderStore final : public Store {
public:
    explicit FolderStore(std::string dir) : dir_(std::move(dir)) {}
    std::string resolve(const std::string &number, const std::string &medium) override {
        sys::make_dirs(dir_ + "/" + number);
        return dir_ + "/" + number + "/" + medium;
    }
    sys::Lock lock(const std::string &number) override {
        if (locks_.count(number)) return sys::Lock::Taken;
        sys::make_dirs(dir_ + "/" + number);
        sys::Lock r;
        auto l = std::make_unique<sys::FileLock>(dir_ + "/" + number + "/lock.txt", &r);
        if (r == sys::Lock::Taken) locks_[number] = std::move(l);
        return r;
    }
    void unlock(const std::string &number) override { locks_.erase(number); }
    const std::string &dir() const { return dir_; }

private:
    std::string dir_;
    std::map<std::string, std::unique_ptr<sys::FileLock>> locks_;
};

// A game's drive (one slot: the floppy) or reader (two: the memory cards), changed by the cards
// touched to the cabinet's readers. The core says how its slots are busy and how a file goes in
// (use(slot, "") takes the medium out, written back first); everything else is here.
class Changer {
public:
    struct Medium {
        std::string what;        // "Floppy disk", "Memory card" (a slot's number added when there are two)
        std::string file;        // its file in a card's folder: "floppy.img", "memcard.mcr"
        int slots = 1;
        std::function<bool(int slot)> busy;                                          // the game is writing it
        // the card's medium into the slot, from file (number and file "": the slot left empty, the
        // medium in it written back and taken out first). false (and why): not a medium's image
        std::function<bool(int slot, const std::string &number, const std::string &file, std::string &why)> use;
        std::function<void(const std::string &)> say;                               // a line for the player
    };
    Changer(Store &store, Medium m) : store_(store), m_(std::move(m)) {}

    // A card presented at reader `reader` (0, 1). The change happens now, or when the game has
    // finished writing that slot (frame())
    void touch(int reader, const std::string &number) {
        if (reader == 1) second_reader_ = true;
        pending_ = {reader, number};
        has_pending_ = true;
        told_wait_ = false;
        frame();
    }
    // Once a frame: a change that was waiting for the game
    void frame() {
        if (!has_pending_) return;
        const auto [reader, number] = pending_;
        int slot = 0;
        bool out = false;
        if (const int at = slot_of(number); at >= 0) slot = at, out = true;
        else slot = slot_for(reader);
        if (m_.busy && m_.busy(slot)) {
            if (!told_wait_) tell(name(slot) + " of card " + shown(number) + " waits: the game is writing " + it(slot));
            told_wait_ = true;
            return;
        }
        has_pending_ = false;
        if (out) {
            take_out(slot, "taken out");
            return;
        }
        if (store_.lock(number) == sys::Lock::Held) {
            tell(name(slot) + " of card " + shown(number) + ": the card is in use at another cabinet");
            return;
        }
        const std::string before = held_[slot];
        if (!before.empty()) take_out(slot, "taken out");
        std::string why;
        if (!m_.use(slot, number, store_.resolve(number, m_.file), why)) {
            store_.unlock(number);
            tell(name(slot) + " of card " + shown(number) + " can't go in: " + why);
            return;
        }
        held_[slot] = number;
        tell(name(slot) + " of card " + shown(number) + " in " + place());
    }
    const std::string &held(int slot) const { return held_[slot]; }
    bool waiting() const { return has_pending_; }
    bool any_presented() const { return !held_[0].empty() || !held_[1].empty() || has_pending_; }

private:
    int slot_of(const std::string &number) const {
        for (int s = 0; s < m_.slots; s++)
            if (held_[s] == number) return s;
        return -1;
    }
    // reader 1 -> slot 1, reader 2 -> slot 2; with one reader (none from the second seen), a second
    // card goes to slot 2 while slot 1 holds another; one slot takes any reader's
    int slot_for(int reader) const {
        if (m_.slots == 1) return 0;
        if (reader == 1) return 1;
        if (!held_[0].empty() && held_[1].empty() && !second_reader_) return 1;
        return 0;
    }
    void take_out(int slot, const char *how) {
        std::string why;
        m_.use(slot, "", "", why);
        const std::string number = held_[slot];
        held_[slot].clear();
        if (number.empty()) return;
        store_.unlock(number);
        tell(name(slot) + " of card " + shown(number) + " " + how);
    }
    std::string name(int slot) const { return m_.slots > 1 ? m_.what + " " + std::to_string(slot + 1) : m_.what; }
    std::string it(int) const { return m_.slots > 1 ? "the card" : "the disk"; }
    std::string place() const { return m_.slots > 1 ? "its slot" : "the drive"; }
    void tell(const std::string &s) {
        if (m_.say) m_.say(s);
    }

    Store &store_;
    Medium m_;
    std::string held_[2];
    std::pair<int, std::string> pending_;
    bool has_pending_ = false, told_wait_ = false, second_reader_ = false;
};

}  // namespace hk::cardmedia
