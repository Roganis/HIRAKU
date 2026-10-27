# Mods

A mod changes a game without changing its data: HIRAKU lays it over the game as the game runs.
Mods are off unless chosen, a game with mods keeps its saves apart from the clean game's, and the
regression cases always run without them (PLAN.md section 20).

What a mod can do is the same on every board, because it works on the parts HIRAKU emulates:

| layer | what | how |
|---|---|---|
| **memory** (`[[patch]]`) | code and data in the game's RAM | bytes at an address, put there whenever the bytes the mod expects are there (so a patch waits for code loaded later, and comes back if it's loaded again) |
| **media** (`[[bytes]]`, `[[file]]`) | anything the game reads from its discs or disks | the bytes it reads at an offset, or a whole file (by its path on an ISO 9660 disc, or on a Konami Python 1 disk), served from the mod's own files |

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
the game), in order; the command line takes `--mod FOLDER`, several in order. Where two mods
change the same thing, the first one wins, as with ifs_layeredfs (whose mods are listed by name).

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
(`<game>+<mod>+<mod>`), started as a copy of the clean game's saves the first time.

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
hz = "display"                  # the display's refresh rate (no window: 119.88), or a number (30-1000)
while_changing = 0x018ff668     # ... while this word of its memory changes from frame to frame
# while_equal = 0x00f898d8      # ... and (optional) while this word (4 bytes) holds `value`
# value = 4

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

## High frame rates (`[[rate]]`)

A game draws one picture per vsync. `[[rate]]` makes the vsync come `hz` times a second of the
machine's own time while a word of the game's memory keeps changing (two frames in a row: on;
two still: back to the machine's rate). `hz = "display"` follows the display's refresh rate, so
the game's frames meet the display's one for one (display pacing then shows each for one
refresh). The CPUs keep their clocks, so each frame has less time; a game that keeps up draws
more pictures a second. What it computes from a clock (a song's position, timing windows) stays
right and moves more smoothly; what it counts in frames (animations, effects, menu timers) would
run faster, which is why the rate is tied to a condition, and what `between_ticks` is for.

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

The Firebeat pop'n music (4-8) are another case (checked on pop'n 4, 2026-10-19, with the
Firebeat core's frame period halved by hand): the attract demo's song lasts the same number of
frames at twice the rate (about 2,400 at both), so its play goes by frames, not by a clock, and
so does DJ Main's pop'n music 1. A faster vsync would only make them faster: on those cores the
display's extra refreshes get in-between pictures instead (`--smooth-motion`, docs/SMOOTH.md),
for every game on them, with no mod.

Only the Python and DJ Main cores change their rate so far; elsewhere `[[rate]]` says so in the
log and does nothing.

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
  at ... in place`) says when the game's process has put one in. `[[rate]]` doesn't apply (the
  game draws at the cabinet's 60 a second), nor patches tied to one.
- **Files**: the PC games' mods are folders of files at their paths, as
  [ifs_layeredfs](https://github.com/mon/ifs_layeredfs) has them for the later games: a file at
  `<mod>/a/b` is the game's `data/a/b` (any case). The game opens the mod's file instead, whole,
  any length (`mods-files.txt` in the save folder lists them; the save folder's `pc.log` says when
  the game first opens each). `[[file]]` with `medium = "data"` and the file's `path` under it
  does the same from a file elsewhere in the mod. `[[bytes]]` doesn't apply (no disk image).
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
  and those left out (PLAN.md section 34; how to install a pack: [SONGS-BACK.md](SONGS-BACK.md)). Not yet: songs past the game's own (custom songs), PNGs
  in the `_ifs` folders, and their scores on HIRAKU's e-AMUSEMENT server.

- **The last 32-bit PC pop'n** (`[pc] loader = "pc32"`): a mod's folder is laid over the game's
  data folder (new files too, the first mod winning), and inside the game's packs as ifs_layeredfs
  lays them out: `<mod>/sd/system/system28_ifs/system28.2dx` is the member `system28.2dx` of
  `data/sd/system/system28.ifs`, for any pack the game mounts (a pack inside a pack too). The save
  folder's `pc.log` lists the files and members read from the mods (`mods: ...`). Its music data
  (popnhax's XML, any `.xml` in the folder) brings back the songs the game left out, each whose
  Normal chart's `sd/<folder>/<file>.ifs` and banner are there (`songs: ...` in `pc.log`). Not yet:
  songs past the game's own and charts added to the game's songs. Pictures as PNGs work too
  (`<pack>_ifs/tex/<name>.png`, as translations have them): each is converted for the game the
  first time it's shown in a run (`mods: picture ...` in `pc.log`), its size the game's, or the
  mod's own `tex/texturelist.xml`'s; not yet `dxt5` pictures, nor new pictures without a list of the
  mod's (PLAN.md section 35).

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
3.8 ms of host work a frame).

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
