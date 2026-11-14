/**
 * @file In-game Rich Presence: the mod's state file -> a Discord activity.
 *
 * While the game runs, t2r-ext.dll rewrites one small file in the game folder:
 *
 *   <game_dir>/t2r-rpc-state.txt     key=value lines, ASCII keys, UTF-8 values
 *
 *   v=1                     format version (unknown versions are ignored)
 *   st=match                menu | search | lobby | vote | gearup | load | match | dead | spec | post
 *   map=p1-bookstore        map image slug (the game's map-pvp-<slug> picture; picks the card image)
 *   mapn=Bookstore (Part I) map name as the game shows it (English)
 *   mode=sr                 sr (Supply Raid) | int (Interrogation) | surv (rounds, for a later mode)
 *   moden=Supply Raid       mode name as the game shows it (English)
 *   lv=12  elv=9            Supply Raid: team lives left, ours / theirs
 *   in=3  ein=2  inn=5      Interrogation: interrogations done, ours / theirs / needed for the safebox
 *   box=1  ebox=1           Interrogation: we are fighting for their safebox / they are at ours
 *   sc=2  esc=1  rd=4       rounds won, ours / theirs; current round (no mode uses it yet)
 *   k=5  dn=3  x=1          this match: my kills, downs, executions (the ext sends downs + executions)
 *   clan=WLF                faction of the clan (WLF / Seraphites; empty = no clan yet)
 *   wk=3  day=4  sv=14      week (1-12) and day (1-7) of the clan's twelve weeks, survivors
 *   t0=1763054400           unix seconds the match started (elapsed timer)
 *   np=8  npmax=8           players in the match / lobby (or party members in the menus), of how many
 *   win=1                   post match: 1 we won, 0 we lost, absent = unknown / draw
 *
 * Every key is optional; unknown keys are ignored, so the ext and the launcher can be
 * updated apart. The mod rewrites the file at least every 10 s while it runs (a
 * heartbeat), so a file older than STALE_MS is a leftover of a crashed / old game.
 *
 * All display text goes through the launcher's i18n (T), so every language the
 * launcher has is a language the card has. Map / mode names come from the game in
 * English and are translated like any other launcher string when a table has them.
 */
'use strict';

const fs = require('fs');
const path = require('path');

const FILE = 't2r-rpc-state.txt';
const STALE_MS = 45000;
const STATES = new Set([
  'menu',
  'search',
  'lobby',
  'vote',
  'gearup',
  'load',
  'match',
  'dead',
  'spec',
  'post',
]);
// card images served by the project site (Discord fetches https images through its media proxy)
const IMG_BASE = 'https://twelve-weeks.com/assets/rpc/';
// site/assets/rpc/<slug>.jpg (512 x 512, from the map vote pictures); a map without one (a map pack) shows the logo
const MAP_IMAGES = new Set([
  'forest',
  'gas-station',
  'school',
  'shipyard',
  'resort',
  'seraphite-camp',
  'houses',
  'hospital',
  'hardware',
  'motel',
  'hillcrest-stores',
  'tv-station',
  'train-station',
  'downtown',
  'lodge',
  'jackson-suburbs',
  'outpost',
  'p1-bookstore',
  'wilds-houses',
  'capitol-shops',
  'into-the-city',
  'overpass',
  'chase',
  'jackson-market',
]);
const LOGO = IMG_BASE + 'logo-bw.png';        // black-on-white logo (was the Discord app asset 'twelve_weeks_logo')
const SITE = 'https://twelve-weeks.com';

/**
 * Parse the state file text. Never throws.
 * @param {string} text
 * @returns {object|null} null when the text is not a v1 state file
 */
function parse(text) {
  if (typeof text !== 'string' || !text) return null;
  const s = {};
  for (const raw of text.split(/\r?\n/)) {
    const i = raw.indexOf('=');
    if (i <= 0) continue;
    const k = raw.slice(0, i).trim();
    if (!/^[a-z0-9]{1,8}$/.test(k)) continue;
    s[k] = raw
      .slice(i + 1)
      .trim()
      .slice(0, 64);
  }
  if (s.v !== '1' || !STATES.has(s.st)) return null;
  return s;
}

/**
 * Read the state file of a game folder, ignoring leftovers.
 * @param {string|null} gameDir
 * @param {number} [notBefore] epoch ms: a file last written before this (the launch) is a leftover
 * @param {number} [now]
 * @returns {object|null}
 */
function read(gameDir, notBefore, now) {
  if (!gameDir) return null;
  const file = path.join(gameDir, FILE);
  try {
    const st = fs.statSync(file);
    const t = now || Date.now();
    if (t - st.mtimeMs > STALE_MS) return null;
    if (notBefore && st.mtimeMs < notBefore - 2000) return null;
    return parse(fs.readFileSync(file, 'utf8'));
  } catch {
    return null;
  }
}

const int = (v) => {
  const n = parseInt(v, 10);
  return Number.isFinite(n) && n >= 0 && n < 100000 ? n : null;
};
const text = (v) => (typeof v === 'string' && v ? v : null);

/**
 * Build the Discord activity for a parsed state.
 * @param {object} s parsed state (see parse)
 * @param {(s: string, vars?: object) => string} T launcher translation function
 * @param {{ mapImages?: Set<string> }} [opts]
 * @returns {import('./noop.js').RpcActivity}
 */
function build(s, T, opts) {
  const imgs = (opts && opts.mapImages) || MAP_IMAGES;
  const tr = (str, vars) => (T ? T(str, vars) : str);
  const map = text(s.mapn) ? tr(s.mapn) : null;
  const mode = text(s.moden) ? tr(s.moden) : null;
  const both = (a, b) => (a && b ? `${a} · ${b}` : a || b || null);
  const k = int(s.k),
    dn = int(s.dn),
    x = int(s.x);
  const wk = int(s.wk),
    day = int(s.day);
  const clan = text(s.clan);
  const week = wk
    ? day
      ? tr('Week {w}, day {d}', { w: wk, d: day })
      : tr('Week {w}', { w: wk })
    : null;
  const sv = int(s.sv);
  const clanLine = both(clan ? tr(clan) : null, week);
  const clanHover = both(clanLine, sv ? tr('{n} {n|survivor|survivors}', { n: sv }) : null);

  // the score line of the mode (state line during a match)
  function scoreLine() {
    if (s.mode === 'sr') {
      const lv = int(s.lv),
        elv = int(s.elv);
      if (lv == null) return null;
      const ours = tr('{n} {n|life|lives} left', { n: lv });
      return elv == null ? ours : `${ours} · ${tr('enemy {n}', { n: elv })}`;
    }
    if (s.mode === 'int') {
      if (s.box === '1') return tr('Fighting for the safebox');
      if (s.ebox === '1') return tr('Defending the safebox');
      const n = int(s.in),
        need = int(s.inn),
        e = int(s.ein);
      if (n == null) return null;
      const ours = need
        ? tr('Interrogations {n}/{need}', { n, need })
        : tr('Interrogations: {n}', { n });
      return e == null ? ours : `${ours} · ${tr('enemy {n}', { n: e })}`;
    }
    const sc = int(s.sc),
      esc = int(s.esc),
      rd = int(s.rd);
    if (sc != null && esc != null)
      return both(rd ? tr('Round {n}', { n: rd }) : null, `${sc}–${esc}`);
    return rd ? tr('Round {n}', { n: rd }) : null;
  }

  let details, state;
  switch (s.st) {
    case 'menu':
      details = tr('In the main menu');
      state = clanLine || tr('Ready');
      break;
    case 'search':
      details = tr('Looking for a match');
      state = mode || clanLine;
      break;
    case 'lobby':
      details = tr('In the lobby');
      state = both(mode, map) || clanLine;
      break;
    case 'vote':
      details = tr('Voting for the next map');
      state = mode || clanLine;
      break;
    case 'gearup':
      details = tr('Gearing up');
      state = both(mode, map);
      break;
    case 'load':
      details = map ? tr('Loading {map}', { map }) : tr('Loading');
      state = mode;
      break;
    case 'post': {
      details = both(tr('Match over'), map);
      const res = s.win === '1' ? tr('Victory') : s.win === '0' ? tr('Defeat') : null;
      state = both(res, mode);
      break;
    }
    default: {
      // match / dead / spec: mode · map, then the mode's score; dead / spectating say so first
      details = both(mode, map) || tr('In a match');
      const sl = scoreLine();
      const lead = s.st === 'dead' ? tr('Down') : s.st === 'spec' ? tr('Spectating') : null;
      state = both(lead, sl);
    }
  }

  const a = { name: 'Twelve Weeks', details: details || tr('Playing Twelve Weeks') };
  if (state) a.state = state;
  const t0 = /^[0-9]{9,11}$/.test(s.t0 || '') ? parseInt(s.t0, 10) : null; // unix seconds
  if (t0 && ['match', 'dead', 'spec'].includes(s.st)) a.timestamps = { start: t0 };
  const np = int(s.np),
    npmax = int(s.npmax);
  if (np && npmax && np <= npmax && s.st !== 'post' && s.st !== 'load') {
    a.partySize = np;
    a.partyMax = npmax;
  }

  // card art: the map picture when there is one (logo as the small badge), else the logo
  const stats = [];
  if (k != null) stats.push(tr('{n} {n|kill|kills}', { n: k }));
  if (dn != null) stats.push(tr('{n} {n|down|downs}', { n: dn }));
  if (x != null && x > 0) stats.push(tr('{n} {n|execution|executions}', { n: x }));
  const statLine =
    stats.length && ['match', 'dead', 'spec', 'post'].includes(s.st) ? stats.join(' · ') : null;
  const key = text(s.map);
  const assets = {};
  // corner badge = the clan's faction emblem, hover = week / day / survivors; no clan yet: the logo (with a map picture only)
  const fac = s.clan === 'WLF' ? 'fac-wlf-bw' : s.clan === 'Seraphites' ? 'fac-sera-bw' : null;   // black on white
  if (key && /^[a-z0-9_-]{1,32}$/.test(key) && imgs.has(key)) {
    assets.largeImage = IMG_BASE + key + '.jpg';
    assets.largeText = both(map, statLine) || map || undefined;
    assets.smallImage = fac ? IMG_BASE + fac + '.png' : LOGO;
    assets.smallText = clanHover || tr('Twelve Weeks');
  } else {
    assets.largeImage = LOGO;
    assets.largeText = statLine || (fac ? tr('Twelve Weeks') : clanHover) || tr('Twelve Weeks');
    if (fac) {
      assets.smallImage = IMG_BASE + fac + '.png';
      assets.smallText = clanHover;
    }
  }
  a.assets = assets;
  a.buttons = [{ label: tr('Get Twelve Weeks'), url: SITE }];
  return clip(a);
}

// Discord: details / state / image texts 2..128 chars, button labels up to 32
function clip(a) {
  const c = (v, n) =>
    typeof v === 'string'
      ? v.length > n
        ? v.slice(0, n - 1) + '…'
        : v.length < 2
          ? v + ' '
          : v
      : v;
  a.details = c(a.details, 128);
  if (a.state) a.state = c(a.state, 128);
  if (a.assets) {
    if (a.assets.largeText) a.assets.largeText = c(a.assets.largeText, 128);
    if (a.assets.smallText) a.assets.smallText = c(a.assets.smallText, 128);
  }
  if (a.buttons) a.buttons = a.buttons.map((b) => ({ label: c(b.label, 32), url: b.url }));
  return a;
}

/**
 * A stable key of what the card shows (the elapsed timer start included), used to skip
 * redundant updates.
 * @param {object} a activity
 * @returns {string}
 */
function signature(a) {
  return JSON.stringify(a);
}

module.exports = { FILE, STALE_MS, IMG_BASE, MAP_IMAGES, parse, read, build, signature };
