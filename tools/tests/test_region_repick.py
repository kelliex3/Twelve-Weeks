"""1. The re-pick kept a host far worse than the best candidate in lobbies of 6+: a ping-only handover needed a proven target, and a host
   with lag records still counted as proven. Such a host now counts as unproven, and a target proven at least as well may take over.
2. The lobby book put a searcher into the fullest lobby within the ping limit whatever its region. Region groups (EU / NA / other) now
   stay apart until somebody waited MM_REGION_WAIT_S.
Run: python tests/test_region_repick.py"""
import os, sys, tempfile, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
FAILS = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: FAILS.append(what)
tmp = tempfile.mkdtemp()
for k in [k for k in os.environ if k.startswith("T2R_")]:
    os.environ.pop(k, None)
os.environ.update(T2R_HOST="127.0.0.1", T2R_PORT="0", T2R_ADMIN_PORT="0", T2R_STATUS_FILE="0", T2R_MATCH_SIZE="8", T2R_ENGINE_SLOTS="8",
                  T2R_P2P="0", T2R_DB=os.path.join(tmp, "t2r.db"))
import daemon, hostcap, hostmig  # noqa: E402
daemon.DB_PATH = os.path.join(tmp, "t2r.db"); daemon.LOG_PATH = os.path.join(tmp, "daemon.log")
daemon.evt = lambda *a, **k: None
daemon.log = lambda *a, **k: None
hostcap.MODE = 2
def tok(n):
    return ("%02x" % n) * 32

# ---- 1a. repick_cap: a host with lag records counts as unproven (rank 1) for the ping-only handover
SL = list(range(8))
players = {s: tok(0x40 + s) for s in SL}
inv = {t: s for s, t in players.items()}
daemon.HOSTCAP.verdict = lambda t, n: (1, "proven 3111 KB/s (need 1575) but lagged x2 at ~750 KB/s") if inv[t] == 0 else (1, "unknown")
daemon.HOSTCAP.lagged_in = lambda mid, t: False
daemon.HOSTCAP.proven_for = lambda t, n: inv[t] == 0
daemon.pair_ping = lambda a, b, raw=False, now=None: 120
daemon.REPICK.observe("m1", {(players[a], players[b]): 120 for a in SL for b in SL if a < b})
class FakeHM:
    def freezes(self, mid, slot, w):
        return 0
real_hm = daemon.HM
daemon.HM = FakeHM()
cap = daemon.repick_cap("m1", players, SL, 0)
check(bool(cap) and not cap.get("host_bad") and cap["rank"].get(0) == 1, "host proven but lagged: rank 1 in the ping-only race (%r)" % (cap and cap["rank"].get(0),))
daemon.HOSTCAP.verdict = lambda t, n: (0, "proven 3111 KB/s (need 1575)") if inv[t] == 0 else (1, "unknown")
cap = daemon.repick_cap("m1", players, SL, 0)
check(bool(cap) and cap["rank"].get(0) == 0, "a clean proven host stays rank 0")
daemon.HM = real_hm

# ---- 1b. _decide: host rank 1 + unproven target 100 ms better -> migrates (after the confirm checks); host rank 0 -> holds
clk = [100000.0]
rp = hostmig.HostRepick(now=lambda: clk[0], log=lambda *a: None, mode=2)
roster = frozenset(SL)
scores = {0: (303, 1500), 3: (197, 1100)}
for s in SL:
    scores.setdefault(s, (260, 1400))
def run(rank0):
    rp.recs.clear()
    cap = {"host_bad": None, "rank": {s: (rank0 if s == 0 else 1) for s in SL}, "max_rank": 0, "text": {}}
    acts = []
    for _ in range(3):
        clk[0] += 100
        acts.append(rp.check("m2", clk[0] - 400, roster, 0, scores, 500, cap=cap)[0])
    return acts
a = run(1)
check("migrate" in a, "lagged host 303 ms, unproven target 197 ms: migrates (%r)" % (a,))
a = run(0)
check("migrate" not in a, "clean proven host: an unproven target still may not take over (%r)" % (a,))

# ---- 2. lobby book: region groups stay apart for MM_REGION_WAIT_S
REG = {}
daemon.mm_region = lambda t: REG.get(t)
MM = daemon.MM
E = [tok(0x10 + i) for i in range(4)]; O = [tok(0x20 + i) for i in range(2)]; N = [tok(0x30 + i) for i in range(2)]
for t in E: REG[t] = "EU"
for t in O: REG[t] = "other"
REG[N[0]] = "NA-east"; REG[N[1]] = "NA-west"
now = time.time()
MM.queue.clear()
for t in E + O + N: MM.queue[t] = now - 10
check(MM._book_fits(E[:3], [O[0]]) is False, "EU lobby, fresh searcher from another region: not placed together")
check(MM._book_fits(E[:3], [E[3]]) is True, "same region: placed together")
check(MM._book_fits([N[0]], [N[1]]) is True, "NA-east and NA-west are one group")
check(MM._book_fits(E[:3], [tok(0x5a)]) is True, "unknown region: no rule")
MM.queue[E[0]] = now - daemon.MM_REGION_WAIT_S - 5
check(MM._book_fits(E[:3], [O[0]]) is True, "the lobby waited MM_REGION_WAIT_S: other regions may join")
MM.queue[E[0]] = now - 10
MM.queue[O[0]] = now - daemon.MM_REGION_WAIT_S - 5
check(MM._book_fits(E[:3], [O[0]]) is True, "the searcher waited MM_REGION_WAIT_S: it may join")
MM.queue[O[0]] = now - 10
class RQ:
    def mid_of(self, t):
        return "pm1" if t in (E[0], O[1]) else None
real_rq = MM.rq
MM.rq = RQ()
check(MM._book_fits(E[:3], [O[1]]) is True, "the post-match lobby of one match stays together")
MM.rq = real_rq
print("%d FAIL" % len(FAILS))
sys.exit(1 if FAILS else 0)
