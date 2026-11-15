# Twelve Weeks Launcher

Desktop launcher for TLOU2 Twelve Weeks: Electron + plain HTML/CSS/JS, no framework, portable Windows exe.
TLOU2 / Twelve Weeks menu look: vertical text menu, warm cream semibold type, hairline dividers, slow cross-fades, a desaturated slowly drifting
background (optional), faint static grain, game UI sounds and music; WLF / Seraphite accent kept subtle.

## What it does

- **Play** - ports `release/client/T2R-Play.ps1` (`lib/game.js` + `lib/game/*`): finds the game (Steam `libraryfolders.vdf` + `appmanifest_2531310.acf`,
  common library folders, manual browse), installs the mod files into the game folder when `version.txt` changed (foreign
  `winmm.dll` kept once as `.t2r-original`, slim build via `slim/t2r-slim.exe`, `t2r-ext.dll`, `tlou-ii-mp/`, feature flags),
  patches the server ip/port into `winmm.dll` (same XOR sites), picks the player name (`t2r-name.txt` > launcher override >
  Steam name > Windows user), writes the name token `tlou-ii-mp.json`, starts `steam/t2r-steam.exe` (same arguments), runs the
  `0xB0` server/version check, sets `ShowLauncher=0` and starts `tlou-ii.exe`.
- **Updates** - manifest download (`API.md`): per-file sha256, size, 4 parallel connections, resumable `.part` + `Range`,
  verify (full re-hash) and repair, progress with speed / ETA, launcher self-update (swaps the portable exe).
- **ADD TO STEAM** - button left of Ko-fi (main screen, settings): adds the launcher to Steam as a non-Steam game "Twelve Weeks" with cover / wide cover / hero / logo art (`assets/steam/`, built by `scripts/gen-steam-art.py`). `lib/steam-shortcut.js` edits `userdata/<id>/config/shortcuts.vdf` (binary vdf, backup `.bak-twelveweeks`, an existing entry with the same exe or the name Twelve Weeks is updated in place) and copies the art to `config/grid/`. A running Steam is asked to quit (never killed) after a confirm, then started again.
- **Field notes** - `/news.json`. **Server panel** - `/status.json`, falling back to a TCP / version probe of the game server.
- **Clan** - how your clan is doing (week / day, survivors, hungry / sick, needed per day, active mission, event, last match, boosters) from
  `/clan.json` (read-only, `tools/update_server/clan_view.py`).
- **Camp feed** - the 2013 clan ticker, much deeper: small diary lines about the people of your clan (who is sick, who died, who sang by the fire) from `/clanfeed.json`;
  a CAMP journal in the Clan tab and one-line toasts on the home screen (`src/feed.js`: one at a time, fade in / out with a gradient as long as the sentence, soft tick).
- **Loadout (GEAR UP)** - the mod's own pages: 6 custom loadouts + the 8 pre-made classes, 13 LP, real weapon / skill names, levels and LP costs
  (`src/gamedata.js`, generated from `tools/ext/skills.c` + `t2r-ext.c` by `scripts/gen-gamedata.py`). Real, two-way, by name: the game writes its custom loadouts to `t2r-loadout.json` next to `tlou-ii.exe` (the launcher shows them), a saved edit
  is written to `t2r-loadout-launcher.json` and the game applies it in its menu (file contract: `API.md` "Loadout files"; `lib/loadoutfile.js`, `tools/ext/loadjson.c`).
  The tab shows "Synced with game" or "Will apply when the game is in the menu"; `launcher.json` keeps only a cache. No faction picker: the faction is chosen once, for the clan, in the game.
- **Music** - three calm TLOU2 tracks per theme (neutral, WLF, Seraphites), cross-faded in turn; the set follows the clan's faction (`Scenes.setFaction` -> `Mus.setTheme`): a theme change switches with a 3.5 s cross-fade at once when the running track is under 12 s old (start-up), otherwise the running track plays out and the next one comes from the new set. Ducked while downloading, faded out on PLAY and off while the game runs; own volume + toggle.
- **Settings** - game folder (auto-detect / browse), player name, faction accent, background, effects + intensity,
  sound volume, client folder, open game / client / logs folder, verify / repair, About
  (launcher version, build date, hardware, update launcher).

Where the client files live: `client\` next to the exe if it contains `winmm.dll` (the shipped-zip layout, no download
needed), else `%APPDATA%\Twelve Weeks Launcher\client`, or the folder set in Settings > Files.
Server and update host are built in, nothing to set up: `defaults.js` holds `OFFICIAL_SERVER` (the game server, written into winmm.dll by the Play flow) and `OFFICIAL_FILE_HOST` (the update server; **TODO: its public playit tunnel address**, the current value is a placeholder). Stamp them at build time with `node scripts/stamp-defaults.js <host:port> <http://update host:port>` before `npm run dist`. While the update server is unreachable the launcher shows "update server unreachable" and Play still works with the installed client. Dev / test only (not in the UI): env `F2_SERVER`, `F2_FILE_HOST`, `F2_CHANNEL`. Old `server` / `file_host` / `channel` values in `launcher.json` are dropped at start.

## Install layout and self-update (pin-safe, since 0.1.16)

The portable exe unpacks itself into %TEMP% on every start, and the old self-update overwrote the exe in place, so a taskbar pin captured a temp path (Electron icon, "app doesn't exist" after the next start).
Now the first run of any portable build installs itself into the fixed root `%LOCALAPPDATA%\Twelve Weeks Launcher` (`main/install.js`): the Electron runtime exe (written once, never replaced), a frozen shim `resourcespp\main.js` that loads
the newest complete `app-<version>pp.asar`, `icon.ico`, Start menu + Desktop shortcuts with AppUserModelID `org.twelveweeks.launcher` (also set by `app.setAppUserModelId` and `setAppDetails` relaunch command = the fixed exe).
An update downloads the new portable exe (unchanged `launcher.json` contract), starts it, and it ADDS `app-<new>` and restarts the same fixed exe. Old versions are pruned (2 kept). Old installs migrate on the first update (their swap script
replaces the portable exe; its first run installs). The old portable exe keeps working as a forwarder; `portable-dir.txt` remembers its folder so a shipped `client\` next to it is still used. Test: `node test/install-test.js`.

Product rename: installs from before it use `%LOCALAPPDATA%\Factions II Launcher` (runtime `Factions II Launcher.exe`, AppUserModelID `org.factions2.launcher`), settings in
`%APPDATA%\Factions II Launcher` (`~/.config/Factions II Launcher` on Linux), and park the mod in `<game>\Factions2-disabled` / `Factions2-sp-tools` while in SP mode.
The new build migrates silently: the first start moves the settings folder (`migrateUserData`), the first bootstrap moves the install root and renames the runtime exe
(`migrateRoot`; a root it cannot move is installed fresh and the old one removed later by `removeLegacyRoot`), Start menu / Desktop links and taskbar / Start pins that
start the old exe are rewritten (`migrateShortcuts`), and the first SP journal read renames the parking folders and rewrites the journal paths (`lib/spmode.js` `migrateLegacy`).
Each step is idempotent and uses the old place while a file there is held open. An old launcher updates as usual: it starts the new portable exe, which bootstraps into the new root.
Test: `node test/migration-test.js`.

## Run / build

```
cd release/launcher
npm install
npm start                  # dev run
npm test                   # node test/selftest.js: token, winmm patch, updater (resume, verify, repair) vs the mock server
npm run dist               # dist/TwelveWeeks-Launcher-<version>.exe (portable)
node test/mock-server.js 15390 [client-folder]   # fake update server (serves a real client folder if given)
node scripts/gen-assets.js # regenerate placeholder sounds + icon
```

Screenshots for review: `F2_USERDATA=<scratch> F2_SHOT=<dir> electron .` writes a PNG per view and quits (point `file_host` in
`<scratch>/launcher.json` at the mock server).

## REPORT A BUG
Menu entry REPORT A BUG: description (required), when, map / mode, optional crash dumps, the exact list of files that goes out, SEND, then a report code (R-xxxx).
`lib/report.js` (+ `lib/report/*`) collects `t2r-ext.log` (+ up to 3 previous rotations), `t2r-sp.log`, `tlou-ii-mp.log`, `t2r-ext-boot.txt`, `t2r-version.txt`, `t2r-settings.txt`, the launcher
log, `t2r-play.log`, a generated `system.txt`; scrubs tokens; zips (own writer, no dependency); POSTs to `<file host>/report` (API.md). Test: `node test/report-test.js`
(scratch folder only). Server side + CLI: `tools/update_server/` (`report_store.py`, `reports.py`, `test_reports.py`).

## Where the art and sound come from (all from the game / mod files on this machine, CPU-only extraction)

- Menu sounds `assets/sounds/*.wav`: TLOU2 `common.psarc soundbank4/gbl-menu.bnk` (menu-change, -select, -cycle, -open, -back, -error, -toggle,
  -slider-tick, -close), decoded with `tools/local/vgmstream` via `tools/dsar_list.py`.
- Music `assets/music/*.ogg` (Opus 64 kbps stereo, 6.3 MB in all, 4.7 MB of it the faction sets), all from `common.psarc music1/*.xvag` (vgmstream, 4 ch = two stereo stems -> stereo as front + 0.7 x rear, interactive-music chunks chained with 2.5 s cross-fades, loudness-normalised to -23 LUFS, faded in/out):
  - neutral: `01-ambient` = `397_joelgrave_ambient_loop`, `02-slide-guitar` = `409_efg-guitar-flashback-a_035_mx`, `03-guitar` = `015_t2r_the-guitar-play-b`.
  - WLF (Seattle / Abby's side): `wlf-01-seattle` = `648_seattle_explore_mood_a` + `655_watchtower_new_unaware_chunk_a` + `501_watchtowerescape_unaware_chunk_b` (103 s); `wlf-02-watchtower` = `219_watchtower_unaware_chunk_a` + `658_watchtowerescape_unaware_chunk_d` + `270_floodedcity_unaware_chunk_b` + `460_abbybreweryfight_mood_e` (94 s); `wlf-03-brewery` = `460_abbybreweryfight_mood_a`, `_c`, `_b`, `_d` (105 s).
  - Seraphites (island / forest): `sera-01-forest` = `644_elliescarisland_new_unaware_chunk_a`, `_h`, `_c`, `_e` (99 s; cues `nor-scar-forest-unaware`, `ski-hunting-party-unaware`); `sera-02-island` = `240_elliescarisland_full_unaware_chunk_a`, `_c`, `_g`, `_i` (103 s); `sera-03-abby-island` = `611_a_abbyscarisland_sle_intro` + `611_g_abbyscarisland_low_tension_loop` + `608_abbyscarisland_unaware_chunk_c` (113 s). Calm / unaware beds only, no combat cues.
- Icons `assets/icons/*.png`: exactly the icon each GEAR UP row uses (`tools/ext/t2r-ext.c` weapon tables, `death_ui.c` icon table): the mod's rendered icons (`re/modfs_feat/gtex1/streaming/t2r/icons`, 1080p size: colt, m14, burst-rifle, frontier, tac-shotgun, shorty, cz75, el-diablo, double-barrel, milsniper, machete + the `*-supp` quiet variants), the game's own HUD icons read from `actor97/gui2-textures-common.pak` with `tools/models/ndpak.py` (revolver, bow, crossbow, hunting pistol, specter, pump shotgun, assault rifle, vepr-scp, the empty-slot crossed circle `blocked.png` = `none.png`) and `re/icons` (glock, mpx5, remington-bolt-scp).
- Backgrounds `assets/bg/*.jpg` (1920x1080, desaturated grade, 130-420 KB each): TLOU2's own stills from `core.psarc gtex1/streaming/menus/`: the Extras art gallery (`art-gallery/act2/16-forward-base` = the WLF stadium, `20-save-lev` and `21-abby-fights-militia` = Haven and the burning island, `03-seattle-arrival` / `12-flooded-city` = Seattle), the chapter-select renders (`chapters/24,26,39`) and the encounter-select renders (`encounters/e03,e38,e45,e46,e48,e50`). Extracted read-only with `tools/dsar_list.py` (`open_files` / `extract`), trimmed of their letterbox bars, cropped to 16:9, graded. Nothing from the player's recordings. Sets in `src/scenes.js` (captions EN + PL in `scripts/i18n-table.py`): WLF 9, Seraphites 10, neutral 6; no image is shared between sets.
- These assets are not part of the public repository.

## Swapping assets

- Sounds: replace `assets/sounds/{hover,click,tab,confirm,back,error,toggle,tick,launch}.wav` (same names). Music: the `.ogg` files in `assets/music` (names listed in `sets` in `src/audio.js`).
- Backgrounds: add a jpg to `assets/bg` and an entry to `list` in `src/scenes.js`.
- Icon: `assets/icon.ico` / `icon.png`.

## Notes

- Hard-coded nothing about the server: with no server set the Play button asks for one.
- Code layout (split into modules, contributed by OmegaMan; `lib/game.js`, `lib/update.js` and `lib/report.js` are thin facades, everything that used to be imported from them still is):
  - `main.js` starts the app and wires `main/*`: `env` (runtime flags), `config` (launcher.json), `paths`, `locale`, `mode` (SP/MP), `player` (name, t2r-name.txt), `jobs` (update, Play, SP/MP switch, launcher self-update; one busy guard), `status` (update check + server status, with the sticky last-good answers), `clan`, `report`, `startup`, `window`, and `main/ipc/*` (one file per area, every handler goes through one error-translating wrapper).
  - `lib/game/*` (discovery, dll, server, install, processes, steam, play), `lib/update/*` (net, files, staging, launcher, errors), `lib/report/*` (collect, artifacts, build, zip, scrub, transport, system), `lib/linux.js`, `lib/spmode.js`, `lib/loadoutfile.js`, `lib/daemon.js`, `lib/clanwire.js`, `lib/attach.js`, `lib/health.js`, `lib/sticky.js`, `lib/sysinfo.js`.
  - Renderer = classic scripts in the order of `src/index.html` (no bundler): `src/app/{core,ui,data,play,settings,news,report}.js` then `src/app.js` (startup); GEAR UP is `src/loadout-model.js` (rules), `src/loadout-view.js` (HTML), `src/loadout.js` (controller). A new `src/app/*.js` file must be added to `index.html` and to `files` in package.json (`src/**` already covers it).
  - `dev/` (screenshot scenarios, only used with `F2_SHOT`) is not packaged; `test/srcs.js` lets the source-text tests read a file group (`main.js` + `main/*`, ...).
  - `npm run lint` / `npm run format:check` (eslint + prettier, config in `eslint.config.mjs` / `.prettierrc`, dev dependencies only). `src/index.html`, `src/gamedata.js`, `src/ticker.js`, the i18n JSON and `test/` are deliberately not reformatted (see `.prettierignore`: translation keys are exact text-node strings, generated files come from `scripts/gen-*.py`).

## Linux (Steam / Steam Deck, AppImage)

`npm run dist:linux` builds `dist/TwelveWeeks-Launcher-<version>.AppImage` (run it on a Linux machine or CI; electron-builder cannot produce an AppImage on plain Windows). Same UI; `lib/linux.js` + the Linux branch of `lib/game.js` do what `release/client/t2r-linux.py` does, with the Steam launch option automated:

- **Finding things**: Steam roots `~/.steam/steam`, `~/.local/share/Steam`, Flatpak `~/.var/app/com.valvesoftware.Steam/.local/share/Steam`, `libraryfolders.vdf` (SD cards), `appmanifest_2531310.acf` installdir, Proton from Steam's `CompatToolMapping` (the game's, else the default, else the newest) in `steamapps/common` / `compatibilitytools.d`.
- **Install**: the same file copy as Windows. The slim client's `winmm.dll` is rebuilt by `slim/t2r-slim.exe` run under Proton (`proton run`, own prefix `<userData>/t2r-steam-prefix`). If the update server publishes a full client as channel `<channel>-linux` (`publish.py --channel stable-linux`) the launcher uses that and skips the Proton step; the manifest may carry `launcher_linux` (same fields as `launcher`) for the AppImage self-update.
- **winmm override without a launch option**: before every start (game closed) it writes `"winmm"="native,builtin"` into `compatdata/2531310/pfx/user.reg` (`HKCU\Software\Wine\DllOverrides`, backup `user.reg.t2r-backup`) and `ShowLauncher=0`. If the prefix does not exist yet (the game was never started through Steam) PLAY says so: start the game once from Steam, close it, press PLAY again.
- **Steam helper**: `t2r-steam.exe` through `proton run` in its own prefix, outside the game's launch, `SteamAppId=480`, `--alive-file <game>/t2r-alive.txt` (the ext touches it every 5 s), pid in `<userData>/linux-state.json`, log `<userData>/t2r-steam.log`, Proton output `t2r-steam-proton.log`. `t2r-linux.txt` (`server=`, `helper=external`) tells the ext.
- **Start**: `steam steam://rungameid/2531310` (Flatpak: `flatpak run com.valvesoftware.Steam ...`, else `xdg-open`). Game running = a process with `tlou-ii.exe` on its command line (not the helper).
- Electron runs with `--no-sandbox` on Linux (an AppImage cannot use the setuid sandbox helper).
- Tests: `npm test` runs `test/linux-test.js` (fixture Steam folders, VDF, Proton choice, `user.reg` edit, /proc scan, the whole Play flow with a faked Proton and Steam).

## Compressed downloads

If a manifest entry has `zurl`/`zsize`/`enc` (`gzip` or `br`, both decoded with Node's built-in zlib, no native addons) the updater downloads the compressed blob resumably into `<file>.z.part`, stream-decodes it into `<file>.part`, checks the raw sha256 and renames; any failure falls back to the raw url. Progress/ETA count compressed bytes. Entries without those fields (and old servers) work as before. Tests: `npm test` (mixed raw+gzip+brotli, resume mid compressed file, damaged blob -> fallback).
