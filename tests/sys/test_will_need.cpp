// SPDX-License-Identifier: GPL-3.0-or-later
// hk::sys::will_need (runtime/shell/sys.h): a read-ahead hint never changes what a read gets, and on
// Windows its own thread reads the range (Linux's kernel does it there: nothing to count).
//
//   test_will_need      exit 0 when all hold
#include "shell/sys.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

int failed = 0;

void check(bool ok, const char *what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what);
    failed += !ok;
}

uint32_t word_at(uint64_t k) { return (uint32_t)(k * 2654435761u) ^ (uint32_t)(k >> 7); }

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const std::string path = (fs::temp_directory_path() / "hk-will-need-test.bin").string();
    constexpr uint64_t kWords = 8 << 20;   // 32 MB
    {
        FILE *f = fopen(path.c_str(), "wb");
        if (!f) return printf("FAIL  cannot write %s\n", path.c_str()), 1;
        std::vector<uint32_t> buf(1 << 16);
        for (uint64_t k = 0; k < kWords; k += buf.size()) {
            for (size_t i = 0; i < buf.size(); i++) buf[i] = word_at(k + i);
            fwrite(buf.data(), 4, buf.size(), f);
        }
        fclose(f);
    }
    FILE *f = fopen(path.c_str(), "rb");
    check(f != nullptr, "the file opens");
    if (!f) return 1;
    const int fd = fileno(f);
    const uint64_t before = hk::sys::will_need_read();
    // the hint for the whole file, then reads through it as an emulated drive makes them, the hint
    // moved on as they go: every word as written
    hk::sys::will_need(fd, 0, kWords * 4);
    bool same = true;
    std::vector<uint32_t> got(1 << 15);
    for (uint64_t k = 0; k < kWords && same; k += got.size()) {
        if (k % (1 << 20) == 0) hk::sys::will_need(fd, k * 4, 8 << 20);
        same = hk::sys::pread(fd, got.data(), got.size() * 4, k * 4) == (ssize_t)(got.size() * 4);
        for (size_t i = 0; i < got.size() && same; i++) same = got[i] == word_at(k + i);
    }
    check(same, "reads with the hint get the file's bytes");
#ifdef _WIN32
    // its thread reads the last range asked for (8 MB from the file's last 4 MB on: to its end)
    const auto t0 = std::chrono::steady_clock::now();
    while (hk::sys::will_need_read() - before < (4u << 20) && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    printf("      read ahead: %.1f MB\n", (double)(hk::sys::will_need_read() - before) / (1 << 20));
    check(hk::sys::will_need_read() - before >= (4u << 20), "Windows: the read-ahead thread read the range");
#else
    check(hk::sys::will_need_read() == before, "Linux: the kernel reads ahead, not a thread of HIRAKU's");
#endif
    fclose(f);
    fs::remove(path);
    printf(failed ? "%d failed\n" : "all hold\n", failed);
    return failed ? 1 : 0;
}
