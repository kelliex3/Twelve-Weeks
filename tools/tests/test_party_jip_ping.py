"""A party joins a running match only when every searching member passes the ping limit (the leader passed a far host, the mate was at
241-243 ms; checking only the leader sent the leader in alone and the party ended up in two matches).
Run: python tests/test_party_jip_ping.py   (temp DB, no sockets)."""
import os, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
FAILS = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: FAILS.append(what)
def tok(name):
    return (name.encode().hex() + "0123456789abcdef" * 8)[:64]
NAMES = ["P1", "P2", "P3", "P4", "Lead", "Mate", "Solo"]
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

daemon.PARTIES.invite_token(LEAD, MATE)
daemon.PARTIES.accept_token(MATE, LEAD)
check(daemon.PARTIES.pid(LEAD) and daemon.PARTIES.pid(LEAD) == daemon.PARTIES.pid(MATE), "Lead + Mate are one party")

# 1. the leader passes, the mate is at 243 ms -> nobody of the party joins, the reason names the mate
running("a1b2c3d4", T[:4])
PINGS.clear(); PINGS[MATE] = 243
STARTED.clear(); LOGS.clear(); search(LEAD, MATE)
daemon.jip_pass()
check(not STARTED, "mate over the limit: the leader is NOT sent in alone (started %r)" % STARTED)
check(not any("could not follow" in l for l in LOGS), "no 'could not follow' split")
check(any("party mate" in l and "stays together" in l for l in LOGS), "the reason names the party mate: %r" % [l for l in LOGS if "jip:" in l][:2])
check(LEAD in MM.queue and MATE in MM.queue, "both keep searching")

# 2. both pass: both join, same team
unqueue(); running("a1b2c3d4", T[:4])
PINGS.clear(); STARTED.clear(); LOGS.clear(); search(LEAD, MATE)
daemon.jip_pass()
names = sorted(s[0] for s in STARTED)
check(names == ["Lead", "Mate"], "both within the limit: both join (%r)" % STARTED)
check(len(STARTED) == 2 and STARTED[0][2] % 2 == STARTED[1][2] % 2, "same team (%r)" % STARTED)

# 3. a lone player is unchanged: its own ping decides
unqueue(); running("a1b2c3d4", T[:4])
PINGS.clear(); PINGS[MATE] = 243; STARTED.clear(); search(SOLO)
daemon.jip_pass()
check([s[0] for s in STARTED] == ["Solo"], "a player without a party joins as before (%r)" % STARTED)

# 4. unchanged: a party whose mate is not searching waits for it (PARTY-WAIT, MM.jip_ready), whatever the pings
unqueue(); running("a1b2c3d4", T[:4])
PINGS.clear(); STARTED.clear(); search(LEAD)
daemon.jip_pass()
check(not STARTED, "mate not searching: the leader waits as before (%r)" % STARTED)

print("ALL OK" if not FAILS else "%d failure(s)" % len(FAILS))
sys.exit(1 if FAILS else 0)
