/**
 * @file Full Play orchestration: preflight, install, launch and window watching.
 */
'use strict';

const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');
const linux = require('../linux');
const PS = require('../../src/playstate');
const upd = require('../update');
const spmode = require('../spmode');
const health = require('../health');
const { run, exists, readTrim, noProgress } = require('./util');
const { ctx, testHooks, startedPids } = require('./state');
const { cleanName, nameToken } = require('./identity');
const { isGameDir, exeBuild, STEAM_APP_ID, EXE_TIMESTAMP, EXE_IMAGE_SIZE } = require('./discovery');
const { setDllTarget, verifyWinmm } = require('./dll');
const { resolveServer, probeServer } = require('./server');
const {
  installMod,
  syncFeatureFlags,
  modCurrent,
  missingModFiles,
  writeToken,
  listFiles,
} = require('./install');
const { psGameProcs, killProc, killSteamHelpers, spTopUp } = require('./processes');
const {
  steamGate,
  STEAM_MSG,
  steamRunning,
  startSteamHelper,
  playerName,
  startFailure,
} = require('./steam');
const presence = require('../rpc/presence'); // Discord Rich Presence (lib/rpc, OmegaMan)

// PLAY SINGLEPLAYER: the plain game with no mod, no Steam helper, no daemon, no server probe. Windows starts the same tlou-ii.exe MP starts
// (spawnGameExe; not through steam://); Linux keeps steam://rungameid/<appid> (Proton needs Steam to start it).
// opts: { cfg: {game_dir}, openUrl(url) -> Promise (Linux), platform, linuxDeps, log, progress, noWait }. Never installs or writes anything into the game folder.
/**
 * Launch the plain single-player executable without the mod or helper.
 * @param {object} opts
 * @returns {Promise<object>}
 */
async function playSingleplayer(opts) {
  const { cfg, log } = opts;
  const plat = opts.platform || process.platform;
  ctx.plat = plat;
  ctx.deps = opts.linuxDeps || {};
  if (!(await steamGate(plat)).running) throw new Error(STEAM_MSG);
  if (!cfg.game_dir) throw new Error('The game folder is not set. Pick it in Settings > Game.');
  if (!isGameDir(cfg.game_dir))
    throw new Error(
      `tlou-ii.exe was not found in ${cfg.game_dir} (moved, renamed, or removed by an antivirus). Pick the game folder again in Settings > Game, or verify the game files in Steam.`,
    );
  if ((await psGameProcs()).length) throw new Error('The game is already running. Close it first.');
  const progress = opts.progress || noProgress;
  // base-mod leftovers of an install that is already in SP mode are parked first (no game process runs here: checked just above)
  try {
    const t = await spTopUp(cfg.game_dir, true);
    if (t.moved && log)
      log(
        `SWITCH TO SP: ${t.moved} leftover mod entries moved to ${spmode.disabledDir(cfg.game_dir)} (${t.files.join(', ')})`,
      );
  } catch (e) {
    if (log) log('SP top-up: ' + e.message);
  }
  if (spmode.modInGame(cfg.game_dir, (f) => verifyWinmm(f, 0)))
    throw new Error(
      'The mod files are in the game folder again, so the game would start with the mod. Press SWITCH TO MP, then SWITCH TO SP again.',
    );
  progress({
    phase: 'play-window',
    label: 'Starting the game (the first start after an update can take a minute)',
    unit: 'time',
    done: 0,
    total: 0,
  });
  // Vanilla game: the launcher hands presence over completely and shows nothing itself, so only the game can publish an activity.
  presence.started(cfg.game_dir);
  await presence.vanilla();
  let url = null,
    w;
  try {
    if (plat === 'win32') {
      const exe = path.join(cfg.game_dir, 'tlou-ii.exe');
      if (!testHooks.spawn)
        await run('reg', [
          'add',
          'HKCU\\Software\\Naughty Dog\\The Last of Us Part II',
          '/v',
          'ShowLauncher',
          '/t',
          'REG_DWORD',
          '/d',
          '0',
          '/f',
        ]); // same as MP
      opts.log(`Starting the plain game (single player): ${exe}`);
      const spawnedAt = Date.now();
      await spawnGameExe(cfg);
      w = await awaitGameStart(opts, progress, opts.log, null, spawnedAt);
    } else {
      url = spmode.steamUrl(STEAM_APP_ID);
      opts.log(`Starting the plain game (single player) through Steam: ${url}`);
      await opts.openUrl(url);
      w = opts.noWait
        ? { windowUp: false, exited: false, waitedMs: 0 }
        : await waitForGameWindow({
            progress,
            requireSeen: true,
            timeoutMs: opts.windowTimeoutMs || 240000,
            pollMs: (opts.linuxDeps || {}).pollMs || 1500,
          });
    }
    if (w.windowUp) opts.log(`Game window up after ${Math.round(w.waitedMs / 1000)} s.`);
    else if (w.exited)
      throw new Error(health.NEXT_STEPS.replace('press Play again', 'press PLAY SINGLEPLAYER again'));
    else
      opts.log(
        `No game window after ${Math.round(w.waitedMs / 1000)} s - still waiting in the background (is Steam asking for a login or an update?).`,
      );
    return { windowUp: w.windowUp, url };
  } catch (e) {
    await presence.ended();
    throw e;
  }
}

const START_LABEL = 'Starting the game (the first start after an update can take a minute)';
const STEAM_STARTING_LABEL =
  'Steam is starting the game - if a Steam window or dialog is open (install step, cloud sync), finish it';
const STEAM_STARTING_LOG =
  'The game process is alive but shows no window yet. Steam is probably running a first-start step (install script, cloud sync, EULA): if a Steam window or dialog is open, finish it - the launcher keeps waiting.';
// The mod's ext (t2r-ext.dll) opens t2r-ext.log in the game folder at its first game tick: a write after `sinceMs` = "mod loaded and the
// game runs". t2r-ext-boot.txt does not count: it is written from DllMain, before the game has done anything.
// After the first MP session a player's game started as plain single player five times in a row (window up, no t2r-ext-boot.txt line, no
// ext log) and the launcher said nothing. A game window does not prove the mod loaded: watch the boot trail / ext log for a while after the start and,
// when nothing was written, log WHICH mod files the game folder holds (so a report shows the cause) and say so. Never ends or restarts anything.
function modFilesSnapshot(gameDir) {
  const out = [];
  const one = (n) => {
    try {
      const st = fs.statSync(path.join(gameDir, n));
      out.push(`${n} ${st.isDirectory() ? 'dir' : st.size + ' B'}`);
    } catch {
      out.push(`${n} MISSING`);
    }
  };
  for (const n of [
    'winmm.dll',
    't2r-ext.dll',
    't2r-version.txt',
    'modloader.asi',
    'mods',
    'tlou-ii-mp.psarc',
    'winmm.dll.t2r-original',
    'version.dll',
    'TwelveWeeks-disabled',
    'Factions2-disabled', // (parking folder name before the product rename)
  ])
    one(n);
  return out.join(', ');
}
function modBootedSince(gameDir, sinceMs) {
  for (const n of ['t2r-ext-boot.txt', 't2r-ext.log']) {
    try {
      if (fs.statSync(path.join(gameDir, n)).mtimeMs >= sinceMs) return true;
    } catch {}
  }
  return false;
}
function watchModLoad({ gameDir, spawnedAt, log, waitMs = 90000, pollMs = 3000 }) {
  const t0 = Date.now();
  const tick = () => {
    try {
      if (modBootedSince(gameDir, spawnedAt - 2000)) return;
      if (Date.now() - t0 < waitMs) return void setTimeout(tick, pollMs).unref();
      log(
        `WARNING: the game is running but the mod did not load (no t2r-ext-boot.txt / t2r-ext.log write ${Math.round(waitMs / 1000)} s after the start) - it is probably showing the plain single-player menu. Game folder: ${modFilesSnapshot(gameDir)}. Press Verify / Repair in Settings > Files, then Play again.`,
      );
    } catch {}
  };
  setTimeout(tick, pollMs).unref();
}

/**
 * Check whether the mod logged activity after a timestamp.
 * @param {string} gameDir
 * @param {number} sinceMs
 * @returns {boolean}
 */
function modLoadedSince(gameDir, sinceMs) {
  try {
    const st = fs.statSync(path.join(gameDir, 't2r-ext.log'));
    return st.mtimeMs >= sinceMs && st.size > 0;
  } catch {
    return false;
  }
}

// After the game exe was spawned: wait for its window or for the mod to load. A Steam-licensed exe started directly exits and Steam starts it again
//. A restarted process that stays alive without a window is NOT a startup crash (first-launch step after a Steam verify):
// it is waited for up to opts.steamWaitMs (5 min) with a clear status. Only a process that exits without the mod ever loading is a failure.
// -> { windowUp, exited, waitedMs, modLoaded? }
/**
 * Wait for a launched game to reach a usable state.
 * @param {object} opts
 * @param {Function} progress
 * @param {Function} log
 * @param {string} gameDir
 * @param {number} spawnedAt
 * @returns {Promise<object>}
 */
async function awaitGameStart(opts, progress, log, gameDir, spawnedAt) {
  const modLoaded = gameDir ? () => modLoadedSince(gameDir, spawnedAt - 2000) : null;
  const poll = (opts.linuxDeps || {}).pollMs || 1500,
    clk = opts.clock || {}; // clk: tests only ({ now, sleep })
  const w = opts.noWait
    ? { windowUp: false, exited: false, waitedMs: 0 }
    : await waitForGameWindow({
        ...clk,
        progress,
        modLoaded,
        pollMs: poll,
        timeoutMs: opts.windowTimeoutMs || 180000,
      });
  if (!w.exited) return w;
  log(
    'The game process ended right after the start - waiting up to 60 s for Steam to start it again ...',
  );
  const w2 = await waitForGameWindow({
    ...clk,
    progress,
    modLoaded,
    pollMs: poll,
    timeoutMs: opts.restartGraceMs ?? 60000,
    goneAfter: Infinity,
  });
  if (w2.windowUp) return { ...w2, waitedMs: w.waitedMs + w2.waitedMs };
  if (!w2.alive) return w; // Steam never restarted it: a real startup failure
  log(STEAM_STARTING_LOG);
  const w3 = await waitForGameWindow({
    ...clk,
    progress,
    modLoaded,
    pollMs: poll,
    label: STEAM_STARTING_LABEL,
    timeoutMs: opts.steamWaitMs ?? 300000,
  });
  const waitedMs = w.waitedMs + w2.waitedMs + w3.waitedMs;
  if (w3.windowUp) return { ...w3, waitedMs };
  if (w3.exited) return { windowUp: false, exited: true, waitedMs, afterRestart: true }; // alive for a while, then gone without the mod
  return { windowUp: false, exited: false, waitedMs }; // still alive after the wait: not a crash, the caller keeps waiting in the background
}

/**
 * Check whether the mod reported activity since the launch.
 * @param {Function|null} modLoaded
 * @returns {boolean}
 */
function modStarted(modLoaded) {
  try {
    return !!(modLoaded && modLoaded());
  } catch {
    return false; // best effort
  }
}

/**
 * Classify one window poll; returns a final result or the next seen/gone state.
 * @param {object} state
 * @returns {{result?: object, seen?: boolean, gone?: number}}
 */
function classifyWindowPoll({
  list,
  modLoaded,
  seen,
  gone,
  requireSeen,
  goneAfter,
  waited,
  timeoutMs,
}) {
  const waitedMs = waited;
  if (list.some((p) => p.window)) return { result: { windowUp: true, exited: false, waitedMs } };
  // the mod's ext wrote to its log since the start = the game runs with the mod, window or not (a Steam first-launch dialog can cover the window)
  if (modStarted(modLoaded))
    return { result: { windowUp: true, modLoaded: true, exited: false, waitedMs } };
  const nextSeen = seen || list.length > 0;
  const nextGone = list.length || (requireSeen && !nextSeen) ? 0 : gone + 1;
  if (nextGone >= goneAfter && waited > 10000)
    return { result: { windowUp: false, exited: true, alive: false, waitedMs } };
  if (waited >= timeoutMs)
    return {
      result: { windowUp: false, exited: false, alive: list.length > 0, waitedMs: waited },
    };
  return { seen: nextSeen, gone: nextGone };
}

// Wait for the window of the game this launcher just started. procs() -> [{id, window}] (injectable for tests). Never ends a process.
// Resolves { windowUp, exited, waitedMs }. exited: no tlou-ii process at all for `goneAfter` polls in a row after the first 10 s.
/**
 * Poll processes until the game window appears, exits or times out.
 * @param {object} [options]
 * @returns {Promise<{windowUp: boolean, exited: boolean, waitedMs: number}>}
 */
async function waitForGameWindow({
  procs = psGameProcs,
  timeoutMs = 180000,
  pollMs = 1500,
  goneAfter = 6,
  requireSeen = false,
  progress = noProgress,
  modLoaded = null,
  label = START_LABEL,
  now = Date.now,
  sleep = (ms) => new Promise((r) => setTimeout(r, ms)),
} = {}) {
  const t0 = now();
  let gone = 0,
    seen = false;
  for (;;) {
    const waited = now() - t0;
    progress({
      phase: 'play-window',
      label,
      unit: 'time',
      done: Math.round(waited / 1000),
      total: 0,
    });
    let list = [];
    try {
      list = await procs();
    } catch {
      /* a failed poll is just a miss */
    }
    const next = classifyWindowPoll({
      list,
      modLoaded,
      seen,
      gone,
      requireSeen,
      goneAfter,
      waited,
      timeoutMs,
    });
    if (next.result) return next.result;
    seen = next.seen;
    gone = next.gone;
    await sleep(pollMs);
  }
}

/**
 * Resolve a leftover game process before starting a new one.
 * @param {object} opts
 * @param {Function} log
 * @param {Function} progress
 * @returns {Promise<void>}
 */
async function handleLeftoverProcesses(opts, log, progress) {
  // Leftover tlou-ii processes: only those that already existed when PLAY was pressed (recorded now) and still have no window after a
  // generous time, and only after the user agreed. A process this launcher starts below is never touched (a cold boot
  // without a window for 12 s was ended by the old 8 s rule).
  progress({
    phase: 'play-check',
    label: 'Checking for a running game',
    unit: 'none',
    done: 0,
    total: 0,
  });
  const before = (await psGameProcs()).map((p) => p.id);
  if (!before.length) return;
  const t0 = Date.now();
  for (;;) {
    const d = PS.leftover({
      before,
      ours: [...startedPids],
      now: await psGameProcs(),
      waitedMs: Date.now() - t0,
    });
    if (d.action === 'none') break;
    if (d.action === 'running')
      throw new Error(
        'The game is already running. Close it and start it only from this launcher.',
      );
    if (d.action === 'ask') {
      const yes = opts.confirmEnd ? await opts.confirmEnd(d.pids) : false;
      if (!yes)
        throw new Error(
          'A tlou-ii.exe process without a window is still running (it was started before). Close it in Task Manager, then press Play again.',
        );
      for (const id of d.pids) {
        log(
          `Ending a leftover game process (pid ${id}, no window for ${Math.round((Date.now() - t0) / 1000)} s, confirmed).`,
        );
        await killProc(id);
      }
      break;
    }
    progress({
      phase: 'play-check',
      label: 'Waiting for a game process that is still starting',
      unit: 'time',
      done: Math.round((Date.now() - t0) / 1000),
      total: 0,
    });
    await new Promise((r) => setTimeout(r, 1000));
  }
}

/**
 * Recover a cut-off client update and install missing mod files.
 * @param {object} options
 * @returns {Promise<void>}
 */
async function prepareClientFiles({ clientDir, cfg, log, progress, opts, extra }) {
  if (upd.hasPendingUpdate(clientDir)) {
    // an update was cut off while swapping files in: the client folder is half old, half new
    const rec = await upd
      .recoverStaged(clientDir)
      .catch(() => ({ pending: true, complete: false }));
    if (rec.pending && !rec.complete)
      throw new Error(
        'The last update did not finish (the client files are half old, half new). Press Update client (or Verify / Repair), then play again.',
      );
  }
  if (opts.reinstall || !modCurrent(clientDir, cfg)) {
    log('Installing mod files into the game folder ...');
    await installMod(clientDir, cfg, log, progress, opts.onChild, extra);
    log('Mod files updated.');
  } else await syncFeatureFlags(clientDir, cfg.game_dir);
}

/**
 * Ensure the Linux Proton prefix loads the mod's winmm.dll.
 * @param {string} gameDir
 * @param {object} ldeps
 * @param {Function} log
 * @returns {void}
 */
function prepareLinuxPrefix(gameDir, ldeps, log) {
  // Linux: the game's Proton prefix must load the mod's winmm.dll (native override) - done while the game is closed, before anything starts
  const pr = linux.prepareGamePrefix(gameDir, {
    procDir: ldeps.procDir,
  });
  if (!pr.ok) throw new Error(pr.reason);
  if (pr.changed)
    log(
      "The game's Proton prefix now loads the mod (winmm override set; no Steam launch option needed).",
    );
}

/**
 * Start the platform Steam helper and resolve the Steam player name when available.
 * @param {object} options
 * @returns {Promise<{helper: object|null, steamName: string|null}>}
 */
async function startPlayHelper({
  lx,
  plat,
  ldeps,
  clientDir,
  cfg,
  userData,
  ip,
  port,
  log,
  progress,
}) {
  // the Steam helper reads the token file (the player's name), so the file exists before it starts; rewritten below once the Steam name is known
  let steamName = null;
  let helper = null;
  if (lx) writeToken(cfg, playerName(cfg, null, plat).name);
  try {
    if (lx) {
      progress({
        phase: 'play-helper',
        label: 'Starting the Steam helper (the first start creates its Proton prefix)',
        unit: 'none',
        done: 0,
        total: 0,
      });
      helper = await linux.startHelper({
        clientDir,
        gameDir: cfg.game_dir,
        dataDir: userData,
        server: `${ip}:${port}`,
        log,
        deps: ldeps,
      });
      if (helper.pid) {
        steamName = helper.name ? cleanName(helper.name) || null : null;
        log(
          helper.reached
            ? 'Steam helper: reached Steam (Steam shows you as "Playing Spacewar" - that is it).'
            : 'Steam helper: still starting - continuing; it keeps waiting for the game (up to 30 min).',
        );
        // fallback copy for the game's own start of the helper (only used when no external helper runs)
        try {
          const sd = path.join(clientDir, 'steam');
          for (const f of listFiles(sd)) {
            const d = path.join(cfg.game_dir, 't2r-steam', path.relative(sd, f));
            fs.mkdirSync(path.dirname(d), { recursive: true });
            fs.copyFileSync(f, d);
          }
        } catch {}
      } else
        log(
          `Steam helper could not start: ${helper.reason} - matches with other players will use the server relay (slower).`,
        );
    } else {
      progress({
        phase: 'play-helper',
        label: 'Starting the Steam helper',
        unit: 'none',
        done: 0,
        total: 0,
      });
      steamName = await startSteamHelper(clientDir, cfg, `${ip}:${port}`, log);
      if (!steamName && !(await steamRunning()))
        log(
          'Steam does not seem to be running. Start Steam first (the game needs it for matchmaking with friends); continuing without the Steam helper.',
        );
    }
  } catch (e) {
    log(`Steam helper could not start (${e.message}) - playing without it.`);
  }
  return { helper, steamName };
}

/**
 * Write the player name and runtime flags before launching.
 * @param {object} options
 * @returns {{pn: object, version: string}}
 */
function preparePlayerState({
  cfg,
  plat,
  steamName,
  progress,
  log,
  opts,
  server,
  ip,
  port,
  dll,
  clientDir,
}) {
  const pn = playerName(cfg, steamName, plat);
  progress({
    phase: 'play-check',
    label: 'Writing your player name',
    unit: 'none',
    done: 0,
    total: 0,
  });
  writeToken(cfg, pn.name);
  // DEBUG MODE: t2r-debug-mode.txt in the game folder turns the ext's log
  // limits off and its per-frame black box on (t2r-ext.c g_debug_mode); removed when the mode is off, so a normal start is never slowed
  try {
    const dbg = path.join(cfg.game_dir, 't2r-debug-mode.txt');
    if (opts.debugMode) {
      fs.writeFileSync(dbg, 'on\n');
      log('DEBUG MODE: full logs + black box on (bigger t2r-ext.log, slightly lower fps).');
    } else if (exists(dbg)) fs.unlinkSync(dbg);
  } catch (e) {
    log(`Debug mode flag could not be written: ${e.message}`);
  }
  if (pn.source === 'steam')
    log(`Steam is running: playing as '${pn.name}' (Steam shows Spacewar).`);
  if (health.state(opts.healthFile).loop)
    log(
      `Note: the game already closed on startup ${health.state(opts.healthFile).count} times in a row.`,
    );
  if (setDllTarget(dll, ip, port)) log(`Server address updated: ${server} -> ${ip}:${port}`);
  const version = readTrim(path.join(clientDir, 'version.txt'));
  return { pn, version };
}

/**
 * Probe the server for reachability and version compatibility.
 * @param {object} options
 * @returns {Promise<object|null>}
 */
async function verifyPlayServer({
  opts,
  cfg,
  ip,
  port,
  pn,
  version,
  server,
  lx,
  plat,
  extra,
  log,
  progress,
}) {
  if (opts.skipCheck) return null;
  progress({
    phase: 'play-check',
    label: 'Checking the server',
    unit: 'none',
    done: 0,
    total: 0,
  });
  const probe = await probeServer(ip, port, nameToken(pn.name, cfg.token_id), version, 6000, lx);
  if (!probe.reachable) {
    await killSteamHelpers(plat, extra);
    throw new Error(
      `Cannot reach the server at ${server}. Is the host's server running? Is the address right (it changes if the host re-created the tunnel)?`,
    );
  }
  if (probe.versionOk === false) {
    await killSteamHelpers(plat, extra);
    throw new Error(
      `VERSION MISMATCH: your game build is '${version}' but the server runs '${probe.serverVersion || '?'}'. Press UPDATE CLIENT, then play again.`,
    );
  }
  log(
    probe.older
      ? '(The server did not answer the version check - older server. Continuing.)'
      : `Server reachable, same build (${version}).`,
  );
  return probe;
}

/**
 * Re-check the mod files and reinstall once when they disappeared.
 * @param {object} options
 * @returns {Promise<void>}
 */
async function ensurePlayModFiles({
  clientDir,
  cfg,
  progress,
  log,
  opts,
  extra,
  lx,
  plat,
  dll,
  ip,
  port,
}) {
  // the mod files must still be there (an antivirus may have removed them since the copy): one reinstall, then a clear message
  progress({
    phase: 'play-check',
    label: 'Checking the mod files',
    unit: 'none',
    done: 0,
    total: 0,
  });
  await new Promise((r) => setTimeout(r, lx ? 200 : 1500));
  let miss = missingModFiles(clientDir, cfg.game_dir);
  if (!miss.length) return;
  log(`Mod files missing before the start: ${miss.join(', ')} - installing once more`);
  await installMod(clientDir, cfg, log, progress, opts.onChild, extra);
  setDllTarget(dll, ip, port);
  await new Promise((r) => setTimeout(r, lx ? 200 : 3000));
  miss = missingModFiles(clientDir, cfg.game_dir);
  if (miss.length) {
    await killSteamHelpers(plat, extra);
    throw new Error(
      lx
        ? `The mod files (${miss.join(', ')}) disappeared from the game folder right after they were copied (read-only folder or a cleanup tool?): ${cfg.game_dir}`
        : `Your antivirus removed the mod files (${miss.join(', ')}) from the game folder right after they were copied. Without them the game starts without the mod. Open Windows Security > Virus & threat protection > Protection history, restore the item, add the game folder and the launcher's client folder to the exclusions, then press Play again.`,
    );
  }
}

/**
 * Launch the game through Steam on Linux and wait for its process.
 * @param {object} options
 * @returns {Promise<object>}
 */
async function launchLinuxPlay({
  opts,
  cfg,
  ldeps,
  helper,
  pn,
  ip,
  port,
  server,
  plat,
  extra,
  log,
  progress,
}) {
  // t2r-linux.txt = the ext's Linux mode: no Steam-helper gate, server= for its own helper start, helper=external = ours already runs
  fs.writeFileSync(
    path.join(cfg.game_dir, 't2r-linux.txt'),
    `server=${ip}:${port}\n${helper && helper.pid ? 'helper=external\n' : ''}`,
  );
  const lo = linux.launchOptions(ldeps.home);
  if (lo.length)
    log(`(Steam launch option set for the game: ${lo[0]} - fine, the launcher does not need one.)`);
  log(`Starting TLOU2 Twelve Weeks as '${pn.name}' on ${server} through Steam ...`);
  const sc = linux.steamLaunchCommand(ldeps.home);
  await new Promise((resolve, reject) => {
    const launch = ldeps.launch || spawn;
    const c = launch(sc.cmd, sc.args, { detached: true, stdio: 'ignore' });
    c.once('error', (e) =>
      reject(
        new Error(
          `Could not start Steam (${sc.cmd}: ${e.message}). Start Steam yourself, then start The Last of Us Part II from it.`,
        ),
      ),
    );
    c.once('spawn', () => {
      if (c.unref) c.unref();
      resolve();
    });
  });
  // Steam (a cold start can take a minute) then Proton start the game: wait for its process in /proc. requireSeen: "exited" only
  // counts after the process was seen once (Steam itself may need a long time to get going).
  const w = opts.noWait
    ? { windowUp: false, exited: false, waitedMs: 0 }
    : await waitForGameWindow({
        progress,
        requireSeen: true,
        pollMs: ldeps.pollMs || 1500,
        timeoutMs: opts.windowTimeoutMs || 300000,
      });
  if (w.windowUp) {
    health.recordStartOk(opts.healthFile);
    log(`Game process up after ${Math.round(w.waitedMs / 1000)} s.`);
  } else if (w.exited) {
    await killSteamHelpers(plat, extra);
    throw new Error(startFailure(opts.healthFile, health.NEXT_STEPS));
  } else
    log(
      `No game process after ${Math.round(w.waitedMs / 1000)} s - still waiting in the background (is Steam asking for a login or an update?).`,
    );
  return { name: pn.name, source: pn.source, windowUp: w.windowUp };
}

/**
 * Launch the game directly on Windows and wait for its window.
 * @param {object} options
 * @returns {Promise<object>}
 */
async function launchWindowsPlay({ opts, cfg, pn, server, log, progress }) {
  // skip the game's own Play launcher
  progress({
    phase: 'play-check',
    label: 'Starting the game',
    unit: 'none',
    done: 0,
    total: 0,
  });
  await run('reg', [
    'add',
    'HKCU\\Software\\Naughty Dog\\The Last of Us Part II',
    '/v',
    'ShowLauncher',
    '/t',
    'REG_DWORD',
    '/d',
    '0',
    '/f',
  ]);
  log(`Starting TLOU2 Twelve Weeks as '${pn.name}' on ${server} ...`);
  progress({
    phase: 'play-window',
    label: 'Starting the game (the first start after an update can take a minute)',
    unit: 'time',
    done: 0,
    total: 0,
  });
  const spawnedAt = Date.now();
  await spawnGameExe(cfg);
  // a cold start shows no window for a minute or more: wait for it (up to 3 min), with live progress; never end the process
  const w = await awaitGameStart(opts, progress, log, cfg.game_dir, spawnedAt);
  if (w.windowUp && !testHooks.spawn) watchModLoad({ gameDir: cfg.game_dir, spawnedAt, log });
  if (w.windowUp) {
    health.recordStartOk(opts.healthFile);
    log(
      w.modLoaded
        ? `The mod loaded after ${Math.round(w.waitedMs / 1000)} s (no game window seen yet).`
        : `Game window up after ${Math.round(w.waitedMs / 1000)} s.`,
    );
  } else if (w.exited) {
    await killSteamHelpers();
    throw new Error(startFailure(opts.healthFile, health.NEXT_STEPS));
  } else
    log(
      `No game window after ${Math.round(w.waitedMs / 1000)} s - still waiting in the background.`,
    );
  return { name: pn.name, source: pn.source, windowUp: w.windowUp };
}

// ---------------------------------------------------------------- the whole Play flow
// opts: { clientDir, cfg: {game_dir, token_id, custom_name}, server, skipCheck, reinstall, log(msg), userData (Linux: where the helper's prefix / log live),
//         platform (tests), linuxDeps (tests: { home, procDir, spawn, launch, pollMs }) }
/**
 * Run the full multiplayer Play flow.
 * @param {object} opts
 * @returns {Promise<object>}
 */
async function play(opts) {
  const { clientDir, cfg, server, log } = opts;
  const plat = opts.platform || process.platform;
  const lx = plat === 'linux';
  const ldeps = opts.linuxDeps || {};
  const extra = {
    platform: plat,
    userData: opts.userData,
    linuxDeps: opts.linuxDeps,
  };
  ctx.plat = plat;
  ctx.deps = ldeps;
  ctx.extra = extra;
  const progress = opts.progress || noProgress;
  if (!(await steamGate(plat)).running) throw new Error(STEAM_MSG); // defence in depth: the launcher UI blocks Play too
  if (!cfg.game_dir) throw new Error('The game folder is not set. Pick it in Settings > Game.');
  if (!exists(cfg.game_dir))
    throw new Error(
      `The game folder ${cfg.game_dir} does not exist any more (moved, renamed or uninstalled, or its drive is not connected). Pick the folder again in Settings > Game.`,
    );
  if (!isGameDir(cfg.game_dir))
    throw new Error(
      `tlou-ii.exe was not found in ${cfg.game_dir} (moved, renamed, or removed by an antivirus). Pick the game folder again in Settings > Game, or verify the game files in Steam.`,
    );
  if (lx && !opts.userData)
    throw new Error('Internal error: no data folder for the Linux Steam helper.');
  const eb = exeBuild(cfg.game_dir);
  if (eb.known === false) {
    log(
      `tlou-ii.exe build differs: timestamp ${eb.ts.toString(16)} size ${eb.size.toString(16)} (expected ${EXE_TIMESTAMP.toString(16)} ${EXE_IMAGE_SIZE.toString(16)})`,
    );
    if (!opts.skipCheck)
      throw new Error(
        'Your tlou-ii.exe is not the version this mod is built for (1.6.10721, the current Steam build). In Steam: right-click the game > Properties > Installed Files > Verify integrity of game files, do not use a modified tlou-ii.exe, then try again. (Tick "skip server check" to start anyway; the game will probably crash.)',
      );
  }
  await handleLeftoverProcesses(opts, log, progress);

  const dll = path.join(cfg.game_dir, 'winmm.dll');
  await prepareClientFiles({ clientDir, cfg, log, progress, opts, extra });
  progress({
    phase: 'play-check',
    label: 'Checking the server',
    unit: 'none',
    done: 0,
    total: 0,
  });
  const { ip, port } = await resolveServer(server);
  if (lx) prepareLinuxPrefix(cfg.game_dir, ldeps, log);
  const { helper, steamName } = await startPlayHelper({
    lx,
    plat,
    ldeps,
    clientDir,
    cfg,
    userData: opts.userData,
    ip,
    port,
    log,
    progress,
  });
  const { pn, version } = preparePlayerState({
    cfg,
    plat,
    steamName,
    progress,
    log,
    opts,
    server,
    ip,
    port,
    dll,
    clientDir,
  });
  await verifyPlayServer({
    opts,
    cfg,
    ip,
    port,
    pn,
    version,
    server,
    lx,
    plat,
    extra,
    log,
    progress,
  });
  await ensurePlayModFiles({
    clientDir,
    cfg,
    progress,
    log,
    opts,
    extra,
    lx,
    plat,
    dll,
    ip,
    port,
  });
  // The launcher owns presence from here: it shows an in-game status until the
  // mod claims the owner file, and clears immediately if the mod already did.
  presence.started(cfg.game_dir);
  let result;
  try {
    if (lx)
      result = await launchLinuxPlay({
        opts,
        cfg,
        ldeps,
        helper,
        pn,
        ip,
        port,
        server,
        plat,
        extra,
        log,
        progress,
      });
    else result = await launchWindowsPlay({ opts, cfg, pn, server, log, progress });
    if (result.windowUp) {
      // the server's address is never shown: only which of the two servers
      await presence.game({
        details: 'Playing Twelve Weeks',
        state: `${pn.name} on the ${cfg.server_target === 'test' ? 'testing' : 'live'} server`,
        timestamps: { start: Math.floor(Date.now() / 1000) },
      });
    }
    return result;
  } catch (e) {
    await presence.ended();
    throw e;
  }
}

// Start tlou-ii.exe from the game folder directly - the same exe and the same way for MP and PLAY SINGLEPLAYER (SP must not go
// through steam://: it opens what MP opens, only without the mod files). Records the pid as ours. Resolves once the process is spawned.
/**
 * Spawn the game executable for the configured client.
 * @param {object} cfg
 * @returns {import('child_process').ChildProcess}
 */
// (players saw "Steam SDK failed to initialize ... relaunch from the Steam Library" after starting the launcher
// through our "Add to Steam" shortcut, and from then on also from the launcher itself): Steam starts a non-Steam shortcut with its own SteamAppId /
// SteamGameId (the shortcut's id) and overlay variables in the environment. The launcher keeps that environment and every tlou-ii.exe it starts
// inherited it, so the game's SteamAPI_Init asked for the shortcut's app id instead of TLOU2's. The game gets the environment of a plain start.
const STEAM_ENV =
  /^(SteamAppId|SteamGameId|SteamOverlayGameId|SteamClientLaunch|SteamEnv|SteamNoOverlayUIDrawing|SteamStreamingSession|SteamTenfoot|SteamCompatAppId|STEAM_COMPAT_.*|EnableConfiguratorSupport|SDL_GAMECONTROLLER_IGNORE_DEVICES|SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD)$/i;
function gameEnv(env) {
  const out = {};
  for (const k of Object.keys(env || {})) if (!STEAM_ENV.test(k)) out[k] = env[k];
  return out;
}

function spawnGameExe(cfg) {
  const exe = path.join(cfg.game_dir, 'tlou-ii.exe');
  if (testHooks.spawn) return testHooks.spawn(exe, cfg.game_dir); // tests only
  return new Promise((resolve, reject) => {
    const c = spawn(exe, [], {
      cwd: cfg.game_dir,
      detached: true,
      stdio: 'ignore',
      env: gameEnv(process.env),
    });
    if (c.pid) startedPids.add(c.pid);
    c.once('error', (e) =>
      reject(
        new Error(
          e.code === 'ENOENT'
            ? `tlou-ii.exe disappeared from ${cfg.game_dir} just before it was started (moved, or removed by an antivirus). Pick the game folder again in Settings > Game.`
            : e.code === 'UNKNOWN' || /1223|EACCES|EPERM/.test(String(e.code || e.message))
              ? 'Windows stopped tlou-ii.exe from starting (an administrator / security prompt was closed). Untick "Run this program as an administrator" in tlou-ii.exe > Properties > Compatibility.'
              : e.message,
        ),
      ),
    );
    c.once('spawn', () => {
      c.unref();
      resolve();
    });
  });
}

module.exports = {
  playSingleplayer,
  modLoadedSince,
  modFilesSnapshot,
  watchModLoad,
  awaitGameStart,
  waitForGameWindow,
  play,
  spawnGameExe,
  gameEnv,
};
