"""Host migration (daemon side). See re/host-migration.md for the whole flow.

winmm's gameplay session is a star: every client talks only to the match host (slot 0 at
start), the host runs the match authority (timer, team lives, respawn grants, match end op
0x12) and forwards everybody's state. When the host goes away the clients hit winmm's 5 s
"Connection to host lost" timeout and the match is over for everyone.

With migration on (daemon env T2R_HOST_MIGRATION=1) each client's t2r-ext asks the server
what to do as soon as the host goes quiet (op 0xE6 on its own short TCP connection, like the
clan ops) and holds winmm's timeout meanwhile. The server decides the host is gone, picks a
new one among the players still asking, and every ext then rewires its winmm session to the
new host (the new host flips itself to the host role and keeps its own replicated copy of the
match timer and lives).

  ext -> server  0xE6 [64B token][u8 flags][u8 epoch]   flags bit0 = the host is silent for us
  server -> ext  0xE7 [u8 enabled][u8 host slot][u8 epoch][u8 alive mask][u8 state]
                 enabled 0 = migration off on this server (the ext stays out of the way)
                 epoch   = number of migrations so far in this match; an ext whose own epoch is
                           lower switches to 'host slot'; alive mask = slots still in the match
                 state   0 normal, 1 host loss suspected (keep holding), 2 give up (no match
                           known / nobody left to host: let winmm end the match),
                           3 planned handover being prepared (HostRepick, see below)
  request flags: bit0 host silent, bit1 'this ext can do a planned handover', bit2 'prepared' (saw state 3)

Planned handover (T2R_HOST_REPICK=2): the daemon picks a better host while the old one is fine. Phase 1 (state 3): every
player is told, polls every 0.5 s and answers with bit2. Phase 2 (all alive players prepared, or abort after HANDOVER_TIMEOUT):
epoch += 1, host = the chosen slot, the old host stays in the alive mask - an ext that sees a newer epoch whose OLD host is
still alive treats it as a handover: the old host steps down to a client, the new host also opens a peer for the old host.

This module is pure logic (no sockets) so tools/tests/test_hostmig.py can drive it with a
fake clock.
"""
import os, threading, time

OP_POLL, OP_REPLY = 0xE6, 0xE7
ST_NORMAL, ST_SUSPECT, ST_GIVE_UP, ST_HANDOVER = 0, 1, 2, 3   # 3 = a planned handover is being prepared (poll fast, flag bit2 when ready)


def placeholder(slot):
    """A 64-char token-shaped stand-in for a slot whose player came back in ANOTHER slot (the crashed old host: slot 0 means
    'host' to the client, so it can never return there). Never a real token; the daemon lists it as departed."""
    return ("f0" * 30) + ("%04x" % slot)


# ------------------------------------------------------------ machine performance (host pick + migration)
# Each ext reports its recent frame stalls (op 0xCA, see daemon.py "PERFORMANCE REPORT"). A player with big stalls
# in the last 5 minutes gets PENALTY_MS added to its worst ping when the host is chosen (twice that when severe), so
# it loses host to any candidate within that many ms of it. No (fresh) report = unknown = no penalty.
def _envi(name, default):
    try:
        return int(os.environ.get(name, default))
    except ValueError:
        return default

PERF_ON = os.environ.get("T2R_HOSTPERF", "1") != "0"
PERF_PENALTY_MS = _envi("T2R_HOSTPERF_PENALTY_MS", 40)   # added to the worst ping of a stuttering player
PERF_N1000 = _envi("T2R_HOSTPERF_N1000", 2)              # >= this many stalls of >= 1 s in the window = stuttering
PERF_N250 = _envi("T2R_HOSTPERF_N250", 6)                # ... or this many stalls of >= 250 ms
PERF_SEVERE_X = _envi("T2R_HOSTPERF_SEVERE_X", 2)        # >= this x the thresholds = severe: penalty doubled
PERF_FRESH_S = _envi("T2R_HOSTPERF_FRESH_S", 360)        # a report older than this says nothing (the client's own window is 300 s)
OP_PERF, OP_PERF_REPLY = 0xCA, 0xCB


def parse_perf(pl):
    """[64B token][u8 ver=1][u8 n250][u8 n1000][u16 worst ms][u16 avg frame ms*10][u8 flags] -> (token, dict) or None."""
    if len(pl) < 72 or pl[64] != 1:
        return None
    return pl[:64].decode("latin1", "ignore"), {
        "n250": pl[65], "n1000": pl[66], "worst": int.from_bytes(pl[67:69], "little"),
        "avg_ms": int.from_bytes(pl[69:71], "little") / 10.0, "vram": pl[71] & 1}


def perf_penalty(rep):
    """Penalty ms (0, PERF_PENALTY_MS or twice that) for one report dict; 0 for None (unknown)."""
    if not PERF_ON or not rep:
        return 0
    n1, n2 = rep["n1000"], rep["n250"]
    if n1 >= PERF_N1000 * PERF_SEVERE_X or n2 >= PERF_N250 * PERF_SEVERE_X:
        return PERF_PENALTY_MS * 2
    if n1 >= PERF_N1000 or n2 >= PERF_N250:
        return PERF_PENALTY_MS
    return 0


class PerfScores:
    """Latest performance report per token (and per Steam account, for a token that has not reported yet)."""

    def __init__(self, now=time.time):
        self.now = now
        self.by_token = {}
        self.by_steam = {}
        self.lock = threading.Lock()

    def report(self, token, rep, steamid=0):
        rep = dict(rep, t=self.now())
        with self.lock:
            self.by_token[token] = rep
            if steamid:
                self.by_steam[steamid] = rep

    def prune(self):
        """Drop reports older than PERF_FRESH_S (get() ignores them anyway): one entry per token / account ever seen otherwise. Returns how many went."""
        now = self.now()
        n = 0
        with self.lock:
            for d in (self.by_token, self.by_steam):
                for k in [k for k, r in d.items() if now - r["t"] > PERF_FRESH_S]:
                    del d[k]; n += 1
        return n

    def get(self, token, steamid=0):
        with self.lock:
            rep = self.by_token.get(token) or (self.by_steam.get(steamid) if steamid else None)
        if rep and self.now() - rep["t"] <= PERF_FRESH_S:
            return rep
        return None

    def penalty(self, token, steamid=0):
        return perf_penalty(self.get(token, steamid))


def host_key(pings, penalty=0):
    """Sort key of a host candidate: (worst ping + performance penalty, total ping)."""
    return (max(pings) + penalty, sum(pings))


# ------------------------------------------------------------ region (host comes from the majority region / the pinned player's region)
# The Steam ping location string a helper reports (0xF4, SteamNetworkingUtils::ConvertPingLocationToString) lists the
# Steam relay data centres near it as "<pop>=<ping ms>[+<via ms>]" entries, e.g. "fra=23+2,ams=27+3,lhr=35": the
# player's region is the region of its lowest-ping known data centre. (Format from Valve's documentation; no live
# location string has been logged yet - the helper's reports are only stored. Unknown codes are skipped.)
import re
REGION_OF_POP = {}
for _r, _pops in (("EU", "ams fra lhr par sto sto2 vie waw mad hel lux mil ber prg arn cdg man dub osl cph sof buc lis"),
                  ("NA-east", "iad atl ord mia ewr dfw yto tor bos cmh den hou dtw"),
                  ("NA-west", "sea lax sjc pdx phx slc van yvr sfo"),
                  ("other", "sgp hkg tyo seo bom maa del dxb jnb gru scl lim eze syd mel bog mex bkk kul tpe osa fuk cpt lax2")):
    for _p in _pops.split():
        REGION_OF_POP[_p] = _r
REGION_FALLBACK_UNKNOWN_FRAC = 0.5   # more than this share of the players with no known region: no region rule


def region_of(loc):
    """'EU' / 'NA-east' / 'NA-west' / 'other', or None when the string has no known data centre."""
    if not loc:
        return None
    best = None
    for pop, ms in re.findall(r"([a-z][a-z0-9]{2,4})=(\d+)", loc.lower()):
        r = REGION_OF_POP.get(pop)
        if r and (best is None or int(ms) < best[0]):
            best = (int(ms), r)
    return best[1] if best else None


MM_REGIONS = ("EU", "NA-east", "NA-west")   # 'other' lumps South America + Asia + Oceania together: never a "same region" for matchmaking


def region_first(tokens, region_fn, members_fn=None, waited_fn=None, max_wait=None):
    """Matchmaking order with same-region priority: the queue-ordered `tokens`, players of the lead region first, queue order
    kept inside each group (stable). Lead region = the known region (EU / NA-east / NA-west) with the most searchers; a tie goes to the region
    of the earliest searcher. A party counts as the region of its first member that has one (members_fn(t) -> the party's tokens). A player
    that has waited max_wait s or more keeps its queue place whatever its region (waited_fn(t) -> seconds), so nobody waits forever.
    Returns (ordered list, lead region); lead None = no known region, list unchanged."""
    tokens = list(tokens)
    reg = {}
    for t in tokens:
        if t in reg:
            continue
        group = list(members_fn(t)) if members_fn else [t]
        if t not in group:
            group = [t] + group
        r = None
        for m in group:
            mr = region_fn(m)
            if mr in MM_REGIONS:
                r = mr
                break
        for m in group:
            reg[m] = r
    cnt = {}
    for t in tokens:
        if reg[t]:
            cnt[reg[t]] = cnt.get(reg[t], 0) + 1
    if not cnt:
        return tokens, None
    top = max(cnt.values())
    lead = next(reg[t] for t in tokens if reg[t] and cnt[reg[t]] == top)

    def first(t):
        if reg[t] == lead:
            return 0
        if waited_fn is not None and max_wait is not None and waited_fn(t) >= max_wait:
            return 0
        return 1
    return sorted(tokens, key=first), lead


def parse_pins(text):
    """'76561198000000042:EU,1234:NA-east' -> {steamid: region}"""
    out = {}
    for part in (text or "").split(","):
        if ":" in part:
            a, r = part.split(":", 1)
            try:
                out[int(a.strip())] = r.strip()
            except ValueError:
                pass
    return out


MIN_SILENT_S = float(os.environ.get("T2R_HM_MIN_SILENT_S", "5"))   # silence reports must span this long before a still-connected host is replaced
REPLACED_EXCUSE_S = float(os.environ.get("T2R_HM_REPLACED_EXCUSE_S", "10"))   # a leave this soon after being replaced as host (or told so) is no deliberate leave

PINS = parse_pins(os.environ.get("T2R_HOST_REGION_PIN", ""))     # steamid -> region the host must come from while that player is in the match


REGION_GUARD_MS = _envi("T2R_REGION_GUARD_MS", 60)   # the region rule may cost the lobby at most this many ms of worst-case ping (0 = no guard)


def ping_fit(players, pair, limit, units_fn=None, host_ok=None):
    """Ping gate for a match being formed .
    Match 72784a06 formed two South American players with one in Poland at 254 ms to the host; she was removed 60 s into the match.
    Every player of a match plays through the host, so a lobby fits when ONE host candidate has every other player at or under `limit` ms.
    players: the formation's pick (queue order). pair(a, b) -> ms or None (unknown = not judged, kept). units_fn(t) -> the tokens that join
    together with t (a party: dropped whole). host_ok(t) -> may t host (no-host list).
    Returns (kept, dropped, host, info): the host candidate that keeps the most players (tie: lower worst ping to the kept players, then
    queue order) and the players it keeps (queue order). No pair ping known, or limit <= 0 = everybody kept, host None.
    A far player (see below) is dropped even by a host whose ping to it is unknown, and a far host keeps nobody it has no ping to."""
    players = list(players)
    if limit <= 0 or len(players) < 2:
        return players, [], None, "off" if limit <= 0 else "one player"
    unit = {}
    for t in players:
        if t in unit:
            continue
        g = [m for m in (units_fn(t) if units_fn else [t]) if m in players]
        if t not in g:
            g = [t] + g
        for m in g:
            unit[m] = tuple(g)
    if not any(pair(a, b) is not None for i, a in enumerate(players) for b in players[i + 1:]):
        return players, [], None, "no pings known"
    # Far players: every known ping to the others is over the limit (and at least half of them, min 2, are known). An unknown ping is not
    # a pass for them: a Southeast Asia player at 381-414 ms to all four South Americans whose pings were known was matched because two
    # players who had just queued had no ping to him yet, so one of them "kept everybody"; he was removed 60 s in, every match.
    far = set()
    for t in players:
        others = [o for o in players if o not in unit[t]]
        known = [ms for ms in (pair(t, o) for o in others) if ms is not None]
        if others and len(known) >= max(2, (len(others) + 1) // 2) and min(known) > limit:
            far.update(unit[t])
    best = None
    for h in players:
        if host_ok is not None and not host_ok(h):
            continue
        bad = set(far) - set(unit[h])
        for t in players:
            if t == h:
                continue
            ms = pair(h, t)
            if (ms is not None and ms > limit) or (ms is None and h in far and t not in unit[h]):
                bad.update(unit[t])
        if h in bad:                              # a member of the host's own party is over the limit to it: not a host for this lobby
            continue
        kept = [t for t in players if t not in bad]
        known = [pair(h, t) for t in kept if t != h and pair(h, t) is not None]
        key = (-len(kept), max(known) if known else 0, players.index(h))
        if best is None or key < best[0]:
            best = (key, h, kept)
    if best is None:
        return players, [], None, "no host candidate"
    _, h, kept = best
    dropped = [t for t in players if t not in kept]
    return kept, dropped, h, "host %s keeps %d of %d" % (str(h)[:6], len(kept), len(players))


def region_pool(players, region, steamid, pair, pins=None, everyone=None):
    """(candidate subset, info text). region(token)/steamid(token)/pair(a, b) are lookups. A pinned player in the match
    forces its region; else the majority region (tie: lower combined ping inside the region; a tie that ping cannot break =
    no region rule); else (too many unknown / nobody of that region) everybody. `everyone` = every player of the match when
    `players` is only the allowed candidates (no-host players are left out of the candidates but they count for the majority
    and for the worst-case ping). A 1 EU / 1 NA tie was broken ALPHABETICALLY ('EU' < 'NA-east')
    and the region then overrode the pings: an EU host with 205 / 272 ms to the NA players although the NA player had 83 / 205.
    The region rule now never costs more than REGION_GUARD_MS of worst-case ping (the pin stays absolute: owner decision)."""
    pins = PINS if pins is None else pins
    everyone = list(everyone) if everyone else list(players)
    for t in players:
        if t not in everyone:
            everyone.append(t)
    regs = {t: region(t) for t in everyone}
    info = " ".join("%s=%s" % (t[:6], regs[t] or "?") for t in everyone)
    known = [t for t in everyone if regs[t]]
    pinned = next((pins[steamid(t)] for t in everyone if steamid(t) in pins), None)
    if pinned:
        pool = [t for t in players if regs[t] == pinned]
        if pool:
            return pool, "region pin %s (%d of %d) | %s" % (pinned, len(pool), len(players), info)
        return list(players), "region pin %s but nobody there, all | %s" % (pinned, info)
    if not known or (len(everyone) - len(known)) > REGION_FALLBACK_UNKNOWN_FRAC * len(everyone):
        return list(players), "regions unknown for too many, all | %s" % info
    counts = {}
    for t in known:
        counts[regs[t]] = counts.get(regs[t], 0) + 1
    def combined(r):
        m = [t for t in known if regs[t] == r]
        tot = 0
        for a in m:
            for b in m:
                if a < b:
                    tot += pair(a, b) if pair(a, b) is not None else 1000
        return tot
    top = max(counts.values())
    tied = [r for r in counts if counts[r] == top]
    comb = {r: combined(r) for r in tied}
    cnt_txt = ",".join("%s %d" % kv for kv in sorted(counts.items()))
    if len(tied) > 1 and len(set(comb.values())) == 1:
        return list(players), "tied regions (%s), ping cannot break the tie: no region rule | %s" % (cnt_txt, info)
    win = min(tied, key=lambda r: (comb[r], r))
    pool = [t for t in players if regs[t] == win]
    if not pool:
        return list(players), "majority %s has no candidate (%s), all | %s" % (win, cnt_txt, info)
    if REGION_GUARD_MS > 0 and len(pool) < len(players):
        def worst(h):
            p = [pair(h, o) for o in everyone if o != h]
            return None if (not p or None in p) else max(p)
        wp, wa = [worst(h) for h in pool], [worst(h) for h in players]
        if None not in wp and None not in wa and min(wp) > min(wa) + REGION_GUARD_MS:
            return list(players), "majority %s (%s) would cost %d ms of worst-case ping (best %d ms outside it, guard %d): no region rule | %s" % (
                win, cnt_txt, min(wp) - min(wa), min(wa), REGION_GUARD_MS, info)
    return pool, "majority %s (%s) | %s" % (win, cnt_txt, info)


class HostMigration:
    """Per-match record: players (slot = index), current host slot, epoch, gone slots, and the
    last poll / silence report time per slot."""

    def __init__(self, enabled, now=time.time, log=print, ping=None, perf=None, pool=None,
                 alive_window=3.0, silent_window=2.5, lone_wait=6.0, min_left=1, settle=3.0, bridge_grace=4.0, unknown_keep_going=True, collect=3.0,
                 min_silent=None, link_alive=None):
        self.enabled = enabled
        self.now = now
        self.log = log
        self.ping = ping                # ping(token_a, token_b) -> ms or None (Steam estimates)
        self.pool = pool                # pool(tokens) -> (region-filtered tokens, info text)
        self.perf = perf                # perf(token) -> penalty ms (stalls of that machine) or 0
        self.alive_window = alive_window  # a poll this recent = the player is still running
        self.silent_window = silent_window  # a silence report this recent counts
        self.lone_wait = lone_wait      # some (not all) report silence this long and the host's
                                        # ext went quiet too -> host lost
        self.min_left = min_left        # fewer players than this left -> give up
        self.settle = settle            # silence reports ignored this long after a migration
        self.bridge_grace = bridge_grace  # a helper link down this long (and not back) counts as 'host helper gone' (a link flap alone never migrates)
        self.collect = collect          # a lost host's successor is picked this long after the loss (every survivor that asks in time is a candidate), or when all asked
        self.unknown_keep_going = unknown_keep_going  # a poll for a token with no record (daemon restarted mid-match) = 'keep going', never 'give up'
        # The host had ONE 2.09 s frame stall; both clients' ext report silence after 1.5 s (HM_SILENT), 'all players report
        # silence' fired 1 s later and the host was replaced at once - its ext then heard 'give up' and left the match. A host whose link to the
        # server is still up (session socket, Steam helper) is only replaced on silence reports once the silence has lasted min_silent s (the
        # clients hold winmm's timeout for 15 s, HM_HOLD_MS). A host that is confirmed gone (left, session socket lost, helper gone) is not waited for.
        self.min_silent = MIN_SILENT_S if min_silent is None else min_silent
        self.link_alive = link_alive    # link_alive(token) -> False when the host's session socket is gone (None = assume up)
        self.lock = threading.Lock()
        self.matches = {}               # mid -> record
        self.by_token = {}              # token -> mid

    # ------------------------------------------------------------ match lifecycle
    def start(self, mid, players):
        """The match went live (GAME-START): slot i = players[i], host = slot 0."""
        with self.lock:
            self._drop(mid)
            self.matches[mid] = {"players": list(players), "host": 0, "epoch": 0, "gone": set(),
                                 "seen": {}, "silent": {}, "silent_since": None, "lost": None,
                                 "bridge_gone": set(), "bridge_at": {}, "given_up": False, "t0": self.now()}
            for t in players:
                self.by_token[t] = mid

    def add_player(self, mid, token, slot):
        """Join in progress: `token` now holds `slot` of the running match (a new list index, or the index of a player who left:
        that slot is no longer 'gone'). Slots stay list indexes."""
        with self.lock:
            m = self.matches.get(mid)
            if not m:
                return
            pl = m["players"]
            if token in pl and pl.index(token) != slot:
                old = pl.index(token)              # the old host coming back as a client: its old index becomes a gone placeholder
                pl[old] = placeholder(old)
                m["gone"].add(old)
                m["seen"].pop(old, None); m["silent"].pop(old, None)
            while slot > len(pl):                  # a party joined one team with a hole below it: that index is gone
                m["gone"].add(len(pl)); pl.append(placeholder(len(pl)))
            if slot < len(pl):
                self.by_token.pop(pl[slot], None)
                pl[slot] = token
            else:
                pl.append(token)
            m["gone"].discard(slot)
            m["seen"].pop(slot, None); m["silent"].pop(slot, None)
            m.setdefault("joined", {})[slot] = self.now()     # its ext only polls once it is in the match (substate 6): loading until then
            self.by_token[token] = mid
            self.log("HOSTMIG", mid, f"slot {slot} joined in progress")

    def end(self, mid):
        with self.lock:
            self._drop(mid)

    def _drop(self, mid):
        m = self.matches.pop(mid, None)
        if m:
            for t in m["players"]:
                if self.by_token.get(t) == mid:
                    del self.by_token[t]

    def has(self, token):
        with self.lock:
            return token in self.by_token

    def given_up(self, mid):
        with self.lock:
            m = self.matches.get(mid)
            return bool(m and m["given_up"])

    def pending_stale(self, mid, secs):
        """Seconds since the host was lost when no successor has been picked for more than `secs` s (nobody left alive asked: ghost match), else 0."""
        with self.lock:
            m = self.matches.get(mid)
            if not m or not m.get("pending") or m["given_up"] or not m.get("lost"):
                return 0.0
            age = self.now() - m["lost"]
            return age if age > secs else 0.0

    def host_info(self, mid):
        """{"host": slot, "epoch": n, "pending": bool, "given_up": bool} of a match, None if unknown."""
        with self.lock:
            m = self.matches.get(mid)
            if not m:
                return None
            return {"host": m["host"], "epoch": m["epoch"], "pending": bool(m.get("pending")), "given_up": m["given_up"],
                    "alone": bool(m.get("alone"))}

    def snapshot(self, mid):
        """(start time, players list, alive slots, host slot) of a match for the periodic re-pick, None if unknown / given up."""
        with self.lock:
            m = self.matches.get(mid)
            if not m or m["given_up"]:
                return None
            return (m["t0"], list(m["players"]), [s for s in range(len(m["players"])) if s not in m["gone"]], m["host"])

    def mids(self):
        with self.lock:
            return list(self.matches)

    def export_match(self, mid):
        """JSON-able copy of one match record for a daemon restart (live-state file); None if unknown / given up."""
        with self.lock:
            m = self.matches.get(mid)
            if not m or m["given_up"]:
                return None
            out = {}
            for k, v in m.items():
                if k == "ho":
                    continue                                   # a planned handover in preparation is abandoned by a restart
                if isinstance(v, set):
                    v = sorted(v)
                elif isinstance(v, dict):
                    v = {str(a): b for a, b in v.items()}
                out[k] = v
            return out

    def import_match(self, mid, rec):
        """Restore export_match() after a daemon restart. Poll / silence times are reset - the first polls of the clients decide again;
        a host loss that was being collected starts its candidate window afresh (every survivor has to ask again)."""
        with self.lock:
            self._drop(mid)
            now = self.now()
            m = dict(rec)
            for k in ("gone", "cap", "cand0"):
                if k in m:
                    m[k] = set(m[k])
            m["seen"] = {}; m["silent"] = {}; m["silent_since"] = None; m["bridge_at"] = {}
            m["bridge_gone"] = set(); m["adopted"] = now
            for k in ("replaced", "replaced_told", "joined"):   # (export_match wrote the slot keys as strings)
                if isinstance(m.get(k), dict):
                    m[k] = {int(a): b for a, b in m[k].items()}
            m.setdefault("host", 0); m.setdefault("epoch", 0); m.setdefault("gone", set()); m["given_up"] = False
            if m.get("pending"):
                m["collect_t0"] = now; m["lost"] = now; m["cand0"] = set(); m.pop("collect_logged", None)
            if m.get("migrated_at") is not None:
                m["migrated_at"] = now - self.settle           # no 'just migrated' settling window after a restart
            self.matches[mid] = m
            for t in m["players"]:
                if not t.startswith("f0" * 30):                # (host-migration placeholders are not players)
                    self.by_token[t] = mid
            self.log("HOSTMIG", mid, "re-adopted after a daemon restart: host slot", m["host"], "epoch", m["epoch"],
                     "(host loss pending)" if m.get("pending") else "")

    def host_token(self, token):
        """Token of the current host of token's match (None if unknown)."""
        with self.lock:
            m = self.matches.get(self.by_token.get(token))
            return m["players"][m["host"]] if m else None

    # ------------------------------------------------------------ events
    def depart(self, token, reason="left"):
        """token left its running match (op 0x02 / searching again / quit). The host leaving
        triggers the migration at once."""
        with self.lock:
            mid = self.by_token.get(token)
            m = self.matches.get(mid)
            if not m:
                return
            slot = m["players"].index(token)
            if slot in m["gone"]:
                return
            m["gone"].add(slot)
            self.log("HOSTMIG", mid, f"slot {slot} {reason}", "(host)" if slot == m["host"] else "")
            if slot == m["host"]:
                self._host_lost(mid, m, reason)

    def bridge_gone(self, token):
        """token's Steam helper disconnected (it exits with the game, so a crashed or closed
        host). Counted together with a silence report from a client."""
        with self.lock:
            mid = self.by_token.get(token)
            m = self.matches.get(mid)
            if not m:
                return
            slot = m["players"].index(token)
            m["bridge_gone"].add(slot)
            m["bridge_at"][slot] = self.now()
            self.log("HOSTMIG", mid, f"slot {slot} Steam helper gone")
            self._evaluate(mid, m)

    def bridge_back(self, token):
        """token's Steam helper registered again (link recovered): the slot is no longer 'helper gone'."""
        with self.lock:
            mid = self.by_token.get(token)
            m = self.matches.get(mid)
            if not m or token not in m["players"]:
                return
            slot = m["players"].index(token)
            if slot in m["bridge_gone"]:
                m["bridge_gone"].discard(slot); m["bridge_at"].pop(slot, None)
                self.log("HOSTMIG", mid, f"slot {slot} Steam helper back")

    def poll(self, token, flags=0, epoch=0):
        """An ext asks (op 0xE6). Returns the 0xE7 payload."""
        if not self.enabled:
            return bytes([0, 0, 0, 0, ST_GIVE_UP])
        with self.lock:
            mid = self.by_token.get(token)
            m = self.matches.get(mid)
            if not m:
                if self.unknown_keep_going:
                    return bytes([1, 0, 0, 0xFF, ST_NORMAL])
                return bytes([1, 0, 0, 0, ST_GIVE_UP])
            slot = m["players"].index(token)
            now = self.now()
            m["seen"][slot] = now
            if flags & 2:
                m.setdefault("cap", set()).add(slot)          # this ext can take part in a planned handover
            if slot in m["gone"]:
                # we wrote it off (it left, or it was the host we migrated away from): it is
                # not part of this match any more
                if slot in m.get("replaced", {}):
                    m.setdefault("replaced_told", {})[slot] = now
                return bytes([1, m["host"], m["epoch"], self._alive_mask(m), ST_GIVE_UP])
            if m.get("pending"):
                self._host_lost(mid, m, "successor asked")
            # a report only counts against the host the reporter is actually connected to: an
            # ext still on an older epoch is about to switch (the reply tells it), its silence
            # is about the host we already replaced
            # right after a migration the new host needs a moment to take over (it learns it on
            # its next poll): silence then is expected, not a reason to move again
            settling = now - m.get("migrated_at", -1e9) < self.settle
            if flags & 1 and slot != m["host"] and epoch == m["epoch"] and not settling:
                if slot not in m["silent"]:
                    self.log("HOSTMIG", mid, f"slot {slot} reports the host (slot {m['host']}) silent")
                m["silent"][slot] = now
                if m["silent_since"] is None:
                    m["silent_since"] = now
            elif not flags & 1 and slot in m["silent"]:
                m["silent"].pop(slot, None)        # hears the host again
                if not m["silent"]:
                    m["silent_since"] = None
                    m.pop("min_silent_logged", None)
            self._evaluate(mid, m)
            ho = m.get("ho")
            if ho:
                if flags & 4 and epoch == m["epoch"]:
                    ho["ready"].add(slot)
                self._handover_step(mid, m)
            state = ST_GIVE_UP if m["given_up"] else (ST_SUSPECT if m["silent"] else ST_HANDOVER if m.get("ho") else ST_NORMAL)
            return bytes([1, m["host"], m["epoch"], self._alive_mask(m), state])

    # ------------------------------------------------------------ decision
    def _host_dead(self, m, h):
        """The host's link to the server is confirmed down: its Steam helper went away or its session socket is gone."""
        if h in m["bridge_gone"]:
            return True
        if self.link_alive:
            try:
                return not self.link_alive(m["players"][h])
            except Exception:
                return False
        return False

    def replaced_age(self, token):
        """Seconds since `token` was written off as host on silence (migration away from it) or since it was last told so (its first poll
        after that gets 'give up' and the ext leaves the match) - the smaller one; None when it never was."""
        with self.lock:
            m = self.matches.get(self.by_token.get(token))
            if not m or token not in m["players"]:
                return None
            slot = m["players"].index(token)
            t = m.get("replaced", {}).get(slot)
            if t is None:
                return None
            told = m.get("replaced_told", {}).get(slot)
            now = self.now()
            return min(now - t, now - told) if told is not None else now - t

    def _alive_mask(self, m):
        return sum(1 << s for s in range(len(m["players"])) if s not in m["gone"])

    START_GRACE = 12.0          # seconds after the match start in which a polling host is never replaced on silence reports alone

    def _evaluate(self, mid, m):
        if m["given_up"] or m.get("pending"):
            return
        now = self.now()
        h = m["host"]
        for s in [s for s, t in m["silent"].items() if now - t > self.silent_window]:
            del m["silent"][s]                       # stale report (that ext stopped asking)
        if not m["silent"]:
            m["silent_since"] = None
            m.pop("min_silent_logged", None)
            return
        others = [s for s in range(len(m["players"])) if s != h and s not in m["gone"]]
        # every player still in the match must hear silence (one player's own connection
        # trouble must not move the host); players that never report (crashed too, ext off)
        # are covered by lone_wait, a host whose Steam helper went away by bridge_gone
        all_silent = bool(others) and all(s in m["silent"] for s in others)
        # In a 1v1 with both on one PC the client's ext reported the host silent 3 s into the match (its link stamp was stale from the
        # level load) and 'all players' was that ONE report -> the live host (still polling every 2 s) was replaced and the client won
        # instantly. A host whose own ext polled within the alive window is alive: its silence reports alone cannot replace it when that would
        # leave a single player (instant win) or in the first START_GRACE s of the match (links still coming up). Real loss still goes through
        # `lone` (host ext quiet too) or bridge_gone.
        if all_silent and now - m["seen"].get(h, -1e9) <= self.alive_window and (len(others) <= 1 or now - m.get("t0", now) < self.START_GRACE):
            if not m.get("alive_host_logged"):
                m["alive_host_logged"] = 1
                self.log("HOSTMIG", mid, f"host slot {h} still polls - silence reports from {len(others)} player(s) not enough to replace it")
            all_silent = False
        # some players hear silence for lone_wait while the host's own ext stopped asking too
        # (the host's ext polls every 2 s while it hosts): the host is gone
        if all_silent and self.min_silent > 0 and now - m["silent_since"] < self.min_silent and not self._host_dead(m, h):
            if not m.get("min_silent_logged"):
                m["min_silent_logged"] = 1
                fz = m.setdefault("freezes", [])          # one entry per freeze episode of THIS host (HostMigration.freezes, daemon repick_cap)
                fz.append((h, now))
                del fz[:-20]
                self.log("HOSTMIG", mid, f"every player reports host slot {h} silent, but its link to the server is up - waiting until the silence lasts {self.min_silent:.0f} s (a hitch is no host loss)")
            all_silent = False
        lone = (now - m["silent_since"] >= self.lone_wait and
                now - m["seen"].get(h, -1e9) > self.alive_window)
        hgone = h in m["bridge_gone"] and now - m["bridge_at"].get(h, now) >= self.bridge_grace
        if hgone or all_silent or lone:
            why = ("host's Steam helper gone" if hgone else
                   "all players report silence" if all_silent else "silence reports for %.0f s" % self.lone_wait)
            m["gone"].add(h)
            m.setdefault("replaced", {})[h] = now     # written off while it may still run: its leave after the 'give up' reply is excused (replaced_age)
            self._host_lost(mid, m, why)

    # ------------------------------------------------------------ planned handover (HostRepick)
    HANDOVER_TIMEOUT = 30.0     # prepare phase abort (the ext polls every 10 s as a client until it hears state 3)

    def freezes(self, mid, host_slot, window_s):
        """Freeze episodes of host_slot in the last window_s seconds: every other player reported the host silent while its link to the
        server stayed up (the min_silent wait above). A host can freeze for everyone again and again, each freeze shorter than
        min_silent, and so never count as lost."""
        with self.lock:
            m = self.matches.get(mid)
            if not m:
                return 0
            now = self.now()
            return sum(1 for h, t in m.get("freezes", ()) if h == host_slot and now - t <= window_s)

    def roster(self, mid):
        """Alive slots of a match (frozenset), None when unknown."""
        with self.lock:
            m = self.matches.get(mid)
            return frozenset(s for s in range(len(m["players"])) if s not in m["gone"]) if m else None

    POLL_STALE_S = 25.0         # an alive slot whose ext has not polled for this long is 'not polling' (clients poll every 10 s)
    JIP_LOAD_S = 120.0          # a joiner that has not polled yet this long after its join is still loading (not stale)

    def _stale(self, m, now):
        """(stale, loading): alive slots whose ext is not polling. loading = joined in progress < JIP_LOAD_S ago and not polled since
        (its ext arms only in the match). The JIP joiner (slot 2) never polled after a daemon restart, its session was lost
        40 s later - and 'slot 2 not polling' held the whole re-pick of a lagging host."""
        stale, loading = set(), set()
        for s in range(len(m["players"])):
            if s in m["gone"] or now - m["seen"].get(s, m.get("adopted", m.get("t0", -1e9))) <= self.POLL_STALE_S:
                continue                    # (no poll yet: counted from the match start / the re-adoption after a daemon restart)
            j = (m.get("joined") or {}).get(s)
            if s not in m["seen"] and j is not None and now - j < self.JIP_LOAD_S:
                loading.add(s)
            else:
                stale.add(s)
        return stale, loading

    def stale_slots(self, mid):
        """(stale, loading) slot sets of a match (see _stale); (set(), set()) when unknown."""
        with self.lock:
            m = self.matches.get(mid)
            return self._stale(m, self.now()) if m else (set(), set())

    def _handover_ok(self, m, slot=None):
        if not m or not self.enabled:
            return False, "no migration record"
        if m["given_up"] or m.get("pending") or m.get("ho") or m["silent"] or (m["bridge_gone"] - m["gone"]):
            return False, "migration busy / host link trouble"
        now = self.now()
        if now - m.get("migrated_at", -1e9) < 20.0:
            return False, "previous migration just happened"
        stale, loading = self._stale(m, now)
        if loading:
            return False, "slot %d still loading (joined in progress)" % min(loading)
        if m["host"] in stale:
            return False, "host slot %d not polling" % m["host"]
        if slot is not None and slot in stale:
            return False, "target slot %d not polling" % slot
        live = [s for s in range(len(m["players"])) if s not in m["gone"] and s not in stale]
        if len(live) < 2:
            return False, "slot %d not polling" % min(stale) if stale else "nobody to hand over to"
        for s in live:          # a stale slot is left out: it is not asked to prepare and follows the new epoch on its next poll (or leaves)
            if s not in m.get("cap", ()):
                return False, "slot %d ext has no handover support" % s
        return True, ""

    def handover_ok(self, mid):
        """(ok, why): a planned handover could start now - a clean, settled match whose every alive player runs an ext that can do it."""
        with self.lock:
            return self._handover_ok(self.matches.get(mid))

    def handover(self, mid, slot):
        """Start a planned handover to `slot` (phase 1). The old host is NOT marked gone. True when started."""
        with self.lock:
            m = self.matches.get(mid)
            ok, why = self._handover_ok(m, slot)
            if not ok or slot in m["gone"] or slot == m["host"] or not (0 <= slot < len(m["players"])):
                return False
            stale = self._stale(m, self.now())[0]
            roster = frozenset(s for s in range(len(m["players"])) if s not in m["gone"])
            m["ho"] = {"slot": slot, "t": self.now(), "ready": set(), "epoch": m["epoch"], "roster": roster, "need": roster - stale}
            self.log("HOSTMIG", mid, f"planned handover host slot {m['host']} -> slot {slot}: preparing (epoch {m['epoch']})",
                     ("- not polling, left out: %s" % sorted(stale)) if stale else "")
            return True

    def handover_pending(self, mid):
        with self.lock:
            m = self.matches.get(mid)
            return bool(m and m.get("ho"))

    def _handover_step(self, mid, m):
        ho = m.get("ho")
        if not ho:
            return
        now = self.now()
        alive = frozenset(s for s in range(len(m["players"])) if s not in m["gone"])
        why = None
        if m["given_up"] or m.get("pending") or m["epoch"] != ho["epoch"]:
            why = "a real migration took over"
        elif alive - ho["roster"] or (ho.get("need", ho["roster"]) - alive):
            why = "roster changed"
        elif m["silent"]:
            why = "host link trouble"
        elif now - ho["t"] > self.HANDOVER_TIMEOUT:
            why = "not everyone prepared in %.0f s (ready %s of %s)" % (self.HANDOVER_TIMEOUT, sorted(ho["ready"]), sorted(alive))
        if why:
            m.pop("ho", None)
            self.log("HOSTMIG", mid, "planned handover aborted:", why)
            return
        if (alive & ho.get("need", alive)) <= ho["ready"]:
            old = m["host"]
            m["host"] = ho["slot"]
            m["epoch"] += 1
            m["silent"].clear()
            m["silent_since"] = None
            m["migrated_at"] = now
            m.pop("ho", None)
            self.log("HOSTMIG", mid, f"planned handover: host slot {old} -> new host slot {m['host']}",
                     f"epoch {m['epoch']} alive mask {self._alive_mask(m):04b} (old host stays as a client)")

    def _host_lost(self, mid, m, why):
        """The host is gone (slot already in m["gone"]). The successor is NOT the first player that asks (that was whoever's 10 s poll timer fired first,
        whatever its ping): for `collect` s every survivor that polls is a candidate, then _pick scores them all (worst measured ping). A pick
        happens earlier when every survivor has polled since the loss. Players that polled in the alive_window before the loss count too (old
        clients poll every 10 s healthy and cannot answer faster). No candidate at all: wait, the first one that asks wins (as before)."""
        m.pop("ho", None)
        now = self.now()
        old = m["host"]
        left = [s for s in range(len(m["players"])) if s not in m["gone"]]
        if len(left) < self.min_left:
            m["given_up"] = True
            self.log("HOSTMIG", mid, f"host slot {old} lost ({why}); nobody left to host - match ends")
            return
        if not m.get("pending"):
            m["pending"] = True
            m["lost"] = now
            m["collect_t0"] = now
            m["cand0"] = {s for s in left if now - m["seen"].get(s, -1e9) <= self.alive_window}     # polled shortly before the loss
            m.pop("collect_logged", None)
        t0 = m["collect_t0"]
        cands = [s for s in left if s in m.get("cand0", ()) or m["seen"].get(s, -1e9) >= t0]
        asked = all(m["seen"].get(s, -1e9) >= t0 for s in left)
        if not cands:
            if "collect_logged" not in m:
                m["collect_logged"] = 1
                self.log("HOSTMIG", mid, f"host slot {old} lost ({why}); waiting for a player to ask")
            return
        if now - t0 < self.collect and not asked:
            if "collect_logged" not in m:
                m["collect_logged"] = 1
                self.log("HOSTMIG", mid, f"host slot {old} lost ({why}); collecting host candidates for {self.collect:.0f} s ({len(cands)} of {len(left)} so far)")
            return
        # fuzz seed 249: a candidate that asked at the loss and then froze / crashed was picked 16 s later, when the first (new) player finally
        # polled - the pick went to a dead player. Candidates whose last poll is stale are dropped while a fresher one exists (the asker always is).
        fresh = [s for s in cands if now - m["seen"].get(s, -1e9) <= self.alive_window + self.collect]
        if fresh:
            cands = fresh
        elif why == "pending":
            return                          # the periodic tick has only stale candidates: wait for a live player to ask (seed 305: tick picked a client that crashed 11 s before)
        new = self._pick(m, cands)
        m["host"] = new
        m["epoch"] += 1
        m["silent"].clear()
        m["silent_since"] = None
        m.pop("min_silent_logged", None)
        m["lost"] = None
        m["migrated_at"] = now
        # A successor with nobody else alive ends the match on its side at once (ext hm_migrate: 'alone in the match - ending
        # it'), so the match is over even though no 0x12 reached us yet: join in progress must not send anyone into it
        if len(left) == 1:
            m["alone"] = True
        m.pop("pending", None); m.pop("collect_t0", None); m.pop("collect_logged", None); m.pop("cand0", None)
        self.log("HOSTMIG", mid, f"host slot {old} lost ({why}) -> new host slot {new}",
                 f"epoch {m['epoch']} alive mask {self._alive_mask(m):04b}")

    def _pick(self, m, cands):
        """Lowest worst-case Steam ping (+ the machine-stall penalty) to the other candidates, then lowest slot."""
        if self.pool and len(cands) > 1:
            try:
                keep, info = self.pool([m["players"][c] for c in cands])
                sub = [c for c in cands if m["players"][c] in keep]
                if sub:
                    cands = sub
                self.log("HOSTMIG", "region rule:", info)
            except Exception:
                pass

        def pen(s):
            try:
                return self.perf(m["players"][s]) if self.perf else 0
            except Exception:
                return 0

        # One survivor with no ping to anybody (no Steam helper data) made every key unknown, so the pick fell back to slot order and could
        # put the match on a host far from someone. A candidate with no ping to any other candidate is left out of the others' keys (it cannot
        # be judged either way) and is not picked while a judged one exists.
        blind = set()
        if self.ping and len(cands) > 2:
            blind = {c for c in cands if all(self.ping(m["players"][c], m["players"][o]) is None for o in cands if o != c)}
            if len(blind) >= len(cands) - 1:
                blind = set()

        def key(s):
            if self.ping and len(cands) > 1 and s not in blind:
                p = [self.ping(m["players"][s], m["players"][o]) for o in cands if o != s and o not in blind]
                if p and None not in p:
                    return host_key(p, pen(s)) + (s,)
            return (1e9 + pen(s), 1e9, s)
        return min(cands, key=key)

    def tick(self):
        """Periodic (status loop): a host lost while nobody was asking gets a successor as soon
        as someone asks (handled in poll); here only stale silence reports expire."""
        with self.lock:
            for mid, m in list(self.matches.items()):
                if m.get("pending"):
                    self._host_lost(mid, m, "pending")
                else:
                    self._evaluate(mid, m)


# ------------------------------------------------------------ planned host re-pick
# Every REPICK_INTERVAL s per running match the daemon scores every player as host on the live measured pairwise pings (same
# key as the match-start pick: worst ping + stall penalty, then total) and moves the host when the best candidate is clearly better.
REPICK_MODE = _envi("T2R_HOST_REPICK", 1)             # 0 off, 1 check + log only ('would migrate'), 2 live handover
REPICK_INTERVAL = _envi("T2R_HOST_REPICK_INTERVAL_S", 90)
# Calibration: re/host-repick-calibration-1019.md. All tunable by env without a rebuild.
REPICK_MIN_AGE = _envi("T2R_HOST_REPICK_MIN_AGE_S", 120)    # never in the first 120 s (pings are still settling, loading stutter)
REPICK_MIN_LEFT = _envi("T2R_HOST_REPICK_MIN_LEFT_S", 90)   # never with less than 90 s on the round clock (also covers 'last fight')
REPICK_MIN_GAIN_MS = _envi("T2R_HOST_REPICK_GAIN_MS", 40)   # the best host's worst ping must be this much lower ...
REPICK_MIN_GAIN_FRAC = _envi("T2R_HOST_REPICK_GAIN_PCT", 20) / 100.0    # ... and this fraction lower than the current host's
REPICK_CONFIRM = _envi("T2R_HOST_REPICK_CONFIRM", 2)        # same target on this many consecutive checks
REPICK_CONFIRM_GAP_S = _envi("T2R_HOST_REPICK_CONFIRM_GAP_S", 30)  # after a 'would migrate' the confirming check comes this soon (not a full interval)
# (a player in South America hosted two players in Europe; worst ping 315 -> 274 ms with one of them = 13 %, under the 20 % rule,
# so it stayed although the two European players talked through South America, 585 ms instead of 155): with one far player the worst ping is the
# far player's link whoever hosts. A host whose worst ping is not worse and whose TOTAL is clearly lower also wins.
REPICK_TOTAL_GAIN_MS = _envi("T2R_HOST_REPICK_TOTAL_GAIN_MS", 100)    # the total ping must drop by this much ...
REPICK_TOTAL_GAIN_FRAC = _envi("T2R_HOST_REPICK_TOTAL_GAIN_PCT", 20) / 100.0    # ... and this fraction of the current host's total
REPICK_TOTAL_GAIN_PER_MS = _envi("T2R_HOST_REPICK_TOTAL_GAIN_PER_MS", 25)    # ... and on average this much per other player (100 ms over 7 links = 14 ms each: not noticeable)
REPICK_GAP_S = _envi("T2R_HOST_REPICK_GAP_S", 180)          # a 2nd migration needs this long since the 1st
REPICK_MAX = _envi("T2R_HOST_REPICK_MAX", 2)                # per match; the 2nd only when the roster changed since the 1st
# Capacity override (a ~1 Mbit host saturated, every ping to it 83 -> 790-970 ms, nothing handed over: the ping score
# cannot show it - every candidate's worst ping IS its ping to the saturated host). The daemon passes cap={'host_bad': why, 'rank': {slot: 0|1|2},
# 'max_rank': highest target rank allowed}: a host that lagged below the needed upload (hostcap, in this match) or whose link is saturated is
# replaced by the best candidate (proven capacity first, then the ping key), even when the ping rule alone would hold. Same guards as above.
# (live handover on: a host change only for a clear gain): the capacity target is confirmed on a second check (30 s later) and its worst
# ping may not be worse than the lagging host's (was 1 check, up to 150 ms worse).
REPICK_CAP_CONFIRM = _envi("T2R_HOST_REPICK_CAP_CONFIRM", 2)        # same capacity target on this many consecutive checks
# A lagging host used to be kept whenever it had the lowest worst ping, which it usually has (it was picked for ping). The target may be
# up to 25 ms worse, but never above the match ping limit (REPICK_CAP_LIMIT_MS) unless the lagging host is already above it.
REPICK_CAP_MAX_WORSE_MS = _envi("T2R_HOST_REPICK_CAP_WORSE_MS", 25)  # the target's worst ping may be at most this much above the lagging host's
REPICK_CAP_LIMIT_MS = _envi("T2R_PING_MATCH_MS", 175)                # ... and at most this (or the host's own worst, when higher)
# A PING-only handover ("a better connection") in an 8 player lobby put the match on a host whose upload could not carry
# the ~2.3 MB/s the relay needs there: five peers fell silent within seconds, the server counted them gone and the match emptied. In a lobby this big a
# handover that is only about ping goes to a target hostcap has PROVEN (rank 0) for this size; the capacity override above is unchanged.
REPICK_PROVEN_N = _envi("T2R_HOST_REPICK_PROVEN_N", 6)             # lobby size from which a ping-only handover needs a proven target (0 = never)
SAT_X = _envi("T2R_HOST_SAT_PCT", 300) / 100.0                      # link saturated: every ping to the host >= 3x its match baseline ...
SAT_MIN_MS = _envi("T2R_HOST_SAT_MIN_MS", 150)                      # ... and >= this many ms above it


def _guard_code(hold):
    for k, c in (("young", "young"), ("pending", "busy"), ("handover", "busy"), ("loading", "loading"), ("clock", "clock"), ("round ends", "endgame"),
                 ("within", "gap"), ("limit", "limit")):
        if k in hold:
            return c
    return "busy"


def format_check(mid, age, left, host, scores, info, act, what, d):
    """One short line, no names / IPs (slots only):
    [ts] REPICK-CHECK m=<id> age=<s> left=<s|?> host=<slot> cand=<slot>:<worst>/<total>[/l<loss%>][/p<perf ms>] ... rtt <a>-<b>:<ms> ... dec=stay|would|migrate [tgt=<slot> gain=<ms> n=<k>] why=<code>
    why: best | small | total | young | busy | clock | endgame | gap | limit | unknown | confirm | go. 'age' of the oldest pair sample: rttage=<s>."""
    info = info or {}
    perf, loss = info.get("perf") or {}, info.get("loss") or {}
    cands = []
    for s in sorted(scores, key=str):
        k = scores[s]
        c = "%s:%s" % (s, "?" if k is None else "%d/%d" % (k[0], k[1]))
        if k is not None and loss.get(s) is not None:
            c += "/l%g" % loss[s]
        if k is not None and perf.get(s):
            c += "/p%d" % perf[s]
        cands.append(c)
    rtt = info.get("rtt") or {}
    pairs = " ".join("%s-%s:%d" % (a, b, v) for (a, b), v in sorted(rtt.items(), key=lambda kv: (str(kv[0][0]), str(kv[0][1]))) if v is not None)
    kind = d[0]
    why = d[1] if len(d) > 1 and d[1] else "?"
    if kind == "would":
        tgt = d[2]
        cur, new = scores.get(host), scores.get(tgt)
        dec = "dec=%s tgt=%s gain=%d n=%d" % ("migrate" if act == "migrate" else "would", tgt, (cur[0] - new[0]) if cur and new else 0, d[3])
    elif kind == "guard" and len(d) > 2 and why == "total":
        dec = "dec=stay tgt=%s" % d[2]
    elif kind == "guard" and why not in ("unknown",):
        dec = "dec=stay"
    else:
        dec = "dec=stay"
    ag = info.get("age")
    return "m=%s age=%d left=%s host=%s cand=%s rtt %s%s %s why=%s" % (
        mid, age, "?" if left is None else int(left), host, " ".join(cands) or "-", pairs or "-",
        "" if ag is None else " rttage=%d" % ag, dec, why)


class HostRepick:
    """Pure decision logic: check(mid, ...) -> (action, text); action 'migrate' | 'hold'. One record per match."""

    def __init__(self, now=time.time, log=print, mode=None):
        self.now = now
        self.log = log
        self.mode = REPICK_MODE if mode is None else mode
        self.recs = {}
        self.last_text = {}
        self.base = {}              # mid -> {frozenset((token a, token b)): lowest pair ping seen in this match}

    def forget(self, mid):
        self.recs.pop(mid, None)
        self.last_text.pop(mid, None)
        self.base.pop(mid, None)

    def observe(self, mid, rtt):
        """rtt: {(token a, token b): ms or None} of the match right now; keeps the lowest per pair (the link's 'usual' ping)."""
        b = self.base.setdefault(mid, {})
        for (a, c), v in rtt.items():
            if v:
                k = frozenset((a, c))
                if k not in b or v < b[k]:
                    b[k] = v

    def saturated(self, mid, host, others, ping):
        """(True, text) when the host's link looks saturated: every known ping host<->other is >= SAT_X x its match baseline and >= SAT_MIN_MS
        above it, and (3+ players) some pair WITHOUT the host is not - the host is the common factor. ping(a, b) -> ms or None."""
        b = self.base.get(mid) or {}
        hits = []
        for o in others:
            v, b0 = ping(host, o), b.get(frozenset((host, o)))
            if v is None or not b0:
                continue
            if v < b0 * SAT_X or v - b0 < SAT_MIN_MS:
                return False, ""
            hits.append("%d->%d" % (b0, v))
        if not hits:
            return False, ""
        if len(others) >= 2:
            calm = False
            for i, a in enumerate(others):
                for c in others[i + 1:]:
                    v, b0 = ping(a, c), b.get(frozenset((a, c)))
                    if v is not None and b0 and (v < b0 * SAT_X or v - b0 < SAT_MIN_MS):
                        calm = True
            if not calm:
                return False, ""
        return True, "pings to the host " + ", ".join(hits) + " ms"

    def _say(self, mid, text):
        if self.last_text.get(mid) != text:          # compact log: only when the decision changes
            self.last_text[mid] = text
            self.log("HOST-REPICK", mid, text)

    def due(self, mid, started_at):
        r = self.recs.setdefault(mid, {"next": started_at + max(REPICK_MIN_AGE, REPICK_INTERVAL), "streak": (None, 0), "done": 0, "last": None, "roster": None})
        return self.now() >= r["next"]

    def check(self, mid, started_at, roster, host, scores, time_left, busy=None, labels=None, info=None, cap=None):
        """One decision + ONE compact REPICK-CHECK log line per call (see format_check). info (optional): {'rtt': {(a, b): ms}, 'perf': {slot: ms},
        'loss': {slot: pct}, 'age': oldest pair sample age in s}. cap (optional): capacity override, see REPICK_CAP_CONFIRM."""
        self._d = ("?", None)
        act, what = self._decide(mid, started_at, roster, host, scores, time_left, busy, labels, cap)
        try:
            self.log("REPICK-CHECK", format_check(mid, self.now() - started_at, time_left, host, scores, info, act, what, self._d))
        except Exception as e:                       # a log line must never break the decision
            self.log("REPICK-CHECK format error", e)
        return act, what

    def _decide(self, mid, started_at, roster, host, scores, time_left, busy=None, labels=None, cap=None):
        """roster: frozenset of alive players (any hashable); host: current host; scores: {player: key or None} for every candidate
        in the region pool PLUS the current host (key = (worst ms incl. penalty, total ms)); time_left: seconds on the round
        clock, None = unknown/ended; busy: reason string when a migration is already pending / the link is troubled."""
        now = self.now()
        r = self.recs.setdefault(mid, {"next": 0, "streak": (None, 0), "done": 0, "last": None, "roster": None})
        r["next"] = now + REPICK_INTERVAL
        if r["roster"] is None:
            r["roster"] = roster
        hold = None
        if now - started_at < REPICK_MIN_AGE:
            hold = "match too young"
        elif busy:
            hold = busy
        elif time_left is None:
            hold = "round clock unknown or ended"
        elif time_left < REPICK_MIN_LEFT:
            hold = "round ends in %d s" % time_left
        elif r["done"] >= REPICK_MAX or (r["done"] >= 1 and (roster == r["roster"] or now - r["last"] < REPICK_GAP_S)):
            hold = "migration limit reached (%d)" % r["done"] if r["done"] >= REPICK_MAX or roster == r["roster"] else "within %d s of the last migration" % REPICK_GAP_S
        if hold:
            r["streak"] = (None, 0)
            self._d = ("guard", _guard_code(hold))
            self._say(mid, "hold: " + hold)
            return "hold", hold
        cur = scores.get(host)
        cands = {p: k for p, k in scores.items() if k is not None}
        if cap and cap.get("host_bad"):
            rank = cap.get("rank") or {}
            alts = [p for p in cands if p != host and rank.get(p, 1) <= cap.get("max_rank", 1)
                    and (cur is None or (cands[p][0] <= cur[0] + REPICK_CAP_MAX_WORSE_MS and cands[p][0] <= max(cur[0], REPICK_CAP_LIMIT_MS)))]
            if alts:
                best = min(alts, key=lambda p: (rank.get(p, 1), cands[p]))
                tgt, n = r["streak"]
                n = n + 1 if tgt == ("cap", best) else 1
                r["streak"] = (("cap", best), n)
                lab = (labels or {}).get(best, best)
                need = 1 if cap.get("saturated") else REPICK_CAP_CONFIRM      # a saturated link (pings 3x up NOW) moves at once; a lag record is confirmed
                text = "host %s: migrate to %s (%s), worst %s -> %d ms, check %d of %d" % (
                    cap["host_bad"], lab, (cap.get("text") or {}).get(best, "rank %d" % rank.get(best, 1)), "?" if cur is None else cur[0],
                    cands[best][0], n, need)
                self._d = ("would", "cap" if n < need else "capgo", best, n)
                self._say(mid, text)
                if n < need:
                    r["next"] = min(r["next"], now + REPICK_CONFIRM_GAP_S)
                    return "hold", text
                return "migrate", best
            self._say(mid, "host %s, but no candidate with enough upload and a known ping" % cap["host_bad"])
        if cur is None or not cands:
            r["streak"] = (None, 0)
            self._d = ("guard", "unknown")
            self._say(mid, "hold: pings unknown")
            return "hold", "pings unknown"
        best = min(cands, key=lambda p: (cands[p], 0 if p == host else 1))
        if cap and cap.get("rank") and REPICK_PROVEN_N and len(roster) >= REPICK_PROVEN_N:
            rank = cap["rank"]
            # only the current host and targets proven at least as well as it stay in the race. Most matches that kept a host 26-106 ms
            # worse than the best candidate (303 v 197 ms) had a host that was itself not clean (proven but lagged x2-x58):
            # a host with lag records may hand over to an unproven (rank 1) target; a clean proven host still needs a proven one.
            hr = max(0, min(rank.get(host, 1), 1))
            ok = {p: k for p, k in cands.items() if p == host or rank.get(p, 1) <= hr}
            if best != host and best not in ok:
                best2 = min(ok, key=lambda p: (ok[p], 0 if p == host else 1)) if ok else host
                self._say(mid, "hold: best ping target %s is not proven for %d players (%s)" % ((labels or {}).get(best, best), len(roster), (cap.get("text") or {}).get(best, "unproven")))
                best = best2
        if best != host and cands[best][1] > cur[1]:   # a lower worst ping bought with a higher total = a worse host for most of the lobby
            r["streak"] = (None, 0)
            self._d = ("guard", "total", best)
            self._say(mid, "hold: host is fine (worst %d ms, best other has a higher total %d > %d ms)" % (cur[0], cands[best][1], cur[1]))
            return "hold", "total worse"
        worst_gain = best != host and cands[best][0] <= cur[0] - REPICK_MIN_GAIN_MS and cands[best][0] <= cur[0] * (1 - REPICK_MIN_GAIN_FRAC)
        total_gain = (best != host and cands[best][0] <= cur[0]
                      and cur[1] - cands[best][1] >= max(REPICK_TOTAL_GAIN_MS, REPICK_TOTAL_GAIN_PER_MS * (len(roster) - 1))
                      and cands[best][1] <= cur[1] * (1 - REPICK_TOTAL_GAIN_FRAC))
        if not (worst_gain or total_gain):
            r["streak"] = (None, 0)
            self._d = ("stay", "best" if best == host else "small", best)
            self._say(mid, "hold: host is fine (worst %d ms%s)" % (cur[0], "" if best == host else ", best other %d ms - not worth it" % cands[best][0]))
            return "hold", "not worth it"
        if len(roster) < 3:                          # 2 players: both directions are the same link - only the capacity override moves a host
            r["streak"] = (None, 0)
            self._d = ("stay", "small", best)
            return "hold", "2 players"
        tgt, n = r["streak"]
        n = n + 1 if tgt == best else 1
        r["streak"] = (best, n)
        lab = (labels or {}).get(best, best)
        text = "would migrate to %s: worst %d -> %d ms (total %d -> %d%s), check %d of %d" % (lab, cur[0], cands[best][0], cur[1], cands[best][1],
                                                                                    "" if worst_gain else ", total rule", n, REPICK_CONFIRM)
        self._d = ("would", "confirm" if n < REPICK_CONFIRM else "go", best, n)
        self._say(mid, text)
        if n < REPICK_CONFIRM:
            r["next"] = min(r["next"], now + REPICK_CONFIRM_GAP_S)
            return "hold", text
        return "migrate", best

    def migrated(self, mid, roster):
        r = self.recs.get(mid)
        if r:
            r["done"] += 1
            r["last"] = self.now()
            r["roster"] = roster
            r["streak"] = (None, 0)
