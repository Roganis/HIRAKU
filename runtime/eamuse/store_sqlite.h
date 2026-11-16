// SPDX-License-Identifier: GPL-3.0-or-later
// The e-AMUSEMENT server's database back end (store.h), for hiraku-server: everything kept in one
// SQLite 3 file, the system's library. Not the cores' (their servers keep a folder) and not
// hiraku-eamuse's.
//
// One table, so a new game's tables need no change to it:
//   kv(tbl TEXT, key TEXT, value BLOB, changed INTEGER, PRIMARY KEY(tbl, key))
// tbl a Table's name (tables.h), key the key's parts joined by '/' (a byte no part can hold: a key
// is taken only when the folder would take its file's name), value the bytes, changed when they were
// written (seconds since 1970). A copy kept aside (aside) is a row of "<name>.unread", the date its
// key's last part. The owner's limits are the setting table's rows ("limit"/"files"), as the folder's
// are its files. The file's format is PRAGMA user_version (1): a newer one is read, never written.
// WAL, so programs read while one writes; a store's call is one transaction (hold).
#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "eamuse/store.h"

struct sqlite3;
struct sqlite3_stmt;

namespace hk::ea {

class SqliteStore : public Store {
public:
    static constexpr long FORMAT = 1;   // this program's
    // The database in `file`, made if there is none; ok() false if it can't be opened (said)
    explicit SqliteStore(std::string file);
    ~SqliteStore() override;
    SqliteStore(const SqliteStore &) = delete;
    SqliteStore &operator=(const SqliteStore &) = delete;
    bool ok() const { return db_ != nullptr; }
    const std::string &file() const { return file_; }

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
    // rows of a table this program doesn't know (a newer one's), as tbl/key
    std::vector<std::string> strays() const override;

    // The whole database copied into `to`, a new file, by SQLite's online backup: safe while another
    // program serves from it. false: not made (error says why)
    bool backup(const std::string &to, std::string &error);

private:
    struct DbHold;
    std::string read(const std::string &tbl, const std::string &key) const;
    bool write(const std::string &tbl, const std::string &key, const std::string &data, const std::string &label);
    bool writable();        // the file's format is one this program writes (said once if not)
    bool room_for_row(const std::string &tbl, const std::string &key);   // a new row may be made
    void said(const char *what) const;   // SQLite's error, said

    std::string file_;
    sqlite3 *db_ = nullptr;
    long format_ = 0;
    bool newer_said_ = false, full_said_ = false;
    // a call's rows are one transaction: the store is one Hold's (one thread's) at a time
    mutable std::recursive_mutex mu_;
    int depth_ = 0;          // Holds alive (nested ones in the outer one's transaction)
    bool began_ = false;     // the outer one began a transaction
    sqlite3_stmt *get_ = nullptr, *put_ = nullptr, *has_ = nullptr, *count_ = nullptr, *keys_ = nullptr, *tables_ = nullptr;
};

}  // namespace hk::ea
