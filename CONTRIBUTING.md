# Contributing and building

Thanks for helping. Bug reports and pull requests are welcome.

## Ground rules

- Never commit game files or anything extracted from them (paks, textures, audio, animation data,
  string tables, generated headers listed in `tools/ext/GENERATED.md`). `.gitignore` blocks the common
  cases; check `git status` before you commit.
- No binaries (DLLs, executables, archives) in the repository.
- No personal data: no Steam IDs, IP addresses, player names, local paths, tokens or keys. Server
  addresses and secrets come from command-line options, environment variables or files next to the
  database that are not checked in.
- Comments explain why the code does what it does (engine behaviour, offsets, invariants). Keep them
  technical.

## Requirements

- Node.js 20 or newer (launcher)
- Python 3.10 or newer (servers, build tools, tests). The servers use the standard library only.
- MinGW-w64 GCC 13 or newer (the in-game extension and the client helpers), on Windows or as a cross
  compiler on Linux
- For the data-build tools and the extension: your own install of The Last of Us Part II Remastered (PC)

## Launcher (`release/launcher`)

```bash
cd release/launcher
npm ci
npm test
npm start          # run from source
npm run dist       # portable Windows exe (needs the art in assets/, see below)
```

`assets/` (art, music, UI sounds, icon) is not in the repository. `npm test` and `npm start` work
without it; `npm run dist` needs at least `assets/icon.ico` and `assets/icon.png`.
Server addresses are in `defaults.js`; `node scripts/stamp-defaults.js <host:port> <http://host:port>`
stamps others for a build. For local testing set `F2_SERVER` and `F2_FILE_HOST` instead.

## Game server (`tools/daemon.py`)

```bash
python tools/daemon.py
```

Listens on TCP + UDP port 7001 (`T2R_PORT`), stores players, clans and loadouts in a SQLite database
(`T2R_DB`). Other settings are environment variables named `T2R_*`, documented where they are read.

## Update server (`tools/update_server`)

```bash
python tools/update_server/server.py --data <data folder> --port 7002
python tools/update_server/publish.py <client folder> --data <data folder> --channel stable --version <n>
```

See `tools/update_server/README.md` for the endpoints and `release/launcher/API.md` for the contract
with the launcher.

## Admin page and website

- `python tools/admin_site/admin.py --db <t2r.db> ...` - local admin page on 127.0.0.1 (see its README).
- New Refine/FastAPI replacement (Overview ported so far): see `tools/admin_dashboard/README.md`.
  Build the client with `cd tools/admin_dashboard/admin-client && npm run build`, then serve API and
  SPA together with `python tools/admin_dashboard/admin-server/server.py --db <t2r.db> --port 7010`.
- `python tools/web/web.py --db <t2r.db> --news <news.json>` - serves `site/` with leaderboards and player
  pages. The site images (`site/assets/`) are not in the repository.

## Client (`winmm.dll`)

The shipped `winmm.dll` is **not** built from the source in `winmm/`. The release build:

1. starts from a fixed prebuilt binary, `tools/local/winmm-nocave-127.0.0.1-7001.dll`;
2. applies byte patches at fixed offsets (`tools/features/winmm_patches.py`), run by `tools/features/build_dll.py`;
3. adds the `t2r-ext.dll` loader and the data caves, and sets the server address (`tools/features/patch_winmm.py`).

The base binary is not in git. Get a copy from a team member, then install it:

```bash
python tools/fetch_base_dll.py --from <path to the dll>
```

The script checks the SHA-256 and refuses any other file. A `winmm.dll` rebuilt from `winmm/` has different
offsets, so the patches do not fit it. Read and port from `winmm/`; put new features in `tools/ext/`.
See `winmm/README.md` for how to build the client source on its own.

## In-game extension (`tools/ext`)

```bash
python tools/ext/build_ext.py t2r-ext.dll --mode release
```

Generate the headers listed in `tools/ext/GENERATED.md` first. `--mode dev` builds faster for smoke tests.

## Tests

```bash
python tools/update_server/test_update_server.py
python tools/update_server/test_robustness.py
python tools/admin_site/test_admin.py
python tools/admin_dashboard/admin-server/tests/test_overview.py
cd release/launcher && npm test
```

Each Python test file runs on its own (`python <file>`). Many tests in `tools/tests/` need a game
install or built data and skip or fail without them.
