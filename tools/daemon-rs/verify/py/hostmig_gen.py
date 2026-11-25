"""Seeded script generator for hostmig_harness.py (adaptive: it drives the Python module while it writes, so the calls follow the
state: real tokens, current epochs, ripe re-pick checks, ...).

usage: python3 hostmig_gen.py <dir with hostmig.py> <seed> <calls> > script"""
import json, os, random, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostmig_harness as H

TOKENS = ["t%d" % i for i in range(14)]
REGS = ["EU", "NA-east", "NA-west", "other", None, "mars"]
LOCS = ["", "fra=23+2,ams=27+3,lhr=35", "iad=30,atl=40,ord=12", "sea=20,lax=33", "sgp=100,hkg=90,fra=250", "xyz=5", "FRA=5,ams=50",
        "abcdef=5,sea=9", "lax2=4,lax=44", "fra=,sea=3", "ams=7+1,sto2=9", "zzzzz=1,iad=2"]


class Gen:
    def __init__(self, mod_dir, seed, n):
        self.r = random.Random(seed)
        self.eng = H.Engine(mod_dir)
        self.n = n
        self.t = 100000.5
        self.out = []
        self.calls = 0
        self.x = {t: self.r.randint(0, 200) for t in TOKENS}
        self.mseq = 0

    # ------------------------------------------------------------ plumbing
    def emit(self, op, dt=None, **kw):
        if dt is None:
            dt = self.r.choice([0.0, 0.25, 0.5, 0.5, 1.0, 1.0, 2.0, 2.0, 3.0, 5.0, 7.5, 12.0])
        self.t = round(self.t + dt, 3)
        c = dict(op=op, now=self.t, **kw)
        self.out.append(json.dumps(c, sort_keys=True, separators=(",", ":")))
        res = self.eng.exec(c)
        self.calls += 1
        return res["r"]

    def done(self):
        return self.calls >= self.n

    def hm(self):
        return self.eng.hm

    def mids(self):
        return list(self.hm().matches)

    def pick_mid(self):
        ms = self.mids()
        if not ms or self.r.random() < 0.08:
            return "mX%d" % self.r.randint(0, 2)
        return self.r.choice(ms)

    def players_of(self, mid):
        m = self.hm().matches.get(mid)
        return list(m["players"]) if m else []

    def alive_of(self, mid):
        m = self.hm().matches.get(mid)
        return [s for s in range(len(m["players"])) if s not in m["gone"]] if m else []

    def epoch_of(self, mid):
        m = self.hm().matches.get(mid)
        return m["epoch"] if m else 0

    # ------------------------------------------------------------ setup
    def setup(self):
        r = self.r
        kw = {}
        if r.random() < 0.5:
            kw = r.choice([{}, {"collect": 0.0}, {"collect": 5.0}, {"min_left": 2}, {"min_left": 0}, {"settle": 0.0}, {"lone_wait": 3.0},
                           {"min_silent": 0.0}, {"min_silent": 8.0}, {"unknown_keep_going": False}, {"bridge_grace": 0.0},
                           {"alive_window": 6.0, "silent_window": 4.0}])
        pool = r.choice([None, None, "all", "first", "dropfirst", "none", "raise", "last2"])
        self.emit("new", dt=1.0, enabled=r.random() < 0.95, kw=kw, ping=r.random() < 0.9, perf=r.random() < 0.6, pool=pool, link=r.random() < 0.7)
        self.emit("rp_new", dt=0.0, mode=r.choice([None, 1, 2]))
        self.retab()

    def retab(self):
        r = self.r
        for a in TOKENS:
            for b in TOKENS:
                if a == b:
                    continue
                self.tab_ping(a, b)
            self.emit("tab", dt=0.0, t="perf", k=a, v=r.choice([None, 0, 0, 0, 40, 80, "raise" if r.random() < 0.05 else 0]))
            self.emit("tab", dt=0.0, t="link", k=a, v=r.choice([None, True, True, False, "raise" if r.random() < 0.05 else True]))
            self.emit("tab", dt=0.0, t="reg", k=a, v=r.choice(REGS))
            self.emit("tab", dt=0.0, t="sid", k=a, v=r.choice([None, 0, 111, 222, 333]))
            if r.random() < 0.3:
                self.emit("tab", dt=0.0, t="party", k=a, v=r.sample(TOKENS, r.randint(1, 3)))
            self.emit("tab", dt=0.0, t="waited", k=a, v=r.choice([None, 0.0, 30.0, 400.0]))
            self.emit("tab", dt=0.0, t="hostok", k=a, v=r.choice([None, True, True, False]))

    def tab_ping(self, a, b):
        r = self.r
        v = abs(self.x[a] - self.x[b]) + 15 + r.randint(0, 25)
        if r.random() < 0.12:
            v = None
        elif r.random() < 0.05:
            v = r.choice([300, 500, 800])
        self.emit("tab", dt=0.0, t="ping", k=a + "|" + b, v=v)

    def jiggle(self):
        r = self.r
        for _ in range(r.randint(1, 6)):
            a, b = r.sample(TOKENS, 2)
            if r.random() < 0.5:
                self.tab_ping(a, b)
            else:
                v = abs(self.x[a] - self.x[b]) + 15 + r.randint(0, 25)
                self.emit("tab", dt=0.0, t="ping", k=a + "|" + b, v=v)
        a = r.choice(TOKENS)
        self.emit("tab", dt=0.0, t="perf", k=a, v=r.choice([None, 0, 40, 80, "raise" if r.random() < 0.05 else 0]))
        self.emit("tab", dt=0.0, t="link", k=a, v=r.choice([None, True, False, "raise" if r.random() < 0.05 else True]))
        if r.random() < 0.3:
            self.emit("poolmode", dt=0.0, mode=r.choice([None, "all", "first", "dropfirst", "none", "raise", "last2"]))
        if r.random() < 0.2:
            self.x[a] = r.randint(0, 200)

    # ------------------------------------------------------------ match actions
    def start_match(self):
        r = self.r
        k = r.choice([2, 2, 3, 4, 4, 5, 6, 8])
        pl = r.sample(TOKENS, k)
        self.mseq += 1
        mid = "m%d" % (self.mseq % 5 if r.random() < 0.4 else self.mseq)
        self.emit("start", mid=mid, players=pl)
        for t in pl:
            self.emit("poll", dt=0.1, token=t, flags=r.choice([0, 0, 2, 3]), epoch=0)

    def poll_one(self, mid, tok=None, flags=None, epoch=None):
        r = self.r
        pl = self.players_of(mid)
        if tok is None:
            tok = r.choice(pl) if pl and r.random() < 0.88 else r.choice(TOKENS)
        if flags is None:
            flags = r.choice([0, 0, 0, 1, 1, 2, 3, 4, 6, 7, 255])
        if epoch is None:
            ep = self.epoch_of(mid)
            epoch = ep if r.random() < 0.85 else r.choice([0, ep + 1, max(0, ep - 1), 3])
        return self.emit("poll", token=tok, flags=flags, epoch=epoch)

    def round(self, mid, flags_client=0, flags_host=0, skip_host=False, extra=0):
        m = self.hm().matches.get(mid)
        if not m:
            return
        h = m["host"]
        for s in range(len(m["players"])):
            if s in m["gone"] or (skip_host and s == h):
                continue
            f = flags_host if s == h else flags_client
            self.emit("poll", dt=self.r.choice([0.1, 0.25, 0.5]), token=m["players"][s], flags=f | extra, epoch=m["epoch"])
            m = self.hm().matches.get(mid)
            if not m:
                return

    def silent_burst(self, mid):
        r = self.r
        rounds = r.randint(1, 8)
        skip = r.random() < 0.3
        for _ in range(rounds):
            if mid not in self.hm().matches:
                return
            self.round(mid, flags_client=1, flags_host=0, skip_host=skip)
            self.emit("tick", dt=r.choice([0.5, 1.0, 2.0]))

    def crash_host(self, mid):
        r = self.r
        m = self.hm().matches.get(mid)
        if not m:
            return
        h = m["players"][m["host"]]
        how = r.choice(["bridge", "depart", "silence", "linkdown"])
        if how == "bridge":
            self.emit("bridge_gone", token=h)
            if r.random() < 0.4:
                self.emit("bridge_back", dt=1.0, token=h)
                self.emit("bridge_back", dt=1.0, token=h)
        elif how == "depart":
            self.emit("depart", token=h, reason=r.choice(["left", "session lost", "pending", "quit"]))
        elif how == "linkdown":
            self.emit("tab", dt=0.0, t="link", k=h, v=False)
            self.silent_burst(mid)
        else:
            for _ in range(r.randint(3, 9)):
                if mid not in self.hm().matches:
                    return
                self.round(mid, flags_client=1, skip_host=True)
                self.emit("tick", dt=r.choice([1.0, 2.0]))
        if r.random() < 0.5:
            self.emit("pending_stale", dt=r.choice([1.0, 6.0, 30.0]), mid=mid, secs=r.choice([0.0, 5.0]))
            self.emit("replaced_age", dt=0.0, token=h)
        # survivors ask
        for _ in range(r.randint(1, 4)):
            if mid not in self.hm().matches:
                return
            self.round(mid, skip_host=r.random() < 0.5)
            self.emit("tick", dt=r.choice([0.5, 1.0, 3.0, 4.0]))

    def handover(self, mid):
        r = self.r
        m = self.hm().matches.get(mid)
        if not m:
            return
        self.round(mid, extra=2)
        self.emit("handover_ok", mid=mid)
        al = self.alive_of(mid)
        slot = r.choice(al + [-1, 9]) if al else 0
        ok = self.emit("handover", mid=mid, slot=slot)
        if r.random() < 0.3:
            self.emit("export", mid=mid)
        dis = r.choice(["none", "none", "depart", "silent", "late", "partial", "bridge"])
        pl = self.players_of(mid)
        if dis == "depart" and pl:
            self.emit("depart", token=r.choice(pl), reason="left")
        elif dis == "silent" and pl:
            m = self.hm().matches.get(mid)
            if m:
                cl = [p for i, p in enumerate(m["players"]) if i != m["host"] and i not in m["gone"]]
                if cl:
                    self.emit("poll", token=r.choice(cl), flags=1 | 2, epoch=m["epoch"])
        elif dis == "late":
            self.emit("tick", dt=r.choice([31.0, 40.0]))
        elif dis == "bridge" and pl:
            self.emit("bridge_gone", token=r.choice(pl))
        for _ in range(r.randint(1, 4)):
            if mid not in self.hm().matches:
                return
            if dis == "partial":
                m = self.hm().matches.get(mid)
                if m:
                    al = [i for i in range(len(m["players"])) if i not in m["gone"]]
                    for i in al[:max(1, len(al) - 1)]:
                        self.emit("poll", dt=0.2, token=m["players"][i], flags=4 | 2, epoch=m["epoch"])
                    self.emit("tick", dt=r.choice([1.0, 31.0]))
                continue
            self.round(mid, flags_client=4 | 2, flags_host=4 | 2, extra=0)
            if r.random() < 0.15:
                self.emit("depart", token=r.choice(self.players_of(mid)), reason="left")
            if r.random() < 0.15:
                self.emit("poll", dt=r.choice([5.0, 31.0]), token=r.choice(self.players_of(mid)), flags=7, epoch=self.epoch_of(mid))

    def add_player(self, mid):
        r = self.r
        m = self.hm().matches.get(mid)
        if not m:
            self.emit("add", mid=mid, token="t0", slot=0)
            return
        n = len(m["players"])
        tok = r.choice(TOKENS + ["f0" * 30 + "0003"])
        slot = r.choice([n, n, n + 1, n + 2, 0, r.randint(0, max(0, n - 1))])
        self.emit("add", mid=mid, token=tok, slot=slot)
        if r.random() < 0.5:
            self.emit("handover_ok", mid=mid, dt=1.0)

    def import_rec(self, mid):
        r = self.r
        n = r.randint(2, 6)
        pl = r.sample(TOKENS, n)
        base = self.t - 50
        rec = {"players": pl, "host": r.randint(0, n - 1), "epoch": r.randint(0, 3), "gone": sorted(r.sample(range(n), r.randint(0, 1))),
               "seen": {"0": base}, "silent": {"1": base}, "silent_since": base, "lost": None, "bridge_gone": [], "bridge_at": {}, "given_up": True, "t0": base - 100.5}
        if r.random() < 0.5:
            rec.update(pending=True, collect_t0=base, cand0=[0], collect_logged=1, lost=base - 1)
        if r.random() < 0.4:
            rec.update(migrated_at=base - 5.0)
        if r.random() < 0.4:
            rec.update(replaced={"0": base}, replaced_told={"0": base + 1}, cap=[0, 1], joined={"1": base}, alone=True, freezes=[[0, base], [1, base - 2]],
                       alive_host_logged=1, min_silent_logged=1)
        self.emit("import_rec", mid=mid, rec=rec)
        for t in pl:
            self.emit("has", dt=0.0, token=t)

    def query(self, mid):
        r = self.r
        tok = r.choice(self.players_of(mid) or TOKENS)
        k = r.choice(["host_info", "snapshot", "host_token", "replaced_age", "freezes", "roster", "stale_slots", "handover_pending", "has",
                      "given_up", "pending_stale", "hlp", "mids", "sizes", "handover_ok", "export", "host_token", "replaced_age"])
        if k in ("host_info", "snapshot", "roster", "stale_slots", "handover_pending", "given_up", "hlp", "handover_ok", "export"):
            self.emit(k, mid=mid)
        elif k in ("host_token", "replaced_age", "has"):
            self.emit(k, token=tok)
        elif k == "freezes":
            self.emit(k, mid=mid, slot=r.randint(0, 3), win=r.choice([30.0, 300.0]))
        elif k == "pending_stale":
            self.emit(k, mid=mid, secs=r.choice([0.0, 5.0, 20.0]))
        else:
            self.emit(k)

    # ------------------------------------------------------------ re-pick
    def scores_for(self, mid, host_bad):
        r = self.r
        m = self.hm().matches.get(mid)
        if not m:
            return [], 0, []
        al = [s for s in range(len(m["players"])) if s not in m["gone"]]
        host = m["host"] if m["host"] in al else (al[0] if al else 0)
        pl = m["players"]
        sc = []
        for s in al:
            ps = [self.eng.ping(pl[s], pl[o]) for o in al if o != s]
            if not ps or None in ps or r.random() < 0.04:
                sc.append([s, None])
                continue
            pen = self.eng.T["perf"].get(pl[s], 0)
            pen = pen if isinstance(pen, int) else 0
            k = [max(ps) + pen, sum(ps)]
            if s == host and host_bad:
                k = [k[0] + r.choice([10, 30, 45, 60, 90, 200]), k[1] + r.choice([0, 50, 100, 150, 300])]
            elif s != host and r.random() < 0.15:
                k = [k[0], k[1] + r.choice([-120, -60, 90])]
            sc.append([s, k])
        if r.random() < 0.05:
            sc.append([host, None]) if not any(s == host for s, _ in sc) else None
        return sc, host, al

    def repick(self, mid):
        r = self.r
        k = r.choice(["check", "check", "check", "check", "observe", "due", "migrated", "saturated", "forget"])
        m = self.hm().matches.get(mid)
        if k == "observe":
            pl = m["players"] if m else TOKENS
            rtt = []
            for _ in range(r.randint(1, 5)):
                a, b = r.sample(pl if len(pl) > 1 else TOKENS, 2)
                rtt.append([a, b, r.choice([None, 0, 20, 40, 60, 120, 400])])
            self.emit("rp_observe", mid=mid, rtt=rtt)
        elif k == "due":
            self.emit("rp_due", mid=mid, started=self.t - r.choice([10.0, 100.0, 300.5]))
        elif k == "forget":
            self.emit("rp_forget", mid=mid)
        elif k == "migrated":
            self.emit("rp_migrated", mid=mid, roster=sorted(self.alive_of(mid)))
        elif k == "saturated":
            pl = self.players_of(mid) or TOKENS[:4]
            host = r.choice(pl)
            self.emit("rp_saturated", mid=mid, host=host, others=[t for t in pl if t != host][:r.randint(0, 4)])
            if r.random() < 0.5:
                # feed saturated pings: baseline low, then high
                oth = [t for t in pl if t != host][:3]
                self.emit("rp_observe", mid=mid, rtt=[[host, o, 40] for o in oth] + [[a, b, 40] for i, a in enumerate(oth) for b in oth[i + 1:]])
                for o in oth:
                    if r.random() < 0.85:
                        self.emit("tab", dt=0.0, t="ping", k=host + "|" + o, v=r.choice([200, 600, 70, 100, 130, 175, 240, 120]))
                        self.emit("tab", dt=0.0, t="ping", k=o + "|" + host, v=r.choice([200, 600, 70, 100, 130, 175, 240, 120]))
                if r.random() < 0.5 and len(oth) >= 2:
                    self.emit("tab", dt=0.0, t="ping", k=oth[0] + "|" + oth[1], v=r.choice([500, 41, None]))
                    self.emit("tab", dt=0.0, t="ping", k=oth[1] + "|" + oth[0], v=r.choice([500, 41, None]))
                self.emit("rp_saturated", mid=mid, host=host, others=oth)
        else:
            ripe = r.random() < 0.7
            sc, host, al = self.scores_for(mid, r.random() < 0.5)
            if r.random() < 0.4:
                n = r.choice([2, 3, 4, 5, 6, 6, 7, 8])
                al = list(range(n))
                host = r.randint(0, n - 1)
                sc = []
                for s_ in al:
                    k = [r.randint(40, 200), r.randint(100, 900)]
                    if s_ == host and r.random() < 0.7:
                        k = [k[0] + r.randint(0, 130), k[1] + r.choice([0, 20, 60, 120, 200, 400])]
                    sc.append([s_, None if r.random() < 0.05 else k])
            roster = al
            if r.random() < 0.1:
                roster = al[:2]
            started = self.t - (r.choice([200.0, 500.0]) if ripe else r.choice([30.0, 100.0, 130.0, 500.0]))
            left = r.choice([300.0, 300.5, 1000.0]) if ripe else r.choice([None, None, 20.0, 50.0, 89.9, 100.0, 300.0])
            busy = None if ripe or r.random() < 0.8 else r.choice(["migration pending", "a handover is already pending", "slot 2 still loading", "x"])
            labels = None if r.random() < 0.3 else {str(s): "L%d" % s for s, _ in sc if r.random() < 0.8}
            info = None
            if r.random() < 0.8:
                info = {"rtt": [[a, b, r.choice([None, 33, 80, 150])] for a in al for b in al if a < b][:6],
                        "perf": {str(s): r.choice([0, 0, 40, 80]) for s, _ in sc}, "loss": {str(s): r.choice([0, 0.5, 1.25, 3, 12.5, 0.0001, 123456.0, 1234567.0, 1e-5]) for s, _ in sc if r.random() < 0.3}}
                if r.random() < 0.8:
                    info["age"] = r.choice([0, 2.5, 30.0, 61.9])
            cap = None
            c = r.random()
            if c < 0.5:
                cap = None
            else:
                rank = {str(s): r.choice([0, 1, 2]) for s in al if r.random() < 0.8}
                cap = {"host_bad": r.choice([None, None, "H lagged", "H froze"]), "rank": rank, "max_rank": r.choice([0, 1, 1]),
                       "text": {str(s): "txt%d" % s for s in al if r.random() < 0.5}, "saturated": r.random() < 0.3}
                if r.random() < 0.15:
                    del cap["max_rank"]
                if r.random() < 0.15:
                    del cap["saturated"]
                if r.random() < 0.15:
                    cap["rank"] = {}
            self.emit("rp_check", dt=r.choice([30.0, 30.0, 90.0, 1.0, 200.0]), mid=mid, started=started, roster=roster, host=host, scores=sc, left=left, busy=busy,
                      labels=labels, info=info, cap=cap)
            if r.random() < 0.3:
                self.emit("rp_check", dt=30.0, mid=mid, started=started, roster=roster, host=host, scores=sc, left=left, busy=busy, labels=labels, info=info, cap=cap)

    # ------------------------------------------------------------ pure functions
    def pure(self):
        r = self.r
        k = r.choice(["parse_perf", "perf_penalty", "host_key", "region_of", "parse_pins", "region_first", "ping_fit", "ping_fit", "ping_fit", "region_pool", "region_pool", "region_pool", "region_pool", "ps", "ps"])
        if k == "parse_perf":
            b = bytearray(r.randint(0, 80)) if r.random() < 0.2 else bytearray(r.choice([72, 72, 73, 90]))
            for i in range(len(b)):
                b[i] = r.choice([0, 1, 2, 5, 7, 250, 255, 65 + (i % 20)])
            if len(b) > 64 and r.random() < 0.7:
                b[64] = 1
            self.emit("parse_perf", hex=bytes(b).hex())
        elif k == "perf_penalty":
            rep = None if r.random() < 0.15 else {"n250": r.choice([0, 3, 6, 12, 20]), "n1000": r.choice([0, 1, 2, 4, 9])}
            self.emit("perf_penalty", rep=rep)
        elif k == "host_key":
            self.emit("host_key", pings=[r.randint(1, 300) for _ in range(r.randint(1, 7))], pen=r.choice([0, 40, 80]))
        elif k == "region_of":
            self.emit("region_of", loc=r.choice(LOCS + [None]))
        elif k == "parse_pins":
            self.emit("parse_pins", text=r.choice(["", "76561198261455254:EU,1234:NA-east", "x:EU", "12:  EU ,13:NA-west:x", "1_0:EU", "_1:EU", "+5: a", "-3:b", "7", ",,:,1:", " 9 : EU"]))
        elif k == "region_first":
            toks = r.sample(TOKENS, r.randint(1, 8))
            toks = toks + ([] if r.random() < 0.7 else [r.choice(toks)])
            if r.random() < 0.15:
                for t in toks:
                    self.emit("tab", dt=0.0, t="reg", k=t, v=r.choice([None, "other", "mars"]))
            self.emit("region_first", tokens=toks, members=r.random() < 0.5, waited=r.random() < 0.5, max_wait=r.choice([None, 60.0, 300.0]))
        elif k == "ping_fit":
            pl_ = r.sample(TOKENS, r.randint(2, 8))
            sc = r.choice(["plain", "plain", "nopings", "far", "far", "hostunk", "hostunk"])
            if r.random() < 0.1:
                pl_ = pl_[:1]
            for i, a in enumerate(pl_):
                for b in pl_[i + 1:]:
                    for x, y in ((a, b), (b, a)):
                        if sc == "nopings":
                            v = None
                        elif sc == "far" and pl_[0] in (a, b):
                            v = r.choice([300, 400, 500])
                        elif sc == "hostunk" and r.random() < 0.3:
                            v = None
                        else:
                            v = r.choice([20, 60, 120, 170, 200, 260])
                        self.emit("tab", dt=0.0, t="ping", k=x + "|" + y, v=v)
            self.emit("ping_fit", players=pl_, limit=r.choice([100, 150, 175, 250, 0, -1, 175, 175]), units=r.random() < 0.5, hostok=r.random() < 0.5)
        elif k == "ping_fit_old":
            self.emit("ping_fit", players=r.sample(TOKENS, r.randint(1, 8)), limit=r.choice([0, -1, 100, 150, 175, 250]), units=r.random() < 0.5, hostok=r.random() < 0.5)
        elif k == "region_pool":
            pl = r.sample(TOKENS, r.randint(1, 8))
            sc = r.choice(["rand", "same", "tie", "major", "unknown", "pin", "far", "guard", "nocand"])
            regs = {"rand": None, "same": ["EU"] * 14, "tie": ["EU", "NA-east"] * 7, "major": ["EU", "EU", "NA-west"] * 5,
                    "unknown": [None, None, "EU"] * 5, "pin": ["EU", "NA-east"] * 7, "far": ["EU", "NA-east"] * 7,
                    "guard": ["EU", "EU", "NA-west", "NA-west", "NA-west"] * 3, "nocand": ["EU", "EU", "NA-west", "NA-west", "NA-west"] * 3}[sc]
            if sc == "guard":
                pl = r.sample(TOKENS, 5)
            if sc == "nocand":
                pl = r.sample(TOKENS, 5)
            if regs is not None:
                for i, t in enumerate(pl):
                    self.emit("tab", dt=0.0, t="reg", k=t, v=regs[i % len(regs)])
                    self.emit("tab", dt=0.0, t="sid", k=t, v=[111, 222, 333][i % 3] if sc == "pin" else 0)
                if sc == "guard":
                    for i, a in enumerate(pl):
                        for b in pl:
                            if a != b:
                                self.emit("tab", dt=0.0, t="ping", k=a + "|" + b, v=30 if (i < 2 and pl.index(b) < 2) else 200 if (i >= 2 and pl.index(b) >= 2) else 120)
                if sc in ("tie", "far"):
                    for i, a in enumerate(pl):
                        for b in pl[i + 1:]:
                            v = r.choice([30, 30, 200, None]) if sc == "tie" else (30 if (i % 2 == pl.index(b) % 2) else 260)
                            self.emit("tab", dt=0.0, t="ping", k=a + "|" + b, v=v)
                            self.emit("tab", dt=0.0, t="ping", k=b + "|" + a, v=v)
            ev = None if r.random() < 0.5 else pl + r.sample(TOKENS, r.randint(0, 3))
            if sc == "nocand":
                ev = list(pl)
                pl = pl[:2]
            elif r.random() < 0.15:
                ev = pl[:max(1, len(pl) - 1)]
            pins = None if (r.random() < 0.7 and sc != "pin") else {r.choice(["111", "222", "333"]): r.choice(["EU", "NA-east", "other", "", "mars"])}
            self.emit("region_pool", players=pl, everyone=ev, pins=pins)
        else:
            tok = r.choice(TOKENS[:4])
            kk = r.choice(["ps_report", "ps_report", "ps_get", "ps_penalty", "ps_prune", "ps_dump"])
            if kk == "ps_report":
                rep = {"n250": r.choice([0, 3, 6, 12]), "n1000": r.choice([0, 1, 2, 4]), "worst": r.randint(0, 3000), "avg_ms": r.choice([16.7, 33.3, 0.0]), "vram": r.choice([0, 1])}
                self.emit(kk, token=tok, rep=rep, steam=r.choice([0, 111, 222]))
            elif kk in ("ps_get", "ps_penalty"):
                self.emit(kk, dt=r.choice([1.0, 1.0, 100.0, 300.0]), token=tok, steam=r.choice([0, 111, 222]))
            else:
                self.emit(kk, dt=r.choice([1.0, 100.0, 400.0]))

    # ------------------------------------------------------------ main loop
    def run(self):
        r = self.r
        self.setup()
        while not self.done():
            ms = self.mids()
            c = r.random()
            if r.random() < 0.01:
                self.setup()
                continue
            if r.random() < 0.15:
                self.jiggle()
            if not ms or (len(ms) < 3 and c < 0.06):
                self.start_match()
                continue
            mid = self.pick_mid()
            w = r.random()
            if w < 0.22:
                self.poll_one(mid)
            elif w < 0.34:
                self.round(mid, flags_client=r.choice([0, 0, 1]), flags_host=r.choice([0, 0, 2]))
            elif w < 0.40:
                self.silent_burst(mid)
            elif w < 0.46:
                self.crash_host(mid)
            elif w < 0.51:
                self.handover(mid)
            elif w < 0.54:
                self.add_player(mid)
            elif w < 0.57:
                self.emit("depart", token=r.choice(self.players_of(mid) or TOKENS), reason=r.choice(["left", "session lost", "left (back in the queue after the restart)"]))
            elif w < 0.60:
                self.emit(r.choice(["bridge_gone", "bridge_back"]), token=r.choice(self.players_of(mid) or TOKENS))
            elif w < 0.63:
                self.emit("tick")
            elif w < 0.65:
                self.emit("end", mid=mid)
            elif w < 0.67:
                self.emit("export", mid=mid)
            elif w < 0.69:
                if mid in self.eng.exports:
                    self.emit("import_last", mid=mid if r.random() < 0.7 else "mY", src=mid)
                else:
                    self.emit("export", mid=mid)
            elif w < 0.70:
                self.import_rec(r.choice(["mI1", "mI2", mid]))
            elif w < 0.79:
                self.query(mid)
            elif w < 0.87:
                self.repick(mid)
            elif w < 0.97:
                self.pure()
            else:
                self.start_match()
        return "\n".join(self.out) + "\n"


if __name__ == "__main__":
    g = Gen(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]))
    sys.stdout.write(g.run())
