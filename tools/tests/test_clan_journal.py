"""The clan journal's day entries (a complete journal lore overhaul: one coherent chronicle, no repeats, full pages; no two status cards
with the same sentence; and no more "{n}'s antibiotics kept you on your feet at {map}" rows).  tools/clan_journal.py + clan_journal_text(.py / _pl.py).
Temp DBs only.  Run: python tools/tests/test_clan_journal.py"""
import json, os, random, re, sqlite3, sys, tempfile
OLD_NAME = re.compile(r"factions\s*(i{2}|2)\b", re.I)   # the old product name
import sys as _t2s, os as _t2o; _t2s.path.append(_t2o.path.normpath(_t2o.path.join(_t2o.path.dirname(_t2o.path.abspath(__file__)), ".."))); import t2rtmp  # noqa: E401,E402,E702,F401  (temp hygiene: tools/t2rtmp.py)
TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
for k in ("T2R_CLAN_CHRONICLE", "T2R_CLAN_SAGA", "T2R_CLAN_STATUS", "T2R_CLAN_ALIFE"):
    os.environ[k] = "1"
sys.path.insert(0, TOOLS)
import clan  # noqa: E402
import clan_ingame  # noqa: E402
import clan_journal as J  # noqa: E402
import clan_journal_text as EN  # noqa: E402
import clan_journal_text_pl as PL  # noqa: E402
import clan_perks  # noqa: E402
import clan_status  # noqa: E402
import clan_story as CS  # noqa: E402
import clan_story_data as D  # noqa: E402
import clan_supply  # noqa: E402

FAILS = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        FAILS.append(what)

TID = "0123456789abcdef01234567"
TMP = tempfile.mkdtemp()
MAPS = ["Overpass", "Jackson Suburbs", "Hillside", "Lodge", "Convention Center", "Hospital", "School", "Motel", "Bookstore", "Train Station"]


def run(seed, fac=clan.WLF, days=84, lo=0.8, hi=1.7, perks=False):
    """A clan played for up to `days` matches through the daemon's own path (clan.resolve_day -> clan_story.on_day [-> clan_perks.on_day])."""
    p = os.path.join(TMP, "j%d_%d_%d.db" % (seed, fac, days))
    rng = random.Random(seed)
    clan.rng = random.Random(seed + 1)
    c = sqlite3.connect(p)
    clan.ensure(c); CS.ensure(c)
    cl = clan.new_clan(fac)
    clan.save(c, TID, cl)
    CS.on_new_run(c, TID, fac)
    cards = []
    for _ in range(days):
        cl = clan.load(c, TID)
        if cl["status"] != clan.ALIVE:
            break
        meta = clan.meta_of(cl)
        w = clan.event_window(cl["day"])
        if not meta.get("m") and w >= 0:
            meta["m"] = {"id": 1 + (w // 5) % 4, "prog": 0, "left": 3, "ev": w}
            cl["meta"] = json.dumps(meta)
        n = cl["healthy"] + cl["hungry"] + cl["sick"]
        need = clan.needed(n)
        parts = int(need * 50 * rng.uniform(lo, hi))
        kills = rng.randrange(2, 16)
        counts = {1: kills, 2: rng.randrange(1, 5), 3: rng.randrange(1, 4), 4: rng.randrange(2, 6)}
        newc, r = clan.resolve_day(cl, parts, counts=counts)
        _stock, r["sup"] = clan_supply.book((meta.get("sup") or {}).get("stock"), r["supplies"], r["needed"], n, parts, counts)
        r["match"] = {"map": rng.choice(MAPS), "kills": kills, "deaths": rng.randrange(0, 5), "downs": rng.choice([0, 1, 2, 3, 4]), "win": rng.random() < 0.55,
                      "execs": counts[2], "revives": counts[3], "headshots": counts[4]}
        newc["last_result"] = json.dumps(r)
        clan.save(c, TID, newc)
        CS.on_day(c, TID, newc, r)
        if perks:
            pc = dict(counts); pc.update({31: 1 | 1 << 4, 32: 1}); clan_perks.on_day(c, TID, newc, r, pc, r["match"])
        cards.append([clan_status.story_line(e, "en") for e in clan_status.pending(CS._load(c, TID))])
    c.commit()
    return c, cards


def sentences(ents):
    out = []
    for e in ents:
        for _cap, t in e["paras"]:
            out += [x for x in re.split(r"(?<=[.!?])\s+", t) if x]
    return out


# ---- the entry set: the Polish twin, placeholders, faction vocabulary
check(set(EN.POOLS) == set(PL.POOLS) and all(len(EN.POOLS[k]) == len(PL.POOLS[k]) for k in EN.POOLS), "every pool has its Polish twin, line for line (%d pools, %d lines)" % (len(EN.POOLS), sum(len(v) for v in EN.POOLS.values())))
FULL = {"f": "W", "map": "Hillside", "sup": 7, "need": 12, "short": 5, "over": 0, "k": 11, "x": 3, "r": 2, "hs": 5, "dw": 4, "lost": 3, "gain": 2, "n": 3, "size": 14, "week": 4,
        "ill": "flu", "lack": "water", "names": ["Ana", "Rudy", "Kion"], "more": 2}
PPL = {"p": {"A": ["Ana", "f"], "B": ["Rudy", "m"]}}
bad = []
for pool, lines in EN.POOLS.items():
    for i, (scope, _t) in enumerate(lines):
        for fk in ("W", "S"):
            if scope not in ("*", fk):
                continue
            for lang in ("en", "pl"):
                t = J.render(["jn/%s.%d" % (pool, i), dict(PPL, s=dict(FULL, f=fk))], lang)
                if not t or "{" in t or "}" in t or "|" in t:
                    bad.append((pool, i, lang, t))
check(not bad, "every line renders in English and Polish with nothing left unfilled %s" % (bad[:3],))
voice = []
for pool, lines in EN.POOLS.items():
    for i, (scope, t) in enumerate(lines):
        low = " " + t.lower() + " "
        if scope in ("*", "S") and any(w in low for w in D.BANNED_SERA):
            voice.append((pool, i, "S"))
        if scope in ("*", "W") and any(w in low for w in D.BANNED_WOLF):
            voice.append((pool, i, "W"))
check(not voice, "no line a faction can draw uses the other faction's words %s" % (voice[:4],))
check(all(1 <= len(EN.POOLS[k]) for k in EN.POOLS) and not any(OLD_NAME.search(t) for v in EN.POOLS.values() for _s, t in v), "no empty pool, no visible old product name")

# ---- a whole run: one entry per clan day, tied to the day's real numbers, nothing told twice
for seed, fac in ((7, clan.WLF), (3, clan.SERAPHITES)):
    c, cards = run(seed, fac)
    cl = clan.load(c, TID)
    for lang in ("en", "pl"):
        ents = J.entries(c, TID, lang)
        days = [e["day"] for e in ents]
        check(days == list(range(0, len(days))) and len(days) == cl["day"] + 1, "seed %d %s: one entry per clan day, days 0..%d in order (%d entries)" % (seed, lang, cl["day"], len(ents)))
        prose = [s for s in sentences(ents) if not re.search(r"\d", s)]
        dup = sorted({s for s in prose if prose.count(s) > 1})
        dup = [s for s in dup if not re.match(r"^\w+ (went hungry|is fed again|died|recovered|fell sick|joined the clan|głoduje|znów je do syta|nie żyje|wyzdrowiał|wyzdrowiała)", s)]
        check(not dup, "seed %d %s: no sentence is told twice in the run %s" % (seed, lang, dup[:3]))
        check(all(e["title"] for e in ents), "seed %d %s: every entry has a headline" % (seed, lang))
        check(max(sum(len(t) for _c, t in e["paras"]) for e in ents) <= 1100, "seed %d %s: the longest entry fits the reading pane (%d chars)" % (seed, lang, max(sum(len(t) for _c, t in e["paras"]) for e in ents)))
    ents = J.entries(c, TID, "en")
    hist = c.execute("SELECT data FROM clan_journal WHERE token_id=? ORDER BY day", (TID,)).fetchall()
    ok = True
    size = clan.START_SURVIVORS
    for (data,) in hist[1:]:
        rec = json.loads(data)
        ok &= rec["h"] + rec["u"] + rec["s"] == size + rec["dn"]
        size = rec["h"] + rec["u"] + rec["s"]
    check(ok and size == cl["healthy"] + cl["hungry"] + cl["sick"], "seed %d: the entries' survivor counts chain day to day and end at the clan's real size (%d)" % (seed, size))
    st = CS._load(c, TID)
    dead = {d["n"] for d in st["dead"]}
    late = [(e["day"], n) for e in ents for n in dead if any(re.search(r"(?<!\w)%s(?!\w)" % re.escape(n), t) for _c, t in e["paras"])
            and e["day"] > max(d["d"] for d in st["dead"] if d["n"] == n) and not any(p["n"] == n for p in st["people"])]
    check(not late, "seed %d: no entry names a member after the day they died %s" % (seed, late[:3]))
    enemy, own = ("Scars", "Wolves") if fac == clan.WLF else ("Wolves", "Scars")
    text = " ".join(sentences(ents))
    check(enemy in text and own not in text, "seed %d: the enemy is called %s, never %s" % (seed, enemy, own))
    check(not [d for d in cards if len(set(d)) < len(d)], "seed %d: no two pending STATUS UPDATES cards read the same" % seed)
    weeks = {}
    for e in ents:
        weeks.setdefault(e["week"], []).append(e)
    full = [w for w, v in weeks.items() if w < max(weeks)]
    check(all(len(weeks[w]) >= 7 for w in full) and len(weeks) == (cl["day"] - 1) // 7 + 1, "seed %d: %d week pages, every finished week holds its 7 days" % (seed, len(weeks)))

    # ---- the wire: section 6 (week pages) and section 2 (the same entries for an older client)
    t6 = clan_ingame.build(c, TID, "en", 6, 0, 2)
    rows = [l.split("\t") for l in t6.split("\n")]
    K = [r for r in rows if r[0] == "K"]
    Dr = [r for r in rows if r[0] == "D"]
    G = [r for r in rows if r[0] == "G"]
    check(len(K) == 1 and K[0][1] == "2" and int(K[0][2]) == max(weeks) and len(Dr) == len(ents) and all(len(r) == 16 for r in Dr), "seed %d: section 6 lists every entry (%d D rows) and names week 2 of %s" % (seed, len(Dr), K[0][2] if K else "?"))
    wk2 = {str(e["day"]) for e in weeks.get(2, [])}
    check(G and {g[1] for g in G} <= wk2 and all(len(g) == 4 and g[3] for g in G), "seed %d: the paragraphs are the asked week's only (%d G rows)" % (seed, len(G)))
    check(len(t6.encode("utf-8")) < 24000 and len(clan_ingame.build(c, TID, "pl", 6, 0, 0).encode("utf-8")) < 24000, "seed %d: a week page reply stays small (%d bytes)" % (seed, len(t6.encode("utf-8"))))
    t2 = clan_ingame.build(c, TID, "en", 2)
    Jr = [l.split("\t") for l in t2.split("\n") if l.startswith("J\t")]
    check(0 < len(Jr) <= clan_ingame.MAX_JOURNAL and all(len(r) == 14 and r[3] and r[4] and not r[6] for r in Jr) and Jr[0][2].startswith("WEEK "), "seed %d: section 2 carries the newest %d day entries for older clients" % (seed, len(Jr)))
    check(sum(1 for r in Jr if r[12] == "1") == 1 and sum(1 for r in Dr if r[15] == "1") == 1, "seed %d: a first visit marks only the newest entry NEW" % seed)

# ---- the supply perks: no feed rows any more, one journal sentence a day, never the same twice
c, _cards = run(11, clan.WLF, days=40, perks=True)
kept = c.execute("SELECT COUNT(*) FROM clan_feed WHERE token_id=? AND lid='pk/use'", (TID,)).fetchone()[0]
check(kept == 0, "a used supply perk writes no feed row (the 'kept you on your feet at <map>' rows are gone)")
ents = J.entries(c, TID, "en")
recs = [json.loads(d) for (d,) in c.execute("SELECT data FROM clan_journal WHERE token_id=? ORDER BY day", (TID,))]
npk = sum(1 for r in recs if r.get("pk"))
lids = [q[0] for r in recs for p in r["pa"] for q in p[2] if q[0].startswith("jn/perk_")]
check(npk >= 1 and len(lids) == npk and len(set(lids)) == len(lids), "perk use: one sentence in the day's entry, each line once in the run (%d days)" % npk)

# ---- a new run owns its entries; a second match on a day that did not count is added to that day's entry
c, _cards = run(5, clan.WLF, days=6)
n0 = c.execute("SELECT COUNT(*) FROM clan_journal WHERE token_id=?", (TID,)).fetchone()[0]
CS.on_new_run(c, TID, clan.SERAPHITES)
rows = c.execute("SELECT day FROM clan_journal WHERE token_id=?", (TID,)).fetchall()
check(n0 == 7 and rows == [(0,)], "a new clan starts with the founding page only (the old run's %d entries are gone)" % n0)
e0 = J.entries(c, TID, "en", 0)
check(len(e0) == 1 and "elders" in e0[0]["paras"][0][1] and e0[0]["paras"][0][0] == "DAY ONE", "the founding page speaks in the clan's faction voice")
cl = clan.new_clan(clan.SERAPHITES)
clan.save(c, TID, cl)
st = CS._load(c, TID)
for k in range(2):
    before = J.snapshot(st)
    r = {"day": 0, "parts": 300, "supplies": 6, "needed": 9, "left": 0, "died": 0, "counted": 0 if k == 0 else 1, "match": {"map": "Motel", "win": k == 1, "kills": 3}}
    J.on_day(c, TID, st, before, dict(cl, day=k, healthy=5, hungry=0, sick=0, status=clan.ALIVE), r)
e1 = [e for e in J.entries(c, TID, "en", 1) if e["day"] == 1]
check(len(e1) == 1 and sum(1 for cap, _t in e1[0]["paras"] if cap == "THE RAID") == 2, "two matches on one clan day are one entry with both raids")

print("\n%d failed" % len(FAILS) if FAILS else "\nall ok")
sys.exit(1 if FAILS else 0)
