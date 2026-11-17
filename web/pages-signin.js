// SPDX-License-Identifier: GPL-3.0-or-later
// Signed out: sign in, sign up (as the server's sign-up mode allows), a reset code; and the one page
// of an account that must change its password first.
import {post, setCsrf} from './api.js';
import {refreshMe, state} from './app.js';
import {field, form, h, note, sentence, toast, value} from './ui.js';

const NAME_HELP = '1 to 32 letters, digits, “.”, “_” or “-”.';
const PASSWORD_HELP = '8 characters at least. A long phrase is easier to remember than a short jumble.';

function box(title, lead, ...children) {
  return h('div', {class: 'signin'}, h('div', {class: 'signin-card card'},
    h('h1', {text: title, tabindex: '-1'}), lead ? h('p', {class: 'lead', text: lead}) : null, children));
}

function links(...items) {
  return h('p', {class: 'signin-links'}, items.filter(Boolean).map((x, i) => [i ? h('span', {class: 'dot', 'aria-hidden': 'true', text: '·'}) : null, x]));
}

const a = (path, text) => h('a', {href: '#/' + path, text});

async function logIn(name, password, go) {
  const j = await post('/login', {name, password});
  setCsrf(j.csrf);
  await refreshMe();
  go(state.me.must_change_password ? 'password' : 'home');
}

export function signIn(view, {go}) {
  const mode = state.server.signup;
  const f = form([
    field('Name', 'name', {required: true, autocomplete: 'username', maxlength: 32}),
    field('Password', 'password', {type: 'password', required: true, autocomplete: 'current-password'}),
  ], 'Sign in', async (f) => {
    await logIn(value(f, 'name'), f.elements.namedItem('password').value, go);
  });
  // (e\u2011AMUSEMENT: a hyphen that doesn't break the word over two lines)
  view.append(box(`Sign in to ${state.server.name}`, 'Your cards, scores and arcades on this e\u2011AMUSEMENT server.', f,
    links(mode !== 'closed' ? a('signup', 'Sign up') : null, a('reset', 'I have a reset code')),
    mode === 'closed' ? h('p', {class: 'help center', text: 'Sign-up is closed on this server: its admin makes accounts.'}) : null));
}

export function signUp(view, {go}) {
  const mode = state.server.signup;
  if (mode === 'closed') {
    view.append(box('Sign up', null, note('info', 'Sign-up is closed on this server. Ask its admin for an account.'), links(a('signin', 'Back to sign in'))));
    return;
  }
  const fields = [
    field('Name', 'name', {required: true, autocomplete: 'username', maxlength: 32, pattern: '[A-Za-z0-9._\\-]{1,32}', help: NAME_HELP}),
    field('Password', 'password', {type: 'password', required: true, autocomplete: 'new-password', help: PASSWORD_HELP}),
    field('Password again', 'again', {type: 'password', required: true, autocomplete: 'new-password'}),
  ];
  if (mode === 'invite') fields.push(field('Invitation code', 'invite', {required: true, help: 'This server signs people up by invitation: the code its admin gave you.'}));
  const f = form(fields, 'Sign up', async (f) => {
    const name = value(f, 'name'), password = f.elements.namedItem('password').value;
    if (password !== f.elements.namedItem('again').value) return note('error', 'The two passwords are not the same.');
    const body = {name, password};
    if (mode === 'invite') body.invite = value(f, 'invite');
    await post('/signup', body);
    toast('Welcome! Your account is made.');
    await logIn(name, password, go);
  });
  view.append(box('Sign up', mode === 'invite' ? 'An account on this server, with the invitation code you were given.'
    : 'An account keeps your cards together and shows your scores.', f, links(a('signin', 'I have an account'))));
}

export function reset(view, {go}) {
  const f = form([
    field('Reset code', 'code', {required: true, help: 'The code an admin of this server made for you. It works once, for a day.'}),
    field('New password', 'password', {type: 'password', required: true, autocomplete: 'new-password', help: PASSWORD_HELP}),
    field('New password again', 'again', {type: 'password', required: true, autocomplete: 'new-password'}),
  ], 'Set the password', async (f) => {
    const password = f.elements.namedItem('password').value;
    if (password !== f.elements.namedItem('again').value) return note('error', 'The two passwords are not the same.');
    await post('/reset', {code: value(f, 'code'), password});
    toast('Your password is changed: sign in with it.');
    go('signin');
  });
  view.append(box('Reset your password', 'A forgotten password is replaced with a code from an admin.', f, links(a('signin', 'Back to sign in'))));
}

export function mustChange(view, {go}) {
  const f = form([
    field('Password now', 'old', {type: 'password', required: true, autocomplete: 'current-password'}),
    field('New password', 'password', {type: 'password', required: true, autocomplete: 'new-password', help: PASSWORD_HELP}),
    field('New password again', 'again', {type: 'password', required: true, autocomplete: 'new-password'}),
  ], 'Change the password', async (f) => {
    const password = f.elements.namedItem('password').value;
    if (password !== f.elements.namedItem('again').value) return note('error', 'The two passwords are not the same.');
    await post('/me/password', {old_password: f.elements.namedItem('old').value, password});
    await refreshMe();
    toast('Password changed.');
    go('home');
  });
  view.append(box('Choose a new password', `${state.me.name}, this account's password was given to you: choose one of your own before anything else.`,
    note('info', sentence('until it is changed, this is the only page open to you')), f));
}
