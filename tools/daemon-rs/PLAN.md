# t2r-daemon: Rust rewrite of the game daemon

Goal: replace `daemon.py` (9,380 lines deployed) and the modules it imports (about 26,000 more lines, 56 `clan_*` modules among them) with one static Rust binary. The aim is less CPU and RAM on fra (1 vCPU, 961 MB, shared with the update server, web site, admin site, Discord bot and cloudflared).

The deployed copy on fra (`/home/kellie/t2r-live/host186`, build `cz1117u`) is the reference, not `tools/daemon.py`. The two differ by about 1,000 lines of `daemon.py`, and `anticheat.py` is missing from the repo. The TEST daemon (`/home/kellie/t2r-test/host-dbfix`) differs again in `daemon.py`, `clan.py`, `clan_supply.py`, `hostmig.py` and `social.py`.

## 1. Measurements on fra (2026-11-23, about 62 players online, 4 matches)

| What | Value |
|---|---|
| Daemon process | 85.9 MB RSS (98 MB peak), 140 threads, 16.5 % of the CPU over a 20 s sample, 25 to 30 % at peak |
| UDP relay thread | 46 % of the daemon's CPU (152 of 329 ticks) |
| Main thread (TCP accept and thread spawn) | 22 % |
| About 140 per-connection threads, status thread, others | the rest |
| TCP | about 240 new connections per second. The client's RPCs (polls 0xB4, 0xBF, 0xE0, 0xDE, 0xD8, 0xB0 ...) open a new connection for each request, and each gets a new Python thread. STATUS shows `polls=` 1,800 to 2,500 per 10 s, `tcp=` about 135 live |
| UDP | about 58 packets/s in (one 0x03 keepalive per player per second), about 3,500/s out. Each keepalive is answered with a pong (0x04) and also re-sent to every other keepalive socket on the server (about 60 copies). Logs show 0 `RELAY-HELLO`: every match runs Steam P2P, and gameplay goes through the relay only as a fallback |
| daemon.log | 8 MB every 16 minutes. 215k of about 250k lines are `TCP ... recv <hex>` dumps of non-poll frames (0xDE gun-limit polls are 70 % of them) |
| t2r.db | 100 MB, rollback journal. `clan_snap` is 43 MB (515 rows, about 84 KB each), `clan_feed` 27 MB, `clan_story` 18 MB. Every 5 minutes the whole file is copied (under the global DB lock) to `t2r.db.bak` and `PRAGMA integrity_check`ed, then fsynced |

Python's own baseline (measured on sfo, Python 3.12.3): bare `python3` is 10 MB RSS; importing every module the daemon uses (clan text tables included) brings it to 30 MB. The other 56 MB on fra are runtime state, about 140 thread stacks, sqlite connections (8 read-only plus the writers) and allocator fragmentation.

### Stage 1 benchmark (sfo, loopback, 60 s, the same keepalive load)

| Load | Python relay (deployed code, run by itself) | Rust relay |
|---|---|---|
| 65 players at 1 keepalive/s | 4.9 % CPU, 14.5 MB RSS | 2.3 % CPU, 0.7 MB RSS |
| 130 players | 17.3 % CPU | 9.0 % CPU |

The Python number is lower than on fra (7.6 %) because here it runs alone, with no GIL contention. The Rust cost is almost all kernel time for the n² keepalive fan-out (`sendmmsg`, 64 datagrams per call). Without the fan-out (open question 1), the relay would cost about 0.1 %.

## 2. Map of the daemon

### 2.1 Threads and timers (Python)

- **Main thread:** TCP accept loop, one thread per connection (`tcp_conn`, `handle_batch`). Caps: 512 connections, 64 per IP. Frame deadline 15 s, then 120 s idle. Flood limit 3,000 recvs per 10 s; a partial frame must complete within 30 s.
- **OutConn writer threads:** started when a socket's send buffer fills. 256 KB / 512 frames queue, 15 s stall kill.
- **udp:** the relay (stage 1).
- **status, every 10 s:**
  - STATUS line
  - `reap_matches`, `ac_tick`, `gc_tick` (every 300 s)
  - JSON table saves, `readopt_expire`, `live_save` (`live-matches.json`)
  - `net_report` (every 60 s), `ping_check`
  - UDP lines, `db_maintenance_tick`
  - `HM.tick` + `repick_tick`
  - `t2r-status.json`
  - DB backup every 300 s
- **matchmaker, every 1 s:** `shutdown_tick`, `SOCIAL.tick`, `PRIV.gc`, `MM.try_form` → `push_peers` + `start_match`, else `jip_pass`; `auto_balance_tick`.
- **clan-scheduler, every 0.25 s:** books the clan day of reports whose match end never arrived (`CLAN_DUE`, 20 s grace).
- **Admin HTTP:** a thread per request.
- **Short-lived threads and timers:**
  - countdown per formed match (1 s steps), late and join-in-progress countdowns
  - Elo update 45 s after a match end
  - end-by-reports after 8.5 s
  - match free 9 s after the end
  - signal-driven final exit (notice, 6 s wait, RST the session sockets, exit 4 = restart, 0 = stop)

### 2.2 TCP protocol

Frames are `[u16 len LE][op][payload]`. A zero length is skipped; more than 256 KB pending closes the connection. All frames of one `recv` are handled together as a batch, and the order inside a batch matters:
1. presence refresh for `EXT_PRESENCE_OPS`
2. 0x12 match end
3. 0x02 leave
4. the first matching handler group
5. the session / matchmaking / boot block

A batch that holds 0x0f without 0x01/0xfc/0xF0 waits 0.3 s for a second segment before answering.

| Op | Request | Reply | Touches |
|---|---|---|---|
| 0x0f login / profile | `[1c][tok 64][01 02 07][loadout]` | boot reply: 0x10 name + 0x08 lists + 0x16 + 0x14 (+0x0d [01] when no 0x0e) | `players.loadout`, presence, `readopt_claim` |
| 0x0e announce | `[1c][8 nonce][32 name]` | (with 0x0f) socket kept open as the session | `set_name`, `session_reattach` |
| 0x01 / 0xfc find match | name | 0x03 + 0x17 searching list, or 0x11 refusal (+0x0d [02] banned) | `MM.enqueue`, sessions, party / version / ban / helper gates |
| 0x02 leave | – | others: 0x03 + 0x17 | `MM.cancel`, `leave_running_match` |
| 0x05 keepalive, 0x15 poll | – | 0x16 online count; 0x10 name change; 0x17 party lobby; 0x04 match record once; 0x0c countdown steps | `MM.poll` |
| 0x13 leaderboard | – | 0x14 `[n][n × 0x30]` + 1 byte, cached 5 s | `players`, `ratings` |
| 0x09 loaded | – | 0x0a [01] to all when everyone has loaded | `add_matches`, clan match start, HM/INT start, `live_save` |
| 0x12 match end (host) | `[winner]` | – | `record_result`, Elo after 45 s, `match_stats`, free after 9 s |
| 0xF0 Steam helper | `[tok][u64 sid][nlen][name][u16 port]` | 0xF1 | `set_steam`, BRIDGES, `HM.bridge_back` |
| 0xF3 / 0xF4 / 0xF6 (helper) | keepalive / ping location / ping ms | 0xF5 forwards a location | `PING_LOC`, REGION, PING |
| 0xD0 / 0xD2 names | `[tok]` / `[tok][name][tag]` | 0xD1 | `names` |
| 0xE0 / 0xE2 / 0xE4 clan | read / match report / side | 0xE1 | clan engine |
| 0xE8 / 0xEE / 0xEF boosters, missions | | 0xE9 | clan engine |
| 0xB4 menu feed | `[tok][lang]` | 0xB5 `n=k\nid|text` (cached 20 s) | `clan_story.menu_lines` |
| 0xB6 clan page | | 0xB7 | `clan_ingame.build` |
| 0xBF rank table | `[tok]` | 0xBF `n=k HHHHHHHH:rank:weeks[:role]` | 300 newest players, rank, roles |
| 0xA9 / 0xAA / 0xAB achievements | report / read / live | same op | `ach`, `ach_count` |
| 0xE3 roster / clock | `[tok][flags][f32 left][f32 dur][team][lives][lives][0xAF][u16 idle][afk]` | 0xE5 roster + wave byte (+0xA7 team swaps) | MM clock / lives, AFK kick |
| 0xE6 host migration | `[tok][flags][epoch]` | 0xE7 | HM |
| 0xB2 HWID | `[tok][1][mask][12 B × n]` | 0xB3 | `hwids` |
| 0xB0 version | `[vlen][ver][tok][opt 'L']` | 0xB1 `ok= server= client= msg=` | `client_versions.json`, `linux_clients.json` |
| 0xBA notices | `[tok][ver][lang][flags]` | 0xBB `n=k\nid|kind|param|text` | notices, social notes |
| 0xF8 private lobby | | 0xF9 `ok=0 err=off in=0` (off live) | – |
| 0xDA party (old) | `[tok][sub][name]` | 0xDB | PARTIES |
| 0xD8 friends / party | `[tok][1][lang][sub][arg]` | 0xD9 JSON | `social_*` |
| 0xDE gun limits | `[tok][u16 mask]` | 0xDF | GL |
| 0xBC mode queue | `[tok][sub][mode]` | 0xBD | MODEQ |
| 0xDC Interrogation | `[tok][sub][u16 seq][args]` | 0xDD | INT |
| 0xCA performance | `[tok][1][...] + trailers` | 0xCB `ok=1` | PERF, `host_cap`, `oneway` |
| 0xC8 SP-map support | `[tok][u32 mask][flags]` | 0xC9 | SPSUP |
| 0xC6 map packs | | 0xC7 `on=0` (off live) | – |
| 0xEA / 0xEC map vote | `[tok]` / `[tok][option]` | 0xEB | VOTES |
| 0xCC / 0xCE / 0xD4 / 0xD6 anti-cheat | flags / demo chunk / report / recent | 0xCD / 0xCF / 0xD5 / 0xD7 | `review/ac.db`, `review/demos/` |

**Server-pushed frames:**
- 0x04 match record: relay IP, port, session id = CRC32 of the tokens, map, slot, roster of 0x32-byte entries
- 0x0c countdown steps: (1,0), 5..1, vote 160+n, loadout 100+n, booster 130+n, 1, 0
- 0x0a game start, 0x0d close / ban / back to menu, 0x11 note, 0x03 back to searching, 0x17 name list
- 0xF2 Steam P2P slot table, 0xF5 ping location, 0x16 online count

**UDP:** 0x03 + seq → 0x04 + seq, plus the keepalive fan-out; 0x00 hello `[sid][slot]` → 0x01; 0x02 data `[sid][src][dst]` relayed by slot (0xFF = all); anything else → the "GP" fan-out.

### 2.3 State machines

- **Presence:** TTL 15 s from TCP traffic. UDP pings only feed a counter.
- **Matchmaker:** queue → form (fill rule "fast": 8 s steps, 40 s max, 15 s alone; party holds 120 s; region / ping gates; mode queues) → countdown with map vote → launched → loaded → running → ended → freed.
  - Join in progress: clock reports, wave respawn, slot reuse 10 s.
  - Requeue tickets after a match.
  - Reaper: offline 60 s, depart 120 s, load timeout 90 s, prelaunch silence 25 s, match max age 4 h, ended hold 30 s.
- **Host migration** (`hostmig`, mode 2 = planned handover): host pick by capacity / ping / region, repick, bridge gone / back, ghost cleanup.
- **Teams:** `team_rules.txt` groups, parties, skill (parts per match), top players, auto-balance every 90 s, admin moves.
- **Clan day:** report (0xE2) → wait for the match end or 20 s → `clan_finalize`. It runs snapshot / rebook, `apply_match`, `clan_story.on_day`, `clan_perks.on_day` and achievements. Excuses (left, kicked, link loss, restart), late join, rejoin.
- **Restart survival:** `live-matches.json` saves running matches, clan bookkeeping, gun limits and Interrogation every 10 s. A restarted daemon re-adopts a match when its players come back (claim within 90 s).

### 2.4 Files

The DB dir holds:
- `t2r.db` (+ `.bak` every 5 min)
- `t2r-status.json` (every 10 s, read by the update server and the admin site)
- `live-matches.json`
- `client_versions.json`, `linux_clients.json`, `client_langs.json`
- `admin-secret.txt`
- `bans.txt`, `no_host.txt`, `og-steamids.txt`, `web-admins.txt`, `devs.txt` (re-read on mtime)
- `clan-resets/`
- `review/ac.db`, `review/demos/`

The update-data `channels.json` + manifests (target build) are read every 30 s.

The host dir holds `version.txt`, `team_rules.txt` (rewritten by admin POST /teams), `interrogation_lockboxes.json`, `top_players.txt`, `fill_rule.txt` and `daemon.log*`.

### 2.5 Admin listener (127.0.0.1:7011, header `X-Admin-Secret`)

GET:
- `/status` (no secret)
- `/live`, `/notices`, `/sizes`, `/teams`
- `/clan/backups[?steamid=]`

POST:
- `/kick`
- `/notice`, `/notice/clear`
- `/shutdown`, `/shutdown/cancel`
- `/boot-all`
- `/teams`, `/teams/move`
- `/handover`, `/reset-session`
- `/clan/reset`, `/clan/restore`

The JSON shapes are in the deployed `admin_server` (`daemon.py` 9221) and must stay byte-compatible: the admin site parses them.

### 2.6 Database

**Daemon tables:** `players`, `hwids`, `ratings`, `pstats`, `pweapon`, `names`, `social_friends`, `social_seen`, `social_msgs`, `host_cap`, `oneway`, `ach_count`, `ach`.

**Clan tables:**
- `clans` (+ `boost_hi`, `meta` JSON), `boosters`, `clan_snap`
- `clan_story` (state JSON), `clan_feed` (+ `lid`, `params`), `clan_journal`, `clan_alife`
- `match_stats`

**Writers:** the daemon is the only writer except for the admin site's clan reset (through the daemon). The launcher, update server and web site only read.

**JSON byte format:**
- compact `(",", ":")` with `ensure_ascii=True`
- except `clan_feed.names`, which uses the default `", "` separators
- and `clan_feed.params` / `clan_journal.data`, which use `ensure_ascii=False`
- floats as Python `repr`

**Connections:** `synchronous=FULL`, one global write lock, a pool of 8 read-only connections.

### 2.7 Clan engine (write side)

- **Entry points:**
  - `clan.load` / `ensure` / `choose_side` / `apply_match` / `snap_*` / `booster_*` / `mission_choose` / `meta_text` / `state_text`
  - `clan_story.on_new_run` / `on_day` / `menu_lines` / `ensure_roster` / `_load`
  - `clan_perks.on_day` / `wire_text` / `active` / `loss_streak`
  - `clan_status.wire_text`
  - `clan_ingame.build` / `player_rank` / `stats_*`
  - `achievements.*`
  - `clan_reset.*`
- **Size:** about 11,300 lines of logic and 10,300 lines of text tables. Every module is reachable, and every feature switch is on.
- **Randomness:**
  - The day outcome uses an unseeded `random.Random()` (`clan.rng`); `meta_text` uses the global `random`.
  - Everything else is seeded: string seeds (CPython version-2 seeding = SHA-512 of the string), integer CRC32 seeds, and `crc32("|".join(...))` hashes.
  - `choice`, `choices` (weights, bisect over float sums), `randint`, `randrange`, `shuffle`, `sample` (two algorithms), `random`.
  - The chronicle and saga seeds use `st.get("tid")`, which is never set, so every clan draws the same sequence on the same day. A port must keep this.
- **Time:** `slot_of() = now // 60` picks the ticker lines; feed rows younger than 900 s are "fresh"; stored timestamps use `round(now, 3)`.

### 2.8 Overlap with `t2r_clan` (update-rs branch)

`tools/clan-rs` renders the clan card and `menu_lines` for the launcher: the read side. The daemon needs the same `menu_lines` (op 0xB4) and the whole write side. Plan:
- port the shared pieces once: Python `random`, the template renderer, the text tables, `menu_lines`
- put them in a library crate that both binaries use (`tools/clanstory-rs`, crate `t2r_clanstory`)
- when the branches merge, `t2r_clan` keeps the card and calls the shared `menu_lines`

Until then, golden tests for `menu_lines` run against the Python on the same rows, so either crate can be checked against the other.

State on 2026-11-23: `update-rs` commit 861a4e5 has `t2r_clan` with:
- its own `pyrand.rs`: string seeds only; `random` / `randbelow` / `choice` / `shuffle` / single weighted `choices` / `sample_indices`;
- a template renderer (`text.rs`) and `menu_lines` (`story.rs`, `alife.rs`);
- tables exported by `gen/export.py` to `data/clan.json`.

`t2r_clanstory::pyrand` is a vector-tested superset: integer seeds, `getrandbits`, `randint`, `randrange` with step, `choices` with k, and both `sample` algorithms. At merge time:
1. `t2r_clan` switches to it;
2. the write-side engine reuses `t2r_clan`'s renderer and `clan.json` export, extended with the tables only the write side needs;
3. there is one `menu_lines`, the one in `t2r_clan`.

## 3. Target architecture

One binary, `t2r-daemon`, built static musl with lto, `panic=abort`, `codegen-units=1`, strip. No `unwrap` on input: a panic would drop every player.

- **UDP thread:** owns the relay state. A blocking `recv_from` with a 0.5 s timeout runs the 10 s stat lines; the fan-out goes through `sendmmsg`. Done (stage 1).
- **Game thread:** a single-threaded event loop (tokio `current_thread`). It owns all matchmaking, session, presence, vote, host-migration and team state in plain structs: no locks, no GIL. Connections are tasks.
  - A short-lived RPC connection costs one task and one small buffer instead of a Python thread.
  - Timers (countdowns, the 1 s matchmaker tick, the 10 s status tick, delayed Elo) are tasks too.
- **DB thread:** one rusqlite connection that serialises writes like `_dblock`, with the same pragmas. Hot reads (name, Steam id, known token, rank) come from in-memory caches that writers update, instead of being re-read with a 120 s TTL. The clan engine runs here: its work is DB-bound and must be serial per clan.
- **Logger thread:** the same line formats, rotation, header lines and per-category rate limit as Python (done in stage 1).
- **Admin listener:** on the game runtime (loopback HTTP/1.0, the same JSON).

Expected effect: about 1 to 3 MB of code and data plus the state. The text tables stay as `&'static str` in the binary instead of Python objects. CPU becomes syscall-bound. The thread-per-connection and GIL costs disappear.

## 4. Staged migration

Every stage ships behind a switch that falls back to Python. Every stage is verified side by side against the deployed Python before it touches fra. Nothing on fra changes without the user's OK.

| Stage | Content | Verification | Deploy (each needs the user's OK) |
|---|---|---|---|
| 0 | This map and plan | – | – |
| 1 (done, deployable) | UDP relay: `t2r-daemon relay` | `verify/udp_compare.sh`: generated traces run through the deployed `UdpRelay` source and the Rust relay. Every datagram sent and every log line identical: 10 runs, 16k packets and about 615k sends each, every branch hit. Benchmark above | Python needs a 3-line switch to skip its UDP thread (`T2R_UDP=0`, below). Run `t2r-daemon relay` next to it on the same port number (UDP vs TCP do not clash). TEST daemon first (7101), then live. Rollback: drop the env var, stop the relay |
| 2 (partly done) | Foundations, in `tools/clanstory-rs`. Done and checked against CPython 3.12.3 vectors: `random` (about 3,900 checks) and `json.dumps` with float `repr` and CRC32 (about 13,000 checks). Still to do: the DB layer. Covered: CPython `random` (MT19937, seeding, `_randbelow`, `choice` / `choices` / `sample` / `shuffle` / `randint`), Python-compatible JSON writer (separators, `ensure_ascii`, float `repr`), CRC32, the DB layer with the same schema and pragmas | Unit tests against vectors generated by CPython 3.12 | – |
| 3 | Clan engine as `t2r_clanstory`: (a) the text tables converted by a script from the deployed Python, so the text stays byte-identical; (b) `menu_lines` and the clan page (read side); (c) `apply_match` / supply / perks / status / boosters / missions; (d) story, A-Life, chronicle, arcs, saga, loot, journal, continuity; (e) achievements, `clan_reset` | Golden tests: a Python harness with `clan.rng` seeded and the time frozen plays N days of random reports for synthetic clans; Rust plays the same reports on the same DB and must produce identical rows (`clans`, `boosters`, `clan_snap`, `clan_story`, `clan_feed`, `clan_journal`, `clan_alife`, `match_stats`, `ach*`) and replies. Where exact story text proves impractical: stop and ask | – (runs inside stage 4) |
| 4 | TCP core: framing, boot / session / search, matchmaker, countdown + vote, match flow, join in progress, host migration, teams, reaper, social / party, notices, mode queues, requeue, gun limits, Interrogation, anti-cheat, names, versions, HWID, bans, `live-matches.json` (the same format, so a switch-over can re-adopt running matches either way), `t2r-status.json`, admin listener, log lines | A Rust test client (bot) that speaks the client protocol runs scripted scenarios (boot, search, match of 2 to 8, leave, kill, rejoin, host loss, vote, party, notices, admin calls) against the Python daemon and the Rust daemon on sfo with identical env and DB copies. The transcripts (frames each bot received, admin JSON, status file, DB rows, EVT lines) are diffed. `secrets` choices (map rotation, vote ties, match ids) are pinned through a test hook on both sides | – |
| 5 | Shadow load on sfo: 70 to 150 bots playing matches through the Rust daemon for hours | RSS and CPU over time; no panics; log parsed by the admin site's `logindex.py` | – |
| 6 | TEST daemon on fra runs the Rust binary | User and testers play on 7101 | `run-daemon.sh` of t2r-test swaps the command; rollback is one line |
| 7 | Live cutover at a quiet hour | `live-matches.json` keeps running matches across the switch; watch STATUS / EVT | `run-daemon.sh` swaps the command; the Python host folder stays in place as the rollback |

### Stage 1 deployment detail (prepared, not applied; steps in `deploy/README.md`)

Verified end to end on sfo with the deployed `host186` code, the switch applied and `T2R_UDP=0`:
- Python listened on TCP only and the Rust relay on UDP of the same port.
- The boot handshake reply was unchanged.
- Pongs and fan-out came from Rust.
- Idle footprint: Python daemon 55 MB RSS (empty DB), relay 0.66 MB.

1. In the deployed `daemon.py`, start the UDP thread only when `T2R_UDP` is not `0` (`deploy/apply_udp_switch.py`).
   Nothing on the TCP side reads relay state: `PEERS` and `Presence.udp_seen` are only written and counted, and `RELAY` is UDP-only while `T2R_RELAY_STRICT` is off (it is). The status loop already skips the UDP lines when `UDP is None`.
2. Copy `t2r-daemon` to `~/t2r-live/bin/`. Start it from `run-daemon.sh` before Python:
   `T2R_LOG_PATH=$HOME/t2r-live/<host>/relay.log ~/t2r-live/bin/t2r-daemon relay &`
   or give it its own systemd unit `t2r-relay.service`, which is cleaner. The `UDP-*` and `RELAY-*` lines then go to `relay.log`; the admin site reads none of them.
3. TEST daemon first, live after a day of play.

## 5. Dropped or changed (proposed)

Dropped because nothing live uses them. Each needs the user's agreement before the live cutover:
- `T2R_RELAY_STRICT` (off live, would need matchmaker state in the relay thread)
- the SIM lag/jitter/loss simulator and `local/sim-off.txt` (test only)
- Windows-only paths: `SO_EXCLUSIVEADDRUSE`, console handler, `SIGBREAK`, ICMP reset handling on UDP
- `merge_dbs` / `--merge-db` (a one-off playtest tool)
- `Presence.udp_seen` and `PEERS` (written, never read)
- the private lobby subsystem (`T2R_PRIVATE=0` since 11-19; 0xF8 keeps answering `ok=0 err=off in=0`)
- the map-pack registry (no registry file; 0xC7 keeps answering `on=0`)
- `fill_rule.txt` and the "classic" fill rule (live uses `fast`)
- never-called code: `lobby_snapshot`, `build_peer_table`, `get_loadout`, `frames`, `notice_english`, `banned_hwids`, `UPDATE_REFUSAL_NOTE`, `clan_story.pick_for_widget` / `pick_line` / `latest` / `journal`, `clan_perks.LINES` / `line`, `clan.event_name` / `attack_tier` / `tier_reward`, `FEED_SEEN`
- the second clan-scheduler start in `clan_report`

Changed for cost, with the output kept the same:
- one task per connection instead of one thread
- in-memory name, Steam id and rank caches updated by the writers instead of a 120 s TTL
- `clan_op` loads the story state once instead of twice
- `state_text` computes rank and meta once
- the clan page and feed caches stay

Kept although they look odd (behaviour other components or players may rely on):
- the keepalive fan-out (question 1)
- the `tid`-less chronicle and saga seeds
- reads that write (0xE8 mission roll, 0xB4/0xB6 creating story rows, `clan_key` adoption)
- the `players.kills` / `deaths` / `loadout` columns: never read by the daemon, but other readers may use them

## 6. Open questions for the user

1. **Keepalive fan-out:** each 0x03 keepalive is re-sent to every other player's keepalive socket, about 3,500 datagrams/s at 60 players, growing with players². The client's netdiag only counts 0x04 pongs, and every match runs Steam P2P. Can the fan-out be dropped? That takes the relay from about 2 % to about 0.1 % CPU and cuts about 0.5 MB/s of upload.
2. **The 0xDE gun-limit poll log lines:** they are 70 % of `daemon.log`, which rotates every 16 minutes. Can 0xDE join the "noisy" (counted, not logged) ops, as 0xE3 and 0xE6 already are?
3. **DB backup:** every 5 minutes the 100 MB DB is copied under the write lock and `integrity_check`ed. Options: a `quick_check`, a longer interval, or trimming `clan_snap` (43 MB: one 84 KB snapshot of every clan, kept for rebooking a day). Which is acceptable?
4. **Test data:** golden tests for the clan engine use synthetic clans generated by the Python engine itself. A sample of real clans would catch more, but the DB holds player tokens. Is an anonymised extract (tokens and Steam ids replaced by hashes, made on fra into `/tmp` and copied to sfo) acceptable?
5. **TEST daemon:** it runs `host-dbfix` code that differs from live in 5 modules. Which one should the Rust port follow where they differ: live (the default here) or TEST?
