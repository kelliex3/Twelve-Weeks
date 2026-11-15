/**
 * @file The main orchestrator and event loop for the launcher
 */
'use strict';

const fs = require('fs');
const path = require('path');
const { app, nativeTheme, shell, BrowserWindow } = require('electron');
const install = require('./main/install');
const { prepareRuntime } = require('./main/env');
const { createLogger } = require('./main/log');
const { createConfigStore } = require('./main/config');
const { createLocale } = require('./main/locale');
const { createPaths } = require('./main/paths');
const { createMode } = require('./main/mode');
const { createPlayer } = require('./main/player');
const { createJobs } = require('./main/jobs');
const { createBanGuard } = require('./lib/ban');
const hwid = require('./lib/hwid');
const upd = require('./lib/update');
const { createReport } = require('./main/report');
const { createStatus } = require('./main/status');
const { createClan } = require('./main/clan');
const { createSocial } = require('./main/social');
const { createMainWindow } = require('./main/window');
const { registerIpc } = require('./main/ipc');
const { recoverInterruptedWork, topUpSingleplayer, healClientDir } = require('./main/startup');
// Discord Rich Presence (OmegaMan): lib/rpc/* - the adapter is mounted once in start(), presence.js decides when the launcher may show an activity
const { createRpcService } = require('./lib/rpc/index');
const rpcService = require('./lib/rpc/service');
const presence = require('./lib/rpc/presence');

// One fixed AppUserModelID so a taskbar pin / Start tile follows the app across updates (matches the installer's shortcuts).
if (process.platform === 'win32') app.setAppUserModelId(install.AUMID);

const runtime = prepareRuntime(app);

let mainWindow = null;
const send = (channel, data) => {
  if (mainWindow && !mainWindow.isDestroyed()) mainWindow.webContents.send(channel, data);
};
const getWindow = () => mainWindow;

const log = createLogger({ logDir: runtime.logDir, send });
for (const m of runtime.migrationLog || []) log(m);
const config = createConfigStore({ runtime, log });
const locale = createLocale({ app, config, rootDir: __dirname });
const paths = createPaths({ app, runtime, config });
const mode = createMode({ config, log });
const player = createPlayer({ config, runtime, mode, log });
// banned players cannot use the launcher (lib/ban.js): checked at start and every 10 min, the verdict is stored locally
const ban = createBanGuard({
  file: path.join(runtime.userData, 'ban.json'),
  getHost: () => paths.fileHost(),
  getHashes: () => hwid.hardwareHashes(),
  getSteamIds: () => player.readSteamIds(),
  getJson: upd.getJson,
  log,
});
const jobs = createJobs({ runtime, config, paths, log, send, getWindow, mode, locale, ban });
const report = createReport({
  app,
  runtime,
  config,
  paths,
  log,
  send,
  getWindow,
  player,
  locale,
});
const status = createStatus({
  runtime,
  config,
  paths,
  player,
  report,
  jobs,
  mode,
  locale,
  log,
});
const clan = createClan({ config, paths, player, locale, userData: runtime.userData });
const social = createSocial({ config, paths, player, locale });

async function start() {
  config.load();
  config.save(); // persists a freshly generated player id at once (the clan is keyed by it)
  mode.sync();
  rpcService.mount(createRpcService(config, log, player));
  presence.init(config);
  registerIpc({
    app,
    runtime,
    config,
    paths,
    jobs,
    mode,
    player,
    status,
    clan,
    social,
    report,
    locale,
    log,
    send,
    getWindow,
    ban,
  });
  mainWindow = createMainWindow({ rootDir: __dirname, config, jobs, runtime, locale });

  if (!runtime.isScreenshot) {
    ban.check().catch(() => {});
    setInterval(() => ban.check().catch(() => {}), 10 * 60 * 1000).unref();
  }
  recoverInterruptedWork({ config, log });
  healClientDir({ config, log });
  topUpSingleplayer({ config, log });
  log(
    `Launcher ${runtime.launcherVersion} started${runtime.installRoot ? ' (fixed root ' + runtime.installRoot + ')' : ''}`,
  );
  if (runtime.installRoot) {
    // links and pins that still start the install from before the product rename (a bootstrap that could not reach them) are retargeted
    install.migrateShortcuts({
      root: runtime.installRoot,
      legacyRoot: install.legacyInstallRoot(),
      readShortcut,
      writeShortcut: (file, opts) => {
        if (!shell.writeShortcutLink(file, 'replace', opts))
          throw new Error('writeShortcutLink failed: ' + file);
      },
      log,
    });
    install.refreshIcon({
      root: runtime.installRoot,
      iconSrc: path.join(__dirname, 'assets', 'icon.ico'),
      writeShortcut: (file, opts) => {
        if (!shell.writeShortcutLink(file, 'replace', opts))
          throw new Error('writeShortcutLink failed: ' + file);
      },
      log,
      refreshShell: () => {
        try {
          require('child_process')
            .spawn('ie4uinit.exe', ['-show'], { detached: true, stdio: 'ignore' })
            .unref();
        } catch {
          /* the new links already point to a new icon path */
        }
      },
    });
    setTimeout(() => {
      install.prune(runtime.installRoot);
      install.removeLegacyRoot({ root: runtime.installRoot, legacyRoot: install.legacyInstallRoot(), log });
    }, 20000);
  }
  // the main menu's presence (the renderer re-sends it per screen); never awaited, a missing Discord must not hold the start
  if (!runtime.isScreenshot)
    presence.menu({ details: 'In main menu', state: 'Ready' }).catch((e) => log(`RPC sample activity failed: ${e.message}`));

  if (runtime.isScreenshot) {
    require('./dev/screenshots')
      .runScreenshots({ app, window: mainWindow, config, log, locale })
      .catch((e) => {
        log('SHOT ERROR ' + (e && e.message));
        app.quit();
      });
  }
}

nativeTheme.themeSource = 'dark';

/** Target and AppUserModelID of a .lnk, or null (Windows only). */
function readShortcut(file) {
  try {
    return shell.readShortcutLink(file);
  } catch {
    return null;
  }
}

/**
 * A portable exe (first run after the old layout, or a freshly downloaded update) installs itself into the fixed root and hands over.
 * Resolves true when the installed copy was started (this process quits), false to carry on as before.
 */
let bootstrapping = false;
async function bootstrapIntoRoot() {
  bootstrapping = true;
  const root = install.installRoot();
  let splash = null;
  try {
    splash = new BrowserWindow({
      width: 420,
      height: 120,
      frame: false,
      resizable: false,
      backgroundColor: '#070808',
      icon: path.join(__dirname, 'assets', 'icon.png'),
      webPreferences: { sandbox: true },
    });
    splash.loadURL(
      'data:text/html;charset=utf-8,' +
        encodeURIComponent(
          '<body style="margin:0;background:#070808;color:#e9dcc3;font:600 15px Segoe UI,sans-serif;display:flex;align-items:center;justify-content:center;height:100vh;letter-spacing:.08em">Installing the launcher...</body>',
        ),
    );
    const exe = install.bootstrap({
      root,
      legacyRoot: install.legacyInstallRoot(),
      readShortcut,
      version: app.getVersion(),
      execPath: process.execPath,
      resourcesPath: process.resourcesPath,
      env: process.env,
      writeShortcut: (file, opts) => {
        fs.mkdirSync(path.dirname(file), { recursive: true });
        if (!shell.writeShortcutLink(file, 'replace', opts))
          throw new Error('writeShortcutLink failed: ' + file);
      },
      log: (m) => log(m),
    });
    // an older launcher that triggered this update may still be quitting: wait for the single-instance lock, then hand over
    for (let i = 0; i < 40 && !app.requestSingleInstanceLock(); i++)
      await new Promise((r) => setTimeout(r, 500));
    app.releaseSingleInstanceLock();
    install.launchInstalled(exe);
    return true;
  } catch (e) {
    log(`install: failed, running this copy as before: ${(e && e.stack) || e}`);
    return false;
  } finally {
    if (splash && !splash.isDestroyed()) splash.destroy();
    bootstrapping = false;
  }
}

function startNormally() {
  if (!app.requestSingleInstanceLock()) return app.quit();
  app.on('second-instance', () => {
    if (!mainWindow) return;
    if (mainWindow.isMinimized()) mainWindow.restore();
    mainWindow.focus();
  });
  return start();
}

{
  app
    .whenReady()
    .then(async () => {
      if (install.wantsBootstrap() && (await bootstrapIntoRoot())) return app.quit();
      return startNormally();
    })
    .catch((e) => {
      log(`STARTUP FAILED: ${(e && e.stack) || e}`);
      app.quit();
    });

  app.on('window-all-closed', () => {
    if (!bootstrapping) app.quit();
  });
  app.on('render-process-gone', (_event, _webContents, details) =>
    log(`RENDERER GONE: ${details.reason} (exit ${details.exitCode})`),
  );
  app.on('child-process-gone', (_event, details) =>
    log(`CHILD PROCESS GONE: ${details.type} ${details.reason} (exit ${details.exitCode})`),
  );
  process.on('uncaughtException', (e) => {
    log(`UNCAUGHT: ${(e && e.stack) || e}`);
  });
  let quitting = false;
  app.on('before-quit', async (event) => {
    if (quitting) return;
    quitting = true;
    log(`Launcher quitting${jobs.isBusy ? ` (busy: ${jobs.kind})` : ''}`);
    jobs.shutdown();
    // Electron does not await async quit handlers: hold the quit until the RPC pipe closes.
    event.preventDefault();
    try {
      presence.stop();
      // capped: a hung Discord pipe must never keep the launcher from closing
      await Promise.race([rpcService.destroy().catch(() => {}), new Promise((r) => setTimeout(r, 1500))]);
    } finally {
      app.quit();
    }
  });
}
