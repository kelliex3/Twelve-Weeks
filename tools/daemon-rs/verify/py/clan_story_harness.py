"""Differential test of tools/clan_story.py (+ clan_story_pl.py and the data modules they call) against `t2r-daemon selftest clan_story`.

  T2R_PY_DIR=<dir with the deployed .py files> python3 clan_story_harness.py gen SEED N > script.jsonl
  T2R_PY_DIR=<dir> T2R_CL_DB=<fresh sqlite file> python3 clan_story_harness.py run < script.jsonl > out.py.jsonl      (cwd = the python dir)
  T2R_CL_DB=<other file> t2r-daemon selftest clan_story < script.jsonl > out.rs.jsonl                                 (cwd = the python dir)

One JSON line per call ({"i", "r"}, dict key order kept), then a dump of the clans / clan_story / clan_feed / fake_notes tables.
Time is frozen per script line ("t"). The synthetic clans are created by the real clan.py of the python dir (choose_side / apply_match on a scratch
in-memory database at generation time): the script carries the clan rows and the day results, so both sides see the same inputs.
The other clan modules clan_story calls (clan_alife, clan_chronicle, clan_journal, clan_arcs, clan_perks, clan_saga) are replaced by the
deterministic fakes below (the same fakes are in src/clan/ext_clan_story.rs); clan_kids / clan_continuity / clan are the real ones.
"""
import json
import os
import random
import sqlite3
import sys
import time
import types

PYDIR = os.environ.get("T2R_PY_DIR") or os.path.expanduser("~/rsverify/daemon/py")
sys.path.insert(0, PYDIR)
os.chdir(PYDIR)

T = [1700000000.0]
SW = {"alife": False, "chron": False}


# ------------------------------------------------------------------ the fakes
def _note(c, tid, what):
    c.execute("CREATE TABLE IF NOT EXISTS fake_notes(seq INTEGER PRIMARY KEY AUTOINCREMENT, token_id TEXT, note TEXT)")
    c.execute("INSERT INTO fake_notes(token_id,note) VALUES(?,?)", (tid, what))


def _fake_row(kind, text, st, lid, day):
    p = {}
    names = []
    if st["people"]:
        q = st["people"][0]
        names.append(q["n"])
        p["A"] = [q["n"], q["g"]]
    return {"kind": kind, "mood": "calm", "text": text, "names": names, "lid": lid, "params": {"p": p, "s": {"day": day}}}


def install_fakes():
    al = types.ModuleType("clan_alife")
    al.TICKER_SHARE = 3
    al.enabled = lambda: SW["alife"]
    al.reset = lambda c, tid: _note(c, tid, "alife.reset")

    def al_on_day(c, tid, st, newc, r, story_msgs=()):
        day = st["day"]
        _note(c, tid, "alife.on_day %d %d" % (day, len(story_msgs)))
        st["al"] = day
        return [_fake_row("alife", "AL day %d n%d" % (day, len(story_msgs)), st, "al/fake", day)]
    al.on_day = al_on_day
    al.ticker_lines = lambda c, tid, st, slot, n, lang="en", now=None, save=False: [("a%d%02d" % (slot % 100000, i), "AL tick %d %d %s" % (slot, i, lang)) for i in range(n)]
    al.render_msg = lambda lid, params, lang="en": None if lid == "al/none" else "AL[%s|%s]" % (lid, lang)

    ch = types.ModuleType("clan_chronicle")
    ch.enabled = lambda: SW["chron"]
    ch.day_rows = lambda st, newc, r, match=None, used=None, rng=None: [_fake_row("chron", "CH day %d" % st["day"], st, "ch/fake+x", st["day"])]
    ch.render_msg = lambda lid, params, lang="en": "CH[%s|%s]" % (lid, lang)

    jr = types.ModuleType("clan_journal")
    jr.purge = lambda c, tid: _note(c, tid, "journal.purge")
    jr.on_new_run = lambda c, tid, st: _note(c, tid, "journal.on_new_run %d" % len(st["people"]))
    jr.snapshot = lambda st: {"people": len(st["people"]), "day": st["day"]}
    jr.on_day = lambda c, tid, st, before, newc, r: _note(c, tid, "journal.on_day %d before=%d after=%d" % (st["day"], before["people"], len(st["people"])))

    mods = [al, ch, jr]
    for name, tag in (("clan_arcs", "ARC"), ("clan_perks", "PK"), ("clan_saga", "SG")):
        m = types.ModuleType(name)
        m.render_msg = (lambda tag: lambda lid, params, lang="en": "%s[%s|%s]" % (tag, lid, lang))(tag)
        mods.append(m)
    for m in mods:
        sys.modules[m.__name__] = m


def now():
    return T[0]


def script():
    for line in sys.stdin:
        line = line.strip()
        if line:
            yield json.loads(line)


def canon(v):
    return {k: canon(v[k]) for k in sorted(v)} if isinstance(v, dict) else v


# ------------------------------------------------------------------ generator
def gen(r, n):
    import clan
    clan.rng = random.Random(r.randrange(1 << 30))
    db = sqlite3.connect(":memory:")
    T0 = 1700000000.0 + r.randrange(1000000)
    t = [T0]
    out = []

    def emit(op, **kw):
        kw["op"] = op
        kw["t"] = round(t[0], 3)
        out.append(kw)

    def tick():
        t[0] += r.choice([0, 0.5, 3, 20, 61, 200, 900, 1000, 4000, 90000])

    clans = []
    mid_n = [0]

    def new_clan_tid():
        tid = "s:%d" % r.randrange(10 ** 16, 10 ** 17)
        fac = r.choice([clan.WLF, clan.SERAPHITES])
        c0, ok = clan.choose_side(db, tid, fac)
        clans.append({"tid": tid, "fac": fac, "mode": r.choice(["starve", "feast", "mixed", "mixed", "mixed"]), "run": False})
        emit("clan", tid=tid, clan=c0)
        if r.random() < 0.9:
            emit("new_run", tid=tid, fac=fac)
        return clans[-1]

    LANGS = ["en", "pl", "en", "pl", "", "pl-PL", "1", "xx"]

    def ins_row(tid):
        kind, lid, params, text = rand_row(r)
        emit("ins", tid=tid, id=r.randrange(1, 400), day=r.randrange(0, 40), week=r.randrange(1, 7), kind=kind, mood="calm", faction=r.choice([1, 2]), text=text,
             names=r.sample(["Lev", "Ezra", "Ana", "Mara", "Nora"], r.randrange(0, 3)), lid=lid, params=params, ts=round(t[0] - r.choice([0, 10, 5000]), 3))

    def read_ops(tid):
        if r.random() < 0.12:
            ins_row(tid)
        k = r.randrange(14)
        lang = r.choice(LANGS)
        nw = round(t[0] + r.choice([0, 0, 5, 80, 2000]), 3)
        if k == 0:
            emit("menu", tid=tid, n=r.choice([6, 6, 6, 3, 1, 10, 0]), lang=lang, now=nw, save=r.random() < 0.8)
        elif k == 1:
            emit("menu", tid=tid, n=6, lang=lang, now=nw, save=True)
        elif k == 2:
            emit("feed", tid=tid, limit=r.choice([1, 5, 30, 100, 0, -3]), since=r.choice([0, 0, 3, 40]), lang=lang, cb=r.random() < 0.4, who=r.random() < 0.3, fold=r.random() < 0.5)
        elif k == 3:
            emit("latest", tid=tid, n=r.choice([1, 3, 5, 20]), since=r.choice([0, 0, 5]), lang=lang)
        elif k == 4:
            emit("people", tid=tid, limit=r.choice([80, 3, 0]), lang=lang)
        elif k == 5:
            emit("journal", tid=tid, lang=lang, max=r.choice([3, 1, 0, 5]))
        elif k == 6:
            emit("live", tid=tid, n=r.choice([6, 2, 0]), lang=lang, now=nw, save=r.random() < 0.8)
        elif k == 7:
            emit("slot", tid=tid, slot=int(nw // 60) + r.choice([0, 1, 7, 100]), n=r.choice([3, 2, 1, 4]), lang=lang, now=nw)
        elif k == 8:
            emit("state", tid=tid)
        elif k == 9:
            emit("roster", tid=tid, save=r.random() < 0.5)
        elif k == 10:
            emit("pfw", tid=tid, n=r.choice([3, 1, 5]), window=r.choice([12, 4, 30]), lang=lang)
        elif k == 11:
            emit("short", text=rand_text(r), limit=r.choice([118, 40, 60, 5, 200]))
        elif k == 12:
            emit("xp", sym=rand_sym(r))
        else:
            emit("render", **rand_render(r))
        tick()

    for _ in range(r.randrange(2, 5)):
        new_clan_tid()
    while len(out) < n:
        cs = r.choice(clans)
        tid = cs["tid"]
        x = r.random()
        if x < 0.55:
            ld = clan.load(db, tid)
            if ld["status"] != clan.ALIVE:
                if r.random() < 0.7:
                    c0, ok = clan.choose_side(db, tid, cs["fac"] if r.random() < 0.9 else r.choice([clan.WLF, clan.SERAPHITES]))
                    if ok:
                        cs["fac"] = c0["faction"]
                        emit("clan", tid=tid, clan=c0)
                        if r.random() < 0.9:
                            emit("new_run", tid=tid, fac=c0["faction"])
                        tick()
                else:
                    read_ops(tid)
                continue
            size = ld["healthy"] + ld["hungry"] + ld["sick"]
            need = clan.needed(size)
            fac = {"starve": [0, 0, .2, .5], "feast": [1.2, 2, 3, 6], "mixed": [0, .3, .7, 1, 1.1, 1.5, 2.5]}[cs["mode"]]
            parts = int(need * 50 * r.choice(fac) * r.choice([0.8, 1, 1.2]))
            left = r.random() < 0.06
            counts = {1: r.choice([0, 0, 2, 5, 9, 12, 20, 40])} if r.random() < 0.6 else {}
            for lab in (2, 3, 4):
                if r.random() < 0.2:
                    counts[lab] = r.randrange(0, 5)
            mid_n[0] += 1
            new, rr = clan.apply_match(db, tid, "m%d" % mid_n[0], parts, left, counts, r.choice([1.0, 1.0, 1.0, 0.5, 1.4]), r.random() < 0.05, r.choice([0, 0, 0, 2, 5]))
            if rr is None:
                continue
            tick()
            emit("clan", tid=tid, clan=new)
            newc = new
            if r.random() < 0.04:
                # the roster and the clan's counts disagree (a bootstrap mismatch): the roster is trimmed / filled
                newc = dict(new, healthy=max(0, new["healthy"] + r.choice([-3, -2, -1, 1, 2, 4])))
            emit("day", tid=tid, newc=newc, r=rr)
            if r.random() < 0.5:
                read_ops(tid)
        elif x < 0.80:
            read_ops(tid)
        elif x < 0.84:
            # a story-arc / chronicle state patch: a gas mask find, a doomed or an away person, a running arc
            ch = {}
            if r.random() < 0.6:
                ch["log"] = [{"finds": [{"k": r.choice(["gasmask", "knife"])}]} for _ in range(r.randrange(0, 3))]
            if r.random() < 0.6:
                ch["doom"] = {str(r.randrange(1, 14)): r.choice(["bite", "walkout", "turned", "mercy", "suicide", "starved"]) for _ in range(r.randrange(0, 3))}
            if r.random() < 0.5:
                ch["away"] = {str(r.randrange(1, 14)): 1 for _ in range(r.randrange(0, 3))}
            if r.random() < 0.5:
                ch["arcs"] = [{"c": {"A": r.randrange(1, 14), "B": r.randrange(1, 14)}}]
            emit("patch", tid=tid, key="ch", value=ch if r.random() < 0.9 else None)
        elif x < 0.86:
            emit("cfg", alife=r.random() < 0.3, chron=r.random() < 0.15)
        elif x < 0.88:
            # a reset clan that never went through on_new_run: its day counter went back
            fresh = clan.new_clan(cs["fac"])
            fresh["healthy"] = r.randrange(1, 9)
            fresh["hungry"] = r.randrange(0, 3)
            fresh["sick"] = r.randrange(0, 3)
            new, rr = clan.resolve_day(fresh, r.choice([0, 400, 2000]))
            new["token_id"] = tid
            emit("clan", tid=tid, clan=new)
            emit("day", tid=tid, newc=new, r=rr)
        elif x < 0.90 and len(clans) < 8:
            new_clan_tid()
        elif x < 0.92:
            # the other faction of the same clan key (a different faction = a new run)
            other = clan.SERAPHITES if cs["fac"] == clan.WLF else clan.WLF
            fresh = clan.new_clan(other)
            new, rr = clan.resolve_day(fresh, r.choice([0, 600]))
            emit("clan", tid=tid, clan=new)
            emit("day", tid=tid, newc=new, r=rr)
            cs["fac"] = other
        else:
            read_ops(tid)
    # the very end: every clan read in both languages
    for cs in clans:
        for lang in ("en", "pl"):
            emit("menu", tid=cs["tid"], n=6, lang=lang, now=round(t[0], 3), save=False)
            emit("feed", tid=cs["tid"], limit=100, since=0, lang=lang, cb=True, who=False, fold=True)
            emit("people", tid=cs["tid"], limit=80, lang=lang)
    return out


def rand_row(r):
    """A stored feed row of another module (chronicle / saga / arcs / perks / continuity): kind, lid, params (JSON text or None), text."""
    pre = r.choice(["ch/", "sg/", "arc/", "pk/", "cn/", "al/", "ch/", "sg/", "arc/"])
    lid = pre + r.choice(["down", "win", "grief", "haul", "keepcb", "new.1", "spore.3", "birth", "quiet+x~y", "use", "prep", "death", "loss"])
    p = {"a": r.choice([None, "t1", "t2", ""]), "n": r.randrange(0, 5), "s": {"map": r.choice([None, "", "Lakeside", "Hospital"])}, "p": {"A": ["Lev", "m"], "D": ["Ana", "f"], "B": ["Ezra", "m"], "AD": ["x", "n"]}}
    strict = pre in ("sg/", "arc/")        # (read_feed raises on a saga / arc row whose params JSON is not an object: the modules never write one)
    if r.random() < 0.1:
        p = r.choice(([] if strict else [[], 5]) + [{"p": [1]}, {"p": {"A": 1}}, {"p": {"A": []}}])
    params = json.dumps(p) if r.random() < 0.9 else r.choice([None, "", "{bad"] + ([] if strict else ["[]"]))
    head = r.choice(["", "", "DAY 4: THE FIRE AT LEV'S. ", "DZIEŃ 9: POŻAR. ", "DAY 12: LEV AND ANA ARRIVE. ", "DAY 3: GONE. ", "DAY x: no. "])
    body = " ".join(r.choice(["Lev went out to the Hospital with Ana.", "The night was long.", "Nobody slept; Ezra counted the cans.", "We ate.", "\"Quiet,\" said Ana. (She was right.)",
                              "Mara and Lev found a gun.", "It rained!", "What now?", "No one answered", "A very long sentence that goes on and on about the camp, the fence, the river and the cold, until it passes the line limit of the timeline.",
                              "Ana, Lev; Ezra and then Mara."]) for _ in range(r.randrange(0, 7)))
    text = head + body
    kind = r.choice(["chron", "saga_x", "arc", "perk", "death_x", "loss_x", "join", "alife", "diary", "founding", "recap"])
    return kind, lid, params, text


WORDS = ["Lev", "Ezra", "stadium", "He said.", "we're", "Nobody", "DAY 4: THE FIRE.", "DZIEŃ 9: POŻAR.", "x" * 30, "ń", "I", "the camp", "wartość", "a,b;c", "...", "ok!", "Ana went home?", "\n", "|"]


def rand_text(r):
    n = r.randrange(0, 40)
    s = ""
    for _ in range(n):
        s += r.choice(WORDS) + r.choice([" ", " ", ". ", "! ", "\n", ", ", "  "])
    return s


def rand_sym(r):
    import clan_story_data as D
    import clan_story_pl as PLN
    s = {}
    if r.random() < 0.95:
        s["f"] = r.choice(["W", "S"]) if r.random() < 0.95 else r.choice(["X", 3, None])
    if r.random() < 0.9:
        s["day"] = r.randrange(0, 90)
    if r.random() < 0.9:
        s["week"] = r.randrange(1, 14)
    if r.random() < 0.8:
        s["place"] = r.choice(list(PLN.PLACES) + ["nowhere"])
    if r.random() < 0.7:
        s["inf"] = r.choice(D.INFECTED + ["zombie"])
    if r.random() < 0.7:
        s["ill"] = r.choice(list(D.ILLNESS) + ["none", "plague", ""])
    if r.random() < 0.3:
        s["ago"] = r.choice(list(PLN.AGO) + ["long ago"])
    for k in ("n", "lost", "gain"):
        if r.random() < 0.4:
            s[k] = r.choice([0, 1, 2, 3, 4, 5, 9, 10, 11, 12, 22, 25, -1, 100])
    return s


def rand_render(r):
    import clan_story_pl as PLN
    lids = list(PLN.REG)
    lid = r.choice(lids)
    if r.random() < 0.04:
        lid = r.choice(["al/zz", "cn/zz", "arc/x.y", "pk/q", "sg/q", "ch/a+b~c", "nope/00000000", "al/none", "", "t:h/ffffffff"])
    people = {}
    for L in "ABCD":
        if r.random() < 0.85:
            people[L] = [r.choice(["Lev", "Ana", "Mary Ann", "Ezra", "Ktoś", "Someone", "Łukasz", "O'Neil"]), r.choice(["m", "f", "n", "m", "f", "x"])]
    sym = rand_sym(r) if r.random() < 0.9 else {}
    params = {"p": people, "s": sym}
    if r.random() < 0.05:
        params = r.choice([None, [], {"p": 3}, {"p": {"A": ["x"]}}, {"s": []}])
    return {"lid": lid, "params": params, "lang": r.choice(["en", "pl", "pl-PL", "1", ""])}


# ------------------------------------------------------------------ run
def run():
    install_fakes()
    time.time = now
    import clan
    import clan_story as CS
    import clan_story_pl as PLN
    path = os.environ.get("T2R_CL_DB", "/tmp/clan_story_py.db")
    try:
        os.remove(path)
    except OSError:
        pass
    c = sqlite3.connect(path)
    seen = set()
    i = 0
    out = sys.stdout
    for op in script():
        if "t" in op:
            T[0] = op["t"]
        o = op["op"]
        tid = op.get("tid", "")
        lang = op.get("lang", "")
        nw = op.get("now")
        res = None
        if o == "cfg":
            SW["alife"], SW["chron"] = bool(op.get("alife")), bool(op.get("chron"))
            res = True
        elif o == "clan":
            try:
                clan.save(c, tid, op["clan"])
                res = True
            except Exception:
                res = "EXC"
        elif o == "new_run":
            try:
                res = CS.on_new_run(c, tid, op["fac"])
            except Exception:
                res = "EXC"
        elif o == "day":
            try:
                res = CS.on_day(c, tid, op["newc"], op["r"])
            except Exception:
                res = "EXC"
        elif o == "patch":
            st = CS._load(c, tid)
            if st is None:
                res = False
            else:
                if op.get("value") is not None:
                    st[op["key"]] = op["value"]
                else:
                    st.pop(op["key"], None)
                CS._save(c, tid, st)
                res = True
        elif o == "menu":
            res = [list(x) for x in CS.menu_lines(c, tid, op["n"], lang, nw, op["save"])]
        elif o == "live":
            try:
                res = [list(x) for x in CS.live_lines(c, tid, op["n"], lang, nw, op["save"])]
            except Exception:
                res = "EXC"
        elif o == "slot":
            res = [list(x) for x in CS.pick_slot(c, tid, op["slot"], op["n"], lang, nw)]
        elif o == "feed":
            res = CS.read_feed(c, tid, op["limit"], op["since"], lang, op["cb"], op["who"], op["fold"])
        elif o == "latest":
            res = CS.latest(c, tid, op["n"], op["since"], lang)
        elif o == "people":
            res = CS.read_people(c, tid, op["limit"], lang)
        elif o == "journal":
            rows = CS.read_feed(c, tid, 100, 0, lang, callbacks=True, fold=True)
            res = CS.journal(rows, op["max"])
        elif o == "render":
            try:
                res = CS.render_msg(op["lid"], op["params"], lang)
            except Exception:
                res = None
        elif o == "short":
            res = {"short": CS.short_text(op["text"], op["limit"]), "widget": CS.widget_text(op["text"])}
        elif o == "xp":
            res = {}
            for k, f in (("en", CS.expand_en), ("pl", PLN.expand)):
                try:
                    res[k] = canon({kk: str(vv) for kk, vv in f(op["sym"]).items()})
                except Exception:
                    res[k] = None
        elif o == "ins":
            try:
                c.execute("INSERT OR REPLACE INTO clan_feed(token_id,id,ts,day,week,kind,mood,faction,text,names,lid,params) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
                          (tid, op["id"], op["ts"], op["day"], op["week"], op["kind"], op["mood"], op["faction"], op["text"], json.dumps(op["names"]), op["lid"], op["params"]))
                res = True
            except sqlite3.Error:
                res = False
        elif o == "state":
            res = CS._load(c, tid)
        elif o == "roster":
            res = CS.ensure_roster(c, tid, op["save"])
        elif o == "pfw":
            rows = CS.pick_for_widget(c, tid, seen, op["n"], op["window"], lang)
            res = {"rows": [list(x) for x in rows], "seen": sorted(seen)}
        elif o == "lang":
            res = PLN.norm_lang(op["x"])
        out.write(json.dumps({"i": i, "r": res}) + "\n")
        i += 1
    c.commit()
    for t, order in (("clans", "token_id"), ("clan_story", "token_id"), ("clan_feed", "token_id,id"), ("fake_notes", "seq")):
        try:
            rows = [list(x) for x in c.execute("SELECT * FROM %s ORDER BY %s" % (t, order))]
        except sqlite3.Error:
            rows = []
        out.write(json.dumps({"table": t, "rows": rows}) + "\n")


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "gen":
        seed, n = int(sys.argv[2]), int(sys.argv[3])
        for c in gen(random.Random(seed * 7919 + 13), n):
            sys.stdout.write(json.dumps(c, separators=(",", ":")) + "\n")
    else:
        run()


if __name__ == "__main__":
    main()
