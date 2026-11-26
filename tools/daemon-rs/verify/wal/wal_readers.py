"""WAL test readers: hits the DB-reading endpoints of t2r-files / t2r-web / t2r-admin (the live binaries) while wal_writer.py writes.

usage: wal_readers.py DB SECONDS OUT.json FILES_PORT WEB_PORT ADMIN_PORT
A reply counts as an error when the HTTP status is not 200, the connection fails, or the JSON says ok=false / has an "error".
"""
import json
import sqlite3
import sys
import threading
import time
import urllib.request

db, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
fp, wp, ap = sys.argv[4], sys.argv[5], sys.argv[6]
c = sqlite3.connect("file:%s?mode=ro" % db, uri=True)
rows = c.execute("SELECT token, steamid, name FROM players WHERE steamid > 0 ORDER BY last_seen DESC LIMIT 20").fetchall()
c.close()
urls = []
for tok, sid, name in rows[:10]:
    urls += [
        ("files", "http://127.0.0.1:%s/clan.json?player=%s" % (fp, tok)),
        ("files", "http://127.0.0.1:%s/clanfeed.json?player=%s&lang=en" % (fp, tok)),
        ("web", "http://127.0.0.1:%s/api/player?id=s%d" % (wp, sid)),
    ]
urls += [
    ("web", "http://127.0.0.1:%s/api/players" % wp),
    ("web", "http://127.0.0.1:%s/api/leaderboard" % wp),
    ("admin", "http://127.0.0.1:%s/api/overview" % ap),
    ("admin", "http://127.0.0.1:%s/api/players?q=a" % ap),
    ("admin", "http://127.0.0.1:%s/api/clans" % ap),
]
res = {}
lk = threading.Lock()
stop = time.time() + secs


def one(i):
    k = i
    while time.time() < stop:
        svc, u = urls[k % len(urls)]
        k += 4
        t0 = time.perf_counter()
        err = None
        try:
            with urllib.request.urlopen(u, timeout=30) as r:
                body = r.read()
                if r.status != 200:
                    err = "status %d" % r.status
                elif body[:1] in (b"{", b"["):
                    j = json.loads(body)
                    if isinstance(j, dict) and (j.get("ok") is False or j.get("error")):
                        err = "json %s" % str(j.get("error"))[:120]
        except Exception as e:
            err = repr(e)[:160]
        dt = time.perf_counter() - t0
        path = u.split("?")[0].split(":", 2)[2].split("/", 1)[1]
        key = "%s /%s" % (svc, path)
        with lk:
            s = res.setdefault(key, {"n": 0, "err": 0, "max_ms": 0.0, "sum_ms": 0.0, "errs": []})
            s["n"] += 1
            s["sum_ms"] += dt * 1000
            s["max_ms"] = max(s["max_ms"], round(dt * 1000, 1))
            if err:
                s["err"] += 1
                if len(s["errs"]) < 5:
                    s["errs"].append(err)
        time.sleep(0.05)


ts = [threading.Thread(target=one, args=(i,)) for i in range(4)]
for t in ts:
    t.start()
for t in ts:
    t.join()
for s in res.values():
    s["avg_ms"] = round(s.pop("sum_ms") / max(1, s["n"]), 1)
json.dump(res, open(out, "w"), indent=1, sort_keys=True)
