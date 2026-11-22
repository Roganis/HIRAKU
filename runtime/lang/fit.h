// SPDX-License-Identifier: GPL-3.0-or-later
// A text made to fit a width on the game side (the boxes over a game, the performance overlay),
// whatever its language: lines broken where a line may break, a text cut short by whole characters.
// Pure functions: the drawing gives each character's width (a font's advance, or 8 for SDL's 8x8).
//
// Where a line may break: after a space (the space goes); between two characters of which one is
// Japanese or Chinese (Han, kana, their punctuation: those languages put no spaces between words),
// but not before a closing mark (。、」）! ? and the small kana: Japanese's kinsoku) nor after an
// opening one (「（); at a "\n". Not inside a value kept whole (between kKeep and kKeepEnd: an
// argument put into a sentence, a path, a number with its unit) unless the value alone is wider than
// the line. A word wider than the line is broken between characters. Never inside a UTF-8 sequence.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace hk::lang {

// a character's drawn width (a code point)
using CharWidth = std::function<int(uint32_t)>;

// The marks around a value kept on one line (lang::shown(..., true) puts them round its arguments)
constexpr char kKeep = '\x03', kKeepEnd = '\x04';
std::string keep(std::string_view value);
// The text without those marks
std::string unmarked(std::string_view text);

// The characters of a UTF-8 text with where each starts (a byte that isn't UTF-8 is one character)
struct Char {
    uint32_t c;
    size_t at, len;
};
std::vector<Char> chars(std::string_view text);

// The text's drawn width
int width(std::string_view text, const CharWidth &w);

// The text in lines at most max_width wide (none wider unless one character is); "" gives none
std::vector<std::string> wrap(std::string_view text, int max_width, const CharWidth &w);

// The text, or as much of it as fits in max_width with `tail` after it ("..", "…"): whole characters
std::string clip(std::string_view text, int max_width, const CharWidth &w, std::string_view tail = "..");

// Japanese, Chinese: a line may break before and after it
bool breaks_around(uint32_t c);

}  // namespace hk::lang
