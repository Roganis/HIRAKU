# Smooth notes on fast displays: how they work

The complete notes: how each way works on each board, what was measured and checked, and how to
check a new game. The player's guide is [MOTION.md](MOTION.md).

A 120 or 240 Hz display can show notes moving two or four times as smoothly as the cabinet's
60 Hz monitor. A game draws one picture per vsync, though, so the extra smoothness has to come
from somewhere. HIRAKU has two ways, and which one fits depends on how the game keeps time (and a
third, the CRT beam, for a CRT's sharpness in motion rather than more pictures):

| The game's play goes by | Way | Where | Games so far |
|---|---|---|---|
| a clock (a hardware timer read each frame) | more vsyncs: the game draws more pictures (`[[rate]]` mods) | per game: docs/MODS.md | pop'n music 9-14 (Python) |
| frames (each frame moves the notes a fixed step) | in-between pictures: HIRAKU draws them (`--smooth-motion`) | per core: every game on it | DJ Main, Firebeat, Twinkle, System 573 |
| frames, on a Windows PC | in-between pictures: the game's own Direct3D draws them again, the notes moved (`--smooth-motion`) | the PC core (runtime/pc/) | beatmania IIDX RED to GOLD (9th style to EMPRESS) |
| frames, its charts converted from milliseconds as they load | more vsyncs: the charts counted in half frames, the game at twice its rate (`[[rate]]` mod) | per game: docs/MODS.md | beatmania IIDX DJ TROOPERS, EMPRESS |
| frames, its charts in frames | more vsyncs: the charts read with their times doubled, the game at twice its rate (`[[rate]]` mod, `chart_times`) | per game: docs/MODS.md | beatmania IIDX 9th style to GOLD (PC) |

To tell which one a game is: run its attract demo (or a play script) with the vsync at twice the
rate. If the demo song takes the same number of frames (it plays twice as fast, out of step with
its music), the game counts frames. If it takes twice the frames, it follows a clock. pop'n music
4 (Firebeat) and pop'n music 1 (DJ Main) count frames; pop'n music 9-14 follow a clock. The
beatmania games were checked as they draw (below): beatmania (DJ Main), beatmania III (Firebeat)
and beatmania IIDX (Twinkle) all move their notes a fixed step a frame. beatmania IIDX 3rd style
was also run that way (2026-11-11, the play case's song with the PlayStation's vertical blank
twice as often from the stage's start): the stage's result came at the same frame number, in half
the time, the background video (which the player times) out of step. It counts frames.

## In-between pictures (`--smooth-motion`)

The game keeps its rate and runs exactly as before, at its own speed; nothing in the machine
changes. Display pacing keeps the game on the steady clock: each frame has a moment, one frame
period after the last, and gets the display's refreshes from its moment to the next frame's (at
144 Hz, a 58 Hz game's frames get 2 or 3; at 240 Hz 4 or 5; at 60 Hz 1, and every 30th frame 2).
Every refresh shows the frame with what moves carried on to that refresh's moment, a fraction of
a frame along its last move (the first refresh too: it comes after the frame's moment, up to a
refresh):

- a frame's burst of work starts a fixed time before the frame's own moment
  (`pacing::wait_next`), so the controls are read and the sound made at the game's even pace,
  as the machine did, whatever the display's rate. The pictures go out on the display's
  refreshes, the first 0 to 1 refresh after the moment. (Until 2026-11-15 the burst started a
  fixed time before that first refresh instead: on a 120 Hz display a 58 Hz game's bursts came 2
  or 3 refreshes apart, a press was taken up to a refresh sooner or later than the game's clock
  said, and the sound of a frame held a third refresh came 8 ms late, which a short sound queue
  did not cover. What the even pace costs: what a press changes on screen comes up to a refresh
  later, half of one on average, 4 ms at 120 Hz. `HK_PACING_EVEN=0` is the old way, to compare.)
  A core that hands its picture over before the frame's work (the present hook: DJ Main,
  Firebeat, the PlayStation boards) has all of the frame's pictures made then, from the machine
  as it is, and shown after that work and its sound (window.cpp, `g_shot`; under display sync
  of either kind): the wait for the refresh would otherwise stand before the sound
  (`HK_PICTURE_AFTER_SOUND=0`: shown first). The sound's queue is steered at its lowest, as the
  frame's first sound comes, not at the burst's start. On Windows the game's thread waits for no
  picture's time: the frame's own picture too goes to the swap chain's own thread with its commit
  time (`pacing::commit_queued`; `HK_DXGI_QUEUE_OWN=0`: waited for, as it was), so a commit that
  falls late in the frame, and the other pictures' drawing after it, no longer push the next
  frame's start (and its sound) late;
- `Core::render_between(t)` (runtime/shell/core.h) draws the last frame again, each moving thing
  `t` of a frame further along, into a picture of its own;
- the window (runtime/host/window.cpp) shows them on those refreshes through the same picture
  pipeline (CRT shaders and all), and commits each refresh with the compositor's fifo protocol
  (runtime/host/pacing.cpp: `frame_exact` gives the frame's refreshes, `phase(i)` each one's
  moment, `commit_extra` commits it). The rest is display sync's (the commit's lead, the burst
  budget, the grid kept on the compositor's vblanks; the sound steered to its queue at speed 1).
  On Windows there's no fifo: the display's vblanks (runtime/host/dwm.cpp: the graphics kernel's,
  else DWM's estimate) give the last vblank and the refresh period, and each refresh's picture is
  committed at its own commit time. The pictures go through a DXGI flip-model swap chain
  (runtime/host/swapchain.cpp: OpenGL draws into a Direct3D texture, WGL_NV_DX_interop2), which keeps
  each picture's turn as fifo does and says when it reached the screen: the lead is tuned from that
  as on Wayland. A frame's other pictures are all drawn right after its own, each kept in a
  Direct3D texture of its own with its commit time, and a thread of the swap chain's presents them
  then (Direct3D only on it), so the machine's thread goes on at once, as with fifo. Waiting for
  each on the machine's thread, as it first did, left a frame's emulation one refresh to fit in
  whatever the frame's length (7 ms at 120 Hz, 3 at 240): `HK_DXGI_NO_QUEUE=1` brings that back, to
  compare. The exit report says how near their times the thread's pictures went (`video: of them N
  in-between pictures presented at their times by the swap chain's own thread`). Checked against a
  pretend Direct3D device and display (tests/swapchain, under Wine); on a real Windows graphics
  card it is still to be run. Where the driver can't share OpenGL's pictures (Wine), OpenGL's own
  swap through DWM, the lead half a refresh, each picture waited for on the machine's thread.
  Elsewhere (X11, or Wayland without fifo-v1), or with display sync off, nothing changes;
- the exit report says how many it showed (`video: smooth motion: N in-between pictures shown`).

Use it: `--smooth-motion` (display sync on, the default), or the launcher's Settings > Display > "Smooth
motion (DJ Main and Firebeat games)". Any refresh rate works, 144 Hz too, and the game keeps its
exact speed and pitch (without smooth motion, display sync speeds pop'n 4-8 up 2.6 % to fit
60/120/240 Hz, and leaves DJ Main's 58.00 Hz, 3.4 % off, juddering). The faster the display, the
smoother; at 60 Hz it mostly evens out the frame that would be held twice every 30 frames.
`HK_PACING_DEBUG` prints each frame's refreshes and their moments.

### In songs only (`--smooth-motion songs`, the default)

The in-between pictures are a guess one frame ahead, and menus are where it guesses worst: text
redrawn with other letters (a letter paired with another of the same height, carried on), panels
that slide in and stop (carried a frame too far), cursors that bob (carried past where they turn),
lists that wrap. beatmania III's sound select vibrated where nothing moved (a player's report,
2026-11-21: the coin text every 60 frames, the cursor's arrow every 20, a title card as it lands),
and the menus gain little from smoothness. So a game can say, by a word of its memory, when a song
is being played, and smooth motion is only on then:

```toml
[smooth]                       # in games/<game>/game.toml
while_changing = 0x80107f30    # the word that changes every frame while a song plays
# while_equal = 0x...          # and (optional) while this word holds `value`
# value = 4
```

The condition is a mod's `[[rate]]`'s (docs/MODS.md; `runtime/shell/wordcond.h`, shared): on once
the word has changed on two frames in a row, off after two still ones, and with `while_equal` only
while that word holds `value`. It's read from game.toml, not a mod, so the saves' folder stays the
game's own. Outside a song (`runtime/shell/smoothgate.h`, once a frame where the mods' patches go
in):
- no in-between pictures are asked for, and each frame is held for its refreshes. How display
  pacing runs then depends on how far the game's rate is from the display's: locking the game to a
  whole number of refreshes a frame changes its speed, and its music's pitch, by the frame's period
  over theirs. Where that change is 0.5 % or less, the game is locked as without smooth motion
  (beatmania III's 59.94 Hz: 0.1 % faster at 60, 120 or 240 Hz), and at the switch the next frame
  starts afresh on the display's refreshes (`pacing::set_smooth_motion`). Where it is more, the
  game keeps its exact speed outside songs too, the pacing smooth motion uses, each refresh showing
  the latest real frame: Firebeat's pop'n music (58.49 Hz) would run 2.6 % faster on the menus than
  in the songs, a step in the music's speed at every song's start and end, and DJ Main's 58.00 Hz
  (3.4 %) is further off than display sync takes at all (more than 3 %: frames shown as they come).
  So nothing changes speed at the switch, and the frames' moments go on across it
  (`shell/smoothgate.h` `exact_outside_songs`, decided in `pacing::frame`; a 144 Hz display, 2.4
  refreshes a 59.94 Hz frame, is exact outside songs too). The exit report in a window says which
  (`video: outside songs: N frames at the game's exact speed, M locked to the display (locking
  changes its speed +2.58 %: more than 0.5 %, kept exact)`);
- the core is still asked for one in-between picture a frame, not shown, so what it pairs from one
  frame to the next goes on as with smooth motion everywhere: a song's first in-between pictures
  are those smooth motion everywhere makes, bit for bit (checked below). It costs one picture's
  drawing a frame on the menus (an in-between picture of beatmania III: 1.4 ms, "Costs" below),
  against the three or so a 240 Hz display asks for.

`--smooth-motion everywhere` (Settings > Display > Motion > "Smooth motion everywhere") is the way
it was, on every screen. A game without `[smooth]` has it everywhere either way. The run log says
which (`smooth motion: in songs only (while the word at ... changes ...)` or `smooth motion:
everywhere (...)`), and the exit report how many frames had it: headless `smooth motion in songs:
in-between pictures on N of M frames`, in a window `video: smooth motion in songs: asked for on N
of M frames shown`.

| Game | Word | What it is | Open in the attract's How to Play |
|---|---|---|---|
| beatmania III THE FINAL (`bm3final`) | 0x80107f30 | the stage's frame counter: 0 on the sound select, +1 a frame from the stage's title to its end, held after | yes |
| beatmania III (`bm3`) | 0x801062a8 | the frame within the bar, +1 a frame through a song, back to 1 at each bar | yes |
| beatmania III APPEND CORE REMIX (`bm3core`) | 0x801079f0 | the same (a frame now and then without its step: one still frame, which the rule rides over) | yes |
| beatmania III APPEND 6th MIX (`bm36th`) | 0x80108bc8 | the same (as CORE REMIX) | yes |
| beatmania III APPEND 7th MIX (`bm37th`) | 0x80109e80 | the same | yes |
| pop'n music 4 (`popn4`) | 0x8014646c | the song's chart position, +3 to 5 a frame from the stage's first frame to the song's end | no |
| pop'n music 5 (`popn5`) | 0x801406d0 | the same (+4 or 5 a frame) | no |
| pop'n music 6 (`popn6`) | 0x80122fe8 | the song's frame counter, from the notes' first frame (normal and battle mode) | no |
| pop'n music 7 (`popn7`) | 0x80216c94 | the song's frame counter, from the notes' first frame | no (none seen) |
| pop'n music 8 (`popn8`) | 0x8021825c | the song's frame counter, from the notes' first frame | no |
| pop'n music Animelo (`popnanm`) | 0x802ab8dc | the song's frame counter, from the stage's start (after READY GO) | yes (demo play) |
| pop'n music Animelo 2 (`popnanm2`) | 0x802b3d7c | changes every frame from the stage's start (after READY) to the song's end; what it is not known | none seen |
| pop'n music Mickey Tunes! (`popnmt2`) | 0x802af674 | the notes' scroll position (a float), from the notes' first frame | no |
| ParaParaParadise (`ppp`) | 0x801018d0, while_equal 0x801025c0 = 1 | a counter of the stage's frames, and the play screen's flag, which drops as STAGE CLEAR (the result) comes over the play screen | none seen |
| ParaParaParadise 1.1 (`ppp11`) | 0x80101930, while_equal 0x801026e8 = 1 | the same | none seen |
| ParaParaDancing (`ppd`) | 0x80101ac8, while_equal 0x80102e84 = 1 | the same | none seen |
| pop'n music (`popn1j`, DJ Main) | 0x404a28, while_equal 0x40749c = 1 | a word of the play screen, changing every frame from the notes' first frame (the credit's instruction stage too), and a word that is 1 only then | no |
| pop'n music 2, 3, pop'n stage EX (`popn2`, `popn3`, `popnstex`) | 0x404508 | the song's clock (16.16, +1.0 a frame), from the notes' first frame | yes (demo play) |
| beatmania (`bm1stmix`) | 0x4071d8 | the stage's clock (16.16, +1.0 a frame), from the play screen's first frame; the practice stage too | yes (demo play) |
| beatmania 2nd MIX and its other version (`bm2ndmix`, `bm2ndmixa`) | 0x4073f0 | the same | yes |
| beatmania 3rd MIX and its other version (`bm3rdmix`, `bm3rdmixe`) | 0x4079d4 | the same | yes |
| beatmania 4th MIX (`bm4thmix`) | 0x4064ec | a word of the play screen, changing every frame of a stage; open on the rankings screen too (about 400 frames): the only word found that runs through every stage | yes |
| beatmania 5th MIX (`bm5thmix`) | 0x407d3c | the stage's frame counter (+1 a frame) | yes |
| beatmania 6th MIX, 7th MIX, THE FINAL (`bm6thmix`, `bm7thmix`, `bmfinal`) | 0x406554 | the stage's clock (16.16) | yes |
| beatmania Club MIX, complete MIX 2, featuring Dreams Come True, hiphopmania complete MIX 2 (`bmclubmx`, `bmcompm2`, `bmdct`, `hmcompm2`) | 0x407d7c | the stage's frame counter (+1 a frame) | yes |
| beatmania complete MIX, JA-B (`bmcompmxb`) | 0x407934 | the stage's clock (16.16) | yes |
| beatmania CORE REMIX (`bmcorerm`) | 0x407d40 | the stage's clock (16.16) | yes |
| hiphopmania complete MIX (`hmcompmx`) | 0x406254 | changes every frame of the play screen; open on the final stage's title card too (as THE FINAL's from the stage's title) | yes |

The beatmania III words were found by RAM dumps (three frames in a row every 600 frames through two credits and the
attract) and the words that change on every frame of each stage and on none of the menus', and
then checked through two credits (beatmania III APPEND 7th MIX: two stages of one credit) with the
word watched every frame and the rule worked out from the log: open from each stage's start (its
title) to its end, closed on the sound select, between stages, on the results, the continue, the
rankings and the title; open in the attract's How to Play, which plays a song as a demo. Other words
looked right in one credit and weren't: CORE REMIX's and 6th MIX's stage frame counters ran in the
first credit's stage and not the second's, and one of beatmania III's opened for a few frames on
the menus.

The others (2026-11-21) the same way: a run through two credits with the buttons mashed (no
fail-safe needed: pop'n music 4, 5 and the DJ Main games end the credit at the first failed stage,
so each credit has one song there, the others two or three), RAM dumped at three frames in a row
every 900 frames, the words that change at both steps of every song's dumps and at neither of the
menus' grouped by where they change; then the best few watched every frame through the same
credits, and the rule's spans checked against pictures of the frames around each song's start and
end: open within a frame or two of the notes' first frame (pop'n music 1-8 and the anime games) or
of the play screen's (beatmania, ParaParaParadise: the notes come later), closed before the result.
ParaParaParadise's words change through STAGE CLEAR too (the result comes over the play screen),
so a flag of the play screen gates them (`while_equal`: `value` is the word as the machine reads
it, big-endian on Firebeat and DJ Main, `Core::mod_big_endian`). pop'n music 1's best word alone opened for 2 to 8 frames a few times in the
attract; a word that is 1 only in play gates it. Each game's run with `--smooth-motion` then counted
the frames with in-between pictures, the same number as the rule worked out from the watched word
(`out/smooth-more`, local). Left without a word (smooth motion everywhere, as before):
ParaParaParadise 1st Mix Plus, Keyboardmania and Keyboardheaven (no set-up save here to get past
their first-boot errors), Keyboardmania 2nd Mix (its best words stop for 3 or 4 frames inside a
song), Keyboardmania 3rd Mix (its stage word runs on under the RESULT panel, 200 frames, and no
clean flag of the play screen was found to gate it), the DJ Main sets whose media isn't here (`popn1`, `popn1a`,
`popn1k`, `bm3rdmixa`, `bmcmxaac`, `bmcompmx`, `bs4thmix`, `bscompmx`: pop'n music 1's word is
likely theirs, not checked), and the Twinkle and System 573 games. The menus do misbehave on the
Firebeat games without the gate: pop'n music 8's music select drew 207,927 pixels moving that the
game holds still in 150 frames (`tools/still.py` as below; THE FINAL's idle select: 19,942 in 300),
ParaParaParadise's song select 115,245 in 150. DJ Main has no picture dump to measure it with.

Checked on THE FINAL (headless, `HK_BETWEEN_SNAP`, which goes by the same rule): the sound select
left idle (300 frames) and scrolled to the folders (160) make no in-between pictures, where smooth
motion everywhere drew 19,942 and 80,806 pixels moving that the game doesn't move (the coin text,
the cursor's arrow, the title card landing); a song's in-between pictures (5140-5200, 6000-6030) are
the same, bit for bit, in songs, everywhere, and before this change. Not yet seen on a real 120 or
240 Hz display: the switch at a stage's start and end under fifo-v1 or DWM (the headless rig has
neither, so its pacing was the same on both sides).

Checked in a virtual KWin (60 Hz): pop'n 3 and pop'n 4 at their own 58.00 and 58.49 Hz, every
refresh on time, each refresh's moment 1/30 of a frame on from the last's (60 against 58 Hz).

It's a guess one frame ahead. Notes scroll at a steady speed, but in whole pixels: pop'n music 8's
step 2, 3, 3, 2, 3, 3 pixels a frame. Carried on by its last step, a note went 3 on after a 3, the
next frame moved it 2, and it stepped back a pixel: at 240 Hz one refresh in twenty went back, and
the notes were 0.57 px (sd) off a steady line, which showed as judder (owner, 2026-10-20, pop'n
music 8 and beatmania IIDX 7th style). So each core takes the move most of a frame's moving
objects share, within a pixel (notes step 2 or 3 in the same frame; a lyric rising beside them is
a smaller group), as the scroll, its speed their average, keeps its position over the last six frames, and carries what moves
with it along the steady line through them (include/hiraku/scroll_guess.h): the average speed,
from where the line puts this frame, up to a pixel off the frame's own place (so the frame's first
refresh is drawn too, not shown as it is). The notes are then 0.29 px off the line (the whole
pixels' own rounding) and never step back (tests/smooth, at 1 to 4.6 pixels a frame, 144 and 240
Hz; pop'n 8's How to Play: the falling notes on the line, the rising arrows on their own). What
doesn't move with the scroll still goes by its last move: where something stops, turns or
appears, its in-between pictures carry it on up to a frame too far (at most a refresh's worth of
time) before the next frame puts it right.

At hi-speed (owner, 2026-10-20: pop'n music 8 at x4, about 700) a note or two may be all that
scrolls, and slower objects outnumber them: in FRESH at x4 (176 BPM) one note falls 8.4 pixels a
frame while three objects move 2, and those became the scroll. The note then went by its last
step, which jitters with the game's own positions (8, 10, 8, 8, 10, 8, 7), so each frame's pictures
started up to 2 pixels off the last's. So every object also keeps its own track through the
frames, by the pairing below: the least-squares line through its positions since it appeared,
older ones counting less (a tempo change is followed within a few frames), started again where
it leaves it by more than 4 pixels. What doesn't move with the scroll goes along its own line.
Measured in the window (quarter-frame pictures of 120 frames of FRESH at x4, every note followed):
0.35 px off its line, none back (it was 1.00 px, five back).

Which object was which in the frame before is a pairing of look-alikes. The first took the nearest
within 16 pixels; something fast can be further on, or nearer another look-alike's last place. So
each object's look-alikes within 64 pixels are listed, the scroll is voted first (every object
counted once near each move it could have made), and each object takes the look-alike nearest to
standing still or to the scroll's move, within 16 of either. Two things keep it from taking one
object for another (most likely the owner's ghost notes beside real ones in beatmania IIDX 7th style,
2026-10-20; seen in its play case, not in the owner's play):
- standing still is voted too: an object with a look-alike at its own place (its picture changed)
  votes for that, and a scroll needs more votes. A row of identical sprites 20 pixels apart whose
  picture flickers otherwise voted for a move of 20 sideways, each then paired with its neighbour;
- a look-alike with a steady line of its own is taken only near that line (within 8 pixels), or
  with the scroll: IIDX's key flash, lit in one lane and then the next 16 pixels on, was drawn
  halfway between the lanes. Now it just appears in the next, as the game draws it.

The pairing is done once a frame on the PlayStation GPU (each refresh's picture shares it), each
primitive compared only with those of the frame before with its command and colour in its 64-pixel
cell and the eight around, and the votes counted in a grid of moves: a screen of 943 identical
tiles between scenes had made one in-between picture 19 ms, four or five a frame at 240 Hz (the
owner's lag between scenes); now at most 4.3 ms, mostly the drawing.

A game's resolution still sets the steps: beatmania IIDX draws 240 lines, and at normal speed its
notes move one line a frame, so there's no place between; the guess then puts that line's step at
the same point of every frame. The faster the notes (hi-speed), the more the in-between pictures
add. The judgement, the sound and
the latency are the game's own: the in-between pictures add nothing between pressing a button and
the game seeing it.

### DJ Main (runtime/djmain/board.cpp, `render_between`)

The 68020 builds a sprite list in object RAM each frame (the K055555 mixer shows it with the
K056832's tilemaps). The list is rebuilt each frame, with sprites in different slots from one
frame to the next. So each sprite is paired with one of the frame before that uses the same tile
(the pairing above), and moved on by that step. The picture is drawn from a copy of
object RAM and into a buffer of its own, so the machine is never touched. Checked on pop'n music
1 (notes are sprites moving 2 pixels a frame): halfway, the notes are 1 pixel on, nothing else
moved. beatmania (1st mix's "u gotta groove"): the notes, the bar lines and the FREE ZONE panel
are sprites too, and move half a step. The tilemaps (backgrounds) aren't carried on, and needn't
be: through pop'n 1's and beatmania's play cases their scroll registers change 3 times in all (the
registers the games keep changing are the RAM page and ROM bank selects).

**Between two pixels (2026-11-16).** pop'n music 3's notes move 2 pixels a frame on its 384 lines.
In whole pixels an in-between picture had one place for them, a pixel on, so at 240 Hz the four
refreshes of a frame showed them 0, 0, 1, 1 pixels on: what 120 Hz shows (owner: "smooth frames
doesn't work for 120/240 Hz on a 58 Hz game like pop'n 3"). Now each moving sprite's place is taken
unrounded (`ScrollGuess::offset_exact`, `on_line`): along its move's main axis it goes at the whole
pixel below (the other axis rounded), and the picture is drawn again with every sprite that is a
fraction on a pixel further. Where the two pictures differ, each pixel is mixed by the fraction (in
16ths) of the sprite that drew it there, in linear light (the sprite drawing notes which sprite drew
each pixel: `g_owner`): a note 1.5 pixels on has its edge half lit, which a fast display shows as a
note between the two pixels. Two pictures however many fractions (the first version drew one more
picture per fraction and kept only the most shared, the others rounded).

Why mixing, not a bigger picture: the CRT shaders work on the game's own lines (a picture of 4 times
the lines would be 4 times the scanlines), and the sharp-pixels look on whole pixels. The picture stays
the game's size; a moving note's edges are softer on the refreshes between its pixels, and sharp again
on those that land on one.

Checked: pop'n 3's demo play, `--snap-between 4`: each quarter frame now differs (before: the
quarter and three-quarter pictures were the frame's and the half's), and a note's brightness centroid
moves in steps of about a third of its whole-pixel step each quarter, instead of 0, 0, 1, 1.
`HK_BETWEEN_BENCH=4` over 8000 frames: 0.47 ms a picture, worst 2.5 (whole pixels: 0.34, worst 1.19).
`HK_BETWEEN_SUBPIXEL=0` gives whole pixels, bit for bit as before (the bench's hash; the 573's
in-between pictures, which use the same scroll guess, are unchanged too). Not yet seen on a real
120/240 Hz display.

**Notes through the game's own hitches (2026-11-16).** The owner saw pop'n 3's notes judder "connected
to the combo, the judgement or the light beams", the first notes of a song (nothing pressed yet)
smooth. Logged sprite by sprite (`HK_DJ_BETWEEN_LOG`, `HK_DJ_SCROLL_LOG`, `HK_DJ_PLACE_LOG`: each
sprite's guess, the scroll's line each frame, each sprite's drawn place), four things broke the line:

- the game misses an update for its notes now and then, most while it judges: some or all stand still
  a frame, then move twice as far (2, 2, 0, 3.3, 2). A frame with nothing moving threw the line away,
  and its first frames back had the speed up to a pixel a frame off;
- the judgement and combo sprites rising together (4 to 6) outvoted the 2 or 3 notes, and the key
  effects moved the notes' way while every note stood still;
- a note at the judgement line is held there while judged, and was carried on past it, then jumped back;
- notes change tile now and then (an animation frame: 600, 584...), and pairing went by the same
  tile: 967 times in the demo song a note was unpaired, off the line, standing still a frame.

Now (`ScrollGuess::follow`, DJ Main only so far): the line's own objects carry it on, however few or
outnumbered: its move is their moves near what it would move (those standing still or catching up
left out), most of them standing still a missed update, the line going on through it. A sprite joins
the line after two frames moving with it (pop'n 3's key effects move the notes' way for one), and
leaves it when behind by more than one update (it has stopped). Each keeps how far it is behind the
line (lag) and its usual lag (its steady place); how far it is off that now is split into the line's
own error (the median over all of them, taken off all of them alike, a quarter a frame) and its own
(rounding, smoothed into its steady place; a missed update left out). A note is drawn at its steady
place on the line, so its own rounding and missed updates neither move it nor widen or narrow its
distance to the next (the owner: "the notes seem to not all move at the same speed, their distances
sometimes widen or lessen"). A sprite left without a look-alike after the pairing may take an
unclaimed one of another tile at its x within 8 lines (an animation frame). Measured on the demo
song's notes, each quarter frame's drawn place (`HK_DJ_PLACE_LOG`):

| | steps off the note's speed by > 0.3 / 0.6 / 1 px | distance between two notes, sd (mean, worst tenth) |
|---|---|---|
| the game's own frames | | 0.45, 0.56 px |
| the vote alone (before) | 5,398 / 2,919 / 609 | 0.49, 0.67 px |
| now | 242 / 68 / 16 | 0.20, 0.31 px |

The line's speed in that song now stays at 1.57-1.75 pixels a frame (p1-p99); before it restarted
every few frames. An in-between picture: 0.50 ms (worst 2.3).

**Half a sprite list (2026-11-16).** The owner: "sometimes the notes seem to all disappear for a
frame". The game rewrites its sprite list in the vertical blank, and the window's picture is taken at
the blank's end (the monitor's, `set_present_hook`); when busy the game was still writing then, and
the picture had part of the list (pop'n 3's demo song: 4 of 13 notes at frame 6929, 1 of 3 at 6494).
Now, while it wrote the list in the last 2 lines, the run goes on half a line at a time (40 lines at
most) until it's done, and the picture keeps the blank's end as its time (`g_present_at`: taken at the
machine's clock, the frames' lengths varied by up to 0.6 ms and their in-between pictures with them).
The machine is the same (frame and sound digests identical; `HK_DJ_LIST_WAIT=0`: as before). No frame
lost its notes after, and many of the "missed updates" above were these: steps off by more than 0.6
px 68 -> 3, the distance between two notes 0.13 px sd (the game's own frames 0.40).
`HK_SCROLL_HOLD=0`: the vote alone, as before. Firebeat, Twinkle, 573 and the PC games unchanged
(their in-between pictures bit for bit the same).

The same would do for the other cores, each drawing its picture twice where it moves objects:
Firebeat's GCU does since 2026-11-16 (below, "Between two pixels"; the mixing shared,
include/hiraku/subpixel_mix.h). The PlayStation GPU's (`hk_between`, the primitives' vertices) not
yet: line-following there needs it to carry on what it didn't see move (a note that missed an
update stands still), measured worse on IIDX 8th style's big steps as it is.

### Firebeat (runtime/dev/k057714.cpp, `draw_between`)

The GCU draws with commands (fill a rectangle, copy an object from VRAM to a frame buffer with
scale, flip and blending, draw a character), from display lists in VRAM or written to its FIFOs.
The frame buffers are in VRAM too, and its four layers show them. The games draw a frame into a
back buffer and flip at vsync. Once in-between pictures are asked for, the GCU keeps, for each
frame:
- its commands, in order, and the frame buffer origin as it began;
- before a command first draws on a VRAM row in that frame, what the row held.

An in-between picture then puts those rows back as they were before the frame was drawn, runs
the frame's commands again with each object moved on, composes the layers as the display shows
them, and puts the rows back as they are. Each object command is paired (above) with one of the
frame before's with the same flags and either its picture or its height (a note going off the lane
is clipped shorter; an animated one changes picture), whatever its width and horizontal scale
(pop'n 8's notes squash and stretch to the beat, a pixel narrower or wider, and at x4 one in a
few frames changed picture as well: unpaired, it stood still for a frame). Where an object is, for
this:
- its middle: pop'n music 8's notes squash and stretch to the beat about their middle, so their
  left edge moves while they don't;
- unless only its size changed: the same picture from the same left or top edge, longer or shorter,
  is resized there, not moved (its middle moves). beatmania III draws its lane covers (TRANSLUCENT)
  as strips blended over each other, the top one from row 0 and 186 and 178 rows tall by turns
  with the strips under it 8 rows higher or lower: carried on by its middle, it went 2 rows over the
  next strip halfway, and those rows were blended twice, a dark line flickering at the strips' edge
  (issue 15, 2026-11-14; tests/gcu/test_gcu_between.cpp). pop'n music 8's How to Play cuts an arrow
  off from the right this way: it now stays where it is as it goes, as the game draws it;
- and a blended object only moves as the same picture: paired with one of another picture (only
  as tall), it stays where the frame has it. beatmania III's lanes on FRAME SEPARATE with
  TRANS-LUCENT on fade in a stack of strips, 6 rows tall and 15 or 16 apart, each its own picture,
  the stack 8 rows on by turns: each strip paired with the frame before's next one, all were carried
  8 rows and blended twice where they met, dark bands flickering on the lanes (issue 15, 2026-11-15);
- where it reaches the screen: pop'n 8 draws its notes into a strip of VRAM (x 608-760) that a
  later object copies onto the lanes, the strip a few pixels sideways from frame to frame. An object
  inside a later one's source rectangle is placed through that copy (its offset turned back into
  the strip by the copy's scale), and the copy's rows on the screen are drawn again with it, at
  the frame buffer's origin when the copy is drawn relative to it, as it is. A copy's two sides
  are drawn again together, row for row: a screen row it writes needs the strip row it reads (else
  it copies the strip as the frame left it, and a slice of the note at its old place shows under
  the moved one: the owner's "glitch under every note"), until nothing changes: in the buffer at
  row 0 the strip's rows are screen rows too (rows 0-316 go to 29-345), a chain 29 rows a link,
  and two passes left the chain's end as it was (notes split across, at x4);
- in the frame on the screen: the one whose frame buffer the first layer shows (a frame's buffer:
  where its first command, its fb_config, puts the origin). It's the last one drawn; a game drawing
  a frame ahead would show the one before, its objects then going towards the next one's.

All three came from the owner's test (2026-10-20, pop'n music 8 at 240 Hz: "still not smooth",
then glitches under the falling notes): in play, the notes went unmatched (squashed), or matched
in VRAM with the strip's sideways shift as a move (so they were carried sideways, and the copy,
not drawn again in one of the two buffers, left them where they were). Checked in the window: 30
frames of the demo play captured with `HK_PICTURE_SNAP=FIRST-LAST,FILE`, with and without
smooth motion; a falling note's place is 0.60 window pixels off a steady line in time with it (the
pixels' own rounding) and 0.67 off a line in whole frames without it, as it should. Every refresh's
picture can be captured too (`FILE-<frame>-<n>.ppm`). (Headless
snapshots don't show Firebeat's in-between pictures right: the frame hook comes before the
game's flip.)

Checked on pop'n music 4 (the attract demo's notes, objects moving 2 pixels a frame) and pop'n
music 8 (the How to Play, notes and sliding arrows): halfway, only the moving objects differ, 1
pixel on. The regress cases are identical with it built in: nothing is kept until a picture is
asked for. Costs: the rows a frame draws on are copied each frame (about 2.3 MB for pop'n's
three double-buffered layers). An in-between picture redraws only the rows where something moved
(the moved objects' rows before and after; each command keeps the rows it drew on, writes
elsewhere are dropped and commands that reach none of them skipped), and of each, the columns the
frame drew on: on pop'n 4's demo 0.2 ms a picture, from 1.1 for the whole frame, the pictures
identical (pop'n 4 and 8 compared). Where nothing has moved a whole pixel yet, there's no
redraw at all: the frame itself is shown (a note moving 1 pixel a frame is 0 a quarter on).

#### Between two pixels, and the scroll's line (2026-11-17)

As DJ Main's (above): the objects of the scroll's line go along it (`ScrollGuess::follow`, on here
too; `HK_SCROLL_FOLLOW=0` off), and each moving object is drawn at the whole pixel below its place
along its move's main axis, the frame then replayed a second time with each such object a pixel
further on, the two pictures mixed pixel by pixel, in linear light, by the fraction (16ths) of the
object that drew the pixel (`HK_BETWEEN_SUBPIXEL=0`: whole pixels, one replay, as before). Who
drew a pixel: in a replay the GCU notes, for each row drawn again, which command drew each of its
pixels (`own_object`, before the command draws; fills and characters as "none"); an object that
isn't moved a fraction passes on who drew its source's pixel, so pop'n music 8's notes, drawn into
the strip and copied onto the lanes, keep their own fraction on the screen. The layers composed as
`draw()` does give each pixel of the picture its drawer (`own_screen`).

pop'n music 8's demo play (notes 2.33 pixels a frame, the game's own steps 2, 3, 2), every
refresh's picture at 240 Hz (`HK_BETWEEN_SNAP`, the window's first refresh as the window draws it),
each note's middle tracked down nine lanes, about 500 steps (a step should be 0.58 pixels):

| | steps' sd | off by > 0.3 px | > 0.6 px |
|---|---|---|---|
| whole pixels (before) | 0.47 | 91 % | 1.0 % |
| whole pixels, along the line | 0.48 | 91 % | 1.3 % |
| between two pixels, along the line | 0.16 | 5 % | 0.8 % |

Whole pixels the steps are 0 or 1 whatever the guess; the guess itself (`HK_SCROLL_LOG`) evens
out along the line (its quarter steps off by > 0.3 px 15.8 -> 1.3 %, the distance between two
notes 0.77 -> 0.25 px sd, the game's own frames 0.64). Two fixes came with it: a note a hair
behind its place (-0.06 px) goes a pixel back and 15/16 of the way on, and the picture a pixel
further had kept it a pixel back too (both pictures the same, the note a pixel behind in one
refresh of two frames); and the line's speed is now a least-squares fit through its kept
positions (`ScrollGuess::line_speed`; `HK_SCROLL_ENDS=1`: from the first and last, as before),
which with steps of 2, 3, 2 took 2.2 or 2.4 by turns (steps off by > 0.3 px 10.9 -> 5.2 %). On
steady steps (DJ Main's pop'n music 3, DDR Extreme's arrows) the two give the same pictures, bit
for bit. beatmania III THE FINAL to its How to Play (notes 2.25 pixels a frame): the guess's
quarter steps off by > 0.3 px 0.8 %, drawn in whole pixels 89.8 % (> 0.6 px: 41.1 %).

Costs (`HK_BETWEEN_BENCH=4`): an in-between picture of pop'n music 8 0.24 ms whole, 1.2 ms
between two pixels (two replays, who drew what noted in both); beatmania III (640x480) 0.44 and
1.4 ms. The frame on show is composed once (`m_composed`, again only when the frame, the layers or
the next frame's drawing change), and a picture composes only the screen rows that show a row
drawn again (beatmania III: 0.63 -> 0.44 whole, 1.9 -> 1.4 between two pixels; the pictures the
same, bit for bit). At 240 Hz at the game's own speed the window asks for four a frame: about
5 and 5.7 ms of the frame's 16.7. With `HK_BETWEEN_SUBPIXEL=0` the replay is as it was (the line-following
pictures of pop'n music 8 the same, bit for bit, before the line's speed changed). Blended objects
leave a pixel's drawer as it is (what shows through is what moves).

#### beatmania III's gauge and song list (issue 15, 2026-11-17)

A reporter's comparison of the window's raw pictures (III THE FINAL, 120 Hz) found two causes; a
third came up checking them. All three in `tests/gcu/test_gcu_between.cpp` and
`tests/smooth/test_scroll_guess.cpp`:
- cut off, not moved: the life gauge's left edge and its picture's left edge go on together while
  its right edge stays (x 240, 192 wide, its picture from x 672; then x 244, 188 wide, from 676).
  Carried on by its middle, it shook a pixel or two each way as it filled and emptied. An object
  whose edge and picture edge moved together, the far edge staying and the picture the same the
  other way, isn't moved on that axis (the same-edge rule above is its case of no change at all).
  pop'n music 8's How to Play has one arrow shown more from its right this way, now kept still too;
- a copy that moves carries what's in it: the titles are drawn into strips off the screen (rows
  13392-13791) that stay put, and each strip's copy scrolls. A title's move on the screen has its
  copy's in it, and it was moved in the strip by all of it too: the titles went twice as far as
  their rows (in whole pixels, a 1 pixel step's halfway picture matched the frame best 2 pixels on,
  21 % of the list still off; now 1 pixel on, what's off the panels that stay). An object drawn
  through a copy is now moved in the strip by its own move less the copy's;
- the copies' origin: the song select sets the frame buffer's origin twice a frame (row 0 then 480,
  or 1984 then 2464), and the copies' rows on the screen were taken from the first, so their strips
  weren't drawn again with them, and a moved row showed the next one's title. A copy's rows are now
  where it drew them (each command's own rows);
- a jump isn't the scroll's line: the list wobbles a pixel a frame and then snaps 29 at once. The
  line taken through that put the rows 15 pixels from where the game drew them, the frame's own
  picture too, and they shook. When the line's objects move far off it (more than 4 pixels, and
  more than twice its move), the line starts again, and what jumped stays where the frame has it
  for that frame (`HK_SCROLL_JUMP=0`: as before). pop'n music 8's attract has such a jump every 60 frames (the
  bobbing sea of its map screen), now drawn a little differently for about 40 frames after, by up
  to a pixel; its demo play's notes, pop'n music 3 and beatmania III's attract are the
  same, bit for bit.

A later report (THE FINAL's song select, 120 Hz, scrolling fast through songs and folders: folder
pictures and titles sliding against their rows, the list's top row and its border shaking, slices
of the next row along its edge) was the list's rows themselves. The list is rows of 40 drawn edge to
edge (a background copy and a strip copy each), the top one cut off by the game at row 0 (shorter,
from further down its picture: no look-alike, so never paired) and, as it scrolls, a row just come
in (unpaired too). The rest went on along the scroll's line, up to 10 pixels from where the game
drew them, over those two that stood still: in every picture, the frame's own included. The list
goes 8, 0, 16, 16 rows a frame, so it tears again and again.
- Rows edge to edge (the same columns, one's bottom the next one's top, each at least twice as wide
  as tall; or the same turned) are one surface, a list: moved along it unlike each other, none of
  it moves, nor what's drawn through them, nor a row lying over one of them (the cursor's: the row
  under it drawn again into a strip of its own and copied over it), and they stay still 8 frames
  after (`HK_BETWEEN_TILES=0`: as before).
- Rows only, and all of the surface rows: a word's letters side by side, some kept and the others
  carried on, went apart; and beatmania III's backgrounds are strips side by side (4, 128 and 508
  by 480, scrolling sideways, the 4 at the wrap unpaired): the two narrow ones kept still opened a
  seam where they met the wide one, still moving. Such a surface moves as before.
- The trade: while THE FINAL's list scrolls it keeps the game's own steps, as at 60 Hz (the cursor's
  arrow and the rest of the screen still have their in-between pictures); nothing tears.
`tests/gcu/test_gcu_between.cpp` has such a list. The rule changed little else: pop'n music 8's and
beatmania III's key flashes (a button squashed 1 or 2 pixels, drawn as two rows) and beatmania III
6th's stage banner as it opens and closes now keep the game's own pictures for those frames.
A change to ScrollGuess was tried too (an object's very look-alike at its very place taken as
standing still, never a jump off its track) and dropped: the list didn't need it, and objects that
slid in and stopped (THE FINAL's ranking screen) went on along their line for 3 frames, over each
other.

Not covered:
- Keyboardmania (two GCUs, one a monitor): no in-between pictures.
- A game drawing a frame over two vsyncs (30 frames a second) moves its objects per two vsyncs,
  which the GCU's in-between pictures don't know (the PlayStation GPU's do: below): they would
  spread a frame's move over one vsync. Not seen: pop'n music 4 and 8 and beatmania III draw a
  frame every vsync (beatmania III's play: about 78 commands each).
- Objects drawn from the CPU straight into VRAM (not through commands) aren't moved.

### Twinkle and System 573 (beatmania IIDX, DDR, DrumMania, GuitarFreaks; third_party/mame/hiraku-edits.py, `hk_between`)

The PlayStation's GPU draws primitives (polygons, lines, rectangles and sprites, from VRAM
textures) into VRAM; HIRAKU's edit of MAME's GPU already queues each one with a copy of all it
draws from (its packet and the drawing state) for the drawing thread. Once in-between pictures are
asked for:
- the queued items are kept, a frame's from one flip to the next: a write of a new display start
  (the game shows what it drew since its last flip). How many VBLANKs a frame lasts is counted
  too: DDR 2ndMIX draws at 30 frames a second, so its in-between pictures move a quarter, then
  three quarters, of a step. A game that never flips (it draws where it shows) has a frame each
  VBLANK.
- the drawing thread keeps what each upload (CPU-to-VRAM, MoveImage, fill) writes over: while a
  frame shows for two VBLANKs the game already prepares the next, its textures and palettes
  included (DDR's arrows flash by palette), and the frame on show must be drawn again with its own.

An in-between picture waits for the drawing thread, undoes the uploads made since the frame on
show (newest first), moves what moved whole on along its last move, keeps the rows the frame
draws on, draws the frame again on the emulation thread, lets MAME's screen update compose it,
and puts the rows and the uploads back. Matching, with the frame before's (first vertex within 16
pixels): a rectangle or sprite with the same command and colour and at most one other word of its
packet different (a size: a note clipped shorter going off the lane; a texture: an animated one);
a polygon or line with the same command and texture (a shaded one's colours change with its
light), the one whose every vertex moved least. Only what moved whole is carried on (every vertex
by the same step) and, of polygons, only rectangles on the screen's axes (sprites drawn as quads:
DDR's arrows) and level or upright lines (bar lines): a 3D model's polygons (DDR's dancer), each
moving its own way, came apart at their seams when carried on, so they stay as the game draws
them, at its own rate. On Twinkle the DVD/Video CD player's picture under it is taken at the
in-between picture's own moment, so the background video moves on too.

Checked:
- beatmania IIDX 3rd style, the play case's song: each frame clears the back buffer (640x240),
  then about 132 textured rectangles (the notes, 3 lines tall, moving 1 line a frame at that
  speed; the bar lines, semi-transparent; the panels), flipped at VBLANK. Halfway, only the notes
  and bar lines differ. A note moving 1 line a frame has no in-between position at the game's
  resolution (240 lines): in-between pictures help the higher speeds (hi-speed) most.
- DDR 2ndMIX, the play case's song (320x240, 30 frames a second, no clear: about 470 textured and
  shaded polygons a frame, the arrows semi-transparent quads moving 2 lines a frame): the arrows
  and panels move on, the dancer stays whole.
- GuitarFreaks 2ndMIX, the play case's stage: the bar lines move on.

### Other games and boards

Any game on these four cores gets in-between pictures with no per-game work: pop'n music 1-8,
Animelo 1/2, Mickey Tunes, beatmania (DJ Main), beatmania III, beatmania IIDX (1st-8th style), and
the System 573's DDR, DrumMania, GuitarFreaks, Dance Maniax and the rest. Whether they look right
depends on how each draws its notes (sprites or objects moving a steady step, which is usual).
To check a game:
1. find a stretch where notes move (attract demo or a play script);
2. dump the window's picture and an in-between picture at a few frames: on DJ Main,
   `--smooth-motion --snap-every N` writes `frameNNNNNN-half.ppm` beside each snapshot. Firebeat's
   and Twinkle's in-between pictures go with the picture at the end of vblank (after the game's
   flip), not the frame hook's, so they need a dump there. On Firebeat,
   `HK_BETWEEN_SNAP=FIRST-LAST,PREFIX` writes, for each of those frames, the picture at the end of
   vblank (as the window shows it at the game's own speed: what scrolls drawn on the scroll's
   line) and the in-between pictures a quarter, a half and three quarters on, all made then (as
   the window makes them: host/window.cpp `keep_shot`), as
   `PREFIX-<frame>-<0..3>.ppm`; on Twinkle, a screen `m_hk_vblank_end` calling
   `render_between(0.5, w, h)`;
3. compare: only what moves should differ, by half its step.

To see how a Firebeat game draws: log each GCU command with its frame number (type, source,
destination, size, scale) and the layers at each vblank. pop'n music 4 runs one display list a
frame, kicked from its vblank interrupt: fill (clear) the sprite layers, then the objects.

To see how a Twinkle game draws: log each queued item (`hk_draw`: its op and packet words) and the
display start and drawing area at each VBLANK.

What the in-between pictures cost a core, without a display: `HK_BETWEEN_BENCH=K` (runtime/main.cpp)
draws, every frame of a headless run, the K - 1 pictures a display at K times the game's rate asks
for (2: 120 Hz, 4: 240 Hz), where a window would (with the picture at the end of vertical blank on
the cores that have one), and the exit report says how many, the mean time of one, and a hash of
their pixels (two builds that draw the same in-between pictures print the same hash). beatmania
IIDX 3rd style's stage: about 0.8 ms a picture on the PC it was written on, where a frame's
emulation is about 1.5 ms; the frame's clear (640x240) kept and put back around each one was most
of the rest until it was copied a row at a time.

Python (the PS2's GS) could have in-between pictures the same way, not done. For its
clock-following games the `[[rate]]` way is better: the pictures are the game's own.

## The PC beatmania IIDX games

The PC IIDX games (9th style to EMPRESS) count everything in frames, the song's position too
(PLAN.md section 36), so a faster vsync would play them faster: they get in-between pictures, but
the game draws them itself. While smooth motion is on, HIRAKU's DLL in the game records each
frame's drawing (the states set, the textures, and each draw's vertices: these games draw everything
as quads in screen space, with no shaders) and lists its quads with the picture (link v11,
runtime/pc/link.h). hiraku-pc pairs them with the frame before's (the same texture, size and x, the
same place or lower), and for each of the frame's refreshes asks the game, still waiting for its next
frame, to draw the frame again with the notes moved along the scroll's steady line
(hiraku/scroll_guess.h); runtime/pc/d3d8.c and d3d9_smooth.c replay the recording between the frame's
starting and ending states (state blocks) into a picture of their own, about 0.7 ms each. A frame's
in-between pictures are all drawn when the window asks for the first; the game's next frame starts
when it would have (given earlier, its controls were read a frame sooner).

Only the notes move, and of the picture drawn again only the places a note was moved from and to are
taken: the movie (its texture is the decoder's, written meanwhile), the gauge, the judgements and
the effects stay as the game drew them. A quad is a note when it has fallen by the same step (within
a pixel) for three frames, with the scroll, after half a second of play (something falling steadily
all along), inside the lanes (where a bar line, wide and a pixel or two tall, has fallen in the last
5 seconds), and note-sized (12 to 160 pixels wide, up to 64 tall). Each rule came from something
else that moved: letters and score digits paired with the same ones a line up, a result screen's
count-up and its sparks, attract mode's sliding pictures, a panel's rolling text. A frame drawn some
other way (shaders, other render targets, indexed draws) isn't drawn again (pc.log says what, once).

What the recording needs from the device, Direct3D 8 (9th style to DistorteD): the game's vertex
buffers are write-only, which Windows' own Direct3D 8 won't lock to read back (Wine's does), so with
smooth motion each buffer gets a copy in the game's memory as it is made (`HK_PC_SMOOTH`, from
hiraku-pc): the game writes into the copy, its unlock passes that on to the buffer, and a draw's
vertices are read from the copy. And the games' state blocks: HAPPY SKY sets its draws' vertex
format with ApplyStateBlock, which the recording takes as a step of its own (applied again in the
replay), reading the format, texture and stream back from the device after it; without it its
panels and lanes were drawn in the wrong format, black where a note moved.

Checked under Wine in a virtual KWin, with the in-between pictures as snapshots (`--snap-between`):
RED, HAPPY SKY, DistorteD and GOLD move the notes and bar lines in the lanes, nothing else, the
notes never stepping back; with the game at 30 Hz on the 60 Hz display, each frame's second refresh
showed one and the frames' timing was as without. 9th style and HAPPY SKY again with the buffer
copies and the state blocks (2026-11-08): the notes and bar lines move in player 1's lanes, the lanes
drawn whole. On real Windows (a cabinet's GTX 1650, 120 Hz) GOLD and EMPRESS draw theirs; 9th style
and HAPPY SKY drew none there before the buffer copies. Not yet: 10th style, DJ TROOPERS.

EMPRESS can also draw its own pictures at 120 Hz, with a mod (`games/iidx16/mods/high-frame-rate`,
docs/MODS.md): it converts its charts from milliseconds to frames as a song loads, so with that
frame halved its charts are in half frames, and run at twice its rate it plays them at the same
speed, judging as on the cabinet (HIRAKU's DLL takes the lanes' distances back to whole frames).
Every picture is then the game's own, the notes read and judged twice as often; the frame-counted
rest (animations, effects, timers) is held to the cabinet's pace. Checked under Wine, then played on
a 240 Hz display. DJ TROOPERS, the same engine, has the same mod (`games/iidx15/mods/high-frame-rate`,
checked the same way). 9th style to GOLD store their charts in frames: their mods have HIRAKU read
the chart files with every time doubled (`chart_times`, docs/MODS.md); checked under Wine.

## A CRT's motion clarity (`--crt-beam`)

A third way, for the same games as the in-between pictures (DJ Main, Firebeat, Twinkle, System
573): the [Blur Busters CRT Beam Simulator](https://github.com/blurbusters/crt-beam-simulator)
(Mark Rejhon and Timothy Lottes, MIT). A sample-and-hold display (LCD, OLED) shows each frame for
the whole frame, and the eye tracking a moving note smears it; a CRT lit each line for a moment as
the beam passed, so motion stayed sharp. On a display faster than the game, each refresh lights
only the part of the tube the beam sweeps during it, and lets the rest fade: the frame's light,
gamma-corrected, spread over as few refreshes as each pixel's brightness allows (bright pixels over
more, so the picture keeps most of its brightness).

- **On:** Settings > Display > Motion > "CRT beam", or `--crt-beam` (`--crt-beam-gamma`,
  `--crt-beam-gain`). It replaces smooth motion's in-between pictures: both use the frame's other
  refreshes.
- **Needs** what smooth motion needs (display sync, a picture every refresh: Windows, or Wayland
  with fifo-v1, KWin 6.7) and a display at least twice the game's rate: 120 Hz and up, best at 240
  (four refreshes a 60 Hz frame). Below that, or without those, the game says why at exit
  ("CRT beam: ... without (...)").
- **The display:** SDR, its gamma set to match (2.2 by default, as most displays and KDE Plasma's
  SDR output; 2.4 if the display is set so: a mismatch shows as bands, washed-out colour, or a slow
  wave of brightness), its ABL/APL and its own strobing or black frame insertion off. HDR modes and
  local dimming fight the per-refresh brightness.
- **The same refreshes every frame where possible:** where display sync would lock the game to the
  display (within 3 %: 59.94 Hz games at 120 or 240 Hz run 0.1 % faster, pop'n music's 58.49 Hz
  2.6 %, as display sync does without smooth motion), every frame gets the same refreshes. At the
  game's exact speed the light's split over the refreshes drifts (4.10 refreshes a frame for pop'n at
  240 Hz), and the display's own response, its curve and its slow transitions near black, turns
  that into a slow, wavy change of brightness in the darker parts (owner, 2026-10-20: "a slow
  strobe", then "only in lower brightness zones" at pop'n's exact speed; simulated, 2-4 % even on a
  display that matches the gamma exactly, from a 6 Hz beat; dithering and a sharp band edge made it
  no better). DJ Main's 58 Hz (3.4 % off 60) keeps its exact speed, and the drift with it.
- **Brightness** (`gain`, 0.7): lower is sharper and darker; about 0.5 at 120 Hz.
- **Any picture mode:** the mode (sharp, light, full, analog) draws each frame once into a picture
  at the window's size; the beam's pass, every refresh, reads the frame's picture and the one
  before it (three texture reads a pixel, about 0.01 ms).

How HIRAKU runs it (runtime/host/beam_shader.h, picture.cpp, window.cpp):

- The display's response is a power curve (`Gamma`). The original writes the sRGB formula with its
  exponent replaced: sRGB itself at 2.4, its default, but at 2.2 a curve no display has, 5-10 %
  off a 2.2 display, which with the drift above made the slow wave.
- The beam's place comes from time, not a counter: display pacing at the game's own speed
  (`pacing::phase(i)`, the same moments as the in-between pictures) gives each refresh's moment in
  the frame, and `pacing::refreshes_in_frame()` the display's refreshes per frame (2.4 for a 60 Hz
  game at 144 Hz, 4.13 for DJ Main's 58 Hz at 240). The original's frame counter and its LCD
  "anti-retention" slew aren't needed: the ratio is rarely a whole number, so the pattern drifts by
  itself.
- No latency added: the frame the beam draws is the game's latest (the original's demo draws the
  one before it). The next frame isn't there yet when a refresh crosses into it, so that refresh
  draws this frame's top again; the light is the same.
- Keyboardmania's monitors are turned on their side (`Core::monitor_turned`): the beam sweeps left
  to right in each.
- Not with the monitors in windows of their own (Keyboardmania split), nor on the Python core
  (pop'n music 9-14 draw through PCSX2's own window).
- Checked (tests/picture): every refresh against the formula, and the light: over a run of frames,
  what the refreshes show adds up, in linear terms, to the frames' light times the gain (at 4, 2.4
  and 4.004 refreshes a frame). Not yet seen on a fast display.

## More vsyncs (`[[rate]]` mods)

For a game that follows a clock, the vsync itself can come faster: the game draws more pictures
a second, at their true moments. This needs a mod per game (docs/MODS.md: the format, and the
addresses for pop'n music 9-14), because what the game counts in frames (animations, timers,
effects) would run fast with it. Finding what a game needs, as done for pop'n music 9-14:

1. **The clock.** Find the word the play screen recomputes from a hardware timer each frame. On
   pop'n music 10-14 it's a function with a pause flag at +0x20, the timer read, a 64-bit
   subtraction, the result stored at +0 of the same block: a search for that shape in the code
   finds it in each version. Otherwise search RAM for a word that moves at a steady rate during a
   song only. `while_changing` ties the rate to it.
2. **The phase.** The rate must stop when the song ends (results and menus count frames). Find
   the play screen's state (a halfword it switches on, 4 during the song in pop'n music) and give
   it as `while_equal`.
3. **Check the notes.** At 120 and 240 Hz, in the attract demo or a play script stretched to the
   new rate, notes must be where they are at 60 Hz at the same moment of the song, and a play
   case's judgements the same.
4. **Frame counters.** Everything that still runs fast needs its increment patched to +0 on the
   vsyncs between the cabinet's ticks (`between_ticks`). Ways to find them:
   - the objects' update: one function adds each animated object's speed to its frame counter;
     reading the speed as 0 between ticks (pop'n: the `lh rt, 0x20(rs)` before `addiu ..., 100`)
     holds every object at once;
   - try batches of candidate increments (`addiu rX, rX, 1` near a store) as `while_rate` +0
     patches mid-run, and watch the counter (the Python core: `HK_PY_WATCH`); watch the clock too,
     so that a patch that stalls the game isn't taken for a hit;
   - a write watchpoint on the counter (PCSX2's memory checks set to log without pausing, a
     change kept out of the tree): finds counters updated through pointers, which the search
     misses. RAM dumps around the event (`HK_PY_EE_DUMP`) give the candidates;
   - a stack slot that changes isn't necessarily a counter: pop'n 14's "judgement timer" at
     0x01ffea20 was a draw call's spilled argument.
5. **Sound and pacing.** Display pacing follows the rate switch at once and the sound queue is cut
   back after it (runtime/host/pacing.cpp, window.cpp): nothing per game.

The Firebeat and DJ Main cores can change their vsync rate too (`Core::set_vsync_rate` on DJ
Main; Firebeat's frame period is `frame_ticks()`), but their pop'n music count frames, so it would
only make them faster: the in-between pictures are the way there.
