// SPDX-License-Identifier: GPL-3.0-or-later
// What the e-AMUSEMENT server (server.h) keeps, behind one interface: a thing kept is a table
// (tables.h: every kind of thing the server keeps) and a key. The folder back end keeps each as a
// file of the data folder, named from the table's pattern and the key, as HIRAKU always has; the
// file names are made here and in tables.h only.
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hk::ea {

// A kind of thing the server keeps, and how the folder back end names its file: a pattern with one
// {} for each part of the key ("pdata-{}-{}": a player's record by game and ref id; "cards": one file)
struct Table {
    const char *name;
    const char *file;
};
using Key = std::vector<std::string>;

class Store {
public:
    virtual ~Store() = default;
    // What is kept: "" if nothing, or for a key no file is named after
    virtual std::string get(const Table &, const Key &) const = 0;
    // Kept, whole or not at all. false: not kept (a key no file is named after, the limit, a failing
    // disk); it has said why
    virtual bool put(const Table &, const Key &, const std::string &) = 0;
    // What was there and isn't a record, kept beside it (and said), before a new record takes its place
    virtual void aside(const Table &, const Key &, const std::string &had) = 0;
    // The thing's name, for messages (the folder: the file's name)
    virtual std::string label(const Table &, const Key &) const = 0;
    // The owner's limit on `what` ("cards", "files"), or by_default
    virtual long limit(const char *what, long by_default) const = 0;
    struct Hold {
        virtual ~Hold() = default;
    };
    // The store is this call's until the Hold is gone: servers sharing it (each cabinet's own on one
    // PC, hiraku-eamuse's threads) read, change and write back the same things, and one's write would
    // undo another's
    virtual std::unique_ptr<Hold> hold() = 0;
};

// Plain files in a folder: records as lines of tab-separated key=value fields (protocol.h), a
// player's record as the game's bytes. dir "": nothing is kept.
// The folder's format is a number in its file "format" (none: 1), written with the folder's first
// new file. A folder of a newer format than this program's is read but never written to.
class FolderStore : public Store {
public:
    static constexpr long FORMAT = 1;   // this program's
    explicit FolderStore(std::string dir) : dir_(std::move(dir)) {}

    std::string get(const Table &, const Key &) const override;
    bool put(const Table &, const Key &, const std::string &) override;
    void aside(const Table &, const Key &, const std::string &had) override;
    std::string label(const Table &, const Key &) const override;
    long limit(const char *what, long by_default) const override;
    std::unique_ptr<Hold> hold() override;

    // A table's file name for a key: the pattern's {} each replaced by the key's next part ("" if
    // the key has another number of parts). Not checked: get and put refuse a name safe_file refuses.
    static std::string file_name(const Table &, const Key &);

private:
    std::string read(const std::string &name) const;
    bool write(const std::string &name, const std::string &data);
    bool room_for_file();   // a new file may be made (counted from the folder, once)
    bool writable();        // the folder's format is one this program writes (said once if not)
    void stamp();           // the folder's format written, if it has none

    long format_ = -1;      // the folder's format: -1 not read yet, 0 no file yet (format 1)
    bool newer_said_ = false;

    std::string dir_;
    // What a server others can reach keeps is bounded (a call registers a card, writes a card's
    // file): the data folder's files (limit-files, default 200000), a number in a file of the folder
    // for an owner who wants another
    long files_ = -1;   // the folder's files, as counted and made since
    bool files_full_said_ = false;
};

}  // namespace hk::ea
