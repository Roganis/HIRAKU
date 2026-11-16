// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-server: HIRAKU's e-AMUSE3 server on the network, as hiraku-eamuse (tools/eamuse/server.cpp)
// is, keeping what it must remember in a SQLite 3 database (runtime/eamuse/store_sqlite.h) instead
// of a folder: the same answers, the same things kept. Its data goes to and from a folder in the
// folder's own file names (import, export), so a data folder moves into a database and back.
//
//   hiraku-server serve --db FILE [--port 8080] [--bind 0.0.0.0] [--allow ADDRESSES] [--cabinets FILE]
//                 [--key-file FILE] [--clock "2003-06-01 12:00:00"] [--tz 9] [--quiet] [--no-relay]
//   hiraku-server serve --db FILE [--cabinets FILE] --new-cabinet [NAME]  |  [--key-file FILE] --new-key
//   hiraku-server import FOLDER --db FILE     a data folder's files into the database (made if none)
//   hiraku-server export FOLDER --db FILE     the database as a data folder (a new or empty one)
//   hiraku-server put NAME --db FILE          stdin kept as the thing a data folder's file NAME would be
//   hiraku-server get NAME --db FILE          ... and that thing to stdout
//   hiraku-server backup FILE --db FILE       a copy of the database (safe while serve runs)
//   hiraku-server players | scores CARD [GAME] | settings [GAME] | set GAME NAME VALUE  --db FILE
//     serve's options are hiraku-eamuse's; its cabinets.txt and key.txt are beside the database
//     unless --cabinets and --key-file say otherwise. players, scores, settings and set are
//     hiraku-eamuse's --players, --scores, --settings and --set.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "eamuse/admin.h"
#include "eamuse/listener.h"
#include "eamuse/server.h"
#include "eamuse/store_sqlite.h"
#include "shell/sys.h"

namespace {

namespace ea = hk::ea;
constexpr const char *ME = "hiraku-server";

int usage(int status) {
    fprintf(status ? stderr : stdout,
            "usage: hiraku-server serve --db FILE [--port N] [--bind ADDR] [--allow ADDRESSES] [--cabinets FILE]\n"
            "                     [--key-file FILE] [--clock \"YYYY-MM-DD HH:MM:SS\"] [--tz H] [--quiet] [--no-relay]\n"
            "       hiraku-server serve --db FILE [--cabinets FILE] --new-cabinet [NAME]\n"
            "       hiraku-server serve --db FILE [--key-file FILE] --new-key\n"
            "       hiraku-server import FOLDER --db FILE   a data folder's files into the database (made if none)\n"
            "       hiraku-server export FOLDER --db FILE   the database as a data folder (a new or empty one)\n"
            "       hiraku-server put NAME --db FILE        stdin kept as the data folder's file NAME would be\n"
            "       hiraku-server get NAME --db FILE        that thing to stdout\n"
            "       hiraku-server backup FILE --db FILE     a copy of the database, safe while serve runs\n"
            "       hiraku-server players --db FILE         every card, its ref id, each game's player on it and its name\n"
            "       hiraku-server scores CARD [GAME] --db FILE   the card's scores: game, song, chart, score, clear, plays\n"
            "       hiraku-server settings [GAME] --db FILE      the owner's settings: game, name, value, default, range, what it does\n"
            "       hiraku-server set GAME NAME VALUE --db FILE  change one (GAME \"server\": the server's own); VALUE \"\": the default\n"
            "  serve's options are hiraku-eamuse's; cabinets.txt and key.txt are beside the database by default\n");
    return status;
}

// The folder beside the database's file
std::string beside(const std::string &db) {
    const std::filesystem::path p = std::filesystem::path(db).parent_path();
    return p.empty() ? "." : p.string();
}

void report(const ea::Copied &c, const char *from, const char *to) {
    for (const std::string &n : c.not_placed) fprintf(stderr, "%s: %s: no table's (left out)\n", ME, n.c_str());
    for (const std::string &n : c.not_put) fprintf(stderr, "%s: %s: not written\n", ME, n.c_str());
    printf("%s: %zu things from %s to %s%s\n", ME, c.things, from, to, c.not_put.empty() ? "" : " (some not written)");
}

}  // namespace

int main(int argc, char **argv) {
    std::string db;
    bool new_cabinet = false, new_key = false;
    std::string new_name;
    std::vector<std::string> words;   // the command and its own words
    ea::Listener listener;
    listener.program = ME;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (const int taken = listener.option(argc, argv, i)) {
            if (taken == 2) return 2;
        } else if (a == "--db") {
            if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); return 2; }
            db = argv[++i];
        } else if (a == "--new-cabinet") {   // (its name is optional: the next word unless that is an option)
            new_cabinet = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') new_name = argv[++i];
        } else if (a == "--new-key") new_key = true;
        else if (a == "--help" || a == "-h") return usage(0);
        else if (a.size() > 1 && a[0] == '-') { fprintf(stderr, "%s: no option %s\n", ME, a.c_str()); return usage(2); }
        else words.push_back(a);
    }
    if (words.empty()) return usage(2);
    const std::string cmd = words[0];
    const size_t n = words.size() - 1;
    auto word = [&](size_t i) { return i < words.size() ? words[i] : std::string(); };
    const bool fits = cmd == "serve" || cmd == "players" ? n == 0
                      : cmd == "import" || cmd == "export" || cmd == "put" || cmd == "get" || cmd == "backup" ? n == 1
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
        if (const int status = listener.start(beside(db), new_cabinet, new_name, new_key); status >= 0) return status;
        auto store = std::make_unique<ea::SqliteStore>(db);
        if (!store->ok()) return 1;
        ea::Server server(std::move(store));   // every game's, by a call's model (runtime/eamuse/server.cpp)
        return listener.serve(server, db);
    }
    ea::SqliteStore st(db);
    if (!st.ok()) return 1;
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
