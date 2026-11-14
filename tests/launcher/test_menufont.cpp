// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher's fonts for the game menu (launcher/menufont.h), read back by the menu's own reader
// (runtime/menu/font.h): the file is one, has ASCII and the text's other characters in three sizes,
// and a letter has ink. Where the PC has no font at all there is nothing to check.
//
//   test_menufont DIR [FILE TEXT]    with FILE: the font file for TEXT is left there (to look at
//                                    the menu with it: test_menu --ppm, HK_MENU_FONT)
#include <QFile>
#include <QFontInfo>
#include <QGuiApplication>
#include <QImage>

#include <cstdio>

#include "menu/font.h"
#include "menufont.h"

static int g_failed = 0;
#define CHECK(x) \
    do { \
        if (!(x)) g_failed++, fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    } while (0)

int main(int argc, char **argv) {
    if (argc < 2) return 2;
#ifndef _WIN32   // (no display needed; a Windows build has only its own platform, which needs none to draw into a picture)
    qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    QGuiApplication app(argc, argv);
    const QString text = argc > 3 ? QString::fromUtf8(argv[3]) : QString::fromUtf8("pop'n music 18 \xe3\x81\x9b\xe3\x82\x93\xe3\x81\x94\xe3\x81\x8f\xe5\x88\x97\xe4\xbc\x9d caf\xc3\xa9");
    const QByteArray file = menufont::build(app.font(), text);
    if (argc > 2) {
        QFile out(QString::fromLocal8Bit(argv[2]));
        if (out.open(QIODevice::WriteOnly)) out.write(file);
        if (argc > 4)   // and HIRAKU's name as the launcher hands it over, from the picture given
            menufont::logo(QImage(QString::fromLocal8Bit(argv[4]))).save(QString::fromLocal8Bit(argv[2]) + ".logo.bmp", "BMP");
    }
    hk::menu::Font font;
    std::string err;
    CHECK(font.parse((const uint8_t *)file.constData(), (size_t)file.size(), err));
    if (!font.loaded()) {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    printf("the font here: %s; %lld bytes for %lld characters\n", qPrintable(QFontInfo(app.font()).family()), (long long)file.size(), (long long)text.size());
    if (font.width(1, "HIRAKU") == 0) {
        printf("this PC has no font to draw with: not checked\n");
        return 0;
    }
    for (int size = 0; size < 3; size++) {
        CHECK(font.line(size) >= menufont::kPixels[size] / 2 && font.line(size) <= menufont::kPixels[size] * 3);
        CHECK(font.width(size, "HIRAKU") > menufont::kPixels[size] * 2);
        if (size) CHECK(font.width(size, "HIRAKU") > font.width(size - 1, "HIRAKU"));   // larger is wider
    }
    CHECK(font.width(1, "ii") < font.width(1, "MM"));   // not a fixed width
    // a letter has ink, a space none
    std::vector<uint32_t> px(128 * 64, 0xff000000u);
    font.draw(px.data(), 128, 64, 0, 0, 128, 64, 8, 8, 2, "H", 0xffffff, 255);
    int inked = 0;
    for (uint32_t p : px) inked += (p & 0xffffff) != 0;
    CHECK(inked > 40);
    std::fill(px.begin(), px.end(), 0xff000000u);
    font.draw(px.data(), 128, 64, 0, 0, 128, 64, 8, 8, 2, " ", 0xffffff, 255);
    CHECK(std::count(px.begin(), px.end(), 0xff000000u) == (long)px.size());
    // HIRAKU's name: the black around the letters is cut away
    {
        QImage name(200, 100, QImage::Format_RGB32);
        name.fill(Qt::black);
        for (int y = 30; y < 70; y++)
            for (int x = 20; x < 180; x++) name.setPixel(x, y, 0xffffffff);
        const QImage cut = menufont::logo(name);
        CHECK(cut.height() == menufont::kLogo && cut.width() == 160 * menufont::kLogo / 40 && qRed(cut.pixel(0, 0)) > 200);
        CHECK(menufont::logo(QImage()).isNull());
    }
    printf(g_failed ? "%d failed\n" : "ok\n", g_failed);
    return g_failed ? 1 : 0;
}
