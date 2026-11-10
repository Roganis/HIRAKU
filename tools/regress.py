#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bit-exactness and speed checks for the runtime.

  tools/regress.py record [CASE...]   run the cases, keep their digests as the baseline
  tools/regress.py check  [CASE...]   run again, compare with the baseline (frames, sound, run statistics)
  tools/regress.py bench  [CASE...]   time each case (no digest), one at a time

A case is a game, a frame count and optional runtime arguments. Each run writes a
per-frame digest (--digest): a hash of the picture and a running hash of the
sound. A change that is meant to be invisible (a faster renderer, idle skipping,
cheaper fibers) must leave every line identical. Baselines live in
out/regress/baseline/ (out/ is never committed: the digests derive from the game).

A case that comes after another starts from the NVRAM that one left. It starts at once from the
copy kept from the last run (out/regress/cache/, a few GB with the 573's hard disks), and when
its parent ends, the parent's new NVRAM is checked against that copy: the same, and the result
stands; else the case runs again from the new one ("run again"). The runs are deterministic, so
a chain's cases mostly run side by side. HK_REGRESS_SPECULATE=0: each case waits for its parent.
The longest ways run first, by the last run's times, the critical one at normal priority and the
rest lower (HK_REGRESS_JOBS: how many at once, default half the CPUs). Each case is reported as
it finishes, with its wall time; at the end the run's time and critical path, and
out/regress/times.txt with each case's start, end and wall time.
"""
import concurrent.futures as cf
import hashlib
import os
import re
import resource
import shutil
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("HK_REGRESS_BIN") or os.path.join(ROOT, "build", "cmake")   # HK_REGRESS_BIN: another build
# HK_REGRESS_OUT: another output folder (its baseline/ can be a link to out/regress/baseline: a
# check of another build, out of the way of runs in out/regress)
OUT = os.environ.get("HK_REGRESS_OUT") or os.path.join(ROOT, "out", "regress")

# name: (game, frames, extra arguments[, after]). A case with an `after` starts from the NVRAM
# that case left (a power cycle between them: ddr3mj's install, then the game); the two run in
# that order.
CASES = {
    "bm3final-practice": ("bm3final", 12000, ["--script", "oracle/scripts/bm3final-practice.his"]),
    # the other four beatmania III (M6): boot from a blank NVRAM, attract, coins, mode and sound select,
    # the practice stage played with keys 1-5 in turn
    "bm3-play": ("bm3", 9600, ["--script", "oracle/scripts/bm3-play.his"]),
    "bm3core-play": ("bm3core", 9600, ["--script", "oracle/scripts/bm3core-play.his"]),
    "bm36th-play": ("bm36th", 9600, ["--script", "oracle/scripts/bm36th-play.his"]),
    "bm37th-play": ("bm37th", 9600, ["--script", "oracle/scripts/bm37th-play.his"]),
    "popn7-attract": ("popn7", 3600, []),
    # pop'n music Mickey Tunes! from a blank NVRAM: the boot checks, the warm-up, attract to How to Play
    "popnmt2-attract": ("popnmt2", 9000, ["--script", "oracle/scripts/popnmt2-attract.his"]),
    "ppp-attract": ("ppp", 3600, []),
    "kbm-attract": ("kbm", 3600, []),
    # DJ Main: power-on, self-test, disk load, attract (in lockstep with MAME, PLAN.md §17)
    "bm1stmix-boot": ("bm1stmix", 9000, ["--script", "oracle/scripts/bm1stmix-boot.his"]),
    "popn1j-boot": ("popn1j", 9000, ["--script", "oracle/scripts/popn1j-boot.his"]),
    "bm1stmix-play": ("bm1stmix", 12600, ["--script", "oracle/scripts/bm1stmix-play.his"]),
    "popn1j-play": ("popn1j", 13500, ["--script", "oracle/scripts/popn1j-play.his"]),
    # the later disks' own geometries and the 68020's rotates (2026-10-16): Club MIX (255 heads),
    # complete MIX 2 (15 heads, ATA passwords), pop'n stage EX (32 sectors a track)
    "bmclubmx-boot": ("bmclubmx", 9000, ["--script", "oracle/scripts/bmclubmx-boot.his"]),
    "bmcompm2-boot": ("bmcompm2", 9000, ["--script", "oracle/scripts/bmcompm2-boot.his"]),
    "popnstex-boot": ("popnstex", 9000, ["--script", "oracle/scripts/popnstex-boot.his"]),
    # System 573: power-on, BIOS, CD boot, the game's checks, ERROR 147 (no install yet);
    # in lockstep with MAME at the pin (PLAN.md section 19). The clock is fixed.
    "ddr3mj-boot": ("ddr3mj", 3600, ["--rtc", "1791804639", "--script", "oracle/scripts/ddr3mj-boot.his"]),
    # ... and its first boot with the install cassette: the CD to the flash and the PC Card, INFO 506
    "ddr3mj-install": ("ddr3mj", 16200, ["--cassette", "install", "--rtc", "1791878546",
                                         "--script", "oracle/scripts/ddr3mj-install.his"]),
    # ... then with the game cassette, from what the install left: attract, with the MP3 music
    "ddr3mj-attract": ("ddr3mj", 9000, ["--rtc", "1791880031", "--script", "oracle/scripts/ddr3mj-attract.his"],
                       "ddr3mj-install"),
    # ... and a game from there: XANADU stepped through to its result, then stage 2 (with its MP3)
    "ddr3mj-play": ("ddr3mj", 16000, ["--rtc", "1791880871", "--script", "oracle/scripts/ddr3mj-play.his"],
                    "ddr3mj-install"),
    # DDRMAX2 and DDR EXTREME (digital board, ZS01 cassette, 32 MiB PC Card in slot 2): the first boot
    # installs the CD to flash and the PC Card by itself (INFO 508); then from what it left, NOTE 605
    # (Test), attract, coins, the selects' defaults, the first stage stepped through to its result
    "ddrmax2-install": ("ddrmax2", 22800, ["--rtc", "1792188271", "--script", "oracle/scripts/ddrmax2-install.his"]),
    "ddrmax2-play": ("ddrmax2", 18000, ["--rtc", "1792188682", "--script", "oracle/scripts/ddrmax2-play.his"],
                     "ddrmax2-install"),
    "ddrextrm-install": ("ddrextrm", 22800, ["--rtc", "1792187739", "--script", "oracle/scripts/ddrextrm-install.his"]),
    "ddrextrm-play": ("ddrextrm", 18000, ["--rtc", "1792188053", "--script", "oracle/scripts/ddrextrm-play.his"],
                      "ddrextrm-install"),
    # DrumMania 10th Mix (digital board): the first boot installs the CD to flash (INFO 508); then from
    # what it left, NOTE 605 (Test), the boot checks, and "Please call attendant": the multi-session
    # unit, a stub in MAME, is as far as MAME goes
    "drmn10m-install": ("drmn10m", 8400, ["--rtc", "1791883090", "--script", "oracle/scripts/drmn10m-install.his"]),
    "drmn10m-boot": ("drmn10m", 10800, ["--rtc", "1791883869", "--script", "oracle/scripts/drmn10m-boot.his"],
                     "drmn10m-install"),
    "drmn10m-play": ("drmn10m", 13000, ["--rtc", "1791884700", "--script", "oracle/scripts/drmn10m-play.his"],
                     "drmn10m-boot"),
    "drmn10m-hdd": ("drmn10m", 70200, ["--rtc", "1791884700", "--cd", "hdd", "--script", "oracle/scripts/drmn10m-hdd.his"],
                    "drmn10m-boot"),
    "drmn10m-npu": ("drmn10m", 14400, ["--rtc", "1791889000", "--npu", "--script", "oracle/scripts/drmn10m-npu.his"],
                    "drmn10m-hdd"),
    # e-AMUSEMENT (PLAN.md section 19): turned ON in test mode; then a boot on the virtual LAN and the
    # server in the core, and test mode's NETWORK CHECK (CONNECTED)
    "drmn10m-eaon": ("drmn10m", 14860, ["--rtc", "1791890000", "--npu", "--script", "oracle/scripts/drmn10m-eaon.his"],
                     "drmn10m-npu"),
    "drmn10m-net": ("drmn10m", 15600, ["--rtc", "1791891000", "--npu", "--script", "oracle/scripts/drmn10m-net.his"],
                    "drmn10m-eaon"),
    # the card unit (step 3): a new card registered and a game played with it; then the card again
    "drmn10m-card": ("drmn10m", 24000, ["--rtc", "1791892000", "--npu", "--script", "oracle/scripts/drmn10m-card.his"],
                     "drmn10m-net"),
    "drmn10m-card2": ("drmn10m", 15400, ["--rtc", "1791893000", "--npu", "--script", "oracle/scripts/drmn10m-card2.his"],
                      "drmn10m-card"),
    "gtfrk11m-install": ("gtfrk11m", 28800, ["--rtc", "1791884700", "--script", "oracle/scripts/gtfrk11m-install.his"]),
    "gtfrk11m-boot": ("gtfrk11m", 10800, ["--rtc", "1791885700", "--script", "oracle/scripts/gtfrk11m-boot.his"],
                      "gtfrk11m-install"),
    # ... and a game from there: two coins, Start through the session WELCOME (alone), the mode and
    # music select, the first stage (ルージュの伝言) played with no input to GAME OVER, attract
    "gtfrk11m-play": ("gtfrk11m", 19800, ["--rtc", "1791886700", "--script", "oracle/scripts/gtfrk11m-play.his"],
                      "gtfrk11m-boot"),
    # ... its network unit: the data install, e-AMUSEMENT ON, and NETWORK CHECK (as drmn10m's)
    "gtfrk11m-hdd": ("gtfrk11m", 60000, ["--rtc", "1791892000", "--cd", "hdd", "--script", "oracle/scripts/gtfrk11m-hdd.his"],
                     "gtfrk11m-boot"),
    "gtfrk11m-eaon": ("gtfrk11m", 14310, ["--rtc", "1791893000", "--npu", "--script", "oracle/scripts/gtfrk11m-eaon.his"],
                      "gtfrk11m-hdd"),
    "gtfrk11m-net": ("gtfrk11m", 15000, ["--rtc", "1791894000", "--npu", "--script", "oracle/scripts/gtfrk11m-net.his"],
                     "gtfrk11m-eaon"),
    # session play: two processes on one session cable (runtime/sys573/link.h), each case naming
    # its partner (run together, in parallel chains)
    "drmn10m-session": ("drmn10m", 16800, ["--rtc", "1791884700", "--script", "oracle/scripts/drmn10m-session.his",
                                           "--session-host", "out/regress/work/session.sock"], "drmn10m-boot", "gtfrk11m-session"),
    "gtfrk11m-session": ("gtfrk11m", 16800, ["--rtc", "1791884700", "--script", "oracle/scripts/gtfrk11m-session.his",
                                             "--session-join", "out/regress/work/session.sock"], "gtfrk11m-boot", "drmn10m-session"),
    # GuitarFreaks 5th Mix (digital board, 32 MiB PC Card): NOTE 604 (Test), the install to INFO 508; then a
    # game from what it left, the first stage with neck chords and picks (out/oracle/gtrfrk5m/gtrfrk5m-*)
    "gtrfrk5m-install": ("gtrfrk5m", 18000, ["--rtc", "1792220193", "--script", "oracle/scripts/gtrfrk5m-install.his"]),
    "gtrfrk5m-play": ("gtrfrk5m", 16200, ["--rtc", "1792223558", "--script", "oracle/scripts/gtrfrk5m-play.his"],
                      "gtrfrk5m-install"),
    # DrumMania 4th Mix (the first with the multi-session unit): NOTE 604 (Test), the install to INFO 508
    "drmn4m-install": ("drmn4m", 10200, ["--rtc", "1792220906", "--script", "oracle/scripts/drmn4m-install.his"]),
    # ... and the two in session from what their installs left: GuitarFreaks 5th joins DrumMania 4th's
    # session and both play the first stage (their own cable, so this pair can run beside 10th x 11th)
    "drmn4m-session": ("drmn4m", 16200, ["--rtc", "1792224530", "--script", "oracle/scripts/drmn4m-session.his",
                                         "--session-host", "out/regress/work/session-gf5dm4.sock"], "drmn4m-install", "gtrfrk5m-session"),
    "gtrfrk5m-session": ("gtrfrk5m", 16200, ["--rtc", "1792224530", "--script", "oracle/scripts/gtrfrk5m-session.his",
                                             "--session-join", "out/regress/work/session-gf5dm4.sock"], "gtrfrk5m-install", "drmn4m-session"),
    # Before the multi-session unit: DrumMania 3rd and GuitarFreaks 4th, each installed with its install
    # cassette (NOTE 604: Test), then in session SIO1 to SIO1 from what the installs left (the session
    # entry, the caution screen and the music select, the first stage played on both cabinets)
    "drmn3m-install": ("drmn3m", 24000, ["--rtc", "1792218444", "--cassette", "install", "--script", "oracle/scripts/drmn3m-install.his"]),
    "gtrfrk4m-install": ("gtrfrk4m", 24000, ["--rtc", "1792218276", "--cassette", "install", "--script", "oracle/scripts/gtrfrk4m-install.his"]),
    "drmn3m-session": ("drmn3m", 12000, ["--rtc", "1792200000", "--script", "oracle/scripts/drmn3m-session.his",
                                         "--session-host", "out/regress/work/session-gf4dm3.sock"], "drmn3m-install", "gtrfrk4m-session"),
    "gtrfrk4m-session": ("gtrfrk4m", 12000, ["--rtc", "1792200000", "--script", "oracle/scripts/gtrfrk4m-session.his",
                                             "--session-join", "out/regress/work/session-gf4dm3.sock"], "gtrfrk4m-install", "drmn3m-session"),
    # the analog board: first boot, the flash install (two Test presses), attract with CD audio
    "ddr2m-boot": ("ddr2m", 24000, ["--rtc", "1791828936", "--script", "oracle/scripts/ddr2m-boot.his"]),
    # ... and a game: coins, mode and song select, a song stepped through to CLEARED, stage 2
    "ddr2m-play": ("ddr2m", 21000, ["--rtc", "1791837575", "--script", "oracle/scripts/ddr2m-play.his"]),
    # DDR 1st (Dancing Stage, analog board): the first boot's flash install (the disc's pregaps, as
    # MAME lays them out), then a song from what it left, in lockstep with MAME
    "dstage-install": ("dstage", 12000, ["--rtc", "1792180437", "--script", "oracle/scripts/dstage-install.his"]),
    "dstage-play": ("dstage", 16000, ["--rtc", "1792182015", "--script", "oracle/scripts/dstage-play.his"],
                    "dstage-install"),
    # GuitarFreaks 2nd Mix (analog board, X76F100 cassette, 32 MiB PC Card): the first boot's install,
    # Test at the flash prompt, the CD to the flash and the PC Card (out/oracle/gtrfrk2m/gtrfrk2m-install)
    "gtrfrk2m-install": ("gtrfrk2m", 21000, ["--rtc", "1792179133", "--script", "oracle/scripts/gtrfrk2m-install.his"]),
    # ... then a game from what it left: coins, Start, mode and music select, the first stage played with
    # the guitar map (neck chords, picks, wails) to GAME OVER, attract (out/oracle/gtrfrk2m/gtrfrk2m-play)
    "gtrfrk2m-play": ("gtrfrk2m", 11400, ["--rtc", "1792181584", "--script", "oracle/scripts/gtrfrk2m-play.his"],
                      "gtrfrk2m-install"),
    # Twinkle (PLAN.md section 21): beatmania IIDX 3rd style's first power-on, E150, 1P Start,
    # attract; in lockstep with MAME at the pin (out/oracle/bmiidx3/boot)
    "bmiidx3-boot": ("bmiidx3", 9000, ["--rtc", "1791906310", "--video", "off", "--script", "oracle/scripts/bmiidx3-boot.his"]),
    # ... and a game from what it left: coins, Start, mode and sound select, the first stage played
    # with keys and turntable spins, the result, game over (out/oracle/bmiidx3/play)
    "bmiidx3-play": ("bmiidx3", 10000, ["--rtc", "1791916974", "--video", "off", "--script", "oracle/scripts/bmiidx3-play.his"],
                     "bmiidx3-boot"),
    # the same game with the background video (MAME shows none): Video CD chapter 36 under the stage
    "bmiidx3-video": ("bmiidx3", 10000, ["--rtc", "1791916974", "--script", "oracle/scripts/bmiidx3-play.his"],
                      "bmiidx3-boot"),
    # beatmania IIDX 1st style, with the DVD player's CTS on SIO1 (TW-5; stock MAME stops at E600):
    # the first boot, test mode's DVD check and backup clear, the reset, attract (out/oracle/bmiidx/clear,
    # a MAME with the same SIO1: oracle/mame-patches); then a game from what it left (.../play)
    "bmiidx-clear": ("bmiidx", 10820, ["--rtc", "1791951737", "--video", "off", "--script", "oracle/scripts/bmiidx-clear.his"]),
    "bmiidx-play": ("bmiidx", 14000, ["--rtc", "1791957993", "--video", "off", "--script", "oracle/scripts/bmiidx-play.his"],
                    "bmiidx-clear"),
    # the same game with the background video and the graffiti layer the game draws over it
    "bmiidx-video": ("bmiidx", 14000, ["--rtc", "1791957993", "--script", "oracle/scripts/bmiidx-play.his"],
                     "bmiidx-clear"),
    # the rest of the catalogue (TW-5): each style's first power-on, E150, 1P Start, attract; then a
    # game from what it left, a stage played to STAGE FAILED. All in lockstep with MAME at the pin
    # (out/oracle/<set>/boot, .../play). Substream is twinklex2 (X76F041, the player on the UART).
    "bmiidx2-boot": ("bmiidx2", 9000, ["--rtc", "1792014933", "--video", "off", "--script", "oracle/scripts/bmiidx2-boot.his"]),
    "bmiidx2-play": ("bmiidx2", 10000, ["--rtc", "1792043957", "--video", "off", "--script", "oracle/scripts/bmiidx2-play.his"],
                     "bmiidx2-boot"),
    "bmiidx4-boot": ("bmiidx4", 9000, ["--rtc", "1792014933", "--video", "off", "--script", "oracle/scripts/bmiidx4-boot.his"]),
    "bmiidx4-play": ("bmiidx4", 10000, ["--rtc", "1792045140", "--video", "off", "--script", "oracle/scripts/bmiidx4-play.his"],
                     "bmiidx4-boot"),
    "bmiidx5-boot": ("bmiidx5", 9000, ["--rtc", "1792014933", "--video", "off", "--script", "oracle/scripts/bmiidx5-boot.his"]),
    "bmiidx5-play": ("bmiidx5", 10000, ["--rtc", "1792046165", "--video", "off", "--script", "oracle/scripts/bmiidx5-play.his"],
                     "bmiidx5-boot"),
    "bmiidxs-boot": ("bmiidxs", 10000, ["--rtc", "1792047212", "--video", "off", "--script", "oracle/scripts/bmiidxs-boot.his"]),
    "bmiidxs-play": ("bmiidxs", 11300, ["--rtc", "1792048235", "--video", "off", "--script", "oracle/scripts/bmiidxs-play.his"],
                     "bmiidxs-boot"),
    # the DVD styles: the same, and 6th style's game with the picture MAME doesn't show, DVD chapter
    # 85 (the game sends "085"; MAME's player takes it as a Video CD's 8) under the stage
    "bmiidx6-boot": ("bmiidx6", 9000, ["--rtc", "1792049343", "--video", "off", "--script", "oracle/scripts/bmiidx6-boot.his"]),
    "bmiidx6-play": ("bmiidx6", 10000, ["--rtc", "1792050235", "--video", "off", "--script", "oracle/scripts/bmiidx6-play.his"],
                     "bmiidx6-boot"),
    "bmiidx6-video": ("bmiidx6", 10000, ["--rtc", "1792050235", "--script", "oracle/scripts/bmiidx6-play.his"],
                      "bmiidx6-boot"),
    "bmiidx7a-boot": ("bmiidx7a", 10000, ["--rtc", "1792051272", "--video", "off", "--script", "oracle/scripts/bmiidx7a-boot.his"]),
    "bmiidx7a-play": ("bmiidx7a", 11300, ["--rtc", "1792052201", "--video", "off", "--script", "oracle/scripts/bmiidx7a-play.his"],
                      "bmiidx7a-boot"),
    "bmiidx8-boot": ("bmiidx8", 10000, ["--rtc", "1792053239", "--video", "off", "--script", "oracle/scripts/bmiidx8-boot.his"]),
    "bmiidx8-play": ("bmiidx8", 11300, ["--rtc", "1792054162", "--video", "off", "--script", "oracle/scripts/bmiidx8-play.his"],
                     "bmiidx8-boot"),
    # the club kits' IIDX cabinets, no DDR cabinet on the link (docs/CATALOGUE.md, Phase 6): Club
    # Version 1 on the 1st style's board (test mode's backup clear and NETWORK CHECK, then the 1st
    # style's scripted game; against the SIO1-patched MAME), Club Version 2 on Substream's (stock MAME)
    "bmiidxc-clear": ("bmiidxc", 10800, ["--rtc", "1792260779", "--video", "off", "--script", "oracle/scripts/bmiidxc-clear.his"]),
    "bmiidxc-play": ("bmiidxc", 14000, ["--rtc", "1792263397", "--video", "off", "--script", "oracle/scripts/bmiidxc-play.his"],
                     "bmiidxc-clear"),
    "bmiidxca-clear": ("bmiidxca", 10800, ["--rtc", "1792263801", "--video", "off", "--script", "oracle/scripts/bmiidxca-clear.his"]),
    "bmiidxca-play": ("bmiidxca", 14000, ["--rtc", "1792264865", "--video", "off", "--script", "oracle/scripts/bmiidxca-play.his"],
                      "bmiidxca-clear"),
    "bmiidxc2-boot": ("bmiidxc2", 10000, ["--rtc", "1792265698", "--video", "off", "--script", "oracle/scripts/bmiidxc2-boot.his"]),
    "bmiidxc2-play": ("bmiidxc2", 11300, ["--rtc", "1792265990", "--video", "off", "--script", "oracle/scripts/bmiidxc2-play.his"],
                      "bmiidxc2-boot"),
    # Konami Python 1 (PLAN.md section 23): Reliquary hosted; BIOS, card boot, disk boot,
    # attract, coins, NORMAL mode, a song played through to CLEARED. Software GS. --interlaced: the
    # games' own field drawing, as recorded (the default draws every line; only pictures differ)
    "popn9-play": ("popn9", 14000, ["--script", "oracle/scripts/popn9-play.his", "--interlaced"]),
    # the same with the Vulkan GS, the one played with: deterministic on a given GPU and
    # driver (re-record on another PC); keeps GS settings honest (PLAN.md section 23, PY-3)
    "popn9-play-vk": ("popn9", 14000, ["--renderer", "vk", "--script", "oracle/scripts/popn9-play.his", "--interlaced"]),
    # pop'n music 10-14 on the same core: coins, mode, buttons, a song played (one button every 10 frames)
    "popn10-play": ("popn10", 14000, ["--script", "oracle/scripts/popn10-play.his", "--interlaced"]),
    "popn11-play": ("popn11", 14000, ["--script", "oracle/scripts/popn11-play.his", "--interlaced"]),
    "popn12-play": ("popn12", 14000, ["--script", "oracle/scripts/popn12-play.his", "--interlaced"]),
    "popn13-play": ("popn13", 16000, ["--script", "oracle/scripts/popn13-play.his", "--interlaced"]),
    "popn14-play": ("popn14", 14000, ["--script", "oracle/scripts/popn14-play.his", "--interlaced"]),
    # pop'n 12's NET TAISEN between two cabinets on one LAN cable (runtime/python/lancable.h, PLAN.md
    # section 23): e-AMUSEMENT on, the event phases typed to MAX, then the two cabinets' credits at
    # once, a battle with a CPU in the third seat to its RESULT
    "popn12-eamuse-on": ("popn12", 4130, ["--script", "oracle/scripts/popn12-eamuse-on.his"]),
    "popn12-phases": ("popn12", 22140, ["--script", "oracle/scripts/popn12-phases.his"], "popn12-eamuse-on"),
    "popn12-nettaisen-a": ("popn12", 27000, ["--script", "oracle/scripts/popn12-nettaisen-a.his",
                                             "--lan-host", "out/regress/work/popn12-lan.sock"], "popn12-phases", "popn12-nettaisen-b"),
    "popn12-nettaisen-b": ("popn12", 27000, ["--script", "oracle/scripts/popn12-nettaisen-b.his",
                                             "--lan-join", "out/regress/work/popn12-lan.sock"], "popn12-phases", "popn12-nettaisen-a"),
    # the same for pop'n 13 and 14 (no event phase needed); 14's e-AMUSEMENT PASS numbers fixed
    "popn13-eamuse-on": ("popn13", 4710, ["--script", "oracle/scripts/popn13-eamuse-on.his"]),
    "popn13-nettaisen-a": ("popn13", 28600, ["--script", "oracle/scripts/popn13-nettaisen-a.his",
                                             "--lan-host", "out/regress/work/popn13-lan.sock"], "popn13-eamuse-on", "popn13-nettaisen-b"),
    "popn13-nettaisen-b": ("popn13", 28600, ["--script", "oracle/scripts/popn13-nettaisen-b.his",
                                             "--lan-join", "out/regress/work/popn13-lan.sock"], "popn13-eamuse-on", "popn13-nettaisen-a"),
    "popn14-eamuse-on": ("popn14", 4710, ["--script", "oracle/scripts/popn14-eamuse-on.his", "-card0", "E004010000001401"]),
    "popn14-nettaisen-a": ("popn14", 29300, ["--script", "oracle/scripts/popn14-nettaisen-a.his", "-card0", "E004010000001401",
                                             "--lan-host", "out/regress/work/popn14-lan.sock"], "popn14-eamuse-on", "popn14-nettaisen-b"),
    "popn14-nettaisen-b": ("popn14", 29300, ["--script", "oracle/scripts/popn14-nettaisen-b.his", "-card0", "E004010000001402",
                                             "--lan-join", "out/regress/work/popn14-lan.sock"], "popn14-eamuse-on", "popn14-nettaisen-a"),
    # Konami Python 2 (PY-6): the boot program, the core's network and the attract. One software
    # rasterizer thread: with PCSX2's extra threads, a few anti-aliased text pixels (attract's COIN(S)
    # 0/2) come out a little different run to run (the sound and the machine identical)
    "drmnvj-attract": ("drmnvj", 13200, ["--script", "oracle/scripts/drmnvj-attract.his", "--pcsx2-set", "EmuCore/GS/extrathreads=0"]),
    "drmnv2j-attract": ("drmnv2j", 14400, ["--script", "oracle/scripts/drmnv2j-attract.his", "--pcsx2-set", "EmuCore/GS/extrathreads=0"]),
    # e-AMUSEMENT on (test mode, saved on the disk), then a boot with it: the core's server, its
    # network's keepalive and AVS services, the card unit on the P2IO
    "drmnvj-eamuse-on": ("drmnvj", 12870, ["--script", "oracle/scripts/drmnvj-eamuse-on.his", "--pcsx2-set", "EmuCore/GS/extrathreads=0"]),
    "drmnvj-eamuse": ("drmnvj", 13200, ["--script", "oracle/scripts/drmnvj-eamuse.his", "--pcsx2-set", "EmuCore/GS/extrathreads=0"], "drmnvj-eamuse-on"),
    # a card's life: registered and played, then again with its password (GF/DM V's cardutil, gameend)
    "drmnvj-card-new": ("drmnvj", 27100, ["--script", "oracle/scripts/drmnvj-card-new.his", "--pcsx2-set", "EmuCore/GS/extrathreads=0"], "drmnvj-eamuse-on"),
    "drmnvj-card-again": ("drmnvj", 25600, ["--script", "oracle/scripts/drmnvj-card-again.his", "--pcsx2-set", "EmuCore/GS/extrathreads=0"], "drmnvj-card-new"),
}

# Twinkle: the cases above run MAME's program CD drive (--mame-cd), so they stay in lockstep
# with MAME; each game again on the cabinet's 4x drive (the default: a READ's data a sector at a
# time, as the drive reads it; PLAN.md section 21), from the same NVRAM
for _c, (_game, _frames, _args, *_rest) in list(CASES.items()):
    if _game.startswith("bmiidx"):
        CASES[_c] = (_game, _frames, ["--mame-cd"] + _args, *_rest)
        if _c.endswith("-play"):
            CASES[_c + "-drive"] = (_game, _frames, _args, *_rest)

# The club kits (docs/CATALOGUE.md, Phase 6): a DDR 2nd Club Version 573 and its IIDX cabinet in a
# club session, two processes on one serial cable (runtime/sys573/link.h), each pair at once. The
# DDR sides from their install CD's install; the IIDX sides on the 4x drive, as a cabinet plays
CASES.update({
    "ddr2mc-install": ("ddr2mc", 13200, ["--rtc", "1792290000", "--cd", "install", "--script", "oracle/scripts/ddr2mc-install.his"]),
    "ddr2mc2-install": ("ddr2mc2", 13200, ["--rtc", "1792290000", "--cd", "install", "--script", "oracle/scripts/ddr2mc2-install.his"]),
    "ddr2mc-club": ("ddr2mc", 18000, ["--rtc", "1792300000", "--script", "oracle/scripts/ddr2mc-club.his",
                                      "--session-join", "out/regress/work/club1.sock"], "ddr2mc-install", "bmiidxc-club"),
    "bmiidxc-club": ("bmiidxc", 18000, ["--rtc", "1792300000", "--video", "off", "--script", "oracle/scripts/bmiidxc-club.his",
                                        "--session-host", "out/regress/work/club1.sock"], "bmiidxc-clear", "ddr2mc-club"),
    "ddr2mc2-club": ("ddr2mc2", 16000, ["--rtc", "1792300000", "--script", "oracle/scripts/ddr2mc2-club.his",
                                        "--session-join", "out/regress/work/club2.sock"], "ddr2mc2-install", "bmiidxc2-club"),
    "bmiidxc2-club": ("bmiidxc2", 16000, ["--rtc", "1792300000", "--video", "off", "--script", "oracle/scripts/bmiidxc2-club.his",
                                          "--session-host", "out/regress/work/club2.sock"], "bmiidxc2-boot", "ddr2mc2-club"),
})

# Dance Maniax and Mambo a GoGo (docs/CATALOGUE.md, Phase 5; digital board, ZS01 cassette): each
# set's first-boot install from blank NVRAM, then a song from what it left, in lockstep with MAME
# (out/oracle/<set>/{install,play}); the rental Mambo to its RTCRAM error (no e-Amusement device)
CASES.update({
    "dmx-install": ("dmx", 9000, ["--rtc", "1792327450", "--script", "oracle/scripts/dmx-install.his"]),
    "dmx-play": ("dmx", 17700, ["--rtc", "1792328276", "--script", "oracle/scripts/dmx-play.his"], "dmx-install"),
    "dmx-test": ("dmx", 10600, ["--rtc", "1792328102", "--script", "oracle/scripts/dmx-test.his"], "dmx-install"),
    "dncfrks-install": ("dncfrks", 9000, ["--rtc", "1792327900", "--script", "oracle/scripts/dncfrks-install.his"]),
    "dncfrks-play": ("dncfrks", 17100, ["--rtc", "1792328954", "--script", "oracle/scripts/dncfrks-play.his"], "dncfrks-install"),
    "dmx2m-install": ("dmx2m", 9900, ["--rtc", "1792327635", "--script", "oracle/scripts/dmx2m-install.his"]),
    "dmx2m-play": ("dmx2m", 13500, ["--rtc", "1792328646", "--script", "oracle/scripts/dmx2m-play.his"], "dmx2m-install"),
    "dmx2majp-install": ("dmx2majp", 9900, ["--rtc", "1792327789", "--script", "oracle/scripts/dmx2majp-install.his"]),
    "dmx2majp-play": ("dmx2majp", 12600, ["--rtc", "1792328824", "--script", "oracle/scripts/dmx2majp-play.his"], "dmx2majp-install"),
    "mamboagg-install": ("mamboagg", 9300, ["--rtc", "1792327539", "--script", "oracle/scripts/mamboagg-install.his"]),
    "mamboagg-play": ("mamboagg", 13300, ["--rtc", "1792328476", "--script", "oracle/scripts/mamboagg-play.his"], "mamboagg-install"),
    "mamboagg-test": ("mamboagg", 7780, ["--rtc", "1792328203", "--script", "oracle/scripts/mamboagg-test.his"], "mamboagg-install"),
    "mamboagga-install": ("mamboagga", 9300, ["--rtc", "1792330345", "--script", "oracle/scripts/mamboagga-install.his"]),
    "mamboagga-boot": ("mamboagga", 4800, ["--rtc", "1792330438", "--script", "oracle/scripts/mamboagga-boot.his"], "mamboagga-install"),
    # the rental Mambo with the rental unit's presence (--rental) and Konami's Master Calendar
    # (--master-calendar): HIRAKU only, MAME fits neither (docs/CATALOGUE.md, Phase 5)
    "mamboagga-rental-install": ("mamboagga", 9300, ["--rtc", "1792330345", "--rental", "--script",
                                                     "oracle/scripts/mamboagga-rental-install.his"]),
    "mamboagga-factory": ("mamboagga", 3000, ["--rtc", "1792330438", "--rental", "--master-calendar", "7", "--script",
                                              "oracle/scripts/mamboagga-factory.his"], "mamboagga-rental-install"),
})

# report lines that describe the host, not the machine
HOST_LINES = ("profile:", "audio:", "nvram:")
HOST_TIME = re.compile(r" in [0-9.]+ s of host time")   # kbm's synth line
RUN_FILES = re.compile(r", run files in \S+")   # the Python core's: where this checkout keeps them


def after_of(case):
    return CASES[case][3] if len(CASES[case]) > 3 else None


def partner_of(case):
    """a session case's other cabinet: the two run at once, joined by the session cable"""
    return CASES[case][4] if len(CASES[case]) > 4 else None


def game_core(game):
    """the game's core, as games/<game>/game.toml names it ("" when it doesn't: Firebeat)"""
    try:
        with open(os.path.join(ROOT, "games", game, "game.toml")) as f:
            return next((ln.split('"')[1] for ln in f if ln.startswith("core")), "")
    except OSError:
        return ""


def game_binary(game):
    """(binary, its first arguments): the game's own hiraku-<id>, else its core's (a release's build,
    HK_GAME_BINARIES off: hiraku-djmain, hiraku-sys573, hiraku-twinkle, hiraku-firebeat, told the
    game with --game)"""
    own = os.path.join(BIN, "hiraku-" + game)
    if os.path.exists(own) or os.path.exists(own + ".exe"):
        return own, []
    try:
        with open(os.path.join(ROOT, "games", game, "game.toml")) as f:
            core = next((ln.split('"')[1] for ln in f if ln.startswith("core")), "")
    except OSError:
        core = ""
    if core in ("djmain", "sys573", "twinkle", "python"):
        return os.path.join(BIN, "hiraku-" + core), ["--game", game]
    if core in ("", "firebeat"):   # hiraku-firebeat: the executor (runtime/core/exec.h)
        return os.path.join(BIN, "hiraku-firebeat"), ["--game", game]
    return own, []


def chain(case):
    """the cases to run, in order, for this one: the ones it starts after, then itself"""
    c = [case]
    while after_of(c[0]):
        c.insert(0, after_of(c[0]))
    return c


def run(case, digest_path, take=False, nice=0, src=None):
    """take: the case it comes after has no other case to start, so its NVRAM directory is moved
    here rather than copied (the 573's, with an installed hard disk, are ~400 MB). nice: run it at
    that lower priority (what it computes is the same; only how soon). src: start from this NVRAM
    directory (the cached one the case it comes after left last time) instead of that case's"""
    game, frames, extra = CASES[case][:3]
    # paths in out/regress (the session pairs' sockets) go to HK_REGRESS_OUT's folder instead
    extra = [os.path.join(OUT, a[len("out/regress/"):]) if a.startswith("out/regress/") else a for a in extra]
    exe, pre = game_binary(game)
    work = os.path.join(OUT, "work", case)
    os.makedirs(work, exist_ok=True)
    cmd = [exe] + pre + ["--frames", str(frames), "--out", work] + extra
    if game_core(game) in ("sys573", "twinkle"):
        cmd += ["--mame-sound"]   # MAME's 50 Hz sound updates, as the baselines (the cores' default: every ms)
    if after_of(case) or any(after_of(c) == case for c in CASES):
        # its own NVRAM directory: fresh, or what the case it comes after left
        nv = os.path.join(work, "nvram")
        shutil.rmtree(nv, ignore_errors=True)
        if after_of(case):
            src = src or os.path.join(OUT, "work", after_of(case), "nvram")
            if take:
                os.rename(src, nv)
            else:
                shutil.copytree(src, nv)
        else:
            os.makedirs(nv)
        cmd += ["--nvram", nv]
    if digest_path:
        cmd += ["--digest", digest_path]
    t0 = time.monotonic()
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, preexec_fn=(lambda: os.nice(nice)) if nice else None)
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    wall = time.monotonic() - t0
    cpu = (after.ru_utime - before.ru_utime) + (after.ru_stime - before.ru_stime)
    stats = [RUN_FILES.sub("", HOST_TIME.sub("", l)) for l in p.stdout.splitlines()
             if not l.startswith(HOST_LINES) and not l.startswith("frametime")]
    return p.returncode, stats, wall, cpu, p.stdout + p.stderr


def cases_from(argv):
    names = argv or list(CASES)
    for n in names:
        if n not in CASES:
            sys.exit(f"unknown case {n}; known: {', '.join(CASES)}")
    return names + [p for p in (partner_of(n) for n in names) if p and p not in names]


def record_or_check(names, record):
    base = os.path.join(OUT, "baseline")
    cur = os.path.join(OUT, "current")
    os.makedirs(base, exist_ok=True)
    os.makedirs(cur, exist_ok=True)
    t_start = time.monotonic()
    lock = threading.Lock()   # each case's lines printed together, as it finishes
    times = []   # (case, start, end) in seconds from the start, for the summary
    bad = 0

    def report(case, rc, wall, log, asked):
        nonlocal bad
        took = f" [{wall:.0f} s]"
        with lock:
            if rc != 0:
                print(f"{case}: exit {rc}{took}\n{log[-2000:]}")
                bad += 1
            elif not asked:
                print(f"{case}: run to set up the next{took}")
            elif record:
                print(f"{case}: recorded{took}")
            else:
                bad += compare(case, base, cur, took)
            sys.stdout.flush()

    # Every case to run. A case starts from the NVRAM its parent left: from the parent's
    # directory once the parent has run, or at once from the copy the parent left the last time
    # (out/regress/cache/), which the parent's new one is checked against when it ends: the same,
    # and the case started from what it would have; else it runs again from the new one. The runs
    # are deterministic, so the parents mostly leave what they left before and the cases of a
    # chain run at once. HK_REGRESS_SPECULATE=0: each case waits for its parent, as before.
    todo = []
    for n in names:
        todo += [c for c in chain(n) if c not in todo]
    children = {c: [k for k in todo if after_of(k) == c] for c in todo}
    last = last_times()
    spec = os.environ.get("HK_REGRESS_SPECULATE", "1") != "0"
    cache = os.path.join(OUT, "cache")
    cached = {c: read_hash(c) for c in todo if children[c]} if spec else {}

    def cost(c):   # the longest way to the end through c: what to start first
        own = last.get(c, CASES[c][1] / 60)
        return own + max((cost(k) for k in children[c] if not cached.get(c)), default=0)

    # the critical path (the run's length, as far as the last run's times tell): it runs at normal
    # priority, everything else lower, so the others take what CPU it leaves rather than slow it
    critical = set()
    c = max(todo, key=cost, default=None)
    while c:
        critical.add(c)
        c = max((k for k in children[c] if not cached.get(c)), key=cost, default=None)

    # a case: its state (wait, run, ran = done but its start not yet confirmed, ok, failed), what
    # it started from (None, "parent" or "cache"), its result to report once confirmed, and the
    # hash of the NVRAM it left (for its children)
    state = {c: "wait" for c in todo}
    started_from, result, left = {}, {}, {}
    redo = set()   # running, but started from a cache that turned out stale: run again when it ends

    def one(case, src):
        asked = case in names   # the others only set up a case that starts from them
        d = os.path.join(base if record else cur, case + ".digest") if asked else None
        t0 = time.monotonic() - t_start
        # the only case to start from its parent takes the parent's NVRAM directory (not when
        # caching: the parent's is kept to check and cache)
        take = not spec and bool(after_of(case)) and children[after_of(case)] == [case]
        # record: the digest is made beside the baseline and becomes it only when the run ended well
        # (a run that crashed half way would otherwise leave its cut digest as the baseline)
        made = d + ".new" if asked and record else d
        rc, stats, _, _, log = run(case, made, take=take, nice=0 if case in critical else 10, src=src)
        if asked and record:
            if rc == 0 and os.path.exists(made):
                os.replace(made, d)
            elif os.path.exists(made):
                os.remove(made)
        if asked and (rc == 0 or not record):
            with open(d.replace(".digest", ".stats"), "w") as f:
                f.write("\n".join(stats) + "\n")
        h = nvram_hash(case) if children[case] and rc == 0 else None
        t1 = time.monotonic() - t_start   # with its setup (the NVRAM directory)
        with lock:
            times.append((case, t0, t1))
        return rc, t1 - t0, log, asked, h

    def confirmed_start(g):
        """g started from what its parent leaves now: no parent, the parent's own directory, or a
        cache the parent's new NVRAM matches"""
        q = after_of(g)
        if q is None:
            return True
        if started_from.get(g) == "parent":
            return state[q] == "ok"
        return state[q] == "ok" and left.get(q) is not None and left[q] == cached.get(q)

    def confirm(c):
        """c's start is confirmed: report it, then deal with its children that started from the cache"""
        rc = result[c][0]
        state[c] = "ok" if rc == 0 else "failed"
        report(c, *result[c])
        for k in children[c]:
            if started_from.get(k) != "cache" or state[k] not in ("run", "ran"):
                continue
            group = [k] + ([partner_of(k)] if partner_of(k) else [])
            if state[c] == "ok" and left.get(c) == cached.get(c):
                if all(state[g] == "ran" and confirmed_start(g) for g in group):
                    for g in group:
                        confirm(g)
                continue   # still running: confirmed when it ends
            for g in group:   # its start is gone (the parent failed, or left another NVRAM): again
                if state[c] == "ok":
                    with lock:
                        print(f"{g}: run again ({after_of(g)} left another NVRAM than the cached one)")
                if state[g] == "run":
                    redo.add(g)
                elif state[g] == "ran":
                    state[g] = "wait"
                started_from[g] = "parent"

    def finished(c, rc, wall, log, asked, h):
        if c in redo:   # what it started from is gone: it waits for its parent (or is not run)
            redo.discard(c)
            state[c] = "wait"
            return
        result[c] = (rc, wall, log, asked)
        left[c] = h
        state[c] = "ran"
        group = [c] + ([partner_of(c)] if partner_of(c) else [])
        if all(state[g] == "ran" and confirmed_start(g) for g in group):
            for g in group:
                confirm(g)

    # half the CPUs by default: the cores run several threads each, and with every CPU busy the
    # longest chain (the run's length) slows down more than the rest gains (HK_REGRESS_JOBS: other)
    jobs = int(os.environ.get("HK_REGRESS_JOBS") or max(2, (os.cpu_count() or 4) // 2))
    running = {}
    with cf.ThreadPoolExecutor(max_workers=jobs + 1) as ex:   # + 1: a session pair when jobs is 1
        while any(state[c] not in ("ok", "failed") for c in todo):
            for c in sorted(todo, key=cost, reverse=True):
                if state[c] != "wait":
                    continue
                p = partner_of(c)
                group = [c] + ([p] if p else [])
                why = next((after_of(g) for g in group if after_of(g) and state[after_of(g)] == "failed"), None)
                if why:
                    for g in group:
                        state[g] = "failed"
                        with lock:
                            print(f"{g}: not run ({why} failed)")
                        bad += 1
                    continue
                srcs = {}   # where each starts from: nothing, its parent's NVRAM once it has run, or the cache
                for g in group:
                    q = after_of(g)
                    if q is None:
                        srcs[g] = (None, None)
                    elif state[q] == "ok":
                        srcs[g] = (None, "parent")
                    elif cached.get(q) and started_from.get(g) != "parent" and state[q] != "failed":
                        srcs[g] = (os.path.join(cache, q), "cache")
                if len(srcs) < len(group) or (p and p not in todo):
                    continue
                # no more than `jobs` at once, picked here (longest way first) rather than queued in
                # the pool, so a case that becomes ready later doesn't wait behind shorter ones
                if running and len(running) + len(group) > jobs:
                    continue
                for g in group:
                    started_from[g] = srcs[g][1]
                    state[g] = "run"
                    running[ex.submit(one, g, srcs[g][0])] = g
            if not running:
                break
            done_now, _ = cf.wait(list(running), return_when=cf.FIRST_COMPLETED)
            for f in done_now:
                finished(running.pop(f), *f.result())
    for c in todo:   # in chain order: a case after a case not run is not run either
        if state[c] not in ("ok", "failed"):
            q = after_of(c) if after_of(c) and state.get(after_of(c)) == "failed" else None
            print(f"{c}: not run ({q} failed)" if q else f"{c}: not run (what it starts from never ran)")
            state[c] = "failed"
            bad += 1
    # what the parents left, kept for the next run
    if spec:
        for c in todo:
            if children[c] and state[c] == "ok" and left.get(c) and left[c] != cached.get(c):
                save_cache(c, left[c])
    total = time.monotonic() - t_start
    if not record:
        print("all identical" if bad == 0 else f"{bad} case(s) differ")
    write_times(times, total, "record" if record else "check", jobs)
    return 1 if bad else 0


def nvram_hash(case):
    """a hash of the NVRAM directory a case left (its files' names and contents)"""
    h = hashlib.sha1()
    top = os.path.join(OUT, "work", case, "nvram")
    for d, dirs, files in sorted(os.walk(top)):
        dirs.sort()
        for f in sorted(files):
            path = os.path.join(d, f)
            h.update(os.path.relpath(path, top).encode() + b"\0")
            with open(path, "rb") as fh:
                for chunk in iter(lambda: fh.read(1 << 20), b""):
                    h.update(chunk)
            h.update(b"\0")
    return h.hexdigest()


def read_hash(case):
    """the hash of the cached NVRAM a case left last time, or None (no cache)"""
    try:
        h = open(os.path.join(OUT, "cache", case + ".sha1")).read().strip()
    except FileNotFoundError:
        return None
    return h if os.path.isdir(os.path.join(OUT, "cache", case)) else None


def save_cache(case, h):
    """keep the NVRAM a case left (out/regress/cache/CASE, and its hash) for the next run's children"""
    cdir = os.path.join(OUT, "cache")
    os.makedirs(cdir, exist_ok=True)
    tmp = os.path.join(cdir, case + ".tmp")
    shutil.rmtree(tmp, ignore_errors=True)
    shutil.copytree(os.path.join(OUT, "work", case, "nvram"), tmp)
    shutil.rmtree(os.path.join(cdir, case), ignore_errors=True)
    os.rename(tmp, os.path.join(cdir, case))
    with open(os.path.join(cdir, case + ".sha1"), "w") as f:
        f.write(h + "\n")


def last_times():
    """each case's wall time in the last run (out/regress/times.txt), for running the long ways first"""
    t = {}
    try:
        for line in open(os.path.join(OUT, "times.txt")):
            f = line.split("\t")
            if not line.startswith("#") and len(f) == 4:
                t[f[0]] = float(f[3])
    except FileNotFoundError:
        pass
    return t


def write_times(times, total, what, jobs):
    """How long the run took and where: each case's wall time (out/regress/times.txt, the last run's),
    and the run's critical path: the cases that ended last, one starting from the other."""
    lines = [f"# tools/regress.py {what}, {time.strftime('%Y-%m-%d %H:%M')}: {total:.0f} s, {len(times)} cases "
             f"({jobs} at once, on {os.cpu_count()} CPUs)",
             "# case  start s  end s  wall s"]
    for case, t0, t1 in sorted(times, key=lambda t: t[1]):
        lines.append(f"{case}\t{t0:.0f}\t{t1:.0f}\t{t1 - t0:.0f}")
    # a run of some cases keeps the other cases' times from before (their wall times order the
    # next full run), marked as from an earlier run
    ran = {c for c, _, _ in times}
    for case, wall in sorted(last_times().items()):
        if case not in ran:
            lines.append(f"{case}\t-\t-\t{wall:.0f}")
    with open(os.path.join(OUT, "times.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    if times:
        # the run's length: its longest case (no run is shorter), and the case that ended last
        longest = max(times, key=lambda t: t[2] - t[1])
        last = max(times, key=lambda t: t[2])
        print(f"took {total:.0f} s; the longest case: {longest[0]} {longest[2] - longest[1]:.0f} s; "
              f"the last to end: {last[0]} (at {last[2]:.0f} s)")
    print(f"each case's times: {os.path.relpath(os.path.join(OUT, 'times.txt'), ROOT)}")


def compare(case, base, cur, took=""):
    """1 if the case differs from its baseline (printed, `took` after the verdict), else 0"""
    try:
        a = open(os.path.join(base, case + ".digest")).read().splitlines()
        sa = open(os.path.join(base, case + ".stats")).read()
    except FileNotFoundError:
        print(f"{case}: no baseline (run: tools/regress.py record {case}){took}")
        return 1
    b = open(os.path.join(cur, case + ".digest")).read().splitlines()
    sb = open(os.path.join(cur, case + ".stats")).read()
    for k in range(min(len(a), len(b))):
        if a[k] != b[k]:
            fa, pa, aa = a[k].split()
            _, pb, ab = b[k].split()
            what = " and ".join(w for w, d in (("picture", pa != pb), ("sound", aa != ab)) if d)
            print(f"{case}: DIFFERS from frame {fa} ({what}){took}")
            return 1
    if len(a) != len(b):
        print(f"{case}: DIFFERS: {len(a)} frames in the baseline, {len(b)} now{took}")
        return 1
    # how a Firebeat game's functions ran (a game's own recompiled binary, or hiraku-firebeat's
    # executor) and how many the analysis found (Ghidra's, or HIRAKU's own: dead code aside they
    # agree) are not run statistics: the runs must be the same
    how = re.compile(r"^(hiraku \S+:) \d+ (recompiled )?functions, (native|executed \(hiraku-firebeat\))$", re.M)
    sa, sb = how.sub(r"\1 functions", sa), how.sub(r"\1 functions", sb)
    if sa != sb:
        print(f"{case}: frames and sound identical, run statistics DIFFER:{took}")
        for la, lb in zip(sa.splitlines(), sb.splitlines()):
            if la != lb:
                print(f"  was: {la}\n  now: {lb}")
        return 1
    print(f"{case}: identical ({len(a)} frames){took}")
    return 0


def bench(names):
    print(f"{'case':20} {'frames':>6} {'cpu s':>7} {'x real':>7} {'core %':>7}")
    for case in names:
        if partner_of(case):
            print(f"{case:20} skipped: a session case (it needs {partner_of(case)} at once)")
            continue
        game, frames, _ = CASES[case][:3]
        for c in chain(case)[:-1]:   # what it starts after, untimed
            run(c, None)
        rc, stats, wall, cpu, log = run(case, None)
        if rc != 0:
            print(f"{case}: exit {rc}\n{log[-2000:]}")
            continue
        vt = next((float(l.split("virtual time ")[1].split(" s")[0]) for l in stats if "virtual time " in l), frames / 59.94)
        print(f"{case:20} {frames:6} {cpu:7.2f} {vt / cpu:7.2f} {100 * cpu / vt:6.1f}%")
        for l in log.splitlines():
            if l.startswith("frametime"):
                print("   ", l)


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("record", "check", "bench"):
        sys.exit(__doc__)
    names = cases_from(sys.argv[2:])
    if sys.argv[1] == "bench":
        bench(names)
        return 0
    return record_or_check(names, sys.argv[1] == "record")


if __name__ == "__main__":
    sys.exit(main())
