#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The launcher's first songs: the MIDI files of a shared Google Drive folder, into the music folder.

    tools/musicfetch.py FOLDER_LINK DEST

FOLDER_LINK: the folder's link as Drive shows it (.../folders/ID), or its ID. Its MIDI files (.mid,
.midi, .kar, .rmi) go into DEST, each checked to be MIDI; a file DEST has already is left as it is.
No sign-in: the folder must be shared with anyone who has the link. Its own folders aren't looked in.

What it says, one a line: "found N" (the folder's MIDI files), then "got NAME" / "have NAME" /
"failed NAME: WHY" for each, then "done GOT HAVE FAILED". Exit 0 when every one is in DEST, 1 when
some aren't, 2 when the folder couldn't be read (or has none).

The launcher (launcher/music.cpp) runs it on its first launches, when its build names a folder
(local/music-source.txt, HK_MUSIC_SOURCE in CMakeLists.txt) or Settings has one (music/source).
"""
import html
import os
import re
import ssl
import sys
import urllib.request

MIDI = (".mid", ".midi", ".kar", ".rmi")
LIST = "https://drive.google.com/embeddedfolderview?id={}"
FILE = "https://drive.google.com/uc?export=download&id={}"


def tls() -> ssl.SSLContext:   # (as tools/update.py's)
    ctx = ssl.create_default_context()
    if not ctx.cert_store_stats().get("x509_ca"):
        for f in ("/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                  "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem", "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem"):
            if os.path.exists(f):
                ctx.load_verify_locations(f)
                break
    return ctx


def fetch(url: str, limit: int) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (HIRAKU launcher)"})
    with urllib.request.urlopen(req, timeout=30, context=tls()) as r:
        data = r.read(limit + 1)
    if len(data) > limit:
        raise OSError("more than %d MB" % (limit >> 20))
    return data


def folder_id(link: str) -> str:
    m = re.search(r"/folders/([\w-]+)", link) or re.search(r"[?&]id=([\w-]+)", link)
    if m:
        return m.group(1)
    if re.fullmatch(r"[\w-]{10,}", link):
        return link
    raise ValueError("not a Drive folder link: " + link)


def listing(fid: str) -> list:
    """the folder's (file id, name) pairs, MIDI files only"""
    page = fetch(LIST.format(fid), 8 << 20).decode("utf-8", "replace")
    out = []
    for m in re.finditer(r'<div class="flip-entry" id="entry-([\w-]+)".*?<div class="flip-entry-title">(.*?)</div>', page, re.S):
        name = html.unescape(m.group(2)).strip()
        if name.lower().endswith(MIDI):
            out.append((m.group(1), name))
    return out


def safe_name(name: str) -> str:
    """a file name every system takes: no folders, none of Windows' forbidden characters"""
    name = re.sub(r'[\\/:*?"<>|\x00-\x1f]', "_", name).strip(" .")
    return name or "song.mid"


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    link, dest = sys.argv[1], sys.argv[2]
    sys.stdout.reconfigure(encoding="utf-8")   # (the names, to the launcher, whatever Windows' code page)
    try:
        files = listing(folder_id(link))
    except (OSError, ValueError) as e:
        print("error the folder couldn't be read: %s" % e, flush=True)
        return 2
    print("found %d" % len(files), flush=True)
    if not files:
        return 2
    os.makedirs(dest, exist_ok=True)
    got = have = failed = 0
    for fid, name in files:
        name = safe_name(name)
        path = os.path.join(dest, name)
        if os.path.isfile(path) and os.path.getsize(path) > 0:
            have += 1
            print("have " + name, flush=True)
            continue
        try:
            data = fetch(FILE.format(fid), 16 << 20)
            if not (data.startswith(b"MThd") or (data.startswith(b"RIFF") and data[8:12] == b"RMID")):
                raise OSError("Drive gave something else than a MIDI file")
            with open(path + ".part", "wb") as f:
                f.write(data)
            os.replace(path + ".part", path)
            got += 1
            print("got " + name, flush=True)
        except OSError as e:
            failed += 1
            print("failed %s: %s" % (name, e), flush=True)
            try:
                os.remove(path + ".part")
            except OSError:
                pass
    print("done %d %d %d" % (got, have, failed), flush=True)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
