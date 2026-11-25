"""Differential test of the clan base modules (tools/clan.py, clan_supply.py, clan_kids.py, clan_reset.py, clan_continuity.py, clan_portraits.py)
against `t2r-daemon selftest clan_base`.

  T2R_PY_DIR=<dir with the deployed .py files> python3 clan_base_harness.py gen SEED N > script.jsonl
  T2R_TEST_SEED=SEED T2R_PY_DIR=<dir> T2R_CL_DB=<fresh sqlite file> T2R_CL_DIR=<backup dir> python3 clan_base_harness.py run < script.jsonl > out.py.jsonl
  T2R_TEST_SEED=SEED T2R_CL_DB=<other file> T2R_CL_DIR=<other dir> (cwd = the python dir) t2r-daemon selftest clan_base < script.jsonl > out.rs.jsonl

One JSON line per call ({"i", "r", "p" = printed lines}, dict key order kept), then a full dump of every table and of the backup directory.
Time is frozen per script line ("t"); clan.rng = random.Random(seed + 1) and the global random is seeded with seed (src/rnd.rs does the same).
The python dir is the working directory of both sides (the switch files clan_events.txt / clan_portrait_pool.txt live there).
"""
import contextlib
import io
import json
import os
import random
import re
import sqlite3
import sys
import time

PYDIR = os.environ.get("T2R_PY_DIR") or os.path.expanduser("~/rsverify/daemon/py")
SEED = int(os.environ.get("T2R_TEST_SEED", "0") or 0)

DOUBLE = ["Karen", "Reggie", "Mary-Ann", "ellie", "Dina", "x y", "Joel", "Tommy", "Åsa"]


def _expand(r, alt):
    alt = re.sub(r"\(\?:(.*?)\)", lambda m: r.choice(m.group(1).split("|")), alt)
    alt = re.sub(r"\((.*?)\)", lambda m: r.choice(m.group(1).split("|")), alt)
    alt = re.sub(r"\[(\w+)\]", lambda m: r.choice(m.group(1)), alt)
    alt = alt.replace("\\b", "")
    alt = re.sub(r"(.)\?", lambda m: m.group(1) if r.random() < .5 else "", alt)
    alt = alt.replace("\\w*", r.choice(["", "", "a", "ing", "ow", "ów", "x"]))
    return alt


def _alts(p):
    out, depth, cur = [], 0, ""
    for ch in p:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "|" and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    out.append(cur)
    return out


FILL = ["the", "a", "camp", "kitchen", "bread", "garden", "Karen", "Mary-Ann", "ellie's", "x_y", "cook", "ŻÓŁW", "wartość", "shotgun's", "gunman", "and", "we", "ate",
        "soup", "runners", "run", "Reggie", "KAREN", "karen", "1", "42", "ran", "rań", "rany", "cele", "cel", "celu", "celuje", "wojna", "wojny", "Dina", "Åsa"]
PH = ["{A}", "{B}", "{C}", "{D}", "{A_name}", "{B_x}", "{enemy}", "{enemy_gen}", "{wpn}", "{wpn_a}", "{ammo_n}", "{foo}", "{a b}", "{A_}", "{A__x}", "{{A}}", "{", "}", "{A", "{ex_p}",
      "{site}", "{kills_n}", "{x_y_z}", "{_a}", "{a_}", "{AB}", "{A_b_c}"]
PUNCT = [".", "!", "?", ",", "'", ";", ":", "...", "\n", "  ", "\t", " "]


def kid_sentence(r, en_words, pl_words, lang):
    words = en_words if lang == "en" else pl_words
    n = r.randint(1, 9)
    out = []
    for _ in range(n):
        x = r.random()
        if x < .25:
            w = r.choice(words)
        elif x < .6:
            w = r.choice(FILL)
        elif x < .75:
            w = r.choice(PH)
        elif x < .85:
            w = r.choice(words).upper()
        else:
            w = r.choice(words).capitalize()
        if r.random() < .15:
            w += r.choice(PUNCT)
        out.append(w)
        if r.random() < .12:
            out.append(r.choice(PUNCT))
    return " ".join(out)


def gen(r, n):
    sys.path.insert(0, PYDIR)
    import clan_kids
    en_words = [_expand(r, a) for a in _alts(clan_kids._EN)]
    pl_words = [_expand(r, a) for a in _alts(clan_kids._PL)]
    tids = ["s:765611980000000%02d" % i for i in range(4)] + ["%024x" % r.getrandbits(96) for _ in range(3)]
    tokens = {t: "%064x" % r.getrandbits(256) for t in tids}
    t = [1700000000.0]
    out = []

    def emit(op, **kw):
        t[0] += r.choice([0, 0, 1, 30, 600, 3600])
        out.append(dict(t=round(t[0], 2), op=op, **kw))

    nmid = [0]
    mids = []

    def newmid():
        nmid[0] += 1
        m = "mid%d-%s" % (nmid[0], r.choice(["", "é", "x"]))
        mids.append(m)
        return m

    # schema of the tables other modules own (the snapshot / reset / recap functions read them) + maybe an old-style clans table
    emit("sql", q="CREATE TABLE players(token TEXT PRIMARY KEY, parts_total INT DEFAULT 0, parts_n INT DEFAULT 0)")
    emit("sql", q="CREATE TABLE clan_story(token_id TEXT PRIMARY KEY, data TEXT, updated REAL)")
    emit("sql", q="CREATE TABLE clan_feed(id INTEGER PRIMARY KEY AUTOINCREMENT, token_id TEXT, day INT, kind TEXT, text TEXT, ts REAL)")
    emit("sql", q="CREATE TABLE clan_alife(token_id TEXT PRIMARY KEY, data TEXT)")
    emit("sql", q="CREATE TABLE names(token_id TEXT PRIMARY KEY, name TEXT, tag TEXT)")
    if r.random() < .35:
        emit("sql", q="CREATE TABLE clans(token_id TEXT PRIMARY KEY, faction INT DEFAULT 0, status INT DEFAULT 0, day INT DEFAULT 0, healthy INT DEFAULT 0, hungry INT DEFAULT 0,"
                      " sick INT DEFAULT 0, weeks_total INT DEFAULT 0, runs_won INT DEFAULT 0, created REAL, updated REAL)")
        for tid in tids[:4]:
            emit("sql", q="INSERT INTO clans(token_id,faction,status,day,healthy,hungry,sick,weeks_total,runs_won,created,updated) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                 a=[tid, r.choice([1, 2]), r.choice([1, 1, 2, 3]), r.randint(0, 80), r.randint(0, 20), r.randint(0, 5), r.randint(0, 5), r.randint(0, 11), r.randint(0, 3), 1.5, 2.5])
    emit("ensure")
    for tid in tids:
        emit("sql", q="INSERT OR REPLACE INTO players(token,parts_total,parts_n) VALUES(?,?,?)", a=[tokens[tid], r.randint(0, 99999), r.randint(0, 99)])

    def counts():
        c = {}
        for k in r.sample([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 31, 32, 33, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52], r.randint(0, 8)):
            c[str(k)] = r.choice([0, 1, 2, 3, 5, 8, 13, 21, 40])
        if r.random() < .5:
            c["1"] = r.choice([0, 2, 3, 6, 9, 15, 25])
        if r.random() < .25:
            c[str(r.randint(1, 5))] = r.choice([3, 6, 9, 15])
        return c

    def person_set():
        d = {}
        for L in "ABCD":
            if r.random() < .8:
                d[L] = {"r": r.choice(["kid", "kid", "cook", "guard", "medic", "elder"]), "n": r.choice(DOUBLE)}
                if r.random() < .1:
                    d[L]["n"] = ""
        return d

    def feed_rows():
        rows = []
        i = 100
        day = r.randint(0, 20)
        names = ["Karen", "Reggie", "Dina", "Joel"]
        for _ in range(r.randint(0, 14)):
            i += r.choice([1, 1, 1, 2, 5])
            day += r.choice([0, 1, 1, 2, 3])
            k = r.choice(["sick", "worse", "medicine", "recovery", "death_illness", "death_attack", "loss_x", "grief", "hope_after", "other", "chron", "sick", "grief"])
            row = {"id": i, "kind": k, "day": day, "text": r.choice(["Text ", "Another line. ", "  Trailing   ", "day %d was bad. " % day, "dnia %d" % day]) + str(i)}
            if r.random() < .8:
                row["who"] = {"A": r.choice(names), "D": r.choice(names)}
                if r.random() < .15:
                    row["who"].pop(r.choice("AD"))
            if k.startswith("death_") or k.startswith("loss"):
                row["names"] = r.sample(names, r.randint(0, 2))
            if r.random() < .25:
                row["style"] = r.choice(["saga_win", "saga_loss", "saga_down", "saga_prep", "saga_haul", "arc", "saga_haul"])
                row["map"] = r.choice(["Bookstore", "Downtown", "Forest", "Gas Station", "Zzz", ""])
                row["week"] = r.choice([1, 2, 3, None])
                if row["week"] is None:
                    del row["week"]
            if r.random() < .15:
                row["lang"] = r.choice(["pl", "en"])
            if r.random() < .1:
                row["thread"] = "old"
            if r.random() < .08 and rows:
                row["id"] = rows[-1]["id"]
            rows.append(row)
        r.shuffle(rows)
        return rows

    for _ in range(n):
        x = r.random()
        tid = r.choice(tids)
        if x < .30:
            c = counts() if r.random() < .9 else None
            mid = newmid() if r.random() < .93 else (r.choice(mids) if mids else "none")
            if r.random() < .5:
                emit("snap", w="take", tid=tid, mid=mid, token=tokens[tid])
            parts = r.choice([0, 40, 100, 200, 400, 800, 1500, 2500, 4000, 9000, 400, 800, 1500])
            if r.random() < .15:
                parts += r.randint(0, 3000)
            emit("apply", tid=tid, mid=mid, parts=parts, left=r.random() < .12, counts=c, mult=r.choice([1.0, 1.0, 1.0, 1.0, 1.2, 1.5, 1.6, 1.7, 2.0, 3.0, 0.5, 1]),
                 forgive=r.random() < .1, bottles=r.choice([0, 0, 0, 1, 3, 8, 20, 200]))
            if r.random() < .5:
                emit("snap", w="update", tid=tid, mid=mid, kw={"rejoined": r.random() < .5, "stint": {"parts": parts, "counts": c or {}, "mult": 1.0}})
            if r.random() < .15:
                emit("snap", w="get", tid=tid, mid=mid)
            if r.random() < .15:
                emit("snap", w="restore", tid=tid, mid=mid, token=tokens[tid])
        elif x < .37:
            emit("meta_text", tid=tid, pick=r.randint(0, 7), active=r.randint(0, 7))
        elif x < .42:
            emit("mchoose", tid=tid, mid=r.choice([1, 2, 3, 4, 5, 6, 7, 8, 40, 52, 99, 0]), pool_idx=r.choice([None, None, 0, 1, 2, 3]))
        elif x < .46:
            emit("choose", tid=tid, f=r.choice([0, 1, 2, 2, 1, 3, -1]))
        elif x < .50:
            emit(r.choice(["load", "state_text", "rank", "bnext", "bcounts", "auto"]), tid=tid)
        elif x < .53:
            emit(r.choice(["bgrant", "bconsume"]), tid=tid, bid=r.randint(0, 8), n=r.choice([1, 1, 5, 99, 0, -1]))
        elif x < .56:
            emit("save", tid=tid, patch=r.choice([{"meta": json.dumps({"sup": {"stock": {"food": 5}}, "rep": {"1": 5, "2": 2}, "max": 9}), "healthy": 12},
                                                  {"meta": "{bad", "day": 41}, {"meta": json.dumps({"m": {"id": 3, "left": 1, "prog": 1, "ev": -1, "sz": 5}}), "status": 1, "day": 2},
                                                  {"meta": json.dumps({"sup": 5}), "hungry": 3}, {"meta": json.dumps({"sup": {"stock": [1]}})}, {"meta": json.dumps({"sh": 2}), "sick": 3},
                                                  {"last_result": "{bad"}, {"status": 1, "day": r.randint(0, 82), "healthy": r.randint(0, 40)},
                                                  {"status": 1, "day": 82, "healthy": r.randint(1, 30), "meta": ""}, {"status": 1, "day": r.choice([80, 81, 82, 29, 30, 31, 14, 15, 16]), "healthy": r.randint(3, 40), "hungry": r.randint(0, 4), "sick": r.randint(0, 5), "meta": ""}]))
        elif x < .59:
            emit("resolve", tid=tid, parts=r.choice([0, 100, 400, 1500, 4000]), left=r.random() < .2, downs=r.choice([0, 0, 3, 10]), counts=counts(),
                 mult=r.choice([1.0, 1.0, 2.0, 1.5]), forgive=r.random() < .1, bottles=r.choice([0, 0, 4]))
        elif x < .64:
            nm = r.choice(["needed", "supplies_of", "week_day", "event_today", "event_window", "attack_story", "event_name", "token_id", "mission_goals", "attack_tier",
                           "tier_reward", "threshold_booster", "rank_days", "f32", "rnd", "roll", "new_events", "match_tag", "needed", "event_today", "supplies_of"])
            emit("pure", f=nm, a=[r.choice([-3, 0, 1, 4, 5, 6, 9, 14, 15, 29, 30, 34, 44, 45, 64, 65, 70, 79, 80, 82, 83, 84, 100]), r.choice([0, 1, 2, 3, 9, 20]),
                                  r.choice([0, 1, 2, 3]), r.choice([0, 400, 1234])], s=r.choice(["", "ab", "A" * 64, "%064x" % r.getrandbits(256), "ÄB" * 32, "mid", "x"]),
                 x=r.choice([0.0, 0.4, 0.5, 1.5, 2.5, -1.0, 3.75, 12.3, 99.99, 1e-7]))
        elif x < .67:
            w = r.choice(["split", "need_split", "use_of", "intake", "book", "book", "book"])
            emit("sup", w=w, total=r.randint(-3, 60), weights=[r.randint(0, 40) for _ in range(r.randint(1, 4))], need=r.randint(-2, 60), n=r.choice([None, 0, 2, 3, 5, 12, 40]),
                 counts=counts(), parts=r.choice([0, 100, 1234]), left=r.random() < .2, forgive=r.random() < .2, supplies=r.randint(0, 60),
                 stock={k: r.choice([0, 1, 5, 40, 500, 5000]) for k in r.sample(["food", "water", "med", "ammo", "mat", "zzz"], r.randint(0, 5))} if r.random() < .8 else r.choice([None, [], ""]))
        elif x < .76:
            lang = r.choice(["en", "pl", None])
            t1 = kid_sentence(r, en_words, pl_words, "en" if lang != "pl" else "pl")
            t2 = kid_sentence(r, en_words, pl_words, "pl")
            ppl = person_set()
            if ppl and r.random() < .5:
                t1 += " " + r.choice(list(ppl.values()))["n"] + r.choice(["", ".", " was shot.", "!"]) + r.choice(["", " " + r.choice(en_words)])
                t2 += " " + r.choice(list(ppl.values()))["n"] + r.choice(["", ".", "!"]) + r.choice(["", " " + r.choice(pl_words)])
            w = r.choice(["combat", "kid_safe", "kid_text_ok", "letters", "kid_letters", "is_kid", "any_kid", "kid_names", "sets", "combat"])
            if w == "kid_text_ok" and ppl and r.random() < .7:
                nm1, nm2 = r.choice(list(ppl.values()))["n"], r.choice(list(ppl.values()))["n"]
                t1 = r.choice(FILL) + ". " + r.choice(en_words) + " " + nm1 + r.choice([".", " ", "'s", "-x", ""]) + " " + r.choice(FILL)
                t2 = r.choice(FILL) + "! " + r.choice(pl_words) + " " + nm2 + r.choice([".", " ", "x", ""]) + r.choice(FILL)
            if w in ("kid_safe", "kid_letters", "letters") and r.random() < .7:
                t1 = "{%s%s} %s %s" % (r.choice("ABCD"), r.choice(["", "_x", "_ab_c"]), r.choice(en_words), r.choice(PH))
                t2 = "%s {%s} %s" % (r.choice(pl_words), r.choice("ABCD"), r.choice(FILL))
            emit("kid", w=w, x1=t1, x2=t2, lang=lang, people=ppl,
                 st={"people": [{"n": "a", "r": "kid"}, {"n": "b", "r": "guard"}, {"n": "c", "r": "kid"}], "dead": [{"n": "d", "r": "kid"}, {"n": "e", "r": "cook"}, {"n": "a", "r": "kid"}]},
                 name=r.choice(["pool", "COMBAT_KINDS", "x"]), s=r.choice(["death_attack", "thr_enemy", "match_kills", "win", "zzz", "event_hold"]))
        elif x < .83:
            w = r.choice(["tail", "match_tail", "haul_tail", "annotate", "annotate", "render", "recap", "proper", "restore_case", "annotate"])
            rows = feed_rows()
            if w == "recap":
                for _ in range(r.randint(0, 8)):
                    emit("sql", q="INSERT INTO clan_feed(token_id,day,kind,text,ts) VALUES(?,?,?,?,?)", a=[tid, r.randint(0, 40), r.choice(["sick", "chron", "alife", "death_x", "grief"]), "x", 1.0])
            emit("cont", w=w, kind=r.choice(["recovery", "death_illness", "worse", "grief"]), role=r.choice(["ill", "mourn"]), name=r.choice(DOUBLE), a=r.randint(-1, 20), b=r.randint(-1, 25),
                 lang=r.choice(["en", "pl"]), prev={"style": r.choice(["saga_win", "saga_loss", "saga_down", "x", None]), "day": r.choice([0, 3, 20, None])}, m={"map": r.choice(["Forest", "Zzz", "Gas Station"])},
                 rows=rows, dead=[{"n": nm, "d": r.randint(0, 25)} for nm in r.sample(["Karen", "Reggie", "Dina", "Joel"], r.randint(0, 3))],
                 sick_since={nm: r.randint(0, 20) for nm in r.sample(["Karen", "Reggie", "Dina", "Joel"], r.randint(0, 3))},
                 params={"alive": r.choice([0, 1, 7, None]), "d": r.randint(0, 80), "dead": [[nm, "n", r.randint(1, 60)] for nm in r.sample(["Karen", "Reggie", "Dina"], r.randint(0, 3))],
                         "sick": [[nm, "f", r.randint(1, 60)] for nm in r.sample(["Karen", "Reggie", "Dina"], r.randint(0, 3))]},
                 lid=r.choice(["cn/recap", "cn/recap", "other"]), tid=tid, mood=r.choice(["worried", "calm"]),
                 st={"people": [{"n": nm, "s": r.choice(["s", "h", "u"]), "id": r.randint(0, 40), "g": "f"} for nm in r.sample(["Karen", "Reggie", "Dina", "Joel", "Åsa"], r.randint(0, 5))],
                     "dead": [{"n": nm, "d": r.randint(0, 40), "g": r.choice(["f", "m"])} for nm in r.sample(["Karen", "Reggie", "Dina", "Joel", "Åsa"], r.randint(0, 5))], **({"ch": {"cn": 5}} if r.random() < .15 else {})},
                 day=r.randint(0, 50), text=r.choice(["they went to the bookstore (part i) in downtown, karen and mary-ann", "KAREN? karen's, Reggie!", "ksiegarnia: sklepy na capitol hill i las", "nothing here", "downtown downtown"]),
                 extra=r.sample(["Karen", "Mary-Ann", "Åsa", "", "Dina"], r.randint(0, 4)))
        elif x < .88:
            w = r.choice(["enabled", "enabled", "candidates", "pick", "pick", "pick", "faction_of", "look_of", "expr_for", "expr_for"])
            emit("port", w=w, role=r.choice(["kid", "elder", "medic", "cook", "guard", "scout", "scavenger", "mechanic", "gardener", "radio", "zzz", ""]), sex=r.choice(["m", "f"]),
                 fk=r.choice(["W", "S", "N", "X"]), name=r.choice(DOUBLE + ["", "zed", "Ünïcode", "Anna"]), model=r.choice([100, 101, 103, 105, 150, 200, 9999, 130, 140]),
                 want=r.choice(["neutral", "happy", "hurt", "scared", "shock", "perk", "injured", "grieving", "sad", "zzz"]), used=[r.choice([100, 101, 102, 103, 104, 105, 106, 107, 150]) for _ in range(r.randint(0, 12))])
        elif x < .90:
            emit("env", k=r.choice(["T2R_CLAN_NEW_EVENTS", "T2R_PORTRAIT_POOL"]), v=r.choice([None, "0", "1", "off", "no", "", "  ", "yes", "TRUE", " On "]))
        elif x < .92:
            emit("evfile", c=r.choice([None, "0", "off\nx", "1 yes", "  no", "garbage", ""]), m=r.randint(1, 1000000))
        elif x < .93:
            emit("ptfile", c=r.choice([None, "0", "off\n", " no ", "1", ""]))
        elif x < .955:
            w = r.choice(["reset", "reset", "list", "restore", "keys", "restore", "restore"])
            emit("rst", w=w, name=r.choice(["Karen", "a/b c", "Åsa", "", "x" * 60, "player one"]), sid=r.choice([0, 76561198000000001, 76561198000000002, 76561198000000003, 76561198000000000, 5]),
                 tids=[r.choice(tids[4:] + ["BAD", tids[0].upper()]) for _ in range(r.randint(0, 3))], names=r.random() < .3, file=r.choice(["", "../x.json", "bad name.json", "x.json", ".hid.json", "LAST", "LAST", "LAST"]))
        elif x < .975:
            emit("sql", q=r.choice(["INSERT OR REPLACE INTO clan_story(token_id,data,updated) VALUES(?,?,?)", "INSERT OR REPLACE INTO clan_alife(token_id,data) VALUES(?,?)",
                                    "INSERT OR REPLACE INTO names(token_id,name,tag) VALUES(?,?,?)"]) if False else "INSERT OR REPLACE INTO clan_story(token_id,data,updated) VALUES(?,?,?)",
                 a=[tid, "{\"x\":%d}" % r.randint(0, 9), 5.5])
            emit("sql", q="INSERT OR REPLACE INTO names(token_id,name,tag) VALUES(?,?,?)", a=[tid, r.choice(DOUBLE), "TAG"])
            emit("sql", q="INSERT INTO clan_feed(token_id,day,kind,text,ts) VALUES(?,?,?,?,?)", a=[tid, r.randint(0, 40), r.choice(["sick", "chron", "x"]), "t", 2.0])
            emit("sql", q="INSERT OR REPLACE INTO clan_alife(token_id,data) VALUES(?,?)", a=[tid, "{}"])
        else:
            emit("sql", q="UPDATE players SET parts_total=parts_total+?, parts_n=parts_n+1 WHERE token=?", a=[r.randint(0, 500), tokens[tid]])
    emit("dump")
    return out


# ------------------------------------------------------------------------------------------------ run
def run():
    sys.path.insert(0, PYDIR)
    os.chdir(PYDIR)
    T = [0.0]
    time.time = lambda: T[0]
    random.seed(SEED)
    import clan
    import clan_continuity as CON
    import clan_kids as KD
    import clan_portraits as PT
    import clan_reset as RST
    import clan_supply as SUP
    clan.rng = random.Random(SEED + 1)
    dbpath = os.environ["T2R_CL_DB"]
    bdir = os.environ["T2R_CL_DIR"]
    for p in (dbpath, dbpath + "-journal", "clan_events.txt", "clan_portrait_pool.txt"):
        if os.path.exists(p):
            os.remove(p)
    for k in ("T2R_CLAN_NEW_EVENTS", "T2R_PORTRAIT_POOL"):
        os.environ.pop(k, None)
    c = sqlite3.connect(dbpath)
    c.isolation_level = None                                       # autocommit: the Rust side has no implicit transaction either
    out = sys.stdout
    last_file = [None]

    def jl(s):
        return json.loads(s)

    def counts_of(d):
        return {int(k): v for k, v in (d or {}).items()}

    def dump():
        res = {}
        for name, sql in c.execute("SELECT name, sql FROM sqlite_master WHERE type='table' ORDER BY name").fetchall():
            res[name] = {"sql": sql, "rows": [list(r) for r in c.execute('SELECT * FROM "%s" ORDER BY rowid' % name).fetchall()]}
        files = {}
        if os.path.isdir(bdir):
            for f in sorted(os.listdir(bdir)):
                with open(os.path.join(bdir, f), encoding="utf-8") as fh:
                    files[f] = fh.read()
        return {"tables": res, "files": files}

    def op(o):
        k = o["op"]
        if k == "sql":
            c.execute(o["q"], o.get("a") or [])
            return None
        if k == "env":
            if o["v"] is None:
                os.environ.pop(o["k"], None)
            else:
                os.environ[o["k"]] = o["v"]
            return None
        if k == "evfile":
            p = clan.NEW_EVENTS_FILE
            if o["c"] is None:
                if os.path.exists(p):
                    os.remove(p)
            else:
                with open(p, "w", encoding="utf-8") as fh:
                    fh.write(o["c"])
                os.utime(p, (o["m"], o["m"]))
            return None
        if k == "ptfile":
            p = os.path.join(PYDIR, "clan_portrait_pool.txt")
            if o["c"] is None:
                if os.path.exists(p):
                    os.remove(p)
            else:
                with open(p, "w") as fh:
                    fh.write(o["c"])
            return None
        if k == "ensure":
            clan.ensure(c)
            return None
        if k == "load":
            return clan.load(c, o["tid"])
        if k == "save":
            cl = clan.load(c, o["tid"])
            cl.update(o["patch"])
            clan.save(c, o["tid"], cl)
            return clan.load(c, o["tid"])
        if k == "choose":
            return list(clan.choose_side(c, o["tid"], o["f"]))
        if k == "apply":
            r = clan.apply_match(c, o["tid"], o["mid"], o["parts"], o["left"], counts_of(o["counts"]) if o["counts"] is not None else None, o["mult"], o["forgive"], o["bottles"])
            return list(r)
        if k == "resolve":
            cl = clan.load(c, o["tid"])
            return list(clan.resolve_day(cl, o["parts"], o["left"], o["downs"], counts_of(o["counts"]), o["mult"], o["forgive"], o["bottles"]))
        if k == "mchoose":
            mid = o["mid"]
            if o.get("pool_idx") is not None:
                pool = clan.meta_of(clan.load(c, o["tid"])).get("pool") or []
                if o["pool_idx"] < len(pool):
                    mid = pool[o["pool_idx"]]
            return list(clan.mission_choose(c, o["tid"], mid))
        if k == "bcounts":
            return clan.booster_counts(c, o["tid"])
        if k == "bgrant":
            return clan.booster_grant(c, o["tid"], o["bid"], o["n"])
        if k == "bconsume":
            return clan.booster_consume(c, o["tid"], o["bid"])
        if k == "bnext":
            return clan.booster_next(clan.load(c, o["tid"]))
        if k == "auto":
            return clan.auto_event_mission(clan.meta_of(clan.load(c, o["tid"])))
        if k == "meta_text":
            return clan.meta_text(c, o["tid"], o["pick"], o["active"])
        if k == "state_text":
            return clan.state_text(clan.load(c, o["tid"]))
        if k == "rank":
            cl = clan.load(c, o["tid"])
            return [clan.rank_of(cl), clan.week_day(cl["day"]), clan.event_today(cl["day"])]
        if k == "pure":
            f, a, s, x = o["f"], o["a"], o["s"], o["x"]
            if f == "needed":
                return clan.needed(a[0])
            if f == "supplies_of":
                return clan.supplies_of(a[0] * 37, a[2] - 1)
            if f == "week_day":
                return clan.week_day(a[0])
            if f == "event_today":
                return clan.event_today(a[0])
            if f == "event_window":
                return clan.event_window(a[0])
            if f == "attack_story":
                return clan.attack_story(a[0])
            if f == "event_name":
                return [clan.event_name(a[2], a[1]), clan.event_name(1, a[0]), clan.event_name(2, a[0] % 3 - 1)]
            if f == "token_id":
                return clan.token_id(s)
            if f == "mission_goals":
                return [list(clan.mission_goals(m, a[1])) for m in (1, 2, 40, 52)]
            if f == "attack_tier":
                return [clan.attack_tier(a[0]), list(clan.attack_goals())]
            if f == "tier_reward":
                return list(clan.tier_reward(a[2], a[0]))
            if f == "threshold_booster":
                return clan.threshold_booster(a[1] + a[2])
            if f == "rank_days":
                return clan.rank_days(a[3] * 5, a[1], a[0], a[2] + 1)
            if f == "f32":
                return clan.f32(x)
            if f == "rnd":
                return clan.rnd(x)
            if f == "roll":
                return clan.roll(x)
            if f == "new_events":
                return clan.new_events()
            if f == "match_tag":
                return clan.match_tag(s or None)
            return None
        if k == "snap":
            w = o["w"]
            if w == "take":
                clan.snap_take(c, o["tid"], o["mid"], o["token"])
                return None
            if w == "get":
                return clan.snap_get(c, o["tid"], o["mid"])
            if w == "update":
                clan.snap_update(c, o["tid"], o["mid"], **o["kw"])
                return clan.snap_get(c, o["tid"], o["mid"])
            if w == "restore":
                s = clan.snap_get(c, o["tid"], o["mid"])
                if s is not None:
                    clan.snap_restore(c, o["tid"], o["token"], s)
                return s is not None
        if k == "sup":
            w = o["w"]
            if w == "split":
                return SUP.split(o["total"], o["weights"])
            if w == "need_split":
                return SUP.need_split(o["need"], o["n"])
            if w == "use_of":
                return SUP.use_of(o["need"])
            if w == "intake":
                return SUP.intake(o["supplies"], o["parts"], counts_of(o["counts"]), o["left"])
            if w == "book":
                return list(SUP.book(o["stock"], o["supplies"], o["need"], max(0, o["need"] // 2), o["parts"], counts_of(o["counts"]), o["left"], o["forgive"]))
        if k == "kid":
            w = o["w"]
            if w == "combat":
                return [KD.combat_text(o["x1"], "en"), KD.combat_text(o["x2"], "pl"), KD.combat_text(o["x1"], o["lang"]), KD.combat_text("")]
            if w == "kid_safe":
                return [KD.kid_safe(o["x1"], o["people"], o["x2"]), KD.kid_safe(o["x1"], o["people"]), KD.kid_safe(o["x2"], o["people"], o["x1"], "")]
            if w == "kid_text_ok":
                return [KD.kid_text_ok(o["x1"], o["people"]), KD.kid_text_ok(o["x2"], o["people"], "pl")]
            if w == "letters":
                return sorted(KD.letters_in(o["x1"]))
            if w == "kid_letters":
                return sorted(KD.kid_letters(o["x1"], o["people"]))
            if w == "is_kid":
                ps = list(o["people"].values()) + [None, {}, {"r": "kid"}]
                return [[KD.is_kid(p), KD.can_fight(p)] for p in ps]
            if w == "any_kid":
                return KD.any_kid(o["people"])
            if w == "kid_names":
                return sorted(KD.kid_names(o["st"]))
            if w == "sets":
                return [o["s"] in KD.COMBAT_KINDS, o["s"] in KD.COMBAT_POOLS_ALIFE, o["s"] in KD.COMBAT_POOLS_CHRON, o["s"] in KD.COMBAT_SCENES_SAGA]
        if k == "cont":
            w = o["w"]
            if w == "tail":
                return CON.tail(o["kind"], o["role"], o["name"], o["a"], o["b"], o["lang"])
            if w == "match_tail":
                return CON.match_tail(o["prev"], o["m"], o["lang"])
            if w == "haul_tail":
                return CON.haul_tail(o["prev"], o["lang"])
            if w == "annotate":
                rows = o["rows"]
                res = CON.annotate(rows, o["dead"], o["sick_since"], o["lang"])
                return [rows, [m["id"] for m in res]]
            if w == "render":
                return CON.render_msg(o["lid"], o["params"], o["lang"])
            if w == "recap":
                st = o["st"]
                res = CON.recap_rows(c, o["tid"], st, {"day": o["day"]}, o["mood"])
                return [res, st]
            if w == "proper":
                return sorted(CON.proper_nouns(o["extra"]), key=lambda x: (-len(x), x))
            if w == "restore_case":
                return CON.restore_case(o["text"], sorted(CON.proper_nouns(o["extra"]), key=lambda x: (-len(x), x)))
        if k == "port":
            w = o["w"]
            if w == "enabled":
                return PT.enabled()
            if w == "candidates":
                return [[row[0], wt] for row, wt in PT.candidates(o["role"], o["sex"], o["fk"])]
            if w == "pick":
                return PT.pick(o["name"], o["role"], o["sex"], o["fk"], o["used"])
            if w == "faction_of":
                return PT.faction_of(o["model"])
            if w == "look_of":
                return PT.look_of(o["model"])
            if w == "expr_for":
                return PT.expr_for(o["model"], o["want"])
        if k == "rst":
            w = o["w"]
            if w == "keys":
                return RST.keys_for(o["sid"], o["tids"])
            if w == "reset":
                return RST.reset(c, bdir, o["name"], o["sid"], o["tids"], o["names"])
            if w == "list":
                return RST.list_backups(bdir, o["sid"] or None)
            if w == "restore":
                f = o["file"]
                if f == "LAST":
                    fs = sorted(x for x in os.listdir(bdir) if x.endswith(".json")) if os.path.isdir(bdir) else []
                    f = fs[len(fs) // 2] if fs else "none.json"
                return RST.restore(c, bdir, f)
        if k == "dump":
            return dump()
        raise ValueError("unknown op %r" % (o,))

    for i, line in enumerate(sys.stdin):
        line = line.strip()
        if not line:
            continue
        o = jl(line)
        T[0] = o["t"]
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            res = op(o)
        pr = buf.getvalue().splitlines()
        out.write(json.dumps({"i": i, "r": res, "p": pr}, separators=(",", ":")) + "\n")


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "gen":
        seed, n = int(sys.argv[2]), int(sys.argv[3])
        for c in gen(random.Random(seed * 7919 + 13), n):
            sys.stdout.write(json.dumps(c, separators=(",", ":")) + "\n")
    else:
        run()
