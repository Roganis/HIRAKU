// SPDX-License-Identifier: GPL-3.0-or-later
// The names a PC game under Wine is given for this machine's folders.
//
// The game, and HIRAKU's DLLs inside it, open files by names written in the game's code page:
// Wine's follows the locale the game runs in (932 for the Japanese ones, the player's own for
// pop'n 15 to 18). A folder whose name that page can't write can't be opened by them at all: the
// name reaches them with "?" for each letter it lacks ("ゲーム" in a western locale,
// "Téléchargements" in the Japanese one), and the game sits at an empty screen with no error.
//
// So a folder with letters outside ASCII in its path is given another name: a link to it in
// Wine's own folder, C:\hiraku-paths\<number>, which any code page can write. A path of plain
// letters stays Z:\... as before. (On Windows itself: hk::sys::plain_path, shell/sys.h.)
//
// Wine itself has the same trouble with what it is given by this machine's names (its program, the
// folder it starts in): it reads them in the locale's letters, and the Japanese locale the games
// are given is rarely installed, so "é" isn't read at all (the program isn't found, or the game
// starts in C:\windows). Those go through the links too (through()).
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>

namespace hk::pc {

class WinePaths {
public:
    // Windows' name for `path` (this machine's, absolute). When one of its folders has letters
    // outside ASCII the name goes through a link, which make() makes: the folder linked is the
    // last such one (a file's own name is left as it is: its folder's link is what can be done)
    std::string name(const std::string &path) {
        auto windows = [](std::string p) {
            for (char &c : p)
                if (c == '/') c = '\\';
            return p;
        };
        size_t last = std::string::npos;   // the last byte outside ASCII
        for (size_t i = 0; i < path.size(); i++)
            if ((unsigned char)path[i] >= 0x80) last = i;
        if (last == std::string::npos || path.empty() || path[0] != '/') return windows("Z:" + path);
        size_t end = path.find('/', last);   // the end of that letter's part of the path
        if (end == std::string::npos) end = path.size();
        std::error_code ec;
        if (!std::filesystem::is_directory(path.substr(0, end), ec)) {   // a file (or nothing yet): the folder it is in
            const size_t start = path.rfind('/', last);
            return start == 0 ? windows("Z:" + path) : name(path.substr(0, start)) + windows(path.substr(start));
        }
        const std::string folder = path.substr(0, end);
        uint64_t h = 0xcbf29ce484222325ull;   // FNV-1a: the same folder, the same link, in every run
        for (unsigned char c : folder) h = (h ^ c) * 0x100000001b3ull;
        char number[20];
        snprintf(number, sizeof number, "%016llx", (unsigned long long)h);
        links_[number] = folder;
        return windows(std::string("C:/hiraku-paths/") + number + path.substr(end));
    }

    // This machine's name for `path` by the way of its link under Wine's folder `prefix`
    // (.../drive_c/hiraku-paths/<number>/...), or `path` itself when it needs none. For what Wine
    // itself is given by this machine's names (the folder it starts in): it reads those in the
    // locale's letters, and can't when the locale is one that isn't installed
    std::string through(const std::string &prefix, const std::string &path) {
        std::string n = name(path);
        if (n.rfind("C:\\", 0) != 0) return path;
        for (char &c : n)
            if (c == '\\') c = '/';
        return prefix + "/drive_c" + n.substr(2);
    }

    // The links, in Wine's folder `prefix` (which must be there: Wine makes it). False, and why,
    // if one couldn't be made
    bool make(const std::string &prefix, std::string *why = nullptr) const {
        if (links_.empty()) return true;
        const std::filesystem::path dir = std::filesystem::path(prefix) / "drive_c" / "hiraku-paths";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        for (const auto &[number, folder] : links_) {
            const std::filesystem::path at = dir / number;
            if (std::filesystem::is_symlink(at, ec)) {
                if (std::filesystem::read_symlink(at, ec) == std::filesystem::path(folder) && !ec) continue;
                std::filesystem::remove(at, ec);
            }
            std::filesystem::create_directory_symlink(folder, at, ec);
            if (ec) {
                if (why) *why = at.string() + " (for " + folder + "): " + ec.message();
                return false;
            }
        }
        return true;
    }

    bool any() const { return !links_.empty(); }

private:
    std::map<std::string, std::string> links_;   // the link's name -> the folder
};

}  // namespace hk::pc
