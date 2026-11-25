"""Live player-count channels for the Twelve Weeks Discord server.

A category of locked voice channels whose NAMES carry the live counts (nobody can connect or type there; the bot only renames them):

    LIVE PLAYERS
      Supply Raid: 6 in match
      Interrogation: 4 in match
      In lobby: 3            searching (FIND MATCH) + formed lobbies that have not started yet (map vote / loading)
      In menus: 2            online in the game, not searching, not in a match
      Launcher open: 12      launchers polling the update server right now (game running or not; one per address)

Sources (both on the bot's host, read-only):
  status file     the daemon's snapshot (T2R_STATUS_FILE, default ~/t2r-live/data/t2r-status.json, rewritten every 10 s):
                  live.players[].state (match / queue / menu) + live.matches[].state (loading / running / ended) + .game_mode
  launchers file  t2r-launchers.json next to it, written by tools/update_server every 15 s: {"ts", "open", "window_s"}
A daemon without game_mode (older build) counts every match as Supply Raid.

Discord allows about 2 renames per channel per 10 minutes: a channel is renamed only when its text changed and at least
MIN_RENAME_S after its last rename (so a count is at most ~5 minutes behind).

config.json "live_counts": {"enabled": true, "category": "LIVE PLAYERS", "position": null, "status_file": "...", "launchers_file": "...",
                            "channels": [["supply-raid", "Supply Raid: {n} in match"], ...], "offline": "{label}: server offline"}
Channel ids are kept in live_channels.json next to the bot. The bot never deletes anything and only edits the channels it created.

  python live_counts.py [status.json [launchers.json]] [--at-snapshot]    print the channel names for that data (changes nothing)
"""
import asyncio
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
STATE = os.path.join(HERE, "live_channels.json")
DEF_STATUS = os.path.expanduser("~/t2r-live/data/t2r-status.json")
MODES = ("supply-raid", "interrogation")

DEF_CHANNELS = [
    ["supply-raid", "Supply Raid: {n} in match"],
    ["interrogation", "Interrogation: {n} in match"],
    ["lobby", "In lobby: {n}"],
    ["menus", "In menus: {n}"],
    ["launcher", "Launcher open: {n}"],
]
DEF_CATEGORY = "LIVE PLAYERS"
STATUS_STALE_S = 60       # the daemon writes every 10 s: older = daemon down
LAUNCHERS_STALE_S = 90    # the update server writes every 15 s
MIN_RENAME_S = 310        # Discord: 2 renames / 10 min / channel
TICK_S = 20
EDIT_TIMEOUT_S = 20       # discord.py sleeps through a 429 itself: never let one rename hold the loop


def _read(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def counts(status, launchers, now=None):
    """{key: int or None}; None = that source is missing / stale. Pure (tested on a real snapshot)."""
    now = time.time() if now is None else now
    out = {k: None for k in MODES + ("lobby", "menus", "launcher", "online")}
    if status and now - float(status.get("ts") or 0) <= STATUS_STALE_S:
        live = status.get("live") or {}
        ms = {m.get("id"): m for m in live.get("matches") or []}
        c = {k: 0 for k in MODES + ("lobby", "menus")}
        players = live.get("players") or []
        for p in players:
            st = p.get("state")
            if st == "queue":
                c["lobby"] += 1
            elif st == "match":
                m = ms.get(p.get("match")) or {}
                if m.get("state") == "running":
                    gm = m.get("game_mode") or "supply-raid"
                    c[gm if gm in MODES else "supply-raid"] += 1
                elif m.get("state") == "ended":
                    c["menus"] += 1          # post-match screens: back in the menus a moment later
                else:
                    c["lobby"] += 1          # formed, not started: map vote / loading
            else:
                c["menus"] += 1
        out.update(c)
        out["online"] = len(players) if players else int(status.get("players_online") or 0)
    if launchers and now - float(launchers.get("ts") or 0) <= LAUNCHERS_STALE_S:
        out["launcher"] = int(launchers.get("open") or 0)
    return out


def label_of(template):
    return template.split("{")[0].rstrip(" :") or template


def names(cnt, channels=DEF_CHANNELS, offline="{label}: offline"):
    """[(key, channel name)] for these counts."""
    res = []
    for key, tpl in channels:
        n = cnt.get(key)
        res.append((key, (tpl.format(n=n) if n is not None else offline.format(label=label_of(tpl)))[:100]))
    return res


class LiveCounts:
    def __init__(self, bot, guild, cfg):
        self.bot, self.guild, self.cfg = bot, guild, cfg
        self.status_file = cfg.get("status_file") or DEF_STATUS
        self.launchers_file = cfg.get("launchers_file") or os.path.join(os.path.dirname(self.status_file), "t2r-launchers.json")
        self.channels = cfg.get("channels") or DEF_CHANNELS
        self.offline = cfg.get("offline") or "{label}: offline"
        self.min_rename = float(cfg.get("min_rename_s") or MIN_RENAME_S)
        self.last = {}            # channel id -> monotonic time of the last rename attempt
        self.task = None

    def _overwrites(self):
        import discord
        lock = discord.PermissionOverwrite(view_channel=True, connect=False, speak=False, stream=False, send_messages=False,
                                           add_reactions=False, create_instant_invite=False)
        me = discord.PermissionOverwrite(view_channel=True, connect=True, manage_channels=True, send_messages=True)
        return {self.guild.default_role: lock, self.guild.me: me}

    def _load(self):
        return _read(STATE) or {}

    def _save(self, st):
        tmp = STATE + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(st, f, indent=1)
        os.replace(tmp, STATE)

    async def ensure(self):
        """Find or create the category and its channels; re-lock them if someone changed their permissions. Returns {key: channel}."""
        import discord
        g = self.guild
        st = self._load()
        ow = self._overwrites()
        cat = g.get_channel(int(st["category"])) if st.get("category") else None
        if cat is None:
            want = self.cfg.get("category") or DEF_CATEGORY
            cat = next((c for c in g.categories if c.name == want), None)
            if cat is None:
                cat = await g.create_category(want, overwrites=ow, reason="live player counts")
                print("live counts: created category", cat.id, flush=True)
                if self.cfg.get("position") is not None:
                    # a position given at create only ties with the category already there (0 = second); edit() reorders all categories
                    await cat.edit(position=int(self.cfg["position"]), reason="live player counts")
            st["category"] = cat.id
        elif cat.overwrites != ow:
            await cat.edit(overwrites=ow, reason="live player counts: keep locked")
        ids = st.setdefault("channels", {})
        cnt = counts(_read(self.status_file), _read(self.launchers_file))
        out = {}
        for key, name in names(cnt, self.channels, self.offline):
            ch = g.get_channel(int(ids[key])) if ids.get(key) else None
            if ch is None:     # lost state file: a channel of ours in the category whose name starts with this row's label
                lab = label_of(dict(self.channels)[key])
                ch = next((c for c in cat.voice_channels if c.name.startswith(lab)), None)
            if ch is None:
                ch = await g.create_voice_channel(name, category=cat, overwrites=ow, reason="live player counts")
                self.last[ch.id] = time.monotonic()
                print("live counts: created", key, ch.id, flush=True)
            elif ch.overwrites != ow:
                await ch.edit(overwrites=ow, reason="live player counts: keep locked")
            ids[key] = ch.id
            out[key] = ch
        self._save(st)
        return out

    async def tick(self, chans):
        cnt = counts(_read(self.status_file), _read(self.launchers_file))
        for key, name in names(cnt, self.channels, self.offline):
            ch = chans.get(key)
            if ch is None or ch.name == name:
                continue
            now = time.monotonic()
            if now - self.last.get(ch.id, -1e9) < self.min_rename:
                continue
            self.last[ch.id] = now
            try:
                await asyncio.wait_for(ch.edit(name=name, reason="live player counts"), EDIT_TIMEOUT_S)
            except asyncio.TimeoutError:
                print("live counts: rename of", key, "rate limited, next try in", int(self.min_rename), "s", flush=True)
            except Exception as e:    # noqa: BLE001 - a failed rename never stops the loop
                print("live counts: rename", key, repr(e), flush=True)

    async def run(self):
        chans = {}
        while True:
            try:
                if not chans or any(self.guild.get_channel(c.id) is None for c in chans.values()):
                    chans = await self.ensure()
                await self.tick(chans)
            except Exception as e:    # noqa: BLE001
                print("live counts:", repr(e), flush=True)
                chans = {}
            await asyncio.sleep(TICK_S)

    def start(self):
        if self.task is None or self.task.done():
            self.task = asyncio.get_running_loop().create_task(self.run())


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    sf = args[0] if args else DEF_STATUS
    lf = args[1] if len(args) > 1 else os.path.join(os.path.dirname(sf), "t2r-launchers.json")
    s, la = _read(sf), _read(lf)
    at = None
    if "--at-snapshot" in sys.argv:        # judge staleness at the snapshot's own time (a copied file)
        at = float((s or {}).get("ts") or time.time())
        if la:
            la = dict(la, ts=at)
    c = counts(s, la, now=at)
    print(json.dumps(c))
    for k, n in names(c):
        print("%-14s %s" % (k, n))
