// SPDX-License-Identifier: GPL-3.0-or-later
// The fuzz targets' first inputs (tools/fuzz.sh): well-formed ones, made up here, for the fuzzer
// to start from. No game's data: calls and lists of our own.
//
//   fuzz_seeds DIR      writes DIR/kbin/*, DIR/ea/*, DIR/menu/*
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "eamuse/kbin.h"

namespace {

void put(const std::string &path, const std::string &bytes) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream(path, std::ios::binary) << bytes;
}
void u32(std::string &o, uint32_t v) {
    for (int k = 0; k < 4; k++) o += (char)(v >> (8 * k) & 0xff);
}
void u16(std::string &o, int v) { o += (char)(v & 0xff), o += (char)(v >> 8 & 0xff); }

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const std::string dir = argv[1];
    const std::string calls[] = {
        "<?xml version=\"1.0\" encoding=\"Shift_JIS\"?><call model=\"ABC:J:A:A:2001010100\" srcid=\"0123456789ABCDEF0123\"><services method=\"get\"/></call>",
        "<call model=\"M\" srcid=\"S\"><cardmng method=\"inquire\" cardid=\"E004000000000000\" cardtype=\"1\" update=\"0\"/>"
        "<game method=\"get\"><item __type=\"s32\" __count=\"3\">1 2 3</item><name __type=\"str\">abc</name><b __type=\"bin\">00ff10</b>"
        "<ip __type=\"ip4\">1.2.3.4</ip><pair __type=\"2u16\">7 9</pair></game></call>",
        "<response><facility status=\"0\"><location><id>A</id></location></facility></response>",
    };
    int n = 0;
    for (const std::string &xml : calls) {
        put(dir + "/ea/call" + std::to_string(n) + ".xml", xml);
        put(dir + "/ea/http" + std::to_string(n) + ".txt",
            "POST /core HTTP/1.0\r\nX-Eamuse-Info: 1-5cf9445d-0dfe\r\nX-Compress: none\r\nContent-Length: " + std::to_string(xml.size()) + "\r\n\r\n" + xml);
        put(dir + "/kbin/text" + std::to_string(n) + ".xml", xml);
        std::string bin;
        if (hk::ea::kbin::from_text(xml, bin)) put(dir + "/kbin/packed" + std::to_string(n) + ".bin", bin);
        n++;
    }
    put(dir + "/kbin/lz77.bin", std::string("\x00" "abcdefgh" "\x02" "i" "\x00\x13" "jklmnop", 20));
    put(dir + "/menu/list.txt", std::string(1, '\0') +
                                    "hiraku-menu-list 1\ncurrent\tb\ngame\ta\tOne shelf\tA game\t2001  A system\n"
                                    "game\tb\tOne shelf\tAnother game with a long, long name\t2002\t/a/picture.bmp\n# a note\ngame\tc\tAnother shelf\tpop'n music 18 \xe3\x81\x9b\xe3\x82\x93\t2010\n");
    std::string font("HKMF1\0\0\0", 8), ink;
    u32(font, 3);
    for (int size = 0; size < 3; size++) {
        const int box = 6 + size * 4;
        u32(font, (uint32_t)(box + 4));
        u32(font, 3);
        for (const uint32_t c : {(uint32_t)'A', (uint32_t)'?', (uint32_t)0x65e5}) {
            u32(font, c), u16(font, box + 2), u16(font, 1), u16(font, -2), u16(font, box), u16(font, box);
            u32(font, (uint32_t)ink.size());
            ink.append((size_t)box * box, (char)0xc0);
        }
    }
    u32(font, (uint32_t)ink.size());
    put(dir + "/menu/font.bin", std::string(1, '\1') + font + ink);
    printf("fuzz seeds in %s\n", dir.c_str());
    return 0;
}
