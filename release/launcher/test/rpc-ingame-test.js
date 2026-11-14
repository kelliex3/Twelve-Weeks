// In-game Rich Presence: the mod's state file -> Discord card (lib/rpc/ingame.js) and the presence rate limit (lib/rpc/presence.js).
'use strict';
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const ingame = require('../lib/rpc/ingame');
const I18N = require('../src/i18n-core');

const imgs = new Set(['bookstore']);
const card = (txt, lang) => {
  I18N.setLang(lang || 'en');
  return ingame.build(ingame.parse(txt), I18N.T, { mapImages: imgs });
};

// parse: version + state are required, junk is ignored
assert.strictEqual(ingame.parse(''), null);
assert.strictEqual(ingame.parse('v=2\nst=match'), null);
assert.strictEqual(ingame.parse('v=1\nst=bogus'), null);
assert.deepStrictEqual(ingame.parse('v=1\r\nst=menu\r\nBAD KEY=x\nnoequals\n'), { v: '1', st: 'menu' });

// Supply Raid in a match
let a = card('v=1\nst=match\nmap=bookstore\nmapn=Bookstore\nmode=sr\nmoden=Supply Raid\nlv=12\nelv=9\nk=5\ndn=3\nx=1\nt0=1763054400\nnp=8\nnpmax=8\nclan=WLF\nwk=3\nday=4\nsv=14\n');
assert.strictEqual(a.details, 'Supply Raid · Bookstore');
assert.strictEqual(a.state, '12 lives left · enemy 9');
assert.deepStrictEqual(a.timestamps, { start: 1763054400 });
assert.strictEqual(a.partySize, 8);
assert.strictEqual(a.partyMax, 8);
assert.strictEqual(a.assets.largeImage, ingame.IMG_BASE + 'bookstore.jpg');
assert.strictEqual(a.assets.smallImage, ingame.IMG_BASE + 'fac-wlf-bw.png'); // the faction badge
assert.strictEqual(a.assets.smallText, 'WLF · Week 3, day 4 · 14 survivors');
assert.strictEqual(a.assets.largeText, 'Bookstore · 5 kills · 3 downs · 1 execution');
assert.strictEqual(card('v=1\nst=match\nmap=bookstore\nmapn=Bookstore\n').assets.smallImage, 'https://twelve-weeks.com/assets/rpc/logo-bw.png'); // no clan yet: the logo
a = card('v=1\nst=menu\nclan=Seraphites\nwk=2\n'); // no map: logo big, faction badge
assert.strictEqual(a.assets.largeImage, 'https://twelve-weeks.com/assets/rpc/logo-bw.png');
assert.strictEqual(a.assets.smallImage, ingame.IMG_BASE + 'fac-sera-bw.png');
assert.strictEqual(a.assets.smallText, 'Seraphites · Week 2');
assert.strictEqual(a.buttons[0].url, 'https://twelve-weeks.com');
assert.strictEqual(card('v=1\nst=match\nmode=sr\nlv=1\n').state, '1 life left');

// Interrogation: count, then the safebox
a = card('v=1\nst=match\nmap=hillcrest\nmapn=Hillcrest\nmode=int\nmoden=Interrogation\nin=3\nein=2\ninn=5\n');
assert.strictEqual(a.state, 'Interrogations 3/5 · enemy 2');
assert.strictEqual(a.assets.largeImage, 'https://twelve-weeks.com/assets/rpc/logo-bw.png'); // no picture for this map: the logo
assert.strictEqual(card('v=1\nst=match\nmode=int\nin=5\ninn=5\nbox=1\n').state, 'Fighting for the safebox');

// Survivors rounds, dead / spectating, post match, menus
assert.strictEqual(card('v=1\nst=match\nmode=surv\nmoden=Survivors\nsc=2\nesc=1\nrd=4\n').state, 'Round 4 · 2–1');
assert.strictEqual(card('v=1\nst=dead\nmode=sr\nlv=3\n').state, 'Down · 3 lives left');
assert.strictEqual(card('v=1\nst=spec\nmode=sr\nlv=0\nelv=2\n').state, 'Spectating · 0 lives left · enemy 2');
a = card('v=1\nst=post\nmapn=Bookstore\nmoden=Supply Raid\nwin=1\nk=2\n');
assert.strictEqual(a.details, 'Match over · Bookstore');
assert.strictEqual(a.state, 'Victory · Supply Raid');
assert.strictEqual(a.timestamps, undefined);
assert.strictEqual(card('v=1\nst=menu\nclan=Seraphites\nwk=1\n').state, 'Seraphites · Week 1');
assert.strictEqual(card('v=1\nst=menu\n').state, 'Ready');
assert.strictEqual(card('v=1\nst=load\nmapn=Bookstore\n').details, 'Loading Bookstore');
assert.strictEqual(card('v=1\nst=search\nmoden=Supply Raid\n').details, 'Looking for a match');
// lobby / match players, the party in the menus; none after the match
assert.strictEqual(card('v=1\nst=lobby\nnp=3\nnpmax=8\n').partySize, 3);
assert.strictEqual(card('v=1\nst=menu\nnp=2\nnpmax=4\n').partySize, 2);
assert.strictEqual(card('v=1\nst=post\nnp=3\nnpmax=8\n').partySize, undefined);
assert.strictEqual(card('v=1\nst=match\nmode=int\nin=1\ninn=5\nein=5\nebox=1\n').state, 'Defending the safebox');
// Discord limits: 128 chars, a 1-char text is padded
assert.ok(card('v=1\nst=menu\nclan=' + 'x'.repeat(64) + '\n').state.length <= 128);

// every card string has a Polish (and Spanish) translation
const pl = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'src', 'i18n', 'pl.json'), 'utf8'));
const es = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'src', 'i18n', 'es.json'), 'utf8'));
const used = fs
  .readFileSync(path.join(__dirname, '..', 'lib', 'rpc', 'ingame.js'), 'utf8')
  .match(/tr\('[^']+'/g)
  .map((m) => m.slice(4, -1));
for (const k of used) {
  assert.ok(pl[k], 'pl.json lacks ' + k);
  assert.ok(es[k], 'es.json lacks ' + k);
}
I18N.load(pl, 'pl');
I18N.load(es, 'es');
a = card('v=1\nst=match\nmapn=Bookstore\nmode=sr\nmoden=Supply Raid\nlv=5\nelv=2\nk=2\n', 'pl');
assert.strictEqual(a.state, '5 żyć zostało · wróg 2');
assert.strictEqual(card('v=1\nst=match\nmode=sr\nlv=3\n', 'pl').state, '3 życia zostały');
assert.strictEqual(card('v=1\nst=match\nmode=int\nbox=1\n', 'pl').state, 'Walka o sejf');
assert.strictEqual(card('v=1\nst=match\nmapn=Overpass\nmoden=Interrogation\n', 'pl').details, 'Przesłuchanie · Wiadukt');
I18N.setLang('en');

// read: stale and pre-launch files are leftovers
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'rpc-'));
const f = path.join(dir, ingame.FILE);
fs.writeFileSync(f, 'v=1\nst=menu\n');
const now = Date.now();
assert.ok(ingame.read(dir, now - 1000, now));
assert.strictEqual(ingame.read(dir, now + 60000, now + 60000), null); // written before the launch
assert.strictEqual(ingame.read(dir, 0, now + ingame.STALE_MS + 5000), null); // no heartbeat
assert.strictEqual(ingame.read(null), null);

// presence: the in-game card replaces the launch card; changes are rate limited
(async () => {
  const service = require('../lib/rpc/service');
  const owner = require('../lib/rpc/owner');
  const sent = [];
  service.mount({ enabled: true, isReady: true, setActivity: async (x) => sent.push(x), clear: async () => sent.push(null), destroy: async () => {} });
  const presence = require('../lib/rpc/presence');
  presence.init({ values: { game_dir: dir } });
  owner.write(dir, 'launcher');
  presence.started(dir);
  presence.stop(); // no interval in the test: drive checkIngame by hand
  presence.started(dir);
  const t = Date.now() + 1000;
  fs.writeFileSync(f, 'v=1\nst=match\nmode=sr\nmoden=Supply Raid\nlv=12\n');
  await presence.checkIngame(t + 5000);
  assert.strictEqual(sent.length, 1);
  assert.ok(sent[0].timestamps.start > 0); // no t0: counts from the launch
  fs.writeFileSync(f, 'v=1\nst=match\nmode=sr\nmoden=Supply Raid\nlv=11\n');
  await presence.checkIngame(t + 8000); // same screen, 3 s later: held back
  assert.strictEqual(sent.length, 1);
  await presence.checkIngame(t + 21000); // 16 s later: sent
  assert.strictEqual(sent.length, 2);
  assert.strictEqual(sent[1].state, '11 lives left');
  await presence.checkIngame(t + 30000); // nothing changed: nothing sent
  assert.strictEqual(sent.length, 2);
  fs.writeFileSync(f, 'v=1\nst=dead\nmode=sr\nmoden=Supply Raid\nlv=10\n');
  await presence.checkIngame(t + 35000); // new screen 14 s after the last update (under 15 s, over 4 s): sent at once
  assert.strictEqual(sent.length, 3);
  await presence.game({ details: 'Playing Twelve Weeks' }); // the late launch card never covers the game's card
  assert.strictEqual(sent.length, 3);
  await presence.ended();
  assert.strictEqual(sent[3], null); // the game closed: card cleared
  presence.stop();
  fs.rmSync(dir, { recursive: true, force: true });
  console.log('rpc-ingame-test: ok');
})().catch((e) => {
  console.error(e);
  process.exit(1);
});
