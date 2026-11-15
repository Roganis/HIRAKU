# PCSX2 Reliquary oracle (Konami Python)

MAME's Python drivers are skeletons, so the reference for a Python core is PCSX2 Reliquary, a GPL-3.0+ PCSX2 fork that runs Python 1 and 2 games (PLAN.md §23, docs/PYTHON.md). This directory builds it with HIRAKU's patch, and runs a set from pristine media in its stock front end, `pcsx2-qt`.

The same build also compiles HIRAKU's Python core (`runtime/python/`, `hiraku-<game>`, linked into `build/cmake/`): Reliquary's emulator hosted by HIRAKU's shell. The core is checked against `pcsx2-qt` from power-on (identical EE/IOP RAM hashes); scripted inputs then run in the core, with HIRAKU's script runner.

```bash
oracle/pcsx2/build-reliquary.sh all                    # deps, oracle patch, configure, build (capped, -j8)
oracle/pcsx2/run.py popn9 --seconds 150 --snap-every 25                  # attract, screenshots + EE RAM
oracle/pcsx2/run.py popn9 --frames 3000 --snap-every 0 --unlimited       # RAM hashes every 60 frames, then stop
oracle/pcsx2/run.py popn9 --frames 3000 --snap-every 0 --interp          # the same with every CPU interpreted
```

**Files:**
- `build-reliquary.sh`: builds `pcsx2-qt` from the release zip unpacked to `$HK_PYTHON_WORK/pcsx2-reliquary` (default `Python-work` beside the HIRAKU checkout).
  - Six dependencies (KDDockWidgets, plutovg, plutosvg, rapidyaml, libbacktrace, ECM) are built into `$HK_PYTHON_WORK/deps` at the versions and SHA-256s of Reliquary's CI. The rest are Arch packages.
  - Each heavy step runs under `systemd-run` with 10 GB and -j8. The main build peaks at 1.3 GB and takes ~1.5 min.
  - The zip lacks the paraLLEl-GS submodule, so that renderer and Wayland pointer locking are left out.
- `reliquary-oracle.patch` (ten files):
  - a per-vsync hook in `VMManager.cpp`: `HK_FRAMEHASH=<file>` appends `frame ee_hash iop_hash` (FNV-1a over the 32 MiB EE and 2 MiB IOP RAM) every `HK_FRAMEHASH_EVERY` vsyncs; `HK_STOP_FRAME=<n>` exits at vsync n. Works in `pcsx2-qt` and in `hiraku-<game>` alike;
  - `pcsx2/HirakuHooks.h`: the taps the core uses, all null otherwise:
    - the vsync, before and after it is posted to the GS (`VMManager.cpp`, `Counters.cpp`);
    - the audio output and the displayed picture;
    - the interface board's output command, which carries the lamps and is the input poll (`KonamiPython1.cpp`);
    - the interface board's serial port (the e-AMUSEMENT card reader/writer and 10-key): the host can answer the game's writes itself, and longer answers are then delivered whole (`KonamiPython1.cpp`);
    - the interface board's socket commands (it has the network port): the host can answer them itself, for e-AMUSEMENT without host sockets (`KonamiPython1.cpp`);
    - `full_frames`: the GS ignores the games' SCANMSK, so every line is drawn every frame (`GSState.cpp`). The pop'n games draw only the lines of the TV field shown; the core sets this unless run with `--interlaced`;
  - the GS's memory as an anonymous memory file (`GS.cpp`, Linux): stock PCSX2 opens a shared-memory object of a fixed name, `/GS.mem`, and unlinks it straight after, so two emulators starting their GS at the same moment (parallel regress runs) could collide: "Failed to allocate GS memory storage.", then "Aborting application." (21 of 3,200 simultaneous starts in a test);
  - the derived MagicGate keystore is printed only with `PCSX2_PRINT_KEYSTORE` set (stock Reliquary always prints it to stdout);
  - `-DHIRAKU_DIR=<checkout>` in the top `CMakeLists.txt` adds `runtime/python/`.
- `run.py`: one run of a `.py1` set from the collection (the folder named by `HK_PYTHON_SETS`, which must be set; `HK_PYTHON2_SETS` for a `.py2` set).
  - **Pristine media:** the set's small files (BBSRAM, dongles, boot ROM, memory-card dongle) are copied fresh, the CHD is linked, and the CHD write overlay starts empty. Reliquary writes all of these back.
  - **Pinned settings:** RTC 2003-06-01 12:00, MTVU off, game patches off, no audio device, memory-card slot 2 off (as on the cabinet, and as in the core), software GS (`--renderer 14` for Vulkan).
  - **Capped:** 6 GB of memory, and a timeout.
  - **Snapshots:** with `--snap-every S`, a savestate is saved over PINE every S seconds of wall time, and its `Screenshot.png` and EE RAM are kept. These are wall-clock, so not frame-aligned; use the frame hashes to compare runs.
  - **Cost:** host CPU from 30 s on is printed at the end.
  - **Output:** in `$HK_PYTHON_WORK/oracle/out-<set>/`, outside the repo.

**Known so far** (2026-10-13, pop'n 9):
- Runs repeat exactly for a fixed CPU mode, at any host speed: identical EE/IOP hashes at all 50 checkpoints over 3,000 frames.
- JIT and interpreter runs differ from frame 60 on.
- The interpreters run at ~0.75× real time here.

**Caveats:**
- **It needs a window.** Qt's `offscreen` platform gives the GS no surface, so a window opens on the desktop (XWayland, `HK_QPA` to change).
- **Stock Reliquary prints the derived MagicGate keystore.** The patch stops that; `run.py` also strips it from the logs it keeps. Never commit or share raw logs of an unpatched build.
- **Inputs:** `pcsx2-qt` takes no scripted inputs. Scripts run in the core (`build/cmake/hiraku-popn9 --script oracle/scripts/popn9-play.his`).

## The pinned Reliquary, and updating it

HIRAKU is built and checked against **PCSX2 Reliquary 1.9.4**, the release zip `pcsx2-reliquary-1.9.4-reliquary.zip`, SHA-256 `da9b9a601e8b3eeede9eb8bae6d46e81e1001beb98b438859aa847c75b547884`. Reliquary re-syncs with upstream PCSX2 about monthly, and any update can change what the game does frame by frame, so an update is a deliberate step:
1. **Unpack the new release** next to the old one, and point `HK_PYTHON_WORK` at a copy of the work folder, so the pinned build stays usable.
2. **Apply the patch:** `build-reliquary.sh patch`.
   - If a hunk fails, re-apply it by hand. The patch is only taps and hooks (`HirakuHooks.h`) and the keystore guard, so this means finding the same spots in the new files.
   - Then regenerate `reliquary-oracle.patch` from the new tree.
3. **Build:** `configure`, then `build`.
4. **Check the core against the new oracle:**
   - `run.py popn9 --frames 3000 --snap-every 0 --unlimited`;
   - `hiraku-popn9 --frames 3000 --bare-board` with `HK_FRAMEHASH`;
   - the two hash files must be identical. `--bare-board` leaves out HIRAKU's e-AMUSEMENT card unit and network, which plain Reliquary doesn't have: pop'n 13 probes the card unit at frame 1,325, and the hashes part there without it. The same check passes for pop'n 10–14.
5. **Run `tools/regress.py check popn9-play`.**
   - If it still passes, the update changed nothing that pop'n 9 shows.
   - If it differs, look at what changed (pictures, sound, the frame it starts at) before re-recording the baseline. Note the reason in PLAN.md §23.
6. **Update the pinned version and SHA-256 here.**
