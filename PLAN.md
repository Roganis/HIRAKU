# HIRAKU — the plan

HIRAKU runs Konami's BEMANI arcade games natively. Firebeat programs are recompiled to C and run on
HIRAKU's own runtime; DJ Main, System 573 and Twinkle are emulated in cycle-exact lockstep with MAME;
the Konami Python 1/2 games run on a built-in PCSX2 (Reliquary); the Windows-based games run as they
are, with HIRAKU's cabinet DLLs around them (under Wine on Linux). The player supplies their own
dumps: the repository never holds ROMs, programs, assets or code generated from a game.

**How to read this file.**
- Sections keep their numbers, because the code cites them ("PLAN.md section 23"). New work gets a
  new section at the end.
- Each section says what exists now and the decisions worth keeping, then ends with **Open**: one
  line per item, tagged **(build)** still to make, **(owner)** built but waiting on the owner's check
  (a real cabinet, display, controller or Windows PC), **(parked)** put off, optional or out of
  scope on purpose. `grep "(owner)" PLAN.md` lists everything waiting on a check.
- The full text up to 2026-11-14 (progress logs, measurements, dead ends, addresses) is frozen in
  [docs/PLAN-HISTORY.md](docs/PLAN-HISTORY.md), with the same section numbers. How-tos are in
  `docs/` (PLAY, MODS, SMOOTH, EAMUSE3, CATALOGUE, PYTHON...).
- Keep it this way: a section here stays short; when something is done, take it out of **Open**
  and say it in a sentence; detail goes in `docs/` or in the commit.

## Where things stand (2026-11-14)

| Platform | Games | Where it stands | § |
|---|---|---|---|
| Firebeat | beatmania III, pop'n music 4–8, Animelo, Mickey Tunes, ParaParaParadise, Keyboardmania | recompiled; all boot to attract with sound, most play; hands-on play and N100 timings open | §0–§18, §39, §41 |
| DJ Main | beatmania, pop'n music 1, pop'n stage | all 25 sets on hand cycle-exact with MAME | §17 |
| System 573 | DDR, GuitarFreaks, DrumMania, Dance Maniax, Mambo a GoGo | 90 sets in lockstep with MAME, MIPS recompiler, e-AMUSEMENT, session play | §19, §24 |
| Twinkle | beatmania IIDX 1st–8th style | all 18 sets in lockstep, with the background videos MAME lacks | §21 |
| Konami Python 1/2 | pop'n music 9–14, DrumMania V/V2 | play with e-AMUSEMENT and NET TAISEN; GuitarFreaks V2 open | §23 |
| PC, 32-bit | pop'n music 15–18, `popn22.dll`; beatmania IIDX 9th style to EMPRESS | play under Wine with e-AMUSEMENT, several also on real Windows; pop'n 19–21 not started | §30, §33, §35, §36 |
| PC, 64-bit | the 64-bit PC platform | plays under Wine, on one 4:3 screen too; songs back from the music data | §32, §34 |
| Everything | mods, high frame rates, shaders, Windows, releases, cabinet hardware, fast start-up, overlay, game menu, tests | | §20, §25–§29, §31, §37–§44 |

The biggest open pieces: pop'n music 19–21 (§30); GuitarFreaks V2 and its session (§23); a song
added to pop'n 14 (M17, §5); the BI2X board's ACIO2 (§25); the e-AMUSEMENT calls still answered
empty (rankings, rivals, KONAMI ID: §19, §33–§36); frame times on an N100-class PC (§14, §40, §41);
and the owner's checks on real hardware and real Windows.

---

## 0. Why recomp

A Firebeat game is its PowerPC program translated to C ahead of time and linked against one shared runtime that stands in for the board. A native rewrite of each game's flow, judge and scoring (the earlier "Route B", rizu) scales to one game, not about 25; a recomp reuses one runtime, costs each new game a config file and its quirks, and keeps the original game logic exactly. Route B was dropped (§8); the earlier beatmania III format and BGA work, now in the EZ2 port, serves as an oracle.

Full history: docs/PLAN-HISTORY.md §0.

## 1. What the binary tells us (beatmania III The Final, `GCC01`)

Measured with headless Ghidra on 2026-10-11; it holds for every Firebeat program.

| Fact | Value | Consequence |
|---|---|---|
| Image | 1 MiB flat at `0x80000000`, mirrored at `0x0`, entry `0x80003000` | No loader: the unpacked program loads as it is, the BIOS skipped. |
| Functions | 2,204, covering 137,352 of 138,432 decoded instructions | Small for a recomp. |
| FPU | none (soft-float) | No float semantics to match. |
| `bctr` / `blrl` | 37, all jump tables / 149 through pointer tables (e.g. the BGA VM's at `0x800E560C`) | Control flow recoverable; a guest→host table with an interpreter fallback (§3.4). |
| `rfi`, `rfci`, `sc` | 4, 1, 2, all in hand-written assembly | The glue of a preemptive C kernel: glue replaced, kernel translated (§3.3). |
| Privileged surface | DMA ch0–3, EXIER/EXISR/IOCR, PIT, TCR/TSR, TB, SRR0/1, SPRG2/3 | Small and fully known. |
| Self-modifying code | none seen | The fallback interpreter would log it; it never has. |

The sound board's 68000 (512 KB program per SPU revision) is interpreted.

Full history: docs/PLAN-HISTORY.md §1.

## 2. Platform model

The runtime replaces what MAME's `firebeat.cpp` emulates: a PPC403GCX at 66 MHz with 32 MB RAM, one or two K057714 GCUs, ATA/ATAPI, a DPRAM mailbox to the sound board, I/O and lamps, the DS1991 dongle, RTC/NVRAM, flash, UARTs, and the extend board (spectrum analyser, floppy, MIDI). Interrupts: EXT0 GCU vblank (~59.94 Hz), EXT1 extend board, EXT3 SPU mailbox, EXT4 main ATA, plus the 403's PIT and FIT.

| Family | Audio | Storage | Extra |
|---|---|---|---|
| beatmania III (5 versions) | SPU: 68000 + RF5C400 | program CD + HDD on the SPU's ATA | extend board, ST-224 effector over MIDI, turntable sensors |
| pop'n 4–8, Animelo 1/2, Mickey Tunes | SPU; BGM streamed by DMA keyed on RF5C400 reg 09 | program CD + DVD on the SPU's ATA | 9 buttons, lamps |
| ParaParaParadise family | YMZ280B (samples in flash) + CDDA | 2 CDs | IR sensor bars; `!:EXE:00` wrapper |
| Keyboardmania 1st–3rd, Keyboardheaven | YMZ280B + CDDA + XT446 synth card (H8S/2655 + SWP30) | 2 CDs | 2 GCUs and screens (ROT270), MIDI keyboards |

MAME's hand-tuned parts (the SPU's 500 Hz tick and 350 ns DMA word, RF5C400 effects, the GCU's constant status and per-game fixups) are hazards for HIRAKU too. The ST-224 isn't emulated.

Full history: docs/PLAN-HISTORY.md §2.

## 3. Architecture

**Layout.** `analyze/` (Ghidra headless + Python → a per-game manifest), `recomp/` (the PPC403 → C translator), `runtime/` (`core/` memory, context, MMIO bus, indirect-call table, interpreter; `kernel/` exception glue, IRQs, 403 timers and DMA; `dev/` devices; `host/` window, input, sound; `shell/` and one directory per core since §17), `launcher/` (Qt 6), `games/<set>/game.toml`, `oracle/`, `tools/`. `dumps/` and `build/` are git-ignored: no ROMs, programs, assets or generated C are ever committed. C++20, generated C99, Python, Lua for MAME, CMake.

**Recompiler** (docs/RECOMP.md). Ghidra discovery plus HIRAKU's own checks give functions, blocks, jump tables and code-pointer candidates. HIRAKU's own decoder and emitter (not Ghidra's decompiler) make one C function per guest function: registers in `Ctx`, branches as `goto`, `bl` a C call, a known `bctr` table a `switch`. Guest RAM stays big-endian, every access swapped; RAM has a fast path, `0x7xxxxxxx` goes through the MMIO bus. `blrl` looks the target up in a sorted table; a miss runs in the interpreter and is logged. Back-edges and prologues poll `ctx->irq_pending`, so interrupts land at deterministic points.

**Kernel** (docs/KERNEL.md). The program carries a small preemptive kernel in C: user-mode threads, one generic system call, a switch by pointing SPRG3 at another thread's saved 384-byte frame before `rfi`. Its C is recompiled like the rest, so scheduling stays authentic; only ~300 instructions of assembly glue are host code (`runtime/core/glue.cpp`), building the same frames in guest RAM. Each guest thread is a host fiber, switched when SPRG3 names another frame on the way out of an exception. Interrupts are taken only in user code; kernel functions are emitted without polls. The 403 is emulated exactly (DCRs, 4 DMA channels, PIT/FIT/WDT, TB, SPRGs); cache operations are no-ops.

**Interpreter and shadow mode.** The embedded PPC403 interpreter runs what the static pass missed (logged), checks chosen functions both ways on cloned contexts (`--shadow`), and runs a whole game as a plain emulator (`--interp-only`).

**Devices.** MAME's BSD-3 devices as plain C++ classes, so games see the same registers: the K057714 as a software renderer; the SPU (Musashi running the dumped program, RF5C400, MB8421 mailbox, wave RAM, an image-backed SPU-ATA); YMZ280B and CDDA; ATA/ATAPI over CHD/ISO/raw images; DS1991, RTC65271, 29F016A flash, PC16552 UART, the spectrum analyser, the floppy (§39).

**Host.** SDL3 window, controllers and sound; per-family control maps (Keyboardmania's MIDI keyboards, with a SoundFont stand-in when the XT446 ROMs are missing); direct ALSA, WASAPI and ASIO sound back ends (`runtime/host/`); NVRAM, RTC and flash kept per game. Linux first, Windows second (§26).

**Open**
- (parked) Starting keysounds straight from the PPC side (an HLE shortcut for latency): only if the register-level path proves too slow.
- (parked) ParaPara's sensors by camera or IR: keys and controllers are mapped; real sensors are a later extra.

Full history: docs/PLAN-HISTORY.md §3.

## 4. Validation strategy

The method every core still follows:
1. **Unit:** every opcode form, interpreter against emitted C against QEMU (`ctest`, no game data).
2. **Shadow execution** on real game code (§3).
3. **The MAME oracle** (`oracle/README.md`): Lua taps log every MMIO access by frame; frame-keyed `.his` scripts run in both MAME and HIRAKU; RAM, the GCU command stream, mailbox traffic and pictures are compared (`compare.py`, `align.py`, `snapcmp.py`; `audiosync.py` for sound, §18). The emulated cores (DJ Main, 573, Twinkle) go to cycle-exact lockstep (`oracle/lockstep.sh`).
4. **Independent oracles:** the BGA VM and its scene self-test; the extracted charts and keysounds.
5. **Coverage:** no fallback-interpreter entries over a full attract-and-play loop (met, §12).

Every speed change is checked bit-identical by `tools/regress.py` (§14).

Full history: docs/PLAN-HISTORY.md §4.

## 5. Milestones

Each milestone's exit test is in the archive. M0–M5, the first plan, were estimated at four months and were all done on 2026-10-11. Status checked against the repository on 2026-11-14; the open items of M14–M22 are in their own sections.

| # | Milestone | Status | Section |
|---|---|---|---|
| M0 | Infrastructure: repo, ingest, Ghidra manifest, MAME oracle | done | §9 |
| M1 | Translator + interpreter | done | §9, docs/RECOMP.md |
| M2 | Kernel and platform: BM3's STARTUP self-test | done | §10, docs/M2.md |
| M3 | Attract and BGA against MAME, no fallbacks | done | §12, docs/M3.md |
| M4 | Audio: SPU, keysounds, BGM, spectrum analyser | done | §11, docs/M4.md |
| M5 | beatmania III The Final playable, NVRAM kept, latency measured | done; hands-on test open | §13, docs/M5.md |
| M6 | The rest of beatmania III (bm3, core, 6th, 7th) | done 2026-10-14: all play the practice stage; attract 5–8 frames early against MAME | §18, §39 |
| M7 | pop'n 4–8, Animelo 1/2, Mickey Tunes | all boot to attract with sound, 7 and 8 play songs; BGM sample-exact within a piece | §18, §20 |
| M8 | ParaParaParadise family | 1st Mix Plus, `ppp`, `ppp11`, `ppd` play; sensor bits as MAME's | §18 |
| M9 | Native polish: launcher, sound back ends, picture, Windows | done; carried on in later sections | §26–§29, §38, §42, §43 |
| M10 | Keyboardmania (stretch) | all four boot with the real XT446; kbm and Keyboardheaven play a song by their own autoplay | §14 |
| M11 | Efficiency for N100-class PCs | done on the desktop (bm3final ~3.4× MAME); never measured on an N100 | §14, §40, §41 |
| M12 | The core split (`runtime/shell/core.h`) | done 2026-10-12 | §17 |
| M13 | DJ Main core | done: all 25 sets on hand cycle-exact with MAME (2026-10-16) | §17 |
| M14 | System 573 core | done for the media on hand: 90 manifests, analog and digital boards in lockstep with MAME, MIPS recompiler, e-AMUSEMENT, sessions | §19, docs/CATALOGUE.md |
| M15 | Twinkle core (IIDX 1st–8th) | done, all 18 sets: lockstep with MAME, the background videos MAME lacks | §21 |
| M16 | Konami Python 1/2 | pop'n 9–14 play with e-AMUSEMENT and NET TAISEN; DrumMania V/V2 run | §23 |
| M17 | A new song added to pop'n 14 by a mod | not started | §20 |
| M18 | Shaders: Lola Retro, the CRT beam | done 2026-10-20, owner-checked at 240 Hz | §29 |
| M19 | PC pop'n 15–18 (`hiraku-pc`) | done under Wine 2026-10-23 | §30 |
| M20 | The 64-bit PC platform | plays under Wine, on one 4:3 screen too; songs back from the music data | §32, §34 |
| M21 | beatmania IIDX 9th style (C02) | plays with movies and e-AMUSEMENT under Wine; 10th to EMPRESS followed | §33, §36 |
| M22 | The last 32-bit PC pop'n (`popn22.dll`) | steps 1–6 done 2026-10-26 | §35 |

Later work has no milestone number: high frame rates (§37), fast start-up (§38), beatmania III's floppy (§39), release speed (§40, §41), the performance overlay (§42), the game menu (§43), tests (§44).

**Open**
- (owner) M5–M8: hands-on play of the Firebeat games with real controllers and sound hardware.
- (build) M7: the SPU's DVD drive answers at once; modelling its read times is the next suspect for pop'n 7's second piece of music starting 5 ms late (§18).
- (build) M7: `popnmt` (Mickey Tunes, not the `!`): its program CD is in the owner's archive (docs/CATALOGUE.md, B) but not ingested; a new program CD is a new recompile.
- (parked) M8: `ppp2nd` is other hardware, out of scope.
- (parked) M10: autoplay in Keyboardmania 2nd and 3rd Mix needs a hook at a code address, which scripts lack (their demo's switch must be set when play starts).
- (parked) M10/M11: the XT446's H8, ~55 % of the card: see §14.
- (owner) M11: frame times on a real N100-class PC; none on hand.
- (owner) M13: hands-on play of DJ Main with a turntable and pop'n buttons.
- (parked) M13: `bscompmx` and `popn1`: no media.
- (build) M17: all of it: the song list checksum's coverage, the chart format, what else counts the songs.

Full history: docs/PLAN-HISTORY.md §5.

## 6. Risks

All but the legal one are retired: the kernel glue was read and replaced (§3); no fallback entries over a full session (§12); the SPU's timing and pop'n's streaming match MAME (§18); the glue is identical in every Firebeat program and every one recompiles; the XT446 runs MAME's H8S/2655 + SWP30 on the shim, pinned to a MAME master newer than 0.289 for the SWP30's fixes. The GCU keeps MAME's behaviour and workarounds. **Legal:** only the tool and runtime ship; users bring their dumps; no generated C, assets or ROMs in the repo; MAME-derived code keeps its BSD-3 notices; check each borrowed core's licence before use.

Full history: docs/PLAN-HISTORY.md §6.

## 7. Housekeeping found during the survey

Notes on the owner's earlier beatmania III work outside this repository: its BGA VM notes read the dispatch table 5 entries late (it starts at `0x800E560C`; the EZ2 port has it right); its "judge" files are BGA hit/miss hooks, not the gameplay judge; its MAME `patch.lua` uses 1st mix offsets.

Full history: docs/PLAN-HISTORY.md §7.

## 8. Decisions (settled 2026-10-11)

1. **Order:** beatmania III The Final → the other beatmania III → pop'n → ParaPara; Keyboardmania a stretch goal.
2. **Fidelity:** match MAME first, enhance later. HLE shortcuts only after the register-faithful path works and is checked against the oracle; a departure from MAME is a switch, MAME's behaviour kept for the oracle.
3. **Platform:** Linux first, Windows later.
4. **Licence:** GPL-3.0-or-later; MAME-derived files keep their BSD-3 notices.
5. **rizu / Route B is dropped.** Bugs found in other implementations go to the EZ2 port.

Full history: docs/PLAN-HISTORY.md §8.

## 9. M0 and M1 outcomes (2026-10-11)

**M0.** `tools/ingest.py` takes a CD image to the program (ISO9660 → `HIKARU.EXE`/`FIREBEAT.EXE` → ParaPara's wrapper → LZSS unpack) and identifies it by SHA-1 against `games/*/game.toml`. `analyze/ghidra.sh` + `ExportManifest.java` write `build/<game>/manifest.json` (never committed). The oracle: MMIO taps, frame-keyed `.his` scripts, `trace.py`, `compare.py`, `EXACT_PC=1`, `WAV=1`; MAME runs are deterministic but for the RTC seeded from the host clock. On pop'n 4–7 the assembly glue is identical and the BM3 C kernel matches pop'n 5–7 at 115 of 118 unique functions (`analyze/sigmatch.py`); pop'n 4's kernel is compiled differently.

**M1.** One header, `include/hiraku/ppc.h`, defines every instruction for the interpreter and the generated code; 21,600 QEMU-referenced cases over 108 forms pass. bm3final and pop'n 4–7 translate (2,168–2,487 functions) once GCC's return-up epilogue helpers and branches into mid-function are handled.

**Open**
- (parked) docs/KERNEL.md's open items (the kernel C's exact extent, the thread and semaphore API, EXISR bits, PIT/FIT rates): every Firebeat game runs without them.

Full history: docs/PLAN-HISTORY.md §9 and "M0 steps".

## 10. M2 outcome (2026-10-11)

Native code runs the real game on host fibers: BM3 The Final boots BIOS-less into its multithreaded kernel and passes its STARTUP self-test on screen. Its MMIO stream matched MAME's record for record (`oracle/align.py`) up to the first SPU access, and shadow mode was clean. The vectors, interpreted at first, are recompiled since M5. Details: docs/M2.md.

Full history: docs/PLAN-HISTORY.md §10.

## 11. M4 outcome (2026-10-11)

The SPU works (Musashi running Konami's sound program, RF5C400, mailbox, the SPU's HDD feeding wave RAM by DMA, the spectrum analyser): The Final plays a song with sound, at 0 ms lag to MAME. The skipped BIOS time must still pass for the other boards: `--bios-seconds` (default 21.65). Details: docs/M4.md.

Full history: docs/PLAN-HISTORY.md §11.

## 12. M3 outcome (2026-10-11)

The GCU renderer is bit-exact against MAME on 28/28 frames of replayed GCU writes; the BGA engine runs in lockstep with MAME (98.8 % of song frames identical, a constant 13-frame offset); no interpreter fallbacks over a full session. Details: docs/M3.md.

Full history: docs/PLAN-HISTORY.md §12.

## 13. M5 outcome (2026-10-11)

The full loop (attract, coins, stages, results, game over) runs natively, paced on the sound clock, NVRAM kept. The game's own key-to-keysound latency is 2–20 ms, mean 9.5: it scans its keys at 60 Hz as on the cabinet (docs/PLAY.md, "Latency"). Details: docs/M5.md.

**Open**
- (owner) A hands-on test with real sound hardware and controllers.

Full history: docs/PLAN-HISTORY.md §13.

## 14. Efficiency on N100-class PCs (2026-10-12)

**Goal (owner):** beat MAME on efficiency, latency and I/O as well as accuracy. The target is an N100-class mini PC (4 cores, ~60 % of a desktop core, 6 MB L3): judge by worst frames there, not desktop averages. Budget: under 25 % of one core on average, no frame over ~8 ms of host work.

**Tools:** `tools/regress.py record|check|bench` (now over 160 cases, every core) compares each frame's picture hash, a running sound hash and the run's statistics with a baseline: every speed change must be bit-identical. Every run prints `frametime:` (mean to max host work a frame). Regress snapshots are disk writes: lone 20–200 ms frames in `bench` are those.

**Firebeat:** bm3final gameplay 17.0 → 9.6 % of a desktop core, its attract about 3.4× MAME: a small x86-64 fiber switch (not `swapcontext`), GCU rows instead of pixels, one `pread` per stretch of disk with an 8 MB read-ahead hint (a cold-cache worst frame 251 → 9.8 ms). Idle skipping in recompiled code was not done: the idle time is kernel `yield()` calls, and skipping them changes kernel state unless the scheduler is proven periodic (the release's executor remembers those runs instead, §41). The MAME-device cores run MAME's own code, so they are level with MAME by design (ddr2m 1.05×, bm1stmix 0.98×).

**Builds:** clang, with profile-guided optimisation (`tools/pgo.py`): one profile per core in `build/pgo/` (never committed), because the cores build the same sources into separate copies (Musashi as 68000 and 68EC020, MAME's shim for the XT446 and the 573) and one blended profile made DJ Main and ddr2m slower. Measured: kbm −19 %, ddr2m −27 %, popn7 −3 %, bm3final −1 %. Not profiled: Firebeat's recompiled game code (no gain) and DJ Main (5 % slower with one).

**Keyboardmania's XT446**, the heaviest case: the card runs on its own thread (`runtime/firebeat/synth.cpp`, `HK_XT446_INLINE=1` for the old way), bit-identical. Its MEG effect programs are compiled ahead of time from recordings (`tools/meggen.py`; others stay interpreted): kbm attract 32.4 → 25.6 % of a core, 4.4 ms a frame mean. The H8 firmware is ~55 % of the card; per-address handlers gave nothing (2026-10-19).

**Waits and lamps (2026-11-12):** where the sound paces the game, the wait ends at the device's pull (`hk::sys::Wake`, `shell/sys.h`) instead of polling (ddr2m's game thread −31 % CPU); cores with timed inputs (the PC games) keep the 0.25 ms poll. Lamps go out as soon as the frame is emulated, before the wait.

**Open**
- (owner) Measure on a real N100-class PC, in `--window`, with `HK_LOG_STALLS=1`.
- (parked) The XT446's H8: a gain needs a recompiler that folds MAME's cycle checks, or direct work-RAM access; on its own thread it isn't urgent.
- (owner) Lamps sent before the wait: not measured on a cabinet. PC smooth motion's naps: not checked on a faster display or on Windows.

Full history: docs/PLAN-HISTORY.md §14.

## 15. Other BEMANI platforms as cores (2026-10-12)

One shell (launcher, `host/`, scripts, the oracle, lamps and controls) and several machine cores behind one interface: built in §17. DJ Main (68EC020, K056832/K055555, two K054539s, an ATA disk) works in MAME for every set and is interpreted. System 573 (a PS1 with CD, flash, cassettes, and the digital board's MP3) mostly doesn't work in MAME on the digital board; interpreted first, its MIPS then recompiled (§19). Twinkle is a PS1 plus Firebeat's sound board, so the 573 core covers most of it (§21). **Licences:** DuckStation's (non-commercial, no derivatives) is incompatible; MAME's PS1 devices (BSD-3), Beetle PSX and pcsx-redux (GPL-2+) are compatible.

Full history: docs/PLAN-HISTORY.md §15.

## 16. DJ Main, System 573 and Twinkle media (2026-10-12)

The first sets were the owner's non-merged MAME sets, all passing `-verifyroms` (MAME flags the 573 digital sets' cassette chips BAD_DUMP, as every copy is). In MAME, DJ Main costs ~5 % of a desktop core (a core is worth it for latency and I/O, not speed); 573 and Twinkle 15–18 % (~30 % of an N100 core: speed matters). Every new platform needs its first-boot steps; the oracle keeps a seeded NVRAM per game. The full catalogue is docs/CATALOGUE.md.

Full history: docs/PLAN-HISTORY.md §16.

## 17. The core split and the DJ Main core (2026-10-12)

**M12:** `runtime/shell/core.h` is the machine interface (init, run, frame and sound hooks, inputs by MAME name, lamps, guest memory for scripts, statistics); `runtime/main.cpp` the shell, `runtime/<core>/core.cpp` the cores, `runtime/shell/cabinet.cpp` the cabinets.

**M13** (`runtime/djmain/`): Musashi as a 68EC020, MAME's map, K056832 (a tile at a time since ba60888), K055555, two K054539s, the shared ATA code with MAME's drive model and password lock. It runs in **cycle-exact lockstep with MAME** (`tools/regress.py`'s DJ Main cases): work RAM, sprites and palette identical every frame, disk commands on the same clock, sound sample-identical, through boot and scripted games; all 25 sets on hand since 2026-10-16 (`[hdd] chs` gives each disk's geometry).

What it took, found by cycle-stamped traces of both CPUs (`HK_DJ_ITRACE`, `HIRAKU_ITRACE_CYCLES`): Musashi's EC020 with `M68K_EMULATE_010` on; MAME's 68020 costs (no per-bit shift or rotate cost, MOVEM 3 a register, TRAP 24, the reset's 4, the slice checked before the first instruction: opt-in hooks, `third_party/musashi/HIRAKU.md`); the slice ended when an interrupt line changes; MAME's drive (seek from the head, IDX once a rotation, no IRQ at a PIO read's end: `AtaBus::pio_end_irq`); the K054539 status moving only at MAME's stream updates (`commit()`); a script's buttons a frame late as MAME's Lua (`Core::script_button_delay()`); 48 kHz sound handed on as it is (`Core::audio_rate()`).

**Open**

Full history: docs/PLAN-HISTORY.md §17.

## 18. Firebeat music against MAME (2026-10-12 to 10-15)

Firebeat runs recompiled code, so it can't be in lockstep with MAME; what is checked is that the music keeps time. `oracle/audiosync.py HIRAKU.wav MAME.wav` finds the offset from the envelopes, refines it on the waveform, and reports lag, correlation and level in 2 s windows. pop'n 4–8 and Animelo's streamed BGM is sample-exact with MAME within each piece; ParaParaParadise's YMZ280B music has lag 0 and the same level below 4 kHz (HIRAKU halves 88.2 kHz by averaging pairs, so it has more above 8 kHz). The flash chip takes MAME's erase times.

**CPU timing** (what put the attract 22–95 frames behind MAME): each instruction is charged MAME's PPC403 cost (`src/ppc/cycles.h`; the recompiler per straight-line run, `HK_TICK`); a recompiled vector's `rfi` takes a pending interrupt at once (`run_vector`); periodic timers re-arm from their due tick (`Timer::again`); the 403's memory-to-memory DMA costs nothing, as in MAME. Now the beatmania III attract is 5–8 frames early, and pop'n 7's 1.5 s load takes 1,531.8 ms (MAME 1,531.9).

**Open**
- (build) pop'n 7's second piece of music starts 5 ms late (pop'n 4–6 and Animelo not measured again since the DMA fix): next, the DVD drive's read times.
- (build) pop'n 4 after 82 s and Animelo 1 at 88–100 s take a different attract path from MAME's; not looked into.
- (build) Gameplay, not only attract, against MAME's music.
- (parked) A sharper 88.2 → 44.1 kHz filter for the YMZ280B: it changes the digests, so it's the owner's choice.

Full history: docs/PLAN-HISTORY.md §18.

## 19. System 573 (M14, 2026-10-12)

The System 573 core (`runtime/sys573/`, `core = "sys573"`) is MAME's PS1 and 573 devices on the
framework shim (CPU, GTE, GPU, SPU, MDEC, flash, M48T58, ADC0834, CR-589 ATAPI, cassettes, PC Card
slots, the analog and digital I/O boards with FPGA and MAS3507D), the R3000 interpreted and matched
with MAME cycle for cycle; `third_party/mame/README.md` lists every change to MAME's files. There
are 90 manifests: the analog-board games, the DDR digital line to EXTREME with Solo, GuitarFreaks
3rd–11th and DrumMania 2nd–10th with their sessions, Dance Maniax and Mambo a GoGo. Almost all run
in lockstep with MAME at the pin from the first-boot install to a song; the per-set record is
`docs/CATALOGUE.md` (§24). How to play them: `docs/PLAY.md`.

**Decisions (owner, 2026-10-12).** The first set was `ddr2m`, which MAME runs. Where MAME doesn't
support a game, go as far as the documentation allows (psx-spx, the 573 hardware notes, the chips'
datasheets): each departure is a switch, MAME's behaviour stays for the oracle, the source of each
departure is written down, and where the documentation runs out the gap is recorded.

**MAME's timing, which the core keeps.** One CPU cycle per instruction with no wait states or cache
cost; a CD sector costs a fixed 30,000 CPU cycles; the digital board's MP3 counter counts machine
time from the first frame sync at 44.1 kHz. The MP3 decoders ask the FPGA for data as their 50 Hz
updates run, so they are never run from the host side (that would change the machine);
`--fine-sound` (now the default, `--mame-sound` for MAME's) predicts their output ahead (495f785).

**The oracle.** `oracle/mame.sh` (`MAME_BIN`, memory cap `MAME_MEM`), `oracle/lua/hiraku.lua` (573
taps; the M48T58's power-on time, which HIRAKU takes with `--rtc N`), `oracle/lua/itrace573.lua`
beside `HK_573_ITRACE`, and `oracle/lockstep.sh`. The shim's exact mode copies MAME's
`device_scheduler`. First-boot installs use the install cassette (`--cassette install`, the
launcher's first boot) or an NVRAM seed; regress cases can start from another case's NVRAM.

**Beyond MAME (departures, each a switch or limited to the games that have the hardware):**
- The multi-session unit (GXA25), ported from windyfairy's MAME fork (`bemani_k573msu_wip`, BSD-3):
  the TX3927 in MAME's `mips1`, `k573msu`, `tc9446f`, SIO1 as a UART with the PS1's 8-byte receive
  FIFO. Fixes the fork lacked: the shim's `umask`, MAME's ATAPI chunk rounding, the 16550's lost
  full TX FIFO, TX3927 caches, SIO1's DSR kept for the cassette. It runs at the fork's 25 MHz;
  `HK_MSU_132=1` runs the real 132 MHz, which changes nothing in the game and costs ~7 ms a frame.
  drmn10m with the unit: 2.8 ms a frame on the desktop.
- The session cable (`runtime/sys573/link.{h,cpp}`, `--session-host` / `--session-join`): line
  levels stamped with emulated time, TXD once a bit, lockstep every 500 µs (`--session-quantum`),
  over a Unix socket or TCP, so a session replays the same on any host. A host with a unit sends
  its mix to the other side. Which unit decoder feeds which cabinet isn't known: both get the whole
  mix.
- e-AMUSEMENT for DrumMania 10th / GuitarFreaks 11th: the network unit as HLE (`npu.{h,cpp}`, after
  windyfairy's `npu_hle`; its disk is the save folder's `npu_hdd/`; `--npu`, `--cd hdd` for the
  installer), the GF/DM server (`eagfdm.{h,cpp}` on `runtime/eamuse/`; `--eamuse local|URL`, or
  `hiraku-eamuse` for several cabinets), and the card units on SIO1, switched with the
  multi-session unit by the cassette's d7 (`cardunit.{h,cpp}`). The skill total (best 25 songs) is
  an assumption; the game has no formula.
- `gtrfrk9m` only: the ATAPI DMA re-arm waits for a DMA stopped inside a block (3c1ddf3; MAME has
  the same bug).
- `mamboagga`: `--rental` and `--master-calendar AREA`; the replenishment codes are not built, on
  purpose (§24).

**Speed (573-4).** The CPU was ~80 % of a 573 frame. Measures, all bit-identical in regress:
- Profile-guided builds (`tools/pgo.py`): 1.44×; `-O3` is the default (CMakeLists cites this).
  `-march=x86-64-v3` added nothing.
- The R3000 recompiler: `runtime/sys573/mips.h`, `mips_ops.h`, `tools/mipsgen.py`,
  `tools/mipsrec.py`. Blocks come from a recording (`HK_573_RECORD=build/<game>/mips573.txt`),
  so coverage is what was recorded. Loop regions and superblocks; pages holding checked code are
  write-protected; unseen code is interpreted. ddr2m attract: 1.85 → 0.36 ms a frame.
- The GPU draws on a worker thread (`HK_GPU_THREAD=0` to turn it off); the screen update waits only
  for queued drawing that can touch what it shows (5094728).
- Waiting loops are counted instead of run (`runtime/sys573/idle.cpp`, §40).
- Measured and dropped: a cached fetch page (6 % slower), a row-wise frame copy, computed pens.

A release holds no recordings (§28), so it runs the interpreter: DDR 2ndMIX ~1.3 ms a frame in a
release-like build (§40). Only ddr2m, ddr3mj, drmn10m and gtfrk11m have recordings in local builds.
The budget, as for M11: under 25 % of an N100 core on average, no frame over ~8 ms of host work.
`HK_FRAMETIMES=file` and `HK_PROFILE_AFTER=ms` measure.

Full history: docs/PLAN-HISTORY.md §19.

**Open**
- (owner) Hands-on play: DDR with real pads, a DrumMania × GuitarFreaks session, GuitarFreaks with both card readers.
- (owner) Frame times on N100-class hardware, the 573-4 exit; never measured there.
- (owner) A session between two machines over TCP: checked only over loopback (two Wine processes, 2026-10-17, §26).
- (build) GF/DM e-AMUSEMENT: a cleared stage checked in the game, the shop ranking, the internet ranking (`ir`, `irdata`), the land event, KONAMI ID (`userid.ctou` answers 112).
- (build) Build `oracle/mame-patches/ksys573-dma.patch` into a MAME to check `gtrfrk9m`'s departure against it.
- (parked) More recordings per game: only local builds use them; releases run the interpreter.
- (parked) The unit at its real 132 MHz: costs ~7 ms a frame and changes nothing in the game.
- (parked) PCMCIA edit-data cards and the memory-card reader as player features: only if wanted.
- (parked) Karaoke (`k573kara`) and the non-BEMANI 573 games: out of scope unless asked.

## 20. Modding, unlocks and display pacing

Discussed 2026-10-12 with pop'n music 8 as the first pilot; the mods foundation followed on
2026-10-19 (b318a16) with pop'n music 14 as the pilot. Mods are layers over the MAME-exact game:
off by default, the regression cases always run without them, and a mod set gets its own save
folder (`<game>+<mod>...`, started from a copy of the clean one). The section also holds the
unlock tools and display pacing, which grew up beside the mods.

### Mods

A mod is a folder with a `mod.toml` (`[[patch]]`, `[[bytes]]`, `[[file]]`, `[[text]]`, `[[rate]]`)
under `<saves>/mods/<game>/`, given with `--mod FOLDER` or the launcher's Mods… dialog.
- **Memory** (`runtime/host/mods.cpp`): a patch with `original` waits for its bytes (code loaded
  later); without it, it is written every frame like a cheat. Each core takes code patches its
  own way (docs/MODS.md, "Per core"); Firebeat runs a patched function interpreted from its next
  call (`runtime/core/patched.cpp`, 2026-10-20).
- **Media** (`runtime/shell/overlay.{h,cpp}`): byte ranges replaced as an image is read, in every
  core's image layer and in Reliquary through `Hiraku::disk_read`; `[[file]]` finds a file on an
  ISO 9660 disc, or by name on pop'n music 14's disk (`runtime/host/pythondisk.{h,cpp}`; the file
  system in `tools/popnfs.py`, b588cb4). PC games take whole files at their own paths.
- **Cheats:** `tools/modimport.py` turns `.pnach` and MAME `cheat.xml` plain writes into mods.
- **High frame rate** (`[[rate]]`, `between_ticks`): for games that time their charts by a clock
  (pop'n 9–14 on Python, the PC games) the vsync runs at the display's rate during the song and
  frame counters are held to the cabinet's pace. `smooth-notes` mods for pop'n 9–18; the owner
  played pop'n 14 at 240 Hz (2026-10-19, fixes to v0.4 and e65bbdf). Only the Python, DJ Main and
  PC cores change their rate (`Core::set_vsync_rate`); the games on DJ Main, Firebeat, 573 and
  Twinkle count their play in frames, so they get in-between pictures instead (below).
  docs/MODS.md.
- **pop'n 14's songs** sit in 20 MB slots after the file system, scrambled per slot
  (`tools/popnsong.py` reads them, c38276d). Adding a song is milestone M17.

### Unlocks and code macros

`games/<id>/unlocks.toml` describes a game's unlocks and `tools/unlock.py` writes them into the
save (with a backup); the launcher's Unlocks page drives it. Each tool-made save was checked
byte-identical to the one the game writes when the codes are typed (`oracle/scripts/*-phases.his`,
`-operator`, `-unlock`). Done 2026-10-12: pop'n 8 (event phase; Sugoroku de 8 decoded and played
to the board with autoplayed credits, `[[progress]]` in Unlocks), pop'n 7 (operator codes and
BINGO de 7!!, checked with an autoplayed credit), pop'n 6 (operator switches), beatmania III THE
FINAL, Keyboardmania 2nd and 3rd. Neither event reads the date: HIRAKU's fixed clock holds
nothing back (`HK_RTC` sets the Firebeat clock for experiments). Per-credit and power-on codes
are typed by macros (`games/<id>/macros.txt`, `runtime/host/macro.{h,cpp}`, Ctrl+1..9 or
`--macro NAME[@FRAME]`; taps of 8 frames, 16 apart): pop'n 4 and 5, Animelo 2. How to use them:
docs/PLAY.md.

### Display pacing and smooth motion

`runtime/host/pacing.{h,cpp}` runs the machine on the display's refresh grid when a whole
multiple of the game's rate is within 3 % (`HK_DISPLAY_SYNC_SKEW`; 3.5 % with smooth motion),
resampling the sound. On Wayland it uses presentation-time and fifo-v1 and finds the compositor's
deadline by moving the commit later until a miss; on X11 it runs on the mode's rate. `--smooth-motion` (2026-10-19) fills those refreshes with in-between
pictures (`Core::render_between`) for the frame-counted cores (DJ Main, Firebeat GCU, the
PlayStation GPU of 573 and Twinkle), with the game at its exact speed (`frame_exact`). The scroll
guess (`include/hiraku/scroll_guess.h`, `tests/smooth`) pairs look-alikes and carries each on
its own steady line; reworked over the owner's tests at 240 Hz on 2026-10-20, the last with
pop'n 8 "perfect". How it works and how to check a game: docs/SMOOTH.md; options:
docs/PLAY.md, "Frame pacing".

Full history: docs/PLAN-HISTORY.md §20.

**Open**
- (build) M17, a song added to pop'n 14: the song list entry and its checksum, the chart commands, the `ele`/`bt`/`pre` ADPCM layout, a `[[song]]` laid into a free slot; not started.
- (build) Media mods on Python 2 (its disk is PCSX2's DEV9 ATA); hard-disk file maps exist only for pop'n 14's family.
- (owner) The 2026-10-20 smooth-motion fixes on beatmania IIDX 7th style (ghost notes, lag between scenes), seen fixed only in the play case.
- (owner) Display pacing on a real 60 Hz display, and smooth motion at 144 Hz.
- (parked) Native hooks on guest functions (a per-function override check): byte patches have covered every mod so far.
- (parked) Sugoroku de 8: a city actually unlocked in a run; the game's own code runs unchanged.
- (parked) pop'n 6: the effects of the operator switches other than SECRET MUSIC.

## 21. Twinkle: beatmania IIDX 1st to 8th style (M15)

The 573's approach (§19): MAME's devices on the shim, run by its exact scheduler, in cycle-exact
lockstep with MAME; then the background video MAME doesn't show, as a documented departure.
`runtime/twinkle/` ports `twinkle.cpp`'s driver class; every device is MAME's (library
`hiraku_mametw` on top of `hiraku_mame573`), including MAME's generated cycle-exact 68000 for the
SPU board (owner's decision, 2026-10-13: the mailbox makes its timing visible to the PS1, so
Musashi wouldn't do). Every style plays: 1st, 2nd, 3rd, 4th–8th, Substream, the club kits and the
revisions, 18 sets in all (docs/PLAY.md; the catalogue and club sessions in docs/CATALOGUE.md,
Phases 1 and 6). Each is in lockstep with MAME over a boot and a scripted game (main RAM every
frame, every snapshot; for the main sets also traced device accesses and sound lag 0). The main
sets and the club kits are regress cases (`tools/regress.py`: `<set>-boot`/`-clear`, `-play`,
`-video`, `-club`, and a `-play-drive` twin of each play case).

- **TW-0, oracle** (2026-10-13): a MAME at the pin with the Twinkle driver; `oracle/lua/hiraku.lua`
  taps both CPUs and reads the RTC65271's power-on time (`--rtc`); 3rd style's first boot stops
  at E150 and 1P Start clears it.
- **TW-1/TW-2, lockstep and playable** (69fcf65, fe7fb72): the `iidx` controls and lamps in
  `cabinet.cpp`, the 16-segment ticker as `Core::display_text()`. The mix plays at 0.8 of MAME's (`--mame-mix` for 1.0); the sound is
  handed on every millisecond (`--mame-sound` for MAME's 50 Hz, 595392b). Frames shown 16:9, as
  on the cabinet's widescreen CRT (4763834).
- **TW-3, the background video** (3883898; `runtime/twinkle/video.*`, `--video on|off`): an
  xvd701 edit reports the player's state, playback runs from the command's machine time (so the
  machine is untouched and every run the same), FFmpeg decodes the Video CD's MPEG-1 or the DVD's
  MPEG-2 (pl_mpeg dropped: block errors) on a worker thread. While mixer register 0x08 bit 1 is
  set, the video replaces the PlayStation's pure-black pixels, scaled to the whole frame, through
  the Bt812's brightness, contrast, saturation and hue. DVD styles: HIRAKU reads `VIDEO_TS` and
  plays the game's full three-digit chapter, while MAME's reply (a Video CD's two digits) is kept
  for lockstep (`DvdState`, `m_hk_command`, 102f655). These rest on the game's own writes, not on
  documentation: the key colour, placement and song-to-clip pairing are guesses until footage.
- **TW-4, speed** (37a3b95): the 573's R3000 recompiler on recorded blocks (`tools/mipsrec.py`),
  exact; a stage with the video ~1.5 ms a frame on the desktop. Releases have no recorded blocks
  and run the interpreter, made 2.4–3× faster in §40. 3rd style counts its charts in frames, so
  in-between pictures, not `[[rate]]`, are the way to smooth notes here (§40).
- **The CD loads** (271fb51, d51651b): MAME's SCSI drive has no read time, so every byte costs a
  scheduler round (~17 ms frames). By default HIRAKU reads at the cabinet's 4x drive
  (`hk_set_read_rate()` in `nscsi/cd`, third_party/mame/README.md): 300 sectors a second, a
  sector at a time; `--mame-cd` keeps MAME's model, and the regress cases use it except the
  `-play-drive` twins and the club cases. Keysound DMA is burst by memcpy. bmiidx3's worst
  frame on the desktop: 35 → 7.4 ms.
- **TW-5, 1st style beyond MAME** (2b55ac7): MAME stops at E600 because its SIO1 has no CTS; HIRAKU
  switches SIO1 to the fork's UART (`hk_psx_sio1_uart`) with the player holding CTS on, checked
  against a MAME with the same change (`oracle/mame-patches/twinkle-sio1-cts.patch`). The same
  wiring serves `bmiidxa`, `bmiidxc`, `bmiidxca` and Club Version 2's session cable.

Full history: docs/PLAN-HISTORY.md §21.

**Open**
- (owner) Hands-on play with IIDX controls: keys, turntable feel, lamps, ticker. The owner has run Twinkle games on the owner's IIDX cabinet (Windows, BIO2), but no verdict on the controls is recorded.
- (owner) Twinkle with `--smooth-motion` on that cabinet's i5-9400F, after §40's faster interpreter and with a High performance power plan (§42: the Balanced plan held the CPU down).
- (owner) Frame times on an N100-class PC (TW-4's exit: under 25 % of a core, no frame over ~8 ms); a stage was estimated at ~4 ms there, the CD loads' ~4.7 ms desktop frames likely ~2× that.
- (parked) The sub board's mix: whether it keys on black, a luma threshold (mixer 0x30?) or the mask bit, the exact placement (Bt812 HDELAY 126, VDELAY 22), the player's seek and start delays; needs real-cabinet footage.
- (parked) Which clip goes with which song: 3rd style's 5.1.1. asks for chapter 36, which doesn't match descriptions of its movie; 8th style's "004" may be a shared clip, and whether a real XV-D701 answers "004" as chapter 4 is unknown (MAME's error reply kept). Needs footage.
- (parked) Drive seek time (~120 ms access) is not modelled; the data phase starts one sector's time after the command.
- (parked) A PGO profile for Twinkle: wired into `tools/pgo.py`, never trained; releases carry no PGO.

## 22. ARM hosts (2026-10-13)

The cores build and run on 64-bit ARM Linux (aa32fb2), step 1 towards an Android version. The
recompilers emit C and C++, so the work was in the host glue: `hk_fiber_switch` in AArch64
assembly (the ucontext fallback costs ~14 %, and Android's libc has none); `-ffp-contract=off` and
`-fsigned-char` everywhere so results match x86-64; minimp3's NEON path rounds as its SSE path does
(`hiraku-edits.py`); the 573's RAM guard takes the host's page size (16 KiB on Android 15+ and
Asahi); the profiler's ticks from `cntvct_el0`, and a NEON copy of the K057714 blit.
`cmake/aarch64-linux-gnu.cmake` cross-builds with clang, statically linked, so qemu-aarch64 runs
the binaries and the build's own `hiraku-recomp`. On 2026-10-13 the recompiler's output on ARM was
byte-identical to x86's and all 17 regress cases then gave x86's digests under qemu.

Checked again 2026-11-14, after a month of other work had broken the build (zlib and zstd it
lacked: `tools/arm-deps.sh` builds them; two places that needed SDL; 8cb5125): eleven cases across
Firebeat, DJ Main, the 573 and Twinkle give x86's frames and sound under qemu. The ARM build has no
FFmpeg (Twinkle's background video), which its run statistics say. Once, with ten qemu cases
running at once, `bmiidx-play` stopped at frame 2969 with every thread in a futex wait; run again
alone it went through: not seen since.

Full history: docs/PLAN-HISTORY.md §22.

**Open**
- (build) CI doesn't build ARM, so it breaks unseen: an ARM job (cross-build, a few regress cases under qemu), or a check before releases.
- (build) Watch for the futex hang above (Twinkle, under load): if it comes back, the two threads' waits are the place to look.
- (owner) Speed on real ARM hardware: qemu says nothing about it.
- (parked) Android (GLES renderer, frame pacing, launcher, packaging the per-dump game code): to decide when it's time.

## 23. Konami Python 1/2 (M16, started 2026-10-13)

HIRAKU runs the Konami Python games by hosting PCSX2 Reliquary's emulator (a GPL-3.0+ PCSX2 fork) in
its shell: `runtime/python/`, `core = "python"`, the binary `hiraku-python`, built inside Reliquary's
tree by `oracle/pcsx2/build-reliquary.sh` with `oracle/pcsx2/reliquary-oracle.patch` (taps and hooks,
no emulation logic). Reliquary is pinned at 1.9.4 (update procedure: `oracle/pcsx2/README.md`). Python 1
runs pop'n music 9–14, Python 2 DrumMania V and V2 (`drmnvj`, `drmnv2j`). MAME's Python drivers are
skeletons, so Reliquary (`pcsx2-qt`, CPU mode pinned) is the reference: every game's EE and IOP RAM
hashes match it at 50 checkpoints over 3,000 frames (`HK_FRAMEHASH`; `--bare-board` leaves out HIRAKU's
card unit and network, which pop'n 13 probes). Hardware, media, disk format: docs/PYTHON.md; playing:
docs/PLAY.md, "Konami Python 1".

**Decisions (owner, 2026-10-13):** host Reliquary first, replace parts (EE by static recompile, I/O by
HIRAKU's cabinet layer) only where a measurement says it pays; Reliquary is the reference; `popn9` first.
Licence: linking Reliquary makes the Python binaries GPL-3.0 as a whole; HIRAKU's code stays
GPL-3.0-or-later.

| Phase | What | State |
|---|---|---|
| PY-0 | the oracle: Reliquary on Linux, per-vsync RAM hashes | done 2026-10-13 |
| PY-1 | Reliquary hosted, pop'n 9 playable (`popn9-play`) | done (3e604f9) |
| PY-1b | lamps (family `pypopn`), launcher, NVRAM, packaging, the pin | done (abe089f) |
| PY-2 | cost on low-end hardware | open |
| PY-3 | efficiency: picture handoff −7.3 % CPU; PGO −3.2 % (`build-reliquary.sh pgo`, optional); Partial texture preloading +11.4 %, dropped | first steps (59a6322) |
| PY-4 | latency: each vsync's own picture by default (`--picture-overlap`: the old way) | done (59a6322, e1891ec) |
| PY-5 | pop'n 10–14 | done (0b0ef83) |
| PY-6 | Python 2: DrumMania V and V2, steps 1–3 | done (eb772d9, 6e2c841, af67f55) |
| PY-7 | optional: EE static recompile, IOP on the 573's recompiler, the board's TX39 run for real | not started, on purpose |

**Facts that guide the work:**
- **Profile** (pop'n 9, Vulkan): the GS thread ~63 % of host samples, the CPU thread ~37 % (SPU2 most of
  it); EE code ~6 % of all, so an EE recompile wins little (hence PY-7). 2–2.7 ms of CPU-thread work a
  frame on the desktop; at real time 20 % of a core against `pcsx2-qt`'s 34 % (the emulation costs the
  same; the saving is in pacing and presenting). The launcher passes `--renderer vk` by default.
- **Latency:** the game polls its inputs once a frame; 4 frames from a press to the picture in test
  mode, 5 in play; a keysound ~2 ms after the vsync the press lands on.
- **Picture:** pop'n draws interlaced fields (SCANMSK); `Hiraku::full_frames` draws every line
  (cafdfe9), `--interlaced` keeps the fields and the regress cases use it.
- **Repeatable** for a fixed CPU mode. The Vulkan GS only on one GPU and driver: `popn9-play-vk` is
  recorded per PC and keeps GS setting changes honest (PY-3). DrumMania cases use
  `--pcsx2-set EmuCore/GS/extrathreads=0`; Python 2 disk writes go through `Hiraku::sync_disk`; the
  patch pins SPU2's DC filter so release builds match (d809f3d).
- **Baselines re-recorded, and why** (note new ones here): `popn14-play`, its card reader answering at
  boot (1ee4ed0); `popn12-phases` and the NET TAISEN pairs, datagram receives waiting (2af0ee6), and the
  pop'n 12 pair for the machine's time from 0 (b130576); `drmnvj-attract`/`drmnv2j-attract`, the faster
  boot (6e2c841).
- The core exits through `on_exit` → `_exit` (PCSX2's static destructors assert) and never calls
  `EmuFolders::SetDataDirectory()`. Unlocks: pop'n 9's event phase is in the board's BBSRAM
  (`tools/unlock.py` kind `mirror`); 10–14 followed (414fd68..ffd1a96).

### e-AMUSEMENT (pop'n 9–14)

No host sockets: **the card unit** (`cardunit.cpp`, pop'n 9's driver at EE `0x1a1050`–`0x1a6c00`: card
reader/writer and 10-key on the board's serial port, 9600 baud, half-duplex, the game reading its own
packet back first; pop'n 14's IC reader is `icca.cpp`); **the network** (`eanet.cpp`: the board's socket
commands, board command = RPC number + 1, channels 0–7, each answered as an 8-word status block, on a
virtual LAN with the board at 192.168.50.2 and `services` at 192.168.50.1); **the server**
(`easerver*.cpp`, standalone as `hiraku-eamuse`; `-url` sends calls elsewhere), answers built from the
games' own parsers. A call sent unencrypted is answered unencrypted without `X-Eamuse-Info`; an XML
declaration with `encoding=` is refused. Checked by `oracle/scripts/` runs: a card's life, KONAMI ID,
rankings, HITASURA, Gambler Z, pop'n friends, the title message; 10–14 with their events
(8f22226..1ee4ed0); one card for all 9–14 with shared marks and scores (59571ab); NET TAISEN 12–14:
lobby, lockstep LAN cable (`--lan-host`/`--lan-join`), 3–4 cabinets, online relay, results kept
(1611360 … 2026-10-19). `--autoplay` uses pop'n 9's own demo play, for tests. Protocol: docs/EAMUSE3.md;
`tools/eamuse/check.py` and `corpus.json` for server owners. A bemaniutils patch for pop'n 9 stays out of
tree (`tools/eamuse/bemaniutils/`): bemaniutils takes no AI-written contributions.

### Konami Python 2 (PY-6)

DrumMania V (E02) and V2 (F02) from Reliquary's `.py2` (`tools/ingest.py SET.py2`, `board=python2`). A
cabinet needs a console NVRAM with the OSD set up and a network: `ethnet.cpp` (DHCP, DNS, SNTP, TCP, the
AVS services) on 10.50.0.0/24, as a 192.168.x.x cabinet thinks it has no router. Lamps through
`Hiraku::p2io_lamp` (family `drmn`); the card unit is the 573 GF/DM EXTIO unit on the P2IO's serial port
0 (`gfdmcard.cpp`). e-AMUSEMENT: a card's life with each song's best kept (`easerver_gfdm.cpp`;
docs/EAMUSE3.md). Regress: `drmnvj-attract`, `drmnv2j-attract`, `drmnvj-eamuse(-on)`, `drmnvj-card-new/-again`.

Full history: docs/PLAN-HISTORY.md §23.

**Open**
- (owner) PY-2: cost and frame-time spikes on an N100-class PC or old laptop; it decides how far PY-3 goes.
- (build) PY-3's rest, after PY-2: GS texture hashing levels, bit-identical SPU2 speed-ups, a budget like M11's.
- (owner) Hands-on play with real controls and sound, feel and latency (a first try on 2026-10-15 led to cafdfe9).
- (build) GuitarFreaks V2 and its session with DrumMania; waits on the owner's copy of the game.
- (build) DrumMania V's gametop `musicdata` (answered without it so far).
- (build) DrumMania V ingested from MAME's set (only Reliquary's `.py2` now).
- (build) docs/EAMUSE3.md, "Not known yet": no INTERNET RANKING place with a KONAMI ID; a few statuses.
- (build) Watch: under heavy disk I/O (2026-10-15) five Python cases differed; where was not captured.
- (parked) Dancing Stage Fusion: edited media; needs a clean dump or a documented repair.
- (parked) DDR SuperNOVA 1/2, GuitarFreaks/DrumMania V3: no media.
- (parked) PY-7: only if PY-2/PY-3 say the CPUs matter.
- (parked) Sound per SPU2 chunk (≤2 ms to gain); the GS's texture straight in the window (clashes with picture modes).
- (parked) The non-BEMANI Python 1 titles; the bemaniutils patch's title message and frontend pages.

## 24. The rest of the catalogue (2026-10-15)

The plan for the rest of the owner's MAME 0.243 BEMANI archive, in seven phases, with the progress
of each, is `docs/CATALOGUE.md`. Phase 0 (`tools/catalogue.py`, `tools/mameset.py`, older MAME
names and split sets in ingest, NVRAM seeds, CHDs read in place through `runtime/dev/image.h`)
was done 2026-10-16; reading CHDs in place is now what the launcher does (`--copy-chd`). Phases
1–4 are done as far as the media allows: DJ Main's whole catalogue cycle-exact, the Twinkle
revisions, the 573 analog games, the DDR digital line, GuitarFreaks and DrumMania with their
sessions. Phase 5 did the owner's two picks, Dance Maniax and Mambo a GoGo (2026-10-18). Phase 6,
DDR 2nd Club Version × IIDX club sessions, plays both kits with no oracle (no MAME runs them).
Phase 7's Python 2 is the Python core's (§23).

Full history: docs/PLAN-HISTORY.md §24.

**Open**
- (build) GuitarFreaks 9th × DrumMania 8th session: GF 9th runs since 3c1ddf3, the session isn't done.
- (build) Sessions from the launcher (GF×DM and the club kits): it would start the second process with its socket.
- (build) Club play: the IIDX side's mix peaks at 1.6 of full scale and clips at the default gain.
- (owner) Hands-on play of the new sets, and controllers for Dance Maniax's sensors and Mambo's congas (options in `docs/CATALOGUE.md`).
- (parked) Sets waiting on media: older cassette dumps (`dsem`, `dsem2`, `dsfdr`, `dsfdct`, `dsfdcta`, `ddr4ms`, `ddr4msj`, `ddrs2k`, `ddrs2kj`), GF/DM 1st's other regions, `ddrjb`, `ddr2ml`, `drmna`, `ddr2mlb`, `dmxa`, the unit's CD for `pcnfrk4mk`–`pcnfrk10m`.
- (parked) `mamboagga` stops at its plays-used-up screen: emulating the rental replenishment would get round Konami's licensing.
- (parked) Martial Beat, Punch Mania / Fighting Mania, DDR Karaoke: the owner's call per game.
- (parked) Keyboardmania 3rd in the GF 6th × DM 5th session (a Firebeat–573 cable): a stretch, not started.
- (parked) `ddrkids` on Konami GS: a small new core, low priority.

## 25. Real cabinet hardware: card readers and the BIO2 (2026-10-16)

HIRAKU drives a real cabinet's I/O from the PC's side. Konami's ACIO bus after spice2x
(`runtime/host/acio.h`, d2dc121) carries the card readers (`runtime/host/readers.h`): `--reader PATH`
(serial; two on one port as nodes 1 and 2, b58d87a), `--scard` (PC/SC), `--cardio` (hidraw). A held card
is the player's (`Core::card_read`) in pop'n 9–14 and the 573 GF/DM card units; the 10-key comes in a
real reader's bit order (4874e2d), polled every 20 ms (34ffbf3). `--bio2 PATH` (`runtime/host/bio2.h`)
takes the BIO2 with BI2A firmware as the beatmania cabinet's controls and lamps, with IIDX 27's start-up
(ebe8eca; docs/BIO2-BI2A.md). `--popn-io` takes pop'n's FX2LP USB board, on Linux over usbdevfs
(22d9f17), on Windows through the user's `ezusb.dll` in the 32-bit `hiraku-ezusb.exe` (9d31d47);
docs/POPN-IO.md. Settings > Card readers / Cabinet I/O; how-tos in docs/PLAY.md. Pretend devices:
`tests/acio/`, `tests/popnio/`; `HK_ACIO_LOG=1`, `HK_POPNIO_LOG=1` print packets.

A tester's serial reader with pop'n 14 (2026-10-23) showed the 10-key order and an `-autocard` clash,
both fixed (4874e2d, a5e679f). The owner's BIO2 (USB `1ccf:8050`, "BI2X") runs BI2X firmware (about
1.3.3), so it speaks ACIO2. Read-only tests (`tools/bio2/`, 2026-10-18: e367132, 1222f36) found no I/O
module after power-up: BI2X I/O needs a code module from a game's data uploaded at each reset, which
HIRAKU can't carry (docs/BIO2-BI2X.md). **Rules:** never send firmware writes (BI2X `00 7x`, BI2A `0004`)
nor run a game's own I/O library against a real board; never open PID 8040 or 804C as a BI2X; IIDX 28
would likely rewrite the owner's board. The pop'n board's out packets pass an allow-list, and its
firmware goes only into a bare board's RAM.

Full history: docs/PLAN-HISTORY.md §25.

**Open**
- (owner) BIO2 (BI2A): the owner's cabinet's board (Windows, COM5) works for its inputs and lamps (2026-10-23); its ticker and effector sliders aren't reported yet (docs/BIO2-BI2A.md).
- (owner) Serial reader: a magnetic game (pop'n 12/13), two readers on GuitarFreaks, the 20 ms poll, three keys at once.
- (owner) NFC (an e-AMUSEMENT PASS as E004… in the serial reader's order, FeliCa as E00401…) and cardio (udev rule, report layout).
- (owner) A real reader's card in a 573 GF/DM card unit in a running game (`--npu`).
- (owner) pop'n board: firmware load, poll rate, buttons, coin (count or bit 22), lamp order; on real Windows with Cypress's driver.
- (build) BI2X: an ACIO2 driver with the module from the owner's own game data, only with the owner's go-ahead.
- (parked) The BIO2's sliders and Effect as beatmania III's effector, if wanted.

## 26. Windows

HIRAKU's Windows x64 build (launcher, every core, `hiraku-eamuse`, the tools) is cross-built on Linux by the same clang as the Linux build, targeting `x86_64-w64-mingw32` (`cmake/x86_64-w64-mingw32.cmake`, lld, statically linked: no mingw runtime beside it). `tools/win-deps.sh` builds what it links against into `out/win-deps`: SDL3's mingw package, zlib, zstd and a static qtbase of the host's Qt version, so the launcher is one `.exe`. MSVC is not used for HIRAKU's own build; only the Konami Python core is built with clang-cl inside PCSX2 Reliquary's tree (phase 4, done 2026-10-17: `runtime/python/CMakeLists.txt`, the release workflow's `python-windows` job). Phases 1 and 2 were done on 2026-10-16 (89b1f8d): the four cores HIRAKU builds itself give the same digests under Wine as on Linux, NVRAM and the 573 session cable included.

What the code relies on:
- The system layer is `runtime/shell/sys.{h,cpp}` (pages and write faults, `pread`, mapped files, folders, time, Win32 fibers for the Firebeat guest threads, `sys::replace_file`, `sys::write_file`) and `runtime/shell/net.{h,cpp}` (Winsock). Shared headers don't include `<windows.h>`: its macros (`IN`, `OUT`, `ERROR`, `min`, `max`) collide with HIRAKU's names.
- Image streams: Windows has no `fopencookie`, so a CHD's stream is a stand-in that only `image::pread`, `image::size` and `image::close` (`runtime/dev/image.h`) know. Never read an image stream with stdio. `hiraku-chdcat` stands in for chdman where there is none (`tools/chd.py`, `HK_CHD_TOOL=chdcat`).
- Waits: mingw's `std::this_thread::sleep_for` doesn't wait under 1 ms and waits a whole 15.6 ms tick from 1 ms up. **Don't use it in the runtime:** `hk::sys::sleep_for` / `sleep_until` use a high-resolution waitable timer per thread on Windows (4ac386d, measured on Windows 11). The PC games' process asks for a 0.5 ms timer and opts out of power throttling as HIRAKU's DLL or the 64-bit loader starts (`shell/wintimer.h`).
- Folder names a PC game's code page can't write: under Wine a link `C:\hiraku-paths\<FNV-1a of the folder>` (`runtime/pc/gamepath.h`); on Windows `hk::sys::plain_path` (the short name, else a junction under `%ProgramData%\hiraku\paths`) (504a60c, eea0830). Test: `pc_game_paths`.
- Lamps to other programs: `--lights-socket HOST:PORT` over TCP; the Windows launcher uses `127.0.0.1:47817`. Saves and settings in `%LOCALAPPDATA%\hiraku`, or the package's portable `user/`.
- Display pacing on Windows: the mode's refresh rate decides, DWM's estimate only refines it (337989a); the display's own vblanks from the graphics kernel where there (215387f); smooth motion's in-between pictures presented by the DXGI swap chain's own thread (31cfed3). Sound: SDL's WASAPI, WASAPI exclusive and ASIO (§28).
- The ACIO serial port is a Win32 COM port, PC/SC is `winscard`, cardio and `hidout` use SDL's HID calls. The profilers (`HK_PROFILE`, `HK_PY_PROFILE`) and the ACIO pseudo-terminal tests stay Linux only.
- The regression suite and the MAME comparisons stay on Linux; the Windows programs' unit tests run under Wine (§31).

Real Windows so far: the owner's Windows 11 test VM (no GPU: waits, folder names, packages) and the owner's IIDX cabinet (Windows 10, GTX 1650, 120 Hz, ASIO), which runs 0.10.x with its window, sound, display pacing and the PC IIDX games (§42). Full history: docs/PLAN-HISTORY.md §26.

**Open**
- (owner) WASAPI exclusive mode on real Windows (Wine refuses it; only the shared fallback checked).
- (owner) Which of HIRAKU's reader paths the cabinet's readers use on Windows: they read real cards (a642bf0), and a PC game's reader is always HIRAKU's (its `libacio.dll`), but no log has been kept. The game's log says: `reader COMn: ICCA...` for `--reader`, `cardio: ...`, or `scard: ...`. HID lights untried. (The BIO2 through `--bio2 COM5` works: §25.)
- (owner) Firebeat and Python games on real Windows (checked under Wine only).
- (owner) A PC game from a folder its code page can't write, on real Windows; a junction made by a non-administrator account; a drive without short names.
- (parked) Windows 11 taking a hidden process's fine timer away was never seen; the opt-out stays because nothing promises it won't.
- (parked) PGO for the Windows build (clang's `-fprofile-instr-*` left out of it) only if the speed is wanted.

## 27. Firebeat games from a release

A release can't carry a Firebeat game's recompiled binary (that is Konami's code), so a release's one `hiraku-firebeat` runs every Firebeat game from its program alone: no Ghidra, no compiler, no game's code in the binary (f66e801, 2026-10-17, FB-1 to FB-5). FB-1: HIRAKU's own control-flow analysis (`src/ppc/analyze.{h,cpp}`, `hiraku-recomp --analyze`), in Ghidra's order, with GCC's jump tables; against Ghidra's manifests the 21 games differ in dead code only, and all 21 run 6,000 frames identical from either. FB-2: no ingest step; `hiraku-firebeat` analyses the program as it starts (`--spec FILE` or `dumps/<game>/recomp.txt` override). FB-3a: the executor (`runtime/core/exec.{h,cpp}`, `src/ppc/spec.h` shared with `hiraku-recomp`, whose output stays byte-identical) builds each function's steps by a mirror of the recompiler's emitter (same `op_*` calls, cycle charges, polls), so it is exact by construction; the build kit (a bundled compiler) was not needed. FB-4: the launcher and `tools/regress.py` use `hiraku-firebeat` for any Firebeat game without its own binary. FB-5: every Firebeat regress case gives its baseline through it, from both packages (Wine for the zip). With `HK_GAME_BINARIES` off (a release) no per-game Firebeat target is built (`CMakeLists.txt`). Keyboardmania's XT446 MEG programs, compiled from a recording, stay out of a release: interpreted there. The executor's speed: §41 (pop'n 7's attract 1.86 ms a frame, its own binary 1.2). Full history: docs/PLAN-HISTORY.md §27.

**Open**
- (owner) FB-6: frame times on an N100-class PC (no such machine measured yet).
- (parked) PGO of the release runtime: releases build without it (§40); only if FB-6 asks for it.

## 28. A public release

GitHub releases (Linux `.tar.xz`, Windows `.zip`) have been tagged since 2026-10-17 (0.1.x to 0.10.1 by 2026-11-13). Someone unpacks one, starts it with nothing installed, and adds games by dropping MAME sets (zip, 7z or folder, CHDs read in place) or their own dumps on the launcher; saves and settings stay in the package's `user/`. Nothing in a release comes from a game: `HK_GAME_BINARIES` off builds one binary per core (`hiraku-djmain`, `hiraku-sys573`, `hiraku-twinkle`, `hiraku-firebeat`, `hiraku-python`), with no recorded 573/Twinkle blocks (interpreted: §40), no Firebeat game code (§27) and no XT446 MEG recording (cd3c011). `.github/workflows/release.yml` builds both packages on a `v*` tag (a draft release, with FFmpeg's and SDL3's sources) and on pushes to `main`, `ci/...` branches and pull requests (artifacts and tests: §31).

What the packages hold and how they are made:
- R-1: `tools/release.sh linux|windows OUT`; Linux on Ubuntu 22.04 (glibc 2.35+, `tools/release-setup-ubuntu.sh`, `tools/release-deps-linux.sh`: SDL3, zstd, the Qt Company's Qt 6.12, python-build-standalone's Python, 7-Zip); Windows cross-built in an Arch container (`tools/release-deps-windows.sh` → `tools/win-deps.sh`, python.org's embeddable Python, `7zr.exe`).
- `tools/ffmpeg-min.sh`: a minimal static FFmpeg (MPEG program stream and raw MPEG video demuxers, MPEG-1/2 video, Video CD/DVD audio) for Twinkle's background video on both systems (R-2).
- PC/SC is loaded at run time on Linux (`HK_PCSC_DLOPEN`): the games start without libpcsclite.
- R-4, R-5: the Konami Python core: Linux by `tools/release-python-linux.sh` from Reliquary 1.9.4's source, which the owner hosts as the `reliquary-1.9.4` pre-release (2a2e09d); Windows by the `python-windows` job with clang-cl (§26). The PS2 BIOS and MagicGate keys can't ship: users drop them with the game. The Linux Python core needs Vulkan or OpenGL drivers and a desktop-sized `/dev/shm`.
- Every download the dependency scripts use is checked against `tools/release-sums.txt` (`tools/release-fetch.sh`, 9cf2e4a, in releases since 0.10.0); not checked: `llvm.sh`, pip's packages, the distributions' packages.
- The launcher says "Not in this package" for a game a package can't run, with why (`launcher/games.h`).

Checked before each release: a bare Debian 12 container and Wine add games from MAME sets (DJ Main, 573, Twinkle, Firebeat; pop'n 9 for Python) and give the regress baselines. Windows audio added after the first Windows user's feedback (v0.1.2): an output device choice, WASAPI exclusive (`--audio-exclusive`), ASIO (`--audio-asio`, declared from the specification; owner-checked on real Windows 2026-10-18). The launcher's windows fit small screens (`launcher/fit.h`). Twinkle regress cases need `--mame-cd` when run by hand. Full history: docs/PLAN-HISTORY.md §28.

**Open**
- (build) DrumMania V/V2 from MAME's set (`drmnvj.zip`): ingest takes only a `.py2`; mapping MAME's set needs MAME's own disk (`e02_jaa_2005050201`), whose HDD id the set's belongs to, to check against.
- (owner) Frame times of a release on an N100-class PC (§27 FB-6; the 573/Twinkle interpreter's cost: §40).
- (owner) The Konami Python core and WASAPI exclusive on real Windows.

## 29. Lola Retro and the CRT beam simulator (M18)

Two shaders the owner asked for, both MIT and both done on 2026-10-20 and checked by the owner at 240 Hz ("perfect", 87a8aa5). Lola Retro (a libretro slang pack modelling the whole analog chain) is the fourth picture mode: `--picture lola`, the launcher's "Analog: Lola Retro" with a **Look** list, F10's fourth (99a06aa). One conversion (`consumer-tv-scart-rgb`, `tools/slang2glsl.py`); the other presets are looks (`looks/*.params`, `tools/slangp2params.py`) without curvature, overscan or timing. Each core says its monitor (`Core::monitor_khz`: 15, DJ Main 24, Firebeat 31) and the shaders read `hiraku-<k>khz.params` (1084S, studio monitor, PC VGA); overscan 0. Its beam pass is sized at twice the window's width (0.17 ms, not 0.52 at 8192 wide): an RGB look costs 0.33-0.37 ms on an RX 7900 GRE (royale 0.29); an N100's iGPU is about 40-60× slower. Not taken: the HDR presets, the scenes and casings (parameters, off), composite as a default (the cabinets were RGB), the "TV show" preset.

The engine gained what Lola needs (95b2f9d): feedback passes (`<alias>Feedback`, `PassFeedbackN`), `OriginalHistoryN`, and `FrameCount` as the game's frame number, so an in-between picture keeps the frame's count, history and feedback (`tests/picture`).

The CRT beam (`--crt-beam`, `--crt-beam-gamma` 2.2, `--crt-beam-gain` 0.7; Settings > Motion > "CRT beam"; 8674555) is a port of the Blur Busters simulator (`runtime/host/beam_shader.h`, `third_party/crt-beam-simulator`) over the game's two latest pictures. Each refresh draws the slice the beam sweeps then, placed by display pacing at the game's own speed (`pacing::phase`, `pacing::refreshes_in_frame`), on at 1.9 refreshes a frame or more (120 Hz up). Decisions from the owner's tests: plain power curves, not the original's sRGB formula with its exponent swapped (5248f20); the beam takes whole refreshes wherever display sync would lock (within 3 %), since a drifting split leaves a slow wave in dark parts (4a4e25d). Keyboardmania's turned monitors sweep left to right. Not with split windows or the Python core (smooth notes run pop'n 9-14 at the display's rate). How-to: `docs/SMOOTH.md`. Full history: docs/PLAN-HISTORY.md §29.

**Open**
- (owner) The beam on a 144 Hz display (seen only at 240 Hz).
- (parked) DJ Main's 58 Hz is more than 3 % off any display rate, so its beam's split still drifts.
- (parked) HDR output, and the beam for the Python core: not planned.

## 30. The PC pop'n music games 15–18

From pop'n music 15 (2007) on, pop'n is a Windows program. The owner chose (2026-10-21) to wrap Konami's own program rather than reimplement Windows: it runs as it is on Windows and under Wine on Linux, and HIRAKU supplies the cabinet around it. pop'n 15–18 are `.exe` programs on Konami's older `libavs-win32` (ea3 e-AMUSEMENT), which spice2x doesn't run; 19–21 are `popn19/20/21.dll`, 22 on `popn22.dll` (§35), the newest a 64-bit DLL (§32). All four boot, play, go online and battle under Wine (2026-10-21 to 10-23), from `hiraku-pc` on Linux and `hiraku-pc.exe`; on Windows 10 they start and run on a player's PC (GitHub issue 4).

- **PC-1, the cabinet DLLs** (`runtime/pc/`, 32-bit): `ezusb.dll` (pad, lamps, coins, watchdog), `libacio.dll` (the reader, after spice2x: always HIRAKU's; a real reader on a COM port is read by `runtime/host/readers.cpp` and its cards come in through the link), path hooks (`D:\popnNN` to the game's folder, `E:\` to its save folder), Direct3D 9 windowed.
- **PC-2, `hiraku-pc` and the link** (`runtime/pc/link.h`): shared memory carries pad bits with their event times, lamps, coins, the card, pictures and sound. The game is one process group / job object that ends with `hiraku-pc` and is stopped alone (2026-11-10, `runtime/pc/process.h`); the save folder is locked while it runs.
- **PC-3:** ingest of a `prog/<date>/` + `data/` folder, the newest program run from a copy; launcher family `pcpopn`.
- **PC-4, e-AMUSEMENT** (2026-10-22): HIRAKU's pop'n server; `runtime/pc/net.c` answers libavs's lookup, gives a network card, answers the pings. Players kept, carry-over 14 → 18, rankings, one card for all with pop'n 9–14, the PCBID from `-p` (2026-11-10). NET TAISEN through `runtime/pc/vsock.c` and HIRAKU's relay (lobby ports 5741–5744). Records and calls: docs/EAMUSE3.md.
- **PC-5:** pictures through HIRAKU's window (`runtime/pc/core.cpp`: every shell option), sound through HIRAKU's DirectSound (`runtime/pc/dsound.c`), the game's 14 ms wait before `Present` removed (`unlimit_frames`; controls to display 4.9 ms median under Wine), mods (docs/MODS.md "The PC games").
- **PC-6, pop'n 16–18** (2026-10-22/23): encrypted program lists with an empty `prg.bin`, so HIRAKU's DLLs pass; 17's and 18's binary XML (`runtime/python/kbin.h`). `runtime/pc/press.c` makes the board's 360-a-second pad samples from timed events; `--press-time` and `--judge-offset` move the judge routine's presses; `-autopin`.

How to play: docs/PLAY.md "BEMANI PC". Full history: docs/PLAN-HISTORY.md §30.

**Open**
- (build) pop'n 19–21 (`popn19/20/21.dll`): not started.
- (build) pop'n 18's boss battles: not kept (`boss_diff`, `boss_battle_point` are guesses).
- (build) The expert courses' stage scores in 16's and 17's records (the boards are kept).
- (build) File mods for 16–18 (encrypted data lists, numbered folders); memory patches work.
- (build) 16–18's `game.get` phases are guesses; the data folder's `NAME-GAME` overrides each.
- (owner) A real pop'n board and card reader, and the sound's latency on the owner's set-up.
- (parked) A `prg.bin` that lists `ezusb.dll`/`libacio.dll` would need a stand-in `d3d9.dll` patching Konami's DLLs; none seen.

## 31. The code's structure

Done on 2026-10-23 (5d56efa), before more features. A binary per game only where something of the game's is compiled in: `runtime/main.cpp` is compiled once (`hiraku_main`, the id in `shell/built.h`), and games with no recorded code (all DJ Main, most 573 and Twinkle, every Python game but `hiraku-python`) are links to their core's binary, which finds the game in its name; ccache when installed. The e-AMUSEMENT server is in parts (`runtime/python/easerver*.cpp`: calls and files, `_rank`, `_gfdm`, `_popn`, `_share`, `_pc`) on one protocol in `runtime/eamuse/` (MD5, RC4, XML, records, the online client). The two virtual LANs stay apart: the Python board's commands and LAN cable are not the 573's socket calls. Shared helpers: `shell/kvfile.h` for `machine.txt`, `sys::make_dirs`, `copy_file`, `exists`, one ACIO packet decoder; ALSA in `host/alsa.cpp`.

CI: `.github/workflows/release.yml` builds both packages and runs `ctest` on every push to `main`, on `ci/...` branches (the workflow as for `main`, publishing nothing: for a change GitHub's compilers might refuse) and on pull requests. The Windows build's tests run under Wine (`tests/CMakeLists.txt`: `CMAKE_CROSSCOMPILING_EMULATOR`); the Linux job runs them in Xvfb with `HK_TESTS_NO_SKIP=1`, and `HK_TESTS_REQUIRED` stops the configure when a named test can't be set up (cc50573). The Linux job also runs everything under ASan and UBSan (`HK_SANITIZE`, `tools/sanitize.sh`, dc79a0b); Musashi and MAME's H8 and SWP30 are left out of UBSan as third parties' code. By hand: `cmake -B out/win-tests -DCMAKE_TOOLCHAIN_FILE=cmake/x86_64-w64-mingw32.cmake -DHK_GAME_BINARIES=OFF`, build, then `WINEPREFIX=... WINEDEBUG=-all ctest` with no `DISPLAY`. Left as they are on purpose: `main()`'s flat option list and the launcher's command line. The PC games have no bit-exact regress cases (Wine and real time). Full history: docs/PLAN-HISTORY.md §31.

**Open**
- (parked) Not under the sanitizers: the Konami Python core (its own tree), the launcher beyond its report cleaner, the Windows programs.

## 32. The 64-bit PC platform

A loader for 64-bit AVS2 PC games, `runtime/pc64/` (`hiraku-pc64.exe`, 64-bit clang/MinGW, in `bin/pc/`), run by `hiraku-pc` as §30's games are: the same link, window, pacing, sound, inputs and server. A game uses it with `[pc] loader = "pc64"` (`games/popn29`); the launcher lists it only once unlocked. The working notes are kept outside the repository. It plays under Wine; the server keeps its players and scores (`runtime/python/easerver_hc.cpp`, the record read off the game's struct descriptors).

- **At the game's imports:** its devices after spice2x (`board.c`), two screens on one adapter (`display.c`), files (`files.c`), network as §30's (`net.c`); `log.c` logs the exceptions the game throws and, with `HK_PC64_TRACE_STRUCTS`, which part of an answer it doesn't take.
- **Speed and sound:** the game counts 120 frames a second; under 100 Hz it draws two pictures per frame shown (the first `not_shown`). The loader's WASAPI device plays on its own clock in 2.5 ms periods (freed in other amounts, the game resampled its music); checked against the decoded `.2dx`.
- **Pictures under Wine:** DXVK's Direct3D 9, shipped in the package (Wine's own misplaces the pictures); shaders using `sin`/`cos` rewritten; a missing texture coordinate set pointed at set 0 (0c97406).
- **Events and unlocks** (2026-10-26): eight events' phases, each at its last unless `phases-phc` says otherwise; songs and locked charts given to every player unless `unlock-phc` is 0. The game has no phase for event 3, so it keeps NET battles off.
- **One 4:3 screen, nine buttons** (`--single-screen`, `runtime/pc64/crt.c`, 2026-10-24 to 11-10): the main screen's middle 4:3 scaled 1440x1080 → 1280x960 → 640x480; every scene rearranged by editing copies of the game's layouts in the save folder's `single-screen/` (`HK_PC64_OVERLAY`), made from the player's own files by a local tool, never shipped. What the touch screen asks for is on the buttons, shown as panels over the picture: button 5 START/OK, green 3/7 menus, blue 4/6 values, red 5 done; the PIN and name on the 10-key; in a song the 10-key's columns are SUDDEN, HIDDEN and RETIRE (owner's choice, 2026-11-10). No touch-screen view in this mode (owner, 2026-10-29).

Full history: docs/PLAN-HISTORY.md §32.

**Open**
- (build) Server: events 2, 5, 6, 7; the shop and other items; courses, friends; one card for all.
- (build) One screen: the two-player character select, the version string, the mode select's longest description, other markets' prompts (market J's places only).
- (build) One screen: touch-only buttons (sort, settings, favourite, the result's switches): waits on the owner's choice of controls.
- (owner) The one-screen mode, with the fixes of 2026-11-09/10 (f9c5707), on the owner's cabinet.
- (owner) Real Windows (not run on a Windows PC yet, 2026-11-14) and a real two-screen cabinet's I/O: checked under Wine only.
- (parked) NET battles: off in the game itself.

## 33. beatmania IIDX 9th style, the C02 cabinet

From 9th style (2003) to HAPPY SKY, IIDX is a Windows XP Embedded PC game (Konami's model C02) with an EZ-USB board. HIRAKU runs it as it runs the PC pop'n (§30): Konami's `bm2dx.exe` as it is, under Wine on Linux, with HIRAKU's DLLs for the cabinet and `hiraku-pc` as the cabinet they talk to. `games/iidx9` (family `iidx`, `[pc] board = "c02"`, `layout = "revisions"`): ingest finds the program folders by revision (JAA to JAH, the last by name is run) and the `data` folder beside them.

- **The board:** `runtime/pc/ezusb_c02.c`, built as `pc/c02/ezusb.dll`, replaces Konami's (plain C names, 99 exports, 49 called): pad, turntables, sliders, the 16-segment ticker, lamps, coins, watchdog, the FPGA, the security plug (`G?C02J` + A/B/C; `usbBootSecurity("GEC02")` 0) and the H8 with two magnetic card readers and 10-keys. Board calls answer 0 done, 0x60 busy, -33 no board.
- **Pictures:** `runtime/pc/d3d8.c` takes the game's Direct3D 8 (back buffer read at each Present, no `GetRenderTargetData` in D3D8) into the link with the Direct3D 9 games' Present steps. Link v7 added turntables (1024 a turn), sliders, ticker and top lamps.
- **A new cabinet:** the DLL wraps the start-up's settings loader (found by shape in all eight revisions); when nothing loads, the defaults are saved with the game's own routine and the clock set, so neither BACKUP DATA nor CLOCK ERROR stops it. Saved as `settings.bin.0` on E: and `.1` on F:. Operator menu: P1 key 1 down, key 2 up, key 6 executes.
- **Movies:** 157 MPEG-1 streams (`data/movie/*.4`) through DirectShow; Wine plays them through GStreamer. DirectShow opens them itself, outside the path map, so `hiraku-pc` links `data` beside the program's copy (a junction on Windows, made by `make_junction` with `FSCTL_SET_REPARSE_POINT`; `mklink` under `cmd /c` mangled quoted paths).
- **e-AMUSEMENT** (e-AMUSE3, docs/EAMUSE3.md, "beatmania IIDX 9th style"): `net.c` gives the cabinet the domain `eamuse.konami.fun` (`HK_PC_EA_DOMAIN`); PCB id from `-p` (`HK_PC_PCBID`). Cards are 128 bytes as pop'n 9's, kept per card number (`cards/<number>.bin`, a642bf0). The server (`easerver_iidx.cpp`) answers module `C02` (each answer's `method` = method + command's first word, `musicgetrank`): new and returning cards, scores and music select's marks, the most played songs, the weekly song (`iidx9-weekly-song`), EXPERT course rankings (`iidx9-courses`), player 2's card and 10-key (link v8). Two players outside BATTLE send no `music reg`: the game's own rule.
- Bring-up aids: `HK_PC_USB_LOG`, `HK_PC_AVS_LOG`, `HK_PC_THREADLOG`, `HK_DBG_REC` on the card states (`0x18f5400`).

10th style, RED and HAPPY SKY followed in §36. Full history: docs/PLAN-HISTORY.md §33.

**Open**
- (build) e-AMUSEMENT: the record's layout (DJ points); a course for two players; KONAMI ID through the INFORMATION page (the server answers pop'n 9's `userid` calls, never tried from this game).
- (owner) attract's internet ranking screen seen with rankers (the server answers `getranker`; never seen on screen).
- (owner) the lamps and ticker on real lights; a real C02 cabinet.
- (owner) on real Windows: the movies through Windows' own MPEG-1 decoder (an N edition needs the Media Feature Pack) and the data junction to another drive (the game runs on the owner's Windows cabinet, 20063d7; these two not recorded).
- (parked) a boot that never reached the server, seen twice on 2026-10-25, not seen again in four runs with the network logs on.

## 34. Songs back from the player's music data

The 64-bit PC pop'n keeps a slot for every song it ever had (311 of 2374 marked left out). Players' Omnimix packs (popnhax's music data, songs' packs, ifs_layeredfs folders), given as mods, fill them in again and add songs past the table. Nothing of a game comes with HIRAKU. After Milo's work on the same game (popnMods), with thanks. How to install a pack: docs/SONGS-BACK.md; `tools/songsback.py` says what will come back and why not.

- **Songs** (`runtime/pc64/songs.c`): mods laid over the game's data, the first winning (`files.c`, `HK_PC64_DATA`); every `.xml` read before the start; a song whose Normal chart is there is written into the game's table, found by the code that reads it. The loader also sets its regions and tempos (from the chart), names a chart's pack by file type (`X`, `X_0N`, `X_diff`), stretches old banners, and leaves out a song without a banner.
- **Songs past the table** (94941d9, `runtime/pc64/tables.c`): the music and chart tables copied near the DLL, their references and 45 count sites moved (`runtime/pc32/x86.c` decodes 64-bit code). Left at 2374 behind their own checks: the server's song lists, friends' scores, the random choice.
- **Characters back** (2026-10-27/28): removed ones keep stand-in slots with stable ids; the loader fills them from `<chara id>` and grows the table past 2507 (`pc64_charas_grow`). Each needs its hariai and name picture or the game faults. `tools/oldcharas.py` takes them from an older `popn22.dll`, scaled; `tools/fbcharas.py` makes the Mickey Tunes and Animelo songs' own from their Firebeat discs (`tools/afp.py`, `tools/ifs.py`).
- **Market** (c280707): launcher **Market** / `--market` sets ea3's `dest`; mods `every-market` and `usa-pictures` in `games/popn29/mods/`; `[[patch]]` takes `module = "popn.dll"`.
- **Checked** with a player's pack: 410 songs back, 161 past the table; characters from an older game and both discs; restored songs played to their results.

Full history: docs/PLAN-HISTORY.md §34.

**Open**
- (build) Restored songs' scores read back from the server: sent and kept, not yet seen shown again.
- (build) Songs past the table: the cursor back on the card's last song, the random choice, friends' scores.
- (build) Restored characters' small icons (`cs_X` in `icon.ifs`): blank.
- (build) The console songs' own characters (their discs).
- (build) PNGs in the `_ifs` folders (only whole pack members are used).
- (build) By Medal and the extra Random entries (By Medal's source is needed).

## 35. The last 32-bit PC pop'n, `popn22.dll`

pop'n from 22 to the last game before the 64-bit one (§32) is `popn22.dll` on AVS2 2.15.8, started by Konami's `bootstrap.exe`, on the older one-screen cabinet: pop'n 15-18's `ezusb.dll` board and `libacio.dll` reader (§30), Direct3D 9, DirectSound. HIRAKU runs the last build of the last such game (M39, 2025092400) as `games/popn28` (`[pc] loader = "pc32"`, family `pcpopn`), listed by the launcher only once unlocked. Steps 1-6 were done on 2026-10-26 (1ab793e .. 996c49c); everything is checked under Wine.

- **The loader** (`runtime/pc32/`, `hiraku-pc32.exe`, clang i686 MinGW): reads `bootstrap.xml` (DLL, security code `GQM39JAA`, release code), boots AVS2 with NVRAM and RAW as plain folders, answers the licence DLLs (`kbt.dll`, `kld.dll`) as for the 64-bit game, then `dll_entry_init`, `ea3_boot`, `dll_entry_main` (spice2x's order). It mounts D:, E: and F: from the save folder's `dev/vfs/` (found at the game's `avs_fs_mount` import). `hiraku-pc` (`init_pc32`) makes the save folder's `run/` the AVS2 root, `data/` a link, runs it in Japanese, and loads DXVK's 32-bit `d3d9.dll` (Wine's own drew into a corner). `hk_program()` / `HK_PC_PROGRAM` point the DLLs' searches (frame wait, judging, sockets, crash log) at `popn22.dll`, not the loader.
- **Board and reader:** HIRAKU's `ezusb.dll` serves it as pop'n 16-18 (`usbPadReadLast` into a 36-byte buffer: spice2x's 40 would overrun it). `libacio.c` gained the FeliCa calls (any card not ISO 15693 is FeliCa), the dispenser (device 5: none) and the LED strip. The reader is a touch one again, as pop'n 15's: "take your card" waits for the card to leave.
- **Server** (`easerver_p24.cpp`, modules `info24`, `player24`, `lobby24`): the boot calls with `status="0"`; `info24.common` and `player24.start` give areas, the chocolate event, the shop's goods and NET TAISEN's phase. Players (`p24-<ref id>`, scores `p24s-<ref id>`): new and returning cards, the record's nine parts as the game's struct descriptors list them (`HK_PC32_TRACE=1` names a part an answer lacks), friendship, recent and most-played songs. Rules: `total_play_cnt` counts the credit being played (0 gives a broken tutorial stage); `tutorial` 308 skips both tutorials; `event_p28` goes back without `current_order`.
- **Mods** (docs/MODS.md, docs/SONGS-BACK.md): 5a, whole files and pack members laid over the data (`runtime/pc32/files.c`, ifs_layeredfs's layout). 5b, songs the game leaves out written into its music table from popnhax's XML, and charts added to listed songs (`songs.c`); songs past the table by copying the music and chart tables bigger and moving every reference a 32-bit decoder (`x86.c`) finds (`tables.c`, after popnhax's patcher). 5c, PNG pictures converted to the game's `tex/<md5>` members (`dev\hiraku_tex`, avslz when the list says so). `--skip-tutorial` (`HK_PC32_SKIP_TUTORIAL`) for guests and new players.
- **NET TAISEN** (`lobby24`): the lobby is the server's (`lobby24-M39`, shared by servers on one data folder; `netvs-M39` sets the phase, default 1); the cabinets' UDP goes through AVS2's own network calls, hooked by the loader to `vsock.c`'s relay as pop'n 15-18's; `portfw/globalip` answers the relay's address. Two cabinets on one PC start (the loader's `bind` takes a free port when 5703's is taken). Checked: two cabinets meet on a song, play it together, each `write_music` with its rank.

Full history: docs/PLAN-HISTORY.md §35.

**Open**
- (build) `--press-time` and `--judge-offset`: the judging behind the input object at `0x101bbe50` is still to find.
- (build) `info24.common`: the other events' phases and rankings (goods, areas and NET TAISEN's phase are given).
- (build) players: the burger shop's per-shop list and orders, lamps and stamps kept; items and `buy`; course rankings; friends and rivals; carry-over from earlier games (`conversion`); one card with the other pop'n games (this game's song table).
- (build) songs past the table: music select's cursor doesn't come back to one (opens on the recommended list); characters the music data adds (`charadb`).
- (build) mod pictures: `dxt5`, new pictures without the mod's own list, afp animations (`afp/`, `geo/`).
- (build) NET TAISEN: local battles (phase 2), CPUs filling a room, the "対戦待ち" category's room list, what `<netvs>`'s six record numbers count.
- (owner) real Windows; the lamps on real lights.
- (parked) old songs' own backgrounds (none in the packs seen); the game's drawn texts (the tutorial's bubbles: `[[text]]` mods); `prop/trustfiles_*.p7s` (not needed to run).

## 36. beatmania IIDX 10th style to EMPRESS, the C02 cabinet's later games

The same cabinet family after 9th style (§33): 10th style (`games/iidx10`, D01), RED (`iidx11`, E11), HAPPY SKY (`iidx12`, ECO), DistorteD (`iidx13`, FDD), GOLD (`iidx14`, GLD), DJ TROOPERS (`iidx15`, HDD) and EMPRESS (`iidx16`, I00). All boot from a fresh cabinet to attract with NETWORK OK, play credits with their movies and run e-AMUSEMENT on HIRAKU's server under Wine (fe415b5, ccf4ba7, d2767c9, 651e7f2, 93715ba); 9th style, HAPPY SKY, GOLD and EMPRESS also run on the owner's Windows cabinet (20063d7, fcef76e). Ingest tells the games apart by a `mark` string in `bm2dx.exe` and writes `sidcode.txt`. docs/PLAY.md has how to add each; docs/EAMUSE3.md the protocols.

- **The board under its driver** (`runtime/pc/ezusb_dev.c`, after bemanitools' public-domain ezusb-emu): from RED on the board library is in `bm2dx.exe`, so HIRAKU answers as the device (`\\.\Ezusb-0`, SetupAPI's device list): status packets and 64-byte pages to the board's nodes (bit 4 low on a D01 board; coins are the status packet's count). 10th style keeps Konami's `ezusb.dll` on top. GOLD runs on the C02 board, DJ TROOPERS and EMPRESS on the later FX2 board (`io = "fx2"`). The plugs' keys and tables are found in the game's own code by CRC-32; HIRAKU keeps no copy. Three races in Konami's board library that instant answers expose (a semaphore, a lock, a shared named event) are fixed in place (b7e4dd5). Test is held at least 300 ms (RED ignores short presses).
- **The way in:** `RtEffect.dll` (`runtime/pc/rteffect.c`, `pc/c02/`) for 10th style to DistorteD; HIRAKU's `libacio.dll` for GOLD on (`HK_PC_BOARD_IN_ACIO`). 10th style checks its folder against `bm2dx.sdx`, so Konami's file is kept as `RtEffect.konami` and the game's own read of it gets that (`HK_PC_RTEFFECT_FILE`).
- **A new cabinet** (`runtime/pc/c02_setup.c`): the settings' loader and saver found by shape, the block size read from the code (0xbb0 to 0x2ee4), the clock word found as the settings check's first test or before the checksum; HIRAKU's shop (ＨＩＲＡＫＵ, Tokyo) set when none; DJ TROOPERS' DISPLAY TYPE set to A.
- **Under Wine:** lights handed on with alpha 1 (`d3d8.c`); HAPPY SKY's and DistorteD's movie renderer wrapped to take Wine's `VIDEOINFOHEADER` (`movies_as_on_the_cabinet`); `d3dx9_24=n,b`, `LC_ALL=ja_JP.UTF-8`; EMPRESS sees a cabinet graphics card (`HK_PC_GPU_ID`) and runs from its unpacked program. Ingest writes DistorteD's missing `owl.bin` and repairs its 7-pack picture index to 6.
- **Frames:** every game counts everything in frames, the song's position too (no ms clock: dumps showed plain frame counters). So each runs at the rate its charts were counted in (ec7daa0: 9th style to DistorteD 59.939 Hz, GOLD 60.034, DJ TROOPERS and EMPRESS by DISPLAY TYPE, A 60.035 / B 59.935), a faster display gets in-between pictures drawn by the game (docs/SMOOTH.md), and a true high frame rate needs charts counted in shorter frames: done for EMPRESS (1ff5534) and DJ TROOPERS (0f456b8) as `[[rate]] hz = "x2"` mods, played by the owner at 240 Hz. RED's own 13 ms frame wait is taken out as pop'n's (12-15 ms).
- **Presses** (`runtime/pc/press.c`, 8dfb530): all eight judge from a 16-lane table of frame distances; `--press-time` and `--judge-offset` move pressed lanes' distances by whole frames around the game's judging routine, found by shape in every program date.
- **e-AMUSEMENT** (`easerver_iidx.cpp`): 10th style, RED, HAPPY SKY speak 9th style's dialect (`answer_iidx_red`; magnetic cards through the H8, 10-key codes on key down only); DistorteD on use modules with attributes and e-AMUSEMENT passes (`answer_iidx_fdd`; an answer is named after the model, `<FDD method="pcget">`, else dropped). Players, scores and music select's marks for all; GOLD on also DJ points and plays, dan grades and EXPERT course rankings (6d4b9ef, 8dfb530; `tests/eamuse/test_iidxrecords.py`). An IIDX reader sleeps empty from the pass taken until the game wakes it, so two passes come out at a credit's end (01cce64).
- **Effector** (9e4f087, d4dd249): the games set Realtek driver effects (a COM object from GOLD on, `RtEffect.dll` before). `runtime/pc/effector.c` turns them into programs (ECHO 1-5, REVERB 1-3 by each generation's labels, key shift, 10-band EQ, volume; link v12) that `runtime/host/effector.cpp` applies; checked on all from 10th style. RED's label is one ahead of its sound at some slider values: the game's own.
- Debug aids: `HK_PC_USB_LOG` (=2 every call with its thread), `HK_PC_AVS_LOG`, `HK_PC_JNET_LOG`, `HK_PC_XRPC_LOG`, `HK_PC_STDOUT`, `HK_PC_FILELOG`, `HK_PC_FW_DUMP`, `HK_PC_DUMP`.

Full history: docs/PLAN-HISTORY.md §36.

**Open**
- (build) 10th style, RED, HAPPY SKY: dan grades given back (`grade raised` kept; RED's `pc_grade` array, JAD 0x47acf0, not decoded); KONAMI ID; rivals; EXPERT course and weekly rankings (answered empty).
- (build) 10th style: 9th style's cards carried over (`pc takeover`), `music play`.
- (build) DistorteD to EMPRESS: rivals; EMPRESS's `best`, `b`, `t` elements.
- (build) a true high frame rate for 9th style to GOLD (mods made, not yet committed).
- (owner) GOLD on: a dan cleared and a course's ranking seen in the game (checked by the server's tests only).
- (owner) two passes out at a credit's end on the cabinet (checked on EMPRESS under Wine; DistorteD, GOLD, DJ TROOPERS by their code).
- (owner) DistorteD's and HAPPY SKY's movies on real Windows (the MPEG-2 decoder must offer RGB24; bemanitools uses CLVSD); 10th style, RED, DistorteD and DJ TROOPERS on real Windows.
- (owner) lamps and ticker on real lights; a real C02 cabinet.
- (owner) DJ TROOPERS on the C02 board stopped at FM TRNS-OUT before b7e4dd5 fixed the board library's races; not tried since (GOLD works on the cabinet). It runs on the FX2, its own cabinet's board.
- (parked) how the cabinet's effects really sounded: needs a recording to match.
- (parked) HAPPY SKY's white music select background on some graphics cards (bemanitools' option): not seen.

## 37. A high frame rate for the PC pop'n

pop'n 15-18 take the song's position from `timeGetTime`, so the Python pop'n's `[[rate]]` design fits them (the PC IIDX games count in frames: §36). The PC core has `[[rate]]` from link v9 (001c215): the DLL copies the watched words (`watch_addr`/`watch_val`) before each picture and puts `while_rate`/`between_ticks` patches in and out by the cabinet's ticks; `Core::set_vsync_rate` changes rate, clock, sound and pacing from the next frame without a jump; `Core::frame_repeated` keeps a repeated picture from counting as the clock standing still. The mods are `games/popn15..18/mods/smooth-notes`, each for its newest program date (docs/MODS.md has the counters and how they were found). The owner's first try (pop'n 15 at 240 Hz) had the whole play sped up, because dumps far apart missed counters that wrap; now every `add dword [reg(+/-x)], 1` after the sprites' draw routine is held, and the check compares consecutive pictures (`HK_PC_DUMP=+N/K`, `HK_PC_DUMP_HEAP`, `HK_PC_WATCH`).

Full history: docs/PLAN-HISTORY.md §37.

**Open**
- (build) pop'n 16 and 17: short fades copied each frame (a level rising by 10 to 255) still run at the display's rate.
- (build) pop'n 18: a colour cycle (about 80 vertex colours, +7 a picture) whose counter wasn't found (on the heap; the watchpoint didn't catch its writer).
- (owner) the fixed mods on a real 120/144/240 Hz display.
- (parked) older program dates: other addresses; the patches wait for their original bytes, so they do nothing there.
- (build) `--press-time` together with a rate: should be independent (presses timed in hiraku-pc's time), untried.

## 38. Fast start-up

`--fast-boot` (58baa98; Settings > General, on by default; `runtime/host/warp.h`) runs an arcade game's own start-up (20-150 s of checks and loading) as fast as the PC can go, then the game goes on at its own speed. The game computes exactly what it would; only the window's waiting goes: pacing off, sound dropped, at most 60 pictures a second shown with a box (speed and a progress bar) over them. It ends at the game's ready point, `boot_seconds` in its game.toml (the machine's seconds, Firebeat's skipped BIOS time included; `--fast-boot-seconds S` by hand), or at the first press of a game button (`warp::pressed`: keyboard, controller, BIO2, pop'n board), so a start-up waiting for a button isn't run past. Only with a window: headless runs (regress) are untouched. Measured: DJ Main 75 s in 4.4 s, pop'n music 7 (Firebeat) 40 s in 3.3 s, pop'n music 14 (Python) 23 s in 3.9 s. The ready points were read off contact sheets of headless runs (a snapshot every 2 s) as the first second after the last check or loading screen; 573 sets with no installed saves took their nearest measured sibling's, erring long (a late point only speeds a little of the attract, and a button stops it).

Full history: docs/PLAN-HISTORY.md §38 (the per-game times).

**Open**
- (build) DrumMania V and V2 (Python): start-up waits on the session check for a GuitarFreaks partner; no ready point.
- (parked) the PC games: under Wine the game's own clock is real time, nothing to speed up.
- (build) `install_seconds` for the other installing 573 sets (19 have one, from their regress install cases), and the network unit's 15-20 minute install, which has none.
- (build) a 573 session pair (two processes in lockstep) would run only one side fast.

## 39. beatmania III's floppy drive

The cabinet's 3.5" drive holds a player's data (GitHub issue 13; 3ca32a3). `--floppy FILE` (a raw 1.44 MB sector image; `default` = `floppy.img` in the save folder; the launcher's Game menu > Floppy disk) puts a disk in at power-on; the **Floppy** control takes it out and puts it in. `runtime/dev/fdc.h` is the controller and drive, `runtime/firebeat/board.cpp` the disk's file (written back whole through `hk::saves` once changed and the motor has stopped, at a disk taken out, and at the end); a missing file becomes a blank FAT12 disk, as the games need a DOS-formatted one. All five games read and write a whole cylinder a command in non-DMA mode, look for a disk at power-on and at Start, and keep their own scrambled file (`BM31.DAT`..`BM35.DAT`, The Final reads the older ones) and a `BASIC1P.TXT` score sheet. The model is MAME's N82077AA and drive, with departures on purpose from the 82077 data sheet (service asked at 16 − threshold bytes, the non-DMA bit held through execution, the interrupt a level): in MAME about one read in three fails and is retried, here none (`--mame-fdc` gives MAME's; `oracle/mame-patches/firebeat-fdc.patch` gives MAME HIRAKU's). Found on the way and fixed for every Firebeat game: a level interrupt still asserted stays pending after the EXISR acknowledge (`cpu403.cpp`). Without `--floppy` the drive answers as before, so regress baselines stay. Checked: `tests/fdc/test_fdc.cpp`, regress `bm3final-floppy`, `-floppy-mame`, `-floppy-change`, and 315 commands identical to patched MAME, each loading the other's disk.

Full history: docs/PLAN-HISTORY.md §39.

**Open**
- (owner) a real cabinet's disks as raw-sector images: should work, untested.
- (owner) two players each with a disk, changed between them with the control: untried.
- (parked) other image formats (.d88, .hfe): raw images are enough so far.
- (parked) READ TRACK and FORMAT TRACK: no game uses them ("unsupported command"). READ DELETED DATA is in (2fc2926): The Final reads a player's disk's boot sector with it before saving over it.

## 40. The PlayStation cores in a release: the interpreter's speed

A release has no recorded R3000 blocks and no PGO (§28), so `hiraku-twinkle` and `hiraku-sys573` run MAME's interpreter, and the owner's IIDX cabinet (i5-9400F) found Twinkle heavy; two thirds of the R3000's time was the PlayStation library's VSync() wait. 5a0db5b made a release's interpreter 2.4-3 times faster (IIDX 3rd style's stage 3.85 ms a frame to 1.50, every Twinkle and 573 regress case identical on Linux and Windows): waiting loops counted instead of run (`runtime/sys573/idle.cpp`, `HK_573_IDLE=0` off; exact because MAME's CPU is alone in a timeslice; nothing in it knows a game), the bus's page shape as compile-time constants (`third_party/mame/shim/emu.h`), small steps inlined (`HK_HOT`, `hk_one()`), Twinkle's colour conversion on the decoding thread, cheaper in-between pictures. On Windows, smooth motion's in-between pictures are presented by a thread of the DXGI swap chain's (`dxgi::queue`, 31cfed3; `HK_DXGI_NO_QUEUE=1` the old way; `tests/swapchain` against a pretend Direct3D device), so the emulation thread no longer waits for each; on the cabinet with 0.10.0 it put 23 857 in-between pictures out 0.02 ms after their times on average (§42). beatmania IIDX 3rd style counts its charts in frames, so in-between pictures stay the way for Twinkle (a true high rate would be a chart rewrite for ten programs).

Full history: docs/PLAN-HISTORY.md §40.

**Open**
- (owner) Twinkle with smooth motion on the cabinet under the High performance power plan (the 0.9.0 run there ran the CPU at 2277 of 3001 MHz under Balanced).
- (build) the local 573/Twinkle PGO profile predates the new interpreter: run `tools/pgo.py` again.

## 41. Firebeat in a release: the executor's speed

A release runs every Firebeat game on the executor (§27, FB-3a), 3.2-3.7 ms a frame against 1.2-1.8 for a game's own binary; 72 % of it was Konami's kernel handing the processor between threads (about 700 system calls a frame). db46ce5 halved a run's cycles (THE FINAL's practice stage 3.72 ms to 1.53, pop'n music 7's attract 3.18 to 1.86; every Firebeat regress case identical, also with `HK_EXEC_MEMO=check`): an exception handler's run remembered by its starting registers and replayed where what it read is unchanged and the budget outlasts it (`runtime/core/exec.cpp`, `exec_rec.cpp`; `HK_EXEC_MEMO=0` off, `HK_EXEC_STATS=1`; never a run touching a device, the clock, a timer, interrupts or a system call; `exec_names.h` checked by `tests/core/test_exec_names.cpp`); a 32-byte step; the GCU's rows eight pixels at a time (`runtime/dev/k057714_rows.h`, `tests/gcu/test_gcu_rows.cpp`); the frame hook's picture not composed when a window shows the present hook's (`fb::frame_picture_unused`). Measured and left as too small: the 403's memory-to-memory DMA (1-3 %), indirect calls' lookups (under 1 %), the 68000's fetch (2.5 %).

Full history: docs/PLAN-HISTORY.md §41.

**Open**
- (build) the executor's own steps: 35 % of beatmania III's frame, about 2 ns each.
- (build) the GCU's picture composed once a frame (`draw`, `draw_frame`: 12 %) and its plain row copies (8 %).
- (build) disk loading by PIO: 40 ns a word through `hk_mmio_read` and four layers to `AtaBus::read`; pop'n music 7's 6 ms frames. (The 8 MB read-ahead asked at every READ is now asked only when a READ leaves the last range: 8ab86d5, 456 calls to 3 in pop'n 7's attract.)
- (build) one yield in ten falls near an event and runs in full.
- (owner) one picture a frame on a real display (checked on SDL's offscreen driver only).
- (owner) a Firebeat game on Windows (in the packages since 0.10.0; only the unit tests ran, under Wine) and on ARM (the rows' NEON half only by its unit test under qemu).
- (owner) the slow PCs (N100, i5-9400F): nothing measured there.

## 42. The performance overlay

The last feature before 0.10.0 (fb2d0d7): the exit report, live, over the game's picture. `docs/PLAY.md` "The performance overlay" is the user's page, `runtime/host/statsview.h` the code's. F7 steps through three levels (`--stats-overlay line|panel|full`, Settings > Display): a line; a panel with rates, late frames, a frame's work, processor time and a three-second graph; everything, with latencies, busiest threads, the graphics card and the core's lines (`Core::overlay_lines`). A frame's account is the window's (`window.cpp` `stats_frame`): the time between frame starts less the waits is the work, and a wait counts under the part it was waited in (`pacing::waited`); late is the display's word where it gives one (`pacing::late_frames`), else a start more than half a frame late. A slow frame is also a `slow frame` log line, whether the overlay shows or not (the first of each five seconds, forty a run). The PC's numbers come from `runtime/host/sysload.h`, read once a second and only at the last level (AMD via `/sys`, `/proc/self/fdinfo` for amdgpu/i915/xe, NVIDIA's library if installed, Windows' "GPU Engine" counters). The place is dragged with the mouse or Shift+F7, kept in `stats-overlay.txt`. Nothing the overlay draws reaches a snapshot, digest or recording. On the IIDX cabinet with 0.10.0 (Windows 10, GTX 1650, 120 Hz) the panel was dragged with a real mouse and kept; fixed after (d277663, 19e87f5, 3821d83): a present waiting for the display is a wait, the log says when Windows ran the CPU slow and which power plan, logs kept by when written, Windows programs name their commit. The launcher can switch Windows to High performance while a game runs (5622f92, 2026-11-14, the owner's go-ahead; `system/highPerformance`, off by default, `launcher/powerplan.h`): the launcher does it, not the game, and puts the plan back when its window is back or at its next start; tested against a made-up Windows (`launcher_power_plan`).

Full history: docs/PLAN-HISTORY.md §42.

**Open**
- (owner) the power plan's calls on a real Windows (Wine has none; the test VM isn't to have its settings changed).
- (owner) the pointer shown in full screen while the mouse moves, by hand.
- (owner) the GPU line on NVIDIA and Intel cards, and Windows' GPU counters and thread names on real Windows (not recorded from the cabinet run).

## 43. The game menu: changing games from the cabinet

The owner's idea after 0.10.1 (ce91d87): a cabinet has no keyboard or mouse, so holding 5, 7 and 3 of one 10-key (or Test and Service, or `--game-menu-key`) for two seconds (`--game-menu-hold`) quits the game for a menu driven by the machine's controls. `docs/PLAY.md` "The game menu" is the user's page. The way out is the shell's (`runtime/host/menukey.h`), so every core has it; it watches inputs where they reach the core, so the launcher gives the card readers' 10-keys to every game. The menu is `hiraku-menu`, a core of the shell (`runtime/menu/`, `runtime/menu/menu.h`), so it gets the bindings, BIO2, pop'n board, readers, lamps and full screen with nothing written twice. The launcher runs the loop (`mainwindow.cpp` `openMenu`, `menuClosed`): exit code 73 brings the menu, the menu returns an id, the launcher starts it with its usual settings, and a game chosen in the menu goes back to it however it ends (8692617). The list is the launcher's (`launcher/gamemenu.h`): games ready to play with the left game's controls set (`cfg::controlsSet`), or all. Each family's controls are in `runtime/menu/nav.cpp` (rows down, shelves across; a reader's 10-key and the keyboard's arrows work everywhere; a plain gamepad is read by `runtime/menu/pads.h`, 919d73e); `tests/menu/test_menu.cpp` fails for a family that can't choose, change shelf and play, so a new family needs its menu controls. The picture is drawn by hand and timed by frame count, so a scripted run repeats (`game_menu_pick`); the launcher hands over glyphs in its own fonts (`--font`, `launcher/menufont.h` → `runtime/menu/font.h`; 4f4baab). Title pictures come only from the player's own game (579de15, never in the repository): `--menu-picture` (`runtime/host/menupic.h`) keeps a frame at the game's `title_seconds` (86 game.toml) unless a control was pressed or the frame is blank or text; a player's own `menu-pictures/<id>.png` comes first. Checked by unit tests, scripted runs, the whole loop headless and in a nested full-screen session with PC games.

Full history: docs/PLAN-HISTORY.md §43.

**Open**
- (owner) a real reader's 5, 7 and 3 at once; the menu through a real BIO2 or pop'n board, its lamps and ticker.
- (owner) the cover and the windows' order on real Windows; a keyboard's own keys and a real gamepad by hand.
- (parked) a title picture where a time can't find the title (the PC games, whose start-up lasts as long as the disk; sets not measured).

## 44. Tests around the edges

The 2026-11-10 review's test section ended with six things no test covered; all are done (2026-11-14), all but the last finding something on its first run. `tools/lint.py` (ctest `tools_lint`, Python's own modules only) compiles every Python file of tools/, tests and oracle/ with warnings as errors and checks for undefined names (c99d5a9). `tools/smoke.sh` tries each package as downloaded, with no game data: every program answers `--help`, `hiraku-menu` runs a scripted list, the package's Python runs its tools, the launcher draws on Qt's offscreen platform; Linux in a bare Debian 12 container with exactly the README's packages, Windows under Wine (1f8194a; found libxkbcommon and libcurl missing from the README). Fuzz targets (`tests/fuzz/`, `tools/fuzz.sh`, CMake `HK_FUZZ`, a minute each on every push): `fuzz_kbin`, `fuzz_ea`, `fuzz_menu` (1ccef21; found `kbin::from_text` packing empty or over-64-byte names). ctest `tools_unlock` and `tools_mameset` test the save editor and the MAME set tool (2412370; found `unlock.py` taking no `ID=VALUE` after an option before Python 3.12: `parse_intermixed_args` now). The review's open speed items were measured rather than done: the 403's word-by-word DMA isn't in pop'n 7's slow frames (they are the disk); indirect calls and Musashi's fetch are 1-2 %; the server's LZSS record took 9 ms and two short cuts halve it with the same bytes (84bfd77, `eamuse_lzss`).

The last two followed the same day. ThreadSanitizer (`HK_SANITIZE_THREAD`, `tools/sanitize.sh thread`, a step on every push; `tests/tsan.supp` names Mesa's threads and libdbus's locks): the cores are clean, the 573's drawing thread included; three races in the window's sound code fixed (flags the sound device's thread read, a text the ALSA writer set under the overlay's reader, an object destroyed at `exit()` under the sound thread) (4b947b8). The PC platform's self-test game (`tests/pc/selfgame.c`, ctest `pc_selfgame`, asked for with `HK_PC_SELFGAME=1` where there is a display): a small Windows program of HIRAKU's own that loads the board's DLL, reads the pad, draws with Direct3D 9, plays a sound and uses the drive letters, and draws what it saw, so `check_selfgame.py` reads back that every frame shown is the game's next, the button shows a frame after it was held and in no other, the coin counts once, the tone starts in the frame of the press; it passes on Linux, through `hiraku-pc.exe` under Wine (GitHub's Windows job, on Xvfb) and under ThreadSanitizer (ad1c846). The updater's signature was closed by the owner: the GitHub account's own security is the trust, no key.

Full history: docs/PLAN-HISTORY.md §44.

**Open**
- (parked) review item 6.6, the 64-bit platform's picture copied three times: a millisecond a frame of one game, against a new link layout for every DLL and loader.
