# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the HIRAKU login / logout animation in AniCraft through its MCP server.

"Hiraku" (開く) means "to open". Login: a vertical line grows, opens into a black box and
the logo is uncovered inside it; the box stays. Logout: the box closes over the logo into
one vertical line, which shrinks away. Outside the box everything is transparent.

Needs the AniCraft flatpak (ai.storyteller.anicraft) and ffmpeg (libvpx-vp9, libwebp_anim).
Run:  python3 build_anim.py            (writes hiraku_login_logout.anicraft + everything in out/)
      python3 build_anim.py --no-export
"""
import json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
PIECES = os.path.join(HERE, "pieces")
OUT = os.path.join(HERE, "out")
os.makedirs(OUT, exist_ok=True)

W, H, FPS = 1920, 1080, 60
CX, CY = W / 2, H / 2
# Where the 1500x380 logo sits on the stage: placed so its visible art (letters and emoji)
# is centred, which centres it inside the box too.
OX = CX - (122 + 1425) / 2
OY = CY - (52.4 + 327) / 2
LOGIN_FRAMES, LOGOUT_FRAMES = 105, 54
LOGIN_SPEED = 0.65  # login keyframes were laid out for 150 frames; this squeezes them
WHITE, BLACK = "#ffffff", "#000000"


class MCP:
    def __init__(self, app):
        self.p = subprocess.Popen(
            ["flatpak", "run", f"--command={app}-cli", f"ai.storyteller.{app}", "mcp"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        self.n = 0
        self._call("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                  "clientInfo": {"name": "hiraku-build", "version": "1"}})
        self._send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def _send(self, m):
        self.p.stdin.write(json.dumps(m) + "\n"); self.p.stdin.flush()

    def _call(self, method, params):
        self.n += 1
        self._send({"jsonrpc": "2.0", "id": self.n, "method": method, "params": params})
        while True:
            m = json.loads(self.p.stdout.readline())
            if m.get("id") == self.n:
                if "error" in m:
                    raise RuntimeError(m["error"])
                return m["result"]

    def tool(self, name, **args):
        r = self._call("tools/call", {"name": name, "arguments": args})
        texts = [c["text"] for c in r.get("content", []) if c["type"] == "text"]
        if r.get("isError"):
            raise RuntimeError(f"{name} {args}: {texts}")
        try:
            return json.loads(texts[0]) if texts else None
        except json.JSONDecodeError:
            return texts[0]

    def run(self, command, **params):
        r = self.tool("anicraft_run", command=command, params=params)
        return r.get("result", r) if isinstance(r, dict) else r

    def close(self):
        self.p.stdin.close(); self.p.wait(timeout=60)


layout = json.load(open(os.path.join(PIECES, "layout.json")))
centre = {k: (OX + v["x"] + v["w"] / 2, OY + v["y"] + v["h"] / 2) for k, v in layout.items()}
LETTERS = ["H", "I", "R", "A", "K", "U"]
# letters nearest the seam open first
by_dist = sorted(LETTERS, key=lambda k: abs(centre[k][0] - CX))
rank = {k: by_dist.index(k) // 2 for k in LETTERS}  # A,R → 0 · I,K → 1 · H,U → 2

# The box: its sides are the two vertical lines, joined by a top and a bottom bar.
BOX_DX = 750     # sides' distance from the centre (≈100 px clear of the emoji and H)
BOX_H = 432      # inner height (≈78 px clear above and below the letters)
LINE = 4         # line thickness
CLOSE_A, CLOSE_B = 8, 30   # logout: frames where the box starts and finishes closing
SHRINK_A, SHRINK_B = 32, 46  # logout: frames where the remaining line starts and finishes shrinking
GATHER = 0.8   # how far toward the seam the letters start (1 = in place)


def key(a, layer, prop, frame, value):
    a.run("motion.set_key", layer=layer, property=prop, frame=frame, value=value)


def kT(a, layer, prop, frame, value):
    """key() on the login's time scale."""
    key(a, layer, prop, round(frame * LOGIN_SPEED), value)


def eT(a, layer, frame, easing, prop=None):
    """ease() on the login's time scale."""
    ease(a, layer, round(frame * LOGIN_SPEED), easing, prop)


def ease(a, layer, frame, easing, prop=None):
    p = dict(layer=layer, frame=frame, easing=easing)
    if prop:
        p["property"] = prop
    a.run("motion.set_ease", **p)


def tween_layer(a, layer, frames):
    a.run("timeline.insert_frame", layer=layer, frame=frames - 1)
    a.run("timeline.create_motion_tween", layer=layer, frame=0)


def new_symbol_layer(a, name, import_path=None, rect=None, fill=None):
    """Make a layer holding one instance of a new symbol; returns (layer id, symbol id)."""
    if import_path:
        v = layout[name]
        r = a.run("file.import", path=import_path, x=OX + v["x"], y=OY + v["y"])
        layer = r["layer"]
    else:
        a.run("layer.new", name=name)
        layer = a.tool("anicraft_document")["layer"]
        a.run("draw.rect", **rect, fill=fill or WHITE, stroke=None, object=True)
    a.run("layer.rename", name=name, layer=layer)
    a.run("layer.select", layer=layer, select=True)
    s = a.run("modify.convert_to_symbol", name=name)
    return layer, s["symbol"]


def placed_layer(a, name, symbol, x, y):
    a.run("layer.new", name=name)
    layer = a.tool("anicraft_document")["layer"]
    a.run("library.place", symbol=symbol, x=x, y=y)
    return layer


def drop_default_layers(a, keep):
    doc = a.tool("anicraft_document")
    for l in doc["layers"]:
        if l["id"] not in keep:
            a.run("layer.delete", layer=l["id"])


def build():
    a = MCP("anicraft")
    a.run("file.new", width=W, height=H, fps=FPS)
    a.run("doc.set_stage", background="#000000")
    a.run("scene.rename", name="Login")
    first_layers = {l["id"] for l in a.tool("anicraft_document")["layers"]}

    # ---------------- LOGIN: a line appears, opens into a box, the logo is uncovered ----------------
    N = LOGIN_FRAMES
    sym, layers = {}, {}
    side_rect = dict(x=CX - LINE / 2, y=CY - BOX_H / 2 - LINE / 2, w=LINE, h=BOX_H + LINE)
    bar_rect = dict(x=CX - BOX_DX - LINE / 2, y=CY - BOX_H / 2 - LINE / 2, w=2 * BOX_DX + LINE, h=LINE)
    mask_rect = dict(x=CX - BOX_DX, y=CY - BOX_H / 2, w=2 * BOX_DX, h=BOX_H)
    # boxFill: the box's black inside, at the bottom; like the logo it only shows through the
    # mask, so it opens and closes with the box and nothing outside the box is ever drawn
    MASKED = ["emoji", "U", "K", "A", "R", "I", "H", "boxFill"]  # top to bottom

    layers["boxFill"], sym["boxFill"] = new_symbol_layer(a, "boxFill", rect=mask_rect, fill=BLACK)
    for name in LETTERS + ["emoji"]:
        layers[name], sym[name] = new_symbol_layer(a, name, import_path=os.path.join(PIECES, f"{name}.svg"))
    layers["boxMask"], sym["boxMask"] = new_symbol_layer(a, "boxMask", rect=mask_rect)
    layers["sideL"], sym["side"] = new_symbol_layer(a, "sideL", rect=side_rect)
    layers["sideR"] = placed_layer(a, "sideR", sym["side"], CX, CY)
    layers["barTop"], sym["bar"] = new_symbol_layer(a, "barTop", rect=bar_rect)
    layers["barBottom"] = placed_layer(a, "barBottom", sym["bar"], CX, CY + BOX_H / 2)
    drop_default_layers(a, set(layers.values()))

    # a line grows from a point (0–18), then its two halves run out to the box's sides (18–48)
    # and stay there
    for side, sign in (("sideL", -1), ("sideR", 1)):
        L = layers[side]
        tween_layer(a, L, N)
        kT(a, L, "scale_y", 0, 1); kT(a, L, "scale_y", 18, 100)
        kT(a, L, "alpha", 0, 0); kT(a, L, "alpha", 8, 100)
        kT(a, L, "x", 0, CX); kT(a, L, "x", 18, CX); kT(a, L, "x", 48, CX + sign * BOX_DX)
        eT(a, L, 9, "CubicOut", "scale_y")
        eT(a, L, 30, "CubicOut", "x")

    # the top and bottom bars stretch with the sides, closing the box
    for bar in ("barTop", "barBottom"):
        L = layers[bar]
        tween_layer(a, L, N)
        kT(a, L, "alpha", 0, 0); kT(a, L, "alpha", 17, 0); kT(a, L, "alpha", 18, 100)
        kT(a, L, "scale_x", 0, 0.3); kT(a, L, "scale_x", 18, 0.3); kT(a, L, "scale_x", 48, 100)
        eT(a, L, 30, "CubicOut", "scale_x")

    # the inside of the box is a mask that widens with the sides, uncovering the logo
    L = layers["boxMask"]
    tween_layer(a, L, N)
    kT(a, L, "scale_x", 0, 0.1); kT(a, L, "scale_x", 18, 0.1); kT(a, L, "scale_x", 48, 100)
    eT(a, L, 30, "CubicOut", "scale_x")

    # letters: start gathered toward the centre, small and transparent, then open out
    for name in LETTERS:
        L, (cx, cy) = layers[name], centre[name]
        s = 20 + rank[name] * 3
        tween_layer(a, L, N)
        kT(a, L, "x", 0, CX + (cx - CX) * GATHER); kT(a, L, "x", s, CX + (cx - CX) * GATHER)
        kT(a, L, "x", s + 34, cx)
        kT(a, L, "alpha", 0, 0); kT(a, L, "alpha", s, 0); kT(a, L, "alpha", s + 16, 100)
        for p in ("scale_x", "scale_y"):
            kT(a, L, p, 0, 82); kT(a, L, p, s, 82); kT(a, L, p, s + 26, 103); kT(a, L, p, s + 40, 100)
        eT(a, L, s + 10, "CubicOut", "x")
        eT(a, L, s + 8, "QuadOut", "alpha")
        for p in ("scale_x", "scale_y"):
            eT(a, L, s + 10, "CubicOut", p); eT(a, L, s + 30, "QuadInOut", p)

    # emoji: spins and pops in once the letters have landed
    L, (cx, cy) = layers["emoji"], centre["emoji"]
    tween_layer(a, L, N)
    for p in ("scale_x", "scale_y"):
        kT(a, L, p, 0, 0); kT(a, L, p, 62, 0); kT(a, L, p, 78, 125); kT(a, L, p, 88, 100)
        eT(a, L, 70, "CubicOut", p); eT(a, L, 82, "QuadInOut", p)
    kT(a, L, "rotation", 62, -200); kT(a, L, "rotation", 82, 0)
    eT(a, L, 70, "CubicOut", "rotation")

    a.run("timeline.insert_frame", layer=layers["boxFill"], frame=N - 1)
    a.run("layer.set_type", type="mask", layer=layers["boxMask"])
    for name in MASKED:
        a.run("layer.set_type", type="masked", layer=layers[name])
    a.run("timeline.set_label", label="login", layer=layers["sideL"], frame=0)

    # ---------------- LOGOUT: the box closes in over the logo, then its line shrinks away ----------------
    # Starts exactly where the login ends. The sides and bars close in together while the
    # mask inside narrows with them, hiding what they pass over; the line they meet in then
    # shrinks to nothing (the reverse of the login's first beat).
    a.run("scene.new", name="Logout")
    N = LOGOUT_FRAMES
    default = {l["id"] for l in a.tool("anicraft_document")["layers"]}
    lo = {}
    lo["boxFill"] = placed_layer(a, "boxFill", sym["boxFill"], CX, CY)
    for name in LETTERS + ["emoji"]:
        lo[name] = placed_layer(a, name, sym[name], *centre[name])
    lo["boxMask"] = placed_layer(a, "boxMask", sym["boxMask"], CX, CY)
    lo["sideL"] = placed_layer(a, "sideL", sym["side"], CX - BOX_DX, CY)
    lo["sideR"] = placed_layer(a, "sideR", sym["side"], CX + BOX_DX, CY)
    lo["barTop"] = placed_layer(a, "barTop", sym["bar"], CX, CY - BOX_H / 2)
    lo["barBottom"] = placed_layer(a, "barBottom", sym["bar"], CX, CY + BOX_H / 2)
    for l in default:
        a.run("layer.delete", layer=l)
    for name in LETTERS + ["boxFill"]:
        a.run("timeline.insert_frame", layer=lo[name], frame=N - 1)

    # emoji spins out first
    L = lo["emoji"]
    tween_layer(a, L, N)
    for p in ("scale_x", "scale_y"):
        key(a, L, p, 2, 100); key(a, L, p, 5, 115); key(a, L, p, 14, 0)
        ease(a, L, 3, "QuadOut", p); ease(a, L, 10, "CubicIn", p)
    key(a, L, "rotation", 5, 0); key(a, L, "rotation", 14, 200)
    ease(a, L, 10, "CubicIn", "rotation")

    # the sides close in to the centre, then the line they make shrinks to a point
    mid = (CLOSE_A + CLOSE_B) // 2
    for side, sign in (("sideL", -1), ("sideR", 1)):
        L = lo[side]
        tween_layer(a, L, N)
        key(a, L, "x", 0, CX + sign * BOX_DX); key(a, L, "x", CLOSE_A, CX + sign * BOX_DX)
        key(a, L, "x", CLOSE_B, CX)
        ease(a, L, mid, "CubicInOut", "x")
        key(a, L, "scale_y", 0, 100); key(a, L, "scale_y", SHRINK_A, 100); key(a, L, "scale_y", SHRINK_B, 1)
        key(a, L, "alpha", 0, 100); key(a, L, "alpha", SHRINK_B - 4, 100); key(a, L, "alpha", SHRINK_B, 0)
        ease(a, L, (SHRINK_A + SHRINK_B) // 2, "CubicIn", "scale_y")

    # the bars shrink with them
    for bar in ("barTop", "barBottom"):
        L = lo[bar]
        tween_layer(a, L, N)
        key(a, L, "scale_x", 0, 100); key(a, L, "scale_x", CLOSE_A, 100); key(a, L, "scale_x", CLOSE_B, 0.3)
        ease(a, L, mid, "CubicInOut", "scale_x")
        # gone once closed, so no dots stay at the line's ends while it shrinks
        key(a, L, "alpha", 0, 100); key(a, L, "alpha", CLOSE_B, 100); key(a, L, "alpha", CLOSE_B + 1, 0)

    # the mask narrows with exactly the same timing and easing
    L = lo["boxMask"]
    tween_layer(a, L, N)
    key(a, L, "scale_x", 0, 100); key(a, L, "scale_x", CLOSE_A, 100); key(a, L, "scale_x", CLOSE_B, 0.1)
    ease(a, L, mid, "CubicInOut", "scale_x")
    # once closed, move the mask off the stage so no 1 px sliver of the logo stays at the seam
    key(a, L, "y", 0, CY); key(a, L, "y", CLOSE_B, CY); key(a, L, "y", CLOSE_B + 1, -2 * H)
    a.run("layer.set_type", type="mask", layer=L)
    for name in MASKED:
        a.run("layer.set_type", type="masked", layer=lo[name])
    a.run("timeline.set_label", label="logout", layer=lo["sideL"], frame=0)
    a.run("scene.select", scene=0)
    return a


def encode_alpha(png_dir, stem):
    """Transparent WebM (VP9 + alpha) and animated WebP from a transparent PNG sequence."""
    frames = sorted(f for f in os.listdir(png_dir) if f.endswith(".png"))
    first = frames[0]
    pattern = os.path.join(png_dir, first[: first.rindex("_") + 1] + "%05d.png")
    start = int(first[first.rindex("_") + 1:-4])
    base = ["ffmpeg", "-v", "error", "-y", "-framerate", str(FPS), "-start_number", str(start), "-i", pattern]
    subprocess.run(base + ["-c:v", "libvpx-vp9", "-pix_fmt", "yuva420p", "-b:v", "0", "-crf", "24",
                           "-auto-alt-ref", "0", f"{OUT}/{stem}.webm"], check=True)
    subprocess.run(base + ["-c:v", "libwebp_anim", "-lossless", "0", "-quality", "90", "-loop", "1",
                           f"{OUT}/{stem}.webp"], check=True)
    print("encoded", f"{OUT}/{stem}.webm", f"{OUT}/{stem}.webp")


if __name__ == "__main__":
    a = build()
    doc_path = os.path.join(HERE, "hiraku_login_logout.anicraft")
    print(a.run("file.save_as", path=doc_path, confirm=True))
    if "--no-export" not in sys.argv:
        import shutil
        L0, L1 = 0, LOGIN_FRAMES - 1
        O0, O1 = LOGIN_FRAMES, LOGIN_FRAMES + LOGOUT_FRAMES - 1
        for d in ("hiraku_login_png", "hiraku_logout_png"):
            shutil.rmtree(f"{OUT}/{d}", ignore_errors=True)
        for f in ("hiraku_login.mp4", "hiraku_logout.mp4"):  # replaced by *_preview_on_black.mp4
            if os.path.exists(f"{OUT}/{f}"):
                os.remove(f"{OUT}/{f}")
        jobs = [
            # opaque previews on the black stage, for looking at only
            ("file.export_video", dict(path=f"{OUT}/hiraku_login_preview_on_black.mp4", **{"from": L0, "to": L1})),
            ("file.export_video", dict(path=f"{OUT}/hiraku_logout_preview_on_black.mp4", **{"from": O0, "to": O1})),
            ("file.export_gif", dict(path=f"{OUT}/hiraku_login_logout_preview.gif", scale=0.4)),
        ]
        transparent_jobs = [
            ("file.export_lottie", dict(path=f"{OUT}/hiraku_login.json", **{"from": L0, "to": L1})),
            ("file.export_lottie", dict(path=f"{OUT}/hiraku_logout.json", **{"from": O0, "to": O1})),
        ]
        for cmd, p in jobs:
            print(cmd, a.run(cmd, confirm=True, **p))
        # Everything below is transparent: a stage colour with zero alpha makes AniCraft leave
        # the background out, so only the black box, its lines and the logo are drawn.
        a.run("doc.set_stage", background="#00000000")
        for cmd, p in transparent_jobs:
            print(cmd, a.run(cmd, confirm=True, **p))
        for scene, (stem, n) in enumerate((("hiraku_login", LOGIN_FRAMES), ("hiraku_logout", LOGOUT_FRAMES))):
            a.run("scene.select", scene=scene)
            os.makedirs(f"{OUT}/{stem}_png")
            for f in range(n):
                a.run("file.export_png", path=f"{OUT}/{stem}_png/frame_{f + 1:05d}.png", frame=f, confirm=True)
            print("png frames", f"{OUT}/{stem}_png", n)
        a.run("scene.select", scene=0)
        encode_alpha(f"{OUT}/hiraku_login_png", "hiraku_login")
        encode_alpha(f"{OUT}/hiraku_logout_png", "hiraku_logout")
    a.close()
