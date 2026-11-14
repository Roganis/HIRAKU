# HIRAKU's logo and its animation

The game menu's logo (`runtime/menu/logo.h`): it opens on a black screen, goes up into the menu's
top corner as the black fades away, and closes on black again before a game starts. "Hiraku" (開く) is "to open".

| File | What it is |
|---|---|
| `logo.svg` | the logo traced to vectors (VectorCraft's Image Trace, black and white, from `launcher/res/logo.png`), with its black ground |
| `logo_transparent.svg` | the same, its white outlines alone |
| `pieces/` | the logo cut into one SVG a letter and the emoji, and where each sits (`layout.json`) |
| `hiraku_login_logout.anicraft` | the AniCraft document: scenes "Login" (opening, 105 frames) and "Logout" (closing, 54) |
| `build_anim.py` | builds that document in AniCraft through its MCP server and exports the frames |

At 1920x1080 and 60 frames a second. Opening: a thin line grows in the middle (frames 0-12), opens
into the box, its sides moving out and the black and the letters uncovered between them (12-31),
the letters spreading out and fading in; the emoji spins and pops in (40-57); then it stands still.
Closing starts on that picture: the emoji spins away, the box shuts over the name from both sides,
and the line left shrinks to nothing. Nothing is drawn outside the box (1504x436 at 208,322).

## Changing it

1. Edit the constants at the top of `build_anim.py` (`LOGIN_SPEED`, `BOX_DX`, `BOX_H`, `LINE`,
   `CLOSE_A/B`, `SHRINK_A/B`) and run it: `python3 build_anim.py`. It needs the AniCraft flatpak
   (`ai.storyteller.anicraft`) and FFmpeg with libvpx-vp9 and libwebp, and writes `out/` here
   (git-ignored): PNG frames, WebM and WebP with alpha, Lottie and previews. The flatpak reads and
   writes only some folders (the pictures, documents and videos folders): copy this folder there.
2. Bake the frames for the menu:
   `tools/logoanim.py out/hiraku_login_png out/hiraku_logout_png` writes `runtime/menu/logo.anim`
   (the box at a third of its size, 501x145, grey and opacity, each picture from the one before,
   zlib; about 370 KB), which the build carries inside `hiraku-menu` (`cmake/embed.cmake`).
3. Build, and `ctest -R game_menu` (it checks the frame counts, that closing starts on opening's
   last picture and ends on nothing).
