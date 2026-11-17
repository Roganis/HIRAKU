// SPDX-License-Identifier: GPL-3.0-or-later
// An operator's pages (an admin's too, over every arcade): arcades, an arcade's machines and their
// keys, an arcade's settings.
import {del, enc, get, post, put} from './api.js';
import {state} from './app.js';
import {settingsForm} from './settings.js';
import {
  ago, ask, button, card, chip, clear, confirm, count, empty, keepFocus, errorNote, field, form, h, moreActions, note, pageHead, prefecture, prefectureOptions, secret,
  table, toast, value,
} from './ui.js';

const LAUNCHER = 'Paste it into the launcher: Settings > e\u2011AMUSEMENT > Server key.';   // (a hyphen that doesn't break)

export async function arcades(view, {still, go}) {
  const isAdmin = state.me.role === 'admin';
  view.append(pageHead(isAdmin ? 'Arcades' : 'My arcades', isAdmin ? 'Every arcade on this server, and its machines.'
    : 'Your arcades: the shop name and prefecture the games show, their machines and their settings.'));
  const list = h('div');
  const add = form([
    field('Name', 'name', {required: true, maxlength: 64, help: 'The shop’s name, as the games show it.'}),
    field('Prefecture', 'prefecture', {options: prefectureOptions(), value: 13}),
    isAdmin ? field('Owner', 'owner', {maxlength: 32, help: 'An operator’s name. Empty: yours.'}) : null,
  ], 'Add the arcade', async (f) => {
    const body = {name: value(f, 'name'), prefecture: parseInt(value(f, 'prefecture'), 10)};
    if (isAdmin && value(f, 'owner')) body.owner = value(f, 'owner');
    const a = await post('/arcades', body);
    f.reset();
    toast(`${a.name} added.`);
    go('arcades/' + a.id);
  });
  view.append(list, card('Add an arcade', add));
  async function draw() {
    const [a, m] = await Promise.all([get('/arcades'), get('/machines')]);
    if (!still()) return;
    clear(list);
    if (!a.arcades.length) {
      list.append(card(null, empty('No arcade yet. Add one below, then its machines.')));
      return;
    }
    const ul = h('ul', {class: 'arcades'});
    for (const x of a.arcades) {
      const ms = m.machines.filter((y) => y.arcade === x.id);
      const on = ms.filter((y) => y.enabled).length;
      ul.append(h('li', {class: 'card arcade'},
        h('div', {class: 'arcade-main'},
          h('h2', {}, h('a', {href: '#/arcades/' + x.id, text: x.name})),
          h('p', {class: 'muted'}, prefecture(x.prefecture), isAdmin ? ` · ${x.owner ? 'owner ' + x.owner : 'no owner'}` : ''),
          h('p', {class: 'chips'}, chip(count(ms.length, 'machine')), ms.length && on < ms.length ? chip(`${ms.length - on} disabled`, 'warn') : null)),
        h('div', {class: 'arcade-actions'},
          h('a', {class: 'btn btn-primary', href: '#/arcades/' + x.id, text: 'Machines'}),
          h('a', {class: 'btn', href: `#/arcades/${x.id}/settings`, text: 'Settings'}),
          button('Edit', '', () => edit(x)),
          button('Remove', 'quiet', () => remove(x)))));
    }
    list.append(ul);
  }
  async function edit(x) {
    const name = await ask(`Rename ${x.name}`, 'Name', {value: x.name, required: true, maxlength: 64});
    if (name === null) return;
    const pref = await ask(`${name}: prefecture`, 'Prefecture', {options: prefectureOptions(), value: x.prefecture});
    if (pref === null) return;
    try {
      await put('/arcades/' + x.id, {name, prefecture: parseInt(pref, 10)});
      toast('Saved.');
      draw();
    } catch (err) { list.prepend(errorNote(err)); }
  }
  async function remove(x) {
    if (!await confirm(`Remove ${x.name}?`, 'Remove the arcade', true, 'Its machines and its settings go with it: their keys stop working at once.')) return;
    try {
      await del('/arcades/' + x.id);
      toast(`${x.name} removed.`);
      draw();
    } catch (err) { list.prepend(errorNote(err)); }
  }
  await draw();
}

function arcadeTabs(id, here) {
  return h('nav', {class: 'tabs', 'aria-label': 'This arcade'},
    h('a', {href: '#/arcades/' + id, class: here === 'machines' ? 'current' : null, 'aria-current': here === 'machines' ? 'page' : null, text: 'Machines'}),
    h('a', {href: `#/arcades/${id}/settings`, class: here === 'settings' ? 'current' : null, 'aria-current': here === 'settings' ? 'page' : null, text: 'Settings'}));
}

export async function machines(view, {params, still}) {
  const x = await get('/arcades/' + enc(params.id));
  if (!still()) return;
  view.append(h('p', {class: 'crumbs'}, h('a', {href: '#/arcades', text: 'Arcades'})),
    pageHead(x.name, `${prefecture(x.prefecture)}. Each machine answers behind its own key.`), arcadeTabs(x.id, 'machines'));
  const shown = h('div');   // (a new key: here until the page is left)
  const list = h('div');
  const add = form([field('Name', 'name', {required: true, maxlength: 64, help: 'Which cabinet it is: “pop’n by the door”.'})], 'Add the machine', async (f) => {
    const m = await post(`/arcades/${x.id}/machines`, {name: value(f, 'name')});
    f.reset();
    showKey(m.name, m.key);
    draw();
  });
  view.append(shown, list, card('Add a machine', h('p', {class: 'muted', text: 'Its key is shown once, when it is made.'}), add));
  function showKey(name, key) {
    clear(shown, secret(`${name}: its key`, key, LAUNCHER, 'The server takes this machine’s calls with this key only.'));
    shown.scrollIntoView({block: 'nearest'});
  }
  async function act(fn) {
    try { await fn(); draw(); } catch (err) { list.prepend(errorNote(err)); }
  }
  async function draw() {
    const j = await get(`/arcades/${x.id}/machines`);
    if (!still()) return;
    const back = keepFocus(list);
    clear(list);
    if (!j.machines.length) {
      list.append(card(null, empty('No machine yet. Add one: its key goes into that cabinet’s launcher.')));
      return;
    }
    const phone = matchMedia('(max-width: 760px)').matches;   // (there every action shows, as a row's own lines)
    list.append(card(`Machines (${j.machines.length})`, table([
      {key: 'name', label: 'Name', cls: 'nowrap', cell: (m) => h('strong', {text: m.name, title: m.name})},
      {key: 'pcbid', label: 'PCBID', cls: 'mono small', wide: true},
      {key: 'enabled', label: 'State', sort: (m) => (m.enabled ? 0 : 1), cell: (m) => chip(m.enabled ? 'enabled' : 'disabled', m.enabled ? 'ok' : 'warn')},
      {key: 'has_key', label: 'Key', sort: (m) => (m.has_key ? 0 : 1), cell: (m) => chip(m.has_key ? 'has a key' : 'no key', m.has_key ? 'ok' : 'bad')},
      {key: 'last_seen', label: 'Last seen', wide: true, cls: 'nowrap', cell: (m) => h('span', {title: m.last_seen ? new Date(m.last_seen * 1000).toLocaleString() : null, text: ago(m.last_seen)})},
      {key: 'pcbid', label: 'Actions', sortable: false, cell: (m) => {
        const more = moreActions(button(m.enabled ? 'Disable' : 'Enable', 'small', () => act(async () => {
          await put('/machines/' + enc(m.pcbid), {enabled: !m.enabled});
          toast(`${m.name} ${m.enabled ? 'disabled' : 'enabled'}.`);
        })), [
        button('Rename', 'small', () => act(async () => {
          const name = await ask(`Rename ${m.name}`, 'Name', {value: m.name, required: true, maxlength: 64});
          if (name !== null) await put('/machines/' + enc(m.pcbid), {name});
        })),
        button(m.has_key ? 'New key' : 'Make a key', 'small', () => act(async () => {
          if (m.has_key && !await confirm(`A new key for ${m.name}?`, 'Make a new key', true, 'Its old key stops working at once: the cabinet needs the new one.')) return;
          const k = await post('/machines/' + enc(m.pcbid) + '/key');
          showKey(m.name, k.key);
        })),
        button('Remove', 'small-quiet', () => act(async () => {
          if (!await confirm(`Remove ${m.name}?`, 'Remove the machine', true, 'Its key stops working at once.')) return;
          await del('/machines/' + enc(m.pcbid));
          toast(`${m.name} removed.`);
        }))], `More for ${m.name}`);
        if (phone) more.querySelector('details').open = true;
        return more;
      }},
    ], j.machines, {caption: 'Machines', class: 'machines', rowKey: (m) => m.pcbid})));
    back();
  }
  await draw();
}

export async function arcadeSettings(view, {params, still}) {
  const x = await get('/arcades/' + enc(params.id));
  const j = await get(`/arcades/${x.id}/settings`);
  if (!still()) return;
  view.append(h('p', {class: 'crumbs'}, h('a', {href: '#/arcades', text: 'Arcades'})),
    pageHead(x.name, 'What this arcade’s machines are told: events and unlocks, each game’s own. A setting left “as the server’s” follows the server’s value.'),
    arcadeTabs(x.id, 'settings'));
  if (!j.games.length) {
    view.append(card(null, empty('No game has settings an arcade can change.')));
    return;
  }
  view.append(note('info', 'A machine picks up a change at its next call to the server.'),
    settingsForm(j.games, {arcade: true, save: (game, settings) => put(`/arcades/${x.id}/settings`, {settings})}));
}
