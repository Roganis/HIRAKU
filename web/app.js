// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-server's pages (docs/SERVER.md): one page, hash routes (#/stats), drawn from the JSON API.
// Who sees which pages follows GET /me's role: a player's, then an operator's arcades, then an admin's
// server. Nothing is loaded from anywhere but this server.
import {get, onSignedOut, post, setCsrf} from './api.js';
import {clear, h, note, toast} from './ui.js';
import * as signin from './pages-signin.js';
import * as player from './pages-player.js';
import * as operator from './pages-operator.js';
import * as admin from './pages-admin.js';

export const state = {server: null, me: null};

// [hash pattern, page, who: 'out' | 'change' | 'player' | 'operator' | 'admin', section]
const ROUTES = [
  ['signin', signin.signIn, 'out'],
  ['signup', signin.signUp, 'out'],
  ['reset', signin.reset, 'out'],
  ['password', signin.mustChange, 'change'],
  ['home', player.home, 'player', 'play'],
  ['stats', player.myStats, 'player', 'play'],
  ['players', player.players, 'player', 'play'],
  ['players/:name', player.playerStats, 'player', 'play'],
  ['games', player.games, 'player', 'play'],
  ['games/:id', player.gameTop, 'player', 'play'],
  ['account', player.account, 'player', 'account'],
  ['arcades', operator.arcades, 'operator', 'arcades'],
  ['arcades/:id', operator.machines, 'operator', 'arcades'],
  ['arcades/:id/settings', operator.arcadeSettings, 'operator', 'arcades'],
  ['admin/users', admin.users, 'admin', 'server'],
  ['admin/signup', admin.signUp, 'admin', 'server'],
  ['admin/cards', admin.cards, 'admin', 'server'],
  ['admin/settings', admin.settings, 'admin', 'server'],
  ['admin/audit', admin.audit, 'admin', 'server'],
];

const NAV = [
  ['Play', 'play', [['home', 'Home'], ['stats', 'My stats'], ['players', 'Players'], ['games', 'Games']], 'player'],
  ['Arcades', 'arcades', [['arcades', 'My arcades']], 'operator'],
  ['Server', 'server', [['admin/users', 'Users'], ['admin/signup', 'Sign-up'], ['admin/cards', 'Cards'], ['admin/settings', 'Server settings'],
    ['admin/audit', 'Audit log']], 'admin'],
  ['You', 'account', [['account', 'Account']], 'player'],
];

const RANK = {player: 1, operator: 2, admin: 3};
const may = (who) => state.me && !state.me.must_change_password && RANK[state.me.role] >= RANK[who];

function match(pattern, path) {
  const a = pattern.split('/'), b = path.split('/');
  if (a.length !== b.length) return null;
  const params = {};
  for (let i = 0; i < a.length; i++) {
    if (a[i].startsWith(':')) params[a[i].slice(1)] = decodeURIComponent(b[i]);
    else if (a[i] !== b[i]) return null;
  }
  return params;
}

export function go(path) {
  if (location.hash === '#/' + path) route();
  else location.hash = '#/' + path;
}

// the hash's path and its ?query
function where() {
  const raw = location.hash.replace(/^#\/?/, '');
  const q = raw.indexOf('?');
  return {path: q < 0 ? raw : raw.slice(0, q), query: new URLSearchParams(q < 0 ? '' : raw.slice(q + 1))};
}

let drawing = 0;

export async function route() {
  const {path, query} = where();
  const main = document.getElementById('main');
  let found = null, params = {};
  for (const r of ROUTES) {
    const p = match(r[0], path);
    if (p) { found = r; params = p; break; }
  }
  // who may see it: else where they belong
  const home = !state.me ? 'signin' : state.me.must_change_password ? 'password' : 'home';
  if (!found || (found[2] === 'out' && state.me) || (found[2] === 'change' && !(state.me && state.me.must_change_password)) ||
      (!['out', 'change'].includes(found[2]) && !may(found[2]))) {
    if (path !== home) location.replace('#/' + home);
    return;
  }
  drawNav(found ? found[3] : null, path);
  document.body.dataset.section = found && found[3] ? found[3] : 'none';
  document.body.classList.toggle('signed-out', !state.me || !!state.me.must_change_password);
  const mine = ++drawing;
  clear(main);
  const view = h('div', {class: 'view'});
  main.append(view);
  closeMenu();
  try {
    await found[1](view, {params, query, state, go, still: () => mine === drawing});
  } catch (err) {
    if (mine !== drawing) return;
    if (err && err.status === 401) return;   // (signedOut has taken it)
    view.append(note('error', err && err.message ? 'The page could not be drawn: ' + err.message : 'The page could not be drawn.'));
  }
  if (mine !== drawing) return;
  const title = view.querySelector('h1');
  document.title = `${title ? title.textContent + ' · ' : ''}${state.server ? state.server.name : 'HIRAKU'}`;
  if (title && moved) title.focus({preventScroll: true});
  moved = true;
}
let moved = false;

function link(path, text, current) {
  return h('a', {href: '#/' + path, class: current ? 'current' : null, 'aria-current': current ? 'page' : null, text});
}

function drawNav(section, path) {
  const nav = clear(document.getElementById('nav'));
  const who = clear(document.getElementById('who'));
  const menu = document.getElementById('menu');
  menu.hidden = !state.me;
  if (!state.me) return;
  who.append(h('span', {class: 'who-name', text: state.me.name}), h('span', {class: ['role', `role-${state.me.role}`], text: state.me.role}));
  if (!state.me.must_change_password) {
    for (const [label, sec, links, need] of NAV) {
      if (!may(need)) continue;
      nav.append(h('div', {class: ['nav-group', `sec-${sec}`]}, h('p', {class: 'nav-head', text: label}),
        h('ul', {}, links.map(([p, t]) => [p, p === 'arcades' && state.me.role === 'admin' ? 'All arcades' : t]).map(([p, t]) => h('li', {}, link(p, t, path === p || (p !== 'home' && path.startsWith(p + '/'))))))));
    }
  }
  nav.append(h('div', {class: 'nav-group nav-out'}, h('button', {type: 'button', class: 'btn btn-quiet', on: {click: signOut}}, 'Sign out')));
}

async function signOut() {
  try { await post('/logout'); } catch { /* (gone either way) */ }
  state.me = null;
  setCsrf('');
  go('signin');
  toast('Signed out.');
}

// The server's name beside the logo: none when it is the logo's own word
export function showServerName(name) {
  const n = String(name || '').trim();
  document.getElementById('server-name').textContent = n.toLowerCase() === 'hiraku' ? '' : n;
}

function closeMenu() {
  document.body.classList.remove('menu-open');
  document.getElementById('menu').setAttribute('aria-expanded', 'false');
}

// After a log-in or a password change: who is calling now
export async function refreshMe() {
  const me = await get('/me');
  state.me = me;
  setCsrf(me.csrf);
  return me;
}

async function start() {
  document.getElementById('menu').addEventListener('click', () => {
    const open = !document.body.classList.contains('menu-open');
    document.body.classList.toggle('menu-open', open);
    document.getElementById('menu').setAttribute('aria-expanded', String(open));
  });
  try {
    state.server = await get('/server');
  } catch {
    state.server = {name: 'HIRAKU', signup: 'closed', games: []};
  }
  showServerName(state.server.name);
  try {
    await refreshMe();
  } catch {
    state.me = null;
  }
  onSignedOut(() => {
    const was = !!state.me;
    state.me = null;
    setCsrf('');
    if (was) toast('Your session has ended: sign in again.', 'warn');
    go('signin');
  });
  window.addEventListener('hashchange', route);
  route();
}

start();
