# Twelve Weeks launcher - update server API (v1)

Contract between the launcher (`release/launcher`) and the update / file-hosting server (`tools/update_server/`).
Plain HTTP(S), JSON, UTF-8. All URLs are relative to the **file host base URL**.

- Base URL: the launcher's built-in `OFFICIAL_FILE_HOST` (`release/launcher/defaults.js`, stamped at build time; no setting in the UI, everybody uses the same
  update server). Dev override: env `F2_FILE_HOST`. Channel: `stable` (dev: env `F2_CHANNEL`).
- The launcher sends `User-Agent: TwelveWeeks-Launcher`. No authentication in v1.
- Every JSON endpoint must answer within a few seconds; the launcher times out after 4-10 s and then shows OFFLINE
  (an already installed client can still be played).

## GET /manifest.json?channel=stable

`channel` is `stable` (default) or `beta`.

```json
{
  "version": "914be2d5",
  "channel": "stable",
  "notes": "optional one-liner shown in the CLIENT panel",
  "files": [
    { "path": "winmm.dll",              "size": 123456789, "sha256": "<64 hex>", "url": "/files/<sha256>" },
    { "path": "t2r-ext.dll",            "size": 2048000,   "sha256": "<64 hex>" },
    { "path": "tlou-ii-mp/sidbase.bin", "size": 1234,      "sha256": "<64 hex>" },
    { "path": "steam/t2r-steam.exe",    "size": 48027136,  "sha256": "<64 hex>" }
  ],
  "launcher": { "version": "0.2.0", "url": "/files/launcher/0.2.0", "sha256": "<64 hex>", "size": 90000000 }
}
```

Rules:

- `files[].path`: relative to the **client folder** layout that `release/build_release.py` produces
  (`client/winmm.dll`, `client/t2r-ext.dll`, `client/version.txt`, `client/tlou-ii-mp/...`, `client/steam/...`,
  `client/slim/t2r-slim.exe` + `client/t2r-slim.dat` for the slim build, the `t2r-*.txt` feature flags, `server.txt`).
  Forward slashes, no `..`, not absolute. The launcher rejects the whole manifest on an unsafe path.
- `version.txt` must be in the list; its trimmed content is also `version` (this is the build stamp the game server's
  version check `0xB0` compares - launcher and daemon must agree).
- `sha256`: lowercase hex of the file bytes. `size`: bytes. Both are verified after every download.
- `url`: optional, absolute or relative; default `/files/<sha256>`.
- Compressed transfer (optional, all three together): `zsize` (bytes of the compressed blob), `zurl` (absolute or relative, normally `/files/<sha256>.gz` or `.br`) and `enc`: `"gzip"` or `"br"` (brotli). Present only when the compressed blob is >= 5 % smaller. `sha256`/`size`/`url` always describe the RAW file and stay valid for old launchers. A launcher that does not know the `enc` (e.g. `"zstd"` later) ignores the three fields and uses `url`.
  The launcher downloads the compressed bytes with Range/resume into `<dest>.z.part`, stream-decodes them into `<dest>.part`, verifies the raw `sha256`, renames. If that fails (404, damaged blob, checksum) it falls back to `url`. Progress counts compressed bytes where `zsize` exists.
- A file that exists in the previous manifest but not in the new one is deleted from the launcher's client folder.
- `launcher` is optional. If `launcher.version` is newer than the running launcher the About tab offers UPDATE
  LAUNCHER: the launcher downloads `url`, checks `sha256` (and `size` if given, else HEAD `Content-Length`), then swaps its
  portable exe and restarts.
- Versions compare numerically per dot/dash segment (`0.2.0` > `0.1.9`). The client `version` is only compared for equality.

## GET /files/<sha256>

Content-addressed file download. **Must support `Range: bytes=N-`** (206 + `Content-Range`) - the launcher resumes
interrupted downloads from a `.part` file; a 200 answer to a Range request makes it restart that file. `Content-Length`
must be set. Redirects (301/302/307/308) are followed. The launcher downloads 4 files in parallel.
`/files/launcher/<version>` (or any URL given in `launcher.url`) serves the portable launcher exe.

### GET /files/<sha256>.gz , /files/<sha256>.br

The compressed twin of a blob: the raw gzip / brotli stream as plain bytes - `Content-Type: application/octet-stream`, **no `Content-Encoding` header** (the launcher decodes itself, so `Range`/resume work on the exact compressed bytes). Same Range support and 404 rules as the raw blob.

## GET /news.json

NEWS tab. A JSON array (or `{ "items": [...] }`), newest first. One entry per build or hotfix; the list shows `BUILD 85 | 4080fec4  headline  date` and an entry opens (accordion) into its sections:

```json
[ { "id": 10, "date": "2026-10-20", "build": "85", "kind": "build", "stamp": "4080fec4",
    "headline": "Executions, shiv and the maps are back",
    "sections": { "changed": ["..."], "why": ["..."], "watch": ["..."], "known": ["..."], "fixed": ["..."] } } ]
```

- `build`: "85" or "85.2"; `kind`: `build` or `hotfix` (hotfix shows `BUILD 85.2 HOTFIX | stamp`); `stamp`: git hash from the client `version.txt`.
- `sections` (all optional, only non-empty ones render): `changed` = WHAT CHANGED, `why` = WHY, `watch` = WATCH OUT FOR, `known` = KNOWN ISSUES, `fixed` = FIXED. Arrays of plain-text lines.
- Polish copy: every entry carries `"pl": { "headline", "sections": { same keys and line counts }, "title", "body" }`. The launcher shows it when its language is Polish, falling back to the English field per field; older launchers ignore `pl`. Source of truth is `release/news-pl.json` (keyed by entry id); `python release/news_merge_pl.py` writes `pl` into `news.json` (and reports entries without Polish). Every new entry gets both languages before publishing.
- Rich blocks (launcher 0.1.25+, optional): `"blocks": [...]` replaces the sections list in the opened entry. Block types: `{"type":"heading","text"}` (big heading), `{"type":"text","md"}` (blank line = paragraph, lines starting `- ` = list, `**bold**`), `{"type":"media","items":[{"src":"media/name.gif","caption"}]}` (1-3 items side by side, 16:9 frames; gif / webp / png / jpg / mp4 / webm; fetched only when the entry is opened), `{"type":"stats","cols":["DAMAGE",...],"rows":[{"icon":"melee/crowbar.png","name","values":[...]}]}` (icon = a png under launcher assets/icons) and `{"type":"sections","sections":{changed,fixed,...}}` (the usual lists inside the blocks). Keep `sections` filled with the full text anyway: launchers older than 0.1.25 ignore `blocks` and show `sections`. Polish: `pl.blocks` = the same list translated (same length and types; `src` and icons come from the English block; a mismatch falls back to English); `news_merge_pl.py` checks it.
- Media: `GET /media/<name>` on the update server (data dir `media/`, same host as news.json), plain file names only, max 6 MB, served with `Cache-Control: public, max-age=86400` and Range support. The launcher caches each file in its user data (`news-media`) and shows it as a data URI.
- Old entries `{ "id", "date", "version", "title", "body" | "text" }` still work (title as headline, body as one text block).
- Text only (the launcher escapes it). A newer newest `id` than the last one seen lights a pip on the tab and a NEW mark on unseen rows.
- The launcher reads it with an 8 s timeout, ignores the "host recently down" shortcut, re-reads it every 2 min and whenever the tab is opened while empty.

## GET /status.json

Server panel (polled every 30 s):

```json
{ "players": 7, "matches": 1, "server_version": "914be2d5" }
```

### Server notices - `notices` and `target_version` in /status.json

When the update server runs with `--status-file` (run-update-server.bat does) `/status.json` also carries:

```json
{ "online": true, "server_version": "5f499ac9", "target_version": "5f499ac9",
  "notices": [ { "id": 3, "kind": "r", "param": 540, "text": "", "endsAt": 1760800000.0 },
               { "id": 1, "kind": "t", "param": 0, "text": "Maintenance tonight at 22:00", "endsAt": 1760799000.0 } ] }
```

`kind` is language-neutral (`r` planned restart, `param` = seconds left at the daemon's last snapshot, `endsAt` = unix time it happens; `s` shutting
down now; `t` free text, shown as-is; reason text of a restart is in `text`). `target_version` = the stable build published in the update data dir
(else the server build): when the installed build differs show the **UPDATE AVAILABLE** alert (no entry in `notices` for it - the launcher already
knows its own version). Render `r` / `s` in the alert panel as title **SERVER NOTICE**: English "Server restarting in N min." / "Server is shutting
down." and Polish "Restart serwera za N min." / "Serwer się wyłącza."; `t` text as-is. Not wired in the launcher yet: the launcher agent wires the
alert panel to this field (poll interval 30 s is fine; the snapshot is at most 10 s old). The in-game side (t2r-ext notice.c) polls the daemon itself.

If `/status.json` is unavailable the launcher falls back to a TCP connect + `0xB0` version probe against the game
server address (host:port from Settings / server.txt) - same bytes as `Test-Server` in `T2R-Play.ps1` - which can only
say online/offline and the server build, not the player count. The daemon could serve `players`
(`Sessions.online()` count) via the update server or an HTTP side port.

## GET /clan.json?player=<install id>

CLAN tab. Read-only; implemented by `tools/update_server/clan_view.py` (server.py `--db <daemon t2r.db>`), which opens the daemon's
database with `mode=ro` and never writes. `player` = the 12-byte install id (24 hex; the last 24 hex chars of the 64-hex name token also
work) = the key of the `clans` table. The launcher adopts the id of an existing install from `tlou-ii-mp.json` so an existing clan is found.
The id is an identity key: serve it only to trusted networks or leave `--db` off (the tab then says the clan service is unreachable).

```json
{ "ok": true, "hasClan": true, "faction": 1, "factionName": "WLF", "status": "alive|dead|won",
  "daysDone": 17, "daysTotal": 83, "week": 3, "dayOfWeek": 4, "weeks": 12,
  "size": 15, "healthy": 12, "hungry": 2, "sick": 1, "needed": 15, "maxSize": 18, "runsWon": 4, "runsLost": 2,
  "event": { "kind": "attack|growth", "name": "ATTACK", "startDay": 15, "dayOfEvent": 3, "length": 3 },
  "eventToday": null,
  "mission": { "id": 1, "name": "Down Enemies", "progress": 7, "daysLeft": 1, "goals": [3, 6, 9], "inEvent": true },
  "boosterNext": 3, "boosters": [ { "id": 1, "name": "Starting Ammo Multiplier", "count": 2 } ],
  "lastMatch": { "day": 16, "parts": 1310, "supplies": 26, "needed": 14, "gained": 1, "died": 0, "newHungry": 0, "newSick": 0, "healed": 1, "leftEarly": false },
  "lastMission": { "id": 1, "name": "Down Enemies", "progress": 9, "tier": 3, "boostersEarned": 1 } }
```

`{ "ok": true, "hasClan": false }` = no clan yet (the faction is chosen once, at clan creation in the game). Field meaning = `tools/clan.py`
(`state_text` ops 0xE1 / `meta_text` 0xE9 of the daemon: `needed` = 5 + 2N - floor(1.2N), `lday/lparts/lsup/lneed/lgain/ldied/lhungry/lsick/lheal` =
`lastMatch`, `mi/mp/ml` = mission id / progress / matches left, `wk` = weeks_total, `rw/rl` = runsWon/runsLost, `mx` = maxSize).

## GET /clanfeed.json?player=<install id>[&limit=30][&since=<id>][&lang=pl]

`lang` (optional, default `en`; `pl` / `pol` / `pl-PL` = Polish, anything else = English): the language the feed is rendered in. The daemon stores every message
language-neutral (template id + the people with their gender + the variables as keys, `clan_feed.lid` / `params`, see `tools/clan_story_pl.py`) next to the English
`text`, so the same stored message reads back in either language and the ids are identical in both. With `lang=pl`: each message has `lang: "pl"`, `text` / `short`
are Polish (correct verb gender per person, names left in the nominative), and `people[]` carries Polish display words next to the unchanged machine keys
(`roleText`, `stateText`, `moodText`, `illnessText`) and `morale.text`. A message written before the language-neutral columns existed has no template id and is
served in English (`lang: "en"`) whatever was asked; the top level has `lang` too. Without `lang` (older launchers) nothing changes except the extra `lang` field.

CLAN tab "CAMP" journal and the corner toasts on the home screen. Read-only, same rules as `/clan.json` (`clan_feed_view` = `tools/update_server/clanfeed_view.py`,
`--db`, `mode=ro`, same `player` id). The feed is written by the daemon (`tools/clan_story.py`) each time a clan day resolves; a database without
the feed tables answers an empty feed. `limit` 1..60 (default 30), `since` = only messages with a larger id (the launcher polls with the last id it has shown).

```json
{ "ok": true, "hasClan": true, "faction": 1, "factionName": "WLF", "status": "alive", "daysDone": 17,
  "morale": { "value": 41, "label": "hopeful|steady|strained|grim" }, "alive": 15,
  "people": [ { "name": "Priya", "gender": "f", "role": "medic", "state": "healthy|hungry|sick", "mood": "ill", "illness": "bronchitis" } ],
  "dead": [ { "name": "Sam", "day": 16 } ],
  "feed": [ { "id": 9, "day": 17, "week": 3, "kind": "worse", "mood": "afraid", "faction": 1, "names": ["Priya", "Marcus"], "ts": 1760000000.5,
              "text": "Priya's bronchitis is getting worse. Marcus says we need antibiotics, and soon.",
              "short": "Priya's bronchitis is getting worse. Marcus says we need antibiotics, and soon." } ] }
```

Message object (the same one the in-game widget gets, see below): `id` (integer, grows per player, also across clan runs), `day` (clan days done when written) / `week`,
`kind` (`hope plenty calm join recovery event_hold event_growth founding clan_won hope_after hunger starving sick medicine worse event_warn event_heavy
event_massacre event_nojoin clan_dead despair guilt grief grief_mercy loss_many death_illness death_mercy death_starved death_lost death_walkout death_suicide
death_bite death_bite_kin death_attack death_attack_many ticker`; `ticker` = the 2013 one-liners "X is gnawing on rat bones."), `mood`
(`hopeful cheerful calm neutral worried afraid desperate grim despair grieving`), `faction` (1 WLF / 2 Seraphites), `names` (people named in the text),
`ts` (unix seconds), `lang` (`en` | `pl`: the language `text` / `short` are actually in), `text` (full, plain UTF-8, 20-250 chars), `short` (one-line form, at most 118 chars / 150 bytes, whole sentences, no `|` or newline).
Newest first. Every line agrees with the clan numbers: whoever is named sick is sick in `/clan.json`'s `sick` count, deaths follow `lastMatch.died`,
newcomers follow `gained`. `people` lists alive members only (at most 80), `morale` is derived from supply surplus / deficit streaks, deaths, sickness and event outcomes.

### In-game widget format (daemon op 0xB4 -> 0xB5, tools/daemon.py `feed_op`)

The main menu widget asks the daemon every 60 s with `0xB4 [64B token][u8 language]` and gets `0xB5 "n=<k>
<id>|<text>
..."`: at most 3 lines, oldest first, `id` = the
message id above, `text` = the `short` form (plain UTF-8, one line, <= 118 chars and <= 150 bytes, no `|`, no newline, no markup). `daemon.feed_latest(token, limit)` calls
`clan_story.pick_for_widget(conn, clan_key, seen_ids, limit)`: newest unseen lines of the last 12 first, then the cycle restarts, so the client does not show the same
line every minute. `clan_story.latest(conn, clan_key, n, since)` returns the same lines as objects `{id, day, week, kind, mood, faction, text}` for any other caller.
The language byte is optional: absent or `0` = English (all older clients), `1` = Polish, any other value = English. t2r-ext sends the game's TEXT language
(the loc layer's language: Polish when the Polish text table is selected) and asks again at once when the language is switched in the options; the launcher can
send it too (`lib/daemon.js feedLines(host, port, token, 'pl')`). The reply is plain UTF-8 either way (the Polish letters are two-byte sequences; the 150 byte limit
counts bytes, so a Polish line is cut slightly earlier than an English one). An older daemon ignores the byte and answers in English.
The 0xE0 clan state text (`0xE1` reply) is `key=value` integers only: nothing in it is language dependent; the clan panel / mission strip words the ext shows
(WEEK / DAY / HEALTHY ..., mission names) are translated client-side by the ext's loc table (`tools/loc/pl/03-clan.txt`).

Launcher fallback ticker (`src/ticker.js`, generated by `scripts/gen-ticker.py`): `TICKER` (English) and `TICKER_PL` (Polish) have the same keys (`hungry`, `sick`,
`healthy` = parallel lists of `{A}` templates, `notSera`, `namesW`, `namesS`, `rare`); `TICKER_PL` adds `gender` (name -> `m` / `f` / `n`). Polish templates carry gender
switches `{A?<masculine>|<feminine>}`; fill any template with `tickerFill(template, name, gender)` (defined in ticker.js, `n` takes the masculine form). Filter a
Seraphite island's pool with the pool's own `notSera` (the Polish substrings drop exactly the same entries).

Suggested toast colour by `mood`: hopeful / cheerful = warm, worried / afraid = amber, desperate / grim / despair / grieving = muted red, calm / neutral = plain.

## Loadout files (launcher <-> game, no server)

The launcher's GEAR UP tab edits the game's real custom loadouts through two JSON files next to `tlou-ii.exe` (the game folder). Everything is by NAME:
the game's row indexes (`t2r-loadout.txt`, the server PROFILE blob `players.loadout`) depend on tables the mod builds at run time and are never exposed.
The server is not involved: the game keeps sending its PROFILE blob as usual after it applied a launcher file (`tools/ext/loadjson.c`, `lib/loadoutfile.js`).

```
t2r-loadout.json            written by the GAME (the truth)       - rewritten, atomically (tmp + rename), at its start and whenever a loadout / the active pick changes
t2r-loadout-launcher.json   written by the LAUNCHER on SAVE       - atomically; the game applies it in a menu when "rev" > its "launcher_rev"
```

Shared shape (the launcher file has no `launcher_rev`, the game file adds `lp`):

```
{ "format": 1,
  "rev": 1760000000123,                 // unix ms of the write (launcher: strictly increasing, max(now, previous + 1))
  "launcher_rev": 1760000000100,        // game file only: the launcher rev applied last (0 = never)
  "active": { "kind": "custom" | "class", "i": 0 },          // custom 0..5; class = the 8 pre-made ones in gamedata order (COMMANDO ... SUPPRESSION)
  "custom": [                           // exactly 6
    { "small": { "id": "pistol-beretta", "supp": 1 },         // small firearm; null = keep / the free 9mm
      "large": { "id": "rifle-mpx5", "supp": 0 } | null,      // large firearm
      "purch": { "id": "shotgun-baserri-over-under", "supp": 0 } | null,   // purchasable ("supp" always 0; a plain string id is accepted too)
      "skills": [ { "id": "SK_REVIVER", "lv": 2 } | null, x4 ],  // the 4 skill rows in order
      "lp": 12 } ] }                    // game file only: the Loadout Points the game counts
```

- Weapon `id`s are the mod's weapon ids (`rifle-mpx5`, `shotgun-shorty`, `pistol-cz75-enforcer`, ... as in `src/gamedata.js`); `supp: 1` is the suppressed
  variant (`<id>-supp` row; only for guns that have one: +1 LP small, +2 LP large). Skill `id`s are the `SK_` keys of `tools/ext/skills.h` (`src/gamedata.js` lists
  both in the same order); `lv` is 1..3 within the skill's levels. LP costs are the game's own (`lo_cost`): 13 LP at most per loadout.
- Game side, apply rules: the file is looked at twice a second while the game is in a menu; it is applied only when `rev` > `launcher_rev` of the game file, the game is not in a
  match, not on its own GEAR UP pages and has no pending mid-match pick (the next spawn then uses it). Per slot: an unknown weapon / skill id, a suppressed
  variant that does not exist, a bad level or a skill twice keeps that slot's current value (a duplicate is cleared); a loadout over 13 LP (or refused by the game) keeps its
  whole previous value. Then it takes the same path as an in-game save (`t2r-loadout.txt`, `t2r-skills.txt`, the lobby object that becomes the PROFILE blob), writes
  `t2r-loadout.json` with `launcher_rev` = the applied rev, and logs `launcher loadouts: applied rev N (6 loadouts, k slots fixed)` (plus one line per fixed slot).
  A file with a rev not newer than `launcher_rev`, not JSON, over 16 KB or without `rev` / `custom` is ignored (logged). An edit made on the game's GEAR UP pages after the
  launcher wrote (not yet applied) wins: the launcher file's rev is counted as applied.
- Launcher side: on start / when the game folder is known and every 3 s it reads `t2r-loadout.json` into GEAR UP (unless a launcher write is still waiting:
  `launcher_rev` < the last rev written); a saved edit writes `t2r-loadout-launcher.json` (refused over 13 LP). `launcher.json` (`loadout2`, `loadout_rev`,
  `loadout_game_rev`) is a cache only. Loadouts saved by an older launcher (no `loadout_rev` yet) are pushed to the game once instead of being replaced.
  Status line: `Synced with game` (game file current), `Will apply when the game is in the menu` (written, not applied yet), `Start the game once to sync` (no game file yet),
  nothing when the game folder is unknown. Unknown ids in the game's file (a new gun the launcher's data lacks) show as an empty slot.
- Tests: `release/launcher/test/loadoutfile-test.js` (read / write / validate / sync, scratch folders), `tools/tests/test_loadjson.py` (ext reader compiled from `loadjson.c` reads the
  launcher's file; key / id tables agree).

## POST /report  (REPORT A BUG)

Body = ONE zip (`Content-Type: application/zip`, <= 80 MB, usually a few MB): `meta.json` + `files/<name>` (logs, optional crash dumps, `system.txt`).
`meta.json`: `{ text (required, what happened), when, whenText, mapMode, version, name, nameSource, steamId, launcherVersion, os, gpu, includeDumps, files[], skippedDumps[] }`.
Answer: `200 {"ok":true,"code":"R-7F3K"}` (the code is shown to the player), `400` bad zip / no description, `413` too large, `429` more than 5 reports per IP per hour,
`503` reports disabled. Stored by the server in `<data>/reports/<UTC date-time>_<code>_<player>/` (meta.json + files/, flat safe names, zip-slip safe, nothing
executed), never served back. The launcher never sends the name token / `tlou-ii-mp.json` / `t2r-client.json`; text logs have every 64-hex run and `"token"` values
replaced; logs over 20 MB are cut to their last 20 MB; crash dumps (newest 2, game folder `*.mdmp` + `%LOCALAPPDATA%\CrashDumps\tlou-ii*.dmp`) only when ticked and
only if the compressed report stays under 60 MB. Read them with `tools/update_server/reports.py`.

**Screenshots (launcher 0.1.13, optional).** Up to 5 images (png/jpg/webp/gif/bmp, <= 8 MB each) ride inside the same zip as `files/screenshot-1.png` ...
(`meta.json` also lists them as `screenshots: [{name,size}]`, `fileSizes: [{name,size}]`). The server decides the type from the first bytes (a wrong extension is
renamed to the real one); an entry named `screenshot-N.*` that is not a real image, a 6th screenshot or one over 8 MB refuses the WHOLE report (`400`/`413`, nothing
stored). Stored `meta.json` files entries for them carry `kind: "screenshot"` and `contentType`. The 80 MB body cap is unchanged (5 x 8 MB fits).

**Video clips (launcher 0.1.13, optional).** `meta.json.clips` (0-2) announces clips. If accepted, the `POST /report` answer is
`{"ok":true,"code":"R-7F3K","clipKey":"<32 hex>","clipRefused":""}`; when clips are refused `clipKey` is `""` and `clipRefused` says why (more than 3 reports with
clips per IP per hour, reports folder over 20 GB, or < 10 GB free disk; text and logs are still stored). Each clip is then sent with
`PUT /report-clip?code=R-7F3K&key=<clipKey>`, body = the raw file (`Content-Length` required; mp4/mov = `ftyp` box, webm/mkv = EBML header, checked on the first
bytes; <= 200 MB each, <= 250 MB together, 2 per report). The server refuses an oversize `Content-Length` (`413`) before reading a byte, streams the body to disk in
1 MB pieces with a running counter, deletes the partial file on any failure, and stores `files/clip-N.<ext>` (`kind: "video"`). Answers: `200 {"ok":true,"name","size"}`,
`400` bad type / short body / too many, `403` bad key, `404` unknown code, `413` too big, `507` storage full. Not resumable: the launcher retries a dropped clip once from the
start (idle timeout 3 min, so a slow relay is fine). Report text is already filed when a clip fails; the launcher says which clip did not go through.

## Daemon admin / status listener (loopback only, tools/daemon.py `admin_server`)

A small HTTP listener inside the daemon on `127.0.0.1:T2R_ADMIN_PORT` (default **7011**, `0` = off; started with the daemon, so it appears after the next
daemon restart). It is not on the game port, so the playit tunnel does not expose it. It is meant for the launcher / admin site running on the same machine.

| Request | Auth | Answer |
|---|---|---|
| `GET /status` | none | `{"ok":true,"online":N,"queue":N,"matches":N,"server_version":"..","uptime_s":N}`. `online` = `PRES.count()` = players whose last keepalive is under `KEEPALIVE_TTL` (15 s): **the same number the game menu shows as the online count**. No names. |
| `GET /live` | `X-Admin-Secret` | `{"ok":true,"ts":..,"server_version":"..","live":{players[], queue[], matches[], started, mode}}`. Players: `name, pid, steamid, state (menu / queue / match), match, idle_s, queued_s, version, platform, steam_helper`; matches: `id, state (loading / running / ended), age_s, map{level,name}, net, players[{name,pid,team,left,host,ping_ms}], clock{left,duration,ended,age_s}, lives{team0,team1,age_s}`. `pid` = first 12 hex of sha256(token): never the token. |
| `POST /kick` `{"pid":".."}` | `X-Admin-Secret` | Force kick: dequeue, leave / call off the match, clear LOADED / countdown / silence marks, drop presence, send 0x0d (back to the menu) on the held socket. `{"ok":true,"name":..,"did":[..]}`. Logged as `EVT admin-kick`. |

The secret is the first line of `admin-secret.txt` next to the database (`T2R_DB`), created on the first start and never sent to a client. The same data is
also written every 10 s to `t2r-status.json` next to the database (`live` key; `T2R_STATUS_FILE=<path>` moves it, `=0` turns it off), without the secret
and without a kick: tools/update_server `--status-file` reads it too (`/status.json`).

## Part I map pack (temporary tester download, MAPS tab)
`GET <file host>/part1-maps.json` -> `{"pack":"part1-a","ver":1,"hash":"1a2b3c4d","maps":1,"total":<bytes>,"files":[{"name":"part1-a.psarc","sha256":"<64 hex>","size":N,"url":"..."},{"name":"part1-a.txt",...}]}`; exactly one `.txt` (the pack manifest the mod's ext reads), names are plain file names ending in `.psarc` / `.txt`; 404 `{"error":...}` when no pack is published. `GET <url>` serves the file with `Range`. The launcher writes the files to `<game dir>	2r-maps\` (archive first, the `.txt` last, sha256 verified, resumable). Never part of `/manifest.json`.
