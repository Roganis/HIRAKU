// SPDX-License-Identifier: GPL-3.0-or-later
// e-AMUSEMENT for the System 573 GuitarFreaks and DrumMania (eagfdm.h): the cabinet's side. The server's
// answers to them are runtime/eamuse/server_gfdm.cpp.
#include "sys573/eagfdm.h"
#include "shell/sys.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <cstdio>
#include <ctime>
#include <thread>

namespace hk::k573 {

bool Eamuse::open(const std::string &where, const std::string &dir, const std::string &log, std::function<int64_t()> now,
                  const std::string &pcbid, const std::string &key) {
    pcbid_ = pcbid;
    if (where != "local") {
        remote_ = std::make_unique<ea::Remote>(where, net::Key(key));
        if (!remote_->ok()) {
            fprintf(stderr, "--eamuse: %s is not http://host[:port][/path]\n", where.c_str());
            return false;
        }
        // with a key: said now if the server's is another (the game would only show a network error)
        if (std::string why; remote_->key().set() && !remote_->reachable(&why)) fprintf(stderr, "e-AMUSEMENT: %s\n", why.c_str());
    }
    if (!log.empty() && !(log_ = fopen(log.c_str(), "w"))) {
        fprintf(stderr, "HK_573_NET_LOG: cannot write %s\n", log.c_str());
        return false;
    }
    server_ = std::make_unique<ea::Server>(dir);
    server_->now = std::move(now);
    lan_ = std::make_unique<ea::Lan>([this](const ea::Lan::Target &t, const std::string &req, std::function<void(std::string)> done) {
        if (!remote_) {
            done(server_->handle(req));
            return;
        }
        // online: off the emulator's thread; the unit's recv waits for it
        std::thread([this, t, req = std::string(req), done = std::move(done)]() mutable {
            std::string error;
            if (!pcbid_.empty()) ea::set_srcid(req, pcbid_);   // (-p: this cabinet's own, not the dump's)
            std::string answer = remote_->exchange(t, req, &error);
            if (log_) {
                fprintf(log_, "online %s:%u\n  %s\n  %s\n", t.services ? remote_->host().c_str() : t.host.c_str(),
                        t.services ? remote_->port() : t.port, ea::clear(req).c_str(),
                        answer.empty() ? ("failed: " + error).c_str() : ea::clear(answer).c_str());
                fflush(log_);
            }
            done(std::move(answer));
        }).detach();
    });
    lan_->now = server_->now;
    if (log_) lan_->log = server_->log = [this](const std::string &s) { fprintf(log_, "%s\n", s.c_str()); fflush(log_); };
    if (remote_)
        printf("e-AMUSEMENT: online, %s%s%s%s\n", where.c_str(), remote_->key().set() ? ", behind the server's key" : "",
               pcbid_.empty() ? "" : ", as cabinet ", pcbid_.c_str());
    return true;
}

}  // namespace hk::k573
