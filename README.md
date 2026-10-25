<p align="center">
  <img src="launcher/res/logo.png" alt="HIRAKU" width="720">
</p>

<p align="center"><b>“HIRAKU” is all about giggles and fun. Enjoy!!</b></p>

HIRAKU runs Konami's BEMANI arcade games as native or optimized interpreted programs on Linux and Windows. It covers five arcade boards, from DJ Main (1997) to Konami Python (2002–2006), and the first BEMANI PC games (2007–2010), all behind one launcher.

- **Firebeat** games are statically recompiled: HIRAKU analyses each game's PowerPC program and translates it to C, linked against a runtime that stands in for the board. The packages, which carry no game's code, run the same analysis through an executor (`hiraku-firebeat`) instead, with identical results.
- **DJ Main, System 573, Twinkle and Python** run as cores behind the same shell (`runtime/shell/core.h`). Each core is an interpreter, a recompiler fed by recordings, or a hosted emulator.
- **BEMANI PC** games are Windows programs: HIRAKU runs Konami's own program as it is (under Wine on Linux) and is the cabinet around it, its I/O board, card reader, sound and screen, through DLLs of its own (`runtime/pc/`).

HIRAKU ships no game data. You provide your own dumps: drop your MAME ROM sets and CHDs (or disc images) on the launcher, and it does the rest.

## What it runs

| Board | How HIRAKU runs it | Games | Where it stands |
|---|---|---|---|
| **Firebeat** (PPC403) | PowerPC → C static recompilation; the SPU's 68000 is interpreted | beatmania III, APPEND CORE REMIX, 6th MIX, 7th MIX, The Final | Play, with sound, keyboard, controllers and lamps |
| | | ParaParaParadise, v1.1, 1st Mix Plus, ParaParaDancing | Play: song select, a stage danced on the sensor bars, results |
| | | pop'n music 4–8, Animelo 1/2, Mickey Tunes! | Boot to attract with sound; pop'n 8 tested through song select and full songs |
| | | Keyboardmania 1st/2nd/3rd Mix, Keyboardheaven | Boot to attract; the XT446 synth card (an embedded MU100B) is emulated; autoplay clears a song 135/135 GREAT |
| **DJ Main** (68EC020, 1997–2002) | Interpreted (Musashi), about 1 ms of host time a frame | 29 sets: beatmania 1st mix → THE FINAL, complete MIX 1/2, CORE REMIX, DCT, hiphopmania, beatstage, pop'n music 1–3, pop'n stage EX | All 25 sets checked match MAME clock for clock through power-on and attract; beatmania 1st MIX and pop'n music 1 through a whole game: same RAM, pixels and samples |
| **System 573** (PlayStation-based) | MIPS interpreter plus C++ compiled from recorded blocks; MAME's PS1 devices | Dance Dance Revolution 2ndMIX, 3rdMIX | Play, including the first-boot flash install with the install cassette |
| | | DrumMania 10th Mix, GuitarFreaks 11th Mix | **Further than MAME:** they play, including DrumMania × GuitarFreaks session play, the e-AMUSEMENT network unit, and cards |
| **Twinkle** (beatmania IIDX) | The same PS1 core plus the 68000/RF5C400 sound board | beatmania IIDX 1st style, Substream, 2nd–8th style | Play in lockstep with MAME. **Further than MAME:** the background videos play, and 1st style runs (MAME stops it at E600) |
| **Konami Python 1** (PS2-based) | PCSX2 Reliquary, hosted by HIRAKU's shell | pop'n music 9, 10, 11, 12 Iroha, 13 Carnival, 14 FEVER! | Play, with e-AMUSEMENT cards, the event unlocks, and NET TAISEN between cabinets |
| **BEMANI PC** (a Windows PC) | Konami's program as it is, under Wine on Linux; HIRAKU's DLLs stand in for the board, the card reader and DirectSound, and its window shows the pictures | pop'n music 15 ADVENTURE, 16 PARTY♪, 17 THE MOVIE, 18 せんごく列伝 | Play, with e-AMUSEMENT on HIRAKU's server (players, scores, events) and NET TAISEN between cabinets. Checked under Wine; real Windows PCs and cabinets not yet |

The per-game guides are in [docs/PLAY.md](docs/PLAY.md): which files each game needs, its first-boot steps, controls and quirks.

## Highlights

- **e-AMUSEMENT, brought back.** HIRAKU includes its own server for Konami's long-closed network. You can register a card, save your player, and keep rankings. On pop'n music 9–18 the server runs inside the game. To share cards and rankings between PCs, run `hiraku-eamuse`. DrumMania and GuitarFreaks use it through their network unit.
- **One card for all.** One card number follows a player from pop'n music 9 to 18: each game carries the player over from the one before, and clear marks and best scores are shared between all of them.
- **pop'n NET TAISEN.** pop'n music 12–18's online battles work between HIRAKU cabinets, up to three to a match. Cabinets connect through a virtual LAN cable, on one PC or across a network, or online through `hiraku-eamuse`, which also runs the lobby and relays the matches.
- **Events and unlocks.** In the launcher, right-click a game > **Unlocks**. It sets the event phase as an operator's codes would, and shows each event's progress, such as Sugoroku de 8, HOSHI NI NEGAIO, POPN TOURIST and NINPOUCHO. It can also type the cabinet's button codes for you (Ctrl+1…9).
- **Low latency.** Audio is the clock, and the sound queue tunes itself to the smallest lead your PC can play without crackle. You can also send sound straight to an ALSA card with no sound server in between (`--audio-device`), or on Windows have a device alone in WASAPI exclusive mode (`--audio-exclusive`) or play through its ASIO driver (`--audio-asio`). On 573 and Twinkle the sound is handed on every millisecond, not in MAME's 20 ms pieces. Pictures are shown at vblank end, and frame pacing is aware of Wayland.
- **Efficient.** HIRAKU targets N100-class mini PCs and judges its changes by their worst frames, not their average. Profile-guided builds (`tools/pgo.py`) cut CPU use further: Keyboardmania −19 %, DDR −27 %.
- **Controls and lights.** Map keyboards, IIDX/pop'n controllers and analog turntables to each cabinet's controls with click-then-press. You can drive controller LEDs from the cabinet's lamps and watch them live ([docs/LIGHTS.md](docs/LIGHTS.md)).
- **The picture.** Every game is shown at its monitor's shape: 4:3, or 16:9 for beatmania IIDX's widescreen CRT. CRT shaders are built in (Lola Retro, crt-guest-advanced-fast, crt-royale): F10 cycles through them in-game and F8 changes their settings while you play. On a high refresh rate display, a CRT beam simulator (`--crt-beam`) brings back a CRT's motion clarity.
- **Mods.** Patches to a game's memory, replaced files and text, on every board ([docs/MODS.md](docs/MODS.md)); the launcher's right-click > Mods… turns them on per game.
- **Checked against MAME.** Every change is held to bit-identical per-frame picture and sound hashes (`tools/regress.py`). New boards are brought up in lockstep against a scripted MAME oracle.
- **Portable.** `tools/package.sh` builds a package for other Linux PCs that brings its own Qt. In a portable install, your saves and settings live in the HIRAKU folder, so the folder can go on a USB stick. The cores also build and pass the regression cases on 64-bit ARM Linux.

## Download

Get the latest package from the [Releases](../../releases) page and unpack it anywhere:

- **Windows** (10 or later): `hiraku-windows-x86_64.zip`, then start `bin\hiraku.exe`. Windows may warn about an unknown publisher (the build isn't signed): *More info*, then *Run anyway*.
- **Linux** (glibc 2.35 or later: Ubuntu 22.04, Debian 12, Fedora 36 and newer): `hiraku-linux-x86_64.tar.xz`, then start `bin/hiraku`.

Then drop a game's files on the launcher's window, all at once: the MAME ROM set (`.zip`, `.7z` or a folder) and its CHDs (several games' files at once work too). HIRAKU recognises the game, copies what it needs into its own folder, and the game is ready to play. Nothing else needs installing: the packages carry their own Python, 7-Zip, CHD reader and libraries.

The packages run the DJ Main, System 573, Twinkle and Firebeat games, and the Konami Python games too (pop'n music 9–14, DrumMania V/V2; they need Vulkan or OpenGL drivers): add a Python game's folder together with the folders `bios` (the arcade BIOS) and `keys` (the MagicGate key files), which HIRAKU can't ship; a game added without them waits until you drop them, and they're kept for every Python game. For a BEMANI PC game (pop'n music 15–18), drop its folder, the one holding `prog` and `data`; on Linux, Wine must be installed. The packages are portable: your saves and settings stay in their `user` folder.

## Building from source

You need CMake, Ninja, clang, SDL3, zlib and zstd (to read MAME's CHDs) and, for the launcher, Qt 6. Optional: FFmpeg's libraries (IIDX background videos) and FluidSynth (Keyboardmania's stand-in synth). The Python core is built inside PCSX2 Reliquary's own source tree (see [docs/PLAY.md](docs/PLAY.md#konami-python-1-popn-music-914-in-progress-m16)).

```bash
cmake -S . -B build/cmake -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
ninja -C build/cmake
build/cmake/hiraku                          # the launcher
```

To add a game, drop its files on the launcher's window: the MAME ROM set (`.zip`/`.7z`), its CHDs, or disc images (`.cue`/`.bin`, `.iso`, `.img`). You can also use **File > Add game files**, or `tools/ingest.py FILE...` from a terminal. The ingester recognises the game and what each image is. Firebeat games then run in `hiraku-firebeat`, which finds its way around the program by itself when it starts. For about twice the speed, a game's program can be recompiled into its own binary:

```bash
mkdir -p build/bm3final && build/cmake/hiraku-recomp --analyze dumps/bm3final/program.bin --entry 80003000 > build/bm3final/recomp.txt
cmake build/cmake && ninja -C build/cmake hiraku-bm3final
```

The launcher shows which games are ready, and what a game that isn't ready still needs. `hiraku popn7` starts a game straight away, and `build/cmake/hiraku-<game> --window` runs one without the launcher. Settings live in `~/.config/hiraku/`, and saves in `~/.local/share/hiraku/<game>/`. In a portable install, both live in the HIRAKU folder's `user/` instead.

## For developers

```bash
ctest --test-dir build/cmake                # instruction semantics vs QEMU
tools/regress.py record; tools/regress.py check   # runs stay bit-identical (per-frame picture + sound hashes)
tools/regress.py bench                      # share of a core, and host work per frame (mean / p99 / worst)
tools/pgo.py                                # profile-guided rebuild, trained on the regress cases you have
build/cmake/hiraku-bm3final --script oracle/scripts/bm3final-practice.his --wav out/practice.wav
build/cmake/hiraku-bm3final --frames 1800 --snap-every 300 --trace   # headless, PPM frames + MMIO trace
cmake -B build/arm64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake && ninja -C build/arm64 -j2
HK_REGRESS_BIN=build/arm64 tools/regress.py check   # the ARM build against the same baseline
```

- Plan, status and per-board notes: [PLAN.md](PLAN.md)
- Playing, per game: [docs/PLAY.md](docs/PLAY.md)
- How Firebeat translation works: [docs/RECOMP.md](docs/RECOMP.md); the kernel: [docs/KERNEL.md](docs/KERNEL.md)
- Konami Python: [docs/PYTHON.md](docs/PYTHON.md); e-AMUSEMENT and NET TAISEN: [docs/EAMUSE3.md](docs/EAMUSE3.md); mods: [docs/MODS.md](docs/MODS.md), songs back from Omnimix packs: [docs/SONGS-BACK.md](docs/SONGS-BACK.md)
- The MAME oracle: [oracle/README.md](oracle/README.md)

## Licence

GPL-3.0-or-later (see `LICENSE`). Code taken from other projects keeps its own licence and notices. This includes MAME (BSD-3-Clause), Musashi (MIT), PCSX2 Reliquary (GPL-3.0-or-later), minimp3 (CC0), libchdr (BSD-3-Clause, with dr_flac and the LZMA SDK decoder, public domain) libretro's slang-shaders (GPL-2.0-or-later), Lola Retro (MIT) and the Blur Busters CRT Beam Simulator (MIT). Parts of HIRAKU were written after spice2x (GPL-3.0: the card readers, the BIO2 board, the PC games' I/O) and kbinxml (MIT: binary XML). popnhax (CrazyRedMachine) gave the ideas for presses judged when they happened, the PIN typed for the player and text mods; it has no licence, so none of its code is used. The full list, with the references HIRAKU was checked against, is in the launcher's Help > About HIRAKU.

Thanks to the testers, tyam, yaladre, [MohaHtn](https://github.com/MohaHtn) and [CrazyRedMachine](https://github.com/CrazyRedMachine), who tried HIRAKU on their own PCs and cabinets.

beatmania, pop'n music, Keyboardmania, ParaParaParadise, GuitarFreaks, DrumMania, Dance Dance Revolution, beatmania IIDX and e-AMUSEMENT are trademarks of Konami. HIRAKU is not affiliated with or endorsed by Konami.
