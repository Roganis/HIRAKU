#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run a Konami Python 1 game in PCSX2 Reliquary as HIRAKU's reference ("oracle").

Every run starts from pristine media: the game's small files (BBSRAM, dongles, boot
ROM, memory-card dongle) are copied fresh, the CHD is linked read-only and the CHD
write overlay is wiped, because Reliquary writes all of them back.

Settings are pinned for repeatability: RTC fixed, MTVU off, software GS renderer,
no audio device. Runs are memory-capped (systemd-run) and time-limited.

usage: run.py <game> [--seconds N] [--snap-every S] [--frames N] [--hash-every N]
              [--interp] [--unlimited] [--renderer 13|14|11] [--out DIR]
  game: a .py1 name in the collection (popn9 … popn14, dstagef, dogstdx, wswe, wswe2k3, pesta, ppool),
        or a .py2 name in the Python 2 sets ($HK_PYTHON2_SETS: <game>.py2 and <game>/ beside it,
        the retail BIOS in bios/, $HK_PY2_BIOS naming it)
With the oracle patch (reliquary-oracle.patch), DIR/framehash.txt gets EE/IOP RAM
hashes every --hash-every vsyncs and --frames stops the run at that vsync.
Snapshots: a savestate over PINE every S seconds; its Screenshot.png and the EE
RAM (from the state zip) are kept in DIR/snap-<t>.{png,ee.bin}.
"""
import argparse, os, shutil, socket, struct, subprocess, sys, time, zipfile, glob, hashlib

# the folders of your sets: Python 1 (kpython1/, bios/, keys/, PCSX2/inis/) and Python 2 (<game>.py2, bios/)
COLL = os.environ.get("HK_PYTHON_SETS", "")
# default: Python-work beside the HIRAKU checkout
WORK = os.environ.get("HK_PYTHON_WORK", os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))), "Python-work"))
BIN = os.environ.get("HK_RELIQUARY", f"{WORK}/pcsx2-reliquary/build/bin/pcsx2-qt")
COLL2 = os.environ.get("HK_PYTHON2_SETS", "")
TEMPLATE = f"{COLL}/PCSX2/inis/PCSX2.ini"


def ini_set(lines, section, key, value):
    """Set key in [section] (adding either if missing). lines: list of str."""
    sec = f"[{section}]"
    try:
        i = lines.index(sec)
    except ValueError:
        lines += ["", sec, f"{key} = {value}"]
        return
    j = i + 1
    while j < len(lines) and not lines[j].startswith("["):
        if lines[j].split("=")[0].strip() == key:
            lines[j] = f"{key} = {value}"
            return
        j += 1
    while j > i + 1 and lines[j - 1].strip() == "":
        j -= 1
    lines.insert(j, f"{key} = {value}")


def prepare(game, data, interp, slot, renderer=13):
    if os.path.exists(data):
        shutil.rmtree(data)
    root = f"{data}/PCSX2"   # -datapath D keeps everything in D/PCSX2
    os.makedirs(f"{root}/inis")
    os.makedirs(f"{data}/bios")
    py2 = bool(COLL2) and os.path.exists(f"{COLL2}/{game}.py2")
    if not py2 and not COLL:
        sys.exit("run.py: set HK_PYTHON_SETS to the folder of your Python 1 sets (and HK_PYTHON2_SETS for Python 2's)")
    sets, ext = (COLL2, "py2") if py2 else (f"{COLL}/kpython1", "py1")
    # Python 2 boots a retail PS2 BIOS (Reliquary's retail BIOS path); Python 1 the COH-H31000
    bios = os.environ.get("HK_PY2_BIOS", "ps2-0190j-20030822.bin") if py2 else "COH-H31000.BIN"
    for f in glob.glob(f"{COLL2}/bios/*" if py2 else f"{COLL}/bios/COH-H31000.*"):
        os.symlink(f, f"{data}/bios/{os.path.basename(f)}")
    gsrc = f"{sets}/{game}"
    gdst = f"{data}/game/{game}"
    os.makedirs(gdst)
    for f in os.listdir(gsrc):
        s = f"{gsrc}/{f}"
        if f.endswith(".chd"):
            os.symlink(s, f"{gdst}/{f}")
        else:
            shutil.copy2(s, f"{gdst}/{f}")
    shutil.copy2(f"{sets}/{game}.{ext}", f"{data}/game/{game}.{ext}")

    lines = open(TEMPLATE, encoding="utf-8").read().splitlines()
    lines = [l for l in lines if l.split("=")[0].strip() not in ("MainWindowGeometry", "MainWindowState")]
    S = ini_set
    S(lines, "UI", "SetupWizardIncomplete", "false")
    S(lines, "UI", "ConfirmShutdown", "false")
    S(lines, "UI", "StartPaused", "false")
    S(lines, "UI", "PauseOnFocusLoss", "false")
    S(lines, "Folders", "Bios", f"{data}/bios")
    for k, f in (("MgChallengeIvFile", "civ"), ("MgCardKeyStoreFile", "cks"),
                 ("MgEncryptedKeyStoreFile", "eks"), ("MgKeyStoreKeyFile", "kek"),
                 ("ArcadeKelfOverrideKbitFile", "arcade_kelf_override_kbit"),
                 ("ArcadeKelfOverrideKcFile", "arcade_kelf_override_kc")):
        S(lines, "Security", k, f"{COLL}/keys/{f}.bin")
    S(lines, "Filenames", "BIOS", bios)
    S(lines, "Filenames", "ArcadeBIOS", "COH-H31000.BIN")
    S(lines, "EmuCore", "EnablePINE", "true")
    S(lines, "EmuCore", "PINESlot", str(slot))
    S(lines, "EmuCore", "EnablePatches", "false")
    S(lines, "EmuCore", "ManuallySetRealTimeClock", "true")
    # HIRAKU's cabinet clock (runtime/python/core.cpp): 2003-06-01 12:00 JST to the games
    for k, v in (("RtcYear", 3), ("RtcMonth", 5), ("RtcDay", 31), ("RtcHour", 18), ("RtcMinute", 0), ("RtcSecond", 0)):
        S(lines, "EmuCore", k, str(v))
    S(lines, "EmuCore", "SavestateCompressionType", "0")   # plain zip entries: quick to read back
    S(lines, "EmuCore/Speedhacks", "vuThread", "false")
    S(lines, "EmuCore/GS", "Renderer", str(renderer))     # 13 software (default)
    S(lines, "EmuCore/GS", "VsyncEnable", "0")
    S(lines, "SPU2/Output", "Backend", "Null")
    S(lines, "MemoryCards", "Slot2_Enable", "false")   # a Python cabinet has only the dongle, in slot 1
    if py2:
        # Python 2's boot program waits for the network: PCSX2's user-mode Sockets adapter, its
        # own DHCP and DNS answering the game
        for k, v in (("EthApi", "Sockets"), ("EthDevice", "Auto"), ("InterceptDHCP", "true"),
                     ("ModeDNS1", "Internal"), ("ModeDNS2", "Internal")):
            S(lines, "DEV9/Eth", k, v)
    S(lines, "Logging", "EnableFileLogging", "true")
    S(lines, "Logging", "EnableSystemConsole", "true")
    S(lines, "Logging", "EnableEEConsole", "true")
    S(lines, "Logging", "EnableIOPConsole", "true")
    S(lines, "Logging", "EnableTimestamps", "true")
    if interp:
        for k in ("EnableEE", "EnableIOP", "EnableVU0", "EnableVU1"):
            S(lines, "EmuCore/CPU/Recompiler", k, "false")
    open(f"{root}/inis/PCSX2.ini", "w", encoding="utf-8").write("\n".join(lines) + "\n")
    return f"{data}/game/{game}.{ext}"


class Pine:
    def __init__(self, slot):
        rd = os.environ.get("XDG_RUNTIME_DIR", "/tmp")
        self.path = f"{rd}/pcsx2.sock" + ("" if slot == 28011 else f".{slot}")
        self.s = None

    def connect(self, timeout=60):
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(self.path)
                self.s = s
                return True
            except OSError:
                time.sleep(0.5)
        return False

    def call(self, op, payload=b""):
        msg = struct.pack("<IB", 5 + len(payload), op) + payload
        self.s.sendall(msg)
        hdr = b""
        while len(hdr) < 4:
            hdr += self.s.recv(4 - len(hdr))
        n = struct.unpack("<I", hdr)[0]
        body = b""
        while len(body) < n - 4:
            body += self.s.recv(n - 4 - len(body))
        return body[0], body[1:]

    def read32(self, addr):
        r, d = self.call(2, struct.pack("<I", addr))
        return struct.unpack("<I", d)[0] if r == 0 else None

    def status(self):
        r, d = self.call(0xF)
        return struct.unpack("<I", d)[0] if r == 0 else None   # 0 running, 1 paused, 2 shutdown

    def save_state(self, n):
        return self.call(9, bytes([n]))[0] == 0


def emu_pids(data):
    out = []
    for pid in os.listdir("/proc"):
        try:
            cl = open(f"/proc/{pid}/cmdline", "rb").read().split(b"\0")
        except OSError:
            continue
        if cl and cl[0].endswith(b"pcsx2-qt") and data.encode() in cl:
            out.append(int(pid))
    return out


def cpu_seconds(pid):
    """utime + stime of the whole process (all threads), in seconds."""
    f = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
    return (int(f[11]) + int(f[12])) / os.sysconf("SC_CLK_TCK")


def scrub_keys(path):
    """Reliquary prints the derived MagicGate keystore; keep it out of kept logs."""
    try:
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines(True)
    except OSError:
        return
    out, skip = [], False
    for l in lines:
        if "===== KEYSTORE" in l:
            skip = True
        if not skip and not l.lstrip().startswith("MG_"):
            out.append(l)
        if "END KEYSTORE" in l:
            skip = False
    open(path, "w", encoding="utf-8").writelines(out)


def kill_emu(data, sig=15):
    """Signal the emulator started for `data` (not the shell that started this script)."""
    for pid in os.listdir("/proc"):
        try:
            cl = open(f"/proc/{pid}/cmdline", "rb").read().split(b"\0")
        except OSError:
            continue
        if cl and cl[0].endswith(b"pcsx2-qt") and data.encode() in cl:
            os.kill(int(pid), sig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game")
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--snap-every", type=float, default=15)
    ap.add_argument("--interp", action="store_true", help="interpreters instead of the recompilers")
    ap.add_argument("--mem", default="6G")
    ap.add_argument("--frames", type=int, default=0, help="stop at this vsync (needs the oracle patch)")
    ap.add_argument("--hash-every", type=int, default=60, help="EE/IOP RAM hash period in vsyncs (oracle patch)")
    ap.add_argument("--unlimited", action="store_true", help="no frame limiter")
    ap.add_argument("--renderer", type=int, default=13, help="GS renderer: 13 software, 14 Vulkan, 11 null")
    ap.add_argument("--out")
    a = ap.parse_args()
    slot = 28100 + (os.getpid() % 800)
    data = f"{WORK}/oracle/data-{a.game}"
    out = a.out or f"{WORK}/oracle/out-{a.game}"
    os.makedirs(out, exist_ok=True)
    target = prepare(a.game, data, a.interp, slot, a.renderer)
    env = dict(os.environ, QT_QPA_PLATFORM=os.environ.get("HK_QPA", "xcb"))   # offscreen has no window for the GS
    env["HK_FRAMEHASH"] = f"{out}/framehash.txt"
    env["HK_FRAMEHASH_EVERY"] = str(a.hash_every)
    if a.frames:
        env["HK_STOP_FRAME"] = str(a.frames)
    cmd = ["systemd-run", "--user", "--scope", "-q", "-p", f"MemoryMax={a.mem}", "-p", "MemorySwapMax=1G",
           "timeout", str(int(a.seconds + 60)), BIN, "-datapath", data, "-nogui", "-batch",
           "-logfile", f"{out}/emu.log", "-earlyconsolelog", *(["-unlimited"] if a.unlimited else []), "--", target]
    print(" ".join(cmd), flush=True)
    t0 = time.time()
    p = subprocess.Popen(cmd, env=env, stdout=open(f"{out}/stdout.txt", "w"), stderr=subprocess.STDOUT)
    pine = Pine(slot)
    if not pine.connect():
        print("PINE: no connection", flush=True)
    nsnap = 0
    cpu0 = None   # (wall, cpu) at 30 s, after boot and shader set-up
    next_snap = a.snap_every
    while p.poll() is None and time.time() - t0 < a.seconds:
        time.sleep(0.25)
        el = time.time() - t0
        if cpu0 is None and el >= 30 and emu_pids(data):
            cpu0 = (time.time(), cpu_seconds(emu_pids(data)[0]))
        if a.snap_every > 0 and pine.s and el >= next_snap:
            next_snap += a.snap_every
            nsnap += 1
            before = set(glob.glob(f"{data}/PCSX2/sstates/*"))
            ok = pine.save_state(nsnap % 10)
            for _ in range(40):
                time.sleep(0.25)
                new = set(glob.glob(f"{data}/PCSX2/sstates/*")) - before
                if new:
                    break
            new = sorted(set(glob.glob(f"{data}/PCSX2/sstates/*")) - before, key=os.path.getmtime)
            if not ok or not new:
                print(f"t={el:.0f}s save_state failed", flush=True)
                continue
            time.sleep(1.0)
            try:
                z = zipfile.ZipFile(new[-1])
                tag = f"{out}/snap-{int(el):04d}"
                names = z.namelist()
                if "Screenshot.png" in names:
                    open(tag + ".png", "wb").write(z.read("Screenshot.png"))
                ee = [n for n in names if n.lower().startswith("eememory")]
                h = ""
                if ee:
                    b = z.read(ee[0])
                    open(tag + ".ee.bin", "wb").write(b)
                    h = hashlib.sha1(b).hexdigest()[:12]
                print(f"t={el:.0f}s snap {os.path.basename(tag)} ee={h}", flush=True)
            except Exception as e:  # a state still being written
                print(f"t={el:.0f}s snap read failed: {e}", flush=True)
    if cpu0 and emu_pids(data):
        w, c = time.time() - cpu0[0], cpu_seconds(emu_pids(data)[0]) - cpu0[1]
        print(f"host CPU from 30 s on: {c:.1f} s over {w:.1f} s wall = {100 * c / w:.0f} % of one core", flush=True)
    if p.poll() is None:
        kill_emu(data)
        try:
            p.wait(20)
        except subprocess.TimeoutExpired:
            kill_emu(data, 9)
    for f in (f"{out}/emu.log", f"{out}/stdout.txt", *glob.glob(f"{data}/PCSX2/logs/*")):
        scrub_keys(f)
    print(f"exit {p.returncode} after {time.time() - t0:.0f}s; log {out}/emu.log", flush=True)


if __name__ == "__main__":
    main()
