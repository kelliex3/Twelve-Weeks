"""Differential test of daemon.py's K3 chunk (presence / bridges / sessions, versions + languages + server texts, maps / pools / vote / Part I
preference, match record + roster bytes + session ids + teams, match start + countdown, ping locations / pair pings / host choice / P2P push)
against `t2r-daemon selftest k3`.

  python3 k3_harness.py gen SEED N > script.jsonl
  T2R_DB=/tmp/k3py.db T2R_TOOLS_DIR=/tmp/k3tools python3 k3_harness.py run < script.jsonl > out.py.jsonl
  T2R_DB=/tmp/k3rs.db T2R_TOOLS_DIR=/tmp/k3tools t2r-daemon selftest k3 < script.jsonl > out.rs.jsonl ; cmp ...

daemon.py is imported from T2R_PY_DIR (no threads start at import). Time is frozen per line ("t"); `secrets.randbelow` / `token_hex` come from the one
seeded stream the Rust side uses; connections are fakes that record what was sent (Rust: loopback sockets read back after each call).
"""
import json
import os
import random
import secrets
import sys

os.environ.setdefault("T2R_HOST_CAP", "0")
os.environ.setdefault("T2R_DB", "/tmp/k3py.db")
os.environ.setdefault("T2R_TOOLS_DIR", "/tmp/k3tools")
os.environ["T2R_NO_COUNTDOWN"] = "1"
import cseed  # noqa: E402
from cseed import T, emit, log, main, script  # noqa: E402

secrets.randbelow = lambda n: cseed.RNG._randbelow(n)
import time as _time  # noqa: E402
_time.time = lambda: T[0]

DB = os.environ["T2R_DB"]
DIR = os.path.dirname(os.path.abspath(DB))
TOOLS = os.environ["T2R_TOOLS_DIR"]
NAMES = ["Alpha", "bravo", "Charlie Brown", "Charlie", "delta force", "Echo", "Foxtrot", "golf", "Hotel", "india", "Juliet", "kilo", "Zoe-1.x", "Ünï"]
LEVELS = ["to1-forest", "to1-gas-station", "to1-school", "to1-shipyard", "to1-resort", "to1-seraphite-camp", "to1-jesse-houses", "to1-custom-map-1",
          "to1-train-station", "to1-downtown", "to1-p1-bookstore", "to1-p1-plaza", "to1-hardware", "to1-jackson-market", "to1-overpass"]
LOCS = ["ams=20+fra=25,lhr=40", "iad=18 atl=22 ord=30", "sea=12,lax=30,sfo=35", "sgp=40 hkg=60", "gru=30 scl=60", "fra=10 waw=14 ams=19",
        "ord=300 iad=310", "ams=900", "iad=15,xxx=3", "nothing here", "lhr=5"]
CONNS = {}


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
    d.log = log
    d.evt = lambda kind, *a: log("EVT", kind, *a)
    d.SERVER_VERSION = "sv-test"
    d._ping_gate = lambda a, b, now=None: d.pair_ping(a, b)      # (K6 item: the Rust side has the same stand-in until it is ported)
    return d


def clear_all(d):
    import sqlite3
    CONNS.clear()
    for dd in (d.CLIENT_VERSIONS, d.CLIENT_LANGS, d.MATCH_MAP, d.VOTES, d.MATCH_P2P, d.SID_KEEP, d.TIMING, d.TEAM_SWAP, d.MATCH_P1, d.PING, d.PING_AT,
               d.PING_LOC, d.LOC_FWD, d.LOC_HIST, d.LOC_SPIKES, d.REGION, d.REGION_CAND, d.PING_LAST, d.MEASURED_PAIR, d.PING_EXCESS, d.PING_CORR,
               d.PING_FORMED, d.MESH_ON, d.SESS_LOST, d.NAMELESS_BY_IP, d.PEERS, d.VOTE_SHOWN, d._JIP_HV, d.BRIDGES.by_token, d.BRIDGES.by_sid,
               d.SESSIONS.by_token, d.PRES.players, d.PRES.udp_seen, d.LOADED_T):
        dd.clear()
    d.LINUX_CLIENTS.clear()
    d.LOADED.clear()
    d.CD_GOT.clear()
    d._POOL_LOGGED.clear()
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
        for t in ("players", "names"):
            c.execute("DELETE FROM %s" % t)
    d._name_cache.clear()
    d._steam_cache.clear()
    d._tok_gen.clear()
    d._name_gen[0] = 0
    d._name_ver[0] = 0


def hx(b):
    return None if b is None else bytes(b).hex()


def unhx(s):
    return bytes.fromhex(s)


def state(d):
    s = {}
    s["cv"] = list(d.CLIENT_VERSIONS.items())
    s["lc"] = sorted(d.LINUX_CLIENTS)
    s["cl"] = list(d.CLIENT_LANGS.items())
    s["due"] = d._save_due[0]
    s["mm"] = sorted([sorted(k), v] for k, v in d.MATCH_MAP.items())
    s["votes"] = sorted([sorted(k), v["opts"], list(v["votes"].items()), v["winner"]] for k, v in d.VOTES.items())
    s["p2p"] = sorted([sorted(k), v] for k, v in d.MATCH_P2P.items())
    s["sid"] = sorted([sorted(k), v] for k, v in d.SID_KEEP.items())
    s["rot"] = d._rotation[0]
    s["vr"] = list(d.VOTE_RECENT)
    s["vs"] = sorted(d.VOTE_SHOWN.items())
    s["tm"] = sorted([sorted(k), t["start"], list(t["marks"].items()), list(t["loaded"].items())] for k, t in d.TIMING.items())
    s["pl"] = len(d._POOL_LOGGED)
    s["pres"] = [[t, p.get("name"), p.get("last")] for t, p in d.PRES.players.items()]
    s["udp"] = len(d.PRES.udp_seen)
    s["br"] = [[t, b["steamid"], b["port"], b["ip"], b["name"], b["conn"].cid] for t, b in d.BRIDGES.by_token.items()]
    s["bsid"] = sorted(d.BRIDGES.by_sid.items())
    s["sess"] = [[t, c.cid] for t, c in d.SESSIONS.by_token.items()]
    s["lost"] = sorted(d.SESS_LOST.items())
    s["ploc"] = list(d.PING_LOC.items())
    s["ping"] = [[list(k), list(v)] for k, v in d.PING.items()]
    s["pat"] = sorted([list(k), v] for k, v in d.PING_AT.items())
    s["plast"] = sorted([sorted(k), list(v)] for k, v in d.PING_LAST.items())
    s["reg"] = sorted(d.REGION.items())
    s["rc"] = sorted([k, list(v)] for k, v in d.REGION_CAND.items())
    s["lh"] = sorted([k, list(v)] for k, v in d.LOC_HIST.items())
    s["ls"] = sorted(d.LOC_SPIKES.items())
    s["lf"] = sorted(d.LOC_FWD.items())
    s["meas"] = [[list(k), list(v)] for k, v in d.MEASURED_PAIR.items()]
    s["exc"] = [[k, [list(x) for x in v]] for k, v in d.PING_EXCESS.items()]
    s["corr"] = sorted([k, list(v)] for k, v in d.PING_CORR.items())
    s["formed"] = [[k, sorted([sorted(p), v] for p, v in m.items())] for k, m in d.PING_FORMED.items()]
    s["mesh"] = sorted(d.MESH_ON.items())
    s["nl"] = sorted(d.NAMELESS_BY_IP.items())
    s["peers"] = [[k, list(v)] for k, v in d.PEERS.items()]
    s["loaded"] = sorted(d.LOADED)
    s["cdgot"] = sorted(d.CD_GOT)
    s["jhv"] = len(d._JIP_HV)
    s["perf"] = [len(d.PERF.by_token), len(d.PERF.by_steam)]
    files = {}
    for f in ("client_versions.json", "linux_clients.json", "client_langs.json"):
        p = os.path.join(DIR, f)
        files[f] = open(p, encoding="utf-8").read() if os.path.exists(p) else None
    s["files"] = files
    sent = {}
    for cid in sorted(CONNS):
        c = CONNS[cid]
        if c.buf:
            sent[cid] = c.buf.hex()
            c.buf = b""
    s["sent"] = sent
    return s


def jset(o):
    return sorted(o) if isinstance(o, (set, frozenset)) else o


def do_call(d, f, a):
    if f == "seq":
        return [do_call(d, x["f"], x["a"]) for x in a[0]]
    if f == "version_op":
        return hx(d.version_op(unhx(a[0]), tuple(a[1]) if a[1] else None))
    if f == "parse_version_request":
        r = d.parse_version_request(unhx(a[0]))
        return None if r is None else list(r)
    if f == "note_lang":
        return d.note_lang(a[0], a[1])
    if f == "lang_of":
        return d.lang_of(a[0])
    if f == "norm_lang":
        x = a[1]
        if a[0] == "bytes":
            x = unhx(x)
        elif a[0] == "none":
            x = None
        return d.norm_lang(x)
    if f == "srv_text":
        return d.srv_text(a[0], a[1], **a[2])
    if f == "srv_note":
        return hx(d.srv_note(a[0], a[1], **a[2]))
    if f == "version_problem":
        return d.version_problem(a[0])
    if f == "version_refusal":
        return d.version_refusal(a[0])
    if f == "save_tables":
        return d._save_tables_if_due(a[0])
    if f == "save_soon":
        return d._save_soon()
    if f == "map_pool":
        return d.map_pool(a[0], a[1])
    if f == "map_index":
        return d.map_index(a[0])
    if f == "vote_options":
        return d.vote_options(players=a[0])
    if f == "vote_start":
        return d.vote_start(a[0])
    if f == "vote_cast":
        return d.vote_cast(a[0], a[1])
    if f == "vote_close":
        return d.vote_close(a[0])
    if f == "vote_state_text":
        return d.vote_state_text(a[0])
    if f == "vote_op":
        return hx(d.vote_op(a[0], unhx(a[1])))
    if f == "sp_support_op":
        return hx(d.sp_support_op(unhx(a[0])))
    if f == "map_packs_op":
        return hx(d.map_packs_op(unhx(a[0])))
    if f == "p1":
        return [d.p1_pref_on(), d.p1_always(a[0], a[1]), d.p1_owner(a[0], a[1]), d.p1_fits(a[0], a[2], a[1]), d.p1_match(a[3])]
    if f == "maps_misc":
        return [sorted(d.max_team_table().items()), sorted(d.sp_maps_on()), d.team_size(a[0]), list(d.flow_stage_secs(a[1])), d.flow_extra_secs(),
                [list(x) for x in d.countdown_steps(a[2], a[3], a[4], a[5])], d.countdown_seconds(a[6], a[7]), d.map_vote_on(), d.match_flow_on(),
                [m[2] for m in d.MAPS], [d.RESPAWNS.get(m[2], 0) for m in d.MAPS]]
    if f == "match_sid":
        return d.match_sid(a[0])
    if f == "teams":
        return [d.roster_team(a[0]), d.team_in(a[1], a[0], a[2]), d.roster_team_in(a[1], a[0], a[2]), d.team_of(a[0])]
    if f == "build_player_record":
        relay = tuple(a[4]) if a[4] else None
        return hx(d.build_player_record(a[0], a[1], a[2], [tuple(x) for x in a[3]], relay))
    if f == "build_peer_table":
        return hx(d.build_peer_table(a[0]))
    if f == "match_start_frame":
        return hx(d.match_start_frame(a[0], a[1]))
    if f == "stage_name":
        return d.stage_name(a[0], a[1])
    if f == "timing_mark":
        t = d.timing_mark(a[0], a[1])
        return t is not None
    if f == "timing_summary":
        return d.timing_summary(a[0])
    if f == "rekey":
        old, new = frozenset(a[0]), frozenset(a[1])
        for dd in (d.MATCH_MAP, d.MATCH_P2P, d.VOTES, d.TIMING):
            if old in dd and old != new:
                dd[new] = dd.pop(old)
        d.SID_KEEP[new] = a[2]
        return None
    if f == "forget":
        key = frozenset(a[0])
        with d._vote_lock:
            d.VOTES.pop(key, None)
        d.MATCH_MAP.pop(key, None)
        d.TIMING.pop(key, None)
        d.MATCH_P2P.pop(key, None)
        d.SID_KEEP.pop(key, None)
        return None
    if f == "start_match":
        return d.start_match(a[0])
    if f == "countdown":
        return d._countdown_run(a[0])
    if f == "ping_location":
        return d.ping_location(a[0], a[1])
    if f == "ping_report":
        return d.ping_report(a[0], a[1], a[2])
    if f == "pair_ping":
        return d.pair_ping(a[0], a[1], a[2], a[3])
    if f == "ping_kick_learn":
        return d.ping_kick_learn(a[0], a[1], a[2], a[3], a[4])
    if f == "mm_region":
        return d.mm_region(a[0])
    if f == "mesh_peers":
        r = d.mesh_peers(a[0])
        return None if r is None else sorted(r)
    if f == "mesh_activate":
        return d.mesh_activate(a[0])
    if f == "hm_start":
        d.HM.start(a[0], a[1])
        return None
    if f == "perf_op":
        return hx(d.perf_op(unhx(a[0])))
    if f == "perf_penalty":
        return [d.perf_penalty(a[0]), d.ping_match_ms()]
    if f == "best_host_order":
        return d.best_host_order(a[0])
    if f == "host_pool":
        r = d.host_pool(a[0])
        return [list(r[0]), r[1]]
    if f == "host_allowed":
        return [d.host_allowed(a[0]), [d.no_host(t) for t in a[0]]]
    if f == "push_peers":
        return d.push_peers(a[0])
    if f == "party_lead":
        return d.party_lead_searching(a[0])
    if f == "jip_hv":
        return list(d.jip_host_verdict(a[0], a[1]))
    if f == "hc":
        # host capacity rows
        d.HOSTCAP.host_report(a[0], a[1], a[2], a[3], a[4])
        return None
    if f == "pres":
        k = a[0]
        if k == "touch":
            d.PRES.touch(a[1], a[2])
        elif k == "known":
            d.PRES.touch_known(a[1])
        elif k == "drop":
            d.PRES.drop(a[1])
        elif k == "prune":
            return d.PRES.prune(a[1], set(a[2]))
        elif k == "ping":
            d.PRES.ping(tuple(a[1]))
        return [d.PRES.count(), [p["name"] for p in d.PRES.online()], d.PRES.name_of(a[1]) if len(a) > 1 and isinstance(a[1], str) else None]
    if f == "bridge":
        k = a[0]
        if k == "register":
            d.BRIDGES.register(a[1], conn(a[2]), a[3], a[4], a[5], a[6])
        elif k == "unregister":
            return d.BRIDGES.unregister(a[1], conn(a[2]) if a[2] else None)
        elif k == "q":
            t = a[1]
            return [d.BRIDGES.steamid(t), d.BRIDGES.port(t), (d.BRIDGES.conn(t).cid if d.BRIDGES.conn(t) else None), d.BRIDGES.tokens_for_ip(a[2]),
                    d.BRIDGES.name_for_ip(a[2]), sorted((t, v[1]) for t, v in d.BRIDGES.links().items())]
        return None
    if f == "session":
        k = a[0]
        if k == "register":
            d.SESSIONS.register(a[1], conn(a[2]))
        elif k == "unregister":
            d.SESSIONS.unregister(a[1], conn(a[2]) if a[2] else None)
        return [(d.SESSIONS.conn(a[1]).cid if d.SESSIONS.conn(a[1]) else None)]
    if f == "adopt":
        d.adopt_bridge_name(a[0], a[1])
        return list(d.no_helper_note(a[0], a[1]))
    if f == "lost":
        return d.note_session_lost(a[0], conn(a[1]))
    if f == "reattach":
        st = {"token": a[1], "name": a[2], "session": False, "authed": False}
        r = d.session_reattach(conn(a[0]), tuple(a[3]), st)
        return [r, st.get("session"), st.get("authed"), st.get("menu"), st.get("sent_name"), st.get("started")]
    if f == "peer_seen":
        return d.peer_seen(a[0], tuple(a[1]))
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
            for k, v in c.get("left", {}).items():
                m.left[k] = tuple(v)
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
            emit(n, None, state(d))
            continue
        if op == "setmap":
            d.TEAM_SWAP.clear()
            for k, v in c.get("swap", {}).items():
                d.TEAM_SWAP[k] = dict(v)
            d.MATCH_P1.clear()
            d.MATCH_P1.update(c.get("p1", {}))
            d.LOADED.clear()
            d.LOADED.update(c.get("loaded", []))
            d.CD_GOT.clear()
            d.CD_GOT.update(c.get("cdgot", []))
            for k, v in c.get("p2p", []):
                d.MATCH_P2P[frozenset(k)] = v
            for k, v in c.get("sidkeep", []):
                d.SID_KEEP[frozenset(k)] = v
            for k, v in c.get("timing", []):
                d.TIMING[frozenset(k)] = {"start": v, "marks": {}, "loaded": {}}
            emit(n, None, state(d))
            continue
        if op == "nohost":
            p = os.path.join(DIR, "no_host.txt")
            if c["text"] is None:
                if os.path.exists(p):
                    os.remove(p)
            else:
                with open(p, "w", encoding="utf-8") as f:
                    f.write(c["text"])
                os.utime(p, (c["mt"], c["mt"]))
            emit(n, None, None)
            continue
        f, a = c["f"], c.get("a", [])
        r = do_call(d, f, a)
        emit(n, r, state(d))


SID0 = 76561198000000000


def perf_pl(r, tk, cap):
    fl = r.choice([0, 1, 2, 3]) if cap else r.choice([0, 1])
    pl = tk.encode() + bytes([1]) + bytes([r.choice([0, 1, 5, 20]), r.choice([0, 1, 3, 12])]) + r.choice([0, 100, 500, 3000]).to_bytes(2, "little") + r.choice([0, 160, 400]).to_bytes(2, "little") + bytes([fl])
    if cap:
        pl += bytes([1]) + r.choice([100, 800]).to_bytes(2, "little") + r.choice([200, 900]).to_bytes(2, "little") + bytes([r.choice([0, 30, 60, 100]), 8])
        if fl & 2:
            pl += bytes([1, r.choice([5, 20]), r.choice([0, 10]), 0, r.choice([50, 200]), 0, 60])
    if r.random() < 0.08:
        pl = pl[:60]
    return pl.hex()


def gen(r, n):
    toks = [tok(i) for i in range(12)]
    t = 5000.0
    yield {"t": t, "op": "reset"}
    yield {"t": t, "op": "seedrot", "v": r.randrange(0, 40), "seed": r.randrange(1, 10 ** 6)}
    yield {"t": t, "op": "names", "n": {x: r.choice(NAMES) for x in toks if r.random() < 0.8}}
    yield {"t": t, "op": "call", "f": "seq", "a": [[{"f": "bridge", "a": ["register", toks[i], "b%d" % i, SID0 + i, 40000 + i, "1.2.3.%d" % (i % 3), r.choice([None, "Steam%d" % (i % 3)])]} for i in range(12)]
                                                  + [{"f": "session", "a": ["register", toks[i], "s%d" % i]} for i in range(12)]]}
    matches = {}

    def mk_mm():
        pool = toks[:]
        r.shuffle(pool)
        m = {}
        i = 0
        while pool and i < 3:
            k = r.randint(2, 8)
            m["m%d" % i] = [pool.pop() for _ in range(min(k, len(pool)))]
            i += 1
        q = [x for x in pool if r.random() < 0.5]
        mids = list(m)
        return {"matches": m, "launched": [x for x in mids if r.random() < 0.7], "ended": [x for x in mids if r.random() < 0.2],
                "departed": {x: [y for y in m[x] if r.random() < 0.15] for x in mids}, "queue": q,
                "left": {y: [t - r.choice([1.0, 5.0, 20.0]), t] for y in pool if r.random() < 0.3}, "joining": {},
                "started": [x for x in mids if r.random() < 0.6], "started_t": {x: t - r.choice([3.0, 50.0]) for x in mids}}

    cur_mm = mk_mm()
    matches = cur_mm["matches"]
    yield dict(t=t, op="mm", **cur_mm)
    for i in range(n):
        t += r.choice([0.0, 1.0, 5.0, 30.0, 100.0, 4000.0])
        x = r.random()
        c = {"t": t, "op": "call"}
        sub = r.sample(toks, r.randint(0, 9))
        one = r.choice(toks)
        if r.random() < 0.16:
            k = r.random()
            if k < 0.30:      # a burst of pair pings (every host candidate's pings known)
                grp = r.sample(toks[:8], r.randint(3, 8))
                base = r.choice([30, 60, 90, 150, 170])
                rep = []
                for a_ in grp:
                    for b_ in grp:
                        if a_ != b_ and r.random() < 0.93:
                            rep.append({"f": "ping_report", "a": [a_, SID0 + toks.index(b_), max(1, base + r.randint(-20, 120 if r.random() < 0.15 else 20))]})
                c.update(f="seq", a=[rep])
            elif k < 0.50:    # location sequence: region change / spikes / a changed connection
                tk = r.choice(toks[:8])
                seqs = r.choice([["ams=20 fra=25", "ams=20 fra=25", "ams=300 fra=310", "ams=310 fra=300", "ams=320"], ["iad=15 atl=22", "ams=20 fra=25", "ams=18", "ams=22 lhr=30"],
                                 ["sea=12", "sea=14", "lax=900", "lax=910", "lax=905", "lax=900"], [r.choice(LOCS) for _ in range(4)]])
                c.update(f="seq", a=[[{"f": "ping_location", "a": [tk, loc]} for loc in seqs]])
            elif k < 0.68 and cur_mm["matches"]:      # vote flow
                m = r.choice(list(cur_mm["matches"]))
                pl = cur_mm["matches"][m]
                calls = [{"f": "vote_start", "a": [pl]}]
                for _ in range(r.randint(0, 8)):
                    calls.append({"f": "vote_cast", "a": [r.choice(pl), r.choice([0, 1, 2, 2, 3])]})
                calls.append({"f": "vote_state_text", "a": [r.choice(pl)]})
                if r.random() < 0.7:
                    calls.append({"f": "vote_close", "a": [pl]})
                calls.append({"f": "vote_cast", "a": [r.choice(pl), 1]})
                calls.append({"f": "vote_state_text", "a": [r.choice(pl)]})
                c.update(f="seq", a=[calls])
            elif k < 0.82:    # session socket lost in a running match / re-attach
                run = [tk for mid in cur_mm["launched"] if mid not in cur_mm["ended"] for tk in cur_mm["matches"][mid] if tk not in cur_mm["departed"].get(mid, [])]
                if run:
                    tk = r.choice(run)
                    calls = [{"f": "session", "a": ["register", tk, "s%d" % toks.index(tk)]}, {"f": "lost", "a": [tk, "s%d" % toks.index(tk)]}]
                    if r.random() < 0.85:
                        calls.append({"f": "reattach", "a": ["n%d" % r.randrange(3), tk, r.choice(NAMES), ["5.6.7.8", 4000]]})
                    c.update(f="seq", a=[calls])
                else:
                    c.update(f="match_sid", a=[[one]])
            else:             # Steam P2P / server relay for a match, match start
                pl = r.choice(list(cur_mm["matches"].values())) if cur_mm["matches"] else sub
                calls = [{"f": "push_peers", "a": [pl]}, {"f": "match_start_frame", "a": [pl[0] if pl else one, pl]}]
                if len(pl) >= 2 and r.random() < 0.6:       # known pings, the match forms, two removals for ping (the estimate is learned)
                    calls = [{"f": "ping_report", "a": [pl[0], SID0 + toks.index(pl[1]), 40]}, {"f": "ping_report", "a": [pl[1], SID0 + toks.index(pl[0]), 50]}] + calls
                calls.append({"f": "start_match", "a": [pl]})
                if len(pl) >= 2 and r.random() < 0.7:
                    mid_ = next((k for k, v in cur_mm["matches"].items() if v == pl), "zz")
                    calls += [{"f": "ping_kick_learn", "a": [mid_, pl[1], pl[0], r.choice([200, 300, 400]), t]}, {"f": "ping_kick_learn", "a": [mid_, pl[1], pl[0], r.choice([250, 350]), t + 1]}]
                c.update(f="seq", a=[calls])
            yield c
            continue
        if x < 0.02:
            m = mk_mm()
            cur_mm = m
            matches = m["matches"]
            yield dict(t=t, op="mm", **m)
            continue
        if x < 0.05:
            k, v = r.choice([("T2R_SP_MAPS", r.choice([None, "", "all", "1", "none", "hardware,motel", "train-station,downtown,p1-plaza"])),
                             ("T2R_MAP_POOL", r.choice([None, "", "to1-forest,to1-school,to1-resort", "to1-gas-station", "to1-p1-plaza,to1-forest"])),
                             ("T2R_MAP_EXCLUDE", r.choice([None, "", "to1-school,to1-gas-station", "to1-forest"])),
                             ("T2R_MAP", r.choice([None, "rotate", "random", "to1-school", "to1-forest", "to1-p1-plaza"])),
                             ("T2R_MAX_TEAM", r.choice([None, "1", "0"])), ("T2R_VOTE_ALWAYS", r.choice([None, "", "to1-jackson-market", "to1-forest,to1-school"])),
                             ("T2R_VOTE_ALL", r.choice([None, "1", ""])), ("T2R_P1_PREF", r.choice([None, "1", "0"])),
                             ("T2R_SP_GATE", r.choice([None, "1", "0"])), ("T2R_MAP_VOTE", r.choice([None, "1", ""])),
                             ("T2R_MATCH_FLOW", r.choice([None, "1", ""])), ("T2R_COUNTDOWN", r.choice([None, "10", "6", "2", "1"])),
                             ("T2R_PRE_COUNTDOWN", r.choice([None, "5", "2", "0"])), ("T2R_PUB_IP", r.choice([None, "10.1.2.3", "209.25.141.16"])),
                             ("T2R_PUB_UDP_PORT", r.choice([None, "15389", "4000"])), ("T2R_TEST_ONE_TEAM", r.choice([None, "1", ""])),
                             ("T2R_TEST_SAME_SPAWN", r.choice([None, "1"])), ("T2R_NOTE", r.choice([None, "", "hello note", "x" * 300])),
                             ("T2R_P2P", r.choice([None, "0", "1"])), ("T2R_NO_HOST_PICK", r.choice([None, "", "1"])),
                             ("T2R_HOSPITAL_OLD", None), ("T2R_PING_MESH", None)])
            yield {"t": t, "op": "env", "k": k, "v": v}
            continue
        if x < 0.07:
            yield {"t": t, "op": "setmap", "swap": {m: {r.choice(matches[m]): r.randint(0, 1)} for m in matches if r.random() < 0.4},
                   "p1": {m: r.random() < 0.5 for m in matches if r.random() < 0.5}, "loaded": [y for y in toks if r.random() < 0.3],
                   "cdgot": [y for y in toks if r.random() < 0.2],
                   "p2p": [[r.sample(toks, r.randint(2, 4)), r.random() < 0.5] for _ in range(r.randint(0, 3))],
                   "sidkeep": [[r.sample(toks, r.randint(2, 4)), r.randrange(0, 2 ** 32)] for _ in range(r.randint(0, 3))],
                   "timing": [[r.sample(toks, r.randint(2, 4)), t - r.choice([1.0, 20.0])] for _ in range(r.randint(0, 3))]}
            continue
        if x < 0.08:
            nm = r.choice([None, "# comment\n", "%d\n" % (76561198000000000 + r.randrange(30)), "alpha\nKILO # x\n%d" % (76561198000000000 + r.randrange(30)), "\n\n"])
            yield {"t": t, "op": "nohost", "text": nm, "mt": 1000 + r.randrange(1000)}
            continue
        if x < 0.30:      # versions / languages / texts
            k = r.random()
            tk = r.choice(toks)
            ver = r.choice(["sv-test", "abc123", "x" * 41, "bad ver", "v1.2-3+b", "sv-test"])
            if k < 0.35:
                vb = ver.encode("latin1")
                pl = bytes([len(vb)]) + vb
                if r.random() < 0.8:
                    pl += tk.encode()
                    if r.random() < 0.3:
                        pl += b"L"
                elif r.random() < 0.3:
                    pl += b"zz"
                if r.random() < 0.05:
                    pl = b""
                if r.random() < 0.03:
                    pl = bytes([0]) + pl
                c.update(f="version_op", a=[pl.hex(), r.choice([None, ["1.2.3.4", 5000], ["9.9.9.%d" % r.randrange(5), 6000]])])
            elif k < 0.45:
                c.update(f="note_lang", a=[r.choice([tk, tk, tk.upper(), tk[:10], "zz" * 32]), r.choice([0, 1, 1, 0, 2, 255])])
            elif k < 0.5:
                c.update(f="norm_lang", a=r.choice([["none", None], ["bytes", ""], ["bytes", "01"], ["bytes", "05"], ["int", 1], ["int", 0], ["str", "pl-PL"],
                                                   ["str", " Polish "], ["str", "en"], ["str", "PL_pl"], ["str", "1"], ["str", ""], ["str", "pol"]]))
            elif k < 0.6:
                key = r.choice(["update_refusal", "ver_mismatch", "ver_mismatch_linux", "files_edited", "kicked_why", "afk_kicked", "ping_kicked", "load_named", "loss_kicked", "moved"])
                kw = {"a": "A1", "b": "B{x}", "f": "file.dll", "r": "why {a}", "s": 90, "ms": 250, "lim": 190, "pct": 40, "m": 5, "n": "Nme"}
                use = {k2: kw[k2] for k2 in r.sample(list(kw), r.randint(0, 5))}
                if key in ("ver_mismatch", "ver_mismatch_linux"):
                    use = {"a": "A1", "b": "B{x}"}
                elif key == "files_edited":
                    use = {"f": "file.dll"}
                elif key == "kicked_why":
                    use = {"r": "why {a}"}
                elif key == "afk_kicked":
                    use = {"s": 90}
                elif key == "ping_kicked":
                    use = {"ms": 250, "s": 60, "lim": 190}
                elif key == "loss_kicked":
                    use = {"pct": 40, "s": 120, "lim": 30, "m": 5}
                elif key == "load_named":
                    use = {"n": "Nme"}
                else:
                    use = {}
                if r.random() < 0.5:
                    c.update(f="srv_text", a=[key, r.choice(["en", "pl", "PL", "x", ""]), use])
                else:
                    c.update(f="srv_note", a=[key, r.choice(toks), use])
            elif k < 0.8:
                c.update(f=r.choice(["version_problem", "version_refusal"]), a=[r.choice(toks)])
            elif k < 0.9:
                c.update(f="save_tables", a=[t + r.choice([0, 3, 10])])
            else:
                c.update(f="save_soon", a=[])
            yield c
            continue
        if x < 0.50:      # maps
            k = r.random()
            if k < 0.2:
                c.update(f="map_pool", a=[sub, r.choice([None, None, r.sample(toks, r.randint(0, 3))])])
            elif k < 0.32:
                c.update(f="map_index", a=[sub])
            elif k < 0.42:
                c.update(f="vote_options", a=[sub])
            elif k < 0.5:
                c.update(f="vote_start", a=[sub])
            elif k < 0.6 and matches:
                m = r.choice(list(matches))
                c.update(f="vote_cast", a=[r.choice(matches[m] + toks[:2]), r.choice([0, 1, 2, 3, -1, 7])])
            elif k < 0.68 and matches:
                m = r.choice(list(matches))
                c.update(f=r.choice(["vote_close", "vote_close", "vote_start"]), a=[matches[m]])
            elif k < 0.76:
                c.update(f="vote_state_text", a=[r.choice(toks)])
            elif k < 0.82:
                tk = r.choice(toks)
                op = r.choice([0xEA, 0xEC, 0xEC, 0x01])
                pl = tk.encode() + (bytes([r.randint(0, 4)]) if r.random() < 0.8 else b"")
                if r.random() < 0.1:
                    pl = pl[:50]
                c.update(f="vote_op", a=[op, pl.hex()])
            elif k < 0.9:
                tk = r.choice(toks)
                pl = tk.encode() + (r.randrange(0, 2 ** 32)).to_bytes(4, "little") + (bytes([r.randint(0, 3)]) if r.random() < 0.6 else b"")
                if r.random() < 0.08:
                    pl = pl[:40]
                c.update(f="sp_support_op", a=[pl.hex()])
            elif k < 0.94:
                tk = r.choice(toks)
                hs = [r.choice([0xabcd1234, 0x11112222, r.randrange(2 ** 32)]) for _ in range(r.randint(0, 3))]
                pl = tk.encode() + bytes([len(hs)]) + b"".join(h.to_bytes(4, "little") for h in hs)
                c.update(f="map_packs_op", a=[pl.hex()])
            elif k < 0.97 and matches:
                m = r.choice(list(matches))
                c.update(f="p1", a=[r.choice(toks), r.choice([None, r.choice(toks), ""]), m, sub])
            else:
                c.update(f="maps_misc", a=[sub, r.choice([None, 10, 6, 1, 0]), r.choice([5, 2, 0]), r.choice([10, 6, 2, 1]), r.choice([True, False]),
                                          r.choice([None, True, False]), r.choice([None, 5, 3]), r.choice([None, 10, 4])])
            yield c
            continue
        if x < 0.66:      # flow / records / match start
            k = r.random()
            pl = sub or [one]
            if k < 0.15:
                c.update(f="match_sid", a=[pl])
            elif k < 0.25 and matches:
                m = r.choice(list(matches))
                c.update(f="teams", a=[r.randrange(0, 9), m, r.choice([None, r.choice(matches[m]), r.choice(toks)])])
            elif k < 0.38:
                roster = [[i, r.choice(NAMES) + ("é" if r.random() < 0.2 else "")] for i in range(r.randint(0, 8))]
                c.update(f="build_player_record", a=[r.randrange(0, 8), r.randrange(0, 2 ** 32), r.randrange(0, 40), roster, r.choice([None, ["127.0.0.1", r.randrange(1, 65535)]])])
            elif k < 0.45:
                c.update(f="build_peer_table", a=[r.sample(toks, r.randint(0, 11))])
            elif k < 0.60:
                if matches and r.random() < 0.6:
                    m = r.choice(list(matches))
                    c.update(f="match_start_frame", a=[r.choice(matches[m]), matches[m]])
                else:
                    c.update(f="match_start_frame", a=[one, r.choice([[], pl, [one]])])
            elif k < 0.67:
                c.update(f="stage_name", a=[r.choice([1, 2]), r.choice([0, 1, 5, 100, 101, 110, 131, 140, 161, 175, 201, 205])])
            elif k < 0.77:
                c.update(f=r.choice(["timing_mark", "timing_mark", "timing_summary"]), a=[pl] + ([r.choice(["match found", "vote", "level start", "horn"])] if r.random() < 0.7 else []))
                if c["f"] == "timing_summary":
                    c["a"] = [pl]
                elif len(c["a"]) < 2:
                    c["a"].append("vote")
            elif k < 0.83:
                c.update(f="rekey", a=[r.choice([pl, sub]), r.choice([pl, sub + [one]]), r.randrange(1, 2 ** 32)])
            elif k < 0.88:
                c.update(f="forget", a=[pl])
            elif k < 0.95 and matches:
                m = r.choice(list(matches))
                c.update(f="start_match", a=[matches[m]])
            elif matches:
                m = r.choice(list(matches))
                c.update(f="countdown", a=[matches[m]])
            else:
                c.update(f="match_sid", a=[pl])
            yield c
            continue
        if x < 0.90:      # ping
            k = r.random()
            if k < 0.12:
                c.update(f="ping_location", a=[r.choice(toks[:8]), r.choice(LOCS)])
            elif k < 0.30:
                c.update(f="ping_report", a=[r.choice(toks[:8]), 76561198000000000 + r.randrange(8), r.choice([-1, 10, 30, 80, 120, 170, 180, 200, 250, 400])])
            elif k < 0.38:
                a, b = r.sample(toks[:8], 2)
                c.update(f="pair_ping", a=[a, b, r.random() < 0.3, r.choice([None, t])])
            elif k < 0.44:
                a, b = r.sample(toks[:8], 2)
                m = r.choice(list(matches) + ["zz"]) if matches else "zz"
                c.update(f="ping_kick_learn", a=[m, a, b, r.choice([150, 200, 260, 400]), t])
            elif k < 0.48:
                c.update(f="mm_region", a=[r.choice(toks)])
            elif k < 0.52:
                c.update(f=r.choice(["mesh_peers", "mesh_activate"]), a=[r.choice(toks)])
            elif k < 0.60:
                if cur_mm["matches"] and r.random() < 0.5:      # a match under host migration: the capacity and one-way trailers book against its host
                    mid_ = r.choice(list(cur_mm["matches"]))
                    mp = cur_mm["matches"][mid_]
                    act = [x for x in mp if x not in cur_mm["departed"].get(mid_, [])] or mp
                    c.update(f="seq", a=[[{"f": "hm_start", "a": [mid_, mp]}] + [{"f": "perf_op", "a": [perf_pl(r, r.choice(act), True)]} for _ in range(r.randint(1, 4))]])
                else:
                    c.update(f="perf_op", a=[perf_pl(r, r.choice(toks), r.random() < 0.3)])
            elif k < 0.64:
                c.update(f="perf_penalty", a=[r.choice(toks)])
            elif k < 0.76:
                c.update(f="best_host_order", a=[r.sample(toks[:8], r.randint(0, 6))])
            elif k < 0.82:
                c.update(f="host_pool", a=[r.sample(toks[:8], r.randint(1, 6))])
            elif k < 0.86:
                c.update(f="host_allowed", a=[r.sample(toks[:8], r.randint(0, 6))])
            elif k < 0.92:
                c.update(f="push_peers", a=[r.sample(toks[:8], r.randint(1, 5))])
            elif k < 0.95:
                c.update(f="party_lead", a=[r.choice(toks)])
            else:
                c.update(f="jip_hv", a=[r.choice(toks), r.choice([2, 3, 4])]) if r.random() < 0.7 else c.update(f="hc", a=[r.choice(toks), r.choice([3, 4, 6]), r.choice([100, 400, 800]), r.choice([200, 900]), "mx"])
            yield c
            continue
        # presence / bridges / sessions
        k = r.random()
        cid = "c%d" % r.randrange(1, 8)
        tk = r.choice(toks[:8])
        if k < 0.25:
            sub_k = r.choice(["touch", "touch", "known", "drop", "prune", "ping"])
            if sub_k == "touch":
                c.update(f="pres", a=["touch", tk, r.choice(NAMES + ["<b>x</b>", "<>", ""])])
            elif sub_k == "prune":
                c.update(f="pres", a=["prune", r.choice([0, 5, 100, 5000]), r.sample(toks, r.randint(0, 4))])
            elif sub_k == "ping":
                c.update(f="pres", a=["ping", ["10.0.0.%d" % r.randrange(5), 5000 + r.randrange(5)]])
            else:
                c.update(f="pres", a=[sub_k, tk])
            if c["a"][0] != "ping" and c["a"][0] != "prune" and False:
                pass
        elif k < 0.45:
            kk = r.choice(["register", "register", "unregister", "q"])
            if kk == "register":
                ix = toks.index(tk)
                c.update(f="bridge", a=["register", tk, r.choice(["b%d" % ix, "b%d" % ix, cid]), SID0 + ix, r.choice([0, 40000 + ix, 40000 + ix]), r.choice([None, "1.2.3.%d" % r.randrange(3)]), r.choice([None, "Steam" + str(r.randrange(3)), ""])])
            elif kk == "unregister":
                c.update(f="bridge", a=["unregister", tk, r.choice([cid, None])])
            else:
                c.update(f="bridge", a=["q", tk, "1.2.3.%d" % r.randrange(3)])
        elif k < 0.6:
            kk = r.choice(["register", "unregister", "q"])
            c.update(f="session", a=[kk, tk, cid if kk == "register" else r.choice([cid, None])])
        elif k < 0.7:
            c.update(f="adopt", a=[r.choice(toks + ["ab" * 32]), "1.2.3.%d" % r.randrange(3)])
        elif k < 0.8:
            c.update(f="lost", a=[r.choice(toks), cid])
        elif k < 0.95:
            c.update(f="reattach", a=[cid, r.choice(toks + [None, "short"]), r.choice(NAMES), ["5.6.7.8", 4000 + r.randrange(5)]])
        else:
            c.update(f="peer_seen", a=["1.2.3.%d" % r.randrange(40), ["1.2.3.4", r.randrange(5000, 6000)]])
        yield c


if __name__ == "__main__":
    main(gen, run)
