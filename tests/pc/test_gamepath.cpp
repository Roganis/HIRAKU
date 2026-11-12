// SPDX-License-Identifier: GPL-3.0-or-later
// The names a PC game is given for this machine's folders: ones its code page can write, whatever
// the folders are called. Under Wine (runtime/pc/gamepath.h): a link in Wine's folder for a folder
// with letters outside ASCII. On Windows (hk::sys::plain_path): the short names Windows keeps, or a
// link under ProgramData. Each name must lead to the same file.
//
//   test_gamepath [FOLDER]    FOLDER: where to make the test's folders (default: the temporary one)
// On Windows it is run by hand (Wine, a real Windows): its last lines say which ways the PC has.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "pc/gamepath.h"
#include "shell/sys.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;
int failed = 0;
void check(bool ok, const std::string &what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what.c_str());
    fflush(stdout);
    failed += !ok;
}

// "ゲーム" (no western code page has it), "café" (the Japanese one hasn't its last letter)
const char GAME[] = "\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0", CAFE[] = "caf\xc3\xa9";

fs::path as_path(const std::string &utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }
std::string as_utf8(const fs::path &p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}
bool plain(const std::string &s) {
    for (unsigned char c : s)
        if (c >= 0x80) return false;
    return true;
}
void write(const std::string &file, const std::string &text) {
    fs::create_directories(as_path(file).parent_path());
    std::ofstream(as_path(file), std::ios::binary) << text;
}

#ifndef _WIN32
std::string read(const fs::path &file) {
    std::ifstream f(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
// what Wine would open for a name (C:\ is the prefix's drive_c, Z:\ the root)
fs::path through(const std::string &prefix, std::string name) {
    for (char &c : name)
        if (c == '\\') c = '/';
    if (name.rfind("C:/", 0) == 0) return as_path(prefix + "/drive_c/" + name.substr(3));
    return as_path(name.substr(2));
}

void checks(const std::string &base) {
    const std::string prefix = base + "/wine";
    fs::create_directories(as_path(prefix + "/drive_c"));
    const std::string data = base + "/" + GAME + "/G15/data", deep = base + "/" + GAME + "/" + CAFE + "/prog";
    write(data + "/list.bin", "the data");
    write(deep + "/game.exe", "the program");
    write(base + "/plain/file.txt", "plain");
    write(base + "/plain/" + CAFE + ".txt", "a file's own name");
    write(data + "/" + GAME + ".bin", "a file's own name, in such a folder");

    hk::pc::WinePaths paths;
    // plain letters: Z:\ as ever, and no link
    std::string n = paths.name(base + "/plain/file.txt");
    check(n.rfind("Z:\\", 0) == 0 && n.find('/') == std::string::npos, "a path of plain letters is Z:\\...: " + n);
    n = paths.name(base + "/plain/" + CAFE + ".txt");
    check(n.rfind("Z:\\", 0) == 0, "a file with such a name in a plain folder stays where it is (no folder to link)");
    check(!paths.any(), "... and neither asks for a link");

    // a folder with such letters: a link, plain letters from end to end
    const std::string d = paths.name(data), f = paths.name(data + "/list.bin"), p = paths.name(deep + "/game.exe");
    check(d.rfind("C:\\hiraku-paths\\", 0) == 0 && plain(d), "a folder under " + std::string(GAME) + " is given through a link: " + d);
    check(f == d + "\\list.bin", "a file in it, the same link: " + f);
    check(plain(p) && p.rfind("C:\\hiraku-paths\\", 0) == 0 && p.size() == std::string("C:\\hiraku-paths\\").size() + 16 + std::string("\\prog\\game.exe").size(),
          "two such folders one in the other: the link is to the inner one: " + p);
    const std::string own = paths.name(data + "/" + GAME + ".bin");
    check(own.rfind(d + "\\", 0) == 0 && !plain(own), "a file with such a name in such a folder: the folder's link, the file's own name");
    const std::string later = paths.name(data + "/not-there-yet/x.bin");
    check(later == d + "\\not-there-yet\\x.bin", "a file that isn't there yet, the same");
    check(paths.name(data) == d && hk::pc::WinePaths().name(data) == d, "the same folder, the same link, in another run too");

    std::string why;
    check(paths.make(prefix, &why), "the links are made in Wine's folder" + (why.empty() ? "" : ": " + why));
    check(read(through(prefix, f)) == "the data", "the data's file is read through its name");
    check(read(through(prefix, p)) == "the program", "the program's too");
    check(read(through(prefix, own)) == "a file's own name, in such a folder", "and the file with a name of its own");
    check(paths.make(prefix, &why), "made again: nothing to do");
    // this machine's name for a folder by the way of its link (what Wine itself is started in)
    const std::string by_link = paths.through(prefix, data);
    check(by_link.rfind(prefix + "/drive_c/hiraku-paths/", 0) == 0 && plain(by_link.substr(prefix.size())) &&
              read(as_path(by_link + "/list.bin")) == "the data",
          "the folder by the way of its link: " + by_link.substr(prefix.size()));
    check(paths.through(prefix, base + "/plain") == base + "/plain", "... a plain one by its own name");
    // a link left by another run to a folder that has moved: made again
    const fs::path link = through(prefix, d);
    std::error_code ec;
    fs::remove(link, ec);
    fs::create_directory_symlink(as_path(base + "/plain"), link, ec);
    check(paths.make(prefix, &why) && read(through(prefix, f)) == "the data", "a link that led elsewhere leads to the folder again");
    // nothing there to make links in: said
    check(!paths.make(base + "/plain/file.txt", &why) && !why.empty(), "a prefix that can't hold the links: refused, and why");
}
#else
std::wstring wide(const std::string &utf8) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), w.data(), n);
    return w;
}
// The file's text, opened as a program that isn't a Unicode one would: by the name's bytes in the
// PC's code page ("" if the page can't write the name, or nothing opens)
std::string read_as_a_game(const std::string &name) {
    const std::wstring w = wide(name);
    const UINT page = hk::sys::ansi_code_page();
    BOOL lost = FALSE;
    char bytes[2048] = "";
    if (page == CP_UTF8) WideCharToMultiByte(page, 0, w.c_str(), -1, bytes, sizeof bytes, nullptr, nullptr);
    else if (!WideCharToMultiByte(page, WC_NO_BEST_FIT_CHARS, w.c_str(), -1, bytes, sizeof bytes, nullptr, &lost) || lost) return "";
    wchar_t back[2048];   // (this test may itself be a UTF-8 program: the bytes read back as that page's)
    if (!MultiByteToWideChar(page, 0, bytes, -1, back, 2048)) return "";
    HANDLE h = CreateFileW(back, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    char text[256];
    DWORD got = 0;
    ReadFile(h, text, sizeof text, &got, nullptr);
    CloseHandle(h);
    return std::string(text, got);
}
void drop_link(const std::string &name) {   // a link the test had made under ProgramData\hiraku\paths
    const size_t at = name.find("hiraku/paths/");
    if (at == std::string::npos) return;
    RemoveDirectoryW(wide(name.substr(0, at + 13 + 16)).c_str());
}

void checks(const std::string &base) {
    const unsigned page = hk::sys::ansi_code_page();
    printf("this PC's code page: %u\n", page);
    const std::string folder = base + "/" + GAME + " " + CAFE, file = folder + "/sub/list.bin";
    write(file, "the data");
    write(base + "/plain/file.txt", "plain");
    write(folder + "/" + GAME + ".bin", "a file's own name");

    bool ok = false;
    check(hk::sys::plain_path(base + "/plain/file.txt", &ok) == base + "/plain/file.txt" && ok, "a path of plain letters stays as it is");
    if (page != 65001) check(read_as_a_game(file).empty(), "as it is called, a game can't open the file (what this is for)");

    struct { unsigned ways; const char *what; bool must; } const WAYS[] = {
        {3, "any way", true}, {1, "short names", false}, {2, "a link", true},
        {7, "plain letters, any way", true}, {5, "plain letters, short names", false}, {6, "plain letters, a link", true},
    };
    for (const auto &w : WAYS) {
        const std::string name = hk::sys::plain_path(file, &ok, w.ways);
        if (!ok) {
            check(!w.must && name == file, std::string(w.what) + ": none here" + (w.must ? "" : " (this drive keeps no short names: the path comes back as it was)"));
            continue;
        }
        check(read_as_a_game(name) == "the data", std::string(w.what) + ": a game opens the file by " + name);
        if (w.ways & 4) check(plain(name), "... in plain letters only");
        check(name.find('\\') == std::string::npos, "... with / between folders, as it came");
        drop_link(name);
    }
    // the folder itself, and with \ between folders
    std::string back = folder;
    for (char &c : back)
        if (c == '/') c = '\\';
    const std::string dir = hk::sys::plain_path(back, &ok);
    check(ok && dir.find('/') == std::string::npos && read_as_a_game(dir + "\\sub\\list.bin") == "the data", "a folder, given with \\: " + dir);
    drop_link(hk::sys::plain_path(folder, &ok));
    // a file whose own name can't be written: by its short name, or not at all (a link is to a folder)
    const std::string own = hk::sys::plain_path(folder + "/" + GAME + ".bin", &ok);
    check(ok ? read_as_a_game(own) == "a file's own name" : own == folder + "/" + GAME + ".bin",
          ok ? "a file with such a name: by its short name, " + own : "a file with such a name, and no short names here: left as it is, and said");
    drop_link(own);
    // a name the code page can write though it isn't ASCII stays as it is, unless plain letters are asked for
    const std::string cafe = base + "/" + CAFE + "/file.txt";
    write(cafe, "coffee");
    if (!read_as_a_game(cafe).empty()) {
        check(hk::sys::plain_path(cafe, &ok) == cafe && ok, "a name this PC's code page can write stays as it is");
        const std::string p = hk::sys::plain_path(cafe, &ok, 7);
        check(ok && plain(p) && read_as_a_game(p) == "coffee", "... and is given in plain letters when asked: " + p);
        drop_link(p);
    } else printf("      (this PC's code page can't write \"caf\xc3\xa9\": that case isn't tried)\n");
    // a junction: made, made again to another folder, never over a real folder
    const std::string at = base + "/link";
    check(hk::sys::make_junction(at, base + "/plain") && fs::exists(as_path(at + "/file.txt")), "a junction leads to its folder");
    check(hk::sys::make_junction(at, folder) && fs::exists(as_path(at + "/sub/list.bin")) && fs::exists(as_path(base + "/plain/file.txt")),
          "made again to another folder: it leads there, and the first folder is whole");
    check(!hk::sys::make_junction(base + "/plain", folder) && fs::exists(as_path(base + "/plain/file.txt")), "a real folder is never made a junction");
    RemoveDirectoryW(wide(at).c_str());
}
#endif

}  // namespace

int main(int argc, char **argv) {
    std::error_code ec;
    const fs::path base = (argc > 1 ? as_path(argv[1]) : fs::temp_directory_path()) / ("hk-gamepath-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000));
    fs::remove_all(base, ec);
    fs::create_directories(base);
    checks(as_utf8(base));
    fs::remove_all(base, ec);
    printf(failed ? "%d FAILED\n" : "game paths ok\n", failed);
    return failed ? 1 : 0;
}
