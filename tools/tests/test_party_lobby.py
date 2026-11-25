"""Party split by the lobby book (a leader + 3 mates): the 3 mates were merged one by one into a lobby of 5 (the merge kept no
seats for their leader) and that lobby started with 8 while the leader, searching on an old post-match ticket, was in no lobby. Rule: a match
that takes a party member takes the whole searching party. Run: python tools/tests/test_party_lobby.py   (temp DB, no sockets)."""
import os, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
FAILS = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: FAILS.append(what)

# ================================================================ the daemon
def tok(name):
    return (name.encode().hex() + "0123456789abcdef" * 8)[:64]
NAMES = ["Ava", "Ben", "Cal", "Dan", "Eve", "Fay", "Gus", "Hal", "Ivy", "Jon", "Kim", "Lou", "Max", "Ned", "Oli", "Pat"]
T = [tok(n) for n in NAMES]
os.environ.update(T2R_HOST="127.0.0.1", T2R_PORT="0", T2R_ADMIN_PORT="0", T2R_STATUS_FILE="0", T2R_MATCH_SIZE="8", T2R_ENGINE_SLOTS="8",
                  T2R_MIN_PLAYERS="2", T2R_FILL_RULE="classic", T2R_QUEUE_SETTLE="0", T2R_FILL_WAIT="0", T2R_JIP="1", T2R_MAP_VOTE="0", T2R_P2P="0",
                  T2R_LOBBY_BOOK="1", T2R_DB=os.path.join(tempfile.mkdtemp(), "t2r.db"))
os.chdir(tempfile.mkdtemp())
import daemon  # noqa: E402
daemon.DB_PATH = os.environ["T2R_DB"]
daemon.get_name = lambda t: NAMES[T.index(t)] if t in T else None
daemon.evt = lambda *a, **k: None
LOGS = []
daemon.log = lambda *a, **k: LOGS.append(" ".join(str(x) for x in a))
daemon.player_skill = lambda toks: {t: 0.6 for t in toks}
MM = daemon.MM

def reset():
    for a in ("queue", "seen", "matches", "formed", "settle", "qtime", "departed", "clock", "polled", "joining", "departed_t", "lives", "left", "ended_at", "started_t"):
        getattr(MM, a).clear()
    MM.launched.clear(); MM.ended.clear(); MM.started.clear(); MM.last_arrival = 0.0; MM._hold_since = 0.0
    MM.rq.t.clear(); MM.rq.first.clear(); MM.rq.next.clear(); daemon.RQ_CAP.clear()
    MM.book.__init__(daemon.MATCH_SIZE, log=daemon.log)
    daemon.PARTIES.of.clear(); daemon.PARTIES.leaders.clear()
    daemon.PING.clear(); daemon.PING_AT.clear()
    for t in T: daemon.PRES.touch(t, "x")

def search(t, ago=100.0):
    now = time.time()
    MM.queue[t] = now - ago; MM.seen[t] = now; MM.settle[t] = 0
    if t in MM.rq.t: MM.rq.searching(t)              # (as MM.enqueue: a ticket holder searches again)

def finished(players, age=11.0):
    mid = "f%08x" % (abs(hash(tuple(players))) % 0xffffffff)
    now = time.time()
    MM.matches[mid] = list(players); MM.launched.add(mid); MM.started.add(mid); MM.formed[mid] = now - 600
    for t in players: daemon.RQ_CAP[t] = now
    MM.end(mid)
    MM.ended_at[mid] = now - age
    with MM.rq.lock:
        for t in players:
            if t in MM.rq.t: MM.rq.t[t][1] = now - age
    return mid


def nm(ts):
    return [NAMES[T.index(t)] for t in ts] if ts else ts

def party(ts, pid):
    for t in ts: daemon.PARTIES.of[t] = pid
    daemon.PARTIES.leaders[pid] = ts[0]

def whole(r, pt):
    """No formed match holds part of the party."""
    if not r or not r[0]:
        return True
    inn = [t for t in pt if t in r[1]]
    return not inn or len(inn) == len(pt)

OTH = T[0:5]                                          # a post-match lobby: 5 back (Ava .. Eve), Fay still on the screens
def other_lobby():
    finished(T[0:6])
    for t in OTH: search(t, ago=5)
    MM.try_form()

def release():
    """The post-match wait ends (Fay slow): the lobby may start."""
    MM.rq.release_s = 0.0
    with MM.rq.lock:
        for t in MM.rq.t: MM.rq.t[t][1] = time.time() - 200
PT = [T[8], T[9], T[10], T[11]]                       # leader Ivy, mates Jon Kim Lou

# ---- 1. the mates arrive one by one, the leader a little later
reset()
other_lobby()
party(PT, "pz")
bad = None
for i, t in enumerate(PT[1:] + PT[:1]):
    search(t, ago=10 - i)
    r = MM.try_form()
    if not whole(r, PT):
        bad = r
check(bad is None, "mates first, leader last: no match takes part of the party (%s)" % nm(bad and bad[1]))
for t in OTH + PT: MM.seen[t] = time.time()
release()
r = MM.try_form()
r_b = MM.try_form()
MM.rq.release_s = daemon.LOBBY_RELEASE_S
check(r and r[0] is not None, "the lobby of 5 starts once released (%s)" % nm(r and r[1]))
check(whole(r, PT) and whole(r_b, PT), "all searching: a match takes the whole party or none of it (%s)" % nm(r and r[1]))
check(len({MM.book.lobby_of(t) for t in PT if MM.book.lobby_of(t)}) <= 1, "the party sits in one lobby")

# ---- 2. the merge keeps seats for the incoming party's mates that are not in a lobby yet
reset()
other_lobby()
party(PT, "pz")
search(PT[1], ago=5)                                  # one mate searches; the other 3 not yet (their seats count)
MM.try_form()
check(MM.book.lobby_of(PT[1]) != MM.book.lobby_of(OTH[0]), "a mate of a party of 4 is not merged into a lobby of 5")

# ---- 3. the leader holds a post-match ticket whose lobby already formed its next match: the party still searches together
reset()
old = finished([T[8], T[12], T[13], T[14]])
nxt = "n0000001"
MM.matches[nxt] = [T[12], T[13], T[14]]; MM.launched.add(nxt); MM.started.add(nxt); MM.formed[nxt] = time.time() - 60
try:
    with MM.rq.lock:
        MM.rq.next[old] = nxt
except Exception:
    pass
for t in OTH: search(t, ago=60)
MM.try_form()
party(PT, "pz")
search(T[8], ago=8)
for t in PT[1:]: search(t, ago=5)
r1 = MM.try_form()
for t in OTH + PT: MM.seen[t] = time.time()
release()
r2 = MM.try_form()
MM.rq.release_s = daemon.LOBBY_RELEASE_S
check(whole(r1, PT) and whole(r2, PT), "leader on an old ticket: no split (%s / %s)" % (nm(r1 and r1[1]), nm(r2 and r2[1])))
check(MM.book.lobby_of(T[8]) is not None and MM.book.lobby_of(T[8]) == MM.book.lobby_of(T[9]) or (r2 and r2[0] and T[8] in r2[1]),
      "the leader is booked with its party (%s)" % MM.book.lobby_of(T[8]))

print("%d FAIL" % len(FAILS) if FAILS else "ALL OK")
sys.exit(1 if FAILS else 0)
