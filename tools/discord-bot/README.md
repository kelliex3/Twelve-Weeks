# Twelve Weeks Discord bot

Runs the community Discord server: rebuilds the channel layout and role set, posts the banner / rules / faction
pages, handles the faction and ping buttons, welcome cards, slash commands, the bug-report forum desk, and the
LIVE PLAYERS voice-channel counters.

This folder is the code of the bot that runs in production. It is a private copy for the people who work on it.

## What is here

| File | Purpose |
| --- | --- |
| `twelveweeks_bot.py` | Main bot. Modes: `plan`, `overhaul`, `run`, `post <channel...>` (see the module docstring). |
| `bug_desk.py` | `#bug-reports` forum with a required launcher report code, known-issues list, `#general` AutoMod rule. |
| `live_counts.py` | LIVE PLAYERS voice channels (players in match, lobby, launcher open) from the game daemon's status file. |
| `welcome_card.py` | Renders the welcome card image for new members (Pillow + fontTools). |
| `discord_post.py`, `discord_edit.py` | One-shot REST helpers to post / edit patch-note embeds without a gateway login. |
| `post_presskit.py`, `edit_presskit.py` | One-shot REST helpers for the press-kit post. |
| `update_download_links.py` | Rewrites launcher download links in existing posts to a new launcher version. |
| `tools-1120/` | One-time maintenance scripts (export channel history, archive old channels). |
| `test_bug_desk.py`, `test_live_counts.py` | Tests that run without Discord. |
| `twelveweeks-bot.service` | Template systemd unit. |
| `config.example.json`, `channels.example.json` | Placeholder config. Copy and fill in. |
| `art/`, `welcome-art/` | Banner and role art, welcome card background. |

## Setup

Python 3.10+ (production runs 3.12).

```
python -m venv .venv
.venv/bin/pip install -r requirements.txt        # Windows: .venv\Scripts\pip
cp config.example.json config.json               # then edit config.json
```

In the Discord developer portal turn on the **Server Members** and **Message Content** intents. Invite the bot with
the Administrator permission.

## Configuration

`config.json` (next to `twelveweeks_bot.py`, never committed, it holds the bot token):

| Key | Needed | Meaning |
| --- | --- | --- |
| `token` | yes | Bot token. |
| `guild_id` | yes | Id of the Discord server. |
| `downloads` | no | `[label, url]` pairs for the `/files` post. A built-in default is used when absent. |
| `live_counts.enabled` | no | Turn the LIVE PLAYERS counters on. |
| `live_counts.category` | no | Category name for the counter channels. |
| `live_counts.position` | no | Category position (`null` keeps the default). |
| `live_counts.status_file` | no | Path of the daemon's status JSON (default `~/t2r-live/data/t2r-status.json`). |
| `live_counts.launchers_file` | no | Path of the launcher-open snapshot (default: `t2r-launchers.json` next to the status file). |
| `live_counts.channels` | no | `[key, name template]` pairs; `{n}` is replaced by the count. |
| `bug_desk.enabled` | no | Turn the bug desk on. |
| `bug_desk.reports_dir` | no | Folder of uploaded player reports on the update server (used to check report codes). |
| `bug_desk.delete_help` | no | One-time: archive and delete the old `#help` channel. |
| `bug_desk.general_automod` | no | Keep the `#general` bug-talk AutoMod rule (default `true`). |

Other settings from the environment: none. The REST helper scripts read the token from `config.json` too, and
`post_presskit.py` / `edit_presskit.py` take `PRESSKIT_CHANNEL_ID` and `PRESSKIT_MESSAGE_ID` from the environment.

### Files the bot creates and reads at run time

All of them stay out of git (see `.gitignore`):

- `channels.json`: channel name to Discord id, written by `overhaul`, read by the post helpers. Shape in `channels.example.json`.
- `live_channels.json`, `bug_desk.json`, `known_issues.json`, `report_codes.json`: state of the counters and the bug desk.
- `og.json`, `og_members.json`: the OG role state and member list (Discord ids and names).
- `look.json`, `posts.json`: server look and posted message ids.
- `backups/`: archived channel history written by `overhaul`.

## Run

```
python twelveweeks_bot.py plan        # show what the overhaul would do, changes nothing
python twelveweeks_bot.py overhaul    # back up + rebuild the server, then keep running
python twelveweeks_bot.py run         # just run the bot
python twelveweeks_bot.py post welcome rules   # re-post only those channels' images
```

`overhaul` deletes channels after archiving them to `backups/`: read the `plan` output first.

Production: copy `twelveweeks-bot.service`, replace `BOTUSER` and `BOTDIR`, install it in `/etc/systemd/system/`,
then `systemctl enable --now twelveweeks-bot`. Logs go to `bot.out` in the bot folder.

Slash commands: `/lfg`, `/sneakpeek`, `/announce`, `/patchnotes`, `/welcome-preview`, `/og-stop`, `/files`, plus
`/known-issue add|fixed|remove` and `/report` when the bug desk is on.

## Tests

```
python test_live_counts.py
python test_bug_desk.py     # writes bug_desk.json next to itself: run it in a scratch copy
```

## Not in the repo

- `config.json` and every backup of it (bot token).
- Runtime state and logs listed above, `.venv/`, `__pycache__/`, `backups/`.
- Dated patch-note post files (`discord-*.json`).
- Art over 1 MB (`art/choose-your-faction.png`, `art/rules.png`, `art/welcome-guide.png`). Ask the project owner and put them in `art/`.
- Welcome card fonts (`welcome-art/fonts/rough.ttf`, `body.ttf`). They are third-party fonts. Put your own copies there, or
  the card falls back to a system font (`fallback.ttf` or DejaVu Sans Bold).
