"""WAL test writer: the daemon's write pattern on a copy of t2r.db, with the backup of src/d/db.rs (db_backup).

usage: wal_writer.py DB SECONDS MODE(wal|delete) BACKUP_EVERY_S OUT.json

- one long-lived write connection (synchronous=FULL, busy_timeout 10 s), like db.rs open_rw
- small write transactions every 20 ms (player rows: last_seen / kills) and a bigger one every 2 s (a match end: 16 player rows + a
  2 MB blob into a scratch table), each timed from BEGIN to COMMIT
- every BACKUP_EVERY_S: the db.rs backup. WAL: its own read-only connection, no write lock, one step, then journal_mode=DELETE on the
  copy. delete: the backup holds the writer's lock (Python / db.rs without WAL)
Writes the commit latencies (max, p99, count) and the backup durations to OUT.json.
"""
import json
import os
import sqlite3
import sys
import threading
import time

db, secs, mode, bk_every, out = sys.argv[1], float(sys.argv[2]), sys.argv[3], float(sys.argv[4]), sys.argv[5]
lock = threading.Lock()  # DBLOCK
w = sqlite3.connect(db, timeout=10, isolation_level=None, check_same_thread=False)
w.execute("PRAGMA synchronous=FULL")
jm = w.execute("PRAGMA journal_mode=%s" % ("WAL" if mode == "wal" else "DELETE")).fetchone()[0]
w.execute("CREATE TABLE IF NOT EXISTS wal_scratch(id INTEGER PRIMARY KEY, b BLOB)")
toks = [r[0] for r in w.execute("SELECT token FROM players ORDER BY last_seen DESC LIMIT 400")]
lat, big, backups, errs = [], [], [], []
stop = time.time() + secs


def tx(rows, blob):
    t0 = time.perf_counter()
    with lock:
        w.execute("BEGIN IMMEDIATE")
        for t in rows:
            w.execute("UPDATE players SET last_seen=?, kills=kills+0 WHERE token=?", (time.time(), t))
        if blob:
            w.execute("INSERT INTO wal_scratch(b) VALUES (?)", (os.urandom(blob),))
            w.execute("DELETE FROM wal_scratch WHERE id < (SELECT MAX(id) - 5 FROM wal_scratch)")
        w.execute("COMMIT")
    return time.perf_counter() - t0


def backup():
    tmp = db + ".bak.tmp"
    t0 = time.perf_counter()
    try:
        if mode == "wal":
            src = sqlite3.connect("file:%s?mode=ro" % db, uri=True, timeout=10)
            dst = sqlite3.connect(tmp)
            src.backup(dst)
            dst.execute("PRAGMA journal_mode=DELETE")
            dst.close()
            src.close()
        else:
            with lock:
                src = sqlite3.connect(db, timeout=10)
                dst = sqlite3.connect(tmp)
                src.backup(dst)
                dst.close()
                src.close()
        ok = sqlite3.connect(tmp).execute("PRAGMA quick_check").fetchone()[0]
        os.replace(tmp, db + ".bak")
        backups.append({"s": round(time.perf_counter() - t0, 2), "check": ok})
    except Exception as e:
        errs.append("backup: %r" % e)


def backups_loop():
    while time.time() < stop:
        time.sleep(bk_every)
        if time.time() < stop:
            backup()


th = threading.Thread(target=backups_loop, daemon=True)
th.start()
i = 0
last_big = time.time()
while time.time() < stop:
    try:
        if time.time() - last_big >= 2.0:
            last_big = time.time()
            big.append(tx(toks[(i * 16) % 384:(i * 16) % 384 + 16], 2 << 20))
        else:
            lat.append(tx([toks[i % len(toks)]], 0))
    except Exception as e:
        errs.append("write: %r" % e)
    i += 1
    time.sleep(0.02)
th.join(timeout=600)


def stats(v):
    v = sorted(v)
    if not v:
        return {}
    return {"n": len(v), "max_ms": round(v[-1] * 1000, 1), "p99_ms": round(v[int(len(v) * 0.99) - 1] * 1000, 1), "p50_ms": round(v[len(v) // 2] * 1000, 1)}


json.dump({"mode": jm, "small": stats(lat), "big": stats(big), "backups": backups, "errors": errs[:20], "n_errors": len(errs)}, open(out, "w"), indent=1)
w.execute("PRAGMA wal_checkpoint(TRUNCATE)") if mode == "wal" else None
w.close()
