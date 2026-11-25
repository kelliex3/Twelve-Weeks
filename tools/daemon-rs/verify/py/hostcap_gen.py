"""Seeded script generator for hostcap_harness.py.   usage: python3 hostcap_gen.py <db path> <seed> <calls> > script"""
import json, random, sys

TOK = ["h%d" % i for i in range(6)]
MIDS = ["m%d" % i for i in range(4)]
V1 = ("CREATE TABLE host_cap(token TEXT PRIMARY KEY, hosted INT DEFAULT 0, best_kb INT DEFAULT 0, best_n INT DEFAULT 0, last_kb INT DEFAULT 0, "
      "lag_n INT DEFAULT 0, lag_t REAL DEFAULT 0, ceil_kb INT DEFAULT 0, last_t REAL DEFAULT 0)")


def main(db, seed, n):
    r = random.Random(seed)
    t = [100000.5]
    out = []

    def emit(op, dt=None, **kw):
        t[0] = round(t[0] + (r.choice([0, 1, 5, 60, 3600, 86400, 86400 * 10, 86400 * 40]) if dt is None else dt), 3)
        out.append(json.dumps(dict(op=op, now=t[0], **kw), sort_keys=True, separators=(",", ":")))

    emit("init", dt=0, db=db)
    emit("new", dt=0)
    while len(out) < n:
        c = r.random()
        tok, mid = r.choice(TOK), r.choice(MIDS + [None])
        if c < 0.15:
            emit("host_report", token=tok, n=r.choice([1, 2, 3, 4, 8]), avg=r.choice([0, -5, 100, 500, 900, 1500, 2400, 3000]), peak=r.choice([0, 2000]), mid=mid)
        elif c < 0.27:
            h = r.choice(TOK)
            emit("client_report", dt=r.choice([0, 1, 5]), mid=r.choice(MIDS), host=h, token=r.choice(TOK), loss10=r.choice([0, 10, 29, 30, 31, 80, 255]))
        elif c < 0.33:
            emit("note_lag", host=tok, mid=mid)
        elif c < 0.40:
            emit("verdict", token=tok, n=r.choice([1, 2, 3, 4, 5, 8]))
        elif c < 0.45:
            emit("proven_for", token=tok, n=r.choice([2, 4, 6, 8]))
        elif c < 0.50:
            emit("lagged_in", mid=mid, token=tok)
        elif c < 0.55:
            emit("get", token=r.choice(TOK + ["nobody"]))
        elif c < 0.60:
            emit("oneway_note", token=tok, mid=mid, host=r.random() < 0.4)
        elif c < 0.65:
            emit("oneway_get", token=tok, days=r.choice([30, 1, 0, 365]))
        elif c < 0.70:
            emit("forget", live=r.sample(MIDS, r.randint(0, 3)))
        elif c < 0.74:
            emit("new")
        elif c < 0.79:
            q = r.choice(["DROP TABLE IF EXISTS host_cap", "DROP TABLE IF EXISTS host_cap", "DROP TABLE IF EXISTS oneway", "UPDATE host_cap SET lag_mid=NULL",
                          "UPDATE host_cap SET ceil_kb=0", "UPDATE host_cap SET lag_n=0", "UPDATE host_cap SET best_kb=0", "DELETE FROM host_cap",
                          "UPDATE host_cap SET lag_t=lag_t-86400*20"])
            emit("sql", q=q)
            if q.startswith("DROP TABLE IF EXISTS host_cap") and r.random() < 0.6:
                emit("sql", q=V1)
                for tk in r.sample(TOK, 3):
                    emit("sql", q="INSERT INTO host_cap(token,hosted,best_kb,best_n,last_kb,lag_n,lag_t,ceil_kb,last_t) VALUES('%s',%d,%d,%d,%d,%d,%s,%d,%s)" % (
                        tk, r.randint(0, 5), r.choice([0, 300, 900, 2000]), r.choice([0, 4, 8]), r.choice([0, 400, 1200]), r.randint(0, 2),
                        r.choice(["0", "100000.5", "50000.25"]), r.choice([0, 250, 800]), r.choice(["0", "99000.5"])))
        elif c < 0.89:
            cands = [[p, [r.choice([40, 60, 80, 120, 200]), r.choice([100, 300, 500])]] for p in r.sample(TOK, r.randint(0, 6))]
            if r.random() < 0.15 and cands:
                cands.append(list(cands[0]))
            if r.random() < 0.5:
                emit("vtab", dt=0, t={p: [r.choice([0, 1, 1, 2]), "v%d" % r.randint(0, 9)] for p in TOK if r.random() < 0.7})
            emit("pick", n=r.choice([2, 4, 8]), cands=cands, window=r.choice([None, None, 0, 40, 100]), src=r.choice(["table", "table", "db"]))
        elif c < 0.97:
            b = bytearray(r.choice([60, 72, 79, 80, 85, 86, 90]))
            for i in range(len(b)):
                b[i] = r.choice([0, 1, 2, 3, 7, 200, 255])
            if len(b) > 72:
                b[72] = r.choice([1, 1, 1, 0, 2])
            if len(b) > 79:
                b[79] = r.choice([1, 1, 0])
            if len(b) > 71:
                b[71] = r.choice([0, 1, 2, 3])
            emit(r.choice(["parse_cap", "parse_oneway"]), dt=0, hex=bytes(b).hex())
        else:
            emit(r.choice(["need_kb", "need_kb_relay"]), dt=0, n=r.choice([-1, 0, 1, 2, 5, 8, 100]))
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]))
