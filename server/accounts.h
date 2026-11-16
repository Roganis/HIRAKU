// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-server's people and places: accounts and their roles, cards linked to accounts, arcades and
// their machines, each arcade's own event settings, and an audit log, in tables of their own in the
// server's SQLite database beside the things it keeps (runtime/eamuse/store_sqlite.h). A library:
// every operation takes who acts and refuses what that one's role may not do, with the reason; the
// command line (server/main.cpp) and later the server's web pages drive it. hiraku-server's only.
//
// Roles stack: an admin is an operator, an operator a player. The first admin is made with the
// accounts' tables (name admin, a random password shown once, to be changed). Sign-up is open to
// anyone unless an admin says by invitation or closed. A player links a card the server knows with
// a PIN any game keeps for it (eamuse/readers.h pins(); five wrong ones lock the card for linking
// until an admin unlocks it). An operator owns arcades (a shop's name and prefecture) and their
// machines (a PCBID each); a machine of an arcade is answered with its arcade's name, prefecture and
// event settings (Server::cabinet). Every change is in the audit log: who, when, what; never a
// password, PIN or code. A password is kept as PBKDF2-HMAC-SHA256 (pbkdf2-sha256$ROUNDS$SALT$HASH),
// a reset or invitation code as its SHA-256.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "eamuse/readers.h"
#include "eamuse/server.h"
#include "eamuse/store.h"

struct sqlite3;

namespace hk::srv {

namespace ea = hk::ea;

enum class Role { player, op, admin };
const char *role_name(Role);                      // "player", "operator", "admin"
bool role_named(const std::string &, Role &);     // ... and back; false: none of them

// Who acts: a user, the server's own command line (an admin that is no account), or nobody
// signed in (sign-up, a reset code)
struct Who {
    long id = 0;   // the user's; 0: the command line or nobody
    Role role = Role::player;
    std::string name;
    bool nobody = false;
    static Who command_line() { return {0, Role::admin, "(command line)", false}; }
    static Who anyone() { return {0, Role::player, "(anyone)", true}; }
    bool admin() const { return !nobody && role == Role::admin; }
    bool op() const { return !nobody && role >= Role::op; }
    bool user() const { return !nobody && id > 0; }
};

struct User {
    long id = 0;
    std::string name;
    Role role = Role::player;
    bool disabled = false, hide_stats = false, must_change = false;
    int64_t created = 0;
};
struct LinkedCard {
    std::string card, user;
    int64_t linked = 0;
};
struct Arcade {
    long id = 0;
    std::string owner;   // "": no one's (its operator's account removed): admins'
    std::string name;
    int pref = 13;
};
struct Machine {
    std::string pcbid;
    long arcade = 0;
    std::string name;
    bool enabled = true;
    int64_t last_seen = 0;   // 0: never
};
struct AuditLine {
    long id = 0;
    int64_t at = 0;
    std::string user, action, detail;
};
// A player's game, as others may see it (card "" for one who isn't the card's owner or an admin)
struct Stat {
    std::string card, game;
    ea::Profile profile;
    std::vector<ea::Score> scores;
};
// A game's setting for an arcade: its own value ("" none), the server's ("" none), the default
struct SettingLine {
    std::string game, name, arcade, server, by_default;
};

class Accounts {
public:
    static constexpr long VERSION = 1;          // the tables' (the meta table's row "accounts")
    static constexpr uint32_t ROUNDS = 200000;   // PBKDF2's for a new password
    static constexpr int LOCK_TRIES = 5;         // wrong PINs that lock a card for linking
    enum class SignUp { open, invite, closed };

    // The accounts of the database `file` (made if it has none), whose things are `store`'s (the same
    // file's SqliteStore). ok() false if it can't be opened (said)
    Accounts(const std::string &file, ea::Store &store);
    ~Accounts();
    Accounts(const Accounts &) = delete;
    Accounts &operator=(const Accounts &) = delete;
    bool ok() const { return db_ != nullptr; }
    // The first admin's password when this opening made it, else "": to be shown once, nowhere kept
    const std::string &first_password() const { return first_password_; }

    using Refusal = std::string;   // "": done; else why not, for a person

    // --as: a user that is there and not disabled
    Refusal as(const std::string &name, Who &);

    // Users. sign_up: anyone, as the sign-up mode says (an invitation's code when by invitation); an
    // admin in any mode, with any role
    Refusal sign_up(const Who &, const std::string &name, const std::string &password, const std::string &invite = "",
                    Role = Role::player);
    Refusal check_password(const std::string &name, const std::string &password, Who &, bool &must_change);
    // one's own with the old password; an admin's for another without (who must then change it)
    Refusal change_password(const Who &, const std::string &user, const std::string &old_password, const std::string &password);
    Refusal set_role(const Who &, const std::string &user, Role);
    Refusal set_disabled(const Who &, const std::string &user, bool);
    Refusal set_hide_stats(const Who &, const std::string &user, bool);   // one's own, or an admin
    Refusal remove_user(const Who &, const std::string &user);           // its cards unlinked, their data kept
    Refusal users(const Who &, std::vector<User> &);                       // an admin
    Refusal make_reset(const Who &, const std::string &user, std::string &code);   // an admin: a code for a day, once
    Refusal use_reset(const std::string &user, const std::string &code, const std::string &password);

    SignUp sign_up_mode() const;
    Refusal set_sign_up_mode(const Who &, SignUp);
    Refusal make_invite(const Who &, int uses, int days, std::string &code);

    // Cards
    Refusal link_card(const Who &, const std::string &card, const std::string &pin);
    Refusal unlink_card(const Who &, const std::string &card);   // its account's, or an admin
    Refusal unlock_card(const Who &, const std::string &card);   // an admin
    Refusal reset_card_pin(const Who &, const std::string &card, const std::string &pin, ea::PinReset &);   // an admin
    Refusal cards(const Who &, const std::string &user, std::vector<LinkedCard> &);   // user "": who's own
    Refusal stats(const Who &, const std::string &user, std::vector<Stat> &);

    // Arcades and machines: an operator's own, every one for an admin
    Refusal add_arcade(const Who &, const std::string &name, int pref, const std::string &owner, long &id);
    Refusal rename_arcade(const Who &, long id, const std::string &name);
    Refusal set_arcade_pref(const Who &, long id, int pref);
    Refusal remove_arcade(const Who &, long id);   // its machines and settings with it
    Refusal arcades(const Who &, std::vector<Arcade> &);
    Refusal add_machine(const Who &, long arcade, const std::string &name, std::string &pcbid);
    Refusal rename_machine(const Who &, const std::string &pcbid, const std::string &name);
    Refusal enable_machine(const Who &, const std::string &pcbid, bool);
    Refusal remove_machine(const Who &, const std::string &pcbid);
    Refusal machines(const Who &, long arcade, std::vector<Machine> &);   // arcade 0: every one who may see

    // A game's setting for an arcade (arcade 0: the server's, an admin's; game "server": the server's
    // own settings, arcade 0 only), checked against its declaration (games.h); value "": none
    Refusal set_setting(const Who &, long arcade, const std::string &game, const std::string &name, const std::string &value);
    Refusal settings(const Who &, long arcade, const std::string &game, std::vector<SettingLine> &);

    Refusal audit(const Who &, size_t last, std::vector<AuditLine> &);   // an admin: the last ones

    // Serving (server/main.cpp's serve). Whether a call from srcid is answered: -1 the database has no
    // machine (as before: cabinets.txt decides), 1 an enabled machine's (its last_seen noted, once a
    // minute at most), 0 not
    int admits(const std::string &srcid);
    // The machine's arcade, for Server::cabinet
    ea::Cabinet cabinet(const std::string &srcid);
    size_t machine_count();

private:
    struct Tx;
    bool find_user(const std::string &name, User &) const;
    bool find_arcade(long id, Arcade &, long &owner) const;
    Refusal may_edit_arcade(const Who &, long id, Arcade &) const;
    Refusal machine_arcade(const Who &, const std::string &pcbid, Machine &) const;
    void log(const Who &, const std::string &action, const std::string &detail);
    bool exec(const char *sql);
    std::string error() const;
    std::string config(const char *name) const;
    bool admins_left_without(long id) const;
    bool writable(Refusal &) const;

    std::string file_;
    ea::Store &store_;
    sqlite3 *db_ = nullptr;
    long version_ = 0;
    std::string first_password_;
    std::map<std::string, int64_t> seen_;   // a machine's last_seen as written (admits)
};

}  // namespace hk::srv
