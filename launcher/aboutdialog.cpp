// SPDX-License-Identifier: GPL-3.0-or-later
#include "aboutdialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "config.h"
#include "language.h"

namespace {

// A licence text: a whole file, or its first `lines` lines (a notice at the top of a source file)
struct Licence {
    const char *title, *path;
    int lines;
};

// (the titles' words in brackets are translated where shown; the names and licences' names stay)
const Licence kLicences[] = {
    {"HIRAKU: GNU GPL v3", "LICENSE", 0},
    {"MAME: BSD-3-Clause", "third_party/mame/LICENSE", 0},
    {"Musashi: MIT", "third_party/musashi/LICENSE", 0},
    {QT_TRANSLATE_NOOP("AboutDialog", "SoftFloat 2b (Musashi's FPU)"), "third_party/musashi/softfloat/README.txt", 0},
    {"Berkeley SoftFloat 3: BSD-3-Clause", "third_party/mame/src/3rdparty/softfloat3/COPYING.txt", 0},
    {"minimp3: CC0", "third_party/mame/src/3rdparty/minimp3/minimp3.h", 8},
    {"libchdr: BSD-3-Clause", "third_party/libchdr/LICENSE.txt", 0},
    {QT_TRANSLATE_NOOP("AboutDialog", "LZMA SDK (libchdr's LZMA decoder): public domain"), "third_party/libchdr/lzma/LICENSE.txt", 0},
    {"crt-royale: GNU GPL v2", "third_party/slang-shaders/crt/shaders/crt-royale/LICENSE.TXT", 0},
    {QT_TRANSLATE_NOOP("AboutDialog", "crt-guest-advanced-fast: GNU GPL v2 or later"), "third_party/slang-shaders/crt/shaders/guest/fast/bloom_horizontal.slang", 22},
    {"Lola Retro: MIT", "third_party/lola-retro/LICENSE", 0},
    {QT_TRANSLATE_NOOP("AboutDialog", "TinySoundFont (the launcher's music): MIT"), "third_party/tinysoundfont/LICENSE", 0},
    {QT_TRANSLATE_NOOP("AboutDialog", "Lua (mods' scripts): MIT"), "third_party/lua/LICENSE", 0},
    {"Blur Busters CRT Beam Simulator: MIT", "third_party/crt-beam-simulator/LICENSE", 0},
    {"Wayland presentation-time: MIT", "third_party/wayland/presentation-time.xml", 26},
    {"Wayland fifo-v1: MIT", "third_party/wayland/fifo-v1.xml", 24},
    {QT_TRANSLATE_NOOP("AboutDialog", "DXVK: zlib (Linux packages)"), "third_party/dxvk/LICENSE", 0},
    {"Material Design Icons: Apache-2.0", "third_party/mdi/LICENSE", 0},
};

QString readLicence(const Licence &l) {
    QFile f(cfg::rootDir() + "/" + l.path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        // TR: %1 is a licence file's path, %2 HIRAKU's folder
        return AboutDialog::tr("%1 was not found in the HIRAKU folder (%2).").arg(l.path, cfg::rootDir());
    QString text = QString::fromUtf8(f.readAll());
    if (l.lines > 0) text = text.section('\n', 0, l.lines - 1) + "\n";
    return text;
}

QString link(const char *url, const QString &text) { return QString("<a href=\"%1\">%2</a>").arg(url, text); }

// One line of the Software tab: a name (a link), who holds it under which licence, what it does here
QString item(const QString &name, const QString &holder, const QString &what) {
    // TR: one credit in About > Software: %1 the software's name, %2 its licence and holders, %3 what it does in HIRAKU
    return "<li>" + AboutDialog::tr("<b>%1</b> (%2): %3").arg(name, holder, what) + "</li>";
}
// ... a reference with no licence named
QString reference(const QString &name, const QString &what) {
    // TR: one reference in About > Software: %1 its name, %2 what it was used for
    return "<li>" + AboutDialog::tr("<b>%1</b>: %2").arg(name, what) + "</li>";
}

QString aboutHtml() {
    QString h = "<p>" +
                AboutDialog::tr("Konami arcade games as native programs, on Linux and Windows. Firebeat games run "
                                "their PowerPC code recompiled (or, in a package, through HIRAKU's own analysis of it); "
                                "DJ Main, System 573, Twinkle and Python run on the same shell, behind this launcher.") +
                "</p><p>" + AboutDialog::tr("HIRAKU ships no game data. It runs your own dumps.") + "</p>";
    h += "<p>Copyright © 2026 Roganis</p>";
    // (the licence notice is legal text: it stays in English, decision 5; a line says so above it)
    if (language::current() != "en")
        h += "<p><i>" + AboutDialog::tr("The licence notice below is legal text, kept in English.") + "</i></p>";
    h += "<p>HIRAKU is free software: you can redistribute it and/or modify it under the terms of the "
         "GNU General Public License as published by the Free Software Foundation, either version 3 "
         "of the License, or (at your option) any later version. It is distributed in the hope that "
         "it will be useful, but <b>without any warranty</b>; without even the implied warranty of "
         "merchantability or fitness for a particular purpose. See the Licences tab.</p>";
    h += "<p>" + AboutDialog::tr("Code taken from other projects keeps its own licence and notices: see Software and Licences.") + "</p>";
    h += "<p>" +
         AboutDialog::tr("%1 are trademarks of Konami. HIRAKU is not affiliated with or endorsed by Konami.")
             .arg(QLocale().createSeparatedList({"beatmania", "pop'n music", "Keyboardmania", "ParaParaParadise", "GuitarFreaks",
                                                 "DrumMania", "Dance Dance Revolution", "beatmania IIDX", "e-AMUSEMENT"})) +
         "</p>";
    // TR: %1 the Qt version HIRAKU was built with, %2 the one it runs on
    h += "<p><small>" + AboutDialog::tr("Built with Qt %1, running on Qt %2.").arg(QT_VERSION_STR, qVersion()) + "</small></p>";
    return h;
}

QString softwareHtml() {
    // TR: who holds a code's copyright, in About > Software: %1 a project's or person's name
    const auto contributors = [](const char *who) { return AboutDialog::tr("%1 and contributors").arg(who); };
    const QString spiceTeam = AboutDialog::tr("the sp2xdev team and the SpiceTools contributors");
    QString h;
    h += "<h3>" + AboutDialog::tr("Code in HIRAKU") + "</h3><ul>";
    h += item(link("https://www.mamedev.org", "MAME"), "BSD-3-Clause, " + contributors("MAMEdev"),
              AboutDialog::tr("the devices of the XT446 synth card, System 573, Twinkle and the multi-session unit, on "
                              "HIRAKU's own shim of MAME's framework. Copyright holders are named in each file."));
    // TR: %1 a person's online name
    h += item(link("https://github.com/987123879113/mame", AboutDialog::tr("%1's System 573 work").arg("windyfairy")),
              "BSD-3-Clause",
              AboutDialog::tr("the multi-session unit, its TC9446F MP3 decoders, SIO1 as a UART, the TX3927 CPU."));
    h += item(link("https://github.com/kstenerud/Musashi", "Musashi"), "MIT, Karl Stenerud",
              AboutDialog::tr("the 68000 of the Firebeat SPU and the 68EC020 of DJ Main."));
    h += item(link("http://www.jhauser.us/arithmetic/SoftFloat.html", "SoftFloat"), "John R. Hauser",
              // TR: %1 the licence and holders of SoftFloat 3 (names: keep them)
              AboutDialog::tr("release 2b in Musashi; Berkeley SoftFloat 3 (%1) in the MIPS CPUs.")
                  .arg("BSD-3-Clause, The Regents of the University of California"));
    h += item(link("https://github.com/lieff/minimp3", "minimp3"), "CC0, lieff",
              AboutDialog::tr("the MP3 decoding of the 573's digital I/O board and the multi-session unit."));
    h += "<li>" +
         // TR: %1 libchdr (a link), %2 its licence and holders, %3 dr_flac, %4 its holder, %5 the LZMA SDK, %6 its holder
         AboutDialog::tr("<b>%1</b> (%2), with %3 (public domain or MIT-0, %4) and the %5's decoder (public domain, %6): "
                         "MAME's CHDs read in place.")
             .arg(link("https://github.com/rtissera/libchdr", "libchdr"), "BSD-3-Clause, " + contributors("Romain Tisserand"),
                  link("https://github.com/mackron/dr_libs", "dr_flac"), "David Reid",
                  link("https://www.7-zip.org/sdk.html", "LZMA SDK"), "Igor Pavlov") +
         "</li>";
    h += item(link("https://github.com/libretro/slang-shaders", "libretro slang-shaders"), "GPL-2.0-or-later",
              // TR: %1 and %2 the shaders' authors' names (keep them)
              AboutDialog::tr("crt-guest-advanced-fast (%1) and crt-royale (%2), the picture's CRT shaders.")
                  .arg("guest(r), with Dr. Venom", "TroggleMonkey"));
    h += item(link("https://github.com/zoran123456/lola-retro", "Lola Retro"), "MIT, Zoki",
              AboutDialog::tr("the analog CRT picture, the signal, the TV and the tube."));
    h += item(link("https://github.com/blurbusters/crt-beam-simulator", "Blur Busters CRT Beam Simulator"),
              "MIT, Mark Rejhon and Timothy Lottes", AboutDialog::tr("a CRT's rolling scan on a fast display."));
    h += item(link("https://www.lua.org", "Lua 5.4"), "MIT, Lua.org, PUC-Rio", AboutDialog::tr("the language of mods' scripts."));
    h += item(link("https://gitlab.freedesktop.org/wayland/wayland-protocols", "wayland-protocols"), "MIT",
              // TR: %1 and %2 are the makers of two protocols (names: keep them)
              AboutDialog::tr("presentation-time (%1) and fifo-v1 (%2), for frame pacing.").arg("Collabora", "Valve"));
    h += item(link("https://spice2x.github.io", "spice2x"), "GPL-3.0, " + spiceTeam,
              AboutDialog::tr("the 64-bit PC loader's start-up of AVS2 games, its two-screen Direct3D 9, its board, card "
                              "reader and licence-key stand-ins and its touch screen, written after spice2x's."));
    h += item(link("https://github.com/mon/kbinxml", "kbinxml"), "MIT, mon",
              AboutDialog::tr("the binary XML of pop'n music 17 and 18's e-AMUSEMENT, written from its description of "
                              "the format."));
    h += "</ul>";

    h += "<h3>" + AboutDialog::tr("Libraries") + "</h3><ul>";
    h += item(link("https://www.qt.io", "Qt 6"), "LGPL-3.0", AboutDialog::tr("this launcher."));
    h += item(link("https://www.libsdl.org", "SDL 3"), "zlib", AboutDialog::tr("windows, sound, controllers and their LEDs."));
    h += "<li>" +
         // TR: %1 PCSX2 Reliquary (a link), %2 PCSX2 (a link)
         AboutDialog::tr("<b>%1</b> (GPL-3.0-or-later), a fork of %2: the emulator inside the Konami Python core.")
             .arg(link("https://github.com/DiscoStarslayer/pcsx2-reliquary", "PCSX2 Reliquary"),
                  link("https://github.com/PCSX2/pcsx2", "PCSX2")) +
         "</li>";
    h += item(link("https://ffmpeg.org", "FFmpeg"), AboutDialog::tr("LGPL-2.1-or-later; GPL-2.0-or-later as built with x264"),
              AboutDialog::tr("Twinkle's background video and the games' video recordings, when found at build time."));
    h += item(link("https://www.videolan.org/developers/x264.html", "x264"), "GPL-2.0-or-later",
              AboutDialog::tr("H.264 recordings on the processor, inside the packages' FFmpeg."));
    h += "<li>" +
         // TR: %1 nv-codec-headers (a link), %2 AMD AMF (a link)
         AboutDialog::tr("<b>%1</b> and <b>%2</b> headers (MIT): H.264 recordings on NVIDIA and AMD graphics cards, "
                         "with their drivers.")
             .arg(link("https://github.com/FFmpeg/nv-codec-headers", "nv-codec-headers"),
                  link("https://github.com/GPUOpen-LibrariesAndSDKs/AMF", "AMD AMF")) +
         "</li>";
    h += item(link("https://github.com/KhronosGroup/Vulkan-Headers", "Vulkan-Headers"), AboutDialog::tr("Apache-2.0 or MIT"),
              AboutDialog::tr("H.264 recordings by Vulkan video (AMD and Intel cards), with their drivers."));
    h += item(link("https://www.fluidsynth.org", "FluidSynth"), "LGPL-2.1-or-later",
              AboutDialog::tr("Keyboardmania's SoundFont stand-in for the XT446, when found at build time."));
    h += "<li><b>" + link("https://wayland.freedesktop.org", "libwayland-client") + "</b> (MIT).</li>";
    h += item(link("https://pictogrammers.com/library/mdi", "Material Design Icons"), "Apache-2.0, Pictogrammers",
              AboutDialog::tr("the launcher's icons."));
    h += item(link("https://github.com/doitsujin/dxvk", "DXVK"), "zlib, " + contributors("Philip Rebohle"),
              AboutDialog::tr("Direct3D 9 on Vulkan for the 64-bit PC games under Wine, in the Linux packages."));
    h += "</ul>";

    h += "<h3>" + AboutDialog::tr("References") + "</h3><ul>";
    h += reference("MAME", AboutDialog::tr("the oracle every core is checked against, frame by frame and access by access."));
    h += reference("PCSX2 Reliquary", AboutDialog::tr("the oracle for the Python core."));
    h += item(link("https://ghidra-sre.org", "Ghidra"), "Apache-2.0, NSA",
              AboutDialog::tr("the analysis of each game program before translation."));
    h += item(link("https://www.qemu.org", "QEMU"), "GPL-2.0", AboutDialog::tr("the reference for the PowerPC instruction tests."));
    h += item(link("https://spice2x.github.io", "spice2x"), "GPL-3.0, " + spiceTeam,
              // TR: %1 and %2 are people's online names (keep them); reader/ and games/iidx/ are folders
              AboutDialog::tr("the names and meanings of the e-AMUSEMENT options, and the real card readers' and the BIO2 "
                              "board's dialogue (its reader/ and games/iidx/); its cardio (%1) and PC/SC (%2) readers (MIT) "
                              "for what those readers send; for the PC pop'n games, the I/O board's and the card reader's "
                              "DLL interfaces (ezusb, libacio) and the e-AMUSEMENT services they ask for.")
                  .arg("Felix", "nolm"));
    h += item(link("https://github.com/CrazyRedMachine/popnhax", "popnhax"), "CrazyRedMachine",
              // TR: %1 a person's online name (keep it)
              AboutDialog::tr("the ideas behind presses judged when they happened, the board's samples for pop'n music "
                              "16-18, the PIN typed for the player and mods that replace text (popnhax has no licence: "
                              "none of its code is in HIRAKU); and %1's PopnForwarder and advice on the pop'n board.")
                  .arg("CrazyRedMachine"));
    h += item(link("https://github.com/djhackersdev/bemanitools", "bemanitools"), "Unlicense",
              AboutDialog::tr("its ezusb2-popn emulation, a check of the pop'n board's packets."));
    h += item(link("https://github.com/DragonMinded/bemaniutils", "bemaniutils"), "DragonMinded",
              AboutDialog::tr("a reference for e-AMUSEMENT servers and the later games' settings."));
    h += reference(link("https://remywiki.com", "RemyWiki"), AboutDialog::tr("event codes, unlocks and song facts."));
    h += "</ul>";
    h += "<p>" +
         // TR: %1 the testers' names, as a list ("a, b and c")
         AboutDialog::tr("Thanks to the testers, %1, who tried HIRAKU on their own PCs and cabinets, and to everyone who "
                         "dumped, documented and preserved these machines.")
             .arg(QLocale().createSeparatedList({"tyam", "yaladre", link("https://github.com/MohaHtn", "MohaHtn"),
                                                  link("https://github.com/CrazyRedMachine", "CrazyRedMachine")})) +
         "</p>";
    return h;
}

QTextBrowser *browser(const QString &html) {
    auto *b = new QTextBrowser;
    b->setOpenExternalLinks(true);
    b->setHtml(html);
    return b;
}

}  // namespace

AboutDialog::AboutDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("About HIRAKU"));
    resize(640, 520);

    auto *tabs = new QTabWidget;
    tabs->addTab(browser(aboutHtml()), tr("About"));
    tabs->addTab(browser(softwareHtml()), tr("Software"));

    auto *licences = new QWidget;
    auto *which = new QComboBox;
    for (const Licence &l : kLicences) which->addItem(tr(l.title));
    auto *text = new QPlainTextEdit;
    text->setReadOnly(true);
    text->setFont(QFont("monospace"));
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    connect(which, &QComboBox::currentIndexChanged, text,
            [text](int i) { text->setPlainText(i >= 0 ? readLicence(kLicences[i]) : QString()); });
    text->setPlainText(readLicence(kLicences[0]));
    auto *ll = new QVBoxLayout(licences);
    ll->addWidget(which);
    ll->addWidget(text);
    tabs->addTab(licences, tr("Licences"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *aboutQt = buttons->addButton(tr("About Qt"), QDialogButtonBox::HelpRole);
    connect(aboutQt, &QPushButton::clicked, this, [this] { QMessageBox::aboutQt(this); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *icon = new QLabel;
    icon->setPixmap(windowIcon().pixmap(48, 48));
    auto *title = new QLabel("<b style='font-size:large'>HIRAKU</b><br>" + tr("Konami arcade games, native"));
    auto *top = new QHBoxLayout;
    top->addWidget(icon);
    top->addWidget(title, 1);

    auto *l = new QVBoxLayout(this);
    l->addLayout(top);
    l->addWidget(tabs);
    l->addWidget(buttons);
}
