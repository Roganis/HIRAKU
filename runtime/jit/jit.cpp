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

void Code::put(int st, int64_t h0, int64_t h1, int64_t h2, int64_t h3) {
    const Stencil &s = set_.stencils[st];
    const int64_t holes[4] = {h0, h1, h2, h3};
    const size_t base = bytes_.size();
    const size_t kept = (size_t)s.size - s.tail;   // the next stencil follows: no jump to it
    bytes_.insert(bytes_.end(), set_.code + s.code, set_.code + s.code + kept);
    for (int i = 0; i < s.nrelocs; i++) {
        const Reloc &r = set_.relocs[s.relocs + i];
        if ((size_t)r.at + 4 > kept) continue;     // in the jump not copied
        int64_t v;
        switch (r.kind) {
        case ABS32:
            v = holes[r.hole] + r.addend;
            if (v < INT32_MIN || v > (int64_t)UINT32_MAX) ok_ = false;
            break;
        case ABS32S:
            v = holes[r.hole] + r.addend;
            if (v < INT32_MIN || v > INT32_MAX) ok_ = false;
            break;
        default:   // REL32: from this field to the stencil's end, where the next one starts
            v = (int64_t)kept + r.addend - r.at;
            break;
        }
        const uint32_t w = (uint32_t)v;
        memcpy(&bytes_[base + r.at], &w, 4);
    }
}

bool wanted() {
    static const bool on = [] {
        const char *e = getenv("HK_JIT");
        return !e || strcmp(e, "0") != 0;
    }();
    return on;
}

#ifdef _WIN32

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
