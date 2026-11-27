"""bans.txt reads, writes and audit logging."""

from __future__ import annotations

import os
import shutil
import time

from store_common import readable_text


class BansMixin:
    """Reads, writes and audits bans.txt."""

    # ---------------------------------------------------------------- bans
    def _bans_path(self):
        return os.path.join(self.dbdir, "bans.txt")

    def _bans_lines(self):
        try:
            with open(self._bans_path(), encoding="utf-8", errors="ignore") as fh:
                return fh.read().splitlines()
        except OSError:
            return ["# bans (one per line: steam id or name)"]

    @staticmethod
    def _ban_hit(line, steamid, names):
        entry = line.split("#", 1)[0].strip()
        return bool(entry) and ((steamid and entry == str(steamid)) or
                                (not entry.isdigit() and entry.lower() in names))

    def ban_state(self, group):
        """None or {"reason", "line"} when a Steam id or one of the names is listed."""
        for line in self._bans_lines():
            if self._ban_hit(line, group["steamid"], group["alias"]):
                comment = line.split("#", 1)[1] if "#" in line else ""
                reason = comment.split("reason:", 1)[1].strip() if "reason:" in comment else ""
                return {"reason": reason, "line": line.strip()}
        return None

    def banned_index(self):
        """(steam ids, lower-case names) listed in bans.txt, for the player list."""
        ids, names = set(), set()
        for line in self._bans_lines():
            entry = line.split("#", 1)[0].strip()
            if entry and not entry.lower().startswith(("hwid:", "token:")):
                (ids if entry.isdigit() else names).add(entry if entry.isdigit() else entry.lower())
        return ids, names

    def _bans_write(self, lines):
        path = self._bans_path()
        if os.path.exists(path):
            shutil.copy2(path, path + ".bak-admin")   # state before the newest admin-site change
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(lines).rstrip("\n") + "\n")
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(tmp, path)

    def _audit(self, line):
        try:
            with open(os.path.join(self.dbdir, "admin-actions.log"), "a", encoding="utf-8") as fh:
                fh.write("%s %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), line))
        except OSError:
            pass

    def ban(self, pid, reason="", who=""):
        group = self.player_groups().get(pid or "")
        if not group:
            return {"ok": False, "error": "no such player"}
        if self.ban_state(group):
            return {"ok": False, "error": "already banned"}
        reason = readable_text(reason, 160, "\x7f#")
        who = readable_text(who, 60, "\x7f#")
        name = readable_text(group["display"], 60, "\x7f#")
        tail = "%s - admin site %s%s" % (name, time.strftime("%Y-%m-%d"), " by " + who if who else "")
        if reason:
            tail += " | reason: " + reason
        if group["steamid"]:
            entry = "%d  # %s" % (group["steamid"], tail)
        elif name and not name.isdigit():
            entry = "%s  # %s" % (name, tail)   # no Steam id: the name also bans its accounts
        else:
            return {"ok": False, "error": "this player has no Steam id or usable name to ban"}
        self._bans_write(self._bans_lines() + [entry])
        self._audit("BAN %s (%s) by %s: %s" % (name, group["steamid"] or "no steam id", who or "?", reason or "-"))
        kicked = None
        online = next((p for p in self.players_online()["players"]
                       if p.get("playerId") == pid and p.get("pid")), None)
        if online:
            kicked = self.kick(online["pid"], "Banned" + (": " + reason if reason else "")).get("ok", False)
        return {"ok": True, "name": name, "entry": entry, "kicked": kicked}

    def unban(self, pid, who=""):
        group = self.player_groups().get(pid or "")
        if not group:
            return {"ok": False, "error": "no such player"}
        lines = self._bans_lines()
        keep = [line for line in lines if not self._ban_hit(line, group["steamid"], group["alias"])]
        if len(keep) == len(lines):
            return {"ok": False, "error": "not banned"}
        self._bans_write(keep)
        self._audit("UNBAN %s (%s) by %s: removed %d line(s)" %
                    (group["display"], group["steamid"] or "no steam id", who or "?", len(lines) - len(keep)))
        return {"ok": True, "name": group["display"], "removed": len(lines) - len(keep)}
