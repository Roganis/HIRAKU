# Playing with HIRAKU

How to add games and play them, the settings, and a section per board and game: what to add, the
first start, the controls. Each section links to its full notes in [PLAY-NOTES.md](PLAY-NOTES.md).

## Adding a game

Drop the game's files on the launcher's window, or use **File > Add game files** (or **Add a game
folder**). Give them all at once, in any order:

- the MAME ROM set (`.zip`, `.7z`, or a folder of its files);
- the game's CHDs (they are unpacked with MAME's chdman: install MAME, or MAME's flatpak);
- disc and disk images (`.cue`/`.bin`, `.iso`, `.img`).

HIRAKU recognises the game. Several games can go at once, a whole MAME `roms` folder too. CHDs
given without their MAME set are kept, and the error names the set to add.

- **Move or leave:** the launcher asks whether to move the files into the HIRAKU folder or leave
  them where they are (**Settings > General > Games' files when added** answers for good). Files
  left where they are must stay there to play.
- **The files, checked:** the game's details list every file it needs. A MAME game's ROMs and CHDs
  are checked against MAME's: ✓ the dump MAME knows, ✗ another one, – missing.
- **Room on the disk** once unpacked: beatmania III's hard disk 3.4 to 18 GB, a pop'n music DVD
  4.6 GB, a CD under 700 MB.

From a terminal: `tools/ingest.py FILE...` or `hiraku --add FILE...`.

Details: [PLAY-NOTES.md](PLAY-NOTES.md#adding-a-game)

## Updating

A release package's launcher looks for a new release when it starts (Options > Settings > General
turns that off). **Update and restart** puts it in and starts the launcher again, without touching
`dumps/`, `user/` or `out/`. If an update goes wrong, unpack the release's package over HIRAKU's
folder: your games, saves and settings stay. A checkout doesn't offer updates: use git.

Details: [PLAY-NOTES.md](PLAY-NOTES.md#updating-a-release-package)

## Building games from source

A release package needs none of this. In a checkout, each game has a program of its own. A
Firebeat game's is recompiled from its CD once the game is added:

```bash
tools/ingest.py /path/to/cd.cue --romset /path/to/bm3final.zip --hdd /path/to/hdd.img
analyze/ghidra.sh bm3final && analyze/prep.py bm3final && ninja -C build/cmake hiraku-bm3final
```

DJ Main, System 573 and Twinkle games need `ninja -C build/cmake hiraku-GAME`; the Konami Python
games have their own build (their section). Optional: `tools/pgo.py` makes some games need less
CPU; run it again after updating HIRAKU.

Details: [PLAY-NOTES.md](PLAY-NOTES.md#from-source-beatmania-iii-the-final-as-its-own-binary)

## Playing

In the launcher, pick the game and press **Play**.

- **View > Show games not added yet** lists every game HIRAKU runs, and what each one needs.
- **Controls** and **Lights** map your keyboard, controllers and controller LEDs. **Settings** has
  a section each, down its side, for Display, Audio, e-AMUSEMENT, Cabinet I/O, General and Look.
- On a first boot, the game's test-mode steps are shown under the list.
- The **Game** menu (or right-click) has the game's own: Mods, Unlocks, Picture, its save folder,
  **Arguments…** (command-line options for that game) and more.

The machine boots like a cabinet. **Fast start-up** (Settings > General, on by default) runs the
game's start-up as fast as the PC can, without sound, then the game goes on at its own speed; a
coin, Start or Test brings back its own speed at once. Not on the PC games.

The keys, with beatmania III as the example (each game's own are in its section):

| Key | Action |
|---|---|
| Z S X D C | P1 keys 1–5 |
| Left / Right (or Left Shift / A) | P1 turntable |
| Space | P1 foot pedal |
| 5 | coin (the game wants 2 for one player) |
| 1 | P1 start |
| F2 | test mode, 9 service |
| Insert | the floppy disk: take it out, put it in |
| Tab (hold) | fast-forward |
| F11, Alt+Enter | fullscreen |
| F10 | picture: off, CRT light, CRT full, CRT analog |
| F9 | the game's lights over the picture ([LIGHTS.md](LIGHTS.md)) |
| F8 | the CRT shader's settings, changed while you play |
| F7 | how the game runs on this PC: one line, a panel, everything, off |
| F6 | record the game to a video file; again to stop ([below](#recording-video-f6)) |
| F5 | save a screenshot ([below](#screenshots-f5)) |
| Ctrl+1 … Ctrl+9 | type one of the game's codes |
| Esc | quit |

P2 uses M K Comma L Period, Up/Down and 2. Turn the turntable to choose and press a white key (1,
3 or 5) to confirm; Start toggles DOUBLE on the option panel.

**Unlocks:** right-click the game > **Unlocks**. On beatmania III The Final it applies the COLOR
CHECK code (Miracle Moon, CLOUDY MUSIC, RETROFUTURE, The Least 100 sec, COREDESAT, IMPLANTATION's
ANOTHER chart); the title screen then shows the alien icon.

**Codes:** in the games that have them, Ctrl+1 to Ctrl+9 type the cabinet's button codes (hidden
songs, modes) for you; **Unlocks** lists each code and where to type it.

**Floppy disk (beatmania III):** a player's data lives on a floppy disk. Right-click the game >
**Floppy disk** gives it one (the game's own `floppy.img`, or a 1.44 MB image of yours; all five
beatmania III can share it). Have it in before pressing Start; **Insert** takes it out and puts it
in, but not while FD LOADING or FD WRITING shows.

**Memory cards (System 573: DDR 2ndMIX Link Ver. to EXTREME, GuitarFreaks 2ndMIX Link Kit to
4thMIX):** the cabinet's reader takes PlayStation memory cards, with the home version's link and
edit data; DDR 5thMIX, DDRMAX and DDRMAX2 keep a player's best scores in its link file. Right-click the game > **Memory cards** gives each of its two slots one (the game's
own `memcard1.mcr` / `memcard2.mcr`, or a raw 128 KiB card image of yours, which the game reads
and writes in place); from a terminal, `--memcard1 FILE`, `--memcard2 FILE`. **Insert** and
**Delete** take card 1 or 2 out and put it in (GuitarFreaks: **Home** and **End**).

**An e-AMUSEMENT card as the disk or memory card:** with a card reader and no floppies or memory
cards, tick **Each e-AMUSEMENT card its own floppy disk** (or **memory card**) in that menu. A
card touched to the reader then puts its own disk or memory card in (touched again: out; another
card: swapped), kept for that card in `card-media/` beside the saves, for every game. A new card
gets a blank one: fine for beatmania III, but DDR only uses a card with the home version's link
file on it: put that image to the card with **A card's memory card** in the same menu (after the
card has been touched once). Without a reader, **Insert** / **Delete** present the card numbers of
Settings' e-AMUSEMENT page.

**Saves** are in `user/data/<game>/` in a release package (`~/.local/share/hiraku/<game>/` in a
checkout), written when you quit and, on Firebeat, System 573 and Twinkle, every 15 seconds when
they changed.

**A portable install:** a HIRAKU folder that holds a folder named `user` keeps the saves in
`user/data/` and the settings, controls, lights and picture settings in `user/config/` (instead of
`~/.local/share/hiraku/` and `~/.config/hiraku/`). Copy the folder to another PC or a USB stick
and everything comes along. To bring an existing setup, copy those two folders' contents in. Where
these docs give the usual places, read them as `user/data` and `user/config`.

Folders whose names use any alphabet work, for the PC games too: HIRAKU gives them a name they can
read, or says which folder to move.

Details: [PLAY-NOTES.md](PLAY-NOTES.md#play-beatmania-iii-the-final-as-the-example)

## The picture

**Picture** in the toolbar picks how the game looks; **F10** goes through the modes while you play:

| Mode | What it is | Cost |
|---|---|---|
| **Off** | sharp pixels (the default) | none |
| **Light** | guest(r)'s crt-guest-advanced-fast: scanlines, a phosphor mask, glow | small |
| **Full** | TroggleMonkey's crt-royale: beam shapes, phosphor masks, bloom, curvature | moderate |
| **Analog** | Zoki's Lola Retro: the whole analog chain, from the cable to the glass | the heaviest |

- **F8** shows the shader's settings beside the picture. Up and Down pick a setting, Left and
  Right change it, Delete puts back the default. **Enter** saves for all games, **Shift+Enter** for
  this game. While it shows, the game doesn't get its controls. **Settings > Display** maps these
  keys to a controller's buttons.
- **Analog's looks:** presets under **Look** in Analog's tab (consumer TVs, a Trinitron, a studio
  monitor, an Amiga 1084S, PC monitors...). Each game starts with its cabinet monitor's look.
- **For one game:** select it first, or right-click it > **Picture for…**; **↺** puts a slider back.
  The sliders change a running game at once.
- **The shape:** 4:3, as on the cabinet's monitor; beatmania IIDX (Twinkle) 16:9.
- **Firebeat's 480 lines** are drawn progressive. For an interlaced TV's look, set guest's
  **Interlace Mode** to 1, royale's **Interlacing - Toggle** on, or Lola's **Console timing** to 0.
- The shaders need OpenGL 3.3. The settings are in `~/.config/hiraku/picture/`.

Details: [PLAY-NOTES.md](PLAY-NOTES.md#the-picture)

## Controllers

The launcher's **Controls** dialog maps controllers: click a binding, then press the button or spin
the turntable. By default the first controller's buttons 0–4 are the keys (0–6 on IIDX) and axis 0
the turntable.

- Controllers are bound by name, whatever order they are plugged in. A warning sign marks an
  input bound twice.
- *Bind all in turn* asks for each control in order. Layouts can be saved and used as player 1
  or 2.
- **Turntables:** the binding's menu sets the speed (×0.5 to ×16) and the direction. On buttons
  (a PS2 controller's), lower **Speed (buttons)** if a short turn goes past several songs. At ×1
  a step of the controller's is a step of the game's; a turntable on an axis bound in HIRAKU
  0.11.1 or earlier now turns a quarter as far, and ×4 gives the old feel of ×1 (×8 of ×2, ×12
  of ×3).
- **Analog controls** (Keyboardmania's wheels, GuitarFreaks' effect knobs, the IIDX effector
  sliders) take an axis.
- A PS2 controller on a USB adapter may report buttons or an axis held from the start: HIRAKU
  takes them as resting until they are let go once.
- **Try in a game** starts one of the cabinet's games; press Test there for its I/O check.

The beatmania cabinets (DJ Main, IIDX, III) share one set of controls, and so do the pop'n music
ones. **Separate controls for beatmania (DJ Main), beatmania IIDX and beatmania III** (under the
cabinet's name, with one of them chosen) gives each of the three its own, started as a copy of the
shared ones; untick it to share again. Lamps on controller LEDs: [LIGHTS.md](LIGHTS.md).

### Card readers

A real e-AMUSEMENT card reader can stand in for the cabinet's, with its 10-key: in pop'n music
9–14, GuitarFreaks and DrumMania with their card units, and the PC games; in beatmania III and
the memory card games a card is the player's disk or memory card (above). Set it in **Settings >
e-AMUSEMENT > Card readers**; the first reader of a kind is player 1's, the second player 2's.

- **Konami readers** (ICCA, ICCB, ICCC) on a USB-serial adapter: pick its `/dev/serial/by-id/...`
  name. On Linux your user must be in the `uucp` group (Arch, CachyOS) or `dialout`.
- **NFC readers** (ACR122U...) through PC/SC: install `pcsc-lite` and `ccid`, run
  `systemctl enable --now pcscd.socket`, and blacklist the kernel's `pn533_usb` if it takes the
  reader.
- **cardio readers** (USB HID): on Linux a udev rule lets you open them (the notes have it); on
  Windows there's nothing to set up.

A card the game holds has to be taken back (Insert, the card button) before another goes in.

### Cabinet I/O: beatmania IIDX's BIO2

**Settings > Cabinet I/O** takes a real IIDX cabinet's BIO2 board (BI2A firmware) on its USB serial
port: its controls become the game's, and the game's lamps and 16-segment ticker light the
cabinet's. For the beatmania games. BI2X firmware isn't supported yet. Not yet checked on a board.

### Cabinet I/O: pop'n music's USB I/O board

**Settings > Cabinet I/O** takes the USB I/O board of a pop'n music 15 or later cabinet, for every
pop'n music game: its buttons, TEST, SERVICE and coin, and its lamps. On Windows, give it an
`ezusb.dll` (Settings: "its ezusb.dll"), the game's own or a replacement such as
UltimatePopnController's. On Linux HIRAKU loads the board's firmware from your pop'n music 15 or
later's `ezusb.dll`, and a udev rule (`/etc/udev/rules.d/70-popn-io.rules`) lets you open it:

```
SUBSYSTEM=="usb", ATTRS{idVendor}=="04b4", ATTRS{idProduct}=="8613", TAG+="uaccess"
```

Then `sudo udevadm control --reload` and plug the board in again. Not yet checked on a board. An
UltimatePopnController also works as a plain controller. More in [POPN-IO.md](POPN-IO.md).

Details: [PLAY-NOTES.md](PLAY-NOTES.md#controllers), [readers](PLAY-NOTES.md#card-readers),
[BIO2](PLAY-NOTES.md#cabinet-io-beatmania-iidxs-bio2),
[pop'n board](PLAY-NOTES.md#cabinet-io-popn-musics-usb-io-board)

## The game menu: changing games from the cabinet

**Leaving a game:** hold **5, 7 and 3** together on an e-AMUSEMENT reader's 10-key for two
seconds. A "GAME MENU" bar fills; let go before it's full and nothing happens. The game ends with
its saves written. A cabinet with no 10-key holds **Test and Service** together, and you can add a
key of your own (**Settings > General > Game menu**).

**The menu** shows the games ready to play, a shelf per series, driven by the cabinet's controls:

| Cabinet | Choose a game | Change shelf | Play |
|---|---|---|---|
| beatmania, IIDX, beatmania III | the turntable | the black keys | a white key, or Start |
| pop'n music | the blue buttons | the green buttons | the red button |
| pop'n stage | Select left / right | the outer buttons | Select OK |
| DDR | the Up / Down panels | the Left / Right panels | Start |
| DDR Solo | Select L / R, or Up / Down | the Left / Right panels | Start |
| Dance Maniax | Select L / R | the upper sensors | Start |
| DrumMania | Select L / R, or hi-hat / cymbal | the toms | Start, or the snare |
| GuitarFreaks | the neck's R / B | the neck's G | Start, or the pick |
| Mambo a GoGo | Select left / right | the left / right conga | Start, or the centre conga |
| Keyboardmania | the wheel, or the lowest C / D | the lowest black keys | Start |
| ParaParaParadise | the Left / Right buttons | the outer sensor bars | Start |

A reader's 10-key works on any cabinet (**8**/**2** choose, **4**/**6** change shelf, **Enter**
plays), and so do a keyboard's arrows and a plain gamepad. Escape brings the launcher back.
**Settings > General > Game menu** also has "Every game", the hold time, an off switch and
**HIRAKU starts in the game menu**. The menu shows a game's title screen, kept from your own game
(not the PC games), the screen you pressed **F12** on in the game, or your own picture
(**Game > Picture in the game menu**). The launcher's
**Cards** view shows the same pictures.

It needs the launcher. In full screen the screen stays black from one to the next (a game to the
menu, the menu to a game): no desktop, taskbar or launcher window in between. Not yet checked on a
cabinet's reader, BIO2 or pop'n I/O board.

Details: [PLAY-NOTES.md](PLAY-NOTES.md#the-game-menu-changing-games-from-the-cabinet)

## Music in the launcher

The **Music** panel (**View > Music**) plays the MIDI files in `music` in the saves' folder;
HIRAKU comes with none. They play on Keyboardmania's synth card once a Keyboardmania game is added,
else a SoundFont, else Windows' own synth (**Settings > Audio > Music in the launcher** picks).
The 16 parts' meters show what is heard: lower as the volume goes down, flat at 0, where the
speaker by the slider is struck through. A volume under 20 saved before 0.12.0 (when the slider did
nothing on Windows) is set back to the middle once, and the panel says so.

Details: [PLAY-NOTES.md](PLAY-NOTES.md#music-in-the-launcher)

## Sound levels

The machines make their sound at very different levels (a System 573 song about 8 dB louder than a
DJ Main one), so HIRAKU evens them out: a trim for each family of games brings their songs to about
the same loudness (about -18 LUFS), and **Settings > Audio > Volume** works on top of it. Only the
speakers get it: recordings (F6), `--wav` and `--digest` keep the machine's own level. **Every game
about as loud as the others** (Settings > Audio, on by default; `--even-volume off`) turns it off.

| family | trim |
|---|---|
| System 573 | -5 dB |
| Twinkle (beatmania IIDX 1st to 8th style) | -4 dB |
| PC beatmania IIDX (9th style to EMPRESS) | -4 dB |
| PC pop'n music (15 to 18, and the last 32-bit one) | -1 dB |
| Firebeat | 0 |
| Konami Python | +1 dB |
| DJ Main | +3 dB |
| the others (the 64-bit pop'n music, ee'MALL) | 0 |

A game that stands apart from its family can have its own in its `game.toml`: `[sound]` with
`level_db = -2.5` (dB, replacing the family's). The log says which applies (`audio: level evened out: ...`).

## Latency

The games read their keys once a frame, as on the cabinet. What the PC adds is set here:

- **Settings > Audio > Audio queue** (8 ms) tunes itself to this PC; **Fixed** keeps it there
  (the least delay, a click whenever a frame is late).
- **Ahead of other programs** (Settings > Audio, on by default) keeps a browser or a build from
  delaying the game. GameMode is used when installed.
- **Settings > Audio > Output, Device buffer:** on Linux, a sound card of its own (`hw:...`) is the
  lowest latency; nothing else plays on it meanwhile. On Windows, **Exclusive** takes the device
  alone, and the `ASIO:` entries play through the card's ASIO driver.
- **Sound every millisecond** (Settings > Audio, on by default) on System 573 and Twinkle.
- A sound card pulled out leaves the default output; restart the game to take it up again.

Details: [PLAY-NOTES.md](PLAY-NOTES.md#latency)

## Frame pacing

The games don't run at 60 Hz: beatmania III at 59.94 frames a second, pop'n music 4–8 at 58.4866,
Keyboardmania at 58.51.

- **On a 60, 120 or 240 Hz display** (the default) the game runs on the display's refreshes, so
  every frame stays up equally long: pop'n and Keyboardmania 2.6 % faster and higher in pitch,
  beatmania III 0.1 %. Judgement, charts and sound stay in step.
- **144 Hz** and other rates that don't fit judder: set the display to 60, 120 or 240 Hz.
- **Variable refresh** (FreeSync, G-Sync, HDMI VRR): turn off "Smooth frames on a 60, 120 or 240
  Hz display" in Settings and play fullscreen (F11).
- **Settings > Display > Motion** (needs Wayland with KWin 6.7, or Windows, checked under Wine
  only): **Smooth motion** shows the notes where they are at each refresh, on any display, **in
  songs** (the menus each frame held as without it: beatmania III; the other games until they say
  when a song plays, everywhere) or **everywhere** (`--smooth-motion songs|everywhere`); **CRT
  beam** simulates a CRT's beam on 120 Hz and up: set **Display gamma** to your display's, use
  SDR, and turn off the display's ABL/APL and strobing. See [MOTION.md](MOTION.md).

Details: [PLAY-NOTES.md](PLAY-NOTES.md#frame-pacing)

## The performance overlay (F7)

**F7** shows how the game runs on this PC: one line, a panel with a graph, or everything
(Settings > Display, **Performance overlay**). A red mark is a frame shown late, an amber one a
break in the sound. Drag it, or **Shift+F7** moves it. On Windows, **Settings > Audio > Latency**
can switch to the High performance power plan while a game runs.

Details: [PLAY-NOTES.md](PLAY-NOTES.md#the-performance-overlay-f7)

## Recording video (F6)

**F6** records the game to a video file, and stops it: a red **REC** in the corner while it
records (not in the video). The video is the game's own picture at its own size and frame rate,
before the picture mode, scaling or smooth motion, with its sound, and timed by the game's clock:
if the PC is slow for a moment, the video doesn't slow down. Recordings go to `recordings/` in
the saves' folder (`user/data/recordings/` in a release package), named by the game and the time.

Every game records: DJ Main, Firebeat, System 573, Twinkle, the Konami Python games (pop'n music
9–14 at 512x448, as their TV picture: the file says its shape) and the PC games (as HIRAKU shows
them). A picture size that changes and stays (a game's boot screens, a new video mode) goes on
in a second file (`...-2.mkv`), from the moment it changed.

There are two kinds of recording (Settings > Display > Recording):

- **Exact, lossless** (the default): FFV1 video with FLAC sound in Matroska (`.mkv`), every pixel
  and sound as the game made them, so they are big: under 1 MB a second for a still screen, 2–5 MB
  a second while a beatmania or pop'n song plays (beatmania's attract: about 115 MB a minute), and
  up to about 20 MB a second for a PC game's detailed pictures (pop'n music 15's title: 18 MB a
  second, 65 GB an hour). VLC, mpv, MPC-HC, Shotcut, Kdenlive and FFmpeg read them; Windows' own
  player, phones and browsers don't.
- **For sharing, H.264** (`.mp4`, AAC sound): plays everywhere, about a sixth of the size or less
  (beatmania's attract: 19 MB a minute). H.264 keeps the colours at half the resolution, which
  smears pixel art at the game's own size, so the picture can first be enlarged by whole pixels.
  **Automatic** (the default) does it when a graphics card encodes, as many times as fit in
  1920x1440 (beatmania 3x, 1536x1152; pop'n 640x480 3x), and keeps the game's own size when the
  processor does (several times the work there; most games are 480 lines or fewer). 2x, 3x, 4x
  or the game's own size, chosen in Settings, is used whichever encodes. The pixels' shape stays
  as the game's.

The encoder for H.264 (**Encoder** in Settings): **Automatic** takes the graphics card's own
encoder when one works on the PC (NVIDIA's NVENC, AMD's AMF on Windows, Vulkan video with AMD
and Intel cards, VAAPI too when HIRAKU is built from source on Linux, Windows' Media Foundation
with any card), and x264 on the processor otherwise. A card's encoder costs the game nearly
nothing; x264 takes some processor time (beatmania at its own size: about 2 ms a frame, 5 MB a
minute; enlarged 3x: about 12 ms, spread over several cores). **Graphics card** and
**Processor** choose. The run's log says which encoder was taken and why the others weren't.

**File > Convert recordings for sharing** makes an exact recording into an H.264 `.mp4` beside it
afterwards, with the same encoder and size settings; a recording in parts (`...-2.mkv`) becomes as
many `.mp4` files. A recording cut short (the PC lost power, the game crashed) or damaged is
converted as far as it can be read: the box says "Converted up to the damage", and the log how
many frames and seconds were kept. **File > Open the recordings folder** opens the folder.

From the command line (**Arguments…**): `--record` records from the start (`--record FILE`
to a file or folder of yours), `--record-folder DIR` sets F6's folder, `--record-format h264`
records for sharing, `--record-encoder auto|gpu|x264` and `--record-scale auto|1|2|3...` as in
Settings, and `hiraku-menu --convert-recording IN.mkv [OUT.mp4]` converts one (any HIRAKU program
does it). A run with no window (or the fast start-up) always takes x264, so its file is the same
every time.

## Screenshots (F5)

**F5** saves a screenshot: the game's own picture, as the recording has it, without the picture
mode, the lights or the performance overlay, as a PNG in `screenshots/` in the saves' folder
(`user/data/screenshots/` in a release package, **File > Open the screenshots folder**), named by
the game and the time (`bm1stmix-20261121-201502.png`; `-2`, `-3` for more in the same second).
It has the shape of the game's screen: where the game's pixels aren't square, the picture is made
taller or wider (never smaller), so a beatmania IIDX screen of 640x240 on its 16:9 tube is saved
as 640x360 and pop'n music 9–14's 512x448 as 576x448. Games with square pixels (beatmania's
512x384) are saved as they are. **F12** still makes the screen the game's picture in the game
menu. `--screenshot-folder DIR` sets the folder.

## The last runs' logs

**Help > Open the last runs' logs** opens the last 10 runs' logs. **Help > Report a problem…**
makes one file of a run's log and your system, with your folders, names and card numbers taken
out, for a GitHub issue or a mail. HIRAKU sends nothing by itself.

Details: [PLAY-NOTES.md](PLAY-NOTES.md#the-last-runs-logs)

## beatmania III, APPEND CORE REMIX, 6th MIX and 7th MIX

They run like The Final. Each needs its MAME ROM set and two CHDs, the program CD and the hard disk:

| Game | Set | CHDs | Startup code |
|---|---|---|---|
| beatmania III | `bm3` | `gc97201`, `gc97202` | GQ972-JA-B |
| APPEND CORE REMIX | `bm3core` | `a05jca01`, `a05jca02` | GCA05-JC-A |
| APPEND 6th MIX | `bm36th` | `a21jca01`, `a21jca02` | GCA21-JC-A |
| APPEND 7th MIX | `bm37th` | `gcb07jca01`, `gcb07jca02` | GCB07-JC-A |

MAME marks 6th and 7th MIX's dongles BAD_DUMP, but the games accept them. 7th MIX's disk takes
18 GB and can be on another drive.

Details: [PLAY-NOTES.md](PLAY-NOTES.md#beatmania-iii-append-core-remix-6th-mix-and-7th-mix)

## pop'n music 4–8, Animelo 1/2 and Mickey Tunes!

Each game needs its program CD (Disc 1), its data DVD (Disc 2) and its MAME ROM set; Animelo 1
and 2 too. Mickey Tunes! (`popnmt2`) needs `976jba01` and `976jaa02`.

- pop'n 4, 5, 7 and 8 boot to attract with sound; pop'n 8 has played music select and full songs.
  Animelo and Mickey Tunes! boot.
- **pop'n 6's first boot** installs to flash, about 14 minutes of game time (hold **Tab**).

| Key | Action |
|---|---|
| Z S X D C F V G B | buttons 1–9 (odd = lower row) |
| 5 | coin |
| F2 / 9 | test / service |

**Unlocks** (right-click the game > **Unlocks**, with the game not running; the old save is kept):
- **pop'n 8:** pick the event phase and **Apply** (start the game once first). **MAX** unlocks
  everything. Phase **1** or 2 runs the Sugoroku de 8 event, at your own pace.
- **pop'n 7:** levels 1 (the BINGO de 7!! event), 2 (secret songs), 3 (bingo songs) and MAX.
- **pop'n 6:** the operator's five switches, as checkboxes.
- **pop'n 4 and 5:** Ctrl+1 to Ctrl+8 on the title screen with credits in, then START, before the
  title goes back to the KONAMI logo.
- **Animelo 2:** Ctrl+1 (DOUBLE EXCITE) or Ctrl+2 (TRIPLE EXCITE), then START and choose EXCITE.

Details: [PLAY-NOTES.md](PLAY-NOTES.md#popn-music-48-animelo-12-and-mickey-tunes)

## ParaParaParadise

Add the program CD (`a11jaa01`), the audio CD (`a11jaa02`) and the `ppp1mp` ROM set;
ParaParaParadise, v1.1 and ParaParaDancing likewise with their own. All play.

**First boot:** the blank backup RAM and sound flash are reported (E160 or E158, and E124). Press
**F2**, run **SOUND FLASH INIT**, then **ALL FACTORY SETTINGS**.

| Key | Action |
|---|---|
| Z X C V B | sensor bars 1–5 (left to right) |
| Left / Right | the two buttons (test menu: up / down) |
| 1 | start / OK |
| 5 | coin |
| F2 / 9 | test / service |

Details: [PLAY-NOTES.md](PLAY-NOTES.md#paraparaparadise)

## Keyboardmania

Keyboardmania 1st, 2nd, 3rd Mix and Keyboardheaven: the program CD, the second disc (CD audio) and
the MAME ROM set. The notes come from the synth card's ROMs in the set (in a split set,
`xt446.zip`: keep it beside the game's); without them a General MIDI SoundFont plays them.

**First boot:** press **F2** for test mode (**1 / 2** move, **1 + 2** enter):
- **BACKUP CLEAR**, then **1**;
- **SOUND TEST → SOUND FLASH INIT**, then **1**. Without it the sound effects are missing; HIRAKU
  says so in red.
- 2nd Mix: SOUND CHECK → INITIALIZE SOUND FLASH, and ALL FACTORY SETTINGS; 3rd Mix: SOUND OPTIONS
  → INITIALIZE SOUND FLASH, and ALL FACTORY SETTINGS. Answer "YES?" and "ARE YOU SURE?" with **1**
  then **1 + 2**, "PRESS 1P&2P" with **1 + 2**.

| Key | Action |
|---|---|
| Q W E R T Y U I O A S D F G H J K L Z X C V B N | player 1's 24 keys (C1–B2) |
| Keypad 7 8 9 4 5 6 1 2 3 0 . Enter | player 2, C1–B1 |
| Insert Home PageUp Delete End PageDown, Left Down Right Up, Keypad / * | player 2, C2–B2 |
| P / M (player 1), Keypad + / - (player 2) | the wheel, up / down |
| 1 / 2 | 1P / 2P start |
| 5 | coin |
| F2 / 9 | test / service |

- **Two displays:** **Settings > Display > each player's monitor in its own window**.
- **Volume:** it's about 9 dB under DDR, as in MAME; **Settings > Audio > Volume** raises it.
- **Unlocks:** 3rd Mix's PRESSURE mode and hidden songs, 2nd Mix's PRESSURE mode. Hold the wheel
  down on SELECT PLAY MODE until PRESSURE appears.

Details: [PLAY-NOTES.md](PLAY-NOTES.md#keyboardmania-1st2nd3rd-keyboardheaven)

## DJ Main: beatmania, pop'n music 1–3, pop'n stage

All 29 MAME DJ Main sets can be added (beatmania 1st mix to THE FINAL, hiphopmania, pop'n music
1–3, pop'n stage EX): the ROM set and its hard disk CHD. The operator's switches (Free Play,
difficulty, hidden modes) are in **Game > DIP switches…**. Not yet: a hands-on play test.

| Key | beatmania | pop'n music |
|---|---|---|
| Z S X D C | player 1 keys 1–5 | buttons 1–5 (Z S X D C F V G B: 1–9) |
| Left / Right, Left Shift / A | turntable −/+ | — |
| M K , L . | player 2 keys | — |
| 1 / 2 | 1P / 2P start | — |
| E | effect | — |
| 5 | coin | coin |
| F2 / 9 | test / service | test / service |

Details: [PLAY-NOTES.md](PLAY-NOTES.md#dj-main-beatmania-popn-music-1-popn-stage)

## System 573: Dance Dance Revolution 2ndMIX

Add the `ddr2m` set and the CD `895jaa02`. **First boot:** at "DO YOU WANT TO INITIALIZE
FLASH-ROM?" press Test (F2); after about 2.5 minutes, at "FLASH-ROM INITIALIZE COMPLETE", press
Test again. Later boots check the flash (about 80 s). Two coins make a credit.

| Key | Dance Dance Revolution |
|---|---|
| Arrows | player 1's panels |
| Keypad 4 2 8 6 | player 2's panels |
| 1 / 2 | 1P / 2P start |
| 5 | coin |
| F2 / 9 | test / service |

A dance pad is bound in Controls, Dance Dance Revolution page. The sound is mixed at half level,
which never clips. Not yet: a hands-on test with a pad.

Details: [PLAY-NOTES.md](PLAY-NOTES.md#system-573-dance-dance-revolution-2ndmix)

## System 573: Dance Dance Revolution 3rdMIX

Add the `ddr3mj` set and the CD `887jaa02`. Controls are as for 2ndMIX.

**The first boot needs the install cassette:** press **Play**; with no saves yet, the launcher puts
it in, and the game installs itself (about 4½ minutes; hold **Tab**). At **INFO 506**, stop it,
then **Play** again. If the install was stopped earlier, the game shows **ERROR 147**: right-click
it > **Boot once with the install cassette**, and Play.

Details: [PLAY-NOTES.md](PLAY-NOTES.md#system-573-dance-dance-revolution-3rdmix)

## System 573: DrumMania 10th Mix

Add the `drmn10m` set, the CD `d40jaa02` and the multi-session unit's CD `d40jba02` (and
`d40jca02` for e-AMUSEMENT). MAME stops at "Please call attendant"; HIRAKU plays it.

**First boots:** the first installs to flash and shows **INFO 508**: stop it. The next shows
**NOTE 605**: press **Test** (F2). Then it waits for the multi-session unit (about two and a half
minutes from power-on) and goes to attract. Two coins make a credit.

| Key | DrumMania |
|---|---|
| S / D / J / K / L | hi-hat, snare, high tom, low tom, cymbal |
| Space | bass pedal |
| Left / Right | select |
| 1 or Return | start |
| 5 | coin |
| F2 / 9 | test / service |

**e-AMUSEMENT:**
1. **Game > Install the e-AMUSEMENT network unit**, then Play (about 20 minutes of game time; hold
   Tab). At INSTALLATION COMPLETED, stop it and answer Yes.
2. In test mode > NETWORK OPTIONS > e-AMUSEMENT OPTIONS, turn it on (hold Start, Left/Right), then
   SAVE AND EXIT.
3. Insert puts a card in or takes it back; the numeric keypad is the 10-key. A new card gets a name
   and a 4-digit password.

The server runs inside HIRAKU unless **Settings > e-AMUSEMENT** names one (`hiraku-eamuse`:
[EAMUSE.md](EAMUSE.md)).

Details: [PLAY-NOTES.md](PLAY-NOTES.md#system-573-drummania-10th-mix-further-than-mame)

## System 573: GuitarFreaks 11th Mix and session play

Add the `gtfrk11m` set and the CD `d39jaa02` (and `d39jba02` for e-AMUSEMENT). **First boots:** the
first installs (about 7 minutes) and shows **INFO 508**: stop it. The next shows **NOTE 605**:
press **Test** (F2). Two coins make a credit.

| Key | Guitar 1 | Guitar 2 |
|---|---|---|
| Neck R / G / B | A / S / D | J / K / L |
| Pick | Space | Right Shift |
| Wailing | W | I |
| Start | 1 | 2 |

The effect knob is an analog axis. e-AMUSEMENT works as DrumMania's; in its test mode, **P2 R/B
changes a value**. Guitar 2's card is "P2 Card" (Delete), with a 10-key you bind yourself.

**Session play** with DrumMania 10th Mix: the launcher doesn't start it yet. From a terminal, start
DrumMania first:

```bash
hiraku-drmn10m --session-host /tmp/gfdm.sock
hiraku-gtfrk11m --session-join /tmp/gfdm.sock
```

Credits and Start on one cabinet open **WELCOME**, with 15 s for the other to join. Both windows
play the session music.

Details: [PLAY-NOTES.md](PLAY-NOTES.md#system-573-guitarfreaks-11th-mix-and-session-play-with-drummania-further-than-mame)

## System 573: Dance Maniax and Mambo a GoGo

Dance Maniax (`dmx`, `dncfrks`), 2nd Mix (`dmx2m`), J-Paradise (`dmx2majp`) and Mambo a GoGo
(`mamboagg`): the set and its CD. The cassette dumps must be MAME 0.250's or later; older ones stop
at **ERROR 121**.

**First boots:** the first installs (about two and a half minutes) and shows **INFO 508**: stop it
(2nd Mix and J-Paradise first show **NOTE 604**: press **Test**). On the next, Mambo a GoGo shows
**NOTE 605**: press **Test**. Two coins make a credit (Dance Freaks: three).

| Dance Maniax | Player 1 | Player 2 |
|---|---|---|
| Upper left / lower left | W / S | Keypad 7 / 4 |
| Lower right / upper right | L / O | Keypad 6 / 9 |
| Select left / right | Left / Right | Keypad 1 / 3 |
| Start | 1 or Return | 2 or Keypad Enter |

| Mambo a GoGo | Pad 1 (far) | Pad 2 (near left) | Pad 3 (near right) |
|---|---|---|---|
| Left conga | W | A | S |
| Centre conga | Y | G | H |
| Right conga | O | K | L |

Mambo a GoGo's Select is Left and Right, Start 1 or Return. The rental version (`mamboagga`) needs
a rental service that isn't emulated: play `mamboagg`.

Details: [PLAY-NOTES.md](PLAY-NOTES.md#system-573-dance-maniax-and-mambo-a-gogo)

## Twinkle: beatmania IIDX

Each game needs its MAME set and three CHDs: the program CD, the player's disc (Video CD or DVD)
and the hard disk.

| Game | Set | Program CD | Player's disc | Hard disk |
|---|---|---|---|---|
| 1st style | `bmiidx` | `gq863-jab01` | Video CD `gq863a04` | `863hdda01` |
| Substream | `bmiidxs` | `gc983a01` | Video CD `gc983a04` | `983hdda01` |
| 2nd style | `bmiidx2` | `gc985a01` | Video CD `gc985a04` | `985hdda01` |
| 3rd style | `bmiidx3` | `gc992-jac01` | Video CD `gc992-jaa04` | `992hdda01` |
| 4th style | `bmiidx4` | `a03jaa01` | Video CD `a03jaa02` | `a03jaa03` |
| 5th style | `bmiidx5` | `a17jaa01` | Video CD `a17jaa02` | `a17jaa03` |
| 6th style | `bmiidx6` | `b4ujab01` | DVD `b4ujaa02` | `b4ujaa03` |
| 7th style | `bmiidx7a` | `b44jaa01` | DVD `b44jaa02` | `b44jaa03` |
| 8th style | `bmiidx8` | `c44jaa01` | DVD `c44jaa02` | `c44jaa03` |

The DVD styles keep about 10 GB each; 7th style needs 40 GB free while it's unpacked.

**First boot:** it stops at **E150**, then shows **1P START = BACKUP CLEAR**: press **1P Start**
(1), on some styles after about 50 s. 1st style has no prompt: press **Test** (F2), move to
**BACKUP CLEAR** with key 1, press key 6, answer yes with key 2; key 7 leaves (yes: key 2). A 1st
style credit is 3 coins by default.

| Key | Player 1 | Player 2 |
|---|---|---|
| Keys 1–7 | Z S X D C F V | M K , L . ; / |
| Turntable | ← / → (and Left Shift / A) | ↓ / ↑ |
| Start | 1 | 2 |

Effect is E, VEFX W, coin 5, Test and Service F2 and 9. The effector's sliders have no default
binding. The background videos play, which MAME doesn't show; which clip goes with which song
still needs checking against a real cabinet.

### The club kits: beatmania IIDX × DDR 2nd Club Version

A IIDX cabinet and a DDR cabinet play the same song: Club Version 1 is `bmiidxc` (or `bmiidxca`)
with `ddr2mc`, Club Version 2 `bmiidxc2` with `ddr2mc2`. Give each side its first boot from the
launcher (the DDR side installs; the IIDX side's backup is cleared as for its style). Then, from a
terminal, start the IIDX side (it waits) and the DDR side:

```bash
hiraku-bmiidxc --session-host /tmp/club.sock
hiraku-ddr2mc --session-join /tmp/club.sock
```

Club Version 1: credits and Start on the IIDX, then on the DDR within 10 s. Club Version 2: the DDR
player answers **Yes** to "recruit a DJ from beatmania IIDX substream?", then the IIDX player
presses Start. The IIDX player chooses the song.

Details: [PLAY-NOTES.md](PLAY-NOTES.md#twinkle-beatmania-iidx),
[the club kits](PLAY-NOTES.md#the-club-kits-beatmania-iidx--ddr-2nd-club-version-further-than-mame)

## Konami Python 1: pop'n music 9–14

pop'n music 9 to 14 FEVER! run on PCSX2 Reliquary's emulator inside HIRAKU.

**Adding them:** drop the `.py1` (or the set's folder) on the launcher's window. Drop the arcade
BIOS (`bios`) and MagicGate key (`keys`) folders once, for every Python game. The CHD stays where it
is. On another PC, `tools/pack-data.py DEST popn9` copies a game's data. A game that stops at a
blue "Boot Program Error" or a SECURITY ERROR screen at start-up has a file missing or wrong:
[which one to check](PLAY-NOTES.md#a-python-game-stops-at-an-error-screen-at-start-up).

**From source:** unpack Reliquary's source (`pcsx2-reliquary-1.9.4-reliquary.zip`) to
`Python-work/pcsx2-reliquary` beside the checkout, then:

```bash
oracle/pcsx2/build-reliquary.sh all     # once
tools/build-libchdr.sh                  # once
tools/ingest.py /path/to/popn9.py1
```

**Playing:** boot takes about 50 s, as on the cabinet. If your graphics driver doesn't get on with
Vulkan, **Settings > Display > Konami Python games** has Software (and Direct3D on Windows).
`--fill-screen` stretches the picture over the 4:3 screen. pop'n 12–14 start with 5 buttons
selected; blue moves to 9.

| Key | pop'n music |
|---|---|
| Z S X D C F V G B | buttons 1–9, left to right |
| 5 | coin (two coins make a credit) |
| F2 / 9 | test / service (in test mode blue moves, red selects) |
| Insert | the card in or out |
| Numeric keypad | the 10-key (Backspace is Clear) |

**Unlocks:** right-click the game > **Unlocks**, pick the event phase (with the game not running),
and start it. **MAX** releases everything. For most events the dialog shows the progress and can
finish it at once or start it over. The events: pop'n 9 Gambler Z (MAX adds HITASURA), 10 HOSHI NI
NEGAIO, 11 POPN TOURIST and the mileage songs, 12 NINPOUCHO (10:55 to 23:05 Japan time, the shop's
hours; HIRAKU's clock starts at 12:00), 13 POP'N LAND and POP'N WORLD (EXTRA: the BEMANI EXPO
songs), 14 the SHOP EVENT's stories.

**e-AMUSEMENT:** off by default; turn it on in test mode (e-AMUSEMENT OPTIONS, under NETWORK
OPTIONS from pop'n 10).
- After Start, put a card in (Insert), enter a name and a 4-digit PIN twice. Next time the card and
  PIN bring back your player.
- The server runs inside HIRAKU. To share players and rankings between PCs, run
  `hiraku-eamuse --port 8080` on one and put `that-pc:8080` in **Settings > e-AMUSEMENT > EA
  Service URL**; give it a key before it's reachable from outside ([EAMUSE.md](EAMUSE.md)).
- spice2x's options and setups work as they are. **Pick** next to a card offers the cards HIRAKU
  has, or a new number.
- **One card for all the pop'n games:** set **Settings > e-AMUSEMENT > Player 1 Card** once. A
  newer game offers to carry your player over: answer yes, type the old PIN, press Insert again
  when asked for a card (pop'n 14 asks for a transfer number: type any, then pop'n 13's PIN). Clear
  marks then show in every game.
- pop'n 14's card is an IC card (`E004` and 12 hex digits); HIRAKU makes one if none is set.

**NET TAISEN** (pop'n 12 from phase 2, 13 and 14): with e-AMUSEMENT on and a card in, choose NET対戦
on the mode wheel. Alone, nobody comes and the credit plays as CHALLENGE. Online, point every
cabinet at one `hiraku-eamuse` and open TCP port 5740 in its firewall. From a terminal, two
cabinets join with `--lan-host SOCKET` and `--lan-join SOCKET`, each with its own `--nvram`. pop'n
13 and 14 give the INTERNET RANKING instead if `net_event-E13` / `net_event-F14` in the server's
data folder holds `1`.

Details: [PLAY-NOTES.md](PLAY-NOTES.md#konami-python-1-popn-music-914)

## BEMANI PC: pop'n music 15–18, beatmania IIDX 9th style to EMPRESS

HIRAKU runs Konami's own Windows program, under Wine on Linux, with its own window, sound,
controls and cards around it.

- **Adding:** drop the game's folder on the launcher (pop'n music: the one with `prog` and `data`).
  HIRAKU reads it where it is. The newest program is run.
- **Linux:** install Wine; the first start takes a minute. The IIDX movies need GStreamer's plugins
  base, bad and libav (32-bit ones too where Wine has a 32-bit half); without them the stages stay
  black, and the log says what is missing.
- **NVIDIA on Windows:** if the log says HIRAKU fell back to OpenGL, set "Vulkan/OpenGL present
  method" to "Prefer layered on DXGI Swapchain" for `bin\hiraku-pc.exe`.
- **First start:** BACKUP ERROR 5-1502; HIRAKU restarts the game by itself.
- **The picture and sound** go through HIRAKU, with its keys and settings.
- **Presses judged when they happened** and **Judgement offset** (Settings > Audio, Latency); **Type
  the card's PIN** (Settings > e-AMUSEMENT). Free play and the like are in the game's test mode.
- **Mods:** right-click > **Mods…** ([MODS.md](MODS.md)); a mod's own settings: **Settings…** there.
- From a terminal: `bin/hiraku-pc --game popn15 --window`.

**pop'n music 15–18:** e-AMUSEMENT on in test mode > NETWORK OPTIONS; the title says NETWORK OK.
Cards, the PIN and players work; clear marks and best scores are shared with pop'n 9–14. NET
TAISEN: point every cabinet at one `hiraku-eamuse` (lobby port 5741, relay TCP 5740); alone,
pop'n 15 plays the credit as CHALLENGE, 16–18 wait at CUSTOMIZE. A second cabinet on one PC needs its own `--nvram FOLDER`. Not yet:
pop'n 18's boss battles, mods that replace data files, a real board or reader. On Windows 10 they
start and run.

**beatmania IIDX:** drop the game's folder:

| Game | Folder | Cards |
|---|---|---|
| 9th style | `C02` | magnetic |
| 10th style | `D01` | magnetic |
| 11 IIDX RED | `E11` | magnetic |
| 12 HAPPY SKY | `ECO` | magnetic |
| 13 DistorteD | `FDD` | IC (e-AMUSEMENT PASS) |
| 14 GOLD | `GLD` | IC |
| 15 DJ TROOPERS | `HDD` | IC |
| 16 EMPRESS | `I00` | IC |

- A new cabinet starts set up, and test mode's changes are kept. They play offline with movies,
  both players' keys, turntables, Effect, VEFX and the effector sliders.
- **e-AMUSEMENT:** turn it on in test mode, then restart. Each side has a card and a 10-key (Card /
  Keypad, P2 Card / P2 Keypad). A new card is registered in the game (DJ name, PIN); best EX score,
  DJ level and clear are kept per chart.
- **The effector** (10th style on): Effect and VEFX work as on the cabinet, with HIRAKU's own echo,
  reverb and EQ (and PITCH from GOLD on).
- EMPRESS: a new pass accepts terms (turntable left to はい, then a key).
- Not yet: KONAMI ID, rivals, most rankings; lamps and ticker unchecked. Under Wine only so far.

Details: [PLAY-NOTES.md](PLAY-NOTES.md#bemani-pc-popn-music-1518-beatmania-iidx-9th-style-to-empress)
