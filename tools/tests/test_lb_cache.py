"""Leaderboard cache vs the lobby rows cache. Both used the module global _LB_CACHE. The lobby cache clears itself after 256
lobbies, which removed the leaderboard's "t" / "top" / "v" keys: from then on every leaderboard request (op 0x13) and every menu
announce (0x0f + 0x0e, whose reply carries the leaderboard) raised KeyError('top') until the daemon restarted.
Run: python tests/test_lb_cache.py   (temp DB, no sockets)."""
import ast, collections, os, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
FAILS = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: FAILS.append(what)

# 1. no module-level name is bound twice in the daemon files (the class of bug, not only this name)
for fn in ("daemon.py", "social.py", "lobbybook.py"):
    tree = ast.parse(open(os.path.join(ROOT, fn), encoding="utf-8").read())
    seen = collections.defaultdict(list)
    for n in tree.body:
        if isinstance(n, (ast.Assign, ast.AnnAssign)):
            for x in (n.targets if isinstance(n, ast.Assign) else [n.target]):
                if isinstance(x, ast.Name):
                    seen[x.id].append(n.lineno)
        elif isinstance(n, (ast.FunctionDef, ast.ClassDef)):
            seen[n.name].append(n.lineno)
    dup = {k: v for k, v in seen.items() if len(v) > 1}
    check(not dup, "%s: no module-level name bound twice %s" % (fn, dup or ""))

# 2. the leaderboard still answers after the lobby cache cleared itself
os.environ.update(T2R_HOST="127.0.0.1", T2R_PORT="0", T2R_ADMIN_PORT="0", T2R_STATUS_FILE="0", T2R_MATCH_SIZE="8", T2R_ENGINE_SLOTS="8",
                  T2R_LOBBY_BOOK="1", T2R_DB=os.path.join(tempfile.mkdtemp(), "t2r.db"))
os.chdir(tempfile.mkdtemp())
import daemon  # noqa: E402
daemon.DB_PATH = os.environ["T2R_DB"]
daemon.log = lambda *a, **k: None
daemon.evt = lambda *a, **k: None

v0 = daemon.leaderboard_table()
check(isinstance(v0, bytes) and len(v0) >= 2, "leaderboard answers on a fresh daemon")
daemon.MM.book.members = lambda lid: []
err = None
try:
    for i in range(300):                                   # > 256 lobbies: the lobby cache clears itself
        daemon._lobby_entries("L%d" % i)
except Exception as e:                                     # noqa: BLE001
    err = e
check(err is None, "300 lobby row lookups run (%r)" % (err,))
daemon._LB_CACHE["t"] = 0.0                                # cache expired: the next call rebuilds the table
try:
    v1 = daemon.leaderboard_table()
    ok = isinstance(v1, bytes) and len(v1) >= 2
except Exception as e:                                     # noqa: BLE001
    ok = False
    print("    leaderboard raised", repr(e))
check(ok, "leaderboard still answers after the lobby cache cleared itself")
check(set(daemon._LB_CACHE) == {"t", "top", "v"}, "the leaderboard cache holds only its own keys (%s)" % sorted(map(str, daemon._LB_CACHE))[:5])

print("\n%d FAIL" % len(FAILS) if FAILS else "\nALL OK")
sys.stdout.flush()
os._exit(1 if FAILS else 0)
