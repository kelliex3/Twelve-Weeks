/**
 * @file Play-time check of the game folder: moves files that are not part of a clean install + Twelve Weeks out of the way.
 *
 * Why: the mod's winmm.dll checks every archive the game mounts from build/pc/main. An archive it does not
 * know, or a known one of another size, marks the install as modified; winmm then logs in with a substitute token and the
 * server refuses every search ("never finds a match"). Other mods' loaders (dxgi.dll, dinput8.dll, *.asi ...) also load
 * into the game next to the mod.
 *
 * On every Play (game closed):
 * - build/pc/main: every .psarc that is not a known game archive moves to <game>/twelve-weeks-moved/build/pc/main/...
 * - game folder: every *.asi, and every .dll with a DLL-proxy name (the names other loaders use), moves to
 *   <game>/twelve-weeks-moved/... (never winmm.dll, t2r-*.dll, a .dll of the clean install, or OptiScaler,
 *   the upscaler stays under any of its proxy names; a loader that only mentions OptiScaler moves).
 * - A known archive of another size cannot be fixed by moving: Play stops and asks for a Steam file check.
 * Nothing is deleted. Files keep their relative path below the moved folder, so the player can put them back.
 */
'use strict';

const fs = require('fs');
const path = require('path');
const { ARCHIVES, ROOT_DLLS } = require('./clean-list');

const MOVED_DIR = 'twelve-weeks-moved';
const DATA_DIR = path.join('build', 'pc', 'main');
// Names Windows loads from the program folder before System32: what ASI loaders, ReShade, OptiScaler, DXVK and similar use.
// winmm.dll is ours (handled by the install); version.dll can be the SP sandbox's own DLL (checked below).
const PROXY_DLLS = new Set([
  'd3d9.dll',
  'd3d10.dll',
  'd3d11.dll',
  'd3d12.dll',
  'dxgi.dll',
  'ddraw.dll',
  'dinput.dll',
  'dinput8.dll',
  'dsound.dll',
  'version.dll',
  'winhttp.dll',
  'wininet.dll',
  'xinput1_3.dll',
  'xinput1_4.dll',
  'xinput9_1_0.dll',
  'xinput1_2.dll',
  'xinput1_1.dll',
  'opengl32.dll',
  'dbghelp.dll',
  'msimg32.dll',
  'iphlpapi.dll',
  'hid.dll',
  'bcrypt.dll',
  'nvngx.dll',
  'vulkan-1.dll',
  'msacm32.dll',
  'avrt.dll',
  'uxtheme.dll',
  'userenv.dll',
  'wtsapi32.dll',
  'setupapi.dll',
  'mswsock.dll',
  'dwmapi.dll',
  'powrprof.dll',
]);

const MASK = (1n << 64n) - 1n;

/**
 * FNV-1a 64 of a path, as the mod's winmm keys its archive list.
 * @param {string} rel lowercase, forward slashes, relative to build/pc/main
 * @returns {string} 16 hex digits
 */
function pathHash(rel) {
  let h = 0xcbf29ce484222325n;
  for (const b of Buffer.from(rel, 'utf8')) h = ((h ^ BigInt(b)) * 0x100000001b3n) & MASK;
  return h.toString(16).padStart(16, '0');
}

/**
 * List the files below a folder (relative paths with forward slashes). Unreadable folders are skipped.
 * @param {string} dir
 * @param {string} [rel]
 * @param {string[]} [out]
 * @returns {string[]}
 */
function walk(dir, rel = '', out = []) {
  let ents;
  try {
    ents = fs.readdirSync(path.join(dir, rel), { withFileTypes: true });
  } catch {
    return out;
  }
  for (const e of ents) {
    const r = rel ? `${rel}/${e.name}` : e.name;
    if (e.isDirectory()) walk(dir, r, out);
    else if (e.isFile()) out.push(r);
  }
  return out;
}

/**
 * True when a version.dll is the SP sandbox's own DLL (spmode parks it; it is ours).
 * @param {string} file
 * @returns {boolean}
 */
function isSandboxDll(file) {
  try {
    const b = fs.readFileSync(file);
    return b.includes('t2r-sp.txt') && b.includes('t2r-sp.prev.log');
  } catch {
    return false;
  }
}

// Other loaders that mention OptiScaler (they chain to it or name it in their messages) but are not OptiScaler. SCSKiller, a
// D3D12 shader recorder, installs itself as d3d12.dll (and puts itself back after it is removed) and hooks the game's D3D12.
const NOT_OPTISCALER = ['SCSKiller'];

/**
 * True when a DLL / ASI is OptiScaler (kept: graphics only). Its binary names itself under its proxy name ("OptiScaler working
 * as dxgi.dll"); the bare word is not enough, other proxies carry it too.
 * @param {string} file
 * @returns {boolean}
 */
function isOptiScaler(file) {
  try {
    const b = fs.readFileSync(file);
    return b.includes('OptiScaler working as') && !NOT_OPTISCALER.some((m) => b.includes(m));
  } catch {
    return false;
  }
}

/**
 * Find what does not belong in the game folder.
 * @param {string} gameDir
 * @returns {{move: string[], badSize: {rel: string, size: number, want: number}[]}} paths relative to gameDir, forward slashes
 */
function scan(gameDir) {
  const move = [];
  const badSize = [];
  const data = path.join(gameDir, DATA_DIR);
  for (const rel of walk(data)) {
    if (!/\.psarc$/i.test(rel)) continue;
    const want = ARCHIVES.get(pathHash(rel.toLowerCase()));
    const full = `build/pc/main/${rel}`;
    if (want === undefined) {
      move.push(full);
      continue;
    }
    let size = -1;
    try {
      size = fs.statSync(path.join(data, rel)).size;
    } catch {}
    if (size !== want) badSize.push({ rel: full, size, want });
  }
  let names = [];
  try {
    names = fs.readdirSync(gameDir, { withFileTypes: true });
  } catch {}
  for (const e of names) {
    if (!e.isFile()) continue;
    const n = e.name.toLowerCase();
    const full = path.join(gameDir, e.name);
    const loader =
      n.endsWith('.asi') || (n.endsWith('.dll') && PROXY_DLLS.has(n) && !ROOT_DLLS.includes(n));
    if (!loader) continue;
    if (n === 'version.dll' && isSandboxDll(full)) continue;
    if (isOptiScaler(full)) continue;
    move.push(e.name);
  }
  return { move, badSize };
}

/**
 * A free destination below the moved folder (an older moved copy of the same name is kept: name.1, name.2 ...).
 * @param {string} gameDir
 * @param {string} rel
 * @returns {string}
 */
function destFor(gameDir, rel) {
  const base = path.join(gameDir, MOVED_DIR, ...rel.split('/'));
  if (!fs.existsSync(base)) return base;
  for (let i = 1; ; i++) if (!fs.existsSync(`${base}.${i}`)) return `${base}.${i}`;
}

/**
 * Move the foreign files out of the game folder. Throws when Play cannot work (a file could not be moved, or a game
 * archive has another size), unless opts.skipCheck.
 * @param {string} gameDir
 * @param {Function} log
 * @param {object} [opts] { skipCheck }
 * @returns {{moved: string[], badSize: object[]}}
 */
function quarantineForeignFiles(gameDir, log, opts = {}) {
  const { move, badSize } = scan(gameDir);
  const moved = [];
  const failed = [];
  for (const rel of move) {
    const from = path.join(gameDir, ...rel.split('/'));
    const to = destFor(gameDir, rel);
    try {
      fs.mkdirSync(path.dirname(to), { recursive: true });
      fs.renameSync(from, to); // same folder tree = same drive: a rename, never a copy; never deletes
      moved.push(rel);
      log(`Moved ${rel} to ${path.relative(gameDir, to)} (not part of the game or Twelve Weeks).`);
    } catch (e) {
      failed.push(rel);
      log(`Could not move ${rel} (${e.code || e.message}).`);
    }
  }
  if (moved.length)
    log(
      `${moved.length} file(s) from other mods moved to ${path.join(gameDir, MOVED_DIR)}. Twelve Weeks does not run with other mods installed.`,
    );
  for (const b of badSize)
    log(
      `Game file ${b.rel} differs from a clean install (${b.size < 0 ? 'unreadable' : b.size + ' B'}, clean ${b.want} B).`,
    );
  if (opts.skipCheck) return { moved, badSize };
  if (failed.length)
    throw new Error(
      `Could not move ${failed.join(', ')} out of the game folder (in use or no permission). Close other programs that use the game folder, then press Play again. Twelve Weeks cannot find matches while files from other mods are in the game folder.`,
    );
  if (badSize.length)
    throw new Error(
      `These game files differ from a clean install: ${shortList(badSize.map((b) => b.rel))}. With changed game files the server refuses every match search. In Steam: right-click The Last of Us Part II > Properties > Installed Files > Verify integrity of game files, then press Play again.`,
    );
  return { moved, badSize };
}

/**
 * File list for the error text: up to 3 names as they are, more = one entry per folder with its count ("build/pc/main/tts1/ (36)").
 * A player who emptied the language files got all 40 names in one message; the full list stays in the log.
 * @param {string[]} rels
 * @returns {string}
 */
function shortList(rels) {
  if (rels.length <= 3) return rels.join(', ');
  const dirs = new Map();
  for (const r of rels) {
    const d = r.includes('/') ? r.slice(0, r.lastIndexOf('/') + 1) : './';
    dirs.set(d, (dirs.get(d) || 0) + 1);
  }
  return [...dirs].map(([d, n]) => `${d} (${n})`).join(', ');
}

module.exports = { quarantineForeignFiles, scan, pathHash, shortList, MOVED_DIR, PROXY_DLLS };
