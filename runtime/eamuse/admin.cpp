// SPDX-License-Identifier: GPL-3.0-or-later
// What a server's store holds, printed, and a setting changed (admin.h).
#include "eamuse/admin.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "eamuse/readers.h"
#include "eamuse/tables.h"

namespace hk::ea {

namespace {

std::string label(const Game &g) { return *g.id ? g.id : g.model; }

Identity identity(const Store &st, const std::string &card) {
    for (const Identity &id : players(st))
        if (id.card == card) return id;
    return {card, ""};
}

std::string trimmed(std::string v) {
    while (!v.empty() && std::isspace((unsigned char)v.back())) v.pop_back();
    return v;
}

void print_setting(const std::string &game, const Setting &s, const std::string &now) {
    const std::string range = s.kind == Setting::number ? std::to_string(s.min) + ".." + std::to_string(s.max) : "text";
    const std::string line = game + "\t" + s.name + "\t" + trimmed(now) + "\t" + s.by_default + "\t" + range + "\t" + s.help;
    fwrite(line.data(), 1, line.size(), stdout);
    fputc('\n', stdout);
}

}  // namespace

int print_players(const Store &st) {
    for (const Identity &id : players(st)) {
        std::string line = id.card + "\t" + (id.ref.empty() ? "-" : id.ref);
        for (const Game &g : games()) {
            Profile p;
            if (profile(st, g, id, p)) line += "\t" + label(g) + "\t" + p.name;
        }
        fwrite(line.data(), 1, line.size(), stdout);
        fputc('\n', stdout);
    }
    return 0;
}

int print_scores(const Store &st, const std::string &card, const Game *only) {
    const Identity id = identity(st, card);
    for (const Game &g : games()) {
        if (only && &g != only) continue;
        for (const Score &s : scores(st, g, id))
            printf("%s\t%ld\t%ld\t%ld\t%d\t%ld\n", label(g).c_str(), s.song, s.chart, s.score, s.clear, s.plays);
    }
    return 0;
}

int print_settings(const Store &st, const std::string &which) {
    if (which.empty() || which == "server")
        for (const Setting &s : server_settings()) print_setting("server", s, setting(st, s.name));
    for (const Game &g : games())
        if (*g.model && (which.empty() || &g == game_by_id(which)))   // (a model the table doesn't name: its own phase-MODEL)
            for (const Setting &s : g.settings) print_setting(label(g), s, setting(st, g, s.name));
    return 0;
}

int set_setting(Store &st, const std::string &game, const std::string &name, const std::string &value, const AdminNames &as) {
    const char *p = as.program, *d = as.dash;
    const Game *g = game == "server" ? nullptr : game_by_id(game);
    if (!g && game != "server") { fprintf(stderr, "%s: %sset: no game %s (%ssettings lists them)\n", p, d, game.c_str(), d); return 2; }
    const Setting *s = find_setting(g, name.c_str());
    if (!s) { fprintf(stderr, "%s: %sset: %s has no setting %s (%ssettings %s lists them)\n", p, d, game.c_str(), name.c_str(), d, game.c_str()); return 2; }
    if (g && s->table == &tables::setting && (!*g->model || strchr(g->model, '?'))) { fprintf(stderr, "%s: %sset: %s has no settings of its own\n", p, d, game.c_str()); return 2; }
    if (value.find_first_of("\r\n") != std::string::npos) { fprintf(stderr, "%s: %sset: one line\n", p, d); return 2; }
    if (s->kind == Setting::number && !value.empty()) {
        char *end = nullptr;
        const long n = strtol(value.c_str(), &end, 10);
        if (*end || n < s->min || n > s->max) {
            fprintf(stderr, "%s: %sset: %s's %s is a number from %ld to %ld\n", p, d, game.c_str(), name.c_str(), s->min, s->max);
            return 2;
        }
    }
    const Key key = setting_key(g, *s);
    const auto held = st.hold();
    // "": the default back. A setting's empty file reads as none (the answers take the default), so
    // one that is kept is emptied; one that isn't stays so
    if (value.empty() ? !st.get(*s->table, key).empty() && !st.put(*s->table, key, "") : !st.put(*s->table, key, value + "\n")) {
        fprintf(stderr, "%s: %sset: %s not written\n", p, d, st.label(*s->table, key).c_str());
        return 1;
    }
    printf("%s\t%s\t%s\n", game.c_str(), name.c_str(), value.empty() ? s->by_default : value.c_str());
    return 0;
}

}  // namespace hk::ea
