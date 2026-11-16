# hiraku-server

hiraku-server is HIRAKU's e-AMUSEMENT server for many cabinets on a network: the same answers as
`hiraku-eamuse` (docs/EAMUSE3.md), with everything it keeps in one SQLite database, plus accounts,
arcades, machines and a JSON API for web pages and apps. It is built when the system has SQLite 3.

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
number. Behind a proxy, `/` goes to the same port as `/api/` (below).

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
403 not allowed, 404, 409 conflict, 413 a body over 64 KiB, 429 slow down (`Retry-After`).

**Who calls.** `POST /login` answers with a session cookie (`hk_session`, HttpOnly, SameSite=Strict,
Secure) and a `csrf` value; every change made with the cookie sends that value back in `X-HK-CSRF`.
A session lasts 30 days from its last use. An app or a script uses a personal token instead
(`POST /me/tokens`, shown once), sent as `Authorization: Bearer TOKEN`; a `read` token can't change
anything. A disabled account's sessions and tokens stop at once.

**What never goes out:** a password, a PIN, a hash, a session or token or machine key after the
answer that made it, and a card's number to anyone but its own user or an admin. Players' names are
given as the games keep them, made UTF-8; one that can't be is `""` with its bytes beside, as hex
(`player_hex`).

**How often:** log-in 10 tries a minute an address and a name, sign-up 5 an hour an address, card
links 20 an hour an account, anything else 600 a minute a session or token.

| Method | Path | Who | What |
|---|---|---|---|
| GET | /server | anyone | the server's name, sign-up mode, games (id, title) |
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
| GET | /me/stats | user | each linked card's games: player, plays, scores |
| GET | /players | user | users whose stats show |
| GET | /players/{name}/stats | user | their stats (no card numbers) |
| GET | /games | user | the games |
| GET | /games/{id}/top?song=&chart=&limit= | user | best scores among players whose stats show |
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
| POST | /cards/{card}/unlock | admin | a card locked by wrong PINs |
| DELETE | /cards/{card} | admin | unlink it from its account |
| PUT | /cards/{card}/pin | admin | `{pin}` in every place the games keep it |
| GET, PUT | /settings | admin | the server's own settings and every game's defaults |
| GET | /audit?limit= | admin | the last actions: when, who, what |

An operator acts on its own arcades and their machines only; another operator's answer 404.

## Example

    curl -s -c jar -X POST -d '{"name":"alice","password":"a-long-password"}' https://games.example.org/api/v1/login
    curl -s -b jar https://games.example.org/api/v1/me/stats
    curl -s -H "Authorization: Bearer hkt_..." https://games.example.org/api/v1/games/iidx13/top?limit=10
