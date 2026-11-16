// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-server's pages: the pieces every page is made of. Everything from the server goes into the
// document as text (textContent, attributes through the DOM), never as HTML.

// An element: h('p', {class: 'x', text: 'hi'}, child, 'text', [more], null)
export function h(tag, props, ...children) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(props || {})) {
    if (v === undefined || v === null || v === false) continue;
    if (k === 'class') el.className = Array.isArray(v) ? v.filter(Boolean).join(' ') : v;
    else if (k === 'text') el.textContent = String(v);
    else if (k === 'on') for (const [ev, fn] of Object.entries(v)) el.addEventListener(ev, fn);
    else if (k === 'value' || k === 'checked' || k === 'disabled' || k === 'selected' || k === 'required' || k === 'hidden')
      el[k] = v;
    else el.setAttribute(k, v === true ? '' : String(v));
  }
  add(el, children);
  return el;
}

function add(el, children) {
  for (const c of children) {
    if (c === null || c === undefined || c === false) continue;
    if (Array.isArray(c)) add(el, c);
    else el.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
}

export function clear(el) {
  while (el.firstChild) el.firstChild.remove();
  return el;
}

// The server's sentences start in lower case: a page shows them as sentences
export function sentence(s) {
  s = String(s || '');
  return s ? s[0].toUpperCase() + s.slice(1) + (/[.!?)]$/.test(s) ? '' : '.') : '';
}

let ids = 0;
export const uid = (p = 'f') => `${p}${++ids}`;

// --- page frame

// A page's title block: heading, a line under it, and its section's colour
export function pageHead(title, sub, ...extra) {
  return h('div', {class: 'page-head'}, h('h1', {text: title, tabindex: '-1'}), sub ? h('p', {class: 'lead', text: sub}) : null, extra);
}

export function card(title, ...children) {
  return h('section', {class: 'card'}, title ? h('h2', {text: title}) : null, children);
}

// A message box: kind 'error' | 'ok' | 'info' | 'warn'
export function note(kind, text, ...more) {
  return h('div', {class: ['note', `note-${kind}`], role: kind === 'error' ? 'alert' : 'status'}, h('span', {text}), more);
}

export function loading() {
  return h('p', {class: 'loading', role: 'status', text: 'Loading…'});
}

export function empty(text) {
  return h('p', {class: 'empty', text});
}

// --- forms

// A labelled field. opts: type, help, value, required, autocomplete, min, max, inputmode, placeholder,
// pattern, maxlength, options (a select: [[value, label]])
export function field(label, name, opts = {}) {
  const id = uid();
  const helpId = opts.help ? id + 'h' : null;
  let input;
  if (opts.options) {
    input = h('select', {id, name, 'aria-describedby': helpId, required: opts.required},
      opts.options.map(([v, l]) => h('option', {value: String(v), text: l, selected: String(v) === String(opts.value ?? '')})));
  } else {
    input = h('input', {
      id, name, type: opts.type || 'text', value: opts.value ?? '', required: opts.required, autocomplete: opts.autocomplete || 'off',
      min: opts.min, max: opts.max, inputmode: opts.inputmode, placeholder: opts.placeholder, pattern: opts.pattern,
      maxlength: opts.maxlength, spellcheck: opts.type === 'password' ? 'false' : undefined, 'aria-describedby': helpId,
    });
  }
  const wrap = h('div', {class: 'field'}, h('label', {for: id, text: label}), input, opts.help ? h('p', {class: 'help', id: helpId, text: opts.help}) : null);
  wrap.input = input;
  return wrap;
}

// A switch: a checkbox drawn as one
export function toggle(label, checked, help) {
  const id = uid();
  const input = h('input', {type: 'checkbox', id, class: 'switch', checked, role: 'switch'});
  const wrap = h('div', {class: 'field field-inline'}, input, h('label', {for: id, text: label}), help ? h('p', {class: 'help', text: help}) : null);
  wrap.input = input;
  return wrap;
}

export function button(text, kind, on, attrs = {}) {
  return h('button', {type: attrs.type || 'button', class: ['btn', kind && `btn-${kind}`], on: on ? {click: on} : undefined, ...attrs}, text);
}

// A form whose submit runs `run(form)` (async); its own place for an error; the button busy meanwhile.
// run returns nothing, or a node shown under the form (a result), or throws an ApiError (shown)
export function form(children, submitText, run, opts = {}) {
  const out = h('div', {class: 'form-out', 'aria-live': 'polite'});
  const submit = h('button', {type: 'submit', class: ['btn', opts.danger ? 'btn-danger' : 'btn-primary']}, submitText);
  const f = h('form', {class: ['form', opts.class], novalidate: true}, children, h('div', {class: 'actions'}, submit, opts.extra), out);
  f.addEventListener('submit', async (e) => {
    e.preventDefault();
    clear(out);
    if (!f.checkValidity()) {
      const bad = f.querySelector(':invalid');
      const label = bad && bad.id ? f.querySelector(`label[for="${bad.id}"]`) : null;
      out.append(note('error', `${label ? label.textContent : 'A field'}: ${bad ? bad.validationMessage : 'check it'}`));
      if (bad) bad.focus();
      return;
    }
    submit.disabled = true;
    submit.classList.add('busy');
    try {
      const shown = await run(f);
      if (shown) out.append(shown);
    } catch (err) {
      out.append(errorNote(err));
    } finally {
      submit.disabled = false;
      submit.classList.remove('busy');
    }
  });
  f.out = out;
  return f;
}

export function value(f, name) {
  const el = f.elements.namedItem(name);
  return el ? el.value.trim() : '';
}

export function errorNote(err) {
  const msg = err && err.message ? err.message : String(err);
  const n = note('error', sentence(msg));
  if (err && err.status === 429 && err.retry) n.append(h('span', {class: 'muted', text: ` Try again in ${err.retry} s.`}));
  return n;
}

// --- shown once: a key, a token, a code

export function secret(title, text, ...words) {
  const id = uid('s');
  const box = h('input', {id, class: 'secret-text', readonly: true, value: text, 'aria-label': title, spellcheck: 'false'});
  const done = h('span', {class: 'copied', 'aria-live': 'polite'});
  const copy = button('Copy', 'primary', async () => {
    let ok = false;
    try {
      await navigator.clipboard.writeText(text);
      ok = true;
    } catch {
      box.select();
      try { ok = document.execCommand('copy'); } catch { ok = false; }
    }
    done.textContent = ok ? 'Copied.' : 'Select it and copy it by hand.';
  });
  box.addEventListener('focus', () => box.select());
  return h('div', {class: 'secret', role: 'region', 'aria-label': title},
    h('p', {class: 'secret-title', text: title}),
    h('div', {class: 'secret-row'}, box, copy),
    done,
    words.map((w) => h('p', {class: 'secret-words', text: w})),
    h('p', {class: 'secret-once', text: 'Shown this once: it is not kept anywhere you can read it again. Leaving this page forgets it.'}));
}

// --- a question before something that can't be undone

export function confirm(question, yes = 'Yes', danger = true, detail = '') {
  return new Promise((resolve) => {
    const d = h('dialog', {class: 'dialog', 'aria-labelledby': 'dq'});
    const end = (v) => { d.close(); d.remove(); resolve(v); };
    d.append(h('h2', {id: 'dq', text: question}), detail ? h('p', {text: detail}) : null,
      h('div', {class: 'actions'}, button(yes, danger ? 'danger' : 'primary', () => end(true)), button('Cancel', '', () => end(false))));
    d.addEventListener('cancel', (e) => { e.preventDefault(); end(false); });
    document.body.append(d);
    d.showModal();
    d.querySelector('.btn:last-child').focus();
  });
}

// A question with one field (a new name, a PIN): its value, or null
export function ask(question, label, opts = {}) {
  return new Promise((resolve) => {
    const d = h('dialog', {class: 'dialog', 'aria-label': question});
    const f = field(label, 'v', opts);
    const end = (v) => { d.close(); d.remove(); resolve(v); };
    const fm = h('form', {class: 'form', method: 'dialog'}, h('h2', {text: question}), f,
      h('div', {class: 'actions'}, h('button', {type: 'submit', class: 'btn btn-primary'}, opts.ok || 'Save'), button('Cancel', '', () => end(null))));
    fm.addEventListener('submit', (e) => {
      e.preventDefault();
      if (!f.input.checkValidity()) { f.input.reportValidity(); return; }
      end(f.input.value.trim());
    });
    d.addEventListener('cancel', (e) => { e.preventDefault(); end(null); });
    d.append(fm);
    document.body.append(d);
    d.showModal();
    f.input.focus();
    if (f.input.select) f.input.select();
  });
}

// --- tables

// cols: [{key, label, num, wide, sort(row), cell(row) -> node|text}] (wide: a long value, a line of its own at phone width); rows sortable by a heading's button.
// At phone width each row is a block of label/value lines (data-label).
export function table(cols, rows, opts = {}) {
  let by = opts.sort ?? null, desc = opts.desc ?? false;
  const tbody = h('tbody');
  const ths = cols.map((c, i) => {
    const th = h('th', {scope: 'col', class: c.num ? 'num' : null});
    if (c.sortable === false) th.textContent = c.label;
    else th.append(h('button', {type: 'button', class: 'sort', on: {click: () => {
      desc = by === i ? !desc : !!c.num && opts.numDesc !== false;
      by = i;
      fill();
    }}}, c.label, h('span', {class: 'arrow', 'aria-hidden': 'true'})));
    return th;
  });
  const key = (c, r) => (c.sort ? c.sort(r) : r[c.key]);
  function fill() {
    ths.forEach((th, i) => {
      if (by === i) th.setAttribute('aria-sort', desc ? 'descending' : 'ascending');
      else th.removeAttribute('aria-sort');
    });
    let list = rows.slice();
    if (by !== null) {
      const c = cols[by];
      list.sort((a, b) => {
        const x = key(c, a), y = key(c, b);
        if (x === y) return 0;
        if (x === null || x === undefined || x === '') return 1;   // (none last, either way)
        if (y === null || y === undefined || y === '') return -1;
        const r = typeof x === 'number' && typeof y === 'number' ? x - y : String(x).localeCompare(String(y), undefined, {numeric: true});
        return desc ? -r : r;
      });
    }
    clear(tbody);
    for (const r of list) {
      tbody.append(h('tr', {}, cols.map((c) => {
        const v = c.cell ? c.cell(r) : r[c.key];
        return h('td', {class: [c.num ? 'num' : c.cls, c.wide && 'wide'], 'data-label': c.label}, v === null || v === undefined || v === '' ? h('span', {class: 'none', text: '–'}) : v);
      })));
    }
  }
  fill();
  return h('div', {class: 'table-wrap'}, h('table', {class: ['rows', opts.class]}, opts.caption ? h('caption', {class: 'sr', text: opts.caption}) : null,
    h('thead', {}, h('tr', {}, ths)), tbody));
}

// --- words

export function when(t) {
  if (!t) return null;
  const d = new Date(t * 1000);
  return h('time', {datetime: d.toISOString(), text: d.toLocaleString(undefined, {dateStyle: 'medium', timeStyle: 'short'})});
}

export function ago(t) {
  if (!t) return 'never';
  const s = Math.max(0, Date.now() / 1000 - t);
  if (s < 90) return 'just now';
  if (s < 5400) return `${Math.round(s / 60)} minutes ago`;
  if (s < 129600) return `${Math.round(s / 3600)} hours ago`;
  return `${Math.round(s / 86400)} days ago`;
}

// a card number in groups of four, as printed
export function cardNumber(c) {
  return h('span', {class: 'mono card-no', text: String(c).replace(/(.{4})(?=.)/g, '$1 ')});
}

// a name a game keeps; one the server couldn't make text: said so
export function playerName(o, key = 'player') {
  if (o[key]) return h('span', {text: o[key]});
  if (o[key + '_hex']) return h('span', {class: 'muted', title: `bytes ${o[key + '_hex']}`, text: '(a name that can’t be shown)'});
  return null;
}

export const PREFECTURES = ['Hokkaido', 'Aomori', 'Iwate', 'Miyagi', 'Akita', 'Yamagata', 'Fukushima', 'Ibaraki', 'Tochigi', 'Gunma',
  'Saitama', 'Chiba', 'Tokyo', 'Kanagawa', 'Niigata', 'Toyama', 'Ishikawa', 'Fukui', 'Yamanashi', 'Nagano', 'Gifu', 'Shizuoka', 'Aichi',
  'Mie', 'Shiga', 'Kyoto', 'Osaka', 'Hyogo', 'Nara', 'Wakayama', 'Tottori', 'Shimane', 'Okayama', 'Hiroshima', 'Yamaguchi', 'Tokushima',
  'Kagawa', 'Ehime', 'Kochi', 'Fukuoka', 'Saga', 'Nagasaki', 'Kumamoto', 'Oita', 'Miyazaki', 'Kagoshima', 'Okinawa'];

export const prefecture = (n) => PREFECTURES[n - 1] || `#${n}`;
export const prefectureOptions = () => PREFECTURES.map((p, i) => [i + 1, `${i + 1} · ${p}`]);

export function chip(text, kind) {
  return h('span', {class: ['chip', kind && `chip-${kind}`], text});
}

// a short-lived word at the bottom of the screen ("Saved.")
export function toast(text, kind = 'ok') {
  const box = document.getElementById('toasts');
  if (!box) return;
  const t = h('div', {class: ['toast', `toast-${kind}`], role: kind === 'error' ? 'alert' : 'status', text});
  box.append(t);
  setTimeout(() => t.classList.add('out'), 3200);
  setTimeout(() => t.remove(), 3700);
}
