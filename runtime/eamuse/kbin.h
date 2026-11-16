// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Konami's binary XML ("kbin", libavs's binary property format) and the LZ77 that e-AMUSEMENT's
// HTTP bodies may be packed in (X-Compress: lz77). pop'n music 18's libavs sends calls so
// (runtime/pc/, PLAN.md section 30); pop'n 9-15 sent text. Written from mon's kbinxml (MIT,
// https://github.com/mon/kbinxml), which documents the format.
//
// The text form is kbinxml's: a typed value is an element with __type (u8, s32, str, bin, ip4,
// time, 2u16, ...) and its value as text, an array has __count (values space-separated), bin is
// hex; an element without __type is a str when it has text, else void. Strings are kept as their
// bytes (Shift-JIS as the game sent them), escaped for XML.
#include <string>

namespace hk::ea::kbin {

bool is_binary(const std::string &body);   // starts with kbin's signature (0xa0, 0x42 / 0x45)

// kbin to XML text (with an <?xml?> line). false: not kbin, or cut short
bool to_text(const std::string &bin, std::string &xml);

// XML text to kbin (names packed six bits a letter, Shift-JIS). false: not XML it can read
bool from_text(const std::string &xml, std::string &bin);

// Konami's LZ77 (a flag byte, then 8 items: a byte as is, or two bytes: 12 bits back, 4 of length - 3)
std::string lz77_unpack(const std::string &packed);

}  // namespace hk::ea::kbin
