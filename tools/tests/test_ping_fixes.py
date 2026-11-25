"""Matchmaking ping fixes (41 high-ping removals in one night): (1) an uneven lobby whose only within-limit host is on the smaller team swaps him
with a bigger-team player instead of taking an over-limit host, (2) unknown pair pings hold the formation up to T2R_PING_WAIT_S and then leave the
unknown players in the queue, (3) a removal teaches the gates (the measured pair, a per-player correction after two), (4) the gates judge at
T2R_PING_MATCH_MS 175 while the removal stays at 190, (5) the 'no players within N ms' search note.
Run: python tools/tests/test_ping_fixes.py   (no sockets are opened, temp DB)."""
import os, sys, tempfile, time, collections

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
FAILS = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: FAILS.append(what)
def tok(name):
    return (name.encode().hex() + "0123456789abcdef" * 8)[:64]
NAMES = ["Noid", "Rino", "Kure", "Wolf", "Jsub", "Eve", "Frank", "Gina"]
T = [tok(n) for n in NAMES]
tmp = tempfile.mkdtemp()
for k in ("T2R_PING_MATCH_MS", "T2R_PING_WAIT_S", "T2R_PING_MEMORY_S", "T2R_PING_CORR_S", "T2R_HOST_SWAP", "T2R_PING_KICK_MS"):
    os.environ.pop(k, None)
os.environ.update(T2R_HOST="127.0.0.1", T2R_PORT="0", T2R_ADMIN_PORT="0", T2R_STATUS_FILE="0", T2R_MATCH_SIZE="8", T2R_ENGINE_SLOTS="8",
                  T2R_MIN_PLAYERS="2", T2R_FILL_RULE="classic", T2R_QUEUE_SETTLE="0", T2R_FILL_WAIT="0", T2R_JIP="1", T2R_MAP_VOTE="0", T2R_P2P="0",
                  T2R_SECOND_MATCH_MIN="6", T2R_DB=os.path.join(tmp, "t2r.db"))
import daemon, hostmig  # noqa: E402
daemon.DB_PATH = os.path.join(tmp, "t2r.db"); daemon.LOG_PATH = os.path.join(tmp, "daemon.log")
daemon.evt = lambda *a, **k: None
daemon.get_name = lambda t: NAMES[T.index(t)] if t in T else None
LOGS = []
daemon.log = lambda *a, **k: LOGS.append(" ".join(str(x) for x in a))
MM = daemon.MM
NEW = hasattr(daemon, "ping_match_ms")      # (False on the line before the fixes: the new checks then fail)

def setp(a, b, ms, age=1.0):
    for k in ((a, b), (b, a)):
        daemon.PING[k] = collections.deque([ms], maxlen=5); daemon.PING_AT[k] = time.time() - age
def clrp():
    daemon.PING.clear(); daemon.PING_AT.clear()
    for n in ("MEASURED_PAIR", "PING_EXCESS", "PING_CORR", "PING_FORMED", "PING_WAIT_SINCE", "PING_NOTE_AT", "PING_OUT"):
        if hasattr(daemon, n): getattr(daemon, n).clear()
def reset():
    daemon.MM.book.__init__(daemon.MATCH_SIZE) if hasattr(daemon.MM, 'book') else None
    for a in ("queue", "seen", "matches", "formed", "settle", "qtime", "departed", "clock", "polled", "joining", "departed_t", "lives", "left", "ended_at", "started_t"):
        getattr(MM, a).clear()
    MM.launched.clear(); MM.ended.clear(); MM.started.clear(); MM.last_arrival = 0.0; MM._hold_since = 0.0
    MM.rq.t.clear(); MM.rq.first.clear(); MM.rq.next.clear(); daemon.RQ_CAP.clear()
    MM._rq_logged = 0.0; MM._lobby_fill_logged = 0.0
    daemon.PARTIES.of.clear(); daemon.PARTIES.leaders.clear()
    for t in T: daemon.PRES.touch(t, "x")
    LOGS.clear(); daemon.MODEQ.pref.clear()
    clrp()

check(daemon.PING_KICK_MS == 190 and daemon.PING_KICK_S == 60.0, "the removal stays at 190 ms / 60 s")
if NEW:
    check(daemon.PING_MATCH_MS == 175 and daemon.ping_match_ms() == 175, "the match limit defaults to 175 ms")
    check(daemon.PING_WAIT_S == 10.0 and daemon.PING_MEMORY_S == 21600.0 and daemon.PING_CORR_S == 43200.0, "wait 10 s, memory 6 h, correction 12 h")
else:
    check(False, "the new ping switches exist")

# ---------------------------------------------------------------- (1) the 5346a92f shape: 3 v 2, the only within-limit host on the smaller team
def forced(big, small):
    daemon.split_teams = lambda players, party_of=None, skill=None, top=None, rules=None: (list(big), list(small))
orig_split = daemon.split_teams
def shape(skills=None):
    """S = the within-limit host on the smaller team; A, B, C = the bigger team (every one of them over 175 ms to somebody); D = S's team mate."""
    S, A, B, C, D = T[:5]
    clrp()
    for a, b, ms in ((S, A, 100), (S, B, 120), (S, C, 150), (S, D, 170), (A, B, 200), (A, C, 228), (A, D, 100), (B, C, 190), (B, D, 180), (C, D, 100)):
        setp(a, b, ms)
    sk = skills or {S: 0.50, A: 0.62, B: 0.51, C: 0.30, D: 0.45}
    daemon.player_skill = lambda tokens: {t: sk.get(t, 0.5) for t in tokens}
    daemon.top_players = lambda tokens: set()
    daemon.rule_groups = lambda tokens: []
    forced([A, B, C], [S, D])
    return S, A, B, C, D
def worst_to(h, pl):
    return max(daemon.pair_ping(h, o) for o in pl if o != h)
daemon.PARTIES.of.clear()
S, A, B, C, D = shape()
LOGS.clear()
out = daemon.team_order([A, B, C, S, D])
big = out[0::2]; small = out[1::2]
check(len(big) == 3 and S in big and out[0] == S, "5346a92f: the host (within the limit) hosts on the bigger team (slot 0 = %s, even slots %s)" % (daemon.tag(out[0]), [daemon.tag(t) for t in big]))
check(worst_to(out[0], out) <= 175, "nobody is over the match limit to the host (worst %s ms)" % worst_to(out[0], out))
check(B in small, "swapped with the bigger-team player closest in skill (B .51 v S .50): small team %s" % [daemon.tag(t) for t in small])
check(any("HOST-PICK" in l and "swapped with" in l for l in LOGS), "the decision is logged as a HOST-PICK line")
# a party member on the bigger team is never the swap partner; the next closest one is
S, A, B, C, D = shape()
daemon.PARTIES.of.clear()
class _P:   # a party of B + C (one pid): swapping B out would split it
    pass
orig_pid = daemon.PARTIES.pid
daemon.PARTIES.pid = lambda t: "p1" if t in (B, C) else None
out = daemon.team_order([A, B, C, S, D])
check(out[0] == S and A in out[1::2] and B in out[0::2] and C in out[0::2], "a party (B + C) stays together: A is the swap partner (small %s)" % [daemon.tag(t) for t in out[1::2]])
# skill gap too big for every candidate: no swap, the old rule (a bigger-team host)
daemon.PARTIES.pid = orig_pid
S, A, B, C, D = shape({S: 0.05, A: 0.9, B: 0.95, C: 0.85, D: 0.5})
LOGS.clear()
out = daemon.team_order([A, B, C, S, D])
# no swap fits, but moving C to the host's team keeps the host within the limit and the balance better (2.15 -> 0.45): moved
check(out[0] == S and any("moved" in l for l in LOGS), "no bigger-team player close enough in skill: one moves to the host's team (%s)" % daemon.tag(out[0]))
# a bigger-team host within the limit exists: no swap (host_from_team takes him)
S, A, B, C, D = shape()
setp(A, C, 150); setp(A, B, 150); setp(A, D, 100); setp(A, S, 100)
out = daemon.team_order([A, B, C, S, D])
check(out[0] == A and S in out[1::2], "a bigger-team host within the limit exists: the old pick stays (%s hosts, teams untouched)" % daemon.tag(out[0]))
daemon.split_teams = orig_split

# ---------------------------------------------------------------- (4) the margin
reset()
now = time.time()
lim = daemon.ping_match_ms() if NEW else 190
setp(T[0], T[1], 180)
kept, dropped, gh, info = hostmig.ping_fit([T[0], T[1]], lambda a, b: daemon._ping_gate(a, b, now), lim)
check(NEW and dropped and len(kept) == 1, "margin: a 180 ms pair is not matched (gate at %d ms)" % lim)
setp(T[0], T[1], 170)
kept, dropped, gh, info = hostmig.ping_fit([T[0], T[1]], lambda a, b: daemon._ping_gate(a, b, now), lim)
check(NEW and not dropped, "margin: 170 ms is matched")
setp(T[0], T[1], 188)
check(NEW and daemon.jip_ping_block(T[1], "mx", T[0], [T[0]], set(), now) == 188, "join in progress: 188 ms is blocked at the match limit")
setp(T[0], T[1], 170)
check(daemon.jip_ping_block(T[1], "mx", T[0], [T[0]], set(), now) is None, "join in progress: 170 ms joins")
# host pick: a candidate within 175 wins over one at 180-189
reset()
setp(T[0], T[1], 100); setp(T[0], T[2], 182); setp(T[1], T[2], 100)    # T[2] worst 182 as host? no: host T[0] worst 182, T[1] worst 100, T[2] worst 182
bo = daemon.best_host_order([T[0], T[1], T[2]])
check(bo[0] == T[1], "host pick: the candidate within the match limit hosts (%s)" % daemon.tag(bo[0]))
check(daemon.PING_KICK_MS == 190, "the removal limit (PING_KICK_MS) is still 190")

# ---------------------------------------------------------------- (3) learn from the removals
reset()
now = time.time()
H, P1, P2 = T[0], T[1], T[2]
setp(P1, H, 175)
kept, dropped, _, _ = hostmig.ping_fit([H, P1], lambda a, b: daemon._ping_gate(a, b, now), lim)
check(not dropped, "estimate 175 = within the limit: the pair matches before any removal")
if NEW:
    daemon.PING_FORMED["m1"] = {frozenset((P1, H)): 175}
    daemon.ping_kick_learn("m1", P1, H, 397, now)
    check(daemon._ping_gate(P1, H, now) == 397 and daemon._ping_gate(H, P1, now) == 397, "the measured 397 ms is remembered for the pair, both orders")
    kept, dropped, _, _ = hostmig.ping_fit([H, P1], lambda a, b: daemon._ping_gate(a, b, now), lim)
    check(P1 in dropped or H in dropped, "a re-match of the same pair (estimate 175, measured 397) is refused")
    check(daemon.pair_ping(P1, H) == 397 and daemon.pair_ping(P1, H, raw=True) == 175, "pair_ping reads max(estimate, measured); raw stays the estimate (the in-match timer)")
    check(daemon.jip_ping_block(P1, "m9", H, [H], set(), now) == 397, "join in progress: the remembered pair is blocked")
    check(daemon._ping_fresh(P1, H, now) == 175, "the in-match removal timer still reads the raw estimate")
    check(P1 not in daemon.PING_CORR, "one removal is no correction yet")
    # the memory runs out
    daemon.MEASURED_PAIR[(P1, H)] = daemon.MEASURED_PAIR[(H, P1)] = (397, now - daemon.PING_MEMORY_S - 1)
    check(daemon._ping_gate(P1, H, now) == 175, "after T2R_PING_MEMORY_S the estimate is read again")
    # a second under-called removal (other host, measured 306 against an estimate of 175): the median excess is added to P1's other pairs
    setp(P1, P2, 60)
    daemon.PING_FORMED["m2"] = {frozenset((P1, P2)): 175}
    daemon.ping_kick_learn("m2", P1, P2, 306, now)
    check(P1 in daemon.PING_CORR, "two removals with 40+ ms excess: a correction for P1")
    c = daemon.PING_CORR[P1][0]
    check(c == int(((397 - 175) + (306 - 175)) / 2.0), "the correction is the median excess (%s ms)" % c)
    setp(P1, T[3], 60)
    check(daemon._ping_gate(P1, T[3], now) == 60 + c, "P1's estimated pair pings carry the correction (%s)" % daemon._ping_gate(P1, T[3], now))
    check(daemon._ping_gate(T[4], T[3], now) is None, "other players are not corrected")
    setp(T[4], T[3], 60)
    check(daemon._ping_gate(T[4], T[3], now) == 60, "another pair of other players keeps its estimate")
    daemon.PING_CORR[P1] = (c, now - 1)
    check(daemon._ping_gate(P1, T[3], now) == 60, "the correction runs out after T2R_PING_CORR_S")
    # an excess under 40 ms is not recorded
    daemon.PING_EXCESS.clear(); daemon.PING_CORR.clear()
    daemon.PING_FORMED["m3"] = {frozenset((P2, T[3])): 150}
    daemon.ping_kick_learn("m3", P2, T[3], 185, now); daemon.ping_kick_learn("m3", P2, T[3], 186, now)
    check(P2 not in daemon.PING_CORR and not daemon.PING_EXCESS.get(P2), "an excess of 35 ms is no under-call")
    # switches
    mem = daemon.PING_MEMORY_S
    daemon.PING_MEMORY_S = 0.0; daemon.MEASURED_PAIR.clear()
    daemon.ping_kick_learn("m1", P1, H, 397, now)
    check(not daemon.MEASURED_PAIR, "T2R_PING_MEMORY_S=0: nothing remembered")
    daemon.PING_MEMORY_S = mem
else:
    check(False, "learning from removals exists")

# ---------------------------------------------------------------- (2) unknown pings: hold, then leave the unknown ones out
reset()
daemon.has_helper = lambda t: t in T
S1, S2, S3 = T[5], T[6], T[7]
setp(S1, S2, 40)                                   # S3 has no ping to either
def search(t):
    """A search that does not form at once (the test calls try_form itself): queued and polling fresh."""
    MM.queue[t] = time.time(); MM.seen[t] = time.time(); MM.last_arrival = time.time()
r = [search(S1), search(S2), search(S3)]
res = MM.try_form()
check(res == (None, None) and NEW and any("PING-WAIT" in l for l in LOGS), "pings unknown for S3: the formation is held (no blind match; first pass)")
if NEW:
    check(all(t in MM.queue for t in (S1, S2, S3)), "everybody is still searching during the hold")
    for t in (S3,):
        daemon.PING_WAIT_SINCE[t] -= daemon.PING_WAIT_S + 1
    for t in (S1, S2): daemon.PING_WAIT_SINCE[t] -= daemon.PING_WAIT_S + 1
    for t in T[5:8]: MM.seen[t] = time.time()
    mid, pl = MM.try_form()
    check(mid is not None and sorted(pl) == sorted([S1, S2]), "after the wait: S1 + S2 form, S3 (ping unknown) is left out (%s)" % (pl and [daemon.tag(t) for t in pl]))
    check(S3 in MM.queue, "S3 stays searching")
    # the pings arrive in time: formed together
    reset()
    setp(S1, S2, 40)
    for t in (S1, S2, S3): search(t)
    check(MM.try_form() == (None, None), "held again")
    setp(S1, S3, 60); setp(S2, S3, 90)
    for t in T[5:8]: MM.seen[t] = time.time()
    mid, pl = MM.try_form()
    check(mid is not None and sorted(pl) == sorted([S1, S2, S3]), "pings known inside the wait: all three form")
    # switch off: the old behaviour (unknown = not judged)
    reset()
    ws = daemon.PING_WAIT_S; daemon.PING_WAIT_S = 0.0
    setp(S1, S2, 40)
    for t in (S1, S2, S3): search(t)
    mid, pl = MM.try_form()
    check(mid is not None and len(pl) == 3, "T2R_PING_WAIT_S=0: formed with the unknown ping, the old behaviour")
    daemon.PING_WAIT_S = ws
    # players without a Steam helper have no ping at all: never held, never left out for it
    reset()
    daemon.has_helper = lambda t: False
    for t in (S1, S2): search(t)
    mid, pl = MM.try_form()
    check(mid is not None and len(pl) == 2, "players without a Steam helper form as before (no ping can exist)")
    daemon.has_helper = lambda t: t in T
    # a 2-player lobby with an unknown pair after the wait is not formed blind
    reset()
    for t in (S1, S2): search(t)
    check(MM.try_form() == (None, None), "2 players, ping unknown: held")
    for t in (S1, S2): daemon.PING_WAIT_SINCE[t] -= daemon.PING_WAIT_S + 1
    for t in (S1, S2): MM.seen[t] = time.time()
    res = MM.try_form()
    check(res == (None, None) and S1 in MM.queue and S2 in MM.queue, "2 players, ping still unknown after the wait: left out, both keep searching")
else:
    check(False, "unknown-ping hold exists")

# ---------------------------------------------------------------- (5) the search note
if NEW:
    sent = []
    note = lambda t, n: sent.append((t, n))
    clrp()
    t0 = 5000.0
    for dt in (0, 20, 40, 59):
        daemon.ping_left_out_note([T[0]], t0 + dt, note)
    check(not sent, "no note before 60 s of being left out")
    daemon.ping_left_out_note([T[0]], t0 + 61, note)
    check(sent == [(T[0], 190)], "the note comes after 60 s with the kick limit number (%s)" % sent)
    for dt in (80, 100, 120, 150, 170):
        daemon.ping_left_out_note([T[0]], t0 + dt, note)
    check(len(sent) == 1, "at most once per 2 minutes")
    daemon.ping_left_out_note([T[0]], t0 + 190, note)
    check(len(sent) == 2, "again after 2 minutes")
    daemon.ping_left_out_note([], t0 + 200, note)
    daemon.ping_left_out_note([T[0]], t0 + 210, note); daemon.ping_left_out_note([T[0]], t0 + 300, note)
    check(len(sent) == 2, "a searcher who got in and is left out again starts counting afresh (the 2 min gap too)")
    daemon.SOCIAL.note(T[1], "pingfar", "190")
    for lang in (0, 1):
        got = daemon.SOCIAL.notes_for(T[1], lang)
        txt = got[0][3] if got else ""
        daemon.SOCIAL.note(T[1], "pingfar", "190")
        check("190" in txt and len(txt.encode("utf-8")) <= 0xf0, "%s note fits the 0x11 frame: %r (%d B)" % (("en", "pl")[lang], txt, len(txt.encode("utf-8"))))
    check(daemon.SOCIAL.notes_for(T[2], 0) == [], "(no note for a player without one)")
else:
    check(False, "the search note exists")

print("\nFAILED: %d" % len(FAILS) if FAILS else "\nall ok")
sys.exit(1 if FAILS else 0)
