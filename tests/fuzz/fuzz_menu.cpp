// SPDX-License-Identifier: GPL-3.0-or-later
// Fuzzing (tools/fuzz.sh): the game menu's two files, its list of games and its fonts
// (runtime/menu/menu.h, font.h). The launcher writes both, but a file is a file: whatever is in
// one must be refused or drawn, with nothing read or written outside a buffer. The first byte
// says which of the two the rest is taken as.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "menu/font.h"
#include "menu/menu.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    using namespace hk::menu;
    if (!size) return 0;
    std::string err;
    static std::vector<uint32_t> px(848 * 480);
    if (data[0] & 1) {
        Font f;
        if (!f.parse(data + 1, size - 1, err)) return 0;
        // drawn with a text of its own bytes: every character it has and many it hasn't
        const std::string text((const char *)data + 1, std::min<size_t>(size - 1, 64));
        for (int k = 0; k < Font::kSizes; k++) {
            f.width(k, text);
            f.draw(px.data(), 640, 480, 0, 0, 640, 480, (int)(data[0] >> 1) - 20, (int)size % 500 - 10, k, text, 0xffffff, 200);
            f.draw(px.data(), 640, 480, 100, 100, 300, 200, 90, 95, k, "A?" + text, 0x808080, 255);
        }
        return 0;
    }
    List list;
    if (!list.parse(std::string((const char *)data + 1, size - 1), err)) return 0;
    for (Shelf &s : list.shelves)
        for (Game &g : s.games) g.picture.clear();   // (a picture is a file's name: not for a fuzzer to choose)
    State state;
    state.set(list);
    state.set_notice(list.current + "|" + list.current);
    const Act acts[] = {Act::next, Act::shelf_next, Act::prev, Act::shelf_prev, Act::next, Act::start};
    for (uint64_t frame = 0; frame < 6; frame++) {
        draw(state, frame & 1 ? "iidx" : "gamepad", frame * 9, px.data(), frame & 2 ? 848 : 640, 480);
        state.act(acts[(data[0] >> 1) % 6 == frame ? 5 : frame % 5], frame * 9);
        state.animate();
    }
    return 0;
}
