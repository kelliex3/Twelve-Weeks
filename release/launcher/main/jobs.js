/**
 * @file Long-running job controller for update, Play, SP/MP and self-update.
 *
 * Each job kind is its own async function (runUpdate, runPlay, runPlaySp, switchMode,
 * selfUpdate); they all enter through `withJob(kind, fn)`, which owns the busy guard, the
 * AbortController and the busy/release events. New job kinds only need their own function.
 */
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');
const { app, dialog, shell } = require('electron');
const game = require('../lib/game');
const upd = require('../lib/update');
const mapsLib = require('../lib/maps');
const mapsRemote = require('../lib/maps-remote');
const mapsConvert = require('../lib/maps-convert');
const linux = require('../lib/linux');
const installer = require('./install');

/**
 * Create the controller for all long-running launcher jobs.
 * @param {{ runtime: object, config: object, paths: object, log: Function, send: Function, getWindow: Function, mode: object, locale: object }} options
 * @returns {object} Job API used by IPC and the renderer.
 */
function createJobs({ runtime, config, paths, log, send, getWindow, mode, locale, ban }) {
  const I18N = locale.I18N;
  let kind = null;
  let abort = null;
  let slimChild = null;

  function guard(nextKind) {
    if (kind) throw new Error(`Busy (${kind}). Wait for it to finish.`);
    kind = nextKind;
    abort = new AbortController();
    send('busy', kind);
  }

  function release() {
    kind = null;
    abort = null;
    send('busy', null);
  }

  /**
   * Run one job kind under the shared busy guard.
   * @param {string} nextKind
   * @param {Function} fn
   * @returns {Promise<*>}
   */
  async function withJob(nextKind, fn) {
    guard(nextKind);
    try {
      return await fn();
    } finally {
      release();
    }
  }

  const confirmEndDialog = async (pids) =>
    (
      await dialog.showMessageBox(getWindow(), {
        type: 'question',
        buttons: [I18N.T('End it'), I18N.T('Leave it')],
        defaultId: 1,
        cancelId: 1,
        title: 'Twelve Weeks',
        message: I18N.T('A game process from an earlier start is still running without a window.'),
        detail: I18N.T(
          'tlou-ii.exe (pid {pids}) has had no window for more than 90 seconds, so it is probably stuck. End it and start the game?',
          { pids: pids.join(', ') },
        ),
      })
    ).response === 0;

  async function runUpdate({ full = false } = {}) {
    if (ban) ban.assertNotBanned();
    upd.resetDown();
    return withJob('update', async () => {
      try {
        const host = paths.fileHost();
        if (!host) throw new Error('No file host configured (Settings > Files).');
        await upd.refuseIfGameRunning(game.gameStatus, {
          confirmEnd: confirmEndDialog,
          procs: game.psGameProcs,
          ours: game.startedPids,
          kill: game.killProc,
          log,
        });
        const dir = paths.clientDir();
        fs.mkdirSync(dir, { recursive: true });
        send('progress', { phase: 'check', done: 0, total: 1 });
        const recovered = await upd.recoverStaged(dir);
        if (recovered.pending)
          log(
            `Earlier update was not finished: ${
              recovered.complete
                ? 'completed it'
                : `${recovered.missing.length} file(s) are still missing, downloading them`
            }.`,
          );
        let manifest;
        try {
          manifest = await paths.getManifest(host, 3);
        } catch (e) {
          throw upd.explainError(e);
        }
        const cache = path.join(runtime.userData, 'hashcache.json');
        let list;
        if (full) {
          send('progress', { phase: 'verify', done: 0, total: 1 });
          list = await upd.diffFiles(manifest, dir, cache, {
            full: true,
            onProgress: (p) => send('progress', p),
          });
        } else list = await upd.diffFiles(manifest, dir, cache);
        log(
          `${full ? 'Verify' : 'Update'}: ${list.length} file(s) to download (${list.reduce(
            (a, f) => a + f.size,
            0,
          )} bytes), manifest ${manifest.version}`,
        );
        if (list.length) {
          await upd.downloadAll(list, dir, {
            signal: abort.signal,
            stage: true,
            onProgress: (p) => send('progress', { phase: 'download', ...p }),
          });
          await upd.applyStaged(list, dir, manifest);
        }
        await upd.sweepStale(manifest, dir);
        await upd.clearJournal(dir);
        await upd.pruneOld(manifest, dir, path.join(dir, '.t2r-files.json'));
        try {
          fs.writeFileSync(cache, '{}');
        } catch {
          /* cache is optional */
        }
        log(`Client is up to date (${manifest.version}).`);
        return { ok: true, version: manifest.version, downloaded: list.length };
      } catch (e) {
        const explained = upd.explainError(e, e && e.path);
        if (explained !== e) log(`Update failed: ${e && e.message}`);
        throw explained;
      }
    });
  }

  /**
   * MAPS tab, TEMPORARY: download the prebuilt Part I map pack from the update server into <game dir>	2r-maps\.
   * @returns {Promise<object>}
   */
  async function refuseIfGameUp() {
    let s = null;
    try {
      s = await game.gameStatus();
    } catch {
      return;
    }
    if (s && (s.running || s.starting))
      throw new Error('The game is running. Close it first: it uses these map files.');
  }

  async function runMapsDownload() {
    return withJob('maps', async () => {
      await refuseIfGameUp();
      const host = paths.fileHost();
      if (!host) throw new Error('No file host configured (Settings > Files).');
      if (!game.isGameDir(config.values.game_dir))
        throw new Error('Game folder not set. Open Settings > Game.');
      if (config.writesBlocked())
        throw new Error(
          'Test mode: map files are only written to a game folder inside the temp dir.',
        );
      send('progress', { phase: 'check', done: 0, total: 1 });
      try {
        const r = await mapsRemote.downloadRemotePack(host, config.values.game_dir, {
          signal: abort.signal,
          onProgress: (p) =>
            send('progress', {
              phase: 'play-maps',
              label: 'Downloading Part I maps',
              unit: 'bytes',
              ...p,
            }),
        });
        log(
          `Part I maps: ${r.pack} ready (${r.downloaded} of ${r.files} files downloaded, ${r.bytes} bytes).`,
        );
        return { ...r };
      } catch (e) {
        const explained = upd.explainError(e, e && e.path);
        if (explained !== e) log(`Part I maps download failed: ${e && e.message}`);
        throw explained;
      }
    });
  }

  /**
   * MAPS tab: convert the Part I install with p1conv into <game dir>	2r-maps\part1\ (the permanent route).
   * @returns {Promise<object>}
   */
  async function runMapsConvert() {
    return withJob('maps', async () => {
      await refuseIfGameUp();
      if (!game.isGameDir(config.values.game_dir))
        throw new Error('Game folder not set. Open Settings > Game.');
      if (config.writesBlocked())
        throw new Error(
          'Test mode: map files are only written to a game folder inside the temp dir.',
        );
      const exe = mapsConvert.converterPath(runtime.userData);
      if (!exe) throw new Error('The Part I converter is not installed.');
      send('progress', { phase: 'check', done: 0, total: 1 });
      const r = await mapsConvert.convertFromInstall({
        exe,
        part1Dir: config.values.tlou1_dir,
        gameDir: config.values.game_dir,
        signal: abort.signal,
        onProgress: (p) =>
          send('progress', {
            phase: 'play-maps',
            label: 'Converting Part I maps',
            unit: 'pct',
            ...p,
          }),
      });
      log(`Part I maps: converted ${r.files} files (${r.bytes} bytes).`);
      return { ...r };
    });
  }

  async function runPlay(opts = {}) {
    if (ban) ban.assertNotBanned();
    if (process.env.F2_SHOT || process.env.F2_NO_GAME)
      throw new Error('Play is disabled in test / screenshot mode.');
    if (config.writesBlocked())
      throw new Error(
        'Test mode: Play only runs against a game folder inside the temp dir (never the real game).',
      );
    return withJob('play', async () => {
      if (mode.current())
        throw new Error(
          'The launcher is in single-player mode (the mod is switched off). Press SWITCH TO MP first.',
        );
      if (!game.isGameDir(config.values.game_dir))
        throw new Error('Game folder not set. Open Settings > Game.');
      config.adoptIdentity();
      const server = paths.serverAddress();
      if (!server)
        throw new Error(
          'No server address. Set it in Settings > Game (or ship server.txt in the client files).',
        );
      const result = await game.play({
        clientDir: paths.clientDir(),
        cfg: config.values,
        server,
        debugMode: !!config.values.debug_mode,
        skipCheck: !!opts.skipCheck,
        reinstall: !!opts.reinstall,
        log,
        userData: runtime.userData,
        healthFile: path.join(runtime.userData, 'play-health.json'),
        progress: (p) => send('progress', p),
        onChild: (child) => {
          slimChild = child;
          child.once('close', () => {
            if (slimChild === child) slimChild = null;
          });
        },
        confirmEnd: confirmEndDialog,
      });
      try {
        return { ok: true, ...result };
      } finally {
        slimChild = null;
      }
    });
  }

  async function runPlaySp() {
    if (ban) ban.assertNotBanned();
    if (process.env.F2_SHOT || process.env.F2_NO_GAME)
      throw new Error('Play is disabled in test / screenshot mode.');
    if (config.writesBlocked())
      throw new Error(
        'Test mode: Play only runs against a game folder inside the temp dir (never the real game).',
      );
    return withJob('play', async () => {
      if (!mode.current())
        throw new Error('The launcher is in multiplayer mode. Press SWITCH TO SP first.');
      const result = await game.playSingleplayer({
        cfg: config.values,
        log,
        progress: (p) => send('progress', p),
        openUrl: async (url) => {
          if (runtime.isLinux) {
            const command = linux.steamLaunchCommand();
            spawn(command.cmd, command.args, { detached: true, stdio: 'ignore' }).unref();
          } else await shell.openExternal(url);
        },
      });
      return { ok: true, ...result };
    });
  }

  async function switchMode(to) {
    if (process.env.F2_SHOT || process.env.F2_NO_GAME)
      throw new Error('Switching is disabled in test / screenshot mode.');
    if (config.writesBlocked())
      throw new Error(
        'Test mode: switching only runs against a game folder inside the temp dir (never the real game).',
      );
    if (!game.isGameDir(config.values.game_dir))
      throw new Error('Game folder not set. Open Settings > Game.');
    return withJob('sp', async () => {
      let result;
      if (to === 'sp') {
        result = await game.spDisable(config.values.game_dir);
        config.values.sp_mode = true;
      } else {
        result = await game.spEnable(config.values.game_dir);
        config.values.sp_mode = false;
      }
      config.save();
      log(
        `SWITCH TO ${to.toUpperCase()}: ${
          result.already
            ? 'already done'
            : `${result.moved} mod entries moved${
                to === 'sp'
                  ? ' to ' + game.spmode.disabledDir(config.values.game_dir)
                  : ' back into the game folder'
              }`
        }${result.needsRepair ? ' (winmm.dll damaged: repair needed)' : ''}`,
      );
      return {
        mode: to,
        ...result,
        folder: game.spmode.disabledDir(config.values.game_dir),
      };
    });
  }

  async function selfUpdate() {
    upd.resetDown();
    const host = paths.fileHost();
    const manifest = await paths.getManifest(host);
    const launcher = runtime.isLinux ? manifest.launcher_linux : manifest.launcher;
    if (!launcher || !launcher.url) throw new Error('The server lists no launcher build.');
    if (
      runtime.isLinux
        ? !process.env.APPIMAGE
        : !process.env.PORTABLE_EXECUTABLE_FILE && !runtime.installRoot
    )
      throw new Error(
        runtime.isLinux
          ? 'Self-update only works from the AppImage.'
          : 'Self-update only works from the packaged portable exe.',
      );
    return withJob('update', async () => {
      if (!runtime.isLinux && !runtime.installRoot)
        upd.assertWritableNextTo(process.env.PORTABLE_EXECUTABLE_FILE);
      send('progress', {
        phase: 'launcher-swap',
        label: 'Updating the launcher',
        unit: 'none',
        done: 0,
        total: 0,
      });
      const tmp = path.join(os.tmpdir(), 'twelve-weeks-launcher-update');
      fs.mkdirSync(tmp, { recursive: true });
      const downloaded = await upd.downloadLauncher(
        launcher,
        host,
        tmp,
        (p) => send('progress', { phase: 'launcher', ...p }),
        abort.signal,
      );
      if (runtime.isLinux) {
        const target = process.env.APPIMAGE;
        fs.chmodSync(downloaded, 0o755);
        try {
          fs.renameSync(downloaded, target);
        } catch {
          fs.copyFileSync(downloaded, target + '.new');
          fs.chmodSync(target + '.new', 0o755);
          fs.renameSync(target + '.new', target);
        }
        spawn(target, [], { detached: true, stdio: 'ignore' }).unref();
        setTimeout(() => app.quit(), 300);
        return { ok: true };
      }
      if (runtime.installRoot) {
        // fixed-root layout: nothing of ours is replaced. The downloaded build (a portable exe) installs itself as
        // app-<version> next to the others and starts the SAME exe path again, so shortcuts and the taskbar pin keep working.
        const staged = path.join(tmp, 'TwelveWeeks-Launcher-update.exe');
        fs.renameSync(downloaded, staged);
        spawn(staged, [], {
          detached: true,
          stdio: 'ignore',
          env: installer.cleanEnv(),
          cwd: tmp,
        }).unref();
        log(
          `Launcher self-update: handing over to ${manifest.launcher.version || '?'} (fixed root)`,
        );
        setTimeout(() => app.quit(), 600);
        return { ok: true };
      }
      const target = process.env.PORTABLE_EXECUTABLE_FILE;
      const bat = path.join(tmp, 'swap.bat');
      fs.writeFileSync(
        bat,
        `@echo off\r\nchcp 65001 >nul\r\nset n=0\r\n:w\r\nset /a n+=1\r\ncopy /y "${downloaded}" "${target}.new" >nul 2>nul && move /y "${target}.new" "${target}" >nul 2>nul && goto ok\r\nif %n% GEQ 30 goto fail\r\nping -n 2 127.0.0.1 >nul\r\ngoto w\r\n:ok\r\nstart "" "${target}"\r\ndel "${downloaded}" >nul 2>nul\r\nexit /b 0\r\n:fail\r\nstart "" "${downloaded}"\r\n`,
      );
      spawn('cmd.exe', ['/c', bat], {
        detached: true,
        stdio: 'ignore',
        windowsHide: true,
      }).unref();
      log(`Launcher self-update: restarting into ${manifest.launcher.version || '?'}`);
      setTimeout(() => app.quit(), 600);
      return { ok: true };
    });
  }

  function cancel() {
    if (abort) abort.abort();
    return { ok: true };
  }

  function shouldConfirmClose() {
    return !!kind && !(kind === 'update' && !abort) && !runtime.isScreenshot;
  }

  function shutdown() {
    if (slimChild && !slimChild.killed) {
      try {
        slimChild.kill();
      } catch {
        /* already gone */
      }
    }
  }

  return {
    get isBusy() {
      return kind !== null;
    },
    get kind() {
      return kind;
    },
    runUpdate,
    runMapsDownload,
    runMapsConvert,
    runPlay,
    runPlaySp,
    switchMode,
    selfUpdate,
    cancel,
    shouldConfirmClose,
    shutdown,
  };
}

module.exports = { createJobs };
