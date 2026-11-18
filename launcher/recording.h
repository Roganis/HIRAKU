// SPDX-License-Identifier: GPL-3.0-or-later
// Recording a game to a video file, the launcher's side (runtime/host/record.h; Settings > Display >
// Recording; File > Convert recordings for sharing): the options a game is started with, and the
// conversion's files and progress as the game's program prints it (--convert-recording).
#pragma once

#include <QByteArray>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace recording {

// The game's options for F6 (and --convert-recording): format "ffv1" (exact, lossless) or "h264"
// (for sharing); encoder "auto", "gpu" or "x264"; scale 0 (auto) or a whole number. The encoder and
// scale only matter to H.264, and go with the conversion whichever format the game records in
inline QStringList arguments(const QString &format, const QString &encoder, int scale, bool forConversion = false) {
    QStringList a;
    if (format == "h264" && !forConversion) a << "--record-format" << "h264";
    if (format == "h264" || forConversion)
        a << "--record-encoder" << (encoder.isEmpty() ? QString("auto") : encoder) << "--record-scale"
          << (scale > 0 ? QString::number(scale) : QString("auto"));
    return a;
}

// The files to convert: a recording's later parts (NAME-2.mkv...) left out when its first is among
// them (the conversion takes them with it), each once
inline QStringList firstParts(const QStringList &files) {
    QSet<QString> all;
    for (const QString &f : files) all.insert(QFileInfo(f).absoluteFilePath());
    static const QRegularExpression part("^(.*)-([2-9]|[1-9][0-9]+)\\.mkv$", QRegularExpression::CaseInsensitiveOption);
    QStringList out;
    QSet<QString> seen;
    for (const QString &f : files) {
        const QString abs = QFileInfo(f).absoluteFilePath();
        if (seen.contains(abs)) continue;
        seen.insert(abs);
        if (const QRegularExpressionMatch m = part.match(abs); m.hasMatch() && all.contains(m.captured(1) + ".mkv")) continue;
        out << f;
    }
    return out;
}

// The converter's "convert: N %" line: N, else -1
inline int progressOf(const QByteArray &line) {
    static const QRegularExpression re("^convert: (\\d+) %");
    const QRegularExpressionMatch m = re.match(QString::fromUtf8(line.trimmed()));
    return m.hasMatch() ? m.captured(1).toInt() : -1;
}

// How a conversion ended, by the converter's exit code (runtime/main.cpp --convert-recording): 0
// done, 3 done up to where a recording cut short or damaged could be read, anything else failed
enum class Result { Done, UpToDamage, Failed };
inline Result resultOf(bool normalExit, int code) {
    if (!normalExit) return Result::Failed;
    return code == 0 ? Result::Done : code == 3 ? Result::UpToDamage : Result::Failed;
}

// The MP4 a conversion writes for in (runtime/host/record.h's converted_name)
inline QString convertedName(const QString &in) {
    const QFileInfo f(in);
    return f.path() + "/" + f.completeBaseName() + ".mp4";
}

}  // namespace recording
