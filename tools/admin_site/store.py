"""Data layer of the admin site: everything is READ-ONLY except the bug-report 'handled' flag (report store) and the kick request (forwarded to the
daemon's loopback admin listener).

Sources, best first:
  live      daemon admin listener GET /live (needs admin-secret.txt next to the DB)          real time
  snapshot  <db dir>/t2r-status.json, written by the daemon every 10 s (T2R_STATUS_FILE)       <= 10 s old
  log       reconstructed from daemon.log EVT / STATUS / ANNOUNCE lines                         approximate
The database is opened mode=ro with a short timeout for every query (no long locks, never written).
"""
import hashlib
import json
import os
import re
import shutil
import socket
import sqlite3
import sys
import subprocess
import threading
import time
import urllib.error
import urllib.request
import secrets

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)
sys.path.insert(0, HERE)
import clan  # noqa: E402
import logindex  # noqa: E402
try:
    import clan_story  # noqa: E402
except Exception:   # noqa: BLE001
    clan_story = None
try:
    import clan_alife  # noqa: E402  (the clan A-Life simulation, tools/clan_alife.py; optional)
except Exception:   # noqa: BLE001
    clan_alife = None

UPD = os.path.join(TOOLS, "update_server")        # clan_view / clanfeed_view: the SAME code that answers the launcher's /clan.json and /clanfeed.json
sys.path.insert(0, UPD)
try:
    import clan_view  # noqa: E402
    import clanfeed_view  # noqa: E402
except Exception:   # noqa: BLE001
    clan_view = clanfeed_view = None

SNAPSHOT_FRESH = 35.0
EARLY = ("timed out", "admin kick", "jip never loaded")      # departures that are not the normal leave after the result
STATUS_NAMES = {0: "none", clan.ALIVE: "alive", clan.DEAD: "dead", clan.WON: "won"}
HEX_RUN = re.compile(r"\b[0-9a-fA-F]{24,}\b")
HEX_DUMP = re.compile(r"(?:\b[0-9a-f]{2}\b ){6,}[0-9a-f]{2}\b(?: \.\.\.\(\+\d+B\))?")


def redact(line):
    """Never show tokens: long hex strings and raw byte dumps (a token travels as 64 hex chars) are masked."""
    return HEX_DUMP.sub("<bytes>", HEX_RUN.sub("<id>", line))


def pid_of(key):
    return hashlib.sha256(key.encode()).hexdigest()[:10]


def pretty_level(lvl):
    if not lvl:
        return ""
    s = re.sub(r"^(to1|to2)-", "", lvl).replace("-", " ")
    return s.title()


def token_embedded_name(token):
    """The T2N: setup name inside a token (hex of b'T2N:' + name + 0) or ''."""
    try:
        b = bytes.fromhex(token)
    except ValueError:
        return ""
    if b[:4] != b"T2N:":
        return ""
    return b[4:].split(b"\0")[0].decode("utf-8", "replace")


class Admin:
    def __init__(self, db, daemon_log="", reports="", daemon="", status_file="", admin_url="", secret_file="", update_data="", launcher_dir=""):
        self.db = os.path.abspath(db)
        self.dbdir = os.path.dirname(self.db)
        self.reports = reports
        self.log = logindex.LogIndex(daemon_log) if daemon_log else None
        self.daemon_log = daemon_log
        self.daemon = daemon                       # "host:port" of the game port (probe only: connect + close, nothing is sent)
        self.status_file = status_file or os.path.join(self.dbdir, "t2r-status.json")
        self.admin_url = admin_url or "http://127.0.0.1:7011"
        self.secret_file = secret_file or os.path.join(self.dbdir, "admin-secret.txt")
        self.update_data = update_data
        self.launcher_dir = launcher_dir
        self.lock = threading.Lock()
        self.cache = {}
        self.seen_online = {}                      # pid -> first time this site saw it online (session length lower bound)
        self.seen_state = {}                       # pid -> (state, since)
        self.live_state = {"t": 0.0, "v": None}
        self.probe_state = {"t": 0.0, "v": None}
        self._dm = (None, [])                      # (event-list key, derive_matches result): re-derived only when the log has new events
        self._alife_sum = {}                       # clan key -> (clan_alife.updated, summary): a state is decoded again only when it changed

    # ------------------------------------------------------------------ helpers
    def cached(self, key, ttl, fn):
        now = time.time()
        with self.lock:
            c = self.cache.get(key)
            if c and now - c[0] < ttl:
                return c[1]
        try:
            v = fn()
        except Exception as e:    # noqa: BLE001 - a bad read serves the previous value, flagged
            with self.lock:
                c = self.cache.get(key)
            if c:
                v = c[1]
                if isinstance(v, dict):
                    v = dict(v, stale=str(e))
                return v
            raise
        with self.lock:
            self.cache[key] = (now, v)
        return v

    def connect(self):
        c = sqlite3.connect("file:%s?mode=ro" % self.db.replace("\\", "/"), uri=True, timeout=1.5)
        c.execute("PRAGMA query_only=ON")
        return c

    def read_json(self, name, default):
        try:
            with open(os.path.join(self.dbdir, name), encoding="utf-8") as f:
                v = json.load(f)
            return v if isinstance(v, type(default)) else default
        except (OSError, ValueError):
            return default

    # ------------------------------------------------------------------ server up / live data
    def daemon_addr(self):
        a = self.daemon
        if not a and self.log:
            a = (self.log.snapshot().get("listen") or "")
        a = a or "127.0.0.1:7001"
        host, _, port = a.rpartition(":")
        try:
            return host or "127.0.0.1", int(port)
        except ValueError:
            return "127.0.0.1", 7001

    def probe(self):
        """TCP connect + close to the game port (nothing is sent, the daemon logs nothing for it). Cached 4 s."""
        def run():
            host, port = self.daemon_addr()
            t0 = time.time()
            try:
                with socket.create_connection((host, port), timeout=1.5):
                    pass
                return {"open": True, "ms": round((time.time() - t0) * 1000), "addr": "%s:%d" % (host, port)}
            except OSError:
                return {"open": False, "ms": None, "addr": "%s:%d" % (host, port)}
        return self.cached("probe", 4.0, run)

    def secret(self):
        try:
            with open(self.secret_file, encoding="utf-8") as f:
                return f.read().strip()
        except OSError:
            return ""

    def admin_call(self, path, data=None, timeout=1.5):
        sec = self.secret()
        if not sec:
            raise RuntimeError("no admin secret (the daemon creates admin-secret.txt next to the database when it starts with this build)")
        req = urllib.request.Request(self.admin_url + path, data=json.dumps(data).encode() if data is not None else None,
                                     method="POST" if data is not None else "GET")
        req.add_header("X-Admin-Secret", sec)
        req.add_header("Content-Type", "application/json")
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read())

    def live(self):
        """(source, server_version, snapshot dict or None, age seconds)."""
        def run():
            if self.secret():
                try:
                    j = self.admin_call("/live")
                    if j.get("ok") and j.get("live"):
                        return ("live", j.get("server_version", ""), j["live"], 0.0, "")
                except (OSError, ValueError, RuntimeError, urllib.error.URLError):
                    pass
            try:
                with open(self.status_file, encoding="utf-8") as f:
                    d = json.load(f)
                age = time.time() - float(d.get("ts", 0))
                if age < SNAPSHOT_FRESH and d.get("live"):
                    return ("snapshot", d.get("server_version", ""), d["live"], age, "")
                if age < SNAPSHOT_FRESH:
                    return ("snapshot-counts", d.get("server_version", ""),
                            {"players": None, "counts": {"online": d.get("players_online"), "queue": d.get("queue"), "matches": d.get("matches")}}, age, "")
            except (OSError, ValueError):
                pass
            return ("log", "", None, None, "")
        return self.cached("live", 2.0, run)

    # ------------------------------------------------------------------ log-derived live
    def logsnap(self):
        if not self.log:
            return None
        self.log.refresh()
        return self.log.snapshot()

    def derived_matches(self, ls):
        """derive_matches + cleanup of what the log cannot close by itself: a formed match that never started (cancelled countdown) is closed after
        5 min, and while the newest STATUS line (fresh) says matches=0 nothing is running."""
        if not ls:
            return []
        now = time.time()
        ev = ls["events"]
        key = (len(ev), ev[0]["ts"] if ev else 0, ev[-1]["ts"] if ev else 0, ev[-1]["kind"] if ev else "")
        with self.lock:
            hit = self._dm
        if hit[0] != key:                          # the log index trims old events, so the first event is part of the key too
            hit = (key, logindex.derive_matches(ev))
            with self.lock:
                self._dm = hit
        ms = [dict(m) for m in hit[1]]             # the cleanup below sets top-level fields only: a shallow copy keeps the cached list clean
        st = ls["status"]
        idle = bool(st and now - st["ts"] < 45 and st["matches"] == 0)
        for m in ms:
            if m["closed"] is None and ((m["state"] == "loading" and now - m["formed"] > 300) or idle):
                m["closed"] = m["formed"] if m["state"] == "loading" else (st["ts"] if st else now)
                m["closed_why"] = "never started" if m["started"] is None else "ended without a result"
                if m["state"] != "ended":
                    m["state"] = "closed"
        return ms

    def derived_live(self):
        """Approximate live state from the log: open matches, last STATUS counts, recent ANNOUNCE names."""
        ls = self.logsnap()
        if not ls:
            return None
        now = time.time()
        ms = self.derived_matches(ls)
        run = [m for m in ms if m["closed"] is None and m["state"] in ("loading", "running")]
        st = ls["status"]
        fresh = bool(st and now - st["ts"] < 45)
        names_in = set()
        out_m = []
        for m in run:
            gone = {l["name"] for l in m["left"] if l["why"] != "back"}
            pl = []
            for i, n in enumerate(m["players"]):
                left = n in gone
                if not left:
                    names_in.add(n)
                pl.append({"name": n, "pid": None, "team": i % 2, "left": left, "host": n == m["host"], "ping_ms": None})
            out_m.append({"id": m["id"], "state": m["state"], "age_s": round(now - m["formed"]), "map": {"level": m["map"], "name": pretty_level(m["map"])} if m["map"] else None,
                          "mode": "", "net": m["net"], "players": pl, "clock": None, "lives": None})
        recent = {n for ts, n in ls["announces"] if now - ts < 60 and n not in names_in}
        players = [{"name": n, "pid": None, "steamid": 0, "state": "match", "match": None, "idle_s": None, "queued_s": None, "version": "", "platform": "",
                    "steam_helper": None} for n in sorted(names_in)]
        players += [{"name": n, "pid": None, "steamid": 0, "state": "menu", "match": None, "idle_s": None, "queued_s": None, "version": "", "platform": "",
                     "steam_helper": None} for n in sorted(recent)]
        if fresh and st["online"] == 0:
            players = []
        return {"players": players, "matches": out_m, "queue": [], "counts": ({"online": st["online"], "queue": st["queue"], "matches": st["matches"]} if fresh else None),
                "started": ls["started"], "approx": True}

    def live_view(self):
        """The merged live picture used by overview / players / matches."""
        src, ver, snap, age, _ = self.live()
        ls = self.logsnap()
        server_version = ver or (ls["version"] if ls else "")
        if src in ("live", "snapshot"):
            snap = dict(snap)
            snap["counts"] = {"online": len(snap.get("players", [])), "queue": len(snap.get("queue", [])), "matches": len([m for m in snap.get("matches", []) if m["state"] != "ended"])}
            started = snap.get("started")
        else:
            d = self.derived_live()
            if src == "snapshot-counts":
                counts = snap.get("counts") if snap else None
                snap = d or {"players": [], "matches": [], "queue": [], "counts": counts}
                snap["counts"] = counts or snap.get("counts")
            else:
                snap = d or {"players": [], "matches": [], "queue": [], "counts": None}
            started = snap.get("started")
        # since-trackers (this site's own observation: a lower bound)
        now = time.time()
        with self.lock:
            cur = {p["pid"] for p in snap.get("players", []) if p.get("pid")}
            for k in list(self.seen_online):
                if k not in cur:
                    self.seen_online.pop(k, None); self.seen_state.pop(k, None)
            for p in snap.get("players", []):
                k = p.get("pid")
                if not k:
                    continue
                self.seen_online.setdefault(k, now)
                if self.seen_state.get(k, (None,))[0] != p["state"]:
                    self.seen_state[k] = (p["state"], now)
                p["online_s"] = round(now - self.seen_online[k])
                p["state_s"] = round(now - self.seen_state[k][1])
        return {"source": src, "snapshot_age_s": age, "server_version": server_version, "snap": snap, "started": started}

    # ------------------------------------------------------------------ database
    def player_groups(self):
        """One entry per player: the Steam account when known, else the install id (like the daemon's leaderboard)."""
        def run():
            c = self.connect()
            try:
                rows = c.execute("SELECT token,name,kills,deaths,wins,matches,steamid,last_seen,parts_total,parts_n,draws FROM players "
                                 "WHERE length(token)=64").fetchall()
                names = {}
                try:
                    for tid, n, tg in c.execute("SELECT token_id,name,tag FROM names"):
                        names[tid] = (n or "", tg or "")
                except sqlite3.Error:
                    pass
                clans = {}
                try:
                    for r in c.execute("SELECT token_id,faction,status,day,healthy,hungry,sick,weeks_total,runs_won,runs_lost,last_result,meta,updated FROM clans"):
                        clans[r[0]] = r
                except sqlite3.Error:
                    pass
            finally:
                c.close()
            versions = self.read_json("client_versions.json", {})
            linux = set(self.read_json("linux_clients.json", []))
            groups = {}
            norm = lambda n: " ".join((n or "").lower().split())      # noqa: E731
            # One row per PERSON: the Steam account when known (what the clan DB keys by); tokens without one (reinstalls, name-only installs) join the Steam
            # account that owns the same name when exactly one does, else are merged by normalized name + platform.
            steam_of_name = {}
            for tok, name, _k, _d, _w, _m, sid, *_ in rows:
                if sid and sid > 0:
                    for n in (norm(name), norm(token_embedded_name(tok))):
                        if n:
                            steam_of_name.setdefault(n, set()).add(int(sid))
            def person_key(tok, name, sid):
                if sid and sid > 0:
                    return "s%d" % sid
                nm = norm(name) or norm(token_embedded_name(tok))
                if not nm:
                    return "t" + tok[40:64]
                owners = steam_of_name.get(nm, ())
                if len(owners) == 1:
                    return "s%d" % next(iter(owners))
                return "n:%s:%s" % (nm, "linux" if tok in linux else "windows")
            for tok, name, k, d, w, m, sid, ls, pt, pn, dr in rows:
                key = person_key(tok, name, sid)
                sid_orig = bool(sid and sid > 0)          # names of tokens that carry the Steam id win over name-only installs
                if key.startswith("s") and not (sid and sid > 0):
                    sid = int(key[1:])
                g = groups.setdefault(key, {"key": key, "id": pid_of(key), "steamid": int(sid or 0), "tokens": [], "names": set(), "kills": 0, "deaths": 0, "wins": 0,
                                           "matches": 0, "draws": 0, "parts_total": 0, "parts_n": 0, "last_seen": 0.0, "version": "", "platform": "windows", "ver_t": 0.0, "tokens_n": 0,
                                           "name": "", "name_t": (-1, -1.0)})
                g["tokens"].append(tok)
                g["steamid"] = g["steamid"] or int(sid or 0)
                if name:
                    g["names"].add(name)
                e = token_embedded_name(tok)
                if e:
                    g["names"].add(e)
                for f, v in (("kills", k), ("deaths", d), ("wins", w), ("matches", m), ("draws", dr), ("parts_total", pt), ("parts_n", pn)):
                    g[f] += int(v or 0)
                ls = float(ls or 0)
                if name and name.strip() and (1 if sid_orig else 0, ls) > g["name_t"]:
                    g["name"], g["name_t"] = name.strip(), ((1 if sid_orig else 0), ls)
                g["last_seen"] = max(g["last_seen"], ls)
                if tok in versions and ls >= g["ver_t"]:
                    g["version"], g["ver_t"] = versions[tok], ls
                if tok in linux:
                    g["platform"] = "linux"
            out = {}
            for key, g in groups.items():
                toks = sorted(g["tokens"], key=lambda t: 0)
                tids = [("s:%d" % g["steamid"])] if g["steamid"] else []
                tids += [clan.token_id(t) for t in g["tokens"]]
                cands = [clans[t] for t in tids if t in clans]
                cl = max(cands, key=lambda r: (r[2] == clan.ALIVE, r[7] or 0, r[3] or 0, r[12] or 0)) if cands else None
                custom = next((names[t] for t in tids if t in names and names[t][0]), None)
                if custom:
                    g["names"].add(custom[0])
                    g["tag"] = custom[1]
                    g["display"] = ("[%s] %s" % (custom[1], custom[0])) if custom[1] else custom[0]
                else:
                    g["tag"] = ""
                    g["display"] = g["name"] or (sorted(g["names"])[0] if g["names"] else "PLAYER")
                g["clan_key"] = cl[0] if cl else None
                g["clan"] = self._clan_row(cl) if cl else None
                g["losses"] = max(0, g["matches"] - g["wins"] - g["draws"])
                g["token_ids"] = [clan.token_id(t) for t in g["tokens"]]       # install ids (24 hex, not the secret token): the clan reset clears every key of the person
                g["tokens"] = len(toks)
                g["alias"] = {n.lower() for n in g["names"]} | {g["display"].lower()}
                if g["display"] == "PLAYER" and not g["matches"] and not g["clan"]:      # nameless install that never played: noise
                    continue
                out[g["id"]] = g
            return out
        return self.cached("players", 4.0, run)

    @staticmethod
    def _clan_row(r):
        tid, fac, status, day, h, u, s, weeks, rw, rl, last, meta, upd = r
        size = int(h or 0) + int(u or 0) + int(s or 0)
        try:
            lr = json.loads(last or "{}")
        except ValueError:
            lr = {}
        alive = status == clan.ALIVE
        wk, dw = clan.week_day(int(day or 0)) if alive else (0, 0)
        risk = []
        if alive:
            if size <= 2:
                risk.append("tiny clan")
            if size and (int(u or 0) + int(s or 0)) >= 0.6 * size:
                risk.append("%d of %d hungry or sick" % (int(u or 0) + int(s or 0), size))
            if int(lr.get("died", 0) or 0) > 0:
                risk.append("%d died last match" % int(lr.get("died", 0)))
        return {"faction": int(fac or 0), "factionName": clan.FACTION_NAMES.get(int(fac or 0), ""), "status": STATUS_NAMES.get(int(status or 0), "none"),
                "day": int(day or 0), "daysTotal": clan.WIN_DAY, "week": wk, "dayOfWeek": dw, "weeks": clan.WEEKS, "size": size, "healthy": int(h or 0),
                "hungry": int(u or 0), "sick": int(s or 0), "needed": clan.needed(size), "weeksTotal": int(weeks or 0), "runsWon": int(rw or 0), "runsLost": int(rl or 0),
                "lastParts": lr.get("parts"), "lastSupplies": lr.get("supplies"), "lastDied": lr.get("died"), "risk": risk, "updated": upd, "key": tid}

    # ------------------------------------------------------------------ views
    def overview(self):
        lv = self.live_view()
        snap = lv["snap"]
        probe = self.probe()
        ls = self.logsnap()
        now = time.time()
        counts = snap.get("counts") or {}
        players = snap.get("players") or []
        by_state = {"menu": 0, "queue": 0, "match": 0}
        for p in players:
            by_state[p["state"]] = by_state.get(p["state"], 0) + 1
        online = counts.get("online")
        if online is None:
            online = len(players) if lv["source"] != "log" or players else None
        server_up = bool(probe["open"] or lv["source"] in ("live", "snapshot", "snapshot-counts"))
        fresh_log = False
        if ls and ls["status"] and now - ls["status"]["ts"] < 45:
            fresh_log = True
        started = lv["started"] or (ls["started"] if ls else None)
        uptime = round(now - started) if (started and server_up) else None
        latest = self.latest_version()
        sv = lv["server_version"]
        pg = self.player_groups() if os.path.exists(self.db) else {}
        clients = {}
        for p in players:
            v = p.get("version") or "?"
            clients[v] = clients.get(v, 0) + 1
        outdated = sum(n for v, n in clients.items() if sv and v not in (sv, "?"))
        matches_running = counts.get("matches") if counts.get("matches") is not None else len([m for m in snap.get("matches", []) if m["state"] != "ended"])
        notes = []
        if lv["source"] == "log":
            notes.append("No live feed from the daemon (it needs a restart on the build with the admin listener). Online / match data is reconstructed from the log and is approximate.")
        elif lv["source"] == "snapshot":
            notes.append("Data comes from the daemon's 10 s snapshot (the admin listener is not reachable or no admin-secret.txt: kick is unavailable).")
        elif lv["source"] == "snapshot-counts":
            notes.append("The snapshot only holds counts (older daemon build); names come from the log.")
        errs = []
        if ls:
            errs = [{"ts": e["ts"], "line": redact(e["line"])} for e in ls["errors"][-8:]][::-1]
        clans = [g["clan"] for g in pg.values() if g["clan"]]
        seen_keys = {}
        for g in pg.values():
            if g["clan"]:
                seen_keys[g["clan"]["key"]] = g["clan"]
        ms = self.derived_matches(ls)
        return {"now": now, "source": lv["source"], "snapshotAge": lv["snapshot_age_s"], "notes": notes,
                "server": {"up": server_up, "probe": probe, "version": sv, "latest": latest, "uptime_s": uptime, "started": started,
                          "logFresh": fresh_log, "logPath": self.cur_log()},
                "online": {"total": online, "menu": by_state["menu"], "queue": counts.get("queue") if counts.get("queue") is not None else by_state["queue"],
                           "match": by_state["match"]},
                "matchesRunning": matches_running,
                "clients": {"versions": clients, "outdated": outdated},
                "totals": {"players": len(pg), "clans": len(seen_keys), "clansAlive": len([c for c in seen_keys.values() if c["status"] == "alive"]),
                           "matchesLogged": len([m for m in ms if m["state"] == "ended"]),
                           "matches24h": len([m for m in ms if m["state"] == "ended" and now - m["ended"] < 86400]),
                           "reportsNew": len([1 for _, m in self.report_metas() if not m.get("handled")]) if self.reports else 0},
                "errors": errs, "canKick": lv["source"] == "live"}

    def latest_version(self):
        if not self.update_data:
            return ""
        try:
            with open(os.path.join(self.update_data, "channels.json"), encoding="utf-8") as f:
                ch = json.load(f)
            return str(ch.get("stable") or next(iter(ch.values()), ""))
        except (OSError, ValueError, StopIteration, AttributeError):
            return ""

    def players_online(self):
        lv = self.live_view()
        pg = self.player_groups() if os.path.exists(self.db) else {}
        by_steam = {g["steamid"]: g for g in pg.values() if g["steamid"]}
        sv = lv["server_version"]
        out = []
        pings = {}
        for m in lv["snap"].get("matches", []):
            for mp in m.get("players", []):
                if mp.get("ping_ms") is not None and mp.get("pid"):
                    pings[mp["pid"]] = mp["ping_ms"]
        for p in lv["snap"].get("players", []):
            g = by_steam.get(p.get("steamid")) or next((x for x in pg.values() if p["name"].lower() in x["alias"]), None)
            q = dict(p)
            q["ping"] = pings.get(p.get("pid"))
            if g:
                q["playerId"] = g["id"]
                q["clan"] = ({"factionName": g["clan"]["factionName"], "week": g["clan"]["week"], "day": g["clan"]["dayOfWeek"], "size": g["clan"]["size"]}
                             if g["clan"] and g["clan"]["status"] == "alive" else None)
                q["tag"] = g["tag"]
                if not q.get("version"):
                    q["version"] = g["version"]
                if not q.get("platform"):
                    q["platform"] = g["platform"]
            q["outdated"] = bool(sv and q.get("version") and q["version"] != sv)
            out.append(q)
        order = {"match": 0, "queue": 1, "menu": 2}
        best = {}
        for q in out:                      # one row per person (several tokens / connections): the most active live one wins
            k = q.get("playerId") or "n:" + q["name"].lower()
            cur = best.get(k)
            if cur is None or (order.get(q["state"], 3), q.get("idle_s") or 0) < (order.get(cur["state"], 3), cur.get("idle_s") or 0):
                best[k] = q
        out = list(best.values())
        out.sort(key=lambda x: (order.get(x["state"], 3), x["name"].lower()))
        return {"source": lv["source"], "players": out, "serverVersion": sv, "canKick": lv["source"] == "live"}

    def players_all(self, q="", limit=300):
        pg = self.player_groups()
        ql = (q or "").lower().strip()
        lv = self.live_view()
        online_names = {p["name"].lower() for p in lv["snap"].get("players", [])}
        online_sid = {p.get("steamid") for p in lv["snap"].get("players", []) if p.get("steamid")}
        rows, hidden = [], 0
        ban_ids, ban_names = self.banned_index()
        for g in pg.values():
            if ql and not (ql in g["display"].lower() or any(ql in a for a in g["alias"]) or ql in str(g["steamid"])):
                continue
            c = g["clan"]
            online = bool(g["steamid"] in online_sid or (g["alias"] & online_names))
            # stale leftovers: installs that never finished a match - old tokens from reinstalls (a truncated name next to the full one)
            # or people who never got in (a clan picked at CHOOSE YOUR SIDE has no progress without a match).
            # Hidden unless searched for or online.
            if not ql and not online and not g["matches"]:
                hidden += 1
                continue
            rows.append({"id": g["id"], "name": g["display"], "steamid": g["steamid"], "matches": g["matches"], "wins": g["wins"], "losses": g["losses"], "draws": g["draws"],
                         "lastSeen": g["last_seen"], "version": g["version"], "platform": g["platform"],
                         "online": online, "banned": bool(str(g["steamid"]) in ban_ids or (g["alias"] & ban_names)),
                         "faction": c["factionName"] if c else "", "clanStatus": c["status"] if c else "", "week": c["week"] if c else 0,
                         "day": c["dayOfWeek"] if c else 0, "size": c["size"] if c else 0})
        rows.sort(key=lambda r: (-r["lastSeen"],))
        return {"total": len(pg), "shown": min(len(rows), limit), "hidden": hidden, "players": rows[:limit],
                "hasKd": any(g["kills"] or g["deaths"] for g in pg.values())}

    def player(self, pid):
        pg = self.player_groups()
        g = pg.get(pid)
        if not g:
            return {"ok": False, "error": "no such player"}
        ls = self.logsnap()
        ms = self.derived_matches(ls)
        hist = []
        for m in reversed(ms):
            nm = next((n for n in m["players"] if n.lower() in g["alias"]), None)
            if nm is None or m["started"] is None:
                continue
            hist.append({"id": m["id"], "when": m["formed"], "map": pretty_level(m["map"]), "state": m["state"], "result": logindex.result_for(m, nm),
                         "secs": m["secs"], "players": len(m["players"]), "left": any(l["name"] == nm and l["why"] in EARLY for l in m["left"])})
            if len(hist) >= 40:
                break
        feed, people, alife = [], {}, {}
        if g["clan_key"] and os.path.exists(self.db):
            try:
                c = self.connect()
                try:
                    if clan_story:
                        feed = clan_story.read_feed(c, g["clan_key"], 40)
                        people = clan_story.read_people(c, g["clan_key"], 40)
                        pl = {m["id"]: m["text"] for m in clan_story.read_feed(c, g["clan_key"], 40, lang="pl") if m["lang"] == "pl"}
                        for m in feed:                              # the Polish form of every row that has one (what a player on the Polish text sees)
                            m["textPl"] = pl.get(m["id"], "")
                    if clan_alife:
                        alife = clan_alife.view(c, g["clan_key"])
                finally:
                    c.close()
            except sqlite3.Error:
                pass
        reps = []
        for d, m in self.report_metas():
            if (m.get("name") or "").lower() in g["alias"] or (g["steamid"] and str(m.get("steamId", "")) == str(g["steamid"])):
                reps.append({"code": m.get("code"), "received": m.get("received"), "text": (m.get("text") or "")[:160], "handled": bool(m.get("handled"))})
        online = next((p for p in self.players_online()["players"] if p.get("playerId") == pid), None)
        pm = g["parts_total"] / g["parts_n"] if g["parts_n"] else None
        return {"ok": True, "id": pid, "name": g["display"], "tag": g["tag"], "aliases": sorted(g["names"]), "steamid": g["steamid"], "installs": g["tokens"],
                "matches": g["matches"], "wins": g["wins"], "losses": g["losses"], "draws": g["draws"], "kills": g["kills"], "deaths": g["deaths"],
                "avgParts": round(pm) if pm else None, "lastSeen": g["last_seen"], "version": g["version"], "platform": g["platform"], "clan": g["clan"],
                "morale": people.get("morale"), "people": people.get("people", [])[:12], "alive": people.get("alive"), "feed": feed, "alife": alife,
                "history": hist, "reports": reps, "online": online, "ban": self.ban_state(g), "alifeServerOn": clan_alife.enabled() if clan_alife else False,
                "matchLogFrom": (self.log.earliest() if self.log else None), "canKick": bool(online and online.get("pid") and self.live()[0] == "live")}

    def matches(self):
        lv = self.live_view()
        ls = self.logsnap()
        now = time.time()
        live = [m for m in lv["snap"].get("matches", []) if m["state"] != "ended"]
        ms = self.derived_matches(ls)
        hist = []
        for m in reversed(ms):
            if m["closed"] is None or m["started"] is None:      # still open (shown under LIVE) / a countdown that never started
                continue
            t0, t1 = logindex.team_names(m)
            res = ""
            if m["state"] == "ended":
                res = {0: "TEAM 1 WON", 1: "TEAM 2 WON", 2: "DRAW"}.get(m["winner"], "RESULT UNKNOWN")
            else:
                res = "NOT FINISHED (%s)" % (m["closed_why"] or "closed")
            hist.append({"id": m["id"], "formed": m["formed"], "started": m["started"], "ended": m["ended"], "map": pretty_level(m["map"]), "net": m["net"], "ping_ms": m["ping_ms"],
                         "team1": t0, "team2": t1, "result": res, "winner": m["winner"], "secs": m["secs"], "host": m["host"], "state": m["state"],
                         "left": [l["name"] + " (" + l["why"] + ")" for l in m["left"] if l["why"] in EARLY], "joined": m["jip"]})
            if len(hist) >= 60:
                break
        return {"source": lv["source"], "live": live, "history": hist, "approx": lv["source"] == "log", "canKick": lv["source"] == "live"}

    def clanview(self, pid="", lang="en"):
        """What the launcher's CLAN tab gets for this player (the /clan.json + /clanfeed.json payloads), for the read-only clan page.
        `pid` = a player id of the PLAYERS tab (the account's own clan row, whichever install it was played from)."""
        if not clan_view or not clanfeed_view:
            return {"ok": False, "error": "update_server clan views not available"}
        g = self.player_groups().get(pid)
        if not g:
            return {"ok": False, "error": "no such player"}
        if not g["clan_key"]:
            return {"ok": True, "name": g["display"], "clan": {"ok": True, "hasClan": False}, "feed": {"ok": True, "hasClan": False, "feed": []}}
        lang = "pl" if lang == "pl" else "en"
        k = g["clan_key"]
        return {"ok": True, "name": g["display"], "clan": clan_view.view(self.db, "", key=k),
                "feed": clanfeed_view.view(self.db, "", 40, 0, lang, key=k)}

    def clans(self):
        return self.cached("clans", 10.0, self._clans)

    def _alife_summaries(self, c, keys):
        """{clan key: clan_alife.summary}; a state (about 4 KB of JSON each) is decoded only when its `updated` changed since the last call."""
        try:
            upd = dict(c.execute("SELECT token_id,updated FROM clan_alife").fetchall())
        except sqlite3.Error:
            return {}
        out = {}
        for k in keys:
            if k not in upd:
                out[k] = {}
                continue
            with self.lock:
                hit = self._alife_sum.get(k)
            if hit and hit[0] == upd[k]:
                out[k] = hit[1]
                continue
            sm = clan_alife.summary(c, k)
            with self.lock:
                self._alife_sum[k] = (upd[k], sm)
            out[k] = sm
        return out

    def _clans(self):
        pg = self.player_groups()
        rows = []
        seen = set()
        for g in pg.values():
            c = g["clan"]
            if not c or c["key"] in seen:
                continue
            seen.add(c["key"])
            rows.append(dict(c, owner=g["display"], ownerId=g["id"], key=None, lastSeen=g["last_seen"]))
        # clans whose owner is not in the players table (no player row): shown unnamed
        rows.sort(key=lambda r: (r["status"] != "alive", -(r["weeksTotal"] * 100 + r["day"])))
        feed = []
        try:
            c = self.connect()
            try:
                owners = {}
                for g in pg.values():
                    if g["clan_key"]:
                        owners[g["clan_key"]] = g["display"]
                for r in c.execute("SELECT token_id,id,ts,day,week,kind,mood,faction,text FROM clan_feed ORDER BY ts DESC LIMIT 40"):
                    feed.append({"owner": owners.get(r[0], "?"), "id": r[1], "ts": r[2], "day": r[3], "week": r[4], "kind": r[5], "mood": r[6], "faction": r[7],
                                 "text": r[8] if len(r[8] or "") < 400 else r[8][:400] + "..."})
            except sqlite3.Error:
                feed = None
            finally:
                c.close()
        except sqlite3.Error:
            feed = None
        try:
            c = self.connect()
            try:
                if clan_alife:
                    # (one dict lookup per player instead of a scan of every row, and cached summaries: this loop was 75 % of the site's CPU)
                    by_owner = {}
                    for r in rows:
                        by_owner.setdefault(r["ownerId"], []).append(r)
                    sums = self._alife_summaries(c, {g["clan_key"] for g in pg.values() if g["clan_key"]})
                    for g in pg.values():
                        k = g["clan_key"]
                        if k:
                            for r in by_owner.get(g["id"], ()):
                                r["alife"] = sums.get(k, {})
            finally:
                c.close()
        except sqlite3.Error:
            pass
        by = {"WLF": 0, "SERAPHITES": 0}
        for r in rows:
            if r["status"] == "alive" and r["factionName"] in by:
                by[r["factionName"]] += 1
        return {"clans": rows, "feed": feed, "alive": len([r for r in rows if r["status"] == "alive"]), "byFaction": by,
                "atRisk": len([r for r in rows if r["risk"]]), "feedAvailable": feed is not None}

    # ------------------------------------------------------------------ reports
    def report_metas(self):
        if not self.reports or not os.path.isdir(self.reports):
            return []
        out = []
        for d in sorted(os.listdir(self.reports), reverse=True):
            p = os.path.join(self.reports, d, "meta.json")
            if os.path.isfile(p):
                try:
                    with open(p, encoding="utf-8") as f:
                        out.append((os.path.join(self.reports, d), json.load(f)))
                except (OSError, ValueError):
                    pass
        return out

    def reports_list(self):
        rows = []
        for d, m in self.report_metas():
            rows.append({"code": m.get("code"), "received": m.get("received"), "name": m.get("name"), "version": m.get("version"), "mapMode": m.get("mapMode"),
                         "text": (m.get("text") or "").strip().splitlines()[0][:120] if (m.get("text") or "").strip() else "", "handled": bool(m.get("handled")),
                         "files": len(m.get("files", []))})
        uc = secrets.token_urlsafe(32)
        with open("reports.txt", "a") as reports_file:
            reports_file.write(uc)
        return {"reports": rows, "configured": bool(self.reports), "new": len([r for r in rows if not r["handled"]]), "uniqueCode": uc}

    def find_report(self, code):
        code = (code or "").upper()
        if not re.match(r"^R-[A-Z0-9]{4}$", code):
            return None
        for d, m in self.report_metas():
            if (m.get("code") or "").upper() == code:
                return d, m
        return None

    def report(self, code):
        f = self.find_report(code)
        if not f:
            return {"ok": False, "error": "no such report"}
        d, m = f
        m = dict(m)
        m.pop("clipKey", None)           # the upload key is a secret of the player's launcher, never shown
        return {"ok": True, "meta": m, "files": [dict(x, text=x["name"].lower().endswith((".txt", ".log", ".json", ".md", ".ini", ".cfg", ".csv", ".xml")),
                                                      image=x.get("kind") == "screenshot", video=x.get("kind") == "video")
                                                for x in m.get("files", [])]}

    # Bug-report files can live on a storage box (tools/update_server/report_offload.py moves them; <report>/store.json says which box and
    # which files). A moved file is copied back into a small local cache on first use. T2R_REPORT_BOXES = the boxes JSON file the mover uses.
    REPORT_CACHE_MAX = int(os.environ.get("T2R_REPORT_CACHE_MB", "800")) << 20
    _report_fetch_lock = threading.Lock()

    def _report_boxes(self):
        path = os.environ.get("T2R_REPORT_BOXES", "")
        if not path:
            return None
        try:
            with open(os.path.expanduser(path), encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return None

    def _report_cache_dir(self):
        # never under update-data: that folder is what the update server serves
        return os.environ.get("T2R_REPORT_CACHE") or os.path.expanduser("~/.cache/t2r-report-cache")

    def _report_cache_trim(self, keep):
        root = self._report_cache_dir()
        ents = []
        for dp, _dn, fn in os.walk(root):
            for f in fn:
                p = os.path.join(dp, f)
                try:
                    st = os.stat(p)
                except OSError:
                    continue
                ents.append((st.st_atime, st.st_size, p))
        total = sum(e[1] for e in ents)
        for _t, size, p in sorted(ents):
            if total <= self.REPORT_CACHE_MAX:
                break
            if p == keep:
                continue
            try:
                os.remove(p)
                total -= size
            except OSError:
                pass

    def report_local_path(self, d, name):
        """Readable local path of files/<name> of report folder d: the file itself, or a cached copy fetched from its storage box; None if
        neither works. name must already be a listed file name of the report (callers check meta.json)."""
        if not name or "/" in name or "\\" in name or name.startswith("."):
            return None
        p = os.path.join(d, "files", name)
        if os.path.isfile(p):
            return p
        try:
            with open(os.path.join(d, "store.json"), encoding="utf-8") as f:
                st = json.load(f)
            want = int(st["files"][name])
            boxname = st["box"]
        except (OSError, ValueError, KeyError, TypeError):
            return None
        folder = os.path.basename(os.path.normpath(d))
        cp = os.path.join(self._report_cache_dir(), folder, name)
        with self._report_fetch_lock:
            try:
                if os.path.getsize(cp) == want:
                    os.utime(cp)
                    return cp
            except OSError:
                pass
            cfg = self._report_boxes()
            box = next((b for b in (cfg or {}).get("boxes", []) if b.get("name") == boxname), None)
            if not box:
                return None
            os.makedirs(os.path.dirname(cp), exist_ok=True)
            tmp = cp + ".part"
            ssh = "ssh -i %s -o BatchMode=yes -o StrictHostKeyChecking=yes -o UserKnownHostsFile=%s -o ConnectTimeout=10" % (
                os.path.expanduser(cfg.get("key", "~/.ssh/t2r_reports")), os.path.expanduser(cfg.get("known_hosts", "~/.ssh/known_hosts_reports")))
            try:
                r = subprocess.run(["rsync", "-t", "--timeout=60", "-e", ssh, "%s:%s/files/%s" % (box["host"], folder, name), tmp],
                                   capture_output=True, timeout=300)
                if r.returncode != 0 or os.path.getsize(tmp) != want:
                    raise OSError("fetch failed")
                os.replace(tmp, cp)
            except (OSError, subprocess.SubprocessError):
                try:
                    os.remove(tmp)
                except OSError:
                    pass
                return None
            self._report_cache_trim(cp)
            return cp

    IMAGE_TYPES = {"png": "image/png", "jpg": "image/jpeg", "webp": "image/webp", "gif": "image/gif", "bmp": "image/bmp"}

    def report_video(self, code, name):
        """(path, content-type, size) of an attached clip, or None: only files recorded as video by the server, exact listed name, and the bytes
        must start like an mp4/mov (ftyp) or webm/mkv (EBML)."""
        f = self.find_report(code)
        if not f:
            return None
        d, m = f
        ent = next((x for x in m.get("files", []) if x.get("name") == name and x.get("kind") == "video"), None)
        if not ent:
            return None
        p = self.report_local_path(d, name)
        if not p:
            return None
        try:
            with open(p, "rb") as fh:
                h = fh.read(64)
            size = os.path.getsize(p)
        except OSError:
            return None
        if len(h) >= 12 and h[4:8] == b"ftyp":
            ct = "video/quicktime" if h[8:12] == b"qt  " else "video/mp4"
        elif h[:4] == bytes((0x1A, 0x45, 0xDF, 0xA3)):
            ct = "video/webm" if b"webm" in h else "video/x-matroska"
        else:
            return None
        return p, ct, size

    def report_image(self, code, name):
        """(content-type, bytes) of an attached screenshot, or None. Only files the server recorded as screenshots, by exact listed name, and only if
        the bytes really are a png/jpg/webp/gif/bmp (the type comes from the bytes, never from the name)."""
        f = self.find_report(code)
        if not f:
            return None
        d, m = f
        if not any(x.get("name") == name and x.get("kind") == "screenshot" for x in m.get("files", [])):
            return None
        p = self.report_local_path(d, name)
        if not p:
            return None
        try:
            with open(p, "rb") as fh:
                data = fh.read(8 * 1024 * 1024 + 1)
        except OSError:
            return None
        if len(data) > 8 * 1024 * 1024:
            return None
        h = data[:16]
        kind = ("png" if h[:8] == b"\x89PNG\r\n\x1a\n" else "jpg" if h[:3] == b"\xff\xd8\xff" else "webp" if h[:4] == b"RIFF" and h[8:12] == b"WEBP"
                else "gif" if h[:6] in (b"GIF87a", b"GIF89a") else "bmp" if h[:2] == b"BM" else None)
        return (self.IMAGE_TYPES[kind], data) if kind else None

    def report_file(self, code, name, limit=96 * 1024):
        f = self.find_report(code)
        if not f:
            return None
        d, m = f
        if name not in {x["name"] for x in m.get("files", [])}:
            return None
        p = self.report_local_path(d, name)
        if not p:
            return None
        try:
            size = os.path.getsize(p)
            with open(p, "rb") as fh:
                if size > limit:
                    fh.seek(size - limit)
                data = fh.read(limit)
        except OSError:
            return None
        txt = data.decode("utf-8", "replace")
        return {"name": name, "size": size, "truncated": size > limit, "text": txt}

    def mark_handled(self, code, handled=True):
        f = self.find_report(code)
        if not f:
            return {"ok": False, "error": "no such report"}
        d, m = f
        m["handled"] = bool(handled)
        p = os.path.join(d, "meta.json")
        tmp = p + ".tmp"
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump(m, fh, indent=1, ensure_ascii=False)
        os.replace(tmp, p)
        return {"ok": True, "handled": bool(handled)}

    # ------------------------------------------------------------------ logs
    def cur_log(self):
        """The daemon.log being read: the followed file when --daemon-log is auto[:dir], else the fixed path."""
        return (self.log.path if self.log and self.log.follow is not None else self.daemon_log) or ""

    def logs(self, q="", level="all", which="0", n=300, quiet=True):
        self.log and self.log.refresh()
        path = self.cur_log()
        if not path:
            return {"ok": False, "error": "no --daemon-log configured"}
        if which and which != "0":
            if not which.isdigit() or int(which) > 9:
                return {"ok": False, "error": "bad log file"}
            path = "%s.%s" % (path, which)
        try:
            size = os.path.getsize(path)
            with open(path, "rb") as f:
                f.seek(max(0, size - 3 * 1024 * 1024))
                data = f.read()
        except OSError as e:
            return {"ok": False, "error": "cannot read %s: %s" % (os.path.basename(path), e)}
        lines = data.decode("utf-8", "replace").splitlines()
        if size > 3 * 1024 * 1024 and lines:
            lines = lines[1:]
        ql = (q or "").lower()
        out = []
        for ln in lines:
            if quiet and re.match(r"^\[[\d:]+\] (TCP \(|UDP \(|RELAY-|UDP-|STATUS |PERF |DB backup ok|MATCHMAKE |SESSION )", ln) and not logindex.ERR_RE.search(ln):
                continue
            cls = "err" if logindex.ERR_RE.search(ln) and not ln.startswith(("TCP (", "UDP (")) else \
                  "warn" if re.search(r"timed out|silent|drop|refused|mismatch|stall|timeout|reaped|WARN|admin-kick|abort", ln, re.I) else \
                  "evt" if " EVT " in ln[:14] else ""
            if level == "err" and cls != "err":
                continue
            if level == "warn" and cls not in ("err", "warn"):
                continue
            if level == "evt" and cls != "evt" and " EVT " not in ln[:14]:
                continue
            ln = redact(ln)
            if ql and ql not in ln.lower():
                continue
            out.append({"t": ln[:420], "c": cls})
        files = ["0"] + [s for s in sorted((p[len(self.cur_log()) + 1:] for p in __import__("glob").glob(self.cur_log() + ".*")), key=lambda x: (not x.isdigit(), x)) if s.isdigit()]
        return {"ok": True, "lines": out[-n:], "matched": len(out), "file": os.path.basename(path), "files": files, "size": size, "mtime": os.path.getmtime(path)}

    # ------------------------------------------------------------------ kick
    # ---- server notices (daemon admin listener: /notices /notice /shutdown, see tools/notices.py)
    def notices(self):
        try:
            return self.admin_call("/notices")
        except (RuntimeError, OSError, ValueError, urllib.error.URLError) as e:
            return {"ok": False, "error": str(getattr(e, "reason", e)), "notices": []}

    def _notice_call(self, path, data):
        try:
            return self.admin_call(path, data, timeout=5)
        except RuntimeError as e:
            return {"ok": False, "error": str(e)}
        except (OSError, ValueError, urllib.error.URLError) as e:
            return {"ok": False, "error": "daemon admin listener not reachable (%s)" % getattr(e, "reason", e)}

    def notice_send(self, text, seconds):
        # seconds ON SCREEN (2..600); the daemon keeps it long enough for every client's ~6 s poll
        return self._notice_call("/notice", {"text": str(text)[:300], "seconds": max(2.0, min(float(seconds or 8), 600.0))})

    def notice_clear(self, nid):
        return self._notice_call("/notice/clear", {"id": int(nid)})

    def shutdown_schedule(self, minutes, reason, restart):
        return self._notice_call("/shutdown", {"minutes": max(0.0, min(float(minutes), 1440.0)), "reason": str(reason)[:100], "restart": bool(restart)})

    def shutdown_cancel(self):
        return self._notice_call("/shutdown/cancel", {})

    def teams(self):
        """Team rules (team_rules.txt on the daemon) + the split the queue would get now."""
        try:
            return self.admin_call("/teams", timeout=3)
        except RuntimeError as e:
            return {"ok": False, "error": str(e)}
        except (OSError, ValueError, urllib.error.URLError) as e:
            return {"ok": False, "error": "daemon admin listener not reachable (%s)" % getattr(e, "reason", e)}

    def teams_move(self, pid):
        if not __import__("re").match(r"^[0-9a-f]{4,64}$", pid or ""):
            return {"ok": False, "error": "bad id"}
        return self._notice_call("/teams/move", {"pid": pid})

    def teams_set(self, rules):
        return self._notice_call("/teams", {"rules": str(rules)[:4000]})

    # ------------------------------------------------------------------ patch notes (news.json of the update server, live edit)
    NEWS_BACKUPS = 20

    def news_path(self):
        return os.path.join(self.update_data, "news.json") if self.update_data else ""

    def news_get(self):
        p = self.news_path()
        if not p:
            return {"ok": False, "error": "the admin site was started without --update-data, so it cannot find news.json"}
        try:
            with open(p, encoding="utf-8") as f:
                v = json.load(f)
        except FileNotFoundError:
            v = []
        except (OSError, ValueError) as e:
            return {"ok": False, "error": "news.json unreadable: %s" % e}
        return {"ok": True, "entries": v if isinstance(v, list) else [], "path": p}

    @staticmethod
    def _news_secs(v, where):
        if not isinstance(v, dict):
            raise ValueError("%s: sections must be an object" % where)
        out = {}
        for k, lines in v.items():
            if not isinstance(k, str) or not isinstance(lines, list) or not all(isinstance(x, str) for x in lines):
                raise ValueError("%s: section %r must be a list of text lines" % (where, k))
            lines = [x.strip() for x in lines if x.strip()]
            if lines:
                out[k] = lines
        return out

    def news_validate(self, entries):
        if not isinstance(entries, list) or len(entries) > 500:
            raise ValueError("entries must be a list (max 500)")
        seen, out = set(), []
        for e in entries:
            if not isinstance(e, dict):
                raise ValueError("every entry must be an object")
            i = e.get("id")
            if isinstance(i, bool) or not isinstance(i, int):
                raise ValueError("every entry needs an integer id")
            if i in seen:
                raise ValueError("duplicate id %d" % i)
            seen.add(i)
            for k in ("date", "build", "kind", "stamp", "headline"):
                if k in e and not isinstance(e[k], str):
                    raise ValueError("entry %d: %s must be text" % (i, k))
            if not (e.get("headline") or "").strip() and not e.get("title"):
                raise ValueError("entry %d: the headline is empty" % i)
            if e.get("date") and not re.match(r"^\d{4}-\d{2}-\d{2}$", e["date"]):
                raise ValueError("entry %d: date must be YYYY-MM-DD" % i)
            e = dict(e)
            if "headline" in e:
                e["headline"] = e["headline"].strip()
            if "sections" in e:
                e["sections"] = self._news_secs(e["sections"], "entry %d" % i)
            if "pl" in e:
                pl = e["pl"]
                if not isinstance(pl, dict):
                    raise ValueError("entry %d: pl must be an object" % i)
                pl = dict(pl)
                if "headline" in pl:
                    if not isinstance(pl["headline"], str):
                        raise ValueError("entry %d: pl headline must be text" % i)
                    pl["headline"] = pl["headline"].strip()
                if "sections" in pl:
                    pl["sections"] = self._news_secs(pl["sections"], "entry %d pl" % i)
                e["pl"] = pl
            out.append(e)
        return out

    def news_save(self, entries):
        p = self.news_path()
        if not p:
            return {"ok": False, "error": "the admin site was started without --update-data"}
        try:
            out = self.news_validate(entries)
        except ValueError as e:
            return {"ok": False, "error": str(e)}
        d = os.path.dirname(p)
        data = json.dumps(out, ensure_ascii=False, indent=1) + "\n"
        with self.lock:
            backup = ""
            if os.path.isfile(p):
                bdir = os.path.join(d, "news-backups")
                os.makedirs(bdir, exist_ok=True)
                backup = os.path.join(bdir, "news-%s.json" % time.strftime("%Y%m%d-%H%M%S"))
                n = 1
                while os.path.exists(backup):
                    n += 1
                    backup = os.path.join(bdir, "news-%s-%d.json" % (time.strftime("%Y%m%d-%H%M%S"), n))
                shutil.copy2(p, backup)
                old = sorted(f for f in os.listdir(bdir) if f.startswith("news-") and f.endswith(".json"))
                for f in old[:-self.NEWS_BACKUPS]:
                    try:
                        os.remove(os.path.join(bdir, f))
                    except OSError:
                        pass
            tmp = p + ".tmp"
            with open(tmp, "w", encoding="utf-8", newline="\n") as f:
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, p)
        return {"ok": True, "count": len(out), "backup": os.path.basename(backup), "entries": out}

    def kick(self, pid, reason=""):
        if not re.match(r"^[0-9a-f]{4,64}$", pid or ""):
            return {"ok": False, "error": "bad id"}
        reason = re.sub(r"\s+", " ", "".join(ch for ch in str(reason or "") if ch >= " " and ch != "\x7f")).strip()[:120].strip()
        body = {"pid": pid}
        if reason:
            body["reason"] = reason                  # absent when empty: a daemon without the reason support sees the old request
        try:
            return self.admin_call("/kick", body, timeout=5)
        except RuntimeError as e:
            return {"ok": False, "error": str(e)}
        except (OSError, ValueError, urllib.error.URLError) as e:
            return {"ok": False, "error": "daemon admin listener not reachable (%s). It exists on the daemon build with the admin op and needs a daemon restart." % getattr(e, "reason", e)}

    # ---- bans (a ban button on every player for the mods). The list is <db dir>/bans.txt, the file the daemon re-reads
    # when it changes (daemon.py _bans_load): one entry per line, a Steam id or a player name (a name also bans every Steam account
    # that ever used it), '#' starts a comment. A ban from here writes the Steam id (and the name when there is no Steam id).
    def _bans_path(self):
        return os.path.join(self.dbdir, "bans.txt")

    def _bans_lines(self):
        try:
            with open(self._bans_path(), encoding="utf-8", errors="ignore") as f:
                return f.read().splitlines()
        except OSError:
            return ["# bans (one per line: steam id or name)"]

    @staticmethod
    def _ban_hit(line, sid, names):
        e = line.split("#", 1)[0].strip()
        return bool(e) and ((sid and e == str(sid)) or (not e.isdigit() and e.lower() in names))

    def ban_state(self, g):
        """None or {"reason", "line"} for a player group (Steam id or any of its names listed)."""
        for line in self._bans_lines():
            if self._ban_hit(line, g["steamid"], g["alias"]):
                c = line.split("#", 1)[1] if "#" in line else ""
                return {"reason": c.split("reason:", 1)[1].strip() if "reason:" in c else "", "line": line.strip()}
        return None

    def banned_index(self):
        """(steam ids, lower-case names) listed in bans.txt, for the player list."""
        ids, names = set(), set()
        for line in self._bans_lines():
            e = line.split("#", 1)[0].strip()
            if e and not e.lower().startswith(("hwid:", "token:")):
                (ids if e.isdigit() else names).add(e if e.isdigit() else e.lower())
        return ids, names

    def _bans_write(self, lines):
        p = self._bans_path()
        if os.path.exists(p):
            shutil.copy2(p, p + ".bak-admin")          # the state before the newest admin-site change
        tmp = p + ".tmp"
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines).rstrip("\n") + "\n")
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, p)

    def _audit(self, line):
        try:
            with open(os.path.join(self.dbdir, "admin-actions.log"), "a", encoding="utf-8") as f:
                f.write("%s %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), line))
        except OSError:
            pass

    def ban(self, pid, reason="", who=""):
        g = self.player_groups().get(pid or "")
        if not g:
            return {"ok": False, "error": "no such player"}
        if self.ban_state(g):
            return {"ok": False, "error": "already banned"}
        clean = lambda s, n: re.sub(r"\s+", " ", "".join(ch for ch in str(s or "") if ch >= " " and ch not in "\x7f#")).strip()[:n]
        reason, who, name = clean(reason, 160), clean(who, 60), clean(g["display"], 60)
        tail = "%s - admin site %s%s" % (name, time.strftime("%Y-%m-%d"), " by " + who if who else "") + (" | reason: " + reason if reason else "")
        if g["steamid"]:
            entry = "%d  # %s" % (g["steamid"], tail)
        elif name and not name.isdigit():
            entry = "%s  # %s" % (name, tail)          # no Steam id known: the name (the daemon also bans every account that used it)
        else:
            return {"ok": False, "error": "this player has no Steam id or usable name to ban"}
        self._bans_write(self._bans_lines() + [entry])
        self._audit("BAN %s (%s) by %s: %s" % (name, g["steamid"] or "no steam id", who or "?", reason or "-"))
        kicked = None
        online = next((p for p in self.players_online()["players"] if p.get("playerId") == pid and p.get("pid")), None)
        if online:      # online now: kick at once (the daemon refuses the reconnect with the ban)
            kicked = self.kick(online["pid"], "Banned" + (": " + reason if reason else "")).get("ok", False)
        return {"ok": True, "name": name, "entry": entry, "kicked": kicked}

    def unban(self, pid, who=""):
        g = self.player_groups().get(pid or "")
        if not g:
            return {"ok": False, "error": "no such player"}
        lines = self._bans_lines()
        keep = [l for l in lines if not self._ban_hit(l, g["steamid"], g["alias"])]
        if len(keep) == len(lines):
            return {"ok": False, "error": "not banned"}
        self._bans_write(keep)
        self._audit("UNBAN %s (%s) by %s: removed %d line(s)" % (g["display"], g["steamid"] or "no steam id", who or "?", len(lines) - len(keep)))
        return {"ok": True, "name": g["display"], "removed": len(lines) - len(keep)}

    def clan_reset(self, pid, confirm, names=False):
        """Wipe one player's clan. `confirm` must equal the player's name (the page makes the admin type it). The daemon does the write
        (this site never writes the database): it backs the old state up, clears every clan key of the person and logs an audit line."""
        g = self.player_groups().get(pid or "")
        if not g:
            return {"ok": False, "error": "no such player"}
        if (confirm or "").strip().lower() != g["display"].strip().lower():
            return {"ok": False, "error": "the typed name does not match"}
        try:
            r = self.admin_call("/clan/reset", {"name": g["display"], "steamid": g["steamid"], "tokenIds": g["token_ids"], "names": bool(names)}, timeout=15)
        except RuntimeError as e:
            return {"ok": False, "error": str(e)}
        except (OSError, ValueError, urllib.error.URLError) as e:
            return {"ok": False, "error": "daemon admin listener not reachable (%s). The clan reset needs the daemon build that has it (restart)." % getattr(e, "reason", e)}
        with self.lock:
            self.cache.pop("players", None)
            self.cache.pop("clans", None)
        return r

    def clan_backups(self, pid):
        g = self.player_groups().get(pid or "")
        if not g:
            return {"ok": False, "error": "no such player"}
        try:
            return self.admin_call("/clan/backups?steamid=%d" % int(g["steamid"] or 0), None, timeout=5)
        except (RuntimeError, OSError, ValueError, urllib.error.URLError) as e:
            return {"ok": False, "error": str(getattr(e, "reason", e)), "backups": []}

    def clan_restore(self, pid, fname):
        g = self.player_groups().get(pid or "")
        if not g:
            return {"ok": False, "error": "no such player"}
        try:
            r = self.admin_call("/clan/restore", {"file": str(fname or "")}, timeout=15)
        except (RuntimeError, OSError, ValueError, urllib.error.URLError) as e:
            return {"ok": False, "error": str(getattr(e, "reason", e))}
        with self.lock:
            self.cache.pop("players", None)
            self.cache.pop("clans", None)
        return r
