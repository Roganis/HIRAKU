// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-server's accounts, cards, arcades and machines (accounts.h).
#include "accounts.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "eamuse/access.h"
#include "eamuse/games.h"
#include "eamuse/protocol.h"
#include "eamuse/tables.h"
#include "shell/crypt.h"

namespace hk::srv {

namespace {

constexpr const char *TABLES =
    "CREATE TABLE IF NOT EXISTS meta(name TEXT PRIMARY KEY, value TEXT);"
    "CREATE TABLE IF NOT EXISTS users(id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE COLLATE NOCASE, pass TEXT NOT NULL,"
    " role TEXT NOT NULL, disabled INTEGER NOT NULL DEFAULT 0, hide_stats INTEGER NOT NULL DEFAULT 0,"
    " must_change INTEGER NOT NULL DEFAULT 0, created INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS cards(card TEXT PRIMARY KEY COLLATE NOCASE, user INTEGER NOT NULL, linked INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS card_locks(card TEXT PRIMARY KEY COLLATE NOCASE, tries INTEGER NOT NULL, locked INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS arcades(id INTEGER PRIMARY KEY, owner INTEGER, name TEXT NOT NULL, pref INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS machines(pcbid TEXT PRIMARY KEY, arcade INTEGER NOT NULL, name TEXT NOT NULL,"
    " enabled INTEGER NOT NULL, last_seen INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS resets(code_hash TEXT PRIMARY KEY, user INTEGER NOT NULL, expires INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS invites(code_hash TEXT PRIMARY KEY, expires INTEGER NOT NULL, uses_left INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS config(name TEXT PRIMARY KEY, value TEXT);"
    "CREATE TABLE IF NOT EXISTS audit(id INTEGER PRIMARY KEY, at INTEGER NOT NULL, user TEXT NOT NULL, action TEXT NOT NULL,"
    " detail TEXT NOT NULL);";

// A statement used once
struct St {
    sqlite3_stmt *s = nullptr;
    St(sqlite3 *db, const char *sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) s = nullptr;
    }
    ~St() { sqlite3_finalize(s); }
    St(const St &) = delete;
    St &operator=(const St &) = delete;
    St &bind(int i, const std::string &v) {
        if (s) sqlite3_bind_text(s, i, v.data(), (int)v.size(), SQLITE_TRANSIENT);
        return *this;
    }
    St &bind(int i, int64_t v) {
        if (s) sqlite3_bind_int64(s, i, v);
        return *this;
    }
    bool row() { return s && sqlite3_step(s) == SQLITE_ROW; }
    bool run() {
        if (!s) return false;
        const int r = sqlite3_step(s);
        return r == SQLITE_DONE || r == SQLITE_ROW;
    }
    std::string text(int i) const {
        const unsigned char *p = sqlite3_column_text(s, i);
        return p ? std::string(reinterpret_cast<const char *>(p), (size_t)sqlite3_column_bytes(s, i)) : std::string();
    }
    int64_t num(int i) const { return sqlite3_column_int64(s, i); }
};

int64_t now() { return (int64_t)time(nullptr); }

std::string hex(const uint8_t *p, size_t n) {
    static const char *h = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) s += h[p[i] >> 4], s += h[p[i] & 15];
    return s;
}

bool unhex(const std::string &s, std::string &out) {
    out.clear();
    if (s.size() % 2) return false;
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t i = 0; i < s.size(); i += 2) {
        const int a = v(s[i]), b = v(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out += (char)(a << 4 | b);
    }
    return true;
}

bool same(const std::string &a, const std::string &b) {   // in a time that doesn't tell where they differ
    unsigned char d = a.size() == b.size() ? 0 : 1;
    for (size_t i = 0; i < a.size(); i++) d |= (unsigned char)(a[i] ^ (i < b.size() ? b[i] : 0));
    return d == 0;
}

std::string sha_hex(const std::string &s) {
    uint8_t h[32];
    crypt::sha256(s.data(), s.size(), h);
    return hex(h, 32);
}

// A password's kept form: pbkdf2-sha256$ROUNDS$SALT$HASH (hex); "" if the system gives no random bytes
std::string hash_password(const std::string &password) {
    uint8_t salt[16], out[32];
    if (!crypt::random_bytes(salt, sizeof salt)) return "";
    const std::string s(reinterpret_cast<const char *>(salt), sizeof salt);
    crypt::pbkdf2_sha256(password, s, Accounts::ROUNDS, out);
    return "pbkdf2-sha256$" + std::to_string(Accounts::ROUNDS) + "$" + hex(salt, sizeof salt) + "$" + hex(out, sizeof out);
}

bool password_is(const std::string &kept, const std::string &password) {
    const char *prefix = "pbkdf2-sha256$";
    if (kept.rfind(prefix, 0) != 0) return false;
    const size_t a = kept.find('$', std::strlen(prefix)), b = a == std::string::npos ? a : kept.find('$', a + 1);
    if (b == std::string::npos) return false;
    const long rounds = std::strtol(kept.c_str() + std::strlen(prefix), nullptr, 10);
    std::string salt, want;
    if (rounds < 1 || rounds > 100000000 || !unhex(kept.substr(a + 1, b - a - 1), salt) || !unhex(kept.substr(b + 1), want) || want.size() != 32)
        return false;
    uint8_t got[32];
    crypt::pbkdf2_sha256(password, salt, (uint32_t)rounds, got);
    return same(std::string(reinterpret_cast<const char *>(got), 32), want);
}

// 128 bits nobody can guess, as 32 hex digits ("" if the system gives none)
std::string new_code() {
    uint8_t b[16];
    return crypt::random_bytes(b, sizeof b) ? hex(b, sizeof b) : "";
}

// A password for the first admin: 20 letters and digits, none taken for another
std::string new_password() {
    static const char abc[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    constexpr size_t N = sizeof abc - 1;
    std::string out;
    while (out.size() < 20) {
        uint8_t b[32];
        if (!crypt::random_bytes(b, sizeof b)) return "";
        for (uint8_t v : b)
            if (v < 256 / N * N && out.size() < 20) out += abc[v % N];
    }
    return out;
}

Accounts::Refusal name_wrong(const std::string &name) {
    if (name.empty() || name.size() > 32) return "a name is 1 to 32 letters, digits, '.', '_' or '-'";
    for (char c : name)
        if (!std::isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') return "a name is 1 to 32 letters, digits, '.', '_' or '-'";
    if (name[0] == '-' || name[0] == '.') return "a name doesn't begin with '-' or '.'";
    return "";
}

Accounts::Refusal password_wrong(const std::string &p) {
    if (p.size() < 8) return "a password is 8 characters at least";
    if (p.size() > 256) return "a password is 256 bytes at most";
    if (p.find_first_of("\r\n", 0) != std::string::npos || p.find('\0') != std::string::npos) return "a password is one line";
    return "";
}

// An arcade's or a machine's name: one line of text, 1 to 64 bytes, no control characters
Accounts::Refusal label_wrong(const std::string &n) {
    if (n.empty() || n.size() > 64) return "a name is 1 to 64 bytes";
    for (unsigned char c : n)
        if (c < 0x20 || c == 0x7f) return "a name is one line, with no tab or control character";
    return "";
}

bool pref_ok(int pref) { return pref >= 1 && pref <= 47; }

std::string upper(std::string s) {
    for (char &c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

bool same_card(const std::string &a, const std::string &b) { return upper(a) == upper(b); }

const char *const NOT_ALLOWED = "not allowed for this account";

}  // namespace

const char *role_name(Role r) { return r == Role::admin ? "admin" : r == Role::op ? "operator" : "player"; }

bool role_named(const std::string &s, Role &r) {
    if (s == "admin") r = Role::admin;
    else if (s == "operator") r = Role::op;
    else if (s == "player") r = Role::player;
    else return false;
    return true;
}

// One change: its rows and its audit line together, or none
struct Accounts::Tx {
    sqlite3 *db;
    bool begun, done = false;
    explicit Tx(sqlite3 *d) : db(d), begun(sqlite3_exec(d, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK) {}
    bool commit() {
        done = begun && sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK;
        return done;
    }
    ~Tx() {
        if (begun && !done) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    }
};

Accounts::Accounts(const std::string &file, ea::Store &store) : file_(file), store_(store) {
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(file.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        fprintf(stderr, "hiraku-server: cannot open %s: %s\n", file.c_str(), db ? sqlite3_errmsg(db) : "no memory");
        sqlite3_close(db);
        return;
    }
    db_ = db;
    sqlite3_busy_timeout(db_, 15000);
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=FULL", nullptr, nullptr, nullptr);
    Tx tx(db_);
    {
        St v(db_, "SELECT value FROM meta WHERE name='accounts'");
        if (v.row()) version_ = std::strtol(v.text(0).c_str(), nullptr, 10);
    }
    if (version_ > VERSION) return;   // a newer HIRAKU's: read, never written (writable)
    if (!exec(TABLES)) {
        fprintf(stderr, "hiraku-server: %s: cannot make the accounts' tables: %s\n", file.c_str(), error().c_str());
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }
    if (version_ == 0) {   // the tables are new: the first admin
        St n(db_, "SELECT COUNT(*) FROM users");
        const bool none = n.row() && n.num(0) == 0;
        std::string password = none ? new_password() : "", kept = password.empty() ? "" : hash_password(password);
        if (none && kept.empty()) {
            fprintf(stderr, "hiraku-server: no first admin made (the system gives no random numbers)\n");
            sqlite3_close(db_);
            db_ = nullptr;
            return;
        }
        if (none) {
            St u(db_, "INSERT INTO users(name, pass, role, must_change, created) VALUES('admin', ?1, 'admin', 1, ?2)");
            u.bind(1, kept).bind(2, now());
            if (!u.run()) password.clear();
        }
        St m(db_, "INSERT OR REPLACE INTO meta(name, value) VALUES('accounts', ?1)");
        m.bind(1, std::to_string(VERSION));
        if (m.run() && tx.commit()) {
            version_ = VERSION;
            first_password_ = password;
            if (none) log(Who::command_line(), "user.add", "admin (admin, the first)");
        }
        return;
    }
    tx.commit();
}

Accounts::~Accounts() { sqlite3_close(db_); }

bool Accounts::exec(const char *sql) { return sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }
std::string Accounts::error() const { return db_ ? sqlite3_errmsg(db_) : "not open"; }

bool Accounts::writable(Refusal &why) const {
    if (!db_) why = "the database isn't open";
    else if (version_ > VERSION) why = "the accounts were made by a newer HIRAKU: nothing is changed";
    else return true;
    return false;
}

void Accounts::log(const Who &who, const std::string &action, const std::string &detail) {
    St a(db_, "INSERT INTO audit(at, user, action, detail) VALUES(?1, ?2, ?3, ?4)");
    a.bind(1, now()).bind(2, who.name).bind(3, action).bind(4, detail);
    a.run();
}

std::string Accounts::config(const char *name) const {
    St c(db_, "SELECT value FROM config WHERE name=?1");
    c.bind(1, std::string(name));
    return c.row() ? c.text(0) : "";
}

bool Accounts::find_user(const std::string &name, User &u) const {
    St q(db_, "SELECT id, name, role, disabled, hide_stats, must_change, created FROM users WHERE name=?1");
    q.bind(1, name);
    if (!q.row()) return false;
    u.id = (long)q.num(0), u.name = q.text(1);
    if (!role_named(q.text(2), u.role)) u.role = Role::player;
    u.disabled = q.num(3), u.hide_stats = q.num(4), u.must_change = q.num(5), u.created = q.num(6);
    return true;
}

bool Accounts::admins_left_without(long id) const {
    St q(db_, "SELECT COUNT(*) FROM users WHERE role='admin' AND disabled=0 AND id<>?1");
    q.bind(1, (int64_t)id);
    return q.row() && q.num(0) > 0;
}

Accounts::Refusal Accounts::as(const std::string &name, Who &who) {
    User u;
    if (!db_) return "the database isn't open";
    if (!find_user(name, u)) return "no user " + name;
    if (u.disabled) return "the user " + u.name + " is disabled";
    who = Who{u.id, u.role, u.name, false};
    return "";
}

// --- users

Accounts::Refusal Accounts::sign_up(const Who &who, const std::string &name, const std::string &password, const std::string &invite,
                                    Role role) {
    Refusal why;
    if (!writable(why)) return why;
    if (!(why = name_wrong(name)).empty() || !(why = password_wrong(password)).empty()) return why;
    if (role != Role::player && !who.admin()) return "only an admin gives a new account another role than player";
    Tx tx(db_);
    if (!tx.begun) return "the database: " + error();
    std::string code_hash;
    if (!who.admin()) {
        const SignUp mode = sign_up_mode();
        if (mode == SignUp::closed) return "sign-up is closed";
        if (mode == SignUp::invite) {
            if (invite.empty()) return "sign-up is by invitation: an invitation's code is needed";
            code_hash = sha_hex(invite);
            St q(db_, "SELECT uses_left FROM invites WHERE code_hash=?1 AND expires>?2 AND uses_left>0");
            q.bind(1, code_hash).bind(2, now());
            if (!q.row()) return "not an invitation's code (or used up, or past its day)";
        }
    }
    User there;
    if (find_user(name, there)) return "the name " + name + " is taken";
    const std::string kept = hash_password(password);
    if (kept.empty()) return "the system gives no random numbers";
    St u(db_, "INSERT INTO users(name, pass, role, created) VALUES(?1, ?2, ?3, ?4)");
    u.bind(1, name).bind(2, kept).bind(3, std::string(role_name(role))).bind(4, now());
    if (!u.run()) return "the database: " + error();
    if (!code_hash.empty()) {
        St d(db_, "UPDATE invites SET uses_left=uses_left-1 WHERE code_hash=?1");
        d.bind(1, code_hash).run();
    }
    log(who, "user.add", name + " (" + role_name(role) + (code_hash.empty() ? "" : ", by invitation") + ")");
    return tx.commit() ? "" : "the database: " + error();
}

Accounts::Refusal Accounts::check_password(const std::string &name, const std::string &password, Who &who, bool &must_change) {
    if (!db_) return "the database isn't open";
    St q(db_, "SELECT id, name, role, disabled, must_change, pass FROM users WHERE name=?1");
    q.bind(1, name);
    // (a name that isn't there costs as long as a wrong password: nobody learns which names are)
    const bool there = q.row();
    const std::string kept = there ? q.text(5) : "pbkdf2-sha256$" + std::to_string(ROUNDS) + "$00$" + std::string(64, '0');
    const bool right = password_is(kept, password) && there;
    if (!right) return "wrong name or password";
    if (q.num(3)) return "the user " + q.text(1) + " is disabled";
    Role r = Role::player;
    role_named(q.text(2), r);
    who = Who{(long)q.num(0), r, q.text(1), false};
    must_change = q.num(4) != 0;
    return "";
}

Accounts::Refusal Accounts::change_password(const Who &who, const std::string &user, const std::string &old_password,
                                            const std::string &password) {
    Refusal why;
    if (!writable(why)) return why;
    User u;
    if (!find_user(user, u)) return "no user " + user;
    const bool own = who.user() && who.id == u.id;
    if (!own && !who.admin()) return NOT_ALLOWED;
    if (!(why = password_wrong(password)).empty()) return why;
    if (own) {
        St q(db_, "SELECT pass FROM users WHERE id=?1");
        q.bind(1, (int64_t)u.id);
        if (!q.row() || !password_is(q.text(0), old_password)) return "the old password isn't right";
    }
    const std::string kept = hash_password(password);
    if (kept.empty()) return "the system gives no random numbers";
    Tx tx(db_);
    St s(db_, "UPDATE users SET pass=?1, must_change=?2 WHERE id=?3");
    s.bind(1, kept).bind(2, (int64_t)(own ? 0 : 1)).bind(3, (int64_t)u.id);
    if (!s.run()) return "the database: " + error();
    log(who, "user.password", u.name + (own ? "" : " (to be changed by its user)"));
    return tx.commit() ? "" : "the database: " + error();
}

Accounts::Refusal Accounts::set_role(const Who &who, const std::string &user, Role role) {
    Refusal why;
    if (!writable(why)) return why;
    if (!who.admin()) return NOT_ALLOWED;
    User u;
    if (!find_user(user, u)) return "no user " + user;
    Tx tx(db_);
    if (u.role == Role::admin && role != Role::admin && !admins_left_without(u.id)) return "the last admin stays one";
    St s(db_, "UPDATE users SET role=?1 WHERE id=?2");
    s.bind(1, std::string(role_name(role))).bind(2, (int64_t)u.id);
    if (!s.run()) return "the database: " + error();
    log(who, "user.role", u.name + " " + role_name(u.role) + " -> " + role_name(role));
    return tx.commit() ? "" : "the database: " + error();
}

Accounts::Refusal Accounts::set_disabled(const Who &who, const std::string &user, bool disabled) {
    Refusal why;
    if (!writable(why)) return why;
    if (!who.admin()) return NOT_ALLOWED;
    User u;
    if (!find_user(user, u)) return "no user " + user;
    Tx tx(db_);
    if (disabled && u.role == Role::admin && !admins_left_without(u.id)) return "the last admin stays enabled";
    St s(db_, "UPDATE users SET disabled=?1 WHERE id=?2");
    s.bind(1, (int64_t)disabled).bind(2, (int64_t)u.id);
    if (!s.run()) return "the database: " + error();
    log(who, disabled ? "user.disable" : "user.enable", u.name);
    return tx.commit() ? "" : "the database: " + error();
}

Accounts::Refusal Accounts::set_hide_stats(const Who &who, const std::string &user, bool hide) {
    Refusal why;
    if (!writable(why)) return why;
    User u;
    if (!find_user(user, u)) return "no user " + user;
    if (!(who.user() && who.id == u.id) && !who.admin()) return NOT_ALLOWED;
    Tx tx(db_);
    St s(db_, "UPDATE users SET hide_stats=?1 WHERE id=?2");
    s.bind(1, (int64_t)hide).bind(2, (int64_t)u.id);
    if (!s.run()) return "the database: " + error();
    log(who, "user.hide_stats", u.name + (hide ? " on" : " off"));
    return tx.commit() ? "" : "the database: " + error();
}

Accounts::Refusal Accounts::remove_user(const Who &who, const std::string &user) {
    Refusal why;
    if (!writable(why)) return why;
    User u;
    if (!find_user(user, u)) return "no user " + user;
    if (!(who.user() && who.id == u.id) && !who.admin()) return NOT_ALLOWED;
    Tx tx(db_);
    if (u.role == Role::admin && !admins_left_without(u.id)) return "the last admin stays";
    size_t cards = 0;
    {
        St n(db_, "SELECT COUNT(*) FROM cards WHERE user=?1");
        n.bind(1, (int64_t)u.id);
        if (n.row()) cards = (size_t)n.num(0);
    }
    const int64_t id = u.id;
    bool ok = St(db_, "DELETE FROM cards WHERE user=?1").bind(1, id).run() && St(db_, "DELETE FROM resets WHERE user=?1").bind(1, id).run() &&
              St(db_, "UPDATE arcades SET owner=NULL WHERE owner=?1").bind(1, id).run() && St(db_, "DELETE FROM users WHERE id=?1").bind(1, id).run();
    if (!ok) return "the database: " + error();
    log(who, "user.remove", u.name + " (" + std::to_string(cards) + " card" + (cards == 1 ? "" : "s") + " unlinked)");
    return tx.commit() ? "" : "the database: " + error();
}

Accounts::Refusal Accounts::users(const Who &who, std::vector<User> &out) {
    out.clear();
    if (!db_) return "the database isn't open";
    if (!who.admin()) return NOT_ALLOWED;
    St q(db_, "SELECT name FROM users ORDER BY id");
    while (q.row()) {
        User u;
        if (find_user(q.text(0), u)) out.push_back(u);
    }
    return "";
}

Accounts::Refusal Accounts::make_reset(const Who &who, const std::string &user, std::string &code) {
    code.clear();
    Refusal why;
    if (!writable(why)) return why;
    if (!who.admin()) return NOT_ALLOWED;
    User u;
    if (!find_user(user, u)) return "no user " + user;
    const std::string c = new_code();
    if (c.empty()) return "the system gives no random numbers";
    Tx tx(db_);
    // (one code a user at a time: a new one takes the old one's place)
    if (!St(db_, "DELETE FROM resets WHERE user=?1 OR expires<=?2").bind(1, (int64_t)u.id).bind(2, now()).run() ||
        !St(db_, "INSERT INTO resets(code_hash, user, expires) VALUES(?1, ?2, ?3)").bind(1, sha_hex(c)).bind(2, (int64_t)u.id).bind(3, now() + 86400).run())
        return "the database: " + error();
    log(who, "user.reset_code", u.name);
    if (!tx.commit()) return "the database: " + error();
    code = c;
    return "";
}

Accounts::Refusal Accounts::use_reset(const std::string &user, const std::string &code, const std::string &password) {
    Refusal why;
    if (!writable(why)) return why;
    User u;
    if (!find_user(user, u)) return "not a reset code of that user's (or past its day)";
    if (!(why = password_wrong(password)).empty()) return why;
    Tx tx(db_);
    const std::string h = sha_hex(code);
    St q(db_, "SELECT user FROM resets WHERE code_hash=?1 AND expires>?2");
    q.bind(1, h).bind(2, now());
    if (!q.row() || q.num(0) != u.id) return "not a reset code of that user's (or past its day)";
    const std::string kept = hash_password(password);
    if (kept.empty()) return "the system gives no random numbers";
    if (!St(db_, "DELETE FROM resets WHERE code_hash=?1").bind(1, h).run() ||
        !St(db_, "UPDATE users SET pass=?1, must_change=0 WHERE id=?2").bind(1, kept).bind(2, (int64_t)u.id).run())
        return "the database: " + error();
    log(Who{u.id, u.role, u.name, false}, "user.reset_used", u.name);
    return tx.commit() ? "" : "the database: " + error();
}

Accounts::SignUp Accounts::sign_up_mode() const {
    const std::string m = db_ ? config("signup") : "";
    return m == "closed" ? SignUp::closed : m == "invite" ? SignUp::invite : SignUp::open;
}

Accounts::Refusal Accounts::set_sign_up_mode(const Who &who, SignUp mode) {
    Refusal why;
    if (!writable(why)) return why;
    if (!who.admin()) return NOT_ALLOWED;
    const std::string m = mode == SignUp::closed ? "closed" : mode == SignUp::invite ? "invite" : "open";
    Tx tx(db_);
    if (!St(db_, "INSERT OR REPLACE INTO config(name, value) VALUES('signup', ?1)").bind(1, m).run()) return "the database: " + error();
    log(who, "config.signup", m);
    return tx.commit() ? "" : "the database: " + error();
}

Accounts::Refusal Accounts::make_invite(const Who &who, int uses, int days, std::string &code) {
    code.clear();
    Refusal why;
    if (!writable(why)) return why;
    if (!who.admin()) return NOT_ALLOWED;
    if (uses < 1 || uses > 10000) return "an invitation is for 1 to 10000 sign-ups";
    if (days < 1 || days > 365) return "an invitation lasts 1 to 365 days";
    const std::string c = new_code();
    if (c.empty()) return "the system gives no random numbers";
    Tx tx(db_);
    St(db_, "DELETE FROM invites WHERE expires<=?1 OR uses_left<=0").bind(1, now()).run();
    if (!St(db_, "INSERT INTO invites(code_hash, expires, uses_left) VALUES(?1, ?2, ?3)").bind(1, sha_hex(c)).bind(2, now() + 86400LL * days).bind(3, (int64_t)uses).run())
        return "the database: " + error();
    log(who, "config.invite", std::to_string(uses) + " sign-up" + (uses == 1 ? "" : "s") + ", " + std::to_string(days) + " day" + (days == 1 ? "" : "s"));
    if (!tx.commit()) return "the database: " + error();
    code = c;
    return "";
}

// --- cards

namespace {
// The card the store knows by that number (any case), as the store writes it; false: none
bool known_card(const ea::Store &st, const std::string &card, ea::Identity &id) {
    for (const ea::Identity &i : ea::players(st))
        if (same_card(i.card, card)) {
            id = i;
            return true;
        }
    return false;
}
}  // namespace

Accounts::Refusal Accounts::link_card(const Who &who, const std::string &card, const std::string &pin) {
    Refusal why;
    if (!writable(why)) return why;
    if (!who.user()) return "a card is linked to an account: act as its user (--as)";
    ea::Identity id;
    if (card.empty() || !known_card(store_, card, id)) return "no card " + card + " has played on this server";
    Tx tx(db_);
    if (!tx.begun) return "the database: " + error();
    {
        St q(db_, "SELECT user FROM cards WHERE card=?1");
        q.bind(1, id.card);
        if (q.row()) return q.num(0) == who.id ? "the card is linked to this account already" : "the card is another account's";
    }
    int64_t tries = 0;
    {
        St q(db_, "SELECT tries, locked FROM card_locks WHERE card=?1");
        q.bind(1, id.card);
        if (q.row()) {
            if (q.num(1)) return "the card is locked after " + std::to_string(LOCK_TRIES) + " wrong PINs: an admin unlocks it";
            tries = q.num(0);
        }
    }
    bool right = false;
    for (const std::string &p : ea::pins(store_, id)) right |= !pin.empty() && same(p, pin);
    if (!right) {
        tries++;
        const bool lock = tries >= LOCK_TRIES;
        St(db_, "INSERT OR REPLACE INTO card_locks(card, tries, locked) VALUES(?1, ?2, ?3)").bind(1, id.card).bind(2, tries).bind(3, lock ? now() : (int64_t)0).run();
        log(who, "card.link_refused", id.card + " (wrong PIN, " + std::to_string(tries) + " of " + std::to_string(LOCK_TRIES) + (lock ? ": locked)" : ")"));
        tx.commit();
        return lock ? "wrong PIN: the card is now locked for linking (an admin unlocks it)" : "wrong PIN (or the card has none kept)";
    }
    if (!St(db_, "DELETE FROM card_locks WHERE card=?1").bind(1, id.card).run() ||
        !St(db_, "INSERT INTO cards(card, user, linked) VALUES(?1, ?2, ?3)").bind(1, id.card).bind(2, (int64_t)who.id).bind(3, now()).run())
        return "the database: " + error();
    log(who, "card.link", id.card);
    return tx.commit() ? "" : "the database: " + error();
}

Accounts::Refusal Accounts::unlink_card(const Who &who, const std::string &card) {
    Refusal why;
    if (!writable(why)) return why;
    Tx tx(db_);
    St q(db_, "SELECT c.card, c.user, u.name FROM cards c LEFT JOIN users u ON u.id=c.user WHERE c.card=?1");
    q.bind(1, card);
    if (!q.row()) return who.admin() ? "the card " + card + " isn't linked" : NOT_ALLOWED;
    const std::string c = q.text(0), owner = q.text(2);
    if (!(who.user() && q.num(1) == who.id) && !who.admin()) return NOT_ALLOWED;
    if (!St(db_, "DELETE FROM cards WHERE card=?1").bind(1, c).run()) return "the database: " + error();
    log(who, "card.unlink", c + " (" + owner + "'s)");
    return tx.commit() ? "" : "the database: " + error();
}

Accounts::Refusal Accounts::unlock_card(const Who &who, const std::string &card) {
    Refusal why;
    if (!writable(why)) return why;
    if (!who.admin()) return NOT_ALLOWED;
    Tx tx(db_);
    St q(db_, "SELECT card FROM card_locks WHERE card=?1");
    q.bind(1, card);
    if (!q.row()) return "the card " + card + " isn't locked";
    const std::string c = q.text(0);
    if (!St(db_, "DELETE FROM card_locks WHERE card=?1").bind(1, c).run()) return "the database: " + error();
    log(who, "card.unlock", c);
    return tx.commit() ? "" : "the database: " + error();
}

Accounts::Refusal Accounts::reset_card_pin(const Who &who, const std::string &card, const std::string &pin, ea::PinReset &did) {
    did = ea::PinReset{};
    Refusal why;
    if (!writable(why)) return why;
    if (!who.admin()) return NOT_ALLOWED;
    ea::Identity id;
    if (!known_card(store_, card, id)) return "no card " + card + " has played on this server";
    if (pin.size() != 4 || pin.find_first_not_of("0123456789") != std::string::npos) return "a PIN is four digits";
    const bool written = ea::reset_pin(store_, id, pin, did);
    std::string places;
    for (const std::string &p : did.done) places += (places.empty() ? "" : "; ") + p;
    Tx tx(db_);
    log(who, "card.pin_reset", id.card + " (" + (places.empty() ? "no place" : places) + (written ? "" : "; some not written") + ")");
    tx.commit();
    return written ? "" : "some of the card's places weren't written";
}

Accounts::Refusal Accounts::cards(const Who &who, const std::string &user, std::vector<LinkedCard> &out) {
    out.clear();
    if (!db_) return "the database isn't open";
    User u;
    if (user.empty()) {
        if (!who.user()) return who.admin() ? "" : NOT_ALLOWED;   // (the command line holds no card: every one with a user named)
        u.id = who.id, u.name = who.name;
    } else if (!find_user(user, u)) {
        return "no user " + user;
    }
    if (!(who.user() && who.id == u.id) && !who.admin()) return "a card number is shown to its owner only";
    St q(db_, "SELECT card, linked FROM cards WHERE user=?1 ORDER BY linked, card");
    q.bind(1, (int64_t)u.id);
    while (q.row()) out.push_back({q.text(0), u.name, q.num(1)});
    return "";
}

Accounts::Refusal Accounts::stats(const Who &who, const std::string &user, std::vector<Stat> &out) {
    out.clear();
    if (!db_) return "the database isn't open";
    if (who.nobody) return NOT_ALLOWED;
    User u;
    if (!find_user(user, u)) return "no user " + user;
    const bool own = who.user() && who.id == u.id;
    if (u.hide_stats && !own && !who.admin()) return u.name + " keeps their stats to themselves";
    std::vector<std::string> mine;
    {
        St q(db_, "SELECT card FROM cards WHERE user=?1 ORDER BY linked, card");
        q.bind(1, (int64_t)u.id);
        while (q.row()) mine.push_back(q.text(0));
    }
    for (const ea::Identity &id : ea::players(store_)) {
        if (std::none_of(mine.begin(), mine.end(), [&](const std::string &c) { return same_card(c, id.card); })) continue;
        for (const ea::Game &g : ea::games()) {
            Stat s;
            if (!ea::profile(store_, g, id, s.profile)) continue;
            s.card = own || who.admin() ? id.card : "";
            s.game = *g.id ? g.id : g.model;
            s.scores = ea::scores(store_, g, id);
            out.push_back(std::move(s));
        }
    }
    return "";
}

// --- arcades and machines

bool Accounts::find_arcade(long id, Arcade &a, long &owner) const {
    St q(db_, "SELECT a.id, a.owner, u.name, a.name, a.pref FROM arcades a LEFT JOIN users u ON u.id=a.owner WHERE a.id=?1");
    q.bind(1, (int64_t)id);
    if (!q.row()) return false;
    a.id = (long)q.num(0), owner = (long)q.num(1), a.owner = q.text(2), a.name = q.text(3), a.pref = (int)q.num(4);
    return true;
}

Accounts::Refusal Accounts::may_edit_arcade(const Who &who, long id, Arcade &a) const {
    long owner = 0;
    if (!who.op()) return NOT_ALLOWED;
    if (!find_arcade(id, a, owner)) return "no arcade " + std::to_string(id);
    if (!who.admin() && !(who.user() && owner == who.id)) return "the arcade " + std::to_string(id) + " is another operator's";
    return "";
}

Accounts::Refusal Accounts::add_arcade(const Who &who, const std::string &name, int pref, const std::string &owner, long &id) {
    id = 0;
    Refusal why;
    if (!writable(why)) return why;
    if (!who.op()) return "an arcade is an operator's: " + std::string(NOT_ALLOWED);
    if (!(why = label_wrong(name)).empty()) return why;
    if (!pref_ok(pref)) return "a prefecture is a number from 1 to 47";
    int64_t owner_id = who.user() ? who.id : 0;
    std::string owner_name = who.user() ? who.name : "";
    if (!owner.empty()) {
        User o;
        if (!who.admin() && !same_card(owner, who.name)) return "only an admin gives an arcade to another";
        if (!find_user(owner, o)) return "no user " + owner;
        if (o.role < Role::op) return o.name + " isn't an operator (an admin makes one: user role NAME operator)";
        owner_id = o.id, owner_name = o.name;
    }
    Tx tx(db_);
    St a(db_, "INSERT INTO arcades(owner, name, pref) VALUES(?1, ?2, ?3)");
    if (owner_id) a.bind(1, owner_id);
    a.bind(2, name).bind(3, (int64_t)pref);
    if (!a.run()) return "the database: " + error();
    id = (long)sqlite3_last_insert_rowid(db_);
    log(who, "arcade.add", std::to_string(id) + " " + name + " (prefecture " + std::to_string(pref) + ", " + (owner_name.empty() ? "no one" : owner_name) + "'s)");
    return tx.commit() ? "" : "the database: " + error();
}

Accounts::Refusal Accounts::rename_arcade(const Who &who, long id, const std::string &name) {
    Refusal why;
    if (!writable(why)) return why;
    Arcade a;
    if (!(why = may_edit_arcade(who, id, a)).empty() || !(why = label_wrong(name)).empty()) return why;
    Tx tx(db_);
    if (!St(db_, "UPDATE arcades SET name=?1 WHERE id=?2").bind(1, name).bind(2, (int64_t)id).run()) return "the database: " + error();
    log(who, "arcade.rename", std::to_string(id) + " " + a.name + " -> " + name);
    return tx.commit() ? "" : "the database: " + error();
}

Accounts::Refusal Accounts::set_arcade_pref(const Who &who, long id, int pref) {
    Refusal why;
    if (!writable(why)) return why;
    Arcade a;
    if (!(why = may_edit_arcade(who, id, a)).empty()) return why;
    if (!pref_ok(pref)) return "a prefecture is a number from 1 to 47";
    Tx tx(db_);
    if (!St(db_, "UPDATE arcades SET pref=?1 WHERE id=?2").bind(1, (int64_t)pref).bind(2, (int64_t)id).run()) return "the database: " + error();
    log(who, "arcade.pref", std::to_string(id) + " " + std::to_string(a.pref) + " -> " + std::to_string(pref));
    return tx.commit() ? "" : "the database: " + error();
}

Accounts::Refusal Accounts::remove_arcade(const Who &who, long id) {
    Refusal why;
    if (!writable(why)) return why;
    Arcade a;
    if (!(why = may_edit_arcade(who, id, a)).empty()) return why;
    {
        Tx tx(db_);
        if (!St(db_, "DELETE FROM machines WHERE arcade=?1").bind(1, (int64_t)id).run() ||
            !St(db_, "DELETE FROM arcades WHERE id=?1").bind(1, (int64_t)id).run())
            return "the database: " + error();
        log(who, "arcade.remove", std::to_string(id) + " " + a.name + " (its machines with it)");
        if (!tx.commit()) return "the database: " + error();
    }
    // its settings: emptied (an empty one reads as none)
    const auto held = store_.hold();
    for (const ea::Key &k : store_.keys(ea::tables::arcade))
        if (k.size() == 3 && k[0] == std::to_string(id) && !store_.get(ea::tables::arcade, k).empty()) store_.put(ea::tables::arcade, k, "");
    return "";
}

Accounts::Refusal Accounts::arcades(const Who &who, std::vector<Arcade> &out) {
    out.clear();
    if (!db_) return "the database isn't open";
    if (!who.op()) return NOT_ALLOWED;
    St q(db_, "SELECT a.id, u.name, a.name, a.pref FROM arcades a LEFT JOIN users u ON u.id=a.owner WHERE ?1 OR a.owner=?2 ORDER BY a.id");
    q.bind(1, (int64_t)who.admin()).bind(2, (int64_t)who.id);
    while (q.row()) out.push_back({(long)q.num(0), q.text(1), q.text(2), (int)q.num(3)});
    return "";
}

Accounts::Refusal Accounts::machine_arcade(const Who &who, const std::string &pcbid, Machine &m) const {
    if (!who.op()) return NOT_ALLOWED;
    St q(db_, "SELECT pcbid, arcade, name, enabled, last_seen FROM machines WHERE pcbid=?1");
    q.bind(1, upper(pcbid));
    if (!q.row()) return "no machine " + pcbid;
    m = {q.text(0), (long)q.num(1), q.text(2), q.num(3) != 0, q.num(4)};
    Arcade a;
    const Refusal why = may_edit_arcade(who, m.arcade, a);
    return why.empty() ? "" : "the machine " + m.pcbid + " is another operator's";
}

Accounts::Refusal Accounts::add_machine(const Who &who, long arcade, const std::string &name, std::string &pcbid) {
    pcbid.clear();
    Refusal why;
    if (!writable(why)) return why;
    Arcade a;
    if (!(why = may_edit_arcade(who, arcade, a)).empty() || !(why = label_wrong(name)).empty()) return why;
    const std::string id = ea::new_pcbid();
    if (id.empty()) return "the system gives no random numbers";
    Tx tx(db_);
    if (!St(db_, "INSERT INTO machines(pcbid, arcade, name, enabled) VALUES(?1, ?2, ?3, 1)").bind(1, id).bind(2, (int64_t)arcade).bind(3, name).run())
        return "the database: " + error();
    log(who, "machine.add", id + " " + name + " (arcade " + std::to_string(arcade) + ")");
    if (!tx.commit()) return "the database: " + error();
    pcbid = id;
    return "";
}

Accounts::Refusal Accounts::rename_machine(const Who &who, const std::string &pcbid, const std::string &name) {
    Refusal why;
    if (!writable(why)) return why;
    Machine m;
    if (!(why = machine_arcade(who, pcbid, m)).empty() || !(why = label_wrong(name)).empty()) return why;
    Tx tx(db_);
    if (!St(db_, "UPDATE machines SET name=?1 WHERE pcbid=?2").bind(1, name).bind(2, m.pcbid).run()) return "the database: " + error();
    log(who, "machine.rename", m.pcbid + " " + m.name + " -> " + name);
    return tx.commit() ? "" : "the database: " + error();
}

Accounts::Refusal Accounts::enable_machine(const Who &who, const std::string &pcbid, bool on) {
    Refusal why;
    if (!writable(why)) return why;
    Machine m;
    if (!(why = machine_arcade(who, pcbid, m)).empty()) return why;
    Tx tx(db_);
    if (!St(db_, "UPDATE machines SET enabled=?1 WHERE pcbid=?2").bind(1, (int64_t)on).bind(2, m.pcbid).run()) return "the database: " + error();
    log(who, on ? "machine.enable" : "machine.disable", m.pcbid + " " + m.name);
    return tx.commit() ? "" : "the database: " + error();
}

Accounts::Refusal Accounts::remove_machine(const Who &who, const std::string &pcbid) {
    Refusal why;
    if (!writable(why)) return why;
    Machine m;
    if (!(why = machine_arcade(who, pcbid, m)).empty()) return why;
    Tx tx(db_);
    if (!St(db_, "DELETE FROM machines WHERE pcbid=?1").bind(1, m.pcbid).run()) return "the database: " + error();
    log(who, "machine.remove", m.pcbid + " " + m.name + " (arcade " + std::to_string(m.arcade) + ")");
    return tx.commit() ? "" : "the database: " + error();
}

Accounts::Refusal Accounts::machines(const Who &who, long arcade, std::vector<Machine> &out) {
    out.clear();
    if (!db_) return "the database isn't open";
    if (!who.op()) return NOT_ALLOWED;
    if (arcade) {
        Arcade a;
        if (const Refusal why = may_edit_arcade(who, arcade, a); !why.empty()) return why;
    }
    St q(db_, "SELECT m.pcbid, m.arcade, m.name, m.enabled, m.last_seen FROM machines m JOIN arcades a ON a.id=m.arcade"
              " WHERE (?1 = 0 OR m.arcade = ?1) AND (?2 OR a.owner = ?3) ORDER BY m.arcade, m.name, m.pcbid");
    q.bind(1, (int64_t)arcade).bind(2, (int64_t)who.admin()).bind(3, (int64_t)who.id);
    while (q.row()) out.push_back({q.text(0), (long)q.num(1), q.text(2), q.num(3) != 0, q.num(4)});
    return "";
}

// --- settings

namespace {
// The checks hiraku-server's set makes (eamuse/admin.cpp): "" or why not
std::string setting_wrong(const std::string &game, const ea::Game *g, const ea::Setting *s, const std::string &name, const std::string &value) {
    if (!g && game != "server") return "no game " + game;
    if (!s) return game + " has no setting " + name;
    if (value.find_first_of("\r\n") != std::string::npos) return "a setting is one line";
    if (s->kind == ea::Setting::number && !value.empty()) {
        char *end = nullptr;
        const long n = std::strtol(value.c_str(), &end, 10);
        if (*end || n < s->min || n > s->max)
            return game + "'s " + name + " is a number from " + std::to_string(s->min) + " to " + std::to_string(s->max);
    }
    return "";
}
std::string trimmed(std::string v) {
    while (!v.empty() && std::isspace((unsigned char)v.back())) v.pop_back();
    return v;
}
}  // namespace

Accounts::Refusal Accounts::set_setting(const Who &who, long arcade, const std::string &game, const std::string &name, const std::string &value) {
    Refusal why;
    if (!writable(why)) return why;
    Arcade a;
    if (arcade ? !(why = may_edit_arcade(who, arcade, a)).empty() : !who.admin() && !(why = "the server's settings are an admin's").empty()) return why;
    if (arcade && game == "server") return "an arcade has its games' settings only";
    const ea::Game *g = game == "server" ? nullptr : ea::game_by_id(game);
    const ea::Setting *s = g || game == "server" ? ea::find_setting(g, name.c_str()) : nullptr;
    if (!(why = setting_wrong(game, g, s, name, value)).empty()) return why;
    const ea::Table &t = arcade ? ea::tables::arcade : *s->table;
    const ea::Key key = arcade ? ea::arcade_setting_key(*g, *s, std::to_string(arcade)) : ea::setting_key(g, *s);
    if (g && key.empty() && (arcade || s->table == &ea::tables::setting)) return game + " has no settings of its own";
    {
        const auto held = store_.hold();
        if (value.empty() ? !store_.get(t, key).empty() && !store_.put(t, key, "") : !store_.put(t, key, value + "\n"))
            return store_.label(t, key) + " not written";
    }
    Tx tx(db_);
    log(who, "setting.set", (arcade ? "arcade " + std::to_string(arcade) : std::string("server")) + " " + game + " " + name + " = " +
                                (value.empty() ? "(none)" : value));
    tx.commit();
    return "";
}

Accounts::Refusal Accounts::settings(const Who &who, long arcade, const std::string &game, std::vector<SettingLine> &out) {
    out.clear();
    if (!db_) return "the database isn't open";
    Arcade a;
    if (arcade) {
        if (const Refusal why = may_edit_arcade(who, arcade, a); !why.empty()) return why;
    } else if (!who.op()) {
        return NOT_ALLOWED;
    }
    const ea::Game *only = game.empty() ? nullptr : ea::game_by_id(game);
    if (!game.empty() && !only) return "no game " + game;
    for (const ea::Game &g : ea::games()) {
        if (!*g.model || std::strchr(g.model, '?') || (only && &g != only)) continue;
        for (const ea::Setting &s : g.settings) {
            SettingLine l{*g.id ? g.id : g.model, s.name, "", "", s.by_default};
            l.server = trimmed(store_.get(*s.table, ea::setting_key(&g, s)));
            if (arcade) l.arcade = trimmed(store_.get(ea::tables::arcade, ea::arcade_setting_key(g, s, std::to_string(arcade))));
            out.push_back(l);
        }
    }
    return "";
}

Accounts::Refusal Accounts::audit(const Who &who, size_t last, std::vector<AuditLine> &out) {
    out.clear();
    if (!db_) return "the database isn't open";
    if (!who.admin()) return NOT_ALLOWED;
    St q(db_, "SELECT id, at, user, action, detail FROM (SELECT * FROM audit ORDER BY id DESC LIMIT ?1) ORDER BY id");
    q.bind(1, (int64_t)(last ? last : 1000000000));
    while (q.row()) out.push_back({(long)q.num(0), q.num(1), q.text(2), q.text(3), q.text(4)});
    return "";
}

// --- serving

size_t Accounts::machine_count() {
    if (!db_) return 0;
    St q(db_, "SELECT COUNT(*) FROM machines");
    return q.row() ? (size_t)q.num(0) : 0;
}

int Accounts::admits(const std::string &srcid) {
    if (!db_ || machine_count() == 0) return -1;
    const std::string id = ea::pcbid_of(srcid);
    if (id.empty()) return 0;
    St q(db_, "SELECT enabled FROM machines WHERE pcbid=?1");
    q.bind(1, id);
    if (!q.row() || !q.num(0)) return 0;
    const int64_t t = now();
    auto it = seen_.find(id);
    if (it == seen_.end() || t - it->second >= 60 || t < it->second) {
        if (seen_.size() > 4096) seen_.clear();
        seen_[id] = t;
        if (version_ <= VERSION) St(db_, "UPDATE machines SET last_seen=?1 WHERE pcbid=?2").bind(1, t).bind(2, id).run();
    }
    return 1;
}

ea::Cabinet Accounts::cabinet(const std::string &srcid) {
    ea::Cabinet c;
    const std::string id = ea::pcbid_of(srcid);
    if (!db_ || id.empty()) return c;
    St q(db_, "SELECT a.id, a.name, a.pref FROM machines m JOIN arcades a ON a.id=m.arcade WHERE m.pcbid=?1 AND m.enabled=1");
    q.bind(1, id);
    if (q.row()) c = {std::to_string(q.num(0)), q.text(1), (int)q.num(2)};
    return c;
}

}  // namespace hk::srv
