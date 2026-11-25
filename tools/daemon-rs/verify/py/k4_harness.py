"""Differential test of the pure parts of daemon.py's K4 chunk (teams: skill_of_kd, player_skill, rule_match / rule_groups / rule_team_for,
split_teams, order_slots, host_swap_pick, host_from_team, team_order) against `t2r-daemon selftest k4`.

  python3 k4_harness.py gen SEED N > script.jsonl
  T2R_DB=/tmp/k4py.db T2R_TOOLS_DIR=/tmp/k4tools python3 k4_harness.py run < script.jsonl > out.py.jsonl
  T2R_DB=/tmp/k4rs.db T2R_TOOLS_DIR=/tmp/k4tools t2r-daemon selftest k4 < script.jsonl > out.rs.jsonl ; cmp ...

daemon.py is imported from T2R_PY_DIR (no threads start at import). The world (names, Steam ids, pings, no-host players, the scripted
best_host_order, parties, rule / top files, match_stats rows) is set per "world" line; the K3 functions that read live state (pair_ping,
best_host_order, get_name ...) are replaced by the scripted table in both languages.
"""
import os
import sys

os.environ.setdefault("T2R_HOST_CAP", "0")
os.environ.setdefault("T2R_DB", "/tmp/k4py.db")
os.environ.setdefault("T2R_TOOLS_DIR", "/tmp/k4tools")
import cseed  # noqa: E402
from cseed import T, emit, fl, log, main, script, tok  # noqa: E402

import time as _time  # noqa: E402
_time.time = lambda: T[0]          # every module of the daemon (requeue / lobbybook / social ... keep time.time as their clock)

DB = os.environ["T2R_DB"]
TOOLS = os.environ["T2R_TOOLS_DIR"]
NAMES = ["Alpha", "bravo", "Charlie Brown", "Charlie", "delta force", "Echo", "Foxtrot", "golf", "Hotel", "india", "Juliet", "kilo"]
WORLD = {}
MTIME = [1000]


def world_py(w):
    import sqlite3
    import daemon as d
    WORLD.clear()
    WORLD.update(w)
    os.makedirs(TOOLS, exist_ok=True)
    for fn, key in (("team_rules.txt", "rules"), ("top_players.txt", "top")):
        p = os.path.join(TOOLS, fn)
        if w.get(key) is None:
            if os.path.exists(p):
                os.remove(p)
        else:
            with open(p, "w", encoding="utf-8") as f:
                f.write(w[key])
            MTIME[0] += 10
            os.utime(p, (MTIME[0], MTIME[0]))
    d.TEAM_RULES_FILE = os.path.join(TOOLS, "team_rules.txt")
    d.TOP_FILE = os.path.join(TOOLS, "top_players.txt")
    d._top_cache.update(mt=None, keys=frozenset())
    d._rules_cache.update(mt=None, rules=())
    d._skill_cache.clear()
    c = sqlite3.connect(DB)
    c.execute("CREATE TABLE IF NOT EXISTS match_stats(token TEXT, kills INT, deaths INT, ts REAL)")
    c.execute("CREATE TABLE IF NOT EXISTS players(token TEXT PRIMARY KEY, name TEXT, loadout BLOB, kills INT DEFAULT 0, deaths INT DEFAULT 0, wins INT DEFAULT 0, matches INT DEFAULT 0, steamid INTEGER DEFAULT 0, last_seen REAL)")
    c.execute("DELETE FROM match_stats")
    c.execute("DELETE FROM players")
    for t, sid in w.get("players", []):
        c.execute("INSERT INTO players(token, steamid) VALUES (?,?)", (t, sid))
    for t, k, de, ts in w.get("stats", []):
        c.execute("INSERT INTO match_stats(token, kills, deaths, ts) VALUES (?,?,?,?)", (t, k, de, ts))
    c.commit()
    c.close()
    d.MM.__init__()
    d.MATCH_MAP.clear()
    d.JIP_BACK.clear()
    d.TEAM_SWAP.clear()
    d.PING_KICKED.clear()
    for t in w.get("all", []):
        d.PARTIES.leave(t)
    for g in w.get("parties", []):
        for m in g[1:]:
            d.PARTIES.invite_token(g[0], m)
            d.PARTIES.accept_token(m, g[0])


def setup():
    import types
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
    d.log = log
    d.evt = lambda kind, *a: log("EVT", kind, *a)
    d.get_name = lambda t: WORLD.get("names", {}).get(t) or None
    d.token_steamid = lambda t: WORLD.get("sids", {}).get(t, 0)
    d.perf_penalty = lambda t: WORLD.get("perf", {}).get(t, 0)
    d.no_host = lambda t: t in WORLD.get("noh", [])
    d.ping_match_ms = lambda: WORLD.get("lim", 175)

    def pp(a, b, raw=False, now=None):
        for k in ((a, b), (b, a)):
            pass
        v = WORLD.get("pings", {}).get("%s|%s" % (a, b))
        return v

    d.pair_ping = pp

    def bho(players):
        o = WORLD.get("bho", [])
        return [t for t in o if t in players] + [t for t in players if t not in o]

    d.best_host_order = bho
    d.MM_PING_GATE = True
    d.PING_KICK_MS = 190
    d.PRES_TOKENS = lambda: list(WORLD.get("online", []))
    d.silence = lambda t, now=None: 0.0
    d.requeue_keep_mode = lambda t, m: None
    d.ping_wait_judge = lambda kept, gh, pg, now, members, host_ok=None: (False, [])
    d.ping_left_out_note = lambda dropped, now: None
    d.jip_ping_block = lambda *a, **k: None
    d._ping_gate = lambda a, b, now=None: pp(a, b)
    d.mm_region = lambda t: WORLD.get("regions", {}).get(t)
    d.p1_pref_on = lambda: False
    d.p1_always = lambda t, ld=None: False
    d.p1_owner = lambda t, ld=None: False
    d.p1_fits = lambda t, m, ld=None: True
    d.has_helper = lambda t: False
    d.countdown_seconds = lambda: 10.0
    d.map_pool = lambda players, support=None: list(range(40))
    d.MAPS = type("M", (), {"__getitem__": lambda self, i: ("", "", "map%d" % i)})()
    return d


def run():
    d = setup()
    n = 0
    for c in script():
        T[0] = c.get("t", T[0])
        op = c["op"]
        n += 1
        if op == "world":
            world_py(c["w"])
            emit(n, None, None)
            continue
        if op == "mm":
            r = mm_op(d, c)
            emit(n, r, mmstate(d))
            continue
        a = c.get("a", [])
        f = c["f"]
        if f == "skill_of_kd":
            r = d.skill_of_kd(*a)
        elif f == "player_skill":
            r = d.player_skill(a[0])
        elif f == "rule_match":
            r = d.rule_match(a[0], a[1])
        elif f == "rule_groups":
            r = [[k, g] for k, g in d.rule_groups(a[0])]
        elif f == "rule_team_for":
            r = list(d.rule_team_for(a[0], a[1], [(k, g) for k, g in a[2]]))
        elif f == "split_teams":
            pof = a[1]
            sk = a[2]
            top = set(a[3]) if a[3] is not None else None
            rg = [(k, g) for k, g in a[4]] if a[4] is not None else None
            r = [list(x) for x in d.split_teams(a[0], pof, sk, top, rg)]
        elif f == "order_slots":
            r = d.order_slots(a[0], a[1], a[2])
        elif f == "host_swap_pick":
            rg = [(k, g) for k, g in a[6]]
            r = d.host_swap_pick(a[0], a[1], a[2], a[3], a[4], set(a[5]), rg, a[7])
            r = None if r is None else [list(x) for x in r]
        elif f == "host_from_team":
            r = d.host_from_team(a[0], a[1])
        elif f == "team_order":
            r = d.team_order(a[0])
        else:
            raise SystemExit("unknown f " + f)
        emit(n, r, None)


def gen(r, n):
    toks = [tok(i) for i in range(12)]
    w = None

    def make_world():
        k = r.choice([6, 9, 12])
        ts = toks[:k]
        names = {}
        for t in ts:
            if r.random() < 0.9:
                names[t] = r.choice(NAMES) if r.random() < 0.85 else r.choice(NAMES) + " " + r.choice(["x", "7", "Two"])
        sids = {t: r.choice([0, 76561198000000000 + r.randrange(1000)]) for t in ts}
        pings = {}
        for a in ts:
            for b in ts:
                if a != b and r.random() < 0.92:
                    pings["%s|%s" % (a, b)] = r.choice([20, 60, 90, 120, 150, 170, 176, 190, 220, 300, r.randrange(30, 320)])
        if r.random() < 0.5:
            for a in ts:
                for b in ts:
                    if a < b and "%s|%s" % (a, b) in pings:
                        pings["%s|%s" % (b, a)] = pings["%s|%s" % (a, b)]
        parties = []
        pool = ts[:]
        r.shuffle(pool)
        while len(pool) >= 2 and r.random() < 0.55:
            g = [pool.pop() for _ in range(min(len(pool), r.choice([2, 2, 3, 4])))]
            if len(g) >= 2:
                parties.append(g)
        nm = lambda t: names.get(t) or str(sids[t])
        lines = []
        for _ in range(r.choice([0, 1, 2, 3])):
            g = r.sample(ts, r.randint(2, 4))
            lines.append(r.choice(["apart", "together", "Apart", "bogus"]) + " " + r.choice([" ", ", ", " and "]).join((('"%s"' % nm(t)) if r.random() < 0.3 else nm(t)) for t in g) + r.choice(["", " # c"]))
        rules = None if r.random() < 0.2 else "# rules\n" + "\n".join(lines) + "\n"
        top_names = [nm(t) for t in r.sample(ts, r.randint(0, 3))]
        top = None if r.random() < 0.3 else "\n".join(top_names) + "\n"
        players = [[t, sids[t]] for t in ts if r.random() < 0.8]
        stats = []
        for t in ts:
            for _ in range(r.choice([0, 0, 1, 3, 8])):
                stats.append([t, r.choice([-1, 0, 3, 12, 30, None]), r.choice([0, 2, 9, 25, None, -3]), fl(r.random() * 1000)])
        return {"names": names, "sids": sids, "pings": pings, "noh": r.sample(ts, r.choice([0, 0, 1, 2])), "bho": r.sample(ts, len(ts)),
                "perf": {t: r.choice([0, 0, 0, 40]) for t in ts}, "lim": r.choice([175, 175, 190, 120]), "parties": parties, "rules": rules,
                "top": top, "players": players, "stats": stats, "all": toks, "ts": ts}

    t = 5000.0
    for i in range(n):
        if w is None or r.random() < 0.06:
            w = make_world()
            t += 1.0
            yield {"t": t, "op": "world", "w": w}
        ts = w["ts"]
        pof = {}
        for g in w["parties"]:
            for m in g:
                pof[m] = "P%d" % (hash(g[0]) % 1000 if False else toks.index(g[0]))
        t += r.choice([0.0, 1.0, 100.0, 400.0])
        x = r.random()
        c = {"t": t, "op": "call"}
        if x < 0.05:
            c.update(f="skill_of_kd", a=[r.choice([-5, 0, 3, 40]), r.choice([-2, 0, 7, 55])])
        elif x < 0.12:
            c.update(f="player_skill", a=[r.sample(ts, r.randint(0, len(ts)))])
        elif x < 0.20:
            txt = " ".join(r.choice(NAMES + ["x", "Alpha Beta", '"Charlie Brown"', "'delta force'", str(r.choice(list(w["sids"].values())))]) for _ in range(r.randint(0, 5)))
            c.update(f="rule_match", a=[txt, r.sample(ts, r.randint(0, len(ts)))])
        elif x < 0.26:
            c.update(f="rule_groups", a=[r.sample(ts, r.randint(0, len(ts)))])
        elif x < 0.32:
            tk = r.choice(ts)
            others = [o for o in ts if o != tk and r.random() < 0.6]
            rg = []
            for _ in range(r.randint(0, 3)):
                rg.append([r.choice(["apart", "together"]), r.sample(ts, r.randint(2, 4))])
            c.update(f="rule_team_for", a=[tk, {o: r.randint(0, 1) for o in others}, rg])
        elif x < 0.55:
            k = r.choice([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12])
            pl = r.sample(ts, min(k, len(ts)))
            sk = None if r.random() < 0.3 else {t2: fl(0.4 + r.random() * 0.5) for t2 in pl if r.random() < 0.9}
            if sk is not None and r.random() < 0.15:
                sk = {}
            top = None if r.random() < 0.3 else r.sample(pl, r.randint(0, min(3, len(pl))))
            rg = None if r.random() < 0.3 else [[r.choice(["apart", "together"]), r.sample(pl, r.randint(2, min(4, len(pl))))] for _ in range(r.randint(0, 2)) if len(pl) >= 2]
            c.update(f="split_teams", a=[pl, {t2: pof[t2] for t2 in pl if t2 in pof}, sk, top, rg])
        elif x < 0.62:
            pl = r.sample(ts, r.randint(2, min(8, len(ts))))
            k = r.randint(1, len(pl) - 1)
            host = r.choice(pl + [None])
            c.update(f="order_slots", a=[pl[:k], pl[k:], host])
        elif x < 0.80:
            pl = r.sample(ts, r.randint(3, min(8, len(ts))))
            k = r.randint(1, len(pl) - 1)
            small, big = pl[:min(k, len(pl) - k)], pl[min(k, len(pl) - k):]
            if len(big) <= len(small):
                big, small = pl[1:], pl[:1]
            sk = None if r.random() < 0.3 else {t2: fl(0.4 + r.random() * 0.5) for t2 in pl if r.random() < 0.9}
            top = r.sample(pl, r.randint(0, 2))
            rg = [[r.choice(["apart", "together"]), r.sample(pl, r.randint(2, min(4, len(pl))))] for _ in range(r.randint(0, 2))]
            host = small[0] if r.random() < 0.9 else r.choice(pl)
            c.update(f="host_swap_pick", a=[host, big, small, pl, sk, top, rg, {t2: pof[t2] for t2 in pl if t2 in pof}])
        elif x < 0.88:
            pl = r.sample(ts, r.randint(3, min(8, len(ts))))
            c.update(f="host_from_team", a=[pl[:max(1, len(pl) // 2 + 1)], pl])
        else:
            pl = r.sample(ts, r.randint(2, min(8, len(ts))))
            c.update(f="team_order", a=[pl])
        yield c


def mmstate(d):
    m = d.MM

    def sd(x):
        return {k: v for k, v in sorted(x.items())}
    return {"queue": [[k, v] for k, v in m.queue.items()], "seen": sd(m.seen), "matches": [[k, list(v)] for k, v in m.matches.items()],
            "formed": sd(m.formed), "left": {k: list(v) for k, v in sorted(m.left.items())}, "settle": sd(m.settle),
            "qtime": {k: [[a, b] for a, b in v.items()] for k, v in sorted(m.qtime.items())}, "launched": sorted(m.launched),
            "departed": {k: sorted(v) for k, v in sorted(m.departed.items())}, "ended": sorted(m.ended), "ended_at": sd(m.ended_at),
            "started": sorted(m.started), "started_t": sd(m.started_t), "clock": {k: list(v) for k, v in sorted(m.clock.items())},
            "polled": {k: sd(v) for k, v in sorted(m.polled.items())}, "joining": [[k, list(v)] for k, v in m.joining.items()],
            "lives": {k: list(v) for k, v in sorted(m.lives.items())}, "late_bonus": {k: list(v) for k, v in sorted(m.late_bonus.items())},
            "late_mult": {k: list(v) for k, v in sorted(m.late_mult.items())}, "move_team": [[k, list(v)] for k, v in m.move_team.items()],
            "departed_t": sd(m.departed_t), "departed_vol": sorted(m.departed_vol), "last_arrival": m.last_arrival, "hold": m._hold_since,
            "book": {k: [list(l.members), l.hold, l.wait_until] for k, l in sorted(m.book.lobbies.items())}, "bookof": sd(m.book.of),
            "jip_back": {k: list(v) for k, v in sorted(d.JIP_BACK.items())},
            "mm_map": sorted([sorted(k), v] for k, v in d.MATCH_MAP.items())}


def mm_op(d, c):
    m = d.MM
    k = c["m"]
    mids = list(m.matches)
    mid = mids[c["i"] % len(mids)] if (mids and "i" in c) else "none"
    if k == "enqueue":
        r = m.enqueue(c["tok"])
    elif k == "poll":
        m.poll(c["tok"])
        r = None
    elif k == "pollall":
        for t in list(m.queue):
            m.poll(t)
        r = None
    elif k == "try_form":
        r = m.try_form()
    elif k == "cancel":
        r = m.cancel(c["tok"])
    elif k == "dequeue":
        r = m.dequeue(c["tok"])
    elif k == "leave":
        m.leave(c["tok"])
        r = None
    elif k == "depart":
        r = m.depart(c["tok"], c["vol"])
    elif k == "end":
        m.end(mid)
        r = None
    elif k == "abort":
        r = m.abort(mid)
    elif k == "drop":
        m.drop(mid)
        r = None
    elif k == "setmode":
        r = d.MODEQ.set(c["tok"], c["mode"])
    elif k == "start":
        if mid in m.matches:
            m.started.add(mid)
            m.started_t[mid] = T[0]
            m.launched.add(mid)
            d.MATCH_MAP[frozenset(m.matches[mid])] = c["map"]
        r = mid
    elif k == "jip_clock":
        r = m.jip_clock(c["tok"], c["rem"], c["dur"], c["ended"])
    elif k == "clockm":
        r = m.jip_clock(m.matches[mid][0], c["rem"], c["dur"], c["ended"]) if mid in m.matches else None
    elif k == "departm":
        r = m.depart(m.matches[mid][c["j"] % len(m.matches[mid])], c["vol"]) if mid in m.matches else None
    elif k == "jip_lives":
        r = m.jip_lives(c["tok"], c["team"], c["ally"], c["enemy"])
    elif k == "jip_ready":
        r = m.jip_ready()
    elif k == "jip_join":
        why = []
        r = m.jip_join(c["tok"], why, c["need"], c.get("only"), c.get("team"))
        r = [None if r is None else [r[0], r[1], r[2], sorted(r[3])], why]
    elif k == "jip_acked":
        r = m.jip_acked(c["tok"])
    elif k == "jip_done":
        r = m.jip_done(c["tok"])
    elif k == "late":
        r = [m.late_mult_take(c["tok"], mid), m.late_bonus_take(c["tok"], mid)]
    elif k == "query":
        t = c["tok"]
        r = [m.in_match(t), m.players_of(t), m.mid_of(t), m.active_of(t), m.queued(), m.loading(t), m.countdown_over(t), m.all_loaded(t),
             m.jip_can_join(t), m.jip_age(t), sorted(m.mesh_view()[0]), sorted(m.mesh_view()[1].items()),
             [list(x[:6]) + [sorted(x[6])] for x in m.snapshot()], m.jip_joiners(mid)]
    elif k == "gc":
        r = m.gc(set(c["keep"]), T[0])
    else:
        raise SystemExit("unknown mm op " + k)
    return r


def gen_mm(r, n):
    toks = [tok(i) for i in range(10)]
    t = 9000.0
    w = {"names": {}, "sids": {}, "pings": {}, "noh": [], "bho": [], "perf": {}, "lim": 175, "parties": [], "rules": None, "top": None, "players": [],
         "stats": [], "all": toks, "ts": toks, "online": toks[:], "regions": {x: r.choice(["EU", "NA-east", "NA-west", "other"]) for x in toks}}
    if r.random() < 0.6:
        pool = toks[:]
        r.shuffle(pool)
        while len(pool) >= 2 and r.random() < 0.5:
            g = [pool.pop() for _ in range(min(len(pool), r.choice([2, 2, 3, 4])))]
            if len(g) >= 2:
                w["parties"].append(g)
    for a in toks:
        for b in toks:
            if a != b and r.random() < 0.9:
                w["pings"]["%s|%s" % (a, b)] = r.choice([30, 80, 120, 170, 230, 300])
    yield {"t": t, "op": "world", "w": w}
    out = 0
    while out < n:
        t += r.choice([0.2, 1.0, 1.0, 3.0, 4.0, 8.0, 20.0, 90.0])
        x = r.random()
        base = {"t": t, "op": "mm"}
        seq = []
        tk = r.choice(toks)
        if x < 0.25:
            for _ in range(r.randint(1, 5)):
                seq.append(dict(m="enqueue", tok=r.choice(toks)))
            seq.append(dict(m="pollall"))
        elif x < 0.50:
            seq += [dict(m="pollall"), dict(m="try_form")]
        elif x < 0.56:
            seq.append(dict(m="cancel", tok=tk))
        elif x < 0.59:
            seq.append(dict(m="dequeue", tok=tk))
        elif x < 0.61:
            seq.append(dict(m="leave", tok=tk))
        elif x < 0.66:
            seq.append(dict(m="depart", tok=tk, vol=r.random() < 0.5))
        elif x < 0.69:
            seq.append(dict(m="departm", i=r.randrange(5), j=r.randrange(8), vol=r.random() < 0.5))
        elif x < 0.72:
            seq.append(dict(m="end", i=r.randrange(5)))
        elif x < 0.74:
            seq.append(dict(m=r.choice(["abort", "drop"]), i=r.randrange(5)))
        elif x < 0.76:
            seq.append(dict(m="setmode", tok=tk, mode=r.choice([0, 1, 255])))
        elif x < 0.84:
            seq += [dict(m="start", i=r.randrange(5), map=r.randrange(5)), dict(m="clockm", i=r.randrange(5), rem=r.choice([40.0, 200.0, 600.0]), dur=900.0, ended=False),
                    dict(m="jip_lives", tok=tk, team=r.choice([0, 1, 2]), ally=r.randrange(0, 22), enemy=r.randrange(0, 22))]
        elif x < 0.96:
            for _ in range(r.randint(1, 3)):
                seq.append(dict(m="enqueue", tok=r.choice(toks)))
            seq += [dict(m="pollall"), dict(m="clockm", i=r.randrange(5), rem=r.choice([40.0, 200.0, 600.0]), dur=900.0, ended=r.random() < 0.1),
                    dict(m="jip_join", tok=r.choice(toks), need=r.choice([1, 1, 2])), dict(m="jip_ready"), dict(m="late", tok=r.choice(toks), i=r.randrange(5)),
                    dict(m=r.choice(["jip_acked", "jip_done"]), tok=r.choice(toks))]
        elif x < 0.99:
            seq.append(dict(m="query", tok=tk, i=r.randrange(5)))
        else:
            seq.append(dict(m="gc", keep=r.sample(toks, 3)))
        for c in seq:
            c.update(base)
            yield c
            out += 1


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "genmm":
        import json
        import random
        for c in gen_mm(random.Random(int(sys.argv[2]) * 7919 + 17), int(sys.argv[3])):
            sys.stdout.write(json.dumps(c, sort_keys=True, separators=(",", ":")) + "\n")
    else:
        main(gen, run)
