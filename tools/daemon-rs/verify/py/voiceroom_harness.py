"""Differential test of tools/voiceroom.py (see cseed.py): rooms() / payload() / Pusher.tick() on generated parties, requeue groups and helpers.

Connections are named objects (the Python side keeps one `object()` per name; the Rust side one id per name); the state line is the
Pusher's last table as [token, connection name, payload hex]."""
import cseed
from cseed import emit, main, script, tok

PIDS = ["p1", "p2", "p3", "a'b", 'q"x', "é", "p%d" % 7]
MIDS = ["m1", "m2", "m10", "m3"]
NAMES = ["", "Ellie", "Joel", "x" * 30, "é" * 14, "é" * 13 + "a", "\U0001f600" * 7, "Ab", "PLAYER"]


def world(r, toks):
    eligible = [t for t in toks if r.random() < 0.8]
    pid, groups = {}, {}
    for t in toks:
        if r.random() < 0.6:
            pid[t] = r.choice(PIDS)
    for p in PIDS:
        mem = [t for t in toks if pid.get(t) == p]
        if r.random() < 0.15:
            mem = mem + [r.choice(toks)]            # a member that is not in the party (any token)
        if r.random() < 0.15:
            mem = [t for t in mem if r.random() < 0.7]
        groups[p] = mem
    rq = {}
    for m in r.sample(MIDS, r.randint(0, 3)):
        rq[m] = [t for t in toks if r.random() < 0.4] + ([r.choice(toks)] if r.random() < 0.3 else [])
    sid = {t: r.choice([0, 76561190000000000 + r.randrange(10 ** 9), 2 ** 63 + r.randrange(10 ** 6), 2 ** 64 - 1]) for t in toks if r.random() < 0.9}
    names = {t: r.choice(NAMES) for t in toks if r.random() < 0.8}
    return {"eligible": eligible, "pid": pid, "groups": groups, "rq": rq, "sid": sid, "names": names}


def gen(r, n):
    toks = [tok(i) for i in range(r.choice([6, 10, 14]))]
    conns = ["c%d" % i for i in range(40)]
    ci = 0
    cur = {t: None for t in toks}
    w = world(r, toks)
    for _ in range(n):
        x = r.random()
        if x < 0.25:
            w = world(r, toks) if r.random() < 0.5 else w
            yield dict(w, op="rooms")
        elif x < 0.30:
            if r.random() < 0.9:
                yield dict(w, op="payload", key=[r.choice("pr"), r.choice(PIDS + MIDS)], members=r.sample(toks, r.randint(0, 9) if len(toks) >= 9 else r.randint(0, len(toks))))
            else:
                yield dict(w, op="payload", key=None, members=[])
        else:
            if r.random() < 0.4:
                w = world(r, toks)
            helpers = []
            for t in toks:
                if r.random() < 0.85:
                    if cur[t] is None or r.random() < 0.1:
                        cur[t] = conns[ci % len(conns)]
                        ci += 1
                    helpers.append([t, cur[t]])
            r.shuffle(helpers)
            yield dict(w, op="tick", helpers=helpers)


def run():
    import voiceroom
    objs = {}
    names = {}
    pusher = voiceroom.Pusher()

    def conn(n):
        if n not in objs:
            objs[n] = object()
            names[id(objs[n])] = n
        return objs[n]

    for i, c in enumerate(script()):
        eligible = set(c["eligible"])
        pid = c["pid"]
        groups = c["groups"]
        sid = c["sid"]
        nm = c["names"]
        pp = lambda t: pid.get(t)
        pm = lambda t: list(groups.get(pid.get(t), [])) if t in pid else []
        sd = lambda t: sid.get(t, 0)
        na = lambda t: nm.get(t, "")
        op = c["op"]
        r = None
        if op == "rooms":
            r = [[t, v[0][0], v[0][1], voiceroom.room_id(v[0]), v[1]] for t, v in voiceroom.rooms(eligible, pp, pm, c["rq"]).items()]
        elif op == "payload":
            key = tuple(c["key"]) if c.get("key") else None
            r = voiceroom.payload(key, c["members"], sd, na).hex()
        elif op == "tick":
            helpers = {t: conn(n) for t, n in c["helpers"]}
            rm = voiceroom.rooms(eligible, pp, pm, c["rq"])
            r = [[t, names[id(cn)], pl.hex()] for t, cn, pl in pusher.tick(helpers, rm, sd, na)]
        s = [[t, names[ci_], pl.hex()] for t, (ci_, pl) in sorted(pusher.last.items())]
        emit(i, r, s)


main(gen, run)
