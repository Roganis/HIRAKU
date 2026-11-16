// SPDX-License-Identifier: GPL-3.0-or-later
// The e-AMUSEMENT server's database back end (store_sqlite.h): one SQLite 3 file, one table of rows
// named as the folder names its files.
#include "eamuse/store_sqlite.h"
#include "eamuse/games.h"
#include "eamuse/protocol.h"
#include "eamuse/tables.h"
#include "shell/sys.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace hk::ea {

namespace {

constexpr char SEP = '/';                    // between a key's parts (no part holds it: safe_file)
constexpr const char UNREAD[] = ".unread";   // a table's copies kept aside: "<name>.unread"

std::string joined(const Key &k) {
    std::string s;
    for (size_t i = 0; i < k.size(); i++) s += (i ? std::string(1, SEP) : "") + k[i];
    return s;
}

Key parts(const std::string &s) {
    Key k;
    if (s.empty()) return k;
    size_t at = 0;
    for (;;) {
        const size_t end = s.find(SEP, at);
        k.push_back(s.substr(at, end == std::string::npos ? std::string::npos : end - at));
        if (end == std::string::npos) return k;
        at = end + 1;
    }
}

const Table *table_named(const std::string &name) {
    for (const Table *t : tables::all)
        if (name == t->name) return t;
    return nullptr;
}

// A Named's row: its table's name (a copy aside's "<name>.unread") and key (the date its last part).
// false: a name the folder would refuse
bool row_of(const Named &n, std::string &tbl, std::string &key) {
    if (!n.table || !safe_file(file_name(n))) return false;
    tbl = std::string(n.table->name) + (n.aside.empty() ? "" : UNREAD);
    Key k = n.key;
    if (!n.aside.empty()) k.push_back(n.aside);
    key = joined(k);
    return true;
}

// A statement used once: its parameters bound as text or bytes, reset when done
struct Use {
    sqlite3_stmt *s;
    explicit Use(sqlite3_stmt *st) : s(st) {}
    ~Use() {
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
    }
    Use &text(int i, const std::string &v) {
        sqlite3_bind_text(s, i, v.data(), (int)v.size(), SQLITE_TRANSIENT);
        return *this;
    }
    Use &blob(int i, const std::string &v) {
        sqlite3_bind_blob(s, i, v.data(), (int)v.size(), SQLITE_TRANSIENT);
        return *this;
    }
    std::string column(int i) const {
        const void *p = sqlite3_column_blob(s, i);
        return p ? std::string(static_cast<const char *>(p), (size_t)sqlite3_column_bytes(s, i)) : std::string();
    }
};

}  // namespace

void SqliteStore::said(const char *what) const {
    fprintf(stderr, "e-AMUSEMENT: %s: %s: %s\n", file_.c_str(), what, db_ ? sqlite3_errmsg(db_) : "not open");
}

SqliteStore::SqliteStore(std::string file) : file_(std::move(file)) {
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(file_.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        fprintf(stderr, "e-AMUSEMENT: cannot open %s: %s\n", file_.c_str(), db ? sqlite3_errmsg(db) : "no memory");
        sqlite3_close(db);
        return;
    }
    db_ = db;
    sqlite3_busy_timeout(db_, 15000);   // (another program's call or backup holds it a moment)
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    // each call's rows on the disk when its answer goes, as the folder's files are
    sqlite3_exec(db_, "PRAGMA synchronous=FULL", nullptr, nullptr, nullptr);
    // the format, and the table if the file is new: in one transaction, two programs making it at once
    bool begun = sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_stmt *v = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA user_version", -1, &v, nullptr) == SQLITE_OK && sqlite3_step(v) == SQLITE_ROW)
        format_ = sqlite3_column_int64(v, 0);
    sqlite3_finalize(v);
    if (format_ <= FORMAT && begun) {
        const bool made = sqlite3_exec(db_, "CREATE TABLE IF NOT EXISTS kv(tbl TEXT, key TEXT, value BLOB, changed INTEGER, PRIMARY KEY(tbl, key))",
                                       nullptr, nullptr, nullptr) == SQLITE_OK;
        if (!made) said("cannot make its table");
        else if (format_ == 0 && sqlite3_exec(db_, "PRAGMA user_version=1", nullptr, nullptr, nullptr) == SQLITE_OK)
            format_ = FORMAT;
    }
    if (begun && sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        said("cannot make its table");
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    if (format_ == 0) format_ = FORMAT;   // (a file nothing can be written to: read as this program's)
    auto prepare = [&](const char *sql, sqlite3_stmt *&s) {
        if (sqlite3_prepare_v3(db_, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, nullptr) != SQLITE_OK) s = nullptr;
        return s != nullptr;
    };
    if (!prepare("SELECT value FROM kv WHERE tbl=?1 AND key=?2", get_) ||
        !prepare("INSERT INTO kv(tbl, key, value, changed) VALUES(?1, ?2, ?3, ?4) "
                 "ON CONFLICT(tbl, key) DO UPDATE SET value=excluded.value, changed=excluded.changed",
                 put_) ||
        !prepare("SELECT 1 FROM kv WHERE tbl=?1 AND key=?2", has_) || !prepare("SELECT count(*) FROM kv", count_) ||
        !prepare("SELECT key FROM kv WHERE tbl=?1", keys_) || !prepare("SELECT DISTINCT tbl FROM kv", tables_)) {
        said("not a database of HIRAKU's e-AMUSEMENT server");
        for (sqlite3_stmt *s : {get_, put_, has_, count_, keys_, tables_}) sqlite3_finalize(s);
        get_ = put_ = has_ = count_ = keys_ = tables_ = nullptr;
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

SqliteStore::~SqliteStore() {
    if (!db_) return;
    for (sqlite3_stmt *s : {get_, put_, has_, count_, keys_, tables_}) sqlite3_finalize(s);
    sqlite3_close(db_);
}

std::string SqliteStore::read(const std::string &tbl, const std::string &key) const {
    std::lock_guard<std::recursive_mutex> one(mu_);
    if (!db_) return "";
    Use u(get_);
    u.text(1, tbl).text(2, key);
    const int rc = sqlite3_step(get_);
    if (rc == SQLITE_ROW) return u.column(0);
    if (rc != SQLITE_DONE) said("cannot read");
    return "";
}

bool SqliteStore::writable() {
    if (format_ <= FORMAT) return true;
    if (!newer_said_)
        fprintf(stderr, "e-AMUSEMENT: %s was made by a newer HIRAKU (its format %ld, this one's %ld): it is left as it is, nothing is written to it\n",
                file_.c_str(), format_, FORMAT);
    newer_said_ = true;
    return false;
}

bool SqliteStore::room_for_row(const std::string &tbl, const std::string &key) {
    if (!bounded) return true;
    {
        Use u(has_);
        u.text(1, tbl).text(2, key);
        if (sqlite3_step(has_) == SQLITE_ROW) return true;   // (a row there already is only written)
    }
    long rows = 0;
    {
        Use u(count_);
        if (sqlite3_step(count_) == SQLITE_ROW) rows = (long)sqlite3_column_int64(count_, 0);
    }
    const long most = limit("files", std::strtol(find_setting(nullptr, "limit-files")->by_default, nullptr, 10));
    if (rows < most) return true;
    if (!full_said_)
        fprintf(stderr, "e-AMUSEMENT: %s holds %ld things, as many as it may (the setting limit-files sets another number): nothing new is kept\n",
                file_.c_str(), rows);
    full_said_ = true;
    return false;
}

bool SqliteStore::write(const std::string &tbl, const std::string &key, const std::string &data, const std::string &label) {
    const auto held = hold();   // (the limit's count and the row: one transaction)
    if (!db_ || !writable() || !room_for_row(tbl, key)) return false;
    Use u(put_);
    u.text(1, tbl).text(2, key).blob(3, data);
    sqlite3_bind_int64(put_, 4, (sqlite3_int64)time(nullptr));
    if (sqlite3_step(put_) != SQLITE_DONE) {
        fprintf(stderr, "e-AMUSEMENT: cannot write %s in %s (%s): the one from before is kept\n", label.c_str(), file_.c_str(), sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

std::string SqliteStore::get(const Table &t, const Key &k) const { return get_named(Named{&t, k, ""}); }

bool SqliteStore::put(const Table &t, const Key &k, const std::string &data) {
    std::string tbl, key;
    if (!row_of(Named{&t, k, ""}, tbl, key)) {   // (a call's field never names what a folder couldn't)
        fprintf(stderr, "e-AMUSEMENT: not written: a call's field made no file name (%zu bytes)\n", file_name(t, k).size());
        return false;
    }
    return write(tbl, key, data, label(t, k));
}

std::string SqliteStore::get_named(const Named &n) const {
    std::string tbl, key;
    return row_of(n, tbl, key) ? read(tbl, key) : "";
}

bool SqliteStore::put_named(const Named &n, const std::string &data) {
    std::string tbl, key;
    if (!row_of(n, tbl, key)) return false;
    return write(tbl, key, data, tbl + SEP + key);
}

std::string SqliteStore::label(const Table &t, const Key &k) const { return k.empty() ? t.name : std::string(t.name) + SEP + joined(k); }

long SqliteStore::limit(const char *what, long by_default) const {
    const std::string set = get(tables::setting, {"limit", what});
    char *end = nullptr;
    const long n = std::strtol(set.c_str(), &end, 10);
    return end != set.c_str() && n >= 0 ? n : by_default;
}

std::vector<Key> SqliteStore::keys(const Table &t) const {
    std::lock_guard<std::recursive_mutex> one(mu_);
    std::vector<Key> out;
    if (!db_) return out;
    Use u(keys_);
    u.text(1, t.name);
    while (sqlite3_step(keys_) == SQLITE_ROW) out.push_back(parts(u.column(0)));
    std::sort(out.begin(), out.end());
    return out;
}

// Like the folder's: what was there and isn't a record kept as a row of NAME.unread, said
void SqliteStore::aside(const Table &t, const Key &k, const std::string &had) {
    if (had.empty()) return;
    char date[32];
    const time_t now = time(nullptr);
    strftime(date, sizeof date, "%Y%m%d-%H%M%S", localtime(&now));
    const Named n{&t, k, date};
    put_named(n, had);
    std::string tbl, key;
    row_of(n, tbl, key);
    fprintf(stderr, "e-AMUSEMENT: %s isn't a player's file as this HIRAKU keeps them (%zu bytes): kept as %s, a new one made\n",
            label(t, k).c_str(), had.size(), (tbl + SEP + key).c_str());
}

std::vector<Named> SqliteStore::asides() const {
    std::lock_guard<std::recursive_mutex> one(mu_);
    std::vector<Named> out;
    if (!db_) return out;
    std::vector<std::string> names;
    {
        Use u(tables_);
        while (sqlite3_step(tables_) == SQLITE_ROW) names.push_back(u.column(0));
    }
    const size_t n = sizeof UNREAD - 1;
    for (const std::string &tbl : names) {
        if (tbl.size() <= n || tbl.compare(tbl.size() - n, n, UNREAD) != 0) continue;
        const Table *t = table_named(tbl.substr(0, tbl.size() - n));
        if (!t) continue;
        Use u(keys_);
        u.text(1, tbl);
        while (sqlite3_step(keys_) == SQLITE_ROW) {
            Key k = parts(u.column(0));
            if (k.empty()) continue;
            const std::string date = k.back();
            k.pop_back();
            out.push_back(Named{t, k, date});
        }
    }
    return out;
}

std::vector<std::string> SqliteStore::strays() const {
    std::lock_guard<std::recursive_mutex> one(mu_);
    std::vector<std::string> out;
    if (!db_) return out;
    std::vector<std::string> names;
    {
        Use u(tables_);
        while (sqlite3_step(tables_) == SQLITE_ROW) names.push_back(u.column(0));
    }
    const size_t n = sizeof UNREAD - 1;
    for (const std::string &tbl : names) {
        const bool aside = tbl.size() > n && tbl.compare(tbl.size() - n, n, UNREAD) == 0;
        if (table_named(aside ? tbl.substr(0, tbl.size() - n) : tbl)) continue;
        Use u(keys_);
        u.text(1, tbl);
        while (sqlite3_step(keys_) == SQLITE_ROW) out.push_back(tbl + SEP + u.column(0));
    }
    std::sort(out.begin(), out.end());
    return out;
}

struct SqliteStore::DbHold : Store::Hold {
    SqliteStore &st;
    std::unique_lock<std::recursive_mutex> one;
    explicit DbHold(SqliteStore &s) : st(s), one(s.mu_) {
        if (st.depth_++ == 0 && st.db_) {
            st.began_ = sqlite3_exec(st.db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK;
            if (!st.began_) st.said("cannot begin a transaction");
        }
    }
    ~DbHold() override {
        if (--st.depth_ == 0 && st.began_) {
            if (sqlite3_exec(st.db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
                st.said("cannot write what a call changed");
                sqlite3_exec(st.db_, "ROLLBACK", nullptr, nullptr, nullptr);
            }
            st.began_ = false;
        }
    }
};

std::unique_ptr<Store::Hold> SqliteStore::hold() { return std::make_unique<DbHold>(*this); }

bool SqliteStore::backup(const std::string &to, std::string &error) {
    std::lock_guard<std::recursive_mutex> one(mu_);
    if (!db_) { error = "the database isn't open"; return false; }
    if (sys::exists(to)) { error = to + " is there already"; return false; }
    sqlite3 *out = nullptr;
    if (sqlite3_open_v2(to.c_str(), &out, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        error = out ? sqlite3_errmsg(out) : "no memory";
        sqlite3_close(out);
        return false;
    }
    sqlite3_backup *b = sqlite3_backup_init(out, "main", db_, "main");
    int rc = b ? SQLITE_OK : SQLITE_ERROR;
    if (b) {
        // a page at a time would start again whenever the server writes: the whole file in one step,
        // waiting while another program holds the database
        for (int tries = 0; tries < 600; tries++) {
            rc = sqlite3_backup_step(b, -1);
            if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED) break;
            sys::sleep_for(std::chrono::milliseconds(50));
        }
        sqlite3_backup_finish(b);
    }
    if (rc != SQLITE_DONE) error = sqlite3_errmsg(out);
    if (sqlite3_close(out) != SQLITE_OK && rc == SQLITE_DONE) error = "cannot close " + to, rc = SQLITE_ERROR;
    if (rc != SQLITE_DONE) std::remove(to.c_str());
    return rc == SQLITE_DONE;
}

}  // namespace hk::ea
