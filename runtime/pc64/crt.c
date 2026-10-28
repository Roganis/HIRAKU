// SPDX-License-Identifier: GPL-3.0-or-later
// The single-screen mode (HK_PC64_OUTPUT=crt, PLAN.md section 32): for an older cabinet with one
// 4:3 screen, nine buttons and a 10-key, no touch screen and no big buttons.
//   - The picture: the main screen's middle 4:3 (1440x1080), scaled down on the GPU to 640x480
//     (display.c, to_link); the touch screen isn't shown.
//   - The touches the game waits for, made by button 5: in each of these scenes its presses touch
//     the scene's places in turn (title: START; entry: START without a pass; mode select: OK, then
//     CREDIT START).
//   - The touch screen, shown instead (the 10-key's decimal point, or buttons 1 and 9 together, in
//     and out): its picture at 640x400 with a cursor, moved by the 10-key's 8, 2, 4, 6 (and 7, 9,
//     1, 3) or buttons 4, 6, 3, 7 (up, down, left, right), touching with the 10-key's 5 or button 5.
//     The game gets none of the buttons meanwhile. Whatever the touch screen asks for (the options,
//     the PIN, a name, events) can be done there.
//   - A new card's terms (after its PIN): button 5 touches YES, then OK, and the touch screen is
//     shown for the name. Leaving a scene hides it.
//   - HI-SPEED on buttons 4 (down) and 6 (up) at the options (held: again every 0.2 s), the option
//     editor opened first when it isn't (the main screen then shows the game's preview).
//   - The PIN, typed on the 10-key: the game's pad (on the touch screen) always in its digits'
//     order (it picks one of fourteen orders; the loader has it pick the plain one), each digit a
//     touch on its button, 00 the pad's clear.
//   - The music select in nine-button mode: on entering it, once a credit, its category list is
//     opened (the "play with 9 buttons" panel) and "by title" chosen: every category but "giant
//     pop-kun" (the big buttons' charts, which can't be played here) uses the nine buttons.
//   - The play screen's characters, smaller and in the 4:3 picture's side columns (as pop'n's 4:3
//     games have them): the game moves each to its place each frame (AFP's layer position, which
//     popn.dll imports by ordinal); for its characters' layers the whole matrix is set instead. Their
//     windows are made smaller in the layout (hiraku-pc's single-screen/); the songs' pictures the
//     game puts in them, fitted to them.
//   - The characters' big pictures (the character select's, the music select's, the VS screen's,
//     the result's, the game over's): the game gives them at their own size, which the engine keeps
//     whatever the clip's scale; given as PICTURE_SCALE of it (the newer characters' animated ones
//     are made smaller in the layout).
#include "pc64.h"

#include <stdlib.h>
#include <string.h>

struct place { int x, y; };
static const struct { int scene; int count; struct place at[3]; } SCENES[] = {
    {12, 1, {{640, 470}}},               // title: START
    {26, 1, {{640, 470}}},               // entry: START (no pass)
    {20, 2, {{640, 736}, {640, 350}}},   // mode select: OK, CREDIT START
};
#define MUSIC_SELECT 28
#define RESULT 23
#define CHARACTER_SELECT 21
#define GAME_OVER 27
#define CATEGORIES_X 300                 // the music select's category panel (touch screen, 1280x800)
#define CATEGORIES_Y 100
#define BY_TITLE_X 755                   // ... and in its list, "by title"
#define BY_TITLE_Y 192
#define TITLE 12
#define ENTRY 26                         // the entry: the card, its PIN
#define PIN_ORDER 12                     // of the game's fourteen pad orders, the digits in order: a phone's
static const short PIN_X[3] = {496, 636, 776}, PIN_Y[4] = {126, 264, 406, 544};   // 7-9, 4-6, 1-3, 0 (rows)
#define PIN_CLEAR_X 704
#define PIN_CLEAR_Y 544
#define OPTIONS 22                       // the options before a song (the touch screen's editor):
static const struct place EDIT_OPTIONS = {653, 623}, SPEED_UP = {362, 87}, SPEED_DOWN = {362, 555};
static const struct place TERMS[2] = {{393, 450}, {650, 747}};   // a new card's terms: YES, then OK

// The characters (1080-line coordinates): scaled by CHARA_SCALE about a point at the bottom of the
// 16:9 picture's side columns (x 300 and 1620), that point moved into the 4:3 picture's (x 427 and
// 1493: between its edges at 240 and 1680 and the lanes at 614 and 1306), a little up
#define CHARA_SCALE 0.8f
#define PICTURE_SCALE 0.8f   // the characters' big pictures (their top left kept)
#define CHARA_SCALE_LOST 0.64f   // the lost move (a close-up, wider than the others)
#define CHARA_LOST 2
#define CHARA_FROM_Y 1080.0f
#define CHARA_TO_Y 1052.0f
static const float CHARA_FROM_X[2] = {300, 1620}, CHARA_TO_X[2] = {427, 1493};

int pc64_crt(void) {
    static int on = -1;
    if (on < 0) {
        const char *o = getenv("HK_PC64_OUTPUT");
        on = o && !strcmp(o, "crt");
    }
    return on;
}

// The touches, made on a thread of their own (a touch takes 160 ms: the board's reads go on): a
// place, or two with a second between (a list opened, then a choice in it)
static HANDLE g_wake;
static volatile LONG g_pending;
static struct place g_touches[2];
static int g_count;
static DWORD WINAPI toucher(void *unused) {
    (void)unused;
    for (;;) {
        WaitForSingleObject(g_wake, INFINITE);
        if (!InterlockedExchange(&g_pending, 0)) continue;
        for (int i = 0; i < g_count; i++) {
            if (i) Sleep(1000);
            pc64_touch(g_touches[i].x, g_touches[i].y);
        }
    }
    return 0;
}
static void touch_soon(int count, struct place a, struct place b) {
    if (!g_wake) {
        g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        CreateThread(NULL, 0, toucher, NULL, 0, NULL);
    }
    if (g_pending) return;   // one sequence at a time
    g_touches[0] = a, g_touches[1] = b, g_count = count;
    InterlockedExchange(&g_pending, 1);
    SetEvent(g_wake);
}

// The touch screen's view: on, and its cursor in the touch screen's 1280x800
static volatile int g_view, g_cx = 640, g_cy = 400;
int pc64_crt_view(int *x, int *y) {
    if (x) *x = g_cx;
    if (y) *y = g_cy;
    return g_view;
}

void pc64_crt_cursor(int x, int y) {
    g_cx = x < 0 ? 0 : x > 1279 ? 1279 : x;
    g_cy = y < 0 ? 0 : y > 799 ? 799 : y;
}

#define KEY(n) (1u << (n))           // the 10-key's digits (runtime/pc/link.h)
#define KEY_DECIMAL (1u << 11)
#define VIEW_BUTTONS (PC64_IN_BUTTON(1) | PC64_IN_BUTTON(2) | PC64_IN_BUTTON(3) | PC64_IN_BUTTON(4) | PC64_IN_BUTTON(5) | \
                      PC64_IN_BUTTON(6) | PC64_IN_BUTTON(7) | PC64_IN_BUTTON(8) | PC64_IN_BUTTON(9))

// The cursor moved by what's held: a step at once, then on every 16 ms after a quarter of a
// second, faster the longer it's held
static void move_cursor(unsigned in, unsigned keys) {
    static DWORD since, last;
    const int up = (keys & (KEY(8) | KEY(7) | KEY(9))) || (in & PC64_IN_BUTTON(4));
    const int down = (keys & (KEY(2) | KEY(1) | KEY(3))) || (in & PC64_IN_BUTTON(6));
    const int left = (keys & (KEY(4) | KEY(7) | KEY(1))) || (in & PC64_IN_BUTTON(3));
    const int right = (keys & (KEY(6) | KEY(9) | KEY(3))) || (in & PC64_IN_BUTTON(7));
    const DWORD now = GetTickCount();
    if (!(up | down | left | right)) {
        since = 0;
        return;
    }
    int step;
    if (!since) since = now, last = now, step = 4;
    else if (now - since < 250 || now - last < 16) return;
    else last = now, step = now - since < 750 ? 6 : now - since < 1500 ? 12 : 24;
    int x = g_cx + (right - left) * step, y = g_cy + (down - up) * step;
    g_cx = x < 0 ? 0 : x > 1279 ? 1279 : x;
    g_cy = y < 0 ? 0 : y > 799 ? 799 : y;
}

// What the player does, watched on a thread of its own every 2 ms (a tap the game's board reads
// would miss, when it's busy, is seen): button 5's presses in the scenes above made touches, the
// music select's nine buttons chosen, the touch screen's view
static void watch(unsigned in, unsigned keys) {
    static int scene_seen = -1, step, nine_done;
    static unsigned cards_seen;
    static int editing;            // the option editor opened (by the shortcut, or touched in the view)
    static DWORD speed_at;         // HI-SPEED's last step
    static unsigned in_was = ~0u, keys_was = ~0u;   // nothing pressed at the start
    static DWORD scene_at;
    const unsigned in_down = in & ~in_was, keys_down = keys & ~keys_was;
    in_was = in, keys_was = keys;
    // the view, in and out: the decimal point, or buttons 1 and 9 together
    const unsigned both = PC64_IN_BUTTON(1) | PC64_IN_BUTTON(9);
    if ((keys_down & KEY_DECIMAL) || ((in & both) == both && (in_down & both))) {
        g_view = !g_view;
        pc64_log("crt: the touch screen %s", g_view ? "shown" : "hidden");
        return;
    }
    const int scene = pc64_scene;
    if (scene != scene_seen) {   // a new scene: its first place first, the touch screen hidden
        scene_seen = scene, step = 0, scene_at = GetTickCount(), cards_seen = pc64_cards_given, editing = 0;
        if (scene == TITLE) nine_done = 0;   // a new credit
        if (g_view) g_view = 0, pc64_log("crt: the touch screen hidden (scene %d)", scene);
    }
    if (g_view) {
        move_cursor(in, keys);
        if ((keys_down & KEY(5)) || (in_down & PC64_IN_BUTTON(5))) {
            pc64_log("crt: the touch screen touched at %d,%d", g_cx, g_cy);
            if (scene == OPTIONS) editing = 1;
            touch_soon(1, (struct place){g_cx, g_cy}, (struct place){g_cx, g_cy});
        }
        return;
    }
    if (scene == MUSIC_SELECT && !nine_done && GetTickCount() - scene_at > 1500) {   // once it has come in
        nine_done = 1;
        pc64_log("crt: the music select in nine-button mode (its songs by title)");
        touch_soon(2, (struct place){CATEGORIES_X, CATEGORIES_Y}, (struct place){BY_TITLE_X, BY_TITLE_Y});
    }
    if (scene == OPTIONS) {   // HI-SPEED: a step at the press, then every 0.2 s from 0.4 s held
        static DWORD held_since;
        const unsigned speed = in & (PC64_IN_BUTTON(4) | PC64_IN_BUTTON(6));
        const DWORD now = GetTickCount();
        int go = 0;
        if (!speed) held_since = 0;
        else if (in_down & speed) held_since = now, speed_at = now, go = 1;
        else if (held_since && now - held_since >= 400 && now - speed_at >= 200) speed_at = now, go = 1;
        if (go) {
            const struct place arrow = (speed & PC64_IN_BUTTON(6)) ? SPEED_UP : SPEED_DOWN;
            if (!editing) {
                editing = 1;
                pc64_log("crt: the option editor opened for HI-SPEED");
                touch_soon(2, EDIT_OPTIONS, arrow);
            } else {
                touch_soon(1, arrow, arrow);
            }
        }
    }
    if (scene == ENTRY && (keys_down & 0x7ffu)) {   // the PIN pad's buttons
        struct place p = {PIN_CLEAR_X, PIN_CLEAR_Y};
        for (int d = 0; d <= 9; d++)
            if (keys_down & KEY(d)) p = d ? (struct place){PIN_X[(d - 1) % 3], PIN_Y[2 - (d - 1) / 3]} : (struct place){PIN_X[0], PIN_Y[3]};
        touch_soon(1, p, p);
    }
    if ((in_down & PC64_IN_BUTTON(5)) && scene == ENTRY && pc64_cards_given != cards_seen) {   // a card's: its terms
        const struct place p = TERMS[step % 2];
        pc64_log("crt: button 5 touches %d,%d (a new card's terms)", p.x, p.y);
        touch_soon(1, p, p);
        if (step++ % 2) g_cx = 640, g_cy = 400, g_view = 1, pc64_log("crt: the touch screen shown (the name)");
        return;
    }
    if (in_down & PC64_IN_BUTTON(5))
        for (size_t i = 0; i < sizeof SCENES / sizeof SCENES[0]; i++)
            if (SCENES[i].scene == scene) {
                const struct place p = SCENES[i].at[step % SCENES[i].count];
                step++;
                pc64_log("crt: button 5 touches %d,%d (scene %d)", p.x, p.y, scene);
                touch_soon(1, p, p);
            }
}

static DWORD WINAPI watcher(void *unused) {
    (void)unused;
    for (;;) {
        watch(pc64_held | pc64_link_buttons(), pc64_link_keypad());
        Sleep(2);
    }
    return 0;
}

// The board's inputs as the game reads them (board.c, each status read): none of the buttons while
// the touch screen is shown
unsigned pc64_crt_inputs(unsigned in) {
    if (!pc64_crt()) return in;
    static int started;
    if (!started) started = 1, CreateThread(NULL, 0, watcher, NULL, 0, NULL);
    return g_view ? in & ~VIEW_BUTTONS : in;
}

// The play screen's characters. The game keeps its two players' characters in a table (0xe0 bytes
// each): from +8 the layers (BM2D::CLayer *, its AFP layer number at +8) of the character's ten
// moves (win, win, lost, neutral, neutral, fever, great, good, miss, neutral), at +0xb4 and +0xb8
// the places (two shorts each) it puts them. Found by the code that draws them: lea rax,[table];
// movzx edi,cl; imul rbx,rdi,0xe0; mov r14d,0x352.
typedef int (*set_position_fn)(unsigned layer, const float *at);
typedef int (*set_params_fn)(unsigned layer, unsigned which, const float *values);
static set_position_fn g_set_position;
static set_params_fn g_set_params;
static const BYTE *g_players;

// The move (0-9) LAYER shows of a player's character, or -1
static int character(unsigned layer, const float *at) {
    for (int p = 0; p < 2; p++) {
        const BYTE *player = g_players + p * 0xe0;
        const short *places = (const short *)(player + 0xb4);
        if (!((at[0] == places[0] && at[1] == places[1]) || (at[0] == places[2] && at[1] == places[3])))
            continue;   // (a pooled layer of another scene)
        for (int i = 0; i < 10; i++) {
            const BYTE *l = *(const BYTE *const *)(player + 8 + 8 * i);
            if (l && *(const unsigned *)(l + 8) == layer) return i;
        }
    }
    return -1;
}

static int set_position(unsigned layer, const float *at) {
    const int move = at && layer ? character(layer, at) : -1;
    if (move >= 0) {
        const int right = at[0] >= 960;
        const float s = move == CHARA_LOST ? CHARA_SCALE_LOST : CHARA_SCALE;
        const float m[6] = {s, 0, 0, s, CHARA_TO_X[right] + s * (at[0] - CHARA_FROM_X[right]),
                            CHARA_TO_Y + s * (at[1] - CHARA_FROM_Y)};
        return g_set_params(layer, 0x20, m);   // the matrix: a, b, c, d, tx, ty
    }
    return g_set_position(layer, at);
}

// The songs' pictures in the characters' windows: the game looks up the background layout's clips
// for them by name (AFP: the first of a layer's clips named so, then the next), then gives them their
// texture at its own size; fitted to the clip instead (the layout made the clip smaller)
typedef int (*find_clip_fn)(unsigned layer, const char *name);
typedef int (*next_clip_fn)(int clip, int how);
typedef int (*set_texture_fn)(int clip, const void *texture, char fit);
static find_clip_fn g_find_clip;
static next_clip_fn g_next_clip;
static set_texture_fn g_set_texture;
#define WINDOW_CLIPS 16
static volatile LONG g_window_clips[WINDOW_CLIPS], g_window_next;

static int find_clip(unsigned layer, const char *name) {
    const int first = g_find_clip(layer, name);
    const char *bare = name && *name == '/' ? name + 1 : name;
    if (bare && first >= 0 && (!strcmp(bare, "bg_l_usr") || !strcmp(bare, "bg_r_usr")))
        for (int clip = first, n = 0; clip >= 0 && n < 4; clip = g_next_clip(clip, 6), n++) {
            int known = 0;
            for (int i = 0; i < WINDOW_CLIPS; i++) known |= g_window_clips[i] == clip;
            if (!known) g_window_clips[(InterlockedIncrement(&g_window_next) - 1) % WINDOW_CLIPS] = clip;
        }
    return first;
}

// A character's big picture (the hariai): drawn the size of one of the layouts' shapes for them
// (570x736, 760x1000, 960x1080; its texture's size at +4, its part drawn at +12: u0, u1, v0, v1)
static int big_picture(const void *texture) {
    static const float SIZES[][2] = {{570, 736}, {760, 1000}, {960, 1080}};
    const unsigned *size = (const unsigned *)((const BYTE *)texture + 4);
    const float *uv = (const float *)((const BYTE *)texture + 12);
    const float w = (uv[1] - uv[0]) * size[0], h = (uv[3] - uv[2]) * size[1];
    for (size_t i = 0; i < sizeof SIZES / sizeof SIZES[0]; i++)
        if (w > SIZES[i][0] - 2 && w < SIZES[i][0] + 2 && h > SIZES[i][1] - 2 && h < SIZES[i][1] + 2) return 1;
    return 0;
}

static int set_texture(int clip, const void *texture, char fit) {
    for (int i = 0; i < WINDOW_CLIPS && !fit; i++) fit = g_window_clips[i] == clip;
    const int scene = pc64_scene;   // (the VS screen's pictures are given as the music select ends)
    if (!fit && texture &&
        (scene == CHARACTER_SELECT || scene == MUSIC_SELECT || scene == OPTIONS || scene == RESULT || scene == GAME_OVER) &&
        big_picture(texture)) {
        // its texture's size made smaller for the call: the picture's size is taken from it then
        unsigned *size = (unsigned *)((BYTE *)texture + 4);
        const unsigned w = size[0], h = size[1];
        size[0] = (unsigned)(w * PICTURE_SCALE), size[1] = (unsigned)(h * PICTURE_SCALE);
        const int r = g_set_texture(clip, texture, fit);
        size[0] = w, size[1] = h;
        return r;
    }
    return g_set_texture(clip, texture, fit);
}

void pc64_crt_install(HMODULE game) {
    if (!pc64_crt()) return;
    static const BYTE code[] = {0x0f, 0xb6, 0xf9, 0x48, 0x69, 0xdf, 0xe0, 0x00, 0x00, 0x00, 0x41, 0xbe, 0x52, 0x03, 0x00, 0x00};
    BYTE *base = (BYTE *)game;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !g_players; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_CNT_CODE)) continue;
        const BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - sizeof code;
        for (p += 7; p < end; p++)
            if (p[-7] == 0x48 && p[-6] == 0x8d && p[-5] == 0x05 && !memcmp(p, code, sizeof code)) {
                g_players = p + *(const int32_t *)(p - 4);
                break;
            }
    }
    // The PIN pad's order: the game's pick (a random number modulo 14: imul eax,eax,14; sub r8d,eax;
    // movsxd rax,r8d; lea rcx,[rax+rax*4]; lea rax,[orders]) made PIN_ORDER (mov eax,PIN_ORDER)
    static const BYTE pick[] = {0x6b, 0xc0, 0x0e, 0x44, 0x2b, 0xc0, 0x49, 0x63, 0xc0, 0x48, 0x8d, 0x0c, 0x80, 0x48, 0x8d, 0x05};
    BYTE *pin = NULL;
    sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !pin; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_CNT_CODE)) continue;
        BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - sizeof pick;
        for (; p < end; p++)
            if (!memcmp(p, pick, sizeof pick)) {
                pin = p + 3;
                break;
            }
    }
    if (pin) {
        static const BYTE order[] = {0xb8, PIN_ORDER, 0x00, 0x00, 0x00, 0x90};   // mov eax,PIN_ORDER; nop
        DWORD old;
        VirtualProtect(pin, sizeof order, PAGE_EXECUTE_READWRITE, &old);
        memcpy(pin, order, sizeof order);
        VirtualProtect(pin, sizeof order, old, &old);
        FlushInstructionCache(GetCurrentProcess(), pin, sizeof order);
    } else {
        pc64_log("crt: the PIN pad's order not found: the 10-key can't type on it");
    }
    HMODULE afp = GetModuleHandleA("afp-core.dll");
    g_set_params = afp ? (set_params_fn)(void *)GetProcAddress(afp, MAKEINTRESOURCEA(41)) : NULL;
    if (!g_players || !g_set_params) {
        pc64_log("crt: the play screen's characters not found: left as they are");
        return;
    }
    g_set_position = afp ? (set_position_fn)(void *)GetProcAddress(afp, MAKEINTRESOURCEA(47)) : NULL;
    if (!g_set_position || !pc64_import_ordinal(game, "afp-core.dll", 47, (void *)set_position)) {
        pc64_log("crt: the game's layer positions not found: its characters left as they are");
        return;
    }
    for (int i = 0; i < WINDOW_CLIPS; i++) g_window_clips[i] = -1;
    g_find_clip = (find_clip_fn)(void *)GetProcAddress(afp, MAKEINTRESOURCEA(103));
    g_next_clip = (next_clip_fn)(void *)GetProcAddress(afp, MAKEINTRESOURCEA(106));
    // (the texture call's hook before this one's, files.c's, kept in the chain)
    if (!g_find_clip || !g_next_clip || !(g_set_texture = (set_texture_fn)pc64_import_ordinal(game, "afp-core.dll", 137, (void *)set_texture)) ||
        !pc64_import_ordinal(game, "afp-core.dll", 103, (void *)find_clip))
        pc64_log("crt: the game's clip lookups not found: the songs' pictures left at their size");
    pc64_log("crt: the play screen's characters (table at +0x%llx) scaled by %.2f",
             (unsigned long long)(g_players - base), CHARA_SCALE);
}
