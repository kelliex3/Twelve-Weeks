"""Differential test driver for hostmig.py (see tools/daemon-rs/PORTING.md "Verification").

usage: python3 hostmig_harness.py <dir with hostmig.py> < script > out        (Rust side: t2r-daemon selftest hostmig < script > out)

The script is JSON lines, one call each: {"op": ..., "now": float, ...}. The wall clock is frozen to "now" (time.time and
time.monotonic both). Output: one JSON line per call: {"r": result, "l": [captured log lines], "s": hm state, "p": re-pick state}
with sorted keys and compact separators, so the two sides can be compared with cmp."""
import json, os, sys

for _k in [k for k in os.environ if k.startswith("T2R_")]:
    del os.environ[_k]

NOW = [0.0]


def clock():
    return NOW[0]


import time
time.time = clock
time.monotonic = clock


def clean(s):
    return "".join(" " if (ord(c) < 32 or c in "\x7f\x85  ") else c for c in s)


class Engine:
    def __init__(self, mod_dir):
        sys.path.insert(0, mod_dir)
        import hostmig
        self.hm_mod = hostmig
        self.LOG = []
        self.hm = None
        self.rp = None
        self.ps = hostmig.PerfScores(now=clock)
        self.T = {"ping": {}, "perf": {}, "link": {}, "reg": {}, "sid": {}, "party": {}, "waited": {}, "hostok": {}}
        self.pool_mode = None
        self.exports = {}

    def logf(self, *a):
        self.LOG.append(clean(" ".join(str(x) for x in a)))

    # ---- callbacks driven by the tables
    def ping(self, a, b):
        return self.T["ping"].get(a + "|" + b)

    def perf(self, t):
        v = self.T["perf"].get(t, 0)
        if v == "raise":
            raise RuntimeError("x")
        return v

    def link(self, t):
        v = self.T["link"].get(t, True)
        if v == "raise":
            raise RuntimeError("x")
        return v

    def pool(self, ts):
        m = self.pool_mode
        if m == "raise":
            raise RuntimeError("x")
        if m == "first":
            return ts[:1], "pool first"
        if m == "dropfirst":
            return ts[1:], "pool dropfirst"
        if m == "none":
            return [], "pool none"
        if m == "last2":
            return ts[-2:], "pool last2"
        return list(ts), "pool all"

    def conv(self, v):
        if isinstance(v, dict):
            return {str(k): self.conv(x) for k, x in v.items()}
        if isinstance(v, (set, frozenset)):
            return sorted(self.conv(x) for x in v)
        if isinstance(v, (list, tuple)):
            return [self.conv(x) for x in v]
        return v

    def hm_state(self):
        if self.hm is None:
            return None
        with self.hm.lock:
            return {"matches": [[mid, self.conv(m)] for mid, m in self.hm.matches.items()], "by_token": dict(self.hm.by_token)}

    def rp_state(self):
        if self.rp is None:
            return None
        r = self.rp
        return {"recs": self.conv(r.recs), "last_text": dict(r.last_text),
                "base": {mid: sorted([sorted(k), v] for k, v in d.items()) for mid, d in r.base.items()}}

    def exec(self, c):
        """Run one script line; returns the printable dict."""
        if "now" in c:
            NOW[0] = c["now"]
        del self.LOG[:]
        try:
            r = self.call(c)
        except Exception as e:
            r = {"err": type(e).__name__}
        return {"r": r, "l": list(self.LOG), "s": self.hm_state(), "p": self.rp_state()}

    def call(self, c):
        op = c["op"]
        hm_mod, hm, rp = self.hm_mod, self.hm, self.rp
        if op == "new":
            kw = dict(c.get("kw", {}))
            self.hm = hm_mod.HostMigration(c["enabled"], now=clock, log=self.logf, ping=self.ping if c.get("ping") else None,
                                           perf=self.perf if c.get("perf") else None, pool=self.pool if c.get("pool") else None,
                                           link_alive=self.link if c.get("link") else None, **kw)
            self.pool_mode = c.get("pool")
            return None
        if op == "poolmode":
            self.pool_mode = c["mode"]
            return None
        if op == "tab":
            t, k, v = c["t"], c["k"], c["v"]
            if v is None and t in ("ping", "perf", "link", "reg", "sid", "party", "waited", "hostok"):
                self.T[t].pop(k, None) if t != "ping" else self.T[t].__setitem__(k, None)
            else:
                self.T[t][k] = v
            return None
        if op == "start":
            return hm.start(c["mid"], c["players"])
        if op == "add":
            return hm.add_player(c["mid"], c["token"], c["slot"])
        if op == "end":
            return hm.end(c["mid"])
        if op == "has":
            return hm.has(c["token"])
        if op == "given_up":
            return hm.given_up(c["mid"])
        if op == "pending_stale":
            return hm.pending_stale(c["mid"], c["secs"])
        if op == "host_info":
            return self.conv(hm.host_info(c["mid"]))
        if op == "snapshot":
            return self.conv(hm.snapshot(c["mid"]))
        if op == "mids":
            return hm.mids()
        if op == "sizes":
            return [len(hm.matches), len(hm.by_token)]
        if op == "export":
            r = hm.export_match(c["mid"])
            self.exports[c["mid"]] = json.loads(json.dumps(r))
            return self.conv(r)
        if op == "import_last":
            src = c.get("src", c["mid"])
            return hm.import_match(c["mid"], json.loads(json.dumps(self.exports[src])))
        if op == "import_rec":
            return hm.import_match(c["mid"], c["rec"])
        if op == "host_token":
            return hm.host_token(c["token"])
        if op == "depart":
            return hm.depart(c["token"], c["reason"])
        if op == "bridge_gone":
            return hm.bridge_gone(c["token"])
        if op == "bridge_back":
            return hm.bridge_back(c["token"])
        if op == "poll":
            try:
                return list(hm.poll(c["token"], c["flags"], c["epoch"]))
            except ValueError:
                return None
        if op == "replaced_age":
            return hm.replaced_age(c["token"])
        if op == "freezes":
            return hm.freezes(c["mid"], c["slot"], c["win"])
        if op == "roster":
            return self.conv(hm.roster(c["mid"]))
        if op == "stale_slots":
            return self.conv(hm.stale_slots(c["mid"]))
        if op == "handover_ok":
            return list(hm.handover_ok(c["mid"]))
        if op == "handover":
            return hm.handover(c["mid"], c["slot"])
        if op == "handover_pending":
            return hm.handover_pending(c["mid"])
        if op == "tick":
            return hm.tick()
        if op == "hlp":
            m = hm.matches.get(c["mid"])
            return bool(m and (m.get("lost") or m.get("pending")) and not m.get("given_up"))
        # ---- HostRepick
        if op == "rp_new":
            self.rp = hm_mod.HostRepick(now=clock, log=self.logf, mode=c.get("mode"))
            return None
        if op == "rp_forget":
            return rp.forget(c["mid"])
        if op == "rp_due":
            return rp.due(c["mid"], c["started"])
        if op == "rp_observe":
            return rp.observe(c["mid"], {(a, b): v for a, b, v in c["rtt"]})
        if op == "rp_saturated":
            return list(rp.saturated(c["mid"], c["host"], c["others"], self.ping))
        if op == "rp_migrated":
            return rp.migrated(c["mid"], frozenset(c["roster"]))
        if op == "rp_check":
            ii = c.get("info")
            info = None
            if ii is not None:
                info = {"rtt": {(a, b): v for a, b, v in ii.get("rtt", [])}, "perf": {int(k): v for k, v in ii.get("perf", {}).items()},
                        "loss": {int(k): v for k, v in ii.get("loss", {}).items()}}
                if "age" in ii:
                    info["age"] = ii["age"]
            cp = c.get("cap")
            cap = None
            if cp is not None:
                cap = {}
                if "host_bad" in cp:
                    cap["host_bad"] = cp["host_bad"]
                if "rank" in cp:
                    cap["rank"] = {int(k): v for k, v in cp["rank"].items()}
                if "max_rank" in cp:
                    cap["max_rank"] = cp["max_rank"]
                if "text" in cp:
                    cap["text"] = {int(k): v for k, v in cp["text"].items()}
                if "saturated" in cp:
                    cap["saturated"] = cp["saturated"]
            labels = None if c.get("labels") is None else {int(k): v for k, v in c["labels"].items()}
            scores = {s: (None if k is None else tuple(k)) for s, k in c["scores"]}
            return list(rp.check(c["mid"], c["started"], frozenset(c["roster"]), c["host"], scores, c["left"], c.get("busy"), labels, info, cap))
        # ---- PerfScores / pure functions
        if op == "ps_report":
            return self.ps.report(c["token"], dict(c["rep"]), c["steam"])
        if op == "ps_prune":
            return self.ps.prune()
        if op == "ps_get":
            return self.conv(self.ps.get(c["token"], c["steam"]))
        if op == "ps_penalty":
            return self.ps.penalty(c["token"], c["steam"])
        if op == "ps_dump":
            return {"by_token": self.conv(self.ps.by_token), "by_steam": self.conv(self.ps.by_steam)}
        if op == "parse_perf":
            r = hm_mod.parse_perf(bytes.fromhex(c["hex"]))
            return None if r is None else [r[0], self.conv(r[1])]
        if op == "perf_penalty":
            return hm_mod.perf_penalty(c["rep"])
        if op == "host_key":
            return list(hm_mod.host_key(c["pings"], c["pen"]))
        if op == "region_of":
            return hm_mod.region_of(c["loc"])
        if op == "parse_pins":
            return {str(k): v for k, v in hm_mod.parse_pins(c["text"]).items()}
        if op == "region_first":
            T = self.T
            mem = (lambda t: list(T["party"].get(t, [t]))) if c["members"] else None
            wf = (lambda t: T["waited"].get(t, 0.0)) if c["waited"] else None
            r = hm_mod.region_first(c["tokens"], lambda t: T["reg"].get(t), mem, wf, c["max_wait"])
            return [r[0], r[1]]
        if op == "ping_fit":
            T = self.T
            units = (lambda t: list(T["party"].get(t, [t]))) if c["units"] else None
            hok = (lambda t: T["hostok"].get(t, True)) if c["hostok"] else None
            r = hm_mod.ping_fit(c["players"], self.ping, c["limit"], units_fn=units, host_ok=hok)
            return [r[0], r[1], r[2], r[3]]
        if op == "region_pool":
            T = self.T
            pins = None if c["pins"] is None else {int(k): v for k, v in c["pins"].items()}
            r = hm_mod.region_pool(c["players"], lambda t: T["reg"].get(t), lambda t: T["sid"].get(t, 0), self.ping, pins=pins, everyone=c["everyone"])
            return [r[0], r[1]]
        raise KeyError(op)


def main(argv):
    eng = Engine(argv[1])
    out = sys.stdout
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        res = eng.exec(json.loads(line))
        out.write(json.dumps(res, sort_keys=True, separators=(",", ":")) + "\n")


if __name__ == "__main__":
    main(sys.argv)
