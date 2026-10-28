# Playing beatmania III The Final with HIRAKU

## Adding a game

Drop the game's files on the launcher's window, or use **File > Add game files** (or
**Add a game folder**). From a terminal, `tools/ingest.py FILE...` or `hiraku --add FILE...`
does the same.

The files can be:
- the MAME ROM set (`.zip`, `.7z`, or a folder of its files);
- the game's CHDs, which are unpacked with MAME's chdman (installed, or MAME's flatpak) into
  `dumps/<game>/`;
- disc and disk images (`.cue`/`.bin`, `.iso`, `.img`).

Give them all at once, in any order. The ingester recognises the game from:
- for Firebeat, the program on the CD;
- otherwise, the set's name or a CHD's name.

Several games' files can go at once too (a few sets, or a whole MAME `roms` folder): they're told
apart by set name, a CHD by its folder or by the sets MAME has its disk in, and each game is added
in turn. A clone given with its parent also gets the parent's disks; a BIOS set (`sys573.zip`)
goes with every game.

**Moved, or left where they are:** the launcher asks, when files are added, whether to move them
into the HIRAKU folder or leave them where they are (`tools/ingest.py --place move|stay`;
**Settings > General > Games' files when added** answers for good):
- *Move*: the CHDs, the disks and the ROM set (a PC game: its whole folder) go into
  `dumps/<game>/` (the set into `dumps/<game>/set/`) and are gone from where they were: renamed
  on the same drive, copied then removed from another. A game that can't be added (a CHD
  missing) has its files put back. The BIOS and parent sets found beside the set (`sys573.zip`)
  stay, as other games use them.
- *Leave*: HIRAKU reads them where they are (their full path in `machine.txt`): keep them there,
  and a USB drive plugged in, to play.
- The ROMs are copied in either way (they're small), and a Firebeat game's discs are unpacked into
  `dumps/<game>/` either way.

**The game's files, checked:** the launcher's details list every file a game needs, with its size
and MD5. A MAME game's ROMs and CHDs are checked against MAME's SHA-1 (✓ the dump MAME knows, ✗
another one, – missing); a disk unpacked from its CHD, a Python game's and a PC game's files are
listed with nothing to check them against (•). Files over 64 MB are hashed only when asked ("Hash
the big files too"); the hashes are kept in `dumps/<game>/.checklist.json`. From a terminal:
`tools/checklist.py GAME [--big]`.

It also tells each image's role from what it holds: a hard disk, a DVD, or a second CD. The
sections below give each game's files, and the manual form with the roles spelled out
(`--romset`, `--hdd`, `--dvd`, `--cd2`).

CHDs need room once unpacked:
- beatmania III's hard disk: 8–21 GB (1st MIX 8 GB, the others 21 GB); stored with its zero runs
  as holes, 3.4 GB (1st), 6–7 GB (CORE REMIX, 6th, The Final) or 18 GB (7th);
- a pop'n music DVD: 4.6 GB;
- a CD: under 700 MB.

## Updating a release package

A release package's launcher looks for a new release on GitHub when it starts (Options >
Settings > General turns that off), and says so above the game list. **What's new…** shows the
release notes; **Update and restart** downloads the package for your system, checks it against
GitHub's SHA-256, and closes the launcher. `tools/update.py`, run from the new package, then puts
the new files in and starts the launcher again. It never touches `dumps/`, `user/` or `out/`.
The files it replaces go to `update-old/`, and back if anything fails; the launcher removes the
leftovers on its next start. **Help > Check for updates** looks again at any time.
`user/update.log` keeps a line per update. A checkout or a build by hand has no `VERSION` file
and doesn't offer updates: use git.

A game started without the launcher (from a shortcut or the command line) says so too: for six
seconds after it starts, a banner at the bottom of its window names the new release
(`runtime/host/notice.h`). It reuses the launcher's last check while it's under 6 hours old, or
asks GitHub in the background; it shows only if the answer arrives within the game's first 15
seconds, and the same setting turns it off (or `HK_UPDATE_NOTICE=0`). Games the launcher starts
never show it, and it changes nothing the game computes.

## Once: build and ingest your dumps

```bash
cmake -S . -B build/cmake -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
ninja -C build/cmake hiraku-bm3final
tools/ingest.py /path/to/cd.cue --romset /path/to/bm3final.zip --hdd /path/to/hdd.img
analyze/ghidra.sh bm3final && analyze/prep.py bm3final && ninja -C build/cmake hiraku-bm3final
```

**Optional, once your games are ingested:** `tools/pgo.py` rebuilds with profile-guided optimisation, trained on the regression cases of the games you have. It takes about 15 minutes. Results are identical, and some games need much less CPU: Keyboardmania −19 %, DDR −27 %, pop'n −3 %. Run it again after updating HIRAKU.

The ingester writes `dumps/bm3final/`: the unpacked program, `machine.txt` (disc and HDD paths) and `rom/` (dongle image, default NVRAM, sound program). It needs:
- the GCC01 program CD;
- the MAME `bm3final` ROM set;
- the raw HDD image.

## Play

The launcher is the easy way:

```bash
build/cmake/hiraku
```

The list goes by series (beatmania, pop'n music, DDR...), then the systems a series ran on (pop'n music: DJ Main, Firebeat, Konami Python, PC), then the games by year; a series or system you close stays closed. It shows the games you've added; **View > Show games not added yet** lists every game HIRAKU runs, and what each one needs. Pick the game and press **Play**. **Controls** and **Lights** map your keyboard, controllers and controller LEDs, and **Settings** has a tab for each kind of option: Display, Audio, e-AMUSEMENT, Cabinet I/O and General (the launcher and its folders). On a first boot, the game's test-mode steps are shown under the list, and the Status column says what a game's next start does differently (First boot, Installs at the next start, its mods). The **Game** menu (or right-clicking a game) has what is the game's own: Mods, Unlocks, Picture, the install cassette or CD, its save folder, and **Arguments…**, command-line options for that game alone. If a game stops with an error, the launcher says why (a crash, a missing DLL, an instruction the CPU doesn't have) with the game's last messages, and opens its log file for you to send.

Or start a game directly:

```bash
build/cmake/hiraku-bm3final --window
```

The machine boots like a cabinet. The skipped BIOS time passes instantly. Then the game runs its own STARTUP checks for about 20 s; hold **Tab** to fast-forward through them.

| Key | Action |
|---|---|
| Z S X D C | P1 keys 1–5 |
| Left / Right (or Left Shift / A) | P1 turntable |
| Space | P1 foot pedal |
| 5 | coin (the game wants 2 for one player) |
| 1 | P1 start |
| F2 | test mode, 9 service |
| Tab (hold) | fast-forward |
| F11, Alt+Enter | fullscreen |
| F10 | picture: off, CRT light, CRT full, CRT analog |
| F9 | the game's lights over the picture (docs/LIGHTS.md) |
| F8 | the CRT shader's settings, changed while you play (below) |
| Ctrl+1 … Ctrl+9 | type one of the game's codes (below) |
| Esc | quit |

P2 uses M K Comma L Period, Up/Down and 2.

**Navigating:** turn the turntable to choose, and press a white key (1, 3 or 5) to confirm. Start toggles DOUBLE on the option panel.

**Unlocks:** right-click the game > **Unlocks** applies the COLOR CHECK code. It unlocks:
- Miracle Moon, CLOUDY MUSIC, RETROFUTURE, The Least 100 sec and COREDESAT;
- IMPLANTATION's ANOTHER chart.

The title screen then shows the alien icon. The floppy-disk unlocks (RETROFUTURE via SIMON,
CLOUDY MUSIC, sozai mode) are earned per player and listed there too.

**Codes, typed for you:** the button codes players typed on the cabinet (hidden songs, modes),
for the games that have a `games/<id>/macros.txt`.
- **In the window:** Ctrl+1 to Ctrl+9 type the game's first to ninth code where you are now, for
  example on the title screen with credits in. The window title says "typing" until it's done.
- **The list:** right-click the game > **Unlocks** shows each code with its key and where to
  type it. A code the game reads at power-on can be set to type itself at every start there.
- **From a terminal:** `--macro NAME` runs one at its own frame, `--macro NAME@FRAME` at a given
  frame.

**Saves:** NVRAM (operator settings, rankings) lives in `~/.local/share/hiraku/bm3final/`. It is saved on Esc, on window close and on Ctrl-C. Use `--nvram DIR` to keep it elsewhere.

**A portable install.** A HIRAKU folder that holds a folder named `user` keeps everything that is yours inside it: the saves in `user/data/<game>/` (instead of `~/.local/share/hiraku/<game>/`), and the launcher's settings, controls, lights and picture settings in `user/config/` (instead of `~/.config/hiraku/`). It also always runs as itself: the launcher and the games take that folder as their root wherever it is, and the launcher's "HIRAKU folder" setting is greyed out. So the folder can be copied to another PC or a USB stick with everything in it. `tools/package.sh BUILD OUT.tar.xz portable` builds a package with the folder; for an existing copy, create it, then copy `~/.local/share/hiraku/*` into `user/data/` and `~/.config/hiraku/*` into `user/config/` to bring a setup along. Elsewhere, these docs give the usual places; in a portable install, read them as `user/data` and `user/config`. A package from `tools/package.sh` also brings the launcher's own Qt (`bin/qt`; `bin/hiraku` is a script that starts `bin/hiraku.bin` with it, and the games it starts get your own environment back), so a PC without Qt 6.12 runs it. What still comes from the system: glibc 2.38 or later, the graphics drivers, X11 or Wayland, fontconfig, D-Bus, glib, OpenSSL and the sound server, which every desktop has.

**Persistence is unconfirmed.** In MAME and in HIRAKU alike, the scripted sessions so far never made the game write its NVRAM. It probably writes only when a setting or a ranking changes, and none of those sessions changed either. Check whether a test-mode change or a new high score survives a restart.

## The picture

**Picture** in the toolbar picks how the game looks:

| Mode | What it is | Cost |
|---|---|---|
| **Off** | sharp pixels (the default) | none |
| **Light** | guest(r)'s crt-guest-advanced-fast: scanlines, a phosphor mask, glow | small |
| **Full** | TroggleMonkey's crt-royale: beam shapes, real phosphor-mask textures, bloom, halation, optional curvature | moderate |
| **Analog** | Zoki's Lola Retro: the whole analog chain, the cable's signal, the TV's or monitor's electronics, a beam whose spot grows with brightness, the mask, the glass, phosphor afterglow | the heaviest |

F10 cycles the four while you play, and the window's title says which one is on.

**Changing them while you play (F8):** a panel with the shader's settings, the same as the
launcher's sliders, beside the picture so you see each change at once. F8 shows it on the right,
again on the left, a third time hides it. Up and Down pick a setting, Left and Right change it (with
Shift, ten steps at a time); Backspace puts back the saved value, Delete the default. **Enter**
saves the changes for all games, **Shift+Enter** for this game alone, into the same files the
launcher's Picture dialog keeps (a game started from the launcher; on the command line, give a
folder with `--picture-config DIR`). Amber values differ from the default, and a star marks a change
not saved yet. **Settings > Display** maps each of its controls to another key or to a controller's
button, hat or axis, as the Controls dialog does (so a cabinet's buttons can drive it; with a
keyboard, Shift still does ten steps, or saves for this game); on the command line,
`--shader-key ACTION=BINDING` (show, up, down, less, more, saved, default, save, save_game). While
the panel is shown, the game doesn't get its controls; Esc still quits. It's
drawn small (8x8 letters, 316x370), so it fits a 640x480 screen; larger windows draw it two, three
times as big.

**Analog's looks:** Lola Retro comes with presets, each a tube, a cable and a set-up: a consumer TV
on SCART RGB or composite, a Trinitron on S-Video, an old TV on the antenna, a studio monitor
(PVM), an Amiga 1084S, a CRT projector, PC monitors from average to "extreme". Pick one under
**Look** in Analog's tab, then fine-tune it with the sliders. Each game starts with the look for its
cabinet's monitor: the Amiga 1084S (Lola's choice for 15 kHz arcade games) for System 573 and
Twinkle, the studio monitor for DJ Main's medium-resolution one (512×384, 24 kHz), the PC VGA
monitor for Firebeat (640×480, 31 kHz). A look leaves the curvature and the board's timing as they
are, and HIRAKU shows the whole picture (no overscan) unless you set it.

**Settings for:** you can set the mode, and each shader's settings, for all games or for one.
To do it for one game, select it first, or right-click it > **Picture for…**. A game's own
settings only hold what differs from all games', and **↺** puts a slider back.

**Live changes:** the sliders change a running game at once, and **Cancel** puts everything
back.

**The shape:** every game is shown as on its cabinet's monitor, whatever its frame's pixel
count: 4:3, except beatmania IIDX (Twinkle), whose cabinet has a widescreen 16:9 CRT. The
PlayStation-based boards (System 573, Twinkle) switch video modes as they go: GuitarFreaks and
DrumMania show their menus at 512×480, attract and play at 320×240 and test mode at 640×480,
and beatmania IIDX draws 640×240. On the tube each fills the same shape, so the pixels aren't
square. Each frame keeps its own lines and pixels, stretched to that shape.

**480-line games:** HIRAKU draws Firebeat's 640×480 progressive, with a scanline on every
line, like the cabinets' monitors. The shaders would otherwise treat 480 lines as interlaced
(480i, as on a TV). To get that look back, set guest's **Interlace Mode** to 1, turn on
royale's **Interlacing - Toggle**, or set Lola's **Console timing** to 0 (auto).

**Files and options:**
- The settings live in `~/.config/hiraku/picture/` (all games) and
  `~/.config/hiraku/picture/games/<game>/`. Each holds `<shader>.params` files of
  `name = value` lines, and a RetroArch preset's parameter lines work as is.
- From a terminal: `--picture off|light|full|lola` and `--picture-config DIR` (repeatable: later
  folders win).
- When a game exits, it prints each mode's GPU time per frame. That is the number to watch
  on a small GPU.
- If OpenGL 3.3 isn't available, the game says so and shows sharp pixels.
- The shaders themselves: `shaders/README.md`.

## Controllers

Joysticks are opened at start and when plugged in. The console lists them as `joy0`, `joy1` and so on.

The defaults map `joy0` buttons 0–4 to the keys (0–6 on IIDX), axis 0 to the turntable, button 7 to beatmania III's pedal, 8 to start and 9 to Effect (DJ Main, IIDX); coin is the 5 key.

IIDX-style controllers differ in which button and axis numbers they use. The easiest way to map one is the launcher's **Controls** dialog: click a binding, then press the button or spin the turntable. Pressing anything lights up what it's bound to, and the dialog shows the last input pressed. A warning sign marks an input bound to two controls of one game, and binding one offers to move it.

- **Controllers by name.** The dialog binds a controller by its name (`joy[DAO FPS]:button3`), so it keeps its bindings whichever order controllers are plugged in. Older bindings by number (`joy0`) still work; the dialog offers to tie them to the controllers connected.
- **Set up**: *Bind all in turn* asks for each control in order (Back and Skip as you go). Each controller can take the cabinet's default layout or a layout you saved for it, as player 1 or 2, and player 1's controller can be copied to player 2's. Saved layouts live in `controls/layouts/` in the settings folder, so they carry to another PC.
- **Turntables**: a binding's menu sets the speed (×0.5 to ×3) and can reverse the direction; the dial beside it turns as the game will see it.
- **Analog controls** that take a position: Keyboardmania's wheels, GuitarFreaks' effect knobs, the IIDX effector sliders. An axis bound to one sets it from one end to the other (reversed with a negative speed). Keyboardmania's wheels can also go up and down on keys or buttons (P and M, the keypad's + and -); the wheel stays where it's left.
- **An axis as a button** (`axis2+`, `axis2-`, a trigger): some USB controllers, PS2 adapters among them, report their axes only once something changes, so the first press of a key also brings every axis's resting value. HIRAKU takes an axis's first report as where it rests: one that rests at an end (an adapter's unused axis, a trigger) isn't pressed that way until it has moved back. The Controls dialog binds the button such a press sends, not the resting axis. The game's log says `input: joy0 axis 2 first reads -32768: it rests there` when it sees one; `HK_INPUT_LOG=1` logs every button, hat and axis a controller sends, for finding out what one does.
- **Try in a game** saves and starts one of the cabinet's games; press Test there for its I/O check.

The dialog saves `~/.config/hiraku/controls/<cabinet>.txt`, which the launcher passes as `--bindings`. The beatmania cabinets (DJ Main, IIDX, III) share one file, `beatmania.txt`, and the pop'n music ones (DJ Main, Firebeat, Python) share `popn.txt`: each game uses the controls it has and ignores the rest.

By hand: write a bindings file and pass `--bindings FILE`. See `docs/bindings-bm3.example`; the format is in `runtime/host/input.h`.

**Lights:** `--lights FILE` drives HID controller LEDs from the cabinet lamps, and `--lights-socket PATH` publishes them for other tools. See [LIGHTS.md](LIGHTS.md).

### Card readers

A real e-AMUSEMENT card reader can stand in for the cabinet's: a card held to it becomes that player's card in the game and is put to the game's reader, and its 10-key is the game's. That works in pop'n music 9–14 (one reader, player 1's) and in GuitarFreaks and DrumMania with their card units (`--npu`). A card's number is its 16 hex digits. A pop'n card is the save folder's `cards/NUMBER.bin` or IC card NUMBER, as `-card0` would make it; a 573 card unit's is `cards/NUMBER.bin`. Set the readers in the launcher's **Settings > e-AMUSEMENT > Card readers**, or give the options yourself (`runtime/host/readers.h`), as spice2x spells them:

- `--reader PATH`: a Konami e-AMUSEMENT reader (the ICCA/ICCB/ICCC readers spice2x's `-reader` drives) on a USB-serial adapter. Use its `/dev/serial/by-id/...` name, which stays the same whichever socket it's in. A second `--reader` is player 2's. Serial ports belong to the `uucp` group on Arch and CachyOS (`dialout` elsewhere): `sudo usermod -aG uucp $USER`, then log in again. The reader's 00 key is Enter in games whose 10-key has no 00.
- `--scard`: NFC readers such as the ACR122U, through PC/SC. Needs `pcsc-lite` and `ccid`, and the service running: `systemctl enable --now pcscd.socket`. If the kernel's `pn533_usb` driver takes the ACR122U first, blacklist it (`echo "blacklist pn533_usb" | sudo tee /etc/modprobe.d/no-pn533.conf`). A card that isn't an e-AMUSEMENT PASS (FeliCa, a phone) gets an `E00401…` number, as spice2x's `-scardfix` does.
- `--cardio`: cardio readers (USB HID), read through `/dev/hidraw*`, which only root can open by default. A udev rule lets you: `/etc/udev/rules.d/70-cardio.rules` with `KERNEL=="hidraw*", ATTRS{idVendor}=="VVVV", ATTRS{idProduct}=="PPPP", TAG+="uaccess"` (the reader's IDs: `lsusb`). On Windows the readers are found through Windows' HID calls, with nothing to set up.

The first reader of each kind is player 1's, the second player 2's. A card held to the reader is read once (it stays read for 3 s), and a card the game already holds has to be taken back (Insert, the card button) before another goes in.

### Cabinet I/O: beatmania IIDX's BIO2

`--bio2 PATH` (**Settings > Cabinet I/O**) takes a real IIDX cabinet's BIO2 board, as IIDX 25 and 26 drive it (its BI2A firmware), on its USB serial port. Its keys, turntables, start, EFFECT, VEFX, TEST, SERVICE, effector sliders and coin become the game's controls, and the game's lamps light its key lamps, panel, spotlights and neon, with the ticker showing beatmania IIDX's 16-segment display. It's for the beatmania games: IIDX on Twinkle uses everything, and III and the DJ Main games use what their cabinets have. A board with IIDX 27's BI2X firmware speaks Konami's newer ACIO2, which HIRAKU doesn't yet; it says so. The layout is spice2x's, not yet checked on a board: if the turntables turn the wrong way or a control is missing, `HK_ACIO_LOG=1` shows the board's packets.

### Cabinet I/O: pop'n music's USB I/O board

`--popn-io auto` (**Settings > Cabinet I/O**) takes the USB I/O board of a pop'n music cabinet that runs pop'n music 15 or later, the one those games drive through their `ezusb.dll`. Its nine buttons, TEST, SERVICE and coin become the game's controls, and the game's button, top and side lamps light the cabinet's. It works for every pop'n music game: the DJ Main ones (1–3), Firebeat (4–8) and Python (9–14). `auto` takes the first board it finds. If there's more than one, give the board's USB port as Linux names it in `/sys/bus/usb/devices` (for example `--popn-io 3-2.1`).

**On Windows** HIRAKU drives the board the way the games do, through an `ezusb.dll`: give it with `--popn-io-dll FILE` (Settings: "its ezusb.dll"). That's the game's own `ezusb.dll` (pop'n music 15 or later; the board needs Cypress's driver installed, as on the cabinet's PC), or a replacement that drives something else, such as UltimatePopnController's. A PopnIO in its REALIO mode is driven like Konami's board, through the real `ezusb.dll`. The DLL is 32-bit and the games are 64-bit, so a small helper, `hiraku-ezusb.exe` beside them, loads it and passes the buttons and lamps along. A real board takes about 20 s to start, while the DLL loads its firmware.

**On Linux** HIRAKU talks to the board itself. The board keeps no firmware of its own. The game loads it into the board's memory at every start, and it's gone at power-off. A board that comes up without it gets it from `--popn-io-dll FILE`, the `ezusb.dll` of your pop'n music 15 or later. HIRAKU reads the firmware from that file and loads it the way the game does. The board then leaves and comes back with its firmware, which takes a few seconds. The firmware only goes into the board's memory: HIRAKU never writes the board's EEPROM or its security chip. A board that copies Konami's with firmware of its own, such as PopnIO in its REALIO mode, is used as it is (without PopnIO's enhanced polling). Details: [POPN-IO.md](POPN-IO.md).

On Linux, only root can open a USB device directly by default. A udev rule lets you, in `/etc/udev/rules.d/70-popn-io.rules`:

```
SUBSYSTEM=="usb", ATTRS{idVendor}=="04b4", ATTRS{idProduct}=="8613", TAG+="uaccess"
```

Then run `sudo udevadm control --reload` and plug the board in again. 04b4:8613 is the ID of every Cypress FX2LP chip, the board's included, before and after its firmware. A bare FX2LP chip from something else (some logic analysers) has the same ID, so unplug those when you use `auto`, or give the board's port. This hasn't been checked on a board yet. `HK_POPNIO_LOG=1` shows its packets (Linux).

An UltimatePopnController (CrazyRedMachine's) needs none of this on Linux: its buttons are a game controller's (**Controls**), and its lamps are named HID outputs that a lights file drives ([LIGHTS.md](LIGHTS.md)). On Windows it works either way: as a controller with a lights file, or with `--popn-io auto` and its own `ezusb.dll`.

## Latency

**In the game:** HIRAKU measures it as the time from a key press to the RF5C400 starting the keysound. beatmania III scans its keys once per video frame (59.94 Hz), so this is **2–20 ms, mean about 9.5 ms**, exactly as on a real cabinet. Everything above that is the PC.

**In real-time mode, audio is the clock.** The machine runs until 8 ms of sound is queued (`--audio-ms N`), and controls are read every couple of milliseconds while it waits. Total latency is therefore roughly:

> 2–20 ms (game, frame-based) + `--audio-ms` (8) + the audio device buffer (128 frames requested = 2.9 ms) + your sound server

On exit, the console prints the buffer it actually got.

**The queue tunes itself.** `--audio-ms` (8; launcher: Settings > Audio > Audio queue) is where it starts and the least it goes to. As each piece of sound comes, HIRAKU works out the lead it needed: how little was left of the queue, plus one device buffer (the device takes whole buffers, so less than one left breaks the sound). The queue is then kept at what covers all but the two worst seconds of the last minute, up to 30 ms. It grows while frames keep coming late on this PC and comes back down when they stop; a rare stall that no sensible lead covers breaks the sound once instead of adding to every keysound for the rest of the game. The first 10 s don't count (the game's boot, its loads, the display's pacing settling), nor does a stall of 100 ms or more. `--audio-fixed` (the Fixed box) keeps the queue at `--audio-ms` all game long: the least delay, and a click whenever a frame comes later than it covers.

The exit report prints where the queue ended and its range over the run, the device's breaks (all, and after the first 10 s, counting only breaks the sound came back from, not the empty pulls after the game stops), and the run's three latest moments: the frame, the lead it needed and where the time went (`waiting` is the display pacing's own waits; `present` includes them). `HK_LOG_STALLS=1` prints each break, and each second's needed lead, as it happens.

**Priority.** With a window, the game's threads run ahead of other programs, so a browser or a build doesn't delay its frames and sound: on Linux at nice -11 (as far as `RLIMIT_NICE` allows, else through RealtimeKit, as PipeWire raises itself), with a timer slack of 1 µs so that its waits end on time; on Windows above normal. With Feral's GameMode installed, the game asks for it while it runs (the CPU governor at performance, and whatever else your `gamemode.ini` says); on most distributions GameMode may change the governor only for users in the `gamemode` group (`sudo usermod -aG gamemode $USER`, then log in again), and the exit report's `system:` line says what the CPU ran at. The sound server and the desktop keep their own priorities (on a desktop that puts programs in cgroups of their own, they're in another one anyway). A sound card of its own (`--audio-device`) is written by a real-time thread (`SCHED_FIFO` 50, or RealtimeKit's round-robin 20), which sleeps until the card has room. `--normal-priority` (launcher: Settings > Audio, "Ahead of other programs" unticked) leaves the game at normal priority; the card's writer stays real-time. The log's `priority:` line says what it got, and with a card alone the `audio: the card took ...` line how its writer runs.

**The disk.** Once the machine has opened its media, a thread of the lowest CPU and disk priority reads the game's binary and every file it has open up to 1 GB (a disc's image, its ROMs) into memory, and locks them there where `RLIMIT_MEMLOCK` allows (at most 1/16 of the RAM). The binary is mostly the games' recompiled code: without this, a stretch of code run for the first time (a new song, a screen not seen yet) was read from the disk at that moment, and on a hard disk each read is a seek, 10-60 ms in which the frame waits (measured on beatmania IIDX 8th style from a 5400 rpm disk: up to 170 ms; with the files in memory, none). On a PC short of memory, files only read in are taken back by the kernel within minutes, so locking matters. Hard disk images (several GB) aren't read in. The log's `preload:` line says what it read and locked.

**Sound server settings:** the first reports came before the performance fixes in this release. Re-test with the defaults first. On PipeWire the server quantum adds its own latency; a smaller one (`PIPEWIRE_LATENCY=256/48000`) is worth trying, but only if the default is already clean.

**Your own sound card, and its buffer** (launcher: Settings > Audio > Output, Device buffer).
`--audio-device hw:CARD=Generic,DEV=0` opens that card through ALSA with no sound server in
between: the lowest latency, and nothing else plays on it meanwhile. `aplay -l` lists the cards
(`card 1: Generic ... device 0` is `hw:CARD=Generic,DEV=0`). The card is borrowed from the sound
server the way JACK does it (the D-Bus device reservation, `org.freedesktop.ReserveDevice1`):
PipeWire's WirePlumber lets it go when asked, is told "no" while the game plays, and takes it
back the moment the game closes. (Without the reservation, WirePlumber lost the card while a game
held it and didn't get it back after: no sound from it for the desktop until it was restarted.)
If the sound server still has the card open anyway, the game says so and uses the default
instead. `--audio-buffer N` sets the device buffer in frames (default 128, 2.7 ms at
48 kHz): the sound server's quantum, or the card's period. On a card of its own HIRAKU writes
through ALSA itself, keeping a ring of periods full: two on an onboard card (at 64 frames, 128
frames or 2.7 ms), four on a USB card (at 64, 5.3 ms): a USB card's driver starves on fewer and
plays stale sound without reporting it, which made SDL's own ALSA output (two periods) unusable on
USB cards below 384 frames. `HK_ALSA_PERIODS=N` sets the number. Smaller is sooner, until it
crackles; the queue above still tunes itself to it. The exit report's first
`audio:` line says what was opened, and with a card alone, an `audio: the card took ...` line
says whether it kept pace (100 %) and how often it ran dry.

**On Windows** (launcher: Settings > Audio > Output, Exclusive). Output lists the devices by the
names Windows shows; `--audio-device "Speakers (Realtek(R) Audio)"` plays on that one, shared
through the Windows mixer. `--audio-exclusive` (the Exclusive box) takes that device, or the
default one, alone in WASAPI exclusive mode: no mixer in between, its buffer as small as the
device allows (`--audio-buffer`, or the device's minimum if that's larger), other programs silent
on it meanwhile. HIRAKU tries float, 32, 24 and 16 bits at the game's rate, then 48, 44.1 and
96 kHz. If the device refuses (another program has it alone, or "Allow applications to take
exclusive control of this device" is off in its Advanced properties), the game says why and plays
shared. The exit report then has an `audio: the device took ...` line with the share of the device's
rate it took and the periods the game's sound came short.

**ASIO** (Windows; launcher: Settings > Audio > Output, the `ASIO:` entries). `--audio-asio NAME`
plays through the card's own ASIO driver (the name in its control panel; a wrong name lists the
installed ones): no Windows audio in between. The buffer is the one set in the driver's control
panel, unless `--audio-buffer` asks for another the driver allows. The game's rate if the card
takes it, else 48, 44.1 or 96 kHz; 16, 24 or 32-bit or float samples, as the driver asks. If the
driver won't start, the game says why and plays on Windows' default output. The exit report's
`audio: the ASIO driver took ...` line says whether it kept pace and how often the sound came
short. HIRAKU declares the ASIO driver interface itself, from the specification (Steinberg's SDK
is GPLv3 since October 2025 too, but isn't needed). Checked under Wine with pwasio (PipeWire's
ASIO driver for Wine): float, 48 kHz, 64-frame buffers, 99.97 % of the rate; and on real Windows.

**System 573 and Twinkle: sound every millisecond** (the default; launcher: Settings > Audio > "Sound every millisecond"; `--mame-sound` turns it off). These cores run MAME's devices, and MAME hands their sound on 50 times a second: the sound would reach the PC in 20 ms pieces, which adds up to 20 ms of latency, uneven from moment to moment, and the controls would be read once per piece. HIRAKU hands it on every millisecond instead. Measured on beatmania IIDX 3rd style (the exit report's "from a frame starting ... to that moment's sound entering the device"): 9.9 ms every millisecond, 30.1 ms in MAME's pieces.
- The machine runs the same (the regress cases show the same pictures).
- On beatmania IIDX the keysounds then start when the game triggers them. MAME's RF5C400 starts them at its previous update, up to 20 ms early, so the sound no longer matches MAME's recordings.
- On the digital board (DDR 3rd Mix on, DrumMania, GuitarFreaks) the MP3 decoders must still update at MAME's 50 Hz: the game sees each update's request for data. Their sound up to now is decoded ahead of the update on a copy of the decoder, and checked when the update comes. Almost always it's the same (all but 220 of 11 million samples in ddr3mj-play): the few that differ are a song's first or last milliseconds, played as worked out. It costs ~0.1 ms a frame on DrumMania (five decoders).

`--mame-sound` keeps MAME's sound, for comparing with it: the regress cases use it.

## Frame pacing

The games don't run at 60 Hz. Each sets its own video timing, which HIRAKU runs as MAME does:

| Games | Frames per second |
|---|---|
| beatmania III | 59.94 (VGA timing, 25.175 MHz) |
| pop'n music 4–8, Animelo 1/2 | 58.4866 (16.6666 MHz pixel clock) |
| Keyboardmania | 58.51 |

**Smooth frames on a fixed-refresh display (the default).** A 60, 120 or 240 Hz display can't
show 58.49 frames a second evenly: some frames would stay up a refresh longer than others, and the
notes judder. So when the display's rate is within 3 % of a whole multiple of the game's, HIRAKU
runs the game on the display's refreshes:
- Every frame stays up the same number of refreshes (1 at 60 Hz, 2 at 120 Hz, 4 at 240 Hz).
- The game runs at the display's rate: pop'n and Keyboardmania **2.6 % faster** at 60/120/240 Hz,
  beatmania III 0.1 %. The sound plays at the same speed, so it's 2.6 % higher in pitch. The
  game itself is unchanged: its judgement, charts and sound stay in step with each other.
- On Wayland, HIRAKU learns from the compositor when each frame reached the screen, and finds
  how late it can hand a frame over (the lowest latency that still makes the refresh). With
  KWin 6.7's fifo protocol, 99.8-99.97 % of frames stay up exactly their refreshes (measured at
  144 and 240 Hz); the rest are the compositor's own.
- The console says what it chose (`display sync: on: 60.00 Hz display, 1 refresh per frame, the
  game at +2.6 % speed`), and the exit report's `video: display sync` line how it went.

**Motion on a fast display** (launcher: Settings > Display > Motion; DJ Main, Firebeat, Twinkle and System
573 games). Both need Wayland with the fifo protocol (KWin 6.7), or Windows (new, checked under
Wine only). See docs/SMOOTH.md.
- **Smooth motion** (`--smooth-motion`): the game runs at its exact speed on any display (144 Hz
  too), and every refresh shows the notes and other moving things where they are at that
  refresh's moment, carried on along their move, instead of a frame held for 2 or 3 refreshes
  unevenly.
- **CRT beam** (`--crt-beam`): the Blur Busters CRT Beam Simulator. On 120 Hz and up (best at
  240), each refresh lights the part of the tube the electron beam sweeps then and lets the rest
  fade, so moving notes stay as sharp as on a CRT. Set **Display gamma** to your display's (2.2
  for most), use SDR, and turn off the display's ABL/APL and its own strobing; **Brightness** trades
  light for sharpness (0.7; about 0.5 at 120 Hz). It works with any picture mode.

**When it's off:**
- **A display that doesn't fit**, such as 144 Hz (58.49 × 2 = 117, × 3 = 175): frames are shown as
  they come and judder (2 and 3 refreshes alternately). Set the display to 60, 120 or 240 Hz.
- **Variable refresh (FreeSync, G-Sync, HDMI VRR):** turn display sync off (launcher: Settings >
  "Smooth frames on a 60, 120 or 240 Hz display"; or `--display-sync off`). The display then
  follows the game at its exact speed. In KDE: enable FreeSync in the monitor's menu, set System
  Settings > Display > Adaptive sync to Automatic, and play fullscreen (F11).
- **Fast-forward** (Tab).

**Free-running (display sync off):** audio is the clock, and each frame is shown as soon as it's
drawn. The exit report's `video:` line shows how evenly: at the game's rate (mean error
-0.005 ms), within about ±3 ms (sd 1.3 ms, the sound card's 2.8 ms blocks). On a fixed-refresh
display: at 60 Hz one frame shown twice every 0.66 s; at 144 Hz frames alternate 2 and 3
refreshes; at 240 Hz about one frame in ten stays up 5 refreshes instead of 4.

**The picture at the end of vertical blank.** A game changes its picture in the vertical blank
(its vblank interrupt flips pages, scrolls), and the monitor shows the result from the blank's
end. The window shows each core's picture as of then, where the frame's digests and snapshots
keep the start of the blank (the oracle's moment): beatmania III, pop'n 4-8, ParaParaParadise,
Keyboardmania, DJ Main, the System 573 and Twinkle. Not the Python games: PCSX2 draws at the
vsync. With display sync, the frame's emulation up to the blank's end is all that has to come
before the picture: on the 573 the burst budget fell from 3.5 to 2.0 ms (ddr2m). Firebeat games
draw in their vblank interrupt, so they gain nothing there. Free-running, every core's picture
comes ~2 ms after its frame starts instead of a frame (~17 ms) later.

**Direct scanout.** In fullscreen (F11) the compositor can put HIRAKU's picture on the screen
without compositing it, which should bring it sooner. HIRAKU gives it what it needs (an opaque buffer
the size of the screen, no pointer, the window marked as a game) and the exit report says how
many frames went out that way (`video: N frames shown, M of them straight from HIRAKU's
picture`). On KDE it also takes nothing over the game. A colour profile (System Settings > Display >
Color profile, from EDID or ICC) or "prefer accuracy" most likely keeps KWin compositing, since it
then converts every frame's colours: on the desktop this was written on (KWin 6.7, EDID profile,
prefer accuracy) no frame went out directly, fullscreen or not. Fullscreen at 240 Hz, KWin still
showed each picture 5.3 ms after HIRAKU handed it over (the exit report's "commit to screen").

Some cores hand their sound on once per frame: DJ Main, the Python games (pop'n music 9-14) and
Keyboardmania with the XT446 card. For them, free-running, the window waits for the sound after
showing a frame and before the next one runs: the frame is emulated in a quick burst with the
controls just read, then shown, held to a steady time after its start (the slow end of recent
frames' work) so the pictures keep their cadence. Before, the wait came between a frame's
emulation and its picture, a whole frame (~17 ms) from the controls to the picture; now 2.6 ms
on DJ Main, ~5 ms on pop'n 9, 9 ms on Keyboardmania (whose synth card is heavy). The exit report's
`video: from a frame starting` line shows it, in either mode.

## The last runs' logs

The launcher keeps what each game printed (its log pane: the command line, the start-up, the
exit report with its `video:`, `audio:` and `picture:` lines, how it ended) in a file a run, for
the last 10 runs: `~/.local/share/hiraku/logs/<date>-<time>-<game>.log` (a portable package:
`user/data/logs/`). Help > **Open the last runs' logs** opens the folder. When something looks
wrong in a game, that file says what HIRAKU saw.

## Recording and testing

```bash
build/cmake/hiraku-bm3final --script oracle/scripts/bm3final-practice.his --wav out/practice.wav   # scripted, headless
build/cmake/hiraku-bm3final --frames 3600 --snap-every 300                                         # PPM snapshots
```

## beatmania III, APPEND CORE REMIX, 6th MIX and 7th MIX (M6)

The other four beatmania III games run like The Final: the same board, controls and options.
Each needs its MAME ROM set and its two CHDs, the program CD (`01`) and the sound hard disk (`02`):

| Game | Set | CHDs | Startup code |
|---|---|---|---|
| beatmania III | `bm3` | `gc97201`, `gc97202` | GQ972-JA-B |
| beatmania III APPEND CORE REMIX | `bm3core` | `a05jca01`, `a05jca02` | GCA05-JC-A |
| beatmania III APPEND 6th MIX | `bm36th` | `a21jca01`, `a21jca02` | GCA21-JC-A |
| beatmania III APPEND 7th MIX | `bm37th` | `gcb07jca01`, `gcb07jca02` | GCB07-JC-A |

```bash
tools/ingest.py bm36th.7z bm36th/                 # the set and the folder with its CHDs
analyze/ghidra.sh bm36th && analyze/prep.py bm36th && ninja -C build/cmake hiraku-bm36th
build/cmake/hiraku-bm36th --window
```

- **Where they are (2026-10-14):** all four boot to attract and play. Coins, mode select, sound
  select and the practice stage ("Instant Love" with DJ KONAMI) run with sound and scoring.
  Everything is recompiled: nothing is interpreted.
- **Against MAME** (`oracle/scripts/<game>-boot.his`): the boot screens match at the same frame
  numbers, and the attract is pixel-identical, 5–7 frames earlier than MAME's (bm3 5,
  7th MIX and The Final 7). It was 22–95 frames later until the instructions were charged
  MAME's cycles and a native exception's return took a pending interrupt at once
  (PLAN.md section 18).
- **The dongles:** MAME marks 6th and 7th MIX's dumps BAD_DUMP (0xc0 bytes where the others have
  0xc8), but the games accept them (IBTN: OK).
- **7th MIX's disk** has few zero runs: it takes 18 GB. If it doesn't fit beside the others,
  unpack it on another drive and give its path with `--hdd`: `machine.txt` takes any path.

## pop'n music 4–8, Animelo 1/2 and Mickey Tunes! (in progress, M7)

Each game needs its program CD (Disc 1), its data DVD (Disc 2) and its MAME ROM set (`popn4` … `popn7`). The ROM set holds three things:
- the dongle image;
- the SPU's 68000 program, `a02jaa04.3q`;
- the default RTC.

```bash
tools/ingest.py "Pop'n Music 4 (Japan) (Software Disk 1).cue" --dvd "Pop'n Music 4 (Japan) (Software Disk 2).iso" --romset popn4.zip
analyze/ghidra.sh popn4 && analyze/prep.py popn4 && ninja -C build/cmake hiraku-popn4
build/cmake/hiraku-popn4 --window
```

**Status:** pop'n 4, 5, 7 and 8 boot to attract mode with sound (pop'n 8: music select and full songs tested): boot checks, loading from the DVD, title, demo play. Standalone `.7z` ROM sets are accepted.

**pop'n 6, first boot:** it installs SPU data to flash, which takes about 14 minutes of game time (hold **Tab** to fast-forward). Then it boots normally.

Animelo 1 and 2 ingest the same way: `--dvd` takes the `extractdvd` ISO of the `02` CHD.

**pop'n music Mickey Tunes!** (`popnmt2`, GQ976 JBA): MAME's set and its two CHDs, `976jba01` (the program CD) and `976jaa02` (the DVD), ingest as they are (`tools/ingest.py popnmt2.7z popnmt2/`). It's a rental cabinet (MAME's `init_popn_rental`: `[board] cabinet = 6`). It boots to attract: the 976 boot program's checks, WARMING UP NOW, the Disney Interactive logo, the cast, the title and How to Play, as in MAME. The first Mickey Tunes (`popnmt`) needs its own program CD, `976jaa01`, which isn't on hand.

| Key | Action |
|---|---|
| Z S X D C F V G B | buttons 1–9 (odd = lower row) |
| 5 | coin |
| F2 / 9 | test / service |

Joystick buttons 0–8 are the nine buttons; button 9 is the coin.

**pop'n 8 unlocks:** in the launcher, right-click pop'n music 8 > **Unlocks**, pick the event
phase, and **Apply** (the game must not be running).
- **MAX** unlocks everything: the LIVE songs, EX charts, the Sugoroku de 8 songs and
  characters, expert courses, and a "secret" category with the title-screen songs.
- **Apply** writes into the save exactly what the game writes when an operator types the
  phase codes in EVENT OPTIONS, and the old save is kept beside it as `rtc.nv.<date>.bak`.
- The game needs a save first: start it once, through its first-boot setup.
- From a terminal: `tools/unlock.py popn8` lists the phases, and `tools/unlock.py popn8 phase=4`
  sets one.
- The per-credit codes (the LIVE button code, the title-screen timing) are listed there too.

**Sugoroku de 8, the event itself:** set the phase to **1** (or 2) and the game runs its 2002
unlock event, which HIRAKU plays unchanged:
- **The requirement:** before each round a roulette draws one (スゴロク・ノルマ), such as "clear
  a new song" or "score 70000".
- **The board:** if your round meets it, a dice roll (1–6) moves KRAFT around Japan, a board of
  1,004 spaces through 18 cities.
- **Unlocking a city:** the first time around, you get a city's unlock by landing on it exactly.
  A city you pass is given on the next pass.
- **The three stages:**
  1. the songs;
  2. the courses and island songs;
  3. the characters, ending with the HELL the 8 course.
- **No calendar:** the game never looks at the date, so play at your own pace.
- **Progress** is saved with the game. The Unlocks dialog (or `tools/unlock.py popn8`) shows the
  stage, KRAFT's space and lap, the next city, and the cities passed without landing.
- **Skipping it:** the phase codes were the operators' way to skip the event, and phase 3 or
  MAX releases everything at once.

**pop'n 6 unlocks:** right-click pop'n music 6 > **Unlocks**. Each of the operator's five
switches is a checkbox:
- SECRET MUSIC RELEASE;
- SECRET COURSE RELEASE;
- ALL DATA RELEASE (EX charts, RGB mode);
- STEP UP CHALLENGE and STEP UP CHALLENGE 2.

**Apply** writes what the game writes when they are typed in test mode and switched on
(`tools/unlock.py popn6 operator=all` for all of them). With SECRET MUSIC on, the music select
has the SECRET songs.

**pop'n 7 unlocks:** right-click pop'n music 7 > **Unlocks**:
- **How pop'n 7 differs:** its operator codes don't unlock anything by themselves. Each code adds
  a switch to EVENT OPTIONS, and the switch must be YES. The dialog's levels do both, in the
  operator's order.
- **The levels:** 1 runs the BINGO de 7!! event, 2 adds the secret songs, 3 the bingo songs,
  and MAX the expert-course songs.
- **BINGO de 7!!:** a 5×5 card.
  - After every stage, each square whose condition the stage met opens. The screen shows one
    hint each credit.
  - Each full line (12 of them) gets an item.
  - The dialog lists the closed squares with the game's own name for each condition (for
    example `COMBO 573 CLEAR`), and which lines are one square from done.
  - Like pop'n 8's event, it never looks at the date.
- **The checksum:** the save's checksum is kept right, and the old save stays beside it.

**pop'n 4 and 5 codes:** Ctrl+1 to Ctrl+8 on the title screen, with credits in, then START as
usual. Several can be entered for one credit, about two seconds each.
- **On one title screen:** with credits in, the title still cycles through the KONAMI logo about
  every 25 s, and it forgets the codes when it comes back. Type them and press START before that.
- **pop'n 4:** the hidden songs BRITPOP, HORROR and GROOVE ROCK; the POPS, J-TEKNO, DANCE and
  BNSTRK remixes; the special ending track.
- **pop'n 5:** the hidden songs LESSON and スペシャルエンディング (a service track); ENKA, NEO
  ACO, GIRLY and POSITIVE REMIX; the hidden characters SANAE-chan and Ash.
- Both games' lists hold a special-ending code that RemyWiki doesn't have: `2179223718`.

**Animelo 2 codes:** Ctrl+1 (DOUBLE EXCITE) or Ctrl+2 (TRIPLE EXCITE) on the title screen with
credits in, then START and choose EXCITE. NON STOP mode isn't there: this version (JAC) still
checks its code but never turns it on.

## ParaParaParadise (in progress, M8)

You need the program CD (`a11jaa01`), the audio CD (`a11jaa02`) and the MAME `ppp1mp` ROM set, which holds the dongle image `gqa11-ja`. To unpack MAME's CHDs:

```bash
chdman extractcd -i a11jaa01.chd -o a11jaa01.cue -ob a11jaa01.bin
chdman extractcd -i a11jaa02.chd -o a11jaa02.cue -ob a11jaa02.bin
tools/ingest.py a11jaa01.cue --cd2 a11jaa02.cue --romset ppp1mp.zip
analyze/ghidra.sh ppp1mp && analyze/prep.py ppp1mp && ninja -C build/cmake hiraku-ppp1mp
```

**First boot, as on a new cabinet:** the STARTUP PROGRAM reports a blank backup RAM (E160) and blank sound flash (E124). Press **F2** for test mode, then:
1. run **SOUND FLASH INIT**, which copies the YMZ280B samples from the CD;
2. run **ALL FACTORY SETTINGS**.

With `--window` both are saved to `~/.local/share/hiraku/ppp1mp`.

**Status:** with the ROM set, 1st Mix Plus plays: coin, song select, a full song with its CD music. ParaParaParadise (`ppp`), v1.1 (`ppp11`) and ParaParaDancing (`ppd`) ingest the same way with their own discs and sets, need the same first-boot setup (E158/E124 until then), and play: mode and music select, a stage danced with the sensor bars, the result. The sensor bars read as MAME's.

| Key | Action |
|---|---|
| Z X C V B | sensor bars 1–5 (left to right) |
| Left / Right | the two buttons (test menu: up / down) |
| 1 | start / OK |
| 5 | coin |
| F2 / 9 | test / service |

## Keyboardmania 1st/2nd/3rd, Keyboardheaven (in progress, M10)

Each game needs its two discs (the program CD and a second disc with CD audio) and its MAME ROM set (`kbm`, `kbh`, `kbm2nd`, `kbm3rd`):

```bash
tools/ingest.py gq974-ja-c01.cue --cd2 gq974-ja-a02.cue --romset kbm.7z
analyze/ghidra.sh kbm && analyze/prep.py kbm && ninja -C build/cmake hiraku-kbm
```

**Wheels:** each keyboard's modulation wheel is `P1 Wheel` / `P2 Wheel` in Controls: an analog axis, or **Wheel up** and **Wheel down** on keys or buttons (by default P and M for player 1, the keypad's + and - for player 2).

**Screens:** both portrait monitors are shown side by side (MAME's ROT270), at their shape: 3:2 together.
With two displays, **Settings > Display > each player's monitor in its own window** (or `--split-screens`) opens player 2's in a second window on the next display; with fullscreen, each fills its own display. F11 toggles the window with the keyboard. The sound, the inputs and the update banner stay with player 1's window. On Wayland the compositor places windows itself: drag player 2's to its display, then F11.

**First boot:** the backup data is blank (E161) and, on some versions, the sound flash too. Press **F2** for test mode:
- **1 / 2** move the cursor;
- **1 + 2 together** enter an item;
- **BACKUP CLEAR**, then **1** to confirm, sets the defaults;
- **SOUND TEST → SOUND FLASH INIT**, then **1**, copies the sound effects and menu sounds from the CD into the sound flash. Without it those sounds are missing, and the YMZ280B plays erased flash instead: a level stuck near full scale under the rest, which made everything else quiet and clipped. The game doesn't say so; HIRAKU does, in red at the bottom of the window for 15 seconds, and the launcher keeps showing the first-boot steps until the flash is written. The sound to the speakers also passes a DC blocker (5 Hz, as a cabinet amplifier's coupling capacitors), so a stuck level can't push the rest into clipping.

2nd and 3rd Mix have a different menu with the same two steps:
- **2nd Mix:** SOUND CHECK → INITIALIZE SOUND FLASH, and ALL FACTORY SETTINGS.
- **3rd Mix:** SOUND OPTIONS → INITIALIZE SOUND FLASH, and ALL FACTORY SETTINGS.

Each asks three times. Answer "YES?" and "ARE YOU SURE?" with **1** (move to YES) then **1 + 2**, and "PRESS 1P&2P" with **1 + 2**.

| Key | Action |
|---|---|
| Q W E R T Y U I O A S D F G H J K L Z X C V B N | player 1's 24 keys (C1–B2) |
| Keypad 7 8 9 4 5 6 1 2 3 0 . Enter | player 2, C1–B1 |
| Insert Home PageUp Delete End PageDown, Left Down Right Up, Keypad / * | player 2, C2–B2 |
| 1 / 2 | 1P / 2P start |
| 5 | coin |
| F2 / 9 | test / service |

**Status:** all four boot to attract mode, fully native, with CD and YMZ280B sound.

**Volume:** Keyboardmania's mix is as loud as MAME's, about 9 dB under DDR's. **Settings > Audio > Volume** (or `--volume DB`) raises it for every game: 6 dB doubles it. It changes only what the speakers get, not `--wav` or the digests.

**Keyboard notes:** they come from the XT446 synth card, an embedded Yamaha MU100B. HIRAKU runs the card itself: the MU100 firmware on MAME's H8S/2655 and SWP30 emulation (`runtime/dev/xt446.cpp`, `third_party/mame`). It needs the card's ROMs from the MAME set in `dumps/<game>/rom/`, which `tools/ingest.py --romset` copies: `xu50710-m27c160.bin` and the six wave ROMs `sx518b0.ic34` … `xt463a0.ic38` (in a split or merged set they are in `xt446.zip`: keep it beside the game's set). Sets newer than MAME 0.289 also hold the SWP30's `sin-table.bin`; with an older set HIRAKU rebuilds that table itself, byte for byte. It costs about a third of a CPU core.

- **No card ROMs, or `--synth-stand-in`:** the MIDI plays through FluidSynth with a General MIDI SoundFont instead. The notes and timing are the game's own, but the instruments are the SoundFont's.
  - Install a SoundFont with `sudo pacman -S soundfont-fluid`, or point at any `.sf2` with `--soundfont FILE` or `HK_SOUNDFONT`.
  - `HK_SYNTH_GAIN` sets the stand-in's level (default 0.25).
- `HK_MIDI_LOG=1` prints the MIDI stream.
- `build/cmake/hiraku-xt446 ROMDIR OUT.wav --midi LOG` runs the card on its own. LOG has one `<seconds> <hex byte>` per line.
- The card is MAME's emulation from a later master commit than the 0.289 oracle, for the SWP30's accuracy fixes (third_party/mame/README.md).

**Autoplay (Keyboardmania 1st Mix):** the game has a per-player autoplay flag, the one its attract demo uses. `oracle/scripts/kbm-autoplay.his` inserts a credit, starts player 1 and sets the flag (the script's `poke` action): the game then plays every note itself, STAGE CLEAR with 135 GREAT of 135. Run it from a set-up save: `hiraku-kbm --nvram DIR --script oracle/scripts/kbm-autoplay.his`. Useful for watching a whole song, and for checking the XT446 against MAME over one.

**Unlocks:** right-click the game > **Unlocks**.
- **Keyboardmania 3rd:** the startup code (PRESSURE mode and three hidden songs).
- **Keyboardmania 2nd:** PRESSURE mode, normally unlocked by a long wheel hold on the 2P side.
- **Playing PRESSURE:** once it's unlocked, hold the wheel down on SELECT PLAY MODE until
  PRESSURE appears below FREE.

## DJ Main: beatmania, pop'n music 1 (in progress, M13)

DJ Main games aren't recompiled: the board's 68EC020 runs at 16 MHz and is interpreted (Musashi), which costs about 1 ms of host time per frame. Each game needs its MAME ROM set (program, sprite and tile ROMs) and its hard disk as a raw image:

```bash
chdman extracthd -i 753jaa11.chd -o 753jaa11.img     # once; chdman comes with MAME
tools/ingest.py --romset bm1stmix.7z --hdd 753jaa11.img
ninja -C build/cmake hiraku-bm1stmix
build/cmake/hiraku-bm1stmix --window
```

pop'n music 1 (`popn1j`) is the same with `803jaa11` and `popn1j.7z`. The game is found by the set's name (`games/<id>/game.toml`, `core = "djmain"`).

**Every MAME DJ Main set has a `games/<set>/game.toml`** (29, generated from MAME's driver), so any of them is ingested the same way:
- beatmania 1st mix → THE FINAL with the hiphopmania and beatstage versions;
- pop'n music 1–3 and pop'n stage EX.

The disk each set needs is its `[hdd] chd`; clones may share their parent's disk. Things to know:
- **complete MIX and later lock their hard disk with an ATA password.** HIRAKU unlocks it as MAME does: the passwords from MAME's driver are in the `game.toml`, and the game sends them itself.
- **The UA/KA/AA versions and pop'n music 1 TA/AA/KA use other address maps** (`map = "u"` / `"a"`).
- **pop'n stage EX** has ten buttons and three selection buttons (left, OK, right).
- **DIP switches:** the board's three banks of switches, which the operator sets: the coinage
  (Free Play among them), difficulty and gauge levels, demo sounds, and some games' hidden modes
  (pop'n music 1's All Song Mode, RAVE and Bonus Track; 2 and 3's All Song Mode). In the launcher:
  **Game > DIP switches…** (or right-click the game), each switch named as MAME names it; the game
  reads them when it starts. On the command line: `--dip DSW1=0x00` (a whole bank as MAME's port
  reads it, active low; 0xff, every switch off, is MAME's default for every DJ Main game). The
  switches come from MAME's `-listxml` (`games/<set>/dips.txt`, written by `tools/mamedips.py`).
- **Run so far:** bm1stmix, bm2ndmix, bm3rdmix (and EA), 4th–7th MIX, complete MIX (AA), complete MIX 2, CORE REMIX, DCT, THE FINAL, pop'n 1–3. All reach attract mode. 1st–3rd MIX and pop'n 1 match MAME clock for clock; the later ones drift from MAME after about 75 s (PLAN M13).

**Status:** HIRAKU runs both games in step with MAME, clock for clock (`oracle/scripts/*-boot.his`, 155 s):
- power-on, self-test, the disk loads and attract mode;
- a whole game of each:
  - beatmania (`oracle/scripts/bm1stmix-play.his`, 217 s): coins, the turntable at the practice-stage prompt, song select, a song played with keys and scratches, game over;
  - pop'n music (`oracle/scripts/popn1j-play.his`, 233 s): coins, game mode and genre select, a song played over all 9 buttons, game over.

In these runs:
- the work RAM, sprites and palette are identical to MAME's on every frame;
- every snapshot is pixel-identical;
- the sound is sample-identical: HIRAKU plays and records it at the chips' own 48 kHz (`--wav` matches MAME's `-wavwrite`).

**Not yet:** a hands-on play test, turntable feel, and lockstep with MAME for 4th MIX and later.

| Key | beatmania | pop'n music |
|---|---|---|
| Z S X D C | player 1 keys 1–5 | buttons 1–5 (Z S X D C F V G B: 1–9) |
| Left / Right, Left Shift / A | turntable −/+ | — |
| M K , L . | player 2 keys | — |
| 1 / 2 | 1P / 2P start | — |
| E | effect | — |
| 5 | coin | coin |
| F2 / 9 | test / service | test / service |

A controller's analog axis drives the turntable (`P1 Turntable = joy0:axis0`). MAME models the turntable as an 8-bit dial whose steps the board counts 70 at a time; HIRAKU does the same for now, so scripted runs match MAME.

**Checking against MAME:**

```bash
HIRAKU_ROMS=~/bm3mame/roms-cores HIRAKU_TAPS=none oracle/mame.sh bm1stmix oracle/scripts/bm1stmix-boot.his boot
build/cmake/hiraku-bm1stmix --script oracle/scripts/bm1stmix-boot.his --out out/run/bm1stmix-boot
oracle/snapcmp.py out/run/bm1stmix-boot out/oracle/bm1stmix/boot
```

The same with `bm1stmix-play.his` or `popn1j-play.his` checks a game played through.

## System 573: Dance Dance Revolution 2ndMIX (in progress, M14)

DDR 2ndMIX (`ddr2m`) runs on HIRAKU's System 573 core: MAME's PS1 devices with the CPU interpreted, about 2–3.5 ms of host time a frame. It needs MAME's `ddr2m` set (BIOS, the cassette's X76F041) and the CD `895jaa02`:

```bash
tools/ingest.py ddr2m.7z 895jaa02.chd
ninja -C build/cmake hiraku-ddr2m
build/cmake/hiraku-ddr2m --window
```

**Faster with profile-guided optimisation:** `tools/pgo.py` (see "Once: build and ingest your dumps") makes the 573 games need ~27 % less CPU (ddr2m: 1.76 ms a frame instead of 2.41 on the desktop).

**The first boot installs the game to the board's flash:**
1. The game asks "DO YOU WANT TO INITIALIZE FLASH-ROM?": press Test (F2).
2. Erasing and writing take about 2.5 minutes.
3. "FLASH-ROM INITIALIZE COMPLETE": press Test again.

Later boots check the flash (about 80 s, as the real board does) and go to attract mode. The flash lives in the game's NVRAM folder like any other save.

**Coins:** two coins make a credit.

| Key | Dance Dance Revolution |
|---|---|
| Arrows | player 1's panels (a dance pad's buttons 0–3: left, down, up, right) |
| Keypad 4 2 8 6 | player 2's panels |
| 1 / 2 | 1P / 2P start |
| 5 | coin |
| F2 / 9 | test / service |

A dance pad is bound in the launcher's Controls (Dance Dance Revolution page): click a panel, then step on it. Pads that send their arrows as buttons, a hat or axes all work. The panel buttons 1–3 are unbound by default.

**Lamps:** the analog I/O board's outputs as MAME names them (foot panels, start lamps, the four body lamps, the speaker neon), through the lights socket and HID lamps (docs/LIGHTS.md).

**Sound:** the SPU and the CD's music in MAME's balance, the CD at the volume the game sets with MODE SELECT.
- **Played at half level, which never clips.** MAME sums the two at full level and clips the sum, and in DDR they reach twice full scale. That crackles on loud songs: about 1 % of all samples in a game, and a third of them on the loudest. The real board mixes them in analog.
- `--mame-mix` gives MAME's level and clipping back, for comparing with MAME's recordings (`oracle/audiosync.py`).
- The exit report's `sound:` line gives the mix's peak and how much of it clipped.

**Checked against MAME, clock for clock** (`oracle/scripts/ddr2m-boot.his`, `ddr2m-play.his`): the first boot and flash install, attract mode, and a game. That game is coins, mode and song select, and a song stepped through to CLEARED. Main RAM is identical on every frame, every snapshot is pixel-identical, and the sound matches MAME's recording (with `--mame-mix`). Both are regress cases.

**Not yet:** a hands-on test with a pad.

## System 573: Dance Dance Revolution 3rdMIX (in progress, M14)

DDR 3rdMIX (`ddr3mj`) is the first game on the 573's **digital I/O board**: the music is MP3, decrypted by the board's FPGA and decoded by its MAS3507D. It needs MAME's `ddr3mj` set and the CD `887jaa02`. The set carries the BIOS, both cassettes (the game's and the install one, each an X76F100 plus a DS2401) and the board's DS2401.

```bash
tools/ingest.py ddr3mj.7z 887jaa02.chd
ninja -C build/cmake hiraku-ddr3mj
```

**The first boot needs the install cassette**, as on a real cabinet (MAME's note 3 in `ksys573.cpp`):
1. **Play** in the launcher. With no saves yet, it puts the install cassette in the slot (the details pane says so). The game installs itself with no input: the CD to the onboard flash, then to the 32 MiB PC Card. That takes about 4½ minutes of game time (hold **Tab** to fast-forward).
2. At **INFO 506** ("install complete, put the GN887 cassette back and power-cycle"), stop it.
3. **Play** again: it boots with the game's cassette, checks the flash and goes to attract mode.

If an install was stopped before INFO 506, the game shows **ERROR 147**: right-click it, **Boot once with the install cassette**, and Play. From the command line, the install cassette is `--cassette install`:

```bash
build/cmake/hiraku-ddr3mj --cassette install --window
```

Without the install, the game stops at **ERROR 147**. The flash and the PC Card live in the game's NVRAM folder like any other save.

Controls, coins and lamps are DDR's, as for 2ndMIX above. The lamps come from the digital board's outputs.

**Sound:** the SPU plus the MP3, in MAME's balance and played at half level (`--mame-mix` for MAME's level).

**Checked against MAME, clock for clock** (`oracle/scripts/ddr3mj-install.his`, `ddr3mj-attract.his`, `ddr3mj-play.his`): the install, then from what it left, attract mode and a game.
- The game: coins, Start, the selects timed out to XANADU, the song stepped through to its result, then stage 2.
- Main RAM is identical on every frame, and the NVRAM after the install is byte for byte MAME's.
- The sound matches MAME's recording: lag 0, correlation ≥ 0.999.
- Each is a regress case: `ddr3mj-attract` and `ddr3mj-play` start from the NVRAM `ddr3mj-install` leaves.

**Speed** (desktop, recompiled R3000 code): attract 0.48 ms a frame, a song 0.62 ms.

**Not yet:** a hands-on test. MAME flags this set NOT_WORKING without saying why; so far it plays as MAME does.

## System 573: DrumMania 10th Mix (M14, further than MAME)

DrumMania 10th Mix (`drmn10m`) needs MAME's `drmn10m` set, the runtime CD `d40jaa02` and the multi-session unit's CD `d40jba02`:

```bash
tools/ingest.py drmn10m.7z d40jaa02.chd d40jba02.chd
ninja -C build/cmake hiraku-drmn10m
```

**The first boots:**
1. The first boot installs the CD to the onboard flash by itself and shows **INFO 508** (power-cycle): stop it.
2. The next boot shows **NOTE 605** (RTC-RAM cleared): press **Test** (F2).
3. The boot checks load the game's data, then wait for the **multi-session unit**, which boots from its own CD meanwhile: about two and a half minutes from power-on.
4. Then the warning screen, the title and attract. Two coins (5) make a credit.

The unit is a box of its own: a second CPU (a TX3927), a CD drive, four MP3 decoders and four UARTs. The game won't start without it. MAME's is an empty stub, so MAME stops at **"Please call attendant"** (likely why it flags the set NOT_WORKING). HIRAKU runs windyfairy's emulation of it (third_party/mame/README.md). In a one-cabinet game the music comes from the digital board's MP3, as on DDR. The unit's decoders are for session play with GuitarFreaks (below).

**Speed:** about 2.8 ms a frame on the desktop, the unit and the 573 together (p99 6.5 ms). The unit's CPU is interpreted and runs at 25 MHz, as in the fork. Its timers are computed on demand and its RTOS's idle loop is skipped. The real chip's 132 MHz (`HK_MSU_132=1`) changes nothing in the game and costs about 7 ms: the firmware's decoder task polls through the RTOS as fast as the CPU goes.

| Key | DrumMania |
|---|---|
| S / D / J / K / L | hi-hat, snare, high tom, low tom, cymbal |
| Space | bass pedal |
| Left / Right | select |
| 1 or Return | start |
| 5 | coin |
| F2 / 9 | test / service |

**Checked against MAME, clock for clock** (`oracle/scripts/drmn10m-install.his`, `drmn10m-boot.his`):
- the install: main RAM is identical on every frame, and the NVRAM after it is byte for byte MAME's;
- the boot, until the game first uses the link to the unit (28 s in): main RAM is identical on every frame.

From there on the unit answers, which MAME's can't.

The regress cases are `drmn10m-install`, `drmn10m-boot` (now on to attract), `drmn10m-play` (a game: the first stage of 花火), `drmn10m-hdd` (the network unit's whole install), `drmn10m-npu` (a boot with the unit and its disk, to attract), `drmn10m-eaon` (e-AMUSEMENT turned ON), `drmn10m-net` (a boot with it ON, then NETWORK CHECK), `drmn10m-card` (a new card registered, a game played and saved to it) and `drmn10m-card2` (the card again, its password).

**The network PCB unit (e-AMUSEMENT), started:** the e-AMUSEMENT cabinet's network unit, with its hard disk as the save folder's `npu_hdd/` folder (runtime/sys573/npu.h). Ingest the unit's installer CD as well:

```bash
tools/ingest.py drmn10m.7z d40jaa02.chd d40jba02.chd d40jca02.chd
```

1. **Install its data:** `hiraku-drmn10m --cd hdd`. The game formats the disk and installs about 400 MB, about 20 minutes of game time, until **INSTALLATION COMPLETED**. Then quit.
2. **Play with it:** `hiraku-drmn10m --npu`. The boot reads the disk's songs.
3. **e-AMUSEMENT** is OFF by default (test mode > NETWORK OPTIONS > e-AMUSEMENT OPTIONS; hold Start and press Left/Right to change it, then SAVE AND EXIT). With it ON the game connects to HIRAKU's e-AMUSEMENT server: test mode's NETWORK OPTIONS > NETWORK CHECK says **CONNECTED**.
4. **The card:** Insert puts a card in the slot (or takes it back); the numeric keypad is the 10-key (Backspace is Clear). With e-AMUSEMENT ON, coins and Start open ENTRY CARD. A new card is numbered by the server; you then enter a name (Left/Right and Start; END finishes) and a 4-digit password twice. Next time, insert the same card and type its password. At the end of a game the card is written (EJECT CARD) and given back. It is the save folder's `card1.bin` (128 bytes). The server keeps the cards in `eamuse/gfdm-cards` and each player's results in `eamuse/gfdm-<game>-<number>`: the best percentage, skill, score and combo per song and difficulty. The skill point is the best 25 songs' sum, and attract's HIT CHART [全国版] lists the songs played most. The puzzle isn't kept.

**The e-AMUSEMENT server:**

| | How | What for |
|---|---|---|
| **Offline** (default) | nothing to do | one cabinet; the server runs in the core, its data in the save folder's `eamuse/`; runs are reproducible |
| **Online, HIRAKU's server** | on the server machine, `hiraku-eamuse --port 8080 --data DIR`; on each cabinet, `hiraku-drmn10m --npu --eamuse http://server:8080/` | several cabinets (a session's two, say) sharing one server |

- **The cabinet's network is virtual:** it sees a LAN (192.168.50.2, its router and server 192.168.50.1) and HIRAKU answers its DNS, pings and NTP itself. Online, only the game's HTTP calls go out, unchanged (encrypted as the game encrypts them).
- **`hiraku-eamuse` options:** `--bind ADDR` (default 0.0.0.0), `--clock "2003-06-01 12:00:00"` (the time it gives, running from there; default the host's clock), `--tz 9` (with the host's clock, the cabinets' time zone), `--quiet`, `--no-relay` (no pop'n NET TAISEN lobby and relay on port 5740).
- **`HK_573_NET_LOG=FILE`** logs every socket call, and every call and answer in clear.

With the unit fitted the 573 has more to do, and the multi-session unit's link once lost bytes ("Please call attendant") until SIO1 got the PS1's 8-byte receive FIFO. It now boots to attract, and sessions with GuitarFreaks work with it fitted. Without `--npu` the game is as before. `HK_573_NPU_LOG=1` prints the unit's calls (2: their words, 3: every register access).

**Not yet:** a hands-on test.

## System 573: GuitarFreaks 11th Mix, and session play with DrumMania (further than MAME)

GuitarFreaks 11th Mix (`gtfrk11m`) needs MAME's `gtfrk11m` set and its runtime CD `d39jaa02` (`d39jba02` is its network unit's installer, for `--cd hdd`):

```bash
tools/ingest.py gtfrk11m.7z d39jaa02.chd d39jba02.chd
ninja -C build/cmake hiraku-gtfrk11m
```

**The first boots:**
1. The first boot installs the CD to the onboard flash and the flash card by itself (about 7 minutes of game time), then shows **INFO 508**: stop it.
2. The next boot shows **NOTE 605**: press **Test** (F2).
3. Then attract. Two coins (5) make a credit.

| Key | Guitar 1 | Guitar 2 |
|---|---|---|
| Neck R / G / B | A / S / D | J / K / L |
| Pick | Space | Right Shift |
| Wailing | W | I |
| Start | 1 | 2 |

The effect knob is an analog axis (a joystick's first axis). Coin is 5; Test and Service are F2 and 9.

**The network unit (e-AMUSEMENT)** works as DrumMania's (above): `hiraku-gtfrk11m --cd hdd` installs its data (about 15 minutes of game time, to INSTALLATION COMPLETED), then `--npu` fits it, and `--eamuse` picks the server. In its test mode, P1 R/B moves and **P2 R/B changes a value**. Each guitar has a card unit: Insert and the numeric keypad for the first, "P2 Card" (Delete) for the second, whose 10-key you bind yourself. Both cabinets of a session can share one `hiraku-eamuse`.

**Session play.** DrumMania 10th Mix and GuitarFreaks 11th Mix play together through DrumMania's multi-session unit, each in its own process, joined by a session cable:

```bash
hiraku-drmn10m --session-host /tmp/gfdm.sock
```

```bash
hiraku-gtfrk11m --session-join /tmp/gfdm.sock
```

- **Start the DrumMania side first;** it waits for the other. A host:port works too (TCP), for two machines (not tried yet).
- **The two run in lockstep,** at the slower one's pace: about 4.1 ms a frame together on the desktop.
- **To play a session:** credits and Start on one cabinet open the **WELCOME** screen, with 15 s for the other to join (credits and Start there). Then both choose the mode and the music together and play the same song.
- **Check the cable** in test mode: SESSION NETWORK CHECK says PORT 1 (DrumMania) and PORT 2 (GuitarFreaks), OK.
- **The session music** comes from the unit, in the DrumMania process. It goes over the cable to the GuitarFreaks side too, so both windows play it, in time with their own sounds.

The regress cases are `gtfrk11m-install`, `gtfrk11m-boot`, `gtfrk11m-hdd`, `gtfrk11m-eaon` and `gtfrk11m-net` (the network unit, as drmn10m's), and `drmn10m-session` + `gtfrk11m-session` (a session game: both cabinets play the first stage of 花火).

## System 573: Dance Maniax and Mambo a GoGo

Dance Maniax (`dmx`; `dncfrks`, Dance Freaks, is its Korean version), Dance Maniax 2nd Mix (`dmx2m`), 2nd Mix Append J-Paradise (`dmx2majp`) and Mambo a GoGo (`mamboagg`; `mamboagga` is the rental version) are on the digital board, with a ZS01 security cassette. Each needs MAME's set and its runtime CD:

```bash
tools/ingest.py --keep-chd dmx.zip 874jaa.chd
tools/ingest.py --keep-chd mamboagg.zip a40jab02.chd
ninja -C build/cmake hiraku-dmx hiraku-mamboagg
```

The cassette dumps must be MAME's current ones (MAME 0.250 and later: 140-byte `.u1` files). With an older MAME set's 4,116-byte files the games stop at **ERROR 121** (cassette error 9), in MAME too.

**The first boots:**
1. The first boot installs the CD to the onboard flash by itself and shows **INFO 508** (power-cycle) after about two and a half minutes of game time: stop it. Dance Maniax 2nd Mix and J-Paradise first show **NOTE 604** (the CD-ROM drive's update): press **Test** (F2).
2. The next boot: Dance Maniax sets itself up (RTC, then the loading) and plays attract; Mambo a GoGo shows **NOTE 605** (RTC-RAM cleared): press **Test**.
3. Two coins (5) make a credit; Dance Freaks takes three.

**Dance Maniax** has four hand sensors per player: two upper ones above the head and two lower ones at the waist, left and right. You play by moving a hand through a sensor's beam. Each lower sensor is two beams (MAME's D0 and D1); a lower sensor here breaks both. The beams have their own names as well ("P1 D-Sensor D0 L"), with no keys, for a controller that reads them apart.

| Key | Player 1 | Player 2 |
|---|---|---|
| Upper left / lower left | W / S | Keypad 7 / 4 |
| Lower right / upper right | L / O | Keypad 6 / 9 |
| Select left / right | Left / Right | Keypad 1 / 3 |
| Start | 1 or Return | 2 or Keypad Enter |

So player 1 plays with the left hand on W and S and the right hand on O and L. On a controller (`joy0` for player 1, `joy1` for player 2) the sensors are buttons 0–3 (upper left, lower left, lower right, upper right), select 4 and 5, start 9.

**Mambo a GoGo** has three congas, left, centre and right. Each has three pads: pad 1 is the far half of the head, pads 2 and 3 the near left and near right quarters. Each pad is two sensors (MAME gives them one button); a pad here presses both, and each sensor has its own name too. The panel has Select left and right (Left and Right; buttons 11 and 12) and Start: the music select takes Select and Start, test mode Select right and Start.

| Conga | Pad 1 (far) | Pad 2 (near left) | Pad 3 (near right) | Controller |
|---|---|---|---|---|
| Left | W | A | S | buttons 0, 1, 2 |
| Centre | Y | G | H | buttons 3, 4, 5 |
| Right | O | K | L | buttons 6, 7, 8 |

On the keyboard each conga is a triangle, the far pad's key above the near pads' two. Start is 1 or Return (button 9); coin, test and service are 5, F2 and 9 on both games. The launcher's **Controls** dialog has both cabinets, with these defaults. Three Taiko-style drums can stand in for the congas (`docs/CATALOGUE.md`, Phase 5: the hardware options).

**The rental version** (`mamboagga`) needs Konami's e-Amusement rental unit and its rental service. MAME has neither, and stops at ERROR 198 (Start goes on), then after the install at HARD-WARE ERROR +02H, RTCRAM INITIALIZE ERROR. HIRAKU's `--rental` (the unit's presence) and one boot with `--master-calendar 7` (Konami's factory device, which formats the rental records) take it as far as a real rental cabinet with no plays bought: "remaining plays used up, please replenish". Buying plays (Konami's encrypted per-cabinet codes) isn't emulated. Play `mamboagg`: the same game.

**Lamps:** Dance Maniax's sensor lamps (blue and red), spotlights and start lamps; Mambo a GoGo's conga lamps, fire lamps and fans, start and select lamps, as MAME names them (`--lights`).

## Twinkle: beatmania IIDX (in progress, M15)

beatmania IIDX 3rd style (`bmiidx3`) runs on HIRAKU's Twinkle core in lockstep with MAME (PLAN.md section 21). It needs MAME's `bmiidx3` set and its three CHDs: the program CD `gc992-jac01`, the Video CD `gc992-jaa04` and the sound board's hard disk `992hdda01` (unpacked to about 4 GB):

```bash
tools/ingest.py bmiidx3.7z gc992-jac01.chd gc992-jaa04.chd 992hdda01.chd
ninja -C build/cmake hiraku-bmiidx3
```

**The first boot:** the device check passes and stops at **E150** (backup RAM cleared): press **1P Start** (1) to clear it. Then attract; insert coins (5) and press Start.

| Key | Player 1 | Player 2 |
|---|---|---|
| Keys 1–7 | Z S X D C F V | M K , L . ; / |
| Turntable | ← / → (and Left Shift / A) | ↓ / ↑ |
| Start | 1 | 2 |

Effect is E and VEFX is W. Coin is 5; Test and Service are F2 and 9. A USB IIDX controller has its keys on buttons 0–6 and its turntable on the first axis by default; Start, Effect and VEFX are buttons 8, 9 and 10. The effector's five sliders (Volume 1–5) are analog axes with no default binding. Controls are remapped in the launcher.

**Lamps:** the key lamps, Start, VEFX, Effect and credit lamps, eight spotlights, the neon and the board's LEDs, by name for HID LEDs and the lights socket (docs/LIGHTS.md). The 16-segment display above the screen shows in the window title.

**The background video** plays in the middle of the play field during a stage, from the Video CD (`gc992-jaa04`), as the game drives the DVD player; MAME shows none. `--video off` gives MAME's picture. The video needs FFmpeg's libraries (libavformat, libavcodec) when HIRAKU is built; without them the build goes ahead without it. Which clip goes with which song, and the exact keying and placement, still need checking against a real cabinet (PLAN.md section 23, TW-3).

**Speed** (desktop, with the game's R3000 code recompiled: `tools/mipsrec.py bmiidx3`, then `ninja`): a stage 1.5 ms a frame with the video, 1.3 without. **The program CD** reads at the speed of the cabinet's drive, a quad-speed Toshiba XM-5401: a sector at a time, as the drive reads it. Loads between screens take about as long as on a cabinet (2–3 s), at about 4.7 ms a frame. MAME's drive has no read time: it loads about five times faster, but each loading frame costs about 17 ms of host work. `--mame-cd` gives MAME's drive, for comparing with MAME.

The regress cases are `bmiidx3-boot` (the first boot to attract), `bmiidx3-play` (a coin-up and a stage, after it) and `bmiidx3-video` (the same with the video), all with MAME's drive, and `bmiidx3-play-drive` (the game on the cabinet's drive).

### beatmania IIDX (1st style)

The first beatmania IIDX (`bmiidx`, GQ863 JAB) plays too, which MAME's doesn't: there it stops at **E600 DVD INIT ERROR** (MAME flags it NOT_WORKING). The game talks to its DVD player through the PlayStation's own serial port, and waits for the player's CTS line there, which MAME's serial port lacks; HIRAKU's has it (PLAN.md section 21, TW-5). Ingest it like 3rd style:

```bash
tools/ingest.py bmiidx.7z gq863-jab01.chd gq863a04.chd 863hdda01.chd
ninja -C build/cmake hiraku-bmiidx
```

**The first boot** stops at **E150** (backup RAM cleared) with no Start prompt; this style clears it in test mode:
1. Press **Test** (F2).
2. Move down to **BACKUP CLEAR** with key 1 (key 2 moves up) and press key 6; answer yes with key 2.
3. Key 7 leaves test mode (yes: key 2). The game restarts and goes to attract.

Test mode's **DVD CHECK** plays and stops the Video CD. A credit is **3 coins** by default (game settings). At mode select the turntable moves between 4KEYS, 5KEYS, 7KEYS and EXPERT; key 1 takes it. During a stage the game draws its own graffiti layer over the video.

**Speed** (desktop, recompiled R3000 code): a stage 1.8 ms a frame with the video.

Its regress cases: `bmiidx-clear` (the first boot, the DVD check and backup clear in test mode, the restart, attract), `bmiidx-play` (a game of 7KEYS after it) and `bmiidx-video` (the same with the video). They are checked against a MAME given the same serial port (oracle/mame-patches).

### beatmania IIDX Substream, 2nd to 8th style

These play too, ingested the same way: the MAME set and its three CHDs (PLAN.md section 21, TW-5).

| Game | Set | Program CD | Player's disc | Hard disk |
|---|---|---|---|---|
| Substream | `bmiidxs` | `gc983a01` | Video CD `gc983a04` | `983hdda01` |
| 2nd style | `bmiidx2` | `gc985a01` | Video CD `gc985a04` | `985hdda01` |
| 4th style | `bmiidx4` | `a03jaa01` | Video CD `a03jaa02` | `a03jaa03` |
| 5th style | `bmiidx5` | `a17jaa01` | Video CD `a17jaa02` | `a17jaa03` |
| 6th style | `bmiidx6` | `b4ujab01` | DVD `b4ujaa02` | `b4ujaa03` |
| 7th style | `bmiidx7a` | `b44jaa01` | DVD `b44jaa02` | `b44jaa03` |
| 8th style | `bmiidx8` | `c44jaa01` | DVD `c44jaa02` | `c44jaa03` |

```bash
tools/ingest.py bmiidx6.7z b4ujab01.chd b4ujaa02.chd b4ujaa03.chd
ninja -C build/cmake hiraku-bmiidx6
```

**Disk space:** the DVD styles keep about 10 GB each (a 4.5 GB DVD and the hard disk, whose zeros are stored as holes). 7th style's hard disk is 40 GB before that, so unpacking it needs that much free space at first. The images can live on another drive: unpack them there and link them into `dumps/<set>/` under the same names; the ingest keeps an image that is already there at the right size.

**The first boot** stops at **E150**, as in 3rd style, then shows **1P START = BACKUP CLEAR**: press **1P Start** (1). The prompt comes later on some styles (about 50 s in for Substream, 7th and 8th style). 7th and 8th style then run a long device check before the title.

**The background video:** from 6th style on, the player holds a DVD, and the stages play its chapters (MPEG-2, decoded with FFmpeg as the Video CD is). MAME shows none, and its player model reads the game's DVD chapter numbers as a Video CD's; the picture here follows the game's own numbers. Which clip goes with which song still needs checking against real cabinets: in 8th style the game asks for chapter 4 for 5.1.1., which is not that song's own clip.

### The club kits: beatmania IIDX × DDR 2nd Club Version (further than MAME)

A club kit turned a beatmania IIDX cabinet and a DDR cabinet into one session: a DJ on the IIDX and a dancer on the DDR play the same song. HIRAKU runs each cabinet in its own process, a Twinkle and a System 573, joined by a serial cable (MAME has no such cable on the 573; docs/CATALOGUE.md, Phase 6).

| Kit | IIDX cabinet | DDR cabinet | Cable |
|---|---|---|---|
| Club Version 1 | `bmiidxc` or `bmiidxca` (1st style board) | `ddr2mc` | IIDX: the super I/O's COM2, 19,200 baud; DDR: SIO1 |
| Club Version 2 | `bmiidxc2` (Substream board) | `ddr2mc2` | SIO1 to SIO1, 38,400 baud |

Install the DDR side once from its install CD (`--cd install`; the launcher's first boot does it), and clear the IIDX side's backup on its first boot (Club Version 1 in test mode, as the 1st style; Club Version 2 with 1P Start). Then start the IIDX side, which waits, and the DDR side:

```bash
hiraku-bmiidxc --session-host /tmp/club.sock
```

```bash
hiraku-ddr2mc --session-join /tmp/club.sock
```

- **Either side can host** (`--session-host` on one, `--session-join` on the other). A host:port works too (TCP), for two machines (not tried yet).
- **The cable shows** in attract: the IIDX title gets the club version's logo, and its How to Play the "DDRとの通信" banners.
- **Club Version 1:** credits and Start on the IIDX ("Let's play with DanceDanceRevolution / Please wait"), then credits and Start on the DDR within the joining period (10 s by default). The IIDX player chooses the mode and the song; the DDR shows "beatmania IIDX side is choosing". Both then play the IIDX's song, the DDR with its arrows.
- **Club Version 2:** the DDR player starts first and, after the play mode, answers **Yes** to "recruit a DJ from beatmania IIDX substream?" (通信プレイモード); the IIDX player then presses Start ("Here Comes A New DJ!" on the DDR) and chooses as above.
- **The music** comes from the IIDX cabinet; the DDR plays no music of its own in a club song. The IIDX process sends its sound over the cable, so the DDR window plays it too, in time.
- **The launcher** doesn't start sessions yet (nor GuitarFreaks × DrumMania): run the two from a shell.

The regress pairs `ddr2mc-club` + `bmiidxc-club` and `ddr2mc2-club` + `bmiidxc2-club` play a club stage each (after `ddr2mc-install`, `ddr2mc2-install`, `bmiidxc-clear`, `bmiidxc2-boot`).

## Konami Python 1: pop'n music 9–14 (in progress, M16)

pop'n music 9 (`popn9`) runs on HIRAKU's Python core, and so do 10 (`popn10`), 11 (`popn11`), 12 Iroha (`popn12`), 13 Carnival (`popn13`) and 14 FEVER! (`popn14`): PCSX2 Reliquary's emulator hosted by HIRAKU's shell (PLAN.md §23, docs/PYTHON.md). It is built inside Reliquary's own build. You need:
- Reliquary's source, `pcsx2-reliquary-1.9.4-reliquary.zip`, unpacked to `$HK_PYTHON_WORK/pcsx2-reliquary` (default `Python-work` beside the HIRAKU checkout);
- the set as Reliquary takes it: `popn9.py1` (or `popn10.py1` …) and its folder (the disk CHD, the interface board's ROMs, BBSRAM, dongles, the KN00002 memory card), plus the arcade BIOS (`COH-H31000.BIN`, `.NVM`, `.MEC`) and the MagicGate key files (`civ.bin`, `cks.bin`, `eks.bin`, `kek.bin`, `arcade_kelf_override_kbit.bin`, `arcade_kelf_override_kc.bin`). The ingester finds `bios/` and `keys/` next to the sets' folder, or takes `--bios DIR --keys DIR`. They can also come on their own: dropped on the launcher (the files, or their folder), they are kept in `dumps/_python/` for every Python game, and a game added without them ("needs the arcade BIOS ... drop them on HIRAKU's window") waits and is finished when they come.

```bash
oracle/pcsx2/build-reliquary.sh all     # once: Reliquary with HIRAKU's patch, and hiraku-popn9 … hiraku-popn14 (memory-capped)
tools/build-libchdr.sh                  # once: the CHD reader the ingester uses
tools/ingest.py ~/Downloads/konami-python-1-collection/kpython1/popn9.py1
build/cmake/hiraku-popn9 --window --renderer vk
```

- `--renderer vk` draws with Vulkan: less CPU. The default, `sw` (software), is what the reference runs. `tools/regress.py` checks both. On Windows, `dx11` and `dx12` draw with Direct3D. `gl` (OpenGL) doesn't work here: PCSX2's needs a window of its own.
- `--autoplay` (pop'n music 9 only, for now) makes the game play every song itself, as in its attract demo: every note GREAT, the buttons ignored during songs. It's a mod for testing what clears unlock (events, HITASURA, Gambler Z); scores and e-AMUSEMENT rankings count it like any play.
- Each frame's picture is shown at the end of that frame: 4 frames from a button to the picture, the game's own. `--picture-overlap` shows the previous frame's instead, so the GS works alongside the CPU: a little less time per frame on a slow PC, for a frame (16.7 ms) more latency.
- **The picture is progressive.** The pop'n games draw for an interlaced TV: each frame only the lines of the field the TV shows (the GS's SCANMSK), the other half staying a frame old. Shown progressive, that combs on everything that moves; PCSX2's deinterlacing guesses, and notes flicker in thickness. HIRAKU has the GS draw every line of every frame instead, as a 480p game would: the game runs the same (its sound is identical), only the pictures differ. `--interlaced` keeps the games' own drawing (the regression cases use it, to match the reference).
- **The picture has a TV's shape.** The game's 512x448 picture is the GS's display area, which on a TV sits in a wider and taller raster: 2560 of the 2844 clocks of a line, 448 of 480 lines. HIRAKU shows the 4:3 screen as that raster, the picture centred with a thin black border, so everything has the shape it has on a TV, and the CRT shaders' 448 scanlines fall on whole pixels at 1440 (3 each) or 960 lines. `--fill-screen` stretches the picture over the whole 4:3 screen instead, as PCSX2 does: about 4 % wider than on a TV.
- `oracle/pcsx2/build-reliquary.sh pgo` (once, ~5 min) rebuilds with profile-guided optimisation: about 3 % less CPU, same results.
- The CHD stays where it is (linked, not unpacked). The game's writes (BBSRAM, dongle, the disk's own writes as an overlay) go to the NVRAM folder, `~/.local/share/hiraku/popn9`.
- The keys and the BIOS are copied into `dumps/popn9/`, which is never committed. Reliquary's habit of printing the derived keys is off in HIRAKU's build.
- Boot takes about 50 s (BIOS, card, disk), as on the real cabinet.

| Key | pop'n music |
|---|---|
| Z S X D C F V G B | buttons 1–9, left to right (white, yellow, green, blue, red, blue, green, yellow, white) |
| 5 | coin (two coins make a credit) |
| F2 / 9 | test / service |

In test mode, blue (buttons 4/6) moves and red (5) selects; I/O CHECK > INPUT CHECK shows SW1–SW9 lighting for buttons 1–9.

**Lamps:** the 9 button lamps, 5 top lamps and 4 side lamps, through the lights socket and HID lamps (docs/LIGHTS.md), as the game's LAMP CHECK names them.

**pop'n 9 unlocks:** right-click pop'n music 9 > **Unlocks**, pick the event phase, and start the game.
- **MAX** adds HITASURA mode after EXPERT in the mode select, and everything the phases before it release.
- Phases 1–3 run the Gambler Z event and release its songs (RemyWiki). On the cabinet the event's labyrinth needed an e-AMUSEMENT card; what it does without one in HIRAKU is not checked yet.
- From a terminal: `tools/unlock.py popn9` lists the phases, and `tools/unlock.py popn9 phase=4` sets MAX. The game must not be running; the old save is kept as a `.bak`.
- **In the game itself:** test mode > EVENT OPTIONS > PASSWORD, the four codes in order (`unlocks.toml` has them). The tool writes the same bytes the game does.

**e-AMUSEMENT (in progress):** pop'n 9 is the first pop'n with e-AMUSEMENT cards. It is OFF by default (test mode > e-AMUSEMENT OPTIONS), and the game plays normally without it.
- **What works:** with it ON, HIRAKU plays the cabinet's card reader/writer and 10-key, and Konami's e-AMUSEMENT server (closed in 2003) inside the emulator, with no network traffic.
  - The title screen says "e-AMUSEMENT connected!" and "cards can be used!!".
  - A new card is registered as on the cabinet: after Start, put the card in (Insert), enter a name, then a 4-digit PIN twice on the 10-key. The game writes the card ("card save complete") and plays on with it.
  - The player data is saved at the end of each credit (in the save folder's `eamuse/`). Next time, the card and the PIN bring back your name, character and options; a wrong PIN plays without the card.
- **Offline or online:** by default the server runs inside HIRAKU, with the data in the save folder's `eamuse/`.
  - To share cards, players and rankings between PCs, run `hiraku-eamuse --port 8080` on one of them, and put `that-pc:8080` in the launcher's Settings > e-AMUSEMENT > EA Service URL (or `-url that-pc:8080`).
  - **As in spice2x:** the settings and options are spice2x's: `-ea`, `-url`, `-smartea`, `-p` (PCBID), `-card0` / `card0.txt` (the card's number, `E004...`) and `-autocard`. A spice2x setup's values work as they are (docs/EAMUSE3.md, "spice2x's options"). In the launcher, **Pick** next to a card offers the cards HIRAKU already has, or a new number.
  - A line of text in the server's `message` file (in `eamuse/` offline, or `hiraku-eamuse`'s data folder) scrolls across the title screen of every cabinet, from its next start.
  - Any server that implements docs/EAMUSE3.md works the same way; `tools/eamuse/check.py URL` checks one.
- **Course rankings** are kept and given back. The HITASURA and Gambler Z boards, KONAMI ID and ee'MALL follow the game's formats but haven't been seen in play yet (docs/EAMUSE3.md).
- **The card:** `card.bin` in the save folder (`~/.local/share/hiraku/popn9`), 128 bytes, created as a fresh card of the game played (pop'n 9 to 13); with a card number, `cards/NUMBER.bin`. Insert puts it in the slot (or takes it back); the reader takes it when the game opens the slot and gives it back when the game ejects it. With Auto Card Insert (`-autocard p1`) it goes in by itself.
- **The 10-key:** the numeric keypad's 0–9, Enter and `.`, with Backspace as Clear.

**pop'n music 10–14:** they run the same way and play (coins, mode, button select, songs); their lamps and inputs are the same as pop'n 9's, checked in each game's LAMP CHECK. What is pop'n 9's alone so far: `--autoplay`.
- **pop'n 10 unlocks:** right-click pop'n music 10 > **Unlocks**, as for pop'n 9: the event phase, from 1 (INTERNET RANKING) through HOSHI NI NEGAIO (2-4) to MAX (ALL DATA RELEASE). Or `tools/unlock.py popn10 phase=5`, or the five codes in test mode > EVENT OPTIONS > PASSWORD (`unlocks.toml` has them).
  - **HOSHI NI NEGAIO** (phases 2 and 3) is the shop's: every credit's stars fill five buckets of wishes (30 songs, characters, an EX chart), then a black bucket for the HELL 10 course. It runs in HIRAKU as on a cabinet, card or not. Unlocks shows each bucket's progress and can grant every wish at once (`tools/unlock.py popn10 hoshi=all`, or `hoshi=none` for a new event). At phase 4 or MAX everything it gives is playable anyway.
  - **INTERNET RANKING:** with e-AMUSEMENT on, the attract mode shows the EXPERT course rankings, and in phases 2 and 3 the shops' HOSHI NI NEGAIO board, from what the server has (HIRAKU's own, or a shared one for several cabinets).
- **pop'n 11 unlocks:** right-click pop'n music 11 > **Unlocks**: the event phase, from 1 (INTERNET RANKING) through POPN TOURIST (2-4) to MAX (ALL DATA RELEASE). Or `tools/unlock.py popn11 phase=5`, or the five codes in test mode > EVENT OPTIONS > PASSWORD.
  - **POPN TOURIST** (phases 2 and 3) is the shop's board game: its 18 finds release 14 songs, an option, a course and EX charts. Unlocks shows which are found and can mark them all found (`tools/unlock.py popn11 tourist=all`, or `tourist=none` for a new event). Phase 3 also releases the secret songs, and at phase 4 or MAX everything the board gives is playable anyway.
  - **Mileage songs:** nine songs a player buys with an e-AMUSEMENT card's mileage, kept by the server per card. Unlocks shows each card's tickets and can give every card nine tickets for each song (`tools/unlock.py popn11 tickets=all`); this needs HIRAKU's own server.
  - **INTERNET RANKING:** with e-AMUSEMENT on, the attract mode shows the EXPERT course rankings, and in phases 2 and 3 the shops' POPN TOURIST board ("獲得アイテム一覧": finds, and the date of the last one).
- **pop'n 12 unlocks:** right-click pop'n music 12 Iroha > **Unlocks**: the event phase, from 1 (INTERNET RANKING) and 2 (NET TAISEN MODE RELEASE) through NINPOUCHO (3-4) to MAX (ALL DATA RELEASE). Or `tools/unlock.py popn12 phase=5`, or the five codes in test mode > EVENT OPTIONS > PASSWORD.
  - **NINPOUCHO** (phase 3) is the shop's: after each credit the player stops a wheel of the four seasons, and the credit's points go to that season. Each season's points reach its six songs in turn; then ヒナマツリ and course 7. Unlocks shows each season's points and what comes next, and can reach everything at once (`tools/unlock.py popn12 ninpoucho=all`, or `ninpoucho=none` for a new event). Phase 3 also releases the secret songs; at phase 4 or MAX everything NINPOUCHO gives is playable anyway.
  - **NINPOUCHO keeps shop hours:** it only runs from 10:55 to 23:05 Japan time. HIRAKU's cabinet clock starts at 12:00 Japan time.
  - **NET TAISEN** (battles between cabinets): from phase 2, with e-AMUSEMENT on and a card in, NET対戦 is on the mode wheel. The game looks for opponents during song select (30 s). Alone, nobody comes: it says so and starts the credit in CHALLENGE mode.
  - **Online:** cabinets on different PCs meet through `hiraku-eamuse`: point each at it (`-url server:8080`, or the launcher's EA Service URL), with e-AMUSEMENT on. `hiraku-eamuse` also runs NET TAISEN's lobby and relay on TCP port 5740, which must be reachable from the cabinets (open it in the server's firewall; the cabinets need nothing opened). Each cabinet says at start whether it reached the relay. It plays as on real cabinets: no lockstep, each runs at its own speed.
  - **Two cabinets** (two HIRAKU processes on one PC, or on two over TCP): start the first with `--lan-host SOCKET` and the other with `--lan-join SOCKET`. SOCKET is a path (a Unix socket) or `host:port` (TCP; the first listens, the other connects). Each needs its own save folder (`--nvram`) with e-AMUSEMENT on. They form one shop LAN (192.168.50.2 and .3); the first keeps the lobby. The two run in lockstep, a frame at a time, so both go at the slower one's pace. Choose NET対戦 on both, the second a few seconds after the first. They meet, and a CPU takes the third seat. More cabinets: `--lan-cabinets N` on the first (up to 4). A match holds three players, so three cabinets play one together; a fourth waits for a match of its own.
  - **INTERNET RANKING:** with e-AMUSEMENT on, the attract mode shows the EXPERT course rankings, and at phase 3 the shops' NINPOUCHO board ("ポップン秘伝忍法帖", each shop's scrolls).
- **pop'n 13 unlocks:** right-click pop'n music 13 Carnival > **Unlocks**: the event phase, from 1 (POP'N LAND) and 2 (POP'N WORLD) to MAX (ALL DATA RELEASE), and EXTRA (BEMANI EXPO: アジアンミクスチャー, ピアノトロニカ, クイズマジックアカデミー). Or `tools/unlock.py popn13 phase=4 extra=1`. In test mode > EVENT OPTIONS the passwords are hidden: blue right then red opens PHASE PASSWORD (the four codes in order), blue left then red EXTRA PASSWORD.
  - **POP'N LAND** (phase 1) and **POP'N WORLD** (phase 2) are the shop's theme parks: after each credit the player picks a card, and its points build one attraction (a song). Each song opens at its mark, then its HYPER and EX charts; older songs gain new charts; the last attractions open courses 6 and 7. Unlocks shows how far each park is and what is nearest, and can build everything at once (`tools/unlock.py popn13 land=all`, or `land=none` for a new event). Phase 1 also releases the secret songs; phase 2 releases everything POP'N LAND gives, phase 3 POP'N WORLD's.
  - **Shop boards:** with e-AMUSEMENT on, the attract mode shows the shops' ポップランド board at phase 1 and ポップワールド board at phase 2 (each shop's attractions, COMPLETE!! when all are built).
  - **NET TAISEN** (pop'n 13 and 14, any event phase): with e-AMUSEMENT on and a card in, NET対戦 is one left of ENJOY on the mode wheel, and plays as pop'n 12's (above): alone, CHALLENGE mode after the search; with other HIRAKU cabinets on a LAN cable or online, three to a match. The games run either NET TAISEN or the INTERNET RANKING, never both; HIRAKU's server gives NET TAISEN. For the ranking instead, put `1` in the file `net_event-E13` (pop'n 13) or `net_event-F14` (pop'n 14) in the server's data folder (the save folder's `eamuse/`, or `hiraku-eamuse`'s `--data`); `2` or no file is NET TAISEN.
- **pop'n 14 unlocks:** right-click pop'n music 14 FEVER! > **Unlocks**: the event phase, from 1 (the SHOP EVENT's first story) through the seventh story (7) and its end (8) to MAX (ALL DATA RELEASE, with the ee'MALL songs). Or `tools/unlock.py popn14 phase=9`. In test mode > EVENT OPTIONS the password is hidden, as in pop'n 13: blue right then red opens PHASE PASSWORD (the nine codes in order).
  - **The SHOP EVENT** (フィーバー戦士ポップン14, phases 1-7) is the shop's: each phase is a story whose four songs the shop's credits rescue, one at each mark; once a story is done, credits fill FEVER ROBO's gauge, which gives フィーバーロボ. Unlocks shows each story's progress and what is nearest, and can rescue everything at once (`tools/unlock.py popn14 story=all`, or `story=none` for a new event). Each story's songs are released two phases after it, everything at MAX. The final story's four EX charts need a count only reached online; MAX releases them.
  - **INTERNET RANKING:** with e-AMUSEMENT on and `net_event-F14` set to `1` (above, pop'n 13's NET TAISEN), the attract mode shows the EXPERT course rankings, and the story screen shows the songs rescued so far.
- **e-AMUSEMENT:** pop'n 10 to 14 work as pop'n 9 does (cards registered, saved and read back, pop'n 11's mileage too; test mode > NETWORK OPTIONS > e-AMUSEMENT OPTIONS); pop'n 12, 13 and 14 keep their player records and each song's best score.
- **One card for all the pop'n games:** set a card number once (the launcher's Settings > e-AMUSEMENT > Player 1 Card, or `-card0`; pop'n 14 needs one anyway) and use it in every game. The pop'n games share one e-AMUSEMENT folder beside their saves, so the first time you put that card into a newer game, it offers to carry your player over from the game before (name, clear marks, PIN): answer yes, type the old PIN, and when it asks for an unused card of its own, press Insert again. pop'n 14 asks for pop'n 13's transfer number (POP'N ID): type any number, then pop'n 13's PIN. Clear marks then follow the card everywhere: a song cleared in any of the six games shows its best mark in all of them (pop'n 9, which has no medals, shows it as cleared). Scores, options and the name stay each game's own. Saves from before keep their data: each game's old `eamuse/` folder is merged into the shared one the first time it starts (docs/EAMUSE3.md, "One card for all").
- **pop'n 14's e-AMUSEMENT PASS** is an IC card: only a number, `E004` and 12 hex digits, as in spice2x. HIRAKU uses `-card0` or `card0.txt` (the launcher's Player 1 Card), or makes a number and keeps it in the save folder's `card0.txt`. Insert puts the card in the reader's slot; the PIN is typed on the numeric keypad (the reader's 10-key: Enter is its blank key, `.` its 00 key) and checked by the server.
- pop'n 12–14 start in ENJOY mode with 5 buttons selected; blue moves to 9.
- The pop'n 11 dump has a damaged first sector (a PC wrote a blank partition table there); HIRAKU and Reliquary read the boot header's backup copy.

**From the launcher:** drop the `.py1` (or the set's folder) on its window. It draws them with Vulkan; **Settings > Display > Konami Python games** has Software, and on Windows Direct3D 11 and 12, for a PC whose graphics driver doesn't get on with Vulkan.

**On another PC:** `tools/package.sh` takes the Python binaries along with Reliquary's resources and the libraries built for it; `tools/pack-data.py DEST popn9` copies the game's data, CHD included (2.4 GB).

**Not yet:** the board's own CPU (Reliquary answers its FireWire commands directly), Python games other than pop'n 9–14 (they need their own `games/<id>/game.toml`; the ingester tells you the program's SHA-1).

## BEMANI PC: pop'n music 15 ADVENTURE to 18 せんごく列伝 (PLAN.md §30)

From pop'n music 15 on, the games are Windows programs. HIRAKU runs Konami's own program as it is, under Wine on Linux and directly on Windows, and puts its cabinet around it with `hiraku-pc` and two DLLs of its own (`bin/pc/`): the controls, the lamps, the card, and HIRAKU's own window for the pictures.

- **Adding it:** drop the game's folder, the one holding `prog` (its programs, by date) and `data`, on the launcher's window, or `tools/ingest.py FOLDER`. HIRAKU reads it where it is (8 GB): keep it there. The newest program is the one run, from a copy in the save folder with HIRAKU's `ezusb.dll` and `libacio.dll` in place of Konami's. The game's own files aren't changed.
- **Linux:** Wine must be installed (`wine` in the PATH, or `--wine PROGRAM`). The first start makes Wine's folder for the PC games (`pc-wine` beside the saves), which takes a minute.
- **Windows with an NVIDIA card:** full screen needs NVIDIA's "Vulkan/OpenGL present method" set to "Prefer layered on DXGI Swapchain" for `bin\hiraku-pc.exe` (NVIDIA Control Panel > 3D Settings > Manage 3D settings > Program Settings > Add). With the driver's default, HIRAKU's full-screen window holds the screen and Windows refuses the game its Direct3D ("Direct3D の初期化に失敗しました"); in a window the game runs either way. The launcher reads the setting when a PC game starts, says how to set it if it isn't, and writes it in the run's log.
- **First start:** the game makes its backup files and asks to be restarted (BACKUP ERROR 5-1502); `hiraku-pc` restarts it by itself.
- **e-AMUSEMENT:** with it on in test mode > NETWORK OPTIONS, the game reaches HIRAKU's own server (the pop'n games' shared `eamuse/` folder) or `-url`'s, and the title says NETWORK OK. Cards (the reader, `-card0`, or the card key), the PIN on the 10-key, a new player's name, the player kept from credit to credit; a pop'n 14 player of the same card is offered for carrying over. The data folder's files `ir_ranking-G15` (2, the default: the ADVENTURE map; 1: the expert courses' INTERNET RANKING), `net_event-G15` (1, the default: NET TAISEN; 0: none) and `phase-G15` choose the events.
- **NET TAISEN:** alone, the game waits for others while you set up your battle (two minutes), says nobody came and plays the credit in CHALLENGE mode; with other cabinets, CPUs fill the empty seats. To battle other HIRAKU cabinets, run `hiraku-eamuse` on one PC and give every cabinet `-url that-pc:PORT` (the launcher's EA Service URL): they meet in its lobby (port 5741) and play through its relay (TCP 5740), wherever they are.
- **Controls:** the pop'n controls and lamps of the launcher's Controls and Lights (pop'n music, PC), the pop'n board too (`--popn-io auto`, its firmware from the game's own `ezusb.dll`).
- **The picture is HIRAKU's:** the game's own window stays hidden, and each picture it draws goes to HIRAKU's window, as an emulated game's do: the window's size, full screen (F11), the CRT pictures (F10, `--picture`), the beam simulator, display sync, Esc to quit.
- **Latency:** the game starts each frame when HIRAKU's display pacing starts it, as an emulated machine's frame: it reads its controls then, draws (about 3.5 ms under Wine on a fast PC), and its picture goes to the display's next refresh. HIRAKU takes out the game's own wait of 14 ms before each picture (it kept its 60 a second on its own monitor, and HIRAKU's pacing does that now), so from the controls read to the picture handed to the display is 4.9 ms (median) instead of a frame and more (about 18 ms). Each picture brings the sound the game started while making it (a keysound with the press that started it). `--game-ahead` lets the game draw its next picture while one is shown, as before: a frame's more time for a PC too slow to draw in its budget, a frame more from the controls to the screen. The exit report's `hiraku-pc: the game's frame` line says how long its frames take.
- **Presses:** each game reads its buttons once a frame and judges a press as if it came then, up to a frame (17 ms) after it did, as on its cabinet. With `--press-time` (Settings > Audio, Latency: "Presses judged when they happened") HIRAKU hands the game each press's own time, from the controller's or keyboard's event, and the game judges it at that moment. `--judge-offset MS` (Judgement offset) judges every press MS milliseconds earlier than it came, for a TV or sound system that runs late: if the notes look late and your presses land late, a positive offset puts them back in time. Both work in the game's 4 ms steps, on pop'n 15 to 18 (every program date; the log says `presses judged ...`). pop'n 16-18 read their buttons as the board's samples, a press being a sample with the button after four without: HIRAKU gives them six samples a frame, as Konami's board does, so the same button pressed again quickly counts each time (with one sample a frame, as before, a release needed four frames).
- **The PIN:** `-autopin` (Settings > e-AMUSEMENT: "Type the card's PIN") types a known card's PIN on the 10-key when the game asks for it, from HIRAKU's own server (pop'n 14-18).
- **Event mode, free play, stages per credit:** the game's own settings, in test mode (GAME OPTIONS, COIN OPTIONS).
- **The sound goes through HIRAKU** too: HIRAKU's DLL is the game's DirectSound and mixes its sounds as the frames go by (44.1 kHz, 735 sound frames a frame), and HIRAKU plays them with its own sound settings (the device, the latency, ASIO on Windows, the volume), in step with the pictures. Neither Wine's nor Windows' sound mixer is involved.
- **Mods:** as for the other games (right-click: Mods…; [MODS.md](MODS.md), "The PC games"): memory patches at the program's addresses, and files at their paths under the game's `data` folder, read instead of the game's own. Its check of its data passes: HIRAKU gives the modded files' MD5s to the copy of its list in the save folder.
- **On the command line:** `bin/hiraku-pc --game popn15 --window [--fullscreen] [--picture light]`.

**Not yet:** a check with a real board or reader. On Windows 10, pop'n 15 to 18 start and run (a player's PC with an NVIDIA card, GitHub issue 4).

**pop'n music 16, 17, 18:** add them as pop'n 15 (their folders hold `prog` and `data`). They play online with HIRAKU's server: coins, a card in the slot reader (a new player with a name, or one carried over from the game before), songs, and what a credit changes is kept: the options, the character, the last song, each song's clear marks and best scores, the most played songs, pop'n 18's story map. NET TAISEN works between cabinets on one server, and each player's NET results are kept (checked with two cabinets on pop'n 16, 17 and 18). Also kept: pop'n 16's PARTY guests, pop'n 17's characters, tickets and targets, pop'n 18's licences, the expert courses and their rankings (16, 17), pop'n 18's INTERNET RANKING entries. Clear marks and best scores are shared with pop'n 9-15 and between 15-18 (one card for all, [EAMUSE3.md](EAMUSE3.md)). Not yet: pop'n 18's boss battles, mods that replace data files (memory patches work). Real cabinets are unchecked; on Windows 10 they start and run (GitHub issue 4). Details: PLAN.md section 30, PC-6.

**beatmania IIDX 9th style (in progress, PLAN.md §33):** the first IIDX on a Windows PC (Konami's C02 cabinet), run the same way, with a board DLL of its own (`bin/pc/c02/ezusb.dll`). Add it by dropping its `C02` folder (`data` and the programs, a folder each by revision: JAA, JAB...; the last is run) or `tools/ingest.py FOLDER`. It plays offline: coins, the stages with their movies, both players' keys, turntables, Effect, VEFX and the effector sliders (the IIDX controls of the launcher's Controls), the picture and sound through HIRAKU as above. A new cabinet starts set up: the game's own default settings, its clock set, saved the first time (no operator's setup needed); what you change in its test mode (free play, coins, sound, game options) is kept from start to start, in the save folder's `e` and `f` (its two backup drives). e-AMUSEMENT plays on HIRAKU's server as pop'n's: a card slot and 10-key each side (the launcher's Controls: Card and Keypad for player 1, Insert and the keypad; P2 Card and P2 Keypad for player 2, Delete and no keys to start with), the cards kept in the save folder (`card.bin`, `card2.bin`), a new one registered in the game (name, PIN); each player's scores and DJ levels kept and shown on music select, the week's song, the most played songs (the ALL JAPAN RANKING courses) and the EXPERT courses' internet ranking. Its shop name and prefecture are set for it (ＨＩＲＡＫＵ, Tokyo) if unset; e-AMUSEMENT itself is the operator's to turn on in test mode. Not yet: KONAMI ID; the lamps and the 16-segment ticker are passed on but unchecked. Under Wine only so far.

**beatmania IIDX 11 IIDX RED (in progress, PLAN.md §36):** the same cabinet, run the same way. RED's program talks to the board's USB driver itself, so HIRAKU answers there, from an `RtEffect.dll` of its own (`bin/pc/c02/RtEffect.dll`, in place of Konami's Realtek sound effects, which only work on the cabinet's sound chip). Add it by dropping its `E11` folder (`data` and the program folders JAA... ; JAD, the newest, may sit in a folder of its own beside it) or `tools/ingest.py FOLDER`; a program folder rebuilt by someone else since (a packed `bm2dx.exe`) isn't taken. The cabinet's software id (`sidcode.txt`) is written for it. It plays offline as 9th style does: coins, the stages with their movies, both players' keys, turntables, Effect, VEFX and the effector sliders; a new cabinet starts set up and test mode's changes are kept (hold Test a moment to open it). Not yet: e-AMUSEMENT (the card readers answer, empty), the lamps and ticker unchecked. Under Wine only so far.

**beatmania IIDX 12 HAPPY SKY (in progress, PLAN.md §36):** as RED, from HIRAKU's `RtEffect.dll`. Add it by dropping its `ECO` folder (`data` and the program folders JAA... ; JAD is the newest) or `tools/ingest.py FOLDER`. It plays offline as RED does, and a new cabinet starts set up. Not yet: e-AMUSEMENT, the lamps and ticker unchecked. Under Wine only so far.

**beatmania IIDX 13 DistorteD (in progress, PLAN.md §36):** the same way; add its `FDD` folder or `tools/ingest.py FOLDER` (ingest writes a song's missing sound list, `owl.bin`, which the start-up checks). It boots to attract and its demo, takes a credit and reaches MODE SELECT; the music select crashes for now.
