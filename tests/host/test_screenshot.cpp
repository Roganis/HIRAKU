// SPDX-License-Identifier: GPL-3.0-or-later
// Screenshots (runtime/host/screenshot.h) and the PNG they are written as (runtime/host/png.h,
// encode): made-up pictures encoded and read back with decode (every pixel and the size, for a
// 1x1, odd widths and a larger one), the files' names (the time in them, -2, -3... and no file
// written over), a folder that is a file (said, nothing written), a folder with letters outside
// ASCII, and the saver's thread (two pictures in the same second, both kept).
//
//   test_screenshot DIR   DIR: a folder to write in (emptied first). Leaves DIR/check/*.png for
//                         tests/host/check_png.py, which reads them with Python's own zlib
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "host/png.h"
#include "host/screenshot.h"
#include "shell/sys.h"

namespace {

int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            g_fail++;                                                             \
        }                                                                         \
    } while (0)

// A made-up picture: gradients, a few flat areas and some noise, so every row filter gets used
std::vector<uint32_t> picture(int w, int h, uint32_t seed) {
    std::vector<uint32_t> p((size_t)w * h);
    uint32_t r = seed * 2654435761u + 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            r = r * 1664525u + 1013904223u;
            uint32_t v;
            if ((x / 8 + y / 8) % 3 == 0) v = 0x203040;
            else if ((x + y) % 5 == 0) v = r >> 8;
            else v = (uint32_t)(x * 255 / w) << 16 | (uint32_t)(y * 255 / h) << 8 | (uint32_t)((x ^ y) & 0xff);
            p[(size_t)y * w + x] = v & 0xffffff;
        }
    return p;
}

bool same(const std::vector<uint32_t> &rgb, int w, int h, const std::vector<uint8_t> &file) {
    int dw = 0, dh = 0;
    std::vector<uint8_t> rgba;
    std::string err;
    if (!hk::png::decode(file, dw, dh, rgba, err)) {
        fprintf(stderr, "decode %dx%d: %s\n", w, h, err.c_str());
        return false;
    }
    if (dw != w || dh != h) return false;
    for (size_t i = 0; i < rgb.size(); i++) {
        const uint32_t p = rgb[i];
        if (rgba[i * 4] != (p >> 16 & 0xff) || rgba[i * 4 + 1] != (p >> 8 & 0xff) || rgba[i * 4 + 2] != (p & 0xff) || rgba[i * 4 + 3] != 255)
            return false;
    }
    return true;
}

std::vector<uint8_t> read_all(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: test_screenshot DIR\n");
        return 2;
    }
    const std::string dir = argv[1];
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(dir), ec);
    hk::sys::make_dirs(dir + "/check");

    // encode, then decode: the same pixels and size
    const int sizes[][2] = {{1, 1}, {3, 2}, {7, 5}, {1, 9}, {9, 1}, {33, 17}, {641, 481}};
    for (const auto &s : sizes) {
        const std::vector<uint32_t> rgb = picture(s[0], s[1], (uint32_t)(s[0] * 31 + s[1]));
        const std::vector<uint8_t> file = hk::png::encode(rgb.data(), s[0], s[1]);
        CHECK(!file.empty());
        CHECK(same(rgb, s[0], s[1], file));
        const std::string name = dir + "/check/" + std::to_string(s[0]) + "x" + std::to_string(s[1]) + ".png";
        CHECK(hk::sys::write_file(name, file.data(), file.size()));
    }
    {   // a flat picture: small (deflated), and still the same
        std::vector<uint32_t> rgb((size_t)640 * 480, 0x00ff8000);
        const std::vector<uint8_t> file = hk::png::encode(rgb.data(), 640, 480);
        CHECK(same(rgb, 640, 480, file));
        CHECK(file.size() < 10000);
    }
    CHECK(hk::png::encode(nullptr, 4, 4).empty());
    uint32_t one = 0;
    CHECK(hk::png::encode(&one, 0, 1).empty());
    CHECK(hk::png::encode(&one, 1, 8193).empty());

    // the names: the time in them, then -2, -3... with no file written over
    {
        const std::string f = dir + "/names";
        hk::sys::make_dirs(f);
        std::tm t{};
        t.tm_year = 2026 - 1900, t.tm_mon = 10, t.tm_mday = 21, t.tm_hour = 9, t.tm_min = 5, t.tm_sec = 7, t.tm_isdst = -1;
        const std::time_t when = std::mktime(&t);   // (local time, as file_name reads it back)
        const std::string first = hk::host::screenshot::file_name(f, "bm1stmix", when);
        CHECK(first == f + "/bm1stmix-20261121-090507.png");
        std::string out;
        const std::vector<uint32_t> rgb = picture(5, 3, 1);
        CHECK(hk::host::screenshot::save(rgb.data(), 5, 3, 1.0, f, "bm1stmix", when, out) && out == first);
        CHECK(hk::host::screenshot::file_name(f, "bm1stmix", when) == f + "/bm1stmix-20261121-090507-2.png");
        CHECK(hk::host::screenshot::save(rgb.data(), 5, 3, 1.0, f, "bm1stmix", when, out) && out == f + "/bm1stmix-20261121-090507-2.png");
        CHECK(hk::host::screenshot::save(rgb.data(), 5, 3, 1.0, f, "bm1stmix", when, out) && out == f + "/bm1stmix-20261121-090507-3.png");
        CHECK(same(rgb, 5, 3, read_all(first)));
        CHECK(!hk::sys::exists(first + ".tmp"));
    }

    // a folder that is a file: said, nothing written
    {
        const std::string file = dir + "/a-file";
        CHECK(hk::sys::write_file(file, "x", 1));
        std::string out;
        const std::vector<uint32_t> rgb = picture(4, 4, 2);
        CHECK(!hk::host::screenshot::save(rgb.data(), 4, 4, 1.0, file, "g", std::time(nullptr), out));
        CHECK(out.find("is not a folder") != std::string::npos);
        CHECK(!hk::host::screenshot::save(rgb.data(), 4, 4, 1.0, file + "/below", "g", std::time(nullptr), out));
        fprintf(stderr, "(as said: %s)\n", out.c_str());
    }

    // a folder with letters outside ASCII (made with the first picture)
    {
        const std::string f = dir + "/スクリーン ショット é";
        std::string out;
        const std::vector<uint32_t> rgb = picture(12, 7, 3);
        CHECK(!hk::sys::exists(f));
        CHECK(hk::host::screenshot::save(rgb.data(), 12, 7, 1.0, f, "popn14", std::time(nullptr), out));
        CHECK(hk::sys::is_dir(f));
        CHECK(same(rgb, 12, 7, read_all(out)));
    }

    // the saver's thread: two in the same second, both kept, each told
    {
        const std::string f = dir + "/saver";
        int told = 0, ok = 0;
        std::vector<std::string> paths;
        {
            hk::host::screenshot::Saver saver;
            const std::vector<uint32_t> a = picture(320, 240, 4), b = picture(320, 240, 5);
            auto done = [&](bool k, const std::string &p) { told++, ok += k, paths.push_back(p); };
            saver.take(a.data(), 320, 240, 1.0, f, "g", done);
            saver.take(b.data(), 320, 240, 1.0, f, "g", done);
            saver.wait();
            CHECK(told == 2 && ok == 2);
            CHECK(paths.size() == 2 && paths[0] != paths[1]);
            if (paths.size() == 2) CHECK(same(a, 320, 240, read_all(paths[0])) && same(b, 320, 240, read_all(paths[1])));
            saver.take(a.data(), 320, 240, 1.0, f, "g", done);   // (one more, left to the destructor to finish)
        }
        CHECK(told == 3);
    }

    // the screen's shape (host/screenshot.h: pixel_shape, shaped_size, stretch)
    {
        namespace sc = hk::host::screenshot;
        int W = 0, H = 0;
        // 640x240 on a 4:3 screen: pixels twice as tall as wide, every line twice
        const double tall = sc::pixel_shape(4.0 / 3.0, 1, 1, 640, 240);
        CHECK(std::abs(tall - 0.5) < 1e-12);
        sc::shaped_size(640, 240, tall, W, H);
        CHECK(W == 640 && H == 480);
        const std::vector<uint32_t> src = picture(640, 240, 6);
        const std::vector<uint32_t> two = sc::stretch(src.data(), 640, 240, 640, 480);
        bool doubled = two.size() == (size_t)640 * 480;
        for (int y = 0; doubled && y < 480; y++)
            for (int x = 0; x < 640; x++)
                if (two[(size_t)y * 640 + x] != src[(size_t)(y / 2) * 640 + x]) doubled = false;
        CHECK(doubled);
        // ... saved so, through save() and back by decode
        std::string out;
        CHECK(sc::save(src.data(), 640, 240, tall, dir + "/shaped", "tall", std::time(nullptr), out));
        CHECK(same(two, 640, 480, read_all(out)));

        // 512x448 on 4:3: pixels wider than tall, 597 columns (512 * 7/6 = 597.33)
        const double wide = sc::pixel_shape(4.0 / 3.0, 1, 1, 512, 448);
        sc::shaped_size(512, 448, wide, W, H);
        CHECK(W == 597 && H == 448);
        // columns of two colours in turn, three wide, and a flat area: flat stays flat, the first and
        // last columns as they were, a blend only where two different colours meet (and then their mean)
        std::vector<uint32_t> cols((size_t)512 * 448);
        for (int y = 0; y < 448; y++)
            for (int x = 0; x < 512; x++) cols[(size_t)y * 512 + x] = y >= 400 ? 0x405060 : (x / 3) % 2 ? 0xffffff : 0x000000;
        const std::vector<uint32_t> st = sc::stretch(cols.data(), 512, 448, W, H);
        CHECK(st.size() == (size_t)597 * 448);
        bool flat = true, ends = true, blends_ok = true, every = true;
        int blends = 0;
        std::vector<bool> seen(512, false);
        for (int y = 0; y < 448; y++) {
            const uint32_t *o = &st[(size_t)y * 597], *i = &cols[(size_t)y * 512];
            if (o[0] != i[0] || o[596] != i[511]) ends = false;
            for (int x = 0; x < 597; x++) {
                const uint32_t v = o[x];
                if (y >= 400 && v != 0x405060) flat = false;
                if (y < 400 && v != 0 && v != 0xffffff) {
                    blends += y == 0;
                    // the source pixels under it: one black and one white, never three
                    const int64_t a = (int64_t)x * 512, b = a + 512;
                    const int s0 = (int)(a / 597), s1 = (int)((b - 1) / 597);
                    if (s1 != s0 + 1 || i[s0] == i[s1]) blends_ok = false;
                    const int64_t wq = b - (int64_t)s1 * 597;   // the second's share
                    const uint32_t g = (uint32_t)(((i[s1] & 0xff) * wq + (i[s0] & 0xff) * (512 - wq) + 256) / 512);
                    if (v != (g << 16 | g << 8 | g)) blends_ok = false;
                }
                {   // an output column inside one source column is that column's pixel; each source column is under one
                    const int64_t a = (int64_t)x * 512;
                    if ((a / 597) == ((a + 511) / 597) && v != i[a / 597]) ends = false;
                    if (y == 0) seen[(size_t)(a / 597)] = seen[(size_t)((a + 511) / 597)] = true;
                }
            }
        }
        for (int x = 0; x < 512; x++)
            if (!seen[(size_t)x]) every = false;
        CHECK(flat && ends && blends_ok);
        CHECK(blends > 0);
        CHECK(every);   // (no source column left out)

        // square pixels (and within rounding): the frame as it is, bit for bit
        sc::shaped_size(512, 384, sc::pixel_shape(4.0 / 3.0, 1, 1, 512, 384), W, H);
        CHECK(W == 512 && H == 384);
        sc::shaped_size(640, 480, 1.0005, W, H);
        CHECK(W == 640 && H == 480);
        sc::shaped_size(640, 480, sc::pixel_shape(0, 1, 1, 640, 480), W, H);   // (0: square)
        CHECK(W == 640 && H == 480);
        const std::vector<uint32_t> sq = picture(512, 384, 7);
        CHECK(sc::save(sq.data(), 512, 384, 1.0, dir + "/shaped", "square", std::time(nullptr), out));
        CHECK(read_all(out) == hk::png::encode(sq.data(), 512, 384));
        // a size past 8192 when enlarged: kept to 8192
        sc::shaped_size(100, 8000, 0.5, W, H);
        CHECK(W == 100 && H == 8192);
    }

    if (g_fail) fprintf(stderr, "%d check(s) failed\n", g_fail);
    else printf("screenshots: all hold\n");
    return g_fail ? 1 : 0;
}
