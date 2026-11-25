"""Stable search lobbies: a player seen on the searching page, and talked to in the LOBBY chat, stays in the lobby until they back out.

Before: the searching page listed EVERY searcher of the mode (up to 20) and the LOBBY chat was one room for all of them, while the matchmaker
took any 8 (or fewer) of them. People seen and talked to were split off into other matches all the time (a party seen on the page went into
another match while the post-match lobby waited for one player; four countdowns in a row were cancelled by one backout each and the next match
was formed from whoever was ready that second).

Now every public searcher sits in exactly one LOBBY of at most MATCH_SIZE players. The page lists the lobby, the LOBBY chat is the lobby's,
and a match forms from one whole lobby. A member leaves its lobby only by:
  - backing out: leaving the searching page and not searching again within GRACE_S (a page reset or a reconnect is not a leave),
  - backing out of a countdown (then it is kept out of that lobby for BACKOUT_BLOCK_S, so one looping client cannot cancel it again and again),
  - going into a running match alone (only a lobby of one unit - a solo or one party - is offered join in progress),
  - joining a party whose leader is in another lobby (its own choice),
  - the ping gate at the start, when new pings put it over the limit of every host (rare: the pings are checked when it is placed).
Lobbies only ever grow by placement (a new searcher) or merge (two lobbies that fit into one, both whole). A cancelled countdown keeps the
lobby (minus the leaver). After a match the players come back into a lobby with the same id (the same chat room).

Pure bookkeeping (no daemon imports): the caller (daemon.MatchMaker) passes the rules as callbacks and holds its own lock around every call.
"""
import secrets
import time

GRACE_S = 10.0           # a member that left the searching page keeps its seat this long (0x02 page reset / reconnect, then searches again)
BACKOUT_BLOCK_S = 60.0   # a countdown backout is not placed into that lobby again for this long
REJOIN_S = 45.0          # a member that left (grace over) goes back into its old lobby when it searches again within this time and there is room
RELAUNCH_S = 8.0         # a lobby whose countdown was cancelled waits this long for new players before it starts again


class Lobby:
    __slots__ = ("lid", "members", "joined", "away", "created", "last_join", "mid", "src", "hold", "wait_until")

    def __init__(self, lid, now, src=None):
        self.lid = lid
        self.members = []          # tokens, join order
        self.joined = {}           # token -> time it joined this lobby
        self.away = {}             # token -> time it left the searching page (grace running)
        self.created = now
        self.last_join = 0.0         # the newest member's ARRIVAL (its queue time): the step-down clock of this lobby
        self.mid = None            # the match it formed (countdown / loading); None = open
        self.src = src             # the finished match it continues (post-match lobby) or None
        self.hold = False          # a single unit waiting for a join in progress (nobody is merged into it meanwhile)
        self.wait_until = 0.0      # no start before this time (after a cancelled countdown)


class LobbyBook:
    def __init__(self, size, log=None, now=time.time):
        self.size = size
        self.log = log or (lambda *a: None)
        self.now = now
        self.lobbies = {}          # lid -> Lobby
        self.of = {}               # token -> lid
        self.left = {}             # token -> (lid, time it left, blocked until)
        self.mid_lid = {}          # match id -> lid of the lobby that formed it (chat room + post-match lobby id)

    # ------------------------------------------------------------------ queries
    def lobby_of(self, token):
        return self.of.get(token)

    def get(self, lid):
        return self.lobbies.get(lid)

    def members(self, lid):
        lb = self.lobbies.get(lid)
        return list(lb.members) if lb else []

    def mates(self, token):
        """The token's lobby members (itself included), [] when it is in none."""
        return self.members(self.of.get(token))

    def open_lobbies(self):
        return [lb for lb in self.lobbies.values() if lb.mid is None]

    def launch_order(self):
        """Open lobbies, biggest first, then the oldest."""
        return sorted(self.open_lobbies(), key=lambda lb: (-len(lb.members), lb.created))

    def key_for_mid(self, mid):
        """The lobby id (chat room) of the players of match `mid`: the lobby that formed it, else one made from the match id."""
        return self.mid_lid.get(mid) or ("m" + str(mid))

    # ------------------------------------------------------------------ changes
    def _new(self, now, src=None, lid=None):
        if lid is None or lid in self.lobbies:
            lid = "L" + secrets.token_hex(4)
        lb = Lobby(lid, now, src)
        self.lobbies[lid] = lb
        return lb

    def _add(self, lb, token, now, at=None):
        old = self.of.get(token)
        if old == lb.lid:
            lb.away.pop(token, None)
            return
        if old is not None:
            self._take_out(token)
        lb.members.append(token)
        lb.joined[token] = now
        lb.last_join = max(lb.last_join, now if at is None else min(at, now))
        self.of[token] = lb.lid
        self.left.pop(token, None)

    def _take_out(self, token):
        lid = self.of.pop(token, None)
        lb = self.lobbies.get(lid)
        if lb is None:
            return None
        if token in lb.members:
            lb.members.remove(token)
        lb.joined.pop(token, None)
        lb.away.pop(token, None)
        if not lb.members and lb.mid is None:
            del self.lobbies[lid]
        return lid

    def src_lobby(self, mid):
        """The open lobby that continues the finished match `mid` (its returners), or None."""
        for lb in self.lobbies.values():
            if lb.src == mid and lb.mid is None:
                return lb
        return None

    def place(self, token, unit, fits, cap, now=None, src=None, solo=False, at=None, near=None):
        """Put a searcher into a lobby. unit = the tokens that always go together with it (its party, itself included; mates not searching
        yet count for the room). fits(members, newcomers) -> bool (mode / maps / ping); cap(lobby, newcomers) -> seats of that lobby for them
        (MATCH_SIZE minus the seats kept for post-match members and for party mates not searching yet). src = the finished match the token
        comes back from (its post-match lobby first; the first returner opens it). solo = keep it alone for now (a join in progress waits
        for it). near(members, newcomers) -> bool (optional): same region group (or unknown) - region first, never a block: a returner whose
        post-match lobby is mostly of another region takes an open lobby of its own region when one fits, else its post-match lobby as before;
        among the other lobbies the same-region ones come first. Returns (lid, why)."""
        now = self.now() if now is None else now
        lid = self.of.get(token)
        if lid in self.lobbies:
            self.lobbies[lid].away.pop(token, None)
            return lid, "back"
        unit = [token] + [m for m in unit if m != token]
        # a party mate is in a lobby already: the same lobby (room permitting), else the whole party moves to a new one
        for m in unit[1:]:
            ml = self.of.get(m)
            lb = self.lobbies.get(ml)
            if lb is None or lb.mid is not None:
                continue
            if len(lb.members) + 1 <= cap(lb, [token]):
                self._add(lb, token, now, at)
                return lb.lid, "party"
            nb = self._new(now)
            for x in [x for x in unit if self.of.get(x) == ml]:
                self._add(nb, x, now)
            self._add(nb, token, now, at)
            if not lb.members and lb.mid is None:
                self.lobbies.pop(lb.lid, None)
            self.log("LOBBY-BOOK: party of %s moved to a new lobby %s (no room in %s)" % (str(token)[:8], nb.lid, ml))
            return nb.lid, "party-moved"
        need = len(unit)
        lf = self.left.get(token)
        blocked = lf[0] if lf and now < lf[2] else None

        def ok(lb):
            return (lb is not None and lb.mid is None and lb.lid != blocked and not lb.hold and lb.members
                    and len(lb.members) + need <= cap(lb, unit) and fits(list(lb.members), unit))
        def near_ok(lb):
            return near is None or near(list(lb.members), unit)

        def best_open(same_only):
            best = None
            for lb in self.lobbies.values():
                if (src is not None and lb.src == src) or not ok(lb):
                    continue
                nr = near_ok(lb)
                if same_only and not nr:
                    continue
                k = (0 if nr else 1, -len(lb.members), lb.created)
                if best is None or k < best[0]:
                    best = (k, lb)
            return best[1] if best else None
        if src is not None:
            lb = self.src_lobby(src)
            if ok(lb):
                if not solo and lb.members and not near_ok(lb):
                    alt = best_open(True)
                    if alt is not None:
                        self._add(alt, token, now, at)
                        return alt.lid, "own region (post-match lobby %s is mostly another region)" % lb.lid
                self._add(lb, token, now, at)
                return lb.lid, "post-match lobby"
            if lb is None:
                nb = self._new(now, src=src, lid=self.key_for_mid(src))
                self._add(nb, token, now, at)
                return nb.lid, "post-match lobby (first back)"
        if lf and lf[0] != blocked and now - lf[1] <= REJOIN_S and ok(self.lobbies.get(lf[0])):
            lb = self.lobbies[lf[0]]
            self._add(lb, token, now, at)
            return lb.lid, "rejoin"
        if not solo:
            src = None                                   # (best_open: every open lobby is a choice here)
            b = best_open(False)
            if b is not None:
                self._add(b, token, now, at)
                return b.lid, "joined"
        nb = self._new(now)
        self._add(nb, token, now, at)
        nb.hold = bool(solo)
        return nb.lid, "new"

    def away(self, token, now=None):
        """The member left the searching page (or stopped polling): its seat is kept GRACE_S."""
        lb = self.lobbies.get(self.of.get(token))
        if lb is not None and token not in lb.away:
            lb.away[token] = self.now() if now is None else now

    def back(self, token):
        lb = self.lobbies.get(self.of.get(token))
        if lb is not None:
            lb.away.pop(token, None)

    def remove(self, token, why="", now=None, block_s=0.0):
        """Out of its lobby (backed out / into a running match / gone). block_s > 0: not placed into that lobby again for that long."""
        now = self.now() if now is None else now
        lid = self._take_out(token)
        if lid is not None:
            self.left[token] = (lid, now, now + block_s)
            if why:
                self.log("LOBBY-BOOK: %s left lobby %s: %s" % (str(token)[:8], lid, why))
        return lid

    def expire(self, now=None, grace=GRACE_S):
        """Members whose grace ran out: removed. Returns [(token, lid)]."""
        now = self.now() if now is None else now
        out = []
        for lb in list(self.lobbies.values()):
            for t, at in list(lb.away.items()):
                if now - at >= grace:
                    out.append((t, lb.lid))
        for t, lid in out:
            self.remove(t, "left the search (no search again within %.0f s)" % grace, now)
        for t in [t for t, e in self.left.items() if now - e[1] > max(REJOIN_S, BACKOUT_BLOCK_S) * 4]:
            del self.left[t]
        return out

    def merge_pass(self, fits, cap, now=None):
        """Merge whole lobbies that fit into one (open, not holding for a join in progress, nobody away): the smaller one moves into the bigger
        one. Returns [(into lid, from lid)]."""
        now = self.now() if now is None else now
        done = []
        changed = True
        while changed:
            changed = False
            ls = [lb for lb in self.launch_order() if not lb.hold and not lb.away and lb.members]
            for i in range(len(ls) - 1, 0, -1):
                for j in range(i):
                    a, b = ls[j], ls[i]
                    if a.src is not None and b.src is not None:
                        continue
                    if b.src is not None:          # the post-match lobby keeps its id (chat room, returners): the other one moves in
                        a, b = b, a
                    if len(a.members) + len(b.members) > cap(a, list(b.members)):
                        continue
                    if any(self.left.get(t, (None, 0, 0))[0] == a.lid and now < self.left[t][2] for t in b.members):
                        continue                    # a countdown backout of `a` is in `b`
                    if not fits(list(a.members), list(b.members)):
                        continue
                    lj = max(a.last_join, b.last_join)
                    for t in list(b.members):
                        self._add(a, t, now, b.last_join)
                    a.last_join = lj
                    self.lobbies.pop(b.lid, None)
                    done.append((a.lid, b.lid))
                    changed = True
                    break
                if changed:
                    break
        return done

    def launched(self, lid, mid):
        lb = self.lobbies.get(lid)
        if lb is not None:
            lb.mid = mid
            lb.hold = False
            self.mid_lid[mid] = lid
            while len(self.mid_lid) > 512:
                self.mid_lid.pop(next(iter(self.mid_lid)))

    def lobby_of_mid(self, mid):
        for lb in self.lobbies.values():
            if lb.mid == mid:
                return lb
        return None

    def cancelled(self, mid, leaver, now=None):
        """The countdown of `mid` was called off by `leaver`: the lobby is open again with everybody else; the leaver is out of it (blocked
        BACKOUT_BLOCK_S). Returns the lid or None."""
        now = self.now() if now is None else now
        lb = self.lobby_of_mid(mid)
        if lb is None:
            return None
        lb.mid = None
        lb.wait_until = now + RELAUNCH_S
        self.mid_lid.pop(mid, None)
        if leaver in lb.members:
            self.remove(leaver, "backed out of the countdown of %s" % mid, now, BACKOUT_BLOCK_S)
        if not lb.members:
            self.lobbies.pop(lb.lid, None)
        return lb.lid

    def sync_matches(self, alive, started, queued, now=None):
        """Lobbies that formed a match: dropped once it started (its players are in the match now); reopened when the match is gone before its
        start (load timeout / abort) with the members that still search. alive(mid) / started(mid) -> bool; queued = tokens searching."""
        now = self.now() if now is None else now
        qs = set(queued)
        for lb in list(self.lobbies.values()):
            if lb.mid is None:
                continue
            if started(lb.mid):
                for t in lb.members:
                    if self.of.get(t) == lb.lid:
                        del self.of[t]
                del self.lobbies[lb.lid]
            elif not alive(lb.mid):
                self.log("LOBBY-BOOK: match %s of lobby %s is gone before its start: the lobby is open again" % (lb.mid, lb.lid))
                lb.mid = None
                lb.wait_until = now + RELAUNCH_S
                for t in [t for t in lb.members if t not in qs]:
                    lb.away.setdefault(t, now)

    def check(self):
        """Invariants (tests): every member is in `of` once, lobbies never over the size, no empty open lobby."""
        seen = {}
        for lid, lb in self.lobbies.items():
            assert len(lb.members) <= self.size, (lid, len(lb.members))
            assert lb.mid is not None or lb.members, lid
            for t in lb.members:
                assert t not in seen, (t, lid, seen.get(t))
                seen[t] = lid
                assert self.of.get(t) == lid, (t, lid, self.of.get(t))
        for t, lid in self.of.items():
            assert seen.get(t) == lid, (t, lid)
        return True
