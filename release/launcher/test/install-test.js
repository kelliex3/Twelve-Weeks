'use strict';
// node test/install-test.js : fixed-root layout (pin-safe self-update): runtime written once, payloads added per version, shim picks the newest,
// migration from the portable layout keeps the client\ folder and never touches an existing exe
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');
const inst = require('../main/install');

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'f2-install-'));
const mk = (...p) => path.join(tmp, ...p);
const w = (f, t) => { fs.mkdirSync(path.dirname(f), { recursive: true }); fs.writeFileSync(f, t); };

// fake portable unpack dir (what electron-builder extracts to %TEMP%): exe, dll, resources\app.asar
const unpack = mk('unpack');
w(path.join(unpack, 'Twelve Weeks Launcher.exe'), 'EXE-RUNTIME');
w(path.join(unpack, 'ffmpeg.dll'), 'dll');
w(path.join(unpack, 'locales', 'en-US.pak'), 'pak');
w(path.join(unpack, 'resources', 'app.asar'), 'ASAR-1');
w(path.join(unpack, 'resources', 'app.asar'), 'ASAR-1');
// the old portable's folder with the shipped client
const portable = mk('Desktop');
w(path.join(portable, 'client', 'winmm.dll'), 'x');

const root = mk('root');
const lnks = [];
const run = (ver, asarText, dir = portable) => {
  w(path.join(unpack, 'resources', 'app.asar'), asarText);
  return inst.bootstrap({
    root, version: ver, execPath: path.join(unpack, 'Twelve Weeks Launcher.exe'), resourcesPath: path.join(unpack, 'resources'),
    env: { PORTABLE_EXECUTABLE_DIR: dir, APPDATA: mk('appdata'), USERPROFILE: mk('profile') },
    writeShortcut: (f, o) => lnks.push({ f, o }), log: () => {}, tmpDir: mk('faketmp'),
  });
};

// 1. first run (migration from the portable layout)
const exe = run('0.1.16', 'ASAR-1');
assert.strictEqual(exe, path.join(root, 'Twelve Weeks Launcher.exe'));
assert.strictEqual(inst.AUMID, 'org.twelveweeks.launcher');
assert.strictEqual(JSON.parse(fs.readFileSync(path.join(root, 'resources', 'app', 'package.json'), 'utf8')).name, 'twelve-weeks-launcher');
assert.strictEqual(fs.readFileSync(exe, 'utf8'), 'EXE-RUNTIME');
assert(fs.existsSync(path.join(root, 'locales', 'en-US.pak')), 'runtime files copied');
assert(!fs.existsSync(path.join(root, 'resources', 'app.asar')), 'the payload is not part of the runtime');
assert(fs.existsSync(path.join(root, 'resources', 'app', 'main.js')), 'shim written');
assert.strictEqual(fs.readFileSync(path.join(root, 'app-0.1.16', 'app.asar'), 'utf8'), 'ASAR-1');
assert.strictEqual(inst.rememberedPortableDir(root), portable, 'client\ folder of the old portable remembered');
assert.strictEqual(lnks.length, 2, 'start menu + desktop shortcut');
assert(lnks.every((l) => l.o.appUserModelId === inst.AUMID && l.o.target === exe), 'shortcuts carry the AUMID and the fixed exe');
// 2. update: the runtime file is never rewritten, only a new payload folder appears
const stamp = fs.statSync(exe).mtimeMs;
fs.writeFileSync(exe, 'EXE-RUNTIME'); // same bytes; make mtime comparable
const st2 = fs.statSync(exe).mtimeMs;
w(path.join(mk('faketmp'), 'upd', 'client', 'winmm.dll'), 'x');
run('0.1.17', 'ASAR-2', path.join(mk('faketmp'), 'upd'));
assert.strictEqual(fs.statSync(exe).mtimeMs, st2, 'update must not touch the runtime exe');
assert.deepStrictEqual(inst.listPayloads(root).map((p) => p.ver), ['0.1.17', '0.1.16']);
assert.strictEqual(inst.rememberedPortableDir(root), portable, 'a temp-folder update run does not overwrite the remembered folder');
assert.strictEqual(lnks.length, 2, 'no new shortcuts on update');
// 3. an older portable never downgrades
run('0.1.16', 'ASAR-OLD');
assert.strictEqual(fs.readFileSync(path.join(root, 'app-0.1.16', 'app.asar'), 'utf8'), 'ASAR-1');
// 4. interrupted copy (no ok marker) is ignored and pruned; keep newest 2
w(path.join(root, 'app-0.1.99', 'app.asar'), 'half');
assert.deepStrictEqual(inst.listPayloads(root).map((p) => p.ver), ['0.1.17', '0.1.16']);
run('0.1.18', 'ASAR-3');
inst.prune(root, 2);
assert.deepStrictEqual(fs.readdirSync(root).filter((n) => n.startsWith('app-')).sort(), ['app-0.1.17', 'app-0.1.18']);
// 5. the shim picks the newest payload and falls back when it throws
const shim = path.join(root, 'resources', 'app', 'main.js');
const payload = (ver, body) => { const d = path.join(root, 'app-' + ver); fs.rmSync(d, { recursive: true, force: true }); w(path.join(d, 'app.asar', 'main.js'), body); w(path.join(d, 'ok'), '1'); };
// the shim uses process.execPath's dir as root: run it under a copy of node placed in the root
const fakeNode = path.join(root, 'node-copy.exe');
fs.copyFileSync(process.execPath, fakeNode);
payload('0.1.17', "global.__P='17'");
payload('0.1.18', "global.__P='18'");
const probe = (extra) => execFileSync(fakeNode, ['-e', `require(${JSON.stringify(shim)}); console.log(global.__P)`], { encoding: 'utf8', ...extra }).trim();
assert.strictEqual(probe(), '18');
payload('0.1.18', "throw new Error('boom')");
assert.strictEqual(probe(), '17', 'a payload that fails to load falls back to the previous one');
assert(fs.existsSync(path.join(root, 'shim-error.log')));
// 6. env helpers
assert.strictEqual(inst.cleanEnv({ PORTABLE_EXECUTABLE_FILE: 'x', PORTABLE_EXECUTABLE_DIR: 'y', A: '1' }).PORTABLE_EXECUTABLE_FILE, undefined);
assert.strictEqual(inst.wantsBootstrap({ PORTABLE_EXECUTABLE_FILE: 'x' }, 'linux'), false);
assert.strictEqual(inst.wantsBootstrap({}, 'win32'), false);
assert.strictEqual(inst.wantsBootstrap({ PORTABLE_EXECUTABLE_FILE: 'x', F2_INSTALL_ROOT: root }, 'win32', root), true);
// 4. a portable whose exe has another name (electron-builder unpacks <productName>.exe): the installed runtime is always ROOT_EXE
{
  const un2 = mk('unpack2');
  w(path.join(un2, 'electron.exe'), 'EXE-NEW');
  w(path.join(un2, 'resources', 'app.asar'), 'ASAR-9');
  const root2 = mk('root2');
  const exe2 = inst.bootstrap({
    root: root2, version: '0.1.25', execPath: path.join(un2, 'electron.exe'), resourcesPath: path.join(un2, 'resources'),
    env: { APPDATA: mk('appdata2'), USERPROFILE: mk('profile2') }, writeShortcut: (f, o) => lnks.push({ f, o }), log: () => {}, tmpDir: mk('faketmp'),
  });
  assert.strictEqual(exe2, path.join(root2, 'Twelve Weeks Launcher.exe'));
  assert.strictEqual(fs.readFileSync(exe2, 'utf8'), 'EXE-NEW');
  assert(!fs.existsSync(path.join(root2, 'electron.exe')), 'no second runtime exe');
  assert(lnks.slice(-2).every((l) => path.basename(l.f) === 'Twelve Weeks Launcher.lnk' && l.o.target === exe2), 'shortcuts are named after the product');
}

// 5. existing installs: the old shortcuts are renamed (same file = same target and AUMID), a present new link wins, nothing throws
{
  const envR = { APPDATA: mk('appdataR'), USERPROFILE: mk('profileR') };
  const [startDir, deskDir] = inst.shortcutDirs(envR);
  w(path.join(startDir, 'Factions II Launcher.lnk'), 'LNK-START');
  w(path.join(deskDir, 'Factions II Launcher.lnk'), 'LNK-DESK');
  w(path.join(deskDir, 'Twelve Weeks Launcher.lnk'), 'LNK-DESK-NEW');
  assert.strictEqual(inst.renameLegacyShortcuts(envR), 2);
  assert.strictEqual(fs.readFileSync(path.join(startDir, 'Twelve Weeks Launcher.lnk'), 'utf8'), 'LNK-START', 'start menu link renamed, content (target + AUMID) untouched');
  assert(!fs.existsSync(path.join(startDir, 'Factions II Launcher.lnk')));
  assert.strictEqual(fs.readFileSync(path.join(deskDir, 'Twelve Weeks Launcher.lnk'), 'utf8'), 'LNK-DESK-NEW', 'an existing new link is kept');
  assert(!fs.existsSync(path.join(deskDir, 'Factions II Launcher.lnk')), 'duplicate old link dropped');
  assert.strictEqual(inst.renameLegacyShortcuts(envR), 0, 'idempotent');
  // an update (bootstrap on an existing install, shortcuts.ok present) renames the old links and writes no new ones
  const rootU = mk('rootU');
  const unU = mk('unpackU');
  w(path.join(unU, 'Twelve Weeks Launcher.exe'), 'EXE'); w(path.join(unU, 'resources', 'app.asar'), 'A');
  const envU = { APPDATA: mk('appdataU'), USERPROFILE: mk('profileU') };
  const [sU, dU] = inst.shortcutDirs(envU);
  const lU = [];
  const bootU = () => inst.bootstrap({ root: rootU, version: '0.1.25', execPath: path.join(unU, 'Twelve Weeks Launcher.exe'), resourcesPath: path.join(unU, 'resources'), env: envU, writeShortcut: (f, o) => lU.push({ f, o }), log: () => {}, tmpDir: mk('faketmp') });
  bootU();                                   // first install writes the new-named links
  lU.length = 0;
  fs.rmSync(path.join(sU, 'Twelve Weeks Launcher.lnk'), { force: true }); fs.rmSync(path.join(dU, 'Twelve Weeks Launcher.lnk'), { force: true });
  w(path.join(sU, 'Factions II Launcher.lnk'), 'OLD-S'); w(path.join(dU, 'Factions II Launcher.lnk'), 'OLD-D');
  inst.bootstrap({ root: rootU, version: '0.1.26', execPath: path.join(unU, 'Twelve Weeks Launcher.exe'), resourcesPath: path.join(unU, 'resources'), env: envU, writeShortcut: (f, o) => lU.push({ f, o }), log: () => {}, tmpDir: mk('faketmp') });
  assert(fs.existsSync(path.join(sU, 'Twelve Weeks Launcher.lnk')) && fs.existsSync(path.join(dU, 'Twelve Weeks Launcher.lnk')), 'update: old links renamed');
  assert(!fs.existsSync(path.join(sU, 'Factions II Launcher.lnk')) && !fs.existsSync(path.join(dU, 'Factions II Launcher.lnk')));
  assert.strictEqual(lU.length, 0, 'update writes no new shortcuts (AUMID and pin untouched)');
}
void stamp;
fs.rmSync(tmp, { recursive: true, force: true });
console.log('install-test ok');
