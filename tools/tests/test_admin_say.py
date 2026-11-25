"""Offline tests for admin match chat lines (tools/social.py admin_say + S_ACHAT): the admin site types a line into a running match "As:" a name,
every client of that match fetches it with S_ACHAT (since-id poll), other matches see nothing, the line is in chat_log, names / text are cleaned.
Run: python tools/tests/test_admin_say.py   (temp DB)."""
import contextlib, os, sqlite3, sys, tempfile
import sys as _t2s, os as _t2o; _t2s.path.append(_t2o.path.normpath(_t2o.path.join(_t2o.path.dirname(_t2o.path.abspath(__file__)), ".."))); import t2rtmp  # noqa: E402,F401

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import social  # noqa: E402
FAILS = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: FAILS.append(what)

def tok(name):
    return (b"T2N:".hex() + name.encode().hex() + "00" + "0123456789abcdef" * 8)[:64]
NAMES = ["Alice", "Bob", "Carol"]
T = {n: tok(n) for n in NAMES}

class Host:
    def __init__(self):
        self.t = 1000.0
        self.path = os.path.join(tempfile.mkdtemp(), "social.db")
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
    def steamid(self, t): return 0
    def name(self, t): return None
    def game_tokens(self): return []
    def alive_tokens(self): return set()
    def mm_state(self, t): return None

H = Host()
S = social.Social(H, social.Parties(max_size=4, now=H.now))
MATCH = {T["Alice"]: "m1", T["Bob"]: "m1", T["Carol"]: "m2"}
S.match_of = lambda t: MATCH.get(t)

def poll(who, since=0):
    return S.handle(T[who], 0, social.S_ACHAT, str(since))

r = poll("Alice")
check(r["ok"] == 1 and r["achat"]["lines"] == [], "S_ACHAT with no admin lines: ok, empty")
lid, err = S.admin_say("m1", "Ava", "  hello   match  ", who="admin@site")
check(lid and err is None, "admin_say returns an id")
r = poll("Alice")
check([(x["name"], x["t"]) for x in r["achat"]["lines"]] == [("Ava", "hello match")], "Alice (m1) gets the line, text cleaned")
check(r["achat"]["last"] == lid, "last = the line id")
check(poll("Bob")["achat"]["lines"][0]["t"] == "hello match", "Bob (same match) gets it too")
check(poll("Carol")["achat"]["lines"] == [], "Carol (other match) gets nothing")
check(poll("Alice", lid)["achat"]["lines"] == [], "since = last id: nothing new")
lid2, _ = S.admin_say("m1", "Ser<ver>|", "second")
r = poll("Alice", lid)
check([(x["name"], x["t"]) for x in r["achat"]["lines"]] == [("Server", "second")], "only the newer line; < > | stripped from the name")
check(S.admin_say("m1", "   ", "x")[1] == "no name", "empty name refused")
check(S.admin_say("m1", "Server", "  ")[1] == "empty text", "empty text refused")
check(S.admin_say("", "Server", "x")[1] == "no match", "no match id refused")
check(len(S.admin_say("m1", "N" * 60, "x")[0] and poll("Alice", lid2)["achat"]["lines"][0]["name"]) == social.ADMIN_NAME_LEN, "name cut to 24")
MATCH[T["Alice"]] = None
check(poll("Alice")["achat"]["lines"] == [], "not in a match: nothing")
H.t += social.ACHAT_KEEP_S + 1
S.admin_say("m2", "Server", "late")
check("m1" not in S.achat, "old match lines dropped after ACHAT_KEEP_S")
check(poll("Carol")["achat"]["lines"][0]["t"] == "late", "Carol gets the m2 line")
S._clog_flush(force=True)
with H.db() as c:
    rows = c.execute("SELECT kind, room, acct, name, body FROM chat_log ORDER BY id").fetchall()
check(("match", "m1", "admin:admin@site", "Ava", "hello match") in rows, "chat_log has the admin line (kind match, room = match id, acct admin:<who>)")
check(("match", "m1", "admin:site", "Server", "second") in rows, "who missing -> acct admin:site")
print("FAILS:", len(FAILS))
sys.exit(1 if FAILS else 0)
