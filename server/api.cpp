// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-server's JSON API (api.h): the routes, who is calling, the limits, and the shapes of what
// goes in and out. The one file that includes the HTTP library, and with names.cpp the JSON one (third_party/).
#include "api.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#define CPPHTTPLIB_THREAD_POOL_COUNT 4
#include <httplib.h>   // (no TLS: a reverse proxy in front makes it; docs/SERVER.md)
#include <json.hpp>

#include "accounts.h"
#include "names.h"
#include "eamuse/games.h"
#include "eamuse/listener.h"
#include "shell/sys.h"

#if !defined(_WIN32) && __has_include(<iconv.h>)
#include <iconv.h>
#define HK_API_ICONV 1
#endif

namespace hk::srv {

namespace {

namespace ea = hk::ea;
using json = nlohmann::json;
using httplib::Request;
using httplib::Response;

constexpr size_t BODY_MAX = 64 * 1024;
constexpr const char *COOKIE = "hk_session";
constexpr const char *JSON_TYPE = "application/json; charset=utf-8";

// --- text

// JSON's text: never invalid UTF-8 (a byte that isn't is replaced: a name the command line gave an
// arcade, a setting's file)
std::string dump(const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

bool valid_utf8(const std::string &s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        size_t n = 0;
        uint32_t cp = 0;
        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) n = 1, cp = c & 0x1f;
        else if ((c & 0xf0) == 0xe0) n = 2, cp = c & 0x0f;
        else if (c >= 0xf0 && c <= 0xf4) n = 3, cp = c & 0x07;
        else return false;
        if (i + n >= s.size()) return false;
        for (size_t k = 1; k <= n; k++) {
            const unsigned char d = (unsigned char)s[i + k];
            if ((d & 0xc0) != 0x80) return false;
            cp = cp << 6 | (d & 0x3f);
        }
        if ((n == 2 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) || (n == 3 && (cp < 0x10000 || cp > 0x10ffff))) return false;
        i += n + 1;
    }
    return true;
}

std::string hex(const std::string &s) {
    static const char *h = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) out += h[c >> 4], out += h[c & 15];
    return out;
}

// A name as a game keeps it (its own encoding, mostly Shift-JIS) as UTF-8; false: it can't be
#ifdef HK_API_ICONV
bool from_shift_jis(const std::string &in, std::string &out) {
    iconv_t cd = iconv_open("UTF-8", "CP932");
    if (cd == (iconv_t)-1) cd = iconv_open("UTF-8", "SHIFT_JIS");
    if (cd == (iconv_t)-1) return false;
    std::string src = in;
    out.assign(in.size() * 4 + 4, '\0');
    char *ip = src.data(), *op = out.data();
    size_t il = src.size(), ol = out.size();
    const bool ok = iconv(cd, &ip, &il, &op, &ol) != (size_t)-1 && il == 0;
    iconv_close(cd);
    out.resize(out.size() - ol);
    return ok && valid_utf8(out);
}
#else
bool from_shift_jis(const std::string &, std::string &) { return false; }   // (Windows: later)
#endif

// obj[key]: the name as text; one that can't be made text: "" and its bytes as hex beside (key_hex)
void name_field(json &obj, const std::string &key, const std::string &bytes) {
    std::string text;
    const bool ascii = std::all_of(bytes.begin(), bytes.end(), [](char c) { return (unsigned char)c >= 0x20 && (unsigned char)c < 0x7f; });
    if (ascii) text = bytes;
    else if (!from_shift_jis(bytes, text) && !valid_utf8(text = bytes)) text.clear();
    for (char c : text)   // (a control character isn't a name's)
        if ((unsigned char)c < 0x20 || c == 0x7f) text.clear();
    obj[key] = text;
    if (text.empty() && !bytes.empty()) obj[key + "_hex"] = hex(bytes);
}

std::string lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool same(const std::string &a, const std::string &b) {   // in a time that doesn't tell where they differ
    unsigned char d = a.size() == b.size() ? 0 : 1;
    for (size_t i = 0; i < a.size(); i++) d |= (unsigned char)(a[i] ^ (i < b.size() ? b[i] : 0));
    return d == 0;
}

bool whole_number(const std::string &s, long &v, long min, long max) {
    if (s.empty() || s.size() > 18) return false;
    char *end = nullptr;
    v = std::strtol(s.c_str(), &end, 10);
    return !*end && v >= min && v <= max && (std::isdigit((unsigned char)s[0]) || s[0] == '-');
}

json when(int64_t t) { return t ? json(t) : json(nullptr); }

// --- the log: a line a request (Api::start), its start and its user kept by the thread answering it
thread_local std::chrono::steady_clock::time_point t_began{};
thread_local std::string t_user;

// A request's path for the log: no query (httplib's path has none), a card's number as *
std::string logged_path(const std::string &path) {
    std::string p;
    for (const char ch : path.substr(0, 200)) p += ch > ' ' && ch < 127 ? ch : '?';
    for (size_t at = p.find("/cards/"); at != std::string::npos; at = p.find("/cards/", at + 1)) {
        const size_t from = at + 7, to = p.find('/', from);
        if (from < p.size()) p.replace(from, to == std::string::npos ? std::string::npos : to - from, "*");
    }
    return p;
}

// --- answers

void answer(Response &res, const json &j, int status = 200) {
    res.status = status;
    res.set_content(dump(j), JSON_TYPE);
}

void fail(Response &res, int status, const char *code, const std::string &message) {
    answer(res, json{{"error", code}, {"message", message}}, status);
}

// An accounts' refusal as an HTTP status and an error code, by its kind (accounts.h); its sentence
// the message. A refused link says the tries left before the card is locked, and whether it is
void refused(Response &res, const Refusal &why) {
    struct As {
        int status;
        const char *code;
    };
    const As as = [&]() -> As {
        switch (why.code) {
        case Refused::ok:
        case Refused::invalid: return {400, "bad_request"};
        case Refused::incomplete: return {400, "bad_request"};   // (only when nothing was done: the PIN reset's own answer else)
        case Refused::not_found: return {404, "not_found"};
        case Refused::not_allowed: return {403, "not_allowed"};
        case Refused::not_signed_in: return {401, "not_signed_in"};
        case Refused::wrong_password: return {401, "wrong_password"};
        case Refused::old_password: return {403, "wrong_password"};
        case Refused::disabled: return {403, "disabled"};
        case Refused::stats_hidden: return {403, "stats_hidden"};
        case Refused::sign_up: return {403, "signup_refused"};
        case Refused::bad_code: return {403, "bad_code"};
        case Refused::locked: return {403, "card_locked"};
        case Refused::wrong_pin: return {403, "wrong_pin"};
        case Refused::conflict:
        case Refused::too_many: return {409, "conflict"};
        case Refused::read_only: return {503, "read_only"};
        case Refused::failed: return {500, "server_error"};
        }
        return {500, "server_error"};
    }();
    json j{{"error", as.code}, {"message", why.text}};
    if (why.tries_left >= 0) j["tries_left"] = why.tries_left;
    if (why.code == Refused::locked && why.tries_left >= 0) j["locked"] = true;
    answer(res, j, as.status);
}

// --- limits on how often, in memory: a count a window, by key

class Limits {
public:
    // 0: go on; else the seconds until the key may again
    long hit(const std::string &key, int most, long seconds) {
        std::lock_guard<std::mutex> l(mu_);
        const long t = now();
        if (seen_.size() > 50000)   // (old windows forgotten)
            for (auto it = seen_.begin(); it != seen_.end();) it = t - it->second.start >= 3600 ? seen_.erase(it) : std::next(it);
        Window &w = seen_[key];
        if (w.count == 0 || t - w.start >= seconds) w = {t, 0};
        if (w.count >= most) return std::max(1L, seconds - (t - w.start));
        w.count++;
        return 0;
    }

private:
    struct Window {
        long start = 0;
        int count = 0;
    };
    static long now() { return (long)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    std::mutex mu_;
    std::unordered_map<std::string, Window> seen_;
};

bool limited(Limits &l, Response &res, const std::string &key, int most, long seconds) {
    const long wait = l.hit(key, most, seconds);
    if (!wait) return false;
    res.set_header("Retry-After", std::to_string(wait));
    fail(res, 429, "slow_down", "too many tries: again in " + std::to_string(wait) + " s");
    return true;
}

// --- a request's body

// Its fields read by type; the first that isn't what it should be is said (bad)
struct In {
    const json &j;
    std::string bad;
    bool has(const char *k) const { return j.contains(k) && !j[k].is_null(); }
    std::string text(const char *k, bool need = true) {
        if (!has(k)) {
            if (need && bad.empty()) bad = std::string("\"") + k + "\" is needed";
            return "";
        }
        if (!j[k].is_string()) {
            if (bad.empty()) bad = std::string("\"") + k + "\" is a text";
            return "";
        }
        return j[k].get<std::string>();
    }
    bool flag(const char *k, bool &v) {
        if (!has(k)) return false;
        if (!j[k].is_boolean()) {
            if (bad.empty()) bad = std::string("\"") + k + "\" is true or false";
            return false;
        }
        v = j[k].get<bool>();
        return true;
    }
    bool number(const char *k, long &v, long min, long max) {
        if (!has(k)) return false;
        if (!j[k].is_number_integer() || j[k].get<long long>() < min || j[k].get<long long>() > max) {
            if (bad.empty()) bad = std::string("\"") + k + "\" is a whole number from " + std::to_string(min) + " to " + std::to_string(max);
            return false;
        }
        v = (long)j[k].get<long long>();
        return true;
    }
};

// --- who is calling

enum class Need { anyone, user, op, admin };
enum class Auth { none, session, token };

struct Caller {
    Who who = Who::anyone();
    Auth auth = Auth::none;
    bool must_change = false, write = false;
    std::string csrf, session;   // a session's (its CSRF value, its token as the cookie gave it)
    long token = 0;
    std::string address;
};

std::string cookie(const Request &req, const char *name) {
    const std::string all = req.get_header_value("Cookie");
    const std::string want = std::string(name) + "=";
    for (size_t at = 0; at < all.size();) {
        size_t e = all.find(';', at);
        if (e == std::string::npos) e = all.size();
        std::string part = all.substr(at, e - at);
        part.erase(0, part.find_first_not_of(' '));
        if (part.rfind(want, 0) == 0) return part.substr(want.size());
        at = e + 1;
    }
    return "";
}

std::string game_id(const ea::Game &g) { return *g.id ? g.id : g.model; }

const ea::Game *listed_game(const Accounts &acc, const std::string &id) {
    const ea::Game *g = ea::game_by_id(id);
    return g && *g->model && acc.listed(*g) ? g : nullptr;
}

json user_json(const User &u) {
    return {{"name", u.name},           {"role", role_name(u.role)},           {"disabled", u.disabled},
            {"hide_stats", u.hide_stats}, {"must_change_password", u.must_change}, {"created", u.created}};
}
json arcade_json(const Arcade &a) {
    return {{"id", a.id}, {"name", a.name}, {"prefecture", a.pref}, {"owner", a.owner.empty() ? json(nullptr) : json(a.owner)}};
}
json machine_json(const Machine &m) {
    return {{"pcbid", m.pcbid}, {"arcade", m.arcade}, {"name", m.name}, {"enabled", m.enabled}, {"has_key", m.keyed}, {"last_seen", when(m.last_seen)}};
}
json maybe(long v) { return v < 0 ? json(nullptr) : json(v); }

json stats_json(const std::vector<Stat> &stats) {
    json out = json::array();
    for (const Stat &s : stats) {
        const ea::Game *g = ea::game_by_id(s.game);
        json o{{"game", s.game}, {"title", g ? g->title : s.game}, {"plays", maybe(s.profile.plays)}};
        if (!s.card.empty()) o["card"] = s.card;   // (the card's user's, or an admin's: accounts says)
        name_field(o, "player", s.profile.name);
        json scores = json::array();
        for (const ea::Score &sc : s.scores)
            scores.push_back({{"song", sc.song}, {"chart", sc.chart}, {"score", maybe(sc.score)}, {"clear", maybe(sc.clear)}, {"plays", maybe(sc.plays)}});
        o["scores"] = std::move(scores);
        out.push_back(std::move(o));
    }
    return out;
}

// A setting as a page builds a form from it: its declaration, and the values
json setting_json(const ea::Setting *d, const SettingLine &l, bool arcade) {
    json o{{"name", l.name}, {"default", l.by_default}};
    if (d) {
        o["label"] = d->label, o["help"] = d->help, o["kind"] = d->kind == ea::Setting::number ? "number" : "text";
        if (d->kind == ea::Setting::number) o["min"] = d->min, o["max"] = d->max;
    }
    if (arcade) o["value"] = l.arcade.empty() ? json(nullptr) : json(l.arcade), o["server_value"] = l.server.empty() ? json(nullptr) : json(l.server);
    else o["value"] = l.server.empty() ? json(nullptr) : json(l.server);
    return o;
}

json games_settings_json(const Accounts &acc, const std::vector<SettingLine> &lines, bool arcade) {
    json games = json::array();
    std::string last;
    for (const SettingLine &l : lines) {
        const ea::Game *g = ea::game_by_id(l.game);
        if (!g || !acc.listed(*g)) continue;
        if (games.empty() || last != l.game) {
            games.push_back({{"id", l.game}, {"title", g->title}, {"settings", json::array()}});
            last = l.game;
        }
        games.back()["settings"].push_back(setting_json(ea::find_setting(g, l.name.c_str()), l, arcade));
    }
    return games;
}

// --- the web pages (web/): the files of one folder, by a plain relative path, never anything else

// on every page's answer: nothing but the server's own (no inline script or style), no framing
void page_headers(Response &res) {
    res.set_header("Content-Security-Policy", "default-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
    res.set_header("X-Content-Type-Options", "nosniff");
    res.set_header("Referrer-Policy", "same-origin");
    res.set_header("Cache-Control", "no-cache");
}

void page_missing(Response &res) {
    page_headers(res);
    res.status = 404;
    res.set_content("not found\n", "text/plain; charset=utf-8");
}

// a file's type by its ending; null: not one served
const char *web_type(const std::string &name) {
    static const char *const TYPES[][2] = {
        {".html", "text/html; charset=utf-8"}, {".js", "text/javascript; charset=utf-8"}, {".css", "text/css; charset=utf-8"},
        {".svg", "image/svg+xml"},             {".png", "image/png"},                      {".ico", "image/x-icon"},
        {".json", "application/json; charset=utf-8"}, {".txt", "text/plain; charset=utf-8"}};
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) return nullptr;
    const std::string end = lower(name.substr(dot));
    for (const auto &t : TYPES)
        if (end == t[0]) return t[1];
    return nullptr;
}

// A request's path (decoded) as a file of the folder: "" when it isn't one that may be (a part
// empty, "." or ".." or a dot-file, a character other than a letter, digit, '.', '_' or '-')
std::string web_relative(const std::string &path) {
    if (path.empty() || path[0] != '/' || path.size() > 256) return "";
    const std::string rel = path.substr(1);
    if (rel.empty()) return "index.html";
    for (size_t at = 0;;) {
        size_t e = rel.find('/', at);
        if (e == std::string::npos) e = rel.size();
        if (e == at || rel[at] == '.') return "";
        for (size_t k = at; k < e; k++)
            if (!std::isalnum((unsigned char)rel[k]) && rel[k] != '.' && rel[k] != '_' && rel[k] != '-') return "";
        if (e == rel.size()) return rel;
        at = e + 1;
    }
}

// The file's bytes, when it is a plain file inside the folder (a link out of it isn't)
bool web_file(const std::string &root, const std::string &rel, std::string &body) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path file = fs::path(root) / rel;
    if (!fs::is_regular_file(file, ec) || fs::file_size(file, ec) > 8u << 20 || ec) return false;
    const fs::path real = fs::canonical(file, ec), base = fs::canonical(root, ec);
    if (ec) return false;
    const fs::path inside = real.lexically_relative(base);
    if (inside.empty() || *inside.begin() == "..") return false;
    std::ifstream in(real, std::ios::binary);
    body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

// The pages' folder when --web named none: the first with an index.html (api.h says which)
std::string find_web() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string exe = hk::sys::exe_path();
    if (exe.empty()) return "";
    const fs::path dir = fs::path(exe).parent_path();
    std::vector<fs::path> tries{dir / "web", dir / ".." / "share" / "hiraku-server" / "web"};
    std::ifstream cache(dir / "CMakeCache.txt");   // (a build tree's: the checkout it is built from)
    const std::string home = "CMAKE_HOME_DIRECTORY:INTERNAL=";
    for (std::string line; std::getline(cache, line);)
        if (line.rfind(home, 0) == 0) tries.push_back(fs::path(line.substr(home.size())) / "web");
    for (const fs::path &t : tries)
        if (fs::is_regular_file(t / "index.html", ec)) return fs::weakly_canonical(t, ec).string();
    return "";
}

}  // namespace

int ApiOptions::option(int argc, char **argv, int &i) {
    const std::string a = argv[i];
    if (a == "--no-api") enabled = false;
    else if (a == "--api-insecure-cookies") insecure_cookies = true;
    else if (a == "--api-trust-proxy") trust_proxy = true;
    else if (a == "--no-web") web_enabled = false;
    else if (a == "--web") {
        if (i + 1 >= argc) { fprintf(stderr, "--web needs a folder\n"); return 2; }
        web = argv[++i];
    } else if (a == "--names") {
        if (i + 1 >= argc) { fprintf(stderr, "--names needs a folder\n"); return 2; }
        names = argv[++i];
    } else if (a == "--api") {
        if (i + 1 >= argc) { fprintf(stderr, "--api needs ADDRESS:PORT\n"); return 2; }
        const std::string v = argv[++i];
        const size_t colon = v.rfind(':');
        long p = 0;
        if (colon == std::string::npos || colon == 0 || !whole_number(v.substr(colon + 1), p, 1, 65535)) {
            fprintf(stderr, "--api: ADDRESS:PORT (127.0.0.1:8081)\n");
            return 2;
        }
        host = v.substr(0, colon), port = (int)p;
        if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    } else {
        return 0;
    }
    return 1;
}

struct Api::Impl {
    Accounts acc;   // the API's own connection to the database (accounts.h), one request's at a time
    std::mutex one;
    ApiOptions o;
    Limits limits;
    httplib::Server http;
    std::thread thread;
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    Names names;   // the song names' packs (names.h): the API's only

    Impl(const std::string &file, ea::Store &store, const ApiOptions &opt)
        : acc(file, store), o(opt), names(opt.names.empty() ? names_beside(file) : opt.names, opt.quiet) {}
    static std::string names_beside(const std::string &db) {
        const std::filesystem::path p = std::filesystem::path(db).parent_path();
        return ((p.empty() ? std::filesystem::path(".") : p) / "names").string();
    }
    bool has_names(const ea::Game &g) {
        const auto p = names.pack(game_id(g));
        return p && !p->songs.empty();
    }

    std::string address(const Request &req) const {
        if (o.trust_proxy && req.has_header("X-Forwarded-For")) {
            std::string v = req.get_header_value("X-Forwarded-For");
            v = v.substr(v.rfind(',') == std::string::npos ? 0 : v.rfind(',') + 1);
            v.erase(0, v.find_first_not_of(' '));
            while (!v.empty() && v.back() == ' ') v.pop_back();
            if (!v.empty()) return v;
        }
        return req.remote_addr;
    }

    // Who calls (one held): a bearer token first, else the session's cookie. "": known (or nobody
    // said who); else why not
    std::string identify(const Request &req, Caller &c) {
        const std::string auth = req.get_header_value("Authorization");
        if (!auth.empty()) {
            if (auth.rfind("Bearer ", 0) != 0) return "Authorization: Bearer TOKEN";
            bool write = false;
            if (!acc.token(auth.substr(7), c.who, c.must_change, write, c.token).empty()) return "not a token (or revoked, or its user disabled)";
            c.auth = Auth::token, c.write = write;
            return "";
        }
        const std::string s = cookie(req, COOKIE);
        if (s.empty()) return "";
        if (!acc.session(s, c.who, c.must_change, c.csrf).empty()) return "not signed in (or the session ended)";
        c.auth = Auth::session, c.write = true, c.session = s;
        return "";
    }

    using Handler = std::function<void(Caller &, const Request &, In &, Response &)>;
    struct Rule {
        Need need = Need::user;
        bool session_only = false;    // not with a personal token
        bool while_changing = false;  // allowed to a user who must change the password first
    };

    void route(const char *method, const std::string &path, Rule rule, Handler h) {
        auto run = [this, rule, h, method](const Request &req, Response &res) {
            res.set_header("Cache-Control", "no-store");
            res.set_header("X-Content-Type-Options", "nosniff");
            try {
                handle(method, rule, h, req, res);
            } catch (const std::exception &) {
                fail(res, 500, "server_error", "the server could not answer that");
            }
        };
        const std::string p = "/api/v1" + path;
        if (!std::strcmp(method, "GET")) http.Get(p, run);
        else if (!std::strcmp(method, "POST")) http.Post(p, run);
        else if (!std::strcmp(method, "PUT")) http.Put(p, run);
        else http.Delete(p, run);
    }

    void handle(const char *method, const Rule &rule, const Handler &h, const Request &req, Response &res) {
        const bool changes = std::strcmp(method, "GET") != 0;
        Caller c;
        c.address = address(req);
        json body = json::object();
        if (changes && !req.body.empty()) {
            body = json::parse(req.body, nullptr, false);
            if (body.is_discarded() || !body.is_object()) return fail(res, 400, "bad_json", "the body is a JSON object");
        }
        std::unique_lock<std::mutex> held(one, std::defer_lock);
        {
            std::lock_guard<std::mutex> l(one);
            if (const std::string why = identify(req, c); !why.empty()) {
                if (rule.need != Need::anyone) return fail(res, 401, "not_signed_in", why);
                c = Caller{};   // (an old cookie doesn't keep anyone from logging in again)
                c.address = address(req);
            }
        }
        if (c.auth != Auth::none) t_user = c.who.name;
        if (rule.need != Need::anyone || c.auth != Auth::none) {
            if (c.auth == Auth::none) return fail(res, 401, "not_signed_in", "log in first");
            const std::string key = c.auth == Auth::token ? "t:" + std::to_string(c.token) : "s:" + c.session.substr(0, 24);
            if (limited(limits, res, key, 600, 60)) return;
        } else if (limited(limits, res, "a:" + c.address, 600, 60)) {
            return;
        }
        if (rule.need != Need::anyone) {
            if (rule.session_only && c.auth != Auth::session) return fail(res, 403, "session_only", "only with a log-in, not a token");
            if (c.must_change && !rule.while_changing)
                return fail(res, 403, "must_change_password", "change the password first (POST /api/v1/me/password)");
            if (changes && c.auth == Auth::token && !c.write) return fail(res, 403, "read_only_token", "this token reads only");
            if (changes && c.auth == Auth::session && (c.csrf.empty() || !same(req.get_header_value("X-HK-CSRF"), c.csrf)))
                return fail(res, 403, "csrf", "a change needs the log-in's CSRF value in X-HK-CSRF");
            if (rule.need == Need::op && !c.who.op()) return fail(res, 403, "not_allowed", "an operator's or an admin's");
            if (rule.need == Need::admin && !c.who.admin()) return fail(res, 403, "not_allowed", "an admin's");
        }
        In in{body, ""};
        held.lock();
        h(c, req, in, res);
        if (!in.bad.empty()) fail(res, 400, "bad_request", in.bad);
    }

    // --- the routes
    void routes();
    void settings_put(const Caller &, long arcade, In &, Response &);
};

void Api::Impl::settings_put(const Caller &c, long arcade, In &in, Response &res) {
    if (!in.j.contains("settings") || !in.j["settings"].is_array()) return fail(res, 400, "bad_request", "\"settings\": [{game, name, value}]");
    for (const json &s : in.j["settings"]) {
        if (!s.is_object()) return fail(res, 400, "bad_request", "\"settings\": [{game, name, value}]");
        In one{s, ""};
        const std::string game = one.text("game"), name = one.text("name");
        std::string value;
        if (s.contains("value") && s["value"].is_number_integer()) value = std::to_string(s["value"].get<long long>());
        else value = one.text("value", false);   // (null or missing: none)
        if (!one.bad.empty()) return fail(res, 400, "bad_request", one.bad);
        if (game != "server" && !listed_game(acc, game)) return fail(res, 404, "not_found", "no game " + game);
        if (const auto why = acc.set_setting(c.who, arcade, game, name, value); !why.empty()) return refused(res, why);
    }
    std::vector<SettingLine> lines, own;
    if (const auto why = acc.settings(c.who, arcade, "", lines); !why.empty()) return refused(res, why);
    json out{{"games", games_settings_json(acc, lines, arcade != 0)}};
    if (arcade) out["arcade"] = arcade;
    else if (acc.server_settings(c.who, own).empty()) {
        out["server"] = json::array();
        for (const SettingLine &l : own) out["server"].push_back(setting_json(ea::find_setting(nullptr, l.name.c_str()), l, false));
    }
    answer(res, out);
}

void Api::Impl::routes() {
    const Rule anyone{Need::anyone}, user{Need::user}, op{Need::op}, admin{Need::admin};
    Rule changing = user;
    changing.while_changing = true;
    Rule session = user;
    session.session_only = true;
    Rule password = session;
    password.while_changing = true;

    // --- anyone
    route("GET", "/health", anyone, [this](Caller &, const Request &, In &, Response &res) {
        const bool ok = acc.alive();
        const auto up = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
        answer(res, {{"ok", ok}, {"version", hk::sys::build_id()}, {"uptime", (long long)up}}, ok ? 200 : 503);
    });
    route("GET", "/server", anyone, [this](Caller &, const Request &, In &, Response &res) {
        const auto mode = acc.sign_up_mode();
        json games = json::array();
        names.refresh();
        for (const ea::Game &g : ea::games())
            if (*g.model && acc.listed(g)) games.push_back({{"id", game_id(g)}, {"title", g.title}, {"has_names", has_names(g)}});
        answer(res, {{"name", acc.server_name()},
                     {"signup", mode == Accounts::SignUp::closed ? "closed" : mode == Accounts::SignUp::invite ? "invite" : "open"},
                     {"games", games}});
    });
    route("POST", "/signup", anyone, [this](Caller &c, const Request &, In &in, Response &res) {
        const std::string name = in.text("name"), password = in.text("password"), invite = in.text("invite", false);
        if (!in.bad.empty()) return;
        if (limited(limits, res, "signup:" + c.address, 5, 3600)) return;
        if (const auto why = acc.sign_up(Who::anyone(), name, password, invite); !why.empty()) return refused(res, why);
        answer(res, {{"name", name}}, 201);
    });
    route("POST", "/login", anyone, [this](Caller &, const Request &req, In &in, Response &res) {
        const std::string name = in.text("name"), password = in.text("password");
        if (!in.bad.empty()) return;
        if (limited(limits, res, "login-a:" + address(req), 10, 60) || limited(limits, res, "login-n:" + lower(name), 10, 60)) return;
        Who who;
        bool must_change = false;
        if (const auto why = acc.check_password(name, password, who, must_change); !why.empty()) return refused(res, why);
        std::string token, csrf;
        if (const auto why = acc.open_session(who, token, csrf); !why.empty()) return refused(res, why);
        User u;
        acc.user(who, who.name, u);
        res.set_header("Set-Cookie", std::string(COOKIE) + "=" + token + "; Path=/; Max-Age=" + std::to_string(Accounts::SESSION_SECONDS) +
                                         "; HttpOnly; SameSite=Strict" + (o.insecure_cookies ? "" : "; Secure"));
        answer(res, {{"user", user_json(u)}, {"csrf", csrf}, {"must_change_password", must_change}});
    });
    route("POST", "/reset", anyone, [this](Caller &, const Request &req, In &in, Response &res) {
        const std::string name = in.text("name", false), code = in.text("code"), password = in.text("password");
        if (!in.bad.empty()) return;
        if (limited(limits, res, "login-a:" + address(req), 10, 60)) return;
        if (const auto why = acc.use_reset(name, code, password); !why.empty()) return refused(res, why);
        answer(res, {{"ok", true}});
    });

    // --- any user
    route("POST", "/logout", changing, [this](Caller &c, const Request &, In &, Response &res) {
        if (c.auth == Auth::session) acc.close_session(c.session);
        res.set_header("Set-Cookie", std::string(COOKIE) + "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict" + (o.insecure_cookies ? "" : "; Secure"));
        answer(res, {{"ok", true}});
    });
    route("GET", "/me", changing, [this](Caller &c, const Request &, In &, Response &res) {
        User u;
        if (const auto why = acc.user(c.who, c.who.name, u); !why.empty()) return refused(res, why);
        json me = user_json(u);
        me["auth"] = c.auth == Auth::token ? "token" : "session";
        if (c.auth == Auth::token) me["scope"] = c.write ? "write" : "read";
        else me["csrf"] = c.csrf;   // (a page loaded again finds it here; only the cookie's holder reads it)
        answer(res, me);
    });
    route("POST", "/me/password", password, [this](Caller &c, const Request &, In &in, Response &res) {
        const std::string old_password = in.text("old_password"), pw = in.text("password");
        if (!in.bad.empty()) return;
        if (const auto why = acc.change_password(c.who, c.who.name, old_password, pw); !why.empty()) return refused(res, why);
        answer(res, {{"ok", true}});
    });
    route("PUT", "/me/hide_stats", user, [this](Caller &c, const Request &, In &in, Response &res) {
        bool hide = false;
        if (!in.flag("hide_stats", hide)) {
            if (in.bad.empty()) in.bad = "\"hide_stats\" is needed";
            return;
        }
        if (const auto why = acc.set_hide_stats(c.who, c.who.name, hide); !why.empty()) return refused(res, why);
        answer(res, {{"hide_stats", hide}});
    });
    route("GET", "/me/tokens", session, [this](Caller &c, const Request &, In &, Response &res) {
        std::vector<Token> list;
        if (const auto why = acc.tokens(c.who, list); !why.empty()) return refused(res, why);
        json out = json::array();
        for (const Token &t : list)
            out.push_back({{"id", t.id}, {"label", t.label}, {"scope", t.write ? "write" : "read"}, {"created", t.created}, {"last_used", when(t.last_used)}});
        answer(res, {{"tokens", out}});
    });
    route("POST", "/me/tokens", session, [this](Caller &c, const Request &, In &in, Response &res) {
        const std::string label = in.text("label"), scope = in.text("scope");
        if (!in.bad.empty()) return;
        if (scope != "read" && scope != "write") return fail(res, 400, "bad_request", "\"scope\" is \"read\" or \"write\"");
        std::string token;
        long id = 0;
        if (const auto why = acc.make_token(c.who, label, scope == "write", token, id); !why.empty()) return refused(res, why);
        answer(res, {{"id", id}, {"label", label}, {"scope", scope}, {"token", token}}, 201);   // (its only showing)
    });
    route("DELETE", "/me/tokens/:id", session, [this](Caller &c, const Request &req, In &, Response &res) {
        long id = 0;
        if (!whole_number(req.path_params.at("id"), id, 1, 1000000000L)) return fail(res, 404, "not_found", "no such token");
        if (const auto why = acc.revoke_token(c.who, id); !why.empty()) return refused(res, why);
        answer(res, {{"ok", true}});
    });
    route("GET", "/me/cards", user, [this](Caller &c, const Request &, In &, Response &res) {
        std::vector<LinkedCard> list;
        if (const auto why = acc.cards(c.who, "", list); !why.empty()) return refused(res, why);
        json out = json::array();
        for (const LinkedCard &l : list) out.push_back({{"card", l.card}, {"linked", l.linked}});
        answer(res, {{"cards", out}});
    });
    route("POST", "/me/cards", user, [this](Caller &c, const Request &, In &in, Response &res) {
        const std::string card = in.text("card"), pin = in.text("pin");
        if (!in.bad.empty()) return;
        if (limited(limits, res, "link:" + std::to_string(c.who.id), 20, 3600)) return;
        if (const auto why = acc.link_card(c.who, card, pin); !why.empty()) return refused(res, why);
        answer(res, {{"card", card}}, 201);
    });
    route("DELETE", "/me/cards/:card", user, [this](Caller &c, const Request &req, In &, Response &res) {
        std::vector<LinkedCard> mine;
        acc.cards(c.who, "", mine);
        const std::string card = req.path_params.at("card");
        if (std::none_of(mine.begin(), mine.end(), [&](const LinkedCard &l) { return lower(l.card) == lower(card); }))
            return fail(res, 404, "not_found", "no card " + card + " of this account's");
        if (const auto why = acc.unlink_card(c.who, card); !why.empty()) return refused(res, why);
        answer(res, {{"ok", true}});
    });
    route("GET", "/me/stats", user, [this](Caller &c, const Request &, In &, Response &res) {
        std::vector<Stat> stats;
        if (const auto why = acc.stats(c.who, c.who.name, stats); !why.empty()) return refused(res, why);
        answer(res, {{"name", c.who.name}, {"stats", stats_json(stats)}});
    });
    route("GET", "/players", user, [this](Caller &c, const Request &, In &, Response &res) {
        std::vector<std::string> names;
        if (const auto why = acc.players(c.who, names); !why.empty()) return refused(res, why);
        json out = json::array();
        for (const std::string &n : names) out.push_back({{"name", n}});
        answer(res, {{"players", out}});
    });
    route("GET", "/players/:name/stats", user, [this](Caller &c, const Request &req, In &, Response &res) {
        std::vector<Stat> stats;
        const std::string name = req.path_params.at("name");
        if (const auto why = acc.stats(c.who, name, stats); !why.empty()) return refused(res, why);
        answer(res, {{"name", name}, {"stats", stats_json(stats)}});
    });
    route("GET", "/games", user, [this](Caller &, const Request &, In &, Response &res) {
        json out = json::array();
        names.refresh();
        for (const ea::Game &g : ea::games())
            if (*g.model && acc.listed(g))
                out.push_back({{"id", game_id(g)}, {"title", g.title}, {"scores", g.readers != nullptr}, {"has_names", has_names(g)}});
        answer(res, {{"games", out}});
    });
    route("GET", "/games/:id/songs", user, [this](Caller &, const Request &req, In &, Response &res) {   // (its pack's names)
        const ea::Game *g = listed_game(acc, req.path_params.at("id"));
        if (!g) return fail(res, 404, "not_found", "no game " + req.path_params.at("id"));
        names.refresh();
        json songs = json::object(), charts = json::object();
        if (const auto p = names.pack(game_id(*g))) {
            for (const auto &[n, s] : p->songs) {
                json o{{"title", s.title}};
                if (!s.artist.empty()) o["artist"] = s.artist;
                if (!s.genre.empty()) o["genre"] = s.genre;
                songs[std::to_string(n)] = std::move(o);
            }
            for (const auto &[n, c] : p->charts) charts[std::to_string(n)] = c;
        }
        answer(res, {{"game", game_id(*g)}, {"songs", songs}, {"charts", charts}});
    });
    route("GET", "/games/:id/top", user, [this](Caller &c, const Request &req, In &, Response &res) {
        long song = -1, chart = -1, limit = 20;
        if ((req.has_param("song") && !whole_number(req.get_param_value("song"), song, 0, 1000000000L)) ||
            (req.has_param("chart") && !whole_number(req.get_param_value("chart"), chart, 0, 1000000000L)) ||
            (req.has_param("limit") && !whole_number(req.get_param_value("limit"), limit, 1, 100)))
            return fail(res, 400, "bad_request", "song and chart: whole numbers; limit: 1 to 100");
        const std::string id = req.path_params.at("id");
        std::vector<TopScore> top;
        if (const auto why = acc.top(c.who, id, song, chart, (size_t)limit, top); !why.empty()) return refused(res, why);
        json out = json::array();
        for (const TopScore &t : top) {
            json o{{"rank", out.size() + 1}, {"user", t.user.empty() ? json(nullptr) : json(t.user)}, {"song", t.song}, {"chart", t.chart},
                   {"score", t.score}, {"clear", maybe(t.clear)}};
            name_field(o, "player", t.player);
            out.push_back(std::move(o));
        }
        answer(res, {{"game", id}, {"title", ea::game_by_id(id)->title}, {"scores", out}});
    });

    // --- operators (their own) and admins (all)
    route("GET", "/arcades", op, [this](Caller &c, const Request &, In &, Response &res) {
        std::vector<Arcade> list;
        if (const auto why = acc.arcades(c.who, list); !why.empty()) return refused(res, why);
        json out = json::array();
        for (const Arcade &x : list) out.push_back(arcade_json(x));
        answer(res, {{"arcades", out}});
    });
    auto find_arcade = [this](const Caller &c, const std::string &s, Arcade &found) {
        long id = 0;
        std::vector<Arcade> list;
        if (!whole_number(s, id, 1, 1000000000L) || !acc.arcades(c.who, list).empty()) return false;
        for (const Arcade &x : list)
            if (x.id == id) return found = x, true;
        return false;
    };
    route("POST", "/arcades", op, [this](Caller &c, const Request &, In &in, Response &res) {
        long pref = 0, id = 0;
        const std::string name = in.text("name"), owner = in.text("owner", false);
        if (!in.number("prefecture", pref, 1, 47) && in.bad.empty()) in.bad = "\"prefecture\" (1 to 47) is needed";
        if (!in.bad.empty()) return;
        if (const auto why = acc.add_arcade(c.who, name, (int)pref, owner, id); !why.empty()) return refused(res, why);
        std::vector<Arcade> list;
        acc.arcades(c.who, list);
        for (const Arcade &x : list)
            if (x.id == id) return answer(res, arcade_json(x), 201);
        answer(res, {{"id", id}}, 201);
    });
    route("GET", "/arcades/:id", op, [find_arcade](Caller &c, const Request &req, In &, Response &res) {
        Arcade x;
        if (!find_arcade(c, req.path_params.at("id"), x)) return fail(res, 404, "not_found", "no arcade " + req.path_params.at("id"));
        answer(res, arcade_json(x));
    });
    route("PUT", "/arcades/:id", op, [this, find_arcade](Caller &c, const Request &req, In &in, Response &res) {
        Arcade x;
        if (!find_arcade(c, req.path_params.at("id"), x)) return fail(res, 404, "not_found", "no arcade " + req.path_params.at("id"));
        long pref = 0;
        const std::string name = in.text("name", false);
        const bool has_pref = in.number("prefecture", pref, 1, 47);
        if (!in.bad.empty()) return;
        if (in.has("name"))
            if (const auto why = acc.rename_arcade(c.who, x.id, name); !why.empty()) return refused(res, why);
        if (has_pref)
            if (const auto why = acc.set_arcade_pref(c.who, x.id, (int)pref); !why.empty()) return refused(res, why);
        find_arcade(c, req.path_params.at("id"), x);
        answer(res, arcade_json(x));
    });
    route("DELETE", "/arcades/:id", op, [this, find_arcade](Caller &c, const Request &req, In &, Response &res) {
        Arcade x;
        if (!find_arcade(c, req.path_params.at("id"), x)) return fail(res, 404, "not_found", "no arcade " + req.path_params.at("id"));
        if (const auto why = acc.remove_arcade(c.who, x.id); !why.empty()) return refused(res, why);
        answer(res, {{"ok", true}});
    });
    route("GET", "/arcades/:id/machines", op, [this, find_arcade](Caller &c, const Request &req, In &, Response &res) {
        Arcade x;
        if (!find_arcade(c, req.path_params.at("id"), x)) return fail(res, 404, "not_found", "no arcade " + req.path_params.at("id"));
        std::vector<Machine> list;
        if (const auto why = acc.machines(c.who, x.id, list); !why.empty()) return refused(res, why);
        json out = json::array();
        for (const Machine &m : list) out.push_back(machine_json(m));
        answer(res, {{"machines", out}});
    });
    route("POST", "/arcades/:id/machines", op, [this, find_arcade](Caller &c, const Request &req, In &in, Response &res) {
        Arcade x;
        if (!find_arcade(c, req.path_params.at("id"), x)) return fail(res, 404, "not_found", "no arcade " + req.path_params.at("id"));
        const std::string name = in.text("name");
        if (!in.bad.empty()) return;
        std::string pcbid, key;
        if (const auto why = acc.add_machine(c.who, x.id, name, pcbid, &key); !why.empty()) return refused(res, why);
        answer(res, {{"pcbid", pcbid}, {"arcade", x.id}, {"name", name}, {"enabled", true}, {"has_key", true}, {"key", key}}, 201);   // (its only showing)
    });
    auto find_machine = [this](const Caller &c, const std::string &pcbid, Machine &found) {
        std::vector<Machine> list;
        if (!acc.machines(c.who, 0, list).empty()) return false;
        for (const Machine &m : list)
            if (lower(m.pcbid) == lower(pcbid)) return found = m, true;
        return false;
    };
    route("GET", "/machines", op, [this](Caller &c, const Request &, In &, Response &res) {
        std::vector<Machine> list;
        if (const auto why = acc.machines(c.who, 0, list); !why.empty()) return refused(res, why);
        json out = json::array();
        for (const Machine &m : list) out.push_back(machine_json(m));
        answer(res, {{"machines", out}});
    });
    route("GET", "/machines/:pcbid", op, [find_machine](Caller &c, const Request &req, In &, Response &res) {
        Machine m;
        if (!find_machine(c, req.path_params.at("pcbid"), m)) return fail(res, 404, "not_found", "no machine " + req.path_params.at("pcbid"));
        answer(res, machine_json(m));
    });
    route("PUT", "/machines/:pcbid", op, [this, find_machine](Caller &c, const Request &req, In &in, Response &res) {
        Machine m;
        if (!find_machine(c, req.path_params.at("pcbid"), m)) return fail(res, 404, "not_found", "no machine " + req.path_params.at("pcbid"));
        bool on = false;
        const std::string name = in.text("name", false);
        const bool has_on = in.flag("enabled", on);
        if (!in.bad.empty()) return;
        if (in.has("name"))
            if (const auto why = acc.rename_machine(c.who, m.pcbid, name); !why.empty()) return refused(res, why);
        if (has_on)
            if (const auto why = acc.enable_machine(c.who, m.pcbid, on); !why.empty()) return refused(res, why);
        find_machine(c, m.pcbid, m);
        answer(res, machine_json(m));
    });
    route("DELETE", "/machines/:pcbid", op, [this, find_machine](Caller &c, const Request &req, In &, Response &res) {
        Machine m;
        if (!find_machine(c, req.path_params.at("pcbid"), m)) return fail(res, 404, "not_found", "no machine " + req.path_params.at("pcbid"));
        if (const auto why = acc.remove_machine(c.who, m.pcbid); !why.empty()) return refused(res, why);
        answer(res, {{"ok", true}});
    });
    route("POST", "/machines/:pcbid/key", op, [this, find_machine](Caller &c, const Request &req, In &, Response &res) {
        Machine m;
        if (!find_machine(c, req.path_params.at("pcbid"), m)) return fail(res, 404, "not_found", "no machine " + req.path_params.at("pcbid"));
        std::string key;
        if (const auto why = acc.new_machine_key(c.who, m.pcbid, key); !why.empty()) return refused(res, why);
        answer(res, {{"pcbid", m.pcbid}, {"key", key}});   // (its only showing)
    });
    route("GET", "/arcades/:id/settings", op, [this, find_arcade](Caller &c, const Request &req, In &, Response &res) {
        Arcade x;
        if (!find_arcade(c, req.path_params.at("id"), x)) return fail(res, 404, "not_found", "no arcade " + req.path_params.at("id"));
        std::vector<SettingLine> lines;
        if (const auto why = acc.settings(c.who, x.id, "", lines); !why.empty()) return refused(res, why);
        answer(res, {{"arcade", x.id}, {"games", games_settings_json(acc, lines, true)}});
    });
    route("PUT", "/arcades/:id/settings", op, [this, find_arcade](Caller &c, const Request &req, In &in, Response &res) {
        Arcade x;
        if (!find_arcade(c, req.path_params.at("id"), x)) return fail(res, 404, "not_found", "no arcade " + req.path_params.at("id"));
        settings_put(c, x.id, in, res);
    });

    // --- admins
    route("GET", "/users", admin, [this](Caller &c, const Request &, In &, Response &res) {
        std::vector<User> list;
        if (const auto why = acc.users(c.who, list); !why.empty()) return refused(res, why);
        json out = json::array();
        for (const User &u : list) out.push_back(user_json(u));
        answer(res, {{"users", out}});
    });
    route("GET", "/users/:name", admin, [this](Caller &c, const Request &req, In &, Response &res) {
        User u;
        if (const auto why = acc.user(c.who, req.path_params.at("name"), u); !why.empty()) return refused(res, why);
        answer(res, user_json(u));
    });
    route("PUT", "/users/:name", admin, [this](Caller &c, const Request &req, In &in, Response &res) {
        User u;
        if (const auto why = acc.user(c.who, req.path_params.at("name"), u); !why.empty()) return refused(res, why);
        Role role = u.role;
        bool disabled = false, hide = false;
        const std::string r = in.text("role", false);
        const bool has_disabled = in.flag("disabled", disabled), has_hide = in.flag("hide_stats", hide);
        if (!in.bad.empty()) return;
        if (in.has("role") && !role_named(r, role)) return fail(res, 400, "bad_request", "\"role\": player, operator or admin");
        if (in.has("role"))
            if (const auto why = acc.set_role(c.who, u.name, role); !why.empty()) return refused(res, why);
        if (has_disabled)
            if (const auto why = acc.set_disabled(c.who, u.name, disabled); !why.empty()) return refused(res, why);
        if (has_hide)
            if (const auto why = acc.set_hide_stats(c.who, u.name, hide); !why.empty()) return refused(res, why);
        acc.user(c.who, u.name, u);
        answer(res, user_json(u));
    });
    route("GET", "/users/:name/cards", admin, [this](Caller &c, const Request &req, In &, Response &res) {   // (the pages' Cards)
        std::vector<LinkedCard> list;
        if (const auto why = acc.cards(c.who, req.path_params.at("name"), list); !why.empty()) return refused(res, why);
        json out = json::array();
        for (const LinkedCard &l : list) out.push_back({{"card", l.card}, {"linked", l.linked}});
        answer(res, {{"name", req.path_params.at("name")}, {"cards", out}});
    });
    route("DELETE", "/users/:name", admin, [this](Caller &c, const Request &req, In &, Response &res) {
        if (const auto why = acc.remove_user(c.who, req.path_params.at("name")); !why.empty()) return refused(res, why);
        answer(res, {{"ok", true}});
    });
    route("POST", "/users/:name/reset", admin, [this](Caller &c, const Request &req, In &, Response &res) {
        std::string code;
        if (const auto why = acc.make_reset(c.who, req.path_params.at("name"), code); !why.empty()) return refused(res, why);
        answer(res, {{"name", req.path_params.at("name")}, {"code", code}, {"hours", 24}});   // (its only showing)
    });
    auto config_json = [this] {
        const auto mode = acc.sign_up_mode();
        return json{{"name", acc.server_name()},
                    {"signup", mode == Accounts::SignUp::closed ? "closed" : mode == Accounts::SignUp::invite ? "invite" : "open"},
                    {"list_all_games", acc.list_all_games()}};
    };
    route("GET", "/config", admin, [config_json](Caller &, const Request &, In &, Response &res) { answer(res, config_json()); });
    route("PUT", "/config", admin, [this, config_json](Caller &c, const Request &, In &in, Response &res) {
        bool all = false;
        const std::string name = in.text("name", false), mode = in.text("signup", false);
        const bool has_all = in.flag("list_all_games", all);
        if (!in.bad.empty()) return;
        if (in.has("signup") && mode != "open" && mode != "invite" && mode != "closed")
            return fail(res, 400, "bad_request", "\"signup\": open, invite or closed");
        if (in.has("name"))
            if (const auto why = acc.set_server_name(c.who, name); !why.empty()) return refused(res, why);
        if (in.has("signup"))
            if (const auto why = acc.set_sign_up_mode(c.who, mode == "closed"   ? Accounts::SignUp::closed
                                                           : mode == "invite" ? Accounts::SignUp::invite
                                                                              : Accounts::SignUp::open);
                !why.empty())
                return refused(res, why);
        if (has_all)
            if (const auto why = acc.set_list_all_games(c.who, all); !why.empty()) return refused(res, why);
        answer(res, config_json());
    });
    route("POST", "/config/invites", admin, [this](Caller &c, const Request &, In &in, Response &res) {
        long uses = 1, days = 7;
        in.number("uses", uses, 1, 10000);
        in.number("days", days, 1, 365);
        if (!in.bad.empty()) return;
        std::string code;
        if (const auto why = acc.make_invite(c.who, (int)uses, (int)days, code); !why.empty()) return refused(res, why);
        answer(res, {{"code", code}, {"uses", uses}, {"days", days}}, 201);   // (its only showing)
    });
    route("GET", "/cards/:card", admin, [this](Caller &c, const Request &req, In &, Response &res) {
        CardInfo i;
        if (const auto why = acc.card(c.who, req.path_params.at("card"), i); !why.empty()) return refused(res, why);
        json games = json::array();
        for (const CardGame &g : i.games) games.push_back({{"id", g.game}, {"title", g.title}, {"player", g.player}});
        answer(res, {{"card", i.card},
                     {"known", i.known},
                     {"user", i.user.empty() ? json(nullptr) : json(i.user)},
                     {"linked", when(i.linked)},
                     {"locked", i.locked},
                     {"wrong_pins", i.tries},
                     {"tries_left", i.locked ? 0 : std::max(0, Accounts::LOCK_TRIES - i.tries)},
                     {"games", games}});
    });
    route("POST", "/cards/:card/unlock", admin,[this](Caller &c, const Request &req, In &, Response &res) {
        if (const auto why = acc.unlock_card(c.who, req.path_params.at("card")); !why.empty()) return refused(res, why);
        answer(res, {{"ok", true}});
    });
    route("DELETE", "/cards/:card", admin, [this](Caller &c, const Request &req, In &, Response &res) {
        if (const auto why = acc.unlink_card(c.who, req.path_params.at("card")); !why.empty()) return refused(res, why);
        answer(res, {{"ok", true}});
    });
    route("PUT", "/cards/:card/pin", admin, [this](Caller &c, const Request &req, In &in, Response &res) {
        const std::string pin = in.text("pin");
        if (!in.bad.empty()) return;
        ea::PinReset did;
        const auto why = acc.reset_card_pin(c.who, req.path_params.at("card"), pin, did);
        if (!why.empty() && did.done.empty()) return refused(res, why);
        answer(res, {{"changed", did.done}, {"not_changed", did.not_done}, {"complete", why.empty()}});
    });
    route("GET", "/settings", admin, [this](Caller &c, const Request &, In &, Response &res) {
        std::vector<SettingLine> lines, own;
        if (const auto why = acc.settings(c.who, 0, "", lines); !why.empty()) return refused(res, why);
        if (const auto why = acc.server_settings(c.who, own); !why.empty()) return refused(res, why);
        json server = json::array();
        for (const SettingLine &l : own) server.push_back(setting_json(ea::find_setting(nullptr, l.name.c_str()), l, false));
        answer(res, {{"server", server}, {"games", games_settings_json(acc, lines, false)}});
    });
    route("PUT", "/settings", admin, [this](Caller &c, const Request &, In &in, Response &res) { settings_put(c, 0, in, res); });
    route("GET", "/names", admin, [this](Caller &, const Request &, In &, Response &res) {   // (the packs loaded and refused)
        names.refresh();
        json packs = json::array(), refused = json::array();
        for (const auto &p : names.packs()) {
            const ea::Game *g = ea::game_by_id(p->game);
            packs.push_back({{"game", p->game}, {"title", g ? g->title : p->game}, {"file", p->file}, {"songs", p->songs.size()},
                             {"charts", p->charts.size()}, {"time", p->time}});
        }
        for (const NameRefusal &r : names.refused()) refused.push_back({{"file", r.file}, {"why", r.why}});
        answer(res, {{"folder", names.folder()}, {"packs", packs}, {"refused", refused}});
    });
    route("GET", "/audit", admin, [this](Caller &c, const Request &req, In &, Response &res) {
        long last = 100;
        if (req.has_param("limit") && !whole_number(req.get_param_value("limit"), last, 1, 10000))
            return fail(res, 400, "bad_request", "limit: 1 to 10000");
        std::vector<AuditLine> list;
        if (const auto why = acc.audit(c.who, (size_t)last, list); !why.empty()) return refused(res, why);
        json out = json::array();
        for (const AuditLine &l : list) out.push_back({{"id", l.id}, {"at", l.at}, {"user", l.user}, {"action", l.action}, {"detail", l.detail}});
        answer(res, {{"audit", out}});
    });
}

Api::Api(const std::string &file, ea::Store &store, const ApiOptions &o) : impl_(std::make_unique<Impl>(file, store, o)) {}

Api::~Api() { stop(); }

bool Api::start(std::string &error) {
    Impl &m = *impl_;
    if (!m.acc.ok()) return error = "the database isn't open", false;
    m.http.set_payload_max_length(BODY_MAX);
    m.http.set_read_timeout(10);
    m.http.set_write_timeout(10);
    m.http.set_keep_alive_timeout(5);
    m.http.set_keep_alive_max_count(100);
    m.http.set_error_handler([](const Request &req, Response &res) {
        if (!res.body.empty()) return httplib::Server::HandlerResponse::Unhandled;   // (a route's own answer)
        const int s = res.status;
        if (req.path.rfind("/api/", 0) != 0) {   // (the pages' side: plain text)
            page_headers(res);
            res.set_content(s == 404 ? "not found\n" : "not a request the server takes\n", "text/plain; charset=utf-8");
            res.status = s;
            return httplib::Server::HandlerResponse::Handled;
        }
        fail(res, s, s == 404 ? "not_found" : s == 413 ? "too_large" : s == 405 ? "no_such_method" : "bad_request",
             s == 404 ? "no such thing in the API" : s == 413 ? "the body is 64 KiB at most" : "the request isn't one the API takes");
        res.status = s;
        return httplib::Server::HandlerResponse::Handled;
    });
    m.http.set_exception_handler([](const Request &, Response &res, std::exception_ptr) { fail(res, 500, "server_error", "the server could not answer that"); });
    m.routes();
    m.names.refresh(true);   // (the song names read at the start; a pack refused is said, nothing stops)
    std::string pages;   // the web pages' folder, served at / (after the API's routes: /api/ stays theirs)
    if (m.o.web_enabled) {
        pages = m.o.web.empty() ? find_web() : m.o.web;
        std::error_code ec;
        if (!m.o.web.empty() && !std::filesystem::is_regular_file(std::filesystem::path(pages) / "index.html", ec))
            return error = "no index.html in " + pages + " (--web DIR, or --no-web)", false;
    }
    if (!pages.empty()) {
        m.http.Get("/.*", [pages](const Request &req, Response &res) {
            if (req.path.rfind("/api/", 0) == 0) return fail(res, 404, "not_found", "no such thing in the API");
            const std::string rel = web_relative(req.path);
            const char *type = rel.empty() ? nullptr : web_type(rel);
            std::string body;
            if (!type || !web_file(pages, rel, body)) return page_missing(res);
            page_headers(res);
            res.set_content(std::move(body), type);
        });
    }
    if (!m.http.bind_to_port(m.o.host, m.o.port)) {
        error = "cannot listen on " + m.o.host + ":" + std::to_string(m.o.port);
        return false;
    }
    if (!m.o.quiet) {   // a line a request: when, method, path, status, ms, the user if one is logged in
        m.http.set_pre_routing_handler([](const Request &, Response &) {
            t_began = std::chrono::steady_clock::now();
            t_user.clear();
            return httplib::Server::HandlerResponse::Unhandled;
        });
        m.http.set_logger([](const Request &req, const Response &res) {
            const bool timed = t_began != std::chrono::steady_clock::time_point{};
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t_began).count();
            std::string user;
            for (const char ch : t_user) user += (unsigned char)ch > ' ' && ch != 127 ? ch : '?';
            ea::say_line(ea::utc_now() + " api " + logged_path(req.method) + " " + logged_path(req.path) + " " + std::to_string(res.status) + " " +
                         (timed ? std::to_string((long long)ms) + "ms" : "-") + " " + (user.empty() ? "-" : user));
            t_began = {};
            t_user.clear();
        });
    }
    m.thread = std::thread([&m] { m.http.listen_after_bind(); });
    if (!m.o.quiet) {
        printf("hiraku-server: the API on http://%s:%d/api/v1/ (put TLS in front of it: docs/SERVER.md)\n", m.o.host.c_str(), m.o.port);
        if (!pages.empty()) printf("hiraku-server: the web pages from %s on http://%s:%d/\n", pages.c_str(), m.o.host.c_str(), m.o.port);
        else printf("hiraku-server: no web pages (%s)\n", m.o.web_enabled ? "no web folder found: --web DIR" : "--no-web");
        fflush(stdout);
    }
    return true;
}

int health(const std::string &address, int port) {
    const std::string host = address == "0.0.0.0" ? "127.0.0.1" : address == "::" ? "::1" : address;
    const std::string where = (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
    httplib::Client cli(host, port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(5);
    const auto r = cli.Get("/api/v1/health");
    if (!r) {
        printf("hiraku-server: not healthy: no answer on %s (%s)\n", where.c_str(), httplib::to_string(r.error()).c_str());
        return 1;
    }
    const json j = json::parse(r->body, nullptr, false);
    const bool ok = r->status == 200 && j.is_object() && j.contains("ok") && j["ok"] == true;
    std::string version = "?";
    long long up = -1;
    if (j.is_object() && j.contains("version") && j["version"].is_string()) version = j["version"].get<std::string>();
    if (j.is_object() && j.contains("uptime") && j["uptime"].is_number_integer()) up = j["uptime"].get<long long>();
    if (ok) printf("hiraku-server: healthy on %s, build %s, up %lld s\n", where.c_str(), version.c_str(), up);
    else printf("hiraku-server: not healthy on %s: HTTP %d%s\n", where.c_str(), r->status, r->status == 503 ? ", the database doesn't answer" : "");
    return ok ? 0 : 1;
}

void Api::stop() {
    if (!impl_ || !impl_->thread.joinable()) return;
    impl_->http.stop();
    impl_->thread.join();
}

}  // namespace hk::srv
