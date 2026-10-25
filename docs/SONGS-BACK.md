# Songs back (Omnimix) for the 64-bit PC pop'n

The 64-bit PC pop'n keeps a place for every song pop'n music ever had, and leaves a few hundred of
them out. An **Omnimix pack** (the music data and song files players put together for popnhax and
ifs_layeredfs on the earlier PC pop'n games) brings them back: HIRAKU reads the pack as a mod and
fills the songs in before the game starts. Nothing of the game comes with HIRAKU, and nothing is
written into the game's files: the songs, their charts and pictures are the pack's, laid over the
game as it runs (PLAN.md section 34).

With thanks to a friend of HIRAKU's author, whose work on the same game showed the way.

## What you need

- The 64-bit PC pop'n installed in HIRAKU and running (unlocked in the launcher by the Konami code).
- An Omnimix pack: a folder from an earlier PC pop'n's `data_mods`, as popnhax's Omnimix ships it,
  with at least:
  - its music data: `omnimix_musicdb_*.xml` (any `.xml` with `<music id="...">` entries: their
    titles, characters, `display_bpm` and `<chart idx="...">` files),
  - the songs' files: `sd/<folder>/<file>.ifs` (`sd/omni/...`, `sd/popn18/...`),
  - their banners and backgrounds: `tex/system/kc_diff_ifs/kc_NNNN.ifs`,
    `tex/system/bg_diff_ifs/bg_NNNN.ifs` (ifs_layeredfs's folders named after the game's packs).
  A pack's other folders (pictures for other games' screens, translations) are left alone.

## Install

1. Find HIRAKU's mods folder for the game: `<saves>/mods/popn29/`, where `<saves>` is
   `~/.local/share/hiraku` on Linux, `%LOCALAPPDATA%\hiraku` on Windows, or `user/data` in a
   portable package. Create the folders if they aren't there.
2. Put the pack's folder in it, as it is: `<saves>/mods/popn29/Omnimix/` with `sd/`, `tex/` and the
   `.xml` files directly inside. A link to a folder elsewhere works too (a pack is several GB):
   ```bash
   ln -s "/path/to/data_mods/Omnimix" ~/.local/share/hiraku/mods/popn29/Omnimix
   ```
   More packs (`Omnimix2`, `Omnimix3`...) go in beside it, each its own folder.
3. Optionally, give it a name and a line for the launcher: a `mod.toml` in the folder,
   ```toml
   name = "Omnimix"
   game = "popn29"
   description = "Songs back from the earlier games"
   ```
4. Check it before playing (no game started): `tools/songsback.py` reads the game and the pack and
   says which songs will come back and which won't, and why:
   ```bash
   python3 tools/songsback.py ~/.local/share/hiraku/mods/popn29/Omnimix
   ```
   (`--game DIR` if HIRAKU can't find the game's folder; several mod folders in order.)

## Run

1. In the launcher, right-click the game, **Mods…**, tick the pack (and the others, in order: the
   first one wins where two describe the same song), OK.
2. Play. The songs are in the music select with the game's own, under their titles, versions and
   categories; their scores are kept by HIRAKU's e-AMUSEMENT server like any other song's.

From the command line: `hiraku-pc --game popn29 --mod <saves>/mods/popn29/Omnimix`.

With a mod on, the game keeps its saves apart from the clean game's (in `popn29+Omnimix`, started
from a copy), as for every game; the e-AMUSEMENT cards and players are the same.

## What happens

The save folder's `pc.log` says it, at the start:
- `songs: N brought back (M charts added; K described without their files)`: the songs back, and
  how many the pack describes without their chart files (the first 20 by name: `left out: its
  charts' files aren't there`);
- `overlay: /ifs_image_kc/kc_0001.ifs from /hiraku_mod0/...`: a banner taken from the pack.

HIRAKU's own window log says `mods: Omnimix: N files on none of the game's media, left out`: that's
HIRAKU's general mod loader counting the pack's files the game hasn't; for this game they're used
all the same (the songs' files, banners and music data are new to it).

A song comes back when the game left it out, the pack describes it (genre, title, artist), its
Normal chart's file is there and it has a banner (the pack's, or one the game kept); its Easy,
Hyper and EX charts come with it when their files are there (a missing one leaves the song out),
the battle charts are optional. Characters are found by name among the game's own; a song whose
characters the game hasn't (the cover songs' own) shows its other one, or the game's first. The
earlier games' banners are half this game's size: they're stretched to their frames. A music data
file that says Shift-JIS but is UTF-8 is read as UTF-8.

## Not yet

- Songs the game never had a place for (custom songs, ids from 2374 on): left out.
- Characters the game hasn't (the pack's `charadb` only changes the game's own).
- Pictures as PNGs in the `_ifs` folders (ifs_layeredfs converts them): only whole files are used.
- Categories added by other mods (By Medal) and the extra Random entries.
- The launcher doesn't check a pack; `tools/songsback.py` does.

Checked (2026-10-26) with a player's Omnimix pack: the tool and the game agree (226 songs back, 12
left out for missing chart files, 2 for missing texts, 162 custom songs past the table), and a
restored song played through to its result from the launcher's mods folder.
