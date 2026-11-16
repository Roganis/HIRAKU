// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-server: HIRAKU's e-AMUSE3 server on the network, as hiraku-eamuse (tools/eamuse/server.cpp)
// is, keeping what it must remember in a SQLite 3 database (runtime/eamuse/store_sqlite.h) instead
// of a folder: the same answers, the same things kept. Its data goes to and from a folder in the
// folder's own file names (import, export), so a data folder moves into a database and back.
//
//   hiraku-server serve --db FILE [--port 8080] [--bind 0.0.0.0] [--allow ADDRESSES] [--cabinets FILE]
//                 [--key-file FILE] [--clock "2003-06-01 12:00:00"] [--tz 9] [--quiet] [--no-relay] [--no-keys]
//                 [--api 127.0.0.1:8081 | --no-api] [--api-insecure-cookies] [--api-trust-proxy] [--web DIR | --no-web] [--names DIR]
//                 [--log-calls] [--connections 256] [--connections-per-address 64] [--sync full|normal] [--config FILE]
//   hiraku-server serve --db FILE [--cabinets FILE] --new-cabinet [NAME]  |  [--key-file FILE] --new-key
//   hiraku-server import FOLDER --db FILE     a data folder's files into the database (made if none)
//   hiraku-server export FOLDER --db FILE     the database as a data folder (a new or empty one)
//   hiraku-server put NAME --db FILE          stdin kept as the thing a data folder's file NAME would be
//   hiraku-server get NAME --db FILE          ... and that thing to stdout
//   hiraku-server backup FILE --db FILE       a copy of the database (safe while serve runs)
//   hiraku-server backup --dir DIR [--keep N] --db FILE   ... named by the time, the newest N kept
//   hiraku-server health [--api ADDRESS:PORT]  whether a serve answers (exit 0) or not (1)
//   hiraku-server --version
//   hiraku-server players | scores CARD [GAME] | settings [GAME] | set GAME NAME VALUE  --db FILE
//   hiraku-server user|card|arcade|machine|config|audit ... --db FILE [--as USER]   (usage() lists them)
//     serve's options are hiraku-eamuse's; its cabinets.txt and key.txt are beside the database
//     unless --cabinets and --key-file say otherwise. players, scores, settings and set are
//     hiraku-eamuse's --players, --scores, --settings and --set. The accounts, cards, arcades and
//     machines (server/accounts.h) act as the command line itself (an admin) unless --as names a
//     user; a password, PIN or code is read from stdin (a line each) or asked for, never an argument.
//     Once the database has a machine, serve answers its enabled machines only (not cabinets.txt),
//     each behind its own key (machine add, machine key; shell/keyed.h) and as itself only;
//     --no-keys: by the PCBID a call says it is, for a private network or a VPN. serve's JSON API
//     (server/api.h, docs/SERVER.md) listens on --api, a port of its own, behind a reverse proxy's TLS.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "accounts.h"
#include "api.h"
#include "eamuse/admin.h"
#include "eamuse/listener.h"
#include "eamuse/server.h"
#include "eamuse/store_sqlite.h"
#include "shell/sys.h"

#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#endif

namespace {

namespace ea = hk::ea;
constexpr const char *ME = "hiraku-server";

int usage(int status) {
    fprintf(status ? stderr : stdout,
            "usage: hiraku-server serve --db FILE [--port N] [--bind ADDR] [--allow ADDRESSES] [--cabinets FILE]\n"
            "                     [--key-file FILE] [--clock \"YYYY-MM-DD HH:MM:SS\"] [--tz H] [--quiet] [--no-relay] [--no-keys]\n"
            "                     [--api ADDR:PORT | --no-api] [--api-insecure-cookies] [--api-trust-proxy]\n"
            "                     [--web DIR | --no-web] [--names DIR] [--log-calls] [--connections N] [--connections-per-address N]\n"
            "                     [--sync full|normal] [--config FILE]\n"
            "       hiraku-server serve --db FILE [--cabinets FILE] --new-cabinet [NAME]\n"
            "       hiraku-server serve --db FILE [--key-file FILE] --new-key\n"
            "       hiraku-server import FOLDER --db FILE   a data folder's files into the database (made if none)\n"
            "       hiraku-server export FOLDER --db FILE   the database as a data folder (a new or empty one)\n"
            "       hiraku-server put NAME --db FILE        stdin kept as the data folder's file NAME would be\n"
            "       hiraku-server get NAME --db FILE        that thing to stdout\n"
            "       hiraku-server backup FILE --db FILE     a copy of the database, safe while serve runs\n"
            "       hiraku-server backup --dir DIR [--keep N] --db FILE   DIR/hiraku-server-YYYYMMDD-HHMMSS.db (UTC),\n"
            "                                               checked, then its own older ones beyond the newest N (14) removed\n"
            "       hiraku-server health [--api ADDR:PORT]  whether serve's API and database answer: exit 0, else 1\n"
            "       hiraku-server --version                 the build\n"
            "       hiraku-server players --db FILE         every card, its ref id, each game's player on it and its name\n"
            "       hiraku-server scores CARD [GAME] --db FILE   the card's scores: game, song, chart, score, clear, plays\n"
            "       hiraku-server settings [GAME] --db FILE      the owner's settings: game, name, value, default, range, what it does\n"
            "       hiraku-server set GAME NAME VALUE --db FILE  change one (GAME \"server\": the server's own); VALUE \"\": the default\n"
            "  serve's options are hiraku-eamuse's; cabinets.txt and key.txt are beside the database by default.\n"
            "  Once there are machines, each is answered behind its own key only (machine add, machine key);\n"
            "  --no-keys: by the PCBID a call says, for a private network or a VPN\n"
            "  --api: the JSON API's address (127.0.0.1:8081; docs/SERVER.md), behind a reverse proxy's TLS;\n"
            "  --api-insecure-cookies: its log-in cookie without Secure (plain HTTP, tests); --api-trust-proxy:\n"
            "  the caller's address is X-Forwarded-For's last hop\n"
            "  --web: the web pages' folder, served at / on the API's port (default: web beside the program,\n"
            "  ../share/hiraku-server/web, or a build tree's checkout's web/); --no-web: none\n"
            "  --names: the song names' packs (DIR/GAME.json, made with tools/eamuse/names.py from your own games;\n"
            "  default: names beside the database), read again when one changes; none: songs by number\n"
            "  serve's log: a line a game call and a line an API request, no body, card, PIN or password;\n"
            "  --log-calls: the calls' bodies too (PINs and passwords as ***), for finding a fault; --quiet: neither\n"
            "  --connections N: the games' connections served at once (256; 16 to 4096), more wait their turn;\n"
            "  --connections-per-address N: at most N of them from one address (64; 0: no limit; this computer's: none)\n"
            "  --sync full|normal: a change waits for the disk (full), or only at the WAL's checkpoints (normal: a power\n"
            "  cut may lose the last moments' calls, never the database; docs/SERVER.md)\n"
            "  --config FILE: lines \"name = value\" (# comments), the names serve's long options without the\n"
            "  dashes (port = 8080, no-relay = true, db = FILE); an option on the command line wins\n"
            "accounts, cards, arcades, machines (--db FILE; --as USER acts as that user, else the command line, an admin;\n"
            "a password, PIN or code is read from stdin, a line each, or asked for):\n"
            "  user add NAME [--role player|operator|admin]   a new account (password)\n"
            "  user signup NAME [--invite]       sign up as anyone does (--invite: the invitation's code, then the password)\n"
            "  user login NAME                   check a password: name, role, must change\n"
            "  user password NAME                a new password (one's own: the old one first)\n"
            "  user role NAME ROLE | disable NAME | enable NAME | remove NAME | list\n"
            "  user hide NAME yes|no             hide one's stats from other players\n"
            "  user reset NAME                   a one-time password-reset code (a day)\n"
            "  user reset-use NAME               the code, then a new password\n"
            "  user stats NAME                   game, card, player, plays, charts scored, charts cleared\n"
            "  card link CARD                    the PIN of any game the card has a player in\n"
            "  card unlink CARD | unlock CARD | list [USER]\n"
            "  card pin CARD                     a new PIN (four digits) wherever the card's PIN is kept\n"
            "  arcade add NAME PREF [--owner USER] | rename ID NAME | pref ID PREF | remove ID | list\n"
            "  arcade settings ID|server [GAME]  game, name, the arcade's value, the server's, the default\n"
            "  arcade set ID|server GAME NAME VALUE   an arcade's own value (\"\": none), or the server's\n"
            "  machine add ARCADE NAME           a new machine: its key (hk2.PCBID....), shown this once, for the\n"
            "                                    cabinet's -eakey (the launcher's Server key); its PCBID is in it\n"
            "  machine key PCBID                 a new key for the machine (shown this once); the old one stops\n"
            "  machine rename PCBID NAME | enable PCBID | disable PCBID | remove PCBID | list [ARCADE]\n"
            "  config show | signup open|invite|closed | invite [USES [DAYS]]\n"
            "  audit [N]                         the last N actions (all): when, who, what, detail\n");
    return status;
}

// The folder beside the database's file
std::string beside(const std::string &db) {
    const std::filesystem::path p = std::filesystem::path(db).parent_path();
    return p.empty() ? "." : p.string();
}

namespace srv = hk::srv;

// A line of stdin: a password, PIN or code. Asked for (not shown as typed) when stdin is a terminal
bool secret(const char *prompt, std::string &out) {
    out.clear();
#ifndef _WIN32
    const bool tty = isatty(0);
    termios was{};
    if (tty) {
        fprintf(stderr, "%s: ", prompt);
        tcgetattr(0, &was);
        termios quiet = was;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(0, TCSANOW, &quiet);
    }
#endif
    int c;
    bool any = false;
    while ((c = fgetc(stdin)) != EOF && c != '\n') out += (char)c, any = true;
    any = any || c == '\n';
#ifndef _WIN32
    if (tty) {
        tcsetattr(0, TCSANOW, &was);
        fputc('\n', stderr);
    }
#endif
    if (!out.empty() && out.back() == '\r') out.pop_back();
    return any;
}

std::string date(int64_t t) {
    if (!t) return "-";
    const time_t tt = (time_t)t;
    struct tm tm {};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char s[32];
    strftime(s, sizeof s, "%Y-%m-%d %H:%M:%S", &tm);
    return s;
}

void line(const std::vector<std::string> &fields) {
    std::string l;
    for (size_t i = 0; i < fields.size(); i++) l += (i ? "\t" : "") + fields[i];
    l += "\n";
    fwrite(l.data(), 1, l.size(), stdout);
}

struct AccountOptions {
    std::string as, owner, role;
    bool invite = false;
};

// The accounts' commands (usage()): the exit status
int accounts_command(srv::Accounts &acc, const std::vector<std::string> &w, const AccountOptions &o) {
    using R = srv::Accounts::Refusal;
    auto word = [&](size_t i) { return i < w.size() ? w[i] : std::string(); };
    const std::string group = word(0), verb = word(1);
    const size_t n = w.size() < 2 ? 0 : w.size() - 2;   // the verb's own words
    srv::Who who = srv::Who::command_line();
    if (!o.as.empty())
        if (R why = acc.as(o.as, who); !why.empty()) { fprintf(stderr, "%s: --as: %s\n", ME, why.c_str()); return 1; }
    auto refused = [&](const R &why) {
        if (why.empty()) return 0;
        fprintf(stderr, "%s: %s %s: %s\n", ME, group.c_str(), verb.c_str(), why.c_str());
        return 1;
    };
    auto need = [&](const char *prompt, std::string &v) {
        if (secret(prompt, v)) return true;
        fprintf(stderr, "%s: %s %s: no %s on stdin\n", ME, group.c_str(), verb.c_str(), prompt);
        return false;
    };
    auto number = [](const std::string &s, long &v) {
        char *end = nullptr;
        v = std::strtol(s.c_str(), &end, 10);
        return !s.empty() && !*end;
    };
    auto arcade_of = [&](const std::string &s, long &id) {   // "server": 0
        if (s == "server") return id = 0, true;
        return number(s, id) && id > 0;
    };
    auto shape = [&](bool ok) {
        if (!ok) fprintf(stderr, "%s: %s %s: see %s --help\n", ME, group.c_str(), verb.c_str(), ME);
        return ok;
    };
    std::string pass, pass2;
    if (group == "user") {
        if (verb == "add" || verb == "signup") {
            if (!shape(n == 1)) return 2;
            srv::Role role = srv::Role::player;
            if (!o.role.empty() && !srv::role_named(o.role, role)) { fprintf(stderr, "%s: --role: player, operator or admin\n", ME); return 2; }
            std::string code;
            if (verb == "signup" && o.invite && !need("invitation code", code)) return 1;
            if (!need("password", pass)) return 1;
            return refused(acc.sign_up(verb == "signup" ? srv::Who::anyone() : who, word(2), pass, code, role));
        }
        if (verb == "login") {
            if (!shape(n == 1) || !need("password", pass)) return n == 1 ? 1 : 2;
            srv::Who u;
            bool change = false;
            if (const int s = refused(acc.check_password(word(2), pass, u, change))) return s;
            line({u.name, srv::role_name(u.role), change ? "must change" : "-"});
            return 0;
        }
        if (verb == "password") {
            if (!shape(n == 1)) return 2;
            bool own = who.user() && who.name.size() == word(2).size();   // (names are one whatever their case)
            for (size_t i = 0; own && i < who.name.size(); i++) own = std::tolower((unsigned char)who.name[i]) == std::tolower((unsigned char)word(2)[i]);
            if (own && !need("old password", pass2)) return 1;
            if (!need("new password", pass)) return 1;
            return refused(acc.change_password(who, word(2), pass2, pass));
        }
        if (verb == "role") {
            srv::Role role;
            if (!shape(n == 2)) return 2;
            if (!srv::role_named(word(3), role)) { fprintf(stderr, "%s: user role: player, operator or admin\n", ME); return 2; }
            return refused(acc.set_role(who, word(2), role));
        }
        if (verb == "disable" || verb == "enable") return shape(n == 1) ? refused(acc.set_disabled(who, word(2), verb == "disable")) : 2;
        if (verb == "remove") return shape(n == 1) ? refused(acc.remove_user(who, word(2))) : 2;
        if (verb == "hide") {
            if (!shape(n == 2 && (word(3) == "yes" || word(3) == "no"))) return 2;
            return refused(acc.set_hide_stats(who, word(2), word(3) == "yes"));
        }
        if (verb == "list") {
            std::vector<srv::User> users;
            if (!shape(n == 0)) return 2;
            if (const int s = refused(acc.users(who, users))) return s;
            for (const srv::User &u : users)
                line({u.name, srv::role_name(u.role), u.disabled ? "disabled" : "enabled", u.hide_stats ? "stats hidden" : "stats shown",
                      u.must_change ? "must change" : "-", date(u.created)});
            return 0;
        }
        if (verb == "reset") {
            std::string code;
            if (!shape(n == 1)) return 2;
            if (const int s = refused(acc.make_reset(who, word(2), code))) return s;
            line({code});   // (shown this once: only its hash is kept)
            fprintf(stderr, "%s: the code works once, for a day: %s user reset-use %s\n", ME, ME, word(2).c_str());
            return 0;
        }
        if (verb == "reset-use") {
            std::string code;
            if (!shape(n == 1) || !need("reset code", code) || !need("new password", pass)) return n == 1 ? 1 : 2;
            return refused(acc.use_reset(word(2), code, pass));
        }
        if (verb == "stats") {
            std::vector<srv::Stat> stats;
            if (!shape(n == 1)) return 2;
            if (const int s = refused(acc.stats(who, word(2), stats))) return s;
            for (const srv::Stat &st : stats) {
                long cleared = 0;
                for (const ea::Score &sc : st.scores) cleared += sc.clear > 0;
                line({st.game, st.card.empty() ? "-" : st.card, st.profile.name, st.profile.plays < 0 ? "-" : std::to_string(st.profile.plays),
                      std::to_string(st.scores.size()), std::to_string(cleared)});
            }
            return 0;
        }
    } else if (group == "card") {
        if (verb == "link") {
            if (!shape(n == 1) || !need("PIN", pass)) return n == 1 ? 1 : 2;
            return refused(acc.link_card(who, word(2), pass));
        }
        if (verb == "unlink") return shape(n == 1) ? refused(acc.unlink_card(who, word(2))) : 2;
        if (verb == "unlock") return shape(n == 1) ? refused(acc.unlock_card(who, word(2))) : 2;
        if (verb == "pin") {
            if (!shape(n == 1) || !need("new PIN", pass)) return n == 1 ? 1 : 2;
            hk::ea::PinReset did;
            const int s = refused(acc.reset_card_pin(who, word(2), pass, did));
            for (const std::string &p : did.done) line({"changed", p});
            for (const std::string &p : did.not_done) line({"not changed", p});
            return s;
        }
        if (verb == "list") {
            std::vector<srv::LinkedCard> cards;
            if (!shape(n <= 1)) return 2;
            if (const int s = refused(acc.cards(who, word(2), cards))) return s;
            for (const srv::LinkedCard &c : cards) line({c.card, c.user, date(c.linked)});
            return 0;
        }
    } else if (group == "arcade") {
        long id = 0, pref = 0;
        if (verb == "add") {
            if (!shape(n == 2 && number(word(3), pref))) return 2;
            if (const int s = refused(acc.add_arcade(who, word(2), (int)pref, o.owner, id))) return s;
            line({std::to_string(id)});
            return 0;
        }
        if (verb == "rename") return shape(n == 2 && number(word(2), id)) ? refused(acc.rename_arcade(who, id, word(3))) : 2;
        if (verb == "pref") return shape(n == 2 && number(word(2), id) && number(word(3), pref)) ? refused(acc.set_arcade_pref(who, id, (int)pref)) : 2;
        if (verb == "remove") return shape(n == 1 && number(word(2), id)) ? refused(acc.remove_arcade(who, id)) : 2;
        if (verb == "list") {
            std::vector<srv::Arcade> list;
            if (!shape(n == 0)) return 2;
            if (const int s = refused(acc.arcades(who, list))) return s;
            for (const srv::Arcade &a : list) line({std::to_string(a.id), a.name, std::to_string(a.pref), a.owner.empty() ? "-" : a.owner});
            return 0;
        }
        if (verb == "set") {
            if (!shape(n == 4 && arcade_of(word(2), id))) return 2;
            if (const int s = refused(acc.set_setting(who, id, word(3), word(4), word(5)))) return s;
            line({word(2), word(3), word(4), word(5).empty() ? "-" : word(5)});
            return 0;
        }
        if (verb == "settings") {
            std::vector<srv::SettingLine> list;
            if (!shape((n == 1 || n == 2) && arcade_of(word(2), id))) return 2;
            if (const int s = refused(acc.settings(who, id, word(3), list))) return s;
            for (const srv::SettingLine &l : list)
                line({l.game, l.name, id ? (l.arcade.empty() ? "-" : l.arcade) : "-", l.server.empty() ? "-" : l.server, l.by_default});
            return 0;
        }
    } else if (group == "machine") {
        long arcade = 0;
        if (verb == "add" || verb == "key") {
            std::string pcbid, key;
            if (verb == "add" ? !shape(n == 2 && number(word(2), arcade)) : !shape(n == 1)) return 2;
            if (const int s = refused(verb == "add" ? acc.add_machine(who, arcade, word(3), pcbid, &key) : acc.new_machine_key(who, word(2), key))) return s;
            line({key});   // (shown this once: only what the connection's keys are made from is kept)
            fflush(stdout);
            fprintf(stderr, "%s: the machine's key, shown this once: give its cabinet that (-eakey; the launcher: Settings, "
                            "e-AMUSEMENT, Server key); it is the cabinet's PCBID too%s\n",
                    ME, verb == "key" ? ". Its old key is no longer taken" : "");
            return 0;
        }
        if (verb == "rename") return shape(n == 2) ? refused(acc.rename_machine(who, word(2), word(3))) : 2;
        if (verb == "enable" || verb == "disable") return shape(n == 1) ? refused(acc.enable_machine(who, word(2), verb == "enable")) : 2;
        if (verb == "remove") return shape(n == 1) ? refused(acc.remove_machine(who, word(2))) : 2;
        if (verb == "list") {
            std::vector<srv::Machine> list;
            if (!shape(n == 0 || (n == 1 && number(word(2), arcade)))) return 2;
            if (const int s = refused(acc.machines(who, arcade, list))) return s;
            for (const srv::Machine &m : list)
                line({m.pcbid, std::to_string(m.arcade), m.name, m.enabled ? "enabled" : "disabled", m.keyed ? "key" : "no key", date(m.last_seen)});
            return 0;
        }
    } else if (group == "config") {
        if (verb == "show") {
            if (!shape(n == 0)) return 2;
            const auto mode = acc.sign_up_mode();
            line({"signup", mode == srv::Accounts::SignUp::closed ? "closed" : mode == srv::Accounts::SignUp::invite ? "invite" : "open"});
            return 0;
        }
        if (verb == "signup") {
            const std::string m = word(2);
            if (!shape(n == 1 && (m == "open" || m == "invite" || m == "closed"))) return 2;
            return refused(acc.set_sign_up_mode(who, m == "closed" ? srv::Accounts::SignUp::closed
                                                     : m == "invite" ? srv::Accounts::SignUp::invite
                                                                     : srv::Accounts::SignUp::open));
        }
        if (verb == "invite") {
            long uses = 1, days = 7;
            std::string code;
            if (!shape(n <= 2 && (n < 1 || number(word(2), uses)) && (n < 2 || number(word(3), days)))) return 2;
            if (const int s = refused(acc.make_invite(who, (int)uses, (int)days, code))) return s;
            line({code});   // (shown this once: only its hash is kept)
            return 0;
        }
    } else if (group == "audit") {
        long last = 0;
        if (w.size() > 2 || (w.size() == 2 && (!number(word(1), last) || last < 1))) {
            fprintf(stderr, "%s: audit [N]\n", ME);
            return 2;
        }
        std::vector<srv::AuditLine> list;
        if (R why = acc.audit(who, (size_t)last, list); !why.empty()) { fprintf(stderr, "%s: audit: %s\n", ME, why.c_str()); return 1; }
        for (const srv::AuditLine &a : list) line({date(a.at), a.user, a.action, a.detail});
        return 0;
    }
    fprintf(stderr, "%s: no command %s %s (%s --help)\n", ME, group.c_str(), verb.c_str(), ME);
    return 2;
}

// The first admin's password, the once it is made
void first_admin(const srv::Accounts &acc) {
    if (acc.first_password().empty()) return;
    printf("%s: the first admin is \"admin\", password %s (shown this once; change it: %s user password admin --as admin)\n", ME,
           acc.first_password().c_str(), ME);
    fflush(stdout);
}

void report(const ea::Copied &c, const char *from, const char *to) {
    for (const std::string &n : c.not_placed) fprintf(stderr, "%s: %s: no table's (left out)\n", ME, n.c_str());
    for (const std::string &n : c.not_put) fprintf(stderr, "%s: %s: not written\n", ME, n.c_str());
    printf("%s: %zu things from %s to %s%s\n", ME, c.things, from, to, c.not_put.empty() ? "" : " (some not written)");
}

// --- serve's config file (--config): "name = value" lines, the names serve's long options

// The options a config file may name: those taking a value, and the switches (true or false)
const std::set<std::string> VALUED{"db", "port", "bind", "allow", "cabinets", "key-file", "clock", "tz", "api", "web", "names",
                                   "connections", "connections-per-address", "sync"};
const std::set<std::string> SWITCHES{"quiet", "no-relay", "no-keys", "no-api", "api-insecure-cookies", "api-trust-proxy", "no-web", "log-calls"};

bool whole(const std::string &s, long lo, long hi, long &v) {
    char *end = nullptr;
    v = std::strtol(s.c_str(), &end, 10);
    return !s.empty() && !*end && v >= lo && v <= hi;
}

// A value of serve's options: "" fits, else why not
std::string value_wrong(const std::string &name, const std::string &v) {
    long n = 0;
    if (name == "port") return whole(v, 1, 65535, n) ? "" : "a port, 1 to 65535";
    if (name == "tz") return whole(v, -12, 14, n) ? "" : "hours from UTC, -12 to 14";
    if (name == "connections") return whole(v, 16, 4096, n) ? "" : "how many at once, 16 to 4096";
    if (name == "connections-per-address") return whole(v, 0, 4096, n) ? "" : "how many at once from one address, 0 (no limit) to 4096";
    if (name == "sync") return v == "full" || v == "normal" ? "" : "full or normal";
    if (name == "bind") {
        int a, b, c, d;
        char extra;
        return sscanf(v.c_str(), "%d.%d.%d.%d%c", &a, &b, &c, &d, &extra) == 4 && a >= 0 && a < 256 && b >= 0 && b < 256 && c >= 0 && c < 256 && d >= 0 && d < 256
                   ? "" : "an IPv4 address (0.0.0.0: every one)";
    }
    if (name == "api") {
        const size_t colon = v.rfind(':');
        return colon != std::string::npos && colon > 0 && whole(v.substr(colon + 1), 1, 65535, n) ? "" : "ADDRESS:PORT (127.0.0.1:8081)";
    }
    if (name == "clock") {
        int y, mo, d;
        return sscanf(v.c_str(), "%d-%d-%d", &y, &mo, &d) == 3 ? "" : "\"YYYY-MM-DD[ HH:MM:SS]\"";
    }
    if (name == "allow") {
        ea::AddressList list;
        std::string error;
        return list.add(v, &error) ? "" : error;
    }
    return v.empty() ? "a value" : "";
}

// The options of the config file named on the command line (--config), as command-line words, those
// the command line names itself left out (it wins). -1: read (or none named); else the exit status (said)
int read_config(int argc, char **argv, std::vector<std::string> &out) {
    std::string file;
    std::set<std::string> given;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--config") {
            if (i + 1 >= argc) { fprintf(stderr, "%s: --config needs a file\n", ME); return 2; }
            file = argv[++i];
        } else if (a.rfind("--", 0) == 0) {
            given.insert(a.substr(2));
        }
    }
    if (file.empty()) return -1;
    std::ifstream f(file);
    if (!f) { fprintf(stderr, "%s: --config: cannot read %s\n", ME, file.c_str()); return 1; }
    std::string text;
    for (int n = 1; std::getline(f, text); n++) {
        auto trim = [](std::string s) {
            const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        if (const size_t hash = text.find('#'); hash != std::string::npos) text.erase(hash);
        text = trim(text);
        if (text.empty()) continue;
        const size_t eq = text.find('=');
        const std::string name = trim(text.substr(0, eq));
        std::string value = eq == std::string::npos ? "" : trim(text.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        auto wrong = [&](const std::string &why) {
            fprintf(stderr, "%s: %s, line %d: %s\n", ME, file.c_str(), n, why.c_str());
            return 1;
        };
        if (eq == std::string::npos || name.empty()) return wrong("not \"name = value\"");
        if (!VALUED.count(name) && !SWITCHES.count(name)) return wrong("no setting \"" + name + "\" (serve's long options, without the dashes)");
        if (SWITCHES.count(name)) {
            const bool yes = value == "true" || value == "yes" || value == "1" || value == "on";
            if (!yes && value != "false" && value != "no" && value != "0" && value != "off") return wrong(name + ": true or false");
            if (yes && !given.count(name)) out.push_back("--" + name);
            continue;
        }
        if (const std::string why = value_wrong(name, value); !why.empty()) return wrong(name + " = " + value + ": " + why);
        if (given.count(name)) continue;
        out.push_back("--" + name);
        out.push_back(value);
    }
    return -1;
}

// --- serve's stop (SIGTERM, SIGINT): the first asks for it, a second ends the program at once
std::atomic<int> stop_signal{0};

extern "C" void on_stop_signal(int sig) {
    if (stop_signal.exchange(sig)) std::_Exit(128 + sig);
}

void catch_stop_signals() {
#ifdef _WIN32
    signal(SIGINT, on_stop_signal);   // (Ctrl-C; a second one ends it as Windows does)
    signal(SIGTERM, on_stop_signal);
#else
    struct sigaction sa {};
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#endif
}

// --- start-up checks: one line each, what and where

// The database's folder there and written to (its file, and SQLite's -wal and -shm beside it)
bool db_place_ok(const std::string &db) {
    const std::string dir = beside(db);
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        fprintf(stderr, "%s: the database's folder %s isn't there (--db)\n", ME, dir.c_str());
        return false;
    }
#ifndef _WIN32
    if (::access(dir.c_str(), W_OK | X_OK) != 0) {
        fprintf(stderr, "%s: the database's folder %s can't be written to (--db): the database and its -wal file are kept there\n", ME, dir.c_str());
        return false;
    }
    if (hk::sys::exists(db) && ::access(db.c_str(), R_OK | W_OK) != 0) {
        fprintf(stderr, "%s: the database %s can't be read and written (--db)\n", ME, db.c_str());
        return false;
    }
#endif
    return true;
}

// --- backup --dir: the database copied by the time it was, checked, the oldest of its kind removed

// A name backup --dir gives: hiraku-server-YYYYMMDD-HHMMSS.db
bool backup_name(const std::string &n) {
    static const std::string head = "hiraku-server-";
    if (n.size() != head.size() + 18 || n.compare(0, head.size(), head) != 0 || n.compare(n.size() - 3, 3, ".db") != 0) return false;
    for (size_t i = head.size(); i < head.size() + 15; i++)
        if (i == head.size() + 8 ? n[i] != '-' : !std::isdigit((unsigned char)n[i])) return false;
    return true;
}

// The file a sound SQLite database: PRAGMA integrity_check says "ok"
bool intact(const std::string &file, std::string &why) {
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(file.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        why = db ? sqlite3_errmsg(db) : "cannot open it";
        sqlite3_close(db);
        return false;
    }
    std::string said;
    char *error = nullptr;
    const int rc = sqlite3_exec(db, "PRAGMA integrity_check", [](void *to, int n, char **v, char **) {
        if (n > 0 && v[0] && static_cast<std::string *>(to)->empty()) *static_cast<std::string *>(to) = v[0];
        return 0;
    }, &said, &error);
    if (rc != SQLITE_OK) said = error ? error : "the check didn't run";
    sqlite3_free(error);
    sqlite3_close(db);
    if (rc == SQLITE_OK && said == "ok") return true;
    why = said;
    return false;
}

int backup_to_dir(ea::SqliteStore &st, const std::string &db, const std::string &dir, long keep) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)) { fprintf(stderr, "%s: backup: no folder %s, and it can't be made\n", ME, dir.c_str()); return 1; }
    const time_t t = time(nullptr);
    struct tm tm {};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char name[48];
    strftime(name, sizeof name, "hiraku-server-%Y%m%d-%H%M%S.db", &tm);
    const std::string to = (fs::path(dir) / name).string();
    std::string error;
    if (!st.backup(to, error)) { fprintf(stderr, "%s: backup: %s\n", ME, error.c_str()); return 1; }
    if (!intact(to, error)) {
        fs::remove(to, ec);
        fprintf(stderr, "%s: backup: the copy %s failed its check (%s): removed, the older backups kept\n", ME, to.c_str(), error.c_str());
        return 1;
    }
    std::vector<std::string> ours;   // (only files of the name it gives: nothing else in the folder is touched)
    for (const fs::directory_entry &e : fs::directory_iterator(dir, ec))
        if (backup_name(e.path().filename().string()) && e.is_regular_file(ec) && !e.is_symlink(ec)) ours.push_back(e.path().filename().string());
    std::sort(ours.rbegin(), ours.rend());   // newest first
    size_t removed = 0;
    for (size_t i = (size_t)keep; i < ours.size(); i++) removed += fs::remove(fs::path(dir) / ours[i], ec);
    printf("%s: %s copied to %s and checked; %zu older backup%s removed, %zu kept\n", ME, db.c_str(), to.c_str(), removed, removed == 1 ? "" : "s",
           std::min(ours.size(), (size_t)keep));
    return 0;
}

}  // namespace

int main(int argc0, char **argv0) {
    // serve's config file's options first (--config), then the command line's: the last one said wins
    std::vector<std::string> configured;
    if (const int status = read_config(argc0, argv0, configured); status >= 0) return status;
    std::vector<char *> args{argv0[0]};
    for (std::string &w : configured) args.push_back(w.data());
    for (int i = 1; i < argc0; i++) args.push_back(argv0[i]);
    const int argc = (int)args.size();
    char **const argv = args.data();
    std::string db, backup_dir, config;
    long keep = 14;
    bool new_cabinet = false, new_key = false, no_keys = false, keep_given = false, log_calls = false, serve_only = false;
    std::string sync = "full";
    std::string new_name;
    std::vector<std::string> words;   // the command and its own words
    AccountOptions ao;
    ea::Listener listener;
    listener.program = ME;
    listener.max_connections = 256, listener.per_address = 64, listener.wait_at_limit = true;   // (hiraku-eamuse: 64, more closed)
    // (a test's: this computer's own connections counted against --connections-per-address as any other's)
    if (const char *t = std::getenv("HK_TEST_PER_ADDRESS_ALL"); t && *t == '1') listener.own_exempt = false;
    srv::ApiOptions api;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--version") {
            printf("%s %s\n", ME, hk::sys::build_id());
            return 0;
        }
        if ((a == "--port" || a == "--tz") && i + 1 < argc)   // (the listener takes any number: serve's are checked here)
            if (const std::string why = value_wrong(a.substr(2), argv[i + 1]); !why.empty()) {
                fprintf(stderr, "%s: %s %s: %s\n", ME, a.c_str(), argv[i + 1], why.c_str());
                return 2;
            }
        if (a == "--config") {
            config = argv[++i];   // (read_config read it)
        } else if (a == "--dir" || a == "--keep") {
            if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); return 2; }
            if (a == "--dir") backup_dir = argv[++i];
            else if (!whole(argv[++i], 1, 100000, keep)) { fprintf(stderr, "%s: --keep: how many backups to keep, 1 or more\n", ME); return 2; }
            else keep_given = true;
        } else if (a == "--log-calls") {
            log_calls = true;
        } else if (a == "--connections" || a == "--connections-per-address" || a == "--sync") {
            if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); return 2; }
            const std::string v = argv[++i];
            if (const std::string why = value_wrong(a.substr(2), v); !why.empty()) {
                fprintf(stderr, "%s: %s %s: %s\n", ME, a.c_str(), v.c_str(), why.c_str());
                return 2;
            }
            long num = 0;
            if (a == "--sync") sync = v;
            else if (whole(v, 0, 4096, num) && a == "--connections") listener.max_connections = (int)num;
            else listener.per_address = (int)num;
            serve_only = true;
        } else if (const int taken = listener.option(argc, argv, i)) {
            if (taken == 2) return 2;
        } else if (const int api_taken = api.option(argc, argv, i)) {
            if (api_taken == 2) return 2;
        } else if (a == "--db") {
            if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); return 2; }
            db = argv[++i];
        } else if (a == "--new-cabinet") {   // (its name is optional: the next word unless that is an option)
            new_cabinet = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') new_name = argv[++i];
        } else if (a == "--as" || a == "--owner" || a == "--role") {
            if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); return 2; }
            (a == "--as" ? ao.as : a == "--owner" ? ao.owner : ao.role) = argv[++i];
        } else if (a == "--invite") ao.invite = true;
        else if (a == "--new-key") new_key = true;
        else if (a == "--no-keys") no_keys = true;
        else if (a == "--help" || a == "-h") return usage(0);
        else if (a.size() > 1 && a[0] == '-') { fprintf(stderr, "%s: no option %s\n", ME, a.c_str()); return usage(2); }
        else words.push_back(a);
    }
    if (words.empty()) return usage(2);
    const std::string cmd = words[0];
    const size_t n = words.size() - 1;
    auto word = [&](size_t i) { return i < words.size() ? words[i] : std::string(); };
    if (!config.empty() && cmd != "serve") { fprintf(stderr, "%s: --config is serve's\n", ME); return 2; }
    if (serve_only && cmd != "serve") { fprintf(stderr, "%s: --connections, --connections-per-address and --sync are serve's\n", ME); return 2; }
    if ((!backup_dir.empty() || keep_given) && cmd != "backup") { fprintf(stderr, "%s: --dir and --keep are backup's\n", ME); return 2; }
    if (keep_given && backup_dir.empty()) { fprintf(stderr, "%s: backup: --keep goes with --dir\n", ME); return 2; }
    if (cmd == "health") {
        if (n != 0) return usage(2);
        return srv::health(api.host, api.port);
    }
    const bool accounts = cmd == "user" || cmd == "card" || cmd == "arcade" || cmd == "machine" || cmd == "config" || cmd == "audit";
    if (accounts) {
        if (db.empty()) { fprintf(stderr, "%s: %s: which database? (--db FILE)\n", ME, cmd.c_str()); return 2; }
        if (!hk::sys::exists(db)) { fprintf(stderr, "%s: no database %s (--db)\n", ME, db.c_str()); return 1; }
        ea::SqliteStore st(db);
        if (!st.ok()) return 1;
        srv::Accounts acc(db, st);
        if (!acc.ok()) return 1;
        first_admin(acc);
        return accounts_command(acc, words, ao);
    }
    const bool fits = cmd == "serve" || cmd == "players" ? n == 0
                      : cmd == "backup" ? n == (backup_dir.empty() ? 1u : 0u)
                      : cmd == "import" || cmd == "export" || cmd == "put" || cmd == "get" ? n == 1
                      : cmd == "scores" ? n == 1 || n == 2
                      : cmd == "settings" ? n <= 1
                      : cmd == "set" ? n == 3 : false;
    if (!fits) return usage(2);
    if (db.empty()) { fprintf(stderr, "%s: %s: which database? (--db FILE)\n", ME, cmd.c_str()); return 2; }
    // only these make a database that isn't there
    if (cmd != "serve" && cmd != "import" && cmd != "put" && !hk::sys::exists(db)) {
        fprintf(stderr, "%s: no database %s (--db)\n", ME, db.c_str());
        return 1;
    }

    if (cmd == "serve") {
        const bool serving = !new_cabinet && !new_key;
        // what would stop it, said before anything else in one line each: the database's folder, the
        // web pages' folder, the cabinets' list and the key, the games' port
        if (serving && !db_place_ok(db)) return 1;
        if (serving && api.enabled && api.web_enabled && !api.web.empty()) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(std::filesystem::path(api.web) / "index.html", ec)) {
                fprintf(stderr, "%s: the web pages' folder %s has no index.html (--web DIR, or --no-web)\n", ME, api.web.c_str());
                return 1;
            }
        }
        if (const int status = listener.start(beside(db), new_cabinet, new_name, new_key); status >= 0) return status;
        if (const int status = listener.open(); status >= 0) return status;
        ea::set_sync(sync == "normal" ? ea::Sync::normal : ea::Sync::full);   // (every connection opened from here: the store's, the accounts', the API's)
        auto store = std::make_unique<ea::SqliteStore>(db);
        if (!store->ok()) return 1;
        ea::SqliteStore &kept = *store;
        // (the store lives as long as the server; both closed when serve stops, below)
        auto acc = std::make_unique<srv::Accounts>(db, *store);
        if (!acc->ok()) return 1;
        first_admin(*acc);   // (said at once: shown this once, even if the start goes no further)
        auto server = std::make_unique<ea::Server>(std::move(store));   // every game's, by a call's model (runtime/eamuse/server.cpp)
        // the machines answered and their arcades (server/accounts.h), and their keys: asked by the
        // calls (one at a time), the connections' threads and the relay's, so one at a time
        std::mutex one;
        srv::Accounts &a = *acc;
        listener.machines = [&a, &one](const std::string &srcid) { std::lock_guard<std::mutex> l(one); return a.admits(srcid); };
        server->cabinet = [&a, &one](const std::string &srcid) { std::lock_guard<std::mutex> l(one); return a.cabinet(srcid); };
        listener.machine_name = [&a, &one](const std::string &pcbid) { std::lock_guard<std::mutex> l(one); return a.machine_name(pcbid); };
        if (!no_keys) {
            listener.machine_key = [&a, &one](const std::string &pcbid, hk::net::Key &key) {
                std::lock_guard<std::mutex> l(one);
                uint8_t k[32];
                if (!a.machine_key(pcbid, k)) return false;
                key = hk::net::Key::kept(pcbid, k);
                return true;
            };
            listener.machines_only = [&a, &one] { std::lock_guard<std::mutex> l(one); return a.machine_count() > 0; };
        }
        // the JSON API: its own threads and its own connection to the database (server/api.h)
        api.quiet = listener.quiet;
        std::unique_ptr<srv::Api> web;
        if (api.enabled) {
            web = std::make_unique<srv::Api>(db, kept, api);
            if (std::string error; !web->start(error)) {
                fprintf(stderr, "%s: the API: %s (--api ADDRESS:PORT, or --no-api)\n", ME, error.c_str());
                return 1;
            }
        }
        if (const size_t m = a.machine_count()) {
            printf("%s: calls answered for the database's enabled machines only (%zu machine%s; machine list)%s\n", ME, m, m == 1 ? "" : "s",
                   no_keys ? ", by the PCBID a call says (--no-keys)" : ", each behind its own key");
            if (const size_t u = a.machines_without_key(); u && !no_keys)
                printf("%s: %zu machine%s no key, and %s answered: machine key PCBID makes one\n", ME, u, u == 1 ? " has" : "s have", u == 1 ? "isn't" : "aren't");
        }
        // how hard a change waits for the disk, as in force on the store's and the accounts' connections
        {
            const int s1 = kept.synchronous(), s2 = a.synchronous(), want = sync == "normal" ? 1 : 2;
            if (s1 != want || s2 != want) {
                fprintf(stderr, "%s: the database's synchronous is %d and %d, not the %d asked for (--sync)\n", ME, s1, s2, want);
                return 1;
            }
            printf(want == 2 ? "%s: --sync full: each change on the disk before its answer goes (synchronous=FULL)\n"
                             : "%s: --sync normal: changes on the disk at the WAL's checkpoints (synchronous=NORMAL): "
                               "a power cut may lose the last moments' calls, never the database\n",
                   ME);
        }
        // the log: a line a call (eamuse/listener.h), or with --log-calls their bodies, PINs hidden
        listener.call_lines = true;
        listener.log_calls = log_calls;
        listener.version = hk::sys::build_id();
        if (log_calls && !listener.quiet)
            printf("%s: --log-calls: every call's body is logged, cards and names in it (PINs and passwords as ***): "
                   "for finding a fault, not for every day\n", ME);
        // SIGTERM or SIGINT: no more connections, the calls in hand answered, the API stopped, the
        // database closed (its WAL folded in), exit 0. Nothing is asked of the accounts or the store
        // after it (`one` stays held), and the program ends there
        listener.stop = &stop_signal;
        listener.stopped = [&] {
            web.reset();
            one.lock();
            acc.reset();
            server.reset();
            const int sig = stop_signal.load();
            ea::say_line(std::string(ME) + ": stopped (" + (sig == SIGTERM ? "SIGTERM" : sig == SIGINT ? "SIGINT" : "signal " + std::to_string(sig)) +
                         "): the calls in hand answered, the database closed");
            std::_Exit(0);
        };
        catch_stop_signals();
        return listener.serve(*server, db);
    }
    ea::SqliteStore st(db);
    if (!st.ok()) return 1;
    if (cmd != "get" && cmd != "backup") {   // (the accounts' tables, and the first admin, with a new database)
        srv::Accounts acc(db, st);
        if (!acc.ok()) return 1;
        first_admin(acc);
    }
    if (cmd == "import") {
        const std::string folder = word(1);
        if (!std::filesystem::is_directory(folder)) { fprintf(stderr, "%s: no folder %s\n", ME, folder.c_str()); return 1; }
        const ea::FolderStore from(folder);
        const ea::Copied c = ea::copy_store(from, st);
        report(c, folder.c_str(), db.c_str());
        return c.not_put.empty() ? 0 : 1;
    }
    if (cmd == "export") {
        const std::string folder = word(1);
        std::error_code ec;
        if (std::filesystem::exists(folder, ec) && !std::filesystem::is_empty(folder, ec)) {
            fprintf(stderr, "%s: %s isn't empty: export makes a folder of its own\n", ME, folder.c_str());
            return 1;
        }
        hk::sys::make_dir(folder);
        ea::FolderStore to(folder);
        const ea::Copied c = ea::copy_store(st, to);
        report(c, db.c_str(), folder.c_str());
        return c.not_put.empty() ? 0 : 1;
    }
    if (cmd == "put" || cmd == "get") {
        ea::Named named;
        if (!ea::named(word(1), named) || !hk::ea::safe_file(word(1))) {
            fprintf(stderr, "%s: %s: not a name of the data folder's things\n", ME, word(1).c_str());
            return 2;
        }
        if (cmd == "get") {
            const std::string data = st.get_named(named);
            fwrite(data.data(), 1, data.size(), stdout);
            return 0;
        }
        std::string data;
        char buf[65536];
        for (size_t got; (got = fread(buf, 1, sizeof buf, stdin)) > 0;) data.append(buf, got);
        const auto held = st.hold();
        if (!st.put_named(named, data)) { fprintf(stderr, "%s: %s not written\n", ME, word(1).c_str()); return 1; }
        return 0;
    }
    if (cmd == "backup" && !backup_dir.empty()) return backup_to_dir(st, db, backup_dir, keep);
    if (cmd == "backup") {
        std::string error;
        if (!st.backup(word(1), error)) { fprintf(stderr, "%s: backup: %s\n", ME, error.c_str()); return 1; }
        printf("%s: %s copied to %s\n", ME, db.c_str(), word(1).c_str());
        return 0;
    }
    const std::string game = cmd == "scores" ? word(2) : cmd == "players" ? "" : word(1);
    if (!game.empty() && game != "server" && !ea::game_by_id(game)) {
        fprintf(stderr, "%s: no game %s (settings lists them)\n", ME, game.c_str());
        return 2;
    }
    if (cmd == "players") return ea::print_players(st);
    if (cmd == "scores") return ea::print_scores(st, word(1), ea::game_by_id(game));
    if (cmd == "settings") return ea::print_settings(st, game);
    return ea::set_setting(st, game, word(2), word(3), {ME, ""});
}
