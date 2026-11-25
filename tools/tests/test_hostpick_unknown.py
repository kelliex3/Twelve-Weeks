"""Host pick gaps: (1) one unknown pair ping made the pick skip every ping, and capacity chose a host 224 ms from one player while
another candidate had everyone within the 175 ms match limit; (2) on uneven teams, a host within the limit in a party on the smaller team
could not be swapped, and the bigger team's pick hosted at 276 ms - one bigger-team player now moves to the host's team instead.
Run: python tests/test_hostpick_unknown.py"""
import os, sys, tempfile, time, collections
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
import daemon, hostcap  # noqa: E402
daemon.DB_PATH = os.path.join(tmp, "t2r.db"); daemon.LOG_PATH = os.path.join(tmp, "daemon.log")
daemon.evt = lambda *a, **k: None
LOGS = []
_log = daemon.log
daemon.log = lambda *a, **k: LOGS.append(" ".join(str(x) for x in a))
def tok(n):
    return ("%02x" % n) * 32
T = [tok(0x40 + i) for i in range(8)]
def setp(a, b, ms):
    for k in ((a, b), (b, a)):
        daemon.PING[k] = collections.deque([ms], maxlen=5); daemon.PING_AT[k] = time.time() - 1.0
# the logged pings (slot order as queued): T[0] = the capacity pick (224 ms to T[1]), T[2] = within 175 to everyone but one unknown pair
P = {(0, 1): 224, (0, 2): 129, (0, 3): 40, (0, 4): 163, (0, 5): 188, (0, 6): 173, (0, 7): 118,
     (1, 2): 166, (1, 3): 219, (1, 4): 203, (1, 5): 92, (1, 6): 204, (1, 7): 165,
     (2, 3): 121, (2, 4): 90, (2, 5): 87, (2, 7): 54,                       # (2, 6) unknown
     (3, 4): 164, (3, 5): 186, (3, 6): 175, (3, 7): 122,
     (4, 5): 113, (4, 6): 35, (4, 7): 76, (5, 6): 114, (5, 7): 119, (6, 7): 77}
for (a, b), ms in P.items():
    setp(T[a], T[b], ms)
# capacity: T[0] proven, the rest unknown (the live verdicts)
hostcap.MODE = 2
orig_verdict = daemon.HOSTCAP.verdict
daemon.HOSTCAP.verdict = lambda t, n: (0, "proven") if t == T[0] else (1, "unknown")
def worst(h, order):
    return max(P.get((min(a, b), max(a, b)), 0) for a in [T.index(h)] for b in range(8) if b != a)
bo = daemon.best_host_order(list(T))
h = T.index(bo[0])
w = max(P.get((min(h, b), max(h, b)), 0) for b in range(8) if b != h)
check(h != 0, "one unknown pair: the host 224 ms from a player does not host (picked slot %d)" % h)
check(w <= 175, "the picked host has every known ping within 175 ms (worst %d)" % w)
check(sorted(bo) == sorted(T), "same players")
# nobody fits: the capacity pick stays (no worse than before)
for (a, b), ms in P.items():
    setp(T[a], T[b], ms + 100)
bo = daemon.best_host_order(list(T))
check(bo[0] == T[0], "nobody within the limit: capacity picks as before")
# 2 players: one link, unchanged
daemon.PING.clear(); daemon.PING_AT.clear()
setp(T[0], T[1], 300)
check(daemon.best_host_order([T[0], T[1]])[0] == T[0], "2 players: unchanged")

# ---- 2. uneven teams: the host's party is on the smaller team
daemon.PING.clear(); daemon.PING_AT.clear()
X, M, L, K, D = T[0], T[1], T[2], T[3], T[4]
for a, b, ms in ((K, X, 170), (K, M, 160), (K, L, 165), (K, D, 30), (D, X, 270), (D, M, 271), (D, L, 276), (X, M, 50), (X, L, 50), (M, L, 50)):
    setp(a, b, ms)
pl = [X, K, M, D, L]
sk = {X: .83, M: .59, L: .60, K: .88, D: .78}
party = {K: "p1", D: "p1"}
r = daemon.host_swap_pick(K, [X, M, L], [K, D], pl, sk, set(), [], party)
check(r is not None and K in r[0] and D in r[0] and len(r[0]) == 3, "host's party on the smaller team: one bigger-team player moves to it (%r)" % (r and [daemon.tag(x) for x in r[0]],))
check(r is not None and M in r[0], "the move that keeps the closest balance (0.59)")
# the whole bigger team is one party: nothing to move, old rule
party2 = {X: "p2", M: "p2", L: "p2"}
r = daemon.host_swap_pick(K, [X, M, L], [K, D], pl, sk, set(), [], party2)
check(r is None, "bigger team is one party: no move (old rule)")
# a plain swap still wins when it fits (no parties)
r = daemon.host_swap_pick(K, [X, M, L], [K, D], pl, sk, set(), [], {})
check(r is not None and len(r[0]) == 3 and K in r[0] and D not in r[0], "no parties: the swap as before")
# the move would grow the team gap by more than a swap may (2 x HOST_SWAP_SKILL): old rule
r = daemon.host_swap_pick(K, [X, M, L], [K, D], pl, {X: .6, M: .6, L: .6, K: .9, D: .9}, set(), [], party)
check(r is None, "a move that unbalances the teams past the swap budget: old rule")
daemon.HOSTCAP.verdict = orig_verdict
print("%d FAIL" % len(FAILS))
sys.exit(1 if FAILS else 0)
