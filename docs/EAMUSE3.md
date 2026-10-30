# e-AMUSE3 for pop'n music 9 (Konami Python 1)

pop'n music 9 (2003, `C00:J:A:A`, Konami Python 1) was the first pop'n on e-AMUSEMENT. Konami's
servers for it closed in August 2003. HIRAKU plays the cabinet's card reader/writer, its 10-key,
its network, and the server, so the game's e-AMUSEMENT works again: cards, player data, KONAMI ID
and rankings.

This page is for two kinds of readers:
- **HIRAKU players:** how to run the server, offline or online (first section).
- **Server owners:** how to add pop'n 9 to an e-AMUSEMENT server, such as bemaniutils or one of
  your own. Everything the game asks and accepts is below, found from the game's own code (the
  EE program's addresses are given) and checked in HIRAKU where marked.

`tools/eamuse/check.py` checks a server against all of it. For bemaniutils there is a ready
patch, `tools/eamuse/bemaniutils/popn9-python.patch`, checked against the game (its README says
how it maps the game onto bemaniutils). It was written with an AI model, so it is for server
owners' own installs only: bemaniutils accepts no AI-written contributions.

## Running it

**In the game:** test mode > e-AMUSEMENT OPTIONS > e-AMUSEMENT **ON**, SAVE AND EXIT, then restart.
It is OFF by default, and the game plays the same without it.

**Controls:**
- **Insert:** puts the card in the slot, or takes it back when the reader has given it out.
- **The numeric keypad:** the 10-key (0–9, Enter, `.`; Backspace is Clear).

**The card** is `card.bin` in the save folder (`~/.local/share/hiraku/popn9`): 128 bytes, a fresh
pop'n 9 card until the game writes it. With a card number (`-card0`, below) it is
`cards/NUMBER.bin` instead, one file per number.

**Three ways to run the server:**

| | How | What for |
|---|---|---|
| **Offline** (default) | nothing to do: `hiraku-popn9 --window` | one cabinet; data in the save folder's `eamuse/`; runs are reproducible |
| **Online, HIRAKU's server** | on the server machine, `hiraku-eamuse --port 8080 --data DIR`; on each cabinet, `hiraku-popn9 --window -url server:8080` | several cabinets sharing cards, players and rankings |
| **Online, another server** | `hiraku-popn9 --window -url host[:port][/path]` | a server that implements this page |

**spice2x's options:** the e-AMUSEMENT options have spice2x's names and meanings
(`runtime/shell/eaoptions.h`), so a setup made for spice2x carries over. Each takes one dash or two,
and the launcher's Settings > e-AMUSEMENT writes them, laid out like spicecfg's Network page.

| Option | spice2x's name | In HIRAKU |
|---|---|---|
| `-ea` | Basic Local EA Emulation | the server inside HIRAKU. It is also what runs without `-url`, and it is ignored with `-url`, as in spice2x. Unlike spice2x's, it is the whole server: cards and data are kept |
| `-url URL` | EA Service URL | the cabinet's server: `host:port[/path]` (spice2x's form) or `http://host[:port][/path]`. `--eamuse URL` is the older name |
| `-smartea` | Smart EA | with `-url`: if that server doesn't take a connection at start (3 s), the server inside HIRAKU |
| `-p PCBID` | PCBID | 20 hex digits, sent in place of the game's own (pop'n 9's is `01401401000100045FEF`) as `srcid` and `gpcb_id`, for a server that only takes the machines it knows |
| `-card0 NUMBER` | Player 1 Card | the card's number, 16 hex digits (`E004...`). Without it, `card0.txt` (the working directory's, then the save folder's), as spice2x reads it. `-card1` and `card1.txt` are player 2's; pop'n has one reader |
| `-autocard off\|p1\|p2\|both` | Auto Card Insert | the card goes in by itself: it is in the slot from the start, and back in it 8 s after the reader gives it out, so the game takes it whenever it opens the slot |
| `-autopin` (HIRAKU's) | Type the card's PIN | a card HIRAKU's own server knows and holds a PIN for: its PIN typed on the 10-key when the game asks for it, a fixed time after the server reads the card (pop'n 14: 3 s; 15-18: 4 s, then Enter). Not with `-url` (the PIN is the other server's) |

- **A card with a number:** these games number their cards themselves: a new card asks the server
  (`numbering.assign`), and the game writes the number into the card. With `-card0`, HIRAKU gives
  the game the card's number itself, without asking the server, so the card carries the number
  set. Then every call about the card (`userdata`, `rank`, `playerdata`, ...) uses it. A server
  that knows the number from other games (the player's e-AMUSEMENT PASS in spice2x) can then link
  the pop'n card to the same player. The bemaniutils patch does: a number already on an account
  (as an e-AMUSEMENT PASS played in spice2x) saves to that account.
- **In online mode** the game's requests go out as the game sends them: encrypted as the game
  encrypts them, with the URL's path in front of the request path and the Host header set to the
  server. `-p` and `-card0` change the calls' contents, as above. Your network sees plain HTTP.
  HIRAKU does no DNS for the game except for its own lookups (below).
- **`HK_PY_NET_LOG=FILE`** logs every socket command, and every call and answer decrypted.
- **`hiraku-eamuse` options:**
  - `--bind ADDR` (default 0.0.0.0);
  - `--clock "2003-06-01 12:00:00"`: the time `ntp.get` gives, running from there (default: the
    host's clock);
  - `--tz 9`: with the host's clock, the cabinet's time zone in hours from UTC (default 9, Japan);
  - `--quiet`: don't print the calls.

**What the HIRAKU server keeps** (the save folder's `eamuse/`, or `--data`), plain text, one record
a line, tab-separated `key=value` fields:
- `numbering`: the next card number;
- `userdata-CARD`: a card's player data, as the game wrote it;
- `users`: KONAMI IDs (user, pass, cards, active);
- `rank-score`, `rank-hitasura`, `rank-gamblerz`: each entrant's best;
- `eemall-CARD`: optional, ee'MALL item numbers for a card, one a line (the server owner writes these;
  pop'n 9 itself never asks, see `eemall.i_get`);
- `message`: optional, written by the server owner: one line (up to 255 bytes, ASCII or
  Shift-JIS) that every cabinet downloads at start-up and scrolls across its title screen (see
  `package`). Delete it to stop.

## What the cabinet does on the network

1. **The board:** the interface board (not the PS2) has the Ethernet port. It gets its address by
   DHCP, and the game reads it from the board along with the gateway, the DNS server and a
   **local domain** (in HIRAKU, `PYTHON1-00000001`).
2. **The first lookup:** the game resolves `services.<local domain>` and POSTs `services.get` to
   it, on port 80 (`http://services` plus the domain: `0x18cd38`).
3. **The service list:** the answer lists the other services' URLs. The game uses them as given
   (`http://host[:port]/path`), with its own DNS lookups for the names.
4. **The rest:** `pcbtracker.alive`, then the sales report (`posevent`), then `ntp.get`. After
   that e-AMUSEMENT counts as up: the title says "e-AMUSEMENT 接続中!" and "カードが使用できます!!".
5. **Afterwards:** a ping (ICMP echo, raw socket) goes to the gateway from time to time. Three
   failed rounds give ROUTER COMMUNICATION ERROR. In HIRAKU the virtual network answers it.

## Transport

- **HTTP/1.0, one request per connection:**

  ```
  POST / HTTP/1.0
  Host: services.PYTHON1-00000001
  User-Agent:EAMUSE.Httpac/1.0
  Content-length: 84
  X-Eamuse-Info: 1-3eda6962-4e60          (encrypted calls only)

  <body>
  ```

- **The answer:** `HTTP/1.0 200 ...`, a `Content-Length`, and `X-Eamuse-Info` on encrypted
  answers only. Then the body, then **close the connection**: the game reads until the end of
  the stream. Any status but 200 fails the call.
- **No compression and no binary XML:** the body is XML text. Unlike later games, there is no
  `X-Compress` and no KBinXML. (pop'n 13 compresses one thing inside the XML: its player records.)

## Encryption

**Which calls are encrypted** depends on the call. **Answer in kind:**

| Encrypted (with `X-Eamuse-Info`) | In the clear (no `X-Eamuse-Info`) |
|---|---|
| services, pcbtracker, posevent, userid | ntp, numbering, package, userdata, rank, p_friend (eemall: never sent, below) |

- **The cipher:** RC4, with key = MD5(the 6 bytes of `X-Eamuse-Info: 1-xxxxxxxx-xxxx`, as hex,
  then the 26-byte shared secret `69d74627d985ee2187161570d08d93b12455035b6df0d8205df5`). This
  is the same scheme as later e-AMUSEMENT games; bemaniutils' protocol code has it.
- **Encrypted calls:** answer encrypted, with your own `X-Eamuse-Info` (any 1-xxxxxxxx-xxxx value).
- **Calls that come in the clear:** answer in the clear, **without** `X-Eamuse-Info`. The game
  compares the header's crypto version with the call's packet type, and drops an answer that
  differs before reading any XML (`0x190290`). This is the easiest thing to get wrong.

## The envelope

**A call:**

```xml
<call srcid="01401401000100045FEF" model="C00:J:A:A"><MODULE method="..." .../></call>
```

- **srcid:** the cabinet's PCB id. The same value comes as `gpcb_id` in some calls.
- **model:** the game; pop'n music 9 is `C00:J:A:A`.
- **Several calls in one:** a packet can carry more than one module element (the start-up's
  `posevent` sends income, sales and sale together).

**The answer:**

```xml
<response><MODULE status="0" ...>...</MODULE>...</response>
```

- **One module element per call, in order.**
- **Failure:** `status` other than 0, or a `fault` attribute, fails that call; `fault` on
  `<response>` fails them all. Each module's own status codes are listed below.
- **XML details:** no XML declaration is needed, and **one with an `encoding` is refused**: with
  `<?xml version="1.0" encoding="shift-jis"?>` (or `SHIFT_JIS`) in front, the game asks for
  `services.get` again and again; `<?xml version="1.0"?>` or none is fine [game]. Double quotes,
  elements written out or empty (`<x/>`) both parse, attributes in any order, and extra ones
  (`dstid` on `<response>`, `__type` on `<b>`) are ignored [game]. A text payload goes in a child
  element (`userdata`'s `<b>`).
- **The game's parser** (`0x1910e0`) calls a start handler per element, a text handler, and an
  end handler. A call is done when its module element closes.

## The modules

**Legend:**
- **[game]:** sent by the game and answered in HIRAKU, the answer accepted;
- **[code]:** from the game's request format and response parser, not seen in play yet.

Each module's parser is listed as `parse 0x…`.

**services.get** [game] (parse `0x18a808`): the other modules' URLs.

```xml
<call ...><services method="get"/></call>
<response><services expire="600" status="0">
  <item name="posevent" url="http://host[:port]/"/>
  ... pcbtracker, numbering, package, userdata, userid, local ...
</services></response>
```

**Every name above is needed.** The rank, p_friend, eemall and ntp methods look up **`local`**,
not their own names (each method's entry names the item it uses; `0x1e81e8`…). A missing item
means that method is never sent. `expire` is seconds.

**pcbtracker.alive** [game] (`0x18b588`): `<pcbtracker status="0" expire="600"/>`; `expire` optional.

**posevent** [game] (`0x18abb0`, `0x18af68`, `0x18b308`): the coin and sales reports.
- **income** (`slot1="00000002"`, coins as 8 hex digits): answer `<posevent status="0"/>`.
- **sale** (`article1=... payment1=...`) and **sales** (`play= coin= service= article=`): the
  answer **needs `approve`** (hex), e.g. `<posevent status="0" approve="1" margin="0"/>`.
  `margin` is optional, capped at 1000.

**ntp.get** [game] (`0x19b740`):

```xml
<ntp status="0"><time year="2003" month="6" day="1" hour="12" min="0" sec="19"/></ntp>
```

- **The game won't start e-AMUSEMENT** until `ntp.get` answers or 40 s pass.
- **The clock:** the game sets its own clock to this time as given (`0x18a138` → `0x1a9598`) and
  stamps its times "+9". So send the cabinet's local time, Japan time for a Japanese cabinet,
  not UTC.

**package.list / package.intend** [game] (`0x197960`; the flow `0x184410`): downloads. pop'n 9
takes one package, **`message`**: a line of text it scrolls across the top of its title screen
(`0x1237c0`). At start-up, from the title:
1. `<package method="list" pkgtype="all" model=/>`. An empty answer `<package status="0"/>` means
   nothing to download. An offer is an item:
   `<item url="http://host/message" name="message" desc="" size="N" pkgtype="all" sumtype="" sum="" from="" till=""/>`
   (the game keeps url, name, desc, size, pkgtype, sumtype and sum; it looks only for the name
   `message`).
2. `<package method="intend" url="(the item's url)" model=/>`, answered `<package status="0"/>`.
3. A plain HTTP/1.0 `GET` of the url (no e-AMUSE envelope, no encryption); the game keeps at most
   `size` - 1 bytes of the body. ASCII and Shift-JIS (kana and kanji) both show.

Checked in HIRAKU, offline and online through hiraku-eamuse: "HELLO FROM THE SERVER" and
"ひらくへようこそ！ HIRAKU e-AMUSEMENT 稼動中" scrolled across the title. A server should give a url
the cabinet reaches like its services (hiraku-eamuse uses the Host the call came with).

**numbering.assign** [game] (`0x197530`): a new card's number.

```xml
<numbering method="assign" label="cardv1" model="C00:J:A:A" format="card16m10"/>
<numbering status="0" number="2003010000000010"/>
```

- **The number:** 16 hex digits, written into the card as 8 bytes. The HIRAKU server gives
  decimal numbers with a Luhn check digit (its reading of `card16m10`). The game doesn't check
  the digit, but numbers must be unique.

**userdata.read** [game] (`0x197d80`; statuses `0x184ca0`):

```xml
<userdata method="read" card="2003010000000010" model="C00:J:A:A" label=""/>
<userdata status="0" time="2003-06-02 06:42:41+9"><b>BASE64...</b></userdata>
```

- **109:** not registered, so the game registers the player (name, then PIN).
- **110:** expired.
- **Otherwise:** send back exactly the `time` and the `<b>` elements from the last write.

**userdata.write** [game] (`0x197fc0`; answer `0x1981c0`): at the end of a credit.

```xml
<userdata method="write" card="..." time="2003-06-02 06:42:41+9" model="C00:J:A:A" label=""><b>BASE64...</b>...</userdata>
<userdata status="0"/>
```

- **The data:** base64, in `<b>` chunks of up to 0x200 bytes each.
- **The player data:** name, character, options, and the player's **PIN**. The game checks the
  PIN itself; the server only stores the blob.

**userid** (KONAMI ID: a user and password with cards; `0x198858`, `0x198650`; statuses `0x1863e8`, `0x186620`, `0x186818`):

| Call | Answer | Statuses |
|---|---|---|
| `ctou card=` [game] | `<userid status="0" user="NAME" active="1"/>` | 112: no KONAMI ID for the card; active 0 = provisional |
| `adduser model= user= pass= card=` [game] | `<userid status="0" alter=""/>` | 113: the user exists; 114: the card is someone's |
| `addcard user= pass= card= model=` [game] | `<userid status="0"/>` | 115: no such user; 116: wrong password; 114: the card is someone's |

- **`user`** is what the player typed: 8 to 32 characters from `a-z 0-9 . - _` (the screen adds
  "@konami"). **`pass`** is 4 digits from the 10-key.
- **`aflag="%d"`** is in the request builder (`0x1984f8`) but only sent when nonzero; the cabinet's
  flows never set it. **`alter`** (up to 40 characters) is read from a status-0 answer and kept;
  nothing seen shows it. Answering `alter=""` is enough.
- **What the game does with the status:** `adduser` 0: "「KONAMI ID」と「PASSWORD」が登録されました";
  113: back to the menu to try another ID; anything else: an error page, and the credit ends.
  `addcard` 0: "ポップンミュージックカードの登録が完了しました！"; 115 or 116: try again (three times
  in all); anything else: an error page. A failed call is retried three times first.

**rank** (all six: `local`, in the clear):

**`get` / `get_h` / `get_gz`** [game] (parse `0x19a080`, `0x19a5c8`, `0x19aaf0`): the
boards shown in attract mode and after play. The request carries `course_offset course_num
rank_offset rank_num`; `get_gz` has no courses.

```xml
<rank status="0">
  <item course="0" entry="2"/>                                                   (a header per course)
  <item rank="1" name="82A082A082A0" score="98765" cool="321" area="13" id="..."/>
  ...
</rank>
```

- **`get`:** score and cool.
- **`get_h`** (HITASURA): music and combo.
- **`get_gz`** (Gambler Z, by shop): `<item entry="N"/>`, then items with rank, prog, parts (hex),
  shop_name, area and id.
- **Fields:**
  - `name` and `shop_name` are uppercase hex of the Shift-JIS bytes (a plain-text `shop_name`
    shows blank) [game];
  - `id` is at most 36 characters;
  - `area` is the prefecture: 0–46 in JIS order from 0 (0 北海道, 12 東京都, 13 神奈川県, 46 沖縄県),
    255 none [game];
  - up to 20 courses, 100 entries each.
- **Where they show** [game] (only while the cabinet's network check passed, `0x2571f8`):
  - attract mode, after each demo song: "popper's ranking", one page per course (course name,
    entries as "N poppers", then 順位, なまえ, 県名, SCORE, COOL数);
  - attract mode in event phases 1 and 2, after the labyrinth: "たたかえ！ギャンブラーZ お店対抗
    RANKING" (順位, お店のなまえ, 県名, and `parts` drawn as one picture per item);
  - after a course played with a card: INTERNET RANKING, from the `register` answer.

**`register`**, **`register_h`** and **`register_gz`** [game] (parse `0x1992e8`, `0x1997f0`, `0x199c60`; statuses `0x184f68`): a result.

```xml
<rank method="register" card_id= konami_id= gpcb_id= name= course= option= score= cool= great= good= bad= combo= shop_name= area_num=/>
<rank status="0"><item entry="12" rank="3" update="1" name=.. score=.. cool=.. area=.. id=../></rank>
```

- **The answer:** the player's place in the course, the number of entries, and whether the
  result is a new best.
- **`register_h`** sends `music combo`: the songs cleared in the HITASURA run and its combo, e.g.
  `course="0" music="3" combo="365"` for the first course (3 songs) cleared. A run that clears no
  song sends none.
- **`register_gz`** sends `gpcb_id prog parts shop_name area_num`, at the end of a credit in
  event phases 1 and 2 [game], after that credit's labyrinth (its walk and any boss fight), while the
  item list shows:
  - `parts` is the labyrinth's items word (below) in 8 hex digits, `prog` the number of its bits set
    (0–25; `0x188248`). A save with two songs and the first boss sent `prog="4" parts="00084003"`;
    a credit that beat the first boss sent `prog="2" parts="00084000"` at its end.
  - Rank shops by `prog` (then `parts`): hiraku-eamuse does.
  - **Gambler Z progress is the shop's, per cabinet.** The labyrinth itself lives in the cabinet's
    own settings; the server only ranks the shops (see below).
- **`get_h`** and **`get_gz`** come at start-up in phases MAX and 1–3.
- **Phase 3** (GAMBLER Z DATA RELEASE) ends the event [game]: no labyrinth after a credit and no
  `register_gz`, but `get_gz` still comes at start-up, and attract mode shows the shop ranking
  (the final standings) after its demos, in place of the course pages.
- **Other statuses:** 700 and 701 make the game show "not ranked"-style results (meaning
  unconfirmed).
- **When it's sent:** a course result is registered when a card is in use (`0x1359e8`, from the
  result flow `0x129c68`), even for a failed course.
- **With a KONAMI ID** [game]: once `ctou` gives the card an ID, the game sends it as `konami_id`,
  and the INTERNET RANKING screen after a course shows "? 位 ? 人中" instead of the place the
  answer gives. The same with HIRAKU's server and with bemaniutils, whatever `id` and attribute
  order the answer has; without an ID the place shows. The rule wasn't found (the screen's tasks
  around `0x133900`).
- **Seen from the game:**

  ```xml
  <rank method="register" card_id="2003010000000010" konami_id="" gpcb_id="01401401000100045FEF"
        name="82A082A082A0" course="0" option="0" score="918" cool="1" great="1" good="3" bad="36"
        combo="0" shop_name="" area_num="255"/>
  ```

  The name is hex of its Shift-JIS bytes ("あああ"); `area_num` 255 is no prefecture set.

**The Gambler Z labyrinth** [game] ("ギャンブラーZ", pop'n music internet event), for reference: it
is the cabinet's, not the server's. In event phases 1 and 2 (`0x143c98`), after a credit's results
(and its staff roll when all stages were cleared), the game runs the labyrinth (`0x19e188`):
- **Steps:** one per cleared stage, two for a stage whose song counts double (`0x1088c8`, "Walk %d").
  Three cleared stages walked "1階 Aフロア" three cells.
- **The walk** follows direction bits in the map's cells. At a junction the player chooses (buttons
  4 and 6, red to confirm, a 180-frame timer); a treasure cell gives an item; stairs change floor.
- **Bosses:** from floor index 10, a door cell starts a fight: a song from the floor's table (`0x19e628`),
  whose score deals 1–5 damage (70,000 / 80,000 / 90,000 / 95,000, `0x19e720`) to the boss's 25 HP
  (50 on floor index 15). Then the walk goes on. A fight under way (HP saved, nonzero) resumes at
  the next credit's labyrinth. Seen with --autoplay, from saves set to floor index 10 ("5階 Kフロア")
  with HP 25, then 5: the boss's challenge, an EX STAGE (テクノポップ "321 STARS" EX), 100,000 and 5
  damage; at 25 HP the boss stays ("まだまだ…", 20 saved); at 5 it falls ("やーらーれーたー！"),
  INFORMATION "隠しコース 出現！ すわコース" (selectable in EXPERT from the next play), the bits
  0x84000 set, and the hero back on "1階 Aフロア" (floor 0, position 0, 0).
- **Saved** in the settings record (battery RAM, mirrored like the rest of it; the file at +0x10):

  | Offset | Field |
  |---|---|
  | `0x18` | items (low 24 bits; 0xffffff = all) and 0x1000000 = cleared, u32 |
  | `0x1c`, `0x1e` | position, s16 pair (cells; three steps took `0x1c` from -1 to -4) |
  | `0x20` | floor, u8 |
  | `0x22` | the boss's HP left, u16 (0 = no fight under way) |

  - **The bits:** 0–13 a song each, the treasures of floors 1–14 (ジャジー, ポジティブ (CS3);
    モーター5, ボッサ, プログレ (CS4); オイパンク (CS6); メランコリー, テラピー, スムース, モダン80,
    インダストリアル (CS7); ポップス・アンコール, ロックギター, ソフトロック from IIDX (REMIX)); bits
    14+k and 19+k the five bosses (an EX chart and an EXPERT course each: テクノポップ and すわ,
    ターバン and サナ, ピラミッド and パーキッツ, スカ and Des-ROW, オイパンク0 and クラシック);
    bit 24 the last boss, cleared. Read from the item list a save with every bit shows;
    `tools/unlock.py popn9` decodes a save.

  The same record holds the cabinet's shop: `area_num` (`0x35`, u8, 255 = not set) and the shop
  name (`0x37`, Shift-JIS, up to 12 characters), set in test mode > e-AMUSEMENT OPTIONS >
  e-AMUSEMENT SETTINGS (SHOP NAME SETTING, PREFECTURE). Every call that carries `shop_name` sends
  it as hex: "ひらく" and 東京都 went out as `shop_name="82D082E782AD" area_num="12"` [game].

**p_friend.get / ask / state** [game] (`0x19b070`…; the card flow `0x16da30`): pop'n friends.
`p_friend` is the player's character's place in the game's character table (`0x19ecf8`, 0–181).
The answers are status only, `<p_friend status="0"/>`.

| Call | When | Statuses |
|---|---|---|
| `get card_id gpcb_id p_friend shop_name area_num` | after the PIN, while the player data's friend flag (`+0x414`) is 0 | 0 sets the flag; 600: no, without retrying |
| `ask card_id p_friend` | after the PIN, once the flag is set | 0 keeps it; 601: no, without retrying, and the flag is cleared (the next credit sends `get` again) |
| `state card_id p_friend status` | at the end of the credit, before `userdata.write` | `status` is the game's own result (1 after a `get` or `ask` that passed, 0 otherwise) |

Anything else is retried, three times in all. The flag travels in the player data, so it is the
server's `userdata` that decides which of `get` and `ask` a card's next credit sends.

**eemall.i_get** [code] (`0x19b558`): the card's ee'MALL items (bought on Konami's ee'MALL site,
delivered to the game): `<eemall status="0"><item num="N"/>...</eemall>`, up to 99.
**pop'n 9 never sends it:** its only caller (`0x186ef8`) is called from nowhere, neither directly
nor through a pointer. A server may leave it out.

## A card's life (as checked in HIRAKU)

**A new card:**
1. **The title:** with e-AMUSEMENT up, the title says cards can be used.
2. **Start:** coins, then Start; the INFORMATION screen asks for the pop'n card. The reader reads
   it: a fresh pop'n 9 card, "C00 NEW CARD".
3. **The calls:**

   ```
   name entry -> PIN twice on the 10-key ->
   numbering.assign -> (the game formats, writes and reads back the card) ->
   "カードセーブ完了" -> userid.ctou -> play ->
   p_friend.state, userdata.write at the end of the credit
   ```

**The card again:**

```
userdata.read (status 0) -> the PIN -> "name, character, options" ->
p_friend.get (or .ask, once a get passed), userid.ctou -> play -> ... p_friend.state, userdata.write
```

**A wrong PIN:** "パスワードが違います", and play without the card.

**A KONAMI ID, on the cabinet:** at the end of a credit played with a card that has none
(`ctou` 112), the game shows INFORMATION, "KONAMI IDのご案内" (`0x1a8678`), for two minutes:

| Item | Does |
|---|---|
| KONAMI IDとは？ | explains it |
| 新規発行 | a new ID: the ID on a character wheel (buttons 4/6 turn it, red picks; its last items are cursor left and right, delete, 決定 and back), the password twice on the 10-key -> `userid.adduser` |
| カードへ登録 | this card to an existing ID: the ID, the password once -> `userid.addcard` |
| 登録を中止する | (selected at first) leave |

Checked: a new ID `hiraku09` with the first card; then a second card, registered as new, added
to it. The server's `users` then lists both cards, and `ctou` gives the ID for either.

Scripts: `oracle/scripts/popn9-eamuse-on.his`, `popn9-card-new.his`, `popn9-card-again.his`,
`popn9-konamiid-new.his`, `popn9-konamiid-addcard.his`.

## The card

The card holds no player data, only an identity.
- **Layout:** 128 bytes; 0x41 used.
  - bytes 0–3, a header, and byte 4 its CRC-8 (reflected 0x31, initial 0xff, inverted; `0x1a3050`);
  - bytes 6–13, the number from `numbering`, in reverse byte order;
  - bytes 5–0x3e, data; 0x3f–0x40, its CRC-16 (reflected 0x1021, initial 0xffff, inverted;
    `0x1a30c0`).
- **Header types** (`0x1a2728`):

  | Header | Type |
  |---|---|
  | `28 23 41 01` | a new pop'n 9 card |
  | `28 23 49 01` / `28 24 45 11` | a new pop'n 10 / 11 card (their programs; "Later games") |
  | `68 23 41 01` | registered |
  | `6c 23 41 01` | "death" (dead) |
  | `18 23 41 01` / `58 23 41 01` | location test |
  | `08 1f 7d f1` | Konami common |
  | `08 20 82 01` | e-AMUSEMENT common |

## Later games: pop'n music 10–14

Each later pop'n speaks the same transport, cipher and envelope, with its own `model` (`C20:J:A:A`
pop'n 10, `D11:J:A:A` 11, `E12:J:A:B` 12, `E13:J:A:E` 13, `F14:J:A:B` 14). HIRAKU's server picks
the game by the model's first three characters; pop'n 9's answers are unchanged. Player data is
kept per game (`userdata-C20-CARD`, ...). Turning e-AMUSEMENT on: test mode > NETWORK OPTIONS >
e-AMUSEMENT OPTIONS, as in pop'n 9 (pop'n 13 and 14 start their menus' cursors on EXIT).

**pop'n music 10** (checked in HIRAKU: a new card registered, a whole credit saved, the card read
back with its PIN; `oracle/scripts/popn10-eamuse-on.his`, `popn10-card-new.his`,
`popn10-card-again.his`). The EE program's addresses are pop'n 10's.
- **As pop'n 9:** services (plus `eemall` and `shopinf`, looked up by those names), pcbtracker,
  posevent, numbering, package, userdata, userid, p_friend `state`/`ask`.
- **The card:** a new pop'n 10 card is `28 23 49 01` ("C20 NEW CARD", `0x1ad3c8` type 7; used
  `68 23 49 01`). A pop'n 9 card is offered for carrying over (name and clears; it then no longer
  works on pop'n 9): a pop'n 10 `userdata.read` of a card with no pop'n 10 data gets its pop'n 9
  data. When the player refuses, the game hands the card back with a card-unit **reset**.
- **p_friend.get** sends `card_num card_id name shop_name area_num` and reads `<p_friend
  chara_num rare_f/>` (`0x19e7e8`); no `chara_num` means none.
- **rank.exp** (pop'n 9's `rank.register`, the same fields): answer `<rank status="0" update entry
  rank hi_score hi_cool/>` on the element itself (`0x19dda8`).
- **rank.get** (no attributes): the rankings, base64 in `<b>` (`0x19e300`; the game appends one
  `<b>` after another, into a 16 KiB buffer it clears first), copied into a 0x1f30-byte table
  (`0x187be4`, at `0x18287c8`). Little-endian:
  - 15 EXPERT courses of 0x1e4 bytes each: a count (up to 20), then 20 entries of 0x18 bytes:
    0x00 the name (Shift-JIS, 13 bytes), 0x0d the place, 0x0e the prefecture (JIS code, from 1),
    0x10 the score (shown with the game's score display on), 0x14 the COOL count (`0x12b218`).
    The attract mode's INTERNET RANKING screens show them (event phase 1 and on).
  - Then (at 0x1c5c) HOSHI NI NEGAIO's shop board: a count (up to 20), then 20 entries of 0x24
    bytes: 0x00 the shop's name (25 bytes), 0x19 the place, 0x1a the prefecture, 0x1c the shop's
    granted wishes (64-bit; each a bucket icon, all 37 a special one) (`0x13afa0`). Shown as
    "お星様にお願い！ お店対抗 RANKING" in phases 2 and 3.
  - **Size:** the whole table in one `<b>` (10.7 KB) is refused (the game asks again, three
    times); HIRAKU sends it in `<b>` pieces of 768 bytes and leaves out the trailing zeros.
  HIRAKU builds it from `rank.exp` (the best per player and course, by score then COOL) and
  `rank.shop` (the latest per shop, ranked by wishes).
- **rank.course:** the server's ranking course, `<rank status="0" course_num music="a,b,c,d"
  area_num course_name user_name comment type/>` (`0x19e420`); status **117** means none.
- **rank.shop** sends `praystar_lo praystar_hi shop_name area_num`: the shop's HOSHI NI NEGAIO
  wishes (the items word of its settings, low and high 32 bits, signed decimal), its name and
  prefecture (`0x187a98`). Sent after a credit's HOSHI NI NEGAIO screen with e-AMUSEMENT on
  (`0x1a6f78`); answer `<rank status="0"/>` (121 is also taken). HIRAKU keeps the latest per shop
  (`C20-rank-shop`: `shop_name area_num mask mask_hi when`) for `rank.get`'s shop board.
- **message.get:** `<message status="0" phase="N" text="..."/>` (`0x19f4e0`). `phase` >= 0 sets the
  cabinet's event phase when it differs (`0x18dee0`, "PHASE ONLINE UPDATE"); a missing phase reads
  as 0, so answer `-1` unless you mean it. HIRAKU's server takes it from the file `phase-C20`.
- **eemall.get** (`card_id`): `<eemall status="0" now_point add_point>`, then `<item num/>` (up to
  99) and `<kisekae path pri/>` (up to 15) (`0x19eda8`).
- **shopinf.getsservip** (`srcip`): up to five `<sservip ipaddr/>`; none is fine.
- **ntp.get:** one attribute, `<ntp status="0" time="2003-06-01 12:00:00+9"/>` (`0x19da48`: six
  numbers, then an optional zone in hours).

**pop'n music 11** (`D11`; checked in HIRAKU: a new card registered, a credit saved with its
mileage, the card read back with its PIN; `oracle/scripts/popn11-eamuse-on.his`, `popn11-card-new.his`,
`popn11-card-again.his`). Game features move into a module named after the game, with a `type`;
answers are that element again (pop'n 11's parsers `0x18c858`-`0x18ddf8`), and its calls go to
the `local` service.
- **The card:** a new pop'n 11 card is `28 24 45 11` ("D11 NEW CARD"). After the card, red, the
  name (a kana grid: blue moves, red enters, the white buttons switch kana/letters; 決定 is one
  blue-left from あ), the PIN twice.
- **`D11 rank exp`:** pop'n 10's `rank.exp` (plus `chara_num`), answered on `<D11 update entry rank
  hi_score hi_cool/>`. **`pf ask`**, **`game select`** (play counts: `<music num play
  cnt/>` children), **`renewc reg`/`ask`** (a card's renewal: `old_card_id`, `new_card_id`): `<D11/>`.
- **`D11 rank get`:** the rankings in `<b>` pieces, as pop'n 10's `rank.get`, into a 0x1f80-byte
  table (`0x1932a8`): the 15 EXPERT courses as pop'n 10's, except that each entry has the
  prefecture at 0x0d and the place at 0x0e. Then (at 0x1c5c) POPN TOURIST's shop board, "ポップン・
  ツーリスト 獲得アイテム一覧": a count (up to 20), then 20 entries of 0x28 bytes: 0x00 the shop's
  name (24 bytes), 0x18 the place, 0x19 the prefecture, 0x1a-0x1d the month, day, hour and minute
  its finds were last added to, 0x20 the finds word (one bit per find; the STAGE column counts them), 0x24
  `extra` (`0x129240`). The attract mode shows the courses at phase 1 and on, and the shop board
  at phases 2 and 3. HIRAKU ranks the shops by finds, then by the earlier date.
- **`D11 rank shop`** sends `item extra shop_name area_num`: the shop's POPN TOURIST finds word and
  a second word, signed decimal. Answer `<D11/>`. HIRAKU keeps the latest per shop
  (`D11-rank-shop`), dated by the server's clock when the finds grew. (Checked with records written by
  hand; no HIRAKU run has sent one yet.)
- **`D11 pf get`:** `<D11 pf_num mobile/>`. **`D11 game get`:** `<D11 phase message expire_days/>`;
  `phase` as pop'n 10's `message.get` (-1 leaves the cabinet's own; HIRAKU's file `phase-D11`).
- **`D11 player set`** sends `card_id mileage ticket="t0,...,t8,"`; **`player get`** reads them back,
  with `course="c0,c1,c2,c3"`: `<D11 mileage ticket course/>`. HIRAKU keeps `player-D11-CARD`.
- **`eemall.check`:** `<eemall/>`. **`portfw.get`** (`privateip`): `<portfw globalip port/>`; none is fine (pop'n 12–14's NET TAISEN needs them, below).

**pop'n music 12** (`E12`; checked in HIRAKU: a new card registered, a credit saved, the card read
back with its PIN; `oracle/scripts/popn12-eamuse-on.his`, `popn12-card-new.his`, `popn12-card-again.his`).
The game-named calls as pop'n 11's, but **no `userdata`**: the player lives in `E12 playerdata`.
- **The card:** a new pop'n 12 card is `28 25 45 21` (type 0xf, `0x1ad4e4`), and pop'n 12 reads a card
  only with both CRCs right, the data's too (`0x1acbb0`): a fresh card needs the CRC-16 of its blank
  bytes 5-0x3e at 0x3f. Test mode > CARD R/W CHECK > CARD CHECK says NEW CARD for it. After the card,
  OK and red, the name (a kana/letter grid; the 99 s timer registers what is entered), the PIN twice.
- **New cards need the network up:** the game takes them only while its pings come back
  (`0x19803c`). It pings 10.2.1.10, or what the `keepalive` service names: `<item name="keepalive"
  url="ping://ADDRESS?t1=interval&amp;t2=wait&amp;rt=retries&amp;pa=router&amp;ga=center"/>`
  (`0x1981b8`; pop'n 10–14 all look `keepalive` up). It sends each ping on a socket it closes at
  once and reads the replies on another.
- **The interface board's own address:** after a new card's OK the game announces itself on the shop
  LAN (a 28-byte UDP broadcast to port 5700, `0x19fdc0`), first asking the board its address and
  netmask: board network commands **0x08** and **0x09** (RPCs 7 and 8 of the board library; board
  command = RPC number + 1), answered with 4 bytes in the reply's second word. Reliquary doesn't have
  them; without an answer the game waits there for good.
- **`playerdata new`** (`card_id card_num name password shop_name pref`) and **`get`** (`card_id`)
  answer the player's binary record, base64 in `<b>`: `<E12 status="0"><b>...</b></E12>` (`0x1917f8`;
  decoded at `0x18f860` into `0x1867e68`); `get` of an unknown card: status 109. The record, 0x708 bytes:
  0x00 the name (Shift-JIS, 13 bytes), 0x1b the puzzle position, 0x1c-0x33 six puzzle pieces
  (32-bit), 0x34 the password (four digits, a byte each), 0x38 the option and 0x3a the character
  (16-bit), from 0x3c a 16-bit word per song (2-bit clear marks per chart in bits 0-7, flags
  0x100-0x800; `0x1b3958`), from about 0x5f0 the network battle's (`0x19ef40`).
- **`playerdata set`** sends a credit's changes (`0x1919e8`): `card_id name password card_num option
  chara_num mode_num matching room_num puzzle_now puzzle_piece`, then `<music num sheet data score/>`
  per song played (`data` is the song's word in the record), `<expert course_num score_0..3/>`,
  `<netvs .../>`. HIRAKU merges them into the record (`pdata-E12-CARD`) and keeps the best score per
  chart (`pscore-E12-CARD`).
- **`playerdata score`** (`card_id music_num sheet_num`, asked before a song): `<E12 status="0"
  score="N"/>` (`0x192a00`).
- **`game active`** (`shop_name pref phase active card_use name`, at a credit's start and end):
  `<E12/>`. **`game get`** reads `phase message` and net battle's `play_enable local_matching_enable
  matching_sec room_num ojama_msec ojama_gauge cpu_level`.
- **`E12 rank get`:** the rankings in `<b>` pieces, as pop'n 10's `rank.get`, into a 0x1f30-byte
  table (`0x1974a8`, at `0x181c360`): pop'n 10's sizes, but the prefecture before the place, in the
  courses (0x0d, 0x0e) and in the shops (0x19, 0x1a). Then (at 0x1c5c) NINPOUCHO's shop board,
  "ポップン秘伝忍法帖": a count, then 20 entries of 0x24 bytes: 0x00 the shop's name, 0x19 the
  prefecture, 0x1a the place, 0x20 the shop's NINPOUCHO word (its 26 bits drawn as scrolls, all of
  them as 免許皆伝; `0x1a5778` copies the board). The attract mode shows the courses at phase 1 and
  on, the shop board at phase 3. HIRAKU ranks the shops by scrolls, then by the earlier date.
- **`E12 rank shop`** sends `item shop_name pref`: the shop's NINPOUCHO word (`0x197380`). Answer
  `<E12/>`; HIRAKU keeps the latest per shop (`E12-rank-shop`).
- **Not done yet:** `tmp`, `renew` (`playerdata conv`: below, "One card for all"); `cardreis.altercard`; a card left at the OK screen until
  the timer runs out stays on "カードをお取り下さい" (the card status's 0x40, card at the slot mouth,
  isn't played).

**pop'n music 13** (`E13`; checked in HIRAKU: a new card registered, a credit saved, the card read
back with its PIN, also with `-card0`; `oracle/scripts/popn13-eamuse-on.his`, `popn13-card-new.his`,
`popn13-card-again.his`). Its game program exists only in RAM (a second stage loads it at
0x1730000-0x1820000, data to about 0x1940000; the addresses below are there). The game-named calls as
pop'n 12's, keyed by a **ref id** from a new module, `cardmng`.
- **The services list needs `message`, `cardmng` and `cardreis`:** the library looks a call's URL up
  by its module's name in the services list (`0x17d94c0`) and fails the call at once, without
  sending it, when the name is missing (-10). Without `message` the start-up fails right after
  `services.get` with "センターサーバーと通信できません" (5-2007-0000: the status check `0x17e6798`
  sees `message.get` failed), and without `cardmng` a new card is given back after the PIN.
- **The card:** a new pop'n 13 card is `28 25 45 31` (type 0x13 of the header builder `0x180b498`;
  a used one `68 25 45 31`). Both CRCs are checked, as in pop'n 12. The game confirms the card by
  itself (no OK to press), then the name and the PIN twice, as in pop'n 12.
- **`cardmng`** (`0x17f66fc`...; answers parsed into the call's fields at `0x1497c8`...):

  | Call | Answer | When |
  |---|---|---|
  | `getnewid cardtype newflag` | `<cardmng status="0" cardid="16 digits" refid="..."/>` | a new card: its id (written on the card, as `numbering`'s number was) and its player's ref id |
  | `inquire cardtype cardid update` | `<cardmng status="0" refid="..." newflag="0\|1"/>` | a card read back |
  | `gettmpid cardtype oldid` | `<cardmng status="0" newid="..."/>` | an older card's player on a new id (not seen yet) |
  | `rebind cardtype newid` | `<cardmng status="0"/>` | (not seen yet) |

  `cardtype` is 0 for the magnetic card. HIRAKU keeps the cards in `cards` (cardid, refid) and gives
  ref ids `48524B` and 10 hex digits; `inquire` registers a card it doesn't know (newflag 1).
  `userid.ctou` gets the ref id as its `card`.
- **`playerdata`** as pop'n 12's, with `ref_id` in every call (`new` and `get` also send `card_id`);
  HIRAKU keys the player by it (`pdata-E13-REFID`, `pscore-E13-REFID`).
- **The record is compressed:** `new` and `get` answer `<E13 status="0" f_compress="1"><b>...</b></E13>`
  (`0x17f83c0`, `0x17f74d0`). With `f_compress` not 0 the game decompresses the `<b>` data
  (`0x1810240`): 4 bytes, the record's length (little-endian), then LZSS: a flag byte per 8 items, bit
  set = a literal byte, clear = a match of 2 bytes, `p0 | (p1 & 0xf0) << 4` a position in a 4 KiB
  window (starting at 0xfee, zero-filled) and `(p1 & 15) + 3` bytes. Uncompressed, the record (0xd6c
  bytes, about 4.6 KB in base64) makes the answer too big for the game's HTTP reader, and the call
  fails. With `f_compress` 0, `get` answers for an older card take a pop'n 12 record (0x708 bytes,
  `0x17e9dd8`), converted by the game (`conv`).
- **The record** (0xd6c bytes, at `0x166daa8`): 0x00 the name (Shift-JIS), 0x1c the password (four
  digits, a byte each), 0x20 the option and 0x22 the character (16-bit), from 0x28 a 32-bit word per
  song (the clear marks; `set`'s `data`, `0x1810e60`). The rest is not known yet.
- **`playerdata set`** (`ref_id name password card_num option chara_num mode_num music_num sheet_num
  category_num rare_ojama event_chara matching room_num`, then `<music num sheet data score/>` per
  song, `<netvs .../>`) and **`score`** (`ref_id music_num sheet_num s_challenge`) as pop'n 12's.
- **The network:** answers reach the game in pieces of at most 1460 bytes, as TCP over Ethernet
  delivers them; HIRAKU's virtual network hands them over that way (pop'n 13's reader fails on a
  2.5 KB answer handed over in one piece, `0x17ddb28`).
- **`E13 rank get`:** the rankings in `<b>` pieces into a 0x22a4-byte table (`0x17eaed8`, at
  `0x160fb20`; `f_compress` may be left out): the 15 courses as pop'n 12's, then two shop boards,
  POP'N LAND's (shown at phase 1, "ポップランド") and POP'N WORLD's (phase 2), each a count and 20
  entries of 0x28 bytes (`0x17eb010`): 0x00 the shop's name (24 bytes), 0x18 the place, 0x19 the
  prefecture, 0x1a the stage, 0x20 and 0x24 the area's words, low and high (the attractions built,
  drawn per bit, COMPLETE!! for all). pop'n 13's attract mode shows no course boards.
- **`E13 rank shop`** sends `stage item_hi item_lo shop_name pref` after a credit (`0x17ead80`): the
  area of the cabinet's phase. Answer `<E13/>`. HIRAKU puts it on that area's board, from the phase
  the shop's last `game active` sent (`E13-shop-phase`), and keeps the latest per shop and board.
- **Not done yet:** `tmp_set`, `tmp_ref`, `cardreis.altercard`, `gettmpid`, `rebind`
  (`playerdata conv`: below, "One card for all").

**pop'n music 14** (`F14`; checked in HIRAKU: a new e-AMUSEMENT PASS registered, a credit saved, the
card read back with its PIN; `oracle/scripts/popn14-eamuse-on.his`, `popn14-card-new.his`,
`popn14-card-again.his`). Its program is RAM-only too (0x1730000-0x1820000, as pop'n 13's). The
e-AMUSEMENT PASS replaces the magnetic card: an IC card that holds only its id, read by a slotted
reader with its own 10-key; the PIN is checked by the server.
- **The IC card reader** (Konami's slotted "ICCA", on the interface board's serial port at 19200
  baud; the driver is the game's own, `0x17f7f30`; HIRAKU's is `runtime/python/icca.cpp`):
  - The game sends `aa` and wants `aa` back; then packets `aa 00 HI LO 00 LEN payload SUM` (SUM: the
    8-bit sum of HI..payload), answered `aa 81 HI LO 00 LEN payload SUM` (the sum of all after the
    `aa`). Full duplex: no echo. **ACIO escaping:** after the `aa`, a byte `aa` or `ff` goes as `ff`
    and the byte inverted; the game undoes it, so an answer whose card id holds `ff` must be sent so,
    or it comes up short and the reads time out.
  - Commands: `00 03`, `01 00`, `01 30` (start-up, each answered with one byte 00); `01 34` the state;
    `01 35` the slot, payload `18 MODE` (11 open: take a card in; 12 eject; 00 close); `01 39` read
    the card; `01 10` the 10-key.
  - The state, 0x18 bytes: 0 the status (2 ready; the game requires 2 at start-up; 4 after a read
    with no card), 1 the sensors (0x10 front: a card at the slot; 0x20 rear: a card in), 2-9 the
    card's id. The game takes a card when it has seen both sensors on for more than 4 polls
    (`0x173bbd0`), reads it, and ejects it when the credit ends.
  - The 10-key answer is 4 bytes: 0, 0, then the keys held, bits 0-3 of the third byte blank 3 6 9,
    the fourth byte 0 1 4 7 00 2 5 8 (the game makes digit d its bit d, `0x17f93d8`).
  - Without the reader: "IC CARD R/W UNIT ERROR" 5-1508-000 at start-up; a reader that stops
    answering mid-game: 5-1500-000.
- **`facility.get`** at start-up, after `posevent`; without a `facility` service, 5-2009-0000. The
  answer is groups of text elements (`0x17d6860`): `<facility status="0" expire="600"><location><id/>
  <country/><region/><line/></location><portfw><globalip/><globalport/><privateport/></portfw>
  <public><name/><flag/></public></facility>`.
- **A card's life:** `cardmng.inquire cardtype="1" cardid="E004..."` (the card's id is the
  e-AMUSEMENT card number, as on later games and in spice2x). The answer needs all of `refid`,
  `binded`, `expired` and `newflag` (`0x17d51b0`). Then the PIN on the reader's 10-key (four equal
  digits are refused by the game) and `cardmng.authpass refid pass`: status 0, or **116 for a wrong
  PIN** (`0x17ac2c8`). A new player: `bindmodel refid newflag`, a choice between "はじめてあそぶ"
  and carrying data over from pop'n 13 ("カーニバルからデータを引きつぐ", `playerdata conv`), the
  name, then `F14 playerdata new ref_id card_id name ea_passwd`. HIRAKU keeps a card's PIN from its
  first `authpass` (or `getrefid cardtype cardid newflag passwd`, which answers a `refid`). Also
  seen in the program: `bindcard cardtype newid refid`, `playerdata expire`.
- **The record:** compressed as pop'n 13's (`f_compress="1"`, the same LZSS, `0x17fd3d8`). `get`
  answers 0xdb8 bytes (at `0x16506b8`); `new` is read as 0xd6c, and a pop'n 13 player's record
  (0xd6c, `conv`) too. 0x00 the name, 0x1c the option and 0x1e the character (16-bit), from 0x24 a
  32-bit word per song (`0x17fdfa0`); no password.
- **`F14 game get`** also reads `net_event`, `ir_ranking` and `shop_event_ranking` (`0x17e9308`).
  `net_event` 1 means the internet ranking runs (`0x17fdc78`): the attract mode then shows the course
  boards, and courses' results and high scores go online; 2 means NET TAISEN runs instead (below,
  "NET TAISEN"). HIRAKU answers 2, or what the data folder's `net_event-F14` says.
- **`F14 rank get`:** 13 EXPERT courses of 0x1e4 bytes as pop'n 12's (the prefecture before the
  place), a 0x1894-byte table (`0x17db420`, at `0x15f28f8`), and no shop board.
- **`F14 rank exp`** sends `ref_id course_num chara_num option stage_score cool great good bad combo
  shop_name pref`: no name and no score. HIRAKU ranks the player by ref id, scores the course as the
  four stages' scores added up, and takes the name from the player's record (`pdata-F14-REF`).
- **`F14 rank shop`** sends `stage item robo shop_name pref` after a credit (the SHOP EVENT's story,
  its counter, FEVER ROBO's gauge): `<F14/>`; HIRAKU keeps the latest per shop.
- **Not done yet:** `expire`, `bindcard`, `cardreis.altercard` (`playerdata conv`: below, "One
  card for all").

**pop'n music 15 ADVENTURE** (`G15:J:A:A:2007091800`, a Windows PC: `popn15.exe` through hiraku-pc,
PLAN.md §30; checked in HIRAKU under Wine: a new player registered and a credit saved, the card
read back, a pop'n 14 player carried over, an expert course ranked, two cabinets in NET TAISEN). Addresses are `popn15.exe`'s. Its network library is
`libavs-win32` (ea3), which names every field it misses on its debug output (`W:psmap`, `apply:
cannot get url`).
- **Start-up:** services must also list `pcbevent` and a `keepalive` (router, center and server it
  pings); `facility.get` must give every field the library imports, with types (`<portfw><globalip
  __type="ip4">`, `<globalport __type="u16">`, `<public><flag __type="u8">`, `<share>...`), or
  5-2009. Then `pcbevent put`, `package list`, `G15 game_get`. hiraku-pc gives the game a network
  card of its own and answers its pings (runtime/pc/net.c).
- **The module** is the game's model, its methods `type_method` (a table at `0x568700`: builder and
  parser each): `game_get`, `game_active`, `playerdata_get/new/set/score/conv/expire`,
  `rank_exp-entry`, `rank_exp-get`. Every `<b>` answer has `f_compress="1"` (the LZSS of pop'n 13,
  `0x4b7a10`), required by the parser even for small data.
- **`G15 game_get`** (`0x49dcb0`, into `0xc4d3a8`): `phase` (0-2, the cabinet's own unless the
  server's is higher, `0x427130`), `net_event` (0-3, NET TAISEN), `ir_ranking` (`0x427110`: 1
  INTERNET RANKING, the expert courses' boards and the stage scores to beat; 2 the ADVENTURE map,
  "kojin_map", playerdata's `private`; 4 and 5 other ranking periods), `shop_event_ranking`,
  `message`, and NET TAISEN's `local_matching_enable matching_sec ojama_gauge_rate cpu_level`
  (10) `downlimit_win downlimit_class job_exp_rate cpu_higher_release job_sp_rate` (26), the rates
  in percent (below, "NET TAISEN"). HIRAKU answers `ir_ranking` 2 and `net_event` 1, or the data
  folder's `ir_ranking-G15`, `net_event-G15`, `phase-G15`.
- **A card's life** (the card flow `0x485e80`-`0x488d60`): `cardmng.inquire` (`binded`: the card
  has a pop'n 15 player) and `authpass`, `playerdata_expire`; a card with no player then gets
  `inquire model="F14:J:A:A"` and, if `binded` 1 and `expired` 0 there, `playerdata_get
  model="F14"`: pop'n 14's record (0xdb8). The game shows it ("このデータを引き継ぎますか?", the
  medals counted from both records' song words by the same index, `0x482d70`) and on YES sends
  `playerdata_conv ref_id card_id name`, whose answer is the new pop'n 15 record; else the name
  entry and `playerdata_new ref_id card_id name ea_passwd`. `bindmodel`, then `inquire update="1"`
  and `game_active`. Status 109 to a get: no player.
- **The record**, 0xb38 bytes (get, new and conv answers; decoded at `0x49e6c0`, loaded and
  bounded at `0x4819f0` into `0x238bc08`): 0x00 the name (Shift-JIS, 13), 0x1a the mode, 0x1b the
  buttons (-1 none), 0x1c the option (32 bits), 0x20 the character (0xffff none), 0x22 the last song
  chosen, 0x24 its chart, 0x25 its category (0xffff/0xff none), 0x27 `skin_flag`, 0x28 `skin_count`
  (7), 0x30 the 20 most played songs (a music select folder; 0xffff none), 0x58 a 16-bit word per
  song (763): pop'n 12's two-bit marks per chart in the low byte, played flags 0x100-0x4000;
  0x650 and 0x790 per expert course (20) its four stages' scores, the player's best and the scores
  to beat (`0x4266ce`); 0x8d0 a word per expert course (`expert_data`); 0x8f8 the ADVENTURE map
  (`count record_time record_count`, `mapinfo` 8, `event` 32 bytes at 0x924, `point` 0x944,
  `element` 0x946, `position` 0x947, as `0x476940` loads it); 0x948 the NET TAISEN credits played
  (not sent: the server counts them; the first ones explain more, `0x459e2f`); 0x94a-0xb37 NET
  TAISEN (as pop'n 14's fields, other places: below).
- **`G15 playerdata_set`** sends what the credit changed (`0x49ef10`, from the block `0x5dab20` the
  game fills from the record and the credit): `option chara_num mode_num music_num sheet_num
  category_num botton_num skin_flag skin_count expert_course_num expert_data`, a `<music music_num
  sheet_num data score/>` per song played (`data` the song's new word), `<private .../>` (the map)
  and `<netvs .../>`. HIRAKU writes each into the record where the loader reads it, keeps each
  chart's best score (`pscore-G15-REF`) and the plays per song (`pplays-G15-REF`, for 0x30).
- **`G15 playerdata_score`** `music_num sheet_num s_challenge`: `<G15 score/>`, the chart's best
  (shared with pop'n 12-14 as theirs are).
- **`G15 rank_exp-entry`** `ref_id course_num chara_num option stage_score cool great good bad combo
  shop_name pref`: `update entry rank hi_score hi_cool`, as pop'n 14's. **`rank_exp-get`** (no
  attributes): the boards, 22 courses of 0x1e4 bytes as pop'n 14's (`0x427e40`, 0x2998 bytes at
  `0xc4ead8`), no shop board, LZSS'd whole.

**pop'n music 16 PARTY♪, 17 THE MOVIE, 18 せんごく列伝** (`H16`, `I17`, `J39`, Windows PCs through
hiraku-pc, PLAN.md §30 PC-6; checked in HIRAKU under Wine). Addresses are the newest programs'
(16: 2008052700, 17: 2009022500, 18: 2010040500). Start-up as pop'n 15's; 17 and 18's libavs
sends binary XML (kbin) and wants it back (`runtime/python/kbin.h`); `ea3_query` is in
`libavs-win32-ea3.dll` from 17 on.
- **The modules** are `game` (`get`, `active`) and `playerdata` (`get`, `new`, `set`, `expire`,
  `friend`, `conversion`; 16 and 17 also `score`, 17's a stub; a table of name, builder and
  parser: 16 `0x6af030`, 17 `0x659f48`, 18 `0x6a6858`). 16 and 17 pack `<b>` (`f_compress="1"`,
  pop'n 13's LZSS) and 17 crashes on `<b>` as text: it must be `__type="bin"`. 18's `<b>` is
  plain, with `<hiscore __type="bin">` beside it.
- **`game get`**: 16 as pop'n 15's NET TAISEN set (`netvs_phase`, `matching_sec`, `cpu_level`...)
  and phases; 17 `game_phase e_amuse_phase local_matching_enable matching_sec`; 18 also `ir_phase
  event_phase netvs_phase license_phase psp_phase psp_sale_month psp_sale_day boss_diff
  boss_battle_point`. HIRAKU's values are guesses, each the data folder's `NAME-GAME` if there is
  one. What each game takes (outside these, the phase counts as 0), from its accessors:

  | Game | Phase: range (HIRAKU's), what is known |
  |---|---|
  | 16 (`0xcf47ac`, `0x44531c`-`0x445490`) | `phase` 0-2 (the cabinet's), `netvs_phase` 0-7 (1; NET TAISEN from 1), `ir_phase` 0-3 (0), `private_phase` 0-3 (1), `normal_secret_phase` 0-5 (1), `all_hiscore_phase` 0-3 (1). Some are tested for one value (`private_phase` 1 and 2 each turn something on), so the highest isn't simply the most |
  | 17 | `game_phase` (2), `e_amuse_phase` 0-15 (14; NET TAISEN from 4; its tables run to 14) |
  | 18 (`0xfcb508`, `0x45f190`-`0x45f360`) | `game_phase` 0-2 (2; the higher of it and the cabinet's), `ir_phase` 0-13 (0; the INTERNET RANKING runs in 1-5 and 7-12), `event_phase` 0-6 (1; the event runs in 1-5), `netvs_phase` 0-13 (1; NET TAISEN from 1, something more in 3, 6, 8, 10, 12), `license_phase` 0-3 (1), `psp_phase` 0-6 (0) |
- **A card's life**: `cardmng inquire` (`binded`: the card has this game's player), `authpass`,
  `playerdata expire`; with none, `inquire model="<the game before>"` (16: G15, 17: H16, 18: I17)
  and `playerdata get model=...`: that game's record (16 reads pop'n 15's 0xb38 whole, 17 the first
  0x6b0 bytes of 16's packed, 18 the first 0x72c of 17's, unpacked: `0x45490a`), then
  `conversion ref_id card_id name` (the answer is this game's new player, as `get`'s) or the name
  entry and `new`. Status 109: no player.
- **The record** (16: 0x6784 bytes, 17: 0x74bc, 18: 0x1700) is copied to a global and bounded field
  by field (16: `0x447dc0` into `0xf313b8`, 17: `0x440f30` into `0x1228988`, 18: `0x460320` into
  `0x10fc930`), pop'n 15's layout grown: the name (13) and two more names, the mode and buttons
  (0x1a, 0x1b), the option (32 bits; 16: 0x1c, 17: 0x20, 18: 0x34), then the character (16 bits),
  the last song chosen (16), its chart and category; the 20 most played songs (16: 0x28, 17: 0x2c,
  18: 0x40; 0xffff none); a 16-bit word per song, clear marks low and played flags 0x800 << chart
  (16: 0x50, 839 songs; 17: 0x54, 907; 18: 0x68, 976); expert courses' stage scores and clears;
  NET TAISEN's fields. 16 and 17 keep each chart's best score in the record, 32 bits, chart-major
  (16: 0x8dc, 7 charts × 846; 17: 0xcd4, 7 × 950: `0x443c92`); 18 in `<hiscore>`, 17 bits per song
  and chart (index song × 7 + chart, least significant bit first: `0x456570`). 18's last 0x128
  bytes (0x15d8) are its story map (`private`), copied to `0x10af5a8`. The game does not change
  its copy during a credit: what it played comes back only through `set`.
- **`playerdata set`** (16 `0x435f30`, 17 `0x436180`, 18 `0x454bf0`, from a block the game fills
  through the credit: 17's `0x658800`, 18's `0x6a4f50`): `option chara_num music_num sheet_num
  category_num` (16 also `mode_num botton_num`, 17 and 18 `play_mode`), a `<music music_num
  sheet_num data score/>` per song played (`data` the song's new word; 17 adds `play_count`, 18
  `select_count`), `<private>`, `<netvs>`, 18's `<license>` and `<boss>`. A credit played without a
  card sends `set` with no `ref_id`. HIRAKU keeps the fields above in the record, each chart's best
  score (16, 17 in the record; 18 `pscore-J39-REF`, the `<hiscore>` bits), and the plays per song
  (`pplays-GAME-REF`, for the most played), `<netvs>` (NET TAISEN, below), and, where each
  game's loader reads them:

  | Field | Where (and what the game sends) |
  |---|---|
  | `play_mode` (17, 18) | the mode and how it went (17 `0x442bba`, 18 `0x461691`): the record's 0x1a keeps the mode, where the mode select starts (17 `0x49ad1e`, 18 `0x4c383f`). 17's modes 0-5 are `play_mode` 0-1, 2-3 and 11, 5, 6 (the expert courses), 7-10 and 13 (NET TAISEN), 4 and 12; 18's 0-4 are 0-1, 2-3 and 10, 5, 6-9 and 12 (NET TAISEN), 4 and 11 |
  | `last_play_flag` (17, 18) | 0x1c (17 bits 0-1, 18 bits 0-6), the record's own with the credit's bits |
  | `extra_ticket` (16, 17) | 16 0x26, 17 0x2a (at most 2; 17 `0x4716b0`) |
  | `expert_course_num expert_data` (16, 17) | the course's clear (0-4), 16-bit words: 16 11 courses from 0x83c, 17 19 from 0x990; the game sends the better of the record's and the credit's (17 `0x442a20`) |
  | `expert_random_course_num _clear _combo` (16, 17) | a random course by its level (4-42): how many times cleared and full-comboed (at most 99), bytes: 16 0x852 and 0x87d, 17 0x9b6 and 0x9e1 |
  | 16's `<private>` (PARTY) | `guest_count_0..4`: bytes 0x6564..0x6568 (the game sends the record's when more); `secret`: 32 bits at 0x656c (the record's OR'd in), `0x448ef0`. `zoom`, `seed`, the invite types and `ticket_point` are the credit's only |
  | 17's `<private>` | the record's 0xc00-0xcd4, which the game keeps at `0x1209aec` through the credit (`0x451d20`) and sends from (`0x452dc9`): `secret` 0xc00 (32 bits), `main_chara` 0xc04 (3 words), `sub_chara` 0xc0a (10), `latest_main_0..4` 0xc1e + 6k (3 each), `latest_sub_0..4` 0xc3c + 0x14k (10 each), `point` 0xcba, `ticket_point` 0xcbb (5 bytes), `target` 0xcc0, `latest_target` 0xcc1 (5 bytes) |
  | 18's `<private>` | its story map, the record's last 0x128 bytes (0x15d8), copied to `0x10af5a8`: two character lists newest first, which the server moves along as the game does (`0x473cb0`) |
  | 18's `<license license_num sub_license_num clear data1..4>` | 13 licenses of 11 parts, each a clear word and four data words (0x14 bytes; a license 0xdc), from 0x89c up to NET TAISEN's block; the game copies them to its table `0x105b244` (`0x470640`) and sends the parts the credit changed (`0x461862`) |
  | 18's `norma_point` | 0x3e (`0x4963c0`) |

  Not kept: `room_num`, `matching`, `medal_and_friend` (the credit's own counts, as pop'n 15's
  statistics), 18's `<boss boss_num win/>` (a NET TAISEN battle against the event's boss, with
  `game get boss_diff boss_battle_point`: a shop or server tally, not the player's).
- **16's `playerdata score`** `music_num sheet_num s_challenge`: `score nr_active_friend
  friend_score_N` (`0x435d70`): the card's best (below, "Shared scores").
- **The rankings** (module `ranking`): 16 and 17 send the expert courses' results as pop'n 15's
  `rank_exp`: `ranking entry ref_id course_num`, the four stages' scores (16 `stage_score
  "a,b,c,d"`, 17 `stage_score_1..4`), `cool great good bad combo pref` (16 also each stage's
  `chara_num` and `option`), answered `update entry rank hi_score hi_cool`; `ranking get` takes the
  boards, 11 of pop'n 15's 0x1e4 bytes (0x14cc, packed: `f_compress` and `<b>`; 16 `0x4374c0`, 17
  `0x437ad0`). No name is sent: the server's board takes the record's. 18 sends its INTERNET
  RANKING's results (`ranking entry ref_id ir_num sheet_num chara_num option score cool norma_1
  norma_2 pref`) and gets no boards. HIRAKU keeps a board per course (`H16-rank-exp`,
  `I17-rank-exp`) and per `ir_num` (`J39-rank-ir`). Not filled yet: the records' stage scores
  (16: 11 courses' own from 0x6ec and 10 best players' from 0x79c; 17: 19 from 0x7c0 and 10 from
  0x8f0), which courses they follow not being seen (pop'n 15 fills its own from the boards).

## One card for all (pop'n music 9–18)

Each pop'n carries a player over from the one before: it reads the older game's card, asks the
server for that player, offers to carry the name and clears over ("引き継ぎ"), and then wants a
card of its own (pop'n 14: an e-AMUSEMENT PASS and pop'n 13's transfer number). In HIRAKU one card
number (`-card0`, `card0.txt`, the launcher's Player 1 Card) is used in every game, and the player
follows it from game to game: 9 → 10 → 11 → 12 → 13 → 14, each step checked in HIRAKU with the
same number, and on to the PC games, 14 → 15 → 16 → 17 → 18 (their own sections below).

- **One server folder:** with the saves in their default places (the game window's,
  `~/.local/share/hiraku/<game>`, or a portable install's `user/data/<game>`), the pop'n games share
  one e-AMUSEMENT folder beside them, `eamuse/`. A game's own `eamuse/` from before is merged into
  it once and kept renamed (`eamuse.moved`). With `--nvram` elsewhere a game keeps its own folder,
  as before (and regress runs are unchanged).
- **The older card:** a numbered card a game has never seen (no `cards/NUMBER.bin` in its save) is
  shown as the game before's card, as that game left it in its save (`popn9`'s for pop'n 10, ...).
  pop'n 10 rewrites it as its own. pop'n 11-13 read it, then give it back and ask for an unused
  card of their own: once the player has agreed (the game asked the server about the player and
  the old PIN was typed), the card given back is followed by a fresh one of this game with the
  same number, which the game then formats. Refused, the older card stays for another try. pop'n
  14's card is only its number.
- **What each game asks, and what the server answers:**

  | Game | Asks | Answer |
  |---|---|---|
  | 10 | `userdata.read card="NUMBER:C00"` | pop'n 9's `userdata`, as saved (the game converts it) |
  | 11 | `D11 renewc ask`, `userdata.read card="NUMBER:C20"` | pop'n 10's `userdata` |
  | 12 | `E12 playerdata get model="D11"` | pop'n 11's `userdata` `<b>` pieces (the game appends them, `0x191910`) |
  | 13 | `E13 playerdata get model="E12"` | pop'n 12's record, `f_compress="0"` (converted by the game, `0x17e9dd8`) |
  | 14 | `F14 playerdata get ref_id="POP'N ID" model="E13"` | pop'n 13's record, compressed |

  pop'n 14 asks by the transfer number the player typed; the core adds the card's number, and the
  server answers that card's pop'n 13 player whatever was typed (it issues no transfer numbers).
- **`playerdata conv`** follows the player's yes. The games convert the older record in memory
  and their credit's `set` sends only the songs played, so the server builds each new record as
  the game does (checked against the game's own result in RAM):
  - **12** (`conv name new_card_id old_card_id`): from pop'n 11's 0xa28 bytes, the 600 song words
    at 0x410 into the record's words from 0x3c, the song numbers alike, bits 4-5 and 6-7 swapped and
    the high byte dropped (`0x1a0a38`); the PIN from 0x9f3 (a digit a byte; the name follows at
    0x9f7); the name as sent.
  - **13** (`conv name ref_id old_card_id new_card_id`): pop'n 12's song words' low byte (the four
    charts' clear marks, two bits each, the same in both: `0x1812f98`, `0x1812c4c`) as pop'n 13's
    32-bit words from 0x28; pop'n 12's flags above it are left out. The PIN and the name.
  - **14** (`conv name ref_id card_id popn_id ea_passwd`): the game takes the answer's record as
    its own, byte for byte. pop'n 13's record as it is would put song n at pop'n 14's n+1 (14's song
    words start at 0x24, `0x17fdfa0`; 13's at 0x28) and 13's PIN where 14 keeps its option and
    character, though both games number their songs alike (below). So the answer is the record in
    pop'n 14's layout: the name, 13's option and character (0x20, 0x22) at 0x1c, 0x1e, and each
    song's word at 0x24 + 4n; the server keeps the same.
- **Clear marks and scores are shared** (below); each game keeps its own player otherwise: options
  and a name changed in one game stay there. Characters and options: pop'n 14 takes pop'n 13's,
  the others start from the game's defaults. ee'MALL tickets don't carry over (the games say so).

### Shared clear marks

- **One song numbering:** each game's song table (entries with pointers to a genre name and a
  comment) has the same song at the same index in all six games (pop'n 9 and 10: 325 and 390
  entries; 11: 450; 12: 595; 13: 649; 14: 700), the differences being spelling only (kana or
  kanji, "(PS)"-style tags) and "-" entries left where a song was taken out (9's 252 and 257;
  12-14's 244-246, 252, 254, 257, 260, 262-263, 328-329, 337-338, 392; 11-14's 428). An
  entry's words at 8 and 0xc are the song's ids, the same in 11, 12 and 13; pop'n 14 gave 18
  songs new ones (450-492; ten of them rated anew, so charted anew presumably) and rewrote the
  genres and comments of the songs from other games (13's "SURGE LINE" is 14's ギターポップ).
  The songs sent in play agree (pop'n 14's 393 is アンパンマン, on screen and in its table;
  pop'n 12's 542 ドラゴンボールＺ).
- **The marks:** two bits a chart, 1 a clear and 2-3 the better medals, in the low byte of a song's
  word: pop'n 12, 13, 14 in one order (bits 0-1, 2-3, 4-5, 6-7), pop'n 10 and 11 with the last
  two charts the other way round. pop'n 9 keeps a cleared bit a chart, byte 0x416 + song, bits 0-3
  in pop'n 12's order. Found from the games' own conversions, with patterned players:
  - pop'n 9 -> 10: 9's byte 0x416 + n bits 0-3 become 10's flags 0x100, 0x200, 0x800, 0x400 of
    word n (10's song words: 600 at 0x3fc); 9's bits 4-7 and its 0x8-0x404 (scores, presumably)
    aren't carried;
  - pop'n 10 -> 11: the low byte as it is (11's words: 600 at 0x410), 10's flags 0x100-0x800
    become marks of 1 at bits 0, 2, 4, 6; songs past 10's table (390) left out;
  - 11 -> 12, 12 -> 13, 13 -> 14: above.
- **The server** gives each game, when it reads its own player (pop'n 9-11 `userdata.read`, 12-14
  `playerdata get`), every chart's best mark over all the games the card has played (their saved
  players: `userdata-*`, `pdata-*`, pop'n 13's and 14's by the card's ref id), in that game's
  format, for the songs that game has; a game's own better mark and its other bits stay. The game
  then saves them with its own at the end of a credit (pop'n 9-11 save the whole player). Checked:
  marks put in pop'n 9, 10, 11 and 13 reached all six games (in RAM for 12-14), and pop'n 9, 10 and
  11 played a credit with them and saved them.
- **pop'n 15-18** keep a 16-bit word a song with the marks in its low byte as pop'n 12-14 (16 and
  18 set a chart's mark from the same table of values, 1-3 shifted by two bits a chart: 16
  `0x448cf5`, 18 `0x4613fa`), and number their songs as 14. Their song tables (in the program:
  15 `0x5084f0`, 0x28 bytes an entry, 763 songs; 16 `0x523ab8`, 0x28, 839; 17 `0x52d028`, 0x34,
  907; 18 `0x55adc0`, 0x48, 976, in each one's last program) have the same song at the same index,
  differing in spelling only, and flag a song taken out with 0x80000000 (15 had already taken out
  some songs 14 has, its entries' text left in place; 16-18 blank theirs). The server gives 15-18
  the card's marks in their `playerdata get` as it does 12-14. Checked: pop'n 18 holding pop'n 15's
  gold medal on song 0 in its copy of the player.
- **Not shared:** pop'n 13's and 14's other flags in a song's word (0x100-0x4000), and 15-18's.

### Shared scores

- **Where scores are:** pop'n 12-14 send each song played (`<music num sheet data score/>` in
  `playerdata set`) and ask a chart's best before playing it (`playerdata score music_num
  sheet_num`); the server keeps them (`pscore-GAME-PLAYER`). pop'n 9-11 keep no scores: their
  players (1.6-2.6 KB) hold the marks, and no call of theirs carries a song's score.
- **The sheet is the chart,** alike in the three (the same code: `0x1b3958`, `0x1810e60`,
  `0x17fdfa0`), and its levels' place in the song table: 0 Normal, 1 Hyper, 2 5 buttons,
  3 EX, 4-5 battle; 5 one mode's (the game's mode 2 with an option's 0x2000). Charts 0-3 are the
  marks' two-bit places. pop'n 13 and 14 play their own 5-button charts as 6 (marked by flag
  0x200 alone; 7 the same with 0x100), not pop'n 12's 2. Played: GRADIUS (418) on 5 buttons is
  313 notes in pop'n 12 (sheet 2) and 167 in both 13 and 14 (sheet 6); 9 buttons' Normal is
  sheet 0 (pop'n 12; pop'n 13 in NET TAISEN).
- **The server** answers `score` with the best of the card's pop'n 12-18 scores (pop'n 12 by card,
  the others by the card's ref id) for that song and sheet; another game's only for songs both
  have, and pop'n 14's (and 15-18's) own for the 18 songs 14 gave new ids. Checked: pop'n 12 asking
  GRADIUS's sheet 0 got pop'n 13's 77777.
- **pop'n 15-18** number their charts alike, seven a song (each one's song table keeps the
  levels by sheet at the same place: 15 `0x4759ef`, 18 `0x470f08`, the entry's level bytes in the
  same order). pop'n 15 asks a chart's best as 12-14 do (`playerdata_score`), and 16 too
  (`playerdata score`); 16-18 also take all of a player's bests in `playerdata get`: 16 and 17 in
  their records (32 bits a chart, chart-major: 16 at 0x8dc, 846 songs a chart; 17 at 0xcd4, 950),
  18 in `<hiscore>` (17 bits a chart, seven a song).
  The server puts the card's best from its other games into those, for that answer only: the
  record kept is the game's own. Many charts' levels changed from one game to the next (Konami
  re-rated them; the charts the same) and aren't taken as new charts. Checked: pop'n 18 holding
  pop'n 15's 88888 as song 0's Normal best.

## NET TAISEN (pop'n music 12–14)

Battles between cabinets, three players to a match (the battle's three score slots), CPUs in the
empty seats. pop'n 12, 13 and 14 carry
the same library (the strings `globby://`, `LobbyEntry()`, `>>NETVS MASTER/SLAVE/LOCALVS`); the
addresses here are pop'n 12's. In HIRAKU the mode is offered and played: alone, when nobody
comes, the game says so and starts the credit in CHALLENGE mode (`oracle/scripts/popn12-nettaisen.his`);
two HIRAKU cabinets on one LAN cable (`--lan-host`/`--lan-join`, `runtime/python/lancable.h`)
meet through the first one's lobby and battle, a CPU in the third seat
(`oracle/scripts/popn12-nettaisen-a.his`/`-b.his`, regress `popn12-nettaisen-a`/`-b`); cabinets on
different PCs meet online through `hiraku-eamuse`'s relay (below, "Online").

- **When the mode is offered** (`0x19a300`, the mode wheel's NET対戦 and the title's `ICON_NET`):
  the event phase 2 or later (NET TAISEN MODE RELEASE), a card in, and on the network all of:
  - `E12 game get` answered with `play_enable` 1 (`0x191160`, read into `0x2d4748`: `phase`,
    `message`, `renew_f`, `xmas_f`, `play_enable`, `local_matching_enable`, `matching_sec`,
    `room_num` (up to ten, comma-separated), `ojama_msec`, `ojama_gauge`, `cpu_level`);
  - the services list's `globby` item, a URL `globby://HOST:PORT/` (`0x199400`: a dotted address or
    a name for DNS);
  - **`portfw get`** (`privateip`) answered `<portfw globalip="A.B.C.D" port="N"/>`
    (`0x190520`): where the world reaches the cabinet's battle socket. The services list must name
    `portfw`, as any module a call uses (else the library fails the call at once, -10, `0x184ed0`).
    Status 122 or 123: the mode stays off.
  HIRAKU answers `play_enable` 1, `local_matching_enable` 1, `matching_sec` 30, every room 0, and
  leaves out the rest (the game's own: an ojama lasts 1000 ms at least, `0x126f38`); `globalip` is
  the board's own address, `port` 5700, the lobby at the server's host, UDP 5740 (HIRAKU's choice).
- **The credit:** NET対戦, an explainer (red skips), ROOM SELECT (the player's level rooms; a room
  is `1 << room_num[i]` in the lobby), then song select with `matching_sec` to find opponents
  ("受付中"), then the battle, or CHALLENGE mode if nobody came.
- **The lobby** (UDP, from the cabinet's port 3200, one answer awaited per datagram, three tries,
  `0x199860`): a 12-byte header, `lob0`, the length (big-endian, all of it), a CRC-16/X-25 of the
  whole datagram taken with the field at 0 (`0x1adfd0`), a sequence number, the kind, a 0. The
  answer repeats the sequence number and has kind 0 (else "Replay type mismatched", `0x199d98`).
  - **list** (kind 1, 0x10 bytes): the room mask. Answered with the cabinets in its rooms, 0x1c
    bytes each from offset 0xc: the room mask, the address, the port (big-endian), the cabinet's 16
    bytes (below), two zeros; a zero entry ends it (`0x19d380`; at most 0x1bf0 bytes). The cabinet
    itself in the list is skipped; one other there and the cabinet joins it (`SLAVE`).
  - **entry** (kind 0, 0x28 bytes, `0x199ed8`): the room mask, `globalip`, `port`, how many seconds
    to keep it (`matching_sec` × 3 + 5), 16 bytes of the cabinet's own (its address on the LAN
    first). After an entry the cabinet waits for others to call (`MASTER`).
  - **leave** (kind 2, 0x14 bytes): the address and port, when the time runs out.
- **Between cabinets** (`0x19a628`, `0x19a948`): UDP from port 30000 to the other's port 5700
  (`globalip`'s; on one LAN, the 16 bytes' address), a 3-byte tag, `0`, the length, a CRC as the
  lobby's, a byte that turns a 16-byte XOR key (`0x2ef8c0`) over the rest (`0x19ab20`). Six kinds,
  0x18 to 0x1cc bytes (`0x19adc8`); the game's log names the players, standby, the countdown,
  ojama and dialogs among them.
- **On one shop's LAN** (`LOCALVS`, `0x19fdc0`, `0x19f9e0`): after a new card's OK the cabinet
  broadcasts a 0x1c-byte datagram to port 5700 (its own address and netmask: board commands 7 and
  8); a cabinet that has heard one shows a screen of its own first (`0x1644c8`, not read yet)
  and, for a local battle, calls those directly, no lobby.
- **Two cabinets, as seen in HIRAKU** (`popn12-nettaisen-a`/`-b`): each hears the other's
  broadcast at its card's OK, and from then on they exchange datagrams port 30000 to 5700 a few
  times a second. In NET対戦, A (in song select first) finds the lobby empty, enters and waits; B
  lists, finds A and calls it (SLAVE). Both leave song select together; the opponents' screen
  (対戦者紹介) shows both players and a CPU, the song starts on both at the same frame, "通信中…"
  while the scores go round, and each RESULT has all three scores. Each cabinet shows the others'
  scores as they arrive, so they can differ a little between the screens (the CPU's 2885 and 2818
  at one moment).
- **Three and four cabinets, as seen in HIRAKU:** three meet as two do (B and C each list, find A
  and call it) and play with no CPU; the three RESULTs agree. A full match takes its master out of
  the lobby, so a fourth cabinet finds the lobby empty, enters it and waits for a match of its own:
  with nobody else, CHALLENGE mode.
- **Receive timeouts:** the battle's sockets set one (`setsockopt` level ffff, option 0x200,
  seconds and microseconds): the lobby's 1 s a try and 1 ms to empty its queue, another 50 ms. A
  recv waits for a datagram that long; answered at once instead, a lobby request from another
  cabinet (one frame away) went out again before its answer came.
- **pop'n 13 and 14** (checked in HIRAKU: one cabinet, and two on a LAN cable, regress
  `popn13-nettaisen-a`/`-b`, `popn14-nettaisen-a`/`-b`):
  - **When the mode is offered:** no event phase; `game get`'s `net_event` must be 2 (13's
    `0x17edd48`, 14's `0x17ddef8`), with the lobby's URL parsed and `game get` answered. `net_event`
    is the network event running, one at a time: 1 the INTERNET RANKING (every use asks for exactly
    1: the IR icon, the attract mode's course boards, course results online), 2 NET TAISEN. HIRAKU
    answers 2 unless the data folder's `net_event-E13` or `net_event-F14` holds another number.
  - **`game get`'s fields:** 13 (`0x17f8bf0`, read into `0x190bc70`): `phase extra_event net_event
    renew_f id_announce_f message local_matching_enable matching_sec room_num ojama_msec ojama_gauge
    cpu_level`. 14 (`0x17e9308`, at `0x19033a0`): `phase net_event ir_ranking shop_event_ranking
    message local_matching_enable matching_sec ojama_gauge_rate cpu_level` (up to ten)
    `downlimit_win downlimit_class job_exp_rate cpu_higher_release job_sp_rate` (up to 18).
  - **Where the cabinet is reached:** 13 asks `portfw get` as 12 does; 14 reads it from `facility
    get`'s `<portfw><globalip/><globalport/>`. The core puts the cabinet's own address there (on a
    LAN cable, 192.168.50.2 + its number; online, the relay's), whatever the server said.
  - **The credit:** 13: NET対戦 one left of ENJOY on the mode wheel ("リアルタイム3人対戦"), a
    six-page explainer, ROOM SELECT, song select while the lobby looks, the opponents (対戦者紹介),
    the battle, RESULT. 14: one left of ENJOY too, an eight-page explainer, ROOM SELECT, a shop
    explainer, JOB SELECT, song select, MATCHING (the opponents, their jobs), the battle, RESULT.
    Alone, both say nobody came and start CHALLENGE mode.
  - **全国対戦 or 店内対戦:** a pop'n 13 cabinet that has heard another's broadcast asks first,
    nationwide (the lobby) or in-shop (the LAN, no lobby); left to its timer it takes nationwide.
- **Online** (`runtime/python/netrelay.h`): `hiraku-eamuse` keeps the lobby and a relay on TCP
  port 5740, the port its `globby` URL names. Each cabinet with `-url` pointing at it connects
  there at start, and the relay gives it an address of its own, 10.57.0.N. The cabinet puts that
  address in its `portfw` answer (port 5700), as a shop router's address would be, so the other
  games send there. The game's datagrams go over the connection with the address and port the
  game used: to the lobby's port at an address that isn't a cabinet's, the relay's lobby answers;
  to a cabinet's, the relay passes it on, from the sender's address; broadcasts go nowhere. The
  cabinets connect out, so no port forwarding is needed at the cabinets; only the server's 5740
  must be reachable. Not in lockstep: datagrams arrive when the network brings them, and the
  cabinets run free, as real ones did. Checked with two headless cabinets on one PC paced at real
  time ×4 (`HK_PY_PACE=4`): A waits in the lobby, B finds it and joins, a CPU takes the third
  seat, both RESULTs agree, stage 2 together. Against a server without the relay, the cabinet
  says so at start and NET TAISEN finds nobody.
- **After the credit:** `playerdata set` carries `<netvs music_num sheet_num caption fruit class
  class_skill badge get_ojama set_ojama rank stage_record member_name member_pref map_hi map_lo/>`
  (pop'n 13 and 14: their own fields). **The server puts them back into the player's record**, each
  where the game's own loader takes it from (it copies the record, then each field: pop'n 12
  `0x19ef40`, 13 `0x17f28d0`, 14 `0x17e29e8`; the set builders `0x191d28`, `0x17f7b04`, `0x17e84b4`
  send them in the same order), so the next credit's player has their class, badge, last battle and
  the rest. A list is that many values in a row; names are 13 bytes each, in the game's own text.

| game | field: record offset (× count, bytes each) |
|---|---|
| pop'n 12 (0x708) | map_hi 0x5f0 (4), map_lo 0x5f4 (4), stage_record 0x5fe (×3, 2), rank 0x604, member_name 0x60e (×3, 13), member_pref 0x683 (×3), class 0x68c, class_skill 0x68d, music_num 0x68e (2), sheet_num 0x690, set_ojama 0x691 (×3), get_ojama 0x694 (4), badge 0x698, caption 0x699, fruit 0x69a |
| pop'n 13 (0xd6c) | music_num 0xb4c (2), sheet_num 0xb4e, category_num 0xb4f, badge 0xb50, caption 0xb51, type 0xb52, class 0xb53, class_skill 0xb54, class_start 0xb55, record 0xb56, member_name 0xb6a (×3, 13), member_pref 0xbdf (×3), set_ojama 0xc01 (×3), get_ojama_hi 0xc04 (4), get_ojama_lo 0xc08 (4), trump_list 0xc4e (×52, 2), ojama_condition 0xcb6 (×48), ojama_cnt 0xce6 (×10) |
| pop'n 14 (0xdb8) | music_num 0xbdc (2), sheet_num 0xbde, category_num 0xbdf, badge 0xbe0, caption 0xbe1, job_num 0xbe2, class 0xbe3, class_skill 0xbe4, class_start 0xbe5, class_upgrade 0xbe6, record 0xbe7, member_name 0xbfb (×3, 13), member_pref 0xc70 (×3), ojama_condition 0xc79 (×51), job_var 0xd66 (2), job_exp 0xd68 (×9, 4), set_ojama 0xda5 (×3), get_ojama_hi 0xda8 (4), get_ojama_lo 0xdac (4), item_hi 0xdb0 (4), item_lo 0xdb4 (4) |

  pop'n 13 and 14 also send `reset`, which isn't in the record (the loaders don't read it back).

### pop'n music 15 (a PC game)

pop'n 15's NET TAISEN is the same design on Windows: `globbyc.dll` (the lobby) and `libcomm.dll`
(the cabinets' messages), both on `libsocket.dll`, a thin layer over Winsock (32 sockets by number,
four-byte addresses and a port, blocking or not with a timeout, no select). Checked in HIRAKU with
two cabinets (hiraku-pc, Wine) on one hiraku-eamuse: the second finds the first in the lobby, both
show MATCHING with each other and a CPU, and they battle.

- **The mode:** `G15 game_get net_event` 1-3 offers NET対戦 on the mode wheel with a card in
  (`0x448d23`); the number goes into the lobby entry. HIRAKU's server answers 1 (the data folder's
  `net_event-G15` to change it, 0 for none). Then ROOM SELECT, JOB SELECT, music select, and
  MATCHING.
- **The lobby** (globbyc): `globby://host:port/` from services, as pop'n 12-14; the same packets
  (`lob0`, the length, a CRC-16/X-25 with its own field 0, the sequence number, the kind), but an
  answer's header byte 11 counts its entries, which follow with no zero entry. Kinds: 0 register
  (an entry's body as pop'n 12's: rooms, address, port, seconds, the cabinet's own address and
  port), 1 read (a room mask: the entries), 2 erase (address, port), and 3 **participate**: the
  entry, answered with the others already in its rooms. With none, the game stays entered as the
  host for `matching_sec` seconds; with some, it asks one to join (`0x4a3440`). HIRAKU's lobby keeps
  pop'n 15's apart from pop'n 12-14's (port 5741, `Lobby::PC_PORT`).
- **The cabinets' messages** (libcomm, `CommInit("G15")`): UDP, from port 30000 to the other's 5700,
  each `"G15"`, a type, the length, a CRC, the sender's address and ports, a sequence number, the
  data: type 6 asks to join, 7 acknowledges, 5 the players' data, 4 every frame or so in the battle;
  type 1 a broadcast at start-up for cabinets of the same shop (port 2096). The game's own address
  is `avs_net_ctrl(0)` (the network card), its address for the world `ea3_query(8)` (facility's
  portfw `globalip`), and `matching_sec` must be more than 0 or a cabinet leaves the lobby at once.
- **game_get's battle settings:** `local_matching_enable`, `matching_sec` (seconds a cabinet waits
  in the lobby for others; 0 leaves at once), `ojama_gauge_rate` and `cpu_level` (×10, the CPUs'
  strength) in percent of the game's own, `downlimit_win` and `downlimit_class` (the wins that keep
  a class once reached, and how far below it a player may then fall: `0x48e5a2`, `0x48e633`), and
  `job_sp_rate` (×26, the 13 jobs' specials: the even ones each job's chance in percent at its top
  level, `0x42d0a0`, the game's own table `0x4ff058` lowering it at lower levels, 80-100 %; the odd
  ones amounts in percent, `0x42d200`, `0x48e7f1`). Konami's values went with its server. HIRAKU's
  follow what the game does with them and what players described of the mode
  ([pmdream's NET対戦 page](http://pmdream.net/popn/ac15/net.html): the specials fire "sometimes",
  the magician's ojama lasts longer, the warrior's shorter, the fighter's gauge fills faster; a
  player in a "降格のピンチ" who comes 3rd is demoted): `matching_sec` 120 (pop'n 19's, as
  bemaniutils records its server's answer), the rates 100, `downlimit_win` 0 and `downlimit_class`
  10 (no floor: players are demoted), and `job_sp_rate`
  `30,150,30,50,30,150,30,100,0,0,30,0,30,100,100,0,30,0,30,0,30,0,30,0,30,0` (chances 30 %; the
  magician ×1.5, the warrior ×0.5, the fighter ×1.5; the ninja's always on, as described). Each
  can be set in the data folder (`NAME-G15`, the attribute's text). `job_exp_rate`,
  `cpu_higher_release` and `shop_event_ranking` are read but never used.
- **In HIRAKU** (`runtime/pc/vsock.c`): libsocket's calls are served by the DLL over HIRAKU's relay
  (as for pop'n 12-14 online): one TCP connection, an address 10.57.x.y for the cabinet (given to
  the game as its card's and its portfw address); datagrams to the lobby go as they are, to another
  cabinet with a one-byte kind, and TCP connections as open/opened/data/closed messages (the
  library's `CommTcpHost`/`CommTcpClient`, which the game uses only in its developer test scenes
  "TCP HOST TEST"/"TCP CLIENT TEST", `0x420480`/`0x4206a0`: their scene factories, `0x41e7b0` and
  the next, are referred to nowhere in the program, so nothing a player does reaches them). `tests/pc/test_vsock.sh` checks all of it between two
  processes on one relay under Wine: the lobby, datagrams, a TCP connection both ways, a refused
  one, timeouts. Alone (no `-url`), hiraku-pc runs a
  relay and lobby of its own: the game waits `matching_sec` at CUSTOMIZE, says nobody came and
  starts the credit in CHALLENGE mode (checked).
- **After the credit:** `<netvs>` as pop'n 14's fields, the record's places from `0x4a4dc0`; and
  on `playerdata_set` itself `matching` (how many players the match had, `0x4a43b0`) and
  `room_num`, statistics the record doesn't keep. Checked: two cabinets' credits saved, each with
  the match's three members (the CPU's name too), class skill, job experience and record:

| game | field: record offset (× count, bytes each) |
|---|---|
| pop'n 15 (0xb38) | music_num 0x94a (2), sheet_num 0x94c, category_num 0x94d, badge 0x94e, caption 0x94f, job_num 0x950, class 0x951, class_skill 0x952, class_start 0x953, class_upgrade 0x954, record 0x955, member_name 0x969 (×3, 13), member_pref 0x9de (×3), ojama_condition 0x9e7 (×51), job_var 0xad6 (2), job_exp 0xad8 (×13, 4), set_ojama 0xb25 (×3), get_ojama_hi 0xb28 (4), get_ojama_lo 0xb2c (4), item_hi 0xb30 (4), item_lo 0xb34 (4) |
| pop'n 16 (0x6784) | the NET credits 0x6594 (2, the server's count; 0: the game starts the block with its defaults, `0x43c180`, and so does HIRAKU's server), music_num 0x6596 (2), sheet_num 0x6598, category_num 0x6599, badge 0x659a, title_select 0x659b, title_0..3 0x659c (×4, 4), job_num 0x65ac, class 0x65ad, class_skill 0x65ae, class_start 0x65af, class_upgrade 0x65b0, record 0x65b1, member_name 0x65c5 (×3, 13), member_pref 0x663a (×3), ojama_condition 0x6643 (×51), set_ojama 0x66ed (×3), job_var 0x6732 (2), job_exp 0x6734 (×9, 4), recommend 0x6771, get_ojama_hi 0x677c (4), get_ojama_lo 0x6780 (4) (as `0x43bf30` copies them to set's block) |
| pop'n 17 (0x74bc) | the NET credits 0xa40 (2, the server's count; 0: the game starts the block with its defaults, `0x43c7f0`, and so does HIRAKU's server), get_ojama_0/1 0xa0c, 0xa10 (4 each), get_item_0/1 0xa14, 0xa18, used_ojama_0/1 0xa1c, 0xa20, used_item_0/1 0xa24, 0xa28, get_title_0..3 0xa2c (×4, 4), exp 0xa3c (4), music_num 0xa42 (2), sheet_num 0xa94, category_num 0xa95, title_select 0xa96, room 0xa98, room_skill 0xa99, room_start 0xa9a, room_upgrade 0xa9b, record 0xa9c, member_name 0xab0 (×3, 13), member_pref 0xb25 (×3), ojama_condition 0xb47 (×51), set_ojama 0xbf1 (×3), recommend_ojama 0xbf4 (×3), set_item 0xbf7 (×2), parameter 0xbf9 (×4): pop'n 18's block 0x9bc lower, 0x9c0 after its titles (four won, not five), as its loader `0x4411c0` checks them and the battle's results fill set's block (`0x476c95`, from the NET state holding the block 0x7fc lower) |
| pop'n 18 (0x1700) | get_ojama_0/1 0x13c8, 0x13cc (4 each), get_item_0/1 0x13d0, 0x13d4, used_ojama_0/1 0x13d8, 0x13dc, used_item_0/1 0x13e0, 0x13e4, get_title_0..4 0x13e8 (×5, 4), exp 0x13fc (4), music_num 0x1402 (2), sheet_num 0x1454, category_num 0x1455, set_title 0x1456, room 0x1458 (0-9; set's room_num is it), room_skill 0x1459 (15 to start), room_start 0x145a, room_upgrade 0x145b, record 0x145c, member_name 0x1470 (×3, 13; the loader keeps three battles' worth, the game sends and reads back the first), member_pref 0x14e5 (×3), ojama_condition 0x1507 (×51), set_ojama 0x15b1 (×3), recommend_ojama 0x15b4 (×3), set_item 0x15b7 (×2), parameter 0x15b9 (×4), event_point 0x15bd (the battle's results fill set's block, `0x49f4d5`, from the NET state, which holds the record's block 0x1198 lower) |

### pop'n music 16, 17, 18 (PC games)

The same design again, built into the programs: libsocket's calls are static functions over
Winsock (imported by ordinal), libcomm and libkernel are exported by the program, the lobby client
is inside it. HIRAKU finds each libsocket call as the one function that calls its Winsock function
(setsockopt twice: SO_BROADCAST, SO_RCVTIMEO) and makes it jump to vsock.c's, so the relay serves
these games as it serves pop'n 15. The lobby dialect is pop'n 15's; each game has its own (services'
globby URL: 16 port 5742, 17 5743, 18 5744). The messages are tagged with the model (`"J39"`).
Checked under Wine: pop'n 18, two cabinets on one hiraku-eamuse match (MATCHING shows the other
player and a CPU), battle, save `<netvs>`, and the next battle starts from what was saved
(pop'n 18: experience 601 → 1202, skill 17 → 19; 17: 600 → 1200; 16: job experience 600 → 1200),
for each of 16, 17 and 18.

- **The mode:** 18 adds NET対戦 to its mode select (the fifth mode, `0x4c3817`) when online and
  `game get netvs_phase` ≥ 1 (0-13, more NET content at higher phases); 17 when online and
  `e_amuse_phase` ≥ 4 (`0x49adab`; its phases 0-15 open content by stages, HIRAKU answers 14);
  16 when `netvs_phase` ≥ 1 (0-2) and the card's entry went online (`0x491c41`). The mode
  select starts at the record's last mode (18: byte 0x1a).
- **A lone cabinet** waits at CUSTOMIZE, recruiting, for `matching_sec` (what follows not looked at yet).

## GuitarFreaks and DrumMania V (Konami Python 2)

DrumMania V (`E02:J:A:A:2005050201`) seen in HIRAKU's core (PLAN.md §23, PY-6), the same protocol
(the envelope, RC4, the plain calls) over the cabinet's own network adapter. The game's network
library, from the RAM of a running game (no symbols; the addresses are where it loads):

- **Before anything:** e-AMUSEMENT ON in test mode's NETWORK OPTIONS (hold Start and press Left or
  Right to change a value; saved on the hard disk; the game asks for a reboot). The cabinet's own
  address must not be 192.168.x.x: the library takes that as no router ("ルーターと通信できません",
  5-2000-0000; eamusement, `0x947a64`).
- **The library's start-up:** AVS services (`avs-services.c`): `download.eamuse...:42197` by name,
  else a UDP broadcast to port 42197, `SgetRQ` + the MAC + the two XORed, answered with 12 bytes
  (6 skipped, the server's IPv4 address, its port, big-endian); then `GET <MAC>\r\n` over TCP there,
  answered with a line, the length of what follows, and lines `name://a.b.c.d:port` (HIRAKU: none).
- **services get** must list `keepalive` and `ntp` besides the modules: `keepalive`'s URL gives the
  router, center and server the library pings (`?pa=&ia=&ga=&ma=&t1=&t2=`: ga/ma the router, pa/ia
  the center, the URL's host the server, t1/t2 the short and long intervals in seconds; the host an
  IPv4 address); `ntp://host/` its clock (SNTP).
- **Start-up calls:** pcbtracker alive, message get, posevent (income, sales, sale), shopinfo regist
  (`<shop name pref/>`), demodata get (`<favorite nr/><shoprank nr pref pnr/>`). `status="0"`
  answers do.
- **A card:** the cabinet's card unit (the 573 GuitarFreaks/DrumMania magnetic one) reads the card;
  a blank one gets a number (numbering assign, label `cardv1`, format `card16m10`, as pop'n 9's).
  - **cardutil check** `<card no= id=/>` per player: `<cardutil status="0"><card no state pass gdid
    skill/></cardutil>`; `no` must be there. `state` 0 unknown (the card can't be used), 1 not
    registered (the game registers it, keeping the `gdid` given), 2 registered (`pass` its 4 digits,
    `gdid` the player's 10 characters, `skill`), 3 expired (`0x1c483e4`).
  - **Registration:** the name (up to 8 of `A-Z 0-9 . - _ ^ [`; Left/Right and Start, END) and a
    4-digit password twice on the 10-key; **cardutil regist** `<card cardid gdid kingid name pass type
    update/>`, answered with `<cardutil status="0"/>` and nothing in it (a child element fails it:
    "登録に失敗しました", `0x94adbc`). The card is written (the name in it).
  - **The card again:** cardutil check, registered: the name from the card, the password on the
    10-key (checked by the game against `pass`).
  - **information get** and **gametop get** `<player gdid/>`. gametop's answer: `<player gdid styles
    pticket ticket>` (`0x94c990`), in it `musicdata` (base64: the game's own record of the player's
    songs, decoded into the player's 0xc30 bytes, `0x94c890`), `towerdata`, `kingdata`. HIRAKU
    answers the player without them so far.
- **After a credit:** **gameend regist** `<stage mode nr><stagedata no netid newmusic extra king
  clear/>..</stage><shoprank point/><player session king tower><playerdata card gdid styles perfect
  great good poor miss time point><stage seqmode flags score spoint skill combo perc bscore/>..
  </playerdata></player>`, the stages in the order of stagedata; posevent sale; userid ctou;
  **roulette regist** `<player gdid cmplst no exflg tiflg pticket ticket/>`. HIRAKU keeps each
  player's totals and each song and seqmode's best score, combo and percentage, and its plays
  (`gfdmv-GDID`); the players in `gfdmv-cards`.

## beatmania IIDX 9th style (a PC game)

IIDX 9th style (`C02:J:A:H`, PLAN.md §33) talks e-AMUSE3 through its own network DLLs (sonet,
sosys, eam3lib): pop'n 9's envelope and common calls (services, pcbtracker, posevent, ntp,
numbering, userid ctou, userdata read/write, package), its magnetic cards pop'n 9's format.
Its own calls are one module, `C02`, with a method and a command line:
`<C02 method="music" command="reg 2003010000000077 1 908 12 5 7 0"/>` (sonet.dll prints each
request kind's fields in turn, numbers in decimal, names in EUC-JP). The answer:

- `<C02 status="SOK" method="musicreg" .../>`: **method** the method and the command's first word
  run together (sonet.dll `0x10004600` reads nothing of an answer without it: the game then shows
  a network error), **status** a word (SOK; ENOTFOUND, ENOCARDID, EPARAM, EDENIED... errors).
- Values as attributes of `<C02>`; lists as child elements (any name), each read into the game's
  array in turn without a bound: never more than the game asked for. Their parsers: sonet.dll's
  method tables (`0x100136a0` music, `0x100137c0` ranking, `0x10013920` weekly, `0x100135f8` grade;
  per command its name, its field count and a handler per element depth).

The calls, what the game does with the answers (bm2dx JAH), and HIRAKU's answers
(`runtime/python/easerver_iidx.cpp`, files in the server's folder):

- **music reg** `CARD chart song pgreat great djlevel cleared` after each stage (`0x4364d0`): chart
  0-2 SP normal, hyper, another, 3-5 DP; the song's number (style × 100 + n, 908); the DJ level 0
  AAA to 7 F (`0x425800`: 8 − 9·EX/(2·notes), EX = 2·pgreat + great). Answer `mid`, `clid`,
  `bestScore` (1: a new best, the only part the game uses). HIRAKU keeps each card's best by song
  and chart (EX, the best DJ level, cleared ever, plays: `iidx9-scores`).
- **music getrank** `CARD 120` when a card is read: the player's marks for music select
  (`0x415e70`: per song, each chart's DJ level and a cleared bit). Answer the week's song (`mid`,
  `wid`) and a child per chart, `cl="chart"`, its text 4 hex digits a song: `song << 5 | DJ
  level << 2 | cleared << 1`. At most 1050 songs (the game's buffer, 0x189c bytes a player).
- **music getranking** `20`: the most played songs, a child each with `mid` (HIRAKU: by plays on
  this server).
- **weekly getmid**: the week's song `mid` and the week `wid`. **weekly entry** `CARD wid chart
  pgreat great NAME SHOP PREFECTURE opt` after playing the week's song: answer `bstscr`. getinfo,
  getme and getranker exist but the game doesn't ask. The week's song was Konami's choice: HIRAKU
  takes `iidx9-weekly-song` (a song number) or else one of the songs played on the server, another
  each week (none before any play).
- **ranking**: EXPERT mode's courses. The program's course table (`0x482dd0`, 0x7c bytes a
  course: its number, then five songs) holds 12 courses a class; STYLISH is class 0, course 0. The
  ALL JAPAN RANKING courses (PLATINUM, GOLD, SILVER, BRONZE: songs 1-5, 6-10, 11-15, 16-20 of
  `music getranking`, played from the 5th up) aren't these: a song missing there is the shop's own
  (LOCAL RANKING) song in its place (`0x40a84c`).
  - **getinfo**: the ranking `rid` and the six courses attract's internet ranking shows, `c0`-`c5`
    (0-11). HIRAKU: `iidx9-courses` (`rid c0 c1 c2 c3 c4 c5`) or ranking 1 with courses 0-5.
  - **getme** `CARD 120` when EXPERT is chosen: a child per course played, `clid`, `coid`, `gnum`,
    `pgnum`, `tcnt` (tries), `cflg`, read into the game's 120 places (`0x10005510`: clid × 12 +
    coid). Course select shows it as the player's 前回のスコア (FAILED / NO PLAY / the score).
    HIRAKU: the card's last try on each course, its tries, `cflg` 1 when it went through five
    stages.
  - **entry** after a course, when the player says はい to "今回のスコアを登録しますか？" on the
    PASSWORD FOR INTERNET RANKING screen (`0x436960`, `0x10004d50`): `CARD clid coid pgreat great
    NAME PIN opt SHOP n PREFECTURE stages [CARD2 NAME2]` (a course for two: both cards and names, the
    counts added up; empty names drop out of the line). The game then shows "今回のスコアが登録
    されました" and the password. Answer `rentry` (entries on the course), `bstscr` (the card's
    best EX), `bstranking` (its place), `topranker` (the top EX). HIRAKU keeps every entry but the
    PIN (`iidx9-ranking`).
  - **getranker** `RID CLID 20 COID`: the course's top 20, a card's best each: `pid` (the
    prefecture), `gnum`, `pgnum`, `opt`, `name`, `opname` (the shop), `name2`, `udate`
    (HIRAKU: the date, YYYYMMDD).
- **grade raised**: SOK.

## beatmania IIDX RED, HAPPY SKY and DistorteD (PC games)

RED (`E11`), HAPPY SKY (`ECO`) and DistorteD (`FDD`) carry their network library (libjnet) in
`bm2dx.exe`; services, posevent, pcbtracker and numbering are pop'n 9's. RED and HAPPY SKY speak 9th
style's dialect, the module named after the model: `<E11 method="pc" command="get CARD 0"/>`, the
answer `<E11 status="SOK" method="pcget" .../>` (method the method and the command's first word).
libjnet's method tables (RED JAD 0x4ea288 shop, 0x4ea318 grade, 0x4ea368 weekly, 0x4ea3b8 ranking,
0x4ea528 music, 0x4ea698 pc): per command its name, its field count and its answer's handlers by
element depth. Attribute readers: 0x476230 a number, 0x476370 the status, 0x4765c0 a string
(EUC-JP to Shift-JIS), 0x476590 a float.

- **pc reg** `CARD NAME PIN x PREFECTURE y` (HAPPY SKY one field fewer) after NAME ENTRY: answer `id`,
  the IIDX ID (8 digits, shown as 1000-0001 at once).
- **pc get** `CARD 0` when a card is read: children `<pc id idstr name pass cpass spnum dpnum sach dach
  sflg gno pid/>`, `<grade>`, `<ex>`, `<skin>`, `<rlist>` (`<rival>`), `<ocrs>` (`<my>`, `<ord>`,
  `<rnd>`), `<weekly wid mid/>` (RED JAD 0x47a120, 0x47a4a0). sach and dach (the DJ points) are floats.
  A magnetic card asks no PIN on a returning player.
- **pc save** `ID x SP-POINTS DP-POINTS SP-PLAYS DP-PLAYS ...` at the credit's end.
- **pc common**, **pc getinfomsg**, **music crate**, **ranking getranker**, **shop sentinfo PREFECTURE
  NAME -1**: at start-up and in attract (HIRAKU: answered empty). `pc common`'s children: `<wschudule wid
  since till mid0...>`, `<ir beat>`, `<crs coid ...>`, `<mr>`/`<wr>` ranking lists.
- **music reg** `ID chart song pgreat great djlevel clear PREFECTURE SHOP` after a stage (by IIDX ID;
  BEGINNER sends none). **music getrank** `ID 0`: a child per chart, `cl="chart"`, its text 5 letters a
  song from "0123456789:;", a-z, A-Z (six bits each, the first highest): song << 19 | DJ level << 16 |
  clear << 13 | EX score (RED JAD 0x479720, 1200 at most). The clear as music select shows it: 0 NO
  PLAY, 2 EASY CLEAR, 5 FULL COMBO.
- **grade raised** `ID ... SHOP PREFECTURE` after a dan course.

DistorteD's libjnet sends each kind of call as a module of its own with attributes: `<pc method="get"
ver="1" rid="REFID"/>`, `<music method="crate" ver="1" cltype="0"/>`, `<shop method="sentinfo" lid
pid opname bnum bflg/>`; answers `<pc status="0" method="pcget" ...>` (status a number; an answer
without method isn't read). Its tables at JAG 0x5e2a68 (shop) ... 0x5e2c34 (pc); its tasks (JAG
0x4d24e0, a request builder each): 9 pc get (rid), 10 getname, 11 common, 12 lcommon, 13 pc reg (cid
rid name pid), 14 save (iidxid cltype achi pnum opt opt2 gno sflg0-2 sdhd ncomb mcomb mode), 15
takeover (from HAPPY SKY: name pid oiidxid_str cid rid), 16 ifinish, 17 locaend, 18 playstart (side),
19 playend (mode), 20 test. Its IC cards are cardmng's, as pop'n 13's: inquire (112 for a pass the
server doesn't know: the game asks its new PIN twice, then getrefid), authpass, bindmodel; facility at
start-up. `pc get`'s answer carries `name`, `pass` and `pid` itself (JAG 0x4ce7c0) and the record as a
`<pc>` child (0x4ce840), read element by element (none required), then grade, ex, skin, rlist, ocrs,
weekly.

## beatmania IIDX GOLD, DJ TROOPERS and EMPRESS (PC games, libavs)

GOLD (`GLD`) and DJ TROOPERS (`HDD`) keep DistorteD's modules (pc, music, grade, ranking, shop) and
its cards (cardmng), but send them through libavs's e-AMUSE3 as pop'n 15 does (`<pc method="get"
lid="HIRAKU" rid="REFID"/>`; services must list them and `pcbevent`, keepalive and facility as pop'n
15's). Each method's reply has a parser of the game's own (GOLD's method definitions at 0x5495f4:
builder, parser, name, the service `local`, the model; DJ TROOPERS' at 0x55c7xx: name, then three
routines, the parser last). A field or element a parser reads and doesn't find fails the call: the
network library's status -10 (`HK_PC_JNET_LOG`), retried three times, the pass then played as a guest.
The answer's own attributes are text, read with `strtol`. What each needs (GOLD; DJ TROOPERS the same
unless said):

- **pc get** (GOLD 0x495e70): `<pcdata id idstr name pid spnum dpnum sach dach sflg0 sflg1 sflg2 gno
  sdhd sp_opt dp_opt dp_opt2 mcomb ncomb mode pmode/>`, `<grade sgid dgid/>` (-1 none), `<ex/>`,
  `<skin/>`, `<rlist/>` (text lists, 8 characters an entry, empty for none), and in GOLD `<gold now_g
  all_g use_g/>`; optional: rival, rcourse, ocrs, my, ord, rnd, weekly (DJ TROOPERS also visitor,
  attack, team, tranker, mdata).
- **pc reg** (0x497b80): `id`, `id_str`. **pc getname**: `name idstr pid`. **pc takeover**: `id`.
  **pc lcommon**: `ls`. **pc common**: GOLD `<gshop vipg/>` (needed), optional ir, wschedule, ajr,
  wajr, cm, item. **pc visit** (DJ TROOPERS, after pc get): `anum snum pnum aflg sflg pflg`.
- **music reg/play/lplay**: `mid clid crate`. **grade raised**: `pnum`. **ranking entry**: `anum jun`.
  **shop getname**: `opname pid cls_opt hr mi`. save, playstart, playend, sentinfo, keepalive, breg:
  nothing read.
- **music reg** sends `iidxid mid clid pgnum gnum rid` (the DJ level, 0 AAA ... 7 F) `cflg` (the clear)
  and more; **music breg** (BEGINNER) `iidxid mid pgnum gnum rid cflg`. **music getrank** `iidxid
  cltype`: `<r v="VERSION">` elements (GOLD 0x4947a0; v the song's mid / 100), their text 5 letters a
  chart from "0123456789:;", a-z, A-Z (six bits each, the first highest): the song within the version
  << 23 | chart << 19 | EX << 6 | clear << 3 | DJ level (3 letters a song when v is -1). Kept, not
  checked on screen yet.
- **pc save** at the credit's end: `iidxid cltype achi pnum opt opt2 gno sflg0-2 sdhd ncomb mcomb mode
  pmode lid` and GOLD's `now_g all_g use_g`.

EMPRESS (`I00`, its e-AMUSE3 in `libavs-win32-ea3.dll`; method definitions at 0x57decc...: three
routines a method, the parser last) the same, and: `pc reg` sends `did`; `pc get`'s pcdata adds
`lift_len`, and `jnum` (s64) and `bjnum` (18 s32) are needed; `skin` is optional but, if given, 12
u16 (an empty one fails the call); `ex` holds `e` children; music reg/play add `frate`; getrank's
answer is another form (`<m>`, `<best rno>`, `<b>`, `<t>`), not answered yet. libavs logs a parser's
failure as "res: receiver returns failure" (`HK_PC_AVS_LOG`). A new pass is asked to accept terms
(a page of its own) before NAME ENTRY.

## Not known yet

- Why DistorteD plays a returning pass as a guest after its `pc get` (PLAN.md section 36): its
  parser takes what it is given (above), and the answer now has GOLD's elements too.

- What `alter` and `aflag` are for; what the pop'n friends statuses 600 and 601
  mean to a player.
- What 700/701 mean exactly.
- Why INTERNET RANKING shows no place for a card with a KONAMI ID (above).
