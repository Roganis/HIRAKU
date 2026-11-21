// SPDX-License-Identifier: GPL-3.0-or-later
// The test switches a run was started with, for its log's first lines: HIRAKU's HK_* environment
// variables, SDL's own (SDL_*) and the two of the recorded code's (WAIT_SPIN_MICROSECONDS,
// OVERRIDE_VECTOR_ISA). Two cabinet runs with the same command line behaved differently and nothing
// in their logs said why (2026-11): a switch left set in the environment changes the pacing, the
// sound or the picture path without a word. A secret's value is never written: a name with KEY, PIN,
// PASS, TOKEN, SECRET or CARD in it (HK_EA_KEY, the server's key) is said without it, whatever else it is.
#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace hk {

// env: the environment's "NAME=value" strings, ending with a null one (environ)
inline std::string switches_line(const char *const *env) {
    static const char *const SECRET[] = {"KEY", "PIN", "PASS", "TOKEN", "SECRET", "CARD"};
    std::vector<std::string> said;
    for (; env && *env; env++) {
        const std::string e = *env;
        const size_t eq = e.find('=');
        const std::string name = e.substr(0, eq), value = eq == std::string::npos ? "" : e.substr(eq + 1);
        const bool sdl = name.rfind("SDL_", 0) == 0;
        if (name.rfind("HK_", 0) != 0 && !sdl && name != "WAIT_SPIN_MICROSECONDS" && name != "OVERRIDE_VECTOR_ISA") continue;
        std::string upper = name;
        for (char &c : upper) c = (char)toupper((unsigned char)c);
        bool secret = false;   // (WAIT_SPIN_MICROSECONDS has a "PIN" in it, and is the recorded code's spin)
        for (const char *s : SECRET) secret = secret || (upper.find(s) != std::string::npos && name != "WAIT_SPIN_MICROSECONDS");
        // (SDL's are said in full when short; a long value, a list or a path, by its length)
        const size_t most = sdl ? 48 : 160;
        if (secret) said.push_back(name + " (its value not shown)");
        else if (value.size() > most) said.push_back(name + " (" + std::to_string(value.size()) + " characters)");
        else said.push_back(name + "=" + (value.find(' ') != std::string::npos || value.empty() ? "\"" + value + "\"" : value));
    }
    if (said.empty()) return "environment: no test switches set (none of HK_*, SDL_*, WAIT_SPIN_MICROSECONDS, OVERRIDE_VECTOR_ISA)";
    std::sort(said.begin(), said.end());
    std::string out = "environment: test switches set:";
    for (size_t i = 0; i < said.size(); i++) out += (i ? ", " : " ") + said[i];
    return out;
}

}  // namespace hk
