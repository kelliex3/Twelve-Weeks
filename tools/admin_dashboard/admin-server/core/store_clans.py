"""Clan list and launcher clan-view payloads."""

from __future__ import annotations

import sqlite3

from store_common import CLANS_CACHE, clan_alife, clan_view, clanfeed_view


class ClansMixin:
    """Builds clan list, feed and launcher-view payloads."""

    def clanview(self, pid="", lang="en"):
        """The launcher's CLAN tab payload for a player, read-only."""
        if not clan_view or not clanfeed_view:
            return {"ok": False, "error": "update_server clan views not available"}
        group = self.player_groups().get(pid)
        if not group:
            return {"ok": False, "error": "no such player"}
        if not group["clan_key"]:
            return {"ok": True, "name": group["display"],
                    "clan": {"ok": True, "hasClan": False}, "feed": {"ok": True, "hasClan": False, "feed": []}}
        language = "pl" if lang == "pl" else "en"
        key = group["clan_key"]
        return {"ok": True, "name": group["display"],
                "clan": clan_view.view(self.db, "", key=key),
                "feed": clanfeed_view.view(self.db, "", 40, 0, language, key=key)}

    def clans(self):
        return self.cached("clans", CLANS_CACHE, self._build_clans)

    def _alife_summary_map(self, conn, keys):
        """Decode an A-Life state only when its `updated` timestamp changed."""
        try:
            updated = dict(conn.execute("SELECT token_id,updated FROM clan_alife").fetchall())
        except sqlite3.Error:
            return {}
        out = {}
        for key in keys:
            if key not in updated:
                out[key] = {}
                continue
            with self.lock:
                hit = self._alife_summaries_cache.get(key)
            if hit and hit[0] == updated[key]:
                out[key] = hit[1]
                continue
            summary = clan_alife.summary(conn, key)
            with self.lock:
                self._alife_summaries_cache[key] = (updated[key], summary)
            out[key] = summary
        return out

    def _build_clans(self):
        groups = self.player_groups()
        rows, seen = [], set()
        for group in groups.values():
            info = group["clan"]
            if not info or info["key"] in seen:
                continue
            seen.add(info["key"])
            rows.append(dict(info, owner=group["display"], ownerId=group["id"],
                             key=None, lastSeen=group["last_seen"]))
        rows.sort(key=lambda r: (r["status"] != "alive", -(r["weeksTotal"] * 100 + r["day"])))

        feed = self._clan_feed(groups)
        self._attach_alife(groups, rows)
        by_faction = {"WLF": 0, "SERAPHITES": 0}
        for row in rows:
            if row["status"] == "alive" and row["factionName"] in by_faction:
                by_faction[row["factionName"]] += 1
        return {
            "clans": rows, "feed": feed,
            "alive": len([r for r in rows if r["status"] == "alive"]), "byFaction": by_faction,
            "atRisk": len([r for r in rows if r["risk"]]), "feedAvailable": feed is not None,
        }

    def _clan_feed(self, groups):
        owners = {g["clan_key"]: g["display"] for g in groups.values() if g["clan_key"]}
        try:
            conn = self.connect()
            try:
                feed = []
                for r in conn.execute("SELECT token_id,id,ts,day,week,kind,mood,faction,text FROM clan_feed "
                                      "ORDER BY ts DESC LIMIT 40"):
                    feed.append({
                        "owner": owners.get(r[0], "?"), "id": r[1], "ts": r[2], "day": r[3], "week": r[4],
                        "kind": r[5], "mood": r[6], "faction": r[7],
                        "text": r[8] if len(r[8] or "") < 400 else r[8][:400] + "...",
                    })
                return feed
            finally:
                conn.close()
        except sqlite3.Error:
            return None

    def _attach_alife(self, groups, rows):
        if not clan_alife:
            return
        try:
            conn = self.connect()
            try:
                by_owner = {}
                for row in rows:
                    by_owner.setdefault(row["ownerId"], []).append(row)
                keys = {g["clan_key"] for g in groups.values() if g["clan_key"]}
                summaries = self._alife_summary_map(conn, keys)
                for group in groups.values():
                    for row in by_owner.get(group["id"], ()):
                        row["alife"] = summaries.get(group["clan_key"], {})
            finally:
                conn.close()
        except sqlite3.Error:
            pass
