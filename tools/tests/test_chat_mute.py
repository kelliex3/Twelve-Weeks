"""Offline tests for chat moderation (tools/social.py): mutes.txt (timed + permanent, bare Steam ids, reload on change, expiry), "muted" refusals for
PARTY / LOBBY / friend messages, MATCH / TEAM log copies kept and marked muted, the "mute" field in every chat reply, S_MUTE, and the chat_log table.
Run: python tools/tests/test_chat_mute.py   (temp DB)."""
import contextlib, os, sqlite3, sys, tempfile, time
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
S.mute_path = os.path.join(os.path.dirname(H.path), "mutes.txt")
S.match_of = lambda t: "m42"
S.lobby_of = lambda t: "lobbyA"
for n in ("Alice", "Bob", "Carol"):
    H.player(n, game=True)
def call(who, sub, arg=""):
    return S.handle(T[who], 0, sub, arg)
def key(n): return "s:%d" % SID[n]
def write_mutes(text):
    with open(S.mute_path, "w", encoding="utf-8") as f:
        f.write(text)
    S._mute_chk = (0.0, None)          # (the test does not wait MUTE_RELOAD_S)
def rows(where="1=1", args=()):
    S._clog_flush(force=True)          # (the quick path writes its buffer every CHATLOG_FLUSH_S)
    c = sqlite3.connect(H.path)
    try:
        return c.execute("SELECT kind, room, acct, name, body, muted FROM chat_log WHERE " + where + " ORDER BY id", args).fetchall()
    finally:
        c.close()

# --- nobody muted: replies carry mute 0, lines are logged
r = call("Alice", social.S_MUTE)
check(r.get("ok") == 1 and r.get("mute") == 0, "S_MUTE without mutes.txt: ok, mute 0")
r = call("Alice", social.S_MLOG, "m\nhello all")
check(r.get("ok") == 1 and r.get("mute") == 0, "MATCH log line accepted, mute 0")
call("Alice", social.S_MLOG, "t\nflank left")
r = call("Alice", social.S_LSAY, "lobby hi")
check(r.get("ok") == 1 and r.get("mute") == 0, "LOBBY line accepted")
r = call("Alice", social.S_STATE)
check(r.get("mute") == 0, "state reply carries mute 0")
got = rows()
check(("match", "m42", key("Alice"), "Alice", "hello all", 0) in got, "chat_log has the MATCH line with its match id")
check(("team", "m42", key("Alice"), "Alice", "flank left", 0) in got, "chat_log has the TEAM line")
check(any(k == "lobby" and b == "lobby hi" for k, _, _, _, b, _ in got), "chat_log has the LOBBY line")

# --- timed mute for Bob (bare Steam id), permanent for Carol (account key)
write_mutes("# mutes\n%d %d  # spam (admin)\n%s 0 # slurs\n" % (SID["Bob"], int(H.t + 600), key("Carol")))
r = call("Bob", social.S_MUTE)
check(r.get("mute") == 600 and r.get("why") == "spam (admin)", "Bob: timed mute, 600 s left, reason shown (%r)" % (r,))
r = call("Carol", social.S_MUTE)
check(r.get("mute") == -1 and r.get("why") == "slurs", "Carol: permanent mute (-1)")
check(call("Alice", social.S_MUTE).get("mute") == 0, "Alice not muted")
r = call("Bob", social.S_LSAY, "lobby spam")
check(r.get("ok") == 0 and r.get("err") == "muted" and r.get("mute") == 600, "muted LOBBY line refused")
check(not any(m["t"] == "lobby spam" for v in S.lchat.values() for m in v), "refused LOBBY line not shown to the lobby")
r = call("Bob", social.S_MLOG, "m\nmatch spam")
check(r.get("ok") == 0 and r.get("err") == "muted", "muted MATCH log copy answered muted")
check(("match", "m42", key("Bob"), "Bob", "match spam", 1) in rows(), "muted MATCH line still logged, marked muted")

# party: Bob invites Alice (party ops still work while muted), PSAY refused
call("Bob", social.S_ADD, key("Alice")); call("Alice", social.S_ACCEPT, key("Bob"))
call("Bob", social.S_INVITE, key("Alice")); call("Alice", social.S_JOIN, key("Bob"))
check(PA.pid(T["Bob"]) and PA.pid(T["Bob"]) == PA.pid(T["Alice"]), "Bob + Alice in one party while Bob is muted")
r = call("Bob", social.S_PSAY, "party spam")
check(r.get("ok") == 0 and r.get("err") == "muted" and r.get("mute") == 600, "muted PARTY line refused")
r = call("Alice", social.S_PSAY, "hi party")
check(r.get("ok") == 1, "unmuted party member still talks")
check(("party", str(PA.pid(T["Alice"])), key("Alice"), "Alice", "hi party", 0) in rows(), "chat_log has the PARTY line with the party id")
r = call("Bob", social.S_MSG, key("Alice") + "\ndm spam")
check(r.get("ok") == 0 and r.get("err") == "muted", "muted friend message refused")
r = call("Alice", social.S_MSG, key("Bob") + "\nprivate")
check(r.get("ok") == 1, "friend message from an unmuted player works")
check(not rows("body=?", ("private",)), "friend messages are never in chat_log")

# --- expiry and unmute
H.t += 601
check(call("Bob", social.S_MUTE).get("mute") == 0, "timed mute over after its time")
r = call("Bob", social.S_LSAY, "back again")
check(r.get("ok") == 1, "LOBBY line accepted after the mute ran out")
write_mutes("")
check(call("Carol", social.S_MUTE).get("mute") == 0, "removing the line unmutes")
os.remove(S.mute_path)
S._mute_chk = (0.0, None)
check(call("Carol", social.S_MUTE).get("mute") == 0, "no mutes.txt = nobody muted")

# --- reload only on change (cheap): a second read in the same window uses the cache
write_mutes("%s 0\n" % key("Alice"))
check(call("Alice", social.S_MUTE).get("mute") == -1, "mute line without a reason works")
with open(S.mute_path, "w", encoding="utf-8") as f:
    f.write("")
check(call("Alice", social.S_MUTE).get("mute") == -1, "within MUTE_RELOAD_S the cached list is used")
H.t += social.MUTE_RELOAD_S + 1
os.utime(S.mute_path, (time.time() + 5, time.time() + 5))
check(call("Alice", social.S_MUTE).get("mute") == 0, "after MUTE_RELOAD_S a changed file is re-read")

# --- old chat_log rows are deleted
S._clog_gc = 0.0
H.t += social.CHATLOG_AGE_S + 10
call("Alice", social.S_MLOG, "m\nnew line")
call("Alice", social.S_STATE)
got = rows()
check(not any(b == "hello all" for *_, b, _ in got) and any(b == "new line" for *_, b, _ in got), "rows older than CHATLOG_AGE_S deleted")

print("\n%d failure(s)" % len(FAILS))
sys.exit(1 if FAILS else 0)
