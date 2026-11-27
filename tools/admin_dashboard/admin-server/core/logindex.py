"""Incremental reader of the daemon log: EVT lines, STATUS lines, errors, restarts; match reconstruction from the EVT stream.

The daemon log stamps lines "[HH:MM:SS]" only; the date comes from the "# daemon <ctime>" / "# daemon log continued <ctime>" header lines and a
midnight roll (time going backwards). Rotated logs (daemon.log.1 ...) are read once, the current one incrementally (only the new bytes).
Read-only: files are only opened for reading.
"""
import ast
import glob
import os
import re
import threading
import time

LINE = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\] (.*)$")
HEADER = re.compile(r"^# daemon (log continued )?(\w{3} \w{3} +\d+ \d\d:\d\d:\d\d \d{4})")
ERR_RE = re.compile(r"ERR\b|ERROR|Traceback|FATAL|Exception|HANDLER ERR|-ERR\b|not saved|not started|CORRUPT|crash", re.I)
ERR_KEYS = ("err", "traceback", "fatal", "exception", "not saved", "not started", "corrupt", "crash")   # every ERR_RE alternative contains one of these
MAX_EVENTS = 40000
MAX_ERRORS = 300


def parse_list(s):
    """'[...]' repr of a python list of names -> list (never raises)."""
    try:
        v = ast.literal_eval(s)
        return [str(x) for x in v] if isinstance(v, (list, tuple)) else []
    except (ValueError, SyntaxError):
        return []


def _bracket(body, key):
    """The first [...] after `key ` in body (names may hold ']' only in absurd cases: then the list is empty)."""
    i = body.find(key + " [")
    if i < 0:
        return []
    j = body.find("]", i)
    return parse_list(body[i + len(key) + 1:j + 1]) if j > 0 else []


def _candidates(dist):
    """Every daemon.log under dist: PC layout dist/<pt>/host and dist/<pt>/<folder>/host, Pi layout <live>/host and <live>/hostNN (one folder per build)."""
    return (glob.glob(os.path.join(dist, "*", "host", "daemon.log")) + glob.glob(os.path.join(dist, "*", "*", "host", "daemon.log"))
            + glob.glob(os.path.join(dist, "host*", "daemon.log")))


def newest_daemon_log(dist):
    """The daemon.log written last under dist/<pt>/host or dist/<pt>/<folder>/host: the live server moves to a new PT folder with every
    build, so a fixed --daemon-log went stale (the admin site kept reading an old folder)."""
    best, bt = "", -1.0
    for p in _candidates(dist):
        try:
            t = os.stat(p).st_mtime
        except OSError:
            continue
        if t > bt:
            best, bt = p, t
    return best


def prior_daemon_logs(dist, current, keep=4):
    """The daemon.log files of the EARLIER PT folders under dist (oldest first, at most `keep` folders): every PT build has its own host folder with
    its own log, so reading only the newest one made every player's match history / the matches page empty after each build switch (a new build's start made the previous build's matches vanish from the site)."""
    cur = os.path.abspath(current) if current else ""
    found = []
    for p in _candidates(dist):
        if os.path.abspath(p) == cur:
            continue
        try:
            found.append((os.stat(p).st_mtime, p))
        except OSError:
            continue
    found.sort()
    return [p for _t, p in found[-keep:]]


class LogIndex:
    FOLLOW_EVERY = 30.0

    def __init__(self, path):
        # path "auto" or "auto:<dist dir>": follow the newest daemon.log under dist (re-checked every FOLLOW_EVERY s, switch = full re-read)
        self.follow = None
        if path == "auto" or path.startswith("auto:"):
            # core/ -> admin-server/ -> admin_dashboard/ -> tools/ -> repo root (where dist/*/host/daemon.log lives)
            repo = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", ".."))
            self.follow = path[5:] or os.path.join(repo, "dist")
            path = newest_daemon_log(self.follow)
        self.followed = time.time()
        self.path = path
        self.lock = threading.Lock()
        self.checked = 0.0
        self._reset()

    def _reset(self):
        self.pos = 0
        self.sig = None
        self.events = []          # {ts, kind, body}; kind "_start" = daemon (re)start
        self.errors = []          # {ts, line}
        self.announces = []       # (ts, name)
        self.status = None        # {ts, online, queue, matches, tcp}
        self.started = None       # ts of the last daemon start
        self.version = ""
        self.listen = ""
        self.cur_date = None      # (y, m, d) of the line being read
        self.last_secs = None
        self.loaded = False

    # ------------------------------------------------------------------ parsing
    def _set_header(self, text):
        m = HEADER.match(text)
        if not m:
            return None
        try:
            t = time.strptime(m.group(2), "%a %b %d %H:%M:%S %Y")
        except ValueError:
            return None
        self.cur_date = (t.tm_year, t.tm_mon, t.tm_mday)
        self.last_secs = t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec
        return time.mktime(t), bool(m.group(1))

    def _ts(self, hh, mm, ss):
        secs = hh * 3600 + mm * 60 + ss
        y, mo, d = self.cur_date
        if self.last_secs is not None and secs < self.last_secs - 600:     # midnight rolled
            t = time.mktime((y, mo, d, 12, 0, 0, 0, 0, -1)) + 86400
            lt = time.localtime(t)
            self.cur_date = (lt.tm_year, lt.tm_mon, lt.tm_mday)
            y, mo, d = self.cur_date
        self.last_secs = secs
        return time.mktime((y, mo, d, hh, mm, ss, 0, 0, -1))

    def _feed(self, lines, mtime):
        for raw in lines:
            raw = raw.rstrip("\r\n")
            if raw.startswith("# daemon"):
                h = self._set_header(raw)
                if h and not h[1]:
                    self.started = h[0]
                    self.events.append({"ts": h[0], "kind": "_start", "body": ""})
                continue
            m = LINE.match(raw)
            if not m:
                continue
            if self.cur_date is None:                  # no header seen (rotated tail): anchor on the file time
                lt = time.localtime(mtime)
                self.cur_date = (lt.tm_year, lt.tm_mon, lt.tm_mday)
                first = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))
                if first > lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec + 120:
                    t = time.mktime((lt.tm_year, lt.tm_mon, lt.tm_mday, 12, 0, 0, 0, 0, -1)) - 86400
                    l2 = time.localtime(t)
                    self.cur_date = (l2.tm_year, l2.tm_mon, l2.tm_mday)
            ts = self._ts(int(m.group(1)), int(m.group(2)), int(m.group(3)))
            body = m.group(4)
            if body.startswith("EVT "):
                parts = body[4:].split(" ", 1)
                self.events.append({"ts": ts, "kind": parts[0], "body": parts[1] if len(parts) > 1 else ""})
            elif body.startswith("STATUS "):
                kv = dict(re.findall(r"(\w+)=(\d+)", body))
                if "online" in kv:
                    self.status = {"ts": ts, "online": int(kv["online"]), "queue": int(kv.get("queue", 0)), "matches": int(kv.get("matches", 0)),
                                   "tcp": int(kv.get("tcp", 0))}
            elif body.startswith("ANNOUNCE "):
                mm = re.match(r"ANNOUNCE \(.*?\) (.*)$", body)
                if mm:
                    self.announces.append((ts, mm.group(1)))
                    if len(self.announces) > 600:
                        del self.announces[:200]
            elif body.startswith("daemon version "):
                self.version = body.split()[2]
            elif body.startswith("TCP LISTEN "):
                self.listen = body.split()[2]
            # (ERR_RE.search on every line was 70 % of a full log read; the lower-case keyword test is a necessary condition for a match,
            # so the regex now runs only on the few lines that can match)
            if (not body.startswith(("TCP (", "UDP (")) and "recv" not in body[:40] and any(k in body.lower() for k in ERR_KEYS)
                    and ERR_RE.search(body)):
                self.errors.append({"ts": ts, "line": body[:300]})
                if len(self.errors) > MAX_ERRORS:
                    del self.errors[:100]
        if len(self.events) > MAX_EVENTS:
            del self.events[:MAX_EVENTS // 4]

    def _read_new(self, path, from_pos):
        try:
            size = os.path.getsize(path)
            mtime = os.path.getmtime(path)
            with open(path, "rb") as f:
                f.seek(from_pos)
                data = f.read(size - from_pos if size >= from_pos else 0)
        except OSError:
            return from_pos
        if not data:
            return from_pos
        cut = data.rfind(b"\n")
        if cut < 0:
            return from_pos
        text = data[:cut + 1].decode("utf-8", "replace")
        self._feed(text.splitlines(), mtime)
        return from_pos + cut + 1

    @staticmethod
    def _rotated(path):
        rot = []
        for p in glob.glob(path + ".*"):
            suf = p[len(path) + 1:]
            if suf.isdigit():
                rot.append((int(suf), p))
        return [p for _n, p in sorted(rot, reverse=True)]

    # ------------------------------------------------------------------ public
    def _rotated_from(self, ino):
        """The rotated file (daemon.log.N) that is the file we were reading (same inode): the daemon renames daemon.log -> daemon.log.1 and starts a
        new daemon.log, so the old file lives on under a new name."""
        for p in self._rotated(self.path)[::-1]:                  # .1 first
            try:
                if os.stat(p).st_ino == ino:
                    return p
            except OSError:
                pass
        return ""

    def refresh(self, min_interval=1.5):
        if not self.path and not self.follow:
            return
        now = time.time()
        # Once loaded, a refresh that another request thread is already doing is skipped (the caller serves the current data) instead of
        # queueing every request thread on the lock
        if self.loaded:
            if not self.lock.acquire(blocking=False):
                return
        else:
            self.lock.acquire()
        try:
            if now - self.checked < min_interval:
                return
            self.checked = now
            if self.follow and (now - self.followed >= self.FOLLOW_EVERY or not self.path):
                self.followed = now
                p = newest_daemon_log(self.follow)
                if p and p != self.path:
                    self.path = p
                    self._reset()
            if not self.path:
                return
            try:
                st = os.stat(self.path)
            except OSError:
                return
            sig = (st.st_ino, )
            if self.loaded and self.sig is not None and sig != self.sig:
                # Rotated (every 8 MB, ~15 min at peak). Before, this was a full re-read of every rotated log + the prior build's logs
                # (~70 MB, 9 s on a desktop, minutes on the 1-vCPU host) under the lock: the whole admin site hung (Cloudflare 524). Now the rest
                # of the old file is read under its new name and the new file is followed from its start; nothing already read is parsed again.
                old = self._rotated_from(self.sig[0])
                if old:
                    self._read_new(old, self.pos)
                    self.sig = sig
                    self.pos = 0
                else:
                    self._reset()                                   # replaced by something else: start over
            if self.sig is not None and (st.st_size < self.pos or sig != self.sig):    # truncated in place, or replaced
                self._reset()
            if not self.loaded:
                self.sig = sig
                chain = []
                if self.follow:
                    for pp in prior_daemon_logs(self.follow, self.path):
                        chain += self._rotated(pp) + [pp]
                chain += self._rotated(self.path)
                for p in chain:                                   # oldest first
                    self.cur_date = None
                    self.last_secs = None
                    self._read_new(p, 0)
                self.cur_date = None
                self.last_secs = None
                self.loaded = True
            self.sig = sig
            self.pos = self._read_new(self.path, self.pos)
        finally:
            self.lock.release()

    def earliest(self):
        with self.lock:
            return self.events[0]["ts"] if self.events else None

    def snapshot(self):
        with self.lock:
            return {"events": list(self.events), "errors": list(self.errors), "announces": list(self.announces), "status": self.status,
                    "started": self.started, "version": self.version, "listen": self.listen}


# ---------------------------------------------------------------------- match reconstruction
def derive_matches(events):
    """EVT stream -> {mid: match}. A match: id, formed, started, ended, closed, state (loading|running|ended|closed), map, players [names in slot order],
    host, net, ping_ms (max pair), winner (0/1 team, 2 draw, None), secs, reporter, left [{name, why, ts}], closed_why."""
    ms = {}
    order = []

    def get(mid, ts):
        if mid not in ms:
            ms[mid] = {"id": mid, "formed": ts, "started": None, "ended": None, "closed": None, "state": "loading", "map": None, "players": [], "host": None,
                       "net": None, "ping_ms": None, "winner": None, "secs": None, "reporter": None, "left": [], "closed_why": "", "jip": []}
            order.append(mid)
        return ms[mid]

    def close(m, ts, why):
        if m["closed"] is None:
            m["closed"] = ts
            m["closed_why"] = why
            if m["state"] != "ended":
                m["state"] = "closed"
    for e in events:
        k, b, ts = e["kind"], e["body"], e["ts"]
        if k == "_start":
            for m in ms.values():
                if m["closed"] is None:
                    close(m, ts, "server restart")
            continue
        mid = b.split(" ", 1)[0] if b else ""
        if k == "match-formed" and mid:
            m = get(mid, ts)
            m["formed"] = ts
            m["players"] = _bracket(b, "players")
            mh = re.search(r" host (.*?) net (\S+) ping-ms ", b)
            if mh:
                m["host"], m["net"] = mh.group(1), mh.group(2)
            pm = re.search(r"ping-ms (\{[^}]*\})", b)
            if pm:
                try:
                    d = ast.literal_eval(pm.group(1))
                    m["ping_ms"] = max(d.values()) if d else None
                except (ValueError, SyntaxError):
                    pass
        elif k == "game-start" and mid in ms:
            m = ms[mid]
            m["started"] = ts
            m["state"] = "running" if m["state"] == "loading" else m["state"]
            mm = re.search(r" map (\S+)", b)
            if mm:
                m["map"] = mm.group(1)
        elif k == "jip-join" and mid in ms:
            pl = _bracket(b, "players")
            if pl:
                ms[mid]["players"] = pl
            nm = re.match(r"\S+ (.*?) slot ", b)
            if nm:
                ms[mid]["jip"].append(nm.group(1))
        elif k == "match-end" and mid in ms:
            m = ms[mid]
            mw = re.search(r"winner team (\d+)", b)
            m["winner"] = int(mw.group(1)) if mw else None
            ma = re.search(r"after (\d+) s", b)
            m["secs"] = int(ma.group(1)) if ma else None
            mr = re.search(r"reported by (.*?) after ", b)
            m["reporter"] = mr.group(1) if mr else None
            pl = _bracket(b, "players")
            if pl:
                m["players"] = pl
            m["ended"] = ts
            m["state"] = "ended"
        elif k == "left" and mid in ms:
            mm = re.match(r"\S+ (.*?) (left \(0x02\)|timed out|admin kick|jip never loaded|[^|]*?)(?: \||$)", b)
            if mm:
                ms[mid]["left"].append({"name": mm.group(1), "why": mm.group(2).strip(), "ts": ts})
            if "none (match removed)" in b:
                close(ms[mid], ts, "everyone left")
        elif k in ("freed", "reaped") and mid in ms:
            close(ms[mid], ts, "freed" if k == "freed" else "reaped: " + b.split(" ", 1)[1][:80] if " " in b else k)
        elif k == "load-timeout" and mid in ms:
            ms[mid]["load_timeout"] = True
    return [ms[i] for i in order]


def team_names(m):
    """Names per team (slot parity, the daemon's team_of)."""
    t = ([], [])
    for i, n in enumerate(m["players"]):
        t[i % 2].append(n)
    return t


def result_for(m, name):
    """'W' / 'L' / 'D' / '' for a player's name in a finished match."""
    if m.get("winner") is None or m["state"] != "ended" or name not in m["players"]:
        return ""
    if m["winner"] == 2:
        return "D"
    if m["winner"] not in (0, 1):
        return ""
    return "W" if m["players"].index(name) % 2 == m["winner"] else "L"
