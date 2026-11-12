// SPDX-License-Identifier: GPL-3.0-or-later
#include "cleaner.h"

#include <QDir>
#include <QRegularExpression>

namespace report {

namespace {

// A folder as the run's programs may write it: either separator, and a Linux one as Wine's Z: drive
QStringList spellings(const QString &path) {
    QString p = QDir::fromNativeSeparators(path);
    while (p.size() > 1 && p.endsWith('/')) p.chop(1);
    QStringList out{p, QString(p).replace('/', '\\')};
    if (p.startsWith('/')) out << "Z:" + p << "Z:" + QString(p).replace('/', '\\');
    return out;
}

// The text with each match of re replaced by what f makes of it
template <class F> QString each(const QString &s, const QRegularExpression &re, F f) {
    QString out;
    qsizetype at = 0;
    for (auto it = re.globalMatch(s); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        out += s.mid(at, m.capturedStart() - at);
        out += f(m);
        at = m.capturedEnd();
    }
    return out + s.mid(at);
}

// Folders that say nothing of the person, kept as they are
bool system_folder(const QString &p) {
    static const QRegularExpression sys(R"(^(/usr/|/opt/|/proc/|/sys/|/dev/|/etc/|/lib|/bin/|/run/user/\d+/?|[A-Za-z]:[\\/](Windows|Program Files|Program Files \(x86\)|ProgramData)[\\/]))",
                                        QRegularExpression::CaseInsensitiveOption);
    return sys.match(p).hasMatch();
}

}  // namespace

QString Cleaner::clean(const QString &text) {
    QString s = text;
    // HIRAKU's folder, then the home folder around it (the longer first)
    for (const QString &f : folders)
        for (const QString &p : spellings(f))
            if (p.size() > 3) s.replace(p, "<hiraku>", Qt::CaseInsensitive);
    for (const QString &f : homes)
        for (const QString &p : spellings(f))
            if (p.size() > 3) s.replace(p, "<home>", Qt::CaseInsensitive);
    // every other folder: its last part only (a Windows path stops at a space: a name with spaces may
    // leave its later words, which the preview shows)
    // (a folder on the way may have spaces in its name: "C:\\Users\\Some One\\Desktop\\...")
    static const QRegularExpression win(R"((?<![\w<>])[A-Za-z]:[\\/](?:[^\s\\/:*?"<>|;,()']+(?: [^\s\\/:*?"<>|;,()']+)*[\\/])*[^\s\\/:*?"<>|;,()']*)");
    static const QRegularExpression unix_(R"((?<![\w<>.:/~-])/(?:[^\s/;,"'()<>:]+(?: [^\s/;,"'()<>:]+)*/)+[^\s/;,"'()<>:]*)");
    static const QRegularExpression home(R"(<home>(?:[\\/][^\s\\/:*?"<>|;,()']+)*[\\/]?)");
    static const QRegularExpression drive(R"(^[A-Za-z]:[\\/]?$)");
    for (const QRegularExpression *re : {&win, &unix_, &home})
        s = each(s, *re, [](const QRegularExpressionMatch &m) {
            const QString p = m.captured(0);
            if (system_folder(p) || drive.match(p).hasMatch()) return p;   // (a drive alone: the game's own, as E:\\)
            const qsizetype cut = std::max(p.lastIndexOf('/', p.size() - 2), p.lastIndexOf('\\', p.size() - 2));
            const QString sep = p.contains('\\') ? "\\" : "/";
            return "<folder>" + sep + p.mid(cut + 1);
        });
    // the user's and the computer's names, as whole words (not a name that's also HIRAKU's own words)
    static const QStringList ours{"user", "users", "data", "game", "games", "hiraku", "admin", "home", "localhost"};
    for (int i = 0; i < names.size(); i++) {
        const QString &n = names[i];
        if (n.size() < 3 || ours.contains(n, Qt::CaseInsensitive)) continue;
        const QRegularExpression re("(?<![\\w])" + QRegularExpression::escape(n) + "(?![\\w])", QRegularExpression::CaseInsensitiveOption);
        s.replace(re, i == names.size() - 1 && names.size() > 1 ? "<computer>" : "<user>");
    }
    // e-AMUSEMENT cards: a number each, the same through the report
    // (in either case: the PC games' logs write them in small letters)
    static const QRegularExpression card(R"(\b(?:E004|012E)[0-9A-Fa-f]{12}\b)", QRegularExpression::CaseInsensitiveOption);
    s = each(s, card, [this](const QRegularExpressionMatch &m) {
        const QString k = m.captured(0).toUpper();
        if (!cards.contains(k)) cards.insert(k, (int)cards.size() + 1);
        return QString("#%1").arg(cards.value(k));
    });
    static const QRegularExpression pcbid(R"(\b[0-9A-Fa-f]{20}\b)");
    s.replace(pcbid, "<pcbid>");
    // a server's key, where one was typed as an option or set by hand (the launcher gives it neither way)
    static const QRegularExpression key(R"((--?eakey[ =]|HK_EA_KEY=)("[^"\n]*"|'[^'\n]*'|\S+))");
    s.replace(key, "\\1<key>");
    static const QRegularExpression mac(R"(\b(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}\b)");
    s.replace(mac, "<mac>");
    // addresses on the network: this computer's own (127.x), HIRAKU's NET TAISEN ones (10.57.x) kept
    static const QRegularExpression ip(R"(\b(?:\d{1,3}\.){3}\d{1,3}\b)");
    s = each(s, ip, [](const QRegularExpressionMatch &m) {
        const QString a = m.captured(0);
        return a.startsWith("127.") || a.startsWith("10.57.") || a == "0.0.0.0" ? a : QString("<address>");
    });
    // ... and in IPv6: eight groups, or fewer around a "::" (this computer's own, ::1, kept)
    static const QRegularExpression ip6(R"((?<![\w:.])(?:(?:[0-9A-Fa-f]{1,4}:){7}[0-9A-Fa-f]{1,4}|(?:[0-9A-Fa-f]{1,4}(?::[0-9A-Fa-f]{1,4}){0,6})?::(?:[0-9A-Fa-f]{1,4}(?::[0-9A-Fa-f]{1,4}){0,6})?)(?![\w:]))");
    s = each(s, ip6, [](const QRegularExpressionMatch &m) {
        const QString a = m.captured(0);
        return a == "::" || a == "::1" ? a : QString("<address>");
    });
    // A server's name: after http:// (what follows it too: a path or a query may hold a key), where
    // the options that take one give it without (-url server:8080, --eamuse server), and as
    // name:port anywhere. This computer and GitHub are kept
    static const auto kept = [](const QString &name) {
        const QString h = name.toLower();
        return h == "localhost" || h == "github.com" || h.endsWith(".github.com") || h.startsWith("127.");
    };
    // the server as the report names it (an address is <address> already), with its port; of what follows, only that there was something
    static const auto server = [](const QRegularExpressionMatch &m) {
        return m.captured(1) + (m.captured(2).startsWith('<') ? m.captured(2) : QString("<server>")) + m.captured(3) +
               (m.captured(4).size() > 1 ? "/..." : m.captured(4));
    };
    static const QRegularExpression host(R"((https?://)([A-Za-z0-9.-]+|<address>)((?::\d+)?)([^\s"'<>]*))");
    s = each(s, host, [](const QRegularExpressionMatch &m) { return kept(m.captured(2)) ? m.captured(0) : server(m); });
    static const QRegularExpression option(R"(((?:^|\s)--?(?:url|eamuse)[ =])([A-Za-z0-9.-]+|<address>)((?::\d+)?)([^\s"'<>]*))",
                                           QRegularExpression::MultilineOption);
    s = each(s, option, [](const QRegularExpressionMatch &m) {
        // (with http://: done above, "http" being all this one saw of it)
        return kept(m.captured(2)) || m.captured(2).compare("http", Qt::CaseInsensitive) == 0 || m.captured(2).compare("https", Qt::CaseInsensitive) == 0
                   ? m.captured(0) : server(m);
    });
    // (name:port: a name of two parts or more whose last is letters, and not a file's line, "core.cpp:120")
    static const QRegularExpression named(R"((?<![\w.<>/\\-])((?:[A-Za-z0-9-]+\.)+([A-Za-z]{2,})):(\d{2,5})(?![\w.]))");
    static const QStringList files{"c", "cc", "cpp", "h", "hpp", "py", "sh", "toml", "txt", "log", "bin", "dll", "exe", "so", "cue",
                                   "chd", "iso", "img", "json", "xml", "ifs", "lua", "frag", "vert", "glsl", "slang", "cmake", "rs", "js"};
    s = each(s, named, [](const QRegularExpressionMatch &m) {
        if (kept(m.captured(1)) || files.contains(m.captured(2).toLower())) return m.captured(0);
        return "<server>:" + m.captured(3);
    });
    return s;
}

}  // namespace report
