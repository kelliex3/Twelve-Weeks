"""Differential test of tools/clan_perks.py + clan_status.py + clan_ingame.py + achievements.py against `t2r-daemon selftest clan_cl6`
(also `clan_perks`, `clan_status`, `clan_ingame`, `achievements`: one script format).

  T2R_PY_DIR=<dir with the deployed .py files> python3 clan_cl6_harness.py gen SEED N > script.jsonl
  T2R_PY_DIR=<dir> T2R_CL_DB=<fresh sqlite file> python3 clan_cl6_harness.py run < script.jsonl > out.py.jsonl      (cwd = the python dir)
  T2R_CL_DB=<other file> t2r-daemon selftest clan_cl6 < script.jsonl > out.rs.jsonl                                 (cwd = the python dir)

One JSON line per call: {"i", "r"[, "h" = crc32 of the story state after the call][, "err"]}, then {"i": -1, "final": the states} and a dump of every table.
The synthetic clans are made at generation time by the REAL Python pipeline (clan.apply_match + clan_story.on_day, frozen time) in a scratch in-memory
database; the script carries its rows (op "rows"), so both sides read the same inputs.  clan_journal / clan_journal_entries (not part of this port)
are replaced on the run side by deterministic doubles (same specs in src/clan/ext_clan_perks.rs); everything else (clan_loot, clan_chronicle,
clan_story, clan_continuity, clan_kids ...) is the real module on both sides.
"""
import json
import os
import random
import sqlite3
import sys
import time
import types
import zlib

PYDIR = os.environ.get("T2R_PY_DIR") or os.path.expanduser("~/rsverify/daemon/py")
sys.path.insert(0, PYDIR)
os.chdir(PYDIR)

LANGS = ["en", "pl", "pl-PL", "xx", "PL", "en", "pl"]
MAPS = ["Bookstore (Part I)", "Capitol Hill Shops", "Convention Center", "Hospital", "Suburb", "Zzz Unknown Map", "", "Mall", "Gas Station"]
T = [1700000000.0]


def dumps(o):
    return json.dumps(o, separators=(",", ":"))


def crc(o):
    return zlib.crc32(dumps(o).encode("utf-8")) & 0xFFFFFFFF


# ------------------------------------------------------------------ the doubles (same specs in src/clan/ext_clan_perks.rs)
def install_doubles():
    import clan
    jr = types.ModuleType("clan_journal")
    jr.render = lambda sent, lang="en": "JL:%s:%s" % (sent, lang) if isinstance(sent, str) and sent != "x" else ""

    def add_perk(c, tid, st, day, cat, who, mapname=""):
        if day % 11 == 0:
            raise ValueError("double")
        st.setdefault("ch", {}).setdefault("pkj", []).append([day, cat, who["n"], mapname])
        return cat in ("med", "ammo")
    jr.add_perk = add_perk

    def entries(c, tid, lang="en", days_done=None):
        dd = int(days_done or 0) if days_done is not None else 0
        n = 0 if dd % 5 == 0 else max(0, min(90, dd))      # (no entries every 5th day: the older feed rows then fill the page)
        out = []
        for d in range(n):
            wk, dow = clan.week_day(d)
            out.append({"day": d, "week": wk, "dow": dow if d > 0 else 0, "id": 100 + d, "title": "Title %d %s|x" % (d, lang), "tone": ["good", "warn", "bad", "info"][d % 4],
                        "map": "Map%d" % (d % 5), "win": [1, 0, None][d % 3], "sup": d, "need": d + 1, "h": d % 7, "u": (d * 3) % 5, "s": d % 2, "dn": d % 3,
                        "paras": [["RAID", "Para %d a" % d], ["CAMP", "Para %d b\tx" % d]]})
        return out
    jr.entries = entries
    sys.modules["clan_journal"] = jr

    je = types.ModuleType("clan_journal_entries")

    def compose_entries(rows, roster, seen_id=0):
        out = []
        for m in rows:
            text = m.get("text") or ""
            i = int(m.get("id") or 0)
            out.append({"prim": m, "head": text.split(".")[0], "body": text, "authors": list((m.get("names") or [])[:2]), "id": i, "new": 1 if i > seen_id else 0})
        return out
    je.compose_entries = compose_entries
    sys.modules["clan_journal_entries"] = je


# ------------------------------------------------------------------ generator
def tables_dump(db, names):
    out = []
    for t in names:
        try:
            cols = [r[1] for r in db.execute("PRAGMA table_info(%s)" % t)]
            rows = [list(r) for r in db.execute("SELECT * FROM %s" % t)]
        except sqlite3.Error:
            continue
        if cols and rows:
            out.append({"op": "rows", "table": t, "cols": cols, "rows": rows})
    return out


def make_world(r, seed):
    """Real Python pipeline on a scratch database: a few clans, each a number of days. Returns (ops to seed the DB, clan descriptors)."""
    import clan
    import clan_story as CS
    clan.rng = random.Random(seed + 1)
    t = [1700000000.0 + r.randrange(1000000)]
    time.time = lambda: t[0]
    db = sqlite3.connect(":memory:")
    clans = []
    mids = []
    for ci in range(r.randint(2, 4)):
        tid = "s:%d" % r.randrange(10 ** 16, 10 ** 17)
        fac = r.choice([clan.WLF, clan.SERAPHITES])
        clan.choose_side(db, tid, fac)
        CS.on_new_run(db, tid, fac)
        mode = r.choice(["starve", "feast", "mixed", "mixed", "mixed"])
        days = r.randint(6, 34)
        for d in range(days):
            ld = clan.load(db, tid)
            if ld["status"] != clan.ALIVE:
                break
            size = ld["healthy"] + ld["hungry"] + ld["sick"]
            need = clan.needed(size)
            fm = {"starve": [0, 0, .2, .5], "feast": [1.2, 2, 3, 6], "mixed": [0, .3, .7, 1, 1.1, 1.5, 2.5]}[mode]
            parts = int(need * 50 * r.choice(fm) * r.choice([0.8, 1, 1.2]))
            counts = {1: r.choice([0, 0, 2, 5, 9, 12, 20, 40])} if r.random() < 0.6 else {}
            for lab in (2, 3, 4):
                if r.random() < 0.25:
                    counts[lab] = r.randrange(0, 5)
            if r.random() < 0.5:
                counts[31] = r.randrange(0, 4096)
                counts[32] = r.randrange(0, 256)
            mid = "m%08x" % r.getrandbits(32)
            new, rr = clan.apply_match(db, tid, mid, parts, r.random() < 0.06, counts, r.choice([1.0, 1.0, 1.0, 0.5, 1.4]), r.random() < 0.05, r.choice([0, 0, 0, 2, 5]))
            if rr is None:
                continue
            t[0] += r.choice([0, 3, 61, 900, 4000, 90000])
            CS.on_day(db, tid, new, rr)
            db.commit()
            mids.append((tid, mid, d))
        clans.append({"tid": tid, "fac": fac})
    # some clans end: dead or won (end screens, section 7)
    for cd in clans:
        x = r.random()
        if x < 0.25:
            db.execute("UPDATE clans SET status=? WHERE token_id=?", (2, cd["tid"]))
        elif x < 0.4:
            db.execute("UPDATE clans SET status=?, runs_won=runs_won+1 WHERE token_id=?", (3, cd["tid"]))
    db.commit()
    db.execute("DELETE FROM clan_feed WHERE lid LIKE 'al/%'")        # (the A-Life port of CL3 is not in this branch: its rows cannot be rendered on the Rust side yet)
    for cd in clans:
        # the daemon's match ids on the raid log (the key of the RAIDS page's player stats; the scratch pipeline ran without them)
        st = CS._load(db, cd["tid"])
        if st and (st.get("ch") or {}).get("log"):
            for e in st["ch"]["log"]:
                if not e.get("mid") and r.random() < 0.9:
                    e["mid"] = "mid%06x" % r.getrandbits(24)
            CS._save(db, cd["tid"], st)
    db.commit()
    ops = tables_dump(db, ["clans", "clan_story", "clan_feed"])
    return ops, clans, db, t[0]


def person_pool(r):
    import clan_story_data as D
    return D


def gen(seed, n):
    r = random.Random(seed)
    import clan
    import clan_story as CS
    import achievements as A
    import clan_perks as P
    out = []

    def emit(o):
        o["t"] = round(tt[0], 3)
        out.append(o)

    ops, clans, db, tend = make_world(r, seed)
    tt = [tend]
    out.append({"op": "ensure", "t": round(tend, 3)})
    for o in ops:
        emit(o)
    emit({"op": "sql", "sql": "CREATE TABLE IF NOT EXISTS players(token TEXT PRIMARY KEY, steamid INTEGER)", "args": []})
    toks = []
    for i in range(r.randint(3, 8)):
        tok = "%064x" % r.getrandbits(256)
        toks.append(tok)
        cd = r.choice(clans)
        sid = int(cd["tid"][2:]) if r.random() < 0.7 else r.choice([0, 12345, None])
        emit({"op": "sql", "sql": "INSERT OR REPLACE INTO players(token, steamid) VALUES(?,?)", "args": [tok, sid]})
    # the story states (keyed ops)
    keys = []
    for cd in clans:
        row = db.execute("SELECT state FROM clan_story WHERE token_id=?", (cd["tid"],)).fetchone()
        if row:
            st = json.loads(row[0])
            cd["st"] = st
            keys.append(cd["tid"])
            emit({"op": "load", "k": cd["tid"], "st": st})
    # the per-match player stats of every logged raid
    mids = []
    for cd in clans:
        for e in ((cd.get("st") or {}).get("ch") or {}).get("log") or []:
            if e.get("mid"):
                mids.append((cd["tid"], e["mid"], e.get("d")))
    for tid, mid, _d in mids:
        if r.random() < 0.85:
            players = []
            for j in range(r.randint(2, 14)):
                tok = r.choice(toks) if r.random() < 0.5 else "%064x" % r.getrandbits(256)
                players.append({"tok": tok, "name": r.choice(["Ann", "Bob", "Zoë", "Łucja", "Joel", "A|B\tC", "", "x" * 40]), "team": r.choice([0, 1, 0, 1, -1]), "left": r.random() < 0.15})
            emit({"op": "i_roster", "mid": mid, "map": r.choice(MAPS), "rows": players, "winner": r.choice([0, 1, 2, 0, 1])})
            for p in players:
                if r.random() < 0.8:
                    cnt = {}
                    for lab in (1, 2, 3, 4, 21, 22):
                        if r.random() < 0.6:
                            cnt[str(lab)] = r.randrange(0, 15)
                    emit({"op": "i_report", "mid": mid, "token": p["tok"], "name": p["name"], "team": r.choice([p["team"], -1, 0, 1]), "counts": cnt if r.random() < 0.9 else {}, "left": r.random() < 0.1})
                    if r.random() < 0.7:
                        emit({"op": "i_parts", "mid": mid, "token": p["tok"], "parts": r.randrange(0, 900)})
    ach_keys = ["s:%d" % r.randrange(10 ** 5, 10 ** 6) for _ in range(3)]
    dnames = ["d1", "d2", "d3", "d4"]
    shadow = {k: [p["i"] for p in cd["st"]["people"]] for k, cd in ((c["tid"], c) for c in clans if c.get("st"))}
    nextpid = 5000
    langs = LANGS
    while len(out) < n:
        x = r.random()
        if x < 0.14:
            # clan_ingame.build
            tid = r.choice(clans)["tid"] if r.random() < 0.95 else "nope"
            sec = r.choice([1, 1, 2, 2, 3, 3, 4, 5, 6, 6, 7, 7, 0, 8])
            seen_day = 0
            cd = next((c for c in clans if c["tid"] == tid), None)
            if cd and r.random() < 0.7:
                lg = ((cd.get("st") or {}).get("ch") or {}).get("log") or []
                if lg and sec == 5:
                    seen_day = r.choice(lg).get("d") or 0
                elif sec == 6:
                    seen_day = r.choice([0, 1, 2, 3, 4, 99])
                elif lg:
                    seen_day = r.choice([0, r.choice(lg).get("d") or 0])
            emit({"op": "i_build", "tid": tid, "lang": r.choice(langs), "section": sec, "seen_id": r.choice([0, 0, 1, 40, 150, 100000]), "seen_day": seen_day,
                  "token": r.choice(toks + [None]) if r.random() < 0.9 else None})
        elif x < 0.18:
            emit({"op": "i_heads", "rows": [["%064x" % r.getrandbits(256) if r.random() < 0.6 else r.choice(toks), r.choice([0, 1, -1])] for _ in range(r.randint(0, 14))]})
        elif x < 0.20:
            emit({"op": "i_rank", "token": r.choice(toks + ["zz", "%064x" % r.getrandbits(256)])})
        elif x < 0.40 and keys:
            # clan_status on a story state: mutate, observe, read
            k = r.choice(keys)
            st = next(c["st"] for c in clans if c["tid"] == k)
            sh = shadow[k]
            y = r.random()
            if y < 0.5:
                for _ in range(r.choice([0, 1, 1, 2, 4])):
                    z = r.random()
                    if z < 0.5 and sh:
                        emit({"op": "pset", "k": k, "i": r.choice(sh), "f": "s", "v": r.choice(["h", "h", "u", "s"])})
                        if r.random() < 0.4:
                            emit({"op": "pset", "k": k, "i": r.choice(sh), "f": "il", "v": r.choice(["spores", "fever", "flu", ""])})
                    elif z < 0.7 and len(sh) > 3:
                        i = r.choice(sh)
                        sh.remove(i)
                        emit({"op": "kill", "k": k, "i": i, "d": st.get("day", 0), "c": r.choice(["hunger", "bite", "fight", "walkout"])})
                    elif z < 0.85:
                        nextpid += 1
                        p = {"i": nextpid, "n": r.choice(["Ann", "Bob", "Zoë", "Łucja", "Joel", "Mara", "Lev"]) + str(nextpid), "l": "X", "g": r.choice(["m", "f", "n"]), "r": r.choice(["scavenger", "kid", "elder", "medic", "cook", "guard", "scout", "mechanic", "gardener"]),
                             "s": r.choice(["h", "u", "s"]), "il": r.choice(["", "", "flu"]), "is": 0, "id": 0, "k": 0, "jd": 0, "gr": 0, "gl": 0}
                        sh.append(nextpid)
                        emit({"op": "add", "k": k, "p": p})
                    else:
                        # memory / depression counters (the inputs of code_of)
                        if sh:
                            if r.random() < 0.5:
                                emit({"op": "set", "k": k, "path": ["ch", "dep"], "v": {str(i): {"n": r.choice([0, 1, 3, 4, 5]), "d": r.choice([-1, st.get("day", 0), st.get("day", 0) - 1]), "up": -1, "rec": r.choice([True, False])} for i in r.sample(sh, min(len(sh), r.randint(1, 3)))}})
                            else:
                                emit({"op": "set", "k": k, "path": ["ch", "mem"], "v": {str(i): {"grief": r.choice([None, {"d": st.get("day", 0), "until": st.get("day", 0) + 3}, {"d": 1, "until": 99}, {"d": 1, "until": 0}]), "inj": r.choice([0, 1])} for i in r.sample(sh, min(len(sh), r.randint(1, 3)))}})
                if r.random() < 0.5:
                    emit({"op": "set", "k": k, "path": ["day"], "v": st.get("day", 0) + r.choice([0, 1, 1, 2, 5])})
                    st["day"] = st.get("day", 0) + 0   # (only the script's own day estimate; the real day is in the state on both sides)
                rows = [{"names": r.sample([p["n"] for p in st["people"]][:6], min(len(st["people"][:6]), r.randint(0, 2))) if st["people"] else [], "text": r.choice(["DAY 4: THE FIRE AT LEV'S. We ran.", "DZIEŃ 9: POŻAR. Ucieklismy.", "DAY 12: lev and ANA arrive. x", "no head here", "", "DAY 3: ."])}
                        for _ in range(r.randint(0, 3))]
                emit({"op": "s_observe", "k": k, "rows": rows})
            elif y < 0.65:
                emit({"op": "s_code", "k": k})
            elif y < 0.8:
                emit({"op": "s_public", "k": k, "fk": r.choice(["W", "S"]), "lang": r.choice(langs)})
            elif y < 0.88:
                emit({"op": "s_wire", "k": k})
            elif y < 0.92:
                emit({"op": "s_pending", "k": k})
            else:
                e = {"i": r.randint(1, 30), "n": r.choice(["Ann", "Zoë", "Łucja", "A_b-c'd", "x" * 30, "Joel"]), "g": r.choice(["m", "f", "n", None]), "r": r.choice(["scavenger", "kid", ""]), "c": r.randint(1, 9), "f": r.randint(0, 9), "d": r.randint(0, 40)}
                if r.random() < 0.5:
                    e["ln"] = r.choice(["Lev is back.", ""])
                if r.random() < 0.4:
                    e["jl"] = r.choice(["j7", "x", "", 5])
                emit({"op": r.choice(["s_line", "s_images"]), "k": k, "e": e, "lang": r.choice(langs), "fk": r.choice(["W", "S"]), "nost": r.random() < 0.3})
        elif x < 0.58 and keys:
            # clan_perks
            k = r.choice(keys)
            cd = next(c for c in clans if c["tid"] == k)
            row = db.execute("SELECT token_id FROM clans WHERE token_id=?", (k,)).fetchone()
            ld = clan.load(db, k) if row else clan.new_clan(1)
            ld = dict(ld)
            ld["healthy"] = r.randint(0, 12)
            ld["hungry"] = r.choice([0, 0, 1, 3])
            ld["sick"] = r.choice([0, 0, 2])
            ld["status"] = r.choice([1, 1, 1, 1, 2, 3])
            meta = {"sup": {"stock": {c: r.choice([0, 1, 5, 12, 40, 200]) for c in ("food", "water", "med", "ammo", "mat")}}}
            if r.random() < 0.2:
                meta["sup"]["x"] = 1
            if r.random() < 0.1:
                meta = {}
            ld["meta"] = json.dumps(meta, separators=(",", ":"))
            ld["fac"] = r.choice([1, 2, None])
            y = r.random()
            if y < 0.3:
                emit({"op": "p_active", "k": k, "clan": ld, "nostory": r.random() < 0.15})
            elif y < 0.5:
                emit({"op": "p_wire", "k": k, "clan": ld, "nostory": r.random() < 0.1, "streak": r.choice([None, None, 0, 3, 12])})
            elif y < 0.56:
                emit({"op": "p_need", "clan": ld})
            elif y < 0.63:
                emit({"op": "p_uses", "counts": {"31": r.randrange(0, 4096), "32": r.randrange(0, 256), "x": 4}})
            elif y < 0.73:
                emit({"op": "p_credit_day", "k": k, "got": {c: r.choice([0, 0, 1, 3, 8]) for c in ("food", "water", "med", "ammo", "mat") if r.random() < 0.8} if r.random() < 0.9 else None, "day": r.randint(0, 80)})
            elif y < 0.8:
                emit({"op": "p_credit_of", "k": k, "cat": r.choice(["med", "ammo", "mat", "food", "water"])})
            elif y < 0.9:
                cat = r.choice(["med", "ammo", "mat", "food", "water"])
                emit({"op": "p_line", "cat": cat, "who": {"n": r.choice(["Ann", "Zoë", "Łucja"]), "g": r.choice(["f", "m", "n", 1, 0])}, "map": r.choice(MAPS + ["Hospital"]), "lang": r.choice(["en", "pl"]), "day": r.randint(0, 99)})
            else:
                emit({"op": "p_render", "lid": r.choice(["pk/use", "pk/", "ch/x", "", "pk/x"]), "params": r.choice([{"t": "EN text", "tp": "PL tekst"}, {"t": "only en"}, {}, [], None]), "lang": r.choice(langs)})
        elif x < 0.64 and clans:
            cd = r.choice(clans)
            tid = cd["tid"]
            ld = clan.load(db, tid)
            day = r.randint(0, 40)
            rr = {"day": day, "sup": {"got": {c: r.choice([0, 1, 4]) for c in ("food", "water", "med", "ammo", "mat")}}}
            if r.random() < 0.1:
                rr = r.choice([{}, None])
            newc = {"day": day + 1, "faction": cd["fac"]}
            if r.random() < 0.1:
                newc = r.choice([{}, None])
            emit({"op": "p_on_day", "tid": tid, "newc": newc, "r": rr, "counts": {"31": r.randrange(0, 4096), "32": r.randrange(0, 256)} if r.random() < 0.8 else {}, "match": {"map": r.choice(MAPS)} if r.random() < 0.7 else None})
        elif x < 0.97:
            # achievements
            key = r.choice(ach_keys)
            dn = r.choice(dnames)
            y = r.random()
            labs = list(range(1, 128)) + [200, 201]

            def counts():
                c = {}
                for _ in range(r.randint(0, 14)):
                    lab = r.choice([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16, 17, 20, 21, 22, 23, 24, 33, 49] + list(range(64, 91)) + [r.choice(labs)])
                    c[str(lab)] = r.choice([-1, 0, 1, 2, 3, 5, 9, 10, 14, 30, 59, 60, 61, 100, 500])
                if r.random() < 0.05:
                    c["bad"] = 3
                return c
            now = r.choice([None, None, round(tt[0] + r.randint(0, 99), 2)])
            if y < 0.3:
                emit({"op": "a_book", "key": key, "d": dn, "counts": counts(), "win": r.choice([None, True, False]), "left": r.random() < 0.15, "late": r.random() < 0.2, "first": r.random() < 0.6, "now": now})
            elif y < 0.45:
                emit({"op": "a_live", "key": key, "d": dn, "counts": counts(), "now": now})
            elif y < 0.52:
                emit({"op": "a_settle", "key": key, "d": dn, "auth": counts(), "labels": r.sample([21, 22, 23, 24, 33, 1, 4, 7], r.randint(0, 5))})
            elif y < 0.55:
                emit({"op": "a_side", "key": key, "now": now})
            elif y < 0.65:
                before = {"status": r.choice([1, 1, 2, 3]), "faction": r.choice([1, 2])}
                after = {"status": r.choice([1, 2, 3, 3]), "faction": r.choice([1, 2, 0]), "healthy": r.choice([0, 50, 100]), "hungry": r.choice([0, 3]), "sick": r.choice([0, 1]), "days_total": r.randint(0, 800)}
                rr = {"died": r.choice([0, 0, 1, 3]), "evk": r.choice([None, 1, 2]), "evt": r.choice([0, 2, 3]), "mdone": r.choice([0, 1, True]), "mt": r.choice([0, 2, 3])}
                emit({"op": "a_clan", "key": key, "before": before if r.random() < 0.9 else None, "after": after if r.random() < 0.9 else None, "r": rr if r.random() < 0.9 else None, "rank": r.choice([None, None, 0, 1, 7, 30, 150]), "now": now})
            elif y < 0.72:
                emit({"op": "a_san", "counts": counts()})
            elif y < 0.82:
                emit({"op": "a_wire", "key": key, "rank": r.choice([None, 0, 3, 120])})
            elif y < 0.9:
                emit({"op": "a_pub", "key": key, "rank": r.choice([None, 0, 3, 120])})
            elif y < 0.95:
                emit({"op": "a_rows", "key": key, "rank": r.choice([None, 0, 3, 120])})
            else:
                emit({"op": "a_table"})
        else:
            emit({"op": "portrait", "model": r.choice([100, 101, 105, 999]), "expr": r.choice(["neutral", "happy", "sad", "hungry", "dead", "angry", "perk", "depressed", "shock", "sick"])})
    for o in out[:n]:
        print(dumps(o))


# ------------------------------------------------------------------ run (the Python side)
def run():
    install_doubles()
    import achievements as A
    import clan
    import clan_ingame as I
    import clan_perks as P
    import clan_status as S
    import clan_story as CS
    time.time = lambda: T[0]
    dbp = os.environ.get("T2R_CL_DB") or "/tmp/clan_cl6_py.db"
    for p in (dbp, dbp + "-journal"):
        try:
            os.remove(p)
        except OSError:
            pass
    c = sqlite3.connect(dbp)
    sts = {}
    dones = {}
    prints = []

    def cnt(o):
        return o

    for i, line in enumerate(sys.stdin):
        line = line.strip()
        if not line:
            continue
        o = json.loads(line)
        if "t" in o:
            T[0] = o["t"]
        op = o["op"]
        res = {"i": i}
        k = o.get("k")
        st = sts.get(k)
        lang = o.get("lang", "en")
        try:
            if op == "load":
                sts[k] = o["st"]
                res["r"] = True
            elif op == "set":
                if st is not None:
                    d = st
                    for kk in o["path"][:-1]:
                        d = d[kk]
                    d[o["path"][-1]] = o["v"]
                    res["r"] = None
                    res["h"] = crc(st)
            elif op == "pset":
                if st is not None:
                    for p in st["people"]:
                        if p["i"] == o["i"]:
                            p[o["f"]] = o["v"]
                            break
                    res["r"] = None
                    res["h"] = crc(st)
            elif op == "kill":
                if st is not None:
                    for p in list(st["people"]):
                        if p["i"] == o["i"]:
                            st["people"].remove(p)
                            st["dead"].append({"i": p["i"], "n": p["n"], "g": p["g"], "r": p["r"], "d": o["d"], "c": o["c"], "il": p["il"], "k": 0})
                            break
                    res["r"] = None
                    res["h"] = crc(st)
            elif op == "add":
                if st is not None:
                    st["people"].append(o["p"])
                    res["r"] = None
                    res["h"] = crc(st)
            elif op == "sql":
                c.execute(o["sql"], o["args"])
                c.commit()
                res["r"] = True
            elif op == "rows":
                for row in o["rows"]:
                    c.execute("INSERT OR REPLACE INTO %s(%s) VALUES(%s)" % (o["table"], ",".join(o["cols"]), ",".join("?" * len(o["cols"]))), row)
                c.commit()
                res["r"] = True
            elif op == "ensure":
                clan.ensure(c)
                CS.ensure(c)
                c.commit()
                res["r"] = True
            # ---------------------------------------------------------------- clan_status
            elif op == "s_code":
                res["r"] = [S.code_of(st, p) for p in st["people"]]
            elif op == "s_observe":
                res["r"] = S.observe(st, o["rows"])
                res["h"] = crc(st)
            elif op == "s_pending":
                res["r"] = S.pending(st)
            elif op == "s_public":
                res["r"] = S.public(st, o["fk"], lang)
            elif op == "s_wire":
                res["r"] = S.wire_text(st)
            elif op == "s_line":
                res["r"] = S.story_line(o["e"], lang)
            elif op == "s_images":
                e = o["e"]
                res["r"] = [S.images(e, o["fk"], None if o["nost"] else st), S.sex_of(e), S.look_of(e), S.event_id(e)]
            # ---------------------------------------------------------------- clan_perks
            elif op == "p_active":
                res["r"] = P.active(o["clan"], None if o.get("nostory") else st)
            elif op == "p_wire":
                story = None if o.get("nostory") else st
                pk = P.active(o["clan"], story)
                streak = P.loss_streak(story) if o.get("streak") is None else o["streak"]
                res["r"] = [P.wire_text(pk, streak), P.loss_streak(story)]
            elif op == "p_need":
                cl = o["clan"]
                nd = P.need_of(cl)
                sp = P.surplus(cl)
                stk = P.stock_of(cl)
                order = ["food", "water", "med", "ammo", "mat"]
                res["r"] = [[nd[x] for x in order], [stk[x] for x in order], [sp[x] for x in order]]
            elif op == "p_uses":
                cn = {}
                for kk, v in o["counts"].items():
                    try:
                        cn[int(kk)] = v
                    except ValueError:
                        pass
                res["r"] = P.uses_of(cn)
            elif op == "p_credit_day":
                P.credit_day(st, o["got"], o["day"])
                res["r"] = None
                res["h"] = crc(st)
            elif op == "p_credit_of":
                res["r"] = P.credit_of(st, o["cat"])
            elif op == "p_line":
                res["r"] = P.line(o["cat"], o["who"], o["map"], lang, o["day"])
            elif op == "p_render":
                res["r"] = P.render_msg(o["lid"], o["params"], lang)
            elif op == "p_on_day":
                cn = {}
                for kk, v in o["counts"].items():
                    try:
                        cn[int(kk)] = v
                    except ValueError:
                        pass
                res["r"] = [list(x) for x in P.on_day(c, o["tid"], o["newc"], o["r"], cn, o["match"])]
                c.commit()
            # ---------------------------------------------------------------- achievements
            elif op == "a_table":
                import io
                ids = [a["id"] for a in A.ACH]
                res["r"] = [len(ids), len(A.live_rules()), A.ext_header()]
            elif op == "a_book":
                done = dones.setdefault(o["d"], {})
                got = A.book_match(c, o["key"], o["counts"], done, win=o["win"], left=o["left"], late=o["late"], first=o["first"], now=o["now"])
                c.commit()
                res["r"] = [got, {str(kk): done[kk] for kk in sorted(done)}]
            elif op == "a_live":
                done = dones.setdefault(o["d"], {})
                got = A.book_live(c, o["key"], o["counts"], done, now=o["now"])
                c.commit()
                res["r"] = [got, {str(kk): done[kk] for kk in sorted(done)}]
            elif op == "a_settle":
                done = dones.setdefault(o["d"], {})
                auth = {}
                for kk, v in o["auth"].items():
                    try:
                        auth[int(kk)] = v
                    except ValueError:
                        pass
                A.settle(c, o["key"], auth, done, o["labels"])
                c.commit()
                res["r"] = {str(kk): done[kk] for kk in sorted(done)}
            elif op == "a_side":
                res["r"] = A.book_side(c, o["key"], now=o["now"])
                c.commit()
            elif op == "a_clan":
                res["r"] = A.book_clan(c, o["key"], o["before"], o["after"], o["r"], rank_weeks=o["rank"], now=o["now"])
                c.commit()
            elif op == "a_san":
                res["r"] = {str(kk): v for kk, v in A.sanitize(o["counts"]).items()}
            elif op == "a_wire":
                res["r"] = A.wire_text(c, o["key"], o["rank"])
            elif op == "a_pub":
                res["r"] = A.public_rows(c, o["key"], o["rank"])
            elif op == "a_rows":
                res["r"] = [[a["id"], p, t] for a, p, t in A.rows(c, o["key"], o["rank"])]
            # ---------------------------------------------------------------- clan_ingame
            elif op == "i_build":
                res["r"] = I.build(c, o["tid"], o["lang"], o["section"], o["seen_id"], o["seen_day"], token=o["token"])
            elif op == "i_roster":
                I.stats_roster(c, o["mid"], o["map"], [(x["tok"], x["name"], x["team"], x["left"]) for x in o["rows"]], o["winner"])
                c.commit()
                res["r"] = True
            elif op == "i_report":
                cn = {}
                for kk, v in o["counts"].items():
                    try:
                        cn[int(kk)] = v
                    except ValueError:
                        pass
                I.stats_report(c, o["mid"], o["token"], o["name"], o["team"], cn, o["left"])
                c.commit()
                res["r"] = True
            elif op == "i_parts":
                I.stats_parts(c, o["mid"], o["token"], o["parts"])
                c.commit()
                res["r"] = True
            elif op == "i_heads":
                h = I.stats_heads([(a, b) for a, b in o["rows"]])
                res["r"] = [{kk: h[kk] for kk in sorted(h)}, len(I.stats_head_pool())]
            elif op == "i_rank":
                res["r"] = list(I.player_rank(c, o["token"]))
            elif op == "portrait":
                import clan_chronicle as CH
                res["r"] = CH.fit_expr(o["model"], o["expr"])
            else:
                raise ValueError("unknown op " + op)
        except Exception as ex:  # noqa: BLE001
            if os.environ.get("T2R_TRACE"):
                import traceback
                sys.stderr.write("call %d %s: %s" % (i, op, traceback.format_exc()))
            res = {"i": i, "r": None, "err": True}
        if os.environ.get("T2R_FULLST") and "h" in res:
            res["st"] = sts.get(k)
        print(dumps({kk: res[kk] for kk in ("i", "r", "h", "st", "err") if kk in res}))
    print(dumps({"i": -1, "final": sts}))
    for t, order in [("clans", "token_id"), ("clan_story", "token_id"), ("clan_feed", "token_id,id"), ("match_stats", "mid,token"), ("ach_count", "key,lab"), ("ach", "key,aid")]:
        try:
            rows = [list(r) for r in c.execute("SELECT * FROM %s ORDER BY %s" % (t, order))]
        except sqlite3.Error:
            rows = []
        print(dumps({"table": t, "rows": rows}))


if __name__ == "__main__":
    if sys.argv[1] == "gen":
        gen(int(sys.argv[2]), int(sys.argv[3]))
    else:
        run()
