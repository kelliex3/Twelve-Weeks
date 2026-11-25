"""Differential test of tools/lobbybook.py (see cseed.py)."""
import cseed
from cseed import T, emit, fl, log, main, now, script, tok


def gen(r, n):
    toks = [tok(i) for i in range(r.choice([6, 10, 16, 24]))]
    t = 1000.0
    mids = []
    nm = 0
    out = set(toks)          # approximation of the tokens that are in no lobby (the generator does not run the book)
    burst = 0
    burst_src = None
    for _ in range(n):
        t += r.choice([0.1, 0.5, 1.0, 2.0, 3.0, 5.0, 9.0, 11.0, 30.0, 50.0, 70.0, 300.0]) * r.random() * r.choice([0.2, 0.5, 1, 1, 2])
        t = fl(t)
        c = {"t": t}
        x = r.random()
        if burst > 0:
            x, burst = 0.0, burst - 1
        if x < 0.40:
            c["op"] = "place"
            pool = sorted(out) if out and r.random() < 0.75 else toks
            c["token"] = r.choice(pool)
            out.discard(c["token"])
            c["unit"] = r.sample(toks, r.choice([1, 2, 3])) if r.random() < 0.3 else []
            if burst_src is not None and burst > 0:
                c["src"] = burst_src
            elif r.random() < 0.15 and mids:
                c["src"] = r.choice(mids)
                if r.random() < 0.5:
                    burst, burst_src = r.randint(2, 5), c["src"]
            if r.random() < 0.1:
                c["solo"] = True
            if r.random() < 0.3:
                c["at"] = fl(t - r.random() * 20)
            c["cap"] = r.choice([8, 8, 8, 8, 6, 4, 3])
            if r.random() < 0.08:
                c["nofit"] = r.sample(toks, r.choice([1, 2, 3]))
            if r.random() < 0.15:
                c["capx"] = {k: r.choice([1, 2]) for k in r.sample(toks, r.choice([1, 2]))}
        elif x < 0.46:
            c.update(op="away", token=r.choice(toks))
        elif x < 0.52:
            c.update(op="back", token=r.choice(toks))
        elif x < 0.60:
            c.update(op="remove", token=r.choice(toks), why=r.choice(["", "left", "joined the running match m1"]), block=r.choice([0, 0, 60]))
            out.add(c["token"])
        elif x < 0.68:
            c.update(op="expire", grace=r.choice([10, 10, 10, 3, 20]))
        elif x < 0.78:
            c.update(op="merge", cap=r.choice([8, 8, 6, 4]))
            if r.random() < 0.1:
                c["nofit"] = r.sample(toks, 2)
        elif x < 0.84:
            nm += 1
            mid = "m%d" % nm
            mids.append(mid)
            c.update(op="launched", tok=r.choice(toks), mid=mid)
        elif x < 0.89:
            c.update(op="cancelled", mid=r.choice(mids[-4:] or ["none"]), leaver=r.choice(toks))
        elif x < 0.93:
            c.update(op="sync", alive=r.sample(mids, r.randint(0, len(mids))) if mids else [],
                     started=r.sample(mids, r.randint(0, min(1, len(mids)))) if mids else [],
                     queued=r.sample(toks, r.randint(0, len(toks))))
        elif x < 0.95:
            c.update(op="set_hold", tok=r.choice(toks), hold=r.random() < 0.5)
        else:
            c.update(op="q", token=r.choice(toks), tok=r.choice(toks), mid=r.choice(mids or ["none"]))
        yield c


def state(b):
    return {"lobbies": {lid: {"members": lb.members, "joined": lb.joined, "away": [[k, v] for k, v in lb.away.items()],
                              "created": lb.created, "last_join": lb.last_join, "mid": lb.mid, "src": lb.src, "hold": lb.hold,
                              "wait_until": lb.wait_until} for lid, lb in b.lobbies.items()},
            "order": list(b.lobbies.keys()), "of": b.of, "left": {k: list(v) for k, v in b.left.items()},
            "mid_lid": [[k, v] for k, v in b.mid_lid.items()]}


def run():
    import lobbybook
    b = lobbybook.LobbyBook(8, log=log, now=now)
    for i, c in enumerate(script()):
        T[0] = float(c["t"])
        op = c["op"]
        r = None
        if op == "place":
            nofit = set(c.get("nofit", []))
            capx = c.get("capx", {})
            cap_n = c.get("cap", 8)
            r = list(b.place(c["token"], c.get("unit", []), lambda m, n: not any(t in nofit for t in list(m) + list(n)),
                             lambda lb, n: cap_n - sum(capx.get(m, 0) for m in lb.members), now=T[0], src=c.get("src"),
                             solo=c.get("solo", False), at=c.get("at")))
        elif op == "away":
            b.away(c["token"], T[0])
        elif op == "back":
            b.back(c["token"])
        elif op == "remove":
            r = b.remove(c["token"], c["why"], T[0], float(c["block"]))
        elif op == "expire":
            r = [list(x) for x in b.expire(T[0], float(c["grace"]))]
        elif op == "merge":
            nofit = set(c.get("nofit", []))
            cap_n = c.get("cap", 8)
            r = [list(x) for x in b.merge_pass(lambda m, n: not any(t in nofit for t in list(m) + list(n)), lambda lb, n: cap_n, T[0])]
        elif op == "launched":
            b.launched(b.of.get(c["tok"]), c["mid"])
        elif op == "cancelled":
            r = b.cancelled(c["mid"], c["leaver"], T[0])
        elif op == "sync":
            al, st = set(c["alive"]), set(c["started"])
            b.sync_matches(lambda m: m in al, lambda m: m in st, c["queued"], T[0])
        elif op == "set_hold":
            lb = b.lobbies.get(b.of.get(c["tok"]))
            if lb is not None:
                lb.hold = c["hold"]
        elif op == "q":
            lid = b.of.get(c["tok"])
            lb = b.get(lid)
            sl = b.src_lobby(c["mid"])
            lm = b.lobby_of_mid(c["mid"])
            r = {"lobby_of": b.lobby_of(c["token"]), "members": b.members(lid), "mates": b.mates(c["token"]), "key": b.key_for_mid(c["mid"]),
                 "of_mid": lm.lid if lm else None, "src": sl.lid if sl else None,
                 "open": [x.lid for x in b.open_lobbies()], "order": [x.lid for x in b.launch_order()],
                 "get": list(lb.members) if lb else None}
        emit(i, r, state(b))


if __name__ == "__main__":
    main(gen, run)
