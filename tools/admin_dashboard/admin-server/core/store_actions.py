"""Actions forwarded to the daemon's loopback admin listener."""

from __future__ import annotations

import urllib.error

from store_common import PLAYER_ID, readable_text


class ActionsMixin:
    """Sends notices, shutdowns, team changes and kicks."""

    # ---------------------------------------------------------------- daemon admin calls
    def notices(self):
        try:
            return self.admin_call("/notices")
        except (RuntimeError, OSError, ValueError, urllib.error.URLError) as exc:
            return {"ok": False, "error": str(getattr(exc, "reason", exc)), "notices": []}

    def _notice_call(self, path, data):
        try:
            return self.admin_call(path, data, timeout=5)
        except RuntimeError as exc:
            return {"ok": False, "error": str(exc)}
        except (OSError, ValueError, urllib.error.URLError) as exc:
            return {"ok": False, "error": "daemon admin listener not reachable (%s)" % getattr(exc, "reason", exc)}

    def notice_send(self, text, seconds):
        # Seconds ON SCREEN (2..600); the daemon keeps it long enough for every client's ~6 s poll.
        return self._notice_call("/notice", {"text": str(text)[:300],
                                             "seconds": max(2.0, min(float(seconds or 8), 600.0))})

    def notice_clear(self, nid):
        return self._notice_call("/notice/clear", {"id": int(nid)})

    def shutdown_schedule(self, minutes, reason, restart):
        return self._notice_call("/shutdown", {"minutes": max(0.0, min(float(minutes), 1440.0)),
                                               "reason": str(reason)[:100], "restart": bool(restart)})

    def shutdown_cancel(self):
        return self._notice_call("/shutdown/cancel", {})

    def teams(self):
        """Team rules from the daemon plus the split the queue would get now."""
        try:
            return self.admin_call("/teams", timeout=3)
        except RuntimeError as exc:
            return {"ok": False, "error": str(exc)}
        except (OSError, ValueError, urllib.error.URLError) as exc:
            return {"ok": False, "error": "daemon admin listener not reachable (%s)" % getattr(exc, "reason", exc)}

    def teams_move(self, pid):
        if not PLAYER_ID.match(pid or ""):
            return {"ok": False, "error": "bad id"}
        return self._notice_call("/teams/move", {"pid": pid})

    def teams_set(self, rules):
        return self._notice_call("/teams", {"rules": str(rules)[:4000]})

    def kick(self, pid, reason=""):
        if not PLAYER_ID.match(pid or ""):
            return {"ok": False, "error": "bad id"}
        clean = readable_text(reason, 120).strip()
        body = {"pid": pid}
        if clean:
            body["reason"] = clean   # absent when empty: older daemons understand the old request
        try:
            return self.admin_call("/kick", body, timeout=5)
        except RuntimeError as exc:
            return {"ok": False, "error": str(exc)}
        except (OSError, ValueError, urllib.error.URLError) as exc:
            return {"ok": False,
                    "error": "daemon admin listener not reachable (%s). It exists on the daemon build with the admin op and needs a daemon restart." % getattr(exc, "reason", exc)}
