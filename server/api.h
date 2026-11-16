// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-server's JSON API (docs/SERVER.md): a second listener beside the game port, started by
// serve, for the server's web pages and an app. Everything under /api/v1/, JSON in and out; who calls
// is a session (log-in: a cookie, and a CSRF value sent back with each change) or a personal token
// (Authorization: Bearer). A thin layer over the accounts (accounts.h): every rule is theirs; the API
// adds who is calling, the shapes, the limits on how often, and what never goes out (a password, PIN,
// hash, a session, token or machine key after the answer that made it, a card's number to anyone but
// its user or an admin). Its own threads and its own connection to the database: the game port's
// calls share the database with it through the database's and the store's locks. hiraku-server's only.
#pragma once

#include <memory>
#include <string>

#include "eamuse/store.h"

namespace hk::srv {

struct ApiOptions {
    bool enabled = true;                 // --no-api: none
    std::string host = "127.0.0.1";      // --api ADDRESS:PORT
    int port = 8081;
    bool insecure_cookies = false;       // --api-insecure-cookies: the session cookie without Secure (tests on plain HTTP)
    bool trust_proxy = false;            // --api-trust-proxy: the caller is X-Forwarded-For's last hop
    bool quiet = false;
    // The web pages (web/, docs/SERVER.md) served at / on the API's listener: --web DIR, else the
    // first folder with an index.html of: "web" beside the program, ../share/hiraku-server/web
    // beside it, the checkout's web/ when the program is in a build tree (its CMakeCache.txt says
    // where); --no-web: none
    bool web_enabled = true;
    std::string web;

    // argv[i] taken if it is one of the API's options, i moved past its value: 1 taken, 0 not one of
    // them, 2 wrong (said)
    int option(int argc, char **argv, int &i);
};

class Api {
public:
    // The API over the database `file`, whose things are `store`'s (the serving store: it outlives this)
    Api(const std::string &file, hk::ea::Store &store, const ApiOptions &);
    ~Api();
    Api(const Api &) = delete;
    Api &operator=(const Api &) = delete;
    // Listening, on threads of its own; false: not (error says why)
    bool start(std::string &error);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hk::srv
