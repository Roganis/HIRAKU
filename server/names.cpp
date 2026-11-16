// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-server's song names (names.h): a folder's packs, read and read again.
#include "names.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include <json.hpp>

#include "eamuse/games.h"
#include "eamuse/listener.h"

namespace hk::srv {

namespace ea = hk::ea;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr size_t FILES_MAX = 256;   // the folder's files looked at, at most

int64_t steady_seconds() {
    return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int64_t file_seconds(fs::file_time_type t) {   // (the file clock's time as the system's)
    const auto sys = std::chrono::system_clock::now() + std::chrono::duration_cast<std::chrono::system_clock::duration>(t - fs::file_time_type::clock::now());
    return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
}

bool number_key(const std::string &k, long &v) {
    if (k.empty() || k.size() > 10) return false;
    for (char c : k)
        if (c < '0' || c > '9') return false;
    if (k.size() > 1 && k[0] == '0') return false;
    v = std::strtol(k.c_str(), nullptr, 10);
    return v >= 0 && v <= 1000000000L;
}

bool known_game(const std::string &id) {
    const ea::Game *g = ea::game_by_id(id);
    return g && *g->model;
}

}  // namespace

std::string Names::clean(const std::string &s) {
    std::string out;
    size_t chars = 0;
    for (size_t i = 0; i < s.size() && chars < TEXT_MAX;) {
        const unsigned char c = (unsigned char)s[i];
        size_t n = 0;
        uint32_t cp = 0;
        if (c < 0x80) n = 0, cp = c;
        else if (c >= 0xc2 && c <= 0xdf) n = 1, cp = c & 0x1f;
        else if ((c & 0xf0) == 0xe0) n = 2, cp = c & 0x0f;
        else if (c >= 0xf0 && c <= 0xf4) n = 3, cp = c & 0x07;
        else {
            i++;   // (not UTF-8: dropped)
            continue;
        }
        bool ok = i + n < s.size();
        for (size_t k = 1; ok && k <= n; k++) {
            const unsigned char d = (unsigned char)s[i + k];
            ok = (d & 0xc0) == 0x80;
            cp = cp << 6 | (d & 0x3f);
        }
        if (ok && ((n == 2 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) || (n == 3 && (cp < 0x10000 || cp > 0x10ffff)))) ok = false;
        if (!ok) {
            i++;
            continue;
        }
        const bool control = cp < 0x20 || (cp >= 0x7f && cp < 0xa0) || cp == 0x2028 || cp == 0x2029;
        if (!control) out.append(s, i, n + 1), chars++;
        i += n + 1;
    }
    return out;
}

bool Names::parse(const std::string &bytes, const std::string &file, bool (*known)(const std::string &), NamePack &out, std::string &why) {
    if (bytes.size() > FILE_MAX) return why = "over 4 MB", false;
    const json j = json::parse(bytes, nullptr, false);
    const char *shape = "not a names pack: {\"format\": 1, \"game\": ID, \"songs\": {NUMBER: {\"title\": TEXT, ...}}}";
    if (j.is_discarded() || !j.is_object()) return why = "not JSON (a names pack is a JSON object)", false;
    if (!j.contains("format") || !j["format"].is_number_integer() || j["format"].get<long long>() != 1) return why = shape, false;
    if (!j.contains("game") || !j["game"].is_string() || !j.contains("songs") || !j["songs"].is_object()) return why = shape, false;
    const std::string game = j["game"].get<std::string>();
    if (!known(game)) return why = "names a game the server doesn't know (\"" + clean(game).substr(0, 40) + "\")", false;
    if (file != game + ".json") return why = "its game is " + game + ", so its file is " + game + ".json", false;
    if (j["songs"].size() > SONGS_MAX) return why = "over 20000 songs", false;
    NamePack p;
    p.game = game, p.file = file;
    for (const auto &[k, v] : j["songs"].items()) {
        long n = 0;
        if (!number_key(k, n)) return why = "song \"" + clean(k).substr(0, 20) + "\" isn't a song number (0 or more)", false;
        if (!v.is_object() || !v.contains("title") || !v["title"].is_string()) return why = "song " + k + " has no title text", false;
        SongName s;
        s.title = clean(v["title"].get<std::string>());
        for (const char *f : {"artist", "genre"})
            if (v.contains(f) && !v[f].is_null()) {
                if (!v[f].is_string()) return why = "song " + k + "'s " + f + " isn't a text", false;
                (f[0] == 'a' ? s.artist : s.genre) = clean(v[f].get<std::string>());
            }
        if (s.title.empty()) return why = "song " + k + " has an empty title", false;
        p.songs[n] = std::move(s);
    }
    if (j.contains("charts") && !j["charts"].is_null()) {
        if (!j["charts"].is_object() || j["charts"].size() > 1000) return why = "\"charts\" isn't {NUMBER: TEXT} (1000 at most)", false;
        for (const auto &[k, v] : j["charts"].items()) {
            long n = 0;
            if (!number_key(k, n) || !v.is_string()) return why = "chart \"" + clean(k).substr(0, 20) + "\" isn't a number with a text", false;
            p.charts[n] = clean(v.get<std::string>());
        }
    }
    out = std::move(p);
    return true;
}

Names::Names(std::string folder, bool quiet) : folder_(std::move(folder)), quiet_(quiet) {}

void Names::refresh(bool force) {
    if (folder_.empty()) return;
    std::lock_guard<std::mutex> l(mu_);
    const int64_t now = steady_seconds();
    if (!force && looked_ && now - looked_ < LOOK_SECONDS) return;
    looked_ = now;
    auto say = [&](const std::string &line) {
        if (!quiet_) ea::say_line(ea::utc_now() + " hiraku-server: names: " + line);
    };
    std::error_code ec;
    std::map<std::string, std::pair<int64_t, uintmax_t>> here;
    if (fs::is_directory(folder_, ec)) {
        missing_said_ = false;
        for (fs::directory_iterator it(folder_, ec), end; !ec && it != end && here.size() < FILES_MAX; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (name.empty() || name[0] == '.' || name.size() < 6 || name.compare(name.size() - 5, 5, ".json") != 0) continue;
            std::error_code e2;
            if (!it->is_regular_file(e2) || e2) continue;
            const uintmax_t size = it->file_size(e2);
            const auto t = it->last_write_time(e2);
            if (!e2) here[name] = {file_seconds(t), size};
        }
    } else if (!missing_said_ && force) {
        missing_said_ = true;
        say("no folder " + folder_ + ": no song names (--names DIR)");
    }
    for (auto it = files_.begin(); it != files_.end();) {   // (gone: dropped)
        if (here.count(it->first)) ++it;
        else {
            say(it->first + ": gone");
            it = files_.erase(it);
        }
    }
    for (const auto &[name, ts] : here) {
        auto old = files_.find(name);
        if (old != files_.end() && old->second.time == ts.first && old->second.size == ts.second) continue;
        Seen s;
        s.time = ts.first, s.size = ts.second;
        NamePack p;
        if (ts.second > FILE_MAX) {
            s.why = "over 4 MB";
        } else {
            std::ifstream in(fs::path(folder_) / name, std::ios::binary);
            std::string bytes;
            if (in) bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (!in && !in.eof()) s.why = "can't be read";
            else if (parse(bytes, name, known_game, p, s.why)) {
                p.time = ts.first;
                s.pack = std::make_shared<const NamePack>(std::move(p));
            }
        }
        if (s.pack) say(name + ": " + std::to_string(s.pack->songs.size()) + " songs" + (s.pack->charts.empty() ? "" : ", " + std::to_string(s.pack->charts.size()) + " charts"));
        else say(name + ": refused: " + s.why);
        files_[name] = std::move(s);
    }
}

std::shared_ptr<const NamePack> Names::pack(const std::string &game) {
    std::lock_guard<std::mutex> l(mu_);
    const auto it = files_.find(game + ".json");
    return it == files_.end() ? nullptr : it->second.pack;
}

std::vector<std::shared_ptr<const NamePack>> Names::packs() {
    std::lock_guard<std::mutex> l(mu_);
    std::vector<std::shared_ptr<const NamePack>> out;
    for (const auto &[n, s] : files_)
        if (s.pack) out.push_back(s.pack);
    return out;
}

std::vector<NameRefusal> Names::refused() {
    std::lock_guard<std::mutex> l(mu_);
    std::vector<NameRefusal> out;
    for (const auto &[n, s] : files_)
        if (!s.pack) out.push_back({n, s.why});
    return out;
}

}  // namespace hk::srv
