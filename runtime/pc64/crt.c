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
//     opened (the "play with 9 buttons" panel) and "by title" chosen (market J's panel: by genre),
//     then the index's letter it shows chosen: every category but "giant pop-kun" (the big buttons'
//     charts, which can't be played here) uses the nine buttons. Each step once the touch screen's
//     picture (read back, not shown) shows the screen it needs.
//   - Menus on the cabinet's colours, shown on the touch screen's view with the cursor on the item:
//     at the options, green (3, 7) the previous and next option of the FULL OPTION editor, blue (4,
//     6) its value, red (5) done; at the music select, green the categories, blue the previous and
//     next, red the one chosen, then the same for its choices (letters, levels, versions..., found
//     on the picture; the list dragged on past its last row, the cursor's choice found again after
//     by its label). Each touch is checked on the picture and made again if the game let it go.
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

// The touches, made on a thread of their own (a touch takes 160 ms: the board's reads go on):
// sequences of up to four in turn, a second between (a list opened, then a choice in it): touches,
// drags, or a button pressed for the game (PRESS: a.x its number); a few sequences waiting their
// turn (the buttons pressed quicker than the touches go)
enum { TOUCH, DRAG, PRESS };
struct act { struct place a, b; int drag; DWORD wait; };
struct seq { struct act acts[4]; int count; };
#define QUEUE 8
static HANDLE g_wake;
static struct seq g_queue[QUEUE];
static volatile LONG g_queued, g_done;   // sequences put in, and made
static volatile LONG g_pending;          // some waiting or being made
static CRITICAL_SECTION g_queue_lock;
static DWORD WINAPI toucher(void *unused) {
    (void)unused;
    for (;;) {
        WaitForSingleObject(g_wake, INFINITE);
        while (g_done != g_queued) {
            const struct seq q = g_queue[g_done % QUEUE];
            for (int i = 0; i < q.count; i++) {
                if (i) Sleep(q.acts[i].wait);
                if (q.acts[i].drag == DRAG) {
                    pc64_drag(q.acts[i].a.x, q.acts[i].a.y, q.acts[i].b.x, q.acts[i].b.y);
                } else if (q.acts[i].drag == PRESS) {
                    pc64_held |= PC64_IN_BUTTON(q.acts[i].a.x);
                    Sleep(120);
                    pc64_held &= ~PC64_IN_BUTTON(q.acts[i].a.x);
                } else {
                    pc64_touch(q.acts[i].a.x, q.acts[i].a.y);
                }
            }
            Sleep(60);   // the game's frame between two sequences
            InterlockedIncrement(&g_done);
        }
        g_pending = g_done != g_queued;
    }
    return 0;
}
static int run_acts(const struct act *acts, int count) {
    if (!g_wake) {
        InitializeCriticalSection(&g_queue_lock);
        g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        CreateThread(NULL, 0, toucher, NULL, 0, NULL);
    }
    EnterCriticalSection(&g_queue_lock);
    const int full = g_queued - g_done >= QUEUE;
    if (!full) {
        struct seq *q = &g_queue[g_queued % QUEUE];
        for (int i = 0; i < count; i++) q->acts[i] = acts[i];
        q->count = count;
        g_pending = 1;
        InterlockedIncrement(&g_queued);
    }
    LeaveCriticalSection(&g_queue_lock);
    if (!full) SetEvent(g_wake);
    return !full;
}
static void touch_soon(int count, struct place a, struct place b) {
    const struct act acts[2] = {{a, a, TOUCH, 0}, {b, b, TOUCH, 1000}};
    run_acts(acts, count);
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

// The menus, for the cabinet's buttons (green 3 and 7, blue 4 and 6, red 5), shown on the touch
// screen's view (the cursor on the item chosen):
//   - the options (green at the options): the FULL OPTION editor opened, green the previous and next
//     option, blue its value (a slider's arrows; a list's previous or next choice, the one the game
//     shows chosen found by its pink frame), red done;
//   - the categories (green at the music select): their panel opened, blue the previous and next, red
//     the one chosen; then the choices it has (letters, levels, versions...: the buttons found on the
//     picture, the list dragged on past its last or first), blue and red again; green closes.
static int g_menu;            // 0 none, MENU_*
enum { MENU_OPTIONS = 1, MENU_CATEGORIES, MENU_CHOICES };
static int g_item;            // the option, the category
static int g_editing;         // the option editor opened (by the shortcut, a menu or a touch in the view)
static int g_page = -1;       // the editor's page: its FULL OPTION category, or -1 (SIMPLE, or not known)
static DWORD g_menu_at;       // the choices: when their category was touched; a drag: when it was made
static int g_dragged;         // the choices: dragged by (pixels, up < 0), the cursor to be put after it
static int g_steps;           // the choices: blue's steps pressed while the list was moving
static int g_closing;         // the choice touched: the menu closes once it has taken
static const struct place EDIT_FULL = {133, 609}, CLOSE = {117, 107};
static const int FULL_PAGE_X = 328, FULL_PAGE_Y[4] = {206, 307, 408, 509};   // サポート, アレンジ, エリア, ガイド
#define CHOICE_Y_4 {142, 267, 392, 517}
static const struct option {
    const char *name;
    int page, x;
    int up, down;             // a slider's arrows (y)
    int n, y[4];              // a list's choices (y)
    int toggle;               // a slider that's off until its label is touched (HIDDEN, SUDDEN): its label's y
} OPTIONS_LIST[] = {
    {"HI-SPEED", 0, 584, 88, 555, 0, {0}, 0},
    {"JUDGE ADJUST", 0, 785, 88, 555, 0, {0}, 0},
    {"LONG POP", 0, 987, 0, 0, 2, {147, 267}, 0},
    {"POP-KUN", 1, 507, 0, 0, 4, CHOICE_Y_4, 0},
    {"GAUGE", 1, 709, 0, 0, 4, CHOICE_Y_4, 0},
    {"JUDGE+", 1, 909, 0, 0, 4, CHOICE_Y_4, 0},
    {"RANDOM", 1, 1109, 0, 0, 4, CHOICE_Y_4, 0},
    {"HIDDEN", 2, 506, 91, 552, 0, {0}, 617},
    {"SUDDEN", 2, 706, 91, 552, 0, {0}, 617},
    {"LIFT", 2, 906, 91, 552, 0, {0}, 0},
    {"ROOF", 2, 1106, 91, 552, 0, {0}, 0},
    {"GUIDE SE", 3, 709, 0, 0, 4, {144, 270, 395, 520}, 0},
    {"GUIDE SE VOL", 3, 909, 88, 555, 0, {0}, 0},
};
#define NOPTIONS ((int)(sizeof OPTIONS_LIST / sizeof OPTIONS_LIST[0]))
#define OPTION_LABEL_Y 610
// The categories' panel (でっかポップ君, the big buttons', left out)
static const struct place CATEGORY[] = {{518, 187}, {770, 187}, {1025, 187}, {269, 338}, {518, 338},
                                        {770, 338}, {1025, 338}, {269, 490}, {518, 490}, {770, 490}};
#define NCATEGORIES ((int)(sizeof CATEGORY / sizeof CATEGORY[0]))

// The touch screen's picture (half its size), as the view last showed it
static uint32_t g_sub[640 * 400], g_before[640 * 400];
static volatile DWORD g_sub_at;
void pc64_crt_sub_picture(const uint32_t *px, int w, int h) {
    if (w != 640 || h > 400) return;
    memcpy(g_sub, px, sizeof(uint32_t) * (size_t)w * (size_t)h);
    g_sub_at = GetTickCount();
}
static uint32_t sub_px(int x, int y) {   // (x, y) of the 1280x800
    x /= 2, y /= 2;
    return x < 0 || y < 0 || x >= 640 || y >= 400 ? 0xffffff : g_sub[y * 640 + x];
}
// the buttons' edge (dark teal) and the chosen one's frame (pink)
static int teal(uint32_t p) {
    const int r = p >> 16 & 255, g = p >> 8 & 255, b = p & 255;
    return r < 50 && g >= 95 && g <= 145 && b >= 65 && b <= 115;
}
static int pink(uint32_t p) {
    const int r = p >> 16 & 255, g = p >> 8 & 255, b = p & 255;
    return r > 215 && g < 110 && b >= 100 && b <= 180;
}

// The choices' buttons on the picture: rows found down the first column's left edge (x 164), the
// columns by the second's edge (5 across: lefts 160, 360...; 4 across, the versions' logos: 161, 413...)
#define MAX_CELLS 40
static int find_cells(struct place *cell, int *chosen) {
    static const int LEFT5[5] = {160, 360, 560, 760, 960}, LEFT4[4] = {161, 413, 665, 917};
    int rows[12], len[12], nrows = 0, run = -1, longest = 0;
    for (int y = 80; y <= 696 && nrows < 12; y += 2) {
        const int on = teal(sub_px(164, y));
        if (on && run < 0) run = y;
        if ((!on || y == 696) && run >= 0) {
            if (y - run >= 20 && run > 80 && y < 696) {   // (a level button: 45 high)
                rows[nrows] = (run + y) / 2, len[nrows] = y - run;
                if (len[nrows] > longest) longest = len[nrows];
                nrows++;
            }
            run = -1;
        }
    }
    int whole = 0;   // the rows seen whole (one cut by the panel's edge can't be touched)
    for (int r = 0; r < nrows; r++)
        if (len[r] * 10 >= longest * 9) rows[whole++] = rows[r];
    nrows = whole;
    if (!nrows) return 0;
    const int five = teal(sub_px(364, rows[0]));
    const int *left = five ? LEFT5 : LEFT4, ncols = five ? 5 : 4, half = five ? 84 : 107;
    int n = 0;
    *chosen = -1;
    for (int r = 0; r < nrows; r++)
        for (int c = 0; c < ncols && n < MAX_CELLS; c++) {
            if (!teal(sub_px(left[c] + 4, rows[r]))) continue;
            for (int dx = 4; dx <= 14 && *chosen < 0; dx += 2)
                if (pink(sub_px(left[c] - dx, rows[r]))) *chosen = n;
            cell[n++] = (struct place){left[c] + half, rows[r]};
        }
    return n;
}
static int cells_seen(void) {
    struct place cell[MAX_CELLS];
    int chosen;
    return find_cells(cell, &chosen) > 0;
}
// the list's chosen choice (its pink frame), or -1
static int chosen_choice(const struct option *o) {
    for (int i = 0; i < o->n; i++)
        for (int dx = 52; dx <= 74; dx += 2)
            if (pink(sub_px(o->x - dx, o->y[i]))) return i;
    return -1;
}

// HIDDEN's, SUDDEN's value says "OFF" (red letters on its label, below its name)
static int off_label(const struct option *o) {
    int red = 0;
    for (int x = o->x - 26; x <= o->x + 26; x += 2) {
        const uint32_t p = sub_px(x, o->toggle + 15);
        red += (p >> 16 & 255) > 170 && (p >> 8 & 255) < 70 && (p & 255) < 70;
    }
    return red >= 4;
}

static void cursor_to(struct place p) { g_cx = p.x, g_cy = p.y; }

static void options_menu_show(void) {
    const struct option *o = &OPTIONS_LIST[g_item];
    cursor_to((struct place){o->x, OPTION_LABEL_Y});
    struct act acts[4];
    int n = 0;
    if (!g_editing) acts[n++] = (struct act){EDIT_OPTIONS, EDIT_OPTIONS, TOUCH, 0};
    if (g_page != o->page) {
        if (g_page < 0) acts[n++] = (struct act){EDIT_FULL, EDIT_FULL, TOUCH, 700};
        const struct place page = {FULL_PAGE_X, FULL_PAGE_Y[o->page]};
        acts[n++] = (struct act){page, page, TOUCH, 500};
    }
    if (n && run_acts(acts, n)) g_editing = 1, g_page = o->page;
    pc64_log("crt: the options' %s", o->name);
}
static void option_change(int up) {
    const struct option *o = &OPTIONS_LIST[g_item];
    if (o->n) {
        // the choice the game shows chosen, or the one just touched (the picture not caught up yet)
        static int last_item = -1, last_choice;
        static DWORD last_at;
        int now = chosen_choice(o);
        if (last_item == g_item && GetTickCount() - last_at < 1200) now = last_choice;
        int next = now < 0 ? 0 : now + (up ? 1 : -1);
        if (next < 0) next = o->n - 1;
        if (next >= o->n) next = 0;
        touch_soon(1, (struct place){o->x, o->y[next]}, (struct place){0, 0});
        last_item = g_item, last_choice = next, last_at = GetTickCount();
    } else if (o->toggle && off_label(o)) {
        // HIDDEN, SUDDEN: off (its value "OFF" in red under its name) until its label is touched
        const struct place label = {o->x, o->toggle};
        touch_soon(1, label, label);
    } else {
        const struct place arrow = {o->x, up ? o->up : o->down};
        touch_soon(1, arrow, arrow);
    }
}

static void choices_step(int forward) {
    struct place cell[MAX_CELLS];
    int chosen, n = find_cells(cell, &chosen);
    if (!n) return;
    int at = 0, best = 1 << 30;   // the cell the cursor is on
    for (int i = 0; i < n; i++) {
        const int d = abs(cell[i].x - g_cx) + abs(cell[i].y - g_cy);
        if (d < best) best = d, at = i;
    }
    const int next = at + (forward ? 1 : -1);
    if (next >= 0 && next < n) {
        cursor_to(cell[next]);
        return;
    }
    // past the end: the list dragged on two rows (as far as its rows go apart), the cursor put after
    int rowgap = 96;
    for (int i = 1; i < n; i++)
        if (cell[i].y > cell[0].y) {
            rowgap = cell[i].y - cell[0].y;
            break;
        }
    const int dy = forward ? -2 * rowgap : 2 * rowgap;
    const struct place from = {640, forward ? 560 : 260}, to = {640, from.y + dy};
    const struct act drag = {from, to, DRAG, 0};
    if (run_acts(&drag, 1)) {
        memcpy(g_before, g_sub, sizeof g_sub);   // to measure how far it moved (less at the list's ends)
        g_dragged = dy;
        g_menu_at = GetTickCount();
    }
}

// The list still after a drag (it moves as the drag lets go, and glides on): the picture the same
// (its panel) at three of its refreshes 150 ms apart, from a second after the drag was asked for
static int settled(void) {
    static uint32_t last[640 * 400];
    static DWORD last_at;
    static int quiet;
    const DWORD since = GetTickCount() - g_menu_at;
    if (since < 1000) {
        quiet = 0;
        return 0;
    }
    if (g_sub_at - last_at < 150) return 0;
    long diff = 0;
    for (int y = 45; y < 345; y += 3)
        for (int x = 40; x < 600; x += 5) {
            const uint32_t a = last[y * 640 + x], b = g_sub[y * 640 + x];
            diff += abs((int)(a >> 8 & 255) - (int)(b >> 8 & 255));
        }
    memcpy(last, g_sub, sizeof last);
    last_at = g_sub_at;
    quiet = diff < 1500 ? quiet + 1 : 0;
    return quiet >= 2 || since > 4000;
}

// How different a cell's label is in two pictures (its button's middle: opaque, the panel behind
// doesn't show through): the brightness compared, a cell's half width less its edges
static long long cell_diff(const uint32_t *pa, struct place ca, const uint32_t *pb, struct place cb, int half) {
    long long d = 0;
    const int w = half / 2 - 10;
    for (int dy = -9; dy <= 9; dy++)
        for (int dx = -w; dx <= w; dx++) {
            const int xa = ca.x / 2 + dx, ya = ca.y / 2 + dy, xb = cb.x / 2 + dx, yb = cb.y / 2 + dy;
            if (xa < 0 || xb < 0 || ya < 0 || yb < 0 || xa >= 640 || xb >= 640 || ya >= 400 || yb >= 400) return 1LL << 40;
            const uint32_t p = pa[ya * 640 + xa], q = pb[yb * 640 + xb];
            d += abs((int)((p >> 16 & 255) + (p >> 8 & 255) + (p & 255)) - (int)((q >> 16 & 255) + (q >> 8 & 255) + (q & 255)));
        }
    return d;
}

// The music select's screens on the touch screen's picture: its category header (its box's sides),
// the categories' panel (its buttons' left edges)
static int header_seen(void) { return teal(sub_px(117, 100)) && teal(sub_px(345, 100)); }
static int panel_seen(void) { return teal(sub_px(153, 338)) && teal(sub_px(405, 338)) && teal(sub_px(657, 338)); }

// A touch checked: made again (three times at most, 1.2 s apart) until the screen it should bring is
// seen (the game lets some touches go, in its screens' animations)
enum { SEE_PANEL = 1, SEE_NO_PANEL, SEE_NO_CELLS };
static struct place g_check_at;
static int g_check, g_check_tries, g_check_failed;
static DWORD g_check_time;
static int cells_seen(void);
static int seen(int want) {
    switch (want) {
    case SEE_PANEL: return panel_seen();
    case SEE_NO_PANEL: return !panel_seen();
    default: return !panel_seen() && !cells_seen();
    }
}
static void touch_checked(struct place p, int want) {
    touch_soon(1, p, p);
    g_check_at = p, g_check = want, g_check_tries = 1, g_check_failed = 0, g_check_time = GetTickCount();
}
// 1 while a checked touch is waiting for its screen (g_check_failed: given up)
static int checking(void) {
    if (!g_check) return 0;
    const DWORD now = GetTickCount();
    if (g_pending || now - g_check_time < 400 || g_sub_at < g_check_time + 300) return 1;
    if (seen(g_check)) {
        g_check = 0;
        return 0;
    }
    if (now - g_check_time < 1200) return 1;
    if (g_check_tries >= 3) {
        pc64_log("crt: the touch at %d,%d didn't take", g_check_at.x, g_check_at.y);
        g_check = 0, g_check_failed = 1;
        return 0;
    }
    touch_soon(1, g_check_at, g_check_at);
    g_check_tries++, g_check_time = now;
    return 1;
}

// The music select's first index, each step once the screen it needs is seen: its header (the
// panel opened), the panel (the index chosen: market K's by title, J's by genre), the index's letters
// (the one it shows chosen, else the first, touched); given up if a screen isn't seen in time
static volatile int g_auto;      // the step: 0 none, 1 the header, 2 the panel, 3 the letters
static DWORD g_auto_at;          // the step's start
int pc64_crt_wants_sub(void) { return g_auto != 0 || pc64_scene == MUSIC_SELECT; }
static void auto_index(void) {
    const DWORD now = GetTickCount(), since = now - g_auto_at;
    if (g_sub_at < g_auto_at + 200 || checking()) return;   // a picture since the step began; a touch's screen
    if (g_check_failed) {
        pc64_log("crt: the music select's index: left at step %d", g_auto);
        g_auto = 0, g_check_failed = 0;
        return;
    }
    struct place cell[MAX_CELLS];
    int chosen, n;
    switch (g_auto) {
    case 1:   // the header seen: the panel opened
        if (header_seen()) {
            touch_checked((struct place){CATEGORIES_X, CATEGORIES_Y}, SEE_PANEL);
            g_auto = 2, g_auto_at = now;
            return;
        }
        break;
    case 2:   // the panel opened: the index chosen
        touch_checked((struct place){BY_TITLE_X, BY_TITLE_Y}, SEE_NO_PANEL);
        g_auto = 3, g_auto_at = now;
        return;
    case 3:   // the index's letters seen: the one shown chosen touched
        if (since >= 500 && (n = find_cells(cell, &chosen)) > 0) {
            const struct place p = cell[chosen >= 0 ? chosen : 0];
            touch_checked(p, SEE_NO_CELLS);
            pc64_log("crt: the music select's index, its letter at %d,%d", p.x, p.y);
            g_auto = 4, g_auto_at = now;
            return;
        }
        break;
    case 4:   // done
        g_auto = 0;
        return;
    }
    if (since > (g_auto == 1 ? 10000u : 4000u)) {
        pc64_log("crt: the music select's index: step %d not seen, left as it is", g_auto);
        g_auto = 0;
    }
}

// The buttons while a menu is shown
static void menu(unsigned in, unsigned in_down) {
    const unsigned green = PC64_IN_BUTTON(3) | PC64_IN_BUTTON(7), blue = PC64_IN_BUTTON(4) | PC64_IN_BUTTON(6);
    const DWORD now = GetTickCount();
    static DWORD held_since, step_at;
    int go = 0;   // blue's step: at the press, then every 0.2 s from 0.4 s held (the options' sliders)
    if (!(in & blue)) held_since = 0;
    else if (in_down & blue) held_since = now, step_at = now, go = 1;
    else if (g_menu == MENU_OPTIONS && held_since && now - held_since >= 400 && now - step_at >= 200) step_at = now, go = 1;
    const int up = (in & PC64_IN_BUTTON(6)) != 0;
    if (checking()) return;   // a touch waiting for its screen
    if (g_closing || g_check_failed) {   // the choice made (or a touch that didn't take): the menu closed
        if (g_check_failed && g_menu != MENU_OPTIONS) pc64_log("crt: the menu closed");
        g_view = 0, g_menu = 0, g_closing = 0, g_check_failed = 0;
        return;
    }
    switch (g_menu) {
    case MENU_OPTIONS:
        if (in_down & green) {
            g_item = (g_item + ((in_down & PC64_IN_BUTTON(7)) ? 1 : NOPTIONS - 1)) % NOPTIONS;
            options_menu_show();
        } else if (go) {
            option_change(up);
        } else if (in_down & PC64_IN_BUTTON(5)) {
            g_view = 0, g_menu = 0;
            pc64_log("crt: the options' menu closed");
        }
        return;
    case MENU_CATEGORIES:
        if (in_down & green) {
            touch_soon(1, CLOSE, CLOSE);
            g_view = 0, g_menu = 0;
        } else if (go) {
            g_item = (g_item + (up ? 1 : NCATEGORIES - 1)) % NCATEGORIES;
            cursor_to(CATEGORY[g_item]);
        } else if (in_down & PC64_IN_BUTTON(5)) {
            touch_checked(CATEGORY[g_item], SEE_NO_PANEL);
            g_menu = MENU_CHOICES, g_menu_at = now, g_dragged = 0, g_steps = 0;
            pc64_log("crt: the category %d chosen", g_item);
        }
        return;
    case MENU_CHOICES: {
        struct place cell[MAX_CELLS];
        int chosen, n;
        if (g_menu_at && g_dragged && !settled()) return;   // the list still gliding
        if (g_menu_at && now - g_menu_at >= 700 && g_sub_at > g_menu_at + 300) {
            n = find_cells(cell, &chosen);   // its choices shown (or none)
            if (g_dragged) {   // after a drag: the cursor's choice found again (its label), then the next
                int found = -1;
                long long best = 1LL << 40;
                const int half = n > 1 && cell[1].x - cell[0].x > 230 ? 107 : 84;   // a cell's half width (4 or 5 across)
                for (int i = 0; i < n; i++) {
                    const long long d = cell_diff(g_before, (struct place){g_cx, g_cy}, g_sub, cell[i], half);
                    if (d < best) best = d, found = i;
                }
                pc64_log("crt: the list dragged %d: the choice found again at %d,%d", g_dragged, found >= 0 ? cell[found].x : -1,
                         found >= 0 ? cell[found].y : -1);
                if (found >= 0) {
                    const int next = found + (g_dragged < 0 ? 1 : -1);
                    cursor_to(cell[next < 0 ? 0 : next >= n ? n - 1 : next]);
                }
                g_menu_at = 0, g_dragged = 0;
                return;
            }
            if (n) {
                cursor_to(cell[chosen >= 0 ? chosen : 0]);
            } else if (now - g_menu_at >= 2000) {   // a category without choices: done
                g_view = 0, g_menu = 0;
                pc64_log("crt: the category has no choices");
                return;
            } else {
                return;
            }
            g_menu_at = 0, g_dragged = 0;
        }
        if (g_menu_at) {   // waiting for the picture: blue's presses kept for after
            if (go) g_steps += up ? 1 : -1;
            return;
        }
        if (g_steps && !g_pending) {   // ... and made now, one at a time
            const int forward = g_steps > 0;
            g_steps += forward ? -1 : 1;
            choices_step(forward);
            return;
        }
        if (in_down & green) {
            touch_soon(1, CLOSE, CLOSE);
            g_view = 0, g_menu = 0;
        } else if (go) {
            choices_step(up);
        } else if (in_down & PC64_IN_BUTTON(5)) {
            const struct place p = {g_cx, g_cy};
            touch_checked(p, SEE_NO_CELLS);
            g_closing = 1;
            pc64_log("crt: the choice at %d,%d", p.x, p.y);
        }
        return;
    }
    }
}

// What the player does, watched on a thread of its own every 2 ms (a tap the game's board reads
// would miss, when it's busy, is seen): button 5's presses in the scenes above made touches, the
// music select's nine buttons chosen, the touch screen's view
static void watch(unsigned in, unsigned keys) {
    static int scene_seen = -1, step, nine_done;
    static unsigned cards_seen;
    static DWORD speed_at;         // HI-SPEED's last step
    static unsigned in_was = ~0u, keys_was = ~0u;   // nothing pressed at the start
    static DWORD scene_at;
    const unsigned in_down = in & ~in_was, keys_down = keys & ~keys_was;
    in_was = in, keys_was = keys;
    // the view, in and out: the decimal point, or buttons 1 and 9 together
    const unsigned both = PC64_IN_BUTTON(1) | PC64_IN_BUTTON(9);
    if ((keys_down & KEY_DECIMAL) || ((in & both) == both && (in_down & both))) {
        g_view = !g_view, g_menu = 0;
        pc64_log("crt: the touch screen %s", g_view ? "shown" : "hidden");
        return;
    }
    const int scene = pc64_scene;
    if (scene != scene_seen) {   // a new scene: its first place first, the touch screen hidden
        scene_seen = scene, step = 0, scene_at = GetTickCount(), cards_seen = pc64_cards_given, g_editing = 0, g_page = -1, g_menu = 0;
        g_auto = 0, g_check = 0, g_check_failed = 0, g_closing = 0;
        if (scene == TITLE) nine_done = 0;   // a new credit
        if (g_view) g_view = 0, pc64_log("crt: the touch screen hidden (scene %d)", scene);
    }
    if (g_view && g_menu) {
        menu(in, in_down);
        return;
    }
    const unsigned green = PC64_IN_BUTTON(3) | PC64_IN_BUTTON(7);
    if (!g_view && (in_down & green) && scene == OPTIONS) {   // the options' menu
        g_menu = MENU_OPTIONS, g_item = 0, g_view = 1;
        options_menu_show();
        return;
    }
    if (!g_view && (in_down & green) && scene == MUSIC_SELECT && !g_pending && !g_auto && header_seen()) {   // the categories' menu
        touch_checked((struct place){CATEGORIES_X, CATEGORIES_Y}, SEE_PANEL);
        g_menu = MENU_CATEGORIES, g_item = 0, g_view = 1;
        cursor_to(CATEGORY[0]);
        pc64_log("crt: the categories' menu");
        return;
    }
    if (g_view) {
        move_cursor(in, keys);
        if ((keys_down & KEY(5)) || (in_down & PC64_IN_BUTTON(5))) {
            pc64_log("crt: the touch screen touched at %d,%d", g_cx, g_cy);
            if (scene == OPTIONS) g_editing = 1, g_page = -1;
            touch_soon(1, (struct place){g_cx, g_cy}, (struct place){g_cx, g_cy});
        }
        return;
    }
    if (scene == MUSIC_SELECT && !nine_done && GetTickCount() - scene_at > 1500) {   // once it has come in
        nine_done = 1;
        pc64_log("crt: the music select in nine-button mode (its songs by title)");
        g_auto = 1, g_auto_at = GetTickCount();
    }
    if (g_auto) auto_index();
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
            if (!g_editing) {
                g_editing = 1, g_page = -1;
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
