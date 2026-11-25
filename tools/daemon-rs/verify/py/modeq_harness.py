"""Differential test of tools/modeq.py + tools/gen_modes.py (see cseed.py). The mode list comes from modes.json next to the Python
modules (the deployed set has none: the built-in two modes); run once more with a PYDIR holding a 3-mode modes.json."""
import cseed
from cseed import T, emit, fl, log, main, now, script, tok


def gen(r, n):
    toks = [tok(i) for i in range(r.choice([5, 8, 14]))]
    t = 1000.0
    mids = []
    nm = 0
    yield {"t": t, "op": "info"}
    yield {"t": t, "op": "header"}
    for _ in range(n):
        t += r.choice([0.1, 1.0, 5.0, 60.0, 1200.0, 3000.0]) * r.random()
        t = fl(t)
        c = {"t": t}
        x = r.random()
        leaders = {k: r.choice(toks) for k in r.sample(toks, r.randint(0, 4))}
        if r.random() < 0.1:
            leaders[r.choice(toks)] = ""
        if x < 0.25:
            c.update(op="set", token=r.choice(toks), mode=r.choice([0, 0, 1, 1, 2, 3, 255, 255, -1, 256, 1000]))
        elif x < 0.33:
            c.update(op="get", token=r.choice(toks), **({"leader": r.choice(toks + [""])} if r.random() < 0.5 else {}))
        elif x < 0.55:
            ready = r.sample(toks, r.randint(0, len(toks)))
            c.update(op="group", ready=ready, leaders=leaders)
            if r.random() < 0.2 and ready:
                c["ready"] = ready + [ready[0]]
        elif x < 0.63:
            c.update(op="fits", token=r.choice(toks), mid=r.choice(mids or ["none"]), leaders=leaders)
            if r.random() < 0.5:
                c["leader"] = r.choice(toks)
        elif x < 0.72:
            c.update(op="counts", ready=r.sample(toks, r.randint(0, len(toks))), leaders=leaders)
        elif x < 0.80:
            nm += 1
            mid = "m%d" % nm
            mids.append(mid)
            c.update(op="formed", mid=mid, mode=r.choice(["supply-raid", "interrogation", "third", "", None]))
            if c["mode"] is None:
                del c["mode"]
        elif x < 0.84:
            c.update(op="match_mode", mid=r.choice(mids or ["none"]))
        elif x < 0.90:
            ln = r.choice([0, 10, 64, 65, 65, 66, 66, 70])
            c.update(op="parse", pl=[r.choice([r.randrange(256), 48 + r.randrange(10)]) for _ in range(ln)])
        elif x < 0.96:
            q = [r.randint(0, 9) for _ in range(r.choice([0, 2, 2, 3]))]
            c.update(op="reply", q=q, mode=r.choice([0, 1, 2, 255]), forced=r.random() < 0.3)
            if r.random() < 0.5:
                c["total"] = r.randint(0, 20)
        else:
            c.update(op="compat", a=r.choice([0, 1, 2, 255]), b=r.choice([0, 1, 2, 255]))
        yield c
    for i in range(400):
        if i % 100 == 0:
            yield {"t": t + 1, "op": "header"}
        yield {"t": t + 2, "op": "info"}
        break


def state(m):
    return {"pref": {k: list(v) for k, v in m.pref.items()}, "mmode": [[k, v] for k, v in m.mmode.items()], "rr": m.rr}


def run():
    import gen_modes
    import modeq
    m = modeq.ModeQ(now=now)
    for i, c in enumerate(script()):
        T[0] = float(c["t"])
        op = c["op"]
        leaders = c.get("leaders", {})
        leader_of = lambda t: leaders.get(t)
        tk = c.get("token")
        r = None
        if op == "set":
            r = m.set(tk, c["mode"])
        elif op == "get":
            r = m.get(tk, c.get("leader"))
        elif op == "group":
            g, name = m.group(c["ready"], leader_of)
            r = [g, name]
        elif op == "fits":
            r = m.fits(tk, c["mid"], c.get("leader"))
        elif op == "counts":
            r = m.counts(c["ready"], leader_of)
        elif op == "formed":
            m.formed(c["mid"], c.get("mode"))
        elif op == "match_mode":
            r = m.match_mode(c["mid"])
        elif op == "parse":
            p = m.parse(bytes(c["pl"]))
            r = list(p) if p else None
        elif op == "reply":
            r = m.reply(c["q"], c["mode"], c["forced"], c.get("total"))
        elif op == "compat":
            r = m.compat(c["a"], c["b"])
        elif op == "info":
            r = {"modes": list(modeq.MODES), "n": modeq.NMODES, "forced": modeq.forced()}
        elif op == "header":
            try:
                r = gen_modes.header()
            except Exception:
                r = None
        emit(i, r, state(m))


if __name__ == "__main__":
    main(gen, run)
