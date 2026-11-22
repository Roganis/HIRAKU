// SPDX-License-Identifier: GPL-3.0-or-later
#include "fit.h"

namespace hk::lang {

namespace {

// a line doesn't start with these (Japanese's kinsoku: closing marks, small kana, the long vowel)
bool no_break_before(uint32_t c) {
    static const char32_t marks[] = U"、。，．・：；？！ー」』）】〕〉》ゝゞ々ヽヾぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮヵヶ〜…‥）］｝，．：；！？％”’)]},.:;!?%";
    for (char32_t m : marks)
        if (m && c == (uint32_t)m) return true;
    return false;
}
// ... nor ends with these (opening marks)
bool no_break_after(uint32_t c) {
    static const char32_t marks[] = U"「『（【〔〈《［｛“‘([{";
    for (char32_t m : marks)
        if (m && c == (uint32_t)m) return true;
    return false;
}

struct Item {
    uint32_t c;
    size_t at, len;
    int span;   // the kept value it is in (0: none)
};

std::vector<Item> items(std::string_view text) {
    std::vector<Item> out;
    int span = 0, spans = 0;
    for (const Char &ch : chars(text)) {
        if (ch.c == (uint32_t)kKeep) span = ++spans;
        else if (ch.c == (uint32_t)kKeepEnd) span = 0;
        else out.push_back({ch.c, ch.at, ch.len, span});
    }
    return out;
}

std::string bytes(std::string_view text, const std::vector<Item> &it, size_t from, size_t to) {
    while (to > from && it[to - 1].c == ' ') to--;   // (a line's last spaces go)
    std::string out;
    for (size_t k = from; k < to; k++) out.append(text.substr(it[k].at, it[k].len));
    return out;
}

}  // namespace

std::string keep(std::string_view value) { return kKeep + std::string(value) + kKeepEnd; }

std::string unmarked(std::string_view text) {
    std::string out;
    for (char c : text)
        if (c != kKeep && c != kKeepEnd) out += c;
    return out;
}

std::vector<Char> chars(std::string_view s) {
    std::vector<Char> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char b = (unsigned char)s[i];
        size_t n = b < 0x80 ? 1 : (b & 0xe0) == 0xc0 ? 2 : (b & 0xf0) == 0xe0 ? 3 : (b & 0xf8) == 0xf0 ? 4 : 0;
        uint32_t c = n == 1 ? b : n == 2 ? b & 0x1f : n == 3 ? b & 0x0f : b & 0x07;
        bool ok = n > 0 && i + n <= s.size();
        for (size_t k = 1; ok && k < n; k++) {
            const unsigned char t = (unsigned char)s[i + k];
            if ((t & 0xc0) != 0x80) ok = false;
            else c = c << 6 | (t & 0x3f);
        }
        if (!ok) c = 0xfffd, n = 1;   // (not UTF-8: one byte, one character)
        out.push_back({c, i, n});
        i += n;
    }
    return out;
}

bool breaks_around(uint32_t c) {
    return (c >= 0x3000 && c <= 0x303f)       // CJK symbols and punctuation
           || (c >= 0x3040 && c <= 0x30ff)    // hiragana, katakana
           || (c >= 0x3100 && c <= 0x312f)    // bopomofo
           || (c >= 0x31f0 && c <= 0x31ff)    // katakana's small letters
           || (c >= 0x3400 && c <= 0x4dbf)    // Han, extension A
           || (c >= 0x4e00 && c <= 0x9fff)    // Han
           || (c >= 0xf900 && c <= 0xfaff)    // Han, compatibility
           || (c >= 0xff00 && c <= 0xffef)    // full-width forms, half-width kana
           || (c >= 0x20000 && c <= 0x3ffff);   // Han, the later extensions
}

int width(std::string_view text, const CharWidth &w) {
    int n = 0;
    for (const Item &i : items(text)) n += w(i.c);
    return n;
}

std::vector<std::string> wrap(std::string_view text, int max_width, const CharWidth &w) {
    std::vector<std::string> out;
    const std::vector<Item> it = items(text);
    size_t from = 0, brk = 0;   // the line's first character; the last place it may break (before it)
    int used = 0;
    for (size_t j = 0; j < it.size(); j++) {
        const uint32_t c = it[j].c;
        if (c == '\n') {
            out.push_back(bytes(text, it, from, j));
            from = j + 1, brk = from, used = 0;
            continue;
        }
        if (j > from && c != ' ') {
            const Item &p = it[j - 1];
            const bool kept = p.span && p.span == it[j].span;
            const bool may = p.c == ' ' || ((breaks_around(p.c) || breaks_around(c)) && !no_break_before(c) && !no_break_after(p.c));
            if (may && !kept) brk = j;
        }
        used += w(c);
        if (c == ' ' || used <= max_width || j == from) continue;
        // too wide: back to the last break, else (a word wider than the line) before this character
        const size_t cut = brk > from ? brk : j;
        out.push_back(bytes(text, it, from, cut));
        from = cut;
        while (from < it.size() && it[from].c == ' ') from++;
        // the new line read again from its start (its own breaks); `from` only moves on, so this ends
        brk = from, used = 0;
        j = from - 1;   // (the loop's j++: from)
    }
    if (from < it.size()) {
        std::string last = bytes(text, it, from, it.size());
        if (!last.empty()) out.push_back(last);
    }
    return out;
}

std::string clip(std::string_view text, int max_width, const CharWidth &w, std::string_view tail) {
    const std::vector<Item> it = items(text);
    int all = 0;
    for (const Item &i : it) all += w(i.c);
    if (all <= max_width) return bytes(text, it, 0, it.size());
    const int room = max_width - width(tail, w);
    size_t n = 0;
    for (int used = 0; n < it.size() && used + w(it[n].c) <= room; n++) used += w(it[n].c);
    return bytes(text, it, 0, n) + std::string(tail);
}

}  // namespace hk::lang
