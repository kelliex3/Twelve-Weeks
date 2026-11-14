"""Offline tests for the planned host re-pick (hostmig.HostRepick) and the handover protocol of hostmig.HostMigration.
Run: python tools/tests/test_hostrepick.py   (plain python; no sockets, no DB)"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import hostmig  # noqa: E402

FAILS = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        FAILS.append(what)

class Clock:
    def __init__(self): self.t = 1000.0
    def __call__(self): return self.t
    def adv(self, s): self.t += s

def key(worst, total): return (worst, total)

# live evidence: player A (host, EU) 283 / 494, player B 211 / 301, player C 283 / 373
def evidence(): return {0: key(283, 494), 1: key(283, 373), 2: key(211, 301)}

CHK = []
def mk():
    clk = Clock(); LOG = []
    rp = hostmig.HostRepick(now=clk, log=lambda *a: (CHK if str(a[0]).startswith("REPICK-CHECK") else LOG).append(" ".join(map(str, a))), mode=2)
    return rp, clk, LOG
R = frozenset({0, 1, 2})
T0 = 1000.0

# --- hysteresis: needs two consecutive checks on the same target
rp, clk, LOG = mk()
clk.adv(125)
check(rp.due("m", T0), "first check due after 120 s")
a, w = rp.check("m", T0, R, 0, evidence(), 600, None)
check(a == "hold" and "would migrate" in w, "1st sample: hold, 'would migrate' logged: %s" % w)
clk.adv(90)
a, w = rp.check("m", T0, R, 0, evidence(), 500, None)
check(a == "migrate" and w == 2, "2nd consecutive sample: migrate to slot 2 (alderwood): %r" % (w,))
check(any("would migrate to 2" in l for l in LOG), "logged")
# a noisy single sample resets the streak
rp, clk, LOG = mk(); clk.adv(125)
rp.check("m", T0, R, 0, evidence(), 600)
clk.adv(90); rp.check("m", T0, R, 0, {0: key(283, 494), 1: key(283, 480), 2: key(280, 470)}, 500)   # (totals close: no total-rule gain either)
clk.adv(90); a, w = rp.check("m", T0, R, 0, evidence(), 400)
check(a == "hold", "good, bad, good: streak was reset, no migration")
# target flip resets too
rp, clk, LOG = mk(); clk.adv(125)
rp.check("m", T0, R, 0, evidence(), 600)
clk.adv(90); a, w = rp.check("m", T0, R, 0, {0: key(283, 494), 1: key(150, 200), 2: key(211, 301)}, 500)
check(a == "hold", "different best target: streak restarts")

# --- worth it: >= 40 ms AND >= 20 % lower (same totals: the total rule below stays out of it)
for cur, best, ok in ((283, 211, True), (283, 250, False), (100, 55, True), (100, 70, False), (60, 15, True), (300, 245, False), (500, 410, False)):
    rp, clk, LOG = mk(); clk.adv(125)
    sc = {0: key(cur, 400), 1: key(best, 400), 2: key(cur, 400)}
    rp.check("m", T0, R, 0, sc, 600); clk.adv(90)
    a, w = rp.check("m", T0, R, 0, sc, 500)
    check((a == "migrate") == ok, "cur %d best %d -> %s" % (cur, best, "migrate" if ok else "hold"))
# current host already best / unknown pings
rp, clk, LOG = mk(); clk.adv(125)
a, w = rp.check("m", T0, R, 2, evidence(), 600)
check(a == "hold" and "host is fine" in LOG[-1], "host already best: hold")
rp, clk, LOG = mk(); clk.adv(125)
a, w = rp.check("m", T0, R, 0, {0: key(283, 494), 1: None, 2: None}, 600)
check(a == "hold", "candidates' pings unknown: hold")
a, w = rp.check("m", T0, R, 0, {0: None, 1: key(1, 1)}, 600)
check(a == "hold", "current host's own score unknown: hold")
# stall penalty is part of the key: a stuttering best loses
sc = {0: hostmig.host_key([283, 211], 0), 2: hostmig.host_key([211, 90], 80)}
check(sc[2][0] == 291 and min(sc, key=lambda p: sc[p]) == 0, "penalty added to the worst ping")

# --- limits
rp, clk, LOG = mk()
a, w = rp.check("m", T0, R, 0, evidence(), 600)
check(a == "hold" and "young" in w, "never in the first 120 s")
clk.adv(125)
a, w = rp.check("m", T0, R, 0, evidence(), 89)
check(a == "hold" and "ends in" in w, "never in the last 90 s of the round")
a, w = rp.check("m", T0, R, 0, evidence(), None)
check(a == "hold", "clock unknown / ended: hold")
a, w = rp.check("m", T0, R, 0, evidence(), 600, "a handover is already pending")
check(a == "hold" and "pending" in w, "never while a migration is pending")

def migrate_once(rp, clk):
    clk.adv(125); rp.check("m", T0, R, 0, evidence(), 900)
    clk.adv(90); a, w = rp.check("m", T0, R, 0, evidence(), 800)
    assert a == "migrate"
    rp.migrated("m", R)
rp, clk, LOG = mk(); migrate_once(rp, clk)
clk.adv(200)
a, w = rp.check("m", T0, R, 2, {0: key(100, 100), 1: key(10, 10), 2: key(211, 301)}, 800)
check(a == "hold" and "limit" in w, "2nd migration with an unchanged roster: refused")
R2 = frozenset({0, 1, 2, 3})
sc = {0: key(100, 100), 1: key(10, 10), 2: key(211, 301), 3: key(100, 200)}
clk.adv(1); a, w = rp.check("m", T0, R2, 2, sc, 800); clk.adv(90)
a, w = rp.check("m", T0, R2, 2, sc, 700)
check(a == "migrate" and w == 1, "roster changed (JIP) and > 3 min since the last: 2nd migration allowed")
rp.migrated("m", R2)
clk.adv(400)
a, w = rp.check("m", T0, frozenset({0, 1, 2, 3, 4}), 1, {0: key(300, 1), 1: key(300, 1), 2: key(1, 1), 3: key(300, 1), 4: key(300, 1)}, 800)
check(a == "hold" and "limit" in w, "never more than 2 per match")
rp, clk, LOG = mk(); migrate_once(rp, clk); clk.adv(30)
a, w = rp.check("m", T0, R2, 2, sc, 700)
check(a == "hold" and "180" in w, "roster changed but < 3 min since the last: hold")

# --- compact log: same decision repeated = one line
rp, clk, LOG = mk(); clk.adv(125)
for _ in range(4):
    rp.check("m", T0, R, 2, evidence(), 600); clk.adv(90)
check(len(LOG) == 1, "a repeated decision is logged once (%d lines)" % len(LOG))

# --- handover protocol (HostMigration)
LOGH = []
def mkh():
    clk = Clock()
    hm = hostmig.HostMigration(True, now=clk, log=lambda *a: LOGH.append(" ".join(map(str, a))))
    return hm, clk
A, B, C = ["%064x" % i for i in (1, 2, 3)]
def reply(b): return dict(zip(("enabled", "host", "epoch", "alive", "state"), b))
hm, clk = mkh(); hm.start("m", [A, B, C])
for t in (A, B, C): hm.poll(t, 0)
ok, why = hm.handover_ok("m")
check(not ok and "support" in why, "no handover while an ext has not announced support (old ext): %s" % why)
clk.adv(30)
for t in (A, B, C): hm.poll(t, 2)
check(hm.handover_ok("m")[0], "all exts flagged support: ok")
check(hm.handover("m", 2), "handover to slot 2 started")
check(reply(hm.poll(B, 2))["state"] == hostmig.ST_HANDOVER, "players are told state 3 (prepare)")
check(hm.snapshot("m")[3] == 0, "host unchanged during the prepare phase")
hm.poll(A, 2 | 4); hm.poll(B, 2 | 4)
check(hm.snapshot("m")[3] == 0, "not committed until everyone is prepared")
r = reply(hm.poll(C, 2 | 4))
check(r["host"] == 2 and r["epoch"] == 1 and r["alive"] == 0b111 and r["state"] == 0, "all prepared: epoch 1, host slot 2, the old host still alive: %r" % r)
r = reply(hm.poll(A, 2, 0))
check(r["host"] == 2 and r["alive"] == 0b111 and r["state"] == 0, "old host is told the new epoch, not 'give up'")
check(hm.host_token(A) == C, "daemon-side host token follows")
check(not hm.handover("m", 1), "no second handover right after")
# abort: someone never prepares
hm, clk = mkh(); hm.start("m", [A, B, C]); clk.adv(30)
for t in (A, B, C): hm.poll(t, 2)
hm.handover("m", 1)
for t in (A, B): hm.poll(t, 6)
clk.adv(31); r = reply(hm.poll(A, 6))
check(r["state"] == 0 and r["epoch"] == 0 and not hm.handover_pending("m") and "aborted" in LOGH[-1], "timeout: aborted, no migration, state back to normal")
# abort: roster changed (JIP) / host lost during prepare
hm, clk = mkh(); hm.start("m", [A, B, C]); clk.adv(30)
for t in (A, B, C): hm.poll(t, 2)
hm.handover("m", 1)
hm.add_player("m", "%064x" % 9, 3)
hm.poll(A, 6)
check(not hm.handover_pending("m") and hm.snapshot("m")[3] == 0, "roster change during prepare aborts")
hm, clk = mkh(); hm.start("m", [A, B, C]); clk.adv(30)
for t in (A, B, C): hm.poll(t, 2)
hm.handover("m", 1)
hm.depart(A, "left")
check(hm.snapshot("m")[3] in (1, 2) and not hm.handover_pending("m"), "host leaves during prepare: the normal host-loss migration wins, handover gone")

# --- capacity override (match 82599fb6): the host (slot 0, ~1 Mbit) saturated: 0-1 83 -> 746, 0-2 252 -> 905, 1-2 168 calm.
# The ping rule alone holds (746 vs 905 is < 20 %); hostcap says the host lagged in this match.
LIVE = {0: key(905, 1651), 1: key(746, 914), 2: key(905, 1073)}
def capd(bad="BOURADI lagged", rank=None, mx=1):
    return {"host_bad": bad, "rank": rank if rank is not None else {0: 2, 1: 1, 2: 0}, "max_rank": mx}
rp, clk, LOG = mk(); clk.adv(125)
a, w = rp.check("m", T0, R, 0, LIVE, 600)
check(a == "hold", "live numbers, no capacity info: the ping rule holds (%s)" % w)
rp, clk, LOG = mk(); clk.adv(125)
a, w = rp.check("m", T0, R, 0, LIVE, 600, None, None, None, capd())
check(a == "migrate" and w == 2, "lagging host: migrate at once to the PROVEN slot 2 although slot 1 has the better ping (%r)" % (w,))
check(any("lagged" in l and "migrate to 2" in l for l in LOG), "the override is logged: %s" % LOG[-1:])
rp, clk, LOG = mk(); clk.adv(125)
a, w = rp.check("m", T0, R, 0, LIVE, 600, None, None, None, capd(rank={0: 2, 1: 1, 2: 1}))
check(a == "migrate" and w == 1, "no proven candidate: the best ping among the not-lagged ones (slot 1): %r" % (w,))
rp, clk, LOG = mk(); clk.adv(125)
a, w = rp.check("m", T0, R, 0, LIVE, 600, None, None, None, capd(rank={0: 2, 1: 2, 2: 2}))
check(a == "hold", "every candidate lagged too: no override, the ping rule decides (hold)")
rp, clk, LOG = mk(); clk.adv(125)
a, w = rp.check("m", T0, R, 0, {0: key(200, 300), 1: key(400, 500), 2: key(360, 450)}, 600, None, None, None, capd(rank={0: 2, 1: 0, 2: 1}))
check(a == "hold", "override never picks a target > 150 ms worse than the host (%s)" % w)
rp, clk, LOG = mk(); clk.adv(125)
a, w = rp.check("m", T0, R, 0, {0: None, 1: key(746, 914), 2: None}, 600, None, None, None, capd())
check(a == "migrate" and w == 1, "host's own score unknown (a peer not measured): still replaced by the known not-lagged slot 1")
rp, clk, LOG = mk(); clk.adv(125)
a, w = rp.check("m", T0, R, 0, LIVE, 60, None, None, None, capd())
check(a == "hold" and "ends in" in w, "override keeps the last-90-s guard")
a, w = rp.check("m", T0, R, 0, LIVE, 600, "a handover is already pending", None, None, capd())
check(a == "hold", "override keeps the pending guard")
rp, clk, LOG = mk(); clk.adv(125)
check(rp.check("m", T0, R, 0, LIVE, 600, None, None, None, capd())[0] == "migrate", "1st override")
rp.migrated("m", R); clk.adv(100)
a, w = rp.check("m", T0, R, 2, {0: key(905, 1651), 1: key(168, 300), 2: key(905, 1073)}, 600, None, None, None, capd(rank={0: 1, 1: 1, 2: 2}))
check(a == "hold" and ("limit" in w or "180" in w), "one handover per N min: a 2nd override right after is refused (%s)" % w)
# 2 players: only the override (to a PROVEN player) moves a host
R2P = frozenset({0, 1})
rp, clk, LOG = mk(); clk.adv(125)
a, w = rp.check("m", T0, R2P, 0, {0: key(790, 790), 1: key(700, 700)}, 600)
check(a == "hold", "2 players without capacity info: never moved (%s)" % w)
rp, clk, LOG = mk(); clk.adv(125)
a, w = rp.check("m", T0, R2P, 0, {0: key(790, 790), 1: key(790, 790)}, 600, None, None, None, capd("X link saturated", {0: 1, 1: 0}, 0))
check(a == "migrate" and w == 1, "2 players, saturated host, the other proven: handover (%r)" % (w,))
rp, clk, LOG = mk(); clk.adv(125)
a, w = rp.check("m", T0, R2P, 0, {0: key(790, 790), 1: key(790, 790)}, 600, None, None, None, capd("X link saturated", {0: 1, 1: 1}, 0))
check(a == "hold", "2 players, saturated host, the other unproven: hold")
# saturation detector (baseline = lowest ping per pair in this match)
rp, clk, LOG = mk()
P = {}
def pf(a, b): return P.get(frozenset((a, b)))
def setP(**kv):
    for k, v in kv.items():
        P[frozenset(k)] = v
setP(BM=83, BI=252, MI=168)
rp.observe("m", {("B", "M"): 83, ("B", "I"): 252, ("M", "I"): 168})
check(not rp.saturated("m", "B", ["M", "I"], pf)[0], "baseline pings: not saturated")
setP(BM=790, BI=969)
sat, txt = rp.saturated("m", "B", ["M", "I"], pf)
check(sat and "83->790" in txt, "every ping to the host >= 3x + 150 ms, M-I calm: saturated (%s)" % txt)
setP(MI=600)
check(not rp.saturated("m", "B", ["M", "I"], pf)[0], "every pair high (not only the host's): not blamed on the host")
setP(MI=168, BI=400)
check(not rp.saturated("m", "B", ["M", "I"], pf)[0], "one host pair only 1.6x: not saturated")
rp.observe("m", {("B", "M"): 790})
check(rp.base["m"][frozenset("BM")] == 83, "the baseline keeps the lowest ping")
check(rp.saturated("m", "B", ["M"], pf)[0], "2 players: 83 -> 790 saturated")
setP(BM=200)
check(not rp.saturated("m", "B", ["M"], pf)[0], "2 players: 83 -> 200 (2.4x) not saturated")
check(not rp.saturated("x", "B", ["M"], pf)[0], "no baseline: not saturated")
rp.forget("m")
check("m" not in rp.base, "forget drops the baseline")

# --- non-polling slots ('slot 2 not polling' held the whole re-pick)
hm, clk = mkh(); hm.start("m", [A, B, C]); clk.adv(30)
hm.poll(A, 2); hm.poll(B, 2)                            # C (slot 2) never polls
check(hm.stale_slots("m") == ({2}, set()), "C never polled 30 s after the start: stale")
ok, why = hm.handover_ok("m")
check(ok, "a stale slot no longer blocks a handover (%s)" % why)
check(not hm.handover("m", 2), "a stale slot is never the target")
check(hm.handover("m", 1), "handover to B starts")
hm.poll(A, 6)
r = reply(hm.poll(B, 6))
check(r["host"] == 1 and r["epoch"] == 1, "committed once every POLLING player prepared (C left out): %r" % r)
r = reply(hm.poll(C, 2, 0))
check(r["host"] == 1 and r["epoch"] == 1 and r["state"] == 0, "C follows the new epoch on its next poll: %r" % r)
hm, clk = mkh(); hm.start("m", [A, B, C]); clk.adv(30)
for t in (B, C): hm.poll(t, 2)
ok, why = hm.handover_ok("m")
check(not ok and "host" in why, "the host itself not polling: no handover (%s)" % why)
hm, clk = mkh(); hm.start("m", [A, B]); clk.adv(30)
hm.poll(A, 2)
ok, why = hm.handover_ok("m")
check(not ok, "only the host polling: nobody to hand over to (%s)" % why)
# a join in progress that is still loading holds briefly, then counts as stale
D = "%064x" % 4
hm, clk = mkh(); hm.start("m", [A, B, C]); clk.adv(60)
for t in (A, B, C): hm.poll(t, 2)
hm.add_player("m", D, 3)
clk.adv(30)
for t in (A, B, C): hm.poll(t, 2)
ok, why = hm.handover_ok("m")
check(not ok and "loading" in why, "a joiner still loading (30 s, never polled): hold (%s)" % why)
check(hostmig._guard_code(why) == "loading", "guard code 'loading'")
hm.poll(D, 2)
check(hm.handover_ok("m")[0], "the joiner polls: ok")
hm, clk = mkh(); hm.start("m", [A, B, C]); clk.adv(60)
hm.add_player("m", D, 3)
for _ in range(7):
    clk.adv(20)
    for t in (A, B, C): hm.poll(t, 2)
check(hm.stale_slots("m") == ({3}, set()) and hm.handover_ok("m")[0], "a joiner silent > 120 s after its join: stale, no longer holds")
# daemon restart: nobody polled yet -> not stale for 25 s after the re-adoption
hm, clk = mkh(); hm.start("m", [A, B, C]); clk.adv(200)
for t in (A, B, C): hm.poll(t, 2)
rec = hm.export_match("m")
hm2, clk2 = mkh(); clk2.t = clk.t
hm2.import_match("m", rec)
check(hm2.stale_slots("m") == (set(), set()), "just re-adopted: no slot stale yet")
clk2.adv(26); hm2.poll(A, 2); hm2.poll(B, 2)
check(hm2.stale_slots("m") == ({2}, set()), "26 s after the re-adoption the silent slot is stale")

# --- calibration: real match-start candidate sets (anonymised: (worst ms, total ms) per candidate, from live logs).
# Runner-up gaps are tiny (median ~10 ms): a re-pick on the same numbers must almost never fire.
REAL = [((185,456),(196,478)), ((131,287),(156,314)), ((130,283),(157,308)), ((306,717),(312,710)), ((151,190),(160,199)),
        ((124,303),(154,316),(188,394)), ((99,134),(115,150)), ((128,436),(131,423),(139,445)), ((202,252),(221,271)),
        ((127,376),(138,389)), ((122,377),(137,391)), ((112,251),(115,246),(198,419)), ((119,419),(123,429),(130,427),(215,758)),
        ((128,562),(128,563),(138,568),(225,999)), ((198,404),(217,406),(223,428)), ((121,261),(131,285)), ((128,434),(132,448),(139,464)),
        ((109,302),(123,314),(133,330)), ((44,69),(54,79),(54,98)), ((155,195),(157,197)), ((303,506),(307,504)), ((287,422),(306,441)),
        ((289,419),(304,434)), ((288,416),(301,429)), ((194,436),(194,438))]
def would(cs, hi):
    sc = {i: c for i, c in enumerate(cs)}
    rp, clk, LOG = mk(); clk.adv(125)
    rp.check("m", T0, frozenset(sc), hi, sc, 600); clk.adv(90)
    return rp.check("m", T0, frozenset(sc), hi, sc, 500)[0] == "migrate"
fired = sum(would(cs, hi) for cs in REAL for hi in range(len(cs)))
total = sum(len(c) for c in REAL)
check(fired <= 4, "real match-start sets, every candidate as host: %d of %d what-ifs fire (only a truly bad host)" % (fired, total))
nf = sum(would(cs, min(range(len(cs)), key=lambda i: cs[i])) for cs in REAL)
check(nf == 0, "best host in place: 0 migrations on %d real lobbies" % len(REAL))
sc = {0: key(200, 400), 1: key(120, 450), 2: key(210, 500)}
rp, clk, LOG = mk(); clk.adv(125); rp.check("m", T0, R, 0, sc, 600); clk.adv(90)
check(rp.check("m", T0, R, 0, sc, 500)[0] == "hold", "better worst but worse total: hold")
check(hostmig.REPICK_MIN_AGE == 120 and hostmig.REPICK_MIN_LEFT == 90, "defaults: min age 120 s, min left 90 s")

# --- REPICK-CHECK log line: one compact line per check, parseable by tools/reports/repick_stats.py
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "reports"))
import repick_stats  # noqa: E402
def one(sc, left, info=None, host=0, steps=1):
    del CHK[:]
    rp, clk, _ = mk(); clk.adv(125)
    for i in range(steps):
        if i: clk.adv(90)
        rp.check("m1", T0, R, host, sc, left, None, None, info)
    return list(CHK)
info = {"rtt": {(0, 1): 84, (0, 2): 120, (1, 2): 60}, "perf": {0: 40, 1: 0, 2: 0}, "loss": {1: 2}, "age": 31}
c = one(evidence(), 600, info)
check(len(c) == 1 and "\n" not in c[0], "one REPICK-CHECK line per check")
check(c[0].startswith("REPICK-CHECK m=m1 age=125 left=600 host=0 cand=0:283/494/p40 1:283/373/l2 2:211/301 rtt 0-1:84 0-2:120 1-2:60 rttage=31 dec=would tgt=2 gain=72 n=1 why=confirm"), "line format: " + (c[0] if c else ""))
check(len(c[0]) < 220, "line is short (%d chars)" % len(c[0]))
c = one(evidence(), 30)
check("dec=stay" in c[0] and "why=endgame" in c[0], "endgame guard named: " + c[0])
c = one({0: key(100, 300), 1: key(90, 290), 2: key(95, 320)}, 600)
check("dec=stay" in c[0] and "why=small" in c[0], "small gain: stay/small: " + c[0])
c = one({0: key(283, 494), 1: key(150, 600), 2: key(220, 700)}, 600)
check("why=total" in c[0] and "tgt=1" in c[0], "total guard: " + c[0])
c = one({0: key(100, 300), 1: None, 2: key(95, 320)}, 600)
check("1:?" in c[0] and "dec=stay" in c[0], "unknown candidate prints ?")
c = one(evidence(), 500, info, steps=2)
check("dec=migrate tgt=2 gain=72 n=2 why=go" in c[1], "2nd confirmation -> migrate: " + c[1])
# stats
r = repick_stats.parse_line("[10:00:00] REPICK-CHECK " + c[1][len("REPICK-CHECK "):])
check(r and r["dec"] == "migrate" and r["gain"] == 72 and r["rtt"]["0-2"] == 120 and r["tgt"] == "2", "repick_stats parses the line")
def syn(mid, rows):
    return ["[10:00:00] REPICK-CHECK m=%s age=%d left=500 host=0 cand=0:200/300 1:100/250 rtt 0-1:%d dec=%s why=%s" % (mid, 120 + 90 * i, rt, d, w) for i, (rt, d, w) in enumerate(rows)]
L = []
for i in range(12):
    L += syn("s%d" % i, [(80 + i, "stay", "best"), (82 + i, "stay", "best")])
res = repick_stats.analyse(L); v = repick_stats.verdict(res)
check(res["matches"] == 12 and res["checks"] == 24 and v["b"] and v["a"] and v["c"] and v["all"], "synthetic quiet matches: all criteria met")
check(res["rtt_deltas"] == [2] * 12, "rtt variance between consecutive checks")
L2 = L + syn("w1", [(80, "would tgt=2 gain=70 n=1", "confirm"), (80, "migrate tgt=2 gain=70 n=2", "go")])
v = repick_stats.verdict(repick_stats.analyse(L2))
check(v["all"] and abs(v["rate"] - 1 / 13) < 1e-9, "one stable would-migrate in 13 matches (gain 70): still ok")
L3 = L + syn("w1", [(80, "would tgt=2 gain=50 n=1", "confirm"), (80, "would tgt=1 gain=70 n=1", "confirm")])
r3 = repick_stats.analyse(L3); v = repick_stats.verdict(r3)
check(r3["flaps"] == 1 and not v["a"] and not v["c"] and not v["all"], "flapping + small gain: criteria not met")
L4 = []
for i in range(5): L4 += syn("x%d" % i, [(80, "would tgt=1 gain=90 n=1", "confirm")])
check(not repick_stats.verdict(repick_stats.analyse(L4))["all"], "rate >= 0.2 / too few matches: not enable")
check("verdict:" in repick_stats.report(repick_stats.analyse(L)) and repick_stats.analyse([])["matches"] == 0, "report prints, empty log ok")
import tempfile
fd, tp = tempfile.mkstemp(suffix=".log"); os.close(fd)
open(tp, "w").write("\n".join(L2) + "\nother line\n")
check(repick_stats.main([tp]) == 0, "main reads a log file"); os.remove(tp)



# --- total rule: a host in South America with two players in Europe. Worst 315 -> 274 (13 %) failed the worst rule; the
# total 589 -> 429 drops 160 ms (27 %) with a worst that is not worse: migrate. The confirm comes REPICK_CONFIRM_GAP_S later, not a full interval.
for cur, best, ok, what in (((315, 589), (274, 429), True, "01660bc0: 315/589 -> 274/429"),
                            ((315, 589), (315, 520), False, "same worst, total -69 ms"),
                            ((315, 589), (330, 300), False, "worst gets worse, big total gain"),
                            ((300, 1000), (300, 880), False, "total -120 ms but only 12 %")):
    rp, clk, LOG = mk(); clk.adv(125)
    sc = {0: key(*cur), 1: key(*best), 2: key(cur[0], cur[1] + 50)}
    a1, w1 = rp.check("m", T0, R, 0, sc, 600)
    clk.adv(hostmig.REPICK_CONFIRM_GAP_S + 1)
    due2 = rp.due("m", T0)
    a, w = rp.check("m", T0, R, 0, sc, 570)
    good = (a == "migrate") == ok and (not ok or (due2 and "total rule" in str(w1)))
    check(good, "total rule: %s -> %s (confirm due after %d s: %s)" % (what, a, hostmig.REPICK_CONFIRM_GAP_S, due2))
# a check without 'would migrate' keeps the full interval
rp, clk, LOG = mk(); clk.adv(125)
rp.check("m", T0, R, 0, {0: key(200, 400), 1: key(200, 400), 2: key(200, 400)}, 600)
clk.adv(hostmig.REPICK_CONFIRM_GAP_S + 1)
check(not rp.due("m", T0), "no 'would migrate': next check only after the full %d s interval" % hostmig.REPICK_INTERVAL)
# 2 players: one link - neither rule moves the host
rp, clk, LOG = mk(); clk.adv(125)
a, w = rp.check("m", T0, frozenset({0, 1}), 0, {0: key(315, 315), 1: key(100, 100)}, 600)
check(a == "hold", "2 players: hold")

print("\n%d failure(s)" % len(FAILS))
sys.exit(1 if FAILS else 0)
