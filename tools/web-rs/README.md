# t2r-web

The twelve-weeks.com server (`tools/web/web.py` + `news_admin.py` + the clan view it used) as one static Rust binary. Same URLs, status codes,
JSON shapes, `tw_s` session cookie (byte-identical HMAC, so the admin site keeps validating it), Steam login and news editor.

```
t2r-web --db <t2r.db> --news <news.json> [--site site] [--public-url https://twelve-weeks.com]
        [--allow-next https://admin.twelve-weeks.com]... [--host 127.0.0.1] [--port 7020]
```

The flags are web.py's; `--site` defaults to `./site` (web.py: `<repo>/site`). The unit passes every flag, so only the program changes.

Files it touches, all as web.py did: the database is opened read-only. Next to the database it reads `web-secret` (created with mode 600 when
missing), `web-admins.txt`, `devs.txt`, `bans.txt`, and appends to `web-admin.log`. Next to the news file it writes `news.json` (atomic
rename), `news-backups/` (newest 50) and `news-trash.json`. Times in logs and backup names use the box's local time zone, like Python.

## Build

In WSL (or any Linux with the musl target and `musl-gcc`):

```
cd tools/web-rs
CC_x86_64_unknown_linux_musl=musl-gcc cargo build --release --target x86_64-unknown-linux-musl
ls -l target/x86_64-unknown-linux-musl/release/t2r-web
```

## Deploy on fra

1. Copy the binary up and install it next to `t2r-files`:

   ```
   scp target/x86_64-unknown-linux-musl/release/t2r-web <ssh-user>@play.twelve-weeks.com:/tmp/t2r-web
   ssh <ssh-user>@play.twelve-weeks.com
   sudo install -o kellie -g kellie -m 755 /tmp/t2r-web /home/kellie/t2r-live/bin/t2r-web && rm /tmp/t2r-web
   ```

2. Point the unit at it with a drop-in (the unit file itself stays as it is):

   ```
   sudo mkdir -p /etc/systemd/system/t2r-web.service.d
   sudo tee /etc/systemd/system/t2r-web.service.d/rust.conf >/dev/null <<'EOF'
   [Service]
   ExecStart=
   ExecStart=/home/kellie/t2r-live/bin/t2r-web --db /home/kellie/t2r-live/data/t2r.db --news /home/kellie/t2r-live/update-data/news.json --site /home/kellie/t2r-live/site --public-url https://twelve-weeks.com --host 127.0.0.1 --port 7020
   EOF
   sudo systemctl daemon-reload
   sudo systemctl restart t2r-web
   ```

3. Check it:

   ```
   systemctl status t2r-web --no-pager
   tail -3 /home/kellie/t2r-live/web.out
   curl -s http://127.0.0.1:7020/api/news | head -c 200; echo
   curl -s 'http://127.0.0.1:7020/api/leaderboard?by=elo' | head -c 200; echo
   ps -o pid,rss,time,cmd -C t2r-web
   ```

   Then open https://twelve-weeks.com, a player page and the leaderboards, log in with Steam, and open the news editor (`/news.html`)
   and https://admin.twelve-weeks.com as an admin.

Rollback: `sudo rm /etc/systemd/system/t2r-web.service.d/rust.conf && sudo systemctl daemon-reload && sudo systemctl restart t2r-web`.

## Differences from web.py

- JSON is compact (`{"a":1}` instead of `{"a": 1}`); values and key order are the same. `news.json` writes are byte-identical.
- HTTP/1.1 keep-alive (web.py answered HTTP/1.0 and closed every connection, so cloudflared reconnected for each request).
- Static files carry `Last-Modified` and answer a matching `If-Modified-Since` with 304 (pages use the newest of the page and the
  css/js they version). Files are sent with sendfile instead of being read into memory.
- `/api/news` is rebuilt only when `news.json` or `site/news-big.json` changes. The clan block of a profile comes from the same
  60-second snapshot as the rest of the profile (web.py opened the database again for it on every request).
- Removed `/auth/dev-login` and `T2R_WEB_DEV_ADMIN`: a development-only login bypass that has no place in the production binary.
- Limits web.py did not have: 256 concurrent connections, 64 KB request head, 30 s to send a request, 120 s idle keep-alive.
- A broken `news.json` during an admin edit gives a 500 JSON error (web.py dropped the connection). The 503 `detail` for a database
  error is SQLite's message as Rust reports it.
- A few validation quirks of Python's regexes are not copied: a media file name with a trailing newline and dates written with
  non-ASCII digits are rejected; `/page.html;x` is a 404 (Python's `urlparse` dropped the `;x`).
