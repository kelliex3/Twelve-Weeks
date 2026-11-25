"""The rolling DB backup runs every DB_BACKUP_S (default 30 min, was 5 min) and checks the copy with quick_check; db_guard still restores
a zeroed / corrupt t2r.db from the backup, and a damaged copy never replaces the good backup.
Run: python tools/tests/test_db_backup.py"""
import os, sys, tempfile, sqlite3
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
FAILS = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: FAILS.append(what)
tmp = tempfile.mkdtemp()
os.environ.update(T2R_HOST="127.0.0.1", T2R_PORT="0", T2R_ADMIN_PORT="0", T2R_STATUS_FILE="0", T2R_DB=os.path.join(tmp, "t2r.db"))
os.environ.pop("T2R_DB_BACKUP_S", None)
import daemon  # noqa: E402
daemon.LOG_PATH = os.path.join(tmp, "daemon.log")
DB = os.path.join(tmp, "t2r.db")
daemon.DB_PATH, daemon.BAK_PATH = DB, DB + ".bak"

check({0xDE, 0xBF} <= daemon.NOISY_OPS, "gun-limit + rank-badge polls are counted, not logged line by line")
import inspect
check("last_bak = time.time()" in inspect.getsource(daemon.status_loop), "no backup right at start (db_guard just checked the DB)")
check(daemon.DB_BACKUP_S == 1800.0, "backup interval default 30 min (%s s)" % daemon.DB_BACKUP_S)
c = sqlite3.connect(DB)
c.execute("CREATE TABLE t (k INTEGER PRIMARY KEY, v TEXT)")
c.executemany("INSERT INTO t (v) VALUES (?)", [("x" * 200,)] * 2000)
c.commit(); c.close()
check(daemon.db_backup() is True and os.path.isfile(DB + ".bak"), "backup written")
check(not os.path.exists(DB + ".bak.tmp"), "no temp file left")
n = sqlite3.connect(DB + ".bak").execute("SELECT count(*) FROM t").fetchone()[0]
check(n == 2000, "backup holds all rows (%d)" % n)

# damaged live file: the damaged copy is refused, the good backup stays
good = open(DB + ".bak", "rb").read()
with open(DB, "r+b") as f:
    f.seek(4096 * 2); f.write(b"\xff" * 4096 * 3)
daemon.db_backup()
check(open(DB + ".bak", "rb").read() == good, "a damaged copy never replaces the good backup")

# zeroed live file at start: db_guard restores from the backup
open(DB, "wb").close()
st = daemon.db_guard()
n2 = sqlite3.connect(DB).execute("SELECT count(*) FROM t").fetchone()[0]
check(n2 == 2000, "db_guard restored the zeroed db from the backup (%s, %d rows)" % (st, n2))
check(daemon.db_guard() == "ok", "db_guard ok on a good db")
print("%d failure(s)" % len(FAILS))
sys.exit(1 if FAILS else 0)
