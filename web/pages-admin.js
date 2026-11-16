// SPDX-License-Identifier: GPL-3.0-or-later
// An admin's pages: users, sign-up and invitations, cards, the server's settings, the audit log.
import {del, enc, get, post, put} from './api.js';
import {state} from './app.js';
import {settingsForm} from './settings.js';
import {
  ask, button, card, cardNumber, chip, clear, confirm, empty, errorNote, field, form, h, loading, note, pageHead, secret, table, toast,
  toggle, value, when,
} from './ui.js';

// --- Users

export async function users(view, {still}) {
  view.append(pageHead('Users', 'Every account: its role, whether it may sign in, and a reset code for a forgotten password.'));
  const shown = h('div');
  const list = h('div', {}, loading());
  const find = h('input', {type: 'search', placeholder: 'Find a name', 'aria-label': 'Find a user by name'});
  const said = h('div', {'aria-live': 'polite'});
  view.append(shown, card(null, h('div', {class: 'toolbar'}, find), said, list));
  let all = [];
  find.addEventListener('input', fill);
  async function act(fn) {
    clear(said);
    try { await fn(); } catch (err) { said.append(errorNote(err)); }
    await draw();   // (a refused change drawn back as it is)
  }
  function fill() {
    const q = find.value.trim().toLowerCase();
    const rows = all.filter((u) => u.name.toLowerCase().includes(q));
    clear(list).append(rows.length ? table([
      {key: 'name', label: 'Name', cell: (u) => h('span', {}, h('strong', {text: u.name}), u.name === state.me.name ? h('span', {class: 'muted small', text: ' (you)'}) : null,
        u.must_change_password ? chip('must change password', 'warn') : null)},
      {key: 'role', label: 'Role', wide: true, sort: (u) => ({player: 1, operator: 2, admin: 3})[u.role], cell: (u) => {
        const s = h('select', {'aria-label': `Role of ${u.name}`}, ['player', 'operator', 'admin'].map((r) => h('option', {value: r, text: r, selected: r === u.role})));
        s.addEventListener('change', () => act(async () => {
          await put('/users/' + enc(u.name), {role: s.value});
          toast(`${u.name} is now ${s.value === 'admin' ? 'an admin' : s.value === 'operator' ? 'an operator' : 'a player'}.`);
        }));
        return s;
      }},
      {key: 'disabled', label: 'Sign-in', sort: (u) => (u.disabled ? 1 : 0), cell: (u) => chip(u.disabled ? 'disabled' : 'allowed', u.disabled ? 'bad' : 'ok')},
      {key: 'hide_stats', label: 'Stats', sort: (u) => (u.hide_stats ? 1 : 0), cell: (u) => (u.hide_stats ? 'hidden' : 'shown')},
      {key: 'created', label: 'Since', wide: true, cell: (u) => when(u.created)},
      {key: 'name', label: 'Actions', sortable: false, cell: (u) => h('div', {class: 'row-actions'},
        button(u.disabled ? 'Enable' : 'Disable', 'small', () => act(async () => {
          if (!u.disabled && !await confirm(`Disable ${u.name}?`, 'Disable', true, 'Their sessions and tokens stop at once; nothing of theirs is removed.')) return;
          await put('/users/' + enc(u.name), {disabled: !u.disabled});
          toast(`${u.name} ${u.disabled ? 'enabled' : 'disabled'}.`);
        })),
        button('Reset code', 'small', () => act(async () => {
          if (!await confirm(`A reset code for ${u.name}?`, 'Make a code', false, 'They set a new password with it, once, within a day. Give it to them yourself.')) return;
          const j = await post('/users/' + enc(u.name) + '/reset');
          clear(shown).append(secret(`${u.name}: reset code`, j.code, `They enter it under “I have a reset code” on the sign-in page, within ${j.hours} hours.`));
        })),
        button('Delete', 'small-quiet', () => act(async () => {
          if (!await confirm(`Delete ${u.name}?`, 'Delete the account', true, 'Their cards are unlinked; the cards’ players and scores stay on the server. This can’t be undone.')) return;
          await del('/users/' + enc(u.name));
          toast(`${u.name} deleted.`);
        })))},
    ], rows, {sort: 0, caption: 'Users', class: 'users'}) : empty('Nobody by that name.'));
  }
  async function draw() {
    const j = await get('/users');
    if (!still()) return;
    all = j.users;
    fill();
  }
  await draw();
}

// --- Sign-up and invitations

export async function signUp(view, {still}) {
  view.append(pageHead('Sign-up', 'Who may make an account on this server.'));
  const c = await get('/config');
  if (!still()) return;
  const WORDS = {open: 'Open: anyone who reaches the server', invite: 'By invitation: with a code from an admin', closed: 'Closed: admins make accounts'};
  const modeOut = h('div', {'aria-live': 'polite'});
  const radios = h('fieldset', {class: 'radios'}, h('legend', {text: 'Sign-up is'}), Object.entries(WORDS).map(([k, t]) => {
    const id = 'mode-' + k;
    const r = h('input', {type: 'radio', name: 'mode', id, value: k, checked: c.signup === k});
    r.addEventListener('change', async () => {
      clear(modeOut);
      try {
        const j = await put('/config', {signup: k});
        state.server.signup = j.signup;
        toast('Sign-up: ' + WORDS[j.signup].split(':')[0].toLowerCase() + '.');
      } catch (err) { modeOut.append(errorNote(err)); }
    });
    return h('div', {class: 'radio'}, r, h('label', {for: id, text: t}));
  }));
  const shown = h('div');
  const inv = form([
    field('Uses', 'uses', {type: 'number', min: 1, max: 10000, value: 1, required: true, help: 'How many accounts it may make.'}),
    field('Days', 'days', {type: 'number', min: 1, max: 365, value: 7, required: true, help: 'How long it works.'}),
  ], 'Make an invitation', async (f) => {
    const j = await post('/config/invites', {uses: parseInt(value(f, 'uses'), 10), days: parseInt(value(f, 'days'), 10)});
    clear(shown).append(secret('Invitation code', j.code,
      `It makes ${j.uses} account${j.uses === 1 ? '' : 's'} within ${j.days} day${j.days === 1 ? '' : 's'}, entered on the Sign up page.`));
  }, {class: 'form-row'});
  view.append(h('div', {class: 'grid-2'}, card('Mode', radios, modeOut),
    card('Invitations', h('p', {class: 'muted', text: 'A code for sign-up by invitation. Only a code’s fingerprint is kept: copy it now.'}), inv)), shown);
}

// --- Cards

export async function cards(view) {
  view.append(pageHead('Cards', 'A card locked by wrong PINs, a card linked to the wrong account, a forgotten PIN.'));
  const out = h('div');
  const byUser = form([field('User', 'name', {required: true, maxlength: 32, help: 'Their account’s name: their linked cards.'})], 'Find', async (f) => {
    const j = await get('/users/' + enc(value(f, 'name')) + '/cards');
    clear(out);
    out.append(card(`${j.name}’s cards`, j.cards.length ? h('ul', {class: 'cards'}, j.cards.map((c) => cardRow(c.card)))
      : empty('No card is linked to this account.')));
  }, {class: 'form-row'});
  const byNumber = form([field('Card number', 'card', {required: true, maxlength: 24, help: 'The 16 letters and digits the games show. Spaces are fine.'})], 'Open', async (f) => {
    clear(out).append(card('A card', h('ul', {class: 'cards'}, cardRow(value(f, 'card').replace(/[\s-]/g, '')))));
  }, {class: 'form-row'});
  view.append(h('div', {class: 'grid-2'}, card('Find by user', byUser), card('Find by number', byNumber)), out);

  // A card: what the server knows of it first (GET /cards/CARD), then what may be done to it
  function cardRow(number) {
    const facts = h('div', {}, loading());
    const said = h('div', {'aria-live': 'polite'});
    const act = async (fn) => {
      clear(said);
      try { const n = await fn(); if (n) said.append(n); } catch (err) { said.append(errorNote(err)); }
      await show();   // (drawn again as it now is)
    };
    async function show() {
      let c;
      try { c = await get('/cards/' + enc(number)); } catch (err) { clear(facts).append(errorNote(err)); return; }
      clear(facts).append(
        h('p', {class: 'chips'},
          chip(c.known ? 'played on this server' : 'not known to this server', c.known ? 'ok' : 'warn'),
          c.locked ? chip('locked for linking', 'bad') : chip(`${c.tries_left} wrong PIN${c.tries_left === 1 ? '' : 's'} before the lock`, c.wrong_pins ? 'warn' : null)),
        c.user ? h('p', {class: 'muted small'}, `Linked to ${c.user} `, when(c.linked)) : h('p', {class: 'muted small', text: 'Linked to no account.'}),
        c.wrong_pins ? h('p', {class: 'muted small', text: `Wrong PINs so far: ${c.wrong_pins}.`}) : null,
        c.games.length ? h('p', {class: 'chips', 'aria-label': 'Games with a player on this card'}, c.games.map((g) => chip(`${g.title}: ${g.player || '(no name)'}`, 'game')))
          : h('p', {class: 'muted small', text: 'No game has a player on this card.'}));
    }
    show();
    return h('li', {class: 'card-item'}, h('div', {class: 'card-face', 'aria-hidden': 'true'}),
      h('div', {class: 'card-body'}, cardNumber(number), facts,
        h('div', {class: 'row-actions'},
          button('Unlock', 'small', () => act(async () => {
            await post('/cards/' + enc(number) + '/unlock');
            return note('ok', 'Unlocked: it may be linked again.');
          })),
          button('Unlink', 'small', () => act(async () => {
            if (!await confirm('Unlink this card from its account?', 'Unlink', true, 'Its players and scores stay; the account loses them until it links the card again.')) return null;
            await del('/cards/' + enc(number));
            return note('ok', 'Unlinked.');
          })),
          button('Reset PIN', 'small', () => act(async () => {
            const pin = await ask('A new PIN for this card', 'New PIN', {type: 'password', inputmode: 'numeric', pattern: '[0-9]{4}', maxlength: 4, required: true,
              help: 'Four digits, set in every game that keeps a PIN for it.', ok: 'Set the PIN'});
            if (pin === null) return null;
            const j = await put('/cards/' + enc(number) + '/pin', {pin});
            return note(j.complete ? 'ok' : 'warn', j.complete ? `PIN set (${j.changed.join(', ') || 'no game'}).`
              : `PIN set in ${j.changed.join(', ')}; not in ${j.not_changed.join(', ')}.`);
          }))),
        said));
  }
}

// --- the server's settings

export async function settings(view, {still}) {
  view.append(pageHead('Server settings', 'The server’s name, which games are listed, its own settings, and every game’s default for the arcades.'));
  const [c, s] = await Promise.all([get('/config'), get('/settings')]);
  if (!still()) return;
  const name = form([field('Server name', 'name', {required: true, maxlength: 64, value: c.name, help: 'Shown on these pages’ sign-in and title.'})], 'Save', async (f) => {
    const j = await put('/config', {name: value(f, 'name')});
    state.server.name = j.name;
    document.getElementById('server-name').textContent = j.name;
    return note('ok', 'Saved.');
  }, {class: 'form-row'});
  const all = toggle('List every game', c.list_all_games, 'Also the games marked unlisted: their rows show in the lists, stats and settings. It changes no game’s answers.');
  const allOut = h('div', {'aria-live': 'polite'});
  all.input.addEventListener('change', async () => {
    clear(allOut);
    try {
      await put('/config', {list_all_games: all.input.checked});
      toast(all.input.checked ? 'Every game is listed.' : 'Unlisted games are hidden.');
      const again = await get('/settings');
      drawGames(again);
    } catch (err) {
      all.input.checked = !all.input.checked;
      allOut.append(errorNote(err));
    }
  });
  const own = h('div');
  const gamesBox = h('div');
  view.append(h('div', {class: 'grid-2'}, card('This server', name), card('Games listed', all, allOut)),
    card('The server’s own', own), card('Each game’s defaults', h('p', {class: 'muted', text: 'An arcade’s own value wins over these; a game’s default holds where neither is set.'}), gamesBox));
  const save = (game, list) => put('/settings', {settings: list});
  function drawGames(j) {
    clear(own).append(j.server && j.server.length
      ? settingsForm([{id: 'server', title: 'The server', settings: j.server}], {arcade: false, open: ['server'], save: async (g, list) => {
        const a = await save(g, list);
        return {games: [{id: 'server', title: 'The server', settings: a.server || []}]};
      }})
      : empty('The server has no settings of its own.'));
    clear(gamesBox).append(j.games.length ? settingsForm(j.games, {arcade: false, save}) : empty('No game has settings.'));
  }
  drawGames(s);
}

// --- the audit log

export async function audit(view, {still, query}) {
  view.append(pageHead('Audit log', 'Every change: when, who, what. Never a password, PIN or code.'));
  const limit = ['100', '500', '2000'].includes(query.get('limit')) ? query.get('limit') : '100';
  const pick = h('select', {id: 'audit-limit'}, ['100', '500', '2000'].map((n) => h('option', {value: n, text: `The last ${n}`, selected: n === limit})));
  pick.addEventListener('change', () => { location.hash = '#/admin/audit?limit=' + pick.value; });
  const find = h('input', {type: 'search', placeholder: 'Find a user, action or word', 'aria-label': 'Find in the audit log'});
  const holder = h('div', {}, loading());
  view.append(card(null, h('div', {class: 'toolbar'}, h('label', {for: 'audit-limit', class: 'sr', text: 'How many'}), pick, find), holder));
  const j = await get('/audit?limit=' + limit);
  if (!still()) return;
  const fill = () => {
    const q = find.value.trim().toLowerCase();
    const rows = j.audit.filter((l) => !q || `${l.user} ${l.action} ${l.detail}`.toLowerCase().includes(q));
    clear(holder).append(rows.length ? table([
      {key: 'at', label: 'When', num: true, cell: (l) => when(l.at)},
      {key: 'user', label: 'Who'},
      {key: 'action', label: 'Action', cell: (l) => h('code', {text: l.action})},
      {key: 'detail', label: 'Detail', cls: 'wrap', wide: true},
    ], rows, {sort: 0, desc: true, caption: 'Audit log', class: 'audit'}) : empty('Nothing matches.'));
  };
  find.addEventListener('input', fill);
  fill();
}
