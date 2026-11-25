# t2r-admin (Rust admin site)

A single static binary that replaces `tools/admin_site/admin.py` + `store.py` + `logindex.py`. It takes the same flags, serves the same
`tools/admin_site/static/` files unchanged, and answers every URL the pages use (`admin.js`, `clanframe.js`, `newsframe.js`) with the same JSON.
Login (`admin-users.txt`, the website's `tw_s` Steam cookie, `web-admins.txt`), the `--tunnel-host` check, the LIVE / TEST switch and every
admin action (kick, ban / unban, notices, restart, team rules, clan reset / restore, report handled, patch notes) work as before.

The database is opened read-only (`SQLITE_OPEN_READ_ONLY`, `query_only`). The only writes are the ones admin.py makes:
`<db dir>/bans.txt` (+ `.bak-admin`), `<db dir>/admin-actions.log`, a report's `meta.json` (handled flag), `<update-data>/news.json`
(+ `news-backups/`). Everything else goes to the daemon's loopback admin listener.

## Flags

Same as admin.py: `--db` (required), `--port 7010`, `--bind 127.0.0.1`, `--daemon-log <file | auto | auto:<dir>>`, `--reports`, `--daemon`,
`--admin-url`, `--status-file`, `--update-data`, `--users`, `--tunnel-host` (repeatable), `--allow-host` (repeatable), `--web-secret`,
`--web-admins`, `--site-url`, `--test-db`, `--test-admin-url`, `--test-daemon-log`, `--test-daemon`.

Two new ones, because the binary no longer lives next to the files it serves (both relative to the working directory, which `run-admin.sh` sets
to `~/t2r-live`):

- `--static` (default `tools/admin_site/static`): the page files.
- `--launcher` (default `release/launcher`): the launcher's `src/` and `assets/` the CLAN and PATCH NOTES frames reuse (`/launcher/...`).

## Build

In WSL (Arch, with the `rust-musl` and `musl` packages; the binary is static, so it runs on fra's Ubuntu regardless of its glibc):

```
cd tools/admin-rs
CC_x86_64_unknown_linux_musl=musl-gcc cargo build --release --target x86_64-unknown-linux-musl
cargo test --release --target x86_64-unknown-linux-musl
```

The binary is `target/x86_64-unknown-linux-musl/release/t2r-admin` (about 5 MB).

## Deploy on fra

1. Copy the binary up and install it next to `t2r-files`:
   ```
   scp target/x86_64-unknown-linux-musl/release/t2r-admin <ssh-user>@play.twelve-weeks.com:/tmp/
   ssh <ssh-user>@play.twelve-weeks.com 'sudo install -o kellie -g kellie -m 755 /tmp/t2r-admin /home/kellie/t2r-live/bin/t2r-admin'
   ```
2. Change only the command in `run-admin.sh`, keeping a backup:
   ```
   ssh <ssh-user>@play.twelve-weeks.com 'cd /home/kellie/t2r-live && sudo -u kellie cp -p run-admin.sh run-admin.sh.bak-py &&
     sudo -u kellie sed -i "s|python3 -u tools/admin_site/admin.py|bin/t2r-admin|" run-admin.sh && grep t2r-admin run-admin.sh'
   ```
   The line becomes `bin/t2r-admin --db $HOME/t2r-live/data/t2r.db --daemon-log auto:$HOME/t2r-live ... >> $HOME/t2r-live/admin.out 2>&1`.
3. Restart and check:
   ```
   ssh <ssh-user>@play.twelve-weeks.com 'sudo systemctl restart t2r-admin && sleep 2 && tail -3 /home/kellie/t2r-live/admin.out &&
     curl -s -H "Host: 127.0.0.1:7010" http://127.0.0.1:7010/api/server && ps -o rss,cmd -C t2r-admin'
   ```
4. Roll back: `sudo -u kellie cp -p run-admin.sh.bak-py run-admin.sh && sudo systemctl restart t2r-admin`.

## What was dropped, and why

- **Clan A-Life section** of the player page (`alife` is `null`, so `admin.js` hides it). It needed the ~17k-line Python story engine,
  and its texts come from the clan modules admin.py imports, which are older than the daemon's. The user decided this on 2026-11-23.
  The player fields `people`, `alive` and `alifeServerOn` are back (read from `clan_story` state by the shared `t2r_clan` crate).
- **Clan iframe** (`/api/clanview`): it gets the clan card and the menu lines from the shared `t2r_clan` crate (`tools/clan-rs`, the same code
  the Rust update server uses). Its SURVIVORS / RAIDS / JOURNAL tabs stay empty, by the same decision; the launcher's `clan.js` handles the
  missing blocks. The crate follows the clan modules the daemon runs (`host186`), not the older `tools/clan.py` admin.py imported. So event and
  mission names now match what players see in game, e.g. `DYSENTERY OUTBREAK` where admin.py said `RESCUED HOSTAGES`. Of 564 clan cards
  compared, those names are the only differences.
- `textPl` on the player's clan feed rows is back, rendered by the `t2r_clan` crate (the daemon's line set, arc steps from
  `clan-rs/data/arcs.json`). Rows written by newer templates are empty in admin.py (its older registry does not know them), so
  some rows differ: the Rust side is the one that reads them.
- `/bg/<name>.jpg`: nothing requests it. The clan page loads its backgrounds from `/launcher/assets/bg/`.
- The `snapshot-counts` data source: it is only for daemon builds whose `t2r-status.json` has no `live` block. The live and test daemons both
  write it.
- The `clan_feed` query fallback for databases without the `lid` / `params` columns (old schema).
- `uniqueCode` in `/api/reports` (on `main` only, never deployed): it appended a random token to `reports.txt` in the working directory on
  every listing, and no page reads it.
- `load_timeout` and `reporter` in the log's match reconstruction: they were never shown.

## Intended differences from admin.py

- `EVT left` lines of players whose name has a space: admin.py took the first word as the name (`Skull Taffy left (0x02)` gave the name
  `Skull`), so in the log-only live view those players stayed "in the match" after leaving. The name is now matched against the match's
  player list.
- Launcher files and report images got two `Cache-Control` headers (`no-store` and `max-age=...`), so browsers never cached them. Now they
  get only the intended `max-age`.
- A report file name with a path separator is refused. admin.py joined any listed name into a path.
- Inputs that made admin.py answer 500 for a whole page now degrade per item: a `meta.json` that isn't an object, a `last_result` that isn't
  an object, a `daemon version` log line without a version. A POST body that is valid JSON but not an object gets 400 instead of 500.
- The kick / team-move player id no longer accepts a trailing newline.
- The failed-login counter forgets addresses after 10 minutes (admin.py kept them forever). Connections that don't send their headers within
  30 s are closed.

## Faster, smaller

- The daemon log (~90 MB at start, ~30 MB/h added) is parsed once into compact events. Requests no longer copy the event list, and matches
  are re-derived only when new events arrive.
- The player list and the clan list are rebuilt only when `t2r.db` or the client json files change, and at most every 4 s / 10 s as before.
  Bug report metadata is cached per file mtime. `whoami` looks up the Steam id directly.
- The first log load runs at startup, in the background.

## Comparison with admin.py

`compare/run.sh` copies fra's data twice, runs the deployed admin.py and this binary side by side, and fakes the daemon's admin listener
(`fakedaemon.py`). It then compares every endpoint in three modes: listener up, listener down with a fresh `t2r-status.json`, and log only.
It also covers the TEST server switch, every login path, POST validation, and the files ban / unban / report-handled / news write.

```
ADM_SRC=<folder with t2r-live and t2r-test copied read-only from fra> bash compare/run.sh
```

Result on fra's data of 2026-11-23 (924 players, 739 clans, ~90 MB of daemon logs, 10 bug reports): all 2,123 checks give identical
responses, apart from the dropped fields above.

## Measurements

`compare/load.py` on sfo, a droplet the same size as fra (1 vCPU / 961 MB), with fra's data. It polls the way open admin tabs do (overview +
online + one page every 3 s per tab), runs for 5 minutes, and appends to `daemon.log` at fra's rate (~30 MB/h).

| | admin.py | t2r-admin |
|---|---|---|
| first request after start (full log load) | 5.7 s | 0.14 s (log loaded at startup, 1.3 s CPU) |
| RSS at start / after 5 min / peak, 1 tab | 87 / 96 / 118 MB | 12 / 17 / 21 MB |
| CPU, 1 tab | 7.2 % | 0.95 % |
| latency p50 / p95, 1 tab | 221 / 1455 ms | 28 / 72 ms |
| RSS after 5 min / peak, 3 tabs | 117 / 118 MB | 18 / 24 MB |
| CPU, 3 tabs | 17.5 % | 2.6 % |
| latency p50 / p95, 3 tabs | 272 / 1954 ms | 54 / 137 ms |

The binary uses musl's own allocator. mimalloc saved a few ms per rebuild but doubled RSS (~36 MB), which is the wrong trade on this box.
