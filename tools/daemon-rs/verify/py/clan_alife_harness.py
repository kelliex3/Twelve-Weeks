"""Differential test of tools/clan_alife.py (+ clan_alife_lex.py, the text modules) and tools/clan_journal.py (+ clan_journal_entries.py, the text modules)
against `t2r-daemon selftest clan_alife` / `selftest clan_journal` (the same engine: the two harnesses differ only in their op mix).

  T2R_PY_DIR=<dir with the deployed .py files> python3 clan_alife_harness.py gen SEED N > script.jsonl
  T2R_PY_DIR=<dir> T2R_CL_DB=<fresh sqlite file> python3 clan_alife_harness.py run < script.jsonl > out.py.jsonl      (cwd = the python dir)
  T2R_CL_DB=<other file> t2r-daemon selftest clan_alife < script.jsonl > out.rs.jsonl                                 (cwd = the python dir)

The clan days run through the REAL pipeline of the python dir (clan.py clans, clan_story.on_day -> clan_alife.on_day -> clan_journal.on_day, the status cards'
events injected with "stat" ops); the A-Life / journal read functions are called with extra ops. The chronicle, arcs, saga, perks stay switched off (their
rows never occur). Time is frozen per script line ("t"). One JSON line per call ({"i", "r"}), then a dump of every table the modules write.
"""
import json
import os
import random
import sqlite3
import sys
import time

PYDIR = os.environ.get("T2R_PY_DIR") or os.path.expanduser("~/rsverify/daemon/py")
sys.path.insert(0, PYDIR)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.chdir(PYDIR)
os.environ["T2R_CLAN_ALIFE"] = "1"
os.environ["T2R_CLAN_CHRONICLE"] = "0"

T = [1700000000.0]
KEEP_OPS = {"clan", "new_run", "day", "patch", "menu", "live", "slot", "feed", "latest", "people", "journal", "state", "roster"}
CATS = ["med", "ammo", "mat", "food", "water", "xx"]


def now():
    return T[0]


def script():
    for line in sys.stdin:
        line = line.strip()
        if line:
            yield json.loads(line)


def gen(r, n, emph):
    import clan_story_harness as H
    base = H.gen(r, n)
    r2 = random.Random(r.randrange(1 << 30))
    out = []
    tids = []
    last_day = {}
    t = base[0]["t"] if base else 1700000000.0
    for op in base:
        o = op["op"]
        if "t" in op:
            t = op["t"]
        if o == "cfg":
            out.append({"op": "cfg", "alife": r2.random() < 0.9, "t": op["t"]})
            continue
        if o not in KEEP_OPS:
            continue
        tid = op.get("tid")
        if tid and tid not in tids:
            tids.append(tid)
        if o == "day":
            nd = int(op["newc"].get("day", 0))
            if r2.random() < 0.55:
                ev = []
                for _ in range(r2.randrange(1, 5)):
                    ev.append({"d": nd if r2.random() < 0.85 else nd - 1, "c": r2.randrange(0, 11), "i": r2.randrange(1, 16), "n": r2.choice(["Lev", "Ana", "Ezra", "Mara"]),
                               "g": r2.choice(["m", "f", "n"]), **({"jl": ["jn/hungry.0", {"p": {"A": ["Lev", "m"]}, "s": {"f": "W"}}]} if r2.random() < 0.1 else {})})
                out.append({"op": "stat", "tid": tid, "ev": ev, "t": op["t"]})
        out.append(op)
        if not tid:
            continue
        p = 0.5 if emph == "al" else 0.35
        q = 0.5 if emph == "jn" else 0.25
        lang = r2.choice(["en", "pl", "pl-PL", "", "en"])
        nw = round(t + r2.choice([0, 0, 5, 80, 2000, 90000]), 3)
        if o == "day" or r2.random() < 0.15:
            k = r2.random()
            if k < p * 0.5:
                out.append({"op": "al_ticker", "tid": tid, "slot": int(nw // 60) + r2.choice([0, 1, 2, 9, 123]), "n": r2.choice([3, 3, 1, 6, 0, 2]), "lang": lang, "save": r2.random() < 0.5, "t": t})
            elif k < p * 0.8:
                out.append({"op": r2.choice(["al_state", "al_public"]), "tid": tid, "lang": lang, "t": t})
            elif k < p:
                out.append({"op": r2.choice(["al_summary", "al_view", "al_feed_render"]), "tid": tid, "lang": lang, "t": t})
            if r2.random() < q * 0.8:
                out.append({"op": "jn_entries", "tid": tid, "lang": lang, "dd": r2.choice([None, None, 5, 30, 99, 0]), "t": t})
            if r2.random() < q * 0.4:
                out.append({"op": "jn_perk", "tid": tid, "cat": r2.choice(CATS), "doff": r2.choice([0, 1, 2, -1]), "who": r2.randrange(0, 9), "map": r2.choice(["", "Hospital", "Motel", "Downtown", "the Dam"]), "t": t})
            if r2.random() < q * 0.4:
                out.append({"op": "je", "tid": tid, "lang": lang, "seen": r2.choice([0, 5, 50, 500]), "t": t})
            if r2.random() < q * 0.3:
                out.append({"op": "jn_render", "tid": tid, "lang": lang, "t": t})
    for tid in tids:
        for lang in ("en", "pl"):
            out.append({"op": "al_view", "tid": tid, "lang": lang, "t": t})
            out.append({"op": "jn_entries", "tid": tid, "lang": lang, "dd": None, "t": t})
            out.append({"op": "al_feed_render", "tid": tid, "lang": lang, "t": t})
            out.append({"op": "je", "tid": tid, "lang": lang, "seen": 0, "t": t})
    return out


def _al_rows(c, tid):
    return c.execute("SELECT lid, params FROM clan_feed WHERE token_id=? AND lid LIKE 'al/%' ORDER BY id DESC LIMIT 8", (tid,)).fetchall()


def install_fake_chronicle():
    """clan_chronicle is replaced by a switched-off module whose public() is the deterministic fake of src/clan/ext_clan_alife.rs."""
    import types
    ch = types.ModuleType("clan_chronicle")
    ch.enabled = lambda: False

    def public(c, tid, lang="en", limit=40):
        maps = ["Hospital", "Motel", "Downtown", "Lakeside", "Bookstore (Part I)"]
        wins = [True, False, None]
        return {"matches": [{"d": d, "map": maps[d % 5], "win": wins[d % 3], "sup": (d * 7) % 40, "need": 20 + d % 9} for d in range(1, 30)]}
    ch.public = public
    sys.modules["clan_chronicle"] = ch


def run():
    install_fake_chronicle()
    time.time = now
    import clan
    import clan_story as CS
    import clan_story_pl as PLN
    import clan_alife as AL
    import clan_journal as JN
    import clan_journal_entries as JE
    path = os.environ.get("T2R_CL_DB", "/tmp/clan_alife_py.db")
    try:
        os.remove(path)
    except OSError:
        pass
    c = sqlite3.connect(path)
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
        try:
            if o == "cfg":
                os.environ["T2R_CLAN_ALIFE"] = "1" if op["alife"] else "0"
                res = True
            elif o == "clan":
                clan.save(c, tid, op["clan"])
                res = True
            elif o == "new_run":
                res = CS.on_new_run(c, tid, op["fac"])
            elif o == "day":
                res = CS.on_day(c, tid, op["newc"], op["r"])
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
            elif o == "stat":
                st = CS._load(c, tid)
                if st is None:
                    res = False
                else:
                    st.setdefault("ch", {}).setdefault("stat", {})["ev"] = op["ev"]
                    CS._save(c, tid, st)
                    res = True
            elif o == "menu":
                res = [list(x) for x in CS.menu_lines(c, tid, op["n"], lang, nw, op["save"])]
            elif o == "live":
                res = [list(x) for x in CS.live_lines(c, tid, op["n"], lang, nw, op["save"])]
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
            elif o == "state":
                res = CS._load(c, tid)
            elif o == "roster":
                res = CS.ensure_roster(c, tid, op["save"])
            elif o == "al_ticker":
                st = CS._load(c, tid)
                res = [list(x) for x in AL.ticker_lines(c, tid, st, op["slot"], op["n"], lang, None, op["save"])] if st else None
            elif o == "al_state":
                res = AL.read_state(c, tid, lang)
            elif o == "al_public":
                res = AL.public(c, tid, lang)
            elif o == "al_summary":
                res = AL.summary(c, tid)
            elif o == "al_view":
                res = AL.view(c, tid)
            elif o == "al_feed_render":
                res = []
                for lid, params in _al_rows(c, tid):
                    p = json.loads(params) if params else None
                    res.append([AL.render_msg(lid, p, "en"), AL.render_msg(lid, p, "pl"), AL.render_msg(lid, p, lang)])
                for lid, p in (("al/zz", {}), ("al/", {"p": {}}), ("nope", {}), ("al/wx_rain~00000000", {"p": {}}), ("", None)):
                    res.append(AL.render_msg(lid, p, lang))
            elif o == "jn_entries":
                res = JN.entries(c, tid, lang, op["dd"])
            elif o == "jn_perk":
                st = CS._load(c, tid)
                if not st or not st["people"]:
                    res = None
                else:
                    who = st["people"][op["who"] % len(st["people"])]
                    res = JN.add_perk(c, tid, st, max(0, st["day"] + op["doff"]), op["cat"], who, op["map"])
                    CS._save(c, tid, st)
            elif o == "jn_render":
                res = []
                for day, data in c.execute("SELECT day, data FROM clan_journal WHERE token_id=? ORDER BY day DESC LIMIT 3", (tid,)).fetchall():
                    rec = json.loads(data)
                    for kind, cap, sents in rec.get("pa") or []:
                        for q in sents:
                            res.append([JN.render(q, lang), JN.render(q, "en")])
                    if rec.get("ti"):
                        res.append(JN.render(rec["ti"], lang))
                res.append(JN.render(["jn/zz.1", {}], lang))
                res.append(JN.render(["jn/raid_win.9999", {"p": {}, "s": {}}], lang))
                res.append(JN.render("bad", lang))
            elif o == "je":
                st = CS._load(c, tid)
                if not st:
                    res = None
                else:
                    rows = CS.story_rows(CS.read_feed(c, tid, 100, 0, lang, callbacks=True, fold=True))
                    roster = [{"name": p["n"], "portrait": "p%d" % p["i"]} for p in st["people"]]
                    res = JE.compose_entries(rows, roster, op["seen"])
        except Exception as ex:      # noqa: BLE001
            res = "EXC"
        out.write(json.dumps({"i": i, "r": res}) + "\n")
        i += 1
    c.commit()
    for t, order in (("clans", "token_id"), ("clan_story", "token_id"), ("clan_feed", "token_id,id"), ("clan_alife", "token_id"), ("clan_journal", "token_id,day")):
        try:
            rows = [list(x) for x in c.execute("SELECT * FROM %s ORDER BY %s" % (t, order))]
        except sqlite3.Error:
            rows = []
        out.write(json.dumps({"table": t, "rows": rows}) + "\n")


def main(emph="al"):
    if len(sys.argv) >= 2 and sys.argv[1] == "gen":
        seed, n = int(sys.argv[2]), int(sys.argv[3])
        for c in gen(random.Random(seed * 7919 + 13), n, emph):
            sys.stdout.write(json.dumps(c, separators=(",", ":")) + "\n")
    else:
        run()


if __name__ == "__main__":
    main("al")
