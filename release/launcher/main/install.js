/**
 * @file Stable-entry install layout (pin-safe self-update, Windows).
 *
 * Root (%LOCALAPPDATA%\Twelve Weeks Launcher = ROOT_NAME):
 *   Twelve Weeks Launcher.exe  the Electron runtime, written once, never replaced by an update
 *   resources\app\             a frozen shim (package.json + main.js) that loads the newest payload
 *   icon.ico, runtime.ok, portable-dir.txt
 *   app-<version>\app.asar     one folder per launcher version, `ok` marker written last
 * An update only ADDS app-<newver>; the exe path, the shortcuts and the taskbar pin never change.
 * The first run of a portable exe (old layout, or a freshly downloaded update) installs itself here and hands over.
 *
 * Product rename: installs made before it live in %LOCALAPPDATA%\Factions II Launcher (LEGACY_ROOT_NAME), run "Factions II Launcher.exe",
 * use the AppUserModelID org.factions2.launcher and keep their settings in %APPDATA%\Factions II Launcher (~/.config/... on Linux).
 * The first bootstrap of a new build moves that root here (migrateRoot), retargets the shortcuts and taskbar pins (migrateShortcuts),
 * and the first start moves the settings folder (migrateUserData). Every step is idempotent and falls back to the old place when a
 * file is held open, so an interrupted migration is finished by a later start.
 */
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');

const AUMID = 'org.twelveweeks.launcher';
// ROOT_NAME names the install folder, the runtime exe and the Electron userData folder. DISPLAY is what the player sees (shortcut names,
// taskbar relaunch name); both are the same since the rename.
const ROOT_NAME = 'Twelve Weeks Launcher';
const ROOT_EXE = ROOT_NAME + '.exe';
const DISPLAY = 'Twelve Weeks Launcher';
// the same identifiers before the product rename (read only by the migration)
const LEGACY_AUMID = 'org.factions2.launcher';
const LEGACY_ROOT_NAME = 'Factions II Launcher';
const LEGACY_ROOT_EXE = LEGACY_ROOT_NAME + '.exe';

/** @returns {string} the fixed install root */
function installRoot(env = process.env) {
  if (env.F2_INSTALL_ROOT) return env.F2_INSTALL_ROOT;
  const base = env.LOCALAPPDATA || path.join(os.homedir(), 'AppData', 'Local');
  return path.join(base, ROOT_NAME);
}

/** @returns {string} where installs made before the product rename sit */
function legacyInstallRoot(env = process.env) {
  if (env.F2_LEGACY_INSTALL_ROOT) return env.F2_LEGACY_INSTALL_ROOT;
  if (env.F2_INSTALL_ROOT) return null; // a test / dev root never migrates (or removes) the real install
  const base = env.LOCALAPPDATA || path.join(os.homedir(), 'AppData', 'Local');
  return path.join(base, LEGACY_ROOT_NAME);
}

const same = (a, b) => path.resolve(a).toLowerCase() === path.resolve(b).toLowerCase();

/** Is this process the installed runtime (started from the fixed root)? */
function isInstalledRun(execPath = process.execPath, root = installRoot()) {
  return same(path.dirname(execPath), root) && fs.existsSync(path.join(root, 'runtime.ok'));
}

/** Does this process come from a portable exe that has to install itself first? */
function wantsBootstrap(env = process.env, platform = process.platform, root = installRoot(env)) {
  if (platform !== 'win32' || !env.PORTABLE_EXECUTABLE_FILE || env.F2_NO_INSTALL) return false;
  return !same(path.dirname(process.execPath), root);
}

function cmpVersion(a, b) {
  const pa = String(a)
    .split(/[.\-+]/)
    .map((x) => parseInt(x, 10) || 0);
  const pb = String(b)
    .split(/[.\-+]/)
    .map((x) => parseInt(x, 10) || 0);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const d = (pa[i] || 0) - (pb[i] || 0);
    if (d) return d < 0 ? -1 : 1;
  }
  return 0;
}

/** Complete payload folders, newest first. */
function listPayloads(root) {
  let names = [];
  try {
    names = fs.readdirSync(root);
  } catch {
    return [];
  }
  return names
    .filter((n) => /^app-\d/.test(n) && fs.existsSync(path.join(root, n, 'ok')))
    .map((n) => ({ ver: n.slice(4), dir: path.join(root, n) }))
    .sort((a, b) => cmpVersion(b.ver, a.ver));
}

// Frozen shim: it can never be updated (the runtime exe + this file are written once), so it stays tiny and defensive.
const SHIM_MAIN = [
  "'use strict';",
  "const fs = require('fs');",
  "const path = require('path');",
  'const root = path.dirname(process.execPath);',
  'const num = (v) => String(v).split(/[.\\-+]/).map((x) => parseInt(x, 10) || 0);',
  'const cmp = (a, b) => { const x = num(a), y = num(b); for (let i = 0; i < Math.max(x.length, y.length); i++) { const d = (x[i] || 0) - (y[i] || 0); if (d) return d; } return 0; };',
  'let dirs = [];',
  "try { dirs = fs.readdirSync(root).filter((n) => /^app-\\d/.test(n) && fs.existsSync(path.join(root, n, 'ok'))); } catch (e) { /* no payloads */ }",
  'dirs.sort((a, b) => cmp(b.slice(4), a.slice(4)));',
  'let last = null;',
  'for (const d of dirs) {',
  "  try { require(path.join(root, d, 'app.asar', 'main.js')); last = null; break; }",
  "  catch (e) { last = e; try { fs.appendFileSync(path.join(root, 'shim-error.log'), new Date().toISOString() + ' ' + d + ' ' + ((e && e.stack) || e) + '\\n'); } catch (x) { /* ignore */ } }",
  '}',
  "if (last || !dirs.length) throw last || new Error('no launcher payload installed in ' + root);",
  '',
].join('\n');
const SHIM_PKG = JSON.stringify(
  { name: 'twelve-weeks-launcher', productName: ROOT_NAME, version: '0.0.0', main: 'main.js' },
  null,
  2,
);

function copyFileAtomic(src, dest, raw) {
  let f = fs;
  if (raw)
    try {
      f = require('original-fs'); // an .asar is copied as one file, never opened as an archive
    } catch {
      /* plain node */
    }
  f.mkdirSync(path.dirname(dest), { recursive: true });
  const tmp = dest + '.part';
  f.copyFileSync(src, tmp);
  f.renameSync(tmp, dest);
}

/** Copy the Electron runtime (everything except the app payload) into the root. Never touches an existing runtime. */
function installRuntime({ root, exeDir, exeName, srcExeName = exeName, iconSrc }) {
  if (fs.existsSync(path.join(root, 'runtime.ok')) && fs.existsSync(path.join(root, exeName)))
    return false;
  fs.mkdirSync(root, { recursive: true });
  // (Electron's asar-aware fs opened resourcespp.asar as a directory -> ENOTDIR, and the path filter missed it
  // when exeDir came as an 8.3 short path. original-fs = no asar magic; the skip compares the name inside resources\.)
  let ofs = fs;
  try {
    ofs = require('original-fs');
  } catch {
    /* plain node (tests) */
  }
  const isPayload = (src) =>
    path.basename(path.dirname(src)).toLowerCase() === 'resources' &&
    ['app.asar', 'app'].includes(path.basename(src).toLowerCase());
  ofs.cpSync(exeDir, root, {
    recursive: true,
    force: true,
    filter: (src) => !isPayload(src),
  });

  if (srcExeName !== exeName) {
    fs.rmSync(path.join(root, exeName), { force: true });
    fs.renameSync(path.join(root, srcExeName), path.join(root, exeName));
  }

  const shimDir = path.join(root, 'resources', 'app');
  fs.mkdirSync(shimDir, { recursive: true });
  fs.writeFileSync(path.join(shimDir, 'package.json'), SHIM_PKG);
  fs.writeFileSync(path.join(shimDir, 'main.js'), SHIM_MAIN);
  if (iconSrc && fs.existsSync(iconSrc)) copyFileAtomic(iconSrc, path.join(root, 'icon.ico'));
  fs.writeFileSync(path.join(root, 'runtime.ok'), new Date().toISOString());
  return true;
}

/** Add app-<version>\app.asar (marker last). Returns false when that version or a newer one is there already. */
function installPayload({ root, version, asar }) {
  const have = listPayloads(root);
  if (have.some((p) => cmpVersion(p.ver, version) >= 0)) return false;
  const dir = path.join(root, 'app-' + version);
  fs.rmSync(dir, { recursive: true, force: true });
  copyFileAtomic(asar, path.join(dir, 'app.asar'), true);
  fs.writeFileSync(path.join(dir, 'ok'), new Date().toISOString());
  return true;
}

/** Remember where the old portable exe sat: its client\ folder (shipped-zip layout) still belongs to the player. */
function rememberPortableDir({ root, dir, tmpDir = os.tmpdir() }) {
  if (!dir) return false;
  const d = path.resolve(dir).toLowerCase();
  if (d.startsWith(path.resolve(tmpDir).toLowerCase() + path.sep)) return false;
  if (!fs.existsSync(path.join(dir, 'client', 'winmm.dll'))) return false;
  fs.writeFileSync(path.join(root, 'portable-dir.txt'), dir);
  return true;
}

/** Directory that holds the shipped client\ folder when we run from the installed root. */
function rememberedPortableDir(root) {
  try {
    const d = fs.readFileSync(path.join(root, 'portable-dir.txt'), 'utf8').trim();
    return d && fs.existsSync(d) ? d : null;
  } catch {
    return null;
  }
}

/** Keep the newest `keep` payloads; drop the rest and the leftover update download. Best effort. */
function prune(root, keep = 2) {
  try {
    listPayloads(root)
      .slice(keep)
      .forEach((p) => fs.rmSync(p.dir, { recursive: true, force: true }));
    for (const n of fs.readdirSync(root)) {
      // payload folders that never got their `ok` marker (interrupted copy)
      if (/^app-\d/.test(n) && !fs.existsSync(path.join(root, n, 'ok')))
        fs.rmSync(path.join(root, n), { recursive: true, force: true });
    }
  } catch {
    /* ignore */
  }
  try {
    fs.rmSync(path.join(os.tmpdir(), 'twelve-weeks-launcher-update'), {
      recursive: true,
      force: true,
    });
  } catch {
    /* in use by a still-running installer: next start */
  }
}

/** Environment for a child that must not think it is a portable exe. */
function cleanEnv(env = process.env) {
  const e = { ...env };
  for (const k of Object.keys(e)) if (/^PORTABLE_EXECUTABLE_/.test(k)) delete e[k];
  return e;
}

/** Shortcut options shared by the Start menu / Desktop links (AppUserModelID = the one the app sets). */
function shortcutOptions(root, exeName) {
  return {
    target: path.join(root, exeName),
    cwd: root,
    description: DISPLAY,
    icon: path.join(root, 'icon.ico'),
    iconIndex: 0,
    appUserModelId: AUMID,
  };
}

/** Start menu / Desktop folders that hold the launcher's shortcuts. */
function shortcutDirs(env = process.env) {
  const appData = env.APPDATA || path.join(os.homedir(), 'AppData', 'Roaming');
  return [
    path.join(appData, 'Microsoft', 'Windows', 'Start Menu', 'Programs'),
    path.join(env.USERPROFILE || os.homedir(), 'Desktop'),
  ];
}

/**
 * The product was renamed: the old-name link in the Start menu and on the Desktop becomes "Twelve Weeks Launcher.lnk" (migrateShortcuts then
 * points it at the new root). The taskbar pin is a separate copy and keeps its file name.
 * When the new name exists already the old link is dropped. Never throws. Returns the number of links renamed or removed.
 */
function renameLegacyShortcuts(env = process.env, log = () => {}) {
  let n = 0;
  for (const dir of shortcutDirs(env)) {
    const oldLnk = path.join(dir, LEGACY_ROOT_NAME + '.lnk');
    const newLnk = path.join(dir, DISPLAY + '.lnk');
    try {
      if (!fs.existsSync(oldLnk)) continue;
      if (fs.existsSync(newLnk)) fs.rmSync(oldLnk, { force: true });
      else fs.renameSync(oldLnk, newLnk);
      n++;
    } catch (e) {
      log('install: shortcut rename failed: ' + oldLnk + ': ' + (e && e.message));
    }
  }
  return n;
}

const sleepSync = (ms) => {
  if (ms > 0) Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
};

/** Rename that waits out a process still quitting or a virus scanner (EBUSY, EPERM, EACCES). Throws the last error. */
function renameRetry(
  from,
  to,
  { tries = 30, waitMs = 500, sleep = sleepSync, rename = fs.renameSync } = {},
) {
  let last = null;
  for (let i = 0; i < tries; i++) {
    if (i) sleep(waitMs);
    try {
      rename(from, to);
      return;
    } catch (e) {
      last = e;
      if (!/^(EBUSY|EPERM|EACCES)$/.test(e.code || '')) break;
    }
  }
  throw last;
}

const isEmptyDir = (d) => {
  try {
    return fs.readdirSync(d).length === 0;
  } catch {
    return false;
  }
};

/** Does `p` start with the folder `dir` (case-insensitive on Windows)? */
function under(p, dir) {
  if (typeof p !== 'string' || !dir || p.length < dir.length) return false;
  const head = p.slice(0, dir.length);
  const hit =
    process.platform === 'win32' ? head.toLowerCase() === dir.toLowerCase() : head === dir;
  return hit && (p.length === dir.length || p[dir.length] === '\\' || p[dir.length] === '/');
}

/**
 * Move an install made before the product rename (legacyRoot) to the new root. Runs in bootstrap before the runtime install.
 * The old launcher that started this update may still be quitting: its running exe blocks the folder rename, so it is retried for a while.
 * Moved: the runtime exe gets the new name and the shim's package.json the new names (the shim code itself is the same).
 * Not movable (held open, or the new root holds files already): the new root is installed fresh; portable-dir.txt is copied over and the
 * old root is removed later (removeLegacyRoot). Never throws.
 * @returns {{legacy: boolean, moved: boolean}} legacy = an old install was found
 */
function migrateRoot({ root, legacyRoot, log = () => {}, retry = {} }) {
  const res = { legacy: false, moved: false };
  try {
    if (
      !legacyRoot ||
      same(root, legacyRoot) ||
      !fs.existsSync(path.join(legacyRoot, 'runtime.ok'))
    )
      return res;
    res.legacy = true;
    if (fs.existsSync(path.join(root, 'runtime.ok'))) return res; // migrated already (the old root is only a leftover)
    if (fs.existsSync(root) && isEmptyDir(root)) fs.rmdirSync(root);
    if (!fs.existsSync(root)) {
      try {
        fs.mkdirSync(path.dirname(root), { recursive: true });
        renameRetry(legacyRoot, root, retry);
        res.moved = true;
      } catch (e) {
        log(
          'install: could not move the old install root (' + (e && e.code) + '), installing fresh',
        );
      }
    }
    if (res.moved) {
      const oldExe = path.join(root, LEGACY_ROOT_EXE);
      if (fs.existsSync(oldExe) && !fs.existsSync(path.join(root, ROOT_EXE)))
        fs.renameSync(oldExe, path.join(root, ROOT_EXE));
      const shimDir = path.join(root, 'resources', 'app');
      if (fs.existsSync(shimDir)) {
        fs.writeFileSync(path.join(shimDir, 'package.json'), SHIM_PKG);
        fs.writeFileSync(path.join(shimDir, 'main.js'), SHIM_MAIN);
      }
      fs.rmSync(path.join(root, 'shortcuts.ok'), { force: true }); // the links still point into the old root: write them again
      log('install: moved the old install root ' + legacyRoot + ' to ' + root);
    } else {
      fs.mkdirSync(root, { recursive: true });
      const pd = path.join(legacyRoot, 'portable-dir.txt');
      if (fs.existsSync(pd) && !fs.existsSync(path.join(root, 'portable-dir.txt')))
        fs.copyFileSync(pd, path.join(root, 'portable-dir.txt'));
    }
  } catch (e) {
    log('install: old install root migration failed: ' + (e && e.message));
  }
  return res;
}

/**
 * Remove what is left of the old install root once the new root is installed (a migration that could not move it). Best effort: a running
 * old runtime blocks the first rename, so nothing is half deleted. Returns true when it is gone.
 */
function removeLegacyRoot({ root, legacyRoot, log = () => {} }) {
  try {
    if (!legacyRoot || same(root, legacyRoot)) return false;
    if (!fs.existsSync(path.join(root, 'runtime.ok'))) return false;
    const trash = legacyRoot + '.removing';
    if (fs.existsSync(legacyRoot)) {
      if (fs.existsSync(trash)) fs.rmSync(trash, { recursive: true, force: true });
      fs.renameSync(legacyRoot, trash);
    }
    if (!fs.existsSync(trash)) return false;
    fs.rmSync(trash, { recursive: true, force: true });
    log('install: removed the old install root ' + legacyRoot);
    return true;
  } catch (e) {
    log('install: old install root still in use (' + (e && e.code) + ')');
    return false;
  }
}

/** Folders that hold the user's taskbar / Start pins (copies of a shortcut, made by Windows). */
function pinDirs(env = process.env) {
  const appData = env.APPDATA || path.join(os.homedir(), 'AppData', 'Roaming');
  const pinned = path.join(
    appData,
    'Microsoft',
    'Internet Explorer',
    'Quick Launch',
    'User Pinned',
  );
  return [path.join(pinned, 'TaskBar'), path.join(pinned, 'StartMenu')];
}

/**
 * Point every launcher shortcut that still targets the old install root (or carries the old AppUserModelID) at the new runtime with
 * the new AppUserModelID: the Start menu / Desktop links (old file name renamed first) and the taskbar / Start pins. A pin keeps its
 * file name (Explorer finds it by that), only its content is rewritten. Idempotent, never throws.
 * deps: { env, root, legacyRoot, readShortcut(file) -> {target, appUserModelId} | null, writeShortcut(file, opts), log }.
 * Returns the number of links rewritten.
 */
function migrateShortcuts({
  env = process.env,
  root,
  legacyRoot,
  readShortcut,
  writeShortcut,
  log = () => {},
}) {
  renameLegacyShortcuts(env, log);
  if (!readShortcut || !writeShortcut) return 0;
  const opts = shortcutOptions(root, ROOT_EXE);
  const files = [];
  for (const dir of shortcutDirs(env)) files.push(path.join(dir, DISPLAY + '.lnk'));
  for (const dir of pinDirs(env)) {
    try {
      for (const f of fs.readdirSync(dir)) if (/\.lnk$/i.test(f)) files.push(path.join(dir, f));
    } catch {
      /* no pins */
    }
  }
  let n = 0;
  for (const file of files) {
    try {
      if (!fs.existsSync(file)) continue;
      const cur = readShortcut(file);
      if (!cur) continue;
      const target = cur.target || '';
      const old =
        cur.appUserModelId === LEGACY_AUMID ||
        (legacyRoot && under(target, legacyRoot)) ||
        path.basename(target).toLowerCase() === LEGACY_ROOT_EXE.toLowerCase();
      if (!old) continue;
      writeShortcut(file, opts);
      n++;
    } catch (e) {
      log('install: shortcut retarget failed: ' + file + ': ' + (e && e.message));
    }
  }
  if (n) log('install: ' + n + ' shortcut(s) / pin(s) now start ' + path.join(root, ROOT_EXE));
  return n;
}

/**
 * Settings folder (Electron userData) to use, after moving the one from before the product rename (<appData>/Factions II Launcher:
 * %APPDATA% on Windows, ~/.config on Linux) to <appData>/Twelve Weeks Launcher. Synchronous: runs before app ready.
 * An old launcher still quitting holds files in it, so the rename is retried for a while; when it still fails the old folder is used for
 * this run and the next start tries again. A new folder without launcher.json (made by Electron or a failed start) is set aside first.
 * Absolute paths in launcher.json that pointed into the old folder (a client folder picked there) are rewritten.
 * @returns {{dir: string, moved: boolean}}
 */
function migrateUserData({ appData, log = () => {}, retry = {} }) {
  const from = path.join(appData, LEGACY_ROOT_NAME),
    to = path.join(appData, ROOT_NAME);
  try {
    if (!fs.existsSync(from)) return { dir: to, moved: false };
    if (fs.existsSync(path.join(to, 'launcher.json'))) return { dir: to, moved: false }; // done before; the old folder is a leftover
    if (fs.existsSync(to)) {
      if (isEmptyDir(to)) fs.rmdirSync(to);
      else fs.renameSync(to, to + '.pre-migration-' + Date.now());
    }
    renameRetry(from, to, retry);
    const cfg = path.join(to, 'launcher.json');
    try {
      const v = JSON.parse(fs.readFileSync(cfg, 'utf8'));
      let changed = false;
      for (const k of Object.keys(v))
        if (under(v[k], from)) {
          v[k] = to + v[k].slice(from.length);
          changed = true;
        }
      if (changed) fs.writeFileSync(cfg, JSON.stringify(v, null, 2));
    } catch {
      /* no settings yet */
    }
    log('Settings folder moved from ' + from + ' to ' + to);
    return { dir: to, moved: true };
  } catch (e) {
    log(
      'Settings folder migration failed (' +
        ((e && e.code) || (e && e.message)) +
        '), using ' +
        from,
    );
    return { dir: from, moved: false };
  }
}

/**
 * Full self-install from a running portable exe.
 * deps: { root, version, execPath, resourcesPath, env, writeShortcut(path, opts), log }.
 * Returns the installed exe path to launch.
 */
function bootstrap({
  root,
  legacyRoot = null,
  version,
  execPath,
  resourcesPath,
  env,
  writeShortcut,
  readShortcut = null,
  log,
  tmpDir,
  retry,
}) {
  const exeDir = path.dirname(execPath);
  const exeName = path.basename(execPath);
  const asar = path.join(resourcesPath, 'app.asar');
  if (!fs.existsSync(asar)) throw new Error('no app.asar next to the portable runtime');
  const iconSrc = path.join(asar, 'assets', 'icon.ico');
  const migrated = migrateRoot({ root, legacyRoot, log, retry });
  // the portable build unpacks its exe as <productName>.exe; the installed runtime is ROOT_EXE
  const newRuntime = installRuntime({
    root,
    exeDir,
    exeName: ROOT_EXE,
    srcExeName: exeName,
    iconSrc,
  });
  const newPayload = installPayload({ root, version, asar });
  rememberPortableDir({ root, dir: env.PORTABLE_EXECUTABLE_DIR, tmpDir });
  const marker = path.join(root, 'shortcuts.ok');
  migrateShortcuts({ env, root, legacyRoot, readShortcut, writeShortcut, log });
  if (newRuntime || migrated.legacy || !fs.existsSync(marker)) {
    try {
      const opts = shortcutOptions(root, ROOT_EXE);
      const appData = env.APPDATA || path.join(os.homedir(), 'AppData', 'Roaming');
      const start = path.join(
        appData,
        'Microsoft',
        'Windows',
        'Start Menu',
        'Programs',
        DISPLAY + '.lnk',
      );
      writeShortcut(start, opts);
      const desk = path.join(env.USERPROFILE || os.homedir(), 'Desktop', DISPLAY + '.lnk');
      // a Desktop link the player deleted stays deleted when an old install is migrated
      if (fs.existsSync(desk) || !migrated.legacy) writeShortcut(desk, opts);
      fs.writeFileSync(marker, '1');
    } catch (e) {
      log('install: shortcuts failed: ' + (e && e.message));
    }
  }
  log(
    `install: root ${root}, runtime ${newRuntime ? 'written' : 'kept'}, payload ${version} ${newPayload ? 'added' : 'kept'}`,
  );
  return path.join(root, ROOT_EXE);
}

/** Start the installed runtime detached. */
function launchInstalled(exe, args = []) {
  spawn(exe, args, {
    detached: true,
    stdio: 'ignore',
    env: cleanEnv(),
    cwd: path.dirname(exe),
  }).unref();
}

/**
 * icon.ico in the root is copied only when the runtime is first
 * installed, and an update brings only app-<version>pp.asar, so an existing install kept the old icon forever. On every installed start the payload's
 * assets/icon.ico is compared with the root copy; a different one replaces it, is also written as icon-<hash>.ico (a new path, so Explorer's icon cache
 * can't serve the old picture) and the Start menu / Desktop links that exist are rewritten to it. The taskbar pin is the user's copy and is not touched.
 * deps: { root, iconSrc, writeShortcut(path, opts), log, refreshShell() }. Never throws. Returns true when the icon changed.
 */
function refreshIcon({ root, iconSrc, writeShortcut, log = () => {}, refreshShell = () => {} }) {
  try {
    if (!root || !iconSrc || !fs.existsSync(iconSrc)) return false;
    const want = fs.readFileSync(iconSrc);
    const cur = path.join(root, 'icon.ico');
    if (fs.existsSync(cur) && Buffer.compare(fs.readFileSync(cur), want) === 0) return false;
    const tag = require('crypto').createHash('sha256').update(want).digest('hex').slice(0, 8);
    const fresh = path.join(root, 'icon-' + tag + '.ico');
    for (const dest of [cur, fresh]) {
      fs.mkdirSync(path.dirname(dest), { recursive: true });
      fs.writeFileSync(dest + '.part', want);
      fs.renameSync(dest + '.part', dest);
    }
    let n = 0;
    if (writeShortcut)
      for (const dir of shortcutDirs()) {
        const lnk = path.join(dir, DISPLAY + '.lnk');
        if (!fs.existsSync(lnk)) continue;
        try {
          writeShortcut(lnk, { ...shortcutOptions(root, ROOT_EXE), icon: fresh });
          n++;
        } catch (e) {
          log('install: icon shortcut rewrite failed: ' + lnk + ': ' + (e && e.message));
        }
      }
    for (const f of fs.readdirSync(root))
      if (/^icon-[0-9a-f]{8}\.ico$/.test(f) && path.join(root, f) !== fresh)
        fs.rmSync(path.join(root, f), { force: true });
    refreshShell();
    log('install: launcher icon updated (' + tag + '), ' + n + ' shortcut(s) rewritten');
    return true;
  } catch (e) {
    log('install: icon refresh failed: ' + (e && e.message));
    return false;
  }
}

module.exports = {
  refreshIcon,
  AUMID,
  DISPLAY,
  ROOT_NAME,
  ROOT_EXE,
  LEGACY_AUMID,
  LEGACY_ROOT_NAME,
  LEGACY_ROOT_EXE,
  legacyInstallRoot,
  migrateRoot,
  removeLegacyRoot,
  migrateShortcuts,
  migrateUserData,
  pinDirs,
  renameLegacyShortcuts,
  shortcutDirs,
  installRoot,
  isInstalledRun,
  wantsBootstrap,
  cmpVersion,
  listPayloads,
  installRuntime,
  installPayload,
  rememberPortableDir,
  rememberedPortableDir,
  prune,
  cleanEnv,
  shortcutOptions,
  bootstrap,
  launchInstalled,
  SHIM_MAIN,
};
