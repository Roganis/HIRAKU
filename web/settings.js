// SPDX-License-Identifier: GPL-3.0-or-later
// A settings form built from the API's list: each game's declared settings (label, help, range,
// default, the value now). For an arcade, "As the server's" leaves a setting to the server's value;
// for the server, "Default" leaves it to the game's default. Only what changed is sent.
import {clear, count, errorNote, h, note, toast, uid} from './ui.js';

// A value as words: a list ("1,2,3") with a space after each comma, so a long one can wrap; none: "(none)"
const shown = (v) => (v === '' || v === null || v === undefined ? '(none)' : /^[^,\s]+(,[^,\s]+)+$/.test(String(v)) ? String(v).replace(/,/g, ', ') : String(v));

// games: [{id, title, settings: [{name, label, help, kind, min, max, default, value, server_value}]}]
// save(game, [{game, name, value}]) -> the answer's games (drawn again)
export function settingsForm(games, {arcade, save, open}) {
  const wrap = h('div', {class: 'settings'});
  const find = h('input', {type: 'search', placeholder: 'Find a game or a setting', 'aria-label': 'Find a game or a setting'});
  const list = h('div', {class: 'settings-list'});
  if (games.length > 1) wrap.append(h('div', {class: 'toolbar'}, find, h('span', {class: 'muted small', text: count(games.length, 'game')})));
  wrap.append(list);
  const opened = new Set(open || []);
  function draw(gs) {
    clear(list);
    for (const g of gs) list.append(gameBlock(g));
    filter();
  }
  function filter() {
    const q = find.value.trim().toLowerCase();
    for (const d of list.children) {
      const hit = !q || d.dataset.words.includes(q);
      d.hidden = !hit;
      if (q && hit) d.open = true;
    }
  }
  find.addEventListener('input', filter);

  function gameBlock(g) {
    const own = g.settings.filter((s) => s.value !== null && s.value !== undefined).length;
    const d = h('details', {class: 'game-settings', open: opened.has(g.id)});
    d.dataset.words = [g.title, g.id, ...g.settings.map((s) => `${s.label || ''} ${s.name}`)].join(' ').toLowerCase();
    d.addEventListener('toggle', () => (d.open ? opened.add(g.id) : opened.delete(g.id)));
    const rows = g.settings.map((s) => settingRow(g, s));
    const out = h('div', {'aria-live': 'polite'});
    const saveBtn = h('button', {type: 'submit', class: 'btn btn-primary'}, 'Save');
    const f = h('form', {class: 'form', novalidate: true}, rows.map((r) => r.el), h('div', {class: 'actions'}, saveBtn), out);
    f.addEventListener('submit', async (e) => {
      e.preventDefault();
      clear(out);
      const changes = [];
      for (const r of rows) {
        const c = r.change();
        if (c.error) {
          out.append(note('error', c.error));
          r.focus();
          return;
        }
        if (c.changed) changes.push({game: g.id, name: r.name, value: c.value});
      }
      if (!changes.length) {
        out.append(note('info', 'Nothing changed.'));
        return;
      }
      saveBtn.disabled = true;
      try {
        const answer = await save(g.id, changes);
        toast(`${g.title}: saved.`);
        const again = (answer.games || []).find((x) => x.id === g.id);
        if (again) d.replaceWith(gameBlock(again));
      } catch (err) {
        out.append(errorNote(err));
      } finally { saveBtn.disabled = false; }
    });
    d.append(h('summary', {}, h('span', {class: 'summary-title', text: g.title}),
      h('span', {class: 'summary-sub', text: `${count(g.settings.length, 'setting')}${own ? ` · ${own} set ${arcade ? 'here' : 'on the server'}` : ''}`})), f);
    return d;
  }

  function settingRow(g, s) {
    const id = uid('set');
    const now = s.value ?? null;
    const fallback = arcade ? (s.server_value ?? s.default) : s.default;
    const fallbackWord = arcade ? (s.server_value !== null && s.server_value !== undefined ? 'the server’s' : 'the default') : 'the default';
    const number = s.kind === 'number';
    const input = h('input', {
      id, type: number ? 'number' : 'text', value: now ?? fallback ?? '', min: number ? s.min : undefined, max: number ? s.max : undefined,
      inputmode: number ? 'numeric' : undefined, 'aria-describedby': id + 'h', disabled: now === null, maxlength: number ? undefined : 256,
    });
    const leave = h('input', {type: 'checkbox', id: id + 'l', checked: now === null});
    leave.addEventListener('change', () => {
      input.disabled = leave.checked;
      if (leave.checked) input.value = fallback ?? '';
      else input.focus();
    });
    const range = number ? `${s.min} to ${s.max}` : null;
    const facts = [range ? `From ${range}.` : null, `Default ${shown(s.default)}.`,
      arcade && s.server_value !== null && s.server_value !== undefined ? `The server’s: ${shown(s.server_value)}.` : null].filter(Boolean).join(' ');
    const el = h('div', {class: ['setting', now !== null && 'setting-own']},
      h('div', {class: 'setting-text'},
        h('label', {for: id, text: s.label || s.name}),
        h('p', {class: 'help', id: id + 'h'}, s.help ? h('span', {text: s.help + ' '}) : null, h('span', {class: 'facts-line', text: facts}))),
      h('div', {class: 'setting-input'}, input,
        h('span', {class: 'leave'}, leave, h('label', {for: id + 'l', text: arcade ? 'As the server’s' : 'Default'})),
        h('span', {class: 'muted small', text: now === null ? `Now ${fallbackWord}: ${shown(fallback)}` : `Set ${arcade ? 'here' : 'on the server'}`})));
    return {
      el, name: s.name, focus: () => input.focus(),
      change() {
        if (leave.checked) return {changed: now !== null, value: null};
        const v = input.value.trim();
        if (number) {
          if (!/^-?\d+$/.test(v) || +v < s.min || +v > s.max) return {error: `${s.label || s.name}: a whole number from ${s.min} to ${s.max}.`};
          return {changed: String(+v) !== now, value: +v};
        }
        return {changed: v !== (now ?? ''), value: v === '' ? null : v};
      },
    };
  }

  draw(games);
  return wrap;
}
