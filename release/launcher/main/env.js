/**
 * @file Misc functions for getting and setting env vars
 * (persistent way of storing stuff that doesn't use the JSON file)
 */
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const install = require('./install');

/**
 * A function that returns a big JSON object containing data such as:
 * - OS
 * - Test mode
 * - Audio and screenshot support
 * - Launcher files and dirs
 * - Launcher metadata
 */
function prepareRuntime(app) {
  const isLinux = process.platform === 'linux';
  if (isLinux) app.commandLine.appendSwitch('no-sandbox');

  if (!app.isPackaged && !process.env.F2_USERDATA) {
    process.env.F2_NO_GAME = process.env.F2_NO_GAME || '1';
    process.env.F2_MUTE = '1';
    if (!process.env.F2_SHOT) {
      process.env.F2_USERDATA = path.join(os.tmpdir(), 'twelve-weeks-launcher-dev');
    }
  }

  const migrationLog = [];
  if (process.env.F2_USERDATA) app.setPath('userData', process.env.F2_USERDATA);
  else if (process.env.F2_SHOT || process.env.F2_NO_GAME) {
    app.setPath('userData', path.join(os.tmpdir(), 'twelve-weeks-launcher-test'));
  } else if (app.isPackaged) {
    // pinned to ROOT_NAME (the shim's app name used to differ); the folder from before the product rename moves here on the first start
    const ud = install.migrateUserData({
      appData: app.getPath('appData'),
      log: (m) => migrationLog.push(m),
    });
    app.setPath('userData', ud.dir);
  }

  const userData = app.getPath('userData');
  const isTest = !!(process.env.F2_SHOT || process.env.F2_NO_GAME || process.env.F2_USERDATA);
  const tmpDir = os.tmpdir();
  const installed = process.platform === 'win32' && app.isPackaged && install.isInstalledRun();

  let buildDate = '';
  try {
    const entry = path.join(__dirname, '..', 'main.js');
    buildDate = fs.statSync(entry).mtime.toISOString().slice(0, 10);
  } catch {
    /* no build date */
  }

  return {
    isLinux,
    isTest,
    isScreenshot: !!process.env.F2_SHOT,
    muted: process.env.F2_MUTE === '1' || isTest,
    hasUserDataEnv: !!process.env.F2_USERDATA,
    userData,
    logDir: path.join(userData, 'logs'),
    cfgFile: path.join(userData, 'launcher.json'),
    tmpDir,
    tmpDirLower: tmpDir.toLowerCase(),
    // the payload's own package.json: under the fixed-root shim app.getVersion() is the shim's
    launcherVersion: require('../package.json').version,
    installRoot: installed ? install.installRoot() : null,
    migrationLog, // (written to the log once the logger exists)
    buildDate,
  };
}

/**
 * Check whether a path is outside the launcher temp area.
 * @param {string} p
 * @returns {boolean}
 */
function isOutsideTemp(dir, tmpDir) {
  if (!dir) return true;
  const resolved = path.resolve(dir).toLowerCase();
  const temp = path.resolve(tmpDir).toLowerCase() + path.sep;
  return !resolved.startsWith(temp);
}

module.exports = { prepareRuntime, isOutsideTemp };
