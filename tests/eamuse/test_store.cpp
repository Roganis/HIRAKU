// SPDX-License-Identifier: GPL-3.0-or-later
// The e-AMUSEMENT server's store (runtime/eamuse/store.h, tables.h): the folder back end names every
// table's files as HIRAKU always has (and finds each file's table and key again), refuses a key that
// would name a file outside the folder, keeps within the folder's file limit, and keeps aside what
// isn't a record.
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "eamuse/store.h"
#include "eamuse/tables.h"

namespace fs = std::filesystem;
using hk::ea::FolderStore;
using hk::ea::Key;
using hk::ea::Table;
namespace tables = hk::ea::tables;
static int failed = 0;

static void check(bool ok, const std::string &what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what.c_str());
    failed += !ok;
}

static void put_file(const fs::path &p, const std::string &d) { std::ofstream(p, std::ios::binary) << d; }
static std::string get_file(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

int main() {
    const fs::path top = fs::temp_directory_path() / ("hk-store-" + std::to_string((unsigned long long)std::hash<std::string>{}(fs::current_path().string()) ^ (unsigned long long)time(nullptr)));
    fs::remove_all(top);

    // 1. every table's file, for a made-up key, as the data folder has always named it
    struct Named {
        const Table &table;
        Key key;
        const char *file;
    };
    const Named names[] = {
        {tables::cards, {}, "cards"},
        {tables::refid, {}, "refid"},
        {tables::numbering, {}, "numbering"},
        {tables::users, {}, "users"},
        {tables::userdata_c00, {"E0040100000000AA"}, "userdata-E0040100000000AA"},
        {tables::userdata, {"D11", "E0040100000000AA"}, "userdata-D11-E0040100000000AA"},
        {tables::eemall, {"E0040100000000AA"}, "eemall-E0040100000000AA"},
        {tables::player, {"C20", "E0040100000000AA"}, "player-C20-E0040100000000AA"},
        {tables::pdata, {"F14", "48524B0000000001"}, "pdata-F14-48524B0000000001"},
        {tables::pscore, {"J39", "48524B0000000001"}, "pscore-J39-48524B0000000001"},
        {tables::pplays, {"G15", "48524B0000000001"}, "pplays-G15-48524B0000000001"},
        {tables::phc, {"48524B0000000001"}, "phc-48524B0000000001"},
        {tables::phcs, {"48524B0000000001"}, "phcs-48524B0000000001"},
        {tables::p24, {"48524B0000000001"}, "p24-48524B0000000001"},
        {tables::p24s, {"48524B0000000001"}, "p24s-48524B0000000001"},
        {tables::p24_plays, {}, "p24-plays"},
        {tables::lobby24, {}, "lobby24-M39"},
        {tables::rank_score, {}, "rank-score"},
        {tables::rank_hitasura, {}, "rank-hitasura"},
        {tables::rank_gamblerz, {}, "rank-gamblerz"},
        {tables::rank_exp, {"E13"}, "E13-rank-exp"},
        {tables::rank_ir, {"J39"}, "J39-rank-ir"},
        {tables::rank_shop, {"D11"}, "D11-rank-shop"},
        {tables::shop_phase, {"E13"}, "E13-shop-phase"},
        {tables::iidx9_scores, {}, "iidx9-scores"},
        {tables::iidx9_weekly, {}, "iidx9-weekly"},
        {tables::iidx9_ranking, {}, "iidx9-ranking"},
        {tables::iidx_players, {"E11"}, "iidx-E11-players"},
        {tables::iidx_scores, {"I00"}, "iidx-I00-scores"},
        {tables::iidx_courses, {"FDD"}, "iidx-FDD-courses"},
        {tables::gfdmv_cards, {}, "gfdmv-cards"},
        {tables::gfdmv_gdid, {}, "gfdmv-gdid"},
        {tables::gfdmv, {"1234"}, "gfdmv-1234"},
        {tables::gfdm_cards, {}, "gfdm-cards"},
        {tables::gfdm, {"D39", "1234"}, "gfdm-D39-1234"},
        {tables::message, {}, "message"},
        {tables::setting, {"phase", "H16"}, "phase-H16"},
        {tables::phases_phc, {}, "phases-phc"},
        {tables::unlock_phc, {}, "unlock-phc"},
        {tables::netvs_m39, {}, "netvs-M39"},
        {tables::iidx9_weekly_song, {}, "iidx9-weekly-song"},
        {tables::iidx9_courses, {}, "iidx9-courses"},
    };
    std::set<const Table *> named;
    for (const Named &n : names) {
        named.insert(&n.table);
        const std::string got = FolderStore::file_name(n.table, n.key);
        check(got == n.file, std::string(n.table.name) + ": " + got);
    }
    bool all_named = std::size(tables::all) == named.size();
    for (const Table *t : tables::all) all_named = all_named && named.count(t);
    check(all_named, "every table of tables.h is named above");
    check(FolderStore::file_name(tables::pdata, {"F14"}).empty(), "a key with too few parts names no file");
    check(tables::userdata_of("C00", "X").table.file == tables::userdata_c00.file && tables::userdata_of("D11", "X").key.size() == 2,
          "userdata_of: pop'n 9's by card, the others' by game and card");

    // 2. keys that would name a file outside the folder
    const fs::path dir = top / "eamuse";
    {
        FolderStore st(dir.string());
        fs::create_directories(dir);
        put_file(top / "pdata-F14-x", "outside");
        check(st.get(tables::pdata, {"F14", "../../pdata-F14-x"}).empty() && st.get(tables::pdata, {"F14/..", "x"}).empty(),
              "get refuses / and ..");
        check(!st.put(tables::pdata, {"F14", "../x"}, "no") && !st.put(tables::userdata_c00, {"a/b"}, "no") &&
                  !st.put(tables::setting, {"..", "x"}, "no") && !fs::exists(top / "x") && !fs::exists(dir / "a"),
              "put refuses / and .., and writes nothing");
        check(st.put(tables::cards, {}, "c\n") && st.get(tables::cards, {}) == "c\n", "a plain put and get");
        FolderStore none("");
        check(!none.put(tables::cards, {}, "c") && none.get(tables::cards, {}).empty(), "dir \"\": nothing kept");
    }

    // 3. the folder's file limit: a new file past it refused, an existing one still written
    {
        fs::remove_all(dir);
        fs::create_directories(dir);
        put_file(dir / "limit-files", "3\n");
        put_file(dir / "cards", "old");
        FolderStore st(dir.string());
        check(st.limit("files", 200000) == 3 && st.limit("cards", 65536) == 65536, "limit: the owner's, else the default");
        check(st.put(tables::refid, {}, "2\n"), "a new file under the limit");
        check(!st.put(tables::numbering, {}, "2\n") && !fs::exists(dir / "numbering"), "a new file past the limit refused");
        check(st.put(tables::cards, {}, "new") && get_file(dir / "cards") == "new", "an existing file still written");
    }

    // 4. aside: what was there kept as NAME.unread-DATE
    {
        fs::remove_all(dir);
        FolderStore st(dir.string());
        st.put(tables::pdata, {"F14", "R1"}, "short");
        st.aside(tables::pdata, {"F14", "R1"}, "short");
        std::string kept;
        for (const auto &e : fs::directory_iterator(dir))
            if (e.path().filename().string().rfind("pdata-F14-R1.unread-", 0) == 0) kept = get_file(e.path());
        check(kept == "short", "aside keeps the bytes as NAME.unread-DATE");
        check(st.label(tables::pdata, {"F14", "R1"}) == "pdata-F14-R1", "label: the file's name");
        auto held = st.hold();
        check(held && fs::exists(dir / ".lock"), "hold: the folder's .lock");
    }

    // 5. the folder's format: stamped once, with its first new file; a newer one is only read
    {
        fs::remove_all(dir);
        {
            FolderStore st(dir.string());
            st.put(tables::cards, {}, "a\n");
            check(get_file(dir / "format") == "1\n", "the first new file stamps the folder: format 1");
            put_file(dir / "format", "1 \n");   // (an edit would show if it were written again)
            st.put(tables::refid, {}, "2\n");
            st.put(tables::cards, {}, "b\n");
        }
        FolderStore again(dir.string());
        again.put(tables::numbering, {}, "3\n");
        check(get_file(dir / "format") == "1 \n", "... and only once");
        put_file(dir / "format", "2\n");
        FolderStore newer(dir.string());
        check(newer.get(tables::cards, {}) == "b\n", "a newer format's folder is read");
        check(!newer.put(tables::cards, {}, "c\n") && !newer.put(tables::users, {}, "u\n") && get_file(dir / "cards") == "b\n" &&
                  !fs::exists(dir / "users"),
              "... and not written to");
        fs::remove_all(dir);
        fs::create_directories(dir);
        put_file(dir / "limit-files", "0\n");
        FolderStore full(dir.string());
        check(!full.put(tables::cards, {}, "x") && !fs::exists(dir / "format"), "no stamp when nothing new is kept");
    }

    // 6. keys: a folder with a made-up file of every table (and files of none): each file is one
    // table's, the singleton's over a pattern's, the most specific pattern's over the owner's {}-{}
    {
        fs::remove_all(dir);
        fs::create_directories(dir);
        for (const Named &n : names) put_file(dir / n.file, "x");
        for (const char *other : {"format", ".lock", "pdata-F14-R1.unread-20261116-120000", "cards.tmp", "a-b-c-d"}) put_file(dir / other, "x");
        put_file(dir / "limit-cards", "5\n");   // the owner's, a {}-{} too
        fs::create_directories(dir / "sub-dir");
        FolderStore st(dir.string());
        bool each = true;
        for (const Named &n : names) {
            std::vector<Key> want = {n.key};
            if (&n.table == &tables::setting) want = {{"limit", "cards"}, {"phase", "H16"}};
            const std::vector<Key> got = st.keys(n.table);
            if (got != want) {
                each = false;
                printf("       %s: %zu keys\n", n.table.name, got.size());
            }
        }
        check(each, "keys: every table's file is its own, and only its");
        Key k;
        check(FolderStore::table_of("gfdmv-cards", k) == &tables::gfdmv_cards && FolderStore::table_of("gfdmv-77", k) == &tables::gfdmv &&
                  FolderStore::table_of("userdata-C20-X", k) == &tables::userdata && k == Key{"C20", "X"} &&
                  FolderStore::table_of("userdata-X", k) == &tables::userdata_c00 && FolderStore::table_of("p24-plays", k) == &tables::p24_plays &&
                  FolderStore::table_of("E13-rank-exp", k) == &tables::rank_exp && FolderStore::table_of("net_event-F14", k) == &tables::setting &&
                  !FolderStore::table_of("format", k),
              "table_of: the singleton, then the most specific pattern");
    }

    fs::remove_all(top);
    return failed ? 1 : 0;
}
