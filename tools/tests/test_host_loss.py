"""A host whose median packet loss is at or over LOSS_KICK_PCT, and the worst of the match, for LOSS_HOST_S hands the host over to the
best player without heavy loss, before the removal.
Run: python tests/test_host_loss.py"""
import os, sys, tempfile
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
daemon.log = lambda *a, **k: LOGS.append(" ".join(str(x) for x in a))
hostcap.MODE = 2
def tok(n):
    return ("%02x" % n) * 32
P = [tok(0x40 + i) for i in range(6)]
class FakeHM:
    enabled = True
    def __init__(self):
        self.done = None; self.busy = None
    def snapshot(self, mid):
        return (0.0, P, list(range(6)), 0)
    def handover_pending(self, mid):
        return self.done is not None
    def handover_ok(self, mid):
        return (self.busy is None, self.busy or "")
    def stale_slots(self, mid):
        return set(), set()
    def handover(self, mid, slot):
        self.done = slot; return True
daemon.HM = FakeHM()
daemon.REPICK.mode = 2
daemon.REPICK.migrated = lambda mid, roster: None
# pings: slot 3 best (worst 90), slot 1 lagged host record (worst 80), slot 5 itself lossy
PG = {1: 80, 2: 140, 3: 90, 4: 120, 5: 60}
daemon.pair_ping = lambda a, b, raw=False, now=None: max(PG.get(P.index(a), 50), PG.get(P.index(b), 50))
daemon.HOSTCAP.verdict = lambda t, n: (2, "lagged x5") if t == P[1] else (1, "unknown")
daemon.perf_penalty = lambda t: 0
has = hasattr(daemon, "loss_host_handover")
med = {P[0]: (80.0, 5), P[1]: (1.0, 5), P[2]: (2.0, 5), P[3]: (0.5, 5), P[4]: (3.0, 5), P[5]: (40.0, 5)}
r = daemon.loss_host_handover("m1", P, med, 1000.0) if has else None
check(has and r is None and daemon.HM.done is None, "host at 80 % loss: first seen, no handover yet")
r = daemon.loss_host_handover("m1", P, med, 1031.0) if has else None
check(r == 3 and daemon.HM.done == 3, "after 30 s: handed over to the best player without heavy loss and no lag record (slot 3, got %r)" % (r,))
# host loss under the limit: nothing
daemon.HM = FakeHM(); daemon.LOSS_HOST_HIGH.clear() if has else None
med2 = dict(med); med2[P[0]] = (10.0, 5)
for t in (2000.0, 2100.0):
    r = daemon.loss_host_handover("m2", P, med2, t) if has else None
check(daemon.HM.done is None, "host at 10 % loss: stays")
# another player loses more than the host: that player's problem (removal path), no handover
daemon.HM = FakeHM()
med3 = dict(med); med3[P[0]] = (35.0, 5); med3[P[5]] = (60.0, 5)
for t in (3000.0, 3100.0):
    daemon.loss_host_handover("m3", P, med3, t) if has else None
check(daemon.HM.done is None, "a client loses more than the host: no handover")
# busy (silence reports / migration pending): waits, the real migration path handles it
daemon.HM = FakeHM(); daemon.HM.busy = "migration busy / host link trouble"
for t in (4000.0, 4100.0):
    daemon.loss_host_handover("m4", P, med, t) if has else None
check(daemon.HM.done is None, "migration busy: no planned handover")
# no target within the match ping limit: stays, logged once
daemon.HM = FakeHM(); LOGS.clear()
PG.update({1: 300, 2: 300, 3: 300, 4: 300})
for t in (5000.0, 5040.0, 5080.0):
    daemon.loss_host_handover("m5", P, med, t) if has else None
check(daemon.HM.done is None and sum("the host stays" in l for l in LOGS) == 1, "no target within the ping limit: host stays, logged once")
print("%d FAIL" % len(FAILS))
sys.exit(1 if FAILS else 0)
