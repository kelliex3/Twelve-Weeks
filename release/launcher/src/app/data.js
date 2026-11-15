/**
 * @file Renderer state rendering, polling and launcher-update flow.
 *
 * Layout: pure view functions at the top (`set`, `render*`, progress overlay), then the polling
 * loops, then the side-effectful launcher-update flow. Helpers keep read-only rendering separate
 * from network/IPC side effects even though they share this module.
 */
'use strict';

/**
 * Persist a configuration patch and update the local snapshot.
 * @param {object} patch
 * @returns {Promise<void>}
 */
async function save(patch) {
  const r = await f2.setConfig(patch);
  if (r.ok) Object.assign(cfg.resolved, r.resolved);
  Object.assign(
    cfg,
    patch.effects ? { ...patch, effects: { ...cfg.effects, ...patch.effects } } : patch,
  );
}

// ---------------------------------------------------------------- state rendering
/**
 * Set an element's translated text and class.
 * @param {HTMLElement} el
 * @param {string} text
 * @param {string} [cls]
 * @returns {void}
 */
function set(el, text, cls) {
  el.textContent = T(text);
  el.className = cls || '';
  // STATUS panel: a collapsible row (.x) that is in a problem state stays visible while the panel is collapsed
  const row = el.closest('.rowbar.x');
  if (row) row.classList.toggle('prob', cls === 'bad' || cls === 'warn');
}
/**
 * Refresh the player-name label from the resolved identity.
 * @returns {Promise<void>}
 */
async function updateName() {
  // the name the game will use + where it comes from (same priority as the Play flow)
  const r = await f2.playerName();
  if (!r.ok) return;
  set($('pl-name'), r.name, '');
  $('pl-line').textContent = T(r.source);
}
/**
 * Render the client/game/update state cards.
 * @returns {void}
 */
function renderState() {
  const s = state;
  if (!s) return;
  if (s.offline && !s.installed) {
    set($('cl-state'), 'NOT INSTALLED', 'bad');
    $('cl-line').textContent = T(s.error || 'update server unreachable');
  } else if (s.offline) {
    set($('cl-state'), 'OFFLINE', 'warn');
    $('cl-line').textContent =
      T('update server unreachable') +
      (s.localVersion ? T(', build {v} installed', { v: s.localVersion }) : '');
  } else if (s.toDownload.files) {
    set($('cl-state'), 'UPDATE AVAILABLE', 'warn');
    $('cl-line').textContent = T('{size} to download, build {v}', {
      size: fmtBytes(s.toDownload.bytes),
      v: s.manifest.version,
    });
  } else {
    set($('cl-state'), 'UP TO DATE', 'ok');
    $('cl-line').textContent = T('build {v}', {
      v: s.manifest ? s.manifest.version : s.localVersion,
    });
  }
  updateName();
  set($('gm-state'), s.gameOk ? 'FOUND' : 'NOT FOUND', s.gameOk ? 'ok' : 'bad');
  $('gm-line').textContent = s.gameOk ? s.gameDir : T('set it in Settings > Game');
  $('f-todl').textContent = s.manifest
    ? s.toDownload.files
      ? T('{size} ({n} files)', {
          size: fmtBytes(s.toDownload.bytes),
          n: s.toDownload.files,
        })
      : T('nothing')
    : '--';
  $('f-local').textContent = s.localVersion || '--';
  $('f-latest').textContent = s.manifest ? s.manifest.version : '--';
  $('b-launcher-update').disabled = !s.launcherUpdate;
  $('launcher-msg').textContent = s.launcherUpdate
    ? T('Version {v} is available.', { v: s.launcherUpdate.version })
    : T('Launcher is up to date.');
  renderButtons();
}
/**
 * Render the server status card.
 * @returns {void}
 */
function renderStatus() {
  const s = status || {};
  $('online').classList.toggle('up', !!s.online);
  $('on-n').textContent = s.online && s.players != null ? s.players : '\u2014';
  $('ph-count').textContent =
    s.online && s.serverVersion ? T('BUILD {v}', { v: s.serverVersion }) : '';
  if (s.online) {
    set($('srv-state'), 'ONLINE', 'ok');
    $('srv-line').textContent =
      (s.players != null ? T('{n} players, ', { n: s.players }) : '') +
      T('build {v}', { v: s.serverVersion || '?' });
  } else if (cfg.resolved.server && s.reachable === false) {
    set($('srv-state'), 'OFFLINE', 'bad');
    $('srv-line').textContent = cfg.resolved.server;
  } else if (!cfg.resolved.server) {
    set($('srv-state'), 'NOT SET', 'warn');
    $('srv-line').textContent = T('add a server in Settings');
  }
}
/**
 * Load and render the clan summary when not in SP mode.
 * @returns {Promise<void>}
 */
async function loadClanSummary() {
  if (sp) return;
  const r = await f2.clan();
  if (sp) return;
  Clan.summary(r.ok ? r.clan : null);
}
// Steam gate (Windows): Play is blocked while Steam is not running; re-checked every few seconds (a failed probe never blocks, see lib/game.js)
// ---- SWITCH TO SP / MP (lib/spmode.js): the mod's files move to <game>\TwelveWeeks-disabled and back. In SP the clan / server side is hidden, the
// backgrounds and the music are the neutral ones (the same as before a clan is known) and PLAY starts the plain tlou-ii.exe (the same exe as MP, without the mod files).
let sp = false;
/**
 * Apply the single-player/multiplayer UI mode.
 * @param {boolean} on
 * @returns {void}
 */
function setSp(on) {
  const changed = on !== sp;
  sp = !!on;
  if (changed) Mus.switchNow(); // SP <-> MP: the music set changes at once, not after the running track ends
  document.body.classList.toggle('sp-mode', sp);
  $('mi-switch-t').textContent = T(sp ? 'SWITCH TO MP' : 'SWITCH TO SP');
  if (sp) {
    if (curView === 'clan' || curView === 'loadout' || curView === 'friends') showView(null);
    Clan.summary(null); // no clan, no faction: the neutral theme, backgrounds and music (Scenes.setFaction + Mus.setTheme)
    applyLook();
  } else if (changed) loadClanSummary(); // the clan's own imagery and music come back
  renderButtons();
}
$('mi-switch').onclick = async () => {
  if (busy || gs.s === 'starting' || gs.s === 'running') {
    denied(
      'SOMETHING WENT WRONG',
      busy
        ? 'Please wait.'
        : 'The game is running (or still starting). Close it first, then switch.',
    );
    return;
  }
  const to = sp ? 'mp' : 'sp';
  const r = await f2.spSwitch(to);
  if (!r.ok) {
    toast(r.error);
    await loadSpStatus();
    return;
  }
  setSp(to === 'sp');
  if (to === 'sp')
    toast(
      r.moved || r.already
        ? T('The mod was moved to {folder}. The game now starts without it.', {
            folder: r.folder,
          })
        : T(
            'The mod is not in the game folder, so there was nothing to move. Single-player mode is on.',
          ),
      true,
      T('SINGLE PLAYER'),
    );
  else
    toast(
      r.needsRepair
        ? T(
            'The mod files are back in the game folder, but winmm.dll was damaged and was set aside. The next PLAY reinstalls it.',
          )
        : T(
            'The mod files are back in the game folder. The next PLAY checks them and repairs anything that is missing.',
          ),
      true,
      T('MULTIPLAYER'),
    );
  refresh();
};
/**
 * Read the persisted SP/MP mode and apply it.
 * @returns {Promise<void>}
 */
async function loadSpStatus() {
  try {
    const r = await f2.spStatus();
    if (r.ok) setSp(r.sp);
  } catch {}
}
let steamDown = false;
/**
 * Render the Steam gate status.
 * @returns {void}
 */
function renderSteam() {
  if (steamDown) {
    set($('st-state'), 'NOT RUNNING', 'bad');
    $('st-line').textContent = T('Start Steam first - Twelve Weeks needs Steam running');
  } else {
    set($('st-state'), 'RUNNING', 'ok');
    $('st-line').textContent = '';
  }
}
/**
 * Poll the Steam gate and update the UI when it changes.
 * @returns {Promise<void>}
 */
async function pollSteam() {
  if (busy === 'play' || gs.s === 'running' || gs.s === 'starting') return; // the game is up: Steam obviously is
  let down = false;
  try {
    const r = await f2.steam();
    down = !!(r && r.ok !== false && r.required && !r.running);
  } catch {} // an error never blocks
  if (down !== steamDown) {
    steamDown = down;
    renderSteam();
    renderButtons();
  }
}
/**
 * Resolve the Play button label and enabled state.
 * @param {object} v
 * @returns {{label: string, disabled: boolean}}
 */
function playButtonView({
  busy,
  gameState,
  steamDown,
  gameOk,
  sp,
  server,
  installed,
  offline,
  need,
}) {
  if (busy === 'play' || gameState === 'preparing') return { label: 'STARTING', disabled: true };
  if (busy) return { label: 'PLEASE WAIT', disabled: true };
  if (steamDown && gameState === 'idle' && gameOk && (sp || server))
    return { label: 'START STEAM', disabled: true };
  if (gameState === 'starting') return { label: 'STARTING', disabled: true };
  if (gameState === 'running') return { label: 'IN GAME', disabled: true };
  if (!gameOk) return { label: 'SET GAME FOLDER', disabled: false };
  if (sp) return { label: 'PLAY SINGLEPLAYER', disabled: false }; // the plain tlou-ii.exe: no server, no client files needed
  if (!server) return { label: 'SET SERVER', disabled: false };
  if (!installed && offline) return { label: 'RETRY', disabled: false };
  if (need) return { label: 'UPDATE & PLAY', disabled: false };
  return { label: 'PLAY', disabled: false };
}

/**
 * Update Play/update button labels and disabled state.
 * @returns {void}
 */
function renderButtons() {
  const s = state || {},
    t = $('mi-play'),
    b = t.parentElement,
    u = $('mi-update');
  const need = s.toDownload && s.toDownload.files > 0;
  u.hidden = !need || sp;
  $('mi-update-sz').textContent = need ? fmtBytes(s.toDownload.bytes) : '';
  u.disabled = !!busy;
  const view = playButtonView({
    busy,
    gameState: gs.s,
    steamDown,
    gameOk: s.gameOk,
    sp,
    server: s.server,
    installed: s.installed,
    offline: s.offline,
    need,
  });
  t.textContent = T(view.label);
  b.disabled = view.disabled;
}
/**
 * Show the progress overlay for a long-running operation.
 * @param {object} p
 * @returns {void}
 */
function showProgress(p) {
  const el = $('prog');
  if (el.hidden) Mus.duck(true);
  el.hidden = false;
  el.classList.toggle(
    'indet',
    p.phase === 'check' ||
      ((p.phase.startsWith('play-') || p.phase === 'launcher-swap') && !p.total),
  );
  const pct = p.total ? Math.min(100, (p.done / p.total) * 100) : 0;
  $('dl-fill').style.width = pct.toFixed(1) + '%';
  if (p.phase.startsWith('play-') || p.phase === 'launcher-swap') {
    // the PLAY flow: what is happening, a real bar where the size is known, ETA where known
    $('dl-label').textContent =
      T(p.label || 'Working') + (p.files ? `  ${p.filesDone || 0}/${p.files}` : '');
    $('dl-stats').textContent =
      p.unit === 'time'
        ? T('{n} s', { n: p.done })
        : p.unit === 'pct'
          ? T('{pct}%', { pct: pct.toFixed(0) }) +
            (p.eta ? '  ' + T('{t} left', { t: fmtEta(p.eta) }) : '')
          : p.unit === 'bytes'
            ? T('{pct}%  {a} of {b}', {
                pct: pct.toFixed(0),
                a: fmtBytes(p.done),
                b: fmtBytes(p.total),
              }) + (p.eta ? '  ' + T('{t} left', { t: fmtEta(p.eta) }) : '')
            : '';
    return;
  }
  const label = T(
    {
      check: 'Checking for updates',
      verify: 'Verifying files',
      download: 'Downloading',
      launcher: 'Downloading launcher',
    }[p.phase] || 'Working',
  );
  $('dl-label').textContent = label + (p.files ? `  ${p.filesDone}/${p.files}` : '');
  $('dl-stats').textContent =
    p.phase === 'check'
      ? ''
      : T('{pct}%  {a} of {b}', {
          pct: pct.toFixed(0),
          a: fmtBytes(p.done),
          b: fmtBytes(p.total),
        }) +
        (p.speed ? `  ${fmtBytes(p.speed)}/s` : '') +
        (p.eta ? '  ' + T('{t} left', { t: fmtEta(p.eta) }) : '');
}
/**
 * Hide the progress overlay.
 * @returns {void}
 */
function hideProgress() {
  $('prog').hidden = true;
  Mus.duck(false);
}

// ---------------------------------------------------------------- actions
/**
 * Refresh client and server state from main.
 * @returns {Promise<void>}
 */
async function refresh() {
  const [s, st] = await Promise.all([f2.checkState(), f2.serverStatus()]);
  if (s.ok) state = s.state;
  if (st.ok) status = st.status;
  renderState();
  renderStatus();
  if (st.ok) Notices.update(st.status);
  autoLauncherUpdate();
  autoClientUpdate();
}
// A client build published while the launcher is open is downloaded by itself (once per manifest version; a failed try is not
// repeated in a loop) - and while the game runs the player is told to quit it first, once per version. Files are never touched under a running game.
let cuTried = '',
  cuTold = '';
/**
 * Install a newly published client build by itself while idle; tell the player to quit a running game first.
 * @returns {Promise<void>}
 */
async function autoClientUpdate() {
  const act = PlayState.clientUpdateAction(state, gs, busy || lupBusy, sp),
    v = String(
      (state && state.manifest && state.manifest.version) ||
        (state && state.toDownload && state.toDownload.files),
    );
  if (!act) return;
  if (act === 'quit-game') {
    if (cuTold !== v) {
      cuTold = v;
      toast(
        T('A new update is available. Quit the game, then the launcher installs it.'),
        true,
        T('UPDATE AVAILABLE'),
      );
    }
    return;
  }
  if (cuTried === v) return;
  cuTried = v;
  f2.log(`client update ${v}: downloading by itself`);
  await doUpdate('update');
}
// A detected launcher update is announced and installed right away (not only at PLAY): dark overlay over everything,
// white "Launcher update available. Updating.", progress under it; the launcher restarts itself. Once per version, never while the game runs.
let lupBusy = false;
/**
 * Install a pending launcher update automatically when safe.
 * @returns {Promise<void>}
 */
async function autoLauncherUpdate() {
  const lu = state && state.launcherUpdate;
  if (
    !lu ||
    !lu.url ||
    lupBusy ||
    busy ||
    banState.banned ||
    gs.s === 'running' ||
    gs.s === 'starting'
  )
    return;
  // up to 3 tries per version (a launcher closed mid-download tries again on its next start; a swap that never works does not loop forever)
  let tr = '';
  try {
    tr = localStorage.getItem('f2-selfupd-n') || '';
  } catch {}
  const [tv, tn] = tr.split(':'),
    n = tv === String(lu.version) ? parseInt(tn, 10) || 0 : 0;
  if (n >= 3) return;
  try {
    localStorage.setItem('f2-selfupd-n', `${lu.version}:${n + 1}`);
    localStorage.setItem('f2-selfupd', String(lu.version));
  } catch {}
  lupBusy = true;
  const ov = $('loading'),
    t = ov.querySelector('.ld-title'),
    sub = ov.querySelector('.ld-sub');
  t.textContent = T('Launcher update available. Updating.');
  t.classList.add('ld-lup');
  sub.textContent = T('Version {v}', { v: lu.version });
  ov.classList.remove('gone');
  logline(T('Launcher update available. Updating.'));
  f2.log(`launcher update ${lu.version}: overlay on, downloading`);
  const u = await f2.updateLauncher();
  if (u.ok) return; // the launcher quits and starts the new exe
  lupBusy = false;
  ov.classList.add('gone');
  t.classList.remove('ld-lup');
  toast(u.error);
  logline(u.error);
}
/**
 * Run client update or verification.
 * @param {'update'|'verify'} kind
 * @returns {Promise<boolean>}
 */
async function doUpdate(kind) {
  if (busy) return;
  showProgress({ phase: 'check', done: 0, total: 1 });
  const r = await (kind === 'verify' ? f2.verify() : f2.update());
  hideProgress();
  if (!r.ok) {
    toast(r.error);
    logline(r.error);
    return false;
  }
  toast(
    r.downloaded
      ? T('Client updated to {v} ({n} files).', {
          v: r.version,
          n: r.downloaded,
        })
      : T('Client is up to date ({v}).', { v: r.version }),
    true,
  );
  await refresh();
  return true;
}
/**
 * Open a specific settings sub-pane.
 * @param {string} sub
 * @returns {void}
 */
function gotoSettings(sub) {
  showView('settings');
  document.querySelector(`[data-sub=${sub}]`).click();
}
