// SPDX-License-Identifier: GPL-3.0-or-later
#include "aboutdialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "config.h"

namespace {

// A licence text: a whole file, or its first `lines` lines (a notice at the top of a source file)
struct Licence {
    const char *title, *path;
    int lines;
};

const Licence kLicences[] = {
    {"HIRAKU: GNU GPL v3", "LICENSE", 0},
    {"MAME: BSD-3-Clause", "third_party/mame/LICENSE", 0},
    {"Musashi: MIT", "third_party/musashi/LICENSE", 0},
    {"SoftFloat 2b (Musashi's FPU)", "third_party/musashi/softfloat/README.txt", 0},
    {"Berkeley SoftFloat 3: BSD-3-Clause", "third_party/mame/src/3rdparty/softfloat3/COPYING.txt", 0},
    {"minimp3: CC0", "third_party/mame/src/3rdparty/minimp3/minimp3.h", 8},
    {"libchdr: BSD-3-Clause", "third_party/libchdr/LICENSE.txt", 0},
    {"LZMA SDK (libchdr's LZMA decoder): public domain", "third_party/libchdr/lzma/LICENSE.txt", 0},
    {"crt-royale: GNU GPL v2", "third_party/slang-shaders/crt/shaders/crt-royale/LICENSE.TXT", 0},
    {"crt-guest-advanced-fast: GNU GPL v2 or later", "third_party/slang-shaders/crt/shaders/guest/fast/bloom_horizontal.slang", 22},
    {"Lola Retro: MIT", "third_party/lola-retro/LICENSE", 0},
    {"TinySoundFont (the launcher's music): MIT", "third_party/tinysoundfont/LICENSE", 0},
    {"Blur Busters CRT Beam Simulator: MIT", "third_party/crt-beam-simulator/LICENSE", 0},
    {"Wayland presentation-time: MIT", "third_party/wayland/presentation-time.xml", 26},
    {"Wayland fifo-v1: MIT", "third_party/wayland/fifo-v1.xml", 24},
    {"DXVK: zlib (Linux packages)", "third_party/dxvk/LICENSE", 0},
    {"Material Design Icons: Apache-2.0", "third_party/mdi/LICENSE", 0},
};

QString readLicence(const Licence &l) {
    QFile f(cfg::rootDir() + "/" + l.path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString("%1 was not found in the HIRAKU folder (%2).").arg(l.path, cfg::rootDir());
    QString text = QString::fromUtf8(f.readAll());
    if (l.lines > 0) text = text.section('\n', 0, l.lines - 1) + "\n";
    return text;
}

QString link(const char *url, const char *text) { return QString("<a href=\"%1\">%2</a>").arg(url, text); }

QString aboutHtml() {
    return QString(
               "<p>Konami arcade games as native programs, on Linux and Windows. Firebeat games run "
               "their PowerPC code recompiled (or, in a package, through HIRAKU's own analysis of it); "
               "DJ Main, System 573, Twinkle and Python run on the same shell, behind this launcher.</p>"
               "<p>HIRAKU ships no game data. It runs your own dumps.</p>"
               "<p>Copyright © 2026 Roganis</p>"
               "<p>HIRAKU is free software: you can redistribute it and/or modify it under the terms of the "
               "GNU General Public License as published by the Free Software Foundation, either version 3 "
               "of the License, or (at your option) any later version. It is distributed in the hope that "
               "it will be useful, but <b>without any warranty</b>; without even the implied warranty of "
               "merchantability or fitness for a particular purpose. See the Licences tab.</p>"
               "<p>Code taken from other projects keeps its own licence and notices: see Software and Licences.</p>"
               "<p>beatmania, pop'n music, Keyboardmania, ParaParaParadise, GuitarFreaks, DrumMania, "
               "Dance Dance Revolution, beatmania IIDX and e-AMUSEMENT are trademarks of Konami. "
               "HIRAKU is not affiliated with or endorsed by Konami.</p>"
               "<p><small>Built with Qt %1, running on Qt %2.</small></p>")
        .arg(QT_VERSION_STR, qVersion());
}

QString softwareHtml() {
    QString h;
    h += "<h3>Code in HIRAKU</h3><ul>";
    h += "<li><b>" + link("https://www.mamedev.org", "MAME") +
         "</b> (BSD-3-Clause, MAMEdev and contributors): the devices of the XT446 synth card, "
         "System 573, Twinkle and the multi-session unit, on HIRAKU's own shim of MAME's framework. "
         "Copyright holders are named in each file.</li>";
    h += "<li><b>" + link("https://github.com/987123879113/mame", "windyfairy's System 573 work") +
         "</b> (BSD-3-Clause): the multi-session unit, its TC9446F MP3 decoders, SIO1 as a UART, "
         "the TX3927 CPU.</li>";
    h += "<li><b>" + link("https://github.com/kstenerud/Musashi", "Musashi") +
         "</b> (MIT, Karl Stenerud): the 68000 of the Firebeat SPU and the 68EC020 of DJ Main.</li>";
    h += "<li><b>" + link("http://www.jhauser.us/arithmetic/SoftFloat.html", "SoftFloat") +
         "</b> (John R. Hauser): release 2b in Musashi; Berkeley SoftFloat 3 (BSD-3-Clause, "
         "The Regents of the University of California) in the MIPS CPUs.</li>";
    h += "<li><b>" + link("https://github.com/lieff/minimp3", "minimp3") +
         "</b> (CC0, lieff): the MP3 decoding of the 573's digital I/O board and the multi-session unit.</li>";
    h += "<li><b>" + link("https://github.com/rtissera/libchdr", "libchdr") +
         "</b> (BSD-3-Clause, Romain Tisserand and contributors), with " +
         link("https://github.com/mackron/dr_libs", "dr_flac") + " (public domain or MIT-0, David Reid) and the " +
         link("https://www.7-zip.org/sdk.html", "LZMA SDK") + "'s decoder (public domain, Igor Pavlov): "
         "MAME's CHDs read in place.</li>";
    h += "<li><b>" + link("https://github.com/libretro/slang-shaders", "libretro slang-shaders") +
         "</b> (GPL-2.0-or-later): crt-guest-advanced-fast (guest(r), with Dr. Venom) and "
         "crt-royale (TroggleMonkey), the picture's CRT shaders.</li>";
    h += "<li><b>" + link("https://github.com/zoran123456/lola-retro", "Lola Retro") +
         "</b> (MIT, Zoki): the analog CRT picture, the signal, the TV and the tube.</li>";
    h += "<li><b>" + link("https://github.com/blurbusters/crt-beam-simulator", "Blur Busters CRT Beam Simulator") +
         "</b> (MIT, Mark Rejhon and Timothy Lottes): a CRT's rolling scan on a fast display.</li>";
    h += "<li><b>" + link("https://gitlab.freedesktop.org/wayland/wayland-protocols", "wayland-protocols") +
         "</b> (MIT): presentation-time (Collabora) and fifo-v1 (Valve), for frame pacing.</li>";
    h += "<li><b>" + link("https://spice2x.github.io", "spice2x") +
         "</b> (GPL-3.0, the sp2xdev team and the SpiceTools contributors): the 64-bit PC loader's start-up "
         "of AVS2 games, its two-screen Direct3D 9, its board, card reader and licence-key stand-ins and "
         "its touch screen, written after spice2x's.</li>";
    h += "<li><b>" + link("https://github.com/mon/kbinxml", "kbinxml") +
         "</b> (MIT, mon): the binary XML of pop'n music 17 and 18's e-AMUSEMENT, written from its "
         "description of the format.</li>";
    h += "</ul>";

    h += "<h3>Libraries</h3><ul>";
    h += "<li><b>" + link("https://www.qt.io", "Qt 6") + "</b> (LGPL-3.0): this launcher.</li>";
    h += "<li><b>" + link("https://www.libsdl.org", "SDL 3") +
         "</b> (zlib): windows, sound, controllers and their LEDs.</li>";
    h += "<li><b>" + link("https://github.com/DiscoStarslayer/pcsx2-reliquary", "PCSX2 Reliquary") +
         "</b> (GPL-3.0-or-later), a fork of " + link("https://github.com/PCSX2/pcsx2", "PCSX2") +
         ": the emulator inside the Konami Python core.</li>";
    h += "<li><b>" + link("https://ffmpeg.org", "FFmpeg") +
         "</b> (LGPL-2.1-or-later; GPL-2.0-or-later as built with x264): Twinkle's background video and the "
         "games' video recordings, when found at build time.</li>";
    h += "<li><b>" + link("https://www.videolan.org/developers/x264.html", "x264") +
         "</b> (GPL-2.0-or-later): H.264 recordings on the processor, inside the packages' FFmpeg.</li>";
    h += "<li><b>" + link("https://github.com/FFmpeg/nv-codec-headers", "nv-codec-headers") + "</b> and <b>" +
         link("https://github.com/GPUOpen-LibrariesAndSDKs/AMF", "AMD AMF") +
         "</b> headers (MIT): H.264 recordings on NVIDIA and AMD graphics cards, with their drivers.</li>";
    h += "<li><b>" + link("https://github.com/KhronosGroup/Vulkan-Headers", "Vulkan-Headers") +
         "</b> (Apache-2.0 or MIT): H.264 recordings by Vulkan video (AMD and Intel cards), with their drivers.</li>";
    h += "<li><b>" + link("https://www.fluidsynth.org", "FluidSynth") +
         "</b> (LGPL-2.1-or-later): Keyboardmania's SoundFont stand-in for the XT446, when found at build time.</li>";
    h += "<li><b>" + link("https://wayland.freedesktop.org", "libwayland-client") + "</b> (MIT).</li>";
    h += "<li><b>" + link("https://pictogrammers.com/library/mdi", "Material Design Icons") +
         "</b> (Apache-2.0, Pictogrammers): the launcher's icons.</li>";
    h += "<li><b>" + link("https://github.com/doitsujin/dxvk", "DXVK") +
         "</b> (zlib, Philip Rebohle and contributors): Direct3D 9 on Vulkan for the 64-bit PC games "
         "under Wine, in the Linux packages.</li>";
    h += "</ul>";

    h += "<h3>References</h3><ul>";
    h += "<li><b>MAME</b>: the oracle every core is checked against, frame by frame and access by access.</li>";
    h += "<li><b>PCSX2 Reliquary</b>: the oracle for the Python core.</li>";
    h += "<li><b>" + link("https://ghidra-sre.org", "Ghidra") +
         "</b> (Apache-2.0, NSA): the analysis of each game program before translation.</li>";
    h += "<li><b>" + link("https://www.qemu.org", "QEMU") +
         "</b> (GPL-2.0): the reference for the PowerPC instruction tests.</li>";
    h += "<li><b>" + link("https://spice2x.github.io", "spice2x") +
         "</b> (GPL-3.0, the sp2xdev team and the SpiceTools contributors): the names and meanings of the "
         "e-AMUSEMENT options, and the real card readers' and the BIO2 board's dialogue (its reader/ and "
         "games/iidx/); its cardio (Felix) and PC/SC (nolm) readers (MIT) for what those readers send; "
         "for the PC pop'n games, the I/O board's and the card reader's DLL interfaces (ezusb, libacio) and "
         "the e-AMUSEMENT services they ask for.</li>";
    h += "<li><b>" + link("https://github.com/CrazyRedMachine/popnhax", "popnhax") +
         "</b> (CrazyRedMachine): the ideas behind presses judged when they happened, the board's samples "
         "for pop'n music 16-18, the PIN typed for the player and mods that replace text (popnhax has no "
         "licence: none of its code is in HIRAKU); and CrazyRedMachine's PopnForwarder and advice on "
         "the pop'n board.</li>";
    h += "<li><b>" + link("https://github.com/djhackersdev/bemanitools", "bemanitools") +
         "</b> (Unlicense): its ezusb2-popn emulation, a check of the pop'n board's packets.</li>";
    h += "<li><b>" + link("https://github.com/DragonMinded/bemaniutils", "bemaniutils") +
         "</b> (DragonMinded): a reference for e-AMUSEMENT servers and the later games' settings.</li>";
    h += "<li><b>" + link("https://remywiki.com", "RemyWiki") +
         "</b>: event codes, unlocks and song facts.</li>";
    h += "</ul>";
    h += "<p>Thanks to the testers, tyam, yaladre, " + link("https://github.com/MohaHtn", "MohaHtn") + " and " +
         link("https://github.com/CrazyRedMachine", "CrazyRedMachine") + ", who tried HIRAKU on their own "
         "PCs and cabinets, and to everyone who dumped, documented and preserved these machines.</p>";
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
    setWindowTitle("About HIRAKU");
    resize(640, 520);

    auto *tabs = new QTabWidget;
    tabs->addTab(browser(aboutHtml()), "About");
    tabs->addTab(browser(softwareHtml()), "Software");

    auto *licences = new QWidget;
    auto *which = new QComboBox;
    for (const Licence &l : kLicences) which->addItem(l.title);
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
    tabs->addTab(licences, "Licences");

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *aboutQt = buttons->addButton("About Qt", QDialogButtonBox::HelpRole);
    connect(aboutQt, &QPushButton::clicked, this, [this] { QMessageBox::aboutQt(this); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *icon = new QLabel;
    icon->setPixmap(windowIcon().pixmap(48, 48));
    auto *title = new QLabel("<b style='font-size:large'>HIRAKU</b><br>Konami arcade games, native");
    auto *top = new QHBoxLayout;
    top->addWidget(icon);
    top->addWidget(title, 1);

    auto *l = new QVBoxLayout(this);
    l->addLayout(top);
    l->addWidget(tabs);
    l->addWidget(buttons);
}
