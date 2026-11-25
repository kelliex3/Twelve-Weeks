"""Differential test of tools/clan_chronicle.py + tools/clan_arcs.py against `t2r-daemon selftest clan_chronicle`.

  T2R_PY_DIR=<dir with the deployed .py files> python3 clan_chronicle_harness.py gen SEED N > script.jsonl
  T2R_PY_DIR=<dir> T2R_CL_DB=<fresh sqlite file> python3 clan_chronicle_harness.py run < script.jsonl > out.py.jsonl
  T2R_CL_DB=<other file> (cwd = the python dir) t2r-daemon selftest clan_chronicle < script.jsonl > out.rs.jsonl

One JSON line per call: {"i", "r"} (+ "st" = the whole story state after the call for the stateful ops, key order kept).  The story states are built by the
real clan_story functions on the Python side (generator) and travel inside the script ("load" ops); the day inputs are explicit script data.
Modules that are not part of this port (clan_saga, clan_loot, clan_status, clan_story.spare_sick) are replaced on BOTH sides by the same small deterministic
test doubles (here: install_doubles(); Rust: src/clan/ext_clan_chronicle.rs, active only in the selftest).  clan_alife_lex and clan_story.finish_member are real.
"""
import json
import os
import random
import sqlite3
import sys
import types
import zlib

PYDIR = os.environ.get("T2R_PY_DIR") or os.path.expanduser("~/rsverify/daemon/py")
sys.path.insert(0, PYDIR)
os.chdir(PYDIR)

MAPS = ["Bookstore (Part I)", "Capitol Hill Shops", "Convention Center", "Hospital", "Suburb", "Zzz Unknown Map", "", "Mall", "Gas Station"]
LANGS = ["en", "pl", "pl-PL", "xx", "PL", "1"]


def dumps(o):
    return json.dumps(o, separators=(",", ":"))


def _h(*parts):
    return zlib.crc32("|".join(str(x) for x in parts).encode("utf-8")) & 0xFFFFFFFF


# ------------------------------------------------------------------ test doubles (same on the Rust side)
def install_doubles():
    import clan_chronicle as CH
    import clan_story as CS

    saga = types.ModuleType("clan_saga")
    saga.fit = lambda cs, kind, p: (p["i"] * 7 + len(kind)) % 5 - 2

    def saga_run(d, st, newc, r, match, cs, rng, chm, loot, died_now, arc_rows=()):
        rng.random()
        h = CH._h("fakesaga", st.get("tid", ""), d.day)
        cs["sg_n"] = cs.get("sg_n", 0) + 1
        if not d.alive:
            return []
        first, last = d.alive[0], d.alive[-1]
        d.used["P%d" % first["i"]] = d.day
        d.spot.add(last["i"])
        if h % 3 == 0:
            return [{"kind": "chron", "mood": "calm", "text": "SAGA %d %s" % (d.day, first["n"]), "names": [], "lid": "sg/fake", "params": {}}]
        return []
    saga.run = saga_run
    saga.render_msg = lambda lid, params, lang="en": "SAGA-RENDER " + lid
    sys.modules["clan_saga"] = saga

    loot = types.ModuleType("clan_loot")

    def fortune(st, cs, hist=None):
        cs["fort"] = {"s": len(hist) - 3}
        return len(hist) - 3
    loot.fortune = fortune

    def roll_finds(rng, st, cs, r, match, score=None, day=None, left=False):
        n = rng.randrange(0, 3)
        out = []
        for _ in range(n):
            k = rng.choice(["pine", "beans", "bandage", "chess", "teddy"])
            out.append({"k": k, "n": rng.randrange(1, 4), "c": -1})
        return out
    loot.roll_finds = roll_finds
    loot.legacy_loot = lambda finds, have=(): [[f["k"], 1 if f["k"] in CH.KEEP else f["n"]] for f in finds]

    def update_moods(st, cs):
        cs["moods"] = cs.get("moods", 0) + 1
    loot.update_moods = update_moods
    loot.items_text = lambda finds, lang="en": ", ".join("%sx%s" % (f["n"], f["k"]) for f in finds)
    loot.public = lambda finds, lang="en": [{"k": f["k"], "lang": lang} for f in finds or []]
    sys.modules["clan_loot"] = loot

    status = types.ModuleType("clan_status")
    status.HEALTHY = 0
    status.NAMES = ["healthy", "hungry", "sad", "sick", "dead", "injured", "recovered", "new", "depressed", "shock"]
    status.EXPR = {0: "neutral", 1: "hungry", 2: "sad", 3: "sick", 4: "dead", 5: "sick", 6: "neutral", 7: "neutral", 8: "depressed", 9: "shock"}
    status.code_of = lambda st, p: [0, 1, 2, 3, 5, 8, 9][(p["i"] * 5 + st.get("day", 0)) % 7]

    def observe(st, rows=None):
        cs = st["ch"]
        cs["obs"] = cs.get("obs", 0) + len(rows)
    status.observe = observe

    def spublic(st, fk, lang="en"):
        if st.get("day", 0) % 2:
            return {}
        return {"events": [{"k": len(st["people"]), "lang": lang, "f": fk}]}
    status.public = spublic
    sys.modules["clan_status"] = status

    def spare_sick(rng, st, day):
        rng.random()
        st["pid"] += 1
        q = {"i": st["pid"], "n": "Spare%d" % st["pid"], "l": "Xx", "g": "n", "r": "scavenger", "s": "s", "il": "flu", "is": 0, "id": day, "k": 0, "jd": 0, "gr": 0, "gl": 0}
        st["people"].append(q)
        return q
    CS.spare_sick = spare_sick


# ------------------------------------------------------------------ generator
def person(r, pid, role=None, state="h", jd=0, spores=False):
    import clan_story_data as D
    names = D.FIRST_2013
    nm = r.choice(names)
    g = r.choice(["m", "f", "m", "f", "n"]) if r.random() < 0.97 else "n"
    role = role or r.choice(list(D.ROLES) + ["kid", "kid", "elder", "elder", "medic", "cook", "scout"])
    p = {"i": pid, "n": nm + (" " + r.choice(D.LAST_2013) if r.random() < 0.3 else ""), "l": r.choice(D.LAST_2013), "g": g, "r": role, "s": state, "il": "", "is": 0, "id": 0, "k": 0,
         "jd": jd, "gr": r.choice([0, 0, 0, 3, 90]), "gl": 0}
    if state == "s":
        p["il"] = "spores" if (spores or r.random() < 0.3) else r.choice([k for k in D.ILLNESS if k != "spores"])
        p["id"] = jd
    return p


def make_state(r, tid):
    import clan_story as CS
    fac = r.choice([1, 2])
    st = CS.new_state(tid, fac, r.randint(1, 3))
    if r.random() < 0.6:
        st["tid"] = tid
    st["morale"] = r.randint(5, 95)
    st["day"] = r.randint(0, 3)
    n = r.randint(3, 16)
    for _ in range(n):
        st["pid"] += 1
        state = r.choices(["h", "u", "s"], [6, 2, 2])[0]
        st["people"].append(person(r, st["pid"], state=state, jd=st["day"], spores=r.random() < 0.2))
    for p in st["people"]:
        if r.random() < 0.3:
            q = r.choice(st["people"])
            if q is not p and not q["k"] and not p["k"]:
                p["k"], q["k"] = q["i"], p["i"]
    return st


def gen_match(r, day):
    if r.random() < 0.2:
        return None
    m = {"map": r.choice(MAPS), "kills": r.choice([0, 1, 2, 3, 5, 8, 12, 20])}
    m["deaths"] = None if r.random() < 0.25 else r.choice([0, 1, 2, 3, 5, 9])
    m["downs"] = None if r.random() < 0.3 else r.choice([0, 1, 2, 3, 4, 7])
    if r.random() < 0.6:
        m["execs"] = r.randint(0, 5)
        m["revives"] = r.randint(0, 3)
        m["win"] = r.choice([True, False, None])
        m["mid"] = "%08x" % r.getrandbits(32)
    if r.random() < 0.1:
        del m["kills"]
    return m


def gen(seed, n):
    r = random.Random(seed)
    import clan_chronicle as CH
    out = []
    ci = 0
    cur = None
    day = 0
    ids = list(CH.REG)
    nextpid = 1000

    def emit(o):
        out.append(o)

    def new_clan():
        nonlocal ci, cur, day
        ci += 1
        cur = "c%d" % ci
        st = make_state(r, "tid-%d-%d" % (seed, ci))
        day = st["day"]
        emit({"op": "load", "k": cur, "st": st})
        return st
    shadow_ids = []
    st = new_clan()
    shadow_ids = [p["i"] for p in st["people"]]
    emitted = 0
    while len(out) < n:
        x = r.random()
        if x < 0.45:
            # mutations then a day
            day += 1
            emit({"op": "set", "k": cur, "path": ["day"], "v": day})
            if r.random() < 0.5:
                emit({"op": "set", "k": cur, "path": ["morale"], "v": r.randint(0, 100)})
            for _ in range(r.choice([0, 1, 1, 2, 3])):
                y = r.random()
                if y < 0.45 and shadow_ids:
                    emit({"op": "pset", "k": cur, "i": r.choice(shadow_ids), "f": "s", "v": r.choice(["h", "h", "u", "s"])})
                    if r.random() < 0.5:
                        emit({"op": "pset", "k": cur, "i": r.choice(shadow_ids), "f": "il", "v": r.choice(["spores", "fever", "flu", ""])})
                elif y < 0.65 and len(shadow_ids) > 2:
                    i = r.choice(shadow_ids)
                    shadow_ids.remove(i)
                    emit({"op": "kill", "k": cur, "i": i, "d": day if r.random() < 0.7 else day - r.randint(1, 5), "c": r.choice(["hunger", "bite", "fight", "walkout"])})
                else:
                    nextpid += 1
                    p = person(r, nextpid, state=r.choice(["h", "h", "u", "s"]), jd=day if r.random() < 0.6 else max(0, day - 3), spores=r.random() < 0.3)
                    shadow_ids.append(nextpid)
                    emit({"op": "add", "k": cur, "p": p})
            if len(shadow_ids) < 4:
                for _ in range(3):
                    nextpid += 1
                    p = person(r, nextpid, state=r.choice(["h", "u", "s"]), jd=day)
                    shadow_ids.append(nextpid)
                    emit({"op": "add", "k": cur, "p": p})
            m = gen_match(r, day)
            sup = r.randint(0, 14)
            need = r.randint(1, 14)
            rr = {"supplies": sup, "needed": need, "parts": r.randint(0, 400), "gained": r.choice([0, 0, 1, 2]), "evg": r.choice([0, 0, 1])}
            if r.random() < 0.4:
                rr["sup"] = {"need": r.randint(0, 9), "got": {"food": r.randint(0, 3)}, "short": {}, "drawn": {"ammo": 1}}
            mode = r.random()
            newc = {"day": day, "sick": r.choice([0, 0, 1, 2]), "hungry": r.choice([0, 0, 1, 3]), "size": 9}
            call = {"op": "day", "k": cur, "newc": newc, "r": rr, "seed": r.choice([None, r.randint(0, 10 ** 6), r.randint(0, 10 ** 6)])}
            if m is None or mode < 0.15:
                call["match"] = None
                if m is not None:
                    rr["match"] = m
            else:
                call["match"] = m
                if r.random() < 0.2:
                    rr["match"] = {"map": "Hospital", "kills": 99}
            if r.random() < 0.1:
                call["used"] = {} if r.random() < 0.5 else {ids[r.randrange(len(ids))]: day - r.randint(0, 30) for _ in range(r.randint(1, 20))}
            if r.random() < 0.05:
                call["r"] = {} if r.random() < 0.5 else None
            emit(call)
            if day > 90 or len(shadow_ids) > 40:
                st = new_clan()
                shadow_ids = [p["i"] for p in st["people"]]
            if r.random() < 0.15:
                nextpid += 1
        elif x < 0.50:
            emit({"op": "save", "k": cur})
            emit({"op": "public", "k": cur, "lang": r.choice(LANGS)})
        elif x < 0.58:
            emit({"op": r.choice(["persona", "tagline", "assign", "inventory", "match_log", "portrait", "head", "bond"]), "k": cur, "lang": r.choice(LANGS), "n": r.randrange(0, 20),
                  "state": r.choice([None, "h", "u", "s", "i", "d"]), "morale": r.choice([5, 25, 26, 50, 69, 70, 90]), "models": r.choice([1, 2, 4, 6]), "members": r.random() < 0.6})
        elif x < 0.68:
            kind = r.choice(["qty", "loot_phrase", "join", "map", "habit_pref", "roll_loot"])
            if kind == "qty":
                emit({"op": "qty", "key": r.choice(["pine", "beans", "r38", "box9", "abx", "bandage", "nope"]), "n": r.choice([-1, 0, 1, 2, 3, 4, 5, 11, 12, 14, 21, 22, 25, 100, 112]), "lang": r.choice(["en", "pl"])})
            elif kind == "loot_phrase":
                emit({"op": "loot_phrase", "key": r.choice(["pine", "chess", "guitar", "teddy", "seeds", "rags", "nope"]), "n": r.randrange(0, 30), "lang": r.choice(["en", "pl"])})
            elif kind == "join":
                emit({"op": "join", "items": [r.choice(["a", "b b", "żółw", "x"]) for _ in range(r.randrange(0, 5))], "lang": r.choice(["en", "pl"])})
            elif kind == "map":
                emit({"op": "map", "en": r.choice(MAPS + [None, 5]), "lang": r.choice(["en", "pl"])})
            elif kind == "habit_pref":
                emit({"op": "habit_pref", "p": person(r, 1), "fk": r.choice(["W", "S"])})
            else:
                emit({"op": "roll_loot", "seed": r.randint(0, 10 ** 6), "r": {"supplies": r.randint(0, 20), "needed": r.choice([0, 1, 5, 12])}, "match": {"map": "Hospital", "kills": r.choice([0, 3, 14]),
                      "deaths": r.choice([0, None, 4, 9])}, "newc": {"sick": r.choice([0, 1]), "hungry": r.choice([0, 2])}, "have": {k: {} for k in r.sample(["chess", "guitar", "comic", "teddy", "seeds", "cards", "harmonica"], r.randint(0, 7))}})
        elif x < 0.90:
            # stored rows of the chronicle / arcs rendered with unusual params
            lid = r.choice(["ch", "ch", "arc", "arc", "sg", "bogus"])
            people = {}
            for L in "ABCD"[:r.randint(1, 4)]:
                people[L] = [r.choice(["Ann", "Bob", "Zoë", "Łucja", "Joel"]), r.choice(["m", "f", "n", "x"]), r.choice(["scavenger", "kid", "elder", "medic", "cook", "guard", "radio"])]
            if r.random() < 0.05:
                people["A"].pop()
            sym = {"f": r.choice(["W", "S", "W", "S", "Q"]), "day": r.randint(0, 90), "sup": r.randint(0, 9), "need": r.randint(0, 9), "when": r.randint(0, 5), "map": r.choice(MAPS)}
            for k in ("kills", "deaths", "downs"):
                if r.random() < 0.6:
                    sym[k] = r.randint(0, 12)
            if r.random() < 0.3:
                sym["it"] = r.choice(["pine", "beans", "abx", "r38", "nope"])
                sym["itn"] = r.randint(0, 30)
            if r.random() < 0.3:
                sym["loot"] = [[r.choice(["pine", "chess", "r38", "teddy"]), r.randint(1, 20)] for _ in range(r.randint(0, 3))]
            if r.random() < 0.1:
                sym["finds"] = [{"k": "pine", "n": 2, "c": -1}]
            if lid == "ch":
                pick = [r.choice(ids) for _ in range(r.randint(1, 4))]
                if r.random() < 0.05:
                    pick.append("bogus~00000000")
                emit({"op": "render", "lid": "ch/" + "+".join(pick), "params": {"p": people, "s": sym}, "lang": r.choice(LANGS)})
            elif lid == "arc":
                import clan_arcs as A
                kind = r.choice(list(A.ARCS))
                node = r.choice(list(A.ARCS[kind]))
                sym.update({"rel": r.choice(["sibling", "parent", "child", "mentor", "apprentice", "friend", "partner", "nope"]), "task": r.randint(0, 9), "place": r.randint(0, 9), "wish": r.randint(0, 9),
                            "spot": r.randint(0, 9), "keep": r.choice(["chess", "teddy", "nope", "cards"])})
                params = {"p": people, "s": sym, "a": "a1", "n": 2}
                if r.random() < 0.3:
                    params["tie"] = "Tie line."
                    params["tie_pl"] = "Linia."
                emit({"op": "render", "lid": "arc/%s.%s" % (kind, node) if r.random() < 0.95 else "arc/nope.x", "params": params, "lang": r.choice(LANGS)})
            elif lid == "sg":
                emit({"op": "render", "lid": "sg/abc", "params": {"p": people, "s": sym}, "lang": "en"})
            else:
                emit({"op": "render", "lid": r.choice(["", "ch", "xx/1", "ch/"]), "params": r.choice([{}, None, []]), "lang": "en"})
        else:
            emit({"op": "thread", "k": cur, "id": "a%d" % r.randint(1, 30)})
    for o in out[:n]:
        print(dumps(o))


# ------------------------------------------------------------------ run (the Python side)
def run():
    install_doubles()
    import clan_arcs as A
    import clan_chronicle as CH
    import clan_story as CS
    dbp = os.environ.get("T2R_CL_DB") or "/tmp/clan_chron_py.db"
    for p in (dbp, dbp + "-journal"):
        try:
            os.remove(p)
        except OSError:
            pass
    c = sqlite3.connect(dbp)
    c.execute("CREATE TABLE IF NOT EXISTS clan_story (token_id TEXT PRIMARY KEY, state TEXT, updated REAL)")
    sts = {}
    for i, line in enumerate(sys.stdin):
        line = line.strip()
        if not line:
            continue
        o = json.loads(line)
        op = o["op"]
        res = {"i": i}
        st = sts.get(o.get("k"))
        try:
            if op == "load":
                sts[o["k"]] = o["st"]
                res["r"] = True
            elif op == "set":
                d = st
                for kk in o["path"][:-1]:
                    d = d[kk]
                d[o["path"][-1]] = o["v"]
            elif op == "pset":
                for p in st["people"]:
                    if p["i"] == o["i"]:
                        p[o["f"]] = o["v"]
                        break
            elif op == "kill":
                for p in list(st["people"]):
                    if p["i"] == o["i"]:
                        st["people"].remove(p)
                        st["dead"].append({"i": p["i"], "n": p["n"], "g": p["g"], "r": p["r"], "d": o["d"], "c": o["c"], "il": p["il"], "k": 0})
                        break
            elif op == "add":
                st["people"].append(o["p"])
            elif op == "day":
                used = o.get("used")
                rng = random.Random(o["seed"]) if o.get("seed") is not None else None
                rows = CH.day_rows(st, o["newc"], o["r"], o["match"], used, rng)
                res["r"] = {"rows": rows, "en": [CH.render_msg(x["lid"], x["params"], "en") for x in rows], "pl": [CH.render_msg(x["lid"], x["params"], "pl") for x in rows]}
                if used is not None:
                    res["used"] = used
            elif op == "save":
                c.execute("INSERT OR REPLACE INTO clan_story(token_id,state,updated) VALUES(?,?,?)", (o["k"], dumps(st), 0.0))
            elif op == "public":
                res["r"] = CH.public(c, o["k"], o["lang"])
            elif op in ("persona", "tagline", "assign", "inventory", "match_log", "portrait", "head", "bond"):
                fk = "S" if st["fac"] == 2 else "W"
                ppl = st["people"]
                p = ppl[o["n"] % len(ppl)] if ppl else None
                lg = o["lang"]
                members = (list(ppl) + list(st.get("dead") or [])) if o["members"] else None
                if p is None and op in ("persona", "tagline", "portrait", "head", "bond"):
                    res["r"] = None
                elif op == "persona":
                    res["r"] = CH.persona(p, ppl, fk, st if o["n"] % 2 else None)
                elif op == "tagline":
                    res["r"] = CH.tagline(p, ppl, fk, "pl" if lg.startswith("pl") else "en", st if o["n"] % 2 else None)
                elif op == "assign":
                    res["r"] = CH.assign_habits(st, fk)
                elif op == "inventory":
                    res["r"] = CH.inventory(st, "pl" if lg == "pl" else "en")
                elif op == "match_log":
                    res["r"] = CH.match_log(st, "pl" if lg == "pl" else "en", o["n"] + 1)
                elif op == "portrait":
                    res["r"] = CH.portrait_id(p, fk, o["state"], o["morale"], o["models"], members)
                elif op == "head":
                    res["r"] = list(CH.head_model(p, fk, o["models"], members))
                else:
                    q = ppl[(o["n"] + 1) % len(ppl)]
                    res["r"] = [CH.bond_label(q, p, bt, "pl" if lg == "pl" else "en") for bt in ("kin", "mentor", "rival", "sweet", "")]
            elif op == "qty":
                res["r"] = CH.qty(o["key"], o["n"], o["lang"])
            elif op == "loot_phrase":
                res["r"] = CH.loot_phrase(o["key"], o["n"], o["lang"])
            elif op == "join":
                res["r"] = CH.join_list(o["items"], o["lang"])
            elif op == "map":
                res["r"] = CH.map_name(o["en"], o["lang"])
            elif op == "habit_pref":
                res["r"] = CH.habit_pref(o["p"], o["fk"])
            elif op == "roll_loot":
                res["r"] = CH.roll_loot(random.Random(o["seed"]), o["r"], o["match"], o["newc"], o["have"])
            elif op == "render":
                res["r"] = CH.render_msg(o["lid"], o["params"], o["lang"])
            elif op == "thread":
                res["r"] = A.thread(st, o["id"])
        except Exception as ex:  # noqa: BLE001
            if os.environ.get("T2R_TRACE"):
                import traceback
                sys.stderr.write("call %d %s: %s" % (i, op, traceback.format_exc()))
            res["err"] = True
            res["r"] = None
        if st is not None and op in ("day", "persona", "tagline", "assign", "inventory", "match_log", "set", "pset", "kill", "add", "portrait", "head", "bond"):
            if op in ("day", "assign") or "err" in res:
                res["st"] = st
        print(dumps({k: res[k] for k in ("i", "r", "used", "err", "st") if k in res}))
    final = {k: v for k, v in sts.items()}
    print(dumps({"i": -1, "final": final}))


if __name__ == "__main__":
    if sys.argv[1] == "gen":
        gen(int(sys.argv[2]), int(sys.argv[3]))
    else:
        run()
