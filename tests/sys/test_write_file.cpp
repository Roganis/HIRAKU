// SPDX-License-Identifier: GPL-3.0-or-later
// hk::sys::write_file (runtime/shell/sys.h): a save is the whole new file or the file from before,
// never a cut one. A full disk is /dev/full standing in for the file written beside the save.
//
//   test_write_file      exit 0 when all hold
#include "shell/sys.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

namespace {

int failed = 0;

void check(bool ok, const char *what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what);
    failed += !ok;
}

std::string read(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    using namespace hk::sys;
#ifdef _WIN32
    const char *t = getenv("TEMP");
    const std::string dir = std::string(t ? t : ".") + "/hk-write-file-test";
#else
    const std::string dir = "/tmp/hk-write-file-test-" + std::to_string(getpid());
#endif
    make_dirs(dir);
    const std::string save = dir + "/flash0.bin", tmp = save + ".tmp";
    const std::string first(0x200000, 'a'), second = "the next save";

    check(write_file(save, first) && read(save) == first, "a new file: written whole");
    check(!exists(tmp), "nothing left beside it");
    check(write_file(save, second) && read(save) == second, "written again: the new one in its place");
    check(write_file(save, nullptr, 0) && exists(save) && read(save).empty(), "no bytes: an empty file");
    check(!write_file(dir + "/no-such-folder/x", second), "a folder that isn't there: false");

    write_file(save, first);
#ifndef _WIN32
    // the disk full: what is written beside the save goes to /dev/full (ENOSPC at the first write)
    if (symlink("/dev/full", tmp.c_str()) == 0) {
        check(!write_file(save, second), "a full disk: false");
        check(read(save) == first, "a full disk: the save from before whole");
        check(!exists(tmp), "a full disk: nothing left beside it");
    }
#endif
    // the place beside the save can't be written at all (a folder is there)
    make_dir(tmp);
    check(!write_file(save, second) && read(save) == first, "no file can be made beside it: false, the save from before whole");

    std::remove(tmp.c_str());
#ifdef _WIN32
    _rmdir(tmp.c_str());
#else
    rmdir(tmp.c_str());
#endif
    std::remove(save.c_str());
#ifdef _WIN32
    _rmdir(dir.c_str());
#else
    rmdir(dir.c_str());
#endif
    printf(failed ? "%d failed\n" : "all hold\n", failed);
    return failed ? 1 : 0;
}
