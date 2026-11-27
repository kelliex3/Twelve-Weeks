"""Overview and player-list views built from the live feed and database groups."""

from __future__ import annotations

import json
import os
import time

from store_common import STATE_ORDER, STATUS_AGE, redact


class OverviewMixin:
    """Builds the overview and player-list payloads."""

    # ---------------------------------------------------------------- views
    def overview(self):
        """The exact payload the classic `viewOverview(ov, on, nt)` consumes."""
        live = self.live_view()
        snap = live["snap"]
        probe = self.probe()
        log_snap = self.logsnap()
        now = time.time()
        counts = snap.get("counts") or {}
        players = snap.get("players") or []

        by_state = {"menu": 0, "queue": 0, "match": 0}
        for player in players:
            state = player["state"]
            by_state[state] = by_state.get(state, 0) + 1

        online = counts.get("online")
        if online is None:
            online = len(players) if live["source"] != "log" or players else None
        server_up = bool(probe["open"] or live["source"] in ("live", "snapshot", "snapshot-counts"))
        fresh_log = bool(log_snap and log_snap["status"] and now - log_snap["status"]["ts"] < STATUS_AGE)
        started = live["started"] or (log_snap["started"] if log_snap else None)
        uptime = round(now - started) if (started and server_up) else None

        latest = self.latest_version()
        server_version = live["server_version"]
        groups = self.player_groups() if os.path.exists(self.db) else {}
        clients: dict[str, int] = {}
        for player in players:
            version = player.get("version") or "?"
            clients[version] = clients.get(version, 0) + 1
        outdated = sum(n for version, n in clients.items() if server_version and version not in (server_version, "?"))
        running = counts.get("matches")
        if running is None:
            running = len([m for m in snap.get("matches", []) if m["state"] != "ended"])

        notes = []
        if live["source"] == "log":
            notes.append("No live feed from the daemon (it needs a restart on the build with the admin listener). "
                         "Online / match data is reconstructed from the log and is approximate.")
        elif live["source"] == "snapshot":
            notes.append("Data comes from the daemon's 10 s snapshot (the admin listener is not reachable or no admin-secret.txt: kick is unavailable).")
        elif live["source"] == "snapshot-counts":
            notes.append("The snapshot only holds counts (older daemon build); names come from the log.")

        errors = ([{"ts": e["ts"], "line": redact(e["line"])} for e in log_snap["errors"][-8:]][::-1]
                  if log_snap else [])
        seen_clans = {}
        for group in groups.values():
            if group["clan"]:
                seen_clans[group["clan"]["key"]] = group["clan"]
        matches = self.derived_matches(log_snap)
        unhandled = len([1 for _, m in self.report_metas() if not m.get("handled")]) if self.reports else 0

        return {
            "now": now, "source": live["source"], "snapshotAge": live["snapshot_age_s"], "notes": notes,
            "server": {"up": server_up, "probe": probe, "version": server_version, "latest": latest,
                       "uptime_s": uptime, "started": started, "logFresh": fresh_log,
                       "logPath": self.cur_log()},
            "online": {"total": online, "menu": by_state["menu"],
                       "queue": counts.get("queue") if counts.get("queue") is not None else by_state["queue"],
                       "match": by_state["match"]},
            "matchesRunning": running,
            "clients": {"versions": clients, "outdated": outdated},
            "totals": {
                "players": len(groups), "clans": len(seen_clans),
                "clansAlive": len([c for c in seen_clans.values() if c["status"] == "alive"]),
                "matchesLogged": len([m for m in matches if m["state"] == "ended"]),
                "matches24h": len([m for m in matches if m["state"] == "ended" and now - m["ended"] < 86400]),
                "reportsNew": unhandled,
            },
            "errors": errors, "canKick": live["source"] == "live",
        }

    def latest_version(self):
        if not self.update_data:
            return ""
        try:
            with open(os.path.join(self.update_data, "channels.json"), encoding="utf-8") as fh:
                channels = json.load(fh)
            return str(channels.get("stable") or next(iter(channels.values()), ""))
        except (OSError, ValueError, StopIteration, AttributeError):
            return ""

    def players_online(self):
        """Currently connected players, one row per person (most active connection wins)."""
        live = self.live_view()
        groups = self.player_groups() if os.path.exists(self.db) else {}
        by_steam = {g["steamid"]: g for g in groups.values() if g["steamid"]}
        server_version = live["server_version"]
        snap = live["snap"]

        pings = {}
        for match in snap.get("matches", []):
            for player in match.get("players", []):
                if player.get("ping_ms") is not None and player.get("pid"):
                    pings[player["pid"]] = player["ping_ms"]

        rows = []
        for player in snap.get("players", []):
            group = by_steam.get(player.get("steamid")) or next(
                (g for g in groups.values() if player["name"].lower() in g["alias"]), None)
            row = dict(player)
            row["ping"] = pings.get(player.get("pid"))
            if group:
                row["playerId"] = group["id"]
                clan_info = group["clan"]
                row["clan"] = ({"factionName": clan_info["factionName"], "week": clan_info["week"],
                                "day": clan_info["dayOfWeek"], "size": clan_info["size"]}
                               if clan_info and clan_info["status"] == "alive" else None)
                row["tag"] = group["tag"]
                if not row.get("version"):
                    row["version"] = group["version"]
                if not row.get("platform"):
                    row["platform"] = group["platform"]
            row["outdated"] = bool(server_version and row.get("version") and row["version"] != server_version)
            rows.append(row)

        best = {}
        for row in rows:
            key = row.get("playerId") or "n:" + row["name"].lower()
            current = best.get(key)
            if current is None or self._activity(row) < self._activity(current):
                best[key] = row
        rows = list(best.values())
        rows.sort(key=lambda row: (STATE_ORDER.get(row["state"], 3), row["name"].lower()))
        return {"source": live["source"], "players": rows, "serverVersion": server_version,
                "canKick": live["source"] == "live"}

    @staticmethod
    def _activity(row):
        return STATE_ORDER.get(row["state"], 3), row.get("idle_s") or 0

    def players_all(self, q="", limit=300):
        groups = self.player_groups()
        query = (q or "").lower().strip()
        snap = self.live_view()["snap"]
        online_names = {p["name"].lower() for p in snap.get("players", [])}
        online_ids = {p.get("steamid") for p in snap.get("players", []) if p.get("steamid")}
        banned_ids, banned_names = self.banned_index()

        rows, hidden = [], 0
        for group in groups.values():
            if query and not (query in group["display"].lower() or any(query in a for a in group["alias"])
                              or query in str(group["steamid"])):
                continue
            info = group["clan"]
            online = bool(group["steamid"] in online_ids or (group["alias"] & online_names))
            # Hide stale installs that never finished a match, unless searched or online.
            if not query and not online and not group["matches"]:
                hidden += 1
                continue
            rows.append({
                "id": group["id"], "name": group["display"], "steamid": group["steamid"],
                "matches": group["matches"], "wins": group["wins"], "losses": group["losses"],
                "draws": group["draws"], "lastSeen": group["last_seen"], "version": group["version"],
                "platform": group["platform"], "online": online,
                "banned": bool(str(group["steamid"]) in banned_ids or (group["alias"] & banned_names)),
                "faction": info["factionName"] if info else "", "clanStatus": info["status"] if info else "",
                "week": info["week"] if info else 0, "day": info["dayOfWeek"] if info else 0,
                "size": info["size"] if info else 0,
            })
        rows.sort(key=lambda row: -row["lastSeen"])
        return {"total": len(groups), "shown": min(len(rows), limit), "hidden": hidden,
                "players": rows[:limit], "hasKd": any(g["kills"] or g["deaths"] for g in groups.values())}
