"""Differential test of daemon.py's K5 chunk (handle_batch = the request dispatcher, handle_leave, handle_match_end, loaded_clear_match,
jip_requeue_if_empty, prestart_host_lost, maybe_game_start) against `t2r-daemon selftest k5`.

  python3 k5_harness.py gen SEED N > script.jsonl
  T2R_DB=/tmp/k5py.db T2R_TOOLS_DIR=/tmp/k5tools python3 k5_harness.py run < script.jsonl > out.py.jsonl
  T2R_DB=/tmp/k5rs.db T2R_TOOLS_DIR=/tmp/k5tools t2r-daemon selftest k5 < script.jsonl > out.rs.jsonl ; cmp ...

daemon.py is imported from T2R_PY_DIR (no threads start at import). Time is frozen per line ("t"), secrets / randbelow come from the one seeded stream the
Rust side uses, connections are fakes that record what was sent (Rust: socket pairs read back after each call), threading.Timer is queued ("fire").
Items of other chunks that are not ported yet (clan glue, K6 loop items, the private hooks) are replaced on BOTH sides by the same deterministic
stand-ins (the Rust ones are in src/d/ext_k5.rs): they log one "STUB ..." line per call, or answer from the scripted "world".
"""
import json
import os
import random
import secrets
import sys
import types

os.environ.setdefault("T2R_HOST_CAP", "0")
os.environ.setdefault("T2R_DB", "/tmp/k5py.db")
os.environ.setdefault("T2R_TOOLS_DIR", "/tmp/k5tools")
os.environ["T2R_NO_COUNTDOWN"] = "1"
import cseed  # noqa: E402
from cseed import T, emit, log, main, script  # noqa: E402

secrets.randbelow = lambda n: cseed.RNG._randbelow(n)
import time as _time  # noqa: E402
_time.time = lambda: T[0]
_time.monotonic = lambda: T[0]          # daemon.py's steady clock (_SteadyTime) = wall clock at start + monotonic: both frozen, so every object that bound daemon.time.time reads T

DB = os.environ["T2R_DB"]
DIR = os.path.dirname(os.path.abspath(DB))
TOOLS = os.environ["T2R_TOOLS_DIR"]
NAMES = ["Alpha", "bravo", "Charlie Brown", "Charlie", "delta force", "Echo", "Foxtrot", "golf", "Hotel", "india", "Juliet", "kilo", "Zoe-1.x", "Ünï"]
LOCS = ["ams=20+fra=25,lhr=40", "iad=18 atl=22 ord=30", "sea=12,lax=30,sfo=35", "sgp=40 hkg=60", "nothing here", "lhr=5", "ams=900"]
SID0 = 76561198000000000
CONNS = {}
STS = {}
STUB = {"banned": set(), "banned_ips": set(), "install": set(), "loss_hold": set(), "ac_ip": set()}
TIMERS = []


class FakeConn:
    def __init__(self, cid):
        self.cid = cid
        self.buf = b""

    def sendall(self, data, drop=False):
        self.buf += bytes(data)


def conn(cid):
    if cid not in CONNS:
        CONNS[cid] = FakeConn(cid)
    return CONNS[cid]


def tok(i):
    return ("%02x" % i + "c0ffee11deadbeef" * 4)[:64]


def hx(b):
    return None if b is None else bytes(b).hex()


def unhx(s):
    return bytes.fromhex(s)


def setup():
    import daemon as d
    real = d.time

    class Shim:
        def time(self):
            return T[0]

        def monotonic(self):
            return T[0]

        def sleep(self, x):
            pass

        def __getattr__(self, k):
            return getattr(real, k)

    d.time = Shim()
    realthr = d.threading

    class Tmr:
        def __init__(self, delay, f, args=(), kwargs=None):
            self.delay = delay
            self.f = lambda: f(*args, **(kwargs or {}))

        def start(self):
            TIMERS.append(self.f)

    class Thr:
        def __init__(self, target=None, args=(), kwargs=None, daemon=None, name=None):
            self.t, self.a, self.k = target, args, kwargs or {}

        def start(self):
            self.t(*self.a, **self.k)

    class ThrShim:
        Timer = Tmr
        Thread = Thr

        def __getattr__(self, k):
            return getattr(realthr, k)

    d.threading = ThrShim()
    orig_log = d.log
    seen = set()

    def repoint(o, depth):
        if id(o) in seen or depth > 2 or isinstance(o, (type, types.ModuleType, types.FunctionType, str, bytes, int, float, dict, list, set, tuple)):
            return
        seen.add(id(o))
        for k, v in list(getattr(o, "__dict__", {}).items()):
            if v is orig_log:
                setattr(o, k, log)
            elif callable(v) and getattr(v, "__self__", None) is real and getattr(v, "__name__", "") == "time":
                setattr(o, k, d.time.time)
            elif hasattr(v, "__dict__"):
                repoint(v, depth + 1)

    for v in list(vars(d).values()):
        if hasattr(v, "__dict__"):
            repoint(v, 0)
    d.log = log
    d.evt = lambda kind, *a: log("EVT", kind, *a)
    d.SERVER_VERSION = "sv-test"
    d._ping_gate = lambda a, b, now=None: d.pair_ping(a, b)
    # ---- the stand-ins (same behaviour as src/d/ext_k5.rs)
    d.readopt_claim = lambda t: log("STUB", "readopt_claim", t)
    d.live_save = lambda now=None: log("STUB", "live_save", "")
    d.clan_match_ended = lambda players, mid: log("STUB", "clan_match_ended", "%s %s" % (list(players), mid))
    d.clan_match_started = lambda players, mid: log("STUB", "clan_match_started", "%s %s" % (list(players), mid))

    def tagged(t, op):
        def f(pl):
            if len(pl) % 5 == 0:
                return None
            return bytes([ord(t), op]) + bytes(pl[:6])
        return f

    d.names_op = lambda op, pl: tagged("N", op)(pl)
    d.clan_op = lambda op, pl: tagged("C", op)(pl)
    d.meta_op = lambda op, pl: tagged("M", op)(pl)
    d.rank_table_op = tagged("R", 0xBF)
    d.feed_op = tagged("F", 0xB4)
    d.clan13_op = tagged("B", 0xB6)
    d.ach_report_op = tagged("A", 0xA9)
    d.ach_live_op = tagged("A", 0xAB)
    d.ach_read_op = tagged("A", 0xAA)
    d.achievements = d.achievements or types.SimpleNamespace()
    d.leaderboard_table = lambda top=10: b"LB" + bytes([top])

    def stats_roster(c, mid, mapname, rows, winner):
        r = ", ".join("(%r, %r, %d, %s)" % (t, n, tm, l) for t, n, tm, l in rows)
        log("STUB", "stats_roster", "%s %r [%s] %s" % (mid, mapname, r, winner))
    sys.modules["clan_ingame"] = types.SimpleNamespace(stats_roster=stats_roster)
    d.loss_hold_note = lambda token, now=None: b"LH" if token in STUB["loss_hold"] else None
    d.is_banned = lambda token, name=None, ip=None: bool(token in STUB["banned"] or (ip and ip in STUB["banned_ips"]))
    d.ip_banned = lambda ip: "10.0.0.0/8" if ip in STUB["banned_ips"] else None
    d.ident_note = lambda token, steamid=0, hwids=(), ip=None: log("STUB", "ident_note", "%s %s %s" % (token, steamid, ip))
    d.ident_pid = lambda token, steamid=0: ("PID-" + token[:4]) if token in STUB["banned"] else None
    d.attest.WAIT_S = 5.0
    d.install_refusal = lambda token, ip, wait_s=0.0: (("stub install" + (" boot" if wait_s else "")), b"IR") if token in STUB["install"] else None
    d.hwid_op = lambda pl, ip=None: b"HW" + bytes(pl[:2]) + (ip or "").encode()
    d.attest_op = lambda pl, addr: b"AT" + bytes(pl[:2]) + addr[0].encode()
    for nm, o in (("ac_events_op", 0xCC), ("ac_demo_op", 0xCE), ("ac_report_op", 0xD4), ("ac_recent_op", 0xD6)):
        setattr(d, nm, (lambda o: lambda pl: None if len(pl) % 2 == 0 else bytes([ord("A"), ord("C"), o, pl[0]]))(o))
    d.AC = types.SimpleNamespace(note_ip=lambda t, ip: (7, 0.5, "stub evidence") if t in STUB["ac_ip"] else None)
    d.ac_server_event = lambda token, mid, kind, w, evidence: log("STUB", "ac_server_event", "%s %s %s %s %s" % (token, mid, kind, w, evidence))
    d._SILENT.clear()
    return d


def clear_all(d):
    CONNS.clear()
    STS.clear()
    del TIMERS[:]
    for k in STUB:
        STUB[k].clear()
    for dd in (d.CLIENT_VERSIONS, d.CLIENT_LANGS, d.MATCH_MAP, d.VOTES, d.MATCH_P2P, d.SID_KEEP, d.TIMING, d.TEAM_SWAP, d.MATCH_P1, d.PING, d.PING_AT,
               d.PING_LOC, d.LOC_FWD, d.LOC_HIST, d.LOC_SPIKES, d.REGION, d.REGION_CAND, d.PING_LAST, d.MEASURED_PAIR, d.PING_EXCESS, d.PING_CORR,
               d.PING_FORMED, d.MESH_ON, d.SESS_LOST, d.NAMELESS_BY_IP, d.PEERS, d.VOTE_SHOWN, d._JIP_HV, d.BRIDGES.by_token, d.BRIDGES.by_sid,
               d.SESSIONS.by_token, d.PRES.players, d.PRES.udp_seen, d.LOADED_T, d.CLAN_WIN, d.CLAN_MAP, d._JIP_WAIT_LOG, d.JIP_STARTED, d.JIP_REPUSH,
               d.JIP_LATE, d._SILENT, d.CLAN_LAST_MATCH, d.NEW_TOKENS.hits):
        dd.clear()
    for ss in (d.LINUX_CLIENTS, d.LOADED, d.CD_GOT, d._POOL_LOGGED, d.RESULT_BOOKED, d.END_BY_REPORTS):
        ss.clear()
    del d.VOTE_RECENT[:]
    d._save_due[0] = 0.0
    d._rotation[0] = 2
    d.MM.__init__()
    for f in ("client_versions.json", "linux_clients.json", "client_langs.json", "no_host.txt"):
        p = os.path.join(DIR, f)
        if os.path.exists(p):
            os.remove(p)
    d._NO_HOST.update(mtime=None, ids=set(), names=set())
    d.PERF.__init__(now=lambda: T[0])
    with d._dblock, d.dbc() as c:
        d.names.init(c)
        c.execute("CREATE TABLE IF NOT EXISTS match_stats(token TEXT, kills INT, deaths INT, ts REAL)")
        for t in [r[0] for r in c.execute("SELECT name FROM sqlite_master WHERE type='table'").fetchall()]:
            if not t.startswith("sqlite_"):
                c.execute("DELETE FROM %s" % t)
    d._name_cache.clear()
    d._steam_cache.clear()
    d._tok_gen.clear()
    d._name_gen[0] = 0
    d._name_ver[0] = 0
    for m in ("m0", "m1", "m2"):
        d.HM.end(m)
    for t in [tok(i) for i in range(12)]:
        d.PARTIES.leave(t)


def jr(v):
    return json.dumps(v, sort_keys=True, separators=(",", ":"))


def dbdump():
    import sqlite3
    c = sqlite3.connect(DB)
    out = {}
    for (t,) in c.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name").fetchall():
        if t.startswith("sqlite_"):
            continue
        rows = []
        for r in c.execute("SELECT * FROM %s" % t).fetchall():
            rows.append([x.hex() if isinstance(x, (bytes, bytearray)) else x for x in r])
        rows.sort(key=jr)
        out[t] = rows
    c.close()
    return out


def state(d):
    s = {}
    m = d.MM
    s["q"] = list(m.queue.items())
    s["matches"] = [[k, list(v)] for k, v in m.matches.items()]
    s["started"] = sorted(m.started)
    s["st_t"] = sorted(m.started_t.items())
    s["launched"] = sorted(m.launched)
    s["dep"] = sorted([k, sorted(v)] for k, v in m.departed.items())
    s["ended"] = sorted(m.ended)
    s["joining"] = [[k, list(v)] for k, v in m.joining.items()]
    s["left"] = sorted([k, list(v)] for k, v in m.left.items())
    s["loaded"] = sorted(d.LOADED)
    s["loaded_t"] = sorted(d.LOADED_T.items())
    s["cdgot"] = sorted(d.CD_GOT)
    s["lost"] = sorted(d.SESS_LOST.items())
    s["jstart"] = sorted(d.JIP_STARTED.items())
    s["jrepush"] = sorted(d.JIP_REPUSH.items())
    s["jlate"] = sorted(d.JIP_LATE.items())
    s["jwait"] = sorted(d._JIP_WAIT_LOG.items())
    s["cwin"] = [[k[0], k[1], v] for k, v in d.CLAN_WIN.items()]
    s["booked"] = sorted(d.RESULT_BOOKED)
    s["silent"] = sorted(d._SILENT.items())
    s["tm"] = sorted([sorted(k), t["start"], list(t["marks"].items()), list(t["loaded"].items())] for k, t in d.TIMING.items())
    s["pres"] = [[t, p.get("name"), p.get("last")] for t, p in d.PRES.players.items()]
    s["br"] = [[t, b["steamid"], b["port"], b["ip"], b["name"], b["conn"].cid] for t, b in d.BRIDGES.by_token.items()]
    s["sess"] = [[t, c.cid] for t, c in d.SESSIONS.by_token.items()]
    s["nl"] = sorted(d.NAMELESS_BY_IP.items())
    s["ploc"] = list(d.PING_LOC.items())
    s["cv"] = list(d.CLIENT_VERSIONS.items())
    s["hm"] = [[t, bool(d.HM.has(t)), d.HM.host_token(t) if d.HM.has(t) else None] for t in [tok(i) for i in range(12)]]
    s["hmx"] = [d.HM.export_match(m) for m in ("m0", "m1", "m2")]
    s["notes"] = [[t, [list(n) for n in lst]] for t, lst in sorted(d.SOCIAL.notes.items())]
    s["party"] = [[t, d.PARTIES.pid(t)] for t in [tok(i) for i in range(12)]]
    s["db"] = dbdump()
    sts = {}
    for cid in sorted(STS):
        st = STS[cid]
        sts[cid] = [st["token"], st["name"], st["authed"], st["session"], st["is_bridge"], bool(st.get("closed_sent")), bool(st.get("menu")),
                    st.get("started"), st.get("sent_name"), [x.hex() for x in st.get("sent_lobby", [])]]
    s["st"] = sts
    sent = {}
    for cid in sorted(CONNS):
        c = CONNS[cid]
        if c.buf:
            sent[cid] = c.buf.hex()
            c.buf = b""
    s["sent"] = sent
    return s


def kind_of(e, d):
    if isinstance(e, d.RequestAbandoned):
        return "aband"
    if isinstance(e, (ConnectionError, TimeoutError, OSError)):
        return "conn"
    return "other"


def run_batch(d, c):
    cid = c["cid"]
    if cid not in STS or c.get("new"):
        STS[cid] = {"token": None, "name": "PLAYER", "authed": False, "session": False, "is_bridge": False}
    st = STS[cid]
    batch = [(op, unhx(h)) for op, h in c["frames"]]
    a = tuple(c["addr"])
    x = None
    try:
        d.handle_batch(conn(cid), a, st, batch)
    except BaseException as e:  # noqa: BLE001
        if isinstance(e, (KeyboardInterrupt, SystemExit)):
            raise
        x = kind_of(e, d)
    return x


def do_call(d, f, a):
    if f == "loaded_clear":
        return d.loaded_clear_match(a[0], a[1])
    if f == "jip_requeue":
        return d.jip_requeue_if_empty(a[0])
    if f == "prestart":
        return d.prestart_host_lost(a[0], a[1], a[2])
    if f == "mgs":
        try:
            return d.maybe_game_start(a[0])
        except BaseException as e:  # noqa: BLE001
            return "exc:" + kind_of(e, d)
    if f == "leave":
        st = {"token": a[0], "name": a[1]}
        return d.handle_leave(tuple(a[2]), st)
    raise SystemExit("unknown f " + f)


def run():
    d = setup()
    os.makedirs(TOOLS, exist_ok=True)
    clear_all(d)
    n = 0
    for c in script():
        T[0] = c.get("t", T[0])
        op = c["op"]
        n += 1
        if op == "reset":
            clear_all(d)
            emit(n, None, state(d))
            continue
        if op == "env":
            if c["v"] is None:
                os.environ.pop(c["k"], None)
            else:
                os.environ[c["k"]] = c["v"]
            emit(n, None, None)
            continue
        if op == "seedrot":
            d._rotation[0] = c["v"]
            cseed.RNG.seed(c["seed"])
            emit(n, None, None)
            continue
        if op == "names":
            for t, nm in c["n"].items():
                d.set_name(t, nm)
            emit(n, None, None)
            continue
        if op == "world":
            for k in STUB:
                STUB[k].clear()
                STUB[k].update(c.get(k, []))
            d.CLAN_MAP.clear()
            d.CLAN_MAP.update(c.get("clanmap", {}))
            d.RESULT_BOOKED.clear()
            d.RESULT_BOOKED.update(c.get("booked", []))
            d.END_BY_REPORTS.clear()
            d.END_BY_REPORTS.update(c.get("endrep", []))
            d.CLAN_LAST_MATCH.clear()
            d.CLAN_LAST_MATCH.update(c.get("lastmatch", {}))
            d.LINUX_CLIENTS.clear()
            d.LINUX_CLIENTS.update(c.get("linux", []))
            d.CLIENT_VERSIONS.clear()
            for t, v in c.get("cver", {}).items():
                d.CLIENT_VERSIONS[t] = v
            emit(n, None, None)
            continue
        if op == "party":
            for t in [tok(i) for i in range(12)]:
                d.PARTIES.leave(t)
            for g in c["groups"]:
                for m in g[1:]:
                    d.PARTIES.invite_token(g[0], m)
                    d.PARTIES.accept_token(m, g[0])
            emit(n, None, None)
            continue
        if op == "mm":
            m = d.MM
            m.matches.clear()
            for k, v in c["matches"].items():
                m.matches[k] = list(v)
            m.launched.clear()
            m.launched.update(c.get("launched", []))
            m.ended.clear()
            m.ended.update(c.get("ended", []))
            m.departed.clear()
            for k, v in c.get("departed", {}).items():
                m.departed[k] = set(v)
            m.queue.clear()
            for t in c.get("queue", []):
                m.queue[t] = T[0]
            m.left.clear()
            m.joining.clear()
            for k, v in c.get("joining", {}).items():
                m.joining[k] = tuple(v)
            m.started.clear()
            m.started.update(c.get("started", []))
            m.started_t.clear()
            m.started_t.update(c.get("started_t", {}))
            m.seen.clear()
            for t in c.get("queue", []):
                m.seen[t] = T[0]
            d.LOADED.clear()
            d.LOADED.update(c.get("loaded", []))
            d.JIP_LATE.clear()
            for k, v in c.get("jlate", {}).items():
                d.JIP_LATE[k] = tuple(v)
            d.HM.end("zz")
            for mid in c.get("hm", []):
                d.HM.start(mid, list(m.matches[mid]))
            d.TIMING.clear()
            for k, v in c.get("timing", []):
                d.TIMING[frozenset(k)] = {"start": v, "marks": {}, "loaded": {}}
            emit(n, None, state(d))
            continue
        if op == "fire":
            fs = TIMERS[:]
            del TIMERS[:]
            for f in fs:
                f()
            emit(n, None, state(d))
            continue
        if op == "batch":
            x = run_batch(d, c)
            emit(n, x, state(d))
            continue
        f, a = c["f"], c.get("a", [])
        r = do_call(d, f, a)
        emit(n, r, state(d))


# ---------------------------------------------------------------------------------------------------------------- generator
def frame_login(i, big=False, bad=False):
    t = tok(i)
    if bad:
        t = "zz" + t[2:]
    pl = b"\x1c" + t.encode() + b"\x01\x02\x07"
    if big:
        pl += bytes((i * 7 + k) & 0xff for k in range(60))
    return [0x0f, pl.hex()]


def frame_sess(op, name):
    pl = b"\x1c" + b"\x00" * 8 + name.encode("latin1", "ignore")[:32].ljust(32, b"\x00")
    return [op, pl.hex()]


def frame_bridge(i, r, good=True):
    sid = SID0 + i if good else r.choice([5, SID0 + 10 ** 10, SID0 - 10 ** 9])
    nm = r.choice(["", "Steam%d" % i, "Ünï%d" % i, "x" * 40])
    nmb = nm.encode("utf-8")[:60]
    port = r.choice([40000 + i, 0, 70000 % 65536 if r.random() < 0.3 else 41000 + i])
    pl = tok(i).encode() + sid.to_bytes(8, "little") + bytes([len(nmb)]) + nmb + port.to_bytes(2, "little")
    if r.random() < 0.04:
        pl = pl[:r.randint(60, 76)]
    return [0xF0, pl.hex()]


def gen(r, n):
    toks = [tok(i) for i in range(12)]
    t = 5000.0
    out = []

    def emit_(c):
        out.append(c)

    emit_({"t": t, "op": "reset"})
    emit_({"t": t, "op": "seedrot", "v": r.randrange(0, 40), "seed": r.randrange(1, 10 ** 6)})
    emit_({"t": t, "op": "names", "n": {x: r.choice(NAMES) for x in toks if r.random() < 0.7}})
    ips = ["1.2.3.%d" % k for k in range(1, 5)] + ["127.0.0.1", "9.9.9.9"]
    port_n = [41000]

    def addr(i):
        port_n[0] += 1
        return [ips[i % len(ips)] if r.random() < 0.8 else r.choice(ips), port_n[0]]

    def world():
        w = {"t": t, "op": "world"}
        w["banned"] = [x for x in toks if r.random() < 0.08]
        w["banned_ips"] = [x for x in ips if r.random() < 0.08]
        w["install"] = [x for x in toks if r.random() < 0.08]
        w["loss_hold"] = [x for x in toks if r.random() < 0.08]
        w["ac_ip"] = [x for x in toks if r.random() < 0.3]
        w["clanmap"] = {"m%d" % k: r.choice(["to1-forest", "to1-school", "Ünï"]) for k in range(3) if r.random() < 0.7}
        w["booked"] = ["m%d" % k for k in range(3) if r.random() < 0.15]
        w["endrep"] = ["m%d" % k for k in range(3) if r.random() < 0.12]
        w["lastmatch"] = {x: "m%d" % r.randrange(3) for x in toks if r.random() < 0.3}
        w["linux"] = [x for x in toks if r.random() < 0.1]
        w["cver"] = {x: r.choice(["sv-test", "old-1", "sv-test"]) for x in toks if r.random() < 0.6}
        return w

    def mm():
        pool = toks[:]
        r.shuffle(pool)
        m = {}
        i = 0
        while pool and i < 3:
            k = r.randint(2, 6)
            m["m%d" % i] = [pool.pop() for _ in range(min(k, len(pool)))]
            i += 1
        q = [x for x in pool if r.random() < 0.5]
        mids = list(m)
        started = [x for x in mids if r.random() < 0.5]
        launched = [x for x in mids if x in started or r.random() < 0.7]
        joining = {}
        for x in mids:
            if x in started and r.random() < 0.5:
                cand = [p for p in m[x] if r.random() < 0.4]
                for p in cand:
                    joining[p] = [x, t - r.choice([2.0, 30.0])]
        loaded = [p for x in mids for p in m[x] if r.random() < 0.45]
        c = {"t": t, "op": "mm", "matches": m, "launched": launched, "ended": [x for x in mids if r.random() < 0.1],
             "departed": {x: [y for y in m[x] if r.random() < 0.18] for x in mids}, "queue": q, "joining": joining, "started": started,
             "started_t": {x: t - r.choice([3.0, 50.0, 200.0]) for x in mids}, "loaded": loaded,
             "jlate": {p: [r.choice(mids), t - r.choice([5.0, 150.0])] for x in mids for p in m[x] if r.random() < 0.08},
             "hm": [x for x in started if r.random() < 0.6],
             "timing": [[m[x], t - 20.0] for x in mids if r.random() < 0.5]}
        return c

    emit_(world())
    emit_(mm())
    # every player logs in on its session conn and registers a bridge
    for i in range(12):
        emit_({"t": t, "op": "batch", "cid": "b%d" % i, "addr": ["1.2.3.%d" % (i % 4 + 1), 5000 + i], "new": True, "frames": [frame_bridge(i, r)]})
        emit_({"t": t, "op": "batch", "cid": "s%d" % i, "addr": ["1.2.3.%d" % (i % 4 + 1), 6000 + i], "new": True, "frames": [frame_login(i, big=r.random() < 0.3)]})
    while len(out) < n:
        t += r.choice([0.0, 0.0, 1.0, 2.0, 5.0, 30.0, 100.0])
        k = r.random()
        i = r.randrange(12)
        sc = "s%d" % i
        base = {"t": t, "op": "batch", "cid": sc, "addr": addr(i)}
        if k < 0.04:
            emit_(mm())
        elif k < 0.06:
            emit_(world())
        elif k < 0.07:
            emit_({"t": t, "op": "party", "groups": [[toks[a], toks[b]] for a, b in [r.sample(range(12), 2) for _ in range(r.randint(0, 3))] if a != b]})
        elif k < 0.10:
            emit_({"t": t, "op": "fire"})
        elif k < 0.13:
            # a bridge (re)registration / ping location / ping report
            fr = []
            for _ in range(r.randint(1, 3)):
                x = r.random()
                if x < 0.35:
                    fr.append(frame_bridge(i, r, good=r.random() < 0.85))
                elif x < 0.7:
                    fr.append([0xF4, r.choice(LOCS + ["ébad", "x" * 950]).encode("latin1", "ignore").hex()])
                else:
                    pl = (SID0 + r.randrange(12)).to_bytes(8, "little") + r.choice([5, 80, 400, -1]).to_bytes(4, "little", signed=True)
                    fr.append([0xF6, pl[:r.choice([12, 12, 12, 9])].hex()])
            emit_(dict(base, cid="b%d" % i, new=r.random() < 0.3, frames=fr))
        elif k < 0.20:
            # login / boot / announce (the connection may be new or reused)
            fr = []
            if r.random() < 0.9:
                fr.append(frame_login(i, big=r.random() < 0.25, bad=r.random() < 0.05))
            if r.random() < 0.6:
                fr.append(frame_sess(0x0e, r.choice(NAMES + ["", "PLAYER"])))
            if r.random() < 0.25:
                fr.append(frame_sess(0x01, r.choice(NAMES)))
            if r.random() < 0.15:
                fr.append([r.choice([0x05, 0x15, 0x13, 0x07]), ""])
            emit_(dict(base, new=r.random() < 0.4, cid=sc if r.random() < 0.6 else "x%d" % len(out), frames=fr))
        elif k < 0.32:
            # search (FIND MATCH)
            fr = []
            if r.random() < 0.5:
                fr.append(frame_login(i))
            fr.append(frame_sess(r.choice([0x01, 0x01, 0xfc]), r.choice(NAMES)))
            if r.random() < 0.15:
                fr.append([0x0f, frame_login(i)[1]])
            emit_(dict(base, new=r.random() < 0.15, frames=fr))
        elif k < 0.46:
            # keepalive / poll on a held conn
            fr = [[r.choice([0x05, 0x15, 0x05]), ""]]
            if r.random() < 0.15:
                fr.append([0x13, ""])
            emit_(dict(base, frames=fr))
        elif k < 0.58:
            # level loaded
            fr = [[0x09, r.choice(["", "01", "00ff"])]]
            if r.random() < 0.1:
                fr.append([0x05, ""])
            emit_(dict(base, frames=fr))
        elif k < 0.68:
            fr = [[0x02, ""]]
            if r.random() < 0.2:
                fr.append([0x05, ""])
            if r.random() < 0.15:
                fr.append([0x01, frame_sess(0x01, "A")[1]])
            emit_(dict(base, frames=fr))
        elif k < 0.78:
            # match end from (possibly) the host
            host_i = i
            fr = [[0x12, r.choice(["00", "01", "02", "03", "ff", ""]) ]]
            if r.random() < 0.3:
                fr.append([0x02, ""])
            emit_(dict(base, frames=fr))
        elif k < 0.95:
            # ext short connection ops
            ops_ = [0xD0, 0xD2, 0xCC, 0xCE, 0xD4, 0xD6, 0xE0, 0xE2, 0xE4, 0xB4, 0xBF, 0xA9, 0xAA, 0xAB, 0xB6, 0xE8, 0xEE, 0xEF, 0xE3, 0xE6, 0xE6, 0xB2, 0xA1,
                    0xB0, 0xBA, 0xF8, 0xDA, 0xD8, 0xDE, 0xBC, 0xDC, 0xCA, 0xC8, 0xC6, 0xEA, 0xEC, 0x77, 0x08]
            fr = []
            for _ in range(r.choice([1, 1, 1, 2])):
                o = r.choice(ops_)
                body = bytes(r.getrandbits(8) for _ in range(r.choice([0, 3, 10, 12, 20, 30, 70])))
                pl = (tok(r.randrange(12)).encode() + body) if r.random() < 0.85 else body
                if o == 0xE6:
                    pl = tok(i).encode() + bytes([r.choice([0, 1, 3, 9]), r.choice([0, 1, 5])])[:r.choice([2, 2, 1, 0])]
                if o == 0xB0:
                    pl = bytes([r.choice([0, 1, 2])]) + b"sv-test"[:r.choice([0, 3, 7])]
                fr.append([o, pl.hex()])
            if r.random() < 0.1:
                fr.append(frame_login(i))
            emit_(dict(base, cid="x%d" % len(out), new=True, frames=fr))
        else:
            # direct calls
            mids = ["m0", "m1", "m2", "zz"]
            f = r.choice(["loaded_clear", "jip_requeue", "prestart", "mgs", "mgs", "mgs", "leave"])
            mid = r.choice(mids)
            if f == "loaded_clear":
                a = [mid, r.sample(toks, r.randint(0, 5))]
            elif f == "jip_requeue":
                a = [mid]
            elif f == "prestart":
                a = [mid, r.sample(toks, r.randint(1, 5)), r.sample(toks, r.randint(0, 4))]
            elif f == "mgs":
                a = [r.choice(toks)]
            else:
                a = [r.choice(toks + [None]), "Nm", addr(i)]
            emit_({"t": t, "op": "call", "f": f, "a": a})
    return out


if __name__ == "__main__":
    main(gen, run)
