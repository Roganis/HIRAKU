// SPDX-License-Identifier: GPL-3.0-or-later
#include "python/kbin.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace hk::py::kbin {

namespace {

// The value types by their number (kbinxml's format_ids): bytes a value, values an element
// (-1: a length first: str, bin), and how it reads as text
struct Type {
    const char *name;
    int size, count;
    char kind;   // i signed, u unsigned, f float, s str, b bin, p ip4, v void
};
constexpr Type kNone{nullptr, 0, 0, 0};

const Type &type_of(int id) {
    static const Type t[57] = {
        kNone,
        {"void", 0, 0, 'v'}, {"s8", 1, 1, 'i'}, {"u8", 1, 1, 'u'}, {"s16", 2, 1, 'i'}, {"u16", 2, 1, 'u'},
        {"s32", 4, 1, 'i'}, {"u32", 4, 1, 'u'}, {"s64", 8, 1, 'i'}, {"u64", 8, 1, 'u'}, {"bin", 1, -1, 'b'},
        {"str", 1, -1, 's'}, {"ip4", 4, 1, 'p'}, {"time", 4, 1, 'u'}, {"float", 4, 1, 'f'}, {"double", 8, 1, 'f'},
        {"2s8", 1, 2, 'i'}, {"2u8", 1, 2, 'u'}, {"2s16", 2, 2, 'i'}, {"2u16", 2, 2, 'u'}, {"2s32", 4, 2, 'i'},
        {"2u32", 4, 2, 'u'}, {"2s64", 8, 2, 'i'}, {"2u64", 8, 2, 'u'}, {"2f", 4, 2, 'f'}, {"2d", 8, 2, 'f'},
        {"3s8", 1, 3, 'i'}, {"3u8", 1, 3, 'u'}, {"3s16", 2, 3, 'i'}, {"3u16", 2, 3, 'u'}, {"3s32", 4, 3, 'i'},
        {"3u32", 4, 3, 'u'}, {"3s64", 8, 3, 'i'}, {"3u64", 8, 3, 'u'}, {"3f", 4, 3, 'f'}, {"3d", 8, 3, 'f'},
        {"4s8", 1, 4, 'i'}, {"4u8", 1, 4, 'u'}, {"4s16", 2, 4, 'i'}, {"4u16", 2, 4, 'u'}, {"4s32", 4, 4, 'i'},
        {"4u32", 4, 4, 'u'}, {"4s64", 8, 4, 'i'}, {"4u64", 8, 4, 'u'}, {"4f", 4, 4, 'f'}, {"4d", 8, 4, 'f'},
        kNone,   // 46: an attribute
        kNone,   // 47
        {"vs8", 1, 16, 'i'}, {"vu8", 1, 16, 'u'}, {"vs16", 2, 8, 'i'}, {"vu16", 2, 8, 'u'}, {"bool", 1, 1, 'i'},
        {"2b", 1, 2, 'i'}, {"3b", 1, 3, 'i'}, {"4b", 1, 4, 'i'}, {"vb", 1, 16, 'i'},
    };
    return id >= 0 && id < 57 ? t[id] : kNone;
}

int type_id(const std::string &name) {
    static const std::pair<const char *, int> other[] = {{"binary", 10}, {"string", 11}, {"f", 14}, {"d", 15}, {"vs64", 22},
                                                         {"vu64", 23}, {"vd", 25}, {"vs32", 40}, {"vu32", 41}, {"vf", 44}, {"b", 52}};
    for (int i = 1; i < 57; i++)
        if (type_of(i).name && name == type_of(i).name) return i;
    for (const auto &[n, id] : other)
        if (name == n) return id;
    return 0;
}

constexpr int kAttr = 46, kEnd = 190, kEndAll = 191, kArray = 64;
// Limits on what is read (a call comes from the network): elements inside elements, and the values
// an array's __count asks for. The games' own calls are a handful deep, their arrays a few thousand
constexpr int kMaxDepth = 64, kMaxCount = 1 << 20;
const char kSix[] = "0123456789:ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz";

uint64_t be(const uint8_t *p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = v << 8 | p[i];
    return v;
}

std::string escaped(const std::string &s) {
    std::string o;
    for (char ch : s) {
        switch (ch) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        default: o += ch;
        }
    }
    return o;
}

struct Node {
    std::string name, type, text, count;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<std::unique_ptr<Node>> kids;
    Node *up = nullptr;
};

void write_text(const Node &n, std::string &out) {
    out += "<" + n.name;
    for (const auto &[k, v] : n.attrs) out += " " + k + "=\"" + escaped(v) + "\"";
    if (!n.type.empty() && n.type != "void") out += " __type=\"" + n.type + "\"";
    if (!n.count.empty()) out += " __count=\"" + n.count + "\"";
    if (n.text.empty() && n.kids.empty()) {
        out += "/>";
        return;
    }
    out += ">" + escaped(n.text);
    for (const auto &k : n.kids) write_text(*k, out);
    out += "</" + n.name + ">";
}

// One value of a type, from big-endian bytes, as text
std::string value_text(const Type &t, const uint8_t *p) {
    char b[64];
    const uint64_t u = be(p, t.size);
    if (t.kind == 'p') snprintf(b, sizeof b, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    else if (t.kind == 'u') snprintf(b, sizeof b, "%llu", (unsigned long long)u);
    else if (t.kind == 'i') {
        int64_t v = (int64_t)(u << (64 - 8 * t.size)) >> (64 - 8 * t.size);
        snprintf(b, sizeof b, "%lld", (long long)v);
    } else if (t.size == 4) {
        const uint32_t w = (uint32_t)u;
        float f;
        memcpy(&f, &w, 4);
        snprintf(b, sizeof b, "%f", (double)f);
    } else {
        double d;
        memcpy(&d, &u, 8);
        snprintf(b, sizeof b, "%f", d);
    }
    return b;
}

}  // namespace

bool is_binary(const std::string &b) {
    return b.size() >= 2 && (uint8_t)b[0] == 0xa0 && ((uint8_t)b[1] == 0x42 || (uint8_t)b[1] == 0x45);
}

bool to_text(const std::string &bin, std::string &xml) {
    const uint8_t *p = (const uint8_t *)bin.data();
    const size_t n = bin.size();
    if (n < 12 || !is_binary(bin) || (p[3] ^ p[2]) != 0xff) return false;
    const bool six = p[1] == 0x42;
    const size_t node_end = 8 + (size_t)be(p + 4, 4);
    if (node_end + 4 > n) return false;
    const size_t data = node_end + 4, data_len = (size_t)be(p + node_end, 4);
    if (data + data_len > n) return false;
    size_t off = 0, byte_off = 0, word_off = 0;
    auto align = [](size_t &o) { o = (o + 3) & ~(size_t)3; };
    auto in_data = [&](size_t at, size_t len) { return at + len <= data_len; };
    auto sized = [&](std::string &out) {   // a u32 length, then that many bytes
        if (!in_data(off, 4)) return false;
        const size_t len = (size_t)be(p + data + off, 4);
        off += 4;
        if (!in_data(off, len)) return false;
        out.assign((const char *)p + data + off, len);
        off += len;
        align(off);
        return true;
    };
    auto aligned = [&](size_t sz) -> long {   // where a value of sz bytes is (kbin packs small ones)
        if (byte_off % 4 == 0) byte_off = off;
        if (word_off % 4 == 0) word_off = off;
        size_t at;
        if (sz == 1) at = byte_off, byte_off += 1;
        else if (sz == 2) at = word_off, word_off += 2;
        else at = off, off += sz, align(off);
        const size_t trailing = std::max(byte_off, word_off);
        if (off < trailing) off = trailing, align(off);
        return in_data(at, sz) ? (long)at : -1;
    };
    Node root;
    Node *cur = &root;
    int depth = 0;
    size_t i = 8;
    while (i < node_end) {
        if (p[i] == 0) {
            i++;
            continue;
        }
        int t = p[i++];
        const bool arr = t & kArray;
        t &= ~kArray;
        if (t == kEnd) {
            if (cur->up) cur = cur->up, depth--;
            continue;
        }
        if (t == kEndAll) break;
        std::string name;
        if (i >= node_end) return false;
        if (six) {
            const size_t len = p[i++], bytes = (len * 6 + 7) / 8;
            if (i + bytes > node_end) return false;
            for (size_t k = 0; k < len; k++) {   // six bits a letter, the first bit highest
                unsigned v = 0;
                for (size_t b = k * 6; b < k * 6 + 6; b++) v = v << 1 | (p[i + b / 8] >> (7 - b % 8) & 1);
                name += kSix[v];
            }
            i += bytes;
        } else {
            const size_t len = (p[i++] & ~kArray) + 1;
            if (i + len > node_end) return false;
            name.assign((const char *)p + i, len);
            i += len;
        }
        if (t == kAttr) {
            std::string v;
            if (!sized(v)) return false;
            while (!v.empty() && v.back() == 0) v.pop_back();
            cur->attrs.emplace_back(name, v);
            continue;
        }
        const Type &ty = type_of(t);
        if (!ty.name || ++depth > kMaxDepth) return false;
        auto node = std::make_unique<Node>();
        node->name = name;
        node->type = ty.name;
        node->up = cur;
        Node *nn = node.get();
        cur->kids.push_back(std::move(node));
        cur = nn;
        if (ty.kind == 'v') continue;
        if (ty.count < 0) {   // str, bin
            std::string v;
            if (!sized(v)) return false;
            if (ty.kind == 's') {
                while (!v.empty() && v.back() == 0) v.pop_back();
                nn->text = v;
            } else {
                static const char *hx = "0123456789abcdef";
                for (unsigned char ch : v) nn->text += hx[ch >> 4], nn->text += hx[ch & 15];
            }
            continue;
        }
        size_t total = (size_t)ty.count, at;
        if (arr) {
            if (!in_data(off, 4)) return false;
            const size_t len = (size_t)be(p + data + off, 4);
            off += 4;
            at = off;
            if (!in_data(at, len)) return false;
            total = len / (size_t)ty.size;
            off += len;
            align(off);
            nn->count = std::to_string(total / (size_t)ty.count);
        } else {
            const long a = aligned((size_t)ty.size * total);
            if (a < 0) return false;
            at = (size_t)a;
        }
        for (size_t k = 0; k < total; k++) nn->text += (k ? " " : "") + value_text(ty, p + data + at + k * (size_t)ty.size);
    }
    if (root.kids.empty()) return false;
    xml = "<?xml version=\"1.0\"?>";
    write_text(*root.kids[0], xml);
    return true;
}

// ---- text to kbin

namespace {

std::string unescaped(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') {
            o += s[i];
            continue;
        }
        static const std::pair<const char *, char> ents[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
        bool done = false;
        for (const auto &[e, ch] : ents)
            if (s.compare(i, strlen(e), e) == 0) {
                o += ch, i += strlen(e) - 1, done = true;
                break;
            }
        if (!done) o += '&';
    }
    return o;
}

// The XML the server writes: elements, attributes in double or single quotes, text, <?..?> and comments
bool parse_xml(const std::string &x, size_t &i, Node &n, int depth = 0) {
    if (depth > kMaxDepth) return false;
    auto skip = [&] {
        for (;;) {
            while (i < x.size() && std::isspace((unsigned char)x[i])) i++;
            if (x.compare(i, 2, "<?") == 0) i = x.find("?>", i) == std::string::npos ? x.size() : x.find("?>", i) + 2;
            else if (x.compare(i, 4, "<!--") == 0) i = x.find("-->", i) == std::string::npos ? x.size() : x.find("-->", i) + 3;
            else break;
        }
    };
    skip();
    if (i >= x.size() || x[i] != '<') return false;
    size_t e = ++i;
    while (e < x.size() && !std::isspace((unsigned char)x[e]) && x[e] != '/' && x[e] != '>') e++;
    n.name = x.substr(i, e - i);
    i = e;
    for (;;) {
        while (i < x.size() && std::isspace((unsigned char)x[i])) i++;
        if (i >= x.size()) return false;
        if (x[i] == '/') {
            i = x.find('>', i);
            if (i == std::string::npos) return false;
            i++;
            return true;
        }
        if (x[i] == '>') {
            i++;
            break;
        }
        const size_t eq = x.find('=', i);
        if (eq == std::string::npos || eq + 1 >= x.size()) return false;
        std::string k = x.substr(i, eq - i);
        while (!k.empty() && std::isspace((unsigned char)k.back())) k.pop_back();
        const char q = x[eq + 1];
        const size_t end = x.find(q, eq + 2);
        if ((q != '"' && q != '\'') || end == std::string::npos) return false;
        const std::string v = unescaped(x.substr(eq + 2, end - eq - 2));
        if (k == "__type") n.type = v;
        else if (k == "__count") n.count = v;
        else if (k != "__size") n.attrs.emplace_back(k, v);
        i = end + 1;
    }
    for (;;) {   // text and children, to the end tag
        const size_t lt = x.find('<', i);
        if (lt == std::string::npos) return false;
        n.text += unescaped(x.substr(i, lt - i));
        i = lt;
        if (x.compare(i, 2, "</") == 0) {
            i = x.find('>', i);
            if (i == std::string::npos) return false;
            i++;
            return true;
        }
        if (x.compare(i, 4, "<!--") == 0) {
            const size_t c = x.find("-->", i);
            i = c == std::string::npos ? x.size() : c + 3;
            continue;
        }
        auto kid = std::make_unique<Node>();
        if (!parse_xml(x, i, *kid, depth + 1)) return false;
        n.kids.push_back(std::move(kid));
    }
}

struct Writer {
    std::string nodes, data;
    size_t byte_off = 0, word_off = 0;
    bool six = true;

    void pad(std::string &s) {
        while (s.size() % 4) s += '\0';
    }
    void u32(std::string &s, uint32_t v) {
        for (int k = 3; k >= 0; k--) s += (char)(v >> (8 * k));
    }
    void name(const std::string &nm) {
        if (!six) {
            nodes += (char)((nm.size() - 1) | kArray);
            nodes += nm;
            return;
        }
        nodes += (char)nm.size();
        std::vector<uint8_t> bits((nm.size() * 6 + 7) / 8, 0);
        for (size_t k = 0; k < nm.size(); k++) {
            const unsigned v = (unsigned)(strchr(kSix, nm[k]) - kSix);
            for (int b = 0; b < 6; b++)
                if (v >> (5 - b) & 1) bits[(k * 6 + (size_t)b) / 8] |= (uint8_t)(0x80 >> ((k * 6 + (size_t)b) % 8));
        }
        nodes.append(bits.begin(), bits.end());
    }
    void sized(const std::string &v) {
        u32(data, (uint32_t)v.size());
        data += v;
        pad(data);
    }
    void aligned(const std::string &v) {
        if (byte_off % 4 == 0) byte_off = data.size();
        if (word_off % 4 == 0) word_off = data.size();
        if (v.size() == 1) {
            if (byte_off % 4 == 0) data.append(4, '\0');
            data[byte_off++] = v[0];
        } else if (v.size() == 2) {
            if (word_off % 4 == 0) data.append(4, '\0');
            data[word_off] = v[0], data[word_off + 1] = v[1];
            word_off += 2;
        } else {
            data += v;
            pad(data);
        }
    }
    bool node(const Node &n) {
        std::string tn = n.type;
        std::string text = n.text;
        if (tn.empty()) {   // no __type: a str with text, else void
            bool blank = std::all_of(text.begin(), text.end(), [](char ch) { return std::isspace((unsigned char)ch); });
            tn = blank ? "void" : "str";
        }
        const int id = type_id(tn);
        if (!id) return false;
        const Type &ty = type_of(id);
        const bool arr = !n.count.empty();
        nodes += (char)(id | (arr ? kArray : 0));
        name(n.name);
        if (ty.kind == 's') {
            sized(text + '\0');
        } else if (ty.kind == 'b') {
            std::string v;
            std::string h;
            for (char ch : text)
                if (std::isxdigit((unsigned char)ch)) h += ch;
            for (size_t k = 0; k + 1 < h.size(); k += 2) v += (char)std::strtoul(h.substr(k, 2).c_str(), nullptr, 16);
            sized(v);
        } else if (ty.kind != 'v') {
            std::string v;
            size_t at = 0;
            int values = 0;
            while (at < text.size()) {
                while (at < text.size() && std::isspace((unsigned char)text[at])) at++;
                if (at >= text.size()) break;
                size_t e = at;
                while (e < text.size() && !std::isspace((unsigned char)text[e])) e++;
                const std::string one = text.substr(at, e - at);
                at = e;
                uint64_t u = 0;
                if (ty.kind == 'p') {
                    unsigned a = 0, b = 0, c = 0, d = 0;
                    sscanf(one.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d);
                    u = (uint64_t)(a << 24 | b << 16 | c << 8 | d);
                } else if (ty.kind == 'f' && ty.size == 4) {
                    const float f = std::strtof(one.c_str(), nullptr);
                    uint32_t w;
                    memcpy(&w, &f, 4);
                    u = w;
                } else if (ty.kind == 'f') {
                    const double d = std::strtod(one.c_str(), nullptr);
                    memcpy(&u, &d, 8);
                } else if (ty.kind == 'i') {
                    u = (uint64_t)std::strtoll(one.c_str(), nullptr, 10);
                } else {
                    u = std::strtoull(one.c_str(), nullptr, 10);
                }
                for (int k = ty.size - 1; k >= 0; k--) v += (char)(u >> (8 * k));
                values++;
            }
            const int want = arr ? std::clamp(std::atoi(n.count.c_str()), 0, kMaxCount / ty.count) * ty.count : ty.count;
            while (values < want) {   // missing values: zeros
                v.append((size_t)ty.size, '\0');
                values++;
            }
            if (arr) sized(v);
            else aligned(v.substr(0, (size_t)(ty.size * ty.count)));
        }
        std::vector<std::pair<std::string, std::string>> attrs = n.attrs;
        std::sort(attrs.begin(), attrs.end());
        for (const auto &[k, v] : attrs) {
            sized(v + '\0');
            nodes += (char)kAttr;
            name(k);
        }
        for (const auto &k : n.kids)
            if (!node(*k)) return false;
        nodes += (char)(kEnd | kArray);
        return true;
    }
};

bool sixable(const Node &n) {
    auto ok = [](const std::string &s) {
        return !s.empty() && s.size() < 64 && std::all_of(s.begin(), s.end(), [](char ch) { return ch && strchr(kSix, ch); });
    };
    if (!ok(n.name)) return false;
    for (const auto &[k, v] : n.attrs)
        if (!ok(k)) return false;
    for (const auto &k : n.kids)
        if (!sixable(*k)) return false;
    return true;
}

}  // namespace

bool from_text(const std::string &xml, std::string &bin) {
    Node root;
    size_t i = 0;
    if (!parse_xml(xml, i, root)) return false;
    Writer w;
    w.six = sixable(root);
    if (!w.node(root)) return false;
    w.nodes += (char)(kEndAll | kArray);
    w.pad(w.nodes);
    bin.clear();
    bin += (char)0xa0;
    bin += (char)(w.six ? 0x42 : 0x45);
    bin += (char)0x80, bin += (char)0x7f;   // Shift-JIS
    w.u32(bin, (uint32_t)w.nodes.size());
    bin += w.nodes;
    w.u32(bin, (uint32_t)w.data.size());
    bin += w.data;
    return true;
}

std::string lz77_unpack(const std::string &in) {
    std::string out;
    size_t i = 0;
    while (i < in.size()) {
        const uint8_t flags = (uint8_t)in[i++];
        for (int bit = 0; bit < 8; bit++) {
            if (i >= in.size()) return out;
            if (flags >> bit & 1) {
                out += in[i++];
                continue;
            }
            if (i + 1 >= in.size()) return out;
            const unsigned w = (unsigned)(uint8_t)in[i] << 8 | (uint8_t)in[i + 1];
            i += 2;
            const size_t back = w >> 4, len = (w & 15) + 3;
            if (!back) return out;
            for (size_t k = 0; k < len; k++) out += back <= out.size() ? out[out.size() - back] : '\0';
        }
    }
    return out;
}

}  // namespace hk::py::kbin
