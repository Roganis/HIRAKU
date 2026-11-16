// SPDX-License-Identifier: GPL-3.0-or-later
// The e-AMUSEMENT server's folder back end (store.h): a file a thing kept, named from its table's
// pattern and its key.
#include "eamuse/store.h"
#include "eamuse/protocol.h"
#include "shell/sys.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace hk::ea {

std::string FolderStore::file_name(const Table &t, const Key &k) {
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
    sys::make_dir(dir_);
    // whole or not at all: a full or failing disk keeps the player's file from before
    const std::string path = dir_ + "/" + name;
    if (!sys::exists(path) && !room_for_file()) return false;
    if (!sys::write_file(path, data)) {
        fprintf(stderr, "e-AMUSEMENT: cannot write %s: the one from before is kept\n", path.c_str());
        return false;
    }
    return true;
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

bool FolderStore::room_for_file() {
    if (files_ < 0) {
        files_ = 0;
        std::error_code ec;
        for (const auto &e : std::filesystem::directory_iterator(dir_, ec)) (void)e, files_++;
    }
    const long most = limit("files", 200000);
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
    const std::string aside = name + ".unread-" + date;
    write(aside, had);
    fprintf(stderr, "e-AMUSEMENT: %s isn't a player's file as this HIRAKU keeps them (%zu bytes): kept as %s, a new one made\n",
            name.c_str(), had.size(), aside.c_str());
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
