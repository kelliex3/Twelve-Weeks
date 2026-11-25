"""Differential test of tools/gunlimit.py (see cseed.py). Two guns (max 1 and max 2) so the ranking and the full bit go beyond the shipped table."""
import cseed
from cseed import T, emit, fl, log, main, now, script, tok

LIMITS = ((("shotgun-tactical",), 1, "TACTICAL SHOTGUN"), (("x",), 2, "SECOND GUN"))


def gen(r, n):
    toks = [tok(i) for i in range(r.choice([5, 8]))]
    mids = ["m1", "m2", "m3"]
    t = 1000.0
    for _ in range(n):
        t += r.choice([0.02, 0.1, 0.3, 0.7, 2.0, 20.0, 50.0]) * r.random()
        t = fl(t)
        c = {"t": t}
        x = r.random()
        if x < 0.82:
            c.update(op="claim", mid=r.choice(mids), team=r.choice([0, 1, 1, 0, 2]), token=r.choice(toks), mask=r.choice([0, 1, 2, 3, 3, 1, 2, 7]),
                     pre=r.random() < 0.5, slot=r.randint(0, 7))
            if r.random() < 0.5:
                c["active"] = r.sample(toks, r.randint(len(toks) // 2, len(toks)))
            else:
                c["active"] = None
        elif x < 0.86:
            c.update(op="release", token=r.choice(toks))
        elif x < 0.89:
            c.update(op="end", mid=r.choice(mids))
        elif x < 0.92:
            c.update(op="gc", live=r.sample(mids, r.randint(0, 3)))
        elif x < 0.95:
            c.update(op="export", mid=r.choice(mids))
        elif x < 0.97:
            c.update(op="import", mid=r.choice(mids), data={str(r.choice([0, 1])): {str(r.choice([0, 1])): r.sample(toks, r.randint(0, 3))}})
        elif x < 0.99:
            pl = [r.choice([48 + r.randrange(10), 97 + r.randrange(6), 65 + r.randrange(6), r.randrange(256)]) if r.random() < 0.02 else 48 + r.randrange(10)
                  for _ in range(64)]
            ln = r.choice([0, 65, 66, 66, 67])
            c.update(op="parse", pl=(pl + [r.randrange(256), r.randrange(256), 1])[: max(ln, 0)] if ln > 64 else pl[:ln])
        else:
            c.update(op="text", team=r.choice([0, 1]), held=r.randint(0, 3), grant=r.randint(0, 3), deny=r.randint(0, 3), full=r.randint(0, 3), pend=r.randint(0, 3))
        yield c
    yield {"t": t, "op": "table"}


def state(gl):
    held = {mid: {str(team): {str(bit): [[k, v] for k, v in h.items()] for bit, h in bits.items()} for team, bits in teams.items()}
            for mid, teams in gl.held.items()}
    want = {mid: {str(team): {str(bit): [[k, list(v)] for k, v in w.items()] for bit, w in bits.items()} for team, bits in teams.items()}
            for mid, teams in gl.want.items()}
    return {"held": held, "want": want, "horder": list(gl.held), "worder": list(gl.want)}


def run():
    import gunlimit
    gl = gunlimit.GunLimits(limits=LIMITS, now=now, log=log)
    for i, c in enumerate(script()):
        T[0] = float(c["t"])
        op = c["op"]
        r = None
        if op == "claim":
            active = None if c["active"] is None else set(c["active"])
            res = gl.claim(c["mid"], c["team"], c["token"], c["mask"], active, pre=c["pre"], slot=c["slot"], with_pend=True)
            grant, deny, full, held, pend = res
            r = [grant, deny, full, held, pend, gunlimit.GunLimits.text(1, c["team"], held, grant, deny, full, pend)]
        elif op == "release":
            gl.release(c["token"])
        elif op == "end":
            gl.end(c["mid"])
        elif op == "gc":
            gl.gc(set(c["live"]))
        elif op == "export":
            r = gl.export_held(c["mid"])
        elif op == "import":
            gl.import_held(c["mid"], c["data"])
        elif op == "parse":
            p = gl.parse_request(bytes(c["pl"]))
            r = list(p) if p else None
        elif op == "text":
            r = [gunlimit.GunLimits.text(0), gunlimit.GunLimits.text(1, c["team"], c["held"], c["grant"], c["deny"], c["full"], c["pend"])]
        elif op == "table":
            r = gunlimit.table_ids()
        emit(i, r, state(gl))


if __name__ == "__main__":
    main(gen, run)
