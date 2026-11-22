// SPDX-License-Identifier: GPL-3.0-or-later
// Mods' scripts: Lua states, the sandbox, the hk table, the callbacks and what they draw
// (host/modscript.h; docs/MODS.md, "Scripts").
#include "host/modscript.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include "host/mods.h"
#include "shell/core.h"

// (Lua compiled as C++: cmake/lua.cmake)
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

namespace hk::modscript {

namespace {

constexpr int kStep = 1000;              // the count hook's step (instructions)
constexpr size_t kPieces = 4096;         // pieces a layer may have a frame
constexpr size_t kPiecePixels = 1u << 22;   // a text's or a line's picture, at most

struct Script {
    std::string mod, folder, file, chunk;   // chunk: the file's name in messages (folder/mod.lua)
    lua_State *L = nullptr;
    bool alive = true;
    bool memory = false;           // requires "memory": hk.read*/write*
    bool big = false;              // core().mem()'s byte order
    std::unordered_map<std::string, int> regs;   // the core's registers by name (a hook's call)
    int on_frame = 0, on_input = 0, draw_game = 0, draw_window = 0;   // references, 0 none
    int regs_ref = 0, game_ref = 0, window_ref = 0;
    uint32_t *hook_regs = nullptr;   // while a hook's call runs
    const char *in = "";             // the callback running, for messages
    bool drawing = false;            // in draw_game/draw_window: nothing but drawing
    int64_t budget = 0, left = 0;    // instructions: this callback's, left of it
    bool over_budget = false, over_memory = false;
    size_t used = 0, peak = 0;
    uint64_t calls = 0;
    int64_t most = 0;                // the most instructions one callback took
    std::string stopped;             // why the script was stopped
    bool closing = false;            // its state being closed: hk does nothing (finalizers, __gc)

    Script() = default;
    Script(const Script &) = delete;
    Script &operator=(const Script &) = delete;
    ~Script();   // its state closed (clear(), and at exit: g_scripts' own end)
};

std::deque<Script> g_scripts;   // (a deque: the Lua states' extra space points at them)
const uint8_t (*g_font)[8] = nullptr;
std::function<void(const std::vector<std::string> &)> g_notice;
std::vector<Piece> g_game, g_window;
int g_view[6] = {0, 0, 0, 0, 0, 0};   // x, y, w, h, ww, wh
std::vector<uint32_t> g_frame_copy, g_between_copy;

// on_input: the controls given since the last frame, each an edge
struct Event {
    std::string name;
    bool down;
    double time;
};
std::mutex g_input_lock;
std::vector<Event> g_events;
std::unordered_map<std::string, bool> g_down;   // each control's last state
bool g_wants_input = false;                     // a script has on_input (set at load)
uint64_t g_dropped = 0;

Script &of(lua_State *L) { return **static_cast<Script **>(lua_getextraspace(L)); }
// ... for hk's functions: an error while the state is being closed (a __gc finalizer then: at exit
// the machine and the mods' settings may be gone already)
Script &live(lua_State *L) {
    Script &s = of(L);
    if (s.closing) luaL_error(L, "the script is being closed");
    return s;
}

void *alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    Script &s = *static_cast<Script *>(ud);
    const size_t old = ptr ? osize : 0;
    if (nsize == 0) {
        free(ptr);
        s.used -= old;
        return nullptr;
    }
    if (nsize > old && s.used - old + nsize > kMemory) {
        s.over_memory = true;
        return nullptr;
    }
    void *p = realloc(ptr, nsize);
    if (p) {
        s.used = s.used - old + nsize;
        s.peak = std::max(s.peak, s.used);
    }
    return p;
}

void count_hook(lua_State *L, lua_Debug *) {
    Script &s = of(L);
    s.left -= kStep;
    if (s.left < 0) {
        s.over_budget = true;
        lua_sethook(L, count_hook, LUA_MASKCOUNT, 1);   // (a pcall in the script that caught it: again at once)
        luaL_error(L, "over its budget of %d instructions", (int)s.budget);
    }
}

void say(Script &s, const std::string &line) {
    fprintf(stderr, "mods: %s: %s\n", s.mod.c_str(), line.c_str());
    if (g_notice) g_notice({s.mod, line.substr(0, 60)});
}

// The script's Lua state closed and freed: whatever it holds, through alloc (which frees whatever
// the cap), its finalizers run with hk refused and a budget of their own
void close_state(Script &s) {
    if (!s.L) return;
    s.closing = true;
    s.budget = s.left = kBudget;
    s.over_budget = false;
    lua_sethook(s.L, count_hook, LUA_MASKCOUNT, kStep);
    lua_close(s.L);
    s.L = nullptr;
}

void stop(Script &s, const std::string &why) {
    if (!s.alive) return;
    s.alive = false;
    s.stopped = why + " (in " + s.in + ")";
    fprintf(stderr, "mods: %s: its script is stopped, the game goes on: %s\n", s.mod.c_str(), s.stopped.c_str());
    if (g_notice) g_notice({s.mod, "Its script stopped: see the log"});
    close_state(s);
}

// The function on the stack with the nargs values above it, in a protected call with `budget`
// instructions. false, with why: it failed (an error, over its budget or its memory)
bool protected_call(Script &s, int nargs, const char *what, int64_t budget, std::string &why) {
    lua_State *L = s.L;
    s.in = what;
    s.budget = s.left = budget;
    s.over_budget = s.over_memory = false;
    lua_sethook(L, count_hook, LUA_MASKCOUNT, kStep);
    const int status = lua_pcall(L, nargs, 0, 0);
    s.calls++;
    s.most = std::max(s.most, budget - s.left);
    if (status == LUA_OK && !s.over_budget && !s.over_memory) return true;
    why = status != LUA_OK && lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "an error";
    if (s.over_budget) why = "over its budget of " + std::to_string(budget) + " instructions";
    else if (s.over_memory) why = "over its memory, " + std::to_string(kMemory >> 20) + " MB";
    if (status != LUA_OK) lua_pop(L, 1);
    return false;
}
// ... a callback: the script stopped if it fails
bool call(Script &s, int nargs, const char *what, int64_t budget) {
    std::string why;
    if (protected_call(s, nargs, what, budget, why)) return true;
    stop(s, why);
    return false;
}

// ---- the sandbox --------------------------------------------------------------------------------

int l_print(lua_State *L) {
    Script &s = of(L);
    std::string line;
    for (int i = 1, n = lua_gettop(L); i <= n; i++) {
        size_t len;
        const char *t = luaL_tolstring(L, i, &len);
        line += (i > 1 ? "\t" : "") + std::string(t, len);
        lua_pop(L, 1);
    }
    fprintf(stderr, "mods: %s: %s\n", s.mod.c_str(), line.c_str());
    return 0;
}

int l_collectgarbage(lua_State *L) {
    const char *opt = luaL_optstring(L, 1, "");
    if (strcmp(opt, "count") != 0) return luaL_error(L, "collectgarbage: only \"count\" in a mod's script");
    const int k = lua_gc(L, LUA_GCCOUNT, 0), b = lua_gc(L, LUA_GCCOUNTB, 0);
    lua_pushnumber(L, (lua_Number)k + (lua_Number)b / 1024);
    return 1;
}

// load(chunk [, name [, mode [, env]]]): text only (no precompiled chunks)
int l_load(lua_State *L) {
    const int n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, 1);
    if (n >= 2) lua_pushvalue(L, 2);
    else lua_pushnil(L);
    lua_pushliteral(L, "t");
    int args = 3;
    if (n >= 4) lua_pushvalue(L, 4), args++;
    lua_call(L, args, LUA_MULTRET);
    return lua_gettop(L) - n;
}

// math.randomseed(x [, y]): with a seed only (none would be the clock's)
int l_randomseed(lua_State *L) {
    luaL_checkinteger(L, 1);
    const int n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, n, LUA_MULTRET);
    return lua_gettop(L);
}

// ---- hk -----------------------------------------------------------------------------------------

void need_memory(lua_State *L, Script &s, const char *fn) {
    if (!s.memory) luaL_error(L, "hk.%s: the mod's requires doesn't list \"memory\"", fn);
}

int read_n(lua_State *L, int size, const char *fn) {
    Script &s = live(L);
    need_memory(L, s, fn);
    const uint32_t a = (uint32_t)luaL_checkinteger(L, 1);
    const bool sign = lua_toboolean(L, 2);
    const uint8_t *m = core().mem(a, (uint32_t)size);
    if (!m) {
        lua_pushnil(L);
        return 1;
    }
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)m[i] << (8 * (s.big ? size - 1 - i : i));
    lua_pushinteger(L, sign ? (size == 1 ? (lua_Integer)(int8_t)v : size == 2 ? (lua_Integer)(int16_t)v : (lua_Integer)(int32_t)v) : (lua_Integer)v);
    return 1;
}
int write_n(lua_State *L, int size, const char *fn) {
    Script &s = live(L);
    need_memory(L, s, fn);
    if (s.drawing) return luaL_error(L, "hk.%s: not while drawing (draw_game, draw_window only draw)", fn);
    const uint32_t a = (uint32_t)luaL_checkinteger(L, 1);
    const uint32_t v = (uint32_t)luaL_checkinteger(L, 2);
    auto *m = const_cast<uint8_t *>(core().mem(a, (uint32_t)size));
    if (!m) {
        lua_pushboolean(L, 0);
        return 1;
    }
    for (int i = 0; i < size; i++) m[i] = (uint8_t)(v >> (8 * (s.big ? size - 1 - i : i)));
    core().code_changed(a, (uint32_t)size);
    lua_pushboolean(L, 1);
    return 1;
}
int l_read8(lua_State *L) { return read_n(L, 1, "read8"); }
int l_read16(lua_State *L) { return read_n(L, 2, "read16"); }
int l_read32(lua_State *L) { return read_n(L, 4, "read32"); }
int l_write8(lua_State *L) { return write_n(L, 1, "write8"); }
int l_write16(lua_State *L) { return write_n(L, 2, "write16"); }
int l_write32(lua_State *L) { return write_n(L, 4, "write32"); }

int l_setting(lua_State *L) {
    Script &s = live(L);
    const char *name = luaL_checkstring(L, 1);
    int64_t v;
    if (!mods::setting_value(s.folder, name, v)) return luaL_error(L, "hk.setting: the mod has no [[setting]] '%s'", name);
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}
int l_set_setting(lua_State *L) {
    Script &s = live(L);
    if (s.drawing) return luaL_error(L, "hk.set_setting: not while drawing");
    const char *name = luaL_checkstring(L, 1);
    const lua_Integer v = luaL_checkinteger(L, 2);
    int64_t now;
    if (!mods::set_setting(s.folder, name, v, now)) return luaL_error(L, "hk.set_setting: the mod has no [[setting]] '%s'", name);
    lua_pushinteger(L, (lua_Integer)now);
    return 1;
}
int l_counter(lua_State *L) {
    Script &s = live(L);
    lua_pushinteger(L, (lua_Integer)mods::counter_value(s.folder, luaL_checkstring(L, 1)));
    return 1;
}
int l_frame(lua_State *L) {
    live(L);
    lua_pushinteger(L, (lua_Integer)core().frame_count());
    return 1;
}
int l_time(lua_State *L) {
    live(L);
    lua_pushnumber(L, core().seconds());
    return 1;
}
int l_log(lua_State *L) {
    Script &s = live(L);
    fprintf(stderr, "mods: %s: %s\n", s.mod.c_str(), luaL_checkstring(L, 1));
    return 0;
}
int l_notice(lua_State *L) {
    Script &s = live(L);
    say(s, luaL_checkstring(L, 1));
    return 0;
}
int byte_of(lua_State *L, int i) {
    const lua_Integer v = luaL_checkinteger(L, i);
    luaL_argcheck(L, v >= 0 && v <= 255, i, "0 to 255");
    return (int)v;
}
// hk.has(name): this core and build have that capability (mods::capabilities(): what `requires` may list)
int l_has(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    const std::vector<std::string> have = mods::capabilities();
    lua_pushboolean(L, std::find(have.begin(), have.end(), name) != have.end());
    return 1;
}
int l_rgb(lua_State *L) {
    const int r = byte_of(L, 1), g = byte_of(L, 2), b = byte_of(L, 3), a = lua_isnoneornil(L, 4) ? 255 : byte_of(L, 4);
    lua_pushinteger(L, (lua_Integer)((uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b));
    return 1;
}

// a hook's registers: regs.r3, regs.lr = ... (the core's names)
int regs_index(lua_State *L) {
    Script &s = live(L);
    const char *name = luaL_checkstring(L, 2);
    if (!s.hook_regs) return luaL_error(L, "the registers are a hook's, while its call runs");
    auto it = s.regs.find(name);
    if (it == s.regs.end()) return luaL_error(L, "no register '%s' on this machine", name);
    lua_pushinteger(L, (lua_Integer)s.hook_regs[it->second]);
    return 1;
}
int regs_newindex(lua_State *L) {
    Script &s = live(L);
    const char *name = luaL_checkstring(L, 2);
    if (!s.hook_regs) return luaL_error(L, "the registers are a hook's, while its call runs");
    auto it = s.regs.find(name);
    if (it == s.regs.end()) return luaL_error(L, "no register '%s' on this machine", name);
    s.hook_regs[it->second] = (uint32_t)luaL_checkinteger(L, 3);
    return 0;
}

// ---- drawing ------------------------------------------------------------------------------------

int coord(lua_State *L, int i) {
    const lua_Number v = luaL_checknumber(L, i);
    return (int)std::clamp<lua_Number>(std::floor(v), -1e6, 1e6);
}
uint32_t colour(lua_State *L, int i) {
    const lua_Integer c = luaL_checkinteger(L, i);
    luaL_argcheck(L, c >= 0 && c <= 0xffffffffll, i, "a colour: 0xRRGGBB, 0xAARRGGBB or hk.rgb(...)");
    const uint32_t v = (uint32_t)c;
    return v <= 0xffffff ? 0xff000000u | v : v;   // (no alpha given: opaque)
}
// the layer a drawing method is for (its upvalue), and its first argument past the object
std::vector<Piece> &layer(lua_State *L, int &base) {
    Script &s = live(L);
    if (!s.drawing) luaL_error(L, "drawing is for draw_game and draw_window");
    base = lua_istable(L, 1) ? 2 : 1;   // (g:rect(...) or g.rect(...))
    std::vector<Piece> &out = lua_tointeger(L, lua_upvalueindex(1)) ? g_window : g_game;
    if (out.size() >= kPieces) luaL_error(L, "more than %d drawings a frame", (int)kPieces);
    return out;
}
void rect_piece(std::vector<Piece> &out, int x, int y, int w, int h, uint32_t c) {
    if (w <= 0 || h <= 0 || !(c >> 24)) return;
    Piece p;
    p.x = x, p.y = y, p.dw = w, p.dh = h, p.w = p.h = 1;
    p.argb.assign(1, c);
    out.push_back(std::move(p));
}
int d_rect(lua_State *L) {
    int b;
    std::vector<Piece> &out = layer(L, b);
    rect_piece(out, coord(L, b), coord(L, b + 1), coord(L, b + 2), coord(L, b + 3), colour(L, b + 4));
    return 0;
}
int d_box(lua_State *L) {
    int b;
    std::vector<Piece> &out = layer(L, b);
    const int x = coord(L, b), y = coord(L, b + 1), w = coord(L, b + 2), h = coord(L, b + 3);
    const uint32_t c = colour(L, b + 4);
    const int t = std::clamp((int)luaL_optinteger(L, b + 5, 1), 1, 64);
    if (w <= 0 || h <= 0) return 0;
    if (w <= 2 * t || h <= 2 * t) {
        rect_piece(out, x, y, w, h, c);
        return 0;
    }
    rect_piece(out, x, y, w, t, c);
    rect_piece(out, x, y + h - t, w, t, c);
    rect_piece(out, x, y + t, t, h - 2 * t, c);
    rect_piece(out, x + w - t, y + t, t, h - 2 * t, c);
    return 0;
}
int d_line(lua_State *L) {
    int b;
    std::vector<Piece> &out = layer(L, b);
    int x0 = coord(L, b), y0 = coord(L, b + 1), x1 = coord(L, b + 2), y1 = coord(L, b + 3);
    const uint32_t c = colour(L, b + 4);
    const int t = std::clamp((int)luaL_optinteger(L, b + 5, 1), 1, 64);
    if (y0 == y1 || x0 == x1) {   // across or down: a rectangle
        rect_piece(out, std::min(x0, x1), std::min(y0, y1), std::abs(x1 - x0) + t, std::abs(y1 - y0) + t, c);
        return 0;
    }
    Piece p;
    p.x = std::min(x0, x1), p.y = std::min(y0, y1);
    p.w = p.dw = std::abs(x1 - x0) + t, p.h = p.dh = std::abs(y1 - y0) + t;
    if ((size_t)p.w * (size_t)p.h > kPiecePixels) return luaL_error(L, "a line too long to draw");
    p.argb.assign((size_t)p.w * p.h, 0);
    const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    for (int err = dx + dy;;) {   // Bresenham's, a t x t square at each point
        for (int j = 0; j < t; j++)
            for (int i = 0; i < t; i++) p.argb[(size_t)(y0 - p.y + j) * p.w + (x0 - p.x + i)] = c;
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) err += dy, x0 += sx;
        if (e2 <= dx) err += dx, y0 += sy;
    }
    out.push_back(std::move(p));
    return 0;
}
// a text's characters as the 8x8 font has them: ASCII, '?' for anything else (a UTF-8 sequence one)
std::string glyphs_of(const char *t, size_t n) {
    std::string out;
    for (size_t i = 0; i < n && out.size() < 256; i++) {
        const unsigned char c = (unsigned char)t[i];
        if (c >= 0x80 && c < 0xc0) continue;   // (the rest of a character)
        out += c >= 32 && c < 127 ? (char)c : '?';
    }
    return out;
}
int d_text(lua_State *L) {
    int b;
    std::vector<Piece> &out = layer(L, b);
    const int x = coord(L, b), y = coord(L, b + 1);
    size_t n;
    const char *t = luaL_checklstring(L, b + 2, &n);
    const uint32_t c = colour(L, b + 3);
    const int k = std::clamp((int)luaL_optinteger(L, b + 4, 1), 1, 16);
    const std::string g = glyphs_of(t, n);
    lua_pushinteger(L, (lua_Integer)(g.size() * 8 * k));
    if (g.empty() || !g_font || !(c >> 24)) return 1;
    Piece p;
    p.x = x, p.y = y, p.w = p.dw = (int)g.size() * 8 * k, p.h = p.dh = 8 * k;
    p.argb.assign((size_t)p.w * p.h, 0);
    for (size_t i = 0; i < g.size(); i++) {
        const uint8_t *rows = g_font[(unsigned char)g[i] - 32];
        for (int gy = 0; gy < 8; gy++)
            for (int gx = 0; gx < 8; gx++)
                if (rows[gy] >> gx & 1)
                    for (int j = 0; j < k; j++)
                        for (int q = 0; q < k; q++) p.argb[(size_t)(gy * k + j) * p.w + (int)i * 8 * k + gx * k + q] = c;
    }
    out.push_back(std::move(p));
    return 1;
}
int d_text_width(lua_State *L) {
    const int b = lua_istable(L, 1) ? 2 : 1;
    size_t n;
    const char *t = luaL_checklstring(L, b, &n);
    const int k = std::clamp((int)luaL_optinteger(L, b + 1, 1), 1, 16);
    lua_pushinteger(L, (lua_Integer)(glyphs_of(t, n).size() * 8 * k));
    return 1;
}

// the object a draw callback gets: its methods for `which` layer (0 game, 1 window)
int make_canvas(lua_State *L, int which) {
    lua_newtable(L);
    const luaL_Reg methods[] = {{"rect", d_rect}, {"box", d_box}, {"line", d_line}, {"text", d_text}, {nullptr, nullptr}};
    for (const luaL_Reg *m = methods; m->name; m++) {
        lua_pushinteger(L, which);
        lua_pushcclosure(L, m->func, 1);
        lua_setfield(L, -2, m->name);
    }
    lua_pushcfunction(L, d_text_width);
    lua_setfield(L, -2, "text_width");
    return luaL_ref(L, LUA_REGISTRYINDEX);
}
void set_int(lua_State *L, const char *k, int v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, k);
}

// The sandbox and hk, as the script's file first runs (in a protected call)
int open(lua_State *L) {
    Script &s = live(L);
    const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base}, {LUA_STRLIBNAME, luaopen_string}, {LUA_TABLIBNAME, luaopen_table},
                             {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8}};
    for (const luaL_Reg &l : libs) {
        luaL_requiref(L, l.name, l.func, 1);
        lua_pop(L, 1);
    }
    for (const char *gone : {"dofile", "loadfile", "require"}) {
        lua_pushnil(L);
        lua_setglobal(L, gone);
    }
    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");
    lua_pushcfunction(L, l_collectgarbage);
    lua_setglobal(L, "collectgarbage");
    lua_getglobal(L, "load");
    lua_pushcclosure(L, l_load, 1);
    lua_setglobal(L, "load");
    // math.random from a fixed seed; math.randomseed only with one
    lua_getglobal(L, "math");
    lua_getfield(L, -1, "randomseed");
    lua_pushvalue(L, -1);
    lua_pushinteger(L, 0x484b);
    lua_call(L, 1, 0);
    lua_pushcclosure(L, l_randomseed, 1);
    lua_setfield(L, -2, "randomseed");
    lua_pop(L, 1);
    // string.dump's chunks can't be loaded here: gone with them
    lua_getglobal(L, "string");
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
    lua_pop(L, 1);

    lua_newtable(L);
    const luaL_Reg hk[] = {{"read8", l_read8},   {"read16", l_read16},   {"read32", l_read32},           {"write8", l_write8},
                           {"write16", l_write16}, {"write32", l_write32}, {"setting", l_setting},     {"set_setting", l_set_setting},
                           {"counter", l_counter}, {"frame", l_frame},     {"time", l_time},           {"log", l_log},
                           {"notice", l_notice},   {"rgb", l_rgb},         {"has", l_has},             {nullptr, nullptr}};
    luaL_setfuncs(L, hk, 0);
    lua_pushinteger(L, mods::API);
    lua_setfield(L, -2, "api");
    lua_setglobal(L, "hk");

    // a hook's registers: one object, its registers those of the call running
    lua_newuserdatauv(L, 1, 0);
    lua_newtable(L);
    lua_pushcfunction(L, regs_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, regs_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_pushliteral(L, "registers");
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
    s.regs_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    s.game_ref = make_canvas(L, 0);
    s.window_ref = make_canvas(L, 1);

    // the file, as text
    const std::string *text = static_cast<const std::string *>(lua_touserdata(L, 1));
    if (luaL_loadbufferx(L, text->data(), text->size(), ("@" + s.chunk).c_str(), "t") != LUA_OK) return lua_error(L);
    lua_call(L, 0, 0);
    return 0;
}

int global_function(lua_State *L, const char *name) {
    lua_getglobal(L, name);
    if (lua_type(L, -1) != LUA_TFUNCTION) {
        lua_pop(L, 1);
        return 0;
    }
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

void blend(uint32_t &d, uint32_t s) {
    const uint32_t a = s >> 24;
    if (a == 255) {
        d = (d & 0xff000000u) | (s & 0xffffff);
        return;
    }
    if (!a) return;
    uint32_t o = d & 0xff000000u;
    for (int sh = 0; sh < 24; sh += 8) {
        const uint32_t sc = s >> sh & 0xff, dc = d >> sh & 0xff;
        o |= ((sc * a + dc * (255 - a) + 127) / 255) << sh;
    }
    d = o;
}

Script::~Script() { close_state(*this); }

}  // namespace

int load(const std::string &mod, const std::string &folder, const std::string &file, const std::vector<std::string> &needs,
         std::string &err) {
    std::string text;
    {
        std::ifstream f(std::filesystem::path(std::u8string(file.begin(), file.end())), std::ios::binary);
        if (!f) {
            err = file + ": can't be read";
            return -1;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    }
    Script &s = g_scripts.emplace_back();
    s.mod = mod, s.folder = folder, s.file = file;
    {
        const size_t slash = file.find_last_of("/\\");
        s.chunk = folder + "/" + (slash == std::string::npos ? file : file.substr(slash + 1));
    }
    auto has = [&](const char *c) { return std::find(needs.begin(), needs.end(), c) != needs.end(); };
    s.memory = has("memory");
    s.big = core().mod_big_endian();
    const std::vector<std::string> regs = core().mod_registers();
    for (size_t i = 0; i < regs.size(); i++) s.regs[regs[i]] = (int)i;
    s.L = lua_newstate(alloc, &s);
    if (!s.L) {
        g_scripts.pop_back();
        err = file + ": Lua couldn't start";
        return -1;
    }
    *static_cast<Script **>(lua_getextraspace(s.L)) = &s;
    lua_pushcfunction(s.L, open);
    lua_pushlightuserdata(s.L, (void *)&text);
    if (std::string why; !protected_call(s, 1, "the file, as it first runs", kLoadBudget, why)) {
        err = why.rfind(s.chunk, 0) == 0 ? why : s.chunk + ": " + why;   // (Lua's own messages name the file and line)
        close_state(s);
        g_scripts.pop_back();
        return -1;
    }
    lua_State *L = s.L;
    s.on_frame = global_function(L, "on_frame");
    s.on_input = global_function(L, "on_input");
    s.draw_game = global_function(L, "draw_game");
    s.draw_window = global_function(L, "draw_window");
    std::string refused;
    if (s.on_input && !has("input")) refused = "it has on_input: its requires lists \"input\" with it";
    else if ((s.draw_game || s.draw_window) && !has("draw")) refused = "it has draw_game or draw_window: its requires lists \"draw\" with it";
    if (!refused.empty()) {
        err = s.chunk + ": " + refused;
        close_state(s);
        g_scripts.pop_back();
        return -1;
    }
    if (s.on_input) g_wants_input = true;
    std::string has_fns;
    for (auto [ref, fn] : {std::pair{s.on_frame, "on_frame"}, {s.on_input, "on_input"}, {s.draw_game, "draw_game"}, {s.draw_window, "draw_window"}})
        if (ref) has_fns += (has_fns.empty() ? "" : ", ") + std::string(fn);
    fprintf(stderr, "mods: %s: script %s (%s), %zu KB\n", mod.c_str(), s.chunk.c_str(), has_fns.empty() ? "no callbacks" : has_fns.c_str(),
            s.used >> 10);
    return (int)g_scripts.size() - 1;
}

int function_ref(int id, const std::string &name) {
    if (id < 0 || (size_t)id >= g_scripts.size() || !g_scripts[(size_t)id].L) return 0;
    return global_function(g_scripts[(size_t)id].L, name.c_str());
}

void call_hook(int id, int ref, uint32_t *regs, uint32_t at) {
    Script &s = g_scripts[(size_t)id];
    if (!s.alive) return;
    lua_State *L = s.L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s.regs_ref);
    lua_pushinteger(L, (lua_Integer)at);
    s.hook_regs = regs;
    call(s, 2, "a hook's call", kHookBudget);
    s.hook_regs = nullptr;
}

void input(const std::string &name, int value) {
    if (!g_wants_input || (value != 0 && value != 1)) return;   // (buttons only)
    const double t = core().seconds();
    std::lock_guard<std::mutex> lock(g_input_lock);
    bool &was = g_down[name];
    if (was == (value != 0)) return;
    was = value != 0;
    if (g_events.size() >= 1024) {
        g_dropped++;
        return;
    }
    g_events.push_back({name, value != 0, t});
}

bool any() { return !g_scripts.empty(); }

void frame(uint64_t, int w, int h, bool draw) {
    if (g_scripts.empty()) return;
    std::vector<Event> events;
    if (g_wants_input) {
        std::lock_guard<std::mutex> lock(g_input_lock);
        events.swap(g_events);
    }
    for (Script &s : g_scripts) {
        for (const Event &e : events) {
            if (!s.alive || !s.on_input) break;
            lua_rawgeti(s.L, LUA_REGISTRYINDEX, s.on_input);
            lua_pushstring(s.L, e.name.c_str());
            lua_pushboolean(s.L, e.down);
            lua_pushnumber(s.L, e.time);
            call(s, 3, "on_input", kBudget);
        }
        if (s.alive && s.on_frame) {
            lua_rawgeti(s.L, LUA_REGISTRYINDEX, s.on_frame);
            call(s, 0, "on_frame", kBudget);
        }
    }
    g_game.clear();
    g_window.clear();
    if (!draw) return;
    for (Script &s : g_scripts) {
        s.drawing = true;
        if (s.alive && s.draw_game && w > 0 && h > 0) {
            lua_rawgeti(s.L, LUA_REGISTRYINDEX, s.draw_game);
            lua_rawgeti(s.L, LUA_REGISTRYINDEX, s.game_ref);
            set_int(s.L, "width", w);
            set_int(s.L, "height", h);
            call(s, 1, "draw_game", kBudget);
        }
        if (s.alive && s.draw_window && g_view[4] > 0 && g_view[5] > 0) {
            lua_rawgeti(s.L, LUA_REGISTRYINDEX, s.draw_window);
            lua_rawgeti(s.L, LUA_REGISTRYINDEX, s.window_ref);
            set_int(s.L, "width", g_view[4]);
            set_int(s.L, "height", g_view[5]);
            set_int(s.L, "picture_x", g_view[0]);
            set_int(s.L, "picture_y", g_view[1]);
            set_int(s.L, "picture_w", g_view[2]);
            set_int(s.L, "picture_h", g_view[3]);
            set_int(s.L, "unit", std::max(1, g_view[5] / 360));
            call(s, 1, "draw_window", kBudget);
        }
        s.drawing = false;
    }
}

void paint(const std::vector<Piece> &pieces, uint32_t *px, int cw, int ch) {
    for (const Piece &p : pieces) {
        const int x0 = std::max(0, p.x), y0 = std::max(0, p.y), x1 = std::min(cw, p.x + p.dw), y1 = std::min(ch, p.y + p.dh);
        if (p.w == 1 && p.h == 1) {
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) blend(px[(size_t)y * cw + x], p.argb[0]);
            continue;
        }
        for (int y = y0; y < y1; y++) {
            const int sy = (int)((int64_t)(y - p.y) * p.h / p.dh);
            for (int x = x0; x < x1; x++) blend(px[(size_t)y * cw + x], p.argb[(size_t)sy * p.w + (size_t)((int64_t)(x - p.x) * p.w / p.dw)]);
        }
    }
}

const uint32_t *compose(const uint32_t *rgb, int w, int h, std::vector<uint32_t> &out) {
    if (!rgb || g_game.empty() || w <= 0 || h <= 0) return rgb;
    out.assign(rgb, rgb + (size_t)w * h);
    paint(g_game, out.data(), w, h);
    return out.data();
}
const uint32_t *compose_frame(const uint32_t *rgb, int w, int h) { return compose(rgb, w, h, g_frame_copy); }
const uint32_t *compose_between(const uint32_t *rgb, int w, int h) { return compose(rgb, w, h, g_between_copy); }

const std::vector<Piece> &window_pieces() { return g_window; }
const std::vector<Piece> &game_pieces() { return g_game; }

void set_view(int x, int y, int w, int h, int ww, int wh) {
    g_view[0] = x, g_view[1] = y, g_view[2] = w, g_view[3] = h, g_view[4] = ww, g_view[5] = wh;
}

void set_font(const uint8_t (*glyphs)[8]) { g_font = glyphs; }
void set_notice(std::function<void(const std::vector<std::string> &)> fn) { g_notice = std::move(fn); }
void notice(const std::vector<std::string> &lines) {
    if (g_notice) g_notice(lines);
}

std::vector<std::string> report_lines() {
    std::vector<std::string> out;
    for (const Script &s : g_scripts) {
        // (instructions are counted kStep at a time, by the count hook, each call afresh: a call
        // under kStep never reaches a count, so what's known is "under kStep", else to kStep)
        char most[64];
        if (s.most <= 0) snprintf(most, sizeof most, "under %d", kStep);
        else snprintf(most, sizeof most, "%lld to %lld", (long long)s.most, (long long)(s.most + kStep - 1));
        char b[256];
        snprintf(b, sizeof b, "script: %llu calls, the most instructions in one %s, memory at most %zu KB", (unsigned long long)s.calls, most,
                 s.peak >> 10);
        out.push_back(s.mod + ": " + b + (s.alive ? "" : "; stopped: " + s.stopped));
    }
    return out;
}

void report() {
    for (const std::string &l : report_lines()) fprintf(stderr, "mods: %s\n", l.c_str());
    if (g_dropped) fprintf(stderr, "mods: %llu controls not given to on_input (over 1024 in a frame)\n", (unsigned long long)g_dropped);
}

std::vector<std::string> stopped() {
    std::vector<std::string> out;
    for (const Script &s : g_scripts)
        if (!s.alive) out.push_back(s.mod + ": " + s.stopped);
    return out;
}

void clear() {
    g_scripts.clear();   // (each closes its state)
    g_game.clear();
    g_window.clear();
    std::lock_guard<std::mutex> lock(g_input_lock);
    g_events.clear();
    g_down.clear();
    g_wants_input = false;
    g_dropped = 0;
}

}  // namespace hk::modscript
