"""Clan reset / backup forwarding to the daemon."""

from __future__ import annotations

import urllib.error


class ClanAdminMixin:
    """Forwards clan reset and backup requests."""

    # ---------------------------------------------------------------- clan admin
    def clan_reset(self, pid, confirm, names=False):
        """The daemon wipes the clan and backs it up; `confirm` must be the player's name."""
        group = self.player_groups().get(pid or "")
        if not group:
            return {"ok": False, "error": "no such player"}
        if (confirm or "").strip().lower() != group["display"].strip().lower():
            return {"ok": False, "error": "the typed name does not match"}
        try:
            result = self.admin_call("/clan/reset", {"name": group["display"], "steamid": group["steamid"],
                                                     "tokenIds": group["token_ids"], "names": bool(names)}, timeout=15)
        except RuntimeError as exc:
            return {"ok": False, "error": str(exc)}
        except (OSError, ValueError, urllib.error.URLError) as exc:
            return {"ok": False,
                    "error": "daemon admin listener not reachable (%s). The clan reset needs the daemon build that has it (restart)." % getattr(exc, "reason", exc)}
        self.invalidate("players", "clans")
        return result

    def clan_backups(self, pid):
        group = self.player_groups().get(pid or "")
        if not group:
            return {"ok": False, "error": "no such player"}
        try:
            return self.admin_call("/clan/backups?steamid=%d" % int(group["steamid"] or 0), None, timeout=5)
        except (RuntimeError, OSError, ValueError, urllib.error.URLError) as exc:
            return {"ok": False, "error": str(getattr(exc, "reason", exc)), "backups": []}

    def clan_restore(self, pid, fname):
        group = self.player_groups().get(pid or "")
        if not group:
            return {"ok": False, "error": "no such player"}
        try:
            result = self.admin_call("/clan/restore", {"file": str(fname or "")}, timeout=15)
        except (RuntimeError, OSError, ValueError, urllib.error.URLError) as exc:
            return {"ok": False, "error": str(getattr(exc, "reason", exc))}
        self.invalidate("players", "clans")
        return result
