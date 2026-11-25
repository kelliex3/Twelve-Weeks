"""(copy of the setup of test_social_chat.py) Offline tests for the friends + party system (tools/social.py + its daemon wiring): friend requests (send / accept / decline / cancel / remove / duplicates /
limits), presence statuses, name + Steam id search, party leader rules (invite / join / leave / kick / promote / timeout), in-game notes (EN / PL), the real op
0xD8 over TCP, and party matchmaking: a party lands on one team, is never split, waits until every member searches, waits when too big for the open slots,
mixes with solo players, and a leader that leaves disbands it.
Run: python tools/tests/test_pchat_seq.py   (free localhost port, temp DB)."""
import contextlib, json, os, socket, sqlite3, struct, sys, tempfile, threading, time
import sys as _t2s, os as _t2o; _t2s.path.append(_t2o.path.normpath(_t2o.path.join(_t2o.path.dirname(_t2o.path.abspath(__file__)), ".."))); import t2rtmp  # noqa: E402,F401  (temp hygiene: tools/t2rtmp.py)

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import social  # noqa: E402

FAILS = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: FAILS.append(what)

def tok(name):
    return (b"T2N:".hex() + name.encode().hex() + "00" + "0123456789abcdef" * 8)[:64]
NAMES = ["Alice", "Bob", "Carol", "Dave", "Eve", "Frank", "Gina", "Hal", "Ivan"]
T = {n: tok(n) for n in NAMES}
SID = {n: 76561198000000000 + i for i, n in enumerate(NAMES)}

# ------------------------------------------------------------ fake host
class Host:
    def __init__(self):
        self.t = 1000.0
        self.path = os.path.join(tempfile.mkdtemp(), "social.db")
        self.sids = {}              # token -> steam id
        self.names = {}             # token -> shown name
        self.game = set()           # tokens present in game
        self.mm = {}                # token -> match / search
        c = sqlite3.connect(self.path)
        c.execute("CREATE TABLE players(token TEXT PRIMARY KEY, name TEXT, loadout BLOB, steamid INTEGER DEFAULT 0, last_seen REAL)")
        c.execute("CREATE TABLE names(token_id TEXT PRIMARY KEY, name TEXT, tag TEXT, updated REAL)")
        c.commit(); c.close()
    def now(self): return self.t
    def log(self, *a): pass
    @contextlib.contextmanager
    def db(self):
        c = sqlite3.connect(self.path, timeout=10)
        try:
            yield c
            c.commit()
        finally:
            c.close()
    def steamid(self, t): return self.sids.get(t, 0)
    def name(self, t): return self.names.get(t)
    def game_tokens(self): return list(self.game)
    def alive_tokens(self): return set(self.game) | set(self.mm)
    def mm_state(self, t): return self.mm.get(t)
    def player(self, n, steam=True, game=False):
        t = T[n]
        self.names[t] = n
        if steam: self.sids[t] = SID[n]
        if game: self.game.add(t)
        with self.db() as c:
            c.execute("INSERT OR REPLACE INTO players(token,name,steamid,last_seen) VALUES(?,?,?,?)", (t, n, self.sids.get(t, 0), self.t))
        return t

H = Host()
PA = social.Parties(max_size=4, now=H.now)
S = social.Social(H, PA)

# party chat ids keep rising across a daemon restart (a client skips ids at or below the highest it has seen)
old = social.Social(H, social.Parties(max_size=4, now=H.now))
old.pseq += 5000                                   # a busy day of party / lobby lines before the restart
import time as _tm
_real = _tm.time
social.time.time = lambda: _real() + 600           # restarted 10 minutes later
new = social.Social(H, social.Parties(max_size=4, now=H.now))
social.time.time = _real
check(new.pseq > old.pseq, "after a restart the next party line id (%d) is above the last one before it (%d)" % (new.pseq + 1, old.pseq))
check(new.pseq + 10 ** 6 < 2 ** 31, "ids stay inside a 32-bit int (%d)" % new.pseq)
print("%d failure(s)" % len(FAILS))
sys.exit(1 if FAILS else 0)
