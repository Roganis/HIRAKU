#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Updates a HIRAKU package (tools/release.sh's) to the latest GitHub release.

    tools/update.py check [--save FILE]    the latest release, as one line of JSON (--save: also
                                           into FILE, with the time of the check, for the games)
    tools/update.py download DIR           its package for this system into DIR, checked against
                                           GitHub's SHA-256 and unpacked (DIR/new/hiraku);
                                           prints "progress DONE TOTAL" lines, then "ready VERSION".
                                           A package GitHub gives no SHA-256 for isn't taken
    tools/update.py apply ROOT NEW [--wait PID] [--restart]
                                           NEW's files over ROOT's, after process PID has ended;
                                           --restart starts ROOT's launcher afterwards
    tools/update.py recover ROOT [--wait PID] [--restart]
                                           an apply that was cut short (a kill, a power cut), put
                                           right: finished if its new files were all in the folder,
                                           undone if not

The launcher (launcher/updater.cpp) runs them. apply runs from the new package (its own Python and
this script), so none of the files it replaces is in use. It never touches dumps/, user/ or out/.
The package's VERSION file says which release it is, its MANIFEST.txt which files are its own:
those of the old package that the new one no longer has are taken out too.

An apply is one of two things, whenever it is stopped: the old version, whole, or the new one.

    1. the new files are copied into ROOT/update-new (the slow part; ROOT itself isn't touched),
       and sent to the disk
    2. each of ROOT's files goes to ROOT/update-old and the new one in its place: renames only
    3. done: the launcher removes update-old at its next start

ROOT/update-journal says how far it is, with the files' names. Cut during 1, the old version is
whole: update-new is dropped. Cut during 2, the new files are all in ROOT: the renames are
finished. A rename that fails (a file still in use) puts back what was moved, and a cut then goes
on putting back. The next apply or recover does it; the launcher starts one when it finds the
journal, and removes nothing of an update while there is one. ROOT/update-lock is held by the
process doing any of this: one at a time, and the launcher can tell.

HK_UPDATE_API: another URL for the latest release's JSON (testing).
HK_UPDATE_STOP_AT=N, HK_UPDATE_FAIL_AT=N: apply and recover end as killed, or fail, at their Nth
file (testing).
"""
import errno
import hashlib
import json
import os
import shutil
import ssl
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile

API = os.environ.get("HK_UPDATE_API") or "https://api.github.com/repos/Roganis/HIRAKU/releases/latest"
ASSET = "hiraku-windows-x86_64.zip" if sys.platform == "win32" else "hiraku-linux-x86_64.tar.xz"
OLD, STAGE, JOURNAL, LOCK = "update-old", "update-new", "update-journal", "update-lock"
KEEP = {"dumps", "user", "out", "update", OLD, STAGE, JOURNAL, LOCK}   # the user's and the updater's, never replaced
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def version_of(text: str) -> tuple:
    """"v0.1.5" -> (0, 1, 5); anything else sorts first"""
    try:
        return tuple(int(p) for p in text.strip().lstrip("vV").split("."))
    except ValueError:
        return ()


def current_version(root: str) -> str:
    try:
        with open(os.path.join(root, "VERSION")) as f:
            return f.read().strip()
    except OSError:
        return ""


def tls() -> ssl.SSLContext:
    ctx = ssl.create_default_context()
    if not ctx.cert_store_stats().get("x509_ca"):
        # a Python built elsewhere (the Linux package's) may not know where this distribution keeps
        # its certificates
        for f in ("/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                  "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem", "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem"):
            if os.path.exists(f):
                ctx.load_verify_locations(f)
                break
    return ctx


def fetch(url: str):
    req = urllib.request.Request(url, headers={"User-Agent": "HIRAKU-updater", "Accept": "application/vnd.github+json"})
    if url.startswith("file:"):
        return urllib.request.urlopen(req)
    return urllib.request.urlopen(req, timeout=30, context=tls())


def latest() -> dict:
    with fetch(API) as r:
        rel = json.load(r)
    asset = next((a for a in rel.get("assets", []) if a.get("name") == ASSET), None)
    cur = current_version(ROOT)
    out = {"current": cur, "latest": rel.get("tag_name", ""), "name": rel.get("name") or rel.get("tag_name", ""),
           "notes": rel.get("body") or "", "page": rel.get("html_url", ""), "asset": ASSET}
    out["newer"] = bool(cur) and bool(asset) and version_of(out["latest"]) > version_of(cur)
    if asset:
        digest = asset.get("digest") or ""
        out.update(url=asset["browser_download_url"], size=asset.get("size", 0),
                   sha256=digest[7:] if digest.startswith("sha256:") else "")
    return out


def cmd_check(save: str = "") -> int:
    rel = latest()
    print(json.dumps(rel), flush=True)
    if save:
        # what the games read (runtime/host/notice.cpp): a new release, without asking GitHub again
        os.makedirs(os.path.dirname(os.path.abspath(save)), exist_ok=True)
        keep = {k: rel.get(k) for k in ("current", "latest", "name", "newer")}
        keep["checked"] = int(time.time())
        with open(save + ".tmp", "w", encoding="utf-8") as f:
            json.dump(keep, f)
        os.replace(save + ".tmp", save)
    return 0


def cmd_download(dest: str) -> int:
    rel = latest()
    if not rel.get("url"):
        sys.exit(f"the release {rel['latest']} has no {ASSET}")
    if len(rel.get("sha256") or "") != 64:
        # never installed unchecked: what is unpacked here replaces the programs this PC runs
        sys.exit(f"GitHub gives no SHA-256 for {ASSET} of the release {rel['latest']}, so the download can't be "
                 f"checked and isn't installed. The release's page has the package: {rel.get('page', '')}")
    os.makedirs(dest, exist_ok=True)
    total = rel.get("size") or 0
    if shutil.disk_usage(dest).free < 4 * total:
        sys.exit(f"not enough free space in {dest}: the update needs about {4 * total // 2**20} MB")
    pkg = os.path.join(dest, ASSET)
    h, done, last = hashlib.sha256(), 0, 0.0
    with fetch(rel["url"]) as r, open(pkg, "wb") as f:
        total = int(r.headers.get("Content-Length") or total)
        while chunk := r.read(1 << 16):
            f.write(chunk)
            h.update(chunk)
            done += len(chunk)
            if time.monotonic() - last > 0.2:
                print(f"progress {done} {total}", flush=True)
                last = time.monotonic()
    print(f"progress {done} {total}", flush=True)
    if h.hexdigest() != rel["sha256"].lower():
        os.remove(pkg)
        sys.exit("the download doesn't match its SHA-256 on GitHub: try again")
    new = os.path.join(dest, "new")
    shutil.rmtree(new, ignore_errors=True)
    print("unpacking", flush=True)
    if pkg.endswith(".zip"):
        with zipfile.ZipFile(pkg) as z:
            for n in z.namelist():
                if n.startswith(("/", "\\")) or ".." in n.replace("\\", "/").split("/"):
                    sys.exit(f"the package has a file outside its folder: {n}")
            z.extractall(new)
    else:
        with tarfile.open(pkg) as t:
            t.extractall(new, filter="data")
    os.remove(pkg)
    if not os.path.isfile(os.path.join(new, "hiraku", "VERSION")):
        sys.exit("the package has no hiraku/VERSION")
    print(f"ready {rel['latest']}", flush=True)
    return 0


def files_of(folder: str) -> list[str]:
    """the package's own files: its MANIFEST.txt, else what is in it"""
    m = os.path.join(folder, "MANIFEST.txt")
    if os.path.isfile(m):
        with open(m, encoding="utf-8") as f:
            names = [l.rstrip("\r\n") for l in f if l.strip()]
    else:
        names = []
        for d, _, fs in os.walk(folder):
            names += [os.path.relpath(os.path.join(d, x), folder).replace(os.sep, "/") for x in fs]
    return [n for n in names if n.split("/")[0] not in KEEP and ".." not in n.split("/")]


def wait_for(pid: int) -> None:
    if sys.platform == "win32":
        import ctypes
        k = ctypes.windll.kernel32
        h = k.OpenProcess(0x00100000, False, pid)   # SYNCHRONIZE
        if h:
            k.WaitForSingleObject(h, 60000)
            k.CloseHandle(h)
        return
    for _ in range(600):
        try:
            os.kill(pid, 0)
        except OSError:
            return
        time.sleep(0.1)


_steps = 0


def step() -> None:
    """(testing: the Nth file of an apply or a recover is where it is killed, or fails)"""
    global _steps
    _steps += 1
    if os.environ.get("HK_UPDATE_STOP_AT") == str(_steps):
        os._exit(9)
    if os.environ.get("HK_UPDATE_FAIL_AT") == str(_steps):
        raise OSError("a file that can't be moved (HK_UPDATE_FAIL_AT)")


def try_lock(fd: int):
    """True: the file is this process's; False: another has it; None: there are no locks here"""
    if sys.platform == "win32":
        # (LockFileEx itself: msvcrt.locking's error doesn't always say which of the two it was)
        import ctypes
        import msvcrt
        from ctypes import wintypes

        class Overlapped(ctypes.Structure):
            _fields_ = [("Internal", ctypes.c_void_p), ("InternalHigh", ctypes.c_void_p), ("Offset", wintypes.DWORD),
                        ("OffsetHigh", wintypes.DWORD), ("hEvent", wintypes.HANDLE)]

        k = ctypes.WinDLL("kernel32", use_last_error=True)
        k.LockFileEx.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
                                 ctypes.POINTER(Overlapped)]
        k.LockFileEx.restype = wintypes.BOOL
        at = Overlapped()
        if k.LockFileEx(msvcrt.get_osfhandle(fd), 1 | 2, 0, 1, 0, ctypes.byref(at)):   # at once, exclusive; the first byte
            return True
        return False if ctypes.get_last_error() == 33 else None   # ERROR_LOCK_VIOLATION
    import fcntl
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except OSError as e:
        return False if e.errno in (errno.EAGAIN, errno.EWOULDBLOCK) else None


def take_lock(root: str):
    """ROOT/update-lock, held until let_go() or this process's end: one apply or recover at a time
    on a package, and the launcher can tell one is running (hk::sys::file_lock_held). None where
    there are no locks (the file can't be made, a file system without them): as before there were"""
    try:
        fd = os.open(os.path.join(root, LOCK), os.O_RDWR | os.O_CREAT, 0o666)
    except OSError:
        return None
    said = False
    while True:
        got = try_lock(fd)
        if got:
            return fd
        if got is None:
            os.close(fd)
            return None
        if not said:
            print("another update of this folder is running: waiting for it", flush=True)
            said = True
        time.sleep(0.2)


def let_go(lock) -> None:
    if lock is not None:
        os.close(lock)


def to_disk(folder: str) -> None:
    """Everything written to folder's file system, on the disk"""
    if sys.platform.startswith("linux"):
        try:
            import ctypes
            fd = os.open(folder, os.O_RDONLY)
            try:
                if ctypes.CDLL(None, use_errno=True).syncfs(fd) == 0:   # (that file system alone: another's
                    return                                               # slow disk isn't waited for)
            finally:
                os.close(fd)
        except (OSError, AttributeError):
            pass
    os.sync()


def sync_dir(folder: str) -> None:
    if sys.platform != "win32":
        fd = os.open(folder, os.O_RDONLY)
        try:
            os.fsync(fd)
        except OSError:
            pass
        finally:
            os.close(fd)


def read_journal(root: str):
    """ROOT/update-journal: None if there is none"""
    try:
        with open(os.path.join(root, JOURNAL), encoding="utf-8") as f:
            j = json.load(f)
    except FileNotFoundError:
        return None
    except ValueError:
        j = None
    if not isinstance(j, dict) or j.get("state") not in ("copy", "swap", "back") or \
            not all(isinstance(j.get(k), list) for k in ("put", "drop")):
        raise OSError(f"{os.path.join(root, JOURNAL)} can't be read: what state the folder is in isn't known. "
                      "Unpack the release's package over it")
    return j


def write_journal(root: str, j: dict) -> None:
    """whole and on the disk before anything that depends on it is done"""
    path = os.path.join(root, JOURNAL)
    with open(path + ".tmp", "w", encoding="utf-8") as f:
        json.dump(j, f)
        f.flush()
        os.fsync(f.fileno())
    os.replace(path + ".tmp", path)
    sync_dir(root)


def move(src: str, dst: str, tries: int = 20) -> None:
    step()
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    for attempt in range(tries):   # Windows: a file can stay locked a moment after its program ends
        try:
            os.replace(src, dst)
            return
        except PermissionError:
            if attempt == tries - 1:
                raise
            time.sleep(0.5)


def swap(root: str, j: dict) -> None:
    """Each old file to update-old, each new one from update-new into its place. Picks up where a
    cut one stopped: a new file no longer in update-new is in its place"""
    old, stage = os.path.join(root, OLD), os.path.join(root, STAGE)

    def aside(rel: str) -> None:
        dst, b = os.path.join(root, rel), os.path.join(old, rel)
        if os.path.lexists(dst) and not os.path.lexists(b):   # (there already: that one is the old file)
            move(dst, b)

    for rel in j["drop"]:   # the old package's files the new one doesn't have, and their folders if left empty
        aside(rel)
        d = os.path.dirname(rel)
        while d:
            try:
                os.rmdir(os.path.join(root, d))
            except OSError:
                break
            d = os.path.dirname(d)
    for rel in j["put"]:
        src = os.path.join(stage, rel)
        if os.path.lexists(src):
            aside(rel)
            move(src, os.path.join(root, rel))


def undo(root: str, j: dict) -> None:
    """swap() the other way: each new file back to update-new, each old one back to its place.
    Every file is tried; picks up where a cut one stopped"""
    old, stage = os.path.join(root, OLD), os.path.join(root, STAGE)
    failed = None
    for rel in reversed(j["put"]):
        src, dst, b = os.path.join(stage, rel), os.path.join(root, rel), os.path.join(old, rel)
        try:
            if not os.path.lexists(src) and os.path.lexists(dst):   # in its place: out again
                move(dst, src, 1)
            if os.path.lexists(b):
                move(b, dst, 1)
        except OSError as e:
            failed = failed or e
    for rel in j["drop"]:
        dst, b = os.path.join(root, rel), os.path.join(old, rel)
        try:
            if os.path.lexists(b) and not os.path.lexists(dst):
                move(b, dst, 1)
        except OSError as e:
            failed = failed or e
    if failed:
        raise OSError(f"the old files couldn't all be put back ({failed}): it is tried again at the next start")


def drop_stage(root: str, j: dict) -> None:
    """The new files that never went in, removed: the folder is the old version, whole"""
    if j["state"] != "copy":
        j["state"] = "copy"   # (from here update-new is only something to remove, wherever this is cut)
        write_journal(root, j)
    shutil.rmtree(os.path.join(root, STAGE), ignore_errors=True)
    os.remove(os.path.join(root, JOURNAL))


def settle(root: str, j: dict) -> None:
    """From a journal that says "swap": the new version in, or everything back"""
    try:
        swap(root, j)
    except BaseException:
        j["state"] = "back"
        write_journal(root, j)
        undo(root, j)
        drop_stage(root, j)
        raise
    os.remove(os.path.join(root, JOURNAL))   # done: the folder is the new version
    sync_dir(root)
    shutil.rmtree(os.path.join(root, STAGE), ignore_errors=True)


def recover(root: str) -> str:
    """What an apply that was cut short left, put right. "" nothing was left; "new" its renames
    were finished: the folder is the version it was putting in; "old" it was undone: the folder is
    the version from before; "other" the folder is neither by now, and was left as it is"""
    j = read_journal(root)
    if j is None:
        return ""
    v = current_version(root)
    if v and v not in (j.get("from"), j.get("to")):
        # a package unpacked over the folder since: the journal is about files that are no longer
        # there, and what waits in update-new is older than what the folder has
        drop_stage(root, j)
        return "other"
    if j["state"] == "swap":
        settle(root, j)
        return "new"
    if j["state"] == "back":
        undo(root, j)
    drop_stage(root, j)
    return "old"


def apply(root: str, new: str) -> None:
    old, stage = os.path.join(root, OLD), os.path.join(root, STAGE)
    for d in (old, stage):   # the last update's
        shutil.rmtree(d, ignore_errors=True)
        if os.path.exists(d):
            raise OSError(f"{d} can't be removed")
    put = files_of(new)
    drop = sorted(set(files_of(root)) - set(put)) if os.path.isfile(os.path.join(root, "MANIFEST.txt")) else []
    need = sum(os.lstat(os.path.join(new, rel)).st_size for rel in put)
    if shutil.disk_usage(root).free < need + (16 << 20):
        raise OSError(f"not enough free space in {root}: the new files need {need // 2**20 + 16} MB")
    j = {"state": "copy", "from": current_version(root), "to": current_version(new), "put": put, "drop": drop}
    write_journal(root, j)
    # 1. the new files beside the old ones, in the same folder (so that 2 is renames) and on the disk
    print(f"copying {len(put)} files", flush=True)
    try:
        for rel in put:
            step()
            src, dst = os.path.join(new, rel), os.path.join(stage, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst, follow_symlinks=False)
            if sys.platform == "win32":   # (each file; elsewhere all at once, below)
                try:
                    with open(dst, "rb+") as f:
                        os.fsync(f.fileno())
                except OSError:   # (a read-only file: Windows writes it in its own time)
                    pass
        if sys.platform != "win32":
            to_disk(stage)
    except BaseException:
        drop_stage(root, j)
        raise
    # 2. from here the new version is the folder's: a cut is finished, not undone
    j["state"] = "swap"
    write_journal(root, j)
    print("putting them in", flush=True)
    settle(root, j)


def restart_launcher(root: str, args: list) -> None:
    launcher = os.path.join(root, "bin", "hiraku.exe" if sys.platform == "win32" else "hiraku")
    if sys.platform == "win32":
        subprocess.Popen([launcher] + args, cwd=root, creationflags=0x00000008 | 0x00000200, close_fds=True)   # detached, own group
    else:
        subprocess.Popen([launcher] + args, cwd=root, start_new_session=True, stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def log_line(root: str, msg: str) -> None:
    print(msg, flush=True)
    try:
        with open(os.path.join(root, "user" if os.path.isdir(os.path.join(root, "user")) else "", "update.log"), "a",
                  encoding="utf-8") as f:
            f.write(msg + "\n")
    except OSError:
        pass


def failure(root: str, e: Exception) -> str:
    """what an apply or a recover that failed left"""
    if os.path.lexists(os.path.join(root, JOURNAL)):
        return f"failed, and the folder is part old, part new: {e}"
    return f"failed, the old version kept: {e}"


def cmd_apply(root: str, new: str, wait: int, restart: bool) -> int:
    root, new = os.path.abspath(root), os.path.abspath(new)
    lock = take_lock(root)   # (before the wait: a launcher started meanwhile sees an update is running)
    if wait:
        wait_for(wait)
    was, now = current_version(root), current_version(new)
    try:
        # an apply cut short before this one: put right first. If it had this version's files all in
        # the folder, finishing it is the update
        was = (read_journal(root) or {}).get("from") or was
        left = recover(root)
        if left == "new" and current_version(root) == now:
            msg = "done (an update cut short was finished)"
        else:
            apply(root, new)
            msg = "done"
        ok = True
    except Exception as e:
        msg = failure(root, e)
        ok = False
    msg = f"{time.strftime('%Y-%m-%d %H:%M:%S')} {was or '?'} -> {now}: {msg}"
    log_line(root, msg)
    let_go(lock)   # (the launcher started next must not find an update running)
    if restart:
        restart_launcher(root, ["--updated" if ok else "--update-failed", now if ok else msg])
    return 0 if ok else 1


def cmd_recover(root: str, wait: int, restart: bool) -> int:
    root = os.path.abspath(root)
    lock = take_lock(root)
    if wait:
        wait_for(wait)
    left = ""
    try:
        left = recover(root)
        msg = {"new": f"finished, this is {current_version(root)}",
               "old": "its new files weren't all in the folder, so it was undone: the version from before is whole",
               "other": f"the folder has been replaced since (it is {current_version(root)}): left as it is",
               "": "nothing was left to do"}[left]
        ok = True
    except Exception as e:
        msg = failure(root, e)
        ok = False
    msg = f"{time.strftime('%Y-%m-%d %H:%M:%S')} an update that was cut short: {msg}"
    log_line(root, msg)
    let_go(lock)
    if restart:
        restart_launcher(root, ["--updated", current_version(root)] if left == "new" else
                         ["--update-failed", msg] if left == "old" or not ok else [])
    return 0 if ok else 1


def main() -> int:
    a = sys.argv[1:]
    try:
        if a[:1] == ["check"]:
            return cmd_check(a[a.index("--save") + 1] if "--save" in a[:-1] else "")
        if a[:1] == ["download"] and len(a) == 2:
            return cmd_download(a[1])
        if a[:1] == ["apply"] and len(a) >= 3:
            wait = int(a[a.index("--wait") + 1]) if "--wait" in a else 0
            return cmd_apply(a[1], a[2], wait, "--restart" in a)
        if a[:1] == ["recover"] and len(a) >= 2:
            wait = int(a[a.index("--wait") + 1]) if "--wait" in a else 0
            return cmd_recover(a[1], wait, "--restart" in a)
    except OSError as e:   # no network, GitHub not answering, a full disk
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
