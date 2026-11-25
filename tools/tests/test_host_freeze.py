"""Two re-pick gaps: (1) a host that froze for every player again and again (each freeze shorter than the min_silent wait) was never
a bad host; (2) one player with no ping to anybody (no Steam helper data) made every key unknown, in the re-pick and in the successor pick.
Run: python tests/test_host_freeze.py"""
import os, sys, tempfile
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
import hostmig  # noqa: E402
daemon.DB_PATH = os.path.join(tmp, "t2r.db"); daemon.LOG_PATH = os.path.join(tmp, "daemon.log")
daemon.evt = lambda *a, **k: None
hostcap.MODE = 2

class Clock:
    t = 10000.0
    def __call__(self):
        return self.t
clk = Clock()

def tok(n):
    return ("%02x" % n) * 32

# ---- 1. freeze episodes are counted per host (hostmig)
hm = hostmig.HostMigration(enabled=True, now=clk, log=lambda *a: None, link_alive=lambda t: True)
P = [tok(0x20 + i) for i in range(5)]
hm.start("m1", P)
for t in P:
    hm.by_token[t] = "m1"
clk.t += 60
def freeze():
    for t in P:
        hm.poll(t, 0, 0)                  # everyone hears the host
    for t in P[1:]:
        hm.poll(t, 1, 0)                  # every client reports it silent (host still polls: link up)
    clk.t += 1
    hm.poll(P[0], 0, 0)
    for t in P[1:]:
        hm.poll(t, 0, 0)                  # back after 1 s (shorter than min_silent): no host loss
    clk.t += 60
has = hasattr(hm, "freezes")
for _ in range(3):
    freeze()
check(has and hm.freezes("m1", 0, 300) == 3, "3 short freezes of the host are counted (%r)" % (hm.freezes("m1", 0, 300) if has else "no freezes()",))
check(has and hm.snapshot("m1")[3] == 0, "short freezes alone never replace the host")
clk.t += 400
check(has and hm.freezes("m1", 0, 300) == 0, "freezes older than the window do not count")
check(has and hm.freezes("m1", 1, 300) == 0, "another slot has no freezes")

# ---- 2. the re-pick treats a host that froze 3 times as a bad host (daemon.repick_cap)
SL = list(range(5))
players = {s: tok(0x40 + s) for s in SL}
inv = {t: s for s, t in players.items()}
daemon.HOSTCAP.verdict = lambda t, n: (0, "proven 2193 KB/s (need 900)") if inv[t] == 0 else (1, "unknown")
daemon.HOSTCAP.lagged_in = lambda mid, t: False
daemon.HOSTCAP.proven_for = lambda t, n: inv[t] == 0
daemon.pair_ping = lambda a, b, raw=False, now=None: 140 if 0 in (inv[a], inv[b]) else 60
daemon.REPICK.observe("m1", {(players[a], players[b]): 140 if 0 in (a, b) else 60 for a in SL for b in SL if a < b})
real_hm = daemon.HM
class FakeHM:
    n = 0
    def freezes(self, mid, slot, w):
        return self.n if slot == 0 else 0
daemon.HM = FakeHM()
cap = daemon.repick_cap("m1", players, SL, 0)
check(not cap or not cap.get("host_bad"), "no freezes: no capacity override (%r)" % (cap and cap.get("host_bad"),))
daemon.HM.n = 2
cap = daemon.repick_cap("m1", players, SL, 0)
check(not cap or not cap.get("host_bad"), "2 freezes: still no override")
daemon.HM.n = 3
cap = daemon.repick_cap("m1", players, SL, 0)
check(bool(cap and cap.get("host_bad") and "froze" in cap["host_bad"]), "3 freezes in 5 min: the host is a bad host (%r)" % (cap and cap.get("host_bad"),))
check(bool(cap) and cap.get("max_rank") == 1 and not cap.get("saturated"), "targets up to rank 1, confirmed on 2 checks (not the saturated fast path)")
daemon.HM = real_hm

# ---- 3. one player without any ping no longer blocks the re-pick (daemon.repick_tick)
calls = []
real_check = daemon.REPICK.check
def spy(mid, t0, roster, host, scores, left, busy=None, labels=None, info=None, cap=None):
    calls.append(dict(scores))
    return "hold", "spy"
daemon.REPICK.check = spy
daemon.REPICK.mode = 2
pl = [tok(0x60 + i) for i in range(5)]
ix = {t: i for i, t in enumerate(pl)}
def ping3(a, b, raw=False, now=None):
    if 4 in (ix[a], ix[b]):
        return None                       # slot 4: no ping data at all
    return 50 + 10 * (ix[a] + ix[b])
daemon.pair_ping = ping3
daemon.repick_cap = lambda *a: None
daemon.host_pool = lambda toks: (list(toks), "")
daemon.HM = hostmig.HostMigration(enabled=True, now=clk, log=lambda *a: None)
daemon.HM.start("m2", pl)
for t in pl:
    daemon.HM.by_token[t] = "m2"
    daemon.HM.poll(t, 2, 0)
daemon.REPICK.due = lambda mid, t0: True
daemon.repick_tick()
sc = calls[-1] if calls else {}
check(bool(sc) and sc.get(0) is not None, "the host gets a ping key although slot 4 has no ping data (%r)" % (sc,))
check(bool(sc) and all(sc.get(s) is not None for s in (1, 2, 3)), "the other candidates get keys too")
check(bool(sc) and 4 not in sc, "the player without ping data is not a candidate")
# a player who is merely missing ONE ping (not blind) still makes keys unknown, as before
def ping4(a, b, raw=False, now=None):
    if {ix[a], ix[b]} == {1, 2}:
        return None
    return 80
daemon.pair_ping = ping4
calls.clear()
daemon.repick_tick()
sc = calls[-1] if calls else {}
check(bool(sc) and sc.get(1) is None and sc.get(2) is None, "a single missing pair still counts as unknown for those two (%r)" % (sc,))
daemon.REPICK.check = real_check

# ---- 4. the successor after a host loss: a survivor without ping data no longer makes every key unknown (hostmig._pick)
names = ["host", "p1", "p2", "p3", "p4"]
PP = {("p1", "p2"): 213, ("p1", "p4"): 143, ("p2", "p4"): 104}
def ping5(a, b):
    return PP.get((a, b)) or PP.get((b, a))
hm5 = hostmig.HostMigration(enabled=True, now=clk, log=lambda *a: None, ping=ping5)
hm5.start("m1", names)
new = hm5._pick(hm5.matches["m1"], [1, 2, 3, 4])
check(new == 4, "successor = slot 4 (worst 143 ms), not slot 2 (213 ms to slot 1) (got slot %r)" % (new,))
new = hm5._pick(hm5.matches["m1"], [1, 2, 4])
check(new == 4, "same pick without the blind player (got slot %r)" % (new,))
new = hm5._pick(hm5.matches["m1"], [2, 3])
check(new == 2, "two candidates, one blind: lowest slot as before (got slot %r)" % (new,))

print("\n%d FAIL" % len(FAILS))
sys.exit(1 if FAILS else 0)
