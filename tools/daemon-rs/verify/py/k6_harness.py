"""Differential test of daemon.py's K6 chunk (reaper, live-matches.json export / save / load / re-adoption, net report, live snapshot, gc, ping gate /
ping_check, packet loss / loss_check / host handover, kick / AFK, admin HTTP handler) against `t2r-daemon selftest k6`.

  python3 k6_harness.py gen SEED N > script.jsonl
  T2R_DB=/tmp/k6py/x.db python3 k6_harness.py run < script.jsonl > out.py.jsonl
  T2R_DB=/tmp/k6rs/x.db T2R_TEST_EPOCH=1 t2r-daemon selftest k6 < script.jsonl > out.rs.jsonl ; cmp ...

daemon.py is imported from T2R_PY_DIR. The world (names, Steam ids, scripted pair pings, matches, presence ...) is set per "world" line in both languages;
the K1 / K5 calls the K6 code makes (leave_running_match, forget_match_state, abort_award, maybe_game_start, loaded_clear_match, jip_requeue_if_empty,
match_end_via_reports) are recorded and returned as part of the result. Time is frozen per line ("t").
"""
import hashlib
import io
import json
import os
import random
import sys

os.environ.setdefault("T2R_DB", "/tmp/k6py/x.db")
os.environ["T2R_HOST_MIGRATION"] = "1"
os.environ["T2R_HOST_REPICK"] = "2"
os.environ["T2R_STATUS_FILE"] = "0"
os.environ["T2R_PRIVATE"] = "0"
os.environ["T2R_HOST_CAP"] = "0"
os.environ["T2R_VERSION"] = "k6test"
os.environ["T2R_LOSS_HOST_S"] = "30"
os.environ.setdefault("T2R_TOOLS_DIR", "/tmp/k6py/tools")
import cseed  # noqa: E402
from cseed import T, emit, log, main, script  # noqa: E402

import time as _time  # noqa: E402
_time.time = lambda: T[0]

DB = os.environ["T2R_DB"]
DIR = os.path.dirname(os.path.abspath(DB))
SECRET = "k6secret0123456789abcdef"
WORLD = {}
REC = []
MGS = [False]
H = [None]
MTIME = [1000]


def tok(i):
    return ("%02x" % i + "c0ffee11deadbeef" * 4)[:64]


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
    def log6(*a):
        if a and a[0] == "ADMIN-ERR":
            a = ("ADMIN-ERR",)
        log(*a)

    d.log = log6
    for o in (d.HM, d.REPICK, d.GL, d.INT, d.NOTICES, d.PARTIES, d.SOCIAL, d.PERF, d.MODEQ):
        if hasattr(o, "log"):
            o.log = log6
    d.evt = lambda kind, *a: log("EVT", kind, *a)
    d.SERVER_VERSION = "sv-test"
    d.STARTED_AT = 1000.0
    d.get_name = lambda t: WORLD.get("names", {}).get(t) or None
    d.token_steamid = lambda t: WORLD.get("sids", {}).get(t, 0)
    d.perf_penalty = lambda t: WORLD.get("perf", {}).get(t, 0)
    d.no_host = lambda t: t in WORLD.get("noh", [])
    d.ping_match_ms = lambda: WORLD.get("lim", 175)

    def pp(a, b, raw=False, now=None):
        return WORLD.get("pings", {}).get("%s|%s" % (a, b))

    d.pair_ping = pp
    d.PRES_TOKENS = lambda: list(WORLD.get("online", []))
    d.mm_region = lambda t: None
    d.TEAM_RULES_FILE = os.path.join(os.environ["T2R_TOOLS_DIR"], "team_rules.txt")
    d.TOP_FILE = os.path.join(os.environ["T2R_TOOLS_DIR"], "top_players.txt")
    os.makedirs(os.environ["T2R_TOOLS_DIR"], exist_ok=True)
    d._rules_cache.update(mt=None, rules=())
    d.countdown_seconds = lambda: 10.0
    d.has_helper = lambda t: t in WORLD.get("helpers", [])

    def rec(s):
        REC.append(s)

    def leave(t, why):
        rec("leave_running_match %s %s" % (t, why))
        return True

    d.leave_running_match = leave
    d.forget_match_state = lambda p: rec("forget_match_state %s" % ",".join(p))
    d.abort_award = lambda mid, others, host, why: (rec("abort_award %s %s %s %s" % (mid, ",".join(others), host if host is not None else "None", why)), True)[1]
    d.loaded_clear_match = lambda mid, players: rec("loaded_clear_match %s %s" % (mid, ",".join(players or [])))
    d.jip_requeue_if_empty = lambda mid: (rec("jip_requeue_if_empty %s" % mid), False)[1]
    d.maybe_game_start = lambda t: (rec("maybe_game_start %s" % t), MGS[0])[1]
    d.match_end_via_reports = lambda mid, now=None: (rec("match_end_via_reports %s" % mid), False)[1]
    for o in (d.MODEQ, d.HM, d.REPICK, d.GL, d.INT, d.NOTICES, d.PARTIES, d.SOCIAL, d.PERF):
        if hasattr(o, "now"):
            o.now = lambda: T[0]
    with open(os.path.join(DIR, "admin-secret.txt"), "w") as f:
        f.write(SECRET + "\n")
    # the admin handler class
    import http.server as hs

    class Stub:
        allow_reuse_address = False

        def __init__(self, addr, handler):
            H[0] = handler

        def serve_forever(self):
            pass

    orig = hs.ThreadingHTTPServer
    hs.ThreadingHTTPServer = Stub
    try:
        d.admin_server()
    finally:
        hs.ThreadingHTTPServer = orig
    del cseed.LOGS[:]
    return d


def reset(d):
    d.MM.__init__()
    for x in (d.PRES.players, d.PRES.udp_seen, d.LOADED_T, d.SESS_LOST, d._SILENT, d.TEAM_SWAP, d.MATCH_MAP, d.MATCH_P2P, d.SID_KEEP, d.READOPT,
              d.RESTORED_T, d.PING, d.PING_AT, d.PING_LAST, d.PING_HIGH, d.PING_OUT, d.PING_NOTE_AT, d.PING_WAIT_SINCE, d.LOSS, d.LOSS_HIGH, d.LOSS_HOLD,
              d.LOSS_STRIKES, d.LOSS_FLAGS, d.LOSS_HOST_HIGH, d.CLAN_MATCH, d.CLAN_ENDED, d.CLAN_PENDING, d.CLAN_DUE, d.CLAN_KICKED, d.CLAN_REPORTED_END,
              d.MEASURED_PAIR, d.PING_CORR, d.PING_EXCESS, d.PING_LOC, d.LOC_FWD, d.LOC_HIST, d.LOC_SPIKES, d.REGION, d.REGION_CAND, d.JIP_LATE,
              d.MESH_ON, d.JIP_STARTED, d.JIP_REPUSH, d._JIP_LAST_WHY, d._JIP_WAIT_LOG, d.BOOST_PICK, d.BOOST_ACTIVE, d.CLAN_DONE, d.CLAN_EXCUSE_WHY,
              d.CLAN_LEFT_AT, d.CLAN_REJOINED, d.CLAN_LAST_DEPART, d.NAMELESS_BY_IP, d.VOTES, d.TIMING, d.FEED_SEEN, d.PING_FORMED):
        x.clear()
    for s in (d.LOADED, d.CD_GOT, d.AFK_KICKED, d.PING_KICKED, d.LOSS_KICKED, d.CLAN_QUITTERS, d.CLAN_CREDITED, d.CLAN_EXCUSED, d.END_BY_REPORTS,
              d.RESULT_BOOKED, d.ABORT_AWARDED, d._POOL_LOGGED):
        s.clear()
    for mid in list(d.HM.mids()):
        d.HM.end(mid)
    for t in list(d.PARTIES.of):
        d.PARTIES.leave(t)
    d.PARTIES.grace.clear()
    d._last_net[0] = 0.0
    d._gc_last[0] = 0.0
    del REC[:]
    MGS[0] = False
    p = os.path.join(DIR, "live-matches.json")
    if os.path.exists(p):
        os.remove(p)


def world_py(d, w):
    reset(d)
    WORLD.clear()
    WORLD.update({k: w.get(k) for k in ("names", "sids", "pings", "perf", "noh", "lim", "online", "helpers")})
    for k, dflt in (("names", {}), ("sids", {}), ("pings", {}), ("perf", {}), ("noh", []), ("lim", 175), ("online", []), ("helpers", [])):
        if WORLD.get(k) is None:
            WORLD[k] = dflt
    for t, (nm, last) in w.get("pres", {}).items():
        d.PRES.players[t] = {"name": nm, "last": last}
    m = w.get("mm") or {}
    with d.MM.lock:
        for t, q in m.get("queue", []):
            d.MM.queue[t] = q
        for mid, pl in m.get("matches", []):
            d.MM.matches[mid] = list(pl)
            d.MM.qtime[mid] = {}
        d.MM.formed.update(m.get("formed", {}))
        d.MM.launched.update(m.get("launched", []))
        d.MM.started.update(m.get("started", []))
        d.MM.ended.update(m.get("ended", []))
        d.MM.ended_at.update(m.get("ended_at", {}))
        d.MM.started_t.update(m.get("started_t", {}))
        for mid, dl in m.get("departed", {}).items():
            d.MM.departed[mid] = set(dl)
        d.MM.departed_vol.update(m.get("dep_vol", []))
        d.MM.departed_t.update(m.get("dep_t", {}))
        for mid, c in m.get("clock", {}).items():
            d.MM.clock[mid] = tuple(c)
        for mid, c in m.get("lives", {}).items():
            d.MM.lives[mid] = tuple(c)
        for t, mid, t0 in m.get("joining", []):
            d.MM.joining[t] = (mid, t0)
        for t, v in m.get("late_bonus", {}).items():
            d.MM.late_bonus[t] = (v[0], v[1])
        for t, v in m.get("late_mult", {}).items():
            d.MM.late_mult[t] = (v[0], v[1])
        d.MM.timed_out.update(m.get("timed_out", []))
    d.LOADED.update(w.get("loaded", []))
    d.LOADED_T.update(w.get("loaded_t", {}))
    for t, v in w.get("sess_lost", {}).items():
        d.SESS_LOST[t] = (v[0], v[1])
    for mid, sw in w.get("team_swap", {}).items():
        d.TEAM_SWAP[mid] = dict(sw)
    for pl, i in w.get("match_map", []):
        d.MATCH_MAP[frozenset(pl)] = i
    for pl, b in w.get("p2p", []):
        d.MATCH_P2P[frozenset(pl)] = b
    for pl, n in w.get("sid_keep", []):
        d.SID_KEEP[frozenset(pl)] = n
    for k, v in w.get("ping_at", {}).items():
        a, b = k.split("|")
        d.PING_AT[(a, b)] = v
    for a, b, ms, t in w.get("ping_last", []):
        d.PING_LAST[frozenset((a, b))] = (ms, t)
    for mid, pl in w.get("hm", []):
        d.HM.start(mid, list(pl))
    c = w.get("clan") or {}
    d.CLAN_MATCH.update(c.get("match", {}))
    d.CLAN_ENDED.update(c.get("ended", {}))
    for t, p in c.get("pending", {}).items():
        d.CLAN_PENDING[t] = (p[0], p[1], p[2], {int(k): v for k, v in p[3].items()})
    for t, m_ in c.get("quitters", []):
        d.CLAN_QUITTERS.add((t, m_))
    for t, m_ in c.get("credited", []):
        d.CLAN_CREDITED.add((t, m_))
    for t, m_ in c.get("excused", []):
        d.CLAN_EXCUSED.add((t, m_))
    for g in w.get("parties", []):
        for m_ in g[1:]:
            d.PARTIES.invite_token(g[0], m_)
            d.PARTIES.accept_token(m_, g[0])
    for t, x in w.get("loss_flags", {}).items():
        mid, tk = t.split("|")
        d.LOSS_FLAGS[(mid, tk)] = tuple(x)
    for t, x in w.get("ping_high", {}).items():
        a, b = t.split("|")
        d.PING_HIGH[(a, b)] = x
    for t, x in w.get("strikes", {}).items():
        d.LOSS_STRIKES[t] = list(x)
    g = w.get("regs") or {}
    import collections
    for t in g.get("tok", []):
        d.PING_LOC[t] = "ams=20"
        d.LOC_FWD[t] = T[0] - 5
        d.LOC_HIST[t] = collections.deque([5, 6])
        d.LOC_SPIKES[t] = 1
        d.REGION[t] = "eu"
        d.REGION_CAND[t] = ("eu", 1)
        d.JIP_LATE[t] = ("deadbeef", T[0] - 9)
        d.MESH_ON[t] = T[0] - 3
        d.JIP_STARTED[t] = ("deadbeef", T[0] - 9)
        d.JIP_REPUSH[t] = (T[0] - 1, 2)
        d._JIP_WAIT_LOG[t] = T[0] - 4
        d.BOOST_PICK[t] = 1
        d.BOOST_ACTIVE[t] = 2
        d.CD_GOT.add(t)
        d._SILENT[t] = T[0] - 50
        d.CLAN_DONE[t] = "mid"
        d._JIP_LAST_WHY[t] = "why"
    for i, t in enumerate(g.get("ip", [])):
        d.NAMELESS_BY_IP["10.0.0.%d" % i] = t
    for a, b in g.get("pairs", []):
        d.PING[(a, b)] = collections.deque([30, 40])
        d.PING_AT[(a, b)] = T[0] - 7
    for m in g.get("mid", []):
        d.CLAN_REPORTED_END[m] = {"x": T[0]}
        d.END_BY_REPORTS.add(m)
        d.RESULT_BOOKED.add(m)
        d.ABORT_AWARDED.add(m)
        d.CLAN_LAST_DEPART[m] = (T[0] - 500 if len(m) % 2 else T[0] - 5000, "tok")
        d._JIP_LAST_WHY[m] = "why"
    for t, m in g.get("tm", []):
        d.CLAN_EXCUSE_WHY[(t, m)] = "x"
        d.CLAN_LEFT_AT[(t, m)] = T[0] - 3
        d.CLAN_REJOINED[(t, m)] = T[0] - 3


def srt(x):
    return sorted(x)


def sdump(d):
    with d.MM.lock:
        mm = {"queue": [[k, v] for k, v in d.MM.queue.items()], "matches": [[k, v] for k, v in d.MM.matches.items()], "formed": dict(d.MM.formed),
              "launched": srt(d.MM.launched), "started": srt(d.MM.started), "ended": srt(d.MM.ended), "ended_at": dict(d.MM.ended_at),
              "started_t": dict(d.MM.started_t), "departed": {k: srt(v) for k, v in d.MM.departed.items()}, "dep_vol": srt(d.MM.departed_vol),
              "dep_t": dict(d.MM.departed_t), "clock": {k: list(v) for k, v in d.MM.clock.items()}, "lives": {k: list(v) for k, v in d.MM.lives.items()},
              "joining": [[k, v[0], v[1]] for k, v in d.MM.joining.items()], "late_bonus": {k: list(v) for k, v in d.MM.late_bonus.items()},
              "late_mult": {k: list(v) for k, v in d.MM.late_mult.items()}, "timed_out": srt(d.MM.timed_out), "qtime": {k: dict(v) for k, v in d.MM.qtime.items()},
              "seen": dict(d.MM.seen), "settle": dict(d.MM.settle)}
    mp = [[srt(k), v] for k, v in d.MATCH_MAP.items()]
    mp.sort()
    s = {"mm": mm, "loaded": srt(d.LOADED), "loaded_t": dict(d.LOADED_T), "sess_lost": {k: list(v) for k, v in d.SESS_LOST.items()}, "silent": dict(d._SILENT),
         "restored": dict(d.RESTORED_T), "readopt": srt(d.READOPT), "team_swap": {k: dict(v) for k, v in d.TEAM_SWAP.items()}, "match_map": mp,
         "p2p": sorted([[srt(k), v] for k, v in d.MATCH_P2P.items()]), "sid_keep": sorted([[srt(k), v] for k, v in d.SID_KEEP.items()]),
         "hm": srt(d.HM.mids()), "ping_high": sorted([[k[0], k[1], v] for k, v in d.PING_HIGH.items()]), "ping_kicked": sorted([list(k) for k in d.PING_KICKED]),
         "loss": sorted([[k[0], k[1], k[2], [list(x) for x in v]] for k, v in d.LOSS.items()]), "loss_high": sorted([[k[0], k[1], v] for k, v in d.LOSS_HIGH.items()]),
         "loss_kicked": sorted([list(k) for k in d.LOSS_KICKED]), "loss_strikes": {k: list(v) for k, v in d.LOSS_STRIKES.items()},
         "loss_hold": {k: list(v) for k, v in d.LOSS_HOLD.items()}, "loss_flags": sorted([[k[0], k[1], list(v)] for k, v in d.LOSS_FLAGS.items()]),
         "loss_host_high": {k: list(v) for k, v in d.LOSS_HOST_HIGH.items()}, "afk": sorted([list(k) for k in d.AFK_KICKED]),
         "clan_kicked": sorted([[k[0], k[1], v] for k, v in d.CLAN_KICKED.items()]), "clan_match": dict(d.CLAN_MATCH), "clan_ended": dict(d.CLAN_ENDED),
         "clan_pending": {k: [v[0], v[1], v[2], {str(a): b for a, b in v[3].items()}] for k, v in d.CLAN_PENDING.items()}, "clan_due": dict(d.CLAN_DUE),
         "quitters": sorted([list(k) for k in d.CLAN_QUITTERS]), "credited": sorted([list(k) for k in d.CLAN_CREDITED]), "excused": sorted([list(k) for k in d.CLAN_EXCUSED]),
         "ping_out": {k: list(v) for k, v in d.PING_OUT.items()}, "ping_note_at": dict(d.PING_NOTE_AT), "ping_wait": dict(d.PING_WAIT_SINCE),
         "regs": {n: sorted("|".join(k) if isinstance(k, tuple) else str(k) for k in o) for n, o in (
             ("PING_LOC", d.PING_LOC), ("LOC_FWD", d.LOC_FWD), ("LOC_HIST", d.LOC_HIST), ("LOC_SPIKES", d.LOC_SPIKES), ("REGION", d.REGION), ("REGION_CAND", d.REGION_CAND),
             ("JIP_LATE", d.JIP_LATE), ("MESH_ON", d.MESH_ON), ("JIP_STARTED", d.JIP_STARTED), ("JIP_REPUSH", d.JIP_REPUSH), ("JIP_WAIT_LOG", d._JIP_WAIT_LOG),
             ("BOOST_PICK", d.BOOST_PICK), ("BOOST_ACTIVE", d.BOOST_ACTIVE), ("CD_GOT", d.CD_GOT), ("CLAN_DONE", d.CLAN_DONE), ("JIP_LAST_WHY", d._JIP_LAST_WHY),
             ("FEED_SEEN", []), ("NAMELESS", d.NAMELESS_BY_IP), ("PING", d.PING), ("PING_AT", d.PING_AT), ("PING_LAST", [tuple(sorted(k)) for k in d.PING_LAST]),
             ("REPORTED_END", d.CLAN_REPORTED_END), ("END_BY_REPORTS", d.END_BY_REPORTS), ("RESULT_BOOKED", d.RESULT_BOOKED), ("ABORT_AWARDED", d.ABORT_AWARDED),
             ("LAST_DEPART", d.CLAN_LAST_DEPART), ("EXCUSE_WHY", d.CLAN_EXCUSE_WHY), ("LEFT_AT", d.CLAN_LEFT_AT), ("REJOINED", d.CLAN_REJOINED))},
         "pres": sorted(d.PRES.players), "parties": sorted(d.PARTIES.export()["of"]), "rec": REC[:]}
    del REC[:]
    return s


def norm_text(t):
    if t is None:
        return None
    import collections
    o = json.loads(t, object_pairs_hook=collections.OrderedDict)
    for m in o.get("matches", []):
        if isinstance(m.get("hm"), dict):
            m["hm"] = collections.OrderedDict(sorted(m["hm"].items()))
    for k in ("clan_quitters", "clan_credited", "clan_excused"):
        if k in o:
            o[k] = sorted(o[k])
    return {"head": t[:90], "text": json.dumps(o)}


def canon_live(x):
    for k in ("clan_quitters", "clan_credited", "clan_excused"):
        x[k] = sorted(x[k])
    return x


class FakeSock:
    def __init__(self, raw):
        self.raw = raw
        self.out = b""

    def settimeout(self, t):
        pass

    def makefile(self, mode, bufsize=-1):
        return io.BytesIO(self.raw)

    def sendall(self, b):
        self.out += bytes(b)

    def setsockopt(self, *a):
        pass


def http_py(raw):
    import http.server as hs

    class Shim2:
        def time(self):
            return T[0]

        def __getattr__(self, k):
            return getattr(_time, k)

    old = hs.time
    hs.time = Shim2()
    s = FakeSock(raw)
    try:
        H[0](s, ("127.0.0.1", 1), object())
    except Exception as e:  # noqa: BLE001
        s.out += b"##EXC " + repr(e).encode()
    finally:
        hs.time = old
    return s.out


def run():
    d = setup()
    n = 0
    for c in script():
        T[0] = c.get("t", T[0])
        op = c["op"]
        n += 1
        r = None
        if op == "world":
            world_py(d, c["w"])
        elif op == "dump":
            pass
        elif op == "mgs":
            MGS[0] = bool(c["v"])
        elif op == "live_export":
            r = canon_live(json.loads(json.dumps(d.live_export(c.get("now")))))
        elif op == "live_save":
            ok = d.live_save(c.get("now"))
            p = d.live_path()
            r = {"ok": ok, "text": norm_text(open(p).read() if os.path.exists(p) else None)}
        elif op == "live_load":
            r = d.live_load(c.get("now"))
        elif op == "file_copy":
            import shutil
            if os.path.exists(c["from"]):
                shutil.copyfile(c["from"], d.live_path())
        elif op == "file_save_as":
            import shutil
            if os.path.exists(d.live_path()):
                shutil.copyfile(d.live_path(), c["to"])
        elif op == "reset_runtime":
            keep = {"parties": {k: v for k, v in d.PARTIES.of.items()}}
            w = {"names": WORLD["names"], "sids": WORLD["sids"], "pings": WORLD["pings"], "online": WORLD["online"]}
            p = os.path.join(DIR, "live-matches.json")
            data = open(p).read() if os.path.exists(p) else None
            world_py(d, w)
            if data is not None:
                open(p, "w").write(data)
        elif op == "claim":
            r = d.readopt_claim(c["tok"], c.get("now"))
        elif op == "claim_all":
            r = [d.readopt_claim(x) for x in c["ts"]]
        elif op == "roundtrip":
            ok = d.live_save(None)
            p = d.live_path()
            text = open(p).read() if os.path.exists(p) else None
            w = {"names": WORLD["names"], "sids": WORLD["sids"], "pings": WORLD["pings"], "online": WORLD["online"]}
            data = text
            world_py(d, w)
            if data is not None:
                open(p, "w").write(data)
            mids = d.live_load(None)
            r = {"ok": ok, "text": norm_text(text), "mids": mids, "claims": [d.readopt_claim(x) for x in c.get("ts", [])]}
        elif op == "expire":
            r = d.readopt_expire(c.get("now"))
        elif op == "reap":
            r = [list(x) for x in d.reap_matches(c.get("now"))]
        elif op == "net_report":
            d.net_report(c.get("now"), c.get("every", 60.0))
        elif op == "snapshot":
            r = json.loads(json.dumps(d.live_snapshot()))
        elif op == "status":
            live = d.live_snapshot()
            r = json.loads(json.dumps({"ts": 0, "server_version": d.SERVER_VERSION, "players_online": d.PRES.count(), "queue": len(d.MM.queue), "matches": len(d.MM.matches),
                                       "live": live, "roster": d.roster_public(live), "notices": d.notices_public(), "target_version": "-"}, default=str))
        elif op == "roster":
            r = d.roster_public(c["snap"])
        elif op == "gc":
            r = d.gc_state(c.get("now"), c.get("ttl"))
        elif op == "gc_tick":
            r = d.gc_tick(c.get("now"))
        elif op == "ping_gate":
            r = d._ping_gate(c["a"], c["b"], T[0])
        elif op == "ping_fresh":
            r = d._ping_fresh(c["a"], c["b"], T[0])
        elif op == "ping_wait":
            r = d.ping_wait_judge(c["players"], c.get("host"), lambda a, b: d.pair_ping(a, b), T[0],
                                  units_fn=(lambda t: c.get("units", {}).get(t, [])) if c.get("units") is not None else None,
                                  helper=lambda t: t in c.get("helpers", []), wait_s=c.get("wait_s"),
                                  host_ok=(lambda t: t not in c.get("nohost", [])) if c.get("nohost") is not None else None)
            r = [r[0], sorted(r[1])]
        elif op == "left_out":
            sent = []
            r = sorted(d.ping_left_out_note(c["dropped"], T[0], note=lambda t, n: sent.append([t, n])))
            r = [r, sent]
        elif op == "jip_block":
            r = d.jip_ping_block(c["tok"], c["mid"], c["host"], c["active"], set(c.get("joining", [])), T[0])
        elif op == "ping_check":
            r = d.ping_check(c.get("now"))
        elif op == "pres_touch":
            for k in c["toks"]:
                d.PRES.players[k] = {"name": WORLD["names"].get(k) or "p", "last": T[0] - c.get("ago", 0.0)}
        elif op == "ping_touch":
            for k in c["pairs"]:
                a, b = k.split("|")
                d.PING_AT[(a, b)] = T[0]
        elif op == "loss_note":
            d.loss_note(c["tok"], d.parse_loss(bytes.fromhex(c["pl"])), c.get("now"))
        elif op == "parse_loss":
            x = d.parse_loss(bytes.fromhex(c["pl"]))
            r = None if x is None else {"paced": x["paced"], "hbw": x["hbw"], "peers": [list(p) for p in x["peers"]]}
        elif op == "roster_key":
            r = d._roster_key(c["name"])
        elif op == "loss_check":
            r = d.loss_check(c.get("now"))
        elif op == "loss_hold_note":
            r = d.loss_hold_note(c["tok"], c.get("now"))
            r = None if r is None else bytes(r).hex()
        elif op == "kick":
            r = d.kick_token(c["tok"], c.get("why", "admin kick"), c.get("ktxt", "kicked"), c.get("kind", "admin-kick"), c.get("reason", ""),
                             c.get("keep_out", False), c.get("no_penalty", False))
        elif op == "afk":
            r = d.afk_report(c["tok"], c["idle"], c.get("flags", 0))
        elif op == "admin_kick":
            r = d.admin_kick(c["pid"], c.get("reason", ""), c.get("move", False))
        elif op == "reason_clean":
            r = d.kick_reason_clean(c["s"])
        elif op == "reset_session":
            r = d.admin_reset_session(c["pid"])
        elif op == "secret_new":
            p = os.path.join(DIR, "admin-secret.txt")
            if os.path.exists(p):
                os.remove(p)
            if c.get("short"):
                open(p, "w").write("short\n")
            r = d.admin_secret()
            open(p, "w").write(SECRET + "\n")
        elif op == "http":
            r = http_py(bytes.fromhex(c["raw"])).hex()
        else:
            raise SystemExit("unknown op " + op)
        s = sdump(d)
        emit(n, r, s)


def hp(path, secret=True, method="GET", body=None, extra="", ver="HTTP/1.0"):
    h = "%s %s %s\r\n" % (method, path, ver)
    if secret is True:
        h += "X-Admin-Secret: %s\r\n" % SECRET
    elif secret:
        h += "X-Admin-Secret: %s\r\n" % secret
    b = b""
    if body is not None:
        b = body if isinstance(body, bytes) else json.dumps(body).encode()
        h += "Content-Length: %d\r\n" % len(b)
    h += extra + "\r\n"
    return (h.encode("latin-1") + b).hex()


W = [None]


def fnv(nm):
    b = nm.encode("latin1", "ignore")[:24].split(b"\0")[0]
    h = 2166136261
    for c in b:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h


def loss_batch(r, W, t, pick=None):
    w = W[0]
    ms = [m for m in w["mm"]["matches"] if m[0] in w["mm"]["started"]]
    if not ms:
        return [{"op": "parse_loss", "pl": "00" * r.randrange(60, 120), "t": t}]
    mid, pl = pick[:2] if pick else r.choice(ms)
    out = []
    lossy = pick[2] if pick else r.choice(pl)
    for rep in pl:
        if r.random() < 0.15:
            continue
        flags = r.choice([4, 4, 4, 5, 6, 7])
        body = bytearray(71) + bytes([flags, 1]) + bytes(6)
        if flags & 2:
            body += bytes(7)
        peers = [p for p in pl if p != rep and r.random() < 0.9]
        fl = (1 if flags & 1 and r.random() < 0.3 else 0) | (2 if r.random() < 0.05 else 0)
        body += bytes([1, fl, len(peers)])
        for i, p in enumerate(peers):
            nm = w["names"].get(p, "")
            rx = r.randrange(250, 1500)
            lo = r.randrange(0, 40) if p != lossy else r.randrange(rx // 2, rx + 200)
            body += bytes([i]) + fnv(nm).to_bytes(4, "little") + min(rx, 65535).to_bytes(2, "little") + min(lo, 65535).to_bytes(2, "little")
        out.append({"op": "loss_note", "tok": rep, "pl": bytes(body).hex(), "t": t})
    out.append({"op": "loss_check", "t": t})
    return out


def scenario(r, W, t):
    """Rounds of ping / loss reports inside one started match, 20-40 s apart, so the 60 s ping and 120 s loss timers run out."""
    w = W[0]
    ms = [m for m in w["mm"]["matches"] if m[0] in w["mm"]["started"] and m[0] in w["mm"]["launched"]]
    out = []
    if not ms:
        return out, t
    mid, pl = r.choice(ms)
    pairs = ["%s|%s" % (a, b) for a in pl for b in pl if a != b and "%s|%s" % (a, b) in w["pings"]]
    lossy = r.choice(pl)
    for _ in range(r.choice([4, 6, 9, 12])):
        t += r.choice([15.0, 25.0, 40.0])
        out.append({"op": "ping_touch", "pairs": pairs, "t": t})
        out.append({"op": "ping_check", "t": t})
        if r.random() < 0.8:
            out.extend(loss_batch(r, W, t, (mid, pl, lossy)))
        if r.random() < 0.2:
            out.append({"op": "reap", "t": t})
    return out, t


def gen(r, n):
    toks = [tok(i) for i in range(14)]
    names = ["Alpha", "bravo", "Charlie", "delta force", "Echo", "Foxtrot", "golf", "Hotel", "india", "Juliet", "kilo", "Lima", "Mike", "Nov"]
    out = []
    t = 5000.0

    def mk_world():
        nonlocal t
        t += r.choice([100.0, 500.0, 2000.0])
        k = r.choice([6, 8, 10, 14])
        mood = r.random() < 0.6
        rich = r.random() < 0.35
        mood = mood or rich
        ts = toks[:k]
        nm = {x: names[i] for i, x in enumerate(ts) if r.random() < 0.9}
        sids = {x: r.choice([0, 76561198000000000 + r.randrange(1000)]) for x in ts}
        pres = {}
        for x in ts:
            if r.random() < 0.9:
                pres[x] = [nm.get(x, "p%s" % x[:2]), t - r.choice([1.0, 3.0, 8.0, 14.0] if mood else [1.0, 3.0, 8.0, 14.0, 30.0, 70.0, 130.0, 300.0])]
        mm = {"queue": [], "matches": [], "formed": {}, "launched": [], "started": [], "ended": [], "ended_at": {}, "started_t": {}, "departed": {}, "dep_vol": [],
              "dep_t": {}, "clock": {}, "lives": {}, "joining": [], "late_bonus": {}, "late_mult": {}, "timed_out": []}
        pool = ts[:]
        r.shuffle(pool)
        hm = []
        loaded, loaded_t, sess_lost, team_swap, mmap, p2p, sidk, ping_at, clan = [], {}, {}, {}, [], [], [], {}, {"match": {}, "ended": {}, "pending": {}, "quitters": [], "credited": [], "excused": []}
        mids = []
        for mi in range(r.choice([0, 1, 2, 2, 3])):
            sz = r.choice([2, 3, 4, 4, 6])
            if len(pool) < sz:
                break
            pl = [pool.pop() for _ in range(sz)]
            mid = "%08x" % r.getrandbits(32)
            mids.append(mid)
            mm["matches"].append([mid, pl])
            age = r.choice([130.0, 200.0, 400.0]) if rich else r.choice([5.0, 20.0, 60.0, 130.0, 400.0, 2000.0, 15000.0])
            mm["formed"][mid] = t - age
            st = r.choice([0.1, 0.8]) if rich else r.random()
            if st < 0.75:
                mm["launched"].append(mid)
                mm["started"].append(mid)
                mm["started_t"][mid] = t - age + r.choice([5.0, 20.0, 40.0])
            elif st < 0.9:
                mm["launched"].append(mid)
            if r.random() < 0.12 and mid in mm["started"]:
                mm["ended"].append(mid)
                mm["ended_at"][mid] = t - r.choice([5.0, 20.0, 50.0])
            if r.random() < 0.3:
                dp = r.sample(pl, 1)
                mm["departed"][mid] = dp
                if r.random() < 0.5:
                    mm["dep_vol"].append(dp[0])
                mm["dep_t"][dp[0]] = t - r.choice([10.0, 60.0])
            if r.random() < 0.6 and mid in mm["started"]:
                mm["clock"][mid] = [r.choice([100.0, 300.5, 590.0]), 600.0, r.random() < 0.1, t - r.choice([1.0, 9.0, 40.0])]
                mm["lives"][mid] = [r.randrange(0, 30), r.randrange(0, 30), t - r.choice([1.0, 12.0])]
            for x in pl:
                if r.random() < (0.3 if rich else 0.7):
                    loaded.append(x)
                    loaded_t[x] = t - r.choice([5.0, 30.0, 100.0])
                if r.random() < 0.15:
                    sess_lost[x] = [t - r.choice([5.0, 30.0, 80.0]), mid if r.random() < 0.85 else "ffffffff"]
                if r.random() < 0.25 and mid in mm["started"]:
                    mm["late_bonus"][x] = [mid, r.randrange(1, 5)]
                if r.random() < 0.2 and mid in mm["started"]:
                    mm["late_mult"][x] = [mid, r.choice([1.25, 0.5, 2.0])]
                if r.random() < 0.2:
                    clan["match"][x] = mid
            if r.random() < 0.3 and mid in mm["started"]:
                x = r.choice(pl)
                mm["joining"].append([x, mid, t - r.choice([3.0, 20.0, 50.0, 130.0, 300.0])])
            if r.random() < 0.4:
                team_swap[mid] = {x: r.randrange(2) for x in sorted(r.sample(pl, 1 + r.randrange(2)))}
            key = sorted(pl)
            if r.random() < 0.8:
                mmap.append([key, r.randrange(0, 12)])
            if r.random() < 0.5:
                p2p.append([key, r.random() < 0.5])
            if r.random() < 0.5:
                sidk.append([key, r.getrandbits(31)])
            if r.random() < 0.8:
                hm.append([mid, pl])
            for a in pl:
                for b in pl:
                    if a != b and r.random() < 0.7:
                        ping_at["%s|%s" % (a, b)] = t - r.choice([2.0, 30.0, 80.0, 200.0, 5000.0])
            if r.random() < 0.4:
                clan["ended"][mid] = t - r.choice([30.0, 4000.0])
            if r.random() < 0.3:
                x = r.choice(pl)
                clan["pending"][x] = [mid, r.randrange(1, 8), r.randrange(0, 4), {str(i): r.randrange(0, 40) for i in range(r.randrange(0, 3))}]
            if r.random() < 0.3:
                clan["quitters"].append([r.choice(pl), mid])
            if r.random() < 0.3:
                clan["credited"].append([r.choice(pl), mid])
        for x in pool[:r.randrange(0, 4)]:
            mm["queue"].append([x, t - r.choice([2.0, 15.0, 90.0])])
        if r.random() < 0.3:
            clan["ended"]["deadbeef"] = t - r.choice([30.0, 9000.0])
        clan["quitters"].append([r.choice(ts), "oldmatch"])
        clan["pending"] = dict(sorted(clan["pending"].items()))
        pings = {}
        for a in ts:
            for b in ts:
                if a != b and r.random() < 0.8:
                    pings["%s|%s" % (a, b)] = r.choice([20, 45, 90, 140, 170, 185, 200, 230, 280])
        ping_last = []
        for _ in range(r.randrange(0, 5)):
            a, b = r.sample(ts, 2)
            ping_last.append([a, b, r.choice([None, 60, 210, 250]), t - r.choice([10.0, 500.0, 9000.0])])
        parties = []
        if r.random() < 0.4 and len(pool) >= 2:
            parties = [pool[:2]]
        regs = {"tok": [x for x in ts if r.random() < 0.6], "ip": [x for x in ts if r.random() < 0.3], "pairs": [r.sample(ts, 2) for _ in range(r.randrange(0, 6))],
                "mid": [m for m in mids if r.random() < 0.5] + ["%08x" % r.getrandbits(32) for _ in range(r.randrange(0, 3))],
                "tm": [[r.choice(ts), r.choice(mids + ["%08x" % r.getrandbits(32)])] for _ in range(r.randrange(0, 5))]}
        w = {"regs": regs, "names": nm, "sids": sids, "pings": pings, "perf": {x: r.choice([0, 0, 10, 40]) for x in ts if r.random() < 0.3}, "noh": [x for x in ts if r.random() < 0.1],
             "lim": r.choice([150, 175, 190]), "online": [x for x in ts if x in pres], "helpers": [x for x in ts if r.random() < 0.6], "pres": pres, "mm": mm,
             "loaded": sorted(set(loaded)), "loaded_t": loaded_t, "sess_lost": sess_lost, "team_swap": team_swap, "match_map": mmap, "p2p": p2p, "sid_keep": sidk,
             "ping_at": ping_at, "ping_last": ping_last, "hm": hm, "clan": clan, "parties": parties}
        return w, ts, mids, t

    w, ts, mids, tw = mk_world()
    W[0] = w
    out.append({"op": "world", "w": w, "t": tw})
    t = tw
    while len(out) < n:
        x = r.random()
        t += r.choice([0.0, 1.0, 5.0, 10.0, 10.0, 30.0, 61.0, 125.0])
        base = {"t": t}
        k = r.choice(ts)
        if x < 0.03:
            w, ts, mids, t = mk_world()
            W[0] = w
            out.append({"op": "world", "w": w, "t": t})
        elif x < 0.09 and not os.environ.get("K6_NOSCN"):
            o, t = scenario(r, W, t)
            out.extend(o)
        elif x < 0.12:
            out.append(dict(base, op="live_export", now=t if r.random() < 0.5 else None))
        elif x < 0.16:
            out.append(dict(base, op="roundtrip"))
        elif x < 0.24:
            if r.random() < 0.8:
                out.append(dict(base, op="pres_touch", toks=[q for q in ts if r.random() < 0.9], ago=r.choice([0.0, 0.0, 2.0, 20.0, 70.0])))
            out.append(dict(base, op="reap"))
        elif x < 0.28:
            out.append(dict(base, op="net_report", every=r.choice([0.0, 60.0])))
        elif x < 0.34:
            out.append(dict(base, op=r.choice(["snapshot", "status"])))
        elif x < 0.40:
            out.append(dict(base, op=r.choice(["gc", "gc_tick"]), ttl=r.choice([None, None, 60.0, 600.0, 100000.0])))
        elif x < 0.46:
            a, b = r.sample(ts, 2)
            out.append(dict(base, op=r.choice(["ping_gate", "ping_fresh"]), a=a, b=b))
        elif x < 0.50:
            pl = r.sample(ts, min(len(ts), r.randrange(2, 7)))
            out.append(dict(base, op="ping_wait", players=pl, host=r.choice([None, pl[0]]), wait_s=r.choice([None, 0.0, 10.0]),
                            units=({x_: [y for y in pl if r.random() < 0.3] for x_ in pl} if r.random() < 0.5 else None),
                            helpers=[x_ for x_ in pl if r.random() < 0.8], nohost=([x_ for x_ in pl if r.random() < 0.2] if r.random() < 0.5 else None)))
        elif x < 0.53:
            out.append(dict(base, op="left_out", dropped=r.sample(ts, r.randrange(0, 4))))
        elif x < 0.57 and mids:
            mid = r.choice(mids)
            out.append(dict(base, op="jip_block", tok=k, mid=mid, host=r.choice(ts), active=r.sample(ts, 3), joining=r.sample(ts, 1)))
        elif x < 0.63:
            out.append(dict(base, op="ping_check"))
        elif x < 0.72:
            out.extend(loss_batch(r, W, t))
        elif x < 0.76:
            out.append(dict(base, op="loss_check"))
        elif x < 0.79:
            out.append(dict(base, op="kick", tok=k, why=r.choice(["admin kick", "team move"]), ktxt="kicked: bye", kind="admin-kick", reason="x  y",
                            keep_out=r.random() < 0.5, no_penalty=r.random() < 0.5))
        elif x < 0.82:
            out.append(dict(base, op="afk", tok=k, idle=r.choice([10, 89, 90, 200, 0xFFFF]), flags=r.randrange(0, 4)))
        elif x < 0.85:
            pid = hashlib.sha256(k.encode()).hexdigest()[:r.choice([4, 12, 64])]
            out.append(dict(base, op="admin_kick", pid=r.choice([pid, pid, "zz", "ab", pid.upper(), 7]), reason=r.choice(["", "a \"quoted\"  reason\n", "x" * 200]), move=r.random() < 0.3))
        elif x < 0.87:
            out.append(dict(base, op="reason_clean", s=r.choice(["  a  b\t\n c ", "x\x01y\x7fz", " nbsp ", "é" * 130, ""])))
        elif x < 0.88:
            out.append(dict(base, op="reset_session", pid=r.choice(["abcd", "zz", "ab", 5, "0" * 12])))
        elif x < 0.89:
            out.append(dict(base, op="secret_new", short=r.random() < 0.5))
        elif x < 0.90:
            out.append(dict(base, op="mgs", v=r.random() < 0.5))
        elif x < 0.93:
            out.append(dict(base, op="claim_all", ts=ts))
        elif x < 0.94:
            out.append(dict(base, op="expire"))
        else:
            out.append(dict(base, op="http", raw=r.choice(HTTP_CASES(r, k, ts))))
    return out


def HTTP_CASES(r, k, ts):
    pid = hashlib.sha256(k.encode()).hexdigest()[:12]
    c = [hp("/status", secret=False), hp("/status"), hp("/live"), hp("/live", secret="wrong"), hp("/live", secret=False), hp("/notices"), hp("/teams"), hp("/nope"),
         hp("/clan/backups?steamid=76561198000000001"), hp("/clan/backups?steamid=x"), hp("/status?x=1", secret=False), hp("//status", secret=False),
         hp("/live", method="HEAD"), hp("/live", method="PUT"), hp("/kick", method="POST", body={"pid": pid, "reason": "bye"}),
         hp("/kick", method="POST", body={"pid": "zz"}), hp("/kick", method="POST", body=[1, 2]), hp("/kick", method="POST", body=b"{bad"),
         hp("/kick", method="POST", body={}), hp("/nope", method="POST", body={}), hp("/nope", method="POST", body=[]), hp("/nope", method="POST", body=b"nope"),
         hp("/reset-session", method="POST", body={"pid": pid}), hp("/shutdown/cancel", method="POST", body=[]),
         hp("/handover", method="POST", body={}), hp("/handover", method="POST", body={"mid": "00000000", "slot": 1}),
         hp("/handover", method="POST", body={"mid": "00000000", "slot": "x"}), hp("/teams/move", method="POST", body={"pid": pid}),
         hp("/teams/move", method="POST", body={"pid": None}), hp("/notice/clear", method="POST", body={"id": 99999}), hp("/notice/clear", method="POST", body={"id": None}),
         hp("/notice", method="POST", body={"text": ""}), 
         hp("/status", ver="HTTP/1.1", secret=False), hp("/status", ver="HTTP/2.0", secret=False), hp("/status", ver="HTTX/1.0", secret=False),
         hp("/live", extra="Connection: keep-alive\r\n"), hp("/live", secret="é"), hp("/live", extra="X-Foo: bar\r\n  folded\r\n"),
         "".encode().hex(), "0d0a", "474554".encode().hex() if False else b"GET\r\n\r\n".hex(), b"GET /status\r\n\r\n".hex(), b"POST /status\r\n\r\n".hex(),
         b"GET /status HTTP/1.0 extra\r\n\r\n".hex(), b"GET\t/status\tHTTP/1.0\r\n\r\n".hex(), (b"GET /" + b"a" * 70000 + b" HTTP/1.0\r\n\r\n").hex(),
         (b"GET /live HTTP/1.0\r\n" + b"".join(b"X-%d: y\r\n" % i for i in range(120)) + b"\r\n").hex(), (b"GET /live HTTP/1.0\r\nX-A: " + b"b" * 70000 + b"\r\n\r\n").hex(),
         hp("/shutdown", method="POST", body={"minutes": 3, "reason": "test", "restart": False}), hp("/shutdown", method="POST", body={"minutes": "x"}),
         hp("/notice", method="POST", body={"text": "hello", "minutes": 5}), hp("/notice", method="POST", body={"text": "hello", "seconds": "x"}),
         hp("/boot-all", method="POST", body={}), hp("/teams", method="POST", body={"rules": "a=b"}), hp("/teams", method="POST", body={"rules": 5}),
         hp("/live", method="POST", body={}), hp("/kick", method="POST", body=b"", extra=""), hp("/kick", method="POST", body={"pid": pid}, extra="X-Y: 1\r\n")]
    return c


if __name__ == "__main__":
    main(gen, run)
