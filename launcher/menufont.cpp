// SPDX-License-Identifier: GPL-3.0-or-later
#include "menufont.h"

#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace menufont {

namespace {

void u32(QByteArray &out, quint32 v) {
    for (int k = 0; k < 4; k++) out.append((char)(v >> (8 * k) & 0xff));
}
void u16(QByteArray &out, int v) {
    out.append((char)(v & 0xff));
    out.append((char)(v >> 8 & 0xff));
}

}  // namespace

Sizes times(const Sizes &s, double scale) {
    Sizes out = s;
    for (int k = 0; k < 3; k++) out.pixels[k] = std::max(1, (int)std::lround(s.pixels[k] * std::max(1.0, scale)));
    return out;
}

QByteArray build(const QFont &base, const QString &text, const Sizes &sizes, const QList<char32_t> &drawn, const Painter &paint) {
    // the characters: ASCII, then the text's others, each once
    QList<char32_t> chars;
    QSet<char32_t> seen;
    for (char32_t c = 32; c < 127; c++) chars << c, seen << c;
    for (const char32_t c : text.toUcs4())
        if (c >= 160 && !seen.contains(c)) chars << c, seen << c;
    for (const char32_t c : drawn)
        if (paint && !seen.contains(c)) chars << c, seen << c;

    QByteArray out("HKMF1\0\0\0", 8), ink;
    u32(out, (quint32)sizes.count);
    for (int size = 0; size < sizes.count; size++) {
        QFont font(base);
        font.setPixelSize(sizes.pixels[size]);
        font.setWeight(sizes.weight[size]);
        font.setStyleStrategy(QFont::StyleStrategy(QFont::PreferAntialias | QFont::NoSubpixelAntialias));
        const QFontMetrics fm(font);
        const int line = std::clamp(fm.height(), 1, 256), pad = line;   // (room for what hangs over a character's box)
        u32(out, (quint32)line);
        u32(out, (quint32)chars.size());
        for (const char32_t c : chars) {
            const QString one = QString::fromUcs4(&c, 1);
            const bool own = drawn.contains(c) && paint;   // (an icon: a square as high as the line)
            const int advance = own ? line : std::clamp(fm.horizontalAdvance(one), 0, 512);
            // drawn white on nothing, its line's top `pad` down: what is inked, and where
            QImage img(advance + 2 * pad, line + 2 * pad, QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::transparent);
            {
                QPainter p(&img);
                p.setRenderHint(QPainter::TextAntialiasing);
                p.setFont(font);
                p.setPen(Qt::white);
                if (own) {
                    p.setRenderHint(QPainter::Antialiasing);
                    const double inset = line * 0.06;
                    paint(p, QRectF(pad + inset, pad + inset, line - 2 * inset, line - 2 * inset), c);
                } else p.drawText(pad, pad + fm.ascent(), one);
            }
            int x0 = img.width(), y0 = img.height(), x1 = -1, y1 = -1;
            for (int y = 0; y < img.height(); y++) {
                const QRgb *row = (const QRgb *)img.constScanLine(y);
                for (int x = 0; x < img.width(); x++)
                    if (qAlpha(row[x])) x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
            }
            const int w = x1 < 0 ? 0 : x1 - x0 + 1, h = x1 < 0 ? 0 : y1 - y0 + 1;
            u32(out, (quint32)c);
            u16(out, advance);
            u16(out, w ? x0 - pad : 0);
            u16(out, w ? y0 - pad : 0);
            u16(out, w);
            u16(out, h);
            u32(out, (quint32)ink.size());
            for (int y = 0; y < h; y++) {
                const QRgb *row = (const QRgb *)img.constScanLine(y0 + y);
                for (int x = 0; x < w; x++) ink.append((char)qAlpha(row[x0 + x]));
            }
        }
    }
    u32(out, (quint32)ink.size());
    return out + ink;
}

QImage logo(const QImage &picture, int height) {
    const QImage grey = picture.convertToFormat(QImage::Format_Grayscale8);
    int x0 = grey.width(), y0 = grey.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < grey.height(); y++) {
        const uchar *row = grey.constScanLine(y);
        for (int x = 0; x < grey.width(); x++)
            if (row[x] > 48) x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    }
    if (x1 < 0) return {};
    return picture.copy(x0, y0, x1 - x0 + 1, y1 - y0 + 1).scaledToHeight(std::max(1, height), Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB888);
}

}  // namespace menufont
