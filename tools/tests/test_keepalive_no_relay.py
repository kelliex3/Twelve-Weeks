"""UDP keepalives (03 + seq) get their 04 pong and are NOT copied to every other keepalive socket (n players used to make n x (n - 1) packets,
growing with the square of the player count). The gameplay relay (00 HELLO / 02 DATA) is untouched by this.
Run: python tests/test_keepalive_no_relay.py"""
import os, sys, tempfile, struct
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
daemon.DB_PATH = os.path.join(tmp, "t2r.db"); daemon.LOG_PATH = os.path.join(tmp, "daemon.log")
daemon.evt = lambda *a, **k: None

sent = []
r = daemon.UdpRelay(lambda d, a: sent.append((d, a)))
players = [("203.0.113.%d" % i, 40000 + i) for i in range(1, 21)]
t = 1000.0
for rnd in range(3):
    for i, a in enumerate(players):
        r.handle(b"\x03" + struct.pack("<I", rnd * 100 + i), a, now=t)
        t += 0.01
ka = [(d, a) for d, a in sent if d[:1] == b"\x03"]
pong = [(d, a) for d, a in sent if d[:1] == b"\x04"]
check(len(pong) == 60, "every keepalive gets its pong (%d of 60)" % len(pong))
check(all(pong[k][1] == players[k % 20] for k in range(60)), "each pong goes back to its sender")
check(pong[5][0] == b"\x04" + struct.pack("<I", 5), "pong bytes = 04 + the client's seq")
check(len(ka) == 0, "no keepalive copied to other sockets (was 20 x 19 x 3 - ramp = hundreds, now %d)" % len(ka))
check(len(sent) == 60, "nothing else sent (%d)" % len(sent))
print("FAILED: %d" % len(FAILS) if FAILS else "all ok")
sys.exit(1 if FAILS else 0)
