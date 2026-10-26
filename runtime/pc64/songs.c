// SPDX-License-Identifier: GPL-3.0-or-later
// Songs back from the player's music data (PLAN.md section 34): the 64-bit PC pop'n keeps a slot
// for each song it ever had, those it left out marked so (their mask 0x80000000); the mods' music
// data (popnhax's XML: <music id> with its texts, characters and <chart idx> files) fills them in
// again, each song whose charts' files are there (the mods' folders, laid over the game's data by
// files.c, or the game's own), before the game starts. Nothing of the game's comes with HIRAKU:
// the songs, their texts and charts are the player's.
//
// After Milo's work on the same game (popnMods, its native Omnimix, with thanks), whose findings
// this follows: the music table's layout (312 bytes a song: its texts at
// +0, +8, +16 (full width genre, title, artist), +24, +40, +48 (genre, title, artist); its two
// characters at +56; its mask at +60, the folder at +64, the version at +68, the categories at
// +72; the seven charts' levels at +76, their chart numbers at +84, their hold flags at +214), the
// chart table's (40 bytes: type, keys, the folder and file names, four audio parameters), the
// difficulty bits the game asks of the mask, characters found by name, and the old songs' own
// background, which the game waits for forever when it isn't there. HIRAKU's own findings: a song
// is listed only for the regions at +120 (0 in the slots left out), and the music select shows each
// chart's tempos from +222.
//
// The tables are found by the code that reads them, not by the program's date:
//   - the music table: its mask getter (imul rax,rcx,0x138; lea rcx,[image]; mov eax,[rax+rcx+X]);
//     its songs: the id check (lea eax,[rcx+0x55]; mov edx,N; cmp ax,dx: ids -85..N-0x55);
//   - the charts: their file reader (lea rcx,[table+8]; lea rax,[r8+r8*4]; mov rax,[rcx+rax*8]) and
//     its check (mov eax,X; mov edx,N; cmp cx,dx; cmovae cx,ax);
//   - the characters: their name getter (shl rdx,7; lea rax,[table]; mov r8d,0x100; mov rdx,[rdx+rax])
//     and its check (mov edx,N-1; mov eax,X; cmp cx,dx; cmovbe ax,cx);
//   - the old songs' background: after the checks that a song has one (bt eax,8; jae SKIP), the
//     comparison of the one loaded (cmp [loaded],rsi): a song brought back goes to SKIP.
#include "pc64.h"

#include <stdlib.h>
#include <string.h>

#define RECORD 312u
#define REGIONS_AT 120u   // the regions a song is played in (the music select asks for the cabinet's)
#define TEMPOS_AT 222u    // six charts' tempos, 12 bytes each
#define REMOVED 0x80000000u
#define SLOTS 7
static const char *const SLOT_NAMES[SLOTS] = {"ep", "np", "hp", "op", "bp_n", "bp_h", "bp_ex"};
// the difficulty bits the game asks of a song's mask before it reads a chart (Normal and Battle
// Normal have none); slots 4 and 5, the battle charts, may be left out
#define DIFFICULTY_BITS 0x07280000u
#define LOCKED 0x08020080u
static const unsigned SLOT_BITS[SLOTS] = {0x00080000u, 0, 0x01000000u, 0x02000000u, 0, 0x04000000u, 0x00200000u};

static BYTE *g_music, *g_charts, *g_charas;
static unsigned g_songs, g_nchart, g_nchara;
static BYTE *g_bg_site, *g_bg_skip, *g_bg_loaded;

// --- the code's patterns: 0x100 any byte. Where it is (up to `max` places), and how many
static int find_all(BYTE *base, const short *pat, size_t n, BYTE **hits, int max) {
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    int count = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_CNT_CODE)) continue;
        BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - n;
        for (; p < end; p++) {
            size_t k = 0;
            while (k < n && (pat[k] == 0x100 || p[k] == pat[k])) k++;
            if (k == n && count++ < max) hits[count - 1] = p;
        }
    }
    return count;
}
// ... the one place it is (NULL: none, or several)
static BYTE *find(BYTE *base, const short *pat, size_t n) {
    BYTE *hit;
    return find_all(base, pat, n, &hit, 1) == 1 ? hit : NULL;
}
#define X 0x100
static int32_t i32(const BYTE *p) { int32_t v; memcpy(&v, p, 4); return v; }

static int locate(BYTE *base) {
    // the mask getter's end: jns; xor eax,eax; ...; ret; cmp byte [X],0; jne; imul rax,rcx,0x138; lea rcx,[image]; mov eax,[rax+rcx+mask]
    static const short music[] = {0x79, 0x07, 0x33, 0xc0, 0x48, 0x83, 0xc4, 0x28, 0xc3, 0x80, 0x3d, X, X, X, X, 0, 0x75, X,
                                  0x48, 0x69, 0xc1, 0x38, 0x01, 0, 0, 0x48, 0x8d, 0x0d, X, X, X, X, 0x8b, 0x84, 0x08};
    static const short songs[] = {0x8d, 0x41, 0x55, 0xba, X, X, 0, 0, 0x66, 0x3b, 0xc2};
    static const short charts[] = {0x48, 0x8d, 0x0d, X, X, X, X, 0x4b, 0x8d, 0x04, 0x80, 0x48, 0x8b, 0x04, 0xc1};
    static const short nchart[] = {0xb8, X, X, 0, 0, 0xba, X, X, 0, 0, 0x66, 0x3b, 0xca, 0x66, 0x0f, 0x43, 0xc8};
    static const short charas[] = {0x48, 0xc1, 0xe2, 0x07, 0x48, 0x8d, 0x05, X, X, X, X, 0x41, 0xb8, 0, 1, 0, 0, 0x48, 0x8b, 0x14, 0x02};
    static const short nchara[] = {0xba, X, X, 0, 0, 0xb8, X, X, 0, 0, 0x66, 0x3b, 0xca, 0x66, 0x0f, 0x46, 0xc1, 0xc3};
    static const short bg[] = {0x0f, 0xba, 0xe0, 0x08, 0x0f, 0x83, X, X, X, X, 0x48, 0x39, 0x35, X, X, X, X, 0x75};
    BYTE *p;
    if ((p = find(base, music, sizeof music / sizeof *music)) && p + 32 + i32(p + 28) == base) g_music = base + i32(p + 35) - 60;
    if ((p = find(base, songs, sizeof songs / sizeof *songs))) g_songs = (unsigned)i32(p + 4) - 0x55 + 1;
    BYTE *hits[4];   // the folder's reader (+8) and the file's (+16)
    for (int i = 0, k = find_all(base, charts, sizeof charts / sizeof *charts, hits, 4); i < k && i < 4; i++)
        if (!g_charts || hits[i] + 7 + i32(hits[i] + 3) - 8 < g_charts) g_charts = hits[i] + 7 + i32(hits[i] + 3) - 8;
    if ((p = find(base, nchart, sizeof nchart / sizeof *nchart))) g_nchart = (unsigned)i32(p + 6);
    if ((p = find(base, charas, sizeof charas / sizeof *charas))) g_charas = p + 11 + i32(p + 7);
    if ((p = find(base, nchara, sizeof nchara / sizeof *nchara))) g_nchara = (unsigned)i32(p + 1) + 1;
    if ((p = find(base, bg, sizeof bg / sizeof *bg))) g_bg_skip = p + 10 + i32(p + 6), g_bg_site = p + 10, g_bg_loaded = p + 17 + i32(p + 13);
    if (!g_music || !g_charts || !g_charas || g_songs < 1000 || g_songs > 32768 || g_nchart < 1000 || g_nchart > 65535 || g_nchara < 100 ||
        g_nchara > 65535 || !g_bg_site) {
        pc64_log("songs: the game's tables not found (music %p %u, charts %p %u, characters %p %u, background %p): none brought back",
                 (void *)g_music, g_songs, (void *)g_charts, g_nchart, (void *)g_charas, g_nchara, (void *)g_bg_site);
        return 0;
    }
    return 1;
}

// --- the music data: each song's fields, the first mod's where two give one
struct chart {
    char *folder, *file;
    int audio[4];
    unsigned type, keys, level, hold;
};
struct song {
    char *text[6];   // fw_genre, fw_title, fw_artist, genre, title, artist
    char *chara[2];
    unsigned mask, folder, version, categories, has;   // has: bit 0-3 the numbers given, 4 the BPMs
    unsigned short bpm[12];   // display_bpm: each chart's (but the last) tempos, two groups of six
    struct chart chart[SLOTS];
};
static struct song *g_data;   // by id, g_songs of them

static char *keep(const char *s, size_t n) {
    char *k = malloc(n + 1);
    if (k) memcpy(k, s, n), k[n] = 0;
    return k;
}
// a tag's text between start and end (NULL: none), in the game's Shift-JIS, XML's entities undone
static char *text_of(const char *start, const char *end, const char *tag, int utf8) {
    char open[40];
    const size_t n = (size_t)snprintf(open, sizeof open, "<%s", tag);
    for (const char *p = start; p + n < end; p++) {
        if (memcmp(p, open, n) || (p[n] != '>' && p[n] != ' ' && p[n] != '/')) continue;
        const char *gt = memchr(p, '>', (size_t)(end - p));
        if (!gt) return NULL;
        if (gt[-1] == '/') return keep("", 0);
        char close[44];
        snprintf(close, sizeof close, "</%s>", tag);
        const char *c = gt + 1;
        const size_t cn = strlen(close);
        while (c + cn <= end && memcmp(c, close, cn)) c++;
        if (c + cn > end) return NULL;
        size_t len = (size_t)(c - gt - 1);
        char *raw = keep(gt + 1, len);
        if (!raw) return NULL;
        char *w = raw;   // entities
        for (char *r = raw; *r;) {
            static const struct { const char *e; char c; } E[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
            size_t i = 0;
            if (*r == '&')
                for (; i < 5; i++)
                    if (!strncmp(r, E[i].e, strlen(E[i].e))) break;
            if (*r == '&' && i < 5) *w++ = E[i].c, r += strlen(E[i].e);
            else *w++ = *r++;
        }
        *w = 0;
        if (!utf8) return raw;
        const int wn = MultiByteToWideChar(CP_UTF8, 0, raw, -1, NULL, 0);
        WCHAR *wide = wn > 0 ? malloc(sizeof(WCHAR) * (size_t)wn) : NULL;
        char *sj = NULL;
        if (wide && MultiByteToWideChar(CP_UTF8, 0, raw, -1, wide, wn)) {
            const int sn = WideCharToMultiByte(932, 0, wide, -1, NULL, 0, NULL, NULL);
            sj = sn > 0 ? malloc((size_t)sn) : NULL;
            if (sj && !WideCharToMultiByte(932, 0, wide, -1, sj, sn, NULL, NULL)) free(sj), sj = NULL;
        }
        free(wide);
        free(raw);
        return sj;
    }
    return NULL;
}
static int number_of(const char *start, const char *end, const char *tag, unsigned *out) {
    char *t = text_of(start, end, tag, 0);
    if (!t) return 0;
    char *e;
    const unsigned long v = strtoul(t, &e, 10);
    const int ok = e != t;
    free(t);
    if (ok) *out = (unsigned)v;
    return ok;
}

// a music data file: what it gives of a song replaces what was read before (the files are read from
// the last mod's to the first's, each mod's in their names' order: the first mod wins)
static void read_xml(const WCHAR *path) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size;
    char *buf = NULL;
    DWORD got = 0;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 256 << 20 && (buf = malloc((size_t)size.QuadPart + 1)))
        ReadFile(f, buf, (DWORD)size.QuadPart, &got, NULL);
    CloseHandle(f);
    if (!buf) return;
    buf[got] = 0;
    // UTF-8 unless it says Shift-JIS, and then too when its bytes are UTF-8 (a pack's file says
    // shift-jis and isn't: Shift-JIS text is almost never valid UTF-8)
    const int utf8 = !(strstr(buf, "shift-jis") || strstr(buf, "SHIFT-JIS") || strstr(buf, "Shift_JIS") || strstr(buf, "SHIFT_JIS") ||
                       strstr(buf, "cp932") || strstr(buf, "windows-31j")) ||
                     MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buf, (int)got, NULL, 0) > 0;
    for (char *c = buf; (c = strstr(c, "<!--"));) {   // comments: no songs in them
        char *e = strstr(c + 4, "-->");
        if (!e) break;
        memset(c, ' ', (size_t)(e + 3 - c));
        c = e + 3;
    }
    unsigned count = 0;
    for (char *m = buf; (m = strstr(m, "<music"));) {
        if (m[6] != ' ') {   // <musicdb>
            m += 6;
            continue;
        }
        char *gt = strchr(m, '>'), *close = gt ? strstr(gt, "</music>") : NULL;
        if (!close) break;
        char *id_at = strstr(m, "id=");
        unsigned id = id_at && id_at < gt && (id_at[3] == '"' || id_at[3] == '\'') ? (unsigned)strtoul(id_at + 4, NULL, 10) : ~0u;
        m = close + 8;
        if (id >= g_songs) continue;   // a song past the game's table (its own new songs): not yet
        struct song *s = &g_data[id];
        // the song's own fields come before its charts (a chart's <folder> isn't the song's)
        char *own_end = strstr(gt, "<chart");
        if (!own_end || own_end > close) own_end = close;
        static const char *const TEXTS[6] = {"fw_genre", "fw_title", "fw_artist", "genre", "title", "artist"};
        for (int i = 0; i < 6; i++) {
            char *t = text_of(gt, own_end, TEXTS[i], utf8);
            if (t) s->text[i] = t;
        }
        for (int i = 0; i < 2; i++) {
            char *t = text_of(gt, own_end, i ? "chara2" : "chara1", utf8);
            if (t) s->chara[i] = t;
        }
        static const char *const NUMS[4] = {"mask", "folder", "cs_version", "categories"};
        unsigned *const at[4] = {&s->mask, &s->folder, &s->version, &s->categories};
        for (int i = 0; i < 4; i++) {
            unsigned v;
            if (number_of(gt, own_end, NUMS[i], &v)) *at[i] = v, s->has |= 1u << i;
        }
        char *bpm = text_of(gt, own_end, "display_bpm", 0);
        if (bpm) {
            char *p = bpm;
            for (int i = 0; i < 12; i++) s->bpm[i] = (unsigned short)strtoul(p, &p, 10);
            s->has |= 16;
            free(bpm);
        }
        for (char *c = gt; (c = strstr(c, "<chart")) && c < close;) {
            if (c[6] != ' ' && c[6] != '>') {   // <charts>
                c += 6;
                continue;
            }
            char *cgt = strchr(c, '>'), *cend = cgt ? strstr(cgt, "</chart>") : NULL;
            if (!cend || cend > close) break;
            int slot = -1;
            char *idx = strstr(c, "idx=");
            if (idx && idx < cgt && (idx[4] == '"' || idx[4] == '\''))
                for (int i = 0; i < SLOTS; i++)
                    if (!strncmp(idx + 5, SLOT_NAMES[i], strlen(SLOT_NAMES[i])) && idx[5 + strlen(SLOT_NAMES[i])] == idx[4]) slot = i;
            c = cend + 8;
            if (slot < 0) continue;
            struct chart *ch = &s->chart[slot];
            char *folder = text_of(cgt, cend, "folder", utf8), *file = text_of(cgt, cend, "filename", utf8);
            if (folder && file) ch->folder = folder, ch->file = file;
            unsigned v;
            static const char *const AUDIO[4] = {"audio_param1", "audio_param2", "audio_param3", "audio_param4"};
            for (int i = 0; i < 4; i++)
                if (number_of(cgt, cend, AUDIO[i], &v)) ch->audio[i] = (int)v;
            if (number_of(cgt, cend, "file_type", &v)) ch->type = v;
            if (number_of(cgt, cend, "used_keys", &v)) ch->keys = v;
            if (number_of(cgt, cend, "diff", &v)) ch->level = v;
            if (number_of(cgt, cend, "hold_flag", &v)) ch->hold = v;
        }
        count++;
    }
    free(buf);
    if (count) {
        char name[MAX_PATH * 2];
        WideCharToMultiByte(CP_UTF8, 0, path, -1, name, sizeof name, NULL, NULL);
        pc64_log("songs: %u in %s", count, name);
    }
}

// every .xml under a mod's folder, in their names' order
static int by_name(const void *a, const void *b) { return _wcsicmp(*(WCHAR *const *)a, *(WCHAR *const *)b); }
static void xml_files(const WCHAR *dir, WCHAR ***list, size_t *n, size_t *cap, int depth) {
    WCHAR pattern[MAX_PATH * 2];
    _snwprintf(pattern, MAX_PATH * 2, L"%ls\\*", dir);
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW(pattern, &d);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (d.cFileName[0] == L'.' || (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        WCHAR path[MAX_PATH * 2];
        _snwprintf(path, MAX_PATH * 2, L"%ls\\%ls", dir, d.cFileName);
        const size_t len = wcslen(d.cFileName);
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 8) xml_files(path, list, n, cap, depth + 1);
        } else if (len > 4 && !_wcsicmp(d.cFileName + len - 4, L".xml")) {
            if (*n == *cap) {
                *cap = *cap ? *cap * 2 : 64;
                WCHAR **more = realloc(*list, sizeof **list * *cap);
                if (!more) break;
                *list = more;
            }
            (*list)[(*n)++] = _wcsdup(path);
        }
    } while (FindNextFileW(h, &d));
    FindClose(h);
}

// --- the files a song needs: in a mod's folder (over the game's data), or the game's own
static WCHAR g_mods[16][MAX_PATH];
static int g_nmods;
static int data_file(const char *rel) {   // rel: under the game's data folder, Shift-JIS
    WCHAR w[MAX_PATH], path[MAX_PATH * 2];
    if (!MultiByteToWideChar(932, 0, rel, -1, w, MAX_PATH)) return 0;
    for (WCHAR *c = w; *c; c++)
        if (*c == L'/') *c = L'\\';
    for (int i = 0; i <= g_nmods; i++) {
        if (i < g_nmods) _snwprintf(path, MAX_PATH * 2, L"%ls\\%ls", g_mods[i], w);
        else {
            WCHAR game[MAX_PATH] = L"", folder[32];
            MultiByteToWideChar(CP_UTF8, 0, pc64_game_folder(), -1, game, MAX_PATH);
            MultiByteToWideChar(CP_UTF8, 0, pc64_data_folder(), -1, folder, 32);
            _snwprintf(path, MAX_PATH * 2, L"%ls%ls%ls\\%ls", game, game[0] ? L"\\" : L"", folder, w);
        }
        const DWORD a = GetFileAttributesW(path);
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) return 1;
    }
    return 0;
}
static int plain_name(const char *s) { return s && *s && !strpbrk(s, "/\\:") && !strstr(s, ".."); }

// --- the banners: a song brought back needs one (the music select asks for it until it comes): the
// game's own packs (tex/system/kc*.ifs: their pictures named kc_NNNN inside) or a mod's folders
static unsigned char *g_banner;   // by id
static void game_banners(void) {
    WCHAR game[MAX_PATH] = L"", folder[32], pattern[MAX_PATH * 2];
    MultiByteToWideChar(CP_UTF8, 0, pc64_game_folder(), -1, game, MAX_PATH);
    MultiByteToWideChar(CP_UTF8, 0, pc64_data_folder(), -1, folder, 32);
    _snwprintf(pattern, MAX_PATH * 2, L"%ls%ls%ls\\tex\\system\\kc*.ifs", game, game[0] ? L"\\" : L"", folder);
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW(pattern, &d);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        WCHAR path[MAX_PATH * 2];
        _snwprintf(path, MAX_PATH * 2, L"%ls%ls%ls\\tex\\system\\%ls", game, game[0] ? L"\\" : L"", folder, d.cFileName);
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (f == INVALID_HANDLE_VALUE) continue;
        static char buf[1 << 20];
        char tail[8] = {0};
        DWORD got;
        while (ReadFile(f, buf + 8, sizeof buf - 8, &got, NULL) && got) {   // the 8 bytes before kept for a name across reads
            memcpy(buf, tail, 8);
            for (DWORD i = 0; i + 8 <= got + 8; i++)
                if (buf[i] == 'k' && buf[i + 1] == 'c' && buf[i + 2] == '_' && i + 7 < got + 8) {
                    unsigned id = 0, k = 3;
                    for (; k < 8 && buf[i + k] >= '0' && buf[i + k] <= '9'; k++) id = id * 10 + (unsigned)(buf[i + k] - '0');
                    if (k == 7 && id < g_songs) g_banner[id] = 1;
                }
            memcpy(tail, buf + got, 8);
        }
        CloseHandle(f);
    } while (FindNextFileW(h, &d));
    FindClose(h);
}
static int has_banner(unsigned id) {
    if (g_banner[id]) return 1;
    static const char *const KC[] = {"tex/system/kc_diff_ifs", "tex/system/kc_jka_diff_ifs", "tex/system/kc_jka_ifs"};
    char rel[64];
    for (int i = 0; i < 3; i++) {
        snprintf(rel, sizeof rel, "%s/kc_%04u.ifs", KC[i], id);
        if (data_file(rel)) return 1;
    }
    return 0;
}

// --- the characters: by name, the one the game's own songs use most (a name can be several), the
// pair of the two side by side when the song names both
static unsigned short *g_uses;
static unsigned g_unknown_charas;   // songs none of whose characters the game has
static const BYTE *g_image, *g_image_end;
static int chara_is(unsigned i, const char *name) {
    const char *p;
    memcpy(&p, g_charas + (size_t)i * 128, sizeof p);
    const size_t n = strlen(name);
    if ((const BYTE *)p < g_image || (const BYTE *)p + n + 1 > g_image_end) return 0;   // the game's own texts only
    return !memcmp(p, name, n + 1);
}
static int chara_named(const char *name) {
    int best = -1;
    for (unsigned i = 0; i < g_nchara && name && *name && strcmp(name, "0"); i++)
        if (chara_is(i, name) && (best < 0 || g_uses[i] > g_uses[best])) best = (int)i;
    return best;
}
static void characters(const struct song *s, BYTE *rec) {
    int c[2] = {chara_named(s->chara[0]), chara_named(s->chara[1])};
    if (s->chara[0] && s->chara[1] && strcmp(s->chara[0], s->chara[1])) {
        int pair = -1;
        unsigned best = 0;
        for (unsigned i = 0; i + 1 < g_nchara; i++)
            if (chara_is(i, s->chara[0]) && chara_is(i + 1, s->chara[1]) && (pair < 0 || g_uses[i] + g_uses[i + 1] > best))
                pair = (int)i, best = g_uses[i] + g_uses[i + 1];
        if (pair >= 0 && (best || (c[0] < 0 && c[1] < 0))) c[0] = pair, c[1] = pair + 1;
    }
    // a character the game hasn't (an old song's own): the song's other one, else the slot's (the
    // game's first character)
    if (c[0] < 0) c[0] = c[1];
    if (c[1] < 0) c[1] = c[0];
    if (c[0] < 0 && (s->chara[0] || s->chara[1])) g_unknown_charas++;
    for (int k = 0; k < 2; k++)
        if (c[k] >= 0) {
            const uint16_t v = (uint16_t)c[k];
            memcpy(rec + 56 + 2 * k, &v, 2);
        }
}

// --- the background: a song brought back has none of its own (the game's packs don't hold it):
// the game's way for a song without one
static unsigned char *g_back;   // by id: brought back
static int guard_background(BYTE *base) {
    // a page near the game's code (a jump of 32 bits from the site)
    BYTE *page = NULL;
    const uintptr_t at = ((uintptr_t)g_bg_site + 0x200000) & ~(uintptr_t)0xffff;
    for (uintptr_t d = 0; d < 0x40000000 && !page; d += 0x10000) {
        page = VirtualAlloc((void *)(at + d), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!page && at > d) page = VirtualAlloc((void *)(at - d), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    const int64_t rel = page ? (int64_t)(uintptr_t)page - (int64_t)(uintptr_t)(g_bg_site + 5) : 0;
    if (!page || rel < INT32_MIN || rel > INT32_MAX) {
        pc64_log("songs: no room for the background's guard: none brought back");
        return 0;
    }
    // push rax; push rdx; movzx eax,bx (the song); cmp eax,SONGS; jae native;
    // mov rdx,g_back; cmp byte [rdx+rax],0; je native; pop rdx; pop rax; jmp [SKIP]
    // native: pop rdx; pop rax; push rax; mov rax,LOADED; cmp [rax],rsi; pop rax; jmp [CONTINUE]
    BYTE *q = page, *jae, *je;
    static const BYTE start[] = {0x50, 0x52, 0x0f, 0xb7, 0xc3, 0x3d};
    memcpy(q, start, sizeof start), q += sizeof start;
    memcpy(q, &g_songs, 4), q += 4;
    *q++ = 0x73, jae = q++;
    *q++ = 0x48, *q++ = 0xba;
    const uint64_t back = (uint64_t)(uintptr_t)g_back;
    memcpy(q, &back, 8), q += 8;
    *q++ = 0x80, *q++ = 0x3c, *q++ = 0x02, *q++ = 0x00;
    *q++ = 0x74, je = q++;
    static const BYTE jmp[] = {0xff, 0x25, 0, 0, 0, 0};
    *q++ = 0x5a, *q++ = 0x58;
    memcpy(q, jmp, 6), q += 6;
    uint64_t to = (uint64_t)(uintptr_t)g_bg_skip;
    memcpy(q, &to, 8), q += 8;
    BYTE *native = q;
    *q++ = 0x5a, *q++ = 0x58, *q++ = 0x50, *q++ = 0x48, *q++ = 0xb8;
    to = (uint64_t)(uintptr_t)g_bg_loaded;
    memcpy(q, &to, 8), q += 8;
    *q++ = 0x48, *q++ = 0x39, *q++ = 0x30, *q++ = 0x58;
    memcpy(q, jmp, 6), q += 6;
    to = (uint64_t)(uintptr_t)(g_bg_site + 7);
    memcpy(q, &to, 8), q += 8;
    *jae = (BYTE)(native - jae - 1), *je = (BYTE)(native - je - 1);
    DWORD old;
    VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, (size_t)(q - page));
    BYTE patch[7] = {0xe9, 0, 0, 0, 0, 0x90, 0x90};   // jmp page; (the cmp's other two bytes)
    const int32_t disp = (int32_t)rel;
    memcpy(patch + 1, &disp, 4);
    VirtualProtect(g_bg_site, 7, PAGE_EXECUTE_READWRITE, &old);
    memcpy(g_bg_site, patch, 7);
    VirtualProtect(g_bg_site, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_bg_site, 7);
    (void)base;
    return 1;
}

// The songs a cabinet needs a licence for (mask bit 0x20000): the game lists one only when the
// common info's license_music names it (0x180094210), a list of 1187 songs at most (2374 bytes),
// while those songs are far apart (18 in 2026041500, ids 670 to 2321) and unknown to a server.
// HIRAKU is a cabinet licensed for all of them: the bit cleared (the game reads it nowhere else).
#define LICENSED 0x20000u
static void licensed_all(void) {
    unsigned n = 0;
    for (unsigned id = 0; id < g_songs; id++) {
        BYTE *at = g_music + (size_t)id * RECORD + 60;
        uint32_t mask;
        memcpy(&mask, at, 4);
        if ((mask & LICENSED) && mask != REMOVED) mask &= ~LICENSED, memcpy(at, &mask, 4), n++;
    }
    pc64_log("songs: %u licensed songs open", n);
}

void pc64_songs_install(HMODULE game) {
    BYTE *base = (BYTE *)game;
    if (!locate(base)) return;
    licensed_all();
    const char *data = getenv("HK_PC64_DATA");
    if (!data || !*data) return;
    for (const char *d = data; *d && g_nmods < 16;) {
        const char *end = strchr(d, ';');
        char dir[MAX_PATH];
        snprintf(dir, sizeof dir, "%.*s", (int)(end ? end - d : (int)strlen(d)), d);
        if (MultiByteToWideChar(CP_UTF8, 0, dir, -1, g_mods[g_nmods], MAX_PATH)) g_nmods++;
        d = end ? end + 1 : d + strlen(d);
    }
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    g_image = base, g_image_end = base + nt->OptionalHeader.SizeOfImage;
    if (!(g_data = calloc(g_songs, sizeof *g_data)) || !(g_back = calloc(g_songs, 1)) || !(g_uses = calloc(g_nchara, sizeof *g_uses)) ||
        !(g_banner = calloc(g_songs, 1)))
        return;
    // the music data: the last mod first, so the first one's fields win
    size_t files = 0;
    for (int m = g_nmods - 1; m >= 0; m--) {
        WCHAR **list = NULL;
        size_t n = 0, cap = 0;
        xml_files(g_mods[m], &list, &n, &cap, 0);
        if (n > 1) qsort(list, n, sizeof *list, by_name);
        for (size_t i = 0; i < n; i++) read_xml(list[i]), free(list[i]);
        free(list);
        files += n;
    }
    if (!files) return;
    game_banners();
    // the charts in use, the characters' uses
    unsigned char *used = calloc(g_nchart, 1);
    if (!used) return;
    used[0] = 1;
    for (unsigned id = 0; id < g_songs; id++) {
        const BYTE *rec = g_music + (size_t)id * RECORD;
        for (int k = 0; k < SLOTS; k++) {
            uint16_t c;
            memcpy(&c, rec + 84 + 2 * k, 2);
            if (c < g_nchart) used[c] = 1;
        }
        uint32_t mask;
        memcpy(&mask, rec + 60, 4);
        for (int k = 0; k < 2 && mask != REMOVED; k++) {
            uint16_t c;
            memcpy(&c, rec + 56 + 2 * k, 2);
            if (c && c < g_nchara && g_uses[c] < 65535) g_uses[c]++;
        }
    }
    // the songs left out that the music data has, their charts' files there
    BYTE *recs = malloc((size_t)g_songs * RECORD);
    unsigned *ids = malloc(sizeof *ids * g_songs), n = 0, skipped = 0, nnew = 0;
    struct { unsigned short at; const struct chart *c; } *added = malloc(sizeof *added * g_nchart);
    if (!recs || !ids || !added) return;
    for (unsigned id = 0; id < g_songs; id++) {
        const struct song *s = &g_data[id];
        const BYTE *native = g_music + (size_t)id * RECORD;
        uint32_t mask;
        memcpy(&mask, native + 60, 4);
        if (mask != REMOVED || !s->text[3] || !s->text[4] || !s->text[5]) continue;   // the game's own, or not described
        int present[SLOTS] = {0}, ok = 1;
        for (int k = 0; k < SLOTS; k++) {
            const struct chart *c = &s->chart[k];
            if (!plain_name(c->folder) || !plain_name(c->file)) continue;
            char rel[600];
            snprintf(rel, sizeof rel, "sd/%s/%s.ifs", c->folder, c->file);
            present[k] = data_file(rel) && c->level <= 50 && c->type <= 64 ? 1 : 2;
            if (present[k] == 2 && k != 4 && k != 5 && k != 6) ok = 0;   // a missing chart the song needs
        }
        if (present[1] != 1 || !ok) {   // Normal, always
            if (skipped++ < 20) pc64_log("songs: %u (%s) left out: its charts' files aren't there", id, s->text[4]);
            continue;
        }
        if (!has_banner(id)) {
            if (skipped++ < 20) pc64_log("songs: %u (%s) left out: no banner (tex/system/kc_diff_ifs/kc_%04u.ifs)", id, s->text[4], id);
            continue;
        }
        BYTE *rec = recs + (size_t)n * RECORD;
        memcpy(rec, native, RECORD);
        static const unsigned TEXT_AT[6] = {0, 8, 16, 24, 40, 48};
        for (int i = 0; i < 6; i++)
            if (s->text[i]) memcpy(rec + TEXT_AT[i], &s->text[i], 8);
        characters(s, rec);
        if (s->has & 2) memcpy(rec + 64, &s->folder, 4);
        if (s->has & 4) memcpy(rec + 68, &s->version, 4);
        if (s->has & 8) memcpy(rec + 72, &s->categories, 4);
        // the regions it's played in (bits the music select asks for one of: the game's own songs' 0x17)
        const uint16_t regions = 0x17;
        memcpy(rec + REGIONS_AT, &regions, 2);
        // each chart's tempos (12 bytes from +222: a tempo, how much of the song it lasts in
        // thousandths, twice more): display_bpm's two groups (low and high, the same when it's one)
        for (int k = 0; k < 6 && (s->has & 16); k++) {
            const unsigned short a = s->bpm[k], b = s->bpm[k + 6];
            int16_t t[6] = {0};
            if (a && b && a != b) t[0] = (int16_t)a, t[1] = 500, t[2] = (int16_t)b, t[3] = 500;
            else if (a || b) t[0] = (int16_t)(a ? a : b), t[1] = 1000;
            memcpy(rec + TEMPOS_AT + 12 * k, t, sizeof t);
        }
        // its mask, as the music data has it, but for the bits to unlock it (as popnhax clears them:
        // nothing here unlocks it) and the charts' (set below, for the charts there)
        uint32_t m = ((s->has & 1) ? s->mask : 0) & ~REMOVED & ~DIFFICULTY_BITS & ~LOCKED;
        const unsigned nnew_was = nnew;
        for (int k = 0; k < SLOTS && ok; k++) {
            if (present[k] != 1) continue;
            const struct chart *c = &s->chart[k];
            unsigned at = 0;
            for (unsigned j = 0; j < nnew && !at; j++) {   // the same chart as one already added
                const struct chart *o = added[j].c;
                if (o->type == c->type && o->keys == c->keys && !strcmp(o->folder, c->folder) && !strcmp(o->file, c->file) &&
                    !memcmp(o->audio, c->audio, sizeof c->audio))
                    at = added[j].at;
            }
            for (unsigned j = 1; j < g_nchart && !at; j++)
                if (!used[j]) used[j] = 1, at = j, added[nnew].at = (unsigned short)j, added[nnew++].c = c;
            if (!at) ok = 0;
            const uint16_t at16 = (uint16_t)at;
            memcpy(rec + 84 + 2 * k, &at16, 2);
            rec[76 + k] = (BYTE)c->level;
            rec[214 + k] = (BYTE)c->hold;
            m |= SLOT_BITS[k];
        }
        if (!ok) {   // no room for its charts
            for (unsigned j = nnew_was; j < nnew; j++) used[added[j].at] = 0;
            nnew = nnew_was;
            pc64_log("songs: %u (%s) left out: the game's chart table is full", id, s->text[4]);
            continue;
        }
        memcpy(rec + 60, &m, 4);
        ids[n++] = id;
        // its background: a mod's (files.c gives it from the mod's bg pack folder), else none
        char bg[64];
        int has_bg = 0;
        static const char *const BG[] = {"tex/system/bg_diff_ifs", "tex/system/bg_jka_ifs"};
        for (int b = 0; b < 2 && !has_bg; b++) snprintf(bg, sizeof bg, "%s/bg_%04u.ifs", BG[b], id), has_bg = data_file(bg);
        g_back[id] = !has_bg;
    }
    if (!n) {
        pc64_log("songs: none brought back (%zu files of music data, %u songs without their files)", files, skipped);
        return;
    }
    if (!guard_background(base)) return;
    // in place: the charts, then each song (its mask last: the song is there once it's set)
    DWORD old_m, old_c, ignored;
    VirtualProtect(g_music, (size_t)g_songs * RECORD, PAGE_READWRITE, &old_m);
    VirtualProtect(g_charts, (size_t)g_nchart * 40, PAGE_READWRITE, &old_c);
    for (unsigned j = 0; j < nnew; j++) {
        BYTE *e = g_charts + (size_t)added[j].at * 40;
        const struct chart *c = added[j].c;
        memcpy(e, &c->type, 4), memcpy(e + 4, &c->keys, 4);
        memcpy(e + 8, &c->folder, 8), memcpy(e + 16, &c->file, 8);
        memcpy(e + 24, c->audio, 16);
    }
    MemoryBarrier();
    for (unsigned i = 0; i < n; i++) {
        BYTE *dst = g_music + (size_t)ids[i] * RECORD, *rec = recs + (size_t)i * RECORD;
        memcpy(dst, rec, 60);
        memcpy(dst + 64, rec + 64, RECORD - 64);
        MemoryBarrier();
        memcpy(dst + 60, rec + 60, 4);
    }
    VirtualProtect(g_charts, (size_t)g_nchart * 40, old_c, &ignored);
    VirtualProtect(g_music, (size_t)g_songs * RECORD, old_m, &ignored);
    pc64_log("songs: %u brought back (%u charts added; %u described without their files or banner; %u without a character the game has); "
             "the game's %u songs, %u charts", n, nnew, skipped, g_unknown_charas, g_songs, g_nchart);
}
