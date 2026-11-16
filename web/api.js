// SPDX-License-Identifier: GPL-3.0-or-later
// The JSON API (docs/SERVER.md), from the page: the session is the cookie (HttpOnly: the page never
// sees it); the CSRF value is kept in this module's memory only and sent with every change. Nothing
// is written to the browser's storage.

let csrf = '';
let signedOut = null;   // called on a 401 from a call that needs a log-in

export function setCsrf(v) { csrf = v || ''; }
export function onSignedOut(fn) { signedOut = fn; }

export class ApiError extends Error {
  constructor(status, code, message, retry, triesLeft) {
    super(message);
    this.status = status;
    this.code = code;
    this.retry = retry;
    this.triesLeft = triesLeft;   // a refused card link: wrong PINs left before the card is locked (else undefined)
  }
}

// (the calls whose 401 is an answer, not a session that ended)
const PUBLIC = new Set(['/login', '/reset', '/signup', '/server']);

export async function api(method, path, body) {
  const headers = {Accept: 'application/json'};
  if (body !== undefined) headers['Content-Type'] = 'application/json';
  if (method !== 'GET' && csrf) headers['X-HK-CSRF'] = csrf;
  let r;
  try {
    r = await fetch('/api/v1' + path, {
      method, headers, body: body === undefined ? undefined : JSON.stringify(body), credentials: 'same-origin', cache: 'no-store',
    });
  } catch {
    throw new ApiError(0, 'network', 'the server could not be reached: check the connection and try again');
  }
  let j = null;
  try { j = await r.json(); } catch { j = null; }
  if (r.ok) return j || {};
  const err = new ApiError(r.status, (j && j.error) || 'error', (j && j.message) || `the server answered ${r.status}`, r.headers.get('Retry-After'),
    j && Number.isInteger(j.tries_left) ? j.tries_left : undefined);
  if (r.status === 401 && !PUBLIC.has(path.split('?')[0]) && signedOut) signedOut(err);
  throw err;
}

export const get = (p) => api('GET', p);
export const post = (p, b) => api('POST', p, b === undefined ? {} : b);
export const put = (p, b) => api('PUT', p, b);
export const del = (p) => api('DELETE', p);
export const enc = encodeURIComponent;
