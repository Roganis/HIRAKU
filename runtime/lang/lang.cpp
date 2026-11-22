// SPDX-License-Identifier: GPL-3.0-or-later
#include "lang.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

#include "shell/userdir.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace hk::lang {

// --- the plural rule: a C expression of n -----------------------------------------------
// ?:, ||, &&, == != < > <= >=, + - * / %, !, (), n and numbers: what gettext's headers use
struct Catalogue::Rule {
    struct Node {
        char op;        // 'n', '#' (number), '?' (a b c), '!' (a), or a binary: | & = ~ (!=) < > l (<=) g (>=) + - * / %
        long value = 0;
        int a = -1, b = -1, c = -1;
    };
    std::vector<Node> nodes;
    int root = -1, nplurals = 1;

    // -- parsing --
    std::string_view s;
    size_t at = 0;
    bool bad = false;
    void space() {
        while (at < s.size() && isspace((unsigned char)s[at])) at++;
    }
    bool eat(std::string_view tok) {
        space();
        if (s.substr(at, tok.size()) == tok) return at += tok.size(), true;
        return false;
    }
    int node(char op, int a = -1, int b = -1, int c = -1, long v = 0) {
        nodes.push_back(Node{op, v, a, b, c});
        return (int)nodes.size() - 1;
    }
    int primary() {
        space();
        if (eat("(")) {
            const int e = ternary();
            if (!eat(")")) bad = true;
            return e;
        }
        if (eat("!")) return node('!', primary());
        if (eat("n")) return node('n');
        if (at < s.size() && isdigit((unsigned char)s[at])) {
            long v = 0;
            while (at < s.size() && isdigit((unsigned char)s[at])) v = v * 10 + (s[at++] - '0');
            return node('#', -1, -1, -1, v);
        }
        bad = true;
        return node('#');
    }
    int mul() {
        int l = primary();
        for (;;) {
            if (eat("*")) l = node('*', l, primary());
            else if (eat("/")) l = node('/', l, primary());
            else if (eat("%")) l = node('%', l, primary());
            else return l;
        }
    }
    int add() {
        int l = mul();
        for (;;) {
            if (eat("+")) l = node('+', l, mul());
            else if (eat("-")) l = node('-', l, mul());
            else return l;
        }
    }
    int rel() {
        int l = add();
        for (;;) {
            if (eat("<=")) l = node('l', l, add());
            else if (eat(">=")) l = node('g', l, add());
            else if (eat("<")) l = node('<', l, add());
            else if (eat(">")) l = node('>', l, add());
            else return l;
        }
    }
    int eq() {
        int l = rel();
        for (;;) {
            if (eat("==")) l = node('=', l, rel());
            else if (eat("!=")) l = node('~', l, rel());
            else return l;
        }
    }
    int land() {
        int l = eq();
        while (eat("&&")) l = node('&', l, eq());
        return l;
    }
    int lor() {
        int l = land();
        while (eat("||")) l = node('|', l, land());
        return l;
    }
    int ternary() {
        const int c = lor();
        if (!eat("?")) return c;
        const int a = ternary();
        if (!eat(":")) bad = true;
        return node('?', c, a, ternary());
    }
    // "nplurals=2; plural=(n > 1);"
    bool parse(std::string_view header) {
        nodes.clear(), bad = false;
        const size_t np = header.find("nplurals="), pl = header.find("plural=", np == std::string_view::npos ? 0 : np + 9);
        if (np == std::string_view::npos || pl == std::string_view::npos) return false;
        nplurals = atoi(std::string(header.substr(np + 9)).c_str());
        s = header.substr(pl + 7);
        const size_t end = s.find(';');
        if (end != std::string_view::npos) s = s.substr(0, end);
        at = 0;
        root = ternary();
        space();
        return !bad && at == s.size() && nplurals >= 1 && nplurals <= 6;
    }
    long eval(int i, long n) const {
        const Node &d = nodes[(size_t)i];
        switch (d.op) {
        case 'n': return n;
        case '#': return d.value;
        case '!': return !eval(d.a, n);
        case '?': return eval(d.a, n) ? eval(d.b, n) : eval(d.c, n);
        case '|': return eval(d.a, n) || eval(d.b, n);
        case '&': return eval(d.a, n) && eval(d.b, n);
        default: break;
        }
        const long a = eval(d.a, n), b = eval(d.b, n);
        switch (d.op) {
        case '=': return a == b;
        case '~': return a != b;
        case '<': return a < b;
        case '>': return a > b;
        case 'l': return a <= b;
        case 'g': return a >= b;
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': return b ? a / b : 0;
        case '%': return b ? a % b : 0;
        }
        return 0;
    }
    int form(long n) const {
        if (root < 0) return 0;
        const long f = eval(root, n < 0 ? -n : n);
        return f < 0 || f >= nplurals ? 0 : (int)f;
    }
};

int plural_form(const std::string &header_value, long n) {
    Catalogue::Rule r;
    return r.parse(header_value) ? r.form(n) : -1;
}

// --- reading a .po file ----------------------------------------------------------------------
namespace {

bool valid_utf8(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        const int more = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (more < 0 || i + more >= s.size() + (more ? 0 : 1)) return false;
        if (more && c < 0xc2 && more == 1) return false;   // (an overlong form)
        for (int k = 1; k <= more; k++)
            if (((unsigned char)s[i + (size_t)k] >> 6) != 2) return false;
        i += (size_t)more + 1;
    }
    return true;
}

// the %1..%9 a text uses, as a set of digits
std::string placeholders(std::string_view s) {
    std::set<char> got;
    for (size_t i = 0; i + 1 < s.size(); i++)
        if (s[i] == '%' && s[i + 1] >= '1' && s[i + 1] <= '9') got.insert(s[i + 1]);
    return std::string(got.begin(), got.end());
}

std::string key(std::string_view context, std::string_view text) {
    std::string k(context);
    k += '\x04';
    k += text;
    return k;
}

// a header's field ("Language: fr\n")
std::string field(const std::string &header, const char *name) {
    const std::string tag = std::string(name) + ":";
    for (size_t at = 0; at < header.size();) {
        size_t end = header.find('\n', at);
        if (end == std::string::npos) end = header.size();
        const std::string line = header.substr(at, end - at);
        if (line.compare(0, tag.size(), tag) == 0) {
            std::string v = line.substr(tag.size());
            while (!v.empty() && isspace((unsigned char)v.front())) v.erase(v.begin());
            while (!v.empty() && isspace((unsigned char)v.back())) v.pop_back();
            return v;
        }
        at = end + 1;
    }
    return {};
}

}  // namespace

Catalogue::Catalogue() = default;
Catalogue::~Catalogue() = default;
Catalogue::Catalogue(Catalogue &&) noexcept = default;
Catalogue &Catalogue::operator=(Catalogue &&) noexcept = default;

int Catalogue::plurals() const { return rule_ ? rule_->nplurals : 1; }
int Catalogue::plural_index(long n) const { return rule_ ? rule_->form(n) : 0; }

bool Catalogue::parse(std::string_view text, std::string &error, const std::string &name) {
    entries_.clear(), warnings_.clear(), language_.clear(), rule_.reset();
    if (text.size() >= 3 && !memcmp(text.data(), "\xef\xbb\xbf", 3)) text.remove_prefix(3);   // (a BOM)

    // An entry as it is read: its keywords' strings, and where it started
    struct Raw {
        std::string ctx, id, id_plural;
        std::vector<std::string> str;
        bool has_ctx = false, has_id = false, fuzzy = false;
        int line = 0;
    } e;
    std::string *into = nullptr;   // the string a continuation line adds to
    std::string header;
    bool header_seen = false;
    int lineno = 0;
    std::unordered_map<std::string, Entry> out;
    const auto fail = [&](int line, const std::string &why) {
        error = name + ":" + std::to_string(line) + ": " + why;
        entries_.clear();
        return false;
    };
    // an entry read whole: kept, or said why not
    const auto finish = [&]() -> bool {
        if (!e.has_id) {
            if (e.has_ctx || !e.str.empty()) return fail(e.line, "a msgstr or msgctxt without its msgid");
            return true;
        }
        if (e.str.empty()) return fail(e.line, "a msgid without its msgstr");
        if (e.id.empty() && !e.has_ctx) {   // the header
            if (header_seen) return fail(e.line, "a second header (an empty msgid)");
            header_seen = true, header = e.str[0];
        } else if (!e.fuzzy) {
            const bool plural = !e.id_plural.empty();
            bool empty = false;
            for (const std::string &s : e.str) empty |= s.empty();
            Entry en{e.str, plural, e.line};
            const std::string k = key(e.ctx, e.id);
            if (out.count(k)) return fail(e.line, "\"" + e.id + "\" twice in the same context");
            if (!empty) out.emplace(k, std::move(en));
        }
        e = Raw{};
        into = nullptr;
        return true;
    };

    std::istringstream in{std::string(text)};
    std::string line;
    while (std::getline(in, line)) {
        lineno++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t p = 0;
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) p++;
        if (p == line.size()) {   // a blank line ends an entry
            if (!finish()) return false;
            continue;
        }
        if (line[p] == '#') {
            if (line.compare(p, 2, "#~") == 0) continue;   // obsolete: not used
            if (e.has_id && !e.str.empty() && !finish()) return false;   // (a comment starts the next entry)
            if (line.compare(p, 2, "#,") == 0 && line.find("fuzzy", p) != std::string::npos) e.fuzzy = true;
            continue;   // (an entry's line is its first keyword's)
        }
        // a keyword, or a string continuing the last
        std::string word;
        if (line[p] != '"') {
            size_t w = p;
            while (w < line.size() && (isalnum((unsigned char)line[w]) || line[w] == '_' || line[w] == '[' || line[w] == ']')) w++;
            word = line.substr(p, w - p);
            p = w;
            while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) p++;
            if (p == line.size() || line[p] != '"') return fail(lineno, "a keyword without its \"string\": " + line);
        }
        // the string, C escapes
        std::string s;
        size_t q = p + 1;
        for (;; q++) {
            if (q >= line.size()) return fail(lineno, "a string not closed: " + line);
            const char c = line[q];
            if (c == '"') break;
            if (c != '\\') {
                s += c;
                continue;
            }
            if (++q >= line.size()) return fail(lineno, "a string not closed: " + line);
            switch (line[q]) {
            case 'n': s += '\n'; break;
            case 't': s += '\t'; break;
            case 'r': s += '\r'; break;
            case '"': s += '"'; break;
            case '\\': s += '\\'; break;
            default: return fail(lineno, std::string("an escape this reader doesn't know: \\") + line[q]);
            }
        }
        for (q++; q < line.size(); q++)
            if (line[q] != ' ' && line[q] != '\t') return fail(lineno, "something after the string: " + line);
        if (!valid_utf8(s)) return fail(lineno, "not UTF-8 (the catalogues are UTF-8)");
        if (word.empty()) {
            if (!into) return fail(lineno, "a string with no keyword before it");
            *into += s;
            continue;
        }
        if (!e.line) e.line = lineno;
        if (word == "msgctxt") {
            if (e.has_id && !e.str.empty() && !finish()) return false;
            if (!e.line) e.line = lineno;
            if (e.has_ctx || e.has_id) return fail(lineno, "msgctxt where it can't be");
            e.has_ctx = true, e.ctx = s, into = &e.ctx;
        } else if (word == "msgid") {
            if (e.has_id && !e.str.empty() && !finish()) return false;
            if (!e.line) e.line = lineno;
            if (e.has_id) return fail(lineno, "a msgid twice in an entry");
            e.has_id = true, e.id = s, into = &e.id;
        } else if (word == "msgid_plural") {
            if (!e.has_id || !e.id_plural.empty() || !e.str.empty()) return fail(lineno, "msgid_plural where it can't be");
            e.id_plural = s, into = &e.id_plural;
            if (e.id_plural.empty()) return fail(lineno, "an empty msgid_plural");
        } else if (word == "msgstr") {
            if (!e.has_id || !e.str.empty() || !e.id_plural.empty()) return fail(lineno, "msgstr where it can't be (a plural's are msgstr[0]..)");
            e.str.push_back(s), into = &e.str.back();
        } else if (word.compare(0, 7, "msgstr[") == 0 && word.back() == ']') {
            const int k = atoi(word.c_str() + 7);
            if (!e.has_id || e.id_plural.empty() || k != (int)e.str.size()) return fail(lineno, word + " where it can't be");
            e.str.push_back(s), into = &e.str.back();
        } else return fail(lineno, "a keyword this reader doesn't know: " + word);
    }
    lineno++;
    if (!finish()) return false;
    if (!header_seen) return fail(1, "no header (the entry with an empty msgid)");
    if (const std::string ct = field(header, "Content-Type"); !ct.empty() && ct.find("charset=UTF-8") == std::string::npos)
        return fail(1, "the header's charset isn't UTF-8: " + ct);
    language_ = field(header, "Language");
    if (const std::string pf = field(header, "Plural-Forms"); !pf.empty()) {
        rule_ = std::make_unique<Rule>();
        if (!rule_->parse(pf)) return fail(1, "a Plural-Forms this reader can't follow: " + pf);
        // (the rule's view of the header goes with the header: keep nothing pointing into it)
        rule_->s = {};
    }
    // each entry checked against the English: the same %1..%9, a plural's forms as many as the rule's
    std::unordered_map<std::string, Entry> kept;
    for (auto &[k, en] : out) {
        const size_t sep = k.find('\x04');
        const std::string id = k.substr(sep + 1);
        const std::string english = placeholders(id);
        std::string why;
        if (en.plural && (int)en.forms.size() != plurals())
            why = std::to_string(en.forms.size()) + " plural forms where the header says " + std::to_string(plurals());
        // (a plural's form may leave one out: French "un jeu" for "%n game"; never one of its own)
        for (const std::string &f : en.forms) {
            const std::string own = placeholders(f);
            const bool fits = en.plural ? std::includes(english.begin(), english.end(), own.begin(), own.end()) : own == english;
            if (why.empty() && !fits) why = "\"" + f + "\" hasn't the English's %1..%9 (\"" + id + "\"): kept in English";
        }
        if (why.empty()) kept.emplace(k, std::move(en));
        else warnings_.push_back(name + ":" + std::to_string(en.line) + ": " + why);
    }
    entries_ = std::move(kept);
    std::sort(warnings_.begin(), warnings_.end());
    return true;
}

bool Catalogue::load(const std::string &path, std::string &error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = path + ": can't be read";
        *this = Catalogue();
        return false;
    }
    std::ostringstream all;
    all << f.rdbuf();
    std::string name = path.substr(path.find_last_of("/\\") + 1);
    return parse(all.str(), error, name);
}

const std::string *Catalogue::find(std::string_view context, std::string_view text) const {
    const auto it = entries_.find(key(context, text));
    return it == entries_.end() || it->second.plural ? nullptr : &it->second.forms[0];
}

const std::string *Catalogue::find_plural(std::string_view context, std::string_view singular, long n) const {
    const auto it = entries_.find(key(context, singular));
    if (it == entries_.end()) return nullptr;
    if (!it->second.plural) return &it->second.forms[0];
    const size_t f = (size_t)plural_index(n);
    return f < it->second.forms.size() ? &it->second.forms[f] : nullptr;
}

std::string Catalogue::all_texts(std::string_view prefix) const {
    std::string out;
    for (const auto &[k, en] : entries_)
        if (k.compare(0, prefix.size(), prefix) == 0)
            for (const std::string &f : en.forms) out += f;
    return out;
}

std::string arg(std::string_view pattern, std::initializer_list<std::string_view> args, long n) {
    std::string out;
    out.reserve(pattern.size() + 16);
    for (size_t i = 0; i < pattern.size(); i++) {
        const char c = pattern[i], d = i + 1 < pattern.size() ? pattern[i + 1] : 0;
        if (c == '%' && d >= '1' && d <= '9' && (size_t)(d - '1') < args.size()) {
            out += args.begin()[d - '1'];
            i++;
        } else if (c == '%' && d == 'n') {
            out += std::to_string(n);
            i++;
        } else out += c;
    }
    return out;
}

// --- which language ----------------------------------------------------------------------------
const std::vector<Language> &languages() {
    static const std::vector<Language> all = {
        {"en", "English"},
        {"fr", "Fran\xc3\xa7" "ais"},
        {"de", "Deutsch"},
        {"es", "Espa\xc3\xb1ol"},
        {"it", "Italiano"},
        {"pt_PT", "Portugu\xc3\xaas (Portugal)"},
        {"ja", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"},
        {"ko", "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4"},
        {"zh_CN", "\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87"},
        {"zh_TW", "\xe7\xb9\x81\xe9\xab\x94\xe4\xb8\xad\xe6\x96\x87"},
    };
    return all;
}

namespace {

// "fr_CA.UTF-8@euro", "zh-Hant-HK", "pt-BR" -> language, script, region
struct Tag {
    std::string lang, script, region;
};
Tag split(std::string s) {
    for (const char cut : {'.', '@'})
        if (const size_t at = s.find(cut); at != std::string::npos) s.resize(at);
    std::replace(s.begin(), s.end(), '-', '_');
    Tag t;
    std::istringstream in(s);
    std::string part;
    for (int k = 0; std::getline(in, part, '_'); k++) {
        if (k == 0) {
            for (char &c : part) c = (char)tolower((unsigned char)c);
            t.lang = part;
        } else if (part.size() == 4 && t.script.empty() && t.region.empty()) {
            for (char &c : part) c = (char)tolower((unsigned char)c);
            part[0] = (char)toupper((unsigned char)part[0]);
            t.script = part;
        } else if ((part.size() == 2 || part.size() == 3) && t.region.empty()) {
            for (char &c : part) c = (char)toupper((unsigned char)c);
            t.region = part;
        }
    }
    return t;
}

}  // namespace

std::string choose(const std::string &saved, const std::vector<std::string> &system, const std::vector<std::string> &available) {
    const auto has = [&](const std::string &c) { return std::find(available.begin(), available.end(), c) != available.end(); };
    std::vector<std::string> wanted;
    if (!saved.empty() && saved != "system") wanted.push_back(saved);   // (the player's choice first)
    else wanted = system;
    for (const std::string &w : wanted) {
        const Tag t = split(w);
        if (t.lang.empty() || t.lang == "c" || t.lang == "posix") continue;
        if (t.lang == "en") return "en";   // (English before any other: the source itself)
        // exactly, as a catalogue is named: "pt_PT", "zh_TW"
        if (!t.region.empty() && has(t.lang + "_" + t.region)) return t.lang + "_" + t.region;
        if (t.lang == "zh") {   // the script, else the region, says which Chinese
            const bool trad = t.script == "Hant" || (t.script.empty() && (t.region == "TW" || t.region == "HK" || t.region == "MO"));
            if (const std::string c = trad ? "zh_TW" : "zh_CN"; has(c)) return c;
            continue;
        }
        if (has(t.lang)) return t.lang;
        // the language in another region: its catalogue (pt_BR -> pt_PT, for lack of another)
        for (const std::string &a : available)
            if (split(a).lang == t.lang) return a;
    }
    return "en";
}

std::vector<std::string> system_languages() {
    std::vector<std::string> out;
#ifdef _WIN32
    ULONG count = 0, size = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) && size) {
        std::vector<wchar_t> buf(size);
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buf.data(), &size))
            for (const wchar_t *p = buf.data(); *p; p += wcslen(p) + 1) {
                std::string s;
                for (const wchar_t *q = p; *q; q++) s += (char)(*q < 128 ? *q : '?');
                out.push_back(s);
            }
    }
#else
    // LANGUAGE (a list) counts only with a locale that isn't C, as gettext has it
    const char *all = getenv("LC_ALL"), *msg = getenv("LC_MESSAGES"), *lang = getenv("LANG");
    const char *locale = all && *all ? all : msg && *msg ? msg : lang && *lang ? lang : "";
    if (const char *list = getenv("LANGUAGE"); list && *list && *locale && strcmp(locale, "C") && strcmp(locale, "POSIX")) {
        std::istringstream in(list);
        for (std::string part; std::getline(in, part, ':');)
            if (!part.empty()) out.push_back(part);
    }
    if (*locale) out.push_back(locale);
#endif
    return out;
}

// --- the program's catalogue -----------------------------------------------------------
namespace {

struct Current {
    Catalogue cat;
    std::string code = "en";
};
Current &cur() {
    static Current c;
    return c;
}

std::vector<std::string> available_in(const std::vector<std::string> &dirs) {
    std::vector<std::string> out;
    for (const Language &l : languages())
        if (strcmp(l.code, "en"))
            for (const std::string &dir : dirs)
                if (FILE *f = fopen((dir + "/" + l.code + ".po").c_str(), "rb")) {
                    out.push_back(l.code), fclose(f);
                    break;
                }
    return out;
}

std::string read_text(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream all;
    if (f) all << f.rdbuf();
    return all.str();
}

}  // namespace

std::string saved_language(const std::string &root) {
    // QSettings' ini: the key ui/language is "language=" in the section [ui] ("" follows the system)
    std::istringstream in(read_text(hk::user_config_dir(root) + "/launcher.ini"));
    std::string line, section;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && line[0] == '[') section = line;
        else if (section == "[ui]" && line.rfind("language=", 0) == 0) {
            std::string v = line.substr(9);
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            return v;
        }
    }
    return {};
}

std::vector<std::string> catalogue_dirs(const std::string &root) {
    std::vector<std::string> out = {hk::user_data_dir(root) + "/lang"};
    if (root + "/lang" != out[0]) out.push_back(root + "/lang");
    return out;
}

bool load_for(const std::string &root, const std::string &code, Catalogue &out, std::vector<std::string> &said) {
    for (const std::string &dir : catalogue_dirs(root)) {
        const std::string path = dir + "/" + code + ".po";
        if (FILE *f = fopen(path.c_str(), "rb")) fclose(f);
        else continue;   // (not there: the next folder, quietly)
        Catalogue c;
        std::string error;
        if (!c.load(path, error)) {
            said.push_back(dir + "/" + error);
            continue;
        }
        for (const std::string &w : c.warnings()) said.push_back(dir + "/" + w);
        out = std::move(c);
        return true;
    }
    return false;
}

std::string start(const std::string &root) {
    const char *given = getenv("HK_LANG");
    // (HK_LANG: the launcher's choice; else, a game started on its own, the launcher's setting)
    const std::string wanted = given && *given ? given : saved_language(root);
    const std::string code = choose(wanted, system_languages(), available_in(catalogue_dirs(root)));
    if (code == "en") {
        // (a language asked for whose catalogue isn't there: said, not a silent English)
        if (const Tag t = split(wanted); !t.lang.empty() && t.lang != "en" && t.lang != "system" && t.lang != "c" && t.lang != "posix")
            fprintf(stderr, "lang: no catalogue for %s in %s: in English\n", wanted.c_str(), catalogue_dirs(root).back().c_str());
        use(Catalogue(), "en");
        return "en";
    }
    Catalogue c;
    std::vector<std::string> said;
    const bool ok = load_for(root, code, c, said);
    for (const std::string &w : said) fprintf(stderr, "lang: %s\n", w.c_str());
    if (!ok) {
        fprintf(stderr, "lang: no %s catalogue could be read: in English\n", code.c_str());
        use(Catalogue(), "en");
        return "en";
    }
    fprintf(stderr, "lang: %s, %zu texts translated\n", code.c_str(), c.size());
    use(std::move(c), code);
    return code;
}

void use(Catalogue c, const std::string &code) {
    // (once, at start, before the threads that draw: the entries' texts are handed out as they are)
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    cur().cat = std::move(c);
    cur().code = code;
}

const Catalogue &current() { return cur().cat; }
const std::string &code() { return cur().code; }

const char *tr(const char *context, const char *text) {
    const std::string *t = cur().cat.find(context, text);
    return t ? t->c_str() : text;
}

std::string trn(const char *context, const char *singular, const char *plural, long n) {
    const std::string *t = cur().cat.find_plural(context, singular, n);
    return arg(t ? *t : n == 1 ? singular : plural, {}, n);
}

// --- a text said in a log and shown on a screen ------------------------------------------------
// "\x01pattern\x02arg\x02arg": the pattern, then each argument after its \x02
namespace {
constexpr char kLater = '\x01', kArg = '\x02';
bool split_later(const std::string &s, std::string &pattern, std::vector<std::string> &args) {
    if (s.empty() || s[0] != kLater) return false;
    size_t at = 1, next = s.find(kArg, at);
    pattern = s.substr(at, next == std::string::npos ? std::string::npos : next - at);
    while (next != std::string::npos) {
        at = next + 1, next = s.find(kArg, at);
        args.push_back(s.substr(at, next == std::string::npos ? std::string::npos : next - at));
    }
    return true;
}
std::string put(std::string_view pattern, const std::vector<std::string> &args, bool keep, const char *context = nullptr) {
    std::string out;
    for (size_t i = 0; i < pattern.size(); i++) {
        const char c = pattern[i], d = i + 1 < pattern.size() ? pattern[i + 1] : 0;
        if (c == '%' && d >= '1' && d <= '9' && (size_t)(d - '1') < args.size()) {
            // (an argument that is itself a marked text, a medium's name, is shown translated too)
            const std::string v = context ? tr(context, args[(size_t)(d - '1')].c_str()) : args[(size_t)(d - '1')];
            out += keep ? "\x03" + v + "\x04" : v;
            i++;
        } else out += c;
    }
    return out;
}
}  // namespace

std::string later(const char *pattern, std::initializer_list<std::string_view> args) {
    std::string s(1, kLater);
    s += pattern;
    for (std::string_view a : args) s += kArg, s += a;
    return s;
}

std::string english(const std::string &s) {
    std::string pattern;
    std::vector<std::string> args;
    return split_later(s, pattern, args) ? put(pattern, args, false) : s;
}

std::string shown(const char *context, const std::string &s, bool keep) {
    std::string pattern;
    std::vector<std::string> args;
    if (!split_later(s, pattern, args)) return tr(context, s.c_str());
    return put(tr(context, pattern.c_str()), args, keep, context);
}

}  // namespace hk::lang
