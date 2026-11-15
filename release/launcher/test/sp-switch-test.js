'use strict';
// node test/sp-switch-test.js : SWITCH TO SP / MP (lib/spmode.js) and PLAY SINGLEPLAYER, on fake game folders only.
const assert = require('assert');
const fs = require('fs'), path = require('path'), crypto = require('crypto');
const mkTmp = require('./tmp');
const game = require('../lib/game');
const spmode = require('../lib/spmode');

let passed = 0;
const T = async (name, fn) => { try { await fn(); passed++; console.log('  ok  ' + name); } catch (e) { console.log('FAIL  ' + name + '\n' + (e && e.stack || e)); process.exitCode = 1; } };

const winmm = (extra = 0) => { const b = Buffer.alloc(0x60000 + extra); for (const o of [0x56402, 0x58725]) b.writeUInt32LE((Buffer.from([127, 0, 0, 1]).readUInt32LE(0) ^ 0x5A3C9E17) >>> 0, o); for (const o of [0x563F1, 0x58742]) b.writeUInt16LE(7001, o); return b; };
const put = (gd, rel, data) => { const f = path.join(gd, rel); fs.mkdirSync(path.dirname(f), { recursive: true }); fs.writeFileSync(f, data); };
// a game folder as the mod's install leaves it (see lib/game.js installMod), plus stock files and a save-game stand-in that must never move
function mkGame({ foreign = false } = {}) {
  const gd = mkTmp('sp-g-');
  put(gd, 'tlou-ii.exe', 'stub'); put(gd, 'steam_appid.txt', '2531310'); put(gd, 'build/pc/main/core.psarc', 'psarc'); put(gd, 'savedata/slot1.sav', 'SAVE'); put(gd, 'readme.txt', 'stock');
  put(gd, 'winmm.dll', winmm(7)); put(gd, 't2r-ext.dll', 'ext'); put(gd, 'tlou-ii-mp.json', '{"token":"x"}'); put(gd, 't2r-version.txt', 'v1\n');
  put(gd, 'tlou-ii-mp/sidbase.bin', crypto.randomBytes(300)); put(gd, 'tlou-ii-mp/sub/deep.dat', 'deep'); put(gd, 't2r-uc4-downed.txt', ''); put(gd, 't2r-name.txt', 'Ellie\r\n');
  put(gd, 't2r-steam/t2r-steam.exe', 'helper'); put(gd, 't2r-ext.log', 'log'); put(gd, 'winmm.dll.t2r-new', 'half');
  if (foreign) put(gd, 'winmm.dll.t2r-original', crypto.randomBytes(500));
  return gd;
}
function snap(dir, base = dir, out = {}) {   // relative path -> sha1 of every file under dir (folders by their files)
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) snap(p, base, out); else out[path.relative(base, p).replace(/\\/g, '/')] = crypto.createHash('sha1').update(fs.readFileSync(p)).digest('hex');
  }
  return out;
}
const STOCK = ['tlou-ii.exe', 'steam_appid.txt', 'build/pc/main/core.psarc', 'savedata/slot1.sav', 'readme.txt'];
const up = async () => ({ stdout: 'yes\r\n', err: null }), down = async () => ({ stdout: '', err: null });
game.testHooks.procs = async () => []; game.testHooks.helper = async () => false; game.testHooks.steam = up;
const realRename = spmode.hooks.rename;
spmode.hooks.sleep = () => {};

(async () => {
  await T('SWITCH TO SP moves every mod file to TwelveWeeks-disabled with the same relative paths; stock files and saves stay', async () => {
    const gd = mkGame(), before = snap(gd);
    const r = await game.spDisable(gd);
    assert.strictEqual(r.already, false); assert.ok(r.moved >= 8);
    assert.strictEqual(r.folder, path.join(gd, 'TwelveWeeks-disabled'));
    const after = snap(gd), dis = path.join(gd, 'TwelveWeeks-disabled');
    for (const rel of STOCK) assert.strictEqual(after[rel], before[rel], rel + ' must not move');
    for (const rel of Object.keys(before)) if (!STOCK.includes(rel)) {
      assert.ok(!(rel in after), rel + ' is still in the game folder');
      assert.strictEqual(after['TwelveWeeks-disabled/' + rel], before[rel], rel + ' missing in the disabled folder');
    }
    assert.deepStrictEqual(Object.keys(after).filter((k) => !k.startsWith('TwelveWeeks-disabled/')).sort(), [...STOCK].sort());
    assert.ok(!game.spmode.modInGame(gd, (f) => game.verifyWinmm(f, 0)));
    assert.ok(fs.existsSync(path.join(dis, spmode.JOURNAL)));
  });

  await T('idempotent: a second SWITCH TO SP changes nothing; SWITCH TO MP restores a byte-identical game folder and removes the empty disabled folder', async () => {
    const gd = mkGame(), before = snap(gd);
    await game.spDisable(gd); const mid = snap(gd);
    const again = await game.spDisable(gd); assert.strictEqual(again.already, true); assert.deepStrictEqual(snap(gd), mid);
    const r = await game.spEnable(gd);
    assert.strictEqual(r.needsRepair, false);
    assert.deepStrictEqual(snap(gd), before);
    assert.ok(!fs.existsSync(path.join(gd, 'TwelveWeeks-disabled')));
    const r2 = await game.spEnable(gd); assert.strictEqual(r2.already, true); assert.deepStrictEqual(snap(gd), before);   // idempotent too
  });

  await T('a foreign winmm.dll (kept as .t2r-original) is back as winmm.dll in SP; the mod dll waits in the disabled folder; MP swaps them back', async () => {
    const gd = mkGame({ foreign: true }), before = snap(gd);
    const foreignHash = before['winmm.dll.t2r-original'], modHash = before['winmm.dll'];
    const r = await game.spDisable(gd); assert.strictEqual(r.restoredOriginal, true);
    const s = snap(gd);
    assert.strictEqual(s['winmm.dll'], foreignHash); assert.ok(!('winmm.dll.t2r-original' in s));
    assert.strictEqual(s['TwelveWeeks-disabled/winmm.dll'], modHash);
    assert.ok(!game.spmode.modInGame(gd, (f) => game.verifyWinmm(f, 0)));   // the foreign dll is not a T2R build
    await game.spEnable(gd);
    assert.deepStrictEqual(snap(gd), before);
  });

  await T('a game folder without any mod files: nothing to move, no folder created, never throws', async () => {
    const gd = mkTmp('sp-g-'); put(gd, 'tlou-ii.exe', 'stub'); put(gd, 'savedata/a.sav', 's');
    const r = await game.spDisable(gd); assert.strictEqual(r.nothing, true); assert.ok(!fs.existsSync(path.join(gd, 'TwelveWeeks-disabled')));
  });

  await T('a foreign winmm.dll alone (another mod, no T2R files) is never moved', async () => {
    const gd = mkTmp('sp-g-'); put(gd, 'tlou-ii.exe', 'stub'); put(gd, 'winmm.dll', crypto.randomBytes(0x60000));
    const before = snap(gd); const r = await game.spDisable(gd); assert.strictEqual(r.nothing, true); assert.deepStrictEqual(snap(gd), before);
  });

  // The report layouts: Steam at G:\Steam and C:\Program Files (x86)\Steam differ only by the path prefix; the fake folders mirror a full install on top of the base MP mod.
  for (const layout of ['G:/Steam/steamapps/common/The Last of Us Part II', 'C:/Program Files (x86)/Steam/steamapps/common/The Last of Us Part II']) {
    await T('base MP mod (modloader.asi, mods\, its own winmm.dll kept as .t2r-original) is parked in SP and comes back in MP - ' + layout.slice(0, 2), async () => {
      const gd = mkGame({ foreign: true });
      put(gd, 'modloader.asi', 'asi'); put(gd, 'mods/tlou-ii-mp.psarc.ini', 'ini'); put(gd, 'mods/tlou-ii-mp.psarc', 'psarc'); put(gd, 'tlou-ii-mp.psarc', 'p2');
      put(gd, 'mods/other-sp-mod/readme.txt', 'x');
      const before = snap(gd);
      assert.ok(layout.length > 0);
      await game.spDisable(gd);
      const after = snap(gd);
      for (const rel of STOCK) assert.strictEqual(after[rel], before[rel]);
      for (const n of ['winmm.dll', 'modloader.asi', 'mods', 'tlou-ii-mp.psarc', 'winmm.dll.t2r-original']) assert.ok(!fs.existsSync(path.join(gd, n)), n + ' must not stay in the SP game folder');
      assert.ok(!game.spmode.modInGame(gd, (f) => game.verifyWinmm(f, 0)));
      await game.spEnable(gd);
      assert.deepStrictEqual(snap(gd), before);
    });
  }

  await T('already-SP install from an older launcher (base mod left in the game folder, its loader back as winmm.dll): topUp parks the leftovers, SWITCH TO MP restores everything', async () => {
    const gd = mkGame({ foreign: true });
    put(gd, 'modloader.asi', 'asi'); put(gd, 'mods/tlou-ii-mp.psarc.ini', 'ini'); put(gd, 'mods/tlou-ii-mp.psarc', 'psarc'); put(gd, 'tlou-ii-mp.psarc', 'p2'); put(gd, 'tlou-ii-mp.log', 'modloader log');
    const before = snap(gd);
    await game.spDisable(gd);
    const dis = path.join(gd, 'TwelveWeeks-disabled');
    // rebuild the old launcher's state: base mod files in the game folder, base loader restored as winmm.dll, journal without them, swapOriginal
    for (const n of ['modloader.asi', 'mods', 'tlou-ii-mp.psarc', 'tlou-ii-mp.log']) fs.renameSync(path.join(dis, n), path.join(gd, n));
    fs.renameSync(path.join(dis, 'winmm.dll.t2r-original'), path.join(gd, 'winmm.dll'));
    const jf = path.join(dis, spmode.JOURNAL), j = JSON.parse(fs.readFileSync(jf, 'utf8'));
    j.entries = j.entries.filter((e) => !['modloader.asi', 'mods', 'tlou-ii-mp.psarc', 'tlou-ii-mp.log', 'winmm.dll.t2r-original'].includes(e));
    j.ops = j.ops.filter((o) => !/(modloader\.asi|mods|tlou-ii-mp\.psarc|tlou-ii-mp\.log|winmm\.dll\.t2r-original)$/.test(o.to)); j.swapOriginal = true;
    fs.writeFileSync(jf, JSON.stringify(j));
    const r = await game.spTopUp(gd);
    assert.ok(r.moved >= 5, 'moved ' + r.moved);
    for (const n of ['winmm.dll', 'modloader.asi', 'mods', 'tlou-ii-mp.psarc', 'tlou-ii-mp.log', 'winmm.dll.t2r-original']) assert.ok(!fs.existsSync(path.join(gd, n)), n + ' must not stay in the SP game folder');
    assert.ok(!game.spmode.modInGame(gd, (f) => game.verifyWinmm(f, 0)));
    assert.strictEqual((await game.spTopUp(gd)).moved, 0);   // second call: nothing left
    await game.spEnable(gd);
    assert.deepStrictEqual(snap(gd), before);
  });

  await T('a leftover base mod (modloader.asi) in the game folder counts as the mod: PLAY SINGLEPLAYER would refuse', async () => {
    const gd = mkGame(); await game.spDisable(gd); put(gd, 'modloader.asi', 'asi');
    assert.ok(game.spmode.modInGame(gd, (f) => game.verifyWinmm(f, 0)));
  });

  await T('t2r-steam.exe still exiting: SWITCH TO SP waits for it instead of failing', async () => {
    const gd = mkGame(); let n = 0;
    game.testHooks.helperWaitMs = 5000; game.testHooks.helper = async () => ++n < 3;
    try { const r = await game.spDisable(gd); assert.strictEqual(r.already, false); assert.ok(n >= 3); }
    finally { game.testHooks.helperWaitMs = 0; game.testHooks.helper = async () => false; }
  });

  await T('game running (window or still starting) or the Steam helper running: both switches refuse with a clear message and move nothing', async () => {
    const gd = mkGame(), before = snap(gd);
    for (const procs of [[{ id: 5, window: true }], [{ id: 6, window: false }]]) {
      game.testHooks.procs = async () => procs;
      await assert.rejects(game.spDisable(gd), /game is running/); assert.deepStrictEqual(snap(gd), before);
    }
    game.testHooks.procs = async () => []; game.testHooks.helper = async () => true;
    await assert.rejects(game.spDisable(gd), /Steam helper/); assert.deepStrictEqual(snap(gd), before);
    game.testHooks.helper = async () => false;
    await game.spDisable(gd); const mid = snap(gd);
    game.testHooks.procs = async () => [{ id: 7, window: true }];
    await assert.rejects(game.spEnable(gd), /game is running/); assert.deepStrictEqual(snap(gd), mid);
    game.testHooks.procs = async () => [];
  });

  await T('no tlou-ii.exe in the folder: refused, nothing created', async () => {
    const gd = mkTmp('sp-g-'); put(gd, 'winmm.dll', winmm());
    await assert.rejects(game.spDisable(gd), /tlou-ii\.exe/); assert.ok(!fs.existsSync(path.join(gd, 'TwelveWeeks-disabled')));
  });

  await T('partial failure (disk error on the 6th move): everything is rolled back, the folder is byte-identical, no disabled folder is left', async () => {
    const gd = mkGame({ foreign: true }), before = snap(gd);
    let n = 0; spmode.hooks.rename = (a, b) => { if (++n === 6) throw Object.assign(new Error('EIO: i/o error'), { code: 'EIO' }); return realRename(a, b); };
    try { await assert.rejects(game.spDisable(gd), /Could not move/); } finally { spmode.hooks.rename = realRename; }
    assert.deepStrictEqual(snap(gd), before); assert.ok(!fs.existsSync(path.join(gd, 'TwelveWeeks-disabled')));
    await game.spDisable(gd); await game.spEnable(gd); assert.deepStrictEqual(snap(gd), before);   // and the next try works
  });

  await T('antivirus holds a file (EPERM every time): the clear message, rollback, nothing lost', async () => {
    const gd = mkGame(), before = snap(gd);
    spmode.hooks.rename = (a, b) => { if (/t2r-ext\.dll$/.test(a)) throw Object.assign(new Error('EPERM'), { code: 'EPERM' }); return realRename(a, b); };
    try { await assert.rejects(game.spDisable(gd), /antivirus/); } finally { spmode.hooks.rename = realRename; }
    assert.deepStrictEqual(snap(gd), before);
  });

  await T('EPERM that clears after a retry: the move succeeds', async () => {
    const gd = mkGame(), before = snap(gd); let n = 0;
    spmode.hooks.rename = (a, b) => { if (/t2r-ext\.dll$/.test(a) && n++ < 2) throw Object.assign(new Error('EPERM'), { code: 'EPERM' }); return realRename(a, b); };
    try { await game.spDisable(gd); } finally { spmode.hooks.rename = realRename; }
    assert.ok(!fs.existsSync(path.join(gd, 't2r-ext.dll'))); await game.spEnable(gd); assert.deepStrictEqual(snap(gd), before);
  });

  await T('failure while switching back: rolled back to the SP state (still disabled, nothing lost), the retry works', async () => {
    const gd = mkGame(), before = snap(gd);
    await game.spDisable(gd); const mid = snap(gd);
    let n = 0; spmode.hooks.rename = (a, b) => { if (++n === 4) throw Object.assign(new Error('EIO'), { code: 'EIO' }); return realRename(a, b); };
    try { await assert.rejects(game.spEnable(gd), /Could not move/); } finally { spmode.hooks.rename = realRename; }
    const after = snap(gd), strip = (o) => Object.fromEntries(Object.entries(o).filter(([k]) => !k.endsWith(spmode.JOURNAL)));
    assert.deepStrictEqual(strip(after), strip(mid));
    assert.ok(spmode.status(gd).disabled);
    await game.spEnable(gd); assert.deepStrictEqual(snap(gd), before);
  });

  await T('switch back never overwrites: a mod file that appeared in the game folder meanwhile is kept (moved aside), a failed switch puts it back', async () => {
    const gd = mkGame(); await game.spDisable(gd);
    put(gd, 't2r-ext.dll', 'newer one'); const mid = snap(gd);
    let calls = 0; spmode.hooks.rename = (a, b) => { if (++calls === 3) { const e = new Error('nope'); e.code = 'EIO'; throw e; } return realRename(a, b); };
    try { await assert.rejects(game.spEnable(gd), /Could not move/); } finally { spmode.hooks.rename = realRename; }
    const noJ = (o) => { const c = { ...o }; delete c['TwelveWeeks-disabled/.f2-sp-journal.json']; return c; };   // the rollback rewrites the journal as the committed state
    assert.deepStrictEqual(noJ(snap(gd)), noJ(mid)); assert.ok(spmode.status(gd).disabled);
    await game.spEnable(gd);
    const kept = Object.entries(snap(gd)).find(([k]) => /^TwelveWeeks-sp-tools\/replaced-\d+\/t2r-ext\.dll$/.test(k));
    assert.ok(kept && kept[1] === crypto.createHash('sha1').update('newer one').digest('hex'));
  });

  await T('switch back with a file missing from the disabled folder: refused with a clear message, nothing moves', async () => {
    const gd = mkGame(); await game.spDisable(gd);
    fs.unlinkSync(path.join(gd, 'TwelveWeeks-disabled', 't2r-ext.dll')); const mid = snap(gd);
    await assert.rejects(game.spEnable(gd), /t2r-ext\.dll is missing/); assert.deepStrictEqual(snap(gd), mid);
  });

  await T('a damaged winmm.dll that comes back is set aside (never left to crash the game); needsRepair is reported', async () => {
    const gd = mkGame(); await game.spDisable(gd);
    fs.writeFileSync(path.join(gd, 'TwelveWeeks-disabled', 'winmm.dll'), crypto.randomBytes(0x60000));   // damaged while it lay there
    const r = await game.spEnable(gd);
    assert.strictEqual(r.needsRepair, true); assert.ok(!fs.existsSync(path.join(gd, 'winmm.dll')));
    assert.ok(fs.existsSync(path.join(gd, 'TwelveWeeks-disabled', 'winmm.dll.damaged')));
    assert.ok(fs.existsSync(path.join(gd, 't2r-ext.dll')));
  });

  await T('crash recovery: a journal left open by a killed launcher (2 of 6 moves done) is undone at the next start', async () => {
    const gd = mkGame({ foreign: true }), before = snap(gd), d = path.join(gd, 'TwelveWeeks-disabled');
    const names = ['t2r-ext.dll', 'tlou-ii-mp.json', 't2r-version.txt', 't2r-name.txt', 'tlou-ii-mp', 't2r-steam'];
    const ops = names.map((n) => ({ from: path.join(gd, n), to: path.join(d, n) }));
    fs.mkdirSync(d); fs.renameSync(ops[0].from, ops[0].to); fs.renameSync(ops[1].from, ops[1].to);   // the process died here
    fs.writeFileSync(path.join(d, spmode.JOURNAL), JSON.stringify({ v: 1, txn: 'disable', state: 'open', ops, done: 1 }));   // (the journal lags one move behind)
    assert.strictEqual(spmode.status(gd).disabled, false);
    const r = spmode.recover(gd); assert.strictEqual(r.recovered, true);
    assert.deepStrictEqual(snap(gd), before); assert.ok(!fs.existsSync(d));
    assert.strictEqual(spmode.recover(gd).recovered, false);   // idempotent
  });

  await T('crash recovery of a cut-off switch back: undone to the SP state', async () => {
    const gd = mkGame(), before = snap(gd); await game.spDisable(gd);
    const d = path.join(gd, 'TwelveWeeks-disabled'), jd = spmode.status(gd).journal;
    const ops = jd.entries.slice(0, 3).map((n) => ({ from: path.join(d, n), to: path.join(gd, n) }));
    fs.renameSync(ops[0].from, ops[0].to); fs.renameSync(ops[1].from, ops[1].to);
    fs.writeFileSync(path.join(d, spmode.JOURNAL), JSON.stringify({ v: 1, txn: 'enable', state: 'open', ops, done: 2, prevOps: jd.ops, entries: jd.entries, swapOriginal: jd.swapOriginal }));
    assert.strictEqual(spmode.recover(gd).recovered, true);
    assert.ok(spmode.status(gd).disabled); assert.ok(!fs.existsSync(path.join(gd, 't2r-ext.dll')));
    await game.spEnable(gd); assert.deepStrictEqual(snap(gd), before);
  });

  await T('mode survives a launcher restart: the disabled folder is the truth, the saved flag only covers "nothing to move"', async () => {
    const vw = (f) => game.verifyWinmm(f, 0);
    const gd = mkGame(); await game.spDisable(gd);
    assert.strictEqual(spmode.modeFor(gd, false, vw), true);       // flag lost, folder says SP
    assert.strictEqual(spmode.modeFor(gd, true, vw), true);
    await game.spEnable(gd);
    assert.strictEqual(spmode.modeFor(gd, true, vw), false);       // stale flag, the mod is installed again -> MP
    assert.strictEqual(spmode.modeFor(gd, false, vw), false);
    const clean = mkTmp('sp-g-'); put(clean, 'tlou-ii.exe', 'x');
    assert.strictEqual(spmode.modeFor(clean, true, vw), true);     // SP chosen with nothing to move stays SP
    assert.strictEqual(spmode.modeFor(clean, false, vw), false);
    put(clean, 'winmm.dll', winmm());                              // Play.bat / a reinstall put the mod back
    assert.strictEqual(spmode.modeFor(clean, true, vw), false);
    const main = require('./srcs').text('main');
    assert.ok(/sp_mode: false/.test(main) && /key === 'sp_mode'/.test(main), 'sp_mode is a saved setting the renderer cannot set');
    assert.ok(/spmode\.recover\(config\.values\.game_dir\)/.test(main), 'an unfinished switch is recovered at start');
  });

  const sbx = () => Buffer.concat([crypto.randomBytes(200), Buffer.from('t2r-sp.txt t2r-sp.prev.log '), crypto.randomBytes(50)]);
  const TD = 'TwelveWeeks-sp-tools';
  await T('SP tools: the user clash (t2r-sp.txt in the disabled folder from an old launcher AND a fresh one written in SP) no longer fails SWITCH TO MP; both are kept', async () => {
    const gd = mkGame(); put(gd, 't2r-sp.txt', 'old');
    await game.spDisable(gd);   // new launcher: SP tool is never moved into the disabled folder
    assert.ok(fs.existsSync(path.join(gd, 't2r-sp.txt')) && !fs.existsSync(path.join(gd, 'TwelveWeeks-disabled', 't2r-sp.txt')));
    // an old launcher had moved it there; SP then wrote a new one
    fs.renameSync(path.join(gd, 't2r-sp.txt'), path.join(gd, 'TwelveWeeks-disabled', 't2r-sp.txt'));
    const jf = path.join(gd, 'TwelveWeeks-disabled', spmode.JOURNAL), j = JSON.parse(fs.readFileSync(jf, 'utf8')); j.entries.push('t2r-sp.txt'); fs.writeFileSync(jf, JSON.stringify(j));
    put(gd, 't2r-sp.txt', 'fresh'); put(gd, 't2r-sp.log', 'l'); put(gd, 't2r-sp-force.txt', 'f'); put(gd, 'version.dll', sbx());
    assert.ok(!game.spmode.modInGame(gd, (f) => game.verifyWinmm(f, 0)));
    const r = await game.spEnable(gd);
    const s = snap(gd);
    assert.strictEqual(s[TD + '/t2r-sp.txt'], crypto.createHash('sha1').update('fresh').digest('hex'));
    assert.ok(Object.keys(s).some((k) => k.startsWith(TD + '/t2r-sp.txt.')), 'older copy kept');
    for (const n of ['t2r-sp.log', 't2r-sp-force.txt', 'version.dll']) assert.ok((TD + '/' + n) in s && !(n in s), n);
    assert.ok(!('t2r-sp.txt' in s) && 't2r-ext.dll' in s && !fs.existsSync(path.join(gd, 'TwelveWeeks-disabled')));
    assert.strictEqual(r.toolsMoved, 5);
    // and back to SP: tools return, backups stay
    await game.spDisable(gd); const b = snap(gd);
    for (const n of ['t2r-sp.txt', 't2r-sp.log', 't2r-sp-force.txt', 'version.dll']) assert.ok(n in b, n + ' back');
    assert.ok(Object.keys(b).some((k) => k.startsWith(TD + '/t2r-sp.txt.')));
    assert.ok(!(await game.spDisable(gd)).moved);
  });
  await T('SP tools: a foreign version.dll is never moved; PLAY SINGLEPLAYER check ignores SP tools', async () => {
    const gd = mkGame(); put(gd, 'version.dll', crypto.randomBytes(900)); put(gd, 't2r-sp.txt', 'x');
    await game.spDisable(gd); await game.spEnable(gd);
    assert.ok(fs.existsSync(path.join(gd, 'version.dll')) && !fs.existsSync(path.join(gd, TD, 'version.dll')));
    const sp = mkTmp('sp-g-'); put(sp, 'tlou-ii.exe', 'x'); put(sp, 't2r-sp.txt', 'x'); put(sp, 'version.dll', sbx()); put(sp, 'winmm.dll', sbx());
    assert.ok(!spmode.modInGame(sp, (f) => game.verifyWinmm(f, 0)));
    assert.ok(!spmode.plan(sp, { verifyWinmm: (f) => game.verifyWinmm(f, 0) }).items.length);
  });
  await T('SP tools: rollback of a failed SWITCH TO MP puts the tools back; the retry works', async () => {
    const gd = mkGame(); await game.spDisable(gd); put(gd, 't2r-sp.txt', 'x'); put(gd, 'version.dll', sbx()); const mid = snap(gd);
    let n = 0; spmode.hooks.rename = (a, b) => { if (++n === 4) throw Object.assign(new Error('EIO'), { code: 'EIO' }); return realRename(a, b); };
    try { await assert.rejects(game.spEnable(gd), /Could not move/); } finally { spmode.hooks.rename = realRename; }
    const strip = (o) => Object.fromEntries(Object.entries(o).filter(([k]) => !k.endsWith(spmode.JOURNAL)));
    assert.deepStrictEqual(strip(snap(gd)), strip(mid)); assert.ok(spmode.status(gd).disabled);
    await game.spEnable(gd); assert.ok((TD + '/t2r-sp.txt') in snap(gd));
  });

  await T('PLAY SINGLEPLAYER (Windows): starts the same tlou-ii.exe as MP, never steam://, writes nothing into the game folder, starts no helper', async () => {
    const gd = mkGame(); await game.spDisable(gd); const before = snap(gd);
    const urls = [], logs = [], spawned = [];
    game.testHooks.spawn = async (exe, cwd) => { spawned.push([exe, cwd]); };
    try {
      const r = await game.playSingleplayer({ cfg: { game_dir: gd }, openUrl: async (u) => { urls.push(u); }, log: (m) => logs.push(m), noWait: true, platform: 'win32' });
      assert.deepStrictEqual(urls, []); assert.strictEqual(r.url, null);
      assert.deepStrictEqual(spawned, [[path.join(gd, 'tlou-ii.exe'), gd]]);
    } finally { game.testHooks.spawn = null; }
    assert.ok(/spawnGameExe\(cfg\)/.test(game.playSingleplayer.toString()), 'SP uses the same spawn as MP');
    assert.deepStrictEqual(snap(gd), before);
    const main = require('./srcs').text('main');
    assert.ok(game.spmode.STEAM_APP_ID === '2531310' && /STEAM_APP_ID = '2531310'/.test(require('./srcs').text('game')));
    assert.ok(!/startSteamHelper|nameToken/.test(game.playSingleplayer.toString()));
  });

  await T('PLAY SINGLEPLAYER: the Steam gate still applies; a mod in the game folder, a running game or a missing exe refuse', async () => {
    const gd = mkGame(); await game.spDisable(gd); const o = (g, extra = {}) => ({ cfg: { game_dir: g }, openUrl: async () => { throw new Error('must not open'); }, log() {}, noWait: true, platform: 'win32', ...extra });
    game.testHooks.steam = down; await assert.rejects(game.playSingleplayer(o(gd)), (e) => e.message === game.STEAM_MSG); game.testHooks.steam = up;
    await assert.rejects(game.playSingleplayer(o(mkGame())), /mod files are in the game folder again/);
    game.testHooks.procs = async () => [{ id: 9, window: true }]; await assert.rejects(game.playSingleplayer(o(gd)), /already running/); game.testHooks.procs = async () => [];
    await assert.rejects(game.playSingleplayer(o(mkTmp('sp-g-'))), /tlou-ii\.exe was not found/);
    await assert.rejects(game.playSingleplayer(o('')), /not set/);
  });

  await T('PLAY SINGLEPLAYER: waits for the window of the started game (and reports it)', async () => {
    const gd = mkGame(); await game.spDisable(gd); let calls = 0; game.testHooks.spawn = async () => {};
    game.testHooks.procs = async () => (++calls <= 1 ? [] : [{ id: 3, window: true }]);
    const logs = [];
    const r = await game.playSingleplayer({ cfg: { game_dir: gd }, openUrl: async () => {}, log: (m) => logs.push(m), platform: 'win32', linuxDeps: { pollMs: 5 } });
    game.testHooks.procs = async () => []; game.testHooks.spawn = null;
    assert.strictEqual(r.windowUp, true); assert.ok(logs.some((l) => /Game window up/.test(l)));
  });

  await T('updates in SP mode never touch the game folder: Play / re-install / name / loadout writes are refused or skipped, the UI skips the client update', async () => {
    const main = require('./srcs').text('main'), app = require('./srcs').text('app');   // main.js + main/*, src/app.js + src/app/*
    assert.ok(/async function runPlay[\s\S]{0,700}if \(mode\.current\(\)\)\s*throw/.test(main), 'runPlay refuses in SP');
    assert.ok(/game:reinstall[\s\S]{0,250}if \(mode\.current\(\)\)\s*throw/.test(main), 're-install refuses in SP');
    assert.ok(/function writeNameFile[\s\S]{0,500}if \(mode\.current\(\)\) return/.test(main), 'the name file is not written in SP');
    assert.ok(/loadoutDir = \(\) =>\s*game\.isGameDir\(config\.values\.game_dir\) && !mode\.current\(\)/.test(main), 'no loadout files in SP');
    assert.ok(/if \(mode\.current\(\)\) return \{ \.\.\.out, sp: true \}/.test(main), 'no server / daemon traffic in SP');
    assert.ok(/!sp && state\.toDownload[\s\S]{0,80}doUpdate\('update'\)/.test(app) && /!sp && state\.launcherUpdate/.test(app) && /u\.hidden = !need \|\| sp/.test(app), 'doPlay / UPDATE skip the client update in SP');
    // the client update itself (lib/update.js) only writes the client folder, never the game folder
    const gd = mkGame(); await game.spDisable(gd); const before = snap(gd);
    const clientDir = mkTmp('sp-c-'); put(clientDir, 'version.txt', 'v2\n'); put(clientDir, 'winmm.dll', winmm(1));
    assert.deepStrictEqual(snap(gd), before);
    assert.ok(game.modCurrent(clientDir, { game_dir: gd }) === false);   // the mod counts as not installed while it is switched off: only SWITCH TO MP + PLAY installs it
  });

  await T('UI wiring: the button sits left of the ENTER - SELECT hints with the menu-button style; Polish text; neutral imagery and music', async () => {
    const dir = path.join(__dirname, '..', 'src'), html = fs.readFileSync(path.join(dir, 'index.html'), 'utf8'), app = require('./srcs').text('app');
    assert.ok(/<button class="mi mi-sw" id="mi-switch"[^>]*><i><\/i><span id="mi-switch-t">SWITCH TO SP<\/span><\/button><span class="keys">/.test(html));
    const pl = JSON.parse(fs.readFileSync(path.join(dir, 'i18n', 'pl.json'), 'utf8'));
    for (const k of ['SWITCH TO SP', 'SWITCH TO MP', 'PLAY SINGLEPLAYER', 'The mod was moved to {folder}. The game now starts without it.']) assert.ok(pl[k] && pl[k] !== k, k);
    assert.strictEqual(pl['SWITCH TO MP'], 'PRZEŁĄCZ NA MP');
    assert.ok(/Clan\.summary\(null\)/.test(app) && /sp = !!on/.test(app), 'SP resets to the neutral clan-less state (Scenes.setFaction neutral + Mus.setTheme neutral)');
    const scenes = fs.readFileSync(path.join(dir, 'scenes.js'), 'utf8'), clan = fs.readFileSync(path.join(dir, 'clan.js'), 'utf8');
    assert.ok(/Scenes\.setFaction\(r && r\.ok && r\.hasClan \? [^;]*: 'neutral'\)/.test(clan) && /Mus\.setTheme\(n\)/.test(scenes));
    assert.ok(/const spReady = detect\.then\(\(\) => loadSpStatus\(\)\)/.test(app) && /async function loadClanSummary\(\) \{\s*if \(sp\) return/.test(app), 'startup in SP never asks for the clan');
    assert.ok(/const t = ' *$|body\.sp-mode/.test(fs.readFileSync(path.join(dir, 'neat.css'), 'utf8')));
  });

  await T('SWITCH TO MP with a mod entry written again in SP mode (tlou-ii-mp/sidbase.bin): the parked copy wins, the stray one goes to TwelveWeeks-sp-tools/replaced-*', async () => {
    const gd = mkGame(), before = snap(gd);
    await game.spDisable(gd);
    put(gd, 'tlou-ii-mp/sidbase.bin', 'STRAY'); put(gd, 't2r-ext.dll', 'stray-ext');
    const r = await game.spEnable(gd);
    assert.ok(!r.already);
    const now = snap(gd);
    for (const [k, v] of Object.entries(before)) assert.strictEqual(now[k], v, k);
    const rep = Object.keys(now).filter((k) => /^TwelveWeeks-sp-tools\/replaced-\d+\//.test(k)).map((k) => k.replace(/^TwelveWeeks-sp-tools\/replaced-\d+\//, '')).sort();
    assert.deepStrictEqual(rep, ['t2r-ext.dll', 'tlou-ii-mp/sidbase.bin']);
    assert.ok(!fs.existsSync(path.join(gd, 'TwelveWeeks-disabled')));
  });

  // ---- product rename: an install that sits in SP mode has its mod parked in the old folder names, with absolute journal paths into them
  const toLegacy = (gd) => {   // turn a current SP state into the one the launcher before the rename left
    const cur = path.join(gd, spmode.DIR_NAME), old = path.join(gd, spmode.LEGACY_DIR_NAME);
    const curT = path.join(gd, spmode.TOOLS_DIR), oldT = path.join(gd, spmode.LEGACY_TOOLS_DIR);
    const jf = path.join(cur, spmode.JOURNAL);
    const txt = fs.readFileSync(jf, 'utf8').split(JSON.stringify(cur).slice(1, -1)).join(JSON.stringify(old).slice(1, -1));
    fs.writeFileSync(jf, txt);
    realRename(cur, old);
    if (fs.existsSync(curT)) realRename(curT, oldT);
  };

  await T('rename migration: SP mode parked in Factions2-disabled moves to TwelveWeeks-disabled on the first journal read, journal paths follow, SWITCH TO MP restores the game folder', async () => {
    const gd = mkGame(), before = snap(gd);
    await game.spDisable(gd);
    put(gd, path.join(spmode.TOOLS_DIR, 't2r-sp.txt'), 'tool');   // an SP tool parked while MP was active (an older state, kept as is)
    toLegacy(gd);
    const oldJ = JSON.parse(fs.readFileSync(path.join(gd, spmode.LEGACY_DIR_NAME, spmode.JOURNAL), 'utf8'));
    assert.ok(oldJ.ops.every((o) => o.to.includes(spmode.LEGACY_DIR_NAME)), 'fixture: journal points into the old folder');
    const st = spmode.status(gd);
    assert.strictEqual(st.disabled, true);
    assert.strictEqual(st.folder, path.join(gd, spmode.DIR_NAME));
    assert.ok(!fs.existsSync(path.join(gd, spmode.LEGACY_DIR_NAME)) && !fs.existsSync(path.join(gd, spmode.LEGACY_TOOLS_DIR)), 'old folders renamed');
    assert.ok(fs.existsSync(path.join(gd, spmode.TOOLS_DIR, 't2r-sp.txt')));
    const onDisk = JSON.parse(fs.readFileSync(path.join(gd, spmode.DIR_NAME, spmode.JOURNAL), 'utf8'));
    assert.ok(onDisk.ops.every((o) => !o.to.includes(spmode.LEGACY_DIR_NAME) && !o.from.includes(spmode.LEGACY_DIR_NAME)), 'journal rewritten on disk');
    assert.strictEqual(game.spmode.modeFor(gd, true, (f) => game.verifyWinmm(f, 0)), true, 'still SP');
    const r = await game.spEnable(gd);
    assert.ok(!r.already);
    const now = snap(gd);
    for (const [k, v] of Object.entries(before)) assert.strictEqual(now[k], v, k);
    assert.ok(!fs.existsSync(path.join(gd, spmode.DIR_NAME)));
  });

  await T('rename migration: a folder rename that fails (held open) leaves SP mode working from the old folder; a later read renames it; a crash between rename and journal rewrite is healed', async () => {
    const gd = mkGame(), before = snap(gd);
    await game.spDisable(gd);
    toLegacy(gd);
    const oldDir = path.join(gd, spmode.LEGACY_DIR_NAME);
    spmode.hooks.rename = (a, b) => { if (a === oldDir) { const e = new Error('busy'); e.code = 'EBUSY'; throw e; } return realRename(a, b); };
    try {
      const st = spmode.status(gd);
      assert.strictEqual(st.disabled, true, 'old folder still read');
      assert.strictEqual(st.folder, oldDir);
      assert.strictEqual(spmode.disabledDir(gd), oldDir);
    } finally { spmode.hooks.rename = realRename; }
    // crash case: the folder was renamed but the journal still names the old paths
    realRename(oldDir, path.join(gd, spmode.DIR_NAME));
    const j = spmode.status(gd).journal;
    assert.ok(j.ops.every((o) => !o.to.includes(spmode.LEGACY_DIR_NAME)), 'journal paths rewritten on read');
    await game.spEnable(gd);
    assert.deepStrictEqual(snap(gd), before);
  });

  await T('rename migration: an SP switch never parks the old folders themselves, and both names present keeps the old one apart (removed only when empty)', async () => {
    const gd = mkGame();
    fs.mkdirSync(path.join(gd, spmode.LEGACY_TOOLS_DIR));   // empty leftover
    fs.mkdirSync(path.join(gd, spmode.TOOLS_DIR));
    put(gd, path.join(spmode.TOOLS_DIR, 't2r-sp.txt'), 'tool');
    assert.ok(!spmode.plan(gd, { verifyWinmm: (f) => game.verifyWinmm(f, 0) }).items.some((n) => /disabled|sp-tools/i.test(n)));
    spmode.migrateLegacy(gd);
    assert.ok(!fs.existsSync(path.join(gd, spmode.LEGACY_TOOLS_DIR)), 'empty old folder removed');
    assert.deepStrictEqual(spmode.migrateLegacy(gd), [], 'idempotent');
  });

  console.log(`sp-switch-test: ${passed} passed`);
})();
