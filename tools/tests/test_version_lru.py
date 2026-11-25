"""The client version table sat at its cap and each new player pushed the OLDEST entry out. A launch with an unchanged
version never moved its entry, so regulars were evicted while their game ran and every later search was refused "(version) None".
Now the least recently launched token goes first, and an online token is never evicted.
Run: python tools/tests/test_version_lru.py"""
import os, socket, sys
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
daemon._save_client_versions = lambda: None
daemon.token_known = lambda t: True                    # no new-identity rate limit in this test
V = "cz1117v"

def tok(n):
    return "%064x" % (0x7e57000 + n)

def launch(t, ver=V):
    v = ver.encode()
    daemon.version_op(bytes([len(v)]) + v + t.encode(), ("127.0.0.1", 1))

class Conn:
    def sendall(self, b):
        pass

daemon.CLIENT_VERSIONS.clear()
daemon.CLIENT_VERSIONS_MAX = 5

# 1. regular first, then 4 others: the table is full
reg = tok(0)
for i in range(5):
    launch(tok(i))
check(len(daemon.CLIENT_VERSIONS) == 5, "table full at the cap (5)")

# 2. the regular launches again (same version): it moves to the newest end
launch(reg)
check(list(daemon.CLIENT_VERSIONS)[-1] == reg, "an unchanged launch moves the token to the newest end")

# 3. a new player arrives: the least recently launched (tok 1) goes, not the regular
launch(tok(10))
check(reg in daemon.CLIENT_VERSIONS, "the regular is kept after a new player arrives")
check(tok(1) not in daemon.CLIENT_VERSIONS, "the least recently launched token is evicted")
check(len(daemon.CLIENT_VERSIONS) == 5, "the table stays at the cap")

# 4. an online token (live session) is skipped even when it is the oldest
oldest = next(iter(daemon.CLIENT_VERSIONS))
daemon.SESSIONS.register(oldest, Conn())
launch(tok(11))
check(oldest in daemon.CLIENT_VERSIONS, "a token with a live session is never evicted")
check(len(daemon.CLIENT_VERSIONS) == 5, "the table stays at the cap with an online oldest token")

# 5. a searching token is skipped too
second = list(daemon.CLIENT_VERSIONS)[1] if list(daemon.CLIENT_VERSIONS)[0] == oldest else list(daemon.CLIENT_VERSIONS)[0]
daemon.MM.queue[second] = 0.0
launch(tok(12))
check(second in daemon.CLIENT_VERSIONS, "a searching token is never evicted")
daemon.MM.queue.pop(second, None)

# 6. a version change still records the new version
launch(reg, "cz1117w")
check(daemon.CLIENT_VERSIONS.get(reg) == "cz1117w" and list(daemon.CLIENT_VERSIONS)[-1] == reg, "a changed version is recorded at the newest end")

# 7. the refusal from the reports is gone for the regular
launch(reg)
check(daemon.CLIENT_VERSIONS.get(reg) == V, "the regular's version is present (no 'None' refusal)")

print("\n%d FAIL" % len(FAILS))
sys.exit(1 if FAILS else 0)
