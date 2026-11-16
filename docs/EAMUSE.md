# e-AMUSEMENT and NET TAISEN

e-AMUSEMENT went away with the arcades: pop'n music 9's servers, the first, closed in August
2003. HIRAKU plays the cabinet's card reader, its 10-key, its network and the server, so the
games' e-AMUSEMENT works again: cards and PINs, players, scores, rankings, and pop'n music's NET
TAISEN battles. This is the player's guide; the protocol, each game's calls and the records are
in [EAMUSE3.md](EAMUSE3.md).

## What it gives, and which games

e-AMUSEMENT is off in every game until you turn it on in its test mode, and the games play the
same without it. With it on, the server runs inside HIRAKU: nothing to set up, and nothing goes
over the network. To share players between PCs, run `hiraku-eamuse` (below).

| Game | Card | What is kept |
|---|---|---|
| pop'n music 9 | magnetic | name, character, options; the EXPERT course rankings; KONAMI ID |
| pop'n music 10, 11 | magnetic | the player; the EXPERT course rankings; the shop boards (10 HOSHI NI NEGAIO, 11 POPN TOURIST); 11's mileage |
| pop'n music 12, 13 | magnetic | the player's record and each song's best score; the rankings and shop boards |
| pop'n music 14 | IC (e-AMUSEMENT PASS) | the player's record and each song's best score; the course rankings |
| pop'n music 15–18 (PC) | the slot reader | options, character, last song, clear marks, best scores, most played songs, and each game's own (15's ADVENTURE map, 16's PARTY guests, 17's characters and tickets, 18's story map and licences) |
| DrumMania 10th Mix, GuitarFreaks 11th Mix (System 573, with the network unit) | magnetic | the best percentage, skill, score and combo per song and difficulty; the skill point; HIT CHART |
| DrumMania V (Konami Python 2) | magnetic | each song's best score, combo and percentage, and the plays |
| beatmania IIDX 9th style, 10th style, RED, HAPPY SKY (PC) | magnetic | each chart's best EX score, DJ level and clear, shown on music select |
| beatmania IIDX DistorteD, GOLD, DJ TROOPERS, EMPRESS (PC) | IC (e-AMUSEMENT PASS) | as above, and the player's record |

IIDX 9th style also has the week's song, the most played songs (the ALL JAPAN RANKING courses)
and the EXPERT courses' internet ranking. NET TAISEN is in pop'n music 12–18 (below).

Details: [EAMUSE3.md](EAMUSE3.md#later-games-popn-music-1014),
[PLAY-NOTES.md](PLAY-NOTES.md#bemani-pc-popn-music-1518-beatmania-iidx-9th-style-to-empress)

## Turning it on

Each game's own test mode turns it on. Save, then restart the game.

- **pop'n music 9:** test mode > e-AMUSEMENT OPTIONS > e-AMUSEMENT **ON**, SAVE AND EXIT.
- **pop'n music 10–14:** test mode > NETWORK OPTIONS > e-AMUSEMENT OPTIONS. In 13 and 14 the
  menus' cursors start on EXIT.
- **pop'n music 15–18:** test mode > NETWORK OPTIONS. The title then says NETWORK OK.
- **DrumMania 10th Mix and GuitarFreaks 11th Mix:** add the network unit's CD (`d40jca02` or
  `d39jba02`) with the game. **Game > Install the e-AMUSEMENT network unit**, then Play (hold Tab;
  about 20 minutes of game time for DrumMania). At INSTALLATION COMPLETED, stop it and answer Yes.
  Then test mode > NETWORK OPTIONS > e-AMUSEMENT OPTIONS: hold Start and press Left/Right, then
  SAVE AND EXIT. NETWORK CHECK says CONNECTED. In GuitarFreaks' test mode, P2 R/B changes a value.
- **DrumMania V:** test mode's NETWORK OPTIONS (hold Start and press Left or Right). The game saves
  it on its hard disk and asks for a reboot.
- **beatmania IIDX:** turn it on in test mode, then restart. 10th style: e-AMUSEMENT OPTIONS >
  e-AMUSEMENT SETTINGS; RED: NETWORK OPTIONS > e-AMUSEMENT SETTINGS. A new GOLD, DJ TROOPERS or
  EMPRESS cabinet comes set up and starts with NETWORK OK.

Details: [PLAY-NOTES.md](PLAY-NOTES.md#konami-python-1-popn-music-914),
[DrumMania](PLAY-NOTES.md#system-573-drummania-10th-mix-further-than-mame),
[DrumMania V](EAMUSE3.md#guitarfreaks-and-drummania-v-konami-python-2)

## Cards

**In the game:** **Insert** puts the card in the slot, or takes it back when the reader has given
it out. The numeric keypad is the 10-key (Backspace is Clear). After Start, put a card in, enter
a name and a 4-digit PIN twice. Next time the card and the PIN bring back your player.

- **Magnetic cards** (pop'n 9–13, the System 573 and Python 2 games, IIDX 9th style to HAPPY
  SKY) are files in the save folder: on pop'n and IIDX `card.bin`, or `cards/NUMBER.bin` for a
  card with a number. On IIDX 10th style to HAPPY SKY a returning magnetic card asks no PIN.
- **The e-AMUSEMENT PASS** (pop'n 14, IIDX DistorteD to EMPRESS) is an IC card: only a number,
  `E004` and 12 hex digits, as in spice2x. The PIN is checked by the server. For pop'n 14 HIRAKU
  makes a number if none is set. On pop'n 14's reader, Enter is the blank key and `.` the 00 key.
- **Two players** (IIDX, GuitarFreaks): the launcher's **Controls** have Card and Keypad for
  player 1 (Insert and the keypad) and P2 Card and P2 Keypad for player 2 (Delete, and keys you
  bind yourself).
- **IIDX registration:** a new card is registered in the game (DJ name, PIN); from 10th style on
  it gets its IIDX ID. An e-AMUSEMENT PASS asks its PIN twice, then NAME ENTRY. EMPRESS asks a
  new pass to accept terms first (turntable left to はい, then a key).

**The launcher's settings** (Options > Settings > e-AMUSEMENT) have spice2x's names and meanings,
so a spice2x setup carries over:

| Setting | Option | What it does |
|---|---|---|
| Player 1 Card, Player 2 Card | `-card0`, `-card1` | the card's number, 16 hex digits (`E004...`). Empty: `card0.txt`, else the save folder's card, numbered by the server |
| Pick (next to a card) | | the cards HIRAKU already has, or a new number |
| Auto Card Insert | `-autocard off\|p1\|p2\|both` | the card goes in by itself whenever the reader waits for one |
| Type the card's PIN | `-autopin` | types a known card's PIN on the 10-key when the game asks (pop'n 14–18; HIRAKU's own server only) |
| Card readers | `--reader`, `--scard`, `--cardio` | a real e-AMUSEMENT reader and its 10-key: Konami's on a serial adapter, NFC readers, cardio readers |

pop'n music has one reader, player 1's. A card the game holds has to be taken back (Insert)
before another goes in.

Details: [EAMUSE3.md](EAMUSE3.md#running-it), [readers](PLAY-NOTES.md#card-readers)

## What the server keeps

HIRAKU's server keeps its data in the save folder's `eamuse/`, as plain text files. The pop'n
games, the PC ones too, share one `eamuse/` beside their save folders (`user/data/eamuse/` in a
release package). A game's own `eamuse/` from an older HIRAKU is merged in the first time, and
kept renamed as `eamuse.moved`. A game started with `--nvram` elsewhere keeps its own folder.

- **Players aren't lost.** A file the server can't read as a player is kept beside it as
  `NAME.unread-DATE`, and the server says so. Every file is written whole or not at all, so a
  full disk or a kill keeps the file from before. Servers that share a folder take turns.
- **Back it up** with your saves: the folder is your players.
- **Events and rankings:** with e-AMUSEMENT on, the attract mode shows the EXPERT course
  rankings and the shop boards from what the server has, by the event phase (right-click the
  game, then **Unlocks**). pop'n 11's mileage tickets need HIRAKU's own server; Unlocks can give every card
  nine tickets for each mileage song.
- **One event at a time:** pop'n 13 and 14 run NET TAISEN or the INTERNET RANKING, never both.
  HIRAKU gives NET TAISEN. For the ranking, put `1` in the file `net_event-E13` (pop'n 13) or
  `net_event-F14` (pop'n 14) in the server's data folder. pop'n 15: `ir_ranking-G15` (2, the
  default, the ADVENTURE map; 1 the INTERNET RANKING) and `net_event-G15` (1, the default, NET
  TAISEN; 0 none).

Details: [EAMUSE3.md](EAMUSE3.md#the-players-files)

## One card for all (pop'n music 9–18)

Each pop'n carries a player over from the one before. In HIRAKU one card number follows the
player from pop'n 9 to 18.

1. Set **Settings > e-AMUSEMENT > Player 1 Card** once (or `-card0`). pop'n 14 needs one anyway.
2. The first time that card goes into a newer game, it offers to carry your player over from the
   game before. Answer yes and type the old PIN.
3. When the game asks for an unused card of its own, press Insert again.
4. pop'n 14 asks for pop'n 13's transfer number (POP'N ID): type any number, then pop'n 13's PIN.
   pop'n 15 offers a pop'n 14 player of the same card, and 16, 17 and 18 the game before's.

- **Clear marks** then follow the card: a song cleared in any game shows its best mark in all of
  them (pop'n 9, which has no medals, shows it as cleared).
- **Best scores** are shared between pop'n 12–18, for the songs both games have.
- **Each game's own:** options and the name changed in one game stay there. pop'n 14 takes pop'n
  13's character and options; the others start from the game's defaults. ee'MALL tickets don't
  carry over (the games say so).

Details: [EAMUSE3.md](EAMUSE3.md#one-card-for-all-popn-music-918)

## Sharing between PCs: hiraku-eamuse

`hiraku-eamuse` is HIRAKU's server as a program of its own. Several cabinets on one share cards,
players and rankings, and meet in NET TAISEN.

1. On the server PC: `hiraku-eamuse --port 8080 --data DIR`.
2. On each cabinet: **Settings > e-AMUSEMENT > EA Service URL**, `that-pc:8080` (or
   `-url that-pc:8080`). Empty is the server inside HIRAKU.
3. Turn e-AMUSEMENT on in each game's test mode.

**Smart EA** (`-smartea`) falls back to the server inside HIRAKU when that one doesn't take a
connection at start. Both cabinets of a GuitarFreaks × DrumMania session can share one
`hiraku-eamuse`.

**Whom it takes.** e-AMUSEMENT has no log-in: whoever can reach the port can read and change the
players. On a network you trust that may be fine; for anything reached from outside, give it a
key. Each of these can be used alone, and they add up:

- **A key:** `hiraku-eamuse --data DIR --new-key` makes one, writes it to `key.txt` in the data
  folder and prints it. Give every cabinet the same key in **Settings > e-AMUSEMENT > Server key**
  (or `-eakey KEY`). Then nothing goes over the network in clear, and who hasn't the key gets no
  answer. A key on both sides or on neither. To change it, remove the file, make a new one and
  give it to every cabinet. `--key-file FILE` keeps it elsewhere. The PC games work with a key too.
- **Addresses:** `--allow 192.168.1.0/24,10.0.0.5`. Other addresses are closed unanswered. Nothing
  to set on the cabinets: the one for a home network or a party.
- **Cabinets:** `hiraku-eamuse --data DIR --new-cabinet "NAME"` adds a PCBID to `cabinets.txt` and
  prints it. Put it in that cabinet's **Settings > e-AMUSEMENT > PCBID** (`-p`). Once the file is
  there, only those cabinets are answered. Without `-p` a game sends its dump's PCBID, which every
  copy of that game has. The relay carries no PCBID, so NET TAISEN needs `--allow` or a key.

The server prints what it refused, once a minute at most; for a cabinet not on the list, the
PCBID to add. Only the key hides the traffic; a VPN under the server does too.

**Ports:**

| Port | For |
|---|---|
| the one given to `--port` (8080 above) | the games' calls |
| TCP 5740 | pop'n music 12–18's NET TAISEN relay. The lobbies (5741 for pop'n 15, 5742–5744 for 16–18) go through it. `--no-relay` turns it off |

The cabinets connect out: only the server's ports need to be reachable.

**Other options:** `--bind ADDR` (default 0.0.0.0); `--clock "2003-06-01 12:00:00"`, the time it
gives, running from there; `--tz 9`, the cabinets' time zone in hours from UTC (default 9, Japan);
`--quiet`, don't print the calls. A file `message` in the data folder (one line) scrolls across
pop'n 9's title screen on every cabinet from its next start. A number in `limit-cards` or
`limit-files` changes how many cards (65536) and files (200000) it keeps.

**What the folder holds:** `hiraku-eamuse --data DIR --players` lists every card, its ref id and
each game's player on it with its name; `--scores CARD [GAME]` the card's scores (game, song,
chart, score, clear, plays; -1 where the game keeps none). `--settings [GAME]` lists every setting
above and the games' others (value, default, range, what it does); `--set GAME NAME VALUE` changes
one, checked against its range (GAME `server` for the message and the limits; VALUE `""` puts the
default back). Each prints tab-separated lines and ends; no PIN or password is printed.

**Other servers:** any server that implements [EAMUSE3.md](EAMUSE3.md) works, through the same
EA Service URL. For pop'n 9 on bemaniutils there is a patch for server owners' own installs,
`tools/eamuse/bemaniutils/popn9-python.patch`. `tools/eamuse/check.py URL` checks a server.

Details: [EAMUSE3.md](EAMUSE3.md#running-it),
[PLAY-NOTES.md](PLAY-NOTES.md#system-573-drummania-10th-mix-further-than-mame)

## NET TAISEN

Battles between cabinets, three players to a match, CPUs in the empty seats. In pop'n music 12
(from event phase 2: right-click > Unlocks), 13, 14 and 15–18, with e-AMUSEMENT on and a card in.

- **Where:** NET対戦 on the mode wheel (13 and 14: one left of ENJOY). 18 adds it as the fifth mode.
- **Alone** (pop'n 12–15), nobody comes: the game says so and plays the credit in CHALLENGE mode.
  pop'n 15 waits about two minutes first, while you set up your battle. pop'n 16–18 wait at
  CUSTOMIZE for opponents; what follows alone hasn't been looked at yet.
- **Online:** point every cabinet at one `hiraku-eamuse` (EA Service URL) and open TCP port 5740
  in the server's firewall. The cabinets need nothing opened. Each cabinet says at start whether
  it reached the relay; one that doesn't answer is given up after a few seconds, and the game goes
  on without NET TAISEN. Online cabinets run free, as real ones did.
- **More cabinets:** three meet and play with no CPU. A fourth waits for a match of its own.
- **Two cabinets on one PC** need a save folder each: `--nvram FOLDER`.
- **A LAN cable (pop'n 12–14, from a terminal):** start the first with `--lan-host SOCKET` and the
  other with `--lan-join SOCKET`, each with its own `--nvram`. SOCKET is a path, or `host:port`
  over TCP. They run in lockstep, both at the slower one's pace. `--lan-cabinets N` on the first
  for more (up to 4). A pop'n 13 that has heard another cabinet asks nationwide (全国対戦) or
  in-shop (店内対戦); left to its timer it takes nationwide.
- **After a battle** each player's class, badge, last battle and the rest are kept for the next.

Details: [EAMUSE3.md](EAMUSE3.md#net-taisen-popn-music-1214),
[pop'n 15](EAMUSE3.md#popn-music-15-a-pc-game), [16–18](EAMUSE3.md#popn-music-16-17-18-pc-games),
[PLAY-NOTES.md](PLAY-NOTES.md#konami-python-1-popn-music-914)

## Not there yet

- **KONAMI ID** in the IIDX games (pop'n 9's works on the cabinet).
- **IIDX 10th style to EMPRESS:** rivals and most rankings; 10th style doesn't carry a 9th style
  card's player over.
- **pop'n music 18's** boss battles.
- **pop'n 9's** HITASURA and Gambler Z boards and ee'MALL follow the game's formats but haven't
  been seen in play.
- **pop'n 12:** a card left at the OK screen until the timer runs out stays on "カードをお取り下さい".
- **DrumMania V:** the server keeps each player's results but doesn't give the game the player's
  song records back yet.
- **GuitarFreaks and DrumMania:** the puzzle isn't kept.
- **Real readers and boards** haven't been checked on hardware. The IIDX games run under Wine
  only so far.

Details: [PLAY-NOTES.md](PLAY-NOTES.md#bemani-pc-popn-music-1518-beatmania-iidx-9th-style-to-empress),
[pop'n 9–14](PLAY-NOTES.md#konami-python-1-popn-music-914)
