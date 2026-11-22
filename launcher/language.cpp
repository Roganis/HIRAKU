// SPDX-License-Identifier: GPL-3.0-or-later
#include "language.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QLibraryInfo>
#include <QLocale>
#include <QMap>

#include <cstdio>

namespace language {

namespace {

QString g_code = "en";

// The catalogue's msgctxt for Qt's (context, disambiguation)
std::string context(const char *ctx, const char *disambiguation) {
    std::string c = ctx ? ctx : "";
    if (disambiguation && *disambiguation) c += std::string("|") + disambiguation;
    return c;
}

}  // namespace

QString Translator::translate(const char *ctx, const char *sourceText, const char *disambiguation, int n) const {
    if (!sourceText) return {};
    const std::string c = context(ctx, disambiguation);
    const std::string *t = n >= 0 ? hk::lang::current().find_plural(c, sourceText, n) : hk::lang::current().find(c, sourceText);
    return t ? QString::fromStdString(*t) : QString();   // (empty: the next translator, then the English)
}

bool Translator::isEmpty() const { return hk::lang::current().size() == 0; }

QString trn(const char *ctx, const char *singular, const char *plural, long n) {
    const std::string *t = hk::lang::current().find_plural(ctx, singular, n);
    QString s = t ? QString::fromStdString(*t) : QString::fromUtf8(n == 1 ? singular : plural);
    return s.replace("%n", QString::number(n));
}

QString dir(const QString &root) { return root + "/lang"; }

QStringList available(const QString &root) {
    QStringList out;
    const std::vector<std::string> dirs = hk::lang::catalogue_dirs(root.toStdString());   // (the player's, the package's)
    for (const hk::lang::Language &l : hk::lang::languages())
        for (const std::string &d : dirs)
            if (QString(l.code) != "en" && QFileInfo::exists(QString::fromStdString(d) + "/" + l.code + ".po")) {
                out << l.code;
                break;
            }
    return out;
}

QStringList systemLanguages() { return QLocale::system().uiLanguages(); }

QString choose(const QString &saved, const QString &root) {
    const QByteArray given = qgetenv("HK_LANG");
    std::vector<std::string> system, have;
    for (const QString &s : systemLanguages()) system.push_back(s.toStdString());
    for (const QString &s : available(root)) have.push_back(s.toStdString());
    return QString::fromStdString(hk::lang::choose(given.isEmpty() ? saved.toStdString() : given.toStdString(), system, have));
}

QString install(QCoreApplication &app, const QString &code, const QString &root) {
    static Translator ours;
    static QTranslator qt;
    app.removeTranslator(&ours), app.removeTranslator(&qt);   // (a language before: none of it stays)
    g_code = "en";
    hk::lang::use(hk::lang::Catalogue(), "en");
    QLocale::setDefault(QLocale::system());
    if (code.isEmpty() || code == "en") return g_code;
    // the player's own <user data folder>/lang/<code>.po first, else the package's (a broken one of
    // the player's is said with its line, and passed over)
    hk::lang::Catalogue c;
    std::vector<std::string> said;
    const bool ok = hk::lang::load_for(QDir::fromNativeSeparators(root).toStdString(), code.toStdString(), c, said);
    for (const std::string &w : said) fprintf(stderr, "lang: %s\n", w.c_str());
    if (!ok) {
        fprintf(stderr, "lang: no %s catalogue could be read: in English\n", code.toUtf8().constData());
        return g_code;
    }
    const size_t texts = c.size();
    hk::lang::use(std::move(c), code.toStdString());
    g_code = code;
    fprintf(stderr, "lang: %s, %zu texts translated\n", code.toUtf8().constData(), texts);
    // Qt's own texts for those the catalogue hasn't: its qtbase_<code>.qm where this Qt has them
    // (a Linux system's Qt; not the static Windows build's), asked after the catalogue
    const QString qtName = code == "pt_PT" ? "qtbase_pt_BR" : "qtbase_" + code;   // (Qt has Brazil's only)
    for (const QString &d : {dir(root), QLibraryInfo::path(QLibraryInfo::TranslationsPath)})
        if (qt.load(qtName, d)) {
            app.installTranslator(&qt);
            break;
        }
    app.installTranslator(&ours);   // (the last installed is asked first)
    QLocale::setDefault(QLocale(code));   // numbers, dates and sizes as the language writes them
    return g_code;
}

QString current() { return g_code; }

QString cjkFamily(const QString &code) {
    const QFontDatabase::WritingSystem ws = code == "ja"      ? QFontDatabase::Japanese
                                            : code == "ko"    ? QFontDatabase::Korean
                                            : code == "zh_CN" ? QFontDatabase::SimplifiedChinese
                                            : code == "zh_TW" ? QFontDatabase::TraditionalChinese
                                                              : QFontDatabase::Any;
    if (ws == QFontDatabase::Any) return {};
    // the families this PC has for the language's writing system (fontconfig's, Windows' own), in
    // the order the language's own UI fonts come first: the Noto/Source Han family made for it, the
    // system's UI font for it (Windows: Yu Gothic UI, Malgun Gothic, Microsoft YaHei/JhengHei UI)
    const QStringList have = QFontDatabase::families(ws);
    static const QMap<QString, QStringList> preferred = {
        {"ja", {"Noto Sans CJK JP", "Noto Sans JP", "Source Han Sans JP", "Yu Gothic UI", "Meiryo UI", "Meiryo", "IPAexGothic", "IPAGothic", "TakaoGothic", "VL Gothic", "MS UI Gothic"}},
        {"ko", {"Noto Sans CJK KR", "Noto Sans KR", "Source Han Sans KR", "Malgun Gothic", "NanumGothic", "UnDotum", "Gulim"}},
        {"zh_CN", {"Noto Sans CJK SC", "Noto Sans SC", "Source Han Sans SC", "Source Han Sans CN", "Microsoft YaHei UI", "Microsoft YaHei", "WenQuanYi Micro Hei", "WenQuanYi Zen Hei", "SimHei"}},
        {"zh_TW", {"Noto Sans CJK TC", "Noto Sans TC", "Source Han Sans TC", "Source Han Sans TW", "Noto Sans CJK HK", "Microsoft JhengHei UI", "Microsoft JhengHei", "AR PL UMing TW", "PMingLiU"}},
    };
    for (const QString &want : preferred.value(code))
        for (const QString &f : have)
            if (!f.compare(want, Qt::CaseInsensitive)) return f;
    // else one whose name says the language's region ("... JP", "... TC")
    static const QMap<QString, QStringList> tags = {
        {"ja", {" JP", "Japanese"}}, {"ko", {" KR", "Korean"}}, {"zh_CN", {" SC", " CN", " GB"}}, {"zh_TW", {" TC", " TW", " HK"}}};
    for (const QString &tag : tags.value(code))
        for (const QString &f : have)
            if (f.contains(tag, Qt::CaseInsensitive)) return f;
    return have.value(0);   // (whatever has the writing system; none: the warning in Settings says so)
}

void useFamily(const QString &code) {
    const QString cjk = cjkFamily(code);
    if (cjk.isEmpty()) return;
    QFont f = QApplication::font();
    QStringList families = f.families();
    if (families.isEmpty()) families << f.family();
    families.removeAll(cjk);
    families.prepend(cjk);   // (the language's own first: the shared Han characters in its shapes)
    f.setFamilies(families);
    QApplication::setFont(f);
    fprintf(stderr, "lang: the interface's and the games' boxes' font: %s first\n", cjk.toUtf8().constData());
}

QString gameTexts() {
    QString out;
    for (const char *ctx : {"menu", "boxes", "overlay"}) out += QString::fromStdString(hk::lang::current().all_texts(ctx));
    return out;
}

}  // namespace language
