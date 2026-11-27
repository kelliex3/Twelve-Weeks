"""Player detail and match history views."""

from __future__ import annotations

import os
import sqlite3

from store_common import EARLY_DEPARTURES, MATCH_RESULTS, clan_alife, clan_story, logindex, pretty_level


class PlayerDetailMixin:
    """Builds player detail and match history payloads."""

    def player(self, pid):
        group = self.player_groups().get(pid)
        if not group:
            return {"ok": False, "error": "no such player"}

        history = []
        for match in reversed(self.derived_matches(self.logsnap())):
            name = next((n for n in match["players"] if n.lower() in group["alias"]), None)
            if name is None or match["started"] is None:
                continue
            history.append({
                "id": match["id"], "when": match["formed"], "map": pretty_level(match["map"]),
                "state": match["state"], "result": logindex.result_for(match, name), "secs": match["secs"],
                "players": len(match["players"]),
                "left": any(l["name"] == name and l["why"] in EARLY_DEPARTURES for l in match["left"]),
            })
            if len(history) >= 40:
                break

        feed, people, alife = [], {}, {}
        if group["clan_key"] and os.path.exists(self.db):
            try:
                conn = self.connect()
                try:
                    if clan_story:
                        feed = clan_story.read_feed(conn, group["clan_key"], 40)
                        people = clan_story.read_people(conn, group["clan_key"], 40)
                        polish = {m["id"]: m["text"] for m in
                                  clan_story.read_feed(conn, group["clan_key"], 40, lang="pl") if m["lang"] == "pl"}
                        for entry in feed:
                            entry["textPl"] = polish.get(entry["id"], "")
                    if clan_alife:
                        alife = clan_alife.view(conn, group["clan_key"])
                finally:
                    conn.close()
            except sqlite3.Error:
                pass

        reports = []
        for _, meta in self.report_metas():
            if ((meta.get("name") or "").lower() in group["alias"] or
                    (group["steamid"] and str(meta.get("steamId", "")) == str(group["steamid"]))):
                reports.append({"code": meta.get("code"), "received": meta.get("received"),
                                "text": (meta.get("text") or "")[:160], "handled": bool(meta.get("handled"))})
        online = next((p for p in self.players_online()["players"] if p.get("playerId") == pid), None)
        average = group["parts_total"] / group["parts_n"] if group["parts_n"] else None
        return {
            "ok": True, "id": pid, "name": group["display"], "tag": group["tag"],
            "aliases": sorted(group["names"]), "steamid": group["steamid"], "installs": group["tokens"],
            "matches": group["matches"], "wins": group["wins"], "losses": group["losses"], "draws": group["draws"],
            "kills": group["kills"], "deaths": group["deaths"], "avgParts": round(average) if average else None,
            "lastSeen": group["last_seen"], "version": group["version"], "platform": group["platform"],
            "clan": group["clan"], "morale": people.get("morale"), "people": people.get("people", [])[:12],
            "alive": people.get("alive"), "feed": feed, "alife": alife, "history": history, "reports": reports,
            "online": online, "ban": self.ban_state(group),
            "alifeServerOn": clan_alife.enabled() if clan_alife else False,
            "matchLogFrom": self.log.earliest() if self.log else None,
            "canKick": bool(online and online.get("pid") and self.live()[0] == "live"),
        }

    def matches(self):
        live = self.live_view()
        snap = live["snap"]
        running = [m for m in snap.get("matches", []) if m["state"] != "ended"]
        history = []
        for match in reversed(self.derived_matches(self.logsnap())):
            if match["closed"] is None or match["started"] is None:
                continue  # still open, or a countdown that never started
            team1, team2 = logindex.team_names(match)
            if match["state"] == "ended":
                result = MATCH_RESULTS.get(match["winner"], "RESULT UNKNOWN")
            else:
                result = "NOT FINISHED (%s)" % (match["closed_why"] or "closed")
            history.append({
                "id": match["id"], "formed": match["formed"], "started": match["started"],
                "ended": match["ended"], "map": pretty_level(match["map"]), "net": match["net"],
                "ping_ms": match["ping_ms"], "team1": team1, "team2": team2, "result": result,
                "winner": match["winner"], "secs": match["secs"], "host": match["host"],
                "state": match["state"],
                "left": [l["name"] + " (" + l["why"] + ")" for l in match["left"] if l["why"] in EARLY_DEPARTURES],
                "joined": match["jip"],
            })
            if len(history) >= 60:
                break
        return {"source": live["source"], "live": running, "history": history,
                "approx": live["source"] == "log", "canKick": live["source"] == "live"}
