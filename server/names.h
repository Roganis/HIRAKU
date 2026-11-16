// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-server's song names (docs/SERVER.md, "Song names"): packs the owner makes from their own
// games' data (tools/eamuse/names.py), one JSON file a game in a folder, read at run time for the
// web pages and the API. A game's songs are its own numbers (eamuse/readers.h's Score::song); the
// server keeps them by number and shows a name only where a pack gives one. Nothing of a pack goes
// into the database or the answers to the games. hiraku-server's only.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hk::srv {

struct SongName {
    std::string title, artist, genre;   // UTF-8; artist and genre "" when the pack gives none
};

struct NamePack {
    std::string game, file;               // the games table's id, the file's name
    std::map<long, SongName> songs;       // by the game's own song number
    std::map<long, std::string> charts;   // chart numbers' names, when the pack gives them
    int64_t time = 0;                     // the file's last change (seconds since 1970)
};

struct NameRefusal {
    std::string file, why;   // why: one plain line
};

// The packs of one folder, read again when a file changes (looked at most every few seconds, when
// asked). Thread-safe; a pack is shared as it was read, a later read replaces it whole.
class Names {
public:
    static constexpr size_t FILE_MAX = 4u << 20;   // a file's bytes at most
    static constexpr size_t SONGS_MAX = 20000;     // a pack's songs at most
    static constexpr size_t TEXT_MAX = 200;        // a text's characters at most (more cut)
    static constexpr int LOOK_SECONDS = 3;         // the folder looked at again at most this often

    // folder "": none (no names); quiet: no lines said
    explicit Names(std::string folder = "", bool quiet = false);
    // Look at the folder now (force) or when LOOK_SECONDS have passed: a changed file read again, a
    // gone one dropped; a line said for each pack read or refused
    void refresh(bool force = false);
    std::shared_ptr<const NamePack> pack(const std::string &game);   // null: none
    std::vector<std::shared_ptr<const NamePack>> packs();
    std::vector<NameRefusal> refused();
    const std::string &folder() const { return folder_; }

    // A pack's text (its file's bytes) read: the pack, or why not. known(id): the games table has it
    static bool parse(const std::string &bytes, const std::string &file, bool (*known)(const std::string &), NamePack &out, std::string &why);
    // A text as the server keeps it: valid UTF-8 (other bytes dropped), no control characters, at
    // most TEXT_MAX characters
    static std::string clean(const std::string &s);

private:
    struct Seen {
        int64_t time = 0;
        uintmax_t size = 0;
        std::shared_ptr<const NamePack> pack;   // null: refused
        std::string why;
    };
    std::string folder_;
    bool quiet_;
    std::mutex mu_;
    std::map<std::string, Seen> files_;   // by file name
    int64_t looked_ = 0;                  // steady seconds of the last look; 0: never
    bool missing_said_ = false;
};

}  // namespace hk::srv
