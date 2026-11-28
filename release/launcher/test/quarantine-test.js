'use strict';
// node test/quarantine-test.js : Play moves other mods' files out of the game folder (winmm's game-data check -> every search refused).
const assert = require('assert');
const fs = require('fs'),
  path = require('path');
const mkTmp = require('./tmp');
const q = require('../lib/game/quarantine');
const { ARCHIVES } = require('../lib/game/clean-list');

let passed = 0;
const T = (name, fn) => {
  try {
    fn();
    passed++;
    console.log('  ok  ' + name);
  } catch (e) {
    console.log('FAIL  ' + name + '\n' + ((e && e.stack) || e));
    process.exitCode = 1;
  }
};
const put = (gd, rel, data = 'x') => {
  const p = path.join(gd, ...rel.split('/'));
  fs.mkdirSync(path.dirname(p), { recursive: true });
  fs.writeFileSync(p, data);
};
// a sparse file of the clean size (no real bytes written)
const putSized = (gd, rel, size) => {
  const p = path.join(gd, ...rel.split('/'));
  fs.mkdirSync(path.dirname(p), { recursive: true });
  const fd = fs.openSync(p, 'w');
  fs.ftruncateSync(fd, size);
  fs.closeSync(fd);
};
const has = (gd, rel) => fs.existsSync(path.join(gd, ...rel.split('/')));
const SHD = ARCHIVES.get(q.pathHash('shaders.psarc'));
const TTS_HR = ARCHIVES.get(q.pathHash('tts1/croatian-gameplay.psarc'));
const clean = () => {
  const gd = mkTmp('q-g-');
  put(gd, 'tlou-ii.exe');
  put(gd, 'winmm.dll');
  put(gd, 't2r-ext.dll');
  put(gd, 't2r-ext.fix2.dll');
  put(gd, 'steam_api64.dll');
  put(gd, 'sl.interposer.dll');
  put(gd, 'd3d12/D3D12Core.dll');
  put(gd, 'OptiScaler.ini');
  put(gd, 'build/pc/main/paks.txt');
  putSized(gd, 'build/pc/main/shaders.psarc', SHD);
  putSized(gd, 'build/pc/main/tts1/croatian-gameplay.psarc', TTS_HR);
  return gd;
};
const logs = [];
const log = (m) => logs.push(m);

T('hash key matches the winmm manifest (known archives found by name)', () => {
  assert.strictEqual(q.pathHash('bin.psarc'), '90ad66a5fb67b1a7');
  assert.ok(SHD > 0 && TTS_HR > 0);
  assert.strictEqual(ARCHIVES.size, 101);
});

T('clean install + Twelve Weeks files: nothing moves, no error', () => {
  const gd = clean();
  const r = q.quarantineForeignFiles(gd, log);
  assert.deepStrictEqual(r.moved, []);
  assert.deepStrictEqual(r.badSize, []);
  assert.ok(!has(gd, q.MOVED_DIR));
});

T('dropped-in archive, ASI loader and proxy DLLs move; their relative path is kept; nothing is deleted', () => {
  const gd = clean();
  put(gd, 'build/pc/main/mymod.psarc', 'mod');
  put(gd, 'build/pc/main/sub/other.PSARC', 'mod2');
  put(gd, 'build/pc/main/notes.txt', 'n'); // not an archive: winmm never checks it, stays
  put(gd, 'modloader.asi', 'asi');
  put(gd, 'dxgi.dll', 'reshade');
  put(gd, 'dinput8.dll', 'loader');
  put(gd, 'LmxxfNrRuntime.dll', 'helper'); // not a proxy name: inert without its loader, stays
  const r = q.quarantineForeignFiles(gd, log);
  assert.deepStrictEqual(r.moved.sort(), ['build/pc/main/mymod.psarc', 'build/pc/main/sub/other.PSARC', 'dinput8.dll', 'dxgi.dll', 'modloader.asi'].sort());
  for (const m of r.moved) {
    assert.ok(!has(gd, m), m + ' still in place');
    assert.ok(has(gd, q.MOVED_DIR + '/' + m), m + ' not in the moved folder');
  }
  assert.strictEqual(fs.readFileSync(path.join(gd, q.MOVED_DIR, 'dxgi.dll'), 'utf8'), 'reshade');
  for (const k of ['build/pc/main/notes.txt', 'LmxxfNrRuntime.dll', 'winmm.dll', 't2r-ext.dll', 't2r-ext.fix2.dll', 'steam_api64.dll', 'sl.interposer.dll', 'd3d12/D3D12Core.dll', 'build/pc/main/shaders.psarc', 'build/pc/main/tts1/croatian-gameplay.psarc'])
    assert.ok(has(gd, k), k + ' must stay');
  assert.ok(logs.some((l) => /moved to .*twelve-weeks-moved/.test(l)));
});

T('a second move of the same name keeps the older moved copy', () => {
  const gd = clean();
  put(gd, 'dxgi.dll', 'one');
  q.quarantineForeignFiles(gd, log);
  put(gd, 'dxgi.dll', 'two');
  q.quarantineForeignFiles(gd, log);
  assert.strictEqual(fs.readFileSync(path.join(gd, q.MOVED_DIR, 'dxgi.dll'), 'utf8'), 'one');
  assert.strictEqual(fs.readFileSync(path.join(gd, q.MOVED_DIR, 'dxgi.dll.1'), 'utf8'), 'two');
});

T('the SP sandbox version.dll stays; a foreign version.dll moves', () => {
  const gd = clean();
  put(gd, 'version.dll', 'xx t2r-sp.txt yy t2r-sp.prev.log');
  assert.deepStrictEqual(q.quarantineForeignFiles(gd, log).moved, []);
  put(gd, 'version.dll', 'asi loader');
  assert.deepStrictEqual(q.quarantineForeignFiles(gd, log).moved, ['version.dll']);
});

T('a game archive of another size stops Play with the Steam file check; skipCheck only logs it', () => {
  const gd = clean();
  putSized(gd, 'build/pc/main/shaders.psarc', SHD + 16);
  assert.throws(() => q.quarantineForeignFiles(gd, log), /build\/pc\/main\/shaders\.psarc.*Verify integrity of game files/s);
  const r = q.quarantineForeignFiles(gd, log, { skipCheck: true });
  assert.strictEqual(r.badSize.length, 1);
  assert.ok(has(gd, 'build/pc/main/shaders.psarc'), 'a game archive is never moved');
});

T('OptiScaler stays under its proxy names; other loaders next to it still move', () => {
  const gd = clean();
  put(gd, 'dxgi.dll', 'xx OptiScaler working as dxgi.dll yy');
  put(gd, 'winhttp.dll', 'zz OptiScaler working as winhttp.dll zz');
  put(gd, 'OptiScaler.asi', 'OptiScaler working as native upscaler');
  put(gd, 'modloader.asi', 'asi');
  put(gd, 'dinput8.dll', 'loader');
  assert.deepStrictEqual(q.quarantineForeignFiles(gd, log).moved.sort(), ['dinput8.dll', 'modloader.asi']);
  for (const k of ['dxgi.dll', 'winhttp.dll', 'OptiScaler.asi', 'OptiScaler.ini']) assert.ok(has(gd, k), k + ' must stay');
});

T('a loader that only mentions OptiScaler moves (SCSKiller as d3d12.dll)', () => {
  const gd = clean();
  // (SCSKiller's d3d12.dll names OptiScaler in its messages: "OptiScaler's %ls, System32's D3D12CreateDevice")
  put(gd, 'd3d12.dll', "SCSKiller for Windows ... OptiScaler's %ls, System32's D3D12CreateDevice ... OptiScaler.ini");
  put(gd, 'dsound.dll', 'zz OptiScaler zz');
  // naming itself SCSKiller wins even over the OptiScaler phrase (a log string it may carry)
  put(gd, 'winhttp.dll', 'SCSKiller ... OptiScaler working as winhttp.dll');
  assert.deepStrictEqual(q.quarantineForeignFiles(gd, log).moved.sort(), ['d3d12.dll', 'dsound.dll', 'winhttp.dll']);
  assert.ok(has(gd, q.MOVED_DIR + '/d3d12.dll'));
  assert.ok(has(gd, 'd3d12/D3D12Core.dll'), "the game's own d3d12 folder stays");
});

T('a game folder without build/pc/main does not fail', () => {
  const gd = mkTmp('q-e-');
  put(gd, 'tlou-ii.exe');
  assert.deepStrictEqual(q.quarantineForeignFiles(gd, log).moved, []);
});

T('changed-file list: up to 3 names, more grouped by folder', () => {
  assert.strictEqual(q.shortList(['a/x.psarc', 'b/y.psarc']), 'a/x.psarc, b/y.psarc');
  const many = [];
  for (let i = 0; i < 36; i++) many.push(`build/pc/main/tts1/l${i}-menu.psarc`);
  for (let i = 0; i < 4; i++) many.push(`build/pc/main/speech1/s${i}.psarc`);
  assert.strictEqual(q.shortList(many), 'build/pc/main/tts1/ (36), build/pc/main/speech1/ (4)');
});

console.log(`quarantine-test: ${passed} passed`);
