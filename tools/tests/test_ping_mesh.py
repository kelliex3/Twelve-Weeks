"""Ping mesh scope (with ~100 players the server was at 100 % CPU, the all-to-all Steam location fan-out was 90 % of the daemon's messages).
A location goes only between players who can share a match; a new searcher swaps locations at once; ping_report finds the
reporter's peer by steamid in O(1). Run: python tests/test_ping_mesh.py   (temp DB, no sockets)."""
import os, sys, tempfile, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
FAILS = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: FAILS.append(what)
def tok(n):
    return ("%02x" % n) * 32
tmp = tempfile.mkdtemp()
os.environ.update(T2R_HOST="127.0.0.1", T2R_PORT="0", T2R_ADMIN_PORT="0", T2R_STATUS_FILE="0", T2R_MATCH_SIZE="8", T2R_ENGINE_SLOTS="8",
                  T2R_P2P="0", T2R_DB=os.path.join(tmp, "t2r.db"), T2R_QUEUE_SETTLE="0", T2R_FILL_WAIT="0")
os.chdir(tmp)
import daemon  # noqa: E402
daemon.DB_PATH = os.path.join(tmp, "t2r.db"); daemon.LOG_PATH = os.path.join(tmp, "daemon.log")
daemon.evt = lambda *a, **k: None
daemon.log = lambda *a, **k: None
MM = daemon.MM

class Conn:
    def __init__(self): self.sent = []
    def sendall(self, data): self.sent.append(data)
SID, CONN = {}, {}
def online(t):
    SID[t] = 76561198000000000 + len(SID)
    CONN[t] = Conn()
    daemon.BRIDGES.register(t, CONN[t], SID[t])
def got(t):
    """steamids whose location t's helper received (0xF5 frames)."""
    out = []
    for d in CONN[t].sent:
        if d[2] == 0xF5:
            out.append(int.from_bytes(d[3:11], "little"))
    return out
def clear():
    for c in CONN.values(): c.sent.clear()
def loc(n):
    return "fra=%d+3,ams=%d+5" % (20 + n, 30 + n)

A, B, C, D, E, F = [tok(i) for i in range(1, 7)]
for t in (A, B, C, D, E, F):
    online(t)
with MM.lock:
    MM.queue[A] = MM.queue[B] = time.time()            # A, B searching
    MM.matches["m1"] = [C, D]                          # C, D in a live match
# E, F: menu
for i, t in enumerate((A, B, C, D, E, F)):
    daemon.ping_location(t, loc(i))
q, live = MM.mesh_view()
check(q == {A, B} and live == {C: "m1", D: "m1"}, "mesh_view: searchers + live match players")
check(daemon.mesh_peers(E) == set() and daemon.mesh_peers(A) == {B, C, D} and daemon.mesh_peers(C) == {D, A, B},
      "peers: menu player none, searcher every searcher + in-match, in-match its mates + searchers")
check(SID[E] not in got(A) + got(B) + got(C) + got(D) + got(F), "a menu player's location reaches nobody")
check(got(E) == [] and got(F) == [], "menu helpers get no locations")
check(sorted(got(A)) == sorted([SID[B], SID[C], SID[D]]), "a searcher's helper gets the searchers' and in-match players' locations")
check(SID[C] in got(D) and SID[D] in got(C), "matchmates get each other's location")

# a menu player starts searching: one full swap at once, both directions, then nothing for MESH_ACTIVATE_S
clear()
with MM.lock:
    MM.queue[E] = time.time()
daemon.mesh_activate(E)
check(sorted(got(E)) == sorted([SID[A], SID[B], SID[C], SID[D]]), "new searcher gets every searcher / in-match location at once")
check(all(got(t).count(SID[E]) == 1 for t in (A, B, C, D)) and got(F) == [], "and they get the new searcher's location (F in menu: not)")
clear()
daemon.mesh_activate(E)
check(all(got(t) == [] for t in CONN), "a repeated FIND MATCH within %.0f s sends nothing" % daemon.MESH_ACTIVATE_S)

# unchanged location re-forward (60 s): scoped too
clear()
daemon.LOC_FWD[F] = 0
daemon.ping_location(F, loc(5))
check(all(got(t) == [] for t in CONN), "a menu player's 60 s re-forward goes nowhere")
daemon.LOC_FWD[C] = 0
daemon.ping_location(C, loc(2))
check(sorted(t for t in CONN if SID[C] in got(t)) == sorted([A, B, D, E]), "an in-match player's re-forward: its mate + the searchers")

# ping_report: O(1) steamid index, follows register / unregister
daemon.PING.clear()
daemon.ping_report(A, SID[C], 40)
check(daemon.PING.get((A, C)) and list(daemon.PING[(A, C)]) == [40], "ping_report finds the peer by steamid")
daemon.BRIDGES.unregister(C)
daemon.ping_report(A, SID[C], 41)
check(list(daemon.PING[(A, C)]) == [40], "an unregistered helper's steamid no longer resolves")
online(C)                                                # reconnect with a new steamid
daemon.ping_report(A, SID[C], 42)
check(list(daemon.PING[(A, C)]) == [40, 42], "re-registered helper resolves by its new steamid")
daemon.BRIDGES.register(C, CONN[C], SID[C] + 1000)       # same token, steamid changed: old id dropped from the index
check(daemon.BRIDGES.by_sid.get(SID[C]) is None and daemon.BRIDGES.by_sid.get(SID[C] + 1000) == C, "steamid change on re-register updates the index")

# T2R_PING_MESH=0: the old all-to-all
daemon.PING_MESH = False
clear()
daemon.PING_LOC.pop(F, None)
daemon.ping_location(F, loc(9))
check(sorted(t for t in CONN if SID[F] in got(t)) == sorted([A, B, C, D, E]), "T2R_PING_MESH=0: every helper gets it (old behaviour)")
daemon.PING_MESH = True

# load: 100 helpers, 10 searching, 40 in 8 matches, 50 in the menu; every helper sends one new location
for c in list(CONN):
    daemon.BRIDGES.unregister(c)
daemon.PING_LOC.clear(); daemon.LOC_FWD.clear(); daemon.LOC_HIST.clear(); daemon.REGION.clear()
SID.clear(); CONN.clear()
T = [tok(0x10 + i) for i in range(100)]
for t in T:
    online(t)
with MM.lock:
    MM.queue.clear(); MM.matches.clear()
    for t in T[:10]: MM.queue[t] = time.time()
    for m in range(8): MM.matches["L%d" % m] = T[10 + 5 * m: 15 + 5 * m]
def round_sends():
    clear(); daemon.PING_LOC.clear(); daemon.LOC_HIST.clear()
    for i, t in enumerate(T):
        daemon.ping_location(t, loc(i))
    return sum(len(got(t)) for t in T)
new = round_sends()
daemon.PING_MESH = False
old = round_sends()
daemon.PING_MESH = True
print("    location forwards for one round of 100 helpers: all-to-all %d, scoped %d" % (old, new))
check(old == 100 * 99 and new < old / 5, "scoped fan-out is far below all-to-all (%d vs %d)" % (new, old))

print("\n%d FAIL" % len(FAILS) if FAILS else "\nALL OK")
sys.stdout.flush()
os._exit(1 if FAILS else 0)
