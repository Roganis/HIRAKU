#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Add a game: give its files, in any order, and ingest finds out the rest.

    tools/ingest.py FILE_OR_FOLDER...            (the usual way: everything you have for the game)
    tools/ingest.py DISC [--romset SET.zip] [--hdd IMG] [--dvd ISO] [--cd2 CUE]   (roles given)
    tools/ingest.py --exe FIREBEAT.EXE           (a loose program, Firebeat)
    tools/ingest.py SET.py1 [--bios DIR] [--keys DIR]   (Konami Python: Reliquary's descriptor)
    tools/ingest.py SET.py2 [--bios DIR] [--keys DIR]   (Konami Python 2: the same, its retail BIOS)

Files can be MAME ROM sets (.zip, .7z), MAME CHDs, and disc or disk images
(.cue/.bin, .iso, .img); a folder stands for the files in it. CHDs are
extracted with MAME's chdman (memory-capped) into dumps/<game>/.

The game is found by
- its program, for Firebeat: the CD holding HIKARU.EXE / FIREBEAT.EXE, unpacked
  and matched by SHA-1 against games/*/game.toml;
- the MAME set's name otherwise (DJ Main, System 573, Twinkle), or a CHD's name;
- for Konami Python, the EE program on the disk (boot record C, tools/pythonfs.py).
Each image gets its role from what it is: a hard disk is beatmania III's sound
board disk or DJ Main's IDE disk, a DVD pop'n music's, a second CD ParaPara's
audio disc.

Everything goes to dumps/<game id>/ (gitignored: nothing derived from a game
goes in the repo): program.bin for Firebeat, rom/ (the set's files), the
extracted images, and machine.txt, which tells the runtime what the machine is.
"""
from __future__ import annotations

import argparse
import copy
import glob
import hashlib
import os
import re
import shutil
import struct
import subprocess
import zlib
import sys
import tomllib
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import chd  # noqa: E402
import fbexe  # noqa: E402
import mameset  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def all_games() -> list[dict]:
    out = []
    for path in sorted(glob.glob(os.path.join(ROOT, "games", "*", "game.toml"))):
        with open(path, "rb") as fh:
            out.append(tomllib.load(fh))
    return out


def known_games() -> dict[str, dict]:
    """Firebeat games by the SHA-1 of their unpacked program."""
    return {g["program"]["sha1"]: g for g in all_games() if "program" in g}


def set_name_of(romset: str) -> str:
    """A MAME set's name, as games/*/game.toml has it: the archive's (or folder's) name, unless
    an older MAME named the set otherwise, which its contents tell (tools/mameset.py)."""
    base = os.path.basename(os.path.normpath(romset))
    name = base if os.path.isdir(romset) else base.rsplit(".", 1)[0]
    try:
        current = mameset.current_name(name, romset)
    except (OSError, ValueError, zipfile.BadZipFile):
        current = None
    if current and current != name:
        print(f"set      {name} is MAME's {current} (an older MAME's name: told by its ROMs)")
        return current
    return name


def check_dumps(set_name: str, romdir: str) -> None:
    """Says which of the set's ROMs are other dumps than current MAME's: an older MAME's set
    can carry an older dump of a chip (the 573 cassettes' EEPROMs were redone since 0.243).
    The game may still run; it won't match MAME's reference runs."""
    if set_name not in mameset.load():
        return
    have = mameset.archive_crcs(romdir)
    want = {f: (s, c) for f, s, c in mameset.needed_roms(set_name)}
    for f, sc in sorted(have.items()):
        if f in want and want[f] != sc and sc not in want.values():
            print(f"note     {f} is another dump than MAME {mameset.load_build()}'s (CRC {sc[1]}, MAME's "
                  f"{want[f][1]}): an older MAME's set? The game may run, but not as MAME's does")


# Keyboardmania's synth card: its program and wave ROMs (sin-table.bin, which sets after MAME 0.289
# add, the core rebuilds when it is missing)
XT446_ROMS = ["xu50710-m27c160.bin", "sx518b0.ic34", "sx743b0.ic35", "xt445a0-828.ic36",
              "xt461a0-829.ic37", "xt462a0.ic39", "xt463a0.ic38"]


def copy_romset(romset: str, romdir: str) -> list[str]:
    os.makedirs(romdir, exist_ok=True)
    names = []
    if os.path.isdir(romset):
        # a MAME rompath folder: the set's files loose (CHDs and logs left out)
        for n in sorted(os.listdir(romset)):
            src = os.path.join(romset, n)
            if os.path.isfile(src) and not n.lower().endswith(IMAGES + (".log", ".part", ".txt")):
                shutil.copyfile(src, os.path.join(romdir, n))
                names.append(n)
    elif romset.lower().endswith(".7z"):
        # 7-Zip sets (e.g. standalone/non-merged): the 7z tool, or bsdtar (libarchive) when 7z is missing
        if sz := mameset.seven_zip():
            listing = subprocess.run([sz, "l", "-slt", romset], check=True, capture_output=True, text=True).stdout
            names = [os.path.basename(l[7:]) for l in listing.splitlines()[1:]
                     if l.startswith("Path = ") and l[7:] != romset and not l[7:].endswith(".7z")]
            subprocess.run([sz, "e", "-y", f"-o{romdir}", romset], check=True, capture_output=True)
        elif shutil.which("bsdtar"):
            listing = subprocess.run(["bsdtar", "-tf", romset], check=True, capture_output=True, text=True).stdout
            names = [os.path.basename(l) for l in listing.splitlines() if l and not l.endswith("/")]
            # flat, as 7z e: every file straight into romdir
            subprocess.run(["bsdtar", "-xf", romset, "-C", romdir, "-s", ",.*/,,"], check=True, capture_output=True)
        else:
            sys.exit(f"{romset}: a .7z set needs 7-Zip (7-zip.org; on Linux the 7zip or p7zip package) or bsdtar (libarchive)")
    else:
        with zipfile.ZipFile(romset) as z:
            for name in z.namelist():
                if name.endswith("/"):
                    continue
                with open(os.path.join(romdir, os.path.basename(name)), "wb") as fh:
                    fh.write(z.read(name))
                names.append(os.path.basename(name))
    for name in names:
        print(f"rom      {name}")
    return names


def forced_game(args, core: str | None) -> dict | None:
    """The game --game names (of that core, if given), or None."""
    gid = getattr(args, "game", None)
    if not gid:
        return None
    g = next((g for g in all_games() if g["id"] == gid), None)
    if g is None:
        sys.exit(f"ERROR    --game {gid}: no games/{gid}/game.toml")
    return g if core is None or g.get("core") == core else None


def copy_set_with_parents(romset: str, romdir: str, set_name: str) -> list[str]:
    """The set's files, and those of the sets MAME also looks in (its parent, its BIOS, its
    devices: the multi-session unit's, the network unit's) when they sit beside it, as in a MAME
    rompath: a split or merged set keeps its BIOS in sys573.zip, gq863.zip, ..."""
    names = copy_romset(romset, romdir)
    if set_name not in mameset.load():
        return names
    sets = mameset.load()
    others = mameset.chain(set_name)[1:]
    for c in mameset.chain(set_name):
        others += sets[c].get("refs", [])
    here = os.path.dirname(os.path.abspath(romset))
    have = set(names)
    for o in dict.fromkeys(others):
        for cand in (os.path.join(here, o + ".zip"), os.path.join(here, o + ".7z"), os.path.join(here, o)):
            if not os.path.exists(cand) or os.path.abspath(cand) == os.path.abspath(romset):
                continue
            tmp = romdir + ".parent"
            shutil.rmtree(tmp, ignore_errors=True)
            for n in copy_romset(cand, tmp) if os.path.isfile(cand) or os.listdir(cand) else []:
                if n not in have:
                    shutil.move(os.path.join(tmp, n), os.path.join(romdir, n))
                    have.add(n)
                    names.append(n)
            shutil.rmtree(tmp, ignore_errors=True)
            break
    # ROMs under an older MAME's file names ("803k a01.bin" is now "803k_a01.bin"): a copy under the
    # current name, found by size and CRC
    have = mameset.archive_crcs(romdir)
    by_crc = {sc: f for f, sc in have.items()}
    for f, size, crc in mameset.needed_roms(set_name):
        if f not in have and (size, crc) in by_crc:
            shutil.copyfile(os.path.join(romdir, by_crc[(size, crc)]), os.path.join(romdir, f))
            print(f"rom      {f} (the same dump as {by_crc[(size, crc)]}, by size and CRC)")
            names.append(f)
    return names


def ingest_djmain(args) -> int:
    """A DJ Main game: the MAME ROM set (program, sprites, tiles) and the hard disk."""
    set_name = set_name_of(args.romset)
    game = forced_game(args, "djmain") or next((g for g in all_games() if g.get("core") == "djmain" and g.get("mame") == set_name), None)
    if game is None:
        print(f"game     unknown - no DJ Main games/*/game.toml has mame = \"{set_name}\"")
        return 1
    gid = game["id"]
    print(f"game     {gid}: {game['title']} (DJ Main)")
    outdir = os.path.join(args.out, gid)
    names = set(copy_set_with_parents(args.romset, os.path.join(outdir, "rom"), set_name))
    check_dumps(set_name, os.path.join(outdir, "rom"))
    roms = game["roms"]
    missing = [f for k in ("program", "sprites", "tiles") for f in roms[k] if f not in names]
    if missing:
        print(f"ERROR    the set lacks {', '.join(missing)}")
        return 1
    machine_path = os.path.join(outdir, "machine.txt")
    machine: dict[str, str] = {}
    if os.path.exists(machine_path):
        with open(machine_path) as fh:
            machine = dict(l.rstrip("\n").split("=", 1) for l in fh if "=" in l)
    machine.update(core="djmain", family=game["family"], map=game.get("map", "j"),
                   program=",".join(roms["program"]), sprites=",".join(roms["sprites"]), tiles=",".join(roms["tiles"]))
    for k in ("ata_master_password", "ata_user_password"):   # complete MIX on (MAME init_<game>())
        if game["hdd"].get(k):
            machine[k] = game["hdd"][k]
        else:
            machine.pop(k, None)
    # the disk's geometry, as MAME's drive gets it from the CHD: seek times depend on its cylinders
    if game["hdd"].get("chs"):
        machine["hdd_chs"] = game["hdd"]["chs"]
    if args.hdd:
        machine["hdd"] = media_path(args.hdd, outdir)
    if not machine.get("hdd"):
        print(f"ERROR    no hard disk: add {game['hdd']['chd']}.chd (or its raw image)")
        return 1
    with open(machine_path, "w", newline="\n") as fh:   # \n on Windows too: dumps move between PCs
        for k in ["core", "family", "map", "program", "sprites", "tiles", "hdd", "hdd_chs", "ata_master_password", "ata_user_password"]:
            if k in machine:
                fh.write(f"{k}={machine[k]}\n")
    print(f"wrote    {os.path.relpath(machine_path, ROOT)}")
    return 0


def ingest_sys573(args) -> int:
    """A System 573 game: the MAME set (BIOS, cassette and I/O board chips) and the CD."""
    set_name = set_name_of(args.romset) if args.romset else None
    game = forced_game(args, "sys573") or next((g for g in all_games() if g.get("core") == "sys573" and g.get("mame") == set_name), None)
    if game is None:
        print(f"game     unknown - no System 573 games/*/game.toml has mame = \"{set_name}\"")
        return 1
    gid = game["id"]
    print(f"game     {gid}: {game['title']} (System 573)")
    outdir = os.path.join(args.out, gid)
    # a clone from its parent's merged set (--game): the clone's own ROM list, so that what it needs
    # is found by size and CRC (a merged set keeps a dump once, under one clone's name)
    set_name = game.get("mame") or set_name
    names = set(copy_set_with_parents(args.romset, os.path.join(outdir, "rom"), set_name))
    check_dumps(set_name, os.path.join(outdir, "rom"))
    roms = game["roms"]
    keys = ("bios", "h8", "cassette_eeprom", "cassette_id", "install_eeprom", "install_id", "digital_id",
            "msu_flash", "msu_rtc")
    missing = [roms[k] for k in keys if roms.get(k) and roms[k] not in names]
    if missing:
        print(f"ERROR    the set lacks {', '.join(missing)}")
        return 1
    machine_path = os.path.join(outdir, "machine.txt")
    machine: dict[str, str] = {}
    if os.path.exists(machine_path):
        with open(machine_path) as fh:
            machine = dict(l.rstrip("\n").split("=", 1) for l in fh if "=" in l)
    machine.update(core="sys573", family=game["family"])
    for k in keys:
        if roms.get(k):
            machine[k] = roms[k]
    if roms.get("preload"):   # chips the set has filled (an installed flash, a set timekeeper)
        machine["preload"] = ",".join(roms["preload"])
        missing = [p.split(":", 1)[1] for p in roms["preload"] if p.split(":", 1)[1] not in names]
        if missing:
            print(f"ERROR    the set lacks {', '.join(missing)}")
            return 1
    board = game.get("board", {})
    for k in ("io_board", "cassette", "install_kind", "pccard1", "pccard2"):
        if board.get(k):
            machine[k] = board[k]
    if board.get("memory_card_reader"):
        machine["memory_card_reader"] = "1"
    if getattr(args, "cd", None):
        machine["cd"] = media_path(args.cd, outdir)
    if board.get("msu"):
        machine["msu"] = "1"
    if getattr(args, "msu_cd", None):
        machine["msu_cd"] = media_path(args.msu_cd, outdir)
    if board.get("npu"):
        machine["npu"] = "1"
    if getattr(args, "hdd_cd", None):
        machine["hdd_cd"] = media_path(args.hdd_cd, outdir)
    for k in ("install_cd", "install2_cd"):   # MAME's install discs (--cd install, install2)
        if getattr(args, k, None):
            machine[k] = media_path(getattr(args, k), outdir)
    if not machine.get("cd"):
        print(f"ERROR    no CD: add {game['cd']['chd']}.chd (or its .cue/.bin)")
        return 1
    order = ["core", "family", *keys, "preload", "io_board", "cassette", "install_kind", "pccard1", "pccard2", "memory_card_reader", "cd", "msu", "msu_cd",
             "npu", "hdd_cd", "install_cd", "install2_cd"]
    if machine.get("msu") and not machine.get("msu_cd"):
        print(f"ERROR    no multi-session unit CD: add {game['cd'].get('msu_chd')}.chd")
        return 1
    with open(machine_path, "w", newline="\n") as fh:   # \n on Windows too: dumps move between PCs
        for k in order:
            if k in machine:
                fh.write(f"{k}={machine[k]}\n")
    print(f"wrote    {os.path.relpath(machine_path, ROOT)}")
    if getattr(args, "nvram_seed", None):
        return keep_nvram_seed(args.nvram_seed, outdir)
    return 0


def keep_nvram_seed(src: str, outdir: str) -> int:
    """A MAME NVRAM folder with the game installed (flash, timekeeper, cassette, PC Cards), kept
    as dumps/<game>/nvram-seed/: the launcher copies it into the game's saves on its first start,
    so that start skips the install from CD. HIRAKU keeps NVRAM under MAME's file names."""
    files = [f for f in sorted(os.listdir(src)) if os.path.isfile(os.path.join(src, f))]
    if not files:
        print(f"ERROR    {src}: no NVRAM files")
        return 1
    dst = os.path.join(outdir, "nvram-seed")
    shutil.rmtree(dst, ignore_errors=True)
    os.makedirs(dst)
    for f in files:
        shutil.copyfile(os.path.join(src, f), os.path.join(dst, f))
    print(f"nvram    {len(files)} files from {src}: the starting saves ({os.path.relpath(dst, ROOT) if dst.startswith(ROOT) else dst})")
    return 0


def ingest_twinkle(args) -> int:
    """A Twinkle game (beatmania IIDX): the MAME set (BIOS, SPU program, dongle), the program
    CD on the SCSI bus, the Video CD for the external player, and the SPU board's hard disk."""
    set_name = set_name_of(args.romset) if args.romset else None
    game = forced_game(args, "twinkle") or next((g for g in all_games() if g.get("core") == "twinkle" and g.get("mame") == set_name), None)
    if game is None:
        print(f"game     unknown - no Twinkle games/*/game.toml has mame = \"{set_name}\"")
        return 1
    gid = game["id"]
    print(f"game     {gid}: {game['title']} (Twinkle)")
    outdir = os.path.join(args.out, gid)
    names = set(copy_set_with_parents(args.romset, os.path.join(outdir, "rom"), set_name))
    check_dumps(set_name, os.path.join(outdir, "rom"))
    roms = game["roms"]
    keys = ("bios", "spu", "dongle")
    missing = [roms[k] for k in keys if roms.get(k) and roms[k] not in names]
    if missing:
        print(f"ERROR    the set lacks {', '.join(missing)}")
        return 1
    machine_path = os.path.join(outdir, "machine.txt")
    machine: dict[str, str] = {}
    if os.path.exists(machine_path):
        with open(machine_path) as fh:
            machine = dict(l.rstrip("\n").split("=", 1) for l in fh if "=" in l)
    machine.update(core="twinkle", family=game["family"])
    for k in keys:
        if roms.get(k):
            machine[k] = roms[k]
    board = game.get("board", {})
    machine["dongle_type"] = board["dongle"]
    machine["dvd_port"] = board["dvd_port"]
    for flag, key in (("cd", "cd"), ("video_cd", "video_cd"), ("hdd", "hdd")):
        if getattr(args, flag, None):
            machine[key] = media_path(getattr(args, flag), outdir)
    # a DVD (an image of 2048-byte sectors, not a .cue or a CD's CHD) is the player's disc from 6th style on
    v = machine.get("video_cd", "")
    if v and not v.lower().endswith(".cue") and not (v.lower().endswith(".chd") and
                                                      chd.info(v if os.path.isabs(v) else os.path.join(outdir, v)).kind == "cd"):
        machine["video_dvd"] = machine.pop("video_cd")
    # the disk's geometry (MAME's IDE disk reports it): the CHD's, else 16 heads and 63 sectors
    if getattr(args, "hdd_chs", None):
        machine["hdd_chs"] = ",".join(map(str, args.hdd_chs))
    elif machine.get("hdd") and not machine.get("hdd_chs"):
        img = os.path.join(outdir, machine["hdd"]) if not os.path.isabs(machine["hdd"]) else machine["hdd"]
        machine["hdd_chs"] = f"{os.path.getsize(img) // (16 * 63 * 512)},16,63"
    for key, what in (("cd", f"the program CD: add {game['cd']['chd']}.chd"),
                      ("video_cd" if "video_dvd" not in machine else "video_dvd",
                       f"the player's disc: add {game['cd']['video_chd']}.chd"),
                      ("hdd", f"the hard disk: add {game['hdd']['chd']}.chd")):
        if not machine.get(key):
            print(f"ERROR    no {what}")
            return 1
    order = ["core", "family", *keys, "dongle_type", "dvd_port", "cd", "video_cd", "video_dvd", "hdd", "hdd_chs"]
    with open(machine_path, "w", newline="\n") as fh:   # \n on Windows too: dumps move between PCs
        for k in order:
            if k in machine:
                fh.write(f"{k}={machine[k]}\n")
    print(f"wrote    {os.path.relpath(machine_path, ROOT)}")
    return 0


def ingest_firebeat(args) -> int:
    """A Firebeat game: the program disc (or a loose exe), and its other media."""
    if args.exe:
        exe_name = os.path.basename(args.exe)
        with open(args.exe, "rb") as fh:
            exe = fh.read()
    else:
        with fbexe.Disc(args.disc) as disc:
            iso = fbexe.Iso9660(disc)
            entry = fbexe.find_exe(iso)
            exe_name = entry.name
            exe = iso.read_file(entry)
            print(f"disc     {args.disc}  volume '{iso.volume_id}'  "
                  f"{2352 if disc.raw else 2048}-byte sectors")

    prog = fbexe.unpack_exe(exe)
    sha1 = hashlib.sha1(prog.image).hexdigest()
    print(f"exe      {exe_name}  {len(exe)} bytes  sha1 {hashlib.sha1(exe).hexdigest()}"
          f"{'  (ParaPara wrapper)' if prog.ppp_wrapped else ''}")
    print(f"program  {len(prog.image)} bytes @ {prog.load_addr:#010x}  entry {prog.entry:#010x}  sha1 {sha1}")

    if args.check:
        with open(args.check, "rb") as fh:
            ref = fh.read()
        if ref == prog.image:
            print(f"check    identical to {args.check}")
        else:
            diff = next((i for i in range(min(len(ref), len(prog.image))) if ref[i] != prog.image[i]), None)
            print(f"check    DIFFERS from {args.check} (sizes {len(ref)}/{len(prog.image)}, first diff {diff})")
            return 1

    game = known_games().get(sha1)
    if game is None:
        print("game     unknown - no games/*/game.toml has this program sha1")
        gid = "unknown-" + sha1[:8]
    else:
        gid = game["id"]
        print(f"game     {gid}: {game['title']}")
        load = game["program"].get("load", fbexe.LOAD_ADDR)
        if load != prog.load_addr:   # an older program the BIOS puts elsewhere (Keyboardmania 1st)
            prog = fbexe.Program(prog.image, load, fbexe.branch_target(
                int.from_bytes(prog.image[-4:], "big"), load + len(prog.image) - 4), prog.ppp_wrapped)
            print(f"load     {load:#010x} (from games/{gid}/game.toml), entry {prog.entry:#010x}")
        if game["program"]["entry"] != prog.entry:
            print(f"WARNING  game.toml entry {game['program']['entry']:#x} != decoded {prog.entry:#x}")

    outdir = os.path.join(args.out, gid)
    os.makedirs(outdir, exist_ok=True)
    for k in ("disc", "hdd", "dvd", "cd2"):   # images extracted before the game was known
        if getattr(args, k, None):
            setattr(args, k, adopt(getattr(args, k), args.out, outdir))
    with open(os.path.join(outdir, "program.bin"), "wb") as fh:
        fh.write(prog.image)
    print(f"wrote    {os.path.relpath(os.path.join(outdir, 'program.bin'), ROOT)}")

    # what the runtime needs to build the machine (it reads no TOML). A re-run
    # keeps the media paths it is not given (e.g. --hdd from an earlier run).
    machine_path = os.path.join(outdir, "machine.txt")
    machine: dict[str, str] = {}
    if os.path.exists(machine_path):
        with open(machine_path) as fh:
            machine = dict(l.rstrip("\n").split("=", 1) for l in fh if "=" in l)
    board = (game or {}).get("board", {})
    spu = (game or {}).get("spu", {})
    machine["family"] = (game or {}).get("family", machine.get("family", "bm3"))
    # pop'n music's second disc is a DVD on the sound board, never a hard disk: older MAME sets
    # (0.243's) keep it as a hard disk CHD, the same sectors
    if machine["family"] == "popn" and args.hdd and not args.dvd:
        args.dvd, args.hdd = args.hdd, None
        machine.pop("spu_hdd", None)
    if board.get("dongle"):
        machine["dongle"] = board["dongle"]
    if "cabinet" in board:
        machine["cabinet"] = str(board["cabinet"])
    if args.disc:
        machine["cd_master"] = media_path(args.disc, outdir)
    if spu.get("rom"):
        machine["spu_rom"] = spu["rom"]
    if args.cd2:
        machine["cd_slave"] = media_path(args.cd2, outdir)
    if args.hdd:
        machine["spu_hdd"] = media_path(args.hdd, outdir)
    if args.dvd:
        machine["spu_dvd"] = media_path(args.dvd, outdir)
    order = ["family", "dongle", "cabinet", "cd_master", "cd_slave", "spu_rom", "spu_hdd", "spu_dvd"]
    with open(machine_path, "w", newline="\n") as fh:   # \n on Windows too: dumps move between PCs
        for k in order + sorted(set(machine) - set(order)):
            if k in machine:
                fh.write(f"{k}={machine[k]}\n")
    print(f"wrote    {os.path.relpath(machine_path, ROOT)}")

    if args.romset:
        copy_romset(args.romset, os.path.join(outdir, "rom"))
    # what is still missing for this game to run
    if game:
        need = [f for f in (board.get("dongle"), spu.get("rom")) if f]
        if game.get("family") == "kbm":   # the XT446's (a split set keeps them in xt446.zip)
            need += XT446_ROMS
        romdir = os.path.join(outdir, "rom")
        lacking = [f for f in need if not os.path.exists(os.path.join(romdir, f))]
        if lacking and not set(lacking) - set(XT446_ROMS):
            print(f"NEXT     the set has no XT446 synth ROMs (the keyboard's sounds): add MAME's xt446.zip or .7z "
                  f"(a split or merged set keeps them there) ({', '.join(lacking)})")
        elif lacking:
            what = "the security dongle" + (" and the XT446 synth's ROMs (the keyboard's sounds)" if game.get("family") == "kbm" else "")
            print(f"NEXT     add the MAME set too ({game.get('mame', gid)}.zip or .7z), on its own is fine: it holds {what} "
                  f"({', '.join(lacking)})")
    return 0



PY1_FILES = (   # .py1 key -> machine.txt key; the disk is linked, the rest copied to rom/
    ("HddImagePath", "hdd"), ("CfImagePath", "cf"), ("BbsRamPath", "bbsram"),
    ("IoBootRomPath", "io_bootrom"), ("IoConfigRomPath", "io_config_rom"),
    ("InternalDonglePath", "internal_dongle"), ("ExternalDonglePath", "external_dongle"),
    ("MemoryCardDonglePath", "memcard"), ("MemoryCardIdPath", "memcard_id"))
PY_KEYS = ("civ", "cks", "eks", "kek", "arcade_kelf_override_kbit", "arcade_kelf_override_kc")


def find_near(start: str, rel: str, levels: int = 3) -> str | None:
    """rel under start or one of its parents (a collection keeps bios/ and keys/ beside the sets)."""
    d = os.path.abspath(start)
    for _ in range(levels):
        if os.path.exists(os.path.join(d, rel)):
            return os.path.join(d, rel)
        d = os.path.dirname(d)
    return None


def link_or_copy(src: str, dst: str) -> str:
    """A hard link where the filesystems allow it, else a symlink: disks are gigabytes."""
    if os.path.lexists(dst):
        os.remove(dst)
    try:
        os.link(src, dst)
        return "linked"
    except OSError:
        os.symlink(os.path.abspath(src), dst)
        return "symlinked"


def copy_disk(src: str, dst: str) -> str:
    """A hard link where the filesystems allow it, else a copy (not a symlink: the source can go)."""
    if os.path.lexists(dst):
        os.remove(dst)
    try:
        os.link(src, dst)
        return "linked"
    except OSError:
        shutil.copyfile(src, dst + ".part")
        os.replace(dst + ".part", dst)
        return "copied"


MOVED: list[tuple[str, str]] = []   # this run's moves (src, dst): a game that fails puts them back


def move_file(src: str, dst: str, undo: bool = False) -> str:
    """A file or folder moved to dst (--place move): renamed on the same drive, else copied, then
    the original removed once the copy is whole"""
    if os.path.lexists(dst):
        if os.path.exists(dst) and os.path.samefile(src, dst):
            return "have"
        shutil.rmtree(dst) if os.path.isdir(dst) and not os.path.islink(dst) else os.remove(dst)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if not undo:
        MOVED.append((src, dst))
    try:
        os.replace(src, dst)
        return "moved"
    except OSError:
        pass
    if os.path.isdir(src):
        shutil.copytree(src, dst + ".part")
        os.replace(dst + ".part", dst)
        shutil.rmtree(src)
    else:
        shutil.copyfile(src, dst + ".part")
        os.replace(dst + ".part", dst)
        os.remove(src)
    return "moved (copied to this drive, then removed)"


def undo_moves() -> None:
    """A game that couldn't be added: what this run moved goes back where it was"""
    while MOVED:
        src, dst = MOVED.pop()
        if os.path.exists(dst) and not os.path.exists(src):
            move_file(dst, src, undo=True)
            print(f"back     {os.path.basename(src)} (put back where it was: the game wasn't added)")


def place_disk(args, src: str, to: str, keep: str) -> tuple[str, str]:
    """A disk the game reads as it is, by --place: moved to `to`; left where it is (its full path in
    machine.txt); else as `keep` does (link or copy). (what machine.txt names, how)"""
    if args.place == "move":
        return os.path.basename(to), move_file(src, to)
    if args.place == "stay":
        return os.path.abspath(src), "read where it is"
    if keep == "copy":
        return os.path.basename(to), copy_disk(src, to)
    return os.path.basename(to), link_or_copy(src, to)


def move_set(args, gid: str) -> None:
    """--place move: the ROM set given (its archive or folder; not the parents or BIOS sets found
    beside it, which other games share) moved into dumps/<game>/set/ once the game is added"""
    if args.place != "move" or not args.romset or not os.path.exists(args.romset):
        return
    to = os.path.join(args.out, gid, "set", os.path.basename(os.path.normpath(args.romset)))
    print(f"set      {os.path.basename(to)} ({move_file(args.romset, to)} into {gid}/set/)")


PY2_FILES = (   # .py2 key -> machine.txt key; the disk is linked, the rest copied to rom/
    ("HddImagePath", "hdd"), ("HddIdPath", "hdd_id"), ("IlinkIdPath", "ilink_id"), ("NvRamPath", "nvram"),
    ("DongleBlackPath", "dongle_black"), ("DongleWhitePath", "dongle_white"))


def apa_partitions(img) -> list[str]:
    """The names in a PS2 hard disk's APA partition chain (each header: 'APA\\0' at 4, next at 8, id at 16)."""
    names, sector = [], 0
    for _ in range(4096):
        h = img.read(sector * 512, 512)
        if h[4:8] != b"APA\0":
            break
        names.append(h[16:48].split(b"\0")[0].decode("ascii", "replace"))
        sector = struct.unpack_from("<I", h, 8)[0]
        if sector == 0:
            break
    return names


def ingest_python2(args, py2: str) -> int:
    """A Konami Python 2 game, from its .py2 descriptor (PCSX2 Reliquary's format). The disk is
    an ordinary PS2 hard disk; the game is its last Konami game partition (PC.KNAC_<...>.<code>,
    not DOWNLOAD: an update keeps the older game's), matched to games/*/game.toml's partition."""
    import configparser
    import pythonfs
    ini = configparser.ConfigParser(interpolation=None, strict=False, comment_prefixes=(";", "#"))
    ini.optionxform = str
    ini.read(py2, encoding="utf-8")
    if "Game" not in ini:
        print(f"ERROR    {py2}: no [Game] section")
        return 1
    g = ini["Game"]
    base = os.path.dirname(os.path.abspath(py2))
    files = {}
    for key, name in PY2_FILES:
        v = g.get(key, "").strip()
        if v:
            files[name] = os.path.normpath(v if os.path.isabs(v) else os.path.join(base, v))
            if not os.path.exists(files[name]):
                print(f"ERROR    {key} = {v}: no such file")
                return 1
    if "hdd" not in files or "hdd_id" not in files:
        print("ERROR    the .py2 names no disk or no HDD ID (HddImagePath, HddIdPath)")
        return 1
    try:
        parts = apa_partitions(pythonfs.open_image(files["hdd"]))
    except OSError as e:
        print(f"ERROR    reading {files['hdd']}: {e}")
        return 1
    games = [n.rsplit(".", 1)[-1] for n in parts if n.startswith("PC.") and not n.endswith(".DOWNLOAD")]
    if not games:
        print(f"ERROR    {files['hdd']}: no Konami game partition (partitions: {', '.join(parts) or 'none'})")
        return 1
    code = games[-1]
    game = next((x for x in all_games() if x.get("core") == "python"
                 and x.get("python", {}).get("partition") == code), None)
    if game is None:
        print(f"game     unknown - no Python games/*/game.toml has partition = \"{code}\"")
        return 1
    gid = game["id"]
    print(f"game     {gid}: {game['title']} (Konami Python 2, partition {code})")
    outdir = os.path.join(args.out, gid)
    for sub in ("rom", "bios", "keys"):
        os.makedirs(os.path.join(outdir, sub), exist_ok=True)
    machine = {"core": "python", "board": "python2", "family": game["family"], "name": g.get("Name", game["title"]),
               "game_id": g.get("GameId", ""),
               "input_type": g.get("InputType", str(game.get("python", {}).get("input_type", 0)))}
    for name, src in files.items():
        if name == "hdd":
            machine[name], how = place_disk(args, src, os.path.join(outdir, os.path.basename(src)), "link")
            print(f"disk     {os.path.basename(src)} ({how})")
        else:
            shutil.copy2(src, os.path.join(outdir, "rom", os.path.basename(src)))
            machine[name] = "rom/" + os.path.basename(src)
    if "nvram" not in machine:
        print("note     no NvRamPath: the BIOS will stop at its first-time language setup (docs/PYTHON.md)")
    order = ["core", "board", "family", "name", "game_id", "input_type", "hdd", "hdd_id", "ilink_id", "nvram",
             "dongle_black", "dongle_white", "bios", "keys"]
    return finish_python(args, base, outdir, machine, order, 2)


def ingest_python(args, py1: str) -> int:
    """A Konami Python 1 game, from its .py1 descriptor (PCSX2 Reliquary's format)."""
    import configparser
    ini = configparser.ConfigParser(interpolation=None, strict=False, comment_prefixes=(";", "#"))
    ini.optionxform = str
    ini.read(py1, encoding="utf-8")
    if "Game" not in ini:
        print(f"ERROR    {py1}: no [Game] section")
        return 1
    return ingest_python1(args, dict(ini["Game"]), os.path.dirname(os.path.abspath(py1)))


# A Konami Python 1 set's files by name (MAME's and Reliquary's collection's), for a set's
# folder given without its .py1
PY1_NAMES = (("BbsRamPath", "m48t58y.u48"), ("IoBootRomPath", "b22a01.u42"), ("IoConfigRomPath", "*.crom"),
             ("InternalDonglePath", "ds2430.u3"), ("ExternalDonglePath", "ds2430_*.u3"),
             ("MemoryCardDonglePath", "*.ps2"), ("MemoryCardIdPath", "*.id"))


def python1_folder(folder: str) -> dict | None:
    """A Konami Python 1 set's folder without its .py1 (copied without it, or MAME's): the .py1's
    keys from the files' names, when a disk in it is one games/*/game.toml names ([python] hdd).
    The game id is the external dongle's (ds2430_black_gce12jab.u3: GCE12JAB), as the .py1s have it."""
    disks = {x["python"]["hdd"]: x for x in all_games() if x.get("core") == "python" and x.get("python", {}).get("hdd")}
    disk = next((c for c in sorted(glob.glob(os.path.join(folder, "*.chd")))
                 if os.path.splitext(os.path.basename(c))[0] in disks), None)
    if disk is None:
        return None
    game = disks[os.path.splitext(os.path.basename(disk))[0]]
    g = {"Name": game["title"], "HddImagePath": disk}
    for key, pattern in PY1_NAMES:
        found = sorted(glob.glob(os.path.join(folder, pattern)))
        if found:
            g[key] = found[0]
    dongle = os.path.basename(g.get("ExternalDonglePath", ""))
    if dongle.startswith("ds2430_black_"):
        g["GameId"] = os.path.splitext(dongle)[0][len("ds2430_black_"):].upper()
    return g


def python_support(args, base: str, what: str, has: str) -> str | None:
    """The arcade BIOS's or the MagicGate keys' folder (what: bios, keys; has: a file in it): given
    (--bios, --keys, or a folder of that name dropped with the game), near the set (a collection
    keeps them beside its sets), or an already added Python game's copy (dumps/*/bios, keys)."""
    for d in (getattr(args, what), find_near(base, what)) + tuple(sorted(glob.glob(os.path.join(args.out, "*", what)))):
        if d and os.path.exists(os.path.join(d, has)):
            return d
    return None


def ingest_python1(args, g: dict, base: str) -> int:
    """A Konami Python 1 game from its .py1's keys (paths relative to base)."""
    import pythonfs
    files = {}
    for key, name in PY1_FILES:
        v = g.get(key, "").strip()
        if v:
            files[name] = os.path.normpath(v if os.path.isabs(v) else os.path.join(base, v))
            if not os.path.exists(files[name]):
                print(f"ERROR    {key} = {v}: no such file")
                return 1
    disk = files.get("hdd") or files.get("cf")
    if not disk:
        print("ERROR    the .py1 names no disk (HddImagePath / CfImagePath)")
        return 1
    try:
        img = pythonfs.open_image(disk)
        off, size, _ = pythonfs.records(img)["C"]
        ee_sha1 = hashlib.sha1(pythonfs.lz_decode(img.read(off, size))).hexdigest()
    except OSError as e:
        print(f"ERROR    reading {disk}: {e}")
        return 1
    game = next((x for x in all_games() if x.get("core") == "python"
                 and x.get("python", {}).get("ee_sha1") == ee_sha1), None)
    if game is None:
        print(f"game     unknown - no Python games/*/game.toml has ee_sha1 = \"{ee_sha1}\"")
        return 1
    gid = game["id"]
    print(f"game     {gid}: {game['title']} (Konami Python, EE program {ee_sha1[:12]})")
    outdir = os.path.join(args.out, gid)
    for sub in ("rom", "bios", "keys"):
        os.makedirs(os.path.join(outdir, sub), exist_ok=True)
    machine = {"core": "python", "family": game["family"], "name": g.get("Name", game["title"]),
               "io_mode": g.get("IoMode", game.get("python", {}).get("io_mode", "JVS")).upper(),
               "game_id": g.get("GameId", "")}
    for name, src in files.items():
        if name in ("hdd", "cf"):
            to = os.path.join(outdir, os.path.basename(src))
            if not args.place and args.copy_chd and os.path.isfile(to) and not os.path.islink(to) and os.path.getsize(to) == os.path.getsize(src):
                machine[name], how = os.path.basename(src), "have"
            else:   # --copy-chd, the launcher's: the files given (a USB stick) can go afterwards
                machine[name], how = place_disk(args, src, to, "copy" if args.copy_chd else "link")
            print(f"disk     {os.path.basename(src)} ({how})")
        else:
            shutil.copy2(src, os.path.join(outdir, "rom", os.path.basename(src)))
            machine[name] = "rom/" + os.path.basename(src)
    order = ["core", "family", "name", "io_mode", "game_id", "hdd", "cf", "bbsram", "io_bootrom", "io_config_rom",
             "internal_dongle", "external_dongle", "memcard", "memcard_id", "bios", "keys"]
    return finish_python(args, base, outdir, machine, order, 1)


# The arcade BIOS and the MagicGate keys, which HIRAKU can't ship. Dropped on the launcher (loose,
# or in a folder) they are kept in dumps/_python/ for every Konami Python game; a game added
# without them waits in dumps/<game>/machine.pending (its files are in, its machine.txt not
# written) and is finished when they come, from what is already in dumps/, so the game's own
# files needn't be there any more.
PY_STORE = "_python"
PY1_BIOS = "COH-H31000.BIN"       # the Python 1 arcade BIOS, with its .NVM and .MEC
PY2_BIOS = "ps2-0190j"            # the retail BIOS Python 2 boots: ps2-0190j-20030822.bin (SCPH-50000, v1.90 Japan)


def python_support_kind(path: str) -> str | None:
    """bios or keys, for a file HIRAKU keeps for the Python games (by its name)"""
    n = os.path.basename(path).lower()
    if n.startswith(PY1_BIOS.lower()[:-4] + ".") or (n.startswith(PY2_BIOS) and n.endswith(".bin")):
        return "bios"
    if n.endswith(".bin") and n[:-4] in PY_KEYS:
        return "keys"
    return None


def keep_python_support(args, inputs: list[str]) -> list[str]:
    """The BIOS and key files among the inputs (loose, or a folder of only them, or a folder named
    bios/ or keys/ holding them) copied into dumps/_python/bios, keys; the rest returned"""
    rest, kept = [], {"bios": [], "keys": []}
    for f in inputs:
        files = [f] if os.path.isfile(f) else sorted(glob.glob(os.path.join(f, "*"))) if os.path.isdir(f) else []
        files = [x for x in files if os.path.isfile(x) and not x.lower().endswith(".txt")]
        kinds = [python_support_kind(x) for x in files]
        named = os.path.isdir(f) and os.path.basename(os.path.normpath(f)).lower() in ("bios", "keys")
        if not files or not (all(kinds) or (named and any(kinds))):
            rest.append(f)
            continue
        for x, k in zip(files, kinds):
            if not k:
                continue
            to = os.path.join(args.out, PY_STORE, k, os.path.basename(x))
            os.makedirs(os.path.dirname(to), exist_ok=True)
            if not (os.path.exists(to) and os.path.samefile(x, to)):
                shutil.copy2(x, to)
            kept[k].append(os.path.basename(x))
    for k, names in kept.items():
        if names:
            print(f"KEPT     {'the BIOS' if k == 'bios' else 'the MagicGate keys'} for the Konami Python games: {', '.join(names)}")
    return rest


def python_bios(args, base: str, board: int) -> list[str]:
    """The BIOS files to copy: Python 1's COH-H31000.BIN with its .NVM and .MEC, Python 2's
    ps2-0190j*.bin; [] when they aren't anywhere HIRAKU looks"""
    if board == 2:
        for d in (args.bios, find_near(base, "bios"), os.path.join(args.out, PY_STORE, "bios")):
            found = sorted(glob.glob(os.path.join(d, PY2_BIOS + "*.bin"))) if d else []
            if found:
                return found[:1]
        return []
    bios = python_support(args, base, "bios", PY1_BIOS)
    bios_bin = sorted(glob.glob(os.path.join(bios, "*.BIN")) + glob.glob(os.path.join(bios, "*.bin"))) if bios else []
    if not bios_bin:
        return []
    stem = os.path.splitext(bios_bin[0])[0]
    return [bios_bin[0]] + [f for f in sorted(glob.glob(stem + ".*")) if f != bios_bin[0] and not f.lower().endswith(".txt")]


def finish_python(args, base: str, outdir: str, machine: dict, order: list[str], board: int) -> int:
    """A Python game's BIOS and keys copied into dumps/<game>/ and its machine.txt written; without
    them, machine.pending (finished by complete_pending when they are dropped)"""
    import json
    bios = python_bios(args, base, board)
    need = PY_KEYS if board == 1 else PY_KEYS[:4]
    keys = python_support(args, base, "keys", "kek.bin")
    if not keys and board == 2:   # Python 2 sets beside a Python 1 collection: its keys
        keys = find_near(os.path.join(base, "..", "konami-python-1-collection"), "keys")
    missing = [k + ".bin" for k in need if not keys or not os.path.exists(os.path.join(keys, k + ".bin"))]
    if not bios or missing:
        with open(os.path.join(outdir, "machine.pending"), "w", newline="\n") as fh:
            json.dump({"machine": machine, "order": order, "board": board}, fh)
        what = []
        if not bios:
            what.append("the arcade BIOS (COH-H31000.BIN with its .NVM and .MEC)" if board == 1 else
                        "the BIOS ps2-0190j-20030822.bin (the SCPH-50000's v1.90)")
        if missing:
            what.append("the MagicGate key files " + ", ".join(missing))
        print(f"ERROR    {machine.get('name', os.path.basename(outdir))} needs {' and '.join(what)}: drop them on "
              "HIRAKU's window (the files, or their folder) and the game is added")
        return 1
    for f in bios:
        to = os.path.join(outdir, "bios", os.path.basename(f))
        if not (os.path.exists(to) and os.path.samefile(f, to)):
            shutil.copy2(f, to)
    machine["bios"] = "bios/" + os.path.basename(bios[0])
    for k in need:
        f, to = os.path.join(keys, k + ".bin"), os.path.join(outdir, "keys", k + ".bin")
        if not (os.path.exists(to) and os.path.samefile(f, to)):   # a game added again: its own copy
            shutil.copy2(f, to)
    machine["keys"] = "keys"
    print(f"bios     {os.path.basename(bios[0])}, keys copied (dumps/ is gitignored: they stay out of the repo)")
    machine_path = os.path.join(outdir, "machine.txt")
    with open(machine_path, "w", newline="\n") as fh:   # \n on Windows too: dumps move between PCs
        for k in order:
            if machine.get(k):
                fh.write(f"{k}={machine[k]}\n")
    pending = os.path.join(outdir, "machine.pending")
    if os.path.exists(pending):
        os.remove(pending)
    print(f"wrote    {os.path.relpath(machine_path, ROOT)}")
    return 0


def complete_pending(args) -> int:
    """The Python games waiting for their BIOS or keys (machine.pending), finished with what is
    kept now; 0 when all could be"""
    import json
    rc = 0
    for pending in sorted(glob.glob(os.path.join(args.out, "*", "machine.pending"))):
        outdir = os.path.dirname(pending)
        with open(pending, encoding="utf-8") as fh:
            p = json.load(fh)
        print(f"game     {os.path.basename(outdir)}: {p['machine'].get('name', '')} (waiting for its BIOS or keys)")
        rc |= finish_python(args, outdir, outdir, p["machine"], p["order"], p["board"])
    return rc


def media_path(path: str, outdir: str) -> str:
    """A path for machine.txt: relative to dumps/<game>/ when the file is in it."""
    full = os.path.abspath(path)
    base = os.path.abspath(outdir) + os.sep
    return full[len(base):] if full.startswith(base) else full


INCOMING = ".incoming"


def adopt(path: str, out: str, outdir: str) -> str:
    """Move an image extracted into dumps/.incoming/ (game not yet known) to the
    game's folder; a .cue moves with the files it names."""
    inc = os.path.join(os.path.abspath(out), INCOMING) + os.sep
    full = os.path.abspath(path)
    if not full.startswith(inc):
        return path
    moved = os.path.join(outdir, os.path.basename(full))
    if full.lower().endswith(".cue"):
        for f in fbexe_cue_files(full):
            src = os.path.join(os.path.dirname(full), f)
            if os.path.exists(src):
                os.replace(src, os.path.join(outdir, f))
    os.replace(full, moved)
    return moved


def fbexe_cue_files(cue: str) -> list[str]:
    files = []
    with open(cue, errors="replace") as fh:
        for line in fh:
            t = line.strip()
            if t.upper().startswith("FILE "):
                rest = t[5:]
                files.append(rest[1:rest.index('"', 1)] if rest.startswith('"') else rest.split()[0])
    return files


ARCHIVES = (".zip", ".7z")
IMAGES = (".chd", ".cue", ".iso", ".img", ".bin", ".raw")


def disk_name(c: str) -> str:
    """A CHD's disk name as MAME has it: from its header's SHA-1 when MAME knows the disk (a
    renamed file is still told), else the file's name. Says so when the file's name is one of
    MAME's disks but its contents aren't (an older dump)."""
    stem = os.path.splitext(os.path.basename(c))[0]
    try:
        sha1 = mameset.chd_sha1(c)
    except OSError:
        return stem
    owners = mameset.disk_owner(sha1) if sha1 else []
    names = {d for _, d in owners}
    if names and stem not in names:
        name = sorted(names)[0]
        print(f"disk     {os.path.basename(c)} is MAME's {name} (told by its SHA-1)")
        return name
    if not names and any(stem == d[0] for m in mameset.load().values() for d in m.get("disks", [])):
        print(f"note     {os.path.basename(c)}: another dump than MAME {mameset.load_build()}'s {stem} "
              f"(SHA-1 {sha1}); it may still work")
    return stem


def gather(paths: list[str], set_names: set[str]) -> list[str]:
    """The files to look at: folders expanded (a folder named after a MAME set is that
    set, as in a MAME rompath); a .bin a .cue names left to the .cue."""
    files = []
    for p in paths:
        if os.path.isdir(p) and os.path.basename(os.path.normpath(p)) in set_names:
            names = sorted(os.listdir(p))
            if any(not n.lower().endswith(IMAGES + (".log", ".part", ".txt")) for n in names):
                files.append(os.path.normpath(p))   # loose ROM files: the set itself
            files += [os.path.join(p, n) for n in names if n.lower().endswith(IMAGES)]
        elif os.path.isdir(p):
            for base, _, names in os.walk(p, followlinks=True):
                files += [os.path.join(base, n) for n in sorted(names)]
        else:
            files.append(p)
    files = [f for f in files if f.lower().endswith(ARCHIVES + IMAGES) or os.path.isdir(f)]
    named = set()
    for f in files:
        if f.lower().endswith(".cue"):
            named |= {os.path.abspath(os.path.join(os.path.dirname(f), n)) for n in fbexe_cue_files(f)}
    return [f for f in files if os.path.abspath(f) not in named]


def split_by_game(inputs: list[str]) -> list[list[str]]:
    """Several games' files given at once (a drop of many sets, or a whole rompath), one list per
    game: a set's archive or folder by its name, a CHD by its folder or by the sets MAME has its
    disk in. Sets that are no game of HIRAKU's (a BIOS, a parent) go with every game; what names no
    game (a renamed image, a disc) makes a list of its own, told by its contents. One game (or
    none named): the inputs as they are."""
    known = {g["mame"] for g in all_games() if g.get("mame")}
    files = gather(inputs, known)
    keyed, support, loose, chds = {}, [], [], []
    for f in files:
        if f.lower().endswith(ARCHIVES) or os.path.isdir(f):
            base = os.path.basename(os.path.normpath(f))
            name = base if os.path.isdir(f) else base.rsplit(".", 1)[0]
            if name not in known:
                try:
                    name = mameset.current_name(name, f) or name
                except (OSError, ValueError, zipfile.BadZipFile):
                    pass
            if name in known:
                keyed.setdefault(name, []).append(f)
            else:
                support.append(f)
        elif f.lower().endswith(".chd"):
            chds.append(f)
        else:
            loose.append(f)
    for c in chds:
        folder = os.path.basename(os.path.dirname(os.path.abspath(c)))
        if folder in known:
            keyed.setdefault(folder, []).append(c)
            continue
        try:
            sha1 = mameset.chd_sha1(c)
        except OSError:
            sha1 = None
        owners = [s for s, _ in mameset.disk_owner(sha1)] if sha1 else []
        given = [s for s in owners if s in keyed]
        mine = given or [s for s in owners if s in known]
        if len(set(mine)) == 1 or given:
            keyed.setdefault(mine[0], []).append(c)
        else:
            loose.append(c)
    if len(keyed) + (1 if loose else 0) <= 1:
        return [inputs]
    # a clone reads its parent's disks (MAME looks in the parent's folder): given with the parent,
    # they go with the clone too (its parent's ROMs are found beside its set: copy_set_with_parents)
    sets = mameset.load()
    groups = []
    for name, fs in keyed.items():
        disks = [c for p in (mameset.chain(name)[1:] if name in sets else []) for c in keyed.get(p, [])
                 if c.lower().endswith(IMAGES)]
        groups.append(fs + [c for c in disks if c not in fs] + support)
    return groups + ([loose + support] if loose else [])


def has_program(path: str) -> bool:
    """A CD image with a Firebeat program on it."""
    try:
        with fbexe.Disc(path) as disc:
            fbexe.find_exe(fbexe.Iso9660(disc))
        return True
    except Exception:
        return False


def image_kind(path: str) -> str:
    low = path.lower()
    if low.endswith((".img", ".raw")):
        return "hd"
    if low.endswith(".iso") and os.path.getsize(path) > 900_000_000:
        return "dvd"
    return "cd"


def ingest_auto(args, inputs: list[str]) -> int:
    games = all_games()
    files = gather(inputs, {g["mame"] for g in games if g.get("mame")})
    romsets = [f for f in files if f.lower().endswith(ARCHIVES) or os.path.isdir(f)]
    if args.romset:
        romsets.insert(0, args.romset)
    chds = [f for f in files if f.lower().endswith(".chd")]
    images = [f for f in files if f.lower().endswith(IMAGES) and not f.lower().endswith(".chd")]
    if not romsets and not chds and not images:
        print("ERROR    nothing to ingest: give ROM sets (.zip/.7z), CHDs or disc images")
        return 1

    # the game, when a set or a CHD names it (or --game does)
    by_set = {g["mame"]: g for g in games if g.get("mame")}
    hint = forced_game(args, None)
    romset_for = romsets[0] if hint and romsets else None
    for r in romsets if hint is None else []:
        name = set_name_of(r)
        if name in by_set:
            hint = by_set[name]
            romset_for = r
    if hint is None:
        for c in chds:
            stem = disk_name(c)
            for g in games:
                if stem in (g.get("hdd", {}).get("chd"), g.get("cd", {}).get("chd"), g.get("cd", {}).get("msu_chd"),
                            g.get("cd", {}).get("hdd_chd"), g.get("cd", {}).get("video_chd"),
                            g.get("cd", {}).get("install_chd"), g.get("cd", {}).get("install2_chd")):
                    hint = g
    if hint:
        print(f"game     {hint['id']}: {hint['title']} (from {'the set' if romsets else 'the CHD name'})")

    # CHDs: extracted next to the game's other data (or set aside until the program says which game)
    dest = os.path.join(args.out, hint["id"] if hint else INCOMING)
    media = []   # (path, kind)
    disk_of = {}  # extracted image -> MAME's disk name (a renamed CHD's too)
    # --keep-chd: DJ Main, System 573 and Twinkle read their CHDs in place (runtime/dev/image.h),
    # so nothing is extracted; Firebeat's program CD still is (its program is unpacked from it).
    # --copy-chd (the launcher's): the same, from the CHD's copy in dumps/<game>/ (a hard link
    # where it can be: no copying on the same disk), so the files given can go afterwards
    keep = (args.keep_chd or args.copy_chd or args.place) and hint is not None and hint.get("core") in ("djmain", "sys573", "twinkle")
    for c in chds:
        if keep and args.place == "move":
            p, kind = os.path.join(dest, os.path.basename(c)), chd.info(c).kind
            print(f"image    {os.path.basename(p)} ({kind}, {move_file(c, p)} into {os.path.basename(dest)}/, read there)")
            disk_of[p] = disk_name(c)
            media.append((p, kind))
            continue
        if keep:
            p, kind = os.path.abspath(c), chd.info(c).kind
            if args.copy_chd and args.place != "stay":
                os.makedirs(dest, exist_ok=True)
                to = os.path.join(dest, os.path.basename(c))
                if not (os.path.exists(to) and os.path.getsize(to) == os.path.getsize(p)):
                    if os.path.lexists(to):
                        os.remove(to)
                    try:
                        os.link(os.path.realpath(p), to)
                        how = "linked"
                    except OSError:
                        shutil.copyfile(p, to + ".part")
                        os.replace(to + ".part", to)
                        how = "copied"
                    print(f"image    {os.path.basename(to)} ({kind}, {how} into {os.path.basename(dest)}/, read in place)")
                else:
                    print(f"have     {os.path.basename(to)} ({kind}, read in place)")
                p = to
            else:
                print(f"image    {p} ({kind}, read in place)")
            disk_of[p] = disk_name(c)
            media.append((p, kind))
            continue
        try:
            p, kind = chd.extract(c, dest)
            disk_of[p] = disk_name(c)
        except chd.ChdError as e:
            print(f"ERROR    {e}")
            return 1
        print(f"image    {os.path.relpath(p, ROOT) if p.startswith(ROOT) else p} ({kind}, from {os.path.basename(c)})")
        media.append((p, kind))
    for f in images:
        media.append((f, image_kind(f)))
    for flag, kind in (("hdd", "hd"), ("dvd", "dvd"), ("cd2", "cd2")):
        if getattr(args, flag):
            media.append((getattr(args, flag), kind))

    # Firebeat: a program CD decides the game
    cds = [p for p, k in media if k == "cd"]
    program = next((p for p in cds if has_program(p)), None)
    if program:
        args.disc = program
        args.romset = romsets[0] if romsets else None
        args.hdd = args.hdd or next((p for p, k in media if k == "hd"), None)
        args.dvd = args.dvd or next((p for p, k in media if k == "dvd"), None)
        args.cd2 = args.cd2 or next((p for p in cds if p != program), None)
        rc = ingest_firebeat(args)
        if args.place == "move":
            print("note     a Firebeat game's discs are unpacked into HIRAKU's folder: the files given are left where they are")
        try:
            os.rmdir(os.path.join(args.out, INCOMING))   # only if empty
        except OSError:
            pass
        return rc
    if hint is None:
        print("ERROR    which game is this? No program disc, and no games/*/game.toml has this set or CHD name")
        for p, k in media:
            print(f"         {k:4} {p}")
        return 1
    core = hint.get("core")
    args.romset = romset_for or (romsets[0] if romsets else None)
    if core == "djmain":
        args.hdd = args.hdd or next((p for p, k in media if k == "hd"), None)
        rc = ingest_djmain(args)
        move_set(args, hint["id"]) if rc == 0 else undo_moves()
        return rc
    if core == "sys573":
        # the runtime CD, the multi-session unit's (DrumMania 4th Mix and later) and the network
        # unit's data installer (e-AMUSEMENT GuitarFreaks and DrumMania), by name
        want = hint.get("cd", {})
        stem = lambda p: disk_of.get(p) or os.path.splitext(os.path.basename(p))[0]
        args.msu_cd = next((p for p in cds if stem(p) == want.get("msu_chd")), None) if want.get("msu_chd") else None
        args.hdd_cd = next((p for p in cds if stem(p) == want.get("hdd_chd")), None) if want.get("hdd_chd") else None
        args.install_cd = next((p for p in cds if stem(p) == want.get("install_chd")), None) if want.get("install_chd") else None
        args.install2_cd = next((p for p in cds if stem(p) == want.get("install2_chd")), None) if want.get("install2_chd") else None
        args.cd = next((p for p in cds if stem(p) == want.get("chd")), None) or \
            next((p for p in cds if p not in (args.msu_cd, args.hdd_cd, args.install_cd, args.install2_cd)), None)
        rc = ingest_sys573(args)
        move_set(args, hint["id"]) if rc == 0 else undo_moves()
        return rc
    if core == "twinkle":
        want = hint.get("cd", {})
        stem = lambda p: disk_of.get(p) or os.path.splitext(os.path.basename(p))[0]
        args.cd = next((p for p in cds if stem(p) == want.get("chd")), None)
        # the player's disc: a Video CD, or (6th style on) a DVD, which MAME keeps as a hard disk CHD,
        # so the SPU board's disk is told from it by name
        args.video_cd = next((p for p, k in media if stem(p) == want.get("video_chd")), None)
        hdd_name = hint.get("hdd", {}).get("chd")
        args.hdd = args.hdd or next((p for p, k in media if k == "hd" and stem(p) == hdd_name), None) or \
            next((p for p, k in media if k == "hd" and p != args.video_cd), None)
        hd_chd = next((c for c in chds if stem(c) == hdd_name), None) or \
            next((c for c in chds if chd.info(c).kind == "hd" and stem(c) != want.get("video_chd")), None)
        args.hdd_chs = chd.geometry(hd_chd) if hd_chd else None
        rc = ingest_twinkle(args)
        move_set(args, hint["id"]) if rc == 0 else undo_moves()
        return rc
    # its set alone, for a game added already from its CDs (which said NEXT: add the MAME set): the
    # set's ROMs (the dongle, the SPU program) go in beside them
    outdir = os.path.join(args.out, hint["id"])
    machine_txt = os.path.join(outdir, "machine.txt")
    if romsets and os.path.exists(machine_txt) and os.path.exists(os.path.join(outdir, "program.bin")):
        set_name = set_name_of(romset_for or romsets[0])
        for n in copy_set_with_parents(romset_for or romsets[0], os.path.join(outdir, "rom"), set_name):
            print(f"rom      {n}")
        board, spu = hint.get("board", {}), hint.get("spu", {})
        lacking = [f for f in (board.get("dongle"), spu.get("rom")) if f and not os.path.exists(os.path.join(outdir, "rom", f))]
        if lacking:
            print(f"ERROR    the set has no {', '.join(lacking)}")
            return 1
        with open(machine_txt) as fh:   # (as it was: the set adds files, not settings)
            text = fh.read()
        with open(machine_txt, "w") as fh:
            fh.write(text)
        print(f"wrote    {os.path.relpath(machine_txt, ROOT) if machine_txt.startswith(ROOT) else machine_txt}")
        return 0
    print(f"ERROR    {hint['id']} is a Firebeat game: its program CD is needed (none of the images holds one)")
    return 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="*", help="ROM sets, CHDs, disc/disk images, or folders of them")
    ap.add_argument("--exe", help="a loose HIKARU.EXE / FIREBEAT.EXE (Firebeat)")
    ap.add_argument("--out", default=os.path.join(ROOT, "dumps"), help="output root (default: dumps/)")
    ap.add_argument("--check", metavar="RAW", help="compare the unpacked image to a known-good raw dump")
    ap.add_argument("--romset", metavar="ZIP", help="MAME ROM set (.zip/.7z) to copy into dumps/<game>/rom/")
    ap.add_argument("--hdd", metavar="IMG", help="raw HDD image: on the SPU's ATA bus (beatmania III), or DJ Main's IDE disk")
    ap.add_argument("--dvd", metavar="ISO", help="DVD image on the SPU's ATA bus (pop'n music)")
    ap.add_argument("--cd2", metavar="CUE", help="second disc on the main ATA bus (ParaParaParadise audio CD)")
    ap.add_argument("--bios", metavar="DIR", help="Konami Python: the folder with the arcade BIOS (default: a bios/ near the .py1)")
    ap.add_argument("--keys", metavar="DIR", help="Konami Python: the folder with the MagicGate key files (default: a keys/ near the .py1)")
    ap.add_argument("--game", metavar="ID", help="install as this game (games/ID): a clone from its parent's merged set, "
                    "whose name is the parent's")
    ap.add_argument("--keep-chd", action="store_true", help="DJ Main, System 573, Twinkle: read the CHDs where they are "
                    "instead of extracting them (the game then needs them there)")
    ap.add_argument("--copy-chd", action="store_true", help="DJ Main, System 573, Twinkle: link or copy the CHDs into "
                    "dumps/<game>/ and read them there, instead of extracting them (what the launcher does)")
    ap.add_argument("--place", choices=("move", "stay"), help="the files a game reads as they are (CHDs, disks, a PC "
                    "game's folder) moved into dumps/<game>/ with its ROM set, or left where they are (read there); "
                    "the ROMs are copied either way. Default: linked or copied in")
    ap.add_argument("--nvram-seed", metavar="DIR", help="System 573: a MAME NVRAM folder (nvram/<set>/) with the game "
                    "installed; kept as the game's starting saves (a folder named nvram/<set> is told by itself)")
    args = ap.parse_args()
    # a MAME nvram/<set>/ folder among the inputs is the starting saves, not a set of loose ROMs
    for f in list(args.inputs):
        if os.path.isdir(f) and os.path.basename(os.path.dirname(os.path.normpath(f))) == "nvram":
            args.inputs.remove(f)
            args.nvram_seed = args.nvram_seed or f
    args.disc = None
    if args.exe:
        return ingest_firebeat(args)
    if not args.inputs and not args.romset:
        ap.error("give the game's files (ROM set, CHDs, images), or --exe")
    # Konami Python: the arcade BIOS and the MagicGate keys, loose or in their folders, kept for
    # every Python game (dumps/_python/); a game that waited for them is finished now
    rest = keep_python_support(args, args.inputs)
    if len(rest) != len(args.inputs):
        args.inputs = rest
        waiting = glob.glob(os.path.join(args.out, "*", "machine.pending"))
        rc = complete_pending(args) if waiting else 0
        if not args.inputs:
            if not waiting:
                print("NEXT     add a Konami Python game: it will use them")
            return rc
        print()
    # AVS2 PC games (64- and 32-bit): their contents/ folder (modules/, prop/, data), read where it is
    pc64s = [(f, found) for f in args.inputs if os.path.isdir(f) and (found := contents_folder(f)) is not None]
    if pc64s:
        rc = 0
        for f, (game, contents, soft) in pc64s:
            if args.place == "move":   # the contents folder, into dumps/<game>/
                to = os.path.join(args.out, game["id"], "contents")
                print(f"folder   contents ({move_file(contents, to)} into {game['id']}/)")
                contents = to
            rc |= ingest_pc64(args, game, contents, soft)
            args.inputs.remove(f)
        if not args.inputs:
            return rc
        print()
    # PC games: their folder (prog/ and data/), read where it is
    pcs = [(f, found) for f in args.inputs if os.path.isdir(f) and (found := pc_folder(f)) is not None]
    if pcs:
        rc = 0
        for f, (game, program, data) in pcs:
            if args.place == "move":   # the folder holding prog/ and data/, into dumps/<game>/
                top = os.path.dirname(os.path.normpath(data))
                to = os.path.join(args.out, game["id"], os.path.basename(top))
                print(f"folder   {os.path.basename(top)} ({move_file(top, to)} into {game['id']}/)")
                inside = os.path.commonpath([program, top]) == top   # a revision's folder may be elsewhere
                program, data = os.path.join(to, os.path.relpath(program, top)) if inside else program, os.path.join(to, "data")
            rc |= ingest_pc(args, game, program, data)
            args.inputs.remove(f)
        if not args.inputs:
            return rc
        print()
    # Konami Python: a .py1, or a set's folder (Reliquary's collections keep <set>.py1 beside it;
    # without it, the folder's files say what they are); each game given is added
    py1, folders = [f for f in args.inputs if f.lower().endswith(".py1")], []
    used = set(py1)
    for f in args.inputs:
        if os.path.isdir(f):
            near = os.path.normpath(f) + ".py1"
            found = sorted(glob.glob(os.path.join(f, "*.py1"))) + ([near] if os.path.exists(near) else [])
            if found:
                py1.append(found[0])
                used.add(f)
            elif (g := python1_folder(f)) is not None:
                folders.append((f, g))
                used.add(f)
    if py1 or folders:
        rc = 0
        for f in py1:
            rc |= ingest_python(args, f)
        for f, g in folders:
            rc |= ingest_python1(args, g, os.path.abspath(f))
        # the other games given with them (MAME sets, discs): on as below
        rest = [f for f in args.inputs if f not in used and os.path.normpath(f) + ".py1" not in used]
        if not rest:
            return rc
        print()
        args.inputs = rest
        return rc | ingest_rest(args)
    return ingest_rest(args)


def pc_folder(folder: str) -> tuple[dict, str, str] | None:
    """A PC game's folder (PLAN.md section 30): prog/<date>/<program> and data/, as the cabinet's
    drive holds them (pop'n music 15: popn15.exe), or the folder above it. The game is the one whose
    game.toml names the program; the newest date's program is run. (game, program folder, data)."""
    by_exe = {g["pc"]["exe"].lower(): g for g in all_games() if "pc" in g and "exe" in g["pc"] and g["pc"].get("layout") != "revisions"}
    for top in [folder] + sorted(glob.glob(os.path.join(folder, "*"))):
        prog, data = os.path.join(top, "prog"), os.path.join(top, "data")
        if not (os.path.isdir(prog) and os.path.isdir(data)):
            continue
        for date in sorted(os.listdir(prog), reverse=True):
            d = os.path.join(prog, date)
            if not os.path.isdir(d):
                continue
            for f in os.listdir(d):
                if f.lower() in by_exe:
                    return by_exe[f.lower()], os.path.abspath(d), os.path.abspath(data)
    return revisions_folder(folder)


def revision_game(path: str) -> dict | None:
    """The game a revision's program is (IIDX 9th style, 10th style and RED are all bm2dx.exe): the
    one whose game.toml's mark (a string in its program: its model's module names) is in it. A
    program packed or changed by someone since (RED's JAB: rebuilt in 2006, PECompact) has none of
    its strings in the clear and isn't taken."""
    with open(path, "rb") as fh:
        program = fh.read()
    for g in all_games():
        pc = g.get("pc", {})
        if pc.get("layout") == "revisions" and pc.get("exe", "").lower() == os.path.basename(path).lower() and pc.get("mark", "").encode() in program:
            return g
    return None


def revisions_folder(folder: str) -> tuple[dict, str, str] | None:
    """A PC game kept as program folders by revision beside its data (PLAN.md section 33: beatmania
    IIDX 9th style's C02/JAA ... C02/JAH/bm2dx.exe and C02/data/), found a few folders down (a dump
    can hold the newer revisions apart from the drive's copy). The newest revision is run: the last
    by name (JAH after JAG). (game, program folder, data)."""
    exes = {g["pc"]["exe"].lower() for g in all_games() if g.get("pc", {}).get("layout") == "revisions"}
    programs, datas = [], []
    base = os.path.normpath(folder).count(os.sep)
    for d, dirs, files in os.walk(folder):
        if os.path.normpath(d).count(os.sep) - base >= 4:
            dirs[:] = []
        for f in files:
            if f.lower() in exes and (g := revision_game(os.path.join(d, f))):
                programs.append((os.path.basename(d).upper(), d, g))
        datas += [os.path.join(d, x) for x in dirs if x.lower() == "data"]
    if not programs or not datas:
        return None
    _, program, game = max(programs, key=lambda p: p[0])
    # the data folder beside a program folder, else the only one
    beside = [x for x in datas if any(os.path.dirname(x) == os.path.dirname(p[1]) for p in programs)]
    data = (beside or datas)[0]
    return game, os.path.abspath(program), os.path.abspath(data)


def contents_folder(folder: str) -> tuple[dict, str, dict] | None:
    """An AVS2 PC game's folder (PLAN.md sections 32 and 35): contents/ with modules/<dll> and
    prop/, as the cabinet's drive holds it (the 64-bit PC pop'n: modules/popn.dll; the last 32-bit
    one: modules/popn22.dll; both model M39), or the folder above it. The game is the one whose
    game.toml names the DLL and the model. (game, contents folder, the soft: model, dest, spec, rev,
    ext: prop/ea3-config.xml's, else prop/bootstrap.xml's security and release codes)."""
    games = [g for g in all_games() if g.get("pc", {}).get("loader") in ("pc64", "pc32")]
    for top in [folder, os.path.join(folder, "contents")]:
        config, bootstrap = os.path.join(top, "prop", "ea3-config.xml"), os.path.join(top, "prop", "bootstrap.xml")
        soft = {}
        if os.path.isfile(config):
            with open(config, "rb") as fh:
                text = fh.read().decode("shift_jis", "replace")
            soft = {k: m.group(1).strip() for k in ("model", "dest", "spec", "rev", "ext")
                    if (m := re.search(rf"<{k}\b[^>]*>([^<]*)</{k}>", text))}
        if not soft and os.path.isfile(bootstrap):
            with open(bootstrap, "rb") as fh:
                text = fh.read().decode("shift_jis", "replace")
            code = re.search(r"<security_code>\s*(G.)(\w{3})(\w)(\w)(\w)\s*<", text)
            release = re.search(r"<release_code>\s*(\d{10})\s*<", text)
            if code:
                soft = dict(zip(("model", "dest", "spec", "rev"), code.groups()[1:]))
                soft["ext"] = release.group(1) if release else "?"
        if not soft:
            continue
        for g in games:
            dll = g["pc"].get("dll", "")
            if soft.get("model") == g["pc"].get("model") and os.path.isfile(os.path.join(top, "modules", dll)):
                return g, os.path.abspath(top), soft
    return None


def ingest_pc64(args, game: dict, contents: str, soft: dict) -> int:
    """An AVS2 PC game: its contents folder is read where it is (tens of GB); hiraku-pc runs it in
    the save folder with hiraku-pc64.exe (runtime/pc64/, its data mounted from the folder) or
    hiraku-pc32.exe (runtime/pc32/, its data linked into the save folder)."""
    gid, pc = game["id"], game["pc"]
    outdir = os.path.join(args.out, gid)
    os.makedirs(outdir, exist_ok=True)
    loader = pc.get("loader", "pc64")
    family = game.get("family", "pcpopnhc" if loader == "pc64" else "pcpopn")
    lines = ["core=pc", f"loader={loader}", f"family={family}", f"exe={pc['dll']}", f"contents={contents}"]
    with open(os.path.join(outdir, "machine.txt"), "w") as fh:
        fh.write("\n".join(lines) + "\n")
    code = ":".join(soft.get(k, "?") for k in ("model", "dest", "spec", "rev", "ext"))
    print(f"game     {gid}: {game.get('title', gid)} (a {'64' if loader == 'pc64' else '32'}-bit PC game, {code})")
    print(f"contents {contents} (read where it is{'' if contents.startswith(os.path.abspath(args.out)) else ': keep it there'})")
    print(f"OK       dumps/{gid}/machine.txt")
    return 0


def sound_lists(data: str) -> list[str]:
    """beatmania IIDX (DistorteD on) checks each song's sounds at start-up against the folder's owl.bin:
    "IIDX", the version (13), the count, the kind of sum (2: CRC-32), then per file its name (32
    bytes) and the CRC-32 of its bytes 0x200-0x3ff. A folder without one stops the game at HDD DATA
    ERROR (SOUND) (the owner's DistorteD dump lacks sd_data/0494's): one is written from the files,
    as the others are. The song folders given one."""
    sd = os.path.join(data, "sd_data")
    if not os.path.isdir(sd):
        return []
    folders = sorted(d for d in os.listdir(sd) if os.path.isdir(os.path.join(sd, d)) and re.fullmatch(r"\d{4}", d))
    have = [d for d in folders if os.path.isfile(os.path.join(sd, d, "owl.bin"))]
    if len(have) * 2 < len(folders):   # a game that keeps no such lists
        return []
    with open(os.path.join(sd, have[0], "owl.bin"), "rb") as fh:
        _, version, _, kind = struct.unpack("<4sIII", fh.read(16))
    written = []
    for d in folders:
        if d in have:
            continue
        files = sorted(os.listdir(os.path.join(sd, d)), key=lambda f: (not f.lower().endswith(".2dx"), f))
        out = struct.pack("<4sIII", b"IIDX", version, len(files), kind)
        for f in files:
            with open(os.path.join(sd, d, f), "rb") as fh:
                fh.seek(0x200)
                out += f.encode().ljust(32, b"\0")[:32] + struct.pack("<I", zlib.crc32(fh.read(0x200)))
        try:
            with open(os.path.join(sd, d, "owl.bin"), "wb") as fh:
                fh.write(out)
            written.append(d)
        except OSError as e:
            print(f"warning  sd_data/{d} has no owl.bin and none could be written ({e.strerror}): the game stops at HDD DATA ERROR (SOUND)")
    return written


# The song pictures' packs a game's music select has room for (graph/sys/mdata: a 1024x15360 area in
# DistorteD, which 0.gcz-5.gcz fill)
PICTURE_PACKS = {"iidx13": 6}


def picture_packs(data: str, gid: str) -> int:
    """graph/sys/mdata/system.idx lists the song pictures' packs (the count at +6, then a 32-byte name
    each from +0x18). A DistorteD dump going around lists a seventh, 6.gcz, whose pictures land past
    the area the game keeps for them: the music select crashes. The cabinet's lists six (the same file
    with the count 6 and the seventh name blanked). Such an index is made so, the original kept beside
    it as system.idx.orig. The packs dropped (0 if none)."""
    most = PICTURE_PACKS.get(gid)
    path = os.path.join(data, "graph", "sys", "mdata", "system.idx")
    if most is None or not os.path.isfile(path):
        return 0
    with open(path, "rb") as fh:
        idx = bytearray(fh.read())
    count = struct.unpack_from("<H", idx, 6)[0] if len(idx) >= 8 else 0
    if count <= most or len(idx) < 0x18 + count * 0x20:
        return 0
    names = [bytes(idx[0x18 + i * 0x20:0x38 + i * 0x20]).rstrip(b"\0") for i in range(count)]
    if names != [f"/sys/mdata/{i}.gcz".encode() for i in range(count)]:   # not the one known
        print(f"warning  graph/sys/mdata/system.idx lists {count} picture packs, the game has room for {most}: its music select may crash")
        return 0
    struct.pack_into("<H", idx, 6, most)
    idx[0x18 + most * 0x20:0x18 + count * 0x20] = bytes((count - most) * 0x20)
    try:
        if not os.path.exists(path + ".orig"):
            shutil.copy2(path, path + ".orig")
        with open(path, "wb") as fh:
            fh.write(idx)
    except OSError as e:
        print(f"warning  graph/sys/mdata/system.idx lists {count} picture packs and couldn't be repaired ({e.strerror}): the music select crashes")
        return 0
    return count - most


def ingest_pc(args, game: dict, program: str, data: str) -> int:
    """A PC game: its folder is read where it is (several GB); hiraku-pc copies the program folder
    into the save folder to run it, with HIRAKU's DLLs (runtime/pc/host.cpp)."""
    gid, pc = game["id"], game["pc"]
    outdir = os.path.join(args.out, gid)
    os.makedirs(outdir, exist_ok=True)
    lines = ["core=pc", f"family={game.get('family', 'pcpopn')}", f"exe={pc['exe']}", f"program={program}",
             f"data={data}", f"drive={pc.get('drive', 'D:' + chr(92) + gid)}", f"save_drive={pc.get('save_drive', 'E:' + chr(92))}"]
    lines += [f"{k}={pc[k]}" for k in ("save_drive2", "data_as", "board", "security") if k in pc]
    if "sidcode" in pc:   # the cabinet's software id: the model's, the revision's letter added (JAD: D)
        lines.append(f"sidcode={pc['sidcode']}:{os.path.basename(program)[-1:].upper()}")
    with open(os.path.join(outdir, "machine.txt"), "w") as fh:
        fh.write("\n".join(lines) + "\n")
    print(f"game     {gid}: {game.get('title', gid)} (a PC game)")
    print(f"program  {program}")
    print(f"data     {data} (read where it is{'' if data.startswith(os.path.abspath(args.out)) else ': keep it there'})")
    for d in sound_lists(data):
        print(f"repaired sd_data/{d}/owl.bin (the song's sound list, missing from the dump: made from its files)")
    if (n := picture_packs(data, gid)):
        print(f"repaired graph/sys/mdata/system.idx ({n} picture pack{'s' if n > 1 else ''} past the game's room left out, "
              "as the cabinet's; the original kept as system.idx.orig)")
    print(f"OK       dumps/{gid}/machine.txt")
    return 0


def ingest_rest(args) -> int:
    """main's inputs past the Konami Python 1 games: Python 2, else MAME sets, CHDs, disc images."""
    py2 = [f for f in args.inputs if f.lower().endswith(".py2")]
    for f in args.inputs:
        if os.path.isdir(f):
            near = os.path.normpath(f) + ".py2"
            py2 += sorted(glob.glob(os.path.join(f, "*.py2"))) + ([near] if os.path.exists(near) else [])
    if py2:
        return ingest_python2(args, py2[0])
    # several games at once: each on its own (a set given or forced by hand: all of it is one game)
    if not (args.romset or args.game or args.hdd or args.dvd or args.cd2):
        groups = split_by_game(args.inputs)
        if len(groups) > 1:
            print(f"games    {len(groups)} given: each added in turn")
            rc = 0
            for g in groups:
                print()
                rc |= ingest_auto(copy.copy(args), g)
            return rc
    return ingest_auto(args, args.inputs)


if __name__ == "__main__":
    sys.exit(main())
