"""A host whose link saturates is handed over even when it proved its upload earlier (an 8-player lobby: the host proven at ~1960 KB/s, then
every ping to it 80-170 -> 550-640 ms for minutes; the re-pick used to hold 'host is fine' because no 8-player target was proven).
Run: python tests/test_sat_host.py"""
import os, sys, tempfile, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
FAILS = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: FAILS.append(what)
tmp = tempfile.mkdtemp()
os.environ.update(T2R_HOST="127.0.0.1", T2R_PORT="0", T2R_ADMIN_PORT="0", T2R_STATUS_FILE="0", T2R_MATCH_SIZE="8", T2R_ENGINE_SLOTS="8",
                  T2R_P2P="0", T2R_DB=os.path.join(tmp, "t2r.db"))
import daemon  # noqa: E402
import hostcap  # noqa: E402
daemon.DB_PATH = os.path.join(tmp, "t2r.db"); daemon.LOG_PATH = os.path.join(tmp, "daemon.log")
daemon.evt = lambda *a, **k: None
hostcap.MODE = 2

def tok(n):
    return ("%02x" % n) * 32
# slot 0 = the host (proven), 1-7 = the others; their own pairs stay 60-170 ms
SLOTS = list(range(8))
players = {s: tok(0x10 + s) for s in SLOTS}
H = players[0]
RANK = {0: (0, "proven 1963 KB/s (need 1575)"), 1: (2, "lagged x3 at ~140 KB/s (need 1575)")}      # slot 1 lagged as host before
for s in range(2, 8):
    RANK[s] = (1, "unknown")
inv = {t: s for s, t in players.items()}
daemon.HOSTCAP.verdict = lambda t, n: RANK[inv[t]]
daemon.HOSTCAP.lagged_in = lambda mid, t: False
daemon.HOSTCAP.proven_for = lambda t, n: inv[t] == 0
BASE_H = {1: 81, 2: 168, 3: 138, 4: 152, 5: 82, 6: 150, 7: 142}          # host pings at the match start
NOW_H = {1: 550, 2: 640, 3: 621, 4: 643, 5: 556, 6: 635, 7: 631}         # the saturated link
OTHER = 120
state = {"h": BASE_H}
def ping(a, b):
    sa, sb = inv[a], inv[b]
    if 0 in (sa, sb):
        return state["h"][sb or sa]
    return OTHER
daemon.pair_ping = lambda a, b, raw=False, now=None: ping(a, b)
MID = "5a7e0001"
daemon.REPICK.observe(MID, {(players[a], players[b]): ping(players[a], players[b]) for a in SLOTS for b in SLOTS if a < b})

cap = daemon.repick_cap(MID, players, SLOTS, 0)
check(not cap or not cap.get("host_bad"), "healthy pings: no capacity override for the proven host (%r)" % (cap and cap.get("host_bad")))

state["h"] = NOW_H
cap = daemon.repick_cap(MID, players, SLOTS, 0)
check(bool(cap and cap.get("host_bad") and "saturated" in cap["host_bad"]), "saturated link of a PROVEN host is a bad host now: %r" % (cap and cap.get("host_bad")))
check(bool(cap) and cap.get("max_rank") == 1 and cap.get("saturated"), "targets up to rank 1 (unproven, never lagged), saturated flag set: %r" % ((cap and (cap.get("max_rank"), cap.get("saturated"))),))

def key(s):
    pings = [ping(players[s], players[o]) for o in SLOTS if o != s]
    return (max(pings), sum(pings))
scores = {s: key(s) for s in SLOTS}
check(scores[0] == (643, 4276), "host key (643/4276): %r" % (scores[0],))
t0 = time.time() - 322
act, what = daemon.REPICK.check(MID, t0, frozenset(SLOTS), 0, scores, 581, None, None, None, cap)
check(act == "migrate", "saturated host is handed over on the first check (got %r %r)" % (act, what))
check(what not in (0, 1), "target is not the host and not the lagged slot 1 (got %r)" % (what,))
best = min((s for s in range(2, 8)), key=lambda s: scores[s])
check(what == best, "target = best ping key among the not-lagged slots (%r, want %r)" % (what, best))

# SAT_TARGET_RANK 0 = the old target rule (proven only): no proven target in this lobby -> held
daemon.SAT_TARGET_RANK = 0
daemon.REPICK.forget(MID)
daemon.REPICK.observe(MID, {(players[a], players[b]): v for (a, b), v in {(0, o): BASE_H[o] for o in BASE_H}.items()})
daemon.REPICK.observe(MID, {(players[a], players[b]): OTHER for a in range(1, 8) for b in range(1, 8) if a < b})
cap0 = daemon.repick_cap(MID, players, SLOTS, 0)
act0, what0 = daemon.REPICK.check(MID, t0, frozenset(SLOTS), 0, scores, 581, None, None, None, cap0)
check(act0 == "hold", "T2R_HOST_SAT_TARGET_RANK=0: no proven target -> hold (got %r %r)" % (act0, what0))

print("\n%d failed" % len(FAILS) if FAILS else "\nall passed")
sys.exit(1 if FAILS else 0)
