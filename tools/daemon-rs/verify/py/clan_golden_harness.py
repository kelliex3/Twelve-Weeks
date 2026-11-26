"""GOLDEN test of the clan engine: the whole day pipeline, N days of random reports for synthetic clans, against `t2r-daemon selftest clan_golden`.

  python3 clan_golden_harness.py gen SEED DAYS CLANS > script.jsonl            (T2R_PY_DIR = the deployed .py files, cwd = that dir)
  T2R_TEST_SEED=SEED T2R_PY_DIR=<dir> T2R_CL_DB=<db> T2R_GOLD_DIR=<dir> python3 clan_golden_harness.py run < script.jsonl > out.py.jsonl
  T2R_TEST_SEED=SEED T2R_CL_DB=<db> T2R_GOLD_DIR=<dir> t2r-daemon selftest clan_golden < script.jsonl > out.rs.jsonl          (cwd = the python dir)

NO doubles: every clan module is the real one on both sides (clan, clan_supply, clan_kids, clan_reset, clan_continuity, clan_portraits, clan_story(+_pl),
clan_chronicle, clan_arcs, clan_saga(+_pl), clan_loot, clan_alife, clan_journal, clan_perks, clan_status, clan_ingame, achievements).

The script is a list of daemon-level events (one JSON object per line, "t" = the frozen time).  The run side calls the clan ENGINE functions in the order and
with the arguments daemon.py does (every call carries its daemon.py line; the daemon's own session / anti-cheat / matchmaking decisions are script inputs):

  side   0xE4 clan_side            daemon.py:1672  choose_side + story.on_new_run + achievements.book_side        reply = state_text (clan_op 0xE4)
  begin  clan_match_started        daemon.py:2337  CLAN_MAP / CLAN_TEAM / CLAN_MATCH + boost_start (booster_consume)
  live   0xAB ach_live_op          daemon.py:1836  achievements.book_live                                       reply = ach_read
  a9     0xA9 ach_report_op        daemon.py:1790  achievements.book_match on a booked day / held (ACH_PEND)   reply = ach_read
  report 0xE2 clan_report          daemon.py:2203  -> clan_finalize daemon.py:1888 (snap_*, stats_*, apply_match, story.on_day, perks.on_day, ach_book_day)
  end    host 0x12 + clan_match_ended daemon.py:8330 / 2368 (stats_roster, CLAN_WIN, finalize the pending)
  tick   clan_scheduler            daemon.py:2090  finalize the reports whose grace ran out
  rejoin clan_rejoin_after_booking daemon.py:6963
  pick   0xEE / 0xEF meta_op       daemon.py:2400  booster pick / mission choose                                 reply = meta_text
  read   0xE0 / 0xE8 / 0xB4 / 0xB6 / 0xAA   clan_op / meta_op / feed_op / clan13_op / ach_read_op
  reset / restore  admin_clan_reset / admin_clan_restore  daemon.py:10866
  mark   end of a day: sha256 + row count of every table (T2R_GOLD_FULL=1: the rows themselves)
Not mirrored (daemon glue): clan_key's steam-account adoption, the leave verdicts (excuse / kick / quit are script flags), anti-cheat clamps, book_career_stats (pstats),
the feed reply cache, the "ro=" role tail, "mf=" of the meta reply.
"""
import hashlib
import json
import os
import random
import sqlite3
import sys
import time
import zlib

PYDIR = os.environ.get("T2R_PY_DIR") or os.path.expanduser("~/rsverify/daemon/py")
sys.path.insert(0, PYDIR)
os.chdir(PYDIR)

T = [0.0]
MAPS = ["Hospital", "Motel", "Downtown", "Lakeside", "Bookstore (Part I)", "Capitol Shops", "Suburbs", "University", "Plaza"]
NAMES = ["Kellie", "Zażółć", "Ünal", "MAX", "x_x", "Łukasz", "Дима", "a|b", "Mai Chan", "Joel", "ellie_1", "Tess", "Nora", "Abby", "Bill", "Dina", "Jesse", "Lev",
         "Yara", "Mel", "Owen", "Manny", "Seth", "Hana", "Ríos", "Björk", "Çağlar", "Ōtsuki", "Zoë", "O'Neil"]
ACH_E2_ONLY = (1, 2, 3, 4, 5, 6, 21, 22, 23, 24, 33)     # daemon.py:1747
CLAN_WEAPONS = ("", "bow", "revolver", "shorty", "hunting", "semi", "burst", "pistol", "crossbow", "melee", "bomb", "molotov")   # daemon.py:1701
FEED_LINES = 6
CLAN_GRACE = 20.0


def dumps(o):
    return json.dumps(o, separators=(",", ":"))


def sha(s):
    return hashlib.sha256(s.encode("utf-8")).hexdigest()[:24]


# ================================================================== generator
def gen(seed, days, nclans):
    import achievements as A
    import clan
    r = random.Random(seed * 7919 + 13)
    out = []
    t = [1.70e9 + r.randrange(10 ** 7)]

    def E(**o):
        o["t"] = round(t[0], 3)
        out.append(o)

    def adv(a, b):
        t[0] += r.uniform(a, b)

    E(op="init", seed=seed)
    E(op="ensure")
    clans = []
    for i in range(nclans):
        tok = "%064x" % r.getrandbits(256)
        cd = {"tok": tok, "key": "s:%d" % r.randrange(10 ** 16, 10 ** 17), "name": r.choice(NAMES) + (str(i) if r.random() < .5 else ""),
              "style": "hero" if i == 0 else r.choice(["starve", "mixed", "mixed", "feast", "feast", "huge"]), "lang": r.choice([0, 0, 1, 1, 2, 255]),
              "fac": r.choice([clan.WLF, clan.SERAPHITES])}
        clans.append(cd)
        E(op="player", tok=tok, key=cd["key"], name=cd["name"])
    for cd in clans:
        E(op="side", tok=cd["tok"], fac=cd["fac"], only_over=False, lang=cd["lang"])
    labs = list(range(1, 91))

    def counts(style):
        c = {}
        big = style in ("feast", "huge", "hero")
        if style == "hero":
            for lab in (1, 2, 3, 4, 5):                  # (the attack days of the run need the count of the day's mission)
                c[lab] = r.randint(25, 150)
        elif r.random() < .85:
            c[1] = r.choice([0, 1, 2, 3, 5, 8, 12, 20, 40, 90] if big else [0, 0, 1, 2, 4, 7])
        for lab in (2, 3, 4, 5):
            if r.random() < .35:
                c[lab] = r.randrange(0, 7)
        if r.random() < .15:
            c[6] = r.randrange(0, 5)
        if r.random() < .5:
            c[21] = r.randrange(0, 12)
            c[22] = r.randrange(0, 10)
        if r.random() < .4:
            c[23] = r.randrange(0, 15)
        if r.random() < .3:
            c[24] = r.randrange(0, 14)
        if r.random() < .5:
            c[31] = r.randrange(0, 4096)
        if r.random() < .4:
            c[32] = r.randrange(0, 256)
        if r.random() < .25:
            c[33] = r.choice([0, 1, 2, 5, 12, 40, 300])
        if r.random() < .02:
            c[r.randrange(1, 90)] = r.choice([0, 65535, 9999])
        return {str(k): v for k, v in c.items()}

    def a9counts():
        c = {}
        for _ in range(r.randint(1, 14)):
            lab = r.choice(labs) if r.random() < .9 else r.randrange(91, 255)
            c[str(lab)] = r.choice([0, 1, 1, 2, 3, 5, 9, 20, 60, 300, 65535])
        for lab in (1, 2, 3, 21, 22):
            if r.random() < .3:
                c[str(lab)] = r.randrange(0, 40)
        return c

    def parts_of(style):
        if style == "hero":
            return 0 if r.random() < .03 else r.randint(30000, 100000)
        lo, hi = {"starve": (0, 900), "mixed": (100, 4000), "feast": (1500, 9000), "huge": (6000, 40000)}[style]
        x = r.random()
        if x < .05:
            return 0
        if x < .1:
            return r.choice([100000, 250000, 99999])
        return int(r.uniform(lo, hi))

    mnum = [0]
    resets = []
    for day in range(days):
        adv(4 * 3600, 30 * 3600)
        nm = r.choice([0, 1, 1, 2, 2, 3, 4])
        for _ in range(nm):
            mnum[0] += 1
            mid = "%08x" % r.getrandbits(32)
            k = r.randint(1, min(8, len(clans)))
            ps = r.sample(clans, k)
            if clans[0] not in ps and r.random() < .85:          # clan 0 is the hero: it plays nearly every match and is always fed (runs reach the winning day)
                ps[r.randrange(len(ps))] = clans[0]
            for p in ps:
                if r.random() < .7:
                    E(op="side", tok=p["tok"], fac=r.choice([clan.WLF, clan.SERAPHITES]), only_over=True, lang=p["lang"])
                if r.random() < .25:
                    E(op="pick", tok=p["tok"], what="booster", id=r.randrange(0, 12))
                if r.random() < .12:
                    E(op="pick", tok=p["tok"], what="mission", id=r.randrange(0, 9))
                if r.random() < .15:
                    E(op="read", tok=p["tok"], kind=r.choice(["E0", "E8"]), lang=p["lang"])
            adv(1, 30)
            mp = r.choice(MAPS) if r.random() < .93 else ""
            team = {p["tok"]: r.randrange(2) for p in ps}
            E(op="begin", mid=mid, map=mp, players=[{"tok": p["tok"], "team": team[p["tok"]], "mult": r.choice([1.0, 1.0, 1.0, 1.0, 0.5, 1.4, 2.0, 3.0]) if r.random() < .15 else 1.0,
                                                    "late": r.choice([100, 200, 300]) if r.random() < .1 else 0} for p in ps])
            for p in ps:
                if r.random() < .4:
                    for _ in range(r.randint(1, 3)):
                        adv(0.5, 60)
                        E(op="live", tok=p["tok"], counts=a9counts())
                if r.random() < .25:
                    adv(1, 10)
                    E(op="a9", tok=p["tok"], counts=a9counts())
            adv(120, 1800)
            kinds = {}
            for p in ps:
                x = r.random()
                kinds[p["tok"]] = ("normal" if x < .62 else "early" if x < .74 else "left" if x < .82 else "own" if x < .88 else "quit" if x < .92 else "kick" if x < .96 else "exc")
            ended = r.random() < .9
            if not ended:                                            # the host never sent the result: everyone's own game ended, or left
                for tk in kinds:
                    if kinds[tk] in ("normal", "early"):
                        kinds[tk] = "own"

            def report(p, flags, **kw):
                tk = p["tok"]
                adv(0.2, 5)
                parts = 0 if (kw.get("kick") or kw.get("exc")) and r.random() < .4 else parts_of(p["style"])
                E(op="report", tok=tk, parts=parts, flags=flags, counts=counts(p["style"]), lang=p["lang"], **kw)

            for p in ps:
                kd = kinds[p["tok"]]
                if kd in ("early", "left"):
                    report(p, 0)
                elif kd == "own":
                    report(p, 2)
            if any(kinds[p["tok"]] == "left" for p in ps):
                adv(21, 40)
                E(op="tick")
            if ended:
                adv(1, 20)
                dep = [p["tok"] for p in ps if kinds[p["tok"]] == "left"]
                E(op="end", mid=mid, winner=r.choice([0, 1, 2, 0, 1, 3, -1]), players=[p["tok"] for p in ps], departed=dep)
            for p in ps:
                kd = kinds[p["tok"]]
                if kd == "normal":
                    report(p, r.choice([0, 0, 0, 4]))
                elif kd == "quit":
                    report(p, 0, quit=True)
                elif kd == "kick":
                    report(p, 0, kick=True)
                elif kd == "exc":
                    report(p, 0, exc=True)
                if r.random() < .08:
                    adv(1, 5)
                    E(op="report", tok=p["tok"], parts=0, flags=0, counts={}, lang=p["lang"], resend=True)
            adv(5, 60)
            for p in ps:
                if r.random() < .35:
                    E(op="a9", tok=p["tok"], counts=a9counts())
                if r.random() < .07:
                    adv(1, 20)
                    E(op="rejoin", tok=p["tok"])
                    adv(60, 400)
                    report(p, 2)
        # reads of the day
        for _ in range(r.randint(2, 7)):
            p = r.choice(clans)
            adv(0.5, 90)
            x = r.random()
            if x < .25:
                E(op="read", tok=p["tok"], kind=r.choice(["E0", "E8", "AA", "RANK"]), lang=p["lang"])
            elif x < .5:
                E(op="read", tok=p["tok"], kind="B4", lang=r.choice([p["lang"], 0, 1]))
            else:
                E(op="read", tok=p["tok"], kind="B6", lang=r.choice([p["lang"], 0, 1]), section=r.choice([1, 1, 2, 2, 3, 4, 5, 6, 7, 8]),
                  seen_id=r.choice([0, 0, r.randrange(0, 900)]), seen_day=r.choice([0, 0, r.randrange(0, 90)]), big=r.randrange(2))
        if day % 11 == 7 and len(clans) > 1:
            p = r.choice(clans[1:])
            adv(1, 30)
            E(op="reset", tok=p["tok"], names=r.random() < .3)
            resets.append(p["tok"])
            adv(5, 600)
            if r.random() < .5:
                E(op="restore", tok=p["tok"], idx=r.randrange(0, 8))
        adv(1, 30)
        E(op="mark", day=day)
    for o in out:
        print(dumps(o))


# ================================================================== run (the Python side: the clan engine in daemon.py's order)
def run():
    import achievements as A
    import clan
    import clan_ingame as I
    import clan_perks as P
    import clan_reset as RS
    import clan_status as ST
    import clan_story as CS
    full = bool(os.environ.get("T2R_GOLD_FULL"))
    time.time = lambda: T[0]
    seed = int(os.environ.get("T2R_TEST_SEED", "0") or 0)
    random.seed(seed)
    clan.rng = random.Random(seed + 1)
    dbp = os.environ.get("T2R_CL_DB") or "/tmp/clan_golden_py.db"
    for p in (dbp, dbp + "-journal"):
        try:
            os.remove(p)
        except OSError:
            pass
    rdir = os.path.join(os.environ.get("T2R_GOLD_DIR") or "/tmp/clan_golden_py", "clan-resets")
    import shutil
    shutil.rmtree(rdir, ignore_errors=True)
    c = sqlite3.connect(dbp)
    PL = {}                                   # token -> {"key", "name"}   (the players table / clan_key)
    ACH_DONE, ACH_PEND, ACH_DAY, ACH_LIVE_AT = {}, {}, {}, {}
    CLAN_MATCH, CLAN_LAST_MATCH, CLAN_TEAM, CLAN_MAP, CLAN_ENDED, CLAN_PENDING, CLAN_DUE = {}, {}, {}, {}, {}, {}, {}
    CLAN_CREDITED, CLAN_REBOOK, CLAN_DONE, CLAN_WIN = set(), set(), {}, {}
    QUIT, KICK, EXC = set(), set(), set()
    LATE_MULT, LATE_BONUS = {}, {}
    BOOST_PICK, BOOST_ACTIVE = {}, {}
    lang_of = lambda b: CS.PLN.norm_lang(bytes([b & 255]))

    def key_of(tok):
        return PL[tok]["key"]

    # -------------------------------------------------------------- replies
    def state_reply(tok, op):                                                          # clan_op, daemon.py:2594
        key = key_of(tok)
        st = clan.load(c, key)
        txt = clan.state_text(st)
        if op in (0xE0, 0xE2):
            try:                                                                       # daemon.py:2629
                story = CS._load(c, key)
                tail = P.wire_text(P.active(st, story), P.loss_streak(story))
                if tail:
                    txt += " " + tail
            except Exception:
                pass
            try:                                                                       # daemon.py:2642
                story = CS._load(c, key)
                tail = ST.wire_text(story)
                if tail:
                    txt += " " + tail
            except Exception:
                pass
        return txt

    def ach_read(tok, op):                                                             # daemon.py:1866
        try:
            key = key_of(tok)
            row = clan.load(c, key) or {}
            txt = A.wire_text(c, key, int(row.get("days_total") or 0) // 7)
            txt += "LIVE 1\n"
        except Exception:
            txt = ""
        return [op, txt]

    def ach_merge(e2, a9):                                                             # daemon.py:1763
        m = {int(k): int(v) for k, v in (e2 or {}).items()}
        for k, v in (a9 or {}).items():
            k = int(k)
            if k in ACH_E2_ONLY:
                continue
            m[k] = max(m.get(k, 0), int(v))
        return m

    def ach_book_day(tok, mid, counts, win, left, late, before, after, r):             # daemon.py:1771
        got = []
        try:
            key = key_of(tok)
            done = ACH_DONE.setdefault((tok, mid), {})
            first = (tok, mid) not in ACH_DAY
            ACH_DAY[(tok, mid)] = {"e2": dict(counts or {}), "win": win, "left": bool(left), "late": bool(late)}
            A.settle(c, key, counts, done, ACH_E2_ONLY)
            got = A.book_match(c, key, ach_merge(counts, ACH_PEND.pop((tok, mid), None)), done, win=win, left=left, late=late, first=first)
            if r:
                got += [a for a in A.book_clan(c, key, before, after, r) if a not in got]
        except Exception:
            return None
        return got

    def match_facts(tok, mid, r, parts, counts):                                       # clan_match_facts, daemon.py:1692
        cnt = dict(counts or {})
        m = {"map": CLAN_MAP[mid], "supplies": int(r.get("supplies", 0)), "parts": int(parts), "kills": int(cnt.get(1, 0)), "mid": mid}
        if cnt:
            for lab, key in ((2, "execs"), (3, "revives"), (4, "headshots"), (5, "marked"), (21, "downs"), (22, "deaths"), (23, "streak")):
                if lab in cnt or lab <= 5:
                    m[key] = int(cnt.get(lab, 0))
            w = int(cnt.get(24, 0))
            if 0 < w < len(CLAN_WEAPONS):
                m["weapon"] = CLAN_WEAPONS[w]
        res = CLAN_WIN.pop((tok, mid), "none")
        if res in (True, False):
            m["win"] = res
        return m

    def finalize(tok, res):                                                            # clan_finalize, daemon.py:1888
        p = CLAN_PENDING.pop(tok, None)
        if not p:
            return
        mid, parts, flags, counts = p
        CLAN_DONE[tok] = mid                                                           # daemon.py:1935
        if (tok, mid) in CLAN_CREDITED:
            res.append(["credited"])
            return
        left = (mid not in CLAN_ENDED and not (flags & 2)) or (tok, mid) in QUIT      # daemon.py:1941
        forgive_excused = False
        if (tok, mid) in KICK:                                                         # daemon.py:1973
            if int(parts) <= 0:
                res.append(["kick0"])
                return
            left, forgive_excused = False, True
        if left and (tok, mid) in EXC and (tok, mid) not in QUIT:                      # daemon.py:1989
            if int(parts) <= 0:
                res.append(["exc0"])
                return
            left, forgive_excused = False, True
        key = key_of(tok)
        dup = clan.load(c, key).get("last_mid") == mid                                 # daemon.py:1996
        prev = clan.snap_get(c, key, mid)
        rebook = bool(dup and prev and prev.get("stint") and ((tok, mid) in CLAN_REBOOK or prev.get("rejoined")))
        if dup and not rebook:
            res.append(["dup"])
            return
        late, mult0 = 0, 1.0
        if rebook:                                                                     # daemon.py:2003
            st1 = prev["stint"]
            counts2, merged = dict(counts or {}), {int(k): v for k, v in st1["counts"].items()}
            for k, v in counts2.items():
                if k == 23:
                    merged[k] = max(merged.get(k, 0), v)
                elif k == 24:
                    merged[k] = merged.get(k, 0) or v
                else:
                    merged[k] = merged.get(k, 0) + v
            parts, counts = max(0, min(int(parts) + int(st1["parts"]), 100000)), merged
            late, mult0 = int(st1.get("late", 0)), float(st1.get("mult", 1.0))
            forgive_excused = forgive_excused or bool(st1.get("forgive"))
            LATE_MULT.pop((tok, mid), None)
            LATE_BONUS.pop((tok, mid), None)
            clan.snap_restore(c, key, tok, prev)
        else:
            try:                                                                       # daemon.py:2021
                clan.snap_take(c, key, mid, tok)
            except Exception:
                pass
        raw_parts, raw_counts = int(parts), dict(counts or {})
        CLAN_CREDITED.add((tok, mid))
        try:                                                                           # daemon.py:2030
            nm = PL[tok]["name"] or "?"
            I.stats_report(c, mid, tok, nm, CLAN_TEAM.get(mid, {}).get(tok, -1), counts, left)
            I.stats_parts(c, mid, tok, parts)
        except Exception:
            pass
        mult = mult0 if rebook else LATE_MULT.pop((tok, mid), 1.0)                     # daemon.py:2038
        if not left:
            late = late if rebook else LATE_BONUS.pop((tok, mid), 0)
            if late:
                parts += late
        _ach_win = CLAN_WIN.get((tok, mid), "none")                                    # daemon.py:2049
        _ach_late = bool(late) or mult > 1.0
        before = clan.load(c, key)                                                     # daemon.py:2051
        st, r = clan.apply_match(c, key, mid, parts, left, counts, mult=mult if not left else 1.0,
                                 forgive=(mult > 1.0 and not left) or forgive_excused,
                                 bottles=0 if left else max(0, min(int((counts or {}).get(33, 0)), 200)))
        CLAN_REBOOK.discard((tok, mid))
        try:
            clan.snap_update(c, key, mid, rejoined=False,
                             stint={"parts": raw_parts, "counts": {str(k): v for k, v in raw_counts.items()}, "late": 0 if left else late,
                                    "mult": mult, "forgive": bool(forgive_excused or (mult > 1.0 and not left))})
        except Exception:
            pass
        if r and not left and CLAN_MAP.get(mid):                                       # daemon.py:2061
            r["match"] = match_facts(tok, mid, r, parts, counts)
        ex = []
        if r:
            try:
                CS.on_day(c, key, st, r)                                               # daemon.py:2068
            except Exception:
                ex.append("story")
        drawn = None
        if r:
            try:
                drawn = P.on_day(c, key, st, r, counts, r.get("match"))                # daemon.py:2073
            except Exception:
                ex.append("perks")
        got = ach_book_day(tok, mid, counts, _ach_win if _ach_win in (True, False) else None, left, _ach_late, before, st, r)
        res.append(["day", left, parts, mult, clan.state_text(st), r, [list(x) for x in drawn] if drawn is not None else None, got, ex])

    def boost_start(tok):                                                              # daemon.py:2389
        BOOST_ACTIVE.pop(tok, None)
        bid = BOOST_PICK.pop(tok, 0)
        if not bid:
            return None
        ok = clan.booster_consume(c, key_of(tok), bid)
        if ok:
            BOOST_ACTIVE[tok] = bid
        return [bid, ok]

    def meta(tok):
        return clan.meta_text(c, key_of(tok), BOOST_PICK.get(tok, 0), BOOST_ACTIVE.get(tok, 0))

    def counts_in(d):
        return {int(k): int(v) for k, v in (d or {}).items()}

    def dump_all():
        tabs = [r[0] for r in c.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")]
        res = {}
        for t in tabs:
            cols = [r[1] for r in c.execute('PRAGMA table_info("%s")' % t)]
            rows = sorted(dumps(["x'%s'" % bytes(v).hex() if isinstance(v, (bytes, bytearray)) else v for v in r]) for r in c.execute('SELECT * FROM "%s"' % t))
            res[t] = (cols, rows)
        return res

    import io
    SPENT = {}
    real_out = sys.stdout
    i = -1
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        o = json.loads(line)
        i += 1
        if "t" in o:
            T[0] = o["t"]
        op = o["op"]
        tok = o.get("tok")
        res = {"i": i}
        buf = io.StringIO()
        _t0 = time.perf_counter()
        sys.stdout = buf                                       # clan.py prints a log line when a mission pool rolls
        try:
            if op == "init":
                res["r"] = o["seed"] == seed
            elif op == "ensure":
                clan.ensure(c)
                CS.ensure(c)
                A.ensure(c)
                c.execute("CREATE TABLE IF NOT EXISTS players(token TEXT PRIMARY KEY, steamid INTEGER)")
                res["r"] = True
            elif op == "player":
                PL[tok] = {"key": o["key"], "name": o["name"]}
                c.execute("INSERT OR REPLACE INTO players(token, steamid) VALUES(?,?)", (tok, int(o["key"][2:])))
                res["r"] = True
            elif op == "side":                                                         # clan_side + clan_op 0xE4, daemon.py:1672 / 2600
                key = key_of(tok)
                if o["only_over"] and clan.load(c, key).get("status") == clan.ALIVE:
                    res["r"] = "alive"
                else:
                    st, ok = clan.choose_side(c, key, o["fac"])
                    got = None
                    if ok:
                        try:
                            CS.on_new_run(c, key, o["fac"])
                        except Exception:
                            ok = [ok, "story"]
                        try:
                            got = A.book_side(c, key)
                        except Exception:
                            got = "ach"
                    res["r"] = [clan.state_text(st), ok, got]
            elif op == "begin":                                                        # clan_match_started, daemon.py:2337
                mid = o["mid"]
                CLAN_MAP[mid] = o["map"]
                CLAN_TEAM[mid] = {p["tok"]: p["team"] for p in o["players"]}
                bs = []
                for p in o["players"]:
                    CLAN_MATCH[p["tok"]] = mid
                    CLAN_LAST_MATCH[p["tok"]] = mid
                    if p["mult"] != 1.0:
                        LATE_MULT[(p["tok"], mid)] = p["mult"]
                    if p["late"]:
                        LATE_BONUS[(p["tok"], mid)] = p["late"]
                    bs.append(boost_start(p["tok"]))
                res["r"] = bs
            elif op == "live":                                                         # ach_live_op, daemon.py:1836
                cnt = counts_in(o["counts"])
                mid = CLAN_MATCH.get(tok)
                now = time.time()
                got = []
                try:
                    if mid and (tok, mid) not in ACH_DAY and now - ACH_LIVE_AT.get(tok, 0.0) >= 2.0:
                        ACH_LIVE_AT[tok] = now
                        got = A.book_live(c, key_of(tok), cnt, ACH_DONE.setdefault((tok, mid), {}))
                except Exception:
                    got = None
                res["r"] = [got, ach_read(tok, 0xAB)]
            elif op == "a9":                                                           # ach_report_op, daemon.py:1790
                a9 = counts_in(o["counts"])
                mid = CLAN_MATCH.get(tok) or CLAN_LAST_MATCH.get(tok)
                got = []
                try:
                    if mid:
                        day = ACH_DAY.get((tok, mid))
                        if day is None:
                            old = ACH_PEND.get((tok, mid)) or {}
                            ACH_PEND[(tok, mid)] = {k: max(a9.get(k, 0), old.get(k, 0)) for k in sorted(set(a9) | set(old))}
                        else:
                            got = A.book_match(c, key_of(tok), ach_merge(day["e2"], a9), ACH_DONE.setdefault((tok, mid), {}),
                                               win=day["win"], left=day["left"], late=day["late"], first=False)
                except Exception:
                    got = None
                res["r"] = [got, ach_read(tok, 0xA9)]
            elif op == "report":                                                       # clan_report, daemon.py:2203
                mid = CLAN_MATCH.get(tok)
                fin = []
                if mid and CLAN_DONE.get(tok) != mid:
                    parts = max(0, min(int(o["parts"]), 100000))
                    counts = counts_in(o["counts"])
                    flags = o["flags"]
                    if o.get("quit"):
                        QUIT.add((tok, mid))
                    if o.get("kick"):
                        KICK.add((tok, mid))
                    if o.get("exc"):
                        EXC.add((tok, mid))
                    CLAN_PENDING[tok] = (mid, parts, flags, counts)
                    if mid in CLAN_ENDED or (flags & 2):
                        finalize(tok, fin)
                    else:
                        CLAN_DUE[tok] = time.time() + CLAN_GRACE
                res["r"] = [fin, state_reply(tok, 0xE2)]
            elif op == "tick":                                                         # clan_scheduler, daemon.py:2090
                fin = []
                now = time.time()
                for t2 in [t2 for t2, due in list(CLAN_DUE.items()) if due <= now]:
                    CLAN_DUE.pop(t2, None)
                    fin.append(t2[:6])
                    finalize(t2, fin)
                res["r"] = fin
            elif op == "end":                                                          # record_result + clan_match_ended, daemon.py:8330 / 2368
                mid, winner = o["mid"], o["winner"]
                players = o["players"]
                dep = set(o["departed"])
                if winner in (0, 1, 2):
                    for t2 in players:
                        if t2 not in dep:
                            CLAN_WIN[(t2, mid)] = None if winner == 2 else (CLAN_TEAM[mid][t2] == winner)
                    ros = [(t2, PL[t2]["name"] or "?", CLAN_TEAM[mid][t2], t2 in dep) for t2 in players]
                    I.stats_roster(c, mid, CLAN_MAP.get(mid, ""), ros, winner)
                for p in players:
                    BOOST_ACTIVE.pop(p, None)
                CLAN_ENDED[mid] = time.time()
                fin = []
                for p in players:
                    if CLAN_PENDING.get(p, (None,))[0] == mid:
                        finalize(p, fin)
                for m in [m for m, t2 in CLAN_ENDED.items() if time.time() - t2 > 3600]:
                    CLAN_ENDED.pop(m, None)
                res["r"] = fin
            elif op == "rejoin":                                                       # clan_rejoin_after_booking, daemon.py:6963
                mid = CLAN_MATCH.get(tok)
                r = None
                if mid:
                    key = key_of(tok)
                    if CLAN_DONE.get(tok) != mid:
                        try:
                            clan.snap_update(c, key, mid, rejoined=True)
                        except Exception:
                            pass
                        r = "plain"
                    else:
                        CLAN_DONE.pop(tok, None)
                        CLAN_CREDITED.discard((tok, mid))
                        for s in (EXC, QUIT):
                            s.discard((tok, mid))
                        CLAN_REBOOK.add((tok, mid))
                        try:
                            clan.snap_update(c, key, mid, rejoined=True)
                        except Exception:
                            pass
                        r = "rebook"
                res["r"] = r
            elif op == "pick":                                                         # meta_op, daemon.py:2400
                key = key_of(tok)
                if o["what"] == "booster":
                    bid = o["id"]
                    if bid and clan.booster_counts(c, key).get(bid, 0) <= 0:
                        bid = 0
                    if bid:
                        BOOST_PICK[tok] = bid
                    else:
                        BOOST_PICK.pop(tok, None)
                else:
                    _, ok = clan.mission_choose(c, key, o["id"])
                res["r"] = meta(tok)
            elif op == "read":
                kind = o["kind"]
                lang = lang_of(o.get("lang", 0))
                key = key_of(tok)
                if kind == "E0":
                    res["r"] = state_reply(tok, 0xE0)
                elif kind == "E8":
                    res["r"] = meta(tok)
                elif kind == "AA":
                    res["r"] = ach_read(tok, 0xAA)
                elif kind == "RANK":                                                   # rank_of_token, daemon.py:2495
                    res["r"] = list(I.player_rank(c, tok))
                elif kind == "B4":                                                     # feed_op, daemon.py:2457 / 2437
                    try:
                        feed = CS.live_for_widget(c, key, set(), FEED_LINES, lang=lang)
                    except Exception:
                        feed = []
                    lines = []
                    for mid, text in feed[:FEED_LINES]:
                        tx = str(text).replace("\n", " ").replace("|", "/").encode("utf-8")[:150].decode("utf-8", "ignore")
                        lines.append("%s|%s" % (str(mid).replace("|", "").replace("\n", "")[:23], tx))
                    res["r"] = "n=%d\n" % len(lines) + "\n".join(lines)
                elif kind == "B6":                                                     # clan13_op, daemon.py:2533
                    try:
                        CS.ensure_roster(c, key)
                        text = I.build(c, key, lang, o["section"], o["seen_id"], o["seen_day"], token=tok.lower())
                    except Exception:
                        text = ""
                    raw = text.encode("utf-8")
                    cap = 60000 if o["big"] else 24000
                    if len(raw) > cap:
                        cut = raw.rfind(b"\n", 0, cap)
                        raw = raw[:cut + 1] if cut > 0 else raw[:cap]
                    res["r"] = [len(raw), sha(raw.decode("utf-8", "replace"))] + ([raw.decode("utf-8", "replace")] if full else [])
            elif op == "reset":                                                        # admin_clan_reset, daemon.py:10866
                sid = int(key_of(tok)[2:])
                rr = RS.reset(c, rdir, PL[tok]["name"][:64], sid, [], bool(o["names"]))
                res["r"] = rr
            elif op == "restore":                                                      # admin_clan_restore, daemon.py:10893
                sid = int(key_of(tok)[2:])
                bl = RS.list_backups(rdir, sid or None)
                if bl:
                    res["r"] = [RS.restore(c, rdir, bl[o["idx"] % len(bl)]["file"]), bl[o["idx"] % len(bl)]["file"]]
                else:
                    res["r"] = None
            elif op == "mark":
                d = dump_all()
                res["r"] = {"day": o["day"], "tables": {t: [len(rows), sha("\n".join(rows))] for t, (cols, rows) in d.items()}}
                if full:
                    res["rows"] = {t: rows for t, (cols, rows) in d.items()}
            else:
                raise ValueError("unknown op " + op)
            c.commit()
        except Exception as ex:  # noqa: BLE001
            if os.environ.get("T2R_TRACE"):
                import traceback
                sys.stderr.write("call %d %s: %s" % (i, op, traceback.format_exc()))
            try:
                c.rollback()
            except Exception:
                pass
            res = {"i": i, "err": op}
        sys.stdout = real_out
        if os.environ.get("T2R_GOLD_TIME"):
            e = SPENT.setdefault("%s:%s" % (op, o.get("kind", "")), [0.0, 0])
            e[0] += time.perf_counter() - _t0
            e[1] += 1
        if "err" not in res and buf.getvalue():
            res["p"] = buf.getvalue().splitlines()
        real_out.write(dumps(res) + chr(10))
    sys.stdout = real_out
    for k, (t2, n2) in sorted(SPENT.items(), key=lambda kv: -kv[1][0]):
        sys.stderr.write("time %-12s %9.3f s  %6d calls  %8.2f ms/call" % (k, t2, n2, t2 * 1000 / n2) + chr(10))
    d = dump_all()
    for t, (cols, rows) in d.items():
        print(dumps({"table": t, "cols": cols, "rows": rows}))
    files = []
    if os.path.isdir(rdir):
        for fn in sorted(os.listdir(rdir)):
            with open(os.path.join(rdir, fn), "rb") as f:
                files.append([fn, sha(f.read().decode("utf-8", "replace"))])
    print(dumps({"files": files}))


if __name__ == "__main__":
    if sys.argv[1] == "gen":
        gen(int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]))
    else:
        run()
