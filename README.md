# THIS REPO IS UNDERGOING CLEANUP. WE GET THE AI CODE CAN BE HARD TO UNDERSTAND AND ARE WORKING ON IT RIGHT NOW. THANK YOU!

# Twelve Weeks

Twelve Weeks is a free, fan-made multiplayer mod for **The Last of Us Part II Remastered** on PC.
It brings back a clan-based team multiplayer mode in the spirit of the 2013 multiplayer: survive
twelve weeks with your clan, gear up between rounds, craft, revive and execute.

Website: <https://twelve-weeks.com>

> Twelve Weeks is an unofficial fan project. It is not affiliated with, endorsed by or sponsored by
> Sony Interactive Entertainment or Naughty Dog. "The Last of Us" is a trademark of its owners.
> You need your own legitimately purchased copy of the game (Steam). This repository contains no game
> files and no assets taken from the game.

## Playing

Players do not need this repository. Download the launcher from the website; it finds the game, installs
and updates the mod files, and starts the game.

## What is in this repository

| Path | What |
|---|---|
| `release/launcher/` | The desktop launcher (Electron, plain HTML/CSS/JS). See its `README.md` and `API.md`. |
| `release/client/` | Client-side helpers: the Steam networking helper (`steam/t2r-steam.c`), the slim-build expander (`slim/t2r-slim.c`), the PowerShell play script and the Linux / Proton notes. |
| `release/host/` | Scripts for hosting a community server. |
| `tools/daemon.py` | The game server (matchmaking, lobbies, clans, loadouts, leaderboards). Python 3 standard library only. |
| `tools/clan*.py`, `tools/social.py`, ... | Server modules the daemon imports (clan simulation, clan journal text, friends / parties, names, notices). |
| `tools/update_server/` | The update / file server the launcher talks to (manifests, news, status, clan views, bug reports). |
| `tools/admin_site/` | A local admin web page for the server owner. |
| `tools/web/`, `site/` | The public website (landing page, news, leaderboards, player pages). |
| `winmm/` | The client source (C++). Reference only: the shipped `winmm.dll` is a prebuilt binary plus our patches, see CONTRIBUTING.md. |
| `servers/` | A C++ matchmaker and relay for the client protocol. Reference only. |
| `tools/ext/` | `t2r-ext.dll`, the in-game feature extension (C, one unity build: `t2r-ext.c`). |
| `tools/features/`, `tools/maps/`, `tools/part1/`, `tools/loc/` | Data-build tools that patch the game's own data from your local install (UI widgets, DC script patches, maps, translations). |
| `tools/tests/` | Python tests for the server and the build tools. |

## What is not in this repository

- Game files and anything extracted from them: level paks, textures, audio, animation data, string tables.
  Several generated headers in `tools/ext/` (camera tracks, sound clips, animation tables) are produced
  by the data-build tools from your own game install and are not checked in; see `tools/ext/GENERATED.md`.
- The launcher's art, music and sound assets (`release/launcher/assets/`).
- Binaries (DLLs, executables) and the client files distributed by the update server.

## Building and testing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## Credits

Although Twelve Weeks is mostly a one-person project, it wouldn't be the same without the help of others.

- **Kellie**: project lead and design (gameplay, melee, clans, maps, site, launcher).
- **Wedge**: male character animations (not in yet) and the Frontier rifle animations.
- **OmegaMan**: launcher code cleanup, the Linux port, Rich Presence support.
- **DeepQuantum**: server hosting, funding, general help, and being an awesome friend :3
- Everyone who playtested and sent bug reports.

## License

GPL-3.0. See [LICENSE](LICENSE).
