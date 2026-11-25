# Konami Python 1/2: what the hardware, the media and the reference emulator are

Groundwork for a future HIRAKU core (PLAN.md §23). Written 2026-10-13 from:
- dumps of the Python 1 games;
- PCSX2 Reliquary 1.9.4 source, a GPL-3.0+ PCSX2 fork with Python 1/2 support;
- PS2Recomp (ran-j/PS2Recomp, GPLv3);
- MAME master's `kpython.cpp` and `kpython2.cpp`, fetched 2026-10-13.

Working files live outside the repo, in `Python-work/` beside the HIRAKU checkout (`$HK_PYTHON_WORK`): the sources, the Reliquary build, the survey notes (`notes/`), oracle outputs (`oracle/`) and the decoded boot records (`scratch/`). In the repo: `oracle/pcsx2/` (build and run the oracle), `tools/pythonfs.py` (boot records, from a CHD, raw image or CF card) and `tools/chdread.py` (CHD byte ranges; `tools/build-libchdr.sh` builds its library). How sure each fact is: **[V]** checked against the files or by a run; **[S]** taken from a cited source; **[R]** recalled or inferred.

## 1. The boards

### Python 1 (2002–2005)
- **The PS2 side** is a COH-H31000: an arcade-flavoured PS2 with a 4 MiB BIOS (`COH-H31000.BIN`, ROMVER `0100TZ20000306`, built `20000901-114731` from `/work/arcade`) [V]:
  - EE (R5900, 294.912 MHz) with VU0/VU1 and the GS, and 32 MiB of RAM;
  - IOP (R3000A, the PS1 CPU family) with 2 MiB of RAM, and SPU2;
  - the BIOS carries DECI2, TESTMODE, OSDSYS, ILINK, ILSOCK and XSIO2MAN, so it is a TOOL/arcade-style image [V].
- **The interface board** hangs off the PS2's i.LINK (IEEE 1394) port [S: MAME kpython.cpp]. It has:
  - a Toshiba **TMPR3927** (TX39, a big-endian R3000-class MIPS) with 8 MiB of SDRAM;
  - an NEC D72872 1394 link chip, whose config ROM is `d72872gc.crom` (1 KiB, vendor 0x000679) [V];
  - the **IDE hard disk or the CompactFlash card**. The PS2 reaches game storage only over FireWire, through this board [V: Reliquary, and the boot loader below];
  - the cabinet I/O (JAMMA/JVS, pop'n buttons, DDR EXTIO), DS2430 dongles, an M48T58 timekeeper/BBSRAM, and an ADPCM sound player whose output is mixed with the PS2's [S: Reliquary];
  - an H8/3664 on the filter board, undumped [S: MAME].
- **Its boot ROM `b22a01.u42`** (512 KiB, MBM29F400) is TMPR3927 firmware, not a PS2 BIOS [V]:
  - big-endian MIPS from offset 0, BEV exception vectors, TX39 on-chip registers at 0xFFFF_8000+;
  - it has a short stub, then an LZ-compressed body. Build string `Tue Dec  4 15:13:51 2001`;
  - **MAME's copy is ours with every 16-bit word swapped** (MAME loads it with `ROM_LOAD16_WORD_SWAP` as if it were the PS2 BIOS). Apart from that, the copies differ only in the per-board records at 0xF000–0xF050: the MAC address, and 32-byte game records `Gxx##… <year> JAA`.

### Python 2 (2005–2007)
- A COH-H55000 (SCPH-50000-class) PS2. The I/O board is a USB device (P2IO, H8S/2218) [S: MAME kpython2.cpp].
- The HDD sits on the PS2's own DEV9/ATA and is **DNAS-bound** to the console (i.LINK ID) and the disk (HDD ID) [S: MAME, Reliquary docs].
- **On hand (2026-10-16):** DrumMania V (`drmnvj`, E02:J:A:A) and V2 (`drmnv2j`, revision 2005-11-28) [V]:
  - each a 40,000,000,000-byte PS2 hard disk (APA partitions `__mbr`, `__net`, `__system`, `__sysconf`, `__common`, `PP.KNAC_00001` (the boot program), `PC.KNAC_00001.00672.<code>` (the game, 1 GiB pieces), `...DOWNLOAD`), as CHDs (`chdman createhd -c lzma,zlib`: the default codecs take tens of GB of RAM);
  - the black and white DS2430 dongles (8-byte serial first, Reliquary's "old" layout), the HDD ID (128 bytes) and the i.LINK ID (8 bytes); the same IDs in both sets;
  - the BIOS MAME names, `ps2-0190j-20030822.bin`, from a PS2 BIOS collection (SHA-1 `0ea98a25…`); no console NVRAM: the core's sets use one made from Reliquary's generated NVRAM and a set-up Japanese console's OSD blocks (PLAN.md §23, PY-6).
- **The cabinet's I/O** [V] (GuitarFreaks/DrumMania V): the P2IO's inputs as Reliquary maps them; its four lamp banks (DrumMania: bank 3 bits 0-4 the pads, lit by a 0, 6 START, 7 SELECT; bank 2 bit 0 the spot, lit by a 0, 1 the woofer neon, 2 the speaker neons); on its serial port 0 the 573 GuitarFreaks/DrumMania's EXTIO card unit (magnetic card reader/writer and 10-key on Konami's ACIO bus: sync aa aa aa 55, then aa aa 00 00, then packets), not the ICCA Reliquary puts there. e-AMUSEMENT needs a cabinet address outside 192.168.x.x (PLAN.md §23, PY-6).
- **How they boot** [V]: the BIOS starts the disk's boot program (V: 1.2.0 of 2005-05-11; V2: 2.2.0, which starts a newer 2.2.0 of 2005-11-24 from the disk); it brings up the network, asks a server for updates (these disks name `download.eamuse.konami.fun`), starts the game (`F02-JA 20051128-0`); the game checks the session link to its GuitarFreaks (UDP broadcasts), then attract.
- **Adding one** [V]: from its **`.py2`**, Reliquary's descriptor: a small text file whose `[Game]` section names the disk image (`HddImagePath`, a raw image or a CHD), the disk's ID (`HddIdPath`), the i.LINK ID (`IlinkIdPath`), the console's NVRAM (`NvRamPath`) and the black and white dongles (`DongleBlackPath`, `DongleWhitePath`), paths relative to the file. Drop the `.py2` on the launcher's window, or a folder holding it (at any depth), or give it to `tools/ingest.py`; the game is told by the disk's last Konami game partition (`games/*/game.toml`'s `partition`). The BIOS `ps2-0190j-20030822.bin` and the four MagicGate key files (`civ`, `cks`, `eks`, `kek`) once, as for Python 1. A MAME set and its CHD given without the `.py2` are not enough: ingest names the game and asks for the `.py2` (one beside them is used).

## 2. The catalogue (BEMANI first)

| Board | BEMANI | Others |
|---|---|---|
| Python 1 | pop'n music 9, 10, 11, 12 Iroha, 13 Carnival, 14 FEVER! [V]; Dancing Stage Fusion (Europe) [V] | Winning Eleven Arcade 2002/2003 and PES The Arcade, Dog Station / Deluxe, Perfect Pool, Pool Pocket Fortunes, Baseball Heroes 2005, Battle Climaxx!, Monster Gate Online, Nice Smash!, Paintball Mania, Hawaiian De Golf, R.P.M. Red [S: MAME] |
| Python 2 | DDR SuperNOVA, SuperNOVA 2, Dancing Stage SuperNOVA; GuitarFreaks and DrumMania V, V2, V3 [S: MAME] | Thrill Drive 3, Toy's March 1/2, DANCE 86.4 FUNKY RADIO STATION [S: MAME] |

- GuitarFreaks/DrumMania V4–V8 and pop'n 15 onward moved to the BEMANI PC [S: Wikipedia for GFDM; R for pop'n].
- **MAME:** every Python 1 and Python 2 set is `MACHINE_NO_SOUND | MACHINE_NOT_WORKING`, a skeleton [V]. **There is no MAME oracle for these boards.**

## 3. The collection on hand

| Set | Media | Logical size (data) | Game record | State |
|---|---|---|---|---|
| `popn9` pop'n music 9 (JAB) | HDD `c00_jab.chd` | 20.0 GB (~3 GiB of data, the rest `'O'` fill) | `GQC00` | **boots to attract in Reliquary** [V]. MAME flags its own disk BAD_DUMP; ours has a different SHA1 |
| `dstagef` Dancing Stage Fusion | HDD `dstagef.chd` | 1.23 GB (nearly all data) | (Reliquary's built-in WE2K3 record) | **edited media**, see below |
| `dogstdx` Dog Station Deluxe | HDD `c30f04.chd` | 6.0 GB (~480 MiB) | `GNC30` | the only CHD matching MAME |
| `wswe` Winning Eleven 2002 | CF `c18jaa03.chd` | 128 MB | `GEC18` | |
| `wswe2k3` Winning Eleven 2003 | CF `c27jaa03.chd` | 128 MB | `GEC27` | |
| `popn10` pop'n music 10 (JAA) | HDD `popn10.chd` | | `GQC20`, black dongle `GEC20JAA` | **plays in HIRAKU** (2026-10-13) [V] |
| `popn11` pop'n music 11 (JAA) | HDD `popn11.chd` | | `GQD11`, black dongle `GCD11JAA` | **plays in HIRAKU** [V]; sector 0 is a blank PC MBR (below) |
| `popn12` pop'n music 12 Iroha (JAB) | HDD `popn12.chd` | | `G-E12`, black dongle `GCE12JAB` | **plays in HIRAKU** [V] |
| `popn13` pop'n music 13 Carnival (JAE) | HDD `e13_a04.chd` | | `G-E13`, black dongle `GCE13JAE` | **plays in HIRAKU** [V]; two-stage boot (below) |
| `popn14` pop'n music 14 FEVER! (JAB) | HDD `popn14.chd` | | `G-F14`, black dongle `GNF14JAB` | **plays in HIRAKU** [V]; two-stage boot |

Every set also has: the boot ROM, the config ROM, a DS2430 dongle (40 bytes: 32-byte EEPROM + 8-byte ROM ID), the M48T58 BBSRAM (8 KiB), and the **KN00002 memory-card dongle** (`kn00002.ps2`, 8 MiB with ECC, plus the 8-byte `kn00002.id`), identical in all five sets.

**pop'n music 10–14** (arrived 2026-10-13) [V]:
- The boot ROM, config ROM, internal dongle and `kn00002.id` are byte-identical to pop'n 9's. 10–14 share one `kn00002.ps2`, different from pop'n 9's. The BBSRAM differs per game.
- Each adds an **external "black" DS2430** with the game's ID (`ExternalDonglePath`, `GameId` in the `.py1`). Reliquary takes it generically (two Dallas slots); HIRAKU's ingest and core pass it on.
- **popn11's sector 0 is a blank PC MBR** (x86 boot code, no partitions): the disk was initialised on a PC. The header's copies at 0x200 and 0x400 are intact. `tools/pythonfs.py` takes the first of sector 0, 0x200, 0x400 that starts with the header's 8-byte signature; Reliquary boots it as it is.
- **popn13 and popn14 boot in two stages:** record C is a ~300 KB loader that reads `ee/boot/game.bin` from a file system on the disk (`PythonDiskOpen`, `nr_file`; no file names in the clear) and checks the board (`PCB ERROR`). The game itself is only in RAM once it runs.
- **The IOP side is the same:** the same 20 IOPRP modules in every game; ILINK and MCMAN byte-identical, SUBBOARD 64 bytes longer from pop'n 10 on, small SDK revisions elsewhere.
- **e-AMUSEMENT modules differ** (from the EE programs' strings; 13/14 from their RAM): 10/11 as 9 plus `game`, without `parts`; 12 adds `lobby` and `playerdata`; 13 `cardmng`; 14 `facility`. HIRAKU's server knows pop'n 9's calls.

**`dstagef` is not a clean dump** [V]:
- its boot ROM is 32 bytes short: 32 bytes of 0xFF padding are missing before 0x70000, and its 0xF000 record is exactly the one hard-coded in Reliquary. Inserting 32 × 0xFF at 0x6FFE0 restores MAME's ROM apart from the records. Reliquary rejects the short file and falls back to a generated ROM;
- the first sector of its boot record A reads `"LAPA"` and zeros. The rest of record A matches wswe2k3's.

SHA1s of every file are in `Python-work/notes/media-survey.md` §6. The `keys/` files are listed there by size and hash only.

## 4. Storage format ("PythonFS")

- **HDDs:** no PS2 APA/PFS and no MBR. A Konami boot header sits at LBA 0 [V].
- **CF cards:** an MBR with one FAT16 partition holding a single ~120 MiB file (`GEC18JAA.BIN`, `C27JAA.BIN`). That file has the same layout as an HDD [V].
- **The header** (0x140 bytes, repeated at +0x200 and on some disks +0x400; the copies can differ from sector 0, and on popn11 sector 0 is not a header at all) holds three boot records, each stored twice [V]:

  | Record | Fields | Contents |
  |---|---|---|
  | A @0x20 | big-endian: start, end (256-byte units), 0, 0, compressed size, checksum, 0x80000000, 0x8490 | the **TMPR3927 program** (big-endian MIPS) |
  | B @0xA0 | little-endian: start, end, 0, 0, compressed size, raw size, checksum, 0 | the **IOPRP** image (IOP modules: SIFCMD … SIO2MAN, MCMAN, ILINK, SUBBOARD) |
  | C @0x100 | little-endian: start, copies, 0, compressed size, checksum, load address, entry | the **EE game program**, raw (not ELF): load 0x100000, entry 0x100008 (dogstdx: 0xE00000) |

- **Compression: Konami LZ** [V]. A flag byte is read LSB first: 1 = one literal byte; 0 = a two-byte token `b0 b1`, length `(b0 & 15) + 3`, distance `((b0 >> 4) << 8) | b1`. Bytes before the start of the output read as 0.
- **Checksums:** record A's is the sum of the compressed data as big-endian 16-bit words [V]. B's and C's are something else; the loader `START` computes them (open).
- **Checked against the oracle** [V]: pop'n 9's record C, decoded offline, equals Reliquary's EE RAM at 0x100000 for the first 0xD7480 bytes (the code). Only 329 bytes of data differ further on, the ones the game writes at run time.
- **Program sizes** (decoded) [V]:

  | Set | A (TX39) | B (IOPRP) | C (EE) |
  |---|---|---|---|
  | popn9 | 245,507 | 508,994 | 1,417,310 |
  | dstagef | 215,247 | 490,762 | 2,057,931 |
  | dogstdx | 245,427 | 547,176 | 3,950,431 |
  | wswe | 244,227 | 492,123 | 2,235,861 |
  | wswe2k3 | 232,947 | 489,002 | 3,197,799 |
  | popn10 | 247,651 | 506,911 | 2,580,374 |
  | popn11 | 247,795 | 506,911 | 2,418,339 |
  | popn12 | 247,795 | 506,911 | 2,468,936 |
  | popn13 | 247,795 | 508,671 | 301,416 (the loader) |
  | popn14 | 247,795 | 508,671 | 301,216 (the loader) |

  popn11 and popn12 share records A and B (IOPRP built 2004-01-15); popn13 and popn14 share A with them and B with each other (2005-01-25).

  pop'n 9's EE program has ~13,100 `jal`, ~3,300 `jr`, ~1,300 MMI, ~1,200 COP1 and ~1,100 COP2 instruction words (a raw word count, not a disassembly). For scale, BM3 The Final is 1 MiB of PowerPC with 2,204 functions.
- **No directory structure** (pop'n 9–12): game data sits at fixed offsets known to the EE program [R]. pop'n 13 and 14 have a file system (above). Formats seen [V]:
  - `ADP` streams: 4-bit stereo ADPCM at 22,050 Hz, played by the interface board itself (Winning Eleven has ~35 of them);
  - TIM2 textures; uncompressed IRX modules; one MPEG-PS stream (wswe2k3);
  - `Svag` (KCE Tokyo) on dstagef;
  - pop'n 9: 372 `PM09` containers in 20 MiB slots from 0x20000000, one per song [R].
- **Nothing on the disks is encrypted** [V] for the sets surveyed before pop'n 10. pop'n 13/14's `game.bin` is not checked (their loader holds a long key-like string).
- Tools: `tools/pythonfs.py <chd|image> [outdir]` lists and decodes the three boot records of any set on hand, CF cards included; `tools/chdread.py` reads byte ranges of a CHD without extracting it. `Python-work/tools/ps2mc.py` lists a raw PS2 memory card.

## 5. How a Python 1 game boots [V unless marked]

1. The PS2 BIOS boots `boot.bin` from the KN00002 memory card. It is a **KELF** (application type 7, encrypted). The BIOS authenticates the card (MagicGate, keyed by `kn00002.id`) and the mechacon decrypts the KELF with the arcade KBIT/KC keys. **This is the only encrypted step.**
2. The card also holds a plain `IOPRP` and a plain ELF `START` (278,904 bytes, entry 0x01F00008). `START` is the "PS2AC_Subboard_BootLoader": it carries an i.LINK driver and reads the disk's boot header over FireWire (`ReadBootBlock`, `LoadIOPImage`, `LoadEEBinary`).
3. Record B replaces the IOP modules; record C is loaded to EE RAM and run. Record A is the interface board's own program; the board presumably loads it itself [R].
4. Every disk access, the I/O, the dongles and the BBSRAM then go through FireWire block writes to the interface board.

**What this means for a recompiler:** the game's EE program comes straight off the disk, in plain MIPS, with no decryption. The card boot (KELF, MagicGate) can be emulated as Reliquary does, or skipped by loading records B and C directly, which is a departure to be made only after the faithful path is checked.

## 6. The reference emulator: PCSX2 Reliquary

### What it emulates
- **Python 1** [V: source]:
  - `pcsx2/FireWire/FireWire.cpp` (1,002 lines): the PS2's i.LINK controller;
  - `pcsx2/FireWire/Devices/KonamiPython1.cpp` (3,612 lines): **the interface board, as a command-level HLE**. The TX39 firmware never runs. Commands are FireWire block writes; `offset & 0xfff` selects CF (0x390), ATA (0x3a0), ADPCM (0x0d0), UART (0x100), Dallas (0x120), BBSRAM (0x140), boot ROM (0x150), MAC (0x160), JAMMA (0x0e0/0xf80). Dispatcher: `TryHleKonamiCommand`;
  - disk reads complete after a modelled 24 MiB/s plus 0.5 ms, in IOP cycles, so they are deterministic;
  - the M48T58 is plain SRAM, with no clock;
  - the clock is the PS2's RTC, which Reliquary keeps in Japan time (the manual time plus 9 hours), but pop'n 11-14 read it as UTC and add 9 hours themselves (pop'n 12 `0x1a8468`). HIRAKU and the oracle set it so that the games see 2003-06-01 12:00 JST at start (manual time 2003-05-31 18:00): pop'n 12's NINPOUCHO only runs between 10:55 and 23:05;
  - `pcsx2/ChdHddImage.cpp` (527): CHD disks with a write overlay under `hdd-overlays/`.
- **Python 2:** `pcsx2/USB/usb-python2/` (P2IO, ACIO, card reader, DDR EXTIO, Thrill Drive, Toy's March), the HDD on DEV9 ATA with SCE security.
- **Security:** KELF decryption and MagicGate in `pcsx2/CDVD/CDVD.cpp` and `pcsx2/SIO/Memcard/MemoryCardProtocol.cpp`.
- **Licences:** the Python 1 files are GPL-3.0-or-later; `usb-python2/` has no headers, so only the repo's GPL-3 covers it. Compatible with HIRAKU either way. **Reliquary prints the derived MagicGate keystore to its log, so oracle logs must never be shared or committed.**
- **Size of PCSX2's core** (`pcsx2/`, C/C++ lines): ~365,000 in all; GS ~117,000 (renderers ~84,000), x86 JITs ~43,000, USB ~28,000, DEV9 ~18,000, CDVD ~14,000, SPU2 ~6,600, IPU ~4,400, FireWire ~5,300.

### Building and running it [V]
- `oracle/pcsx2/` (see its README): `build-reliquary.sh` builds it with HIRAKU's hook (`reliquary-oracle.patch`); `run.py` runs a set from pristine media with pinned settings, memory-capped. Sources, build and outputs stay in `Python-work/`.
- The build: system packages plus six small dependencies built locally; 899 steps, 83 s, 1.3 GB peak. The paraLLEl-GS submodule is missing from the zip, so that renderer isn't built.
- PCSX2's core library builds without Qt: `pcsx2-gsrunner` links it with no Qt library.
- **It needs a real window:** Qt's `offscreen` platform has no window system for the GS ("Unknown PNI platform 'offscreen'").
- **pop'n 9 boots to attract:** device checks (CORE BOARD, PCB, HDD, SECURITY), warning, title, How To Play, then the loop again (~100 s).
- **Runs repeat exactly** for a fixed CPU mode, frame-limited or not: EE and IOP RAM hashes identical at all 50 checkpoints over 3,000 frames. JIT and interpreter runs differ from frame 60.
- **Host cost** (pop'n 9 attract, Ryzen 5700X3D, whole process): JITs + software GS 44 % of a core; JITs + Vulkan 34 %; interpreters + software GS 78 % and below real time (~0.75×). Unlimited, the JITs do 3,000 frames in ~7 s.
- **What it has for checking** (from the survey):
  - savestates (zip: EE/IOP RAM, registers, VU, SPU2, FireWire state; not the disk);
  - PINE (Unix socket `$XDG_RUNTIME_DIR/pcsx2.sock.<slot>`): EE RAM reads/writes, save/load state, status. Not IOP RAM, and asynchronous unless paused;
  - GS dumps and `pcsx2-gsrunner`; video capture (with the board's ADPCM);
  - EE/IOP debugger breakpoints (also under the JITs); the interpreters for every CPU (`EmuCore/CPU/Recompiler Enable*=false`), with soft-float available in them.
- **What it lacks** (stock): a per-frame hook (HIRAKU's patch adds RAM hashes and a stop frame), any recording of P1IO/P2IO inputs (PCSX2's input recording covers SIO2 pads only), frame stepping outside the UI, scripting. The rest is PLAN §23, PY-0.

## 7. PS2Recomp, in brief

An experimental static recompiler of PS2 ELFs to C++ (GPLv3; ~100,000 lines with tests). Full assessment in `Python-work/notes/ps2recomp-survey.md`.
- R5900 integer and MMI coverage is good. Bugs seen: BLEZ/BGTZ/BLTZ/BGEZ test only 32 bits; `ADD32_OV` relies on signed overflow; FPU control register 0 reads 0.
- The FPU and VU0 macro mode are IEEE (no PS2 clamping); VU microprograms are interpreted to completion with no pipeline timing.
- The runtime is HLE-first (EE kernel, IOP kernel, SIF, CDVD, memory card), with a host-clock VBlank and no cycle model. It has no DEV9, USB, i.LINK, BIOS or KELF.
- **Verdict:** not usable as HIRAKU's runtime (it can't be checked frame by frame against a reference). Its decode tables and MMI semantics are a useful cross-check for HIRAKU's own EE generator.
