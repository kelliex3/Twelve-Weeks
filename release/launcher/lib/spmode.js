/**
 * @file Single-player/multiplayer file switch with a crash-recovery journal.
 */
'use strict';
// SWITCH TO SP / SWITCH TO MP: the mod's files are moved out of the game folder into <game>\TwelveWeeks-disabled\ (same relative paths) so the
// plain, unmodded game starts from Steam, and moved back later. Nothing is copied or deleted: every step is a rename inside one folder tree
// (same volume, so atomic and instant), journaled, rolled back on any failure and finished / undone by recover() after a crash.
//
// Mod-owned entries in the game folder (what lib/game.js installMod + the game itself write there):
//   winmm.dll (a T2R build, checked by its four server-address sites), t2r-ext.dll, tlou-ii-mp\ (data + slim extras), tlou-ii-mp.json (name token),
//   and every root entry named t2r-* (version, flags, name / loadout files, logs, t2r-steam\ Linux fallback, markers ...), plus half-copied
//   winmm.dll.tmp / .t2r-new. winmm.dll.t2r-original is a FOREIGN winmm.dll that installMod set aside: in SP it goes back to winmm.dll (the
//   game folder is exactly as before the mod), and the mod's dll waits in the disabled folder.
// Never touched: tlou-ii.exe, build\, anything else of the game, Steam files, save games (they live in Documents / Steam cloud, not here).
const fs = require('fs');
const path = require('path');

const DIR_NAME = 'TwelveWeeks-disabled';
// Names of the two parking folders before the product rename. An install that sits in SP mode keeps its mod files there until the first
// journal read after the update moves the folder (migrateLegacy). Until that rename works the old folder is used as it is.
const LEGACY_DIR_NAME = 'Factions2-disabled';
const LEGACY_TOOLS_DIR = 'Factions2-sp-tools';
const JOURNAL = '.f2-sp-journal.json'; // inside the disabled folder
const STEAM_APP_ID = '2531310';
const BACKOFF = [0, 200, 500, 1000, 2000];
const hooks = {
  rename: (a, b) => fs.renameSync(a, b),
  sleep: (ms) => {
    if (ms > 0) Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
  },
};

const TOOLS_DIR = 'TwelveWeeks-sp-tools'; // our SP sandbox debug tools (tools/sp-sandbox) wait here while the mod (MP) is active
/**
 * Check whether a filename is an SP debug tool.
 * @param {string} name
 * @returns {boolean}
 */
const spToolName = (name) => /^t2r-sp([.-]|$)/i.test(name); // t2r-sp.txt, t2r-sp.log, t2r-sp.prev.log, t2r-sp-force.txt, t2r-sp-sidbase.bin ...
/**
 * Check whether a filename is a generated SP/MP backup.
 * @param {string} name
 * @returns {boolean}
 */
const isBackupName = (name) => /\.\d{10,}(\.\d+)?$/.test(name); // name.<timestamp>: an older copy kept by enable()
/**
 * Return the owned runtime root for a path.
 * @param {string} name
 * @returns {string}
 */
const ownedRoot = (name) =>
  !spToolName(name) &&
  (/^t2r-/i.test(name) ||
    /^tlou-ii-mp(\.json)?$/i.test(name) ||
    /^winmm\.dll\.(tmp|t2r-new)$/i.test(name));
// The base MP mod the install builds on: modloader.asi + mods\ + tlou-ii-mp.psarc (+ its own small winmm.dll, which installMod keeps as
// winmm.dll.t2r-original). Left in the game folder it loads the MP psarc into the single-player game (boat menu / crash at start), so SP parks it too.
/**
 * Return the mod-loader root for a path.
 * @param {string} name
 * @returns {string}
 */
const baseModRoot = (name) =>
  /^modloader(\.[a-z0-9]+)?$/i.test(name) ||
  /^tlou-ii-mp\.psarc(\..+)?$/i.test(name) ||
  /^tlou-ii-mp\.log(\..+)?$/i.test(name); // (tlou-ii-mp.log = the modloader's log, a stale one next to a plain SP game reads as if a mod still ran)
// A version.dll / winmm.dll is ours only when it carries the sandbox's own file names (t2r-sp.c); a foreign one is never moved.
/**
 * Check whether a DLL is the sandbox/helper DLL.
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
/**
 * Return the parking directory for SP tools.
 * @param {string} game
 * @returns {string}
 */
const toolsDir = (game) => parkDir(game, TOOLS_DIR, LEGACY_TOOLS_DIR);
/**
 * Check whether a game-folder entry is one of the parking folders (current or legacy name).
 * @param {string} name
 * @returns {boolean}
 */
const isParkDir = (name) =>
  [DIR_NAME, TOOLS_DIR, LEGACY_DIR_NAME, LEGACY_TOOLS_DIR].some(
    (d) => d.toLowerCase() === String(name).toLowerCase(),
  );
// SP tool entries present in the game folder root. winmm.dll counts only when it is the sandbox (not a T2R build).
/**
 * List SP tools currently present in the game folder.
 * @param {string} game
 * @param {Function} verifyWinmm
 * @returns {string[]}
 */
function spToolsInGame(game, verifyWinmm) {
  let names = [];
  try {
    names = fs.readdirSync(game);
  } catch {
    return [];
  }
  const out = names.filter((n) => spToolName(n) && !isParkDir(n));
  if (names.includes('version.dll') && isSandboxDll(path.join(game, 'version.dll')))
    out.push('version.dll');
  if (
    names.includes('winmm.dll') &&
    !(verifyWinmm && verifyWinmm(path.join(game, 'winmm.dll')).ok) &&
    isSandboxDll(path.join(game, 'winmm.dll'))
  )
    out.push('winmm.dll');
  return out;
}
/**
 * Check whether a path exists.
 * @param {string} p
 * @returns {boolean}
 */
const exists = (p) => {
  try {
    fs.lstatSync(p);
    return true;
  } catch {
    return false;
  }
};
/**
 * Return the directory holding disabled mod files.
 * @param {string} game
 * @returns {string}
 */
const disabledDir = (game) => parkDir(game, DIR_NAME, LEGACY_DIR_NAME);
/**
 * The parking folder in use: the current name, or the legacy one while only that exists (its rename has not worked yet).
 * @param {string} game
 * @param {string} name
 * @param {string} legacy
 * @returns {string}
 */
function parkDir(game, name, legacy) {
  const cur = path.join(game, name);
  if (exists(cur)) return cur;
  const old = path.join(game, legacy);
  return exists(old) ? old : cur;
}

/**
 * Rewrite absolute journal paths that point into a legacy parking folder so they point into the renamed one.
 * @param {string} game
 * @param {object} j journal (changed in place)
 * @returns {boolean} true when a path changed
 */
function rewriteLegacyPaths(game, j) {
  const pairs = [
    [LEGACY_DIR_NAME, DIR_NAME],
    [LEGACY_TOOLS_DIR, TOOLS_DIR],
  ]
    .filter(([, cur]) => exists(path.join(game, cur)))
    .map(([old, cur]) => [path.join(game, old), path.join(game, cur)]);
  let changed = false;
  const fix = (p) => {
    if (typeof p !== 'string') return p;
    for (const [old, cur] of pairs) {
      const head = p.slice(0, old.length);
      const sameHead =
        process.platform === 'win32' ? head.toLowerCase() === old.toLowerCase() : head === old;
      if (
        sameHead &&
        (p.length === old.length || p[old.length] === path.sep || p[old.length] === '/')
      ) {
        changed = true;
        return cur + p.slice(old.length);
      }
    }
    return p;
  };
  for (const key of ['ops', 'prevOps'])
    if (Array.isArray(j[key]))
      for (const op of j[key]) if (op) ((op.from = fix(op.from)), (op.to = fix(op.to)));
  return changed;
}

/**
 * Move the legacy parking folders (named before the product rename) to the current names. Idempotent, never throws.
 * A folder whose rename fails (a file held open) stays where it is and is used under its old name (parkDir); the next call tries again.
 * When the current name exists too, the legacy folder is left alone (removed only when empty).
 * @param {string} game
 * @returns {string[]} the folders renamed now
 */
function migrateLegacy(game) {
  const done = [];
  if (!game) return done;
  for (const [old, cur] of [
    [LEGACY_DIR_NAME, DIR_NAME],
    [LEGACY_TOOLS_DIR, TOOLS_DIR],
  ]) {
    const from = path.join(game, old),
      to = path.join(game, cur);
    if (!exists(from)) continue;
    try {
      if (exists(to)) {
        if (!fs.readdirSync(from).length) fs.rmdirSync(from);
        continue;
      }
      hooks.rename(from, to);
      done.push(to);
    } catch {
      /* busy: used under the old name until a later call manages it */
    }
  }
  if (done.length) {
    try {
      const f = path.join(game, DIR_NAME, JOURNAL);
      const j = JSON.parse(fs.readFileSync(f, 'utf8'));
      if (rewriteLegacyPaths(game, j)) writeJournal(game, j);
    } catch {
      /* no journal (only the tools folder was parked) */
    }
  }
  return done;
}

/**
 * Create an error carrying a stable machine code.
 * @param {string} code
 * @param {string} message
 * @returns {Error}
 */
function mkErr(msg, code) {
  const e = new Error(msg);
  e.code = code || 'SPMODE';
  e.t2rExplained = true;
  return e;
}

/**
 * Move a file, falling back to copy+unlink across devices.
 * @param {string} from
 * @param {string} to
 * @returns {void}
 */
function move(from, to) {
  // a rename that waits out a virus scanner (EBUSY / EPERM / EACCES); never overwrites
  if (exists(to)) throw mkErr(`Cannot move ${from} to ${to}: the target already exists.`, 'EEXIST');
  fs.mkdirSync(path.dirname(to), { recursive: true });
  let last = null;
  for (const ms of BACKOFF) {
    hooks.sleep(ms);
    try {
      hooks.rename(from, to);
      return;
    } catch (e) {
      last = e;
      if (!/^(EBUSY|EPERM|EACCES)$/.test(e.code || '')) break;
    }
  }
  const busy = /^(EBUSY|EPERM|EACCES)$/.test((last && last.code) || '');
  throw mkErr(
    busy
      ? `Windows (or an antivirus) is holding ${path.basename(from)}: close the game and the Steam helper, wait a moment, then try again. (${last.code})`
      : `Could not move ${path.basename(from)}: ${last && last.message}`,
    last && last.code,
  );
}

/**
 * Read the SP/MP transaction journal.
 * @param {string} game
 * @returns {object|null}
 */
function readJournal(game) {
  migrateLegacy(game);
  try {
    const j = JSON.parse(fs.readFileSync(path.join(disabledDir(game), JOURNAL), 'utf8'));
    rewriteLegacyPaths(game, j); // (a crash between the folder rename and the journal rewrite left old paths in it)
    return j;
  } catch {
    return null;
  }
}
/**
 * Write the SP/MP transaction journal atomically.
 * @param {string} game
 * @param {object|null} journal
 * @returns {void}
 */
function writeJournal(game, j) {
  const d = disabledDir(game);
  fs.mkdirSync(d, { recursive: true });
  const f = path.join(d, JOURNAL),
    t = f + '.new';
  fs.writeFileSync(t, JSON.stringify(j, null, 1));
  fs.renameSync(t, f); // (journal writes bypass hooks.rename: only the mod files are fault-injected)
}

// Runs ops [{from, to}] one by one, journaled; on any failure undoes the finished ones in reverse order and rethrows. Returns nothing.
/**
 * Apply a sequence of recorded file operations.
 * @param {object[]} ops
 * @param {boolean} [reverse]
 * @returns {void}
 */
function runOps(game, j) {
  j.done = 0;
  j.state = 'open';
  writeJournal(game, j);
  try {
    for (const op of j.ops) {
      move(op.from, op.to);
      j.done++;
      writeJournal(game, j);
    }
  } catch (e) {
    rollback(game, j);
    throw e;
  }
}
/**
 * Replay an interrupted transaction in reverse.
 * @param {object} journal
 * @returns {string[]}
 */
function rollback(game, j) {
  // undo every op that took effect (all are checked: the journal may lag one op behind after a crash); idempotent
  const bad = [];
  for (let i = j.ops.length - 1; i >= 0; i--) {
    const op = j.ops[i];
    try {
      if (exists(op.to) && !exists(op.from)) {
        fs.mkdirSync(path.dirname(op.from), { recursive: true });
        fs.renameSync(op.to, op.from);
      }
    } catch (e) {
      bad.push(`${op.to}: ${e.message}`);
    }
  }
  if (bad.length) {
    j.state = 'rollback-failed';
    j.failed = bad;
    try {
      writeJournal(game, j);
    } catch {}
    return bad;
  }
  if (j.txn === 'enable')
    writeJournal(game, {
      v: 1,
      txn: 'disable',
      state: 'committed',
      ops: j.prevOps,
      entries: j.entries,
      swapOriginal: j.swapOriginal,
      at: j.at,
    }); // the mod is disabled again, as before
  else clearIfEmpty(game);
  return bad;
}
/**
 * Remove a directory when it has no entries.
 * @param {string} dir
 * @returns {void}
 */
function clearIfEmpty(game) {
  // remove the journal and the disabled folder when nothing of the mod is in it any more
  const d = disabledDir(game);
  try {
    fs.unlinkSync(path.join(d, JOURNAL));
  } catch {}
  try {
    if (!fs.readdirSync(d).length) fs.rmdirSync(d);
  } catch {}
}

// Entries of the game folder that belong to the mod. { items: [rel], dll: 'move' | 'swap' | 'none' }
/**
 * Plan the file moves required by the current mode.
 * @param {string} game
 * @param {{verifyWinmm: Function}} deps
 * @returns {object}
 */
function plan(game, { verifyWinmm }) {
  const items = [];
  let dll = 'none';
  let names = [];
  try {
    names = fs.readdirSync(game);
  } catch (e) {
    throw mkErr(`Cannot read the game folder ${game} (${e.code || e.message}).`, e.code);
  }
  const hasOrig = names.includes('winmm.dll.t2r-original');
  const baseMod = names.some((n) => /^modloader\.asi$/i.test(n));
  for (const n of names) if ((ownedRoot(n) || baseModRoot(n)) && !isParkDir(n)) items.push(n);
  const mods = names.find((n) => /^mods$/i.test(n));
  if (
    mods &&
    (baseMod ||
      exists(path.join(game, mods, 'tlou-ii-mp.psarc.ini')) ||
      exists(path.join(game, mods, 'tlou-ii-mp.psarc')))
  )
    items.push(mods);
  // the backed-up foreign winmm.dll is a mod loader when modloader.asi is there: it must not come back as winmm.dll in SP
  const origIsBase = hasOrig && baseMod;
  if (origIsBase) items.push('winmm.dll.t2r-original');
  if (names.includes('winmm.dll') && verifyWinmm(path.join(game, 'winmm.dll')).ok) {
    items.push('winmm.dll');
    dll = hasOrig && !origIsBase ? 'swap' : 'move';
  }
  return { items, dll, hasOrig };
}

/**
 * Return the current SP/MP file-switch state.
 * @param {string} game
 * @returns {object}
 */
function status(game) {
  // { disabled: committed manifest present, pending: an unfinished journal, folder, items }
  const j = game ? readJournal(game) : null;
  return {
    folder: game ? disabledDir(game) : '',
    disabled: !!(j && j.state === 'committed' && j.txn === 'disable'),
    journal: j,
  };
}

/**
 * Refuse a file switch while the game is running.
 * @param {string} game
 * @param {Function} [isRunning]
 * @returns {void}
 */
async function guardRunning(deps) {
  // the helper (t2r-steam.exe) exits a few seconds after the game does: wait for it instead of sending the player away
  if (deps.helper) {
    const end = Date.now() + (deps.helperWaitMs == null ? 20000 : deps.helperWaitMs);
    while ((await deps.helper()) && Date.now() < end) await new Promise((r) => setTimeout(r, 250));
  }
  const procs = deps.procs ? await deps.procs() : [];
  if (procs.length)
    throw mkErr(
      'The game is running (or still starting). Close it first, then switch.',
      'EBUSYGAME',
    );
  if (deps.helper && (await deps.helper()))
    throw mkErr(
      'The Twelve Weeks Steam helper (t2r-steam.exe) is still running. Close the game, wait a few seconds, then switch.',
      'EBUSYHELPER',
    );
}

// Crash recovery, run at launcher start and before every switch: an open journal means a step was cut off.
// disable cut off -> undo it (the mod is back, mode stays MP); enable cut off -> undo it (stays SP). Returns { recovered, failed }.
/**
 * Roll back an interrupted SP/MP switch.
 * @param {string} game
 * @returns {{recovered: boolean, failed?: string[]}}
 */
function recover(game) {
  const j = readJournal(game);
  if (!j || j.state === 'committed' || j.state === 'rolledback') return { recovered: false };
  if (j.state === 'rollback-failed' || j.state === 'open') {
    const bad = rollback(game, j);
    return bad.length ? { recovered: false, failed: bad } : { recovered: true };
  }
  return { recovered: false };
}

// ---------------------------------------------------------------- the two switches
// deps: { verifyWinmm(file), procs() -> [{id}], helper() -> bool }
/**
 * Plan the file moves that disable the mod.
 * @param {string} game
 * @param {{items: string[], dll: string}} p
 * @returns {object}
 */
function planDisableMove(game, p) {
  const d = disabledDir(game);
  if (exists(d) && fs.readdirSync(d).some((n) => n !== JOURNAL))
    throw mkErr(
      `The folder ${d} already holds files but is not a finished switch. Move or delete it, then try again.`,
      'EDIRTY',
    );
  const ops = [];
  if (p.dll === 'swap') {
    ops.push({
      from: path.join(game, 'winmm.dll'),
      to: path.join(d, 'winmm.dll'),
    });
    ops.push({
      from: path.join(game, 'winmm.dll.t2r-original'),
      to: path.join(game, 'winmm.dll'),
    });
  }
  for (const rel of p.items) {
    if (rel === 'winmm.dll' && p.dll === 'swap') continue;
    ops.push({ from: path.join(game, rel), to: path.join(d, rel) });
  }
  // SP debug tools parked in TwelveWeeks-sp-tools come back (never overwriting what the game folder already has)
  const tools = [];
  try {
    for (const n of fs.readdirSync(toolsDir(game)))
      if (
        (spToolName(n) || n === 'version.dll' || n === 'winmm.dll') &&
        !isBackupName(n) &&
        !exists(path.join(game, n))
      )
        tools.push(n);
  } catch {}
  return { folder: d, ops, tools, items: p.items, swapOriginal: p.dll === 'swap' };
}

/**
 * Execute a validated disable plan.
 * @param {string} game
 * @param {object} plan
 * @returns {object}
 */
function executeDisableMove(game, plan) {
  if (!plan.ops.length) {
    for (const n of plan.tools) {
      try {
        move(path.join(toolsDir(game), n), path.join(game, n));
      } catch {}
    }
    try {
      fs.rmdirSync(toolsDir(game));
    } catch {}
    return { already: false, folder: plan.folder, moved: 0, files: [], nothing: true };
  }
  const ops = [...plan.ops];
  for (const n of plan.tools)
    ops.push({ from: path.join(toolsDir(game), n), to: path.join(game, n) });
  const j = {
    v: 1,
    txn: 'disable',
    ops,
    swapOriginal: plan.swapOriginal,
    entries: plan.items,
    at: new Date().toISOString(),
  };
  runOps(game, j);
  j.state = 'committed';
  writeJournal(game, j);
  try {
    fs.rmdirSync(toolsDir(game));
  } catch {}
  return {
    already: false,
    folder: plan.folder,
    moved: plan.items.length,
    files: plan.items,
    restoredOriginal: plan.swapOriginal,
  };
}

/**
 * Move the mod out of the game folder for single-player.
 * @param {string} game
 * @param {object} deps
 * @returns {Promise<object>}
 */
async function disable(game, deps) {
  if (!game || !exists(path.join(game, 'tlou-ii.exe')))
    throw mkErr(
      'The game folder is not set or tlou-ii.exe is not there. Pick it in Settings > Game.',
      'ENOGAME',
    );
  await guardRunning(deps);
  const rec = recover(game);
  if (rec.failed)
    throw mkErr(
      `An earlier switch was cut off and could not be undone (${rec.failed[0]}). Nothing was changed now; fix that file and try again.`,
      'ERECOVER',
    );
  const st = status(game);
  if (st.disabled) return { already: true, folder: st.folder, moved: 0, files: [] }; // idempotent
  return executeDisableMove(game, planDisableMove(game, plan(game, deps)));
}

/**
 * Plan the file moves that restore the mod for multiplayer.
 * @param {string} game
 * @param {object} deps
 * @param {object} jd
 * @returns {object}
 */
function planEnableMove(game, deps, jd) {
  const d = disabledDir(game);
  const ops = [];
  const T = toolsDir(game),
    stamp = Date.now();
  // SP tools: whatever is in the game folder now, plus t2r-sp* entries an old launcher's journal listed (they sit in the disabled folder), go to TwelveWeeks-sp-tools.
  const inGame = spToolsInGame(game, deps.verifyWinmm);
  const oldListed = jd.entries.filter((rel) => spToolName(rel) && exists(path.join(d, rel)));
  const incoming = {}; // name -> sources, oldest first
  const addSrc = (n, from) => {
    (incoming[n] = incoming[n] || []).push(from);
  };
  for (const rel of oldListed) addSrc(rel, path.join(d, rel));
  for (const n of inGame) addSrc(n, path.join(game, n));
  const sandboxWinmm = inGame.includes('winmm.dll');
  for (const [n, srcs] of Object.entries(incoming)) {
    const dest = path.join(T, n);
    if (exists(dest)) ops.push({ from: dest, to: `${dest}.${stamp}` }); // keep the older copy, never fail the switch over debug files
    srcs.forEach((from, i) =>
      ops.push({
        from,
        to: i === srcs.length - 1 ? dest : `${dest}.${stamp}.${i + 1}`,
      }),
    );
  }
  const modEntries = jd.entries.filter((rel) => !spToolName(rel));
  if (jd.swapOriginal && !sandboxWinmm && exists(path.join(game, 'winmm.dll')))
    ops.push({
      from: path.join(game, 'winmm.dll'),
      to: path.join(game, 'winmm.dll.t2r-original'),
    });
  // An entry the parked mod owns that showed up again in the game folder while in SP mode (a tool or an older install wrote it, e.g. tlou-ii-mp/sidbase.bin):
  // the parked copy wins, the stray one is moved aside to TwelveWeeks-sp-tools/replaced-<stamp>/ (journaled, so a rollback puts it back) instead of failing the switch.
  for (const rel of modEntries) {
    const to = path.join(game, rel);
    if (exists(to) && !ops.some((o) => o.from === to))
      ops.push({ from: to, to: path.join(T, `replaced-${stamp}`, rel) });
    ops.push({ from: path.join(d, rel), to });
  }
  const modOps = ops.filter((o) => !o.to.startsWith(T + path.sep));
  const missing = modOps.filter((o) => !exists(o.from));
  if (missing.length)
    throw mkErr(
      `${path.basename(missing[0].from)} is missing from ${d} (moved or deleted by hand). Press REPAIR INSTALL in Settings > Files after switching, or restore it there.`,
      'EMISSING',
    );
  const clash = modOps.find(
    (o) =>
      exists(o.to) &&
      !ops.some((p) => p.from === o.to) &&
      !(
        jd.swapOriginal &&
        o.to === path.join(game, 'winmm.dll') &&
        o.from !== path.join(game, 'winmm.dll')
      ),
  ); // never overwrite: stop before the first move
  if (clash)
    throw mkErr(
      `Cannot move the mod back: ${clash.to} already exists in the game folder (a mod file was installed there again). Delete it (or Verify / Repair in Settings > Files), then press SWITCH TO MP again.`,
      'EEXIST',
    );
  return { d, ops, modEntries, inGame, oldListed };
}

/**
 * Execute a validated enable plan.
 * @param {string} game
 * @param {object} plan
 * @param {object} jd
 * @param {object} deps
 * @returns {object}
 */
function executeEnableMove(game, plan, jd, deps) {
  // the disable manifest (prevOps / entries) survives a rollback of the enable: rollback() writes it back as the committed state
  const j = {
    v: 1,
    txn: 'enable',
    ops: plan.ops,
    prevOps: jd.ops,
    swapOriginal: jd.swapOriginal,
    entries: jd.entries,
    at: new Date().toISOString(),
  };
  runOps(game, j);
  // verify the dll that came back
  let needsRepair = false;
  if (jd.entries.includes('winmm.dll') && !deps.verifyWinmm(path.join(game, 'winmm.dll')).ok) {
    try {
      move(path.join(game, 'winmm.dll'), path.join(plan.d, 'winmm.dll.damaged'));
    } catch {}
    needsRepair = true;
  }
  try {
    fs.unlinkSync(path.join(plan.d, JOURNAL));
  } catch {}
  try {
    fs.rmdirSync(plan.d);
  } catch {
    /* a damaged dll or foreign files stay in it */
  }
  return {
    already: false,
    moved: plan.modEntries.length,
    files: plan.modEntries,
    needsRepair,
    toolsMoved: plan.inGame.length + plan.oldListed.length,
  };
}

// TOP-UP of an install that is already in SP mode (a launcher older than the base-mod parking switched it, or a later install/crash left base-mod files in the
// game folder): disable() returns { already: true } for such an install and used to leave them there. Moves what plan() would move now into the
// disabled folder and adds it to the committed manifest, so SWITCH TO MP brings it back. Returns { moved, files }.
/**
 * Top up an install that is already in single-player mode with base-mod leftovers.
 * @param {string} game
 * @param {object} deps
 * @returns {Promise<{moved: number, files: string[]}>}
 */
async function topUp(game, deps) {
  if (!game || !exists(path.join(game, 'tlou-ii.exe'))) return { moved: 0, files: [] };
  const jd = readJournal(game);
  if (!jd || jd.state !== 'committed' || jd.txn !== 'disable') return { moved: 0, files: [] };
  if (deps.procs && (await deps.procs()).length) return { moved: 0, files: [] }; // a running game: moving its files is for later (no waiting, no error)
  const d = disabledDir(game);
  let names = [];
  try {
    names = fs.readdirSync(game);
  } catch {
    return { moved: 0, files: [] };
  }
  const items = [];
  const baseMod = names.some((n) => /^modloader\.asi$/i.test(n));
  for (const n of names)
    if ((ownedRoot(n) || baseModRoot(n)) && !isParkDir(n) && n !== 'winmm.dll.t2r-original')
      items.push(n);
  const mods = names.find((n) => /^mods$/i.test(n));
  if (
    mods &&
    (baseMod ||
      exists(path.join(game, mods, 'tlou-ii-mp.psarc.ini')) ||
      exists(path.join(game, mods, 'tlou-ii-mp.psarc')))
  )
    items.push(mods);
  const ops = [],
    entries = [];
  const add = (from, rel, to) => {
    ops.push({ from, to });
    entries.push(rel);
  };
  for (const rel of items) {
    if (spToolName(rel)) continue;
    if (exists(path.join(d, rel))) continue; // never overwrite: that name is already parked
    add(path.join(game, rel), rel, path.join(d, rel));
  }
  // a mod loader dll: an old launcher put winmm.dll.t2r-original back as winmm.dll. It is not a T2R build (verifyWinmm fails) and not our SP sandbox
  // dll, and modloader.asi sits next to it: park it under the name the current layout uses; the manifest then restores it as winmm.dll.t2r-original.
  const w = path.join(game, 'winmm.dll');
  let swapFix = false;
  if (
    baseMod &&
    names.includes('winmm.dll') &&
    !deps.verifyWinmm(w).ok &&
    !isSandboxDll(w) &&
    !exists(path.join(d, 'winmm.dll.t2r-original')) &&
    !names.includes('winmm.dll.t2r-original')
  ) {
    add(w, 'winmm.dll.t2r-original', path.join(d, 'winmm.dll.t2r-original'));
    swapFix = true;
  }
  if (
    names.includes('winmm.dll.t2r-original') &&
    baseMod &&
    !exists(path.join(d, 'winmm.dll.t2r-original'))
  )
    add(
      path.join(game, 'winmm.dll.t2r-original'),
      'winmm.dll.t2r-original',
      path.join(d, 'winmm.dll.t2r-original'),
    );
  if (!ops.length) return { moved: 0, files: [] };
  const done = [];
  try {
    for (const op of ops) {
      move(op.from, op.to);
      done.push(op);
    }
  } catch (e) {
    for (let i = done.length - 1; i >= 0; i--) {
      try {
        fs.renameSync(done[i].to, done[i].from);
      } catch {}
    }
    throw e;
  }
  const j = Object.assign({}, jd, {
    ops: (jd.ops || []).concat(ops),
    entries: (jd.entries || []).concat(entries.filter((e) => !(jd.entries || []).includes(e))),
    at: new Date().toISOString(),
  });
  if (swapFix) j.swapOriginal = false; // our winmm.dll comes back as winmm.dll; the mod loader as winmm.dll.t2r-original (entries)
  writeJournal(game, j);
  return { moved: entries.length, files: entries };
}

/**
 * Restore the mod files for multiplayer.
 * @param {string} game
 * @param {object} deps
 * @returns {Promise<object>}
 */
async function enable(game, deps) {
  if (!game || !exists(path.join(game, 'tlou-ii.exe')))
    throw mkErr(
      'The game folder is not set or tlou-ii.exe is not there. Pick it in Settings > Game.',
      'ENOGAME',
    );
  await guardRunning(deps);
  const rec = recover(game);
  if (rec.failed)
    throw mkErr(
      `An earlier switch was cut off and could not be undone (${rec.failed[0]}). Nothing was changed now.`,
      'ERECOVER',
    );
  const jd = readJournal(game);
  if (!jd || jd.state !== 'committed' || jd.txn !== 'disable')
    return { already: true, moved: 0, files: [] }; // idempotent: nothing is disabled
  return executeEnableMove(game, planEnableMove(game, deps, jd), jd, deps);
}

// The files that would start the mod: used to refuse "PLAY SINGLEPLAYER" while the game folder still has the mod's winmm.dll.
/**
 * Check whether the mod is currently installed in the game folder.
 * @param {string} game
 * @param {Function} verifyWinmm
 * @returns {boolean}
 */
function modInGame(game, verifyWinmm) {
  const w = path.join(game, 'winmm.dll');
  return (
    (exists(w) && verifyWinmm(w).ok) ||
    exists(path.join(game, 't2r-ext.dll')) ||
    exists(path.join(game, 'modloader.asi'))
  ); // (SP tools never count: a sandbox winmm.dll fails verifyWinmm, t2r-sp* are not mod files)
}

// SP mode from the three truths: a committed disabled-folder manifest, else the saved flag unless the mod is in the game folder again (a reinstall, another game folder).
/**
 * Resolve the effective mode from files and the config flag.
 * @param {string} game
 * @param {boolean} flag
 * @param {Function} verifyWinmm
 * @returns {string}
 */
function modeFor(game, flag, verifyWinmm) {
  if (!game) return !!flag;
  if (status(game).disabled) return true;
  if (flag && !modInGame(game, verifyWinmm)) return true;
  return false;
}

module.exports = {
  topUp,
  DIR_NAME,
  TOOLS_DIR,
  LEGACY_DIR_NAME,
  LEGACY_TOOLS_DIR,
  migrateLegacy,
  spToolName,
  isSandboxDll,
  spToolsInGame,
  JOURNAL,
  STEAM_APP_ID,
  hooks,
  disable,
  enable,
  recover,
  status,
  modInGame,
  modeFor,
  plan,
  ownedRoot,
  baseModRoot,
  disabledDir,
  steamUrl: (id = STEAM_APP_ID) => `steam://rungameid/${id}`,
};
