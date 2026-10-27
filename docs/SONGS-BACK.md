# Songs back (Omnimix) for the 64-bit PC pop'n

The 64-bit PC pop'n keeps a place for every song pop'n music ever had, and leaves a few hundred of
them out. An **Omnimix pack** (the music data and song files players put together for popnhax and
ifs_layeredfs on the earlier PC pop'n games) brings them back: HIRAKU reads the pack as a mod and
fills the songs in before the game starts. Nothing of the game comes with HIRAKU, and nothing is
written into the game's files: the songs, their charts and pictures are the pack's, laid over the
game as it runs (PLAN.md section 34).

With thanks to Milo, whose work on the same game (popnMods) showed the way.

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

5. Characters the game removed (the cover songs' own: anime, TV themes, games): the game keeps a
   place for each, and an older PC pop'n of yours still describes them. `tools/oldcharas.py` reads
   that game's `popn22.dll` (pop'n 22 to the last 32-bit game) and writes a mod with them: their
   names and texts, and their hariai and name pictures from that game's packs, scaled up to this
   game's size (this game's packs haven't them; their own pictures it still has; it needs Pillow,
   `python3 -m pip install pillow`):
   ```bash
   python3 tools/oldcharas.py <older game>/modules/popn22.dll ~/.local/share/hiraku/mods/popn29/Characters
   ```
   Tick it with the pack in **Mods…** (in any order). Only that game's data goes into the folder,
   for these characters; `tools/songsback.py` with both folders says how many come back. A pack's
   own `charadb` (popnhax's `<chara id>` entries) works the same way.

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
- `overlay: /ifs_image_kc/kc_0001.ifs from /hiraku_mod0/...`: a banner taken from the pack;
- `songs: room for N songs and M charts`: the pack has songs past the game's table (below), and
  the game's tables were made bigger for them.

HIRAKU's own window log says `mods: Omnimix: N files on none of the game's media, left out`: that's
HIRAKU's general mod loader counting the pack's files the game hasn't; for this game they're used
all the same (the songs' files, banners and music data are new to it).

A song comes back when the game left it out, the pack describes it (genre, title, artist), its
Normal chart's file is there and it has a banner (the pack's, or one the game kept); its Easy,
Hyper and EX charts come with it when their files are there (a missing one leaves the song out),
the battle charts are optional. A song's tempos (the music select's BPM) are the pack's
`display_bpm`, or read from its Normal chart when the pack gives none (`N with their tempos read
from their charts` in the `brought back` line). Characters are found by name among the game's own and those the
mods describe again (`songs: N characters back` in `pc.log`: only in the places the game kept for
them, and only with their pictures, hariai and name picture there, the game faults without them);
a song whose characters the game hasn't shows its other one, or the game's first. The
earlier games' banners are half this game's size: they're stretched to their frames. A music data
file that says Shift-JIS but is UTF-8 is read as UTF-8.

Songs the game never had a place for (the console games' songs from 3000, custom songs, any id from
2374 on) come back the same way: HIRAKU makes the game's song and chart tables bigger, up to the
pack's last song, and points the game at them. They're in the music select's lists, play, and keep
their scores under their own numbers. A chart the pack adds is read in the old format (that of the
earlier games) unless it has long notes or the pack says `force_new_chart_format`, as popnhax has it.
They aren't drawn by the random choice, have no friends' scores, and the music select may not come
back to one of them as the card's last song (it opens near it). This works with the game's build
HIRAKU knows (2026041500); another build keeps them out and says so in `pc.log`.

## Not yet

- Characters no older PC game describes (one in a player's pack: umaru_23a), and the small icons of
  those that come back (pictures inside the game's `icon.ifs`: their place is blank). A pack's own
  characters' pictures (its `ha_merge_ifs`) are the older games' size: small in the windows.
- The characters the pack put in for songs whose own it hadn't (the console games', pop'n music
  Mickey Tunes', old anime): the originals from the older games' data.
- Pictures as PNGs in the `_ifs` folders (ifs_layeredfs converts them): only whole files are used.
- Categories added by other mods (By Medal) and the extra Random entries.
- The launcher doesn't check a pack; `tools/songsback.py` does.

Checked (2026-10-26) with a player's Omnimix pack: the tool and the game agree (398 songs back, 161 of
them past the game's table; 12 left out for missing chart files, 2 for missing texts, 1 without a
Normal chart), a restored song played through to its result from the launcher's mods folder, and
song 3000 (from the console games) listed under its genre, played to its result and its score sent
as song 3000. Characters (2026-10-27): 28 described by `tools/oldcharas.py` from an older game (32
with the pack's own `charadb`), song 650 shown and played with its own character, its hariai and
name, two stages, its score sent.
