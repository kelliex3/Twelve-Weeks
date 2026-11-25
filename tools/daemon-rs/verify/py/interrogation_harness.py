"""Differential test of tools/interrogation.py (see cseed.py). The Interrogation rng is random.Random(T2R_TEST_SEED) on both sides."""
import json
import random

import cseed
from cseed import T, emit, fl, log, main, now, script, tok


def gen(r, n):
    toks = [tok(i) for i in range(r.choice([8, 12, 16]))]
    lockboxes = {"0": [2, 3], "1": [1, 1], "2": [4, 2], "5": [3, 3]}
    yield {"t": 1000.0, "op": "init", "mode": r.choice(["interrogation", "rotate", "int", "supply-raid", "Interrogation "]), "need": r.choice([2, 3, 5]),
           "kpi": r.choice([0, 2, 4]), "stage_secs": r.choice([18.0, 6.0]), "boost_after": r.choice([720.0, 40.0]), "boost_every": r.choice([90.0, 15.0, 0.0]),
           "boost_step": 0.12, "boost_max": r.choice([4, 2]), "locate_secs": r.choice([20.0, 5.0, 0.0]), "keep": r.choice([30.0, 10.0]),
           "lockboxes": r.choice([lockboxes, {}])}
    t = 1000.0
    nm = 0
    live = {}      # mid -> (players, slots)
    seqs = {}
    for _ in range(n):
        t += r.choice([0.05, 0.5, 1.0, 3.0, 8.0, 25.0]) * r.random() * r.choice([0.5, 1, 1, 2])
        t = fl(t)
        c = {"t": t}
        x = r.random()
        if x < 0.04 or not live:
            nm += 1
            mid = "m%d" % nm
            k = r.choice([2, 4, 6, 8])
            pl = r.sample(toks, min(k, len(toks)))
            slots = [i % 2 for i in range(len(pl))] if r.random() < 0.8 else [r.choice([0, 1]) for _ in pl]
            live[mid] = (pl, slots)
            c.update(op="start", mid=mid, players=pl, slots=slots, map=r.choice([0, 1, 2, 5, 7, 9]))
            if r.random() < 0.7:
                c["mode"] = r.choice(["interrogation", "interrogation", "supply-raid"])
        elif x < 0.06:
            mid = r.choice(sorted(live))
            pl = r.sample(toks, r.randint(1, len(toks)))
            live[mid] = (pl, live[mid][1])
            c.update(op="roster", mid=mid, players=pl, slots=[r.choice([0, 1]) for _ in pl])
        elif x < 0.08:
            c.update(op="end", mid=r.choice(sorted(live)))
            if r.random() < 0.7:
                c["winner"] = r.choice([0, 1, 255])
        elif x < 0.09:
            c.update(op="drop", mid=r.choice(sorted(live)))
        elif x < 0.10:
            c.update(op="pick_mode")
        elif x < 0.115:
            c.update(op="rebind", token=r.choice(toks), cur=r.choice(sorted(live) + ["nomatch"]), tag=r.choice(["TAGA", "tag b"]))
        elif x < 0.13:
            nm += 1
            c.update(op="roundtrip", mid=r.choice(sorted(live)), to="m%d" % nm)
            live["m%d" % nm] = live[c["mid"]]
        elif x < 0.145:
            pl = [48 + r.randrange(10) if r.random() < 0.97 else r.randrange(256) for _ in range(64)]
            ln = r.choice([0, 64, 65, 66, 67, 68, 70])
            tail = [r.randrange(256) for _ in range(6)]
            c.update(op="parse", pl=(pl + tail)[:ln])
        elif x < 0.155:
            c.update(op="has", mid=r.choice(sorted(live)), token=r.choice(toks))
        else:
            mid = r.choice(sorted(live))
            pl, slots = live[mid]
            tk = r.choice(pl) if r.random() < 0.93 else r.choice(toks)
            sub = r.choice([0, 0, 1, 1, 1, 2, 2, 3, 3, 3, 4])
            if sub == 0:
                args = [r.randint(0, 120)] if r.random() < 0.5 else []
            elif sub == 1:
                args = [r.randint(0, 8)] if r.random() < 0.95 else []
            elif sub == 2:
                args = [r.choice([1, 1, 2, 2, 3, 3, 4, 0])] if r.random() < 0.95 else []
            else:
                args = [r.randrange(256)] if r.random() < 0.5 else []
            q = seqs.get(tk, 0)
            if r.random() < 0.15:
                seq = 0
            elif r.random() < 0.1:
                seq = q
            elif r.random() < 0.02:
                seq = 65535
            else:
                seq = (q + 1) & 0xFFFF
            seqs[tk] = seq
            c.update(op="event", token=tk, sub=sub, seq=seq, args=args)
        yield c


def state(it):
    with it.lock:
        return {"matches": {mid: json.loads(json.dumps(rec, default=str)) for mid, rec in it.matches.items()}, "order": list(it.matches),
                "by_token": dict(it.by_token), "rot": it._rot}


def run():
    import interrogation
    it = None
    for i, c in enumerate(script()):
        T[0] = float(c["t"])
        op = c["op"]
        if op == "init":
            lb = {int(k): tuple(v) for k, v in c["lockboxes"].items()}
            it = interrogation.Interrogation(mode=c["mode"], need=c["need"], kills_per_info=c["kpi"], now=now, log=log, rng=random.Random(cseed.SEED),
                                             lockboxes=lb, stage_secs=c["stage_secs"], boost_after=c["boost_after"], boost_every=c["boost_every"],
                                             boost_step=c["boost_step"], boost_max=c["boost_max"], locate_secs=c["locate_secs"], keep=c["keep"])
            emit(i, None, state(it))
            continue
        slots = c.get("slots", [])
        team_of = lambda slot: slots[slot] if slot < len(slots) else slot % 2
        r = None
        if op == "pick_mode":
            r = it.pick_mode()
        elif op == "start":
            r = it.start(c["mid"], c["players"], c["map"], team_of, mode=c.get("mode"))
        elif op == "roster":
            it.roster(c["mid"], c["players"], team_of)
        elif op == "end":
            it.end(c["mid"], c.get("winner"))
        elif op == "drop":
            it.drop(c["mid"])
        elif op == "event":
            r = it.event(c["token"], c["sub"], c["seq"], bytes(c["args"]))
        elif op == "parse":
            p = it.parse_request(bytes(c["pl"]))
            r = None if not p else [p[0], p[1], p[2], list(p[3])]
        elif op == "rebind":
            token, cur, tag = c["token"], c["cur"], c["tag"]
            with it.lock:
                had = it.by_token.get(token)
                done = False
                if had is None or had == cur:
                    if had is None and cur in it.matches:
                        it.by_token[token] = cur
                    done = True
                elif cur in it.matches:
                    it.by_token[token] = cur
                else:
                    it.by_token.pop(token, None)
            if not done:
                log("INTERROGATION rebind", tag, "from match", had, "to", cur if cur in it.matches else "none (its match %s has no record)" % cur)
        elif op == "roundtrip":
            with it.lock:
                ir = it.matches.get(c["mid"])
                rj = json.loads(json.dumps(ir, default=str)) if ir else None
            if rj:
                with it.lock:
                    it.matches[c["to"]] = rj
                    for t in rj.get("players", []):
                        it.by_token[t] = c["to"]
            r = json.dumps(rj) if rj else None
        elif op == "has":
            r = [c["mid"] in it.matches, it.by_token.get(c["token"])]
        emit(i, r, state(it))


if __name__ == "__main__":
    main(gen, run)
