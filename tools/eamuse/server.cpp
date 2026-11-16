// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-eamuse: HIRAKU's e-AMUSE3 server on the network. The same server the cores run inside
// themselves offline (runtime/eamuse/server.h), here behind a plain HTTP/1.0 listener, so several
// cabinets (HIRAKU with -url host:port) can share cards, player data and rankings: every game the
// server answers, the two cabinets of a GuitarFreaks and DrumMania session among them. All of it is
// kept in one folder (one card numbering, one set of KONAMI IDs). docs/EAMUSE3.md describes the protocol.
//
// It also runs NET TAISEN's lobby and relay for pop'n music 12-18 cabinets (runtime/eamuse/netrelay.h),
// on TCP port 5740 of the same address.
//
//   hiraku-eamuse [--port 8080] [--bind 0.0.0.0] [--data DIR] [--allow ADDRESSES] [--cabinets FILE]
//                 [--key-file FILE] [--clock "2003-06-01 12:00:00"] [--tz 9] [--quiet] [--no-relay]
//   hiraku-eamuse [--data DIR] [--cabinets FILE] --new-cabinet [NAME]
//   hiraku-eamuse [--data DIR] [--key-file FILE] --new-key
//   hiraku-eamuse [--data DIR] --players | --scores CARD [GAME] | --settings [GAME] | --set GAME NAME VALUE
//     --data   where it keeps what it must remember (default ./eamuse-data)
//     --allow  the addresses it takes connections from, calls and relay: 192.168.1.0/24,10.0.0.5
//              (and this computer's own); without it, any
//     --cabinets  the cabinets it answers, a PCBID a line (default: cabinets.txt in the data folder,
//              when there is one); without the file, any. Read again whenever it changes
//     --new-cabinet  a PCBID nobody else has, added to that file with NAME after it and printed: the
//              cabinet is started with -p PCBID (the launcher: Settings, e-AMUSEMENT, PCBID)
//     --key-file  the file with the server's key (default: key.txt in the data folder, when there is
//              one). With a key, calls and relay are taken only behind it (shell/keyed.h): nothing
//              goes over the network in clear, and who hasn't the key is answered nothing. Each
//              cabinet is given the same key (-eakey KEY; the launcher: Settings, e-AMUSEMENT, Server key)
//     --new-key  a key nobody can guess, written to that file and printed
//     --players, --scores, --settings, --set: what the data folder holds, printed (tab-separated, a
//              line a thing; names as the games keep them, their bytes), or a setting changed
//              (runtime/eamuse/readers.h, games.h), then it ends. No PIN or password is printed.
//              GAME: a game's id (popn14, iidx16...: --settings lists them) or model (E03), or
//              "server" for the settings that are no game's. --set's VALUE is checked against the
//              setting's range; "" puts the default back (the setting's file emptied, as if none)
//     --clock  the cabinets' time (ntp.get), running from this start (default: the host's clock)
//     --tz     with the host's clock: the cabinets' time zone, hours from UTC (default 9, Japan: the
//              games set their clocks to what ntp.get says and stamp their own times "+9")
//     --quiet  don't print each call
//     --no-relay  no NET TAISEN relay
// The listener and the commands are runtime/eamuse/listener.h's and admin.h's, which hiraku-server
// (server/main.cpp) shares; this program keeps its data in a folder.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "eamuse/admin.h"
#include "eamuse/listener.h"
#include "eamuse/server.h"
#include "shell/sys.h"

int main(int argc, char **argv) {
    std::string data = "eamuse-data";
    bool new_cabinet = false, new_key = false;
    std::string new_name;
    std::string report, report_card, report_game, set_name, set_value;   // --players, --scores, --settings, --set
    hk::ea::Listener listener;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto val = [&]() -> const char * {
            if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); exit(2); }
            return argv[++i];
        };
        if (const int taken = listener.option(argc, argv, i)) {
            if (taken == 2) return 2;
        } else if (a == "--data") data = val();
        else if (a == "--new-cabinet") {   // (its name is optional: the next word unless that is an option)
            new_cabinet = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') new_name = argv[++i];
        } else if (a == "--new-key") new_key = true;
        else if (a == "--players") report = a;
        else if (a == "--scores" || a == "--settings") {   // (--settings' game is optional, as --scores')
            report = a;
            if (a == "--scores") report_card = val();
            if (i + 1 < argc && argv[i + 1][0] != '-') report_game = argv[++i];
        } else if (a == "--set") {
            report = a;
            report_game = val(), set_name = val(), set_value = val();
        } else {
            fprintf(stderr, "usage: hiraku-eamuse [--port N] [--bind ADDR] [--data DIR] [--allow ADDRESSES] [--cabinets FILE] "
                            "[--key-file FILE] [--clock \"YYYY-MM-DD HH:MM:SS\"] [--tz H] [--quiet] [--no-relay]\n"
                            "       hiraku-eamuse [--data DIR] [--cabinets FILE] --new-cabinet [NAME]\n"
                            "       hiraku-eamuse [--data DIR] [--key-file FILE] --new-key\n"
                            "       hiraku-eamuse [--data DIR] --players | --scores CARD [GAME] | --settings [GAME] | --set GAME NAME VALUE\n"
                            "  --allow ADDRESSES   take connections only from these (192.168.1.0/24,10.0.0.5) and this computer\n"
                            "  --cabinets FILE     answer only the cabinets whose PCBIDs it lists, one a line\n"
                            "                      (default: cabinets.txt in the data folder, when there is one)\n"
                            "  --new-cabinet NAME  add a new PCBID to that file and print it, for the cabinet's -p\n"
                            "  --key-file FILE     the server's key (default: key.txt in the data folder, when there is one):\n"
                            "                      calls and relay only behind it, nothing in clear on the network\n"
                            "  --new-key           make a key, write it to that file and print it, for the cabinets' -eakey\n"
                            "  --players           every card the data folder knows, its ref id, each game's player on it and its name\n"
                            "  --scores CARD [GAME]  the card's scores: game, song, chart, score, clear, plays (-1: not kept)\n"
                            "  --settings [GAME]   the owner's settings: game, name, value, default, range, what it does\n"
                            "  --set GAME NAME VALUE  change one (GAME \"server\": the server's own); VALUE \"\": back to the default\n");
            return a == "--help" || a == "-h" ? 0 : 2;
        }
    }
    if (!report.empty()) {   // what the data folder holds, or a setting changed, and no server
        if (!hk::sys::exists(data)) { fprintf(stderr, "hiraku-eamuse: no folder %s (--data)\n", data.c_str()); return 1; }
        if (!report_game.empty() && report_game != "server" && !hk::ea::game_by_id(report_game)) {
            fprintf(stderr, "hiraku-eamuse: no game %s (--settings lists them)\n", report_game.c_str());
            return 2;
        }
        hk::ea::FolderStore st(data);
        if (report == "--players") return hk::ea::print_players(st);
        if (report == "--scores") return hk::ea::print_scores(st, report_card, hk::ea::game_by_id(report_game));
        if (report == "--settings") return hk::ea::print_settings(st, report_game);
        return hk::ea::set_setting(st, report_game, set_name, set_value, {"hiraku-eamuse", "--"});
    }
    hk::sys::make_dir(data);
    if (const int status = listener.start(data, new_cabinet, new_name, new_key); status >= 0) return status;
    hk::ea::Server server(data);   // every game's, by a call's model (runtime/eamuse/server.cpp)
    return listener.serve(server, data);
}
