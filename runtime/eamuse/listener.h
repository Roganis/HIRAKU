// SPDX-License-Identifier: GPL-3.0-or-later
// How hiraku-eamuse (tools/eamuse/server.cpp) and hiraku-server (server/main.cpp) serve cabinets on a
// network: the serving options, the cabinets answered and the server's key (eamuse/access.h,
// shell/keyed.h), the NET TAISEN relay (netrelay.h), and the listening loop (a thread a connection,
// the calls answered one at a time) around a Server (server.h), whatever keeps its data. Not the
// cores': their servers are called in the game's own process.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "eamuse/access.h"
#include "shell/keyed.h"

namespace hk::ea {

class Server;

struct Listener {
    const char *program = "hiraku-eamuse";   // the name its messages begin with
    int port = 8080;
    std::string bind = "0.0.0.0";
    AddressList allowed;          // --allow; empty: any
    std::string cabinets_file;    // --cabinets; "": cabinets.txt in the folder (start's)
    std::string key_file;         // --key-file; "": key.txt in the folder
    int64_t clock_start = -1;     // --clock; -1: the host's clock in --tz
    int tz_hours = 9;
    bool quiet = false, relay = true;

    // argv[i] taken if it is a serving option (--port --bind --allow --cabinets --key-file --clock
    // --tz --quiet --no-relay), i moved past its value: 1 taken, 0 not one of them, 2 wrong (said:
    // the program ends with 2)
    int option(int argc, char **argv, int &i);

    // The cabinets' list and the key read, beside `folder` unless named (and made: --new-cabinet,
    // --new-key, then nothing is served). -1: ready to serve; else the program's exit status
    int start(const std::string &folder, bool new_cabinet = false, const std::string &new_name = "", bool new_key = false);
    // Serves `server` (its clock, log and admit set here) for ever, or until `stop`; `kept`: where it
    // keeps its data, for the first line. Returns if it can't listen or was stopped: the exit status
    int serve(Server &server, const std::string &kept);

    CabinetList cabinets;
    net::Key key;
    // hiraku-server's machines (server/accounts.h): a call's srcid to 1 answered, 0 refused, -1 not
    // theirs to say (the cabinets' list decides, as without it). Asked once a call
    std::function<int(const std::string &srcid)> machines;
    // ... and their keys (shell/keyed.h): with these, a cabinet with a machine's key ("HKK2") is
    // taken behind that machine's key, and only calls as that machine (its srcid) from it. Unset
    // (hiraku-eamuse): such a cabinet is turned away. Called on the connections' threads
    net::MachineKeys machine_key;
    // ... and whether only such cabinets are taken now (the database has machines; not --no-keys)
    std::function<bool()> machines_only;

    // hiraku-server's log (hiraku-eamuse leaves these, and says each call's whole body unless --quiet).
    // call_lines: one line a call (call_line()), no body; log_calls: the bodies, with hidden()'s PINs
    // and passwords as ***. machine_name: a machine's name by its PCBID ("": none)
    bool call_lines = false, log_calls = false;
    std::function<std::string(const std::string &pcbid)> machine_name;
    std::string version;   // the build, said in the first line ("": not said)
    // Set: serve() stops once it is not 0 (the signal's number): no connection taken after, the ones
    // in hand answered (kStopSeconds at most), then stopped() called with no call being answered
    // (and none answered after it); serve() returns 0 when it returns
    const std::atomic<int> *stop = nullptr;
    std::function<void()> stopped;
    static constexpr int kStopSeconds = 3;

    // The games' port opened (serve() opens it if this wasn't called): -1 listening, else the exit
    // status (said in one line)
    int open();
    int fd = -1;
};

// A log line's time: now, ISO 8601 in UTC ("2026-11-16T09:30:00Z")
std::string utc_now();
// A line written whole (one write, then flushed): lines from several threads never mix
void say_line(const std::string &line);
// Log text with every PIN and password in it as *** (attributes and elements named pass, passwd,
// password, ea_passwd, cpass, pin; the PIN word of IIDX's <... method="pc" command="reg CARD NAME PIN ...">)
std::string hidden(const std::string &text);

}  // namespace hk::ea
