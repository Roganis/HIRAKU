// SPDX-License-Identifier: GPL-3.0-or-later
#include "theme.h"

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>

#include "config.h"

namespace theme {

namespace {

Look g_look = System;
QString g_systemStyle;      // the style the launcher started with (System goes back to it)
QPalette g_systemPalette;

// a look's colours: the ground, the text and its quieter shade, lines, the six
struct Colours {
    QColor paper, base, alt, hover, ink, muted, line, accent, accentSoft;
    QColor six[6];
};

const Colours &colours() {
    static const Colours day{
        QColor("#fff8ee"), QColor("#ffffff"), QColor("#fff3e2"), QColor("#f3ebff"),
        QColor("#221547"), QColor("#8e86ae"), QColor("#ece2f7"), QColor("#6b2be0"), QColor("#e6dbff"),
        {QColor("#2457e8"), QColor("#17b24f"), QColor("#ffbf14"), QColor("#ff7a12"), QColor("#ee2a3a"), QColor("#7330e6")}};
    static const Colours night{
        QColor("#120d26"), QColor("#1a1433"), QColor("#1f1840"), QColor("#2a2154"),
        QColor("#f3eeff"), QColor("#948bc0"), QColor("#2e2556"), QColor("#8b52ff"), QColor("#3a2a73"),
        {QColor("#4a7bff"), QColor("#2bd268"), QColor("#ffd23a"), QColor("#ff8f33"), QColor("#ff4757"), QColor("#9a5cff")}};
    return g_look == Night ? night : day;
}

QPalette palette(const Colours &c) {
    QPalette p;
    for (QPalette::ColorGroup g : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        const bool off = g == QPalette::Disabled;
        p.setColor(g, QPalette::Window, c.paper);
        p.setColor(g, QPalette::WindowText, off ? c.muted : c.ink);
        p.setColor(g, QPalette::Base, c.base);
        p.setColor(g, QPalette::AlternateBase, c.alt);
        p.setColor(g, QPalette::Text, off ? c.muted : c.ink);
        p.setColor(g, QPalette::PlaceholderText, c.muted);
        p.setColor(g, QPalette::Button, c.base);
        p.setColor(g, QPalette::ButtonText, off ? c.muted : c.ink);
        p.setColor(g, QPalette::BrightText, Qt::white);
        p.setColor(g, QPalette::Highlight, off ? c.line : c.accent);
        p.setColor(g, QPalette::HighlightedText, Qt::white);
        p.setColor(g, QPalette::Link, c.six[0]);
        p.setColor(g, QPalette::LinkVisited, c.six[5]);
        p.setColor(g, QPalette::ToolTipBase, c.ink);
        p.setColor(g, QPalette::ToolTipText, c.paper);
        p.setColor(g, QPalette::Light, c.base);
        p.setColor(g, QPalette::Midlight, c.alt);
        p.setColor(g, QPalette::Mid, c.line);
        p.setColor(g, QPalette::Dark, c.muted);
        p.setColor(g, QPalette::Shadow, c.ink);
    }
    return p;
}

// a stripe of the six, hard-edged, left to right
QString stripe(const Colours &c) {
    QString s = "qlineargradient(x1:0, y1:0, x2:1, y2:0";
    for (int k = 0; k < 6; k++)
        s += QString(", stop:%1 %3, stop:%2 %3").arg(k / 6.0 + (k ? 0.0001 : 0), 0, 'f', 4)
                 .arg((k + 1) / 6.0, 0, 'f', 4).arg(c.six[k].name());
    return s + ")";
}

// the check boxes' tick, white (a style sheet takes a picture by its file: one made once, in a
// folder of its own that goes with the launcher)
QString tick() {
    static QTemporaryDir dir;
    const QString path = dir.filePath("tick.png");
    if (dir.isValid() && !QFile::exists(path)) {
        QPixmap pm(32, 32);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(Qt::white, 4.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(QPolygonF{QPointF(7, 16.5), QPointF(13.5, 23), QPointF(25, 9)});
        p.end();
        pm.save(path);
    }
    return path;
}

QString sheet(const Colours &c) {
    QString s = R"(
QMainWindow, QDialog { background: @paper; }
QMainWindow::separator { background: @paper; width: 6px; height: 6px; }

QMenuBar { background: @base; padding: 2px 4px; }
QMenuBar::item { padding: 4px 10px; border-radius: 7px; background: transparent; }
QMenuBar::item:selected { background: @hover; }
QMenu { background: @base; border: 2px solid @line; border-radius: 10px; padding: 6px; }
QMenu::item { padding: 5px 22px 5px 26px; border-radius: 7px; }
QMenu::item:selected { background: @accent; color: white; }
QMenu::item:disabled { color: @muted; }
QMenu::separator { height: 2px; background: @line; margin: 4px 8px; }

QMainWindow > QToolBar { background: @base; border: none; border-bottom: 5px solid; border-bottom-color: @stripe;
                         padding: 5px 6px 7px 6px; spacing: 2px; }
QToolBar::separator { width: 2px; background: @line; margin: 8px 4px; }
QToolBar QToolButton { border: none; border-radius: 11px; padding: 3px 6px; font-weight: 600; }
QToolBar QToolButton:hover { background: @hover; }
QToolBar QToolButton:pressed { background: @accentSoft; }
QToolBar QToolButton:disabled { color: @muted; }
QToolButton#hkPlay { background: @c1; color: white; border-bottom: 3px solid @c1dark; }
QToolButton#hkPlay:hover { background: @c1light; }
QToolButton#hkPlay:disabled { background: @line; color: @muted; border-bottom-color: @line; }
QToolButton#hkStop:enabled { background: @c4; color: white; border-bottom: 3px solid @c4dark; }
QToolButton#hkLogo, QToolButton#hkLogo:hover, QToolButton#hkLogo:pressed { background: transparent; padding: 0; }

QLineEdit { background: @base; border: 2px solid @line; border-radius: 9px; padding: 3px 8px;
            selection-background-color: @accent; selection-color: white; }
QLineEdit:focus { border-color: @accent; }
QCheckBox::indicator, QGroupBox::indicator { width: 14px; height: 14px; border: 2px solid @muted; border-radius: 5px; background: @base; }
QCheckBox::indicator:hover { border-color: @accent; }
QCheckBox::indicator:checked, QGroupBox::indicator:checked { background: @accent; border-color: @accent; image: url(@tick); }
QCheckBox::indicator:disabled { border-color: @line; background: @alt; }
QCheckBox::indicator:checked:disabled { background: @muted; border-color: @muted; }
QRadioButton::indicator { width: 14px; height: 14px; border: 2px solid @muted; border-radius: 9px; background: @base; }
QRadioButton::indicator:hover { border-color: @accent; }
QRadioButton::indicator:checked { border-color: @accent;
    background: qradialgradient(cx:0.5, cy:0.5, radius:0.5, fx:0.5, fy:0.5, stop:0 @accent, stop:0.5 @accent, stop:0.56 @base, stop:1 @base); }
QToolBar QLineEdit { border-radius: 15px; padding: 5px 12px; }

QPushButton { background: @base; border: 2px solid @line; border-radius: 10px; padding: 5px 14px; font-weight: 600; }
QPushButton:hover { border-color: @accent; }
QPushButton:pressed { background: @accentSoft; }
QPushButton:default { background: @accent; color: white; border-color: @accent; }
QPushButton:disabled { color: @muted; border-color: @line; background: @alt; }
QComboBox { background: @base; border: 2px solid @line; border-radius: 9px; padding: 3px 8px; }
QComboBox:hover, QComboBox:focus { border-color: @accent; }
QComboBox QAbstractItemView { background: @base; border: 2px solid @line; selection-background-color: @accent; selection-color: white; }

QTreeView, QListView, QTableView { background: @base; alternate-background-color: @alt; border: none; outline: 0; }
QTreeView::item, QListView::item { padding: 3px 2px; border: none; }
QTreeView::item:hover:!selected, QListView::item:hover:!selected { background: @hover; }
QTreeView::item:selected, QListView::item:selected { background: @accent; color: white; }
QHeaderView { background: @base; }
QHeaderView::section { background: @base; color: @muted; border: none; border-bottom: 2px solid @line; padding: 6px; font-weight: 700; }

QDockWidget { color: white; font-weight: 700; }
QDockWidget::title { padding: 5px 10px; margin: 4px 4px 0 4px; border-radius: 9px; background: @accent; }
QDockWidget#details::title { background: @c0; }
QDockWidget#music::title { background: @c3; }
QDockWidget#log::title { background: @c1; }
QDockWidget > QWidget { color: @ink; font-weight: normal; }
QTabBar::tab { background: @alt; color: @ink; border: none; border-radius: 9px; padding: 4px 14px; margin: 3px 2px; font-weight: 600; }
QTabBar::tab:hover { background: @hover; }
QTabBar::tab:selected { background: @accent; color: white; }
QTabWidget::pane { border: 2px solid @line; border-radius: 10px; background: @base; top: -1px; }

QGroupBox { border: 2px solid @line; border-radius: 10px; margin-top: 16px; padding: 10px 6px 6px 6px; font-weight: 700; }
QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 5px; color: @accent; }

QSlider::groove:horizontal { height: 6px; background: @line; border-radius: 3px; }
QSlider::sub-page:horizontal { background: @stripeSoft; border-radius: 3px; }
QSlider::handle:horizontal { background: white; border: 3px solid @accent; width: 8px; margin: -5px 0; border-radius: 7px; }
QSlider::handle:horizontal:hover { border-color: @c3; }

QScrollBar:vertical { background: transparent; width: 11px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 11px; margin: 2px; }
QScrollBar::handle { background: @line; border-radius: 3px; min-height: 28px; min-width: 28px; }
QScrollBar::handle:hover { background: @muted; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

QStatusBar { background: @base; color: @muted; border-top: 2px solid @line; }
QToolTip { background: @ink; color: @paper; border: none; padding: 4px 8px; }
QLabel[hkMuted="true"] { color: @muted; }

QToolButton { border: none; border-radius: 8px; padding: 3px; }
QToolButton:hover { background: @hover; }
QToolButton:checked { background: @accentSoft; color: @accent; font-weight: 700; }
QToolButton#hkMusicPlay { background: @c3; border-radius: 13px; padding: 4px; border-bottom: 2px solid @c3dark; }
QToolButton#hkMusicPlay:hover { background: @c3light; }
)";
    // (longest names first: @c1dark before @c1)
    for (int k = 0; k < 6; k++) {
        s.replace(QString("@c%1dark").arg(k), c.six[k].darker(125).name());
        s.replace(QString("@c%1light").arg(k), c.six[k].lighter(112).name());
        s.replace(QString("@c%1").arg(k), c.six[k].name());
    }
    s.replace("@stripeSoft", QString("qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 %1, stop:0.5 %2, stop:1 %3)")
                                 .arg(c.six[0].name(), c.six[5].name(), c.six[4].name()));
    s.replace("@stripe", stripe(c));
    s.replace("@tick", tick());
    s.replace("@accentSoft", c.accentSoft.name());
    for (auto [name, col] : {std::pair{"@paper", c.paper}, {"@base", c.base}, {"@alt", c.alt}, {"@hover", c.hover},
                             {"@ink", c.ink}, {"@muted", c.muted}, {"@line", c.line}, {"@accent", c.accent}})
        s.replace(name, col.name());
    return s;
}

}  // namespace

Look current() { return g_look; }
bool pop() { return g_look != System; }

QString key(Look look) { return look == Pop ? "pop" : look == Night ? "night" : "system"; }

void apply(Look look) {
    if (g_systemStyle.isEmpty()) {
        g_systemStyle = QApplication::style()->name();
        g_systemPalette = QApplication::palette();
    }
    g_look = look;
    if (look == System) {
        qApp->setStyleSheet(QString());
        QApplication::setStyle(QStyleFactory::create(g_systemStyle));
        QApplication::setPalette(g_systemPalette);
        return;
    }
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QApplication::setPalette(palette(colours()));
    qApp->setStyleSheet(sheet(colours()));
}

void applySaved() {
    QString k = qEnvironmentVariable("HK_THEME");
    if (k.isEmpty()) k = cfg::settings().value("ui/theme", "system").toString();
    apply(k == "pop" ? Pop : k == "night" ? Night : System);
}

QColor rainbow(int k) { return colours().six[std::clamp(k, 0, 5)]; }

QColor along(double t) {
    const double x = std::clamp(t, 0.0, 1.0) * 5;
    const int k = std::min(4, (int)x);
    const double f = x - k;
    const QColor a = rainbow(k), b = rainbow(k + 1);
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * f, a.greenF() + (b.greenF() - a.greenF()) * f,
                            a.blueF() + (b.blueF() - a.blueF()) * f);
}

QColor ready() { return pop() ? rainbow(1) : QColor(0x2e, 0x9e, 0x4a); }
QColor notable() { return g_look == Pop ? QColor("#e86a00") : g_look == Night ? rainbow(3) : QColor(0xc0, 0x80, 0x10); }

QColor badge(const QString &family, QColor fallback) {
    if (!pop()) return fallback;
    static const QColor pink("#ff3d8b"), sky("#1fa8f0"), teal("#12b39a"), indigo("#4b3bd6");
    if (family == "popn" || family == "djpstage") return pink;
    if (family == "djbm" || family == "gtrf") return rainbow(0);
    if (family == "djpopn") return rainbow(2);
    if (family == "pypopn" || family == "dmx") return sky;
    if (family == "pcpopn" || family == "pcpopnhc") return rainbow(1);
    if (family == "ppp" || family == "mambo") return rainbow(3);
    if (family == "kbm") return teal;
    if (family == "ddr" || family == "ddrsolo") return rainbow(5);
    if (family == "drmn") return rainbow(4);
    if (family == "iidx") return g_look == Night ? QColor("#3a2e7a") : QColor("#2a1f5c");
    if (family == "bm3") return indigo;
    return fallback;
}

QIcon glyph(const QString &what, QColor colour) {
    QIcon icon;
    for (int size : {16, 22, 32, 44, 64})
      for (QIcon::Mode mode : {QIcon::Normal, QIcon::Disabled}) {
        const QColor ink = mode == QIcon::Disabled ? colours().muted : colour;
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        const double u = size / 16.0;
        if (what == "play") {
            QPainterPath t;
            t.moveTo(4.5 * u, 2.5 * u);
            t.lineTo(13.5 * u, 8 * u);
            t.lineTo(4.5 * u, 13.5 * u);
            t.closeSubpath();
            p.setPen(QPen(ink, 1.5 * u, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPath(t);
        } else if (what == "pause") {
            p.drawRoundedRect(QRectF(3.5 * u, 2.5 * u, 3.2 * u, 11 * u), u, u);
            p.drawRoundedRect(QRectF(9.3 * u, 2.5 * u, 3.2 * u, 11 * u), u, u);
        } else if (what == "prev" || what == "next") {   // a bar and two arrows
            p.save();
            if (what == "prev") p.translate(size, 0), p.scale(-1, 1);
            for (double x0 : {2.0, 7.0}) {
                QPainterPath t;
                t.moveTo(x0 * u, 3.5 * u);
                t.lineTo((x0 + 5.5) * u, 8 * u);
                t.lineTo(x0 * u, 12.5 * u);
                t.closeSubpath();
                p.drawPath(t);
            }
            p.drawRoundedRect(QRectF(12.2 * u, 3.5 * u, 1.8 * u, 9 * u), 0.6 * u, 0.6 * u);
            p.restore();
        } else if (what == "speaker") {
            QPainterPath t;
            t.moveTo(2 * u, 6 * u);
            t.lineTo(5 * u, 6 * u);
            t.lineTo(9 * u, 2.5 * u);
            t.lineTo(9 * u, 13.5 * u);
            t.lineTo(5 * u, 10 * u);
            t.lineTo(2 * u, 10 * u);
            t.closeSubpath();
            p.drawPath(t);
            p.setPen(QPen(ink, 1.4 * u, Qt::SolidLine, Qt::RoundCap));
            p.setBrush(Qt::NoBrush);
            p.drawArc(QRectF(7 * u, 5 * u, 6 * u, 6 * u), -50 * 16, 100 * 16);
        } else {
            p.drawRoundedRect(QRectF(3 * u, 3 * u, 10 * u, 10 * u), 2 * u, 2 * u);
        }
        icon.addPixmap(pm, mode);
      }
    return icon;
}

QPixmap wordmark(int height, qreal dpr) {
    const QString word = "HIRAKU";
    static const double tilt[] = {-7, 4, -3, 6, -5, 3}, lift[] = {0, -0.05, 0.02, -0.07, 0, -0.03};
    QFont f = QApplication::font();
    f.setWeight(QFont::Black);
    f.setPixelSize(std::max(8, (int)std::lround(height * 0.80)));
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0);
    const QFontMetricsF fm(f);
    const double gap = height * 0.04, edge = height * 0.07;
    double width = edge * 2;
    for (QChar ch : word) width += fm.horizontalAdvance(ch) + gap;
    const double barH = height * 0.20, barTop = height - barH - 1, slant = barH * 0.9;
    width += slant;

    QPixmap pm(QSize((int)std::ceil(width), height) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const Colours &c = colours();

    // the bar, slanted at both ends, under the letters' feet
    QPainterPath bar;
    bar.moveTo(slant, barTop);
    bar.lineTo(width, barTop);
    bar.lineTo(width - slant, barTop + barH);
    bar.lineTo(0, barTop + barH);
    bar.closeSubpath();
    p.fillPath(bar, c.six[5].darker(g_look == Night ? 100 : 110));

    // the letters: a sticker's edge (the ground's colour), a darker shade under, then the colour
    const double base = barTop + barH * 0.55;
    double x = edge + slant * 0.5;
    for (int k = 0; k < word.size(); k++) {
        const double adv = fm.horizontalAdvance(word[k]);
        QPainterPath glyph;
        glyph.addText(-adv / 2, 0, f, QString(word[k]));
        const double cy = -fm.capHeight() / 2;
        p.save();
        p.translate(x + adv / 2, base + lift[k] * height);
        p.translate(0, cy);
        p.rotate(tilt[k]);
        p.translate(0, -cy);
        p.setPen(QPen(c.paper, height * 0.09, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(glyph.translated(height * 0.025, height * 0.035));
        p.drawPath(glyph);
        p.setPen(Qt::NoPen);
        p.fillPath(glyph.translated(height * 0.025, height * 0.035), c.six[k].darker(150));
        p.fillPath(glyph, c.six[k]);
        p.restore();
        x += adv + gap;
    }
    return pm;
}

}  // namespace theme
