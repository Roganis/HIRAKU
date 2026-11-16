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

// Folder names, the exchange format: what the folder back end calls a thing kept is how every back
// end, import and export (copy_store below) and hiraku-server's get and put name it. A thing by
// such a name: its table and key, or a copy of it kept aside (Store::aside), with that copy's date
struct Named {
    const Table *table = nullptr;
    Key key;
    std::string aside;   // the date of a copy kept aside ("20261116-120000"); "": the thing itself
};
// A table's file name for a key: the pattern's {} each replaced by the key's next part ("" if the key
// has another number of parts). Not checked: the stores refuse a name safe_file refuses.
std::string file_name(const Table &, const Key &);
// ... and a Named's: NAME, or NAME.unread-DATE for a copy kept aside ("" for none)
std::string file_name(const Named &);
// The table a file of the folder belongs to, and its key; null for a file no table names. A file is
// one table's: a pattern with no {} that is the name wins; else the matching pattern with the most
// letters of its own, a key's part never holding a '-' ("gfdmv-cards" is the singleton, not gfdmv-{};
// "userdata-C20-X" userdata-{}-{}; the owner's {}-{} takes only what nothing else does)
const Table *table_of(const std::string &file, Key &key);
// ... and a copy kept aside too (NAME.unread-DATE). false: a file no table names (format, .lock, the
// owner's own files)
bool named(const std::string &file, Named &);

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
    // Every key the store holds of a table (in no promised order)
    virtual std::vector<Key> keys(const Table &) const = 0;
    struct Hold {
        virtual ~Hold() = default;
    };
    // The store is this call's until the Hold is gone: servers sharing it (each cabinet's own on one
    // PC, hiraku-eamuse's threads) read, change and write back the same things, and one's write would
    // undo another's
    virtual std::unique_ptr<Hold> hold() = 0;

    // The exchange (folder names, above): every copy kept aside (aside), and a thing or such a copy
    // read or written by its name. put_named is put's for a thing (the same checks), and for a copy
    // aside's without the message
    virtual std::vector<Named> asides() const = 0;
    virtual std::string get_named(const Named &) const = 0;
    virtual bool put_named(const Named &, const std::string &) = 0;
    // What it holds that no table names, by name (the folder: its files of no table's, format and
    // .lock aside), for import's report
    virtual std::vector<std::string> strays() const { return {}; }
    // false while a store is copied into it (copy_store): what the other kept comes in whatever the
    // limit on things kept says (the owner's limits come in with the rest, in no order)
    bool bounded = true;
};

// Everything `from` keeps put in `to` (hiraku-server's import and export): every key of every table
// of tables::all, the owner's settings and limits among them, and the copies kept aside, each thing's
// bytes as they are. A folder exported from a database imported from a folder holds the same files
// with the same bytes (its format file and .lock aside)
struct Copied {
    size_t things = 0;                     // put in `to`
    std::vector<std::string> not_placed;   // what `from` holds that no table names (by name only)
    std::vector<std::string> not_put;      // what `to` refused (by name)
};
Copied copy_store(const Store &from, Store &to);

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
    std::vector<Key> keys(const Table &) const override;
    std::unique_ptr<Hold> hold() override;
    std::vector<Named> asides() const override;
    std::string get_named(const Named &) const override;
    bool put_named(const Named &, const std::string &) override;
    std::vector<std::string> strays() const override;

    // (store.h's file_name and table_of, by their old names)
    static std::string file_name(const Table &t, const Key &k) { return ea::file_name(t, k); }
    static const Table *table_of(const std::string &file, Key &key) { return ea::table_of(file, key); }

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
    // file): the data folder's files (limit-files: its default is games.cpp's server_settings'), a
    // number in a file of the folder for an owner who wants another
    long files_ = -1;   // the folder's files, as counted and made since
    bool files_full_said_ = false;
};

}  // namespace hk::ea
