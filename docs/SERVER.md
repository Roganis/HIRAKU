# hiraku-server

hiraku-server is HIRAKU's e-AMUSEMENT server for many cabinets on a network: the same answers as
`hiraku-eamuse` (docs/EAMUSE3.md), with everything it keeps in one SQLite database, plus accounts,
arcades, machines and a JSON API for web pages and apps. It is built with the system's SQLite 3, or
where there is none (the Windows build) with SQLite's own source in `third_party/sqlite/`.

## Experimental

hiraku-server is **experimental**: this is its first version. It has been tried with real games in
HIRAKU's cores against a live server, not run for months by many people. Expect faults, and changes
between versions. What it doesn't do yet, and what wasn't tried:

- **Card PINs are kept in clear.** They are in the games' own data in the database, as the games send
  them; the server doesn't hash them (the older games check a PIN themselves, in the player's record
  the server hands them, and `export` gives a desktop folder back). Whoever can read the database, a
  backup or an export reads every PIN. The API, the pages and the log never show one.
- **The games' port is for the internet only behind machine keys.** With them, each cabinet's calls
  are sealed by its own key (ChaCha20-Poly1305) and taken only as that machine. They don't hide that a
  cabinet talks to the server, nor protect what was recorded from someone who later gets the key. With
  `--no-keys`, or before the database has machines and with no `key.txt`, anyone who can reach the
  port can read and change every player kept, in clear on the way (e-AMUSEMENT's own cipher uses a
  key every copy of the games holds): a private network or a VPN only.
- **A machine with its key can still read and change any player:** the games ask for a player by
  card number, and the server answers any machine it takes. Give keys to cabinets you trust.
- **One community a server:** one card numbering, one set of rankings and one NET TAISEN lobby for
  every arcade on it. A separate world is a second server.
- **No phone app.** The JSON API is there for one; none is made.
- **Windows:** `hiraku-server.exe` builds from source and was tried (below, "On Windows"), but it has
  no package, no service and no backup timer, and is not among a release's downloads.
- **Not tried:** the system service (`deploy/hiraku-server.service`, its own user, installed as
  root): only the same program as a user's service; the container beyond its build and health check
  (CI); other proxies than Caddy; long runs under load; other machines reaching a Windows server.
- **The pages** were checked in Chromium, with the keyboard too; not in Firefox or Safari.
- **Song names** come only from packs you make from your own games (below): none for GuitarFreaks
  V and V2; GuitarFreaks and DrumMania titles are their 15-letter sort keys and their charts have no
  names; IIDX packs have titles only; pop'n 11 to 16 borrow titles from other games' tables, or show
  the genre. Some games' clear marks have no word yet.
- **An admin's new PIN** (`card pin`, `PUT /cards/{card}/pin`) doesn't reach pop'n music 9, 10 and 11
  or beatmania IIDX 9th style (where 9, 10 and IIDX 9th keep it isn't known; whether 11 takes a
  changed record isn't known); it says which games it set and which not.
- **The games' port is IPv4 only.** On a slow disk with `--sync full` many cabinets make the games'
  timed screens run out: use an SSD or `sync = normal` (below, "Load").
- **Data and API may change.** The database carries its format's number (`PRAGMA user_version`, and
  the accounts' in its `meta` table): an older program finds a newer database and reads it without
  writing to it. Nothing converts a database from one format to another yet, and `/api/v1` and the
  pages may change while this is experimental. Take a backup before every update.

Problems, and what you miss: the repository's issues, https://github.com/Roganis/HIRAKU/issues (no
card numbers, PINs or keys in them).

## Starting it

    hiraku-server import empty-folder --db /srv/hiraku/server.db     # a new database (or a data folder's files)
    hiraku-server serve --db /srv/hiraku/server.db

`serve` listens on two ports:

- **the game port** (`--port 8080 --bind 0.0.0.0`): the cabinets' calls. Once the database has
  machines, each cabinet is answered behind its own machine key only (`hk2.PCBID.SECRET`, given to
  the cabinet as `-eakey` or the launcher's Server key). The calls are encrypted by that key, not by
  HTTPS: forward this port as plain TCP, **never through an HTTP proxy**.
- **the API** (`--api 127.0.0.1:8081`, `--no-api` for none): JSON for people. It speaks plain HTTP on
  the loopback address; put a reverse proxy in front of it for TLS.

The API's other options: `--api-trust-proxy` takes the caller's address from `X-Forwarded-For`'s last
hop (only behind your own proxy), `--api-insecure-cookies` leaves `Secure` off the log-in cookie (tests
on plain HTTP only).

## The web pages

`serve` also serves the server's web pages at `/` on the API's port (`http://127.0.0.1:8081/` by
default): sign-in and sign-up, a player's cards, stats, players and games' best scores, an
operator's arcades, machines (a new machine's key shown once) and arcade settings, and an admin's
users, sign-up, cards, server settings and audit log. Each person sees the pages of their role.

They are plain files in `web/` (HTML, CSS and JavaScript, no build step), read from the first folder
with an `index.html` among: `--web DIR`; `web` beside the program; `../share/hiraku-server/web`
beside it; the checkout's `web/` when the program is in a build tree. `serve` prints the folder it
took; `--no-web` serves none. Only that folder's own files are served (no `..`, no dot-files, no
links out of it), each with `Content-Security-Policy: default-src 'self'` and no framing.

The pages call this server's API only: nothing is loaded from another site (no fonts, no scripts, no
analytics), and nothing is kept in the browser but the log-in's cookie. Songs are shown by their
number, and by their names where the server has a names pack for the game (below). Behind a proxy,
`/` goes to the same port as `/api/` (below).

## Song names

The games keep scores by their own song numbers; the song titles are the games' data, so HIRAKU
holds none. The server shows a title beside a song's number only from a **names pack** you make from
your own games' files with `tools/eamuse/names.py`. A pack is yours: keep it on your server, never
share it, never put it in a repository or a package (the repository ignores `names/` folders, and
the server's package and container leave them out).

    tools/eamuse/names.py --root HIRAKU_ROOT --out DIR [--game ID ...]

reads each game you have ingested (`HIRAKU_ROOT/dumps/ID`) in place, its own song table, and writes
`DIR/ID.json` for the games it knows how to read, saying for each how many songs it wrote or why not.
Put the files in a folder called `names` beside the database (`/var/lib/hiraku-server/names/` for the
service), or name another with `serve --names DIR` (config: `names = DIR`). `serve` reads them at the
start, says one line for each (`names: popn12.json: 595 songs`), and reads a file again when it
changes or drops it when it is gone (it looks at most every 3 seconds, when a page asks). No folder:
no names, songs by number as before. Loading a pack, or refusing one, never touches the games' port.

A pack is one UTF-8 JSON file a game, named after the game's id in the server's table
(`GET /api/v1/games`): `songs` by the game's own song number (the `song` of its scores), each with a
`title` and optionally an `artist` and a `genre`; `charts` optionally names the chart numbers. For
example (made up):

    {"format": 1, "game": "popn12",
     "songs": {"5": {"title": "Made-up Song", "artist": "Made-up Artist", "genre": "Made-up Genre"},
               "20": {"title": "Another Made-up Song"}},
     "charts": {"0": "Normal", "1": "Hyper"}}

A file is refused, in one line, when it is over 4 MB, isn't that JSON (format 1), names a game the
table lacks or is named after another game's id, or holds over 20000 songs. A text is cut at 200
characters and its control characters are dropped. An admin sees the packs loaded and refused on the
Server settings page (`GET /names`).

## The first admin

The first database is made with one account, `admin`, and a random password printed once. Log in
with it (the command line or `POST /api/v1/login`) and change it: until then that account can only
read `/me` and change its password. Then make operators (`PUT /users/NAME {"role": "operator"}`) and
choose how people sign up: open, by invitation, or closed (`PUT /config`).

## TLS in front: Caddy

    games.example.org {
        reverse_proxy 127.0.0.1:8081   # the API (/api/) and the web pages (/)
    }

Start serve with `--api-trust-proxy` so the limits count each caller, not the proxy. The game port
stays a port of its own (8080 here), forwarded as it is.

## The API

Everything is under `/api/v1/`, JSON in and out (`application/json; charset=utf-8`). An error is
`{"error": "code", "message": "plain words"}` with its HTTP status: 400 bad input, 401 not logged in,
403 not allowed, 404, 409 conflict, 413 a body over 64 KiB, 429 slow down (`Retry-After`). A card
link refused for its PIN says how many wrong PINs are left before the card is locked (`tries_left`,
also in the message); one refused because the card is locked has `"error": "card_locked"`,
`"locked": true`, `tries_left` 0.

**Who calls.** `POST /login` answers with a session cookie (`hk_session`, HttpOnly, SameSite=Strict,
Secure) and a `csrf` value; every change made with the cookie sends that value back in `X-HK-CSRF`.
A session lasts 30 days from its last use. An app or a script uses a personal token instead
(`POST /me/tokens`, shown once), sent as `Authorization: Bearer TOKEN`; a `read` token can't change
anything. A disabled account's sessions and tokens stop at once.

**What never goes out:** a password, a PIN, a hash, a session or token or machine key after the
answer that made it, and a card's number to anyone but its own user or an admin. Players' names are
given as the games keep them, made UTF-8; one that can't be is `""` with its bytes beside, as hex
(`player_hex`), wherever a `player` is sent (stats, best scores, `/cards/{card}`). No answer carries a
path on the server's disk.

**Scores.** A game object (`/server`'s and `/games`' items, `/games/{id}/top`, each game of the stats)
has `shows: {plays, song_plays, clear}`: whether the game ever keeps the player's count of plays
(`plays` of the stats), a chart's own count (a score row's `plays`), a clear mark with a word for it
(a row's `clear_name`; a game whose marks have none still sends `clear`, as a number).
It is the game's, not one player's: a page leaves out a column that can only be empty. A score row
has `clear` (the game's own number, `null`: none) and `clear_name`, the mark as a short word
("Cleared", "Failed", "Full combo", "No play"...), `null` when the game keeps none or the number's
meaning isn't known. pop'n's better medals are "Cleared" too (which medal is which isn't known). The System 573 games keep a score for a cleared stage only: a failed one has
`score: null` and its mark. A score of 0 is in its player's stats, not among the best scores.

**How often:** log-in 10 tries a minute an address and a name, sign-up 5 an hour an address, card
links 20 an hour an account, anything else 600 a minute a session or token.

| Method | Path | Who | What |
|---|---|---|---|
| GET | /health | anyone | `{ok, version, uptime}`; 503 when the database doesn't answer |
| GET | /server | anyone | the server's name, sign-up mode, games (id, title, has_names, shows) |
| POST | /signup | anyone | `{name, password[, invite]}` |
| POST | /login | anyone | `{name, password}`: the cookie, `csrf`, `must_change_password` |
| POST | /reset | anyone | `{code, password}`: a reset code from an admin |
| POST | /logout | user | ends the session |
| GET | /me | user | name, role, hide_stats, how signed in |
| POST | /me/password | user (log-in) | `{old_password, password}` |
| PUT | /me/hide_stats | user | `{hide_stats: true}` hides one's stats from other players |
| GET, POST | /me/tokens | user (log-in) | list; make `{label, scope: "read"\|"write"}` (the token once) |
| DELETE | /me/tokens/{id} | user (log-in) | revoke |
| GET, POST | /me/cards | user | list; link `{card, pin}` (any game's PIN; five wrong lock the card) |
| DELETE | /me/cards/{card} | user | unlink |
| GET | /me/stats | user | each linked card's games: title, shows, player, plays, scores (song, chart, score, plays, clear, clear_name) |
| GET | /players | user | users with a card linked whose stats show |
| GET | /players/{name}/stats | user | their stats (no card numbers) |
| GET | /games | user | the games (id, title, scores read, has_names, shows) |
| GET | /games/{id}/songs | user | the game's names pack: `{songs: {"N": {title, artist, genre}}, charts: {"N": name}}` (empty objects without one) |
| GET | /games/{id}/top?song=&chart=&limit= | user | the game's title and shows, its best scores (above 0) among players whose stats show: rank, user, player, song, chart, score, clear, clear_name |
| GET, POST | /arcades | operator | own arcades (admin: all); add `{name, prefecture[, owner]}` |
| GET, PUT, DELETE | /arcades/{id} | operator | `{name, prefecture}` |
| GET, POST | /arcades/{id}/machines | operator | list; add `{name}` (its key once) |
| GET, PUT | /arcades/{id}/settings | operator | each game's settings (value, default, range, help); `{settings: [{game, name, value}]}` |
| GET | /machines | operator | every machine one may see |
| GET, PUT, DELETE | /machines/{pcbid} | operator | `{name, enabled}` |
| POST | /machines/{pcbid}/key | operator | a new key (once); the old one stops |
| GET | /users | admin | every account |
| GET, PUT, DELETE | /users/{name} | admin | `{role, disabled, hide_stats}` |
| GET | /users/{name}/cards | admin | the cards linked to that account |
| POST | /users/{name}/reset | admin | a reset code (once, for a day) |
| GET, PUT | /config | admin | `{name, signup: "open"\|"invite"\|"closed", list_all_games}` |
| POST | /config/invites | admin | `{uses, days}`: an invitation code (once) |
| GET | /cards/{card} | admin | `{card, known, user, linked, locked, wrong_pins, tries_left, games: [{id, title, player}]}`: whether a game has a player on it, its account (`null`: none), its lock, its players; never its PIN |
| POST | /cards/{card}/unlock | admin | a card locked by wrong PINs |
| DELETE | /cards/{card} | admin | unlink it from its account |
| PUT | /cards/{card}/pin | admin | `{pin}` in every place the games keep it |
| GET, PUT | /settings | admin | the server's own settings and every game's defaults |
| GET | /audit?limit= | admin | the last actions: when, who, what |
| GET | /names | admin | `set` (a names folder is configured), the packs loaded (game, title, file, songs, charts, time) and the files refused (file, why) |

An operator acts on its own arcades and their machines only; another operator's answer 404.

## Running it as a service

### The config file

`serve --config FILE` reads its options from a file, one `name = value` a line; `#` starts a comment.
The names are serve's long options without their dashes; an option given on the command line as
well wins over the file. A name the file shouldn't hold, or a value out of range, stops the start
with the file's name and the line's number. A value may be put in double quotes; a relative path is
taken from the folder serve is started in, so a service's file names whole paths.

    # /etc/hiraku-server/server.conf (made-up)
    db = /var/lib/hiraku-server/server.db
    port = 8080
    bind = 0.0.0.0
    api = 127.0.0.1:8081
    api-trust-proxy = true

| Name | Value | What |
|---|---|---|
| db | a file | the database (made if there is none) |
| port | 1 to 65535 | the games' port (8080) |
| bind | an IPv4 address | where the games' port listens (0.0.0.0: every address) |
| allow | addresses | connections taken from these only: `A.B.C.D` or `A.B.C.D/BITS`, commas or spaces between |
| cabinets | a file | the cabinets answered when the database has no machines (cabinets.txt beside the database) |
| key-file | a file | the server's key for all when the database has no machines (key.txt beside the database) |
| clock | "YYYY-MM-DD HH:MM:SS" | the games' clock starts there (tests); none: the host's |
| tz | -12 to 14 | the games' time zone, hours from UTC (9) |
| api | ADDRESS:PORT | the API and the web pages (127.0.0.1:8081) |
| web | a folder | the web pages' folder (it must hold index.html) |
| names | a folder | the song names' packs (`names` beside the database; above) |
| quiet | true or false | only the start-up lines, refusals and errors |
| log-calls | true or false | every call's body in the log (below) |
| no-relay | true or false | no NET TAISEN lobby and relay |
| no-keys | true or false | machines answered by the PCBID a call says, without their keys (a private network) |
| no-api | true or false | no API and no web pages |
| no-web | true or false | the API without the web pages |
| api-trust-proxy | true or false | the API's caller is `X-Forwarded-For`'s last hop (behind your own proxy only) |
| api-insecure-cookies | true or false | the log-in cookie without `Secure` (plain-HTTP tests only) |
| connections | 16 to 4096 | the games' connections served at once (256); at that many, more wait their turn in the port's queue |
| connections-per-address | 0 to 4096 | at most that many of them from one address (64; 0: no limit); more from it are closed and said once a minute. This computer's own addresses have no limit |
| sync | full or normal | how hard a change waits for the disk (full; below) |

A switch is `true`, `yes`, `on` or `1`, or `false`, `no`, `off` or `0`.

### How hard a change waits for the disk: `--sync`

`--sync full` (the default): each call that changes something is on the disk before its answer
goes (SQLite's `synchronous=FULL`). `--sync normal`: the changes reach the disk at the database
log's checkpoints instead, every few hundred changes (`synchronous=NORMAL`, in WAL mode), and the
calls stop waiting for the disk. What `normal` gives up: after a power cut or a crash of the whole
computer, the last moments' calls (a credit's scores, a new player) may be missing. The database
is never left broken, and a crash or kill of hiraku-server alone loses nothing. Choose it for a
slow disk (a hard disk, an SD card, a cheap VPS's) under many cabinets, on a computer with a UPS
or where losing the last seconds before a power cut is no worry; keep `full` otherwise. serve says
at its start which it is.

### Start-up

Anything that keeps serve from starting is said in one line, what and where, with exit status 1: the
database's folder missing or not writable (SQLite keeps its `-wal` and `-shm` files beside the
database), the games' or the API's port taken, a config file's line, a web folder without
`index.html`. A wrong command line exits with 2. `hiraku-server --version` prints the build; serve's
first line and `/health` say it too.

### The log

serve writes its log to standard output, a whole line at a time, flushed at once, so journald or a
container's log keeps them in order. Every line of serve's begins with its time, ISO 8601 in UTC: the
start-up lines, the calls', the API's, the refusals' and the stop's.

    2026-11-16T09:30:00Z call 0120A1B2C3D4E5F60718 "Front left" F14:J:A:A cardmng.inquire 200 3ms
    2026-11-16T09:30:01Z api POST /api/v1/login 200 96ms -
    2026-11-16T09:30:02Z api GET /api/v1/me/stats 200 2ms alice

A game call's line: the machine's PCBID, its name (`-` when the database has none), the game's
model, `module.method` of each request in the call (comma-separated), the HTTP status and how long
the answer took. An API request's line: the method, the path without its query string (a card's
number in it as `*`), the status, the time, and the user's name when one is logged in (`-` if not).
Refused connections and calls are said (`refused: ...`, whatever `--quiet` says), each refusal once a
minute at the most by its reason and by whom: another reason, another address or another machine is
said at once. A cabinet with a machine's key the server doesn't take (no such machine, a disabled one) is
told so (`HKKU`, docs/EAMUSE3.md); an old key is said when its cabinet closes at the server's hello.

**What it never holds:** a call's body, a card's number, a PIN, a password, a session cookie, a
CSRF value, a token, a reset or invitation code, a machine's key.

`--log-calls` adds every call's whole body and answer, for finding a fault: cards, refids and
players' names are in them, but the values of the attributes and elements named `pass`, `passwd`,
`password`, `ea_passwd`, `cpass` and `pin` (and the PIN in IIDX's `pc reg` command) are `***`, and a
body that isn't XML is not shown. serve says at its start that bodies are being logged. Don't leave
it on. `--quiet` keeps only the start-up lines, refusals and errors.

### Health

`GET /api/v1/health` on the API's port needs no log-in. It answers 200 with `{"ok": true,
"version": "...", "uptime": SECONDS}` when the database answers a query, else 503 with `"ok":
false`; nothing else is in it. For a container's or a monitor's check:

    hiraku-server health --api 127.0.0.1:8081

asks that address (the API's default when none is given), prints one line and exits 0 when the
server and its database answer, else 1. It needs no database of its own.

### Backups

    hiraku-server backup --dir /var/backups/hiraku-server --keep 14 --db /var/lib/hiraku-server/server.db

writes `hiraku-server-YYYYMMDD-HHMMSS.db` (the time in UTC) into the folder with SQLite's online
backup, safe while serve runs; checks it (`PRAGMA integrity_check`); then removes its own older
backups beyond the newest 14 (`--keep N`). It never touches a file of another name. A backup that
fails its check is removed and the older ones are kept (exit 1). Run it daily from a timer; two in
the same second fail (the name is taken). `backup FILE` still makes one copy where it is told.

A backup is the whole server: the games' data and the accounts, arcades, machines and their keys,
sessions and the audit log. To restore one: stop serve, put the backup in place of the database
(removing any `-wal` and `-shm` files beside the old one), start serve. Or take the games' data out of
a backup without stopping anything: `hiraku-server export FOLDER --db BACKUP` makes a data folder of
it, which `import` reads into a database, or `hiraku-eamuse` serves as it is. `export` holds the
games' data only (cards and their PINs, players, scores, settings) and says so: no accounts, arcades,
machines or keys, so a database made from it by `import` has a new first admin and no machines.

### Stopping and updating

SIGTERM or SIGINT (Ctrl-C) stops serve cleanly: it takes no new connections, lets the calls in hand
finish (3 seconds at most), stops the API, closes the database (its WAL folded into the file, no
`-wal` left), prints one line and exits 0. A second signal ends it at once (exit 128 + the
signal's number). On Windows the console's events do the same (below, "On Windows").

To update, stop serve, replace the program (and its web folder), start it again. The database
carries its format's number (`PRAGMA user_version`, and the accounts' own in its `meta` table): a
program that finds a newer format than its own reads the database and writes nothing to it, and
says so. So take a backup before an update: going back to the older program afterwards means going
back to that backup.

## Installing

hiraku-server has a package of its own, `hiraku-server-VERSION-linux-x86_64-experimental.tar.xz`
among a release's downloads (made by `tools/package-server.sh BUILD_DIR`, without `VERSION` unless
`HK_VERSION` is set; tried by `tools/smoke-server.sh`): `bin/hiraku-server`, its web
pages in `share/hiraku-server/web`, these docs and the licences in `share/doc/hiraku-server`, and
`deploy/` (the files below), and a release's number in `VERSION`. It is not in HIRAKU's desktop packages. It needs glibc and libstdc++ as
new as its README says, and the system's SQLite 3: on Debian or Ubuntu, `apt install libsqlite3-0`.

### From the package, as a service

The package in `/opt/hiraku-server`, a system user of its own, its data in `/var/lib/hiraku-server`
(the database, `key.txt` and `cabinets.txt` beside it, `backups/`), its options in
`/etc/hiraku-server/server.conf`:

    sudo tar -xJf hiraku-server-VERSION-linux-x86_64-experimental.tar.xz -C /opt
    sudo useradd --system --user-group --home-dir /var/lib/hiraku-server --shell /usr/sbin/nologin hiraku-server
    cd /opt/hiraku-server/deploy
    sudo install -D -m 0644 server.conf.example /etc/hiraku-server/server.conf     # then read it, change what you need
    sudo cp hiraku-server.service hiraku-server-backup.service hiraku-server-backup.timer /etc/systemd/system/
    sudo systemctl daemon-reload
    sudo systemctl enable --now hiraku-server.service hiraku-server-backup.timer
    sudo journalctl -u hiraku-server | grep 'first admin'    # the first admin's password, printed once

`hiraku-server.service` runs `serve --config /etc/hiraku-server/server.conf` as the user
`hiraku-server` (`Type=exec`, `Restart=on-failure`); systemd makes `/var/lib/hiraku-server` its own
(`StateDirectory`) and stops it with SIGTERM (the clean stop above). It sees the rest of the system
read only, no home folders, no devices, no other user's processes, and only IPv4, IPv6 and local
sockets. `hiraku-server-backup.timer` runs `backup --dir /var/lib/hiraku-server/backups --keep 14`
once a day (a day missed while the machine was off is made up at its start). If `server.conf`
names another database, change the backup unit's `--db` to it as well.

Log in to the web pages with `admin` and that password, and change it (or `sudo -u hiraku-server
/opt/hiraku-server/bin/hiraku-server user password admin --as admin --db /var/lib/hiraku-server/server.db`).
The accounts' commands work on the running server's database: run them as the user `hiraku-server`.

### Behind Caddy

`deploy/Caddyfile.example`: Caddy gets a certificate for the server's name, redirects plain HTTP to
HTTPS, tells browsers to keep to HTTPS (`Strict-Transport-Security`) and passes `/api/` and the web
pages to `127.0.0.1:8081`; `server.conf` then says `api-trust-proxy = true` (Caddy sets
`X-Forwarded-For` to the browser's address and drops one the browser sent). The games' port
(8080) is not proxied: open it in the firewall as plain TCP, each machine's calls encrypted by its key.

### As a container

`deploy/Dockerfile` builds only hiraku-server on Debian 12 and runs it on Debian 12 slim as a user of
its own:

    docker build -f deploy/Dockerfile -t hiraku-server .
    docker run -d --name hiraku-server --restart unless-stopped -v hiraku-data:/data \
      -p 8080:8080 -p 127.0.0.1:8081:8081 hiraku-server
    docker logs hiraku-server 2>&1 | grep 'first admin'

The database, its key and cabinet files are in the volume `/data`; take backups with
`docker exec hiraku-server /opt/hiraku-server/bin/hiraku-server backup --dir /data/backups --db /data/server.db`
(and copy them off the machine). The log is the container's (`docker logs`). Its health check runs
`hiraku-server health`; `docker stop` sends SIGTERM, which stops it cleanly. Inside the container the
API listens on every address so that the published port reaches it: publish 8081 on the host's
`127.0.0.1` only, and put the proxy there (add `--api-trust-proxy` to the command when you do).

### Updating

Take a backup (`systemctl start hiraku-server-backup`), stop the service, unpack the new package
over `/opt/hiraku-server` (its `deploy/` files are examples: compare your units and `server.conf`
with them), start it. A container: build the new image, `docker rm -f` the old container and start
the new one on the same volume. Going back to an older program afterwards means going back to that
backup (above, "Stopping and updating": an older program reads a newer database without writing).

### On Windows

`hiraku-server.exe` builds in the Windows cross-build (`cmake/x86_64-w64-mingw32.cmake`, target
`hiraku-server`; SQLite compiled in, nothing beside it to install) and runs on Windows 10 1903 or
later. It builds from source only: it has no package yet and is not among a release's downloads (the
desktop zip leaves it out). Copy `hiraku-server.exe` and the `web` folder beside it, then, in a
console:

    hiraku-server.exe import empty-folder --db D:\hiraku\server.db
    hiraku-server.exe serve --config D:\hiraku\server.conf

What differs from Linux:

- **No service.** There is no Windows service or scheduled task for it, and no backup timer: it
  runs in a console window while someone is signed in (or start it with Task Scheduler yourself),
  and `backup --dir` is run by hand or by a task of your own.
- **Stopping:** Ctrl-C, Ctrl-Break or closing its console window stop it cleanly, as SIGTERM does on
  Linux (the log says which: `stopped (the console closed)`); signing out and shutting Windows down
  are handled the same way (not tried). `taskkill /PID N` without `/F` doesn't stop it (Windows says a console program can only be
  ended with `/F`); `taskkill /F` ends it without the stop: the database stays whole (SQLite's WAL),
  its `-wal` file is folded in at the next start.
- **Paths:** any letters and spaces in the folders' names; the web pages are found in `web` beside
  the .exe.
- **Firewall:** bound to `127.0.0.1` it needs nothing; listening on the network (`bind = 0.0.0.0`)
  needs Windows' firewall to let it in, as any server does (not tried).
- Player names the games keep in Shift-JIS are turned into UTF-8 by Windows' own code page 932,
  as by iconv on Linux.

Tried on Windows 11 (2026-11-17, Defender on, a made-up arcade): `serve` from a config file on a new
database, `health`, the pages and the API (sign-up, log-in, `/me`), an arcade and a machine with its
key, `key_client.exe` calls behind it (and refused with another key), a player with a Japanese name
read back through `/me/stats`, `backup --dir`, `export`, each stop above, all in a folder named
`テスト フォルダ café` too. Under Wine: the ten transcripts, `test_api.py` and `test_accounts.py`.
Not tried: other machines on a network reaching it, a reverse proxy in front, long runs under load.

### Never reachable from outside

- the games' port with `no-keys` on (any cabinet that names a PCBID is answered as that machine):
  only on a private network or a VPN;
- the API's port itself (8081): only through the proxy, which adds HTTPS;
- the database and its `-wal` and `-shm` files, `key.txt`, `cabinets.txt`, the backups: they hold
  every card and its PIN (in clear: above, "Experimental"), every session and what the machines' keys are made from. The service's folder is `0750` and what it makes
  `0640` at most (`UMask=0027`); a container's are in its volume. Copies of backups kept elsewhere need the same care.

## Load

`tools/eamuse/load.py HIRAKU-SERVER --cabinets N --seconds S` starts a `serve --no-keys` of its own
on 127.0.0.1 and has N made-up cabinets play credits against it at once, as fast as it answers:
pop'n music 12, pop'n music 15 and beatmania IIDX 16 in turn (the card read, the player got or
made, the credit's scores, a ranking entry; a third of the credits with a new card), while another
process asks `/api/v1/health` twice a second and one `backup --dir` runs half way through. A
3-second run is the unit test `eamuse_load_smoke`.

Measured 2026-11-16 on an 8-core desktop CPU (AMD Zen 3, 16 threads) shared with other work, the
client in 16 processes on the same machine, 30 seconds a run, serve's defaults (256 connections at
once, the cabinets all from this computer). "Disk": the database on a 2.5-inch laptop hard disk,
nearly full, whose `fsync` takes about 33 ms; "RAM": the database in a memory file system, as a
stand-in for a fast SSD.

| Cabinets | Where | --sync | Calls a second | Credits | p50 | p95 | p99 | max | Errors | Backup | Database |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 20 | disk | full | 14 | 91 | 1612 ms | 2185 ms | 2360 ms | 2.5 s | 0 | 15.7 s | 2.4 MB |
| 100 | disk | full | 12 | 100 | 7602 ms | 13837 ms | 14575 ms | 14.8 s | 0 | 15.5 s | 4.6 MB |
| 300 | disk | full | 7 | 0 | 19718 ms | 29382 ms | 29916 ms | 30.1 s | 435 | 1.3 s | 4.4 MB |
| 20 | disk | normal | 314 | 1825 | 7.9 ms | 588 ms | 725 ms | 1.3 s | 0 | 0.26 s | 12.7 MB |
| 100 | disk | normal | 270 | 1645 | 184 ms | 817 ms | 1371 ms | 1.9 s | 0 | 0.74 s | 9.5 MB |
| 300 | disk | normal | 197 | 1670 | 750 ms | 4098 ms | 4761 ms | 8.0 s | 0 | 1.5 s | 45.1 MB |
| 20 | RAM | full | 975 | 5637 | 19.5 ms | 42.7 ms | 51.6 ms | 87 ms | 0 | 0.01 s | 11.1 MB |
| 100 | RAM | full | 954 | 5548 | 104 ms | 197 ms | 226 ms | 438 ms | 0 | 1.9 s | 11.2 MB |
| 300 | RAM | full | 881 | 5303 | 330 ms | 589 ms | 628 ms | 1.3 s | 0 | 0.01 s | 11.2 MB |
| 20 | RAM | normal | 991 | 5725 | 19.1 ms | 42.8 ms | 51.9 ms | 88 ms | 0 | 0.12 s | 11.2 MB |
| 100 | RAM | normal | 956 | 5551 | 103 ms | 199 ms | 218 ms | 417 ms | 0 | 0.02 s | 11.2 MB |
| 300 | RAM | normal | 903 | 5439 | 323 ms | 605 ms | 664 ms | 1.3 s | 0 | 0.03 s | 11.5 MB |

Calls a second and the latencies count the calls answered; Credits, the credits played to their end
in the 30 seconds; Errors, the calls that failed (each one a connection closed without an answer).
The health check answered every time in every run (61 of 61, the slowest in 19 ms), and every
backup taken in the middle of the load passed its check.

What changed since the first measurement: serve no longer closes the connections beyond 64 at
once but serves 256 and lets the rest wait their turn, so with 100 and 300 cabinets no call is
turned away any more where thousands were; and `--sync normal` lets a slow disk take some 20 times
the changes it took with `full`.

What this means:

- **The disk sets the pace.** With `--sync full` each call that changes something waits for the
  disk to confirm it: on the hard disk 12 to 15 changes a second, and with more cabinets than that
  the calls queue. At 300 cabinets the queue grows past 15 seconds, a connection's limit, and
  those calls are closed unanswered (the 435 errors; a game tries again later). On a fast disk the
  same program answers about 950 calls a second (a credit is 6 to 9 calls), and then the CPU is
  the limit. Put the database on an SSD, or choose `--sync normal` (above) for a slow disk.
- **`--sync normal` on the hard disk** takes 200 to 300 calls a second, a third of what a fast disk
  takes, with no error; its database log (`-wal`) grows while the disk is that busy and is folded
  in when the load drops.
- **Real cabinets are far gentler.** A cabinet plays a credit every two or three minutes, which is
  a call every 15 to 20 seconds on average: 300 busy cabinets make 15 to 20 calls a second. That
  is about what the slow hard disk takes with `full`, and a fiftieth of what a fast disk takes.
- **The database stays small**: about 12 MB after some 5700 credits (a third of them on new
  cards). A backup of it takes a fraction of a second on a fast disk, up to 16 s on the slow one,
  and the calls go on meanwhile.

## Example

    curl -s -c jar -X POST -d '{"name":"alice","password":"a-long-password"}' https://games.example.org/api/v1/login
    curl -s -b jar https://games.example.org/api/v1/me/stats
    curl -s -H "Authorization: Bearer hkt_..." https://games.example.org/api/v1/games/iidx13/top?limit=10
