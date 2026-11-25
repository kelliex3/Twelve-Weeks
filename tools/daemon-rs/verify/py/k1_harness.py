"""Differential test of daemon.py's K1 chunk (names / caches / leaderboard / Elo / record_result / bans / update_target / token helpers)
against `t2r-daemon selftest k1`.

  python3 k1_harness.py gen SEED N > script.jsonl
  T2R_DB=/tmp/k1py.db python3 k1_harness.py run < script.jsonl > out.py.jsonl
  T2R_DB=/tmp/k1rs.db t2r-daemon selftest k1 < script.jsonl > out.rs.jsonl ; cmp ...

Both sides use their own DB file in the same directory (bans.txt / update-data live next to it). Time is frozen per line ("t").
"""
import os
import shutil
import sys

os.environ.setdefault("T2R_DB", "/tmp/k1py.db")
os.environ["T2R_ELO_NO_PERF"] = "0"
import cseed  # noqa: E402
from cseed import T, emit, log, main, script  # noqa: E402

import time as _time  # noqa: E402
_time.time = lambda: T[0]

DB = os.environ["T2R_DB"]
DIR = os.path.dirname(os.path.abspath(DB))
NAMES = ["Alpha", "bravo", "Charlie Brown", "Charlie", "delta<f>", "Echo>", "Foxtrot", "golf", "Hotel", "india", "Juliet", "kilo", "<>"]


def setup():
    import daemon as d
    real = d.time

    class Shim:
        def time(self):
            return T[0]

        def monotonic(self):
            return T[0]

        def sleep(self, x):
            pass

        def __getattr__(self, k):
            return getattr(real, k)

    d.time = Shim()
    d.log = log
    d.evt = lambda kind, *a: log("EVT", kind, *a)
    d.SERVER_VERSION = "sv-test"
    d.BRIDGES.steamid = lambda t: 0
    return d


def reset(d):
    d._name_cache.clear()
    d._steam_cache.clear()
    d._tok_gen.clear()
    d._name_gen[0] = 0
    d._name_ver[0] = 0
    d._elo_pool.update(t=0.0, p=0.4)
    d._bans.update(mtime=None, ids=set(), names=set(), hw=set(), tok=set(), ip=[], pid=set(), reason={})
    d._target.update(t=0.0, v="")
    p = os.path.join(DIR, "bans.txt")
    if os.path.exists(p):
        os.remove(p)
    shutil.rmtree(os.path.join(DIR, "update-data"), ignore_errors=True)


def seed_py(d, s):
    with d._dblock, d.dbc() as c:
        c.execute("CREATE TABLE IF NOT EXISTS match_stats(mid TEXT, token TEXT, parts INT, kills INT, downs INT, deaths INT, revives INT)")
        d.names.init(c)
        d.elo_ensure(c)
        for t in ("players", "match_stats", "ratings", "names"):
            c.execute("DELETE FROM %s" % t)
        for r in s.get("players", []):
            c.execute("INSERT INTO players(token,name,steamid,kills,deaths,wins,matches,draws,last_seen) VALUES (?,?,?,?,?,?,?,?,?)", r)
        for r in s.get("stats", []):
            c.execute("INSERT INTO match_stats(mid,token,parts,kills,downs,deaths,revives) VALUES (?,?,?,?,?,?,?)", r)
        for r in s.get("names", []):
            c.execute("INSERT OR REPLACE INTO names(token_id,name,tag,updated) VALUES (?,?,?,?)", r)
    reset(d)


def state_py(d):
    import sqlite3
    c = sqlite3.connect(DB)
    out = {}
    for t, q in (("players", "SELECT token,name,loadout,kills,deaths,wins,matches,draws,steamid,last_seen FROM players ORDER BY token"),
                 ("ratings", "SELECT key,elo,n,updated FROM ratings ORDER BY key"), ("names", "SELECT token_id,name,tag,updated FROM names ORDER BY token_id")):
        try:
            rows = c.execute(q).fetchall()
        except Exception:
            rows = []
        out[t] = [[x.hex() if isinstance(x, bytes) else x for x in r] for r in rows]
    c.close()
    out["ver"] = d._name_ver[0]
    return out


def write_bans(content, mtime):
    p = os.path.join(DIR, "bans.txt")
    if content is None:
        if os.path.exists(p):
            os.remove(p)
        return
    with open(p, "wb") as f:
        f.write(content.encode("utf-8"))
    os.utime(p, (mtime, mtime))


def write_update(u):
    ud = os.path.join(DIR, "update-data")
    shutil.rmtree(ud, ignore_errors=True)
    if u is None:
        return
    os.makedirs(os.path.join(ud, "manifests", "stable"))
    os.makedirs(os.path.join(ud, "blobs"))
    for rel, txt in u.items():
        p = os.path.join(ud, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(txt.encode("utf-8"))


def bans_state(d):
    b = d._bans
    return {"mtime": b["mtime"], "ids": sorted(b["ids"]), "names": sorted(b["names"]), "hw": sorted(b["hw"]), "tok": sorted(b["tok"]),
            "ip": sorted(str(x) for x in b["ip"]), "pid": sorted(b["pid"]), "reason": dict(sorted(b["reason"].items()))}


def call(d, f, a):
    if f == "leaderboard":
        return [list(x) for x in d.leaderboard(*a)]
    if f == "elo_update":
        d.elo_update(a[0], a[1], set(a[2]), a[3])
        return None
    if f == "elo_of":
        rec = {k: tuple(v) for k, v in a[1].items()} if a[1] is not None else None
        return d.elo_of_tokens(a[0], rec)
    if f == "record_result":
        d.record_result(a[0], a[1], set(a[2]), None)
        return None
    if f == "get_name":
        return d.get_name(a[0]) or ""
    if f == "token_steamid":
        return d.token_steamid(a[0])
    if f == "custom_name":
        return list(d.custom_name(a[0]))
    if f == "name_key":
        return d.name_key(a[0])
    if f == "set_name":
        d.set_name(a[0], a[1])
        return None
    if f == "set_steam":
        d.set_steam(a[0], a[1], a[2])
        return None
    if f == "name_dirty":
        d.name_dirty(a[0], a[1])
        return None
    if f == "names_save":
        with d._dblock, d.dbc() as c:
            d.names.save(c, a[0], a[1], a[2])
        return None
    if f == "save_loadout":
        d.save_loadout(a[0], bytes.fromhex(a[1]))
        return None
    if f == "get_loadout":
        r = d.get_loadout(a[0])
        return None if r is None else bytes(r).hex()
    if f == "add_matches":
        d.add_matches(a[0])
        return None
    if f == "token_name":
        return d.token_name(a[0])
    if f == "tag":
        return d.tag(a[0])
    if f == "safe_name":
        return d.safe_name(a[0])
    if f == "name_taken":
        with d._dblock, d.dbc() as c:
            return d.name_taken(c, a[0], a[1])
    if f == "bans":
        write_bans(a[0], a[1])
        d._bans_load()
        return bans_state(d)
    if f == "update_target":
        write_update(a[0])
        d._target.update(t=0.0, v="")
        return d.update_target()
    if f == "elo_shown":
        return d.elo_shown(a[0], a[1])
    raise ValueError(f)


def run():
    d = setup()
    n = 0
    for c in script():
        T[0] = c.get("t", T[0])
        n += 1
        if c["op"] == "seed":
            seed_py(d, c["s"])
            emit(n, None, None)
            continue
        try:
            r = call(d, c["f"], c["a"])
        except Exception as ex:
            r = "EXC"
            log("EXC", type(ex).__name__)
        emit(n, r, state_py(d))


# ------------------------------------------------------------------ generator
def hx(r, n):
    return "".join(r.choice("0123456789abcdef") for _ in range(n))


def gen(r, n):
    toks = []
    for i in range(14):
        if r.random() < 0.5:
            nm = r.choice(NAMES).replace("<", "").encode()[:15]
            raw = b"T2N:" + nm + b"\0"
            raw += bytes(r.getrandbits(8) for _ in range(32 - len(raw)))
            toks.append(raw.hex())
        else:
            toks.append(hx(r, 64))
    sids = [76561198000000000 + r.randrange(10 ** 9) for _ in range(8)]
    t = [1000.0]

    def pick(k=None):
        k = k if k is not None else r.randint(0, 9)
        return [r.choice(toks) if r.random() < 0.93 else r.choice(["", "abc", hx(r, 12), toks[0].upper()]) for _ in range(k)]

    def nul(v):
        return None if r.random() < 0.15 else v

    def seed():
        pl = []
        for tk in toks:
            if r.random() < 0.2:
                continue
            sid = r.choice(sids) if r.random() < 0.5 else 0
            nm = r.choice(NAMES + [None, ""]) if r.random() < 0.8 else None
            m = r.randint(0, 40)
            pl.append([tk, nm, sid, r.randint(0, 300), r.randint(0, 300), r.randint(0, m), m, r.randint(0, min(m, 3)), t[0] - r.randint(0, 10 ** 6)])
        st = []
        for mid in ("m1", "m2", "m3"):
            for tk in r.sample(toks, r.randint(0, 8)):
                st.append([mid, tk, nul(r.randint(-5, 900)) if r.random() < 0.7 else None, nul(r.randint(-2, 30)), nul(r.randint(-1, 20)), nul(r.randint(-1, 20)), nul(r.randint(-1, 9))])
        nm = []
        for tk in toks:
            if r.random() < 0.25:
                key = ("s:%d" % r.choice(sids)) if r.random() < 0.5 else tk[40:64]
                nm.append([key, r.choice(NAMES[:8]), r.choice(["", "TAG", "XY"]), 5.0])
        return {"players": pl, "stats": st, "names": nm}

    yield {"op": "seed", "s": seed(), "t": t[0]}
    for _ in range(n):
        t[0] += r.choice([0, 0, 0.5, 2, 4, 40, 700])
        k = r.random()
        base = {"t": t[0], "op": "call"}
        if k < 0.02:
            yield {"op": "seed", "s": seed(), "t": t[0]}
            continue
        if k < 0.10:
            f, a = "leaderboard", [r.choice([1, 3, 10, 50])]
        elif k < 0.22:
            f, a = "elo_update", [pick(), r.choice([0, 1, 2, 2, 5, -1]), pick(r.randint(0, 2)), r.choice([None, "m1", "m2", "m3", "zz"])]
        elif k < 0.28:
            f = "elo_of"
            ts = pick()
            a = [ts, None if r.random() < 0.6 else {x: [r.randint(0, 9), r.randint(0, 9), r.randint(0, 3)] for x in ts if r.random() < 0.6}]
        elif k < 0.33:
            f, a = "record_result", [pick(), r.choice([0, 1, 2]), pick(r.randint(0, 2))]
        elif k < 0.45:
            f, a = r.choice(["get_name", "get_name", "token_steamid", "custom_name", "name_key", "token_name", "tag"]), [pick(1)[0] if True else None]
        elif k < 0.50:
            f, a = "set_name", [pick(1)[0], r.choice(NAMES)]
        elif k < 0.55:
            f, a = "set_steam", [pick(1)[0], r.choice(sids), r.choice(NAMES)]
        elif k < 0.60:
            f = "name_dirty"
            a = [r.choice([None, pick(1)[0]]), r.choice([None, "s:%d" % r.choice(sids), r.choice(toks)[40:64]])]
        elif k < 0.65:
            f, a = "names_save", [r.choice([("s:%d" % r.choice(sids)), r.choice(toks)[40:64]]), r.choice(NAMES + [""]), r.choice(["", "AB", "TOOLONGTAG"])]
        elif k < 0.68:
            f, a = "save_loadout", [pick(1)[0], hx(r, 20)]
        elif k < 0.70:
            f, a = "get_loadout", [pick(1)[0]]
        elif k < 0.73:
            f, a = "add_matches", [pick()]
        elif k < 0.76:
            f, a = "safe_name", [r.choice(NAMES + ["", " <a> ", "x"])]
        elif k < 0.80:
            f, a = "name_taken", [r.choice([("s:%d" % r.choice(sids)), r.choice(toks)[40:64]]), r.choice(NAMES + ["", "  "])]
        elif k < 0.84:
            f, a = "elo_shown", [r.uniform(500, 1500), r.randint(-3, 200)]
        elif k < 0.94:
            f = "bans"
            lines = []
            for _ in range(r.randint(0, 9)):
                c = r.random()
                if c < 0.2:
                    lines.append("%d" % r.choice(sids) + r.choice(["", " # reason: cheating", "  #REASON:  x y "]))
                elif c < 0.35:
                    lines.append(r.choice(NAMES).lower() + r.choice(["", " # reason: name ban", "#x"]))
                elif c < 0.45:
                    lines.append("hwid:" + hx(r, r.choice([10, 24, 30])))
                elif c < 0.55:
                    lines.append("token:" + r.choice(toks) if r.random() < 0.7 else "token:zz")
                elif c < 0.7:
                    lines.append("ip:" + r.choice(["203.0.113.7", "203.0.113.0/24", "10.1.2.3/255.255.0.0", "2001:db8::1", "2001:db8::/32", "300.1.1.1", "1.2.3.4/33", "1.2.3.4/0.0.0.255", "::/0"]))
                elif c < 0.8:
                    lines.append("pid:p-" + hx(r, 8))
                elif c < 0.9:
                    lines.append(r.choice(["# comment", "", "   ", "12345", "Some Name # reason: r1"]))
                else:
                    lines.append(r.choice(NAMES))
            content = None if r.random() < 0.12 else r.choice(["\n", "\r\n", "\r"]).join(lines)
            a = [content, t[0] + r.choice([0, 1, 1, 5])]
        else:
            f = "update_target"
            if r.random() < 0.3:
                u = None
            else:
                sha = hx(r, 64) if r.random() < 0.85 else "XYZ"
                u = {}
                if r.random() < 0.9:
                    u["channels.json"] = r.choice(['{"stable": "76"}', '{"stable": ""}', '{"stable": 5}', '{"stable": 0}', '[1]', 'nope'])
                    u["manifests/stable/76.json"] = '{"files": [{"path": "other"}, {"path": "version.txt", "sha256": "%s"}]}' % sha
                    u["manifests/stable/5.json"] = '{"files": []}'
                    if r.random() < 0.8:
                        u["blobs/" + sha] = r.choice(["  0f44cc94\n", "abc", "\n"])
            a = [u]
        yield dict(base, f=f, a=a)


if __name__ == "__main__":
    main(gen, run)
