"""Offline tests for the host upload capacity history (tools/hostcap.py + daemon wiring: op 0xCA trailer, HOST-PICK cap, HOST-REPICK cap).
Run: python tools/tests/test_hostcap.py   (plain python; temp DB, no sockets)."""
import os, sys, tempfile
import sys as _t2s, os as _t2o; _t2s.path.append(_t2o.path.normpath(_t2o.path.join(_t2o.path.dirname(_t2o.path.abspath(__file__)), ".."))); import t2rtmp  # noqa: E402,F401

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)
os.environ.update(T2R_HOST="127.0.0.1", T2R_PORT="0", T2R_P2P="0")
os.environ.pop("T2R_NO_HOST_PICK", None)
os.environ.pop("T2R_HOST_CAP", None)
import daemon, hostmig, hostcap  # noqa: E402
daemon.DB_PATH = os.path.join(tempfile.mkdtemp(), "daemon.db")

FAILS = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        FAILS.append(what)

def tok(n): return (n.encode().hex() + "0123456789abcdef" * 8)[:64]
NAMES = ["A", "B", "C", "D"]
T = {n: tok(n) for n in NAMES}
daemon.tag = lambda t: next((n for n, x in T.items() if x == t), t[:6])

clock = [1000000.0]
logs = []
hc = hostcap.HostCap(lambda: daemon.db(), now=lambda: clock[0], log=lambda *a: logs.append(a))

# --- payload
def trailer(avg, peak, loss10, n=3):
    return bytes([1]) + avg.to_bytes(2, "little") + peak.to_bytes(2, "little") + bytes([loss10, n])
body = T["A"].encode("latin1") + bytes([1, 0, 0]) + (0).to_bytes(2, "little") + (170).to_bytes(2, "little") + bytes([0])
check(len(body) == 72 and hostcap.parse_cap(body) is None, "old ext (no trailer) parses to None")
check(hostmig.parse_perf(body + trailer(700, 900, 12)) is not None, "old daemon parser accepts the extended payload (extra bytes ignored)")
c = hostcap.parse_cap(body + trailer(700, 900, 12, 3))
check(c == {"avg_kb": 700, "peak_kb": 900, "loss10": 12, "n": 3}, "trailer decoded")
check(hostcap.parse_cap(body + bytes([2]) + bytes(6)) is None, "unknown trailer version ignored")
check(hostcap.parse_cap(body + bytes([1, 0, 0])) is None, "short trailer ignored")

# --- need
check(hostcap.need_kb(2) == 225 and hostcap.need_kb(8) == 1575 and hostcap.need_kb(1) == 0, "need = 225 KB/s per extra player")

# --- history -> verdict
check(hc.verdict(T["A"], 8)[0] == 1, "never measured = unknown (neutral rank 1)")
hc.host_report(T["A"], 8, 1700, 2100, "m1")
check(hc.verdict(T["A"], 8)[0] == 0 and hc.verdict(T["A"], 4)[0] == 0, "proven 1700 KB/s hosting 8 covers 8 and 4")
hc.host_report(T["B"], 4, 640, 700, "m2")
check(hc.verdict(T["B"], 4)[0] == 1, "640 KB/s does not prove a 4 lobby (needs 3 x 225 = 675): unproven")
check(hc.verdict(T["B"], 3)[0] == 0 and hc.verdict(T["B"], 8)[0] == 1, "640 KB/s proves a 3-player lobby, not an 8 (unproven = neutral, not lagged)")
hc.host_report(T["C"], 2, 900, 900, "m3")
check(hc.get(T["C"]) is None, "2-player hosting is no evidence (no relay)")
hc.host_report(T["C"], 5, 0, 0, "m3")
check(hc.get(T["C"]) is None, "0 KB/s sample ignored")
hc.host_report(T["B"], 4, 300, 400, "m4")
check(hc.get(T["B"])["best_kb"] == 640 and hc.get(T["B"])["last_kb"] == 300, "best only goes up; last follows")

# --- lag evidence from clients
check(not hc.client_report("m9", T["D"], T["A"], 50), "one lossy client report is not a lag")
check(not hc.client_report("m9", T["D"], T["A"], 5), "a clean report resets the streak")
check(not hc.client_report("m9", T["D"], T["A"], 50), "streak restarted")
check(hc.client_report("m9", T["D"], T["A"], 60), "2 lossy reports in a row charge the host")
check(not hc.client_report("m9", T["D"], T["B"], 60) and not hc.client_report("m9", T["D"], T["B"], 60), "once per match per host")
check(hc.get(T["D"])["lag_n"] == 1, "one lag record")
check(hc.verdict(T["D"], 8)[0] == 2, "a lagged host with no known tx level = rank 2")
hc.host_report(T["D"], 8, 2000, 2200, "m9")
check(hc.get(T["D"])["best_kb"] == 0, "a match known to lag does not raise proven capacity")
hc.host_report(T["D"], 4, 500, 520, "m10")
hc.note_lag(T["D"], "m10")
r = hc.get(T["D"])
check(r["ceil_kb"] == 500, "ceiling = tx level the host lagged at")
check(hc.verdict(T["D"], 8)[0] == 2, "lagged at 500 KB/s: an 8 lobby (need 1575) is rank 2")
check(hc.verdict(T["D"], 2)[0] in (0, 1), "a 2-player lobby needs only 225 KB/s, below 90 % of the 500 KB/s ceiling: not lagged")
clock[0] += 31 * 86400
check(hc.verdict(T["D"], 8)[0] == 1, "a lag record older than 30 days expires (back to unproven)")
clock[0] -= 31 * 86400
hc.host_report(T["C"], 8, 2000, 2100, "m11")
hc.host_report(T["C"], 8, 230, 240, "m12"); hc.note_lag(T["C"], "m12")
check(hc.verdict(T["C"], 8)[0] == 1 and "but lagged" in hc.verdict(T["C"], 8)[1], "a lag contradicted by proven capacity (2000 KB/s, ceiling 230) is neutral, not rank 2")

# --- schema v2: lag_mid persists 'lagged in this match' across a daemon restart (a restart charged match 82599fb6 twice)
check(hc.lagged_in("m12", T["C"]) and not hc.lagged_in("m11", T["C"]), "lagged_in: the match the lag belongs to")
hcr = hostcap.HostCap(lambda: daemon.db(), now=lambda: clock[0])           # a restarted daemon: empty memory
check(hcr.lagged_in("m12", T["C"]), "lagged_in survives a restart (db column lag_mid)")
n0 = hcr.get(T["C"])["lag_n"]
hcr.client_report("m12", T["C"], T["A"], 60); hcr.client_report("m12", T["C"], T["A"], 60)
check(hcr.get(T["C"])["lag_n"] == n0, "after a restart the same match is not charged again")
import sqlite3
v1 = os.path.join(tempfile.mkdtemp(), "v1.db")
c1 = sqlite3.connect(v1)
c1.execute("""CREATE TABLE host_cap(token TEXT PRIMARY KEY, hosted INT DEFAULT 0, best_kb INT DEFAULT 0, best_n INT DEFAULT 0, last_kb INT DEFAULT 0,
    lag_n INT DEFAULT 0, lag_t REAL DEFAULT 0, ceil_kb INT DEFAULT 0, last_t REAL DEFAULT 0)""")
c1.execute("INSERT INTO host_cap(token, best_kb, best_n) VALUES('old', 900, 4)"); c1.commit(); c1.close()
hv = hostcap.HostCap(lambda: sqlite3.connect(v1), now=lambda: clock[0])
g = hv.get("old")
check(g and g["best_kb"] == 900 and g["lag_mid"] is None, "a v1 table (live db) gets the lag_mid column, rows kept")
hv.note_lag("old", "mX")
check(hv.lagged_in("mX", "old"), "v1 -> v2 table records lag_mid")

# --- pick
def vd(t, n): return hc.verdict(t, n)
cands = [(T["A"], (100, 300)), (T["B"], (110, 330)), (T["C"], (120, 340)), (T["D"], (95, 290))]
ch, lines = hostcap.pick(cands, 8, vd)
check(ch == T["A"], "D has the best ping but lagged: the proven A within the window wins (%s)" % daemon.tag(ch))
ch, _ = hostcap.pick([(T["B"], (95, 290)), (T["A"], (140, 300))], 8, vd)
check(ch == T["B"], "outside the 40 ms window capacity never moves a NOT-lagged host")
ch, _ = hostcap.pick([(T["D"], (95, 290)), (T["A"], (140, 300))], 8, vd)
check(ch == T["A"], "a host that LAGGED at this size loses to a not-lagged one up to 150 ms worse (10-28)")
ch, _ = hostcap.pick([(T["D"], (95, 290)), (T["A"], (260, 300))], 8, vd)
check(ch == T["D"], "... but not to one more than 150 ms worse")
ch, _ = hostcap.pick([(T["B"], (100, 300)), (T["C"], (100, 310))], 8, vd)
check(ch == T["B"], "equal rank (both unproven) = the ping key decides")
ch, _ = hostcap.pick([(T["C"], (100, 300)), (T["A"], (110, 320))], 8, vd)
check(ch == T["A"], "unknown C vs proven A within the window: proven wins")
ch, _ = hostcap.pick([(T["D"], (100, 300)), (T["C"], (120, 320))], 8, vd)
check(ch == T["C"], "lagged D vs unknown C within the window: unknown wins")
ch, _ = hostcap.pick([(T["D"], (100, 300))], 8, vd)
check(ch == T["D"], "a lagged sole candidate still hosts")
check(hostcap.pick([], 8, vd) == (None, []), "no candidates")

# --- daemon wiring: best_host_order (log only by default, live with MODE 2)
daemon.HOSTCAP = hc
daemon.PING.clear()
players = [T["B"], T["D"], T["A"], T["C"]]
def setp(a, b, v): daemon.PING[(T[a], T[b])] = v; daemon.PING[(T[b], T[a])] = v
for a in NAMES:
    for b in NAMES:
        if a < b:
            setp(a, b, 80)
setp("D", "A", 60); setp("D", "B", 60); setp("D", "C", 60)     # D has the best worst ping (60) but lagged as an 8-host, 3 players here
daemon.PERF.by_token.clear(); daemon.PERF.by_steam.clear()
daemon.REGION.clear() if hasattr(daemon, "REGION") else None
hc2 = hostcap.HostCap(lambda: daemon.db(), now=lambda: clock[0], log=lambda *a: None)
daemon.HOSTCAP = hc2
hc2.host_report(T["D"], 4, 400, 450, "x1"); hc2.note_lag(T["D"], "x1")     # D lagged at 400 KB/s: a 4 lobby needs 675
hc2.host_report(T["A"], 4, 900, 950, "x2")                                    # A proved 900
daemon.log = lambda *a: logs.append(a)
logs.clear()
hostcap.MODE = 1
out = daemon.best_host_order(list(players))
check(out[0] == T["D"], "mode 1 (log only): the ping pick is untouched (%s)" % daemon.tag(out[0]))
check(any(l[0] == "HOST-PICK" and l[1] == "cap:" and "would pick A" in " ".join(map(str, l)) for l in logs), "mode 1 logs 'HOST-PICK cap: ... would pick A instead of D'")
hostcap.MODE = 2
out = daemon.best_host_order(list(players))
check(out[0] == T["A"], "mode 2 (live): the proven A hosts (%s)" % daemon.tag(out[0]))
hostcap.MODE = 0
logs.clear()
out = daemon.best_host_order(list(players))
check(out[0] == T["D"] and not any(l[:2] == ("HOST-PICK", "cap:") for l in logs), "mode 0: off, no cap log")
check(hostcap._envi("T2R_NO_SUCH_VAR", 2) == 2 and "T2R_HOST_CAP" not in os.environ, "(default MODE is 2 = live since 10-28)")
hostcap.MODE = 2
out = daemon.best_host_order([T["D"], T["A"]])
check(out[0] == T["A"], "2-player start (no ping pick): the player that lagged as host does not host when the other did not lag (%s)" % daemon.tag(out[0]))
out = daemon.best_host_order([T["B"], T["C"]])
check(out[0] == T["B"], "2-player start, both neutral: queue order kept")
hostcap.MODE = 1
out = daemon.best_host_order([T["D"], T["A"]])
check(out[0] == T["D"], "2-player start, mode 1: queue order kept (log only)")
# a 2-player start also scores the hosts on their pings to online players in no match (the usual join-in-progress arrivals)
hostcap.MODE = 2
_ping_bak, _pres_bak, _hc_bak = dict(daemon.PING), dict(daemon.PRES.players), daemon.HOSTCAP
class _V:                                                      # clean capacity view: D lagged as host, everyone else unknown
    def verdict(self, t, n): return (2, "lagged") if t == T["D"] else (1, "unknown")
daemon.HOSTCAP = _V()
import time as _t
daemon.PRES.players.clear()
for _n in ("A", "B", "C"):
    daemon.PRES.players[T[_n]] = {"name": _n, "last": _t.time()}
setp("B", "C", 300); setp("B", "A", 280); setp("C", "A", 150)     # B far from both; C and the idle A close
logs.clear()
out = daemon.best_host_order([T["B"], T["C"]])
check(out[0] == T["C"], "2-player start + idle online A: C (worst 300, total 450) hosts, not queue-first B (300 / 580) (%s)" % daemon.tag(out[0]))
check(any(l[:2] == ("HOST-PICK", "2 players + likely joiners") for l in logs), "logs 'HOST-PICK 2 players + likely joiners'")
del daemon.PRES.players[T["A"]]
out = daemon.best_host_order([T["B"], T["C"]])
check(out[0] == T["B"], "no idle online player: queue order kept (%s)" % daemon.tag(out[0]))
daemon.PRES.players[T["A"]] = {"name": "A", "last": _t.time()}
daemon.PING.pop((T["C"], T["A"]), None); daemon.PING.pop((T["A"], T["C"]), None)
out = daemon.best_host_order([T["B"], T["C"]])
check(out[0] == T["B"], "idle player with an unknown ping to one of them: not counted, queue order kept (%s)" % daemon.tag(out[0]))
setp("C", "A", 150); del daemon.PRES.players[T["B"]]                 # (B idle too would put C 300 ms out of the lag window)
out = daemon.best_host_order([T["D"], T["C"]])
check(out[0] == T["C"], "capacity still decides inside the window: lagged D loses to unknown C (%s)" % daemon.tag(out[0]))
daemon.PING.clear(); daemon.PING.update(_ping_bak)
daemon.PRES.players.clear(); daemon.PRES.players.update(_pres_bak)
daemon.HOSTCAP = _hc_bak
hostcap.MODE = 1

# --- daemon wiring: perf_op with trailer (host tx + client loss) needs a live match record; fakes
class FakeMM:
    def active_of(self, t): return "mid1", list(players)
class FakeHM:
    host = 0
    def snapshot(self, mid): return (0, list(players), [0, 1, 2, 3], FakeHM.host)
daemon.MM_REAL = daemon.MM
daemon.MM, daemon.HM_REAL, daemon.HM = FakeMM(), daemon.HM, FakeHM()
daemon.HOSTCAP = hc2
host = players[0]          # B hosts
rep = daemon.perf_op(host.encode("latin1") + bytes([1, 0, 0]) + bytes(2) + (170).to_bytes(2, "little") + bytes([0]) + trailer(800, 900, 0, 3))
check(rep is not None and hc2.get(host)["last_kb"] == 800, "perf_op: the host's report stores its tx (and still answers 0xCB)")
cl = players[1]
for lo in (50, 50):
    daemon.perf_op(cl.encode("latin1") + bytes([1, 0, 0]) + bytes(2) + (170).to_bytes(2, "little") + bytes([0]) + trailer(5, 8, lo, 3))
check(hc2.get(host)["lag_n"] == 1, "perf_op: a client's lossy reports twice charge the host")
rep = daemon.perf_op(cl.encode("latin1") + bytes([1, 0, 0]) + bytes(2) + (170).to_bytes(2, "little") + bytes([0]))
check(rep is not None, "perf_op: old ext payload (no trailer) still works")

# --- repick log
logs.clear()
players_ = list(players)
daemon.hostcap_repick_log("mid1", players_, [0, 1, 2, 3], 1, {0: (80, 240), 1: (70, 210), 2: (85, 250), 3: (90, 260)}, "hold", "x")
txt = " ".join(" ".join(map(str, l)) for l in logs)
check("cap: host D lagged" in txt and "would favour" in txt, "HOST-REPICK cap: a lagging host with a proven alternative in the window -> would favour (log only)")
daemon.MM, daemon.HM = daemon.MM_REAL, daemon.HM_REAL

# --- repick_tick end to end (match 82599fb6): the host saturated + lagged, slot 2 the JIP joiner not polling
import time as _time
class Clk:
    def __init__(self): self.t = _time.time()
    def __call__(self): return self.t
REAL = (daemon.HM, daemon.REPICK, daemon.HOSTCAP, daemon.log, daemon.evt, hostcap.MODE)
def setup_rt(names, mid):
    clk = Clk()
    hm = hostmig.HostMigration(True, now=clk, log=lambda *a: logs.append(a), ping=daemon.pair_ping, perf=daemon.perf_penalty, pool=daemon.host_pool)
    daemon.HM = hm
    daemon.REPICK = hostmig.HostRepick(now=clk, log=lambda *a: logs.append(a), mode=2)
    hc4 = hostcap.HostCap(lambda: daemon.db(), now=clk, log=lambda *a: None)
    daemon.HOSTCAP = hc4
    hm.start(mid, [T[n] for n in names])
    with daemon.MM.lock:
        daemon.MM.clock[mid] = (600, 900, False, _time.time())
    return clk, hm, hc4
def polls(hm, names, fl=2):
    for n in names:
        hm.poll(T[n], fl)
daemon.log = lambda *a: logs.append(a)
daemon.evt = lambda *a: logs.append(("EVT",) + a)
hostcap.MODE = 2
daemon.PING.clear()
T.update(BO=tok("BO"), MO=tok("MO"), IV=tok("IV"))
def setp2(a, b, v): daemon.PING[(T[a], T[b])] = v; daemon.PING[(T[b], T[a])] = v
clk, hm, hc4 = setup_rt(["BO", "MO", "IV"], "live1")
setp2("BO", "MO", 83); setp2("BO", "IV", 252); setp2("MO", "IV", 168)
polls(hm, ["BO", "MO", "IV"])
daemon.repick_tick()                                         # young match: only the baseline is taken
check(daemon.REPICK.base.get("live1", {}).get(frozenset((T["BO"], T["MO"]))) == 83, "repick_tick records each link's baseline every pass")
hc4.host_report(T["IV"], 3, 467, 500, "other")               # 467 KB/s was measured elsewhere
hc4.host_report(T["BO"], 3, 130, 178, "live1"); hc4.note_lag(T["BO"], "live1")
setp2("BO", "MO", 790); setp2("BO", "IV", 969)
clk.t += 130
polls(hm, ["BO", "MO"])                                      # IV (slot 2) stopped polling
logs.clear()
daemon.repick_tick()
txt = " | ".join(" ".join(map(str, l)) for l in logs)
check("not polling host migration: slot 2" in txt, "the non-polling JIP slot is logged and left out as a candidate")
check(hm.handover_pending("live1") and hm.matches["live1"]["ho"]["slot"] == 1, "the lagging host is handed over to MO (IV is proven but not polling): %s" % txt[-300:])
check(2 not in hm.matches["live1"]["ho"]["need"], "IV is not asked to prepare")
polls(hm, ["BO", "MO"], 6)
check(hm.snapshot("live1")[3] == 1, "handover committed: MO hosts")
# same with IV polling: IV (proven) is the target
clk, hm, hc4 = setup_rt(["BO", "MO", "IV"], "live2")
setp2("BO", "MO", 83); setp2("BO", "IV", 252); setp2("MO", "IV", 168)
polls(hm, ["BO", "MO", "IV"]); daemon.repick_tick()
hc4.host_report(T["BO"], 3, 130, 178, "live2"); hc4.note_lag(T["BO"], "live2")
setp2("BO", "MO", 790); setp2("BO", "IV", 969)
clk.t += 130; polls(hm, ["BO", "MO", "IV"]); daemon.repick_tick()
check(hm.handover_pending("live2") and hm.matches["live2"]["ho"]["slot"] == 2, "all polling: the PROVEN IV is the target, not the better-ping MO")
# saturation only, no lag record yet, MO unproven: 3 players -> no handover to an unproven player
clk, hm, hc4 = setup_rt(["BO", "MO", "IV"], "live3")
setp2("BO", "MO", 83); setp2("BO", "IV", 252); setp2("MO", "IV", 168)
polls(hm, ["BO", "MO", "IV"]); daemon.repick_tick()
setp2("BO", "MO", 790); setp2("BO", "IV", 969)
clk.t += 130; polls(hm, ["BO", "MO"]); daemon.repick_tick()
check(not hm.handover_pending("live3"), "saturated host, no lag yet, the only polling candidate unproven: hold")
# 2 players: saturated host, the other proven -> handover; unproven -> nothing
hc4.host_report(T["MO"], 4, 700, 720, "x9")
T["UN"] = tok("UN")
for mid, other in (("two1", "MO"), ("two2", "UN")):
    clk, hm, hc4 = setup_rt(["BO", other], mid)
    setp2("BO", other, 83)
    polls(hm, ["BO", other]); daemon.repick_tick()
    setp2("BO", other, 790)
    clk.t += 130; polls(hm, ["BO", other]); daemon.repick_tick()
    check(hm.handover_pending(mid) == (other == "MO"), "2 players, host link 83 -> 790 ms: %s" % ("other proven -> handover" if other == "MO" else "other unproven -> hold"))
# 2 players, calm link: never checked
clk, hm, hc4 = setup_rt(["BO", "MO"], "two3")
setp2("BO", "MO", 83); polls(hm, ["BO", "MO"]); daemon.repick_tick()
clk.t += 130; polls(hm, ["BO", "MO"]); logs.clear(); daemon.repick_tick()
check(not hm.handover_pending("two3") and not any(str(l[0]).startswith("REPICK-CHECK") for l in logs), "2 players, calm link: no check at all")
daemon.HM, daemon.REPICK, daemon.HOSTCAP, daemon.log, daemon.evt, hostcap.MODE = REAL

# --- db
hc3 = hostcap.HostCap(lambda: daemon.db(), now=lambda: clock[0])
check(hc3.get(T["A"]) is not None, "history persists in the db across instances")

print("\n%d failed" % len(FAILS))
sys.exit(1 if FAILS else 0)
