# t2r-files

The whole Twelve Weeks update server in one static binary: it replaces `tools/update_server/server.py` (and the earlier t2r-files that only served blobs and proxied the rest to that Python). Clan views come from the shared crate `tools/clan-rs` (`t2r_clan`).

| Endpoint | |
|---|---|
| `GET/HEAD /manifest.json?channel=&version=` | manifest with `url` / `zurl` per file, `launcher`, `launcher_linux`; 503 while a blob is missing |
| `GET/HEAD /files/<sha256>[.gz\|.br]` | blobs by `sendfile`, single Range |
| `GET /download/<name>` | named downloads (`downloads.json`) |
| `GET /media/<name>` | news media, `Cache-Control: public, max-age=86400` |
| `GET /part1-maps.json`, `/part1-files/<sha256>` | Part I pack for the Steam ids in `part1-eligible.txt` |
| `GET /news.json`, `/versions.json`, `/status.json` | `/status.json` callers are counted into `<status dir>/t2r-launchers.json` every 15 s |
| `GET /ban-check?h=&s=&t=` | launcher ban screen (`bans.txt` next to the db) |
| `GET /clan.json?player=`, `/clanfeed.json?player=&lang=` | clan card and the menu lines the game shows (`t2r_clan`) |
| `POST /report`, `PUT /report-clip?code=&key=` | bug reports into `<reports>/<date>_<code>_<player>/`, daemon log lines added 5 s later |

## Flags

Same as `server.py`: `--data`, `--host`, `--port`, `--public-url`, `--status-file`, `--db`, `--reports` (default `<data>/reports`, empty = reports off), `--daemon` (accepted, ignored). Also `--listen host:port` (instead of `--host`/`--port`), `--daemon-log-dir` (default `$HOME/t2r-live/host186`), `--max-conns` (1500), `--rate-mbit` (0 = unlimited), `--stats-secs` (60), `--backend` (accepted, ignored).

Environment, as before: `T2R_REPORTS_MAX_GB` (20), `T2R_REPORTS_MIN_FREE_GB` (10), `T2R_DAEMON_LOG_DIR`, `T2R_DAEMON_ARCHIVE_DAYS` (3), `T2R_LAUNCHER_WINDOW_S` (75).

Logs go to stderr: one stats line per `--stats-secs`, plus the same event lines as the Python (`report R-XXXX stored in ...`, `ban-check: BANNED ...`, `manifest ...: N blob(s) missing`). There are no per-request access lines; the HTTPS side is in Caddy's access log.

## Differences from server.py (intentional)

- `/clanfeed.json` answers only `ok, hasClan, lang, faction, factionName, status, daysDone, menu, slot`. The journal `feed`, the camp roster (`alive`, people ...), `alife` and `chron` blocks are gone: launchers 0.1.37-0.1.51 read only `ok`, `hasClan` and `menu`.
- `/clan.json` and the `menu` lines follow the clan modules the daemon runs (host186), not the older copies in the update server's `tools/`. Today that changes event names (`FRIENDLY SURVIVORS`, `INFECTED HORDE`, `SPOILED SUPPLIES` ...), mission names, and the menu now has the daemon's A-Life lines, so the launcher shows what the game shows.
- No `--daemon` probe: without `--status-file`, `/status.json` says offline.
- `/part1-files/<sha>.gz` no longer falls back to the main blob folder (404).
- Unsupported methods get a JSON 501 instead of Python's HTML page.
- JSON is sent as UTF-8 instead of `\u` escapes (same values).
- A report zip whose entry fails to unpack gets `400 not a zip file` instead of a dropped connection.

## Build

Static musl, so the glibc version of the server does not matter. From WSL (Arch) or any Linux with the `x86_64-unknown-linux-musl` target and `musl-gcc`:

```sh
cd tools/files-rs
CC_x86_64_unknown_linux_musl=musl-gcc cargo build --release --locked --target x86_64-unknown-linux-musl
ls -la target/x86_64-unknown-linux-musl/release/t2r-files     # ~5.8 MB, static-pie
cargo test --release                                          # unit tests (any host)
```

## Deploy on fra (live)

Today `t2r-files.service` (port 7002, Caddy proxies 443 to it) forwards everything but blobs to `t2r-update.service` (Python, 127.0.0.1:7003). The new binary answers everything itself, so the Python service stops.

1. Copy the binary up and keep the old one for rollback:

   ```sh
   scp target/x86_64-unknown-linux-musl/release/t2r-files <ssh-user>@play.twelve-weeks.com:/tmp/t2r-files.new
   ssh <ssh-user>@play.twelve-weeks.com
   cd /home/kellie/t2r-live
   sudo cp -p bin/t2r-files bin/t2r-files.old
   sudo install -o kellie -g kellie -m 755 /tmp/t2r-files.new bin/t2r-files.new
   sudo -u kellie bin/t2r-files.new --listen 127.0.0.1:7012 --data update-data --db data/t2r.db --status-file /tmp/t2r-check/t2r-status.json --reports '' &
   curl -s 127.0.0.1:7012/manifest.json | head -c 300; curl -s 127.0.0.1:7012/news.json | head -c 100; kill %1
   ```

   (`/tmp/t2r-check` only keeps the trial instance from writing the live `t2r-launchers.json`; `mkdir -p /tmp/t2r-check && cp data/t2r-status.json /tmp/t2r-check/` first.)

2. Point the unit at the new flags. `sudo cp /etc/systemd/system/t2r-files.service /etc/systemd/system/t2r-files.service.bak-rs`, then `sudo systemctl edit --full t2r-files` and set:

   ```ini
   [Unit]
   Description=Twelve Weeks update server (t2r-files, Rust): blobs, launcher API, clan views, bug reports
   After=network-online.target
   Wants=network-online.target

   [Service]
   Type=simple
   User=kellie
   WorkingDirectory=/home/kellie/t2r-live
   Environment=T2R_REPORTS_MIN_FREE_GB=3
   ExecStart=/home/kellie/t2r-live/bin/t2r-files --listen 0.0.0.0:7002 --data /home/kellie/t2r-live/update-data --db /home/kellie/t2r-live/data/t2r.db --status-file /home/kellie/t2r-live/data/t2r-status.json --daemon-log-dir /home/kellie/t2r-live/host186 --max-conns 1500
   StandardOutput=append:/home/kellie/t2r-live/files.out
   StandardError=append:/home/kellie/t2r-live/files.out
   Restart=always
   RestartSec=2
   LimitNOFILE=65536

   [Install]
   WantedBy=multi-user.target
   ```

   (`T2R_REPORTS_MIN_FREE_GB=3` comes from `run-update.sh`; reports keep going to `update-data/reports`.)

3. Switch over (a second or two without the update server):

   ```sh
   sudo mv /home/kellie/t2r-live/bin/t2r-files.new /home/kellie/t2r-live/bin/t2r-files
   sudo systemctl daemon-reload
   sudo systemctl restart t2r-files
   sudo systemctl stop t2r-update && sudo systemctl disable t2r-update
   ```

4. Check:

   ```sh
   tail -3 /home/kellie/t2r-live/files.out
   curl -s https://play.twelve-weeks.com/status.json | head -c 200
   curl -s 'https://play.twelve-weeks.com/manifest.json?channel=stable' | grep -o '"url":"[^"]*' | head -2    # https:// links
   cat /home/kellie/t2r-live/data/t2r-launchers.json                                                        # "open" > 0 after a minute
   ps -o rss,cmd -C t2r-files
   ```

Rollback: `sudo cp /etc/systemd/system/t2r-files.service.bak-rs /etc/systemd/system/t2r-files.service && sudo cp -p /home/kellie/t2r-live/bin/t2r-files.old /home/kellie/t2r-live/bin/t2r-files && sudo systemctl daemon-reload && sudo systemctl enable --now t2r-update && sudo systemctl restart t2r-files`.

## TEST instance

`t2r-test-update.service` runs `~/t2r-test/run-update.sh` (port 7102, the test db and status file, live's update-data). Replace its loop body with the binary (same flags as before; reports stay in `<data>/reports` as with the Python):

```sh
#!/bin/sh
cd $HOME/t2r-live
while true; do
  $HOME/t2r-live/bin/t2r-files --port 7102 --data $HOME/t2r-live/update-data --db $HOME/t2r-test/data/t2r.db --status-file $HOME/t2r-test/data/t2r-status.json >> $HOME/t2r-test/update.out 2>&1
  sleep 3
done
```

then `sudo systemctl restart t2r-test-update`.

## Measured (sfo, 1 vCPU / 961 MB, same data as fra on 2026-11-23)

5000 requests in the live mix (status 40 %, manifest 25 %, clanfeed 20 %, news 7 %, ban-check, clan), 8 concurrent clients:

| | Python server.py | t2r-files |
|---|---|---|
| RSS idle | 23 MB | 4.7 MB |
| RSS after the run / peak | 51 MB / 53 MB | 16 MB / 19 MB |
| server CPU per request (mix) | 14.2 ms | 2.7 ms |
| CPU: status / manifest / news | 0.77 / 3.6 / 8.4 ms | 0.05 / 2.2 / 0.4 ms |
| CPU: clanfeed en / pl | 45 / 50 ms | 4.8 / 5.1 ms |
| CPU: clan / ban-check | 3.5 / 2.0 ms | 1.2 / 0.9 ms |
| wall time for the run | 92 s | 19 s |

The live Python process on fra sat at 54-71 MB RSS.
