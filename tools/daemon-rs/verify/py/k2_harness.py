"""Differential test of daemon.py's K2 chunk (clan glue: clan_key / clan_report / clan_finalize / match end by reports / match start and end / boosters /
the clan ops 0xE0-0xE4, 0xE8-0xEF, 0xB4, 0xB6, 0xBF, 0xD0-0xD2, 0xA9-0xAB / leaderboard table / admin clan reset + restore) against `t2r-daemon selftest k2`.

  python3 k2_harness.py gen SEED N > script.jsonl
  T2R_TEST_SEED=SEED T2R_DB=/tmp/k2py/t2r.db python3 k2_harness.py run < script.jsonl > out.py.jsonl
  T2R_TEST_SEED=SEED T2R_DB=/tmp/k2rs/t2r.db t2r-daemon selftest k2 < script.jsonl > out.rs.jsonl ; cmp ...

daemon.py is imported from T2R_PY_DIR with the REAL clan.py / clan_story.py / clan_story_pl.py / clan_reset.py / names.py / rankbadge.py / roles.py. The modules that are
not ported yet or belong to other chunks are replaced by DETERMINISTIC DOUBLES, the same ones as src/d/ext_k2.rs and src/clan/ext_clan_story.rs (T2R_K2_DOUBLES=0
keeps the real clan_perks / clan_status / clan_ingame / achievements modules: only for the integration run, when Rust has them too):
  clan_alife / clan_chronicle / clan_journal / clan_arcs / clan_saga / the render_msg of clan_perks   (the fakes of clan_story_harness.py)
  clan_perks (active / loss_streak / wire_text / on_day), clan_status.wire_text, clan_ingame (build / stats_report / stats_parts / player_rank),
  achievements (settle / book_match / book_live / book_side / book_clan / wire_text / BY_ID): they log their arguments into the table dbl_notes
  anti-cheat: AC = None (private module); loaded_clear_match / silence / PRIV.is_private_mid / readopt_claim / note_lang /
  PRES.touch / leave_running_match (the K3 / K5 / K6 / KP items): scripted doubles; threading.Timer / Thread: a queue the script fires ("timers" op).
Time is frozen per script line ("t"); clan.rng = random.Random(seed + 1); the global random is seeded with the seed.
"""
import contextlib
import hashlib
import io
import json
import os
import random
import sqlite3
import sys

os.environ.setdefault("T2R_HOST_CAP", "0")
os.environ.setdefault("T2R_DB", "/tmp/k2py/t2r.db")
os.environ["T2R_AC"] = "0"
os.environ["T2R_MAP"] = "to1-forest"      # map_index / flow_extra_secs / note_lang / PRES are the REAL K3 code on both sides (a fixed map: no rotation draws)
os.environ.setdefault("T2R_TOOLS_DIR", "/tmp/k2tools")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cseed  # noqa: E402
from cseed import T, emit, log, main, script  # noqa: E402

import time as _time  # noqa: E402
_time.time = lambda: T[0]
_time.monotonic = lambda: T[0]

DB = os.environ["T2R_DB"]
DIR = os.path.dirname(os.path.abspath(DB))
os.makedirs(DIR, exist_ok=True)
DOUBLES = os.environ.get("T2R_K2_DOUBLES", "1") != "0"
SEED = int(os.environ.get("T2R_TEST_SEED", "0") or 0)

TIMERS = []          # (due, seq, fn)
PRIVMIDS = set()
SILENT = {}
LEAVE = [False]


def tok(i):
    return ("%02x" % i) * 32


def sidof(i):
    return 76561198000000000 + i


def unhex(s):
    return bytes.fromhex(s)


# ------------------------------------------------------------------ the doubles (mirrors of src/d/ext_k2.rs)
def _cs(d):
    return "{" + ", ".join("%d: %d" % (k, v) for k, v in sorted(d.items())) + "}"


def _co(d):
    return "{" + ", ".join("%d: %d" % (k, v) for k, v in d.items()) + "}"


def _ob(v):
    return "None" if v is None else ("True" if v else "False")


def _note(c, what):
    c.execute("CREATE TABLE IF NOT EXISTS dbl_notes(seq INTEGER PRIMARY KEY AUTOINCREMENT, note TEXT)")
    c.execute("INSERT INTO dbl_notes(note) VALUES(?)", (what,))


class Boom(Exception):
    """an exception whose repr is its text (the Rust glue logs the error text of a Result<_, String>)"""
    def __repr__(self):
        return self.args[0]


def install_doubles():
    import types
    import clan_story_harness as H
    H.install_fakes()
    pk = sys.modules["clan_perks"]
    if not DOUBLES:
        for n in ("clan_perks", "clan_status", "clan_ingame", "achievements", "clan_alife", "clan_chronicle", "clan_journal", "clan_arcs", "clan_saga"):
            sys.modules.pop(n, None)
        return

    def active(clan_row, story):
        day = int(clan_row.get("day") or 0)
        cap = len(story["people"]) if story is not None else -1
        cats = [("food", "f"), ("water", "w"), ("med", "m")]
        return [{"cat": cats[i][0], "wire": i + 1, "key": cats[i][1], "cap": cap} for i in range(day % 3)]

    def loss_streak(story):
        return len(story["people"]) % 3 if story else 0

    def wire_text(perks, streak):
        if not perks:
            return ""
        return "pk=%s ls=%d" % (",".join("%s:%d:%d" % (p["cat"], p["wire"], p["cap"]) for p in perks), streak)

    def on_day(c, tid, newc, r, counts, matchv):
        n = (counts or {}).get(31, 0)
        _note(c, "perks.on_day %s day=%d counts=%s match=%s" % (tid, int(newc.get("day", 0)), _cs(counts or {}), "True" if matchv is not None else "False"))
        if n > 7:
            raise Exception("perks double: boom")
        return [("food", n), ("med", 1)] if n > 0 else []
    pk.active, pk.loss_streak, pk.wire_text, pk.on_day = active, loss_streak, wire_text, on_day

    st = types.ModuleType("clan_status")
    st.wire_text = lambda story: "" if story is None else ("st=%d" % (len(story["people"]) % 4) if len(story["people"]) % 4 else "")
    sys.modules["clan_status"] = st

    ig = types.ModuleType("clan_ingame")

    def build(c, tid, lang, section, seen_id, seen_day, token=None):
        if section == 7:
            raise Boom("ingame double: boom")
        n = seen_id % 4000
        s = "HEAD %s %s %d %d %d %s\n" % (tid, lang, section, seen_id, seen_day, token if token is not None else "-")
        s += "".join("R%d|%s|%s\n" % (i, tid, lang) for i in range(n))
        if section == 8:
            s += "x" * 30000
        return s
    ig.build = build
    ig.stats_report = lambda c, mid, token, name, team, counts, left: _note(c, "ingame.stats_report %s %s %s team=%d counts=%s left=%s" % (mid, token, name, team, _cs(counts), "True" if left else "False"))
    ig.stats_parts = lambda c, mid, token, parts: _note(c, "ingame.stats_parts %s %s %d" % (mid, token, parts))
    ig.player_rank = lambda c, token: (int(token[0:2], 16) % 10, int(token[2:4], 16) % 1000)
    sys.modules["clan_ingame"] = ig

    ach = types.ModuleType("achievements")

    class ById:
        def __getitem__(self, k):
            return {"title": "T-" + k}
    ach.BY_ID = ById()

    def ids(counts):
        return ["m%d" % k for k, v in counts.items() if v >= 5][:3]

    def settle(c, key, auth, done, labels):
        _note(c, "ach.settle %s auth=%s done=%s labels=%d" % (key, _cs(auth), _cs(done), len(labels)))
        for lab in labels:
            if lab in auth:
                done[lab] = max(done.get(lab, 0), auth[lab])

    def book_match(c, key, counts, done, win=None, left=False, late=False, first=False, now=None):
        _note(c, "ach.book_match %s counts=%s done=%s win=%s left=%s late=%s first=%s" % (key, _co(counts), _cs(done), _ob(win), _ob(left), _ob(late), _ob(first)))
        for k, v in counts.items():
            done[k] = max(done.get(k, 0), v)
        return ids(counts)

    def book_live(c, key, counts, done, now=None):
        _note(c, "ach.book_live %s counts=%s done=%s" % (key, _co(counts), _cs(done)))
        for k, v in counts.items():
            done[k] = max(done.get(k, 0), v)
        return ["l" + s for s in ids(counts)]

    def book_side(c, key, now=None):
        _note(c, "ach.book_side %s" % key)
        return ["side"]

    def book_clan(c, key, before, after, r, rank_weeks=None, now=None):
        def day(v):
            return int(v.get("day", -1)) if v is not None else -2
        _note(c, "ach.book_clan %s before=%d after=%d r=%s" % (key, day(before), day(after), "True" if r is not None else "False"))
        return ["c%d" % day(after), "m1"]

    def wire_text_a(c, key, rank_weeks=None):
        _note(c, "ach.wire_text %s weeks=%s" % (key, "None" if rank_weeks is None else str(rank_weeks)))
        return "ACH %s %d\n" % (key, rank_weeks if rank_weeks is not None else -1)
    ach.settle, ach.book_match, ach.book_live, ach.book_side, ach.book_clan, ach.wire_text = settle, book_match, book_live, book_side, book_clan, wire_text_a
    sys.modules["achievements"] = ach


class TimerShim:
    def __init__(self, delay, fn, args=(), kwargs=None):
        self.delay, self.fn, self.args = delay, fn, args
        self.daemon = False

    def start(self):
        SEQ[0] += 1
        TIMERS.append((T[0] + self.delay, SEQ[0], self.fn))


class ThreadShim:
    def __init__(self, *a, **k):
        pass

    def start(self):
        pass


SEQ = [0]


def setup():
    import types
    install_doubles()
    import daemon as d
    real = d.time

    class Shim:
        def time(self):
            return T[0]

        monotonic = time

        def sleep(self, x):
            pass

        def __getattr__(self, k):
            return getattr(real, k)

    d.time = Shim()
    realthr = d.threading

    class ThrShim:
        Timer = TimerShim
        Thread = ThreadShim

        def __getattr__(self, k):
            return getattr(realthr, k)
    d.threading = ThrShim()

    d.log = log
    d.evt = lambda kind, *a: log("EVT", kind, *a)
    d.AC = None
    d.clan.rng = random.Random(SEED + 1)
    random.seed(SEED)
    d.loaded_clear_match = lambda mid, players: None
    d.silence = lambda t, now=None: SILENT.get(t, 0.0)
    d.readopt_claim = lambda token, now=None: None
    realpriv = d.PRIV

    class PrivShim:
        def is_private_mid(self, mid):
            return mid in PRIVMIDS

        def __getattr__(self, k):
            return getattr(realpriv, k)
    d.PRIV = PrivShim()

    def leave(token, why):
        log("DBL leave_running_match", d.tag(token), why)
        return True
    d._real_leave = d.leave_running_match
    d._dbl_leave = leave
    return d


def apply_leave(d):
    d.leave_running_match = d._dbl_leave if LEAVE[0] else d._real_leave


# ------------------------------------------------------------------ world / state
STATICS_CLEAR = ["CLAN_MATCH", "CLAN_TEAM", "CLAN_MAP", "CLAN_ENDED", "CLAN_PENDING", "CLAN_CREDITED", "CLAN_QUITTERS", "CLAN_EXCUSE_WHY", "CLAN_KICKED", "CLAN_ABORTED",
                 "CLAN_LEFT_AT", "CLAN_REJOINED", "CLAN_LAST_DEPART", "CLAN_EXCUSED", "CLAN_LINK_EXCUSES", "CLAN_CLOSING", "CLAN_CLOSE_DROPS", "CLAN_LOST_EXCUSES",
                 "CLAN_WIN", "CLAN_MOVED", "CLAN_REBOOK", "CLAN_DONE", "CLAN_DUE", "CLAN_LAST_MATCH", "PRIV_LAST_MID", "CLAN_REPORTED_END", "END_BY_REPORTS", "RESULT_BOOKED",
                 "BOOST_PICK", "BOOST_ACTIVE", "ACH_DONE", "ACH_PEND", "ACH_DAY", "ACH_LIVE_AT", "FEED_SEEN", "_FEED_CACHE", "_RANK_TOK", "CLAN_PERK_LOGGED", "RESTORED_T",
                 "_end_timers"]


def wipe_db(d, w):
    with d._dblock, d.dbc() as c:
        for (n,) in c.execute("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'").fetchall():
            c.execute('DELETE FROM "%s"' % n)
        try:
            c.execute("DELETE FROM sqlite_sequence")
        except sqlite3.Error:
            pass
        d.names.init(c)
        d.elo_ensure(c)
        for r in w.get("players", []):
            c.execute("INSERT INTO players(token,name,steamid,kills,deaths,wins,matches,draws,last_seen) VALUES (?,?,?,?,?,?,?,?,?)", r)
        for r in w.get("names", []):
            c.execute("INSERT OR REPLACE INTO names(token_id,name,tag,updated) VALUES (?,?,?,?)", r)


def mm_set(d, m):
    for mid, pl in m.get("matches", {}).items():
        d.MM.matches[mid] = list(pl)
    d.MM.started.update(m.get("started", []))
    d.MM.ended.update(m.get("ended", []))
    for mid, ts in m.get("departed", {}).items():
        d.MM.departed[mid] = set(ts)
    for t, (mid, tm) in m.get("joining", {}).items():
        d.MM.joining[t] = (mid, tm)
    for t, (mid, x) in m.get("late_mult", {}).items():
        d.MM.late_mult[t] = (mid, x)
    for t, (mid, n) in m.get("late_bonus", {}).items():
        d.MM.late_bonus[t] = (mid, n)


def world(d, w):
    del TIMERS[:]
    PRIVMIDS.clear()
    PRIVMIDS.update(w.get("priv", []))
    SILENT.clear()
    SILENT.update(w.get("silent", {}))
    LEAVE[0] = bool(w.get("test_leave"))
    apply_leave(d)
    for fn in ("devs.txt", "web-admins.txt", "og-steamids.txt"):
        p = os.path.join(DIR, fn)
        if fn in w.get("roles", {}):
            with open(p, "w") as f:
                f.write(w["roles"][fn])
        elif os.path.exists(p):
            os.remove(p)
    wipe_db(d, w)
    for n in STATICS_CLEAR:
        getattr(d, n).clear()
    d._LB_CACHE.update(t=0.0, top=None, v=b"")
    d._name_cache.clear()
    d._steam_cache.clear()
    d._tok_gen.clear()
    d._name_gen[0] = 0
    d._name_ver[0] = 0
    d._elo_pool.update(t=0.0, p=0.4)
    d.MM.__init__()
    d.TEAM_SWAP.clear()
    for mid, m in w.get("swap", {}).items():
        d.TEAM_SWAP[mid] = dict(m)
    for mid, t in w.get("restored", {}).items():
        d.RESTORED_T[mid] = t
    mm_set(d, w.get("mm", {}))
    shutil_rm(os.path.join(DIR, "clan-resets"))


def shutil_rm(p):
    import shutil
    shutil.rmtree(p, ignore_errors=True)


def S(d):
    def ts(s):
        return sorted([list(x) for x in s])

    def kv(m):
        return sorted([list(k) if isinstance(k, tuple) else k, v] for k, v in m.items())

    def okv(m):
        return [[list(k) if isinstance(k, tuple) else k, v] for k, v in m.items()]
    mm = d.MM
    return {
        "cm": kv(d.CLAN_MATCH), "map": okv(d.CLAN_MAP), "team": [[m, sorted(v.items())] for m, v in d.CLAN_TEAM.items()], "ended": okv(d.CLAN_ENDED),
        "pend": sorted([t, p[0], p[1], p[2], sorted(p[3].items())] for t, p in d.CLAN_PENDING.items()),
        "cred": ts(d.CLAN_CREDITED), "quit": ts(d.CLAN_QUITTERS), "exc": ts(d.CLAN_EXCUSED), "excwhy": kv(d.CLAN_EXCUSE_WHY), "done": kv(d.CLAN_DONE),
        "leftat": kv(d.CLAN_LEFT_AT), "rej": kv(d.CLAN_REJOINED), "moved": kv(d.CLAN_MOVED), "rebook": ts(d.CLAN_REBOOK), "kicked": okv(d.CLAN_KICKED),
        "aborted": okv(d.CLAN_ABORTED), "due": okv(d.CLAN_DUE), "linkex": kv(d.CLAN_LINK_EXCUSES), "lostex": kv(d.CLAN_LOST_EXCUSES), "win": kv(d.CLAN_WIN),
        "bp": kv(d.BOOST_PICK), "ba": kv(d.BOOST_ACTIVE), "achdone": sorted([list(k), sorted(v.items())] for k, v in d.ACH_DONE.items()),
        "achpend": [[list(k), list(v.items())] for k, v in d.ACH_PEND.items()],
        "achday": [[list(k), list(v["e2"].items()), v["win"], v["left"], v["late"]] for k, v in d.ACH_DAY.items()], "achlive": okv(d.ACH_LIVE_AT),
        "rend": sorted([m, list(v.items())] for m, v in d.CLAN_REPORTED_END.items()), "ebr": sorted(d.END_BY_REPORTS), "rb": sorted(d.RESULT_BOOKED),
        "lastm": kv(d.CLAN_LAST_MATCH), "privlast": kv(d.PRIV_LAST_MID), "feedseen": sorted(d.FEED_SEEN), "perklog": sorted(d.CLAN_PERK_LOGGED),
        "etimers": sorted(d._end_timers), "ntimers": len(TIMERS),
        "mm": {"m": sorted([m, list(p)] for m, p in mm.matches.items()), "st": sorted(mm.started), "en": sorted(mm.ended),
               "dep": sorted([m, sorted(s)] for m, s in mm.departed.items()), "join": sorted([t, list(v)] for t, v in mm.joining.items()),
               "lm": sorted([t, list(v)] for t, v in mm.late_mult.items()), "lb": sorted([t, list(v)] for t, v in mm.late_bonus.items())},
    }


def dump_db():
    c = sqlite3.connect(DB)
    out = {}
    for n in ("players", "clans", "boosters", "clan_snap", "clan_story", "clan_feed", "pstats", "pweapon", "names", "dbl_notes", "fake_notes", "clan_journal", "clan_alife", "ach", "ach_count"):
        try:
            rows = c.execute('SELECT * FROM "%s" ORDER BY rowid' % n).fetchall()
        except sqlite3.Error:
            rows = []
        out[n] = [[x.hex() if isinstance(x, bytes) else x for x in r] for r in rows]
    c.close()
    bd = os.path.join(DIR, "clan-resets")
    files = {}
    if os.path.isdir(bd):
        for fn in sorted(os.listdir(bd)):
            with open(os.path.join(bd, fn), "rb") as f:
                files[fn] = hashlib.sha1(f.read()).hexdigest()
    out["resets"] = files
    return out


def fire_timers(d):
    n = 0
    while True:
        due = [e for e in TIMERS if e[0] <= T[0]]
        if not due:
            break
        e = min(due, key=lambda x: (x[0], x[1]))
        TIMERS.remove(e)
        e[2]()
        n += 1
    return n


def hexr(b):
    return None if b is None else b.hex()


def setkey(d, c):
    n, k = c["name"], c.get("key")
    k = tuple(k) if isinstance(k, list) else k
    v = c.get("val")
    if n in ("CLAN_QUITTERS", "CLAN_EXCUSED", "CLAN_REBOOK", "CLAN_CREDITED", "CLAN_CLOSE_DROPS"):
        getattr(d, n).add(k)
    elif n == "CLAN_WIN":
        d.CLAN_WIN[k] = v
    elif n in ("CLAN_KICKED", "CLAN_ABORTED", "CLAN_EXCUSE_WHY", "CLAN_LEFT_AT", "CLAN_REJOINED", "CLAN_MOVED", "CLAN_DONE", "BOOST_PICK", "CLAN_ENDED", "CLAN_MATCH"):
        getattr(d, n)[k] = v
    elif n == "CLAN_LINK_EXCUSES":
        d.CLAN_LINK_EXCUSES[k] = list(v)
    elif n == "RESULT_BOOKED":
        d.RESULT_BOOKED.add(k)
    elif n == "CLAN_MAP":
        d.CLAN_MAP[k] = v
    elif n == "CLAN_PENDING":
        d.CLAN_PENDING[k] = (v[0], v[1], v[2], {int(a): b for a, b in v[3]})


def one(d, c):
    o = c["op"]
    if o == "world":
        world(d, c["w"])
        return None
    if o == "unset":
        k = tuple(c["key"]) if isinstance(c["key"], list) else c["key"]
        n = c["name"]
        if n in ("CLAN_CREDITED", "CLAN_EXCUSED", "CLAN_QUITTERS", "CLAN_REBOOK"):
            getattr(d, n).discard(k)
        else:
            getattr(d, n).pop(k, None)
        return None
    if o == "setsid":
        with d._dblock, d.dbc() as cc:
            cc.execute("UPDATE players SET steamid=? WHERE token=?", (c["sid"], c["token"]))
        return None
    if o == "restored":
        d.RESTORED_T[c["mid"]] = c["t"]
        return None
    if o == "priv":
        PRIVMIDS.update(c["mids"])
        return None
    if o == "mmset":
        mm_set(d, c["m"])
        return None
    if o == "set":
        setkey(d, c)
        return None
    if o == "silent":
        SILENT.clear()
        SILENT.update(c["m"])
        return None
    if o == "started":
        d.clan_match_started(c["players"], c["mid"])
        return None
    if o == "ended":
        d.clan_match_ended(c["players"], c["mid"])
        return None
    if o == "report":
        st = d.clan_report(c["token"], c["parts"], c["flags"], {int(k): v for k, v in c["counts"]})
        return d.clan.state_text(st)
    if o == "finalize":
        d.clan_finalize(c["token"])
        return "ok"
    if o == "tick":
        now = T[0]
        for t in [t for t, due in list(d.CLAN_DUE.items()) if due <= now]:
            d.CLAN_DUE.pop(t, None)
            try:
                d.clan_finalize(t)
            except Exception as e:  # noqa: BLE001
                log("CLAN scheduler", repr(e))
        return "ok"
    if o == "timers":
        return fire_timers(d)
    if o == "mer":
        return d.match_end_via_reports(c["mid"])
    if o == "booster":
        with d._dblock, d.dbc() as cc:
            return d.clan.booster_grant(cc, d.clan_key(cc, c["token"]), c["bid"], c["n"])
    if o == "frame":
        pl = unhex(c["pl"])
        code = c["code"]
        f = {0xE0: d.clan_op, 0xE2: d.clan_op, 0xE4: d.clan_op, 0xE8: d.meta_op, 0xEE: d.meta_op, 0xEF: d.meta_op, 0xD0: d.names_op, 0xD2: d.names_op}.get(code)
        if f is not None:
            return hexr(f(code, pl))
        g = {0xB4: d.feed_op, 0xB6: d.clan13_op, 0xBF: d.rank_table_op, 0xA9: d.ach_report_op, 0xAB: d.ach_live_op, 0xAA: d.ach_read_op}[code]
        return hexr(g(pl))
    if o == "lb":
        return hexr(d.leaderboard_table(c["top"]))
    if o == "admin_reset":
        return d.admin_clan_reset(c["body"])
    if o == "admin_restore":
        body = dict(c["body"])
        if body.get("file") == "@last":
            bd = os.path.join(DIR, "clan-resets")
            fs = sorted(f for f in os.listdir(bd) if f.endswith(".json")) if os.path.isdir(bd) else []
            body["file"] = fs[-1] if fs else ""
        return d.admin_clan_restore(body)
    if o == "dump":
        return dump_db()
    raise SystemExit("unknown op " + o)


def run():
    d = setup()
    n = 0
    for c in script():
        T[0] = c.get("t", T[0])
        n += 1
        buf = io.StringIO()
        try:
            with contextlib.redirect_stdout(buf):
                r = one(d, c)
        except SystemExit:
            raise
        except Exception as e:  # noqa: BLE001
            r = "EXC"
            sys.stderr.write("op %d %s: %r" % (n, c.get("op"), e) + chr(10))
        for line in buf.getvalue().splitlines():      # print() of the clan modules (missions: pool rolled ...)
            log("PRINT", line)
        emit(n, r, S(d) if c["op"] != "dump" else None)


# ------------------------------------------------------------------ generator
LABS = [1, 2, 3, 4, 5, 6, 21, 22, 23, 24, 31, 32, 33, 8, 40]
NAMES = ["Alpha", "bravo", "Charlie Brown", "delta", "Echo", "Foxtrot", "golf", "Hotel", "india", "Juliet", "kilo", "Łukasz", "Żaneta"]


def gen(r, n):
    out = []
    t = [1700000000.0 + r.randrange(100000)]
    mids = [0]

    def emit_(**kw):
        kw["t"] = round(t[0], 3)
        out.append(kw)

    def tick():
        t[0] += r.choice([0, 0, 1, 3, 7, 25, 70, 400, 4000])

    def mid():
        mids[0] += 1
        return "%08x" % (r.randrange(1 << 30) * 16 + mids[0] % 16)

    ntok = 0
    toks = []

    def new_world():
        nonlocal toks, ntok
        ntok = r.choice([5, 8, 10])
        toks = [tok(i + 1) for i in range(ntok)]
        sids = {tk: (sidof(i) if r.random() < 0.7 else 0) for i, tk in enumerate(toks)}
        players = []
        for i, tk in enumerate(toks):
            nm = r.choice(NAMES) if r.random() < 0.7 else None
            players.append([tk, nm, sids[tk], r.randrange(300), r.randrange(300), r.randrange(40), r.randrange(40, 80), r.randrange(0, 5), t[0] - r.randrange(10000)])
        roles = {}
        if r.random() < 0.7:
            lines = ["%d %s # x" % (sidof(i), r.choice(["", "lead", ""])) for i in range(ntok) if r.random() < 0.3]
            roles["devs.txt"] = "\n".join(lines) + "\n"
        if r.random() < 0.5:
            roles["web-admins.txt"] = "\n".join("%d # %s" % (sidof(i), r.choice(["web", "TEMP"])) for i in range(ntok) if r.random() < 0.3) + "\n"
        if r.random() < 0.5:
            roles["og-steamids.txt"] = "\n".join("%d" % sidof(i) for i in range(ntok) if r.random() < 0.4) + "\n"
        names = [[("s:%d" % sids[tk]) if sids[tk] and r.random() < 0.8 else tk[:48], r.choice(NAMES), r.choice(["", "AB", "x"]), t[0]] for tk in toks if r.random() < 0.4]
        w = {"players": players, "names": names, "roles": roles, "test_leave": True, "mm": {}, "silent": {tk: r.choice([0.0, 5.0, 100.0]) for tk in toks if r.random() < 0.3},
             "priv": [], "restored": {}, "swap": {}}
        emit_(op="world", w=w)
        for tk in toks:
            if r.random() < 0.88:
                emit_(op="frame", code=0xE4, pl=(tk.encode() + bytes([r.choice([1, 2, 2, 1, 3, 0])])).hex())
            tick()

    def counts_():
        k = r.choice([0, 0, 1, 3, 6, 18])
        out_ = []
        for _ in range(k):
            out_.append([r.choice(LABS), r.choice([0, 1, 2, 4, 5, 9, 13, 30, 120, 300, 5000])])
        return out_

    def e2frame(tk, parts, flags, counts):
        b = tk.encode() + (r.randrange(1 << 20)).to_bytes(4, "little") + parts.to_bytes(4, "little") + bytes([flags])
        if counts or r.random() < 0.2:
            b += bytes([len(counts)])
            for lab, v in counts:
                b += bytes([lab]) + v.to_bytes(2, "little")
        return b.hex()

    def read_ops():
        tk = r.choice(toks)
        x = r.random()
        h = (sidof(r.randrange(ntok + 3)).to_bytes(8, "little") if r.random() < 0.4 else b"")
        if x < 0.18:
            emit_(op="frame", code=0xE0, pl=(tk.encode() + h).hex())
        elif x < 0.28:
            emit_(op="frame", code=0xE8, pl=(tk.encode() + h).hex())
        elif x < 0.36:
            emit_(op="frame", code=0xEF, pl=(tk.encode() + bytes([r.choice([0, 1, 2, 3, 5, 9, 20, 33, 250])])).hex())
        elif x < 0.46:
            emit_(op="frame", code=0xB4, pl=(tk.encode() + bytes([r.choice([0, 1, 2, 9])]) * r.choice([0, 1, 1])).hex())
        elif x < 0.56:
            sec = r.choice([1, 2, 3, 4, 5, 7, 8, 1, 2])
            b = tk.encode() + bytes([r.choice([0, 1, 5]), sec])
            if r.random() < 0.8:
                b += r.randrange(1 << 20).to_bytes(4, "little")
            if r.random() < 0.6:
                b += r.randrange(900).to_bytes(2, "little")
            if r.random() < 0.7:
                b += bytes([r.choice([0, 1])])
            emit_(op="frame", code=0xB6, pl=b.hex())
        elif x < 0.62:
            emit_(op="frame", code=0xBF, pl=tk.encode().hex())
        elif x < 0.72:
            if r.random() < 0.35:
                emit_(op="frame", code=0xD0, pl=tk.encode().hex())
            else:
                nm = r.choice(NAMES + ["", "x" * 40, "ÁB", "bad<name>", "Alpha"]).encode()
                tg = r.choice([b"", b"AB", b"xyz1", b"toolongtag", "Ż".encode()])
                b = tk.encode() + bytes([len(nm)]) + nm + bytes([len(tg)]) + tg
                if r.random() < 0.1:
                    b = b[:-r.randrange(1, 4)]
                if r.random() < 0.05:
                    b = tk.encode() + bytes([5]) + b"\xff\xfe\xfd\xfc\xfb" + bytes([0])
                emit_(op="frame", code=0xD2, pl=b.hex())
        elif x < 0.80:
            lab = [[r.choice(LABS), r.choice([0, 3, 7, 40])] for _ in range(r.choice([0, 2, 5]))]
            b = tk.encode() + r.randrange(1 << 20).to_bytes(4, "little") + bytes([len(lab)]) + b"".join(bytes([a]) + v.to_bytes(2, "little") for a, v in lab)
            emit_(op="frame", code=0xA9, pl=b.hex())
        elif x < 0.88:
            lab = [[r.choice(LABS), r.choice([0, 3, 7, 40])] for _ in range(r.choice([0, 2, 5]))]
            b = tk.encode() + bytes([len(lab)]) + b"".join(bytes([a]) + v.to_bytes(2, "little") for a, v in lab)
            emit_(op="frame", code=0xAB, pl=b.hex())
        elif x < 0.94:
            emit_(op="frame", code=0xAA, pl=(tk.encode() + h).hex())
        elif x < 0.97:
            emit_(op="lb", top=r.choice([10, 3, 1000]))
        else:
            emit_(op="frame", code=r.choice([0xE0, 0xE2, 0xE8, 0xB4, 0xB6, 0xD0, 0xAA, 0xA9]), pl=r.choice(["", "00" * 10, ("zz" * 32).encode().hex(), "ab" * 33]))

    new_world()
    left = n - len(out)
    while len(out) < n:
        tick()
        x = r.random()
        if x < 0.04 and len(out) > 20:
            new_world()
            continue
        if x < 0.2:
            read_ops()
            continue
        if x < 0.23:
            # boosters: grant + pick
            tk = r.choice(toks)
            emit_(op="booster", token=tk, bid=r.choice([1, 2, 3, 4, 7, 9, 5]), n=r.choice([1, 2, 0, 3]))
            emit_(op="frame", code=0xEE, pl=(tk.encode() + bytes([r.choice([0, 1, 2, 3, 4, 7, 8])])).hex())
            continue
        if x < 0.25:
            emit_(op="dump")
            continue
        if x < 0.28:
            # an account that gets its Steam id after its clan exists under the install token id: the next clan_key adopts the clan
            emit_(op="setsid", token=r.choice(toks), sid=sidof(100 + r.randrange(5)))
            continue
        if x < 0.31:
            nm = r.choice(NAMES)
            body = {"name": nm, "steamid": r.choice([0, sidof(r.randrange(ntok)), str(sidof(r.randrange(ntok))), "abc", None, 3.7, [1]]),
                    "tokenIds": r.sample([tk[:48] for tk in toks], r.randrange(0, 3)), "names": r.random() < 0.3}
            if r.random() < 0.1:
                body["tokenIds"] = "ab"
            emit_(op="admin_reset", body=body)
            continue
        if x < 0.34:
            emit_(op="admin_restore", body={"file": r.choice(["@last", "@last", "@last", "", "nope.json", "../x"])})
            continue
        # ---- a match
        m = mid()
        k = r.choice([1, 2, 3, 4, 6, 8])
        pl = r.sample(toks, min(k, len(toks)))
        started = r.random() < 0.9
        mm = {"matches": {m: pl}, "started": [m] if started else []}
        if r.random() < 0.05:
            mm["ended"] = [m]
        if r.random() < 0.15:
            mm["departed"] = {m: r.sample(pl, r.randrange(0, len(pl) + 1))}
        if r.random() < 0.08:
            mm["joining"] = {r.choice(toks): [m, t[0]]}
        for tkn in pl:
            if r.random() < 0.15:
                mm.setdefault("late_mult", {})[tkn] = [m, r.choice([1.0, 1.25, 1.5, 2.9, 3.0])]
            if r.random() < 0.15:
                mm.setdefault("late_bonus", {})[tkn] = [m, r.choice([100, 200, 300])]
        emit_(op="mmset", m=mm)
        if r.random() < 0.05:
            emit_(op="silent", m={tkn: r.choice([0.0, 5.0, 100.0, 4000.0]) for tkn in pl})
        # boosters picked before the start
        for tkn in pl:
            if r.random() < 0.25:
                emit_(op="booster", token=tkn, bid=r.choice([1, 2, 3, 4, 7]), n=r.choice([1, 2]))
                emit_(op="frame", code=0xEE, pl=(tkn.encode() + bytes([r.choice([0, 1, 2, 3, 4, 7])])).hex())
        if r.random() < 0.06:
            emit_(op="priv", mids=[m])
        if r.random() < 0.08:
            emit_(op="restored", mid=m, t=t[0] - r.choice([5, 300, 900]))
        sp = pl if r.random() < 0.9 else pl[:1]
        if r.random() < 0.9:
            tick()
            emit_(op="started", players=sp, mid=m)
        for tkn in pl:
            if r.random() < 0.3:
                read_ops()
        # departures / marks
        for tkn in pl:
            y = r.random()
            if y < 0.10:
                emit_(op="set", name="CLAN_LEFT_AT", key=[tkn, m], val=t[0] + r.choice([0, 5, 50]))
                if r.random() < 0.6:
                    emit_(op="set", name="CLAN_EXCUSED", key=[tkn, m])
                    emit_(op="set", name="CLAN_EXCUSE_WHY", key=[tkn, m], val=r.choice(["host lost (migration pending)", "7 s after Bob left"]))
            elif y < 0.14:
                emit_(op="set", name="CLAN_QUITTERS", key=[tkn, m])
            elif y < 0.18:
                emit_(op="set", name="CLAN_KICKED", key=[tkn, m], val="ping limit")
            elif y < 0.22:
                emit_(op="set", name="CLAN_ABORTED", key=[tkn, m], val="everyone else left")
            elif y < 0.26:
                emit_(op="set", name="CLAN_MOVED", key=[tkn, m], val=t[0] - r.choice([0, 100, 300]))
            elif y < 0.30:
                emit_(op="set", name="CLAN_REBOOK", key=[tkn, m])
            elif y < 0.33:
                emit_(op="set", name="CLAN_REJOINED", key=[tkn, m], val=t[0] - r.choice([0, 100, 2000]))
            elif y < 0.36:
                emit_(op="set", name="CLAN_LINK_EXCUSES", key=tkn, val=[t[0] - r.choice([10, 90000]) for _ in range(r.choice([1, 2]))])
            if r.random() < 0.15:
                emit_(op="set", name="CLAN_DONE", key=tkn, val=m)
            if r.random() < 0.1:
                emit_(op="set", name="CLAN_CREDITED", key=[tkn, m])
        mode = r.random()
        n_rep = 0
        for tkn in pl:
            if r.random() < 0.85:
                parts = r.choice([0, 0, 50, 300, 900, 2500, 120000])
                flags = r.choice([0, 0, 1, 2, 2, 4, 6, 3, 0])
                counts = counts_()
                tick()
                if r.random() < 0.5:
                    emit_(op="report", token=tkn, parts=parts, flags=flags, counts=counts)
                else:
                    emit_(op="frame", code=0xE2, pl=e2frame(tkn, parts, flags, counts))
                n_rep += 1
                if r.random() < 0.2:
                    emit_(op="report", token=tkn, parts=parts, flags=flags, counts=counts)      # a re-send
                if r.random() < 0.12:
                    emit_(op="report", token=tkn, parts=r.choice([10, 700]), flags=r.choice([0, 2]), counts=counts_())   # a second half
                if r.random() < 0.12:
                    emit_(op="frame", code=0xA9, pl=(tkn.encode() + bytes(4) + bytes([2]) + bytes([r.choice(LABS)]) + (5).to_bytes(2, "little") + bytes([r.choice(LABS)]) + (9).to_bytes(2, "little")).hex())
        if r.random() < 0.3:
            emit_(op="timers")
        if mode < 0.45:
            if r.random() < 0.3:
                emit_(op="set", name="CLAN_WIN", key=[r.choice(pl), m], val=r.choice([True, False, None]))
            tick()
            emit_(op="ended", players=pl if r.random() < 0.85 else pl[:-1] or pl, mid=m)
        elif mode < 0.6:
            t[0] += r.choice([5, 9, 30])
            emit_(op="mer", mid=m)
            emit_(op="timers")
            emit_(op="tick")
        elif mode < 0.8:
            t[0] += r.choice([21, 25, 70])
            emit_(op="tick")
            emit_(op="timers")
            if r.random() < 0.5:
                emit_(op="ended", players=pl, mid=m)
        else:
            emit_(op="finalize", token=r.choice(pl))
            emit_(op="tick")
        if r.random() < 0.3:
            emit_(op="ended", players=[r.choice(pl)], mid=m)
        for tkn in pl:
            if r.random() < 0.2:
                emit_(op="finalize", token=tkn)
        if r.random() < 0.25:
            # a player that left and joined the SAME match again after its day was booked (clan_rejoin_after_booking): its next report replaces the booking
            for tkn in r.sample(pl, min(len(pl), r.choice([1, 2]))):
                for nm_ in ("CLAN_CREDITED", "CLAN_EXCUSED", "CLAN_QUITTERS"):
                    emit_(op="unset", name=nm_, key=[tkn, m])
                for nm_ in ("CLAN_LEFT_AT", "CLAN_REJOINED", "CLAN_MOVED"):
                    emit_(op="unset", name=nm_, key=[tkn, m])
                emit_(op="unset", name="CLAN_DONE", key=tkn)
                emit_(op="set", name="CLAN_REBOOK", key=[tkn, m])
                tick()
                emit_(op="report", token=tkn, parts=r.choice([0, 150, 800]), flags=r.choice([0, 2, 2]), counts=counts_())
                emit_(op="finalize", token=tkn)
                if r.random() < 0.3:
                    emit_(op="finalize", token=tkn)
        if r.random() < 0.4:
            emit_(op="dump")
    emit_(op="dump")
    return out


if __name__ == "__main__":
    main(gen, run)
