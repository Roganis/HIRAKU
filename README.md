<p align="center">
  <img src="launcher/res/logo.png" alt="HIRAKU" width="720">
</p>

<p align="center"><b>“HIRAKU” is all about giggles and fun. Enjoy!!</b></p>

HIRAKU plays Konami's BEMANI arcade games on Linux and Windows, from DJ Main (1997) to the first BEMANI PC cabinets (2010), all from one launcher. It brings back what went away with the arcades too: e-AMUSEMENT cards, scores and rankings, events, and pop'n music's NET TAISEN battles.

HIRAKU ships no game data: you bring your own dumps.

## What it plays

| Board | Games | Status |
|---|---|---|
| **DJ Main** | beatmania 1st MIX to THE FINAL, hiphopmania, pop'n music 1–3, pop'n stage (29 sets) | Play; matches MAME clock for clock |
| **Firebeat** | beatmania III (all five), pop'n music 4–8, Animelo 1/2, Mickey Tunes!, ParaParaParadise, Keyboardmania | beatmania III and ParaParaParadise play; pop'n music and Keyboardmania boot, some songs play |
| **System 573** | Dance Dance Revolution, GuitarFreaks, DrumMania, Dance Maniax, Percussion Freaks, Mambo A Go-Go (90 sets) | In lockstep with MAME, plus e-AMUSEMENT and GuitarFreaks × DrumMania session play, which MAME doesn't run |
| **Twinkle** | beatmania IIDX 1st style to 8th style, Substream, the club kits (18 sets) | Play, with the background videos MAME doesn't show |
| **Konami Python** | pop'n music 9–14, DrumMania V and V2 | pop'n music plays, with e-AMUSEMENT and NET TAISEN; DrumMania V and V2 run |
| **BEMANI PC** | pop'n music 15–18, beatmania IIDX 9th style to 16 EMPRESS | Play, with e-AMUSEMENT; Windows programs, run under Wine on Linux |

What each game needs, its first start and its quirks: [docs/PLAY.md](docs/PLAY.md).

## Highlights

- **e-AMUSEMENT, back.** A server of HIRAKU's own: register a card, keep your player, scores and rankings. One card follows a player from pop'n music 9 to 18. Share it between PCs with `hiraku-eamuse`. ([docs/EAMUSE.md](docs/EAMUSE.md))
- **NET TAISEN.** pop'n music 12–18's online battles between HIRAKU cabinets, on a LAN or online.
- **Events and unlocks.** Right-click a game > **Unlocks…** to set the event phases and see their progress.
- **The game menu.** Hold 5, 7 and 3 on the cabinet's 10-key to change games without a keyboard.
- **Low latency.** The sound is the clock and tunes itself to your PC; ALSA, WASAPI exclusive and ASIO outputs.
- **The picture.** Each game at its monitor's shape, CRT shaders (F10), a CRT beam simulator, and smooth motion or true high frame rates on 120–240 Hz displays. ([docs/SMOOTH.md](docs/SMOOTH.md))
- **Controls and lights.** Keyboards, IIDX/pop'n controllers and turntables, card readers and cabinet I/O boards; the cabinet's lamps on your controller's LEDs. ([docs/LIGHTS.md](docs/LIGHTS.md))
- **Mods.** Patches, replaced files and text on every board, turned on per game in **Mods…**. ([docs/MODS.md](docs/MODS.md))
- **Light on the PC.** Made for N100-class mini PCs, and checked frame by frame against MAME.

## Getting started

1. Download the latest package from [Releases](../../releases) and unpack it anywhere:
   - **Windows** 10 or later: `hiraku-windows-x86_64.zip`, then start `HIRAKU.exe`. The build isn't signed: if Windows warns, click *More info*, then *Run anyway*.
   - **Linux** with glibc 2.35 or later (Ubuntu 22.04, Debian 12, Fedora 36 and newer): `hiraku-linux-x86_64.tar.xz`, then start `HIRAKU`.
2. Drop a game's files on the launcher's window: the MAME ROM set (`.zip`, `.7z` or a folder) with its CHDs, or disc images. HIRAKU recognises the game and copies what it needs.
   - Konami Python games: also drop the `bios` and `keys` folders, once.
   - BEMANI PC games: drop the game's folder (the one with `prog` and `data`, or the IIDX revision folders). On Linux, install Wine.
3. Double-click the game.

The package is portable: your saves and settings stay in its `user` folder. Updates come through the launcher.

## Building from source

You need CMake, Ninja, clang, SDL3, zlib, zstd and, for the launcher, Qt 6. FFmpeg (IIDX videos) and FluidSynth are optional.

```bash
cmake -S . -B build/cmake -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
ninja -C build/cmake
build/cmake/hiraku                    # the launcher
```

The Konami Python core builds inside PCSX2 Reliquary's own build: `oracle/pcsx2/build-reliquary.sh all` ([docs/PLAY.md](docs/PLAY.md#konami-python-1-popn-music-914)). Tests and the regression cases: `ctest --test-dir build/cmake`, `tools/regress.py check`.

## Documentation

- **Players:** [PLAY.md](docs/PLAY.md) (games, settings, controls, latency), [MODS.md](docs/MODS.md), [SMOOTH.md](docs/SMOOTH.md), [LIGHTS.md](docs/LIGHTS.md), [EAMUSE.md](docs/EAMUSE.md) (e-AMUSEMENT and NET TAISEN)
- **Developers:** [PLAN.md](PLAN.md) (status and plans), [docs/README.md](docs/README.md) (every document), [oracle/README.md](oracle/README.md) (the MAME oracle)

## How it works

Firebeat games are statically recompiled from PowerPC to C (the packages, which carry no game code, run the same analysis through an executor). DJ Main, System 573, Twinkle and Python run as cores behind one shell: interpreters, recompilers fed by recordings, or a hosted emulator (PCSX2 Reliquary). BEMANI PC games run Konami's own program, with HIRAKU's DLLs as the cabinet around it. More in [docs/RECOMP.md](docs/RECOMP.md) and [PLAN.md](PLAN.md).

## Licence

GPL-3.0-or-later (see `LICENSE`). Code taken from other projects keeps its own licence and notices: MAME (BSD-3-Clause), Musashi (MIT), PCSX2 Reliquary (GPL-3.0-or-later), minimp3 (CC0), libchdr (BSD-3-Clause, with dr_flac and the LZMA SDK decoder, public domain), libretro's slang-shaders (GPL-2.0-or-later), Lola Retro (MIT) and the Blur Busters CRT Beam Simulator (MIT). Parts of HIRAKU were written after spice2x (GPL-3.0: the card readers, the BIO2 board, the PC games' I/O) and kbinxml (MIT: binary XML). popnhax (CrazyRedMachine) gave the ideas for presses judged when they happened, the PIN typed for the player and text mods; it has no licence, so none of its code is used. The full list is in the launcher's Help > About HIRAKU.

Thanks to the testers, tyam, yaladre, [MohaHtn](https://github.com/MohaHtn) and [CrazyRedMachine](https://github.com/CrazyRedMachine), who tried HIRAKU on their own PCs and cabinets.

beatmania, pop'n music, Keyboardmania, ParaParaParadise, GuitarFreaks, DrumMania, Dance Dance Revolution, beatmania IIDX and e-AMUSEMENT are trademarks of Konami. HIRAKU is not affiliated with or endorsed by Konami.
