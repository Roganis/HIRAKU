// SPDX-License-Identifier: GPL-3.0-or-later
// Copy and patch (jit.h): stencils copied into a program, and memory that can run it.
#include "jit/jit.h"

#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace hk::jit {

void Code::put(int st, std::initializer_list<int64_t> holes, std::initializer_list<int> targets) {
    const Stencil &s = set_.stencils[st];
    const size_t base = bytes_.size();
    const size_t kept = (size_t)s.size - s.tail;   // the next stencil follows: no jump to it
    bytes_.insert(bytes_.end(), set_.code + s.code, set_.code + s.code + kept);
    for (int i = 0; i < s.nrelocs; i++) {
        const Reloc &r = set_.relocs[s.relocs + i];
        if ((size_t)r.at + 4 > kept) continue;     // in the jump not copied
        int64_t v;
        switch (r.kind) {
        case ABS32:
        case ABS32S:
            if (r.hole >= holes.size()) { ok_ = false; continue; }
            v = holes.begin()[r.hole] + r.addend;
            if (v < INT32_MIN || v > (r.kind == ABS32 ? (int64_t)UINT32_MAX : (int64_t)INT32_MAX)) ok_ = false;
            break;
        case REL32T: {
            if (r.hole >= targets.size()) { ok_ = false; continue; }
            const int l = targets.begin()[r.hole];
            if (l < 0 || (size_t)l >= labels_.size()) { ok_ = false; continue; }
            if (labels_[(size_t)l] < 0) {          // not bound yet: written when it is
                pending_.push_back({base + r.at, r.addend, l});
                continue;
            }
            v = labels_[(size_t)l] + r.addend - (int64_t)(base + r.at);
            break;
        }
        default:   // REL32: from this field to the stencil's end, where the next one starts
            v = (int64_t)kept + r.addend - r.at;
            break;
        }
        const uint32_t w = (uint32_t)v;
        memcpy(&bytes_[base + r.at], &w, 4);
    }
}

int Code::label() {
    labels_.push_back(-1);
    return (int)labels_.size() - 1;
}

void Code::bind(int label) {
    if (label < 0 || (size_t)label >= labels_.size() || labels_[(size_t)label] >= 0) { ok_ = false; return; }
    const int64_t here = (int64_t)bytes_.size();
    labels_[(size_t)label] = here;
    size_t n = 0;
    for (const Fix &f : pending_) {
        if (f.label != label) { pending_[n++] = f; continue; }
        const uint32_t w = (uint32_t)(here + f.addend - (int64_t)f.at);
        memcpy(&bytes_[f.at], &w, 4);
    }
    pending_.resize(n);
}

void Code::clear() {
    bytes_.clear();
    labels_.clear();
    pending_.clear();
    ok_ = true;
}

bool wanted() {
    static const bool on = [] {
        const char *e = getenv("HK_JIT");
        return !e || strcmp(e, "0") != 0;
    }();
    return on;
}

// ---- memory that can run -------------------------------------------------------------------
namespace {
bool writable(void *p, size_t n, bool on);   // RW (on) or RX
void *reserve(size_t n);                     // RX, zero-filled
void unreserve(void *p, size_t n);
size_t page() {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
#else
    return (size_t)sysconf(_SC_PAGESIZE);
#endif
}
}  // namespace

Arena::~Arena() {
    for (Piece &p : pieces_) unreserve(p.base, p.size);
}

void *Arena::add(const std::vector<unsigned char> &code) {
    if (code.empty() || failed_) return nullptr;
    const size_t need = (code.size() + 15) & ~(size_t)15;
    if (pieces_.empty() || pieces_.back().at + need > pieces_.back().size) {
        const size_t size = need > ((size_t)1 << 21) ? (need + 0xffff) & ~(size_t)0xffff : (size_t)1 << 21;
        if (reserved_ + size > limit_) return nullptr;
        void *m = reserve(size);
        if (!m) { failed_ = true; return nullptr; }
        pieces_.push_back({static_cast<unsigned char *>(m), size, 0});
        reserved_ += size;
    }
    Piece &p = pieces_.back();
    unsigned char *to = p.base + p.at;
    // the pages it lands on, writable while it is written
    const size_t pg = page();
    unsigned char *lo = p.base + (p.at & ~(pg - 1));
    const size_t span = ((p.at + need + pg - 1) & ~(pg - 1)) - (p.at & ~(pg - 1));
    if (!writable(lo, span, true)) { failed_ = true; return nullptr; }
    memcpy(to, code.data(), code.size());
    if (!writable(lo, span, false)) { failed_ = true; return nullptr; }
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), to, code.size());
#else
    __builtin___clear_cache((char *)to, (char *)to + code.size());
#endif
    p.at += need;
    used_ += need;
    return to;
}

#ifdef _WIN32

namespace {
bool writable(void *p, size_t n, bool on) {
    DWORD old;
    return VirtualProtect(p, n, on ? PAGE_READWRITE : PAGE_EXECUTE_READ, &old) != 0;
}
void *reserve(size_t n) { return VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READ); }
void unreserve(void *p, size_t) { VirtualFree(p, 0, MEM_RELEASE); }
}  // namespace

void *executable(const std::vector<unsigned char> &code) {
    if (code.empty()) return nullptr;
    void *m = VirtualAlloc(nullptr, code.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!m) return nullptr;
    memcpy(m, code.data(), code.size());
    DWORD old;
    if (!VirtualProtect(m, code.size(), PAGE_EXECUTE_READ, &old)) {
        VirtualFree(m, 0, MEM_RELEASE);
        return nullptr;
    }
    FlushInstructionCache(GetCurrentProcess(), m, code.size());
    return m;
}

void release(void *mem, size_t) {
    if (mem) VirtualFree(mem, 0, MEM_RELEASE);
}

#else

namespace {
bool writable(void *p, size_t n, bool on) { return mprotect(p, n, on ? PROT_READ | PROT_WRITE : PROT_READ | PROT_EXEC) == 0; }
void *reserve(size_t n) {
    // (a system that forbids executable memory refuses here or at the first add: the caller interprets)
    void *m = mmap(nullptr, n, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return m == MAP_FAILED ? nullptr : m;
}
void unreserve(void *p, size_t n) { munmap(p, n); }
}  // namespace

void *executable(const std::vector<unsigned char> &code) {
    if (code.empty()) return nullptr;
    void *m = mmap(nullptr, code.size(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return nullptr;
    memcpy(m, code.data(), code.size());
    // (a system that forbids making memory executable refuses here: the caller interprets)
    if (mprotect(m, code.size(), PROT_READ | PROT_EXEC) != 0) {
        munmap(m, code.size());
        return nullptr;
    }
    __builtin___clear_cache((char *)m, (char *)m + code.size());
    return m;
}

void release(void *mem, size_t size) {
    if (mem) munmap(mem, size);
}

#endif

}  // namespace hk::jit
