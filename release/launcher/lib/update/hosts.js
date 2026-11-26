/**
 * @file Which address each update-server request goes to (launcher 0.1.46).
 *
 * The official update server has two public addresses:
 * - api:    https://dl.twelve-weeks.com (Cloudflare, HTTPS). Small requests: manifest, status, news, clan, reports.
 * - direct: http://play.twelve-weeks.com:7002 (fra1 itself, plain HTTP). Big files and clip uploads: Cloudflare limits
 *           large files (and uploads over 100 MB), so they stay on the direct address.
 * Some provider filters (some ISP security filters) block plain HTTP on :7002, so every request falls back to the other
 * address when its own one fails at the network level or answers with a redirect (our server never redirects; a filter
 * does). A failed address is skipped for DOWN_MS. URLs on any other host (GitHub release files) are never touched.
 * Unconfigured (tests, test server, F2_FILE_HOST) every function leaves URLs as they are.
 *
 * 0.1.49: a third address, tls = https://play.twelve-weeks.com (fra1 itself over HTTPS on 443). Some filters cut plain HTTP on :7002 AND
 * the Cloudflare address (a Steam Deck saw "read ECONNRESET" on both). Small requests try api, tls, direct; big files tls, direct, api.
 * Every request walks the list until one address gets through (a failed one is skipped for DOWN_MS).
 *
 * 0.1.50: a fourth address, ip = OFFICIAL_IP_HOST (the server by bare IP, plain HTTP). Some filters block the whole twelve-weeks.com
 * domain (the site, Cloudflare and both server names reset on some networks while the bare-IP link worked). It is always the last try.
 */
'use strict';

const DOWN_MS = 10 * 60 * 1000;
// paths that carry big bodies: blobs, Part I files, named downloads, clip uploads
const BIG = /^\/(files|part1-files|download|report-clip)(\/|$|\?)/;

let api = null; // URL objects of the addresses, or null when unconfigured
let direct = null;
let tls = null; // optional third address (0.1.49)
let ip = null; // optional fourth address (0.1.50)
const downUntil = { api: 0, direct: 0, tls: 0, ip: 0 };

/**
 * Set the official addresses (or clear them with no argument).
 * @param {{api?: string, direct?: string, tls?: string, ip?: string}} [o]
 * @returns {void}
 */
function configure(o = {}) {
  api = o.api ? new URL(o.api) : null;
  direct = o.direct ? new URL(o.direct) : null;
  tls = o.tls ? new URL(o.tls) : null;
  ip = o.ip ? new URL(o.ip) : null;
  downUntil.api = downUntil.direct = downUntil.tls = downUntil.ip = 0;
}

// a URL's port, or its scheme's default
const portOf = (u) => u.port || (u.protocol === 'https:' ? '443' : '80');

const configured = () => !!(api && direct);
const up = (k, now) => (now || Date.now()) >= downUntil[k];

/**
 * Which of our addresses a URL points at: 'api' (any scheme or port on the api host name: the server writes
 * http://dl... links), 'direct', 'tls', 'ip', or null.
 * @param {URL} u
 * @returns {'api'|'direct'|'tls'|'ip'|null}
 */
function whose(u) {
  if (!configured()) return null;
  if (u.hostname === api.hostname) return 'api';
  if (u.hostname === direct.hostname && portOf(u) === portOf(direct)) return 'direct';
  if (tls && u.hostname === tls.hostname && portOf(u) === portOf(tls)) return 'tls';
  if (ip && u.hostname === ip.hostname && portOf(u) === portOf(ip)) return 'ip';
  return null;
}

// the addresses a request tries, in order
function order(u) {
  const big = BIG.test(u.pathname);
  const list = big ? ['tls', 'direct', 'api', 'ip'] : ['api', 'tls', 'direct', 'ip'];
  return list.filter((k) => (k !== 'tls' || tls) && (k !== 'ip' || ip));
}

function swap(u, k) {
  const t = k === 'api' ? api : k === 'tls' ? tls : k === 'ip' ? ip : direct;
  const out = new URL(u.toString());
  out.protocol = t.protocol;
  out.hostname = t.hostname;
  out.port = t.port; // (setting host without a port keeps the old one)
  return out.toString();
}

/**
 * The address a request should use now.
 * @param {string} url
 * @param {number} [now]
 * @returns {string}
 */
function route(url, now) {
  let u;
  try {
    u = new URL(url);
  } catch {
    return url;
  }
  const k = whose(u);
  if (!k) return url;
  const list = order(u);
  const pick = list.find((a) => up(a, now)) || list[0];
  return swap(u, pick);
}

/**
 * Note that a request to url failed. Returns the URL to retry on the next address, or null (not ours, or all down).
 * @param {string} url
 * @param {number} [now]
 * @returns {string|null}
 */
function fail(url, now) {
  let u;
  try {
    u = new URL(url);
  } catch {
    return null;
  }
  const k = whose(u);
  if (!k) return null;
  const t = now || Date.now();
  downUntil[k] = t + DOWN_MS;
  const next = order(u).find((a) => a !== k && up(a, t));
  return next ? swap(u, next) : null;
}

/**
 * True when two URLs belong to the same update server (same origin, or both one of our addresses).
 * @param {string} a
 * @param {string} b
 * @returns {boolean}
 */
function sameServer(a, b) {
  const ua = new URL(a);
  const ub = new URL(b);
  return ua.origin === ub.origin || (!!whose(ua) && !!whose(ub));
}

// errors after which the other address is worth a try: network errors, TLS failures, a filter cutting the connection
const switchable = (e) =>
  /timeout|ECONN|ENOTFOUND|EHOSTUNREACH|ENETUNREACH|EAI_AGAIN|EPROTO|CERT|TLS|SSL|hang up|redirected/i.test(
    String((e && (e.code || '') + ' ' + (e.message || '')) || e),
  );

/**
 * Run fn(url) on the address route() picks; when it fails with a switchable error, on each next address in turn.
 * @template T
 * @param {string} url
 * @param {(url: string) => Promise<T>} fn
 * @returns {Promise<T>}
 */
async function withFallback(url, fn) {
  let cur = route(url);
  for (let i = 0; ; i++) {
    try {
      return await fn(cur);
    } catch (e) {
      const alt = switchable(e) ? fail(cur) : null;
      remember(cur, e, alt && i < 3 ? alt : null);
      if (!alt || i >= 3) throw e;
      cur = alt;
    }
  }
}

// 0.1.51: the last HISTORY_MAX failed requests (time, address, path, error), for files/network.txt in bug reports.
// Reports used to say only "read ECONNRESET"; which address failed, and on which path, was lost.
const HISTORY_MAX = 40;
const failures = [];
function remember(url, e, alt) {
  let k = '?';
  let path = '?';
  try {
    const u = new URL(url);
    k = whose(u) || u.host;
    path = u.pathname; // never the query: it can carry a token
  } catch {}
  let next = null;
  try {
    next = alt ? whose(new URL(alt)) : null;
  } catch {}
  const err = String((e && (e.code ? e.code + ' ' : '') + (e.message || '')) || e)
    .replace(/\s+/g, ' ')
    .slice(0, 200);
  failures.push({ t: new Date().toISOString(), k, path, err, next });
  if (failures.length > HISTORY_MAX) failures.shift();
}

/**
 * True when a response is a redirect (any 3xx) from one of our addresses. Our update server never redirects, so it is a
 * network filter's block page (0.1.51: 303 included; it used to pass as a plain answer and stop the client install).
 * @param {{statusCode: number}} res
 * @param {string} url
 * @returns {boolean}
 */
function blockedRedirect(res, url) {
  if (!res || res.statusCode < 300 || res.statusCode > 399) return false;
  try {
    return !!whose(new URL(url));
  } catch {
    return false;
  }
}

/** @returns {{t: string, k: string, path: string, err: string, next: string|null}[]} recent failed requests, oldest first */
const history = () => failures.slice();

/**
 * The bare IPv4 of our server for one of its names, or null. The game server name (play.twelve-weeks.com) has no
 * fallback of its own: a player's own DNS can say it is not found while every public resolver and the Cloudflare
 * address work, so PLAY stopped at "The server name ... was not found". The update server already uses this IP as its
 * last try (0.1.50); the game link may use it when the name lookup fails.
 * @param {string} name
 * @returns {string|null}
 */
function knownIp(name) {
  if (!configured() || !ip) return null;
  const n = String(name || '').toLowerCase();
  return n === direct.hostname || (tls && n === tls.hostname) ? ip.hostname : null;
}

/** @returns {Record<string, string>} the configured addresses by key (empty when unconfigured) */
function addresses() {
  const o = {};
  for (const [k, u] of Object.entries({ api, tls, direct, ip })) if (u) o[k] = u.origin;
  return o;
}

module.exports = {
  withFallback,
  knownIp,
  configure,
  configured,
  route,
  fail,
  sameServer,
  switchable,
  whose: (url) => whose(new URL(url)),
  history,
  note: remember,
  blockedRedirect,
  addresses,
  DOWN_MS,
};
