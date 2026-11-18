# hiraku-server's API: a guide for app developers

This is for someone writing an app, a page or a script against hiraku-server's JSON API without
reading its C++. The API's description, for generating a client and for checking, is
[openapi.yaml](openapi.yaml) (OpenAPI 3.1; [openapi.json](openapi.json) is the same as JSON). The
server itself is in [SERVER.md](SERVER.md): how to run it, configure it, put it behind a proxy.

What is written here is what the code does (`server/api.cpp`, `server/accounts.cpp`), and the test
`eamuse_openapi` keeps the description to it: every route, its role, and every answer of the API's
tests checked against the schemas.

## What the server is

hiraku-server is HIRAKU's e-AMUSEMENT server for many cabinets: the arcade games of HIRAKU's cores
(pop'n music, beatmania IIDX, DrumMania, GuitarFreaks...) save their players, scores and settings on
it, as they did on Konami's network. It keeps everything in one SQLite database. It listens on two
ports:

- **the games' port** (8080): the cabinets' own protocol, encrypted by each machine's key. Not for
  apps.
- **the API's port** (8081 on 127.0.0.1, behind a reverse proxy with TLS): this API, JSON for people,
  and the server's own web pages at `/`, which are the API's first client (`web/`).

People have accounts, and an account has one role:

| Role | What it may do |
|---|---|
| player | its own account, cards, stats and tokens; read other players' stats and the best scores |
| operator | a player's, plus its own arcades, their machines and their settings |
| admin | everything: every account, arcade, machine and card, the server's configuration and settings, the audit log |

Roles stack: an admin is an operator, an operator a player. The first account, `admin`, is made
with the database, its password printed once and to be changed at the first log-in.

## Base URL and versions

Everything is under `/api/v1/`, on the API's port or behind the proxy:

    https://games.example.org/api/v1/server        (behind a proxy; made-up name)
    http://127.0.0.1:8081/api/v1/server             (on the server's computer)

The proxy passes `/api/` and `/` to the same port (`deploy/Caddyfile.example`).

**Versions.** `v1` is the only version. hiraku-server is **experimental** (SERVER.md,
"Experimental"): `/api/v1` and its answers may change between HIRAKU versions, without a `v2` beside
it, until the server is called stable. Nothing is promised yet. What has held so far and is unlikely
to change: the error shape, the log-in and token scheme, the names of the fields listed here. An app
should ignore fields it doesn't know (the server may add some) and check `GET /health`'s `version`
(the server's build) when something looks different. The description at
`GET /api/v1/openapi.yaml` (or `.json`) on a running server is the one for that server's build.

## Who calls: sessions, tokens, CSRF

Two ways to say who calls:

1. **A session** (a log-in). `POST /login {name, password}` answers with a cookie,
   `hk_session=hks_...` (`HttpOnly; SameSite=Strict; Path=/; Max-Age=2592000`, and `Secure` unless
   serve runs with `--api-insecure-cookies`), and a **CSRF value** in the body (`csrf`). Every request
   with the cookie that isn't a GET (to an operation that needs a log-in) must send that value back in
   the header `X-HK-CSRF`, else 403 `csrf`. A session lasts 30 days from its last use (each use makes
   it 30 days again, noted once an hour at most). `GET /me` gives the CSRF value again, to whoever
   holds the cookie.
2. **A personal token.** `POST /me/tokens {label, scope}` (with a session) makes one, `hkt_` and 64
   hex digits, shown in that answer only. It is sent as `Authorization: Bearer hkt_...`, needs no CSRF
   value, and lasts until it is revoked (`DELETE /me/tokens/{id}`). Scope `read` makes GETs only
   (anything else: 403 `read_only_token`); `write` may do what a session may, except the session-only
   operations: listing, making and revoking tokens, and changing the password. 50 tokens an account at
   most.

When a request has an `Authorization` header, it is the token that counts and the cookie is not
looked at; a header that isn't `Bearer ...` is refused (401) on an operation that needs a log-in.
On a public operation (`/health`, `/server`, `/signup`, `/login`, `/reset`, the description) a wrong
cookie or token is ignored and the request goes on as nobody's.

A disabled or removed account's sessions and tokens stop at once (401). An account marked
`must_change_password` (the first admin; one whose password an admin set) may only read `GET /me`,
change its password (`POST /me/password`) and log out; everything else answers 403
`must_change_password`.

The order the server checks a request in, so an app knows which error comes first: the body is a JSON
object (400 `bad_json`; every method but GET), who calls (401), how often (429), a token where only a
session will do (403 `session_only`), the password to change (403), a read token writing (403), the
CSRF value (403), the role (403 `not_allowed`), then the operation's own checks.

### A worked sequence (curl, made-up names)

    API=https://games.example.org/api/v1

    # an account (if sign-up is open: GET $API/server says "signup")
    curl -s -X POST $API/signup -d '{"name":"alex","password":"a-long-made-up-password"}'
    # -> 201 {"name":"alex"}

    # log in: the cookie into a jar, the CSRF value from the body
    curl -s -c jar -X POST $API/login -d '{"name":"alex","password":"a-long-made-up-password"}'
    # -> 200 {"csrf":"0f1e...","must_change_password":false,"user":{"name":"alex","role":"player",...}}
    CSRF=0f1e...

    # a read with the cookie
    curl -s -b jar $API/me/stats

    # a change with the cookie: the CSRF value too
    curl -s -b jar -H "X-HK-CSRF: $CSRF" -X PUT $API/me/hide_stats -d '{"hide_stats":true}'

    # a token for the app (shown once: keep it)
    curl -s -b jar -H "X-HK-CSRF: $CSRF" -X POST $API/me/tokens -d '{"label":"my phone","scope":"read"}'
    # -> 201 {"id":3,"label":"my phone","scope":"read","token":"hkt_0011..."}

    # from then on, the token alone
    curl -s -H "Authorization: Bearer hkt_0011..." "$API/games/iidx13/top?limit=10"

    # log out (the session ends, the cookie is cleared)
    curl -s -b jar -H "X-HK-CSRF: $CSRF" -X POST $API/logout

The server doesn't look at `Content-Type`: a body is read as JSON whatever it says. Send
`application/json` anyway.

### What an app can do today

- **A native app or a script** (phone, desktop, server-side): yes. Log in once with the user's
  name and password, read `hk_session` from the `Set-Cookie` header and `csrf` from the body, make a
  token with them (`POST /me/tokens`, cookie and `X-HK-CSRF`), then keep the token (in the platform's
  secure storage) and forget the password. A cookie jar that follows browser rules won't keep a
  `Secure` cookie over plain HTTP: against a test server on `http://`, run serve with
  `--api-insecure-cookies`, or set the `Cookie` header yourself. There is no OAuth, no refresh token,
  and no way to make a token without a session.
- **A web page on the server's own origin** (served by the same proxy, under the same host name):
  yes, as the server's own pages do (`web/api.js`): the browser keeps the cookie, the page keeps the
  CSRF value in memory and sends it with each change.
- **A web page on another origin**: no. The server sends no CORS headers
  (`Access-Control-Allow-Origin` and the rest) and answers a preflight `OPTIONS` with 404, and the
  cookie is `SameSite=Strict`, so a browser neither sends the cookie across sites nor lets the page
  read an answer. A browser app has to be served from the server's origin, or call its own backend,
  which calls the API with a token.

## The data

| Thing | What it is | Its key |
|---|---|---|
| account (user) | a person: name, role, `disabled`, `hide_stats`, `must_change_password`, `created` | its name, 1 to 32 letters, digits, `.`, `_`, `-` (any case: `Alex` and `alex` are one) |
| card | an e-AMUSEMENT card a game has seen: its number (16 characters, as the games send it, kept upper case) | its number |
| PIN | the card's four digits, as the games keep it, in each game's own record. The API never sends one | — |
| player | what a game keeps for a card: a player name in the game's own encoding, counts of plays, scores | card and game |
| game | a row of the server's table of games (`GET /games`): `id` (`iidx13`, `popn12`...), `title` | `id` |
| score | a chart's record: `song` and `chart` in the game's own numbers, `score`, `plays`, `clear` | game, card, song, chart |
| arcade | an operator's shop: `name`, `prefecture` (1 to 47, Japan's numbering, which the games show; 13 is Tokyo), `owner` | `id` (a number) |
| machine | a cabinet of an arcade: `pcbid` (20 hex digits), `name`, `enabled`, `has_key`, `last_seen` | `pcbid` (any case) |
| machine key | `hk2.PCBID.SECRET`: what the cabinet encrypts its calls with. Shown once, when made | — |
| setting | a game's option for its machines (an event, an unlock): a value of the arcade's, else the server's, else the default | game id and name |

How this maps to what the games send: a cabinet's game reads a card, sends its number and PIN, and
the server finds or makes that card's **player** in that game's own records. Players exist without
any account; they belong to cards, not to people. An **account links cards** (`POST /me/cards` with
the card's number and its PIN, as any of the card's games keeps it): from then on the account's stats
(`GET /me/stats`) are its cards' players, one entry a card and a game. Unlinking a card leaves its
players and scores on the server. One card numbering serves every game: one card can have a player in
pop'n music 12 and one in beatmania IIDX 13.

Players' names are given as the games keep them, made UTF-8 (most games keep Shift-JIS). A name that
can't be made text is `""` with its bytes beside it in hex (`player_hex`), wherever a `player` is sent.

A game object (`/server`'s and `/games`' items, `/games/{id}/top`, each stat) has `shows: {plays,
song_plays, clear}`: whether the game ever keeps a player's count of plays, a chart's own count, a
clear mark with a word for it. It is the game's, not one player's: leave out a column that can only be
empty. A score row's `clear` is the game's own number (`null`: none) and `clear_name` the mark as a
short word ("Cleared", "Failed", "Full combo"...), `null` when the game keeps none or the number's
meaning isn't known. The System 573 games keep a score for a cleared stage only: a failed one has
`score: null` and its mark. A score of 0 is in its player's stats, never among the best scores.

Songs are numbers. Their titles are the games' data, which HIRAKU doesn't ship: a server shows them
only from a names pack its owner made from their own games (SERVER.md, "Song names").
`GET /games/{id}/songs` gives the pack's titles (empty objects when there is none), and `has_names`
says whether there is one. An app should always fall back to the number.

**What never goes out:** a password, a PIN, a hash, a session, a token, a reset or invitation code,
or a machine key after the answer that made it; a card's number to anyone but its own account and
admins (another player's stats and the best scores have no card numbers); a path on the server's
disk.

## The areas

The table of every operation is in SERVER.md ("The API"); each one's fields, answers and errors are
in openapi.yaml. In short:

**Public** (no log-in): `GET /health` (`{ok, version, uptime}`; 503 when the database doesn't
answer), `GET /server` (the server's name, sign-up mode, games), `POST /signup`, `POST /login`,
`POST /reset` (a new password with an admin's reset code), `GET /openapi.yaml`, `GET /openapi.json`.

**One's own account**: `GET /me`, `POST /me/password`, `PUT /me/hide_stats`, `POST /logout`,
`GET/POST /me/tokens`, `DELETE /me/tokens/{id}`.

**Cards**: list, link, unlink one's own.

    curl -s -H "Authorization: Bearer $WRITE_TOKEN" -X POST $API/me/cards -d '{"card":"E0040000000000A1","pin":"1234"}'
    # 201 {"card":"E0040000000000A1"}
    # a wrong PIN: 403 {"error":"wrong_pin","message":"... 4 tries left ...","tries_left":4}
    # the fifth wrong one in a row: 403 {"error":"card_locked","locked":true,"tries_left":0,...}

The card must have played on this server (404 otherwise) and be no one's (409). A locked card stays
locked for linking until an admin unlocks it (`POST /cards/{card}/unlock`). The games go on reading
it as before; only linking is locked.

**Scores**: `GET /me/stats`, `GET /players` (accounts whose stats show), `GET /players/{name}/stats`
(403 `stats_hidden` when its owner hides them), `GET /games`, `GET /games/{id}/songs`,
`GET /games/{id}/top?song=&chart=&limit=` (best first, above 0, among players whose stats show).

**Arcades and machines** (operators: their own; admins: all): `GET/POST /arcades`,
`GET/PUT/DELETE /arcades/{id}`, `GET/POST /arcades/{id}/machines`, `GET /machines`,
`GET/PUT/DELETE /machines/{pcbid}`, `POST /machines/{pcbid}/key`,
`GET/PUT /arcades/{id}/settings`. Another operator's arcade or machine answers 404, as if it
weren't there.

    curl -s -H "Authorization: Bearer $OP_TOKEN" -X POST $API/arcades -d '{"name":"Example Arcade","prefecture":13}'
    curl -s -H "Authorization: Bearer $OP_TOKEN" -X POST $API/arcades/1/machines -d '{"name":"Front left"}'
    # 201 {..., "pcbid":"0120A1B2C3D4E5F60718", "key":"hk2.0120A1B2C3D4E5F60718...."}   (the key: this once only)
    curl -s -H "Authorization: Bearer $OP_TOKEN" -X PUT $API/arcades/1/settings \
         -d '{"settings":[{"game":"popn14","name":"net_event","value":3}]}'

A setting's `value` is sent as text or a whole number and given back as text; `null` clears the
arcade's own value (the server's applies, then the default). Each setting comes with what a form
needs: `label`, `help`, `kind` (`number` or `text`), `min` and `max`, `default`.

**Admins**: `GET /users`, `GET/PUT/DELETE /users/{name}`, `GET /users/{name}/cards`,
`POST /users/{name}/reset`, `GET/PUT /config` (name, sign-up mode, whether unlisted games show),
`POST /config/invites`, `GET /cards/{card}` (whether known, its account, its lock, its players),
`POST /cards/{card}/unlock`, `DELETE /cards/{card}`, `PUT /cards/{card}/pin`,
`GET/PUT /settings`, `GET /names`, `GET /audit?limit=`.

**Changes of several fields** (`PUT` of a user, an arcade, a machine, the config, a list of
settings) are made one after another, in the order the description gives. A refusal stops there and
is the answer; what was changed before it stays. Read the thing again after an error.

## Errors

Every error is JSON with its HTTP status:

    {"error": "not_found", "message": "no arcade 7"}

Act on `error`; `message` is for a person (plain English, may change). A refused card link adds
`tries_left`, and `locked: true` when the card is locked.

| Status | `error` | When |
|---|---|---|
| 400 | `bad_json` | the body isn't a JSON object (any method but GET) |
| 400 | `bad_request` | a field missing, of the wrong type, out of range, a value that isn't one (a name's, a password's shape...) |
| 401 | `not_signed_in` | no session or token where one is needed, or one that ended, was revoked, or whose account is disabled |
| 401 | `wrong_password` | `POST /login`: the name or the password (the same answer for both) |
| 403 | `session_only`, `must_change_password`, `read_only_token`, `csrf`, `not_allowed` | the checks above |
| 403 | `wrong_password` | `POST /me/password`: the old password |
| 403 | `disabled` | `POST /login` of a disabled account |
| 403 | `stats_hidden`, `signup_refused`, `bad_code`, `wrong_pin`, `card_locked` | the operation's own refusals |
| 404 | `not_found` | no such thing, one the caller may not see, a path the API hasn't, or a method a path hasn't |
| 409 | `conflict` | taken already, another's, the last admin, 50 tokens |
| 413 | `too_large` | a body over 64 KiB |
| 429 | `slow_down` | too many calls: wait `Retry-After` seconds |
| 500 | `server_error` | the database or the system failed |
| 503 | `read_only` | the database was written by a newer hiraku-server: this one changes nothing |

## Limits

| What | Limit |
|---|---|
| a request's body | 64 KiB (413) |
| calls | 600 a minute a session or token; 600 a minute an address with no log-in |
| `POST /login` | 10 a minute an address, and 10 a minute a name |
| `POST /reset` | 10 a minute an address (shared with log-in's) |
| `POST /signup` | 5 an hour an address |
| `POST /me/cards` | 20 an hour an account, successful links included |
| tokens | 50 an account |
| `/games/{id}/top` | `limit` 1 to 100, 20 by default |
| `/audit` | `limit` 1 to 10000, 100 by default |
| connections | read and write timeouts of 10 s, keep-alive 5 s and 100 requests |

A 429 has `Retry-After` (seconds). There is no paging: the other lists (users, players, arcades,
machines, stats, tokens) come whole. The limits on how often are counted in the server's memory and
start again when it restarts. Behind a proxy, serve must run with `--api-trust-proxy` to count each
caller's address rather than the proxy's.

The games' port has limits of its own (a card's userdata 128 KiB, and others: EAMUSE3.md); they
don't touch the API.

## Formats

- **Times**: whole seconds since 1970-01-01 UTC (`created`, `linked`, `last_seen`, `last_used`,
  `at`, `time`); `null` where something never happened (`last_seen`, `last_used`, a card's `linked`).
- **Numbers**: whole numbers. Song and chart numbers are the game's own; a score's scale is the
  game's (EX score, points...).
- **Text**: UTF-8. A name of an arcade, a machine, a token's label or the server's: 1 to 64 bytes of
  one line. A password: 8 to 256 bytes, one line. A PIN: four digits.
- **Path parameters**: URL-encode them. A card number and a PCBID are found in any case; a game id is
  exact. An arcade or token id that isn't a whole number answers 404.
- **Names in answers**: `GET /players/{name}/stats` and `GET /users/{name}/cards` give back `name`
  as it was in the path; `POST /me/cards` gives back `card` as it was sent. The stored forms are in
  `GET /me`, `GET /users`, `GET /me/cards`.
- **Answers** carry `Cache-Control: no-store` and `X-Content-Type-Options: nosniff`.

## What the API doesn't offer yet

- CORS, for browser apps on another origin (above).
- OAuth, refresh tokens, or a token made without a session.
- Paging, sorting or filtering beyond `top`'s and `audit`'s.
- Making a player or changing a score: the games make them, the API reads them.
- A player's own name or its games' options, rankings by period, NET TAISEN, events in progress.
- Removing one's own account (an admin does it), changing one's own name.
- Pushed events (websockets, webhooks): poll, within the limits.
- `ETag`, `If-None-Match`, `405 Method Not Allowed` (a method a path hasn't is 404).
- Rate limit headers other than `Retry-After`.

## A server to develop against

On Linux, from a checkout (README.md, "Building from source"; the server needs the system's SQLite 3
for CMake to make it), or with the server's package (SERVER.md, "Installing"):

    ninja -C build/cmake hiraku-server
    mkdir -p dev/empty
    build/cmake/hiraku-server import dev/empty --db dev/server.db      # prints the first admin's password
    build/cmake/hiraku-server serve --db dev/server.db --no-relay --api 127.0.0.1:8081 --api-insecure-cookies

Then `http://127.0.0.1:8081/` is the web pages and `http://127.0.0.1:8081/api/v1/` the API, on plain
HTTP (so `--api-insecure-cookies`; never on a server others reach). Log in as `admin` and change the
password (`POST /me/password`), make a player account, an operator (`PUT /users/NAME
{"role":"operator"}`), an arcade and a machine. Cards and scores come from games: play on a HIRAKU
cabinet pointed at this server (SERVER.md), or put made-up data in before `serve`, as the tests do
(`tests/eamuse/test_api.py`: `hiraku-server put cards --db dev/server.db` reads
`cardid=...<TAB>refid=...<TAB>pass=...` lines on its standard input). The API's tests are the most
complete examples of each call: `tests/eamuse/test_api.py`, `test_openapi.py`.

## Generating a client

The description is plain OpenAPI 3.1 with no tool-specific parts but `x-hiraku-*` notes. For
example, with OpenAPI Generator (not tried here):

    curl -s http://127.0.0.1:8081/api/v1/openapi.yaml -o openapi.yaml
    openapi-generator-cli generate -i openapi.yaml -g typescript-fetch -o hiraku-client

Each operation's `operationId` (`logIn`, `getMyStats`, `linkCard`, `createMachine`...) becomes the
client's method name. Things to set up by hand in a generated client: the bearer token
(`bearerToken`), or for a session the cookie and the `X-HK-CSRF` header on changes; reading `error`
from a non-2xx answer (schema `Error`); `Retry-After` on 429. Tools that only take OpenAPI 3.0 may
stumble on 3.1's `type: [string, "null"]`.

## Changing the API

For HIRAKU's own developers: a route added, removed or changed in `server/api.cpp` is changed in
`docs/openapi.yaml` too, then `tools/eamuse/openapi.py` writes the JSON twin. `eamuse_openapi`
fails when the routes and the description differ, when a role differs, or when an answer of the
tests doesn't fit its schema.
