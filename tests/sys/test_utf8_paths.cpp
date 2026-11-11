// SPDX-License-Identifier: GPL-3.0-or-later
// HIRAKU's paths are UTF-8 strings everywhere, and the plain-char file functions must take them
// so: a folder with letters outside ASCII (and, on Windows, outside the PC's own code page) is
// made, written in, read back and renamed through each of them, and reaches a second process
// whole through its arguments and its environment. On Windows that holds only for a program with
// cmake/windows/utf8.manifest in it (every program of HIRAKU's own); elsewhere it always does.
//
//   test_utf8_paths              exit 0 when all hold
//   test_utf8_paths child DIR    (itself, started by the test: DIR and $HK_TEST_DIR must be the folder)
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "shell/sys.h"

namespace {

namespace fs = std::filesystem;
int failed = 0;
void check(bool ok, const char *what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what);
    fflush(stdout);
    failed += !ok;
}

// "ユーザー café", as UTF-8 bytes
const char NAME[] = "\xe3\x83\xa6\xe3\x83\xbc\xe3\x82\xb6\xe3\x83\xbc caf\xc3\xa9";

fs::path as_path(const std::string &utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }
std::string as_utf8(const fs::path &p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

// what a second process says of the folder it was given: 0 both ways, else which failed
int child(const char *arg) {
    const char *env = getenv("HK_TEST_DIR");
    int r = 0;
    if (!hk::sys::is_dir(arg) || !std::ifstream(std::string(arg) + "/written.txt")) r |= 1;
    if (!env || !hk::sys::is_dir(env) || !std::ifstream(std::string(env) + "/written.txt")) r |= 2;
    return r;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 3 && std::string(argv[1]) == "child") return child(argv[2]);

    std::error_code ec;
    const fs::path top = fs::temp_directory_path() / as_path(std::string("hk-utf8-") + NAME);
    fs::remove_all(top, ec);
    const std::string dir = as_utf8(top);   // the folder's name as HIRAKU carries it: UTF-8

    check(hk::sys::make_dir(dir) && fs::is_directory(top), "a folder is made by its UTF-8 name (sys::make_dir)");
    check(hk::sys::is_dir(dir) && hk::sys::exists(dir), "... and found by it (sys::is_dir, sys::exists)");
    check(hk::sys::write_file(dir + "/written.txt", std::string("kept\n")) && fs::file_size(top / "written.txt", ec) == 5,
          "a file is written in it (sys::write_file)");
    {
        FILE *f = fopen((dir + "/c.txt").c_str(), "wb");
        const bool ok = f && fputs("c\n", f) >= 0;
        if (f) fclose(f);
        check(ok && fs::exists(top / "c.txt"), "... and by fopen");
    }
    {
        std::ofstream(dir + "/stream.txt") << "s\n";
        std::string got;
        std::ifstream(dir + "/stream.txt") >> got;
        check(got == "s" && fs::exists(top / "stream.txt"), "... and by a std::ofstream, read back by a std::ifstream");
    }
    check(hk::sys::replace_file(dir + "/c.txt", dir + "/stream.txt") && !fs::exists(top / "c.txt"), "a file in it replaces another (sys::replace_file)");
    check(fs::is_directory(fs::path(dir)) || fs::is_directory(as_path(dir)), "std::filesystem takes the same string");
    {
        const hk::sys::FileLock lock(dir + "/.lock");
        check(lock.held() && hk::sys::file_lock_held(dir + "/.lock"), "a lock file in it is held (sys::FileLock)");
    }
    // a second process: its arguments and its environment
    const std::string self = hk::sys::exe_path();
    check(!self.empty() && hk::sys::exists(self), "this program's own path is one it can open (sys::exe_path)");
#ifdef _WIN32
    _putenv_s("HK_TEST_DIR", dir.c_str());
#else
    setenv("HK_TEST_DIR", dir.c_str(), 1);
#endif
    const int r = hk::sys::run_quiet({self, "child", dir});
    check(r >= 0 && !(r & 1), "a program started with the folder as an argument finds it");
    check(r >= 0 && !(r & 2), "... and by an environment variable");

    fs::remove_all(top, ec);
    printf(failed ? "%d failed\n" : "all hold\n", failed);
    return failed ? 1 : 0;
}
