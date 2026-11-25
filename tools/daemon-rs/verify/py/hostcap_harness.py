"""Differential test driver for hostcap.py (see PORTING.md "Verification").

usage: python3 hostcap_harness.py <dir with hostcap.py> < script > out        (Rust side: t2r-daemon selftest hostcap < script > out)

JSON lines {"op": ..., "now": float, ...}; the first line must be {"op": "init", "db": "<path>"} (the sqlite file is deleted first).
Output per call: {"r": result, "s": {in-memory maps}, "d": {table dumps}} with sorted keys, compact separators."""
import json, os, sqlite3, sys

for _k in [k for k in os.environ if k.startswith("T2R_")]:
    del os.environ[_k]

NOW = [0.0]


def clock():
    return NOW[0]


import time
time.time = clock
time.monotonic = clock


class Engine:
    def __init__(self, mod_dir):
        sys.path.insert(0, mod_dir)
        import hostcap
        self.m = hostcap
        self.path = None
        self.hc = None
        self.V = {}

    def connect(self):
        return sqlite3.connect(self.path)

    def dump_tables(self):
        out = {}
        c = sqlite3.connect(self.path)
        try:
            for t in ("host_cap", "oneway"):
                try:
                    cur = c.execute("SELECT * FROM %s ORDER BY token" % t)
                    out[t] = {"cols": [d[0] for d in cur.description], "rows": [list(r) for r in cur.fetchall()]}
                except Exception:
                    out[t] = None
        finally:
            c.close()
        return out

    def state(self):
        hc = self.hc
        if hc is None:
            return None
        return {"mlag": sorted([[k, v] for k, v in hc.mlag.items()]), "streak": sorted([[k[0], k[1], v] for k, v in hc.streak.items()]),
                "schema_ok": hc._schema_ok}

    def exec(self, c):
        if "now" in c:
            NOW[0] = c["now"]
        try:
            r = self.call(c)
        except Exception:
            r = {"err": 1}
        return {"r": r, "s": self.state(), "d": self.dump_tables() if self.path else None}

    def call(self, c):
        op, m, hc = c["op"], self.m, self.hc
        if op == "init":
            self.path = c["db"]
            if os.path.exists(self.path):
                os.remove(self.path)
            return None
        if op == "new":
            self.hc = m.HostCap(self.connect, now=clock, log=lambda *a: None)
            return None
        if op == "sql":
            cn = self.connect()
            try:
                cn.execute(c["q"])
                cn.commit()
            finally:
                cn.close()
            return None
        if op == "vtab":
            self.V = {k: tuple(v) for k, v in c["t"].items()}
            return None
        if op == "get":
            return hc.get(c["token"])
        if op == "lagged_in":
            return hc.lagged_in(c["mid"], c["token"])
        if op == "host_report":
            return hc.host_report(c["token"], c["n"], c["avg"], c["peak"], c["mid"])
        if op == "client_report":
            return hc.client_report(c["mid"], c["host"], c["token"], c["loss10"])
        if op == "note_lag":
            return hc.note_lag(c["host"], c["mid"])
        if op == "forget":
            return hc.forget_matches(set(c["live"]))
        if op == "proven_for":
            return hc.proven_for(c["token"], c["n"])
        if op == "verdict":
            return list(hc.verdict(c["token"], c["n"]))
        if op == "oneway_note":
            return list(hc.oneway_note(c["token"], c["mid"], c["host"]))
        if op == "oneway_get":
            return list(hc.oneway_get(c["token"], c["days"]))
        if op == "pick":
            cands = [(p, tuple(k)) for p, k in c["cands"]]
            vf = hc.verdict if c["src"] == "db" else (lambda t, n: self.V.get(t, (1, "unknown")))
            r = m.pick(cands, c["n"], vf, window_ms=c["window"], label=lambda t: "L" + t)
            return [r[0], r[1]]
        if op == "parse_cap":
            return m.parse_cap(bytes.fromhex(c["hex"]))
        if op == "parse_oneway":
            return m.parse_oneway(bytes.fromhex(c["hex"]))
        if op == "need_kb":
            return m.need_kb(c["n"])
        if op == "need_kb_relay":
            return m.need_kb_relay(c["n"])
        raise KeyError(op)


def main(argv):
    eng = Engine(argv[1])
    for line in sys.stdin:
        line = line.strip()
        if line:
            sys.stdout.write(json.dumps(eng.exec(json.loads(line)), sort_keys=True, separators=(",", ":")) + "\n")


if __name__ == "__main__":
    main(sys.argv)
