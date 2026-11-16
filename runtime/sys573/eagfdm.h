// SPDX-License-Identifier: GPL-3.0-or-later
// e-AMUSEMENT for the System 573 GuitarFreaks and DrumMania (9th to 11th Mix): the network the
// network PCB unit is on (runtime/eamuse/lan.h), and the server it reaches: HIRAKU's own inside the
// core (runtime/eamuse/server.h; these games' answers: server_gfdm.cpp) or one online (PLAN.md section
// 19, "e-AMUSEMENT").
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#include "eamuse/lan.h"
#include "eamuse/remote.h"
#include "eamuse/server.h"

namespace hk::k573 {

class Eamuse {
public:
    // where: "local" (the server in the core, keeping its data in `dir`) or http://host[:port][/path]
    // (online). log: "" or a file for every socket call and every call and answer in clear.
    // now: the cabinet's clock (seconds since 1970, local time). pcbid: "" or the cabinet's own PCBID,
    // sent to an online server in place of the game's (-p). key: "" or an online server's key
    // (-eakey; shell/keyed.h). false: a message was printed.
    bool open(const std::string &where, const std::string &dir, const std::string &log, std::function<int64_t()> now,
              const std::string &pcbid = "", const std::string &key = "");
    ea::Lan *lan() { return lan_.get(); }

private:
    std::unique_ptr<ea::Lan> lan_;
    std::unique_ptr<ea::Server> server_;
    std::unique_ptr<ea::Remote> remote_;
    FILE *log_ = nullptr;
    std::string pcbid_;
};

}  // namespace hk::k573
