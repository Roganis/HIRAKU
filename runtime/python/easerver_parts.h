// SPDX-License-Identifier: GPL-3.0-or-later
// What the e-AMUSEMENT server's files (easerver*.cpp) share: the protocol's pieces (eamuse/protocol.h)
// and the server's own. Not for the server's users (easerver.h).
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "eamuse/protocol.h"
#include "python/easerver.h"

namespace hk::py {

using ea::attr;
using ea::esc;
using ea::from_lines;
using ea::header;
using ea::num;
using ea::read_attrs;
using ea::Record;
using ea::safe_name;
using ea::to_line;

// The three rankings (docs/EAMUSE3.md, "rank"): what is kept and how entries compare
struct EamuseServer::Board {
    const char *file;
    const char *who;                   // the request's field naming the entrant
    std::vector<const char *> keep;    // fields from the request kept with the entry
    const char *first, *second;        // ranked by these, larger first
    int second_base;                   // parts is hex
};
using Board = EamuseServer::Board;
// easerver_rank.cpp: pop'n 9's three boards; what pop'n 10 on keep of an entry (rank.exp)
extern const Board BOARD_SCORE, BOARD_H, BOARD_GZ;
extern const std::vector<const char *> EXP_KEEP;
bool better(const Board &b, const Record &x, const Record &y);   // x ranks above y on b

// easerver.cpp: LZSS as the games pack (X-Compress: lz77), base64, printf of two numbers
std::string lzss(const std::string &in);
std::string base64(const std::string &in);
std::string unbase64_b(const std::string &x);
std::string fmt(const char *f, unsigned a, unsigned b);
// The PC pop'n games, libavs-win32's e-AMUSEMENT (runtime/pc/): pop'n 15, 16, 17, 18
bool pc_popn(const std::string &game);

// A player record's parts, as the later pop'n games read them (easerver_hc.cpp, easerver_p24.cpp):
// a field: its name, AVS type, count (0: one value) and a new player's value
struct Field {
    const char *name, *type;
    int count;
    const char *value;
};

// <name __type="t" [__count="n"]>values</name>: `value` for each of an array's values, or (whole)
// the array's values as kept
inline std::string node(const Field &f, const std::string &value, bool whole = false) {
    std::string v = value;
    if (f.count > 0 && !whole) {
        v.clear();
        for (int i = 0; i < f.count; i++) v += (i ? " " : "") + value;
    }
    return std::string("<") + f.name + " __type=\"" + f.type + "\"" + (f.count > 0 ? " __count=\"" + std::to_string(f.count) + "\"" : "") +
           ">" + esc(v) + "</" + f.name + ">";
}
template <size_t N>
std::string part(const char *name, const Field (&fields)[N], const std::map<std::string, std::string> &given) {
    std::string x = std::string("<") + name + ">";
    for (const Field &f : fields) {
        const auto g = given.find(f.name);
        x += g != given.end() ? node(f, g->second, true) : node(f, f.value ? f.value : "");
    }
    return x + "</" + name + ">";
}

// The text of a request's first <name>...</name> ("" if none, or empty)
inline std::string child(const std::string &inner, const char *name) {
    const std::string open = std::string("<") + name;
    for (size_t at = inner.find(open); at != std::string::npos; at = inner.find(open, at + 1)) {
        const char after = at + open.size() < inner.size() ? inner[at + open.size()] : '\0';
        if (after != ' ' && after != '>' && after != '/') continue;
        const size_t gt = inner.find('>', at);
        if (gt == std::string::npos || inner[gt - 1] == '/') return "";
        const size_t close = inner.find(std::string("</") + name + ">", gt);
        return close == std::string::npos ? "" : inner.substr(gt + 1, close - gt - 1);
    }
    return "";
}

// A player's fields kept in a Record as part.field: those of a part given, and the part's from a
// request
template <size_t N>
std::map<std::string, std::string> kept(const Record &p, const char *part, const Field (&fields)[N]) {
    std::map<std::string, std::string> given;
    for (const Field &f : fields)
        if (const std::string k = std::string(part) + "." + f.name; p.count(k)) given[f.name] = attr(p, k.c_str());
    return given;
}
template <size_t N>
void keep(Record &p, const char *part, const Field (&fields)[N], const std::string &from) {
    for (const Field &f : fields)
        if (from.find(std::string("<") + f.name + " ") != std::string::npos) p[std::string(part) + "." + f.name] = child(from, f.name);
}


}  // namespace hk::py
