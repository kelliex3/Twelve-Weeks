"""Differential test of tools/map_packs.py: SpSupport, the parsers, group_p1 and Packs without a registry (see cseed.py)."""
import cseed
from cseed import T, emit, fl, log, main, now, script, tok


def gen(r, n):
    toks = [tok(i) for i in range(r.choice([5, 8]))]
    levels = [r.choice(["to1-a", "to1-p1-b", "to1-c", "to1-p1-d", "e-p1-f", "g"]) + str(i) for i in range(r.randint(3, 14))]
    if r.random() < 0.3:
        levels.append(levels[0])
    yield {"t": 1000.0, "op": "init", "levels": levels}
    allv = levels + ["base-map", "to1-x"]
    t = 1000.0
    for _ in range(n):
        t += r.choice([0.1, 5.0, 600.0, 7000.0, 25000.0]) * r.random()
        t = fl(t)
        c = {"t": t}
        x = r.random()
        if x < 0.2:
            mask = 0
            for k in range(len(levels)):
                if r.random() < 0.5:
                    mask |= 1 << k
            if r.random() < 0.2:
                mask |= r.getrandbits(40)
            c.update(op="report", token=r.choice(toks), mask=mask, flags=r.choice([0, 1, 1, 0, 255, 256, 3]))
        elif x < 0.3:
            c.update(op=r.choice(["p1_always", "p1_owner", "mask_of", "reply"]), token=r.choice(toks))
        elif x < 0.5:
            c.update(op="allowed", level=r.choice(allv), players=r.sample(toks, r.choice([0, 1, 2, 3, 4])))
        elif x < 0.55:
            c.update(op="is_sp", level=r.choice(allv))
        elif x < 0.68:
            ready = r.sample(toks, r.randint(0, len(toks)))
            c.update(op="group_p1", ready=ready, always=r.sample(toks, r.randint(0, 3)), owner=r.sample(toks, r.randint(0, len(toks))))
        elif x < 0.78:
            ln = r.choice([0, 40, 64, 67, 68, 68, 69, 70])
            pl = [48 + r.randrange(10) if r.random() < 0.98 else r.randrange(256) for _ in range(64)] + [r.randrange(256) for _ in range(8)]
            c.update(op=r.choice(["parse_sp", "parse_flags"]), pl=pl[:ln])
        elif x < 0.9:
            ln = r.choice([0, 64, 65, 65, 69, 73, 77, 109, 120])
            pl = [48 + r.randrange(10) if r.random() < 0.98 else r.randrange(256) for _ in range(64)]
            pl = (pl + [r.choice([0, 1, 2, 5, 11, 12, 40, 255])] + [r.randrange(256) for _ in range(60)])[:ln]
            c.update(op="parse_report", pl=pl)
        elif x < 0.97:
            c.update(op="packs", token=r.choice(toks), level=r.choice(allv), players=r.sample(toks, r.choice([0, 1, 2])),
                     hashes=[r.getrandbits(32) for _ in range(r.randint(0, 3))], levels2=r.sample(allv, r.randint(0, 4)))
        else:
            c.update(op="env")
        yield c


def run():
    import map_packs
    sp = None
    packs = map_packs.Packs([], clock=now)
    for i, c in enumerate(script()):
        T[0] = float(c["t"])
        op = c["op"]
        r = None
        if op == "init":
            sp = map_packs.SpSupport(c["levels"], clock=now)
        elif op == "report":
            r = sp.report(c["token"], c["mask"], c["flags"])
        elif op == "p1_always":
            r = sp.p1_always(c["token"])
        elif op == "p1_owner":
            r = sp.p1_owner(c["token"])
        elif op == "mask_of":
            r = sp.mask_of(c["token"])
        elif op == "allowed":
            r = sp.allowed(c["level"], c["players"])
        elif op == "is_sp":
            r = sp.is_sp(c["level"])
        elif op == "reply":
            r = sp.reply_text(c["token"])
        elif op == "parse_sp":
            p = map_packs.parse_sp_report(bytes(c["pl"]))
            r = list(p) if p else None
        elif op == "parse_flags":
            r = map_packs.parse_sp_flags(bytes(c["pl"]))
        elif op == "group_p1":
            al, ow = set(c["always"]), set(c["owner"])
            g, k = map_packs.group_p1(c["ready"], lambda t: t in al, lambda t: t in ow)
            r = [g, k]
        elif op == "packs":
            ml = [("", "", lvl, 0) for lvl in c["levels2"]]
            r = {"report": sorted(packs.report(c["token"], c["hashes"])), "ids": sorted(packs.ids_of(c["token"])),
                 "allowed": packs.allowed(c["level"], c["players"]), "is_pack_map": packs.is_pack_map(c["level"]),
                 "maps": len(packs.maps()), "text": packs.reply_text(c["token"], ml)}
        elif op == "parse_report":
            p = map_packs.parse_report(bytes(c["pl"]))
            r = list(p) if p else None
        elif op == "env":
            r = {"enabled": map_packs.enabled(), "gating": map_packs.sp_gating()}
        have = {k: [v[0], v[1], sp.flags.get(k, 0)] for k, v in sp.have.items()} if sp else {}
        emit(i, r, {"have": have})


if __name__ == "__main__":
    main(gen, run)
