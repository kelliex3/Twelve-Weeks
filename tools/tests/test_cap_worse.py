"""A host that lagged in the match was kept whenever it had the lowest worst ping (REPICK_CAP_MAX_WORSE_MS was 0).
Case: the host lagged (1089 KB/s, need 1125) at a worst ping of 171 ms, a proven 1871 KB/s target sat at 172 ms, and the lagging host stayed.
Run: python tests/test_cap_worse.py"""
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
    t = 1000.0
    def __call__(self):
        return self.t

def run(scores, rank, max_rank=1, checks=2, host=0):
    clk = Clock()
    rp = hostmig.HostRepick(log=lambda *a: None, now=clk)
    cap = {"host_bad": "host lagged in this match", "rank": rank, "text": {}, "max_rank": max_rank}
    t0 = clk.t - 600
    roster = frozenset(scores)
    act = what = None
    for _ in range(checks):
        act, what = rp.check("m", t0, roster, host, scores, 300, None, None, None, cap)
        clk.t += 31
    return act, what

# as logged: cand=0:169/319 1:196/415 3:172/329 4:176/343 5:173/328 (slot 2 = the far player, not in the pool)
S = {0: (169, 319), 1: (196, 415), 3: (172, 329), 4: (176, 343), 5: (173, 328)}
R = {0: 2, 1: 1, 3: 0, 4: 1, 5: 1}
act, what = run(S, R)
check(act == "migrate" and what == 3, "lagging host hands over to the proven target 3 ms worse (got %r %r)" % (act, what))

# a target 30 ms worse is still refused (only clearly comparable targets)
S2 = {0: (120, 300), 1: (150, 340), 2: (151, 350)}
act, what = run(S2, {0: 2, 1: 0, 2: 1})
check(act == "hold", "a target 30 ms worse is refused (got %r %r)" % (act, what))

# never above the match ping limit when the host is under it
S3 = {0: (165, 300), 1: (180, 320), 2: (200, 400)}
act, what = run(S3, {0: 2, 1: 0, 2: 1})
check(act == "hold", "a target over the 175 ms match limit is refused while the host is under it (got %r %r)" % (act, what))

# host already over the limit: a target up to its own worst + 25 is fine
S4 = {0: (188, 400), 1: (182, 380), 2: (205, 420)}
act, what = run(S4, {0: 2, 1: 1, 2: 1})
check(act == "migrate" and what == 1, "host over the limit: a better target is taken (got %r %r)" % (act, what))

# a lagged (rank 2) target is never taken
S5 = {0: (150, 300), 1: (152, 310)}
act, what = run(S5, {0: 2, 1: 2})
check(act == "hold", "a target that lagged too (rank 2) is never taken (got %r %r)" % (act, what))

# still confirmed on 2 checks
act, what = run(S, R, checks=1)
check(act == "hold", "one check alone does not move the host (got %r %r)" % (act, what))

print("\n%d FAIL" % len(FAILS))
sys.exit(1 if FAILS else 0)
