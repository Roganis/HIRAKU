#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A System 573 game's manifest (games/<id>/game.toml), from MAME's driver.

  tools/gen573.py SET [--id ID] [--title TITLE] [--family F] [--first-boot TEXT] [--write]

Everything comes from tools/data/mame-bemani.json (tools/mameset.py --refresh with MAME's
ksys573.cpp): which chip each ROM is (its region), the disks, and the machine config:
- the I/O board: k573a, analog (GX700-PWB(F)); k573d, digital (GX894-PWB(B));
- the cassettes: cassx/cassxi/cassy/cassyi/cassyyi/cassxzi/casszi;
- PC Cards, the JVS memory card reader, the multi-session unit.
The family follows MAME's input ports (ddr, gtrfrks, drmn). Prints the manifest; --write writes it
(not over an existing one).
"""
from __future__ import annotations

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mameset  # noqa: E402

ROOT = mameset.ROOT

FAMILIES = {   # MAME input ports -> HIRAKU family, its cabinet comment
    "ddr": ("ddr", "DDR cabinet: 4 panels per player, menu buttons"),
    "gtrfrks": ("gtrf", "GuitarFreaks cabinet: two guitars (neck R/G/B, pick, wailing, effect knob)"),
    "drmn": ("drmn", "DrumMania cabinet: five pads, the bass pedal, select buttons"),
    "ddrsolo": ("ddrsolo", "DDR Solo cabinet: one player, six panels (up-left, up-right too), Select L/R"),
    "dmx": ("dmx", "Dance Maniax cabinet: per player four hand sensors (up, two down rows; left and right), Select L/R"),
    "mamboagg": ("mambo", "Mambo a GoGo cabinet: three congas (left, centre, right), three zones each"),
}
CHIPS = {"x": "X76F041", "xi": "X76F041", "y": "X76F100", "yi": "X76F100", "zi": "ZS01"}
CASSETTE = {"x": "X76F041, no DS2401", "xi": "X76F041 + DS2401", "y": "X76F100, no DS2401",
            "yi": "X76F100 + DS2401", "zi": "ZS01 + DS2401"}
BOARD = {"analog": "GX700-PWB(F): lamp and stage outputs; the music is CD audio",
         "digital": "GX894-PWB(B): FPGA + MAS3507D MP3"}


def q(v: str) -> str:
    return '"' + v + '"'


def line(key: str, value: str, comment: str, width: int = 32) -> str:
    s = f"{key} = {value}"
    return f"{s:<{width}}# {comment}" if comment else s


def generate(name: str, gid: str | None = None, title: str | None = None, family: str | None = None,
             first_boot: str | None = None) -> str:
    sets = mameset.load()
    if name not in sets:
        sys.exit(f"{name}: not a set in {mameset.DATA}")
    m = sets[name]
    c = m.get("k573")
    if not c:
        sys.exit(f"{name}: no System 573 config (refresh tools/data with MAME's ksys573.cpp: tools/mameset.py --refresh)")
    if c["io_board"] not in BOARD:
        sys.exit(f"{name}: the {c['io_board']} I/O board isn't in HIRAKU")
    fam = family or FAMILIES.get(c["inputs"], (None,))[0]
    if not fam:
        sys.exit(f"{name}: MAME's '{c['inputs']}' inputs have no HIRAKU family yet (--family)")
    fam_comment = next((v[1] for v in FAMILIES.values() if v[0] == fam), "")
    gid = gid or name
    # the files, by MAME's region (the default BIOS: roms keep only it)
    regions = m.get("regions", {})
    roms = [r[0] for r in mameset.needed_roms(name)]
    def by_region(region):
        return next((f for f in roms if regions.get(f) == region), None) or \
            next((f for f, rg in regions.items() if rg == region and f in roms), None)
    bios = next(f for f in roms if regions.get(f) == "maincpu:rom") if any(regions.get(f) == "maincpu:rom" for f in roms) \
        else "700a01.22g"
    h8 = by_region("h8_response") or "h8a01.bin"
    eeprom, cid = by_region("cassette:game:eeprom"), by_region("cassette:game:id")
    ieeprom, iid = by_region("cassette:install:eeprom"), by_region("cassette:install:id")
    disks = {d[2]: d[0] for d in m.get("disks", [])}

    year = m.get("year", "")
    head = re.sub(r"\s+", " ", m["title"])
    plain = re.sub(r" \(G.*?\)$", "", head)   # (not in the f-string: Python before 3.12 takes no backslash there)
    out = [f"# {head}, Konami {year}: generated from MAME's ksys573.cpp (tools/gen573.py)",
           f'id = "{gid}"',
           f'title = "{title or plain}"',
           line("family", q(fam), fam_comment, 24),
           line("mame", q(name), "MAME set name, used by the oracle", 24),
           line("core", q("sys573"), f"System 573 (runtime/sys573/, PLAN.md section 19); {c['io_board']} I/O board", 24)]
    if ieeprom:
        out.append('install_cassette = "yes"   # the launcher\'s first boot uses the install cassette (--cassette install)')
    if first_boot:
        out.append(f"first_boot = {q(first_boot)}")
    out += ["", "[roms]",
            line("bios", q(bios), "573 BIOS"),
            line("h8", q(h8), "the H8's start-up response (JVS check)")]
    if eeprom:
        out.append(line("cassette_eeprom", q(eeprom), f"the game cassette's {CHIPS[c['cassette']]}"))
    if cid:
        out.append(line("cassette_id", q(cid), "... and its DS2401 serial number"))
    if ieeprom:
        out.append(line("install_eeprom", q(ieeprom), f"the install cassette's {CHIPS[c.get('install_cassette', c['cassette'])]}"
                        " (first boot: --cassette install)"))
    if iid:
        out.append(line("install_id", q(iid), "... and its DS2401"))
    if c["io_board"] == "digital":
        out.append(line("digital_id", q("digital-id.bin"), "the digital I/O board's DS2401"))
    # chips the set has filled: an installed flash (MAME region 29f016a.*), a set timekeeper (m48t58)
    pre = [f"{rg}:{f}" for f, rg in sorted(regions.items(), key=lambda t: t[1])
           if f in roms and (rg.startswith("29f016a.") or rg == "m48t58")]
    if pre:
        out.append(f"preload = [{', '.join(q(p) for p in pre)}]   # chips the set has filled: device:file (MAME's regions)")
    if c.get("msu"):
        out.append(line("msu_flash", q("flash.20t"), "the multi-session unit's TX3927 firmware (29F400)"))
        out.append(line("msu_rtc", q("m48t58y.6t"), "... and its M48T58Y"))
    out += ["", "[board]",
            line("io_board", q(c["io_board"]), BOARD[c["io_board"]]),
            line("cassette", q(c["cassette"]), f"MAME's cassette variant: {CASSETTE[c['cassette']]} (cass{c['cassette']})")]
    if c.get("install_cassette") and c["install_cassette"] != c["cassette"]:
        out.append(line("install_kind", q(c["install_cassette"]), f"the install cassette's variant: {CASSETTE[c['install_cassette']]}"))
    for slot in ("pccard1", "pccard2"):
        if c.get(slot):
            n = c[slot][:-2]
            out.append(line(slot, q(c[slot]), f"MAME's {slot}_{c[slot]}(): a Fujitsu {n} MiB flash card in slot {slot[-1]}"))
    if c.get("memory_card_reader"):
        out.append(line("memory_card_reader", "true", "the JVS memory card reader"))
    if c.get("msu"):
        out.append(line("msu", "true", "the multi-session unit (GXA25)"))
    out += ["", "[cd]"]
    if "runtime" in disks:
        out.append(line("chd", q(disks["runtime"]), "the runtime CD", 24))
    if "install" in disks:
        out.append(line("install_chd", q(disks["install"]), "MAME's install disc: the first boot's CD (--cd install)", 24))
    if "install2" in disks:
        out.append(line("install2_chd", q(disks["install2"]), "MAME's second install disc", 24))
    if "multisession" in disks:
        out.append(line("msu_chd", q(disks["multisession"]), "the multi-session unit's CD", 24))
    return "\n".join(out) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("set")
    ap.add_argument("--id")
    ap.add_argument("--title")
    ap.add_argument("--family")
    ap.add_argument("--first-boot")
    ap.add_argument("--write", action="store_true", help="write games/<id>/game.toml (never over an existing one)")
    a = ap.parse_args()
    text = generate(a.set, a.id, a.title, a.family, a.first_boot)
    if not a.write:
        print(text, end="")
        return 0
    path = os.path.join(ROOT, "games", a.id or a.set, "game.toml")
    if os.path.exists(path):
        sys.exit(f"{path} exists: not overwritten")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write(text)
    print(f"wrote {os.path.relpath(path, ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
