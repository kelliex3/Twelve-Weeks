"""Differential test of tools/requeue.py (see cseed.py)."""
import cseed
from cseed import T, emit, fl, log, main, now, script, tok


def gen(r, n):
    toks = [tok(i) for i in range(r.choice([6, 10, 16]))]
    yield {"t": 1000.0, "op": "init", "wait_max": r.choice([90.0, 120.0, 20.0]), "ticket_s": r.choice([300.0, 60.0, 100.0]),
           "offline_s": r.choice([30.0, 10.0]), "release_s": r.choice([0.0, 0.0, 15.0, 40.0]), "min_gap": r.choice([0.0, 3.0, 9.0])}
    t = 1000.0
    mids = []
    nm = 0
    for _ in range(n):
        t += r.choice([0.1, 0.5, 1.0, 2.0, 4.0, 8.0, 15.0, 40.0, 150.0]) * r.random() * r.choice([0.3, 1, 1, 2])
        t = fl(t)
        c = {"t": t}
        x = r.random()
        if x < 0.10 or not mids:
            nm += 1
            mid = "m%d" % nm
            mids.append(mid)
            c.update(op="open", mid=mid, tokens=r.sample(toks, r.randint(0, len(toks))) + ([""] if r.random() < 0.1 else []) + ([toks[0]] if r.random() < 0.2 else []))
        elif x < 0.28:
            c.update(op="searching", token=r.choice(toks))
        elif x < 0.31:
            c.update(op="drop", token=r.choice(toks))
        elif x < 0.34:
            c.update(op="drop_search", token=r.choice(toks))
        elif x < 0.37:
            c.update(op="has", token=r.choice(toks), now=r.random() < 0.5)
        elif x < 0.39:
            c.update(op="mid_of", token=r.choice(toks))
        elif x < 0.40:
            c.update(op="tokens")
        else:
            op = r.choice(["groups", "held", "ready", "formed", "kept", "next_of", "status", "members", "lobby", "groups", "held", "ready", "lobby"])
            c["op"] = op
            c["token"] = r.choice(toks)
            c["queued"] = r.sample(toks, r.randint(0, len(toks)))
            c["sil"] = {k: r.choice([0.0, 1.5, 12.0, 31.0, 80.0]) for k in r.sample(toks, r.randint(0, len(toks)))}
            c["inm"] = r.sample(toks, r.choice([0, 0, 0, 1, 2]))
            if op == "formed":
                nm += 1
                mid = "m%d" % nm
                mids.append(mid)
                c["mid"] = mid
                c["tokens"] = r.sample(toks, r.randint(0, len(toks)))
            elif op == "kept":
                c["mid"] = r.choice(mids[-8:])
        yield c


def state(q):
    return {"t": [[k, e[0], e[1], e[2]] for k, e in q.t.items()], "first": q.first, "next": q.next}


def run():
    import requeue
    q = None
    for i, c in enumerate(script()):
        T[0] = float(c["t"])
        op = c["op"]
        if op == "init":
            q = requeue.Requeue(c["wait_max"], c["ticket_s"], c["offline_s"], now=now, release_s=c["release_s"], min_gap=c["min_gap"])
            emit(i, None, state(q))
            continue
        sil = c.get("sil", {})
        inm = set(c.get("inm", []))
        silence = lambda t: sil.get(t, 0.0)
        in_match = lambda t: t in inm
        queued = c.get("queued", [])
        tk = c.get("token")
        r = None
        if op == "open":
            r = q.open(c["mid"], c["tokens"], T[0])
        elif op == "searching":
            r = q.searching(tk)
        elif op == "drop":
            r = q.drop(tk)
        elif op == "drop_search":
            r = q.drop_search(tk)
        elif op == "has":
            r = q.has(tk, T[0] if c.get("now") else None)
        elif op == "mid_of":
            r = q.mid_of(tk)
        elif op == "tokens":
            r = q.tokens()
        elif op == "groups":
            g, gone = q.groups(queued, silence, in_match, T[0])
            r = {"g": g, "order": list(g), "gone": [list(x) for x in gone]}
        elif op == "held":
            h, w, gone = q.held(queued, silence, in_match, T[0])
            r = {"held": sorted(h), "waiting": w, "worder": list(w), "gone": [list(x) for x in gone]}
        elif op == "ready":
            r = [list(x) for x in q.ready_groups(queued, silence, in_match, T[0])]
        elif op == "formed":
            r = sorted(q.formed(c["mid"], c["tokens"]))
        elif op == "kept":
            r = q.kept(c["mid"], T[0])
        elif op == "next_of":
            r = q.next_of(tk)
        elif op == "status":
            r = list(q.status(tk, queued, silence, in_match, T[0]))
        elif op == "members":
            m = q.members(tk, queued, silence, in_match, T[0])
            r = None if m is None else [list(m[0]), list(m[1])]
        elif op == "lobby":
            r = q.lobby(tk, queued, silence, in_match, T[0])
        emit(i, r, state(q))


if __name__ == "__main__":
    main(gen, run)
