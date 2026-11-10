#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/update.py download: a package is unpacked only when it matches the SHA-256 the release
gives for it; one the release gives none for, or another's, is refused and nothing of it is left.
A made-up release in a folder (HK_UPDATE_API a file: URL), no network.

    test_update.py      exit 0 when all hold
"""
import hashlib
import io
import json
import os
import subprocess
import sys
import tarfile
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ASSET = "hiraku-windows-x86_64.zip" if sys.platform == "win32" else "hiraku-linux-x86_64.tar.xz"
failed = 0


def check(ok: bool, what: str) -> None:
    global failed
    print(("ok    " if ok else "FAIL  ") + what, flush=True)
    failed += not ok


def package(path: str) -> str:
    """a package with the two files download looks for; its SHA-256"""
    files = {"hiraku/VERSION": b"9.9.9\n", "hiraku/bin/hiraku": b"#!/bin/sh\n"}
    if path.endswith(".zip"):
        with zipfile.ZipFile(path, "w") as z:
            for n, data in files.items():
                z.writestr(n, data)
    else:
        with tarfile.open(path, "w:xz") as t:
            for n, data in files.items():
                info = tarfile.TarInfo(n)
                info.size = len(data)
                t.addfile(info, io.BytesIO(data))
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def download(tmp: str, name: str, digest) -> tuple[int, str, str]:
    """update.py download against a release whose asset has that digest (None: no digest field)"""
    pkg = os.path.join(tmp, ASSET)
    asset = {"name": ASSET, "browser_download_url": "file://" + pkg.replace(os.sep, "/"), "size": os.path.getsize(pkg)}
    if digest is not None:
        asset["digest"] = digest
    api = os.path.join(tmp, name + ".json")
    with open(api, "w") as f:
        json.dump({"tag_name": "v9.9.9", "name": "HIRAKU v9.9.9", "html_url": "https://example.invalid/release", "assets": [asset]}, f)
    dest = os.path.join(tmp, name)
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "update.py"), "download", dest],
                       env={**os.environ, "HK_UPDATE_API": "file://" + api.replace(os.sep, "/")}, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr, dest


def nothing_left(dest: str) -> bool:
    return not os.path.exists(os.path.join(dest, "new")) and not os.path.exists(os.path.join(dest, ASSET))


with tempfile.TemporaryDirectory() as tmp:
    sha = package(os.path.join(tmp, ASSET))

    code, out, dest = download(tmp, "good", "sha256:" + sha)
    check(code == 0 and "ready v9.9.9" in out, "a package that matches its SHA-256: unpacked")
    check(os.path.isfile(os.path.join(dest, "new", "hiraku", "VERSION")), "... its files there")

    code, out, dest = download(tmp, "upper", "sha256:" + sha.upper())
    check(code == 0, "... the SHA-256 in capitals: the same")

    code, out, dest = download(tmp, "none", None)
    check(code != 0 and "SHA-256" in out, "a release that gives no SHA-256 for it: refused, and says why")
    check(nothing_left(dest), "... nothing downloaded or unpacked")

    code, out, dest = download(tmp, "empty", "")
    check(code != 0 and nothing_left(dest), "an empty one: refused")

    code, out, dest = download(tmp, "md5", "md5:" + "0" * 32)
    check(code != 0 and nothing_left(dest), "a digest of another kind: refused")

    code, out, dest = download(tmp, "wrong", "sha256:" + "0" * 64)
    check(code != 0 and "doesn't match" in out, "another package's SHA-256: refused")
    check(nothing_left(dest), "... nothing unpacked, the download removed")

print(f"{failed} failed" if failed else "all hold")
sys.exit(1 if failed else 0)
