/**
 * @file Stable-entry install layout (pin-safe self-update, Windows).
 *
 * Root (%LOCALAPPDATA%\Factions II Launcher = ROOT_NAME, never changes; the product is called Twelve Weeks Launcher):
 *   Factions II Launcher.exe   the Electron runtime, written once, never replaced by an update
 *   resources\app\             a frozen shim (package.json + main.js) that loads the newest payload
 *   icon.ico, runtime.ok, portable-dir.txt
 *   app-<version>\app.asar     one folder per launcher version, `ok` marker written last
 * An update only ADDS app-<newver>; the exe path, the shortcuts and the taskbar pin never change.
 * The first run of a portable exe (old layout, or a freshly downloaded update) installs itself here and hands over.
 */
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');

const AUMID = 'org.factions2.launcher';
// ROOT_NAME is an identifier, not a display name: the install folder, the runtime exe and the Electron userData folder of every existing
// install carry it, so it never changes. DISPLAY is what the player sees (shortcut names, taskbar relaunch name).
const ROOT_NAME = 'Factions II Launcher';
const ROOT_EXE = ROOT_NAME + '.exe';
const DISPLAY = 'Twelve Weeks Launcher';

/** @returns {string} the fixed install root */
function installRoot(env = process.env) {
  if (env.F2_INSTALL_ROOT) return env.F2_INSTALL_ROOT;
  const base = env.LOCALAPPDATA || path.join(os.homedir(), 'AppData', 'Local');
  return path.join(base, ROOT_NAME);
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
  { name: 'factions2-launcher', productName: ROOT_NAME, version: '0.0.0', main: 'main.js' },
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
 * The product was renamed (Twelve Weeks): "Factions II Launcher.lnk" in the Start menu and on the Desktop becomes "Twelve Weeks Launcher.lnk".
 * A rename keeps target, icon and AppUserModelID, so nothing else changes; the taskbar pin is a separate copy and is never touched.
 * When the new name exists already the old link is dropped. Never throws. Returns the number of links renamed or removed.
 */
function renameLegacyShortcuts(env = process.env, log = () => {}) {
  let n = 0;
  for (const dir of shortcutDirs(env)) {
    const oldLnk = path.join(dir, ROOT_NAME + '.lnk');
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

/**
 * Full self-install from a running portable exe.
 * deps: { root, version, execPath, resourcesPath, env, writeShortcut(path, opts), log }.
 * Returns the installed exe path to launch.
 */
function bootstrap({ root, version, execPath, resourcesPath, env, writeShortcut, log, tmpDir }) {
  const exeDir = path.dirname(execPath);
  const exeName = path.basename(execPath);
  const asar = path.join(resourcesPath, 'app.asar');
  if (!fs.existsSync(asar)) throw new Error('no app.asar next to the portable runtime');
  const iconSrc = path.join(asar, 'assets', 'icon.ico');
  // the portable build unpacks its exe as <productName>.exe (now "Twelve Weeks Launcher.exe"); the installed runtime keeps its old name
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
  renameLegacyShortcuts(env, log);
  if (newRuntime || !fs.existsSync(marker)) {
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
      if (!fs.existsSync(desk)) writeShortcut(desk, opts);
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
