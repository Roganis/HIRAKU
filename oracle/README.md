# MAME oracle

(Konami Python has no MAME driver that runs; its oracle is PCSX2 Reliquary, in `oracle/pcsx2/`.)

MAME (0.289, flatpak `org.mamedev.MAME`) runs the real game from a
deterministic input script. A Lua script records every access the game makes
to the hardware. The HIRAKU runtime will record the same stream in the same
format, so `compare.py` can point at the first place the two disagree.

```bash
oracle/mame.sh bm3final oracle/scripts/bm3final-boot.his        # boot -> attract, ~40 s
oracle/mame.sh bm3final oracle/scripts/bm3final-practice.his    # -> practice song, ~2.5 min
oracle/trace.py summary out/oracle/bm3final/bm3final-practice --frames 5000:12000
oracle/trace.py dump    out/oracle/bm3final/bm3final-practice --group dpram --limit 50
oracle/compare.py out/oracle/bm3final/run-a out/oracle/bm3final/run-b
oracle/align.py   out/oracle/bm3final/bm3final-practice out/run/bm3final   # MAME vs the HIRAKU runtime
```

The runtime writes the same `mmio.bin` format (`hiraku-<game> --trace`).

`audiosync.py HIRAKU.wav MAME.wav` checks that music keeps time where the two can't run in lockstep (Firebeat): the overall offset, then the lag, correlation and level in 2 s windows (PLAN.md §18).

`align.py` compares MAME with the runtime:
- **Start point:** both streams are cut at the first UART access, because MAME's trace includes the BIOS and HIRAKU starts at the game's entry.
- **Interrupt noise:** the status LEDs and the VBlank handler's accesses are dropped, since their position depends on interrupt timing.
- **RTC:** read data is ignored.

Runs land in `out/oracle/<game>/<run>/` (gitignored). Each run starts with an empty NVRAM directory, so the machine boots from the ROM set's default NVRAM/RTC image.

## Setup

- MAME needs the ROM set zip plus CHDs in `HIRAKU_ROMS` (default `~/bm3mame/roms`), e.g. `bm3final.zip` and `bm3final/gcc01jca01.chd` (CD) and `gcc01jca02.chd` (HDD).
- `chdman` is inside the flatpak:
  `flatpak run --command=chdman org.mamedev.MAME createcd -i cd.cue -o gcc01jca01.chd`
- A CHECKSUM warning on a re-ripped CD is expected and harmless.
- The machine for the GCC01 disc is **`bm3final`**. Booting it as `bm3` fails with E801.

| Variable | Effect |
|---|---|
| `HIRAKU_ROMS` | rompath |
| `HIRAKU_TAPS` | MMIO groups to record (`gcu,dpram,ata,io,uart,rtc,ext,flash`; default all) |
| `HIRAKU_FOLD=0` | disable folding (one record per access; ~60× bigger) |
| `RUN_SECONDS` | emulated-time cap (default 900) |
| `EXACT_PC=1` | run under `-debug -debugger none`. PCs become exact and `itrace` works, at about 6× slower |
| `WAV=1` | also write `audio.wav` (48 kHz, 3 channels) |
| `WINDOW=1` | window and sound instead of headless/unthrottled |
| `HIRAKU_LUA=file.lua` | one more Lua file run with the script: a one-off probe (taps, `trace`, prints), without editing `hiraku.lua` |
| `HIRAKU_NVRAM=dir` | start from a HIRAKU `--nvram` directory (Firebeat: `rtc.nv` → `rtc`, `flash0/1/2.bin` → `flash_main`/`flash_snd1`/`flash_snd2`), so both sides boot from the same state |

## Input scripts (`.his`)

One directive per line; `#` starts a comment; quote names with spaces.

```
game bm3final                         # informational
watch <name> u8|u16|u32 <addr>        # value logged every frame in frames.txt
hash  <name> <lo> <hi>                # FNV-1a of RAM [lo,hi) every frame
at <frame> <action>
every <n> <action>                    # from frame 0
from <frame> every <n> <action>
on "<event>"[#k] +<n> <action>        # HIRAKU only: n frames after the event (its k-th time, default 1st)
```

Events (`on`) come from the runtime, not MAME: hiraku-pc names each e-AMUSEMENT call as it reaches
its server, `ea:MODULE.METHOD` (`ea:cardmng.inquire`, `ea:playerdata.get`...), so a PC game's
script can follow the game's own pace (its boot takes as long as its disk and network allow); the
run's `log.txt` lists each event with its frame.

Actions:
- `press "<field>" [frames=8]`
- `hold "<field>"` / `release "<field>"`
- `set "<field>" <value>` (analog, e.g. turntable)

In MAME a button pressed at frame F is first seen at frame F+1. MAME latches digital inputs in its frame update, which runs before this script's frame callback. Analog values are read live. The HIRAKU shell copies this for cores that run in lockstep with MAME (`Core::script_button_delay()`: DJ Main).

A field's name is `PORT/Field` or just `Field` when unique: beatmania's turntables are `TT1/Dial` and `TT2/Dial`. A wrong name stops the run, with the port's fields in the error.
- `snap`
- `dump <name> <lo> <hi>`
- `poke <addr> <byte>...`: write guest memory before the frame runs, bytes in memory order (as `dump` reads them). For a game's own debug or demo switches: `kbm-autoplay.his` sets Keyboardmania's autoplay flag.
- `mark <text>`
- `itrace on|off` (MAME debugger instruction trace, `EXACT_PC=1` only; about 29 MB per frame)
- `quit`

Fields are MAME input names (`"Coin 1"`, `"P1 Button 3"`). Use `PORT/name` when a name is ambiguous, e.g. `TURNTABLE_P1/Turntable`.

The runtime will execute the same files, so every input is keyed to a frame (or, HIRAKU's `on`, to an event) and never to wall-clock time.

## Outputs

| File | Content |
|---|---|
| `mmio.bin` | folded MMIO stream, 32-byte records; layout in `trace.py` |
| `frames.txt` | per frame: emulated time, watches, hashes |
| `log.txt` | what the script did, frame by frame |
| `snap/<set>/NNNN.png` | snapshots |
| `dump-*.bin`, `itrace-f*.txt`, `audio.wav` | as requested |

**Folding.** Consecutive accesses with the same (kind, group, address, byte mask) become one record with a count. If their data varied (ATA sector reads, VRAM uploads), the record carries an FNV-1a hash of the data sequence instead (shown as `#`). The PC is deliberately left out of the key, so a run with approximate PCs folds exactly like one with exact PCs. One minute of boot is 39.5M accesses in 642k records (20 MB).

**Addresses.** Addresses are as MAME's taps see them: aligned to the 32-bit bus word, with the accessed byte lanes in `mask`. For example, an `lhz 0x7fe0000e` shows up as `addr 7fe0000c, mask 0000ffff`.

## What has been established (bm3final, 2026-10-11)

- **Determinism.** The same script gives identical frames, RAM hashes and MMIO streams, except for RTC clock reads, which MAME seeds from the host clock (5 of 1.49M records in the practice run). `compare.py` ignores RTC read data unless `--strict-rtc`.
- **PCs.** Without `EXACT_PC`, MAME's PPC recompiler reports an approximate PC: block-granular, sometimes the instruction *after* the access (e.g. `0x800c2198` for an `lhz` at `0x800c2194`). With `EXACT_PC=1` the PCs are exact.
- **Timeline of `bm3final-practice.his`:**

  | Frame | Event |
  |---|---|
  | ~1000 | BIOS → `HIKARU.EXE` |
  | ~1900 | STARTUP self-test |
  | ~3000 | attract |
  | 3600 | coins |
  | ~4000 | mode select |
  | ~4400 | sound select |
  | ~4800 | practice stage starts (bar counter `0x80103674` advancing) |
- **Volume.** Boot is dominated by ATA PIO data reads (33M) and GCU VRAM uploads (5.8M). In-song there are about 440 accesses per frame, mostly GCU FIFO writes, plus about 0.3 SPU mailbox writes per frame (keysound commands).

## Patched MAME for departures

Where HIRAKU departs from MAME on purpose, a MAME patched the same way keeps the lockstep check
going past the point where stock MAME differs. The patches apply to MAME at HIRAKU's pin
(58f90f43) and run with `MAME_BIN=<that build>`:

- `mame-patches/twinkle-sio1-cts.patch` (beatmania IIDX 1st style, PLAN.md section 21, TW-5):
  SIO1 as windyfairy's UART (`sio_uart.*`, as HIRAKU has it) with the DVD player's CTS, which
  the game's DVD check needs (stock MAME: E600 DVD INIT ERROR). Build the `twinkle` subtarget:
  `git worktree add ../mame-tw5 58f90f43 && cd ../mame-tw5 && git apply .../twinkle-sio1-cts.patch`,
  then `make SUBTARGET=twinkle SOURCES=src/mame/konami/twinkle.cpp` (as for the stock Twinkle oracle).

- `mame-patches/firebeat-fdc.patch` (beatmania III's floppy drive, `runtime/dev/fdc.h`): the
  floppy controller's FIFO as the chip's data sheet has it (a read asks for service at
  16 - threshold bytes, the non-DMA status bit stays set through the execution phase, the result
  comes once the FIFO is empty), and its interrupt passed on by the extend board as a level.
  Stock MAME is what `--mame-fdc` matches. Build the `firebeat` subtarget of the pin:
  `make SUBTARGET=firebeat SOURCES=src/mame/konami/firebeat.cpp` (the FDC of 0.289 is the same
  for these games; the flatpak works for the stock comparison).

## The floppy controller's traffic (beatmania III)

`lua/fdc.lua` logs every access to the controller under MAME, and the runtime writes the same
log with `HK_FDC_LOG=FILE`; `fdc.py` reads a log as commands and compares two.

```bash
cp blank.img out/oracle/disk-mame.img        # a blank formatted disk (the runtime makes one: --floppy NEW.img)
HIRAKU_LUA=oracle/lua/fdc.lua MAME_BIN=<patched firebeat> MAME_ARGS="-flop $PWD/out/oracle/disk-mame.img" \
    HIRAKU_TAPS=none oracle/mame.sh bm3final oracle/scripts/bm3final-floppy.his
head -1 out/oracle/bm3final/bm3final-floppy/fdc.txt        # "rtc 2026-11-11 12:52:52": MAME's clock chip at the start
cp blank.img out/oracle/disk-hiraku.img
HK_RTC="2026-11-11 12:52:52" HK_FDC_LOG=out/oracle/hiraku-fdc.txt build/cmake/hiraku-bm3final --frames 16200 \
    --script oracle/scripts/bm3final-floppy.his --floppy out/oracle/disk-hiraku.img
oracle/fdc.py compare out/oracle/bm3final/bm3final-floppy/fdc.txt out/oracle/hiraku-fdc.txt
```

- The disk image must be outside the folder of `HIRAKU_LUA` (the flatpak gets that one read-only).
- With the patch and HIRAKU's default, the two send the same commands and get the same results
  (2026-11-11: 315 lines each, none different). Stock MAME against `--mame-fdc`: the same
  commands, but the tries that fail (FIFO data taken for a result, overruns) fall elsewhere.
- What a game writes depends on the clock: the volume's serial number and the files' times
  (the same in both with `HK_RTC` set to MAME's clock, the log's first line: the chip's own
  reading, which can be a second or two before the host's time when the script starts), and
  the scrambling of its `BM3x.DAT`, which differs between any two runs that aren't
  cycle-identical. The two disks then differ in that file's bytes alone (2026-11-11); beyond
  that they compare by each machine loading the other's disk (FD OK).

MAME runs the autoboot script again after every machine reset (a game resetting itself through
its watchdog, as the 1st style does after test mode); `lua/hiraku.lua` arms only once.
