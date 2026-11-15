# Smooth notes on fast displays

A 120 or 240 Hz display can show the notes moving two or four times as smoothly as the cabinet's
60 Hz monitor. The games draw one picture a frame, so HIRAKU has three ways to use the display's
extra refreshes. Which one a game gets depends on how it keeps time. How each works, and how it
was checked: [SMOOTH.md](SMOOTH.md).

| Way | What you see | Games | Where to turn it on |
|---|---|---|---|
| **Smooth motion** | HIRAKU draws a picture for every refresh, the notes carried on to that moment | DJ Main, Firebeat, Twinkle and System 573 games; the PC beatmania IIDX games (9th style to EMPRESS) | **Settings > Display > Motion** |
| **High frame rate mods** | the game itself draws more pictures, each at its true moment | pop'n music 9–18; beatmania IIDX 15 DJ TROOPERS and 16 EMPRESS | right-click the game > **Mods…** |
| **CRT beam** | a CRT's sharpness in motion, not more pictures | the same games as smooth motion | **Settings > Display > Motion** |

All three need display sync (on by default), and a picture every refresh: Windows, or Wayland with
KWin 6.7. On X11 they do nothing. None of them adds latency or changes the judgement: the game
sees your presses as before.

## Smooth motion

The game keeps its own speed and pitch, on any display: 60, 120, 144 or 240 Hz. The faster the
display, the smoother. At 60 Hz it mostly evens out the frame that would otherwise be held twice
now and then.

- It's a guess one frame ahead. The notes scroll steadily, so they follow a steady line and never
  step back. Something that stops, turns or appears can be carried on up to one refresh too far
  before the next frame puts it right.
- A game's resolution still sets its steps. beatmania IIDX on Twinkle draws 240 lines, and at
  normal speed its notes move one line a frame: there's nothing in between to show. The faster the
  notes (hi-speed), the more smooth motion adds.
- **The PC beatmania IIDX games** draw the in-between pictures themselves, through HIRAKU: only
  the notes and bar lines move, the movie, gauge and effects stay as the game drew them. Checked
  on real Windows with GOLD and EMPRESS; under Wine with RED, HAPPY SKY, DistorteD and 9th style.
  Not yet: 10th style, DJ TROOPERS.
- **Not covered:** Keyboardmania (two monitors), and the Konami Python games (pop'n music 9–14:
  they have high frame rate mods instead).

Details: [SMOOTH.md](SMOOTH.md#in-between-pictures---smooth-motion),
[the PC games](SMOOTH.md#the-pc-beatmania-iidx-games)

## High frame rate mods

For a game that times its notes by a clock, the display's rate can be the game's: it draws every
picture itself, at its true moment, and judges as on the cabinet. What the game counts in frames
(animations, effects, timers) is held to the cabinet's pace. Each game needs its own mod, which
HIRAKU comes with:

- **pop'n music 9–14** and **15–18** (`smooth-notes`): while a song plays, the game runs at the
  display's refresh rate (120, 144, 240 Hz...); menus and results stay at 60.
- **beatmania IIDX 15 DJ TROOPERS and 16 EMPRESS** (`high-frame-rate`): the charts are counted in
  half frames and the game runs at exactly twice its rate, so it needs a display at 120 or 240 Hz.
  Checked on a 240 Hz display.

Turn one on: right-click the game > **Mods…**. A game with mods keeps its saves apart from the
clean game's. Details: [MODS.md](MODS.md#high-frame-rates-rate).

## CRT beam

The [Blur Busters CRT Beam Simulator](https://github.com/blurbusters/crt-beam-simulator). An LCD
or OLED shows each frame for the whole frame, and your eye tracking a moving note smears it; a CRT
lit each line for a moment as the beam passed, so motion stayed sharp. The CRT beam lights, at each
refresh, only the part of the screen the beam sweeps then and lets the rest fade.

- **Settings > Display > Motion > CRT beam.** It replaces smooth motion: both use the frame's
  other refreshes. It works with any picture mode (F10).
- **A display at 120 Hz or more**, best at 240 Hz. Below that the game says why at exit.
- **Set the display up for it:** SDR (not HDR), **Display gamma** set to your display's (2.2 by
  default; 2.4 if the display is set so: a mismatch shows as bands, washed-out colour or a slow wave
  of brightness), and the display's ABL/APL, local dimming and its own strobing or black frame
  insertion off.
- **Brightness:** lower is sharper and darker: 0.7 at 240 Hz, about 0.5 at 120 Hz.
- Games running close to the display's rate (most of them, at 120 or 240 Hz) are locked to it, so
  every frame gets the same refreshes. DJ Main's 58 Hz keeps its own speed, and its darker parts
  may show a slow change of brightness.
- **Not covered:** Keyboardmania with its monitors in windows of their own, and the Konami Python
  games.

Details: [SMOOTH.md](SMOOTH.md#a-crts-motion-clarity---crt-beam)
