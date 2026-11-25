# Mods

A mod changes a game without changing its data: HIRAKU lays it over the game as the game runs.
Mods are off unless chosen, a game with mods keeps its saves apart from the clean game's, and the
regression cases always run without them (PLAN.md section 20).

What a mod can do is the same on every board, because it works on the parts HIRAKU emulates:

| layer | what | how |
|---|---|---|
| **memory** (`[[patch]]`) | code and data in the game's RAM | bytes at an address, put there whenever the bytes the mod expects are there (so a patch waits for code loaded later, and comes back if it's loaded again) |
| **media** (`[[bytes]]`, `[[file]]`) | anything the game reads from its discs or disks | the bytes it reads at an offset, or a whole file (by its path on an ISO 9660 disc, or on a Konami Python 1 disk), served from the mod's own files |
| **instructions** (`[[hook]]`, the mod API) | a register or word of RAM just before an instruction runs | a fixed change (by a number or a setting the player moves with a key), or a count; Firebeat for now (below) |

What it changes is the game's business: which file holds the song list, which function draws the
notes. A mod for one game carries that knowledge; the layers don't need any.

A mod ships only what it adds: new text, songs, pictures, code. Never the game's own data: a
mod's files are the mod author's, and its patches name the bytes they replace so they only apply
to the game they were made for.

## Where mods live

`<saves>/mods/<game>/<mod>/` (the saves' folder: `user/data` in a portable package, else
`~/.local/share/hiraku`, `%LOCALAPPDATA%\hiraku` on Windows), one folder per mod: its `mod.toml`
and the files it names, or the game's files at their own paths (below), or both. Mods that come
with HIRAKU are in `games/<game>/mods/<mod>/` (pop'n music 9-14's `smooth-notes`); a folder of the
player's with the same name wins. The launcher lists both under a game's **Mods…** (right-click
the game), in order, and **Settings…** there opens the chosen mod's settings (below); the command
line takes `--mod FOLDER`, several in order. Where two mods
change the same thing, the first one wins, as with ifs_layeredfs (whose mods are listed by name). That
holds for memory patches too: of two `[[patch]]`es that expect the same bytes, the first mod's goes
in; one with no `original` (written every frame) is left out where a mod before it patches, and
the log says so.

## Codes a mod types (`macros.txt`)

A mod's folder can hold a `macros.txt`, the same format as a game's own
(`games/<game>/macros.txt`, described in `runtime/host/macro.h`): button codes typed for the
player. With the mod on, its macros come after the game's, the mods' in their order, both in the
game (Ctrl+1 to Ctrl+9 type the first nine) and on the launcher's **Unlocks…** page, where a
macro with an `at` line can be ticked to be typed at every start. A mod made of codes alone needs
no `mod.toml`.

## The game's files at their own paths (as ifs_layeredfs)

The PC-based BEMANI games are modded with [ifs_layeredfs](https://github.com/mon/ifs_layeredfs):
a mod is a folder of the game's files at the paths the game has them, laid over its data, and a
picture inside one of the game's archives can be a plain PNG named after it, which ifs_layeredfs
converts. HIRAKU's mods do the same on the arcade boards' discs and disks:
- a file in a mod's folder at the path of one of the game's files is that file: on a Konami
  Python disk (pop'n music 14's `image/256_title/system.idx`), or on an ISO 9660 disc
  (`DATA/TITLE.BIN`, case doesn't count). No mod.toml is needed: a folder of files is a mod,
  named after the folder;
- a PNG at a picture's path with `.png` for its own extension is that picture
  (`image/kc/kc_001.png` for `image/kc/kc_001.gzz`): converted as the mod loads, to the game's
  picture of the same size and kind (pop'n music 14's 16-bit pictures as they are, its 8-bit
  ones to the nearest colours of their folder's palette, which stays the game's). The PNG must
  be the picture's size; any PNG that isn't interlaced will do;
- the mod's `mod.toml`, the files its tables name, and hidden files and folders (a dot first)
  are left out, and so is anything that matches none of the game's files (the log says how
  many, and the first).

Getting the files out to start from: `tools/popnfs.py IMAGE pngs [FOLDER...] OUT` writes the
pictures (all, or a folder's: `image/256_title`) as PNGs at their paths in OUT; copy the ones
edited into a mod's folder at the same paths. `tools/popnfs.py IMAGE extract NAME... OUT` writes
other files as the game uses them. What ifs_layeredfs has that isn't here: `.merged.xml` (the
arcade games' data isn't XML), the `_ifs` folders (their archives aren't IFS), and the allow and
block lists (the launcher's ticks do that).

## Text

The simplest way is `[[text]]`: the string as the game shows it and its replacement, as popnhax's
translation dictionaries have them for the later pop'n music games. HIRAKU finds it as a whole
string (a zero byte before and after it, so a longer string holding the same words isn't
changed) and puts the replacement there, the rest zeros. Its room is the string, its zero and the
zeros after it, up to the next string; a replacement that doesn't fit is refused (the log says
how much room there is), and one string must keep a zero at its end. Where it's looked for:
- on the PC games, in the program as Windows loads it, once at the start (`from` and `to`, at the
  program's addresses, narrow it); every program date of a game finds its own place, so one mod
  serves all of them;
- on the emulated machines, in the range `from`..`to` of the main CPU's memory (required), once a
  second until it's there (a program loaded later is found when it comes), then kept in place as
  a `[[patch]]` with what was found as its `original`.

A text found in several places is refused unless `every = true` (or a narrower range): the same
words can be a menu's and a file name's. The log says each one found (`mods: ... patch at ...`).

`[[patch]]` at an address does the same for one place, and any bytes. `tools/modtext.py`
writes them: `tools/modtext.py strings DUMP [LO HI] > text.tsv` lists the strings in a RAM dump
(the Python core's `HK_PY_EE_DUMP=FRAME:FILE`; any core's memory saved from address 0) with their
room (the string, its zero and the zeros after it), Japanese or ASCII with a space unless `--all`;
give the lines to translate a fourth column, and `tools/modtext.py mod GAME text.tsv > mod.toml`
makes the patches, in the game's Shift-JIS, padded with zeros. A translation that doesn't fit its
room is refused. The screen's font must have the characters: pop'n music 14's boot checks
(`PCB CHECK` at 0x01904cc0) are drawn in an ASCII-only font, its game screens in a Japanese one.
Much of what a player reads on pop'n music 14 is pictures (the menus' lettering), changed as
above.

Which pop'n music game has the most text to translate this way (surveyed 2026-10-21, RAM dumps at
each game's song select, screens compared with the strings): **pop'n music 11**, then 10 and 9.
Their song selects draw the genre comment, the line under the song list, from the program's
text. pop'n music 12 to 14 show pictures there (banners, cards), and the Firebeat games (4 to 8)
have little text beyond test mode; Animelo 2 carries its songs' lyrics. What was learned on
pop'n music 11:
- the song table is 0x38-byte records from 0x3283fc (451: POPS, then `S_BANA_000`... in order), each
  pointing to its banner name (+0x00), its genre (+0x24) and its comment (+0x28); the strings sit
  in a pool around 0x32e000-0x350000, padded with zeros to 8 bytes, so a translation may take the
  string's room (`modtext.py` measures it);
- the comment line takes plain ASCII, but every character gets a full-width cell (about 32 to
  the line); it types itself out, then scrolls as a ticker, so a comment may be as long as its
  room: twice as many letters as the Japanese it replaces;
- the genre list beside it, the song titles and the title bar during play are pictures; the
  table's genre names aren't what the list shows;
- OSUSUME mode's quiz is text too: 359 records of 0x1ec bytes from 0x2e3138 (the last two,
  from 0x30df58, are its closing questions), each a question in 0x80 bytes, a u32 answer count
  and five answers of 0x40 bytes followed by 8 bytes of data; then a pool of 149 answers 0x42
  bytes apart from 0x30e330 (one question draws from it; its own text is at 0x2cbaf0). The
  question is one line of wide cells, about 27 letters, with no wrapping or scrolling (a
  newline is dropped); the answers use a narrow font, about 12 letters to a box, and a longer
  one spills past its edges, readable. The button select's two captions are text as well
  (0x2e2ed8, 0x2e2ef0);
- its pictures aren't in pop'n music 14's file system (`tools/popnfs.py` finds no directory on
  its disk) but in packs: the textures (u32 size and LZSS, as pop'n 14's, unencrypted, of the
  same "GC" pictures), then an index: u32 size, u16 1, u16 the textures' count, u32 0x1a0, their
  names from 0x20 (32 bytes each: `/MS/0.GCZ`), then at 0x1a0 the sprites' names and, 8-aligned
  after them, a rectangle each (four u16s: x, y, w, h, the textures stacked 512 rows apart).
  `HK_PY_DISK_LOG` shows which packs a screen reads. The mode select's lettering is in `/CMN`
  (from 0x6378c00: STAGE, SELECT, DESIDE, SKIP_T, the courses' names CN_*) and `/MS` (0xd50b600:
  the cards; 0xd533200: caption strips that pop'n 11 doesn't seem to show). A changed picture
  goes back as `[[bytes]]` at its texture's place, packed no longer than the original: an
  optimal-parse LZSS packs Konami's own textures about 2 % smaller, room enough for new
  lettering. The lettering matches best in the game's own font, which the game draws its live
  text with: `/256_FONT1/0.GCZ` (pack index at 0x220c800, 8-bit, palette at the index's end),
  16x16 cells in Shift-JIS order (0x8140 at row 2; digits row 9, A-Z rows 10-11, a-z rows
  12-13), palette 0 clear, 193-207 the black outline, 210-255 the yellow body by coverage;
  trimmed to each glyph's ink it sets English proportionally. Few edge levels (four) pack
  smaller than smooth ones, which matters when a texture's room is tight;
- the song select's genre list is pictures: `/256_S_BANA1` (index 0x25bd600, six 8-bit
  textures from 0x2558e00, one strip of 104x26 cells cut every 512 rows, so some cells span two
  textures). Cell S_BANA_n is the n-th record of the song table (POPS, the one before
  S_BANA_000's, is 0). Each cell keeps its own colours (a body colour, a fringe at the strokes'
  left and right ends), and the game recolours them as it draws, so redrawn lettering takes the
  cell's own colours, every pixel the nearest of those the cell already uses;
- OSUSUME's pictures (instructions board, station sign, answer strips L01-L05, the difficulty
  arrow, "this one!", the curry levels) are `/OS` (index 0xf53e800, five 16-bit textures from
  0xf4ad000);
- found by a marker mod: every Japanese string tagged `#n` and the game played through. Tag
  only what lies in the data (4-byte aligned, three or more Japanese characters, from
  0x200000): a few code bytes read as Japanese, and changing those hangs the boot.

pop'n music 10 and 9 work the same way (the same comment line and the same OSUSUME quiz records
and answer pool, many of the texts pop'n music 11's own, so its translations carry over by text):
- pop'n music 10's song table is 0x48-byte records around 0x34fc70 (390, 349 comments), each
  pointing to its genre, banner name and comment, then 0x10 further its `S_BANA_n`; its comment
  line is pop'n music 11's (full-width cells, then a ticker);
- pop'n music 9's is 0x60-byte records from 0x2351b0 (324, the last a stub), each starting with
  a word, then `0x01fe0000 | index`, with the genre at +0x38, the banner name at +0x3c and the
  comment at +0x40. Its newest songs' strings sit right after the table, away from the older
  ones. Its comment line uses a narrower font: more letters fit;
- both games' menus, captions and genre lists are pictures, in pop'n music 11's packs (the
  same index and LZSS textures; pop'n music 9 names them its own way: `/CMN`, `/FONT`,
  `/BANA_S`; pop'n music 10 `/256_CMN`, `/256_FONT`, `/256_S_BANA1`), so the same picture work
  applies;
- pop'n music 10's pictures, done for its English mod: the genre labels (`/256_S_BANA1` and 2,
  122x40 pills, black lettering over the song's logo, faded and dithered; the song table holds
  Konami's own English name for every genre), the interface's lettering (`/256_CMN` at the mode
  select and `/256_CMN2` from boot: the scroll's instructions, the course names, the bottom bar's
  captions), the caption strips (`/MS`, `/BS`: pop'n music 11's `/MS/1` strips, shown here) and
  OSUSUME's (`/OS`). Some sprites are drawn with another palette than the pack's own (the bottom
  bar's captions are): redrawn, a sprite keeps to the palette entries it already uses. The
  palettes have no partly clear colours, so a lettering's outer edge is hard; anti-aliasing goes
  between the ink, its edge and its outline. `HK_PY_DISK_LOG` finds a screen's textures: the run
  of reads that ends at a pack's index;
- pop'n music 9's packs index differently: u32 size, u16 version (0x200 and up), u16 the
  textures' count, then four offsets; texture names from 0x18 and no sprite names: the sprites
  are the numbered rectangles (four u16s each) after the first offset's u32 count. Its menus
  (`/MS`, `/CMN`), genre labels (`/BANA_S`, 122x34 cells, cell 0 POPS, cell i+1 the song table's
  record i) and OSUSUME (`/OS`, the question heading put together from big single characters)
  are all 16-bit pictures.

A translation mod's patches carry the Japanese they replace (that's how they know their game), so
such a mod is Konami's text too: share it as a mod, never in HIRAKU's own folders.

A game with mods on keeps its saves in a folder of its own, named after the mods
(`<game>+<mod>+<mod>`), started as a copy of the clean game's saves the first time. A mod's
settings don't change which: the same mods with another timing or another set of files chosen
(below) keep the same saves.

## mod.toml

```toml
name = "Sample"                 # shown in the launcher
version = "1.0"
game = "popn14"                 # the game it's for: games/<id>
author = "..."
description = "What it does, in a line or two."

[[patch]]                       # bytes in the game's memory
address = 0x0191cdb8            # where, as the core's mem() takes it (the main CPU's address)
# module = "popn.dll"           # the PC games: address is an offset in this module of the game's
original = "46524545 20504c41 59"   # hex: what must be there (spaces are ignored)
replace  = "4d4f4444 45442120 21"   # hex: what goes there (as long as original)
# once = true                   # apply the first time only (default: whenever original is there again)
# while_rate = true             # in place only while a [[rate]] is on, original back otherwise
# between_ticks = true          # ... and then only on the vsyncs between the cabinet's ticks (below)
# per_picture = true            # a float of one cabinet frame's time (no replace): the rate's picture's while it's on
# Without `original`, replace is written every frame, whatever is there (a cheat: MAME's and
# PCSX2's write their values so); with once = true, only the first frame. Careful with that: from
# the very first frame means while the game is still loading or unpacking its code there too,
# and pop'n music 14's unpacker copies earlier bytes forward, spreading a write made too early
# into its neighbours. With `original`, a patch waits for the bytes it was made for. Bytes are in the
# machine's own order: big-endian on DJ Main (68000) and Firebeat (PowerPC), little-endian on
# System 573, Twinkle and Konami Python (MIPS).

[[text]]                        # a string of the game's, replaced wherever it is (no address)
find = "CREDITS"                # the game's text, written here as UTF-8 (the game's is Shift-JIS)
replace = "COINS"               # what goes in its place: no longer than its room (below)
# from = 0x00100000             # where to look (the emulated machines need it: their program's
# to   = 0x00400000             #   text, so a copy in a loading buffer isn't taken for it)
# every = true                  # each place it's found (default: it must be in one place only)

[[rate]]                        # the machine's vsync at another rate while the game is somewhere
hz = "display"                  # the display's refresh rate (no window: 119.88), a number (30-1000),
                                # or "x2" to "x8": that many times the machine's own rate
while_changing = 0x018ff668     # ... while this word of its memory changes from frame to frame
# while_equal = 0x00f898d8      # ... and (optional) while this word (4 bytes) holds `value`
# value = 4
# chart_times = 2               # PC IIDX 9th style to GOLD: the chart files' times read this many times (= hz's "x")

[[bytes]]                       # bytes the game reads from one of its media
medium = "hdd"                  # the medium's key in dumps/<game>/machine.txt
offset = 0x1234000              # where in it (as the image reads: chdman's extracted bytes)
from = "data/blob.bin"          # the mod's file (its length is how much is replaced)

[[file]]                        # a whole file on an ISO 9660 disc (CDs and DVDs)
medium = "cd_master"
path = "DATA/TITLE.BIN"
from = "files/title.bin"        # as long as the original, or shorter (the rest is zeros)

[[file]]                        # or on a Konami Python 1 disk (pop'n music 14 and its family)
medium = "hdd"
path = "image/256_title/0.gzz"  # the game's name for it (tools/popnfs.py list), or #hash
from = "files/0.gzz"            # the file as the game uses it (tools/popnfs.py extract): any length
```

Numbers are decimal or `0x` hex. A mod is checked when it's loaded: an unknown key, a `game` other
than the one being started, a patch whose `original` and `replace` differ in length, or a
missing `from` file stops the game with a message saying which.

## Hooks, settings and actions (the mod API)

A patch changes the game's code; a hook leaves it as it is and does one small thing just before an
instruction runs, every time it runs: a register or a word of RAM set, raised or lowered, by a
number or by a setting the player moves with a key while playing, or a count kept. That's enough
for a timing mod (the times the game judges presses by, shifted by a setting in milliseconds),
without a HIRAKU of one's own.

```toml
name = "Judging shift"
game = "bm3final"
api = 1                                  # the mod API this mod is written for (it's 1)
requires = ["hook", "settings", "actions"]   # what it uses: checked against what this HIRAKU has
program = "1b5bd916011107a82f5324558f60c1cbb144c0e3"   # (optional) the program it was made for

[[setting]]                     # a number the player sets, kept from one game to the next
name = "shift"                  # letters, digits, _ and -
label = "Judging (ms)"          # shown when it changes
min = -100
max = 100
default = 0
step = 1                        # what an action moves it by (default 1)

[[action]]                      # a key or button of the player's that moves a setting
name = "later"
binding = "key:Keypad 8"        # a bindings file's spelling: key:..., joy0:button3, joy[NAME]:hat0 up...
setting = "shift"
do = "up"                       # "up" or "down" by its step (held: again and again), or "toggle"
                                # (from min to max and back)
# notify = false                # (optional) no box over the game when it moves the setting: for a
                                # mod that shows its own values (needs "quiet" in requires)

[[hook]]                        # before the instruction at `at` runs:
at = 0x80012340                 # the program's own address (as a disassembler shows it)
sub = "r3"                      # set = / add = / sub = a register by the machine's own name,
                                # or memory: an address, with size = 1, 2 or 4 (bytes, default 4)
value = "setting:shift"         # a whole number, or a setting's value
# scale = 0.06                  # the value times this, rounded (milliseconds to 60ths of a second)

[[hook]]
at = 0x80012400
count = "best"                  # or: a counter (said in the log at the end)
when = "r3"                     # (optional) only while this register (or memory: an address and
min = 10                        # when_size) is from min to max, signed; either may be left out
max = 10
```

The rules, which stay as they are for every HIRAKU with mod API 1 (a later one only adds keys,
tables and capabilities; one that changes a rule is API 2, and a HIRAKU says which it has):

- **A hook runs before its instruction**, each time that instruction runs, whichever way the code
  runs: translated ahead of time or interpreted (a function a `[[patch]]` changed, code the
  analysis didn't find, `--interp-only`). The instruction then runs on what the hook left. Hooks
  at one address run in the mods' order (the first mod's first), then in their order in the file.
  A hook's change to a register or to RAM is the game's from then on; a hook doesn't change code
  (a patch does).
- **Registers by the machine's own names**: Firebeat (PowerPC 403) `r0`-`r31`, `cr`, `xer`,
  `lr`, `ctr`. Values are 32 bits; `add`/`sub` wrap; `when` compares signed (a memory word by its
  size).
- **Memory in the machine's own order**: big-endian on Firebeat, as `[[patch]]`'s bytes. An
  address is the program's (Firebeat's 0x80000000 and up).
- **Settings live in the saves' folder**: `<saves>/mod-settings/<game>/<mod folder>.txt` (see
  *Where mods live*), lines of `name=value`, and each action's binding as
  `action.<name>=key:...`, which the player may change there while the game isn't running (the
  file is written when a setting changes, as saves are). A value outside min..max is taken as the
  nearest end. A changed setting shows for two seconds in a box over the game (its mod, its label
  and value) and in the log. An action with `notify = false` (a mod that draws its values itself;
  `requires` lists `quiet`) moves, keeps and logs its setting the same way, with no box; only a
  setting a `when` names still says, once it changed, that its files and patches change from the
  game's next start, which no mod can show. `notify = true` is the default, and needs nothing. `--mod-setting NAME=VALUE` (`MOD/NAME=VALUE`, MOD the mod's folder's
  name, where two mods have the name) sets one for a run without keeping it: scripts and tests.
- **An action never takes one of the game's controls**: where the game has the same key or button,
  the game keeps it and the log says so once.
- **Checked at start, refused with a message**: `api` above this HIRAKU's ("this mod is for mod
  API 2, this HIRAKU's is 1"); a capability in `requires` that this core or build hasn't ("this
  mod needs hook; this core/build has none (it has: settings, actions)"); a `[[hook]]`,
  `[[setting]]`, `[[action]]`, `[[files]]`, `choices` or a table's `when` in a mod without
  `api = 1`, or not listed in its `requires` (`hook`, `settings`, `actions`, `choose`, and `quiet`
  for an action's `notify = false`); an action's `notify` other than true or false; a program other than `program`'s ("this mod is for the program
  whose SHA-1 is ...; this run's (...) is ..."); a register the machine hasn't; and a hook at an
  address in none of the program's functions (it would never run).
- `program` is the SHA-1 of the whole program image the run loads (`dumps/<game>/program.bin` on
  Firebeat, or `--image`), the same as `games/<game>/game.toml`'s `[program] sha1` and
  `sha1sum`'s.

`--mod-api` (any game's program) prints the API version, the capabilities, the registers, the
byte order and the program's SHA-1, and exits:

```
$ hiraku-firebeat --game bm3final --mod-api
mod API 1
capabilities: hook settings script input choose memory actions quiet draw
registers: r0 r1 ... r31 cr xer lr ctr
memory: big-endian
program: 1b5bd916011107a82f5324558f60c1cbb144c0e3 (.../dumps/bm3final/program.bin)
```

Which programs have what: `settings`, `script`, `input` and `choose` every one; `memory` every one whose
machine is emulated (not the PC games, whose memory is their own process's); `actions` (and
`quiet` with it) and `draw` every one with a window (all of a release's); `hook` only `hiraku-firebeat`, the executor
(PLAN.md section 27), which builds the program's functions at start and puts a hook's step in
before its instruction. A Firebeat game's
own binary (`hiraku-bm3final`, built from source with the game's code compiled in) has no hooks:
given a mod that needs them, it starts `hiraku-firebeat` from its own folder in its place, with
`--game` and the same options (on Windows, where a release has only `hiraku-firebeat` anyway, it
says which program to start). A game whose run passes a hook runs as it would without it, frame
for frame, when the hook changes nothing (a setting at 0): no cycle moves; the executor only stops
remembering the runs of an exception's handler that pass one (each run runs its hooks). With no
mod nothing is added: no step, no check on the way.

### Scripts

What a fixed hook can't do, a script can: a Lua 5.4 file in the mod's folder, with functions the
game calls on its frames, on the controls it's given, on hooked instructions, and to draw over its
picture or beside it. A timing mod's counters, a cover over part of the playfield, panels in the
bars beside a 4:3 picture: all in a `mod.toml` and a `mod.lua`, on a release's program. The
sample in `tests/mods/sample-script` (made-up addresses) has each.

```toml
name = "Timing panel"
game = "bm3final"
api = 1
requires = ["hook", "script", "input", "draw", "memory", "settings"]
script = "mod.lua"                       # a file in the mod's folder
# ... [[setting]]s "shift" and "cover" as above

[[hook]]
at = 0x80012340
call = "on_judge"                        # a function of the script's, in place of set/add/sub/count
```

```lua
local early, late = 0, 0

function on_judge(regs, at)              -- before the instruction at `at`
  regs.r3 = regs.r3 - hk.setting("shift")
  local off = hk.read32(0x80123450, true)   -- (made up) signed
  if off < 0 then early = early + 1 else late = late + 1 end
end

function on_input(name, down, time)      -- "P1 Button 3", true/false, the machine's seconds
end

function on_frame()                      -- once a game frame
end

function draw_game(g)                    -- in the game picture's pixels
  g:rect(100, 0, 200, hk.setting("cover"), 0x404040)
end

function draw_window(w)                  -- in the window's pixels, after the picture mode
  if w.picture_x > 0 then
    w:rect(0, 0, w.picture_x, w.height, hk.rgb(16, 20, 28))
    w:text(8 * w.unit, 8 * w.unit, "EARLY " .. early, 0xff6060, 2 * w.unit)
  end
end
```

**Capabilities** (`requires`): `script` for `script =`, and for a hook's `call` with `hook`;
`input` for `on_input`; `draw` for `draw_game` and `draw_window`; `memory` for `hk.read*` and
`hk.write*`. A script that defines a callback its `requires` doesn't allow is refused at start.

**The callbacks** (each optional, global functions):

- `on_frame()`: once a game frame, after the frame's controls and the frame's input script or
  macro steps.
- `on_input(name, down, time)`: each button the game is given, pressed (`down` true) or let go,
  by its name in the game's control table (as a bindings file names it: "P1 Button 3", "1 Player Start";
  `runtime/shell/cabinet.cpp` has each cabinet's), after the player's bindings: from a key or a controller, an input script (`--script`) or a macro. `time` is
  the machine's time in seconds (as `hk.time()`) when the game was given it. They come before the
  frame's `on_frame`, in order; only changes (a button given again as it was isn't one); analog
  controls (a turntable's or a slider's position) aren't given.
- `function(regs, at)` named by a `[[hook]]`'s `call`: before the instruction at `at`, as the
  hook rules above. `regs.r3` reads a register by the machine's own name (a 32-bit value, 0 and
  up), `regs.r3 = v` writes it (taken as 32 bits: -1 is 0xffffffff); a name the machine hasn't is
  an error. `regs` is only good during that call. `when`, `min` and `max` still apply.
- `draw_game(g)`: once a game frame, where the game has a window. `g.width`, `g.height`: the
  game's picture (Firebeat 640 x 480). What it draws is laid over the window's copy of the picture,
  before the picture mode (a CRT shader takes it with the picture), and over smooth motion's
  in-between pictures (`--smooth-motion`) too.
- `draw_window(w)`: likewise, in the window's own pixels, after the picture mode, under HIRAKU's
  own boxes. `w.width`, `w.height`: the window; `w.picture_x`, `w.picture_y`, `w.picture_w`,
  `w.picture_h`: where the picture is in it (the bars beside a 4:3 picture on a wide screen are
  `0 .. picture_x` and `picture_x + picture_w .. width`); `w.unit`: whole times a 360-line screen
  (2 at 720 lines, 3 at 1080), for sizes that follow the window. Only in HIRAKU's OpenGL
  window (every program with a window, unless it fell back to SDL's renderer: then the game layer
  shows and this one doesn't).

**Drawing** (methods of `g` and `w`; x, y in pixels from the top left, numbers taken down to whole
ones; a colour is `0xRRGGBB` (opaque), `0xAARRGGBB`, or `hk.rgb(r, g, b [, a])`, alpha 0-255):

- `:rect(x, y, width, height, colour)`: a filled rectangle.
- `:box(x, y, width, height, colour [, thickness])`: its outline (1 pixel by default).
- `:line(x1, y1, x2, y2, colour [, thickness])`: a line, both ends in it.
- `:text(x, y, text, colour [, scale])`: the 8x8 font of HIRAKU's own panels (ASCII; anything else
  shows as `?`), `scale` whole times (1-16); its width in pixels back. `:text_width(text [, scale])`
  without drawing. At most 256 characters.
- Later drawing goes over earlier, each mod's over the mods' after it in the list. Up to 4096
  drawings a layer a frame. Drawing functions draw nothing else: `hk.write*` and `hk.set_setting`
  are errors there.

**hk** (the one table a script is given):

| | |
|---|---|
| `hk.read8(a)`, `hk.read16(a)`, `hk.read32(a)` | the machine's memory at `a`, in its own byte order, 0 and up; `hk.read16(a, true)` signed; nil where `a` isn't plain memory (as `[[patch]]`'s addresses) |
| `hk.write8(a, v)`, `hk.write16(a, v)`, `hk.write32(a, v)` | written (taken as that many bits); false where it isn't plain memory. The machine's code there is translated again, as for a patch; don't write code from a hook's call |
| `hk.setting(name)` | a `[[setting]]` of this mod's, now |
| `hk.set_setting(name, v)` | set (clamped to its ends) and kept, as an action does; its value back (one a `when` names: its files and patches from the next start) |
| `hk.counter(name)` | a `[[hook]]` `count`'s count (0: none of that name) |
| `hk.frame()` | the game frame's number |
| `hk.time()` | the machine's time, seconds since power-on (never the PC's clock) |
| `hk.log(text)` | a line in the log (`print` too) |
| `hk.notice(text)` | a line on the window for three seconds, under the mod's name (and the log) |
| `hk.rgb(r, g, b [, a])` | a colour |
| `hk.api` | the mod API's version, 1 |
| `hk.has(name)` | true where this core and build have that capability (`--mod-api`'s list), else false: what a mod.toml can't ask. Older builds have no `hk.has`: `hk.has and hk.has("quiet")` |

**The sandbox**: Lua 5.4's base library less `dofile`, `loadfile` and `require` (`load` takes text
only; `collectgarbage` only `"count"`; `print` goes to the log), and `string` (less `dump`),
`table`, `math` and `utf8`. Nothing of `io`, `os`, `package`, `debug` or `coroutine`: no files, no
clock, nothing outside the game. `math.random` starts from a fixed seed, and `math.randomseed`
takes one only with a number; a script's string hashes are fixed too, so with the same controls a
run does the same each time (the order `pairs` gives for keys that are tables or functions is
the one thing that isn't: don't let it decide what is written).

**Limits**: a callback may take a million Lua instructions (a hook's call 100,000; the file as it
first runs, ten million), and a script hold 32 MB. Over either, or an error in a callback (a
`pcall` in the script that catches it doesn't stop that), and **the script is stopped**: the log
says why and where ("mods: Timing panel: its script is stopped, the game goes on: over its budget
of 1000000 instructions (in on_frame)"), the window says so, and the game plays on without it
(its hooks with `call` do nothing from then on; the mod's other hooks and patches stay). An error
as the file **first runs** (at start, before the machine: memory isn't there yet, nor the
settings as kept: read them from `on_frame` on) refuses the mod, with the file and line.

**What stays the game's own**: the picture a run's digests, snapshots, input scripts' pictures and
**recordings (`--record`, F6) have no mod drawing in them**: only the window's copy has it. With no
mod nothing changes; with a script, its callbacks run on the machine's thread, between its frames
(and before its hooked instructions), so two runs with the same input script give the same digests.

At exit the log has a line for each script: its calls, the most instructions one took, its memory
at most, and why it stopped if it did.

Not there yet (later versions of the API): hooks on the other cores (DJ Main, System 573, Twinkle,
the Konami Python and PC games), pictures and sounds from a mod's files, files chosen again while
the game runs (a change applies from the next start, below).

### Files and patches chosen by a setting (`choices`, `when`, `[[files]]`)

A mod can carry more than one way of doing something and let the player choose: two sets of
charts, a picture in two styles, a patch on or off. The player chooses with a setting, in the
launcher (**Mods…**, the mod, **Settings…**) or with an action's key; the mod's tables say which
setting value each file and patch goes with.

```toml
name = "Charts in two timings"
game = "iidx16"
api = 1
requires = ["settings", "choose"]

[[setting]]
name = "timing"
label = "Backing track timing"
choices = ["Arcade", "PS2"]     # the values' names: 0 and 1 (min and max are theirs)
default = "Arcade"              # by name or by number (default: the first)

[[files]]                       # a folder of the mod's, laid over the game's files as the mod's own
folder = "alt/ps2-timing"       #   folder is (alt/ps2-timing/sd_data/1234/1234.1 is the game's
when = "timing=PS2"             #   sd_data/1234/1234.1), only while the setting has this value

[[patch]]                       # any table but [[hook]], [[setting]] and [[action]] takes a `when`
address = 0x00401000
original = "75"
replace = "eb"
when = "timing=1"               # a value by number works too
```

- **`choices`** names a `[[setting]]`'s values, 0 to one less than their count; `min` and `max`
  aren't given then, `default` and `--mod-setting`, the settings file and a `when` take a name or
  a number, and the name is what is shown: the box over the game, the log, the launcher. The
  settings file keeps the number. A setting from 0 to 1 without names is a tick box in the
  launcher, any other a number box with its `min`, `max` and `step`.
- **`when = "SETTING=VALUE"`** on `[[patch]]`, `[[text]]`, `[[bytes]]`, `[[file]]`, `[[rate]]`
  and `[[files]]`: the table is read and checked as any (an unknown key, a missing `from` file
  still refuse the mod), and used only while the setting has that value. A `[[hook]]`'s `when` is
  its own (a register or a word of memory, above): a hook follows a setting with
  `value = "setting:NAME"`. A `when` naming a setting the mod hasn't, or a value outside its
  range or not among its `choices`, refuses the mod with the file's line.
- **`[[files]] folder = "..."`**: a folder inside the mod's, laid over the game's media as the
  mod's own folder is (replaced files, new files where the medium takes them, pictures as PNGs on
  pop'n music 14's disk), its files **winning over the mod's own** at the same path. Of two
  `[[files]]` that hold at once, the first in the file wins. The files under a folder that a
  `[[files]]` names, chosen or not, are that folder's only: never laid at their own path in the mod
  (`alt/ps2-timing/...` isn't the game's `alt/...`), and neither are the files a table's `from`
  names, used or not. Without `when`, the folder is laid always.
- **When**: the values are those the run starts with: the settings file, then `--mod-setting`,
  read before any `when` is looked at. The game opens its files whenever it likes, so they aren't
  changed while it runs: a setting that a `when` names, changed while playing (an action's key,
  `hk.set_setting`), is kept and applies from the next start; the box over the game says so
  ("Files and patches: from the game's next start") and the log too.
- **Between mods** nothing changes: the first mod in the list wins, whatever its settings.
- **Saves**: a mod's settings don't make a save folder of their own (the saves are kept per set
  of mods that are on, not per setting).
- **Media**: every one the loader lays files on: Konami Python 1 disks and ISO 9660 discs (the
  emulated boards), the PC games' data folder through hiraku-pc (replaced and, for IIDX 9th style
  to EMPRESS and the ee'MALL station, new files), and the two pop'n whose own loader lays the mods'
  folders (the 64-bit pop'n and the last 32-bit one): they are handed each chosen `[[files]]`
  folder as a mod folder of its own just before its mod's, and leave every `[[files]]` folder out
  of their search for music data (`.xml`); their games never ask for a file at a path such as
  `alt/...`.
- **Older HIRAKUs** (0.13.0 and before) don't know `[[files]]`, `choices` or `when` on these
  tables and refuse a mod that has them ("unknown table", "unknown key"), and one that lists
  `choose` in `requires` with "this mod needs choose": never laid wrong. A mod that uses them
  lists `choose`.

**The launcher's panel** (**Mods…**, a mod, **Settings…**, for a mod that is on or not): a row a
setting, its label and a tick box, a list or a number box, the default marked; the actions' keys
under them, read-only ("Keypad 8: Lift higher (Lift (pixels) up by 10)"); **Defaults**, OK,
Cancel. It reads the mod.toml with the loader's own reader (`runtime/host/modsettings.h`), so a
mod the game would refuse says why there, and writes the same settings file the game keeps,
every other line in it as it was (the actions' bindings, a setting the mod no longer has). The
Mods dialog shows the chosen mod's settings under its description.

## High frame rates (`[[rate]]`)

A game draws one picture per vsync. `[[rate]]` makes the vsync come `hz` times a second of the
machine's own time while a word of the game's memory keeps changing (two frames in a row: on;
two still: back to the machine's rate). `hz = "display"` follows the display's refresh rate, so
the game's frames meet the display's one for one (display pacing then shows each for one
refresh). `hz = "x2"` (to `"x8"`) is a whole multiple of the machine's own rate as it is (a PC
IIDX's monitor check's: 59.935 or 60.035 Hz), for a game whose mod counts in shorter frames itself
(beatmania IIDX EMPRESS, below), where only an exact multiple keeps its music with its notes.
`chart_times = 2` (with `hz = "x2"`) has a PC IIDX of 9th style to GOLD read its chart files with
every time doubled (below). The CPUs keep their clocks, so each frame has less time; a game that keeps up draws
more pictures a second. What it computes from a clock (a song's position, timing windows) stays
right and moves more smoothly; what it counts in frames (animations, effects, menu timers) would
run faster, which is why the rate is tied to a condition, and what `between_ticks` is for.
(The same condition, in a game's own game.toml as `[smooth]`, says when a song is played for smooth
motion in songs only: docs/SMOOTH.md.)

A game that moves its animations on by a time each picture, a constant of one cabinet frame,
gets its picture's own time with `per_picture` on that float's patch (`original` its 4 bytes, no
`replace`): while the rate is on it holds the original times the cabinet's rate over the rate's
(half of it at 120 Hz, a quarter at 240), the original again when the rate is off.

The cabinet's ticks: while a rate is on, HIRAKU keeps count of the machine's time, and a vsync
that completes another 1/59.94 s is a tick (every second one at 119.88 Hz, every fourth at 239.76,
5 in 12 at 144). A `[[patch]]` with `between_ticks` is in place on the vsyncs between ticks only:
an increment patched to +0 there advances once per cabinet frame at any rate. Patches are put
in place and taken out between frames (the Python core clears the EE recompiler's blocks over
them each time).

Above 90 Hz the Python core hands over the previous vsync's picture (as `--picture-overlap`), so
the PS2's GS draws while its EE runs on: waiting for each vsync's own picture took 2.4 ms of a
4.2 ms frame at 240 Hz (the owner's PC; frames ran late and the sound broke up), and the frame
of latency it costs is 4 ms there, not 17. At the cabinet's rate it waits again as before.

Display pacing follows the change: its smoothed estimate of the game's rate takes a jump of more
than 5 % at once (smoothed, after 240 -> 60 Hz the sound played at a quarter of its speed for
a while and queued seconds), and a sound queue grown far past its target is cut back at once
rather than steered down over minutes (runtime/host/pacing.cpp, window.cpp).

A PC that can't keep the rate (the PC games so far, whose core measures it): a game whose pictures
take longer than the display's refreshes falls behind without end at the rate (each late frame
drawn at once after, the display showing pictures again: 2026-11-22, 11 ms pictures at 240 Hz,
2,912 of 9,691 frames shown again). Instead, while the rate is on, the time the game takes to a
picture (the 108th of its last 120) is held against the rate's refresh: when it is more than
three quarters of it, the rate steps down to the display's rate divided by 2, 3, 4... (the
smallest that fits; always a whole fraction, so each picture holds the same number of refreshes,
and never under the cabinet's rate), for the rest of the run, with a line in the log and three
seconds of notice on the window ("This PC draws the game at 60 Hz, not 240: its pictures take
11.0 ms"). Not for `hz = "x2"` (its game counts its charts in its frames: only that rate keeps
its music with its notes). runtime/host/ratestep.h, tests/host/test_ratestep.cpp. Before it steps
down, and on a PC just short of the rate, the PC core keeps its frames on a steady grid of the
rate's period: a picture late by less than a refresh used to begin the next period anew, so the
shell's frames, and the game's music with them (a frame's sound each), fell behind its song clock
for good, frame after frame (pop'n 18 at 240 Hz with pictures a little too slow: its music 25 s
behind its notes 54 s into a song in the rig, the owner's "desync" of 2026-11-22); now a frame
the game hasn't drawn by its time is shown again and made up, and the music stays within a few
milliseconds of the notes. The exit report says when pictures at a rate were not made up, and
when the rate stepped down.

pop'n music 14 (`games/popn14/mods/smooth-notes`, 2026-10-19):
- the play screen computes the song's position from a hardware timer on every frame
  (FUN_01740298, the word at 0x018ff668), draws the notes from it and judges from it: while that
  word changes (from the song's start to its end) the vsync follows the display;
- checked headless at 119.88, 239.76 and 479.52 Hz: a new picture on every vsync (the EE keeps
  up even at 480), the notes where they are at 60 Hz for the same moment of the song, and the
  play case's results identical with its presses at the same moments;
- the animated objects (the characters, the hits' sparks, many effects) go through one update
  (around 0x01736fb4) that adds each object's speed (percent, +0x20) to its frame counter (+8,
  with the remainder at +0xa); reading the speed as 0 between ticks (`between_ticks` on the load
  at 0x01736fbc) holds every object at the cabinet's pace: the characters' counter advances 500
  times in 1,000 vsyncs at 120 Hz, 250 at 240, 125 at 480, and their poses match 60 Hz's at the
  same moments. (Taking the objects' "paused" branch instead skips their drawing: they blink.)
- the rate holds only while the play screen's phase (0x00f898d8, `while_equal`) is 4, the song
  itself: phases 0-3 start it, 5-7 end it (cleared or failed), and the results follow; with the
  clock alone the rate stayed up through a cleared song's ending (2026-10-19, the owner's play:
  the results' animations ran fast and the sound fell behind);
- the HUD's animations (the difficulty name, NORMAL/HYPER/EX...) take their time from the play
  screen's frame counter (0x00f8b63c, +1 at 0x0174ce7c) and others from the vsync routine's
  three counters (0x0172f360): `between_ticks` on each;
- the GREAT/GOOD/BAD text's timer is counted through a pointer (`lw v0, 0(v1); addiu v0, v0, 1;
  slti v1, v0, 21`, at 0x0174dbb8): found with a write watchpoint (PCSX2's memory checks made to
  log without pausing, a change kept out of the tree) on candidates from RAM dumps around a
  judgement. At the cabinet's pace (between_ticks) the text shows 19 vsyncs at 120 Hz against 10
  frames at 60. The same code is in pop'n 9-13 (0x00117850, 0x0011cf80, 0x0011cbf0, 0x0011d248,
  0x0175bdb8), and pop'n 13 and 14 have an effect counter counted the same way (0x01766720,
  0x01758a90). Its timer is a counter in a
  long-lived stack frame (0x01ffea20 in the attract demo), counted through a pointer; not found
  among the increment-and-store sites.

pop'n music 9 to 13 (`games/popn<N>/mods/smooth-notes`, 2026-10-19): the same engine. Their
song clocks were found by the clock function's shape (pop'n 10-13: a pause flag at +0x20, the
timer, a 64-bit subtraction and conversion, the result stored at +0 of the same block; pop'n 9,
an older build, by a RAM search) and checked to move only from a song's start; the objects'
update is the same code in every one (`lh v1, 0x20(sN)` before `addiu a1, zero, 100`), and pop'n
10-13 have the 13-frame effect timer too:

| game | song clock | play phase | objects' speed load | effect timer |
|---|---|---|---|---|
| pop'n 9 | 0x0025a188 | not found | 0x00106ca4 | — |
| pop'n 10 | 0x002fa3e0 | 0x00376090 | 0x001084dc | 0x001163bc |
| pop'n 11 | 0x002ca180 | 0x0034e778 | 0x00108d94 | 0x00116a84 |
| pop'n 12 | 0x002d2488 | 0x0035ad00 | 0x0010847c | 0x00115ccc |
| pop'n 13 | 0x01907fd8 | 0x00fa90e8 | 0x01745a14 | 0x01753350 |
| pop'n 14 | 0x018ff668 | 0x00f898d8 | 0x01736fbc | 0x017448f0 |

The play phase (a halfword the gameplay function switches on: `lh v1, X(gp)` or a `lui` base,
then `sltiu v0, v1, 8`) is 4 while the song plays (checked on each through a play). pop'n 9's
(0x0025a0f8, five states) stays 4 into the results, so its rate follows the clock alone, which
stops with the song (frame 12783 of the play case, its ending and results then at 60).

v0.5 (2026-10-20): the vsync routine's three counters and the HUD's counter in every version:

| game | vsync routine (its +1, +1, -1) | the play screen's counter | its increment |
|---|---|---|---|
| pop'n 9 | 0x00100848 (0x00100854, 0x00100864, 0x00100870) | 0x0025a0f0 (64-bit) | 0x0010bca8 |
| pop'n 10 | 0x00100830 (0x0010083c, 0x0010084c, 0x00100858) | none: the HUD's time is the vsync routine's | — |
| pop'n 11 | 0x00100840 (0x0010084c, 0x0010085c, 0x00100868) | 0x0034e844 | 0x0011bf1c |
| pop'n 12 | 0x00100840 (0x0010084c, 0x0010085c, 0x00100868) | 0x0035add4 | 0x0011c5e8 |
| pop'n 13 | 0x0173dc60 (in v0.4) | 0x00faae1c | 0x0175b0ac |
| pop'n 14 | 0x0172f360 (in v0.4) | 0x00f8b63c | 0x0174ce7c |

Found from two RAM dumps 10 frames apart during a song (`HK_PY_EE_DUMP=8000-8010:FILE`): the
words that grew by exactly 10, then the code that loads, adds 1 to and stores each one (through a
register's `lui`, or gp: 0x0025da70, 0x003776f0, 0x003500f0, 0x0035d0f0 for 9-12, from the
vsync routine's and the phase's accesses). Checked at 119.88 Hz on each: every one of them changes
500 times in 1,000 vsyncs of the song.

Checked on each at 119.88 Hz: the rate on from the song's start, a new picture on every vsync,
the clock at 8.34 ms a vsync; on pop'n 12 an object's frame counter at the cabinet's pace (500
times in 1,000 vsyncs).

pop'n music 15 to 18 on the PC (`games/popn<N>/mods/smooth-notes`, 2026-10-27; each for its newest
program date): the same design as 9-14's, found by the same shapes in all four programs.
- The song's clock is `timeGetTime` from the song's start, divided by a rate (1.0) and kept in
  milliseconds (pop'n 15's 0x43cc50; its main loop takes it `>> 2`, the 4 ms steps it judges in),
  and the play screen's phase is a halfword (the same code in all four) that is the song's while it
  plays: 4 in pop'n 15 and 16, 2 in 17 and 18 (their phases renumbered, stepped with `add`): the rate
  is on while the clock changes and the phase is the song's.
- Held to the cabinet's pace (`between_ticks`, each `+1` made `+0`): every animated object's frame
  (each `add dword [reg(+x)], 1` right after a call of the sprites' draw routine, which draws an
  object at its frame: 11 to 21 places a game, characters, hit effects, the lanes' and judgements'
  animations, on any screen though held only while the rate is on), the play screen's frame
  counter, the frames since the song's start (the RETIRE? wait after the gauge runs out reads it:
  without it a failing guest's stage ended early), the attract demo's length (2000 frames, counted
  down), a counter to 1200 frames, and in pop'n 17 and 18 a set of counters a loop steps by one
  (`add [ebp+esi*4+x], reg`, the register also stepping the loop: the add itself made a 7-byte
  no-op) and a timer to 240 frames beside them, found with a data watchpoint (`HK_PC_WATCH`)
  since they're reached through a pointer.
- How they were found and checked: the game's memory (its globals and heaps: `HK_PC_DUMP`,
  `HK_PC_DUMP_HEAP`; `+N/K`: K pictures in a row from N pictures after the rate comes on) dumped on
  consecutive pictures of a song at 120 Hz: a value held to the cabinet's pace moves on every other
  picture, one still counting the pictures on every one. (A first version compared dumps 10 and 100
  pictures apart, which misses every counter that wraps sooner, an animation's loop: the owner saw
  the characters and effects at 240 Hz's speed.) Checked under Wine at 120 Hz (a 60 Hz display, two
  pictures drawn a frame): pop'n 15 in a credit's song (1,101 values at the cabinet's pace, no game
  counter at the pictures': only times and sound positions move each picture), 16-18 in the attract
  demo's song; the rate on from the song's start to its end; the sound unbroken across the
  switches.

| game | program | song clock | play phase (the song's) | patches |
|---|---|---|---|---|
| pop'n 15 | 2007091800 | 0x00c71008 | 0x00c5bd3c (4) | 25 |
| pop'n 16 | 2008052700 | 0x00d27f78 | 0x00d12c4c (4) | 20 |
| pop'n 17 | 2009022500 | 0x010408f8 | 0x0102b520 (2) | 18 |
| pop'n 18 | 2010040500 | 0x0107c030 | 0x0106699c (2) | 20 |

The last 32-bit PC pop'n (`games/popn28/mods/smooth-notes`, 2026-11-14; the program of 2025092400):
the same song clock as pop'n 15-18 (`timeGetTime` from the song's start over a rate of 1.0, kept in
milliseconds at 0x1099b194 as the play screen reads it), and the play screen's phase a halfword
(0x109927a4) that is 2 while the song plays: the rate is on while both say so, in the attract
demo's song too. Its animations are the AFP library's (`libafp-win32.dll`: every layer the game
draws), moved on each picture by a time the game gives them, a constant of one cabinet frame
(0.016677 s at 0x102abe70, `afp_do_render`'s first argument): `per_picture` makes it the rate's
picture's time. The game's own counters of pictures, held to the cabinet's pace: the play screen's
(0x10de78dc: what blinks on odd pictures, colour cycles), its count from its start (0x1099aebc), the
pictures without a button held (0x10de7950: the stage ends at 1200, 20 s, with a warning at 600), each
player's character's time in its state (0x10999fb0), a 21-picture loop (0x10999f70) and the attract
demo's length (2000 pictures counted down at 0x109ce6f8). Found as pop'n 15-18's were (consecutive
dumps at the rate, the heaps too; data watchpoints for the writers); the animations' time by its
uses: the heaps' words moving each picture at 120 Hz fell from 92 to 38 with it. Checked under Wine
at 80 Hz (what the nested 60 Hz display keeps up with: 80 pictures a second; at 120 the game drew
80): a character's loop 197 frames against 148 at 60 (148 x 80/60 = 197.3), the demo 2,389 frames
against 1,790 at 60.6 Hz (29.9 s and 29.5 s); the notes on the clock; the rate off at the song's
end. Not checked: a real 120 or 240 Hz display (the owner's), judging at the rate.
The "80" was the game's own wait, not the display: before each Present it slept until 2 ms before
16 ms after the last one returned, 10 ms of every frame (runtime/pc/cabinet.c `unlimit_frames`
takes it out since 2026-11-23); since then it draws 120 and 240 pictures a second in the rig, 0.8 ms
each.

beatmania IIDX 16 EMPRESS on the PC (`games/iidx16/mods/high-frame-rate`, 2026-11-09; the program of
2009072200): a game that counts everything in frames, its song's position too, so the clock-following
design above doesn't fit. Its charts are milliseconds converted to frames as a song loads (0x489300,
with the frame the monitor check gives: 16.6528 ms for VGA, 16.6806 for S-VIDEO, kept in 0x2951764),
and the scroll's speed is figured from the same frame (0x4879f0, 0x4892c0). The mod halves both
frames: the charts are counted in half frames, and the game runs at exactly twice its rate
(`hz = "x2"`, 120.07 or 119.87 Hz) while the chart's position (0x029393b0) moves, in a song and in the
attract demo, so its notes move twice as often at the same speed, in step with its music.
- What the chart code counts in its frames, doubled: a note missed 15 frames late (31 half frames:
  16 frames' worth, less one), drawn 4 frames past its time, the lanes' coming notes 21 frames
  ahead, a note let go 60 frames late, the longest chart (18,000 frames), the turntable's move
  (measured over 2 frames, 4 now) and its turning back (4 frames, 8), and a failed stage's end
  (153 frames of the chart's position after the failure: its STAGE FAILED shutter closed in half
  the time without it).
- The judging: every window is the game's own, in its frames (the chart's, PGREAT's distance 0 or
  1, the turntable's frame). HIRAKU's DLL sees the halved frame in the mod's patch
  (runtime/pc/cabinet.c `chart_frame`, `hk_chart_scale`) and, around the game's judging, takes each
  lane's distance back to whole frames (the frame it falls in; runtime/pc/press.c, the same place
  as `--press-time`), so each window is as long in time as on the cabinet. hiraku-pc still hears the
  monitor check's frame (the frame given doubled).
- Held to the cabinet's pace (`between_ticks`): the engine's animated objects (each drawn at its
  frame, then stepped: 0x405642), the effects' objects (judgements, combo), the keys' bombs, beams
  and presses, the turntables' pictures, the play and demo screens' frame counters and fades, a
  lamp's blink, a background effect, and the timers beside them: found by dumps of four pictures
  in a row at 120 Hz in a song and in the demo (`HK_PC_DUMP`, `HK_PC_DUMP_HEAP`) and data
  watchpoints (`HK_PC_WATCH`): the demo ends when its animated object reaches its end (0x46a6ec),
  which had it end in 18 to 21 seconds of its 35.
- Not for a song longer than 4 minutes 32 seconds: the notes' times are 16-bit (32,767 half frames).
- Checked under Wine (a 60 Hz nested display, two pictures a frame), against the same song at 60 Hz
  with the same presses at the same moments: a new picture every frame, the bar lines 1.6 pixels a
  frame where they move 3.2, at the same height at the same moment of the song (2 to 5 pixels: the
  two frames before the rate comes on); the stage failed at the same moment (88.6 s into the
  chart), with the same judgements to within a press or two (GOOD 6/4, BAD 11/11, POOR 75/76:
  windows of half the length would have halved them); the movies in time. Then played by the
  owner on a 240 Hz display (2026-11-09, smooth motion on as well): nothing found wrong.

beatmania IIDX 15 DJ TROOPERS on the PC (`games/iidx15/mods/high-frame-rate`, 2026-11-10; the program
of 2008031100) is EMPRESS's engine, done the same way: its charts are converted as they load at
0x47c750 (the frame in 0x29c53f4), its position is 0x029ad048, and most of its places were found from
EMPRESS's by the code around them, the rest by what they count. The rate is on in a song and in the
attract demo (a HOW TO PLAY lesson that is a chart on autoplay). One counter EMPRESS's notes don't
have: the cabinet's idle timer (0x27adf70, frames without a key or the turntable touched: a song
ends after 40 s of it, a warning at 28 s) is held to the cabinet's pace, or an idle song ended
after 20 s. Checked under Wine against the same song at 60 Hz: the same notes, bar lines, movie
and gauge at the same moment; a song played to its end in 6,766 frames at both rates; an idle song
ended at the same moment (2,555 and 2,557 frames); the judging windows the same in both
(0x298d8e0: ±2, ±7, ±15 frames for the keys); the demo the same length and combos. Then played by
the owner on a 240 Hz display (2026-11-10, the check's 59.95 Hz frame: 119.87 Hz): nothing found wrong.

beatmania IIDX 9th style to GOLD on the PC (`games/iidx9` to `iidx14/mods/high-frame-rate`, 2026-11-14;
the programs 9th style JAH, 10th style JAE, RED JAD, HAPPY SKY JAD, DistorteD JAG, GOLD 2007072301)
count their charts in frames: an event's time in a chart file (`sd_data/NNNN/NNNN.1`) is a frame
number, not milliseconds as from DJ TROOPERS on. Their mods have the charts read with every time
doubled instead of halving a frame: `chart_times = 2` in the `[[rate]]` table has HIRAKU's DLL
give the game, for each chart file it opens, a copy with its events' times doubled (made in the
save folder's `charts-x2` once, about 40 MB for GOLD's 464 songs). Each game's notes move down by a step a frame worked out from the BPM
(its scroll ring's speed): halved, so they move as far a second; what the chart code counts in
frames is doubled (a note looked for 60 frames late, let go 60 after, drawn 4 after; a failed
stage's end; the turntable's samples; the gauge graph's every 32 frames; the longest chart,
10,800 frames, past which the chart code doesn't look for its end); and the screens' counters,
lamps, lanes' beams and the ticker are held to the cabinet's pace. A note's time is kept in 16
bits; the longest chart of the six (8,637 frames) fits doubled, so no song is too long. The
judging goes through HIRAKU's DLL as EMPRESS's does (the lanes' distances taken back to whole
cabinet frames). Where a change needed more bytes than an instruction had (a window's late edge
doubled, a 122-frame end past its byte), the instruction calls a few bytes written in the padding
after a routine. Checked under Wine against the same runs at 60 Hz: GOLD's song left untouched
lasted the same to the frame (18,240 to 25,300) and ended with the same 70 POORs, FAILED, and the
same gauge graph; the attract demos of GOLD, DistorteD and 10th style the same length to the
frame (2,790, 2,070 and 3,390 frames), the next one starting at the same frame; RED's played
song failed at the same point with the same judgements to within a few notes (99 against 103
judged, nearly all POOR); 9th style's played song the same length (7,110 and 7,140
frames, a snapshot apart); the notes, bar lines and movie the same at the same moment in HAPPY SKY,
10th and 9th style. Before the screens' counters were held, the demos ended early (RED's at
1,630 frames of 2,620). Not yet played on a 240 Hz display, nor judged with real presses.

The Firebeat pop'n music (4-8) are another case (checked on pop'n 4, 2026-10-19, with the
Firebeat core's frame period halved by hand): the attract demo's song lasts the same number of
frames at twice the rate (about 2,400 at both), so its play goes by frames, not by a clock, and
so does DJ Main's pop'n music 1. A faster vsync would only make them faster: on those cores the
display's extra refreshes get in-between pictures instead (`--smooth-motion`, docs/SMOOTH.md),
for every game on them, with no mod.

Only the Python, DJ Main and PC (32-bit) cores change their rate so far; elsewhere `[[rate]]` says so in the
log and does nothing.

## beatmania III's turntable (`games/bm3final/mods/iidx-turntable`)

The Final judges a scratch from the turntable's moves over the last 8 frames (issue 15, 2026-11-15:
the function at 0x80042fd8). Each frame's move goes into a ring of 8 (0x80255d40, 32 bytes a
player, its index at 0x80255d80) and the ring is summed. With no scratch going on, a frame's move
of more than 4 counts starts one; during one, the sum turned the other way by more than 16 starts
another; the scratch ends when the sum is 0, 8 frames after the turntable stops. Until then a
second scratch the same way doesn't count, and a quick turn back and forth sums to nothing and only
ends it: fast scratching (VOIDDD's start) falls through. The mod makes the ring 2 frames (its
index `mod 2`, the sum over 2 entries) and the turn back 4 counts, as beatmania IIDX's turntable
feels. Checked in a scripted stage, the scratch state read each frame: three flicks 5 frames apart
were one scratch for 18 frames, now three of 2; a flick, one back and one forward 2 frames apart
were a scratch that ended at the turn back, now three. Not checked on real hardware or by ear.

## beatmania IIDX's keysounds on Twinkle (`games/<id>/mods/quick-keysounds`)

The Twinkle IIDX games read the keys at the start of a frame, but a press's keysound reaches the
sound board only with the next frame's sounds: the board keys it on about 18 ms after the frame
the press was read in, where the PC IIDX games take about 4 ms (2026-11-22). Every style does it the
same way: the game queues its sounds (a queue of 16) and sends them once a frame, early in the main
loop (the sound service: the whole queue to the dual-port RAM and the board's mailbox rung, or with
none queued one of the board's other commands, so one command a frame); the play screen's frame
reads the presses and queues their keysounds later in that frame. The mod (off by default, as every
mod) calls the game's own service again right after the play screen's frame when the queue holds
only the presses' keysounds (it was empty where the keys' part began, so the chart's sounds, such as
the music's start, keep their frame) and the board had no command yet that frame (a second command
in a frame, while the board still works on the first, can lose its keysound). The next frame's
service then sends nothing where it would have sent those keysounds (its "board free" test reads the
mod's flag too), so the board's other commands keep their frames too. Its routines lie in main RAM
the machine leaves at zero and never touches (0x8000d000, in the BIOS's area below the program:
nothing read, wrote or ran there over boot, attract, play, test mode and its reset, in every style),
so the patch carries nothing of the program there; two words of the gap after the queue hold its
flag and what the board last got, and the game's own paths take as many instructions as before up to
each write to the dual-port RAM. The DEMO PLAY's keysounds come the same way (the autoplay presses
the keys), so they move too. Not how the machine behaved: the keysound comes one frame sooner.

Each set has its own program (no two share the bytes), so each has its own mod, made by the same
method from the same instruction shapes, each replaced word checked against the program. The styles
come in three shapes: from 3rd style on the board's mailbox has a
function of its own, through which every command rings, and the mod notes the frame of the last ring
there; in 1st style, Substream and 2nd style the queue's send and the service ring it themselves, so
the mod marks a command in two instruction slots that did nothing (a nop after the send's return and
one after a load where the board's other commands are taken), clearing the mark each frame. The club kits'
driver is older again (the send rings the mailbox itself, the play screen's frame is called from
three places, the main loop keeps no frame number): a word in the gap is set after a ring and cleared
when the play screen's frame ends.

| game | set | program | shape | status |
|---|---|---|---|---|
| beatmania IIDX (1st style) | bmiidx | GQ863 JAB | the send's own ring | done: key-on 17.90 -> 1.85 ms |
| | bmiidxa | GQ863 JAA | the send's own ring | done: 17.90 -> 1.84 ms |
| Substream | bmiidxs | GC983 JAA | the send's own ring | done: 17.90 -> 1.73 ms |
| | bmiidxsa | GC983 JAA (983-AA, Asia) | the send's own ring | done: 17.90 -> 1.73 ms |
| 2nd style | bmiidx2 | GC985 JAA | the send's own ring | done: 17.97 -> 1.78 ms |
| 3rd style | bmiidx3 | GC992 JAC | the mailbox function | done: 17.93 -> 1.76 ms |
| | bmiidx3a | GC992 JAA | the mailbox function | done: 17.93 -> 1.76 ms |
| | bmiidx3b | GC992 JAB | the mailbox function | done: 17.93 -> 1.76 ms |
| 4th style | bmiidx4 | GCA03 JAA | the mailbox function | done: 17.95 -> 1.79 ms |
| 5th style | bmiidx5 | GCA17 JAA | the mailbox function | done: 17.95 -> 1.82 ms |
| 6th style | bmiidx6 | GCB4U JAB | the mailbox function | done (2026-11-22): 17.95 -> 1.83 ms |
| | bmiidx6a | GCB4U JAA | the mailbox function | done: 17.95 -> 1.83 ms |
| 7th style | bmiidx7 | GCB44 JAB | the mailbox function | done: 18.08 -> 1.99 ms |
| | bmiidx7a | GCB44 JAA | the mailbox function | done: 17.95 -> 1.85 ms |
| 8th style | bmiidx8 | GCC44 JAA | the mailbox function | done: 18.06 -> 1.98 ms |
| club kit | bmiidxc | 896 JAB | the club kits' | done: 17.96 -> 1.94 ms |
| | bmiidxca | 896 JAA | the club kits' | done: 17.96 -> 1.94 ms |
| club kit 2 | bmiidxc2 | GE984 | the club kits' | done: 17.94 -> 1.76 ms |

Checked headless (2026-11-22 and -23; `--wav`, the dual-port RAM's writes and the board's key-ons
logged), per style: 1st (bmiidx: full set; bmiidxa: the reduced set), Substream (bmiidxs full, bmiidxsa reduced), 2nd (full),
3rd (bmiidx3 full, bmiidx3a and 3b reduced), 4th (full): 14 to 30 presses in a stage, key-on about
17.9 ms after the press's frame began, now 1.73 to 1.85 (1.69-1.97), and the recorded keysound
(the press run against one without that press) 17.4 ms after it, now 1.3; a press offered later in a
frame waits for the next frame's read, so the worst case is 34.6-34.7 ms, now 18.5. A run without
presses records the same sound and pictures, byte for byte, and the dual-port RAM is written at the
same moments, in all seventeen new sets; with presses, the chart's and the board's other commands go out in the same
frames with the same bytes (a few of them 0.2 to 1.6 µs off, as in 6th style); the same presses give
the same pictures to the result screen, and the same requests and key-ons, also in a song played with
a key group every frame (none lost, the queue never more than 2 deep); the reset from test mode brings
the hooks back with the reloaded program.
5th, 6th (JAA), 7th (both) and 8th style and the three club kits each had the whole set of checks,
with the same results (key-on 1.76 to 1.99 ms). The club kits were also run as a pair with their DDR
cabinet, the mod on the IIDX side: both cabinets' pictures identical in every frame, the keysounds a
frame sooner in the session too (the DDR cabinet plays the IIDX's sound, so its recording changes with it).
When the board already had a command in the frame (a turntable's, the chart's), the keysound waits
for the next frame, as the game's did; with a new keysound in every frame (a key every frame through
a song) that wait carries on to the next frame's keysounds, so in such a run most keysounds keep the
game's timing (none lost). Not checked on a cabinet; nobody has listened.

## pop'n music's keysounds on DJ Main (`games/<id>/mods/quick-keysounds`)

The DJ Main pop'n music games read the buttons at the start of a frame, but a press's keysound is
keyed on only by the next frame's sound work, about 18 to 19 ms after the frame the press was read
in (2026-11-23). The 68EC020 plays the two K054539s itself, with a small kernel taking turns between
its tasks: a sound is a request in a ring of 64, queued by the game's play routine under the ring's
lock; once a frame the sound task runs the ring's service, which takes ONE request and plays it
(and, only in a frame where it took none, the voices' tick: their ages, fades, ends and chained
sounds). The game task runs after it in the frame, and its key handling queues the pressed buttons'
keysounds: they wait for the next frame's service, and a chord's keysounds come one a frame.

The mod (off by default, as every mod) has the key handling queue a keysound through a routine of
its own, which does the play routine's work as the play routine does it (the same kernel calls, the
caller's own task block, the game's own queueing) and, before giving the lock back, sends the new
keysound at once with the game's own service when every request before it in the ring is one of the
mod's placeholders and the frame's service took none (or a placeholder). It leaves a placeholder in
the keysound's place in the ring (the driver's "pan" request, whose handler writes only a voice's
word that nothing reads), so the next frames' services take the placeholder where they took the
keysound: every other request (the chart's, the system's) is played in its frame, the voices' tick is
skipped in the same frames, and the ring fills as before. The routine runs only when a keysound is
queued, so a game without presses is the same, every write to the sound chips at the same moment.

A setting, **Chords**: "One sound a frame (the machine's)" (the default) sends the first keysound of
a frame at once and leaves the others of a chord to the machine's one-a-frame order; "Together" sends
every key's keysound of the frame at once (a chord's sounds start together, about 0.2 ms apart, where
the machine needs a frame for each). Not how the machine behaved, either way: the keysound comes one
frame sooner, and with Together a chord's keysounds come in its frame.

The program is in ROM, so the routine lies in the ROM's padding after the program (0xFF bytes that
nothing reads or runs but the ROM check). The ROM check (at power-on and in test mode) sums each ROM
chip's bytes, so the mod lowers more of the padding's bytes until both sums are the program's own
(without them the power-on check stops with E121/E122 PRG ROM ERROR); test mode's ROM TEST says OK.
The patches carry 6 bytes of the program each (the key handling's call, one per call: two in pop'n
stage EX), the rest is the padding's 0xFF.

| game | set | program | key-on, ms from the press's frame start | status |
|---|---|---|---|---|
| pop'n music | popn1j | GQ803 JA-A | 18.84 -> 2.45 (1.81-2.99) | done, full set |
| | popn1a | AA-A | 18.00 -> 1.53 (0.88-2.11) | done, reduced set (pop'n 1's program, revised) |
| | popn1k | KA-A | 18.37 -> 1.75 (1.14-2.68) | done, reduced set |
| | popn1 | TA-A | - | not made: its program ROMs are not among the dumps here |
| pop'n music 2 | popn2 | GQ831 JA-A | 18.46 -> 2.18 (1.38-3.28) | done, full set |
| pop'n music 3 | popn3 | GQ980 JA-A | 18.43 -> 2.61 (2.05-3.44) | done, full set |
| pop'n stage EX | popnstex | GQ970 JB-A | 18.63 -> 2.01 (1.70-2.46) | done, full set (keysounds only for a press that hits a note) |

Checked headless (2026-11-23; `--wav`, every write to the sound chips, the ring and the voices'
choice logged): per program 14 to 22 presses with a keysound in a stage: the recorded keysound
(the press run against one without that press) 18.5 to 18.9 ms after the press's frame began, now
2.0 to 2.6; a press offered later in a frame is read in the next frame, so the worst case is 35.7 to
36.1 ms, now 19.2 to 19.9. A run without presses (power-on from a blank NVRAM to a song) records the same sound and
pictures and writes the chips at the same moments, byte for byte. With presses, the chart's and the
system's sounds and the voices' ticks go out in the same frames with the same register values (a few
cycles off in time); the same presses give the same pictures through a stage played a button every
10 frames (to the game over and the ranking), with chords (pop'n 1 and 3, stage EX), and after test
mode. A song played with a button group every frame (chords of 2 to 4 and a 9-button mash, far past
a player: the ring of 64 is full and drops requests on the machine too) gives the same requests, the
same key-ons per voice, the same fill and drops, and the game's own work (its calls of the play
routine, its judgments) the same, in pop'n 1 (JA-A, KA-A), 2 and 3; in such frames the 68EC020 is at
its limit, and the routine's few microseconds a press (and with Together the keysounds' own work,
moved into the press's frame) can move an animation (a character's pose, the gauge's blink, a hit's
spark) by a frame (pop'n 1 in the mash; pop'n 2 in the mash with Together and once in the chords),
and the sound task's work by up to 1.7 ms within its frame; in pop'n 1 AA-A the mash makes the
machine itself miss presses when its frame overruns, and with the mod it missed 6 fewer of 13,758.
The voices: pop'n 1 JA-A's and AA-A's keysounds have voices of their own (the same in every run); in
pop'n 1 KA-A, 2, 3 and stage EX a keysound takes a free voice, and as the earlier keysounds end a frame sooner too, some take another voice than on the
machine (pop'n 3, a stage of 539 presses: 30 another voice, and once a playing voice of the same
priority taken where the machine found a free one: 58 such takes against the machine's 57).
After test mode (a sound played in the sound test, then its EXIT: the game starts again) the mod
works as before. Not checked on a cabinet; nobody has listened.

## How the layers work

- **Patches:** every frame, before the game's frame runs, each patch looks at its address: with
  its `replace` bytes already there, nothing to do; with its `original` bytes there, it writes
  `replace` and tells the core (`Core::code_changed`), so a core that runs translated code
  translates that part again; with anything else there (code not loaded yet, another version of
  the game), it waits. `once` patches stop looking once applied. The log says when each patch is
  first applied.
- **Media:** a byte range of an image is replaced as the image is read (`runtime/shell/overlay.h`),
  in the image layer every HIRAKU core reads its media through (`runtime/dev/image.h`), and in
  the Python core's disk reader (PCSX2 Reliquary's, through the `Hiraku::disk_read` hook). Writes
  the game makes there go to the game's own save copy of the disk as before, but reads of a
  replaced range keep returning the mod's bytes.
- **Files on ISO 9660 discs** are found by their path in the disc's directory (data tracks of 2048
  bytes a sector, or 2352 with the sector's 16-byte header and 288-byte trailer around them) and
  replaced as `[[bytes]]` of their sectors.
- **Files on Konami Python 1 disks** (`runtime/host/pythondisk.cpp`; the file system is told in
  `tools/popnfs.py`): the disk's directory (sector 0x4800) gives a file's place by the hash of its
  name. The mod's file is packed the way the game reads that file (LZSS for pictures, with its
  size first; a folder's `system.idx` as it is, with its size first; the IOP modules as they
  are), unencrypted (the game decrypts only files that carry a tag), and laid in the file's own
  sectors when it fits there, else in the unused room after the last file (checked to be zeros).
  The directory, with the file's new place, size and byte sum, is laid over the disk's (its own
  sum made again), so the loader and the disk check find it. A name the disk doesn't have is
  added to the directory. The game's program (`ee/boot/game.bin`) isn't replaced this way: its
  code and text are `[[patch]]`es. A file of the same content can still change the game's
  timing: packed again it is a little longer or shorter, or somewhere else, so it loads a frame
  sooner or later (pop'n music 14's title loop then shows a different animation; the play case
  joins up again once a song starts).

## The PC games

pop'n music 15 and later are Windows programs that HIRAKU runs as they are (PLAN.md section 30),
so their memory and files are the game's own process's, not an emulated machine's:
- **Texts** (`[[text]]`) are looked for in the program file at the start and become patches.
- **Patches** go to HIRAKU's DLL in the game (a list in the save folder, `mods-patches.txt`),
  which looks at each one as the program is loaded (before any of its code runs) and before each
  picture, with the same rules: `original` there, `replace` goes in (read-only pages too: code,
  constant text); `once`; no `original`, written every time (a cheat). The log (`mods: ... patch
  at ... in place`) says when the game's process has put one in.
- **`[[rate]]`** (the 32-bit PC games, 2026-10-27): the words it watches are the game's own, which
  HIRAKU's DLL copies out before each picture; while it is on, hiraku-pc lets the game draw `hz`
  pictures a second (its clock, its sound and the window's pacing go on without a jump; on a
  display slower than the rate, the pictures between two shown ones aren't shown), and the DLL
  counts the cabinet's ticks itself, putting `while_rate` and `between_ticks` patches in and out
  before each picture. A frame that only shows the last picture again (the game drew none: a load,
  a hitch) isn't counted as the watched word standing still. Not the 64-bit pop'n (already 120).
- **Files**: the PC games' mods are folders of files at their paths, as
  [ifs_layeredfs](https://github.com/mon/ifs_layeredfs) has them for the later games: a file at
  `<mod>/a/b` is the game's `data/a/b` (any case). The game opens the mod's file instead, whole,
  any length (`mods-files.txt` in the save folder lists them; the save folder's `pc.log` says when
  the game first opens each). `[[file]]` with `medium = "data"` and the file's `path` under it
  does the same from a file elsewhere in the mod. `[[bytes]]` doesn't apply (no disk image).
  The path is matched however the game writes it: `\` or `/`, a doubled one (IIDX EMPRESS opens
  `..\data\sd_data\\...`), `.\`, `x\..`, a separator at the end, any case.
- **New files** (the 32-bit games under hiraku-pc but pop'n 15-18, 2026-11-21: IIDX 9th style to
  EMPRESS, the ee'MALL station): a file the game's data folder lacks is added to it, new folders
  too: the game opens it, looks at it and finds it at `data/a/b` as if it were there, and a
  listing of its folder (`*.*`) shows it, each name once (a replaced file with the mod's file's
  size). Where two mods have the same file, the first in the list wins, as for replaced files; a
  new folder in two mods lists both mods' files. `[[file]]` with `medium = "data"` and a `path`
  the data folder lacks adds that file. Never the game's: `mod.toml`, `macros.txt`, the files a
  table names (`from`), anything hidden (a name starting with `.`, folders and all), and a path
  that would go under one of the game's files or over one of its folders; anything else in the
  folder is, a readme at the top too (`data/readme.txt`, which no game asks for). hiraku-pc says
  how many files were added and the first; the DLL's `pc.log` line counts replaced files, new
  files and new folders. DirectShow opens IIDX's movies itself: a new or replaced
  `movie/NNNN.4` reaches it by the name the game hands it. A `[[files]]` folder whose `when`
  holds (above) is laid as the mod's own folder is, before it: its files replace and add too. pop'n 15-18 keep the old way: a file
  their data lacks is left out (said in the log, with the first), as 15 checks its files by
  their scrambled names against `dat.bin` and 16-18 take no mods' files yet. Other media (disc
  and disk images) take no new files either.
- **The game's check of its data**: pop'n music 15 keeps an MD5 of every data file in its
  program folder's `dat.bin` and checks a twentieth of them at each boot (error 5-1503 when one
  differs). HIRAKU runs the program from a copy in the save folder, and writes that copy's
  `dat.bin` at each start from the program's own, with the mods' files' MD5s for the files they
  replace: the check passes, and a game without mods is checked against Konami's list as it was.
- pop'n music 15's data files have scrambled names (`data/mbyukaomrfknhnko/...`); which file is
  what is the mod's business, as for any game.
- pop'n music 16-18: patches work (checked on 18, 2026-10-22: a boot-screen string). Files not
  yet: their data lists (`dat0.bin`...) are encrypted (AES-256 with the key in the program) and name
  the files by their logical paths, while the disk holds numbered folders libavs maps them from;
  a mod's files need both worked out (PLAN.md section 30, PC-6).

- **The 64-bit PC pop'n** (`[pc] loader = "pc64"`): a mod's folder is laid over the game's data
  folder, new files too, the first mod winning. Its music data (popnhax's XML, any `.xml` in the
  folder: `<music id>` with its texts, characters, `display_bpm` and `<chart idx>` files) brings back
  the songs the game left out, each whose Normal chart's `sd/<folder>/<file>.ifs` is there; a
  picture the game reads from inside one of its packs (a song's banner `kc_NNNN.ifs`, its
  background) can be a whole file in the mod's folder named after the pack, as ifs_layeredfs's
  (`tex/system/kc_diff_ifs/kc_0001.ifs`). Its `[[patch]]`es name the module they're in
  (`module = "popn.dll"`, `address` an offset in it: the 64-bit game's addresses don't fit the
  table), each put in when the game is loaded and checked each frame (`mods: patch at ... in place`
  in `pc.log`); a patch with a module isn't for the 32-bit games (left out, and said). HIRAKU's
  `games/popn29/mods/`: `every-market` (the songs and characters a cabinet's market leaves out,
  listed in every one) and `usa-pictures` (the USA's market on data without its pictures).
  The save folder's `pc.log` lists the songs brought back
  and those left out (PLAN.md section 34; how to install a pack: [SONGS-BACK.md](SONGS-BACK.md)). Songs past the game's
  own (custom songs) work too. HIRAKU's e-AMUSEMENT server keeps their scores, but they haven't been
  seen read back in the game yet. Not yet: PNGs in the `_ifs` folders.

- **The last 32-bit PC pop'n** (`[pc] loader = "pc32"`): a mod's folder is laid over the game's
  data folder (new files too, the first mod winning), and inside the game's packs as ifs_layeredfs
  lays them out: `<mod>/sd/system/system28_ifs/system28.2dx` is the member `system28.2dx` of
  `data/sd/system/system28.ifs`, for any pack the game mounts (a pack inside a pack too). The save
  folder's `pc.log` lists the files and members read from the mods (`mods: ...`). Its music data
  (popnhax's XML, any `.xml` in the folder) brings back the songs the game left out, each whose
  Normal chart's `sd/<folder>/<file>.ifs` and banner are there (`songs: ...` in `pc.log`), songs past
  the game's own too, and charts it adds to the game's own songs. Pictures as PNGs work too
  (`<pack>_ifs/tex/<name>.png`, as translations have them): each is converted for the game the
  first time it's shown in a run (`mods: picture ...` in `pc.log`), its size the game's, or the
  mod's own `tex/texturelist.xml`'s; not yet `dxt5` pictures, nor new pictures without a list of the
  mod's (PLAN.md section 35). A mod's `[[patch]]`es and `[[rate]]` go to the game's DLL as the other
  PC games' do (its addresses as `popn22.dll` lays itself out, from 0x10000000), and `[[text]]` is
  looked for in that DLL.

- **Addresses in a DLL game are the DLL's own, at its preferred base** (2026-11-22): the last
  32-bit PC pop'n's `popn22.dll` (0x10000000) and the 64-bit pop'n's `popn.dll` (0x180000000) don't
  always load there (Wine 11.22 moves every DLL that has a relocation table when the program
  starting it is marked DYNAMIC_BASE, as HIRAKU's loaders are: a new place each run; Windows may
  move one whose place is taken). A `[[patch]]` address, a `[[rate]]`'s words, `[[text]]`'s places
  and the debug switches' addresses (`HK_PC_WATCH`, `HK_DBG_REC`) are written as a disassembler
  shows the DLL at its preferred base; the game's side moves each one inside the DLL by where it is
  now, and a patch's `original` and `replace` bytes at the fields the DLL's relocation table names
  (an absolute operand: `add [0x10de78dc], ebx`) are compared and written as relocated (`replace`'s
  only where it still holds an address in the DLL: a nop over the operand stays a nop). `HK_PC_DUMP`
  names its files by the preferred address too. A 64-bit patch with `module` still gives an offset
  in that module. `pc.log` says where the DLL is (`mods: popn22.dll at ..., not its own base ...`).
  The EXE games (pop'n 15-18, the PC IIDX games, the ee'MALL station) are at their own base:
  nothing changes there (`runtime/pc/imagebase.h`).

## Per core

| core | patches | media |
|---|---|---|
| DJ Main | data and code (the 68000 is interpreted) | the hard disk |
| System 573, Twinkle | data and code: pages of code the recompiler's blocks came from are write-protected, so a patch moves their epoch on and the blocks are checked again before they run (a patched block runs interpreted) | CDs (a cue sheet's data file), the hard disk, DVD |
| Firebeat | data and code: the program runs translated ahead of time (recompiled, or the executor's steps), so a function a patch changes runs interpreted from its next call on (the log names it) | CDs, the SPU's disk or DVD |
| Konami Python 1 | data and code (the EE recompiler's blocks over a patch are cleared) | the hard disk, the CF card (PCSX2 Reliquary's reads, through `Hiraku::disk_read`); files by name on pop'n music 14's disk |
| Konami Python 2 | data and code | not yet (its disk is PCSX2's DEV9 ATA) |
| PC (pop'n music 15 on) | data and code, at the program's own addresses (`popn15.exe` from 0x400000, as a disassembler shows it); HIRAKU's DLL puts them in place inside the game's process, as the program loads and before each of its pictures | the game's files, whole: a file in the mod's folder at its path under the game's `data` folder (or `[[file]]` with `medium = "data"`) is read instead of the game's |

Checked (2026-10-19): pop'n music 14, a test-menu string patched once its program had loaded,
and the disk's boot header laid over with its own bytes (the play case's digest unchanged, the
read logged); DDR 2ndMIX, a CD sector range and `PSX.EXE` as a `[[file]]` laid over with their
own bytes (3,000 frames unchanged); the launcher passing the mods and starting the mod set's saves
from a copy.

Checked (2026-10-19): pop'n music 14's `image/256_title/0.gzz` (the attract screens' small
lettering) as a `[[file]]`, unchanged (the play case: the title loop's timing moves, the rest of
the pictures the same) and with its bytes changed (the coin count's digits changed on screen).

Firebeat code patches (checked 2026-10-20 on pop'n music 4, in its own binary and in
`hiraku-firebeat`): a function whose code a patch changes runs in the interpreter from its next
call on, reading the code as it is now; a call already under way finishes in the translated code,
so a loop that never returns (a game's main loop) keeps its old code. The two binaries give the
same run, frame for frame. Two costs: the interpreter takes interrupts between any two
instructions where the translated code takes them at its polls, so even a patch that changes
nothing moves the machine's timing a little (pop'n 4: the picture the same in every frame, the
sound reaching the same total a few frames apart); and it is slower, which shows only in a
function the game calls very often (pop'n 4's message poller, called ~3,000 times a frame: 1.1 →
3.8 ms of host work a frame). In `hiraku-firebeat` the rest of the program keeps its speed: the runs the
executor remembers and replays are forgotten once when a patch first changes a function, and a run
that reaches a changed function is never remembered.

pop'n music 13 and 14 keep their program (`ee/boot/game.bin`) encrypted on the disk, so its text
and code are changed with `[[patch]]` once it's loaded, not on the disk.

## pop'n music 14's disk

`tools/popnfs.py IMAGE list` lists its files (10,165: 10,010 named from the game's own strings and
index files, the rest as `#hash`), `tools/popnfs.py IMAGE extract NAME... OUT` writes them as the
game uses them (decrypted with the keys in the disk's own loader, unpacked). What's there:
`image/<folder>/` holds the pictures (`.gzz`: a `GC` header, then the picture), each folder with
its `system.idx` listing them; `iop/modules/` the IOP's drivers; `ee/boot/game.bin` the program.

A picture, for a mod: export it (`tools/popnfs.py IMAGE pngs image/256_title OUT`), edit the PNG
(same size) and put it in the mod's folder at its path (`image/256_title/1.png`); or convert it
yourself (`tools/popnfs.py IMAGE frompng image/256_title/1.gzz title1.png files/1.gzz`) and name
`files/1.gzz` in a `[[file]]`. 16-bit pictures (song banners `kc/`, backgrounds,
characters) take any colour; the 8-bit ones (the `256_*` folders: the menus' lettering and
parts) share their folder's 256-colour palette, so the PNG's colours become the nearest of it.
The songs are not in these files: each has a 20 MB slot after them, its parts scrambled with
the slot's number. `tools/popnsong.py IMAGE list` lists the 749 songs' slots and
`tools/popnsong.py IMAGE extract SLOT OUT` writes a slot's keysounds, charts, music and preview
(PLAN.md section 20). Mods can't add or change songs yet.

Its menus' pictures are lettered sprites: each folder's `system.idx` names them and gives their
rectangles as pop'n music 11's pack index does (without its leading size word; the rectangles
8-byte aligned), so a translation edits the PNGs at those rectangles. The mode select is
`image/mode_sel` (cards, captions, the blue/red button lines, cut into pieces for their exit
animation, each with a white twin for the flash), the button select `image/button_sel`, the song
select's EASY/HARD board `image/enjoy_ms`, the bottom bar's button guides `image/256_cmn` and the
how-to-play `image/256_tutorial` (each phrase in pieces that animate on their own, most with a
white `_BG` shape behind).
Where the rectangles start after a `system.idx`'s sprite names varies from folder to folder
(2 to 12 bytes on): the start that gives rectangles that fit the textures and don't overlap is
the right one. pop'n music 14 keeps much of its text in the program's memory too (the program
runs from 0x1700000): the song table is 700 records of 0x30 bytes from 0x193d7e8, each with its
genre at +0x10 and its comment at +0x14 (the comment line is the CHALLENGE song select's ticker,
which takes plain ASCII; ENJOY's song select has none); and around it the EXPERT courses'
descriptions, the random-song menus, the norma explanations, the card and network messages, the
characters' quips, names and items, and the story mode's narration (split across strings in the
middle of words: the highest address is read first). Leave alone the name entry's tables and
its list of refused words at the end of that area, which the game compares input with.
pop'n music 13 Carnival is laid out the same way, its program's data a little lower: the song
table is 649 records of 0x38 bytes from 0x1941940 (genre and comment pointers at the start of
each), most of its comments and messages the same text as pop'n music 14's, and its own set of
characters' reactions. Its pictures are the same kind of folders (`modeselect`, `buttonselect`,
`enjoy`, `256_card13`, `norma`, `hariai`, `expert_sel`, `result_e`), drawn in its circus style.

## Cheats

`tools/modimport.py GAME FILE OUTDIR` turns a PCSX2 patch file (`.pnach`) or a MAME cheat file
(`cheat.xml`) into mod folders, one per cheat, in the machine's byte order: copy the ones wanted to
the game's mods folder. Only plain writes come over (no conditions, no parameters); they have no
`original`, so the warning above applies.
