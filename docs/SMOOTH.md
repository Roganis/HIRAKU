# Smooth notes on fast displays

A 120 or 240 Hz display can show notes moving two or four times as smoothly as the cabinet's
60 Hz monitor. A game draws one picture per vsync, though, so the extra smoothness has to come
from somewhere. HIRAKU has two ways, and which one fits depends on how the game keeps time (and a
third, the CRT beam, for a CRT's sharpness in motion rather than more pictures):

| The game's play goes by | Way | Where | Games so far |
|---|---|---|---|
| a clock (a hardware timer read each frame) | more vsyncs: the game draws more pictures (`[[rate]]` mods) | per game: docs/MODS.md | pop'n music 9-14 (Python) |
| frames (each frame moves the notes a fixed step) | in-between pictures: HIRAKU draws them (`--smooth-motion`) | per core: every game on it | DJ Main, Firebeat, Twinkle, System 573 |
| frames, on a Windows PC | in-between pictures: the game's own Direct3D draws them again, the notes moved (`--smooth-motion`) | the PC core (runtime/pc/) | beatmania IIDX RED to GOLD (9th style to EMPRESS) |

To tell which one a game is: run its attract demo (or a play script) with the vsync at twice the
rate. If the demo song takes the same number of frames (it plays twice as fast, out of step with
its music), the game counts frames. If it takes twice the frames, it follows a clock. pop'n music
4 (Firebeat) and pop'n music 1 (DJ Main) count frames; pop'n music 9-14 follow a clock. The
beatmania games were checked as they draw (below): beatmania (DJ Main), beatmania III (Firebeat)
and beatmania IIDX (Twinkle) all move their notes a fixed step a frame.

## In-between pictures (`--smooth-motion`)

The game keeps its rate and runs exactly as before, at its own speed; nothing in the machine
changes. Display pacing keeps the game on the steady clock: each frame has a moment, one frame
period after the last, and gets the display's refreshes from its moment to the next frame's (at
144 Hz, a 58 Hz game's frames get 2 or 3; at 240 Hz 4 or 5; at 60 Hz 1, and every 30th frame 2).
Every refresh shows the frame with what moves carried on to that refresh's moment, a fraction of
a frame along its last move (the first refresh too: it comes after the frame's moment, up to a
refresh):

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
  as on Wayland. Where the driver can't share OpenGL's pictures (Wine), OpenGL's own swap through
  DWM, the lead half a refresh. Elsewhere (X11, or Wayland without fifo-v1), or with display sync
  off, nothing changes;
- the exit report says how many it showed (`video: smooth motion: N in-between pictures shown`).

Use it: `--smooth-motion` (display sync on, the default), or the launcher's Settings > Display > "Smooth
motion (DJ Main and Firebeat games)". Any refresh rate works, 144 Hz too, and the game keeps its
exact speed and pitch (without smooth motion, display sync speeds pop'n 4-8 up 2.6 % to fit
60/120/240 Hz, and leaves DJ Main's 58.00 Hz, 3.4 % off, juddering). The faster the display, the
smoother; at 60 Hz it mostly evens out the frame that would be held twice every 30 frames.
`HK_PACING_DEBUG` prints each frame's refreshes and their moments.

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
   flip), not the frame hook's, so they need a dump there (a few lines in `vblank_end_fire`,
   runtime/firebeat/board.cpp, or a screen `m_hk_vblank_end` on Twinkle, calling
   `render_between(0.5, w, h)`);
3. compare: only what moves should differ, by half its step.

To see how a Firebeat game draws: log each GCU command with its frame number (type, source,
destination, size, scale) and the layers at each vblank. pop'n music 4 runs one display list a
frame, kicked from its vblank interrupt: fill (clear) the sprite layers, then the objects.

To see how a Twinkle game draws: log each queued item (`hk_draw`: its op and packet words) and the
display start and drawing area at each VBLANK.

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

Checked under Wine in a virtual KWin, with the in-between pictures as snapshots (`--snap-between`):
RED, HAPPY SKY, DistorteD and GOLD move the notes and bar lines in the lanes, nothing else, the
notes never stepping back; with the game at 30 Hz on the 60 Hz display, each frame's second refresh
showed one and the frames' timing was as without. Not yet: 9th style, 10th style, DJ TROOPERS and
EMPRESS; a real 120/144/240 Hz display; real Windows (DWM's pacing).

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
