// SPDX-License-Identifier: GPL-3.0-or-later
// PlayStation memory cards in the System 573's memory card reader (memcard.h).
#include "sys573/memcard.h"

#include "emu.h"
#include "imagedev/memcard.h"
#include "shell/saves.h"
#include "shell/sys.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace hk::k573 {

namespace {

constexpr size_t FRAME = 128;

std::pair<std::string, std::string> place(const std::string &f) {   // the file's folder and its name there
    const size_t slash = f.find_last_of("/\\");
    return slash == std::string::npos ? std::make_pair(std::string("."), f) : std::make_pair(f.substr(0, slash), f.substr(slash + 1));
}

template <class S>
const std::string &name_of(const S &s) { return s.label.empty() ? s.file : s.label; }

bool read_whole(const std::string &path, std::vector<uint8_t> &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return !in.bad();
}

}  // namespace

uint8_t memory_card_frame_sum(const uint8_t *frame) {
    uint8_t x = 0;
    for (size_t i = 0; i < FRAME - 1; i++) x ^= frame[i];
    return x;
}

std::vector<uint8_t> blank_memory_card() {
    std::vector<uint8_t> c(kMemoryCardBytes, 0);
    auto frame = [&](size_t n) { return c.data() + n * FRAME; };
    frame(0)[0] = 'M', frame(0)[1] = 'C';
    for (size_t n = 1; n <= 15; n++) {   // the directory: each block free (0xa0), no next block
        uint8_t *f = frame(n);
        f[0] = 0xa0;
        f[8] = f[9] = 0xff;
    }
    for (size_t n = 16; n <= 35; n++) {   // the broken-frame list: none
        uint8_t *f = frame(n);
        memset(f, 0xff, 4);
        f[8] = f[9] = 0xff;
    }
    memcpy(frame(63), frame(0), FRAME);   // (the header's copy, as cards are formatted)
    for (size_t n : {0, 63}) frame(n)[FRAME - 1] = memory_card_frame_sum(frame(n));
    for (size_t n = 1; n <= 35; n++) frame(n)[FRAME - 1] = memory_card_frame_sum(frame(n));
    return c;
}

void MemoryCards::configure(const std::string &slot1, const std::string &slot2) {
    slots_[0] = Slot{};
    slots_[1] = Slot{};
    slots_[0].file = slot1;
    slots_[1].file = slot2;
}

bool MemoryCards::attach(device_memcard_image_interface *slot1, device_memcard_image_interface *slot2, std::string &why) {
    slots_[0].dev = slot1;
    slots_[1].dev = slot2;
    for (int k = 0; k < 2; k++) {
        if (!given(k)) continue;
        if (!slots_[k].dev) {
            why = "this game has no memory card reader";
            return false;
        }
        if (!insert(k, true, why)) return false;
    }
    return true;
}

// The file's card into the slot: a new file is a formatted card. false (and why): not a card's image
bool MemoryCards::insert(int k, bool at_power_on, std::string &why) {
    Slot &s = slots_[k];
    saves::writer().drain();   // (the file may still be on its way from when the card came out)
    saves::recover(place(s.file).first);
    std::vector<uint8_t> image;
    bool is_new = false;
    s.read_only = false;
    s.known = true;
    if (!sys::exists(s.file)) {
        image = blank_memory_card();
        is_new = true;
        sys::make_dirs(place(s.file).first);
        if (!sys::write_file(s.file, image.data(), image.size())) {
            s.read_only = true;
            fprintf(stderr, "memory card %d: cannot write %s: what the game saves on this card is kept for this run only\n", k + 1, name_of(s).c_str());
        }
    } else if (!read_whole(s.file, image) || image.size() != kMemoryCardBytes) {
        why = name_of(s) + " is not a PlayStation memory card's raw image (" + std::to_string(kMemoryCardBytes) + " bytes)";
        return false;
    } else {
        std::error_code ec;
        const auto perms = std::filesystem::status(s.file, ec).permissions();
        s.read_only = !ec && (perms & std::filesystem::perms::owner_write) == std::filesystem::perms::none;
        if (s.read_only) fprintf(stderr, "memory card %d: %s can't be written: what the game saves on this card is kept for this run only\n", k + 1, name_of(s).c_str());
    }
    s.bytes = std::move(image);
    s.dev->hk_insert(&s.bytes);
    s.inserted = true;
    s.fresh = true;
    s.kept = s.seen = s.dev->hk_writes();
    s.failed = saves::writer().failed();
    // (the file's name on stderr: stdout's lines are a run's statistics, the same wherever its files are)
    printf("memory card %d: %s in the slot%s\n", k + 1, is_new ? "a new formatted card" : "the card", at_power_on ? " at power-on" : "");
    fprintf(stderr, "memory card %d: %s\n", k + 1, name_of(s).c_str());
    return true;
}

void MemoryCards::tick(double seconds) {
    for (Slot &s : slots_)
        if (s.inserted && s.dev->hk_writes() != s.seen) s.seen = s.dev->hk_writes(), s.changed_at = seconds;
}

void MemoryCards::keep_slot(Slot &s, bool now) {
    if (s.read_only) return;
    if (now) saves::writer().drain();
    // a set the writer couldn't write: the file may not be what it was thought to be
    if (const unsigned failed = saves::writer().failed(); failed != s.failed) s.failed = failed, s.known = false;
    const uint64_t writes = s.dev->hk_writes();
    if (s.known && writes == s.kept) return;
    const auto [dir, name] = place(s.file);
    std::vector<saves::File> set{{name, std::string(s.bytes.begin(), s.bytes.end())}};
    s.kept = writes, s.known = true;
    if (!now) {
        saves::writer().write(dir, std::move(set));
        return;
    }
    if (!saves::write_set(dir, set)) {
        s.known = false;
        fprintf(stderr, "memory card: cannot write %s: the card as it was before is kept\n", name_of(s).c_str());
    }
}

void MemoryCards::keep(bool now, double seconds) {
    for (Slot &s : slots_) {
        if (!s.inserted) continue;
        // while playing, a card the game is still writing waits: a save is several frames, written
        // a command at a time
        if (!now && (s.dev->hk_writes() != s.seen || seconds - s.changed_at < 1.0)) continue;
        keep_slot(s, now);
    }
}

std::string MemoryCards::toggle(int k) {
    if (!given(k) || !slots_[k].dev) return {};
    Slot &s = slots_[k];
    std::string words;
    if (s.inserted) {
        keep_slot(s, false);   // (on the writer's thread: the game doesn't wait for the file)
        s.dev->hk_insert(nullptr);
        s.inserted = false;
        words = "Memory card " + std::to_string(k + 1) + " taken out";
    } else {
        std::string why;
        words = insert(k, false, why) ? "Memory card " + std::to_string(k + 1) + " in its slot"
                                      : "No memory card " + std::to_string(k + 1) + ": " + why;
    }
    printf("memory card %d: %s\n", k + 1, words.c_str());
    if (said) said(words);
    return words;
}

bool MemoryCards::use(int k, const std::string &file, const std::string &label, std::string &why) {
    if (k < 0 || k > 1 || !slots_[k].dev) return why = "this game has no memory card reader", false;
    Slot &s = slots_[k];
    if (s.inserted) {
        keep_slot(s, true);   // (now: the file may be another cabinet's as soon as this one lets go of it)
        s.dev->hk_insert(nullptr);
        s.inserted = false;
    }
    s.file = file, s.label = label;
    if (file.empty()) return true;
    if (insert(k, false, why)) return true;
    s.file.clear(), s.label.clear();
    return false;
}

bool MemoryCards::busy(int k, double seconds) const {
    if (k < 0 || k > 1 || !slots_[k].inserted) return false;
    const Slot &s = slots_[k];
    return s.dev->hk_writes() != s.seen || seconds - s.changed_at < 1.0;
}

void MemoryCards::before_reader_write(int slot) {
    if (slot < 0 || slot > 1 || !slots_[slot].inserted || !slots_[slot].fresh) return;
    slots_[slot].fresh = false;
    slots_[slot].dev->hk_ignore_writes(true);
}

void MemoryCards::after_reader_write() {
    for (Slot &s : slots_)
        if (s.dev) s.dev->hk_ignore_writes(false);
}

bool MemoryCards::in(int slot) const { return slot >= 0 && slot < 2 && slots_[slot].inserted; }

}  // namespace hk::k573
