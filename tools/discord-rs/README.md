# Twelve Weeks Discord bot (Rust)

One static binary, `twelveweeks-bot`, that replaces the Python bot in `tools/discord-bot` (`twelveweeks_bot.py run` and
its modules). It reads and writes the same files in the same folder (`config.json`, `channels.json`, `bug_desk.json`,
`live_channels.json`, `report_codes.json`, `known_issues.json`), so deploying it means: copy the binary, change
`ExecStart`, restart.

## What it does

| Part | Same as in Python |
| --- | --- |
| Buttons on the `#choose-your-faction` post | WLF / Seraphites (switches faction), Sneak peek pings, LFG pings (creates the LFG role on first use). |
| `#new-survivors` welcome card | Same picture: avatar, name, member number. Same fonts and background from `welcome-art/`. |
| Slash commands | `/lfg`, `/sneakpeek`, `/announce`, `/patchnotes`, `/files`, `/welcome-preview`, and with the bug desk on `/known-issue add\|fixed\|remove` and `/report`. |
| Bug desk | `#bug-reports` forum: report code check against the update server's reports folder and `report_codes.json`, the replies, tags, rename and lock, `discord.txt` next to the report, the pinned guide with the known-issues list, the `#general` AutoMod rule. |
| LIVE PLAYERS | The locked voice channels whose names carry the counts from the daemon's status file and the launcher snapshot. |
| `post <channel...>` | Re-posts the art (and the buttons) in those channels. REST only: it does not log in a second time while the service runs. |

Changed on purpose:

- Button clicks are answered within Discord's 3 seconds even when a role change is slow (the Python bot logged
  `Unknown interaction` for these).
- Forum posts are checked once their first message exists. The Python bot answered too early on posts with uploads and
  got `403 Cannot message this thread until after the post author has sent an initial message`.
- A count channel that Discord rate limits is skipped until Discord allows it again, without holding up the others.
- The AutoMod exempt list follows new and deleted channels within a minute (Python: only at start).
- A failed `/sneakpeek` / `/announce` / `/patchnotes` / `/files` upload (for example a file over the upload limit)
  tells the moderator why. In Python the command stayed on "thinking".
- `report_codes.json` gets the build of a report whose `meta.json` was written after the folder.
- The bot only asks the gateway for the server, channel, role and member events (no message stream).

Not ported (the Python files stay in `tools/discord-bot` for these):

- `plan` / `overhaul`: one-time server rebuild from 11-05, destructive, done. `channels.json` exists, so a re-run would
  only have re-posted art; `post` covers that.
- OG role on join and `/og-stop`: OG closed at release (`og.json`: `"active": false` since 11-20). `og.json` and
  `og_members.json` are left as they are.
- The one-time parts of the bug desk setup (export and delete `#help`, move the old text `#bug-reports` to ARCHIVE):
  both ran on 11-22 (`bug_desk.json`: `help_deleted`, `forum`). Creating the forum and the guide post when they are
  missing is kept.
- REST helper scripts `discord_post.py`, `discord_edit.py`, `post_presskit.py`, `edit_presskit.py`,
  `update_download_links.py` and `tools-1120/`: admin one-offs, run by hand, standard library only. They work with the
  system `python3` and cost nothing while they are not running, so they stay Python.

## Build

Static Linux x86_64 (musl, rustls, no OpenSSL). In WSL:

```
cd tools/discord-rs
cargo build --release --target x86_64-unknown-linux-musl
cargo test --target x86_64-unknown-linux-musl
```

The binary is `target/x86_64-unknown-linux-musl/release/twelveweeks-bot` (about 3.5 MB).

## Deploy on fra

The bot folder is `/home/kellie/twelveweeks-bot`, the unit `twelveweeks-bot.service`, the user `kellie`.

1. Copy the binary next to the Python files:

   ```
   scp target/x86_64-unknown-linux-musl/release/twelveweeks-bot <ssh-user>@play.twelve-weeks.com:/tmp/
   ssh <ssh-user>@play.twelve-weeks.com
   sudo install -o kellie -g kellie -m 755 /tmp/twelveweeks-bot /home/kellie/twelveweeks-bot/twelveweeks-bot
   ```

2. Back up the state files (the bot rewrites some of them):

   ```
   cd /home/kellie/twelveweeks-bot
   sudo -u kellie tar czf /home/kellie/twelveweeks-bot-state-$(date +%Y%m%d-%H%M).tgz \
     channels.json bug_desk.json live_channels.json report_codes.json $(ls known_issues.json 2>/dev/null)
   ```

3. Point the service at the binary with a drop-in (the unit file stays as it is):

   ```
   sudo mkdir -p /etc/systemd/system/twelveweeks-bot.service.d
   printf '[Service]\nExecStart=\nExecStart=/home/kellie/twelveweeks-bot/twelveweeks-bot run\n' | \
     sudo tee /etc/systemd/system/twelveweeks-bot.service.d/rust.conf
   sudo systemctl daemon-reload
   sudo systemctl restart twelveweeks-bot
   ```

4. Check it:

   ```
   systemctl status twelveweeks-bot --no-pager
   tail -f /home/kellie/twelveweeks-bot/bot.out      # "logged in as Twelve Weeks#5416 (run)"
   ```

   In Discord: click a faction button, run `/report` in a staff channel, look at the LIVE PLAYERS names.

Roll back to Python:

```
sudo rm /etc/systemd/system/twelveweeks-bot.service.d/rust.conf
sudo systemctl daemon-reload
sudo systemctl restart twelveweeks-bot
```

Once the Rust bot has run for a while, `.venv/` and `__pycache__/` can go; the helper scripts do not need them.

## Other commands

```
twelveweeks-bot post choose-your-faction files     # re-post art + buttons (files: only when every download link answers)
twelveweeks-bot live-counts [status.json [launchers.json]] [--at-snapshot]
twelveweeks-bot desk-check <reports dir> <report_codes.json> <title> <first message>
twelveweeks-bot known-text known_issues.json
twelveweeks-bot welcome-card out.png "Name" avatar.png 1234 username
twelveweeks-bot commands                            # the slash command definitions
```

`--dir <folder>` before the command uses another bot folder; by default it is the folder the binary is in. Log lines go
to stdout (`bot.out` under systemd) with a UTC time stamp.

## Comparing with the Python bot

`compare/` runs the same input through the deployed Python modules and through the binary. On a Linux box with a
Python that has Pillow, fontTools and discord.py (the bot's `.venv`), and the bot folder with `welcome-art/fonts`:

```
PY=/path/to/.venv/bin/python sh compare/run.sh ./twelveweeks-bot /path/to/python/bot /tmp/cmp [t2r-status.json ...]
```

- `live.py`: channel names for synthetic and real status files. They must be equal.
- `desk.py`: bug desk replies, tags, renames and locks for a set of forum posts, the known-issues text and `/report`.
  They must be equal.
- `cards.sh`: welcome cards for eight names and avatars, with PSNR and pixel-difference numbers and side-by-side
  pictures (`side-N.png`: Python, Rust, difference x4). Text edges differ slightly: Pillow draws glyphs through
  FreeType with hinting, the binary through ab_glyph without.
