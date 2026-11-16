// SPDX-License-Identifier: GPL-3.0-or-later
// Every user's pages: Home (cards), My stats, Players, Games, Account. Songs are shown by their
// number: the server knows no titles.
import {del, enc, get, post, put} from './api.js';
import {state} from './app.js';
import {
  ago, button, card, cardNumber, chip, clear, confirm, empty, errorNote, field, form, h, loading, note, pageHead, playerName, secret,
  sentence, table, toast, toggle, value, when,
} from './ui.js';

// --- Home: my cards

export async function home(view, {still}) {
  view.append(pageHead(`Hello, ${state.me.name}`, 'Your cards on this server. A card you link brings its players and scores to your account.'));
  const list = h('div', {}, loading());
  view.append(h('div', {class: 'grid-2'}, card('My cards', list), linkCard(() => draw())));
  async function draw() {
    const [cards, stats] = await Promise.all([get('/me/cards'), get('/me/stats')]);
    if (!still()) return;
    clear(list);
    if (!cards.cards.length) {
      list.append(empty('No card is linked yet. Link one with its number and PIN.'));
      return;
    }
    const ul = h('ul', {class: 'cards'});
    for (const c of cards.cards) {
      const games = stats.stats.filter((s) => s.card && s.card.toLowerCase() === c.card.toLowerCase());
      ul.append(h('li', {class: 'card-item'},
        h('div', {class: 'card-face', 'aria-hidden': 'true'}),
        h('div', {class: 'card-body'},
          h('div', {class: 'card-top'}, cardNumber(c.card), unlink(c)),
          h('p', {class: 'muted small'}, 'Linked ', when(c.linked)),
          games.length ? h('p', {class: 'chips', 'aria-label': 'Games with a player on this card'}, games.map((g) => chip(g.title, 'game')))
            : h('p', {class: 'muted small', text: 'No game has a player on it yet.'}))));
    }
    list.append(ul);
  }
  function unlink(c) {
    return button('Unlink', 'quiet', async () => {
          if (!await confirm('Unlink this card?', 'Unlink', true, 'Its players and scores stay on the server; they leave your account until you link it again.')) return;
          try {
            await del('/me/cards/' + enc(c.card));
            toast('Card unlinked.');
            draw();
          } catch (err) { list.prepend(errorNote(err)); }
        }, {'aria-label': `Unlink card ${c.card}`});
  }
  await draw();
}

function linkCard(done) {
  const f = form([
    field('Card number', 'card', {required: true, autocomplete: 'off', maxlength: 24, help: 'The 16 letters and digits the games show for the card. Spaces are fine.'}),
    field('PIN', 'pin', {type: 'password', required: true, inputmode: 'numeric', pattern: '[0-9]{4}', maxlength: 4, autocomplete: 'off',
      help: 'The four digits you enter at the cabinet with this card.'}),
  ], 'Link the card', async (f) => {
    try {
      await post('/me/cards', {card: value(f, 'card').replace(/[\s-]/g, ''), pin: value(f, 'pin')});
    } catch (err) {
      if (err.triesLeft === undefined) throw err;
      // (a wrong PIN: how many more before the card is locked for linking, or that it is)
      return h('div', {}, errorNote(err), err.code === 'card_locked' || err.triesLeft === 0
        ? note('warn', 'The card is locked for linking: an admin of this server unlocks it.')
        : note('warn', `${err.triesLeft} ${err.triesLeft === 1 ? 'try' : 'tries'} left before the card is locked for linking.`));
    }
    f.reset();
    toast('Card linked.');
    done();
    return note('ok', 'Card linked.');
  });
  return card('Link a card', h('p', {class: 'muted', text: 'A card that has played on this server, with any game’s PIN for it. Five wrong PINs lock the card for linking until an admin unlocks it.'}), f);
}

// --- stats: a game picker, the player, the scores

function statsView(view, stats, query, base) {
  if (!stats.length) {
    view.append(card(null, empty('No game has a player on these cards yet. Play with a linked card and the scores show here.')));
    return;
  }
  let pick = Math.min(Math.max(parseInt(query.get('g') || '0', 10) || 0, 0), stats.length - 1);
  const label = (s) => `${s.title}${s.player ? ' · ' + s.player : ''}`;
  const sel = h('select', {id: 'game-pick'}, stats.map((s, i) => h('option', {value: String(i), text: label(s), selected: i === pick})));
  const body = h('div');
  sel.addEventListener('change', () => {
    pick = parseInt(sel.value, 10);
    history.replaceState(null, '', `#/${base}?g=${pick}`);
    draw();
  });
  view.append(h('div', {class: 'toolbar'}, h('label', {for: 'game-pick', text: 'Game'}), sel), body);
  function draw() {
    clear(body);
    const s = stats[pick];
    const best = s.scores.reduce((m, x) => (x.score !== null && x.score > m ? x.score : m), 0);
    body.append(h('div', {class: 'facts'},
      fact('Player', playerName(s) || h('span', {class: 'none', text: '–'})),
      fact('Plays', s.plays ?? '–'),
      fact('Songs with a score', new Set(s.scores.map((x) => x.song)).size),
      fact('Best score', best || '–'),
      s.card ? fact('Card', cardNumber(s.card)) : null));
    if (!s.scores.length) {
      body.append(card(null, empty('No scores kept for this game yet.')));
      return;
    }
    const find = h('input', {type: 'search', id: 'song-find', inputmode: 'numeric', placeholder: 'Song number', 'aria-label': 'Show one song number'});
    const holder = h('div');
    const fill = () => {
      const q = find.value.trim();
      const rows = q ? s.scores.filter((x) => String(x.song).startsWith(q)) : s.scores;
      clear(holder).append(rows.length ? scoresTable(rows) : empty(`No song number starting ${q}.`));
    };
    find.addEventListener('input', fill);
    body.append(card('Scores', h('div', {class: 'toolbar'}, find, h('span', {class: 'muted small', text: `${s.scores.length} charts`})), holder));
    fill();
  }
  draw();
}

function fact(k, v) {
  return h('div', {class: 'fact'}, h('span', {class: 'fact-k', text: k}), h('span', {class: 'fact-v'}, v));
}

export const clearMark = (c) => (c === null || c === undefined ? null : h('span', {class: ['clear', `clear-${Math.min(Math.max(c, 0), 9)}`], title: `Clear type ${c}`, text: String(c)}));

function scoresTable(rows) {
  return table([
    {key: 'song', label: 'Song', num: true},
    {key: 'chart', label: 'Chart', num: true},
    {key: 'score', label: 'Score', num: true},
    {key: 'clear', label: 'Clear', num: true, cell: (r) => clearMark(r.clear)},
    {key: 'plays', label: 'Plays', num: true},
  ], rows, {sort: 0, desc: false, caption: 'Scores'});
}

export async function myStats(view, {query, still}) {
  view.append(pageHead('My stats', 'Each game a linked card has a player in: the player, and every score the server keeps.'));
  const j = await get('/me/stats');
  if (!still()) return;
  statsView(view, j.stats, query, 'stats');
}

// --- Players

export async function players(view, {still}) {
  view.append(pageHead('Players', 'The people on this server who show their stats.'));
  const j = await get('/players');
  if (!still()) return;
  if (!j.players.length) {
    view.append(card(null, empty('Nobody shows their stats yet.')));
    return;
  }
  const find = h('input', {type: 'search', placeholder: 'Find a name', 'aria-label': 'Find a player by name'});
  const ul = h('ul', {class: 'people'});
  const fill = () => {
    const q = find.value.trim().toLowerCase();
    clear(ul);
    const list = j.players.filter((p) => p.name.toLowerCase().includes(q));
    for (const p of list) ul.append(h('li', {}, h('a', {href: '#/players/' + enc(p.name), class: 'person'}, h('span', {class: 'avatar', 'aria-hidden': 'true', text: p.name.slice(0, 1).toUpperCase()}), h('span', {text: p.name}))));
    if (!list.length) ul.append(h('li', {class: 'muted', text: 'Nobody by that name.'}));
  };
  find.addEventListener('input', fill);
  view.append(card(null, h('div', {class: 'toolbar'}, find, h('span', {class: 'muted small', text: `${j.players.length} players`})), ul));
  fill();
}

export async function playerStats(view, {params, query, still}) {
  view.append(h('p', {class: 'crumbs'}, h('a', {href: '#/players', text: 'Players'})), pageHead(params.name, 'Their games and scores.'));
  try {
    const j = await get('/players/' + enc(params.name) + '/stats');
    if (!still()) return;
    statsView(view, j.stats, query, 'players/' + enc(params.name));
  } catch (err) {
    if (err.status === 403 || err.status === 404) view.append(note('info', sentence(err.message)));
    else throw err;
  }
}

// --- Games and their best scores

export async function games(view, {still}) {
  view.append(pageHead('Games', 'The games this server answers. Pick one for its best scores among the players who show theirs.'));
  const j = await get('/games');
  if (!still()) return;
  const ul = h('ul', {class: 'tiles'});
  for (const g of j.games) {
    ul.append(h('li', {}, h('a', {href: '#/games/' + enc(g.id), class: ['tile', !g.scores && 'tile-quiet']},
      h('span', {class: 'tile-title', text: g.title}), h('span', {class: 'tile-sub', text: g.scores ? 'Best scores' : 'Scores not read yet'}))));
  }
  view.append(ul);
}

export async function gameTop(view, {params, query, still}) {
  const g = (await get('/games')).games.find((x) => x.id === params.id);
  if (!still()) return;
  view.append(h('p', {class: 'crumbs'}, h('a', {href: '#/games', text: 'Games'})), pageHead(g ? g.title : params.id, 'Best scores among the players who show theirs, best first.'));
  if (g && !g.scores) view.append(note('info', 'The server does not read this game’s scores yet.'));
  const out = h('div');
  const f = form([
    field('Song number', 'song', {type: 'number', min: 0, inputmode: 'numeric', value: query.get('song') || '', help: 'Empty: every song.'}),
    field('Chart', 'chart', {type: 'number', min: 0, inputmode: 'numeric', value: query.get('chart') || '', help: 'Empty: every chart.'}),
    field('How many', 'limit', {options: [[10, '10'], [20, '20'], [50, '50'], [100, '100']], value: query.get('limit') || '20'}),
  ], 'Show', async (f) => {
    const q = new URLSearchParams();
    for (const k of ['song', 'chart', 'limit']) if (value(f, k)) q.set(k, value(f, k));
    history.replaceState(null, '', `#/games/${enc(params.id)}?${q}`);
    await draw(q);
  }, {class: 'form-row'});
  view.append(card(null, f), out);
  async function draw(q) {
    clear(out).append(loading());
    const j = await get(`/games/${enc(params.id)}/top?${q}`);
    clear(out);
    if (!j.scores.length) {
      out.append(card(null, empty('No scores for that yet.')));
      return;
    }
    out.append(card(null, table([
      {key: 'rank', label: 'Rank', num: true},
      {key: 'user', label: 'Player', wide: true, sort: (r) => r.user || r.player || '', cell: (r) => r.user
        ? h('span', {}, h('a', {href: '#/players/' + enc(r.user), text: r.user}), r.player ? h('span', {class: 'muted small', text: ` · ${r.player}`}) : null)
        : playerName(r)},
      {key: 'song', label: 'Song', num: true},
      {key: 'chart', label: 'Chart', num: true},
      {key: 'score', label: 'Score', num: true},
      {key: 'clear', label: 'Clear', num: true, cell: (r) => clearMark(r.clear)},
    ], j.scores, {sort: 0, caption: 'Best scores'})));
  }
  const q = new URLSearchParams();
  for (const k of ['song', 'chart', 'limit']) if (query.get(k)) q.set(k, query.get(k));
  try { await draw(q); } catch (err) { clear(out).append(errorNote(err)); }
}

// --- Account: password, hidden stats, personal tokens

export async function account(view, {still}) {
  view.append(pageHead('Account', `Signed in as ${state.me.name} (${state.me.role}).`));
  const pw = form([
    field('Password now', 'old', {type: 'password', required: true, autocomplete: 'current-password'}),
    field('New password', 'password', {type: 'password', required: true, autocomplete: 'new-password', help: '8 characters at least.'}),
    field('New password again', 'again', {type: 'password', required: true, autocomplete: 'new-password'}),
  ], 'Change the password', async (f) => {
    const password = f.elements.namedItem('password').value;
    if (password !== f.elements.namedItem('again').value) return note('error', 'The two new passwords are not the same.');
    await post('/me/password', {old_password: f.elements.namedItem('old').value, password});
    f.reset();
    return note('ok', 'Password changed.');
  });

  const hide = toggle('Hide my stats from other players', state.me.hide_stats, 'When on, others don’t see you in Players or among a game’s best scores. You still see your own.');
  const hideOut = h('div', {'aria-live': 'polite'});
  hide.input.addEventListener('change', async () => {
    clear(hideOut);
    hide.input.disabled = true;
    try {
      const j = await put('/me/hide_stats', {hide_stats: hide.input.checked});
      state.me.hide_stats = j.hide_stats;
      toast(j.hide_stats ? 'Your stats are hidden.' : 'Your stats show.');
    } catch (err) {
      hide.input.checked = !hide.input.checked;
      hideOut.append(errorNote(err));
    } finally { hide.input.disabled = false; }
  });

  const tokens = h('div', {}, loading());
  const made = h('div');
  const mk = form([
    field('Label', 'label', {required: true, maxlength: 64, help: 'What it is for: “phone app”, “score script”.'}),
    field('It may', 'scope', {options: [['read', 'Read only'], ['write', 'Read and change']], value: 'read'}),
  ], 'Make a token', async (f) => {
    const j = await post('/me/tokens', {label: value(f, 'label'), scope: value(f, 'scope')});
    f.reset();
    clear(made).append(secret(`Token “${j.label}”`, j.token, 'An app or a script sends it as “Authorization: Bearer” and the token. Keep it like a password.'));
    drawTokens();
  });

  view.append(h('div', {class: 'grid-2'}, card('Password', pw), card('Privacy', hide, hideOut)),
    card('Personal tokens', h('p', {class: 'muted', text: 'For an app or a script that reads your stats. A token works until you revoke it; a read-only one can change nothing.'}),
      made, tokens, h('h3', {text: 'A new token'}), mk));

  async function drawTokens() {
    const j = await get('/me/tokens');
    if (!still()) return;
    clear(tokens);
    if (!j.tokens.length) {
      tokens.append(empty('No tokens.'));
      return;
    }
    tokens.append(table([
      {key: 'label', label: 'Label'},
      {key: 'scope', label: 'May', cell: (t) => chip(t.scope === 'write' ? 'read and change' : 'read only', t.scope === 'write' ? 'warn' : '')},
      {key: 'created', label: 'Made', wide: true, cell: (t) => when(t.created)},
      {key: 'last_used', label: 'Last used', wide: true, cell: (t) => ago(t.last_used)},
      {key: 'id', label: '', sortable: false, cell: (t) => button('Revoke', 'quiet', async () => {
        if (!await confirm(`Revoke “${t.label}”?`, 'Revoke', true, 'Whatever uses it stops working at once.')) return;
        try {
          await del('/me/tokens/' + t.id);
          toast('Token revoked.');
          drawTokens();
        } catch (err) { tokens.prepend(errorNote(err)); }
      }, {'aria-label': `Revoke ${t.label}`})},
    ], j.tokens, {caption: 'Personal tokens'}));
  }
  await drawTokens();
}
