# admin_dashboard

Refine/React admin panel with a FastAPI backend, ported from `tools/admin_site/`. The classic panel
is kept intact as a reference/rollback; the `Twelve-Weeks-Main` worktree is the behavioural oracle
for data compatibility.

## Layout

```
admin_dashboard/
├── admin-server/        FastAPI backend + ported data layer (core/store*.py, core/logindex.py)
│   ├── server.py        API routes + built-SPA fallback
│   ├── config.py        CLI/env settings (all paths resolved from --db or the repo root)
│   ├── service.py       LIVE/TEST selection over the ported store
│   └── tests/           parity tests against the Main worktree oracle
└── admin-client/        Refine + React client
    └── src/pages/overview/   Overview port (matches classic viewOverview)
```

The backend reads the same files and calls the same daemon endpoints as the classic site, and
serves the API under `/api`. The built client is served from `/` (SPA fallback), so a single
process serves both. In dev, Vite proxies/uses `VITE_API_URL=/api` (see `.env.example`).

## Run locally

Backend (from the repo root; Python 3.10+, dependencies in `admin-server/requirements.txt`):

```bash
python -m venv /tmp/admin-venv && /tmp/admin-venv/bin/pip install -r tools/admin_dashboard/admin-server/requirements.txt
/tmp/admin-venv/bin/python tools/admin_dashboard/admin-server/server.py \
  --db tools/t2r.db --daemon-log auto --port 7010
```

`--daemon-log auto` follows the newest `dist/*/host/daemon.log`; pass an explicit path or set
`T2R_ADMIN_DAEMON_LOG` to pin one. Other classic options (`--reports`, `--admin-url`, `--daemon`,
`--update-data`, `--test-db`, …) are accepted as well; run with `--help` to list them.

Frontend, for hot reload:

```bash
cd tools/admin_dashboard/admin-client
npm install
npm run dev          # http://localhost:5173, API at /api (same-origin via Vite)
npm run build        # writes admin-client/dist, which the backend then serves
```

Then open <http://127.0.0.1:7010/> (redirects to `/overview`).

## Overview parity

`/api/overview`, `/api/online` and `/api/notices` return the legacy JSON shapes verbatim from
`core/store.py` (a readable rewrite of the classic store with identical public methods and payloads).
That file is a thin facade: `store_common.py` holds shared constants/helpers/infrastructure and the
`Admin` methods are split across `store_live.py`, `store_players.py`, `store_overview.py`,
`store_player_detail.py`, `store_clans.py`, `store_reports.py`, `store_logs.py`, `store_actions.py`,
`store_news.py`, `store_bans.py` and `store_clan_admin.py`. `Overview` consumes exactly those three
endpoints, like the classic `viewOverview(ov, on, nt)`, and mirrors `paintTop()` for the shared
header/footer. Parity is enforced by:

```bash
/tmp/admin-venv/bin/python tools/admin_dashboard/admin-server/tests/test_overview.py
```

The test builds the oracle's fixture DB, calls both implementations and asserts the payloads are
equal key-for-key (only the `now` timestamp is ignored). It uses a sibling `Twelve-Weeks-Main`
worktree when present and falls back to the in-tree `tools/admin_site`; override the worktree
location with `TWELVE_WEEKS_MAIN`.

## Status / caveats

- Only the Overview page is ported so far; other routes are placeholders in `App.tsx`.
- Auth, host checks and CSRF from the classic panel are **not** ported. `/api/whoami` is a local
  stub, so keep the backend bound to loopback (the default) until auth is ported.
- `tools/admin_site/` remains as a rollback reference; `Twelve-Weeks-Main` is the current behavioural
  oracle. Do not delete the classic panel yet.
