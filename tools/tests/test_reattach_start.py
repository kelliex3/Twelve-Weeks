"""A session socket that died on the loading screen missed the match's GAME-START (0x0a); after the re-attach the client waited
for it forever. The start is re-sent to the re-attached socket only when it went out while the socket was down.
Run: python tools/tests/test_reattach_start.py"""
import os, socket, sys, time
import sys as _t2s, os as _t2o; _t2s.path.append(_t2o.path.normpath(_t2o.path.join(_t2o.path.dirname(_t2o.path.abspath(__file__)), ".."))); import t2rtmp  # noqa: E402,F401
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
FAILS = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        FAILS.append(what)
s = socket.socket(); s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]; s.close()
os.environ.update(T2R_HOST="127.0.0.1", T2R_PORT=str(port))
import daemon  # noqa: E402
daemon.update_target = lambda: None
daemon.REATTACH_START_DELAY_S = 0.05
GS = daemon.msg(0x0a, b"\x01")

class Conn:
    def __init__(self):
        self.out = []
    def sendall(self, b):
        self.out.append(bytes(b))
    def starts(self):
        return sum(1 for b in self.out if b == GS)

def tok(n):
    return "54324e3a" + "4a6970" + "00" * 13 + ("%024x" % (0x4000 + n))

def match(mid, players, started_at=None):
    with daemon.MM.lock:
        daemon.MM.matches[mid] = list(players)
        daemon.MM.launched.add(mid)
        if started_at is not None:
            daemon.MM.started.add(mid); daemon.MM.started_t[mid] = started_at

def reattach(t, lost_at, mid):
    old = Conn(); daemon.SESSIONS.register(t, old)
    daemon.SESS_LOST[t] = (lost_at, mid)
    c = Conn()
    st = {"token": t, "name": "p"}
    ok = daemon.session_reattach(c, ("127.0.0.1", 1), st)
    time.sleep(0.3)
    return ok, c

now = time.time()
# 1: lost 11 s ago, the match started 8 s ago (while lost): re-sent once
a, b = tok(1), tok(2)
match("e1", [b, a], started_at=now - 8)
ok, c = reattach(a, now - 11, "e1")
check(ok, "re-attach accepted")
check(c.starts() == 1, "GAME-START re-sent once to a socket lost before the start (%d)" % c.starts())

# 2: lost after the start (mid-match drop): never re-sent
a, b = tok(3), tok(4)
match("e2", [b, a], started_at=now - 300)
ok, c = reattach(a, now - 10, "e2")
check(ok and c.starts() == 0, "no GAME-START for a socket lost mid-match (%d)" % c.starts())

# 3: lost on the loading screen, match not started yet: nothing now (maybe_game_start sends it later)
a, b = tok(5), tok(6)
match("e3", [b, a])
ok, c = reattach(a, now - 5, "e3")
check(ok and c.starts() == 0, "no GAME-START while the match has not started (%d)" % c.starts())

# 4: a joiner whose own GAME-START went out while it was lost (the match started long ago): re-sent
a, b = tok(7), tok(8)
match("e4", [b, a], started_at=now - 400)
daemon.JIP_STARTED[a] = ("e4", now - 4)
ok, c = reattach(a, now - 9, "e4")
check(c.starts() == 1, "joiner: its own GAME-START that went out while lost is re-sent (%d)" % c.starts())

# 5: a joiner lost after its own GAME-START: not re-sent
a, b = tok(9), tok(10)
match("e5", [b, a], started_at=now - 400)
daemon.JIP_STARTED[a] = ("e5", now - 60)
ok, c = reattach(a, now - 9, "e5")
check(c.starts() == 0, "joiner lost after its own start: not re-sent (%d)" % c.starts())

# 6: a joiner still waiting for its own start: not re-sent (the match itself started long ago)
a, b = tok(11), tok(12)
match("e6", [b, a], started_at=now - 400)
with daemon.MM.lock:
    daemon.MM.joining[a] = ("e6", now - 20)
ok, c = reattach(a, now - 9, "e6")
check(c.starts() == 0, "joiner before its own start: not re-sent (%d)" % c.starts())
with daemon.MM.lock:
    daemon.MM.joining.pop(a, None)

# 7: the player left during the delay: not re-sent
a, b = tok(13), tok(14)
match("e7", [b, a], started_at=now - 3)
daemon.REATTACH_START_DELAY_S = 0.4
old = Conn(); daemon.SESSIONS.register(a, old); daemon.SESS_LOST[a] = (now - 6, "e7")
c = Conn()
daemon.session_reattach(c, ("127.0.0.1", 1), {"token": a, "name": "p"})
with daemon.MM.lock:
    daemon.MM.departed.setdefault("e7", set()).add(a)
time.sleep(0.7)
check(c.starts() == 0, "left before the delayed send: not re-sent (%d)" % c.starts())

print("FAILED: %d" % len(FAILS) if FAILS else "all ok")
sys.exit(1 if FAILS else 0)
