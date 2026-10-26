# HIRAKU — Static recompilation of Konami Firebeat

The plan began with Firebeat (§0–§13) and grew a section per core and feature since. The code cites
sections by number ("PLAN.md section 23"), so they keep their numbers; new work gets a new section.

| § | About | Kind |
|---|---|---|
| 0–8 | Firebeat: why recomp, the binary, the platform, architecture, validation, milestones, risks, decisions | the first plan |
| 9–13 | Firebeat milestones M1–M5 | outcomes |
| 14 | Efficiency on N100-class PCs, PGO | measurements, decisions |
| 15–17 | Other BEMANI platforms as cores; DJ Main, 573, Twinkle media; the core split, DJ Main | plan, outcomes |
| 18 | Firebeat music against MAME | outcomes |
| 19 | System 573 (M14) | roadmap, outcomes |
| 20 | Modding | plan, outcomes |
| 21 | Twinkle, beatmania IIDX (M15) | roadmap, outcomes |
| 22 | ARM hosts | outcomes |
| 23 | Konami Python 1/2 (M16): the core, e-AMUSEMENT, NET TAISEN | roadmap, outcomes |
| 24 | The rest of the catalogue | plan |
| 25 | Real cabinet hardware: card readers, BIO2 | outcomes, bring-up list |
| 26 | Windows | outcomes |
| 27–28 | Firebeat from a release; public releases | outcomes |
| 29 | Shaders: Lola Retro, the CRT beam | outcomes |
| 30 | The PC pop'n music games (15–18) | roadmap, outcomes |
| 31 | The code's structure: the build, the e-AMUSEMENT server, shared helpers | outcomes |

Goal: native PC executables of the Firebeat library: beatmania III, pop'n music 4–8 / Animelo / Mickey Tunes, ParaParaParadise, and Keyboardmania as a stretch goal. The approach is to statically translate each game's PowerPC program to C and link it against one shared runtime that replaces the arcade board.

The user supplies their own dumps. The repo never contains ROMs, executables, assets, or recompiled output, because the generated C is derived from the game. This is the N64Recomp/Zelda64Recomp model.

Plan written 2026-10-11. It is based on:
- `BM3-extract/` (REVIEW.md, tooling, Ghidra project);
- `beatmania-III-BGA-engine.zip`;
- the MAME 0.289 Firebeat driver (`firebeat.cpp`, `k057714.cpp`, `rf5c400.cpp`);
- the `~/bm3mame` Lua oracle;
- a fresh headless-Ghidra measurement of the BM3 Final program image (§1).

---

## 0. Why recomp rather than continuing the native-reimplementation route

REVIEW.md recommended the rizu reimplementation ("Route B") for one game. That was right for one game, but it does not scale to roughly 25 releases. Route B needs fresh reverse engineering of every game's flow, judge, scoring, and UI. A recomp reuses one runtime, and each additional game mostly costs a config file plus fixes for that game's quirks. Recomp is also the only route that preserves the original game logic exactly.

Route B (rizu) is dropped (§8). The BM3 format and BGA work, now living in the EZ2 port, serves as an **oracle** and test suite for the recomp.

---

## 1. What the binary tells us (measured, BM3 The Final `GCC01`)

| Fact | Value | Consequence |
|---|---|---|
| Image | 1 MiB flat at `0x80000000`, mirrored at `0x0`, entry `0x80003000` (tail branch) | No relocations, no loader. Load `hikaru_raw.bin` directly and skip the BIOS. |
| Functions (Ghidra auto-analysis) | **2,204**; 137,352 of 138,432 decoded instructions fall inside functions | Small. N64 recomps handle 5–10× this. |
| FPU instructions | **0** (soft-float via libgcc) | No float semantics to get right. |
| Computed jumps `bctr` | **37, all 37 resolved** to jump tables by Ghidra | The control-flow graph is statically recoverable. |
| Indirect calls `blrl` | 149 (function-pointer tables, e.g. BGA VM table at `0x800E560C`) | Needs a guest-address → host-function lookup table. A fallback is still required (§3.4). |
| Exception return `rfi` | 4 (`0x80000DD4`, `0x80003630`, `0x800037B4`, `0x8000393C`) plus 1 `rfci`, all in hand-written asm outside functions | Assembly glue of a preemptive C kernel. The glue is replaced; the C kernel is translated (§3.3, docs/KERNEL.md). |
| System calls `sc` | 2 (`0x800C52D4`, `0x800C96B4`) | One generic "call kernel function pointer" system call. |
| Privileged registers | DMA ch0–3 DCRs, EXIER/EXISR/IOCR, PIT, TCR/TSR, TBLO/TBHI, SRR0/1, SPRG2/3, ICCR/DCCR | Small, fully known 403 surface. |
| Odd 403/GCC-isms | `lswi`/`stswi` ×85, `lmw`/`stmw` ×525, `mtcrf 0xff` | Easy, but they must be implemented exactly. |
| Self-modifying code | None observed (only one `dcbf`, plus `iccci`/`dccci` cache init) | To be confirmed by the runtime's fallback logger. |
| Threads | Preemptive; watchdog/file/charlib/media/game threads (create/cancel/join) | Guest threads → host fibers, switched when the kernel retargets SPRG3 (§3.3). |

The sound CPU is a 68000 with a 512 KB program per SPU revision. It is small enough to **interpret** rather than recompile.

---

## 2. Platform model (from MAME; what the runtime must replace)

```
            ┌──────────── PPC403GCX 66 MHz, 32 MB RAM ────────────┐
 BIOS ──X   │  recompiled game code  +  HLE kernel/threads        │
 (skipped)  └──┬────────┬────────┬───────────┬──────────┬────────┘
               │MMIO    │MMIO    │DPRAM      │MMIO      │403 serial
         K057714 GCU   ATA/ATAPI  mailbox    I/O, lamps, DS1991 dongle
         (1 or 2)      CD/DVD/HDD   │        RTC/NVRAM, flash, UART,
                                    ▼        extend board (FFT, FDC, MIDI)
                      SPU: 68000 + RF5C400 + 32 MB wave RAM + own ATA (HDD/DVD)
                      ── or ── YMZ280B (+ CDDA from 2nd CD)
```

| Family | Audio path | Storage | Extra hardware |
|---|---|---|---|
| beatmania III (bm3, core, 6th, 7th, final) | SPU (68k + RF5C400). 3 SPU ROM revisions | Program CD + HDD on the SPU's ATA | Extend board: spectrum analyser, floppy, MIDI UART to the ST-224 effector; turntable/effect sensors at `7d000340` |
| pop'n 4–8, Animelo 1/2, Mickey Tunes | SPU. BGM is streamed by 1 MB double-buffered DMA keyed on RF5C400 reg 09 | Program CD + DVD on the SPU's ATA | 9 buttons, lamps |
| ParaParaParadise / Dancing / 1.1 / 1st Mix+ | YMZ280B (flash sample ROM) + CDDA from a second CD | 2 CDs | IR sensors at `7d000340`. `!:EXE:00` wrapper around the program |
| Keyboardmania 1st–3rd / Keyboard Heaven | YMZ280B + CDDA + **XT446 (embedded MU100B) synth**: H8S/2655 + SWP30, both in MAME | 2 CDs | 2 GCUs / 2 screens (ROT270), MIDI keyboards on the extend-board UARTs, mod wheels |

Interrupts:
- EXT0 is GCU VBlank (~59.94 Hz, 800×525 @ 25.175 MHz).
- EXT1 is the extend board.
- EXT3 is the SPU mailbox.
- EXT4 is main ATA.
- PIT and FIT come from the 403 itself.

Known imprecision in MAME (these are hazards for us too):
- The SPU 500 Hz tick and 350 ns DMA word rate are tuned by hand ("too fast cuts keysounds, too slow buzzes").
- RF5C400 envelopes, reverb, chorus and filter are approximate or missing.
- The GCU status register is constant, and the reg `0x6c` transparency hack and pop'n 8 flip fixups are workarounds.
- The ST-224 is not emulated. The XT446 runs on MAME's H8S/2655 and SWP30 sources (`third_party/mame`).

---

## 3. Architecture

### 3.1 Repository layout

```
HIRAKU/
  PLAN.md
  docs/                 hardware notes, per-game deltas, DONE-CRITERIA, PORT-DELTAS (EZ2DECOMP conventions)
  tools/                dump ingestion (disc → program image), misc Python
  launcher/             `hiraku`, the Qt 6 front end: game list, controls/lights mapping, settings
  analyze/              Ghidra headless scripts + Python → per-game manifest (functions, jump tables, fn-ptr tables)
  recomp/               C++ PPC403 → C translator (decoder, emitter, CFG checks)
  runtime/
    core/               guest memory, CPU context, MMIO bus, indirect-call table, fallback PPC interpreter
    kernel/             exception glue (frames, SPRG switch → fibers), IRQ delivery, 403 timers/DMA/DCRs
    dev/                gcu (software renderer), spu (68k core + rf5c400 + mb8421), ymz280b, cdda,
                        ata/atapi (image-backed), rtc65271, flash, ds1991, uart, extend board, fft
    host/               SDL3 video / input / audio; low-latency backends (PipeWire, WASAPI-excl, ASIO) later
  games/<set>/game.toml per-game config: image hash, entry, HLE symbol addresses, patches, IO map, disc layout
  oracle/               MAME Lua taps + compare/summarize (ez2oracle-style), deterministic input scripts
  dumps/   (gitignored) user CHDs, ISOs, BIOS/SPU ROMs
  build/   (gitignored) generated C + binaries
```

Languages:
- C++20 for the recompiler and runtime;
- generated C99 for game code;
- Python for analysis;
- Lua for the MAME side;
- CMake for the build.

### 3.2 Recompiler (`recomp/`)

- **Discovery** uses Ghidra headless (12.1.2 is already installed), extended by our own checks. It exports a manifest with:
  - function starts and ends;
  - basic blocks;
  - jump tables;
  - data words that look like code pointers into `.text` (these are the candidate `blrl` targets).
- **Translation** uses its own PPC decoder and emitter rather than Ghidra's P-code decompiler output. A per-instruction emitter gives exact CR/XER/CA semantics and is straightforward to audit. The full integer 403 subset is about 150 opcode forms.
- **Output.** Each guest function becomes one C function `void f_8004bb0(Ctx*)`:
  - guest GPR, CR, LR, CTR and XER live in `Ctx`;
  - branches inside the function become `goto`;
  - `bl` becomes a direct C call;
  - `bctr` through a known table becomes a `switch`.
- **Memory.** Guest RAM stays **big-endian**, and every load/store byteswaps. This is trivially correct with DMA, VRAM uploads and DPRAM, and costs nothing at this code size. Addresses are masked (bit 31 mirror). RAM gets a fast path; everything in `0x7xxxxxxx` goes through the MMIO bus.
- **Indirect calls.** `blrl` and computed `bctrl` look the target up in a sorted guest→host table. A miss falls into the runtime's PPC interpreter and is logged with a hint to add the address to the manifest.
- **Safe points.** Loop back-edges and function prologues get a cheap `if (ctx->irq_pending)` check, so interrupts and context switches are delivered at deterministic points.
- **Idle detection.** Planned: mark known wait-for-VBlank and spin loops in the manifest, and sleep the host instead of burning a core. **Measured 2026-10-12, it doesn't apply as planned (§14):** the guest has no such loop. Its idle time is threads calling the kernel's `yield()` in wait loops, so a bit-exact skip would mean proving that the whole scheduler state repeats. Deferred.

### 3.3 Kernel: translate the C kernel, replace only the assembly glue (`runtime/kernel/`)

Details are in [docs/KERNEL.md](docs/KERNEL.md). The program carries a small **preemptive kernel in C**:
- threads run in user mode;
- there is one generic system call ("run this kernel function pointer");
- context switching works by pointing SPRG3 at another thread's saved 384-byte frame before `rfi`.

That shapes the design (it was settled once the kernel had been read):
- **The kernel's C code is recompiled like everything else**, so scheduling, priorities, semaphores and timers stay authentic, as "match MAME first" requires.
- **Only the assembly glue (~300 instructions) is replaced with host code.** That covers boot, the vectors, frame save/restore, and the SPRG2/3 stack switch. The host glue builds the same frames in guest RAM.
- **Each guest thread is a host fiber.** On the way out of an exception, if SPRG3 now names another thread's frame, the host switches fibers. A frame never seen before starts a new fiber at its SRR0.
- **Interrupts are taken only in user code** (the kernel runs with EE off). Recompiled user code polls `irq_pending` at back-edges and calls. Kernel functions are emitted without polls.
- The 403 surface is emulated exactly: DCRs, 4 DMA channels including software mem→mem, PIT/FIT/WDT, TB and SPRGs. Cache operations are no-ops.
- **Cross-game reuse.** The kernel and the other platform libraries (ATA, GCU, SPU, dongle) are almost certainly a shared Konami Firebeat SDK. BM3 gets labelled first, then **Ghidra FunctionID/BSim signatures** locate the same routines in the pop'n, PPP and KBM images, like N64Recomp's libultra matching.

### 3.4 Fallback interpreter and shadow mode

The runtime embeds a simple PPC403 interpreter, used for three things:
1. executing anything the static pass missed (logged);
2. **shadow mode**, a debug build that runs selected recompiled functions and the interpreter on cloned contexts and diffs registers and memory writes. This catches translator bugs at the exact instruction;
3. bring-up before the manifest is complete.

### 3.5 Devices: register-faithful first, HLE second

Port the BSD-3 MAME devices as plain C++ classes (no MAME framework), so games see the same registers:

- **GCU K057714** (~1k LOC): a software renderer over a 1024×16384 16-bit VRAM. It covers:
  - 4-dword command FIFO;
  - display-list call/end;
  - fill, blit with scale, flip and 2 blend modes;
  - 2bpp chars;
  - 4 display contexts with brightness.

  Software rendering is fast enough and avoids GPU read-after-write hazards. The output is 640×480 (or 2×640×480 for KBM), presented scaled with integer or CRT-shader options.
- **SPU:** a permissively licensed 68000 interpreter core running the dumped SPU ROM, plus the RF5C400 port, MB8421 mailbox, 32 MB wave RAM, and an image-backed SPU-ATA with "instant" DMA. Timing is driven by the **audio clock** (44.1 kHz frames). That replaces MAME's hand-tuned 350 ns / 500 Hz hacks with something that can be reasoned about and measured.
- **YMZ280B + CDDA** for PPP/KBM: port the MAME code, with CDDA read from the user's disc images.
- **ATA/ATAPI:** a minimal PIO/DMA state machine backed by CHD/ISO/raw HDD images. Reuse `bm3.py` knowledge for the BM3 HDD (16-bit swap, 32 MiB chunks).
- **Small devices:**
  - DS1991 dongle (state machine, enclave from user dump, or the existing nodongle patches);
  - RTC65271 + NVRAM (persisted to disk = save data);
  - 29F016A flash;
  - PC16552 UART;
  - extend-board IRQ latch;
  - spectrum analyser (wdl FFT over the RF5C400 output, as in MAME);
  - floppy (stub, or a disk image).
- **Later HLE** for latency: once register-level audio works, optionally intercept the PPC-side keysound-trigger path and start voices immediately. This needs no 68k round-trip and brings latency close to a native engine with preloaded samples.

### 3.6 Host layer

- SDL3 window and gamepad.
- Configurable IO maps per family:
  - IIDX-style controllers / turntable for BM3;
  - pop'n controller or keyboard;
  - **ParaPara:** 5 hand-sensor zones (keyboard or pads; camera or IR as a later extra);
  - **Keyboardmania:** host MIDI-in for the keyboards. The XT446 card runs its own firmware, with a General MIDI SoundFont as the fallback when its ROMs are missing.
- Audio: SDL3 at first, then PipeWire low-latency on Linux and WASAPI-exclusive/ASIO on Windows. The game loop is paced by the audio clock, and VBlank is derived from it.
- NVRAM/RTC/flash are persisted per game. Linux is the primary target and Windows the second (MinGW/clang-cl).

---

## 4. Validation strategy (the part that makes it converge)

1. **Unit:** a translator test suite. Every opcode form is generated, run through the interpreter and the emitted C, and compared. This runs in CI and needs no game data.
2. **Shadow execution** (§3.4) on real game code.
3. **MAME differential oracle.** Extend `~/bm3mame` / the zip's `tools/oracle`:
   - memory taps on `0x7E800000` (GCU), `0x74000000` (DPRAM) and `7d000800` (inputs) log every MMIO access with a frame number;
   - deterministic input scripts in one shared format (ez2oracle `.osc`-style), executed by both MAME (Lua) and the runtime;
   - per frame, compare RAM regions (object list `0x801000cc`, VM vars, measure counter, judge state), the GCU command stream, mailbox traffic, and framebuffer snapshots;
   - `compare.py` / `summarize.py` produce a `report.md` with diff PNGs, as in ez2oracle.
4. **Independent oracles:** the C/Python BGA VM and `bm3audit --selftest` (498 scenes), and the extracted charts and keysounds (the recomp's sample uploads must match `bm3.py`'s TOC).
5. **Coverage:** a `census`/`spine` equivalent. It lists functions reached in the oracle traces that the manifest lacks, and how many fallback-interpreter entries each session hits. The target is 0 per full attract-plus-play loop.

---

## 5. Milestones

Durations are rough, assume one person, and are assisted. Each milestone has a concrete exit test.

| # | Milestone | Exit criterion | Est. |
|---|---|---|---|
| **M0** | Infrastructure | Repo, CMake, `game.toml` for `bm3final`, dump ingestion (CD → `HIKARU.EXE` → LZSS unpack == `hikaru_raw.bin`). Ghidra manifest export. MAME oracle extended with MMIO taps and input scripts. | 1–2 wk — **done 2026-10-11** |
| **M1** | Translator + interpreter | All 2,204 BM3 functions emitted and compiling. Opcode test suite green. Interpreter boots the image to the first MMIO access. | 3–5 wk — **done 2026-10-11** (docs/RECOMP.md) |
| **M2** | Kernel + platform bring-up | Threads, IRQs, PIT, DMA, RTC, dongle, ATA all present. BM3 reaches the `GCC01-JC-A STARTUP` self-test text with GCU output on screen. Shadow mode is clean over boot. | 3–4 wk — **done 2026-10-11** |
| **M3** | Attract and BGA | Attract loop runs. Frame-diff vs MAME is ≤ tolerance on 10 forced scenes (`forcescene.lua`). Zero fallback-interpreter hits. | 2–3 wk — **done 2026-10-11** (docs/M3.md) |
| **M4** | Audio | 68k + RF5C400 + SPU-ATA working. Keysounds and BGM match MAME captures, with no cutoffs or buzz. Spectrum analyser works. | 3–5 wk — **done 2026-10-11** (docs/M4.md) |
| **M5** | BM3 Final playable | Coin → mode select → song → result → name entry with real controls. NVRAM persists. Measured input→sound latency is published. | 2–3 wk — **done 2026-10-11**, except the hands-on test with real audio hardware and controllers. That test now also covers the 2026-10-12 fiber change (never run in `--window`). Waiting on the owner (can't test right now). |
| **M6** | Rest of beatmania III | `bm3`, `bm3core`, `bm36th`, `bm37th` run. Mostly manifests and SDK signatures plus SPU ROM revision differences. | **done 2026-10-14** (docs/PLAY.md):<br>• All four boot to attract and play the practice stage with sound, fully recompiled (2,139–2,251 functions; core, 6th and 7th each needed two `extra_entries`, as The Final).<br>• Against MAME the boot matches at the same frames; the attract is pixel-identical, 5–7 frames early (it was 22–95 late until the CPU timing fix of §18).<br>• **Left:** hands-on play. |
| **M7** | pop'n music 4–8, Animelo 1/2, Mickey Tunes | Same SPU stack, plus DVD on the SPU's ATA and BGM streaming via RF5C400 reg 09 (the hardest audio-timing case). | **in progress** (docs/PLAY.md):<br>• pop'n 4, 5, 7 and 8 boot to attract with sound; pop'n 8 also plays music select and full songs.<br>• pop'n 6 boots after a ~14 min first-boot flash install.<br>• Animelo 1/2 boot.<br>• Against MAME over attract (§18), pop'n 4–8 and Animelo's DVD-streamed BGM is sample-exact within each piece. Piece starts differ by 8–14 ms, probably DVD read timing.<br>• **Left:** model the DVD drive's read times; play every title hands-on.<br>• Mickey Tunes! (`popnmt2`, added 2026-10-14) boots to attract like MAME (the cast, the title, How to Play), a few seconds behind it through the warm-up as pop'n 4–8 are; a rental cabinet (`cabinet = 6`, MAME's init_popn_rental). Regress: `popnmt2-attract`. `popnmt` lacks its program CD (976jaa01). |
| **M8** | ParaParaParadise family | `!:EXE:00` loader, YMZ280B, CDDA, sensor input mapping. | **in progress**:<br>• 1st Mix Plus plays (coin, song select, a full song with CD music).<br>• `ppp`, `ppp11` and `ppd` play too (2026-10-14, from set-up saves: SOUND FLASH INIT and factory settings done): coins, mode and music select, a stage danced with the sensor bars (GREAT/GOOD judged), the result, name entry or the next stage; `ppp11` and `ppd` clear their first stage.<br>• **Sensors against MAME (firebeat.cpp `ppp`):** the same: four 16-bit words at 0x7d000340, each with 0x0100; bar 1 bits 0-2, bar 2 bits 3-5, bar 3 bits 6-7 plus word 1's bit 0, bar 4 word 1 bits 1-3, bar 5 word 2 bits 0-2 (bars 6-8 the Korean cabinet's); IN0 idles at 0x7f (bit 6 set, bit 7 clear) as MAME's.<br>• ppp's attract music (YMZ280B) keeps time with MAME (lag 0, no drift); levels match below 4 kHz, HIRAKU has a little more above 8 kHz (§18).<br>• **Left:** hands-on play.<br>• `ppp2nd` is different hardware, out of scope. |
| **M9** | Native polish | Low-latency audio backends, display options, input remap UI, Windows build, per-game config UX. | ongoing — the `hiraku` launcher (game list, controls and lights mapping, settings) and cabinet lamps (HID LEDs, socket: docs/LIGHTS.md) are in.<br>• Adding a game: drop its files on the launcher (or `tools/ingest.py FILE...`, `hiraku --add`). The game is found from its program or set name, and CHDs are unpacked with chdman, memory-capped (`tools/chd.py`).<br>• A portable build for other Linux PCs: `tools/package.sh`.<br>• The picture: off / light (crt-guest-advanced-fast) / full (crt-royale), for all games or one, with sliders that change a running game (launcher Picture; `--picture`, F10). The slang presets are converted ahead of time (`tools/slang2glsl.py`: glslang + SPIRV-Cross), and `runtime/host/picture.cpp` runs them as RetroArch's filter chain does. |
| **M10** | Keyboardmania (stretch) | Dual GCU / dual screen, MIDI-in, the XT446 synth card. | **in progress**:<br>• kbm, kbh, kbm2nd and kbm3rd boot to attract (dual GCU, ROT270, MIDI keyboards, CD audio).<br>• The real XT446 card runs (MAME H8S/2655 + SWP30, pinned to MAME master): in lockstep with MAME 0.289 for the first 84.5 s of attract MIDI; SoundFont stand-in when its ROMs are missing.<br>• **Plays a song through (2026-10-15):** the game's own autoplay flag (the attract demo's, 0x8022be72 + player in kbm), set by a script's new `poke`, plays stage 1 to STAGE CLEAR, 135 GREAT of 135 (`oracle/scripts/kbm-autoplay.his`), with the XT446 playing.<br>• **Cost:** the MEG's effect programs compiled ahead of time (§14): kbm attract 32.4 → 25.6 % of a desktop core, bit-identical.<br>• **Left:** the rest of the cost (the H8 firmware ~55 % of the card), the N100 risk; Keyboardheaven too (0x8022cb8a + player, `kbh-autoplay.his`, 135/135). 2nd and 3rd Mix are a C++ rewrite (demo.cc, demoplay.cc, play.cc): their attract demo does autoplay real songs from the normal charts (`/KMS/%s_%c%c.KMS`), but not through anything a frame-by-frame poke reaches: the play config's demo byte (+0x11, config pointer at 0x80111774), the play state's copy (+0x2e, on the stack), and swapping the three strategy objects for the demo's all leave a real game unplayed. The config is built and FUN_80039f08 called in the same frame, so the switch probably has to be in place when play starts: it needs a hook at a code address, which scripts lack. |
| **M11** | Efficiency for N100-class PCs | Every Firebeat game within its budget on an N100-class mini PC (§14): average under 25 % of one core, no frame over ~8 ms of host work. Bit-identical to the current runs (`tools/regress.py`). | **in progress** (§14):<br>• bm3final about 3.4× more efficient than MAME, 9.6 % of a desktop core in gameplay.<br>• Keyboardmania: the XT446 synth card runs on its own thread, mean frame 7.2 → 6.5 ms, worst 12.8 → 10.6 ms (bit-identical).<br>• Disk images read ahead in big stretches: a cold-cache bm3 practice run went from a 251 ms worst frame to 9.8 ms.<br>• **Left:** the SWP30 MEG is still ~5 ms a frame on its core (kbm).<br>• **The H8 (2026-10-19), measured, not changed:** the card alone over 120 s of attract MIDI is 23.9 s of CPU, the H8 about half. Its firmware has no idle loop to skip (the main loop's voice round-robin, 0x3c14, changes state each pass). 78 % of its instructions come from 295 addresses and 99 % from 2,284, so handlers compiled per address were tried: MAME's own h8.lst handlers, one per hot address, words as constants, every bus access's exit kept (tools/h8gen.py, bit-identical, 98.6 % of instructions through them). No gain at all (23.9 s either way): the cost is the per-instruction framework (execute_run, prefetch_done, the cycle checks) and MAME's memory system on data accesses (address_space, ~10 %), not decoding. Reverted. A real gain needs either a recompiler that also folds the cycle checks across an instruction run (keeping MAME's exits exact) or direct RAM access for the 128 KB work RAM; the card runs on its own thread (~20 % of a desktop core), so neither is urgent.<br>• **Blocked** on measuring real N100 hardware (not available right now). |
| **M12** | Cores: the shell split | The host layer (launcher, window, input, lights, scripts, oracle) talks to the machine through one core interface. Firebeat moves behind it unchanged (bit-identical digests). | **done 2026-10-12** (§17): `runtime/shell/core.h`; Firebeat and DJ Main are cores, each binary links one. Firebeat bit-identical. |
| **M13** | DJ Main core | beatmania 1st mix → THE FINAL, pop'n 1–3, pop'n stage: interpreted 68EC020 plus MAME's devices, checked against MAME. Analog turntable and lamps. | **in progress** (§17, docs/PLAY.md):<br>• `bm1stmix` and `popn1j` run in **cycle-exact lockstep with MAME**: work RAM, sprites and palette identical on every frame, with sound sample-identical. This holds through boot and a scripted game of each: beatmania (coin, turntable, song select, a song with keys and scratches, game over) and pop'n (all 9 buttons).<br>• In `tools/regress.py`.<br>• All 29 MAME sets have game files; the ATA password lock is ported from MAME (unit-tested).<br>• 2026-10-12: 14 more sets ingested and booted against MAME for 155 s: 2nd and 3rd MIX are cycle-exact like 1st mix. 4th MIX → THE FINAL, complete MIX (AA) and pop'n 2/3 reach attract, but their work RAM leaves MAME's between frames ~4380 and ~4730 (73–79 s in). Sprites and snapshots mostly stay identical: a few bytes at first, a timing offset later (5th, compm2, DCT, cmxaac). 1st–3rd MIX and pop'n 1 don't do this, so look for something the later games do at that time.<br>• **2026-10-16: that divergence was two things, and every set on hand is now in lockstep** (25 of MAME's 29, 155 s boot: work RAM, sprites and palette on every frame, every snapshot):<br>&nbsp;&nbsp;1. Musashi charged the 68000's per-bit cost for ROL/ROR/ROXL/ROXR by a register count (`orig_shift<<CYC_SHIFT`, 32 sites the first `HK_SHIFT_CYCLES` pass missed). MAME charges nothing per bit before the 68030. 4th MIX's decompression loop (`rol.l D5,D6`) showed it, with cycle-stamped traces of both CPUs (`HK_DJ_ITRACE`, `HIRAKU_ITRACE_CYCLES`).<br>&nbsp;&nbsp;2. The board assumed every disk was 16 heads × 63 sectors. MAME takes the geometry from the CHD (15, 255 or 9 heads; 52 or 32 sectors), and seek times follow its cylinders. game.toml `[hdd] chs` carries it now.<br>• **2026-10-19, faster drawing:** the K056832's pages a tile at a time (attributes, colour and pixel row once per 8 pixels, not per pixel): bm1stmix-play 1.03 → 0.72 ms a frame (p99 1.85 → 1.45), every regress case identical.<br>• **Left:** `bscompmx` and `popn1` (no media on hand); hands-on test. |
| **M14** | System 573 core | The roadmap is §19. MAME's PS1 devices with an interpreted CPU, in cycle-exact lockstep with MAME on an analog-board game (CD audio). Then the digital board (MP3 + FPGA). Then a static MIPS recompiler if the CPU is what costs most on an N100. | **in progress** (§19):<br>• `ddr3mj` (digital board) runs **in lockstep with MAME at the pin**: main RAM identical on all 3,600 frames of a 60 s boot, all 60 snapshots pixel-identical. That covers the BIOS, the CD boot, the cassette and PC Card checks and ERROR 147 (no install yet). It is a regress case. Host cost ~3.5 ms a frame on the desktop, interpreted.<br>• Every device is MAME's own code on the framework shim, run by a copy of MAME's scheduler.<br>• **`ddr2m` (analog board, 2026-10-12): in lockstep with MAME at the pin for 400 s.** That covers the first boot, the flash install (two Test presses) and attract with its demo and video: main RAM identical on all 24,000 frames, all 400 snapshots pixel-identical. The sound (SPU + CD audio) matches MAME's WAV: lag 0, correlation ≥ 0.998, level 1.00. It is a regress case.<br>• **A whole game too (`oracle/scripts/ddr2m-play.his`, 350 s):** the same first boot, then two coins, Start, NORMAL, How To Play skipped, BOYS played by stepping on all four panels (GOOD/GREAT/BOO, DANGER, CLEARED), stage 2's song select and Ready. Main RAM identical on all 21,000 frames, 350/350 snapshots, the sound lag 0 with correlation ≥ 0.998. Regress case `ddr2m-play`. **This is 573-2's lockstep exit test, met; the hands-on test with a pad remains.**<br>• The board is chosen per game (`io_board`, `cassette`, `pccard2`, `memory_card_reader` in game.toml): the analog GX700-PWB(F) is ported from MAME. DDR controls and lamps are in `cabinet.cpp`. Coin, start, mode and song select, and a stage start work from the installed flash.<br>• Fixed: the shim's CD reader didn't byte-swap little-endian CD audio (noise); the shim's address map took over 12 GB per 573 run (per-byte tables for MAME's unmapped ranges), which got the desktop OOM-killed during a parallel regress run.<br>• **Owner's hands-on test (2026-10-12): "worked nicely", with slight crackles.** Those came from MAME's mix: the SPU and the CD audio summed at 1.0 each peak at 2.0 and clip at the output (~1 % of samples, a third on loud songs; MAME's WAV clips identically). The 573 core now plays the mix at 0.5, which never clips, in MAME's balance. `--mame-mix` restores MAME's level for oracle comparisons. A departure from MAME's output stage only: the machine is unchanged.<br>• **Next:** the install flow for `ddr3mj` (573-3). |
| **M15** | Twinkle core (beatmania IIDX 1st–8th) | The M14 PS1 core plus Twinkle's SPU (68000 + RF5C400, already in HIRAKU), SCSI CD, ATA hard disk. Then the background video MAME lacks: Video CD (MPEG-1) for early styles, DVD for 6th–8th. | **in progress** (§21):<br>• **TW-0 done:** oracle (pinned MAME with `twinkle.cpp`, Lua taps on both CPUs), `bmiidx3` and `bmiidx` ingested.<br>• **TW-1, boot and attract done (2026-10-13):** `hiraku-bmiidx3` (MAME's PS1 and MAME's 68000, all of MAME's devices on the shim) runs in lockstep with MAME: main RAM on all 9,000 frames, 150/150 snapshots, all 2.15 M traced device accesses, sound lag 0. Regress case `bmiidx3-boot`.<br>• **TW-2 (2026-10-13):** IIDX controls, lamps, the 16-segment display in the title, launcher; a scripted game (coins, Start, menus, a stage with keys and turntable, result) in lockstep with MAME; regress `bmiidx3-play`. Hands-on test pending.<br>• **TW-3 (2026-10-13), further than MAME:** the background video from the Video CD, as the game drives the DVD player, keyed into the PlayStation's black with the Bt812's colour controls; the attract intro and the stage videos play. Open: which clip goes with which song, the exact key and placement (real-cabinet footage). Regress `bmiidx3-video`.<br>• **TW-4 (2026-10-13):** the 573's R3000 recompiler on the game's code, the video decoded on a worker thread: 3.0 → 1.5 ms a frame on the desktop, a stage with the video 1.5 ms (p99 2.0). The CD loads between screens (up to ~40 ms a frame, MAME's per-byte SCSI model) answered 2026-10-15: the cabinet's 4x drive, a sector at a time (`--mame-cd` keeps MAME's), ~4.7 ms a frame.<br>• **TW-5 (2026-10-14), further than MAME:** the 1st style (`bmiidx`) boots, plays and shows its videos (MAME: E600 DVD INIT ERROR): SIO1 with the DVD player's CTS. In lockstep with a MAME given the same SIO1 (oracle/mame-patches); regress `bmiidx-clear`, `bmiidx-play`, `bmiidx-video`.<br>• `bmiidx3` (3rd style, MAME working) is the first set; `bmiidx` (1st style, MAME `NOT_WORKING`) arrived 2026-10-13.<br>• Phases TW-0 (oracle) → TW-1 (lockstep) → TW-2 (playable) → TW-3 (background video, beyond MAME) → TW-4 (N100 budget) → TW-5 (other styles, DVD).<br>• MAME ~570 % real time. |
| **M17** | A song added to pop'n music 14 (modding pilot, §20) | A new song, packed by a tool from the modder's own files, appears in the song list, plays with its sounds and its charts, and keeps its scores, through a mod folder alone (nothing on the user's disk changed). The pieces: a new entry past the last in the song list (lisbin), the list's integrity checksum made again (FUN_017c18b8, constants 0x3f306b90 / 0x70f93885), the sounds encoded in the game's ADPCM and placed in a free 20 MB slot of the song area (from 0x20000000, LCG-scrambled: tools/popnsong.py reads them), the charts in the game's format, and the song select, unlocks and score records accepting the new number. | **not started** (split from the pilot 2026-10-20). Most of it is reverse engineering still to do: the checksum's coverage, the chart format, and what else counts the songs. |
| **M18** | Two more shaders: Lola Retro and the CRT beam simulator (§29) | Lola Retro as a picture mode with presets chosen per board; the Blur Busters beam simulator as a motion mode on 120 Hz+ displays, paced from the game's real rate; both from the launcher. | **done 2026-10-20** (§29): the engine (feedback, frame history, FrameCount in game frames), Lola as a picture mode with looks per monitor, the CRT beam (`--crt-beam`); the owner, at 240 Hz: Lola "perfect", the beam "perfect" (after its curve and whole-refresh fixes). |

Rough total to playable BM3 Final is about 4 months. Each later family should cost weeks, not months, if the SDK-signature bet in §3.3 holds.

---

## 6. Risks and how they're handled

| Risk | Mitigation |
|---|---|
| Thread library is more exotic than it looks (e.g. switching inside ISRs) | Read the `0x80003400–0x80003630` asm early, in M1. Fallback: keep CPU state in `Ctx` and run threads cooperatively on one host thread. |
| Missed `blrl` targets / data-as-code | Fallback interpreter + logging + oracle coverage census. |
| SPU timing (keysound cutoffs, pop'n reg 09 streaming) | An audio-clock-driven scheduler is deterministic. Compare DPRAM/RF5C400 traffic against MAME. HLE the keysound path if needed. |
| GCU unknowns (status bits, reg 6c, flips) | Start from MAME's behaviour and hacks. Fix per-game issues against real-hardware video where available (`BM3-videos/` has 270 reference recordings). |
| Other games don't share the SDK | **Mostly retired for the kernel** (identical glue in 5/5 programs; C kernel matched in pop'n 5–7). Still open for the driver libraries. Where code isn't shared, it costs manual labelling, but the translator and devices carry over unchanged. |
| XT446 synth (KBM) | Done with MAME's H8S/2655 + SWP30 on a framework shim. The SWP30 is pinned to a MAME master commit newer than the 0.289 oracle, for its accuracy fixes (0.289 had e.g. an unstable IIR2). |
| Legal | Ship only the tool and runtime. Users provide dumps. Generated C, assets and ROMs are never committed. The MAME-derived code is BSD-3 (firebeat.cpp, k057714.cpp, rf5c400.cpp), so keep attribution. Check each borrowed core's licence before use. The BGA tools zip is GPLv3, which matches HIRAKU's licence. |

---

## 7. Housekeeping items found during the survey

- **BGA VM opcode base is off by 5 in the old `BM3-extract` notes only.** The dispatch table starts at `0x800E560C` (entry 0 = END `0x80044DBC`; checked in the binary). `ghidra_scripts/DumpVM.java`, `bga_vm.py`, `bm3vm.py` and `REVIEW.md` read it from `0x800E5620`. The EZ2 port (`EZ2-projects/port/BM3-BGA.md` §5, `scene/bm3bga.c`) already uses the correct base, so nothing needs filing there.
- REVIEW.md overstates the judge work. `gates.c`, `miss.c` and `stops.c` are BGA hit/miss hooks, not the gameplay judge. Real judge windows and gauge are still unknown. The recomp gets them for free, and they can be read straight out of traces.
- `bm3mame/patch.lua` uses 1st-mix offsets and does not apply to `bm3final`.

---

## 8. Decisions (settled 2026-10-11)

1. **Order:** BM3 Final → other BM3 → pop'n → ParaPara; Keyboardmania is a stretch goal.
2. **Fidelity:** match MAME first, enhance later. HLE shortcuts (e.g. the direct keysound path) only after the register-faithful path works and is checked against the oracle.
3. **Platform:** Linux first (this machine, CachyOS). Windows later.
4. **Licence:** GPL-3.0-or-later. MAME-derived files keep their BSD-3 notices. The GPLv3 BGA tools may be merged in directly.
5. **rizu / Route B is dropped.** All effort goes to HIRAKU. Bugs found in existing implementations are reported to the EZ2 port (`EZ2-projects/port`, the active project), following its `NEEDS-OWNER.md` / `REQUESTS.md` conventions.

## 9. M1 outcome (2026-10-11)

Details are in docs/RECOMP.md.
- **Semantics:** one header (`include/hiraku/ppc.h`) defines every instruction for both the interpreter and the generated code. 21,600 QEMU-referenced cases over 108 forms pass (`ctest`).
- **Translation:** all of bm3final and pop'n 4–7 translate (2,168–2,487 functions each) and compile in seconds. Two things had to be handled along the way:
  - **return-up epilogue helpers** (GCC `_restgpr_*_x`);
  - **secondary entries**: branches into the middle of functions.
- **Running:** in interpreter mode each program boots through the kernel into its threads. Shadow checks have found **0 mismatches** between native and interpreted functions so far (bm3final: 153 checked calls; each pop'n: 143–151).
- **Next (M2):** host fibers per guest thread, interrupt delivery and devices. Until then:
  - native execution stops at the first thread switch;
  - the game waits on hardware that does not exist.

## 10. M2 outcome (2026-10-11)

The full write-up is in docs/M2.md.
- **Native code runs the real game on host fibers.** BM3 Final boots through the BIOS-less entry into its multithreaded kernel, with 3 guest threads and about 2.3M fiber switches in 30 virtual seconds.
- **The STARTUP self-test runs on screen** (GCU port, software renderer):
  - **OK:** I/O, IBTN (DS1991 dongle over the 403 serial port), VER, BACKUP, RTC, FDD, ST224, ATAPI, ISO (ATAPI CD reading the user's disc image);
  - **E704:** SPU (no sound board until M4); HDD is behind the SPU.
- **Oracle agreement:**
  - The runtime's MMIO stream matches MAME record for record (`oracle/align.py`, with interrupt-handler accesses and host-clock RTC data masked) for **23,707 records**, up to the first SPU mailbox read.
  - The floppy and MIDI-UART sub-streams match for 63 and 128 records.
- **Shadow mode is clean over boot:** 271 native-vs-interpreted calls identical, 0 mismatches. Calls that touch devices or time are abandoned as impure before they act.
- **Real time:** `--window` (SDL3) shows the game paced to the wall clock: 11.7 virtual seconds took 11.8 s.
- **Performance work outstanding (M9):**
  - The boot RAM clear (356M instructions) and the exception vector glue (about 90 instructions per exception) are interpreted, which is most of the CPU time.
  - Recompiling the vector code is the obvious next step.

## 11. M4 outcome (2026-10-11)

The full write-up is in docs/M4.md.
- **The SPU works:** a 68000 (Musashi) running Konami's sound program, a port of MAME's RF5C400, the mailbox, an HDD on the SPU's ATA bus feeding wave RAM by DMA, and the spectrum analyser.
- **beatmania III The Final:** all startup checks pass, attract mode runs, and the oracle's practice script plays a song with sound.
- **Against MAME:** audio is aligned at 0 ms lag, with RMS within about 5–15%.
- **Lesson:** the skipped BIOS time must still pass for the other boards (`--bios-seconds`).

## 12. M3 outcome (2026-10-11)

The full write-up is in docs/M3.md.
- **Renderer:** bit-exact against MAME on 28/28 frames when replaying MAME's GCU writes.
- **BGA engine:** in lockstep with MAME, 98.8% of song frames byte-identical, at a constant 13-frame song-start offset.
- **Coverage:** no interpreter fallbacks over a full session.

## 13. M5 outcome (2026-10-11)

The full write-up is in docs/M5.md; how to play is in docs/PLAY.md.
- **Game loop:** the full loop (attract → coins → stages → results → game over → attract) runs natively.
- **Real time:** `--window` paces on the audio clock and has keyboard and joystick controls.
- **Latency:** the game's own key-to-keysound latency is 2–20 ms, mean 9.5 ms (measured hands-on). The game scans its keys at 60 Hz, as on the cabinet. The rest is the host audio path.
- **Saves:** NVRAM persists.
- **Pending:** a hands-on test with real audio hardware and controllers.

## 14. Efficiency on N100-class PCs (2026-10-12)

**Goal (owner):** what HIRAKU should beat MAME on, besides accuracy:
- **efficiency:** MAME's BEMANI drivers hiccup on small PCs;
- **latency;**
- **I/O:** analog turntables, lamps.

The target machine is an **N100-class x86 mini PC**: 4 cores, roughly 60 % of a desktop core's speed, 6 MB L3 cache. Judge new work by its worst frames on that kind of machine, not by averages on a fast desktop. The 5700X3D used for these numbers has a 96 MB L3 cache, which flatters emulators.

**Tools:**
- `tools/regress.py record|check|bench` runs four cases (bm3final practice, popn7, ppp, kbm attract).
  - `check` compares a hash of every frame's picture, a running hash of the sound, and the run statistics with a recorded baseline.
  - Every change below is **bit-identical**. The GCU replay also stays pixel-identical to MAME on 28/28 frames.
- `--digest FILE` writes those per-frame hashes.
- Every run prints `frametime:`: host work per frame (pacing waits excluded) as mean/p50/p99/p99.9/max. For the worst frame it also gives the split into disk, draw, 68000 and glue time.
- `HK_PROFILE=1` now also names samples inside shared libraries.

**Results** (share of one desktop core, 5700X3D, headless):

| Case | Before | After |
|---|---|---|
| bm3final practice (gameplay) | 17.0 % | 9.6 % |
| popn7 attract | 7.0 % | 5.0 % |
| ppp attract | 5.5 % | 3.0 % |
| kbm attract | 43.8 % | 38.2 % |
| bm3final 90 s attract vs MAME `-bench` | 7.7× real time (MAME 3.6×) | 12.3× real time, **about 3.4× MAME** |

**The MAME-device cores against MAME (2026-10-12 night).** The same method as above: MAME's `-bench` against HIRAKU headless, the same 150 s of game time, 3 runs each, on the same desktop (wall time, start-up included).

| Case | MAME (flatpak 0.289) | MAME at the pin | HIRAKU | HIRAKU per frame |
|---|---|---|---|---|
| `ddr2m`, from the installed flash (flash check, attract with 3D, demo, video) | 650 % real time | 595 % | 683 %, **1.05× MAME** | mean 2.46 ms, p99 4.0, worst 4.8 |
| `bm1stmix`, power-on to attract | 1,807 % | — | 1,762 %, **0.98× MAME** | mean 0.94 ms, p99 1.7, worst 2.2 |

- **These cores are level with MAME, by design.** They run MAME's own CPU and device code; only the scheduler and memory map are HIRAKU's. Firebeat's 3.4× comes from the static recompiler.
- **DJ Main needs nothing more.** Under 1 ms a frame is under 2 ms on an N100.
- **System 573 is at the edge of the budget on an N100.** Scaled by ~0.6, ddr2m is ~4 ms a frame on average (~25 % of a core) and ~8 ms at worst. Section 19's 573-4 has the profile.
- **With `tools/pgo.py` (2026-10-13):** ddr2m runs at ~950 % real time, ~1.45× MAME. Mean 1.76 ms a frame, worst 4.2 ms; ~2.9 / ~7 ms scaled to an N100.

**Profile-guided optimisation for every core (2026-10-13, `tools/pgo.py`).**
- It trains on the regress cases whose games are ingested and keeps one profile per core (build/pgo/<core>.profdata, never committed).
- Per core, because the cores build the same sources into separate copies: Musashi as a 68000 and as a 68EC020, MAME's shim for the XT446 and for the 573. A single shared profile blended them, and DJ Main and ddr2m got slower.
- CPU seconds per case, 2 runs each, every case bit-identical:

| Case | Before | With PGO |
|---|---|---|
| kbm attract | 23.35 | 18.8 (**−19 %**, mean 5.2 ms a frame) |
| popn7 attract | 3.10 | 3.00 (−3 %) |
| ppp attract | 1.83 | 1.80 (−2 %) |
| bm3final practice | 19.35 | 19.12 (−1 %) |
| ddr2m, 150 s from the flash | 21.9 (wall) | 15.9 (**−27 %**) |
| bm1stmix boot | 8.71 | 8.75 (no profile) |

- **Profiled:**
  - Firebeat's runtime, its SPU's 68000 and the XT446 (the synth card's SWP30 is where kbm gains).
  - The 573's MAME code.
- **Not profiled:**
  - Firebeat's recompiled game code: no gain, and only the trained games would get a profile.
  - DJ Main: 5 % slower with its profile.
- **The training mix matters:** ddr2m was 1.44× with a profile trained without the flash-install run, and is 1.38× with it.
- **A measurement note:** the regress cases write a snapshot every 60 frames to out/ on the DATA disk. Single 20–200 ms "frames" in `regress.py bench` are those writes, not the machine: with out/ on tmpfs, bm1stmix's worst frame is 2.2 ms.

**What it took:**
- **Fibers:** guest thread switches use a small x86-64 context switch instead of `swapcontext`. `swapcontext` made a system call (signal mask) on each of ~48,000 switches per second, which was 14 % of a run.
- **GCU:** blits, fills and scanout run per row wherever a row neither wraps VRAM nor overlaps itself. Same-parity rows are plain array copies; opposite-parity rows use a fixed pair shuffle. Full-brightness scanout uses SSE2. Every other case keeps MAME's per-pixel loop.
- **Spectrum analyser:** twiddle factors are computed once per sample rate instead of per sample (same values).
- **Profiling scopes:** they read the TSC, not `clock_gettime`. The scopes run on every guest exception.
- **Keyboardmania:** the two-screen rotation works in 32×32 tiles.

**Not done, and why:**
- **Idle skipping (§3.2).** bm3final's idle time is ~9.7 M `yield()` system calls in 200 s, from 96 wait-loop call sites (`f_800c787c` → kernel function `0x800c7830`). Skipping them changes kernel state unless the whole scheduler cycle is proven periodic. Deferred.
- **A render thread.** Scanout now costs ~0.18 ms per frame on this PC. A thread would also need VRAM snapshots or write fences, and would move presentation off the vblank. It isn't worth that.

**Where the time goes now:**
- bm3final gameplay:
  - GCU blits ~25 %, close to memory speed;
  - recompiled game code and kernel ~20 %, much of it the yield churn;
  - 68k SPU ~8 %;
  - scanout ~8 %.
- Keyboardmania: the SWP30's MEG effects DSP (MAME code) is ~40 % of its run, so kbm is the one family likely to struggle on an N100.

**Keyboardmania's synth card (later on 2026-10-12):**
- The XT446 runs on a thread of its own (`runtime/firebeat/synth.cpp`), beside the game's CPU.
  - The game only sends it MIDI. The board queues "render n samples" and "MIDI byte" in the same order as before, so every byte lands on the same sample.
  - The board holds each frame's audio and adds the card's samples at vblank, with the same sums in the same order. The output is bit-identical (`kbm-attract`).
  - Latency is unchanged: the frame's audio is handed on at the same point.
  - `HK_XT446_INLINE=1` runs the card on the main thread, as before.
- The MEG's interpreter is about 8 % faster (`third_party/mame/README.md`), bit-identical with MAME's `step()`.
- Measured on the 5700X3D, kbm attract:
  - mean host work per frame 7.18 → 6.46 ms;
  - worst frame 12.8 → 10.6 ms.
- The card is now the critical path, at about 5.3 ms a frame:
  - the MEG is 64 % of it, about 12 ns a step, limited by the serial dependencies between its steps rather than instruction count;
  - the H8 firmware does real work (7.3 M instructions/s, no idle loop to skip).
- Tried and dropped, bit-identical but no faster:
  - a branchless MEG ALU (slower);
  - a fast path for arithmetic-only steps (1 %).

**Compiled MEG programs (2026-10-15).** The firmware loads only a handful of effect programs (five across all four Keyboardmania games' attracts and a played song), and nearly every step of them works (329–375 of 384 use the ALU: no idle steps to skip). `tools/meggen.py` compiles each recorded program into straight-line C++ with its fields and its delay-slot contents as constants (third_party/mame/README.md); unrecorded programs stay interpreted.
- The card alone, 120 s of attract MIDI: 32.5 → 24.3 s of CPU. kbm attract: 32.4 → 25.6 % of a core, mean host work 5.65 → 4.42 ms a frame, worst 9.46 → 6.17 ms. Bit-identical (`kbm-attract`, the card's WAV).
- Folding the decode alone gave 6 %; the delay slots (which register, pending index and memory writes, known per step within a program) gave the rest.
- Tried and dropped: a typed fast path for MAME's memory caches in the shim (the H8's fetches, the MEG's reverb RAM): no gain.
- What remains: the MEG's real arithmetic (~45 % of the card) and the H8 firmware (~55 %; MAME's cycle-exact core, a recompiler would be the next big step).

**Next:**
1. Measure on real N100 hardware, in `--window`, with `HK_LOG_STALLS=1`.
2. The XT446 (kbm) is ~3.5 ms a frame on one core now. What's left is structural: a faster H8 core (a recompiler for the firmware), or the card on two threads (the H8 and the MEG).
3. ~~Read-ahead for the large images.~~ **Done 2026-10-12** (`runtime/dev/ata.cpp`):
   - A READ's sectors are now fetched with one `pread` per stretch, not a stdio seek and read per sector.
   - Each READ also asks the kernel for the next 8 MB. In bm3 practice, 600 of the 615 READs on the sound hard disk continue the previous one.
   - Measured with the image evicted from the page cache (`posix_fadvise(DONTNEED)`, no root needed), on a DRAM-less SATA SSD:
     - bm3final practice, worst frame 251–335 ms → 9.8 ms;
     - pop'n 8's DVD reads, 4 ms → 0.06 ms in their worst frame, warm or cold.
   - Bit-identical (`tools/regress.py`).
   - What follows is the original note. CD images up to 1.5 GB are already warmed in the background, and every READ gets a `WILLNEED` hint (`ata.cpp`). The hard disk (bm3, 20 GB) and the DVDs (pop'n) only get the hint, issued just before the synchronous read, so it gives little lead time. One run had a single 125 ms frame at a song load that never reproduced, most likely a cold host file cache. Slower storage on an N100 makes that likelier.

## 15. Other BEMANI platforms as cores (discussed 2026-10-12)

**Idea:** one shell (launcher, `host/`, scripts, the MAME oracle, lamps and controls mapping) and several machine **cores** behind a common interface. The interface covers:
- start-up from the game config;
- running to a time;
- the framebuffer, and sound pulled by the host;
- a per-cabinet description of controls and lamps;
- NVRAM and flash.

The coupling to remove is `runtime/host/{window,input,lights,script}.cpp` including `firebeat/board.h`.

| | DJ Main | System 573 |
|---|---|---|
| Hardware | 68EC020 at 16 MHz, 1 MiB EPROM, 64 KiB RAM; K056832 + K055555 video; 2× K054539; ATA hard disk (password-locked from complete MIX on) | PS1 (R3000A + GTE, GPU, SPU) with an ATAPI CD, PCMCIA flash and security cassettes; digital I/O board (FPGA + MAS3507D MP3) |
| Games | beatmania 1st mix → THE FINAL, complete MIX 1/2, Club MIX, CORE REMIX, DCT, hiphopmania/beatstage, pop'n music 1–3, pop'n stage/EX (~20) | DDR 1st → Extreme, GuitarFreaks 1st–11th, DrumMania 1st–10th, Dance Maniax, Mambo a GoGo, Martial Beat (~60) |
| MAME | every set working, no flags | every BEMANI set has imperfect sound. The analog-board games (DDR 1st/2nd, GuitarFreaks 1st/2nd) work; almost every digital-board game is NOT_WORKING (§19) |
| CPU plan | interpret (Musashi; trivially cheap) | interpret to bring it up, then statically recompile the MIPS (the N100 budget needs it) |
| Main risk | MAME's tilemap/graphics framework is tangled into K056832 | the MP3/FPGA sync that DDR's timing depends on; the size of a PS1 core |

**Reused from Firebeat:**
- Musashi;
- `ata.cpp`;
- the CD-audio path;
- the `third_party/mame` shim for MAME devices (BSD-3);
- the analog turntable input and lamps;
- the oracle tooling, which works on any MAME driver.

**Licences:** DuckStation's licence (non-commercial, no-derivatives since 2024) is incompatible with HIRAKU. MAME's PS1 devices (BSD-3), Mednafen/Beetle PSX or pcsx-redux (GPL-2+) are compatible.

**Bonus:** Twinkle (beatmania IIDX 1st–8th) is a PS1 board with a 68000 + RF5C400 sound board, so a 573 PS1 core covers most of it (M15). MAME emulates the external DVD/VCD player's serial control but not the video itself: its driver lists "emulate dvd player and video mixing" as a TODO. Showing the background videos would be something HIRAKU does that MAME doesn't.

**Media:** on hand since 2026-10-12 (§16).

## 16. DJ Main, System 573 and Twinkle media (2026-10-12)

**Where it is:**
- MAME rompath `~/bm3mame/roms-cores/`. It holds hard links to the downloads in `~/Downloads`: the `.7z` sets, and each set's CHDs in a folder named after the set.
- Every set is non-merged: each archive carries its BIOS (System 573 `700*.22g`, Twinkle `863a*`) and its parent's files.

**`-verifyroms`:** all five sets pass, and every CHD has a non-zero data SHA-1.
- `bm1stmix`, `popn1j` and `bmiidx3` are "good".
- `ddr3mj` and `drmn10m` are "best available": MAME itself flags their cassette chips (and drmn10m's discs) as BAD_DUMP. The same goes for every copy.

**Boot test in MAME** (MAME 0.289, 90–150 s, headless, no inputs):

| Set | Platform | Disc(s) | Where it stops | MAME speed |
|---|---|---|---|---|
| `bm1stmix` beatmania (1997) | DJ Main | `753jaa11` (HDD) | title screen | 1,961 % |
| `popn1j` pop'n music (1998) | DJ Main | `803jaa11` (HDD) | title screen | 1,921 % |
| `ddr3mj` DDR 3rd Mix | 573, digital I/O | `887jaa02` | ERROR 147: flash checksum. Needs the first install: set MAME's cassette slot to "install", reset, install, then back to "game" (ksys573.cpp, note 3). | 672 % |
| `drmn10m` DrumMania 10th Mix | 573, digital I/O | `d40jaa02` (runtime), `d40jba02` (multisession), `d40jca02` (HDD installer, needs the network unit) | INFO 508: install from CD to flash done, power-cycle to play | 657 % |
| `bmiidx3` IIDX 3rd style | Twinkle | `gc992-jac01` (program CD), `gc992-jaa04` (Video CD), `992hdda01` (HDD) | device check all OK but backup RAM: E150, 1P Start clears it (normal first boot) | 567 % |

**What this means for the plan:**
- **DJ Main is already cheap in MAME**, about 5 % of a desktop core. A DJ Main core is worth building for latency and I/O (analog turntable, lamps, controller mapping), not for efficiency.
- **573 and Twinkle cost about 15–18 % of a desktop core in MAME**: roughly 30 % of an N100 core, before MAME's video and sound output. Efficiency matters there.
- **First boot:** every new platform needs its first-boot steps, as Firebeat did (ppp, pop'n 6). For the MAME oracle, keep a seeded NVRAM per game, as with kbm.

## 17. The core split and the first DJ Main core (2026-10-12)

**M12, the core split:**
- `runtime/shell/core.h` is the machine interface: init, run, frame and audio hooks, inputs by MAME name, lamps, guest memory for scripts, statistics.
- `runtime/main.cpp` is the shell; `runtime/firebeat/core.cpp` and `runtime/djmain/core.cpp` are the cores. The cabinet tables moved to `runtime/shell/cabinet.cpp`.
- Firebeat stays bit-identical (`tools/regress.py`).

**M13, the DJ Main board** (`runtime/djmain/`):
- Musashi as a 68EC020, MAME's memory map and handlers.
- K056832 ported from MAME, with its DJ Main drawing path.
- K055555 priorities, sprites with MAME's zoom.
- Two K054539s ported from MAME.
- The shared ATA code, with MAME's drive timing on top.
- Interpreting the 16 MHz CPU costs about 1 ms of host work per frame (MAME runs these games at ~1,960 % of real time).

**What it took to match MAME, found with frame/time-stamped oracles** (`oracle/snapcmp.py`, MAME Lua taps, MAME debugger traces):

1. **A Musashi bug.** With `M68K_EMULATE_010` off, the EC020 is decoded as a 68000 in places: no scaled index, no memory-indirect addressing. beatmania's `jmp ([8,pc,d3.l*4])` switch went astray and its loader never ran again. Fix: the option is on (third_party/musashi/HIRAKU.md).
2. **MAME's 68020 cycle costs.**
   - Shifts and rotates have no per-bit cost before the 68030.
   - MOVEM stores cost 3 cycles a register, not 4.
   - Musashi's costs made the CPU ~1 % slow, enough to miss a polling window after the sound-data load.
   - Now each disk command lands a constant 1.40 ms from MAME's.
3. **MAME's drive model** (atastorage.cpp):
   - seek time from the head position, which follows every sector read but the last;
   - 10 µs for simple commands, BSY while busy;
   - the interrupt delivered when the busy time ends;
   - no interrupt at the end of a PIO read (`AtaBus::pio_end_irq`, off for DJ Main; Firebeat's SPU depends on it);
   - the interrupt line passed on only when it changes.

**Lockstep with MAME (later the same day):**

4. **The vblank begins at multiples of the frame period** (MAME's screen: the beam is then at line 384). Its interrupt, the screen update and the frame notifier all happen there, not 384 lines into the frame. This was the constant 1.40 ms offset, from the moment the game enables the interrupt.
5. **A TRAP costs its own 4 cycles plus the whole exception** in MAME: 24, where Musashi charged 20. beatmania's kernel calls are TRAP #0.
6. **Musashi only looks at the interrupt lines when `m68k_execute()` starts.** MAME's scheduler aborts the CPU's timeslice whenever a device changes a line, so the interrupt is taken after the current instruction. The board now ends the timeslice when the IDE or vblank line changes. Single-stepped trace runs had hidden this.

**Result:**
- Both games run in lockstep with MAME through power-on, self-test, the disk loads and attract mode (155 s):
  - 150/150 snapshots pixel-identical with no frame offset;
  - the sprite list and palette identical on all 9,000 frames;
  - all 135 disk commands within 25 µs (the precision of the MAME taps);
  - 813 kernel calls compared one by one, identical to within half a microsecond.
- **The K054539 output is sample-identical to MAME's WAV** (`HK_DJ_RAW48`), one 48 kHz sample apart.
- Both games are cases in `tools/regress.py` (bm1stmix-boot, popn1j-boot).
- Host cost: ~1 ms a frame (5.7 % of a desktop core), most of it drawing.

**Gameplay and cycle-exact lockstep (later the same day).** `oracle/scripts/bm1stmix-play.his` plays a game:
- 2 coins and 1P start;
- the turntable picks "skip" at the practice-stage prompt;
- sound select;
- "u gotta groove", played with a fixed pattern of keys and scratches; it fails;
- continue countdown, game over, back to attract (217 s).

Comparing it with MAME, and tracing both CPUs instruction by instruction (MAME's debugger `trace` with `totalcycles` against a temporary Musashi instruction hook), found:
7. **MAME's Lua input lands a frame late.** MAME latches digital inputs in its frame update, which runs before the Lua frame callback. A button the script presses at frame F is seen from F+1; an analog value it sets is read live. The shell now delays a script's button changes by `Core::script_button_delay()` (1 for DJ Main, 0 for Firebeat, whose recorded runs predate it). Host input is not delayed: it matches MAME's own keyboard path. The turntable field is `TT1/Dial`, not `TT1/P1 Dial`.
8. **The K054539's key-on status and channel positions only move at MAME's stream updates:** on a write to that chip and 50 times a second. MAME doesn't update the stream on reads. beatmania's sound driver counts frames while a channel plays, so it saw samples end up to a frame early. The board still renders on every access (for latency); reads return the registers as of the last commit (`K054539::commit()`).
9. **Musashi's reset exception (4 cycles on a 68020) wasn't counted**: `m68k_execute()` took it from the first slice without reporting it, so the whole run sat 4 cycles early and frame-boundary interrupts landed an instruction apart.
10. **An idle loop (`bra` to itself) that ends exactly on a slice boundary ran once more** in Musashi (`remaining % cost` is 0 there).
11. **MAME's IDE drive sets the index-pulse bit (IDX)** on a status read once more than a rotation (1/90 s) has passed since it last did. The game copies the status into RAM.
12. **MAME's drive times are exact attotimes, and the CPU stops on the last whole clock at or before them.** HIRAKU rounded up: 400 ns is 6 clocks, not 7.
13. **MAME's 68k checks the slice before its first instruction (a `while` loop), Musashi after it (`do`-`while`).** An interrupt arriving during another's exception (the vblank 6 clocks after a disk interrupt) is taken before the first handler instruction.

Points 9, 10 and 13 are opt-in Musashi hooks (`third_party/musashi/HIRAKU.md`), so the Firebeat SPU is unchanged.

**Now:** cycle-for-cycle lockstep with MAME.
- **Boot, both games:** the whole work RAM, the sprite list and the palette are identical on every one of the 9,000 frames, and all snapshots match.
- **beatmania gameplay:** the same for all 12,600 frames, with 210/210 snapshots identical.
- **Disk:** every disk command is issued on the same CPU clock as in MAME (217 of 217 in gameplay).
- **Sound:** the chip output is sample-identical in all three runs (10.4 M samples in gameplay, keysounds included).
- `bm1stmix-play` is the third DJ Main case in `tools/regress.py`.

**pop'n music 1 gameplay** (`oracle/scripts/popn1j-play.his`, 233 s) matched MAME on the first run:
- the script: 2 coins, the red button starts, game mode select, a blue Sel button moves the genre twice, then "Quick Master" played with a fixed pattern over all 9 buttons, the continue countdown, ranking, game over and attract;
- work RAM, sprites and palette identical on all 13,500 frames, and 225/225 snapshots;
- the sound chips sample-identical (11.2 M samples);
- it's the case `popn1j-play` in `tools/regress.py`.

**Every MAME DJ Main set (2026-10-12).**
- `games/<set>/game.toml` for all 29 sets, generated from MAME's driver.
- `map = "u"` / `"a"` for MAME's `main_map_u` / `_a`: the IDE ports and tile RAM elsewhere.
- `djpstage` for pop'n stage EX: 10 buttons plus 3 selection buttons; lamps not decoded.
- CMake builds a binary for every `core = "djmain"` file.
- The **ATA password lock** is ported from MAME (`runtime/dev/ata.cpp`):
  - a disk with passwords is locked at power-on and soft reset;
  - reads fail with ERR/ABRT;
  - SECURITY UNLOCK clears a password when its 32 bytes match;
  - both must be cleared;
  - `tests/ata/test_ata_security.cpp` covers it.
- Run against MAME (2026-10-12, 155 s boot): bm1stmix, popn1j, bm2ndmix and bm3rdmix are cycle-exact. bm4thmix–bmfinal, bmcmxaac and popn2/3 diverge in work RAM at ~73–79 s (see M13). bm3rdmixe boots; MAME 0.289 lacks the set.
- 2026-10-16: all 25 sets on hand are cycle-exact through the boot, after the rotate-cost and disk-geometry fixes (M13).

**Lamps.**
- beatmania's lamp table follows MAME's decoding bit for bit: buttons, start, effect, the four highlights, the neon and the board LEDs. The writes themselves are cycle-exact with MAME.
- MAME doesn't name pop'n's bits, so I worked them out from `popn1j-play`, registers logged against the scripted presses:
  - buttons 1–9 on reg 0 bits 0–4 and 6–9;
  - a 5-step top-lamp chase on reg 1 bits 6–10;
  - 4 side lamps on reg 1 bits 0–3;
  - reg 1 bit 15 is the coin counter and bit 14 the coin blocker.
- They take Firebeat pop'n's names (`runtime/shell/cabinet.cpp`, docs/LIGHTS.md), so one lights file serves both.

**Still open:**
- ~~The 48 → 44.1 kHz resampling.~~ Done 2026-10-12:
  - a core now says what rate it hands the shell (`Core::audio_rate()`), and DJ Main hands on the K054539s' 48 kHz as they are;
  - the SDL device and `--wav` follow that rate, so there's no resampling (most devices run at 48 kHz);
  - `--wav` now scales by 32768, as the chips and MAME do, so a DJ Main recording is sample-identical to MAME's `-wavwrite` (10.4 M samples in `bm1stmix-play`).
- **The ATA security unlock**, for complete MIX and later.
- **Faster drawing:** the tilemaps are drawn per pixel.

## 18. Firebeat music against MAME (2026-10-12)

Firebeat runs recompiled code, so it can't be in lockstep with MAME. What can be checked is whether the music keeps time. The tool is `oracle/audiosync.py HIRAKU.wav MAME.wav`:
- it finds the offset between the runs from the loudness envelopes, then refines it on the waveform (music repeating every couple of beats fooled the envelope alone);
- it reports the lag, waveform correlation and level in 2 s windows, with levels measured at each file's own rate.

**Setup.**
- MAME sets for `ppp` and `popn8` are in `~/bm3mame/roms-fb`: HIRAKU's ROM files plus the CHDs from ~/Downloads, hard-linked.
- Both machines start from the same saved state: HIRAKU's `--nvram` directory, and `HIRAKU_NVRAM=dir oracle/mame.sh ...` for MAME.
- From blank NVRAM, ppp stops at a flash-data error on both, wanting test mode.
- From blank NVRAM, pop'n 8 spends about a minute at its flash check in MAME. HIRAKU raced through it: its flash chip (`runtime/dev/flash29f016a.cpp`) erased instantly.
  - It now takes MAME's erase times: 1 s per 64 KiB sector, 16 s for the chip, close to the datasheet's typical values.
  - Until an erase ends, reads return the toggling status and writes are ignored.
  - pop'n 8's first boot now follows MAME's: the flash check, then the logos and attract.
  - It costs a minute or so on a first boot, as on the real board. The regress runs don't erase flash, so they're unchanged.

**Results, 2 minutes of attract:**
- **pop'n 8, DVD-streamed BGM** (RF5C400 streaming): in step to within 1 sample over 28 s (no drift), waveform correlation 0.998–1.000, the same level.
- **ParaParaParadise:** its attract music turned out to be YMZ280B sample playback, not CD audio: with the YMZ280B muted, HIRAKU is silent. The samples are the CD's music track, ADPCM-coded into flash.
  - Timing: from 78 s on, lag 0 samples, correlation 0.94–1.0, no drift. Checked against the disc's own track, both emulators jump between samples at the same moments by the same amounts.
  - The first sound comes 4.4 ms later in HIRAKU relative to the rest.
  - Level: the same below 4 kHz, within 0.3 %. Above 8 kHz HIRAKU has more: +5 % at 8–12 kHz, up to +35–46 % at 16–20 kHz. HIRAKU brings the chip's 88.2 kHz down to 44.1 kHz by averaging pairs, which filters less than MAME's resampler. Overall 1.00–1.03.
  - A sharper downsampling filter would match MAME better and alias less, but it changes Firebeat's sound digests, so it's left as a choice.
  - (The first version of this check reported "0.7 dB louder": `audiosync.py` resampled MAME linearly, which dulls its top octaves. Levels are now measured at each file's own rate.)
- HIRAKU starts 3.45 s (ppp) and 11.4 s (pop'n 8) ahead of MAME because it skips most of the BIOS boot.

**pop'n 4–7 and Animelo 1/2**, 2 minutes of attract each. pop'n 4, 6 and Animelo 2 started from saved state; the others blank.
- **Within each piece of music, every one is sample-exact with MAME:** correlation 0.99–1.000, level 1.00–1.01, no drift.
- **Between pieces, start times differ by 8–14 ms** (Animelo 2: 8–12 ms; pop'n 5 and 7: 14 ms; pop'n 6: 10.6 ms), earlier or later than MAME, about half a frame. The next likely cause is HIRAKU's DVD drive answering READs at once, where MAME models read times, as DJ Main needed for its disk.
- **pop'n 4 after 82 s and Animelo 1 at 88–100 s play different content** from MAME: the attract sequence goes a different way. Not looked into.
- The SPU's DVD drive gets ATA command E2 (STANDBY) and aborts it, as MAME's ATAPI device does.
- MAME sets: `mame-roms/<set>/` beside the checkout. The CHDs were converted from the Firebeat-dumps images (`mame-roms/convert.sh`), so MAME warns about their checksums.
  - DVDs need `chdman createdvd -hs 2048`: MAME 0.289's DVD drive throws "unhandled exception" on chdman's default 4 KiB hunks.

**Not checked:** ppp's sensors, and gameplay rather than attract.

**CPU timing (2026-10-14).** beatmania III's attract ran 22–95 frames behind MAME's. Two causes, both fixed:
- **What an instruction costs.** HIRAKU charged every instruction one cycle. MAME's PowerPC front end (`ppcfe.cpp`, the 403 class) folds branches: `b` costs 0, and so does a `bc` that tests the CR when the instruction before it didn't modify the CR (also `mcrf` and the CR logical instructions); `mulli`/`mullw`/`mulhw` cost 4, `divw` 33, `sc`/`rfi` 3, `lmw`/`stmw` one per register. The startup's byte-by-byte RAM clear (11 instructions, 10 MAME cycles a byte, 32 MiB) alone ran 0.54 s long. `src/ppc/cycles.h` has MAME's table; the interpreter charges it per instruction, the recompiler per straight-line run (`HK_TICK`). The interpreter is now within a frame of MAME through bm3core's boot (first serial access 26.923 s against 26.945 s; first mailbox access 32.227 s against 32.246 s).
- **An interrupt after a native exception.** The interpreter's `rfi` takes an interrupt that came due while the vector ran at once; a recompiled vector's `rfi` returned to the glue, and the interrupt waited for the next scheduled event (up to the 2^20-cycle quantum, ~16 ms). The vblank handler ran 2–11 ms late and the kernel's timing work slipped with it: native ran 0.48 s behind the interpreter by bm3core's STARTUP screen. `run_vector` (runtime/core/glue.cpp) now kicks the scheduler after the vector when an interrupt is pending. Native is then within 2 ms of the interpreter.
- **Results:** the attract is 5–7 frames *early* now (bm3, 7th MIX, The Final), from 89, 85 and 95 frames late. pop'n music 7's second piece of music starts 15.5 ms late, where it started 13.8 ms early: still about a frame off, so its cause is elsewhere (the DVD drive answering reads at once is next). Its "NOW LOADING MUSIC DATA" phase is shorter.
- Every Firebeat regress digest changed with this; the baselines were re-recorded.

**Periodic timers (2026-10-14).** MAME's DVD drive answers at once, as HIRAKU's does (`atapihle.cpp`: "TODO: delay data"), and MAME moves the SPU's DMA one word every 350 ns, as HIRAKU does in bursts of 64. Yet pop'n music 7's long loads from the DVD took 3 % longer in HIRAKU (3,103 ms against 3,015; 1,574 against 1,532), measured from the main CPU's mailbox command to the SPU's answer.
- **Cause:** an event runs at the first poll after it is due, a few to a few dozen cycles late, and every periodic timer re-armed itself from that late "now": the DMA burst (1,478 cycles a period, so ~3 %), and less so the SPU's 500 Hz tick, vblank, the YMZ280B tick and Keyboardmania's key scan. MAME's timer callbacks run at their exact time.
- **Fix:** `Timer::again(period)` re-arms from the tick the firing was due at; those five use it.
- **Result:** the loads take 3,017 and 1,542 ms (MAME 3,015 and 1,532). pop'n 7's first piece of music now correlates 0.999 with MAME's (0.97 before). Its second piece still starts ~14 ms late: the main CPU's commands drift ~0.1 s against MAME's between loads, still to find. beatmania III is unchanged (5–8 frames early).

**The PPC403's own DMA (2026-10-15).** That main-CPU drift came in steps of 33–85 ms, each where the game uploads to the GCU: about 2.5 M words by the PPC403's memory-to-memory DMA (`mtdcr` at 0x8002c8f4 in pop'n 7), not by a CPU loop. HIRAKU charged 2 cycles a unit, a guess from M2; MAME's `ppc4xx_dma_exec` does the transfer at once, charging nothing. Now HIRAKU charges nothing either. The main CPU's commands then stay within ~10 ms of MAME's over all three loads (they drifted 0.18 s), the 1.5 s load takes 1,531.8 ms (MAME 1,531.9), and pop'n 7's second piece of music starts 5 ms late (13.8 ms early at first, then 15.5 and 14 ms late). beatmania III: 7–8 frames early, as before.

## 19. System 573 roadmap (M14, written 2026-10-12)

The approach is DJ Main's (§17): port MAME's devices, interpret the CPU, and match MAME cycle for cycle before anything else. Then make it cheap enough for an N100.

### What MAME has

Sources: MAME master `ksys573.cpp` (149 sets), `k573dio`, `k573fpga`, `mas3507d`, `k573cass`, and the PS1 devices in `devices/cpu/psx`, `video/psx`, `sound/spu`. Every file is BSD-3, which is fine for GPLv3.

**The board:**
- A CXD8530CQ (R3000A with GTE) at 33.8688 MHz with 4 MiB of RAM.
- The CXD8561Q GPU, and the SPU with 512 KiB of RAM.
- BIOS `700A01` (4 Mbit).
- 16 MiB of onboard flash: eight 29F016As, the chip HIRAKU already has (`runtime/dev/flash29f016a`).
- An M48T58 timekeeper, an ADC0834 for analog inputs, and two PCMCIA slots.
- A CR-589 ATAPI CD drive, whose CD audio goes into the board.
- The security cassette on the PS1's serial port 1: an X76F041, X76F100 or ZS01, some with a DS2401 serial number.
- JVS, for the memory-card reader.

**The I/O boards** (mapped at 0x1f640000):
- **Analog, GX700-PWB(F):** DDR 1st and 2nd, GuitarFreaks 1st and 2nd, DrumMania 1st, early Dancing Stage. Music is CD audio.
- **Digital, GX894-PWB(B):** DDR 3rd Mix and later, GuitarFreaks 3rd and later, DrumMania 2nd and later, Dance Maniax, Mambo a GoGo, Martial Beat. It has an FPGA that decrypts MP3 data, held in 64 MiB of EDO RAM, and feeds it to a MAS3507D. It also drives lamps.

**MAME's status:**
- Every BEMANI set is flagged `IMPERFECT_SOUND`.
- **The analog-board games mostly work in MAME:** DDR 1st (all but `ddrja`), 2nd Mix with its Link and Club versions, GuitarFreaks 1st and 2nd, and the analog Dancing Stage sets.
- **Almost every digital-board set is `NOT_WORKING`.** The exceptions are three Korean Percussion Freaks sets. DrumMania 1st (analog) is `NOT_WORKING` too. The driver doesn't say why.
- **Correction to §15 and §16:** `ddr3mj` is a digital-board game, not an analog one. Both 573 sets on hand, `ddr3mj` and `drmn10m`, are digital.

**MAME's timings, which decide how hard lockstep will be:**
- **CPU: one cycle per instruction**, including the load delay slots, with no wait states and no cache-miss cost (the execute loop in `psx.cpp`). That is easy to match exactly. It also makes a cycle-exact static recompiler straightforward: a block costs its instruction count.
- **CD: a fixed 30,000 CPU cycles per sector** (`ATAPI_CYCLES_PER_SECTOR`; the driver notes the BIOS needs at least 2,000).
- **The MP3 playback counter**, which the digital games read to place their notes:
  - MAME's FPGA counts machine time from the first MPEG frame sync, at 44.1 kHz, not decoded samples.
  - So the MP3 decoder can run on a worker thread, as the XT446 does, without changing what the game sees.

**Size of the port:** about 16,000 lines of MAME code.

| Part | Lines |
|---|---|
| CPU, GTE, DMA, IRQ, timers, SIO | ~5,200 |
| GPU | ~3,500 |
| SPU | ~3,200 |
| Driver parts | ~1,500 |
| Digital board (k573dio, FPGA, MAS3507D) | ~1,350 |
| Cassette chips | ~1,500 |

**Cost in MAME:** 650–670 % of real time, about 15 % of a desktop core (§16).

### Media needed

"Match MAME first" needs a game MAME runs. Neither set on hand qualifies.

Get one analog set:
- **`ddr2m`** (DDR 2ndMIX, CD `895jaa02`), recommended: DDR is the biggest family and its pads are plain switches.
- or **`gtrfrks`** (GuitarFreaks, CD `886__d02`).

Each is the analog board, an X76F041 cassette and one CD, nothing else. MAME's set holds the BIOS, the cassette data and the CHD.

### Phases

Each phase has an exit test. Sizes are assisted-effort guesses; DJ Main went faster than its estimate.

**573-0. Oracle and media** (a day or two)
- **`tools/ingest.py` for 573 sets:**
  - the BIOS and the cassette EEPROM;
  - the CD, via `chdman extractcd` (memory-capped);
  - a `game.toml` giving the I/O board, cassette type, PCMCIA cards and flash.
- **MAME reference runs** for each set: boot to attract, and a scripted song.
  - First-boot installs (CD to flash) are done once in MAME, and the flash, timekeeper and cassette are kept as seeded NVRAM (`HIRAKU_NVRAM`), as for kbm and pop'n 8.
  - `ddr3mj` installs with MAME's "install" cassette (ksys573.cpp, note 3).
- **Lua taps:** CPU state, main RAM, VRAM and SPU RAM at chosen frames. If Lua can't reach VRAM, use the debugger's `save` command.
- **Determinism:** two MAME runs from the same script must match. The M48T58's clock comes from the host, as Firebeat's RTC65271 did; pin it or mask it in comparisons.
- **Exit:** reference runs for `ddr2m` (or `gtrfrks`), `ddr3mj` and `drmn10m` that repeat exactly.

**573-1. The board, interpreted, in lockstep** (1–2 weeks)
- **What to build in `runtime/sys573/`:**
  - MAME's PS1 CPU (GTE, DMA, interrupts, root counters, the serial port for the cassette);
  - the GPU and the SPU;
  - the 573 memory map;
  - the flash bank, on the existing flash chip;
  - the timekeeper, ADC0834, analog I/O board, and X76F041/X76F100 cassettes;
  - the CR-589 on `runtime/dev/ata` (which already does ATAPI), with MAME's per-sector time;
  - CD audio, through the existing path.
- **Bring-up order:** the BIOS to its self-test, then its flash-against-CD check, then the game.
- **Measure the host cost by part** (CPU, GPU, SPU) with `HK_PROFILE`. This decides 573-4.
- **Exit:** the analog set boots and runs attract in lockstep with MAME: main RAM and VRAM identical at every snapshot, SPU output sample-identical. Regress cases are added.

**573-2. Playable analog games** (about a week)
- **Controls**, by MAME name in `cabinet.cpp`:
  - DDR: 4 panels per player and the menu buttons;
  - GuitarFreaks: 3 neck buttons, pick, wailing, and the effect knob (analog, through the ADC0834).
- **Lamps:** the analog board's outputs (`gx700pwbf_output`) mapped to the cabinet and pad lights (docs/LIGHTS.md).
- **Scripts:** a scripted game (coin, song select, a song with steps, results) in lockstep, like `bm1stmix-play`.
- **Launcher entries and PLAY.md.**
- **Exit:** a scripted song in cycle-exact lockstep with MAME, and a hands-on test by the owner, with real pads if available.

**573-3. Digital I/O, the MP3 games** (2–3 weeks; the main risk)
- **What to port:**
  - `k573dio` and `k573fpga`: MP3 decryption, the playback counter, lamp outputs;
  - the MAS3507D: MAME decodes with its `mp3_audio`; HIRAKU decodes on a worker thread;
  - the DS2401, and the ZS01 and X76F100 cassettes;
  - the multi-session and network units, stubbed as MAME does.
- **Games:** `ddr3mj` and `drmn10m` are already on hand.
- **Lockstep with MAME first.** Then find out why MAME flags these sets `NOT_WORKING`:
  - play them through in MAME, and read its bug tracker and git log;
  - depart from MAME only where a second reference shows it is wrong: hardware notes (psx-spx, community 573 research such as spicyjpeg's), or recordings from a real cabinet if the owner can get them;
  - each departure is a switch, so a MAME-exact mode stays available for the oracle.
- **Exit:** `ddr3mj` plays a full song with the MP3 in step with the arrows (`oracle/audiosync.py` against MAME's WAV), and `drmn10m` plays a song.
- **Progress (2026-10-13): `ddr3mj` installs, runs attract and plays a song, in lockstep with MAME.**
  - **What it took:**
    - MAME's install cassette: the slot's "install" option, from the set's `ge887ja.u1`/`u6`, with `--cassette install`. The core's own MAME devices already did the rest (digital board, FPGA, MAS3507D, YI cassette, PC Card).
    - The MP3 in the mix: the MAS3507D's stream as its 50 Hz updates produce it. It is never updated from the host side: it decodes and asks the FPGA for data as it updates, so that would change the machine. The mix waits for it, up to 20 ms. Other MP3 rates are resampled for the host only; this game's are 44.1 kHz.
    - `MAME_ARGS` in `oracle/mame.sh`, for slot options.
    - Regress cases that start from another case's NVRAM (the power cycle between install and game).
  - **The install** (`ddr3mj-install.his`, 16,200 frames, no inputs): main RAM identical to MAME on every frame, and all 27 NVRAM files (flash, PC Card, timekeeper, cassette) byte-identical after it.
  - **Attract** (`ddr3mj-attract.his`, 9,000 frames) and **a game** (`ddr3mj-play.his`, 16,000 frames: XANADU stepped through to its result, then stage 2):
    - main RAM identical to MAME on every frame;
    - the sound, MP3 included, matches MAME's WAV: lag 0, correlation ≥ 0.999, level 1.00.
  - **Speed** (recompiled, desktop): attract 0.48 ms a frame, the song 0.62 ms.
  - **Why MAME flags these sets NOT_WORKING is still open.** Its sources don't say, and so far the game behaves as MAME does. Next: play it through by hand, and compare the arrows with the music.
  - **Next:**
    - ~~the install cassette in the launcher~~: done (a first boot uses it; right-click "Boot once with the install cassette");
    - ~~`drmn10m`: ingest, the ZI cassette (ZS01 + DS2401), DrumMania's controls and lamps~~: done; the multi-session unit: below.
- **Progress (2026-10-13): `drmn10m` (DrumMania 10th Mix) runs as far as MAME does.**
  - **What it took:**
    - the ZI cassette (ZS01 + DS2401);
    - the digital board's outputs through MAME's `drmn_output_callback`;
    - DrumMania's inputs: the pads on IN2's stage bits, the cymbal and bass pedal on its buttons, select L/R;
    - the `drmn` family in `cabinet.cpp` (controls, lamps) and the launcher.
  - **The flow:**
    1. The first boot installs the runtime CD (`d40jaa02`) to the onboard flash by itself and shows INFO 508 (power-cycle).
    2. The next boot shows NOTE 605 (RTC-RAM cleared): Test goes on.
    3. The boot checks run, then **"Please call attendant": the multi-session unit can't be found.**
  - **MAME stops at the same screen.** Its `k573msu` is an empty stub, which is the likely reason behind its NOT_WORKING flag.
  - Regress cases `drmn10m-install` and `drmn10m-boot` (from what the install left), checked against MAME.
  - **The multi-session unit (GXA25)** is a box of its own:
    - a Toshiba TMPR3927 (MIPS R3900) running firmware from a 29F400 flash, `flash.20t`, which is in MAME's set;
    - an ATAPI CD drive with the multi-session disc (`d40jba02`, on hand);
    - four TC9446F audio decoders (MPEG-2 audio / AC-3);
    - two PC16552 UARTs, and its own M48T58.
  - It streams and mixes the music, so the songs' audio comes from it. The 573 talks to it over SIO1, the 8-pin mini-DIN cable. From DrumMania 4th Mix on the game won't run without it.
  - **There is an emulation to start from:** the branch `bemani_k573msu_wip` of 987123879113's (windyfairy's) MAME fork, last changed 2022-03, BSD-3. It has:
    - a TX3927 CPU in `mips1.cpp` (~1,500 lines);
    - `k573msu.cpp` (~600 lines);
    - `tc9446f.cpp` (~330 lines, decoding through pl_mpeg);
    - SIO1 changes in `psx/sio.cpp` (~500 lines).
  - It is a WIP, about 11,000 MAME commits behind, with TODOs (the CPU clock, an interrupt trigger, a buffer reset "hack").
  - **The plan:**
    1. Port it onto the shim as a departure from MAME: a switch, documented with its source. MAME-exact stays the default and the oracle's mode.
    2. Its SIO1 changes touch the PS1 device every 573 game uses: they go in only for games with the unit, or behind the switch, so DDR's lockstep holds.
    3. A second MIPS CPU then runs in the machine. MAME's `mips1` interpreter first; the recompiler later if it costs.
    4. Exit: DrumMania 10th Mix reaches attract and plays a song with its music.
- **Progress (2026-10-13): the multi-session unit runs, and DrumMania 10th Mix gets past it, further than MAME.** Attract (logo, title, hit chart, the How To Play demo) and a game (mode and music select, the first stage of 花火 with its music, the hit chart updated after) run.
  - **Ported** from windyfairy's fork (third_party/mame/README.md lists every change): the TX3927 in MAME's current `mips1.cpp`, plus SoftFloat 3; `k573msu`; `tc9446f`; and SIO1 as a UART (`psxsio1_uart_device`), which only games with the unit get.
  - **What it took, beyond the port:**
    - **The shim's `umask`** didn't repeat `umask16` across the 32-bit bus as MAME does, so the unit's 16-bit UART handlers were never called. The boot loader then failed its UART check: "DEVICE CHECK FAIL. HALT.", LED code 0xc0.
    - **MAME's ATAPI layer** rounds a chunk under the byte count limit down to whole sectors, so a REQUEST SENSE asked for 8 bytes at a time got none. Fixed as the fork does: such a chunk is all the data.
    - **MAME's 16550 lost a full TX FIFO:** 16 queued bytes looked like none. The unit's replies stopped after 16 bytes, and the 573 restarted the session every few seconds. Fixed with a count. This is also the bug behind Keyboardmania's silent XT446 in MAME.
    - **The TX3927 without cache emulation:** with MAME's caches and the fork's ignored `CACHE`, the unit ran stale code after its OS copied itself to RAM.
    - **SIO1's DSR stays the cassette's.** The fork also wires the unit's DTR to it; the firmware drops DTR while setting up its UARTs, and the game reported ERROR 120 (no cassette).
  - **What the unit does:**
    1. Its boot loader (`GXA25 BOOTLOADER`; `HK_MSU_CONSOLE=1` prints it) tests its RAM, configures the FPGA, checks its UARTs, initialises the four TC9446Fs, then loads and checks `/GXA25PWB.EBF` from its CD.
    2. The program talks to the 573 over SIO1 at 115200 baud: SLIP-framed packets with sequence numbers and CRCs, a keepalive every second, and the game's `GCD40JAA`.
    3. The 573 has it open `JA10.DAT`.
    - In a single-cabinet game the songs' music is the digital board's MP3, not the unit's. Its decoders (`NNNN__BK.DAT` / `___K.DAT` on its CD) are for session play with GuitarFreaks; their four streams are in the mix (at 0.65, the fork's routing), silent so far.
  - **Cost:** the unit is a second MIPS, interpreted by MAME's `mips1`, and costs more than the whole 573:
    - at first, at the fork's 25 MHz (the default), the run took ~6–10 ms a frame on the desktop;
    - at the real 132 MHz (8.25 MHz × 16, `HK_MSU_132=1`), ~31 ms. The game goes the same way at both: the same link timing, the same lamps.
    - The profile is `mips1`'s memory path (`translate`, `fetch`, `read_sized`, a `std::function` per access), around the firmware's DSP-status polling loop.
  - **Against MAME:** `drmn10m-install` is still MAME's: main RAM on every frame, and the NVRAM byte for byte. `drmn10m-boot` is MAME's until the game first touches SIO1 (frame 1688), where the link to the unit starts.
  - **Regress cases:** `drmn10m-install`, `drmn10m-boot` (now on to attract) and `drmn10m-play` (a game through the first stage). All the other cases were unchanged by the shim's umask fix.
  - **Next:** a faster TX3927 (below); session play needs a second cabinet.
- **Progress (2026-10-13): the unit, 3.7× faster.** The drmn10m run, the unit's steady state after its boot: 10.6 → 2.8 ms a frame on the desktop (p99 6.5 ms), the unit and the 573 together. The unit's boot: 7.1 → 3.0 ms.
  - **Exact** (the unit's RAM, PC and cycles every 60 frames unchanged, `HK_MSU_HASH=file`), 10.6 → 6.7 ms:
    - the shim's `space()` was a search on every access;
    - `mips1`'s fetch built a `std::function` per instruction;
    - the TX3927 maps kseg0/1 inline instead of through the virtual `translate()`, and reads memory pages directly.
  - **Timers on demand** (6.9 → 5.0 ms): the fork's three TX3927 timers fired ~1.5 M emu_timer events a second. They are now computed when read or written, with an emu_timer only for an enabled interrupt's tick. The unit's timing moves slightly, so the drmn10m cases were re-recorded after checking the same flow: link, mode and music select, the stage, the hit chart.
  - **Idle skipping** (5.0 → 3.2 ms, with the bench's digest): three quarters of the unit's instructions were its RTOS's idle loop, `j .` with a `nop`, which only an interrupt leaves. It now ends the timeslice at once. `HK_MSU_NOIDLE=1` turns it off.
  - **What's left:** the unit's share is the RTOS's decoder-control task, which polls through the scheduler (semaphores, task switches) as fast as the CPU runs. That's why the real 132 MHz still costs ~7 ms: 5× the polls for the same result.
  - drmn10m's R3000 recording now covers attract and a game, and PGO is trained on the drmn10m cases too (`pgo.py` builds with at most 8 jobs: 16 at once went past its 12 GB cap).
- **Started (2026-10-13): e-AMUSEMENT, step 1: the network PCB unit and its hard disk.** drmn10m's e-AMUSEMENT is the pop'n 9 protocol (the same RC4 secret, envelope and modules, plus GFDM's `gamedata`, shoprank, landdata and session), which the 573 game speaks itself (`httpac.c`). The unit only gives it sockets and files.
  - **The unit** (GUC07: a TX3927, an Ethernet chip and a 10 GB IDE disk, cabled to a PC card in slot 2; a stub in MAME) is high-level: `runtime/sys573/npu.{h,cpp}`, after windyfairy's HLE (github.com/987123879113/mame, branch `npu_hle`, 45e5eaa1, BSD-3). The 573 calls it through a 16-bit FIFO: start-up 0x80–0x82, sockets 0x22–0x2e, settings 0x3a, files 0x40–0x54. It needs no ROM.
  - **The disk is a folder,** the save folder's `npu_hdd/` (and a RAM disk, `npu_ram/`, emptied at power-on), since the game only asks for files. FORMAT empties it and writes `.format`, the partition figures that devctl 2 reads back. Without `.format` it doesn't mount, like an unformatted disk.
  - **Fitted with `--npu`.** `--cd hdd` boots the unit's data installer CD (`d40jca02`, game.toml's `hdd_chd`, ingested as `hdd_cd`). The game finds the disk unformatted, formats it, and installs 78 songs' data (407 MB, ~69,000 frames) to INSTALLATION COMPLETED. The next boot reads it all (DATA 3/4). Regress: `drmn10m-hdd` (the whole install, ~70,000 frames) and `drmn10m-npu` (a boot with it to attract).
  - **Beyond windyfairy's unit, from the game's driver (drmn10m's addresses):**
    - The 573 and the unit each publish their place in the two 64 KiB rings (the game writes 0x34/0x36 and reads the unit's back). windyfairy's counted words received instead, which drifts. The unit now follows the game's positions.
    - Message 0x39 is the unit's link event: its first word, "link up", is kept at 0x80139834. The network module (0x80058de8) fetches the settings (0x3a) only after it. The unit now sends it after call 0x37 (link events on); windyfairy's only answered call 0x38, which this game never makes.
    - Message 1 is a ping. The game answers each with call 0 and counts them (it waits for the first at start-up).
  - **e-AMUSEMENT ON** (test mode > NETWORK OPTIONS > e-AMUSEMENT OPTIONS, hold Start + Left/Right): the game fetches the seven settings, opens a socket (it fails: no network yet, step 2) and says センターサーバーと通信できません. Service goes on to the game in standby.
  - **Fixed (2026-10-13): with the unit fitted, the multi-session unit's link lost bytes ("Please call attendant").** The unit's polling keeps the 573 busier, so SIO1 was read late, and the fork's SIO1 held one received byte: 134 overruns from the link's start, none without the unit. The packets lost bytes and failed their CRC, and the 573 acknowledged the unit's stream short (`ff 20 30` where it had `ff 20 3e`: these are stream positions, sent and acknowledged, much like TCP's), then reconnected in a loop. SIO1 now has the PS1's 8-byte receive FIFO (its RX interrupt mode counts 1, 2, 4 or 8 bytes): no overruns, the boot goes to attract, and a session with GuitarFreaks works with the unit fitted. Nothing changes where no byte was lost (every other case identical). The link's late start (180 s here, 139.5 s without the unit) is the boot itself, which reads the disk's 78 songs.
  - **Next:** that difference; then step 2, the sockets through the Python core's VirtualNet (`runtime/python/eanet`), moved to shared code.
- **Progress (2026-10-13): GuitarFreaks 11th Mix runs, and DrumMania 10th Mix and GuitarFreaks 11th Mix play a session together.** Beyond MAME (both sets NOT_WORKING, its unit a stub).
  - **GuitarFreaks 11th Mix (`gtfrk11m`)** is MAME's set: the digital board, a ZI cassette and a 32 MiB flash card in slot 1 (`pccard1`, new). The first boot installs the runtime CD (`d39jaa02`) to the onboard flash, then to the card, until INFO 508 (~26,400 frames). After NOTE 605 (Test), attract and its demo play. About 1.8 ms a frame.
    - The `gtrf` family: per guitar, R/G/B, pick, wailing and the effect knob (MAME's dial, an analog axis), and Start. Service moves to IN3 as in MAME's `gtrfrks` ports. The lamps are the LAMPS port's (spots, starts).
    - Its network unit's installer is `d39jba02` (`--cd hdd`, as drmn10m's).
    - DrumMania 10th Mix sessions with GuitarFreaks **11th** Mix, not 10th (the 10th Mix pairs with DrumMania 9th).
  - **Session play is two processes on a session cable** (`runtime/sys573/link.{h,cpp}`). Each game is its own executable, with its own recompiled R3000 code, and windyfairy's fork also joins two MAMEs (`msu_port2` to the GuitarFreaks' `rs232_network`, null modem).
    - **The DrumMania side hosts** (`--session-host SOCKET`). Its unit's second port (`duart_com_0:chan0`) is on the cable, wired as SIO1 is to the first.
    - **GuitarFreaks joins** (`--session-join SOCKET`). Its SIO1 is a UART there, as on the DrumMania side, and the unit's DTR doesn't reach it (SIO1's DSR is the cassette's).
    - **The cable carries line levels** (TXD, DTR, RTS each way) stamped with the sender's emulated time. TXD goes once a bit, as the sending UART's bit clock calls it: the fork's SIO1 takes one bit per call, not by time (with changes only, every byte arrived as 0x55).
    - **Lockstep:** every quantum (500 µs; `--session-quantum`) each side sends what changed and waits for the other, and a change lands one quantum later, a cable 500 µs long. So a session replays the same on any host: two runs were identical on both sides, every snapshot.
    - **Transport:** a Unix socket or TCP (host:port).
    - **Cost:** the pair runs at the slower side's pace, about 4.1 ms a frame (the DrumMania side, its unit included).
  - **What happens:**
    - GuitarFreaks' SESSION NETWORK CHECK says PORT 2, OK; DrumMania's, PORT 1.
    - Coins and Start on the DrumMania side open WELCOME and its joining period (SESSION PLAY SETTINGS: 15 s), and the GuitarFreaks cabinet enters (PLAYER 1 ENTRY) with coins and Start there.
    - Both then show CAUTION, the mode and music select, and the first stage of 花火 together, SESSION on the guitar's screen.
    - The music is the unit's first MP3 decoder (peak 0.93, the first time it plays). It plays on both cabinets: the host sends its unit's mix (its decoders at 0.65) with each step, and the joining side mixes it in. That side waits for it from the start, as the host waits for its decoders, so the music lands with its own sounds of the same moment. The two recordings correlate 0.96 at 0 ms lag through the stage.
  - **Regress:** `gtfrk11m-install`, `gtfrk11m-boot`, and the pair `drmn10m-session` + `gtfrk11m-session` (a case may name a partner; the two run at once; `bench` skips them). The session sound on the GuitarFreaks side changed its digest's sound column only.
  - **Which decoder goes to which cabinet isn't known.** Each port has a sub board with its own DAC (GXA25-PWB(B)), but the fork routes all four decoders to one output, and only decoder 0 played with both cabinets in. So both cabinets get the unit's whole mix.
  - **Not yet:**
    - Hands-on play.
    - Two machines over TCP, not tried.
- **Progress (2026-10-13): e-AMUSEMENT, step 2: the network and the server.** With e-AMUSEMENT ON, drmn10m and gtfrk11m now connect: test mode's NETWORK CHECK says CONNECTED, the router, center and server answering 100%.
  - **Shared code, `runtime/eamuse/`** (from the Python core's, python-groundwork branch, which isn't on main yet):
    - `protocol`: the e-AMUSE3 pieces (MD5, RC4 key from X-Eamuse-Info, the `<call>` parser, record files);
    - `lan`: the network a cabinet sees, with no host sockets of its own. Socket calls in neutral form; DNS ("services..." is 192.168.50.1, other names get addresses of their own), ICMP echo for every raw socket, NTP (UDP 123, new), and HTTP/1.0 requests handed to a handler that answers at once or later;
    - `server`: the modules games share (numbering, KONAMI ID `userid`, posevent, pcbtracker, the owner's message), each game adding its own;
    - `remote`: online, the game's requests forwarded unchanged to a real server.
    The Python core can move onto it when its branch merges (its `eanet`/`easerver`/`eaclient` are the originals). Done for the protocol and the client (§31).
  - **The unit's sockets** (`npu.cpp`, windyfairy's formats): socket, connect, send, recv, sendto, recvfrom, setsockopt, close on the LAN. Addresses come in the game's sockaddr_in (network byte order). A recv whose answer isn't in yet (online) is answered later: the unit polls each millisecond and replies to that call's id, as its blocking recv would. Offline, the answer is there at once, so runs stay reproducible.
  - **GuitarFreaks and DrumMania's server** (`runtime/sys573/eagfdm.{h,cpp}`):
    - `services.get`: the game looks up `posevent`, `pcbtracker`, `numbering`, `userid` and `local` (everything else). It also reads two addresses (drmn10m 0x8003caa8): `keepalive` (`ping://`, `pa=`, `ga=`: the hosts the NETWORK CHECK pings) and `ntp` (`ntp://`). They are kept in its settings; ours are all 192.168.50.1.
    - At boot: `services.get`, `pcbtracker.alive`, `posevent` (income, sales, sale), then `gamedata.dataget` (sent in clear, `sid="020304050607"`) asking for the favourites (128) and the shop ranking (20). Its answer is a tree (the parser's tables at 0x80133190): `system` (status), `favorite`, `shoprank` (`shop`, `prefshop`, from/to), `ir`, `landdata` (`land`). An empty `<gamedata status="0"/>` is accepted; what they hold shows only in attract and play.
  - **Options:** `--eamuse local` (default: the server in the core, its data in the save folder's `eamuse/`) or `--eamuse http://host[:port][/path]` (online). `HK_573_NET_LOG=FILE` logs each socket call and each call and answer in clear.
  - **`hiraku-eamuse`** (`tools/eamuse/server.cpp`): the same server on the network, for several cabinets (a session's two, say), dispatching by the game's model (xx39 GuitarFreaks, xx40 DrumMania; any other to the Python core's pop'n server, §23, in the same data folder). Checked: drmn10m against it on 127.0.0.1 boots, makes the same calls, and NETWORK CHECK says CONNECTED.
  - **GuitarFreaks 11th Mix** makes the same calls (model `D39:J:A:A`) and connects the same way. Its unit's installer (`d39jba02`, `--cd hdd`) installs 366 MB to INSTALLATION COMPLETED in about 57,000 frames. In its test mode P2 R/B changes a value (DrumMania: hold Start + Left/Right).
  - **Regress:** `drmn10m-eaon` (test mode: e-AMUSEMENT ON, after `drmn10m-npu`) and `drmn10m-net` (the boot's calls, then NETWORK CHECK); `gtfrk11m-hdd` (the unit's install, after `gtfrk11m-boot`), `gtfrk11m-eaon` and `gtfrk11m-net`, the same for GuitarFreaks.
  - **Out of test mode, the game now stops at 拡張カードユニットが見つかりません** (the card unit isn't found; DEVICE 7 NOT FOUND at boot). With e-AMUSEMENT ON it requires the card reader. That is step 3, and with it the card's `gamedata` calls (gametop, cardcheck, cardregist, gameend, autoir, rend) and the KONAMI ID's.
- **Progress (2026-10-14): e-AMUSEMENT, step 3: the card unit.** A new card is registered (name, password), a game is played and saved to it, and the card comes back with its password. The server keeps each player's results and ranks them. GuitarFreaks and DrumMania boot with DEVICE 7 COMPLETE.
  - **The unit** (`runtime/sys573/cardunit.{h,cpp}`, after windyfairy's `k573acio`/`k573cardunit`): a magnetic card reader/writer and 10-key per player ("ICCA"; one on DrumMania, two on GuitarFreaks), on SIO1 at 38,400 baud. Fitted with `--npu`.
    - **SIO1 is switched** between it and the multi-session unit (or the session cable) by the cassette's d7 (the game's `swuart_get_device`: EXTIO or MSU). The fork stops the other side's clock; here it is a switch, and the side switched out sends into nothing and hears idle. It needs no MAME change: the board catches its own write of d7.
    - **High-level and byte-wise:** the 573's bits are framed back into bytes, and the answers go out a byte every ten bit times while it is switched in (SIO1 takes one bit per call).
  - **The bus, from the game's driver (drmn10m's "external_io_board", 0x8008fe44-0x80091550), where windyfairy's differs:**
    - two syncs, `aa aa aa 55` and `aa aa 00 00`, each repeated for 8 s with any matching answer counted (0x8008fcf0);
    - the node count goes round the chain: the answer is the packet with the count in it (0x80090f6c);
    - the version and start commands: the 573 hears its own packet first, then the answer `aa aa node sub len data sum` (0x80091160, 0x80091380). windyfairy's put that echo inside the answer. The version's type is big-endian;
    - node commands: the packet heard back, then `aa 01 node sub len data sum` (0x80091410);
    - the length byte is 2^(low nibble − 1), plus 1 with the high nibble set: 1, 2, 4, ... 128, 129 bytes;
    - every sum is all the bytes after the header (0x8009150c).
  - **The card** is the save folder's `card1.bin` (`card2.bin`): 128 bytes, blank (windyfairy's header) until the game writes it at the end of a game: its number, `2003010000000010`, at 0x0e.
  - **The 10-key:** each poll (sub command 26) answers the newest press as a PS/2 set 2 scan code, as pop'n's.
  - **The server's card calls** (`eagfdm.cpp`, from the answer handlers, tables at 0x80133130-0x80133270):
    - `numbering.assign` (label `cardv1`, format `card16m10`): a blank card's number, as pop'n 9's;
    - `gamedata.cardcheck <card id/>`: `<system status/>`, `<card status/>`. Status 1 is a new card (the game asks a name and a 4-digit password twice), 2 a registered one (with `pass`, `skill`, `color`, `gdid`: the game asks the password and checks it), 3 an expired one, 0 an error (0x8003f14c, 0x80039c00);
    - `gamedata.cardregist <card id irid name pass type update pazzleno recovery/>`: `<card status gdid/>`. The server keeps the cards in `eamuse/gfdm-cards`, each with its gdid (the player's number);
    - **The player's results** are kept per card (gdid) and game (D39, D40) in `eamuse/gfdm-<game>-<gdid>`: for each song (its netid) and sequence (seqmode, the difficulty), the best percentage, skill, score and combo, and the plays.
    - `gameend` sends the songs (`stagedata`: stage → netid) and per player its stages (seqmode, perc; a cleared one adds score, skill, combo). The answer (0x80040fd4, element names compared inline): `<player no skill skill_order skill_all>`, the new skill and its place among the players, with a `<stage all order best/>` per stage, its place in that song and sequence.
    - `gametop <player no gdid recovery/>`: the player's lists, 256 songs by netid (0x80042144-0x80042220, the text 0x80041bd4): `skillseqs` (a digit a song, the sequence that counts for the skill, `x` none), `musicseqs` (the same, the last played), `skillpercs seqmode=N` (two digits a song, the best percentage; `A0` is 100). `recovery` is required: the request's, sent back.
    - **The skill total** is the best 25 songs' sum, each counting its best skill (the game sends each cleared stage's own). The game has no formula for the total: this is the GuitarFreaks/DrumMania rule of the time as known, an assumption. `cardcheck` gives it to the game.
    - `dataget`: `<favorite nr netids from to/>`, the most played songs by every player here (each netid followed by `?`; the period as YYYYMMDD, both required). Attract then shows HIT CHART [全国版] with them. `shoprank` is left out: one cabinet has no shops to rank.
    - Checked through `hiraku-eamuse` with plain calls: two players, a cleared stage each on the same song, the skills, the places and the lists. In the game: a failed stage, then the card again, `gametop` with its lists accepted. A cleared stage in the game isn't tested: a scripted game plays no notes.
    - `rend` (the puzzle), `autoir`: `<system status="0"/>`, nothing kept. `userid.ctou` at the end of a game: no KONAMI ID (112).
    - The game writes some attributes as `name = "value"`; the shared parser now takes that.
  - **The boot check (DEVICE 7):** the 573 resets the bus with 64 zero bytes and waits for them to go out. SIO1 sends only while its CTS is on, and on DrumMania that was the multi-session unit's RTS, still off while the unit starts: DEVICE 7 NOT FOUND, and "the card unit isn't found" until a later retry. CTS now follows the switch: ready for the card units, the unit's (or the session cable's) RTS otherwise. Both games now say DEVICE 7 COMPLETE. Without card units the switch does nothing.
  - **A session with the card units on both cabinets** (DrumMania hosting, GuitarFreaks joining, e-AMUSEMENT ON on both): WELCOME, both entered, ENTRY CARD (Start skips it), the stage, the result. SIO1 switches between the link and the card units without upsetting the link.
  - **Inputs:** Card (Insert), the numeric keypad for the 10-key (Backspace: Clear); GuitarFreaks' second reader is "P2 Card" (Delete), its 10-key unbound.
  - **Regress:** `drmn10m-card` (a new card through registration, a game, and the save) and `drmn10m-card2` (the card again, its password, `gametop` with its lists). The cases with `--npu` changed: the card units answer the boot check, which moves the boot (`drmn10m-eaon` and `drmn10m-net` press Test 1,500 frames later).
  - **Next:** a cleared stage in the game (the skill's units and its display); dataget's shop ranking; the internet ranking (`ir`, `irdata`) and the land event (`landdata`, `land`); KONAMI ID (userid); GuitarFreaks hands-on with both readers.

- **Progress (2026-10-14): GuitarFreaks 11th Mix recompiled.** The first test on the MacBook (HD 5000, a 2013 dual-core) ran it interpreted at 8.6 ms a frame (p99 17 ms: 799 late frames), with the CRT shader at 8.9 ms of GPU. Its recording (`tools/mipsrec.py`'s way, over the install, boot, a new `gtfrk11m-play`, the unit's install and the network check: 22,657 blocks) halves the CPU here: boot and attract 1.84 → 0.91 ms a frame, a game 2.13 → 1.08 ms. Exact: `gtfrk11m-boot` matches its baseline, and `gtfrk11m-play` the interpreter's digest. A package needs the recordings in its own tree's `build/<game>/` (the package worktree had none for the 573 games).
  - **Regress:** `gtfrk11m-play` (two coins, Start through the session WELCOME alone, the mode and music select, ルージュの伝言 played with no input to GAME OVER, attract).
  - The MacBook run also had one 778 ms frame (frame 3,473, an audio gap). Here, long frames came only while other runs loaded the machine; `HK_FRAMETIMES` now tells a busy frame from a waiting one.

**573-4. Efficiency** (sized by the measurements in 573-1)
- **Measured (2026-10-12, `HK_PROFILE=1`, ddr2m 150 s from the installed flash, 21,600 samples):**

  | Where | Share |
  |---|---|
  | R3000 interpreter (`psxcpu_device::execute_run`) | ~45 % |
  | Memory accesses through the shim's address map, mostly instruction fetch (`read_sized`) | ~30 % |
  | MAME's per-instruction breakpoint and debugger hooks | ~5 % |
  | GPU drawing | ~5 % |
  | Copying the frame out of the screen bitmap | ~3 % |
  | MDEC (the intro video) | ~2–3 % |
  | SPU, CD, DMA, timers | ~3 % |

  - **The CPU dominates (~80 %), so the MIPS recompiler below is the way to Firebeat-like gains.**
  - **Done (2026-10-13): profile-guided optimisation, 1.44× for free.**
    - `tools/pgo.py` builds the 573 code instrumented, trains it on the ingested games (ddr2m-play, ddr3mj-boot), and rebuilds with the profile. The profile stays out of the repo.
    - ddr2m from the flash: 21.8 → 15.1 s for 150 s of game time (993 % real time, ~1.5× MAME's 650 %). Frames: mean 2.41 → 1.67 ms, p99 4.0 → 3.4, worst 4.8 → 4.1.
    - Every regress case is bit-identical.
    - `-O3` alone is ~4 % and is now the default. `-march=x86-64-v3` added nothing and could let FMA contraction change floating-point results.
  - **Tried and dropped: the "cheap steps" below, measured A/B.**
    - A cached instruction-fetch page: 6 % *slower*.
    - A row-wise frame copy: no change.
    - The breakpoint checks are the emulated CPU's own debug registers (CP0 DCIC); they must stay, and they exit on their first test.
    - The sampler's shares inside the interpreter loop are skewed: the switch's dispatch cost lands on the fetch line.
  - **Cheaper first steps considered** (each tried above): a cached instruction-fetch page, skipping the breakpoint hooks, a row-wise frame copy.
  - **Started (2026-10-13): the R3000 recompiler** (`runtime/sys573/mips.h`, `mips_ops.h`, `tools/mipsgen.py`, `tools/mipsrec.py`).
    - **Recording:** a run with `HK_573_RECORD=build/<game>/mips573.txt` interprets and notes every block it starts (up to a branch and its delay slot, max 64 instructions) with its instruction words. Counts add up over runs. `tools/mipsrec.py` records a game's regress cases.
    - **Generation:** `tools/mipsgen.py` turns the recording into one C++ function per block, built into the game's binary (CMake, when the recording exists).
      - Registers live in locals.
      - MAME's pipeline (`m_delayr`/`m_delayv`: the load and branch delay slots) is worked out at generation time. The one thing left to run time is a load still pending when the block starts.
      - RAM, ROM and the scratchpad are accessed through the shim's page table; anything else through MAME's handlers, at MAME's `m_icount` for that instruction.
      - An instruction a block doesn't do (an exception, a bus error, `mtc0`, `rfe`, COP1/3) goes to MAME's interpreter (`execute_one()`), and the block ends.
      - A device that cuts the timeslice short during an access ends the block after that instruction, as MAME's loop does.
    - **Running:** MAME's `execute_run()` offers each instruction outside a delay slot to the dispatcher.
      - A block runs when its words are at that address, the timeslice has room for all of it, and the CPU is in kernel mode with caches not isolated and no hardware breakpoints. Then the next block chains straight on.
      - Code is checked once: main RAM's 4 KiB host pages holding checked code are write-protected (`mprotect`). Any write to them (a store, DMA, a device) faults once and moves an epoch on, so every block is checked again. A page that keeps faulting stays unprotected, and its blocks are checked on every run.
      - Code no recording saw is interpreted.
    - **Exactness:** all 573 regress cases are bit-identical with blocks: ddr3mj-boot, ddr2m-boot, ddr2m-play (main RAM hashed on all 21,000 frames too). Blocks run 100.0 % of the instructions there.
    - **Speed, ddr2m attract from the installed flash, 9,000 frames, desktop:**

      | Build | Mean frame |
      |---|---|
      | PGO interpreter (6ea1cac) | 1.8–1.9 ms |
      | blocks | 0.65 ms (2.8×; MAME's 650 % is ~2.6 ms) |

      - Without that run in the recording, blocks covered 64 % and the frame was 1.57 ms. **Coverage depends on what was recorded.**
      - In the 150 s: 325 M block runs (7.8 instructions each), 135 code pages protected, 46 written to.
    - **Second pass (2026-10-13), the next bottlenecks.** Same ddr2m attract run (mean frame), and its demo play, frames 5,400–9,000 (closest to a song), apart. All bit-identical: every 573 regress case, main RAM hashed on every ddr2m-play frame.

      | Step | Mean frame | Demo play |
      |---|---|---|
      | after the first pass, PGO profile refreshed | 0.65 ms | ~0.67 ms |
      | the board's frame copy: rows instead of `pix()` per pixel | 0.57 ms | |
      | shim: one handler covering an access, called directly (the flash's 35 M byte reads per 150 s, the I/O registers) | 0.51 ms | |
      | MDEC: the IDCT over the nonzero coefficients only | 0.48 ms | 0.63 ms |
      | GPU: the level tables computed; textured pixels at shade 0x80 skip the shade tables | 0.48 ms | 0.61 ms |
      | **loop regions:** blocks that loop back to a block compile into one function with gotos | **0.36 ms** | **0.49 ms** |

      - **Loop regions** were the big one: the game waits for vblank by polling a RAM counter in a two-block loop (0x80020e18 ↔ 0x80020e64), which ran 450 M times in the recordings. Block runs in 150 s: 277 M → 24 M.
        - A jump to a block in the same function is a goto. Before it, the function checks that the timeslice has room for the target's first stretch and that no checked code was written (`hk_psx::epoch`).
        - The function's code is checked as a whole.
      - **Blocks run on past conditional branches that aren't taken** (superblocks). At each further stretch they check the timeslice has room. Alone this was no faster; loop regions build on it.
      - **Tools:** `tools/mipsgen.py` runs on every build and does nothing unless the recording changed, so `tools/mipsrec.py` then `ninja` is enough. `HK_FRAMETIMES=file` writes every frame's host time (and, since 2026-10-14, the emulation thread's CPU time and the major page faults in it: a long frame with little CPU waited, on the disk or for the host's CPU); `HK_PROFILE_AFTER=ms` starts the sampler later.
      - **Measured and dropped:**
        - transparent texels without a branch: no change;
        - the screen's pens computed instead of looked up: 3 % slower;
        - compiling MAME's GPU with strict aliasing: not tried, since MAME itself builds with `-fno-strict-aliasing`.
      - **Where the time goes now (demo play):** MAME's GPU ~45 % (textured polygons 27 %, the screen conversion 6 %), the recompiled code ~12 %, the vblank wait loop ~5 %.
    - **Done (2026-10-15): the GPU draws on a worker thread** (`third_party/mame/hiraku-edits.py`, `video/psx.*`).
      - MAME's command parser, the status, every register and DMA stay on the emulation thread, in step with the CPU. What writes VRAM goes into a queue instead: each primitive, `MoveImage`, and the CPU-to-VRAM words, with a copy of the state the drawing reads (texture page and window, drawing area and offset, mask bits, the packet).
      - The drawing functions are MAME's own, moved into a `raster` object that holds that copy; the shade and blend tables became static so both sides share them.
      - The worker draws the queue in order. VRAM-to-CPU transfers wait for it to empty. The screen update waits only for queued items that may write the rows and columns it shows: each item carries the rectangle it can write (the drawing area for primitives, which clip to it; its own rectangle for a fill, a copy or an upload). Games draw the next picture into the other buffer, so the screen update rarely waits. Either way, every read sees VRAM as MAME's would. A textured polygon's texture page reaches the status on the emulation thread, as in MAME.
      - MAME's `FlatRectangle16x16` tests the row before adding the drawing offset, so its rows are taken as unbounded.
      - The queue: a 4 MiB ring. The worker spins briefly when idle, then sleeps on a futex. `HK_GPU_THREAD=0` draws each item on the emulation thread as it is queued.
      - The shim now stops every device before freeing any (MAME's order): the GPU's RAM is freed before the GPU.
      - **Exact:** all 44 573 and Twinkle regress cases are identical with the worker, and `ddr2m-play` without it.
      - **Speed, desktop:**

        | Run | Without | Waiting for the whole queue (a339312) | Waiting for what it shows | CPU, all threads |
        |---|---|---|---|---|
        | ddr2m attract and demo, 9,000 frames | 3.31 s | 3.12 s | 2.88 s (−13 %) | 3.2 → 3.6 s |
        | its demo play (frames 5,400–9,000), mean frame | 0.46 ms | 0.415 ms | 0.375 ms (−19 %; median 0.39 → 0.24) | |
        | `bmiidx3-play` (with its RAM hash), mean frame | 2.63 ms | 2.34 ms | 2.30 ms (−12.5 %) | 26.2 → 27.3 s |

      - Waiting for the whole queue, the screen update waited ~70 µs in four frames out of ten: ddr2m (30 fps) queues its drawing around scanline 45, but in host time late in the frame, after the game's logic. That drawing goes to the back buffer.
      - Handing the picture over a frame later would also have removed the wait, but cost a frame of display latency.
      - ddr2m's p99 frame rose a little (1.2 → 1.35–1.45 ms): the worker draws ~25 % slower than inline (another core's caches), which shows when a frame does wait.
    - **Next:**
      - exact fast-forward of the vblank wait loop: within a timeslice nothing but the CPU touches RAM;
      - record more of each game (songs, menus);
      - measure on N100-class hardware;
      - `ddr3mj` and the digital board's MP3 path.
- **Budget**, as for M11: under 25 % of an N100 core on average, and no frame over ~8 ms of host work.
- **If the CPU dominates, statically recompile the MIPS.**
  - The code runs from RAM, loaded from flash or CD, so the recompiler works from the blocks the interpreter has executed in reference runs, keyed by a hash of their bytes.
  - The interpreter runs anything unseen or rewritten; pages holding recompiled code are watched for writes.
  - It stays cycle-exact under MAME's one-cycle-per-instruction model. A block costs its length, and a block that would cross the end of a timeslice runs in the interpreter.
  - Firebeat's shadow mode carries over.
- **If the GPU dominates:** faster span routines, still bit-identical (`tools/regress.py`).
- **The SPU and MP3 run on a worker thread**, handing over per frame as the XT446 does.
- **Exit:** every 573 regress case bit-identical, with timings measured on N100-class hardware.

**573-5. The rest of the catalogue** (ongoing, family by family)
- **In scope:**
  - DDR 3rd Mix to EXTREME;
  - the DDR Solo games: their own FPGA map and outputs;
  - GuitarFreaks 3rd–11th and DrumMania 2nd–10th, with the GF+DM session link stubbed as in MAME;
  - Dance Maniax (hand sensors), Mambo a GoGo, Martial Beat, Dancing Stage.
- **Out of scope unless asked:** karaoke (`k573kara`) and the non-BEMANI games (fishing, Bishi Bashi, GunMania).
- **Later, only if wanted:** PCMCIA edit-data cards and the memory-card reader.

**Then M15, Twinkle** (roadmap: §21). It reuses 573-1 to 573-4 for the PS1 core. It adds:
- the 68000 + RF5C400 sound board, already in HIRAKU;
- a SCSI CD and an ATA hard disk;
- the background video MAME lacks.

### Progress (2026-10-12): 573-0 and the start of 573-1

**The oracle:**
- MAME built at HIRAKU's pin, the 573 driver only: `mame-pin/k573` (beside the checkout), from `build573.sh` (memory-capped, `OSD=sdl3`).
- `oracle/mame.sh` takes `MAME_BIN=...` and caps every MAME run's memory (`MAME_MEM`, default 4G).
- `oracle/lua/hiraku.lua` knows the 573:
  - tap groups for its I/O, in all three PS1 address mirrors;
  - the PC under the PS1's register name;
  - the M48T58's power-on time in `log.txt` ("power-on time N"). HIRAKU starts at the same time with `--rtc N`.
- `oracle/lua/itrace573.lua` traces MAME's CPU with total cycles (`EXACT_PC=1`). `HK_573_ITRACE=file,from,to` writes HIRAKU's trace in the same terms.

**The core (`runtime/sys573/`, `core = "sys573"`):**
- A port of `ksys573.cpp`'s driver class for `ddr3m`: the memory map, flash bank, ports, ATAPI DMA, H8 response, JVS, the DDR stage multiplexor.
- MAME's own devices on the framework shim: the PS1 (CPU, GTE, DMA, IRQ, timers, SIO, MDEC, GPU, SPU), 29F016A flash, M48T58, ADC0834, the ATAPI stack with the CR-589, the YI cassette (X76F100 + DS2401), the PC Card slots with the 32 MiB flash card, the digital I/O board (FPGA, MAS3507D, minimp3), the JVS memory card reader with its PS1 controller ports.
- `dumps/ddr3mj/machine.txt` is written by hand for now; the CD comes from the CHD (`chdman extractcd`).

**The shim grew (third_party/mame/README.md):**
- an exact mode, a copy of MAME's `device_scheduler`:
  - the timer list in MAME's order;
  - the quantum, and MAME's cycle division (`divshift`);
  - aborted timeslices;
  - input lines through synchronize() timers;
  - local time through the device clock;
- MAME's screen timing, sound streams with their 50 Hz update, NVRAM files under MAME's names, slots, device interfaces, runtime map installs, a CD image class over `.cue`/`.iso`.
- The XT446 keeps the simple scheduler; every regress case is bit-identical.

**What lockstep took:**
- **Card detect** starts at 1 on both PC Card slots (active low), as in MAME's constructor.
- **The JVS memory card reader** is part of `ddr3m`: the game polls it at frame 187.
- **`nvram_default()` at start:** the cassette EEPROM is loaded from its region, and the flash is erased.
- **The M48T58** shows the time only after its first 1 s tick; the oracle reads it after that.

### Decisions (owner, 2026-10-12)
1. **The first set is `ddr2m`** (DDR 2ndMIX). MAME runs it, so 573-0 to 573-2 are checked against MAME.
2. **Where MAME doesn't support a game yet, go as far as the documentation allows, where that makes sense.**
   - Sources: psx-spx, the 573 hardware notes, the chips' datasheets (the MAS3507D's, the cassette chips').
   - Each departure from MAME is a switch, with MAME's behaviour kept for the oracle.
   - Write down which source each departure rests on. Where the documentation runs out, stop and record the gap.

## 20. Modding (discussed 2026-10-12; pilot: pop'n music 8)

Mods are layers over the MAME-exact game: off by default, with the regression cases run without
them. A mod set gets its own save, so the clean one is never touched.

The order of work:
1. **Unlocks and cheats.**
   - Started with pop'n 8's unlocks; cheats come later.
   - Cheats will need a writable memory path, `cheat.xml` import, and input scripts attached to a mod.
2. **Code patches.**
   - Byte patches to PowerPC code, with the patched functions run by the interpreter (it fetches
     from RAM; native code never sees the patch).
   - Native hooks on guest functions, through a per-function override check whose cost is
     measured first.
3. **Asset overlays** at the ATA/ATAPI sector level, with file maps from the discs' ISO9660;
   the hard-disk layouts are per game.
4. **Pilots on one game:** new songs; notes drawn at the display's rate from the audio clock,
   the logic staying at 60 Hz.

### The foundation (2026-10-19; docs/MODS.md)

Asked by the owner: game-agnostic ways for asset packs (languages, songs) and code changes (high
frame rate), pop'n music 14 as the pilot. The foundation is in:
- **The format:** a folder per mod with its `mod.toml` (name, version, game; `[[patch]]`,
  `[[bytes]]`, `[[file]]`), in `<saves>/mods/<game>/`, `--mod FOLDER` (several, in order).
- **Memory** (`runtime/host/mods.cpp`): each patch put in place every frame its expected bytes
  are there (so it waits for code loaded later), or written every frame like a cheat;
  `Core::code_changed` lets a core translate again (Python: the EE recompiler's blocks cleared;
  573/Twinkle: their write-protected code pages already catch it; Firebeat: the functions a patch
  changes run interpreted from their next call on, `runtime/core/patched.cpp`, 2026-10-20).
- **Media** (`runtime/shell/overlay.{h,cpp}`): byte ranges of an image replaced as it's read, in
  the image layer (every HIRAKU core) and through a new `Hiraku::disk_read` hook in the Reliquary
  patch (Python 1's disk and CF card); `[[file]]` finds a file on an ISO 9660 disc (cooked or raw
  sectors) and lays it over its sectors.
- **The launcher:** Mods… on a game's right-click menu (on/off, order, the folder), the mods
  passed to the game, and a save folder per mod set, `<game>+<mod>...`, started from a copy of
  the clean game's.
- **Cheats:** `tools/modimport.py` (`.pnach`, MAME `cheat.xml` plain writes).
- Checked on pop'n music 14 (a test-menu text patch, an identity disk overlay with an unchanged
  digest), DDR 2ndMIX (CD bytes and `PSX.EXE` as `[[file]]`, unchanged), and the launcher.

**Pilot step 1, pop'n music 14's file system (done, 2026-10-19):** worked out from the loader
(boot record C, a Ghidra project of its own) and the game's program; told in full in
`tools/popnfs.py`, which lists and extracts the files.
- **The directory:** 0x1f8 sectors at sector 0x4800, 24-byte entries: name hash, first sector,
  size, 0, byte sum, 0. It ends at an empty entry holding the sum of the bytes before it. The
  loader searches it linearly. Sector 0x4a00 holds a second one, for `disk/write/` (the saves).
- **The names:** a CRC (0x04c11db7) over each character's low 6 bits. They come from the game's
  strings (`/bg/bg_%03d.gzz`, `disk0:image/%s/system.idx`, ...) and the folders' `system.idx`
  lists: 10,010 of 10,165 files named.
- **Encryption:** a 4-byte tag (13G0..13A9, 13P0 for the program), the plain length, then
  Blowfish-CBC. Each tag picks a key and IV from a table in the loader (IV words stored
  little-endian). Files without a known tag aren't decrypted, so a mod's file needs no key.
- **Packing:** the pictures (`.gzz`, `GC` containers) are a u32 size plus Okumura LZSS (4 KB
  window from 0xfee), at most 0x80018 bytes unpacked. The 719 largest stop a few bytes short;
  the game decodes on into what follows. The index files are stored as they are; the program is
  only encrypted.
- **In HIRAKU** (`runtime/host/pythondisk.{h,cpp}`): `[[file]]` on the disk. The mod's file is
  packed the game's way, unencrypted, in place or in the unused room after the last file (55 MB
  of zeros up to 0x25000000). The directory is laid over with its sums made again, and new
  names are added. The Python core got a libchdr reader for mods (`overlay::set_reader`).
- **Pictures:** `GC` containers, either 16-bit (A1B5G5R5) or 8-bit. The 92 8-bit folders (the
  `256_*` menus among them) share one 256-colour palette: the last 1 KB of the folder's
  `system.idx`, in the GS's CSM1 entry order. `tools/popnfs.py topng` / `frompng` convert both
  ways, byte-identical for an unchanged picture; frompng takes the palette's nearest colours.
- **Checked:** a picture changed on screen. The same picture repacked runs the play case
  through; the title loop's timing moves with the file's size and place, then joins up again.
  The full regress identical with all this in (151 cases, 2026-10-19).
- **Finding the songs:** `HK_PY_DISK_LOG=FILE` logs every read of the board's disk ("frame
  medium offset size"), to see where a song's data is read from.

**Pilot step 2, the songs on the disk (worked out 2026-10-19; `tools/popnsong.py`):** the disk log
of the play case showed per-song reads past the file system; the program (FUN_017c0438,
FUN_017c04a8, FUN_017c1be0 "pnote header error") tells the rest.
- **Slots:** song slot n is 20 MB from sector 0x100000 + n * 0xa000. 753 slots carry a header,
  749 of them songs (4 placeholders whose charts are all one 2 KB stand-in).
- **Scrambling:** XOR with bits 5-12 of an LCG seeded by the slot (state = slot + 13; state =
  state * 0x5d588b65 + 13 per byte), restarted at each read; those bits repeat every 8 KB and
  the parts start on 8 KB boundaries, so one 8 KB pattern per slot.
- **Header:** "LB13", the slot, 12 parts (sector, size, tag): `ele` keysounds (SPU ADPCM),
  `pwm` their table (44,100 Hz), seven charts (`np` `hp` `ep` `op` `bp` `xep` `xnp`; a sheet a
  song lacks repeats `np`), `bt` the music (streamed while playing), `pre` the preview.
- **Charts:** a u32 size and LZSS (as the pictures), then 8-byte events: time in ms, a command
  and its data (notes, keysound changes, tempo, measure lines).
- **Next:** which slot is which song (the song list in the program), the chart commands, the
  ADPCM layout of `ele`/`bt`/`pre`; then a slot made from a mod's files (`[[song]]`, laid
  scrambled into a free slot the way `[[file]]` lays files) and the song list's entry.
- **Not in these files:** the songs. They are in slots after the file system (step 2 below).

**Pilot: high frame rate (2026-10-19, `[[rate]]`, docs/MODS.md):** pop'n music 14 computes the
song's position from a hardware timer each frame (FUN_01740298 → 0x018ff668) and draws and
judges notes from it. PCSX2's NTSC frame rate sets the vsync's interval in machine time, so
`Core::set_vsync_rate` (the Python core: `EmuConfig.GS.FramerateNTSC` + `UpdateVSyncRate`, at
the next vsync) gives the game more frames a second while the clock moves; `hz = "display"`
follows the display (`pacing::refresh_hz`), any number to 1000 otherwise. `between_ticks`
patches keep frame counters at the cabinet's pace at any rate: pop'n 14's animated objects read
their speed as 0 between ticks. `games/popn<N>/mods/smooth-notes` for pop'n 9-14 (the same engine: table in docs/MODS.md). Checked at 120, 240 and 480 Hz
(a picture every vsync; notes where they are at 60 Hz; same results; characters at the
cabinet's pace). Left: the judgement text's timer (counted through a pointer in a stack frame);
the window at a real high-refresh display; other cores' `set_vsync_rate`.

**The pilot:** a translation (the pictures' `GC` format to and from PNG; the text in
`game.bin` as `[[patch]]`es) and smooth note scrolling are done; a song added is its own
milestone now (M17). Python 2's disk is later. Firebeat code patches are done
(2026-10-20): a recompiled function's entry poll, the executor's calls and the dispatch send a
changed function to the interpreter (docs/MODS.md, Per core).

### Smooth motion: the scroll guess (2026-10-20)

The owner saw a lot of judder in the notes with smooth motion (pop'n music 8, beatmania IIDX 7th style, 240 Hz).
The in-between pictures carried each object on by its last move, but notes scroll in whole pixels (pop'n 8: 2, 3,
3...), so after a 3 they went 3 on, the next frame moved them 2, and they stepped back: one refresh in twenty at
240 Hz, 0.57 px off a steady line. Now each core (Firebeat GCU, DJ Main, the PlayStation GPU) takes the move most
of a frame's moving objects share as the scroll and carries what moves with it along the steady line through its
last six positions (include/hiraku/scroll_guess.h; the frame's first refresh drawn too): 0.29 px, the pixels' own
rounding, no step back (tests/smooth; pop'n 8's How to Play logged: falling notes on the line, rising arrows on
their own). IIDX at normal speed moves a line a frame of 240: nothing between to show; the step now falls at the
same point each frame. Regress identical (the guess runs only for in-between pictures). `--snap-from`,
`--snap-between N` write in-between pictures headless (Firebeat's belong to the window's picture, so compare
them among themselves). Second test: still a constant stutter, and glitches under falling notes. The logs
showed the pictures reaching the screen in order (the frames' other refreshes all shown, none replaced), so
the pictures themselves: in pop'n 8's play the notes squash and stretch to the beat (their horizontal scale
51, 52, 53/64 from frame to frame), and the GCU matched objects only of the same size, so almost no note matched:
they stood still between frames (the stutter) while other parts moved (the glitches, likely). Now the same
picture and height match with another width or scale; and the scroll is the largest group of moves within a
pixel (falling notes stepping 2 and 3 outnumber a rising lyric), its speed their average. Third test: no
difference. Traced in the window (a 60 Hz nested KWin, smooth motion's first refresh of each frame
captured): pop'n 8 draws its notes into a VRAM strip that a later object copies onto the lanes, the strip a few
pixels sideways frame to frame, and the notes squash about their middle. So objects are now placed through the
copy that shows them and by their middle, and the copy's screen rows (at the buffer's origin) are drawn again;
the frame on the screen is chosen by the buffer its own fb_config sets. Result in the window: a falling note
0.60 window px off a steady line in time (rounding), the plain picture 0.67 off a line in whole frames.
`HK_PICTURE_SNAP` takes a range. Left: the owner's eyes again; IIDX 7th (Twinkle) not traced this way yet.
Fourth test: the glitch gone, the scrolling still not smooth, at hi-speed (x4, about 700). Played that way in the
window (a script: CHALLENGE, FRESH 176 BPM, both yellows and blue three times at the option select; every frame's
quarter pictures saved): the one note on screen falls 8.4 px a frame while three objects move 2, and those became
the scroll, so the note went by its last step, which jitters (8, 10, 8, 8, 10, 8, 7); 1.00 px off its line, steps
of 0 or 4 at each frame's start. Now each object keeps its own line through the frames (by the pairing; least
squares, older positions counting less) and goes along it when not with the scroll; the pairing votes the scroll
first and looks 64 px away (it took the nearest within 16); pop'n 8's notes pair whatever their width (a squash a
pixel narrower with a new picture left them unpaired for a frame); the copy's rows are coupled until nothing
changes (at buffer row 0 the strip's rows are screen rows too: notes split). Result: 0.35 px off, none back
(whole pixels alone: 0.29). IIDX's and DJ Main's pairing changed the same way, not traced at hi-speed.
Fifth test: pop'n 8 perfect; IIDX 7th: ghost notes in the lanes beside real ones, and lag spikes between scenes.
Traced with the bmiidx7a-play case in the window: the lag was the pairing (68000 candidates on a screen of identical
tiles, a std::map of votes: 19 ms a picture, at 240 Hz four or five a frame); now once a frame on the PlayStation
GPU, candidates by command, colour and 64-pixel cell, votes in a grid of moves, objects with an exact look-alike at
their place not voting: at most 4.3 ms. The ghosts, most likely (seen in the play case, not the owner's play): identical sprites a lane apart paired, by a
sideways "scroll" a flickering row voted for (now standing still is voted too) and by IIDX's key flash moving
lane to lane (now a pairing far off the look-alike's own steady line is a jump: not carried). tests/smooth has
both; pop'n 8 at x4 unchanged (0.33 px).

### Pilot: pop'n music 8 unlocks (done)

- **What the phase is:** the event phase is one byte of the RTC NVRAM, kept three times
  (`rtc.nv` 0xbf, 0x57b, 0xa37), with no checksum.
- **How it was found:** the four codes from remywiki were typed in EVENT OPTIONS > PASSWORD
  by `oracle/scripts/popn8-phases.his`, each from the save the previous one left, and the saves
  were compared. Each code changed only those bytes, 0 → 4.
- **Checked in play:** at MAX, the music select gets a "secret" category with the title-screen
  songs.
- **The tool:** `games/<id>/unlocks.toml` describes a game's unlocks, and `tools/unlock.py`
  reads and writes them in the save (with a backup). The launcher shows them under right-click >
  Unlocks.
- **Checked:** setting MAX with the tool gives a save byte-identical to the game's own.

### Sugoroku de 8, the event itself (step 1: the clock, 2026-10-12)

- **The event runs in HIRAKU.** Phase 1 turns it on, and attract mode shows the
  すごろく DE 8 絶賛実施中! board, with KRAFT on Tokyo, Shizuoka next, and a hidden-song rate of 0%.
- **The game never gives the event the date.** pop'n 8 reads the clock chip's time only through
  `0x8009ccdc`/`0x8009cd34`, and the callers of the readers above them are, exhaustively:
  - the test-mode clock setting (`0x8007f158`) and operator screens (`0x80083310`/`0x80083728`);
  - the boot check, with its battery and update-in-progress tests (`0x80071b3c`, `0x800715a4`);
  - the coin bookkeeping (`0x80079ec0`: a weekday via `0x8008a358`, and a 365-day history
    shifted at boot by `0x8007a014`);
  - error-log timestamps (`0x8006d1dc`, `0x800713c4`).

  The bookkeeping block (`*0x80100e64`, 0x10b4 bytes, saved to flash) is reached only through
  its accessors at `0x80079a70`–`0x8007a54c`. Outside bookkeeping and test mode they are called
  only for coins, play time and play counts, and none returns a date. No function pointer
  leads to any of them.
- **Booting phase 1 on five dates** (10 July, 26 July, 5 August, 30 September 2002, and
  12 October 2026) gives an identical attract mode, and none of the five boots writes the save.
- **Consequence:** HIRAKU's fixed clock doesn't hold the event back, and no event calendar is
  needed. RemyWiki's "each week" can't be a date the game checks. The rule will show in the
  event's own code and state.
- **`HK_RTC="YYYY-MM-DD[ HH:MM:SS]"`** sets the Firebeat clock for experiments; the default
  stays 2002-07-26 12:00.

### Sugoroku de 8, step 2: a round played to the board (done, 2026-10-12)

- **A round reaches the board.** Before each round, `FUN_80024c84` draws the requirement into
  `0x8052d7b0`: one of 13 for the mode, from the table at `0x80024ea0`. A round that meets it
  ends on the board: the roulette, then KRAFT moving (`oracle/scripts/popn8-sugoroku.his`).
- **The autoplay that met the requirement:**
  - Stage 1 is played from the chart in RAM: 8-byte records `{u32 time, type, 0x45, lane,
    sample}`, with type 1 a note in lane 0–8.
  - The times are in milliseconds, so a note is due at song frame t × 58.4866 / 1000: pop'n runs
    the GCU at its 16.6666 MHz pixel clock, 58.4866 frames/s (measured, steady after boot, the
    same in pop'n 4, 5, 7 and Animelo 2), not 60 or 59.94. 58.5 was used and is close enough
    (under 2 frames in two minutes); 60 drifted 2.6 %. The song's frame count is at `0x8021825c`.
  - Stage results are at `0x80218310`: combo, -, GREAT, GOOD, BAD.
  - The result was 176 GREAT, 2 GOOD, 0 BAD.
- **The event block** follows the phase in the RTC NVRAM, kept three times (+0, +0x4bc, +0x978).
  Each field is big-endian u32:

  | Offset | Field |
  |---|---|
  | `0xbc` | phase |
  | `0xc0` | stage, 0–2 |
  | `0xc4` | unlocked mask |
  | `0xc8` | missed mask |
  | `0xcc` | position, 0–1003 |
  | `0xd0` | lap |

  - `FUN_800bab40` loads the block (`FUN_8006fe8c`), moves KRAFT one space at a time, and saves
    it (`FUN_8006fee4`).
  - Two credits moved the position 0 → 1 → 6, and it survives a restart: the attract board's
    progress bar moved.
- **The rules** are in `FUN_800b8e64`:
  - **The board** is 1004 spaces (`0x800bcad0`, 12 bytes each: coordinates, flags, city). The
    city records (`0x800bb5d0`, 0x34 bytes) hold each stage's unlock bit.
  - **First time around a stage**, a city needs an exact landing; a city passed goes in the
    missed mask and is given on the next pass.
  - **Stage goals:** `0x3ffff` (18 cities), `0x7f8` (8 cities), `0x1fffff` (18 cities plus the
    three island bits). Completing the last gives `0xbb001e`, the HELL the 8 course.
  - **RemyWiki's "week"** is this stage, not a date.
- **The event runs at phases 1 and 2**; phase 3 replaces the board in attract mode.
- **In HIRAKU:** `games/popn8/unlocks.toml` `[[progress]]` describes the block, the board's
  cities, and the stage goals. `tools/unlock.py` decodes it, and the Unlocks dialog shows the
  stage, unlocks, KRAFT's space and lap, the next city, and the cities missed.
- **Not yet seen:** a city actually unlocked. 静岡 is 21 spaces on, an exact landing.

### Pilot 2: pop'n music 7 (done, 2026-10-12)

- **The operator codes** (RemyWiki) add switches to EVENT OPTIONS: BINGO de 7, SECRET DATA
  RELEASE, BINGO de 7 DATA RELEASE, EXPERT MODE DATA RELEASE. Each is NO until the operator
  sets it (hold red, press blue) and saves.
- **In the save:** one 32-bit word, kept three times (`rtc.nv` 0x84/0x88/0x8c).
  - The high byte holds two bits per switch: code entered, and on.
  - The low 24 bits are the bingo squares.
  - A 16-bit byte sum (0x5c-0xec0, at 0x56) covers it.
  - The boot check (`FUN_8005b940`) repairs the three copies by majority, and drops a switch
    whose code is missing.
- **The tool:** `tools/unlock.py` gained checksums; its MAX save is byte-identical to the
  game's own (`oracle/scripts/popn7-phases.his`).
- **BINGO de 7!!:**
  - **Opening squares:** after each stage, `FUN_800c4824` tests every closed square's condition
    (`FUN_800c2008`; the conditions' names are at `0x800c1134`, 16 bytes per bingo number) and
    opens those met.
  - **The card:** 24 numbers loaded from the DVD, plus the free centre. Squares map to bits
    0-23 column by column (the table at `0x800c18a0`). Lines A-L: the diagonals A and L, the
    columns B-F, the rows G-K. The numbers are shown doubled on screen.
  - **The hint** (`0x80352ae0`) is a random closed square each credit, not saved.
- **Verified:**
  - An autoplayed stage, ANTHEM / GLORIA at 154 GREAT (same chart format and timing as
    pop'n 8), opened square 26, `SCORE 100000 DEFSPEED` (`oracle/scripts/popn7-bingo.his`).
  - The event never reads the date: the clock readers are bookkeeping, error log, test and
    boot, and the event screen is identical on four dates, 2002 to 2026.
- **In the Unlocks dialog:** the level, the open squares, the lines done or one square away,
  and every closed square's condition.

### pop'n music 6 (done, 2026-10-12)

- **The codes:** `38281838` on test mode's main menu adds OPERATOR COMMAND OPTIONS. There, the
  last 12 presses of buttons 1-4 and 6-9 are compared with five codes in the program
  (`FUN_80071810`, table at 0x800721f8: 12 button masks per code, newest first). Each match
  adds an item, NO until switched on and saved on EXIT.
- **In the save:** one byte per flag in the RTC block (0x5c on).
  - `0x8c` is the menu.
  - Code bytes: `0x8d` SECRET MUSIC, `0x8e` ALL DATA, `0x8f` STEP UP, `0x90` SECRET COURSE,
    `0x91` STEP UP 2.
  - Switch bytes: `0x92`-`0x95`, in that order, without STEP UP 2.
  - The 16-bit byte sum covers 0x5c-0xec8, at 0x56.
- **The tool:** `games/popn6/unlocks.toml` kind = "switches"; `tools/unlock.py` and the
  Unlocks dialog (checkboxes) set them. Its everything-on save is byte-identical to the game's
  (`oracle/scripts/popn6-operator.his`).
- **Checked in play:** with SECRET MUSIC on, the music select has the SECRET songs.
- **Not checked:** the other switches' effects, which are RemyWiki's.

### beatmania III THE FINAL, Keyboardmania 3rd and 2nd (done, 2026-10-12)

Each unlock is one byte of the RTC NVRAM, kept three times, with no checksum. Each was done in
HIRAKU, the saves were compared, and `tools/unlock.py`'s save is byte-identical to the game's.

| Game | Offsets | How | Script |
|---|---|---|---|
| bm3final | `0x52f`/`0x9ff`/`0xecf` | COLOR CHECK code, 2P START held, keys 1-5 1P and 6-10 2P; written when test mode is left. Test mode is entered by holding Test through the boot check. | `bm3final-unlock.his` |
| kbm3rd | `0xf8`/`0x599`/`0xa3a` | 2P START + 1P F#2/G#2/A#2 pressed during the startup test | `kbm3rd-unlock.his` |
| kbm2nd | `0x95`/`0x4ab`/`0x8c1` | 2P side, wheel held down on SELECT PLAY MODE | `kbm2nd-pressure.his` |

- **kbm3rd:** the keyboards report changes, so the keys must go down during the test.
- **kbm2nd** (`FUN_80013600`): PRESSURE appears after more than 49 clicks past FREE, 3 once
  unlocked. A click is 48 of the wheel's offset from centre / 32 per frame: 16 frames at full
  deflection, so 50 clicks only just fit in the 15 s timer. The flag is saved when the selection
  ends.

### Code macros: per-credit and power-on codes (2026-10-12)

Codes that last a credit or until power-off can't be saved, so HIRAKU types them:
`games/<id>/macros.txt`, run by `runtime/host/macro.{h,cpp}` from the frame hook (before the
input script), with inputs through `core().set_input` like the scripts'.
- **Triggers:** Ctrl+1..9 in the window; `--macro NAME[@FRAME]` (several queue: one due while
  another types starts after it). The launcher's Unlocks dialog lists them, and a macro with an
  `at` frame gets an "every start" checkbox (`macros/<id>` in the settings → `--macro`).
- **Steps:** `keys` (digits), `press`, `hold`/`release`, `set` (analog), `wait`, `tap N gap M`.
- **Timing:** taps of 8 frames, 16 apart, by default. With 4/12, pop'n 4 lost a press (it never
  reached the ring) in the sixth code of a row; with 4/8, pop'n 5 lost one.
- **pop'n 5** (`FUN_8005b408`, called from the title screen `FUN_800385a0`):
  - 16 presses in a ring (`0x80264dc8`), compared newest first against the table at
    `0x8005b5e0` (0x28 bytes each: 16 u16 masks, length, name); 60 idle frames clear it.
  - A match sets `0x802649c8 + 4·index` until the title is set up again (`FUN_8005b2f0`),
    and `FUN_8003dbe0` makes the songs available: song 0 for LESSON, 0x3d for [SP]ENDING,
    5/0x1a for the remixes, 0x1d/0x1c for the characters.
  - Eight codes, one more than RemyWiki lists: `2179223718` [SP]ENDING.
  - All eight were typed back to back by the macros and set their flags; LESSON and
    スペシャルエンディング were seen in the music select.
  - With credits in, the title still cycles through the attract logos (about 1,600 frames after
    the coins); the new title clears the flags. All eight at 8/16 don't fit in one pass.
- **pop'n 4** (`FUN_80052e58`, called from `FUN_80034ae0`): the same checker, with a 10-press
  ring (`0x80268410`) and 0x1c-byte entries at `0x80053030` (10 masks, length, name); flags at
  `0x80268010`. Eight codes: BRITPOP `33997733`, HORROR `333111333`, GROOVEROCK `6861379`,
  [RX]POPS `44322366`, [RX]J-TEKNO `864291`, [RX]DANCE `68686824`, [RX]BNSTRK `444322211`,
  [SP]ENDING `2179223718`. All eight set their flags typed back to back (frames 5100–6142).
- **Animelo 2** (`FUN_800369a0`): the last 16 presses (`0x80298858`, newest first) against three
  tables at `0x80037a00`; the attract screens show the codes. DOUBLE EXCITE `19283746` sets
  `0x80102374`, TRIPLE `1919282837374646` `0x80102370` (both checked); `FUN_8002862c` reads them
  in modes 2 and 6. NON STOP `1496149614961496` calls a region check (`FUN_8002ebd8`) and drops
  the result: in JAC nothing sets its flag `0x80102384`.

### Display pacing: smooth frames on fixed-refresh displays (2026-10-12)

The games run at 59.94 (beatmania III), 58.4866 (pop'n) and 58.51 Hz (Keyboardmania). Shown as
they come, a fixed-refresh display judders. `runtime/host/pacing.{h,cpp}` runs the machine on the
display's refresh grid when the display is within 3 % of a whole multiple (the game 2.6 % fast at
60/120/240 Hz, the sound resampled with `SDL_SetAudioStreamFrequencyRatio`, steered to keep the
queue at its target). The emulated machine is unchanged.
- **Per frame:** burst (the machine), draw, wait until the commit time (a lead before a grid
  vblank), present, wait until the next burst (a budget, p99 of burst + draw + 1 ms, before the
  next commit). The audio queue is topped up at once when it's under half its target.
- **Wayland presentation-time** gives the vblank times, the refresh period and each frame's
  commit-to-screen time. KWin 6.7 at 240 Hz: about 8 ms at best, and the deadline wanders by up
  to a refresh with its render-time estimate. Frames committed near it came out irregular, and
  0.5-1 % were irregular at any phase.
- **fifo-v1** (each frame's commit plus k-1 empty commits, each waiting for the previous barrier)
  stops early display. Irregular frames, 2.5 min runs: 144 Hz at 2 refreshes per frame 838 without
  fifo, 3 with (0.03 %); 240 Hz at 4, about 2,500 (the first designs) against 15 (0.17 %).
- **Finding the deadline:** the commit moves later, a 32nd of a refresh every 20 frames, until the
  median of 10 commit-to-screen times is a refresh slower than the best (a miss: with fifo the
  queue moved). Then the grid catches up a refresh and the commit goes a 32nd plus 1.5 ms earlier;
  later misses add 0.75 ms. Learning a fixed "pipeline" offset, or reacting to single irregular
  frames, failed (both chase the compositor's own jank).
- **Without Wayland** (X11): the grid runs on the display mode's rate with an unknown phase.
- Tested with the window on DP-1 (240 Hz) and DP-2 (144 Hz, with `HK_DISPLAY_SYNC_SKEW` to force a
  fit); 60 Hz still to test on a real 60 Hz display (the N100 cabinet).
- **Smooth motion (2026-10-19, `--smooth-motion`, docs/SMOOTH.md):** the k - 1 empty commits can
  carry in-between pictures instead (the core's `render_between`): DJ Main's sprites and the
  Firebeat GCU's objects carried on by their last move, for the games that count their play in
  frames (pop'n music 1-8 and the rest of those two cores). Checked offline on pop'n 1, 4 and 8
  (halfway pictures: only the notes, half a step on). Then at the game's own speed (the owner's
  pop'n 3 at 240 Hz didn't fit: 58.00 Hz needs 3.4 %): each frame gets the refreshes from its
  moment on the steady clock to the next's (any number, any display, 144 Hz too) and each refresh
  the frame carried on to its own moment (`frame_exact`, `phase`). Checked in a virtual KWin at
  60 Hz. Left: DJ Main's tilemap scroll; the owner's 144/240 Hz displays.
  The beatmania games (2026-10-19): DJ Main's and beatmania III's notes move as sprites/objects
  (checked on 1st mix and beatmania III's practice stage); beatmania IIDX (Twinkle) through the
  PlayStation GPU's queue (`hk_between`, hiraku-edits.py): the shown frame's primitives moved on
  and drawn again into VRAM, composed, the rows put back; about 0.6 ms a picture.
  System 573 (2026-10-20): its `render_between`, and the GPU's frames now go from flip to flip
  (DDR 2ndMIX runs at 30 frames a second), uploads since the frame on show undone while it's
  drawn again (DDR's arrows flash by palette), and only what moves whole and is a sprite,
  rectangle or level line carried on (DDR's 3D dancer came apart). Checked on DDR 2ndMIX's and
  GuitarFreaks 2ndMIX's play cases.

## 21. Twinkle roadmap (M15, written 2026-10-13)

The approach is the 573's (§19): MAME's devices on the shim, an interpreted CPU, cycle-exact lockstep with MAME first. Then the one thing MAME doesn't do at all, the background video, as a documented departure. Then the N100 budget.

### Media on hand

| Set | Style | MAME config | Program CD | Video CD | HDD | Dongle | MAME status |
|---|---|---|---|---|---|---|---|
| `bmiidx3` | 3rd style (GC992 JAC) | `twinklei` | `gc992-jac01` | `gc992-jaa04` | `992hdda01` | `992a02`, M24C02 EEPROM (plain) | **works** (`IMPERFECT_SOUND`); first boot stops at E150, 1P Start clears it (§16) |
| `bmiidx` | 1st style (863 JAB), downloaded 2026-10-13 | `twinklex` | `gq863-jab01` | `gq863a04` (4 tracks) | `863hdda01` (3.2 GB) | `863a02`, X76F041, `BAD_DUMP` (write and config passwords unrecoverable; the read side is complete) | **`NOT_WORKING`**: the DVD player check needs RTS→CTS on the PS1's SIO1, which MAME's SIO lacks |
| `bmiidx2` | 2nd style (GC985 JAA), downloaded 2026-10-14 | `twinklei` | `gc985a01` | `gc985a04` | `985hdda01` | `985a02`, M24C02 | works |
| `bmiidxs` | Substream (GC983 JAA) | `twinklex2` | `gc983a01` | `gc983a04` | `983hdda01` | `983a02`, X76F041, `BAD_DUMP` | works |
| `bmiidx4` | 4th style (GCA03 JAA) | `twinklei` | `a03jaa01` | `a03jaa02` | `a03jaa03` | `a03`, M24C02 | works |
| `bmiidx5` | 5th style (GCA17 JAA) | `twinklei` | `a17jaa01` | `a17jaa02` | `a17jaa03` | `a17`, M24C02 | works |
| `bmiidx6` | 6th style (GCB4U JAB) | `twinklei` | `b4ujab01` | DVD `b4ujaa02` | `b4ujaa03` | `b4u`, M24C02 | works (MAME: no picture, DVD chapters read as a Video CD's) |
| `bmiidx7a` | 7th style (GCB44 JAA) | `twinklei` | `b44jaa01` | DVD `b44jaa02` | `b44jaa03` | `b44`, M24C02 | works (as 6th) |
| `bmiidx8` | 8th style (GCC44 JAA) | `twinklei` | `c44jaa01` | DVD `c44jaa02` | `c44jaa03` | `c44`, M24C02 | works (as 6th) |

- Both carry the BIOS `863a03.7b` (512 KiB) and the SPU program `863a05.2x` (512 KiB).
- `bmiidx3` is in the rompath and verified (§16). `bmiidx` is only in `~/Downloads` so far: hard-link it into `~/bm3mame/roms-cores/bmiidx/` and run `-verifyroms` first. Its three CHDs have non-zero data SHA-1s.
- **Disk:** 63 GB free on DATA (2026-10-13). The raw HDD images are ~1.5 and ~3.2 GB before `tools/chd.py` punches holes.

### What MAME has

Sources: MAME master `twinkle.cpp` (1,700 lines, 18 sets plus the BIOS), `bus/rs232/xvd701.cpp`, and the devices it uses. All BSD-3.

**The main board (TWINKLE/MAIN):**
- The same PS1 chipset as the 573: CXD8530CQ at 33.8688 MHz with 4 MiB of RAM, CXD8561Q GPU with 2 MiB of VRAM, the SPU with 512 KiB.
- **SCSI, not ATAPI,** for the program CD: an NCR 53CF96 with a Toshiba XM-5401 drive, on the PS1's DMA channel 5.
- An RTC65271 (as on Firebeat), an LTC1232 watchdog (1.2 s), and an SMC FDC37C665GT super I/O whose second 16550 talks to the DVD player from Substream on.
- The security dongle, bit-banged through 0x1f270000/0x1f280000: an X76F041 (1st style, Substream, Club Version 2) or an M24C02 (2nd style and later).
- The I/O mux at 0x1f220000 (an index register, then data), the key LEDs, and the Bt812 video decoder's registers at 0x1f2b0000.

**The SPU board (TWINKLE/SPU) is Firebeat's SPU board.** MAME's two memory maps are the same address for address:
- a 68000 at 16 MHz running `863a05`, RF5C400, 24 MiB of wave RAM in three 8 MiB banks, a CY7C131 mailbox to the main CPU;
- an ATA hard disk whose DMA fills the wave RAM.
- The differences are small: IRQ acknowledges through a control register (bits 7–10), IRQ1 at 60 Hz, and the DMA loop.

**The sub board (TWINKLE/SUB2)** decodes the external player's video (Bt812), buffers it in field memories and overlays the PS1 picture on it. **MAME ignores it:** the register writes go nowhere and nothing is mixed.

**The DVD player:** a JVC XV-D701 on a serial line, modelled by MAME's `xvd701` as far as the game can see (JLIP protocol at 9600 baud, 8O1: power, play, pause, stop, seek chapter, next/previous, status, ID; replies 100 ms later). It produces no picture. 1st style uses the PS1's SIO1 over the mini-DIN; Substream and later use the super I/O's UART on the DSUB.

**Controls and lamps:** 7 keys and a turntable per player (the turntable is an 8-bit position counter), Start ×2, VEFX, Effect, Test, Service, Coin, 5 effector sliders (4 bits each). Lamps: 1P/2P Start, VEFX, Effect, credit, 8 spotlights, neon, 14 key LEDs, a 9-character 16-segment ticker, board LEDs.

**Speed:** `bmiidx3` at 567 % in MAME (§16), about 18 % of a desktop core.

### What HIRAKU already has, and what is new

**Reused:**
- the 573 core's PS1 on the shim, its exact scheduler, the recompiler (573-4) and its tools;
- MAME's `x76f041`, `ins8250` (with the TX FIFO fix), `diserial`, `sio_uart`, `cdda`, `ataintf`/`atadev` on the shim;
- the Firebeat SPU's board logic (`runtime/spu`) and the RF5C400 port;
- the oracle (`oracle/mame.sh`, `hiraku.lua`, snapcmp, audiosync), regress and PGO;
- the analog turntable input and the lamps path (docs/LIGHTS.md).

**To port:**

| Part | Lines | Note |
|---|---|---|
| `twinkle.cpp` driver class | ~1,700 | memory map, I/O mux, SPU board glue, SCSI DMA |
| SCSI: `ncr53c90`, `nscsi_bus`, `nscsi_hle`, `nscsi/cd` | ~4,300 | new bus type for the shim |
| `fdc37c665gt` | ~530 | needs the 16550s; the floppy controller and parallel port can be stubs if the games never touch them (check with a tap) |
| `i2cmem` (M24C02) | ~620 | 2nd style on |
| `xvd701` + `rs232` port | ~500 | |
| `rtc65271` | ~720 | HIRAKU's Firebeat copy runs on its own clock; the shim needs MAME's |
| `watchdog`, `cy7c131` | ~300 | |
| **the 68000** | one generated file (~4 MB) | see the decision below |

**Decision for the owner: which 68000.**
- MAME's SPU 68000 is its new cycle-exact core (`m68000.cpp`, generated by `m68000gen.py`), not Musashi. Firebeat's SPU runs Musashi and only had to match MAME's sound, not its cycles.
- On Twinkle, the 68000 and the PS1 share the mailbox, so the 68000's timing is visible to the PS1. Lockstep needs MAME's core.
- **Recommendation:** port MAME's generated 68000 (one variant) onto the shim. It costs one big compile unit (like a `gen573` block file: build it at `-j2`). Firebeat could switch to it later if it ever wants cycle lockstep.
- **Decided (owner, 2026-10-13): MAME's 68000.**

### Phases

Each phase has an exit test. Sizes are assisted-effort guesses; the 573 went faster than its estimates.

**TW-0. Oracle and media** (a day)
- `tools/ingest.py` for Twinkle sets: BIOS, SPU ROM, dongle, the program CD and Video CD via `chdman extractcd`, the HDD via `extracthd` (memory-capped, holes punched), and a `game.toml` with `core = "twinkle"`, the dongle type and the DVD port (SIO1 or super I/O).
- A MAME build at the pin with `twinkle.cpp` (as `build573.sh`).
- `hiraku.lua`: Twinkle tap groups (I/O mux, SCSI, mailbox, dongle, mixer), the 68000 as a second tapped CPU, the RTC's power-on time.
- `bmiidx3-boot.his`: first boot, E150, 1P Start, attract. Seeded NVRAM afterwards for the later runs.
- **Exit:** two MAME runs of `bmiidx3-boot` match, and the reference run is in `out/oracle/bmiidx3/`.

**TW-1. The board, interpreted, in lockstep** (1–2 weeks)
- `runtime/twinkle/`. First split the PS1 machine out of `runtime/sys573/` into something both cores link, so the recompiler, GPU and scheduler work stay shared.
- Bring-up order: the BIOS to its SCSI boot, the program CD loading, the SPU board's boot (68000, HDD to wave RAM, mailbox), the device check, E150, attract.
- The DVD player is MAME's `xvd701`, unchanged.
- **Exit:** `bmiidx3` boot and attract in lockstep with MAME: main RAM identical every frame, the 68000's RAM and the wave RAM at every snapshot, pixel-identical snapshots, the WAV matching (lag 0, correlation ≥ 0.998). Regress cases `bmiidx3-boot`, `bmiidx3-attract`. Peak RSS (VmHWM) checked before adding them, as for the 573.

**TW-2. Playable** (about a week)
- An `iidx` family in `cabinet.cpp`: keys, turntables (analog axis, as bm3 and DJ Main), Start, VEFX, Effect, the effector sliders.
- Lamps: spotlights, neon, key LEDs, Start/VEFX/Effect, and the 16-segment ticker as text (HID, socket, and an on-screen strip).
- `bmiidx3-play.his`: coins, mode and song select, a song with keys and scratches, the result. In lockstep.
- Launcher entry, PLAY.md.
- **Exit:** the scripted song in cycle-exact lockstep with MAME, then the owner's hands-on test with an IIDX controller.

**TW-3. The background video: further than MAME** (2–3 weeks; the main unknown)

The game already tells the player what to play. HIRAKU adds the picture. The machine doesn't change: the player's serial replies stay MAME's, so lockstep holds. `--video off` gives MAME's picture.

1. **The disc:** parse the Video CD (`ENTRIES.VCD`, the MPEG tracks as Mode 2 Form 2 sectors) to map a chapter to a sector. Check that the shim's CD image keeps raw Mode 2 sectors.
2. **Decode:** MPEG-1 with pl_mpeg (MIT, single header, MPEG-1 only, which is exactly a Video CD), on a worker thread. 352×240 at 29.97 fps costs little.
3. **Timing:** playback starts at the machine time of the PLAY reply, so it is reproducible and can be checked. Measure the real player's seek and start delays only if footage shows they matter.
4. **The mix, the open question.** The sub board keys the PS1 picture over the video. Nobody has documented how. Find out from:
   - the game's writes to the Bt812 and the mixer (0x1f2b0000; MAME's notes: reg 0x08 bit 1 "perform overlay mixing"; HCLOCK 853, ACTIVE_PIXELS 706);
   - the PS1 frame during a song: what fills the video window (a key colour, black, the mask bit?);
   - real-cabinet footage of 3rd style, for placement, scaling and colour.

   Write down which source each choice rests on (decision 2 of §19). Where the sources run out, stop and record the gap.
5. **Exit:** 3rd style's attract and a song show the right clip, in the right place, at the right time, against real-cabinet footage. The audio and the machine stay MAME-exact.

**TW-4. Efficiency** (a week)
- Record `bmiidx3` runs for the MIPS recompiler (`tools/mipsrec.py`). Its blocks come from code the program CD loads into RAM, as on the 573.
- Measure the parts with `HK_PROFILE`: the PS1 GPU (already ~45 % of 573 gameplay; IIDX draws more notes than DDR), the RF5C400, the 68000, SCSI.
- The video decoder and the mix run off the emulation thread.
- PGO profile `twinkle.profdata`.
- **Exit:** the N100 budget (§14): average under 25 % of one core, no frame over ~8 ms of host work, bit-identical to the interpreter.

**TW-5. The rest of the catalogue** (weeks, mostly media and per-set config)
- **1st style (`bmiidx`, on hand), a departure:** add CTS to the PS1's SIO1 (psx-spx documents SIO1's CTS bit), behind a switch, for the DVD check MAME fails. The X76F041 dump lacks only the passwords the game never uses.
- **Substream, Club Version 2 (`twinklex2`):** X76F041, player on the super I/O UART.
- **2nd, 4th, 5th style:** `twinklei`, Video CD, like 3rd.
- **6th, 7th, 8th style: DVD.** An MPEG-2 decoder (libmpeg2, GPL-2+, or FFmpeg's libavcodec, LGPL) and DVD-Video chapter navigation (the IFO tables). 8th style may use a different player (XV-S1100; MAME's code only mentions it).
- **Club kits (`bmiidxc`, `bmiidxca`):** two program CDs; find out what the second one does. *(2026-10-17: the second disc is the DDR cabinet's, 896 JA A01, "DDR CLUB KIT INSTALLER" with a 16 MiB flash image, MAME's `ddr2mc` install disc; the archive's two IIDX discs are the JAA and JAB revisions. All three club sets, `bmiidxc2` too, run alone in lockstep with MAME, Club Version 1 against the SIO1-patched MAME. The link: Club Version 1 on the super I/O's COM2 at 19,200 8N1, Club Version 2 on SIO1 at 38,400. docs/CATALOGUE.md, Phase 6 progress.)*
- *Ugoke! Purimucho* (touch panel, camera, printer) is out of scope.
- **Media needed:** every set except `bmiidx3` and `bmiidx`. Recommended next: `bmiidx2` or `bmiidx4` (Video CD, MAME working), then `bmiidx6` for the DVD path. *(2026-10-14: all of these on hand except the club kits and the other revisions; see the progress below.)*

### Progress (2026-10-13): TW-0

**The oracle:**
- MAME built at HIRAKU's pin, the Twinkle driver only: `mame-pin/twinkle` (beside the checkout), from `buildtw.sh` (memory-capped, `-j3`). Both sets pass `-verifyroms` (`bmiidx` is "best available": MAME marks its dongle as a bad dump).
- `oracle/mame.sh` takes snapshots at the screen's own size for Twinkle (`-snapview native`): the set's layout draws the 16-segment display around the screen. Twinkle's screen is 640×240.
- `oracle/lua/hiraku.lua` knows Twinkle (it tells it from the 573 by its SCSI controller):
  - tap groups for the main board (mailbox, SCSI, super I/O, I/O, RTC, mixer, and the PS1's own) in all three address mirrors;
  - tap groups on the SPU board's 68000 (control, mailbox, ATA, RF5C400), recorded with the 68000's PC;
  - the RTC65271's power-on time ("power-on time N" in `log.txt`), read from MAME's save items: the registers sit behind an index register that a bus read would change.

**The media:** `tools/ingest.py` takes Twinkle sets (`ingest_twinkle`); `games/bmiidx3` and `games/bmiidx` have `core = "twinkle"`, which the launcher hides until the core exists. Both are in `dumps/` (3.9 and 1.7 GB, with holes punched). **The Video CDs have one track per clip:** 38 for 3rd style, 28 for 1st style.

**The reference run** (`oracle/scripts/bmiidx3-boot.his`, 9,000 frames, out/oracle/bmiidx3/boot):
- First power-on: BIOS, SCSI boot, device check by frame ~1,380 with E150 (backup RAM), "1P START = BACKUP CLEAR"; 1P Start at frame 2,400; "Clear completed", warning, Konami, BEMANI, the title, the How To Play demo.
- **Two runs started in the same second match exactly:** the RAM hash on all 9,000 frames, all 2,151,643 device accesses, the 150 snapshots, and the NVRAM. Runs started in different seconds differ by the RTC; HIRAKU will take `--rtc` as the 573 does.
- The cleared NVRAM (`rtc`, `security`) is kept in `out/oracle/bmiidx3/seed/` for later runs (`HIRAKU_NVRAM`).
- **MAME's speed at the pin:** 480–489 % of real time (`-bench 90` from the cleared NVRAM: boot, device check, attract with the demo). The 567 % of §16 was MAME 0.289 idling at E150.

**1st style** (`oracle/scripts/bmiidx-boot.his`, 3,600 frames, no inputs): MAME stops at the device check with **E600 DVD INIT ERROR**: DVD and backup RAM BAD, SCSI, CD-ROM, security and sound OK. The missing CTS, as the driver says. This is TW-5's starting point.

**Found on the way, for TW-1 and TW-3:**
- The super I/O is used only for its second UART, at the PC's COM2 address (0x2f8): no floppy controller, parallel port or configuration-mode access during boot and attract. Those parts can be stubs.
- The mixer gets ~200 writes during boot (Bt812 index/data at 0x00/0x08, a bit-banged serial line at 0x20).
- The How To Play demo draws the middle of the play field dark: where the video goes.

### Progress (2026-10-13): TW-1, the board in lockstep with MAME

**`runtime/twinkle/`**, `core = "twinkle"`, `hiraku-bmiidx3`: a port of `twinkle.cpp`'s driver class (memory maps, the I/O mux, the SPU board's control register and hard-disk DMA into wave RAM, SCSI DMA on the PS1's channel 5, the dongle's port bits, the DVD player on SIO1 or the super I/O's UART). Every device is MAME's own code on the shim, run by the exact scheduler:
- the PS1 as on the 573 (shared library `hiraku_mame573`);
- **MAME's 68000** (the generated cycle-exact core, as decided), the 53CF96 and the XM-5401 on MAME's SCSI bus, the FDC37C665GT, the M24C02 or X76F041 dongle, the RTC65271, the LTC1232 watchdog, the CY7C131 mailbox, the RF5C400, the RS-232 port with MAME's XV-D701, MAME's ATA hard disk on the raw image (`hdd_chs` from the CHD) — library `hiraku_mametw`;
- the shim's additions for them are listed in third_party/mame/README.md (interruptible accesses and wait states, masked submaps, device ROM regions, shares, the periodic interrupt...).

**Result:** `bmiidx3-boot` (first power-on, device check, E150, 1P Start, attract, the How To Play demo; 9,000 frames, `--rtc` from MAME's log):
- main RAM identical to MAME on **all 9,000 frames**;
- **all 150 snapshots pixel-identical**;
- **all 2,151,644 traced device accesses identical**, PS1 and 68000 sides, at the same emulated times (to the nanosecond the doubles keep).
- **the sound matches MAME's WAV** (`--wav` against `WAV=1`, same `--rtc`): lag 0 in every window with sound, correlation ≥ 0.997, level 1.00 (the music starts ~61 s in);
- regress case `bmiidx3-boot`; peak memory ~75 MB early in the run (the 573's runs are ~250 MB).
- Host cost, interpreted, desktop: ~3.2 ms a frame (MAME at the pin: ~485 %).
- With the shim's additions, every existing regress case stays identical (the 573's, kbm's and the rest).

**What it took, beyond the port:**
- The 68000's program needs no byte swap here: the file's little-endian words are already how the shim's big-endian space holds them on this host. With a swap, the 68000 started at 0x8600 and took a Line-F exception at once.
- **Masked submaps** were the first divergence (frame 1,079): the shim put the 53CF96's 8-bit registers at consecutive bytes, while MAME spreads them over the lanes `umask16(0x00ff)` selects (two to a 32-bit word: register 8 at 0x10). The game's write-and-read-back test of the configuration register then failed.
- **The effector's sliders start centred in MAME** (frame 1,273): an `IPT_AD_STICK` field whose default is its minimum reads (65536 · (15 ≪ 24) / 131072 + 2²³) ≫ 24 = 8, so the I/O mux gives 0x88, not 0.
- `--trace` writes the oracle's MMIO format with the same groups and folding; comparing two traces record by record found both divergences in minutes.

### Progress (2026-10-13): TW-2, playable

- **Controls** (`cabinet.cpp`, family `iidx`), by MAME's names: the 14 keys (`P1-1 (SW1)` …; bindings files name them as the other beatmania cabinets do, `P1 Button 1` …, since the launcher gives the three one file), the turntables (an analog axis, or two buttons each, through the host's turntable path: 1024 counts a turn become MAME's 8-bit "1P Table"/"2P Table" position, as DJ Main's dials), Start ×2, Effect, VEFX, the five effector sliders, Coin, Test, Service. Keyboard defaults Z S X D C F V and M K , L . ; /; a USB IIDX controller's keys on buttons 0–6.
- **Lamps:** the key lamps (`key_led_w`), Start, VEFX, Effect and credit (I/O 0x37), the eight spotlights (0x87, in MAME's order), the neon (0x8f), the board's LEDs, by name for HID LEDs and the lights socket.
- **The 16-segment display** as text (`Core::display_text()`, new and optional), shown in the window title: it scrolls "BEATMANIA 2DX 3RD STYLE", then the mode prompt. `HK_TW_DISPLAY=1` prints its changes.
- **The launcher** lists Twinkle games (an IIDX icon, the ingest instructions); docs/PLAY.md has a Twinkle section.
- **Sound:** MAME's mix peaks at 1.07 of full scale in a stage; the core plays at 0.8 of it by default, `--mame-mix` gives MAME's level (as the 573's).
- **`oracle/scripts/bmiidx3-play.his`** (10,000 frames, from the NVRAM `bmiidx3-boot` leaves, which is byte-identical to MAME's): two coins, Start, mode and sound select, the first stage (5.1.1.) played with a fixed pattern of the seven keys and turntable spins in both directions, the result (failed), game over, attract. **In lockstep with MAME:** main RAM on all 10,000 frames, 166/166 snapshots, all 3,067,812 traced accesses, the sound lag 0 in all 63 windows with sound (correlation ≥ 0.995 but one window at 0.988, level 1.00–1.03). Regress case `bmiidx3-play`, after `bmiidx3-boot`.
- **Left for TW-2:** the owner's hands-on test with an IIDX controller (keys, turntable feel, lamps).

### Progress (2026-10-13): TW-3, the background video (further than MAME)

**What the game asks of the player** (decoded from the traced UART writes; 11-byte JLIP packets): in attract, *seek chapter 1* at 58.5 s and *stop* at 80.1 s (21.6 s; AVSEQ01, the "WELCOME TO THE CYBER-beat-NATION" intro, is 22.4 s); in a game, *stop*, *pause*, *seek chapter 36* while paused, *play* as the stage starts, *pause* when it ends.

**The disc:** a Video CD, 37 entries (`ENTRIES.VCD`), one MPEG-1 clip per track (`AVSEQ01`–`37`, 352×240, 29.97 frames a second), every clip already shaped to the play field's window (a central column and a narrower foot, black around). **Chapter N is entry N** (MAME's reading of the command: three digits, divided by ten for a VCD).

**The sub board, as the game sets it** (MAME stores none of it):
- mixer register 0x08: 0x09 in menus, **0x0b during a stage** (bit 1, MAME's "perform overlay mixing"), back to 0x09 after; also 0x0b for the attract intro (frames 3,628–4,790), while the game draws nothing but black: the intro movie fills the screen and ends on "WELCOME TO THE CYBER-beat-NATION", then the title. A second, independent case for the model below;
- the Bt812's picture controls, rewritten every frame of a stage: brightness −20, contrast 0x76 (92 %), hue 0, and **saturation animated** (ramps from 2 to 0x39, then flashes to 0x80/0xfc: an effect on the video).
- Inside the video window the PlayStation draws only pure black, and white particles on top.

**What HIRAKU does** (`runtime/twinkle/video.*`, `--video on|off`, default on):
- MAME's `xvd701` reports the player's state after each command (a HIRAKU edit: chapter, stop/play/pause, power, whether the chapter restarts); playback runs from the machine time of the command, so the picture at a given machine time is always the same, and the machine is untouched;
- the clip is read from the Video CD's raw sectors (Mode 2 Form 2) and decoded with FFmpeg's libavformat/libavcodec (the system's, through pkg-config; optional: without them Twinkle builds with MAME's picture), on through the following entries as a player plays on. pl_mpeg came first and was dropped: on this disc a fifth of some frames came out wrong (blocks), against FFmpeg's clean decode, and it can't do the MPEG-2 of 6th–8th style's DVDs anyway;
- **while mixer bit 1 is on, the video replaces every pure-black pixel of the PlayStation's picture**, scaled to the whole frame, through the Bt812's brightness, contrast, saturation and hue. The clip's window then fits the gap between the 1P and 2P lanes exactly, with the game's particles on top.
- Checked: the picture HIRAKU composites from a frame is the same as a stand-alone compositor's output from FFmpeg's decode of that frame; regress case `bmiidx3-video` (the scripted game with the video on, identical run after run); `bmiidx3-boot` and `bmiidx3-play` run with `--video off`, MAME's picture.
- Cost: decoding on the emulation thread, a few ms in some frames (p99 18.7 ms against ~12 without, desktop, a loaded machine); TW-4 moves it to a worker thread.

**Not known, and what the choices rest on** (decision 2 of §19):
- **Which clip belongs to which song.** 5.1.1. asked for chapter 36, a CG sky with rainbows and lyrics; remywiki describes 5.1.1.'s movie as a first-person view of an old man's dream with no people. Either 3rd style's disc pairs them differently, or the chapter numbering is off by some rule the attract intro doesn't show. Needs real-cabinet footage of a 3rd style stage.
- **The key:** pure black (the only colour the game draws in the window besides its white particles). Whether the sub board keys on black, on a luma threshold (mixer register 0x30 = 0x10?) or on the PlayStation's mask bit is not known.
- **Placement and scale:** the whole 352×240 picture over the whole 640×240 screen; the game's Bt812 timing (HDELAY 126, ACTIVE_PIXELS 706, VDELAY 22) may shift it by a few pixels.
- **The player's delays** (seek, start) are taken as none.

### Progress (2026-10-13): TW-4, efficiency

Measured on the desktop (Ryzen 5700X3D) with the scripted game from the cleared NVRAM, its RAM hash left out (the hash alone was 28 % of a run).

| | mean | a stage: mean / p99 | worst frames |
|---|---|---|---|
| TW-3, interpreted, video decoded on the emulation thread | 3.03 ms | 2.6 / 3.1 ms (video off) | 18–40 ms, CD loads |
| recompiled R3000 (`tools/mipsrec.py bmiidx3`, 6,242 blocks, 99.5 % of instructions) | 1.53 ms | 1.29 / 1.60 ms (video off) | the same |
| + the video decoded on a worker thread, converted once per frame in integers | — | 1.53 / 1.95 ms (video on) | the same |

- **The recompiler is the 573's** (`runtime/sys573/mips.*`, `tools/mipsgen.py`): the same PS1, its code loaded from the program CD into RAM. Exact: main RAM on all 10,000 frames of `bmiidx3-play` and 166/166 snapshots still match MAME. The recording (build/bmiidx3/mips573.txt, never committed) covers the boot, attract and the scripted game.
- **The video** decodes on a worker thread, up to 8 frames ahead; the emulation takes the frame it needs in order, waiting for it if the worker hasn't got there, so the picture is the same on every run. The colour conversion runs once per video frame (or Bt812 change), in integers.
- **The hard disk image** asks the kernel for each 8 MiB stretch ahead (`POSIX_FADV_WILLNEED`): from a cold page cache the run's worst frames are the same as from a warm one.
- **PGO:** `tools/pgo.py` trains Twinkle too (`twinkle.profdata`, for `hiraku_mametw` and the board); not run yet (it rebuilds everything). The PS1 parts keep the 573's profile.
- **Tried and dropped:** cheaper shim scheduling (pooled `synchronize()` timers, `attotime::seconds()` without a 128-bit division, cached cycle lengths) made the CD-load frames 16 % slower in an alternating A/B (13.1 → 15.2 ms); likely the edited hot functions losing their 573 profile.
- **The CD loads (answered 2026-10-15, below).** When the game reads its program CD (screen changes: title, menus, after a stage) about 110 frames cost 13–40 ms on the desktop, so likely 30–100 ms on an N100. The time is MAME's SCSI model, which hands every byte over through zero-delay timers (`nscsi`'s REQ/ACK handshake, the 53CF96's DRQ), and the scheduler round each of them takes. MAME pays it too. Options: a cheaper path through the shim's scheduler and devcb for these events, measured with its own PGO; or a departure, moving whole sectors at the timing MAME's byte steps add up to (a switch, MAME's exact path kept for the oracle).
- **N100 estimate** (not measured: ~2.5× this desktop): a stage at ~4 ms with the video, a quarter of a frame, within the budget; the CD loads are not.

### Progress (2026-10-14): TW-5, the 1st style (further than MAME)

beatmania IIDX 1st style (`bmiidx`) boots, plays and shows its videos; MAME stops it at E600.

- **Why E600:** the game's routine that sends the player a byte (0x8004d248) sends only while SIO1's status has TX ready (bit 0) **and CTS** (bit 8), and gives up after about 40 frames. MAME's SIO has no CTS input (its `twinkle_dvd_type1` has the line commented out, "CTS isn't implemented in SIO").
- **The departure:** for the 1st style SIO1 is windyfairy's UART (`sio_uart.*`, already HIRAKU's SIO1 on 573s with the multi-session unit: RTS out, CTS in, the 8-byte receive FIFO), switched in by `hk_psx_sio1_uart`, with the player's CTS wired as MAME's comment has it (the XV-D701 holds it on). The game sets 8 bits, odd parity (the fork reads mode bits 4-5 as psx-spx's "vice versa" note has them; the player's own frame is 8O1), 9,800 baud. Up to the first SIO1 access (frame 1261) the run is MAME's, frame for frame.
- **The oracle:** a MAME patched the same way (`oracle/mame-patches/twinkle-sio1-cts.patch`, built in a worktree of the pin). Against it, `bmiidx-clear` (first boot, test mode's DVD check and backup clear, TEST EXIT, the watchdog reset, attract; 10,820 frames) and `bmiidx-play` (3 coins, 7KEYS, the stage, STAGE FAILED, game over, attract; 14,000 frames): main RAM on every frame, 180/180 and 233/233 snapshots, every traced device access identical in time and data (2.55 M and 3.54 M runs of accesses; the traces' frame tags differ only where a 68000 burst crosses a frame boundary), sound lag 0, correlation >= 0.998. The NVRAM the clear leaves is byte for byte MAME's.
- **Bugs found on the way:**
  - The fork's UART never set `m_baud` (nor three other members): MAME read heap garbage (0x30), HIRAKU 0. Zeroed in its constructor (both).
  - The shim kept a CPU's cycle count over a reset; MAME's `interface_pre_reset` zeroes it (and resumes the CPU). The 68000 syncs autovectored interrupt acknowledges to its E clock by that count, so after the 1st style's watchdog reset it ran 5 cycles off MAME's. Now as MAME (`cpu_device::hk_pre_reset`), and the soft reset eats the executing CPU's cycles as MAME's `eat_all_cycles` does.
  - MAME runs the autoboot script again on every reset: `hiraku.lua` restarted from frame 0 mid-run, truncating its own files. It arms once now.
  - The exit report's 68000 PC was always 0 (the shim's PC hook only runs under a debugger): a `hk_pc()` accessor.
- **Differences from 3rd style:** no 1P Start backup clear (test mode's BACKUP CLEAR); a credit is 3 coins; mode select (4KEYS/5KEYS/7KEYS/EXPERT) by the turntable. The stage videos are masked to the play field's shape on the disc, and the game draws its own graffiti layer (the song's letters, spray-painted) over them, in white: the full-frame scaling lines up with the play field exactly, which supports TW-3's placement.
- **Speed:** with its R3000 blocks recorded (6,907, from `bmiidx-clear` and `bmiidx-play`; exact against MAME), the scripted game takes 1.55 ms a frame on the desktop with the video, a stage 1.78 ms (p99 2.3).
- **The Video CD from a cold disk:** the emulation waits for the frame it needs, and the decoder thread read the disc a sector at a time; on a busy disk (another session's runs, swap) single frames took 0.9 to 3 s, and TW-3's "host contention" stall was this. Each chapter is now asked of the kernel whole when it starts (`POSIX_FADV_WILLNEED`): from a cold cache the worst frame is 47 ms, as with the disc cached.
- **Open:** a hands-on test. The other sets on the same SIO1 wiring (`bmiidxa`, the club kit's `bmiidxc` and `bmiidxca`) aren't on hand. *(All three on hand since 2026-10-16/17 and in lockstep with the patched MAME: docs/CATALOGUE.md, Phases 1 and 6.)*

### Progress (2026-10-15): TW-5, the rest of the catalogue on hand

The owner downloaded the remaining sets on 2026-10-14. All 21 new CHDs match MAME's SHA-1s and all seven sets verify (`-verifyroms`; Substream's dongle `983a02` is MAME's `BAD_DUMP`, best available). Every style below boots, clears E150 and plays a scripted stage; the VCD styles and Substream with no change to the core.

| Set | Style | Program CD | Player's disc | HDD | Clear prompt | Lockstep with MAME (stock pin) |
|---|---|---|---|---|---|---|
| `bmiidx2` | 2nd style (GC985 JAA) | `gc985a01` | Video CD `gc985a04` | `985hdda01` | ~f2300 | boot, play |
| `bmiidx4` | 4th style (GCA03 JAA) | `a03jaa01` | Video CD `a03jaa02` | `a03jaa03` | ~f2280 | boot, play |
| `bmiidx5` | 5th style (GCA17 JAA) | `a17jaa01` | Video CD `a17jaa02` | `a17jaa03` | ~f2300 | boot, play |
| `bmiidxs` | Substream (GC983 JAA), `twinklex2` | `gc983a01` | Video CD `gc983a04` | `983hdda01` | ~f3100 | boot, play |
| `bmiidx6` | 6th style (GCB4U JAB) | `b4ujab01` | DVD `b4ujaa02` | `b4ujaa03` | ~f2340 | boot, play |
| `bmiidx7a` | 7th style (GCB44 JAA) | `b44jaa01` | DVD `b44jaa02` | `b44jaa03` (40 GB) | ~f2940 | boot, play |
| `bmiidx8` | 8th style (GCC44 JAA) | `c44jaa01` | DVD `c44jaa02` | `c44jaa03` | ~f2940 | boot, play |

- **Lockstep, all seven (checked 2026-10-15):** every frame of each boot and play run has the same main-RAM hash and time as MAME's run of the same script with the same clock (`out/oracle/<set>/boot`, `.../play`), 9,000–11,300 frames each. They are regress cases: `<set>-boot` and `<set>-play` for all seven, and `bmiidx6-video` with 6th style's DVD picture on.

- **Media:** from 4th style on the set names the three disks 01 (program CD), 02 (the player's disc), 03 (the SPU board's hard disk). MAME keeps the DVDs as hard-disk CHDs (GDDD, 2048-byte sectors), so `tools/ingest.py` now picks the player's disc and the hard disk by their names, and writes a DVD as `video_dvd` in machine.txt.
- **Disk space and the BX500:** the DVD styles' disks were extracted to the home NVMe (`~/hiraku-hdd/<set>/`, linked from `dumps/`): 7th style's hard disk is 40 GB logical (5.8 GB after hole punching). On 2026-10-15 01:17 the DATA SSD timed out under an hour of sustained writes (a DVD extract, four MAME runs, another session's regress) and went read-only (fsck clean, SMART clean); heavy IO on DATA is now one job at a time.

**The DVD (6th style on), further than MAME.** MAME's player model (`xvd701`) shows no picture and assumes a Video CD: a seek's three digits are read as a Video CD's two (the chapter divided by ten). The DVD styles send three: 6th style asks for "085" for 5.1.1. (MAME: chapter 8), 7th "064" (MAME: 6), 8th "004" and "001" (MAME: 0, which its model answers with an error). MAME's replies, which the game sees, are unchanged; HIRAKU's picture follows the game's commands instead (`DvdState` in `runtime/twinkle/board.cpp`: xvd701's rules with the full chapter number; the commands come through a second HIRAKU hook in xvd701, `m_hk_command`, which `HK_TW_PLAYER=1` also prints).
- **The disc:** ISO 9660 finds `VIDEO_TS`; `VIDEO_TS.IFO`'s title table gives title 1's title set; its IFO maps chapter N (part of title) to a program chain and program, the program to its first cell, the cell to its first sector in the title VOBs. All three discs hold one title (90, 79 and 80 chapters, one cell each). FFmpeg demuxes the VOBs (MPEG-PS) and decodes the MPEG-2 (720x480, 29.97 fps), on the same worker thread as the Video CD.
- **Checks:** 6th style's chapter 85 and 7th style's chapter 64 are 5.1.1.'s own clip, its title in the picture ("dj nagureo: 5.1.1."). 8th style's chapter 4 is another clip: chapters 1-4 there are all 2:11 long, likely shared clips for older songs. That needs footage of a real 8th style cabinet.
- **Open:** whether a real XV-D701 treats "004" as chapter 4 (MAME's model and the real player differ for chapters 1-9 on a DVD, and the game sees that reply). MAME's error reply is kept for lockstep; a switch to the real player's answer waits for evidence.

**Speed:** with each style's R3000 blocks recorded from its boot and play scripts (`build/<set>/mips573.txt`, 6,000 to 12,600 blocks), the scripted games average 2.6 to 3.0 ms a frame with the video, the DVD styles at the top of that, on a desktop busy with a MAME run and another session; 3rd style took 2.7 ms in the same conditions (1.5 ms on a quiet machine, TW-4). Interpreted, about 4 ms. The MPEG-2 decode runs on the video worker thread, as the Video CD's does.

### Progress (2026-10-15): the CD loads

- **What they cost.** A load is a run of frames at ~17–19 ms of host work each on the desktop (the 3rd style's after sound select: 29 frames, ~1.5 MB). MAME's XM-5401 has no read time: every byte of a READ goes over the SCSI bus at once, one 53CF96 step (its settle delay) and one zero-delay DMA timer each, ~3.2 MB/s. That is ~52,600 bytes a frame, and between those events MAME's scheduler runs both CPUs in 3–5-cycle slices: ~130,000 slices a frame. The time is split about evenly between the SCSI model, the scheduler and the CPU slices.
- **Exact cheaper events don't reach the budget.** The two CPUs' interleaving depends on those event times (the quantum is a frame), so to stay MAME's every slice stays. Caching the scheduler's per-clock divisions and skipping needless timer searches gave 6–7 % without PGO, and the same kind of change measured slower under the 573 profile on 2026-10-13. Not kept.
- **The drive's read speed (the default; `--mame-cd` is MAME's).** The XM-5401 the board has is a 4x drive: 300 sectors a second. `nscsi_cdrom_device::hk_set_read_rate()` (hiraku-edits.py) delays a READ's data phase by one sector's time (MAME's `scsi_data_command_delay()` hook). After each sector, the next waits until the drive has read it (`scsi_data_byte_period()`). Within a sector the bytes still go at the bus's pace, MAME's handshake. Between sectors nothing fires, and the CPUs run in their usual slices.
  - The 3rd style's load: 29 frames at ~17 ms → 138 frames (2.3 s, about the real drive's) at ~4.7 ms; ~10,200 controller events a frame.
  - The game plays the same: the scripted game reaches the same menus, song and result, a little later.
- **The keysound loads, exact.** After a load, the SPU board reads its keysounds from the hard disk by DMA: three frames at 16 ms. MAME's ATA path takes each word through the interface's slots, two virtual calls and four DMA-mode checks.
  - `hk_read_dma_burst()` (hiraku-edits.py: `ataintf`, `atadev`, `hdd`, `atahle`) takes a buffer's words but the last in one go, when `dma_r()` would take each through `read_data()` with nothing else changing. The last word still goes through `dma_r()`, which refills the buffer or ends the transfer.
  - The shim's hard disk image is mapped instead of `pread()` a sector at a time.
  - Those frames: 14–17 → 5–7 ms, identical.
  - **2026-10-20:** the 4x drive's worst frames (8,849–8,851, the result screen) were keysound DMA too. `hk_read_dma_burst()` now copies the buffer's words with one `memcpy` (little-endian hosts; the word swap otherwise), and the SPU board's DMA request bursts them straight into wave RAM, where they went through a 256-word buffer and were copied again one by one. Found with `HK_PROFILE_FRAMES=8849-8852 HK_PROFILE_MAIN=1` (the sampler on those frames' work and the main thread only).
- **The whole scripted game** (`bmiidx3-play`'s inputs, no RAM hash; desktop):

  | Program CD | p99 frame | worst frame |
  |---|---|---|
  | MAME's, before | 16.3 ms | 35.0 ms |
  | MAME's (`--mame-cd`), with the keysound path | 8.2 ms | 24.5 ms |
  | the 4x drive (default) | 4.9 ms | 10.9 ms |
  | the 4x drive, DMA by memcpy (2026-10-20) | 5.7 ms | 7.4 ms |

  (The last row is a later measurement, with the fine sound on, from frame 2,000: p99.9 6.15 ms, no frame over 8 ms where 15 were.)

- **Regress:** every Twinkle case runs with `--mame-cd`, identical to its baseline (still in lockstep with MAME). Each play case also has a `-drive` twin on the 4x drive, from the same NVRAM (`tools/regress.py`).
- **Left:** the worst frames (~7 ms, the result screen) and the loads' ~4.7 ms are desktop numbers; an N100 is likely ~2× that. No seek time is modelled (the drive's ~120 ms access); the data phase starts one sector's time after the command.

### Risks
- **The mix is undocumented** (TW-3). Fallback: a plain "video behind the PS1 picture where it is black" mode, labelled as a guess.
- **The 68000 port** is a big generated file; if the shim can't host it cleanly, Musashi with tolerance-based checks on the SPU side, accepting that lockstep stops at the mailbox.
- **SCSI is a new bus on the shim.** MAME's nscsi uses its own handshake timers and line callbacks; the exact scheduler must reproduce their ordering.
- **DVD styles** add an MPEG-2 decoder: heavier on an N100, but on its own core.

## 22. ARM hosts (2026-10-13)

Step 1 towards an Android/ARM version: the cores on 64-bit ARM Linux. The recompilers emit C and
C++, not x86 code, and neither guest CPU has an FPU, so the work was in the host glue:
- **Fibers:** `hk_fiber_switch` in AArch64 assembly too (x19-x30, d8-d15, FPCR). The ucontext
  fallback costs ~14 % (a system call per switch) and Android's libc has no ucontext at all.
- **Same results everywhere:** `-ffp-contract=off` (AArch64 compilers fuse `a*b+c` by default;
  no change on x86-64, which has no FMA without `-march`) and `-fsigned-char` (plain char is
  unsigned on AArch64). minimp3's NEON path rounded its 16-bit output differently from the SSE
  path; it now rounds as SSE does (`hiraku-edits.py`).
- **Page size from the host:** the 573's RAM and its write-protect guard used 4 KiB; ARM hosts
  can have 16 KiB pages (Android 15+, Asahi).
- The profiler's ticks (`cntvct_el0`) and sampled PC; a NEON copy of the K057714 blit.
- `cmake/aarch64-linux-gnu.cmake` cross-builds with clang on Arch's `aarch64-linux-gnu-gcc`,
  statically linked, so qemu-aarch64 runs the binaries (and the build's own `hiraku-recomp`).

**Result:** the recompiler's output on ARM is byte-identical to x86's, and all 17 regress cases
give the same digests on the ARM build (under qemu) as on x86. Not measured yet: speed on real
ARM hardware (qemu says nothing about it). Android itself (GLES renderer, frame pacing,
launcher, packaging of the per-dump game code) is still to decide.

## 23. Konami Python 1/2 (M16, started 2026-10-13)

The owner asked for the groundwork for a Python core. The facts behind this section are in docs/PYTHON.md. The work files (sources, the Reliquary build, oracle outputs, survey notes, decoded boot records) live in `Python-work/` beside the checkout, outside the repo; the oracle build and runner are in `oracle/pcsx2/`.

### What is different from every core so far

- **Python is a PlayStation 2.** EE (R5900 + VU0/VU1), GS, IOP (R3000A), SPU2, IPU, DMAC/VIF/GIF. PCSX2's core is ~365,000 lines; MAME's whole 573 port was ~16,000. Porting a PS2 into HIRAKU device by device, as was done for DJ Main and the 573, is not a few-week job.
- **There is no MAME oracle.** Every Python 1 and Python 2 set in MAME is a NOT_WORKING skeleton. The only emulator that runs these games is **PCSX2 Reliquary**, a GPL-3.0+ PCSX2 fork. "Match MAME first" becomes "match Reliquary first", with two caveats:
  - PCSX2 is not cycle-exact hardware; its timing model is its own. Lockstep against it means matching *its* model, as HIRAKU matched MAME's.
  - Reliquary emulates the Python 1 interface board (a TX39 MIPS with its own firmware) at the FireWire command level; the board's CPU never runs. So below that layer the reference is an HLE, not hardware truth.
- **The game program is easy to get at.** Each disk carries the EE program as plain MIPS, only LZ-compressed (docs/PYTHON.md §4). The offline decode of pop'n 9's equals Reliquary's EE RAM byte for byte in its code. So a static recompiler has its input without any decryption; only the memory-card boot loader is a KELF.

### Measured (2026-10-13, Ryzen 5700X3D, pop'n 9 attract, Reliquary built here)

- **Reliquary builds on Linux** with system packages plus six small local dependencies: 83 s, 1.3 GB peak (`oracle/pcsx2/build-reliquary.sh`).
- **pop'n 9 boots to attract in it:** device checks, warning, title, How To Play, looping every ~100 s.
- **Host cost at real-time speed**, whole process, from 30 s to 150 s of attract:

  | CPU mode | GS renderer | Host CPU |
  |---|---|---|
  | JITs (default) | software | 44 % of one core |
  | JITs | Vulkan | 34 % of one core (plus the GPU) |
  | interpreters | software | 78 %, and still **below real time**: ~0.75× (3,000 frames in ~66 s unlimited) |

  - Unlimited, the JITs run 3,000 frames in ~7 s including start-up: 7× real time or more.
  - For comparison, HIRAKU's bm3final takes 10–13 % of a core and ddr2m ~10 %. pop'n 9 in Reliquary is 3–4× that, before any N100 discount (×2 or worse per core).
- **Runs repeat exactly.** Same settings and CPU mode: EE and IOP RAM identical at all 50 checkpoints over 3,000 frames, frame-limited or not (the `HK_FRAMEHASH` hook). JIT and interpreter runs differ from frame 60 on, so the oracle pins its CPU mode.
- **PCSX2's EE timing is a static table:** each opcode has a cost in 1/8 cycles (`R5900OpcodeTables.cpp`; 9 by default, 2×8 for MULT, 14×8 for DIV), summed per block and scaled at the block's end (`Interpreter.cpp:31-52, 214`). A recompiled block can carry its cost as a constant, as on the 573.
- **pop'n 9's EE program** decoded offline equals Reliquary's EE RAM in all its code; 1.4 MB, ~13,000 `jal`.

### Options for the core

1. **Host Reliquary's emulation core inside HIRAKU** (`runtime/python/`, `core = "python"`). Link PCSX2's `pcsx2` core library, which doesn't need Qt (checked: `pcsx2-gsrunner` builds against it with no Qt library linked), and implement its `Host::` interface on HIRAKU's shell: window, audio, input scripts, cabinet controls, lamps, NVRAM.
   - Playable soonest. Identical to the oracle by construction.
   - HIRAKU then owns a fork of a ~365,000-line emulator that its upstream re-syncs monthly; efficiency is PCSX2's, not better.
2. **Port the PS2 into HIRAKU piece by piece**, the 573 way.
   - Matches the codebase's style. Months of work before anything boots, against a reference that isn't cycle-exact anyway.
3. **Both, in order (recommended).** Start from (1), then replace parts behind the same seams only where a measurement says it pays:
   - **the EE game program by static recompilation**, the Firebeat pipeline on MIPS: Ghidra manifest (Ghidra has no R5900 language; it needs an extension or HIRAKU's own decoder), one C function per guest function, PCSX2's EE interpreter as the fallback and as the shadow-mode reference in the same process;
   - the interface board's I/O replaced by HIRAKU's cabinet layer (pop'n buttons, lamps, latency);
   - the GS: Reliquary's software renderer for the oracle, its Vulkan renderer for play, unless the N100 numbers say otherwise.
   - Each replacement is checked in lockstep against the unmodified Reliquary path, as the 573 is against MAME.

The efficiency case is weaker than on the 573. There the baseline was an interpreter; here it is PCSX2's mature x86-64 JITs. HIRAKU's gains on Python would come mostly from latency, I/O and the shell, plus whatever the EE recompile and an idle-skip give over the JIT. PY-0 measures this before anything is decided.

### Roadmap (revised 2026-10-13, after PY-1, from a measured profile)

PY-0 (the oracle) and PY-1 (Reliquary hosted, pop'n 9 playable) are done; see Progress below. The phase list written before them had the EE recompile as the main work. A profile of the running core says otherwise.

**Where the host time goes** (pop'n 9, `popn9-play.his`, Vulkan GS, 5700X3D, the core's `HK_PY_PROFILE=1` sampler; ~45,000 samples):

| Thread | Share of all samples | Inside it |
|---|---|---|
| GS | ~63 % | CPU-side texture decoding and swizzling (GSVector ops, `ReadAndExpandBlock16`, ...) ~35 %; texture-cache hashing (XXH3) ~15 %; HIRAKU's picture conversion (`on_gs_frame`) ~9 %; the Vulkan driver ~6 % |
| CPU (EE, IOP, VU, SPU2, board) | ~37 % | SPU2 mixing ~31 %; EE JIT code ~17 %; libc copies ~14 % (part of it the core's 0.9 MB picture copy per frame); IOP JIT code ~7 %; event and counter scheduling ~5 %; CHD decompression ~2 % |

- **The EE recompile is no longer the main work.** EE code is ~6 % of all samples, about 1 % of a desktop core. A static recompile can't win more than that on pop'n 9.
- **The cost is in the GS, the picture handoff and SPU2.**
- **Per 100 s of game** (`--no-picture` A/B): Vulkan ~18 % of a core (CPU thread ~9 %, GS ~7 %, picture readback ~2–3 %); software GS ~26 %.

Each phase ends with `tools/regress.py check popn9-play` bit-identical, unless it says otherwise.

**PY-1b. Finish the playable base** (days)
1. **The owner's hands-on test**: picture, sound (crackles?), feel. Its findings come first.
2. **Lamps.** Decode the interface board's JAMMA output command (0x0e0/0xf80 in `KonamiPython1.cpp`) into a Python pop'n lamp family in `cabinet.cpp`. The ground truth is the game's own test mode, I/O CHECK > LAMP CHECK, lamp by lamp. Then the lights socket and HID lamps (docs/LIGHTS.md).
3. **Launcher.** The game shows from `games/popn9/game.toml`; its "not built" hint must name `oracle/pcsx2/build-reliquary.sh`. Windowed runs get `--renderer vk`.
4. **NVRAM.** Check that settings, bookkeeping and the disk overlay survive restarts, and that ALL FACTORY SETTINGS in test mode works.
5. **Packaging** (`tools/package.sh`, the owner's laptop test):
   - ship `hiraku-popn9`, Reliquary's `resources/` and the three local libraries (ryml, plutosvg, plutovg), with an `$ORIGIN` rpath;
   - drop the hard FFmpeg dependency (`USE_LINKED_FFMPEG=OFF`, loaded at run time) if the build allows.
6. **Reliquary updates.** Pin the version (1.9.4) and write down how to re-apply the patch to a new release. Keep the patch small: taps and hooks, no logic.

**PY-2. Measure properly** (1–2 days)
- An A/B harness (alternating runs, medians), as for the 573; `HK_PY_PROFILE` for the breakdown.
- **Low-end hardware:** the owner's Haswell laptop (Mesa's `hasvk` Vulkan, or `--renderer gl`), the MacBook pack, an N100 when there is one. Judge by frame-time spikes, not means.
- **Exit:** a cost table per part, desktop and low-end. It decides how far PY-3 goes.

**PY-3. Efficiency, cheapest first**
1. **The picture handoff** (~9 % of GS samples, plus copies on the CPU thread):
   - hand RGBA over as it comes and let the shell's GL upload swizzle it;
   - swap buffers instead of copying;
   - keep one download texture instead of creating a render target and a download texture every frame (a patch to `SaveSnapshotToMemory`'s path).
2. **PGO for Reliquary's core with HIRAKU's**, trained on `popn9-play`, as `tools/pgo.py` does for the other cores (the 573 got 1.44×).
3. **GS settings that leave the picture unchanged:** texture preloading and hashing levels. Check them with picture digests on a Vulkan regress case, which this needs.
4. **SPU2:** profile-driven speed-ups that keep the output bit-identical, small and upstreamable. It stays on the CPU thread, because the game reads its state back.
5. **Only if the readback still shows:** present the GS's picture straight into the shell's window, sharing the texture. That interacts with HIRAKU's picture modes (CRT shaders), so the readback stays the default.
- **Exit:** a budget set from PY-2, like M11's: a share of an N100 core on average, and no frame over a set host time.

**PY-4. Latency and I/O** (HIRAKU's reasons to exist here)
- **Input:** find how often the game's IOP driver polls the interface board over FireWire (the board's command trace, `FW_VERBOSE_LOGS`). Then measure key-to-picture with a latency script, as for pop'n 4 and Animelo 2.
- **Picture:** the picture of vsync N reaches the shell at vsync N+1. Add an option to wait for vsync N itself, trading GS/EE overlap for a frame less latency, and measure both.
- **Sound:** delivered once per vsync (16.7 ms batches). Deliver per SPU2 chunk for less audio latency.

**PY-5. The rest of Python 1, as media arrive**
- **pop'n music 10–14:** a `games/<id>/game.toml` each (the ingester prints the EE program's SHA-1), the IoMode, and a play script as a regress case.
- **Dancing Stage Fusion** needs a clean dump, or a documented repair (the boot ROM's missing 32 bytes, record A's first sector).
- The non-BEMANI titles only if wanted.

**PY-6. Python 2** (needs media: DDR SuperNOVA 1/2, GuitarFreaks/DrumMania V–V3)
- `.py2` ingest; P2IO over USB; the DEV9 hard disk bound by DNAS to the HDD ID and the i.LINK ID; the e-amusement card reader stubbed or emulated as Reliquary does.
- Cabinet families: DDR exists; GuitarFreaks (neck, pick, wailing, effect knob) and DrumMania (pads, pedal) are new.

**PY-7. Optional, only where PY-2/PY-3 measurements show the CPUs matter**
- **The EE recompiled statically.** The groundwork stands: plain-MIPS program on the disk, a static cost table, PS2Recomp's tables as a cross-check. Upper bound ~6 % of samples on pop'n 9; a heavier 3D Python 2 game could change that.
- **The IOP on the 573's MIPS recompiler** (the same R3000A): ~7 % of the CPU thread.
- **The interface board's TX39 firmware run for real**, instead of Reliquary's command-level emulation: only if a game needs behaviour the emulation lacks. It would be a departure from the reference, so it needs a hardware source.

### Progress (2026-10-13): PY-0 and PY-1

**The oracle, `oracle/pcsx2/`:**
- **The patch** (`reliquary-oracle.patch`, six files):
  - the RAM-hash hook;
  - `pcsx2/HirakuHooks.h`: taps for the vsync, the audio output after the interface board's ADPCM, and the displayed picture;
  - the keystore printout made opt-in (`PCSX2_PRINT_KEYSTORE`);
  - a CMake entry point (`-DHIRAKU_DIR`) that builds HIRAKU's Python core inside Reliquary's tree. Reliquary's CMake assumes it is top-level in ~50 places, so it can't go inside HIRAKU's.
- **Memory-card slot 2 is off** in the oracle as in the core: a Python cabinet has only the dongle, in slot 1. With slot 2 on, the BIOS probes it and the runs part at frame ~180.
- **Dropped from PY-0:** scripted P1IO input in `pcsx2-qt`. The core runs Reliquary's own code, and HIRAKU's script runner drives its inputs, so the oracle only has to confirm that the core matches it from power-on. It does, below.

**The core, `runtime/python/` (`core = "python"`, `hiraku-popn9`):**
- **Built by** `oracle/pcsx2/build-reliquary.sh` against Reliquary's `PCSX2` library. The binary sits next to Reliquary's resources and is linked into `build/cmake/`.
  - HIRAKU's shell sources are compiled there too, with `-fexceptions`: PCSX2 builds without exceptions.
- **`Host::`:** implemented after `pcsx2-gsrunner`. The GS runs surfaceless.
- **Pictures:** the GS thread hands each presented frame over (native size: 512×448 on pop'n 9). At vsync N the CPU thread waits for frame N−1, so pictures are deterministic and the GS still overlaps the EE.
- **Sound:** 48 kHz, gathered per vsync.
- **The run's files:** the BBSRAM, dongles, boot ROM and memory card are copied into the NVRAM folder (or a fresh folder per scripted run); the CHD gets Reliquary's write overlay there.
- **Exit:** the shell's `exit()` ends through glibc `on_exit` → `_exit`. PCSX2's static destructors assert while the VM is still mapped.
- **Settings stay out of `~/.config/PCSX2`.** `EmuFolders::SetDataDirectory()` would create it, so the core sets the folders itself.
- **`tools/ingest.py <set>.py1`:**
  - identifies the game by the SHA-1 of its decoded EE program (`games/popn9/game.toml [python] ee_sha1`);
  - copies the board's files, the BIOS and the keys into `dumps/popn9/`, and links the CHD.

**Results (pop'n 9):**
- **Matches the oracle:** EE and IOP RAM hashes identical to `pcsx2-qt`'s at all 50 checkpoints up to frame 3,000 (JITs, software GS).
- **Repeatable:** picture and sound digests identical over 6,000 frames.
- **Plays:** `oracle/scripts/popn9-play.his` goes from power-on through coins, NORMAL and song select to "Requiem", scored, and CLEARED. It is the regress case `popn9-play` (14,000 frames, ~850 MB peak).
- **Buttons checked in test mode:** I/O CHECK > INPUT CHECK lights SW1–SW9 for buttons 1–9; Service lights SERVICE. Coins count.
- **The window works:** the shell's OpenGL picture beside PCSX2's surfaceless Vulkan device. The Vulkan GS (`--renderer vk`) gives the same game, with the same scores.
- **Host cost** (headless, 100 s of game, this desktop, noisy):
  - software GS ~30 % of a core; Vulkan ~18–27 %;
  - the CPU thread's own work 1.9–2.3 ms a frame on average;
  - one 175 ms frame at boot (frame 326);
  - to be measured properly (A/B, median) and on N100-class hardware.

**PY-1b (2026-10-13), done but for the owner's test:**
- **Lamps:** the patch taps the interface board's JAMMA output command (`Hiraku::p1io_output`). Word 8 holds the lamps.
  - The bits were read off the game's LAMP CHECK, item by item: SW-LAMP 1–9 = bits 0–8, HI-LAMP 1–5 = bits 14–10, LEFT-LAMP 1–2 = 15–16, RIGHT-LAMP 1–2 = 17–18.
  - They form the cabinet family `pypopn`: pop'n's controls, Firebeat pop'n's lamp names. All 18 lamps light through the lights socket.
  - `HK_PY_P1IO_LOG=<file>` logs the command's payload changes.
- **The input poll:** the game sends that command, and gets the input report back, **exactly once per frame**. This is the fact PY-4 starts from.
- **Launcher:**
  - accepts a `.py1`;
  - the ingester also takes a set's folder (the `.py1` beside it);
  - Python games get `--renderer vk`, a `pypopn` family name and icon, and Python-specific install and build hints.
- **NVRAM:**
  - Reliquary rewrites the BBSRAM file in the NVRAM folder on every write; the core copies the dump's files there only when they're missing.
  - pop'n 9 writes nothing in normal play: bookkeeping is OFF by default, and a factory reset rewrites identical bytes.
  - A scripted change of a saved setting is still to do. Test-mode submenus miss 6-frame presses (10 work), and the hold-red-and-press-blue change didn't take in a script. The owner's hands-on test covers it meanwhile.
- **Packaging:**
  - Reliquary is built with `$ORIGIN/lib` in the run path and FFmpeg loaded at run time (the binaries no longer need it).
  - `tools/package.sh` adds `resources/`, the libraries built for Reliquary and the CHD reader. The packaged binary runs with the build's `deps/` moved away.
- **Reliquary pinned:** 1.9.4, the zip's SHA-256 and the update procedure are in `oracle/pcsx2/README.md`. The patch: 7 files, 190 lines.

**PY-3 and PY-4, first steps (2026-10-13).** Measured with `tools/ab.py`: alternating runs, 5 each, medians of total process CPU. Vulkan, 6,000 frames unless noted. Every change is bit-identical (`popn9-play`, and the new `popn9-play-vk` case) unless noted.

| Change | CPU | Notes |
|---|---|---|
| **Picture handoff:** `bswap32(p) >> 8` instead of per-pixel masks; buffers swapped, not copied | **−7.3 %** | `on_gs_frame`: 8.9 → 1.0 % of GS samples |
| **PGO** (`build-reliquary.sh pgo`, trained on `popn9-play`, both renderers) | **−3.2 %** (software GS −1.9 %) | far from the 573's 1.44×: the hot paths are JIT code and vectorised texture code; kept as an optional step |
| Texture preloading Partial instead of Full | **+11.4 %**: dropped | the picture is identical; None differs in 17 frames |
| **Each vsync's own picture** (`vsync_posted` hook; `--picture-overlap` for the old way) | +1.4 % (wall +9 % unthrottled) | **one frame less latency**; EE/IOP hashes identical in both modes; now the default |

- The Vulkan GS is deterministic on this GPU and driver: identical digests over 14,000 frames. That makes `popn9-play-vk` possible; its baseline must be recorded per PC.
- **Latency** (`oracle/scripts/popn9-latency.his`, `tools/pylatency.py`: test mode's INPUT CHECK, button 1 every 60 frames):
  - 4 frames from a press or release to the changed picture, every time; 5 before the picture change;
  - the game polls once per frame, and in windowed play the frame runs within ~2 ms of the shell reading the controls, so later input reading would gain little;
  - sound goes to the shell once per vsync, but a frame's emulation is a ~2 ms burst, so finer delivery would gain ~2 ms at most;
  - the 4 frames are pop'n 9's own pipeline.
- **Gameplay latency.** Runs are deterministic, so the method is two runs of the same song, identical but for one press (frame 6003); the first differing audio sample and picture are the press's effects.
  - **Picture:** 5 frames after the press.
  - **Keysound (button 5): ~2 ms after the vsync at which the press is applied.** The game polls just after the vsync and starts the voice at once. Button 1 made no sound difference at that point of the song.
  - What the host adds on top: up to a frame until the press is read (the game polls once a frame, as on the cabinet), the audio queue (8 ms by default), the display.
  - **Display sync off (2026-10-15):** the shell's audio wait came between a frame's ~2 ms burst and its picture (the core hands each vsync's sound on just before the frame hook), so a picture came ~17 ms after the controls were read. With `Core::audio_per_frame()` the window waits after the picture instead and holds each picture to the p99 of recent frames' work: 17.3 → 5.4 ms (p50) from the frame's start to the picture, the same cadence (`runtime/host/window.cpp`). With display sync it was already a burst: gameplay frames p50 1.9 ms, p99 4.3 (software GS; Vulkan 3.0 / 4.4, its readback makes the GS wait 1.5 ms against 0.9).
- **Against stock Reliquary (`pcsx2-qt`, same build, same settings, same pop'n 9 attract from power-on), 2026-10-13:**

  | | pcsx2-qt | hiraku-popn9 | |
  |---|---|---|---|
  | Playing at real time, windowed, CPU from 30 s to 150 s, Vulkan | 34 % of a core | 20 % | **−40 %** |
  | The same, software GS | 44 % | 29 % | **−34 %** |
  | Flat out, CPU for 6,000 frames, Vulkan (pcsx2-qt still presenting; the core headless) | 16.8 s | 16.3 s | −3 % |
  | The same, software GS | 22.9 s | 23.0 s | 0 % |

  - Medians of 2 real-time and 3 flat-out runs, alternated. Audio was null in both, and the display ran at 144 Hz, so the core's display sync was off.
  - **The emulation costs the same:** it is the same code. The saving appears only at real-time speed, so it is in how each paces and presents a frame. The split between pcsx2-qt's frame limiter, its presentation and its Qt UI is **not measured**.
- **Unlocks (§20's system), 2026-10-13:** pop'n 9's event phase, set from the launcher's Unlocks dialog or `tools/unlock.py popn9 phase=0..4`.
  - **How it was found:** the four RemyWiki codes typed in test mode > EVENT OPTIONS > PASSWORD (`oracle/scripts/popn9-phases.his`), the saves compared, then the program read.
  - **Where it lives:** the interface board's BBSRAM (`m48t58y.u48`), a small file system (`bbsram0:/settings`, `/book`) written by Konami's `mirror.c`:
    - each file is kept twice (settings0 at 0x0, settings1 at 0x110);
    - each copy is a 12-byte header {sum, size, data sum} with the data at +0x10. The sums are of 32-bit LE words, each XORed with its index (`0x1ab6f8`);
    - the 0x60-byte settings record (RAM `0x1212180`) ends with its own sum at +0x5c (`0x1a9b28`): five segments, each byte XORed with its index in the segment, plus 0x50. A record with a wrong sum is ignored at boot (the phase reads `--`);
    - the phase is record +0x14, 0–4 (`0x1aa4f0` reads it).
  - **Checked:**
    - every phase written by the tool, from phase 0 or from MAX, is byte-identical to the game's own save at that phase;
    - a tool-made MAX save boots at MAX (EVENT OPTIONS), and the mode select then has HITASURA after EXPERT.
  - **Not checked:** what phases 1–3 do without e-AMUSEMENT (the Gambler Z labyrinth needed a card).
  - `tools/unlock.py` gained checksum kind `mirror` (with an optional `inner` record sum) for it. Code macros (`macros.txt`) already run in the Python core, which shares `runtime/main.cpp`; pop'n 9 has no per-credit codes known yet that would need one.
- **e-AMUSEMENT, 2026-10-13 (owner: "start work on eamusement"):** pop'n 9 is the first pop'n on it. Read with Ghidra 12.1.2 headless (`MIPS:LE:64:64-32addr`, raw binary at 0x100000, `$gp` = 0x25da70, functions seeded at every `addiu sp,sp,-n`; scripts in `$HK_PYTHON_WORK/ghidra/`).
  - **What the game has:**
    - an e-AMUSE3 client: DHCP and sockets on the interface board (the board has the Ethernet port, and the game sends it socket commands);
    - HTTP/1.0 to a host named `services` (User-Agent `EAMUSE.Httpac/1.0`, an `X-Eamuse-Info` header, XML bodies), with services `services.get`, `numbering.assign`, `package.list`/`intend`, user data, rankings, KONAMI ID, p-friend, the coin count, NTP and ee'MALL;
    - a motorized magnetic card reader/writer and a 10-key, on the board's serial port.
  - **The card and the server:** the card holds only an ID and a type. Its first 4 bytes plus a CRC-8 give the type (`0x1a28c0`): `28 23 41 01` a new pop'n 9 card, `68 23 41 01` a used one, and others for Konami common, location-test and "death" cards. The ID is 8 bytes, written as 16 hex digits. A new card gets its ID from the server (`numbering`), and is then formatted, written and read back. A used card's player data is read from the server and written back to it. So a card does nothing without a server.
  - **Step 1, the card unit (done):** `runtime/python/cardunit.{h,cpp}`, behind a new hook in the Reliquary patch (`Hiraku::uart_write`/`uart_reply`, installed for family `pypopn` only).
    - The bus (driver `0x1a1050`–`0x1a6c00`): 9600 baud, half-duplex. The game reads its own packet back before each answer, which is what Reliquary's stock answer lacked (CARD UNIT ERROR).
    - Packets are `aa T ADDR CMD LEN [payload] SUM`. Bus commands: 01 address, 02 revision, 03 start. Card commands: 10 reset, 12 status (0x80 card in, 0x20 read done, 0x10 buffer loaded), 14 control (1 open, 2 eject, 4 read, 5 write), 16/18 write/read the 128-byte buffer, 1e format. 10-key: 20 init, 24 status, 26 events (PS/2 set-2 scan codes).
    - Reliquary delivers 38 received bytes per transfer, so with the hook it now drains longer answers (the card read is 141 bytes).
    - **Checked:** with e-AMUSEMENT ON, the boot passes CARD R/W CHECK (test mode shows the unit as VER 1.0.0) and 10 KEY CHECK. Test mode's CARD CHECK reads the card: "CHECK IS COMPLETED. NEW CARD" (`oracle/scripts/popn9-eamuse-on.his`, then `popn9-cardcheck.his`). popn9-play and popn9-play-vk are unchanged. With e-AMUSEMENT OFF the game sends the unit no commands (4,000 frames from power-on, `HK_PY_CARD_LOG`).
    - **Not checked yet:** writing a card (only a new card's creation writes, after the server's numbering), and the 10-key in play.
  - **Step 2, the network (done):** `runtime/python/eanet.{h,cpp}`, a virtual LAN with no host sockets, behind a second hook (`Hiraku::net_command`, with `net_respond`/`net_write`/`iop_read`).
    - **The commands:** the board command is the game's RPC number + 1 (EE wrappers `0x1b3648`–`0x1b6ae8`): 0b socket, 0c bind, 0d listen, 0e accept, 0f connect, 13 setsockopt, 14 send, 15 sendto, 16 recv, 17 recvfrom, 1c shutdown, 1d close, 1e a property (0x101 IP, 1 mask, 3 gateway, 6 DNS, 0xf the local domain `PYTHON1-00000001`).
    - Each answer is the channel's 8-word status block: word 1 the result (byte-swapped), words 2 and 3 the sender for recvfrom.
    - Start-up and DHCP (00–07, "bound") stay Reliquary's.
    - **The LAN:** the board at 192.168.50.2/24; gateway, DNS and `services` at 192.168.50.1. DNS answers `services.*`, and TCP port 80 there reaches an HTTP/1.0 handler in the core.
    - **The game's start-up** (state machine `0x182c58`): net init, DHCP, the addresses, then e-AMUSE3 starts (`0x18ec30`: DNS, domain, port 0x7e90). It resolves `services.PYTHON1-00000001`, connects to port 80 and POSTs `/` with `X-Eamuse-Info: 1-xxxxxxxx-xxxx` and an RC4-encrypted body.
    - **The crypto is the modern e-AMUSEMENT one** (as in bemaniutils): key = MD5(the header's 6 bytes + the 26-byte shared secret), RC4, plain XML with no compression. The first request decrypts to `<call srcid="01401401000100045FEF" model="C00:J:A:A"><services method="get"/></call>`.
    - **Checked:** popn9-play(-vk) unchanged. With e-AMUSEMENT OFF the game sends the board no socket commands (4,000 frames, `HK_PY_NET_LOG`).
  - **Step 3, the server (in progress):** `runtime/python/easerver.{h,cpp}`: the protocol's MD5/RC4, a small XML reader, and answers built from the game's response parsers. State is kept in the save folder's `eamuse/`.
    - **Modules and methods** (the method entries at `0x1e81e8`…, each {name, `services` item, -, handlers: init, build, parse, end}):
      - services get;
      - posevent income/sales/sale (sale and sales need `approve`);
      - pcbtracker alive;
      - numbering assign (`number`);
      - package list/intend;
      - userdata read (109 = not registered, 110 = expired; `time`, `<b>`) and write;
      - userid ctou (`user`, `active`), adduser (`alter`), addcard;
      - rank get/register (+ `_h` HITASURA, `_gz` Gambler Z);
      - p_friend get/state/ask;
      - eemall i_get;
      - ntp get (`<time year month day hour min sec>`).
      - `services` must list the items `posevent`, `pcbtracker`, `numbering`, `package`, `userdata`, `userid` and **`local`** (rank, p_friend, eemall and ntp look up `local`).
    - **The envelope:** `<response><MODULE status="0" …>…</MODULE>…</response>`, one element per call, in order. `fault` or a non-zero `status` fails the call.
    - **Encryption follows the call's packet type.** Some calls go out unencrypted (numbering, ntp), and their answer must also be unencrypted, **without** `X-Eamuse-Info` (`0x190290` compares the header's crypto version with the packet type's). Answering them encrypted failed them before any XML was read. That was found with a temporary EE-interpreter PC trace, kept outside the patch (`$HK_PYTHON_WORK/scratch/Interpreter.cpp.eetrace`, `HK_EE_TRACE=pc,…`, run with `--interp`).
    - **The start-up** (`0x182c58`): services.get → pcbtracker.alive → posevent → ntp.get → status 0. The title then says "e-AMUSEMENT 接続中!" and "カードが使用できます!!". A ping check (raw ICMP, `0x1885b8`) runs every so often; the LAN answers the echo with an IP header.
    - **Checked, a new card** (`oracle/scripts`-style runs from the e-AMUSEMENT-on save):
      - credits and Start, then the INFORMATION screen asks for the pop'n card;
      - Insert: the reader takes it and reads "NEW CARD"; then name entry, then the PIN twice on the 10-key;
      - `numbering.assign` gets `2003010000000010` (16 digits, Luhn check);
      - the game formats, writes and reads back the card: `68 23 41 01` + CRC-8, the ID at 6–13 (byte-reversed), CRC-16 at 0x3f;
      - "カードセーブ完了", then `userid.ctou` (no KONAMI ID), and mode select with the card.
    - **Checked, a whole credit and the card again** (`oracle/scripts/popn9-card-new.his`, then `popn9-card-again.his`, same save):
      - at the credit's end the game sends `p_friend.state` and `userdata.write card=… time="2003-06-02 06:42:41+9"`, with the player data base64 in `<b>` (chunks of 0x200 bytes, `0x197fc0`); the server keeps the time and the `<b>` chunks;
      - next credit: `userdata.read` gets them back, the game skips the name and asks the PIN;
      - 1234 shows the player's name ("あああ"), character and options, then `p_friend.get` and `userid.ctou`, and mode select with the card;
      - a wrong PIN gets "パスワードが違います" and, after the retries, play without the card;
      - the PIN is checked by the game, from the player data.
    - **Rankings, KONAMI ID, ee'MALL** (2026-10-13), from the parsers (`0x19a080`…, `0x1992e8`…, `0x198858`…, `0x19b558`) and the status mappings (`0x184f68`, `0x1863e8`, `0x186620`, `0x186818`):
      - **Boards:** score per course, HITASURA per course, and Gambler Z per shop. Each entrant's best is kept; the board is sorted; `register` answers with the place, the number of entries and `update`.
      - **userid:** ctou 112 means no KONAMI ID; adduser 113/114; addcard 115/116/114.
      - **ee'MALL:** items per card, from `eemall-CARD`.
      - **Checked in play** (`oracle/scripts/popn9-card-expert.his`): an EXPERT course with the card, failed in its first song, still sends `rank.register`. The name is hex Shift-JIS, `area_num` 255. The game then shows INTERNET RANKING, "あああ さんの New ハイスコア 918 は 4位 4人中です", from the server's answer against three seeded entries.
    - **Online** (`--eamuse http://host[:port][/path]`, `eaclient.cpp`): the core forwards each request to the server. Answers arrive on a worker thread, and the game's recv waits for them (the board's status block is written when the answer is there). DNS gives every name an address of its own on the virtual LAN, so the URLs the server hands out reach their hosts.
      - **`hiraku-eamuse`** (`tools/eamuse/server.cpp`): the same server behind an HTTP/1.0 listener.
      - **Checked:** a card registered through it on 127.0.0.1; offline unchanged.
    - **For server owners:**
      - `docs/EAMUSE3.md`: transport, the crypto and its in-kind rule, the envelope, every module with its statuses, the card, a card's life.
      - `tools/eamuse/corpus.json`: the game's calls, captured or built from its formats, flagged.
      - `tools/eamuse/check.py URL`: the corpus checked against the parsers' rules, then a stateful scenario. hiraku-eamuse passes all of it.
    - **Launcher:** Settings > e-AMUSEMENT > Server (empty = offline) passes `--eamuse`.
    - **Phases MAX and 1–3 with the card:**
      - start-up sends `rank.get_h` (MAX) and `rank.get_gz` (1–3);
      - a Gambler Z credit (phase 1) ends with `register_gz prog="0" parts="00000000"`, and attract then shows "Gambler Z in progress" pages;
      - the labyrinth is the cabinet's (settings record 0x18–0x23; `0x1aa7f0`…), and the credit set +0x1c to 0xffff;
      - a HITASURA run with no song cleared sends no `register_h`.
    - **`--autoplay`** (the owner's request, for testing): the game's own demo play in a real credit. `0x257084` is pop'n 9's autoplay flag: each song's start sets it to "mode (`0x25a0d8`) == 2, the attract demo" (`0x10e570`); while it is set, each note reaching the line is judged GREAT (`0x10cbe8`). The play screen's button reader (`0x10dd80`) gives nothing in the demo by its own mode test. The mod patches both tests (`0x10e644` bnel → nop, `0x10dd9c` beq → b) through PCSX2's TLB at a vsync, as its own patches are written, so the recompiler drops the blocks.
      - **Checked:** "Requiem" 201 GREAT, combo 200, 100000, CLEARED; a whole NORMAL credit, ALL STAGE CLEARED. Without the flag, `popn9-play` and `popn9-play-vk` are identical.
      - **HITASURA** (`oracle/scripts/popn9-hitasura.his`, phase MAX): the first course's three songs cleared, life ×50 left; `rank.register_h course="0" music="3" combo="365"` (now captured in the corpus).
      - **Gambler Z** (`oracle/scripts/popn9-gamblerz.his`, phase 1): after the results and the staff roll, the labyrinth (`0x19e188`, phases 1 and 2 by `0x143c98`): "1階 Aフロア", 3 steps, one per cleared stage (`0x1088c8`, two for a stage whose song counts double). Settings +0x1c went -1 → -4. `register_gz` still sent `prog="0" parts="00000000"`.
      - **The labyrinth, read from the program** (docs/EAMUSE3.md): cells hold direction bits; a junction is the player's choice (buttons 4/6, red, a 180-frame timer); treasure cells give items (flags +0x18, low 24 bits); stairs change floor (+0x20); from floor index 10, door cells start a boss song whose score deals 1–5 damage (70k/80k/90k/95k) to 25 HP, 50 on floor index 15 (+0x22). The record also holds the shop's area (+0x35) and name (+0x37) that every `rank.register*` sends.
    - **The clock:** `ntp.get` is set into the cabinet's clock as given (`0x18a138` → `0x1a9598`), and the game stamps its times "+9". `hiraku-eamuse` sends Japan time (`--tz`); the core sends its emulated clock.
    - **KONAMI ID on the cabinet** (done): at the end of a credit with a card that has none (`ctou` 112), INFORMATION "KONAMI IDのご案内" (`0x1a8678`): 新規発行 (the ID on a character wheel, 8–32 of `a-z0-9.-_`; a 4-digit password twice on the 10-key) sends `userid.adduser`; カードへ登録 (ID and password once) sends `userid.addcard`. Checked with `oracle/scripts/popn9-konamiid-new.his` (`hiraku09` made) and `popn9-konamiid-addcard.his` (a second, fresh card added): the server's `users` lists both cards and `ctou` gives the ID for each. The game sends no `aflag` (only when nonzero); both calls are now captured in the corpus (with test card numbers, so replaying it doesn't take numbers a server hands out).
    - **Gambler Z on the server** (done): `register_gz` sends `parts` = the labyrinth's items word (`%08x`) and `prog` = its bits set (`0x188248`); a save with two songs and the first boss sent `prog="4" parts="00084003"`. The bits, read from the item list of a save with all of them: 0–13 a song per floor's treasure, 14+k/19+k a boss's EX chart and EXPERT course, 24 cleared. `tools/unlock.py popn9` decodes them (`[[progress]] kind = "gamblerz"`), so the launcher's Unlocks dialog shows them.
    - **Ranking pages** (done): attract mode shows "popper's ranking" per course after each demo song, and in phases 1–2 the Gambler Z shop ranking after the labyrinth, both from the server's boards, while the network check passed (`0x2571f8`). `area` is 0-based JIS order (0 北海道, 12 東京都, 46 沖縄県, 255 none); `shop_name` hex Shift-JIS, from test mode's SHOP NAME SETTING (`oracle/scripts/popn9-shopname.his`: "ひらく" went out as `82D082E782AD`).
    - **pop'n friends** (done): `get` while the player data's friend flag is 0, `ask` once it's set, `state` at the credit's end; all three seen. 600 (get) and 601 (ask) are final noes. **ee'MALL:** `eemall.i_get` is unreachable in pop'n 9 (its wrapper `0x186ef8` has no caller).
    - **A boss fought in play** (done): saves edited to floor index 10 with a fight under way (HP at settings +0x22), then the autoplayed Gambler Z credit: the boss's EX STAGE (テクノポップ "321 STARS" EX) at 100,000 deals 5; HP 25 -> 20 saved, and from 5 the boss falls: "隠しコース 出現！ すわコース", bits 0x84000, back to 1階 Aフロア, and the same credit's `register_gz` sends `prog="2" parts="00084000"`. `register_gz` comes after the labyrinth (the docs had it before).
    - **Phase 3** (done): the event's end. `get_gz` at start-up; after a credit no labyrinth and no `register_gz`; attract shows the shop ranking after its demos in place of the course pages, and no labyrinth (an autoplayed credit and 12,000 frames of attract from a save with items).
    - **package: the title message** (done): after `package.list`, the game looks for an item named `message`; if offered it sends `package.intend url=..` and GETs the url (plain HTTP), keeping `size`-1 bytes, and scrolls the text across its title screen (`0x184410`, `0x1237c0`). The server offers the owner's `message` file (≤255 bytes, one line, ASCII or Shift-JIS) and answers the GET; checked offline and online through hiraku-eamuse, ASCII and kanji. All 23 calls the game makes are now captured in the corpus (eemall.i_get is never sent).
    - **bemaniutils** (done, out of tree): `tools/eamuse/bemaniutils/popn9-python.patch` (base 1e66fcd) adds pop'n 9 to a bemaniutils server: a backend for all its calls (`bemani/backend/popn/popn9.py`, game code `C00`), several requests per call in the dispatcher, and answers in kind in the XML layer (no declaration when the request has none: pop'n 9 rejects `encoding=`; untyped element text kept for `userdata.write`). Written with an AI model, so not for upstream: bemaniutils has refused AI contributions since 1e66fcd (README); the patch and its README say so. Checked with MariaDB 11 in Docker and HIRAKU online: check.py all pass; a new card numbered, written, saved and read back with its PIN; a KONAMI ID made on the cabinet (113 for one taken); course, HITASURA and Gambler Z registrations and boards. bemaniutils' own tests pass (71; 5 need packages not installed). Not supported: the title message (its GET is the health check), frontend pages.
    - **Found on the way:** with a KONAMI ID on the card, INTERNET RANKING shows "? 位 ? 人中" (both servers; cause not found). The XML declaration with `encoding=` is refused by the game (without one, or `<?xml version="1.0"?>`, is fine).
    - **Not done yet:** nothing known on the server side; the meanings of a few statuses (docs/EAMUSE3.md, "Not known yet").
    - **Seen once:** one `popn9-play` check differed right after a rebuild, then was identical three times running. `popn11-play` did the same once (2026-10-14, in a full regress right after a rebuild), then was identical twice. On 2026-10-15, with the disk saturated by another session's CHD extraction (load average ~20, a 64-minute regress), five of them differed at once (`popn9-play-vk`, `popn10`–`12-play`, `popn14-play`, from frames 693–813); `popn12-play` and `popn14-play` were identical when rerun alone. So under heavy I/O contention the Python core is not deterministic: something in PCSX2's threads or the disk reads depends on host timing. Where they differed was not captured; watch for it.
  - **Debugging aids:** `HK_PY_WATCH=addr,…` prints EE RAM words when they change. Ghidra headless decompiles (`$HK_PYTHON_WORK/ghidra`: `Decomp.java OUT addr|range:A-B…`, `Refs.java addr…`).
- **Tools:**
  - `tools/ab.py`: A/B of two binaries, or of two settings with `--a-args`/`--b-args`;
  - `--pcsx2-set Section/Key=value`: any emulator setting, for experiments;
  - `--no-picture`, `HK_PY_PROFILE`.

**Next:**
- the owner's test;
- PY-2 on low-end hardware, which decides whether PY-3 goes into PCSX2's GS texture path or SPU2 (both mean patching emulator logic, not just taps);
- hands-on latency with real controls and audio (the owner). The profile behind it was measured with the core's `HK_PY_PROFILE=1` sampler and its `--no-picture` option (both added after PY-1).

### pop'n music 10–14 (2026-10-13)

The owner's media arrived (Reliquary `.py1` sets beside popn9). Survey, then support, the same day:
- **The board is the same** [V]: boot ROM, config ROM, internal dongle and memory-card ID byte-identical to pop'n 9's; one `kn00002.ps2` for 10–14; the same 20 IOPRP modules. New: an external "black" DS2430 per game (`GameId` GEC20JAA, GCD11JAA, GCE12JAB, GCE13JAE, GNF14JAB), which ingest and the core already passed on. Details: docs/PYTHON.md §3–4.
- **Two media quirks:**
  - popn11's sector 0 is a blank PC MBR. `tools/pythonfs.py header()` takes the first of 0, 0x200, 0x400 that starts with the header's signature; copies can differ from sector 0 on intact disks (dogstdx, dstagef, wswe2k3), so a copy is only a fallback.
  - popn13/14's record C is a loader for `ee/boot/game.bin` on a disk file system. Identification by record C's SHA-1 still works (each loader is distinct).
- **Added:** `games/popn10..14/game.toml` (no MAME sets exist), `hiraku_python_game(popn10..14)`.
- **Matches the oracle** [V]: EE/IOP RAM hashes identical to `pcsx2-qt` at all 50 checkpoints to frame 3,000 for every game. pop'n 13 only with the new `--bare-board`: it probes the card unit at frame 1,325, and HIRAKU's card unit answers where plain Reliquary has none (each side repeats itself exactly; found with `HK_PY_CARD_LOG`).
- **`OPHFlagHack`:** Reliquary's game list sets it for every Python 1 game but Perfect Pool; the core didn't. It now does. pop'n 9's play case is identical either way.
- **Plays** [V]: `oracle/scripts/popn10..14-play.his` (coins, mode, button select, a song with a button every 10 frames; pop'n 10 clears its first stage), regress cases `popn10-play` … `popn14-play`, recorded and repeating. Menus differ: 10/11 as 9 plus a BUTTON SELECT; 12–14 ENJOY with 5 buttons preselected; 13's mode select settles later. Menu presses need 10 frames (4 don't pick).
- **Lamps and inputs** [V]: each game's LAMP CHECK steps through the same word-8 bits in the same order as pop'n 9 (`pypopn` holds); pop'n 14's INPUT CHECK lights SW1–9 and SERVICE for buttons 1–9 and service.
- **Host cost:** like pop'n 9, ~2.4 ms of CPU-thread work per frame on average (software GS, headless).
- **pop'n 9 only, so far:** `--autoplay` (it now says so for other games), unlocks, and the e-AMUSEMENT server's calls. From the programs' strings: 10/11 add `game` and drop `parts`; 12 adds `lobby` and `playerdata`; 13 `cardmng`; 14 `facility`.
- **One card for all, pop'n 9–14 (owner asked 2026-10-15; stage 1 done)** [V]: one card number in every game; the default saves share one e-AMUSEMENT folder (a game's old one merged in once); a game shown the previous game's card for a number it has never seen, a fresh card of its own after the player agrees; the server answers each carry-over read (pop'n 10/11 `userdata.read NUMBER:MODEL`, 12-14 `playerdata get model=`) and builds the new record on `conv` as the game does (checked against the game's own conversion in RAM). Checked with one number from pop'n 9 through 14. Regress cases unchanged (they use `--nvram` elsewhere). Stage 2 (owner: "continue"): **clear marks shared (done)** [V]: all six games number songs alike (their song tables); every game, reading its own player, gets each chart's best mark over the card's players in all games, in its own format (9: cleared bits; 10/11: last two charts swapped); pop'n 14's carry-over now answered in 14's layout (its byte copy of 13's record had shifted songs by one). **Best scores shared, pop'n 12-14 (done, 2026-10-16)** [V]: the same sheet is the same chart in the three (13 and 14's 5-button mode plays their own charts, sheet 6); pop'n 9-11 keep no scores. docs/EAMUSE3.md, "One card for all" and "Shared scores".
- **NET TAISEN, pop'n 12–14 (owner asked 2026-10-15):** battles between cabinets, three players to a match, through a lobby (`globby://`, UDP) or on one shop's LAN (broadcasts), the cabinets then talking to each other over UDP. The three games share one library. Protocol: docs/EAMUSE3.md, "NET TAISEN".
  1. **One cabinet (done, 2026-10-15)** [V]: `runtime/python/lobby.{h,cpp}`, the lobby (entry, list, leave, kept per room for the entry's seconds), on HIRAKU's LAN at 192.168.50.1:5740. The virtual LAN answers datagrams to it, keeps each datagram's sender, and an empty `recvfrom` on a non-DNS UDP socket now times out (-1), as the game's sockets expect (it drains a queue by reading until one does). The server lists `portfw` and `globby`, answers `portfw get` with the board's address and port 5700, and gives pop'n 12 `play_enable` in `game get`. NET対戦 is on the mode wheel; the credit runs ROOM SELECT, song select while the lobby looks, then "nobody came" and CHALLENGE mode (`oracle/scripts/popn12-nettaisen.his`). The 7 Python regress cases identical; the card credits of pop'n 9, 11, 13 and 14 as before.
  2. **Two cabinets on one LAN (done, 2026-10-15)** [V]: `runtime/python/lancable.{h,cpp}`, a star of HIRAKU processes (`--lan-host SOCKET`, `--lan-join SOCKET`; a Unix socket or TCP). Each cabinet has its own address (192.168.50.2 + its number); the virtual LAN tracks bound ports, puts datagrams for other cabinets, broadcasts and (on the others) the lobby's on the cable, and delivers what comes in to the socket bound to its port. At each frame's end every side hands over its datagrams and takes the others' (the host gathers and sends everyone all), so the cabinets run in lockstep and a battle replays exactly: two runs, started in either order at different priorities, gave identical digests on both cabinets. The two meet through the first cabinet's lobby (A waits, B joins), battle with a CPU in the third seat, and reach a RESULT with all three scores. Regress: `popn12-eamuse-on` → `popn12-phases` (MAX) → the pair `popn12-nettaisen-a`/`-b`.
  3. **Three and four (done, 2026-10-15)** [V]: three processes meet in one match (no CPU), the three RESULTs agree; with four, A, B and C play and D, finding the lobby empty (a full match leaves it), waits alone and goes to CHALLENGE mode, the game's rule. Each run twice (the threes side by side): every cabinet's digests identical. Found on the way: a datagram recv now waits for its socket's timeout (option 0x200; in whole frames, a shorter one times out at once), as the board's; answered at once, a guest's lobby requests went out two to five times before their answers came a frame later. Re-recorded `popn12-phases` and the pair for it (sound from frame 1,077, when the sockets start waiting; `popn12-phases`' pictures unchanged).
  4. **Online (done, 2026-10-15)** [V]: `runtime/python/netrelay.{h,cpp}`. `hiraku-eamuse` runs the lobby and a relay on TCP 5740 (the globby URL's port; `--no-relay` off); a cabinet with `-url` connects there, gets an address (10.57.0.N) that the core writes into its `portfw` answer, and sends its game's datagrams over the connection; the relay answers lobby datagrams and passes the rest to the cabinet with that address. Cabinets connect out (no NAT to cross). Not in lockstep, as real cabinets. Checked: two headless cabinets with `-url` to a local `hiraku-eamuse`, paced with the new `HK_PY_PACE=4` (frames at 4 × 59.94 Hz), matched, battled with a CPU, same RESULT, stage 2 together; without the relay, a message and nobody found. The Python regress cases identical.
  5. **pop'n 13 and 14 (done, 2026-10-15)** [V]: no `play_enable` and no event phase there: `game get`'s `net_event` 2 opens the mode (1 is the INTERNET RANKING; the games run one or the other, so pop'n 14 loses its ranking by default, the owner's choice; the data folder's `net_event-E13`/`-F14` switches). pop'n 14 reads its address from `facility get`, which the core now corrects as `portfw` (each cabinet its own on a LAN cable, the relay's online). Both play alone (CHALLENGE after the search) and two on a LAN cable (a CPU in the third seat, the same RESULT on both); pop'n 13 asks 全国対戦 or 店内対戦 first when it has heard another cabinet. Regress: `popn13-eamuse-on` → the pair `popn13-nettaisen-a`/`-b`; `popn14-eamuse-on` → `popn14-nettaisen-a`/`-b` (fixed `-card0`). The `<netvs>` fields stay for step 6.
  6. **Results (done, 2026-10-19)** [V]: `playerdata set`'s `<netvs>` goes back into the player's record for pop'n 12, 13 and 14, each field where the game's loader reads it from (docs/EAMUSE3.md has the three layouts, read from the loaders and the set builders). Checked through `hiraku-eamuse` (every field of all three, round trip) and the 13 pop'n 12–14 regress cases identical.

### Konami Python 2: DrumMania V and V2 (PY-6 step 1, 2026-10-16)

The owner's media: DrumMania V (E02, 2005) and V2 (F02, revision 2005-11-28) as PS2 hard-disk images (40 GB, the second community-prepared: its boot program asks `download.eamuse.konami.fun`), with their dongles, HDD ID and i.LINK ID; and a PS2 BIOS collection holding MAME's `ps2-0190j-20030822.bin` (SCPH-50000 v1.90). GuitarFreaks V2 is still downloading. Both games boot to attract in `pcsx2-qt` and in the core [V].
- **Ingest:** `tools/ingest.py SET.py2` (Reliquary's descriptor). A Python 2 disk is an ordinary PS2 disk with encrypted programs, so the game is told by its last Konami game partition (`PC.KNAC_00001.00672.<code>`; an update keeps the older game's): `games/drmnvj` (E02-JA), `games/drmnv2j` (F02-JA). The retail BIOS, the four MagicGate key files.
- **What a cabinet needs that the dumps lack:** the console's NVRAM (without one, the BIOS stops at its first-time "Select language"): Reliquary's generated NVRAM (it holds the i.LINK ID) with the OSD blocks (0x200-0x240, 0x270-0x2f0) of a set-up Japanese console from the BIOS collection; and a network: the boot program waits for one ("ネットワークを初期化しています") before anything else.
- **The core** (`board=python2` in machine.txt): writes the `.py2`, the NVRAM a run's own copy, the disk's writes in the run's overlay; the inputs to Reliquary's P2IO on USB port 1 (the `drmn` cabinet family's names); no memory card.
- **Its own network, in step with the machine** (`runtime/python/ethnet.{h,cpp}`, oracle patch `Hiraku::eth_send/eth_poll`): the PS2's adapter's frames go to the core and its answers come back from SMAP's update on the CPU thread (no host adapter, no receive thread). DHCP (the cabinet 10.50.0.2, see below), ARP, ping, DNS, SNTP, TCP. The game looks for its GuitarFreaks over UDP broadcasts (port 21030), gives up and goes to attract.
- **Repeatable:** the hard disk's writes went through PCSX2's I/O thread, whose pace decided when flushes completed: two runs drifted from frame 866. `Hiraku::sync_disk` writes them on the CPU thread; two runs side by side then gave identical digests over 9,000 frames. Regress: `drmnvj-attract` (13,200 frames), `drmnv2j-attract` (14,400). 2.7 ms of host work per frame (software GS).
- **Step 2 (2026-10-16): controls, lamps, e-AMUSEMENT** [V]:
  - **Inputs:** test mode's INPUT CHECK lights each pad, the pedal, the select buttons, coin, test and service alone. A credit plays (coins, start, mode, song: `あこがれ` then HIT CHART's rank 1).
  - **Lamps:** a new hook `Hiraku::p2io_lamp(bank, value)` (the P2IO's LAMP_OUT, its four banks). From LAMP CHECK item by item: bank 3 bits 0-4 the pads (lit by a 0), 6 START and 7 SELECT; bank 2 bit 0 SPOT (0), 1 WOOFER, 2 SPEAKER (the `drmn` family's Neon Top). Published as the family's lamps (`--lights-socket`, checked).
  - **The network's DNS answers had gone to port 53**, not the asker's: the boot program had been timing out, not finding "no such name". Fixed, and the boot program's own start-up, AVS services (`avs-services.c`: a broadcast `SgetRQ` + MAC on UDP 42197, then `GET <MAC>` to the server it names), now answered (an empty list): the game starts in half the time (frame ~3,400, not ~7,000).
  - **e-AMUSEMENT** (on in NETWORK OPTIONS; hold Start + Select to change a value; saved on the disk, then a reboot): `services.eamuse.konami.fun` is the core's server (`http`, TCP port 80); services, pcbtracker, message, posevent, shopinfo regist, demodata get. Three things it wants that a LAN of one doesn't give:
    - the library's keepalive (`ea3-keepalive.c`) pings a router, a center and a server, from the services' `keepalive` item (`...?pa=&ia=&ga=&ma=&t1=&t2=`), and `ntp`: both added for E02/F02/E03/F03, answered at the server's address;
    - **a cabinet whose address is 192.168.x.x has "no router"** ("ルーターと通信できません" 5-2000-0000: eamusement's check at 0x947a64, `(own address & 0xffff) == 0xa8c0`): so the Python 2 LAN is 10.50.0.0/24;
    - **the card unit** ("カードユニットが見つかりません" without it): not an ICCA as Reliquary has it but the 573 GuitarFreaks/DrumMania's EXTIO card unit (magnetic card reader/writer and 10-key; the game's ExtIOBoardInit, MagCardAcs, KeyboardAcs) on the P2IO's serial port 0: `runtime/python/gfdmcard.{h,cpp}`, the 573 unit's protocol byte-wise (a new hook pair `Hiraku::p2io_sci_write/read` and `p2io_sci_ports`). The reset step repeats its sync for 8 s by design; then the second sync, the node, its version, init, the 10-key's polls.
  - Then attract with the e-AMUSEMENT logo. Repeatable: two runs with e-AMUSEMENT on, identical digests over 12,000 frames. Regress: `drmnvj-eamuse-on` (test mode) → `drmnvj-eamuse`; `drmnvj-attract`, `drmnv2j-attract` re-recorded (the faster boot).
  - **The regress cases draw with one software rasterizer thread** (`--pcsx2-set EmuCore/GS/extrathreads=0`): with PCSX2's extra threads, two pixels of attract's anti-aliased "0/2" (and two frames at the BIOS's start) came out a shade different in some runs, the sound and the machine identical (a drawing race in the renderer, not the emulation). Not needed for playing; pop'n's cases never showed it.
- **Step 3 (2026-10-16): a card's life** [V]: a blank card gets a number, the server's cardutil check gives it a gdid (not registered), the player enters a name and a password, cardutil regist; the card is written; information and gametop; two songs; gameend regist keeps each song's best on the server; the card goes back. Again: check (registered), the password, two songs, the records add up. The calls and their answers (from the game's parsers): docs/EAMUSE3.md, "GuitarFreaks and DrumMania V". Regress `drmnvj-card-new` → `drmnvj-card-again` (after `drmnvj-eamuse-on`).
- **Next:** gametop's `musicdata` (the player's songs as the game keeps them, so it shows its records); GuitarFreaks (V2) and its session with DrumMania (two processes, as the 573's, over this network).

### Decisions (owner, 2026-10-13)

1. **Option 3:** host Reliquary's core in HIRAKU first (PY-1), then replace parts (the EE by static recompilation, the I/O by HIRAKU's cabinet layer) where a measurement says it pays.
2. **Reliquary is the reference** for Python, in place of MAME, whose Python drivers are skeletons. The oracle pins its CPU mode.
3. **The first set is `popn9`.** The owner gets the other games later for testing. (pop'n 10–14 arrived 2026-10-13; the owner asked for their support.)
- Still open: a clean `dstagef`; GuitarFreaks V2 and the other Python 2 games' media.
- Licence: linking Reliquary makes the Python binaries GPL-3.0 as a whole (its Python 2 files carry only the repo's GPL-3). HIRAKU's own code stays GPL-3.0-or-later.

## 24. The rest of the catalogue (written 2026-10-15)

The owner's MAME 0.243 BEMANI archive, what HIRAKU doesn't run yet, and a plan in seven phases: [docs/CATALOGUE.md](docs/CATALOGUE.md).
- **Phase 0 (done 2026-10-16):**
  - `tools/catalogue.py` and `tools/mameset.py`: sets told by contents;
  - ingest of older MAME names and split sets;
  - NVRAM seeds;
  - CHDs read in place (`runtime/dev/image.h`, `--keep-chd`).

## 25. Real cabinet hardware: card readers and the BIO2 (2026-10-16)

Konami's ACIO bus from the PC's side, after spice2x (`runtime/host/acio.h`), in commit d2dc121. The owner has an e-AMUSEMENT serial reader, an NFC or cardio reader, and (later) an IIDX BIO2 board. Everything below was tested only against pretend devices on pseudo-terminals (`tests/acio/`) and, for the readers, in pop'n 14 with a scripted reader; none of it has met the real hardware.

**Done:**
- Card readers (`runtime/host/readers.h`): `--reader PATH` (spice2x's serial dialogue: key exchange, encrypted status), `--scard` (PC/SC), `--cardio` (hidraw). A card held there is the player's card (`Core::card_read`): pop'n 9–14 (`make_card()`), GuitarFreaks and DrumMania's card units (`cards/NUMBER.bin`). The 10-key goes to the game, 00 as Enter where it has no 00.
- `--bio2 PATH` (`runtime/host/bio2.h`): the BIO2 with IIDX 25–26's BI2A firmware as the beatmania cabinet's controls and lamps, in spice2x's `games/iidx/bi2a.cpp` layout.
- Launcher: Settings > Card readers, Settings > Cabinet I/O. Docs: docs/PLAY.md, "Card readers" and "Cabinet I/O".

**Bring-up, when the hardware is at hand** (`HK_ACIO_LOG=1` prints every packet):
1. **BIO2** (the least certain): start IIDX on Twinkle with the board set in Settings > Cabinet I/O.
   - Does it answer the handshake, and at which rate (115200, 57600, 38400 or 19200 are tried)?
   - Does it say BI2A? A board flashed by IIDX 27 or later says something else, which means ACIO2 (below).
   - The start-up sent is IIDX 27's (docs/BIO2-BI2A.md, 2026-10-18): a 250 ms break and `aa` echoes at 115200, `0001 [00]`, `0002`, `0003`, `0100 [2d]` (IIDX's init byte; SDVX's is 3b), `0120 [00 00]` (the watchdog off), then `0152` polls (20 ms, three tries). IIDX 27 has no BIO2 firmware writer and never compares the version; never send `0004` (IIDX 28's BI2A firmware writer starts with it).
   - The turntables' direction (byte 16/17 deltas, × 4 to 1024 a turn), each key and button, the five sliders (high nibbles), the coin count (byte 8).
   - The lamps: keys, panel, spotlights 1–8, neon, and the ticker showing the 16-segment text.
2. **Serial reader:** pop'n 14 (IC card) and pop'n 12 or 13 (magnetic on the game's side), then two readers on GuitarFreaks. Check that the 10-key's keys are the right ones. spice2x's own 10-key path looked wrong, so HIRAKU maps the reader's bits as pop'n 14's reader emulation does (`python/icca.h`).
3. **NFC (PC/SC):** pcscd running, an e-AMUSEMENT PASS read as E004… in the same byte order as on the serial reader (ISO 15693 ids come backwards from PC/SC; `readers.cpp` reverses them by the ATR), and a FeliCa card given an E00401… number.
4. **cardio:** the udev rule, then the report layout: a report id, 1 for ISO 15693 and 2 for FeliCa, then 8 bytes; a descriptor without report ids is read from byte 0.
5. **GuitarFreaks and DrumMania card units in a running game:** needs `--npu` and the network unit's disk; that code path has run in no game yet.

**The owner's board (2026-10-18), found without opening it:** USB `1ccf:8050`, manufacturer "KAM", product "BI2X", serial 0000, full speed, CDC-ACM (control interface with an interrupt endpoint; data interface, bulk in/out, 64-byte packets) → `/dev/ttyACM0` (`/dev/serial/by-id/usb-KAM_BI2X_0000-if00`). ModemManager is inactive (it would probe a new ACM device with AT commands).
- `1ccf:8050` is the BIO2 board itself (spice2x: pop'n, GITADORA, BC all look for it); the product string follows the firmware flashed. "BI2X" means a BI2X-family firmware (IIDX 27 and later and other games), so the board speaks **ACIO2**, not the BI2A ACIO that `--bio2` and bemanitools' `iidxio-bio2`/`sdvxio-bio2` speak (bemanitools: "assumes that the BIO2 is already flashed to the correct firmware"; its IIDX 27–31 hooks emulate the BIO2 rather than drive a BI2X).
- **The bricking risk** is the firmware write: Konami's libaio carries firmware images and writes one when the board's doesn't match (`aioIob2Bi2x_CreateWriteFirmContext`, `WriteFirmGetState`; spice2x `games/qks/bi2x_hook.cpp`). An interrupted or wrong write can leave the board unusable. HIRAKU must never send firmware-write commands, nor run a game's own I/O library against the real board.
- ACIO2, from spice2x's `acio2emu/` (an emulator of the board, so it shows what games send): packets of node, tag, a variable-length payload size, payload flags (byte-stuffed, raw, replace, LZ; optionally obfuscated by an LCG) and a CRC. The BI2X node's commands: 2 (firmware version), 16, 19, 120, 800, 801, 802 (start-up, answered with fixed values), 784 (poll inputs), 785 (write outputs), 786. The meaning of the poll's input bytes for IIDX isn't in spice2x (its IIDX support hooks libaio).
- **First contact (2026-10-18, owner's go-ahead; `tools/bio2/bi2x_version.py`, two packets, read-only):** on USB power alone the board stayed silent; with its 12 V supply plugged in it answers. Host packets: `AA node tag size (flags<<4|crc4)` + payload + crc7, node = index × 2. Sent `AA 00 00 02 07 00 01 4A` (master: assign nodes) → `AA 00 00 03 4D 00 01 20 11` (payload `00 01 20`: one node, id 0x20); `AA 02 01 02 04 00 02 18` (node 1: version) → `AA 03 01 23 5C` + 35 scrambled bytes. The answer's header byte has bit 4 set (obfuscated); the payload descrambles with an LCG (1103515245, 12345, mod 2^32) seeded 43 (or ≡ 43 mod 128), each byte XORed with the state's low byte and kept to 7 bits: `00 02 00` + `0D 06 00 01 | 00 01 03 03 | "BI2X" | 01 01 19 06 | 00 00 00 33 | 00…` — spice2x's BC node answers `… 00 01 02 08 | "BI2X" | …`, so this board's BI2X firmware is version 1.3.3 (or so: bytes with bit 7 set may decode otherwise). Which game flashed it isn't in the answer. The board was unchanged afterwards (same USB device, no reset).
- **Next, with the owner's go-ahead:** either capture a real game talking to the board (a USB capture on the PC that runs it: no risk to the board), or a read-only probe from HIRAKU: open the port at 115200, send only command 2 (version) in an ACIO2 packet, read the answer, close. The capture also gives the input layout.

**From the games' own code (2026-10-18, static analysis; docs/BIO2-BI2A.md, docs/BIO2-BI2X.md):** HIRAKU's BI2A driver now does IIDX 27's start-up and timing, lamps as bit 7, a 6-bit coin count (the pretend-board test checks the start-up). BI2X: the IIDX I/O commands go to a code module the host uploads at every reset (16/19/120: allocate, write, start), and IIDX 28's module is built for firmware 1.2 while the owner's board runs 1.3.3 (IIDX 28 would refuse it). The module is Konami's code: HIRAKU can't carry it, only load it from the owner's own game data. Never send `00 7x` (in-band firmware writes); never open PID 8040 (the loader) or 804C as a BI2X.

**Next after that:**
- If the BIO2 runs BI2X firmware: ACIO2. spice2x's `acio2emu/` has the framing: a variable-length size, CRC4/CRC7, byte-stuffed, raw, replace or LZ payloads, obfuscation. It also has the BI2X node's commands (784 poll, 785 output), but the IIDX input layout on the wire is not in spice2x (only libaio-level, `games/iidx/bi2x_hook.cpp`).
- The BIO2's sliders and Effect for beatmania III (its effector is "Effect 1..7"), if wanted.

**pop'n music's USB I/O board (2026-10-21):** the owner asked for the pop'n cabinet's I/O and its modern stand-ins, after CrazyRedMachine's PopnForwarder. Its author's advice: drive the board through the game's `ezusb.dll` (`usbStart`, `usbFirmResult`, `usbCoinMode`, `usbPadRead`, `usbLamp`), which gives the real board, PopnIO (without its enhanced polling) and UltimatePopnController (through its replacement `ezusb.dll`). That DLL is 32-bit and needs Cypress's Windows driver, so:
- **Linux, done:** `--popn-io auto|PORT`, `--popn-io-dll FILE` (`runtime/host/popnio.h`; Settings > Cabinet I/O). HIRAKU drives the board over usbdevfs as `ezusb.dll` does (read from its code, docs/POPN-IO.md). A bare FX2LP gets the game's firmware from the user's `ezusb.dll` (found by its records' shape, loaded into RAM only), then a handshake, the coin mode `09 03`, and 64-byte interrupt polls: controls in (bytes 8–11, active low), lamps out (bytes 8–11), coins from the count. Out packets are checked against an allow-list (no EEPROM or security command can go out). `tests/popnio` runs it all against a pretend board. UltimatePopnController already works as a HID controller with named lamp outputs (lights file); PopnIO in REALIO mode is taken as the board.
- **Bring-up, when a board is at hand** (`HK_POPNIO_LOG=1`): the udev rule; does a bare board take the firmware and come back saying "KONAMI" on the same port; does it answer interrupt ins steadily (how often); each button, TEST, SERVICE, the coin (count, or only the switch bit 22?); the lamps, and which top lamp is topmost and which side lamp is which (HIRAKU: bit k−1 = Top Lamp k, bits 8–11 = Side Lamp 1–4); whether the bulk handshake is answered.
- **Windows, done (2026-10-21)**, the author's way: `hiraku-ezusb.exe` (`runtime/host/ezusb/helper.c`, built 32-bit by the same clang, `--target=i686-w64-mingw32`) loads the `ezusb.dll` given by `--popn-io-dll` (the game's, UltimatePopnController's, PopnIO's), calls `usbStart(0)`, `usbFirmResult()` until 0, `usbCoinMode(3)`, then `usbPadRead`/`usbCoinGet`/`usbLamp` at about 1 kHz, and trades the words with HIRAKU through shared memory (`ezusb/link.h`). It stops with HIRAKU (lamps off, `usbEnd`). Coins: the count when it moves, else pad bit 22 (UltimatePopnController's DLL counts nothing). `tests/popnio/test_ezusb_helper.cpp` (Windows builds with HK_TESTS, run under Wine) drives it against a pretend 32-bit DLL with the games' mangled names. Unchecked: a real board with Cypress's driver on real Windows; whether the Python core's Windows build (clang-cl) finds the helper beside it.

## 26. Windows (written 2026-10-16)

Windows x64 builds of the launcher and the games, cross-built on Linux and tested under Wine first, as the ARM port was with qemu (§22). The cores are plain C++ (the ARM port settled `char` and floating point, so results should match Linux bit for bit). SDL3, Qt6, zstd, zlib and libchdr all run on Windows, and spice2x, where the card readers and BIO2 code came from, is a Windows program. What has to move is the layer that talks to the system.

**Toolchain:** mingw-w64 (GCC 16, Arch's `mingw-w64-gcc`), with `cmake/mingw-w64.cmake` as the toolchain file, statically linked so the folder needs no mingw runtime. This is the EZ2 port's setup, proven there under Wine. The dependencies live outside the tree in `out/win-deps/`: SDL3's official mingw development package, and zlib and zstd cross-built from source (`tools/win-deps.sh`). clang's PGO flags (`-fprofile-instr-*`) are left out of that build; llvm-mingw would bring them back later if the speed is wanted. MSVC is not planned: it would mean redoing the GCC flags and attributes, and it is the toolchain most likely to choke on the recompiled code's very large functions.

**What changes, by area:**

| Area | Files | Linux → Windows | Phase |
|---|---|---|---|
| PowerPC fibers | `runtime/core/glue.cpp` | The assembly switch is ELF-only (System V registers). Use Win32 fibers (`ConvertThreadToFiber`/`CreateFiber`/`SwitchToFiber`), which don't make a system call. | 1 |
| 573 self-modifying-code guard | `runtime/sys573/mips.cpp` | `mprotect` + SIGSEGV → `VirtualProtect` + a vectored exception handler | 1 |
| Disk reads, files | `runtime/dev/image.cpp`, `ata.cpp`, the `stat` calls | `pread` → `ReadFile` at an offset; 64-bit offsets; `stat`/`S_ISDIR` → `std::filesystem` or `_stat64` | 1 |
| Save and settings folders | `runtime/shell/userdir.h`, `launcher/config.cpp` | XDG → `%APPDATA%\hiraku`; the portable `user/` folder is the natural Windows default | 1 |
| Linux-only pieces | Wayland pacing, the direct ALSA path, the profiling samplers (`sampler.cpp`, `HK_PY_PROFILE`), hidraw | Left out of the Windows build in CMake, with stubs where called | 1 |
| Process niceties | `runtime/main.cpp` | `setrlimit`/`nice` → `SetPriorityClass` or nothing | 1 |
| Networking | `sys573/link.cpp`, `python/lancable.cpp`, `netrelay.cpp`, `eaclient.cpp`, `eamuse/remote.cpp`, `tools/eamuse/server.cpp` | BSD sockets → Winsock behind one small shim header (`WSAStartup`, `closesocket`, `WSAPoll`, non-blocking setup); Unix sockets → loopback TCP | 2 |
| Lights socket | `host/lights.cpp`, the launcher's `getuid()` | Unix socket → loopback TCP or a named pipe | 2 |
| Launcher | `launcher/` | `.exe` names when it looks for the games; `windeployqt`; the serial device lists | 2 |
| Packaging | — | A zip: the launcher, `hiraku-<id>.exe`, the Qt and SDL DLLs, an empty `user/`. Unsigned builds get SmartScreen warnings. | 2 |
| Frame pacing | `host/pacing.cpp` | Wayland presentation feedback → plain SDL vsync first, then DXGI frame statistics or `DwmGetCompositionTimingInfo` | 3 |
| Audio latency | `host/window.cpp` | SDL3's WASAPI; `--fine-sound`'s 1 ms timer → `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` | 3 |
| Card readers and BIO2 | `host/acio.cpp`, `readers.cpp`, `hidout.cpp`, Settings | termios → Win32 COM (`\\.\COMn`, DCB, COMMTIMEOUTS); pcsc-lite → winscard; hidraw → Windows HID or SDL's hidapi | 3 |
| Python | Reliquary's tree | PCSX2 builds on Windows only with MSVC or clang-cl, so a separate toolchain and PCSX2's Windows dependency bundle; HIRAKU's host files in `hk_python_shell` must build under clang-cl too. How far Reliquary has drifted from Windows-buildable upstream is unknown. | 4 |

**Phases:**
1. **Builds and boots under Wine:** the games (Firebeat, Twinkle, DJ Main, 573) build as `.exe` and reach attract with SDL's default audio and vsync. No network and no cabinet hardware yet. Check: repeatable regression cases give the same hashes under Wine as on Linux.
2. **The launcher and networking:** Qt launcher, Winsock shim, lights socket, the zip.
3. **Parity on real Windows:** pacing, the audio timer, card readers, BIO2, HID lights. Wine can't prove latency or hardware.
4. **Python** (a separate track): clang-cl inside Reliquary's tree.

Keep the regression suite and the MAME comparisons on Linux. To check: no file names in the game data that differ only by case (Windows treats them as one).

**Phase 1 (2026-10-16): done for the four cores HIRAKU builds itself.**
- Toolchain: clang 23 targeting `x86_64-w64-mingw32` on Arch's mingw-w64 (headers, CRT, libstdc++, winpthreads), lld, statically linked: the same compiler as the Linux builds rather than mingw's GCC. `cmake/x86_64-w64-mingw32.cmake`; `tools/win-deps.sh` puts SDL3 (its mingw package), zlib and zstd in `out/win-deps`. The build's own `hiraku-recomp` runs under Wine; its output is the Linux one's byte for byte, line ends aside.
- `runtime/shell/sys.{h,cpp}`: page memory and protection, the write-fault handler (SIGSEGV on Linux, a vectored exception handler on Windows), `pread`, mapped files, folders, calendar time, the program's path, thread CPU time; Win32 fibers for the Firebeat core's guest threads. `runtime/shell/net.{h,cpp}`: sockets (Winsock). Linux keeps what it called before, moved behind these.
- Image streams: a CHD's stream was a `fopencookie` stream, which Windows has no counterpart of. Its readers now use `image::pread`, `image::size` and `image::close` (`runtime/dev/image.h`); on Windows a CHD's stream is a stand-in only those know.
- Pulled forward from phases 2-3, as they were small: the Winsock layer (the 573 session link, the e-AMUSEMENT client, the lights socket), the ACIO serial port as a Win32 COM port, PC/SC through `winscard`. Still Linux only then: cardio (since 2026-10-17 on Windows through SDL's HID calls, `readers.cpp`; no reader tried), `hiraku-eamuse`, the profiler (`HK_PROFILE`), the ACIO tests (pseudo-terminals), Twinkle's background video (no FFmpeg for Windows yet).
- **Checked under Wine 2026-10-16**, digests compared with Linux: popn7-attract, bm3final-practice, bm37th-play (18 GB disk), kbm-attract (Firebeat, 1.35 M fiber switches in popn7's 3600 frames); bm1stmix-boot, popn1j-boot (DJ Main); ddr3mj-boot, ddr3mj-install → ddr3mj-attract, drmn10m-install → drmn10m-boot (573, with its write guard and the NVRAM chain); bmiidx3-boot → bmiidx3-play (Twinkle). All 13 the same as Linux, NVRAM included. (bmiidx3's two stored baselines predate 271fb51's 4x CD drive; they were compared with today's Linux build.)
- Not yet: a window and sound under Wine or on real Windows (all runs above were headless), the session cable between two Wine processes. *(2026-10-17: both since done; the cable: GuitarFreaks 5th × DrumMania 4th, two Wine processes over loopback TCP, `drmn4m-install`, `gtrfrk5m-install`, `drmn4m-session` and `gtrfrk5m-session` give the Linux baselines.)*

**Phase 2 (2026-10-16): the e-AMUSEMENT server, the launcher, the zip.**
- `hiraku-eamuse.exe`: the server and the NET TAISEN relay on `net.h` (`netrelay.cpp`, `tools/eamuse/server.cpp`; `--clock` without `strptime`/`timegm`). Under Wine: it answers a call and its relay port listens.
- Lamps: `--lights-socket HOST:PORT` listens on TCP (any OS); the Windows launcher uses `127.0.0.1:47817` (Windows' `QLocalSocket` is a named pipe, which the game can't serve). Checked: the Windows popn7 under Wine, read from Linux.
- Launcher: `hiraku-<id>.exe`; Python found as `py -3` or `python` (`cfg::python()`) for ingest and unlocks; COM ports listed from the registry; PC/SC wording; cardio off; the Linux-only environment restore left out. Saves in `%LOCALAPPDATA%\hiraku`, where the launcher's Qt keeps its own (runtime and `tools/unlock.py`).
- Qt for Windows: qtbase 6.12.0 (the host's version, whose moc/rcc/uic build the launcher) cross-built static with the same clang (`tools/win-deps.sh`, 4th argument): the launcher is one `.exe`, no Qt DLLs. Qt 6.12's Windows plugin doesn't build with OpenGL off: left on (dynamic OpenGL, nothing extra needed).
- `tools/package-win.sh BUILD OUT.zip [portable]`: the games and launcher stripped, SDL3.dll, games/, docs/, tools' Python scripts, a README. Checked: popn7 run from the unpacked zip gives its baseline digest.
- **Checked under Wine** (in a nested `kwin_wayland --virtual --xwayland`, so nothing on the owner's screen): the launcher lists the 80 games with the built `.exe`s ready; started from it, popn7 runs windowed to its attract; the launcher's settings and the game's saves land in `%LOCALAPPDATA%\hiraku`. Linux unchanged: popn7-attract, ddr3mj-boot and the 573 session pair give their baselines.
- Full build (2026-10-16): every game, the launcher, the server and the tools build for Windows (1093 steps, 25 min at -j4); 7 more cases under Wine give their baselines (bm3-play, popnmt2-attract, ppp-attract, popn1j-play, bmclubmx-boot, bmcompm2-boot, popnstex-boot). `out/hiraku-win64.zip`: 213 MB (zip packs each file alone; the Linux package's xz shares one dictionary across the binaries).
- Ingest with nothing to install (2026-10-16): `hiraku-chdcat --info` and its extraction stand in for chdman (`tools/chd.py` uses it when there's no chdman, or with `HK_CHD_TOOL=chdcat`): byte-identical to chdman's for two CDs and a hard disk. The launcher ingests with `--copy-chd`: DJ Main, 573 and Twinkle CHDs are linked or copied into `dumps/<game>/` and read in place, never extracted. The Windows zip carries python.org's embeddable Python 3.14.8 (`hiraku\python`, found first by the launcher); 7-Zip is found where it installs itself. `machine.txt` is written with `\n` line ends and the cores ignore a `\r`. Checked under Wine with nothing installed: bm1stmix (zip + CHD) and bmiidx3 (zip + 5 CHDs) ingested by the package's own Python and chdcat; bm1stmix-boot gives its baseline, bmiidx3-boot the same digest as Linux from the extracted images.
- Not yet: `tools/ingest.py` for Firebeat sets on Windows (the program CD is still extracted, now by chdcat; untested) (7-Zip, chdman, paths), HID lights on Windows (`hidout` uses SDL's HID calls on Windows: built, untried with a device), real Windows at all (phase 3).

**Phase 4, HIRAKU's side (2026-10-17): the Python core's own sources for clang-cl.** Reliquary builds on Windows with clang-cl and MSVC's headers, not mingw's, so the Python core's files meet what mingw had hidden: no `ssize_t`, `unistd.h`, `S_ISDIR` or `strncasecmp`; `UNICODE` defined for the `hiraku-<game>` binaries (PCSX2's), which turns `gai_strerror` into the wide one; `min`/`max` macros from `<windows.h>` in C++; and exceptions off for every target.
- `runtime/python/CMakeLists.txt`: on MSVC-style compilers `/W4 /EHsc` (clang-cl's `-Wall` is `-Weverything`), and the directory drops PCSX2's `_HAS_EXCEPTIONS=0`, as `-fexceptions` does on Linux. On Windows `hk_python_shell` takes HIRAKU's Windows pieces as its own build does: `wasapi.cpp`, `asio.cpp`, `ws2_32`, `ole32 avrt uuid`, `winscard` (PC/SC), `NOMINMAX`; no pkg-config PC/SC or Wayland.
- `shell/sys.h`, `net.h`: `ssize_t` for MSVC; `sys::replace_file` (Windows' `rename` fails on an existing file: the card and e-AMUSEMENT saves, `cardunit.cpp`, `easerver.cpp`, wrote once only); `net::resolve_error` (`gai_strerrorA` on Windows). Linux calls what it called before.
- `lancable.cpp` and `eaclient.cpp` on `net.h`, as `sys573/link.cpp` and `eamuse/remote.cpp` are (a Unix socket path or `host:port`; Windows 10 has Unix sockets); `eaclient` connects within its time limit on Windows (`net::connect_within`), where the send timeout doesn't bound a connect. `strncasecmp` is `_strnicmp` for MSVC (`easerver.cpp`, `eaclient.cpp`); `hidout.cpp` (the sysfs permission check) and `userdir.h` (`S_ISDIR`) guarded.
- `core.cpp`: the `HK_PY_PROFILE` sampler (SIGPROF, `dladdr`, `cxxabi.h`) and the PGO hook are Linux only; folders, existence and an absolute run folder through `std::filesystem` on Windows. Windows has no `on_exit`: `atexit` leaves before PCSX2's static destructors as on Linux, but isn't told the status, so an interrupted run (130) leaves with 0.
- The patch (`reliquary-oracle.patch`) needed nothing: its only system-specific hunk (`memfd_create`, GS.cpp) is already in the non-Windows branch. It applies to a fresh unzip of the 1.9.4 zip.
- Checked: every `runtime/python/*.cpp` but `core.cpp`, and the shell files touched, compile for `x86_64-w64-mingw32` (also with PCSX2's `UNICODE`/`WIN32_LEAN_AND_MEAN`), and against Wine's MSVC-style headers with `--target=x86_64-pc-windows-msvc` (an approximation of clang-cl; `core.cpp` too, with Reliquary's headers: only Wine's old libc++ `<bit>` fails). Linux unchanged: popn9-play, popn14-play, drmnvj-attract, drmnvj-eamuse and the popn12 NET TAISEN pair give their baselines; `-url` to `hiraku-eamuse` connects (client and relay), and to a closed port says so.
- Open: on Windows the Python core leaves through `atexit` (no `on_exit`), which isn't told the status: a run stopped with Ctrl+C ends with 0, not 130. The same `rename` fix for `eamuse/protocol.cpp` (the server's saves) and `sys573/mips.cpp` (block recordings).

## 27. Firebeat games from a release (written 2026-10-16)

The goal: someone with a HIRAKU release and a MAME romset (or their own Firebeat discs) plays the 21 Firebeat games (beatmania III, pop'n music 4-8, ParaParaParadise, Keyboardmania) with nothing else to install, on Linux and Windows, and the games run exactly as the regress baselines say. A release can't carry them built: a Firebeat binary is the game's program recompiled (Konami's code). Today a user needs Ghidra, Java and the whole source build (`analyze/ghidra.sh`, `analyze/prep.py`, `hiraku-recomp`, clang), so in practice they can't.

**Measured 2026-10-16** (popn7 attract, this PC): the PowerPC interpreter alone (`--interp-only`) takes 19.3 ms a frame (native: 1.4 ms; a frame is 16.7 ms), and its picture leaves the native run's at frame 378: it boots the kernel itself (LLE), where native runs the guest's threads as fibers. So the interpreter as it is can't stand in for the build.

**Roadmap:**
1. **FB-1, analysis without Ghidra.** `analyze/` in HIRAKU's own code, on `src/ppc/decode.cpp`: functions from the entry, the exception vectors, calls and code pointers in data (Ghidra's orphan-code pass, `ExportManifest.java`'s extra functions), each function's ranges, tail branches, and `bctr` jump tables (the `rlwinm`/`lwzx`/`mtctr` patterns), plus `[recomp] extra_entries`. Python (it ships with releases now) or C++ in `hiraku-recomp`. **Exit test:** for all 21 games, `recomp.txt` from it either matches the Ghidra one, or differs only where the difference provably doesn't matter, and the regress cases give their baselines.
2. **FB-2, ingest does it.** Adding a Firebeat game runs the analysis and writes `dumps/<id>/recomp.txt` (no `build/`), with progress in the launcher's log. The game shows as ready once the next step can run it.
3. **FB-3, running without a compiler.** Two ways, measured against each other before choosing:
   - (a) **A recomp-driven executor:** `hiraku-recomp`'s understanding of each function (the same `op_*` calls, `HK_POLL` points, calls and tail branches) turned into a compact pre-decoded form at load time, run by a threaded dispatcher inside the release binary. Exact by construction (the same operation code and poll points as the generated C, so the same scheduling); goal ≤ 3-4 ms a frame here, ≤ 8 ms on an N100.
   - (b) **A build kit:** the release compiles the game once on the user's PC: `hiraku-recomp` plus a bundled compiler (clang, ~100-150 MB, or TinyCC at ~1 MB with slower code) and the runtime as a library. Native speed, but a big download and minutes per game.
   (a) first: no download, no compiler. (b) only if (a) can't reach the speed.
4. **FB-4, the release's Firebeat binary:** one `hiraku-firebeat` (the runtime, no game code) that takes the game from `dumps/<id>/` (program and `recomp.txt`), as DJ Main's binary already does; the launcher starts it for every Firebeat game. Keyboardmania's XT446 synth runs interpreted there (its compiled MEG programs come from a firmware recording, so they stay out too).
5. **FB-5, checks:** every Firebeat regress case run through the release path (ingest from the romset, analysis, the executor) gives its baseline digest; the release workflow builds `hiraku-firebeat` with no game code in it (no `build/<id>/` anywhere on the CI machine).
6. **FB-6, speed:** frame times on the N100-class target. Profile-guided builds of the runtime (its profiles hold counts for HIRAKU's own code, not the game's) if they help.

Not in scope: the Konami Python games in releases (PCSX2 Reliquary, PLAN §23) and IIDX's three Twinkle sets with no core yet.

**Done (2026-10-17): FB-1 to FB-5.** A release's `hiraku-firebeat` runs every Firebeat game from its program alone: no Ghidra, no compiler, no game's code in the binary.
- **FB-1, HIRAKU's own analysis** (`src/ppc/analyze.{h,cpp}`, `hiraku-recomp --analyze PROGRAM [--entry --base --extra]`), in Ghidra's order:
  1. its function start patterns on the raw image: `stwu r1, -N(r1)` or `mflr r0` right after the end of other code (a return, a branch, data);
  2. flow from the program's entry and game.toml's extra entries (`bl` targets become functions; bodies follow fall-through, branches and jump tables up to another function's entry);
  3. the same patterns again where nothing reached, then `stwu`+`mflr` pairs anywhere, one at a time in address order;
  4. as a last resort, addresses the code builds (`lis`/`addi` pairs) and aligned data words, when they point at code nothing reached that starts right after other code's end (not text: four printable bytes are a string; branches must land inside the program);
  5. the exception vectors last, in order, each unless covered, a vector's `b handler` making the handler a function.
  Jump tables: GCC's switch dispatch (`cmplwi` bound, `slwi 2`, `addis`/`lwz` or `lis`/`addi`/`lwzx`, `mtctr`, `bctr`), each target once. Then prep.py's secondary entries. A fifth of a second for Keyboardmania 3rd's program.
  Against Ghidra's manifests for the 21 games (`out/fb1/cmp.py`, scratch): pop'n 7 and Animelo identical; the rest differ in dead code only (906 entries/ranges in all, Keyboardmania 3rd the most: Ghidra decoded far more unreferenced code there). **What counts:** all 21 games run 6,000 frames identical from either analysis, with no interpreter fallback from HIRAKU's own (pop'n music Mickey Tunes! has one from Ghidra's).
- **FB-2:** no step at all: `hiraku-firebeat` analyses the program when it starts (`--spec FILE`, or a `dumps/<game>/recomp.txt`, override it).
- **FB-3a, the executor** (`runtime/core/exec.{h,cpp}`; `src/ppc/spec.h` shared with `hiraku-recomp`, whose output stays byte-identical): each function as steps built by a mirror of the recompiler's emitter (the same `op_*` calls, cycle charges, polls, calls, returns, jump tables and fall-throughs), run by a threaded dispatcher with every `op_*` inline and the cycle charges folded into the steps they precede where nothing jumps between. The runtime calls guest functions through `call_guest`/`has_guest` (core/dispatch.cpp), recompiled or executed. pop'n 7's attract: 2.9 ms a frame (native 1.1 ms). (b), the build kit, is not needed.
- **FB-4:** `hiraku-firebeat` (CMake, always built): the game from `--game`, its entry and load address from game.toml's `[program]`; the launcher and `tools/regress.py` use it for Firebeat games with no binary of their own.
- **FB-5, checks:** all 9 Firebeat regress cases through `hiraku-firebeat` from HIRAKU's own analysis give their baselines (frames and sound; the statistics' function count and how the functions ran are left out of the comparison). From the release package in a bare Debian 12 container: beatmania III and ParaParaParadise added from MAME archive sets (the package's own Python and CHD reader) give their baselines; pop'n 7 runs, but the archive's program CD is MAME's dump, not the redump the baseline was recorded from. The Windows zip under Wine: beatmania III added from the MAME set and run by `hiraku-firebeat.exe` gives the Linux baseline. Ingest now gives pop'n music's second disc to the sound board as its DVD even when an older MAME set keeps it as a hard disk CHD.
- Not yet: FB-6 (frame times on the N100-class target).

## 28. A public release: games working from dropped-in data alone (written 2026-10-17)

The goal: a GitHub release (Linux `.tar.xz`, Windows `.zip`) that someone downloads, starts, and adds games to by dropping their MAME sets (or their own dumps) on the launcher, with nothing else to install. State on 2026-10-17: the repo is public, but there is no release workflow and no release; the packages are made by hand on the owner's PC (`tools/package.sh`, `tools/package-win.sh`).

**What already works from a package** (the launcher's drop-in, `tools/ingest.py --copy-chd`):
- DJ Main, System 573 and Twinkle: a MAME set (zip, or a folder of its files) and its CHDs. The set is told by its contents (`tools/mameset.py`), the CHDs are linked or copied into `dumps/<id>/` and read in place, and `hiraku-chdcat` stands in for chdman.
- The Windows zip carries python.org's embeddable Python for ingest; Linux uses the system's `python3`.
- Konami Python 1 (pop'n 9-14), on a PC with the Reliquary build: a set's folder, with or without its `.py1` (c9a115b), with the PS2 BIOS and MagicGate keys dropped with it or found from a Python game already added.

**What's needed, in order:**
1. **R-1, the release workflow** (GitHub Actions; the cores need no game data to build):
   - Linux: clang, Qt 6.12 to bundle (`tools/package.sh` bundles the host's), on an older distribution than Arch so the package runs on older glibc; SDL3 from source if the runner's is too old.
   - Windows: the clang + mingw-w64 cross build (§26) on Linux; `tools/win-deps.sh`'s SDL3, zlib, zstd and static Qt 6.12. Qt takes long to build: cache it.
   - **No game data on the build machine:** there is no `build/` or `dumps/`, so nothing derived from a game can be in a release. System 573 and Twinkle then have no recorded R3000 blocks (`build/<id>/mips573.txt`) and run interpreted: about 1.85 ms a frame for DDR 2ndMIX instead of 0.36. To check on the N100-class target (§14). The Firebeat targets aren't built at all (no `build/<id>/recomp.txt`).
   - Release notes list which games a release can run.
2. **R-2, nothing else to install for adding games:**
   - `.7z` sets need 7-Zip (`tools/mameset.py`). Bundle 7-Zip's stand-alone `7za` (LGPL) in both packages and look for it there first.
   - Twinkle's background video uses FFmpeg, which the Windows build has none of yet.
   - The three IIDX Twinkle sets with no core stay out until they have one.
3. **R-3, Firebeat (21 games): §27, FB-1 to FB-5.** A Firebeat binary is the game's program recompiled (Konami's code), so a release can't carry one, and today adding a Firebeat game needs Ghidra, Java and the source build. FB-1 to FB-5 replace that with HIRAKU's own analysis, run by ingest, and one `hiraku-firebeat` that runs the game from it without a compiler. Keyboardmania's XT446 MEG programs compiled from a recording (§14, `tools/meggen.py`) stay out too: interpreted in a release. None of FB-1 to FB-5 is started.
4. **R-4, Konami Python on Linux:** the release workflow builds PCSX2 Reliquary with `oracle/pcsx2/reliquary-oracle.patch` (`oracle/pcsx2/build-reliquary.sh`) and the Python games against it; `tools/package.sh` already bundles Reliquary's `resources/` and libraries. The BIOS and keys can't ship: users drop them in with the games. Python 2 (DrumMania V/V2) still needs its `.py2`: ingest from MAME's set (`drmnvj.zip` and its CHD) and building its NVRAM (Reliquary's, with the OSD blocks of a console NVRAM the user gives) aren't done.
5. **R-5, Konami Python on Windows:** §26 phase 4 (Reliquary builds on Windows only with clang-cl).
6. **R-6, checks before each release:**
   - Real Windows (§26 phase 3): window, sound, frame pacing, card readers, BIO2, HID lights. So far only Wine.
   - Firebeat ingest on Windows (untested).
   - A clean-PC test: unpack the package on a PC with nothing installed, add a few games from MAME sets (a DJ Main, a 573 with its install CD, a Twinkle), and compare their regress cases' digests.

R-1 and R-2 give a first release that runs DJ Main, System 573 and Twinkle (less the three sets). R-3 adds Firebeat, R-4 and R-5 Konami Python.

**R-3 done (2026-10-17):** §27's FB-1 to FB-5; Firebeat games run from both packages.

**R-4 (2026-10-17), partly:** `hiraku-python`, the Python core's own binary (`--game`, built in Reliquary's tree beside `hiraku-<game>`); the launcher and `tools/regress.py` use it; all 23 Python cases through it give their baselines. **Blocked:** the release workflow can't build Reliquary: the pinned 1.9.4 has no public source (the public repository, StillJC/pcsx2-reliquary, stops at 1.5.1, last pushed 2026-06); the owner's zip came from elsewhere. Options for the owner: host the 1.9.4 source zip (PCSX2 is GPL-3.0) as an asset of a HIRAKU release the workflow downloads, or ask Reliquary's authors to publish it. Also open: DrumMania V/V2 from MAME's set (`drmnvj.zip`: its BIOS, dongles, HDD id and NVRAM are all in it) needs MAME's own disk (`e02_jaa_2005050201`): its HDD id belongs to that disk, not to the torrent's image; ingest can map the set once someone has the CHD to check it with.

**R-1 and R-2 done (2026-10-17), not yet run on GitHub:**
- One binary per core: `hiraku-djmain`, `hiraku-sys573`, `hiraku-twinkle` take the game with `--game ID` (or their name as `hiraku-ID`, a link). `HK_GAME_BINARIES` off (a release) builds only those: no Firebeat binaries, no recorded 573/Twinkle blocks (interpreted), no XT446 MEG recording, whatever `build/` holds. The launcher and `tools/regress.py` (`HK_REGRESS_BIN`, and `HK_REGRESS_OUT` for another output folder) use a core's binary when a game has none of its own. Checked: 13 DJ Main, 573 and Twinkle cases through the core binaries give their baselines.
- The launcher in a package: Firebeat and Konami Python games show "Not in this package", with why; the how-to texts say what dropping files does (no chdman, nothing unpacked); an added game is named by its title and selected.
- `tools/release.sh linux|windows OUT` builds and packages from a checkout (`HK_TESTS` off, `--as-needed`); `tools/release-setup-ubuntu.sh` installs the build tools (Ubuntu 22.04: glibc 2.35, LLVM 21 from apt.llvm.org, CMake from PyPI); `tools/release-deps-linux.sh` builds SDL3 (shared), zstd (static) and the FFmpeg below, and fetches Qt 6.12 (aqtinstall), python-build-standalone's Python 3.14 and 7-Zip's `7zz`; `tools/release-deps-windows.sh` runs `tools/win-deps.sh` on fetched sources (the static qtbase of the host's Qt version) and adds FFmpeg and `7zr.exe`. `.github/workflows/release.yml`: both packages on a `v*` tag (a draft release, with FFmpeg's and SDL3's sources) or by hand (artifacts); Windows in an Arch container.
- `tools/ffmpeg-min.sh`: FFmpeg 9.0.2 static with only the MPEG program stream and raw MPEG video demuxers, MPEG-1/2 video and the Video CD/DVD audio parsers and decoders (stream detection needs them: without the raw demuxer the video stream had no codec, a black background), with nasm. The Windows build has Twinkle's video now (`runtime/twinkle/video.cpp` reads the disc through `image::pread`, not stdio).
- PC/SC loaded at run time on Linux (`HK_PCSC_DLOPEN`, `runtime/host/pcsc_load.cpp`): the games start without libpcsclite.
- The packages: Linux 42 MB (`.tar.xz`: the three cores, the launcher with the Qt Company's Qt (its own libraries and the xcb extensions only: the rest is the system's), SDL3, Python, 7-Zip; glibc 2.35), Windows 32 MB (`.zip`; it was 213 MB with a binary per game). Both portable (`user/`).
- **Checked** (R-6's clean-PC test): in a bare Debian 12 container (plus `libwayland-client0`, which every desktop has), the package's own Python adds beatmania, DDR 2ndMIX and IIDX from the MAME archive's sets (zip, folder, `.7z`), and `bm1stmix-play`, `ddr2m-boot` (with MAME 0.289's cassette EEPROM; the archive's is the older dump) and `bmiidx-clear` give their baselines; the launcher runs under Xvfb, and `--add` (what a drop does) adds a game. The Windows zip under Wine: `bm1stmix-play` added and run from the zip gives its baseline; IIDX 1st style with video (`bmiidx-clear` → `bmiidx-video`) gives the Linux baselines.
- Found on the way: `strcasestr` (no such function on Windows) in `cdromimg.cpp` since da578d6; the Twinkle cases need `--mame-cd` by hand as `tools/regress.py` gives it.

### §28 checklist: someone with a GitHub build and their game data (2026-10-17)

What such a user meets, step by step, and where each stands (✓ done and checked; ✗ not yet, why):
1. ✓ **Find and download a build:** the README's Download section; `.github/workflows/release.yml` makes the packages (both jobs run end to end in fresh containers here). ✗ Not on GitHub yet: needs the owner to push and tag.
2. ✓ **Unpack and start, nothing installed:** Linux (glibc 2.35+, `bin/hiraku`, its own Qt, SDL3, Python, 7-Zip) and Windows (`bin\hiraku.exe`, its own Python, 7-Zip). Checked: bare Debian 12 container (with a desktop's libraries) and Wine. ✗ Real Windows not tried (a VM would need a Windows download: owner's call).
3. ✓ **See what can be played:** every game listed; Konami Python ones say "Not in this package" and why; each game's page says which files it needs.
4. ✓ **Add a game by dropping files:** MAME sets as zip, 7z or folder, CHDs read in place (copied in, so the files can go), disc images; Python 1 folders without their .py1, with the BIOS and keys dropped along. Checked from the MAME archive's sets: DJ Main, 573, Twinkle, Firebeat, on both systems. Older MAME sets' pop'n DVDs as hard disk CHDs: handled.
5. ✓ **Play, exactly as the source build does:** one binary per core; Firebeat through `hiraku-firebeat` (own analysis, executor): the regress baselines come out identical from the packages. Twinkle's background video on both systems.
6. ✓ **Saves and settings stay with the folder** (portable `user/`), as the README says.
7. ✓ **Konami Python games (pop'n music 9-14, DrumMania V/V2) from the Linux package:** the owner hosts Reliquary 1.9.4's source (the `reliquary-1.9.4` pre-release); `tools/release-python-linux.sh` builds `hiraku-python` on Ubuntu 22.04 (PCSX2's own dependency script less Qt, KDDockWidgets and SDL; PCSX2's interface off; PC/SC loaded at run time; libpcap and `libchdr_mini.so` in the package). Checked: in a bare Debian 12 container with a desktop's graphics drivers (Mesa's Vulkan), pop'n 9 added from the collection's folder with `bios` and `keys` gives `popn9-play`'s baseline. Needs Vulkan or OpenGL drivers and a desktop-sized `/dev/shm`. ✗ DrumMania V from MAME's set: needs MAME's own disk image to check against.
8. ✓ **Konami Python on Windows** (§26 phase 4, 2026-10-17): `hiraku-python.exe` built by the release workflow's `python-windows` job on GitHub's Windows runners (PCSX2 Reliquary with clang-cl and its own dependency script less Qt, KDDockWidgets and SDL; SDL's own MSVC package), packaged into the zip's `bin\` with its DLLs and `resources\`; `libchdr_mini.dll` for the ingest. Fixed on the way: MSVC's headers (`ssize_t`, exceptions, `UNICODE`), `setvbuf(_IOLBF, 0)` (MSVC's runtime rejects it), PCSX2's Windows paths with `/` (a Reliquary patch hunk), COM multithreaded before SDL's window, WASAPI's ids, `pythonfs.py` and the DLL missing from the zip. Checked under Wine: the packaged launcher adds pop'n 9 (folder + `bios` + `keys`), and it runs headless, with Vulkan, and windowed. `popn9-play` keeps the game in step with Linux (the pictures differ on 147 of 14,000 frames, the sound's running hash from frame 47: floating point from another compiler and Wine's C runtime). ✗ Real Windows.
9. ✗ **Frame times on an N100-class PC** (FB-6): needs the hardware. On this PC the executor runs pop'n 7 at 2.9 ms a frame (native 1.1).

**The first Windows user's feedback** (v0.1.2, 2026-10-17):
- ✓ IIDX reset by its watchdog every 29.5 s: a CHD's track length read through stdio, which a CHD stream doesn't answer on Windows (e5d4008).
- ✓ The launcher on a small screen: every window fits the screen it opens on (`launcher/fit.h`: shrunk and moved, a 640x480 screen included); Settings scrolls with OK and Cancel kept below. Checked in a 640x480 nested KWin, Linux and Wine. `hiraku --show settings|controls|lights|picture|unlocks|about|main --shot FILE` opens one and saves its picture.
- ✓ A choice of output device on Windows (Settings > Audio > Output: the devices by the names Windows shows; `--audio-device NAME`, shared).
- ✓ WASAPI exclusive mode (`--audio-exclusive`, Settings > Audio > Exclusive; `runtime/host/wasapi.cpp`): the device alone, event-driven, MMCSS "Pro Audio", float/32/24/16 bits, falls back to shared with the reason. ✗ Not tried on real Windows: Wine's audio refuses exclusive mode (the shared fallback and its message checked there).
- ✓ ASIO (`--audio-asio NAME`, Settings > Audio > Output's `ASIO:` entries; `runtime/host/asio.cpp`): the driver interface declared from the specification, as Wine's pwasio does (Steinberg's SDK is GPLv3 since 2025-10-29, but not needed); drivers from the registry, the driver's own buffer unless `--audio-buffer`, its sample form written on its thread. Checked under Wine with pwasio: float, 48 kHz, 64 frames, 99.97 % of the rate; on real Windows by the owner (2026-10-18): works.

## 29. Two more shaders: Lola Retro and the CRT beam simulator (reviewed 2026-10-20)

The owner asked for two shaders (M18 in the table): **Lola Retro** (Zoki, MIT, github.com/zoran123456/lola-retro,
reviewed at 2640253, 2026-10-19) and the **Blur Busters CRT Beam Simulator** (Mark Rejhon and Timothy Lottes, MIT,
github.com/blurbusters/crt-beam-simulator, 734786a). The owner's own beam-simulator work (a gamescope ReShade port,
outside the repo) is where the lessons in "What carries over" come from.

### Lola Retro: a CRT picture, the whole analog chain

- **What it is:** a libretro slang preset pack (13 presets × curved/flat, plus HDR versions), 8 passes:
  native (finds the console pixel), encode (RGB → composite/S-Video/RF signal at the console's real pixel clock and
  subcarrier phase), decode (the TV's luma trap or comb filter, chroma bandwidth, colour decoder with "red push"),
  glow (two passes), beam (a spot per line whose width grows with brightness, per gun; a pass 8192 wide), crt (mask,
  curvature, halation, white point, glass, the casing and room around the tube), present. It is checked against
  measured test patterns (colour bars within ~2/255; light output of alternate lines 0.4996 of white).
- **It fits HIRAKU's engine as it is.** `tools/slang2glsl.py` converts `consumer-tv-composite` with no change (8
  passes, 59 parameters) and `runtime/host/picture.cpp` runs it (bm1stmix, nested KWin, 1280×960): the picture is
  right. One thing is missing: the crt pass reads `LR_CRTFeedback`, its own output a frame ago (the afterglow),
  which the engine doesn't provide (RetroArch's `<alias>Feedback` / `PassFeedbackN`): it reads black, so there is
  no afterglow; the rest works.
- **Arcade timings are built in:** `LR_CONSOLE` 7 = PC VGA (31 kHz), 8 = arcade 15 kHz, 9 = arcade 24 kHz, 4 =
  PlayStation, and `LR_CABLE` 3 = RGB (the encode/decode chain skipped). So: DJ Main, System 573 and Twinkle →
  15 kHz arcade RGB (the 1084S / studio-monitor tube); Firebeat's 640×480 → 31 kHz (`pc-monitor-vga`); Python's
  480i/480p likewise by its mode. HIRAKU knows each core's mode, so it can set these per board, as `hiraku.params`
  sets 480-line pictures progressive for the two shaders already there.
- **Cost** (RX 7900 GRE, 1280×960, GPU time per frame): sharp 0.01 ms, crt-guest-advanced-fast 0.17, crt-royale
  0.29, **Lola 0.93** (3× royale). An N100's iGPU is roughly 40-60× slower than this card: Lola is likely too heavy
  there at full screen (royale already borderline); a desktop-GPU option, labelled so. Most of it is probably the
  fixed 8192-wide fp16 beam pass; to measure per pass before choosing presets.
- **Not taken:** the HDR presets (they write HDR10 themselves; HIRAKU's window is SDR OpenGL), the scenes and
  casings around the tube (kept as parameters, off), the composite/RF presets as defaults (the cabinets were RGB;
  composite stays a choice).

### The CRT beam simulator: a rolling scan on a fast display

- **What it is:** one pass. Each display refresh draws the slice of a simulated CRT refresh that the beam would be
  sweeping then: rows above the beam glow from the new frame, rows below fade from the old one, with each pixel's
  light (in linear light, gamma-corrected, `GAMMA`) spread over as few refreshes as its brightness allows ("variable
  per-pixel MPRT", `GAIN_VS_BLUR`). It reads the three latest source frames. 60 Hz content on a 240 Hz display looks
  like a CRT's motion; 120-144 Hz works with less benefit. Any display/content ratio works (2.4 at 144 Hz).
- **Its own words on integrating it:** the demo fakes its history by scrolling one picture sideways
  (`MOTION_SPEED`, the "rolling image" the owner's port had to set to 0); a real integration keeps the last three
  frames and runs the pass at every display refresh, independently of the game's frame rate, with the simulated CRT
  refresh at the content's rate. That is exactly what HIRAKU's display pacing already does for smooth motion
  (`pacing.h`): a present at every refresh through fifo-v1 (KWin 6.7) or DWM's vblank, and `pacing::phase(i)`, the
  exact moment of refresh i within the game frame. So the beam's position comes from real time and the game's own
  rate (58.00 Hz DJ Main, 59.94 Firebeat), not from counting refreshes, and the "frames per Hz" constant goes away.

### What carries over from the owner's gamescope port

- It must present at every vblank, locked to it: a missed or doubled refresh shows as a dark or bright band. In
  HIRAKU the pacing reports missed refreshes already (presentation-time); the exit report should count them for the
  beam simulator, as the port's `STATS=1` counted composites (~240 a second = running, ~60 = not).
- The content's frame rate has to be paced to the display (the port needed MangoHud's limiter): HIRAKU does that.
- Gamma has to match the monitor (the port settled on 2.2; the shader's default is 2.4); SDR, not HDR; ABL/APL off.
  `GAIN_VS_BLUR` 0.7 at 240 Hz. These become settings with those defaults, with the shader's own warnings in the
  launcher.
- The port needed forced compositing; HIRAKU's direct scanout (fullscreen) is better still, nothing to force.

### The plan (M18)

1. **The engine (done 2026-10-20):** feedback textures (`<alias>Feedback`, `PassFeedbackN`: a pass whose output
   is read so keeps two targets, swapped at each new frame; the last pass then draws into one and is copied to the
   window); `OriginalHistoryN` (a ring of the frame's pictures, as long as the preset reads). `FrameCount` is the
   game's frame number (`picture::draw`'s `frame`): with smooth motion it used to advance every display refresh,
   flipping a shader's interlaced fields (royale's 480i, Lola's fields and subcarrier phase) at the display's rate.
   A draw of the same frame again (an in-between picture) replaces the frame's picture and keeps FrameCount, the
   history and the feedback. `tests/picture` (a hidden GL window, small presets) checks all three for new frames and
   repeated ones. In the game: sharp, light, full and Lola's pictures identical to before (bm1stmix, nested KWin);
   Lola's afterglow now there (against `LR_PERSIST = 0`, the pixels that differ are the moving notes, bar lines and
   dancer).
2. **Lola as a picture mode (done 2026-10-20):** `--picture lola`, the launcher's "Analog: Lola Retro" (its tab,
   with a **Look** list), F10's fourth. Measured first, pass by pass (`HK_PICTURE_PASSES=1`): the beam pass, 8192
   samples wide whatever the window, was 0.52 of the composite preset's 0.93 ms; at twice the window's width
   (`slang2glsl.py --set`) 0.17 ms, the picture the same (PSNR 56 dB), so HIRAKU's conversion sizes it so. With an
   RGB look the whole picture is 0.33-0.37 ms (royale 0.29). One conversion (`consumer-tv-scart-rgb`); the other
   presets, which differ only in their values, are looks (`looks/*.params`, `tools/slangp2params.py`) without the
   curvature, overscan and timing. Each core says its monitor (`Core::monitor_khz`: 15, DJ Main 24, Firebeat 31)
   and the shaders read `hiraku-<k>khz.params` for it: the default look per monitor (1084S, studio monitor, PC
   VGA: Lola's own advice for arcade games, checked on IIDX 3rd, beatmania III and 1st mix frames), and Firebeat's
   VGA timing (Lola's auto reads 640x480 as 480i). Overscan 0 for all: the whole picture, HUD included.
   `HK_PICTURE_SNAP` now counts game frames. Not taken: the "TV show" preset (an extra pass, the author's test)
   and the HDR presets.
3. **The beam simulator as a motion mode (done 2026-10-20, not yet seen on a fast display):** `--crt-beam`
   (`--crt-beam-gamma`, `--crt-beam-gain`), the launcher's Settings > Motion > "CRT beam" (with smooth motion and
   "each frame held" the other two). The pass is a port (runtime/host/beam_shader.h, MIT notice;
   third_party/crt-beam-simulator) over the game's own two latest pictures, which the picture mode draws once a
   frame at the window's size; each refresh is the pass alone (0.01 ms). Its place is the refresh's moment from
   display pacing at the game's own speed (`pacing::phase`, `pacing::refreshes_in_frame`: 2.4 at 144 Hz for a
   60 Hz game), on when that is 1.9 or more (120 Hz and up) for every frame alike; the original's counter and
   LCD slew go away. The frame being drawn is the latest (the demo's convention would add a frame of latency); a
   refresh crossing into the next frame draws this one's top again (the same light). Keyboardmania's turned
   monitors (`Core::monitor_turned`) sweep left to right. Not with split windows, nor the Python core.
   `tests/picture` checks each refresh against the formula and that the refreshes' light adds up to the frames'
   times the gain (4, 2.4, 4.004 refreshes a frame); in a nested KWin (60 Hz) games run with it and say why it's
   off. **Owner's test (2026-10-20):** Lola Retro "perfect"; the beam showed a slow, wavy change of brightness.
   The cause: the original's curve (the sRGB formula with its exponent replaced: sRGB at its default 2.4, but at
   2.2 a curve no display has, 5-10 % off a 2.2 display) and, at exact speed, the light's split over the refreshes
   drifting (4.004 refreshes a frame: a cycle every 4 s), which turns any mismatch into a wave (simulated: 2-16 %
   swings at 4.004, none at 4.0). Fixed: plain power curves, and the beam takes whole refreshes where the display
   is within 0.5 % (`pacing::set_whole_refreshes`: 59.94 Hz games at 120/240 Hz, 0.1 % faster). Second test
   (pop'n 8, 58.49 Hz: exact speed): the wave only in darker parts. Simulated, a drifting split leaves a 6 Hz
   beat of 2-4 % in darks even on a matching display (not rounding: dithering didn't help; a sharp band edge was
   worse), which slow dark transitions make more of. So the beam now locks wherever display sync would (3 %:
   pop'n 2.6 % faster). DJ Main's 58 Hz still drifts. The exit report now counts the frames' other refreshes'
   commits (shown, late, replaced): the smooth-motion runs had 44 % of frames' first commits "replaced" in
   direct scanout, unexplained (the beam's top band would have flashed).
4. **Not first:** the Python core (pop'n 9-14 draw through Reliquary's GS, and smooth notes run those games at the
   display's rate, where a beam simulation doesn't apply); HDR output.
5. **Check:** offline, the beam pass's output summed over a game frame's refreshes equals the frame in linear light
   (its photon budget), and snapshots per refresh show the rolling band; then the owner's eyes on a 240 Hz and a 144 Hz display.

## 30. The PC pop'n music games (written 2026-10-21)

From pop'n music 15 ADVENTURE (2007) on, pop'n is a Windows program on a PC. The owner chose to **wrap** the original program, as `hiraku-python` wraps PCSX2 (Reliquary), rather than reimplement Windows: on Windows it runs as it is, on Linux under Wine, and HIRAKU supplies the cabinet around it from DLLs of its own. The first game is pop'n 15, and the later ones come once it is in a good state.

**The games, by how they start:**
- 15–18: an `.exe` (`popn15.exe`) on Konami's older library, `libavs-win32` with the ea3 e-AMUSEMENT. spice2x doesn't run these.
- 19 Tune Street, 20 fantasia, 21 Sunny Park: `popn19/20/21.dll`. 22 Lapistoria to the last M39 builds: `popn22.dll`. All are 32-bit and started by spice2x (GPL-3.0, like HIRAKU), which replaces the cabinet's calls with its own.
- The newest is a 64-bit `popn.dll` on the cabinet with a second screen and the BI3A I/O.

**pop'n 15, measured (G15, programs 2007-03-30 to 2007-09-18):**
- **What it calls:**
  - `ezusb.dll`: the same `usbPadRead`/`usbLamp`/`usbCoinGet` calls as the FX2 board behind `--popn-io`. Pad bits: test 7, service 6, coin 22, buttons 1–9 = 8–16.
  - `libacio.dll`: the IC card reader on COM1.
  - `libavs-win32`: ea3 e-AMUSEMENT.
  - `libcomm`/`globbyc`: local link and lobby.
  - Windows: Direct3D 9 with `d3dx9_24`, DirectSound, and ACM for ADPCM.
- **Paths:** `D:\popn15\data` and `D:\popn15\prog\<date>`. E: is its backup drive (settings, bookkeeping, `dcs.bin`). NVRAM and RAW live in `./CONF` under the program folder.
- **Integrity lists:** `prg.bin` and `dat.bin` hold a count, then entries of an MD5 and a path. Each boot hashes one twentieth of the data files, and `E:\dcs.bin` says which twentieth is next. The program folder checked so far lists no program files in `prg.bin`.
- **Boot:** on a new E: the game makes its backup files and asks to be switched off and on (BACKUP ERROR 5-1502). Without a network it stops at "no router" (5-2000) until SERVICE (play offline) or test mode's e-AMUSEMENT OFF. It finds e-AMUSEMENT by DNS (`eamuse.konami.fun`) and pings the default gateway.

**Steps** (✓ done 2026-10-21: pop'n 15 boots, plays offline at 60.00 pictures a second with sound, from `hiraku-pc` on Linux under Wine 11 and from the Windows `hiraku-pc.exe` under Wine; not yet on real Windows, nor with a real board, reader or controller):
1. ✓ **PC-1, the cabinet DLLs** (`runtime/pc/`, built 32-bit by the same clang as `hiraku-ezusb.exe`, for both packages):
   - `ezusb.dll`: the pad, lamps, coins and watchdog.
   - `libacio.dll`: the reader, after spice2x's `acio/icca`. `--reader` keeps the game's own `libacio.dll` and points COM1 at the real reader.
   - Path hooks in the game's modules' imports (`CreateFileA`, `FindFirstFileA`, `GetFileAttributesA`, `CreateDirectoryA`, `DeleteFileA`, `MoveFileA`, `RemoveDirectoryA`, `GetFullPathNameA`): `D:\popn15` goes to the game's folder and `E:\` to its save folder. The same on Windows and Wine, with no drive letters.
   - Direct3D 9 created windowed, so the picture scales to the window.
2. ✓ **PC-2, `hiraku-pc` and the link** (also: the first boot's restart done for the player; `--popn-io` with the game's own `ezusb.dll` for a bare board's firmware):
   - The host process starts the game: Wine on Linux, in a prefix in HIRAKU's user data; directly on Windows.
   - It shares a small memory with the DLLs: a `/dev/shm` file on Linux (Wine maps it as a file), a named mapping on Windows. The memory carries pad bits, lamps, coins and the card.
   - The host side is HIRAKU's own: bindings and joysticks (`input.cpp`), lamps and HID lights, card readers, and the pop'n board natively on Linux (`popnio.cpp`), which Konami's `ezusb.dll` can't reach under Wine.
   - The game's window has the keyboard, so the DLL reads keys itself from the bindings the host passes.
3. ✓ **PC-3, adding and starting it** (the launcher's family "pop'n music (PC)", `pcpopn`, with pop'n's controls and lamps; both packages carry `bin/pc/`): ingest tells the folder (`prog/<date>/popn15.exe` + `data/`) and runs the newest program, with the data read where it is. Plus a launcher entry, the offline/e-AMUSEMENT setting and the reader.
4. ✓ **PC-4, e-AMUSEMENT** (2026-10-22: network, cards, the player, rankings, NET TAISEN; docs/EAMUSE3.md, "pop'n music 15"):
   - Done: `hiraku-pc` runs HIRAKU's pop'n server (`runtime/python/easerver.cpp`) on a local port with the pop'n games' shared folder; `runtime/pc/net.c` answers libavs's name lookup of `services.eamuse.konami.fun` with it, gives the cabinet its own network card (`GetAdaptersInfo`: 192.168.0.2/24, router 192.168.0.1; under Wine libavs read 128.0.0.0 and kept its network off) and answers the keepalive's ICMP pings on a UDP socket (raw sockets want administrator rights on Windows). The server answers G15 with a keepalive item, `pcbevent`, and the full `facility` libavs imports (`location`, `line/id`, typed `portfw`, `public`, `share`). The title shows OK! and NETWORK OK; NETWORK CHECK all 10/10.
   - Coins online: the game sets coin mode 1 and counts the coin switch (pad bit 22) itself; the DLL turns each coin into a pulse. The reader: a held card is active at every status read; no card reads as closed (state 10), or the card screen can't be skipped. The 10-key edge names keys by the reader's grid number (spice2x's `KEYPAD_KEY_CODE_NUMS`).
   - A new card: `cardmng inquire` (new), `authpass`, `G15 playerdata_expire`, `inquire` with model F14 (a pop'n 14 player on the card?), `G15 playerdata_get model=F14`, `bindmodel`. The G15 module joins type and method (`playerdata_get` for pop'n 14's `type="playerdata" method="get"`).
   - The player (G15 module, `answer_g15`): `playerdata_get/new/set/conv/score/expire` on the game's 0xb38-byte record (its layout read from the loader `0x4819f0`, the set block `0x5dab20` and the NET TAISEN builder `0x4a4dc0`), kept as `pdata-G15-REF`; a pop'n 14 player carried over (`inquire`/`get model="F14"`, then `conv`: the songs' words by the same index); the most played songs and per-chart best scores kept by the server; one card for all with pop'n 9-14 (marks and scores). The ADVENTURE map (`ir_ranking` 2, `<private>`), the expert courses' INTERNET RANKING (`ir_ranking` 1: `rank_exp-entry`/`-get`, 22 courses, and the stage scores to beat in the record), `game_get` and `game_active`. Checked under Wine: new card with name entry, returning card, a pop'n 14 carry-over, an expert course ranked.
   - NET TAISEN (`runtime/pc/vsock.c`): the game's libsocket served over HIRAKU's relay (pop'n 12-14's online relay; hiraku-eamuse's with `-url`, else hiraku-pc's own), the PC lobby on port 5741 with pop'n 15's dialect (participate). Checked: two hiraku-pc cabinets on one hiraku-eamuse matched, battled (with a CPU) and saved their results; `tests/pc/test_vsock.sh` checks vsock.c between two processes (lobby, datagrams, TCP both ways, refusals, timeouts; the game itself uses TCP only in its developer test screens). The battle settings Konami's server held (`job_sp_rate`, `downlimit_*`) follow the game's code and players' descriptions (docs/EAMUSE3.md), settable. The Windows build (hiraku-pc.exe, cross-built) checked under Wine: e-AMUSEMENT, a new card, its relay. Not checked: a real Windows PC, a real board or reader.
   Original plan: pop'n 15 speaks ea3 (`cardmng`, `pcbtracker`, `playerdata_*`) like pop'n 13/14. `runtime/python/easerver.cpp` grows pop'n 15's player data, so one card carries 9–15 (`popn-one-card`).
5. **PC-5, enhancements:**
   - ✓ Shaders and pacing: through HIRAKU's window (5b).
   - ✓ **Latency** (2026-10-22): `frame_go` is given when the shell's frame starts (its pacing's burst), not when the picture is copied: the game reads its controls then (0.1 ms after, p99), draws and reads back (3.5 ms p99 under Wine on the owner's PC) and its picture goes to the next refresh. pop'n 15's own frame clock stood in the way: before each `Present` it waits until 14 ms have passed since the last (`timeGetTime`, `Sleep`), which held the picture to the frame's end; the DLL makes that branch always go on when HIRAKU paces (`unlimit_frames`, found by its shape and the program's import slots: two sites in each of the five program dates). Each picture's `Present` mixes its frame's sound (`audio_frame_end`), so a keysound comes with the frame that read its press (on HIRAKU's sound timeline 0.15 frames after the press's frame, measured by recording a song with and without presses); the core counts 735 sound frames per frame shown and hands a late picture's sound on at half a frame. Measured in a nested 60 Hz session with display sync: controls read to picture handed to the display 4.9 ms (median; a frame more before, about 18), sound breaks over a 3-minute run 14 (28 before), none while playing. `--game-ahead` keeps the old way (a frame's slack to draw). Without a window the core keeps the 60 Hz grid itself (`Options::window`).
   - ✓ **Mods** (2026-10-22, docs/MODS.md "The PC games"): `[[patch]]`es go to the DLL, which puts them in place in the game's process as the program loads and before each picture (read-only pages too); files at their paths under `data/` (and `[[file]]` with `medium = "data"`) are opened instead of the game's; the program's copy of `dat.bin` gets their MD5s, so the boot's data check passes (checked: a test mod's string patch on the boot screen, 39 data files changed, two of them in the boot's checked twentieth, GAMEDATA CHECK OK). A modded save folder (`popn15+mod`) keeps the shared e-AMUSEMENT folder.
5b. ✓ **The picture through HIRAKU's window** (owner's request, before e-AMUSEMENT): `hiraku-pc` is `runtime/main.cpp` with a PC core (`runtime/pc/core.cpp`), so every shell option works as for the emulated cores (window, full screen, `--picture`, `--crt-beam`, display sync, scripts, `--snap-every`). The DLL reads each picture back (`GetRenderTargetData` into system memory) into the link's pixel area and waits for `frame_go`; the core copies it, lets the game draw the next while the shell shows this one (the game a frame ahead of the screen, as on its own monitor), and repeats the last picture while the game draws none (start-up, loading) so the window keeps answering. The game's window stays hidden (`ShowWindow` hooked). Measured under Wine: 60.00 pictures a second from the game with nothing repeated once it runs; display sync on a 60 Hz display at +0.00 % speed. 
5c. ✓ **The sound through HIRAKU** (`runtime/pc/dsound.c`): the game's `DirectSoundCreate8` (pop'n 15 imports it by ordinal, 11) gets HIRAKU's DirectSound when HIRAKU's window shows the game: the device, a primary buffer, secondary PCM buffers (duplicates share their data), volume, pan, frequency, cursors (the write cursor a frame's sound ahead), position notifications; 3D and property sets refused. A mixer thread in the DLL mixes as far as the core asks into the link's ring (`audio_want`/`audio_made`/`audio_taken`); the core asks for 735 frames a frame shown, spread over the frame as it passes, plays them through the shell's `audio_push`, and gives a frame's silence until the game's sound starts. pop'n 15's buffers are all 44.1 kHz 16-bit (178 mono, 63 stereo), so nothing is resampled. Checked: two runs give the same sound sample for sample at the same moment (correlation 1.000); against Wine's own DirectSound, 0.996 where Wine's recording had no dropouts. Breaks only while starting and loading.
6. **PC-6, pop'n 16–18, then 19 onwards** through spice2x, with the same link and DLL hooks loaded beside it.
   - **pop'n 16–18 (started 2026-10-22; `games/popn16`, `popn17`, `popn18`)**: the dumps (H16, I17, J39) are each a folder of programs by date and `data/` (numbered folders: libavs maps the names), plus `prog/_htpec/`: Konami's protected programs, the main folders holding unpacked ones with emptied program lists. The program lists are encrypted from 16 on (`prg.bin`, `dat0..2.bin`: AES-256-CBC, the IV first, ciphertext stealing; inside, pop'n 15's format; the key 32 bytes in the program): `prg.bin` is empty, so HIRAKU's DLLs aren't checked. Their `ezusb.dll`/`libacio.dll` exports are pop'n 15's; the 14 ms frame wait is there twice in every program date (`unlimit_frames` finds them).
     - **pop'n 16, 17 and 18 play with e-AMUSEMENT** (2026-10-22): boot (I/O, security, backup, data, IC card checks OK), go online with HIRAKU's server (the whole `facility` map; pings through `WSASendTo`/`WSARecvFrom`), take coins (libavs's coin events; `ea3_query` is `libavs-win32-ea3.dll`'s from 17 on), read cards in a slot reader (a card in until the game's EJECT, at the mouth, then the reader starting again and asleep a moment: 18's ENTRY, backed out of, waits for SLEEP), and 17's and 18's calls come as binary XML (`runtime/python/kbin.h`). Buttons in play come from `usbPadReadLast`'s samples (a running counter, ten samples newest first). pop'n 16's unpacked program faulted at start-up until a render target Wine refuses in A4R4G4B4 was made in A8R8G8B8 (`CreateTexture`).
     - **The players are kept** (`answer_j39`, docs/EAMUSE3.md): each game copies its record to a global and bounds it field by field (16 `0x447dc0`, 17 `0x440f30`, 18 `0x460320`), so the layout came from there; `set`'s fields are merged where the loader reads them: the option, character, last song and chart, each song's word (marks, played flags), the best scores (16, 17 in the record, chart-major; 18 its `<hiscore>`, 17 bits each), the most played songs, 16's mode and buttons, 18's story map (`private`, the record's last 0x128 bytes). Checked by dumping the game's own copies through a credit (cabinet.c `HK_DBG_REC`: a pointer's or an address's buffer each time it changes) and matching them to what `set` sent. 16's `playerdata.score` answers from the record. Carry-over: `get model=<the game before>` (16 reads 15's whole record, 17 16's first 0x6b0 bytes, 18 17's first 0x72c, unpacked) and `conversion` (marks and scores by song number; checked offline 15 → 16 → 17 → 18, and in pop'n 18 with a pop'n 17 player: shown, carried over, played). A credit played without a card sends `set` with no ref_id (answered, nothing kept).
     - **NET TAISEN (2026-10-22/23)**: 16-18 have pop'n 15's libsocket, libcomm and lobby client built into the program; `vsock.c` finds libsocket's 15 calls by the Winsock function each one alone calls and makes them jumps to its own, so the relay serves them as pop'n 15's. One lobby per game (ports 5742-5744). 18 offers NET対戦 with `netvs_phase` ≥ 1, 17 with `e_amuse_phase` ≥ 4 (default now 14), 16 with `netvs_phase` ≥ 1. Checked under Wine with two cabinets on one hiraku-eamuse (`HK_PC_RELAY` to its relay, each its own server on one data folder): 16, 17 and 18 match and battle; the `<netvs>` results are kept (16: its block at 0x6594, 17: 0xa0c, each with a NET credit count the game resets the block by, so the server counts them and writes the game's defaults first; 18: 0x13c8; docs/EAMUSE3.md) and each game's next battle starts from them. Scripts follow the game through `on "ea:..."` events (oracle/README.md).
     - **One card for all** (2026-10-23): 15-18 join pop'n 9-14's shared clear marks and scores (docs/EAMUSE3.md): their song tables number songs as 14 (found in each program, a song taken out flagged), the marks in each song word's low byte as 12-14's; 16-18 get the card's marks and bests from its other games in `playerdata get` (16 and 17 in their records, 18 in `<hiscore>`), 15 and 16 in `score` too. Checked: the server against a card seeded in 13-18 (out/onecard/test.py), and pop'n 18 holding pop'n 15's medal and score in memory.
     - **More of the player kept** (2026-10-23, docs/EAMUSE3.md table): 16's and 17's `private`, 18's `license` and `norma_point`, 16's and 17's expert courses and extra tickets, `last_play_flag`, and 17's and 18's mode decoded from `play_mode` (18's had been kept as sent, a different number). Found from each game's loader and set builder; checked with made-up calls (tests/eamuse/test_pcrecords.py) and in the games' memory (17's private copy, 18's license table hold what the server put in the record).
     - **Open**: 18's `boss` (a server tally, by the look of it), the expert courses' stage scores in 16's and 17's records (their boards are kept since 2026-10-23: `ranking` entry/get, 18's INTERNET RANKING entries); game.get's phases are guesses (the data folder's `NAME-GAME` overrides each); mods change memory only (the data lists are encrypted and libavs maps numbered folders: file mods need both).
     - **Coins**: the coin switch pulses by time (60 ms on, 60 off) in coin modes 1 and 2 (pop'n 18's test mode sets 2); a pulse of four pad reads was counted twice by pop'n 15 (two coins gave two credits at two coins a credit) and missed by pop'n 18.
     - **Presses (2026-10-23, after popnhax's ideas; none of its code)**: pop'n 16-18 take their buttons in play from the board's samples, a press being a sample with the button after four without it: HIRAKU gave one sample a read, so a release needed four frames and the same button pressed again sooner didn't count (a mash of 3 frames on, 3 off on pop'n 18: 17 presses judged in a song). `press.c` makes the samples from the pad's timed changes (`link.h`: `pad_ev`, each with the controller's own event time, `hk::host::input_time()`) at 360 a second, six a frame as the board's: 569 judged in the same mash. A button down and up between two reads is held down for one read (`taps()`, pop'n 15's `usbPadRead` too).
       Judging: each game reads its pad once a frame, takes its song's time (`timeGetTime`, in 4 ms steps: the time `>> 2`) and, per button pressed, calls one routine that compares the lane's distance to its nearest note (a 16-bit count of steps, a table of 9 lanes, 8 bytes each, the note's pointer before it) with six windows a routine sets to -44, -28, -12, 12, 28, 44 (a chart can change them). Found by that shape in all 13 program dates of 15-18 (one routine, one call each: pop'n 18 2010040500 at 0x4876e0, distances 0x107d1b4); `--press-time` moves the call to `judge_press`, which takes the distance back to when the press happened (the newest going down the game had read, as long ago as that in hiraku-pc's time; 50 ms at most), and `--judge-offset MS` moves every press. Checked under Wine through a song on pop'n 15, 17 and 18 (scripted presses: 11-18 ms old when judged, 3-4 steps), found at boot on 16.
       `-autopin`: the server's `on_card_pin` (a known card with a PIN, at `cardmng.inquire`) has the core type it through `hk::macro::post` (16-18: 4 s later, then Enter; pop'n 14: 3 s, no Enter). Mods' `[[text]]` (docs/MODS.md) finds a string in the program file for the PC games.
     - Bring-up aids in the DLL: `HK_DBG_REC=[=]ADDR:SIZE:DIR[,...]` (a buffer, or each one a pointer holds, written to DIR each time it changes), `HK_PC_CRASHLOG` (a fault's place and the program's return addresses), `HK_PC_AVS_LOG` (libavs's file, package and status calls), `HK_PC_WAITLOG` (where the game waits).

**Open:**
- A program folder whose `prg.bin` lists `ezusb.dll` or `libacio.dll` would fail the program check with HIRAKU's in their place. HIRAKU's code would then come in another way, leaving Konami's files as they are: a stand-in `d3d9.dll` beside the program (a file the list doesn't name), patching the board's and reader's calls in Konami's DLLs as spice2x does.
- To check by the owner: a real Windows PC, the pop'n controller and board, a card reader (the card logic is spice2x's, untested here), the sound's latency.

## 31. The code's structure (2026-10-23)

What was done before more features, as the code had grown (63k hand-written lines):

- **CI on every push to main** (`.github/workflows/release.yml`): both packages and `ctest`, so a fault on one system shows before a release is tagged (v0.4.1's Windows Python build failed on a name Windows' headers take as a macro, `small`). CTest has every test now: kbin, hiraku-eamuse's one card and pop'n 16–18 records, vsock under Wine, mods' `[[text]]`, ingest's Python BIOS and keys.
- **A binary per game only where something of the game's is compiled in.** `runtime/main.cpp` was compiled 163 times (the game id a define); now once (`hiraku_main`), the id a one-line file (`shell/built.h`). The games with no recorded code (all DJ Main, 95 of 108 System 573 and Twinkle, every Python game but `hiraku-python`) are links to their core's binary, which finds the game in its name. `build/cmake`: 11 GB → 7.5 GB, and a change to the shell relinks 44 binaries instead of 176. ccache when installed.
- **The e-AMUSEMENT server in parts** (`runtime/python/easerver*.cpp`): the calls and files (`easerver.cpp`), rankings (`_rank`), GuitarFreaks and DrumMania V (`_gfdm`), pop'n 9–14 (`_popn`), one card for all (`_share`), pop'n 15–18 (`_pc`). The protocol's pieces (MD5, RC4, the XML reader, records) and the online client are `runtime/eamuse/`'s, which had been forked from this server and drifted (the 573's client lacked Windows' connect limit). The two virtual LANs stay apart: the Python board's commands, timeouts and LAN cable are not the 573's socket calls.
- **Shared helpers**: `machine.txt` read by `shell/kvfile.h` (six copies before), `sys::make_dirs`, `copy_file`, `exists` (three), the host ACIO port's packets by one decoder (two). ALSA's output out of `window.cpp` into `host/alsa.cpp`; the launcher's Settings built a tab per function (the same pixels, checked).
- Left as they are: `main()`'s options (a flat list reads as well as a table would), the launcher's command line (one condition per feature). Not possible: regress cases for the PC games (Wine and real time: not bit-exact).

## 32. The 64-bit PC platform (written 2026-10-23)

A loader for 64-bit AVS2 PC games, `runtime/pc64/` (`hiraku-pc64.exe`, built 64-bit by clang for MinGW, in `pc/` beside the 32-bit DLLs), run by `hiraku-pc` as §30's games are: the same link, window, pacing, sound, inputs and e-AMUSEMENT server. A `games/` entry uses it with `[pc] loader = "pc64"`; the launcher lists such games only once unlocked. The working notes are kept outside the repo.

- **Its game's speed:** the game counts 120 frames a second (its logic follows its pictures: drawn 60 a second it ran at half speed). hiraku-pc counts its time in those ticks: on a display under 100 Hz it lets the game draw two pictures for each frame shown (the first neither shown nor read back: the link's `not_shown`, v6), so display sync's grid fits 60 Hz; on 120 and 240 Hz one; on others the core paces itself at 120. Its sound comes 367.5 samples a tick. Checked by the game's own countdown: 5 seconds for every 300 frames shown at 59.87 Hz, 119.7 pictures a second.
- **Its sound:** the loader's WASAPI device plays on its own clock, in whole periods of 2.5 ms (reported by `GetDevicePeriod`; the buffer four periods, 10 ms), into the link's ring, which hiraku-pc takes as it comes. The game mixes in the device's periods and stretches its mix to the space it is given: freed in other amounts (a frame's sound at a time, as hiraku-pc first asked for it) it resampled its music all the time, its high notes a fizzle. Checked by comparing a song through the game with its decoded sound file: a single tap at 65-77 dB, as through Wine's own device.
- **Its controllers** go on while the touch screen's window has the focus (a click there takes it under Wine): SDL's joystick events in the background for these games.
- **One screen, nine buttons** (an older cabinet: one 4:3 screen, nine buttons and a 10-key; `hiraku-pc --single-screen`, step 1 of 3): the main screen's middle 4:3 (1440x1080) scaled on the GPU to 640x480 (two linear steps) is all HIRAKU shows, at 60 Hz with the game at 120 underneath; the touch screen's window is hidden. Button 5 makes the touches the game waits for (title START, entry START without a pass, mode select OK then CREDIT START), and on entering the music select, once a credit, its category list is opened and "by title" chosen (every category but the big buttons' uses the nine buttons). A guest credit played through with SERVICE and button 5 only, on a nine-button chart. Step 2: the play screen rearranged after pop'n 15's 640x480 one (the same sizes at the 4:3 crop's scale: lanes 320 px wide, the gauge 320x45, the COMBO box 110x50; COMBO and the credits at the left column's foot, BPM/HI-SP and the network state at the right's, STAGE under the rival's name; no score or judgement counts during play, as pop'n 15; the win, lost and fever effects on the 4:3 columns' middles, their parts drawn in to the columns' width; the big buttons' pop-kun beside the lanes hidden). In the launcher: Game > One 4:3 screen, nine buttons. Step 3, the touch screen without one: the touch screen shown instead of the main screen (the 10-key's decimal point, or buttons 1 and 9 together), at 640x400 with a cursor (the 10-key's 8/2/4/6 and diagonals, or buttons 4/6/3/7; touching with 5), the buttons kept from the game meanwhile and the view hidden at each new scene: the options, a new player's name and anything else the touch screen asks for. The PIN typed on the 10-key (the game's pad held to its plain order, one of the fourteen it picks from; 00 its clear); a new card's terms on button 5 (YES, then OK), the touch screen then shown for the name. The controls watched on a thread of their own every 2 ms (a tap the game's board reads missed while it was busy is seen). Checked: a new card registered (PIN, terms, name) and a card's full credit to its result written. HI-SPEED on buttons 4 and 6 at the options (held: every 0.2 s), the editor opened first (checked: 1.0 to 2.1 on the main screen's summary). Parked (2026-10-26) for the PC songs' work: the other screens rearranged (music select first; packs extracted locally), the main screen's touch-panel prompts. The game's AFP layouts and GE2D shapes edited in place (placements moved, one-quad shapes cut with their texture), written to copies of its packs in the save folder's `single-screen/`, which hiraku-pc gives the loader (`HK_PC64_OVERLAY`: AVS2's CreateFileW/A opening a file there before the game's). The layout is made from the player's own files by a local tool, never shipped. The characters at 0.8 of their size (the lost move, a close-up, at 0.64), each in the middle of its 4:3 column: the game moves each character's layer every frame (AFP's layer position, imported by ordinal), and for those layers the loader sets the whole matrix instead; the layers are the game's own table's (found by the code that draws them) at the places that table holds. Their windows (the old characters' frames, the songs' pictures' area) scaled and moved the same way in the layout, and the songs' pictures the game sets in them (an AFP clip's texture) fitted to them (afp-core's attach flag) instead of at their own size. Not yet: the result screen's close-ups. Next: the card's PIN and name from the 10-key and buttons.
- **Its shaders:** Wine's HLSL compiler makes `sin()`/`cos()` its `sincos` instruction, which it can't write for ps_2_x, and failed one of the game's effects (some explosions then white squares); the loader retries such a shader with both as a polynomial.

## 33. beatmania IIDX 9th style, the C02 cabinet (written 2026-10-25)

From 9th style (2003) to HAPPY SKY, IIDX is a Windows PC game (Konami's model C02, Windows XP Embedded) with an EZ-USB I/O board, where 1st to 8th were Twinkle's (§21). HIRAKU runs it as it runs the PC pop'n games (§30): Konami's program as it is, under Wine on Linux, with HIRAKU's DLLs for the cabinet around it, and `hiraku-pc` as the cabinet they talk to. 9th style comes first; 10th, RED and HAPPY SKY are the same cabinet and program family.

**The owner's dump** (outside the repo): a `D:` drive's `C02/` with `data/` (4.9 GB, 1218 files, all matching the dump's MD5 list) and eight program folders, `JAA` (2003-06-08) to `JAH` (2003-09-02, the newest), each with `sidcode.txt` (`C02:J:A:x`). The ones the MD5 list names match it. Not in the dump and not needed: `BOOT.exe`, `gamestart.bat`, `select.exe`, `udfilecopy.exe`.

**The program, measured (JAH):**
- **What it calls:**
  - `ezusb.dll`: the C02 board, another API than pop'n's (plain C names, 99 exports): pad, turntables (`usbGetTurnTable`), effect sliders (`usbGetSlide`), the 16-segment ticker (`usb16Seg`), the keys' lamps, LEDs and the top unit's lamps and neon, coins, watchdog, an FPGA its firmware is written to at start-up (`data/hex/030408.hex`), a serial port, the security plug (through `security.dll`) and an H8 with the magnetic card readers and 10-keys (`usbH8*`, `usbCard*`). Every revision's program imports the same 49 calls and every revision's DLL exports the same 99.
  - `sonet.dll`, `sosys.dll`, `avs.dll`, `eam3lib.dll`, `eam3mod.dll`: e-AMUSE3, the 2003 protocol HIRAKU's server already speaks for pop'n 9–14 and GuitarFreaks/DrumMania V (`runtime/eamuse/protocol.h`, docs/EAMUSE3.md). Its modules are `userid`, `userdata`, `numbering` and `package`, the game's model `GEC02`.
  - Windows: Direct3D **8** (`d3d8`), DirectSound (ordinal 11, `DirectSoundCreate8`, as pop'n 15), ACM, DirectShow (the movies).
- **Paths:** `..\data\` from the program folder (`D:\C02\JAx`): `graph` (`.gcz` pictures, "GC" as pop'n 14's), `sd_data` (`.1` charts, `.2dx` sounds, encrypted: `%eNc`), `movie` (`.4`: 157 MPEG-1 system streams, 2.6 GB), `info`, `hex`. Backups `e:\bkeep.dat` and `f:\bkeep.dat`, `settings.bin`; `avs.dll` keeps its NVRAM `avs00000.bin` on E: and F:.
- **Board calls** (from Konami's DLL): all cdecl; 0 is done or fine, 0x60 busy (the game asks again), -33 the board not answering. The pad as `usbPadRead` gives it (active high): player 1's keys 1–7 bits 8–14, player 2's 15–21, the starts 24 and 25, VEFX 26, EFFECT 27, test 28, service 29 (spice2x, GPL-3.0, gives Konami's DLL the same packet); a turntable 0–255 each, the sliders 0–15. `usbCheckAlive` 0 three times in a row is USB I/O ERROR.
- **The security plug** (`0x403100`): `usbGetSecurity`'s 8 bytes must be `G`, any, `C02`, `J`, then `A`, `B` or `C`; `usbBootSecurity("GEC02", 0, 0, 0)` must give 0. Else SECURITY ERROR.
- **A new cabinet's settings**: the operator settings (`settings.bin`, 0xb4c bytes, a checksum at `+0x68`) are loaded at `0x437f10` over the game's defaults (at `0x1916fb8`). None saved yet: the start-up's BACKUP DATA is NG (its flag `0x1892f7c`), and the settings' check (`0x438550`) finds the clock never set (`+0x64` zero; CLOCK SETUP saves it 1): CLOCK ERROR. Both send the game to its operator menu (ALL FACTORY SETTINGS saves the defaults, the clock unset; CLOCK SETUP: the game's own clock, from 2003/05/31 17:00, set with `SetLocalTime`, saved through `0x438080`). An unset shop name (未設定) or prefecture is SHOP NAME / PREFECTURE ERROR (e-AMUSEMENT only). HIRAKU's C02 DLL wraps the start-up's call of the loader: when nothing loads, the defaults it left are saved, the clock set, with the game's own save routine, and the start-up goes on; from then on the settings load as on a cabinet and the operator's changes are kept (the loader, the saver and the call found by shape: one each in all eight revisions; without them, both checks' branches are made to go on, the defaults never saved).
- **The operator menu**: P1 key 2 moves up, key 1 down, key 6 executes.

**What carries over from §30, and what's new:**
- As it is: `hiraku-pc`'s core (window, pacing, link, sound out, scripts), `cabinet.c`'s path map, Escape, mods and crash log, `dsound.c`, `net.c`'s name lookup and network card, the e-AMUSE3 server's protocol, cards (magnetic: `cardtype` 0) and records, the nested-KWin test rig.
- New: `runtime/pc/ezusb_c02.c` (the C02 board); Direct3D 8 in `cabinet.c` (hooks of its own, or a d3d8-to-d3d9 wrapper in front so the Direct3D 9 hooks serve: DXVK's d3d8 or d3d8to9); the movies (Wine plays them through winegstreamer and the host's MPEG-1 decoder; Windows 10 needs an MPEG-1 DirectShow decoder: bemanitools ships CLVSD.ax for it; else HIRAKU decodes them itself); the link's turntables, sliders, ticker and top lamps; IIDX's controls in `hiraku-pc` (the Twinkle family's: turntables as axes, BIO2 on the owner's cabinet); the magnetic card readers and 10-keys through the H8; the server's `GEC02` modules and record.
- Not needed: `libacio.c` (no ACIO reader), `vsock.c` (no battles between cabinets), `press.c` (pop'n's judging; IIDX's would be its own).
- bemanitools (public domain; its iidxhook1 runs 9th to HAPPY SKY by keeping Konami's `ezusb.dll` and answering the USB device beneath it) is a reference for the card unit's and ticker's protocols.

**Steps:**
1. **IIDX-1, the board and a first boot** (started 2026-10-25): `ezusb_c02.c` with all 99 calls, each logged the first times (`HK_PC_USB_LOG`: all), built into `pc/c02/ezusb.dll`; the program run under Wine with it alone (no `hiraku-pc` yet), D:, E: and F: as folders. Checked: the board's start-up, FPGA, watchdog and security pass at once; Direct3D 8 draws under Wine (640x480); the pad reaches the game (`HK_PAD_SCRIPT`: the operator menu moves and opens); with the new cabinet's two checks passed, JAH boots to its attract loop (start-up USB I/O and BACKUP DATA OK, the card readers and network skipped, WARNING, logos, title, the demo play, the rankings). Not yet: the operator settings are never saved (each start has the defaults), the turntables rest at 0, the movies unchecked.
2. **IIDX-2, through `hiraku-pc`** (2026-10-25):
   - **Done:** `games/iidx9` (family `iidx`: Twinkle's IIDX controls, lamps and bindings; `[pc] board = "c02"`, `layout = "revisions"`, `data_as`, `save_drive2`); ingest finds the program folders by revision a few folders down (the last by name is run: the dump keeps JAH apart from the drive's JAG) and the `data` folder beside one. `hiraku-pc` copies the C02 DLL (`bin/pc/c02/ezusb.dll`, in both packages) in place of Konami's, maps `..\data`, E: and F: (`HK_PC_MAP`), and links the data beside the program's copy (a symbolic link; a junction on Windows): Windows' DirectShow opens the movies itself, where the path map doesn't reach, and without it the stages stayed black, waiting for their movie. `runtime/pc/d3d8.c`: the game's Direct3D 8 in a hidden window, its back buffer lockable and read at each Present into the link (Direct3D 8 has no `GetRenderTargetData`), then the game waits for hiraku-pc's frame as the Direct3D 9 games do (`cabinet.c`'s Present steps shared: `hk_present_begin`, `hk_picture_drawn`, `hk_put_pixels`). Link v7: the turntables (`turntable_add`: 1024 a turn, as the BIO2's and Twinkle's), sliders, the ticker (`display_text`), the top lamps, neon and LEDs; the C02 pad bits (`HK_PC_C02_*`). `HK_PC_THREADLOG` for bring-up.
   - **Checked under Wine** (scripts: `--script`, `set "P1 Turntable" N`): the attract loop, coins (2 a credit), the entry, BEGINNER mode, a song chosen and its stage played with its movie and judgements, 60 pictures a second with the game's frame drawn 0.8 ms after its start (no frame wait of its own to take out); the sound through HIRAKU's DirectSound the whole way (gaps only while a stage loads); the operator menu's INPUT CHECK 1 and 2 show each key, Effect, the turntables (64, 200, 128) and sliders as set. The Windows build (`build/win-check`) links.
   - **The operator settings kept** (2026-10-25): a new cabinet's defaults saved by the game's own routine, the clock set (see "A new cabinet's settings" above), as `settings.bin.0` on E: and `.1` on F:. Checked: the first start saves them and goes to the attract loop, the next loads them; FREE PLAY turned on in COIN OPTIONS (SAVE AND EXIT: NOW SAVING) is on at the next start. The operator menu: key 1 down, key 2 up, key 6 executes or changes the value (in CLOCK SETUP, key 1 moves through the clock's fields).
   - **Not yet:** the lamps and ticker on real lights; real Windows (the junction, DirectShow's MPEG-1 decoder).
3. **IIDX-3, the movies** (2026-10-25):
   - **What they are:** 157 MPEG-1 system streams (`data/movie/*.4`), video only, 176x240 or 240x240, 30 fps. The game plays them through the DirectX SDK's "Texture3D" DirectShow sample (its own renderer filter into a texture; its errors say so: "Could not create source filter to graph!"), with whatever MPEG-1 splitter and decoder Windows has. bemanitools needs an extra decoder (CyberLink's CLVSD.ax) only from IIDX 12 on: Windows' own MPEG-1 decoder (quartz) is expected to serve 9th to 11th.
   - **Under Wine:** winegstreamer decodes them (the host's GStreamer: `mpegpsdemux`, `mpeg2dec`); they play once the data is where the game names it (step 2's link).
   - **The Windows path:** the junction was made with `cmd /c mklink /J`, whose quoting `cmd /c` mangles (each argument quoted, a path with spaces): it failed under Wine and would on Windows. Now made by `hiraku-pc` itself (`make_junction`: the folder, then `FSCTL_SET_REPARSE_POINT` with a mount point; no rights needed; remade each start, never over a real folder). Checked with the Windows `hiraku-pc.exe` under Wine: Wine lists `<JUNCTION> data [...]`, a credit plays through the first stage with its movie, a second start remakes it.
   - **To check on a real Windows PC** (the owner's cabinet: Windows 10 IoT LTSC): the movies through Windows' own MPEG-1 decoder (an N edition has none without the Media Feature Pack); the junction to data on another drive.
4. **IIDX-4, e-AMUSEMENT** (started 2026-10-25):
   - **The network** (done): test mode > e-AMUSEMENT OPTIONS > e-AMUSEMENT SETTINGS turns it on (with it on, SHOP NAME and PREFECTURE must be set: the DLL sets HIRAKU's, ＨＩＲＡＫＵ in Tokyo, when unset, at `+0x48` and `+0x46`, saved as the clock is). The library (`sonet`, `sosys`, `avs`, `eam3lib`, `eam3mod`: e-AMUSE3, as pop'n 9's) asks `services.<the PC's domain>`: `net.c` gives the cabinet the domain `eamuse.konami.fun` (`GetNetworkParams`, with `HK_PC_EA_DOMAIN`) so its name lookup answers it, and takes its pings' `WSASocketA`. The board's PCB id (`usbGetPCBID`, 10 bytes) is `-p`'s or `01201000000000C02001` (`HK_PC_PCBID`). Checked: `services.get`, `pcbtracker.alive`, `posevent`, the pings, NETWORK OK on screen.
   - **The card readers** (done): `usbH8IdConfirm` answers 2 units (0: both NG(C) at start-up). A card is 128 bytes as pop'n 9's: a 5-byte header (`0x469740`: 4 bytes encoding the kind and the model "C02", a CRC-8), the number in bytes 6-13, a CRC-16 of 5-0x3e in 0x3f-0x40. Kinds the game takes: `48 23 41 21 ba` registered, `08 23 41 21 5a` new, `08 1f 7d f1 08` Konami's and `08 20 82 01 7c` e-AMUSEMENT's common cards (new); others are refused. HIRAKU's card files (`card.bin`, `card2.bin` in the save folder) start as a new common e-AMUSEMENT card. Calls: `usbCardOpen` / `GetStatus` (3 = a card in) / `Read` / `Write` / `Format` / `Eject`, 128 bytes each, done at once.
   - **The 10-key** (done): PS/2 set 2 scan codes (`0x469630`: the keypad's digits, Enter, Backspace as Clear...), through `usbH8KeyboardGetSize`, `GetSizeDone` (the count) and `ReadData(unit, size class, buffer)`; unit 1 player 1's from hiraku-pc's keypad bits (`HK_PC_KEY_CLEAR` added), unit 2 player 2's (link v8: `keypad2`, and `card_seq2` for player 2's card in unit 2). The family `iidx` has a card and 10-key for each player (`Card`, `Keypad N`; `P2 Card`, `P2 Keypad N`; Insert and Delete the cards, the keypad player 1's 10-key, player 2's unbound).
   - **A new card** (done): the game asks `numbering.assign` (label `cardv1`, format `card16m10`), then on Start: NAME ENTRY (the DJ name), the PIN twice on the 10-key, formats and writes the card, `userdata.write` (`<b>`: the name, 8 bytes; the PIN, 4; then the record, 0xff where unset), then an INFORMATION page (KONAMI ID) and the credit. The server keeps it as `userdata-C02-<card>`; IIDX 9th has its own path in `EamuseServer::answer` (not pop'n 10-14's), its module `C02` answered `SOK` (`sonet` 0x10003840: a status word; anything else is an error the game calls a network error).
   - **A registered card** (done): `userid.ctou` (112, no KONAMI ID: `sonet`'s 20, taken), `userdata.read`, `C02.music getrank CARD 120`, then Start; after each stage `C02.music reg CARD N SCORE ...` (the stage's result), at the end "今回のスコアが正しく保存されました" and `userdata.write` (the record: its play count +1, a time). The fix: a `C02` answer must name what it answers, its `method` attribute the method and the command's first word run together (`musicgetrank`, `weeklygetmid`; `sonet` 0x10004600 matches it against its command table and reads nothing of an answer without it: the status stayed unset and the game called it a network error). The card's states (`0x18f5400` + 8 a player, the state at +4): 1-3 reading, 7 `ctou`, 5 `userdata.read`, 14, 8 `getrank` (12 retry, 19 error), 20 ready for Start.
   - **The scores and rankings** (2026-10-25): the module `C02`'s calls taken apart from `sonet.dll`'s method tables and `bm2dx`'s request sites, answered by `runtime/python/easerver_iidx.cpp` (docs/EAMUSE3.md, "beatmania IIDX 9th style"). `music reg CARD chart song pgreat great djlevel cleared` after each stage: kept per card, song and chart (the best EX, DJ level, cleared, plays; `iidx9-scores`), `bestScore` 1 on a new best. `music getrank` gives them back when the card is read (a child per chart, 4 hex digits a song: song << 5 | DJ level << 2 | cleared << 1: music select's marks). `music getranking`: the most played songs. `weekly getmid`/`entry`: the week's song (`iidx9-weekly-song`, else one of the songs played here, another each week) and each card's best on it. `ranking`: EXPERT mode's courses (12 a class; STYLISH is class 0 course 0): `entry` after a course when the player agrees on the PASSWORD FOR INTERNET RANKING screen (kept but the PIN it carries), `getme` the card's last try per course (course select's 前回のスコア), `getranker` a course's top 20, `getinfo` the six courses attract shows (0-5, or `iidx9-courses`). Checked under Wine with the registered card: its stages kept (`bestScore` 1, then 0 for a worse one), `getranking` listing them; on the next credit music select shows the card's DJ level on the played chart (1P DJ LEVEL F) and the week's song marked WEEKLY, and playing it sends `weekly entry` (in the same call as `reg`), kept.
   - **Seen twice, not understood:** a boot where the game never reached the server ("サーバーへの登録が確認できません", no call at all, the pings answered); the next boot was fine, and four runs since with the network logs on did not show it again.
   - **EXPERT courses** (checked under Wine): STYLISH played to a FAILED stage, はい on the ranking screen: `ranking entry` sent and kept, "今回のスコアが登録されました" and the password shown; on the next credit course select shows STYLISH as FAILED (`getme`) where it said NO PLAY. `getranker` checked on the server alone (attract asks every 32 loops).
   - **Player 2** (checked under Wine): a new card in unit 2 registered on the 2P side (NAME ENTRY "2P PLAYER", the PIN twice on player 2's 10-key, the card written, `userdata.write`), a credit played on the 2P side; the card again: `ctou`, `userdata.read`, `getrank` by its number, the PIN on player 2's 10-key, its stage kept by `music reg`. Both cards in one credit: both read, both INFORMATION pages side by side, a two-player credit to its end and both players' records written (`userdata.write` each). Two players outside BATTLE send no `music reg` at all: the game's own rule (`0x40ebf0`: both players' bits and the battle flag `0x18f44d4` clear skip it).
   - **Not yet:** attract's internet ranking screen seen with rankers, a course for two players, both players' cards in one credit, the record's layout (DJ points), KONAMI ID.
   - Bring-up aids: `HK_PC_AVS_LOG` also logs `sonet_request`/`sonet_finish` (each request's result once); `HK_DBG_REC` on the card states (`0x18f5400`, 8 bytes a player: the state at +4; 20 = waiting for Start); scripts' `on "ea:userdata.read" +N`.
5. Then 10th style, RED and HAPPY SKY (bemanitools' iidxhook1 runs all four on this cabinet).

## M0 steps (all done)

1. ~~`git init` HIRAKU and add `.gitignore` for `dumps/` and `build/`~~ Done.
2. ~~Write the dump ingester~~ Done (`tools/ingest.py`; verified byte-identical from `.cue/.bin` and `.iso`). The ingester: CD image → ISO9660 → `HIKARU.EXE`/`FIREBEAT.EXE` → optional ParaPara wrapper → LZSS unpack → identify by SHA-1 against `games/*/game.toml`.
3. ~~Turn the scratch Ghidra stats script into a manifest exporter~~ Done (`analyze/ghidra.sh` + `ExportManifest.java`: 2,205 functions, 376 code-pointer candidates, 36 orphan-code ranges). It exports: functions, blocks, jump tables, pointer-table candidates → `build/<game>/manifest.json` (generated, not committed).
4. ~~Disassemble and annotate the kernel stub~~ Done: docs/KERNEL.md. Remaining: the kernel's exact extent and the thread/semaphore API.
5. ~~MAME oracle with MMIO taps, shared input scripts and reference traces~~ Done (`oracle/`, see `oracle/README.md`).
   - **Taps** record every hardware access into a folded binary stream.
   - **`.his` input scripts** are frame-keyed, so the runtime will run the same files.
   - **Tools:** `trace.py` (summary/dump) and `compare.py` (first divergence).
   - **Optional modes:** `EXACT_PC=1` for exact PCs and instruction traces; `WAV=1` for audio.
   - **Reference runs:** `bm3final-boot.his` and `bm3final-practice.his` (boot → attract → coins → mode select → practice song, with a fixed key pattern).
   - **Findings:** MAME runs are deterministic except RTC reads seeded from the host clock. The runtime will replay RTC values when compared against a trace.
6. ~~Check SDK sharing on pop'n~~ Done. The redump bundle holds pop'n 4–7 (program CD + DVD each); program CDs are extracted to `DATA/Firebeat-dumps/` and ingested (`games/popn4..7/game.toml`).
   - **Kernel:** the assembly glue is identical in all five programs. The BM3 C kernel matches pop'n 5/6/7 at 115/118 unique functions (`analyze/sigmatch.py`). pop'n 4 has the same glue but a differently compiled C kernel. Details are in docs/KERNEL.md.
   - **Game code** shares about 10% with BM3, and pop'n 6↔7 share 35%.
   - The "SDK sharing" risk in §6 is mostly retired for the kernel. The ATA/GCU/SPU driver libraries are still to be checked the same way once they are labelled in BM3.

## 34. Songs back from the player's music data (written 2026-10-26)

The 64-bit PC pop'n keeps a slot in its music table for every song it ever had, those it left out
marked so (mask 0x80000000: 311 of 2374). Players' Omnimix packs (popnhax's music data: XML with
`<music id>` texts, characters, BPMs and `<chart idx>` files, the songs' packs in `sd/`, their
banners and backgrounds in ifs_layeredfs's `_ifs` folders) fill them in again. After the work of a
friend of HIRAKU's author on the same game (popnMods' native Omnimix: its tables' layouts, the
difficulty bits, characters by name, the old songs' background guard; credited when it ships).
Nothing of a game comes with HIRAKU: the songs are the player's.

- **The mods reach the loader** (`hiraku-pc` passes the launcher's mods as `HK_PC64_DATA`); each
  mod folder is laid over the game's data folder, files it adds too (`runtime/pc64/files.c`: the
  game's CreateFileW/A and FindFirstFile in avs2-core, several layers, the first wins).
- **The songs** (`runtime/pc64/songs.c`): every `.xml` under the mods' folders read before the game
  starts (the first mod's fields win, later files of a mod over earlier ones; Shift-JIS or UTF-8,
  comments out). A song left out that the data describes, with its Normal chart's file there (a mod's
  or the game's), is written into the game's table: texts, characters by name (the pair side by
  side, the one the game's own songs use most), folder, version, categories, levels, holds, charts
  (new entries in the chart table, shared when two are the same), its difficulties' mask bits, the
  unlock bits (0x08020080) cleared. Found by the code that reads the tables, not by addresses.
- **What the game needs besides** (found here): the regions a song is listed for (+120: the game's
  songs' 0x17; 0 in the slots left out, so the music select never listed them: popnMods' Unlock All
  hid this), each chart's tempos (+222, 12 bytes a chart: tempo, share of the song in thousandths,
  twice more), from `display_bpm`'s two groups.
- **Pictures inside the game's packs**: a song's banner (`/ifs_image_kc/kc_NNNN.ifs`), background,
  characters' names and pictures are read one by one through AVS2's open and stat (ordinals 79,
  100); a mod's `tex/system/kc_diff_ifs/kc_NNNN.ifs` (and the other names the packs had) is given
  instead, through a mount of each mod folder (`/hiraku_modN`). Without a banner the game asks for
  it forever (the music select stops).
- **The background guard**: the game waits for an old song's own background forever when it isn't
  there; a song brought back without one in a mod takes the game's way for a song without one
  (a jump at the comparison after the background checks, to a stub with the songs' bitmap).
- Checked (2026-10-26) with a player's Omnimix pack: 225 songs back (261 charts; 13 described
  without their files, left out), song 1 found in the title index, its banner from the pack, its
  Light chart played through to the result at its 110 BPM.
- **Old banners** (the earlier games' are 246x60, this game's 488x118): shown at their size they fill
  a quarter of the frame, the frame's "Loading" around it. The texture call (afp-core ordinal 137:
  the texture's atlas size at +4/+8, its corners as fractions from +12) stretches a texture of that
  size to its clip (its fit flag). (Doubling the picture's size in its texture list instead makes
  the game's decoder assert: the size is the pixels it decodes.)
- **Songs left out by the loader:** no banner anywhere (the game's packs keep some for songs they
  left out: their `kc_NNNN` names are read from `tex/system/kc*.ifs`; else a mod's) — the music
  select would wait for it forever; a character the game hasn't takes the song's other one.
  Music data that says Shift-JIS but is valid UTF-8 is read as UTF-8 (a pack's file is).
- **For players:** `docs/SONGS-BACK.md` (installing a pack in the mods folder, the launcher's Mods…,
  `mod.toml`), and `tools/songsback.py`, which reads the game's program and the packs and says what
  will come back and why not (the loader's rules; checked: the same 226 songs).
- **Not yet:** scores for restored songs checked on HIRAKU's e-AMUSEMENT server (it keeps any song
  number), songs past the table (ids ≥ 2374: custom songs; popnMods' high-ID work is a staged
  experiment), characters the game hasn't, the `_ifs` folders' PNGs (only whole members), By Medal
  and the extra Random entries (By Medal's source is needed).

## 35. The last 32-bit PC pop'n, `popn22.dll` (written 2026-10-26)

pop'n music from 22 to the last game before the 64-bit one (§32) is `popn22.dll`, a DLL on AVS2
started by Konami's `bootstrap.exe`, like the 64-bit game's. Its cabinet is the older one, though:
one screen, the `ezusb.dll` board and `libacio.dll` reader of pop'n 15-18 (§30), Direct3D 9 and
DirectSound. HIRAKU runs it with the parts it already has: §32's way of starting an AVS2 game,
built 32-bit, with §30's DLLs around it. The first game is the last build of the last `popn22.dll`
game (M39, 2025092400).

**The owner's dump** (outside the repo): `modules/` and `prop/` as the cabinet has them (`bootstrap.exe`,
its DRM and licence DLLs `k*.dll`), and `data/` (5209 files, 32 GB): exactly the list in
`prop/filepath.info`. `prop/trustfiles_*.p7s` are Konami's signed lists of the files, their
content encrypted (`bootstrap.exe` decodes it); not read yet.

**The program, measured (2025092400):**
- **Exports:** `dll_entry_init` and `dll_entry_main` only.
- **What it calls:**
  - `ezusb.dll`: the board. All 59 exports have the same names as pop'n 15-18's (`runtime/pc/ezusb.def`).
  - `libacio.dll`: the reader (ICCA). 30 calls; 14 of them aren't in `runtime/pc/libacio.c` yet: felica
    (`ac_io_icca_is_felica`, `get_uid_felica`), `ac_io_icca_crypt_init`, the card dispenser
    (`ac_io_hbhi_req_carddispenser_*`) and an LED strip (`ac_io_hdxs_*`).
  - `libavs-win32.dll` 2.15.8 and `libavs-win32-ea3.dll`: AVS2's exports under other names than the
    64-bit game's 2.17 (`XCd229cc…`, `ea3_boot` = `XE592acd00008c`; spice2x lists them), and
    `avs_boot` takes two heaps (`bootstrap.xml`: 48 MB AVS, 32 MB standard).
  - Windows: Direct3D 9 with `d3dx9_41`, DirectSound, ACM, WINMM, IPHLPAPI. The opening movie is
    Windows Media (`data/mov/op_*.wmv`).
- **e-AMUSEMENT:** the modules `info24`, `player24`, `lobby24` (kbin XML, as pop'n 17-18's).
  `bootstrap.xml`: the security code `GQM39JAA`, the data on the DRM file system (`kdm.dll`,
  `D:/m39/contents`), which the dump already has as plain files.

**Steps:**
1. **The loader** (started 2026-10-26): `runtime/pc32/` (`hiraku-pc32.exe`, clang i686 for MinGW,
   in `pc/` beside the DLLs): AVS2 2.15.8 booted with `prop/avs-config.xml`, the defaults copied
   into `/dev/nvram`, the game's DLL loaded, `dll_entry_init` with the soft ID, `ea3_boot`, then
   `dll_entry_main` (as `runtime/pc64/main.c`, spice2x's order). HIRAKU's `ezusb.dll` and
   `libacio.dll` in the game's search path in place of Konami's; the missing `libacio` calls added.
   Run under Wine alone (no `hiraku-pc` yet) to see how far it boots.
   - **Done (2026-10-26):** the loader reads Konami's `bootstrap.xml` for the DLL, its
     `<component><param>` (`pcb_type` 0), the security code (`GQM39JAA`: model, dest, spec, rev) and
     the release code (the soft ID's ext: the cabinet's `ea3-config.xml` has no `<soft>` or `<id>`).
     `avs-config.xml` as the cabinet's but NVRAM and RAW plain folders in `dev/` (theirs are on
     `D:\m39\contents`), `net/enable_raw` a bool (a u8 in the file: 2.15.8's boot stops at
     `property_psmap_import() failed`). The licence key (`kbt.dll`, `kld.dll`, Konami's talk to the
     dongle) answered as for the 64-bit game: two keys, every file check passed (`bt_get_ikey_status`,
     `bt_fcheck_*`, through the game's `GetModuleHandleA`/`GetProcAddress`); without it 5-1501
     (SECURITY). The reader's device 5 is the card dispenser: none (else DISPENSER CHECKING for a
     minute; pop'n 15-18 only ask for device 1).
   - **Checked under Wine** (the program's folder copied with HIRAKU's DLLs and `hiraku-pc32.exe` in
     it, the data linked, `LC_ALL=ja_JP.UTF-8` for its Shift-JIS text): the boot screen's USB I/O,
     SECURITY, BACKUP DATA, GAME DATA and IC CARD OK in 10 s, then 5-2002 (no center server: step 2).
2. **To the attract loop:** `games/` entry and ingest; the board's start-up and security, the
   reader, the network and `info24`'s boot calls on HIRAKU's server; a new cabinet's first-boot
   errors.
   - **Done (2026-10-26):** `games/popn28` (`[pc] loader = "pc32"`, family `pcpopn`); ingest finds
     the contents folder by `modules/popn22.dll` and the model, the soft ID from `bootstrap.xml`
     (machine.txt: `loader=pc32`, `contents=`). `hiraku-pc` (`init_pc32`) makes the save folder's
     `run/` the game's AVS2 root: `modules/` copied with HIRAKU's `ezusb.dll`, `libacio.dll` and
     `hiraku-pc32.exe` (the program's copy for pop'n 15-18 and this one: `copy_program`), `prop/`
     copied, `data/` a link (a junction on Windows), Japanese (`LC_ALL`), the same link, server and
     pacing as pop'n 15-18. Listed by the launcher only once unlocked, as the 64-bit games.
   - **The cabinet's drives:** the game mounts `e:/` as `/e_drive` (its downloads in `up/`, `tmp/`)
     through AVS2; with no such drive the package loader tried to make its folder once a second for a
     minute, then faulted (popn22.dll+0x137184). The loader mounts D:, E: and F: from the save
     folder's `dev/vfs/drive_<letter>` instead (as spice2x), found at the game's import of
     `avs_fs_mount` by its address (AVS2's calls are imported by ordinal).
   - **The server:** `services` lists `info24` and `player24` for M39 too (the library fails a call to
     a module not listed). The start-up asks `services`, `pcbtracker.alive`, `message.get`,
     `facility.get` (the 64-bit game's answer serves), `pcbevent.put`, `package.list`, then
     `pcb24.error` (a fault it saved from the last run), `pcb24.boot` and `info24.common`; each answered
     `status="0"` with nothing more is enough for NETWORK OK.
   - **Its pictures:** Wine's own Direct3D 9 drew everything after the start-up checks magnified into
     a corner (as the 64-bit game's did); DXVK's 32-bit `d3d9.dll` in `bin/pc/dxvk32/` is copied beside
     the game's modules and loaded instead (`d3d9=n,b`; the Linux package takes it from
     `HK_PACKAGE_DXVK/x32/`).
   - **Checked under Wine** (`hiraku-pc --game popn28 --window`): the start-up checks, then the title
     and attract loop (NEW SONG pages, "e-amusement pass をタッチしてください", NETWORK OK) at 640x480;
     pop'n 15 through the shared code still boots to its title; the Windows `hiraku-pc.exe` links.
   - **Not yet:** `info24.common`'s content (events' phases, rankings, goods: bemaniutils' pop'n 27
     answer is the model, the game's own reader the reference), real Windows.
3. **Through `hiraku-pc`:** pictures, sound, coins, buttons (pop'n 16-18's timed presses, the judging
   routine by shape), the game's own frame wait taken out; a credit played.
   - **Done (2026-10-26):** the DLLs look for the game's own routines in its program, which here is
     `popn22.dll`, not the loader: `hk_program()` (`HK_PC_PROGRAM`, set by `hiraku-pc` from
     machine.txt's `exe`; else the EXE as before) for the frame wait, the judging routine, NET
     TAISEN's socket calls and the crash log. The 14 ms frame wait is pop'n 15's shape and is found.
   - **The board as the game reads it:** `usbPadRead` once a frame; `usbPadReadLast` into a 36-byte
     buffer (`0x10de4288`, the next field at `+0x24`: spice2x's 40 bytes would overrun it) read as
     pop'n 16-18's (a counter, then ten 16-bit samples, newest first, through its button table at
     `0x101ed048`), which HIRAKU's `ezusb.dll` already gives (22 bytes, at the board's 360 a second);
     the coins as the coin switch's pulses (`usbCoinMode` 1 and 2).
   - **Checked under Wine** (`out/` script: two coins, then the red button): guest entry, 3 STAGES for
     2 coins, mode select (the tutorial question; a guest's first credit plays the tutorial as stage 1
     either way), the tutorial stage, the character and music select, a song played with judgements
     and its result ("New Record!", the result's server call), the third stage. Sound the whole way;
     60 pictures a second on display sync, the controls read 0.1 ms into the frame, the picture
     handed 13 ms after its start (DXVK's read-back), 3 % of frames late, all in loads.
   - **Not yet:** `--press-time` and `--judge-offset` (no judging routine of pop'n 16-18's shape: the
     samples go to an input object at `0x101bbe50` (+0x10c the last counter, +0x110 the new samples;
     a counter wrap counts one too few) whose judging is still to find); the lamps on real lights.
4. **Players:** `player24`'s calls and the record's layout from the game's code; scores; one card
   with the other pop'n games.
   - **Done (2026-10-26): a player made, kept and read back** (`runtime/python/easerver_p24.cpp`).
     A new card: `cardmng.inquire` (new), the PIN twice → `authpass`, `bindmodel`, the game's terms
     (YES is button 4, then red), CARD NAME on the 10-key (kana as on a phone; button 4 deletes, 6
     moves on, red ends), confirmed → `player24.new` → the record. A known card: `authpass`, `read`,
     `read_score`. Then `start` (the play id and the common info), `read_option` and `write_music`
     for each stage, at the credit's end `write` and `logout`. The reader is a touch one again (as
     pop'n 15's: the card held 2 s; "take your card" waits for it to leave).
   - **The record's layout from the game:** its answers are read through AVS2's struct reader
     (`property_psmap_import`, imported by ordinal) with 16-byte descriptors (type, flags (1: may be
     left out), struct offset, size or count, name, default; an array's type 0x41 above its values'),
     50 of them in the program. `player24.new`'s and `read`'s answer (`CXrpcPlayerNewMethod`'s vtable
     slot 7) runs nine `C*GameData` handlers in turn (slot 8 each), the first that fails failing the
     call ("receiver returns failure"): account, info, config (with option and custom_cate), item and
     chara_param (any number), customize, netvs (its arrays given: `set_recommend`'s default is broken,
     as in the 64-bit game), course_data and event_p28 (may be left out). The server's field tables
     are the descriptors' fields in their order. `HK_PC32_TRACE=1` logs each struct read and node
     looked up (the loader hooks both at the game's imports), which names the part an answer lacks.
   - **What the server keeps** (`p24-<ref id>`): the name as sent (Shift-JIS), the parts `write`
     sends (account, info, config, option, customize, event2021, event_p28: the burger shop's state),
     each character's friendship (`chara_param`), the play count, the latest ten songs (from the
     stages) and the twenty most played (from the scores); each chart's best and plays
     (`p24s-<ref id>`), shown by the game on its result screen as BEST SCORE.
   - **Checked under Wine:** a new card through registration and a credit; the same card again (its
     free first play, best score shown, three stages, `write`, `logout`); the record with friendship
     and the event read back. Server only (the game's own `write` replayed): the record as kept.
   - **Not yet:** the burger shop's per-shop list (0x10387cc8), items (`item`) and their purchase
     (`buy`), the common info's events (`phase`), course rankings, friends/rivals, carry-over from
     the earlier games (`conversion`), one card with the other pop'n games (this game's song table).
5. **Mods:** a player's data mods (Omnimix packs, ifs_layeredfs folders) laid over the data, as §34.
6. **NET TAISEN** through `lobby24`.
