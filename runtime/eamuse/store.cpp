// SPDX-License-Identifier: GPL-3.0-or-later
// The e-AMUSEMENT server's folder back end (store.h): a file a thing kept, named from its table's
// pattern and its key.
#include "eamuse/store.h"
#include "eamuse/games.h"
#include "eamuse/protocol.h"
#include "eamuse/tables.h"
#include "shell/sys.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace hk::ea {

std::string file_name(const Table &t, const Key &k) {
    std::string name;
    size_t part = 0;
    for (const char *p = t.file; *p; p++) {
        if (p[0] == '{' && p[1] == '}') {
            if (part == k.size()) return "";
            name += k[part++];
            p++;
        } else
            name += *p;
    }
    return part == k.size() ? name : "";
}

std::string FolderStore::read(const std::string &name) const {
    if (dir_.empty() || !ea::safe_file(name)) return "";
    std::ifstream f(dir_ + "/" + name, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

bool FolderStore::write(const std::string &name, const std::string &data) {
    if (dir_.empty()) return false;
    if (!ea::safe_file(name)) {   // (a call's field never names a file outside the folder)
        fprintf(stderr, "e-AMUSEMENT: not written: a call's field made no file name (%zu bytes)\n", name.size());
        return false;
    }
    if (!writable()) return false;
    sys::make_dir(dir_);
    // whole or not at all: a full or failing disk keeps the player's file from before
    const std::string path = dir_ + "/" + name;
    if (!sys::exists(path)) {
        if (!room_for_file()) return false;
        stamp();
    }
    if (!sys::write_file(path, data)) {
        fprintf(stderr, "e-AMUSEMENT: cannot write %s: the one from before is kept\n", path.c_str());
        return false;
    }
    return true;
}

bool FolderStore::writable() {
    if (format_ < 0) {
        const std::string path = dir_ + "/format";
        format_ = 0;
        if (sys::exists(path)) {
            const std::string had = read("format");
            format_ = std::max(1L, std::strtol(had.c_str(), nullptr, 10));
        }
    }
    if (format_ <= FORMAT) return true;
    if (!newer_said_)
        fprintf(stderr, "e-AMUSEMENT: %s was made by a newer HIRAKU (its format %ld, this one's %ld): it is left as it is, nothing is written to it\n",
                dir_.c_str(), format_, FORMAT);
    newer_said_ = true;
    return false;
}

void FolderStore::stamp() {
    if (format_ != 0) return;
    format_ = FORMAT;
    const std::string path = dir_ + "/format";
    if (sys::exists(path)) return;   // (another server sharing the folder stamped it)
    if (!sys::write_file(path, std::to_string(FORMAT) + "\n")) fprintf(stderr, "e-AMUSEMENT: cannot write %s\n", path.c_str());
    else if (files_ >= 0) files_++;
}

std::string FolderStore::get(const Table &t, const Key &k) const { return read(file_name(t, k)); }
bool FolderStore::put(const Table &t, const Key &k, const std::string &data) { return write(file_name(t, k), data); }
std::string FolderStore::label(const Table &t, const Key &k) const { return file_name(t, k); }

long FolderStore::limit(const char *what, long by_default) const {
    const std::string set = read(std::string("limit-") + what);
    char *end = nullptr;
    const long n = std::strtol(set.c_str(), &end, 10);
    return end != set.c_str() && n >= 0 ? n : by_default;
}

namespace {
// The pattern's key in a file's name, a part a {} (no '-' in it): false if the name isn't the pattern's.
// Its letters of its own, for the most specific pattern
bool matches(const char *pattern, const std::string &file, Key &key, size_t &letters) {
    key.clear();
    letters = 0;
    size_t at = 0;
    for (const char *p = pattern; *p; p++) {
        if (p[0] == '{' && p[1] == '}') {
            size_t end = at;
            while (end < file.size() && file[end] != '-') end++;
            const std::string part = file.substr(at, end - at);
            if (!ea::safe_name(part)) return false;
            key.push_back(part);
            at = end;
            p++;
        } else {
            if (at >= file.size() || file[at] != *p) return false;
            at++, letters++;
        }
    }
    return at == file.size();
}
}  // namespace

const Table *table_of(const std::string &file, Key &key) {
    const Table *best = nullptr;
    size_t most = 0;
    for (const Table *t : tables::all) {
        Key k;
        size_t letters = 0;
        if (!matches(t->file, file, k, letters)) continue;
        if (k.empty()) {   // (a name with no {}: that file is the table)
            key.clear();
            return t;
        }
        if (!best || letters > most) best = t, most = letters, key = k;
    }
    return best;
}

namespace {
constexpr const char ASIDE[] = ".unread-";   // NAME.unread-DATE: a copy kept aside
}

std::string file_name(const Named &n) {
    if (!n.table) return "";
    const std::string name = file_name(*n.table, n.key);
    return name.empty() || n.aside.empty() ? name : name + ASIDE + n.aside;
}

bool named(const std::string &file, Named &n) {
    n = Named{};
    if ((n.table = table_of(file, n.key))) return true;
    const size_t at = file.rfind(ASIDE);
    if (at == std::string::npos) return false;
    const std::string date = file.substr(at + sizeof ASIDE - 1);
    if (date.empty() || date.find_first_not_of("0123456789-_ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") != std::string::npos)
        return false;
    n.table = table_of(file.substr(0, at), n.key);
    n.aside = date;
    return n.table != nullptr;
}

std::vector<Key> FolderStore::keys(const Table &t) const {
    std::vector<Key> out;
    if (dir_.empty()) return out;
    std::error_code ec;
    for (const auto &e : std::filesystem::directory_iterator(dir_, ec)) {
        const std::string name = e.path().filename().string();
        Key k;
        if (e.is_regular_file(ec) && table_of(name, k) == &t) out.push_back(k);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool FolderStore::room_for_file() {
    if (!bounded) return true;
    if (files_ < 0) {
        files_ = 0;
        std::error_code ec;
        for (const auto &e : std::filesystem::directory_iterator(dir_, ec)) (void)e, files_++;
    }
    const long most = limit("files", std::strtol(find_setting(nullptr, "limit-files")->by_default, nullptr, 10));
    if (files_ >= most) {
        if (!files_full_said_) fprintf(stderr, "e-AMUSEMENT: %s holds %ld files, as many as it may (the file limit-files there sets another number): nothing new is kept\n", dir_.c_str(), files_);
        files_full_said_ = true;
        return false;
    }
    files_++;
    return true;
}

// A player's file that isn't a player (not the size of the game's record, or no line of it reads:
// cut short by an older HIRAKU, damaged, another layout's) reads as "no player", and the next new or
// set would take its name: it is first kept beside it as NAME.unread-DATE (the host's clock), and said.
void FolderStore::aside(const Table &t, const Key &k, const std::string &had) {
    if (had.empty() || dir_.empty()) return;
    const std::string name = file_name(t, k);
    char date[32];
    const time_t now = time(nullptr);
    strftime(date, sizeof date, "%Y%m%d-%H%M%S", localtime(&now));
    const std::string aside = ea::file_name(Named{&t, k, date});
    write(aside, had);
    fprintf(stderr, "e-AMUSEMENT: %s isn't a player's file as this HIRAKU keeps them (%zu bytes): kept as %s, a new one made\n",
            name.c_str(), had.size(), aside.c_str());
}

std::vector<Named> FolderStore::asides() const {
    std::vector<Named> out;
    if (dir_.empty()) return out;
    std::error_code ec;
    for (const auto &e : std::filesystem::directory_iterator(dir_, ec)) {
        Named n;
        if (e.is_regular_file(ec) && named(e.path().filename().string(), n) && !n.aside.empty()) out.push_back(n);
    }
    return out;
}

std::string FolderStore::get_named(const Named &n) const { return read(ea::file_name(n)); }
bool FolderStore::put_named(const Named &n, const std::string &data) { return write(ea::file_name(n), data); }

std::vector<std::string> FolderStore::strays() const {
    std::vector<std::string> out;
    if (dir_.empty()) return out;
    std::error_code ec;
    for (const auto &e : std::filesystem::directory_iterator(dir_, ec)) {
        const std::string name = e.path().filename().string();
        Named n;
        if (name != "format" && name != ea::FOLDER_LOCK && !named(name, n)) out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

Copied copy_store(const Store &from, Store &to) {
    Copied c;
    const bool was = to.bounded;
    to.bounded = false;
    {
        const auto held = to.hold();   // (a database: one transaction)
        auto put = [&](const Named &n, const std::string &data) {
            if (to.put_named(n, data)) c.things++;
            else c.not_put.push_back(file_name(n));
        };
        for (const Table *t : tables::all)
            for (const Key &k : from.keys(*t)) put(Named{t, k, ""}, from.get(*t, k));
        for (const Named &n : from.asides()) put(n, from.get_named(n));
    }
    c.not_placed = from.strays();
    to.bounded = was;
    return c;
}

namespace {
struct FolderHold : Store::Hold {
    sys::FileLock lock;
    explicit FolderHold(const std::string &path) : lock(path) {}
};
}  // namespace

std::unique_ptr<Store::Hold> FolderStore::hold() {
    if (!dir_.empty()) sys::make_dir(dir_);
    return std::make_unique<FolderHold>(dir_.empty() ? "" : dir_ + "/" + ea::FOLDER_LOCK);
}

}  // namespace hk::ea
