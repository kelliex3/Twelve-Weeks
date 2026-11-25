"""Server outage: the DB backup held a read lock for 14 s. Social._adopt's COMMIT failed with "database is locked", the error was
swallowed and the transaction stayed open with sqlite's PENDING lock: every new reader of t2r.db then waited and failed, and the daemon
queued behind it until a restart. A failed adopt must roll back (no lock left) and be retried on the next poll.
Run: python tools/tests/test_social_adopt_lock.py"""
import os, sys, tempfile, sqlite3
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
FAILS = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: FAILS.append(what)
tmp = tempfile.mkdtemp()
os.environ.update(T2R_HOST="127.0.0.1", T2R_PORT="0", T2R_ADMIN_PORT="0", T2R_STATUS_FILE="0", T2R_P2P="0",
                  T2R_DB=os.path.join(tmp, "t2r.db"))
import daemon  # noqa: E402
import social  # noqa: E402
DB = os.path.join(tmp, "t2r.db")
daemon.DB_PATH = DB; daemon.LOG_PATH = os.path.join(tmp, "daemon.log")
daemon.evt = lambda *a, **k: None

def launcher_token(name, idhex):
    raw = bytearray(32)
    raw[0:4] = b"T2N:"
    nb = name.encode("utf-8")[:15]
    raw[4:4 + len(nb)] = nb
    raw[20:32] = bytes.fromhex(idhex)
    return bytes(raw).hex()

S = daemon.SOCIAL
ID = "11" * 12
tok = launcher_token("Adopter", ID)
pal = launcher_token("Pal", "22" * 12)
daemon.set_steam(pal, 76561198000000202, "Pal")
S.handle(pal, 0, social.S_STATE)                          # creates the social tables
old = social.acct_hash(ID)
with sqlite3.connect(DB) as c0:                            # a friend made while the token was still an install ("t:") account
    c0.execute("INSERT INTO social_friends(a,b,state,since) VALUES(?,?,1,0)", (old, "s:76561198000000202"))
key = "s:76561198000000201"

backup = sqlite3.connect(DB, isolation_level=None)         # = db_backup(): a reader holding SHARED for the whole copy
backup.execute("BEGIN")
backup.execute("SELECT count(*) FROM social_friends").fetchall()
c = sqlite3.connect(DB, timeout=0.3)
check(S._adopt(c, tok, key) is False, "adopt during the backup's read lock fails (locked)")
check(not c.in_transaction, "failed adopt left no open transaction")
backup.execute("COMMIT"); backup.close()
x = sqlite3.connect(DB, timeout=0.3)
try:
    n = x.execute("SELECT count(*) FROM social_friends WHERE a=?", (old,)).fetchone()[0]
    check(n == 1, "after the backup another connection reads at once (no PENDING lock left); old row still there")
except sqlite3.OperationalError as e:
    check(False, "after the backup another connection reads at once (got %r)" % (e,))
try:
    x.execute("INSERT INTO social_seen(acct,name,last) VALUES('probe','p',0)"); x.commit()
    check(True, "and writes")
except sqlite3.OperationalError as e:
    check(False, "and writes (got %r)" % (e,))
x.close()

# _adopt_once retries a failed adopt on the next poll
S.seen_w.pop(("adopt", tok), None)
blk = sqlite3.connect(DB, isolation_level=None); blk.execute("BEGIN"); blk.execute("SELECT 1 FROM social_friends").fetchall()
S._adopt_once(c, tok, key)
blk.execute("COMMIT"); blk.close()
check(S.seen_w.get(("adopt", tok)) is None, "failed adopt is not marked done")
S._adopt_once(c, tok, key)
c.commit()
with sqlite3.connect(DB) as c1:
    moved = c1.execute("SELECT count(*) FROM social_friends WHERE a=?", (key,)).fetchone()[0]
    left = c1.execute("SELECT count(*) FROM social_friends WHERE a=?", (old,)).fetchone()[0]
check(moved == 1 and left == 0 and S.seen_w.get(("adopt", tok)) == 1, "retry moves the friend to the Steam account")
c.close()
print("\n%d failure(s)" % len(FAILS) if FAILS else "\nall ok")
sys.exit(1 if FAILS else 0)
