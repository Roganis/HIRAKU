// SPDX-License-Identifier: GPL-3.0-or-later
// Songs back from the player's music data (PLAN.md section 35, step 5b), as runtime/pc64/songs.c
// does for the 64-bit game (section 34): popn22.dll keeps a slot in its music table for each song it
// ever had, those it left out marked so (mask 0x80000000); the mods' music data (popnhax's XML:
// <music id> with its texts, characters and <chart idx> files) fills them in again, each song whose
// charts' files are there (the mods' folders, laid over the game's data by files.c, or the game's
// own), before the game starts. Nothing of the game's comes with HIRAKU: the songs, their texts and
// charts are the player's.
//
// The tables, in the program's data, found by the code that reads them:
//   - the music table: 172 bytes a song, by id: the six texts' pointers at +0 (fw_genre, fw_title,
//     fw_artist, genre, title, artist), the two characters at +24, the mask at +28, the folder at
//     +32, the version at +36, the categories at +40, six charts' levels at +44, their chart numbers
//     at +50, their tempos at +140 (two groups of six), their hold flags at +164. Its characters'
//     getters (cwde; imul eax,eax,0xac; mov ax,[eax+X]; ret; movsx ecx,ax; imul ecx,ecx,0xac;
//     mov ax,[ecx+X-2]; ret) and its id check (cmp ax,-85; jle; mov ecx,N; cmp cx,ax: ids -85..N-1);
//   - the charts: 32 bytes each (folder and file pointers, audio_param1-4, file_type, used_keys), and
//     their count, from a reader (mov edx,N; cmp ax,dx; jge; movsx ebx,ax; shl ebx,5; ...; add ebx,T).
//     It's full; a song brought back takes the entries of the charts no song uses (the left-out
//     songs' own: their counts are the per-chart records' too, so the table can't grow);
//   - the characters: 76 bytes each, by number, the name's pointer at +4, from a reader (mov edx,N;
//     cmp ax,dx; jge; movsx ebx,ax; imul ebx,ebx,0x4c; ...; add ebx,T).
// The six charts: Easy, Normal, Hyper, EX, Battle Normal, Battle Hyper; the game asks the mask for
// Easy's (0x80000), Hyper's (0x1000000), EX's (0x2000000) and Battle Hyper's (0x4000000).
#include "pc32.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECORD 172u
#define CHART 32u
#define CHARA 76u
#define REMOVED 0x80000000u
#define SLOTS 6
static const char *const SLOT_NAMES[SLOTS] = {"ep", "np", "hp", "op", "bp_n", "bp_h"};
#define DIFFICULTY_BITS 0x07080000u
#define LOCKED 0x080200c0u   // the bits to unlock a song (0x40, 0x80: Hyper's and EX's, the game's checks)
static const unsigned SLOT_BITS[SLOTS] = {0x00080000u, 0, 0x01000000u, 0x02000000u, 0, 0x04000000u};

static BYTE *g_music, *g_charts, *g_charas;   // g_charas: character 0's name pointer
static unsigned g_songs, g_nchart, g_nchara;

// --- the code's patterns: 0x100 any byte. Where it is (the one place: NULL when none, or several)
#define X 0x100
static BYTE *find(BYTE *base, const short *pat, size_t n) {
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    BYTE *hit = NULL;
    int count = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_CNT_CODE)) continue;
        BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - n;
        for (; p < end; p++) {
            size_t k = 0;
            while (k < n && (pat[k] == X || p[k] == pat[k])) k++;
            if (k == n) hit = p, count++;
        }
    }
    return count == 1 ? hit : NULL;
}
static uint32_t u32(const BYTE *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static int locate(BYTE *base) {
    static const short count[] = {0x66, 0x83, 0xf8, 0xab, 0x7e, X, 0xb9, X, X, 0, 0, 0x66, 0x3b, 0xc8};
    static const short charas_of_song[] = {0x98, 0x69, 0xc0, 0xac, 0, 0, 0, 0x66, 0x8b, 0x80, X, X, X, X, 0xc3,
                                           0x0f, 0xbf, 0xc8, 0x69, 0xc9, 0xac, 0, 0, 0, 0x66, 0x8b, 0x81, X, X, X, X, 0xc3};
    static const short charts[] = {0xba, X, X, 0, 0, 0x66, 0x3b, 0xc2, 0x0f, 0x8d, X, X, X, X, 0x0f, 0xbf, 0xd8, 0xc1, 0xe3, 0x05,
                                   0x50, 0x8b, 0xf1, 0x81, 0xc3};
    static const short charas[] = {0xba, X, X, 0, 0, 0x66, 0x3b, 0xc2, 0x0f, 0x8d, X, X, X, X, 0x0f, 0xbf, 0xd8, 0x6b, 0xdb, 0x4c,
                                   0x50, 0x8b, 0xf1, 0x81, 0xc3};
    BYTE *p;
    if ((p = find(base, count, sizeof count / sizeof *count))) g_songs = u32(p + 7);
    if ((p = find(base, charas_of_song, sizeof charas_of_song / sizeof *charas_of_song)) && u32(p + 10) == u32(p + 27) + 2)
        g_music = (BYTE *)(uintptr_t)(u32(p + 27) - 24);
    if ((p = find(base, charts, sizeof charts / sizeof *charts))) g_nchart = u32(p + 1), g_charts = (BYTE *)(uintptr_t)u32(p + 25);
    if ((p = find(base, charas, sizeof charas / sizeof *charas))) g_nchara = u32(p + 1), g_charas = (BYTE *)(uintptr_t)u32(p + 25);
    if (!g_music || !g_charts || !g_charas || g_songs < 1000 || g_songs > 32768 || g_nchart < 1000 || g_nchart > 65535 || g_nchara < 100 ||
        g_nchara > 65535) {
        pc32_log("songs: the game's tables not found (music %p %u, charts %p %u, characters %p %u): none brought back", (void *)g_music,
                 g_songs, (void *)g_charts, g_nchart, (void *)g_charas, g_nchara);
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
    unsigned mask, folder, version, categories, has;   // has: bit 0-3 the numbers given, 4 the tempos
    unsigned short bpm[12];   // display_bpm: each chart's tempos, two groups of six
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
        char *raw = keep(gt + 1, (size_t)(c - gt - 1));
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
    const long v = strtol(t, &e, 10);
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
    // UTF-8 unless it says Shift-JIS, and then too when its bytes are UTF-8 (Shift-JIS text is almost
    // never valid UTF-8)
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
        if (id >= g_songs) continue;   // a song past the game's table (the console songs, custom ones): not yet
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
        pc32_log("songs: %u in %s", count, name);
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

// --- the files a song needs: in a mod's folder (over the game's data), or the game's own (data\,
// in the folder the loader runs in)
static WCHAR g_mods[16][MAX_PATH];
static int g_nmods;
static int data_file(const char *rel) {   // rel: under the game's data folder, Shift-JIS
    WCHAR w[MAX_PATH], path[MAX_PATH * 2];
    if (!MultiByteToWideChar(932, 0, rel, -1, w, MAX_PATH)) return 0;
    for (WCHAR *c = w; *c; c++)
        if (*c == L'/') *c = L'\\';
    for (int i = 0; i <= g_nmods; i++) {
        if (i < g_nmods) _snwprintf(path, MAX_PATH * 2, L"%ls\\%ls", g_mods[i], w);
        else _snwprintf(path, MAX_PATH * 2, L"data\\%ls", w);
        const DWORD a = GetFileAttributesW(path);
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) return 1;
    }
    return 0;
}
static int plain_name(const char *s) { return s && *s && !strpbrk(s, "/\\:") && !strstr(s, ".."); }

// --- the banners: a song brought back needs one (the game's packs, tex/system/kc*.ifs, name theirs
// kc_NNNN inside; or a mod's tex/system/kc_diff_ifs/kc_NNNN.ifs)
static unsigned char *g_banner;   // by id
static void game_banners(void) {
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW(L"data\\tex\\system\\kc*.ifs", &d);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        WCHAR path[MAX_PATH * 2];
        _snwprintf(path, MAX_PATH * 2, L"data\\tex\\system\\%ls", d.cFileName);
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
    char rel[64];
    snprintf(rel, sizeof rel, "tex/system/kc_diff_ifs/kc_%04u.ifs", id);
    return g_banner[id] || data_file(rel);
}

// --- the characters: by name, the one the game's own songs use most (a name can be several), the
// pair of the two side by side when the song names both
static unsigned short *g_uses;
static unsigned g_unknown_charas;   // songs none of whose characters the game has
static const BYTE *g_image, *g_image_end;
static int chara_is(unsigned i, const char *name) {
    const char *p;
    memcpy(&p, g_charas + (size_t)i * CHARA, sizeof p);
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
    // a character the game hasn't (an old song's own): the song's other one, else the game's first
    if (c[0] < 0) c[0] = c[1];
    if (c[1] < 0) c[1] = c[0];
    if (c[0] < 0 && (s->chara[0] || s->chara[1])) g_unknown_charas++;
    for (int k = 0; k < 2; k++)
        if (c[k] >= 0) {
            const uint16_t v = (uint16_t)c[k];
            memcpy(rec + 24 + 2 * k, &v, 2);
        }
}

// --- the charts: a song's chart takes an entry already the same, else one no song uses: one with
// the same file's name (the left-out song's own entry, its folder an old game's), else one whose own
// file the game's data hasn't (a dead one: entries no song uses can still be the game's own, read by
// number, as the tutorial's)
static unsigned char *g_used;   // by chart: a song uses it
static const char *chart_text(const BYTE *e, int at) {
    const char *p;
    memcpy(&p, e + at, sizeof p);
    return (const BYTE *)p >= g_image && (const BYTE *)p < g_image_end ? p : "";
}
static int same_chart(const BYTE *e, const struct chart *c) {
    int32_t audio[4];
    uint32_t type, keys;
    memcpy(audio, e + 8, 16), memcpy(&type, e + 24, 4), memcpy(&keys, e + 28, 4);
    return !strcmp(chart_text(e, 0), c->folder) && !strcmp(chart_text(e, 4), c->file) && !memcmp(audio, c->audio, 16) &&
           type == c->type && keys == c->keys;
}
// the game's own file of a chart (data\sd\<folder>\<file>.ifs)
static int game_file(const char *folder, const char *file) {
    char rel[MAX_PATH];
    snprintf(rel, sizeof rel, "data\\sd\\%s\\%s.ifs", folder, file);
    const DWORD a = GetFileAttributesA(rel);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static const struct chart *g_new[65536];   // by chart: what a free entry is to become (written at the end)
static unsigned chart_for(const struct chart *c) {
    for (unsigned j = 1; j < g_nchart; j++)
        if ((g_used[j] && !g_new[j] && same_chart(g_charts + (size_t)j * CHART, c)) || (g_new[j] && g_new[j] == c)) return j;
    for (unsigned j = 1; j < g_nchart; j++)   // another song's chart brought back, the same
        if (g_new[j] && !strcmp(g_new[j]->folder, c->folder) && !strcmp(g_new[j]->file, c->file) &&
            !memcmp(g_new[j]->audio, c->audio, 16) && g_new[j]->type == c->type && g_new[j]->keys == c->keys)
            return j;
    for (int pass = 0; pass < 2; pass++)
        for (unsigned j = 1; j < g_nchart; j++) {
            if (g_used[j]) continue;
            const BYTE *e = g_charts + (size_t)j * CHART;
            if (pass ? game_file(chart_text(e, 0), chart_text(e, 4)) : strcmp(chart_text(e, 4), c->file)) continue;
            g_used[j] = 1, g_new[j] = c;
            return j;
        }
    return 0;
}

void pc32_songs_install(HMODULE game) {
    const char *data = getenv("HK_PC32_DATA");
    if (!data || !*data) return;
    for (const char *d = data; *d && g_nmods < 16;) {
        const char *end = strchr(d, ';');
        char dir[MAX_PATH];
        snprintf(dir, sizeof dir, "%.*s", (int)(end ? end - d : (int)strlen(d)), d);
        if (MultiByteToWideChar(CP_UTF8, 0, dir, -1, g_mods[g_nmods], MAX_PATH)) g_nmods++;
        d = end ? end + 1 : d + strlen(d);
    }
    BYTE *base = (BYTE *)game;
    if (!locate(base)) return;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    g_image = base, g_image_end = base + nt->OptionalHeader.SizeOfImage;
    if (g_nchart > sizeof g_new / sizeof *g_new || !(g_data = calloc(g_songs, sizeof *g_data)) ||
        !(g_uses = calloc(g_nchara, sizeof *g_uses)) || !(g_banner = calloc(g_songs, 1)) || !(g_used = calloc(g_nchart, 1)))
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
    // the charts in use, the characters' uses (the songs the game lists, and its special ones before
    // song 0: ids -85..-1)
    g_used[0] = 1;
    for (int id = -85; id < (int)g_songs; id++) {
        const BYTE *rec = g_music + (ptrdiff_t)id * (ptrdiff_t)RECORD;
        uint32_t mask;
        memcpy(&mask, rec + 28, 4);
        if (mask & REMOVED) continue;
        for (int k = 0; k < SLOTS; k++) {
            uint16_t c;
            memcpy(&c, rec + 50 + 2 * k, 2);
            if (c < g_nchart) g_used[c] = 1;
        }
        for (int k = 0; k < 2; k++) {
            uint16_t c;
            memcpy(&c, rec + 24 + 2 * k, 2);
            if (c < g_nchara && g_uses[c] < 65535) g_uses[c]++;
        }
    }
    // the songs left out that the music data has, their charts' files there
    BYTE *recs = malloc((size_t)g_songs * RECORD);
    unsigned *ids = malloc(sizeof *ids * g_songs), n = 0, skipped = 0, full = 0;
    if (!recs || !ids) return;
    for (unsigned id = 0; id < g_songs; id++) {
        const struct song *s = &g_data[id];
        const BYTE *native = g_music + (size_t)id * RECORD;
        uint32_t mask;
        memcpy(&mask, native + 28, 4);
        if (!(mask & REMOVED) || !s->text[3] || !s->text[4] || !s->text[5]) continue;   // the game's own, or not described
        int present[SLOTS] = {0}, ok = 1;
        for (int k = 0; k < SLOTS; k++) {
            const struct chart *c = &s->chart[k];
            if (!plain_name(c->folder) || !plain_name(c->file)) continue;
            char rel[600];
            snprintf(rel, sizeof rel, "sd/%s/%s.ifs", c->folder, c->file);
            present[k] = data_file(rel) && c->level <= 50 && c->type <= 64 ? 1 : 2;
            if (present[k] == 2 && k != 4 && k != 5) ok = 0;   // a missing chart the song needs (the battle ones may be)
        }
        if (present[1] != 1 || !ok) {   // Normal, always
            if (skipped++ < 20) pc32_log("songs: %u (%s) left out: its charts' files aren't there", id, s->text[4]);
            continue;
        }
        if (!has_banner(id)) {
            if (skipped++ < 20) pc32_log("songs: %u (%s) left out: no banner (tex/system/kc_diff_ifs/kc_%04u.ifs)", id, s->text[4], id);
            continue;
        }
        BYTE *rec = recs + (size_t)n * RECORD;
        memcpy(rec, native, RECORD);
        for (int i = 0; i < 6; i++)
            if (s->text[i]) memcpy(rec + 4 * i, &s->text[i], 4);
        characters(s, rec);
        if (s->has & 2) memcpy(rec + 32, &s->folder, 4);
        if (s->has & 4) memcpy(rec + 36, &s->version, 4);
        if (s->has & 8) memcpy(rec + 40, &s->categories, 4);
        if (s->has & 16) memcpy(rec + 140, s->bpm, sizeof s->bpm);   // the charts' tempos, two groups of six
        // its mask, as the music data has it, but for the bits to unlock it (as popnhax clears them:
        // nothing here unlocks it) and the charts' (set below, for the charts there)
        uint32_t m = ((s->has & 1) ? s->mask : 0) & ~REMOVED & ~DIFFICULTY_BITS & ~LOCKED;
        for (int k = 0; k < SLOTS && ok; k++) {
            if (present[k] != 1) continue;
            const unsigned at = chart_for(&s->chart[k]);
            if (!at) ok = 0;
            const uint16_t at16 = (uint16_t)at;
            memcpy(rec + 50 + 2 * k, &at16, 2);
            rec[44 + k] = (BYTE)s->chart[k].level;
            rec[164 + k] = (BYTE)s->chart[k].hold;
            m |= SLOT_BITS[k];
        }
        if (!ok) {   // no free chart entry left: not this song (its charts' entries stay taken)
            if (full++ < 5) pc32_log("songs: %u (%s) left out: no chart entry free for it", id, s->text[4]);
            continue;
        }
        memcpy(rec + 28, &m, 4);
        ids[n++] = id;
    }
    // the charts the music data adds to songs the game lists (Omnimix's Easy charts): a slot the game
    // asks a mask bit for (Easy, Hyper, EX, Battle Hyper) that the song hasn't, its chart's file there
    unsigned *kept = malloc(sizeof *kept * g_songs), nkept = 0, added = 0;
    BYTE *kept_recs = malloc((size_t)g_songs * RECORD);
    if (!kept || !kept_recs) return;
    for (unsigned id = 0; id < g_songs; id++) {
        const struct song *s = &g_data[id];
        const BYTE *native = g_music + (size_t)id * RECORD;
        uint32_t mask;
        memcpy(&mask, native + 28, 4);
        if (mask & REMOVED) continue;
        BYTE *rec = kept_recs + (size_t)nkept * RECORD;
        memcpy(rec, native, RECORD);
        int more = 0;
        for (int k = 0; k < SLOTS; k++) {
            const struct chart *c = &s->chart[k];
            if (!SLOT_BITS[k] || (mask & SLOT_BITS[k]) || !plain_name(c->folder) || !plain_name(c->file) || c->level > 50 || c->type > 64)
                continue;
            char rel[600];
            snprintf(rel, sizeof rel, "sd/%s/%s.ifs", c->folder, c->file);
            if (!data_file(rel)) continue;
            const unsigned at = chart_for(c);
            if (!at) continue;
            const uint16_t at16 = (uint16_t)at;
            memcpy(rec + 50 + 2 * k, &at16, 2);
            rec[44 + k] = (BYTE)c->level;
            rec[164 + k] = (BYTE)c->hold;
            mask |= SLOT_BITS[k];
            more++;
        }
        if (!more) continue;
        memcpy(rec + 28, &mask, 4);
        kept[nkept++] = id;
        added += (unsigned)more;
    }
    if (!n && !nkept) {
        pc32_log("songs: none brought back (%zu files of music data, %u songs without their files or banner)", files, skipped);
        return;
    }
    // in place (the program's data, before it runs): the charts, then each song
    unsigned charts = 0;
    for (unsigned j = 1; j < g_nchart; j++) {
        const struct chart *c = g_new[j];
        if (!c) continue;
        BYTE *e = g_charts + (size_t)j * CHART;
        memcpy(e, &c->folder, 4), memcpy(e + 4, &c->file, 4), memcpy(e + 8, c->audio, 16);
        memcpy(e + 24, &c->type, 4), memcpy(e + 28, &c->keys, 4);
        charts++;
    }
    for (unsigned i = 0; i < n; i++) memcpy(g_music + (size_t)ids[i] * RECORD, recs + (size_t)i * RECORD, RECORD);
    for (unsigned i = 0; i < nkept; i++) memcpy(g_music + (size_t)kept[i] * RECORD, kept_recs + (size_t)i * RECORD, RECORD);
    free(kept), free(kept_recs);
    char some[256] = "";
    for (unsigned i = 0, at = 0; i < n && i < 12 && at < sizeof some - 8; i++) at += (unsigned)snprintf(some + at, sizeof some - at, " %u", ids[i]);
    pc32_log("songs: the first brought back:%s", some);
    free(recs), free(ids);
    pc32_log("songs: %u brought back, %u charts added to %u of the game's (%u chart entries given to them; %u described without their "
             "files or banner; %u without a chart entry; %u without a character the game has); the game's %u songs, %u charts",
             n, added, nkept, charts, skipped, full, g_unknown_charas, g_songs, g_nchart);
}
