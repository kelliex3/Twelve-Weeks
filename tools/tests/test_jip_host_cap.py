"""A host that lagged before at ~734 KB/s hosted a 3-player match and joins in progress filled it to 8 (needs ~1575 KB/s):
pings went to 4-5 s. A join is refused when the host's lag record says it cannot carry one more player; unknown / proven hosts are unchanged.
Run: python tools/tests/test_jip_host_cap.py   (temp DB, no sockets)."""
import os, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
FAILS = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: FAILS.append(what)
def tok(name):
    return (name.encode().hex() + "0123456789abcdef" * 8)[:64]
NAMES = ["Dyno", "Thew", "Steely", "Xaq", "Mina", "Des", "Solo"]
T = [tok(n) for n in NAMES]
H, LEAD, MATE, SOLO = T[0], T[4], T[5], T[6]
tmp = tempfile.mkdtemp()
os.environ.update(T2R_HOST="127.0.0.1", T2R_PORT="0", T2R_ADMIN_PORT="0", T2R_STATUS_FILE="0", T2R_MATCH_SIZE="8", T2R_ENGINE_SLOTS="8",
                  T2R_MIN_PLAYERS="2", T2R_JIP="1", T2R_MAP_VOTE="0", T2R_P2P="0", T2R_DB=os.path.join(tmp, "t2r.db"))
os.chdir(tmp)
import daemon  # noqa: E402
daemon.DB_PATH = os.path.join(tmp, "t2r.db"); daemon.LOG_PATH = os.path.join(tmp, "daemon.log")
daemon.evt = lambda *a, **k: None
daemon.get_name = lambda t: NAMES[T.index(t)] if t in T else None
LOGS = []
daemon.log = lambda *a, **k: LOGS.append(" ".join(str(x) for x in a))
MM = daemon.MM
PINGS = {}                                             # joiner -> ms to the host over the limit (None = passes)
daemon.jip_ping_block = lambda token, mid, host, active, joining, now: PINGS.get(token)
STARTED = []
daemon.jip_start = lambda token, mid, slot, players: STARTED.append((daemon.get_name(token), mid, slot)) or True

def running(mid, pl):
    now = time.time()
    with MM.lock:
        for m in list(MM.matches):
            MM.matches.pop(m, None)
        MM.matches[mid] = list(pl); MM.formed[mid] = now - 200; MM.launched.add(mid); MM.started.add(mid); MM.started_t[mid] = now - 150
        MM.qtime[mid] = {}; MM.departed[mid] = set()
        daemon.MATCH_MAP[frozenset(pl)] = next(iter(daemon.MAPS)) if isinstance(daemon.MAPS, dict) else 0
    for t in pl:
        MM.jip_clock(t, 400.0, 600.0, False)
def search(*ts):
    now = time.time()
    with MM.lock:
        for t in ts:
            MM.queue[t] = now; MM.seen[t] = now
def unqueue():
    with MM.lock:
        for t in (LEAD, MATE, SOLO):
            MM.queue.pop(t, None); MM.seen.pop(t, None); MM.joining.pop(t, None)


now = time.time()
daemon.HOSTCAP._upd(H, hosted=3, best_kb=734, best_n=3, last_kb=734, lag_n=2, lag_t=now - 600, ceil_kb=734, last_t=now - 600)

def joins(players, who):
    unqueue(); getattr(daemon, "_JIP_HV", {}).clear(); running("0000cafe", players)
    PINGS.clear(); STARTED.clear(); LOGS.clear(); search(who)
    daemon.jip_pass()
    return [s[0] for s in STARTED]

# 1. the lagged-host shape: 4 players on the lagged host, a 5th would need ~900 KB/s
check(joins(T[:4], SOLO) == [], "5th player is NOT sent to a host that lagged at ~734 KB/s")
check(any("cannot carry 5 players" in l for l in LOGS), "the reason names the host capacity: %r" % [l for l in LOGS if "jip" in l.lower()][:2])
check(SOLO in MM.queue, "the player keeps searching")
# 2. 3 players: a 4th needs ~675 KB/s, the host proved 734: allowed
check(joins(T[:3], SOLO) == ["Solo"], "4th player still joins (host proved 734 KB/s, need 675)")
# 3. a host without a record (unknown) is unchanged
daemon.HOSTCAP._upd(T[1], hosted=0)
check(joins([T[1], T[0], T[2], T[3], T[5]], SOLO) == ["Solo"], "unknown host: joins as before")
# 4. switch off = old behaviour
daemon.JIP_HOST_CAP = False
check(joins(T[:4], SOLO) == ["Solo"], "T2R_JIP_HOST_CAP=0: the old join (5th player sent in)")
daemon.JIP_HOST_CAP = True

print("ALL OK" if not FAILS else "%d failure(s)" % len(FAILS))
sys.exit(1 if FAILS else 0)
