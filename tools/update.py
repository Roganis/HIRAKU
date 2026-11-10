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

The launcher (launcher/updater.cpp) runs all three. apply runs from the new package (its own
Python and this script), so none of the files it replaces is in use. It never touches dumps/,
user/ or out/; the files it replaces go to ROOT/update-old, and back if anything fails. The
package's VERSION file says which release it is, its MANIFEST.txt which files are its own:
those of the old package that the new one no longer has are taken out too.

HK_UPDATE_API: another URL for the latest release's JSON (testing).
"""
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
KEEP = {"dumps", "user", "out", "update", "update-old"}   # the user's, never replaced
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


def apply(root: str, new: str) -> None:
    old = os.path.join(root, "update-old")
    shutil.rmtree(old, ignore_errors=True)
    if os.path.exists(old):
        raise OSError(f"{old} can't be removed")
    put, moved = [], []   # files copied in; (file, where the old one went)

    def aside(rel: str) -> None:
        dst = os.path.join(root, rel)
        if os.path.lexists(dst):
            b = os.path.join(old, rel)
            os.makedirs(os.path.dirname(b), exist_ok=True)
            os.replace(dst, b)
            moved.append((dst, b))

    try:
        incoming = files_of(new)
        for rel in incoming:
            aside(rel)
            dst = os.path.join(root, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(os.path.join(new, rel), dst, follow_symlinks=False)
            put.append(dst)
        if os.path.isfile(os.path.join(root, "update-old", "MANIFEST.txt")):
            for rel in set(files_of(old)) - set(incoming):
                aside(rel)
    except BaseException:
        for f in reversed(put):
            try:
                os.remove(f)
            except OSError:
                pass
        for dst, b in reversed(moved):
            try:
                os.replace(b, dst)
            except OSError:
                pass
        raise


def cmd_apply(root: str, new: str, wait: int, restart: bool) -> int:
    root, new = os.path.abspath(root), os.path.abspath(new)
    if wait:
        wait_for(wait)
    log = os.path.join(root, "user" if os.path.isdir(os.path.join(root, "user")) else "", "update.log")
    was, now = current_version(root), current_version(new)
    msg = f"{time.strftime('%Y-%m-%d %H:%M:%S')} {was or '?'} -> {now}: "
    try:
        for attempt in range(20):   # Windows: a file can stay locked a moment after its program ends
            try:
                apply(root, new)
                break
            except PermissionError:
                if attempt == 19:
                    raise
                time.sleep(0.5)
        msg += "done"
        ok = True
    except Exception as e:
        msg += f"failed, the old version kept: {e}"
        ok = False
    print(msg, flush=True)
    try:
        with open(log, "a", encoding="utf-8") as f:
            f.write(msg + "\n")
    except OSError:
        pass
    if restart:
        launcher = os.path.join(root, "bin", "hiraku.exe" if sys.platform == "win32" else "hiraku")
        args = [launcher, "--updated" if ok else "--update-failed", now if ok else msg]
        if sys.platform == "win32":
            subprocess.Popen(args, cwd=root, creationflags=0x00000008 | 0x00000200, close_fds=True)   # detached, own group
        else:
            subprocess.Popen(args, cwd=root, start_new_session=True, stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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
    except OSError as e:   # no network, GitHub not answering, a full disk
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
