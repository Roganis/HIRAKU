// SPDX-License-Identifier: GPL-3.0-or-later
// runtime/eamuse/kbin.h: text -> kbin -> text keeps every element, attribute, type and value
// (arrays, the small types kbin packs in shared words, strings, binary, ip4, floats), and the
// bytes of a small call are kbin's as mon's kbinxml writes them. Exit 0 when all hold.
#include <cstdio>
#include <string>

#include "eamuse/kbin.h"

static int g_fail;
static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    g_fail |= !ok;
}

int main() {
    using namespace hk::ea::kbin;
    const std::string xml =
        "<?xml version=\"1.0\"?><call model=\"J39:J:A:A:2010040500\" srcid=\"0\"><game method=\"get\">"
        "<a __type=\"u8\">7</a><b __type=\"s16\">-2</b><c __type=\"u8\">255</c><d __type=\"s32\">-100000</d>"
        "<e __type=\"2u16\">1 65535</e><f __type=\"ip4\">10.57.0.1</f><g __type=\"u8\" __count=\"3\">1 2 3</g>"
        "<h __type=\"bin\">00ff10</h><i>text</i><j __type=\"float\">1.500000</j><k __type=\"u64\">18446744073709551615</k>"
        "<x_Y __type=\"time\">1792661764</x_Y><empty/></game></call>";
    std::string bin, back, bin2;
    check(from_text(xml, bin), "text to kbin");
    check(is_binary(bin), "kbin's signature");
    check(to_text(bin, back), "kbin to text");
    check(from_text(back, bin2) && bin2 == bin, "the same bytes again from the text read back");
    for (const char *want : {"<a __type=\"u8\">7</a>", "<b __type=\"s16\">-2</b>", "<d __type=\"s32\">-100000</d>",
                             "<e __type=\"2u16\">1 65535</e>", "<f __type=\"ip4\">10.57.0.1</f>",
                             "<g __type=\"u8\" __count=\"3\">1 2 3</g>", "<h __type=\"bin\">00ff10</h>", "<i __type=\"str\">text</i>",
                             "<j __type=\"float\">1.500000</j>", "<k __type=\"u64\">18446744073709551615</k>", "model=\"J39:J:A:A:2010040500\""})
        check(back.find(want) != std::string::npos, want);
    // <call model="M" srcid="S"><game method="get"/></call> as kbinxml writes it (Shift-JIS, six-bit names)
    std::string small;
    from_text("<call model=\"M\" srcid=\"S\"><game method=\"get\"/></call>", small);
    static const unsigned char ref[] = {
        0xa0, 0x42, 0x80, 0x7f, 0x00, 0x00, 0x00, 0x20, 0x01, 0x04, 0xa2, 0x6c, 0x71, 0x2e, 0x05, 0xcb, 0x4a, 0x6a,
        0xc4, 0x2e, 0x05, 0xe3, 0x7a, 0x2e, 0xa4, 0x01, 0x04, 0xb2, 0x6c, 0xaa, 0x2e, 0x06, 0xca, 0xae, 0x6d, 0xd2,
        0x90, 0xfe, 0xfe, 0xff, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x02, 0x4d, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x02, 0x53, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x67, 0x65, 0x74, 0x00};
    check(small == std::string((const char *)ref, sizeof ref), "a small call's bytes as kbinxml's");
    // Konami's LZ77: literals, a back reference, the end
    const std::string packed = std::string("\x0f" "abca", 5) + std::string("\x00\x45\x00\x00", 4);
    check(lz77_unpack(packed) == "abcaabcaabca", "LZ77: four letters, eight from four back, the end");
    return g_fail;
}
