"""Live daemon/snapshot/log feed for the admin dashboard store."""

from __future__ import annotations

import json
import socket
import time
import urllib.error
import urllib.request

from store_common import (
    LIVE_CACHE, MATCH_SETUP_GRACE, PROBE_CACHE, SNAPSHOT_FRESH, STATUS_AGE,
    logindex, pretty_level,
)


class LiveMixin:
    """Returns live, snapshot and reconstructed log views."""

    # ---------------------------------------------------------------- server / live data
    def daemon_addr(self):
        """Configured game port, else the one the log heard, else the default."""
        addr = self.daemon or (self.log.snapshot().get("listen") if self.log else "") or "127.0.0.1:7001"
        host, _, port = addr.rpartition(":")
        try:
            return host or "127.0.0.1", int(port)
        except ValueError:
            return "127.0.0.1", 7001

    def probe(self):
        """TCP connect + close: nothing is sent, so the daemon logs nothing."""
        def run():
            host, port = self.daemon_addr()
            addr = "%s:%d" % (host, port)
            started = time.time()
            try:
                with socket.create_connection((host, port), timeout=1.5):
                    pass
                return {"open": True, "ms": round((time.time() - started) * 1000), "addr": addr}
            except OSError:
                return {"open": False, "ms": None, "addr": addr}
        return self.cached("probe", PROBE_CACHE, run)

    def secret(self):
        try:
            with open(self.secret_file, encoding="utf-8") as fh:
                return fh.read().strip()
        except OSError:
            return ""

    def admin_call(self, path, data=None, timeout=1.5):
        """Call the daemon's loopback admin listener with the shared secret."""
        secret = self.secret()
        if not secret:
            raise RuntimeError("no admin secret (the daemon creates admin-secret.txt next to the database when it starts with this build)")
        request = urllib.request.Request(
            self.admin_url + path,
            data=json.dumps(data).encode() if data is not None else None,
            method="POST" if data is not None else "GET",
        )
        request.add_header("X-Admin-Secret", secret)
        request.add_header("Content-Type", "application/json")
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return json.loads(response.read())

    def live(self):
        """Return (source, server_version, live dict or None, age seconds, unused)."""
        def run():
            if self.secret():
                try:
                    reply = self.admin_call("/live")
                    if reply.get("ok") and reply.get("live"):
                        return ("live", reply.get("server_version", ""), reply["live"], 0.0, "")
                except (OSError, ValueError, RuntimeError, urllib.error.URLError):
                    pass
            try:
                with open(self.status_file, encoding="utf-8") as fh:
                    snapshot = json.load(fh)
                age = time.time() - float(snapshot.get("ts", 0))
                if age < SNAPSHOT_FRESH and snapshot.get("live"):
                    return ("snapshot", snapshot.get("server_version", ""), snapshot["live"], age, "")
                if age < SNAPSHOT_FRESH:
                    return ("snapshot-counts", snapshot.get("server_version", ""),
                            {"players": None, "counts": {"online": snapshot.get("players_online"),
                                                         "queue": snapshot.get("queue"),
                                                         "matches": snapshot.get("matches")}}, age, "")
            except (OSError, ValueError):
                pass
            return ("log", "", None, None, "")
        return self.cached("live", LIVE_CACHE, run)

    def logsnap(self):
        if not self.log:
            return None
        self.log.refresh()
        return self.log.snapshot()

    def derived_matches(self, snap):
        """Derive matches from the log and close what the log cannot close itself."""
        if not snap:
            return []
        now = time.time()
        events = snap["events"]
        fingerprint = (len(events), events[0]["ts"] if events else 0,
                       events[-1]["ts"] if events else 0, events[-1]["kind"] if events else "")
        with self.lock:
            cached = self._matches_cache
        if cached[0] != fingerprint:
            # The log index trims old events, so the first event is part of the key too.
            cached = (fingerprint, logindex.derive_matches(events))
            with self.lock:
                self._matches_cache = cached
        matches = [dict(m) for m in cached[1]]   # cleanup only sets top-level fields

        status = snap["status"]
        idle = bool(status and now - status["ts"] < STATUS_AGE and status["matches"] == 0)
        for match in matches:
            never_started = match["state"] == "loading" and now - match["formed"] > MATCH_SETUP_GRACE
            if match["closed"] is None and (never_started or idle):
                match["closed"] = match["formed"] if match["state"] == "loading" else (status["ts"] if status else now)
                match["closed_why"] = "never started" if match["started"] is None else "ended without a result"
                if match["state"] != "ended":
                    match["state"] = "closed"
        return matches

    def derived_live(self):
        """Approximate live state from the log: open matches, last counts, recent names."""
        snap = self.logsnap()
        if not snap:
            return None
        now = time.time()
        status = snap["status"]
        fresh = bool(status and now - status["ts"] < STATUS_AGE)

        matches = []
        names_in = set()
        for match in self.derived_matches(snap):
            if match["closed"] is not None or match["state"] not in ("loading", "running"):
                continue
            gone = {p["name"] for p in match["left"] if p["why"] != "back"}
            players = []
            for i, name in enumerate(match["players"]):
                if name not in gone:
                    names_in.add(name)
                players.append({"name": name, "pid": None, "team": i % 2, "left": name in gone,
                                "host": name == match["host"], "ping_ms": None})
            matches.append({
                "id": match["id"], "state": match["state"], "age_s": round(now - match["formed"]),
                "map": {"level": match["map"], "name": pretty_level(match["map"])} if match["map"] else None,
                "mode": "", "net": match["net"], "players": players, "clock": None, "lives": None,
            })

        recent = {name for ts, name in snap["announces"] if now - ts < 60 and name not in names_in}
        template = lambda name, state: {  # noqa: E731 - one small dict shape for both kinds of row
            "name": name, "pid": None, "steamid": 0, "state": state, "match": None,
            "idle_s": None, "queued_s": None, "version": "", "platform": "", "steam_helper": None,
        }
        players = [template(name, "match") for name in sorted(names_in)]
        players += [template(name, "menu") for name in sorted(recent)]
        if fresh and status["online"] == 0:
            players = []
        counts = {"online": status["online"], "queue": status["queue"], "matches": status["matches"]} if fresh else None
        return {"players": players, "matches": matches, "queue": [], "counts": counts,
                "started": snap["started"], "approx": True}

    def live_view(self):
        """The merged live picture shared by overview / players / matches."""
        source, version, snap, age, _ = self.live()
        log_snap = self.logsnap()
        server_version = version or (log_snap["version"] if log_snap else "")

        if source in ("live", "snapshot"):
            snap = dict(snap)
            snap["counts"] = {
                "online": len(snap.get("players", [])),
                "queue": len(snap.get("queue", [])),
                "matches": len([m for m in snap.get("matches", []) if m["state"] != "ended"]),
            }
            started = snap.get("started")
        else:
            derived = self.derived_live()
            if source == "snapshot-counts":
                counts = snap.get("counts") if snap else None
                snap = derived or {"players": [], "matches": [], "queue": [], "counts": counts}
                snap["counts"] = counts or snap.get("counts")
            else:
                snap = derived or {"players": [], "matches": [], "queue": [], "counts": None}
            started = snap.get("started")

        self._track_sessions(snap)
        return {"source": source, "snapshot_age_s": age, "server_version": server_version,
                "snap": snap, "started": started}

    def _track_sessions(self, snap):
        """Online/state durations observed by this site (a lower bound, not the server's truth)."""
        now = time.time()
        with self.lock:
            current = {p["pid"] for p in snap.get("players", []) if p.get("pid")}
            for pid in list(self.seen_online):
                if pid not in current:
                    self.seen_online.pop(pid, None)
                    self.seen_state.pop(pid, None)
            for player in snap.get("players", []):
                pid = player.get("pid")
                if not pid:
                    continue
                self.seen_online.setdefault(pid, now)
                if self.seen_state.get(pid, (None,))[0] != player["state"]:
                    self.seen_state[pid] = (player["state"], now)
                player["online_s"] = round(now - self.seen_online[pid])
                player["state_s"] = round(now - self.seen_state[pid][1])
