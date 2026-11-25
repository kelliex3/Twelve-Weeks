"""Differential test of tools/clan_saga.py + clan_saga_pl.py + clan_loot.py against `t2r-daemon selftest clan_saga` (and `clan_loot`: the same script).

  T2R_PY_DIR=<dir with the deployed .py files> python3 clan_saga_harness.py gen SEED N > script.jsonl
  T2R_PY_DIR=<dir> T2R_CL_DB=<fresh sqlite file> python3 clan_saga_harness.py run < script.jsonl > out.py.jsonl
  T2R_CL_DB=<other file> (cwd = the python dir) t2r-daemon selftest clan_saga < script.jsonl > out.rs.jsonl

The story states are built by the real clan_story functions (generator), the day inputs are explicit script data.  The "day" ops run the WHOLE chronicle day (clan_chronicle.day_rows)
with the REAL saga, Polish saga and loot modules (clan_status / clan_story.spare_sick stay the deterministic doubles of clan_chronicle_harness on both sides): the state after every day
(st["ch"] with the saga memory, the loot memory, the depression counters) and every row, English and Polish text, are compared.  The direct ops call every public function the other
modules use (fit, render_msg, fortune, roll_finds, legacy_loot, update_moods, items_text, public) and the helpers of the three modules.
"""
import json
import os
import random
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import clan_chronicle_harness as H  # noqa: E402  (chdir + sys.path of the deployed modules)

dumps = H.dumps
LANGS = ["en", "pl", "pl-PL", "xx", "PL"]
WEAPONS = ["bow", "revolver", "shorty", "hunting", "semi", "burst", "pistol", "crossbow", "melee", "bomb", "molotov", "nope"]


def maps():
    import clan_chronicle_maps as MN
    return list(MN.PL) + ["Bookstore (Part I)", "Suburb", "Zzz Unknown Map"]


def gen_match(r, day, MAPS):
    if r.random() < 0.12:
        return None
    m = {"map": r.choice(MAPS)}
    if r.random() < 0.95:
        m["kills"] = r.choice([0, 1, 2, 3, 5, 8, 12, 20])
    m["deaths"] = None if r.random() < 0.2 else r.choice([0, 1, 2, 3, 5, 9])
    m["downs"] = None if r.random() < 0.25 else r.choice([0, 1, 2, 3, 4, 7])
    if r.random() < 0.8:
        m["execs"] = r.choice([0, 0, 1, 2, 3, 5, 12])
        m["revives"] = r.choice([0, 1, 2, 3, 4])
        m["headshots"] = r.choice([0, 1, 2, 5, 11])
        m["win"] = r.choice([True, False, None, True, False, 1, 0])
        m["mid"] = "%08x" % r.getrandbits(32)
    if r.random() < 0.5:
        m["weapon"] = r.choice(WEAPONS)
    if r.random() < 0.4:
        m["streak"] = r.choice([0, 1, 3, 7])
    return m


def gen_sup(r):
    cats = ["food", "water", "med", "ammo", "mat"]
    got = {k: r.choice([0, 0, 1, 2, 3, 5, 9, 14]) for k in cats}
    need = {k: r.choice([0, 1, 2, 3, 4]) for k in cats}
    short = {k: r.choice([0, 0, 0, 1, 2, 4]) for k in cats}
    sv = [k for k in cats[:3] if short[k]]
    cause = max(sv, key=lambda x: (short[x], x == "med", x)) if sv else ""
    return {"got": got, "need": need, "short": short, "drawn": {k: r.choice([0, 1]) for k in cats}, "net": dict(short), "stock": {k: r.choice([0, 2, 5, 11]) for k in cats},
            "days": {k: r.choice([0, 1, 2, 4, 11]) for k in cats}, "cause": cause}


def fake_find(r, LT, MAPS):
    it = LT.ITEMS[r.choice(list(LT.ITEMS))]
    kd = r.choice(list(__import__("clan_loot_data2").MICRO))
    f = {"k": it["key"], "n": r.choice([1, 1, 2, 3, 5, 12]), "c": r.choice([-1, -1, 0, 3, 7, 12, 15, 17, 21, 99]), "m": r.choice(MAPS), "t": it["tier"], "d": r.randint(0, 40), "w": r.choice(["", "Ann", "Bob"]),
         "g": r.choice(["m", "f", "n"]), "kd": kd, "mi": r.randint(0, 20), "pl": r.choice(["", "the east ward", "the cistern", "the nonexistent place"]), "v": r.randint(0, 7),
         "h": r.choice([-1, -1, -2, 0, 1, 4, 9, 13]), "ha": r.randint(0, 999)}
    if r.random() < 0.1:
        f["rare"] = 1
    return f


def gen_vars(r, SD, PD, CH_USE):
    """A variable dict like the saga builds (expand() input): mostly valid values, now and then a junk one."""
    out = {}
    en_maps = list(SD.MAPS) + ["Zzz Map"]
    kinds = r.sample(["map", "prev_map", "kmap", "place", "place2", "kplace", "camp", "leader", "enemies", "enemy", "enemy_n", "smell", "sound", "air", "k", "ex_p", "ex_t", "rv_p", "hs_p",
                      "dn_p", "dt_p", "wpn", "site", "ysite", "dsite", "cloth", "dcloth", "cause", "job", "poss", "dpos", "dposx", "poss_short", "kitem", "kitem_a", "fear", "fear_what",
                      "past", "lack_n", "lack_days", "dgo", "kd_ago", "nth", "lossw", "prev_res", "n_left", "ammo_n", "food_n", "item_pl", "item_short", "AW", "kd", "yd", "n", "day",
                      "loot_list", "place_gen", "other"], r.randint(3, 14))
    nw = ["no", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten"]
    junk = r.random() < 0.06
    for k in kinds:
        if k in ("map", "prev_map", "kmap"):
            m = r.choice(en_maps)
            out[k] = m if m in ("Downtown", "Hillside", "Hillcrest", "Houses", "Suburbs") and r.random() < 0.5 else "the " + m
        elif k in ("place", "place2", "kplace"):
            md = SD.MAPS[r.choice(list(SD.MAPS))] if r.random() < 0.9 else SD.GENERIC
            out[k] = r.choice(md["places"])
        elif k == "camp":
            out[k] = r.choice(["the east barracks", "the FOB mess", "the old gym", "the pharmacy block", "the stadium tunnel", "the long house", "the kitchen house", "the weaving house", "the shore camp", "the harbor hut"])
        elif k == "leader":
            out[k] = r.choice(["Command", "the Prophet"])
        elif k in ("enemies", "enemy"):
            out[k] = r.choice(["the Scars", "Scars", "the Wolves", "Wolves"])
        elif k == "enemy_n":
            out[k] = r.choice(["Scar", "Wolf"])
        elif k in ("smell", "sound", "air"):
            md = SD.MAPS[r.choice(list(SD.MAPS))]
            a = r.choice(md["air"])
            out[k] = ("the smell of " if a[0] == "s" else "the sound of ") + a[1]
        elif k == "k":
            out[k] = r.choice([0, 1, 2, 5, 13])
        elif k in ("ex_p", "rv_p", "hs_p", "lack_n", "lack_days", "ammo_n", "food_n"):
            n = r.choice([1, 2, 3, 5, 10, 11, 12, 22])
            one = {"ex_p": "execution", "rv_p": "teammate", "hs_p": "headshot", "lack_n": "unit", "lack_days": "day", "ammo_n": "round", "food_n": "can"}[k]
            out[k] = "%s %s" % (nw[n] if n <= 10 else n, one if n == 1 else one + "s")
        elif k in ("ex_t", "dn_p", "dt_p"):
            n = r.choice([1, 2, 3, 5, 12])
            out[k] = "once" if n == 1 else "twice" if n == 2 else "%s times" % (nw[n] if n <= 10 else n)
        elif k == "wpn":
            out[k] = r.choice(list(SD.WEAPONS.values()))[0]
        elif k in ("site", "ysite", "dsite"):
            out[k] = r.choice(SD.SITES)
        elif k in ("cloth", "dcloth"):
            out[k] = r.choice(SD.CLOTHS)
        elif k == "cause":
            out[k] = r.choice(SD.CAUSES).format(enemy_n=r.choice(["Scar", "Wolf"]))
        elif k == "job":
            out[k] = r.choice(list(SD.JOBS.values()) + ["work"])
        elif k in ("poss", "dpos", "dposx"):
            out[k] = r.choice(SD.POSSESSIONS + ["boots", "a small keepsake"])
        elif k == "poss_short":
            p = r.choice(SD.POSSESSIONS + ["boots", "a small keepsake"])
            out[k] = p.split(" ", 1)[1] if p.split(" ", 1)[0] in ("a", "an") else p
        elif k in ("kitem", "kitem_a"):
            out[k] = r.choice(["a chessboard", "chessboard", "a guitar string", "a comic book", "teddy bear", "a teddy bear", "a deck of cards"])
        elif k == "fear":
            out[k] = r.choice(SD.FEARS)[0]
        elif k == "fear_what":
            out[k] = r.choice(SD.FEARS)[1]
        elif k == "past":
            import clan_chronicle as CH
            out[k] = r.choice(CH.PASTS)[2]
        elif k == "dgo":
            out[k] = r.choice(["gone since yesterday", "gone three days", "gone two days", "gone eleven days"])
        elif k in ("kd_ago",):
            out[k] = r.choice(["yesterday", "today", "three days ago", "two days ago", "twelve days ago"])
        elif k == "nth":
            out[k] = r.choice(["second", "third", "fourth", "again", "seventh"])
        elif k == "lossw":
            out[k] = r.choice(["two", "three", "five", "ten", "eleven"])
        elif k == "prev_res":
            out[k] = r.choice(["a win", "a loss", "a plain day"])
        elif k == "n_left":
            out[k] = r.choice(["none left", "3 left", "12 left"])
        elif k in ("item_pl", "item_short"):
            out[k] = r.choice([e[2] for e in CH_USE.values()] + [".38 rounds"])
        elif k == "AW":
            out[k] = r.choice(["Ann", "Bob"])
        elif k in ("kd", "yd", "n", "day"):
            out[k] = r.randint(0, 99)
        elif k == "loot_list":
            out[k] = "two cans of beans and a deck of cards"
        elif k == "place_gen":
            out[k] = "the cistern"
        else:
            out[k] = r.choice(["x", 5, None])
    if junk and out:
        out[r.choice(list(out))] = r.choice(["Zzz nope", "the Zzz", "", "five", 7])
    return out


def person_dict(r, i):
    return {"n": r.choice(["Ann", "Bob", "Zoë", "Łucja", "Joel", "Mia"]), "g": r.choice(["m", "f", "n", "x"]), "r": r.choice(["scavenger", "kid", "elder", "medic", "cook", "guard"]), "i": i}


def gen(seed, n):
    r = random.Random(seed)
    import clan_chronicle as CH
    import clan_loot as LT
    import clan_saga_data as SD
    import clan_saga_pl_data as PD
    MAPS = maps()
    out = []
    ci = 0
    cur = None
    day = 0
    nextpid = 1000
    ids = list(CH.REG)

    def emit(o):
        out.append(o)

    def new_clan():
        nonlocal ci, cur, day
        ci += 1
        cur = "c%d" % ci
        st = H.make_state(r, "tid-%d-%d" % (seed, ci))
        day = st["day"]
        emit({"op": "load", "k": cur, "st": st})
        return st

    st = new_clan()
    shadow_ids = [p["i"] for p in st["people"]]
    while len(out) < n:
        x = r.random()
        if x < 0.55:
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
                elif y < 0.68 and len(shadow_ids) > 3:
                    i = r.choice(shadow_ids)
                    shadow_ids.remove(i)
                    emit({"op": "kill", "k": cur, "i": i, "d": day if r.random() < 0.75 else day - r.randint(1, 5), "c": r.choice(["hunger", "bite", "fight", "walkout"])})
                else:
                    nextpid += 1
                    p = H.person(r, nextpid, state=r.choice(["h", "h", "u", "s"]), jd=day if r.random() < 0.6 else max(0, day - 3), spores=r.random() < 0.3)
                    shadow_ids.append(nextpid)
                    emit({"op": "add", "k": cur, "p": p})
            if len(shadow_ids) < 5:
                for _ in range(4):
                    nextpid += 1
                    p = H.person(r, nextpid, state=r.choice(["h", "u", "s"]), jd=day)
                    shadow_ids.append(nextpid)
                    emit({"op": "add", "k": cur, "p": p})
            m = gen_match(r, day, MAPS)
            rr = {"supplies": r.randint(0, 24), "needed": r.randint(1, 14), "parts": r.randint(0, 400), "gained": r.choice([0, 0, 1, 2]), "evg": r.choice([0, 0, 1])}
            if r.random() < 0.75:
                rr["sup"] = gen_sup(r)
            if r.random() < 0.04:
                rr["left"] = True
            newc = {"day": day, "sick": r.choice([0, 0, 1, 2]), "hungry": r.choice([0, 0, 1, 3]), "size": 9}
            call = {"op": "day", "k": cur, "newc": newc, "r": rr, "seed": r.choice([None, r.randint(0, 10 ** 6), r.randint(0, 10 ** 6)]), "match": m}
            if r.random() < 0.08:
                call["used"] = {} if r.random() < 0.5 else {ids[r.randrange(len(ids))]: day - r.randint(0, 30) for _ in range(r.randint(1, 20))}
            emit(call)
            if day > 70 or len(shadow_ids) > 30:
                st = new_clan()
                shadow_ids = [p["i"] for p in st["people"]]
        elif x < 0.60:
            emit({"op": "save", "k": cur})
            emit({"op": "public", "k": cur, "lang": r.choice(LANGS)})
        elif x < 0.72:
            kind = r.choice(["fortune", "roll_finds", "roll_finds", "update_moods", "legacy_loot", "items_text", "public_l", "render_find", "finds_story", "keepsakes", "supply_value", "item_phrase", "loc_phrase", "icon", "join"])
            if kind == "fortune":
                hist = [{"win": r.choice([True, False, None]), "sup": r.randint(0, 20), "need": r.randint(0, 14)} for _ in range(r.randint(0, 9))]
                emit({"op": "fortune", "k": cur, "hist": hist, "morale": r.randint(0, 100), "day": r.randint(0, 80)})
            elif kind == "roll_finds":
                m = gen_match(r, day, MAPS) or {}
                rr = {"supplies": r.randint(0, 24), "needed": r.randint(1, 14), "parts": r.randint(0, 400)}
                if r.random() < 0.7:
                    rr["sup"] = gen_sup(r)
                if r.random() < 0.05:
                    rr["left"] = True
                emit({"op": "roll_finds", "k": cur, "seed": r.randint(0, 10 ** 6), "r": rr, "match": m, "score": r.choice([None, -1.0, -0.5, 0.0, 0.3, 0.6, 1.0, round(r.uniform(-1, 1), 3)]),
                      "day": r.choice([None, day, day + 1, r.randint(0, 90)]), "left": r.random() < 0.05})
            elif kind == "update_moods":
                emit({"op": "update_moods", "k": cur})
            elif kind == "legacy_loot":
                emit({"op": "legacy_loot", "finds": [fake_find(r, LT, MAPS) for _ in range(r.randint(0, 6))] + [{"k": r.choice(["chess", "deck", "mixedseed", "pine", "harmonica", "guitar", "comic", "teddy", "beans"]), "n": 2, "c": -1}],
                      "have": {k: {} for k in r.sample(["chess", "guitar", "comic", "teddy", "seeds", "cards", "harmonica"], r.randint(0, 5))}})
            elif kind == "items_text":
                emit({"op": "items_text", "finds": [fake_find(r, LT, MAPS) for _ in range(r.randint(0, 5))], "lang": r.choice(["en", "pl", "xx"])})
            elif kind == "public_l":
                emit({"op": "public_l", "finds": [fake_find(r, LT, MAPS) for _ in range(r.randint(0, 4))] + ([{"k": "nope", "n": 1}] if r.random() < 0.1 else []), "lang": r.choice(["en", "pl"])})
            elif kind == "render_find":
                emit({"op": "render_find", "f": fake_find(r, LT, MAPS), "lang": r.choice(["en", "pl"]), "hook": r.random() < 0.7})
            elif kind == "finds_story":
                emit({"op": "finds_story", "finds": [fake_find(r, LT, MAPS) for _ in range(r.randint(0, 4))], "lang": r.choice(["en", "pl"]), "limit": r.choice([1, 2, 3])})
            elif kind == "keepsakes":
                emit({"op": "keepsakes", "finds": [fake_find(r, LT, MAPS) for _ in range(r.randint(0, 6))]})
            elif kind == "supply_value":
                emit({"op": "supply_value", "finds": [fake_find(r, LT, MAPS) for _ in range(r.randint(0, 6))]})
            elif kind == "item_phrase":
                emit({"op": "item_phrase", "key": r.choice(list(LT.ITEMS) + ["nope"]), "n": r.choice([-1, 0, 1, 2, 3, 4, 5, 11, 12, 22, 25, 100]), "c": r.choice([-1, 0, 5, 12, 13, 21, 99]), "lang": r.choice(["en", "pl"])})
            elif kind == "loc_phrase":
                emit({"op": "loc_phrase", "f": fake_find(r, LT, MAPS), "lang": r.choice(["en", "pl"]), "wp": r.random() < 0.5})
            elif kind == "icon":
                emit({"op": "icon", "key": r.choice(list(LT.ITEMS) + ["nope"])})
            else:
                emit({"op": "ljoin", "items": [r.choice(["a", "b b", "żółw", "x"]) for _ in range(r.randrange(0, 5))], "lang": r.choice(["en", "pl"])})
        elif x < 0.80:
            kind = r.choice(["fit", "fit", "render", "they_fix", "map_art", "facts", "small"])
            if kind == "fit":
                emit({"op": "fit", "k": cur, "kind": r.choice(["missing", "sick", "wish", "betray", "blame", "keep", "new", "nope"]), "n": r.randrange(0, 20)})
            elif kind == "render":
                people = {L: [r.choice(["Ann", "Bob"]), "m", "kid"] for L in "AB"}
                emit({"op": "sg_render", "lid": r.choice(["sg/abc", "sg/x", "ch/abc", "", "sg/"]), "params": r.choice([{"t": "Text.", "tp": "Tekst."}, {"t": "Only."}, {"tp": "Tylko."}, {}, None, [], {"p": people}]),
                      "lang": r.choice(["en", "pl", "pl-PL", "xx", "PL", "Pl"])})
            elif kind == "they_fix":
                words = ["they mentions", "They is", "they was a", "they has", "they does it", "they goes", "they ties", "they washes", "they misses", "they focuses", "they always", "they notices that",
                         "they carries", "they is_", "they sings", "they Is", "they lies"]
                emit({"op": "they_fix", "text": " ".join(r.choice(words) + r.choice(["", ".", ","]) for _ in range(r.randint(1, 6)))})
            elif kind == "map_art":
                emit({"op": "map_art", "name": r.choice(MAPS + ["", None, "Downtown", "Hillside", "Houses"])})
            elif kind == "facts":
                emit({"op": "facts", "m": r.choice([None, {}, gen_match(r, day, MAPS), {"map": "Hospital", "kills": "5", "win": r.choice([True, False, 1, 0, 2, None, "x"]), "weapon": r.choice(WEAPONS)}])})
            elif kind == "small":
                emit({"op": "small", "n": r.choice([-3, -1, 0, 1, 2, 3, 5, 10, 11, 12, 100]), "d": r.randint(0, 20)})
            else:
                emit({"op": "pools"})
        else:
            kind = r.choice(["expand", "expand", "expand", "plfn"])
            if kind == "expand":
                fk = r.choice(["W", "S"])
                people = {"A": person_dict(r, 1)}
                if r.random() < 0.7:
                    people["B"] = person_dict(r, 2)
                import clan_chronicle as CH2
                v = gen_vars(r, SD, PD, CH2.USE)
                if r.random() < 0.5:
                    v["cause"] = r.choice(SD.CAUSES).format(enemy_n="Scar" if fk == "W" else "Wolf")
                over = r.choice([None, None, {"loot_list": "lista", "finds_text": "tekst"}])
                emit({"op": "expand", "v": v, "people": people, "fk": fk, "over": over})
            else:
                emit({"op": "plfn", "fn": r.choice(["times", "count", "ex_phrase", "hs_phrase", "unit_phrase", "ago", "gone", "losses", "num", "past_pl", "from", "prep"]),
                      "n": r.choice([-1, 0, 1, 2, 3, 4, 5, 10, 11, 12, 14, 21, 22, 25]), "case": r.choice(["nom", "acc", "gen"]), "en": r.choice(["yesterday", "today", "three days ago", "gone since yesterday", "gone five days", "two", "ten", "11", "x"]),
                      "w": r.choice(["szkoła", "zbocze", "wschodnie koszary", "sala", "stacja", "Wschód", "źródło", "wielka"]), "g": r.choice(["m", "f"])})
    for o in out[:n]:
        print(dumps(o))
    print(dumps({"op": "pools"}))


# ------------------------------------------------------------------ run (the Python side)
def run():
    H.install_doubles()
    for name in ("clan_saga", "clan_loot"):                     # the doubles of the chronicle test are replaced by the REAL modules here
        sys.modules.pop(name, None)
    import clan_chronicle as CH
    import clan_loot as LT
    import clan_saga as SG
    import clan_saga_pl as PLS
    dbp = os.environ.get("T2R_CL_DB") or "/tmp/clan_saga_py.db"
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
        showst = False
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
                import random
                used = o.get("used")
                rng = random.Random(o["seed"]) if o.get("seed") is not None else None
                showst = True
                rows = CH.day_rows(st, o["newc"], o["r"], o["match"], used, rng)
                res["r"] = {"rows": rows, "en": [CH.render_msg(x["lid"], x["params"], "en") for x in rows], "pl": [CH.render_msg(x["lid"], x["params"], "pl") for x in rows]}
                if used is not None:
                    res["used"] = used
            elif op == "save":
                c.execute("INSERT OR REPLACE INTO clan_story(token_id,state,updated) VALUES(?,?,?)", (o["k"], dumps(st), 0.0))
                c.commit()
            elif op == "public":
                res["r"] = CH.public(c, o["k"], o["lang"])
            elif op == "fortune":
                showst = True
                cs = CH.ch_state(st)
                st2 = dict(st, morale=o["morale"], day=o["day"])
                res["r"] = LT.fortune(st2, cs, hist=o["hist"])
            elif op == "roll_finds":
                import random
                showst = True
                cs = CH.ch_state(st)
                res["r"] = LT.roll_finds(random.Random(o["seed"]), st, cs, o["r"], o["match"], score=o["score"], day=o["day"], left=o["left"])
            elif op == "update_moods":
                showst = True
                cs = CH.ch_state(st)
                res["r"] = LT.update_moods(st, cs)
            elif op == "legacy_loot":
                res["r"] = LT.legacy_loot(o["finds"], have=o["have"])
            elif op == "items_text":
                res["r"] = LT.items_text(o["finds"], o["lang"])
            elif op == "public_l":
                res["r"] = LT.public(o["finds"], o["lang"])
            elif op == "render_find":
                res["r"] = LT.render_find(o["f"], o["lang"], o["hook"])
            elif op == "finds_story":
                res["r"] = LT.finds_story(o["finds"], o["lang"], o["limit"])
            elif op == "keepsakes":
                res["r"] = LT.keepsakes(o["finds"])
            elif op == "supply_value":
                res["r"] = LT.supply_value(o["finds"])
            elif op == "item_phrase":
                res["r"] = [LT.item_phrase(o["key"], o["n"], o["c"], o["lang"]), LT.noun_phrase(o["key"], o["lang"])]
            elif op == "loc_phrase":
                res["r"] = LT.loc_phrase(o["f"], o["lang"], o["wp"])
            elif op == "icon":
                res["r"] = [LT.icon_for(o["key"]), LT.icon_wanted(o["key"]), LT.missing_icons() if o["key"] == "pine" else None]
            elif op == "ljoin":
                res["r"] = LT.join_list(o["items"], o["lang"])
            elif op == "fit":
                cs = CH.ch_state(st)
                ppl = st["people"]
                p = ppl[o["n"] % len(ppl)] if ppl else {"i": 0, "r": "cook"}
                res["r"] = SG.fit(cs, o["kind"], p)
            elif op == "sg_render":
                res["r"] = SG.render_msg(o["lid"], o["params"], o["lang"])
            elif op == "they_fix":
                res["r"] = SG.they_fix(o["text"])
            elif op == "map_art":
                res["r"] = [SG.map_art(o["name"]), SG.map_data(o["name"]) is SG.SD.GENERIC]
            elif op == "facts":
                res["r"] = SG.facts(o["m"])
            elif op == "small":
                n = o["n"]
                res["r"] = [SG._nw(n), SG.times_p(n), SG.count_p(n, "x", "xs"), SG.ago(o["d"], o["d"] + n)]
            elif op == "pools":
                res["r"] = {k: [[s, t, sorted(pk), sorted(vk)] for s, t, pk, vk in v] for k, v in sorted(SG.POOLS.items())}
            elif op == "expand":
                try:
                    out, _src = PLS.expand(o["v"], o["people"], o["fk"], o["over"])
                    res["r"] = sorted(out.items())
                except Exception:  # noqa: BLE001
                    res["r"] = "EXC"
            elif op == "plfn":
                fn, n, case, en, w, g = o["fn"], o["n"], o["case"], o["en"], o["w"], o["g"]
                if fn == "times":
                    res["r"] = PLS.times(n)
                elif fn == "count":
                    res["r"] = PLS.count(n, "a", "b", "c", fem=n % 2 == 0)
                elif fn == "ex_phrase":
                    res["r"] = PLS.ex_phrase(n, case)
                elif fn == "hs_phrase":
                    res["r"] = PLS.hs_phrase(n, case)
                elif fn == "unit_phrase":
                    res["r"] = PLS.unit_phrase(n, case)
                elif fn == "ago":
                    res["r"] = PLS.ago(en)
                elif fn == "gone":
                    res["r"] = PLS.gone(en)
                elif fn == "losses":
                    res["r"] = PLS.losses(en)
                elif fn == "num":
                    res["r"] = PLS._num(en)
                elif fn == "past_pl":
                    res["r"] = PLS.past_pl(en if o["n"] % 2 else "grew up in the QZ", g)
                elif fn == "from":
                    res["r"] = PLS._from(w)
                else:
                    res["r"] = [PLS._prep("w", w), PLS._prep("na", w)]
            elif op == "render":
                res["r"] = CH.render_msg(o["lid"], o["params"], o["lang"])
        except Exception as ex:  # noqa: BLE001
            if os.environ.get("T2R_TRACE"):
                import traceback
                sys.stderr.write("call %d %s: %s" % (i, op, traceback.format_exc()))
            res["err"] = True
            res["r"] = None
        if st is not None and (showst or "err" in res) and op not in ("save", "public"):
            res["st"] = st
        print(dumps({k: res[k] for k in ("i", "r", "used", "err", "st") if k in res}))
    print(dumps({"i": -1, "final": {k: v for k, v in sts.items()}}))


if __name__ == "__main__":
    if sys.argv[1] == "gen":
        gen(int(sys.argv[2]), int(sys.argv[3]))
    else:
        run()
