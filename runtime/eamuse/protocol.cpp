// SPDX-License-Identifier: GPL-3.0-or-later
// e-AMUSE3: the pieces every game shares (protocol.h).
#include "eamuse/protocol.h"
#include "shell/sys.h"

#include <sys/stat.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>

#ifdef _MSC_VER
#define strncasecmp _strnicmp   // MSVC's name for it (the Python core builds with clang-cl)
#endif

namespace hk::ea {

namespace {

// The protocol's shared secret (as in bemaniutils' protocol code)
constexpr uint8_t SECRET[26] = {0x69, 0xd7, 0x46, 0x27, 0xd9, 0x85, 0xee, 0x21, 0x87, 0x16, 0x15, 0x70, 0xd0,
                                0x8d, 0x93, 0xb1, 0x24, 0x55, 0x03, 0x5b, 0x6d, 0xf0, 0xd8, 0x20, 0x5d, 0xf5};

}  // namespace

// name="value" pairs from the start of an element (after its name), to its '>'
size_t read_attrs(const std::string &x, size_t at, std::map<std::string, std::string> &out, bool *self_closing) {
    while (at < x.size()) {
        while (at < x.size() && std::isspace((unsigned char)x[at])) at++;
        if (at >= x.size()) break;
        if (x[at] == '/' || x[at] == '>') {
            *self_closing = x[at] == '/';
            const size_t e = x.find('>', at);
            return e == std::string::npos ? x.size() : e + 1;
        }
        const size_t eq = x.find('=', at);
        if (eq == std::string::npos || eq + 1 >= x.size()) break;
        size_t v = eq + 1;   // GuitarFreaks and DrumMania write some as name = "value"
        while (v < x.size() && std::isspace((unsigned char)x[v])) v++;
        if (v >= x.size()) break;
        const char q = x[v];
        const size_t end = x.find(q, v + 1);
        if (end == std::string::npos) break;
        std::string name = x.substr(at, eq - at);
        while (!name.empty() && std::isspace((unsigned char)name.back())) name.pop_back();
        out[name] = x.substr(v + 1, end - v - 1);
        at = end + 1;
    }
    return x.size();
}

// MD5 (RFC 1321)
std::vector<uint8_t> md5(const std::vector<uint8_t> &msg) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                              14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    std::vector<uint8_t> m = msg;
    const uint64_t bits = (uint64_t)msg.size() * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 0; i < 8; i++) m.push_back((uint8_t)(bits >> (8 * i)));
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)m[off + 4 * i] | (uint32_t)m[off + 4 * i + 1] << 8 | (uint32_t)m[off + 4 * i + 2] << 16 |
                   (uint32_t)m[off + 4 * i + 3] << 24;
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++) {
            uint32_t f;
            int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            const uint32_t t = d;
            d = c;
            c = b;
            const uint32_t x = a + f + K[i] + w[g];
            b = b + ((x << R[i]) | (x >> (32 - R[i])));
            a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    std::vector<uint8_t> out;
    for (uint32_t v : h)
        for (int i = 0; i < 4; i++) out.push_back((uint8_t)(v >> (8 * i)));
    return out;
}

std::string header(const std::string &head, const char *name) {
    const size_t n = std::strlen(name);
    for (size_t at = head.find("\r\n"); at != std::string::npos; at = head.find("\r\n", at + 2)) {
        if (head.size() - at - 2 < n + 1 || strncasecmp(head.c_str() + at + 2, name, n) != 0 || head[at + 2 + n] != ':') continue;
        size_t b = at + 3 + n;
        while (b < head.size() && head[b] == ' ') b++;
        const size_t e = head.find("\r\n", b);
        return head.substr(b, e == std::string::npos ? std::string::npos : e - b);
    }
    return "";
}

std::vector<uint8_t> key(const std::string &info) {
    // "1-xxxxxxxx-xxxx": the 6 bytes, then the secret
    std::vector<uint8_t> m;
    std::string hex;
    for (char c : info.substr(info.find('-') + 1))
        if (std::isxdigit((unsigned char)c)) hex += c;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) m.push_back((uint8_t)std::stoul(hex.substr(i, 2), nullptr, 16));
    m.insert(m.end(), SECRET, SECRET + sizeof SECRET);
    return md5(m);
}

void rc4(const std::vector<uint8_t> &k, std::string &d) {
    uint8_t s[256];
    for (int i = 0; i < 256; i++) s[i] = (uint8_t)i;
    for (int i = 0, j = 0; i < 256; i++) {
        j = (j + s[i] + k[i % k.size()]) & 255;
        std::swap(s[i], s[j]);
    }
    int i = 0, j = 0;
    for (char &c : d) {
        i = (i + 1) & 255;
        j = (j + s[i]) & 255;
        std::swap(s[i], s[j]);
        c = (char)((uint8_t)c ^ s[(s[i] + s[j]) & 255]);
    }
}

std::string clear(const std::string &m) {
    const size_t he = m.find("\r\n\r\n");
    if (he == std::string::npos) return m;
    const std::string head = m.substr(0, he), info = header(head, "X-Eamuse-Info");
    std::string body = m.substr(he + 4);
    if (!info.empty()) rc4(key(info), body);
    return head.substr(0, head.find("\r\n")) + (info.empty() ? " (plain): " : ": ") + body;
}

bool parse(const std::string &x, Call &c) {
    size_t at = x.find("<call");
    if (at == std::string::npos) return false;
    std::map<std::string, std::string> a;
    bool sc = false;
    at = read_attrs(x, at + 5, a, &sc);
    c.srcid = a["srcid"];
    c.model = a["model"];
    for (at = x.find('<', at); at != std::string::npos && at + 1 < x.size() && x[at + 1] != '/'; at = x.find('<', at)) {
        size_t e = at + 1;
        while (e < x.size() && !std::isspace((unsigned char)x[e]) && x[e] != '/' && x[e] != '>') e++;
        Request r;
        r.module = x.substr(at + 1, e - at - 1);
        sc = false;
        at = read_attrs(x, e, r.attrs, &sc);
        r.method = r.attrs["method"];
        if (!sc) {
            const size_t close = x.find("</" + r.module, at);
            r.inner = x.substr(at, close == std::string::npos ? std::string::npos : close - at);
            // (a close tag cut before its '>': the call ends there; + 1 on "none" would start it over)
            const size_t end = close == std::string::npos ? close : x.find('>', close);
            at = end == std::string::npos ? x.size() : end + 1;
        }
        c.requests.push_back(std::move(r));
    }
    return !c.requests.empty();
}

std::vector<Element> elements(const std::string &x) {
    std::vector<Element> out;
    for (size_t at = x.find('<'); at != std::string::npos && at + 1 < x.size(); at = x.find('<', at)) {
        if (x[at + 1] == '/' || x[at + 1] == '?' || x[at + 1] == '!') { at++; continue; }
        size_t e = at + 1;
        while (e < x.size() && !std::isspace((unsigned char)x[e]) && x[e] != '/' && x[e] != '>') e++;
        Element el;
        el.name = x.substr(at + 1, e - at - 1);
        bool sc = false;
        at = read_attrs(x, e, el.attrs, &sc);
        out.push_back(std::move(el));
    }
    return out;
}

std::string esc(const std::string &v) {
    std::string o;
    for (char ch : v) {
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

std::string attr(const std::map<std::string, std::string> &m, const char *k) {
    const auto it = m.find(k);
    return it == m.end() ? "" : it->second;
}

long num(const std::map<std::string, std::string> &m, const char *k, int base) {
    return std::strtol(attr(m, k).c_str(), nullptr, base);
}

bool safe_name(const std::string &s) {
    if (s.empty() || s.size() > 40) return false;
    for (char ch : s)
        if (!std::isalnum((unsigned char)ch) && ch != '-' && ch != '_') return false;
    return true;
}

bool safe_file(const std::string &s) {
    if (s.empty() || s.size() > 120 || s[0] == '.') return false;
    for (char ch : s)
        if (!std::isalnum((unsigned char)ch) && ch != '-' && ch != '_' && ch != '.') return false;
    return true;
}

std::string to_line(const Record &r) {
    std::string l;
    for (const auto &[k, v] : r) {
        std::string clean = v;
        for (char &ch : clean)
            if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
        l += (l.empty() ? "" : "\t") + k + "=" + clean;
    }
    return l + "\n";
}

std::vector<Record> from_lines(const std::string &text) {
    std::vector<Record> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t e = text.find('\n', at);
        if (e == std::string::npos) e = text.size();
        const std::string line = text.substr(at, e - at);
        at = e + 1;
        if (line.empty()) continue;
        Record r;
        size_t f = 0;
        while (f <= line.size()) {
            size_t t = line.find('\t', f);
            if (t == std::string::npos) t = line.size();
            const std::string field = line.substr(f, t - f);
            if (const size_t eq = field.find('='); eq != std::string::npos) r[field.substr(0, eq)] = field.substr(eq + 1);
            f = t + 1;
        }
        out.push_back(std::move(r));
    }
    return out;
}

std::string Store::read(const std::string &name) const {
    if (dir_.empty() || !safe_file(name)) return "";
    std::ifstream f(dir_ + "/" + name, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void Store::write(const std::string &name, const std::string &data) const {
    if (dir_.empty()) return;
    if (!safe_file(name)) {
        fprintf(stderr, "e-AMUSEMENT: not written: a call's field made no file name (%zu bytes)\n", name.size());
        return;
    }
    sys::make_dir(dir_);
    // whole or not at all: a full or failing disk keeps the player's file from before
    const std::string path = dir_ + "/" + name;
    if (!sys::write_file(path, data)) fprintf(stderr, "e-AMUSEMENT: cannot write %s: the one from before is kept\n", path.c_str());
}

void set_srcid(std::string &request, const std::string &pcbid) {
    const size_t he = request.find("\r\n\r\n");
    if (he == std::string::npos) return;
    std::string head = request.substr(0, he), body = request.substr(he + 4);
    const std::string info = header(head, "X-Eamuse-Info");
    if (!info.empty()) rc4(key(info), body);
    // the <call ...> tag's own srcid="..." (a space before it: not another attribute's end)
    const size_t call = body.find("<call");
    const size_t end = call == std::string::npos ? call : body.find('>', call);
    size_t at = call == std::string::npos ? call : body.find(" srcid=\"", call);
    if (end == std::string::npos || at == std::string::npos || at > end) return;
    at += 8;
    const size_t close = body.find('"', at);
    if (close == std::string::npos || close > end) return;
    const size_t was = body.size();
    body.replace(at, close - at, pcbid);
    if (!info.empty()) rc4(key(info), body);
    if (body.size() != was)
        for (size_t l = head.find("\r\n"); l != std::string::npos; l = head.find("\r\n", l + 2)) {
            if (head.size() < l + 17) break;
            std::string name = head.substr(l + 2, 15);
            for (char &c : name) c = (char)std::tolower((unsigned char)c);
            if (name != "content-length:") continue;
            const size_t e = head.find("\r\n", l + 2);
            head.replace(l + 2, (e == std::string::npos ? head.size() : e) - l - 2, "Content-Length: " + std::to_string(body.size()));
            break;
        }
    request = head + "\r\n\r\n" + body;
}

}  // namespace hk::ea
