// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher's drawn pieces (launcher/theme.h): an icon's path (third_party/mdi/icons.h) filled
// where its shape is and nowhere else, at any size; and the check boxes' switches, their pictures
// made and named by the style sheet.
#include <QApplication>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QRegularExpression>

#include <cstdio>

#include "theme.h"
#include "../third_party/mdi/icons.h"

static int g_failed = 0;
#define CHECK(x) \
    do { \
        if (!(x)) g_failed++, fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    } while (0)

// mdi::monitor at `size`: is the point (in the icon's 24 units) painted?
static bool monitorAt(int size, double x, double y) {
    QImage im(size, size, QImage::Format_ARGB32);
    im.fill(Qt::transparent);
    QPainter p(&im);
    theme::drawIcon(&p, mdi::monitor, QRectF(0, 0, size, size), Qt::red);
    p.end();
    return qAlpha(im.pixel(int(x * size / 24), int(y * size / 24))) > 128;
}

int main(int argc, char **argv) {
#ifndef _WIN32
    qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    QApplication app(argc, argv);
    for (int size : {24, 48}) {
        CHECK(monitorAt(size, 2, 10));     // the screen's left edge (x 1..3)
        CHECK(!monitorAt(size, 12, 10));   // the screen itself: a hole (its inner outline runs the other way)
        CHECK(monitorAt(size, 12, 19));    // its stand
        CHECK(!monitorAt(size, 4, 21));    // beside the foot
    }
    // a path not ours draws nothing, and doesn't stop
    QImage im(24, 24, QImage::Format_ARGB32);
    im.fill(Qt::transparent);
    {
        QPainter p(&im);
        theme::drawIcon(&p, "M 1 1 Q 3 3 4 4 Z", QRectF(0, 0, 24, 24), Qt::red);
    }
    CHECK(qAlpha(im.pixel(12, 12)) == 0);

    // the switches: four pictures (on, off, and each dimmed), at 1x and 2x
    const QString sheet = theme::switches(QColor("#ee2a3a"), QColor("#d6cce6"));
    QStringList files;
    for (const auto &m : QRegularExpression(R"(url\(([^)]+)\))").globalMatch(sheet)) files << m.captured(1);
    CHECK(files.size() == 4);
    for (const QString &f : files) {
        CHECK(QFile::exists(f));
        CHECK(QFile::exists(QString(f).replace(".png", "@2x.png")));
        CHECK(QImage(f).size() == QSize(36, 20));
    }
    // the "on" switch: its colour on the left, the white knob on the right
    if (files.size() == 4) {
        const QImage on(files[1]);
        CHECK(on.pixelColor(8, 10).red() > 200 && on.pixelColor(8, 10).green() < 80);
        CHECK(on.pixelColor(26, 10) == QColor(Qt::white));
    }
    CHECK(theme::mix(QColor(0, 0, 0), QColor(200, 100, 50), 0.5) == QColor(100, 50, 25));

    if (g_failed) fprintf(stderr, "%d failed\n", g_failed);
    return g_failed ? 1 : 0;
}
