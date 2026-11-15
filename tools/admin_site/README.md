# Twelve Weeks admin site

Styled like the public website (twelve-weeks.com: same fonts, colours, header bar, tables, tiles, chips, panels; phone layout included). Public URL **https://admin.twelve-weeks.com** (Cloudflare tunnel -> this server).

## Admin accounts (Steam login first, token as fallback)

**Steam admins (primary).** Open https://admin.twelve-weeks.com and press "Log in with Steam". The site signs you in on twelve-weeks.com (cookie `tw_s`,
valid for `twelve-weeks.com` and every subdomain) and sends you back here. You are in when your Steam64 id is a line in
`<db dir>/web-admins.txt` (one id per line, `# comments` allowed, re-read on every change, no restart). The very same list lets those people edit the
news on twelve-weeks.com. Someone logged in with Steam who is *not* on the list sees a "Not an admin" page with their Steam id to send to the owner.
Add one: `echo "76561198000000000  # name" >> <db dir>/web-admins.txt`.
How to find a Steam64 id: open the Steam profile page in a browser; if the address is `steamcommunity.com/profiles/7656...` that number is it, if it is a
`/id/name` custom URL paste the profile link into steamid.io (or look at the player in this panel's PLAYERS tab: the STEAM ID column). It is the 17-digit
number starting with `7656`.

**Token accounts (fallback).** Under the "Use an access token" link on the login page (https://admin.twelve-weeks.com/login-token) the browser asks for a
user name and password (HTTP Basic). Accounts are lines `name token` in `<db dir>/admin-users.txt` (re-read on every request, no restart). Ten failed token logins from one address lock it for 10 minutes.

**Where the checks live.** `admin.py` validates the `tw_s` cookie with the website's secret (`--web-secret`, default `<db dir>/web-secret`; HMAC-SHA256 of
`<steamid>.<expiry>`, expired or forged = login page) and the id against `--web-admins` (default `<db dir>/web-admins.txt`). `--site-url` (default
`https://twelve-weeks.com`) is where the Steam button and Log out go. The website's `web.py` takes `--allow-next` (default `https://admin.twelve-weeks.com`):
the only https origins `/auth/steam?next=` may send you back to after login (subdomains of the public host only, never an open redirect). Unauthenticated
visitors get a styled login page (HTTP 401, no `WWW-Authenticate`, so no browser dialog). Requests from this machine over 127.0.0.1 (ssh tunnel) still
need no login. Start line: `admin.py --bind 127.0.0.1 --tunnel-host admin.twelve-weeks.com --db data/t2r.db ...` (the two new args default to files next to the DB).

A local, launcher-styled web page for the server owner (Python stdlib only, binds **127.0.0.1** by default, `--bind 0.0.0.0` + a users file for LAN access).

```
python tools/admin_site/admin.py --db "%LOCALAPPDATA%\T2R-TwelveWeeks\t2r.db" --daemon-log <host folder>\daemon.log ^
       --reports <update data>\reports [--port 7010] [--daemon 127.0.0.1:7001] [--admin-url http://127.0.0.1:7011] [--update-data <update data>]
```
Open http://127.0.0.1:7010/ . Keys: `1-8` tabs, `/` search, `Esc` back, `P` pause the 3 s refresh, `R` refresh now.

Tabs: OVERVIEW (server up/down, uptime, build vs latest published, online / in match / searching, matches, recent errors), PLAYERS (online now with state,
clan, ping, build, platform, session length and a FORCE KICK button; all-time list with search; per-player page with clan state, morale, clan feed, match
history, bug reports), MATCHES (live matches with teams, map, host, ping, time left, reinforcements; recent results), CLANS (all clans, at-risk ones, feed),
REPORTS (bug reports, files, text preview, mark handled, attached screenshots as click-to-open thumbnails via `/api/report-image`, attached clips in an inline player via `/api/report-video` with HTTP Range seeking; both only for files the server recorded as screenshot/video, type from the bytes), LOGS (daemon log tail, filter, error highlighting, rotated logs, tokens masked).

## Where the data comes from
| Source | When | Gives |
|---|---|---|
| `live` | daemon build with the admin listener (needs a daemon restart) and `admin-secret.txt` next to the DB | real-time players / queue / matches, ping, kick |
| `snapshot` | `t2r-status.json` next to the DB, written by the same daemon build every 10 s | same, up to 10 s old, no kick |
| `log` | always available from `--daemon-log` (EVT / STATUS / ANNOUNCE lines) | online count (last STATUS line), running matches (players, map, host), history; approximate: no scores, no ping, names of menu players only if they opened the MP menu in the last minute |
The database (`t2r.db`) is opened `mode=ro` per query (1.5 s timeout, never written, no long locks). Player stats, clans, names, feed, client builds
(`client_versions.json`) and platform (`linux_clients.json`) come from it and the files beside it. Kills / deaths / executions are not recorded by the
server, so they are not shown (the page says so). Match history is reconstructed from the retained daemon logs (`daemon.log` + rotated files).

## Safety
* Only the report store (`handled` flag in meta.json) and, after an in-page confirm, `POST /kick` to the daemon's loopback listener are written.
* Requests need `Host: 127.0.0.1|localhost:<port>` (DNS rebinding); POSTs need the `X-Admin-Request` header and a same-origin `Origin` (CSRF).
* Tokens are never shown: players are addressed by a hash id; log lines have long hex runs and byte dumps masked.
* The game port is only probed with a connect + close (nothing is sent; the daemon logs nothing for it).

Tests: `python tools/admin_site/test_admin.py` and `python tools/tests/test_admin_kick.py` (daemon side: live snapshot, kick, listener).

## Clan page (launcher CLAN tab, read-only)

The player page embeds `/clan.html?id=<player id>` (iframe): the launcher's own `clan.js`, `clan-fx.js`, `clan-status.js`, styles and portrait / icon assets, served read-only from `release/launcher/` (whitelisted paths under `/launcher/src/` and `/launcher/assets/`), fed by `/api/clanview?id=<player id>[&lang=pl]`, which returns the same payloads as the update server's `/clan.json` and `/clanfeed.json` (`tools/update_server/clan_view.py`, `clanfeed_view.py`, called with `key=` = the player's clan row). Tabs OVERVIEW / SURVIVORS / RAIDS / JOURNAL are the launcher's sections grouped; EN / PL button inside. `&still=1` skips the entrance motion (screenshots).
