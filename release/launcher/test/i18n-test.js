'use strict';
// node test/i18n-test.js : the Polish table is complete and consistent.
const fs = require('fs'), path = require('path'), assert = require('assert');
const OLD_NAME = new RegExp('factions\\s*(i{2}|2)\\b', 'i');   // the old product name
const I18N = require('../src/i18n-core');
const dir = path.join(__dirname, '..', 'src');
const pl = JSON.parse(fs.readFileSync(path.join(dir, 'i18n', 'pl.json'), 'utf8')), gpl = JSON.parse(fs.readFileSync(path.join(dir, 'i18n', 'game-pl.json'), 'utf8')), en = JSON.parse(fs.readFileSync(path.join(dir, 'i18n', 'en.json'), 'utf8'));
const data = new Function(fs.readFileSync(path.join(dir, 'gamedata.js'), 'utf8') + '\nreturn GAMEDATA;')();
I18N.load(gpl); I18N.load(pl); I18N.setLang('pl');
const vars = (s) => [...String(s).matchAll(/\{([A-Za-z0-9_]+)[!|}]/g)].map((m) => m[1]).sort().join(',');

// en.json lists exactly the pl.json keys; every Polish template uses the same placeholders as its English key
assert.deepStrictEqual(Object.keys(en).sort(), Object.keys(pl).sort(), 'en.json and pl.json keys differ');
for (const [k, v] of Object.entries(pl)) { if (/\{/.test(k) || /\{/.test(v)) assert.strictEqual(vars(v).split(',').filter(Boolean).filter((x, i, a) => a.indexOf(x) === i).join(','), vars(k).split(',').filter(Boolean).filter((x, i, a) => a.indexOf(x) === i).join(','), 'placeholders differ: ' + k); }

// every static text node / attribute of index.html
const html = fs.readFileSync(path.join(dir, 'index.html'), 'utf8').replace(/<script[\s\S]*?<\/script>|<style[\s\S]*?<\/style>|<svg[\s\S]*?<\/svg>|<!--[\s\S]*?-->/g, '');
const unescape = (s) => s.replace(/&middot;/g, '·').replace(/&amp;/g, '&');
const statics = [...html.matchAll(/>([^<>]+)</g)].map((m) => unescape(m[1]).trim()).filter((t) => /[A-Za-z]/.test(t)).concat([...html.matchAll(/(?:placeholder|title)="([^"]+)"/g)].map((m) => m[1]));
const keep = new Set(['Twelve Weeks', 'TWELVE WEEKS', 'THE LAST OF US', 'ENTER', 'ESC', 'ELECTRON', 'ENGLISH', 'POLSKI', 'ESPAÑOL', 'STATUS', 'STEAM', 'LAUNCHER', 'SYSTEM', 'THE LAST OF US PART II', 'THE LAST OF US PART I', 'The Last of Us Part II', 'The Last of Us Part I']);
const missing = statics.filter((t) => I18N.T(t) === t && !keep.has(t));
assert.deepStrictEqual(missing, [], 'untranslated static text: ' + missing.join(' | '));

// T() literals in the sources
const lits = [];
// (the sources are prettier-formatted: T( may be followed by a line break; app.js / loadout.js / main.js are split into src/app/*, src/loadout-*.js, main/*: test/srcs.js)
const SRCS = require('./srcs');
const litSrc = [SRCS.text('app'), SRCS.text('loadout'), SRCS.text('main'), SRCS.read('src/clan.js'), SRCS.read('src/feed.js')].join('\n');
for (const m of litSrc.matchAll(/\bT\(\s*'((?:[^'\\]|\\.)*)'/g)) lits.push(m[1].replace(/\\'/g, "'"));
// the Play button labels are translated as T(view.label): their literals sit in playButtonView (src/app/data.js)
for (const m of SRCS.text('app').matchAll(/\blabel: '([A-Z][A-Z &]+)'/g)) lits.push(m[1]);
const lost = [...new Set(lits)].filter((t) => !(t in pl) && !(t in gpl));
assert.deepStrictEqual(lost, [], 'T() texts without a Polish entry: ' + lost.join(' | '));

// the game data
const names = [];
for (const s of data.skills) { names.push(s.name, ...s.desc); for (let k = 1; k <= s.levels; k++) names.push(s.name + ' ' + ['', 'I', 'II', 'III'][k]); }
for (const slot of Object.values(data.weapons)) for (const w of slot) { names.push(w.name, w.desc); if (w.supIcon) names.push('SUPPRESSED ' + w.name); }
for (const c of data.classes) names.push(c.name, c.desc);
for (const m of Object.values(data.missions)) names.push(m.name);
names.push(...Object.values(data.boosters));
const gm = [...new Set(names)].filter((t) => I18N.T(t) === t && !/^(VEPR|MPX5|CZ75|9MM PISTOL|M14|SPECTER|EL DIABLO)$/.test(t));
assert.deepStrictEqual(gm, [], 'game data without Polish: ' + gm.join(' | '));

// player-visible sentences built in main.js / lib/*.js (errors, dialogs, progress labels): each one needs a pl.json entry (main.js wrap() / the renderer translate them by this text).
// The scan reads lines that throw / label / explain; ${...} in the source and {name} in a key both count as one placeholder.
{
  const norm = (s) => s.replace(/\$\{\}|\{[A-Za-z0-9_]+(?:!|\|[^}]*)?\}/g, '\u0001');
  const keys = new Set(Object.keys(pl).map(norm));
  const DEV_ONLY = /^(Unsafe path in manifest|Bad manifest|HTTP 416|Checksum mismatch|Internal error|Play is disabled in test|Test mode:|Disabled in test|Steam helper:|Game folder not set\.$|Close the game first\.$)/;
  // log-only lines and a report-file note that the unformatted source kept on lines the scan never looked at (not shown as UI text)
  const LOG_ONLY = /^(Server reachable, same build|The mod loaded after|Game window up after|WER crash dump \(has the ext)/;
  const scan = (line) => {
    const out = []; let i = 0;
    while (i < line.length) {
      const c = line[i];
      if (c === "'" || c === '"') { let j = i + 1, s = ''; while (j < line.length && line[j] !== c) { if (line[j] === '\\') { s += line[j + 1]; j += 2; } else s += line[j++]; } out.push(s); i = j + 1; }
      else if (c === '`') { let j = i + 1, s = ''; while (j < line.length && line[j] !== '`') { if (line[j] === '$' && line[j + 1] === '{') { let d = 1; j += 2; while (j < line.length && d) { if (line[j] === '{') d++; else if (line[j] === '}') d--; j++; } s += '${}'; } else if (line[j] === '\\') { s += line[j + 1]; j += 2; } else s += line[j++]; } out.push(s); i = j + 1; }
      else if (c === '/' && line[i + 1] === '/') break; else i++;
    }
    return out;
  };
  const sentences = [];
  // main.js / lib/update.js / lib/game.js / lib/report.js are split into main/*, lib/update/*, lib/game/*, lib/report/* (test/srcs.js groups). The formatter puts a long
  // string on its own line after `new Error(` / `label:` / `T(` ...: those lines are joined back first, so the scan sees every sentence it saw before the split.
  const joinWrapped = (t) => t.replace(/(\bnew Error\(|\blabel:|\bmessage:|\bdetail:|\btext =|\bconst msg =|\breturn|\bstartFailure\(|buttons:|[?:(])\s*\n\s*(?=['"`])/g, '$1 ');
  const files = [...SRCS.files('main'), ...SRCS.files('update'), ...SRCS.files('game'), 'lib/health.js', ...SRCS.files('report')];
  for (const f of files)
    joinWrapped(SRCS.read(f)).split(/\r?\n/).forEach((l, n) => {
      if (/new Error\(|\btext = |label:|\bmessage:|\bdetail:|buttons:|startFailure\(|const msg = |\breturn `|^\s*[?:] ['`]/.test(l))
        for (const s of scan(l)) if (/^([A-Z]|tlou-ii\.exe)/.test(s) && /\S+ \S+ \S+/.test(s) && !DEV_ONLY.test(s) && !LOG_ONLY.test(s)) sentences.push([f + ':' + (n + 1), s]);
    });
  // a log line that sat on a scanned line before the split (the saved-report note)
  sentences.push(['main/report.js', 'Bug report could not be sent (${}); saved for later.']);
  const nokey = sentences.filter(([, s]) => !keys.has(norm(s))).map(([w, s]) => w + ' ' + s);
  assert.deepStrictEqual(nokey, [], 'launcher sentences without a Polish entry in pl.json: ' + nokey.join(' | '));
  assert.ok(sentences.length > 40, 'the sentence scan found too little: ' + sentences.length);
}
// behaviour
assert.strictEqual(I18N.T('PLAY'), 'GRAJ'); assert.strictEqual(I18N.T('SUPPRESSED HUNTING RIFLE') === 'CICHY KARABIN MYŚLIWSKI' && I18N.T('SUPPRESSED BOW') === 'ŁUK Z TŁUMIKIEM', true);
assert.strictEqual(I18N.T('Client updated to 74 (5 files).'), 'Klient zaktualizowany do wersji 74 (5 plików).');
assert.strictEqual(I18N.T('Client updated to 74 (2 files).'), 'Klient zaktualizowany do wersji 74 (2 pliki).');
assert.strictEqual(I18N.T('Client updated to 74 (1 files).').includes('1 plik)'), true);
assert.strictEqual(I18N.T('TACTICAL SHOTGUN costs 3. You have 1 left.'), I18N.T('TACTICAL SHOTGUN') + ': koszt 3 PKT. Zostało ci 1 PKT.');
assert.strictEqual(I18N.T('WEEK 3: DAY 4'), 'TYDZIEŃ 3: DZIEŃ 4'); assert.strictEqual(I18N.T('12/13 Loadout Points'), 'Punkty wyposażenia: 12/13');
I18N.setLang('en'); assert.strictEqual(I18N.T('PLAY'), 'PLAY'); assert.strictEqual(I18N.T('WEEK 3: DAY 4'), 'WEEK 3: DAY 4');
// background sets: every image exists, every caption is translated, no image sits in two sets, no leftover files
{
  const SC = new Function(fs.readFileSync(path.join(dir, 'scenes.js'), 'utf8') + '\nreturn Scenes;')();
  const seen = new Map(), bgdir = path.join(__dirname, '..', 'assets', 'bg');
  I18N.setLang('pl');
  for (const [set, rows] of Object.entries(SC.SETS)) for (const [id, cap] of rows) {
    assert.ok(fs.existsSync(path.join(bgdir, id + '.jpg')), 'missing background ' + id);
    assert.ok(!seen.has(id), id + ' is in both ' + seen.get(id) + ' and ' + set); seen.set(id, set);
    assert.ok(pl[cap] !== undefined, 'caption not translated: ' + cap);
  }
  assert.deepStrictEqual(fs.readdirSync(bgdir).map((f) => f.replace(/\.jpg$/, '')).sort(), [...seen.keys()].sort(), 'assets/bg and scenes.js differ');
}
// real runtime messages come out in Polish (pattern entries match what the lib functions actually build)
{
  I18N.setLang('pl');
  const upd = require('../lib/update'), health = require('../lib/health');
  const isPl = (m) => I18N.T(m) !== m && !/\{[a-z]+\}/.test(I18N.T(m));
  const e = (code, where) => { const x = new Error('x'); x.code = code; return upd.explainError(x, where).message; };
  for (const m of [e('ENOSPC', 'C:\\a\\b.dat.part'), e('ENOSPC'), e('EBUSY', 'C:\\a\\b.dat.part'), e('EROFS', 'C:\\a\\b.dat'), e('EROFS'), e('ECONNRESET'), health.loopMessage(3)]) assert.ok(isPl(m), 'runtime message not translated: ' + m);
  assert.ok(!/\{|\}/.test(I18N.T(health.loopMessage(3))), 'placeholder left in the loop message');
  assert.ok(I18N.T(e('EBUSY', 'C:\\a\\b.dat.part')).includes('b.dat'), 'file name lost');
  I18N.setLang('en');
}
// Spanish (OmegaMan, src/i18n/es.json + game-es.json, registered in src/i18n/languages.js): partial on purpose, a missing entry shows the English text.
// Every es.json key is a current en.json key with the same placeholders, game-es.json covers game-pl.json, and the per-language tables never mix.
{
  const es = JSON.parse(fs.readFileSync(path.join(dir, 'i18n', 'es.json'), 'utf8')), ges = JSON.parse(fs.readFileSync(path.join(dir, 'i18n', 'game-es.json'), 'utf8'));
  const LANGS = require('../src/i18n/languages');
  assert.deepStrictEqual(LANGS.SUPPORTED_LANGS, ['en', 'pl', 'es']);
  const stale = Object.keys(es).filter((k) => !(k in en));
  assert.deepStrictEqual(stale, [], 'es.json keys that en.json does not have: ' + stale.join(' | '));
  const uniq = (s) => vars(s).split(',').filter(Boolean).filter((x, i, a) => a.indexOf(x) === i).join(',');
  for (const [k, v] of Object.entries(es)) assert.strictEqual(uniq(v), uniq(k), 'es.json placeholders differ: ' + k);
  assert.deepStrictEqual(Object.keys(gpl).filter((k) => !(k in ges)), [], 'game-es.json misses game-pl.json keys');
  for (const v of Object.values(es).concat(Object.values(ges))) assert.ok(!OLD_NAME.test(v), 'old product name in Spanish text: ' + v);
  const html = fs.readFileSync(path.join(dir, 'index.html'), 'utf8');
  for (const l of LANGS.SUPPORTED_LANGS) assert.ok(html.includes('data-lang="' + l + '"'), 'no language button for ' + l);
  const C = require('../src/i18n-core');
  C.load(pl, 'pl'); C.load(es, 'es');
  C.setLang('es'); assert.strictEqual(C.T('PLAY'), es.PLAY || 'PLAY'); assert.notStrictEqual(C.T('PLAY'), pl.PLAY);
  C.setLang('pl'); assert.strictEqual(C.T('PLAY'), 'GRAJ');
  C.setLang('es-ES'); assert.strictEqual(C.lang, 'es');
  C.setLang('en'); assert.strictEqual(C.T('PLAY'), 'PLAY');
  console.log('i18n-test: Spanish', Object.keys(es).length, 'of', Object.keys(en).length, 'launcher strings,', Object.keys(ges).length, 'game strings');
}
console.log('i18n-test OK:', Object.keys(pl).length, 'launcher strings,', Object.keys(gpl).length, 'game strings,', statics.length, 'static texts,', new Set(lits).size, 'T() literals');
