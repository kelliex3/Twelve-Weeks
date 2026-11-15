'use strict';
// node test/migration-test.js : product rename. An install from before it (install root, runtime exe, AppUserModelID, settings folder,
// shortcuts and taskbar pins under the old names) is moved to the new names by the first start of a new build, silently and idempotently.
// The SP parking folders are covered in sp-switch-test.js.
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const mkTmp = require('./tmp');
const inst = require('../main/install');

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
const w = (f, t) => {
  fs.mkdirSync(path.dirname(f), { recursive: true });
  fs.writeFileSync(f, t);
};
const r = (f) => fs.readFileSync(f, 'utf8');
const busy = (code = 'EBUSY') => {
  const e = new Error('held open');
  e.code = code;
  return e;
};
const noWait = { tries: 3, waitMs: 0, sleep: () => {} };

// .lnk stand-ins: JSON with the fields shell.readShortcutLink returns
const readShortcut = (f) => {
  try {
    return JSON.parse(r(f));
  } catch {
    return null;
  }
};
const writes = [];
const writeShortcut = (f, o) => {
  writes.push(f);
  w(f, JSON.stringify(o));
};

/** A temp world: LOCALAPPDATA with the old install, APPDATA with links and pins, a portable unpack dir with the new build. */
function world() {
  const base = mkTmp('mig-');
  const env = { APPDATA: path.join(base, 'Roaming'), USERPROFILE: path.join(base, 'profile'), LOCALAPPDATA: path.join(base, 'Local') };
  const legacyRoot = path.join(env.LOCALAPPDATA, inst.LEGACY_ROOT_NAME);
  const root = path.join(env.LOCALAPPDATA, inst.ROOT_NAME);
  const legacyExe = path.join(legacyRoot, inst.LEGACY_ROOT_EXE);
  // the old install: runtime, shim, one payload, markers
  w(legacyExe, 'OLD-RUNTIME');
  w(path.join(legacyRoot, 'ffmpeg.dll'), 'dll');
  w(path.join(legacyRoot, 'resources', 'app', 'package.json'), JSON.stringify({ name: 'factions2-launcher', productName: inst.LEGACY_ROOT_NAME }));
  w(path.join(legacyRoot, 'resources', 'app', 'main.js'), 'old shim');
  w(path.join(legacyRoot, 'app-0.1.41', 'app.asar'), 'ASAR-OLD');
  w(path.join(legacyRoot, 'app-0.1.41', 'ok'), '1');
  w(path.join(legacyRoot, 'runtime.ok'), '1');
  w(path.join(legacyRoot, 'shortcuts.ok'), '1');
  w(path.join(legacyRoot, 'portable-dir.txt'), path.join(base, 'Desktop'));
  w(path.join(base, 'Desktop', 'client', 'winmm.dll'), 'x');
  const oldLink = JSON.stringify({ target: legacyExe, appUserModelId: inst.LEGACY_AUMID });
  const [startDir, deskDir] = inst.shortcutDirs(env);
  const [taskbar] = inst.pinDirs(env);
  w(path.join(startDir, inst.LEGACY_ROOT_NAME + '.lnk'), oldLink);
  w(path.join(deskDir, inst.DISPLAY + '.lnk'), oldLink);   // renamed by a build before this one: new name, old target
  w(path.join(taskbar, inst.LEGACY_ROOT_NAME + '.lnk'), oldLink);
  w(path.join(taskbar, 'Other App.lnk'), JSON.stringify({ target: 'C:\\Other\\other.exe', appUserModelId: 'other' }));
  // the new build as the portable exe unpacks it
  const unpack = path.join(base, 'Temp', 'unpack');
  w(path.join(unpack, inst.DISPLAY + '.exe'), 'NEW-RUNTIME');
  w(path.join(unpack, 'ffmpeg.dll'), 'dll');
  w(path.join(unpack, 'resources', 'app.asar'), 'ASAR-NEW');
  const boot = (extra = {}) =>
    inst.bootstrap({
      root,
      legacyRoot,
      version: '0.2.0',
      execPath: path.join(unpack, inst.DISPLAY + '.exe'),
      resourcesPath: path.join(unpack, 'resources'),
      env,
      readShortcut,
      writeShortcut,
      log: () => {},
      tmpDir: path.join(base, 'Temp'),
      retry: noWait,
      ...extra,
    });
  return { base, env, root, legacyRoot, legacyExe, startDir, deskDir, taskbar, boot };
}

T('identifiers: new names, old ones kept only for the migration', () => {
  assert.strictEqual(inst.ROOT_NAME, 'Twelve Weeks Launcher');
  assert.strictEqual(inst.ROOT_EXE, 'Twelve Weeks Launcher.exe');
  assert.strictEqual(inst.AUMID, 'org.twelveweeks.launcher');
  assert.strictEqual(inst.LEGACY_AUMID, 'org.factions2.launcher');
  const pkg = require('../package.json');
  assert.strictEqual(pkg.name, 'twelve-weeks-launcher');
  assert.strictEqual(pkg.build.appId, inst.AUMID);
  assert.strictEqual(pkg.build.linux.executableName, 'twelve-weeks-launcher');
  assert.ok(pkg.build.deb.fpm.includes('--replaces=factions2-launcher'), 'the deb replaces the old package');
  assert.strictEqual(inst.installRoot({ LOCALAPPDATA: 'X:\\L' }), path.join('X:\\L', 'Twelve Weeks Launcher'));
  assert.strictEqual(inst.legacyInstallRoot({ LOCALAPPDATA: 'X:\\L' }), path.join('X:\\L', 'Factions II Launcher'));
  assert.strictEqual(inst.legacyInstallRoot({ LOCALAPPDATA: 'X:\\L', F2_INSTALL_ROOT: 'X:\\T' }), null, 'a test root never migrates the real install');
});

T('update from an old install: the root moves (runtime renamed, not rewritten), payloads kept, new payload added, links and pins retargeted', () => {
  const W = world();
  writes.length = 0;
  const exe = W.boot();
  assert.strictEqual(exe, path.join(W.root, 'Twelve Weeks Launcher.exe'));
  assert.ok(!fs.existsSync(W.legacyRoot), 'old root moved');
  assert.strictEqual(r(exe), 'OLD-RUNTIME', 'the moved runtime is kept (written once, never replaced)');
  assert.ok(!fs.existsSync(path.join(W.root, inst.LEGACY_ROOT_EXE)), 'no exe left under the old name');
  assert.deepStrictEqual(inst.listPayloads(W.root).map((p) => p.ver), ['0.2.0', '0.1.41']);
  assert.strictEqual(JSON.parse(r(path.join(W.root, 'resources', 'app', 'package.json'))).name, 'twelve-weeks-launcher', 'shim package renamed');
  assert.strictEqual(r(path.join(W.root, 'resources', 'app', 'main.js')), inst.SHIM_MAIN);
  assert.strictEqual(inst.rememberedPortableDir(W.root), path.join(W.base, 'Desktop'), 'portable-dir.txt came along');
  const start = path.join(W.startDir, inst.DISPLAY + '.lnk'),
    desk = path.join(W.deskDir, inst.DISPLAY + '.lnk'),
    pin = path.join(W.taskbar, inst.LEGACY_ROOT_NAME + '.lnk');
  assert.ok(!fs.existsSync(path.join(W.startDir, inst.LEGACY_ROOT_NAME + '.lnk')), 'old-name Start link renamed');
  for (const f of [start, desk, pin]) {
    const l = readShortcut(f);
    assert.strictEqual(l.target, exe, f + ' target');
    assert.strictEqual(l.appUserModelId, inst.AUMID, f + ' AUMID');
  }
  assert.ok(fs.existsSync(pin), 'the pin keeps its file name');
  assert.strictEqual(readShortcut(path.join(W.taskbar, 'Other App.lnk')).appUserModelId, 'other', 'other pins untouched');
  assert.ok(fs.existsSync(path.join(W.root, 'shortcuts.ok')));
  // second start of the same build / a later update: nothing more to do
  writes.length = 0;
  W.boot();
  assert.strictEqual(writes.length, 0, 'idempotent: no link written again');
  assert.strictEqual(inst.migrateShortcuts({ env: W.env, root: W.root, legacyRoot: W.legacyRoot, readShortcut, writeShortcut }), 0);
});

T('a deleted Desktop link stays deleted; a missing Start link is written', () => {
  const W = world();
  fs.rmSync(path.join(W.deskDir, inst.DISPLAY + '.lnk'));
  fs.rmSync(path.join(W.startDir, inst.LEGACY_ROOT_NAME + '.lnk'));
  W.boot();
  assert.ok(!fs.existsSync(path.join(W.deskDir, inst.DISPLAY + '.lnk')));
  assert.strictEqual(readShortcut(path.join(W.startDir, inst.DISPLAY + '.lnk')).appUserModelId, inst.AUMID);
});

T('old launcher still quitting: the root rename is retried until its exe lets go', () => {
  const W = world();
  let n = 0;
  W.boot({ retry: { tries: 5, waitMs: 0, sleep: () => {}, rename: (a, b) => { if (++n < 3) throw busy('EPERM'); fs.renameSync(a, b); } } });
  assert.strictEqual(n, 3);
  assert.ok(!fs.existsSync(W.legacyRoot) && fs.existsSync(path.join(W.root, 'Twelve Weeks Launcher.exe')));
});

T('old root held open for good: fresh install into the new root, portable-dir.txt copied, the old root removed later', () => {
  const W = world();
  const exe = W.boot({ retry: { ...noWait, rename: () => { throw busy(); } } });
  assert.strictEqual(r(exe), 'NEW-RUNTIME', 'fresh runtime from the portable');
  assert.ok(fs.existsSync(W.legacyRoot), 'old root left alone for now');
  assert.strictEqual(inst.rememberedPortableDir(W.root), path.join(W.base, 'Desktop'));
  assert.strictEqual(readShortcut(path.join(W.taskbar, inst.LEGACY_ROOT_NAME + '.lnk')).target, exe, 'pins point at the new runtime anyway');
  assert.strictEqual(inst.removeLegacyRoot({ root: W.root, legacyRoot: W.legacyRoot }), true);
  assert.ok(!fs.existsSync(W.legacyRoot) && !fs.existsSync(W.legacyRoot + '.removing'));
  assert.strictEqual(inst.removeLegacyRoot({ root: W.root, legacyRoot: W.legacyRoot }), false, 'idempotent');
});

T('no old install: a plain first install, nothing migrated, removeLegacyRoot never touches the new root', () => {
  const W = world();
  fs.rmSync(W.legacyRoot, { recursive: true });
  const exe = W.boot();
  assert.strictEqual(r(exe), 'NEW-RUNTIME');
  assert.strictEqual(inst.removeLegacyRoot({ root: W.root, legacyRoot: W.root }), false);
  assert.ok(fs.existsSync(exe));
});

T('settings folder: moved once (token, loadouts, logs), launcher.json paths into it rewritten, then left alone', () => {
  const appData = mkTmp('mig-ud-');
  const from = path.join(appData, inst.LEGACY_ROOT_NAME),
    to = path.join(appData, inst.ROOT_NAME);
  w(path.join(from, 'launcher.json'), JSON.stringify({ token_id: 'abc', client_dir: path.join(from, 'client'), game_dir: 'G:\\Game' }));
  w(path.join(from, 'loadouts', 'one.json'), '{}');
  w(path.join(from, 'logs', 'launcher.log'), 'log');
  const res = inst.migrateUserData({ appData, retry: noWait });
  assert.deepStrictEqual(res, { dir: to, moved: true });
  assert.ok(!fs.existsSync(from));
  const cfg = JSON.parse(r(path.join(to, 'launcher.json')));
  assert.strictEqual(cfg.token_id, 'abc');
  assert.strictEqual(cfg.client_dir, path.join(to, 'client'));
  assert.strictEqual(cfg.game_dir, 'G:\\Game');
  assert.ok(fs.existsSync(path.join(to, 'loadouts', 'one.json')));
  assert.deepStrictEqual(inst.migrateUserData({ appData, retry: noWait }), { dir: to, moved: false }, 'idempotent');
  // an old launcher run again later recreates the old folder: the new one wins
  w(path.join(from, 'launcher.json'), '{}');
  assert.deepStrictEqual(inst.migrateUserData({ appData, retry: noWait }), { dir: to, moved: false });
});

T('settings folder: a new folder without launcher.json is set aside; a held-open old folder is used as it is for this run', () => {
  const appData = mkTmp('mig-ud-');
  const from = path.join(appData, inst.LEGACY_ROOT_NAME),
    to = path.join(appData, inst.ROOT_NAME);
  w(path.join(from, 'launcher.json'), '{"a":1}');
  w(path.join(to, 'Crashpad', 'x'), 'electron made this');
  const held = inst.migrateUserData({ appData, retry: { ...noWait, rename: () => { throw busy(); } } });
  assert.deepStrictEqual(held, { dir: from, moved: false });
  assert.ok(fs.existsSync(path.join(from, 'launcher.json')), 'nothing lost');
  const res = inst.migrateUserData({ appData, retry: noWait });
  assert.strictEqual(res.moved, true);
  assert.strictEqual(r(path.join(to, 'launcher.json')), '{"a":1}');
  assert.ok(fs.readdirSync(appData).some((n) => n.startsWith(inst.ROOT_NAME + '.pre-migration-')), 'the stray new folder is kept aside');
  const none = mkTmp('mig-ud-');
  assert.deepStrictEqual(inst.migrateUserData({ appData: none }), { dir: path.join(none, inst.ROOT_NAME), moved: false });
});

console.log(`migration-test: ${passed} passed`);
