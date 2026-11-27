"""Database-backed player and clan grouping for the admin dashboard store."""

from __future__ import annotations

import json
import sqlite3

from store_common import (
    CLAN_COLUMNS, PLAYERS_CACHE, PLAYER_COLUMNS, STATUS_NAMES,
    ClanRow, PlayerRow, clan, normalize_name, pid_of, token_embedded_name,
)


class PlayersMixin:
    """Builds one group per person from the game database."""

    # ---------------------------------------------------------------- database / players
    def player_groups(self):
        """One entry per person: Steam account when known, else install id, as in the leaderboard."""
        return self.cached("players", PLAYERS_CACHE, self._build_player_groups)

    def _load_player_tables(self):
        conn = self.connect()
        try:
            rows = [PlayerRow(*r) for r in conn.execute(
                "SELECT %s FROM players WHERE length(token)=64" % PLAYER_COLUMNS).fetchall()]
            names = {}
            try:
                for token_id, name, tag in conn.execute("SELECT token_id,name,tag FROM names"):
                    names[token_id] = (name or "", tag or "")
            except sqlite3.Error:
                pass
            clans = {}
            try:
                for raw in conn.execute("SELECT %s FROM clans" % CLAN_COLUMNS):
                    row = ClanRow(*raw)
                    clans[row.key] = row
            except sqlite3.Error:
                pass
        finally:
            conn.close()
        versions = self.read_json("client_versions.json", {})
        linux = set(self.read_json("linux_clients.json", []))
        return rows, names, clans, versions, linux

    @staticmethod
    def _steam_ids_by_name(rows):
        """Map each known name to the Steam accounts that used it."""
        owners: dict[str, set[int]] = {}
        for row in rows:
            if not row.steamid or row.steamid <= 0:
                continue
            for name in (normalize_name(row.name), normalize_name(token_embedded_name(row.token))):
                if name:
                    owners.setdefault(name, set()).add(int(row.steamid))
        return owners

    @staticmethod
    def _person_key(row, owners, linux):
        if row.steamid and row.steamid > 0:
            return "s%d" % row.steamid
        name = normalize_name(row.name) or normalize_name(token_embedded_name(row.token))
        if not name:
            return "t" + row.token[40:64]
        known = owners.get(name, ())
        if len(known) == 1:
            return "s%d" % next(iter(known))
        return "n:%s:%s" % (name, "linux" if row.token in linux else "windows")

    @staticmethod
    def _clan_row(row: ClanRow):
        size = int(row.healthy or 0) + int(row.hungry or 0) + int(row.sick or 0)
        try:
            last = json.loads(row.last_result or "{}")
        except ValueError:
            last = {}
        alive = row.status == clan.ALIVE
        week, day_of_week = clan.week_day(int(row.day or 0)) if alive else (0, 0)
        risk = []
        if alive:
            if size <= 2:
                risk.append("tiny clan")
            if size and (int(row.hungry or 0) + int(row.sick or 0)) >= 0.6 * size:
                risk.append("%d of %d hungry or sick" % (int(row.hungry or 0) + int(row.sick or 0), size))
            if int(last.get("died", 0) or 0) > 0:
                risk.append("%d died last match" % int(last.get("died", 0)))
        return {
            "faction": int(row.faction or 0), "factionName": clan.FACTION_NAMES.get(int(row.faction or 0), ""),
            "status": STATUS_NAMES.get(int(row.status or 0), "none"), "day": int(row.day or 0),
            "daysTotal": clan.WIN_DAY, "week": week, "dayOfWeek": day_of_week, "weeks": clan.WEEKS,
            "size": size, "healthy": int(row.healthy or 0), "hungry": int(row.hungry or 0),
            "sick": int(row.sick or 0), "needed": clan.needed(size), "weeksTotal": int(row.weeks or 0),
            "runsWon": int(row.runs_won or 0), "runsLost": int(row.runs_lost or 0),
            "lastParts": last.get("parts"), "lastSupplies": last.get("supplies"), "lastDied": last.get("died"),
            "risk": risk, "updated": row.updated, "key": row.key,
        }

    def _build_player_groups(self):
        rows, names, clans, versions, linux = self._load_player_tables()
        owners = self._steam_ids_by_name(rows)
        groups: dict[str, dict] = {}

        for row in rows:
            key = self._person_key(row, owners, linux)
            steamid = row.steamid
            has_steamid = bool(steamid and steamid > 0)
            if key.startswith("s") and not has_steamid:
                steamid = int(key[1:])   # a name-only install joined a Steam account
            group = groups.setdefault(key, {
                "key": key, "id": pid_of(key), "steamid": int(steamid or 0), "tokens": [],
                "names": set(), "kills": 0, "deaths": 0, "wins": 0, "matches": 0, "draws": 0,
                "parts_total": 0, "parts_n": 0, "last_seen": 0.0, "version": "", "platform": "windows",
                "ver_t": 0.0, "name": "", "name_t": (-1, -1.0),
            })
            group["tokens"].append(row.token)
            group["steamid"] = group["steamid"] or int(steamid or 0)
            if row.name:
                group["names"].add(row.name)
            embedded = token_embedded_name(row.token)
            if embedded:
                group["names"].add(embedded)
            for field, value in (("kills", row.kills), ("deaths", row.deaths), ("wins", row.wins),
                                 ("matches", row.matches), ("draws", row.draws),
                                 ("parts_total", row.parts_total), ("parts_n", row.parts_n)):
                group[field] += int(value or 0)
            last_seen = float(row.last_seen or 0)
            # A token carrying the Steam id wins over a name-only install.
            if row.name and row.name.strip() and (int(has_steamid), last_seen) > group["name_t"]:
                group["name"], group["name_t"] = row.name.strip(), (int(has_steamid), last_seen)
            group["last_seen"] = max(group["last_seen"], last_seen)
            if row.token in versions and last_seen >= group["ver_t"]:
                group["version"], group["ver_t"] = versions[row.token], last_seen
            if row.token in linux:
                group["platform"] = "linux"

        out = {}
        for group in groups.values():
            token_ids = ([("s:%d" % group["steamid"])] if group["steamid"] else [])
            token_ids += [clan.token_id(t) for t in group["tokens"]]
            candidates = [clans[t] for t in token_ids if t in clans]
            chosen = max(candidates, key=lambda r: (r.status == clan.ALIVE, r.weeks or 0,
                                                    r.day or 0, r.updated or 0)) if candidates else None
            custom = next((names[t] for t in token_ids if t in names and names[t][0]), None)
            if custom:
                group["names"].add(custom[0])
                group["tag"] = custom[1]
                group["display"] = "[%s] %s" % (custom[1], custom[0]) if custom[1] else custom[0]
            else:
                group["tag"] = ""
                group["display"] = group["name"] or (sorted(group["names"])[0] if group["names"] else "PLAYER")
            group["clan_key"] = chosen.key if chosen else None
            group["clan"] = self._clan_row(chosen) if chosen else None
            group["losses"] = max(0, group["matches"] - group["wins"] - group["draws"])
            group["token_ids"] = [clan.token_id(t) for t in group["tokens"]]
            group["tokens"] = len(group["tokens"])
            group["alias"] = {n.lower() for n in group["names"]} | {group["display"].lower()}
            # Nameless install that never played: noise, not a player row.
            if group["display"] == "PLAYER" and not group["matches"] and not group["clan"]:
                continue
            out[group["id"]] = group
        return out
